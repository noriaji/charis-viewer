/**
 * @file rmlsystem.h
 * @brief RmlUi's start and end, its system and file interfaces, and the "RmlUi:" log line (research/364).
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

// RmlUi costs nothing while no window uses it: it is started by the first host (acquire(), when a window
// with a plus_rml_view loads its document) and shut down RELEASE_DELAY_SECONDS after the last host went
// (release()), at once while the viewer quits (the hosts go in LLViewerWindow::shutdownViews, before
// shutdownGL: LLImageGL, LLVertexBuffer and LLFontManager are still there). Opening a window in that delay
// keeps it running, so opening and closing start and end it at most once a minute. A start that failed is
// not tried again in that session; a safe start (plus::safemode) does not start it. Main thread only.
//
// The file interface reads only files under app_settings/plus/rmlui/ (isDocumentPath()): no absolute path,
// no "..", no ':' (drives, URLs), at most MAX_FILE_BYTES. Nothing is fetched from the network.

#ifndef PLUS_RMLSYSTEM_H
#define PLUS_RMLSYSTEM_H

#include "rmlcommon.h"

#include <string>

namespace plus::rmlui
{
    class Renderer;

    constexpr F64 RELEASE_DELAY_SECONDS = 60.0;
    constexpr S64 MAX_FILE_BYTES = 1024 * 1024;
    constexpr size_t MAX_PATH_CHARS = 200;
    constexpr F64 STATS_PERIOD_SECONDS = 5.0;

    enum class Start
    {
        Ready,      // RmlUi runs; the host holds it until release()
        SafeMode,   // a safe start: RmlUi is not used (nothing held)
        Failed,     // the start failed, now or before in this session (nothing held)
    };

    // What the first acquire() of a start took (both 0 when RmlUi was running already).
    struct StartTimes
    {
        F64 init_ms = 0.0;
        F64 fonts_ms = 0.0;
        S32 font_faces = 0;
    };

    // A host begins to use RmlUi (starting it when it is not running). Ready: call release() once later.
    Start acquire(StartTimes& times);
    // A host that got Ready stops using RmlUi.
    void release();
    // The render interface: only while a host holds RmlUi.
    Renderer& renderer();

    // A path the file interface opens: relative to app_settings/plus/rmlui/, letters, digits and _ . - /
    // only, no empty, "." or ".." part, no Windows device name, at most MAX_PATH_CHARS.
    bool isDocumentPath(const std::string& path);
    // A name of letters, digits and '_' only, 1 to max_chars long ("[[key]]"'s key, a UI image's name).
    bool isPlainName(const std::string& name, size_t max_chars);

    // Counters for the "RmlUi:" line: plain main-thread integers, counted always (no allocation, no lock);
    // the times only while the line is on (statsOn()).
    struct Stats
    {
        U32 draws = 0;              // frames a host drew
        U32 updates = 0;            // Context::Update calls
        U32 renders = 0;            // Context::Render calls (recordings)
        U32 replays = 0;            // frames drawn from the last recording (no Update, no Render)
        U32 render_geometry = 0;    // RenderGeometry calls (what RmlUi asked to draw)
        U32 runs = 0;               // drawRange calls made for them
        U32 run_builds = 0;         // a list's runs made again (a changed recording or opacity)
        U32 run_reuses = 0;         // packed runs that took the last build's buffer (no vertices copied)
        U32 run_packs = 0;          // packed runs whose vertices were copied into a new buffer
        U32 compiles = 0;           // CompileGeometry calls
        U64 compile_bytes = 0;      // the vertices and indices handed to them
        F64 update_ms = 0.0;
        F64 render_ms = 0.0;
        U32 timed_draws = 0;        // draws whose Update / Render times are in the sums
    };
    Stats& stats();
    // The "RmlUi:" line is on: LLRHI::sPeriodicLogs or LLRHI::sFrameCost (a flag test each).
    bool statsOn();
    // Called from each host's draw: every STATS_PERIOD_SECONDS while statsOn(), one line, then the counters start over.
    void logStatsIfDue(F64 now_seconds);
}

#endif // PLUS_RMLSYSTEM_H
