/**
 * @file llvktracker.h
 * @brief What native Vulkan renderers may ask the GL state tracker (llvkgl).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#ifndef LL_LLVKTRACKER_H
#define LL_LLVKTRACKER_H

#include "llvkmirror.h"
#include "llrhiframe.h"     // research/208 S0: the census times the main thread's takes
#if LL_WINDOWS
#include "llwin32headers.h"    // research/156 T14: SRWLOCK
#endif

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The tracker's lock (llvkgl's sLock; LLVKTracker::lock()). A recursive
// mutex. Private base: std::lock_guard<std::recursive_mutex> on it does not
// compile, so every lock goes through lock() here.
// Research/156 T14 (LLRHI_TRACKER_SRW=1 at startup, off by default; Windows):
// an SRW lock with its holder's thread id instead of the recursive mutex (a
// call into the C++ runtime's DLL at every take): a nested take only moves
// the depth, the first one takes the SRW lock. Chosen once, when the mutex
// is made, never while it is held.
class LLVKTrackerMutex : private std::recursive_mutex
{
public:
    LLVKTrackerMutex();

    void lock()
    {
#if LL_WINDOWS
        if (mSRW)
        {
            const U32 me = (U32)GetCurrentThreadId();
            if (mOwner.load(std::memory_order_relaxed) != me)
            {
                if (!TryAcquireSRWLockExclusive(&mSRWLock))
                {
                    mWaits.fetch_add(1, std::memory_order_relaxed);
                    const LLRHI::CensusLockWait census;     // research/208 S0
                    AcquireSRWLockExclusive(&mSRWLock);
                }
                mOwner.store(me, std::memory_order_relaxed);
            }
            entered();
            return;
        }
#endif
        if (!std::recursive_mutex::try_lock())
        {
            mWaits.fetch_add(1, std::memory_order_relaxed);    // held by another thread
            const LLRHI::CensusLockWait census;     // research/208 S0
            std::recursive_mutex::lock();
        }
        entered();
    }
    // Research/180 9 3: takes (lock(), any thread) that found the lock held
    // by another thread and waited, since the last call.
    U32 takeWaits() { return mWaits.exchange(0, std::memory_order_relaxed); }
    bool try_lock()
    {
#if LL_WINDOWS
        if (mSRW)
        {
            const U32 me = (U32)GetCurrentThreadId();
            if (mOwner.load(std::memory_order_relaxed) != me)   // an SRW lock's try is not reentrant
            {
                if (!TryAcquireSRWLockExclusive(&mSRWLock))
                {
                    return false;
                }
                mOwner.store(me, std::memory_order_relaxed);
            }
            entered();
            return true;
        }
#endif
        if (!std::recursive_mutex::try_lock())
        {
            return false;
        }
        entered();
        return true;
    }
    void unlock()
    {
        if (!--mDepth && LLRHI::renderThreadCensusOn())
        {
            LLRHI::renderThreadCensusLockLeft();    // research/208 S0 (the main thread's only)
        }
#if LL_WINDOWS
        if (mSRW)
        {
            if (!mDepth)
            {
                if (mOwner.load(std::memory_order_relaxed) != (U32)GetCurrentThreadId())
                {
                    noteForeignUnlock();
                }
                mOwner.store(0, std::memory_order_relaxed);     // before the release: a waiter must not see itself
                ReleaseSRWLockExclusive(&mSRWLock);
            }
            return;
        }
#endif
        std::recursive_mutex::unlock();
    }

private:
    void entered()
    {
        if (!mDepth++ && LLRHI::renderThreadCensusOn())
        {
            LLRHI::renderThreadCensusLockTaken();   // research/208 S0 (the main thread's only)
        }
    }
    // Released on another thread than the one that took it (logged once).
    void noteForeignUnlock();

    U32 mDepth = 0;     // how deep the holder holds it (only the holder touches it)
    std::atomic<U32> mWaits{ 0 };   // research/180 9 3: takeWaits()
#if LL_WINDOWS
    bool mSRW = false;
    SRWLOCK mSRWLock = SRWLOCK_INIT;
    std::atomic<U32> mOwner{ 0 };   // the holder's thread id (0: none); compared only with the reader's own
#endif
};

// GL still creates and fills every buffer and texture; llvkgl keeps their
// Vulkan copies. Native renderers (llvknative) draw with those copies through
// this narrow interface instead of reaching into the tracker's internals.
// Everything here runs on the render thread; take lock() for a whole pass.
namespace LLVKTracker
{
    typedef unsigned int GLName;

    LLVKTrackerMutex& lock();

    // Frames recorded so far (resources released in frame f are reusable from f + 2).
    U32 frame();

    // The Vulkan copy of a GL buffer, with pending GL writes uploaded.
    VkBuffer drawBuffer(GLName buffer);

    // A GL buffer's contents as GL has them now (the tracker's CPU copy), and
    // their size; null when there is no such buffer. Valid until the next GL
    // call on the buffer.
    const U8* bufferContents(GLName buffer, size_t& size);
    // Called with a GL buffer name (under lock()) whenever GL writes the
    // buffer's contents or deletes it.
    void setBufferChangedCallback(void (*callback)(GLName buffer));
    // One more such callback, called after that one (for a second user of
    // the buffers' contents: the attribute pool, llvkattribpool).
    void addBufferChangedCallback(void (*callback)(GLName buffer));

    // A GL texture as sampled by a native pass. Missing or incomplete
    // textures come back as a 1x1 white stand-in.
    struct Sampled
    {
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        U64 serial = 0;     // the view's serial (LLVKBindless compares it, texture-leak 2026-10-27)
    };
    Sampled sampledTexture(GLName texture);

    // GL state that native pipelines bake in.
    struct PipelineState
    {
        bool depth_clamp = false;
        U32 polygon_mode = 0;
    };
    PipelineState pipelineState();

    // Opens a rendering scope on the current GL draw framebuffer: the sampled
    // textures are moved to shader-read layout first, dynamic state follows the
    // current GL state, and the generic path's binding cache is reset (the
    // native pass binds its own pipelines and sets). Null when the framebuffer
    // is incomplete or no frame is being recorded, or when a sampled texture
    // is one of its color attachments. The framebuffer's own depth may be
    // sampled: it is then attached read-only (target.depth_read_only, layout
    // DEPTH_STENCIL_READ_ONLY_OPTIMAL for the sampler too) and the pass's
    // pipelines must not write depth or stencil.
    VkCommandBuffer beginPass(const std::vector<GLName>& sampled, LLVKMirror::Target& target);
    // The cull mode beginPass set from the GL state (right after it, for a
    // native pass that changes it per draw and sets it back: research/137 N7).
    VkCullModeFlags passCullMode();
    // For a native pass that renders to a target of its own instead (a
    // detached window's image): moves the sampled textures to shader-read
    // layout in the frame's commands and resets the generic path's binding
    // cache, without opening a scope on the GL framebuffer. False when no
    // frame is being recorded. Closed with endPass like beginPass.
    bool prepareSampled(const std::vector<GLName>& sampled);
    // Closes a native pass whose pipelines keep only some of that dynamic
    // state dynamic (binding them leaves the rest undefined): the next
    // generic draw sets all of it again. Under lock(), after its last draw.
    void endPass();

    // Diagnostics (research/105, LLRHI_DETACH_LOG=4), under lock().
    // The current GL draw framebuffer as the tracker resolves it, and the GL
    // state a clear or a draw into it would use, in words.
    std::string describeDrawTarget();
    // A GL texture's Vulkan image and views, in words.
    std::string describeTexture(GLName texture);
    // Records, in the frame's commands, copies of texels of a GL texture's
    // level 0 (x, y: GL rows, bottom up) into buffer from offset, 4 bytes
    // each; the texture is left in shader-read layout. False when it has no
    // image of 4-byte texels or no frame is being recorded.
    bool copyTexels(GLName texture, const std::vector<std::pair<S32, S32>>& points, VkBuffer buffer, VkDeviceSize offset);

    // Called with a GL texture name just before the texture is deleted.
    void setTextureDeletedCallback(void (*callback)(GLName texture));
    // A second such callback (the GPU probe list, research/50 step 4).
    void addTextureDeletedCallback(void (*callback)(GLName texture));
}

#endif // LL_LLVKTRACKER_H
