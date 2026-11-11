#include "Hyacinth.h"

#include "Diag.h"
#include "GLState.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;

namespace hyacinth
{
namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame may advance by. A stalled host must
/// not jerk the focus or the orbs.
constexpr double kMaxFrameDelta = 0.25;

/// How much of a subject's new look the template learns a frame, while the
/// match is a good one: a twentieth, so a passing occluder is not learned.
constexpr float kTemplateBlend = 0.05f;

/// A match below this is a lost subject: the track holds where it was.
constexpr float kLostBelow = 0.5f;

/// The guided filter's regulariser. A window whose luma varies by less than
/// about 3% (its standard deviation) is flat, and its depth is the window's
/// mean.
constexpr float kGuideEpsilon = 1e-3f;

/// A highlight block with less light than this (linear light x pixels) is
/// not drawn: spread over the smallest disc that is drawn, a 3-pixel
/// radius, it would add less than a thousandth.
constexpr float kMinEnergy = 0.02f;

/// The light a Light X/Y light has, linear light x pixels, as a fraction of
/// the picture's area: about a 60-pixel lamp at 1080, three times white.
constexpr double kLightEnergy = 2e-3;
/// Lights in the picture with less than this are not flared.
constexpr double kMinLight = 1e-4;
/// Its colour: a warm white.
constexpr float kLightColour[ 3 ] = { 1.0f, 0.93f, 0.82f };

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}

float f( double v )
{
	return static_cast< float >( v );
}

int ceilDiv( int a, int b )
{
	return ( a + b - 1 ) / b;
}

/// sRGB to linear, for the threshold. The GLSL's toLinear, in double.
double toLinear( double code )
{
	return code < 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}

/// Unit texture slots. The first four are bound for every pass; a pass's
/// own inputs start at kUnitPass.
enum Unit : int
{
	kUnitPicture    = 0,
	kUnitTrackState = 1,
	kUnitGuide      = 2,
	kUnitFocusState = 3,
	kUnitPass       = 4,
	kUnitCount      = 9
};
} // namespace

//---------------------------------------------------------------------------
// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree.
static_assert( PT_COUNT - PT_ABOUT_FIRST == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h" );

//---------------------------------------------------------------------------
HyacinthPlugin::HyacinthPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The focus pull and the orbs run in seconds.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged parameter is
	// a plain 0..1 float: SetParamInfo clamps a STANDARD default into 0..1
	// before a range could be attached (SDK b1afaf9).
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]          = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::Boolean: SetParamInfo( id, info.name, FF_TYPE_BOOLEAN, info.defaultValue > 0.5f ); break;
		case Kind::Event: SetParamInfo( id, info.name, FF_TYPE_EVENT, false ); break;
		case Kind::XPos: SetParamInfo( id, info.name, FF_TYPE_XPOS, info.defaultValue ); break;
		case Kind::YPos: SetParamInfo( id, info.name, FF_TYPE_YPOS, info.defaultValue ); break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	spiral = Spiral();
}

