/**
 * @file selffirst.h
 * @brief The agent's own avatar first after login: a log of what the cloud
 *        waits for, and (PlusSelfFirst) its textures and meshes ahead of the
 *        world's.
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

#ifndef PLUS_SELFFIRST_H
#define PLUS_SELFFIRST_H

class LLUUID;
class LLVOAvatar;

// research/122 in the sl-viewer repository. The hooks return what they are
// given (or false) unless PlusSelfFirst is on and the agent's avatar is still
// loading after login (at most ACTIVE_SECONDS after the world shows); only the
// count of the world's texture requests (countWorldRequest) runs always.
namespace plus::selffirst
{
    // The <plus> hook in LLViewerFetchedTexture::updateFetch(): the priority
    // the texture is fetched with (upstream: its virtual size). The agent's
    // bakes and the textures of its attachments go above every world texture
    // (whose virtual size can reach LLViewerFetchedTexture::sMaxVirtualSize,
    // 8192^2, above the 2048^2 a boosted bake gets). Main thread.
    float fetchPriority(const LLUUID& id, float priority);

    // The <plus> hooks in LLTextureFetchWorker::acquireHttpSemaphore() and
    // releaseHttpSemaphore(): the world's HTTP texture requests in flight,
    // counted on their own (always, PlusSelfFirst on or off). true when the
    // request was counted as the world's (priority below the agent's from
    // fetchPriority); the worker gives it back with uncountWorldRequest() when
    // it releases the slot. Texture fetch thread: atomics only.
    bool countWorldRequest(float priority);
    void uncountWorldRequest();

    // Whether a fetch priority is one of the agent's own textures' (from
    // fetchPriority; research/294's counters tell them apart). Any thread.
    bool isOwnPriority(float priority);

    // The <plus> hooks in LLTextureFetchWorker::doWork() (WAIT_HTTP_RESOURCE)
    // and LLTextureFetch::releaseHttpWaiters(): true holds a world texture's
    // HTTP request (it waits for a slot, as when all are taken) while the
    // world's own requests in flight take its share of high_water. The
    // agent's (priority from fetchPriority) are never held, and never take
    // the world's share away. Texture fetch thread: reads atomics only.
    bool holdTexture(float priority, int high_water);

    // The <plus> hook at the start of LLTextureFetch::releaseHttpWaiters():
    // while the world's share is in flight, the waiters (mostly held world
    // textures, often thousands) are looked through at most WAITER_SCANS a
    // second instead of on every update of the fetch thread. Texture fetch
    // thread (or the main thread when the fetcher is not threaded).
    bool skipWaiterScan(int high_water);

    // The <plus> hook in calculate_score() (llmeshrepository.cpp): the score
    // of a mesh request of a rigged or attached volume, avatar its avatar.
    // The agent's go above every world request. Main thread.
    float meshScore(const LLVOAvatar* avatar, float score);

    // The <plus> hook in LLMeshRepository::notifyLoadedMeshes(): how many
    // pending mesh requests may go now (upstream: push_count). The world's
    // requests (active_count less the agent's meshes still waiting) are held
    // to the world's share of high_water; the agent's waiting meshes may go
    // on top of it (they are sorted first). Main thread.
    int meshPushCount(int push_count, int active_count, int high_water);
}

#endif // PLUS_SELFFIRST_H
