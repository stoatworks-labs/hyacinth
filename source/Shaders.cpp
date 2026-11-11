#include "Shaders.h"

namespace hyacinth::shaders
{

const char* const kVersion = "#version 410 core\n";

const char* const kQuadVertex = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// Shared by every pass: the hooks, light, the picture, the frame, the hash
// and where the focus point is.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
//Shaders.h's Hook bits and sizes, which this text must repeat.
const int kHookLinearDepth  = 1;
const int kHookNoEnergy     = 2;
const int kHookCatEyeKeeps  = 4;
const int kHookNoFlip       = 8;
const int kHookSpikePerEdge = 16;
const int kHookGhostOffAxis = 32;
const int kHookFringeSwap   = 64;
const int kHookNoOcclusion  = 128;
const int kHookBackSpread   = 256;
const int kHookRoundTrip    = 512;
const int kHookNoSubTexel   = 1024;
const int kHookFocusLinear  = 2048;
const int kHookDepthNoRect  = 4096;
const int kHookNoGuide      = 8192;

const int kTemplateSide  = 24;
const int kHistogramBins = 512;
const int kHistogramSide = 32;
const int kSpiralCount   = 96;
const int kMaxBlades     = 12;
const int kGhostCount    = 6;
const float kPi          = 3.14159265358979;

uniform sampler2D Picture;//the host's input
uniform vec4 PictureRect; //picture uv -> input texture uv: offset, scale
uniform vec4 DepthRect;   //the same for the depth half of Side by Side, Over Under
uniform vec2 Frame;       //the picture, full-resolution pixels
uniform float Aspect;     //Frame.x / Frame.y
uniform float HalfDiagonal;//the centre to a corner, picture heights
uniform int Hooks;

uniform sampler2D TrackState;//2x1: ( position, velocity ), ( confidence, frames, 0, 0 )
uniform int TrackActive;
uniform vec2 FocusPoint;     //uv, the operator's point

bool hooked( int bit )
{
	return ( Hooks & bit ) != 0;
}

//sRGB, both ways. Light adds and spreads in linear light.
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}
float lumaOf( vec3 c )
{
	return dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
}

vec4 picture( vec2 at )
{
	return texture( Picture, PictureRect.xy + at * PictureRect.zw );
}

//Picture heights from the centre: a disc is round at any aspect.
vec2 heights( vec2 at )
{
	return vec2( ( at.x - 0.5 ) * Aspect, at.y - 0.5 );
}

//Where the picture's field position is, as a lens sees it: 0 at the centre,
//1 at a corner. Cat Eye and Swirl grow with it.
vec2 fieldOf( vec2 at )
{
	return heights( at ) / HalfDiagonal;
}

vec2 trackedPoint()
{
	return texelFetch( TrackState, ivec2( 0, 0 ), 0 ).xy;
}
vec2 focusPoint()
{
	return TrackActive != 0 ? trackedPoint() : FocusPoint;
}

//PCG (O'Neill), output-mixed: exact in 32 bits on every implementation.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
float unitOf( uint h )
{
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}
)";

//---------------------------------------------------------------------------
// The scene: nearness, the focus, the circle of confusion and the highlights.
//---------------------------------------------------------------------------
const char* const kScene = R"(
const int kDepthPlane   = 0;
const int kDepthRadial  = 1;
const int kDepthLuma    = 2;
const int kDepthHaze    = 3;
const int kDepthSubject = 4;
const int kDepthAlpha   = 5;
const int kDepthSide    = 6;
const int kDepthOver    = 7;

uniform int DepthSource;
uniform vec3 PlaneEq;    //Plane: n = PlaneEq.x h.x + PlaneEq.y h.y + PlaneEq.z, h in heights
uniform float RadialInv; //Radial: 1 / the distance at which n reaches 0, heights
uniform sampler2D Guide; //the guided filter's ( a.rgb, b ), quarter resolution
uniform int DepthInvert;
uniform sampler2D FocusState;//1x1: ( focus nearness, its target, 0, 1 )
uniform float BlurPx;    //the CoC's radius per unit of nearness, pixels

uniform float Gain;          //Highlights: how much brighter than white white was
uniform float ThresholdCode; //where the highlights start, code value
uniform float ThresholdLight;//the same, linear

float nearnessAt( vec2 at )
{
	float n;
	if( DepthSource == kDepthPlane )
	{
		vec2 h = heights( at );
		n      = PlaneEq.x * h.x + PlaneEq.y * h.y + PlaneEq.z;
	}
	else if( DepthSource == kDepthRadial )
		n = 1.0 - length( heights( at ) - heights( focusPoint() ) ) * RadialInv;
	else if( DepthSource <= kDepthSubject )
	{
		//The fast guided filter: coefficients from a quarter-resolution pass,
		//the guide (the picture's colour) at full resolution.
		vec4 ab = texture( Guide, at );
		n       = dot( ab.rgb, picture( at ).rgb ) + ab.a;
	}
	else if( DepthSource == kDepthAlpha )
		n = picture( at ).a;
	else
	{
		vec4 rect = hooked( kHookDepthNoRect ) ? PictureRect : DepthRect;
		n         = lumaOf( texture( Picture, rect.xy + at * rect.zw ).rgb );
	}
	n = clamp( n, 0.0, 1.0 );
	return DepthInvert != 0 ? 1.0 - n : n;
}

float focusNearness()
{
	return texelFetch( FocusState, ivec2( 0, 0 ), 0 ).r;
}

//The thin lens. A point at distance d, the lens focused at s, blurs to a
//circle of diameter A f |s - d| / ( d ( s - f ) ) = A f s / ( s - f ) x
//|1/s - 1/d|: linear in the difference of NEARNESS, so the CoC's radius is
//BlurPx ( n - nf ). Positive in front of focus, negative behind.
float cocOf( float n, float nf )
{
	if( hooked( kHookLinearDepth ) )
	{
		float d  = 1.0 / max( n, 0.02 );
		float df = 1.0 / max( nf, 0.02 );
		return BlurPx * ( df - d ) / df;
	}
	return BlurPx * ( n - nf );
}

//Video clips at white what the scene had far above it. Above Threshold the
//light is restored, as a gain that grows to Gain at white. Brightness is the
//largest channel, not luma: a red lamp clips in red alone, and is a light.
vec3 restored( vec3 code )
{
	float y = max( code.r, max( code.g, code.b ) );
	float s = clamp( ( y - ThresholdCode ) / max( 1.0 - ThresholdCode, 1e-3 ), 0.0, 1.0 );
	return toLinear( code ) * ( 1.0 + Gain * s * s );
}
//The part of that light above Threshold: what the sprites carry.
vec3 excessOf( vec3 light )
{
	float y = max( light.r, max( light.g, light.b ) );
	return light * max( 0.0, 1.0 - ThresholdLight / max( y, 1e-6 ) );
}
//Only a disc wider than a sprite cell is scattered; a smaller one stays in
//the picture, where the gather blurs it.
float scatteredOf( float coc )
{
	return smoothstep( 3.0, 6.0, abs( coc ) );
}
)";

//---------------------------------------------------------------------------
// The aperture: what a defocused point looks like, where it is in the field.
//---------------------------------------------------------------------------
const char* const kIris = R"(
const float kObstruction = 0.45;
const float kOnionRings  = 6.0;

uniform int ShapeIndex;
uniform int Blades;
uniform vec2 EdgeNormal[ kMaxBlades ];
uniform float ApothemValue;
uniform float BladeCurve;
uniform vec2 IrisTurn;//cos, sin of Iris Rotate
uniform float CatEye;
uniform float Swirl;
uniform float Squeeze;
uniform float Bubble;
uniform float Onion;
uniform float AaPx;  //the edge's ramp, pixels: one texel of the buffer drawn into

float dot2( vec2 v )
{
	return dot( v, v );
}

