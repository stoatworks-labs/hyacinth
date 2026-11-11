#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Option parameters hold the element index.

    Units. Lengths on the picture are in picture HEIGHTS (a 16:9 frame is
    1.78 heights across), so a bokeh disc is the same shape at any aspect.
    Depth is NEARNESS, n: 1 at the nearest thing in the scene, 0 at
    infinity, and linear in 1/distance. That is the quantity a thin lens's
    blur is linear in (AGENTS.md, "The one idea"), and the one a plane in
    front of a pinhole camera is affine in across the picture.
*/
namespace hyacinth
{
/// Every control, in the order Resolume shows them. The About block is LAST,
/// so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Lens -------------------------------------------------------------------
	PT_MODE = 0,
	PT_BLUR,

	// -- Depth: where everything is ------------------------------------------
	PT_DEPTH_FROM,
	PT_PLANE_ANGLE,
	PT_HORIZON,
	PT_RADIUS,
	PT_DEPTH_EDGES,
	PT_DEPTH_INVERT,

	// -- Focus: what is sharp --------------------------------------------------
	PT_FOCUS,
	PT_FOCUS_DEPTH,
	PT_FOCUS_X,
	PT_FOCUS_Y,
	PT_GRAB,
	PT_TRACK_SIZE,
	PT_FOCUS_SPEED,

	// -- Aperture: the shape of a defocused point -------------------------------
	PT_SHAPE,
	PT_BLADES,
	PT_BLADE_CURVE,
	PT_IRIS_ROTATE,
	PT_CAT_EYE,
	PT_SWIRL,
	PT_SQUEEZE,
	PT_BUBBLE,
	PT_ONION,
	PT_FRINGE,

	// -- Highlights: the scene's light above what video could hold -------------
	PT_HIGHLIGHTS,
	PT_THRESHOLD,

	// -- Orbs: bokeh of lights that are not in the picture ------------------------
	PT_ORBS,
	PT_ORB_COUNT,
	PT_ORB_SIZE,
	PT_ORB_DRIFT,

	// -- Flare: light reflected between the lens's own surfaces -----------------
	PT_FLARE,
	PT_FLARE_FROM,
	PT_LIGHT_X,
	PT_LIGHT_Y,
	PT_GHOSTS,
	PT_HALO,
	PT_STARBURST,
	PT_STREAK,
	PT_DIRT,

