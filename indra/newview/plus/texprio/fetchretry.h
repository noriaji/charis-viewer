/**
 * @file fetchretry.h
 * @brief A texture whose fetch failed for a passing reason is asked for again, not flagged missing (research/299 tex-fetch-retry)
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

#ifndef PLUS_TEXPRIO_FETCHRETRY_H
#define PLUS_TEXPRIO_FETCHRETRY_H

#include "stdtypes.h"

#include "lluuid.h"

#include <memory>
#include <string>

class LLViewerFetchedTexture;
namespace LLCore
{
    struct HttpStatus;
    class HttpHeaders;
    class HttpResponse;
}

// research/299 in the sl-viewer repository. Upstream flags a texture whose fetch ended with no data as a missing
// asset for good (setIsMissingAsset: nothing clears it for a world texture), and everything that shows it draws it
// gray for the rest of the session. A real fetch ended so with HTTP 403 in the field (50 in 3.5 minutes at a busy
// event, shared by about a thousand faces). And a second step that fails (the first 600 bytes came, the rest did
// not) decodes the first bytes at the coarsest level and the worker goes back to INIT and asks for the same range
// again, at once, until it works or the texture's wanted level changes: the texture shows "fetching" and stays
// coarse (it comes right when the camera comes near and the level wanted changes).
//
// Here, a failure that may pass is not flagged and not left: for a texture retries are for, a second step that
// failed ends on its first bytes (the worker stays DONE, mPlusShortEnd); the texture sees it (a "similar request
// finished", or data not needed) and asks for the rest again after a wait that grows with each try. The
// failure is told from how the worker ended (LLTextureFetch::plusEndOfRequest):
//   404, 410, a decode that failed for good: it is not there: flagged at once, as before
//   429:                                      the server asks for less: 10, 30, 90 s on screen; 120, 300 off screen
//   5xx, 499, connection and timeout errors: libcurl has tried these 5 times already: 60, 180 s on screen; 180, 600
//   403 and the other 4xx, no data and no error: not known why: 5, 15, 45, 120 s on screen; 60, 180 off screen
//   (a CDN keeps a 4xx answer for a while; 8 403s in 10 s are taken as its limit: the pause of a busy server)
// each wait with +-20% (from the texture's id, no random numbers), doubled for a minute after a 429 or 503. A retry is
// one HTTP attempt (libcurl's own retries off), so the limit counts attempts: a bucket of RETRIES_PER_SECOND a
// second (near on screen first, then the rest on screen, off screen last and OFF_SCREEN_PER_SECOND at most).
// All retries stop for OVERLOAD_PAUSE_SECONDS after any 429 / 503 (plus/
// httpautotune sees them) and while a burst of connection errors says the line is down. After the last try the
// texture is flagged as before. A texture waiting for a retry is drawn gray (GrayWire), as one flagged missing is.
//
// The failed fetch worker is deleted when the retry is scheduled (the same way as the idle clean-up of updateFetch
// and setIsMissingAsset do), a second or more before the new request makes a new worker, as a step up in level does.
// Main thread only.
// Switches: PlusTexFetchRetry (setting, on by default), LLRHI_TEX_FETCH_RETRY=0 (environment).
namespace plus::texprio::fetchretry
{
    constexpr U32 RETRIES_PER_SECOND = 4;
    constexpr U32 OFF_SCREEN_PER_SECOND = 1;
    constexpr F64 OVERLOAD_PAUSE_SECONDS = 10.0;
    constexpr F64 OVERLOAD_SCALE_SECONDS = 60.0;

    // LLViewerFetchedTexture::processFetchResults: the fetch ended with no data (and the texture has an image or not).
    // true: a retry is scheduled, do not flag it missing or lift its level. false: as before (switch off, a plain
    // world texture only, 404, 410, a failed decode, or the tries are used up).
    bool onFetchFailure(LLViewerFetchedTexture* tex);

    // A request that ended on the first bytes it had (decoded coarser than asked) because its HTTP request failed:
    // called by processFetchResults ("data not needed") and updateFetch (createRequest said "a similar request
    // finished"). Scheduled: a retry is scheduled (drop the worker). GaveUp: it failed and no retry is left (or
    // 404/410): the level it has is the end. None: nothing failed (a short file, switched off, not a plain world
    // texture, a retry already waiting).
    enum class ShortEnd { None, Scheduled, GaveUp };
    ShortEnd onShortEnd(LLViewerFetchedTexture* tex);

    // processFetchResults, when image data came: the tries start again if the data reached the level asked for
    // (the first bytes alone are not the end of the tries).
    void onFetchSuccess(LLViewerFetchedTexture* tex, bool reached_level);

    // processFetchResults, a fetch that ended with no worker behind it (another texture of the same id cancelled it,
    // it was let go at priority 0) while a retry of this texture was out: the try is used, the next is scheduled
    // within the limits (true), not asked at once.
    bool onLostRequest(LLViewerFetchedTexture* tex);

    // LLTextureFetchWorker::onCompleted, an HTTP answer with texture data came (a line that is down is told by
    // connection errors and no such answer for long). Any thread.
    void noteHttpData();

    // updateFetch, before a request is made: false while a scheduled retry is not due (its wait, the pause, the
    // second's limits, a line that is down, off screen: not in view yet).
    bool retryDue(LLViewerFetchedTexture* tex);

    // Whether retries are on (setting and environment, as the main thread sees it now).
    bool switchedOn();
    // Whether they are for this texture: on, and a plain world texture.
    bool applies(LLViewerFetchedTexture* tex);
    // Inside a ReissueScope for a texture they are for: the request being made may end on its first bytes when its
    // second step fails (LLTextureFetch::createRequest hands this to the worker). false outside one.
    bool shortEndAllowed();

    // updateFetch, after a request was made: a due retry went out (counts against the second's limits).
    void noteRequested(LLViewerFetchedTexture* tex);

    // Around updateFetch's createRequest: while it lives, the request made is a retry (its worker sends one HTTP
    // attempt, with the answer's headers).
    class ReissueScope
    {
    public:
        explicit ReissueScope(LLViewerFetchedTexture* tex);
        ~ReissueScope();
        ReissueScope(const ReissueScope&) = delete;
        ReissueScope& operator=(const ReissueScope&) = delete;
    };
    bool reissueAllowed();

    // GrayWire: the texture has no image and waits for a retry (drawn gray at once).
    bool waiting(const LLViewerFetchedTexture* tex);

    // research/302: the schedule (or the pause) holds updateFetch's visits: retryDue() would say no, without its counting.
    bool retryHeld(LLViewerFetchedTexture* tex);

    // LLViewerFetchedTexture::setIsMissingAsset (flagged for good) and LLViewerTextureList::createImage (a texture
    // made): each logged, bounded per session; a texture made again for an id flagged before is counted, and when
    // its image comes.
    void noteFlagged(LLViewerFetchedTexture* tex);
    void noteCreated(LLViewerFetchedTexture* tex);

    // LLTextureFetchWorker::onCompleted, for a request of a retry that failed: a few answers' headers per session
    // (which of the CDN or the origin said so), kept to be logged by logWindow on the main thread. Any thread.
    void noteReply(const LLUUID& id, const LLCore::HttpStatus& status, const std::shared_ptr<LLCore::HttpHeaders>& headers);

    // LLTextureFetchWorker::onCompleted, a texture's HTTP request ended (research/299 section 8: why a 403 for the
    // range the viewer asks and not for the one it asks when the camera comes near). Logged by logWindow: every 4xx
    // answer but 404 (the request's range, the stage, the levels asked, the answer's header names and the first
    // bytes of its body), and the first success of an id that was refused before (its range and levels), a few
    // lines per session. Any thread.
    struct RequestEnd
    {
        LLUUID id;
        S32 http = 0;           // the HTTP status (0: not an HTTP status)
        bool ok = false;
        S32 offset = 0;         // the range asked: from offset, size bytes (0: to the end)
        S32 size = 0;
        S32 have_bytes = 0;     // bytes of the image the worker already had (0: the first step)
        S32 requested_discard = 0;
        S32 desired_discard = 0;    // (at the answer: the worker may have been given a new request since)
        S32 desired_size = 0;
        bool default_type = false;  // FTT_DEFAULT: only the default fetch type is told
        bool ranges_off = false;    // HttpRangeRequestsDisable: no range was sent
        bool retry = false;     // the worker is flagged as a retry of research/299 (at the answer)
        bool whole_file = false;
        S32 got_bytes = 0;      // the body's length
    };
    void noteRequestEnd(const RequestEnd& end, LLCore::HttpResponse* response);

    // A texture's fetch history on one line (research/302): the last HTTP status of its worker, the HTTP requests sent
    // (dropped workers added up), the retries, whether it was refused with a 4xx before. Main thread.
    std::string describe(LLViewerFetchedTexture* tex);

    // plus/texprio's logStats: the counters of the window, then they start again.
    void logWindow(F32 seconds);
}

#endif // PLUS_TEXPRIO_FETCHRETRY_H
