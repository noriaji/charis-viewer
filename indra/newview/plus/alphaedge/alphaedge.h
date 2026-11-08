/**
 * @file alphaedge.h
 * @brief The colour of a texture's transparent texels pushed out from the opaque ones when it
 *        is decoded (PlusAlphaEdgeFix, research/alpha-mask-edge-2026-10-26.md (d)).
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

#ifndef PLUS_ALPHAEDGE_H
#define PLUS_ALPHAEDGE_H

#include "stdtypes.h"

#include <vector>

class LLFace;
class LLImageRaw;
class LLUUID;
class LLViewerObject;

// SL's textures are straight alpha: the colour of a transparent texel is whatever the maker's
// program left there, most often black (the default moon's is grey 0x55). Bilinear filtering
// and the mipmaps mix that colour into the edge of the opaque part: an alpha mask shows a black
// rim (it draws the mixed colour opaque), a blended edge (the moon) a dark fringe.
//
// When a texture used for colour has been decoded (the decode threads, before the main thread
// sees it, so before the alpha analysis and the BC7 compression), the colour of its texels of
// low alpha is filled from the opaque ones round them: the next opaque texels' where there are
// some, else a pyramid's average of the opaque part (push-pull). The alpha is not changed, so
// what is drawn where and how much is the same; only the colour the filters mix in at the edges.
// Nothing at all per frame.
//
// Only the textures the scene uses for colour whose alpha is coverage: an object's diffuse with
// no material or a material of alpha mode blend or mask, a PBR base colour of alpha mode blend or
// mask, the sky's sun and moon. Where the alpha is not coverage the colour under alpha 0 is drawn
// and must stay: a normal or specular map (gloss), a diffuse of alpha mode none (the alpha is
// ignored) or emissive mask (the alpha says where it glows; a sign's picture is all over), a PBR
// base colour of alpha mode opaque (fix-r5, research/bugloop-log.md round 3). A texture that has
// ever been bound as one of those is left alone; one whose face's material has not arrived yet
// is not counted until it has (a decode before that is not filled; the next one is).
namespace plus::alphaedge
{
    // LLViewerTexture::addFace(): the texture id is bound to a face on that channel
    // (LLRender::eTexIndex). Main thread. Also reads PlusAlphaEdgeFix (cached) for the decoders.
    void noteUse(const LLUUID& id, U32 channel, const LLFace* face);

    // LLVOVolume::setTEMaterialParams (a legacy material arrived or changed after the textures
    // were bound) and the PBR faces' rebuild (a GLTF material or override can change the alpha
    // mode of the same base colour texture): the use of te's diffuse and PBR base colour again,
    // by the material it has now. Main thread.
    void noteMaterial(const LLViewerObject* object, U8 te);

    // lltexturefetch.cpp, a decode done (the decode thread): fills the colour of raw's texels of
    // low alpha if the switch is on and the texture is known as colour only.
    void onDecoded(const LLUUID& id, LLImageRaw* raw);

    // ---- alphaedge use rule (fix-r5) ----
    // Plain values only (tools/selftest sim-alphaedge compiles this block on its own).

    // The texels at or above this alpha are opaque (their colour is the source and is kept);
    // below it the colour is filled, wholly at 0 and less towards it ((a / SOLID_ALPHA)^2 of
    // the texel's own colour is kept).
    constexpr U8 SOLID_ALPHA = 32;

    namespace rule
    {
        // What a texture has been bound to faces as (recorded by texture id, the bits or'ed)
        constexpr U8 USE_COLOUR = 1;    // colour whose alpha is coverage: may be filled
        constexpr U8 USE_OTHER = 2;     // anything else: never filled

        // The slot a texture is bound to
        enum Slot : U8
        {
            SLOT_DIFFUSE,       // LLRender::DIFFUSE_MAP
            SLOT_BASECOLOR,     // LLRender::BASECOLOR_MAP
            SLOT_SKY_NEXT,      // LLRender::ALTERNATE_DIFFUSE_MAP on the sky (the next sun or moon)
            SLOT_OTHER_MAP,     // any other channel (normal, specular, metallic-roughness, emissive)
        };

        // LLMaterial::eDiffuseAlphaMode and LLGLTFMaterial::AlphaMode (static_assert'ed in
        // alphaedge.cpp)
        constexpr U8 DIFFUSE_NONE = 0;
        constexpr U8 DIFFUSE_BLEND = 1;
        constexpr U8 DIFFUSE_MASK = 2;
        constexpr U8 DIFFUSE_EMISSIVE = 3;
        constexpr U8 GLTF_OPAQUE = 0;
        constexpr U8 GLTF_BLEND = 1;
        constexpr U8 GLTF_MASK = 2;

        // What the face's texture entry says about its material
        struct Face
        {
            bool te = false;                // it has one (objects; the sky, water, terrain may not)
            bool pbr = false;               // a GLTF render material
            U8 gltf_alpha_mode = GLTF_OPAQUE;
            bool material_id = false;       // a legacy material id
            bool material_params = false;   // its LLMaterial (arrives after the id)
            U8 diffuse_alpha_mode = DIFFUSE_BLEND;
        };

        // The use to record for a texture bound to slot on face; 0: nothing to record (not
        // drawn there, or not known yet)
        inline U8 useOf(Slot slot, const Face& face)
        {
            switch (slot)
            {
            case SLOT_SKY_NEXT:
                return USE_COLOUR;
            case SLOT_DIFFUSE:
                if (!face.te)
                {
                    return USE_COLOUR;
                }
                if (face.pbr)
                {
                    return 0;   // the legacy diffuse is not drawn on a PBR face
                }
                if (face.material_params)
                {
                    switch (face.diffuse_alpha_mode)
                    {
                    case DIFFUSE_BLEND:
                    case DIFFUSE_MASK:
                        return USE_COLOUR;
                    case DIFFUSE_NONE:
                    case DIFFUSE_EMISSIVE:
                        return USE_OTHER;
                    default:
                        return 0;
                    }
                }
                // No material: the alpha is coverage (blend, or the automatic mask). A material
                // id whose parameters have not arrived: not known yet.
                return face.material_id ? 0 : USE_COLOUR;
            case SLOT_BASECOLOR:
                if (!face.te || !face.pbr)
                {
                    return 0;
                }
                switch (face.gltf_alpha_mode)
                {
                case GLTF_BLEND:
                case GLTF_MASK:
                    return USE_COLOUR;
                case GLTF_OPAQUE:
                    return USE_OTHER;
                default:
                    return 0;
                }
            default:
                return USE_OTHER;
            }
        }

        // The record after one more use (a use other than colour is never forgotten)
        inline U8 merge(U8 recorded, U8 use)
        {
            return (U8)(recorded | use);
        }

        // Whether a texture so recorded is filled
        inline bool fills(U8 recorded)
        {
            return recorded == USE_COLOUR;
        }
    }

    // The work memory of one decode thread, kept between images (the larger images free it).
    struct Scratch
    {
        struct Cell
        {
            U8 c[3];    // the opaque texels' colour, weighted by their alpha
            U8 pad;
            F32 w;      // the sum of their alpha (0: none under this cell)
        };
        std::vector<Cell> cells;    // every level of the pyramid from the half size up to 1 x 1
        std::vector<size_t> offset; // where each level starts
        std::vector<S32> width;
        std::vector<S32> height;
    };
    // ---- end of the alphaedge use rule ----

    // The fill itself: data is width x height texels of components (2: luminance + alpha,
    // 4: RGBA; others are left) bytes. false: nothing changed (no texel of low alpha, or none
    // opaque). Any thread.
    bool fillTransparent(U8* data, S32 width, S32 height, S32 components, Scratch& scratch);
}

#endif // PLUS_ALPHAEDGE_H