//Inigo Quilez's five-pointed star, points at radius r.
float sdStar5( vec2 p, float r, float rf )
{
	const vec2 k1 = vec2( 0.809016994375, -0.587785252292 );
	const vec2 k2 = vec2( -k1.x, k1.y );
	p.x = abs( p.x );
	p -= 2.0 * max( dot( k1, p ), 0.0 ) * k1;
	p -= 2.0 * max( dot( k2, p ), 0.0 ) * k2;
	p.x = abs( p.x );
	p.y -= r;
	vec2 ba = rf * vec2( -k1.y, k1.x ) - vec2( 0.0, 1.0 );
	float h = clamp( dot( p, ba ) / dot( ba, ba ), 0.0, r );
	return length( p - ba * h ) * sign( p.y * ba.x - p.x * ba.y );
}

//Inigo Quilez's heart: the point at the origin, the lobes up to y = 1.1.
float sdHeart( vec2 p )
{
	p.x = abs( p.x );
	if( p.y + p.x > 1.0 )
		return sqrt( dot2( p - vec2( 0.25, 0.75 ) ) ) - sqrt( 2.0 ) / 4.0;
	return sqrt( min( dot2( p - vec2( 0.0, 1.0 ) ), dot2( p - 0.5 * max( p.x + p.y, 0.0 ) ) ) ) * sign( p.x - p.y );
}

//The unit aperture, as it shows behind focus: negative inside. Iris.cpp's
//ShapeSd is this in double, for the area.
float shapeSd( vec2 u )
{
	float radius = length( u );
	if( ShapeIndex == 1 )
		return max( radius - 1.0, kObstruction - radius );
	if( ShapeIndex == 2 )
		return sdStar5( u, 1.0, 0.45 );
	if( ShapeIndex == 3 )
		return sdHeart( 0.6 * u + vec2( 0.0, 0.55 ) ) / 0.6;
	float poly = -1e9;
	for( int k = 0; k < kMaxBlades; ++k )
	{
		if( k >= Blades )
			break;
		poly = max( poly, dot( u, EdgeNormal[ k ] ) );
	}
	poly -= ApothemValue;
	return poly + ( ( radius - 1.0 ) - poly ) * BladeCurve;
}

//Over-corrected spherical aberration brightens a disc's rim behind focus and
//its middle in front; polished aspheric elements leave onion rings. Both are
//functions of rho^2 with a mean of zero over the disc, so neither moves light
//in or out of it. Past the rim (the edge's ramp) the profile is the rim's:
//carried on outward it would add light the area does not count.
//`onion` is Onion faded where the rings are finer than the buffer can draw.
float profileOf( float rho2, float side, float onion )
{
	rho2      = min( rho2, 1.0 );
	float p   = 1.0 - Bubble * side * ( 2.0 * rho2 - 1.0 );
	float x   = rho2 * kOnionRings;
	float tri = abs( fract( x ) - 0.5 ) * 2.0;
	p *= 1.0 + onion * ( 2.0 * tri - 1.0 );
	return max( p, 0.0 );
}

//Petzval's field curvature narrows a disc toward the centre of the field: the
//swirl. 40% at a corner at Swirl 1.
float swirlOf( vec2 field )
{
	return 1.0 - 0.4 * Swirl * dot( field, field );
}

//The aperture turned by Iris Rotate: R( -angle ) u.
vec2 irisFrame( vec2 u )
{
	return vec2( IrisTurn.x * u.x + IrisTurn.y * u.y, -IrisTurn.y * u.x + IrisTurn.x * u.y );
}

//The light at q (pixels from the disc's centre) of a defocused point whose
//circle of confusion has radius r, in front of focus (side +1) or behind
//(-1), at a field position. 1 is the light of a point spread evenly over the
//unit aperture; the caller divides by the area.
float irisLight( vec2 q, float r, float side, vec2 field )
{
	vec2 u = q / r;
	//An anamorphic lens squeezes the picture, not the iris: its discs are tall.
	u.y /= Squeeze;
	float f2 = dot( field, field );
	if( Swirl > 0.0 && f2 > 1e-8 )
	{
		vec2 e   = field * inversesqrt( f2 );
		float ur = dot( u, e );
		u += e * ( ur / swirlOf( field ) - ur );
	}
	//The barrel's shadow: off axis the light has to pass two round openings
	//that no longer line up, so the disc is their overlap, a cat's eye. What
	//they cut off is lost, which is mechanical vignetting.
	float vignette = -1e9;
	vec2 d         = 0.5 * CatEye * field;
	if( CatEye > 0.0 )
		vignette = max( length( u - d ), length( u + d ) ) - 1.0;
	//In front of focus the cone has not yet crossed: the aperture shows the
	//other way up.
	vec2 ui = irisFrame( u );
	if( side > 0.0 && !hooked( kHookNoFlip ) )
		ui = -ui;
	float sd    = max( shapeSd( ui ), vignette );
	float cover = clamp( 0.5 - sd * r / AaPx, 0.0, 1.0 );
	if( cover <= 0.0 )
		return 0.0;
	//A ring's width at the rim is r / ( 2 kOnionRings ): drawn only where that
	//is two texels or more, faded in from one.
	float rings = smoothstep( 2.0 * kOnionRings, 4.0 * kOnionRings, r / AaPx );
	float light = cover * profileOf( dot( u, u ), side, Onion * rings );
	if( hooked( kHookCatEyeKeeps ) && CatEye > 0.0 )
		light /= max( 1.0 - 1.2732395 * length( d ), 0.05 );
	return light;
}
)";

//---------------------------------------------------------------------------
// 1. The analysis picture: 4x4 pixels a texel, code values, luma in alpha.
//---------------------------------------------------------------------------
const char* const kAnalysisFragment = R"(
in vec2 uv;
out vec4 fragColor;

uniform vec2 CellUv;//one texel of this buffer, in picture uv

void main()
{
	//Four bilinear taps, each the mean of a 2x2 block: the 4x4 block's mean.
	vec2 o = 0.25 * CellUv;
	vec3 c = 0.25 * ( picture( uv + vec2( -o.x, -o.y ) ).rgb + picture( uv + vec2( o.x, -o.y ) ).rgb
	                  + picture( uv + vec2( -o.x, o.y ) ).rgb + picture( uv + o ).rgb );
	fragColor = vec4( c, lumaOf( c ) );
}
)";

//---------------------------------------------------------------------------
// Haze: the dark channel (He, Sun and Tang 2009), a separable minimum.
//---------------------------------------------------------------------------
const char* const kDarkFragment = R"(
out vec4 fragColor;

uniform sampler2D Source;
uniform ivec2 Dir;
uniform int Radius;
uniform int FromColour;//1: the minimum over the channels first

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Source, 0 );
	float m    = 1e9;
	for( int i = -Radius; i <= Radius; ++i )
	{
		vec4 s = texelFetch( Source, clamp( p + Dir * i, ivec2( 0 ), size - 1 ), 0 );
		m      = min( m, FromColour != 0 ? min( min( s.r, s.g ), s.b ) : s.r );
	}
	fragColor = vec4( m, 0.0, 0.0, 1.0 );
}
)";

const char* const kReduceFragment = R"(
out vec4 fragColor;

uniform sampler2D Source;
uniform int Block;

