/**
 * @file llvkgl_texref.cpp
 * @brief Texture numbers (research/197 stage 1a): LLRHI::Textures' table, the sampler table, the switch and its verify.
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

#include "llvkgl_internal.h"

#include <deque>

#if LL_GL_FUNC_POINTER

// A texture's number (LLRHI::TexRef) is made with its first image
// (LLImageGL::syncRHITex) and keeps its value for the texture's whole life;
// every new image (a discard level made again, an image made on the image
// thread, none after a VRAM purge) is told to it (replaceImage). Draws name
// the texture by its number (a draw handle) and gather_slots takes the slot
// the number's entry published (the publication rule in
// llvkgl_descriptors.cpp) or resolves the entry's image as a name would be.
// Here: the table and LLRHI::Textures' backend, the clear of a number's
// publication, the frame's switch (and its auto mode), verify's checks, the
// sampler table and the VKPerf line. Everything under sLock.

namespace llvkgl
{
    // ---- research/197 stage 1a: the texture number table (tools/selftest sim_texhandles extracts it) ----
    std::vector<TexRefEntry> sTexRefs(1);   // number 0 is never given out
    TexRefCounts sTexRefCounts;
    TexRefCounts sTexRefTotals;
    bool sTexRefTake = false;
    bool sTexRefVerify = false;
namespace
{
    // A retired number is given out again only this many frames later: a
    // draw handle made before its retirement (the frame's packets and
    // direct draws) never names another texture. Records past the frame (a
    // packet part's {slot, gen}) are held by the publication's gen, which
    // is never reset.
    constexpr U32 RETIRE_FRAMES = 8;
    std::deque<U32> sTexRefFree;            // retired numbers, oldest first

    // The number's publication goes, and its texture's pointer back (R2).
    void texref_unpublish(U32 id, TexRefEntry& e)
    {
        if (!e.pub.slot1)
        {
            return;
        }
        if (Texture* tex = sTextures.find(e.image))
        {
            if (tex->published_ref == id)
            {
                tex->published_ref = 0;
            }
        }
        e.pub.slot1 = 0;
        ++e.pub.gen;
        ++sTexRefCounts.clears;
    }
} // namespace

    void unpublish_ref(Texture& tex)
    {
        const U32 id = tex.published_ref;
        if (!id)
        {
            return;
        }
        tex.published_ref = 0;
        if (id < sTexRefs.size() && sTexRefs[id].pub.slot1)
        {
            sTexRefs[id].pub.slot1 = 0;
            ++sTexRefs[id].pub.gen;
            ++sTexRefCounts.clears;
        }
    }

    // A new number for an image (0: none yet); 0 when every number is taken.
    U32 texref_create(GLuint image)
    {
        U32 id = 0;
        if (!sTexRefFree.empty() && sTexRefs[sTexRefFree.front()].free_frame <= sFrame)
        {
            id = sTexRefFree.front();
            sTexRefFree.pop_front();
        }
        else if (sTexRefs.size() <= LLRHI::TEX_HANDLE_ID_MASK)
        {
            id = (U32)sTexRefs.size();
            sTexRefs.emplace_back();    // grows with the textures, not per frame
        }
        else
        {
            return 0;   // the texture goes by its GL name
        }
        TexRefEntry& e = sTexRefs[id];
        e.image = image;
        e.pub.slot1 = 0;    // (its gen goes on from where the number's last texture left it)
        e.version = 0;
        e.live = true;
        ++sTexRefCounts.created;
        return id;
    }

    void texref_replace(U32 id, GLuint image)
    {
        TexRefEntry* e = texref_entry(id);
        if (!e || e->image == image)
        {
            return;
        }
        texref_unpublish(id, *e);   // R1
        e->image = image;
        ++sTexRefCounts.replaced;
    }

    void texref_retire(U32 id)
    {
        TexRefEntry* e = texref_entry(id);
        if (!e)
        {
            return;
        }
        texref_unpublish(id, *e);
        e->image = 0;
        e->live = false;
        e->free_frame = sFrame + RETIRE_FRAMES;
        sTexRefFree.push_back(id);
        ++sTexRefCounts.retired;
    }
    // ---- end of the texture number table ----

    void texref_clear()
    {
        for (U32 id = 1; id < sTexRefs.size(); ++id)
        {
            texref_unpublish(id, sTexRefs[id]);
        }
    }

    void texref_check_all()
    {
        static U32 logged = 0;
        for (U32 id = 1; id < sTexRefs.size(); ++id)
        {
            const TexRefEntry& e = sTexRefs[id];
            const U32 p = e.pub.slot1;
            if (!p || !e.live)
            {
                continue;
            }
            const Texture* tex = sTextures.find(e.image);
            U32 broken = 0;     // I1 1, I2 2, I3 4, I4 8, I5 16, I6 32, R1/R2 64
            if (!tex || tex->published_ref != id)
            {
                broken |= 64;
            }
            else if (tex->bindless_slot != p - 1)
            {
                broken |= 1;
            }
            else
            {
                const U32 ok = LLVKBindless::checkSlot(p - 1, e.image, tex->view, sampler_for(tex, false), tex->view_serial);
                broken |= ok & LLVKBindless::SLOT_OWNER_OK ? 0 : 1;
                broken |= ok & LLVKBindless::SLOT_WANTED_OK ? 0 : 2;
                broken |= ok & LLVKBindless::SLOT_WRITTEN_OK ? 0 : 4;
                broken |= tex->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ? 0 : 8;
                broken |= tex->attachable || tex->attach_wanted || tex->rendered_to ? 16 : 0;
                broken |= e.version == tex->version ? 0 : 32;
            }
            if (broken)
            {
                ++sTexRefCounts.breaks;
                ++sTexRefTotals.breaks;
                if (logged < 8)
                {
                    ++logged;
                    LL_WARNS("Vulkan") << "texture number verify: number " << id << " (image " << e.image << ") published slot " << p - 1
                                       << " breaks" << (broken & 1 ? " I1" : "") << (broken & 2 ? " I2" : "") << (broken & 4 ? " I3" : "")
                                       << (broken & 8 ? " I4" : "") << (broken & 16 ? " I5" : "") << (broken & 32 ? " I6" : "")
                                       << (broken & 64 ? " R" : "") << " (" << LLVKBindless::describe(p - 1) << ")" << LL_ENDL;
                }
            }
        }
    }
namespace
{
    // ---- the sampler table (research/197 2.1 d) ----
    // What LLTexUnit's setTextureAddressModeFast and setTextureFilteringOptionFast
    // give a 2D texture for a sampler number: the stage 2 records sample by
    // it; stage 1 only compares it with what the texture has (verify).
    struct SamplerParams
    {
        GLenum min_filter = GL_LINEAR;
        GLenum mag_filter = GL_LINEAR;
        GLenum wrap = GL_REPEAT;
        F32 anisotropy = 1.f;       // 0: left as it is (no anisotropic filtering on the device)
    };
    F32 sAnisotropy = 0.f;          // Textures::setAnisotropy

    SamplerParams sampler_params(U16 index)
    {
        SamplerParams p;
        const U32 address = index / LLRHI::SAMPLER_FILTER_COUNT;
        const U32 filter = index % LLRHI::SAMPLER_FILTER_COUNT;
        p.wrap = address == LLRHI::SAMPLER_MIRROR ? GL_MIRRORED_REPEAT : address == LLRHI::SAMPLER_CLAMP ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        const bool mips = filter & 1;
        const U32 option = filter / 2;      // LLTexUnit::eTextureFilterOptions
        p.mag_filter = option == 0 ? GL_NEAREST : GL_LINEAR;
        if (option >= 2 && mips)
        {
            p.min_filter = GL_LINEAR_MIPMAP_LINEAR;
        }
        else if (option >= 1)
        {
            p.min_filter = mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
        }
        else
        {
            p.min_filter = mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
        }
        p.anisotropy = sAnisotropy <= 0.f ? 0.f : option == 3 && sAnisotropy > 1.f ? sAnisotropy : 1.f;
        return p;
    }

    // ---- the switch ----
    bool sSeenOn = false, sSeenVerify = false;
    U32 sAutoFrames = 0;
    U64 sAutoChecksFrom = 0;
    // As the slot publication's auto mode (research/180): verify for this
    // many frames of the world with this many comparisons in them and not one
    // mismatch, then on; off for the run at the first mismatch or break.
    constexpr U32 AUTO_FRAMES = 600;
    constexpr U64 AUTO_CHECKS = 20000;

    U64 auto_checks(const TexRefCounts& c)
    {
        return c.checked + c.viewer_checked;
    }

    bool texref_auto()
    {
        if (!LLRHI::sTexHandles || !LLRHI::sTexHandlesVerify)
        {
            LLRHI::sTexHandlesAuto = false;     // not in the state it starts from (a cue changed it)
            return false;
        }
        const TexRefCounts& c = sTexRefTotals;
        if (!LLRHI::sWorldShown)
        {
            // The login screen: verified (a mismatch still turns it off), not counted toward on.
            sAutoFrames = 0;
            sAutoChecksFrom = auto_checks(c);
        }
        else
        {
            ++sAutoFrames;
        }
        if (c.mismatches || c.breaks || c.image_mismatches)
        {
            LLRHI::sTexHandles = false;
            LLRHI::sTexHandlesAuto = false;
            LL_WARNS("Vulkan") << "texture numbers (auto): verify found " << c.mismatches << " slot mismatches, " << c.breaks
                               << " broken invariants and " << c.image_mismatches << " numbers of another image: off for this run (research/197 1a)"
                               << LL_ENDL;
            return true;
        }
        const U64 world_checks = auto_checks(c) - sAutoChecksFrom;
        if (sAutoFrames >= AUTO_FRAMES && world_checks >= AUTO_CHECKS)
        {
            LLRHI::sTexHandlesVerify = false;
            LLRHI::sTexHandlesAuto = false;
            LL_INFOS("Vulkan") << "texture numbers (auto): " << world_checks << " comparisons over " << sAutoFrames
                               << " frames of the world, no mismatch: on (research/197 1a)" << LL_ENDL;
            return true;
        }
        return false;
    }
} // namespace

    bool texref_frame()
    {
        const bool by_auto = LLRHI::sTexHandlesAuto && sSeenOn && texref_auto();
        const bool on = LLRHI::sTexHandles;
        const bool verify = on && LLRHI::sTexHandlesVerify;
        if (on != sSeenOn || verify != sSeenVerify)
        {
            if (sSeenOn && !by_auto && LLRHI::sTexHandlesAuto)
            {
                LLRHI::sTexHandlesAuto = false;     // a cue changed a switch: the auto mode is over
            }
            sTexRefTotals = TexRefCounts();
            sAutoFrames = 0;
            sAutoChecksFrom = 0;
            sSeenOn = on;
            sSeenVerify = verify;
            LL_INFOS("Vulkan") << "texture numbers " << (!on ? "off" : verify ? (LLRHI::sTexHandlesAuto ? "auto (verify first)" : "verify") : "on")
                               << " (research/197 1a)" << LL_ENDL;
        }
        sTexRefTake = on && !verify;
        sTexRefVerify = verify;
        return on;
    }

    std::string texref_report(U32 frames)
    {
        const TexRefCounts& c = sTexRefCounts;
        if (!frames || !(sSeenOn || c.created || c.retired))
        {
            return std::string();
        }
        std::ostringstream out;
        out << ", texture numbers [" << (!sSeenOn ? "off" : sSeenVerify ? (LLRHI::sTexHandlesAuto ? "auto verify" : "verify") : "on")
            << ", live " << (sTexRefs.size() - 1 - sTexRefFree.size()) << " made " << c.created << " new images " << c.replaced
            << " retired " << c.retired << ", hits " << c.hits / frames << " publishes " << c.publishes / frames << " clears "
            << c.clears / frames;
        if (sSeenVerify)
        {
            out << " (verify checked " << c.checked / frames << " mismatches " << c.mismatches << " breaks " << c.breaks << "; viewer checked "
                << c.viewer_checked / frames << " other image " << c.image_mismatches << " unnumbered " << c.unnumbered / frames
                << " sampler table differs " << c.sampler_diffs / frames << ")";
        }
        out << "]";
        return out.str();
    }

    void texref_reset_counters()
    {
        sTexRefCounts = TexRefCounts();
    }
namespace
{
    // ---- LLRHI::Textures' backend ----
    LLRHI::TexRef native_texref_create(const LLRHI::TextureDesc& desc)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return LLRHI::TexRef{ texref_create(desc.image) };
    }

    void native_texref_replace(LLRHI::TexRef ref, U32 image)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        texref_replace(ref.id, image);
    }

    void native_texref_retire(LLRHI::TexRef ref)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        texref_retire(ref.id);
    }

    void native_texref_anisotropy(F32 level)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        sAnisotropy = level;
    }

    // Verify, from the viewer's direct draws: the number names the image the
    // texture has now (its GL name). False: it does not (the draw goes by the
    // GL name). The sampler table is compared too, but only counted: stage 1
    // samples with what the texture has.
    bool native_texref_verify(LLRHI::TexRef ref, U32 image, U16 sampler)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        static U32 logged = 0;
        ++sTexRefCounts.viewer_checked;
        ++sTexRefTotals.viewer_checked;
        if (!ref.id)
        {
            // Made before the backend (or every number taken): it goes by name.
            ++sTexRefCounts.unnumbered;
            ++sTexRefTotals.unnumbered;
            return false;
        }
        const TexRefEntry* e = texref_entry(ref.id);
        if (!e || e->image != image)
        {
            ++sTexRefCounts.image_mismatches;
            ++sTexRefTotals.image_mismatches;
            if (logged < 8)
            {
                ++logged;
                LL_WARNS("Vulkan") << "texture number verify: number " << ref.id << (e ? "" : " (not live)") << " names image "
                                   << (e ? e->image : 0) << ", the texture's GL name is " << image << LL_ENDL;
            }
            return false;
        }
        const Texture* tex = sTextures.find(image);
        if (tex && tex->target == GL_TEXTURE_2D)
        {
            const SamplerParams p = sampler_params(sampler);
            if (tex->min_filter != p.min_filter || tex->mag_filter != p.mag_filter || tex->wrap_s != p.wrap || tex->wrap_t != p.wrap)
            {
                ++sTexRefCounts.sampler_diffs;
            }
        }
        return true;
    }
} // namespace

    void install_texref()
    {
        LLRHI::Textures::Backend backend;
        backend.create = &native_texref_create;
        backend.replaceImage = &native_texref_replace;
        backend.retire = &native_texref_retire;
        backend.setAnisotropy = &native_texref_anisotropy;
        backend.verify = &native_texref_verify;
        LLRHI::Textures::install(backend);
        LL_INFOS("VKGL") << "texture numbers: " << (!LLRHI::sTexHandles ? "off" : LLRHI::sTexHandlesAuto ? "auto" : LLRHI::sTexHandlesVerify ? "verify" : "on")
                         << " (LLRHI_TEX_HANDLES, research/197 1a)" << LL_ENDL;
    }
} // namespace llvkgl

#endif // LL_GL_FUNC_POINTER
