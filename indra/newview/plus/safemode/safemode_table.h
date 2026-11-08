/**
 * @file safemode_table.h
 * @brief Safe mode (research/254): the one table of what a safe start turns
 *        off. A new experimental switch that is on by default gets a line
 *        here too (research/149).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef PLUS_SAFEMODE_TABLE_H
#define PLUS_SAFEMODE_TABLE_H

// Fixed values only: nothing from outside the program (the environment, the
// command line, a file) goes into a safe start's switches. The names and
// values are plain words (checked below), so they go into an environment
// block and a command line as they are, without quoting.
//
// Each switch's own comment (llrhi.h and the files named) says what its
// value 0 does; every one here has a maintained off path (a cue toggle or a
// switch for tests). Most LLRHI_* switches are read by static initializers
// before WinMain, so they reach a safe start only through the environment of
// a new process (safemode_launch.cpp starts one).

namespace plus::safemode
{
    struct Switch
    {
        const wchar_t* name;
        const wchar_t* value;
    };

    // The environment of a safe start: LLRHI_* switches that are on by default.
    inline constexpr Switch ENV_SWITCHES[] = {
        // A window that can be seen without focus keeps working as in front (plus/background, research/303): a safe start
        // takes the load off, as the official viewer does behind another window.
        { L"LLRHI_BACKGROUND_KEEP", L"0" },
        // The attribute pool keeps its vertex streams by the LL array an attribute reads (research/293 branch 1).
        { L"LLRHI_VB_SOURCE_STREAMS", L"0" },
        // GPU-driven drawing: DGC, GPU culling, the draw lists and runs (research/46, 124 V5).
        { L"LLRHI_GPU_DRIVEN", L"0" },
        { L"LLRHI_GPU_DRIVEN_DGC", L"0" },
        { L"LLRHI_DRAW_TEMPLATES", L"0" },
        // The native shadow, rigged and terrain paths (research/23), merged shadow packets.
        { L"LLRHI_NATIVE_SHADOWS", L"0" },
        { L"LLRHI_NATIVE_RIGGED", L"0" },
        { L"LLRHI_NATIVE_TERRAIN", L"0" },
        { L"LLRHI_SHADOW_PACKET_MERGE", L"0" },
        // Numbers in place of names, packets, published slots, tables (research/180, 197, 203; 124 V6).
        { L"LLRHI_TEX_HANDLES", L"0" },
        { L"LLRHI_VB_HANDLES", L"0" },
        { L"LLRHI_PACKETS", L"0" },
        { L"LLRHI_SLOT_PUBLISH", L"0" },
        { L"LLRHI_PASS_STATE", L"0" },
        { L"LLRHI_LIGHT_TABLE", L"0" },
        { L"LLRHI_TEXANIM_GPU", L"0" },
        // Paths that depend on device features or GPU encoding (research/124 5.5).
        { L"LLRHI_ALPHA_FAST", L"0" },
        { L"LLRHI_MATERIAL_FAST", L"0" },
        { L"LLRHI_TEX_BCN", L"0" },
        { L"LLRHI_TEX_COMPACT", L"0" },         // research/285 X2: texture images copied into fuller pool blocks
        { L"LLRHI_FRAME_PACING", L"0" },
        { L"LLRHI_OIT", L"0" },
        { L"LLRHI_PIPELINE_CACHE", L"0" },
        // Video memory: images over 8 MB in memory of their own, VMA's default blocks 64 MB (research/285 X3).
        { L"LLRHI_VK_BIG_BLOCKS", L"0" },
        // Work on other threads (research/63, 66, 179, 188).
        { L"LLRHI_PRESENT_THREAD", L"0" },
        { L"LLRHI_AVATAR_THREADS", L"0" },
        { L"LLRHI_SHADOW_RECORD_THREADS", L"0" },
        { L"LLRHI_SHADOW_PACKET_THREADS", L"0" },
        { L"LLRHI_CULL_REUSE", L"0" },
        // Textures on screen first (research/285): the fetch order and the floor, the bias off screen only,
        // off screen scaled down first.
        { L"LLRHI_TEX_VIEW_FETCH", L"0" },
        { L"LLRHI_TEX_VIEW_BIAS", L"0" },
        { L"LLRHI_TEX_VIEW_FREE", L"0" },
        // What is on screen does not settle for level 5, the first 600 bytes (research/303).
        { L"LLRHI_TEX_ONSCREEN_COARSEST", L"0" },
        // What is drawn as the gray stand-in asks for its first level and goes with what is on screen (research/304).
        { L"LLRHI_TEX_GRAY_FLOOR", L"0" },
        // The texture cache's index, header records and fast cache are opened and closed for every request (research/306 P3).
        { L"LLRHI_TEX_CACHE_KEEP_OPEN", L"0" },
        // What the cache index does not have is read from the cache all the same (research/306).
        { L"LLRHI_TEX_CACHE_MISS_SKIP", L"0" },
        // A failed texture download that may pass is asked for again (research/299).
        { L"LLRHI_TEX_FETCH_RETRY", L"0" },
        // The idle memos (research/178, 180, 193).
        { L"LLRHI_IDLE_SKIP_TEXANIM", L"0" },
        { L"LLRHI_IDLE_SKIP_OMEGA", L"0" },
        { L"LLRHI_IDLE_SKIP_MEMO", L"0" },
        { L"LLRHI_SEEN_BY_MEMO", L"0" },
        { L"LLRHI_VPARAM_MEMO", L"0" },
        // Charis's own passes: never recorded, whatever their settings say.
        { L"LLRHI_SSGI", L"0" },
        { L"LLRHI_VOLUMETRIC", L"0" },
        { L"LLRHI_SSR", L"0" },
        { L"LLRHI_GTAO", L"0" },
        { L"LLRHI_SKIN_SSS", L"0" },
        { L"LLRHI_SKIN_FILL", L"0" },
        { L"LLRHI_WET_LOOK", L"0" },
        { L"LLRHI_RAIN", L"0" },
        { L"LLRHI_GLARE", L"0" },
        { L"LLRHI_ATMOSPHERE", L"0" },
        { L"LLRHI_UPSCALE", L"0" },
        { L"LLRHI_TAA", L"0" },
        { L"LLRHI_LOOK", L"0" },
        // VRAM given back while it is short: the GL buffers' copies (research/245, 285; off = PlusBufferCompact off).
        { L"LLRHI_VK_BUFFER_COMPACT", L"0" },
        // ... and VMA's empty blocks freed for the frames after a pass (research/285 compact-hysteresis, a local VMA change).
        { L"LLRHI_VK_EMPTY_TRIM", L"0" },
        // No ray tracing extensions on the device (research/230).
        { L"LLRHI_VK_RAYTRACING", L"0" },
    };

    // Settings with no switch of their own, given to a safe start as "--set
    // <name> <value>" (LLAppViewer::initConfiguration: not saved, the user's
    // settings.xml keeps its values).
    inline constexpr Switch SETTINGS[] = {
        { L"PlusVolumetricClouds", L"0" },      // plus/volclouds
        { L"PlusSkyStars", L"0" },              // plus/starfield: the official stars
        { L"PlusJ2CDecoder", L"0" },            // plus/j2cdecoder: OpenJPEG decodes (slj2k is on by default)
    };

    // The environment variable that marks a safe start (the child), with its
    // reason as the value. Its presence also keeps the child from starting
    // another one.
    inline constexpr wchar_t ENV_MARK[] = L"CHARIS_SAFE_MODE";
    inline constexpr wchar_t REASON_ARGUMENT[] = L"argument";   // --safe
    inline constexpr wchar_t REASON_SHIFT[] = L"shift";         // Shift held at the start
    inline constexpr wchar_t REASON_SETTING[] = L"setting";     // "start in safe mode next time too" (safemode_files.h)
    inline constexpr wchar_t REASON_OFFER[] = L"offer";         // the starts before did not get through, and the user said yes

    // Set (to anything) by the unattended tools (tools/smoke-start.ps1, bake-shaders.sh,
    // dummy-sim/run-local.ps1, launch.ps1 -Unattended): no question, "start in safe mode next time
    // too" not followed, the start not counted. --safe and Shift still work. Only whether it is there
    // is read.
    inline constexpr wchar_t ENV_NO_PROMPT[] = L"CHARIS_SAFE_MODE_NO_PROMPT";

    // A plain word: letters, digits and '_'.
    constexpr bool plainWord(const wchar_t* text)
    {
        if (!text || !*text)
        {
            return false;
        }
        for (; *text; ++text)
        {
            const wchar_t c = *text;
            if (!((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_'))
            {
                return false;
            }
        }
        return true;
    }

    template <size_t N>
    constexpr bool plainTable(const Switch (&table)[N])
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (!plainWord(table[i].name) || !plainWord(table[i].value))
            {
                return false;
            }
        }
        return true;
    }

    static_assert(plainTable(ENV_SWITCHES), "safe mode switches: plain words only");
    static_assert(plainTable(SETTINGS), "safe mode settings: plain words only");
    static_assert(plainWord(ENV_MARK) && plainWord(ENV_NO_PROMPT) && plainWord(REASON_ARGUMENT) && plainWord(REASON_SHIFT) && plainWord(REASON_SETTING) &&
                      plainWord(REASON_OFFER),
                  "safe mode mark: plain words only");
}

#endif // PLUS_SAFEMODE_TABLE_H
