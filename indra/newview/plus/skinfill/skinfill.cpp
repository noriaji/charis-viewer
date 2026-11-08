/**
 * @file skinfill.cpp
 * @brief Softer shadows on faces: a fill light and weaker ambient occlusion
 *        on the skin only. Registers with the plus foundation
 *        (research/110-skin-fill.md).
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

// Faces in Second Life take deep shadows (under the brows, in the eye
// sockets, under the nose and chin, from hair and hats) that make them look
// harsh. Photographers fill those with a reflector; this does the same on the
// skin pixels only, which the skin mark of plus/skinsss tells (Bakes on Mesh
// skin and system bodies, in the main view's stencil), and on the eyes in
// such a face (the mark's eye bit), which a reflector lights as well: a lit
// face with the eyes left dark looks eerie (research/110 section 14).
// Nothing else changes.
//
// PlusSkinFill: 0 off (the default), 1 weak, 2 medium, 3 strong; at least
// medium in snapshots with the snapshot boost. Vulkan only: the passes are
// recorded by the backend (llvulkan/llvkskinfill.cpp). While it is off the
// mark is not made (the main view's depth keeps its official format).
//
// Each frame, on the skin pixels:
//  1. The light map's ambient occlusion (g, the official SSAO's or GTAO's) is
//     weakened, before the official blur (which softens the skin's edge).
//     softenLight (official) then lights the skin with more of the ambient.
//  2. After the opaque lighting, a grey fill light as bright as the light
//     that casts the sun shadows is added where that light does not reach:
//     the sun's reach min(N.L, shadow) is lifted towards a shape that
//     favours faces turned to the camera (a reflector beside it), never
//     lowered, so sunlit skin stays as it is. Only shadows cast from near the
//     skin are filled (the light reaches the points about NEAR_REACH towards
//     it): a roof, a tree or a building still shades the face as before. The
//     light is taken no brighter than one that shows white as white
//     (FILL_SUN_CEILING): under a sky with a blinding sun the filled face
//     does not glow white. It is grey, the skin's own colour lifted: a
//     classic sky's yellow, green or orange sun dyed faces red and orange
//     (fix-round41). And the filled skin stays darker than the same skin in
//     full sun (SUNLIT_SHARE): under a bright sky, whose ambient lights the
//     shadows well already, the face does not come out pale.
//
// Everything that weighs the fill is continuous (research/110 section 13):
// the official shadow (the light map's r, filtered and blurred), the near
// shadow (filtered as the official shadows are, averaged over three
// distances) and the fade at the edge of the skin. The fill cannot draw
// shapes of its own; it only lifts what the official lighting left dark.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "skinfill.h"

#include "presets/presets.h"
#include "skinsss/skinsss.h"

#include "llcallbacklist.h"
#include "llenvironment.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigl.h"
#include "llrhiskinfill.h"
#include "llviewercontrol.h"
#include "pipeline.h"

#include <cstring>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace
{
    constexpr char SETTING_LEVEL[] = "PlusSkinFill";

    enum Level : S32
    {
        LEVEL_OFF = 0,
        LEVEL_WEAK = 1,
        LEVEL_MEDIUM = 2,
        LEVEL_STRONG = 3,
        LEVEL_COUNT = 4,
    };
    // Snapshots with the boost take at least this (the user's own if stronger).
    constexpr S32 SNAPSHOT_BOOST_LEVEL = LEVEL_MEDIUM;

    // Per level (research/110, section 5): how far the sun's reach in the
    // shadows is lifted towards the fill's shape, and how much of the ambient
    // occlusion is taken off the skin. Weak is a light touch (fix-round41:
    // 0.25 already lifted a face under a blue sky until it looked pale).
    struct Strength
    {
        F32 fill;
        F32 occlusion;
    };
    constexpr Strength STRENGTHS[LEVEL_COUNT] =
    {
        { 0.f,   0.f   },   // off
        { 0.15f, 0.25f },   // weak
        { 0.30f, 0.45f },   // medium
        { 0.45f, 0.65f },   // strong
    };

    // The filled skin is at most this share as bright as the same skin in
    // full sun (the sun's light plus the sky's ambient): shadows stay
    // shadows, and a face is never lifted past its own sunlit side.
    constexpr F32 SUNLIT_SHARE = 0.8f;

    // The eyes take this share of what the skin takes (the fill and the
    // weaker occlusion alike): lit by the same reflector as the face around
    // them, a little less, so they stay set in their sockets. The fill never
    // passes the sun's own light (strong lifts the shadows to 45% of it), nor
    // that share of a light that brings white to white on the screen (see
    // FILL_SUN_CEILING), so the whites of the eyes cannot be blown out.
    constexpr F32 EYE_SHARE = 0.8f;

    // The brightest light (linear, its brightest channel) the fill takes its
    // share of: the light that shows a white surface as white on the screen
    // at an exposure of 1 (the exposure the tone mapping applies,
    // plus::effectiveExposure(): RenderExposure, or 1 under a classic sky,
    // where the tone mapping takes the "no post" shader and neither
    // RenderExposure nor the exposure map does anything; fix-round39). A sky's sun may be far
    // brighter than that (a classic sky's sunlight colour goes up to 3, some
    // 13 times this once linear): its sunlit skin is blown out already, and
    // even weak's 25% of it blew out the filled shadows too, the whole face
    // glowing white (fix-round36). Over this the fill's light is scaled down
    // to it (its colour kept), so weak, medium and strong lift the shadows to
    // at most 15%, 30% and 45% of white whatever the sky; skies whose sun is
    // within it (the default ones) fill as before.
    constexpr F32 FILL_SUN_CEILING = 1.f;

    // How much the fill favours faces turned to the camera: its shape is
    // 1 - DIRECTIONALITY on faces seen edge-on, 1 on faces seen head-on.
    constexpr F32 DIRECTIONALITY = 0.5f;
    // Shadows cast from nearer the skin than this (m, towards the light; the
    // shader looks at 0.5 to 1.5 times it) are filled: past a head, hair and
    // a wide hat's brim, short of a roof or a tree. At 0.5 a brim reaching
    // past the nearest point left the face under it dark and the part below
    // filled, a bright band from the nose down (fix-round41).
    constexpr F32 NEAR_REACH = 0.8f;

    constexpr U32 SHADOW_CASCADES = LLRHI::SKIN_FILL_CASCADES;

    S32 levelIndex(S32 level)
    {
        return llclamp(level, (S32)LEVEL_OFF, (S32)LEVEL_COUNT - 1);
    }

    // The level for this frame: the setting, raised by the snapshot boost.
    S32 currentLevel()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_LEVEL, LEVEL_OFF);
        S32 out = levelIndex(level);
        if (plus::presets::snapshotBoostActive())
        {
            out = llmax(out, (S32)SNAPSHOT_BOOST_LEVEL);
        }
        return out;
    }

    bool backendReady()
    {
        return LLRHI::sSkinFill && LLRHI::sSkinFillMask && LLRHI::sSkinFillShrink && LLRHI::sSkinFillSoften &&
               LLRHI::sSkinFillOcclusion && LLRHI::sSkinFillLight;
    }

    // For plus::skinsss::addMarkUser.
    bool markWanted()
    {
        return backendReady() && currentLevel() > LEVEL_OFF;
    }

    // ------------------------------------------------------------------
    // The masks (research/110 section 13). Made on first use and kept while
    // the feature is on; never destroyed by a static destructor, which would
    // run without a GL context.
    //  - mask: R8 at full resolution, sharing the main view's depth (its
    //    stencil holds the skin mark): 1 on skin, EYE_SHARE on the eyes, 0
    //    elsewhere. For the light map pass, which cannot test the stencil (the
    //    light map has no depth).
    //  - shrunk, soft: R8 at 1/factor each way, the share of each block the
    //    mask covers (skin and eyes alike: the eyes are no hole in the face)
    //    and its blur. Sampled bilinearly, the soft mask fades both passes out
    //    towards the edge of the skin, over about two shrunk texels (some 10
    //    pixels at 1080p, the same share of the picture at any size): a patch
    //    of skin among other surfaces (a hole in a face's overlay layer) is
    //    not lit apart from them, and no edge of the fill is sharp.

    // The fade at the skin's edge: one shrunk texel per this share of the
    // screen's height (180 texels high: a factor of 6 at 1080p, 8 at 1440p).
    constexpr F32 SOFT_MASK_TEXELS_HIGH = 180.f;
    constexpr U32 MIN_SHRINK_FACTOR = 2;
    constexpr U32 MAX_SHRINK_FACTOR = 16;

    struct Masks
    {
        LLRenderTarget mask;
        LLRenderTarget shrunk;
        LLRenderTarget soft;
        U32 factor = 1;             // mask pixels per soft texel, each way
        U32 generation = 0;         // the screen buffers the mask shares the depth of
        U32 built_frame = U32_MAX;  // the frame whose skin they hold
        plus::RetryWait retry;      // after they could not be made (VRAM full)
    };

    Masks& masks()
    {
        static Masks* masks = new Masks();
        return *masks;
    }

    // Also a half-made one (an allocation that failed is not isComplete() but
    // may hold its framebuffer); release() of an empty target does nothing.
    void releaseTarget(LLRenderTarget& target)
    {
        target.release();
    }

    void releaseMasks()
    {
        Masks& m = masks();
        releaseTarget(m.mask);
        releaseTarget(m.shrunk);
        releaseTarget(m.soft);
        m.built_frame = U32_MAX;
    }

    U32 shrinkFactor(U32 height)
    {
        const U32 factor = (U32)ll_round((F32)height / SOFT_MASK_TEXELS_HIGH);
        return llclamp(factor, MIN_SHRINK_FACTOR, MAX_SHRINK_FACTOR);
    }

    // target at width x height in format, filtered as given (bound once to
    // set it, then unit 0 is left empty, not holding the new texture in the
    // middle of the lighting); false when it could not be made.
    bool allocateTarget(LLRenderTarget& target, U32 width, U32 height, U32 format, LLTexUnit::eTextureFilterOptions filter)
    {
        if (!target.allocate(width, height, format))
        {
            return false;
        }
        target.bindTexture(0, 0, filter);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        return true;
    }

    // The masks at the size of screen, the full one with its depth; false
    // when they could not be made.
    bool prepareMasks(LLRenderTarget& screen)
    {
        Masks& m = masks();
        const U32 width = screen.getWidth();
        const U32 height = screen.getHeight();
        const U32 generation = plus::skinsss::screenBuffersGeneration();
        if (m.mask.isComplete() && m.generation == generation && m.mask.getWidth() == width && m.mask.getHeight() == height)
        {
            return true;
        }
        if (!m.retry.due())
        {
            return false;   // they could not be made a moment ago: not again every frame
        }
        releaseMasks();
        const U32 factor = shrinkFactor(height);
        const U32 soft_width = (width + factor - 1) / factor;
        const U32 soft_height = (height + factor - 1) / factor;
        // The mask and the shrunk mask are read texel by texel (texelFetch);
        // the soft mask is read between its texels (bilinear: the fade is smooth).
        if (screen.isBoundInStack() || !allocateTarget(m.mask, width, height, GL_R8, LLTexUnit::TFO_POINT) ||
            !allocateTarget(m.shrunk, soft_width, soft_height, GL_R8, LLTexUnit::TFO_POINT) ||
            !allocateTarget(m.soft, soft_width, soft_height, GL_R8, LLTexUnit::TFO_BILINEAR))
        {
            releaseMasks();
            if (!screen.isBoundInStack())
            {
                m.retry.failed();   // an allocation failed (a bound screen is only this frame's)
            }
            return false;
        }
        m.retry.succeeded();
        screen.shareDepthBuffer(m.mask);
        m.factor = factor;
        m.generation = generation;
        return true;
    }

    // Draws the three masks from this frame's skin mark; false when a pass
    // could not be recorded (the passes on them then skip the frame or go
    // without the fade).
    bool buildMasks(LLRenderTarget& screen)
    {
        Masks& m = masks();
        m.built_frame = U32_MAX;
        if (!prepareMasks(screen))
        {
            return false;
        }
        gGL.flush();
        m.mask.bindTarget();
        LLRHI::clearColor(0, 0, 0, 0);
        m.mask.clear(GL_COLOR_BUFFER_BIT);   // colour only: the depth is the main view's
        LLRHI::SkinFillMask mask;
        mask.value = 1.f;
        mask.eye_value = EYE_SHARE;
        bool drawn = LLRHI::sSkinFillMask(mask);
        m.mask.flush();
        if (!drawn)
        {
            return false;
        }

        LLRHI::SkinFillShrink shrink;
        shrink.mask_texture = m.mask.getTexture();
        shrink.mask_width = m.mask.getWidth();
        shrink.mask_height = m.mask.getHeight();
        shrink.factor = m.factor;
        m.shrunk.bindTarget();
        drawn = LLRHI::sSkinFillShrink(shrink);
        m.shrunk.flush();
        if (!drawn)
        {
            return false;
        }

        LLRHI::SkinFillSoften soften;
        soften.shrunk_texture = m.shrunk.getTexture();
        m.soft.bindTarget();
        drawn = LLRHI::sSkinFillSoften(soften);
        m.soft.flush();
        if (drawn)
        {
            m.built_frame = gFrameCount;
        }
        return drawn;
    }

    // The soft mask of this frame (0 when there is none: no fade) and the
    // full-resolution pixel centre -> its uv scale.
    U32 softMaskThisFrame(F32 uv_scale[2])
    {
        const Masks& m = masks();
        uv_scale[0] = uv_scale[1] = 0.f;
        if (m.built_frame != gFrameCount || !m.soft.isComplete())
        {
            return 0;
        }
        uv_scale[0] = 1.f / (F32)(m.soft.getWidth() * m.factor);
        uv_scale[1] = 1.f / (F32)(m.soft.getHeight() * m.factor);
        return m.soft.getTexture();
    }

    // ------------------------------------------------------------------

    // The main view's light map, painted and not bound (as plus/gtao's).
    bool mainLightMapReady()
    {
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return false;
        }
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        return rt.deferredLight.isComplete() && !rt.deferredLight.isBoundInStack() && rt.deferredScreen.getDepth() &&
               rt.deferredLight.getWidth() == rt.deferredScreen.getWidth() &&
               rt.deferredLight.getHeight() == rt.deferredScreen.getHeight();
    }

    // The light that cast this frame's sun shadows (LLPipeline::generateSunShadow:
    // the sun while it is up, else the moon), eye space, and its colour as
    // softenLight's sunlit (linear); false when it gives no light.
    bool shadowingLight(LLVector3& direction, LLColor3& color)
    {
        const LLEnvironment& environment = LLEnvironment::instance();
        LLSettingsSky::ptr_t sky = environment.getCurrentSky();
        if (!sky)
        {
            return false;
        }
        const bool sun = environment.getIsSunUp();
        const LLVector4& towards = sun ? gPipeline.mTransformedSunDir : gPipeline.mTransformedMoonDir;
        direction.set(towards.mV[VX], towards.mV[VY], towards.mV[VZ]);
        color = linearColor3(sun ? sky->getSunDiffuse() : sky->getMoonDiffuse());
        return direction.normalize() > 0.f && (color.mV[VRED] > 0.f || color.mV[VGREEN] > 0.f || color.mV[VBLUE] > 0.f);
    }

    // color (the shadowing light, linear) as the fill takes its share of:
    // scaled down, its hue kept, so that its brightest channel is no more
    // than FILL_SUN_CEILING at the user's exposure.
    LLColor3 fillSunColor(const LLColor3& color)
    {
        const F32 ceiling = FILL_SUN_CEILING / plus::effectiveExposure();
        const F32 brightest = llmax(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE]);
        if (brightest <= ceiling)
        {
            return color;
        }
        return color * (ceiling / brightest);
    }

    // The sky's ambient light (linear), its luminance: an estimate of what
    // softenLight lights the shadows with (the classic skies' ambient; the
    // reflection probes' irradiance and the sky's scales are not read here,
    // and scale the sun much as they scale the ambient).
    F32 ambientLuminance()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        return sky ? plus::luminance(linearColor3(sky->getTotalAmbient())) : 0.f;
    }

    // The sun's shadows of this frame are there to read: the light map's r and the cascades the fill reads.
    bool sunShadowsReady()
    {
        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        if (LLPipeline::RenderShadowDetail <= 0 || !rt.deferredLight.isComplete() ||
            rt.deferredLight.getWidth() != rt.deferredScreen.getWidth() || rt.deferredLight.getHeight() != rt.deferredScreen.getHeight())
        {
            return false;
        }
        for (U32 i = 0; i < SHADOW_CASCADES; ++i)
        {
            if (!rt.shadow[i].getDepth())
            {
                return false;
            }
        }
        return true;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusSkinFill), part of the
        // quality levels (featuretable.txt).
        // Set in the photo tools (app_settings/plus/phototools/catalog.xml, tab "subject": the Avatar tab).
        // Drawn from the <plus> hooks in LLPipeline::renderDeferredLighting().
        plus::skinsss::addMarkUser(&markWanted);
        // The mark comes and goes with the setting. Asked once idle, when the
        // cached control has the new value (the handlers may run before it does).
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_LEVEL))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                          { doOnIdleOneTime(&plus::skinsss::markWantChanged); });
        }
    }

    plus::FeatureRegistration sRegistration({ "skinfill", &init, {} });
}

namespace plus::skinfill
{
    void renderLightMap()
    {
        const S32 level = currentLevel();
        if (level == LEVEL_OFF || !backendReady())
        {
            releaseMasks();
            return;
        }
        if (!plus::skinsss::markedThisFrame() || gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT)
        {
            return;
        }
        LLRHI::gpuMark("skinfill");
        // The masks serve both passes; the light map's g counts only with the
        // official SSAO (softenLight's HAS_SSAO).
        if (buildMasks(gPipeline.mRT->deferredScreen) && LLPipeline::RenderDeferredSSAO && mainLightMapReady())
        {
            const Masks& m = masks();
            LLRHI::SkinFillOcclusion pass;
            pass.mask_texture = m.mask.getTexture();
            pass.soft_mask_texture = softMaskThisFrame(pass.soft_uv_scale);
            pass.lift = STRENGTHS[level].occlusion;
            LLRenderTarget& light_map = gPipeline.mRT->deferredLight;
            light_map.bindTarget();
            LLRHI::sSkinFillOcclusion(pass);
            light_map.flush();
        }
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest of the lighting goes back to its segment
    }

    void render()
    {
        const S32 level = currentLevel();
        if (level == LEVEL_OFF || !backendReady() || !plus::skinsss::markedThisFrame() || gCubeSnapshot ||
            gPipeline.mRT != &gPipeline.mMainRT || LLRenderTarget::getCurrentBoundTarget() != &gPipeline.mMainRT.screen)
        {
            return;
        }
        LLVector3 light_direction;
        LLColor3 light_color;
        if (!shadowingLight(light_direction, light_color))
        {
            return;
        }

        const LLPipeline::RenderTargetPack& rt = *gPipeline.mRT;
        LLRHI::SkinFillLight pass;
        pass.depth_texture = rt.deferredScreen.getDepth();
        pass.diffuse_texture = rt.deferredScreen.getTexture(0);
        pass.normal_texture = rt.deferredScreen.getTexture(2);
        pass.soft_mask_texture = softMaskThisFrame(pass.soft_uv_scale);
        pass.shadows = sunShadowsReady();
        if (pass.shadows)
        {
            pass.light_map_texture = rt.deferredLight.getTexture();
            for (U32 i = 0; i < SHADOW_CASCADES; ++i)
            {
                pass.shadow_textures[i] = rt.shadow[i].getDepth();
                memcpy(pass.shadow_matrices[i], glm::value_ptr(gPipeline.mSunShadowMatrix[i]), sizeof(pass.shadow_matrices[i]));
                pass.shadow_far[i] = gPipeline.mSunClipPlanes.mV[i];
            }
        }
        // The camera's projection, loaded for this frame's scene.
        const glm::mat4 inverse_projection = glm::inverse(get_current_projection());
        memcpy(pass.inverse_projection, glm::value_ptr(inverse_projection), sizeof(pass.inverse_projection));
        const F32 strength = STRENGTHS[level].fill;
        const LLColor3 fill_sun = fillSunColor(light_color);
        // Grey, as bright as that light: the skin's own colour is lifted, not
        // dyed in the sun's (fix-round41).
        const F32 fill_grey = luminance(fill_sun) * strength;
        for (U32 c = 0; c < 3; ++c)
        {
            pass.light_direction[c] = light_direction.mV[c];
            pass.light_color[c] = fill_grey;
        }
        pass.sun_luminance = luminance(light_color);
        pass.ambient_luminance = ambientLuminance();
        pass.sunlit_share = SUNLIT_SHARE;
        pass.directionality = DIRECTIONALITY;
        pass.near_reach = NEAR_REACH;
        pass.eye_share = EYE_SHARE;

        LLRHI::gpuMark("skinfill");
        gGL.flush();
        LLRHI::sSkinFillLight(pass);
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest goes back to its segment
    }
}