void main()
{
	ivec2 base = ivec2( gl_FragCoord.xy ) * Block;
	ivec2 size = textureSize( Source, 0 );
	float m    = 0.0;
	for( int j = 0; j < Block; ++j )
		for( int i = 0; i < Block; ++i )
		{
			ivec2 q = base + ivec2( i, j );
			if( q.x < size.x && q.y < size.y )
				m = max( m, texelFetch( Source, q, 0 ).r );
		}
	fragColor = vec4( m, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Subject: the colours of the thing at the focus point (row 0) and of the
// ring round it (row 1), as 8x8x8 histograms, each sample splatted
// trilinearly into its eight bins; column 512 counts the samples.
//---------------------------------------------------------------------------
const char* const kHistogramVertex = R"(
uniform sampler2D Analysis;
uniform float AnalysisLod;
uniform float TrackHeights;

out float vWeight;

void main()
{
	int s      = gl_InstanceID;
	int corner = gl_VertexID;
	vec2 g     = ( vec2( float( s % kHistogramSide ), float( s / kHistogramSide ) ) + 0.5 ) / float( kHistogramSide ) * 2.0 - 1.0;
	vec2 off   = g * 1.25 * TrackHeights;//the box is two and a half patches across
	vec2 at    = focusPoint() + vec2( off.x / Aspect, off.y );
	float rs   = length( off ) / TrackHeights;
	int row    = rs < 0.4 ? 0 : ( rs > 0.7 ? 1 : -1 );
	vWeight    = 0.0;
	if( row < 0 || at.x < 0.0 || at.x > 1.0 || at.y < 0.0 || at.y > 1.0 )
	{
		gl_Position = vec4( 2.0, 2.0, 2.0, 1.0 );
		return;
	}
	vec3 c  = clamp( textureLod( Analysis, at, AnalysisLod ).rgb, 0.0, 1.0 ) * 7.0;
	vec3 b0 = floor( min( c, vec3( 6.999 ) ) );
	vec3 f  = c - b0;
	float column;
	if( corner == 8 )
	{
		column  = float( kHistogramBins );
		vWeight = 1.0;
	}
	else
	{
		ivec3 d   = ivec3( corner & 1, ( corner >> 1 ) & 1, ( corner >> 2 ) & 1 );
		vec3 w    = mix( 1.0 - f, f, vec3( d ) );
		vWeight   = w.x * w.y * w.z;
		ivec3 bin = ivec3( b0 ) + d;
		column    = float( bin.x + 8 * bin.y + 64 * bin.z );
	}
	gl_Position = vec4( ( column + 0.5 ) / float( kHistogramBins + 1 ) * 2.0 - 1.0, float( row ) - 0.5, 0.0, 1.0 );
}
)";

const char* const kHistogramFragment = R"(
in float vWeight;
out vec4 fragColor;

void main()
{
	fragColor = vec4( vWeight, 0.0, 0.0, 0.0 );
}
)";

//---------------------------------------------------------------------------
// The raw estimate, before the guided filter.
//---------------------------------------------------------------------------
const char* const kRawDepthFragment = R"(
in vec2 uv;
out vec4 fragColor;

uniform sampler2D Analysis;
uniform sampler2D Dark;
uniform sampler2D Airlight;
uniform sampler2D Histogram;
uniform float TrackHeights;

void main()
{
	vec4 a = textureLod( Analysis, uv, 0.0 );
	float p;
	//How much the guided filter should trust this texel: see below.
	float w = 1.0;
	if( DepthSource == kDepthLuma )
		p = a.a;
	else if( DepthSource == kDepthHaze )
	{
		//I = J t + A ( 1 - t ), and a haze-free picture's dark channel is
		//about 0, so t = 1 - dark / A. t = exp( -beta d ): distance is
		//-ln t, and nearness falls as the haze thickens.
		float A = max( texelFetch( Airlight, ivec2( 0, 0 ), 0 ).r, 1e-3 );
		float t = clamp( 1.0 - 0.95 * textureLod( Dark, uv, 0.0 ).r / A, 0.05, 1.0 );
		p       = 1.0 - clamp( log( t ) / log( 0.05 ), 0.0, 1.0 );
	}
	else
	{
		//How much likelier this colour is on the subject than round it,
		//weighted toward where the subject is.
		ivec3 bin  = ivec3( floor( clamp( a.rgb, 0.0, 1.0 ) * 7.0 + 0.5 ) );
		int column = bin.x + 8 * bin.y + 64 * bin.z;
		float fgN  = max( texelFetch( Histogram, ivec2( kHistogramBins, 0 ), 0 ).r, 1.0 );
		float bgN  = max( texelFetch( Histogram, ivec2( kHistogramBins, 1 ), 0 ).r, 1.0 );
		float pf   = texelFetch( Histogram, ivec2( column, 0 ), 0 ).r / fgN;
		float pb   = texelFetch( Histogram, ivec2( column, 1 ), 0 ).r / bgN;
		float m    = pf / ( pf + pb + 0.02 );
		vec2 d     = ( heights( uv ) - heights( focusPoint() ) ) / ( 1.2 * TrackHeights );
		p          = m * exp( -0.5 * dot( d, d ) );
		//A texel across an edge is a mix of its two sides' colours, which
		//neither histogram holds: its m says nothing. It weighs what its
		//colour's evidence weighs, and the colour model fills it in.
		w = ( pf + pb ) / ( pf + pb + 0.02 );
	}
	fragColor = vec4( p, max( w, 1e-3 ), 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// The guided filter (He, Sun and Tang 2010): q = mean( a ) I + mean( b ).
//---------------------------------------------------------------------------
//The colour guided filter (He, Sun and Tang 2010, section 3.5). In each
//window the depth is taken as a linear function of the picture's COLOUR,
//p = a . I + b, fitted by least squares with a ridge Epsilon; the depth's
//edges then follow the picture's wherever its colour changes, luma or not.
//That matters most for Subject, whose estimate IS a colour judgement: a red
//thing on a green ground of the same luma gives a luma guide nothing.
//Thirteen sums per window -- I (3), p, I I^T (6), I p (3) -- in four parts,
//each made and summed along the row in one pass. Every sum is weighted by
//the raw estimate's confidence, and the weights' own sum rides in the last
//part (a weighted least-squares fit, the same fit when every weight is 1).
const char* const kGuideRowFragment = R"(
out vec4 fragColor;

uniform sampler2D Analysis;
uniform sampler2D Raw;
uniform int Part;
uniform int Radius;

vec4 partAt( ivec2 q )
{
	vec3 I  = texelFetch( Analysis, q, 0 ).rgb;
	vec2 dw = texelFetch( Raw, q, 0 ).rg;
	float d = dw.x, w = dw.y;
	if( Part == 0 )
		return w * vec4( I, d );
	if( Part == 1 )
		return w * vec4( I.r * I, I.g * I.g );
	if( Part == 2 )
		return w * vec4( I.g * I.b, I.b * I.b, I.r * d, I.g * d );
	return vec4( w * I.b * d, w, 0.0, 0.0 );
}

void main()
{
	ivec2 q    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Raw, 0 );
	vec4 s     = vec4( 0.0 );
	for( int i = -Radius; i <= Radius; ++i )
		s += partAt( clamp( q + ivec2( i, 0 ), ivec2( 0 ), size - 1 ) );
	fragColor = s / float( 2 * Radius + 1 );
}
)";

const char* const kBoxFragment = R"(
out vec4 fragColor;

uniform sampler2D Source;
uniform ivec2 Dir;
uniform int Radius;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Source, 0 );
	vec4 s     = vec4( 0.0 );
	for( int i = -Radius; i <= Radius; ++i )
		s += texelFetch( Source, clamp( p + Dir * i, ivec2( 0 ), size - 1 ), 0 );
	fragColor = s / float( 2 * Radius + 1 );
}
)";

const char* const kGuideCoefFragment = R"(
out vec4 fragColor;

uniform sampler2D Means0;//I, p
uniform sampler2D Means1;//rr, rg, rb, gg
uniform sampler2D Means2;//gb, bb, rp, gp
uniform sampler2D Means3;//bp, and the weights
uniform sampler2D Raw;
uniform float Epsilon;

