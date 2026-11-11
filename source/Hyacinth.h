#pragma once

#include "Controls.h"
#include "Iris.h"
#include "PassBuffer.h"
#include "Shaders.h"

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace hyacinth
{
/**
    The plugin.

    A thin lens, focused at one distance, in front of the clip. Every pixel
    gets a depth (from a plane, a radius, the clip's own luma, haze or
    colours, its alpha, or a depth half beside or under it), the lens a
    focus distance (set, read at a point, or read off whatever the tracker is
    following), and from those two each pixel's circle of confusion. The
    picture is then defocused by it, and its highlights drawn as the
    aperture they would really show as. Lens flare, orbs and lens dirt are
    the same aperture again. Shaders.h has the passes; AGENTS.md has why.
*/
class HyacinthPlugin : public CFFGLPlugin
{
public:
	HyacinthPlugin();

	FFResult InitGL( const FFGLViewportStruct* viewport ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* input ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	FFResult SetTime( double time ) override;

	//-------------------------------------------------------------------
	// For the harness. Nothing in the plugin's own operation calls these;
	// they exist so `hytest` can measure what the shipping passes produced,
	// and show that its checks fail against a wrong model.
	//-------------------------------------------------------------------

	/// The offline harness DECLARES its clock unit (seconds) rather than
	/// letting the plugin vote on it against the wall clock -- a harness
	/// renders hundreds of frames a second, which the vote would read as
	/// milliseconds.
	void SetClockScaleForTest( double scale )
	{
		clockScale = scale;
	}
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}

	struct TrackReadout
	{
		float x = 0, y = 0;  ///< uv
		float vx = 0, vy = 0;///< uv a frame
		float confidence = 0;
		float frames     = 0;
	};
	/// The tracker's state after the last frame. Needs the GL context.
	TrackReadout ReadTrackForTest();

	/// The focus nearness after the last frame, and its target.
	std::array< float, 2 > ReadFocusForTest();

	/// The buffers a check may read back.
	enum class TestBuffer
	{
		Prefilter,///< half: linear light, signed CoC (full pixels) in alpha
		Gather,
		Bokeh,    ///< half: the sprites and the flare, linear light
		Highlights,
		Guide,
		Lights
	};
	/// RGBA32F, row 0 at the bottom. Needs the GL context.
	bool ReadBufferForTest( TestBuffer which, std::vector< float >& out, int& width, int& height );

	/// Times the tracker has been grabbed afresh.
	int Regrabs() const
	{
		return regrabs;
	}
	/// Whether the tracker ran in the last frame.
	bool TrackerRan() const
	{
		return trackerRan;
	}
	/// The highlights' mip level the sprites were drawn from last frame.
	int SpriteLevel() const
	{
		return spriteLevel;
	}
	/// Times the aperture's area has been integrated.
	int AreaBuilds() const
	{
		return areaBuilds;
	}

private:
	void UpdateClock();
	bool ensureBuffers( int width, int height );
	void use( shaders::Pass pass );
	void into( PassBuffer& target );
	void drawInstanced( GLenum mode, int vertices, int instances );

	//-------------------------------------------------------------------
	// Uniforms on the program in use, by name. A frame sets several hundred,
	// so each program keeps its locations: the names are literals, and a
	// pointer is a good enough key.
	//-------------------------------------------------------------------
	GLint at( const char* name );
	void set( const char* name, int v );
	void set( const char* name, float v );
	void set( const char* name, float x, float y );
	void set( const char* name, float x, float y, float z );
	void set( const char* name, float x, float y, float z, float w );
	void seti( const char* name, int x, int y );

	float params[ PT_COUNT ] = {};

	std::array< ffglex::FFGLShader, static_cast< size_t >( shaders::Pass::Count ) > programs;
	ffglex::FFGLScreenQuad quad;
	GLuint emptyVao = 0;//the sprites, flare and histogram draw from gl_VertexID alone
	std::array< std::unordered_map< const char*, GLint >, static_cast< size_t >( shaders::Pass::Count ) > locations;
	int current = -1;

	//-------------------------------------------------------------------
	// Buffers. All of them cover the whole picture, whatever their size.
	//-------------------------------------------------------------------
	PassBuffer analysis;  ///< quarter, mipmapped: code RGB, luma
	PassBuffer darkA, darkB, reduceA, airlight;
	PassBuffer histogram; ///< 513 x 2
	PassBuffer raw;       ///< quarter: the depth estimate before the filter
	PassBuffer guideScratch, guideMeans[ 4 ], guideCoef, guide;
	PassBuffer templates[ 2 ];
	PassBuffer coarseScores, coarseBest, fineScores;
	PassBuffer trackState[ 2 ];///< 2 x 1, ping-pong
	PassBuffer focusState[ 2 ];///< 1 x 1, ping-pong
	PassBuffer prefilter;      ///< half, mipmapped
	PassBuffer highlights;     ///< quarter, mipmapped
	PassBuffer tiles, dilated;
	PassBuffer gather;         ///< half
	PassBuffer bokeh;          ///< half, additive
	PassBuffer lights;         ///< 16 x 18
	int templateCur = 0;
	int stateCur    = 0;
	int focusCur    = 0;

	//-------------------------------------------------------------------
	// The tracker and the focus, between frames.
	//-------------------------------------------------------------------
	bool firstFrame   = true;
	bool trackerRan   = false;
	bool wasTracking  = false;
	bool lastGrab     = false;
	float lastFocusX  = -1.0f;
	float lastFocusY  = -1.0f;
	int regrabs       = 0;
	int spriteLevel   = 0;

	//-------------------------------------------------------------------
	// The aperture's area, integrated when its shape changes.
	//-------------------------------------------------------------------
	IrisDesc areaDesc;
	IrisArea area;
	bool areaValid = false;
	int areaBuilds = 0;
	std::array< float, 2 * kSpiralCount > spiral {};

	//-------------------------------------------------------------------
	// Time. See rosette: the host's clock unit is voted on against the wall
	// clock, because Resolume has sent both seconds and milliseconds. The
	// orbs' phases are kept in double and reduced before they become floats.
	//-------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double clockScale   = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;
	double now          = 0.0;
	double lastNow      = -1.0;
	bool settledJump    = false;
	double driftPhase   = 0.0;
	double twinklePhase = 0.0;

	int hooks = 0;
};

} // namespace hyacinth
