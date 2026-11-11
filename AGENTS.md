# AGENTS.md: Hyacinth

Read this before you change the lens model, the aperture, the guided filter, the tracker, a tolerance in the harness or a default.

## The idea

**A thin lens focused at one distance blurs a point at another into a disc whose radius is linear in the difference of their NEARNESSES, not their distances.** A point at distance *d*, the lens focused at *s*, blurs to a circle of diameter *A f* |*s* − *d*| / (*d* (*s* − *f*)) = (*A f s* / (*s* − *f*)) · |1/*s* − 1/*d*|. So the plugin keeps depth as nearness *n* (1 near, 0 at infinity) and every circle of confusion is

    CoC = Blur Size × (n − n_focus)      pixels; positive in front of focus, negative behind

Everything the plugin draws comes from that line, a depth for every pixel, and one aperture:

1. **A depth for every pixel** (Depth From):
   - **Plane:** the ground, a wall, a table. Seen through a pinhole, a plane's nearness is affine across the picture (1/*z* is linear in screen position for a plane). It is 1 at the corner nearest along Plane Angle and 0 on the horizon.
   - **Radial:** 1 at the focus point, falling to 0 a Radius away.
   - **Luma**, **Clip Alpha**, and a depth map packed **Side by Side** or **Over Under** with the picture: read as they are.
   - **Haze:** the dark channel prior (He, Sun and Tang 2009). A haze-free picture's darkest channel over a patch is about 0, so the haze's transmission is *t* = 1 − dark/*A*, *A* the air light; *t* = e^(−βd), so −ln *t* is distance.
   - **Subject:** colour histograms of the box round the focus point and of the ring round that. A colour likelier in the box than in the ring is near; a Gaussian falloff from the focus point keeps a matching colour far away from becoming near.
   - Haze and Subject are judged at a quarter of the resolution, then put on the picture's own edges by the **colour guided filter** (He, Sun and Tang 2010, §3.5): in every window the depth is a least-squares linear function of the colour, p = a·I + b, with a ridge ε = 1e-3. The coefficients are computed at quarter resolution and applied to the full-resolution picture ("fast guided filter"). Each texel's weight is the raw estimate's evidence (below).
2. **A focus distance** (Focus): Manual (Focus Depth), Point (the depth at Focus X, Focus Y), or Track (the depth at whatever was there when grabbed, followed). The focus eases to its target as exp(−*t*/τ), τ = Focus Speed, in seconds of the host's clock.
3. **The tracker:** a 24×24 template of the analysis buffer's luma and colour, matched by zero-mean normalised cross-correlation less a chroma penalty, over a 17×17 coarse grid (2 template texels apart), then a 9×9 fine grid (half a texel apart) with a parabola through the peak each way. It learns the subject's new look slowly (5% a frame) and only while the match is good (confidence over 0.7). Below 0.5 it is lost: it holds where it was, and its prediction of the subject's motion decays.
4. **One aperture.** `shapeSd()` (GLSL) and `ShapeSd()` (`Iris.cpp`) are the same function: N straight blades rounded toward a circle by Blade Curve, a catadioptric mirror's ring, a star or a heart. It is turned by Iris Rotate, and in front of focus it is drawn upside down (the cone has not crossed yet).
   - **Cat's eye:** off axis the light has to pass two round openings that no longer line up, so the disc is their overlap. What they cut off is lost: that is mechanical vignetting.
   - **Swirl** shortens the radial axis toward the corners (field curvature); **Squeeze** is an anamorphic lens's vertical stretch. Both change the disc's area and not its light.
   - **Bubble** (spherical aberration) and **Onion Rings** (an aspheric's polishing marks) are profiles in ρ² with zero mean over the disc: they move light within it, never in or out. Bubble brightens the rim behind focus and the middle in front.
   - **Fringe** (longitudinal chromatic aberration) focuses red and blue at a nearness Fringe away from green: offsets (−½, +½, −½)·δ, so in front of focus the rim is magenta and behind it green.
5. **The defocus** (Depth of Field): scatter-as-gather over a 96-sample spiral, at half resolution, with the largest CoC near each 8×8 tile dilated so a big disc can reach a pixel. What is in front of a pixel and what is behind it are gathered apart, so a sharp thing hides a blurred background and a blurred foreground spreads over a sharp one.
6. **The highlights:** the clip's code values clip at white, so what was brighter than white is gone. Highlights restores it as a gain up to 32 on the brightest code values (max channel, so a saturated light counts), and what is above Threshold in each 4×4 block is drawn as one **sprite**: the aperture at that block's CoC, carrying exactly that light. Under 6 px of CoC a highlight is handed to the gather, wholly under 3 px.
7. **The flare** is the same aperture again. The brightest compact spot in each of 16×9 cells (non-maximum suppression over 15×15 texels, compactness 1 − outer/inner mean) gets six ghosts on the line through the light and the picture's centre, a halo, a starburst, an anamorphic streak and a glow. A starburst has one spike along each blade edge's normal, both ways: N spikes for even N, 2N for odd (an aperture's Fraunhofer pattern is centrosymmetric). **Orbs** are the aperture at random places, lit by the scene's colour there, drifting. **Dirt** is specks on the front element, lit where light falls on them.

## Looked at, nothing taken

- **He, Sun and Tang** (dark channel 2009, guided filter 2010) and **He and Sun's fast guided filter** (2015): papers, implemented from the equations.
- **Shimizu and Okutomi** (2005) for the parabola's sub-pixel bias, used only for the tracker check's bound.
- No code, shader, asset or number came from any other bokeh or flare implementation. The ghost table is placed and coloured by eye from photographs of real flares (`Iris.cpp`, `Ghosts()`).

## Decisions taken without asking

- **Name and id.** `SW Hyacinth`, `HY01`, bundle `com.stoatworks.ffgl.hyacinth`, harness `hytest`. HY01 was checked free against the fleet's ids in `~/Projects/*/*/source` and `~/dev`. Built in `~/dev/hyacinth`, then moved to `~/Projects/resolume/hyacinth` and made public as `stoatworks-labs/hyacinth` (2026-11-11).
- **Beside gaffer and tilter, not instead of them.** gaffer is a lens on a depth map the clip already has (Radial, Luma, Alpha, split halves), racked by the music through a focus puller and a rattling rig. tilter is a tilt-shift lens: a plane of focus across the frame. Hyacinth's own ground is everything else:
  - depth estimated from the picture (a plane to a horizon, haze, the subject's colours), snapped to its edges
  - focus that follows a tracked object
  - the highlights as the aperture with their light conserved, and Bokeh Only, bokeh with nothing defocused
  - the aperture's character: mirror, star, heart, cat's eye, swirl, squeeze, bubble, onion rings, fringe
  - lens flare, orbs and dirt from the same aperture

  No code came from either. The thin lens's CoC is the textbook one all three share.
- **Depth is nearness throughout,** because the thin lens is linear in it. A negative control (`--negative`, "the CoC from distance") shows `--coc` can tell the difference.
- **No neural depth.** A monocular depth network (MiDaS, Depth Anything and their kind) is the obvious way to get real depth from any clip. It is out of scope for an FFGL plugin at 0.1.0: model size, inference outside GL, and each model's own licence. The route for real depth today is a depth map made elsewhere, packed Side by Side or Over Under with the picture. That is the follow-up to weigh, not a hole.
- **Side by Side and Over Under output the picture half, stretched to the whole frame.** The packed depth half is input only. **Mix 0 on a packed clip** therefore gives the stretched picture half, not the packed clip as it came in, and the bit-exact passthrough is only claimed for unpacked clips.
- **Output alpha is mix(clip alpha, 1, Mix).** Defocused light spreads past the clip's alpha edges, and keeping the clip's alpha would cut it there. At Mix 0 the clip's alpha comes through untouched.
- **Orbs are at infinity.** They are far lights out of focus, not dust on the lens, so a sharp subject in front of them hides them. Their size is Orb Size, not Blur Size, so they stay where the user puts them as the focus moves.
- **The twinkle's phase wraps every ~45 minutes** (4096 cycles at 1.5 Hz, reduced in double on the CPU): one jump in the orbs' twinkle per 45 minutes of running. The drift's phase is reduced over a whole number of picture widths and does not jump.
- **The flare's ghost table and the elements' energies are taste,** stated as taste in `Iris.cpp`. What is physics is the line the ghosts lie on, their shape (the aperture), that they move with the light, and the spike count.
- **The streak sends 4% of a light's energy** (`kStreakLight`). At first it sent 0.3% and was invisible at Streak 1. A real anamorphic flare is a few percent, and the control should do something at its top.
- **The guided filter is the colour one, weighted** (see the traps). The luma version left an edge between two colours of similar luma soft over the whole window.
- **Default Blur Size is 3% of the height per unit of nearness.** At 1080p the street's background lights, with focus on the ball, are 28 px discs. At 320×180 the same discs are 2 px and handed to the gather, so the sweep sets Blur Size to its top for the aperture's controls.
- **The user guide** (`docs/USER-GUIDE.md`) was written when the repo landed in the fleet (2026-11-11), not with the build. The About block, generated from the website's entry, has four buttons (User guide, Project page, Source on GitHub, Support the work), so the plugin has 47 parameters.

## The traps actually hit

- **`centroid` is a GLSL keyword** (an interpolation qualifier). A local named `centroid` stopped the lights pass compiling at all, on Apple's driver and in glslc. `verify.sh`'s reserved-word grep now has it, with `invariant`, `buffer`, `shared` and the memory qualifiers.
- **The gather owns a share of every small highlight.** The energy check read the sprites' buffer and found a front-of-focus disc 10–13% dim at 320×180. That was the gather's share: its CoC was 5.4 px, so `scatteredOf()` gave the sprite 90%. The energy cases now all use 0.05 H discs (9 px at 180p), wholly the sprites'.
- **A profile carried past the rim adds light the area does not count.** The edge's anti-aliasing ramp reaches past ρ = 1, and Bubble's (2ρ² − 1) kept growing there. Both sides now clamp ρ² at 1 (the rim's value). The GLSL and `Iris.cpp` must agree, or the energy normalisation is off.
- **Onion rings finer than the buffer alias.** Bubble +0.8 with Onion 0.7 lost 13% of a 9 px disc's light. A ring at the rim is r/(2·6) wide, so the rings now fade in from where that is one texel to where it is two.
- **A spike centred between two texel rows has a flat top two texels wide,** and rounding on it reads as two maxima. Axis-aligned spikes (6 blades at 0°, 7 at 90°) counted 8 and 16. The starburst check now counts runs above the ring's middle, not local maxima.
- **Show Depth tints the focal plane, and the plane check skips tinted pixels.** An inverted plane's sky is nearness 1, inside the tint at Focus Depth 0.999. The inverted case focuses at 0.001 instead.
- **A luma guide cannot see a colour edge.** Subject's red square on a blue ground differ by 0.09 in luma. The luma guided filter left the depth's edge 18 px wide at 720p (bound 2). The colour guided filter's 3×3 covariance fixed it to 8 px.
- **An edge texel's colour is in neither histogram.** A quarter-resolution texel across the square's edge averages red and blue into a purple that neither the subject's histogram nor the ring's holds, so its raw nearness was 0: a seam of zeros along every edge. The least-squares fit then bent round it, an undershoot to 0.14 beside a 0.30 ground. Weighting each texel by its colour's evidence, (pf + pb)/(pf + pb + 0.02), lets the colour model fill the seam in. The edge is now 2 px.
- **The tracker cannot be finer than the analysis buffer.** At 320×180 a template texel is 1.1 px, a quarter of an analysis texel. The template only resamples it, and the match error (0.13 px RMS) is set by the analysis texel, not the fine step. The track check's step is the larger of the two.
- **Apple's software renderer interpolates varyings about a thousandth of a pixel off.** A plane at 0° (2 units of nearness per picture width) came out 8e-6 off the pinhole's line there, against 1e-7 on the GPU. The composite now computes its position from `gl_FragCoord`, which is exact on both.
- **Apple's software renderer computes mix(a, b, 1) as a + (b − a),** an ulp from b per channel. The depth view stopped being exactly grey, and the check's grey test rejected a third of the picture. At Mix 1 the composite writes the effect as it is.
- **A wide bound hides a mutant.** The first mutant run (at 320×180 only) let three mutants through: the cat's eye a fifth too far apart, the blades a tenth of a step round, and green focused twice as far. Each was inside 320×180's bound and far outside 1280×720's. Mutants now run their check at both rasters.
- **An equivalent mutant.** Mirroring the spikes (`across = q.x n.y + q.y n.x`) changed nothing: a regular iris with a vertex straight up is symmetric about both axes. It was replaced by one that miscounts the star's edges.
- **A check that does not pin a least-squares fit passes a wrong one.** With the covariance's inverse given a wrong cofactor, the Subject edge still passed: any coefficients that separate two colours give a step. `--depth` now applies the plugin's own read-back coefficients to Luma, an exact linear function of the colour. The fit must return it, less the ridge's shrinkage.
- **Allocate every buffer before binding anything.** The SDK's Scoped* bindings restore 0 on exit, not what was bound (the fleet's trap, kept from holofoil).

## Verified, and assumed

**Verified** on this Mac (the numbers are in the README's Status):

- the CoC is linear in nearness, the same either side of focus
- a disc carries exactly its highlight's light, for every shape, profile, squeeze and swirl; the cat's eye loses what two circles' overlap says
- the blades' corners, the flip in front of focus, Squeeze's aspect
- the fringe's sign and size either side of focus
- spike counts and directions, and a round aperture's none
- the ghosts on the line through the light and the centre
- a sharp foreground hides the bokeh behind it bit for bit; a blurred background spreads no farther over a nearer thing than that thing's blur
- the plane against a pinhole camera; Radial; the packed depth halves; the guided filter's least-squares fit; Subject's edge on the picture's; Haze ranking known distances
- the focus pull's exponential, and Point
- a moving textured subject tracked well under a fine step
- the clock's unit and origin; GL state handed back across a resize mid-track; bit-exact passthrough
- every slider's range, cue sheets, 47 parameter names
- a stand-in host (oxbow) loads it, reads its name, id and type, and renders 120 frames
- on Windows, Resolume Arena 7.27.1 (win-lab, software rendering) loads the release workflow's DLL, reads all 47 parameters as declared, renders with a clean log, and every control that can act on a still changes its picture (plugin-bench's `arena/expect/hyacinth.json`: 9 passed, 0 failed, 1 skipped)

**Assumed, not measured:**

- That Resolume on macOS calls it as Arena on Windows and oxbow do. It has never been loaded in Resolume on a Mac.
- That Focus Speed follows Arena's frames. The gate's carrier is a still under a settled focus, so the pull is checked only offline.
- That it looks right in Resolume on real footage, or on a real GPU on Windows: the gate ran on software rendering.
- That Subject, Haze and the tracker work on real footage. They were checked only on synthetic pictures: a red square on blue, a hazed scene of saturated patches, a textured patch translating. Nothing has checked a subject that turns, scales or is occluded.
- That the defaults suit real footage, and that the ghost table and the flare's energies look like a real lens. Those are taste.
- That a depth map from a depth-estimation tool, packed Side by Side, uses the convention here (white near). Depth Invert is there if not.
- The cost on any GPU but this Mac's.

## Would this hold on another rasteriser, at another raster?

Every picture check runs at 1280×720 and 320×180, and again on Apple's software renderer at 320×180. Every bound below comes from a texel, a ulp or a stated model, not from what this Mac printed.

- **`--passthrough`:** bit for bit, by construction. Mix 0 and the untouched pixels are `texelFetch` and an early return.
- **`--coc`:** the disc's edge is placed to half a half-resolution texel (1 px). The fitted slope is held to that pixel at the widest disc, so it is tight at 720p (±3.9%) and loose at 180p (±15%), where the per-point bound carries the check.
- **`--energy`:** 1% plus 2(w/R)², w the edge's ramp: a corner or curve leaves a remainder of order w²/R². That is 1.6% at 720p and 10.9% at 180p.
- **`--shape`:** a corner is located to the ramp's width over the radius, in degrees: 5.7° at 720p, 22.7° at 180p. Squeeze is ±0.03.
- **`--fringe`:** the difference of squared radii, to Blur Size px² (two disc edges, each half a texel).
- **`--starburst`:** the ring is sampled every quarter degree, and a spike is located to half a texel at its radius: 0.9° at 720p, 2.9° at 180p.
- **`--ghosts`:** the PCA axis passes within two half-resolution texels (4 px) of the centre and the light.
- **`--occlusion`:** the sharp side is bit for bit. The midground's grey is held to half an 8-bit level beyond where the background's blur can reach.
- **`--depth`:**
  - Plane and Radial: a few float ulps (2e-6).
  - the packed halves: the bilinear weights' 8-bit fixed point on a gradient under 1/256 a texel (2e-5)
  - the guided filter's fit: the ridge's most shrinkage at one sigma, |w|√ε/2 = 0.012 RMS
  - Subject's edge: 2 px, the picture's one plus the coefficients' quarter texel
  - Haze: a rank correlation over 0.9
- **`--focus`:** 1e-5 against exp(−*t*/τ), float arithmetic on order-1 numbers over 60 frames.
- **`--track`:** a sixth (RMS) and a half (worst) of the fine step, the step being the larger of half a template texel and half an analysis texel (Shimizu and Okutomi's bias for a parabola).
- **`--clock`:** 1e-5 between the late and early hosts' takes; the phases are reduced in double before they become floats.
- **`--state`, `--controls`, `--cues`, `--names`:** exact.

## The negative controls and mutants

**Negative controls.** `hytest --negative` runs 16 wrong models. Fourteen are built into the real shader through `SetHooksForTest`; two are in the harness (a millisecond clock taken as seconds, ramped cues). Every one must fail its check:

- the CoC from distance, not nearness
- a disc as bright as its point whatever its size
- the barrel's shadow brightening what is left instead of losing light
- bokeh in front of focus the same way up as behind
- one spike per blade edge instead of a line through the light
- the ghosts pushed off the axis
- the fringe's colours swapped
- a sharp foreground not hiding the bokeh behind it
- the background's blur spreading over a sharp foreground
- every pixel through linear light and back (no exact passthrough)
- the tracker's peak at the best candidate, no parabola
- the focus pulled at a constant rate, not eased
- Side by Side's depth read from the picture's half
- the estimated depth used raw, not guided
- a millisecond clock taken as seconds
- ramped cues

**Mutants.** `tools/mutate.sh` changes one character of the shipped code and runs the one check that should see it, at both rasters. It makes 21 mutants: 13 in the GLSL, 8 in the C++. All 21 are caught.

### The recorded GLSL mutation

The first full run, 2026-11-11, at 320×180 only, caught 16 of 21. Three of the five survivors were GLSL. This one showed a hole in a check:

    source/Shaders.cpp
    	float i01 = srb * sgb - srg * sbb;
    	                      ^ - -> +
    == mutant: GLSL: the colour covariance's inverse with a wrong cofactor, - -> +
       FAIL  --depth still PASSES -- the check does not cover this code

**Why it survived.** The only check on the guided filter was Subject's edge: two colours, and the depth's step between them. Any coefficients that separate red from blue give a step there, so a wrong inverse still made a sharp edge.

**The fix.** `--depth` now has a Luma case. Luma is an exact linear function of the colour, so a least-squares fit must return it. The harness reads the plugin's coefficients back and applies them to each quarter texel's colour (the 4×4 block's mean, which it takes itself). The shipped filter returns luma to 0.0023 RMS (bound 0.012). The mutant now fails `--depth`.

Three more were inside 320×180's wider bounds: the cat's eye's openings a fifth too far apart (GLSL), the blades a tenth of a step round and green focused twice as far (C++). Running each mutant's check at both rasters caught all three at 1280×720. The fifth, the mirrored spikes (GLSL), was equivalent and was replaced (above).

## The sweep's honest list

`tools/sweep.py` sweeps all 42 controls, each where it acts. All 42 are live. The contexts:

- **Wide discs (Blur Size at its top):** Shape, Blades, Blade Curve, Cat Eye, Swirl, Squeeze, Bubble and Fringe. At 320×180 the default leaves the lights 2 px circles, which the gather draws.
- **1280×720, Bokeh Only, Blur Size at its top:** Onion Rings, which are not drawn where a ring is under a texel, and Iris Rotate, swept half a heptagon's step.
- **A turned plane:** Focus X. The default plane's depth does not change across the frame.
- **Manual focus at 0.3:** Depth Invert. Under Point focus the focus inverts with the depth, and about 0.5 inversion is a mirror.
- **Haze:** Depth Edges. Luma comes through the colour guided filter unchanged whatever its radius.
- **Radial:** Radius. **Manual:** Focus Depth. **Track:** Grab and Track Size. **Some Orbs:** Orb Count, Orb Size and Orb Drift. **Some Flare from a Light:** Light X, Light Y, Ghosts, Halo, Starburst, Streak and Dirt.

`--bare` (no context) reports those as dead or barely alive, and that run exits 1 on purpose.

## Not done

- Never loaded into Resolume on macOS. On Windows, only on software rendering (win-lab), against a still.
- No presets, no browser demo, no OpenFX port.
- The fast guided filter's subsampling: detail finer than a quarter texel (4 px) is softened in Haze and Subject, and in Luma where a window's colour varies less than √ε.
- The tracker follows translation only: no scale or rotation search.
