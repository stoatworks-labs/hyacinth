#pragma once

#include <string>

/**
	The passes, in the order a frame runs them.

	**What is where.** Lengths are full-resolution PIXELS in the CoC and the
	sprites, picture HEIGHTS in the geometry. Depth is nearness n (1 near, 0 at
	infinity). Every work buffer covers the whole picture, so a buffer is read
	at the picture's own uv whatever its size; a buffer's texel is
	`Frame / size` pixels.

	1. **analysis** (quarter): the picture, 4x4 pixels a texel, code values,
	   luma in alpha, mipmapped. The tracker and the depth estimators read it.
	2. **tracker** (only while something is tracked): **template** (24x24),
	   **score** over a grid of candidate offsets (zero-mean normalised
	   cross-correlation of luma, less a chroma penalty), **pick** the best
	   (coarse, then fine, with a parabola through the fine peak), which writes
	   the 2x1 track state: position, velocity, confidence.
	3. **depth** (Luma, Haze, Subject only): **dark** (the dark channel, two
	   separable minimum passes) and **reduce** (its maximum: the air light) for
	   Haze; **histogram** (colour histograms of the subject and of the ring
	   round it, splatted as points) for Subject; then **raw** (the estimate)
	   and the **colour guided filter** (He, Sun and Tang 2010): four rows
	   (the thirteen window sums, made and summed along the row), four
	   columns, coefficients (a 3x3 solve), box, box. The coefficients are
	   read at full resolution against the full picture's colour (their
	   "fast guided filter"), so the depth's edges are the picture's.
	4. **focus** (1x1): the focus nearness, eased toward its target.
	5. **prefilter** (half, mipmapped): linear light with the highlights
	   restored, less what the sprites will scatter; the signed CoC in alpha.
	6. **highlights** (quarter, mipmapped): what each 4x4 block has above
	   Threshold, and its CoC.
	7. **tiles** and **dilate**: the largest CoC near each 8x8 tile.
	8. **gather** (half): the defocus, scatter-as-gather over a 96-sample
	   spiral, split into what is in front of the pixel and what is behind it.
	9. **sprites** (half, additive): one sprite per highlight block, drawn as
	   the aperture at its CoC; then the orbs, the same sprite.
	10. **lights** (16x9) and **flare** (half, additive): the brightest spot in
	   each of 144 cells, and per light its ghosts, halo, starburst, streak.
	11. **composite** (the host's framebuffer).

	No sin, cos, tan or atan anywhere in the GLSL (GLSL promises nothing
	about their accuracy). Directions come from the CPU.
*/
namespace hyacinth::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kScene;
extern const char* const kIris;
extern const char* const kAnalysisFragment;
extern const char* const kDarkFragment;
extern const char* const kReduceFragment;
extern const char* const kHistogramVertex;
extern const char* const kHistogramFragment;
extern const char* const kRawDepthFragment;
extern const char* const kGuideRowFragment;
extern const char* const kBoxFragment;
extern const char* const kGuideCoefFragment;
extern const char* const kTemplateFragment;
extern const char* const kScoreFragment;
extern const char* const kPickFragment;
extern const char* const kFocusFragment;
extern const char* const kPrefilterFragment;
extern const char* const kHighlightFragment;
extern const char* const kTileFragment;
extern const char* const kDilateFragment;
extern const char* const kGatherFragment;
extern const char* const kSpriteVertex;
extern const char* const kSpriteFragment;
extern const char* const kLightsFragment;
extern const char* const kFlareVertex;
extern const char* const kFlareFragment;
extern const char* const kCompositeFragment;

enum class Pass
{
	Analysis,
	Dark,
	Reduce,
	Histogram,
	RawDepth,
	GuideRow,
	Box,
	GuideCoef,
	Template,
	Score,
	Pick,
	Focus,
	Prefilter,
	Highlight,
	Tile,
	Dilate,
	Gather,
	Sprite,
	Lights,
	Flare,
	Composite,
	Count
};

/// A pass's two stages, assembled from the pieces above. The one place the
/// order is written; tools/glslc.sh's ASSEMBLED table mirrors it.
struct Program
{
	std::string vertex;
	std::string fragment;
};
Program Assemble( Pass pass );
const char* NameOf( Pass pass );

//---------------------------------------------------------------------------
// Sizes. kCommon repeats every one of these.
//---------------------------------------------------------------------------
constexpr int kTemplateSide = 24;  ///< the tracked patch, texels a side
constexpr int kCoarseGrid   = 17;  ///< candidates a side, coarse
constexpr double kCoarseStep = 2.0;///< template texels between coarse candidates
constexpr int kFineGrid     = 9;
constexpr double kFineStep  = 0.5;
constexpr int kHistogramBins = 512;///< 8 x 8 x 8 RGB, plus a column for the count
constexpr int kHistogramSide = 32; ///< samples a side over the subject's box
constexpr int kTileSize     = 8;   ///< half-resolution texels a tile
constexpr int kLightsX      = 16;
constexpr int kLightsY      = 9;
constexpr int kFlareElements = 10;  ///< six ghosts, halo, starburst, streak, glow

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model.
enum Hook : int
{
	kHookLinearDepth  = 1 << 0, ///< the CoC from distance, not nearness
	kHookNoEnergy     = 1 << 1, ///< a disc as bright as its point whatever its size
	kHookCatEyeKeeps  = 1 << 2, ///< the barrel's shadow brightens what is left instead of losing light
	kHookNoFlip       = 1 << 3, ///< bokeh in front of focus the same way up as behind
	kHookSpikePerEdge = 1 << 4, ///< one diffraction spike per blade edge, not a line through the light
	kHookGhostOffAxis = 1 << 5, ///< the ghosts pushed off the line through the light and the centre
	kHookFringeSwap   = 1 << 6, ///< green in focus nearer than red and blue, not farther
	kHookNoOcclusion  = 1 << 7, ///< a sharp foreground does not hide the bokeh behind it
	kHookBackSpread   = 1 << 8, ///< the background's blur spreads over a sharp foreground
	kHookRoundTrip    = 1 << 9, ///< no exact passthrough: every pixel through linear light and back
	kHookNoSubTexel   = 1 << 10,///< the tracker's peak taken at the best candidate, no parabola
	kHookFocusLinear  = 1 << 11,///< the focus pulled at a constant rate, not eased
	kHookDepthNoRect  = 1 << 12,///< Side by Side's depth read from the picture's half
	kHookNoGuide      = 1 << 13 ///< the estimated depth used raw, not guided by the picture
};

} // namespace hyacinth::shaders
