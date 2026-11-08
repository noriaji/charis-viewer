/**
 * @file llvowlsky.cpp
 * @brief LLVOWLSky class implementation
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "pipeline.h"

#include "llvowlsky.h"
#include "llsky.h"
#include "lldrawpoolwlsky.h"
#include "llface.h"
#include "llviewercontrol.h"
#include "llenvironment.h"
#include "llsettingssky.h"
#include "plus/realstars/realstars.h" // <plus> </plus>
#include "plus/realstars/realstarcat.h" // <plus> the line quad's corners </plus>
#include "llimagegl.h" // <plus> research/288: the constellation pictures' texture </plus>

constexpr U32 SKY_DETAIL = 18; // Any lower and there will be artifacts

// <plus> plus/realstars: the tint of the constellation lines and pictures (research/288): bluish, not
// white (the tone curve whitens a pale blue). The alpha carries each one's own data.
static LLColor4U constellation_tint(U8 alpha)
{
    return LLColor4U(92, 154, 255, alpha);
}
// </plus>

inline U32 LLVOWLSky::getNumStacks(void)
{
    return SKY_DETAIL;
}

inline U32 LLVOWLSky::getNumSlices(void)
{
    return 2 * SKY_DETAIL;
}

inline U32 LLVOWLSky::getStripsNumVerts(void)
{
    return (getNumStacks() - 1) * getNumSlices();
}

inline U32 LLVOWLSky::getStripsNumIndices(void)
{
    return 2 * ((getNumStacks() - 2) * (getNumSlices() + 1)) + 1 ;
}

// <plus> plus/starfield: const, and the star field has as many as its catalogue (was: static, 1000)
inline U32 LLVOWLSky::getStarsNumVerts(void) const
{
    if (mStarMode > plus::starfield::MODE_OFFICIAL)
    {
        return (U32)mStarVertices.size();
    }
    return 1000;
}
// </plus>

inline U32 LLVOWLSky::getStarsNumIndices(void)
{
    return 1000;
}

LLVOWLSky::LLVOWLSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
    : LLStaticViewerObject(id, pcode, regionp, true)
{
    initStars();
}

void LLVOWLSky::idleUpdate(LLAgent &agent, const F64 &time)
{

}

bool LLVOWLSky::isActive(void) const
{
    return false;
}

LLDrawable * LLVOWLSky::createDrawable(LLPipeline * pipeline)
{
    pipeline->allocDrawable(this);

    //LLDrawPoolWLSky *poolp = static_cast<LLDrawPoolWLSky *>(
        gPipeline.getPool(LLDrawPool::POOL_WL_SKY);

    mDrawable->setRenderType(LLPipeline::RENDER_TYPE_WL_SKY);

    return mDrawable;
}

// a tiny helper function for controlling the sky dome tesselation.
inline F32 calcPhi(const U32 &i, const F32 &reciprocal_num_stacks)
{
    // Calc: PI/8 * 1-((1-t^4)*(1-t^4))  { 0<t<1 }
    // Demos: \pi/8*\left(1-((1-x^{4})*(1-x^{4}))\right)\ \left\{0<x\le1\right\}

    // i should range from [0..SKY_STACKS] so t will range from [0.f .. 1.f]
    F32 t = float(i) * reciprocal_num_stacks; //SL-16127: remove: / float(getNumStacks());

    // ^4 the parameter of the tesselation to bias things toward 0 (the dome's apex)
    t *= t;
    t *= t;

    // invert and square the parameter of the tesselation to bias things toward 1 (the horizon)
    t = 1.f - t;
    t = t*t;
    t = 1.f - t;

    return (F_PI / 8.f) * t;
}

void LLVOWLSky::resetVertexBuffers()
{
    mStripsVerts.clear();
    mStarsVerts = nullptr;
    mMilkyBox = nullptr; // <plus> research/224 </plus>
    mFsSkyVerts = nullptr;

    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
}

void LLVOWLSky::cleanupGL()
{
    mStripsVerts.clear();
    mStarsVerts = nullptr;
    mFsSkyVerts = nullptr;
    // <plus> research/224: the Milky Way's box and picture are GL objects; asked for again by
    // refreshStars() (a bake is a second) once the GL is back
    mMilkyBox = nullptr;
    if (mMilky)
    {
        mMilky->release();
    }
    // research/288: the constellation pictures, made again when next drawn
    releaseStarArt();
    mArtTextureFailed = false;
    // </plus>

    LLDrawPoolWLSky::cleanupGL();
}

void LLVOWLSky::restoreGL()
{
    LLDrawPoolWLSky::restoreGL();
    gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
}

bool LLVOWLSky::updateGeometry(LLDrawable * drawable)
{
    LL_PROFILE_ZONE_SCOPED;
    LLStrider<LLVector3>    vertices;
    LLStrider<LLVector2>    texCoords;
    LLStrider<U16>          indices;

    if (mFsSkyVerts.isNull())
    {
        mFsSkyVerts = new LLVertexBuffer(LLDrawPoolWLSky::ADV_ATMO_SKY_VERTEX_DATA_MASK);

        if (!mFsSkyVerts->allocateBuffer(4, 6))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer on full screen sky update" << LL_ENDL;
        }

        bool success = mFsSkyVerts->getVertexStrider(vertices)
                    && mFsSkyVerts->getTexCoord0Strider(texCoords)
                    && mFsSkyVerts->getIndexStrider(indices);

        if(!success)
        {
            LL_ERRS() << "Failed updating WindLight fullscreen sky geometry." << LL_ENDL;
        }

        *vertices++ = LLVector3(-1.0f, -1.0f, 0.0f);
        *vertices++ = LLVector3( 1.0f, -1.0f, 0.0f);
        *vertices++ = LLVector3(-1.0f,  1.0f, 0.0f);
        *vertices++ = LLVector3( 1.0f,  1.0f, 0.0f);

        *texCoords++ = LLVector2(0.0f, 0.0f);
        *texCoords++ = LLVector2(1.0f, 0.0f);
        *texCoords++ = LLVector2(0.0f, 1.0f);
        *texCoords++ = LLVector2(1.0f, 1.0f);

        *indices++ = 0;
        *indices++ = 1;
        *indices++ = 2;
        *indices++ = 1;
        *indices++ = 3;
        *indices++ = 2;

        mFsSkyVerts->unmapBuffer();
    }

    {
        const F32 dome_radius = LLEnvironment::instance().getCurrentSky()->getDomeRadius();

        const U32 max_buffer_bytes = gSavedSettings.getS32("RenderMaxVBOSize")*1024;
        const U32 data_mask = LLDrawPoolWLSky::SKY_VERTEX_DATA_MASK;
        const U32 max_verts = max_buffer_bytes / LLVertexBuffer::calcVertexSize(data_mask);

        const U32 total_stacks = getNumStacks();

        const U32 verts_per_stack = getNumSlices();

        // each seg has to have one more row of verts than it has stacks
        // then round down
        const U32 stacks_per_seg = (max_verts - verts_per_stack) / verts_per_stack;

        // round up to a whole number of segments
        const U32 strips_segments = (total_stacks+stacks_per_seg-1) / stacks_per_seg;

        mStripsVerts.resize(strips_segments, NULL);

#if RELEASE_SHOW_DEBUG
        LL_INFOS() << "WL Skydome strips in " << strips_segments << " batches." << LL_ENDL;

        LLTimer timer;
        timer.start();
#endif

        for (U32 i = 0; i < strips_segments ;++i)
        {
            LLVertexBuffer * segment = new LLVertexBuffer(LLDrawPoolWLSky::SKY_VERTEX_DATA_MASK);
            mStripsVerts[i] = segment;

            U32 num_stacks_this_seg = stacks_per_seg;
            if ((i == strips_segments - 1) && (total_stacks % stacks_per_seg) != 0)
            {
                // for the last buffer only allocate what we'll use
                num_stacks_this_seg = total_stacks % stacks_per_seg;
            }

            // figure out what range of the sky we're filling
            const U32 begin_stack = i * stacks_per_seg;
            const U32 end_stack = begin_stack + num_stacks_this_seg;
            llassert(end_stack <= total_stacks);

            const U32 num_verts_this_seg = verts_per_stack * (num_stacks_this_seg+1);
            llassert(num_verts_this_seg <= max_verts);

            const U32 num_indices_this_seg = 1+num_stacks_this_seg*(2+2*verts_per_stack);
            llassert(num_indices_this_seg * sizeof(U16) <= max_buffer_bytes);

            bool allocated = segment->allocateBuffer(num_verts_this_seg, num_indices_this_seg);
#if RELEASE_SHOW_WARNS
            if( !allocated )
            {
                LL_WARNS() << "Failed to allocate Vertex Buffer on update to "
                    << num_verts_this_seg << " vertices and "
                    << num_indices_this_seg << " indices" << LL_ENDL;
            }
#else
            (void) allocated;
#endif

            // lock the buffer
            bool success = segment->getVertexStrider(vertices)
                && segment->getTexCoord0Strider(texCoords)
                && segment->getIndexStrider(indices);

#if RELEASE_SHOW_DEBUG
            if(!success)
            {
                LL_ERRS() << "Failed updating WindLight sky geometry." << LL_ENDL;
            }
#else
            (void) success;
#endif

            // fill it
            buildStripsBuffer(begin_stack, end_stack, vertices, texCoords, indices, dome_radius, verts_per_stack, total_stacks);

            // and unlock the buffer
            segment->unmapBuffer();
        }

#if RELEASE_SHOW_DEBUG
        LL_INFOS() << "completed in " << llformat("%.2f", timer.getElapsedTimeF32().value()) << "seconds" << LL_ENDL;
#endif
    }

    updateStarColors();
    updateStarGeometry(drawable);

    LLPipeline::sCompiles++;

    return true;
}

void LLVOWLSky::drawStars(void)
{
    //  render the stars as a sphere centered at viewer camera
    if (mStarsVerts.notNull())
    {
        mStarsVerts->setBuffer();
        // <plus> plus/starfield: every star's six vertices (the official draws four of the six, so
        // two thirds of its stars)
        if (mStarMode > plus::starfield::MODE_OFFICIAL)
        {
            mStarsVerts->drawArrays(LLRender::TRIANGLES, 0, getStarsNumVerts() * 6);
            return;
        }
        // </plus>
        mStarsVerts->drawArrays(LLRender::TRIANGLES, 0, getStarsNumVerts()*4);
    }
}

// <plus> plus/starfield (research/176)
void LLVOWLSky::drawStarGlow(void)
{
    if (mStarsVerts.notNull() && mStarMode >= plus::starfield::MODE_MILKY_WAY && mGlowCount > 0)
    {
        mStarsVerts->setBuffer();
        mStarsVerts->drawArrays(LLRender::TRIANGLES, getStarsNumVerts() * 6, mGlowCount * 6);
    }
}

bool LLVOWLSky::prepareMilkyBox(void)
{
    // research/224: a box round the camera (the star field's matrix turns it with the stars); the
    // fragment shader takes the direction of each pixel from the box's position.
    if (mMilkyBox.isNull())
    {
        const F32 r = LLEnvironment::instance().getCurrentSky()->getDomeRadius();
        const LLVector3 corner[8] =
        {
            LLVector3(-r, -r, -r), LLVector3(r, -r, -r), LLVector3(r, r, -r), LLVector3(-r, r, -r),
            LLVector3(-r, -r, r), LLVector3(r, -r, r), LLVector3(r, r, r), LLVector3(-r, r, r)
        };
        static const U8 triangle[36] =
        {
            0, 2, 1,  0, 3, 2,      // -z
            4, 5, 6,  4, 6, 7,      // +z
            0, 1, 5,  0, 5, 4,      // -y
            2, 3, 7,  2, 7, 6,      // +y
            0, 4, 7,  0, 7, 3,      // -x
            1, 2, 6,  1, 6, 5       // +x
        };
        LLPointer<LLVertexBuffer> box = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        LLStrider<LLVector3> verticesp;
        if (!box->allocateBuffer(36, 0) || !box->getVertexStrider(verticesp))
        {
            LL_WARNS() << "Failed to make the Milky Way's box" << LL_ENDL;
            return false;
        }
        for (const U8 index : triangle)
        {
            *(verticesp++) = corner[index];
        }
        box->unmapBuffer();
        mMilkyBox = box;
    }
    return true;
}

void LLVOWLSky::drawMilkyBox(void)
{
    if (mMilkyBox.isNull())
    {
        return;
    }
    mMilkyBox->setBuffer();
    mMilkyBox->drawArrays(LLRender::TRIANGLES, 0, 36);
}

void LLVOWLSky::refreshMilky()
{
    if (wantsMilkyPicture())
    {
        if (!mMilky)
        {
            mMilky = std::make_unique<plus::starfield::MilkyBaker>();
        }
        LLVector3 pole;
        LLVector3 centre;
        if (mStarReal)
        {
            plus::realstars::galacticPlane(pole, centre);
        }
        else
        {
            plus::starfield::madeUpPlane(pole, centre);
        }
        mMilky->want(pole, centre);
        mMilky->poll();
    }
    else if (mMilky)
    {
        mMilky->release();
        mMilky->poll();     // a bake that was in flight ends here, thrown away
    }
}

void LLVOWLSky::drawStarLines(void)
{
    if (mStarsVerts.notNull() && mStarReal && mLineCount > 0)
    {
        mStarsVerts->setBuffer();
        mStarsVerts->drawArrays(LLRender::TRIANGLES, (getStarsNumVerts() + mGlowCount) * 6, mLineCount * 6);
    }
}

void LLVOWLSky::drawStarArt(void)
{
    if (mStarsVerts.notNull() && mStarReal && mArtCount > 0)
    {
        mStarsVerts->setBuffer();
        mStarsVerts->drawArrays(LLRender::TRIANGLES, (getStarsNumVerts() + mGlowCount + mLineCount) * 6, mArtCount * 6);
    }
}

U32 LLVOWLSky::prepareStarArt(void)
{
    // research/288 section 3.2: one 2D array of the pictures (a layer each, 512 x 512, 8 mips),
    // its blocks uploaded as starart-tex.bin has them (BC4: nothing is decoded or converted here).
    // The file (about 15 MB) is read and checked on a thread (ArtPicturesReader) from the first
    // frame the pictures are wanted; the frame it is in, the main thread uploads it (8 copies)
    // with unit 0, as LLCubeMapArray::allocate does. Bound by the stars' pass.
    if (mArtTexture || mArtTextureFailed || !mStarReal || mArtCount == 0)
    {
        return mArtTexture;
    }
    namespace rs = plus::realstars;
    if (!mArtReader)
    {
        mArtReader = std::make_unique<rs::ArtPicturesReader>(rs::artLayersNeeded(mStarArt));
    }
    const rs::ArtPicturesReader::State read = mArtReader->poll();
    if (read == rs::ArtPicturesReader::State::Reading)
    {
        return 0;   // not this frame
    }
    // the reader goes when this returns (its file's bytes with it)
    const std::unique_ptr<rs::ArtPicturesReader> reader = std::move(mArtReader);
    if (read == rs::ArtPicturesReader::State::Failed)
    {
        mArtTextureFailed = true;
        return 0;
    }
    const std::vector<U8>& bytes = reader->bytes();
    const U32 layers = reader->layers();
    if (!glTexImage3D || !glCompressedTexSubImage3D)
    {
        LL_WARNS("Plus") << "real stars: no glTexImage3D / glCompressedTexSubImage3D; no constellation pictures" << LL_ENDL;
        mArtTextureFailed = true;
        return 0;
    }
    U32 name = 0;
    LLImageGL::generateTextures(1, &name);
    LLTexUnit* unit = gGL.getTexUnit(0);
    unit->bindManual(LLTexUnit::TT_TEXTURE_2D_ARRAY, name, true);
    // The storage, then each mip's layers in one copy (starart-tex.bin keeps a mip's layers
    // together). An image that could not be made (out of memory, or too large) or a copy that could
    // not be recorded shows as a GL error (the backend's, as GL's): the texture is let go and not
    // tried again, so no plate is drawn from a white stand-in.
    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i)
    {
        // errors left by others are not this texture's (read away; a few at most)
    }
    bool made = true;
    for (U32 mip = 0; mip < rs::ART_TEX_MIPS && made; ++mip)
    {
        const GLsizei side = (GLsizei)rs::artTexSide(mip);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, mip, GL_COMPRESSED_RED_RGTC1, side, side, (GLsizei)layers, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        made = glGetError() == GL_NO_ERROR;
    }
    for (U32 mip = 0; mip < rs::ART_TEX_MIPS && made; ++mip)
    {
        const GLsizei side = (GLsizei)rs::artTexSide(mip);
        const size_t mip_bytes = (size_t)layers * rs::artTexMipBytes(mip);
        glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, mip, 0, 0, 0, side, side, (GLsizei)layers, GL_COMPRESSED_RED_RGTC1,
                                  (GLsizei)mip_bytes, bytes.data() + rs::artTexMipOffset(layers, mip));
        made = glGetError() == GL_NO_ERROR;
    }
    if (made)
    {
        // the levels the file has (the image's own chain goes on to 1 x 1), clamped at the edges, mips blended
        const LLRHI::TexParameter levels[2] = { { GL_TEXTURE_BASE_LEVEL, 0.f }, { GL_TEXTURE_MAX_LEVEL, (F32)(rs::ART_TEX_MIPS - 1) } };
        LLRHI::sTexParameters(GL_TEXTURE_2D_ARRAY, levels, 2);
        unit->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        unit->setTextureFilteringOption(LLTexUnit::TFO_TRILINEAR);
    }
    unit->unbind(LLTexUnit::TT_TEXTURE_2D_ARRAY);
    if (!made)
    {
        LL_WARNS("Plus") << "real stars: the constellation pictures' texture could not be made; none drawn" << LL_ENDL;
        LLImageGL::deleteTextures(1, &name);
        mArtTextureFailed = true;
        return 0;
    }
    mArtTexture = name;
    LL_INFOS("Plus") << "real stars: constellation pictures made, " << layers << " layers, "
                     << (bytes.size() >> 10) << " KB" << LL_ENDL;
    return mArtTexture;
}

void LLVOWLSky::dropStarArtIfUnwanted(void)
{
    // the texture (about 15 MB) and its read only while the pictures are asked for
    if (!(plus::realstars::artWanted() && mStarReal))
    {
        if (mArtTexture || mArtReader)
        {
            releaseStarArt();
        }
        mArtTextureFailed = false;  // switched on again, tried again
    }
}

void LLVOWLSky::releaseStarArt(void)
{
    mArtReader.reset();     // a read in flight is waited for (it is the file's alone)
    if (mArtTexture)
    {
        LLImageGL::deleteTextures(1, &mArtTexture);
        mArtTexture = 0;
    }
}

void LLVOWLSky::refreshStars(void)
{
    plus::realstars::update();
    dropStarArtIfUnwanted(); // research/288
    const S32 star_mode = plus::starfield::mode();
    // PlusSkyStarsDense and PlusSkyStarsMilkyWayHQ matter to the star field only (the official stars
    // are made at random: made again, they would jump); the baked picture is for PlusSkyStars 2 only
    const bool dense_changed = star_mode > plus::starfield::MODE_OFFICIAL && plus::starfield::dense() != mStarDense;
    if (star_mode == mStarMode && plus::realstars::wanted() == mStarRealAsked
        && !dense_changed && plus::starfield::bakedMilkyWay(star_mode) == mStarHQ)
    {
        // research/177 §2.7: PlusSkyStarsConstellationBoost changed: the colours only, the buffer
        // made again at the next rebuild (the catalogue is not read again)
        const F32 boost = plus::starfield::constellationBoost();
        if (boost != mStarBoost && mStarReal)
        {
            mStarBoost = boost;
            plus::starfield::boostedColors(mStarField, mStarBoost, mStarColors);
            mStarsVerts = nullptr;
            if (mDrawable.notNull())
            {
                gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
            }
        }
        refreshMilky();
        return;
    }
    // PlusSkyStars, PlusSkyStarsReal, PlusSkyStarsDense or PlusSkyStarsMilkyWayHQ changed: a new
    // catalogue, and the buffer made again at the next rebuild
    initStars();
    mStarsVerts = nullptr;
    if (mDrawable.notNull())
    {
        gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_ALL);
    }
    refreshMilky();
}

bool LLVOWLSky::updateStarFieldGeometry()
{
    const U32 stars = getStarsNumVerts();
    const U32 glows = mGlowCount;
    const U32 lines = mLineCount;
    const U32 arts = mArtCount;     // research/288
    const U32 total = (stars + glows + lines + arts) * 6;
    if (stars == 0 || mStarColors.size() < stars)
    {
        return false;
    }

    if (mStarsVerts.isNull())
    {
        mStarsVerts = new LLVertexBuffer(LLDrawPoolWLSky::STAR_VERTEX_DATA_MASK);
        if (!mStarsVerts->allocateBuffer(total, 0))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer for the star field: " << total << " vertices" << LL_ENDL;
            mStarsVerts = nullptr;
            return false;
        }
    }

    LLStrider<LLVector3> verticesp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;
    if (!(mStarsVerts->getVertexStrider(verticesp)
        && mStarsVerts->getColorStrider(colorsp)
        && mStarsVerts->getTexCoord0Strider(texcoordsp)))
    {
        LL_WARNS() << "Failed updating the star field's geometry." << LL_ENDL;
        mStarsVerts = nullptr;  // not drawn with what is in it; made again at the next rebuild
        return false;
    }

    // The two triangles of a quad on the corners (-1,-1) (1,-1) (1,1) (-1,1)
    static const LLVector2 corners[6] =
    {
        LLVector2(-1.f, -1.f), LLVector2(1.f, -1.f), LLVector2(1.f, 1.f),
        LLVector2(-1.f, -1.f), LLVector2(1.f, 1.f), LLVector2(-1.f, 1.f)
    };

    // A star is a point: the vertex shader spreads its quad in clip space
    // (starsV.glsl), so all six vertices are at its place.
    for (U32 i = 0; i < stars; ++i)
    {
        const LLColor4U color(mStarColors[i]);
        for (const LLVector2& corner : corners)
        {
            *(verticesp++) = mStarVertices[i];
            *(texcoordsp++) = corner;
            *(colorsp++) = color;
        }
    }

    // A patch of the Milky Way is a quad in the sky, as wide as its angular radius
    // (the Gaussian of the fragment shader is 0.7 of the half width at its 2 sigma).
    const F32 radius = mStarVertices.empty() ? 0.f : mStarVertices[0].length();
    for (U32 i = 0; i < glows; ++i)
    {
        const LLVector3 at = mStarField.glow_dir[i];
        LLVector3 left = at % LLVector3(0.f, 0.f, 1.f);
        if (left.lengthSquared() < 1e-6f)
        {
            left = at % LLVector3(1.f, 0.f, 0.f);
        }
        left.normVec();
        LLVector3 up = at % left;
        up.normVec();
        const F32 half = radius * mStarField.glow_radius[i] * 1.43f;
        const LLVector3 center = at * radius;
        const LLColor4U color(mStarField.glow_color[i]);
        for (const LLVector2& corner : corners)
        {
            *(verticesp++) = center + left * (corner.mV[VX] * half) + up * (corner.mV[VY] * half);
            *(texcoordsp++) = corner;
            *(colorsp++) = color;
        }
    }

    // A constellation line (plus/realstars, research/177 §2.6) is a thin quad that starsV.glsl
    // spreads to its width in pixels: each vertex sits at one end and carries the other end
    // (its right ascension and declination, J2000, in radians), and in its alpha the side of the
    // line it steps to as it sees the line from its own end and which end it is
    // (plus::realstars::LINE_CORNERS, realstarcat.h).
    for (U32 i = 0; i < lines; ++i)
    {
        const LLVector3& a = mStarField.line_a[i];
        const LLVector3& b = mStarField.line_b[i];
        const LLVector2 to_b(atan2f(b.mV[VY], b.mV[VX]), asinf(llclamp(b.mV[VZ], -1.f, 1.f)));
        const LLVector2 to_a(atan2f(a.mV[VY], a.mV[VX]), asinf(llclamp(a.mV[VZ], -1.f, 1.f)));
        for (const plus::realstars::LineCorner& corner : plus::realstars::LINE_CORNERS)
        {
            *(verticesp++) = (corner.atB ? b : a) * radius;
            *(texcoordsp++) = corner.atB ? to_a : to_b;
            *(colorsp++) = constellation_tint(plus::realstars::lineCornerAlpha(corner));
        }
    }

    // A constellation picture (research/288 section 3.2) is a flat plate: its four corners
    // (plus::realstars::ArtPlate, on the plane through the picture's three reference stars)
    // times the sky's radius, two triangles (ART_TRIANGLE_CORNERS). The vertex attributes:
    // position the corner, texcoord0 the place in the picture (ART_CORNER_UV: (0,0) top left ..
    // (0,1) bottom left), colour the tint of the lines and in its alpha the picture's layer in
    // the 2D array (0 .. 127, as the lines carry their code in it; starsV.glsl takes it back).
    for (U32 i = 0; i < arts; ++i)
    {
        const plus::realstars::ArtPlate& plate = mStarArt.plates[i];
        const LLColor4U color = constellation_tint((U8)plate.layer);
        for (const int corner : plus::realstars::ART_TRIANGLE_CORNERS)
        {
            const float* c = plate.corner[corner];
            *(verticesp++) = LLVector3(c[0], c[1], c[2]) * radius;
            *(texcoordsp++) = LLVector2(plus::realstars::ART_CORNER_UV[corner][0], plus::realstars::ART_CORNER_UV[corner][1]);
            *(colorsp++) = color;
        }
    }

    mStarsVerts->unmapBuffer();
    return true;
}
// </plus>

void LLVOWLSky::drawFsSky(void)
{
    if (mFsSkyVerts.isNull())
    {
        updateGeometry(mDrawable);
    }

    LLGLDisable disable_blend(GL_BLEND);

    mFsSkyVerts->setBuffer();
    mFsSkyVerts->drawRange(LLRender::TRIANGLES, 0, mFsSkyVerts->getNumVerts() - 1, mFsSkyVerts->getNumIndices(), 0);
    gPipeline.addTrianglesDrawn(mFsSkyVerts->getNumIndices());
    LLVertexBuffer::unbind();
}

void LLVOWLSky::drawDome(void)
{
    if (mStripsVerts.empty())
    {
        updateGeometry(mDrawable);
    }

    LLGLDepthTest gls_depth(GL_TRUE, GL_FALSE);

    std::vector< LLPointer<LLVertexBuffer> >::const_iterator strips_vbo_iter, end_strips;
    end_strips = mStripsVerts.end();
    for(strips_vbo_iter = mStripsVerts.begin(); strips_vbo_iter != end_strips; ++strips_vbo_iter)
    {
        LLVertexBuffer * strips_segment = strips_vbo_iter->get();

        strips_segment->setBuffer();

        strips_segment->drawRange(
            LLRender::TRIANGLE_STRIP,
            0, strips_segment->getNumVerts()-1, strips_segment->getNumIndices(),
            0);
        gPipeline.addTrianglesDrawn(strips_segment->getNumIndices());
    }

    LLVertexBuffer::unbind();
}

void LLVOWLSky::initStars()
{
    const F32 DISTANCE_TO_STARS = LLEnvironment::instance().getCurrentSky()->getDomeRadius();

    // <plus> plus/starfield (research/176): a catalogue of thousands, faint and bright, tinted
    mStarMode = plus::starfield::mode();
    mStarDense = plus::starfield::dense();                      // research/224
    mStarHQ = plus::starfield::bakedMilkyWay(mStarMode);
    mStarRealAsked = plus::realstars::wanted();
    mStarReal = false;
    mLineCount = 0;
    // research/288: the pictures' plates go with the real sky (read again with it; the files do not
    // change, so a texture made stays until refreshStars() finds the real sky or the switch gone);
    // a texture that could not be made is tried again
    mStarArt.plates.clear();
    mArtCount = 0;
    mArtTextureFailed = false;
    if (mStarMode > plus::starfield::MODE_OFFICIAL)
    {
        // plus/realstars (research/177): the real stars in J2000 over the whole sphere, or, when
        // they are not asked for or their catalogue cannot be read, the made-up sky
        mStarReal = mStarRealAsked && plus::realstars::fill(mStarMode, mStarField);
        if (!mStarReal)
        {
            plus::starfield::generate(mStarMode, mStarField);
        }
        mLineCount = mStarReal ? (U32)mStarField.line_a.size() : 0;
        if (mStarReal)
        {
            plus::realstars::fillArt(mStarArt);
            mArtCount = (U32)mStarArt.plates.size();
        }
        const size_t count = mStarField.star_dir.size();
        mStarVertices.resize(count);
        mStarBoost = plus::starfield::constellationBoost();    // research/177 §2.7
        plus::starfield::boostedColors(mStarField, mStarBoost, mStarColors);
        mStarIntensities.assign(count, 1.f);
        for (size_t i = 0; i < count; ++i)
        {
            mStarVertices[i] = mStarField.star_dir[i] * DISTANCE_TO_STARS;
        }
        mGlowCount = (U32)mStarField.glow_dir.size();
        return;
    }
    mGlowCount = 0;
    mStarField.clear();
    // </plus>

    // Initialize star map
    mStarVertices.resize(getStarsNumVerts());
    mStarColors.resize(getStarsNumVerts());
    mStarIntensities.resize(getStarsNumVerts());

    std::vector<LLVector3>::iterator v_p = mStarVertices.begin();
    std::vector<LLColor4>::iterator v_c = mStarColors.begin();
    std::vector<F32>::iterator v_i = mStarIntensities.begin();

    U32 i;

    for (i = 0; i < getStarsNumVerts(); ++i)
    {
        v_p->mV[VX] = ll_frand() - 0.5f;
        v_p->mV[VY] = ll_frand() - 0.5f;

        // we only want stars on the top half of the dome!

        v_p->mV[VZ] = ll_frand()/2.f;

        v_p->normVec();
        *v_p *= DISTANCE_TO_STARS;
        *v_i = llmin((F32)pow(ll_frand(),2.f) + 0.1f, 1.f);
        v_c->mV[VRED]   = 0.75f + ll_frand() * 0.25f ;
        v_c->mV[VGREEN] = 1.f ;
        v_c->mV[VBLUE]  = 0.75f + ll_frand() * 0.25f ;
        v_c->mV[VALPHA] = 1.f;
        v_c->clamp();
        v_p++;
        v_c++;
        v_i++;
    }
}

void LLVOWLSky::buildStripsBuffer(U32 begin_stack,
                                  U32 end_stack,
                                  LLStrider<LLVector3> & vertices,
                                  LLStrider<LLVector2> & texCoords,
                                  LLStrider<U16> & indices,
                                  const F32 dome_radius,
                                  const U32& num_slices,
                                  const U32& num_stacks)
{
    U32 i, j;
    F32 phi0, theta, x0, y0, z0;
    const F32 reciprocal_num_stacks = 1.f / num_stacks;

    llassert(end_stack <= num_stacks);

    // stacks are iterated one-indexed since phi(0) was handled by the fan above
#if NEW_TESS
    for(i = begin_stack; i <= end_stack; ++i)
#else
    for(i = begin_stack + 1; i <= end_stack+1; ++i)
#endif
    {
        phi0 = calcPhi(i, reciprocal_num_stacks);

        for(j = 0; j < num_slices; ++j)
        {
            theta = F_TWO_PI * (float(j) / float(num_slices));

            // standard transformation from  spherical to
            // rectangular coordinates
            x0 = sin(phi0) * cos(theta);
            y0 = cos(phi0);
            z0 = sin(phi0) * sin(theta);

#if NEW_TESS
            *vertices++ = LLVector3(x0 * dome_radius, y0 * dome_radius, z0 * dome_radius);
#else
            if (i == num_stacks-2)
            {
                *vertices++ = LLVector3(x0*dome_radius, y0*dome_radius-1024.f*2.f, z0*dome_radius);
            }
            else if (i == num_stacks-1)
            {
                *vertices++ = LLVector3(0, y0*dome_radius-1024.f*2.f, 0);
            }
            else
            {
                *vertices++     = LLVector3(x0 * dome_radius, y0 * dome_radius, z0 * dome_radius);
            }
#endif

            // generate planar uv coordinates
            // note: x and z are transposed in order for things to animate
            // correctly in the global coordinate system where +x is east and
            // +y is north
            *texCoords++    = LLVector2((-z0 + 1.f) / 2.f, (-x0 + 1.f) / 2.f);
        }
    }

    //build triangle strip...
    *indices++ = 0 ;

    S32 k = 0 ;
    for(i = 1; i <= end_stack - begin_stack; ++i)
    {
        *indices++ = i * num_slices + k ;

        k = (k+1) % num_slices ;
        for(j = 0; j < num_slices ; ++j)
        {
            *indices++ = (i-1) * num_slices + k ;
            *indices++ = i * num_slices + k ;

            k = (k+1) % num_slices ;
        }

        if((--k) < 0)
        {
            k = num_slices - 1 ;
        }

        *indices++ = i * num_slices + k ;
    }
}

void LLVOWLSky::updateStarColors()
{
    // <plus> plus/starfield: the catalogue's colours do not change
    if (mStarMode > plus::starfield::MODE_OFFICIAL)
    {
        return;
    }
    // </plus>
    std::vector<LLColor4>::iterator v_c = mStarColors.begin();
    std::vector<F32>::iterator v_i = mStarIntensities.begin();
    std::vector<LLVector3>::iterator v_p = mStarVertices.begin();

    const F32 var = 0.15f;
    const F32 min = 0.5f; //0.75f;
    //const F32 sunclose_max = 0.6f;
    //const F32 sunclose_range = 1 - sunclose_max;

    //F32 below_horizon = - llmin(0.0f, gSky.mVOSkyp->getToSunLast().mV[2]);
    //F32 brightness_factor = llmin(1.0f, below_horizon * 20);

    static S32 swap = 0;
    swap++;

    if ((swap % 2) == 1)
    {
        F32 intensity;                      //  max intensity of each star
        U32 x;
        for (x = 0; x < getStarsNumVerts(); ++x)
        {
            //F32 sundir_factor = 1;
            LLVector3 tostar = *v_p;
            tostar.normVec();
            //const F32 how_close_to_sun = tostar * gSky.mVOSkyp->getToSunLast();
            //if (how_close_to_sun > sunclose_max)
            //{
            //  sundir_factor = (1 - how_close_to_sun) / sunclose_range;
            //}
            intensity = *(v_i);
            F32 alpha = v_c->mV[VALPHA] + (ll_frand() - 0.5f) * var * intensity;
            if (alpha < min * intensity)
            {
                alpha = min * intensity;
            }
            if (alpha > intensity)
            {
                alpha = intensity;
            }
            //alpha *= brightness_factor * sundir_factor;

            alpha = llclamp(alpha, 0.f, 1.f);
            v_c->mV[VALPHA] = alpha;
            v_c++;
            v_i++;
            v_p++;
        }
    }
}

bool LLVOWLSky::updateStarGeometry(LLDrawable *drawable)
{
    // <plus> plus/starfield
    if (mStarMode > plus::starfield::MODE_OFFICIAL)
    {
        return updateStarFieldGeometry();
    }
    // </plus>

    LLStrider<LLVector3> verticesp;
    LLStrider<LLColor4U> colorsp;
    LLStrider<LLVector2> texcoordsp;

    if (mStarsVerts.isNull())
    {
        mStarsVerts = new LLVertexBuffer(LLDrawPoolWLSky::STAR_VERTEX_DATA_MASK);
        if (!mStarsVerts->allocateBuffer(getStarsNumVerts()*6, 0))
        {
            LL_WARNS() << "Failed to allocate Vertex Buffer for Sky to " << getStarsNumVerts() * 6 << " vertices" << LL_ENDL;
        }
    }

    bool success = mStarsVerts->getVertexStrider(verticesp)
        && mStarsVerts->getColorStrider(colorsp)
        && mStarsVerts->getTexCoord0Strider(texcoordsp);

    if(!success)
    {
        LL_ERRS() << "Failed updating star geometry." << LL_ENDL;
    }

    // *TODO: fix LLStrider with a real prefix increment operator so it can be
    // used as a model of OutputIterator. -Brad
    // std::copy(mStarVertices.begin(), mStarVertices.end(), verticesp);

    if (mStarVertices.size() < getStarsNumVerts())
    {
        LL_ERRS() << "Star reference geometry insufficient." << LL_ENDL;
    }

    for (U32 vtx = 0; vtx < getStarsNumVerts(); ++vtx)
    {
        LLVector3 at = mStarVertices[vtx];
        at.normVec();
        LLVector3 left = at%LLVector3(0,0,1);
        LLVector3 up = at%left;

        F32 sc = 16.0f + (ll_frand() * 20.0f);
        left *= sc;
        up *= sc;

        *(verticesp++)  = mStarVertices[vtx];
        *(verticesp++) = mStarVertices[vtx]+up;
        *(verticesp++) = mStarVertices[vtx]+left+up;
        *(verticesp++)  = mStarVertices[vtx];
        *(verticesp++) = mStarVertices[vtx]+left+up;
        *(verticesp++) = mStarVertices[vtx]+left;

        *(texcoordsp++) = LLVector2(1,0);
        *(texcoordsp++) = LLVector2(1,1);
        *(texcoordsp++) = LLVector2(0,1);
        *(texcoordsp++) = LLVector2(1,0);
        *(texcoordsp++) = LLVector2(0,1);
        *(texcoordsp++) = LLVector2(0,0);

        *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        *(colorsp++)    = LLColor4U(mStarColors[vtx]);
        *(colorsp++)    = LLColor4U(mStarColors[vtx]);
    }

    mStarsVerts->unmapBuffer();
    return true;
}
