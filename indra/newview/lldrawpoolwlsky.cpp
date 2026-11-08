/**
 * @file lldrawpoolwlsky.cpp
 * @brief LLDrawPoolWLSky class implementation
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

#include "lldrawpoolwlsky.h"

#include "llerror.h"
#include "llface.h"
#include "llimage.h"
#include "llrender.h"
#include "llenvironment.h"
#include "llglslshader.h"
#include "llgl.h"

#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewercamera.h"
#include "pipeline.h"
#include "llsky.h"
#include "llvowlsky.h"
#include "llsettingsvo.h"
#include "llviewercontrol.h"
#include "llrhigl.h"
#include "llrendertarget.h"            // <plus> plus/starfield: the size of the target </plus>
#include "plus/plus.h"                 // <plus> plus/starfield: isNoPost() </plus>
#include "plus/starfield/starfield.h"  // <plus> </plus>
#include "plus/skyscreen/skyscreen.h"  // <plus> </plus>
#include "plus/nightclouds/nightclouds.h" // <plus> </plus>
#include "plus/volclouds/volclouds.h" // <plus> research/186 </plus>
#include "plus/realstars/realstars.h"  // <plus> </plus>
#include "plus/rainbow/rainbow.h"      // <plus> research/255 </plus>
#include "llworldpassstate.h"           // <plus> research/204: the pass states </plus>
// <plus> research/204 10 (vk-s4c2): the end of a sky part's draws. LLRHI_PASS_UNITS on: the part keeps
// its units 0 and 1 (made active as the unbinds would); else the unbinds as before, and in verify the
// units they leave (white) watched by the backend.
static void end_sky_units()
{
    if (LLRHI::passUnitsKeep())
    {
        gGL.flush();    // as the unbinds do
        gGL.getTexUnit(1)->activate();
        return;
    }
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
    LLRHI::passUnitsLeft(0x3u, gFrameCount);
}
// </plus>

extern bool gCubeSnapshot;

// <plus> plus/rainbow (research/255): a reflection probe's irradiance pass is being drawn (as
// LLSettingsVOSky::applySpecial tells it): no clouds there, and no rainbow
static bool probe_irradiance_pass()
{
    return gCubeSnapshot && !gPipeline.mReflectionMapManager.isRadiancePass();
}
// </plus>

static LLStaticHashedString sCamPosLocal("camPosLocal");
static LLStaticHashedString sCustomAlpha("custom_alpha");
// <plus> plus/starfield (research/176)
static LLStaticHashedString sStarField("star_field");
static LLStaticHashedString sStarGlow("star_glow");
static LLStaticHashedString sStarPeak("star_peak");
static LLStaticHashedString sStarNoPost("star_nopost");
static LLStaticHashedString sStarGain("star_gain");
static LLStaticHashedString sCloudNight("plus_cloud_night");   // plus/nightclouds
static LLStaticHashedString sStarZenith("star_zenith");         // plus/realstars (research/177)
static LLStaticHashedString sStarReal("star_real");
static LLStaticHashedString sStarLine("star_line");
static LLStaticHashedString sStarArt("star_art");               // research/288: the constellation pictures
static LLStaticHashedString sStarTwinkle("star_twinkle");       // research/176 section 11
static LLStaticHashedString sStarHaze("star_haze");             // research/224: the air's haze on the stars' dimming
static LLStaticHashedString sMilkyShown("milky_shown");         // research/224: the baked Milky Way's brightest, on the screen

namespace
{
    // The linear light, before tone mapping, at the centre of the brightest star (magnitude -1)
    // and of one Milky Way patch at full level (the patches' times PlusSkyStarGain; the stars
    // take the gain after their compression, starsF.glsl, research/176 section 9). Overlapping
    // patches add up to about ten times the second. Tuned on paper (research/176 section 6).
    constexpr F32 STAR_FIELD_PEAK = 30.f;
    constexpr F32 STAR_GLOW_PEAK = 0.012f;
    // research/224: the baked Milky Way brightest texel (about 1) as it is to show on the screen
    // (linear, before PlusSkyStarGain x PlusSkyStarsMilkyWayGain, 0.25 by default): skyMilkyF.glsl
    // undoes what the scene does to the sky, as the stars do. On paper; to be tuned on a night sky.
    constexpr F32 MILKY_SHOWN_PEAK = 0.24f;
    // plus/realstars: the constellation lines' brightness on the screen (linear) at
    // PlusSkyStarsLineAlpha 1, before the sky's star brightness (starsF.glsl). At the default
    // 0.35 about a sixth-magnitude star's centre: faint, under the stars.
    constexpr F32 STAR_LINE_PEAK = 0.2f;
    // research/288: the constellation pictures' brightness on the screen (linear) for a white
    // texel at PlusSkyStarsArtAlpha 1, before the sky's star brightness (starsF.glsl). The pictures
    // are not evened out: each one's brightest texel is 98 .. 215 / 255 as Stellarium drew it
    // (star-art-data, 2026-11-06). At the default 0.5 the brightest picture's top is about 0.1, a
    // fifth-magnitude star's centre, the faintest's about half that (research/288 section 3.2).
    // On paper; PlusSkyStarsArtAlpha tunes it on a night sky.
    constexpr F32 STAR_ART_PEAK = 0.25f;
}
// </plus>

static LLGLSLShader* cloud_shader = NULL;
static LLGLSLShader* sky_shader   = NULL;
static LLGLSLShader* sun_shader   = NULL;
static LLGLSLShader* moon_shader  = NULL;

static float sStarTime;

LLDrawPoolWLSky::LLDrawPoolWLSky(void) :
    LLDrawPool(POOL_WL_SKY)
{
}

LLDrawPoolWLSky::~LLDrawPoolWLSky()
{
}

LLViewerTexture *LLDrawPoolWLSky::getDebugTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::beginDeferredPass(S32 pass)
{
    sky_shader = &gDeferredWLSkyProgram;
    cloud_shader = &gDeferredWLCloudProgram;

    sun_shader = &gDeferredWLSunProgram;

    moon_shader = &gDeferredWLMoonProgram;
}

void LLDrawPoolWLSky::endDeferredPass(S32 pass)
{
    sky_shader   = nullptr;
    cloud_shader = nullptr;
    sun_shader   = nullptr;
    moon_shader  = nullptr;

    // clear the depth buffer so haze shaders can use unwritten depth as a mask
    LLRHI::clear(GL_DEPTH_BUFFER_BIT);
}

void LLDrawPoolWLSky::renderDome(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader * shader) const
{
    llassert_always(NULL != shader);

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();

    //chop off translation
    if (LLPipeline::sReflectionRender && camPosLocal.mV[2] > 256.f)
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], 256.f-camPosLocal.mV[2]*0.5f);
    }
    else
    {
        gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    }


    // the windlight sky dome works most conveniently in a coordinate system
    // where Y is up, so permute our basis vectors accordingly.
    gGL.rotatef(120.f, 1.f / F_SQRT3, 1.f / F_SQRT3, 1.f / F_SQRT3);

    gGL.scalef(0.333f, 0.333f, 0.333f);

    gGL.translatef(0.f,-camHeightLocal, 0.f);

    // Draw WL Sky
    shader->uniform3f(sCamPosLocal, 0.f, camHeightLocal, 0.f);

    gSky.mVOWLSkyp->drawDome();

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

extern LLPointer<LLImageGL> gEXRImage;

static bool use_hdri_sky()
{
    static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
    static LLCachedControl<bool> irradiance_only(gSavedSettings, "RenderHDRIIrradianceOnly", false);

    return gCubeSnapshot && (!irradiance_only || !gPipeline.mReflectionMapManager.isRadiancePass()) ? gEXRImage.notNull() : // always use HDRI for reflection probes when available
        gEXRImage.notNull() ? hdri_split > 0.f : // fallback to EEP sky when split screen is zero
        false; // no HDRI available, always use EEP sky

}

void LLDrawPoolWLSky::renderSkyHazeDeferred(const LLVector3& camPosLocal, F32 camHeightLocal) const
{
    if (!gSky.mVOSkyp)
    {
        return;
    }

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY))
    {
        LLWorldPassState pass_state("sky haze"); // <plus> research/204: the pass state </plus>
        if (use_hdri_sky())
        {
            sky_shader = &gEnvironmentMapProgram;
            sky_shader->bind();
            S32 idx = sky_shader->enableTexture(LLShaderMgr::ENVIRONMENT_MAP);
            if (idx > -1)
            {
                gGL.getTexUnit(idx)->bind(gEXRImage);
            }

            static LLCachedControl<F32> hdri_exposure(gSavedSettings, "RenderHDRIExposure", 0.0f);
            static LLCachedControl<F32> hdri_rotation(gSavedSettings, "RenderHDRIRotation", 0.f);
            static LLCachedControl<F32> hdri_split(gSavedSettings, "RenderHDRISplitScreen", 1.f);
            static LLStaticHashedString hdri_split_screen("hdri_split_screen");

            LLMatrix3 rot;
            rot.setRot(0.f, hdri_rotation*DEG_TO_RAD, 0.f);

            sky_shader->uniform1f(LLShaderMgr::SKY_HDR_SCALE, powf(2.f, hdri_exposure));
            sky_shader->uniformMatrix3fv(LLShaderMgr::DEFERRED_ENV_MAT, 1, GL_FALSE, (F32*) rot.mMatrix);
            sky_shader->uniform1f(hdri_split_screen, gCubeSnapshot ? 1.f : hdri_split);
        }
        else
        {
            sky_shader->bind();
        }

        LLGLSPipelineDepthTestSkyBox sky(true, true);

        sky_shader->uniform1i(LLShaderMgr::CUBE_SNAPSHOT, gCubeSnapshot ? 1 : 0);

        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLViewerTexture* rainbow_tex = gSky.mVOSkyp->getRainbowTex();
        LLViewerTexture* halo_tex  = gSky.mVOSkyp->getHaloTex();

        sky_shader->bindTexture(LLShaderMgr::RAINBOW_MAP, rainbow_tex);
        sky_shader->bindTexture(LLShaderMgr::HALO_MAP,  halo_tex);

        F32 moisture_level  = (float)psky->getSkyMoistureLevel();
        F32 droplet_radius  = (float)psky->getSkyDropletRadius();
        F32 ice_level       = (float)psky->getSkyIceLevel();

        // hobble halos and rainbows when there's no light source to generate them
        if (!psky->getIsSunUp() && !psky->getIsMoonUp())
        {
            moisture_level = 0.0f;
            ice_level      = 0.0f;
        }

        // <plus> plus/rainbow (research/255): PlusRainbow on, skyRainbowF.glsl works the rainbow out in place of
        // the sky's texture (whose moisture is then 0). None in a probe's irradiance pass (as the clouds) nor on
        // the HDRI sky.
        if (plus::rainbow::enabled())
        {
            moisture_level = 0.0f;
        }
        plus::rainbow::bind(*sky_shader, *psky, sky_shader != &gEnvironmentMapProgram && !probe_irradiance_pass());
        // </plus>

        sky_shader->uniform1f(LLShaderMgr::MOISTURE_LEVEL, moisture_level);
        sky_shader->uniform1f(LLShaderMgr::DROPLET_RADIUS, droplet_radius);
        sky_shader->uniform1f(LLShaderMgr::ICE_LEVEL, ice_level);

        sky_shader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        sky_shader->uniform1i(LLShaderMgr::SUN_UP_FACTOR, psky->getIsSunUp() ? 1 : 0);

        // <plus> research/204: the pass state (renderDome sets matrices and draws).
        // research/207 T4: LLVOWLSky::drawDome draws with LLGLDepthTest(GL_TRUE, GL_FALSE):
        // the depth write is off for the dome. Declared so, and set so before the check
        // while declaring (off: the same values as `sky`, nothing changes).
        LLGLDepthTest dome_depth(GL_TRUE, pass_state.declaring() ? GL_FALSE : GL_TRUE, GL_LEQUAL);
        if (pass_state.declaring())
        {
            LLRHI::PassState state = world_sky_pass_state(*sky_shader, false, 0);
            state.sampler(world_pass_channel(*sky_shader, LLShaderMgr::RAINBOW_MAP)).sampler(world_pass_channel(*sky_shader, LLShaderMgr::HALO_MAP));
            state.sampler(sky_shader == &gEnvironmentMapProgram ? world_pass_channel(*sky_shader, LLShaderMgr::ENVIRONMENT_MAP) : -1);
            pass_state.declare(state);
        }
        pass_state.check(1);
        LLRHI::PassStateWrite write_through(pass_state); // research/207 T4: on, the declaration for the dome
        // </plus>

        /// Render the skydome
        renderDome(origin, camHeightLocal, sky_shader);

        sky_shader->unbind();
    }
}

void LLDrawPoolWLSky::renderStarsDeferred(const LLVector3& camPosLocal) const
{
    // <plus> research/288: the pictures' texture let go once switched off, on the HDRI sky too
    // (which returns below, before refreshStars())
    if (gSky.mVOWLSkyp)
    {
        gSky.mVOWLSkyp->dropStarArtIfUnwanted();
    }
    // </plus>
    if (!gSky.mVOSkyp || use_hdri_sky())
    {
        return;
    }

    // <plus> plus/starfield: follows PlusSkyStars (a new catalogue, the buffer made again)
    if (gSky.mVOWLSkyp)
    {
        gSky.mVOWLSkyp->refreshStars();
    }
    // research/288: the constellation pictures (PlusSkyStarsArt, the real sky only: their places
    // are the real stars'). Their file is read on a thread from the first frame they are wanted,
    // and the texture made the frame it is in, here before the pass (no texture is made inside
    // it, research/207 T4); none until then. Only for a sky that shows the stars (the
    // test of star_alpha below, which a reflection passes only on such a sky too). Off: a cached
    // setting read, nothing else. None at PlusSkyStarsArtAlpha 0 (nothing would be drawn; a texture
    // made stays, so a slider through 0 does not make it again).
    const bool art_asked = gSky.mVOWLSkyp && plus::realstars::artWanted() && gSky.mVOWLSkyp->isStarReal()
                        && gSky.mVOWLSkyp->hasStarArt() && plus::realstars::artAlpha() > 0.f
                        && LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f >= 0.001f;
    const U32 art_texture = art_asked ? gSky.mVOWLSkyp->prepareStarArt() : 0;
    // </plus>

    LLWorldPassState pass_state("sky stars"); // <plus> research/204: the pass state </plus>
    LLGLSPipelineBlendSkyBox gls_sky(true, false);

    gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

    F32 star_alpha = LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f;

    // If start_brightness is not set, exit
    if(star_alpha < 0.001f)
    {
        LL_DEBUGS("SKY") << "star_brightness below threshold." << LL_ENDL;
        return;
    }

    gDeferredStarProgram.bind();

    LLViewerTexture* tex_a = gSky.mVOSkyp->getBloomTex();
    LLViewerTexture* tex_b = gSky.mVOSkyp->getBloomTexNext();

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();

    if (tex_a && (!tex_b || (tex_a == tex_b)))
    {
        // Bind current and next sun textures
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b && !tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_b);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);
        blend_factor = 0;
    }
    else if (tex_b != tex_a)
    {
        gGL.getTexUnit(0)->bind(tex_a);
        gGL.getTexUnit(1)->bind(tex_b);
    }

    gGL.pushMatrix();
    gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    // <plus> plus/realstars (research/177): the real sky turned for the place and the time (J2000
    // to the agent frame) instead of the official slow turn
    const bool real_sky = gSky.mVOWLSkyp && gSky.mVOWLSkyp->getStarMode() > plus::starfield::MODE_OFFICIAL
                       && gSky.mVOWLSkyp->isStarReal();
    const LLVector3 sun_dir = real_sky ? LLEnvironment::instance().getCurrentSky()->getSunDirection() : LLVector3::z_axis;
    const plus::realstars::Frame* real_frame = real_sky ? &plus::realstars::frame(sun_dir) : nullptr;
    if (real_frame)
    {
        gGL.multMatrix(real_frame->gl);
    }
    else
    // </plus>
    gGL.rotatef(gFrameTimeSeconds*0.01f, 0.f, 0.f, 1.f);
    gDeferredStarProgram.uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

    if (LLPipeline::sReflectionRender)
    {
        star_alpha = 1.0f;
    }
    gDeferredStarProgram.uniform1f(sCustomAlpha, star_alpha);

    sStarTime = (F32)LLFrameTimer::getElapsedSeconds() * 0.5f;

    gDeferredStarProgram.uniform1f(LLShaderMgr::WATER_TIME, sStarTime);

    // <plus> plus/starfield (research/176): the new look needs the target's size (a star is a
    // few pixels wide whatever the field of view) and whether the picture is clamped to 1
    const bool star_field = gSky.mVOWLSkyp->getStarMode() > plus::starfield::MODE_OFFICIAL;
    gDeferredStarProgram.uniform1f(sStarField, star_field ? 1.f : 0.f);
    if (star_field)
    {
        const F32 width = (F32)(LLRenderTarget::sCurResX ? LLRenderTarget::sCurResX : (U32)gGLViewport[2]);
        const F32 height = (F32)(LLRenderTarget::sCurResY ? LLRenderTarget::sCurResY : (U32)gGLViewport[3]);
        gDeferredStarProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, llmax(width, 1.f), llmax(height, 1.f));
        gDeferredStarProgram.uniform1f(sStarNoPost, plus::isNoPost() ? 1.f : 0.f);
        gDeferredStarProgram.uniform1f(sStarGlow, 0.f);
        gDeferredStarProgram.uniform1f(sStarPeak, STAR_FIELD_PEAK);
        gDeferredStarProgram.uniform1f(sStarGain, plus::starfield::gain());
        // plus/realstars: the zenith in the buffer's frame (the made-up sky's is z), for the horizon
        const LLVector3 zenith = real_frame ? real_frame->zenith : LLVector3(0.f, 0.f, 1.f);
        gDeferredStarProgram.uniform3f(sStarZenith, zenith.mV[VX], zenith.mV[VY], zenith.mV[VZ]);
        gDeferredStarProgram.uniform1f(sStarReal, real_frame ? 1.f : 0.f);
        // research/224: the air dims the stars the more, the hazier it is (0.5 + 0.7 x haze; the
        // default haze 0.7 gives about 1, the dimming as it was)
        gDeferredStarProgram.uniform1f(sStarHaze, 0.5f + 0.7f * (F32)LLEnvironment::instance().getCurrentSky()->getHazeDensity());
        gDeferredStarProgram.uniform2f(sStarLine, 0.f, 0.f);
        // research/176 section 11: the twinkle's strength and clock (real time, not SL's)
        gDeferredStarProgram.uniform2f(sStarTwinkle, plus::starfield::twinkle(), plus::starfield::twinkleTime());
    }
    // The stars come out at the brightness they mean whatever the sky's HDR scale, the exposure
    // and the tone curve (research/176 section 9): starsF.glsl undoes them. Bound in either
    // look (the shader declares the exposure map in both).
    const S32 exposure_channel = plus::skyscreen::bind(gDeferredStarProgram,
        plus::skyscreen::current(LLEnvironment::instance().getCurrentSky().get()));
    // research/288: the pictures' array on its channel, for the pictures' draw below (the real
    // sky's frame only; none at PlusSkyStarsArtAlpha 0)
    const F32 art_shown = real_frame && art_texture ? STAR_ART_PEAK * plus::realstars::artAlpha() : 0.f;
    const S32 art_channel = art_shown > 0.f ? gDeferredStarProgram.enableTexture(LLShaderMgr::PLUS_STAR_ART_MAP, LLTexUnit::TT_TEXTURE_2D_ARRAY) : -1;
    if (art_channel > -1)
    {
        gGL.getTexUnit(art_channel)->bindManual(LLTexUnit::TT_TEXTURE_2D_ARRAY, art_texture, true);
    }
    // </plus>

    // <plus> research/204: the pass state (the Milky Way, the lines and the pictures below change uniforms only)
    if (pass_state.declaring())
    {
        LLRHI::PassState state = world_sky_pass_state(gDeferredStarProgram, false, GL_ONE);
        state.sampler(tex_a || tex_b ? 0 : -1).sampler(tex_a && tex_b && tex_a != tex_b ? 1 : -1).sampler(exposure_channel);
        state.sampler(art_channel); // research/288
        pass_state.declare(state);
    }
    pass_state.check(1);
    // research/207 T4: on, the declaration for the stars, the Milky Way and the lines (the
    // rest of the function makes no state call: uniforms, units, the program, the matrix)
    LLRHI::PassStateWrite write_through(pass_state);
    // </plus>

    gSky.mVOWLSkyp->drawStars();

    // <plus> plus/starfield: the Milky Way's patches, after the stars, by the same program
    // (PlusSkyStarsMilkyWayGain: the patches' own brightness on top of the stars' gain, on their
    // light at the centre, so the star curve in starsF.glsl is not touched; 0 draws none)
    const F32 milky_way_gain = plus::starfield::milkyWayGain();
    if (star_field && milky_way_gain > 0.f && gSky.mVOWLSkyp->hasStarGlow())
    {
        gDeferredStarProgram.uniform1f(sStarGlow, 1.f);
        gDeferredStarProgram.uniform1f(sStarPeak, STAR_GLOW_PEAK * plus::starfield::gain() * milky_way_gain);
        pass_state.check(1); // research/204
        gSky.mVOWLSkyp->drawStarGlow();
    }

    // plus/realstars (research/177 §2.6): the constellation lines, faint, on any sky with the stars
    if (real_frame && gSky.mVOWLSkyp->hasStarLines() && plus::realstars::linesWanted())
    {
        // dimmed by the sky's star brightness in starsF.glsl, as the stars are (not by the sun)
        const F32 shown = STAR_LINE_PEAK * plus::realstars::lineAlpha();
        if (shown > 0.f)
        {
            gDeferredStarProgram.uniform1f(sStarGlow, 2.f);
            gDeferredStarProgram.uniform2f(sStarLine, shown, plus::realstars::lineWidth());
            pass_state.check(1); // research/204
            gSky.mVOWLSkyp->drawStarLines();
        }
    }

    // research/288: the constellation pictures, faint, under the same light as the lines (dimmed
    // by the sky's star brightness, gone under the horizon: starsF.glsl). Added on top, so the
    // order with the stars and the lines does not matter.
    if (art_channel > -1)
    {
        gDeferredStarProgram.uniform1f(sStarGlow, 3.f);
        gDeferredStarProgram.uniform1f(sStarArt, art_shown);
        pass_state.check(1); // research/204
        gSky.mVOWLSkyp->drawStarArt();
    }
    // </plus>

    end_sky_units(); // <plus> research/204 10 (vk-s4c2): was the unbinds of units 0 and 1 </plus>
    // <plus> plus/starfield
    plus::skyscreen::unbind(exposure_channel);
    if (art_channel > -1)
    {
        gGL.getTexUnit(art_channel)->unbind(LLTexUnit::TT_TEXTURE_2D_ARRAY); // research/288
    }
    // </plus>

    gDeferredStarProgram.unbind();

    gGL.popMatrix();
}

// <plus> research/224: the Milky Way baked into a picture of the sky (plus/starfield/skymilky.h),
// PlusSkyStars 2 with PlusSkyStarsMilkyWayHQ. A box round the camera, turned with the stars (the
// real sky's matrix or the official slow turn); skyMilkyF.glsl takes the picture's place from the
// direction of each pixel. The blend and the depth are the stars' (added on the sky, behind everything).
void LLDrawPoolWLSky::renderStarMilkyDeferred(const LLVector3& camPosLocal) const
{
    if (!gSky.mVOSkyp || !gSky.mVOWLSkyp || use_hdri_sky())
    {
        return;
    }
    LLVOWLSky* sky = gSky.mVOWLSkyp.get();
    LLImageGL* picture = sky->getMilkyTexture();
    const F32 milky_way_gain = plus::starfield::milkyWayGain();
    if (!sky->wantsMilkyPicture() || !picture || milky_way_gain <= 0.f)
    {
        return;
    }
    F32 star_alpha = LLEnvironment::instance().getCurrentSky()->getStarBrightness() / 500.0f;
    // as renderStarsDeferred: the sky without stars first; a reflection draws them at full
    if (star_alpha < 0.001f)
    {
        return;
    }
    if (LLPipeline::sReflectionRender)
    {
        star_alpha = 1.0f;
    }

    // the box's vertex buffer first: none is made inside the pass (research/207 T4), and a box that
    // cannot be made leaves the pass undeclared and unchecked
    if (!sky->prepareMilkyBox())
    {
        return;
    }

    LLWorldPassState pass_state("sky milky way");
    LLGLSPipelineBlendSkyBox gls_sky(true, false);
    gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

    gDeferredStarMilkyProgram.bind();
    const S32 picture_channel = gDeferredStarMilkyProgram.enableTexture(LLShaderMgr::ENVIRONMENT_MAP);
    if (picture_channel > -1)
    {
        gGL.getTexUnit(picture_channel)->bind(picture);
    }

    gGL.pushMatrix();
    gGL.translatef(camPosLocal.mV[0], camPosLocal.mV[1], camPosLocal.mV[2]);
    // the stars' own turn (renderStarsDeferred)
    const bool real_sky = sky->getStarMode() > plus::starfield::MODE_OFFICIAL && sky->isStarReal();
    const LLVector3 sun_dir = real_sky ? LLEnvironment::instance().getCurrentSky()->getSunDirection() : LLVector3::z_axis;
    const plus::realstars::Frame* real_frame = real_sky ? &plus::realstars::frame(sun_dir) : nullptr;
    if (real_frame)
    {
        gGL.multMatrix(real_frame->gl);
    }
    else
    {
        gGL.rotatef(gFrameTimeSeconds*0.01f, 0.f, 0.f, 1.f);
    }

    gDeferredStarMilkyProgram.uniform1f(sCustomAlpha, star_alpha);
    const LLVector3 zenith = real_frame ? real_frame->zenith : LLVector3(0.f, 0.f, 1.f);
    gDeferredStarMilkyProgram.uniform3f(sStarZenith, zenith.mV[VX], zenith.mV[VY], zenith.mV[VZ]);
    gDeferredStarMilkyProgram.uniform1f(sStarReal, real_frame ? 1.f : 0.f);
    gDeferredStarMilkyProgram.uniform1f(sStarHaze, 0.5f + 0.7f * (F32)LLEnvironment::instance().getCurrentSky()->getHazeDensity());
    gDeferredStarMilkyProgram.uniform1f(sMilkyShown, MILKY_SHOWN_PEAK * plus::starfield::gain() * milky_way_gain);
    // what the scene does to the sky is undone here too (plus/skyscreen), as in the stars'
    const S32 exposure_channel = plus::skyscreen::bind(gDeferredStarMilkyProgram,
        plus::skyscreen::current(LLEnvironment::instance().getCurrentSky().get()));

    if (pass_state.declaring())
    {
        LLRHI::PassState state = world_sky_pass_state(gDeferredStarMilkyProgram, false, GL_ONE);
        state.sampler(world_pass_channel(gDeferredStarMilkyProgram, LLShaderMgr::ENVIRONMENT_MAP)).sampler(exposure_channel);
        pass_state.declare(state);
    }
    pass_state.check(1);
    LLRHI::PassStateWrite write_through(pass_state);

    sky->drawMilkyBox();

    end_sky_units();
    plus::skyscreen::unbind(exposure_channel);

    gDeferredStarMilkyProgram.unbind();

    gGL.popMatrix();
}
// </plus>

void LLDrawPoolWLSky::renderSkyCloudsDeferred(const LLVector3& camPosLocal, F32 camHeightLocal, LLGLSLShader* cloudshader) const
{
    if (use_hdri_sky())
    {
        return;
    }

    if (gPipeline.canUseWindLightShaders() && gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_CLOUDS) && gSky.mVOSkyp && gSky.mVOSkyp->getCloudNoiseTex())
    {
        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        LLRHI::gpuMark("clouds"); // <plus> research/186 S0: the cloud dome apart from the sky's haze, stars and moon in the GPU time (VKPerf) </plus>
        LLWorldPassState pass_state("sky clouds"); // <plus> research/204: the pass state </plus>
        LLGLSPipelineBlendSkyBox pipeline(true, true);

        cloudshader->bind();

        LLPointer<LLViewerTexture> cloud_noise      = gSky.mVOSkyp->getCloudNoiseTex();
        LLPointer<LLViewerTexture> cloud_noise_next = gSky.mVOSkyp->getCloudNoiseTexNext();

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        F32 cloud_variance = psky ? (F32)psky->getCloudVariance() : 0.0f;
        F32 blend_factor   = psky ? (F32)psky->getBlendFactor() : 0.0f;

        if (psky->getCloudScrollRate().isExactlyZero())
        {
            blend_factor = 0.f;
        }

        // if we even have sun disc textures to work with...
        if (cloud_noise || cloud_noise_next)
        {
            if (cloud_noise && (!cloud_noise_next || (cloud_noise == cloud_noise_next)))
            {
                // Bind current and next sun textures
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next && !cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise_next, LLTexUnit::TT_TEXTURE);
                blend_factor = 0;
            }
            else if (cloud_noise_next != cloud_noise)
            {
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP, cloud_noise, LLTexUnit::TT_TEXTURE);
                cloudshader->bindTexture(LLShaderMgr::CLOUD_NOISE_MAP_NEXT, cloud_noise_next, LLTexUnit::TT_TEXTURE);
            }
        }

        cloudshader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
        cloudshader->uniform1f(LLShaderMgr::CLOUD_VARIANCE, cloud_variance);
        cloudshader->uniform1f(LLShaderMgr::SUN_MOON_GLOW_FACTOR, psky->getSunMoonGlowFactor());

        // <plus> plus/nightclouds (research/176 section 10): the night clouds under an
        // auto-adjusted classic sky; 0 (as official) otherwise. The exposure map is bound either
        // way (the shader declares it); what the scene does to the sky is only worked out when
        // it is used.
        const F32 night_lift = plus::nightclouds::lift(psky.get());
        cloudshader->uniform1f(sCloudNight, night_lift);
        if (cloudshader == &gDeferredWLCloudVolProgram)
        {
            plus::volclouds::bind(*cloudshader, *psky); // research/186, research/244
        }
        // plus/rainbow (research/255): the sky's bow over the clouds too (none in an irradiance pass, where
        // the clouds are not drawn, nor on the HDRI sky, which returns above)
        plus::rainbow::bind(*cloudshader, *psky, !probe_irradiance_pass());
        const S32 exposure_channel = plus::skyscreen::bind(*cloudshader,
            night_lift > 0.f ? plus::skyscreen::current(psky.get()) : plus::skyscreen::Screen());
        // </plus>

        // <plus> research/204: the pass state (renderDome sets matrices and draws).
        // research/207 T4: the dome is drawn without depth writes (as the haze above).
        LLGLDepthTest dome_depth(GL_TRUE, pass_state.declaring() ? GL_FALSE : GL_TRUE, GL_LEQUAL);
        if (pass_state.declaring())
        {
            LLRHI::PassState state = world_sky_pass_state(*cloudshader, false, GL_ONE_MINUS_SRC_ALPHA);
            state.sampler(cloud_noise || cloud_noise_next ? world_pass_channel(*cloudshader, LLShaderMgr::CLOUD_NOISE_MAP) : -1);
            state.sampler(cloud_noise && cloud_noise_next && cloud_noise != cloud_noise_next ?
                          world_pass_channel(*cloudshader, LLShaderMgr::CLOUD_NOISE_MAP_NEXT) : -1);
            state.sampler(exposure_channel);
            pass_state.declare(state);
        }
        pass_state.check(1);
        LLRHI::PassStateWrite write_through(pass_state); // research/207 T4: on, the declaration for the dome
        // </plus>

        /// Render the skydome
        renderDome(camPosLocal, camHeightLocal, cloudshader);

        plus::skyscreen::unbind(exposure_channel); // <plus> </plus>

        cloudshader->unbind();

        end_sky_units(); // <plus> research/204 10 (vk-s4c2): was the unbinds of units 0 and 1 </plus>
    }
}

void LLDrawPoolWLSky::renderHeavenlyBodies()
{
    if (!gSky.mVOSkyp || use_hdri_sky()) return;

    LLWorldPassState pass_state("sky sun"); // <plus> research/204: the pass state </plus>
    LLGLSPipelineBlendSkyBox gls_skybox(true, true); // SL-14113 we need moon to write to depth to clip stars behind

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();
    gGL.pushMatrix();
    gGL.translatef(origin.mV[0], origin.mV[1], origin.mV[2]);

    LLFace * face = gSky.mVOSkyp->mFace[LLVOSky::FACE_SUN];

    F32 blend_factor = (F32)LLEnvironment::instance().getCurrentSky()->getBlendFactor();
    bool can_use_vertex_shaders = gPipeline.shadersLoaded();
    bool can_use_windlight_shaders = gPipeline.canUseWindLightShaders();


    if (gSky.mVOSkyp->getSun().getDraw() && face && face->getGeomCount())
    {
        LLPointer<LLViewerTexture> tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLPointer<LLViewerTexture> tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.getTexUnit(1)->unbind(LLTexUnit::TT_TEXTURE);

        // if we even have sun disc textures to work with...
        if (tex_a || tex_b)
        {
            // if and only if we have a texture defined, render the sun disc
            if (can_use_vertex_shaders && can_use_windlight_shaders)
            {
                sun_shader->bind();

                if (tex_a && (!tex_b || (tex_a == tex_b)))
                {
                    // Bind current and next sun textures
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b && !tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                    blend_factor = 0;
                }
                else if (tex_b != tex_a)
                {
                    sun_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                    sun_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                }

                LLColor4 color(gSky.mVOSkyp->getSun().getInterpColor());

                sun_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
                sun_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);

                // <plus> research/204: the pass state
                if (pass_state.declaring())
                {
                    LLRHI::PassState state = world_sky_pass_state(*sun_shader, true, GL_ONE_MINUS_SRC_ALPHA);
                    state.sampler(world_pass_channel(*sun_shader, LLShaderMgr::DIFFUSE_MAP));
                    state.sampler(tex_a && tex_b && tex_a != tex_b ? world_pass_channel(*sun_shader, LLShaderMgr::ALTERNATE_DIFFUSE_MAP) : -1);
                    pass_state.declare(state);
                }
                pass_state.check(1);
                LLRHI::PassStateWrite write_through(pass_state); // research/207 T4: on, the declaration for the disc
                // </plus>

                face->renderIndexed();

                end_sky_units(); // <plus> research/204 10 (vk-s4c2): was the unbinds of units 0 and 1 </plus>

                sun_shader->unbind();
            }
        }
    }

    // <plus> research/moon-quad-cuts-milkyway-2026-10-26 6-B: the moon is drawn by
    // renderMoonDeferred(), after the stars and the Milky Way, without writing depth. Its alpha
    // covers them (soft edges) instead of the depth test cutting them (a square or a jagged black
    // band where the texture's alpha is not 0).
    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderMoonDeferred()
{
    if (!gSky.mVOSkyp || use_hdri_sky()) return;

    LLWorldPassState pass_state("sky moon"); // research/204: the pass state
    LLGLSPipelineBlendSkyBox gls_skybox(true, false); // depth test against the scene, no depth write

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();
    gGL.pushMatrix();
    gGL.translatef(origin.mV[0], origin.mV[1], origin.mV[2]);

    const bool can_use_vertex_shaders = gPipeline.shadersLoaded();
    const bool can_use_windlight_shaders = gPipeline.canUseWindLightShaders();

    LLFace* face = gSky.mVOSkyp->mFace[LLVOSky::FACE_MOON];
    // </plus>

    if (gSky.mVOSkyp->getMoon().getDraw() && face && face->getTexture(LLRender::DIFFUSE_MAP) && face->getGeomCount() && moon_shader)
    {
        LLViewerTexture* tex_a = face->getTexture(LLRender::DIFFUSE_MAP);
        LLViewerTexture* tex_b = face->getTexture(LLRender::ALTERNATE_DIFFUSE_MAP);

        LLColor4 color(gSky.mVOSkyp->getMoon().getInterpColor());

        if (can_use_vertex_shaders && can_use_windlight_shaders && (tex_a || tex_b))
        {
            moon_shader->bind();

            if (tex_a && (!tex_b || (tex_a == tex_b)))
            {
                // Bind current and next sun textures
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b && !tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
                //blend_factor = 0;
            }
            else if (tex_b != tex_a)
            {
                moon_shader->bindTexture(LLShaderMgr::DIFFUSE_MAP, tex_a, LLTexUnit::TT_TEXTURE);
                //moon_shader->bindTexture(LLShaderMgr::ALTERNATE_DIFFUSE_MAP, tex_b, LLTexUnit::TT_TEXTURE);
            }

            LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

            F32 moon_brightness = (float)psky->getMoonBrightness();

            moon_shader->uniform1f(LLShaderMgr::MOON_BRIGHTNESS, moon_brightness);
            moon_shader->uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, gSky.mVOSkyp->getMoon().getColor().mV);
            moon_shader->uniform4fv(LLShaderMgr::DIFFUSE_COLOR, 1, color.mV);
            //moon_shader->uniform1f(LLShaderMgr::BLEND_FACTOR, blend_factor);
            moon_shader->uniform3fv(LLShaderMgr::DEFERRED_MOON_DIR, 1, psky->getMoonDirection().mV); // shader: moon_dir

            // <plus> research/204: the pass state
            if (pass_state.declaring())
            {
                pass_state.declare(world_sky_pass_state(*moon_shader, false, GL_ONE_MINUS_SRC_ALPHA)
                                   .sampler(world_pass_channel(*moon_shader, LLShaderMgr::DIFFUSE_MAP)));
            }
            pass_state.check(1);
            LLRHI::PassStateWrite write_through(pass_state); // research/207 T4: on, the declaration for the disc
            // </plus>

            face->renderIndexed();

            end_sky_units(); // <plus> research/204 10 (vk-s4c2): was the unbinds of units 0 and 1 </plus>

            moon_shader->unbind();
        }
    }

    gGL.popMatrix();
}

void LLDrawPoolWLSky::renderDeferred(S32 pass)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_WL_SKY);
    if (!gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY) || gSky.mVOSkyp.isNull())
    {
        return;
    }

    // TODO: remove gSky.mVOSkyp and fold sun/moon into LLVOWLSky
    gSky.mVOSkyp->updateGeometry(gSky.mVOSkyp->mDrawable);

    const F32 camHeightLocal = LLEnvironment::instance().getCamHeight();

    LLVector3 const & origin = LLViewerCamera::getInstance()->getOrigin();

    if (gPipeline.canUseWindLightShaders())
    {
        renderSkyHazeDeferred(origin, camHeightLocal);
        renderHeavenlyBodies();
        if (!gCubeSnapshot)
        {
            renderStarsDeferred(origin);
            renderStarMilkyDeferred(origin); // <plus> research/224: the baked Milky Way </plus>
        }
        renderMoonDeferred(); // <plus> the moon over the stars (research/moon-quad-cuts-milkyway) </plus>

        if (!gCubeSnapshot || gPipeline.mReflectionMapManager.isRadiancePass()) // don't draw clouds in irradiance maps to avoid popping
        {
            // <plus> plus/volclouds (research/186): the volumetric program while PlusVolumetricClouds is on
            renderSkyCloudsDeferred(origin, camHeightLocal, plus::volclouds::wanted() ? &gDeferredWLCloudVolProgram : cloud_shader);
            // </plus>
        }
    }
}



LLViewerTexture* LLDrawPoolWLSky::getTexture()
{
    return NULL;
}

void LLDrawPoolWLSky::resetDrawOrders()
{
}

//static
void LLDrawPoolWLSky::cleanupGL()
{
}

//static
void LLDrawPoolWLSky::restoreGL()
{
}
