/**
    hytest -- render Hyacinth offline, and measure the lens it claims to be.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, on a synthetic clock, in a headless CGL context, and reads the
    host's output (and, where a check needs it, one of the plugin's own
    buffers) back as floats. What the picture is measured against is worked
    out here again, differently and in double: the circle of confusion from
    the thin-lens equation in DISTANCE, the vignetted aperture's area from
    the closed form of two circles' overlap, a polygon's corners from its
    blade count, the focus pull from the exponential it should be, and the
    tracker against where this file moved the subject to.

    The pictures are built so that the output reads back as a measurement:

      - with Depth From = Clip Alpha the clip's alpha IS the nearness, so a
        check sets every pixel's depth exactly;
      - a single bright block on black, in Bokeh Only, leaves nothing in the
        bokeh buffer but that block's aperture.

        hytest --out /tmp/hyacinth.png    the defaults on the harness's street
        hytest --list                     every parameter and its default
        hytest --pipe                     raw RGBA frames in on stdin, out on stdout
        hytest --film N                   N frames of the street, raw RGBA on stdout
        hytest --offline                  the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; an option or an event STEPS.

    HYTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Controls.h"
#include "Hyacinth.h"
#include "Iris.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace hyacinth;


namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;
int g_checks   = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	++g_checks;
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

/// How far apart two pixels are, the worst channel, relative to the larger;
/// alpha must match exactly.
double relativeApart( const float* a, const float* b )
{
	double worst = 0.0;
	for( int c = 0; c < 4; ++c )
	{
		if( a[ c ] == b[ c ] )
			continue;
		if( c == 3 )
			return std::numeric_limits< double >::infinity();
		const double big = std::max( std::fabs( a[ c ] ), std::fabs( b[ c ] ) );
		worst            = std::max( worst, std::fabs( static_cast< double >( a[ c ] ) - b[ c ] ) / big );
	}
	return worst;
}

/// What two DRAWS of the same pixel through the same arithmetic may differ
/// by. On a GPU, nothing: bit for bit. Apple's software renderer does not
/// promise that: redrawing one unchanged frame there, the first few draws
/// after a program is linked differ from the later ones by up to 5.7e-6
/// (84 float steps), in blocks of rows, by a different amount each run --
/// consistent with the renderer swapping in a differently compiled shader
/// part way through a draw. So on it a claim that two draws agree is a claim
/// to 2e-5. The clip's own pixels (texelFetch, passed through) are bit for
/// bit everywhere, and every check of them says so.
double drawsAgreeWithin()
{
	return g_software ? 2e-5 : 0.0;
}
std::string agreement( double apart )
{
	if( !g_software )
		return "bit for bit";
	char text[ 96 ];
	std::snprintf( text, sizeof text, "software renderer: within %.0e, worst %.1e", drawsAgreeWithin(), apart );
	return text;
}

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "HYTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "hytest: HYTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_XPOS: return "x";
	case FF_TYPE_YPOS: return "y";
	case FF_TYPE_BUFFER: return "buffer";
	default: return "other";
	}
}

/// A control whose value is a choice, a switch or a press: cues STEP between
/// keys for these, and everything else ramps.
bool stepsBetweenCues( unsigned int type )
{
	return type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}


//---------------------------------------------------------------------------
// sRGB in double, for the references.
//---------------------------------------------------------------------------
double linearOf( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
double codeOf( double light )
{
	return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow( light, 1.0 / 2.4 ) - 0.055;
}



//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

/// Eight bits, as a host's clip is.
float eightBit( double v )
{
	return static_cast< float >( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) / 255.0 );
}
double smoothstep( double a, double b, double x )
{
	const double t = std::clamp( ( x - a ) / ( b - a ), 0.0, 1.0 );
	return t * t * ( 3.0 - 2.0 * t );
}

/// The harness's street at dusk, `t` seconds in: a paved ground receding
/// to a horizon 30% down the frame (where the default Plane puts it), a
/// string of lights over it, street lamps either side and the lit windows
/// of a town on the horizon, and a ball standing on the ground at the
/// default focus point, rolling side to side. The ground is exactly the
/// default plane's depth; the lights are above it, where the plane calls
/// them infinity -- which is what makes them bokeh.
Floats buildScene( int width, int height, double t = 0.0 )
{
	Floats out( static_cast< size_t >( width ) * height * 4 );
	const double aspect  = static_cast< double >( width ) / height;
	const double horizon = 0.7;
	//Ground: v = horizon - horizon (1 - Y) n for a point Y high at nearness n
	//(the camera one unit up).
	auto project = [ & ]( double X, double Y, double n, double& u, double& v ) {
		u = 0.5 + X * n * horizon / aspect;
		v = horizon - horizon * ( 1.0 - Y ) * n;
	};
	struct Light
	{
		double u, v, r;//r in heights
		double rgb[ 3 ];
	};
	std::vector< Light > lightsList;
	for( int k = 0; k < 23; ++k )
	{
		const double u   = ( k + 0.5 ) / 23.0;
		const double sag = 4.0 * u * ( 1.0 - u );
		const double hue = hash01( 31u, static_cast< uint32_t >( k ) );
		Light l { u, 0.93 - 0.09 * sag, 0.006, { 1.0, 0.75 + 0.25 * hue, 0.45 + 0.4 * ( 1.0 - hue ) } };
		lightsList.push_back( l );
	}
	for( double Z : { 1.6, 2.6, 4.2, 7.0, 12.0 } )
		for( double X : { -1.6, 1.6 } )
		{
			double u = 0.0, v = 0.0;
			project( X, 1.7, 1.0 / Z, u, v );
			lightsList.push_back( { u, v, 0.03 / Z, { 1.0, 0.82, 0.55 } } );
		}
	for( int k = 0; k < 40; ++k )
	{
		const double u = hash01( 77u, static_cast< uint32_t >( k ) );
		const double v = horizon + 0.004 + 0.035 * hash01( 78u, static_cast< uint32_t >( k ) );
		const double c = hash01( 79u, static_cast< uint32_t >( k ) );
		lightsList.push_back( { u, v, 0.0025, { 1.0, 0.7 + 0.3 * c, 0.3 + 0.5 * c } } );
	}

	//The ball: radius 0.3, resting on the ground at nearness 0.43.
	const double ballN = 0.43, ballR = 0.3;
	double ballU = 0.0, ballV = 0.0;
	project( 0.55 * std::sin( 0.7 * t ), ballR, ballN, ballU, ballV );
	const double ballRv = horizon * ballR * ballN;

	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 ) / width, v = ( y + 0.5 ) / height;
			double rgb[ 3 ];
			if( v >= horizon )
			{
				const double s = ( v - horizon ) / ( 1.0 - horizon );
				rgb[ 0 ]       = 0.30 + ( 0.05 - 0.30 ) * s;
				rgb[ 1 ]       = 0.20 + ( 0.07 - 0.20 ) * s;
				rgb[ 2 ]       = 0.32 + ( 0.18 - 0.32 ) * s;
				//A skyline: blocks of dark buildings on the horizon.
				const int block   = static_cast< int >( u * 37.0 );
				const double tall = 0.012 + 0.05 * hash01( 5u, static_cast< uint32_t >( block ) );
				if( v - horizon < tall )
					rgb[ 0 ] = 0.07, rgb[ 1 ] = 0.06, rgb[ 2 ] = 0.09;
			}
			else
			{
				const double n = ( horizon - v ) / horizon;
				const double X = ( u - 0.5 ) * aspect / ( n * horizon ), Z = 1.0 / n;
				const bool odd = ( static_cast< long >( std::floor( X * 2.0 ) ) + static_cast< long >( std::floor( Z * 2.0 ) ) ) & 1;
				//Fade the tiles out toward the horizon, where they would alias.
				const double tile = odd ? 1.0 : 0.0;
				const double fade = smoothstep( 0.0, 0.25, n );
				const double base[ 3 ] = { 0.36, 0.31, 0.26 }, dark[ 3 ] = { 0.22, 0.19, 0.17 };
				for( int c = 0; c < 3; ++c )
					rgb[ c ] = 0.28 + ( ( tile > 0.5 ? base[ c ] : dark[ c ] ) - 0.28 ) * fade;
				//The wet street's reflection of the lights' glow.
				rgb[ 0 ] += 0.06 * fade;
				rgb[ 1 ] += 0.03 * fade;
			}
			//The ball, shaded, with a highlight.
			const double bx = ( u - ballU ) * aspect / ballRv, by = ( v - ballV ) / ballRv;
			const double b2 = bx * bx + by * by;
			if( b2 < 1.0 )
			{
				const double nz    = std::sqrt( 1.0 - b2 );
				const double lit   = std::max( 0.0, -0.45 * bx + 0.55 * by + 0.7 * nz );
				const double spec  = std::pow( std::max( 0.0, -0.3 * bx + 0.5 * by + 0.81 * nz ), 60.0 );
				rgb[ 0 ]           = std::min( 1.0, 0.85 * ( 0.2 + 0.8 * lit ) + spec );
				rgb[ 1 ]           = std::min( 1.0, 0.28 * ( 0.2 + 0.8 * lit ) + spec );
				rgb[ 2 ]           = std::min( 1.0, 0.12 * ( 0.2 + 0.8 * lit ) + spec );
			}
			for( const Light& l : lightsList )
			{
				const double dx = ( u - l.u ) * aspect, dy = v - l.v;
				const double d  = std::sqrt( dx * dx + dy * dy ) / l.r;
				if( d < 1.6 )
				{
					const double core = 1.0 - smoothstep( 0.8, 1.0, d );
					const double halo = 0.25 * ( 1.0 - smoothstep( 1.0, 1.6, d ) );
					for( int c = 0; c < 3; ++c )
						rgb[ c ] = std::max( rgb[ c ], core * l.rgb[ c ] + halo * l.rgb[ c ] * 0.5 );
				}
			}
			float* p = &out[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			for( int c = 0; c < 3; ++c )
				p[ c ] = eightBit( rgb[ c ] );
			p[ 3 ] = 1.0f;
		}
	return out;
}

void fillFlat( Floats& card, double r, double g, double b, double a = 1.0 )
{
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = static_cast< float >( r );
		card[ i + 1 ] = static_cast< float >( g );
		card[ i + 2 ] = static_cast< float >( b );
		card[ i + 3 ] = static_cast< float >( a );
	}
}

Floats flatCard( int width, int height, double r, double g, double b, double a = 1.0 )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	fillFlat( card, r, g, b, a );
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin, a float output framebuffer, a synthetic clock and,
// when asked for, a synthetic beat.
//---------------------------------------------------------------------------
struct Rig
{
	HyacinthPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	int frame  = 0;
	double fps = 60.0;
	double bpm = 0.0;///< 0: no beat sent
	/// The host's clock: SetTime( origin + seconds x unit ), declared to the
	/// plugin as `clockScale` seconds a unit.
	double timeOrigin = 0.0, timeUnit = 1.0, clockScale = 1.0;
	std::function< void( int ) > beforeFrame;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		const Floats card = picture ? *picture : buildScene( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/hyacinth for which shader\n" );
			return false;
		}
		plugin.SetClockScaleForTest( clockScale );
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, float value )
	{
		plugin.SetFloatParameter( id, value );
	}
	template< typename E >
	void Option( unsigned int id, E value )
	{
		plugin.SetFloatParameter( id, static_cast< float >( static_cast< int >( value ) ) );
	}
	float Get( unsigned int id )
	{
		return plugin.GetFloatParameter( id );
	}

	double Seconds( int f ) const
	{
		return static_cast< double >( f ) / fps;
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			if( beforeFrame )
				beforeFrame( frame );
			plugin.SetTime( timeOrigin + Seconds( frame ) * timeUnit );
			if( bpm > 0.0 )
				plugin.SetBeatInfo( static_cast< float >( bpm ), static_cast< float >( std::fmod( Seconds( frame ) * bpm / 240.0, 1.0 ) ) );
			++frame;
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}
};

int byteOf( float v )
{
	return static_cast< int >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) );
}

const float* pixelAt( const Floats& image, int width, int x, int y )
{
	return &image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( HyacinthPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, or for an option the name of one of
/// its elements; anything else is refused, never read as 0 (polyhedral's
/// trap: strtof( "Scan" ) is 0, silently).
bool applySetting( HyacinthPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_TEXT || parameter.type == FF_TYPE_BUFFER )
			{
				error = name + " cannot be set from here";
				return false;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( HyacinthPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first, on the synthetic clock, with a
// beat at --bpm (120 unless told otherwise). --film renders the moving card.
//===========================================================================
int runPipe( int width, int height, double fps, double bpm, const std::string& scriptPath, int count, bool readStdin,
             const std::vector< std::string >& settings )
{
	Rig rig;
	rig.fps = fps;
	rig.bpm = bpm;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}
		else
			rig.Upload( buildScene( width, height, rig.Seconds( index ) ) );

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks           = 0;    ///< shaders::Hook bits
	bool clockAsSeconds = false;///< --clock: a millisecond host taken for a seconds one
	bool cuesRamp       = false;///< --cues: every control ramps
};

using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
/// A 720p frame and a small one: a disc's radius in pixels, a tracker's
/// sub-pixel accuracy and a bit-exact passthrough all have to hold at both.
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

void applyPerturb( Rig& rig, const Perturb& perturb )
{
	rig.plugin.SetHooksForTest( perturb.hooks );
}

/// The lens as quiet as it goes: nothing but what a check turns on.
void quiet( Rig& rig )
{
	rig.Set( PT_CAT_EYE, 0.0f );
	rig.Set( PT_SWIRL, 0.0f );
	rig.Set( PT_BUBBLE, ParamFromBubble( 0.0 ) );
	rig.Set( PT_ONION, 0.0f );
	rig.Set( PT_FRINGE, 0.0f );
	rig.Set( PT_SQUEEZE, ParamFromSqueeze( 1.0 ) );
	rig.Set( PT_FLARE, 0.0f );
	rig.Set( PT_ORBS, 0.0f );
	rig.Set( PT_DIRT, 0.0f );
	rig.Set( PT_FOCUS_SPEED, 0.0f );
}

Floats readBuffer( Rig& rig, HyacinthPlugin::TestBuffer which, int& w, int& h )
{
	Floats out;
	rig.plugin.ReadBufferForTest( which, out, w, h );
	return out;
}

//===========================================================================
// --passthrough: the clip, bit for bit, wherever the lens leaves it alone.
//===========================================================================
int runPassthrough( const Perturb& perturb )
{
	std::printf( "\n=== passthrough: at Mix 0 every mode, depth and view gives back the clip bit for bit; in focus, a pixel no blur "
	             "or bokeh reaches is the clip's own; alpha is mix( clip, 1, Mix )\n" );
	for( const Raster& raster : kRasters )
	{
		Floats scene = buildScene( raster.w, raster.h );
		//Alpha falling across the frame in eight-bit steps.
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
				scene[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + 3 ] = eightBit( 1.1 - 1.2 * ( x + 0.5 ) / raster.w );
		int mixWrong = 0, cases = 0;
		for( int mode = 0; mode < static_cast< int >( Mode::Count ); ++mode )
			for( int depth = 0; depth < static_cast< int >( DepthFrom::Count ); ++depth )
			{
				if( depth == static_cast< int >( DepthFrom::SideBySide ) || depth == static_cast< int >( DepthFrom::OverUnder ) )
					continue;//the output is one half of the clip, stretched: not the clip
				for( int view = 0; view < static_cast< int >( Show::Count ); ++view )
				{
					Rig rig;
					if( !rig.Init( raster.w, raster.h, &scene ) )
						return 1;
					rig.Set( PT_MODE, static_cast< float >( mode ) );
					rig.Set( PT_DEPTH_FROM, static_cast< float >( depth ) );
					rig.Set( PT_SHOW, static_cast< float >( view ) );
					rig.Set( PT_FLARE, 0.6f );
					rig.Set( PT_ORBS, 0.6f );
					rig.Set( PT_DIRT, 0.6f );
					rig.Set( PT_MIX, 0.0f );
					applyPerturb( rig, perturb );
					if( !rig.Render( 2 ) )
						return 1;
					const Floats out = rig.Output();
					for( size_t i = 0; i < out.size(); ++i )
						mixWrong += out[ i ] != scene[ i ];
					++cases;
				}
			}
		Check( mixWrong == 0, fmt( "%dx%d Mix 0, %d cases of mode, depth and view, with flare, orbs and dirt on: %d values changed",
		                           raster.w, raster.h, cases, mixWrong ) );

		//In focus: Clip Alpha nearness 0.5 over the top half (focused on),
		//0.1 under it (far behind). The sky's lights are in the sharp half.
		Floats split = buildScene( raster.w, raster.h );
		for( int y = 0; y < raster.h; ++y )
			for( int x = 0; x < raster.w; ++x )
				split[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + 3 ] = y >= raster.h / 2 ? 0.5f : 0.1f;
		for( float mixAmount : { 1.0f, 0.25f } )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &split ) )
				return 1;
			quiet( rig );
			rig.Option( PT_DEPTH_FROM, DepthFrom::ClipAlpha );
			rig.Option( PT_FOCUS, Focus::Manual );
			rig.Set( PT_FOCUS_DEPTH, 0.5f );
			rig.Set( PT_BLUR, ParamFromBlur( 0.05 ) );
			rig.Set( PT_MIX, mixAmount );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			//Clear of the boundary by the smoothstep's 1.5 pixels, a
			//half-resolution texel and the bilinear tap either side.
			const int clear = raster.h / 2 + 4;
			int changed = 0, checked = 0, alphaWrong = 0, blurred = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const float* o = pixelAt( out, raster.w, x, y );
					const float* s = pixelAt( split, raster.w, x, y );
					const float wantA = s[ 3 ] * ( 1.0f - mixAmount ) + mixAmount;
					alphaWrong += std::fabs( o[ 3 ] - wantA ) > 1e-6f;
					if( y >= clear )
					{
						++checked;
						changed += o[ 0 ] != s[ 0 ] || o[ 1 ] != s[ 1 ] || o[ 2 ] != s[ 2 ];
					}
					else if( y < raster.h / 2 - 8 )
						blurred += o[ 0 ] != s[ 0 ] || o[ 1 ] != s[ 1 ] || o[ 2 ] != s[ 2 ];
				}
			Check( changed == 0 && alphaWrong == 0 && blurred > 0,
			       fmt( "%dx%d Mix %.2f, focused on the top half: %d of %d sharp pixels changed (0 wanted), %d alphas not mix( clip, 1, Mix ), "
			            "%d blurred pixels changed (some wanted)",
			            raster.w, raster.h, mixAmount, changed, checked, alphaWrong, blurred ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back, and
// a resize under a running instance keeps the track and the focus.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back, in every path; a resize keeps the track and the "
	             "focus\n" );
	struct Path
	{
		const char* name;
		std::function< void( Rig& ) > set;
	};
	const Path paths[] = {
		{ "defaults", []( Rig& ) {} },
		{ "Track, Subject", []( Rig& r ) {
			 r.Option( PT_FOCUS, Focus::Track );
			 r.Option( PT_DEPTH_FROM, DepthFrom::Subject );
		 } },
		{ "Haze, flare", []( Rig& r ) {
			 r.Option( PT_DEPTH_FROM, DepthFrom::Haze );
			 r.Set( PT_FLARE, 0.7f );
			 r.Set( PT_DIRT, 0.5f );
		 } },
		{ "Bokeh Only, orbs, a light", []( Rig& r ) {
			 r.Option( PT_MODE, Mode::BokehOnly );
			 r.Set( PT_ORBS, 0.7f );
			 r.Set( PT_FLARE, 0.7f );
			 r.Option( PT_FLARE_FROM, FlareFrom::Light );
		 } },
	};
	for( const Path& path : paths )
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		path.set( rig );
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 6; ++frame )
		{
			if( frame == 2 && !rig.Resize( 256, 144 ) )
				return 1;
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, rig.width, rig.height );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, rig.width, rig.height );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			rig.plugin.SetTime( frame / 60.0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == rig.width && viewport[ 3 ] == rig.height, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( !glIsEnabled( GL_PROGRAM_POINT_SIZE ), "point-size" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
			expect( glGetError() == GL_NO_ERROR, "gl-error" );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		Check( problems == 0, fmt( "%-26s six frames, a resize on the third: viewport, vertex array, array buffer, program, active unit, "
		                           "framebuffer, blend, scissor, point size, clear colour, colour mask, eight texture units, no GL error "
		                           "(%d wrong:%s)",
		                           path.name, problems, what.empty() ? " none" : what.c_str() ) );
	}

	//The track and the focus are the instance's, not the raster's: a host
	//that changes the composition's size mid-show must not lose the subject
	//or snap the focus.
	{
		Rig rig;
		if( !rig.Init( 640, 360 ) )
			return 1;
		rig.Option( PT_FOCUS, Focus::Track );
		rig.Set( PT_FOCUS_SPEED, ParamFromFocusTau( 0.5 ) );
		rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildScene( rig.width, rig.height, rig.Seconds( f ) ) ); };
		if( !rig.Render( 30 ) )
			return 1;
		const auto trackBefore = rig.plugin.ReadTrackForTest();
		const auto focusBefore = rig.plugin.ReadFocusForTest();
		const int grabsBefore  = rig.plugin.Regrabs();
		if( !rig.Resize( 480, 270 ) || !rig.Render( 1 ) )
			return 1;
		const auto trackAfter = rig.plugin.ReadTrackForTest();
		const auto focusAfter = rig.plugin.ReadFocusForTest();
		//One frame of the ball's roll, at most: 0.55 x 0.7 rad/s x 0.43 x 0.7 / aspect.
		const double roll = 0.55 * 0.7 * 0.43 * 0.7 / ( 16.0 / 9.0 ) / 60.0;
		const double moved = std::hypot( trackAfter.x - trackBefore.x, trackAfter.y - trackBefore.y );
		//The focus may move by one frame's ease toward a target that has not moved.
		const double step = std::fabs( focusAfter[ 0 ] - focusBefore[ 0 ] );
		const double ease = ( 1.0 - std::exp( -1.0 / 60.0 / 0.5 ) ) * std::fabs( focusBefore[ 1 ] - focusBefore[ 0 ] ) + 0.01;
		Check( rig.plugin.Regrabs() == grabsBefore && moved <= 2.0 * roll + 0.004 && step <= ease,
		       fmt( "a resize from 640x360 to 480x270 30 frames into a track: grabbed again %d times (0 wanted); the track moved %.4f uv "
		            "(a frame's roll is %.4f); the focus moved %.4f (a frame's ease is at most %.4f)",
		            rig.plugin.Regrabs() - grabsBefore, moved, roll, step, ease ) );
	}
	return Verdict();
}

//===========================================================================
// --controls (no GL): every slider's ends and middle are what Controls.h
// says, written here as numbers, and every inverse undoes its mapping.
//===========================================================================
int runControls( const Perturb& )
{
	std::printf( "\n=== controls: each slider's ends and middle, against the numbers Controls.h states, and each inverse undoes its "
	             "mapping\n" );
	struct Mapping
	{
		const char* name;
		std::function< double( float ) > forward;
		std::function< float( double ) > inverse;
		double low, middle, high;///< in the units the header states
		double unit;             ///< what those units are in the plugin's
	};
	const double geo = -12345.0;//marks "the geometric mean": computed below
	const std::vector< Mapping > list = {
		{ "Blur Size (of height)", BlurFromParam, ParamFromBlur, 0.0, 0.04, 0.08, 1.0 },
		{ "Plane Angle (deg)", PlaneAngleFromParam, ParamFromPlaneAngle, 0.0, 180.0, 360.0, kDegree },
		{ "Horizon", HorizonFromParam, nullptr, 0.0, 0.5, 1.0, 1.0 },
		{ "Radius (heights)", RadiusFromParam, ParamFromRadius, 0.1, geo, 3.0, 1.0 },
		{ "Depth Edges (of height)", DepthEdgesFromParam, ParamFromDepthEdges, 0.005, geo, 0.08, 1.0 },
		{ "Track Size (of height)", TrackSizeFromParam, ParamFromTrackSize, 0.04, geo, 0.4, 1.0 },
		{ "Focus Speed tau (s)", FocusTauFromParam, ParamFromFocusTau, 0.0, 0.5, 2.0, 1.0 },
		{ "Blades", []( float v ) { return static_cast< double >( BladesFromParam( v ) ); },
		  []( double b ) { return ParamFromBlades( static_cast< int >( b ) ); }, 3.0, 8.0, 12.0, 1.0 },
		{ "Iris Rotate (deg)", IrisRotateFromParam, nullptr, 0.0, 180.0, 360.0, kDegree },
		{ "Cat Eye", CatEyeFromParam, nullptr, 0.0, 0.5, 1.0, 1.0 },
		{ "Swirl", SwirlFromParam, nullptr, 0.0, 0.5, 1.0, 1.0 },
		{ "Squeeze", SqueezeFromParam, ParamFromSqueeze, 1.0, 1.5, 2.0, 1.0 },
		{ "Bubble", BubbleFromParam, ParamFromBubble, -1.0, 0.0, 1.0, 1.0 },
		{ "Fringe (nearness)", FringeFromParam, ParamFromFringe, 0.0, 0.03, 0.06, 1.0 },
		{ "Highlights (gain)", HighlightGainFromParam, ParamFromHighlightGain, 0.0, 8.0, 32.0, 1.0 },
		{ "Orb Count", []( float v ) { return static_cast< double >( OrbCountFromParam( v ) ); }, nullptr, 0.0, 200.0, 400.0, 1.0 },
		{ "Orb Size (of height)", OrbSizeFromParam, ParamFromOrbSize, 0.01, geo, 0.15, 1.0 },
		{ "Orb Drift (heights/s)", OrbDriftFromParam, nullptr, 0.0, 0.1, 0.2, 1.0 },
	};
	for( const Mapping& m : list )
	{
		const double middle    = m.middle == geo ? std::sqrt( m.low * m.high ) : m.middle;
		const double want[ 3 ] = { m.low, middle, m.high };
		double worst           = 0.0;
		for( int k = 0; k < 3; ++k )
		{
			const double got = m.forward( 0.5f * static_cast< float >( k ) ) / m.unit;
			worst            = std::max( worst, std::fabs( got - want[ k ] ) / std::max( 1.0, std::fabs( want[ k ] ) ) );
		}
		double trip = 0.0;
		if( m.inverse )
			for( int k = 0; k <= 9; ++k )
			{
				//Blades is whole steps: round trip on its own steps.
				const float v = static_cast< float >( k ) / 9.0f;
				trip          = std::max( trip, static_cast< double >( std::fabs( m.inverse( m.forward( v ) ) - v ) ) );
			}
		Check( worst < 1e-9 && trip < 1e-6, fmt( "%-26s %g, %g, %g at 0, 0.5, 1 (worst %.1e, bound 1e-9); inverse round trip %.1e (bound 1e-6)",
		                                        m.name, m.forward( 0.0f ) / m.unit, m.forward( 0.5f ) / m.unit, m.forward( 1.0f ) / m.unit,
		                                        worst, trip ) );
	}
	//Options read back 0..1 in some hosts whatever their count (the fleet's
	//trap): the index comes from the table's own count, never past its end.
	Check( OptionIndex( 7.0f, 8 ) == 7 && OptionIndex( 9.0f, 8 ) == 7 && OptionIndex( -1.0f, 8 ) == 0 && OptionIndex( NAN, 8 ) == 0
	           && OptionIndex( 0.6f, 3 ) == 1,
	       "an option's index is clamped to its own count, and a NaN is the first element" );
	return Verdict();
}

//===========================================================================
// --cues (no GL): options and events step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options, switches and events, and ramps standard controls\n" );
	std::istringstream sheet( "0 Shape 0\n60 Shape 3\n0 Depth Invert 0\n60 Depth Invert 1\n0 Blur Size 0\n60 Blur Size 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	HyacinthPlugin plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid = valueAt( found->second, 29, ramp );
		const float a = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	HyacinthPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	int optionNames = 0;
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
		for( int e = 0; e < InfoOf( id ).optionCount; ++e )
			optionNames += std::strlen( InfoOf( id ).options[ e ] ) > 16;
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About";
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast && optionNames == 0,
	       fmt( "SW Hyacinth: %u parameters, %d too long, %d duplicated, %d clashing addresses, %d option names too long; the About "
	            "block is last (%s)",
	            plugin.GetNumParams(), longNames, dupes, clashes, optionNames, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}


//===========================================================================
// Measuring a picture of light.
//===========================================================================

/// A black clip whose alpha is `nearness` everywhere (Depth From = Clip
/// Alpha reads it as the depth), with a `side`-pixel block of code value
/// `code` whose corner is at (x0, y0).
Floats pointPicture( int w, int h, int x0, int y0, int side, double nearness, double code = 1.0 )
{
	Floats out( static_cast< size_t >( w ) * h * 4, 0.0f );
	for( size_t i = 3; i < out.size(); i += 4 )
		out[ i ] = static_cast< float >( nearness );
	for( int y = y0; y < y0 + side; ++y )
		for( int x = x0; x < x0 + side; ++x )
			for( int c = 0; c < 3; ++c )
				out[ ( static_cast< size_t >( y ) * w + x ) * 4 + c ] = static_cast< float >( code );
	return out;
}

/// A block of `side` pixels at about (fx, fy) of the frame, on the quarter
/// grid so that it is one highlight cell.
void blockAt( int w, int h, double fx, double fy, int& x0, int& y0 )
{
	x0 = 8 * static_cast< int >( std::lround( fx * w / 8.0 ) );
	y0 = 8 * static_cast< int >( std::lround( fy * h / 8.0 ) );
}

/// The light a block of `side` pixels at code `code` puts into the sprites:
/// the scene's light restored at that code, less Threshold's. Worked out
/// here from Controls.h's words: a gain that reaches Gain at white, and the
/// light above Threshold's.
double excessOfBlock( double code, double gain, double threshold, int side )
{
	const double s        = std::clamp( ( code - threshold ) / ( 1.0 - threshold ), 0.0, 1.0 );
	const double restored = linearOf( code ) * ( 1.0 + gain * s * s );
	return std::max( 0.0, restored - linearOf( threshold ) ) * side * side;
}

/// Light-weighted moments of one channel of a buffer, in FULL-resolution
/// pixels (a buffer texel is `scale` pixels): the total (light x pixels), the
/// centre and the mean squared distance from it.
struct Moments
{
	double total = 0.0, cx = 0.0, cy = 0.0, r2 = 0.0, xx = 0.0, yy = 0.0;
};
Moments momentsOf( const Floats& buffer, int w, int h, double scaleX, double scaleY, int channel )
{
	Moments m;
	double sx = 0.0, sy = 0.0, sxx = 0.0, syy = 0.0;
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const double v = buffer[ ( static_cast< size_t >( y ) * w + x ) * 4 + channel ];
			if( v == 0.0 )
				continue;
			const double px = ( x + 0.5 ) * scaleX, py = ( y + 0.5 ) * scaleY;
			m.total += v;
			sx += v * px;
			sy += v * py;
			sxx += v * px * px;
			syy += v * py * py;
		}
	if( m.total <= 0.0 )
		return m;
	m.cx = sx / m.total;
	m.cy = sy / m.total;
	m.xx = sxx / m.total - m.cx * m.cx;
	m.yy = syy / m.total - m.cy * m.cy;
	m.r2 = m.xx + m.yy;
	m.total *= scaleX * scaleY;
	return m;
}

/// Bilinear read of one channel at a point in buffer texels.
double sampleAt( const Floats& buffer, int w, int h, double x, double y, int channel )
{
	x -= 0.5;
	y -= 0.5;
	const int x0 = static_cast< int >( std::floor( x ) ), y0 = static_cast< int >( std::floor( y ) );
	const double fx = x - x0, fy = y - y0;
	auto at = [ & ]( int xi, int yi ) {
		xi = std::clamp( xi, 0, w - 1 );
		yi = std::clamp( yi, 0, h - 1 );
		return static_cast< double >( buffer[ ( static_cast< size_t >( yi ) * w + xi ) * 4 + channel ] );
	};
	return ( at( x0, y0 ) * ( 1 - fx ) + at( x0 + 1, y0 ) * fx ) * ( 1 - fy ) + ( at( x0, y0 + 1 ) * ( 1 - fx ) + at( x0 + 1, y0 + 1 ) * fx ) * fy;
}

/// The output's linear light (the output buffer is float: nothing clips).
Floats linearOutput( const Floats& out )
{
	Floats lin( out.size() );
	for( size_t i = 0; i < out.size(); ++i )
		lin[ i ] = ( i % 4 == 3 ) ? out[ i ] : static_cast< float >( linearOf( out[ i ] ) );
	return lin;
}

/// A lone highlight in the dark, in a rig of its own: the lens set by
/// `setup`, the block at (fx, fy) of the frame, the clip's nearness `n`.
struct Shot
{
	Floats bokeh;
	int bw = 0, bh = 0;
	double scaleX = 1.0, scaleY = 1.0;
	Floats out;
	int x0 = 0, y0 = 0;
};
bool shoot( const Raster& raster, double fx, double fy, int side, double n, const std::function< void( Rig& ) >& setup,
            const Perturb& perturb, Shot& shot )
{
	blockAt( raster.w, raster.h, fx, fy, shot.x0, shot.y0 );
	const Floats picture = pointPicture( raster.w, raster.h, shot.x0, shot.y0, side, n );
	Rig rig;
	if( !rig.Init( raster.w, raster.h, &picture ) )
		return false;
	quiet( rig );
	rig.Option( PT_DEPTH_FROM, DepthFrom::ClipAlpha );
	rig.Option( PT_FOCUS, Focus::Manual );
	setup( rig );
	applyPerturb( rig, perturb );
	if( !rig.Render( 2 ) )
		return false;
	shot.bokeh  = readBuffer( rig, HyacinthPlugin::TestBuffer::Bokeh, shot.bw, shot.bh );
	shot.scaleX = static_cast< double >( raster.w ) / shot.bw;
	shot.scaleY = static_cast< double >( raster.h ) / shot.bh;
	shot.out    = rig.Output();
	return true;
}

//===========================================================================
// --coc: a defocused point's disc is as wide as the thin lens says: its
// radius is Blur Size x | n - n_focus |, linear in NEARNESS, the same in
// front of focus as behind.
//===========================================================================
int runCoc( const Perturb& perturb )
{
	std::printf( "\n=== coc: a point's disc, in the output, has radius Blur Size x |n - n_focus| -- linear in nearness (1/distance), the "
	             "same either side of focus\n" );
	for( const Raster& raster : kRasters )
	{
		const double B = 0.08 * raster.h;
		//The thin lens, in DISTANCE: a point at d, the lens focused at s,
		//blurs to a circle of diameter A f |d - s| / ( d ( s - f ) ). With s
		//far beyond f this is A f |1/s - 1/d|... the harness writes it as
		//distances and lets the arithmetic find the nearness in it.
		auto thinLens = [ & ]( double n, double nf ) {
			const double d = 1.0 / n, s = 1.0 / nf;
			return B * std::fabs( d - s ) / ( d * s );
		};
		const double nf = 0.5;
		std::vector< double > want, got2;
		std::string line;
		for( double n : { 0.05, 0.15, 0.25, 0.75, 0.85, 0.95 } )
		{
			Shot shot;
			if( !shoot( raster, 0.5, 0.5, 4, n, [ & ]( Rig& r ) {
				    r.Set( PT_FOCUS_DEPTH, static_cast< float >( nf ) );
				    r.Set( PT_BLUR, 1.0f );
				    r.Set( PT_BLADE_CURVE, 1.0f );
				    r.Set( PT_BUBBLE, ParamFromBubble( 0.0 ) );
			    }, perturb, shot ) )
				return 1;
			const Floats lin = linearOutput( shot.out );
			const Moments m  = momentsOf( lin, raster.w, raster.h, 1.0, 1.0, 1 );
			want.push_back( thinLens( n, nf ) );
			got2.push_back( 2.0 * m.r2 );//a uniform disc of radius R has <r^2> = R^2 / 2
			line += fmt( " %.2f:%.1f/%.1f", n, std::sqrt( 2.0 * m.r2 ), want.back() );
		}
		//The rasters and ramps add a constant to R^2 (a blur's variance adds);
		//fit R_measured^2 = a R_lens^2 + c and require a = 1.
		double sx = 0, sy = 0, sxx = 0, sxy = 0;
		const double k = static_cast< double >( want.size() );
		for( size_t i = 0; i < want.size(); ++i )
		{
			const double x = want[ i ] * want[ i ];
			sx += x;
			sy += got2[ i ];
			sxx += x * x;
			sxy += x * got2[ i ];
		}
		const double a = ( k * sxy - sx * sy ) / ( k * sxx - sx * sx );
		const double c = ( sy - a * sx ) / k;
		double worst   = 0.0;
		for( size_t i = 0; i < want.size(); ++i )
			worst = std::max( worst, std::fabs( std::sqrt( std::max( got2[ i ] - c, 0.0 ) ) - want[ i ] ) );
		//Bound: the disc's edge is a one-texel ramp at half resolution and the
		//output reads it back through a bilinear tap, a constant the fit
		//takes out; what is left is where the edge falls between texel
		//centres, under half a half-resolution texel (one pixel). The slope
		//is held to the same pixel at the widest disc: a slope that moved
		//that disc's edge by more is a wrong lens, one that moved it by less
		//is the raster. (At 180p the widest disc is 6.5 px, so there the
		//points carry the check; the slope's bound is the 720p raster's.)
		const double slopeBound = 1.0 / *std::max_element( want.begin(), want.end() );
		Check( std::fabs( std::sqrt( a ) - 1.0 ) < slopeBound && worst < 1.0,
		       fmt( "%dx%d Blur %.1f px, focus 0.5; radius measured/lens at n =%s; fitted slope %.4f (1 +- %.3f), offset %.2f px^2, worst "
		            "point %.2f px (bound 1 px)",
		            raster.w, raster.h, B, line.c_str(), std::sqrt( a ), slopeBound, c, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --energy: a defocused point keeps its light, whatever the aperture's
// shape, profile or squeeze -- except what the barrel's shadow takes, which
// is the closed form's lens of two circles.
//===========================================================================
int runEnergy( const Perturb& perturb )
{
	std::printf( "\n=== energy: the bokeh of a highlight carries exactly the light above Threshold, for every shape, profile and squeeze, in "
	             "front of focus and behind; the cat's eye loses what two circles' overlap says\n" );
	struct Case
	{
		const char* name;
		double fx, fy;
		bool dof;
		double n, nf;
		std::function< void( Rig& ) > set;
		double keep;///< the fraction of the light that survives; < 0: the cat's eye at that field
	};
	//The field position of (fx, fy), as the plugin's shader defines it:
	//picture heights from the centre over the half-diagonal. Written here
	//from Controls.h's words.
	const double catEye = 0.8;
	const std::vector< Case > cases = {
		{ "Iris 7, curve 0.25", 0.5, 0.5, false, 0.5, 0.5, []( Rig& ) {}, 1.0 },
		{ "Iris 6, straight", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) {
			 r.Set( PT_BLADES, ParamFromBlades( 6 ) );
			 r.Set( PT_BLADE_CURVE, 0.0f );
		 }, 1.0 },
		{ "Iris 3, straight", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) {
			 r.Set( PT_BLADES, ParamFromBlades( 3 ) );
			 r.Set( PT_BLADE_CURVE, 0.0f );
		 }, 1.0 },
		{ "round", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) { r.Set( PT_BLADE_CURVE, 1.0f ); }, 1.0 },
		{ "Mirror", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) { r.Option( PT_SHAPE, Shape::Mirror ); }, 1.0 },
		{ "Star", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) { r.Option( PT_SHAPE, Shape::Star ); }, 1.0 },
		{ "Heart", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) { r.Option( PT_SHAPE, Shape::Heart ); }, 1.0 },
		{ "Bubble -0.8", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) { r.Set( PT_BUBBLE, ParamFromBubble( -0.8 ) ); }, 1.0 },
		{ "Bubble +0.8, Onion 0.7", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) {
			 r.Set( PT_BUBBLE, ParamFromBubble( 0.8 ) );
			 r.Set( PT_ONION, 0.7f );
		 }, 1.0 },
		{ "in front, Bubble 0.6", 0.5, 0.5, true, 0.9, 0.1, []( Rig& r ) { r.Set( PT_BUBBLE, ParamFromBubble( 0.6 ) ); }, 1.0 },
		{ "behind, Bubble 0.6", 0.5, 0.5, true, 0.1, 0.9, []( Rig& r ) { r.Set( PT_BUBBLE, ParamFromBubble( 0.6 ) ); }, 1.0 },
		{ "Squeeze 1.7", 0.5, 0.5, false, 0.5, 0.5, []( Rig& r ) { r.Set( PT_SQUEEZE, ParamFromSqueeze( 1.7 ) ); }, 1.0 },
		{ "Swirl 1, off centre", 0.8, 0.75, false, 0.5, 0.5, []( Rig& r ) { r.Set( PT_SWIRL, 1.0f ); }, 1.0 },
		{ "Fringe 0.05, in front", 0.5, 0.5, true, 0.9, 0.1, []( Rig& r ) { r.Set( PT_FRINGE, ParamFromFringe( 0.05 ) ); }, 1.0 },
		{ "Cat Eye 0.8, round, off centre", 0.82, 0.78, false, 0.5, 0.5, [ = ]( Rig& r ) {
			 r.Set( PT_BLADE_CURVE, 1.0f );
			 r.Set( PT_CAT_EYE, static_cast< float >( catEye ) );
		 }, -1.0 },
	};
	for( const Raster& raster : kRasters )
	{
		const double gain = 8.0, threshold = 0.75;
		const int side    = 4;
		const double want = excessOfBlock( 1.0, gain, threshold, side );
		const double AaPx = static_cast< double >( raster.w ) / ( ( raster.w + 1 ) / 2 );
		for( const Case& c : cases )
		{
			//Every disc 0.05 H in radius, in focus or not: 9 px at the small
			//raster, so a defocused one is wholly the sprites' (a highlight
			//under 6 px is handed to the gather in part, and its light is then
			//in the gather's buffer, not this one).
			const double blur = c.dof ? 0.05 / std::fabs( c.n - c.nf ) : 0.05;
			Shot shot;
			if( !shoot( raster, c.fx, c.fy, side, c.n, [ & ]( Rig& r ) {
				    r.Option( PT_MODE, c.dof ? Mode::DepthOfField : Mode::BokehOnly );
				    r.Set( PT_FOCUS_DEPTH, static_cast< float >( c.nf ) );
				    r.Set( PT_BLUR, ParamFromBlur( blur ) );
				    r.Set( PT_BUBBLE, ParamFromBubble( 0.0 ) );
				    c.set( r );
			    }, perturb, shot ) )
				return 1;
			double keep = c.keep;
			if( keep < 0.0 )
			{
				//Two unit circles whose centres are s apart overlap in
				//2 acos( s/2 ) - ( s/2 ) sqrt( 4 - s^2 ), of pi.
				const double hx = ( ( shot.x0 + 0.5 * side ) / raster.w - 0.5 ) * raster.w / raster.h;
				const double hy = ( shot.y0 + 0.5 * side ) / raster.h - 0.5;
				const double halfDiagonal = 0.5 * std::sqrt( std::pow( static_cast< double >( raster.w ) / raster.h, 2.0 ) + 1.0 );
				const double s = catEye * std::hypot( hx, hy ) / halfDiagonal;
				keep = ( 2.0 * std::acos( 0.5 * s ) - 0.5 * s * std::sqrt( 4.0 - s * s ) ) / kPi;
			}
			const double radius = 0.05 * raster.h;
			double worst        = 0.0;
			double got[ 3 ];
			for( int ch = 0; ch < 3; ++ch )
			{
				got[ ch ] = momentsOf( shot.bokeh, shot.bw, shot.bh, shot.scaleX, shot.scaleY, ch ).total;
				worst     = std::max( worst, std::fabs( got[ ch ] / ( want * keep ) - 1.0 ) );
			}
			//Bound: the edge's ramp is one texel of the buffer it is drawn
			//into, which a sum over texel centres integrates exactly along a
			//straight edge; a corner or a curve leaves a remainder of the
			//order of the ramp's width squared over the disc's area, w^2/R^2.
			const double bound = 0.01 + 2.0 * ( AaPx / radius ) * ( AaPx / radius );
			Check( worst < bound, fmt( "%dx%d %-30s %7.1f %7.1f %7.1f light x px, want %7.1f (x %.4f kept): worst %.2f%% (bound %.2f%%)",
			                           raster.w, raster.h, c.name, got[ 0 ], got[ 1 ], got[ 2 ], want * keep, keep, 100.0 * worst,
			                           100.0 * bound ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --shape: the aperture's corners are its blades, where the blade count and
// Iris Rotate put them; in front of focus it shows the other way up.
//===========================================================================

/// The disc's outline: its radius in each of `count` directions (0 = +x,
/// counter-clockwise, y up), where the light falls to half the disc's
/// interior, read on the buffer from the light's centre.
std::vector< double > outlineOf( const Shot& shot, double cx, double cy, double reach, int count, double& interior )
{
	//The interior: the median of a disc of half the radius.
	std::vector< double > inside;
	for( int y = 0; y < shot.bh; ++y )
		for( int x = 0; x < shot.bw; ++x )
		{
			const double px = ( x + 0.5 ) * shot.scaleX - cx, py = ( y + 0.5 ) * shot.scaleY - cy;
			if( px * px + py * py < 0.16 * reach * reach )
				inside.push_back( shot.bokeh[ ( static_cast< size_t >( y ) * shot.bw + x ) * 4 + 1 ] );
		}
	std::sort( inside.begin(), inside.end() );
	interior = inside.empty() ? 0.0 : inside[ inside.size() / 2 ];
	std::vector< double > radius( static_cast< size_t >( count ) );
	for( int i = 0; i < count; ++i )
	{
		const double a = 2.0 * kPi * i / count;
		const double dx = std::cos( a ), dy = std::sin( a );
		double r = 0.0;
		for( double t = 0.2 * reach; t < 2.0 * reach; t += 0.05 )
		{
			const double v = sampleAt( shot.bokeh, shot.bw, shot.bh, ( cx + t * dx ) / shot.scaleX, ( cy + t * dy ) / shot.scaleY, 1 );
			if( v < 0.5 * interior )
			{
				r = t;
				break;
			}
		}
		radius[ static_cast< size_t >( i ) ] = r;
	}
	return radius;
}

/// The strongest of harmonics 3..12 of the outline, and where harmonic
/// `k`'s peak is (degrees, mod 360/k).
int dominantHarmonic( const std::vector< double >& radius, int k, double& peakDegrees, double& strength )
{
	const int n  = static_cast< int >( radius.size() );
	int best     = 0;
	double bestA = -1.0;
	for( int h = 3; h <= 12; ++h )
	{
		double re = 0.0, im = 0.0;
		for( int i = 0; i < n; ++i )
		{
			const double a = 2.0 * kPi * i / n;
			re += radius[ static_cast< size_t >( i ) ] * std::cos( h * a );
			im += radius[ static_cast< size_t >( i ) ] * std::sin( h * a );
		}
		const double amp = std::hypot( re, im ) / n;
		if( amp > bestA )
		{
			bestA = amp;
			best  = h;
		}
		if( h == k )
		{
			const double phase = std::atan2( im, re ) / h;
			double deg         = phase / kDegree;
			const double period = 360.0 / h;
			deg                 = std::fmod( std::fmod( deg, period ) + period, period );
			peakDegrees         = deg;
			strength            = amp;
		}
	}
	return best;
}

int runShape( const Perturb& perturb )
{
	std::printf( "\n=== shape: an N-bladed iris's bokeh has N corners, one straight up behind focus, turned by Iris Rotate; in front of "
	             "focus the aperture shows upside down; Squeeze makes it that much taller\n" );
	struct Case
	{
		int blades;
		double rotate;//degrees
		bool front;
	};
	const Case cases[] = { { 5, 0.0, false }, { 5, 0.0, true }, { 8, 0.0, false }, { 7, 20.0, false }, { 7, 20.0, true }, { 3, 0.0, true } };
	for( const Raster& raster : kRasters )
	{
		for( const Case& c : cases )
		{
			const double n = c.front ? 0.9 : 0.1, nf = c.front ? 0.2 : 0.8;
			Shot shot;
			if( !shoot( raster, 0.5, 0.5, 4, n, [ & ]( Rig& r ) {
				    r.Set( PT_FOCUS_DEPTH, static_cast< float >( nf ) );
				    r.Set( PT_BLUR, 1.0f );
				    r.Set( PT_BLADES, ParamFromBlades( c.blades ) );
				    r.Set( PT_BLADE_CURVE, 0.0f );
				    r.Set( PT_BUBBLE, ParamFromBubble( 0.0 ) );
				    r.Set( PT_IRIS_ROTATE, static_cast< float >( c.rotate / 360.0 ) );
			    }, perturb, shot ) )
				return 1;
			const Moments m   = momentsOf( shot.bokeh, shot.bw, shot.bh, shot.scaleX, shot.scaleY, 1 );
			const double R    = 0.08 * raster.h * 0.7;
			double interior   = 0.0;
			const auto radius = outlineOf( shot, m.cx, m.cy, R, 720, interior );
			double peak = 0.0, strength = 0.0;
			const int dominant = dominantHarmonic( radius, c.blades, peak, strength );
			//A corner straight up behind focus; in front, the aperture turned
			//half a turn.
			const double period = 360.0 / c.blades;
			double want         = 90.0 + c.rotate + ( c.front ? 180.0 : 0.0 );
			want                = std::fmod( std::fmod( want, period ) + period, period );
			double miss         = std::fabs( peak - want );
			miss                = std::min( miss, period - miss );
			//Bound: the outline is read to a twentieth of a pixel along each
			//ray, but the edge it finds is drawn on the half-resolution grid;
			//a corner R px out located to one texel (2 px) is 2/R radians.
			const double bound = std::max( 1.5, ( 2.0 * shot.scaleX / R ) / kDegree );
			Check( dominant == c.blades && miss < bound,
			       fmt( "%dx%d %d blades, Iris Rotate %g, %s focus: strongest harmonic %d; a corner at %.2f deg (mod %.1f), want %.2f "
			            "(miss %.2f, bound %.2f)",
			            raster.w, raster.h, c.blades, c.rotate, c.front ? "in front of" : "behind", dominant, peak, period, want, miss,
			            bound ) );
		}
		//Squeeze: a round aperture, Squeeze 1.6, comes out 1.6 times as
		//tall as it is wide.
		Shot shot;
		if( !shoot( raster, 0.5, 0.5, 4, 0.1, [ & ]( Rig& r ) {
			    r.Set( PT_FOCUS_DEPTH, 0.8f );
			    r.Set( PT_BLUR, 1.0f );
			    r.Set( PT_BLADE_CURVE, 1.0f );
			    r.Set( PT_SQUEEZE, ParamFromSqueeze( 1.6 ) );
		    }, perturb, shot ) )
			return 1;
		const Moments m = momentsOf( shot.bokeh, shot.bw, shot.bh, shot.scaleX, shot.scaleY, 1 );
		//An ellipse's second moments are a^2/4 and b^2/4; the half-texel
		//raster adds the same variance to both, so compare the differences
		//against a round shot.
		Shot round;
		if( !shoot( raster, 0.5, 0.5, 4, 0.1, [ & ]( Rig& r ) {
			    r.Set( PT_FOCUS_DEPTH, 0.8f );
			    r.Set( PT_BLUR, 1.0f );
			    r.Set( PT_BLADE_CURVE, 1.0f );
		    }, perturb, round ) )
			return 1;
		const Moments o     = momentsOf( round.bokeh, round.bw, round.bh, round.scaleX, round.scaleY, 1 );
		const double extra  = o.xx - 0.25 * std::pow( 0.08 * raster.h * 0.7, 2.0 );
		const double ratio  = std::sqrt( std::max( m.yy - extra, 0.0 ) / std::max( m.xx - extra, 1e-9 ) );
		Check( std::fabs( ratio - 1.6 ) < 0.03, fmt( "%dx%d Squeeze 1.6, round: the disc is %.3f times as tall as wide (1.6 +- 0.03)", raster.w,
		                                            raster.h, ratio ) );
	}
	return Verdict();
}

//===========================================================================
// --fringe: longitudinal chromatic aberration. Red and blue come to focus
// farther than green, so in front of focus their discs are the larger
// (a magenta rim) and behind it the smaller (a green rim), by Blur Size x
// the Fringe either way.
//===========================================================================
int runFringe( const Perturb& perturb )
{
	std::printf( "\n=== fringe: red's and blue's discs are larger than green's in front of focus and smaller behind, R_r^2 - R_g^2 = 2 coc "
	             "Blur Size x Fringe\n" );
	for( const Raster& raster : kRasters )
		for( bool front : { true, false } )
		{
			const double n = front ? 0.85 : 0.1, nf = front ? 0.35 : 0.6, fringe = 0.05;
			Shot shot;
			if( !shoot( raster, 0.5, 0.5, 4, n, [ & ]( Rig& r ) {
				    r.Set( PT_FOCUS_DEPTH, static_cast< float >( nf ) );
				    r.Set( PT_BLUR, 1.0f );
				    r.Set( PT_BLADE_CURVE, 1.0f );
				    r.Set( PT_FRINGE, ParamFromFringe( fringe ) );
			    }, perturb, shot ) )
				return 1;
			const double B   = 0.08 * raster.h;
			const double coc = B * ( n - nf );
			//Each channel's disc: radius | coc - B ( nf_c - nf ) |, with red and
			//blue focused half a Fringe farther (smaller nearness) than the
			//mean and green half nearer. Worked out here as distances squared.
			const double rr = std::fabs( coc + 0.5 * B * fringe ), rg = std::fabs( coc - 0.5 * B * fringe );
			const double want = rr * rr - rg * rg;
			const Moments mr = momentsOf( shot.bokeh, shot.bw, shot.bh, shot.scaleX, shot.scaleY, 0 );
			const Moments mg = momentsOf( shot.bokeh, shot.bw, shot.bh, shot.scaleX, shot.scaleY, 1 );
			const Moments mb = momentsOf( shot.bokeh, shot.bw, shot.bh, shot.scaleX, shot.scaleY, 2 );
			const double gotR = 2.0 * ( mr.r2 - mg.r2 ), gotB = 2.0 * ( mb.r2 - mg.r2 );
			//Bound: each disc's radius is good to a fraction of a texel (--coc);
			//the difference of two squares R^2 at R ~ 30 px to 2 R x 0.5 px.
			const double bound = 2.0 * std::fabs( coc ) * 0.5 * shot.scaleX;
			Check( std::fabs( gotR - want ) < bound && std::fabs( gotB - want ) < bound && gotR * want > 0.0,
			       fmt( "%dx%d %s focus (coc %+.1f px): R_r^2 - R_g^2 = %+.1f, R_b^2 - R_g^2 = %+.1f px^2, want %+.1f (bound %.1f)", raster.w,
			            raster.h, front ? "in front of" : "behind", coc, gotR, gotB, want, bound ) );
		}
	return Verdict();
}


//===========================================================================
// --starburst: diffraction off straight blade edges. A line through the
// light per edge, along the edge's normal: N spikes for an even blade
// count (opposite edges share a line), 2N for an odd one, none for a round
// aperture.
//===========================================================================
int runStarburst( const Perturb& perturb )
{
	std::printf( "\n=== starburst: N blades give N spikes when N is even and 2N when odd, along the blade edges' normals; a round aperture "
	             "gives none\n" );
	struct Case
	{
		int blades;
		double curve, rotate;
		Shape shape;
		int want;
	};
	const Case cases[] = { { 6, 0.0, 0.0, Shape::Iris, 6 }, { 7, 0.0, 0.0, Shape::Iris, 14 }, { 5, 0.3, 15.0, Shape::Iris, 10 },
		                   { 8, 0.0, 0.0, Shape::Iris, 8 }, { 7, 1.0, 0.0, Shape::Iris, 0 }, { 7, 0.0, 0.0, Shape::Mirror, 0 },
		                   { 3, 0.0, 0.0, Shape::Star, 10 } };
	for( const Raster& raster : kRasters )
		for( const Case& c : cases )
		{
			const Floats black = pointPicture( raster.w, raster.h, 0, 0, 0, 0.5 );
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &black ) )
				return 1;
			quiet( rig );
			rig.Set( PT_FLARE, 0.5f );
			rig.Option( PT_FLARE_FROM, FlareFrom::Light );
			rig.Set( PT_LIGHT_X, 0.5f );
			rig.Set( PT_LIGHT_Y, 0.5f );
			rig.Set( PT_GHOSTS, 0.0f );
			rig.Set( PT_HALO, 0.0f );
			rig.Set( PT_STREAK, 0.0f );
			rig.Set( PT_STARBURST, 1.0f );
			rig.Option( PT_SHAPE, c.shape );
			rig.Set( PT_BLADES, ParamFromBlades( c.blades ) );
			rig.Set( PT_BLADE_CURVE, static_cast< float >( c.curve ) );
			rig.Set( PT_IRIS_ROTATE, static_cast< float >( c.rotate / 360.0 ) );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			int bw = 0, bh = 0;
			const Floats bokeh = readBuffer( rig, HyacinthPlugin::TestBuffer::Bokeh, bw, bh );
			const double sx = static_cast< double >( raster.w ) / bw, sy = static_cast< double >( raster.h ) / bh;
			//A ring round the light, past the glow, inside the spikes' reach.
			const double rho = 0.12 * raster.h;
			const int count  = 1440;
			std::vector< double > ring( count );
			double lo = 1e30, hi = 0.0;
			for( int i = 0; i < count; ++i )
			{
				const double a = 2.0 * kPi * i / count;
				ring[ static_cast< size_t >( i ) ] =
					sampleAt( bokeh, bw, bh, ( 0.5 * raster.w + rho * std::cos( a ) ) / sx, ( 0.5 * raster.h + rho * std::sin( a ) ) / sy, 1 );
				lo = std::min( lo, ring[ static_cast< size_t >( i ) ] );
				hi = std::max( hi, ring[ static_cast< size_t >( i ) ] );
			}
			//A spike: one run of the ring above its middle (and above twice
			//the trough), placed at the run's brightest sample. Runs, not local
			//maxima: a spike centred between two texel rows has a flat top two
			//texels wide, and rounding on it makes two maxima of one spike.
			std::vector< double > peaks;
			const double mid = 0.5 * ( lo + hi );
			auto above = [ & ]( int i ) {
				const double v = ring[ static_cast< size_t >( ( i % count + count ) % count ) ];
				return v > mid && v > 2.0 * lo;
			};
			int start = 0;//a sample below, so no run is split by the wrap
			while( start < count && above( start ) )
				++start;
			if( start < count )
				for( int i = start + 1; i <= start + count; ++i )
				{
					if( !above( i ) || above( i - 1 ) )
						continue;
					int best = i, j = i;
					for( ; above( j ); ++j )
						if( ring[ static_cast< size_t >( j % count ) ] > ring[ static_cast< size_t >( best % count ) ] )
							best = j;
					peaks.push_back( 360.0 * ( best % count ) / count );
					i = j;
				}
			const int got = hi > 4.0 * lo + 1e-9 ? static_cast< int >( peaks.size() ) : 0;
			//Every spike along a blade edge's normal, either way.
			const int lines = c.shape == Shape::Star ? 5 : c.blades;
			double worst    = 0.0;
			for( double p : peaks )
			{
				double best = 360.0;
				for( int k = 0; k < lines; ++k )
				{
					const double normal = 90.0 + ( k + 0.5 ) * 360.0 / lines + c.rotate;
					for( double along : { normal, normal + 180.0 } )
					{
						double d = std::fmod( std::fabs( p - along ), 360.0 );
						best     = std::min( best, std::min( d, 360.0 - d ) );
					}
				}
				worst = std::max( worst, best );
			}
			//Bound: the ring is sampled every quarter degree; a spike a texel
			//wide at rho is located to half a texel, 0.5 x 2 px / rho.
			const double bound = 0.25 + ( 0.5 * sx / rho ) / kDegree;
			Check( got == c.want && ( c.want == 0 || worst < bound ),
			       fmt( "%dx%d %s %d blades, curve %.1f, rotate %g: %d spikes (want %d), the worst %.2f deg off an edge's normal "
			            "(bound %.2f)",
			            raster.w, raster.h, InfoOf( PT_SHAPE ).options[ static_cast< int >( c.shape ) ], c.blades, c.curve, c.rotate, got,
			            c.want, worst, bound ) );
		}
	return Verdict();
}

//===========================================================================
// --ghosts: every ghost lies on the line from the light through the
// picture's centre, and moves with the light.
//===========================================================================
int runGhosts( const Perturb& perturb )
{
	std::printf( "\n=== ghosts: the flare's ghosts lie on the line through the light and the picture's centre, wherever the light is\n" );
	for( const Raster& raster : kRasters )
		for( const std::array< double, 2 > light : { std::array< double, 2 > { 0.82, 0.72 }, std::array< double, 2 > { 0.3, 0.2 } } )
		{
			const Floats black = pointPicture( raster.w, raster.h, 0, 0, 0, 0.5 );
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &black ) )
				return 1;
			quiet( rig );
			rig.Set( PT_FLARE, 0.6f );
			rig.Option( PT_FLARE_FROM, FlareFrom::Light );
			rig.Set( PT_LIGHT_X, static_cast< float >( light[ 0 ] ) );
			rig.Set( PT_LIGHT_Y, static_cast< float >( light[ 1 ] ) );
			rig.Set( PT_GHOSTS, 1.0f );
			rig.Set( PT_HALO, 0.0f );
			rig.Set( PT_STREAK, 0.0f );
			rig.Set( PT_STARBURST, 0.0f );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			int bw = 0, bh = 0;
			const Floats bokeh = readBuffer( rig, HyacinthPlugin::TestBuffer::Bokeh, bw, bh );
			const double sx = static_cast< double >( raster.w ) / bw, sy = static_cast< double >( raster.h ) / bh;
			const double Lx = light[ 0 ] * raster.w, Ly = light[ 1 ] * raster.h;
			const double Cx = 0.5 * raster.w, Cy = 0.5 * raster.h;
			//Everything but the light's own glow (a quarter of a tenth of the
			//height, drawn out to four times that).
			const double glow = 0.11 * raster.h;
			double total = 0, mx = 0, my = 0, xx = 0, yy = 0, xy = 0;
			for( int y = 0; y < bh; ++y )
				for( int x = 0; x < bw; ++x )
				{
					const double px = ( x + 0.5 ) * sx, py = ( y + 0.5 ) * sy;
					if( std::hypot( px - Lx, py - Ly ) < glow )
						continue;
					const float* p = &bokeh[ ( static_cast< size_t >( y ) * bw + x ) * 4 ];
					const double v = 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ];
					total += v;
					mx += v * px;
					my += v * py;
					xx += v * px * px;
					yy += v * py * py;
					xy += v * px * py;
				}
			mx /= total;
			my /= total;
			xx = xx / total - mx * mx;
			yy = yy / total - my * my;
			xy = xy / total - mx * my;
			//The principal axis of the ghosts' light.
			const double angle = 0.5 * std::atan2( 2.0 * xy, xx - yy );
			const double ux = std::cos( angle ), uy = std::sin( angle );
			auto offLine = [ & ]( double px, double py ) { return std::fabs( ( px - mx ) * uy - ( py - my ) * ux ); };
			const double axis = std::hypot( Lx - Cx, Ly - Cy );
			const double missC = offLine( Cx, Cy ), missL = offLine( Lx, Ly );
			//Bound: each ghost is a disc on the line, drawn at half resolution;
			//the light-weighted line through them is good to a fraction of a
			//texel, and a texel (2 px) is the bound.
			const double bound = 2.0 * sx;
			Check( missC < bound && missL < bound && total > 0.0,
			       fmt( "%dx%d the light at (%.2f, %.2f): the ghosts' axis passes %.2f px from the centre and %.2f px from the light "
			            "(bound %.1f px; the light is %.0f px from the centre)",
			            raster.w, raster.h, light[ 0 ], light[ 1 ], missC, missL, bound, axis ) );
		}
	return Verdict();
}

//===========================================================================
// --occlusion: a sharp thing in front is solid. The bokeh of a light behind
// it does not show through it, and the blur of what is behind does not
// spread over it.
//===========================================================================
int runOcclusion( const Perturb& perturb )
{
	std::printf( "\n=== occlusion: a sharp foreground hides the bokeh behind it, bit for bit; a blurred background spreads over a nearer, "
	             "less blurred thing no farther than that thing's own blur reaches\n" );
	for( const Raster& raster : kRasters )
	{
		const int edge = raster.w / 2;
		{
			//Left: near, in focus, mid-grey. Right: far, black, with a light 12
			//pixels from the edge whose disc reaches well over it.
			Floats picture( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					float* p = &picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					const bool near = x < edge;
					p[ 0 ] = p[ 1 ] = p[ 2 ] = near ? eightBit( 0.4 ) : 0.0f;
					p[ 3 ] = near ? 0.8f : 0.1f;
				}
			const int bx = edge + 8, by = 8 * ( raster.h / 16 );
			for( int y = by; y < by + 4; ++y )
				for( int x = bx; x < bx + 4; ++x )
					for( int c = 0; c < 3; ++c )
						picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 + c ] = 1.0f;
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &picture ) )
				return 1;
			quiet( rig );
			rig.Option( PT_DEPTH_FROM, DepthFrom::ClipAlpha );
			rig.Option( PT_FOCUS, Focus::Manual );
			rig.Set( PT_FOCUS_DEPTH, 0.8f );
			rig.Set( PT_BLUR, ParamFromBlur( 0.06 ) );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			const double reach = 0.06 * raster.h * 0.7;
			int changed = 0, checked = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < edge - 3; ++x )
				{
					const float* o = pixelAt( out, raster.w, x, y );
					const float* s = pixelAt( picture, raster.w, x, y );
					++checked;
					changed += o[ 0 ] != s[ 0 ] || o[ 1 ] != s[ 1 ] || o[ 2 ] != s[ 2 ];
				}
			Check( changed == 0, fmt( "%dx%d a light %d px behind a sharp edge, its disc %.0f px wide: %d of %d sharp pixels changed (0 wanted)",
			                          raster.w, raster.h, bx - edge, reach, changed, checked ) );
		}
		{
			//Left: a midground a little out of focus. Right: the far
			//background, much more so, and brighter (below Threshold).
			const double nf = 0.8, nMid = 0.7, nFar = 0.0, B = 0.08 * raster.h;
			Floats picture( static_cast< size_t >( raster.w ) * raster.h * 4, 0.0f );
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					float* p = &picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					const bool mid = x < edge;
					p[ 0 ] = p[ 1 ] = p[ 2 ] = mid ? eightBit( 0.25 ) : eightBit( 0.7 );
					p[ 3 ] = static_cast< float >( mid ? nMid : nFar );
				}
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &picture ) )
				return 1;
			quiet( rig );
			rig.Option( PT_DEPTH_FROM, DepthFrom::ClipAlpha );
			rig.Option( PT_FOCUS, Focus::Manual );
			rig.Set( PT_FOCUS_DEPTH, static_cast< float >( nf ) );
			rig.Set( PT_BLUR, 1.0f );
			rig.Set( PT_HIGHLIGHTS, 0.0f );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			const Floats out = rig.Output();
			//How far the background may reach into the midground: twice the
			//midground's own blur and half a texel (the gather's limit), plus
			//the footprint of the mip level its samples are read at and the
			//bilinear tap back to full resolution. Worked out here from the
			//gather's sample spacing.
			const double half = static_cast< double >( raster.w ) / ( ( raster.w + 1 ) / 2 );
			const double cocMid = B * ( nf - nMid ), cocFar = B * ( nf - nFar );
			const double G       = cocFar / half;
			const double spacing = std::sqrt( kPi * G * G / kSpiralCount );
			const double clear   = ( 2.0 * cocMid / half + 0.5 ) * half + spacing * half + 2.0 * half;
			double worst = 0.0;
			int checked  = 0;
			for( int y = raster.h / 4; y < 3 * raster.h / 4; ++y )
				for( int x = 0; x < edge; ++x )
				{
					if( edge - ( x + 0.5 ) < clear || x < cocFar )
						continue;
					++checked;
					worst = std::max( worst, std::fabs( static_cast< double >( pixelAt( out, raster.w, x, y )[ 1 ] ) - eightBit( 0.25 ) ) );
				}
			//Bound: the midground blurred is still the midground's grey, to
			//the output's float arithmetic; an eighth of an 8-bit step.
			Check( checked > 0 && worst < 0.5 / 255.0,
			       fmt( "%dx%d a midground blurred %.1f px against a background blurred %.1f px: past %.1f px from the edge the midground "
			            "is its own grey to %.5f (bound %.5f, %d pixels)",
			            raster.w, raster.h, cocMid, cocFar, clear, worst, 0.5 / 255.0, checked ) );
		}
	}
	return Verdict();
}


//===========================================================================
// --depth: the depth each source gives, read back through Show = Depth.
//===========================================================================

/// Show = Depth's grey, where it is not the focal band's tint.
bool depthAt( const Floats& out, int w, int x, int y, double& n )
{
	const float* p = pixelAt( out, w, x, y );
	if( p[ 0 ] != p[ 1 ] || p[ 1 ] != p[ 2 ] )
		return false;
	n = p[ 0 ];
	return true;
}

int runDepth( const Perturb& perturb )
{
	std::printf( "\n=== depth: a plane recedes to its horizon as a pinhole sees it; Radial falls off from the focus point; Side by Side and "
	             "Over Under read the depth half; Subject and Haze find what is near; the guided filter puts the depth's edges on the "
	             "picture's\n" );
	for( const Raster& raster : kRasters )
	{
		const double aspect = static_cast< double >( raster.w ) / raster.h;
		//Show Depth tints what is in focus, and those pixels are not read; the
		//focus goes where no case's depth is (a case may move it).
		auto render = [ & ]( const Floats& picture, const std::function< void( Rig& ) >& set, Floats& out ) {
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &picture ) )
				return false;
			quiet( rig );
			rig.Option( PT_SHOW, Show::Depth );
			rig.Option( PT_FOCUS, Focus::Manual );
			rig.Set( PT_FOCUS_DEPTH, 0.999f );
			set( rig );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return false;
			out = rig.Output();
			return true;
		};
		const Floats scene = buildScene( raster.w, raster.h );

		//The plane. The harness's own reckoning: the frame's corner nearest
		//along the plane's direction is nearness 1; the horizon, Horizon of
		//the way from the farthest corner to it, is 0; between them a
		//pinhole camera sees 1/distance fall linearly.
		struct Plane
		{
			double degrees, horizon;
			bool invert;
		};
		for( const Plane& plane : { Plane { 270.0, 0.3, false }, Plane { 0.0, 0.5, false }, Plane { 135.0, 0.2, true }, Plane { 40.0, 0.8, false } } )
		{
			Floats out;
			if( !render( scene, [ & ]( Rig& r ) {
				    r.Set( PT_PLANE_ANGLE, static_cast< float >( plane.degrees / 360.0 ) );
				    r.Set( PT_HORIZON, static_cast< float >( plane.horizon ) );
				    r.Set( PT_DEPTH_INVERT, plane.invert ? 1.0f : 0.0f );
				    //Inverted, the sky past the horizon is nearness 1: focus at
				    //the near corner instead, which is 0.
				    if( plane.invert )
					    r.Set( PT_FOCUS_DEPTH, 0.001f );
			    }, out ) )
				return 1;
			const double ex = std::cos( plane.degrees * kDegree ), ey = std::sin( plane.degrees * kDegree );
			double nearX = 0, nearY = 0, farX = 0, farY = 0, best = -1e9, worstC = 1e9;
			for( double cx : { -0.5 * aspect, 0.5 * aspect } )
				for( double cy : { -0.5, 0.5 } )
				{
					const double s = cx * ex + cy * ey;
					if( s > best )
						best = s, nearX = cx, nearY = cy;
					if( s < worstC )
						worstC = s, farX = cx, farY = cy;
				}
			const double hx = farX + plane.horizon * ( nearX - farX ), hy = farY + plane.horizon * ( nearY - farY );
			const double span = ( nearX - hx ) * ex + ( nearY - hy ) * ey;
			double worst = 0.0;
			int band     = 0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double n = 0.0;
					if( !depthAt( out, raster.w, x, y, n ) )
					{
						++band;
						continue;
					}
					const double px = ( ( x + 0.5 ) / raster.w - 0.5 ) * aspect, py = ( y + 0.5 ) / raster.h - 0.5;
					double want = span > 1e-9 ? ( ( px - hx ) * ex + ( py - hy ) * ey ) / span : 0.0;
					want        = std::clamp( want, 0.0, 1.0 );
					if( plane.invert )
						want = 1.0 - want;
					worst = std::max( worst, std::fabs( n - want ) );
				}
			//Bound: float arithmetic on numbers of order 1, a few ulps.
			Check( worst < 2e-6 && band < raster.w * raster.h / 20,
			       fmt( "%dx%d Plane at %g deg, Horizon %.1f%s: worst %.1e from the pinhole's nearness (bound 2e-6); %d pixels in the focus "
			            "tint (under %d)",
			            raster.w, raster.h, plane.degrees, plane.horizon, plane.invert ? ", inverted" : "", worst, band,
			            raster.w * raster.h / 20 ) );
		}

		//Radial: 1 at the focus point, 0 a Radius away.
		{
			const double fx = 0.3, fy = 0.6, radius = 0.8;
			Floats out;
			if( !render( scene, [ & ]( Rig& r ) {
				    r.Option( PT_DEPTH_FROM, DepthFrom::Radial );
				    r.Set( PT_FOCUS_X, static_cast< float >( fx ) );
				    r.Set( PT_FOCUS_Y, static_cast< float >( fy ) );
				    r.Set( PT_RADIUS, ParamFromRadius( radius ) );
			    }, out ) )
				return 1;
			double worst = 0.0;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					double n = 0.0;
					if( !depthAt( out, raster.w, x, y, n ) )
						continue;
					const double d = std::hypot( ( ( x + 0.5 ) / raster.w - fx ) * aspect, ( y + 0.5 ) / raster.h - fy );
					worst          = std::max( worst, std::fabs( n - std::clamp( 1.0 - d / radius, 0.0, 1.0 ) ) );
				}
			Check( worst < 2e-6, fmt( "%dx%d Radial round (0.3, 0.6), Radius 0.8: worst %.1e (bound 2e-6)", raster.w, raster.h, worst ) );
		}

		//Side by Side and Over Under: the depth is the other half's luma,
		//at the same place in it.
		for( DepthFrom packing : { DepthFrom::SideBySide, DepthFrom::OverUnder } )
		{
			const bool side = packing == DepthFrom::SideBySide;
			Floats picture  = buildScene( raster.w, raster.h );
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const bool depthHalf = side ? x >= raster.w / 2 : y < raster.h / 2;
					if( !depthHalf )
						continue;
					const double u = side ? ( x - raster.w / 2 + 0.5 ) / ( raster.w / 2 ) : ( x + 0.5 ) / raster.w;
					const double v = side ? ( y + 0.5 ) / raster.h : ( y + 0.5 ) / ( raster.h / 2 );
					const float g  = eightBit( 0.1 + 0.6 * u + 0.25 * v );
					float* p       = &picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					p[ 0 ] = p[ 1 ] = p[ 2 ] = g;
				}
			Floats out;
			if( !render( picture, [ & ]( Rig& r ) { r.Option( PT_DEPTH_FROM, packing ); }, out ) )
				return 1;
			//The depth half sampled as the host's texture is: bilinear, at the
			//same uv within its half.
			double worst = 0.0;
			for( int y = 2; y < raster.h - 2; ++y )
				for( int x = 2; x < raster.w - 2; ++x )
				{
					double n = 0.0;
					if( !depthAt( out, raster.w, x, y, n ) )
						continue;
					const double u = ( x + 0.5 ) / raster.w, v = ( y + 0.5 ) / raster.h;
					const double tu = side ? 0.5 + 0.5 * u : u, tv = side ? v : 0.5 * v;
					const double want = sampleAt( picture, raster.w, raster.h, tu * raster.w, tv * raster.h, 0 );
					worst             = std::max( worst, std::fabs( n - want ) );
				}
			//Bound: a GPU's bilinear weights are 8-bit fixed point (1/256 of a
			//texel), on a gradient of under 1/256 a texel: 2e-5.
			Check( worst < 2e-5, fmt( "%dx%d %s: the depth is the depth half's luma to %.1e (bound 2e-5)", raster.w, raster.h,
			                          InfoOf( PT_DEPTH_FROM ).options[ static_cast< int >( packing ) ], worst ) );
		}

		//Luma: the estimate is the picture's luma, an exact linear function
		//of its colour, so the colour guided filter -- a least-squares fit
		//of the estimate to the colour in every window, averaged over the
		//windows -- must return it, less only what the ridge shrinks. Along
		//a direction in which a window's colour varies by sigma, the ridge
		//scales the fit by sigma^2 / ( sigma^2 + Epsilon ), which moves a
		//texel c sigma from the window's mean by at most c |w| sqrt( Epsilon )
		//over 2 (w the luma weights, |w| = 0.75), the worst at sigma^2 =
		//Epsilon. The plugin's own coefficients (a.rgb, b), read back, are
		//applied here to each quarter texel's colour -- the 4x4 block's
		//mean, which the harness takes itself -- where an exact fit is exact
		//(every window's model is exact on every texel it holds). At full
		//resolution the coefficients are interpolated across texels, the
		//fast filter's subsampling: a different limit, which AGENTS.md
		//records, and the Subject case below checks the edge it gives.
		//The picture: patches of random colour 0.1 H across.
		{
			Floats patches( static_cast< size_t >( raster.w ) * raster.h * 4 );
			const double side = 0.1 * raster.h;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const uint32_t cx = static_cast< uint32_t >( x / side ), cy = static_cast< uint32_t >( y / side );
					float* p = &patches[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					for( int ch = 0; ch < 3; ++ch )
						p[ ch ] = eightBit( 0.1 + 0.8 * hash01( cx * 3u + static_cast< uint32_t >( ch ), cy + 7919u ) );
					p[ 3 ] = 1.0f;
				}
			Rig rig;
			if( !rig.Init( raster.w, raster.h, &patches ) )
				return 1;
			quiet( rig );
			rig.Option( PT_DEPTH_FROM, DepthFrom::Luma );
			applyPerturb( rig, perturb );
			if( !rig.Render( 2 ) )
				return 1;
			int gw = 0, gh = 0;
			const Floats guide = readBuffer( rig, HyacinthPlugin::TestBuffer::Guide, gw, gh );
			double sum2 = 0.0, worst = 0.0;
			int count   = 0;
			const double weights[ 3 ] = { 0.2126, 0.7152, 0.0722 };//Rec. 709, on code values
			for( int ty = 0; ty < gh && ( ty + 1 ) * 4 <= raster.h; ++ty )
				for( int tx = 0; tx < gw && ( tx + 1 ) * 4 <= raster.w; ++tx )
				{
					double I[ 3 ] = { 0, 0, 0 };
					for( int y = 0; y < 4; ++y )
						for( int x = 0; x < 4; ++x )
							for( int ch = 0; ch < 3; ++ch )
								I[ ch ] += patches[ ( static_cast< size_t >( ty * 4 + y ) * raster.w + tx * 4 + x ) * 4 + ch ] / 16.0;
					const float* ab = &guide[ ( static_cast< size_t >( ty ) * gw + tx ) * 4 ];
					double e        = ab[ 3 ];
					for( int ch = 0; ch < 3; ++ch )
						e += ( ab[ ch ] - weights[ ch ] ) * I[ ch ];
					sum2 += e * e;
					worst = std::max( worst, std::fabs( e ) );
					++count;
				}
			const double rms = std::sqrt( sum2 / std::max( count, 1 ) );
			//Bound: c = 1, RMS: |w| sqrt( 1e-3 ) / 2 = 0.012.
			const double bound = 0.749 * std::sqrt( 1e-3 ) / 2.0;
			Check( rms < bound && count > 0,
			       fmt( "%dx%d Luma: the guided filter's fit, applied to each quarter texel's colour, is the colour's luma to %.4f RMS, worst "
			            "%.3f (bound %.4f RMS: the ridge's most shrinkage at one sigma)",
			            raster.w, raster.h, rms, worst, bound ) );
		}

		//Subject: a red square on a blue ground, the focus point on it. The
		//square is near and the ground far, and the depth's edge is the
		//square's, to the pixel: that is the guided filter's work, since the
		//colours are judged at a quarter of the resolution. The two colours'
		//lumas are close (0.34 and 0.25), so a guide of luma alone leaves
		//the edge soft; the guide has to be the colour.
		{
			const double half = 0.1;
			Floats picture( static_cast< size_t >( raster.w ) * raster.h * 4 );
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const double px = ( ( x + 0.5 ) / raster.w - 0.5 ) * aspect, py = ( y + 0.5 ) / raster.h - 0.5;
					const bool in   = std::fabs( px - 0.0137 ) < half && std::fabs( py ) < half;
					const double grain = 0.03 * ( hash01( static_cast< uint32_t >( x ), static_cast< uint32_t >( y ) ) - 0.5 );
					float* p = &picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					p[ 0 ]   = eightBit( ( in ? 0.9 : 0.1 ) + grain );
					p[ 1 ]   = eightBit( ( in ? 0.2 : 0.25 ) + grain );
					p[ 2 ]   = eightBit( ( in ? 0.1 : 0.7 ) + grain );
					p[ 3 ]   = 1.0f;
				}
			Floats out;
			if( !render( picture, [ & ]( Rig& r ) {
				    r.Option( PT_DEPTH_FROM, DepthFrom::Subject );
				    r.Set( PT_TRACK_SIZE, ParamFromTrackSize( 0.3 ) );
				    r.Set( PT_DEPTH_EDGES, ParamFromDepthEdges( 0.025 ) );
			    }, out ) )
				return 1;
			//Along the middle row, across the square's left edge.
			const int row   = raster.h / 2;
			const double xe = ( 0.5 + ( 0.0137 - half ) / aspect ) * raster.w;
			double inside = 0.0, outside = 0.0;
			depthAt( out, raster.w, static_cast< int >( xe + 0.05 * raster.h ), row, inside );
			depthAt( out, raster.w, static_cast< int >( xe - 0.05 * raster.h ), row, outside );
			double x10 = -1, x90 = -1;
			for( int x = static_cast< int >( xe - 0.05 * raster.h ); x < static_cast< int >( xe + 0.05 * raster.h ); ++x )
			{
				double n = 0.0;
				depthAt( out, raster.w, x, row, n );
				const double f = ( n - outside ) / ( inside - outside );
				if( x10 < 0 && f > 0.1 )
					x10 = x;
				if( x90 < 0 && f > 0.9 )
					x90 = x;
			}
			const double width = x90 - x10;
			//Bound: the picture's edge is one pixel wide, and the coefficients
			//vary over a quarter-resolution texel: two pixels.
			Check( inside - outside > 0.4 && width <= 2.0 && x10 >= 0,
			       fmt( "%dx%d Subject: the square at nearness %.3f, the ground %.3f (0.4 apart at least); the depth's edge rises 10-90%% "
			            "in %.0f px (bound 2), at x %.0f where the square's edge is %.1f",
			            raster.w, raster.h, inside, outside, width, x10, xe ) );
		}

		//Haze: I = J t + A ( 1 - t ), t = exp( -beta d ). The harness puts
		//the scene at distances it chooses, hazes it, and requires the
		//estimate to rank the pixels as the distances do.
		{
			const double A = 0.85, beta = 1.2;
			Floats picture( static_cast< size_t >( raster.w ) * raster.h * 4 );
			std::vector< double > truth( static_cast< size_t >( raster.w ) * raster.h );
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const double u = ( x + 0.5 ) / raster.w, v = ( y + 0.5 ) / raster.h;
					//Farther up the frame and to the right, in steps.
					const double d = 0.2 + 2.2 * v + 0.6 * u;
					const double t = std::exp( -beta * d );
					//Saturated colours, so every patch has a dark channel.
					const uint32_t cell = static_cast< uint32_t >( x / 6 ) * 7919u + static_cast< uint32_t >( y / 6 );
					double J[ 3 ] = { hash01( cell, 1 ), hash01( cell, 2 ), hash01( cell, 3 ) };
					J[ cell % 3 ] *= 0.05;
					float* p = &picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					for( int c = 0; c < 3; ++c )
						p[ c ] = eightBit( J[ c ] * t + A * ( 1.0 - t ) );
					p[ 3 ]                                               = 1.0f;
					truth[ static_cast< size_t >( y ) * raster.w + x ] = 1.0 / d;
				}
			Floats out;
			if( !render( picture, [ & ]( Rig& r ) { r.Option( PT_DEPTH_FROM, DepthFrom::Haze ); }, out ) )
				return 1;
			//Spearman's rank correlation, on a grid of samples.
			std::vector< std::pair< double, double > > pairs;
			for( int y = 8; y < raster.h - 8; y += 4 )
				for( int x = 8; x < raster.w - 8; x += 4 )
				{
					double n = 0.0;
					if( depthAt( out, raster.w, x, y, n ) )
						pairs.emplace_back( truth[ static_cast< size_t >( y ) * raster.w + x ], n );
				}
			auto ranks = [ & ]( bool second ) {
				std::vector< size_t > order( pairs.size() );
				for( size_t i = 0; i < order.size(); ++i )
					order[ i ] = i;
				std::sort( order.begin(), order.end(), [ & ]( size_t a, size_t b ) {
					return second ? pairs[ a ].second < pairs[ b ].second : pairs[ a ].first < pairs[ b ].first;
				} );
				std::vector< double > r( pairs.size() );
				for( size_t i = 0; i < order.size(); ++i )
					r[ order[ i ] ] = static_cast< double >( i );
				return r;
			};
			const auto ra = ranks( false ), rb = ranks( true );
			double d2 = 0.0;
			for( size_t i = 0; i < ra.size(); ++i )
				d2 += ( ra[ i ] - rb[ i ] ) * ( ra[ i ] - rb[ i ] );
			const double n   = static_cast< double >( ra.size() );
			const double rho = 1.0 - 6.0 * d2 / ( n * ( n * n - 1.0 ) );
			Check( rho > 0.9, fmt( "%dx%d Haze over a scene 0.2 to 3 units deep: the estimate ranks it as the distances do, Spearman %.3f "
			                       "(0.9 at least)",
			                       raster.w, raster.h, rho ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --focus: the focus pulls as a first-order lag: after a jump in the
// target, the rest of the way closes by exp( -t / tau ).
//===========================================================================
int runFocus( const Perturb& perturb )
{
	std::printf( "\n=== focus: a pull to a new focus closes as exp( -t / tau ), frame by frame; Point focuses on the depth at the point\n" );
	const Raster raster = kRasters.back();
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		quiet( rig );
		const double tau = 0.3;
		rig.Option( PT_FOCUS, Focus::Manual );
		rig.Set( PT_FOCUS_SPEED, ParamFromFocusTau( tau ) );
		rig.Set( PT_FOCUS_DEPTH, 0.2f );
		applyPerturb( rig, perturb );
		if( !rig.Render( 10 ) )
			return 1;
		const double first = rig.plugin.ReadFocusForTest()[ 0 ];
		rig.Set( PT_FOCUS_DEPTH, 0.8f );
		double worst = 0.0;
		for( int k = 1; k <= 60; ++k )
		{
			if( !rig.Render( 1 ) )
				return 1;
			const double got  = rig.plugin.ReadFocusForTest()[ 0 ];
			const double want = 0.8 - 0.6 * std::exp( -k / 60.0 / tau );
			worst             = std::max( worst, std::fabs( got - want ) );
		}
		//Bound: sixty steps of a float multiply-add, each good to an ulp.
		Check( std::fabs( first - 0.2f ) < 1e-6 && worst < 1e-5,
		       fmt( "Manual 0.2 then 0.8, tau %.1f s at 60 fps: held at %.6f, then the worst frame %.1e from 0.8 - 0.6 exp( -t / tau ) "
		            "(bound 1e-5)",
		            tau, first, worst ) );
	}
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		quiet( rig );
		rig.Option( PT_FOCUS, Focus::Point );
		rig.Set( PT_FOCUS_X, 0.62f );
		rig.Set( PT_FOCUS_Y, 0.21f );
		applyPerturb( rig, perturb );
		if( !rig.Render( 2 ) )
			return 1;
		//The default plane: nearer at the bottom, the horizon 30% down.
		const double want = std::clamp( ( 0.7 - 0.21 ) / 0.7, 0.0, 1.0 );
		const auto got    = rig.plugin.ReadFocusForTest();
		Check( std::fabs( got[ 0 ] - want ) < 1e-5 && std::fabs( got[ 1 ] - want ) < 1e-5,
		       fmt( "Point at (0.62, 0.21) on the default plane: focused at %.6f, target %.6f, the plane's nearness there %.6f (bound "
		            "1e-5)",
		            got[ 0 ], got[ 1 ], want ) );
	}
	return Verdict();
}

//===========================================================================
// --track: the tracker follows a textured subject moving by fractions of a
// pixel, to a fraction of its own search step.
//===========================================================================
int runTrack( const Perturb& perturb )
{
	std::printf( "\n=== track: grabbed on a textured subject moving (1.37, -0.61) px a frame (at 720), the track stays on it to a sixth "
	             "of the fine search's step, frame after frame\n" );
	for( const Raster& raster : kRasters )
	{
		const double scale = raster.h / 720.0;
		const double vx = 1.37 * scale, vy = -0.61 * scale;
		const double side = 0.2 * raster.h;
		const double x0 = 0.4 * raster.w, y0 = 0.55 * raster.h;
		//Value noise, sampled bilinearly at the sub-pixel position: the
		//subject really is between pixels.
		auto noise = [ & ]( uint32_t seed, double x, double y, double cell ) {
			x /= cell;
			y /= cell;
			const double fx = x - std::floor( x ), fy = y - std::floor( y );
			const auto ix = static_cast< int32_t >( std::floor( x ) ), iy = static_cast< int32_t >( std::floor( y ) );
			auto h = [ & ]( int32_t a, int32_t b ) { return hash01( seed ^ static_cast< uint32_t >( a * 73856093 ), static_cast< uint32_t >( b ) ); };
			const double sx = fx * fx * ( 3 - 2 * fx ), sy = fy * fy * ( 3 - 2 * fy );
			return ( h( ix, iy ) * ( 1 - sx ) + h( ix + 1, iy ) * sx ) * ( 1 - sy ) + ( h( ix, iy + 1 ) * ( 1 - sx ) + h( ix + 1, iy + 1 ) * sx ) * sy;
		};
		auto frameAt = [ & ]( int f ) {
			Floats picture( static_cast< size_t >( raster.w ) * raster.h * 4 );
			const double cx = x0 + vx * f, cy = y0 + vy * f;
			for( int y = 0; y < raster.h; ++y )
				for( int x = 0; x < raster.w; ++x )
				{
					const double px = x + 0.5, py = y + 0.5;
					const double dx = px - cx, dy = py - cy;
					const bool in = std::fabs( dx ) < 0.5 * side && std::fabs( dy ) < 0.5 * side;
					const double cell = in ? 0.035 * raster.h : 0.06 * raster.h;
					const double g = in ? 0.25 + 0.6 * noise( 11u, dx + 1000.0, dy + 1000.0, cell ) : 0.3 + 0.3 * noise( 23u, px, py, cell );
					float* p = &picture[ ( static_cast< size_t >( y ) * raster.w + x ) * 4 ];
					p[ 0 ]   = eightBit( in ? g : 0.6 * g );
					p[ 1 ]   = eightBit( in ? 0.7 * g : g );
					p[ 2 ]   = eightBit( in ? 0.4 * g : 0.9 * g );
					p[ 3 ]   = 1.0f;
				}
			return picture;
		};
		const Floats start = frameAt( 0 );
		Rig rig;
		if( !rig.Init( raster.w, raster.h, &start ) )
			return 1;
		quiet( rig );
		rig.Option( PT_FOCUS, Focus::Track );
		rig.Set( PT_FOCUS_X, static_cast< float >( x0 / raster.w ) );
		rig.Set( PT_FOCUS_Y, static_cast< float >( y0 / raster.h ) );
		const double trackSize = 0.15;
		rig.Set( PT_TRACK_SIZE, ParamFromTrackSize( trackSize ) );
		applyPerturb( rig, perturb );
		rig.beforeFrame = [ & ]( int f ) { rig.Upload( frameAt( f ) ); };
		double sum2 = 0.0, worst = 0.0, lowest = 1.0;
		const int frames = 60;
		for( int f = 0; f < frames; ++f )
		{
			if( !rig.Render( 1 ) )
				return 1;
			if( f == 0 )
				continue;
			const auto t = rig.plugin.ReadTrackForTest();
			const double ex = t.x * raster.w - ( x0 + vx * f ), ey = t.y * raster.h - ( y0 + vy * f );
			sum2 += ex * ex + ey * ey;
			worst  = std::max( worst, std::hypot( ex, ey ) );
			lowest = std::min( lowest, static_cast< double >( t.confidence ) );
		}
		const double rms = std::sqrt( sum2 / ( frames - 1 ) );
		//The fine search's step, pixels: half a template texel -- or half an
		//analysis texel (4 px), where that is the coarser. The tracker sees
		//the picture through the quarter-resolution analysis buffer, and a
		//template finer than it only resamples it: at 180p a template texel
		//is 1.1 px, and the peak is smooth on the analysis texel's scale,
		//not on the step's.
		const double templatePx = trackSize * raster.h / shaders::kTemplateSide;
		const double step       = 0.5 * std::max( templatePx, 4.0 );
		//Bound: a parabola through a correlation peak sampled at that step is
		//biased by under a tenth of it (Shimizu and Okutomi, 2005, for a
		//peak smooth on that scale); without it, the error is uniform over
		//the step, sqrt( 1/12 ) = 0.29 of it, RMS.
		Check( rms < step / 6.0 && worst < 0.5 * step && rig.plugin.Regrabs() == 1 && lowest > 0.7,
		       fmt( "%dx%d %d frames: RMS error %.3f px, worst %.3f px (bounds %.3f, %.3f: a sixth and half of the %.2f px step); grabbed "
		            "%d time(s); the lowest confidence %.3f",
		            raster.w, raster.h, frames - 1, rms, worst, step / 6.0, 0.5 * step, step, rig.plugin.Regrabs(), lowest ) );
	}
	return Verdict();
}

//===========================================================================
// --clock: the focus pull and the orbs follow the clock in seconds, and a
// host whose clock has run to 499,217,238 ms renders the same take as one
// that started at 0 s.
//===========================================================================
int runClock( const Perturb& perturb )
{
	std::printf( "\n=== clock: the focus pull and the orbs run in seconds; a host whose clock has run to 499,217,238 ms renders the same "
	             "take as one that started at 0 s\n" );
	const Raster raster = kRasters.back();
	auto take = [ & ]( bool late, Floats& out, double& focus ) {
		Rig rig;
		if( late )
		{
			rig.timeOrigin = 499217238.0;
			rig.timeUnit   = 1000.0;
			rig.clockScale = perturb.clockAsSeconds ? 1.0 : 0.001;
		}
		if( !rig.Init( raster.w, raster.h ) )
			return false;
		rig.Set( PT_ORBS, 0.8f );
		rig.Set( PT_ORB_DRIFT, 1.0f );
		rig.Option( PT_FOCUS, Focus::Manual );
		rig.Set( PT_FOCUS_SPEED, ParamFromFocusTau( 0.5 ) );
		rig.Set( PT_FOCUS_DEPTH, 0.1f );
		applyPerturb( rig, perturb );
		if( !rig.Render( 20 ) )
			return false;
		rig.Set( PT_FOCUS_DEPTH, 0.9f );
		if( !rig.Render( 110 ) )
			return false;
		out   = rig.Output();
		focus = rig.plugin.ReadFocusForTest()[ 0 ];
		return true;
	};
	Floats early, late;
	double fe = 0.0, fl = 0.0;
	if( !take( false, early, fe ) || !take( true, late, fl ) )
		return 1;
	double worst = 0.0;
	for( size_t i = 0; i < early.size(); ++i )
		worst = std::max( worst, static_cast< double >( std::fabs( early[ i ] - late[ i ] ) ) );
	const double want = 0.9 - 0.8 * std::exp( -110.0 / 60.0 / 0.5 );
	Check( worst <= 1e-5 && std::fabs( fe - want ) < 1e-5,
	       fmt( "130 frames, the focus pulled at frame 20 with tau 0.5 s, 120 orbs drifting: focused at %.6f (want %.6f); the late "
	            "host's take differs by %.1e (bound 1e-5)",
	            fe, want, worst ) );
	return Verdict();
}


//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
	};
	const Case cases[] = {
		{ "defaults (Plane, Point)", []( Rig& ) {} },
		{ "Blur Size 1, Subject, Track", []( Rig& r ) {
			 r.Set( PT_BLUR, 1.0f );
			 r.Option( PT_DEPTH_FROM, DepthFrom::Subject );
			 r.Option( PT_FOCUS, Focus::Track );
		 } },
		{ "Haze, flare, dirt, 400 orbs", []( Rig& r ) {
			 r.Option( PT_DEPTH_FROM, DepthFrom::Haze );
			 r.Set( PT_FLARE, 0.8f );
			 r.Set( PT_DIRT, 0.6f );
			 r.Set( PT_ORBS, 0.8f );
			 r.Set( PT_ORB_COUNT, 1.0f );
		 } },
		{ "Bokeh Only, Blur Size 1", []( Rig& r ) {
			 r.Option( PT_MODE, Mode::BokehOnly );
			 r.Set( PT_BLUR, 1.0f );
		 } },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 60 frames of warm-up, median and worst of 60, on the street\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 60 ) )
				return 1;
			constexpr int kTimed = 60;
			std::vector< double > times, cpu;
			for( int f = 0; f < kTimed; ++f )
			{
				const auto start = std::chrono::steady_clock::now();
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
				cpu.push_back( std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
			std::sort( times.begin(), times.end() );
			std::sort( cpu.begin(), cpu.end() );
			std::printf( "  %-32s %-6s GPU median %6.2f ms, worst %6.2f  (%4.1f%% of a 60 fps frame); wall %5.2f ms\n", c.what, size.name,
			             times[ kTimed / 2 ], times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ), cpu[ kTimed / 2 ] );
		}
	glDeleteQueries( 1, &query );
	return 0;
}


struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "passthrough", runPassthrough, false },
		{ "coc", runCoc, false },
		{ "energy", runEnergy, false },
		{ "shape", runShape, false },
		{ "fringe", runFringe, false },
		{ "starburst", runStarburst, false },
		{ "ghosts", runGhosts, false },
		{ "occlusion", runOcclusion, false },
		{ "depth", runDepth, false },
		{ "focus", runFocus, false },
		{ "track", runTrack, false },
		{ "clock", runClock, false },
		{ "state", runState, false },
		{ "cues", runCues, true },
		{ "names", runNames, true },
		{ "controls", runControls, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "passthrough", runPassthrough, "every pixel through linear light and back", []( Perturb& p ) { p.hooks = kHookRoundTrip; } );
	add( "coc", runCoc, "the circle of confusion linear in distance, not nearness", []( Perturb& p ) { p.hooks = kHookLinearDepth; } );
	add( "energy", runEnergy, "a disc as bright as its point, whatever its size", []( Perturb& p ) { p.hooks = kHookNoEnergy; } );
	add( "energy", runEnergy, "the barrel's shadow brightens what it leaves", []( Perturb& p ) { p.hooks = kHookCatEyeKeeps; } );
	add( "shape", runShape, "the aperture the same way up in front of focus", []( Perturb& p ) { p.hooks = kHookNoFlip; } );
	add( "fringe", runFringe, "green focused farther than red and blue", []( Perturb& p ) { p.hooks = kHookFringeSwap; } );
	add( "starburst", runStarburst, "one spike per blade edge, not a line through the light", []( Perturb& p ) { p.hooks = kHookSpikePerEdge; } );
	add( "ghosts", runGhosts, "the ghosts pushed off the axis", []( Perturb& p ) { p.hooks = kHookGhostOffAxis; } );
	add( "occlusion", runOcclusion, "a sharp foreground does not hide the bokeh behind it", []( Perturb& p ) { p.hooks = kHookNoOcclusion; } );
	add( "occlusion", runOcclusion, "the background's blur spreads as far as its own", []( Perturb& p ) { p.hooks = kHookBackSpread; } );
	add( "depth", runDepth, "Side by Side's depth read from the picture's half", []( Perturb& p ) { p.hooks = kHookDepthNoRect; } );
	add( "depth", runDepth, "the estimated depth used raw, not guided by the picture", []( Perturb& p ) { p.hooks = kHookNoGuide; } );
	add( "focus", runFocus, "the focus pulled at a constant rate", []( Perturb& p ) { p.hooks = kHookFocusLinear; } );
	add( "track", runTrack, "the peak taken at the best candidate, no parabola", []( Perturb& p ) { p.hooks = kHookNoSubTexel; } );
	add( "clock", runClock, "a millisecond host taken for a seconds one", []( Perturb& p ) { p.clockAsSeconds = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/hyacinth.png";
	std::vector< std::string > settings;
	int width = 1280, height = 720, frames = 30;
	double fps = 60.0;
	std::string mode, scriptPath;
	int filmFrames = -1;
	bool sizeGiven = false, still = false;
	std::map< int, std::vector< std::string > > later;//--at FRAME "Name=V"

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "hytest -- render Hyacinth offline and measure its lens\n\n"
			             "  --out PATH        render the street and write a PNG (default /tmp/hyacinth.png)\n"
			             "  --frozen          (with --out) the ball does not roll\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --frames N        frames before reading back (default 30)\n"
			             "  --fps N           the synthetic clock's rate (default 60)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name). Repeatable.\n"
			             "  --at N \"Name=V\"  (with --out) set it at frame N instead; an event is released the frame after\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the street, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: see tools/verify.sh;  --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--frozen" )
			still = true;
		else if( argument == "--at" && i + 2 < argc )
		{
			const int at = std::atoi( argv[ ++i ] );
			later[ at ].push_back( argv[ ++i ] );
		}
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || !( fps > 0.0 ) )
	{
		std::fprintf( stderr, "--size and --fps must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		HyacinthPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, fps, 0.0, scriptPath, 0, true, settings );
	else if( mode == "film" )
		result = runPipe( width, height, fps, 0.0, scriptPath, filmFrames, false, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		rig.fps = fps;
		if( !rig.Init( width, height ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( !still )
				rig.beforeFrame = [ & ]( int f ) { rig.Upload( buildScene( width, height, rig.Seconds( f ) ) ); };
			bool rendered = true;
			std::vector< std::pair< int, std::string > > release;
			for( int f = 0; f < std::max( frames, 1 ) && rendered; ++f )
			{
				for( const auto& r : release )
					if( r.first == f )
					{
						std::string ignored;
						applySetting( rig.plugin, r.second + "=0", ignored );
					}
				const auto due = later.find( f );
				if( due != later.end() )
					for( const std::string& setting : due->second )
					{
						std::string error;
						if( !applySetting( rig.plugin, setting, error ) )
						{
							std::fprintf( stderr, "--at %d %s: %s\n", f, setting.c_str(), error.c_str() );
							return 2;
						}
						//An event is a press: released on the next frame, as a host would.
						const std::string name = setting.substr( 0, setting.find( '=' ) );
						for( const NamedParameter& parameter : listParameters( rig.plugin ) )
							if( parameter.name == name && parameter.type == FF_TYPE_EVENT )
								release.emplace_back( f + 1, name );
					}
				rendered = rig.Render( 1 );
			}
			if( !rendered )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
			{
				const auto focus = rig.plugin.ReadFocusForTest();
				std::printf( "wrote %s -- %dx%d, %d frames at %g fps (%.2f s), focused at nearness %.3f\n", outPath.c_str(), width,
				             height, frames, fps, frames / fps, focus[ 0 ] );
			}
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
