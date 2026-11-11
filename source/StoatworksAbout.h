/*
 * Stoatworks Labs - About window data for Hyacinth.
 *
 * PROVISIONAL HAND COPY, adapted from hyacinth's generated header on
 * 2026-11-11, because hyacinth is not in the website's projects.json yet.
 * When it is registered, stoatworks-backend/scripts/sync-about.py generates
 * this file and overwrites it. `guide` is empty because no user guide exists
 * yet, so the block has no User guide button (graticule did the same).
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Hyacinth";
    inline constexpr auto slug = "hyacinth";
    inline constexpr auto hook = "Depth of field, bokeh and flares, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/hyacinth/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/hyacinth";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}
