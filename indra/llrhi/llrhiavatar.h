/**
 * @file llrhiavatar.h
 * @brief Stage 4b (research/197 3.5, research/199): rigged avatars on the
 * Vulkan backend's own terms. Each avatar x skin palette keeps one row of the
 * palette table's resident region for its life (the draws, and later the
 * resident draw records, name it by a number that does not change), and a
 * compute skinning kernel is checked against the CPU on the very rows the
 * draws read.
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

#ifndef LL_LLRHIAVATAR_H
#define LL_LLRHIAVATAR_H

#include "stdtypes.h"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace LLRHI
{
    struct PaletteRow;
}

namespace LLRHIAvatar
{
    // The switches' values: off, verify (the new way runs beside the old one,
    // which draws, and they are compared), auto (verify until AUTO_FRAMES
    // checked frames had no mismatch, then on; off at the first mismatch), on.
    enum Mode : U8
    {
        MODE_OFF = 0,
        MODE_VERIFY,
        MODE_AUTO,
        MODE_ON,
    };
    const char* modeName(Mode mode);

    // LLRHI_AVATAR_PALETTE (research/199 A): resident palette rows. Unset: off
    // (research/219: verify while LLRHI_SCENE_RIGGED is not off).
    Mode paletteMode();
    // A backend is installed and the mode (as auto has moved it) writes the
    // resident rows: verify, auto or on.
    bool paletteWrites();
    // ... and draws read them (on, or auto after its checks).
    bool paletteDraws();
    // auto: one frame's checks of the resident rows came out (mismatches found);
    // moves auto to on after AUTO_FRAMES clean frames, to off at a mismatch.
    void notePaletteChecks(U32 checked, U32 mismatches);
    constexpr U32 AUTO_FRAMES = 600;

    // LLRHI_COMPUTE_SKIN (research/199 B): off or verify ("on" runs as verify
    // until the vertex placement of stage 1 lands; one line in the log says so).
    Mode computeSkinMode();
    bool computeSkinVerify();
    // Jobs a frame checks at most, and vertices per job.
    constexpr U32 SKIN_JOBS_PER_FRAME = 4;
    constexpr U32 SKIN_VERTICES_PER_JOB = 4096;

    // As objectSkinV.glsl's MAX_JOINTS_PER_MESH_OBJECT (LL_MAX_JOINTS_PER_MESH_OBJECT).
    constexpr U32 MAX_JOINTS = 110;

    // One compute skinning check: the vertices of a rigged face, skinned on the
    // GPU with the palette row the draws read, against the CPU with the joints
    // as built (the viewer's palette, mGLMp).
    struct SkinVerifyJob
    {
        const F32* positions = nullptr;     // 4 floats per vertex (LLVector4a), w ignored
        const F32* weights = nullptr;       // 4 floats per vertex: joint index + weight (LLVolumeFace::mWeights)
        U32 vertices = 0;
        const F32* palette = nullptr;       // 12 floats per joint as uploaded (3 vec4s, see skinPosition)
        U32 joints = 0;
        U32 row_base = 0;                   // the draws' row (LLRHI::PaletteRow base and frame)
        U32 row_frame = 0xffffffffu;
        U64 label = 0;                      // for the log (avatar and skin)
    };

    // The backend's side (llvulkan/llvkavatar.cpp). Not LLRHI hooks: one
    // object.
    class Backend
    {
    public:
        virtual ~Backend() = default;
        // vec4s of one resident row (the prebuilt palette room, 110 joints x 3).
        virtual U32 rowRoom() const = 0;
        // A resident row for owner (never 0), or SLOT_NONE (none free now: the
        // region grows at a later frame's head; draw the old way meanwhile).
        virtual U32 allocateRow(U64 owner) = 0;
        // Gives the row back; reused only after the frames in flight are done.
        virtual void releaseRow(U32 base, U64 owner) = 0;
        // Writes vec4s of data into the row in this frame's table and fills row
        // as LLRHI::sPlacePalette would (base, room, the backend's frame).
        // False: nothing written (not owner's row, no frame being recorded,
        // the table not grown to the region yet), row untouched.
        virtual bool writeRow(U32 base, U64 owner, const F32* data, U32 vec4s, LLRHI::PaletteRow& row) = 0;
        // research/219: the backend's frame now, as writeRow puts it in
        // PaletteRow::frame (a row written at another frame is in the other
        // frame slot's table: write it again before this frame's draws read it).
        virtual U32 frame() const = 0;
        // research/219 (verify): copies vec4s of the resident row at base, as
        // this frame's table holds it now, into out. False: no frame being
        // recorded, or the table does not hold the row.
        virtual bool readRow(U32 base, U32 vec4s, F32* out) const = 0;
        // Queues one compute skinning check (computeSkinVerify). False: not taken.
        virtual bool skinVerify(const SkinVerifyJob& job) = 0;
    };
    constexpr U32 SLOT_NONE = 0xffffffffu;
    void setBackend(Backend* backend);
    Backend* backend();

    // ---- avatar palette slot rule (research/199) ----
    // The rows of the palette table's resident region. Plain values only: the
    // selftest (sim-avatarpalette) compiles this block on its own.
    //
    // A row belongs to one owner (an avatar x skin) from allocate to release.
    // Released rows wait RELEASE_FRAMES frame heads: draws recorded earlier in
    // the frame of the release (and the frame before, on the other table) may
    // still read the old owner's joints there. The region grows only at a
    // frame's head (LLVKBindless::beginFrame, before the frame takes any row of
    // the table's rolling part, which starts after the region): allocate asks
    // for it when no row is free, and frameHead takes the size the table applied.
    struct SlotRule
    {
        static constexpr U32 RELEASE_FRAMES = 2;
        static constexpr U32 FIRST_ROWS = 256;
        static constexpr U32 MAX_ROWS = 2048;
        static constexpr U32 NONE = 0xffffffffu;

        struct Waiting
        {
            U32 row;
            U64 head;       // the frame head it was released in
        };

        std::vector<U64> owner;             // per row, 0: free or waiting
        std::vector<U32> free_rows;         // free now (taken from the back)
        std::deque<Waiting> waiting;        // released, oldest first
        U32 rows = 0;                       // rows of the region now
        U32 wanted = 0;                     // rows asked for (from the next frame head on)
        U64 head = 0;                       // the frame head now

        // A new frame head; applied_rows: the region's size in rows as the
        // table applied it at this head (never below rows: it only grows).
        void frameHead(U64 new_head, U32 applied_rows)
        {
            head = new_head;
            while (!waiting.empty() && waiting.front().head + RELEASE_FRAMES <= head)
            {
                free_rows.push_back(waiting.front().row);
                waiting.pop_front();
            }
            if (applied_rows > rows)
            {
                owner.resize(applied_rows, 0);
                // highest first, so the lowest new row is taken first
                for (U32 r = applied_rows; r-- > rows;)
                {
                    free_rows.push_back(r);
                }
                rows = applied_rows;
            }
        }

        // The rows the region should have from the next frame head on.
        U32 wantedRows() const { return wanted > rows ? wanted : rows; }

        U32 allocate(U64 key)
        {
            if (!key)
            {
                return NONE;
            }
            if (free_rows.empty())
            {
                const U32 grow = rows ? rows * 2 : FIRST_ROWS;
                wanted = grow < MAX_ROWS ? grow : MAX_ROWS;
                return NONE;
            }
            const U32 row = free_rows.back();
            free_rows.pop_back();
            owner[row] = key;
            return row;
        }

        bool release(U32 row, U64 key)
        {
            if (!owns(row, key))
            {
                return false;
            }
            owner[row] = 0;
            waiting.push_back({ row, head });
            return true;
        }

        bool owns(U32 row, U64 key) const { return key && row < rows && owner[row] == key; }
    };

    // The skinned position of one vertex, as getObjectSkinnedTransform
    // (objectSkinV.glsl) computes it and the compute kernel (llvkavatar.cpp)
    // does: palette is the row as the shaders read it, 3 vec4s per joint, each
    // a column of the mat3x4 (xyz: the 3x3 column, w: that row's translation).
    // Index past `joints` (the shader reads past the palette there): false.
    inline bool skinPosition(const F32* palette, U32 joints, const F32* weight4, const F32* position, F32* out)
    {
        F32 w[4];
        U32 index[4];
        F32 sum = 0.f;
        for (U32 k = 0; k < 4; ++k)
        {
            const F32 fl = std::floor(weight4[k]);
            w[k] = weight4[k] - fl;
            F32 clamped = fl < 0.f ? 0.f : fl;
            clamped = clamped > (F32)(MAX_JOINTS - 1) ? (F32)(MAX_JOINTS - 1) : clamped;
            index[k] = (U32)clamped;
            sum += w[k];
        }
        const F32 scale = 1.f / sum;
        F32 m[3][3] = {};
        F32 t[3] = {};
        for (U32 k = 0; k < 4; ++k)
        {
            if (index[k] >= joints)
            {
                return false;
            }
            const F32 wk = w[k] * scale;
            const F32* c = palette + (size_t)index[k] * 12;
            for (U32 col = 0; col < 3; ++col)
            {
                for (U32 r = 0; r < 3; ++r)
                {
                    m[col][r] += c[col * 4 + r] * wk;
                }
                t[col] += c[col * 4 + 3] * wk;
            }
        }
        for (U32 r = 0; r < 3; ++r)
        {
            out[r] = m[0][r] * position[0] + m[1][r] * position[1] + m[2][r] * position[2] + t[r];
        }
        return true;
    }
    // ---- end of the avatar palette slot rule ----
}

#endif // LL_LLRHIAVATAR_H
