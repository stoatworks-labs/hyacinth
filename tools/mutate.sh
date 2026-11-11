#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders, or built its references from the plugin's own functions, would
# pass every mutant here.
#
# A mirrored spike (across = q.x n.y + q.y n.x) is NOT here: every iris
# with a vertex straight up is symmetric about its own axes, so the mirror
# draws the same spikes -- an equivalent mutant, which no check can catch.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK is
# symlinked, not copied), built arm64-only, with only the named check run at
# both of its rasters (the small one's bounds are wide: a cat's eye a fifth
# off is inside 320x180's and far outside 1280x720's), under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught). Most of them are in the GLSL, because that is
# where the optics are: the harness writes the thin lens, the aperture's
# outline, the cat's eye's two-circle overlap, the pinhole's plane and the
# exponential ease from their own formulas, in double, so a mutant in any of
# those has nothing on the harness's side to hide behind. Iris.cpp's area is
# the one model both sides share, and its mutant is caught because the GLSL
# draws the profile itself.
#
#     tools/mutate.sh            every mutant
#     tools/mutate.sh TEXT       only the mutants whose description contains TEXT
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|	return BlurPx * ( n - nf );|	return BlurPx * ( n + nf );|--coc|GLSL: the CoC from the sum of the nearnesses, - -> +"
	"source/Shaders.cpp|	float p   = 1.0 - Bubble * side * ( 2.0 * rho2 - 1.0 );|	float p   = 1.0 - Bubble * side * ( 2.0 * rho2 - 2.0 );|--energy|GLSL: the bubble profile no longer of zero mean, 1 -> 2"
	"source/Iris.cpp|	double p             = 1.0 - desc.bubble * side * ( 2.0 * rho2 - 1.0 );|	double p             = 1.0 - desc.bubble * side * ( 2.0 * rho2 - 2.0 );|--energy|C++: the area the sprites are normalised by integrates the wrong profile, 1 -> 2"
	"source/Shaders.cpp|		vignette = max( length( u - d ), length( u + d ) ) - 1.0;|		vignette = max( length( u - d ), length( u + d ) ) - 2.0;|--energy|GLSL: the barrel's two openings twice the aperture, no cat's eye, 1 -> 2"
	"source/Shaders.cpp|	vec2 d         = 0.5 * CatEye * field;|	vec2 d         = 0.6 * CatEye * field;|--energy|GLSL: the two openings a fifth farther apart, 5 -> 6"
	"source/Shaders.cpp|		ui = -ui;|		ui = +ui;|--shape|GLSL: the aperture the same way up in front of focus, - -> +"
	"source/Shaders.cpp|	u.y /= Squeeze;|	u.y *= Squeeze;|--shape|GLSL: an anamorphic squeeze making the disc wider, not taller, / -> *"
	"source/Iris.cpp|		const double a       = 0.5 * kPi + ( k + 0.5 ) * 2.0 * kPi / n;|		const double a       = 0.5 * kPi + ( k + 0.6 ) * 2.0 * kPi / n;|--shape|C++: the blades a tenth of a step round, 5 -> 6"
	"source/Hyacinth.cpp|		set( \"FringeOffset\", f( -0.5 * fringe ), f( 0.5 * fringe ), f( -0.5 * fringe ) );|		set( \"FringeOffset\", f( -0.5 * fringe ), f( 1.5 * fringe ), f( -0.5 * fringe ) );|--fringe|C++: green focused twice as far from red and blue, 0 -> 1"
	"source/Hyacinth.cpp|	const int spikeBlades = shape == Shape::Star ? 5 : desc.blades;|	const int spikeBlades = shape == Shape::Star ? 6 : desc.blades;|--starburst|C++: the star's spikes from six edges' normals, not five, 5 -> 6"
	"source/Shaders.cpp|		vec2 axis = Lpx - Cpx;|		vec2 axis = Lpx + Cpx;|--ghosts|GLSL: the ghosts' axis from the frame's corner, not its centre, - -> +"
	"source/Shaders.cpp|	float i01 = srb * sgb - srg * sbb;|	float i01 = srb * sgb + srg * sbb;|--depth|GLSL: the colour covariance's inverse with a wrong cofactor, - -> +"
	"source/Shaders.cpp|		n = 1.0 - length( heights( at ) - heights( focusPoint() ) ) * RadialInv;|		n = 2.0 - length( heights( at ) - heights( focusPoint() ) ) * RadialInv;|--depth|GLSL: Radial's nearness a unit too high, 1 -> 2"
	"source/Hyacinth.cpp|	const double sHorizon = -sMax + HorizonFromParam( params[ PT_HORIZON ] ) * 2.0 * sMax;|	const double sHorizon = -sMax + HorizonFromParam( params[ PT_HORIZON ] ) * 3.0 * sMax;|--depth|C++: the horizon placed half as far again, 2 -> 3"
	"source/Hyacinth.cpp|		const double alpha = tau > 0.0 ? 1.0 - std::exp( -dt / tau ) : 1.0;|		const double alpha = tau > 0.0 ? 1.0 - std::exp( -dt * tau ) : 1.0;|--focus|C++: the focus eased at a rate of tau, not 1/tau, / -> *"
	"source/Shaders.cpp|				sub.x = clamp( 0.5 * ( l - r ) / den, -0.5, 0.5 );|				sub.x = clamp( 0.5 * ( l + r ) / den, -0.5, 0.5 );|--track|GLSL: the tracker's sub-texel parabola through the wrong point, - -> +"
	"source/Shaders.cpp|	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );|	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.5 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );|--occlusion|GLSL: light encoded at 1/2.5, not sRGB's 1/2.4, 4 -> 5"
	"source/Shaders.cpp|	return toLinear( code ) * ( 1.0 + Gain * s * s );|	return toLinear( code ) * ( 2.0 + Gain * s * s );|--energy|GLSL: a clipped highlight restored twice as bright before Gain, 1 -> 2"
	"source/Shaders.cpp|		energy = hooked( kHookNoEnergy ) ? e / CellAreaPx : e / ( unit * radius * radius * Squeeze * swirlOf( field ) );|		energy = hooked( kHookNoEnergy ) ? e / CellAreaPx : e * ( unit * radius * radius * Squeeze * swirlOf( field ) );|--energy|GLSL: a disc's light multiplied by its area, not spread over it, / -> *"
	"source/Controls.cpp|	return low * std::pow( high / low, clamp01( v ) );|	return low * std::pow( high * low, clamp01( v ) );|--controls|C++: every geometric slider off its top, / -> *"
	"source/Controls.cpp|constexpr double kBlurMost       = 0.08;|constexpr double kBlurMost       = 0.09;|--controls|C++: Blur Size's top an eighth wider than the table says, 8 -> 9"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

only="${1:-}"
caught=0
ran=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	[[ -z "$only" || "$what" == *"$only"* ]] || continue
	ran=$(( ran + 1 ))
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"

	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target hytest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	limit 300 "$tree/build/hytest" "$check" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "$ran" "$caught"
[[ "$ran" -gt 0 && "$caught" -eq "$ran" ]]
