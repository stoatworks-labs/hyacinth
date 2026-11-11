# hyacinth

Shallow depth of field, bokeh and lens flare for Resolume Arena/Avenue, as an FFGL effect.

- **What it does:** gives every pixel of the clip a depth, focuses a thin lens at one distance, and defocuses the picture by each pixel's circle of confusion. The clip's highlights are drawn as the aperture they would really show as. Or it leaves the picture sharp and adds only the bokeh, orbs, lens flare and lens dirt.
- **Depth:** a plane to a horizon, a radius round the focus point, the clip's luma, haze, "what looks like the subject", the clip's alpha, or a depth map packed Side by Side or Over Under.
- **Focus:** a set distance, the depth at a point, or the depth of whatever a tracker is following.
- **Build:** C++/GLSL, CMake MODULE → universal `.bundle` (macOS).
- **Identity:** MIT. ID `HY01`, display name `SW Hyacinth`, bundle `com.stoatworks.ffgl.hyacinth`, v0.1.0.
- **Status:** public at `github.com/stoatworks-labs/hyacinth`, checked out at `~/Projects/resolume/hyacinth`. Released at v0.1.0 (2026-11-11). Never loaded into Resolume on macOS; on Windows the fleet's Arena gate passes on Arena 7.27.1.

The name is an oblique reference: Hyacinth Bucket insisted it was pronounced "Bouquet", and bouquet is close to bokeh.

Read `AGENTS.md` before changing the lens model, the aperture, the guided filter, the tracker, a default or any tolerance in the harness.

## Commands (CMake)

- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects)
- Render offline: `./build/hytest --out /tmp/frame.png`. That is the harness's street at dusk, 30 frames in, the focus on a rolling ball. Add `--frozen` to stop the ball.
- List parameters: `./build/hytest --list`
- Set anything by name: `./build/hytest --set "Shape=Heart" --set "Blur Size=0.8"`. An option takes its element's name or its index.
- Change something mid-take: `--at 60 "Focus Depth=0.9"` (an event is released the frame after).
- Film: `./build/hytest --film 300 --size 1280x720 | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through it: `ffmpeg -i in.mov -vf fps=60 -f rawvideo -pix_fmt rgba - | ./build/hytest --pipe --size WxH | ffmpeg …`, with `--script cues.txt` (`frame Name value` per line) to automate it.

## Verify

- **Everything:** `tools/verify.sh`. It runs:
  - the GLSL greps (reserved words, trig, WebGL2, MSVC's literal limit) and `glslc` on all 25 assembled shaders
  - a fresh universal build, with no warnings outside the SDK
  - lipo, plist, ad-hoc signature, `oxbow probe` and `selftest`
  - every check at 1280×720 and 320×180, and again on Apple's software renderer at 320×180
  - `--offline`, the pipe format, the negative controls, the 21 mutants, the sweep and the bench

  `OXBOW=` points at an oxbow binary if `~/Projects/resolume/oxbow/build/oxbow` isn't built.
- **The lens:**
  - `--coc`: a point's disc has radius Blur Size × |n − n_focus|, fitted over six depths either side of focus
  - `--energy`: a disc carries exactly its highlight's light, for every shape, profile and squeeze; the cat's eye loses what two circles' overlap says
  - `--shape`: the disc's corners are its blades, upside down in front of focus; Squeeze makes it that much taller
  - `--fringe`: R_r² − R_g² = 2 coc · Blur Size · Fringe, either side of focus
  - `--occlusion`: a sharp foreground hides the bokeh behind it bit for bit; a blurred background spreads no farther over a nearer thing than that thing's own blur
- **The flare:** `--starburst` (N spikes for even N, 2N for odd, on the edges' normals) and `--ghosts` (on the line through the light and the centre).
- **Depth, focus and tracking:**
  - `--depth`: Plane against a pinhole camera; Radial; Side by Side and Over Under; the guided filter's least-squares fit; Subject's edge on the picture's; Haze ranking against known distances
  - `--focus`: the pull is exp(−t/τ) frame by frame; Point reads the depth at the point
  - `--track`: a moving textured subject followed to a sixth of the fine search's step
- **The rest:** `--passthrough` (Mix 0 and the in-focus picture bit for bit), `--clock` (a host clock 499 million ms in) and `--state` (GL state handed back, across a resize mid-track). No GL needed: `--controls`, `--cues`, `--names`, and `--offline` runs those three.
- **The checks can fail:** `--negative` (16 wrong models) and `tools/mutate.sh` (21 one-character mutants; `tools/mutate.sh TEXT` runs only the ones whose description contains TEXT).
- **No dead controls:** `python3 tools/sweep.py` sweeps 42 controls, all live. `--bare` lists the ones that only act in context.
- **Render cost:** `./build/hytest --bench`.
- **CI** (`.github/workflows/ci.yml`): macOS arm64 build, `hytest --list` and `--offline`, glslc; a Windows x64 build through vcpkg's GLEW. The repo is public, so it runs on GitHub's free minutes. `release.yml` builds and publishes on a `v*` tag; a dispatch builds the Windows DLL without publishing, for the Arena gate.

## Notes

- **Depth is nearness**, n = 1/distance scaled to 0..1 (1 near, 0 at infinity). A thin lens's CoC is linear in nearness, so CoC = Blur Size × (n − n_focus) in pixels, positive in front of focus.
- **The passes** are listed in order in `Shaders.h`. Each work buffer covers the whole picture, so every one is read at the picture's uv.
- **One aperture everywhere.** The bokeh sprites, the orbs, the ghosts and the starburst's spike directions all come from the same `shapeSd()` and the CPU's edge normals (`Iris.cpp`).
- **The sprites are normalised by `AreaOf()`** (`Iris.cpp`), the aperture's integral on a fine grid, rebuilt only when the shape changes. The harness counts the rebuilds (`--state`).
- **Highlights under 6 px of CoC are handed to the gather** (`scatteredOf()`), all of them under 3 px. A check that reads the sprites' buffer alone must use larger discs.
- **The guided filter is the colour one** (He, Sun and Tang 2010, §3.5), weighted by the raw estimate's evidence. Four row passes make the 13 window sums; the coefficients are a 3×3 solve.
- **Test hooks:** `uniform int Hooks` ships, so the harness measures the real shader. Its bits are in `Shaders.h` and must be repeated in `kCommon`.
- **No trig in the GLSL.** Directions come from the CPU. Randomness is PCG, never `sin`. `verify.sh` greps for trig.
- **The composite works from `gl_FragCoord`**, not the quad's interpolated uv: the software renderer's varyings are about a thousandth of a pixel off.
- **Passthrough uses `texelFetch`** and an early return, so Mix 0 and the untouched parts of the picture are bit-exact. At Mix 1 the effect is written as it is, not through `mix()`.
- **Host parameters:** all are 0..1, mapped in `Controls.cpp`, and options hold the element index. `--controls` checks every mapping against the numbers in `Controls.h`, written into the harness as literals.
- **GLSL reserved words** (`centroid sample patch input output filter common active half layout flat packed`, `noise1`–`noise4` and the rest of verify.sh's list) must not be identifiers. `centroid` was used here once and stopped a pass compiling.
- **Override `SetTextParameter`** to return `FF_SUCCESS` for the About block, or no host can instantiate the plugin.
- **`hyacinth_core` is an OBJECT library:** the plugin registers itself from a file-scope constructor that nothing references.
- **Allocate every buffer before binding any.** The SDK's Scoped* bindings restore 0 on exit, not what was bound before.
- **The macOS build must be universal.** Check with `lipo`, never the build log.
- **No trademarks** in the plugin's name, labels, options or any future preset names.
- **Generated files:** `source/StoatworksAbout.h` (backend `scripts/sync-about.py`, from the website's `projects.json`), `ATTRIBUTIONS.md` and the README's attributions block (`sync-attributions.py`), `.github/ISSUE_TEMPLATE/` (`sync-issue-templates.py`), `.github/FUNDING.yml` (`sync-funding.sh`) and `scripts/release-lib.sh` (the backend's `release/`). `docs/USER-GUIDE.pdf` is built by the website's `scripts/build_guides.py hyacinth`. Never edit any of them here.

## Not done yet

- Never loaded into Resolume on macOS. On Windows the fleet's Arena gate passes on Arena 7.27.1 (plugin-bench, `arena/expect/hyacinth.json`). No presets, no browser demo, no OpenFX port.

## Diagnostics

`source/Diag.{h,cpp}` writes a log file only, with no crash handler (this runs inside Resolume).

    ~/Library/Logs/hyacinth/hyacinth.YYYY-MM-DD.log
    %LOCALAPPDATA%\hyacinth\logs\hyacinth.YYYY-MM-DD.log

`HYACINTH_LOG_DIR` overrides the folder.