//---------------------------------------------------------------------------
FFResult HyacinthPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	for( int i = 0; i < static_cast< int >( shaders::Pass::Count ); ++i )
	{
		const auto pass                 = static_cast< shaders::Pass >( i );
		const shaders::Program program = shaders::Assemble( pass );
		locations[ static_cast< size_t >( i ) ].clear();
		if( programs[ static_cast< size_t >( i ) ].Compile( program.vertex, program.fragment ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the plugin simply
		//does nothing in Resolume. These lines are the only record of which
		//pass it was.
		diag::error( std::string( "the " ) + shaders::NameOf( pass ) + " shader failed to compile - the plugin will do nothing" );
		FFGLLog::LogToHost( "Hyacinth: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	glGenVertexArrays( 1, &emptyVao );

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void HyacinthPlugin::UpdateClock()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	const double raw = hostTime;
	//Resolume has been seen sending both seconds and milliseconds through
	//SetTime, and nothing in the call says which. Vote on it against the wall
	//clock for a few frames, then stop asking. (Rosette's code, by way of
	//holofoil.)
	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				//The clock is about to jump from wall time to the host's own,
				//which can be hours in: no dt across the jump.
				settledJump = true;
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	now = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( raw ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );
}

//---------------------------------------------------------------------------
// Uniforms.
//---------------------------------------------------------------------------
GLint HyacinthPlugin::at( const char* name )
{
	auto& cache = locations[ static_cast< size_t >( current ) ];
	const auto found = cache.find( name );
	if( found != cache.end() )
		return found->second;
	const GLint location = glGetUniformLocation( programs[ static_cast< size_t >( current ) ].GetGLID(), name );
	cache.emplace( name, location );
	return location;
}
void HyacinthPlugin::set( const char* name, int v )
{
	glUniform1i( at( name ), v );
}
void HyacinthPlugin::set( const char* name, float v )
{
	glUniform1f( at( name ), v );
}
void HyacinthPlugin::set( const char* name, float x, float y )
{
	glUniform2f( at( name ), x, y );
}
void HyacinthPlugin::set( const char* name, float x, float y, float z )
{
	glUniform3f( at( name ), x, y, z );
}
void HyacinthPlugin::set( const char* name, float x, float y, float z, float w )
{
	glUniform4f( at( name ), x, y, z, w );
}
void HyacinthPlugin::seti( const char* name, int x, int y )
{
	glUniform2i( at( name ), x, y );
}

void HyacinthPlugin::use( shaders::Pass pass )
{
	current = static_cast< int >( pass );
	glUseProgram( programs[ static_cast< size_t >( current ) ].GetGLID() );
}

void HyacinthPlugin::into( PassBuffer& target )
{
	glBindFramebuffer( GL_FRAMEBUFFER, target.GetGLID() );
	glViewport( 0, 0, static_cast< GLsizei >( target.GetWidth() ), static_cast< GLsizei >( target.GetHeight() ) );
}

void HyacinthPlugin::drawInstanced( GLenum mode, int vertices, int instances )
{
	if( instances <= 0 )
		return;
	glBindVertexArray( emptyVao );
	glDrawArraysInstanced( mode, 0, vertices, instances );
	glBindVertexArray( 0 );
}

//---------------------------------------------------------------------------
bool HyacinthPlugin::ensureBuffers( int width, int height )
{
	//All of them, before anything is bound for the frame: every
	//ffglex::Scoped* binding clears to 0 on exit rather than restoring, and
	//PassBuffer::Ensure binds under one. Each is a no-op once allocated.
	const int qw = ceilDiv( width, 4 ), qh = ceilDiv( height, 4 );
	const int hw = ceilDiv( width, 2 ), hh = ceilDiv( height, 2 );
	using S        = PassBuffer::Sampling;
	const GLint pf = GL_RGBA16F;
	const GLint df = GL_RGBA32F;
	bool ok        = true;
	ok             = ok && analysis.Ensure( qw, qh, pf, S::Mipmapped );
	ok             = ok && darkA.Ensure( qw, qh, pf, S::Nearest );
	ok             = ok && darkB.Ensure( qw, qh, pf, S::Linear );
	ok             = ok && reduceA.Ensure( ceilDiv( qw, 16 ), ceilDiv( qh, 16 ), df, S::Nearest );
	ok             = ok && airlight.Ensure( 1, 1, df, S::Nearest );
	ok             = ok && histogram.Ensure( shaders::kHistogramBins + 1, 2, df, S::Nearest );
	ok             = ok && raw.Ensure( qw, qh, pf, S::Linear );
	ok             = ok && guideScratch.Ensure( qw, qh, df, S::Nearest );
	for( PassBuffer& m : guideMeans )
		ok = ok && m.Ensure( qw, qh, df, S::Nearest );
	ok             = ok && guideCoef.Ensure( qw, qh, df, S::Nearest );
	ok             = ok && guide.Ensure( qw, qh, df, S::Linear );
	for( PassBuffer& t : templates )
		ok = ok && t.Ensure( shaders::kTemplateSide, shaders::kTemplateSide, df, S::Nearest );
	ok = ok && coarseScores.Ensure( shaders::kCoarseGrid, shaders::kCoarseGrid, df, S::Nearest );
	ok = ok && coarseBest.Ensure( 1, 1, df, S::Nearest );
	ok = ok && fineScores.Ensure( shaders::kFineGrid, shaders::kFineGrid, df, S::Nearest );
	for( PassBuffer& s : trackState )
		ok = ok && s.Ensure( 2, 1, df, S::Nearest );
	for( PassBuffer& s : focusState )
		ok = ok && s.Ensure( 1, 1, df, S::Nearest );
	ok = ok && prefilter.Ensure( hw, hh, pf, S::Mipmapped );
	ok = ok && highlights.Ensure( qw, qh, pf, S::Mipmapped );
	ok = ok && tiles.Ensure( ceilDiv( hw, shaders::kTileSize ), ceilDiv( hh, shaders::kTileSize ), pf, S::Nearest );
	ok = ok && dilated.Ensure( ceilDiv( hw, shaders::kTileSize ), ceilDiv( hh, shaders::kTileSize ), pf, S::Nearest );
	ok = ok && gather.Ensure( hw, hh, pf, S::Linear );
	ok = ok && bokeh.Ensure( hw, hh, pf, S::Linear );
	ok = ok && lights.Ensure( shaders::kLightsX, 2 * shaders::kLightsY, df, S::Nearest );
	return ok;
}

//---------------------------------------------------------------------------
FFResult HyacinthPlugin::ProcessOpenGL( ProcessOpenGLStruct* pgl )
{
	if( pgl == nullptr || pgl->numInputTextures < 1 || pgl->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& source = *( pgl->inputTextures[ 0 ] );
	if( source.Width == 0 || source.Height == 0 )
		return FF_FAIL;
	const int width  = static_cast< int >( source.Width );
	const int height = static_cast< int >( source.Height );

	//The host's viewport and blend state, put back however this returns.
	ScopedGLState restore;
	const GLint* hostViewport = restore.saved.viewport;
	glDisable( GL_BLEND );
	glDisable( GL_PROGRAM_POINT_SIZE );

	if( !ensureBuffers( width, height ) )
	{
		diag::error( "could not allocate the work buffers" );
		return FF_FAIL;
	}

	//---------------------------------------------------------------------
	// Time.
	//---------------------------------------------------------------------
	UpdateClock();
	if( settledJump )
	{
		lastNow     = -1.0;
		settledJump = false;
	}
	const double dt = lastNow >= 0.0 ? std::clamp( now - lastNow, 0.0, kMaxFrameDelta ) : 0.0;
	lastNow         = now;

	//---------------------------------------------------------------------
	// The controls, in the units the passes want.
	//---------------------------------------------------------------------
	auto option = [ this ]( unsigned int id, int count ) { return OptionIndex( params[ id ], count ); };
	const auto mode      = static_cast< Mode >( option( PT_MODE, static_cast< int >( Mode::Count ) ) );
	const auto depthFrom = static_cast< DepthFrom >( option( PT_DEPTH_FROM, static_cast< int >( DepthFrom::Count ) ) );
	const auto focus     = static_cast< Focus >( option( PT_FOCUS, static_cast< int >( Focus::Count ) ) );
	const auto shape     = static_cast< Shape >( option( PT_SHAPE, static_cast< int >( Shape::Count ) ) );
	const auto flareFrom = static_cast< FlareFrom >( option( PT_FLARE_FROM, static_cast< int >( FlareFrom::Count ) ) );
	const auto show      = static_cast< Show >( option( PT_SHOW, static_cast< int >( Show::Count ) ) );

	const double W         = width;
	const double H         = height;
	const double aspect    = W / H;
	const bool dof         = mode == Mode::DepthOfField;
	const double blurPx    = BlurFromParam( params[ PT_BLUR ] ) * H;
	const double flare     = clamp01( params[ PT_FLARE ] );
	const bool flareOn     = flare > 0.0;
	const bool tracking    = focus == Focus::Track;
	const bool trackRuns   = tracking || ( flareOn && flareFrom == FlareFrom::Tracked );
	const bool estimated   = depthFrom == DepthFrom::Luma || depthFrom == DepthFrom::Haze || depthFrom == DepthFrom::Subject;
	const double trackSize = TrackSizeFromParam( params[ PT_TRACK_SIZE ] );
	const float focusX     = f( clamp01( params[ PT_FOCUS_X ] ) );
	const float focusY     = f( clamp01( params[ PT_FOCUS_Y ] ) );

	//---------------------------------------------------------------------
	// Where the picture and its depth are in the input texture. The host may
	// pad a texture past the picture (HardwareWidth); a Side by Side or Over
	// Under clip has two pictures in one.
	//---------------------------------------------------------------------
	const double sx = source.HardwareWidth > 0 ? W / source.HardwareWidth : 1.0;
	const double sy = source.HardwareHeight > 0 ? H / source.HardwareHeight : 1.0;
	float pictureRect[ 4 ] = { 0.0f, 0.0f, f( sx ), f( sy ) };
	float depthRect[ 4 ]   = { 0.0f, 0.0f, f( sx ), f( sy ) };
	const bool packed      = depthFrom == DepthFrom::SideBySide || depthFrom == DepthFrom::OverUnder;
	if( depthFrom == DepthFrom::SideBySide )
	{
		pictureRect[ 2 ] = f( 0.5 * sx );
		depthRect[ 0 ]   = f( 0.5 * sx );
		depthRect[ 2 ]   = f( 0.5 * sx );
	}
	else if( depthFrom == DepthFrom::OverUnder )
	{
		//GL's v runs up: the top half is the upper half of v.
		pictureRect[ 1 ] = f( 0.5 * sy );
		pictureRect[ 3 ] = f( 0.5 * sy );
		depthRect[ 3 ]   = f( 0.5 * sy );
	}
	const bool exact = !packed && hostViewport[ 2 ] == width && hostViewport[ 3 ] == height;

	//---------------------------------------------------------------------
	// The plane: nearness affine across the picture, as the ground's is in
	// front of a pinhole camera. 1 at the nearest corner, 0 on the horizon.
	//---------------------------------------------------------------------
	const double planeAngle = PlaneAngleFromParam( params[ PT_PLANE_ANGLE ] );
	const double ex = std::cos( planeAngle ), ey = std::sin( planeAngle );
	const double sMax     = 0.5 * ( aspect * std::fabs( ex ) + std::fabs( ey ) );
	const double sHorizon = -sMax + HorizonFromParam( params[ PT_HORIZON ] ) * 2.0 * sMax;
	const double planeDen = std::max( sMax - sHorizon, 0.02 * sMax );

	//---------------------------------------------------------------------
	// The aperture.
	//---------------------------------------------------------------------
	IrisDesc desc;
	desc.shape        = shape;
	desc.blades       = BladesFromParam( params[ PT_BLADES ] );
	desc.curve        = clamp01( params[ PT_BLADE_CURVE ] );
	desc.bubble       = BubbleFromParam( params[ PT_BUBBLE ] );
	desc.onion        = clamp01( params[ PT_ONION ] );
	if( !areaValid || desc != areaDesc )
	{
		area      = AreaOf( desc );
		areaDesc  = desc;
		areaValid = true;
		++areaBuilds;
	}
	//A five-pointed star's edges lie on five lines, which are a pentagon's
	//edges' directions: its starburst is a five-bladed iris's.
	const int spikeBlades = shape == Shape::Star ? 5 : desc.blades;
	const auto normalsD   = EdgeNormals( spikeBlades );
	float normals[ 2 * kMaxBlades ];
	for( int i = 0; i < 2 * kMaxBlades; ++i )
		normals[ i ] = f( normalsD[ static_cast< size_t >( i ) ] );
	//Straight edges diffract into spikes; curved ones spread the light round.
	const double straight = shape == Shape::Iris ? 1.0 - desc.curve : shape == Shape::Star ? 1.0 : 0.0;
	const double turn     = IrisRotateFromParam( params[ PT_IRIS_ROTATE ] );
	const double squeeze  = SqueezeFromParam( params[ PT_SQUEEZE ] );

	const int qw = static_cast< int >( analysis.GetWidth() ), qh = static_cast< int >( analysis.GetHeight() );
	const int hw = static_cast< int >( prefilter.GetWidth() ), hh = static_cast< int >( prefilter.GetHeight() );
	const double quarterPx = W / qw;  ///< an analysis texel, pixels
	const double halfPx    = W / hw;  ///< a half-resolution texel, pixels
	const double toTexels  = hw / W;

	const double threshold = clamp01( params[ PT_THRESHOLD ] );

	//---------------------------------------------------------------------
	// What every pass is handed.
	//---------------------------------------------------------------------
	auto shared = [ & ]() {
		set( "Picture", static_cast< int >( kUnitPicture ) );
		set( "PictureRect", pictureRect[ 0 ], pictureRect[ 1 ], pictureRect[ 2 ], pictureRect[ 3 ] );
		set( "DepthRect", depthRect[ 0 ], depthRect[ 1 ], depthRect[ 2 ], depthRect[ 3 ] );
		set( "Frame", f( W ), f( H ) );
		set( "Aspect", f( aspect ) );
		set( "HalfDiagonal", f( 0.5 * std::sqrt( aspect * aspect + 1.0 ) ) );
		set( "Hooks", hooks );
		set( "TrackState", static_cast< int >( kUnitTrackState ) );
		set( "TrackActive", tracking ? 1 : 0 );
		set( "FocusPoint", focusX, focusY );

		set( "DepthSource", static_cast< int >( depthFrom ) );
		set( "PlaneEq", f( ex / planeDen ), f( ey / planeDen ), f( -sHorizon / planeDen ) );
		set( "RadialInv", f( 1.0 / RadiusFromParam( params[ PT_RADIUS ] ) ) );
		set( "Guide", static_cast< int >( kUnitGuide ) );
		set( "DepthInvert", params[ PT_DEPTH_INVERT ] > 0.5f ? 1 : 0 );
		set( "FocusState", static_cast< int >( kUnitFocusState ) );
		set( "BlurPx", f( blurPx ) );
		set( "Gain", f( HighlightGainFromParam( params[ PT_HIGHLIGHTS ] ) ) );
		set( "ThresholdCode", f( threshold ) );
		set( "ThresholdLight", f( toLinear( threshold ) ) );
	};
	auto iris = [ & ]() {
		set( "ShapeIndex", static_cast< int >( shape ) );
		set( "Blades", spikeBlades );
		glUniform2fv( at( "EdgeNormal" ), kMaxBlades, normals );
		set( "ApothemValue", f( Apothem( desc.blades ) ) );
		set( "BladeCurve", f( desc.curve ) );
		set( "IrisTurn", f( std::cos( turn ) ), f( std::sin( turn ) ) );
		set( "CatEye", f( CatEyeFromParam( params[ PT_CAT_EYE ] ) ) );
		set( "Swirl", f( SwirlFromParam( params[ PT_SWIRL ] ) ) );
		set( "Squeeze", f( squeeze ) );
		set( "Bubble", f( desc.bubble ) );
		set( "Onion", f( desc.onion ) );
		set( "AaPx", f( halfPx ) );
	};
	//The units every pass may read: the picture, the track, the depth's
	//guide and the focus. Bound again for each pass, since a pass may have
	//just written the one that was bound.
	auto bindShared = [ & ]() {
		bindUnit( kUnitPicture, source.Handle );
		bindUnit( kUnitTrackState, trackState[ stateCur ].TextureID() );
		bindUnit( kUnitGuide, guide.TextureID() );
		bindUnit( kUnitFocusState, focusState[ focusCur ].TextureID() );
	};
	auto begin = [ & ]( shaders::Pass pass, PassBuffer& target ) {
		into( target );
		use( pass );
		bindShared();
		shared();
	};

	//---------------------------------------------------------------------
	// 1. The analysis picture.
	//---------------------------------------------------------------------
	begin( shaders::Pass::Analysis, analysis );
	set( "CellUv", f( 1.0 / qw ), f( 1.0 / qh ) );
	quad.Draw();
	analysis.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. The tracker.
	//---------------------------------------------------------------------
	const double stepHeights = trackSize / shaders::kTemplateSide;
	const float templateLod  = f( std::max( 0.0, std::log2( stepHeights * H / quarterPx ) ) );
	auto trackerCommon       = [ & ]() {
		set( "Analysis", static_cast< int >( kUnitPass ) );
		set( "AnalysisLod", templateLod );
		set( "TemplateStep", f( stepHeights / aspect ), f( stepHeights ) );
		set( "GrabPos", focusX, focusY );
	};
	trackerRan = trackRuns;
	if( trackRuns )
	{
		const bool grab  = params[ PT_GRAB ] > 0.5f;
		const bool moved = std::fabs( focusX - lastFocusX ) > 1e-6f || std::fabs( focusY - lastFocusY ) > 1e-6f;
		const bool regrab = firstFrame || !wasTracking || ( grab && !lastGrab ) || moved;
		if( regrab )
		{
			++regrabs;
			PassBuffer& next = templates[ 1 - templateCur ];
			begin( shaders::Pass::Template, next );
			trackerCommon();
			bindUnit( kUnitPass, analysis.TextureID() );
			bindUnit( kUnitPass + 1, templates[ templateCur ].TextureID() );
			set( "OldTemplate", static_cast< int >( kUnitPass + 1 ) );
			set( "TemplateBlend", 1.0f );
			set( "Regrab", 1 );
			quad.Draw();
			templateCur = 1 - templateCur;

			begin( shaders::Pass::Pick, trackState[ 1 - stateCur ] );
			trackerCommon();
			bindUnit( kUnitPass, fineScores.TextureID() );
			set( "Scores", static_cast< int >( kUnitPass ) );
			set( "Grid", shaders::kFineGrid );
			set( "Final", 1 );
			set( "Regrab", 1 );
			set( "CandidateStep", f( shaders::kFineStep ) );
			set( "LostBelow", kLostBelow );
			quad.Draw();
			stateCur = 1 - stateCur;
		}
		else
		{
			auto score = [ & ]( PassBuffer& target, int fromState, int grid, double step ) {
				begin( shaders::Pass::Score, target );
				trackerCommon();
				bindUnit( kUnitPass, analysis.TextureID() );
				bindUnit( kUnitPass + 1, templates[ templateCur ].TextureID() );
				bindUnit( kUnitPass + 2, coarseBest.TextureID() );
				set( "Template", static_cast< int >( kUnitPass + 1 ) );
				set( "Centre", static_cast< int >( kUnitPass + 2 ) );
				set( "FromState", fromState );
				set( "Grid", grid );
				set( "CandidateStep", f( step ) );
				quad.Draw();
			};
			auto pick = [ & ]( PassBuffer& scores, PassBuffer& target, int final, int grid, double step ) {
				begin( shaders::Pass::Pick, target );
				trackerCommon();
				bindUnit( kUnitPass, scores.TextureID() );
				set( "Scores", static_cast< int >( kUnitPass ) );
				set( "Grid", grid );
				set( "Final", final );
				set( "Regrab", 0 );
				set( "CandidateStep", f( step ) );
				set( "LostBelow", kLostBelow );
				quad.Draw();
			};
			score( coarseScores, 1, shaders::kCoarseGrid, shaders::kCoarseStep );
			pick( coarseScores, coarseBest, 0, shaders::kCoarseGrid, shaders::kCoarseStep );
			score( fineScores, 0, shaders::kFineGrid, shaders::kFineStep );
			pick( fineScores, trackState[ 1 - stateCur ], 1, shaders::kFineGrid, shaders::kFineStep );
			stateCur = 1 - stateCur;

			//Learn the subject's look where it now is.
			begin( shaders::Pass::Template, templates[ 1 - templateCur ] );
			trackerCommon();
			bindUnit( kUnitPass, analysis.TextureID() );
			bindUnit( kUnitPass + 1, templates[ templateCur ].TextureID() );
			set( "OldTemplate", static_cast< int >( kUnitPass + 1 ) );
			set( "TemplateBlend", kTemplateBlend );
			set( "Regrab", 0 );
			quad.Draw();
			templateCur = 1 - templateCur;
		}
	}
	wasTracking = trackRuns;
	lastGrab    = params[ PT_GRAB ] > 0.5f;
	lastFocusX  = focusX;
	lastFocusY  = focusY;

	//---------------------------------------------------------------------
	// 3. The estimated depths, and the guided filter.
	//---------------------------------------------------------------------
	if( estimated )
	{
		if( depthFrom == DepthFrom::Haze )
		{
			//He's patch is 15 pixels at 600; about 1.2% of the height a side.
			const int radius = std::max( 1, static_cast< int >( std::lround( 0.012 * qh ) ) );
			begin( shaders::Pass::Dark, darkA );
			bindUnit( kUnitPass, analysis.TextureID() );
			set( "Source", static_cast< int >( kUnitPass ) );
			seti( "Dir", 1, 0 );
			set( "Radius", radius );
			set( "FromColour", 1 );
			quad.Draw();
			begin( shaders::Pass::Dark, darkB );
			bindUnit( kUnitPass, darkA.TextureID() );
			set( "Source", static_cast< int >( kUnitPass ) );
			seti( "Dir", 0, 1 );
			set( "Radius", radius );
			set( "FromColour", 0 );
			quad.Draw();

			//The air light: the haziest the picture gets.
			begin( shaders::Pass::Reduce, reduceA );
			bindUnit( kUnitPass, darkB.TextureID() );
			set( "Source", static_cast< int >( kUnitPass ) );
			set( "Block", 16 );
			quad.Draw();
			begin( shaders::Pass::Reduce, airlight );
			bindUnit( kUnitPass, reduceA.TextureID() );
			set( "Source", static_cast< int >( kUnitPass ) );
			set( "Block", static_cast< int >( std::max( reduceA.GetWidth(), reduceA.GetHeight() ) ) );
			quad.Draw();
		}
		if( depthFrom == DepthFrom::Subject )
		{
			into( histogram );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			use( shaders::Pass::Histogram );
			bindShared();
			shared();
			bindUnit( kUnitPass, analysis.TextureID() );
			set( "Analysis", static_cast< int >( kUnitPass ) );
			//Each sample stands for a patch 2.5 track sides / 32 across.
			const double spacingPx = 2.5 * trackSize * H / shaders::kHistogramSide;
			set( "AnalysisLod", f( std::max( 0.0, std::log2( spacingPx / quarterPx ) ) ) );
			set( "TrackHeights", f( trackSize ) );
			setAdditiveBlend();
			glPointSize( 1.0f );
			drawInstanced( GL_POINTS, 9, shaders::kHistogramSide * shaders::kHistogramSide );
			glDisable( GL_BLEND );
		}

		begin( shaders::Pass::RawDepth, raw );
		bindUnit( kUnitPass, analysis.TextureID() );
		bindUnit( kUnitPass + 1, darkB.TextureID() );
		bindUnit( kUnitPass + 2, airlight.TextureID() );
		bindUnit( kUnitPass + 3, histogram.TextureID() );
		set( "Analysis", static_cast< int >( kUnitPass ) );
		set( "Dark", static_cast< int >( kUnitPass + 1 ) );
		set( "Airlight", static_cast< int >( kUnitPass + 2 ) );
		set( "Histogram", static_cast< int >( kUnitPass + 3 ) );
		set( "TrackHeights", f( trackSize ) );
		quad.Draw();

		const int radius = std::clamp( static_cast< int >( std::lround( DepthEdgesFromParam( params[ PT_DEPTH_EDGES ] ) * H / quarterPx ) ), 1, 32 );
		auto box = [ & ]( PassBuffer& from, PassBuffer& to, int dx, int dy ) {
			begin( shaders::Pass::Box, to );
			bindUnit( kUnitPass, from.TextureID() );
			set( "Source", static_cast< int >( kUnitPass ) );
			seti( "Dir", dx, dy );
			set( "Radius", radius );
			quad.Draw();
		};
		for( int part = 0; part < 4; ++part )
		{
			begin( shaders::Pass::GuideRow, guideScratch );
			bindUnit( kUnitPass, analysis.TextureID() );
			bindUnit( kUnitPass + 1, raw.TextureID() );
			set( "Analysis", static_cast< int >( kUnitPass ) );
			set( "Raw", static_cast< int >( kUnitPass + 1 ) );
			set( "Part", part );
			set( "Radius", radius );
			quad.Draw();
			box( guideScratch, guideMeans[ part ], 0, 1 );
		}

		begin( shaders::Pass::GuideCoef, guideCoef );
		static const char* const means[] = { "Means0", "Means1", "Means2", "Means3" };
		for( int part = 0; part < 4; ++part )
		{
			bindUnit( kUnitPass + part, guideMeans[ part ].TextureID() );
			set( means[ part ], static_cast< int >( kUnitPass + part ) );
		}
		bindUnit( kUnitPass + 4, raw.TextureID() );
		set( "Raw", static_cast< int >( kUnitPass + 4 ) );
		set( "Epsilon", kGuideEpsilon );
		quad.Draw();

		box( guideCoef, guideScratch, 1, 0 );
		//Not the bound guide: it is about to be written.
		into( guide );
		use( shaders::Pass::Box );
		bindUnit( kUnitPicture, source.Handle );
		bindUnit( kUnitGuide, 0 );
		bindUnit( kUnitPass, guideScratch.TextureID() );
		shared();
		set( "Source", static_cast< int >( kUnitPass ) );
		seti( "Dir", 0, 1 );
		set( "Radius", radius );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 4. The focus.
	//---------------------------------------------------------------------
	{
		const double tau = FocusTauFromParam( params[ PT_FOCUS_SPEED ] );
		const double alpha = tau > 0.0 ? 1.0 - std::exp( -dt / tau ) : 1.0;
		begin( shaders::Pass::Focus, focusState[ 1 - focusCur ] );
		bindUnit( kUnitPass, focusState[ focusCur ].TextureID() );
		set( "PrevFocus", static_cast< int >( kUnitPass ) );
		set( "FocusMode", focus == Focus::Manual ? 0 : 1 );
		set( "FocusManual", f( clamp01( params[ PT_FOCUS_DEPTH ] ) ) );
		set( "FocusAlpha", f( alpha ) );
		set( "FocusReset", firstFrame ? 1 : 0 );
		set( "FocusReach", f( 0.1 * trackSize ) );
		quad.Draw();
		focusCur = 1 - focusCur;
	}

	//---------------------------------------------------------------------
	// 5. The prefilter, and 6. the highlights.
	//---------------------------------------------------------------------
	if( dof )
	{
		begin( shaders::Pass::Prefilter, prefilter );
		quad.Draw();
		prefilter.GenerateMipmaps();
	}
	begin( shaders::Pass::Highlight, highlights );
	set( "CellUv", f( 1.0 / qw ), f( 1.0 / qh ) );
	quad.Draw();
	highlights.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 7. Tiles and 8. the gather.
	//---------------------------------------------------------------------
	if( dof )
	{
		begin( shaders::Pass::Tile, tiles );
		bindUnit( kUnitPass, prefilter.TextureID() );
		set( "Source", static_cast< int >( kUnitPass ) );
		set( "ToTexels", f( toTexels ) );
		quad.Draw();

		begin( shaders::Pass::Dilate, dilated );
		bindUnit( kUnitPass, tiles.TextureID() );
		set( "Source", static_cast< int >( kUnitPass ) );
		set( "Reach", std::clamp( static_cast< int >( std::ceil( blurPx * toTexels / shaders::kTileSize ) ), 1, 16 ) );
		quad.Draw();

		begin( shaders::Pass::Gather, gather );
		bindUnit( kUnitPass, prefilter.TextureID() );
		bindUnit( kUnitPass + 1, dilated.TextureID() );
		set( "Prefilter", static_cast< int >( kUnitPass ) );
		set( "Dilated", static_cast< int >( kUnitPass + 1 ) );
		glUniform2fv( at( "Spiral" ), kSpiralCount, spiral.data() );
		set( "ToTexels", f( toTexels ) );
		set( "MaxLod", prefilter.MaxMipLevel() );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 9. The sprites: the highlights' bokeh, then the orbs.
	//---------------------------------------------------------------------
	into( bokeh );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	const double gain     = HighlightGainFromParam( params[ PT_HIGHLIGHTS ] );
	const double orbs     = clamp01( params[ PT_ORBS ] );
	const int orbCount    = OrbCountFromParam( params[ PT_ORB_COUNT ] );
	const double fringe   = FringeFromParam( params[ PT_FRINGE ] );
	const bool drawBokeh  = threshold < 1.0 && ( dof ? blurPx > 0.0 : blurPx >= 1.5 );
	driftPhase   = std::fmod( driftPhase + dt * 0.5 * OrbDriftFromParam( params[ PT_ORB_DRIFT ] ), 1.0 );
	twinklePhase = std::fmod( twinklePhase + dt * 1.5, 4096.0 );

	//A wide blur draws the highlights from a coarser level: fewer, larger
	//cells, each still a point to a disc that size, and the same light.
	const int maxLevel = static_cast< int >( highlights.MaxMipLevel() );
	spriteLevel        = std::clamp( static_cast< int >( std::floor( std::log2( std::max( blurPx, 1.0 ) / 24.0 ) ) ), 0, std::min( 3, maxLevel ) );
	const int cellsX   = std::max( 1, qw >> spriteLevel );
	const int cellsY   = std::max( 1, qh >> spriteLevel );

	auto spriteCommon = [ & ]( int kind ) {
		into( bokeh );
		use( shaders::Pass::Sprite );
		bindShared();
		shared();
		iris();
		bindUnit( kUnitPass, highlights.TextureID() );
		bindUnit( kUnitPass + 1, prefilter.TextureID() );
		bindUnit( kUnitPass + 2, analysis.TextureID() );
		set( "Highlights", static_cast< int >( kUnitPass ) );
		set( "Prefilter", static_cast< int >( kUnitPass + 1 ) );
		set( "Analysis", static_cast< int >( kUnitPass + 2 ) );
		set( "SpriteKind", kind );
		set( "Occlude", dof ? 1 : 0 );
		set( "BokehOnly", dof ? 0 : 1 );
		set( "BokehRadiusPx", f( blurPx ) );
		set( "FringeOffset", f( -0.5 * fringe ), f( 0.5 * fringe ), f( -0.5 * fringe ) );
		set( "AreaUnit", f( area.behind ), f( area.front ) );
		set( "MinEnergy", kMinEnergy );
	};
	setAdditiveBlend();
	if( drawBokeh && gain > 0.0 )
	{
		spriteCommon( 0 );
		seti( "CellGrid", cellsX, cellsY );
		set( "CellLevel", spriteLevel );
		set( "CellAreaPx", f( ( W / cellsX ) * ( H / cellsY ) ) );
		drawInstanced( GL_TRIANGLE_STRIP, 4, cellsX * cellsY );
	}
	if( orbs > 0.0 && orbCount > 0 )
	{
		spriteCommon( 1 );
		set( "OrbLod", f( std::max( 0.0f, analysis.MaxMipLevel() - 3.0f ) ) );
		set( "OrbGain", f( 0.5 * orbs ) );
		set( "OrbRadiusPx", f( OrbSizeFromParam( params[ PT_ORB_SIZE ] ) * H ) );
		set( "DriftPhase", f( driftPhase ) );
		set( "TwinklePhase", f( twinklePhase ) );
		drawInstanced( GL_TRIANGLE_STRIP, 4, orbCount );
	}
	glDisable( GL_BLEND );

	//---------------------------------------------------------------------
	// 10. The flare.
	//---------------------------------------------------------------------
	const double flareGain = 100.0 * flare * flare;
	if( flareOn )
	{
		if( flareFrom == FlareFrom::Highlights )
		{
			begin( shaders::Pass::Lights, lights );
			bindUnit( kUnitPass, highlights.TextureID() );
			set( "Highlights", static_cast< int >( kUnitPass ) );
			seti( "LightGrid", shaders::kLightsX, shaders::kLightsY );
			set( "CellAreaPx", f( quarterPx * ( H / qh ) ) );
			quad.Draw();
		}

		into( bokeh );
		use( shaders::Pass::Flare );
		bindShared();
		shared();
		iris();
		bindUnit( kUnitPass, lights.TextureID() );
		set( "Lights", static_cast< int >( kUnitPass ) );
		seti( "LightGrid", shaders::kLightsX, shaders::kLightsY );
		set( "LightMode", static_cast< int >( flareFrom ) );
		set( "LightPos", f( clamp01( params[ PT_LIGHT_X ] ) ), f( clamp01( params[ PT_LIGHT_Y ] ) ) );
		set( "LightColour", kLightColour[ 0 ], kLightColour[ 1 ], kLightColour[ 2 ] );
		set( "LightEnergy", f( kLightEnergy * H * H ) );
		set( "MinLight", f( kMinLight * H * H ) );
		set( "FlareGain", f( flareGain ) );
		set( "GhostGain", f( clamp01( params[ PT_GHOSTS ] ) ) );
		set( "HaloGain", f( clamp01( params[ PT_HALO ] ) ) );
		set( "StarGain", f( clamp01( params[ PT_STARBURST ] ) * straight ) );
		set( "StreakGain", f( clamp01( params[ PT_STREAK ] ) ) );
		float ghostAt[ kGhostCount ], ghostRadius[ kGhostCount ], ghostReflect[ kGhostCount ], ghostTint[ 3 * kGhostCount ],
			ghostSoft[ kGhostCount ];
		for( int i = 0; i < kGhostCount; ++i )
		{
			const Ghost& g      = Ghosts()[ static_cast< size_t >( i ) ];
			ghostAt[ i ]        = g.at;
			ghostRadius[ i ]    = g.radius;
			ghostReflect[ i ]   = g.reflect;
			ghostSoft[ i ]      = g.soft;
			ghostTint[ 3 * i ]     = g.tint[ 0 ];
			ghostTint[ 3 * i + 1 ] = g.tint[ 1 ];
			ghostTint[ 3 * i + 2 ] = g.tint[ 2 ];
		}
		glUniform1fv( at( "GhostAt" ), kGhostCount, ghostAt );
		glUniform1fv( at( "GhostRadius" ), kGhostCount, ghostRadius );
		glUniform1fv( at( "GhostReflect" ), kGhostCount, ghostReflect );
		glUniform3fv( at( "GhostTint" ), kGhostCount, ghostTint );
		glUniform1fv( at( "GhostSoft" ), kGhostCount, ghostSoft );
		set( "AaPxFlare", f( halfPx ) );
		const int lightCount = flareFrom == FlareFrom::Highlights ? shaders::kLightsX * shaders::kLightsY : 1;
		setAdditiveBlend();
		drawInstanced( GL_TRIANGLE_STRIP, 4, lightCount * shaders::kFlareElements );
		glDisable( GL_BLEND );
	}

	//---------------------------------------------------------------------
	// 11. The composite, into the host's framebuffer and viewport.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pgl->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	use( shaders::Pass::Composite );
	bindShared();
	shared();
	bindUnit( kUnitPass, gather.TextureID() );
	bindUnit( kUnitPass + 1, bokeh.TextureID() );
	bindUnit( kUnitPass + 2, prefilter.TextureID() );
	bindUnit( kUnitPass + 3, highlights.TextureID() );
	set( "Blurred", static_cast< int >( kUnitPass ) );
	set( "Bokeh", static_cast< int >( kUnitPass + 1 ) );
	set( "Prefilter", static_cast< int >( kUnitPass + 2 ) );
	set( "Highlights", static_cast< int >( kUnitPass + 3 ) );
	set( "ModeIndex", static_cast< int >( mode ) );
	set( "ShowIndex", static_cast< int >( show ) );
	set( "MixAmount", f( clamp01( params[ PT_MIX ] ) ) );
	seti( "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
	set( "ViewSize", f( hostViewport[ 2 ] ), f( hostViewport[ 3 ] ) );
	set( "Exact", exact ? 1 : 0 );
	set( "DirtGain", f( 2.0 * clamp01( params[ PT_DIRT ] ) ) );
	set( "GlareLod", f( std::max( 0.0f, highlights.MaxMipLevel() - 3.0f ) ) );
	const int glareLight = flareOn && flareFrom != FlareFrom::Highlights ? static_cast< int >( flareFrom ) : 0;
	set( "LightMode", glareLight );
	set( "LightPos", f( clamp01( params[ PT_LIGHT_X ] ) ), f( clamp01( params[ PT_LIGHT_Y ] ) ) );
	set( "LightColour", kLightColour[ 0 ], kLightColour[ 1 ], kLightColour[ 2 ] );
	set( "LightEnergyGlare", f( 4.0 * flare * flare ) );
	set( "TrackHeights", f( trackSize ) );
	quad.Draw();

	unbindTextureUnits( kUnitCount );
	glActiveTexture( GL_TEXTURE0 );
	glUseProgram( 0 );
	current    = -1;
	firstFrame = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult HyacinthPlugin::DeInitGL()
{
	for( auto& program : programs )
		program.FreeGLResources();
	for( auto& cache : locations )
		cache.clear();
	quad.Release();
	if( emptyVao != 0 )
	{
		glDeleteVertexArrays( 1, &emptyVao );
		emptyVao = 0;
	}
	PassBuffer* all[] = { &analysis, &darkA, &darkB, &reduceA, &airlight, &histogram, &raw, &guideScratch,
		                  &guideMeans[ 0 ], &guideMeans[ 1 ], &guideMeans[ 2 ], &guideMeans[ 3 ], &guideCoef, &guide, &templates[ 0 ], &templates[ 1 ], &coarseScores, &coarseBest,
		                  &fineScores, &trackState[ 0 ], &trackState[ 1 ], &focusState[ 0 ], &focusState[ 1 ],
		                  &prefilter, &highlights, &tiles, &dilated, &gather, &bokeh, &lights };
	for( PassBuffer* b : all )
		b->Destroy();
	//A fresh context starts the tracker and the focus afresh.
	firstFrame = true;
	areaValid  = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// The harness's read-backs.
//---------------------------------------------------------------------------
namespace
{
bool readBack( const PassBuffer& buffer, std::vector< float >& out, int& width, int& height )
{
	if( !buffer.IsValid() )
		return false;
	width  = static_cast< int >( buffer.GetWidth() );
	height = static_cast< int >( buffer.GetHeight() );
	out.assign( static_cast< size_t >( width ) * height * 4, 0.0f );
	GLint previous = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previous );
	glBindFramebuffer( GL_FRAMEBUFFER, buffer.GetGLID() );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, out.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previous ) );
	return true;
}
} // namespace

HyacinthPlugin::TrackReadout HyacinthPlugin::ReadTrackForTest()
{
	TrackReadout r;
	std::vector< float > texels;
	int w = 0, h = 0;
	if( !readBack( trackState[ stateCur ], texels, w, h ) || w < 2 )
		return r;
	r.x          = texels[ 0 ];
	r.y          = texels[ 1 ];
	r.vx         = texels[ 2 ];
	r.vy         = texels[ 3 ];
	r.confidence = texels[ 4 ];
	r.frames     = texels[ 5 ];
	return r;
}

std::array< float, 2 > HyacinthPlugin::ReadFocusForTest()
{
	std::vector< float > texels;
	int w = 0, h = 0;
	if( !readBack( focusState[ focusCur ], texels, w, h ) )
		return { 0.0f, 0.0f };
	return { texels[ 0 ], texels[ 1 ] };
}

bool HyacinthPlugin::ReadBufferForTest( TestBuffer which, std::vector< float >& out, int& width, int& height )
{
	switch( which )
	{
	case TestBuffer::Prefilter: return readBack( prefilter, out, width, height );
	case TestBuffer::Gather: return readBack( gather, out, width, height );
	case TestBuffer::Bokeh: return readBack( bokeh, out, width, height );
	case TestBuffer::Highlights: return readBack( highlights, out, width, height );
	case TestBuffer::Guide: return readBack( guide, out, width, height );
	case TestBuffer::Lights: return readBack( lights, out, width, height );
	}
	return false;
}

//---------------------------------------------------------------------------
FFResult HyacinthPlugin::SetTime( double time )
{
	hostTime = time;
	return FF_SUCCESS;
}

char* HyacinthPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult HyacinthPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult HyacinthPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float HyacinthPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

} // namespace hyacinth
