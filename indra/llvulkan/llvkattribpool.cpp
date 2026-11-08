/**
 * @file llvkattribpool.cpp
 * @brief GPU-driven drawing, stage G4b: every vertex attribute and the indices of
 * GL vertex/index buffer pairs pooled into large buffers (research/46).
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

#include "linden_common.h"

#include "llvkattribpool.h"

#include "llglheaders.h"
#include "llrhi.h"
#include "llrhiresource.h"    // research/197 stage 1b: LLRHI::Buffers
#include "llrhivbcensus.h"    // research/293 branch 0
#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    using GLName = LLVKTracker::GLName;

    // A chunk's vertex range (every stream has room for this many vertices) and
    // its index buffer (32-bit indices). The first chunk holds most scenes
    // (about 250 MB with the usual streams); the ones after it are a quarter of
    // that, so a scene that only just overflows the first does not take another
    // 250 MB of VRAM from the textures (research/54: the second chunk held 5% of
    // its room). A pair larger than that gets a chunk its size, up to the first's.
    constexpr U32 CHUNK_VERTICES = 1u << 21;
    constexpr U32 CHUNK_INDICES = 1u << 23;
    constexpr U32 MORE_CHUNK_VERTICES = 1u << 19;
    constexpr U32 MORE_CHUNK_INDICES = 1u << 21;
    // Allocation units, in vertices and in indices (shorter free lists).
    constexpr U32 VERTEX_GRANULE = 64;
    constexpr U32 INDEX_GRANULE = 64;
    constexpr U32 MAX_STRIDE = 64;
    constexpr U32 MAX_COPIED = 16;      // LLRHI::DirectDraw::MAX_ATTRIBUTES
    constexpr U32 MAX_RUN_DRAWS = 4096; // the most draws llvkgl puts in one indirect draw (RUN_MAX_CAPACITY)
    // Bytes copied into the pool per frame, through the frame's upload ring
    // (256 MB, shared with everything else uploaded in the frame).
    constexpr U64 COPY_BUDGET = 16ull * 1024 * 1024;
    // A range dropped while frame f is recorded was last read by frame f at the
    // latest; frame f + 2 starts after frame f finished (two in flight). One
    // more for drops between frames, after the counter moved.
    constexpr U32 RELEASE_DELAY = 3;
    static_assert(RELEASE_DELAY == LLVKMirror::FRAMES_IN_FLIGHT + 1, "RELEASE_DELAY: the frames in flight and one more");
    constexpr U32 IDLE_FRAMES = 1800;   // pairs not drawn for this long go; copied again when drawn
    constexpr U32 SWEEP_FRAMES = 256;
    // (research/218 vk-s7g, research/207 7.1 T8: the redeem memo of
    // research/180 10.20 is retired. It answered tickets by GL name only; with
    // the vertex buffer numbers on, the draw templates' and packets' tickets go
    // by number, and a pair without a number is redeemed the full way.)
    // A GL buffer whose copies were dropped twice within this many frames is
    // being rewritten (flexible prims, scripted changes): drawn from GL until
    // it has been left alone this long.
    constexpr U32 CHURN_FRAMES = 120;
    // Research/197 stage 1b (vertex buffer numbers): in verify, 1 in this many
    // numbers keeps a place (a power of two); a sampled place is compared with
    // GL's copy at most once in this many frames, and a frame compares at most
    // this many bytes. A released number is given out again this many frames
    // later. The auto mode: this many frames of the world and this many
    // comparisons with no mismatch, then on.
    constexpr U32 NUMBER_VERIFY_SAMPLE = 16;
    constexpr U32 NUMBER_CHECK_FRAMES = 16;
    constexpr U64 NUMBER_CHECK_BYTES = 1024 * 1024;
    constexpr U32 NUMBER_RETIRE_FRAMES = 8;
    constexpr U32 NUMBER_MAX = 1u << 22;
    constexpr U32 NUMBER_AUTO_FRAMES = 600;
    constexpr U64 NUMBER_AUTO_CHECKS = 300;

    U64 round_up(U64 value, U64 unit)
    {
        return (value + unit - 1) / unit * unit;
    }

    // Free ranges of one chunk: best fit, freed ranges merged with their neighbours.
    class RangeAllocator
    {
    public:
        void reset(U32 size)
        {
            mBySize.clear();
            mByOffset.clear();
            insert(0, size);
        }

        bool allocate(U32 size, U32& offset)
        {
            auto it = mBySize.lower_bound(size);
            if (it == mBySize.end())
            {
                return false;
            }
            const U32 have = it->first;
            const U32 at = it->second;
            mBySize.erase(it);
            mByOffset.erase(at);
            if (have > size)
            {
                insert(at + size, have - size);
            }
            offset = at;
            return true;
        }

        void release(U32 offset, U32 size)
        {
            auto next = mByOffset.lower_bound(offset);
            if (next != mByOffset.end() && offset + size == next->first)
            {
                size += next->second;
                eraseBySize(next->first, next->second);
                next = mByOffset.erase(next);
            }
            if (next != mByOffset.begin())
            {
                auto prev = std::prev(next);
                if (prev->first + prev->second == offset)
                {
                    offset = prev->first;
                    size += prev->second;
                    eraseBySize(prev->first, prev->second);
                    mByOffset.erase(prev);
                }
            }
            insert(offset, size);
        }

    private:
        void insert(U32 offset, U32 size)
        {
            mByOffset[offset] = size;
            mBySize.emplace(size, offset);
        }

        void eraseBySize(U32 offset, U32 size)
        {
            auto range = mBySize.equal_range(size);
            for (auto it = range.first; it != range.second; ++it)
            {
                if (it->second == offset)
                {
                    mBySize.erase(it);
                    return;
                }
            }
        }

        std::multimap<U32, U32> mBySize;    // size -> offset
        std::map<U32, U32> mByOffset;       // offset -> size
    };

    // One attribute array of the chunk's pairs: its key (the location the draws give, or with
    // LLRHI::sVBSourceStreams the LL array they read, research/293 branch 1) and stride.
    struct Stream
    {
        U32 location = 0;       // the key
        U32 stride = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
    };

    struct Chunk
    {
        VkBuffer indices = VK_NULL_HANDLE;
        VmaAllocation index_memory = nullptr;
        std::vector<Stream> streams;
        RangeAllocator vertex_ranges;
        RangeAllocator index_ranges;
        U32 vertex_capacity = 0;    // room in each stream
        U32 index_capacity = 0;     // room in the index buffer
    };

    // An attribute array of a pair, copied into the stream of its key (location) and stride.
    struct Copied
    {
        U32 location = 0;   // the stream's key (Attribute::streamKey: the location, or the LL array the attribute reads)
        U32 stride = 0;
        U32 offset = 0;     // where the array starts in the GL vertex buffer
        // research/293 branch 1: the locations whose attributes this array serves (a bit each): the
        // key's own and, with LLRHI::sVBSourceStreams, those of the attributes that read it from
        // inside (TEXTURE_INDEX reads the positions). NumberPlace::copied is the union over the
        // copies: the locations a draw can read from the place. Without the switch: the one bit
        // of `location`, as before.
        U32 locations = 0;
    };
    inline U32 location_bit(U32 location)
    {
        return location < 32 ? 1u << location : 0u;
    }
    // Whether the pair has the array attribute `a` reads copied: Missing (copy it), Same (it is there with the
    // same stride and start; the attribute's location is noted among those the array serves, `noted` set when
    // that is new), Other (copied as another layout of the buffer: the caller drops the place and makes it again).
    enum class CopiedState { Missing, Same, Other };
    template <class Entry_>
    CopiedState copied_state(Entry_& e, const LLRHI::DirectDraw::Attribute& a, bool& noted)
    {
        const U32 key = a.streamKey();
        for (U32 k = 0; k < e.copied_count; ++k)
        {
            Copied& c = e.copied[k];
            if (c.location != key)
            {
                continue;
            }
            if (c.stride != a.stride || c.offset != a.offset - a.streamShift())
            {
                return CopiedState::Other;
            }
            const U32 bit = location_bit(a.location);
            if (bit & ~c.locations)
            {
                c.locations |= bit;
                noted = true;
            }
            return CopiedState::Same;
        }
        return CopiedState::Missing;
    }

    // A GL vertex/index buffer pair (keyed by the vertex buffer).
    struct Entry
    {
        GLName vertex_buffer = 0;
        GLName index_buffer = 0;
        U32 index_type = 0;
        U32 vertices = 0;       // LLVertexBuffer::getNumVerts
        U32 indices = 0;        // LLVertexBuffer::getNumIndices
        bool placed = false;    // false: not copyable (drawn from GL until GL writes it)
        U32 chunk = 0;
        U32 vertex_base = 0;
        U32 vertex_room = 0;
        U32 index_base = 0;
        U32 index_room = 0;
        Copied copied[MAX_COPIED];
        U32 copied_count = 0;
        U32 used_frame = 0;
        U64 serial = 0;         // which placement (Ticket::serial)
        U32 placement = 0;      // research/46 G5: which placement pins name (0: not placed)
        U32 pins = 0;           // lists drawing the placement (not dropped while idle)
        // Research/197 stage 1b, a place by number (Pool::numbered): its
        // number; the frame a draw last read it (a write in that frame goes
        // to a new place); the frame and upload barrier epoch of the copies
        // into it since the last barrier, and what they covered of each
        // attribute's array (bytes, by copied[] index) and of the indices
        // (at MAX_COPIED): a copy over any of it needs a barrier first.
        U32 number = 0;
        U32 writes = 0;         // research/230 R2: writes into the place by number (NumberPlace::writes)
        U32 read_frame = U32_MAX;
        U32 written_frame = U32_MAX;
        U32 written_epoch = 0;
        U32 written_lo[MAX_COPIED + 1] = {};
        U32 written_hi[MAX_COPIED + 1] = {};
    };

    // A dropped pair's ranges, free again once no frame in flight reads them.
    struct Release
    {
        U32 chunk = 0;
        U32 vertex_base = 0;
        U32 vertex_room = 0;
        U32 index_base = 0;
        U32 index_room = 0;
        U32 frame = 0;
    };

    struct Churn
    {
        U32 frame = 0;      // when the buffer's copies were last dropped
        U32 strikes = 0;    // drops within CHURN_FRAMES of the one before
    };

    // ---- research/197 stage 1b: vertex buffer numbers ----
    // A number (LLRHI::BufferRef): its place, if it has one.
    struct NumberEntry
    {
        U32 pair = 0;           // Pool::numbered[pair - 1] (0: no place)
        U32 free_frame = 0;     // released: given out again from this frame
        bool live = false;
    };
    // Verify: what went into a sampled number's place, kept on the CPU
    // (the streams as Entry::copied, the indices as 32-bit ones).
    struct NumberShadow
    {
        std::vector<U8> streams[MAX_COPIED];
        std::vector<U32> indices;
        U32 checked_frame = 0;
        bool checked_once = false;
    };
    struct NumberCounts
    {
        U64 made = 0, released = 0, placed = 0, redeemed = 0;
        U64 writes = 0, write_bytes = 0, in_place = 0, moved = 0, barriers = 0;
        U64 dropped = 0, relaid = 0, never = 0, unaligned = 0;
        U64 sampled = 0, checked = 0, check_bytes = 0, mismatches = 0;
    };

    enum class CopyResult
    {
        Placed,
        Later,      // the pool, the frame's budget or the upload ring is full: try again later
        Never,      // the data does not fit the description: drawn from GL until GL writes it
    };

    struct Pool
    {
        bool tried = false;
        bool supported = false;
        bool callback = false;
        bool used = false;
        PFN_vkCmdDrawIndexedIndirectCount draw_indirect_count = nullptr;
        U64 budget = 512ull * 1024 * 1024;     // device memory the pool may take
        U64 allocated = 0;                      // device memory it took
        std::vector<Chunk> chunks;
        std::unordered_map<GLName, Entry> entries;                      // by vertex buffer
        std::unordered_map<GLName, std::vector<GLName>> by_index;       // index buffer -> vertex buffers of its entries
        std::unordered_map<GLName, Churn> churn;
        std::vector<Release> releases;
        U32 frame = U32_MAX;            // the frame place() last ran in
        U64 copied_this_frame = 0;
        U32 last_sweep = 0;
        U64 used_vertices = 0;
        U64 used_indices = 0;
        // The smallest pair (vertex and index room) that found no room and no
        // new chunk: pairs at least that large in both are not tried again
        // until ranges are released or a chunk is added (0: none failed).
        U32 full_vertex_room = 0;
        U32 full_index_room = 0;
        // Moves whenever an entry is erased (and, with LLRHI_POOL_LAZY_FORGET=0,
        // whenever a pair is dropped): a Ticket's entry pointer is good while it
        // stays (unordered_map nodes stay put until erased). redeem() checks
        // the entry is still the ticket's placement.
        U32 epoch = 0;
        // Entries forgotten but kept as empty nodes until the next sweep
        // (research/58, LLRHI::sPoolLazyForget): not counted as pairs.
        U32 forgotten = 0;
        U64 serials = 0;
        U64 number_writes = 0;                  // research/230 R2 (numberWriteSerial)
        U32 placements = 0;                     // placement numbers handed out (Entry::placement)
        void (*dropped)(GLName vertex_buffer) = nullptr;    // setDropCallback
        // Since the last takeStats.
        U32 copies = 0, drops = 0, never = 0, later_budget = 0, later_full = 0, later_ring = 0, rewritten = 0, relaid = 0;
        U64 copied_bytes = 0;
        // research/218 vk-s7f (research/207 7.1): the old paths since the last
        // takeOldPathStats: draws placed and tickets redeemed by the GL name
        // (no number).
        U64 old_named_places = 0, old_named_redeems = 0;
        // Research/197 stage 1b: the vertex buffer numbers and their places.
        bool numbers_installed = false;
        std::vector<NumberEntry> numbers = std::vector<NumberEntry>(1);     // number 0 is never given out
        std::deque<U32> number_free;            // released numbers, oldest first
        std::vector<Entry> numbered;            // the places (NumberEntry::pair - 1); grows, never shrinks
        std::vector<std::unique_ptr<NumberShadow>> shadows;     // beside numbered: verify's sample only
        std::vector<U32> numbered_free;
        U32 upload_epoch = 0;                   // barriers numbered copies recorded into the uploads
        U64 checked_this_frame = 0;             // verify's bytes compared this frame
        bool number_take = false;               // this frame: draws take places by number (on)
        bool number_verify = false;             // ... a sample keeps them, compared only (verify)
        bool number_seen_on = false, number_seen_verify = false;
        U32 number_auto_frames = 0;
        U64 number_auto_checks_from = 0;
        U32 number_logged = 0;
        NumberCounts number_counts;             // since the last numbersReport
        NumberCounts number_totals;             // since the switches last changed (auto)
        // Research/198 5.5: the retained scene's interest in numbers' places.
        void (*number_listener)(U32 number) = nullptr;
        std::vector<U16> number_interest;       // by number: the scene's records of it
        bool scene_drawing = false;             // the scene draws from places (LLVKAttribPool::sceneDrawing)
        U32 scene_read_frame = U32_MAX;         // the frame the scene last drew in
    };
    // Never destroyed: GL buffers can still be deleted (and reported) while
    // static objects go away at exit.
    Pool& sPool = *new Pool();

    void drop(Pool& p, Entry& e)
    {
        if (e.placed)
        {
            // Lists drawing from the copy stop first (research/46 G5); its pins go with it.
            e.placement = 0;
            e.pins = 0;
            if (p.dropped)
            {
                p.dropped(e.vertex_buffer);
            }
            Release r;
            r.chunk = e.chunk;
            r.vertex_base = e.vertex_base;
            r.vertex_room = e.vertex_room;
            r.index_base = e.index_base;
            r.index_room = e.index_room;
            r.frame = LLVKTracker::frame();
            p.releases.push_back(r);
            e.placed = false;
            e.copied_count = 0;
            ++p.drops;
        }
        if (!LLRHI::sPoolLazyForget)
        {
            ++p.epoch;  // as before research/58 (redeem() now also checks the entry itself)
        }
    }

    // An entry forget() emptied but kept (research/58): no pair, in no index
    // list, erased at the next sweep. A placed pair always has a vertex buffer.
    inline bool is_forgotten(const Entry& e)
    {
        return e.vertex_buffer == 0;
    }

    void unlink_index(Pool& p, const Entry& e)
    {
        auto it = p.by_index.find(e.index_buffer);
        if (it != p.by_index.end())
        {
            std::vector<GLName>& names = it->second;
            names.erase(std::remove(names.begin(), names.end(), e.vertex_buffer), names.end());
            if (names.empty())
            {
                p.by_index.erase(it);
            }
        }
    }

    // Drops the entry of the vertex buffer and forgets it. With
    // LLRHI::sPoolLazyForget the node stays as an empty entry until the next
    // sweep (returned; place() fills it again), so the tickets of every other
    // pair keep their entry pointers (no new epoch). Else it is erased (end()).
    std::unordered_map<GLName, Entry>::iterator forget(Pool& p, std::unordered_map<GLName, Entry>::iterator it)
    {
        drop(p, it->second);
        unlink_index(p, it->second);
        if (LLRHI::sPoolLazyForget)
        {
            it->second = Entry();
            ++p.forgotten;
            return it;
        }
        p.entries.erase(it);
        ++p.epoch;
        return p.entries.end();
    }

    // ---- research/197 stage 1b: vertex buffer numbers (part 1: places going, the switch) ----
    // Research/198 5.5: the scene draws this number's place (it has records of it).
    inline bool scene_interest(const Pool& p, U32 number)
    {
        return number < p.number_interest.size() && p.number_interest[number] != 0;
    }

    // Research/198 5.5: the place of a number the scene draws from changed
    // (made, moved, another attribute copied, gone).
    inline void number_changed(Pool& p, U32 number)
    {
        if (p.number_listener && scene_interest(p, number))
        {
            p.number_listener(number);
        }
    }

    // A number's place goes: its ranges back to the pool once no frame in
    // flight reads them (as drop()), its slot for another place. No drop
    // callback (the probe lists keep pairs by name only): a ticket by number
    // is redeemed from the number's entry.
    void numbered_drop(Pool& p, U32 number)
    {
        NumberEntry& n = p.numbers[number];
        if (!n.pair)
        {
            return;
        }
        const U32 index = n.pair - 1;
        Entry& e = p.numbered[index];
        if (e.placed)
        {
            Release r;
            r.chunk = e.chunk;
            r.vertex_base = e.vertex_base;
            r.vertex_room = e.vertex_room;
            r.index_base = e.index_base;
            r.index_room = e.index_room;
            r.frame = LLVKTracker::frame();
            p.releases.push_back(r);
        }
        e = Entry();
        p.shadows[index].reset();
        p.numbered_free.push_back(index);
        n.pair = 0;
        number_changed(p, number);     // research/198 5.5
    }

    // The place was made for another layout of the buffer (it was allocated
    // again, or its index type changed) than the one given.
    bool other_layout(const Entry& e, U32 vertex_buffer, U32 index_buffer, U32 index_type, U32 vertices, U32 indices)
    {
        return e.vertex_buffer != vertex_buffer || e.index_buffer != index_buffer || e.index_type != index_type ||
               e.vertices != vertices || e.indices != indices;
    }

    // The auto mode, once a frame while it runs (as the texture numbers',
    // llvkgl_texref.cpp). True: it changed the switches.
    bool numbers_auto(Pool& p)
    {
        if (!LLRHI::sVBHandles || !LLRHI::sVBHandlesVerify)
        {
            LLRHI::sVBHandlesAuto = false;  // not in the state it starts from (a cue changed it)
            return false;
        }
        const NumberCounts& c = p.number_totals;
        if (!LLRHI::sWorldShown)
        {
            // The login screen: verified (a mismatch still turns it off), not counted toward on.
            p.number_auto_frames = 0;
            p.number_auto_checks_from = c.checked;
        }
        else
        {
            ++p.number_auto_frames;
        }
        if (c.mismatches)
        {
            LLRHI::sVBHandles = false;
            LLRHI::sVBHandlesAuto = false;
            LL_WARNS("Vulkan") << "vertex buffer numbers (auto): verify found " << c.mismatches
                               << " places that differ from GL's copy: off for this run (research/197 1b)" << LL_ENDL;
            return true;
        }
        const U64 checks = c.checked - p.number_auto_checks_from;
        if (p.number_auto_frames >= NUMBER_AUTO_FRAMES && checks >= NUMBER_AUTO_CHECKS)
        {
            LLRHI::sVBHandlesVerify = false;
            LLRHI::sVBHandlesAuto = false;
            LL_INFOS("Vulkan") << "vertex buffer numbers (auto): " << checks << " comparisons over " << p.number_auto_frames
                               << " frames of the world, no mismatch: on (research/197 1b)" << LL_ENDL;
            return true;
        }
        return false;
    }

    // Once a frame (begin_frame): the switches as the cue or the auto mode
    // left them. LLRHI::sVBNative follows (the viewer sends numbers and
    // writes while it is on). From verify to on the sample's places stay
    // (every write reached them); any other change starts with no places:
    // off sent no writes, and on's places have no CPU copies to compare.
    void numbers_step(Pool& p)
    {
        if (!p.numbers_installed)
        {
            return;
        }
        const bool by_auto = LLRHI::sVBHandlesAuto && p.number_seen_on && numbers_auto(p);
        const bool on = LLRHI::sVBHandles;
        const bool verify = on && LLRHI::sVBHandlesVerify;
        if (on != p.number_seen_on || verify != p.number_seen_verify)
        {
            if (p.number_seen_on && !by_auto && LLRHI::sVBHandlesAuto)
            {
                LLRHI::sVBHandlesAuto = false;  // a cue changed a switch: the auto mode is over
            }
            if (!(p.number_seen_verify && on && !verify))
            {
                for (U32 number = 1; number < (U32)p.numbers.size(); ++number)
                {
                    numbered_drop(p, number);
                }
            }
            for (std::unique_ptr<NumberShadow>& shadow : p.shadows)
            {
                shadow.reset();
            }
            p.number_totals = NumberCounts();
            p.number_auto_frames = 0;
            p.number_auto_checks_from = 0;
            p.number_seen_on = on;
            p.number_seen_verify = verify;
            LL_INFOS("Vulkan") << "vertex buffer numbers " << (!on ? "off" : verify ? (LLRHI::sVBHandlesAuto ? "auto (verify first)" : "verify") : "on")
                               << " (research/197 1b)" << LL_ENDL;
        }
        p.number_take = on && !verify;
        p.number_verify = verify;
        LLRHI::sVBNative = on;
    }
    // ---- end of part 1 ----

    bool churning(const Pool& p, GLName name, U32 frame)
    {
        auto it = p.churn.find(name);
        return it != p.churn.end() && it->second.strikes >= 2 && frame - it->second.frame < CHURN_FRAMES;
    }

    void strike(Pool& p, GLName name)
    {
        const U32 frame = LLVKTracker::frame();
        Churn& c = p.churn[name];
        c.strikes = c.strikes && frame - c.frame < CHURN_FRAMES ? c.strikes + 1 : 1;
        c.frame = frame;
    }

    // LLVKTracker's buffer callback: GL wrote the buffer or deleted it.
    void on_buffer_changed(GLName name)
    {
        Pool& p = sPool;
        bool had = false;
        auto it = p.entries.find(name);
        if (it != p.entries.end() && !is_forgotten(it->second))
        {
            forget(p, it);
            had = true;
        }
        auto by = p.by_index.find(name);
        if (by != p.by_index.end())
        {
            const std::vector<GLName> names = by->second;   // forget() edits the list
            for (GLName vertex_buffer : names)
            {
                auto e = p.entries.find(vertex_buffer);
                if (e != p.entries.end() && !is_forgotten(e->second) && e->second.index_buffer == name)
                {
                    forget(p, e);
                    had = true;
                }
            }
            p.by_index.erase(name);
        }
        if (had)
        {
            strike(p, name);
        }
    }

    void sweep(Pool& p, U32 frame)
    {
        p.last_sweep = frame;
        for (auto it = p.entries.begin(); it != p.entries.end(); )
        {
            if (is_forgotten(it->second))
            {
                // Forgotten since the last sweep (research/58): no pair to drop.
                it = p.entries.erase(it);
                --p.forgotten;
                ++p.epoch;
            }
            else if (frame - it->second.used_frame > IDLE_FRAMES && !it->second.pins)
            {
                drop(p, it->second);
                it = p.entries.erase(it);
                ++p.epoch;
            }
            else
            {
                ++it;
            }
        }
        // Research/197 stage 1b: places by number not drawn for as long go too (the number stays).
        // Research/198 5.5: not those the retained scene draws from (its draws do not mark them used).
        for (U32 i = 0; i < (U32)p.numbered.size(); ++i)
        {
            const U32 number = p.numbered[i].number;
            if (number && frame - p.numbered[i].used_frame > IDLE_FRAMES && !(p.scene_drawing && scene_interest(p, number)))
            {
                numbered_drop(p, number);
            }
        }
        // The index buffer map again, without the entries gone since.
        p.by_index.clear();
        for (const auto& kv : p.entries)
        {
            p.by_index[kv.second.index_buffer].push_back(kv.first);
        }
        for (auto it = p.churn.begin(); it != p.churn.end(); )
        {
            if (frame - it->second.frame >= CHURN_FRAMES)
            {
                it = p.churn.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void begin_frame(Pool& p, U32 frame)
    {
        p.frame = frame;
        p.copied_this_frame = 0;
        p.checked_this_frame = 0;
        numbers_step(p);    // research/197 stage 1b
        // Ranges no frame in flight reads any more go back to their chunk.
        size_t kept = 0;
        for (size_t i = 0; i < p.releases.size(); ++i)
        {
            const Release r = p.releases[i];
            if (frame - r.frame >= RELEASE_DELAY)
            {
                Chunk& c = p.chunks[r.chunk];
                c.vertex_ranges.release(r.vertex_base, r.vertex_room);
                c.index_ranges.release(r.index_base, r.index_room);
                p.used_vertices -= r.vertex_room;
                p.used_indices -= r.index_room;
                p.full_vertex_room = p.full_index_room = 0;     // room again: everything may be tried
            }
            else
            {
                p.releases[kept++] = r;
            }
        }
        p.releases.resize(kept);
        if (frame - p.last_sweep >= SWEEP_FRAMES)
        {
            sweep(p, frame);
        }
    }

    bool create_buffer(Pool& p, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& memory)
    {
        if (p.allocated + size > p.budget)
        {
            return false;
        }
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        // TRANSFER_SRC too: numbered_move copies a placement to its new place inside the pool (research/197 1b;
        // VUID-vkCmdCopyBuffer-srcBuffer-00118 in the validation layer, found by the 10-26 verify run).
        info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (LLVKDevice::instance().generatedCommands())
        {
            // Device generated commands bind the chunks' buffers by address (research/50 step 4).
            info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        if (LLVKDevice::instance().hasRayTracing())
        {
            // Ray tracing (research/230 R2): the acceleration structures read the chunks by address. A buffer's
            // usage cannot change after it is made, so a GPU that can always gets the flags (they cost nothing).
            info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &gpu, &buffer, &memory, nullptr) != VK_SUCCESS)
        {
            LL_WARNS("VKAttribPool") << "pool buffer of " << (size >> 20) << " MB could not be allocated; the pool stays at "
                                     << (p.allocated >> 20) << " MB" << LL_ENDL;
            p.budget = p.allocated;     // no more tries
            buffer = VK_NULL_HANDLE;
            memory = nullptr;
            return false;
        }
        p.allocated += size;
        return true;
    }

    // A new chunk with room for at least a pair of vertex_room vertices and
    // index_room indices (both at most CHUNK_VERTICES / CHUNK_INDICES).
    bool add_chunk(Pool& p, U32 vertex_room, U32 index_room)
    {
        U32 vertices = CHUNK_VERTICES, indices = CHUNK_INDICES;
        if (!p.chunks.empty())
        {
            vertices = MORE_CHUNK_VERTICES;
            indices = MORE_CHUNK_INDICES;
            while ((vertices < vertex_room || indices < index_room) && vertices < CHUNK_VERTICES)
            {
                vertices <<= 1;
                indices <<= 1;
            }
        }
        // Room for the streams the first chunk needed as well, so a new chunk's
        // pairs do not wait for streams the budget no longer has.
        VkDeviceSize streams = 0;
        if (!p.chunks.empty())
        {
            for (const Stream& s : p.chunks.front().streams)
            {
                streams += (VkDeviceSize)vertices * s.stride;
            }
        }
        if (p.allocated + (VkDeviceSize)indices * 4 + streams > p.budget)
        {
            return false;
        }
        Chunk chunk;
        if (!create_buffer(p, (VkDeviceSize)indices * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, chunk.indices, chunk.index_memory))
        {
            return false;
        }
        chunk.vertex_capacity = vertices;
        chunk.index_capacity = indices;
        chunk.vertex_ranges.reset(vertices);
        chunk.index_ranges.reset(indices);
        p.chunks.push_back(std::move(chunk));
        p.full_vertex_room = p.full_index_room = 0;
        if (p.chunks.size() == 1)
        {
            // The switch's state, once (an environment variable spelled wrong leaves the default: on)
            LL_INFOS("VKAttribPool") << "attribute pool streams by " << (LLRHI::sVBSourceStreams ? "the array an attribute reads (source)" : "location")
                                     << " (LLRHI_VB_SOURCE_STREAMS, research/293)" << LL_ENDL;
        }
        LL_INFOS("VKAttribPool") << "pool chunk " << p.chunks.size() << " (" << vertices << " vertices, " << indices
                                 << " indices), " << (p.allocated >> 20) << " of " << (p.budget >> 20) << " MB" << LL_ENDL;
        return true;
    }

    // The chunk's stream for an attribute location and stride, made on first use.
    Stream* stream_for(Pool& p, U32 chunk, U32 location, U32 stride, bool make)
    {
        if (chunk >= p.chunks.size())
        {
            return nullptr;
        }
        Chunk& c = p.chunks[chunk];
        for (Stream& s : c.streams)
        {
            if (s.location == location && s.stride == stride)
            {
                return &s;
            }
        }
        if (!make)
        {
            return nullptr;
        }
        Stream s;
        s.location = location;
        s.stride = stride;
        if (!create_buffer(p, (VkDeviceSize)c.vertex_capacity * stride, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, s.buffer, s.memory))
        {
            return nullptr;
        }
        c.streams.push_back(s);
        return &c.streams.back();
    }

    // Room in the frame's upload ring and the upload commands, within the frame's budget.
    CopyResult staging(Pool& p, U64 bytes, VkCommandBuffer& cmd, VkBuffer& buffer, VkDeviceSize& offset, U8*& data)
    {
        if (p.copied_this_frame + bytes > COPY_BUDGET)
        {
            ++p.later_budget;
            return CopyResult::Later;
        }
        LLVKMirror& mirror = LLVKMirror::instance();
        cmd = mirror.uploadCommands();
        void* dst = nullptr;
        if (!cmd || !mirror.allocate(bytes, 16, buffer, offset, dst))
        {
            ++p.later_ring;
            return CopyResult::Later;
        }
        data = (U8*)dst;
        p.copied_this_frame += bytes;
        p.copied_bytes += bytes;
        return CopyResult::Placed;
    }

    // Ranges for a pair of vertex_room vertices and index_room indices in one
    // chunk, a new chunk when none has the room. False: no room and no new
    // chunk (remembered, so the pair and larger ones wait for a release).
    bool take_ranges(Pool& p, U32 vertex_room, U32 index_room, U32& chunk, U32& vertex_base, U32& index_base)
    {
        chunk = U32_MAX;
        for (U32 c = 0; c < (U32)p.chunks.size() && chunk == U32_MAX; ++c)
        {
            Chunk& ch = p.chunks[c];
            if (ch.vertex_ranges.allocate(vertex_room, vertex_base))
            {
                if (ch.index_ranges.allocate(index_room, index_base))
                {
                    chunk = c;
                }
                else
                {
                    ch.vertex_ranges.release(vertex_base, vertex_room);
                }
            }
        }
        if (chunk == U32_MAX)
        {
            if (!add_chunk(p, vertex_room, index_room) || !p.chunks.back().vertex_ranges.allocate(vertex_room, vertex_base) ||
                !p.chunks.back().index_ranges.allocate(index_room, index_base))
            {
                // Remembered, so the pair (and larger ones) wait for a release.
                if (!p.full_vertex_room || (vertex_room <= p.full_vertex_room && index_room <= p.full_index_room))
                {
                    p.full_vertex_room = vertex_room;
                    p.full_index_room = index_room;
                }
                ++p.later_full;
                if (LLRHIVBCensus::sOn)
                {
                    LLRHIVBCensus::count(LLRHIVBCensus::C_RANGES_FAILED);   // research/293 branch 0: counting only
                }
                return false;
            }
            chunk = (U32)p.chunks.size() - 1;
        }
        if (LLRHIVBCensus::sOn)
        {
            LLRHIVBCensus::count(LLRHIVBCensus::C_RANGES_TAKEN);    // research/293 branch 0: counting only
        }
        return true;
    }

    // Places the pair (vertex and index ranges in one chunk) and copies its
    // indices, as 32-bit indices. Its attributes are copied by copy_attribute.
    CopyResult copy_in(Pool& p, Entry& e)
    {
        // The cheap checks first: a pair that cannot be placed now costs no scan.
        const bool index32 = e.index_type == GL_UNSIGNED_INT;
        if (!index32 && e.index_type != GL_UNSIGNED_SHORT)
        {
            return CopyResult::Never;
        }
        const U32 vertex_room = (U32)round_up(e.vertices, VERTEX_GRANULE);
        const U32 index_room = (U32)round_up(e.indices, INDEX_GRANULE);
        if (vertex_room > CHUNK_VERTICES || index_room > CHUNK_INDICES || (U64)e.indices * 4 > COPY_BUDGET)
        {
            return CopyResult::Never;   // bigger than a chunk, or than a frame's copies could ever take
        }
        if (p.full_vertex_room && vertex_room >= p.full_vertex_room && index_room >= p.full_index_room)
        {
            ++p.later_full;             // a pair no larger found no room, and none was freed since
            return CopyResult::Later;
        }
        if (p.copied_this_frame + (U64)e.indices * 4 > COPY_BUDGET)
        {
            ++p.later_budget;
            return CopyResult::Later;
        }
        size_t index_bytes_gl = 0;
        const U8* indices = LLVKTracker::bufferContents(e.index_buffer, index_bytes_gl);
        const U32 index_size = index32 ? 4 : 2;
        if (!indices || (U64)e.indices * index_size > index_bytes_gl)
        {
            return CopyResult::Never;
        }

        U32 chunk = U32_MAX;
        U32 vertex_base = 0, index_base = 0;
        if (!take_ranges(p, vertex_room, index_room, chunk, vertex_base, index_base))
        {
            return CopyResult::Later;
        }
        auto release = [&]()
        {
            p.chunks[chunk].vertex_ranges.release(vertex_base, vertex_room);
            p.chunks[chunk].index_ranges.release(index_base, index_room);
        };

        // Every index has to name one of the pair's vertices: past them the
        // streams hold other pairs (where GL would read the buffer's other arrays).
        U32 max_index = 0;
        if (index32)
        {
            const U32* src = (const U32*)indices;
            for (U32 i = 0; i < e.indices; ++i)
            {
                max_index = llmax(max_index, src[i]);
            }
        }
        else
        {
            const U16* src = (const U16*)indices;
            for (U32 i = 0; i < e.indices; ++i)
            {
                max_index = llmax(max_index, (U32)src[i]);
            }
        }
        if (max_index >= e.vertices)
        {
            release();      // never written: free at once
            return CopyResult::Never;
        }

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize ring_offset = 0;
        U8* dst = nullptr;
        const U64 bytes = (U64)e.indices * 4;
        if (staging(p, bytes, cmd, ring, ring_offset, dst) != CopyResult::Placed)
        {
            release();      // never written: free at once
            return CopyResult::Later;
        }
        U32* out = (U32*)dst;
        if (index32)
        {
            memcpy(out, indices, (size_t)bytes);
        }
        else
        {
            const U16* src = (const U16*)indices;
            for (U32 i = 0; i < e.indices; ++i)
            {
                out[i] = src[i];
            }
        }
        VkBufferCopy region = { ring_offset, (VkDeviceSize)index_base * 4, bytes };
        vkCmdCopyBuffer(cmd, ring, p.chunks[chunk].indices, 1, &region);

        e.chunk = chunk;
        e.vertex_base = vertex_base;
        e.vertex_room = vertex_room;
        e.index_base = index_base;
        e.index_room = index_room;
        e.copied_count = 0;
        e.placed = true;
        e.serial = ++p.serials;
        e.placement = ++p.placements ? p.placements : ++p.placements;
        e.pins = 0;
        p.used_vertices += vertex_room;
        p.used_indices += index_room;
        ++p.copies;
        return CopyResult::Placed;
    }

    // Copies one attribute array of a placed pair into its stream.
    CopyResult copy_attribute(Pool& p, Entry& e, const LLRHI::DirectDraw::Attribute& a)
    {
        if (!a.stride || a.stride > MAX_STRIDE || e.copied_count >= MAX_COPIED)
        {
            return CopyResult::Never;
        }
        // The array this attribute reads, whole (research/293 branch 1: not from the attribute's own
        // offset when it reads from inside an array, like TEXTURE_INDEX).
        const U32 key = a.streamKey();
        const U32 shift = a.streamShift();
        if (a.offset < shift)
        {
            return CopyResult::Never;
        }
        const U32 array_offset = a.offset - shift;
        const U64 bytes = (U64)e.vertices * a.stride;
        if (bytes > COPY_BUDGET)
        {
            return CopyResult::Never;   // more than a frame's copies could ever take
        }
        if (p.copied_this_frame + bytes > COPY_BUDGET)
        {
            ++p.later_budget;
            return CopyResult::Later;
        }
        size_t vertex_bytes_gl = 0;
        const U8* vertices = LLVKTracker::bufferContents(e.vertex_buffer, vertex_bytes_gl);
        if (!vertices || (U64)array_offset + bytes > vertex_bytes_gl)
        {
            return CopyResult::Never;
        }
        Stream* s = stream_for(p, e.chunk, key, a.stride, true);
        if (!s)
        {
            // The pool's memory is spent (it never shrinks): the pair stays
            // with the GL buffers, its ranges free for pairs that need no new stream.
            ++p.later_full;
            return CopyResult::Never;
        }
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize ring_offset = 0;
        U8* dst = nullptr;
        if (staging(p, bytes, cmd, ring, ring_offset, dst) != CopyResult::Placed)
        {
            return CopyResult::Later;
        }
        memcpy(dst, vertices + array_offset, (size_t)bytes);
        VkBufferCopy region = { ring_offset, (VkDeviceSize)e.vertex_base * a.stride, bytes };
        vkCmdCopyBuffer(cmd, ring, s->buffer, 1, &region);
        Copied& c = e.copied[e.copied_count++];
        c.location = key;
        c.stride = a.stride;
        c.offset = array_offset;
        c.locations = location_bit(key) | location_bit(a.location);
        return CopyResult::Placed;
    }

    // ---- research/197 stage 1b: vertex buffer numbers (part 2: places by number) ----
    inline NumberEntry* number_at(Pool& p, U32 number)
    {
        return number && number < (U32)p.numbers.size() && p.numbers[number].live ? &p.numbers[number] : nullptr;
    }

    // Two copies over the same bytes of a place within one frame's uploads
    // need a barrier between them: the copies of a command buffer may run in
    // any order (and the synchronization checks say so). The pairs by name
    // never need one (copied once into new ranges); a place by number is
    // written again, often a face at a time (ranges that do not overlap:
    // no barrier for those).
    bool written_since_barrier(const Pool& p, const Entry& e, U32 frame)
    {
        return e.written_frame == frame && e.written_epoch == p.upload_epoch;
    }

    // Part k of the place (an attribute's array by copied[] index, bytes; or
    // the indices at MAX_COPIED) is written over [lo, hi): whether a copy
    // since the last barrier covered any of it.
    bool overlaps_written(const Pool& p, const Entry& e, U32 frame, U32 k, U64 lo, U64 hi)
    {
        return written_since_barrier(p, e, frame) && lo < e.written_hi[k] && e.written_lo[k] < hi;
    }

    void note_written(const Pool& p, Entry& e, U32 frame, U32 k, U64 lo, U64 hi)
    {
        if (!written_since_barrier(p, e, frame))
        {
            for (U32 i = 0; i <= MAX_COPIED; ++i)
            {
                e.written_lo[i] = U32_MAX;
                e.written_hi[i] = 0;
            }
            e.written_frame = frame;
            e.written_epoch = p.upload_epoch;
        }
        e.written_lo[k] = llmin(e.written_lo[k], (U32)lo);
        e.written_hi[k] = llmax(e.written_hi[k], (U32)hi);
    }

    // The whole place was written (copied in, or moved).
    void note_written_whole(const Pool& p, Entry& e, U32 frame)
    {
        for (U32 k = 0; k < e.copied_count; ++k)
        {
            note_written(p, e, frame, k, 0, (U64)e.vertices * e.copied[k].stride);
        }
        note_written(p, e, frame, MAX_COPIED, 0, e.indices);
    }

    void upload_barrier(Pool& p, VkCommandBuffer cmd)
    {
        VkMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        ++p.upload_epoch;
        ++p.number_counts.barriers;
    }

    // Verify: the CPU copy of a sampled place, as copy_in and copy_attribute
    // copied it from the tracker's copy (indices as 32-bit ones).
    void shadow_indices(NumberShadow& shadow, const Entry& e)
    {
        size_t bytes = 0;
        const U8* src = LLVKTracker::bufferContents(e.index_buffer, bytes);
        shadow.indices.assign(e.indices, 0);
        const bool index32 = e.index_type == GL_UNSIGNED_INT;
        for (U32 i = 0; src && i < e.indices && (size_t)(i + 1) * (index32 ? 4 : 2) <= bytes; ++i)
        {
            shadow.indices[i] = index32 ? ((const U32*)src)[i] : (U32)((const U16*)src)[i];
        }
    }

    void shadow_stream(NumberShadow& shadow, const Entry& e, U32 k)
    {
        const Copied& c = e.copied[k];
        const size_t want = (size_t)e.vertices * c.stride;
        size_t bytes = 0;
        const U8* src = LLVKTracker::bufferContents(e.vertex_buffer, bytes);
        shadow.streams[k].assign(want, 0);
        if (src && c.offset + want <= bytes)
        {
            memcpy(shadow.streams[k].data(), src + c.offset, want);
        }
    }

    // Verify: a sampled place's CPU copy against the tracker's copy of the
    // GL buffers now (what the GL names hold: every write went there too).
    // A difference is a write that did not reach the number (or reached it
    // wrong), the thing that would draw a wrong shape with the switch on.
    void numbered_check(Pool& p, U32 index, U32 frame)
    {
        const Entry& e = p.numbered[index];
        NumberShadow& shadow = *p.shadows[index];
        shadow.checked_frame = frame;
        shadow.checked_once = true;
        size_t vertex_bytes = 0, index_bytes = 0;
        const U8* vertices = LLVKTracker::bufferContents(e.vertex_buffer, vertex_bytes);
        const U8* indices = LLVKTracker::bufferContents(e.index_buffer, index_bytes);
        const char* part = nullptr;
        U32 location = 0;
        size_t at = 0;
        U64 bytes = 0;
        for (U32 k = 0; k < e.copied_count && !part; ++k)
        {
            const Copied& c = e.copied[k];
            const size_t want = (size_t)e.vertices * c.stride;
            bytes += want;
            location = c.location;
            if (!vertices || c.offset + want > vertex_bytes || shadow.streams[k].size() != want)
            {
                part = "the vertex buffer's size";
            }
            else if (memcmp(shadow.streams[k].data(), vertices + c.offset, want) != 0)
            {
                part = "an attribute";
                while (at < want && shadow.streams[k][at] == vertices[c.offset + at])
                {
                    ++at;
                }
            }
        }
        if (!part)
        {
            const bool index32 = e.index_type == GL_UNSIGNED_INT;
            bytes += (U64)e.indices * 4;
            if (!indices || (size_t)e.indices * (index32 ? 4 : 2) > index_bytes || shadow.indices.size() != e.indices)
            {
                part = "the index buffer's size";
            }
            else
            {
                for (U32 i = 0; i < e.indices; ++i)
                {
                    const U32 value = index32 ? ((const U32*)indices)[i] : (U32)((const U16*)indices)[i];
                    if (value != shadow.indices[i])
                    {
                        part = "the indices";
                        at = i;
                        break;
                    }
                }
            }
        }
        p.checked_this_frame += bytes;
        ++p.number_counts.checked;
        ++p.number_totals.checked;
        p.number_counts.check_bytes += bytes;
        if (part)
        {
            ++p.number_counts.mismatches;
            ++p.number_totals.mismatches;
            if (p.number_logged < 8)
            {
                ++p.number_logged;
                LL_WARNS("Vulkan") << "vertex buffer number verify: number " << e.number << " (vertex buffer " << e.vertex_buffer
                                   << ", index buffer " << e.index_buffer << ", " << e.vertices << " vertices, " << e.indices
                                   << " indices): " << part << (strcmp(part, "an attribute") ? "" : " (location ")
                                   << (strcmp(part, "an attribute") ? std::string() : std::to_string(location) + ")") << " differs from GL's copy at "
                                   << at << " (research/197 1b)" << LL_ENDL;
            }
        }
    }

    // A write lands while a draw of this frame reads the place: the pair
    // moves to a new place first, its contents copied there on the GPU in
    // the frame's uploads (which run before the frame's draws): the draws
    // recorded so far read the old place as it was, later ones the new one.
    // The old ranges go back after the frames in flight. False: no room, or
    // no upload commands now (the caller drops the place).
    bool numbered_move(Pool& p, Entry& e, U32 frame)
    {
        U32 chunk = U32_MAX, vertex_base = 0, index_base = 0;
        if (!take_ranges(p, e.vertex_room, e.index_room, chunk, vertex_base, index_base))
        {
            return false;
        }
        auto give_back = [&]()   // never written: free at once
        {
            p.chunks[chunk].vertex_ranges.release(vertex_base, e.vertex_room);
            p.chunks[chunk].index_ranges.release(index_base, e.index_room);
        };
        VkBuffer from[MAX_COPIED] = {};
        VkBuffer to[MAX_COPIED] = {};
        for (U32 k = 0; k < e.copied_count; ++k)
        {
            const Copied& c = e.copied[k];
            const Stream* old_stream = stream_for(p, e.chunk, c.location, c.stride, false);
            from[k] = old_stream ? old_stream->buffer : VK_NULL_HANDLE;
            const Stream* new_stream = stream_for(p, chunk, c.location, c.stride, true);   // may grow the chunk's streams
            to[k] = new_stream ? new_stream->buffer : VK_NULL_HANDLE;
            if (!from[k] || !to[k])
            {
                give_back();
                return false;
            }
        }
        VkCommandBuffer cmd = LLVKMirror::instance().uploadCommands();
        if (!cmd)
        {
            give_back();
            return false;
        }
        if (written_since_barrier(p, e, frame))
        {
            upload_barrier(p, cmd);     // the old place is read after a copy into it
        }
        for (U32 k = 0; k < e.copied_count; ++k)
        {
            const U32 stride = e.copied[k].stride;
            const VkBufferCopy region = { (VkDeviceSize)e.vertex_base * stride, (VkDeviceSize)vertex_base * stride,
                                          (VkDeviceSize)e.vertices * stride };
            if (region.size)
            {
                vkCmdCopyBuffer(cmd, from[k], to[k], 1, &region);
            }
        }
        const VkBufferCopy indices = { (VkDeviceSize)e.index_base * 4, (VkDeviceSize)index_base * 4, (VkDeviceSize)e.indices * 4 };
        if (indices.size)
        {
            vkCmdCopyBuffer(cmd, p.chunks[e.chunk].indices, p.chunks[chunk].indices, 1, &indices);
        }
        Release r;
        r.chunk = e.chunk;
        r.vertex_base = e.vertex_base;
        r.vertex_room = e.vertex_room;
        r.index_base = e.index_base;
        r.index_room = e.index_room;
        r.frame = LLVKTracker::frame();
        p.releases.push_back(r);
        e.chunk = chunk;
        e.vertex_base = vertex_base;
        e.index_base = index_base;
        e.serial = ++p.serials;     // tickets of the old place fail (they draw it again by number)
        e.placement = ++p.placements ? p.placements : ++p.placements;
        e.read_frame = U32_MAX;
        e.written_frame = U32_MAX;  // a new place: only the copies just recorded are in it
        note_written_whole(p, e, frame);
        p.used_vertices += e.vertex_room;
        p.used_indices += e.index_room;
        ++p.number_counts.moved;
        number_changed(p, e.number);   // research/198 5.5
        return true;
    }

    // A pair whose attribute cannot be copied: its place goes, the entry
    // stays as not copyable until the buffer is written or laid out again.
    void numbered_unplace(Pool& p, U32 index)
    {
        Entry& e = p.numbered[index];
        if (e.placed)
        {
            Release r;
            r.chunk = e.chunk;
            r.vertex_base = e.vertex_base;
            r.vertex_room = e.vertex_room;
            r.index_base = e.index_base;
            r.index_room = e.index_room;
            r.frame = LLVKTracker::frame();
            p.releases.push_back(r);
            e.placed = false;
            e.copied_count = 0;
            number_changed(p, e.number);   // research/198 5.5
        }
        p.shadows[index].reset();
    }

    // place() for a draw with a number: the number's place, made (copied
    // from the tracker's copy, as a pair by name) at its first draw or after
    // it went. sample: verify's (a CPU copy beside it, compared now and then;
    // nothing draws from it, so it is not marked read).
    bool place_numbered(Pool& p, const LLRHI::DirectDraw& d, LLVKAttribPool::Placement& out, LLVKAttribPool::Ticket& ticket, bool sample)
    {
        const U32 frame = p.frame;
        NumberEntry* n = number_at(p, d.vb);
        if (!n)
        {
            return false;
        }
        bool changed = false;   // research/198 5.5: a place made, or an attribute's array copied into it
        if (n->pair)
        {
            Entry& e = p.numbered[n->pair - 1];
            if (other_layout(e, d.vertex_buffer, d.index_buffer, d.index_type, d.buffer_vertices, d.buffer_indices))
            {
                numbered_drop(p, d.vb);     // a layout the number was not told of (made again below)
                ++p.number_counts.relaid;
            }
            else if (!e.placed)
            {
                e.used_frame = frame;
                return false;               // not copyable, until written or laid out again
            }
        }
        if (!n->pair)
        {
            Entry e;
            e.vertex_buffer = d.vertex_buffer;
            e.index_buffer = d.index_buffer;
            e.index_type = d.index_type;
            e.vertices = d.buffer_vertices;
            e.indices = d.buffer_indices;
            e.used_frame = frame;
            e.number = d.vb;
            const CopyResult result = copy_in(p, e);
            if (result == CopyResult::Later)
            {
                return false;
            }
            if (result == CopyResult::Placed)
            {
                note_written(p, e, frame, MAX_COPIED, 0, e.indices);
                changed = true;
            }
            U32 index = 0;
            if (!p.numbered_free.empty())
            {
                index = p.numbered_free.back();
                p.numbered_free.pop_back();
            }
            else
            {
                index = (U32)p.numbered.size();
                p.numbered.emplace_back();      // grows with the buffers drawn, not per frame
                p.shadows.emplace_back();
            }
            p.numbered[index] = e;
            n->pair = index + 1;
            if (result == CopyResult::Never)
            {
                ++p.number_counts.never;
                return false;
            }
            if (sample)
            {
                p.shadows[index].reset(new NumberShadow());
                shadow_indices(*p.shadows[index], p.numbered[index]);
                ++p.number_counts.sampled;
            }
        }
        const U32 index = n->pair - 1;
        Entry& e = p.numbered[index];
        NumberShadow* shadow = p.shadows[index].get();
        // Every attribute the draw reads, copied once per place (as place()).
        for (U32 k = 0; k < d.attribute_count; ++k)
        {
            const LLRHI::DirectDraw::Attribute& a = d.attributes[k];
            const CopiedState state = copied_state(e, a, changed);
            if (state == CopiedState::Other)
            {
                // Another layout of the same buffer: made again on its next draw.
                numbered_drop(p, d.vb);
                ++p.number_counts.relaid;
                return false;
            }
            if (state == CopiedState::Same)
            {
                continue;   // the array is there (this location, if new, is noted: research/293 branch 1)
            }
            const CopyResult result = copy_attribute(p, e, a);
            if (result == CopyResult::Later)
            {
                return false;
            }
            if (result == CopyResult::Never)
            {
                numbered_unplace(p, index);
                ++p.number_counts.never;
                return false;
            }
            note_written(p, e, frame, e.copied_count - 1, 0, (U64)e.vertices * a.stride);
            changed = true;
            if (shadow)
            {
                shadow_stream(*shadow, e, e.copied_count - 1);
            }
        }
        e.used_frame = frame;
        if (!sample)
        {
            e.read_frame = frame;
        }
        if (changed)
        {
            number_changed(p, d.vb);   // research/198 5.5
        }
        if (shadow && p.number_verify && (!shadow->checked_once || frame - shadow->checked_frame >= NUMBER_CHECK_FRAMES) &&
            p.checked_this_frame < NUMBER_CHECK_BYTES)
        {
            numbered_check(p, index, frame);
        }
        out.chunk = e.chunk;
        out.vertex_offset = (S32)e.vertex_base;
        out.first_index = e.index_base;
        ticket.vb = d.vb;
        ticket.vertex_buffer = e.vertex_buffer;
        ticket.serial = e.serial;
        ticket.epoch = U32_MAX;
        ticket.entry = nullptr;
        ticket.placement = out;
        ++p.number_counts.placed;
        return true;
    }

    // LLRHI::Buffers::write's work: `size` bytes of the number's vertex (or
    // index) buffer at `offset`, from src, into the number's place: every
    // copied attribute whose array the bytes overlap, the indices they hold
    // (made 32-bit). In place, unless a draw of this frame read the place
    // (then it moves first). Nothing to do when the place has no copy of
    // those bytes (an attribute no draw read yet: copied from the tracker's
    // copy when one does). When the write cannot land now (no room to move,
    // the frame's copy budget or upload ring is spent, no frame is being
    // recorded) the place goes: made again at its next draw.
    void numbered_write(Pool& p, U32 number, const LLRHI::BufferLayout& now, bool indices, U32 offset, U32 size, const U8* src)
    {
        NumberEntry* n = number_at(p, number);
        if (!n || !n->pair)
        {
            return;
        }
        const U32 frame = LLVKTracker::frame();
        if (frame != p.frame)
        {
            begin_frame(p, frame);  // its switch step may drop every place
            if (!n->pair)
            {
                return;
            }
        }
        const U32 index = n->pair - 1;
        Entry& e = p.numbered[index];
        ++e.writes;
        ++p.number_writes;
        if (other_layout(e, now.vertex_buffer, now.index_buffer, now.index_type, now.vertices, now.indices))
        {
            numbered_drop(p, number);
            ++p.number_counts.relaid;
            return;
        }
        if (!e.placed)
        {
            numbered_drop(p, number);   // not copyable before: tried again at its next draw
            return;
        }
        // The pieces of the place the bytes land in: k is the attribute (its
        // copied index), MAX_COPIED the indices; rel is where in the
        // attribute's array (bytes) or the index list (indices); count its
        // bytes (indices); from where in src.
        struct Piece
        {
            U32 k = 0;
            U64 rel = 0;
            U64 count = 0;
            U64 from = 0;
        };
        Piece pieces[MAX_COPIED];
        U32 piece_count = 0;
        U64 total = 0;
        const bool index32 = e.index_type == GL_UNSIGNED_INT;
        if (indices)
        {
            const U32 index_size = index32 ? 4 : 2;
            if (offset % index_size || size % index_size)
            {
                numbered_drop(p, number);   // not whole indices (LLVertexBuffer always writes whole ones)
                ++p.number_counts.unaligned;
                return;
            }
            const U64 first = offset / index_size;
            const U64 last = llmin(((U64)offset + size) / index_size, (U64)e.indices);
            if (first < last)
            {
                pieces[0].k = MAX_COPIED;
                pieces[0].rel = first;
                pieces[0].count = last - first;
                pieces[0].from = 0;
                piece_count = 1;
                total = (last - first) * 4;

                // As copy_in: every rewritten index has to name one of the
                // pair's vertices (past them the streams hold other pairs).
                U32 max_index = 0;
                if (index32)
                {
                    const U32* from = (const U32*)src;
                    for (U64 j = 0; j < pieces[0].count; ++j)
                    {
                        max_index = llmax(max_index, from[j]);
                    }
                }
                else
                {
                    const U16* from = (const U16*)src;
                    for (U64 j = 0; j < pieces[0].count; ++j)
                    {
                        max_index = llmax(max_index, (U32)from[j]);
                    }
                }
                if (max_index >= e.vertices)
                {
                    numbered_drop(p, number);
                    ++p.number_counts.dropped;
                    return;
                }
            }
        }
        else
        {
            for (U32 k = 0; k < e.copied_count; ++k)
            {
                const Copied& c = e.copied[k];
                const U64 begin = c.offset;
                const U64 end = begin + (U64)e.vertices * c.stride;
                const U64 s0 = llmax((U64)offset, begin);
                const U64 s1 = llmin((U64)offset + size, end);
                if (s0 < s1)
                {
                    Piece& piece = pieces[piece_count++];
                    piece.k = k;
                    piece.rel = s0 - begin;
                    piece.count = s1 - s0;
                    piece.from = s0 - offset;
                    total += s1 - s0;
                }
            }
        }
        if (!piece_count)
        {
            return;
        }
        if (p.scene_read_frame == frame && scene_interest(p, number))
        {
            e.read_frame = frame;   // research/198 5.5: the retained scene's draws of this frame read it too
        }
        if (e.read_frame == frame)
        {
            if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only
            {
                LLRHIVBCensus::count(LLRHIVBCensus::C_POOL_MOVED);
                if (LLRHIVBCensus::sSnapshot)
                {
                    LLRHIVBCensus::count(LLRHIVBCensus::C_SNAP_MOVED);
                }
            }
            if (!numbered_move(p, e, frame))
            {
                numbered_drop(p, number);
                ++p.number_counts.dropped;
                return;
            }
        }
        else
        {
            ++p.number_counts.in_place;
        }
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceSize ring_offset = 0;
        U8* dst = nullptr;
        if (staging(p, total, cmd, ring, ring_offset, dst) != CopyResult::Placed)
        {
            if (LLRHIVBCensus::sOn)     // research/293 branch 0: counting only (reads what staging left; calls nothing that moves a counter)
            {
                LLRHIVBCensus::count(!LLVKMirror::instance().isRecording() ? LLRHIVBCensus::C_POOL_NO_RECORD
                                     : cmd ? LLRHIVBCensus::C_POOL_STAGING_FAILED : LLRHIVBCensus::C_POOL_BUDGET);
            }
            numbered_drop(p, number);
            ++p.number_counts.dropped;
            return;
        }
        for (U32 i = 0; i < piece_count; ++i)
        {
            if (overlaps_written(p, e, frame, pieces[i].k, pieces[i].rel, pieces[i].rel + pieces[i].count))
            {
                upload_barrier(p, cmd);     // an earlier copy of this frame covered some of these bytes
                break;
            }
        }
        NumberShadow* shadow = p.shadows[index].get();
        U64 at = 0;
        for (U32 i = 0; i < piece_count; ++i)
        {
            const Piece& piece = pieces[i];
            VkBuffer target = VK_NULL_HANDLE;
            VkDeviceSize target_offset = 0;
            U64 bytes = 0;
            if (piece.k == MAX_COPIED)
            {
                bytes = piece.count * 4;
                U32* out = (U32*)(dst + at);
                if (index32)
                {
                    memcpy(out, src + piece.from, (size_t)bytes);
                }
                else
                {
                    const U16* from = (const U16*)(src + piece.from);
                    for (U64 j = 0; j < piece.count; ++j)
                    {
                        out[j] = from[j];
                    }
                }
                target = p.chunks[e.chunk].indices;
                target_offset = ((VkDeviceSize)e.index_base + piece.rel) * 4;
                if (shadow && piece.rel + piece.count <= shadow->indices.size())
                {
                    memcpy(shadow->indices.data() + piece.rel, out, (size_t)bytes);
                }
            }
            else
            {
                const Copied& c = e.copied[piece.k];
                const Stream* stream = stream_for(p, e.chunk, c.location, c.stride, false);
                if (!stream)
                {
                    numbered_drop(p, number);   // never so: copy_attribute made it (the copies recorded so far land in ranges that go)
                    ++p.number_counts.dropped;
                    return;
                }
                bytes = piece.count;
                memcpy(dst + at, src + piece.from, (size_t)bytes);
                target = stream->buffer;
                target_offset = (VkDeviceSize)e.vertex_base * c.stride + piece.rel;
                if (shadow && piece.rel + piece.count <= shadow->streams[piece.k].size())
                {
                    memcpy(shadow->streams[piece.k].data() + piece.rel, src + piece.from, (size_t)bytes);
                }
            }
            const VkBufferCopy region = { ring_offset + at, target_offset, bytes };
            vkCmdCopyBuffer(cmd, ring, target, 1, &region);
            note_written(p, e, frame, piece.k, piece.rel, piece.rel + piece.count);
            at += bytes;
        }
        ++p.number_counts.writes;
        p.number_counts.write_bytes += total;
    }

    // redeem() for a ticket by number: the number's place, while it is the
    // ticket's (the same serial: not moved, dropped or made again since).
    bool redeem_numbered(Pool& p, LLVKAttribPool::Ticket& t, LLVKAttribPool::Placement& out)
    {
        NumberEntry* n = number_at(p, t.vb);
        Entry* e = n && n->pair ? &p.numbered[n->pair - 1] : nullptr;
        if (!e || !e->placed || e->serial != t.serial || !p.number_take)
        {
            t.serial = 0;
            return false;
        }
        const U32 frame = LLVKTracker::frame();
        e->used_frame = frame;
        e->read_frame = frame;
        ++p.number_counts.redeemed;
        out = t.placement;
        return true;
    }
    // ---- end of part 2 ----
}

bool LLVKAttribPool::supported()
{
    Pool& p = sPool;
    if (!p.tried)
    {
        p.tried = true;
        const LLVKDevice& device = LLVKDevice::instance();
        bool count = false;
        if (device.isReady())
        {
            // The device enables drawIndirectCount whenever the GPU has it (llvkdevice.cpp).
            VkPhysicalDeviceVulkan12Features have12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            have.pNext = &have12;
            vkGetPhysicalDeviceFeatures2(device.getPhysicalDevice(), &have);
            count = have12.drawIndirectCount == VK_TRUE;
            if (count)
            {
                p.draw_indirect_count =
                    (PFN_vkCmdDrawIndexedIndirectCount)vkGetDeviceProcAddr(device.getDevice(), "vkCmdDrawIndexedIndirectCount");
            }
        }
        p.supported = device.isReady() && device.hasMultiDrawIndirect() && device.hasDrawIndirectFirstInstance() && count &&
                      p.draw_indirect_count && device.getLimits().maxDrawIndirectCount >= MAX_RUN_DRAWS;
        // LLRHI_DRAW_POOL_MB: the most device memory the pool takes (default 512 MB).
        U64 mb = 512;
        LLRHI::envNumber("LLRHI_DRAW_POOL_MB", 0, ~(U64)0, mb);
        p.budget = llclamp(mb, (U64)64, (U64)4096) * 1024 * 1024;
        if (p.supported)
        {
            LL_INFOS("VKAttribPool") << "vertex attribute pool up to " << (p.budget >> 20) << " MB" << LL_ENDL;
        }
        else
        {
            LL_WARNS("VKAttribPool") << "no multi-draw indirect with a first instance and a count buffer: draw runs unavailable" << LL_ENDL;
        }
    }
    return p.supported;
}

PFN_vkCmdDrawIndexedIndirectCount LLVKAttribPool::drawIndexedIndirectCount()
{
    return supported() ? sPool.draw_indirect_count : nullptr;
}

bool LLVKAttribPool::place(const LLRHI::DirectDraw& d, Placement& out)
{
    Ticket ticket;
    return place(d, out, ticket);
}

bool LLVKAttribPool::place(const LLRHI::DirectDraw& d, Placement& out, Ticket& ticket)
{
    Pool& p = sPool;
    if (!d.vertex_buffer || !d.index_buffer || !d.index_count || !d.buffer_vertices || !d.buffer_indices || !d.attributes ||
        !d.attribute_count || d.attribute_count > MAX_COPIED || (d.index_type != GL_UNSIGNED_SHORT && d.index_type != GL_UNSIGNED_INT) ||
        (U64)d.first_index + d.index_count > d.buffer_indices || !supported())
    {
        return false;
    }
    if (!p.callback)
    {
        // From the first copy on; it stays when the switch goes off, so copies
        // never outlive a GL write.
        LLVKTracker::addBufferChangedCallback(&on_buffer_changed);
        p.callback = true;
    }
    p.used = true;
    const U32 frame = LLVKTracker::frame();
    if (frame != p.frame)
    {
        begin_frame(p, frame);
    }
    // ---- research/197 stage 1b: a draw with a vertex buffer number ----
    if (d.vb && p.number_take)
    {
        return place_numbered(p, d, out, ticket, false);
    }
    if (d.vb && p.number_verify && !(d.vb & (NUMBER_VERIFY_SAMPLE - 1)))
    {
        // Verify: the sample keeps a place by number beside the pair by name (never drawn).
        Placement sample_place;
        Ticket sample_ticket;
        place_numbered(p, d, sample_place, sample_ticket, true);
    }
    ticket.vb = 0;
    ++p.old_named_places;   // research/218 vk-s7f: by the GL name (the old path)
    // ---- end of the draw with a number ----

    auto it = p.entries.find(d.vertex_buffer);
    if (it != p.entries.end() && !is_forgotten(it->second))
    {
        Entry& e = it->second;
        if (e.index_buffer != d.index_buffer || e.index_type != d.index_type || e.vertices != d.buffer_vertices ||
            e.indices != d.buffer_indices)
        {
            // The viewer describes the pair differently than when it was copied: copy it again.
            it = forget(p, it);     // the emptied entry, or end()
            ++p.relaid;
        }
        else
        {
            e.used_frame = frame;
            if (!e.placed)
            {
                return false;
            }
        }
    }
    // A new pair: no entry, or one forget() emptied (research/58).
    if (it == p.entries.end() || is_forgotten(it->second))
    {
        if (churning(p, d.vertex_buffer, frame) || churning(p, d.index_buffer, frame))
        {
            ++p.rewritten;
            return false;
        }
        Entry e;
        e.vertex_buffer = d.vertex_buffer;
        e.index_buffer = d.index_buffer;
        e.index_type = d.index_type;
        e.vertices = d.buffer_vertices;
        e.indices = d.buffer_indices;
        e.used_frame = frame;
        const CopyResult result = copy_in(p, e);
        if (result == CopyResult::Later)
        {
            return false;
        }
        if (it != p.entries.end())
        {
            it->second = e;     // into the forgotten entry's node
            --p.forgotten;
        }
        else
        {
            it = p.entries.emplace(d.vertex_buffer, e).first;
        }
        p.by_index[d.index_buffer].push_back(d.vertex_buffer);
        if (result == CopyResult::Never)
        {
            ++p.never;
            return false;
        }
    }

    // Every attribute the draw reads, copied once per pair.
    Entry& e = it->second;
    for (U32 k = 0; k < d.attribute_count; ++k)
    {
        const LLRHI::DirectDraw::Attribute& a = d.attributes[k];
        bool noted = false;     // (the pairs by name have no scene to tell)
        const CopiedState state = copied_state(e, a, noted);
        if (state == CopiedState::Other)
        {
            // Another layout of the same buffer: copy it again on its next draw.
            forget(p, it);
            ++p.relaid;
            return false;
        }
        if (state == CopiedState::Same)
        {
            continue;
        }
        const CopyResult result = copy_attribute(p, e, a);
        if (result == CopyResult::Later)
        {
            return false;
        }
        if (result == CopyResult::Never)
        {
            drop(p, e);     // stays as not copyable until GL writes it
            ++p.never;
            return false;
        }
    }
    out.chunk = e.chunk;
    out.vertex_offset = (S32)e.vertex_base;
    out.first_index = e.index_base;
    ticket.vertex_buffer = e.vertex_buffer;
    ticket.serial = e.serial;
    ticket.epoch = p.epoch;
    ticket.entry = &e;
    ticket.placement = out;
    return true;
}

namespace
{
    // A ticket by name: the pool's record of the pair (looked up again when
    // an entry was erased since), still the ticket's placement; marks it used
    // this frame. (research/218 vk-s7g: the redeem memo before it is retired.)
    bool redeem_full(Pool& p, LLVKAttribPool::Ticket& t, U32 frame, LLVKAttribPool::Placement& out)
    {
        if (t.epoch != p.epoch)
        {
            // Some pair was dropped or forgotten since: look this one up again.
            auto it = p.entries.find(t.vertex_buffer);
            if (it == p.entries.end() || !it->second.placed || it->second.serial != t.serial)
            {
                t.serial = 0;   // gone for good (a new placement has a new serial)
                return false;
            }
            t.entry = &it->second;
            t.epoch = p.epoch;
        }
        Entry& e = *(Entry*)t.entry;
        if (!e.placed || e.serial != t.serial)
        {
            // Dropped, forgotten (an emptied entry) or placed again since, in the
            // same node (research/58: forgetting no longer moves the epoch).
            t.serial = 0;
            return false;
        }
        e.used_frame = frame;
        out = t.placement;
        return true;
    }
}

bool LLVKAttribPool::redeem(Ticket& t, Placement& out)
{
    Pool& p = sPool;
    if (!t.serial)
    {
        return false;
    }
    if (t.vb)
    {
        return redeem_numbered(p, t, out);  // research/197 stage 1b
    }
    ++p.old_named_redeems;  // research/218 vk-s7f: by the GL name (the old path)
    return redeem_full(p, t, LLVKTracker::frame(), out);
}

VkBuffer LLVKAttribPool::streamBuffer(U32 chunk, U32 location, U32 stride)
{
    Stream* s = stream_for(sPool, chunk, location, stride, false);
    return s ? s->buffer : VK_NULL_HANDLE;
}

VkBuffer LLVKAttribPool::indexBuffer(U32 chunk)
{
    return chunk < sPool.chunks.size() ? sPool.chunks[chunk].indices : VK_NULL_HANDLE;
}

U32 LLVKAttribPool::chunkCount()
{
    return (U32)sPool.chunks.size();
}

bool LLVKAttribPool::streamAddress(U32 chunk, U32 location, U32 stride, VkDeviceAddress& address, VkDeviceSize& size)
{
    address = 0;
    size = 0;
    Stream* s = stream_for(sPool, chunk, location, stride, false);
    if (!s || !s->buffer || !(LLVKDevice::instance().generatedCommands() || LLVKDevice::instance().hasRayTracing()))
    {
        return false;
    }
    VkBufferDeviceAddressInfo info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = s->buffer;
    address = vkGetBufferDeviceAddress(LLVKDevice::instance().getDevice(), &info);
    size = (VkDeviceSize)sPool.chunks[chunk].vertex_capacity * stride;
    return address != 0;
}

bool LLVKAttribPool::indexAddress(U32 chunk, VkDeviceAddress& address, VkDeviceSize& size)
{
    address = 0;
    size = 0;
    if (chunk >= sPool.chunks.size() || !sPool.chunks[chunk].indices ||
        !(LLVKDevice::instance().generatedCommands() || LLVKDevice::instance().hasRayTracing()))
    {
        return false;
    }
    VkBufferDeviceAddressInfo info = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    info.buffer = sPool.chunks[chunk].indices;
    address = vkGetBufferDeviceAddress(LLVKDevice::instance().getDevice(), &info);
    size = (VkDeviceSize)sPool.chunks[chunk].index_capacity * 4;
    return address != 0;
}

U32 LLVKAttribPool::pin(U32 vertex_buffer)
{
    Pool& p = sPool;
    auto it = p.entries.find(vertex_buffer);
    if (it == p.entries.end() || !it->second.placed || !it->second.placement)
    {
        return 0;
    }
    ++it->second.pins;
    return it->second.placement;
}

void LLVKAttribPool::unpin(U32 vertex_buffer, U32 pin)
{
    Pool& p = sPool;
    auto it = p.entries.find(vertex_buffer);
    if (pin && it != p.entries.end() && it->second.placement == pin && it->second.pins)
    {
        --it->second.pins;
    }
}

void LLVKAttribPool::setDropCallback(void (*callback)(U32 vertex_buffer))
{
    sPool.dropped = callback;
}

std::string LLVKAttribPool::takeStats(U32 frames)
{
    Pool& p = sPool;
    if (!p.used)
    {
        return std::string();
    }
    frames = llmax(frames, 1u);
    std::ostringstream out;
    out << "attribute pool " << (p.allocated >> 20) << " of " << (p.budget >> 20) << " MB in " << p.chunks.size() << " chunks (vertices "
        << p.used_vertices << ", indices " << p.used_indices << "), pairs " << p.entries.size() - p.forgotten << ", copied " << p.copies << " ("
        << (p.copied_bytes >> 20) << " MB), dropped " << p.drops << ", relaid " << p.relaid << ", not copyable " << p.never
        << ", deferred (budget " << p.later_budget << ", full " << p.later_full << ", ring " << p.later_ring << ", rewritten "
        << p.rewritten << ") over " << frames << " frames";
    p.copies = p.drops = p.never = p.later_budget = p.later_full = p.later_ring = p.rewritten = p.relaid = 0;
    p.copied_bytes = 0;
    return out.str();
}

std::string LLVKAttribPool::takeOldPathStats()
{
    Pool& p = sPool;
    std::ostringstream out;
    out << "vertex buffers by GL name: places " << p.old_named_places << ", redeems " << p.old_named_redeems;
    p.old_named_places = p.old_named_redeems = 0;
    return out.str();
}

// ---- research/197 stage 1b: LLRHI::Buffers' backend ----
namespace
{
    LLRHI::BufferRef numbers_create()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Pool& p = sPool;
        U32 id = 0;
        if (!p.number_free.empty() && p.numbers[p.number_free.front()].free_frame <= LLVKTracker::frame())
        {
            id = p.number_free.front();
            p.number_free.pop_front();
        }
        else if (p.numbers.size() < NUMBER_MAX)
        {
            id = (U32)p.numbers.size();
            p.numbers.emplace_back();   // grows with the buffers, not per frame
        }
        else
        {
            return LLRHI::BufferRef();  // the buffer goes by its GL names
        }
        NumberEntry& n = p.numbers[id];
        n.live = true;
        n.pair = 0;
        ++p.number_counts.made;
        return LLRHI::BufferRef{ id };
    }

    void numbers_layout(LLRHI::BufferRef ref, const LLRHI::BufferLayout& now)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Pool& p = sPool;
        const NumberEntry* n = number_at(p, ref.id);
        if (n && n->pair &&
            other_layout(p.numbered[n->pair - 1], now.vertex_buffer, now.index_buffer, now.index_type, now.vertices, now.indices))
        {
            numbered_drop(p, ref.id);
            ++p.number_counts.relaid;
        }
    }

    void numbers_write(LLRHI::BufferRef ref, const LLRHI::BufferLayout& now, bool indices, U32 offset, U32 size, const void* src)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        numbered_write(sPool, ref.id, now, indices, offset, size, (const U8*)src);
    }

    void numbers_release(LLRHI::BufferRef ref)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        Pool& p = sPool;
        NumberEntry* n = number_at(p, ref.id);
        if (!n)
        {
            return;
        }
        numbered_drop(p, ref.id);
        if (LLRHIVBCensus::sOn)
        {
            LLRHIVBCensus::release(ref.id);     // research/293 branch 0: counting only
        }
        n->live = false;
        n->free_frame = LLVKTracker::frame() + NUMBER_RETIRE_FRAMES;   // a ticket of this frame never finds another buffer's place
        p.number_free.push_back(ref.id);
        ++p.number_counts.released;
    }
}

void LLVKAttribPool::installNumbers()
{
    if (!LLRHI::sDirect)
    {
        LL_INFOS("VKAttribPool") << "vertex buffer numbers off: no direct draws (research/197 1b)" << LL_ENDL;
        return;
    }
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        sPool.numbers_installed = true;
    }
    LLRHI::Buffers::Backend backend;
    backend.create = &numbers_create;
    backend.layout = &numbers_layout;
    backend.write = &numbers_write;
    backend.release = &numbers_release;
    LLRHI::Buffers::install(backend);
    LL_INFOS("VKAttribPool") << "vertex buffer numbers: "
                             << (!LLRHI::sVBHandles ? "off" : LLRHI::sVBHandlesAuto ? "auto" : LLRHI::sVBHandlesVerify ? "verify" : "on")
                             << " (LLRHI_VB_HANDLES, research/197 1b)" << LL_ENDL;
}

std::string LLVKAttribPool::numbersReport(U32 frames)
{
    Pool& p = sPool;
    const NumberCounts& c = p.number_counts;
    if (!p.numbers_installed || !(p.number_seen_on || c.made || c.released))
    {
        return std::string();
    }
    frames = llmax(frames, 1u);
    std::ostringstream out;
    out << ", vertex buffer numbers ["
        << (!p.number_seen_on ? "off" : p.number_seen_verify ? (LLRHI::sVBHandlesAuto ? "auto verify" : "verify") : "on") << ", live "
        << (p.numbers.size() - 1 - p.number_free.size()) << " made " << c.made << " released " << c.released << ", places "
        << (p.numbered.size() - p.numbered_free.size()) << ", placed " << c.placed / frames << " redeemed " << c.redeemed / frames
        << ", writes " << c.writes / frames << " (" << c.write_bytes / frames / 1024 << " KB, in place " << c.in_place / frames
        << ", moved " << c.moved / frames << ", barriers " << c.barriers / frames << "), dropped " << c.dropped << " relaid " << c.relaid
        << " not copyable " << c.never << " unaligned " << c.unaligned;
    if (p.number_seen_verify)
    {
        out << " (verify: sampled " << c.sampled << " checked " << c.checked << " (" << c.check_bytes / 1024 << " KB) mismatches "
            << c.mismatches << ")";
    }
    out << "]";
    p.number_counts = NumberCounts();
    return out.str();
}
// ---- end of LLRHI::Buffers' backend ----

// ---- research/198 5.5: the retained scene's places by number ----
// Only while draws take places by number (the switch on, not verifying):
// then every write reaches the place (numbered_write) and it is what a draw
// by number would read. A verify sample's place is not handed out.
bool LLVKAttribPool::numberPlace(U32 number, NumberPlace& out)
{
    Pool& p = sPool;
    const NumberEntry* n = number_at(p, number);
    if (!p.number_take || !n || !n->pair)
    {
        return false;
    }
    const Entry& e = p.numbered[n->pair - 1];
    if (!e.placed)
    {
        return false;
    }
    out.chunk = e.chunk;
    out.vertex_base = e.vertex_base;
    out.index_base = e.index_base;
    out.vertices = e.vertices;
    out.indices = e.indices;
    out.serial = e.serial;
    out.writes = e.writes;
    out.copied = 0;
    for (U32 k = 0; k < e.copied_count; ++k)
    {
        out.copied |= e.copied[k].locations;    // research/293 branch 1: the locations each array serves (the key's bit alone without the switch)
    }
    return true;
}

bool LLVKAttribPool::numbersTaken()
{
    return sPool.number_take;
}

void LLVKAttribPool::setNumberListener(void (*listener)(U32 number))
{
    sPool.number_listener = listener;
}

U64 LLVKAttribPool::numberWriteSerial()
{
    return sPool.number_writes;
}

void LLVKAttribPool::numberInterest(U32 number, bool on)
{
    Pool& p = sPool;
    if (!number || number >= NUMBER_MAX)
    {
        return;     // not a number
    }
    if (number >= p.number_interest.size())
    {
        p.number_interest.resize(std::max((size_t)number + 1, p.number_interest.size() * 2), 0);   // doubling: not per record
    }
    U16& count = p.number_interest[number];
    if (on)
    {
        count = count == U16_MAX ? count : (U16)(count + 1);
    }
    else if (count)
    {
        --count;
    }
}

void LLVKAttribPool::sceneDrawing(bool on)
{
    sPool.scene_drawing = on;
}

void LLVKAttribPool::sceneDrew()
{
    sPool.scene_read_frame = LLVKTracker::frame();
}
// ---- end of the retained scene's places ----

std::string LLVKAttribPool::memoryReport()
{
    Pool& p = sPool;
    if (!p.used)
    {
        return std::string();
    }
    U64 vertex_capacity = 0, index_capacity = 0;
    for (const Chunk& c : p.chunks)
    {
        vertex_capacity += c.vertex_capacity;
        index_capacity += c.index_capacity;
    }
    std::ostringstream out;
    out << "attribute pool " << (p.allocated >> 20) << " MB in " << p.chunks.size() << " chunks (vertices " << p.used_vertices
        << " of " << vertex_capacity << ", indices " << p.used_indices << " of " << index_capacity << ", at most "
        << (p.budget >> 20) << " MB)";
    return out.str();
}