void main()
{
	ivec2 q = ivec2( gl_FragCoord.xy );
	if( hooked( kHookNoGuide ) )
	{
		fragColor = vec4( 0.0, 0.0, 0.0, texelFetch( Raw, q, 0 ).r );
		return;
	}
	vec4 m3  = texelFetch( Means3, q, 0 );
	float wInv = 1.0 / m3.y;//the weights are 1e-3 at least
	vec4 m0  = texelFetch( Means0, q, 0 ) * wInv;
	vec4 m1  = texelFetch( Means1, q, 0 ) * wInv;
	vec4 m2  = texelFetch( Means2, q, 0 ) * wInv;
	m3.x *= wInv;
	vec3 mu  = m0.rgb;
	float pm = m0.a;
	//The colour's covariance plus the ridge: symmetric, positive definite.
	float srr = max( m1.x - mu.r * mu.r, 0.0 ) + Epsilon;
	float srg = m1.y - mu.r * mu.g;
	float srb = m1.z - mu.r * mu.b;
	float sgg = max( m1.w - mu.g * mu.g, 0.0 ) + Epsilon;
	float sgb = m2.x - mu.g * mu.b;
	float sbb = max( m2.y - mu.b * mu.b, 0.0 ) + Epsilon;
	vec3 c    = vec3( m2.z, m2.w, m3.x ) - mu * pm;
	//Its inverse, by the adjugate.
	float i00 = sgg * sbb - sgb * sgb;
	float i01 = srb * sgb - srg * sbb;
	float i02 = srg * sgb - srb * sgg;
	float i11 = srr * sbb - srb * srb;
	float i12 = srg * srb - srr * sgb;
	float i22 = srr * sgg - srg * srg;
	float det = srr * i00 + srg * i01 + srb * i02;
	vec3 a    = vec3( i00 * c.x + i01 * c.y + i02 * c.z, i01 * c.x + i11 * c.y + i12 * c.z,
	                  i02 * c.x + i12 * c.y + i22 * c.z ) / det;
	fragColor = vec4( a, pm - dot( a, mu ) );
}
)";

//---------------------------------------------------------------------------
// The tracker.
//---------------------------------------------------------------------------
const char* const kTemplateFragment = R"(
out vec4 fragColor;

uniform sampler2D Analysis;
uniform float AnalysisLod;
uniform vec2 TemplateStep;//one template texel, uv
uniform sampler2D OldTemplate;
uniform float TemplateBlend;
uniform int Regrab;
uniform vec2 GrabPos;

void main()
{
	vec2 centre = Regrab != 0 ? GrabPos : trackedPoint();
	//Learn the new look slowly, and only while the match is a good one.
	float blend = Regrab != 0 ? 1.0 : TemplateBlend * step( 0.7, texelFetch( TrackState, ivec2( 1, 0 ), 0 ).x );
	vec2 t      = gl_FragCoord.xy - 0.5 * float( kTemplateSide );
	vec4 now    = textureLod( Analysis, centre + t * TemplateStep, AnalysisLod );
	vec4 old    = texelFetch( OldTemplate, ivec2( gl_FragCoord.xy ), 0 );
	fragColor   = mix( old, now, blend );
}
)";

const char* const kScoreFragment = R"(
out vec4 fragColor;

uniform sampler2D Analysis;
uniform float AnalysisLod;
uniform vec2 TemplateStep;
uniform sampler2D Template;
uniform sampler2D Centre;  //the coarse stage's best ( uv, score )
uniform int FromState;     //1: search round the prediction (coarse)
uniform int Grid;
uniform float CandidateStep;//template texels between candidates

void main()
{
	vec2 c;
	if( FromState != 0 )
	{
		vec4 s = texelFetch( TrackState, ivec2( 0, 0 ), 0 );
		c      = s.xy + s.zw;
	}
	else
		c = texelFetch( Centre, ivec2( 0, 0 ), 0 ).xy;
	vec2 k  = ( gl_FragCoord.xy - 0.5 - 0.5 * float( Grid - 1 ) ) * CandidateStep;
	vec2 at = c + k * TemplateStep;

	//Zero-mean normalised cross-correlation of luma: a change of brightness
	//or contrast does not lose the subject. Chroma breaks ties.
	float n  = float( kTemplateSide * kTemplateSide );
	float sT = 0.0, sI = 0.0, sTT = 0.0, sII = 0.0, sTI = 0.0, chroma = 0.0;
	for( int y = 0; y < kTemplateSide; ++y )
		for( int x = 0; x < kTemplateSide; ++x )
		{
			vec4 T  = texelFetch( Template, ivec2( x, y ), 0 );
			vec2 o  = vec2( float( x ), float( y ) ) + 0.5 - 0.5 * float( kTemplateSide );
			vec4 I  = textureLod( Analysis, at + o * TemplateStep, AnalysisLod );
			sT += T.a;
			sI += I.a;
			sTT += T.a * T.a;
			sII += I.a * I.a;
			sTI += T.a * I.a;
			vec3 dc = ( T.rgb - T.a ) - ( I.rgb - I.a );
			chroma += dot( dc, dc );
		}
	float vT  = max( sTT - sT * sT / n, 0.0 );
	float vI  = max( sII - sI * sI / n, 0.0 );
	float ncc = ( sTI - sT * sI / n ) / sqrt( max( vT * vI, 1e-12 ) );
	fragColor = vec4( ncc - 4.0 * chroma / n, at, 0.0 );
}
)";

const char* const kPickFragment = R"(
out vec4 fragColor;

uniform sampler2D Scores;
uniform int Grid;
uniform int Final;  //0: write the best ( uv, score ); 1: update the track state
uniform int Regrab;
uniform vec2 GrabPos;
uniform vec2 TemplateStep;
uniform float CandidateStep;
uniform float LostBelow;

float scoreAt( ivec2 i )
{
	return texelFetch( Scores, i, 0 ).r;
}

void main()
{
	float best = -1e9;
	ivec2 bi   = ivec2( 0 );
	for( int j = 0; j < Grid; ++j )
		for( int i = 0; i < Grid; ++i )
		{
			float s = scoreAt( ivec2( i, j ) );
			if( s > best )
			{
				best = s;
				bi   = ivec2( i, j );
			}
		}
	vec2 at = texelFetch( Scores, bi, 0 ).yz;
	if( Final == 0 )
	{
		fragColor = vec4( at, best, 0.0 );
		return;
	}

	//A parabola through the peak and its neighbours, each way: the subject
	//moves by fractions of a texel, and so must the track.
	vec2 sub = vec2( 0.0 );
	if( !hooked( kHookNoSubTexel ) )
	{
		if( bi.x > 0 && bi.x < Grid - 1 )
		{
			float l = scoreAt( bi - ivec2( 1, 0 ) ), r = scoreAt( bi + ivec2( 1, 0 ) );
			float den = l - 2.0 * best + r;
			if( den < 0.0 )
				sub.x = clamp( 0.5 * ( l - r ) / den, -0.5, 0.5 );
		}
		if( bi.y > 0 && bi.y < Grid - 1 )
		{
			float l = scoreAt( bi - ivec2( 0, 1 ) ), r = scoreAt( bi + ivec2( 0, 1 ) );
			float den = l - 2.0 * best + r;
			if( den < 0.0 )
				sub.y = clamp( 0.5 * ( l - r ) / den, -0.5, 0.5 );
		}
	}
	at += sub * CandidateStep * TemplateStep;

	vec4 s0 = texelFetch( TrackState, ivec2( 0, 0 ), 0 );
	vec4 s1 = texelFetch( TrackState, ivec2( 1, 0 ), 0 );
	vec4 o0, o1;
	if( Regrab != 0 )
	{
		o0 = vec4( GrabPos, 0.0, 0.0 );
		o1 = vec4( 1.0, 0.0, 0.0, 0.0 );
	}
	else if( best >= LostBelow )
	{
		o0 = vec4( at, mix( s0.zw, at - s0.xy, 0.6 ) );
		o1 = vec4( best, s1.y + 1.0, 0.0, 0.0 );
	}
	else
	{
		//Lost: hold where it was, and stop predicting.
		o0 = vec4( s0.xy, 0.5 * s0.zw );
		o1 = vec4( best, s1.y + 1.0, 0.0, 0.0 );
	}
	fragColor = gl_FragCoord.x < 1.0 ? o0 : o1;
}
)";

//---------------------------------------------------------------------------
// The focus: a first-order ease toward the target, as a focus puller.
//---------------------------------------------------------------------------
const char* const kFocusFragment = R"(
out vec4 fragColor;

uniform int FocusMode;  //0 Manual, else the depth at the focus point
uniform float FocusManual;
uniform float FocusAlpha;//1 - exp( -dt / tau )
uniform int FocusReset;
uniform float FocusReach;//the sampled window's spacing, heights
uniform sampler2D PrevFocus;

