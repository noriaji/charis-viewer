/**
 * @file packetmaterials.cpp
 * @brief Draw packets (research/180 stage 4 P1): the legacy material loop of
 *        LLDrawPoolMaterials::renderDeferred, its static batches as packets.
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

// Why the pixels cannot change (research/180 10.1): a batch drawn the usual
// way in this loop is its uniform calls (only the values that differ from
// the batch before), its textures by name (plus/materiallist DirectUnits) or
// its binds, and pushDirectBatch: the pass's matrices times its model
// matrix (a static batch's: its region's), its buffers and range, and its
// template (research/46 G4c). An entry carries exactly those: the uniform
// values (the backend applies them to a copy of the program's material row,
// never to the program), the texture names directName gives (the same names
// directTexture hands the draw), the pass's matrices as pushDirectBatch makes
// them for the batch's model matrix (made once per model matrix a frame; a
// packet holds the entries of one model matrix and, P1d, an object's short
// stretches with matrices of their own), and the buffers and range. The backend
// resolves the program, the run, the pass cache state and the words that do not change per batch once
// per packet, and per entry checks the entry against the record the usual
// way left beside the batch's template; anything that differs stops the
// packet there, and that batch goes the usual way in its place (the entries
// before it already drawn), which writes the record again.

#include "llviewerprecompiledheaders.h"

#include "packet.h"

#include "llappviewer.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrhi.h"
#include "llrhidrawlist.h"
#include "llrhihash.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llvertexbuffer.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llworld.h"
#include "pipeline.h"
#include "llgraywire.h"
#include "plus/materiallist/materiallist.h"
#include "plus/skinsss/skinsss.h"

#include "glm/gtc/type_ptr.hpp"

#include <cstring>
#include <intrin.h>

namespace
{
    using LLRHI::PacketEntry;
    constexpr U32 KEEP_UNIT = LLRHI::DirectDraw::KEEP_UNIT;
    // Entries collected before they are handed over (a longer stretch goes in
    // several packets). Static storage: no allocation per loop or frame.
    constexpr U32 MAX_ENTRIES = 4096;
    PacketEntry sEntries[MAX_ENTRIES];
    LLDrawInfo* sBatches[MAX_ENTRIES];
    U32 sLoopSerial = 0;

    // PacketEntry::values: where the loop's uniforms go (fixed places, all
    // filled whether the program has the uniform or not).
    constexpr U32 VALUE_SPECULAR = 0;       // 4 floats
    constexpr U32 VALUE_INTENSITY = 4;
    constexpr U32 VALUE_MIN_ALPHA = 5;
    constexpr U32 VALUE_FULLBRIGHT = 6;

    // ---- research/180 stage 4: the packet's region matrices (tools/selftest sim_packet extracts it) ----
    // The pass's matrices with a model matrix (a static batch's region's:
    // registerFace gives every static batch &region->mRenderMatrix; P1c, a
    // moving object's: its render or world matrix), made once per model
    // matrix, view and projection a frame (keyed by their words: a region or
    // an object moved, or another pass's view, makes them again), not per
    // batch nor per packet. `Made` is what is made (the viewer's: the
    // DirectMatrices and their pointers, with an id).
    //
    // P1d: the key is what the matrices are made from and nothing else: the
    // model matrix's words (not its address: two matrices with the same words
    // make the same matrices), the view, the projection, and which pointers
    // the bound shader has fillDirectMatrices fill (not the shader: the loops
    // of a pass, a shader each, share an object's matrices). A matrix id
    // shared by programs is safe: the backend's rows by id also compare the
    // program.
    constexpr U32 KEY_MODEL = 1;        // model_words hold a model matrix (else none)
    constexpr U32 KEY_NORMAL = 2;       // the shader has the normal matrix
    constexpr U32 KEY_INVERSE = 4;      // the inverse modelview
    constexpr U32 KEY_MVP = 8;          // the modelview projection
    struct MatrixKey
    {
        F32 model_words[16] = {};       // its words (zero without one)
        F32 view[16] = {};
        F32 projection[16] = {};
        U32 bits = 0;                   // KEY_*
    };

    // Bitwise, as pushDirectBatch compares the view.
    inline bool same_key(const MatrixKey& a, const MatrixKey& b)
    {
        if (a.bits != b.bits)
        {
            return false;
        }
        if (memcmp(a.model_words, b.model_words, sizeof(a.model_words)) != 0)
        {
            return false;
        }
        if (memcmp(a.view, b.view, sizeof(a.view)) != 0)
        {
            return false;
        }
        return memcmp(a.projection, b.projection, sizeof(a.projection)) == 0;
    }

    // A key's hash (its words bitwise, as same_key compares them): FNV-1a
    // (selftest sim_packet copies this block, with llrhihash.h).
    inline U64 key_hash(const MatrixKey& k)
    {
        U64 h = LLRHI::FNV1A_BASIS_SHORT ^ k.bits;
        h = LLRHI::fnv1aFloats(h, k.model_words, 16);
        h = LLRHI::fnv1aFloats(h, k.view, 16);
        h = LLRHI::fnv1aFloats(h, k.projection, 16);
        return h ^ (h >> 29);
    }

    template <typename Made> struct MatrixSets
    {
        // Model matrices times views a frame (heavy-extreme asked 67 to 69
        // with the shader in the key, research/180 10.11). A set is never
        // given up within its frame, so what get or kept returned stays until
        // the frame ends; past SIZE, get makes the key again into the spare
        // (correct, slower: what pushDirectBatch makes for every model
        // matrix) and kept says none.
        static constexpr U32 SIZE = 128;
        static constexpr U32 BUCKETS = 256;     // a power of two, twice SIZE: a probe always ends
        struct Set
        {
            MatrixKey key;
            Made made;
        };
        Set sets[SIZE];
        Set spare;
        U32 frame = U32_MAX;
        U32 used = 0;                   // sets of this frame
        U32 epoch = 0;                  // per frame; a bucket of another epoch is empty
        U32 bucket_epoch[BUCKETS] = {};
        U32 bucket_set[BUCKETS] = {};   // the set's index

        // The set of `key` this frame, made now if there is none and room;
        // nullptr: the frame's sets are full.
        template <typename Make> Set* find(U32 frame_now, const MatrixKey& key, Make& make)
        {
            if (frame != frame_now)
            {
                frame = frame_now;
                used = 0;
                if (++epoch == 0)
                {
                    epoch = 1;  // wrapped: no bucket may look current
                    for (U32& e : bucket_epoch)
                    {
                        e = 0;
                    }
                }
            }
            for (U32 b = (U32)key_hash(key) & (BUCKETS - 1);; b = (b + 1) & (BUCKETS - 1))
            {
                if (bucket_epoch[b] != epoch)
                {
                    if (used >= SIZE)
                    {
                        return nullptr;
                    }
                    Set& s = sets[used];
                    bucket_epoch[b] = epoch;
                    bucket_set[b] = used++;
                    s.key = key;
                    make(s.key, s.made);
                    return &s;
                }
                Set& s = sets[bucket_set[b]];
                if (same_key(s.key, key))
                {
                    return &s;
                }
            }
        }

        // The made matrices of `key` this frame; make(key, made) when there are
        // none. Past SIZE: the spare, made again (until the next get).
        template <typename Make> const Made& get(U32 frame_now, const MatrixKey& key, Make make)
        {
            if (Set* s = find(frame_now, key, make))
            {
                return s->made;
            }
            spare.key = key;
            make(spare.key, spare.made);
            return spare.made;
        }

        // As get, but only matrices that stay until the frame ends (a packet's
        // entry holds them past later gets): nullptr when the frame's sets are full.
        template <typename Make> const Made* kept(U32 frame_now, const MatrixKey& key, Make make)
        {
            Set* s = find(frame_now, key, make);
            return s ? &s->made : nullptr;
        }
    };
    // ---- end of the packet's region matrices ----

    // What pushDirectBatch's matrix cache makes for a batch (the same
    // arithmetic: the view times the model matrix, LLRender::fillDirectMatrices),
    // with an id of its own.
    struct MadeMatrices
    {
        LLRender::DirectMatrices storage;
        LLRHI::DirectDraw draw;     // its matrix pointers and id only
    };
    MatrixSets<MadeMatrices> sMatrixSets;

    // The key of the pass's matrices with `model` under the bound shader.
    MatrixKey matrix_key(const LLMatrix4* model)
    {
        MatrixKey key;
        if (model)
        {
            key.bits |= KEY_MODEL;
            memcpy(key.model_words, model->mMatrix, sizeof(key.model_words));
        }
        memcpy(key.view, gGLModelView, sizeof(key.view));
        memcpy(key.projection, glm::value_ptr(gGL.getProjectionMatrix()), sizeof(key.projection));
        // Which pointers fillDirectMatrices fills, by the same tests.
        if (LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr)
        {
            key.bits |= shader->getUniformLocation(LLShaderMgr::NORMAL_MATRIX) > -1 ? KEY_NORMAL : 0;
            key.bits |= shader->getUniformLocation(LLShaderMgr::INVERSE_MODELVIEW_MATRIX) >= 0 ? KEY_INVERSE : 0;
            key.bits |= shader->getUniformLocation(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX) > -1 ? KEY_MVP : 0;
        }
        return key;
    }

    // What pushDirectBatch's matrix cache makes for the key (the bound shader's pointers).
    void make_matrices(const MatrixKey& k, MadeMatrices& m)
    {
        glm::mat4 modelview = glm::make_mat4(k.view);
        if (k.bits & KEY_MODEL)
        {
            modelview *= glm::make_mat4(k.model_words);
        }
        m.draw = LLRHI::DirectDraw();
        gGL.fillDirectMatrices(modelview, m.storage, m.draw);
        m.draw.matrix_id = LLRHI::DrawList::newMatrixId();
        ++LLRHI::sPacketCensus.matrix_sets;
    }

    // A packet's matrices (Packet::matrices): until the next call.
    const LLRHI::DirectDraw& pass_matrices(const LLMatrix4* model)
    {
        const LLRHI::DirectDraw& made = sMatrixSets.get(gFrameCount, matrix_key(model), make_matrices).draw;
        if (&made == &sMatrixSets.spare.made.draw)
        {
            ++LLRHI::sPacketCensus.matrix_sets_full;   // past the frame's sets: made again
        }
        return made;
    }

    // P1d: an entry's own matrices (PacketEntry::matrices): until the frame
    // ends; nullptr when the frame's sets are full.
    const LLRHI::DirectDraw* entry_matrices(const LLMatrix4* model)
    {
        const MadeMatrices* made = sMatrixSets.kept(gFrameCount, matrix_key(model), make_matrices);
        if (!made)
        {
            ++LLRHI::sPacketCensus.matrix_sets_full;
        }
        return made ? &made->draw : nullptr;
    }

    // Whether `model` is a region's render matrix (a static batch's).
    bool is_region_matrix(const LLMatrix4* model)
    {
        for (const LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            if (region && model == &region->mRenderMatrix)
            {
                return true;
            }
        }
        return false;
    }

    // The census's distinct uniform value sets of a frame (research/180 10.4
    // P1-0): a small open table of their hashes, emptied by the frame number.
    struct ValueSets
    {
        static constexpr U32 SIZE = 1024;   // a power of two; full: no more counted this frame
        U64 keys[SIZE] = {};
        U32 frames[SIZE] = {};
        U32 used = 0;
        U32 frame = U32_MAX;
    };
    ValueSets sValueSets;

    void count_value_set(const F32* values)
    {
        ValueSets& v = sValueSets;
        if (v.frame != gFrameCount)
        {
            v.frame = gFrameCount;
            v.used = 0;
        }
        const U64 key = LLRHI::fnv1aFloats(LLRHI::FNV1A_BASIS_SHORT, values, LLRHI::PACKET_VALUES) | 1;  // never 0
        for (U32 probe = 0, at = (U32)key & (ValueSets::SIZE - 1); probe < 16; ++probe, at = (at + 1) & (ValueSets::SIZE - 1))
        {
            if (v.frames[at] != v.frame)
            {
                if (v.used * 2 >= ValueSets::SIZE)
                {
                    return;     // half full: stop counting rather than probe long
                }
                v.frames[at] = v.frame;
                v.keys[at] = key;
                ++v.used;
                ++LLRHI::sPacketCensus.value_sets;
                return;
            }
            if (v.keys[at] == key)
            {
                return;
            }
        }
    }

    // P1c: a packet costs its head (the backend's gather of the pass's words,
    // the lock, the pass's matrices): an object's batches go by packet only
    // where this many or more of them come in a row (fewer: P1d, inside the
    // packet around them with their own matrices, which costs a matrix row
    // each, not a head; LLRHI_PACKET_SHORT=0, the usual way as before P1d).
    // Not tuned by a measurement yet (the census counts the short ones).
    constexpr U32 MIN_OBJECT_STRETCH = 3;

    // How many batches from `from` on (at most `want`) have model matrix `model`, in a row.
    U32 stretch_of(LLDrawInfo* const* from, LLDrawInfo* const* end, const LLMatrix4* model, U32 want)
    {
        U32 n = 0;
        for (; from != end && n < want && *from && (*from)->mModelMatrix == model; ++from)
        {
            ++n;
        }
        return n;
    }

    // The census's object matrices by address (verify and census only):
    // whether their words moved in the last MOVING_FRAMES frames. A small
    // open table, emptied when half full.
    constexpr U32 MOVING_FRAMES = 30;
    struct Motion
    {
        static constexpr U32 SIZE = 4096;   // a power of two
        struct Slot
        {
            const void* at = nullptr;
            U64 words = 0;                  // a hash of the matrix's words
            U32 seen = U32_MAX;             // the frame it was last hashed
            U32 first = 0;                  // the frame it was first seen
            U32 changed = U32_MAX;          // the frame its words last changed (U32_MAX: never seen to)
        };
        Slot slots[SIZE];
        U32 used = 0;
    };
    Motion sMotion;

    void count_motion(const LLDrawInfo& params)
    {
        Motion& m = sMotion;
        const U32 frame = gFrameCount;
        const void* at = params.mModelMatrix;
        U32 raw[16];
        memcpy(raw, params.mModelMatrix->mMatrix, sizeof(raw));
        const U64 words = LLRHI::fnv1aWords(LLRHI::FNV1A_BASIS_SHORT, raw, 16);
        Motion::Slot* s = nullptr;
        const U64 h = (U64)(uintptr_t)at;
        for (U32 probe = 0, i = (U32)((h >> 4) ^ (h >> 16)) & (Motion::SIZE - 1); probe < 16; ++probe, i = (i + 1) & (Motion::SIZE - 1))
        {
            Motion::Slot& slot = m.slots[i];
            if (slot.at == at)
            {
                s = &slot;
                break;
            }
            if (!slot.at)
            {
                if (m.used * 2 >= Motion::SIZE)
                {
                    break;
                }
                slot = Motion::Slot();
                slot.at = at;
                slot.words = words;
                slot.seen = frame;
                slot.first = frame;
                ++m.used;
                s = &slot;
                break;
            }
        }
        if (!s)
        {
            for (Motion::Slot& slot : m.slots)
            {
                slot = Motion::Slot();  // full (or a long probe): start again
            }
            m.used = 0;
            return;
        }
        if (s->seen != frame)
        {
            if (s->words != words)
            {
                s->changed = frame;
                s->words = words;
            }
            s->seen = frame;
        }
        LLRHI::PacketCensus& c = LLRHI::sPacketCensus;
        if (s->changed != U32_MAX && frame - s->changed < MOVING_FRAMES)
        {
            ++c.moving;
        }
        else if (frame - s->first >= MOVING_FRAMES)
        {
            ++c.still;
            const U8 kind = params.mRHIMatrixKind;
            const U8 keeps = plus::packet::KIND_ATTACHMENT | plus::packet::KIND_FLEXIBLE | plus::packet::KIND_ANIMESH | plus::packet::KIND_OMEGA;
            if ((kind & (plus::packet::KIND_ACTIVE_ROOT | plus::packet::KIND_ACTIVE_CHILD)) && !(kind & keeps))
            {
                ++c.returnable;
            }
        }
    }

    // The census's counts of a batch with an object's matrix, by its kind.
    void count_object(const LLDrawInfo& params, bool motion)
    {
        LLRHI::PacketCensus& c = LLRHI::sPacketCensus;
        ++c.objects;
        const U8 kind = params.mRHIMatrixKind;
        c.kind_unknown += kind ? 0 : 1;
        c.animated_child += (kind & plus::packet::KIND_ANIMATED_CHILD) ? 1 : 0;
        c.active_root += (kind & plus::packet::KIND_ACTIVE_ROOT) ? 1 : 0;
        c.active_child += (kind & plus::packet::KIND_ACTIVE_CHILD) ? 1 : 0;
        c.attachment += (kind & plus::packet::KIND_ATTACHMENT) ? 1 : 0;
        c.flexible += (kind & plus::packet::KIND_FLEXIBLE) ? 1 : 0;
        c.animesh += (kind & plus::packet::KIND_ANIMESH) ? 1 : 0;
        c.omega += (kind & plus::packet::KIND_OMEGA) ? 1 : 0;
        if (motion)
        {
            count_motion(params);
        }
    }

    // The note on one usual-way draw (cleared on every way out).
    struct NoteScope
    {
        explicit NoteScope(const LLRHI::PacketNote* note) { LLRHI::tPacketNote = note; }
        ~NoteScope() { LLRHI::tPacketNote = nullptr; }
        NoteScope(const NoteScope&) = delete;
        NoteScope& operator=(const NoteScope&) = delete;
    };

    class Loop
    {
    public:
        Loop(LLGLSLShader* shader, S32 diffuse_channel, S32 spec_channel, S32 norm_channel, bool verify, bool census_only);
        Loop(const Loop&) = delete;
        Loop& operator=(const Loop&) = delete;

        // False: the pass cannot take packets (nothing done).
        bool usable() const { return mUsable; }
        // False: not the loop this copies (no diffuse unit): not counted either.
        bool countable() const { return mCountable; }
        void run(LLDrawInfo** begin, LLDrawInfo** end);
        void census(LLDrawInfo** begin, LLDrawInfo** end);

    private:
        // A channel's texture name, worked out when the batch's texture
        // differs from the batch before's (as DirectUnits and the binds do).
        struct Name
        {
            S32 unit = -1;
            U32 which = 0;              // the batch's map: 0 normal, 1 specular, 2 diffuse
            bool diffuse = false;
            LLTexture* texture = nullptr;
            U32 name = KEEP_UNIT;
            bool have = false;
        };
        U32 nameOf(Name& n, LLTexture* texture);
        bool entryOf(LLDrawInfo& params, PacketEntry& e);
        bool takeStretch(const LLDrawInfo& params, LLDrawInfo* const* next, LLDrawInfo* const* end);
        bool ownMatrices(const LLDrawInfo& params, PacketEntry& e);
        void official(LLDrawInfo& params, const PacketEntry* entry);
        void flush();
        void verifyHead();

        LLGLSLShader* mShader;
        GLint mIntensity, mBrightness, mMinAlpha, mSpecular;
        S32 mDiffuseChannel, mSpecChannel, mNormChannel;
        // The official loop's own state (it starts as the official code leaves it before the loop).
        F32 mLastIntensity = 0.f;
        F32 mLastFullbright = 0.f;
        F32 mLastMinimumAlpha = 0.f;
        LLVector4 mLastSpecular = LLVector4(0, 0, 0, 0);
        LLTexture* mLastNormalMap = nullptr;
        LLTexture* mLastSpecMap = nullptr;
        LLTexture* mLastDiffuse = nullptr;
        plus::materiallist::DirectUnits mDirectUnits;

        Name mNames[LLRHI::MAX_PACKET_UNITS];   // per mPacket.units
        LLRHI::Packet mPacket;
        LLRHI::PacketNote mNote;
        U32 mCount = 0;             // entries collected (sEntries, sBatches)
        U32 mEmptyCalls = 0;        // packets in a row that drew nothing
        U32 mHeadTries = 0;         // verify: tries of the head
        const LLMatrix4* mRegionMatrix = nullptr;   // the last model matrix found to be a region's
        const LLMatrix4* mOtherMatrix = nullptr;    // the last found not to be
        bool mOtherEye = false;                     // mOtherMatrix is an eye attachment's while the skin is marked
        const LLMatrix4* mPacketModel = nullptr;    // the model matrix of mPacket.matrices
        const LLMatrix4* mStretchModel = nullptr;   // P1c: the model matrix of the stretch being taken (nullptr: none)
        bool mHaveMatrices = false;                 // mPacket.matrices made (for mPacketModel)
        bool mEntryObject = false;                  // entryOf's last entry has an object's matrix (P1c)
        bool mUsable = false;
        bool mCountable = false;
        bool mLive = false;         // entries are made (not refused for this loop)
        bool mPackets = true;       // they go by packet (not given up for this loop)
        bool mVerify = false;
        bool mCensusOnly = false;
        bool mVerifyHead = false;   // verify: the backend has this stretch's head
        bool mHeadRefused = false;  // verify: the backend takes no packet of this pass
    };

    Loop::Loop(LLGLSLShader* shader, S32 diffuse_channel, S32 spec_channel, S32 norm_channel, bool verify, bool census_only)
        : mShader(shader),
          mIntensity(shader->getUniformLocation(LLShaderMgr::ENVIRONMENT_INTENSITY)),
          mBrightness(shader->getUniformLocation(LLShaderMgr::EMISSIVE_BRIGHTNESS)),
          mMinAlpha(shader->getUniformLocation(LLShaderMgr::MINIMUM_ALPHA)),
          mSpecular(shader->getUniformLocation(LLShaderMgr::SPECULAR_COLOR)),
          mDiffuseChannel(diffuse_channel),
          mSpecChannel(spec_channel),
          mNormChannel(norm_channel),
          mDirectUnits(diffuse_channel, spec_channel, norm_channel),
          mVerify(verify),
          mCensusOnly(census_only)
    {
        // The units, in the official loop's order: normal, specular, diffuse.
        const S32 channels[LLRHI::MAX_PACKET_UNITS] = { norm_channel, spec_channel, diffuse_channel };
        for (U32 k = 0; k < LLRHI::MAX_PACKET_UNITS; ++k)
        {
            if (channels[k] < 0 || channels[k] >= 32)
            {
                if (k == 2)
                {
                    return;     // no diffuse unit: not the loop this copies
                }
                continue;
            }
            Name& n = mNames[mPacket.unit_count];
            n.unit = channels[k];
            n.which = k;
            n.diffuse = k == 2;
            mPacket.units[mPacket.unit_count++] = (U32)channels[k];
        }
        mCountable = true;
        // The uniforms the loop sets per batch, each with its native slot:
        // a uniform the backend does not take natively goes through GL, which
        // a packet cannot do.
        struct Wanted
        {
            GLint location;
            U32 index;
            U32 components;
            U32 first;
        };
        const Wanted wanted[] =
        {
            { mSpecular, LLShaderMgr::SPECULAR_COLOR, 4, VALUE_SPECULAR },
            { mIntensity, LLShaderMgr::ENVIRONMENT_INTENSITY, 1, VALUE_INTENSITY },
            { mMinAlpha, LLShaderMgr::MINIMUM_ALPHA, 1, VALUE_MIN_ALPHA },
            { mBrightness, LLShaderMgr::EMISSIVE_BRIGHTNESS, 1, VALUE_FULLBRIGHT },
        };
        for (const Wanted& w : wanted)
        {
            if (w.location < 0)
            {
                continue;
            }
            const S32 slot = shader->mNativeProgram ? shader->nativeSlotOfIndex(w.index) : -1;
            if (slot < 0)
            {
                return;
            }
            LLRHI::PacketUniform& u = mPacket.uniforms[mPacket.uniform_count++];
            u.slot = slot;
            u.components = w.components;
            u.first = w.first;
        }
        mPacket.loop = ++sLoopSerial;
        mNote.packet = &mPacket;
        mNote.verify = verify;
        mUsable = true;
        mLive = true;
    }

    U32 Loop::nameOf(Name& n, LLTexture* texture)
    {
        texture = LLTexUnit::unitTexture(texture, n.unit);     // GrayWire's look on unit 0, as bindFast (review retro-vulkan 1)
        if (n.have && n.texture == texture)
        {
            return n.name;
        }
        U32 name = KEEP_UNIT;
        if (!texture)
        {
            // No diffuse map: the white texture (unbindFast). No normal or
            // specular map: the official loop keeps the unit (not an entry).
            name = n.diffuse ? LLTexUnit::sWhiteTexture : KEEP_UNIT;
        }
        else
        {
            name = LLTexUnit::directRef(texture, n.unit);   // research/197 1a: its number while LLRHI::sTexHandles is on
            if (name != KEEP_UNIT)
            {
                texture->setActive();   // as directTexture does for a texture it hands over by name
            }
        }
        n.texture = texture;
        n.name = name;
        n.have = true;
        return name;
    }

    // Whether a packet may take the batch (counted for the census), and its entry.
    bool Loop::entryOf(LLDrawInfo& params, PacketEntry& e)
    {
        LLRHI::PacketCensus& c = LLRHI::sPacketCensus;
        ++c.batches;
        mEntryObject = false;
        if (!params.mRHIDrawTemplate)
        {
            ++c.no_template;
            return false;
        }
        if (params.mAvatar.notNull())
        {
            ++c.avatar;
            return false;
        }
        if (params.mTextureMatrix)
        {
            ++c.texture_matrix;
            return false;
        }
        // A static batch's model matrix is its region's (registerFace's
        // region_space); a moving one's (isActive, ANIMATED_CHILD) is its
        // object's. P1c: those too (a packet holds one model matrix, made
        // from its words when the packet starts, as pushDirectBatch makes it).
        if (params.mModelMatrix && params.mModelMatrix != mRegionMatrix)
        {
            if (params.mModelMatrix == mOtherMatrix || !is_region_matrix(params.mModelMatrix))
            {
                if (params.mModelMatrix != mOtherMatrix)
                {
                    mOtherMatrix = params.mModelMatrix;
                    // A rigid object worn on an eye point (an attachment: an
                    // object's matrix) is marked while drawn (the skin mark's
                    // BatchScope in pushDirectBatch): an entry would draw it unmarked.
                    mOtherEye = plus::skinsss::detail::sMarking && plus::skinsss::detail::isEyeAttachment(params);
                }
                if (mOtherEye)
                {
                    ++c.eye_attachment;
                    return false;
                }
                count_object(params, mVerify || mCensusOnly);
                if (!LLRHI::sPacketObjects)
                {
                    ++c.model_matrix;
                    return false;
                }
                mEntryObject = true;
            }
            else
            {
                mRegionMatrix = params.mModelMatrix;
            }
        }
        LLVertexBuffer* vb = params.mVertexBuffer;
        if (!vb || vb->isMapped())
        {
            ++c.mapped;
            return false;
        }
        LLTexture* const textures[LLRHI::MAX_PACKET_UNITS] = { params.mNormalMap.get(), params.mSpecularMap.get(), params.mTexture.get() };
        for (U32 k = 0; k < mPacket.unit_count; ++k)
        {
            Name& n = mNames[k];
            const U32 name = nameOf(n, textures[n.which]);
            if (name == KEEP_UNIT)
            {
                ++c.unnamed;
                return false;
            }
            e.names[k] = name;
        }
        e.owner = &params.mRHIDrawTemplate;
        e.matrices = nullptr;   // the packet's (ownMatrices may give it its own)
        e.vertex_buffer = vb->getGLBuffer();
        e.index_buffer = vb->getGLIndices();
        e.index_type = vb->getIndicesType();
        e.buffer_vertices = vb->getNumVerts();
        e.buffer_indices = vb->getNumIndices();
        e.first_index = params.mOffset;
        e.index_count = params.mCount;
        memcpy(&e.values[VALUE_SPECULAR], params.mSpecColor.mV, 4 * sizeof(F32));
        e.values[VALUE_INTENSITY] = params.mEnvIntensity;
        e.values[VALUE_MIN_ALPHA] = params.mAlphaMaskCutoff;
        e.values[VALUE_FULLBRIGHT] = params.mFullbright ? 1.f : 0.f;
        e.values[7] = 0.f;
        ++c.eligible;
        if (mEntryObject)
        {
            ++c.object_matrix;
        }
        else
        {
            c.region_matrix += params.mModelMatrix ? 1 : 0;
        }
        if (mVerify || mCensusOnly)
        {
            count_value_set(e.values);
        }
        return true;
    }

    // P1c: whether the entry entryOf just made goes by packet with the
    // packet's matrices as far as its model matrix goes: a region's (or none)
    // always; an object's in a stretch of MIN_OBJECT_STRETCH or more of its
    // batches in a row (`next` the batch after it). False: a short stretch
    // (the caller takes it with matrices of its own, P1d, or the usual way).
    bool Loop::takeStretch(const LLDrawInfo& params, LLDrawInfo* const* next, LLDrawInfo* const* end)
    {
        if (mEntryObject && params.mModelMatrix != mStretchModel &&
            1 + stretch_of(next, end, params.mModelMatrix, MIN_OBJECT_STRETCH - 1) < MIN_OBJECT_STRETCH)
        {
            return false;
        }
        mStretchModel = params.mModelMatrix;
        return true;
    }

    // P1d: whether a short stretch's entry joins the packet being collected
    // with matrices of its own (e.matrices; nullptr where they are the
    // packet's), not cutting it: only behind a packet's matrices (a head),
    // and while the frame's matrix sets have room (they stay until the frame
    // ends; an entry holds them until its packet is drawn). Counted either way.
    bool Loop::ownMatrices(const LLDrawInfo& params, PacketEntry& e)
    {
        LLRHI::PacketCensus& c = LLRHI::sPacketCensus;
        e.matrices = nullptr;
        if (LLRHI::sPacketShort && mHaveMatrices)
        {
            if (params.mModelMatrix == mPacketModel)
            {
                ++c.short_taken;
                return true;    // the packet's own model matrix: its matrices
            }
            e.matrices = entry_matrices(params.mModelMatrix);
            if (e.matrices)
            {
                ++c.short_taken;
                return true;
            }
        }
        ++c.short_stretch;
        return false;
    }

    // The official loop's body for one batch (lldrawpoolmaterials.cpp,
    // renderDeferred, static: no palette), with the entry's note around its
    // draw when it has one.
    void Loop::official(LLDrawInfo& params, const PacketEntry* entry)
    {
        LLGLSLShader* const shader = mShader;
        if (mSpecular > -1 && params.mSpecColor != mLastSpecular)
        {
            mLastSpecular = params.mSpecColor;
            shader->uniform4fv(LLShaderMgr::SPECULAR_COLOR, 1, mLastSpecular.mV);
        }

        if (mIntensity != -1 && mLastIntensity != params.mEnvIntensity)
        {
            mLastIntensity = params.mEnvIntensity;
            shader->uniform1f(LLShaderMgr::ENVIRONMENT_INTENSITY, mLastIntensity);
        }

        if (mMinAlpha > -1 && mLastMinimumAlpha != params.mAlphaMaskCutoff)
        {
            mLastMinimumAlpha = params.mAlphaMaskCutoff;
            shader->uniform1f(LLShaderMgr::MINIMUM_ALPHA, mLastMinimumAlpha);
        }

        F32 fullbright = params.mFullbright ? 1.f : 0.f;
        if (mBrightness > -1 && mLastFullbright != fullbright)
        {
            mLastFullbright = fullbright;
            shader->uniform1f(LLShaderMgr::EMISSIVE_BRIGHTNESS, mLastFullbright);
        }

        if (!mDirectUnits.byName() && mNormChannel > -1 && params.mNormalMap != mLastNormalMap)
        {
            mLastNormalMap = params.mNormalMap;
            llassert(mLastNormalMap);
            gGL.getTexUnit(mNormChannel)->bindFast(mLastNormalMap);
        }

        if (!mDirectUnits.byName() && mSpecChannel > -1 && params.mSpecularMap != mLastSpecMap)
        {
            mLastSpecMap = params.mSpecularMap;
            llassert(mLastSpecMap);
            gGL.getTexUnit(mSpecChannel)->bindFast(mLastSpecMap);
        }

        if (!mDirectUnits.byName() && params.mTexture != mLastDiffuse)
        {
            mLastDiffuse = params.mTexture;
            if (mLastDiffuse)
            {
                gGL.getTexUnit(mDiffuseChannel)->bindFast(mLastDiffuse);
            }
            else
            {
                gGL.getTexUnit(mDiffuseChannel)->unbindFast(LLTexUnit::TT_TEXTURE);
            }
        }

        bool tex_setup = false;

        //not batching textures or batch has only 1 texture -- might need a texture matrix
        if (params.mTextureMatrix)
        {
            gGL.getTexUnit(0)->activate();
            gGL.matrixMode(LLRender::MM_TEXTURE);

            gGL.loadMatrix((GLfloat*)params.mTextureMatrix->mMatrix);
            gPipeline.mTextureMatrixOps++;

            tex_setup = true;
        }

        {
            mNote.entry = entry;
            const NoteScope note(entry ? &mNote : nullptr);
            // research/243: GrayWire for the draw by name too (drawGeometry has its own). research/256: twice
            // while the materialize band sweeps it, but not under a note (one draw makes the entry's record; a
            // band's textures are held off the packets, so a batch with an entry has none)
            GrayWire::drawBatch(params, [&]()
            {
                if (!mDirectUnits.draw(params))
                {
                    LLRenderPass::drawGeometry(params);
                }
            }, entry == nullptr);
        }

        if (tex_setup)
        {
            gGL.getTexUnit(0)->activate();
            gGL.loadIdentity();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            // The direct draw synced unit 0's texture matrix with the batch's
            // animation; the packets after it go by the program's: the identity
            // back now (as materiallist's drewOldWay).
            gGL.syncMatrices();
        }
    }

    // The entries so far, as packets; each one a packet stops at drawn the
    // usual way in its place (writing its record), the rest handed over again.
    void Loop::flush()
    {
        // The packets go by the program's matrices (unit 0's texture matrix
        // among them): synced with gGL first (only the hash compared when
        // nothing changed).
        gGL.syncMatrices();
        U32 at = 0;
        while (at < mCount)
        {
            mPacket.entries = sEntries + at;
            mPacket.count = mCount - at;
            mPacket.verify = false;
            mPacket.stop = LLRHI::PACKET_DONE;
            const U32 drawn = LLRHI::sPacketDraw(mPacket);
            at += llmin(drawn, mCount - at);
            mEmptyCalls = drawn ? 0 : mEmptyCalls + 1;
            if (at >= mCount)
            {
                break;
            }
            // Nothing of this loop goes by packet (the program, the pass: the
            // rest the usual way, no records either), or packet after packet
            // drew nothing (each costs the head: the rest the usual way, its
            // records made for the next frame).
            if (mPacket.stop == LLRHI::PACKET_REFUSED || mEmptyCalls >= 3)
            {
                const bool records = mPacket.stop != LLRHI::PACKET_REFUSED;
                mPackets = false;
                mLive = records;
                for (; at < mCount; ++at)
                {
                    official(*sBatches[at], records ? &sEntries[at] : nullptr);
                }
                break;
            }
            official(*sBatches[at], &sEntries[at]);
            ++at;
        }
        mCount = 0;
    }

    // Verify: the backend's head for this loop, once the run is open for its
    // program (after a batch went the usual way), a few tries at most.
    void Loop::verifyHead()
    {
        if (mVerifyHead || mHeadRefused || mHeadTries >= 4)
        {
            return;
        }
        ++mHeadTries;
        mPacket.entries = nullptr;
        mPacket.count = 0;
        mPacket.verify = true;
        mPacket.stop = LLRHI::PACKET_DONE;
        LLRHI::sPacketDraw(mPacket);
        mVerifyHead = mPacket.stop == LLRHI::PACKET_DONE;
        mHeadRefused = mPacket.stop == LLRHI::PACKET_REFUSED;   // not this pass
    }

    void Loop::run(LLDrawInfo** begin, LLDrawInfo** end)
    {
        const U64 start = __rdtsc();
        U64 batches = 0;
        // As the official loop: the backend's lock held across it (research/180 9 3, off unless LLRHI_TRACKER_HOLD).
        LLRHI::TrackerHold tracker_hold(begin != end);
        // What pushDirectBatch does first for every batch (the projection,
        // lights and the pass's modelview, as through GL), before the pass's
        // matrices are made from the same state.
        gGL.syncMatrices();
        mPacket.matrices = nullptr;
        for (LLDrawInfo** i = begin; i != end;)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_MATERIAL("materials draw loop (packets)");
            LLDrawInfo* const batch = *i;
            LLCullResult::increment_iterator(i, end);
            if (!batch)
            {
                continue;   // as pushBatches skips one (it draws nothing)
            }
            LLDrawInfo& params = *batch;
            ++batches;
            if (mLive)
            {
                PacketEntry* e = &sEntries[mCount];
                bool own = false;   // P1d: a short stretch's entry, in this packet with its own matrices
                if (entryOf(params, *e) && (takeStretch(params, i, end) || (own = ownMatrices(params, *e))))
                {
                    // The entries of a packet share their model matrix (the
                    // region's or an object's): another one hands the ones so
                    // far over first (their matrices stay as they are), then
                    // the pass's matrices for this one (made once a frame per
                    // model matrix). An entry with its own matrices does not.
                    if (!own && (!mHaveMatrices || params.mModelMatrix != mPacketModel))
                    {
                        if (mCount)
                        {
                            flush();    // mCount 0 after it: this entry moves to the first place
                            ++LLRHI::sPacketCensus.matrix_cuts;
                            sEntries[0] = *e;
                            e = &sEntries[0];
                        }
                        // Verify: a packet takes its head again with the new
                        // matrices, so verify does too (the shared words are
                        // compared with the head of their own matrices).
                        if (mVerify && mHaveMatrices)
                        {
                            mVerifyHead = false;
                            mPacket.loop = ++sLoopSerial;
                            mHeadTries = 0;
                        }
                        mPacketModel = params.mModelMatrix;
                        mPacket.matrices = &pass_matrices(mPacketModel);
                        mHaveMatrices = true;
                        if (!mLive)
                        {
                            official(params, nullptr);  // the flush found the pass refused: the rest the usual way, no records
                            continue;
                        }
                    }
                    if (mVerify)
                    {
                        official(params, e);
                        verifyHead();
                        continue;
                    }
                    if (!mPackets)
                    {
                        official(params, e);    // packets given up for this loop: its record still made
                        continue;
                    }
                    sBatches[mCount++] = &params;
                    if (mCount == MAX_ENTRIES)
                    {
                        flush();
                    }
                    continue;
                }
            }
            flush();
            official(params, nullptr);
            mStretchModel = nullptr;    // an object's batches after it: a stretch of their own
            // Verify: a batch the packet would not take may change what the
            // loop's head holds (a texture matrix loaded and reset): a packet
            // would start again after it, so verify takes the head again too.
            if (mVerify)
            {
                if (mVerifyHead)
                {
                    mVerifyHead = false;
                    mPacket.loop = ++sLoopSerial;   // the backend's head of the stretch before is not compared any more
                }
                mHeadTries = 0;
            }
        }
        flush();
        LLRHI::PacketCensus& c = LLRHI::sPacketCensus;
        ++c.loops;
        c.loop_batches += batches;
        c.loop_ticks += __rdtsc() - start;
    }

    // Census only (P1-0): what a packet could take, nothing drawn here.
    void Loop::census(LLDrawInfo** begin, LLDrawInfo** end)
    {
        PacketEntry e;
        for (LLDrawInfo** i = begin; i != end;)
        {
            LLDrawInfo* const batch = *i;
            ++i;
            if (!batch)
            {
                continue;
            }
            LLDrawInfo& params = *batch;
            if (!entryOf(params, e))
            {
                mStretchModel = nullptr;    // as run: a batch the usual way ends a stretch
            }
            else if (!takeStretch(params, i, end))
            {
                // A short stretch: P1d would take it with its own matrices
                // (none made here; the sets' room is not counted), else the usual way.
                if (LLRHI::sPacketShort)
                {
                    ++LLRHI::sPacketCensus.short_taken;
                }
                else
                {
                    ++LLRHI::sPacketCensus.short_stretch;
                    mStretchModel = nullptr;
                }
            }
        }
        ++LLRHI::sPacketCensus.loops;
    }
}

namespace plus
{
namespace packet
{
    bool drawMaterials(LLGLSLShader* shader, LLDrawInfo** begin, LLDrawInfo** end, S32 diffuse_channel, S32 spec_channel,
                       S32 norm_channel)
    {
        const bool verify = LLRHI::sPacketsVerify;
        const bool on = LLRHI::sPackets && !verify;
        const bool census = LLRHI::sPacketsCensus;
        if (!on && !verify && !census)
        {
            return false;   // off: this test per loop call, nothing else
        }
        if (begin == end || !shader || !LLRHI::sPacketDraw || !LLRHI::sGLDrawsDisabled || !LLRHI::sDirect || !LLRHI::sDirectDraw ||
            !LLRHI::sDrawRuns || !LLRHI::sDrawTemplates || !LLRHI::sDrawTemplate || LLGLSLShader::sCurBoundShaderPtr != shader)
        {
            return false;
        }
        Loop loop(shader, diffuse_channel, spec_channel, norm_channel, verify, !on && !verify);
        if (!on && !verify)
        {
            if (loop.countable())
            {
                loop.census(begin, end);
            }
            return false;   // the official loop draws
        }
        if (!loop.usable())
        {
            return false;
        }
        loop.run(begin, end);
        return true;
    }

    U8 matrixKind(const LLDrawable& drawable, bool rigged, bool region_space)
    {
        if (rigged)
        {
            return 0;
        }
        if (region_space)
        {
            return KIND_REGION;
        }
        // registerFace's order: ANIMATED_CHILD before isActive.
        U8 kind = 0;
        if (drawable.isState(LLDrawable::ANIMATED_CHILD))
        {
            kind |= KIND_ANIMATED_CHILD;
        }
        else if (drawable.isActive())
        {
            kind |= drawable.isRoot() ? KIND_ACTIVE_ROOT : KIND_ACTIVE_CHILD;
        }
        if (const LLViewerObject* object = drawable.getVObj())
        {
            if (object->isAttachment())
            {
                kind |= KIND_ATTACHMENT;
            }
            if (object->isFlexible())
            {
                kind |= KIND_FLEXIBLE;
            }
            if (object->isAnimatedObject())
            {
                kind |= KIND_ANIMESH;
            }
            if (!object->getAngularVelocity().isExactlyZero())
            {
                kind |= KIND_OMEGA;
            }
        }
        return kind;
    }
}
}
