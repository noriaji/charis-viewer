 /**
 * @file llrender.cpp
 * @brief LLRender implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "linden_common.h"

#include "llrender.h"

#include "llvertexbuffer.h"
#include "llcubemap.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llrendertarget.h"
#include "lltexture.h"
#include "llshadermgr.h"
#include "hbxxh.h"
#include "glm/gtc/type_ptr.hpp"
#include "llrhi.h"
#include "llrhiimmediate.h" // LLRHI native (research/137 N9)
// <plus> research/207 T4: the state copy for the world passes' write-through
#include "llglstates.h"
#include "llrhipassstate.h"
// </plus>
// <plus> frame cost (research/95)
#include "llthread.h"
// </plus>

#if GL_ARB_debug_output
#ifndef APIENTRY
#define APIENTRY
#endif

extern void APIENTRY gl_debug_callback(GLenum source,
                                GLenum type,
                                GLuint id,
                                GLenum severity,
                                GLsizei length,
                                const GLchar* message,
                                GLvoid* userParam)
;
#endif

thread_local LLRender gGL;

// Handy copies of last good GL matrices
F32 gGLModelView[16];
F32 gGLLastModelView[16];
F32 gGLLastProjection[16];
F32 gGLProjection[16];

// transform from last frame's camera space to this frame's camera space (and inverse)
glm::mat4 gGLDeltaModelView;
glm::mat4 gGLInverseDeltaModelView;

S32 gGLViewport[4];


U32 LLRender::sUICalls = 0;
U32 LLRender::sUIVerts = 0;
U32 LLTexUnit::sWhiteTexture = 0;
F32 LLRender::sAnisotropicFilteringLevel = 0.f;
bool LLRender::sGLCoreProfile = false;
bool LLRender::sNsightDebugSupport = false;
LLVector2 LLRender::sUIGLScaleFactor = LLVector2(1.f, 1.f);
bool LLRender::sClassicMode = false;

static const GLenum sGLTextureType[] =
{
    GL_TEXTURE_2D,
    GL_TEXTURE_RECTANGLE,
    GL_TEXTURE_CUBE_MAP,
    GL_TEXTURE_CUBE_MAP_ARRAY,
    GL_TEXTURE_2D_MULTISAMPLE,
    GL_TEXTURE_3D,
    GL_TEXTURE_2D_ARRAY // <plus> TT_TEXTURE_2D_ARRAY (research/288) </plus>
};

static const GLint sGLAddressMode[] =
{
    GL_REPEAT,
    GL_MIRRORED_REPEAT,
    GL_CLAMP_TO_EDGE
};

const U32 immediate_mask = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_COLOR | LLVertexBuffer::MAP_TEXCOORD0;

static const GLenum sGLBlendFactor[] =
{
    GL_ONE,
    GL_ZERO,
    GL_DST_COLOR,
    GL_SRC_COLOR,
    GL_ONE_MINUS_DST_COLOR,
    GL_ONE_MINUS_SRC_COLOR,
    GL_DST_ALPHA,
    GL_SRC_ALPHA,
    GL_ONE_MINUS_DST_ALPHA,
    GL_ONE_MINUS_SRC_ALPHA,

    GL_ZERO // 'BF_UNDEF'
};

// <plus> research/207 T4: the state copy as the world passes' write-through
// sees it (LLRHI::PassStateCopy). The copy is this thread's gGL (thread_local)
// with LLGLState's caps and LLGLDepthTest; the world passes run on the render
// thread.
namespace
{
    class StateCopy final : public LLRHI::PassStateCopy
    {
    public:
        void read(LLRHI::PassState& out) override { gGL.readStateCopy(out); }
        void write(const LLRHI::PassState& values, U32 fields) override { gGL.writeStateCopy(values, fields); }
    };
    StateCopy sStateCopy;

    void install_state_copy()
    {
        LLRHI::sPassStateCopy = &sStateCopy;
    }

    // The factor that LLRender::blendFunc sends as `gl` (GL_ZERO: BF_ZERO, not BF_UNDEF).
    bool blend_factor_of(U32 gl, LLRender::eBlendFactor& out)
    {
        for (U32 i = 0; i < (U32)LLRender::BF_UNDEF; ++i)
        {
            if (sGLBlendFactor[i] == gl)
            {
                out = (LLRender::eBlendFactor)i;
                return true;
            }
        }
        return false;
    }
}
// </plus>

LLTexUnit::LLTexUnit(S32 index)
    : mCurrTexType(TT_NONE),
    mCurrTexture(0),
    mHasMipMaps(false),
    mIndex(index)
{
    llassert_always(index < (S32)LL_NUM_TEXTURE_LAYERS);
}

//static
U32 LLTexUnit::getInternalType(eTextureType type)
{
    return sGLTextureType[type];
}

// <plus> Native texture binding (research/39 N4; research/207 T2: Vulkan only,
// no GL fallback): the backend takes these directly (LLRHI::sActiveTexture,
// sBindTexture, sTexParameters; LLVertexBuffer::initClass stops the viewer if
// they are missing); each does what the GL calls would.

// glActiveTexture(GL_TEXTURE0 + unit) then glBindTexture(target, name).
static inline void bind_texture_on_unit(S32 unit, GLenum target, U32 name)
{
    LLRHI::sBindTexture((U32)unit, target, name);
}

// glBindTexture(target, name) on unit, which the caller has made the active one
// (LLTexUnit::activate). The same call: the backend's bind names its unit.
static inline void bind_texture_on_active_unit(S32 unit, GLenum target, U32 name)
{
    LLRHI::sBindTexture((U32)unit, target, name);
}
// </plus>

// <plus> frame cost (research/95): immediate mode counts, the main thread's, while LLRHI_FRAME_COST is on
static inline bool count_immediate()
{
    return LLRHI::sFrameCost && on_main_thread();
}

static inline void count_immediate(U32& counter)
{
    if (count_immediate())
    {
        ++counter;
    }
}

// A texture bound with vertices pending: they are flushed first.
static inline void count_texture_flush(bool pending)
{
    if (pending && count_immediate())
    {
        ++LLRHI::sImmediate.texture_flushes;
    }
}

// The same, telling apart binding the texture already bound (bound_name) again.
static inline void count_texture_flush(bool pending, const LLTexture* texture, U32 bound_name, bool force_bind)
{
    if (pending && count_immediate())
    {
        const LLImageGL* gl_tex = texture ? texture->getGLTexture() : nullptr;
        const bool same = !force_bind && gl_tex && gl_tex->getTexName() == bound_name;
        ++(same ? LLRHI::sImmediate.same_texture_flushes : LLRHI::sImmediate.texture_flushes);
    }
}
// </plus>

// glTexParameteri/f for each, on the active unit's texture of target.
static inline void set_tex_parameters(GLenum target, const LLRHI::TexParameter* params, U32 count)
{
    LLRHI::sTexParameters(target, params, count); // <plus> research/207 T2: Vulkan only </plus>
}

void LLTexUnit::refreshState(void)
{
    // We set dirty to true so that the tex unit knows to ignore caching
    // and we reset the cached tex unit state

    gGL.flush();

    if (mCurrTexType != TT_NONE)
    {
        bind_texture_on_unit(mIndex, sGLTextureType[mCurrTexType], mCurrTexture);
    }
    else
    {
        bind_texture_on_unit(mIndex, GL_TEXTURE_2D, 0);
    }
}

void LLTexUnit::activate(void)
{
    if (mIndex < 0) return;

    if ((S32)gGL.mCurrTextureUnitIndex != mIndex || gGL.mDirty)
    {
        gGL.flush();
        LLRHI::sActiveTexture((U32)mIndex); // <plus> research/207 T2: Vulkan only </plus>
        gGL.mCurrTextureUnitIndex = mIndex;
    }
}

void LLTexUnit::enable(eTextureType type)
{
    if (mIndex < 0) return;

    if ( (mCurrTexType != type || gGL.mDirty) && (type != TT_NONE) )
    {
        activate();
        if (mCurrTexType != TT_NONE && !gGL.mDirty)
        {
            disable(); // Force a disable of a previous texture type if it's enabled.
        }
        mCurrTexType = type;

        gGL.flush();
    }
}

void LLTexUnit::disable(void)
{
    if (mIndex < 0) return;

    if (mCurrTexType != TT_NONE)
    {
        unbind(mCurrTexType);
        mCurrTexType = TT_NONE;
    }
}

LLTexture* LLTexUnit::sUnit0Override = nullptr;  // <plus> GrayWire </plus>

void LLTexUnit::bindFast(LLTexture* texture)
{
    if (sUnit0Override && mIndex == 0) { texture = sUnit0Override; } // <plus> GrayWire </plus>
    LLImageGL* gl_tex = texture->getGLTexture();
    texture->setActive();
    // <plus> research/207 T2: the bind below (the backend's) makes the unit active </plus>
    gGL.mCurrTextureUnitIndex = mIndex;
    mCurrTexture = gl_tex->getTexName();
    mCurrTexType = gl_tex->getTarget();
    if (!mCurrTexture)
    {
        LL_PROFILE_ZONE_NAMED("MISSING TEXTURE");
        //if deleted, will re-generate it immediately
        texture->forceImmediateUpdate();
        gl_tex->forceUpdateBindStats();
        texture->bindDefaultImage(mIndex);
    }
    bind_texture_on_active_unit(mIndex, sGLTextureType[gl_tex->getTarget()], mCurrTexture);
    mHasMipMaps = gl_tex->mHasMipMaps;
    if (gl_tex->mTexOptionsDirty)
    {
        gl_tex->mTexOptionsDirty = false;
        setTextureAddressModeFast(gl_tex->mAddressMode, gl_tex->getTarget());
        setTextureFilteringOptionFast(gl_tex->mFilterOption, gl_tex->getTarget());
    }
}

// <plus> LLRHI (research/197 stage 1a): what a direct draw names gl_tex by (it
// has a GL name and no sampler options to apply): its number as a draw handle
// while the switch is on, else its GL name. Verify: the backend compares the
// number's image with the GL name; a number of another image is not handed
// out (the draw goes by the GL name, as before).
static inline U32 direct_handle(const LLImageGL* gl_tex)
{
    const U32 name = gl_tex->getTexName();
    const LLRHI::TexRef ref = gl_tex->rhiTex();
    if (!LLRHI::sTexHandles || !ref.id) // research/218 vk-s7g: the translation thread's branch is gone (research/207 T1)
    {
        if (LLRHI::sTexHandles && LLRHI::sTexHandlesVerify && !ref.id)
        {
            LLRHI::Textures::verify(ref, name, 0);  // counted as one without a number
        }
        return name;    // off, or no number (names, as before)
    }
    const U16 sampler = gl_tex->rhiSampler();
    if (LLRHI::sTexHandlesVerify && !LLRHI::Textures::verify(ref, name, sampler))
    {
        return name;
    }
    return LLRHI::texHandle(ref, sampler);
}
// </plus>

U32 LLTexUnit::directTexture(LLTexture* texture)
{
    texture = unitTexture(texture, mIndex); // <plus> GrayWire: as bindFast (review retro-vulkan 1) </plus>
    LLImageGL* gl_tex = texture->getGLTexture();
    // <plus> research/256 mat-fix: the avatar's hold only. A batch whose texture the materialize band holds
    // reaches this draw through GrayWire::drawBatch already (directName and directRef keep that hold, so the
    // paths that skip drawBatch hand such a batch to the pools' loops): its loaded texture goes by its number
    // as any loaded texture does, not bound to the unit (KEEP_UNIT) for a draw that reads the unit.
    if (!gl_tex || !gl_tex->getTexName() || gl_tex->mTexOptionsDirty || gl_tex->wireHeld())
    // </plus>
    {
        bindFast(texture);
        return LLRHI::DirectDraw::KEEP_UNIT;
    }
    texture->setActive();
    return direct_handle(gl_tex); // LLRHI (research/197 1a): was gl_tex->getTexName()
}

// static
U32 LLTexUnit::directRef(LLTexture* texture)
{
    // LLRHI (research/197 stage 1a): directName, as a draw handle when the switch is on
    LLImageGL* gl_tex = texture ? texture->getGLTexture() : nullptr;
    if (!gl_tex || !gl_tex->getTexName() || gl_tex->mTexOptionsDirty || gl_tex->heldOnGL()) // <plus> research/256: either GrayWire hold </plus>
    {
        return LLRHI::DirectDraw::KEEP_UNIT;
    }
    return direct_handle(gl_tex);
}

// <plus> GrayWire (review retro-vulkan 1)
// static
U32 LLTexUnit::directRef(LLTexture* texture, S32 unit)
{
    return directRef(unitTexture(texture, unit));
}
// </plus>

// static
U32 LLTexUnit::directName(LLTexture* texture)
{
    LLImageGL* gl_tex = texture ? texture->getGLTexture() : nullptr;
    if (!gl_tex || !gl_tex->getTexName() || gl_tex->mTexOptionsDirty || gl_tex->heldOnGL()) // <plus> research/256: either GrayWire hold </plus>
    {
        return LLRHI::DirectDraw::KEEP_UNIT;
    }
    return gl_tex->getTexName();
}

bool LLTexUnit::bind(LLTexture* texture, bool for_rendering, bool forceBind)
{
    if (sUnit0Override && mIndex == 0) { texture = sUnit0Override; } // <plus> GrayWire </plus>
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    stop_glerror();
    if (mIndex >= 0)
    {
        // <plus> frame cost (research/95)
        count_texture_flush(gGL.mCount > 0, texture, mCurrTexture, forceBind);
        // </plus>
        gGL.flush();

        LLImageGL* gl_tex = NULL ;

        if (texture != NULL && (gl_tex = texture->getGLTexture()))
        {
            if (gl_tex->getTexName()) //if texture exists
            {
                //in audit, replace the selected texture by the default one.
                if ((mCurrTexture != gl_tex->getTexName()) || forceBind)
                {
                    activate();
                    enable(gl_tex->getTarget());
                    mCurrTexture = gl_tex->getTexName();
                    bind_texture_on_active_unit(mIndex, sGLTextureType[gl_tex->getTarget()], mCurrTexture);
                    if(gl_tex->updateBindStats())
                    {
                        texture->setActive() ;
                        texture->updateBindStatsForTester() ;
                    }
                    mHasMipMaps = gl_tex->mHasMipMaps;
                    if (gl_tex->mTexOptionsDirty)
                    {
                        gl_tex->mTexOptionsDirty = false;
                        setTextureAddressMode(gl_tex->mAddressMode);
                        setTextureFilteringOption(gl_tex->mFilterOption);
                    }
                }
            }
            else
            {
                //if deleted, will re-generate it immediately
                texture->forceImmediateUpdate() ;

                gl_tex->forceUpdateBindStats() ;
                return texture->bindDefaultImage(mIndex);
            }
        }
        else
        {
            if (texture)
            {
                LL_DEBUGS() << "NULL LLTexUnit::bind GL image" << LL_ENDL;
            }
            else
            {
                LL_DEBUGS() << "NULL LLTexUnit::bind texture" << LL_ENDL;
            }
            return false;
        }
    }
    else
    { // mIndex < 0
        return false;
    }

    return true;
}

bool LLTexUnit::bind(LLImageGL* texture, bool for_rendering, bool forceBind, S32 usename)
{
    stop_glerror();
    if (mIndex < 0) return false;

    U32 texname = usename ? usename : texture->getTexName();

    if(!texture)
    {
        LL_DEBUGS() << "NULL LLTexUnit::bind texture" << LL_ENDL;
        return false;
    }

    if(!texname)
    {
        if(LLImageGL::sDefaultGLTexture && LLImageGL::sDefaultGLTexture->getTexName())
        {
            return bind(LLImageGL::sDefaultGLTexture) ;
        }
        stop_glerror();
        return false ;
    }

    if ((mCurrTexture != texname) || forceBind)
    {
        // <plus> frame cost (research/95)
        count_texture_flush(gGL.mCount > 0);
        // </plus>
        gGL.flush();
        stop_glerror();
        activate();
        stop_glerror();
        enable(texture->getTarget());
        stop_glerror();
        mCurrTexture = texname;
        bind_texture_on_active_unit(mIndex, sGLTextureType[texture->getTarget()], mCurrTexture);
        stop_glerror();
        texture->updateBindStats();
        mHasMipMaps = texture->mHasMipMaps;
        if (texture->mTexOptionsDirty)
        {
            stop_glerror();
            texture->mTexOptionsDirty = false;
            setTextureAddressMode(texture->mAddressMode);
            setTextureFilteringOption(texture->mFilterOption);
            stop_glerror();
        }
    }

    stop_glerror();

    return true;
}

bool LLTexUnit::bind(LLCubeMap* cubeMap)
{
    if (mIndex < 0) return false;

    gGL.flush();

    if (cubeMap == NULL)
    {
        LL_WARNS() << "NULL LLTexUnit::bind cubemap" << LL_ENDL;
        return false;
    }

    if (mCurrTexture != cubeMap->mImages[0]->getTexName())
    {
        if (LLCubeMap::sUseCubeMaps)
        {
            activate();
            enable(LLTexUnit::TT_CUBE_MAP);
            mCurrTexture = cubeMap->mImages[0]->getTexName();
            bind_texture_on_active_unit(mIndex, GL_TEXTURE_CUBE_MAP, mCurrTexture);
            mHasMipMaps = cubeMap->mImages[0]->mHasMipMaps;
            cubeMap->mImages[0]->updateBindStats();
            if (cubeMap->mImages[0]->mTexOptionsDirty)
            {
                cubeMap->mImages[0]->mTexOptionsDirty = false;
                setTextureAddressMode(cubeMap->mImages[0]->mAddressMode);
                setTextureFilteringOption(cubeMap->mImages[0]->mFilterOption);
            }
            return true;
        }
        else
        {
            LL_WARNS() << "Using cube map without extension!" << LL_ENDL;
            return false;
        }
    }
    return true;
}

// LLRenderTarget is unavailible on the mapserver since it uses FBOs.
bool LLTexUnit::bind(LLRenderTarget* renderTarget, bool bindDepth)
{
    if (mIndex < 0) return false;

    gGL.flush();

    if (bindDepth)
    {
        llassert(renderTarget->getDepth()); // target MUST have a depth buffer attachment

        bindManual(renderTarget->getUsage(), renderTarget->getDepth());
    }
    else
    {
        bindManual(renderTarget->getUsage(), renderTarget->getTexture());
    }

    return true;
}

bool LLTexUnit::bindManual(eTextureType type, U32 texture, bool hasMips)
{
    if (mIndex < 0)
    {
        return false;
    }

    if(mCurrTexture != texture)
    {
        // <plus> frame cost (research/95)
        count_texture_flush(gGL.mCount > 0);
        // </plus>
        gGL.flush();

        activate();
        enable(type);
        mCurrTexture = texture;
        bind_texture_on_active_unit(mIndex, sGLTextureType[type], texture);
        mHasMipMaps = hasMips;
    }
    return true;
}

void LLTexUnit::unbind(eTextureType type)
{
    stop_glerror();

    if (mIndex < 0) return;

    //always flush and activate for consistency
    //   some code paths assume unbind always flushes and sets the active texture
    gGL.flush();
    activate();

    // Disabled caching of binding state.
    if (mCurrTexType == type)
    {
        mCurrTexture = 0;

        if (type == LLTexUnit::TT_TEXTURE)
        {
            bind_texture_on_active_unit(mIndex, sGLTextureType[type], sWhiteTexture);
        }
        else
        {
            bind_texture_on_active_unit(mIndex, sGLTextureType[type], 0);
        }
        stop_glerror();
    }
}

void LLTexUnit::unbindFast(eTextureType type)
{
    activate();

    // Disabled caching of binding state.
    if (mCurrTexType == type)
    {
        mCurrTexture = 0;

        if (type == LLTexUnit::TT_TEXTURE)
        {
            bind_texture_on_active_unit(mIndex, sGLTextureType[type], sWhiteTexture);
        }
        else
        {
            bind_texture_on_active_unit(mIndex, sGLTextureType[type], 0);
        }
    }
}

void LLTexUnit::setTextureAddressMode(eTextureAddressMode mode)
{
    if (mIndex < 0 || mCurrTexture == 0) return;

    gGL.flush();

    activate();

    setTextureAddressModeFast(mode, mCurrTexType);
}

void LLTexUnit::setTextureAddressModeFast(eTextureAddressMode mode, eTextureType tex_type)
{
    const F32 wrap = (F32)sGLAddressMode[mode];
    const LLRHI::TexParameter params[] =
    {
        { GL_TEXTURE_WRAP_S, wrap },
        { GL_TEXTURE_WRAP_T, wrap },
        { GL_TEXTURE_WRAP_R, wrap },
    };
    const bool wrap_r = tex_type == TT_CUBE_MAP || tex_type == TT_CUBE_MAP_ARRAY || tex_type == TT_TEXTURE_3D;
    set_tex_parameters(sGLTextureType[tex_type], params, wrap_r ? 3 : 2);
}

void LLTexUnit::setTextureFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    if (mIndex < 0 || mCurrTexture == 0 || mCurrTexType == LLTexUnit::TT_MULTISAMPLE_TEXTURE) return;

    gGL.flush();

    setTextureFilteringOptionFast(option, mCurrTexType);
}

void LLTexUnit::setTextureFilteringOptionFast(LLTexUnit::eTextureFilterOptions option, eTextureType tex_type)
{
    LLRHI::TexParameter params[3];
    U32 count = 0;

    params[count++] = { GL_TEXTURE_MAG_FILTER, (F32)(option == TFO_POINT ? GL_NEAREST : GL_LINEAR) };

    GLenum min_filter;
    if (option >= TFO_TRILINEAR && mHasMipMaps)
    {
        min_filter = GL_LINEAR_MIPMAP_LINEAR;
    }
    else if (option >= TFO_BILINEAR)
    {
        min_filter = mHasMipMaps ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
    }
    else
    {
        min_filter = mHasMipMaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    }
    params[count++] = { GL_TEXTURE_MIN_FILTER, (F32)min_filter };

    if (gGLManager.mHasAnisotropic)
    {
        F32 aniso_level = 1.f;
        if (option == TFO_ANISOTROPIC && LLRender::sAnisotropicFilteringLevel > 1.f)
        {
            aniso_level = llclamp(LLRender::sAnisotropicFilteringLevel, 1.f, gGLManager.mMaxAnisotropy);
        }
        params[count++] = { GL_TEXTURE_MAX_ANISOTROPY, aniso_level };
    }

    set_tex_parameters(sGLTextureType[tex_type], params, count);
}

// Useful for debugging that you've manually assigned a texture operation to the correct
// texture unit based on the currently set active texture in opengl.
void LLTexUnit::debugTextureUnit(void)
{
    if (mIndex < 0) return;

    GLint activeTexture;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    if ((GL_TEXTURE0 + mIndex) != activeTexture)
    {
        U32 set_unit = (activeTexture - GL_TEXTURE0);
        LL_WARNS() << "Incorrect Texture Unit!  Expected: " << set_unit << " Actual: " << mIndex << LL_ENDL;
    }
}

LLLightState::LLLightState(S32 index)
: mIndex(index),
  mEnabled(false),
  mConstantAtten(1.f),
  mLinearAtten(0.f),
  mQuadraticAtten(0.f),
  mSpotExponent(0.f),
  mSpotCutoff(180.f)
{
    if (mIndex == 0)
    {
        mDiffuse.set(1,1,1,1);
        mDiffuseB.set(0,0,0,0);
        mSpecular.set(1,1,1,1);
    }

    mSunIsPrimary = true;

    mAmbient.set(0,0,0,1);
    mPosition.set(0,0,1,0);
    mSpotDirection.set(0,0,-1);
}

void LLLightState::enable()
{
    mEnabled = true;
}

void LLLightState::disable()
{
    mEnabled = false;
}

void LLLightState::setDiffuse(const LLColor4& diffuse)
{
    if (mDiffuse != diffuse)
    {
        ++gGL.mLightHash;
        mDiffuse = diffuse;
    }
}

void LLLightState::setDiffuseB(const LLColor4& diffuse)
{
    if (mDiffuseB != diffuse)
    {
        ++gGL.mLightHash;
        mDiffuseB = diffuse;
    }
}

void LLLightState::setSunPrimary(bool v)
{
    if (mSunIsPrimary != v)
    {
        ++gGL.mLightHash;
        mSunIsPrimary = v;
    }
}

void LLLightState::setSize(F32 v)
{
    if (mSize != v)
    {
        ++gGL.mLightHash;
        mSize = v;
    }
}

void LLLightState::setFalloff(F32 v)
{
    if (mFalloff != v)
    {
        ++gGL.mLightHash;
        mFalloff = v;
    }
}

void LLLightState::setAmbient(const LLColor4& ambient)
{
    if (mAmbient != ambient)
    {
        ++gGL.mLightHash;
        mAmbient = ambient;
    }
}

void LLLightState::setSpecular(const LLColor4& specular)
{
    if (mSpecular != specular)
    {
        ++gGL.mLightHash;
        mSpecular = specular;
    }
}

void LLLightState::setPosition(const LLVector4& position)
{
    //always set position because modelview matrix may have changed
    ++gGL.mLightHash;
    mPosition = position;
    //transform position by current modelview matrix
    glm::vec4 pos(position);
    pos = gGL.getModelviewMatrix() * pos;
    mPosition.set(glm::value_ptr(pos));
}

void LLLightState::setConstantAttenuation(const F32& atten)
{
    if (mConstantAtten != atten)
    {
        mConstantAtten = atten;
        ++gGL.mLightHash;
    }
}

void LLLightState::setLinearAttenuation(const F32& atten)
{
    if (mLinearAtten != atten)
    {
        ++gGL.mLightHash;
        mLinearAtten = atten;
    }
}

void LLLightState::setQuadraticAttenuation(const F32& atten)
{
    if (mQuadraticAtten != atten)
    {
        ++gGL.mLightHash;
        mQuadraticAtten = atten;
    }
}

void LLLightState::setSpotExponent(const F32& exponent)
{
    if (mSpotExponent != exponent)
    {
        ++gGL.mLightHash;
        mSpotExponent = exponent;
    }
}

void LLLightState::setSpotCutoff(const F32& cutoff)
{
    if (mSpotCutoff != cutoff)
    {
        ++gGL.mLightHash;
        mSpotCutoff = cutoff;
    }
}

void LLLightState::setSpotDirection(const LLVector3& direction)
{
    //always set direction because modelview matrix may have changed
    ++gGL.mLightHash;

    //transform direction by current modelview matrix
    glm::vec3 dir(direction);
    const glm::mat3 mat(gGL.getModelviewMatrix());
    dir = mat * dir;

    mSpotDirection.set(glm::value_ptr(dir));
}

LLRender::LLRender()
  : mDirty(false),
    mCount(0),
    mMode(LLRender::TRIANGLES),
    mCurrTextureUnitIndex(0)
{
    for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; i++)
    {
        mTexUnits[i].mIndex = i;
    }

    for (U32 i = 0; i < LL_NUM_LIGHT_UNITS; ++i)
    {
        mLightState[i].mIndex = i;
    }

    for (U32 i = 0; i < 4; i++)
    {
        mCurrColorMask[i] = true;
    }

    mCurrBlendColorSFactor = BF_UNDEF;
    mCurrBlendAlphaSFactor = BF_UNDEF;
    mCurrBlendColorDFactor = BF_UNDEF;
    mCurrBlendAlphaDFactor = BF_UNDEF;

    mMatrixMode = LLRender::MM_MODELVIEW;

    for (U32 i = 0; i < NUM_MATRIX_MODES; ++i)
    {
        for (U32 j = 0; j < LL_MATRIX_STACK_DEPTH; ++j)
        {
            mMatrix[i][j] = glm::identity<glm::mat4>();
        }
        mMatIdx[i] = 0;
        mMatHash[i] = 0;
        mCurMatHash[i] = 0xFFFFFFFF;
    }

    mLightHash = 0;
}

LLRender::~LLRender()
{
    shutdown();
}

bool LLRender::init(bool needs_vertex_buffer)
{
#if GL_ARB_debug_output && !LL_DARWIN
    if (gGLManager.mHasDebugOutput && gDebugGL)
    { //setup debug output callback
        //glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_LOW_ARB, 0, NULL, GL_TRUE);
        glDebugMessageCallback((GLDEBUGPROC) gl_debug_callback, NULL);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    }
#endif

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setAmbientLightColor(LLColor4::black);

    cullFace(GL_BACK);

    install_state_copy(); // <plus> research/207 T4: the copy the world passes write through </plus>

    // necessary for reflection maps
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

#if LL_WINDOWS
    if (glGenVertexArrays == nullptr)
    {
        return false;
    }
#endif

    { //bind a dummy vertex array object so we're core profile compliant
        U32 ret;
        glGenVertexArrays(1, &ret);
        glBindVertexArray(ret);
    }

    if (needs_vertex_buffer)
    {
        initVertexBuffer();
    }
    return true;
}

void LLRender::initVertexBuffer()
{
    llassert_always(mBuffer.isNull());
    stop_glerror();
    mBuffer = new LLVertexBuffer(immediate_mask);
    mBuffer->allocateBuffer(4096, 0);
    mBuffer->getVertexStrider(mVerticesp);
    mBuffer->getTexCoord0Strider(mTexcoordsp);
    mBuffer->getColorStrider(mColorsp);
    stop_glerror();
}

void LLRender::resetVertexBuffer()
{
    mBuffer = nullptr;
    mBufferDataList = nullptr;
    mVBCache.clear();
}

void LLRender::shutdown()
{
    resetVertexBuffer();
}

void LLRender::refreshState(void)
{
    mDirty = true;

    U32 active_unit = mCurrTextureUnitIndex;

    for (U32 i = 0; i < mTexUnits.size(); i++)
    {
        mTexUnits[i].refreshState();
    }

    mTexUnits[active_unit].activate();

    setColorMask(mCurrColorMask[0], mCurrColorMask[1], mCurrColorMask[2], mCurrColorMask[3]);

    flush();

    mDirty = false;
}

void LLRender::syncLightState()
{
    LLGLSLShader *shader = LLGLSLShader::sCurBoundShaderPtr;

    if (!shader)
    {
        return;
    }

    if (shader->mLightHash != mLightHash)
    {
        shader->mLightHash = mLightHash;

        LLVector4 position[LL_NUM_LIGHT_UNITS];
        LLVector3 direction[LL_NUM_LIGHT_UNITS];
        LLVector4 attenuation[LL_NUM_LIGHT_UNITS];
        LLVector3 diffuse[LL_NUM_LIGHT_UNITS];
        LLVector3 diffuse_b[LL_NUM_LIGHT_UNITS];
        bool      sun_primary[LL_NUM_LIGHT_UNITS];
        LLVector2 size[LL_NUM_LIGHT_UNITS];

        for (U32 i = 0; i < LL_NUM_LIGHT_UNITS; i++)
        {
            LLLightState *light = &mLightState[i];

            position[i]  = light->mPosition;
            direction[i] = light->mSpotDirection;
            attenuation[i].set(light->mLinearAtten, light->mQuadraticAtten, light->mSpecular.mV[2], light->mSpecular.mV[3]);
            diffuse[i].set(light->mDiffuse.mV);
            diffuse_b[i].set(light->mDiffuseB.mV);
            sun_primary[i] = light->mSunIsPrimary;
            size[i].set(light->mSize, light->mFalloff);
        }

        shader->uniform4fv(LLShaderMgr::LIGHT_POSITION, LL_NUM_LIGHT_UNITS, position[0].mV);
        shader->uniform3fv(LLShaderMgr::LIGHT_DIRECTION, LL_NUM_LIGHT_UNITS, direction[0].mV);
        shader->uniform4fv(LLShaderMgr::LIGHT_ATTENUATION, LL_NUM_LIGHT_UNITS, attenuation[0].mV);
        shader->uniform2fv(LLShaderMgr::LIGHT_DEFERRED_ATTENUATION, LL_NUM_LIGHT_UNITS, size[0].mV);
        shader->uniform3fv(LLShaderMgr::LIGHT_DIFFUSE, LL_NUM_LIGHT_UNITS, diffuse[0].mV);
        shader->uniform3fv(LLShaderMgr::LIGHT_AMBIENT, 1, mAmbientLightColor.mV);
        shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_primary[0] ? 1 : 0);

        if (sClassicMode)
        {
            shader->uniform3fv(LLShaderMgr::AMBIENT, 1, mAmbientLightColor.mV);
            shader->uniform3fv(LLShaderMgr::SUNLIGHT_COLOR, 1, diffuse[0].mV);
            shader->uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, diffuse_b[0].mV);
        }
    }
}

void LLRender::syncMatrices()
{
    STOP_GLERROR;
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    static const U32 name[] =
    {
        LLShaderMgr::MODELVIEW_MATRIX,
        LLShaderMgr::PROJECTION_MATRIX,
        LLShaderMgr::TEXTURE_MATRIX0,
        LLShaderMgr::TEXTURE_MATRIX1,
        LLShaderMgr::TEXTURE_MATRIX2,
        LLShaderMgr::TEXTURE_MATRIX3,
    };

    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;

    static glm::mat4 cached_mvp;
    static glm::mat4 cached_inv_mdv;
    static U32 cached_mvp_mdv_hash = 0xFFFFFFFF;
    static U32 cached_mvp_proj_hash = 0xFFFFFFFF;

    static glm::mat4 cached_normal;
    static U32 cached_normal_hash = 0xFFFFFFFF;

    if (shader)
    {
        bool mvp_done = false;

        U32 i = MM_MODELVIEW;
        if (mMatHash[MM_MODELVIEW] != shader->mMatHash[MM_MODELVIEW])
        { //update modelview, normal, and MVP
            const glm::mat4& mat = mMatrix[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]];

            // if MDV has changed, update the cached inverse as well
            if (cached_mvp_mdv_hash != mMatHash[MM_MODELVIEW])
            {
                cached_inv_mdv = glm::inverse(mat);
            }

            shader->uniformMatrix4fv(name[MM_MODELVIEW], 1, GL_FALSE, glm::value_ptr(mat));
            shader->mMatHash[MM_MODELVIEW] = mMatHash[MM_MODELVIEW];

            //update normal matrix
            S32 loc = shader->getUniformLocation(LLShaderMgr::NORMAL_MATRIX);
            if (loc > -1)
            {
                if (cached_normal_hash != mMatHash[i])
                {
                    cached_normal = glm::transpose(cached_inv_mdv);
                    cached_normal_hash = mMatHash[i];
                }

                auto norm = glm::value_ptr(cached_normal);

                F32 norm_mat[] =
                {
                    norm[0], norm[1], norm[2],
                    norm[4], norm[5], norm[6],
                    norm[8], norm[9], norm[10]
                };

                shader->uniformMatrix3fv(LLShaderMgr::NORMAL_MATRIX, 1, GL_FALSE, norm_mat);
            }

            if (shader->getUniformLocation(LLShaderMgr::INVERSE_MODELVIEW_MATRIX) >= 0)
            {
                shader->uniformMatrix4fv(LLShaderMgr::INVERSE_MODELVIEW_MATRIX, 1, GL_FALSE, glm::value_ptr(cached_inv_mdv));
            }

            //update MVP matrix
            mvp_done = true;
            loc = shader->getUniformLocation(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX);
            if (loc > -1)
            {
                U32 proj = MM_PROJECTION;

                if (cached_mvp_mdv_hash != mMatHash[i] || cached_mvp_proj_hash != mMatHash[MM_PROJECTION])
                {
                    cached_mvp = mat;
                    cached_mvp = mMatrix[proj][mMatIdx[proj]] * cached_mvp;
                    cached_mvp_mdv_hash = mMatHash[i];
                    cached_mvp_proj_hash = mMatHash[MM_PROJECTION];
                }

                shader->uniformMatrix4fv(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX, 1, GL_FALSE, glm::value_ptr(cached_mvp));
            }
        }

        i = MM_PROJECTION;
        if (mMatHash[MM_PROJECTION] != shader->mMatHash[MM_PROJECTION])
        { //update projection matrix, normal, and MVP
            const glm::mat4& mat = mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]];

            // GZ: This was previously disabled seemingly due to a bug involving the deferred renderer's regular pushing and popping of mats.
            // We're reenabling this and cleaning up the code around that - that would've been the appropriate course initially.
            // Anything beyond the standard proj and inv proj mats are special cases.  Please setup special uniforms accordingly in the future.
            if (shader->getUniformLocation(LLShaderMgr::INVERSE_PROJECTION_MATRIX) >= 0)
            {
                glm::mat4 inv_proj = glm::inverse(mat);
                shader->uniformMatrix4fv(LLShaderMgr::INVERSE_PROJECTION_MATRIX, 1, false, glm::value_ptr(inv_proj));
            }

            // Used by some full screen effects - such as full screen lights, glow, etc.
            if (shader->getUniformLocation(LLShaderMgr::IDENTITY_MATRIX) >= 0)
            {
                shader->uniformMatrix4fv(LLShaderMgr::IDENTITY_MATRIX, 1, GL_FALSE, glm::value_ptr(glm::identity<glm::mat4>()));
            }

            shader->uniformMatrix4fv(name[MM_PROJECTION], 1, GL_FALSE, glm::value_ptr(mat));
            shader->mMatHash[MM_PROJECTION] = mMatHash[MM_PROJECTION];

            if (!mvp_done)
            {
                //update MVP matrix
                S32 loc = shader->getUniformLocation(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX);
                if (loc > -1)
                {
                    if (cached_mvp_mdv_hash != mMatHash[MM_MODELVIEW] || cached_mvp_proj_hash != mMatHash[MM_PROJECTION])
                    {
                        U32 mdv = MM_MODELVIEW;
                        cached_mvp = mat;
                        cached_mvp *= mMatrix[mdv][mMatIdx[mdv]];
                        cached_mvp_mdv_hash = mMatHash[MM_MODELVIEW];
                        cached_mvp_proj_hash = mMatHash[MM_PROJECTION];
                    }

                    shader->uniformMatrix4fv(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX, 1, GL_FALSE, glm::value_ptr(cached_mvp));
                }
            }
        }

        for (i = MM_TEXTURE0; i < NUM_MATRIX_MODES; ++i)
        {
            if (mMatHash[i] != shader->mMatHash[i])
            {
                shader->uniformMatrix4fv(name[i], 1, GL_FALSE, glm::value_ptr(mMatrix[i][mMatIdx[i]]));
                shader->mMatHash[i] = mMatHash[i];
            }
        }


        if (shader->mFeatures.hasLighting || shader->mFeatures.calculatesLighting || shader->mFeatures.calculatesAtmospherics)
        { //also sync light state
            syncLightState();
        }
    }
    STOP_GLERROR;
}

void LLRender::fillDirectMatrices(const glm::mat4& modelview, DirectMatrices& storage, LLRHI::DirectDraw& draw) const
{
    // The same arithmetic as syncMatrices, for the uniforms the shader has.
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    if (!shader)
    {
        return;
    }
    storage.modelview = modelview;
    draw.modelview = glm::value_ptr(storage.modelview);

    const bool normal = shader->getUniformLocation(LLShaderMgr::NORMAL_MATRIX) > -1;
    const bool inverse = shader->getUniformLocation(LLShaderMgr::INVERSE_MODELVIEW_MATRIX) >= 0;
    if (normal || inverse)
    {
        storage.inverse_modelview = glm::inverse(modelview);
        if (inverse)
        {
            draw.inverse_modelview = glm::value_ptr(storage.inverse_modelview);
        }
        if (normal)
        {
            const glm::mat4 normal_matrix = glm::transpose(storage.inverse_modelview);
            const F32* norm = glm::value_ptr(normal_matrix);
            const F32 norm_mat[] =
            {
                norm[0], norm[1], norm[2],
                norm[4], norm[5], norm[6],
                norm[8], norm[9], norm[10]
            };
            memcpy(storage.normal, norm_mat, sizeof(norm_mat));
            draw.normal = storage.normal;
        }
    }
    if (shader->getUniformLocation(LLShaderMgr::MODELVIEW_PROJECTION_MATRIX) > -1)
    {
        storage.modelview_projection = mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]] * modelview;
        draw.modelview_projection = glm::value_ptr(storage.modelview_projection);
    }
}

void LLRender::translatef(const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::translate(mMatrix[mMatrixMode][mMatIdx[mMatrixMode]], glm::vec3(x, y, z));
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::scalef(const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::scale(mMatrix[mMatrixMode][mMatIdx[mMatrixMode]], glm::vec3(x, y, z));
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] *= glm::ortho(left, right, bottom, top, zNear, zFar);
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::rotatef(const GLfloat& a, const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    flush();

    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::rotate(mMatrix[mMatrixMode][mMatIdx[mMatrixMode]], glm::radians(a), glm::vec3(x,y,z));
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::pushMatrix()
{
    flush();

    {
        if (mMatIdx[mMatrixMode] < LL_MATRIX_STACK_DEPTH-1)
        {
            mMatrix[mMatrixMode][mMatIdx[mMatrixMode]+1] = mMatrix[mMatrixMode][mMatIdx[mMatrixMode]];
            ++mMatIdx[mMatrixMode];
        }
        else
        {
            LL_WARNS() << "Matrix stack overflow." << LL_ENDL;
        }
    }
}

void LLRender::popMatrix()
{
    flush();
    {
        if (mMatIdx[mMatrixMode] > 0)
        {
            --mMatIdx[mMatrixMode];
            mMatHash[mMatrixMode]++;
        }
        else
        {
            LL_WARNS() << "Matrix stack underflow." << LL_ENDL;
        }
    }
}

void LLRender::loadMatrix(const GLfloat* m)
{
    flush();
    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::make_mat4((GLfloat*) m);
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::multMatrix(const GLfloat* m)
{
    flush();
    {
        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] *= glm::make_mat4(m);
        mMatHash[mMatrixMode]++;
    }
}

void LLRender::matrixMode(eMatrixMode mode)
{
    if (mode == MM_TEXTURE)
    {
        U32 tex_index = gGL.getCurrentTexUnitIndex();
        // the shaders don't actually reference anything beyond texture_matrix0/1 outside of terrain rendering
        llassert(tex_index <= 3);
        mode = eMatrixMode(MM_TEXTURE0 + tex_index);
        if (mode > MM_TEXTURE3)
        {
            // getCurrentTexUnitIndex() can go as high as 32 (LL_NUM_TEXTURE_LAYERS)
            // Large value will result in a crash at mMatrix
            LL_WARNS_ONCE() << "Attempted to assign matrix mode out of bounds: " << mode << LL_ENDL;
            mode = MM_TEXTURE0;
        }
    }

    mMatrixMode = mode;
}

LLRender::eMatrixMode LLRender::getMatrixMode()
{
    if (mMatrixMode >= MM_TEXTURE0 && mMatrixMode <= MM_TEXTURE3)
    { //always return MM_TEXTURE if current matrix mode points at any texture matrix
        return MM_TEXTURE;
    }

    return mMatrixMode;
}


void LLRender::loadIdentity()
{
    flush();

    {
        llassert_always(mMatrixMode < NUM_MATRIX_MODES) ;

        mMatrix[mMatrixMode][mMatIdx[mMatrixMode]] = glm::identity<glm::mat4>();
        mMatHash[mMatrixMode]++;
    }
}

const glm::mat4& LLRender::getModelviewMatrix()
{
    return mMatrix[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]];
}

const glm::mat4& LLRender::getProjectionMatrix()
{
    return mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]];
}

void LLRender::translateUI(F32 x, F32 y, F32 z)
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "Need to push a UI translation frame before offsetting" << LL_ENDL;
    }

    mUIOffset.back().add(LLVector4a(x, y, z));
}

void LLRender::scaleUI(F32 x, F32 y, F32 z)
{
    if (mUIScale.empty())
    {
        LL_ERRS() << "Need to push a UI transformation frame before scaling." << LL_ENDL;
    }

    mUIScale.back().mul(LLVector4a(x, y, z));
}

void LLRender::pushUIMatrix()
{
    if (mUIOffset.empty())
    {
        mUIOffset.emplace_back(0.f);
    }
    else
    {
        mUIOffset.push_back(mUIOffset.back());
    }

    if (mUIScale.empty())
    {
        mUIScale.emplace_back(1.f);
    }
    else
    {
        mUIScale.push_back(mUIScale.back());
    }
}

void LLRender::popUIMatrix()
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "UI offset stack blown." << LL_ENDL;
    }
    mUIOffset.pop_back();
    mUIScale.pop_back();
}

LLVector3 LLRender::getUITranslation()
{
    if (mUIOffset.empty())
    {
        return LLVector3::zero;
    }

    return LLVector3(mUIOffset.back().getF32ptr());
}

LLVector3 LLRender::getUIScale()
{
    if (mUIScale.empty())
    {
        return LLVector3::all_one;
    }

    return LLVector3(mUIScale.back().getF32ptr());
}


void LLRender::loadUIIdentity()
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "Need to push UI translation frame before clearing offset." << LL_ENDL;
    }

    mUIOffset.back().clear();
    mUIScale.back().splat(1);
}

void LLRender::setColorMask(bool writeColor, bool writeAlpha)
{
    setColorMask(writeColor, writeColor, writeColor, writeAlpha);
}

void LLRender::setColorMask(bool writeColorR, bool writeColorG, bool writeColorB, bool writeAlpha)
{
    flush();

    if (mCurrColorMask[0] != writeColorR ||
        mCurrColorMask[1] != writeColorG ||
        mCurrColorMask[2] != writeColorB ||
        mCurrColorMask[3] != writeAlpha)
    {
        mCurrColorMask[0] = writeColorR;
        mCurrColorMask[1] = writeColorG;
        mCurrColorMask[2] = writeColorB;
        mCurrColorMask[3] = writeAlpha;

        // <plus> native state (research/39 N4); research/207 T4: no GL to fall
        // back on (LLVertexBuffer::initClass stops without the hook)
        if (LLRHI::sColorMask)
        {
            LLRHI::sColorMask(writeColorR, writeColorG, writeColorB, writeAlpha);
        }
        // </plus>
    }
}

void LLRender::setSceneBlendType(eBlendType type)
{
    switch (type)
    {
        case BT_ALPHA:
            blendFunc(BF_SOURCE_ALPHA, BF_ONE_MINUS_SOURCE_ALPHA);
            break;
        case BT_ADD:
            blendFunc(BF_ONE, BF_ONE);
            break;
        case BT_ADD_WITH_ALPHA:
            blendFunc(BF_SOURCE_ALPHA, BF_ONE);
            break;
        case BT_MULT:
            blendFunc(BF_DEST_COLOR, BF_ZERO);
            break;
        case BT_MULT_ALPHA:
            blendFunc(BF_DEST_ALPHA, BF_ZERO);
            break;
        case BT_MULT_X2:
            blendFunc(BF_DEST_COLOR, BF_SOURCE_COLOR);
            break;
        case BT_REPLACE:
            blendFunc(BF_ONE, BF_ZERO);
            break;
        default:
            LL_ERRS() << "Unknown Scene Blend Type: " << type << LL_ENDL;
            break;
    }
}

void LLRender::blendFunc(eBlendFactor sfactor, eBlendFactor dfactor)
{
    llassert(sfactor < BF_UNDEF);
    llassert(dfactor < BF_UNDEF);
    if (mCurrBlendColorSFactor != sfactor || mCurrBlendColorDFactor != dfactor ||
        mCurrBlendAlphaSFactor != sfactor || mCurrBlendAlphaDFactor != dfactor)
    {
        mCurrBlendColorSFactor = sfactor;
        mCurrBlendAlphaSFactor = sfactor;
        mCurrBlendColorDFactor = dfactor;
        mCurrBlendAlphaDFactor = dfactor;
        flush();
        // <plus> native state (research/39 N4); research/207 T4: no GL to fall back on
        if (LLRHI::sBlendFunc)
        {
            LLRHI::sBlendFunc(sGLBlendFactor[sfactor], sGLBlendFactor[dfactor], sGLBlendFactor[sfactor], sGLBlendFactor[dfactor]);
        }
        // </plus>
    }
}

void LLRender::blendFunc(eBlendFactor color_sfactor, eBlendFactor color_dfactor,
             eBlendFactor alpha_sfactor, eBlendFactor alpha_dfactor)
{
    llassert(color_sfactor < BF_UNDEF);
    llassert(color_dfactor < BF_UNDEF);
    llassert(alpha_sfactor < BF_UNDEF);
    llassert(alpha_dfactor < BF_UNDEF);

    if (mCurrBlendColorSFactor != color_sfactor || mCurrBlendColorDFactor != color_dfactor ||
        mCurrBlendAlphaSFactor != alpha_sfactor || mCurrBlendAlphaDFactor != alpha_dfactor)
    {
        mCurrBlendColorSFactor = color_sfactor;
        mCurrBlendAlphaSFactor = alpha_sfactor;
        mCurrBlendColorDFactor = color_dfactor;
        mCurrBlendAlphaDFactor = alpha_dfactor;
        flush();

        // <plus> native state (research/39 N4); research/207 T4: no GL to fall back on
        if (LLRHI::sBlendFunc)
        {
            LLRHI::sBlendFunc(sGLBlendFactor[color_sfactor], sGLBlendFactor[color_dfactor],
                              sGLBlendFactor[alpha_sfactor], sGLBlendFactor[alpha_dfactor]);
        }
        // </plus>
    }
}

LLTexUnit* LLRender::getTexUnit(U32 index)
{
    if (index < mTexUnits.size())
    {
        return &mTexUnits[index];
    }
    else
    {
        LL_DEBUGS() << "Non-existing texture unit layer requested: " << index << LL_ENDL;
        return &mDummyTexUnit;
    }
}

LLLightState* LLRender::getLight(U32 index)
{
    if (index < mLightState.size())
    {
        return &mLightState[index];
    }

    return NULL;
}

void LLRender::setAmbientLightColor(const LLColor4& color)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (color != mAmbientLightColor)
    {
        ++mLightHash;
        mAmbientLightColor = color;
    }
}

bool LLRender::verifyTexUnitActive(U32 unitToVerify)
{
    if (mCurrTextureUnitIndex == unitToVerify)
    {
        return true;
    }
    else
    {
        LL_WARNS() << "TexUnit currently active: " << mCurrTextureUnitIndex << " (expecting " << unitToVerify << ")" << LL_ENDL;
        return false;
    }
}

void LLRender::clearErrors()
{
    while (glGetError())
    {
        //loop until no more error flags left
    }
}

void LLRender::beginList(std::list<LLVertexBufferData> *list)
{
    if (mBufferDataList)
    {
        LL_ERRS() << "beginList called while another list is open." << LL_ENDL;
    }
    llassert(LLGLSLShader::sCurBoundShaderPtr == &gUIProgram);
    flush();
    mBufferDataList = list;
}

void LLRender::endList()
{
    if (mBufferDataList)
    {
        flush();
        mBufferDataList = nullptr;
    }
    else
    {
        llassert(false); // endList called without an open list
    }
}

void LLRender::begin(const GLuint& mode)
{
    if (mode != mMode)
    {
        if (mMode == LLRender::LINES ||
            mMode == LLRender::TRIANGLES ||
            mMode == LLRender::POINTS)
        {
            flush();
        }
        else if (mCount != 0)
        {
            LL_ERRS() << "gGL.begin() called redundantly." << LL_ENDL;
        }

        mMode = mode;
    }
}

void LLRender::end()
{
    if (mCount == 0)
    {
        return;
        //IMM_ERRS << "GL begin and end called with no vertices specified." << LL_ENDL;
    }

    if ((mMode != LLRender::LINES &&
        mMode != LLRender::TRIANGLES &&
        mMode != LLRender::POINTS) ||
        mCount > 2048)
    {
        flush();
    }
}

// LLRHI native (research/137 N9) begin
// The flush's vertices straight into the frame's ring (LLRHI::sImmediateDraw,
// llrhiimmediate.h): the vertex block genBuffer's LLVertexBuffer would hold
// (LLVertexBuffer::calcOffsets for the program's attributes) with the
// attributes getAttributes gives for it, so the draw reads the same values
// through the same pipeline. Only for programs whose every attribute comes
// from LLRender's arrays (position, its w as the texture index, texcoord 0,
// color); the others go the old way (the rest of their vertex block is what
// allocateBuffer left there). False: nothing was drawn.
static bool flush_to_ring(LLRender& render, U32 mode, U32 count, const LLVector4a* vertices, const LLVector2* texcoords,
                          const LLColor4U* colors)
{
    constexpr U32 ring_attributes = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXTURE_INDEX | LLVertexBuffer::MAP_TEXCOORD0 |
                                    LLVertexBuffer::MAP_COLOR;
    const U32 mask = LLGLSLShader::sCurBoundShaderPtr->mAttributeMask;
    if (!count || mode >= LLRender::NUM_MODES || !(mask & LLVertexBuffer::MAP_VERTEX) || (mask & ~ring_attributes))
    {
        return false;
    }
    U32 offsets[LLVertexBuffer::TYPE_MAX] = {};
    LLRHI::ImmediateDraw draw;
    draw.mode = LLVertexBuffer::sGLMode[mode];
    draw.vertex_count = count;
    draw.bytes = LLVertexBuffer::calcOffsets(mask, offsets, count);
    auto piece = [&draw](const void* data, U32 bytes, U32 offset)
    {
        draw.pieces[draw.piece_count++] = { data, bytes, offset };
    };
    auto attribute = [&draw](U32 location, U32 size, U32 type, bool normalized, bool integer, U32 stride, U32 offset)
    {
        draw.attributes[draw.attribute_count++] = { location, size, type, normalized, integer, stride, offset };
    };
    // In LLVertexBuffer::getAttributes' order, with its formats and strides.
    piece(vertices, count * (U32)sizeof(LLVector4a), offsets[LLVertexBuffer::TYPE_VERTEX]);
    if (mask & LLVertexBuffer::MAP_TEXCOORD0)
    {
        piece(texcoords, count * (U32)sizeof(LLVector2), offsets[LLVertexBuffer::TYPE_TEXCOORD0]);
        attribute(LLVertexBuffer::TYPE_TEXCOORD0, 2, GL_FLOAT, false, false, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_TEXCOORD0],
                  offsets[LLVertexBuffer::TYPE_TEXCOORD0]);
    }
    if (mask & LLVertexBuffer::MAP_COLOR)
    {
        piece(colors, count * (U32)sizeof(LLColor4U), offsets[LLVertexBuffer::TYPE_COLOR]);
        attribute(LLVertexBuffer::TYPE_COLOR, 4, GL_UNSIGNED_BYTE, true, false, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_COLOR],
                  offsets[LLVertexBuffer::TYPE_COLOR]);
    }
    if (mask & LLVertexBuffer::MAP_TEXTURE_INDEX)
    {
        attribute(LLVertexBuffer::TYPE_TEXTURE_INDEX, 1, GL_UNSIGNED_INT, false, true, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_VERTEX],
                  offsets[LLVertexBuffer::TYPE_VERTEX] + 12);
    }
    attribute(LLVertexBuffer::TYPE_VERTEX, 3, GL_FLOAT, false, false, LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_VERTEX],
              offsets[LLVertexBuffer::TYPE_VERTEX]);
    render.syncMatrices();  // as drawArrays does before its draw
    return LLRHI::sImmediateDraw(draw);
}
// LLRHI native (research/137 N9) end

void LLRender::flush()
{
    STOP_GLERROR;
    if (mCount > 0)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
        llassert_always(LLGLSLShader::sCurBoundShaderPtr != nullptr);

        if (!mUIOffset.empty())
        {
            sUICalls++;
            sUIVerts += mCount;
        }
        // <plus> frame cost (research/95)
        count_immediate(LLRHI::sImmediate.flushes);
        // </plus>

        //store mCount in a local variable to avoid re-entrance (drawArrays may call flush)
        U32 count = mCount;

        if (mMode == LLRender::TRIANGLES)
        {
            if (mCount%3 != 0)
            {
            count -= (mCount % 3);
            LL_WARNS() << "Incomplete triangle requested." << LL_ENDL;
            }
        }

        if (mMode == LLRender::LINES)
        {
            if (mCount%2 != 0)
            {
                count -= (mCount % 2);
                LL_WARNS() << "Incomplete line requested." << LL_ENDL;
            }
        }

        mCount = 0;

        if (mBuffer && !mBufferDataList && LLRHI::immediateRing() && // LLRHI native (research/137 N9)
            flush_to_ring(*this, mMode, count, mVerticesp.get(), mTexcoordsp.get(), mColorsp.get())) // LLRHI native (research/137 N9)
        {
            resetStriders(count);
            return;
        }

        if (mBuffer)
        {

            LLVertexBuffer *vb;

            U32 attribute_mask = LLGLSLShader::sCurBoundShaderPtr->mAttributeMask;
            // <plus> genBuffer() always writes the positions, so the buffer always has them. A
            // program without attributes (mask 0) made a 0-attribute buffer, and the cache below
            // (keyed by the positions only) then handed it to the next program drawing the same
            // vertices: "Attribute mask mismatch" with data_mask 0x1, mTypeMask 0x0 after a
            // shader reload (the BRDF LUT's full-screen quad in createLUTBuffers) (fix-round106)
            if (LL_UNLIKELY(!(attribute_mask & LLVertexBuffer::MAP_VERTEX)))
            {   // diagnosis: which program has no position attribute (once per program)
                LL_WARNS_ONCE() << "Immediate draw with a program without a position attribute: "
                                << LLGLSLShader::sCurBoundShaderPtr->mName << LL_ENDL;
            }
            attribute_mask |= LLVertexBuffer::MAP_VERTEX;
            // </plus>

            if (mBufferDataList)
            {
                vb = genBuffer(attribute_mask, count);
                mBufferDataList->emplace_back(
                    vb,
                    mMode,
                    count,
                    gGL.getTexUnit(0)->mCurrTexture,
                    mMatrix[MM_MODELVIEW][mMatIdx[MM_MODELVIEW]],
                    mMatrix[MM_PROJECTION][mMatIdx[MM_PROJECTION]],
                    mMatrix[MM_TEXTURE0][mMatIdx[MM_TEXTURE0]]
                    );
            }
            else
            {
                vb = bufferfromCache(attribute_mask, count);
            }

            drawBuffer(vb, mMode, count);
        }
        else
        {
            // mBuffer is present in main thread and not present in an image thread
            LL_ERRS() << "A flush call from outside main rendering thread" << LL_ENDL;
        }

        resetStriders(count);
    }
}

LLVertexBuffer* LLRender::bufferfromCache(U32 attribute_mask, U32 count)
{
    LLVertexBuffer *vb = nullptr;
    HBXXH64 hash;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache hash");

        hash.update((U8*)mVerticesp.get(), count * sizeof(LLVector4a));
        hash.update((U8*)&attribute_mask, sizeof(attribute_mask)); // <plus> buffers of other layouts never shared (fix-round106) </plus>
        if (attribute_mask & LLVertexBuffer::MAP_TEXCOORD0)
        {
            hash.update((U8*)mTexcoordsp.get(), count * sizeof(LLVector2));
        }

        if (attribute_mask & LLVertexBuffer::MAP_COLOR)
        {
            hash.update((U8*)mColorsp.get(), count * sizeof(LLColor4U));
        }

        hash.finalize();
    }

    U64 vhash = hash.digest();

    // check the VB cache before making a new vertex buffer
    // This is a giant hack to deal with (mostly) our terrible UI rendering code
    // that was built on top of OpenGL immediate mode.  Huge performance wins
    // can be had by not uploading geometry to VRAM unless absolutely necessary.
    // Most of our usage of the "immediate mode" style draw calls is actually
    // sending the same geometry over and over again.
    // To leverage this, we maintain a running hash of the vertex stream being
    // built up before a flush, and then check that hash against a VB
    // cache just before creating a vertex buffer in VRAM
    std::unordered_map<U64, LLVBCache>::iterator cache = mVBCache.find(vhash);

    if (cache != mVBCache.end())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache hit");
        // cache hit, just use the cached buffer
        vb = cache->second.vb;
        cache->second.touched = std::chrono::steady_clock::now();
    }
    else
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache miss");
        // <plus> frame cost (research/95); research/155 T0b: always counted for the VKPerf line
        count_immediate(LLRHI::sImmediate.cache_misses);
        LLRHI::sVBCacheMisses.fetch_add(1, std::memory_order_relaxed);
        // </plus>
        vb = genBuffer(attribute_mask, count);

        mVBCache[vhash] = { vb , std::chrono::steady_clock::now() };

        static U32 miss_count = 0;
        miss_count++;
        if (miss_count > 1024)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb cache clean");
            miss_count = 0;
            auto now = std::chrono::steady_clock::now();

            using namespace std::chrono_literals;
            // every 1024 misses, clean the cache of any VBs that haven't been touched in the last second
            for (std::unordered_map<U64, LLVBCache>::iterator iter = mVBCache.begin(); iter != mVBCache.end(); )
            {
                if (now - iter->second.touched > 1s)
                {
                    iter = mVBCache.erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }
    }
    return vb;
}

LLVertexBuffer* LLRender::genBuffer(U32 attribute_mask, S32 count)
{
    LLVertexBuffer * vb = new LLVertexBuffer(attribute_mask);
    vb->allocateBuffer(count, 0);

    // Non-Apple path uses glBufferSubData inside setXxxData, so the VBO
    // must already be bound. On Apple, the VBO is lazily created in
    // _unmapBuffer (LLAppleVBOPool); calling setBuffer() here would bind
    // mGLBuffer == 0 and then setupVertexBuffer would issue
    // glVertexAttribIPointer with a non-null offset against no bound
    // GL_ARRAY_BUFFER -> GL_INVALID_OPERATION in core profile.
    if (!gGLManager.mIsApple)
    {
        vb->setBuffer();
    }

    vb->setPositionData(mVerticesp.get());

    if (attribute_mask & LLVertexBuffer::MAP_TEXCOORD0)
    {
        vb->setTexCoord0Data(mTexcoordsp.get());
    }

    if (attribute_mask & LLVertexBuffer::MAP_COLOR)
    {
        vb->setColorData(mColorsp.get());
    }

#if LL_DARWIN
    // unmapBuffer creates the GL buffer, uploads, and leaves it bound,
    // drawBuffer's later setBuffer() then runs setupVertexBuffer against
    // a valid VBO.
    vb->unmapBuffer();
#endif
    vb->unbind();

    return vb;
}

void LLRender::drawBuffer(LLVertexBuffer* vb, U32 mode, S32 count)
{
    vb->setBuffer();
    vb->drawArrays(mode, 0, count);
}

void LLRender::resetStriders(S32 count)
{
    mVerticesp[0] = mVerticesp[count];
    mTexcoordsp[0] = mTexcoordsp[count];
    mColorsp[0] = mColorsp[count];

    mCount = 0;
}

void LLRender::vertex3f(const GLfloat& x, const GLfloat& y, const GLfloat& z)
{
    //the range of mVerticesp, mColorsp and mTexcoordsp is [0, 4095]
    if (mCount > 2048)
    { //break when buffer gets reasonably full to keep GL command buffers happy and avoid overflow below
        switch (mMode)
        {
            case LLRender::POINTS: flush(); break;
            case LLRender::TRIANGLES: if (mCount%3==0) flush(); break;
            case LLRender::LINES: if (mCount%2 == 0) flush(); break;
        }
    }

    if (mCount > 4094)
    {
    //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    if (mUIOffset.empty())
    {
        mVerticesp[mCount].set(x,y,z);
    }
    else
    {
        LLVector4a vert(x, y, z);
        vert.add(mUIOffset.back());
        vert.mul(mUIScale.back());
        mVerticesp[mCount] = vert;
    }

    mCount++;
    mVerticesp[mCount] = mVerticesp[mCount-1];
    mColorsp[mCount] = mColorsp[mCount-1];
    mTexcoordsp[mCount] = mTexcoordsp[mCount-1];
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, S32 vert_count)
{
    if (mCount + vert_count > 4094)
    {
        //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    for (S32 i = 0; i < vert_count; i++)
    {
        mVerticesp[mCount] = verts[i];

        mCount++;
        mTexcoordsp[mCount] = mTexcoordsp[mCount-1];
        mColorsp[mCount] = mColorsp[mCount-1];
    }

    if( mCount > 0 ) // ND: Guard against crashes if mCount is zero, yes it can happen
        mVerticesp[mCount] = mVerticesp[mCount-1];
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, S32 vert_count)
{
    if (mCount + vert_count > 4094)
    {
        //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    for (S32 i = 0; i < vert_count; i++)
    {
        mVerticesp[mCount] = verts[i];
        mTexcoordsp[mCount] = uvs[i];

        mCount++;
        mColorsp[mCount] = mColorsp[mCount-1];
    }

    if (mCount > 0)
    {
        mVerticesp[mCount] = mVerticesp[mCount - 1];
        mTexcoordsp[mCount] = mTexcoordsp[mCount - 1];
    }
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, LLColor4U* colors, S32 vert_count)
{
    if (mCount + vert_count > 4094)
    {
        //  LL_WARNS() << "GL immediate mode overflow.  Some geometry not drawn." << LL_ENDL;
        return;
    }

    for (S32 i = 0; i < vert_count; i++)
    {
        mVerticesp[mCount] = verts[i];
        mTexcoordsp[mCount] = uvs[i];
        mColorsp[mCount] = colors[i];

        mCount++;
    }

    if (mCount > 0)
    {
        mVerticesp[mCount] = mVerticesp[mCount - 1];
        mTexcoordsp[mCount] = mTexcoordsp[mCount - 1];
        mColorsp[mCount] = mColorsp[mCount - 1];
    }
}

void LLRender::vertex2i(const GLint& x, const GLint& y)
{
    vertex3f((GLfloat) x, (GLfloat) y, 0);
}

void LLRender::vertex2f(const GLfloat& x, const GLfloat& y)
{
    vertex3f(x,y,0);
}

void LLRender::vertex2fv(const GLfloat* v)
{
    vertex3f(v[0], v[1], 0);
}

void LLRender::vertex3fv(const GLfloat* v)
{
    vertex3f(v[0], v[1], v[2]);
}

void LLRender::texCoord2f(const GLfloat& x, const GLfloat& y)
{
    mTexcoordsp[mCount] = LLVector2(x,y);
}

void LLRender::texCoord2i(const GLint& x, const GLint& y)
{
    texCoord2f((GLfloat) x, (GLfloat) y);
}

void LLRender::texCoord2fv(const GLfloat* tc)
{
    texCoord2f(tc[0], tc[1]);
}

void LLRender::color4ub(const GLubyte& r, const GLubyte& g, const GLubyte& b, const GLubyte& a)
{
    if (!LLGLSLShader::sCurBoundShaderPtr || LLGLSLShader::sCurBoundShaderPtr->mAttributeMask & LLVertexBuffer::MAP_COLOR)
    {
        mColorsp[mCount] = LLColor4U(r,g,b,a);
    }
    else
    { //not using shaders or shader reads color from a uniform
        diffuseColor4ub(r,g,b,a);
    }
}
void LLRender::color4ubv(const GLubyte* c)
{
    color4ub(c[0], c[1], c[2], c[3]);
}

void LLRender::color4f(const GLfloat& r, const GLfloat& g, const GLfloat& b, const GLfloat& a)
{
    color4ub((GLubyte) (llclamp(r, 0.f, 1.f)*255),
        (GLubyte) (llclamp(g, 0.f, 1.f)*255),
        (GLubyte) (llclamp(b, 0.f, 1.f)*255),
        (GLubyte) (llclamp(a, 0.f, 1.f)*255));
}

void LLRender::color4fv(const GLfloat* c)
{
    color4f(c[0],c[1],c[2],c[3]);
}

void LLRender::color3f(const GLfloat& r, const GLfloat& g, const GLfloat& b)
{
    color4f(r,g,b,1);
}

void LLRender::color3fv(const GLfloat* c)
{
    color4f(c[0],c[1],c[2],1);
}

void LLRender::diffuseColor3f(F32 r, F32 g, F32 b)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, r,g,b,1.f);
    }
}

void LLRender::diffuseColor3fv(const F32* c)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, c[0], c[1], c[2], 1.f);
    }
}

void LLRender::diffuseColor4f(F32 r, F32 g, F32 b, F32 a)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, r,g,b,a);
    }
}

void LLRender::diffuseColor4fv(const F32* c)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, c);
    }
}

void LLRender::diffuseColor4ubv(const U8* c)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, c[0]/255.f, c[1]/255.f, c[2]/255.f, c[3]/255.f);
    }
}

void LLRender::diffuseColor4ub(U8 r, U8 g, U8 b, U8 a)
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    llassert(shader != NULL);

    if (shader)
    {
        shader->uniform4f(LLShaderMgr::DIFFUSE_COLOR, r/255.f, g/255.f, b/255.f, a/255.f);
    }
}

void LLRender::setLineWidth(F32 width)
{
    gGL.flush();

    width = llclamp(width, gGLManager.mAliasedLineRange[0], gGLManager.mAliasedLineRange[1]);
    if(mLineWidth != width)
    {
        mLineWidth = width;
        // <plus> native state (research/39 N4); research/207 T4: no GL to fall back on
        if (LLRHI::sLineWidth)
        {
            LLRHI::sLineWidth(width);
        }
        // </plus>
    }
}

void LLRender::cullFace(U32 mode)
{
    // <plus> native state (research/39 N4); research/207 T4: no GL to fall
    // back on, and the face kept for the state copy (still sent every time)
    mCurrCullFace = mode;
    if (LLRHI::sCullFace)
    {
        LLRHI::sCullFace(mode);
    }
    // </plus>
}

// <plus> research/207 T4: the state copy as a pass state (see llrender.h)
void LLRender::readStateCopy(LLRHI::PassState& out) const
{
    out = LLRHI::PassState();
    out.blend = LLGLState::capCopy(GL_BLEND);
    out.blend_src_rgb = sGLBlendFactor[mCurrBlendColorSFactor];
    out.blend_dst_rgb = sGLBlendFactor[mCurrBlendColorDFactor];
    out.blend_src_a = sGLBlendFactor[mCurrBlendAlphaSFactor];
    out.blend_dst_a = sGLBlendFactor[mCurrBlendAlphaDFactor];
    // (equations: LLRender has none to set; LLRHI::PassState's default is GL_FUNC_ADD, the backend's too)
    for (U32 i = 0; i < 4; ++i)
    {
        out.color_mask[i] = mCurrColorMask[i];
    }
    LLGLDepthTest::readCopy(out.depth_test, out.depth_write, out.depth_func);
    out.cull = LLGLState::capCopy(GL_CULL_FACE);
    out.cull_face = mCurrCullFace;
    out.stencil = LLGLState::capCopy(GL_STENCIL_TEST);
    out.polygon_offset = LLGLState::capCopy(GL_POLYGON_OFFSET_FILL);
    out.depth_clamp = LLGLState::capCopy(GL_DEPTH_CLAMP);
    out.program = LLGLSLShader::sCurBoundShader;
}

void LLRender::writeStateCopy(const LLRHI::PassState& values, U32 fields)
{
    fields &= LLRHI::FIELDS_WRITTEN;
    if (!fields)
    {
        return;
    }
    flush();    // the immediate-mode vertices still waiting are drawn in the state they were made in
    if (fields & LLRHI::FIELD_BLEND_FUNC)
    {
        eBlendFactor color_s, color_d, alpha_s, alpha_d;
        if (blend_factor_of(values.blend_src_rgb, color_s) && blend_factor_of(values.blend_dst_rgb, color_d) &&
            blend_factor_of(values.blend_src_a, alpha_s) && blend_factor_of(values.blend_dst_a, alpha_d))
        {
            blendFunc(color_s, color_d, alpha_s, alpha_d);
        }
        else
        {
            LL_WARNS_ONCE("PassState") << "write-through: a blend factor LLRender has no name for; the blend function is left as it is"
                                       << LL_ENDL;
        }
    }
    if (fields & LLRHI::FIELD_BLEND)
    {
        LLGLState::writeCap(GL_BLEND, values.blend);
    }
    if (fields & LLRHI::FIELD_COLOR_MASK)
    {
        setColorMask(values.color_mask[0], values.color_mask[1], values.color_mask[2], values.color_mask[3]);
    }
    if (fields & LLRHI::FIELD_DEPTH_TEST)
    {
        LLGLDepthTest::writeTest(values.depth_test);
    }
    if (fields & LLRHI::FIELD_DEPTH_WRITE)
    {
        LLGLDepthTest::writeMask(values.depth_write);
    }
    if (fields & LLRHI::FIELD_DEPTH_FUNC)
    {
        LLGLDepthTest::writeFunc(values.depth_func);
    }
    if ((fields & LLRHI::FIELD_CULL_FACE) && values.cull_face != mCurrCullFace)
    {
        cullFace(values.cull_face);
    }
    if (fields & LLRHI::FIELD_CULL)
    {
        LLGLState::writeCap(GL_CULL_FACE, values.cull);
    }
    if (fields & LLRHI::FIELD_STENCIL)
    {
        LLGLState::writeCap(GL_STENCIL_TEST, values.stencil);
    }
    if (fields & LLRHI::FIELD_POLYGON_OFFSET)
    {
        LLGLState::writeCap(GL_POLYGON_OFFSET_FILL, values.polygon_offset);
    }
    if (fields & LLRHI::FIELD_DEPTH_CLAMP)
    {
        LLGLState::writeCap(GL_DEPTH_CLAMP, values.depth_clamp);
    }
}
// </plus>

void LLRender::debugTexUnits(void)
{
    LL_INFOS("TextureUnit") << "Active TexUnit: " << mCurrTextureUnitIndex << LL_ENDL;
    std::string active_enabled = "false";
    for (U32 i = 0; i < mTexUnits.size(); i++)
    {
        if (getTexUnit(i)->mCurrTexType != LLTexUnit::TT_NONE)
        {
            if (i == mCurrTextureUnitIndex) active_enabled = "true";
            LL_INFOS("TextureUnit") << "TexUnit: " << i << " Enabled" << LL_ENDL;
            LL_INFOS("TextureUnit") << "Enabled As: " ;
            switch (getTexUnit(i)->mCurrTexType)
            {
                case LLTexUnit::TT_TEXTURE:
                    LL_CONT << "Texture 2D";
                    break;
                case LLTexUnit::TT_RECT_TEXTURE:
                    LL_CONT << "Texture Rectangle";
                    break;
                case LLTexUnit::TT_CUBE_MAP:
                    LL_CONT << "Cube Map";
                    break;
                default:
                    LL_CONT << "ARGH!!! NONE!";
                    break;
            }
            LL_CONT << ", Texture Bound: " << getTexUnit(i)->mCurrTexture << LL_ENDL;
        }
    }
    LL_INFOS("TextureUnit") << "Active TexUnit Enabled : " << active_enabled << LL_ENDL;
}

glm::mat4 get_current_modelview()
{
    return glm::make_mat4(gGLModelView);
}

glm::mat4 get_current_projection()
{
    return glm::make_mat4(gGLProjection);
}

glm::mat4 get_last_modelview()
{
    return glm::make_mat4(gGLLastModelView);
}

glm::mat4 get_last_projection()
{
    return glm::make_mat4(gGLLastProjection);
}

void copy_matrix(const glm::mat4& src, F32* dst)
{
    auto matp = glm::value_ptr(src);
    for (U32 i = 0; i < 16; i++)
    {
        dst[i] = matp[i];
    }
}

void set_current_modelview(const glm::mat4& mat)
{
    copy_matrix(mat, gGLModelView);
}

void set_current_projection(const glm::mat4& mat)
{
    copy_matrix(mat, gGLProjection);
}

void set_last_modelview(const glm::mat4& mat)
{
    copy_matrix(mat, gGLLastModelView);
}

void set_last_projection(const glm::mat4& mat)
{
    copy_matrix(mat, gGLLastProjection);
}

glm::vec3 mul_mat4_vec3(const glm::mat4& mat, const glm::vec3& vec)
{
#if 1 // SIMD path results in strange crashes. Fall back to scalar for now.
    const float w = vec[0] * mat[0][3] + vec[1] * mat[1][3] + vec[2] * mat[2][3] + mat[3][3];
    return glm::vec3(
       (vec[0] * mat[0][0] + vec[1] * mat[1][0] + vec[2] * mat[2][0] + mat[3][0]) / w,
       (vec[0] * mat[0][1] + vec[1] * mat[1][1] + vec[2] * mat[2][1] + mat[3][1]) / w,
       (vec[0] * mat[0][2] + vec[1] * mat[1][2] + vec[2] * mat[2][2] + mat[3][2]) / w
    );
#else
    LLVector4a x, y, z, s, t, p, q;

    x.splat(vec.x);
    y.splat(vec.y);
    z.splat(vec.z);

    s.splat<3>(mat[0].data);
    t.splat<3>(mat[1].data);
    p.splat<3>(mat[2].data);
    q.splat<3>(mat[3].data);

    s.mul(x);
    t.mul(y);
    p.mul(z);
    q.add(s);
    t.add(p);
    q.add(t);

    x.mul(mat[0].data);
    y.mul(mat[1].data);
    z.mul(mat[2].data);

    x.add(y);
    z.add(mat[3].data);
    LLVector4a res;
    res.load3(glm::value_ptr(vec));
    res.setAdd(x, z);
    res.div(q);
    return glm::make_vec3(res.getF32ptr());
#endif
}
