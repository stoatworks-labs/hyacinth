# Attributions

Hyacinth is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the GL state guard, the pass buffer, the log and the tooling — Stoatworks holofoil

<https://github.com/stoatworks-labs/holofoil>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape and its table of controls, source/GLState.h (radar's, which is vectrix's, from resolume-scopes), source/PassBuffer.{h,cpp} (the fleet's, without a depth buffer) and source/Diag.{h,cpp}; the host clock's unit vote (rosette's); the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues and --offline; and tools/verify.sh, tools/mutate.sh, tools/glslc.sh, tools/sweep.py and tools/wincheck.ps1 (slitscan's). All adapted.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Guided Image Filtering — K. He, J. Sun and X. Tang, ECCV 2010; IEEE TPAMI 35(6), 2013

The colour guided filter (section 3.5): in each window the depth is a least-squares linear function of the colour with a ridge, which puts an estimated depth's edges on the picture's. Implemented from the equations, weighted by the raw estimate's evidence. hytest --depth checks it returns an exact linear function of the colour, and that Subject's depth edge lands on the picture's edge.

### Fast Guided Filter — K. He and J. Sun, arXiv:1505.00996, 2015

<https://arxiv.org/abs/1505.00996>

Computing the guided filter's coefficients at a quarter of the resolution and applying them to the full-resolution picture.

### Single Image Haze Removal Using Dark Channel Prior — K. He, J. Sun and X. Tang, CVPR 2009

Depth From Haze: a haze-free picture's darkest channel over a patch is near zero, so the haze's transmission is 1 − dark / air light, and its logarithm is distance. hytest --depth checks the ranking on a hazed scene of known distances.

### Sub-pixel estimation error cancellation on area-based matching — M. Shimizu and M. Okutomi, IJCV 63(3), 2005

The parabola fit's sub-pixel bias, used only to set the bound of hytest --track.

### Hash Functions for GPU Rendering — M. Jarzynski and M. Olano, JCGT 9(3), 2020

<https://jcgt.org/published/0009/03/02/>

The PCG hash (after Melissa E. O'Neill's PCG) behind the orbs and the lens dirt, so the same pattern comes out on every GPU.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Real lenses' bokeh and flare

Cat's-eye bokeh towards the corners, the soap-bubble rims of over-corrected lenses, the onion rings of aspheric elements, swirl, anamorphic ovals and streaks, a mirror lens's ring, shaped-aperture cut-outs, and ghosts on the line through the light and the centre. The ghost table's places and colours are set by eye from photographs of real flares; nothing was taken from any other bokeh or flare implementation.

## Standards and published specifications

What the implementation is measured against.

- **IEC 61966-2-1 (sRGB)** — the transfer function in source/Shaders.cpp: the clip is linearised so defocused light spreads and adds as light does, then encoded again.
- **ITU-R BT.709 luminance** — the weights Depth From Luma reads the clip's brightness with. hytest --depth checks the guided filter returns them.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
