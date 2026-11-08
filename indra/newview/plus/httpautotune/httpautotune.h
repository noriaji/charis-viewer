/**
 * @file httpautotune.h
 * @brief Fetch connections tuned to the line (plus/httpautotune)
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

#ifndef PLUS_HTTPAUTOTUNE_H
#define PLUS_HTTPAUTOTUNE_H

#include "stdtypes.h"

// The number of HTTP connections for textures and mesh2 starts at 64 and follows the line, between
// 24 and 192 for the textures, 24 and 64 for the mesh (a bulk load that must not take the line from the textures): every few seconds, from the attempts libcurl finished since the
// last look (LLCore::HttpRequest::setCompletionObserver()), it goes up while all connections are busy
// and the time to the first byte stays near the best seen, a quarter down when that time doubles, and
// half down on HTTP 429 / 503. PlusFetchAutoTune (on by default) switches it; off, or with the user's
// own TextureFetchConcurrency / Mesh2MaxConcurrentRequests, the official limits hold.
namespace plus::httpautotune
{
    // LLAppCoreHttp::refreshSettings() (llappcorehttp.cpp), for each class: the limit to set. The
    // tuned one for a tuned class, else setting (the official value) unchanged. policy is the class's
    // LLCore policy id (the completions are told by it). Main thread; not every frame.
    U32 connectionLimit(int app_policy, U32 policy, U32 setting);

    // The fetchers' own limits of requests in flight (LLTextureFetch::commonUpdate(), the mesh
    // repository's update): raised with the tuned limit so more connections have requests to carry.
    // Nothing when the class is not tuned. Any thread; atomics only.
    void raiseWater(int app_policy, S32& high_water, S32& low_water);

    // Whether the texture HTTP class had a 429 or 503 answer within the last `seconds` (any thread; research/299's retries
    // of failed texture fetches stop while it holds).
    bool throttledWithin(F64 seconds);
}

#endif // PLUS_HTTPAUTOTUNE_H
