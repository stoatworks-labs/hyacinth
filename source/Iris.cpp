#include "Iris.h"

#include <algorithm>
#include <cmath>

namespace hyacinth
{
std::array< double, 2 * kMaxBlades > EdgeNormals( int blades )
{
	std::array< double, 2 * kMaxBlades > out {};
	const int n = std::clamp( blades, 3, kMaxBlades );
	for( int k = 0; k < n; ++k )
	{
		//Vertex k sits at 90 + k x 360/N degrees, so edge k (between vertex k
		//and k+1) faces half a step on.
		const double a       = 0.5 * kPi + ( k + 0.5 ) * 2.0 * kPi / n;
		out[ 2 * k ]         = std::cos( a );
		out[ 2 * k + 1 ]     = std::sin( a );
	}
	return out;
}

double Apothem( int blades )
{
	return std::cos( kPi / std::clamp( blades, 3, kMaxBlades ) );
}

namespace
{
double length2( double x, double y )
{
	return std::sqrt( x * x + y * y );
}
double dot2( double x, double y )
{
	return x * x + y * y;
}

/// Inigo Quilez's five-pointed star, points at radius r, inner radius r x rf.
double sdStar5( double px, double py, double r, double rf )
{
	const double k1x = 0.809016994375, k1y = -0.587785252292;
	const double k2x = -k1x, k2y = k1y;
	px = std::fabs( px );
	double d = std::max( k1x * px + k1y * py, 0.0 );
	px -= 2.0 * d * k1x;
	py -= 2.0 * d * k1y;
	d = std::max( k2x * px + k2y * py, 0.0 );
	px -= 2.0 * d * k2x;
	py -= 2.0 * d * k2y;
	px = std::fabs( px );
	py -= r;
	const double bax = rf * -k1y - 0.0, bay = rf * k1x - 1.0;
	const double h   = std::clamp( ( px * bax + py * bay ) / ( bax * bax + bay * bay ), 0.0, r );
	const double s   = py * bax - px * bay;
	return length2( px - bax * h, py - bay * h ) * ( s > 0.0 ? 1.0 : s < 0.0 ? -1.0 : 0.0 );
}

/// Inigo Quilez's heart: the point at the origin, the lobes up to y = 1.1.
double sdHeart( double px, double py )
{
	px = std::fabs( px );
	if( py + px > 1.0 )
		return std::sqrt( dot2( px - 0.25, py - 0.75 ) ) - std::sqrt( 2.0 ) / 4.0;
	const double m = 0.5 * std::max( px + py, 0.0 );
	const double s = px - py;
	return std::sqrt( std::min( dot2( px, py - 1.0 ), dot2( px - m, py - m ) ) ) * ( s > 0.0 ? 1.0 : s < 0.0 ? -1.0 : 0.0 );
}
} // namespace

namespace
{
/// ShapeSd with the blades' normals worked out once: AreaOf calls it for
/// every cell of its grid.
double shapeSdWith( const IrisDesc& desc, const std::array< double, 2 * kMaxBlades >& normals, double apothem, double ux,
                    double uy )
{
	const double radius = length2( ux, uy );
	switch( desc.shape )
	{
	case Shape::Mirror: return std::max( radius - 1.0, kObstruction - radius );
	case Shape::Star: return sdStar5( ux, uy, 1.0, 0.45 );
	case Shape::Heart: return sdHeart( 0.6 * ux, 0.6 * uy + 0.55 ) / 0.6;
	case Shape::Iris:
	case Shape::Count: break;
	}
	const int n = std::clamp( desc.blades, 3, kMaxBlades );
	double poly = -1e9;
	for( int k = 0; k < n; ++k )
		poly = std::max( poly, ux * normals[ 2 * k ] + uy * normals[ 2 * k + 1 ] );
	poly -= apothem;
	return poly + ( ( radius - 1.0 ) - poly ) * std::clamp( desc.curve, 0.0, 1.0 );
}
} // namespace

double ShapeSd( const IrisDesc& desc, double ux, double uy )
{
	return shapeSdWith( desc, EdgeNormals( desc.blades ), Apothem( desc.blades ), ux, uy );
}

double Profile( const IrisDesc& desc, double rho2, double side )
{
	rho2                 = std::min( rho2, 1.0 );
	double p             = 1.0 - desc.bubble * side * ( 2.0 * rho2 - 1.0 );
	const double x       = rho2 * kOnionRings;
	const double tri     = std::fabs( ( x - std::floor( x ) ) - 0.5 ) * 2.0;
	p *= 1.0 + desc.onion * ( 2.0 * tri - 1.0 );
	return std::max( p, 0.0 );
}

IrisArea AreaOf( const IrisDesc& desc )
{
	//The coverage the GLSL draws, integrated on a fine grid: a ramp one cell
	//wide across the edge, which integrates to the area to second order in
	//the cell (a hard step would be first order).
	constexpr int kCells   = 480;
	constexpr double kSpan = 1.05;
	const double h         = 2.0 * kSpan / kCells;
	IrisArea area;
	const auto normals   = EdgeNormals( desc.blades );
	const double apothem = Apothem( desc.blades );
	for( int j = 0; j < kCells; ++j )
	{
		const double y = -kSpan + ( j + 0.5 ) * h;
		for( int i = 0; i < kCells; ++i )
		{
			const double x     = -kSpan + ( i + 0.5 ) * h;
			const double cover = std::clamp( 0.5 - shapeSdWith( desc, normals, apothem, x, y ) / h, 0.0, 1.0 );
			if( cover <= 0.0 )
				continue;
			const double rho2 = x * x + y * y;
			area.behind += cover * Profile( desc, rho2, -1.0 );
			area.front += cover * Profile( desc, rho2, +1.0 );
		}
	}
	area.behind *= h * h;
	area.front *= h * h;
	return area;
}

std::array< float, 2 * kSpiralCount > Spiral()
{
	std::array< float, 2 * kSpiralCount > out {};
	const double golden = kPi * ( 3.0 - std::sqrt( 5.0 ) );
	for( int i = 0; i < kSpiralCount; ++i )
	{
		//Equal area per sample: radius as the square root of the fraction.
		const double r   = std::sqrt( ( i + 0.5 ) / kSpiralCount );
		const double a   = i * golden;
		out[ 2 * i ]     = static_cast< float >( r * std::cos( a ) );
		out[ 2 * i + 1 ] = static_cast< float >( r * std::sin( a ) );
	}
	return out;
}

const std::array< Ghost, kGhostCount >& Ghosts()
{
	//Six reflections, placed and coloured by eye from photographs of real
	//flares: the big faint ones across the centre, the small bright ones near
	//the light. What is physics is the line they all lie on, their shape and
	//that they move with the light; what is taste is the table.
	static const std::array< Ghost, kGhostCount > table = { {
		{ -0.45f, 0.035f, 1.00f, { 1.00f, 0.72f, 0.38f }, 0.10f },
		{ -0.95f, 0.090f, 0.45f, { 0.40f, 0.78f, 1.00f }, 0.25f },
		{ 0.40f, 0.020f, 1.20f, { 0.55f, 1.00f, 0.45f }, 0.08f },
		{ -1.35f, 0.140f, 0.30f, { 0.78f, 0.48f, 1.00f }, 0.35f },
		{ 0.70f, 0.050f, 0.60f, { 1.00f, 0.52f, 0.70f }, 0.15f },
		{ -0.20f, 0.012f, 1.50f, { 1.00f, 0.95f, 0.85f }, 0.06f },
	} };
	return table;
}

} // namespace hyacinth
