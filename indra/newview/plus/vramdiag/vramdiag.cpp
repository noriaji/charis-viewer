/**
 * @file vramdiag.cpp
 * @brief A census of the images over 8 MB, for an overrun of the driver's VRAM budget (research/354 vram-stall-diag).
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

#include "vramdiag.h"

#include "llimagegl.h"
#include "llrhi.h"
#include "lltimer.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "plus/texprio/texprio.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace plus::vramdiag
{
    const bool sOn = LLRHI::envSet("LLRHI_VRAM_DIAG");
}

namespace
{
    constexpr F64 REPORT_SECONDS = 10.0;
    constexpr U64 MB = 1024ull * 1024ull;
    constexpr U64 BIG_BYTES = 8 * MB;       // the device's line for memory of an image's own (llvkdevice.cpp TEXTURE_POOL_LARGEST_CLASS)
    constexpr size_t TOP = 5;

    // What a texture over 8 MB is, by the viewer's own rules for BC7 (rhi_compressible in llviewertexture.cpp), most
    // specific first.
    enum Origin : U32
    {
        OR_SELF_BAKE = 0,   // the agent's own bake (boost avatar baked self, avatar self)
        OR_OTHER_BAKE,      // another avatar's bake (a server or host bake, boost avatar baked or avatar)
        OR_BOOST,           // any other boost: high, selected, sculpted, bump, HUD, UI, map...
        OR_LOCAL,           // a local file
        OR_SCULPT_LIGHT,    // a sculpt map or a projector light
        OR_NOT_LOD,         // not a LOD texture
        OR_ELIGIBLE,        // the viewer lets the backend store it BC7
        OR_COUNT
    };
    const char* const ORIGIN_NAMES[OR_COUNT] = { "own bake", "other avatars' bakes", "boosted (not a bake)", "local file",
                                                 "sculpt map or projector light", "not a LOD texture",
                                                 "eligible for BC7 (LOD texture, no boost)" };

    constexpr U32 DISCARDS = 4;
    const char* const DISCARD_NAMES[DISCARDS] = { "discard 0 (full size)", "discard 1", "discard 2", "discard 3 and up" };
    constexpr U32 SIZES = 4;
    const char* const SIZE_NAMES[SIZES] = { "longest side 1024 and under", "2048", "4096", "8192 and up" };

    Origin origin_of(LLViewerFetchedTexture& tex)
    {
        const S32 boost = tex.getBoostLevel();
        const FTType ft = tex.getFTType();
        if (boost == LLGLTexture::BOOST_AVATAR_BAKED_SELF || boost == LLGLTexture::BOOST_AVATAR_SELF)
        {
            return OR_SELF_BAKE;
        }
        if (ft == FTT_SERVER_BAKE || ft == FTT_HOST_BAKE || boost == LLGLTexture::BOOST_AVATAR_BAKED || boost == LLGLTexture::BOOST_AVATAR)
        {
            return OR_OTHER_BAKE;
        }
        if (boost != LLGLTexture::BOOST_NONE)
        {
            return OR_BOOST;
        }
        if (ft == FTT_LOCAL_FILE)
        {
            return OR_LOCAL;
        }
        if (tex.forSculpt() || tex.getNumVolumes(LLRender::SCULPT_TEX) > 0 || tex.getNumVolumes(LLRender::LIGHT_TEX) > 0)
        {
            return OR_SCULPT_LIGHT;
        }
        if (tex.getType() != LLViewerTexture::LOD_TEXTURE)
        {
            return OR_NOT_LOD;
        }
        return OR_ELIGIBLE;
    }

    U32 size_of(S32 width, S32 height)
    {
        const S32 side = llmax(width, height);
        return side <= 1024 ? 0 : side <= 2048 ? 1 : side <= 4096 ? 2 : 3;
    }

    // How an image over 8 MB is stored.
    enum class Store : U8
    {
        Bc7,
        Plain,
        PlainScaled     // plain, and its discard level is over 0 (scaled down from the full size)
    };

    // Images over 8 MB, summed: how many are stored BC7, how many plain, and of the plain ones how many were scaled down.
    struct Sum
    {
        U32 count = 0;
        U64 bytes = 0;
        U32 bc7 = 0;
        U64 bc7_bytes = 0;
        U32 plain = 0;
        U64 plain_bytes = 0;
        U32 scaled = 0;         // plain, and discard level over 0
        U64 scaled_bytes = 0;
        void add(U64 b, Store store)
        {
            ++count;
            bytes += b;
            if (store == Store::Bc7)
            {
                ++bc7;
                bc7_bytes += b;
            }
            else
            {
                ++plain;
                plain_bytes += b;
                if (store == Store::PlainScaled)
                {
                    ++scaled;
                    scaled_bytes += b;
                }
            }
        }
    };

    struct Top
    {
        U64 bytes;
        LLViewerFetchedTexture* tex;
        Origin origin;
        bool bc7;
    };

    // What the log shows of a texture's UUID: not the agent's own bake (it ties the log to the person who sends it), and of
    // the others the first 8 characters only, as the texture census (LLRHI_TEX_CENSUS) does (a texture the agent wears
    // cannot be told from here, so none is given whole).
    std::string shown_id(const Top& t)
    {
        return t.origin == OR_SELF_BAKE ? std::string("(own bake)") : t.tex->getID().asString().substr(0, 8);
    }

    void put_sum(std::ostream& out, const Sum& s)
    {
        out << s.count << " images " << (s.bytes / MB) << " MB; BC7 " << s.bc7 << " (" << (s.bc7_bytes / MB) << " MB), plain " << s.plain
            << " (" << (s.plain_bytes / MB) << " MB), of them scaled down " << s.scaled << " (" << (s.scaled_bytes / MB) << " MB)";
    }

    void put_table(std::ostream& out, const char* title, const char* const* names, const Sum* sums, U32 count)
    {
        out << "\n  " << title << ":";
        for (U32 i = 0; i < count; ++i)
        {
            if (sums[i].count)
            {
                out << "\n    " << names[i] << ": ";
                put_sum(out, sums[i]);
            }
        }
    }

    void census(const plus::vramdiag::Budget& budget)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        LLTimer took;

        // The textures of the list, their images' bytes and which are stored BC7 (the texture census's two lookups).
        // Kept from one message to the next (cleared, not freed): a scan every 10 s does not allocate them again.
        static std::vector<LLViewerFetchedTexture*> textures;
        static std::vector<U32> names;
        static std::unordered_set<const LLImageGL*> listed;
        static std::vector<U64> bytes;
        static std::unique_ptr<bool[]> compressed;
        static size_t compressed_size = 0;
        static std::vector<U32> other_names;
        static std::vector<U64> other;
        textures.clear();
        names.clear();
        listed.clear();
        textures.reserve(gTextureList.getNumImages());
        names.reserve(gTextureList.getNumImages());
        for (const auto& entry : gTextureList)
        {
            LLViewerFetchedTexture* tex = entry.get();
            if (!tex)
            {
                continue;
            }
            const LLImageGL* gl = tex->getGLTexture();
            textures.push_back(tex);
            names.push_back(gl ? gl->getTexName() : 0);
            if (gl)
            {
                listed.insert(gl);
            }
        }
        const bool known = LLRHI::sTextureImageBytes != nullptr;
        bytes.assign(names.size(), 0);
        if (compressed_size < names.size())
        {
            compressed_size = names.size() + names.size() / 4 + 1;
            compressed.reset(new bool[compressed_size]);
        }
        std::fill(compressed.get(), compressed.get() + compressed_size, false);
        if (known && !names.empty())
        {
            LLRHI::sTextureImageBytes(names.data(), (U32)names.size(), bytes.data());
            if (LLRHI::sTextureCompressed)
            {
                LLRHI::sTextureCompressed(names.data(), (U32)names.size(), compressed.get());
            }
        }

        U64 list_bytes = 0, bc7_all_bytes = 0;
        U32 bc7_all = 0;
        Sum big, origins[OR_COUNT], discards[DISCARDS], tiers[plus::texprio::TIER_COUNT], sizes[SIZES];
        static std::vector<Top> top;
        top.clear();
        for (size_t i = 0; known && i < textures.size(); ++i)
        {
            list_bytes += bytes[i];
            if (compressed[i])
            {
                ++bc7_all;
                bc7_all_bytes += bytes[i];
            }
            if (bytes[i] <= BIG_BYTES)
            {
                continue;
            }
            LLViewerFetchedTexture* tex = textures[i];
            const LLImageGL* gl = tex->getGLTexture();
            if (!gl)
            {
                continue;
            }
            const Origin origin = origin_of(*tex);
            const Store store = compressed[i] ? Store::Bc7 : gl->getDiscardLevel() > 0 ? Store::PlainScaled : Store::Plain;
            const U32 tier = llmin((U32)tex->mRhiViewTier, (U32)plus::texprio::TIER_COUNT - 1);
            big.add(bytes[i], store);
            origins[origin].add(bytes[i], store);
            discards[llclamp(gl->getDiscardLevel(), 0, (S32)DISCARDS - 1)].add(bytes[i], store);
            tiers[tier].add(bytes[i], store);
            sizes[size_of(gl->getWidth(), gl->getHeight())].add(bytes[i], store);
            top.push_back({ bytes[i], tex, origin, store == Store::Bc7 });
        }

        // Images over 8 MB that are not in the list (render targets made through GL, dynamic textures, bakes made here).
        U32 other_count = 0;
        U64 other_bytes = 0;
        if (known)
        {
            other_names.clear();
            for (const LLImageGL* gl : LLImageGL::sImageList)
            {
                if (gl && gl->getHasGLTexture() && !listed.count(gl))
                {
                    other_names.push_back(gl->getTexName());
                }
            }
            other.assign(other_names.size(), 0);
            if (!other_names.empty())
            {
                LLRHI::sTextureImageBytes(other_names.data(), (U32)other_names.size(), other.data());
            }
            for (U64 b : other)
            {
                if (b > BIG_BYTES)
                {
                    ++other_count;
                    other_bytes += b;
                }
            }
        }

        std::partial_sort(top.begin(), top.begin() + llmin(TOP, top.size()), top.end(),
                          [](const Top& a, const Top& b) { return a.bytes > b.bytes; });
        top.resize(llmin(TOP, top.size()));

        std::ostringstream out;
        out << "VRAM diag textures (research/354, LLRHI_VRAM_DIAG): " << textures.size() << " textures in the list";
        if (!known)
        {
            out << ", the backend gives no image sizes (not Vulkan): nothing to count";
        }
        else
        {
            out << ", " << (list_bytes / MB) << " MB; images over 8 MB: ";
            put_sum(out, big);
            out << "; stored BC7 at any size " << bc7_all << " (" << (bc7_all_bytes / MB) << " MB); not in the list over 8 MB: "
                << other_count << " images " << (other_bytes / MB) << " MB";
        }
        out << "; the viewer counts " << llformat("%.0f", budget.used_mb) << " of its limit " << llformat("%.0f", budget.limit_mb) << " MB";
        if (budget.real)
        {
            out << ", VRAM " << (budget.usage / MB) << " of " << (budget.budget / MB) << " MB (driver budget)";
        }
        out << ", discard bias " << LLViewerTexture::sDesiredDiscardBias << ", texture pressure " << LLViewerTexture::sRhiTexPressure
            << ", BC7 encoder " << (LLRHI::sTexBCN ? "on" : "off") << "; " << llformat("%.1f", took.getElapsedTimeF32() * 1000.f)
            << " ms to count";
        if (known)
        {
            put_table(out, "over 8 MB by origin (the viewer's BC7 rules)", ORIGIN_NAMES, origins, OR_COUNT);
            put_table(out, "over 8 MB by discard level now", DISCARD_NAMES, discards, DISCARDS);
            // The tier names come from texprio, so they are put one by one.
            out << "\n  over 8 MB by view tier (as of each texture's last visit):";
            for (U32 t = 0; t < plus::texprio::TIER_COUNT; ++t)
            {
                if (tiers[t].count)
                {
                    out << "\n    " << plus::texprio::tierName((U8)t) << ": ";
                    put_sum(out, tiers[t]);
                }
            }
            put_table(out, "over 8 MB by size now", SIZE_NAMES, sizes, SIZES);
            // BC7 is 8 bits a texel against 32: a quarter of the bytes, whole mip chain.
            out << "\n  BC7 would save at most (3/4 of the plain bytes, an estimate): " << (origins[OR_ELIGIBLE].plain_bytes / MB * 3 / 4)
                << " MB if every eligible plain image went BC7, " << (big.plain_bytes / MB * 3 / 4) << " MB if every plain image over 8 MB did"
                << " (bakes, boosted ones too)";
            if (LLRHI::sTexBCNReport)
            {
                out << "\n  " << LLRHI::sTexBCNReport();
            }
            out << "\n  largest " << top.size() << " (UUID start, size now of the full size, MB, discard level, origin, store, view tier):";
            for (const Top& t : top)
            {
                const LLImageGL* gl = t.tex->getGLTexture();
                out << "\n    " << shown_id(t) << " " << gl->getWidth() << "x" << gl->getHeight() << " of "
                    << t.tex->getFullWidth() << "x" << t.tex->getFullHeight() << ", " << (t.bytes / MB) << " MB, discard "
                    << gl->getDiscardLevel() << ", " << ORIGIN_NAMES[t.origin] << ", " << (t.bc7 ? "BC7" : "plain") << ", "
                    << plus::texprio::tierName(llmin(t.tex->mRhiViewTier, (U8)(plus::texprio::TIER_COUNT - 1)));
            }
        }
        LL_INFOS("VKPerf") << out.str() << LL_ENDL;
    }
}

void plus::vramdiag::frame(const Budget& budget)
{
    static F64 next_seconds = 0.0;
    const F64 now = LLTimer::getTotalSeconds();
    if (next_seconds == 0.0)
    {
        next_seconds = now + REPORT_SECONDS;
        return;
    }
    if (now < next_seconds)
    {
        return;
    }
    next_seconds = now + REPORT_SECONDS;
    census(budget);
}
