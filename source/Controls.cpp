#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace hyacinth
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
float inverseLinear( double value, double most )
{
	return static_cast< float >( std::clamp( value / most, 0.0, 1.0 ) );
}

constexpr double kBlurMost       = 0.08;
constexpr double kRadiusLow      = 0.1, kRadiusHigh = 3.0;
constexpr double kEdgesLow       = 0.005, kEdgesHigh = 0.08;
constexpr double kTrackLow       = 0.04, kTrackHigh = 0.4;
constexpr double kFocusTauMost   = 2.0;
constexpr int kBladesLow         = 3, kBladesHigh = 12;
constexpr double kSqueezeMost    = 2.0;
constexpr double kFringeMost     = 0.06;
constexpr double kHighlightsMost = 32.0;
constexpr int kOrbCountMost      = 400;
constexpr double kOrbLow         = 0.01, kOrbHigh = 0.15;
constexpr double kOrbDriftMost   = 0.2;

const char* const kModeNames[]  = { "Depth of Field", "Bokeh Only" };
const char* const kDepthNames[] = { "Plane", "Radial", "Luma", "Haze", "Subject", "Clip Alpha", "Side by Side", "Over Under" };
const char* const kFocusNames[] = { "Manual", "Point", "Track" };
const char* const kShapeNames[] = { "Iris", "Mirror", "Star", "Heart" };
const char* const kFlareNames[] = { "Highlights", "Light", "Tracked" };
const char* const kShowNames[]  = { "Picture", "Depth", "Blur", "Tracker" };

ControlInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, nullptr, 0 };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count };
}
ControlInfo of( unsigned int id, const char* name, const char* group, Kind kind, float value )
{
	return { id, name, group, kind, value, nullptr, 0 };
}

//---------------------------------------------------------------------------
// The defaults: a tilt-shift look that works on most footage before anything
// is set. The ground recedes from the bottom of the frame to a horizon a
// third of the way down, the focus is on whatever is just below the middle,
// and the iris is a seven-bladed one stopped down a little. AGENTS.md has
// why each number is what it is.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_MODE, "Mode", "Lens", 0.0f, kModeNames, static_cast< int >( Mode::Count ) ),
	standard( PT_BLUR, "Blur Size", "Lens", ParamFromBlur( 0.03 ) ),

	option( PT_DEPTH_FROM, "Depth From", "Depth", 0.0f, kDepthNames, static_cast< int >( DepthFrom::Count ) ),
	standard( PT_PLANE_ANGLE, "Plane Angle", "Depth", ParamFromPlaneAngle( 270.0 * kDegree ) ),
	standard( PT_HORIZON, "Horizon", "Depth", 0.3f ),
	standard( PT_RADIUS, "Radius", "Depth", ParamFromRadius( 0.8 ) ),
	standard( PT_DEPTH_EDGES, "Depth Edges", "Depth", ParamFromDepthEdges( 0.025 ) ),
	of( PT_DEPTH_INVERT, "Depth Invert", "Depth", Kind::Boolean, 0.0f ),

	option( PT_FOCUS, "Focus", "Focus", 1.0f, kFocusNames, static_cast< int >( Focus::Count ) ),
	standard( PT_FOCUS_DEPTH, "Focus Depth", "Focus", 0.5f ),
	of( PT_FOCUS_X, "Focus X", "Focus", Kind::XPos, 0.5f ),
	of( PT_FOCUS_Y, "Focus Y", "Focus", Kind::YPos, 0.4f ),
	of( PT_GRAB, "Grab", "Focus", Kind::Event, 0.0f ),
	standard( PT_TRACK_SIZE, "Track Size", "Focus", ParamFromTrackSize( 0.15 ) ),
	standard( PT_FOCUS_SPEED, "Focus Speed", "Focus", ParamFromFocusTau( 0.2 ) ),

	option( PT_SHAPE, "Shape", "Aperture", 0.0f, kShapeNames, static_cast< int >( Shape::Count ) ),
	standard( PT_BLADES, "Blades", "Aperture", ParamFromBlades( 7 ) ),
	standard( PT_BLADE_CURVE, "Blade Curve", "Aperture", 0.25f ),
	standard( PT_IRIS_ROTATE, "Iris Rotate", "Aperture", 0.0f ),
	standard( PT_CAT_EYE, "Cat Eye", "Aperture", 0.35f ),
	standard( PT_SWIRL, "Swirl", "Aperture", 0.0f ),
	standard( PT_SQUEEZE, "Squeeze", "Aperture", ParamFromSqueeze( 1.0 ) ),
	standard( PT_BUBBLE, "Bubble", "Aperture", ParamFromBubble( 0.25 ) ),
	standard( PT_ONION, "Onion Rings", "Aperture", 0.0f ),
	standard( PT_FRINGE, "Fringe", "Aperture", ParamFromFringe( 0.015 ) ),

	standard( PT_HIGHLIGHTS, "Highlights", "Highlights", ParamFromHighlightGain( 8.0 ) ),
	standard( PT_THRESHOLD, "Threshold", "Highlights", 0.75f ),

	standard( PT_ORBS, "Orbs", "Orbs", 0.0f ),
	standard( PT_ORB_COUNT, "Orb Count", "Orbs", 0.3f ),
	standard( PT_ORB_SIZE, "Orb Size", "Orbs", ParamFromOrbSize( 0.04 ) ),
	standard( PT_ORB_DRIFT, "Orb Drift", "Orbs", 0.25f ),

	standard( PT_FLARE, "Flare", "Flare", 0.0f ),
	option( PT_FLARE_FROM, "Flare From", "Flare", 0.0f, kFlareNames, static_cast< int >( FlareFrom::Count ) ),
	of( PT_LIGHT_X, "Light X", "Flare", Kind::XPos, 0.75f ),
	of( PT_LIGHT_Y, "Light Y", "Flare", Kind::YPos, 0.75f ),
	standard( PT_GHOSTS, "Ghosts", "Flare", 0.5f ),
	standard( PT_HALO, "Halo", "Flare", 0.3f ),
	standard( PT_STARBURST, "Starburst", "Flare", 0.5f ),
	standard( PT_STREAK, "Streak", "Flare", 0.0f ),
	standard( PT_DIRT, "Dirt", "Flare", 0.0f ),

	option( PT_SHOW, "Show", "Output", 0.0f, kShowNames, static_cast< int >( Show::Count ) ),
	standard( PT_MIX, "Mix", "Output", 1.0f ),
};