	// -- Output -----------------------------------------------------------------
	PT_SHOW,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Option,  ///< an element index
	Boolean, ///< 0 or 1
	Event,   ///< a press: acts on the rising edge
	XPos,    ///< 0..1 across the frame
	YPos     ///< 0..1 up the frame
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name; ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, or the index
	const char* const* options;
	int optionCount;
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options. The shaders switch on these indices, so their order is fixed.
//---------------------------------------------------------------------------

/// What the lens does to the picture.
enum class Mode
{
	DepthOfField = 0,///< the picture defocused by depth, highlights as bokeh
	BokehOnly,       ///< the picture left sharp; its highlights' bokeh drawn over it
	Count
};

/// Where the depth comes from.
enum class DepthFrom
{
	Plane = 0, ///< a plane receding to a horizon: the ground, a wall, a table top
	Radial,    ///< nearest at the focus point, receding all round it
	Luma,      ///< bright is near (Depth Invert: far)
	Haze,      ///< the dark channel prior: haze thickens with distance
	Subject,   ///< what looks like the thing at the focus point is near, the rest far
	ClipAlpha, ///< the clip's alpha IS the depth (white near)
	SideBySide,///< left half picture, right half depth
	OverUnder, ///< top half picture, bottom half depth
	Count
};

/// What decides the focus distance.
enum class Focus
{
	Manual = 0,///< Focus Depth
	Point,     ///< the depth at Focus X, Focus Y
	Track,     ///< the depth at whatever was at Focus X, Focus Y when grabbed, followed
	Count
};

/// The aperture: what a defocused point looks like.
enum class Shape
{
	Iris = 0,///< Blades straight-edged blades, rounded by Blade Curve
	Mirror,  ///< a catadioptric lens's ring: the secondary mirror blocks the middle
	Star,    ///< a five-pointed cut-out in front of the lens
	Heart,   ///< a heart-shaped cut-out
	Count
};

/// Where the flare's light comes from.
enum class FlareFrom
{
	Highlights = 0,///< every bright spot in the picture
	Light,         ///< one light at Light X, Light Y that the picture does not have
	Tracked,       ///< that light, on whatever is being tracked
	Count
};

/// What the output shows.
enum class Show
{
	Picture = 0,
	Depth,  ///< nearness as grey (white near), the focal plane marked
	Blur,   ///< the circle of confusion: red in front of focus, blue behind
	Tracker,///< the picture, with the tracker's box and the focus point
	Count
};

/// An option's index from the value the host stored (it reads back 0..1
/// whatever the count -- the fleet's trap -- so the table's own count is
/// what bounds it).
int OptionIndex( float value, int count );

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

constexpr double kPi     = 3.14159265358979323846;
constexpr double kDegree = kPi / 180.0;

/// Blur Size: the circle of confusion's RADIUS for a nearness difference of
/// 1 (the nearest thing with the lens focused at infinity), 0 to 8% of the
/// picture's height, linearly.
double BlurFromParam( float value );
float ParamFromBlur( double heights );

/// Plane Angle: which way the plane comes toward the camera, 0 to 360
/// degrees: 0 nearer to the right, 90 nearer at the top, 270 nearer at the
/// bottom (the ground).
double PlaneAngleFromParam( float value );
float ParamFromPlaneAngle( double radians );

/// Horizon: where the plane meets infinity, 0 (the far edge of the frame)
/// to 1 (the near edge), linearly, measured along Plane Angle.
double HorizonFromParam( float value );

/// Radius: how far from the focus point Radial reaches infinity, 0.1 to 3
/// picture heights, geometrically.
double RadiusFromParam( float value );
float ParamFromRadius( double heights );

/// Depth Edges: the guided filter's radius, 0.5% to 8% of the picture's
/// height, geometrically. It snaps an estimated depth (Haze, Subject) to the
/// picture's own edges. Luma goes through it too, and comes out as it went
/// in: luma is a linear function of the colour, which the filter fits exactly.
double DepthEdgesFromParam( float value );
float ParamFromDepthEdges( double heights );

/// Track Size: the tracked patch's side, 4% to 40% of the picture's height,
/// geometrically.
double TrackSizeFromParam( float value );
float ParamFromTrackSize( double heights );

/// Focus Speed: the focus pull's time constant, tau = 2 s x v^2. 0 is
/// instant; the default is a fifth of a second.
double FocusTauFromParam( float value );
float ParamFromFocusTau( double seconds );

/// Blades: 3 to 12, the slider in nine equal steps.
int BladesFromParam( float value );
float ParamFromBlades( int blades );

/// Iris Rotate: 0 to 360 degrees, linearly.
double IrisRotateFromParam( float value );

/// Cat Eye: the lens barrel's shadow, 0 to 1. At 1 a point in the picture's
/// corner is cut to half its width.
double CatEyeFromParam( float value );

/// Swirl: the field's curvature, 0 to 1. At 1 a disc in the corner is 40%
/// narrower toward the centre than round it.
double SwirlFromParam( float value );

/// Squeeze: an anamorphic lens's, 1 (round) to 2 (twice as tall), linearly.
double SqueezeFromParam( float value );
float ParamFromSqueeze( double ratio );

/// Bubble: spherical aberration, -1 to +1 with 0 at the middle. Positive
/// brightens the rim of the discs behind focus and the middle of the ones
/// in front, as an over-corrected lens does.
double BubbleFromParam( float value );
float ParamFromBubble( double amount );

/// Fringe: longitudinal chromatic aberration, 0 to 0.06 of nearness between
/// where green and where red and blue come to focus.
double FringeFromParam( float value );
float ParamFromFringe( double nearness );

/// Highlights: how much brighter than white the scene's highlights were,
/// a gain of 0 to 32 at full white, as 32 v^2.
double HighlightGainFromParam( float value );
float ParamFromHighlightGain( double gain );

/// Orb Count: 0 to 400, the slider in 400 steps.
int OrbCountFromParam( float value );

/// Orb Size: an orb's radius, 1% to 15% of the picture's height, geometrically.
double OrbSizeFromParam( float value );
float ParamFromOrbSize( double heights );

/// Orb Drift: 0 to 0.2 picture heights a second, linearly.
double OrbDriftFromParam( float value );

} // namespace hyacinth
