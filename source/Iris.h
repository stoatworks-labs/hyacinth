#pragma once

#include "Controls.h"

#include <array>

/**
    The aperture, on the CPU side.

    One shape stands behind everything Hyacinth draws. A defocused point is a
    picture of the aperture, scaled by its circle of confusion; a ghost in a
    flare is a picture of the aperture too, reflected back through the lens;
    and the starburst round a bright light is the diffraction of the
    aperture's straight blade edges, one line through the light per edge. So
    Blades sets all three: a seven-bladed iris gives heptagonal bokeh,
    heptagonal ghosts and fourteen spikes (an odd blade count's opposite
    edges are not parallel, so each edge's line is its own pair of spikes);
    an eight-bladed one gives octagons and eight spikes, two edges to a line.

    The GLSL evaluates the shape (`shapeSd` in Shaders.cpp). This file holds
    what the GLSL is handed: the blade edges' normals, the shape's area at
    unit radius (so a defocused point keeps its energy whatever its shape),
    the gather's sample spiral, and the flare's ghosts. `ShapeSd` here is the
    same function in double, for the area; the harness's `--energy` measures
    the picture against that area, so the two copies cannot drift apart
    without a check failing.
*/
namespace hyacinth
{
constexpr int kMaxBlades = 12;

/// The aperture's shape as the controls set it.
struct IrisDesc
{
	Shape shape    = Shape::Iris;
	int blades     = 7;
	double curve   = 0.25;///< 0 straight blades, 1 a circle
	double bubble  = 0.0; ///< spherical aberration, -1..1
	double onion   = 0.0; ///< onion rings, 0..1

	bool operator==( const IrisDesc& o ) const
	{
		return shape == o.shape && blades == o.blades && curve == o.curve && bubble == o.bubble && onion == o.onion;
	}
	bool operator!=( const IrisDesc& o ) const
	{
		return !( *this == o );
	}
};

/// Unit normals of the blade edges, the iris unrotated: a vertex at the top.
/// Entries past `blades` are zero.
std::array< double, 2 * kMaxBlades > EdgeNormals( int blades );

/// The polygon's apothem at circumradius 1: cos( pi / N ).
double Apothem( int blades );

/// The radius of the Mirror shape's central obstruction, as a fraction.
constexpr double kObstruction = 0.45;

/// Onion rings across the disc: whole periods of rho^2, so their mean is zero.
constexpr double kOnionRings = 6.0;

/// The unit shape's signed distance (negative inside), background
/// orientation, circumradius about 1. The GLSL's shapeSd() in double.
double ShapeSd( const IrisDesc& desc, double ux, double uy );

/// The radial profile a disc carries: spherical aberration and onion rings.
/// `side` is +1 in front of focus and -1 behind.
double Profile( const IrisDesc& desc, double rho2, double side );

/// The shape's light at unit radius, integrated: area times mean profile,
/// for a disc behind focus [0] and in front of it [1]. A disc of radius r
/// spreads E over AreaOf()[side] r^2, so dividing by that keeps E.
struct IrisArea
{
	double behind = 0.0;
	double front  = 0.0;
};
IrisArea AreaOf( const IrisDesc& desc );

//---------------------------------------------------------------------------
// The gather: a golden-angle spiral, evenly spread over the unit disc.
//---------------------------------------------------------------------------
constexpr int kSpiralCount = 96;
std::array< float, 2 * kSpiralCount > Spiral();

//---------------------------------------------------------------------------
// The flare's ghosts: each is the aperture, reflected between two of the
// lens's surfaces, so it lies on the line from the light through the
// picture's centre, at `at` times the light's offset from the centre
// (negative: across the centre), and is coloured by the coatings it passed.
//---------------------------------------------------------------------------
constexpr int kGhostCount = 6;
struct Ghost
{
	float at;       ///< position on the axis: centre + at x (light - centre)
	float radius;   ///< picture heights
	float reflect;  ///< relative brightness
	float tint[ 3 ];///< linear RGB
	float soft;     ///< edge softness, a fraction of the radius
};
const std::array< Ghost, kGhostCount >& Ghosts();

} // namespace hyacinth
