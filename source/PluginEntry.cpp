#include "Hyacinth.h"

/**
    The one registration.

    This file is listed directly in the Hyacinth MODULE target, not in
    hyacinth_core: `CFFGLPluginInfo` registers itself from a file-scope
    constructor and nothing ever references it by name, so in a STATIC
    archive the linker is entitled to drop the whole translation unit --
    giving a bundle that loads, exports `plugMain`, and reports that it
    contains no plugins. The core stays an OBJECT library for the same reason.

    The name is `SW Hyacinth`, eleven characters. The FFGL name field is
    `char[ 16 ]` and is **not** null-terminated, so the host truncates without
    saying so. `oxbow probe` is what reads it back the way a host does.
*/
namespace
{
class HyacinthEffect : public hyacinth::HyacinthPlugin
{
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< HyacinthEffect >,// Create method
	"HY01",                         // Plugin unique ID of maximum length 4
	"SW Hyacinth",                  // Plugin name
	2,                              // API major version number
	1,                              // API minor version number
	0,                              // Plugin major version number
	1,                              // Plugin minor version number
	FF_EFFECT,                      // Plugin type
	"Depth of field, bokeh and lens flare.\n\nA lens focused at one distance, in front of the clip. Give the "
	"picture a depth (a plane, a radius round a point, its own brightness, haze or colours, its alpha, or a depth map "
	"beside or under it), focus on a depth, a point or something the tracker follows, and everything else defocuses "
	"by how far it is from focus, its highlights spreading into the shape of the aperture. Or keep the picture sharp "
	"and add only the bokeh. Lens flare, drifting orbs and lens dirt are the same aperture again.",
	"Hyacinth FFGL effect"// About
);

extern "C" const char* HyacinthBuildStamp()
{
	return "hyacinth " HYACINTH_VERSION ", built " __DATE__ " " __TIME__;
}