static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in kTable" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	if( !std::isfinite( value ) )
		return 0;
	const long index = std::lround( value );
	return static_cast< int >( std::clamp< long >( index, 0, count - 1 ) );
}

double BlurFromParam( float value )
{
	return clamp01( value ) * kBlurMost;
}
float ParamFromBlur( double heights )
{
	return inverseLinear( heights, kBlurMost );
}

double PlaneAngleFromParam( float value )
{
	return clamp01( value ) * 2.0 * kPi;
}
float ParamFromPlaneAngle( double radians )
{
	return inverseLinear( radians, 2.0 * kPi );
}

double HorizonFromParam( float value )
{
	return clamp01( value );
}

double RadiusFromParam( float value )
{
	return geometric( value, kRadiusLow, kRadiusHigh );
}
float ParamFromRadius( double heights )
{
	return inverseGeometric( heights, kRadiusLow, kRadiusHigh );
}

double DepthEdgesFromParam( float value )
{
	return geometric( value, kEdgesLow, kEdgesHigh );
}
float ParamFromDepthEdges( double heights )
{
	return inverseGeometric( heights, kEdgesLow, kEdgesHigh );
}

double TrackSizeFromParam( float value )
{
	return geometric( value, kTrackLow, kTrackHigh );
}
float ParamFromTrackSize( double heights )
{
	return inverseGeometric( heights, kTrackLow, kTrackHigh );
}

double FocusTauFromParam( float value )
{
	const double v = clamp01( value );
	return kFocusTauMost * v * v;
}
float ParamFromFocusTau( double seconds )
{
	return static_cast< float >( std::sqrt( std::clamp( seconds / kFocusTauMost, 0.0, 1.0 ) ) );
}

int BladesFromParam( float value )
{
	return kBladesLow + static_cast< int >( std::lround( clamp01( value ) * ( kBladesHigh - kBladesLow ) ) );
}
float ParamFromBlades( int blades )
{
	return static_cast< float >( std::clamp( blades - kBladesLow, 0, kBladesHigh - kBladesLow ) )
	       / static_cast< float >( kBladesHigh - kBladesLow );
}

double IrisRotateFromParam( float value )
{
	return clamp01( value ) * 2.0 * kPi;
}

double CatEyeFromParam( float value )
{
	return clamp01( value );
}

double SwirlFromParam( float value )
{
	return clamp01( value );
}

double SqueezeFromParam( float value )
{
	return 1.0 + clamp01( value ) * ( kSqueezeMost - 1.0 );
}
float ParamFromSqueeze( double ratio )
{
	return inverseLinear( ratio - 1.0, kSqueezeMost - 1.0 );
}

double BubbleFromParam( float value )
{
	return 2.0 * clamp01( value ) - 1.0;
}
float ParamFromBubble( double amount )
{
	return static_cast< float >( std::clamp( 0.5 + 0.5 * amount, 0.0, 1.0 ) );
}

double FringeFromParam( float value )
{
	return clamp01( value ) * kFringeMost;
}
float ParamFromFringe( double nearness )
{
	return inverseLinear( nearness, kFringeMost );
}

double HighlightGainFromParam( float value )
{
	const double v = clamp01( value );
	return kHighlightsMost * v * v;
}
float ParamFromHighlightGain( double gain )
{
	return static_cast< float >( std::sqrt( std::clamp( gain / kHighlightsMost, 0.0, 1.0 ) ) );
}

int OrbCountFromParam( float value )
{
	return static_cast< int >( std::lround( clamp01( value ) * kOrbCountMost ) );
}

double OrbSizeFromParam( float value )
{
	return geometric( value, kOrbLow, kOrbHigh );
}
float ParamFromOrbSize( double heights )
{
	return inverseGeometric( heights, kOrbLow, kOrbHigh );
}

double OrbDriftFromParam( float value )
{
	return clamp01( value ) * kOrbDriftMost;
}

} // namespace hyacinth