void main()
{
	float target;
	if( FocusMode == 0 )
		target = FocusManual;
	else
	{
		vec2 c    = focusPoint();
		float sum = 0.0, w = 0.0;
		for( int j = -2; j <= 2; ++j )
			for( int i = -2; i <= 2; ++i )
			{
				float wt = exp( -0.5 * float( i * i + j * j ) );
				sum += wt * nearnessAt( c + vec2( float( i ) / Aspect, float( j ) ) * FocusReach );
				w += wt;
			}
		target = sum / w;
	}
	float prev = texelFetch( PrevFocus, ivec2( 0, 0 ), 0 ).r;
	float nf;
	if( FocusReset != 0 )
		nf = target;
	else if( hooked( kHookFocusLinear ) )
		nf = prev + clamp( target - prev, -FocusAlpha, FocusAlpha );
	else
		nf = prev + ( target - prev ) * FocusAlpha;
	fragColor = vec4( nf, target, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// 5. The prefilter (half): what the gather blurs, and the CoC.
//---------------------------------------------------------------------------
const char* const kPrefilterFragment = R"(
in vec2 uv;
out vec4 fragColor;

void main()
{
	vec3 light = restored( picture( uv ).rgb );
	float coc  = cocOf( nearnessAt( uv ), focusNearness() );
	fragColor  = vec4( light - excessOf( light ) * scatteredOf( coc ), coc );
}
)";

//---------------------------------------------------------------------------
// 6. The highlights (quarter): the light above Threshold, and the CoC.
//---------------------------------------------------------------------------
const char* const kHighlightFragment = R"(
in vec2 uv;
out vec4 fragColor;

uniform vec2 CellUv;

void main()
{
	vec2 o = 0.25 * CellUv;
	vec3 e = excessOf( restored( picture( uv + vec2( -o.x, -o.y ) ).rgb ) ) + excessOf( restored( picture( uv + vec2( o.x, -o.y ) ).rgb ) )
	         + excessOf( restored( picture( uv + vec2( -o.x, o.y ) ).rgb ) ) + excessOf( restored( picture( uv + o ).rgb ) );
	fragColor = vec4( 0.25 * e, cocOf( nearnessAt( uv ), focusNearness() ) );
}
)";

//---------------------------------------------------------------------------
// 7. The largest CoC near each tile, in the prefilter's texels.
//---------------------------------------------------------------------------
const char* const kTileFragment = R"(
out vec4 fragColor;

uniform sampler2D Source;
uniform float ToTexels;

void main()
{
	ivec2 base = ivec2( gl_FragCoord.xy ) * 8;
	ivec2 size = textureSize( Source, 0 );
	float m    = 0.0;
	for( int j = 0; j < 8; ++j )
		for( int i = 0; i < 8; ++i )
		{
			ivec2 q = base + ivec2( i, j );
			if( q.x < size.x && q.y < size.y )
				m = max( m, abs( texelFetch( Source, q, 0 ).a ) );
		}
	fragColor = vec4( m * ToTexels, 0.0, 0.0, 1.0 );
}
)";

const char* const kDilateFragment = R"(
out vec4 fragColor;

uniform sampler2D Source;
uniform int Reach;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Source, 0 );
	float m    = 0.0;
	for( int j = -Reach; j <= Reach; ++j )
		for( int i = -Reach; i <= Reach; ++i )
			m = max( m, texelFetch( Source, clamp( p + ivec2( i, j ), ivec2( 0 ), size - 1 ), 0 ).r );
	fragColor = vec4( m, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// 8. The gather: scatter-as-gather. A pixel receives a sample's light if the
//    sample's disc reaches it, spread over the disc's area. What is in front
//    of the pixel lies over it with its coverage; what is at or behind it is
//    averaged, and may reach no further than the pixel's own disc, so a sharp
//    thing is never painted over by the blurred background behind it.
//---------------------------------------------------------------------------
const char* const kGatherFragment = R"(
out vec4 fragColor;

uniform sampler2D Prefilter;
uniform sampler2D Dilated;
uniform vec2 Spiral[ kSpiralCount ];
uniform float ToTexels;//pixels -> the prefilter's texels
uniform float MaxLod;

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	vec2 size = vec2( textureSize( Prefilter, 0 ) );
	vec4 c0   = texelFetch( Prefilter, p, 0 );
	float ca  = c0.a * ToTexels;
	float cc  = abs( ca );
	float G   = texelFetch( Dilated, p / 8, 0 ).r;
	if( G < 0.5 )
	{
		fragColor = vec4( c0.rgb, 0.0 );
		return;
	}
	//Each sample stands for this much of the disc, and is read from the mip
	//level that averages over it: nothing between two samples is missed.
	float area = kPi * G * G / float( kSpiralCount );
	float lod  = clamp( log2( sqrt( area ) ) - 0.5, 0.0, MaxLod );
	float wc   = 1.0 / ( kPi * max( cc * cc, 0.25 ) );
	vec3 back  = c0.rgb * wc;
	float backW = wc;
	vec3 front  = vec3( 0.0 );
	float frontW = 0.0;
	float ahead  = 1.0 + 0.05 * cc;
	for( int i = 0; i < kSpiralCount; ++i )
	{
		vec2 off = Spiral[ i ] * G;
		vec4 s   = textureLod( Prefilter, ( vec2( p ) + 0.5 + off ) / size, lod );
		float sa = s.a * ToTexels;
		float cs = abs( sa );
		float d  = length( off );
		if( sa > ca + ahead )
		{
			float w = clamp( cs - d + 0.5, 0.0, 1.0 ) * area / ( kPi * max( cs * cs, 0.25 ) );
			front += s.rgb * w;
			frontW += w;
		}
		else
		{
			float r = hooked( kHookBackSpread ) ? cs : min( cs, 2.0 * cc + 0.5 );
			float w = clamp( r - d + 0.5, 0.0, 1.0 ) * area / ( kPi * max( r * r, 0.25 ) );
			back += s.rgb * w;
			backW += w;
		}
	}
	vec3 b      = back / backW;
	float alpha = clamp( frontW, 0.0, 1.0 );
	vec3 f      = frontW > 0.0 ? front / frontW : b;
	//A sharp pixel is the composite's, at full resolution, and only what lies
	//in front of it comes from here: so for one, the colour is the front's
	//alone, and the composite lays it over the sharp pixel by alpha.
	float own = smoothstep( 0.75, 1.5, abs( c0.a ) );
	fragColor = vec4( mix( f, mix( b, f, alpha ), own ), alpha );
}
)";

//---------------------------------------------------------------------------
// 9. Sprites: each highlight block drawn as the aperture at its CoC, and the
//    orbs, which are the same sprite with no block behind it.
//---------------------------------------------------------------------------
const char* const kSpriteVertex = R"(
uniform sampler2D Highlights;
uniform ivec2 CellGrid;
uniform int CellLevel;   //the highlights' mip level drawn: a wide blur draws fewer, larger cells
uniform float CellAreaPx;//pixels a highlight block covers
uniform int SpriteKind;  //0 highlights, 1 orbs
uniform int BokehOnly;
uniform float BokehRadiusPx;
uniform vec3 FringeOffset;//each channel's focus, less green's, in nearness
uniform vec2 AreaUnit;    //the aperture's light at unit radius: behind, in front
uniform float MinEnergy;

uniform sampler2D Analysis;
uniform float OrbLod;
uniform float OrbGain;
uniform float OrbRadiusPx;
uniform float DriftPhase;  //drift periods so far, reduced to [0,1) in double
uniform float TwinklePhase;//twinkle cycles so far, reduced in double

flat out vec3 vEnergy;
flat out vec3 vRadius;
flat out vec3 vSide;
flat out vec2 vField;
flat out float vCoc;
out vec2 vLocal;

const vec2 kCorners[ 4 ] = vec2[ 4 ]( vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ), vec2( 1.0, 1.0 ) );

