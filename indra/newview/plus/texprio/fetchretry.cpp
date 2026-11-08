/**
 * @file fetchretry.cpp
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

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "fetchretry.h"
#include "texprio.h"
#include "plus/httpautotune/httpautotune.h"

#include "httpcommon.h"
#include "bufferarray.h"
#include "httpheaders.h"
#include "httpresponse.h"
#include "llappviewer.h"
#include "llrhi.h"
#include "lltexturefetch.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace plus::texprio::fetchretry
{
namespace
{
    // Why a fetch failed, for the waits.
    enum Kind : U8
    {
        KIND_PERMANENT = 0,     // 404, 410, a decode that failed for good: it is not there
        KIND_UNKNOWN,           // 403 and the other 4xx; no data and no error
        KIND_RATE,              // 429: the server asks for less
        KIND_LIBRARY,           // 5xx, 499, connection and timeout errors: libcurl has tried these already
        KIND_COUNT
    };

    // The waits (seconds) before the 1st, 2nd... retry. [kind][off screen, on screen]
    constexpr U32 MAX_TRIES = 4;
    struct Waits
    {
        U32 count;
        F32 seconds[MAX_TRIES];
    };
    constexpr Waits WAITS[KIND_COUNT][2] = {
        { { 0, {} }, { 0, {} } },                                                           // permanent
        { { 2, { 60.f, 180.f } }, { 4, { 5.f, 15.f, 45.f, 120.f } } },                      // unknown
        { { 2, { 120.f, 300.f } }, { 3, { 10.f, 30.f, 90.f } } },                           // 429
        { { 2, { 180.f, 600.f } }, { 2, { 60.f, 180.f } } },                                // libcurl has tried
    };
    constexpr F64 OFF_SCREEN_BACKSTOP_SECONDS = 120.0;   // an off-screen retry goes out this long after its time at the latest
    constexpr F64 ON_SCREEN_EARLIEST_SECONDS = 5.0;      // a retry scheduled off screen goes out this soon after the failure once in view
    constexpr F32 JITTER = 0.2f;
    constexpr size_t FLAGGED_MAX = 4096;
    constexpr U32 LINES_MAX = 200;                       // logged flags and re-creations per session
    constexpr U32 REPLIES_MAX = 6;                       // logged answers' headers per session
    constexpr size_t REFUSED_MAX = 512;
    constexpr S32 RANGE_END_MAX = 20000000;              // upstream asks to the end of the file past this (HTTP_REQUESTS_RANGE_END_MAX)
    constexpr F64 DOWN_NO_DATA_SECONDS = 20.0;           // connection errors and no HTTP data for this long: the line is down
    constexpr U32 DOWN_ERRORS = 3;                       // this many connection errors within DOWN_ERROR_SECONDS
    constexpr F64 DOWN_ERROR_SECONDS = 10.0;
    constexpr U32 BURST_403 = 8;                         // 403s in BURST_SECONDS: all retries wait as for a busy server
    constexpr F64 BURST_SECONDS = 10.0;
    // The bucket needs this many tokens for a retry of: near on screen, the rest on screen, off screen (and
    // OFF_SCREEN_PER_SECOND at most): the nearest go first when the bucket runs low.
    constexpr F64 TOKENS_NEAR = 1.0;
    constexpr F64 TOKENS_VIEW = 2.0;
    constexpr F64 TOKENS_OFF = 3.0;

    // Counters of the window (main thread).
    struct Counts
    {
        U32 failures[KIND_COUNT] = {};
        U32 short_ends = 0;         // of those, a second step that ended coarse (the worker DONE)
        U32 scheduled = 0;
        U32 issued = 0;
        U32 succeeded = 0;
        U32 exhausted = 0;
        U32 held = 0;               // visits held by the pause, the second's limits or a line that is down
        U32 recreated = 0;          // a texture made again for an id flagged before
        U32 recreated_loaded = 0;   // ... and its image came
    };
    Counts sCounts;

    F64 sOverloadAt = -1.0e9;       // the last 429 / 503 (or a burst of 403s) a failed fetch of ours saw
    F64 sTokens = (F64)RETRIES_PER_SECOND;  // the retries' bucket: RETRIES_PER_SECOND a second, that many at most
    F64 sTokensAt = 0.0;
    F64 sOffSecond = 0.0;           // the whole second the off-screen retries count
    U32 sOffUsed = 0;
    F64 sBurstAt = 0.0;             // 403s in a short time: a limit of the CDN, not of one texture
    U32 sBurst = 0;
    std::atomic<U64> sLastHttpDataClock{ 0 };   // the last HTTP answer with texture data (any thread): the line is up
    F64 sConnErrAt = 0.0;           // connection errors in a short time with no HTTP data for long: the line is down
    U32 sConnErrors = 0;
    U32 sLines = 0;
    std::mutex sReplyMutex;                 // the answers' lines come from the fetch thread, are logged by the main thread
    std::vector<std::string> sReplyLines;
    std::atomic<U32> sReplyQueued{ 0 };     // lines queued in all, at most REPLIES_MAX
    bool sReissue = false;          // inside a ReissueScope (the request being made is a retry)
    bool sShortEnd = false;         // inside a ReissueScope (the request made may end on its first bytes: retries are for it)
    std::unordered_set<LLUUID> sFlagged;    // ids flagged missing (bounded), for noteCreated

    // Whether it is on, as the main thread last saw it.
    bool enabled()
    {
        static LLCachedControl<bool> setting(gSavedSettings, "PlusTexFetchRetry", true);
        static const bool env = LLRHI::envDefaultOn("LLRHI_TEX_FETCH_RETRY");
        return env && setting();
    }

    // How many times a failure of the unknown kind (403 and the other 4xx) is asked for again at most (research/305: the
    // live log's 74 refused textures were refused on each of up to 4 tries and none passed): the table's count or less.
    U32 unknownTries()
    {
        static LLCachedControl<U32> tries(gSavedSettings, "PlusTexFetchRetryUnknownTries", 1);
        return llmin((U32)tries(), MAX_TRIES);
    }

    F64 timeNow()
    {
        return LLTimer::getTotalSeconds();
    }

    // On screen: in view, boosted, used with no face (a projector light image, the terrain textures), or drawn as the
    // gray stand-in lately (research/304: the face scan may list no face for it: tier none).
    bool onScreen(LLViewerFetchedTexture* tex)
    {
        return tex->mRhiViewTier == TIER_NEAR || tex->mRhiViewTier == TIER_VIEW || tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH ||
               tex->getBoostLevel() == LLGLTexture::BOOST_TERRAIN || tex->getNumVolumes(LLRender::LIGHT_TEX) > 0 ||
               plus::texprio::grayDrawnNow(tex);
    }

    bool nearOnScreen(LLViewerFetchedTexture* tex)
    {
        return tex->mRhiViewTier == TIER_NEAR || tex->getBoostLevel() >= LLGLTexture::BOOST_HIGH;
    }

    // How the last request of this texture's worker ended. No worker: the texture's own last result.
    struct End
    {
        LLCore::HttpStatus status;
        bool decode_failed = false;
        U32 http_tries = 0;
        bool has_worker = false;    // else status and http_tries are the defaults, not an answer
    };
    End endOf(LLViewerFetchedTexture* tex)
    {
        End end;
        // (no worker: it was cancelled; the status stays the default success, "not known")
        end.has_worker = LLAppViewer::getTextureFetch()->plusEndOfRequest(tex->getID(), end.status, end.decode_failed, end.http_tries);
        return end;
    }

    Kind classify(const End& end)
    {
        // (a decode that failed after a failed HTTP request is the HTTP's: the first bytes it had were not whole)
        if (end.decode_failed && end.status)
        {
            return KIND_PERMANENT;
        }
        const LLCore::HttpStatus& status = end.status;
        if (status.isHttpStatus())
        {
            const U32 code = status.getType();
            if (code == 404 || code == 410)
            {
                return KIND_PERMANENT;
            }
            if (code == 429)
            {
                return KIND_RATE;
            }
            if (code >= 499 && code <= 599)
            {
                return KIND_LIBRARY;
            }
            return KIND_UNKNOWN;
        }
        // Not an HTTP reply: a connection or timeout error is the line's; the rest (a success value with no data
        // too: the worker ended some other way) is not known.
        return !status && status.isRetryable() ? KIND_LIBRARY : KIND_UNKNOWN;
    }

    bool overloaded(F64 now)
    {
        return now - sOverloadAt < OVERLOAD_SCALE_SECONDS || plus::httpautotune::throttledWithin(OVERLOAD_SCALE_SECONDS);
    }

    // +-JITTER from the texture id and the try number: the same texture does not retry in step with another.
    F32 jitter(LLViewerFetchedTexture* tex, U32 attempt)
    {
        const U32 h = (tex->getID().getCRC32() + attempt * 2654435761u) % 1000u;
        return 1.f + JITTER * (2.f * (F32)h / 999.f - 1.f);
    }

    // Whether this is a plain world texture, which a retry is for: fetched by id from the asset server (not bakes,
    // map tiles, local files, media or the textures made from memory).
    bool plain(LLViewerFetchedTexture* tex)
    {
        return tex->getFTType() == FTT_DEFAULT && !tex->isForSculptOnly() && tex->getBoostLevel() < LLGLTexture::BOOST_ICON &&
               (tex->getType() == LLViewerTexture::FETCHED_TEXTURE || tex->getType() == LLViewerTexture::LOD_TEXTURE);
    }

    // The bucket, filled to RETRIES_PER_SECOND at that rate.
    void refill(F64 now)
    {
        sTokens = llmin((F64)RETRIES_PER_SECOND, sTokens + (now - sTokensAt) * (F64)RETRIES_PER_SECOND);
        sTokensAt = now;
        if (std::floor(now) != sOffSecond)
        {
            sOffSecond = std::floor(now);
            sOffUsed = 0;
        }
    }

    bool budgetLeft(F64 now, LLViewerFetchedTexture* tex)
    {
        refill(now);
        if (nearOnScreen(tex))
        {
            return sTokens >= TOKENS_NEAR;
        }
        if (onScreen(tex))
        {
            return sTokens >= TOKENS_VIEW;
        }
        return sTokens >= TOKENS_OFF && sOffUsed < OFF_SCREEN_PER_SECOND;
    }

    // The line is down: several connection errors at once and no HTTP answer with data for a while. No try is used then.
    bool lineDown(F64 now, bool connection_error)
    {
        if (!connection_error)
        {
            return false;
        }
        if (now - sConnErrAt > DOWN_ERROR_SECONDS)
        {
            sConnErrAt = now;
            sConnErrors = 0;
        }
        ++sConnErrors;
        const U64 at = sLastHttpDataClock.load(std::memory_order_relaxed);
        const U64 clock = LLTimer::getCurrentClockCount();
        const F64 quiet = at && clock >= at ? (F64)(clock - at) / (F64)get_timer_info().mClockFrequency : at ? 0.0 : 1.0e9;
        return sConnErrors >= DOWN_ERRORS && quiet > DOWN_NO_DATA_SECONDS;
    }

    // Schedules the next retry of tex for a failure of this kind; false: no tries left.
    bool schedule(LLViewerFetchedTexture* tex, Kind kind, const LLCore::HttpStatus& status, bool refused_first = false)
    {
        const F64 now = timeNow();
        if (status.isHttpStatus() && (status.getType() == 429 || status.getType() == 503))
        {
            sOverloadAt = now;      // a loaded server: the retries pause and wait twice as long for a while
        }
        else if (status.isHttpStatus() && status.getType() == 403)
        {
            if (now - sBurstAt > BURST_SECONDS)
            {
                sBurstAt = now;
                sBurst = 0;
            }
            if (++sBurst >= BURST_403)
            {
                sOverloadAt = now;  // many 403s at once are a limit of the CDN: as for a busy server
            }
        }
        const bool on_screen = onScreen(tex);
        const Waits& waits = WAITS[kind][on_screen ? 1 : 0];
        const U32 tries = tex->mRhiRetryCount;
        const bool down = lineDown(now, kind == KIND_LIBRARY && !status.isHttpStatus() && !status);
        // Only the refusals the live log showed (research/305), decided by onFetchFailure: an HTTP 4xx answer of the
        // unknown kind (403...) for a first step with no image yet. A lost request, a failure with no HTTP answer, a
        // short end (a second step, the first bytes there) keep the table's tries.
        const U32 allowed = refused_first && kind == KIND_UNKNOWN ? llmin(waits.count, unknownTries()) : waits.count;
        if (tries >= allowed && !down)      // (a line that is down is waited out: it does not use up the last try)
        {
            ++sCounts.exhausted;
            return false;
        }
        const F32 scale = overloaded(now) ? 2.f : 1.f;
        const U32 step = llmin(tries, waits.count - 1);     // (waits.count is not 0 here: a line that is down is a library failure)
        tex->mRhiRetryAt = now + (F64)(waits.seconds[step] * scale * jitter(tex, tries));
        tex->mRhiRetryFailAt = now;
        tex->mRhiRetryOff = !on_screen;
        if (!down)     // a line that is down does not use up a try
        {
            tex->mRhiRetryCount = (U8)(tries + 1);
        }
        tex->mRhiRetryInFlight = false;
        ++sCounts.scheduled;
        return true;
    }

    std::string clean(const std::string* value)
    {
        if (!value)
        {
            return "-";
        }
        std::string out = value->substr(0, 80);
        for (char& c : out)
        {
            if (c < 0x20 || c > 0x7e)
            {
                c = '?';
            }
        }
        return out;
    }
}

bool switchedOn()
{
    return enabled();
}

bool applies(LLViewerFetchedTexture* tex)
{
    return enabled() && plain(tex);
}

bool shortEndAllowed()
{
    return sShortEnd;
}

bool onFetchFailure(LLViewerFetchedTexture* tex)
{
    if (!enabled() || !plain(tex))
    {
        return false;
    }
    const End end = endOf(tex);
    tex->mRhiHttpTries += end.http_tries;   // (the worker is dropped at each retry: the texture adds them up, once)
    const Kind kind = classify(end);
    ++sCounts.failures[kind];
    if (kind == KIND_PERMANENT)
    {
        return false;
    }
    // (a 4xx answer: not a 1xx, 2xx, 3xx or 6xx one; and no image yet: the first step)
    const bool refused_first = kind == KIND_UNKNOWN && end.status.isHttpStatus() && !end.status && end.status.getType() >= 400 &&
                               end.status.getType() < 500 && tex->getDiscardLevel() < 0;
    return schedule(tex, kind, end.status, refused_first);
}

ShortEnd onShortEnd(LLViewerFetchedTexture* tex)
{
    if (!enabled() || !plain(tex) || tex->mRhiRetryAt > 0.0)
    {
        return ShortEnd::None;  // (one already scheduled: the visits wait for it)
    }
    const End end = endOf(tex);
    if (end.status || end.decode_failed)
    {
        return ShortEnd::None;  // it ended with what it could get (a short file): nothing failed
    }
    tex->mRhiHttpTries += end.http_tries;
    const Kind kind = classify(end);
    ++sCounts.short_ends;
    ++sCounts.failures[kind];
    if (kind == KIND_PERMANENT)
    {
        return ShortEnd::GaveUp;
    }
    return schedule(tex, kind, end.status) ? ShortEnd::Scheduled : ShortEnd::GaveUp;
}

void onFetchSuccess(LLViewerFetchedTexture* tex, bool reached_level)
{
    tex->mRhiRetryInFlight = false;     // (data came: the retry that was out has ended, whatever level it reached)
    if (tex->mRhiRetryCount > 0 && reached_level)     // (the first bytes alone are not the end of the tries)
    {
        ++sCounts.succeeded;
        tex->mRhiRetryCount = 0;
        tex->mRhiRetryAt = 0.0;
    }
    if (tex->mRhiAfterMissing)
    {
        ++sCounts.recreated_loaded;
        tex->mRhiAfterMissing = false;
    }
}

bool retryDue(LLViewerFetchedTexture* tex)
{
    if (tex->mRhiRetryAt <= 0.0 || !enabled())
    {
        return true;    // (switched off while it waited: asked at the next visit)
    }
    const F64 now = timeNow();
    const bool on_screen = onScreen(tex);
    // One scheduled off screen goes out soon after it comes into view (but not within ON_SCREEN_EARLIEST_SECONDS of
    // the failure: the server has not had a moment).
    const bool came_into_view = tex->mRhiRetryOff && on_screen && now >= tex->mRhiRetryFailAt + ON_SCREEN_EARLIEST_SECONDS;
    if (now < tex->mRhiRetryAt && !came_into_view)
    {
        return false;
    }
    if (!on_screen && now < tex->mRhiRetryAt + OFF_SCREEN_BACKSTOP_SECONDS)
    {
        return false;   // off screen: when it comes into view, or at the backstop
    }
    if (gDisconnected || now - sOverloadAt < OVERLOAD_PAUSE_SECONDS || plus::httpautotune::throttledWithin(OVERLOAD_PAUSE_SECONDS) ||
        !budgetLeft(now, tex))
    {
        ++sCounts.held;
        return false;
    }
    return true;
}

void noteRequested(LLViewerFetchedTexture* tex)
{
    if (tex->mRhiRetryAt <= 0.0)
    {
        return;
    }
    const F64 now = timeNow();
    refill(now);
    sTokens = llmax(0.0, sTokens - 1.0);
    sOffUsed += onScreen(tex) ? 0 : 1;
    ++sCounts.issued;
    tex->mRhiRetryAt = 0.0;     // out; a new failure schedules the next (mRhiRetryCount keeps the tries)
    tex->mRhiRetryInFlight = true;
}

ReissueScope::ReissueScope(LLViewerFetchedTexture* tex)
{
    const bool on = applies(tex);
    sShortEnd = on;
    sReissue = on && tex->mRhiRetryAt > 0.0;    // only a retry that retryDue() let through has it set at this point
}

ReissueScope::~ReissueScope()
{
    sReissue = false;
    sShortEnd = false;
}

bool reissueAllowed()
{
    return sReissue;
}

bool retryHeld(LLViewerFetchedTexture* tex)
{
    // retryDue()'s refusals by time, place and pause, without its counting; the token budget (budgetLeft) is not read
    // (a visit held only by the budget shows as the next reason, as it is not the schedule that holds it)
    if (tex->mRhiRetryAt <= 0.0 || !enabled())
    {
        return false;
    }
    const F64 now = timeNow();
    const bool on_screen = onScreen(tex);
    const bool came_into_view = tex->mRhiRetryOff && on_screen && now >= tex->mRhiRetryFailAt + ON_SCREEN_EARLIEST_SECONDS;
    if (now < tex->mRhiRetryAt && !came_into_view)
    {
        return true;
    }
    if (!on_screen && now < tex->mRhiRetryAt + OFF_SCREEN_BACKSTOP_SECONDS)
    {
        return true;
    }
    return gDisconnected || now - sOverloadAt < OVERLOAD_PAUSE_SECONDS || plus::httpautotune::throttledWithin(OVERLOAD_PAUSE_SECONDS);
}

bool waiting(const LLViewerFetchedTexture* tex)
{
    return tex->mRhiRetryCount > 0 && (tex->mRhiRetryAt > 0.0 || tex->mRhiRetryInFlight) && tex->getDiscardLevel() < 0 &&
           !tex->isMissingAsset() && enabled();
}

bool onLostRequest(LLViewerFetchedTexture* tex)
{
    if (!tex->mRhiRetryInFlight)
    {
        return false;
    }
    {
        // (a worker that is there ended by itself: onFetchFailure classifies that)
        LLCore::HttpStatus status;
        bool decode_failed = false;
        U32 tries = 0;
        if (LLAppViewer::getTextureFetch()->plusEndOfRequest(tex->getID(), status, decode_failed, tries))
        {
            return false;
        }
    }
    // A retry went out and its worker is gone without a result (another texture of the same id cancelled it, it
    // was let go at priority 0): it used its try. Scheduled again, within the limits, not asked at once.
    tex->mRhiRetryInFlight = false;
    return schedule(tex, KIND_UNKNOWN, LLCore::HttpStatus());
}

void noteHttpData()
{
    sLastHttpDataClock.store(LLTimer::getCurrentClockCount(), std::memory_order_relaxed);
}

void noteFlagged(LLViewerFetchedTexture* tex)
{
    if (!plain(tex) || tex->getTextureListType() != TEX_LIST_STANDARD)
    {
        return;     // (the textures a retry is for, in the ordinary list: map tiles and icons would fill the set)
    }
    if (sFlagged.size() < FLAGGED_MAX)
    {
        sFlagged.insert(tex->getID());
    }
    if (sLines < LINES_MAX)
    {
        ++sLines;
        const End end = endOf(tex);
        LL_INFOS("TextureMemory") << "texture flagged missing for good (research/299): " << tex->getID() << ", tier "
                                  << tierName(tex->mRhiViewTier) << ", retries " << (S32)tex->mRhiRetryCount << ", HTTP requests sent "
                                  << (tex->mRhiHttpTries > 0 ? tex->mRhiHttpTries : end.http_tries) << ", last HTTP status " << end.status.toHex()
                                  << (end.decode_failed ? ", decode failed" : "") << ", faces " << tex->getTotalNumFaces() << LL_ENDL;
    }
}

void noteCreated(LLViewerFetchedTexture* tex)
{
    if (sFlagged.empty() || tex->getTextureListType() != TEX_LIST_STANDARD)
    {
        return;
    }
    if (sFlagged.erase(tex->getID()) > 0)
    {
        tex->mRhiAfterMissing = true;
        ++sCounts.recreated;
        if (sLines < LINES_MAX)
        {
            ++sLines;
            LL_INFOS("TextureMemory") << "texture made again for an id flagged missing before (research/299): " << tex->getID() << LL_ENDL;
        }
    }
}

void noteReply(const LLUUID& id, const LLCore::HttpStatus& status, const std::shared_ptr<LLCore::HttpHeaders>& headers)
{
    if (!status.isHttpStatus() || !headers || sReplyQueued.load(std::memory_order_relaxed) >= REPLIES_MAX)
    {
        return;
    }
    // (LLCore keeps the names of the answer's headers in lower case.) Via is left out, and of x-cache only the first
    // word: both can carry the host name of a proxy of the user's.
    std::string cache = clean(headers->find("x-cache"));
    cache = cache.substr(0, cache.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"));
    std::string line = "failed retry answer (research/299): " + id.asString() + " HTTP " + std::to_string((S32)status.getType()) +
                       ", server " + clean(headers->find("server")) + ", x-cache " + cache + ", age " +
                       clean(headers->find("age")) + ", retry-after " + clean(headers->find("retry-after"));
    std::lock_guard<std::mutex> lock(sReplyMutex);
    if (sReplyQueued.load(std::memory_order_relaxed) < REPLIES_MAX)
    {
        sReplyLines.push_back(line);
        sReplyQueued.fetch_add(1, std::memory_order_relaxed);
    }
}

namespace
{
    bool headerNameOk(const std::string& name)
    {
        if (name.empty() || name.size() > 40)
        {
            return false;
        }
        for (char c : name)
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            {
                return false;
            }
        }
        return true;
    }

    bool wordChar(char c)
    {
        return std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '+' || c == '/' || c == '=' || c == '.' || c == ':' || c == '@';
    }

    // Four runs of digits with dots between, anywhere in the word (1.2.3.4, ip=1.2.3.4, 1.2.3.4:80)
    bool hasIpv4(const std::string& token)
    {
        for (size_t start = 0; start < token.size(); ++start)
        {
            size_t at = start;
            int groups = 0;
            while (groups < 4)
            {
                const size_t digits = at;
                while (at < token.size() && std::isdigit((unsigned char)token[at]))
                {
                    ++at;
                }
                if (at == digits)
                {
                    break;
                }
                ++groups;
                if (groups < 4)
                {
                    if (at >= token.size() || token[at] != '.')
                    {
                        break;
                    }
                    ++at;
                }
            }
            if (groups == 4)
            {
                return true;
            }
        }
        return false;
    }

    // The standard error codes of S3 and CloudFront (letters only, 12 or more of them: a request id or a token has
    // digits or is not a word of this list), kept as they are so that the body can be told (research/305).
    bool knownErrorCode(const std::string& token)
    {
        static const char* const CODES[] = { "accessdenied", "signaturedoesnotmatch", "requestexpired", "invalidrequest", "allaccessdisabled",
                                             "expiredtoken", "invalidaccesskeyid", "internalerror", "serviceunavailable", "accountproblem",
                                             "invalidargument", "missingsecurityheader", "methodnotallowed", "invalidsecurity",
                                             "authorizationheadermalformed", "requesttimetoolarge", "nosuchbucket", "badrequest" };
        std::string lower;
        for (char c : token)
        {
            if (!std::isalpha((unsigned char)c))
            {
                return false;
            }
            lower += (char)std::tolower((unsigned char)c);
        }
        for (const char* code : CODES)
        {
            if (lower == code)
            {
                return true;
            }
        }
        return false;
    }

    // A piece of a CDN's answer made safe to log: letters, digits, spaces and . , : ; ( ) / - _ only; a run of 12 or
    // more word characters (a request id, a token) or one with an @ is #; something with an IP address in it is #ip;
    // something that starts like a URL is url. Cut at 60 characters.
    std::string safeWords(const std::string& text)
    {
        std::string out;
        size_t i = 0;
        while (i < text.size() && out.size() < 60)
        {
            const char c = text[i];
            const bool http_start = i + 4 <= text.size() && std::tolower((unsigned char)text[i]) == 'h' &&
                                    std::tolower((unsigned char)text[i + 1]) == 't' && std::tolower((unsigned char)text[i + 2]) == 't' &&
                                    std::tolower((unsigned char)text[i + 3]) == 'p';
            if (http_start || text.compare(i, 2, "//") == 0 || text.compare(i, 2, "&#") == 0)
            {
                out += "url";
                break;
            }
            if (!wordChar(c))
            {
                out += (c == ' ' || c == ',' || c == ';' || c == '(' || c == ')') ? c : ' ';
                ++i;
                continue;
            }
            size_t j = i;
            while (j < text.size() && wordChar(text[j]))
            {
                ++j;
            }
            const std::string token = text.substr(i, j - i);
            std::string core = token;   // a full stop or colon at the end belongs to the sentence: only for the known codes
            while (!core.empty() && (core.back() == '.' || core.back() == ':'))
            {
                core.pop_back();
            }
            if (hasIpv4(token) || std::count(token.begin(), token.end(), ':') >= 2)
            {
                out += "#ip";      // (tested on the word as it is: 2001:db8:: has its colons)
            }
            else if (knownErrorCode(core))
            {
                out += token;
            }
            else if (token.size() >= 12 || token.find('@') != std::string::npos)
            {
                out += "#";
            }
            else
            {
                out += token;
            }
            i = j;
        }
        return out;
    }

    // What a refusal's body is, in a few safe words: the <title> or the <Code> of an HTML or XML answer, else its kind.
    std::string bodySummary(LLCore::BufferArray* body)
    {
        if (!body || body->size() == 0)
        {
            return "empty";
        }
        char buffer[2049];
        const size_t length = body->read(0, buffer, 2048);
        std::string text(buffer, length);
        std::string lower = text;
        for (char& c : lower)
        {
            c = (char)std::tolower((unsigned char)c);
        }
        for (const char* tag : { "title", "code" })
        {
            const std::string open = std::string("<") + tag + ">";
            const std::string close = std::string("</") + tag + ">";
            const size_t from = lower.find(open);
            if (from != std::string::npos)
            {
                const size_t to = lower.find(close, from + open.size());
                if (to != std::string::npos)
                {
                    return std::string(tag) + ": " + safeWords(text.substr(from + open.size(), std::min<size_t>(to - from - open.size(), 120)));
                }
            }
        }
        const bool markup = !text.empty() && text[0] == '<';
        bool printable_text = true;
        for (char c : text)
        {
            printable_text = printable_text && ((unsigned char)c >= 0x20 || c == '\n' || c == '\r' || c == '\t') && (unsigned char)c < 0x7f;
        }
        const char* kind = markup ? (lower.find("html") != std::string::npos ? "html" : "xml") : printable_text ? "text" : "binary";
        return std::string(kind) + " of " + std::to_string((S32)body->size()) + " bytes";
    }

    struct Refusal
    {
        S32 end = -2;           // the range's last byte (-1: to the end of the file) of the line logged last
        U32 repeats = 0;        // refusals since then
        U32 logged = 0;
    };
    std::unordered_map<LLUUID, Refusal> sRefusals;     // sReplyMutex
    std::atomic<U32> sRefusedCount{ 0 };               // entries in sRefusals (the success path skips the lock at 0)
    std::atomic<U32> sRefusedLines{ 0 };
    std::atomic<U32> sOkLines{ 0 };
    constexpr U32 REFUSED_LINES_MAX = 80;
    constexpr U32 OK_LINES_MAX = 40;
}

void noteRequestEnd(const RequestEnd& end, LLCore::HttpResponse* response)
{
    if (!end.default_type)     // FTT_DEFAULT only (map tiles, bakes and local files go the same way and refuse for their own reasons)
    {
        return;
    }
    const bool refused = !end.ok && end.http >= 400 && end.http < 500 && end.http != 404 && end.http != 416;
    if (!refused && !end.ok)
    {
        return;
    }
    if (end.ok && sRefusedCount.load(std::memory_order_relaxed) == 0)
    {
        return;
    }
    if (refused && sRefusedLines.load(std::memory_order_relaxed) >= REFUSED_LINES_MAX && sRefusedCount.load(std::memory_order_relaxed) >= REFUSED_MAX)
    {
        return;
    }
    const bool open_ended = end.size <= 0 || (S64)end.offset + end.size > RANGE_END_MAX;
    const S32 range_end = open_ended ? -1 : end.offset + end.size - 1;
    U32 earlier_refusals = 0;
    {
        // One lock: the id's record (a refusal is logged the first time and when the range's last byte changes; the
        // others are counted and told with the success line)
        std::lock_guard<std::mutex> lock(sReplyMutex);
        if (end.ok)
        {
            const auto found = sRefusals.find(end.id);
            if (found == sRefusals.end())
            {
                return;
            }
            earlier_refusals = found->second.logged + found->second.repeats;
            sRefusals.erase(found);
            sRefusedCount.store((U32)sRefusals.size(), std::memory_order_relaxed);
        }
        else
        {
            if (sRefusals.size() >= REFUSED_MAX && sRefusals.find(end.id) == sRefusals.end())
            {
                return;
            }
            Refusal& record = sRefusals[end.id];
            sRefusedCount.store((U32)sRefusals.size(), std::memory_order_relaxed);
            if (record.logged > 0 && record.end == range_end)
            {
                ++record.repeats;
                return;
            }
            record.end = range_end;
            ++record.logged;
        }
    }
    if ((end.ok ? sOkLines : sRefusedLines).load(std::memory_order_relaxed) >= (end.ok ? OK_LINES_MAX : REFUSED_LINES_MAX))
    {
        return;
    }
    std::string line = std::string(end.ok ? "ok after a refusal" : "refused") + " (research/299 section 8): " + end.id.asString() +
                       ", HTTP " + std::to_string(end.http) + ", " +
                       (end.ranges_off ? std::string("(ranges off: the whole file asked for)")
                                       : "range bytes=" + std::to_string(end.offset) + "-" +
                                             (open_ended ? std::string() : std::to_string(range_end))) +
                       ", " + (end.have_bytes > 0 ? "second" : "first") + " step (the image had " + std::to_string(end.have_bytes) +
                       " bytes), asked level " + std::to_string(end.requested_discard) + ", now wanted level " +
                       std::to_string(end.desired_discard) + ", now wanted size " + std::to_string(end.desired_size) +
                       (end.retry ? ", worker flagged as a retry" : ", worker not flagged as a retry") +
                       (end.whole_file ? ", the file is whole" : "") + ", body " + std::to_string(end.got_bytes) + " bytes";
    if (end.ok)
    {
        unsigned int offset = 0, length = 0, full = 0;
        if (response)
        {
            response->getRange(&offset, &length, &full);
        }
        // a 200 for the whole file has no Content-Range: its body is the file (a range from 0 that ended there)
        if (!full && end.http == 200 && end.offset == 0 && end.got_bytes > 0)
        {
            full = (unsigned int)end.got_bytes;
        }
        line += ", file size " + (full ? std::to_string(full) : std::string("not known")) + ", refused " + std::to_string(earlier_refusals) +
                " times before";
    }
    else if (response)
    {
        const std::shared_ptr<LLCore::HttpHeaders> headers = response->getHeaders();
        line += ", header names:";
        if (headers)
        {
            size_t shown = 0;
            for (const auto& header : *headers)
            {
                if (++shown > 14)
                {
                    break;
                }
                line += " " + (headerNameOk(header.first) ? header.first : std::string("?"));
            }
            if (headers->size() == 0)
            {
                line += " (none)";
            }
        }
        else
        {
            line += " (no headers kept: only a retry keeps them)";
        }
        line += ", body: " + bodySummary(response->getBody());
    }
    (end.ok ? sOkLines : sRefusedLines).fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(sReplyMutex);
    sReplyLines.push_back(line);
}

std::string describe(LLViewerFetchedTexture* tex)
{
    const End end = endOf(tex);
    bool refused = false;
    {
        std::lock_guard<std::mutex> lock(sReplyMutex);
        refused = sRefusals.count(tex->getID()) > 0;
    }
    // (the texture adds a worker's tries when it drops it: two numbers, the dropped workers' and the live worker's;
    // no worker: its status is not an answer)
    return "last HTTP status " + (end.has_worker ? end.status.toHex() : std::string("not known (no worker)")) + ", HTTP requests sent " +
           std::to_string(tex->mRhiHttpTries) + " by dropped workers, " + (end.has_worker ? std::to_string(end.http_tries) : std::string("0")) +
           " by the live worker, retries " + std::to_string((S32)tex->mRhiRetryCount) +
           (refused ? ", refused with a 4xx before" : ", not refused before");
}

void logWindow(F32 seconds)
{
    {
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lock(sReplyMutex);
            lines.swap(sReplyLines);
        }
        for (const std::string& line : lines)
        {
            LL_INFOS("TextureMemory") << line << LL_ENDL;
        }
    }
    const Counts& c = sCounts;
    LL_INFOS("TextureMemory") << llformat("texture fetch retry (research/299) over %.0f s [%s]: failed fetches, by why: permanent "
                                          "(404, 410, decode) %u, not known (403...) %u, 429 %u, libcurl tried already (5xx, connection) %u "
                                          "(second steps that ended coarse: %u); retries scheduled %u, sent %u, image came after a retry "
                                          "%u, tries used up (flagged missing) %u; visits held (pause, %u a second, line down) %u; "
                                          "textures made again for an id flagged before %u (image came %u)",
                                          seconds, enabled() ? "on" : "off", c.failures[KIND_PERMANENT], c.failures[KIND_UNKNOWN],
                                          c.failures[KIND_RATE], c.failures[KIND_LIBRARY], c.short_ends, c.scheduled, c.issued,
                                          c.succeeded, c.exhausted, RETRIES_PER_SECOND, c.held, c.recreated, c.recreated_loaded)
                              << LL_ENDL;
    sCounts = Counts();
}
}