void main()
{
	vec2 corner = kCorners[ gl_VertexID ];
	vec2 at;
	vec3 energy, radius, side;
	float coc;
	bool dead = false;
	if( SpriteKind == 0 )
	{
		ivec2 cell = ivec2( gl_InstanceID % CellGrid.x, gl_InstanceID / CellGrid.x );
		vec4 h     = texelFetch( Highlights, cell, CellLevel );
		at         = ( vec2( cell ) + 0.5 ) / vec2( CellGrid );
		coc        = h.a;
		vec3 cocs;
		float scattered;
		if( BokehOnly != 0 )
		{
			coc       = -BokehRadiusPx;
			cocs      = vec3( coc );
			scattered = 1.0;
		}
		else
		{
			//Longitudinal chromatic aberration: each colour comes to focus at
			//its own nearness, so each has its own disc.
			vec3 off  = hooked( kHookFringeSwap ) ? -FringeOffset : FringeOffset;
			cocs      = vec3( coc ) - BlurPx * off;
			scattered = scatteredOf( coc );
		}
		vec3 e = h.rgb * scattered * CellAreaPx;
		radius = max( abs( cocs ), vec3( 1.0 ) );
		side   = mix( vec3( -1.0 ), vec3( 1.0 ), vec3( greaterThan( cocs, vec3( 0.0 ) ) ) );
		vec2 field = fieldOf( at );
		vec3 unit  = mix( vec3( AreaUnit.x ), vec3( AreaUnit.y ), vec3( greaterThan( side, vec3( 0.0 ) ) ) );
		//A defocused point's light, spread over its disc. A squeeze or a swirl
		//changes the disc's area and not the light; the barrel's shadow takes
		//light away, which is why this does not divide by it.
		energy = hooked( kHookNoEnergy ) ? e / CellAreaPx : e / ( unit * radius * radius * Squeeze * swirlOf( field ) );
		dead   = lumaOf( e ) < MinEnergy;
	}
	else
	{
		uint j      = uint( gl_InstanceID );
		uint h1     = pcg( j * 6u + 1u ), h2 = pcg( j * 6u + 2u ), h3 = pcg( j * 6u + 3u );
		uint h4     = pcg( j * 6u + 4u ), h5 = pcg( j * 6u + 5u ), h6 = pcg( j * 6u + 6u );
		//A whole number of picture widths and heights a drift period, so the
		//phase can be reduced on the CPU without a jump.
		vec2 speed  = vec2( float( int( h3 % 5u ) - 2 ), float( h4 % 3u + 1u ) );
		at          = fract( vec2( unitOf( h1 ), unitOf( h2 ) ) + DriftPhase * speed );
		float r     = OrbRadiusPx * ( 0.55 + 0.9 * unitOf( h5 ) );
		float tt    = TwinklePhase * ( 0.5 + unitOf( h6 ) ) + 64.0 * unitOf( h1 ^ h4 );
		float k     = floor( tt );
		float f     = tt - k;
		float n0    = unitOf( pcg( j * 977u + uint( k ) ) ), n1 = unitOf( pcg( j * 977u + uint( k ) + 1u ) );
		float glint = 0.55 + 0.45 * mix( n0, n1, f * f * ( 3.0 - 2.0 * f ) );
		//The scene's own colour there, as a light of that colour.
		vec3 c  = toLinear( textureLod( Analysis, at, OrbLod ).rgb );
		c       = c / max( max( c.r, max( c.g, c.b ) ), 0.05 );
		radius  = vec3( r );
		side    = vec3( -1.0 );
		//At infinity, so a sharp subject in front of them hides them.
		coc     = cocOf( 0.0, focusNearness() );
		energy  = c * OrbGain * glint;
	}
	float extent = max( radius.x, max( radius.y, radius.z ) ) * max( Squeeze, 1.0 ) + 2.0 * AaPx;
	vLocal       = corner * extent;
	vEnergy      = energy;
	vRadius      = radius;
	vSide        = side;
	vField       = fieldOf( at );
	vCoc         = coc;
	vec2 px      = at * Frame + vLocal;
	gl_Position  = dead ? vec4( 2.0, 2.0, 2.0, 1.0 ) : vec4( px / Frame * 2.0 - 1.0, 0.0, 1.0 );
}
)";

const char* const kSpriteFragment = R"(
in vec2 vLocal;
flat in vec3 vEnergy;
flat in vec3 vRadius;
flat in vec3 vSide;
flat in vec2 vField;
flat in float vCoc;
out vec4 fragColor;

uniform sampler2D Prefilter;
uniform int Occlude;

void main()
{
	vec3 light = vec3( irisLight( vLocal, vRadius.r, vSide.r, vField ), irisLight( vLocal, vRadius.g, vSide.g, vField ),
	                   irisLight( vLocal, vRadius.b, vSide.b, vField ) ) * vEnergy;
	if( Occlude != 0 && !hooked( kHookNoOcclusion ) )
	{
		//Something nearer than the sprite's point hides it, as much as it is
		//sharp: an in-focus subject in front of a background light is solid.
		float dest    = texture( Prefilter, gl_FragCoord.xy / vec2( textureSize( Prefilter, 0 ) ) ).a;
		float behind  = smoothstep( 1.0, 3.0, dest - vCoc );
		float through = clamp( abs( dest ) / max( abs( vCoc ), 1.0 ), 0.0, 1.0 );
		light *= 1.0 - behind * ( 1.0 - through );
	}
	fragColor = vec4( light, 0.0 );
}
)";

//---------------------------------------------------------------------------
// 10. The flare's lights: the brightest spot in each of 16x9 cells. Rows
//     0..8 hold ( uv, energy ), rows 9..17 the light's colour, summed.
//---------------------------------------------------------------------------
const char* const kLightsFragment = R"(
out vec4 fragColor;

uniform sampler2D Highlights;
uniform ivec2 LightGrid;
uniform float CellAreaPx;

void main()
{
	ivec2 cell     = ivec2( gl_FragCoord.xy );
	bool colourRow = cell.y >= LightGrid.y;
	if( colourRow )
		cell.y -= LightGrid.y;
	ivec2 size = textureSize( Highlights, 0 );
	ivec2 lo   = cell * size / LightGrid;
	ivec2 hi   = max( ( cell + 1 ) * size / LightGrid, lo + 1 );
	float best = 0.0;
	ivec2 at   = lo;
	for( int y = lo.y; y < hi.y; ++y )
		for( int x = lo.x; x < hi.x; ++x )
		{
			float e = lumaOf( texelFetch( Highlights, ivec2( x, y ), 0 ).rgb );
			if( e > best )
			{
				best = e;
				at   = ivec2( x, y );
			}
		}
	//A light is a peak: the brightest texel for seven round it, and brighter
	//than its surroundings. A wide bright area (a sky, a lit wall) throws
	//veiling glare, not ghosts, and a light on a cell's edge must not be
	//found again by the cell next door.
	vec3 sum      = vec3( 0.0 );
	vec2 weighted = vec2( 0.0 );
	float inner = 0.0, outer = 0.0;
	bool peak   = best > 0.0;
	for( int j = -7; j <= 7; ++j )
		for( int i = -7; i <= 7; ++i )
		{
			ivec2 q = at + ivec2( i, j );
			if( q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y )
				continue;
			vec3 c  = texelFetch( Highlights, q, 0 ).rgb;
			float e = lumaOf( c );
			if( e > best )
				peak = false;
			if( abs( i ) <= 3 && abs( j ) <= 3 )
			{
				sum += c;
				inner += e;
				weighted += e * ( vec2( q ) + 0.5 );
			}
			else
				outer += e;
		}
	float compact = peak ? clamp( 1.0 - ( outer / 176.0 ) / max( inner / 49.0, 1e-6 ), 0.0, 1.0 ) : 0.0;
	vec2 where    = ( inner > 0.0 ? weighted / inner : vec2( at ) + 0.5 ) / vec2( size );
	float total   = inner * compact;
	sum *= compact;
	fragColor  = colourRow ? vec4( sum * CellAreaPx, 0.0 ) : vec4( where, total * CellAreaPx, 0.0 );
}
)";

const char* const kFlareVertex = R"(
uniform sampler2D Lights;
uniform ivec2 LightGrid;
uniform int LightMode;  //0 the picture's highlights, 1 Light X/Y, 2 the tracked point
uniform vec2 LightPos;
uniform vec3 LightColour;
uniform float LightEnergy;
uniform float MinLight;
uniform float FlareGain;
uniform float GhostGain;
uniform float HaloGain;
uniform float StarGain;
uniform float StreakGain;
uniform float GhostAt[ kGhostCount ];
uniform float GhostRadius[ kGhostCount ];
uniform float GhostReflect[ kGhostCount ];
uniform vec3 GhostTint[ kGhostCount ];
uniform float GhostSoft[ kGhostCount ];
uniform float AaPxFlare;

flat out int vKind;
flat out vec3 vColour;
flat out float vRadius;
flat out float vSoft;
out vec2 vLocal;

const vec2 kCorners[ 4 ] = vec2[ 4 ]( vec2( -1.0, -1.0 ), vec2( 1.0, -1.0 ), vec2( -1.0, 1.0 ), vec2( 1.0, 1.0 ) );
const int kElements = 10;

//How much of a light each element sends: a few percent, as a coated lens's
//surfaces reflect.
const float kGhostLight  = 0.02;
const float kHaloLight   = 0.01;
const float kStarLight   = 0.004;
const float kStreakLight = 0.04;
const float kGlowLight   = 0.05;

void main()
{
	vec2 corner = kCorners[ gl_VertexID ];
	int light   = gl_InstanceID / kElements;
	int element = gl_InstanceID % kElements;
	vec2 L;
	vec3 colour;
	float energy;
	bool dead = false;
	if( LightMode == 0 )
	{
		ivec2 c = ivec2( light % LightGrid.x, light / LightGrid.x );
		vec4 a  = texelFetch( Lights, c, 0 );
		L       = a.xy;
		energy  = a.z;
		colour  = texelFetch( Lights, c + ivec2( 0, LightGrid.y ), 0 ).rgb / max( energy, 1e-6 );
		dead    = element == 9;//a light the picture has needs no glow drawn
	}
	else
	{
		L      = LightMode == 1 ? LightPos : trackedPoint();
		energy = LightEnergy;
		colour = LightColour;
		dead   = light > 0;
	}
	dead = dead || energy < MinLight;

	vec2 Lpx  = L * Frame;
	vec2 Cpx  = 0.5 * Frame;
	vec2 centre;
	vec2 extent;
	float gain = energy * FlareGain;
	vSoft      = 0.0;
	if( element < kGhostCount )
	{
		//The ghosts: the aperture, on the line from the light through the
		//centre, moving across the frame against the light.
		vec2 axis = Lpx - Cpx;
		centre    = Cpx + axis * GhostAt[ element ];
		if( hooked( kHookGhostOffAxis ) )
			centre += vec2( -axis.y, axis.x ) * 0.15;
		float r = GhostRadius[ element ] * Frame.y;
		vColour = colour * GhostTint[ element ] * ( gain * GhostGain * GhostReflect[ element ] * kGhostLight / ( kPi * r * r ) );
		vRadius = r;
		vSoft   = GhostSoft[ element ];
		vKind   = 0;
		extent  = vec2( r * 1.08 + 2.0 * AaPxFlare );
	}
	else if( element == 6 )
	{
		float r = 0.11 * Frame.y;
		vColour = colour * ( gain * HaloGain * kHaloLight / ( 2.0 * kPi * r * 0.06 * r * 1.7725 ) );
		vRadius = r;
		vKind   = 1;
		extent  = vec2( r * 1.25 );
	}
	else if( element == 7 )
	{
		float len = 0.25 * Frame.y * clamp( sqrt( energy / ( 2.0e-3 * Frame.y * Frame.y ) ), 0.35, 2.0 );
		vColour   = colour * ( gain * StarGain * kStarLight / ( len * 2.0 ) );
		vRadius   = len;
		vKind     = 2;
		extent    = vec2( len );
	}
	else if( element == 8 )
	{
		float len = 0.6 * Frame.y;
		vColour   = colour * ( gain * StreakGain * kStreakLight / ( len * 4.0 ) );
		vRadius   = len;
		vKind     = 3;
		extent    = vec2( len, 0.012 * Frame.y + 4.0 );
	}
	else
	{
		float r = 0.025 * Frame.y;
		vColour = colour * ( gain * kGlowLight / ( kPi * r * r ) );
		vRadius = r;
		vKind   = 4;
		extent  = vec2( 4.0 * r );
	}
	vLocal      = corner * extent;
	vec2 px     = Lpx + ( centre - Lpx ) * float( element < kGhostCount ) + vLocal;
	gl_Position = dead ? vec4( 2.0, 2.0, 2.0, 1.0 ) : vec4( px / Frame * 2.0 - 1.0, 0.0, 1.0 );
}
)";

const char* const kFlareFragment = R"(
in vec2 vLocal;
flat in int vKind;
flat in vec3 vColour;
flat in float vRadius;
flat in float vSoft;
out vec4 fragColor;

//A ghost's colours land at slightly different sizes: the lens disperses.
const float kGhostDispersion = 0.04;

float ghostOf( vec2 q, float r, float soft )
{
	float sd = shapeSd( irisFrame( q / r ) );
	return clamp( 0.5 - sd * r / max( AaPx, soft * r ), 0.0, 1.0 );
}

void main()
{
	vec3 light = vec3( 0.0 );
	if( vKind == 0 )
	{
		for( int c = 0; c < 3; ++c )
			light[ c ] = ghostOf( vLocal * ( 1.0 + kGhostDispersion * ( float( c ) - 1.0 ) ), vRadius, vSoft );
		light *= vColour;
	}
	else if( vKind == 1 )
	{
		float rho = length( vLocal );
		for( int c = 0; c < 3; ++c )
		{
			float rc   = vRadius * ( 1.0 + 0.03 * ( float( c ) - 1.0 ) );
			float x    = ( rho - rc ) / ( 0.06 * vRadius );
			light[ c ] = exp( -x * x );
		}
		light *= vColour;
	}
	else if( vKind == 2 )
	{
		//Diffraction off each straight blade edge throws light at right angles
		//to it, both ways: a line through the light per edge. Opposite edges of
		//an even iris are parallel and share a line, so N blades give N spikes
		//when N is even and 2N when it is odd.
		vec2 q     = irisFrame( vLocal );
		float rho  = length( q );
		float fall = max( 1.0 - rho / vRadius, 0.0 );
		fall *= fall;
		//No narrower than a texel of the buffer it is drawn into, or a spike
		//falls between two texels' centres and is lost.
		float w = max( 1.2 + 0.006 * rho, AaPx );
		float s = 0.0;
		for( int k = 0; k < kMaxBlades; ++k )
		{
			if( k >= Blades )
				break;
			vec2 n       = EdgeNormal[ k ];
			float across = q.x * n.y - q.y * n.x;
			if( hooked( kHookSpikePerEdge ) && dot( q, n ) < 0.0 )
				continue;
			s += exp( -across * across / ( w * w ) );
		}
		light = vColour * s * fall / w;
	}
	else if( vKind == 3 )
	{
		float x = abs( vLocal.x ) / vRadius;
		float y = vLocal.y / ( 0.004 * vRadius + 1.5 );
		float f = max( 1.0 - x, 0.0 );
		light   = vColour * vec3( 0.55, 0.75, 1.0 ) * exp( -y * y ) * f * f / ( 0.004 * vRadius + 1.5 );
	}
	else
	{
		float r2 = dot( vLocal, vLocal ) / ( vRadius * vRadius );
		light    = vColour * ( 0.5 * exp( -4.0 * r2 ) + 2.0 * exp( -40.0 * r2 ) );
	}
	fragColor = vec4( light, 0.0 );
}
)";

//---------------------------------------------------------------------------
// 11. The composite, into the host's framebuffer.
//---------------------------------------------------------------------------
const char* const kCompositeFragment = R"(
out vec4 fragColor;

uniform sampler2D Blurred;
uniform sampler2D Bokeh;
uniform sampler2D Prefilter;
uniform sampler2D Highlights;
uniform int ModeIndex;  //0 Depth of Field, 1 Bokeh Only
uniform int ShowIndex;
uniform float MixAmount;
uniform ivec2 ViewOrigin;
uniform vec2 ViewSize;
uniform int Exact;      //1: the output pixel IS an input pixel (no rect, same size)
uniform float DirtGain;
uniform float GlareLod;
uniform int LightMode;
uniform vec2 LightPos;
uniform vec3 LightColour;
uniform float LightEnergyGlare;
uniform float TrackHeights;

//Lens dirt: specks and smudges on the front element, which only show where
//light falls on them.
float dirtAt( vec2 at )
{
	vec2 p  = heights( at ) * 9.0 + 40.0;
	float d = 0.0;
	for( int octave = 0; octave < 2; ++octave )
	{
		vec2 q     = p * ( octave == 0 ? 1.0 : 2.3 );
		ivec2 cell = ivec2( floor( q ) );
		for( int j = -1; j <= 1; ++j )
			for( int i = -1; i <= 1; ++i )
			{
				ivec2 c   = cell + ivec2( i, j );
				uint h    = pcg( uint( c.x ) * 73856093u ^ uint( c.y ) * 19349663u ^ uint( octave ) * 83492791u );
				vec2 spot = vec2( c ) + vec2( unitOf( h ), unitOf( pcg( h ) ) );
				float r   = 0.15 + 0.35 * unitOf( pcg( h + 1u ) );
				float a   = unitOf( pcg( h + 2u ) );
				if( a > 0.55 )
					d += ( a - 0.55 ) * 2.2 * ( 1.0 - smoothstep( 0.4 * r, r, length( q - spot ) ) );
			}
	}
	return d;
}

vec3 glareAt( vec2 at )
{
	vec3 g = 4.0 * textureLod( Highlights, at, GlareLod ).rgb;
	if( LightMode != 0 )
	{
		vec2 L = LightMode == 1 ? LightPos : trackedPoint();
		vec2 d = heights( at ) - heights( L );
		g += LightColour * LightEnergyGlare * exp( -dot( d, d ) / 0.045 );
	}
	return g;
}

void main()
{
	//From the fragment's own position, not the quad's interpolated uv: a
	//software rasteriser's varyings are a thousandth of a pixel off, which a
	//steep depth plane turns into 1e-5 of nearness.
	vec2 at  = ( gl_FragCoord.xy - vec2( ViewOrigin ) ) / ViewSize;
	vec4 src = Exact != 0 ? texelFetch( Picture, ivec2( gl_FragCoord.xy ) - ViewOrigin, 0 ) : picture( at );
	if( MixAmount <= 0.0 )
	{
		fragColor = src;
		return;
	}
	vec3 lin = toLinear( src.rgb );
	vec3 col = lin;
	float t  = 0.0;
	if( ModeIndex == 0 )
	{
		vec4 b    = texture( Blurred, at );
		float own = smoothstep( 0.75, 1.5, abs( texture( Prefilter, at ).a ) );
		t         = max( own, b.a );
		col       = mix( mix( lin, b.rgb, b.a ), b.rgb, own );
	}
	vec3 add = texture( Bokeh, at ).rgb;
	if( DirtGain > 0.0 )
		add += DirtGain * dirtAt( at ) * glareAt( at );

	//Where nothing was blurred and nothing added, the clip, bit for bit.
	bool untouched = t == 0.0 && add == vec3( 0.0 ) && !hooked( kHookRoundTrip );
	vec3 outRgb    = untouched ? src.rgb : toCode( col + add );

	if( ShowIndex == 1 )
	{
		float n  = nearnessAt( at );
		float nf = focusNearness();
		outRgb   = vec3( n );
		if( abs( n - nf ) < 0.004 )
			outRgb = mix( outRgb, vec3( 1.0, 0.55, 0.1 ), 0.6 );
	}
	else if( ShowIndex == 2 )
	{
		float coc  = cocOf( nearnessAt( at ), focusNearness() );
		float most = max( BlurPx, 1.0 );
		outRgb     = vec3( clamp( coc / most, 0.0, 1.0 ), 0.6 * ( 1.0 - smoothstep( 0.75, 1.5, abs( coc ) ) ),
                       clamp( -coc / most, 0.0, 1.0 ) );
	}
	else if( ShowIndex == 3 )
	{
		vec2 px   = at * Frame;
		vec2 c    = focusPoint() * Frame;
		float hs  = 0.5 * TrackHeights * Frame.y;
		vec2 dd   = abs( px - c );
		float box = step( max( dd.x, dd.y ), hs + 1.5 ) * step( hs - 1.5, max( dd.x, dd.y ) );
		float conf = TrackActive != 0 ? texelFetch( TrackState, ivec2( 1, 0 ), 0 ).x : 1.0;
		vec3 ink   = conf >= 0.7 ? vec3( 0.2, 1.0, 0.3 ) : ( conf >= 0.5 ? vec3( 1.0, 0.75, 0.1 ) : vec3( 1.0, 0.2, 0.15 ) );
		vec2 p0    = abs( px - FocusPoint * Frame );
		float cross = step( min( p0.x, p0.y ), 0.75 ) * step( max( p0.x, p0.y ), 12.0 );
		outRgb      = mix( outRgb, ink, box );
		outRgb      = mix( outRgb, vec3( 1.0 ), cross );
	}
	//At Mix 1 the effect exactly: mix( a, b, 1 ) may be a + ( b - a ), an ulp
	//from b (a software rasteriser does just that).
	fragColor = MixAmount >= 1.0 ? vec4( outRgb, 1.0 ) : vec4( mix( src.rgb, outRgb, MixAmount ), mix( src.a, 1.0, MixAmount ) );
}
)";

//---------------------------------------------------------------------------
Program Assemble( Pass pass )
{
	const std::string v = kVersion;
	const std::string quad = v + kQuadVertex;
	const std::string common = v + kCommon;
	switch( pass )
	{
	case Pass::Analysis: return { quad, common + kAnalysisFragment };
	case Pass::Dark: return { quad, common + kDarkFragment };
	case Pass::Reduce: return { quad, common + kReduceFragment };
	case Pass::Histogram: return { common + kHistogramVertex, v + kHistogramFragment };
	case Pass::RawDepth: return { quad, common + kScene + kRawDepthFragment };
	case Pass::GuideRow: return { quad, common + kGuideRowFragment };
	case Pass::Box: return { quad, common + kBoxFragment };
	case Pass::GuideCoef: return { quad, common + kGuideCoefFragment };
	case Pass::Template: return { quad, common + kTemplateFragment };
	case Pass::Score: return { quad, common + kScoreFragment };
	case Pass::Pick: return { quad, common + kPickFragment };
	case Pass::Focus: return { quad, common + kScene + kFocusFragment };
	case Pass::Prefilter: return { quad, common + kScene + kPrefilterFragment };
	case Pass::Highlight: return { quad, common + kScene + kHighlightFragment };
	case Pass::Tile: return { quad, common + kTileFragment };
	case Pass::Dilate: return { quad, common + kDilateFragment };
	case Pass::Gather: return { quad, common + kGatherFragment };
	case Pass::Sprite: return { common + kScene + kIris + kSpriteVertex, common + kScene + kIris + kSpriteFragment };
	case Pass::Lights: return { quad, common + kLightsFragment };
	case Pass::Flare: return { common + kScene + kIris + kFlareVertex, common + kScene + kIris + kFlareFragment };
	case Pass::Composite: return { quad, common + kScene + kCompositeFragment };
	case Pass::Count: break;
	}
	return {};
}

const char* NameOf( Pass pass )
{
	static const char* const names[] = { "analysis", "dark", "reduce", "histogram", "raw", "guiderow", "box", "guidecoef",
		                                 "template", "score", "pick", "focus", "prefilter", "highlight", "tile", "dilate",
		                                 "gather", "sprite", "lights", "flare", "composite" };
	const int i = static_cast< int >( pass );
	return i >= 0 && i < static_cast< int >( Pass::Count ) ? names[ i ] : "?";
}

} // namespace hyacinth::shaders
