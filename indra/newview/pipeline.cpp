/**
 * @file pipeline.cpp
 * @brief Rendering pipeline.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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
// <plus> research/295: the rebuild causes line
#include <iomanip>
#include <sstream>
// </plus>
#include "llrhi.h"
#include "llrhigl.h"
#include "llrhiocclusion.h" // LLRHI native (research/137 N8)
#include "llrhihash.h" // <plus> research/273 #9: FNV-1a </plus>
#include "llthread.h"
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// library includes
#include "llimagepng.h"
#include "llaudioengine.h" // For debugging.
#include "llerror.h"
#include "llviewercontrol.h"
#include "llfasttimer.h"
#include "llfontgl.h"
#include "llfontvertexbuffer.h"
#include "llnamevalue.h"
#include "llpointer.h"
#include "llprimitive.h"
#include "llvolume.h"
#include "material_codes.h"
#include "v3color.h"
#include "llui.h"
#include "llglheaders.h"
#include "llrender.h"
#include "llstartup.h"
#include "lltrans.h"
#include "llwindow.h"   // swapBuffers()

// newview includes
#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llframetimer.h"
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llimageworker.h"
#include "lldrawable.h"
#include "lldrawpoolalpha.h"
#include "lldrawpoolavatar.h"
#include "lldrawpoolbump.h"
#include "lldrawpooltree.h"
#include "lldrawpoolwater.h"
#include "llface.h"
#include "llfeaturemanager.h"
#include "llfloatertelehub.h"
#include "llfloaterreg.h"
#include "llhudmanager.h"
#include "llhudnametag.h"
#include "llhudtext.h"
#include "lllightconstants.h"
#include "llmeshrepository.h"
#include "llpipelinelistener.h"
#include "llresmgr.h"
#include "llselectmgr.h"
#include "llsky.h"
#include "lltracker.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "llviewercamera.h"
#include "llviewermediafocus.h"
#include "llviewertexturelist.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h" // for audio debugging.
#include "llviewerwindow.h" // For getSpinAxis
#include "llvoavatarself.h"
#include "lltoolmorph.h"    // LLRHI: LLVisualParamHint::anyInstances (research/64)
#include <cfloat>
#include "llvocache.h"
#include "llvosky.h"
#include "llvowlsky.h"
#include "llvotree.h"
#include "llvovolume.h"
#include "llvosurfacepatch.h"
#include "llvowater.h"
#include "llvotree.h"
#include "llvopartgroup.h"
#include "llworld.h"
#include "llcubemap.h"
#include "llviewershadermgr.h"
#include "llviewerstats.h"
#include "llviewerjoystick.h"
#include "llviewerdisplay.h"
#include "llspatialpartition.h"
#include "llscenerecords.h" // <plus> research/197 stage 2: the retained scene's views </plus>
#include "llsceneshadow.h" // <plus> research/206 stage 5: the sun shadow cascades from the scene's records </plus>
#include "llrhiframe.h" // <plus> research/208 13.6: LLRHI_SCENE_SHADOW_PROBE </plus>
#include "llmutelist.h"
#include "lltoolpie.h"
#include "llnotifications.h"
#include "llpathinglib.h"
#include "llfloaterpathfindingconsole.h"
#include "llfloaterpathfindingcharacters.h"
#include "llfloatertools.h"
#include "llpanelface.h"
#include "llpathfindingpathtool.h"
#include "llscenemonitor.h"
#include "llprogressview.h"
#include "llcleanup.h"
#include "gltfscenemanager.h"

#include "llenvironment.h"
#include "llsettingsvo.h"

#include "SMAAAreaTex.h"
#include "SMAASearchTex.h"
#include "llerror.h"

// <plus>
#include "plus/activecensus/activecensus.h"
#include "plus/idlecheap/idlecensus.h"
#include "plus/avatarrefl/avatarrefl.h"
#include "plus/cullinto/cullinto.h"
#include "plus/cullinto/cullrecord.h"
#include "plus/cullinto/cullreuse.h"
#include "plus/derez/derez.h" // research/257
#include "plus/focuslock/focuslock.h"
#include "plus/glare/glare.h"
#include "plus/gtao/gtao.h"
#include "lllocallights.h" // research/203 (was plus/lightsmemo)
#include "plus/look/look.h"
#include "plus/atmosphere/atmosphere.h"
#include "plus/rain/rain.h"
#include "plus/retained/retained.h"
#include "plus/shadowbridge/shadowbridge.h"
#include "plus/shadowdist/shadowdist.h"
#include "plus/shadowpenumbra/shadowpenumbra.h"
#include "plus/volclouds/volclouds.h" // research/186
#include "plus/shadowfar/shadowfar.h"
#include "plus/shadowreuse/shadowreuse.h"
#include "plus/shadowshift/shadowshift.h"
#include "plus/skinfill/skinfill.h"
#include "plus/skinsss/skinsss.h"
#include "plus/ssgi/ssgi.h"
#include "plus/ssr/ssr.h"
#include "plus/upscale/upscale.h"
#include "plus/taa/taa.h"
#include "plus/version/version.h"
#include "plus/volumetric/volumetric.h"
#include "plus/wetlook/wetlook.h"
// </plus>
#include "plus/shadowrest/shadowrest.h" // LLRHI native (research/137 N10)
#include "llpassscope.h" // LLRHI native (research/137 N12)

#if LL_CLANG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#elif LL_GNUC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wrestrict"
#endif
#ifndef LL_WINDOWS
#define A_GCC 1
#endif
#define A_CPU 1
#include "app_settings/shaders/class1/deferred/CASF.glsl" // This is also C++
#if LL_CLANG
#pragma clang diagnostic pop
#elif LL_GNUC
#pragma GCC diagnostic pop
#endif

extern bool gSnapshot;
bool gShiftFrame = false;

//cached settings
bool LLPipeline::WindLightUseAtmosShaders;
bool LLPipeline::RenderDeferred;
F32 LLPipeline::RenderDeferredSunWash;
U32 LLPipeline::RenderFSAAType;
U32 LLPipeline::RenderResolutionDivisor;
bool LLPipeline::RenderUIBuffer;
S32 LLPipeline::RenderShadowDetail;
S32 LLPipeline::RenderShadowSplits;
bool LLPipeline::RenderDeferredSSAO;
F32 LLPipeline::RenderShadowResolutionScale;
bool LLPipeline::RenderDelayCreation;
bool LLPipeline::RenderAnimateRes;
bool LLPipeline::FreezeTime;
S32 LLPipeline::DebugBeaconLineWidth;
F32 LLPipeline::RenderHighlightBrightness;
LLColor4 LLPipeline::RenderHighlightColor;
F32 LLPipeline::RenderHighlightThickness;
bool LLPipeline::RenderSpotLightsInNondeferred;
LLColor4 LLPipeline::PreviewAmbientColor;
LLColor4 LLPipeline::PreviewDiffuse0;
LLColor4 LLPipeline::PreviewSpecular0;
LLColor4 LLPipeline::PreviewDiffuse1;
LLColor4 LLPipeline::PreviewSpecular1;
LLColor4 LLPipeline::PreviewDiffuse2;
LLColor4 LLPipeline::PreviewSpecular2;
LLVector3 LLPipeline::PreviewDirection0;
LLVector3 LLPipeline::PreviewDirection1;
LLVector3 LLPipeline::PreviewDirection2;
F32 LLPipeline::RenderGlowMaxExtractAlpha;
F32 LLPipeline::RenderGlowWarmthAmount;
LLVector3 LLPipeline::RenderGlowLumWeights;
LLVector3 LLPipeline::RenderGlowWarmthWeights;
S32 LLPipeline::RenderGlowResolutionPow;
S32 LLPipeline::RenderGlowIterations;
F32 LLPipeline::RenderGlowWidth;
F32 LLPipeline::RenderGlowStrength;
bool LLPipeline::RenderGlowNoise;
bool LLPipeline::RenderDepthOfField;
bool LLPipeline::RenderDepthOfFieldInEditMode;
F32 LLPipeline::CameraFocusTransitionTime;
F32 LLPipeline::CameraFNumber;
F32 LLPipeline::CameraFocalLength;
F32 LLPipeline::CameraFieldOfView;
F32 LLPipeline::RenderShadowNoise;
F32 LLPipeline::RenderShadowBlurSize;
F32 LLPipeline::RenderSSAOScale;
U32 LLPipeline::RenderSSAOMaxScale;
F32 LLPipeline::RenderSSAOFactor;
LLVector3 LLPipeline::RenderSSAOEffect;
F32 LLPipeline::RenderShadowOffsetError;
F32 LLPipeline::RenderShadowBiasError;
F32 LLPipeline::RenderShadowOffset;
F32 LLPipeline::RenderShadowBias;
F32 LLPipeline::RenderSpotShadowOffset;
F32 LLPipeline::RenderSpotShadowBias;
LLDrawable* LLPipeline::RenderSpotLight = nullptr;
F32 LLPipeline::RenderEdgeDepthCutoff;
F32 LLPipeline::RenderEdgeNormCutoff;
LLVector3 LLPipeline::RenderShadowGaussian;
F32 LLPipeline::RenderShadowBlurDistFactor;
bool LLPipeline::RenderDeferredAtmospheric;
F32 LLPipeline::RenderHighlightFadeTime;
F32 LLPipeline::RenderFarClip;
LLVector3 LLPipeline::RenderShadowSplitExponent;
F32 LLPipeline::RenderShadowErrorCutoff;
F32 LLPipeline::RenderShadowFOVCutoff;
bool LLPipeline::CameraOffset;
F32 LLPipeline::CameraMaxCoF;
F32 LLPipeline::CameraDoFResScale;
F32 LLPipeline::RenderAutoHideSurfaceAreaLimit;
bool LLPipeline::RenderScreenSpaceReflections;
S32 LLPipeline::RenderScreenSpaceReflectionIterations;
F32 LLPipeline::RenderScreenSpaceReflectionRayStep;
F32 LLPipeline::RenderScreenSpaceReflectionDistanceBias;
F32 LLPipeline::RenderScreenSpaceReflectionDepthRejectBias;
F32 LLPipeline::RenderScreenSpaceReflectionAdaptiveStepMultiplier;
S32 LLPipeline::RenderScreenSpaceReflectionGlossySamples;
S32 LLPipeline::RenderBufferVisualization;
bool LLPipeline::RenderMirrors;
S32 LLPipeline::RenderHeroProbeUpdateRate;
S32 LLPipeline::RenderHeroProbeConservativeUpdateMultiplier;
bool LLPipeline::RenderAvatarCloth;
LLTrace::EventStatHandle<S64> LLPipeline::sStatBatchSize("renderbatchsize");

const U32 LLPipeline::MAX_PREVIEW_WIDTH = 512;

const F32 BACKLIGHT_DAY_MAGNITUDE_OBJECT = 0.1f;
const F32 BACKLIGHT_NIGHT_MAGNITUDE_OBJECT = 0.08f;
const F32 ALPHA_BLEND_CUTOFF = 0.598f;
const F32 DEFERRED_LIGHT_FALLOFF = 0.5f;
const U32 DEFERRED_VB_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_TEXCOORD1;

extern S32 gBoxFrame;
extern bool gDisplaySwapBuffers;
extern bool gDebugGL;
extern bool gCubeSnapshot;
extern bool gSnapshotNoPost;

bool    gAvatarBacklight = false;

bool    gDebugPipeline = false;
LLPipeline gPipeline;
const LLMatrix4* gGLLastMatrix = NULL;

LLTrace::BlockTimerStatHandle FTM_RENDER_GEOMETRY("Render Geometry");
LLTrace::BlockTimerStatHandle FTM_RENDER_GRASS("Grass");
LLTrace::BlockTimerStatHandle FTM_RENDER_INVISIBLE("Invisible");
LLTrace::BlockTimerStatHandle FTM_RENDER_SHINY("Shiny");
LLTrace::BlockTimerStatHandle FTM_RENDER_SIMPLE("Simple");
LLTrace::BlockTimerStatHandle FTM_RENDER_TERRAIN("Terrain");
LLTrace::BlockTimerStatHandle FTM_RENDER_TREES("Trees");
LLTrace::BlockTimerStatHandle FTM_RENDER_UI("UI");
LLTrace::BlockTimerStatHandle FTM_RENDER_WATER("Water");
LLTrace::BlockTimerStatHandle FTM_RENDER_WL_SKY("Windlight Sky");
LLTrace::BlockTimerStatHandle FTM_RENDER_ALPHA("Alpha Objects");
LLTrace::BlockTimerStatHandle FTM_RENDER_CHARACTERS("Avatars");
LLTrace::BlockTimerStatHandle FTM_RENDER_BUMP("Bump");
LLTrace::BlockTimerStatHandle FTM_RENDER_MATERIALS("Render Materials");
LLTrace::BlockTimerStatHandle FTM_RENDER_FULLBRIGHT("Fullbright");
LLTrace::BlockTimerStatHandle FTM_RENDER_GLOW("Glow");
LLTrace::BlockTimerStatHandle FTM_GEO_UPDATE("Geo Update");
LLTrace::BlockTimerStatHandle FTM_POOLRENDER("RenderPool");
LLTrace::BlockTimerStatHandle FTM_POOLS("Pools");
LLTrace::BlockTimerStatHandle FTM_DEFERRED_POOLRENDER("RenderPool (Deferred)");
LLTrace::BlockTimerStatHandle FTM_DEFERRED_POOLS("Pools (Deferred)");
LLTrace::BlockTimerStatHandle FTM_POST_DEFERRED_POOLRENDER("RenderPool (Post)");
LLTrace::BlockTimerStatHandle FTM_POST_DEFERRED_POOLS("Pools (Post)");
LLTrace::BlockTimerStatHandle FTM_STATESORT("Sort Draw State");
LLTrace::BlockTimerStatHandle FTM_PIPELINE("Pipeline");
LLTrace::BlockTimerStatHandle FTM_CLIENT_COPY("Client Copy");
LLTrace::BlockTimerStatHandle FTM_RENDER_DEFERRED("Deferred Shading");

LLTrace::BlockTimerStatHandle FTM_RENDER_UI_HUD("HUD");
LLTrace::BlockTimerStatHandle FTM_RENDER_UI_3D("3D");
LLTrace::BlockTimerStatHandle FTM_RENDER_UI_2D("2D");

static LLTrace::BlockTimerStatHandle FTM_STATESORT_DRAWABLE("Sort Drawables");

static LLStaticHashedString sTint("tint");
static LLStaticHashedString sAmbiance("ambiance");
static LLStaticHashedString sAlphaScale("alpha_scale");
static LLStaticHashedString sNormMat("norm_mat");
static LLStaticHashedString sOffset("offset");
static LLStaticHashedString sScreenRes("screenRes");
static LLStaticHashedString sDelta("delta");
static LLStaticHashedString sDistFactor("dist_factor");
static LLStaticHashedString sKern("kern");

// GPU pass timing (LLRHI::gpuMark): the report line a draw pool's work goes to.
// After the deferred lighting only the alpha pools are "alpha" (research/54:
// the other pools drawn then, and the HUD attachments, have names of their own;
// LLDrawPoolAlpha splits its segment further).
static const char* gpu_pool_name(U32 type, bool post_deferred)
{
    if (LLPipeline::sRenderingHUDs)
    {
        return "hud";
    }
    switch (type)
    {
    case LLDrawPool::POOL_SKY:
    case LLDrawPool::POOL_WL_SKY:
        return "sky";
    case LLDrawPool::POOL_WATEREXCLUSION:
    case LLDrawPool::POOL_VOIDWATER:
    case LLDrawPool::POOL_WATER:
        return "water";
    case LLDrawPool::POOL_GLOW:
        return post_deferred ? "glow" : "gbuffer";
    case LLDrawPool::POOL_ALPHA_PRE_WATER:
    case LLDrawPool::POOL_ALPHA_POST_WATER:
        return post_deferred ? "alpha" : "gbuffer";
    default:
        break;
    }
    if (!post_deferred)
    {
        return "gbuffer";
    }
    switch (type)
    {
    case LLDrawPool::POOL_FULLBRIGHT:
    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        return "fullbright";
    case LLDrawPool::POOL_BUMP:
        return "fullbright shiny";
    case LLDrawPool::POOL_AVATAR:
        return "post avatar";
    case LLDrawPool::POOL_GLTF_PBR:
    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        return "post pbr";
    default:
        return "post other";
    }
}
static LLStaticHashedString sKernScale("kern_scale");
static LLStaticHashedString sSmaaRTMetrics("SMAA_RT_METRICS");

// LLRHI (research/52): moves with every change of mNearbyLights, so that the
// probe faces of a frame can share what the light loop reads of each light.
static U32 sNearbyLightsVersion = 0;

//----------------------------------------

void drawBox(const LLVector4a& c, const LLVector4a& r);
void drawBoxOutline(const LLVector3& pos, const LLVector3& size);
U32 nhpo2(U32 v);
LLVertexBuffer* ll_create_cube_vb(U32 type_mask);

void display_update_camera();
//----------------------------------------

S32     LLPipeline::sCompiles = 0;

bool    LLPipeline::sPickAvatar = true;
bool    LLPipeline::sDynamicLOD = true;
bool    LLPipeline::sShowHUDAttachments = true;
bool    LLPipeline::sRenderMOAPBeacons = false;
bool    LLPipeline::sRenderPhysicalBeacons = true;
bool    LLPipeline::sRenderScriptedBeacons = false;
bool    LLPipeline::sRenderScriptedTouchBeacons = true;
bool    LLPipeline::sRenderParticleBeacons = false;
bool    LLPipeline::sRenderSoundBeacons = false;
bool    LLPipeline::sRenderBeacons = false;
bool    LLPipeline::sRenderHighlight = true;
LLRender::eTexIndex LLPipeline::sRenderHighlightTextureChannel = LLRender::DIFFUSE_MAP;
bool    LLPipeline::sForceOldBakedUpload = false;
S32     LLPipeline::sUseOcclusion = 0;
bool    LLPipeline::sAutoMaskAlphaDeferred = true;
bool    LLPipeline::sAutoMaskAlphaNonDeferred = false;
bool    LLPipeline::sRenderTransparentWater = true;
bool    LLPipeline::sBakeSunlight = false;
bool    LLPipeline::sNoAlpha = false;
bool    LLPipeline::sUseFarClip = true;
bool    LLPipeline::sShadowRender = false;
bool    LLPipeline::sRenderGlow = false;
bool    LLPipeline::sReflectionRender = false;
bool    LLPipeline::sDistortionRender = false;
bool    LLPipeline::sImpostorRender = false;
bool    LLPipeline::sImpostorRenderAlphaDepthPass = false;
bool    LLPipeline::sUnderWaterRender = false;
bool    LLPipeline::sTextureBindTest = false;
bool    LLPipeline::sRenderAttachedLights = true;
bool    LLPipeline::sRenderAttachedParticles = true;
bool    LLPipeline::sRenderDeferred = false;
bool    LLPipeline::sReflectionProbesEnabled = false;
S32     LLPipeline::sVisibleLightCount = 0;
bool    LLPipeline::sRenderingHUDs;
bool    LLPipeline::sProbeFaceRender = false;
U32     LLPipeline::sProbeCullSerial = 0;
U32     LLPipeline::sRHIListCullKinds = 0;
F32     LLPipeline::sDistortionWaterClipPlaneMargin = 1.0125f;

// EventHost API LLPipeline listener.
static LLPipelineListener sPipelineListener;

static LLCullResult* sCull = NULL;

// <plus> research/207 6.3: validate_framebuffer_object gone (never called) </plus>

// Add color attachments for deferred rendering
// target -- RenderTarget to add attachments to
bool addDeferredAttachments(LLRenderTarget& target, bool for_impostor = false)
{
    U32 orm = GL_RGBA;
    U32 norm = GL_RGBA16;
    U32 emissive = GL_RGB16F;

    static LLCachedControl<bool> has_emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);
    static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
    bool hdr = has_hdr() && gGLManager.mGLVersion > 4.05f;

    if (!hdr)
    {
        norm = GL_RGB10_A2;
        emissive = GL_RGB;
    }

    bool valid = true;
    valid      = valid && target.addColorAttachment(orm);    // frag-data[1] specular OR PBR ORM
    valid      = valid && target.addColorAttachment(norm);
    if (has_emissive)
    {
        valid = valid && target.addColorAttachment(emissive); // frag_data[3] PBR emissive OR material env intensity
    }

    return valid;
}

LLPipeline::LLPipeline() :
    mBackfaceCull(false),
    mMatrixOpCount(0),
    mTextureMatrixOps(0),
    mNumVisibleNodes(0),
    mNumVisibleFaces(0),
    mPoissonOffset(0),

    mInitialized(false),
    mShadersLoaded(false),
    mTransformFeedbackPrimitives(0),
    mRenderDebugFeatureMask(0),
    mRenderDebugMask(0),
    mOldRenderDebugMask(0),
    mMeshDirtyQueryObject(0),
    mGroupQ1Locked(false),
    mResetVertexBuffers(false),
    mLastRebuildPool(NULL),
    mLightMask(0),
    mLightMovingMask(0)
{
    mNoiseMap = 0;
    mTrueNoiseMap = 0;
    mLightFunc = 0;

    for(U32 i = 0; i < 8; i++)
    {
        mHWLightColors[i] = LLColor4::black;
    }
}

void LLPipeline::connectRefreshCachedSettingsSafe(const std::string name)
{
    LLPointer<LLControlVariable> cntrl_ptr = gSavedSettings.getControl(name);
    if ( cntrl_ptr.isNull() )
    {
        LL_WARNS() << "Global setting name not found:" << name << LL_ENDL;
    }
    else
    {
        cntrl_ptr->getCommitSignal()->connect(boost::bind(&LLPipeline::refreshCachedSettings));
    }
}

void LLPipeline::init()
{
    refreshCachedSettings();

    mRT = &mMainRT;

    gOctreeMaxCapacity = gSavedSettings.getU32("OctreeMaxNodeCapacity");
    gOctreeMinSize = gSavedSettings.getF32("OctreeMinimumNodeSize");
    sDynamicLOD = gSavedSettings.getBOOL("RenderDynamicLOD");
    sRenderAttachedLights = gSavedSettings.getBOOL("RenderAttachedLights");
    sRenderAttachedParticles = gSavedSettings.getBOOL("RenderAttachedParticles");

    mReflectionMapManager.refreshSettings();

    mInitialized = true;

    stop_glerror();

    //create render pass pools
    getPool(LLDrawPool::POOL_WATEREXCLUSION);
    getPool(LLDrawPool::POOL_ALPHA_PRE_WATER);
    getPool(LLDrawPool::POOL_ALPHA_POST_WATER);
    getPool(LLDrawPool::POOL_SIMPLE);
    getPool(LLDrawPool::POOL_ALPHA_MASK);
    getPool(LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK);
    getPool(LLDrawPool::POOL_GRASS);
    getPool(LLDrawPool::POOL_FULLBRIGHT);
    getPool(LLDrawPool::POOL_BUMP);
    getPool(LLDrawPool::POOL_MATERIALS);
    getPool(LLDrawPool::POOL_GLOW);
    getPool(LLDrawPool::POOL_GLTF_PBR);
    getPool(LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK);

    resetFrameStats();

    if (gSavedSettings.getBOOL("DisableAllRenderFeatures"))
    {
        clearAllRenderDebugFeatures();
    }
    else
    {
        setAllRenderDebugFeatures(); // By default, all debugging features on
    }
    clearAllRenderDebugDisplays(); // All debug displays off

    if (gSavedSettings.getBOOL("DisableAllRenderTypes"))
    {
        clearAllRenderTypes();
    }
    else if (gNonInteractive)
    {
        clearAllRenderTypes();
    }
    else
    {
        setAllRenderTypes(); // By default, all rendering types start enabled
    }

    // make sure RenderPerformanceTest persists (hackity hack hack)
    // disables non-object rendering (UI, sky, water, etc)
    if (gSavedSettings.getBOOL("RenderPerformanceTest"))
    {
        gSavedSettings.setBOOL("RenderPerformanceTest", false);
        gSavedSettings.setBOOL("RenderPerformanceTest", true);
    }

    mOldRenderDebugMask = mRenderDebugMask;

    mBackfaceCull = true;

    // Enable features
    LLViewerShaderMgr::instance()->setShaders();

    for (U32 i = 0; i < 2; ++i)
    {
        mSpotLightFade[i] = 1.f;
    }

    if (mCubeVB.isNull())
    {
        mCubeVB = ll_create_cube_vb(LLVertexBuffer::MAP_VERTEX);
    }

    mDeferredVB = new LLVertexBuffer(DEFERRED_VB_MASK);
    mDeferredVB->allocateBuffer(8, 0);

    {
        mScreenTriangleVB = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX);
        mScreenTriangleVB->allocateBuffer(3, 0);
        LLStrider<LLVector3> vert;
        mScreenTriangleVB->getVertexStrider(vert);

        vert[0].set(-1, 1, 0);
        vert[1].set(-1, -3, 0);
        vert[2].set(3, 1, 0);

        mScreenTriangleVB->unmapBuffer();
    }

    //
    // Update all settings to trigger a cached settings refresh
    //
    connectRefreshCachedSettingsSafe("RenderAutoMaskAlphaDeferred");
    connectRefreshCachedSettingsSafe("RenderAutoMaskAlphaNonDeferred");
    connectRefreshCachedSettingsSafe("RenderUseFarClip");
    connectRefreshCachedSettingsSafe("RenderAvatarMaxNonImpostors");
    connectRefreshCachedSettingsSafe("UseOcclusion");
    // DEPRECATED -- connectRefreshCachedSettingsSafe("WindLightUseAtmosShaders");
    // DEPRECATED -- connectRefreshCachedSettingsSafe("RenderDeferred");
    connectRefreshCachedSettingsSafe("RenderDeferredSunWash");
    connectRefreshCachedSettingsSafe("RenderFSAAType");
    connectRefreshCachedSettingsSafe("RenderResolutionDivisor");
    connectRefreshCachedSettingsSafe("RenderUIBuffer");
    connectRefreshCachedSettingsSafe("RenderShadowDetail");
    connectRefreshCachedSettingsSafe("RenderShadowSplits");
    connectRefreshCachedSettingsSafe("RenderDeferredSSAO");
    connectRefreshCachedSettingsSafe("RenderShadowResolutionScale");
    connectRefreshCachedSettingsSafe("RenderDelayCreation");
    connectRefreshCachedSettingsSafe("RenderAnimateRes");
    connectRefreshCachedSettingsSafe("FreezeTime");
    connectRefreshCachedSettingsSafe("DebugBeaconLineWidth");
    connectRefreshCachedSettingsSafe("RenderHighlightBrightness");
    connectRefreshCachedSettingsSafe("RenderHighlightColor");
    connectRefreshCachedSettingsSafe("RenderHighlightThickness");
    connectRefreshCachedSettingsSafe("RenderSpotLightsInNondeferred");
    connectRefreshCachedSettingsSafe("PreviewAmbientColor");
    connectRefreshCachedSettingsSafe("PreviewDiffuse0");
    connectRefreshCachedSettingsSafe("PreviewSpecular0");
    connectRefreshCachedSettingsSafe("PreviewDiffuse1");
    connectRefreshCachedSettingsSafe("PreviewSpecular1");
    connectRefreshCachedSettingsSafe("PreviewDiffuse2");
    connectRefreshCachedSettingsSafe("PreviewSpecular2");
    connectRefreshCachedSettingsSafe("PreviewDirection0");
    connectRefreshCachedSettingsSafe("PreviewDirection1");
    connectRefreshCachedSettingsSafe("PreviewDirection2");
    connectRefreshCachedSettingsSafe("RenderGlowMaxExtractAlpha");
    connectRefreshCachedSettingsSafe("RenderGlowWarmthAmount");
    connectRefreshCachedSettingsSafe("RenderGlowLumWeights");
    connectRefreshCachedSettingsSafe("RenderGlowWarmthWeights");
    connectRefreshCachedSettingsSafe("RenderGlowResolutionPow");
    connectRefreshCachedSettingsSafe("RenderGlowIterations");
    connectRefreshCachedSettingsSafe("RenderGlowWidth");
    connectRefreshCachedSettingsSafe("RenderGlowStrength");
    connectRefreshCachedSettingsSafe("RenderGlowNoise");
    connectRefreshCachedSettingsSafe("RenderDepthOfField");
    connectRefreshCachedSettingsSafe("RenderDepthOfFieldInEditMode");
    connectRefreshCachedSettingsSafe("CameraFocusTransitionTime");
    connectRefreshCachedSettingsSafe("CameraFNumber");
    connectRefreshCachedSettingsSafe("CameraFocalLength");
    connectRefreshCachedSettingsSafe("CameraFieldOfView");
    connectRefreshCachedSettingsSafe("RenderShadowNoise");
    connectRefreshCachedSettingsSafe("RenderShadowBlurSize");
    connectRefreshCachedSettingsSafe("RenderSSAOScale");
    connectRefreshCachedSettingsSafe("RenderSSAOMaxScale");
    connectRefreshCachedSettingsSafe("RenderSSAOFactor");
    connectRefreshCachedSettingsSafe("RenderSSAOEffect");
    connectRefreshCachedSettingsSafe("RenderShadowOffsetError");
    connectRefreshCachedSettingsSafe("RenderShadowBiasError");
    connectRefreshCachedSettingsSafe("RenderShadowOffset");
    connectRefreshCachedSettingsSafe("RenderShadowBias");
    connectRefreshCachedSettingsSafe("RenderSpotShadowOffset");
    connectRefreshCachedSettingsSafe("RenderSpotShadowBias");
    connectRefreshCachedSettingsSafe("RenderEdgeDepthCutoff");
    connectRefreshCachedSettingsSafe("RenderEdgeNormCutoff");
    connectRefreshCachedSettingsSafe("RenderShadowGaussian");
    connectRefreshCachedSettingsSafe("RenderShadowBlurDistFactor");
    connectRefreshCachedSettingsSafe("RenderDeferredAtmospheric");
    connectRefreshCachedSettingsSafe("RenderHighlightFadeTime");
    connectRefreshCachedSettingsSafe("RenderFarClip");
    connectRefreshCachedSettingsSafe("RenderShadowSplitExponent");
    connectRefreshCachedSettingsSafe("RenderShadowErrorCutoff");
    connectRefreshCachedSettingsSafe("RenderShadowFOVCutoff");
    connectRefreshCachedSettingsSafe("CameraOffset");
    connectRefreshCachedSettingsSafe("CameraMaxCoF");
    connectRefreshCachedSettingsSafe("CameraDoFResScale");
    connectRefreshCachedSettingsSafe("RenderAutoHideSurfaceAreaLimit");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflections");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflectionIterations");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflectionRayStep");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflectionDistanceBias");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflectionDepthRejectBias");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflectionAdaptiveStepMultiplier");
    connectRefreshCachedSettingsSafe("RenderScreenSpaceReflectionGlossySamples");
    connectRefreshCachedSettingsSafe("RenderBufferVisualization");
    connectRefreshCachedSettingsSafe("RenderMirrors");
    connectRefreshCachedSettingsSafe("RenderHeroProbeUpdateRate");
    connectRefreshCachedSettingsSafe("RenderHeroProbeConservativeUpdateMultiplier");
    connectRefreshCachedSettingsSafe("RenderAvatarCloth");

    LLPointer<LLControlVariable> cntrl_ptr = gSavedSettings.getControl("CollectFontVertexBuffers");
    if (cntrl_ptr.notNull())
    {
        cntrl_ptr->getCommitSignal()->connect([](LLControlVariable* control, const LLSD& value, const LLSD& previous)
        {
            bool enable_buffers = control->getValue().asBoolean();
            LLFontVertexBuffer::enableBufferCollection(enable_buffers);
            LLFontWidthBuffer::enableBufferCollection(enable_buffers);
        });
    }

    cntrl_ptr = gSavedSettings.getControl("CollectUIImageVertexBuffers");
    if (cntrl_ptr.notNull())
    {
        cntrl_ptr->getCommitSignal()->connect([](LLControlVariable* control, const LLSD& value, const LLSD& previous)
        {
            bool enable_buffers = control->getValue().asBoolean();
            LLUIImage::enableDisplayListsCollection(enable_buffers);
        });
    }
}

LLPipeline::~LLPipeline()
{
}

void LLPipeline::cleanup()
{
    assertInitialized();

    mGroupQ1.clear() ;

    for(pool_set_t::iterator iter = mPools.begin();
        iter != mPools.end(); )
    {
        pool_set_t::iterator curiter = iter++;
        LLDrawPool* poolp = *curiter;
        if (poolp->isFacePool())
        {
            LLFacePool* face_pool = (LLFacePool*) poolp;
            if (face_pool->mReferences.empty())
            {
                mPools.erase(curiter);
                removeFromQuickLookup( poolp );
                delete poolp;
            }
        }
        else
        {
            mPools.erase(curiter);
            removeFromQuickLookup( poolp );
            delete poolp;
        }
    }

    if (!mTerrainPools.empty())
    {
        LL_WARNS() << "Terrain Pools not cleaned up" << LL_ENDL;
    }
    if (!mTreePools.empty())
    {
        LL_WARNS() << "Tree Pools not cleaned up" << LL_ENDL;
    }

    delete mAlphaPoolPreWater;
    mAlphaPoolPreWater = nullptr;
    delete mAlphaPoolPostWater;
    mAlphaPoolPostWater = nullptr;
    delete mSkyPool;
    mSkyPool = NULL;
    delete mTerrainPool;
    mTerrainPool = NULL;
    delete mWaterPool;
    mWaterPool = NULL;
    delete mSimplePool;
    mSimplePool = NULL;
    delete mFullbrightPool;
    mFullbrightPool = NULL;
    delete mGlowPool;
    mGlowPool = NULL;
    delete mBumpPool;
    mBumpPool = NULL;
    // don't delete wl sky pool it was handled above in the for loop
    //delete mWLSkyPool;
    mWLSkyPool = NULL;
    delete mWaterExclusionPool;
    mWaterExclusionPool = nullptr;

    releaseGLBuffers();

    mFaceSelectImagep = NULL;

    mMovedList.clear();
    mMovedBridge.clear();
    mShiftList.clear();

    mInitialized = false;

    mDeferredVB = NULL;
    mScreenTriangleVB = nullptr;

    mCubeVB = NULL;

    mReflectionMapManager.cleanup();
    mHeroProbeManager.cleanup();
}

//============================================================================

void LLPipeline::destroyGL()
{
    stop_glerror();
    unloadShaders();
    mHighlightFaces.clear();

    resetDrawOrders();

    releaseGLBuffers();

    if (mMeshDirtyQueryObject)
    {
        glDeleteQueries(1, &mMeshDirtyQueryObject);
        mMeshDirtyQueryObject = 0;
    }
}

void LLPipeline::requestResizeScreenTexture()
{
    gResizeScreenTexture = true;
}

void LLPipeline::requestResizeShadowTexture()
{
    gResizeShadowTexture = true;
}

void LLPipeline::resizeShadowTexture()
{
    releaseSunShadowTargets();
    releaseSpotShadowTargets();
    allocateShadowBuffer(mRT->width, mRT->height);
    gResizeShadowTexture = false;
}

void LLPipeline::resizeScreenTexture()
{
    if (gPipeline.shadersLoaded())
    {
        GLuint resX = gViewerWindow->getWorldViewWidthRaw();
        GLuint resY = gViewerWindow->getWorldViewHeightRaw();

        // <plus> plus/upscale (research/165 review 4): nothing made for an empty (minimized) window; the window is
        // compared with the size the targets were made for and the drawn size with the one wanted now (was: the
        // window with mRT->screen, which differ whenever the view is drawn smaller)
        if (resX == 0 || resY == 0)
        {
            return;
        }
        if (gResizeScreenTexture || (resX != mRT->width) || (resY != mRT->height) ||
            !plus::upscale::drawnSizeMatches(resX, resY, RenderResolutionDivisor, mRT->screen.getWidth(), mRT->screen.getHeight()))
        // </plus>
        {
            releaseScreenBuffers();
            releaseSunShadowTargets();
            releaseSpotShadowTargets();
            allocateScreenBuffer(resX,resY);
            gResizeScreenTexture = false;
        }
    }
}

bool LLPipeline::allocateScreenBuffer(U32 resX, U32 resY)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    eFBOStatus ret = doAllocateScreenBuffer(resX, resY);
    // <plus> skin mark (plus/skinsss): the main view's depth gets a stencil while skin is marked
    plus::skinsss::onScreenBuffers();
    // </plus>

    return ret == FBO_SUCCESS_FULLRES;
}


LLPipeline::eFBOStatus LLPipeline::doAllocateScreenBuffer(U32 resX, U32 resY)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    // try to allocate screen buffers at requested resolution and samples
    // - on failure, shrink number of samples and try again
    // - if not multisampled, shrink resolution and try again (favor X resolution over Y)
    // Make sure to call "releaseScreenBuffers" after each failure to cleanup the partially loaded state

    // refresh cached settings here to protect against inconsistent event handling order
    refreshCachedSettings();

    eFBOStatus ret = FBO_SUCCESS_FULLRES;
    if (!allocateScreenBufferInternal(resX, resY))
    {
        //failed to allocate at requested specification, return false
        ret = FBO_FAILURE;

        releaseScreenBuffers();

        //reduce resolution
        while (resY > 0 && resX > 0)
        {
            resY /= 2;
            if (allocateScreenBufferInternal(resX, resY))
            {
                return FBO_SUCCESS_LOWRES;
            }
            releaseScreenBuffers();

            resX /= 2;
            if (allocateScreenBufferInternal(resX, resY))
            {
                return FBO_SUCCESS_LOWRES;
            }
            releaseScreenBuffers();
        }

        LL_WARNS() << "Unable to allocate screen buffer at any resolution!" << LL_ENDL;
    }

    return ret;
}

bool LLPipeline::allocateScreenBufferInternal(U32 resX, U32 resY)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    bool has_hdr = gSavedSettings.getBOOL("RenderHDREnabled");
    bool hdr = gGLManager.mGLVersion > 4.05f && has_hdr;

    if (mRT == &mMainRT)
    { // hacky -- allocate auxillary buffer

        gCubeSnapshot = true;

        if (sReflectionProbesEnabled)
        {
            mReflectionMapManager.initReflectionMaps();
        }

        mRT = &mAuxillaryRT;
        U32 res = mReflectionMapManager.mProbeResolution * 4;  //multiply by 4 because probes will be 16x super sampled
        allocateScreenBufferInternal(res, res);

        if (RenderMirrors)
        {
            mHeroProbeManager.initReflectionMaps();
            res = mHeroProbeManager.mProbeResolution;  // We also scale the hero probe RT to the probe res since we don't super sample it.
            mRT = &mHeroProbeRT;
            allocateScreenBufferInternal(res, res);
        }

        mRT = &mMainRT;
        gCubeSnapshot = false;
    }

    // remember these dimensions
    mRT->width = resX;
    mRT->height = resY;

    // <plus> plus/upscale (research/165): the main view drawn smaller (and upscaled in renderFinalize), otherwise the
    // official divisor as before (was: resX and resY divided by RenderResolutionDivisor here)
    const bool plus_main_view = mRT == &mMainRT;
    const bool plus_upscaled = plus::upscale::drawSize(resX, resY, plus_main_view, RenderResolutionDivisor, resX, resY);
    if (plus_main_view)
    {
        plus::upscale::noteMainTargets(plus_upscaled, resX, resY);
    }
    // </plus>

    S32 shadow_detail = RenderShadowDetail;
    bool ssao = RenderDeferredSSAO;

    //allocate deferred rendering color buffers
    if (!mRT->deferredScreen.allocate(resX, resY, GL_RGBA, true)) return false;
    if (!addDeferredAttachments(mRT->deferredScreen)) return false;

    GLuint screenFormat = hdr ? GL_RGBA16F : GL_RGBA;

    if (!mRT->screen.allocate(resX, resY, GL_RGBA16F)) return false;

    mRT->deferredScreen.shareDepthBuffer(mRT->screen);

    if (hdr || shadow_detail > 0 || ssao || RenderDepthOfField)
    { //only need mRT->deferredLight for hdr OR shadows OR ssao OR dof
        if (!mRT->deferredLight.allocate(resX, resY, screenFormat)) return false;
    }
    else
    {
        mRT->deferredLight.release();
    }

    // <plus> plus/upscale (research/165 review 1): the shadow maps follow the window's size, not the drawn one (was: resX, resY)
    allocateShadowBuffer(plus_upscaled ? mRT->width : resX, plus_upscaled ? mRT->height : resY);
    // </plus>

    if (!gCubeSnapshot) // hack to not re-allocate various targets for cube snapshots
    {
        U32 post_color_fmt = gSavedSettings.getBOOL("RenderHighPrecisionPostProcess") ? GL_RGBA16F : GL_RGBA;

        if (RenderUIBuffer)
        {
            if (!mUIScreen.allocate(resX, resY, GL_RGBA))
            {
                return false;
            }
        }

        if (plus::upscale::effectiveAAType(RenderFSAAType) > 0) // <plus> plus/upscale: at least FXAA while upscaled (was: RenderFSAAType > 0) </plus>
        {
            if (!mFXAAMap.allocate(resX, resY, post_color_fmt)) return false;
            if (RenderFSAAType == 2)
            {
                if (!mSMAABlendBuffer.allocate(resX, resY, post_color_fmt, false)) return false;
            }
        }
        else
        {
            mFXAAMap.release();
            mSMAABlendBuffer.release();
        }

        //water reflection texture (always needed as scratch space whether or not transparent water is enabled)
        mWaterDis.allocate(resX, resY, screenFormat, true);

        if(RenderScreenSpaceReflections)
        {
            mSceneMap.allocate(resX, resY, screenFormat, true);
        }
        else
        {
            mSceneMap.release();
        }

        mPostPingMap.allocate(resX, resY, post_color_fmt);
        mPostPongMap.allocate(resX, resY, post_color_fmt);

        // The water exclusion mask needs its own depth buffer so we can take care of the problem of multiple water planes.
        // Should we ever make water not just a plane, it also aids with that as well as the water planes will be rendered into the mask.
        // Why do we do this? Because it saves us some janky logic in the exclusion shader when we generate the mask.
        // Regardless, this should always only be an R8 texture unless we choose to start having multiple kinds of exclusion that 8 bits can't handle.
        // - Geenz 2025-02-06
        bool success = mWaterExclusionMask.allocate(resX, resY, GL_R8, true);

        assert(success);

        // used to scale down textures
        // See LLViwerTextureList::updateImagesCreateTextures and LLImageGL::scaleDown
        mDownResMap.allocate(1024, 1024, GL_RGBA);

        mBakeMap.allocate(LLAvatarAppearanceDefines::SCRATCH_TEX_WIDTH, LLAvatarAppearanceDefines::SCRATCH_TEX_HEIGHT, GL_RGBA);
    }
    //HACK make screenbuffer allocations start failing after 30 seconds
    if (gSavedSettings.getBOOL("SimulateFBOFailure"))
    {
        return false;
    }

    gGL.getTexUnit(0)->disable();

    stop_glerror();

    return true;
}

// must be even to avoid a stripe in the horizontal shadow blur
inline U32 BlurHappySize(U32 x, F32 scale) { return U32( x * scale + 16.0f) & ~0xF; }

bool LLPipeline::allocateShadowBuffer(U32 resX, U32 resY)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    S32 shadow_detail = RenderShadowDetail;

    F32 scale = gCubeSnapshot ? 1.0f : llmax(0.f, RenderShadowResolutionScale); // Don't scale probe shadow maps
    // <plus> plus/shadowdist: the sun's maps follow the shadow distance and the snapshot boost (was: scale)
    const F32 sun_scale = plus::shadowdist::sunResolutionScale(scale, resX, resY);
    U32 sun_shadow_map_width = BlurHappySize(resX, sun_scale);
    U32 sun_shadow_map_height = BlurHappySize(resY, sun_scale);
    // </plus>

    if (shadow_detail > 0)
    { //allocate 4 sun shadow maps
        for (U32 i = 0; i < 4; i++)
        {
            if (!mRT->shadow[i].allocate(sun_shadow_map_width, sun_shadow_map_height, 0, true))
            {
                return false;
            }
        }
    }
    else
    {
        for (U32 i = 0; i < 4; i++)
        {
            releaseSunShadowTarget(i);
        }
    }

    if (!gCubeSnapshot) // hack to not allocate spot shadow maps during ReflectionMapManager init
    {
        U32 width = (U32)(resX * scale);
        U32 height = width;

        if (shadow_detail > 1)
        { //allocate two spot shadow maps
            U32 spot_shadow_map_width = width;
            U32 spot_shadow_map_height = height;
            for (U32 i = 0; i < 2; i++)
            {
                if (!mSpotShadow[i].allocate(spot_shadow_map_width, spot_shadow_map_height, 0, true))
                {
                    return false;
                }
            }
        }
        else
        {
            releaseSpotShadowTargets();
        }
    }


    // set up shadow map filtering and compare modes
    if (shadow_detail > 0)
    {
        if (mRT == &mMainRT) // LLRHI: what TFO_ANISOTROPIC gives them (research/64)
        {
            mRHISunShadowAniso = gGLManager.mHasAnisotropic && LLRender::sAnisotropicFilteringLevel > 1.f
                                 ? LLRender::sAnisotropicFilteringLevel : 1.f;
        }
        for (U32 i = 0; i < 4; i++)
        {
            LLRenderTarget* shadow_target = getSunShadowTarget(i);
            if (shadow_target)
            {
                gGL.getTexUnit(0)->bind(getSunShadowTarget(i), true);
                gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
                gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);

                LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);     // <plus> research/207 6.3 </plus>
                LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);     // <plus> research/207 6.3 </plus>
            }
        }
    }

    if (shadow_detail > 1 && !gCubeSnapshot)
    {
        for (U32 i = 0; i < 2; i++)
        {
            LLRenderTarget* shadow_target = getSpotShadowTarget(i);
            if (shadow_target)
            {
                gGL.getTexUnit(0)->bind(shadow_target, true);
                gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
                gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);

                LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);     // <plus> research/207 6.3 </plus>
                LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);     // <plus> research/207 6.3 </plus>
            }
        }
    }

    return true;
}

//static
void LLPipeline::updateRenderTransparentWater()
{
    sRenderTransparentWater = gSavedSettings.getBOOL("RenderTransparentWater");
}

// static
void LLPipeline::refreshCachedSettings()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;
    LLPipeline::sAutoMaskAlphaDeferred = gSavedSettings.getBOOL("RenderAutoMaskAlphaDeferred");
    LLPipeline::sAutoMaskAlphaNonDeferred = gSavedSettings.getBOOL("RenderAutoMaskAlphaNonDeferred");
    LLPipeline::sUseFarClip = gSavedSettings.getBOOL("RenderUseFarClip");
    LLVOAvatar::sMaxNonImpostors = gSavedSettings.getU32("RenderAvatarMaxNonImpostors");
    LLVOAvatar::updateImpostorRendering(LLVOAvatar::sMaxNonImpostors);

    LLPipeline::sUseOcclusion =
            (!gUseWireframe
            && LLFeatureManager::getInstance()->isFeatureAvailable("UseOcclusion")
            && gSavedSettings.getBOOL("UseOcclusion")) ? 2 : 0;

    WindLightUseAtmosShaders = true; // DEPRECATED -- gSavedSettings.getBOOL("WindLightUseAtmosShaders");
    RenderDeferred = true; // DEPRECATED -- gSavedSettings.getBOOL("RenderDeferred");
    RenderDeferredSunWash = gSavedSettings.getF32("RenderDeferredSunWash");
    RenderFSAAType = gSavedSettings.getU32("RenderFSAAType");
    RenderResolutionDivisor = gSavedSettings.getU32("RenderResolutionDivisor");
    RenderUIBuffer = gSavedSettings.getBOOL("RenderUIBuffer");
    RenderShadowDetail = gSavedSettings.getS32("RenderShadowDetail");
    RenderShadowSplits = gSavedSettings.getS32("RenderShadowSplits");
    RenderDeferredSSAO = gSavedSettings.getBOOL("RenderDeferredSSAO");
    RenderShadowResolutionScale = gSavedSettings.getF32("RenderShadowResolutionScale");
    RenderDelayCreation = gSavedSettings.getBOOL("RenderDelayCreation");
    RenderAnimateRes = gSavedSettings.getBOOL("RenderAnimateRes");
    FreezeTime = gSavedSettings.getBOOL("FreezeTime");
    DebugBeaconLineWidth = gSavedSettings.getS32("DebugBeaconLineWidth");
    RenderHighlightBrightness = gSavedSettings.getF32("RenderHighlightBrightness");
    RenderHighlightColor = gSavedSettings.getColor4("RenderHighlightColor");
    RenderHighlightThickness = gSavedSettings.getF32("RenderHighlightThickness");
    RenderSpotLightsInNondeferred = gSavedSettings.getBOOL("RenderSpotLightsInNondeferred");
    PreviewAmbientColor = gSavedSettings.getColor4("PreviewAmbientColor");
    PreviewDiffuse0 = gSavedSettings.getColor4("PreviewDiffuse0");
    PreviewSpecular0 = gSavedSettings.getColor4("PreviewSpecular0");
    PreviewDiffuse1 = gSavedSettings.getColor4("PreviewDiffuse1");
    PreviewSpecular1 = gSavedSettings.getColor4("PreviewSpecular1");
    PreviewDiffuse2 = gSavedSettings.getColor4("PreviewDiffuse2");
    PreviewSpecular2 = gSavedSettings.getColor4("PreviewSpecular2");
    PreviewDirection0 = gSavedSettings.getVector3("PreviewDirection0");
    PreviewDirection1 = gSavedSettings.getVector3("PreviewDirection1");
    PreviewDirection2 = gSavedSettings.getVector3("PreviewDirection2");
    RenderGlowMaxExtractAlpha = gSavedSettings.getF32("RenderGlowMaxExtractAlpha");
    RenderGlowWarmthAmount = gSavedSettings.getF32("RenderGlowWarmthAmount");
    RenderGlowLumWeights = gSavedSettings.getVector3("RenderGlowLumWeights");
    RenderGlowWarmthWeights = gSavedSettings.getVector3("RenderGlowWarmthWeights");
    RenderGlowResolutionPow = gSavedSettings.getS32("RenderGlowResolutionPow");
    RenderGlowIterations = gSavedSettings.getS32("RenderGlowIterations");
    RenderGlowWidth = gSavedSettings.getF32("RenderGlowWidth");
    RenderGlowStrength = gSavedSettings.getF32("RenderGlowStrength");
    RenderGlowNoise = gSavedSettings.getBOOL("RenderGlowNoise");
    RenderDepthOfField = gSavedSettings.getBOOL("RenderDepthOfField");
    RenderDepthOfFieldInEditMode = gSavedSettings.getBOOL("RenderDepthOfFieldInEditMode");
    CameraFocusTransitionTime = gSavedSettings.getF32("CameraFocusTransitionTime");
    CameraFNumber = gSavedSettings.getF32("CameraFNumber");
    CameraFocalLength = gSavedSettings.getF32("CameraFocalLength");
    CameraFieldOfView = gSavedSettings.getF32("CameraFieldOfView");
    RenderShadowNoise = gSavedSettings.getF32("RenderShadowNoise");
    RenderShadowBlurSize = gSavedSettings.getF32("RenderShadowBlurSize");
    RenderSSAOScale = gSavedSettings.getF32("RenderSSAOScale");
    RenderSSAOMaxScale = gSavedSettings.getU32("RenderSSAOMaxScale");
    RenderSSAOFactor = gSavedSettings.getF32("RenderSSAOFactor");
    RenderSSAOEffect = gSavedSettings.getVector3("RenderSSAOEffect");
    RenderShadowOffsetError = gSavedSettings.getF32("RenderShadowOffsetError");
    RenderShadowBiasError = gSavedSettings.getF32("RenderShadowBiasError");
    RenderShadowOffset = gSavedSettings.getF32("RenderShadowOffset");
    RenderShadowBias = gSavedSettings.getF32("RenderShadowBias");
    RenderSpotShadowOffset = gSavedSettings.getF32("RenderSpotShadowOffset");
    RenderSpotShadowBias = gSavedSettings.getF32("RenderSpotShadowBias");
    RenderEdgeDepthCutoff = gSavedSettings.getF32("RenderEdgeDepthCutoff");
    RenderEdgeNormCutoff = gSavedSettings.getF32("RenderEdgeNormCutoff");
    RenderShadowGaussian = gSavedSettings.getVector3("RenderShadowGaussian");
    RenderShadowBlurDistFactor = gSavedSettings.getF32("RenderShadowBlurDistFactor");
    RenderDeferredAtmospheric = gSavedSettings.getBOOL("RenderDeferredAtmospheric");
    RenderHighlightFadeTime = gSavedSettings.getF32("RenderHighlightFadeTime");
    RenderFarClip = gSavedSettings.getF32("RenderFarClip");
    RenderShadowSplitExponent = gSavedSettings.getVector3("RenderShadowSplitExponent");
    RenderShadowErrorCutoff = gSavedSettings.getF32("RenderShadowErrorCutoff");
    RenderShadowFOVCutoff = gSavedSettings.getF32("RenderShadowFOVCutoff");
    CameraOffset = gSavedSettings.getBOOL("CameraOffset");
    CameraMaxCoF = gSavedSettings.getF32("CameraMaxCoF");
    CameraDoFResScale = gSavedSettings.getF32("CameraDoFResScale");
    RenderAutoHideSurfaceAreaLimit = gSavedSettings.getF32("RenderAutoHideSurfaceAreaLimit");
    RenderScreenSpaceReflections = gSavedSettings.getBOOL("RenderScreenSpaceReflections");
    RenderScreenSpaceReflectionIterations = gSavedSettings.getS32("RenderScreenSpaceReflectionIterations");
    RenderScreenSpaceReflectionRayStep = gSavedSettings.getF32("RenderScreenSpaceReflectionRayStep");
    RenderScreenSpaceReflectionDistanceBias = gSavedSettings.getF32("RenderScreenSpaceReflectionDistanceBias");
    RenderScreenSpaceReflectionDepthRejectBias = gSavedSettings.getF32("RenderScreenSpaceReflectionDepthRejectBias");
    RenderScreenSpaceReflectionAdaptiveStepMultiplier = gSavedSettings.getF32("RenderScreenSpaceReflectionAdaptiveStepMultiplier");
    RenderScreenSpaceReflectionGlossySamples = gSavedSettings.getS32("RenderScreenSpaceReflectionGlossySamples");
    RenderBufferVisualization = gSavedSettings.getS32("RenderBufferVisualization");
    RenderMirrors = gSavedSettings.getBOOL("RenderMirrors");
    RenderHeroProbeUpdateRate = gSavedSettings.getS32("RenderHeroProbeUpdateRate");
    RenderHeroProbeConservativeUpdateMultiplier = gSavedSettings.getS32("RenderHeroProbeConservativeUpdateMultiplier");
    RenderAvatarCloth = gSavedSettings.getBOOL("RenderAvatarCloth");

    sReflectionProbesEnabled = LLFeatureManager::getInstance()->isFeatureAvailable("RenderReflectionsEnabled") && gSavedSettings.getBOOL("RenderReflectionsEnabled");
    RenderSpotLight = nullptr;

    if (gNonInteractive)
    {
        LLVOAvatar::sMaxNonImpostors = 1;
        LLVOAvatar::updateImpostorRendering(LLVOAvatar::sMaxNonImpostors);
    }

    bool enable_buffers = gSavedSettings.getBOOL("CollectFontVertexBuffers");
    LLFontVertexBuffer::enableBufferCollection(enable_buffers);
    LLFontWidthBuffer::enableBufferCollection(enable_buffers);
    enable_buffers = gSavedSettings.getBOOL("CollectUIImageVertexBuffers");
    LLUIImage::enableDisplayListsCollection(enable_buffers);
}

void LLPipeline::releaseGLBuffers()
{
    assertInitialized();

    if (mNoiseMap)
    {
        LLImageGL::deleteTextures(1, &mNoiseMap);
        mNoiseMap = 0;
    }

    if (mTrueNoiseMap)
    {
        LLImageGL::deleteTextures(1, &mTrueNoiseMap);
        mTrueNoiseMap = 0;
    }

    if (mSMAAAreaMap)
    {
        LLImageGL::deleteTextures(1, &mSMAAAreaMap);
        mSMAAAreaMap = 0;
    }

    if (mSMAASearchMap)
    {
        LLImageGL::deleteTextures(1, &mSMAASearchMap);
        mSMAASearchMap = 0;
    }

    releaseLUTBuffers();

    mWaterDis.release();

    mSceneMap.release();

    mWaterExclusionMask.release();

    mPostPingMap.release();
    mPostPongMap.release();

    mFXAAMap.release();

    mUIScreen.release();

    mDownResMap.release();

    mBakeMap.release();

    for (U32 i = 0; i < 3; i++)
    {
        mGlow[i].release();
    }

    mHeroProbeManager.cleanup(); // release hero probes

    releaseScreenBuffers();
    releaseShadowCaches(); // LLRHI (fix-round22)

    gBumpImageList.destroyGL();
    LLVOAvatar::resetImpostors();
}

void LLPipeline::releaseLUTBuffers()
{
    if (mLightFunc)
    {
        LLImageGL::deleteTextures(1, &mLightFunc);
        mLightFunc = 0;
    }

    mPbrBrdfLut.release();

    mExposureMap.release();
    mLuminanceMap.release();
    mLastExposure.release();
    mLUTContentsPending = false;    // <plus> research/261: nothing to fill until createLUTBuffers </plus>

}

void LLPipeline::releaseShadowBuffers()
{
    releaseSunShadowTargets();
    releaseSpotShadowTargets();
    releaseShadowCaches(); // LLRHI (fix-round22)
}

// LLRHI (fix-round22): the static shadow caches (sun cascades and the probes' shared
// maps) go with the GL buffers, and their entries no longer count as drawn, so after
// destroyGL/restoreGL nothing uses the old depth images.
void LLPipeline::releaseShadowCaches()
{
    for (U32 i = 0; i < 4; ++i)
    {
        mShadowStaticCache[i].release();
        mShadowCacheEntry[i].valid = false;
        mShadowCacheEntry[i].reused = false;
    }
    for (U32 i = 0; i < 2; ++i)
    {
        mProbeShadowStaticCache[i].release();
        mProbeShadowCacheEntry[i].valid = false;
        mProbeShadowCacheEntry[i].reused = false;
    }
    plus::shadowshift::release(); // <plus> plus/shadowshift: what it kept of the cached depths, and its check's target </plus>
}

void LLPipeline::releaseScreenBuffers()
{
    plus::upscale::releaseTargets(); // <plus> plus/upscale: its window-sized targets go with the screen's </plus>
    plus::taa::releaseTargets(); // <plus> plus/taa (research/185): its history goes with the screen's </plus>
    plus::volclouds::releaseTargets(); // <plus> plus/volclouds (research/186): the clouds' shadow map </plus>
    releaseProbeStatic(); // LLRHI: the probe faces' static G-buffers follow the probe G-buffer
    mRT->screen.release();
    mRT->deferredScreen.release();
    mRT->deferredLight.release();

    mAuxillaryRT.screen.release();
    mAuxillaryRT.deferredScreen.release();
    mAuxillaryRT.deferredLight.release();

    mHeroProbeRT.screen.release();
    mHeroProbeRT.deferredScreen.release();
    mHeroProbeRT.deferredLight.release();
}

void LLPipeline::releaseSunShadowTarget(U32 index)
{
    llassert(index < 4);
    mRT->shadow[index].release();
}

void LLPipeline::releaseSunShadowTargets()
{
    for (U32 i = 0; i < 4; i++)
    {
        releaseSunShadowTarget(i);
    }
}

void LLPipeline::releaseSpotShadowTargets()
{
    if (!gCubeSnapshot) // hack to avoid freeing spot shadows during ReflectionMapManager init
    {
        for (U32 i = 0; i < 2; i++)
        {
            mSpotShadow[i].release();
        }
    }
}

void LLPipeline::createGLBuffers()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    stop_glerror();
    assertInitialized();

    stop_glerror();

    GLuint resX = gViewerWindow->getWorldViewWidthRaw();
    GLuint resY = gViewerWindow->getWorldViewHeightRaw();

    // allocate screen space glow buffers
    const U32 glow_res = llmax(1, llmin(512, 1 << gSavedSettings.getS32("RenderGlowResolutionPow")));
    const bool glow_hdr = gSavedSettings.getBOOL("RenderGlowHDR");
    const U32 glow_color_fmt = glow_hdr ? GL_RGBA16F : GL_RGBA;
    for (U32 i = 0; i < 3; i++)
    {
        mGlow[i].allocate(512, glow_res, glow_color_fmt);
    }

    allocateScreenBuffer(resX, resY);
    // Do not zero out mRT dimensions here. allocateScreenBuffer() above
    // already sets the correct dimensions. Zeroing them caused resizeShadowTexture()
    // to fail if called immediately after createGLBuffers (e.g., post graphics change).
    // mRT->width = 0;
    // mRT->height = 0;


    if (!mNoiseMap)
    {
        const U32 noiseRes = 128;
        LLVector3 noise[noiseRes*noiseRes];

        F32 scaler = gSavedSettings.getF32("RenderDeferredNoise")/100.f;
        for (U32 i = 0; i < noiseRes*noiseRes; ++i)
        {
            noise[i] = LLVector3(ll_frand()-0.5f, ll_frand()-0.5f, 0.f);
            noise[i].normVec();
            noise[i].mV[2] = ll_frand()*scaler+1.f-scaler/2.f;
        }

        LLImageGL::generateTextures(1, &mNoiseMap);

        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mNoiseMap);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGB16F, noiseRes, noiseRes, GL_RGB, GL_FLOAT, noise, false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    }

    if (!mTrueNoiseMap)
    {
        const U32 noiseRes = 128;
        F32 noise[noiseRes*noiseRes*3];
        for (U32 i = 0; i < noiseRes*noiseRes*3; i++)
        {
            noise[i] = ll_frand()*2.0f-1.0f;
        }

        LLImageGL::generateTextures(1, &mTrueNoiseMap);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTrueNoiseMap);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGB16F, noiseRes, noiseRes, GL_RGB,GL_FLOAT, noise, false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    }

    if (!mSMAAAreaMap)
    {
        std::vector<U8> tempBuffer(AREATEX_SIZE);
        for (U32 y = 0; y < AREATEX_HEIGHT; y++)
        {
            U32 srcY = AREATEX_HEIGHT - 1 - y;
            // unsigned int srcY = y;
            memcpy(&tempBuffer[y * AREATEX_PITCH], areaTexBytes + srcY * AREATEX_PITCH, AREATEX_PITCH);
        }

        LLImageGL::generateTextures(1, &mSMAAAreaMap);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mSMAAAreaMap);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RG8, AREATEX_WIDTH, AREATEX_HEIGHT, GL_RG,
            GL_UNSIGNED_BYTE, tempBuffer.data(), false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    if (!mSMAASearchMap)
    {
        std::vector<U8> tempBuffer(SEARCHTEX_SIZE);
        for (U32 y = 0; y < SEARCHTEX_HEIGHT; y++)
        {
            U32 srcY = SEARCHTEX_HEIGHT - 1 - y;
            // unsigned int srcY = y;
            memcpy(&tempBuffer[y * SEARCHTEX_PITCH], searchTexBytes + srcY * SEARCHTEX_PITCH, SEARCHTEX_PITCH);
        }

        LLImageGL::generateTextures(1, &mSMAASearchMap);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mSMAASearchMap);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_R8, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
            GL_RED, GL_UNSIGNED_BYTE, tempBuffer.data(), false);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    if (!mSMAASampleMap)
    {
        LLPointer<LLImageRaw>               raw_image = new LLImageRaw;
        LLPointer<LLImagePNG>               png_image = new LLImagePNG;
        static LLCachedControl<std::string> sample_path(gSavedSettings, "SamplePath", "");
        if (gDirUtilp->fileExists(sample_path()) && png_image->load(sample_path()) && png_image->decode(raw_image, 0.0f))
        {
            U32 format = 0;
            switch (raw_image->getComponents())
            {
            case 1:
                format = GL_RED;
                break;
            case 2:
                format = GL_RG;
                break;
            case 3:
                format = GL_RGB;
                break;
            case 4:
                format = GL_RGBA;
                break;
            default:
                return;
            };
            LLImageGL::generateTextures(1, &mSMAASampleMap);
            gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mSMAASampleMap);
            LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, GL_RGB, raw_image->getWidth(),
                raw_image->getHeight(), format, GL_UNSIGNED_BYTE, raw_image->getData(), false);
            stop_glerror();
            gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
            gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }
    }

    createLUTBuffers();

    gBumpImageList.restoreGL();
}

F32 lerpf(F32 a, F32 b, F32 w)
{
    return a + w * (b - a);
}

void LLPipeline::createLUTBuffers()
{
    if (!mLightFunc)
    {
        U32 lightResX = gSavedSettings.getU32("RenderSpecularResX");
        U32 lightResY = gSavedSettings.getU32("RenderSpecularResY");
        F32* ls = nullptr;
        try
        {
            ls = new F32[lightResX*lightResY];
        }
        catch (std::bad_alloc&)
        {
            LLError::LLUserWarningMsg::showOutOfMemory();
            // might be better to set the error into mFatalMessage and rethrow
            LL_ERRS() << "Bad memory allocation in createLUTBuffers! lightResX: "
                << lightResX << " lightResY: " << lightResY << LL_ENDL;
        }
        F32 specExp = gSavedSettings.getF32("RenderSpecularExponent");
        // Calculate the (normalized) blinn-phong specular lookup texture. (with a few tweaks)
        for (U32 y = 0; y < lightResY; ++y)
        {
            for (U32 x = 0; x < lightResX; ++x)
            {
                ls[y*lightResX+x] = 0;
                F32 sa = (F32) x/(lightResX-1);
                F32 spec = (F32) y/(lightResY-1);
                F32 n = spec * spec * specExp;

                // Nothing special here.  Just your typical blinn-phong term.
                spec = powf(sa, n);

                // Apply our normalization function.
                // Note: This is the full equation that applies the full normalization curve, not an approximation.
                // This is fine, given we only need to create our LUT once per buffer initialization.
                spec *= (((n + 2) * (n + 4)) / (8 * F_PI * (powf(2, -n/2) + n)));

                // Since we use R16F, we no longer have a dynamic range issue we need to work around here.
                // Though some older drivers may not like this, newer drivers shouldn't have this problem.
                ls[y*lightResX+x] = spec;
            }
        }

        U32 pix_format = GL_R16F;
#if LL_DARWIN
        if(!gGLManager.mIsApple)
        {
            // Need to work around limited precision with 10.6.8 and older drivers
            //
            pix_format = GL_R32F;
        }
#endif
        LLImageGL::generateTextures(1, &mLightFunc);
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mLightFunc);
        LLImageGL::setManualImage(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE), 0, pix_format, lightResX, lightResY, GL_RED, GL_FLOAT, ls, false);
        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_TRILINEAR);
        LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);     // <plus> research/207 6.3 </plus>
        LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);     // <plus> research/207 6.3 </plus>

        delete [] ls;
    }

    // <plus> research/261: the targets first (as before, the BRDF LUT, the exposure, luminance and last exposure
    // maps), then their contents now if a frame takes them, otherwise in the first one (updateGL). Under the splash
    // screen (gPipeline.init) Vulkan dropped the draw and the clear: the exposure map stayed unwritten where nothing
    // measures it (HDR off: Low and LowMid), and the sky's undoing of the exposure (plus/skyscreen) divided by it.
    mPbrBrdfLut.allocate(512, 512, GL_RG16F);
    mExposureMap.allocate(1, 1, GL_R16F);
    mLuminanceMap.allocate(256, 256, GL_R16F, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_AUTO);
    mLastExposure.allocate(1, 1, GL_R16F);

    mLUTContentsPending = true;
    makePendingLUTContents();
}

// research/261: was the second half of createLUTBuffers (the targets were allocated in between)
void LLPipeline::makePendingLUTContents()
{
    if (!mLUTContentsPending || !LLRHI::frameRecording())
    {
        return;     // made already, or no frame to take the draw and the clear yet
    }
    mLUTContentsPending = false;

    mPbrBrdfLut.bindTarget();

    if (gDeferredGenBrdfLutProgram.isComplete())
    {
        gDeferredGenBrdfLutProgram.bind();
        llassert_always(LLGLSLShader::sCurBoundShaderPtr != nullptr);

        gGL.begin(LLRender::TRIANGLE_STRIP);
        gGL.vertex2f(-1, -1);
        gGL.vertex2f(-1, 1);
        gGL.vertex2f(1, -1);
        gGL.vertex2f(1, 1);
        gGL.end();
        gGL.flush();
    }
    else
    {
        LL_WARNS("Brad") << gDeferredGenBrdfLutProgram.mName << " failed to load, cannot be used!" << LL_ENDL;
    }

    gDeferredGenBrdfLutProgram.unbind();
    mPbrBrdfLut.flush();

    // 1: no exposure where none is measured (HDR off); research/207 6.3: LLRHI::clearColor, was glClearColor
    mExposureMap.bindTarget();
    LLRHI::clearColor(1, 1, 1, 0);
    mExposureMap.clear();
    LLRHI::clearColor(0, 0, 0, 0);
    mExposureMap.flush();
}
// </plus>


void LLPipeline::restoreGL()
{
    assertInitialized();

    LLViewerShaderMgr::instance()->setShaders();

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;
        for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            if (part)
            {
                part->restoreGL();
        }
        }
    }
}

bool LLPipeline::shadersLoaded()
{
    return (assertInitialized() && mShadersLoaded);
}

bool LLPipeline::canUseWindLightShaders() const
{
    return true;
}

bool LLPipeline::canUseAntiAliasing() const
{
    return true;
}

void LLPipeline::unloadShaders()
{
    LLViewerShaderMgr::instance()->unloadShaders();
    mShadersLoaded = false;
}

void LLPipeline::assertInitializedDoError()
{
    LL_ERRS() << "LLPipeline used when uninitialized." << LL_ENDL;
}

//============================================================================

void LLPipeline::enableShadows(const bool enable_shadows)
{
    //should probably do something here to wrangle shadows....
}

class LLOctreeDirtyTexture : public OctreeTraveler
{
public:
    const std::set<LLViewerFetchedTexture*>& mTextures;

    LLOctreeDirtyTexture(const std::set<LLViewerFetchedTexture*>& textures) : mTextures(textures) { }

    virtual void visit(const OctreeNode* node)
    {
        LLSpatialGroup* group = (LLSpatialGroup*) node->getListener(0);

        if (!group->hasState(LLSpatialGroup::GEOM_DIRTY) && !group->isEmpty())
        {
            for (LLSpatialGroup::draw_map_t::iterator i = group->mDrawMap.begin(); i != group->mDrawMap.end(); ++i)
            {
                for (LLSpatialGroup::drawmap_elem_t::iterator j = i->second.begin(); j != i->second.end(); ++j)
                {
                    LLDrawInfo* params = *j;
                    LLViewerFetchedTexture* tex = LLViewerTextureManager::staticCastToFetchedTexture(params->mTexture);
                    if (tex && mTextures.find(tex) != mTextures.end())
                    {
                        group->setState(LLSpatialGroup::GEOM_DIRTY);
                    }
                }
            }
        }

        for (LLSpatialGroup::bridge_list_t::iterator i = group->mBridgeList.begin(); i != group->mBridgeList.end(); ++i)
        {
            LLSpatialBridge* bridge = *i;
            traverse(bridge->mOctree);
        }
    }
};

// Called when a texture changes # of channels (causes faces to move to alpha pool)
void LLPipeline::dirtyPoolObjectTextures(const std::set<LLViewerFetchedTexture*>& textures)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    assertInitialized();

    // *TODO: This is inefficient and causes frame spikes; need a better way to do this
    //        Most of the time is spent in dirty.traverse.

    for (pool_set_t::iterator iter = mPools.begin(); iter != mPools.end(); ++iter)
    {
        LLDrawPool *poolp = *iter;
        if (poolp->isFacePool())
        {
            ((LLFacePool*) poolp)->dirtyTextures(textures);
        }
    }

    LLOctreeDirtyTexture dirty(textures);
    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;
        for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            if (part)
            {
                dirty.traverse(part->mOctree);
            }
        }
    }
}

LLDrawPool *LLPipeline::findPool(const U32 type, LLViewerTexture *tex0)
{
    assertInitialized();

    LLDrawPool *poolp = NULL;
    switch( type )
    {
    case LLDrawPool::POOL_SIMPLE:
        poolp = mSimplePool;
        break;

    case LLDrawPool::POOL_GRASS:
        poolp = mGrassPool;
        break;

    case LLDrawPool::POOL_ALPHA_MASK:
        poolp = mAlphaMaskPool;
        break;

    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        poolp = mFullbrightAlphaMaskPool;
        break;

    case LLDrawPool::POOL_FULLBRIGHT:
        poolp = mFullbrightPool;
        break;

    case LLDrawPool::POOL_GLOW:
        poolp = mGlowPool;
        break;

    case LLDrawPool::POOL_TREE:
        poolp = get_if_there(mTreePools, (uintptr_t)tex0, (LLDrawPool*)0 );
        break;

    case LLDrawPool::POOL_TERRAIN:
        poolp = get_if_there(mTerrainPools, (uintptr_t)tex0, (LLDrawPool*)0 );
        break;

    case LLDrawPool::POOL_BUMP:
        poolp = mBumpPool;
        break;
    case LLDrawPool::POOL_MATERIALS:
        poolp = mMaterialsPool;
        break;
    case LLDrawPool::POOL_ALPHA_PRE_WATER:
        poolp = mAlphaPoolPreWater;
        break;
    case LLDrawPool::POOL_ALPHA_POST_WATER:
        poolp = mAlphaPoolPostWater;
        break;

    case LLDrawPool::POOL_AVATAR:
    case LLDrawPool::POOL_CONTROL_AV:
        break; // Do nothing

    case LLDrawPool::POOL_SKY:
        poolp = mSkyPool;
        break;

    case LLDrawPool::POOL_WATER:
        poolp = mWaterPool;
        break;

    case LLDrawPool::POOL_WL_SKY:
        poolp = mWLSkyPool;
        break;

    case LLDrawPool::POOL_GLTF_PBR:
        poolp = mPBROpaquePool;
        break;
    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        poolp = mPBRAlphaMaskPool;
        break;

    case LLDrawPool::POOL_WATEREXCLUSION:
        poolp = mWaterExclusionPool;
        break;

    default:
        llassert(0);
        LL_ERRS() << "Invalid Pool Type in  LLPipeline::findPool() type=" << type << LL_ENDL;
        break;
    }

    return poolp;
}


LLDrawPool *LLPipeline::getPool(const U32 type, LLViewerTexture *tex0)
{
    LLDrawPool *poolp = findPool(type, tex0);
    if (poolp)
    {
        return poolp;
    }

    LLDrawPool *new_poolp = LLDrawPool::createPool(type, tex0);
    addPool( new_poolp );

    return new_poolp;
}


// static
LLDrawPool* LLPipeline::getPoolFromTE(const LLTextureEntry* te, LLViewerTexture* imagep)
{
    U32 type = getPoolTypeFromTE(te, imagep);
    return gPipeline.getPool(type, imagep);
}

//static
U32 LLPipeline::getPoolTypeFromTE(const LLTextureEntry* te, LLViewerTexture* imagep)
{
    if (!te || !imagep)
    {
        return 0;
    }

    LLMaterial* mat = te->getMaterialParams().get();
    LLGLTFMaterial* gltf_mat = te->getGLTFRenderMaterial();

    bool color_alpha = te->getColor().mV[3] < 0.999f;
    bool alpha = color_alpha;
    if (imagep)
    {
        alpha = alpha || (imagep->getComponents() == 4 && imagep->getType() != LLViewerTexture::MEDIA_TEXTURE) || (imagep->getComponents() == 2);
    }

    if (alpha && mat)
    {
        switch (mat->getDiffuseAlphaMode())
        {
            case 1:
                alpha = true; // Material's alpha mode is set to blend.  Toss it into the alpha draw pool.
                break;
            case 0: //alpha mode set to none, never go to alpha pool
            case 3: //alpha mode set to emissive, never go to alpha pool
                alpha = color_alpha;
                break;
            default: //alpha mode set to "mask", go to alpha pool if fullbright
                alpha = color_alpha; // Material's alpha mode is set to none, mask, or emissive.  Toss it into the opaque material draw pool.
                break;
        }
    }

    if (alpha || (gltf_mat && gltf_mat->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND))
    {
        return LLDrawPool::POOL_ALPHA;
    }
    else if ((te->getBumpmap() || te->getShiny()) && (!mat || mat->getNormalID().isNull()))
    {
        return LLDrawPool::POOL_BUMP;
    }
    else if (gltf_mat)
    {
        return LLDrawPool::POOL_GLTF_PBR;
    }
    else if (mat && !alpha)
    {
        return LLDrawPool::POOL_MATERIALS;
    }
    else
    {
        return LLDrawPool::POOL_SIMPLE;
    }
}


void LLPipeline::addPool(LLDrawPool *new_poolp)
{
    assertInitialized();
    mPools.insert(new_poolp);
    addToQuickLookup( new_poolp );
}

void LLPipeline::allocDrawable(LLViewerObject *vobj)
{
    LLDrawable *drawable = new LLDrawable(vobj);
    vobj->mDrawable = drawable;

    //encompass completely sheared objects by taking
    //the most extreme point possible (<1,1,0.5>)
    drawable->setRadius(LLVector3(1,1,0.5f).scaleVec(vobj->getScale()).length());
    if (vobj->isOrphaned())
    {
        drawable->setState(LLDrawable::FORCE_INVISIBLE);
    }
    drawable->updateXform(true);
}


void LLPipeline::unlinkDrawable(LLDrawable *drawable)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    assertInitialized();

    LLPointer<LLDrawable> drawablep = drawable; // make sure this doesn't get deleted before we are done

    // Based on flags, remove the drawable from the queues that it's on.
    if (drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        LLDrawable::drawable_vector_t::iterator iter = std::find(mMovedList.begin(), mMovedList.end(), drawablep);
        if (iter != mMovedList.end())
        {
            mMovedList.erase(iter);
        }
    }

    if (drawablep->getSpatialGroup())
    {
        if (!drawablep->getSpatialGroup()->getSpatialPartition()->remove(drawablep, drawablep->getSpatialGroup()))
        {
#ifdef LL_RELEASE_FOR_DOWNLOAD
            LL_WARNS() << "Couldn't remove object from spatial group!" << LL_ENDL;
#else
            LL_ERRS() << "Couldn't remove object from spatial group!" << LL_ENDL;
#endif
        }
    }

    // <plus> research/203: was mLights.erase(drawablep);
    if (mLights.erase(drawablep))
    {
        gLocalLightTable.remove(drawablep);
    }
    // </plus>

    for (light_set_t::iterator iter = mNearbyLights.begin();
                iter != mNearbyLights.end(); iter++)
    {
        if (iter->drawable == drawablep)
        {
            mNearbyLights.erase(iter);
            ++sNearbyLightsVersion;
            break;
        }
    }

    for (U32 i = 0; i < 2; ++i)
    {
        if (mShadowSpotLight[i] == drawablep)
        {
            mShadowSpotLight[i] = NULL;
        }

        if (mTargetShadowSpotLight[i] == drawablep)
        {
            mTargetShadowSpotLight[i] = NULL;
        }
    }
}

//static
void LLPipeline::removeMutedAVsLights(LLVOAvatar* muted_avatar)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    light_set_t::iterator iter = gPipeline.mNearbyLights.begin();
    while (iter != gPipeline.mNearbyLights.end())
    {
        const LLViewerObject* vobj = iter->drawable->getVObj();
        if (vobj
            && vobj->getAvatar()
            && vobj->isAttachment()
            && vobj->getAvatar() == muted_avatar)
        {
            // <plus> research/203: the record leaves with the light; was gPipeline.mLights.erase(iter->drawable);
            if (gPipeline.mLights.erase(iter->drawable))
            {
                gLocalLightTable.remove(iter->drawable);
            }
            // </plus>
            iter = gPipeline.mNearbyLights.erase(iter);
            ++sNearbyLightsVersion;
        }
        else
        {
            iter++;
        }
    }
}

U32 LLPipeline::addObject(LLViewerObject *vobj)
{
    if (RenderDelayCreation)
    {
        mCreateQ.push_back(vobj);
    }
    else
    {
        createObject(vobj);
    }

    return 1;
}

void LLPipeline::createObjects(F32 max_dtime)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    LLTimer update_timer;

    while (!mCreateQ.empty() && update_timer.getElapsedTimeF32() < max_dtime)
    {
        LLViewerObject* vobj = mCreateQ.front();
        if (!vobj->isDead())
        {
            createObject(vobj);
        }
        mCreateQ.pop_front();
    }

    //for (LLViewerObject::vobj_list_t::iterator iter = mCreateQ.begin(); iter != mCreateQ.end(); ++iter)
    //{
    //  createObject(*iter);
    //}

    //mCreateQ.clear();
}

void LLPipeline::createObject(LLViewerObject* vobj)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LLDrawable* drawablep = vobj->mDrawable;

    if (!drawablep)
    {
        drawablep = vobj->createDrawable(this);
    }
    else
    {
        LL_ERRS() << "Redundant drawable creation!" << LL_ENDL;
    }

    llassert(drawablep);

    if (vobj->getParent())
    {
        vobj->setDrawableParent(((LLViewerObject*)vobj->getParent())->mDrawable); // LLPipeline::addObject 1
    }
    else
    {
        vobj->setDrawableParent(NULL); // LLPipeline::addObject 2
    }

    markRebuild(drawablep, LLDrawable::REBUILD_ALL);

    if (drawablep->getVOVolume() && RenderAnimateRes)
    {
        // fun animated res
        drawablep->updateXform(true);
        drawablep->clearState(LLDrawable::MOVE_UNDAMPED);
        drawablep->setScale(LLVector3(0,0,0));
        drawablep->makeActive();
    }
}


void LLPipeline::resetFrameStats()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    assertInitialized();

    sCompiles        = 0;
    mNumVisibleFaces = 0;

    if (mOldRenderDebugMask != mRenderDebugMask)
    {
        gObjectList.clearDebugText();
        mOldRenderDebugMask = mRenderDebugMask;
    }
}

//external functions for asynchronous updating
void LLPipeline::updateMoveDampedAsync(LLDrawable* drawablep)
{
    LL_PROFILE_ZONE_SCOPED;
    if (FreezeTime)
    {
        return;
    }
    if (!drawablep)
    {
        LL_ERRS() << "updateMove called with NULL drawablep" << LL_ENDL;
        return;
    }
    if (drawablep->isState(LLDrawable::EARLY_MOVE))
    {
        return;
    }

    assertInitialized();

    // update drawable now
    drawablep->clearState(LLDrawable::MOVE_UNDAMPED); // force to DAMPED
    plus::idlecensus::Scope plus_census(plus::idlecensus::MOVE_ASYNC); // <plus> research/193 A (off: a flag test) </plus>
    drawablep->updateMove(); // returns done
    plus_census.stop(); // <plus> research/193 A </plus>
    drawablep->setState(LLDrawable::EARLY_MOVE); // flag says we already did an undamped move this frame
    // Put on move list so that EARLY_MOVE gets cleared
    if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        mMovedList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_MOVE_LIST);
    }
}

void LLPipeline::updateMoveNormalAsync(LLDrawable* drawablep)
{
    LL_PROFILE_ZONE_SCOPED;
    if (FreezeTime)
    {
        return;
    }
    if (!drawablep)
    {
        LL_ERRS() << "updateMove called with NULL drawablep" << LL_ENDL;
        return;
    }
    if (drawablep->isState(LLDrawable::EARLY_MOVE))
    {
        return;
    }

    assertInitialized();

    // update drawable now
    drawablep->setState(LLDrawable::MOVE_UNDAMPED); // force to UNDAMPED
    plus::idlecensus::Scope plus_census(plus::idlecensus::MOVE_ASYNC); // <plus> research/193 A (off: a flag test) </plus>
    drawablep->updateMove();
    plus_census.stop(); // <plus> research/193 A </plus>
    drawablep->setState(LLDrawable::EARLY_MOVE); // flag says we already did an undamped move this frame
    // Put on move list so that EARLY_MOVE gets cleared
    if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        mMovedList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_MOVE_LIST);
    }
}

void LLPipeline::updateMovedList(LLDrawable::drawable_vector_t& moved_list)
{
    LL_PROFILE_ZONE_SCOPED;
    for (LLDrawable::drawable_vector_t::iterator iter = moved_list.begin();
         iter != moved_list.end(); )
    {
        LLDrawable::drawable_vector_t::iterator curiter = iter++;
        LLDrawable *drawablep = *curiter;
        if (!drawablep)
        {
            iter = moved_list.erase(curiter);
            continue;
        }
        bool done = true;
        if (!drawablep->isDead() && (!drawablep->isState(LLDrawable::EARLY_MOVE)))
        {
            plus::activecensus::CostScope spin_cost(drawablep, plus::activecensus::COST_MOVE); // <plus> research/186 step 1 (off: one flag test) </plus>
            plus::idlecensus::Scope plus_census(plus::idlecensus::MOVE_LIST); // <plus> research/193 A (off: a flag test) </plus>
            done = drawablep->updateMove();
        }
        drawablep->clearState(LLDrawable::EARLY_MOVE | LLDrawable::MOVE_UNDAMPED);
        if (done)
        {
            if (drawablep->isRoot() && !drawablep->isState(LLDrawable::ACTIVE))
            {
                drawablep->makeStatic();
            }
            drawablep->clearState(LLDrawable::ON_MOVE_LIST);
            if (drawablep->isState(LLDrawable::ANIMATED_CHILD))
            { //will likely not receive any future world matrix updates
                // -- this keeps attachments from getting stuck in space and falling off your avatar
                drawablep->clearState(LLDrawable::ANIMATED_CHILD);
                markRebuild(drawablep, LLDrawable::REBUILD_VOLUME);
                if (drawablep->getVObj())
                {
                    drawablep->getVObj()->dirtySpatialGroup();
                }
            }
            iter = moved_list.erase(curiter);
        }
    }
}

void LLPipeline::updateMove()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    if (FreezeTime)
    {
        return;
    }

    assertInitialized();

    for (LLDrawable::drawable_set_t::iterator iter = mRetexturedList.begin();
            iter != mRetexturedList.end(); ++iter)
    {
        LLDrawable* drawablep = *iter;
        if (drawablep && !drawablep->isDead())
        {
            drawablep->updateTexture();
        }
    }
    mRetexturedList.clear();

    updateMovedList(mMovedList);

    plus::activecensus::frame(); // <plus> research/180 10.12: the census line, the R1 verify (off: two flag tests) </plus>

    //balance octrees
    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
        iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;
        for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            if (part)
            {
                // <plus> plus/cullreuse: LLOctreeRoot::balance (false: it folded the root's
                // only, empty child) tells no listener; the root's bounds and the keys of
                // the cull and shadow reuse hear of it through unbound()
                if (!part->mOctree->balance())
                {
                    if (LLViewerOctreeGroup* root = (LLViewerOctreeGroup*)part->mOctree->getListener(0))
                    {
                        root->unbound();
                    }
                }
                // </plus>
            }
        }

        //balance the VO Cache tree
        LLVOCachePartition* vo_part = region->getVOCachePartition();
        if(vo_part)
        {
            vo_part->mOctree->balance();
        }
    }
}

/////////////////////////////////////////////////////////////////////////////
// Culling and occlusion testing
/////////////////////////////////////////////////////////////////////////////

//static
F32 LLPipeline::calcPixelArea(LLVector3 center, LLVector3 size, LLCamera &camera)
{
    llassert(!gCubeSnapshot); // shouldn't be doing ANY of this during cube snap shots
    LLVector3 lookAt = center - camera.getOrigin();
    F32 dist = lookAt.length();

    //ramp down distance for nearby objects
    //shrink dist by dist/16.
    if (dist < 16.f)
    {
        dist /= 16.f;
        dist *= dist;
        dist *= 16.f;
    }

    //get area of circle around node
    F32 app_angle = atanf(size.length()/dist);
    F32 radius = app_angle*LLDrawable::sCurPixelAngle;
    return radius*radius * F_PI;
}

//static
F32 LLPipeline::calcPixelArea(const LLVector4a& center, const LLVector4a& size, LLCamera &camera)
{
    LLVector4a origin;
    origin.load3(camera.getOrigin().mV);

    LLVector4a lookAt;
    lookAt.setSub(center, origin);
    F32 dist = lookAt.getLength3().getF32();

    //ramp down distance for nearby objects
    //shrink dist by dist/16.
    if (dist < 16.f)
    {
        dist /= 16.f;
        dist *= dist;
        dist *= 16.f;
    }

    //get area of circle around node
    F32 app_angle = atanf(size.getLength3().getF32() / dist);
    F32 radius = app_angle * LLDrawable::sCurPixelAngle;
    return radius * radius * F_PI;
}

void LLPipeline::grabReferences(LLCullResult& result)
{
    sCull = &result;
}

void LLPipeline::clearReferences()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    sCull = NULL;
    mGroupSaveQ1.clear();
}

void check_references(LLSpatialGroup* group, LLDrawable* drawable)
{
    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawablep = (LLDrawable*)(*i)->getDrawable();
        if (drawable == drawablep)
        {
            LL_ERRS() << "LLDrawable deleted while actively reference by LLPipeline." << LL_ENDL;
        }
    }
}

void check_references(LLDrawable* drawable, LLFace* face)
{
    for (S32 i = 0; i < drawable->getNumFaces(); ++i)
    {
        if (drawable->getFace(i) == face)
        {
            LL_ERRS() << "LLFace deleted while actively referenced by LLPipeline." << LL_ENDL;
        }
    }
}

void check_references(LLSpatialGroup* group, LLFace* face)
{
    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
        if(drawable)
        {
        check_references(drawable, face);
    }
}
}

void LLPipeline::checkReferences(LLFace* face)
{
#if 0
    if (sCull)
    {
        for (LLCullResult::sg_iterator iter = sCull->beginVisibleGroups(); iter != sCull->endVisibleGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, face);
        }

        for (LLCullResult::sg_iterator iter = sCull->beginAlphaGroups(); iter != sCull->endAlphaGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, face);
        }

        for (LLCullResult::sg_iterator iter = sCull->beginDrawableGroups(); iter != sCull->endDrawableGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, face);
        }

        for (LLCullResult::drawable_iterator iter = sCull->beginVisibleList(); iter != sCull->endVisibleList(); ++iter)
        {
            LLDrawable* drawable = *iter;
            check_references(drawable, face);
        }
    }
#endif
}

void LLPipeline::checkReferences(LLDrawable* drawable)
{
#if 0
    if (sCull)
    {
        for (LLCullResult::sg_iterator iter = sCull->beginVisibleGroups(); iter != sCull->endVisibleGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, drawable);
        }

        for (LLCullResult::sg_iterator iter = sCull->beginAlphaGroups(); iter != sCull->endAlphaGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, drawable);
        }

        for (LLCullResult::sg_iterator iter = sCull->beginDrawableGroups(); iter != sCull->endDrawableGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, drawable);
        }

        for (LLCullResult::drawable_iterator iter = sCull->beginVisibleList(); iter != sCull->endVisibleList(); ++iter)
        {
            if (drawable == *iter)
            {
                LL_ERRS() << "LLDrawable deleted while actively referenced by LLPipeline." << LL_ENDL;
            }
        }
    }
#endif
}

void check_references(LLSpatialGroup* group, LLDrawInfo* draw_info)
{
    for (LLSpatialGroup::draw_map_t::iterator i = group->mDrawMap.begin(); i != group->mDrawMap.end(); ++i)
    {
        LLSpatialGroup::drawmap_elem_t& draw_vec = i->second;
        for (LLSpatialGroup::drawmap_elem_t::iterator j = draw_vec.begin(); j != draw_vec.end(); ++j)
        {
            LLDrawInfo* params = *j;
            if (params == draw_info)
            {
                LL_ERRS() << "LLDrawInfo deleted while actively referenced by LLPipeline." << LL_ENDL;
            }
        }
    }
}


void LLPipeline::checkReferences(LLDrawInfo* draw_info)
{
#if 0
    if (sCull)
    {
        for (LLCullResult::sg_iterator iter = sCull->beginVisibleGroups(); iter != sCull->endVisibleGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, draw_info);
        }

        for (LLCullResult::sg_iterator iter = sCull->beginAlphaGroups(); iter != sCull->endAlphaGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, draw_info);
        }

        for (LLCullResult::sg_iterator iter = sCull->beginDrawableGroups(); iter != sCull->endDrawableGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            check_references(group, draw_info);
        }
    }
#endif
}

void LLPipeline::checkReferences(LLSpatialGroup* group)
{
#if CHECK_PIPELINE_REFERENCES
    if (sCull)
    {
        for (LLCullResult::sg_iterator iter = sCull->beginVisibleGroups(); iter != sCull->endVisibleGroups(); ++iter)
        {
            if (group == *iter)
            {
                LL_ERRS() << "LLSpatialGroup deleted while actively referenced by LLPipeline." << LL_ENDL;
            }
        }

        for (LLCullResult::sg_iterator iter = sCull->beginAlphaGroups(); iter != sCull->endAlphaGroups(); ++iter)
        {
            if (group == *iter)
            {
                LL_ERRS() << "LLSpatialGroup deleted while actively referenced by LLPipeline." << LL_ENDL;
            }
        }

        for (LLCullResult::sg_iterator iter = sCull->beginDrawableGroups(); iter != sCull->endDrawableGroups(); ++iter)
        {
            if (group == *iter)
            {
                LL_ERRS() << "LLSpatialGroup deleted while actively referenced by LLPipeline." << LL_ENDL;
            }
        }
    }
#endif
}


bool LLPipeline::visibleObjectsInFrustum(LLCamera& camera)
{
    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;

        for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            if (part)
            {
                if (hasRenderType(part->mDrawableType))
                {
                    if (part->visibleObjectsInFrustum(camera))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool LLPipeline::getVisibleExtents(LLCamera& camera, LLVector3& min, LLVector3& max)
{
    const F32 X = 65536.f;

    min = LLVector3(X,X,X);
    max = LLVector3(-X,-X,-X);

    LLViewerCamera::eCameraID saved_camera_id = LLViewerCamera::sCurCameraID;
    LLViewerCamera::sCurCameraID = LLViewerCamera::CAMERA_WORLD;

    bool res = true;

    // LLRHI: a face of the realtime reflection probe can take its extents from
    // one walk for all six faces (research/44, P2).
    const bool shared = saved_camera_id == LLViewerCamera::CAMERA_WORLD && probeFaceExtentsShared(camera, min, max, res);

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            !shared && iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;

        for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            if (part)
            {
                if (hasRenderType(part->mDrawableType))
                {
                    if (!part->getVisibleExtents(camera, min, max))
                    {
                        res = false;
                    }
                }
            }
        }
    }

    LLViewerCamera::sCurCameraID = saved_camera_id;
    return res;
}

static LLTrace::BlockTimerStatHandle FTM_CULL("Object Culling");

// static
bool LLPipeline::isWaterClip()
{
    // We always pretend that we're not clipping water when rendering mirrors.
    return (gPipeline.mHeroProbeManager.isMirrorPass()) ? false : (!sRenderTransparentWater || gCubeSnapshot) && !sRenderingHUDs;
}

namespace
{
    // LLRHI: one walk of a spatial partition's octree for the six faces of a
    // reflection probe. For each face it does exactly what one walk with that
    // face's camera alone does (LLOctreeCull or LLOctreeCullNoFarClip for the
    // face's cull, LLOctreeCullVisExtents for its shadow point set): the same
    // tests on the same nodes in the same order, each face keeping its own
    // "partly / fully inside" state. A node is visited once for all the faces
    // that reach it. The cull's far clip sphere test of a node is made once for
    // the faces whose spheres are the same (origin and radius bit for bit; the
    // faces' radii come from their own frustum corners and can differ in the
    // last bit), and once per face otherwise.
    class ProbeFacesWalk : public LLViewerOctreeCull
    {
    public:
        enum EMode { CULL, CULL_NO_FAR_CLIP, VIS_EXTENTS };

        ProbeFacesWalk(EMode mode, LLCamera* cameras, U32 count)
            : LLViewerOctreeCull(&cameras[0]), mMode(mode), mCameras(cameras), mCount(llmin(count, (U32)6))
        {
            for (U32 f = 0; f < 6; ++f)
            {
                mFaceRes[f] = 0;
                mSphereOf[f] = f;
                mSphereGroup[f] = mSphereObjects[f] = -1;
            }
            for (U32 f = 1; f < mCount; ++f)
            {
                for (U32 g = 0; g < f; ++g)
                {
                    if (cameras[g].getOrigin() == cameras[f].getOrigin() &&
                        cameras[g].mFrustumCornerDist == cameras[f].mFrustumCornerDist)
                    {
                        mSphereOf[f] = g;
                        break;
                    }
                }
            }
        }

        // LLViewerOctreeCull::traverse for every face in faces.
        void walk(const OctreeNode* node, U32 faces)
        {
            LLViewerOctreeGroup* group = (LLViewerOctreeGroup*)node->getListener(0);

            if (earlyFail(group)) // occlusion only: the same for every face
            {
                return;
            }

            for (U32 f = 0; f < mCount; ++f)
            {
                mSphereGroup[f] = mSphereObjects[f] = -1;
            }
            U32 inside = 0; // faces that go into the node
            U32 tested = 0; // faces that tested the node and start over after it
            for (U32 f = 0; f < mCount; ++f)
            {
                const U32 bit = 1u << f;
                if (!(faces & bit))
                {
                    continue;
                }
                S32& res = mFaceRes[f];
                if (res == 2 || (res && group->hasState(LLViewerOctreeGroup::SKIP_FRUSTUM_CHECK)))
                {
                    inside |= bit;
                }
                else
                {
                    mCamera = &mCameras[f];
                    mFace = f;
                    res = frustumCheck(group);
                    tested |= bit;
                    if (res)
                    {
                        inside |= bit;
                    }
                }
            }

            if (inside)
            {
                visitFaces(node, group, inside);
                for (U32 i = 0; i < node->getChildCount(); ++i)
                {
                    walk(node->getChild(i), inside);
                }
            }

            for (U32 f = 0; f < mCount; ++f)
            {
                if (tested & (1u << f))
                {
                    mFaceRes[f] = 0;
                }
            }
        }

        // CULL modes: per face, the groups LLPipeline::markNotCulled would push.
        std::vector<LLSpatialGroup*>* mVisibleGroups = nullptr;  // [6]
        std::vector<LLSpatialGroup*>* mDrawableGroups = nullptr; // [6]
        // VIS_EXTENTS: per face, what LLOctreeCullVisExtents gathers.
        LLVector4a* mMin = nullptr;                              // [6]
        LLVector4a* mMax = nullptr;                              // [6]
        bool* mEmpty = nullptr;                                  // [6]

    protected:
        bool earlyFail(LLViewerOctreeGroup* base_group) override
        {
            LLSpatialGroup* group = (LLSpatialGroup*)base_group;
            if (mMode == VIS_EXTENTS)
            {   // LLOctreeCullVisExtents::earlyFail
                return group->getOctreeNode()->getParent() && LLPipeline::sUseOcclusion &&
                       group->isOcclusionState(LLSpatialGroup::OCCLUDED);
            }
            // LLOctreeCull::earlyFail
            if (LLPipeline::sReflectionRender)
            {
                return false;
            }
            group->checkOcclusion();
            if (group->getOctreeNode() && group->getOctreeNode()->getParent() && LLPipeline::sUseOcclusion &&
                group->isOcclusionState(LLSpatialGroup::OCCLUDED))
            {
                gPipeline.markOccluder(group);
                return true;
            }
            return false;
        }

        // With mCamera and mFace set to the face's.
        S32 frustumCheck(const LLViewerOctreeGroup* group) override
        {
            switch (mMode)
            {
                case CULL:
                {
                    S32 res = AABBInFrustumNoFarClipGroupBounds(group);
                    if (res != 0)
                    {
                        S32& sphere = mSphereGroup[mSphereOf[mFace]];
                        if (sphere < 0)
                        {
                            sphere = AABBSphereIntersectGroupExtents(group);
                        }
                        res = llmin(res, sphere);
                    }
                    return res;
                }
                case CULL_NO_FAR_CLIP:
                    return AABBInFrustumNoFarClipGroupBounds(group);
                default:
                    return AABBInFrustumGroupBounds(group);
            }
        }

        S32 frustumCheckObjects(const LLViewerOctreeGroup* group) override
        {
            switch (mMode)
            {
                case CULL:
                {
                    S32 res = AABBInFrustumNoFarClipObjectBounds(group);
                    if (res != 0)
                    {
                        S32& sphere = mSphereObjects[mSphereOf[mFace]];
                        if (sphere < 0)
                        {
                            sphere = AABBSphereIntersectObjectExtents(group);
                        }
                        res = llmin(res, sphere);
                    }
                    return res;
                }
                case CULL_NO_FAR_CLIP:
                    return AABBInFrustumNoFarClipObjectBounds(group);
                default:
                    return AABBInFrustumObjectBounds(group);
            }
        }

    private:
        // LLViewerOctreeCull::visit (checkObjects, then processGroup) for every face in faces.
        void visitFaces(const OctreeNode* node, LLViewerOctreeGroup* base_group, U32 faces)
        {
            if (node->getElementCount() == 0)
            {
                return;
            }
            if (node->getChildCount() != 0) // a leaf was already checked against its tightest box
            {
                for (U32 f = 0; f < mCount; ++f)
                {
                    const U32 bit = 1u << f;
                    if ((faces & bit) && mFaceRes[f] == 1)
                    {
                        mCamera = &mCameras[f];
                        mFace = f;
                        if (!frustumCheckObjects(base_group))
                        {
                            faces &= ~bit;
                        }
                    }
                }
            }
            if (!faces)
            {
                return;
            }

            LLSpatialGroup* group = (LLSpatialGroup*)base_group;
            if (mMode != VIS_EXTENTS)
            {   // LLPipeline::markNotCulled
                if (group->isEmpty())
                {
                    return;
                }
                std::vector<LLSpatialGroup*>* lists = group->getSpatialPartition()->mRenderByGroup ? mVisibleGroups : mDrawableGroups;
                for (U32 f = 0; f < mCount; ++f)
                {
                    if (faces & (1u << f))
                    {
                        lists[f].push_back(group);
                    }
                }
                return;
            }

            // LLOctreeCullVisExtents::processGroup
            for (U32 f = 0; f < mCount; ++f)
            {
                if (!(faces & (1u << f)))
                {
                    continue;
                }
                if (mFaceRes[f] < 2)
                {
                    mCamera = &mCameras[f];
                    mFace = f;
                    if (AABBInFrustumObjectBounds(group) > 0)
                    {
                        mEmpty[f] = false;
                        const LLVector4a* exts = group->getObjectExtents();
                        update_min_max(mMin[f], mMax[f], exts[0]);
                        update_min_max(mMin[f], mMax[f], exts[1]);
                    }
                }
                else
                {
                    mEmpty[f] = false;
                    const LLVector4a* exts = group->getExtents();
                    update_min_max(mMin[f], mMax[f], exts[0]);
                    update_min_max(mMin[f], mMax[f], exts[1]);
                }
            }
        }

        EMode mMode;
        LLCamera* mCameras;
        U32 mCount;
        S32 mFaceRes[6];
        U32 mFace = 0;           // the face mCamera is
        U32 mSphereOf[6];        // the first face with the same far clip sphere
        S32 mSphereGroup[6];     // this node's far clip sphere tests, by mSphereOf (-1: not made)
        S32 mSphereObjects[6];
    };

    // The planes, origin and far clip radius a walk tests against, bit for bit
    // (LLPlane::equal allows a tolerance).
    bool same_cull_camera(LLCamera& a, LLCamera& b, bool user_clip)
    {
        const U32 planes = user_clip ? LLCamera::AGENT_PLANE_USER_CLIP_NUM : LLCamera::AGENT_PLANE_NO_USER_CLIP_NUM;
        if (a.getAgentPlaneCount() != planes || b.getAgentPlaneCount() != planes)
        {
            return false;
        }
        for (U32 i = 0; i < planes; ++i)
        {
            const LLPlane& pa = a.getAgentPlane(i);
            const LLPlane& pb = b.getAgentPlane(i);
            for (S32 k = 0; k < 4; ++k)
            {
                if (pa[k] != pb[k])
                {
                    return false;
                }
            }
        }
        return a.getOrigin() == b.getOrigin() && a.mFrustumCornerDist == b.mFrustumCornerDist;
    }

    typedef std::vector<LLPipeline::SharedProbeCull::Walked> probe_partitions_t;

    // The partitions updateCull or getVisibleExtents walk under the current render types.
    void collect_probe_partitions(probe_partitions_t& parts)
    {
        parts.clear();
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (part && gPipeline.hasRenderType(part->mDrawableType))
                {
                    LLSpatialGroup* root = (LLSpatialGroup*)part->mOctree->getListener(0);
                    parts.push_back({ part, root, root->getUnboundCount() });
                }
            }
        }
    }

    // Why a face of the realtime probe walked alone (counted for the first face
    // of a probe that did; its later faces follow it).
    enum EProbeAlone
    {
        ALONE_CAMERA_FIRST, // the first face's camera is not the one cubeSnapshot set up
        ALONE_CAMERA,       // a later face's camera is not
        ALONE_WATER,        // the water clip changed
        ALONE_TYPES,        // the render types changed
        ALONE_PARTITIONS,   // other partitions, or a root unbound (and rebound) since the walk
        ALONE_DIRTY,        // a root dirty: an octree changed since the walk
        ALONE_REASONS
    };
    const char* const sProbeAloneNames[ALONE_REASONS] = { "first camera", "camera", "water clip", "render types", "partitions",
                                                         "a root dirty" };

    // True when the same partitions would be walked and none of their octrees changed
    // bounds, contents or shape since the shared walk: such a change makes the root
    // dirty (unbound), which a clean root with the same unbound count never was.
    bool probe_partitions_unchanged(const probe_partitions_t& walked, EProbeAlone& why)
    {
        static probe_partitions_t now;
        collect_probe_partitions(now);
        if (now != walked)
        {
            why = ALONE_PARTITIONS;
            return false;
        }
        for (const auto& part : now)
        {
            if (part.root->isDirty())
            {
                why = ALONE_DIRTY;
                return false;
            }
        }
        return true;
    }

    bool same_render_types(const std::string& saved, const bool* enabled, size_t size)
    {
        return saved.size() == size && !memcmp(saved.data(), enabled, size);
    }

    struct ProbeSharedCullStats
    {
        U32 probes = 0;
        U32 faces_shared = 0;      // faces whose groups came from the shared walk
        U32 faces_alone = 0;       // faces that walked alone
        U32 extents_shared = 0;
        U32 extents_alone = 0;
        U64 groups = 0;            // groups handed to the faces from the shared walk
        U32 alone_why[ALONE_REASONS] = {};      // probes whose faces fell back, by the first face's reason
        U32 extents_alone_why[ALONE_REASONS] = {};
        F32 logged_at = 0.f;
    };
    ProbeSharedCullStats sProbeSharedCullStats;

    void log_probe_alone(std::ostream& out, const U32* counts)
    {
        bool any = false;
        for (U32 r = 0; r < ALONE_REASONS; ++r)
        {
            if (counts[r])
            {
                out << (any ? ", " : " (fell back on ") << sProbeAloneNames[r] << " " << counts[r];
                any = true;
            }
        }
        if (any)
        {
            out << ")";
        }
    }
}

void LLPipeline::beginSharedProbeCull()
{
    SharedProbeCull& shared = mSharedProbeCull;
    shared.active = LLRHI::sProbeSharedCull;     // <plus> research/207 6.3 </plus>
    shared.face = 0;
    shared.cameras_ready = false;
    shared.culled = false;
    shared.extents_ready = false;
}

void LLPipeline::endSharedProbeCull()
{
    SharedProbeCull& shared = mSharedProbeCull;
    shared.active = false;
    shared.cameras_ready = false;
    shared.culled = false;
    shared.extents_ready = false;
    for (U32 f = 0; f < 6; ++f)
    {
        shared.visible_groups[f].clear();
        shared.drawable_groups[f].clear();
    }
    shared.partitions.clear();
    shared.extents_partitions.clear();
}

bool LLPipeline::cullProbeFaceShared(LLCamera& camera, bool water_clip)
{
    SharedProbeCull& shared = mSharedProbeCull;
    if (!shared.active || !shared.cameras_ready || shared.face >= 6 || !gCubeSnapshot || sShadowRender || sReflectionRender ||
        sUseOcclusion != 0 || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD ||
        &camera != static_cast<LLCamera*>(LLViewerCamera::getInstance()))
    {
        return false;
    }

    ProbeSharedCullStats& stats = sProbeSharedCullStats;
    const U32 face = shared.face;
    if (face == 0)
    {
        if (gFrameTimeSeconds - stats.logged_at >= 10.f)
        {
            if (stats.probes)
            {
                std::ostringstream alone, extents_alone;
                log_probe_alone(alone, stats.alone_why);
                log_probe_alone(extents_alone, stats.extents_alone_why);
                LL_INFOS("RHIProbe") << "probe shared cull over " << stats.probes << " probes: faces from the shared walk "
                                     << stats.faces_shared << ", walked alone " << stats.faces_alone << alone.str()
                                     << ", groups handed out " << stats.groups << "; shadow point sets shared "
                                     << stats.extents_shared << ", alone " << stats.extents_alone << extents_alone.str() << LL_ENDL;
            }
            stats = ProbeSharedCullStats();
            stats.logged_at = gFrameTimeSeconds;
        }
        ++stats.probes;

        shared.culled = false;
        shared.extents_ready = false;
        shared.water_clip = water_clip;

        // The faces' cameras get the clip plane updateCull just gave this one.
        for (U32 f = 0; f < 6; ++f)
        {
            if (water_clip)
            {
                LLPlane plane = camera.getAgentPlane(LLCamera::AGENT_PLANE_USER_CLIP);
                shared.cameras[f].setUserClipPlane(plane);
            }
            else
            {
                shared.cameras[f].disableUserClipPlane();
            }
        }
        if (!same_cull_camera(camera, shared.cameras[0], water_clip))
        {
            ++stats.faces_alone;
            ++stats.alone_why[ALONE_CAMERA_FIRST];
            return false;
        }

        shared.render_types.assign((const char*)mRenderTypeEnabled, sizeof(mRenderTypeEnabled));
        for (U32 f = 0; f < 6; ++f)
        {
            shared.visible_groups[f].clear();
            shared.drawable_groups[f].clear();
        }
        collect_probe_partitions(shared.partitions);
        for (auto& part : shared.partitions)
        {   // as LLSpatialPartition::cull
            part.root->rebound();
            part.unbounds = part.root->getUnboundCount();
            ProbeFacesWalk walk(part.part->mInfiniteFarClip ? ProbeFacesWalk::CULL_NO_FAR_CLIP : ProbeFacesWalk::CULL,
                                shared.cameras, 6);
            walk.mVisibleGroups = shared.visible_groups;
            walk.mDrawableGroups = shared.drawable_groups;
            walk.walk(part.part->mOctree, (1u << 6) - 1);
        }
        shared.culled = true;
    }
    else
    {
        EProbeAlone why = ALONE_REASONS;    // none: an earlier face of this probe fell back
        bool ok = shared.culled;
        if (ok && water_clip != shared.water_clip)
        {
            why = ALONE_WATER;
            ok = false;
        }
        else if (ok && !same_cull_camera(camera, shared.cameras[face], water_clip))
        {
            why = ALONE_CAMERA;
            ok = false;
        }
        else if (ok && !same_render_types(shared.render_types, mRenderTypeEnabled, sizeof(mRenderTypeEnabled)))
        {
            why = ALONE_TYPES;
            ok = false;
        }
        else if (ok)
        {
            ok = probe_partitions_unchanged(shared.partitions, why);
        }
        if (!ok)
        {
            // The rest of this probe's faces walk alone too.
            if (why != ALONE_REASONS)
            {
                ++stats.alone_why[why];
            }
            shared.culled = false;
            shared.extents_ready = false;
            ++stats.faces_alone;
            return false;
        }
    }

    // What markNotCulled does for each group this face's own walk would find, in the same order.
    for (LLSpatialGroup* group : shared.visible_groups[face])
    {
        group->setVisible();
        sCull->pushVisibleGroup(group);
    }
    for (LLSpatialGroup* group : shared.drawable_groups[face])
    {
        group->setVisible();
        sCull->pushDrawableGroup(group);
    }
    const U32 count = (U32)(shared.visible_groups[face].size() + shared.drawable_groups[face].size());
    mNumVisibleNodes += count;
    stats.groups += count;
    ++stats.faces_shared;
    return true;
}

bool LLPipeline::probeFaceExtentsShared(LLCamera& camera, LLVector3& min, LLVector3& max, bool& empty)
{
    SharedProbeCull& shared = mSharedProbeCull;
    if (!shared.active || !shared.culled || shared.face >= 6 || !gCubeSnapshot || sUseOcclusion != 0)
    {
        return false;
    }

    ProbeSharedCullStats& stats = sProbeSharedCullStats;
    const U32 face = shared.face;
    if (face == 0)
    {
        shared.extents_ready = false;
        // As generateSunShadow makes its main_camera from each face's camera.
        for (U32 f = 0; f < 6; ++f)
        {
            shared.extents_cameras[f] = shared.cameras[f];
            shared.extents_cameras[f].calcAgentFrustumPlanes(shared.extents_cameras[f].mAgentFrustum);
        }
        if (!same_cull_camera(camera, shared.extents_cameras[0], shared.water_clip))
        {
            ++stats.extents_alone;
            ++stats.extents_alone_why[ALONE_CAMERA_FIRST];
            return false;
        }

        shared.extents_render_types.assign((const char*)mRenderTypeEnabled, sizeof(mRenderTypeEnabled));
        // As getVisibleExtents and LLSpatialPartition::getVisibleExtents, per face.
        const F32 X = 65536.f;
        LLVector4a lo[6], hi[6];
        bool none[6];
        for (U32 f = 0; f < 6; ++f)
        {
            lo[f].set(X, X, X);
            hi[f].set(-X, -X, -X);
            none[f] = true;
        }
        collect_probe_partitions(shared.extents_partitions);
        for (auto& part : shared.extents_partitions)
        {   // as LLSpatialPartition::getVisibleExtents
            part.root->rebound();
            part.unbounds = part.root->getUnboundCount();
            ProbeFacesWalk walk(ProbeFacesWalk::VIS_EXTENTS, shared.extents_cameras, 6);
            walk.mMin = lo;
            walk.mMax = hi;
            walk.mEmpty = none;
            walk.walk(part.part->mOctree, (1u << 6) - 1);
        }
        for (U32 f = 0; f < 6; ++f)
        {
            shared.extents_min[f].set(lo[f].getF32ptr());
            shared.extents_max[f].set(hi[f].getF32ptr());
            shared.extents_empty[f] = none[f];
        }
        shared.extents_ready = true;
    }
    else
    {
        EProbeAlone why = ALONE_REASONS;    // none: an earlier face of this probe fell back
        bool ok = shared.extents_ready;
        if (ok && !same_cull_camera(camera, shared.extents_cameras[face], shared.water_clip))
        {
            why = ALONE_CAMERA;
            ok = false;
        }
        else if (ok && !same_render_types(shared.extents_render_types, mRenderTypeEnabled, sizeof(mRenderTypeEnabled)))
        {
            why = ALONE_TYPES;
            ok = false;
        }
        else if (ok)
        {
            ok = probe_partitions_unchanged(shared.extents_partitions, why);
        }
        if (!ok)
        {
            if (why != ALONE_REASONS)
            {
                ++stats.extents_alone_why[why];
            }
            shared.extents_ready = false; // the rest of this probe's faces gather alone too
            ++stats.extents_alone;
            return false;
        }
    }

    min = shared.extents_min[face];
    max = shared.extents_max[face];
    empty = shared.extents_empty[face];
    ++stats.extents_shared;
    return true;
}

void LLPipeline::updateCull(LLCamera& camera, LLCullResult& result)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE; //LL_RECORD_BLOCK_TIME(FTM_CULL);
    LL_PROFILE_GPU_ZONE("updateCull"); // should always be zero GPU time, but drop a timer to flush stuff out

    // <plus> research/184 9: the main camera's cull in finer FrameCost segments (LLRHI_FRAME_COST=2; off: one flag test)
    const bool cost = plus::cullinto::costDetail(camera);
    plus::cullinto::costMark(cost, "cull setup");
    // </plus>
    setCullClipPlane(camera);

    grabReferences(result);

    sCull->clear();

    // LLRHI: a face of the realtime reflection probe can take its groups from
    // one walk of the octrees for all six faces (research/44, P2).
    const bool shared_cull = cullProbeFaceShared(camera, isWaterClip());

    // <plus> research/184 A1: the main camera's walks on the cull threads, by subtree (plus/cullinto); the loop below in the same order
    if (!shared_cull && plus::cullinto::run(camera))
    {
        plus::cullinto::costMark(cost, "cull sky"); // research/184 9
        cullSky(camera);
        return;
    }
    // </plus>
    // <plus> research/188: the main camera's walks replayed from their traces while it stands still (plus/cullinto/cullreuse)
    plus::cullreuse::beginCull(camera, !shared_cull);
    // </plus>

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;

        // <plus> research/184 9: with the finer segments the roots' bounds first, apart (cull() rebounds
        // them again: nothing left to do then); the walks below as before
        if (cost && !shared_cull)
        {
            plus::cullinto::costMark(cost, "cull rebound");
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (part && hasRenderType(part->mDrawableType))
                {
                    ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();
                }
            }
        }
        plus::cullinto::costMark(cost, "cull walk");
        // </plus>
        for (U32 i = 0; !shared_cull && i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            // <plus> research/188: part->cull(camera) or its replay; a position not culled is forgotten
            // (plus/cullinto/cullreuse) (was: part->cull(camera) for a partition whose type is drawn)
            if (part && hasRenderType(part->mDrawableType))
            {
                plus::cullreuse::cull(part, camera);
            }
            else
            {
                plus::cullreuse::skip();
            }
            // </plus>
        }

        //scan the VO Cache tree
        LLVOCachePartition* vo_part = region->getVOCachePartition();
        if(vo_part)
        {
            plus::cullinto::costMark(cost, "cull vo cache"); // <plus> research/184 9 </plus>
            vo_part->cull(camera, sUseOcclusion > 0);
        }
    }

    plus::cullreuse::endCull(); // <plus> research/188: the counts, the auto mode, the VKPerf line </plus>
    plus::cullinto::costMark(cost, "cull sky"); // <plus> research/184 9 </plus>
    cullSky(camera);
}

void LLPipeline::setCullClipPlane(LLCamera& camera)
{
    bool water_clip = isWaterClip();

    if (water_clip)
    {

        LLVector3 pnorm;

        F32 water_height = LLEnvironment::instance().getWaterHeight();

        if (sUnderWaterRender)
        {
            //camera is below water, cull above water
            pnorm.setVec(0, 0, 1);
        }
        else
        {
            //camera is above water, cull below water
            pnorm = LLVector3(0, 0, -1);
        }

        LLPlane plane;
        plane.setVec(LLVector3(0, 0, water_height), pnorm);

        camera.setUserClipPlane(plane);
    }
    else
    {
        camera.disableUserClipPlane();
    }
}

void LLPipeline::cullSky(LLCamera& camera)
{
    if (hasRenderType(LLPipeline::RENDER_TYPE_SKY) &&
        gSky.mVOSkyp.notNull() &&
        gSky.mVOSkyp->mDrawable.notNull())
    {
        gSky.mVOSkyp->mDrawable->setVisible(camera);
        sCull->pushDrawable(gSky.mVOSkyp->mDrawable);
        gSky.updateCull();
        stop_glerror();
    }

    if (hasRenderType(LLPipeline::RENDER_TYPE_WL_SKY) &&
        gPipeline.canUseWindLightShaders() &&
        gSky.mVOWLSkyp.notNull() &&
        gSky.mVOWLSkyp->mDrawable.notNull())
    {
        gSky.mVOWLSkyp->mDrawable->setVisible(camera);
        sCull->pushDrawable(gSky.mVOWLSkyp->mDrawable);
    }
}

// LLRHI: zero coverage cull (research/61). The view of the cull markNotCulled
// serves, null outside the passes that have one: 0 the main camera, 1..4 the
// sun shadow cascades. Set and cleared right around their culls.
static LLRHI::CoverageView sRHICoverageViews[5];
static LLRHI::CoverageView* sRHICoverage = nullptr;

static void rhi_coverage_set(LLRHI::CoverageView& view, const glm::mat4& view_proj, U32 width, U32 height, bool cull)
{
    memcpy(view.view_proj, glm::value_ptr(view_proj), sizeof(view.view_proj));
    view.width = (F32)width;
    view.height = (F32)height;
    view.active = (LLRHI::sZeroCoverageCull || LLRHI::sTinyStats) && width > 0 && height > 0;
    view.cull = view.active && cull && LLRHI::sZeroCoverageCull;
    if (view.active && LLRHI::sTinyStats)
    {
        ++view.counts.frames;
    }
}

// LLRHI_TINY_STATS: every 5 s, per pass, what its cull found per frame (research/61).
static void rhi_coverage_log()
{
    static F32 logged_at = 0.f;
    if (!LLRHI::sTinyStats)
    {
        for (LLRHI::CoverageView& view : sRHICoverageViews)
        {
            view.counts = LLRHI::CoverageCounts();
        }
        logged_at = gFrameTimeSeconds;
        return;
    }
    if (gFrameTimeSeconds - logged_at < 5.f)
    {
        return;
    }
    logged_at = gFrameTimeSeconds;
    static const char* const names[5] = { "main", "sun0", "sun1", "sun2", "sun3" };
    for (U32 i = 0; i < 5; ++i)
    {
        LLRHI::CoverageCounts& c = sRHICoverageViews[i].counts;
        if (c.frames)
        {
            const F64 f = (F64)c.frames;
            LL_INFOS("RHITiny") << llformat("tiny %s over %u frames, groups (draws) per frame: tested %.0f (%.0f), "
                                            "no pixel centre %.1f (%.1f), <=1x1 %.1f (%.1f), <=2x2 %.1f (%.1f), skipped %.1f (%.1f)",
                                            names[i], c.frames, c.groups / f, c.draws / f, c.zero_groups / f, c.zero_draws / f,
                                            c.one_groups / f, c.one_draws / f, c.two_groups / f, c.two_draws / f,
                                            c.skipped_groups / f, c.skipped_draws / f) << LL_ENDL;
        }
        c = LLRHI::CoverageCounts();
    }
}

void LLPipeline::rhiCoverageMainBegin(bool for_snapshot)
{
    rhi_coverage_log();
    sRHICoverage = nullptr;
    LLRHI::CoverageView& view = sRHICoverageViews[0];
    view.active = false;
    view.cull = false;
    if (!LLRHI::sZeroCoverageCull && !LLRHI::sTinyStats)
    {
        return;
    }
    // Only the plain world view: its matrices are the ones the frame's G-buffer,
    // alpha and glow passes draw with (setup3DRender; generateSunShadow puts
    // them back), into targets of the G-buffer's size with a viewport covering
    // all of it. Not snapshots (tiles, zoom), wireframe (lines reach half a
    // pixel beyond the bounds), the CameraOffset debug view (draws from a shadow
    // camera) or a frame that resizes the screen targets after this cull.
    LLViewerCamera* camera = LLViewerCamera::getInstance();
    if (for_snapshot || gCubeSnapshot || gUseWireframe || CameraOffset || gResizeScreenTexture || mRT != &mMainRT ||
        sShadowRender || sReflectionRender || sImpostorRender || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD ||
        camera->getZoomFactor() != 1.f)
    {
        return;
    }
    // Debug displays and beacons draw from the cull's groups too: count only.
    const bool cull = !mRenderDebugMask && !LLFloaterReg::instanceVisible("beacons");
    rhi_coverage_set(view, get_current_projection() * get_current_modelview(), mMainRT.deferredScreen.getWidth(),
                     mMainRT.deferredScreen.getHeight(), cull);
    view.jitter = plus::taa::jitterMargin(); // <plus> plus/taa (research/185 §13): the G-buffer may be drawn moved by this much </plus>
    if (view.active)
    {
        sRHICoverage = &view;
    }
}

void LLPipeline::rhiCoverageMainEnd()
{
    sRHICoverage = nullptr;
}

void LLPipeline::markNotCulled(LLSpatialGroup* group, LLCamera& camera)
{
    if (group->isEmpty())
    {
        return;
    }

    if (group->mRHIListResident && (sRHIListCullKinds & group->mRHIListKind))
    {
        return; // LLRHI: the GPU shadow list culls and draws this group for the sun shadow camera
    }

    group->setVisible();

    if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot)
    {
        group->updateDistance(camera);
    }

    assertInitialized();

    // <plus> research/184 A1: on a cull thread the pushes and the count are recorded, and done by the
    // main thread in the serial walk's order (plus/cullinto; markOccluder records itself)
    plus::cullinto::Record* const record = plus::cullinto::tRecord;
    // </plus>

    // LLRHI: a group whose bounds hold no pixel centre of this pass draws nothing
    // in it (research/61). It stays visible (textures, LOD bookkeeping, the
    // object cache and occlusion go on as before); only its sort and draws go.
    if (sRHICoverage && rhi_zero_coverage(group, *sRHICoverage))
    {
        // left out of the cull result
    }
    else if (!group->getSpatialPartition()->mRenderByGroup)
    { //render by drawable
        // <plus> research/184 A1 (was: the push alone)
        if (record)
        {
            record->push(plus::cullinto::Op::DRAWABLE_GROUP, group);
        }
        else
        {
            sCull->pushDrawableGroup(group);
        }
        // </plus>
    }
    else
    {   //render by group
        // <plus> research/184 A1 (was: the push alone)
        if (record)
        {
            record->push(plus::cullinto::Op::VISIBLE_GROUP, group);
        }
        else
        {
            sCull->pushVisibleGroup(group);
        }
        // </plus>
    }

    if (group->needsUpdate() ||
        group->getVisible(LLViewerCamera::sCurCameraID) < LLDrawable::getCurrentFrame() - 1)
    {
        // include this group in occlusion groups, not because it is an occluder, but because we want to run
        // an occlusion query to find out if it's an occluder
        markOccluder(group);
    }
    // <plus> research/184 A1 (was: the count alone)
    if (record)
    {
        ++record->visible_nodes;
    }
    else
    {
        mNumVisibleNodes++;
    }
    // </plus>
}

void LLPipeline::markOccluder(LLSpatialGroup* group)
{
    // <plus> research/184 A1: on a cull thread, done by the main thread in the serial walk's order (it
    // writes the parent's state, which the walk of a sibling's subtree reads here) (plus/cullinto)
    if (plus::cullinto::Record* record = plus::cullinto::tRecord)
    {
        record->push(plus::cullinto::Op::MARK_OCCLUDER, group);
        return;
    }
    // </plus>
    if (sUseOcclusion > 1 && group && !group->isOcclusionState(LLSpatialGroup::ACTIVE_OCCLUSION))
    {
        LLSpatialGroup* parent = group->getParent();

        if (!parent || !parent->isOcclusionState(LLSpatialGroup::OCCLUDED))
        { //only mark top most occluders as active occlusion
            sCull->pushOcclusionGroup(group);
            group->setOcclusionState(LLSpatialGroup::ACTIVE_OCCLUSION);

            if (parent &&
                !parent->isOcclusionState(LLSpatialGroup::ACTIVE_OCCLUSION) &&
                parent->getElementCount() == 0 &&
                parent->needsUpdate())
            {
                sCull->pushOcclusionGroup(group);
                parent->setOcclusionState(LLSpatialGroup::ACTIVE_OCCLUSION);
            }
        }
    }
}

void LLPipeline::doOcclusion(LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("doOcclusion");
    llassert(!gCubeSnapshot);
    // <plus> skin mark (plus/skinsss): the occlusion boxes leave the skin stencil bit alone
    plus::skinsss::OcclusionPause skin_mark_pause;
    // </plus>

    if (sReflectionProbesEnabled && sUseOcclusion > 1 && !LLPipeline::sShadowRender && !gCubeSnapshot)
    {
        gGL.setColorMask(false, false);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        LLGLDisable cull(GL_CULL_FACE);

        gOcclusionCubeProgram.bind();

        if (mCubeVB.isNull())
        { //cube VB will be used for issuing occlusion queries
            mCubeVB = ll_create_cube_vb(LLVertexBuffer::MAP_VERTEX);
        }
        mCubeVB->setBuffer();

        LLRHI::sOcclusionScope = 2; // LLRHI
        LLRHI::Occlusion::Batch::get().open("probes"); // LLRHI native (research/137 N8)
        mReflectionMapManager.doOcclusion();
        mHeroProbeManager.doOcclusion();
        LLRHI::Occlusion::Batch::get().submit(); // LLRHI native (research/137 N8)
        LLRHI::sOcclusionScope = 0; // LLRHI
        gOcclusionCubeProgram.unbind();

        gGL.setColorMask(true, true);
    }

    if (sReflectionProbesEnabled && sUseOcclusion > 1 && !LLPipeline::sShadowRender && !gCubeSnapshot)
    {
        gGL.setColorMask(false, false);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        LLGLDisable cull(GL_CULL_FACE);

        gOcclusionCubeProgram.bind();

        if (mCubeVB.isNull())
        { //cube VB will be used for issuing occlusion queries
            mCubeVB = ll_create_cube_vb(LLVertexBuffer::MAP_VERTEX);
        }
        mCubeVB->setBuffer();

        LLRHI::sOcclusionScope = 2; // LLRHI
        LLRHI::Occlusion::Batch::get().open("hero probes"); // LLRHI native (research/137 N8)
        mHeroProbeManager.doOcclusion();
        LLRHI::Occlusion::Batch::get().submit(); // LLRHI native (research/137 N8)
        LLRHI::sOcclusionScope = 0; // LLRHI
        gOcclusionCubeProgram.unbind();

        gGL.setColorMask(true, true);
    }

    if (LLPipeline::sUseOcclusion > 1 &&
        (sCull->hasOcclusionGroups() || LLVOCachePartition::sNeedsOcclusionCheck))
    {
        LLVertexBuffer::unbind();

        gGL.setColorMask(false, false);

        LLGLDisable blend(GL_BLEND);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);

        LLGLDisable cull(GL_CULL_FACE);

        gOcclusionCubeProgram.bind();

        if (mCubeVB.isNull())
        { //cube VB will be used for issuing occlusion queries
            mCubeVB = ll_create_cube_vb(LLVertexBuffer::MAP_VERTEX);
        }
        mCubeVB->setBuffer();
        LLRHI::Occlusion::Batch::get().open("groups"); // LLRHI native (research/137 N8)

        for (LLCullResult::sg_iterator iter = sCull->beginOcclusionGroups(); iter != sCull->endOcclusionGroups(); ++iter)
        {
            LLSpatialGroup* group = *iter;
            if (!group->isDead())
            {
                group->doOcclusion(&camera);
                group->clearOcclusionState(LLSpatialGroup::ACTIVE_OCCLUSION);
            }
        }

        //apply occlusion culling to object cache tree
        for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
        {
            LLVOCachePartition* vo_part = (*iter)->getVOCachePartition();
            if(vo_part)
            {
                vo_part->processOccluders(&camera);
            }
        }
        LLRHI::Occlusion::Batch::get().submit(); // LLRHI native (research/137 N8)

        gGL.setColorMask(true, true);
    }
}

bool LLPipeline::updateDrawableGeom(LLDrawable* drawablep)
{
    bool update_complete = drawablep->updateGeometry();
    if (update_complete && assertInitialized())
    {
        drawablep->setState(LLDrawable::BUILT);
    }
    return update_complete;
}

void LLPipeline::updateGL()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    // <plus> research/261: the LUT targets' contents that createLUTBuffers could not make (no frame open yet).
    // Called at the head of display_startup and of the world's updates, outside any target: made at the login
    // screen once a frame is open, before the world's first reader (the probes, the mirrors, the sky). The world's
    // call only matters when the login screen drew nothing (minimized throughout).
    if (mLUTContentsPending)
    {
        makePendingLUTContents();
    }
    // </plus>
    {
        while (!LLGLUpdate::sGLQ.empty())
        {
            LLGLUpdate* glu = LLGLUpdate::sGLQ.front();
            glu->updateGL();
            glu->mInQ = false;
            LLGLUpdate::sGLQ.pop_front();
        }
    }
}

static bool waits_for_build_time(LLSpatialGroup* group);
static bool may_wait_for_rebuild(LLSpatialGroup* group);

void LLPipeline::clearRebuildGroups()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LLSpatialGroup::sg_vector_t hudGroups;

    mGroupQ1Locked = true;
    // Iterate through all drawables on the priority build queue,
    for (LLSpatialGroup::sg_vector_t::iterator iter = mGroupQ1.begin();
         iter != mGroupQ1.end(); ++iter)
    {
        LLSpatialGroup* group = *iter;

        if (!group || group->isDead())
        {
            continue;
        }
        // If the group contains HUD objects, save the group
        // LLRHI: also groups whose (re)build waits for frame time: nothing else
        // queues them again, and they would keep drawing nothing or old batches.
        if (group->isHUDGroup() ||
            (mPriorityRebuildBudget >= 0.f && may_wait_for_rebuild(group)) ||
            (mGroupRebuildBudget >= 0.f && waits_for_build_time(group)))
        {
            hudGroups.push_back(group);
        }
        // Else, no HUD objects so clear the build state
        else
        {
            group->clearState(LLSpatialGroup::IN_BUILD_Q1);
        }
    }

    // Clear the group
    mGroupQ1.clear();

    // Copy the saved HUD groups back in
    mGroupQ1.assign(hudGroups.begin(), hudGroups.end());
    mGroupQ1Locked = false;
}

void LLPipeline::clearRebuildDrawables()
{
    // Clear all drawables on the priority build queue,
    for (LLDrawable::drawable_list_t::iterator iter = mBuildQ1.begin();
         iter != mBuildQ1.end(); ++iter)
    {
        LLDrawable* drawablep = *iter;
        if (drawablep && !drawablep->isDead())
        {
            drawablep->clearState(LLDrawable::IN_REBUILD_Q);
        }
    }
    mBuildQ1.clear();

    //clear all moving bridges
    for (LLDrawable::drawable_vector_t::iterator iter = mMovedBridge.begin();
         iter != mMovedBridge.end(); ++iter)
    {
        LLDrawable *drawablep = *iter;
        drawablep->clearState(LLDrawable::EARLY_MOVE | LLDrawable::MOVE_UNDAMPED | LLDrawable::ON_MOVE_LIST | LLDrawable::ANIMATED_CHILD);
    }
    mMovedBridge.clear();

    //clear all moving drawables
    for (LLDrawable::drawable_vector_t::iterator iter = mMovedList.begin();
         iter != mMovedList.end(); ++iter)
    {
        LLDrawable *drawablep = *iter;
        drawablep->clearState(LLDrawable::EARLY_MOVE | LLDrawable::MOVE_UNDAMPED | LLDrawable::ON_MOVE_LIST | LLDrawable::ANIMATED_CHILD);
    }
    mMovedList.clear();

    for (LLDrawable::drawable_vector_t::iterator iter = mShiftList.begin();
        iter != mShiftList.end(); ++iter)
    {
        LLDrawable *drawablep = *iter;
        drawablep->clearState(LLDrawable::EARLY_MOVE | LLDrawable::MOVE_UNDAMPED | LLDrawable::ON_MOVE_LIST | LLDrawable::ANIMATED_CHILD | LLDrawable::ON_SHIFT_LIST);
    }
    mShiftList.clear();
}

// A group whose first build may wait for a later frame's time (setGroupRebuildBudget):
// a static volume group never built, so it has no draw infos to draw yet. Other
// partitions (terrain, trees, particles, ...) draw through their pools with empty
// draw maps, and a built group's old draw infos may point at matrices of drawables
// and faces that are gone, so those always build before they are drawn.
// A dead group's partition may be gone with its region (research/96): it is not
// looked at, and the group leaves the queue (rebuildGeom does nothing for it).
static bool waits_for_build_time(LLSpatialGroup* group)
{
    if (group->isDead())
    {
        return false;
    }
    const LLSpatialPartition* partition = group->getSpatialPartition();
    return partition && partition->mPartitionType == LLViewerRegion::PARTITION_VOLUME &&
           group->mDrawMap.empty() && group->hasState(LLSpatialGroup::GEOM_DIRTY);
}

// LLRHI: a built group whose rebuild may wait for a later frame's time
// (setPriorityRebuildBudget). Until then it draws its old batches, as it did
// before it was marked: they hold their vertex buffers, textures and materials;
// a static region group's model matrix is the region's, and its old geometry
// stays in its buffers (rebuildMesh leaves a GEOM_DIRTY group alone). It must
// rebuild now if anything those batches point at may have gone since its last
// build (mRHIRebuildNow: a drawable left, a texture matrix was freed, its GL state
// was destroyed) or they are rigged (skin info), and so must every other kind of
// group (bridges, avatars, particles, terrain, ...), alpha re-sorts (no
// GEOM_DIRTY) and never-built ones (waits_for_build_time), as upstream.
static bool may_wait_for_rebuild(LLSpatialGroup* group)
{
    if (group->isDead())
    {
        return false;   // its partition may be gone (waits_for_build_time)
    }
    LLSpatialPartition* partition = group->getSpatialPartition();
    return partition && partition->mPartitionType == LLViewerRegion::PARTITION_VOLUME && !partition->asBridge() &&
           group->hasState(LLSpatialGroup::GEOM_DIRTY) && !group->mDrawMap.empty() &&
           !group->mRHIRebuildNow && !group->mRHIRiggedBatches;
}

void LLPipeline::setPriorityRebuildBudget(F32 seconds)
{
    // Counters of the frame that ended, logged every 10 s.
    struct Stats
    {
        U32 frames = 0;
        U32 frames_waiting = 0;     // frames that left groups waiting
        F64 spent = 0.0;
        F32 spent_max = 0.f;
        U64 built = 0;
        U64 waiting = 0;
        U32 waiting_max = 0;
        F32 logged_at = 0.f;
        LLPipeline::RebuildCauses causes;       // <plus> research/295: summed over the frames </plus>
    };
    static Stats stats;
    if (mPriorityRebuildBudget >= 0.f)
    {
        ++stats.frames;
        stats.frames_waiting += mPriorityWaiting ? 1 : 0;
        stats.spent += mPriorityRebuildSpent;
        stats.spent_max = llmax(stats.spent_max, mPriorityRebuildSpent);
        stats.built += mPriorityBuiltWaiting;
        stats.waiting += mPriorityWaiting;
        stats.waiting_max = llmax(stats.waiting_max, mPriorityWaiting);
        // <plus> research/295: add this frame's causes, then start the next frame's (only with the periodic logs)
        if (LLRHI::sPeriodicLogs)
        {
            for (U32 i = 0; i < RC_COUNT; ++i)
            {
                stats.causes.count[i] += mRebuildCauses.count[i];
                stats.causes.seconds[i] += mRebuildCauses.seconds[i];
            }
            for (U32 i = 0; i < RCAM_COUNT; ++i)
            {
                stats.causes.camera_count[i] += mRebuildCauses.camera_count[i];
                stats.causes.camera_seconds[i] += mRebuildCauses.camera_seconds[i];
            }
            for (U32 i = 0; i < RC_TYPE_SLOTS; ++i)
            {
                stats.causes.type_count[i] += mRebuildCauses.type_count[i];
                stats.causes.type_seconds[i] += mRebuildCauses.type_seconds[i];
            }
            if (mRebuildCauses.worst_seconds > stats.causes.worst_seconds)
            {
                stats.causes.worst_seconds = mRebuildCauses.worst_seconds;
                stats.causes.worst_cause = mRebuildCauses.worst_cause;
                stats.causes.worst_camera = mRebuildCauses.worst_camera;
                stats.causes.worst_type = mRebuildCauses.worst_type;
            }
        }
        // </plus>
    }
    // <plus> research/295: the next frame's count starts empty
    if (LLRHI::sPeriodicLogs)
    {
        mRebuildCauses = RebuildCauses();
    }
    // </plus>
    if (gFrameTimeSeconds - stats.logged_at >= 10.f)
    {
        if (stats.frames)
        {
            const F64 frames = (F64)stats.frames;
            LL_INFOS("RHIProfile") << llformat("priority rebuilds over %u frames: %.2f ms per frame (max %.2f), "
                                               "built that could wait %.1f per frame, left waiting %.1f per frame "
                                               "(max %u, in %u frames)",
                                               stats.frames, stats.spent * 1000.0 / frames, stats.spent_max * 1000.f,
                                               (F64)stats.built / frames, (F64)stats.waiting / frames, stats.waiting_max,
                                               stats.frames_waiting)
                                   << LL_ENDL;
            // <plus> research/295: what was built, by cause, camera pass and partition (ms per frame / groups per frame), and the
            // slowest single group (ms). Counted: the groups of the priority queue's first loop (why each had to build now; the
            // ones that waited are not here) and of postSort's drawable groups loop (bridges, avatars, trees, once for each camera
            // pass); not counted: the never-built groups' loop. Only with the periodic logs (LLRHI_PERIODIC_LOGS).
            if (LLRHI::sPeriodicLogs)
            {
                static const char* const CAUSE_NAMES[RC_COUNT] = { "waitable", "alpha-only", "bridge", "rigged", "rebuild-now", "non-volume",
                                                                   "volume-other", "drawable-groups" };
                static const char* const CAMERA_NAMES[RCAM_COUNT] = { "main", "shadow", "impostor", "rain-map", "hud" };
                std::ostringstream detail;
                detail << std::fixed << std::setprecision(2) << "priority rebuild causes (ms per frame / groups per frame; the queue's first loop and"
                       << " postSort's drawable groups loop):";
                for (U32 i = 0; i < RC_COUNT; ++i)
                {
                    detail << " " << CAUSE_NAMES[i] << " " << stats.causes.seconds[i] * 1000.0 / frames << "/" << (F64)stats.causes.count[i] / frames << ";";
                }
                detail << " by camera:";
                for (U32 i = 0; i < RCAM_COUNT; ++i)
                {
                    detail << " " << CAMERA_NAMES[i] << " " << stats.causes.camera_seconds[i] * 1000.0 / frames << "/"
                           << (F64)stats.causes.camera_count[i] / frames << ";";
                }
                detail << " by partition type:";
                for (U32 i = 0; i < RC_TYPE_SLOTS; ++i)
                {
                    if (stats.causes.type_count[i])
                    {
                        detail << " " << i << " " << stats.causes.type_seconds[i] * 1000.0 / frames << "/" << (F64)stats.causes.type_count[i] / frames << ";";
                    }
                }
                if (stats.causes.worst_seconds > 0.f)
                {
                    detail << " slowest group " << stats.causes.worst_seconds * 1000.0 << " ms (" << CAUSE_NAMES[stats.causes.worst_cause] << ", "
                           << CAMERA_NAMES[stats.causes.worst_camera] << ", partition type " << stats.causes.worst_type << ")";
                }
                else
                {
                    detail << " slowest group none";
                }
                LL_INFOS("RHIProfile") << detail.str() << LL_ENDL;
            }
            // </plus>
        }
        stats = Stats();
        stats.logged_at = gFrameTimeSeconds;
    }

    mPriorityRebuildBudget = seconds;
    mPriorityRebuildSpent = 0.f;
    mPriorityRebuiltAny = false;
    mPriorityBuiltWaiting = 0;
    if (seconds < 0.f)
    {
        mPriorityWaiting = 0;
    }
}

void LLPipeline::updateLoadRadius(F32 dt)
{
    constexpr F32 START_RADIUS = 15.f;
    constexpr F32 GROWTH_RATE = 0.2f;           // of the radius per second once the inside is done
    constexpr S32 BLURRY_DONE = 32;             // on-screen textures still blurry, counted as "inside done"
    constexpr F32 QUIET_TIME = 1.f;             // seconds the inside must stay done (new objects' textures get requested meanwhile)
    constexpr F32 STALL_GROWTH = 0.02f;         // of the radius per second (at least MIN_GROWTH) once nothing grew for STALL_TIME
    constexpr F32 MIN_GROWTH = 2.f;             // m/s: everything still gets built
    constexpr F32 STALL_TIME = 10.f;
    constexpr U32 MESHES_DONE = 2;              // mesh objects inside still without their mesh
    constexpr S32 CREATE_DONE = 16;             // decoded textures of things inside still waiting for GL creation
    static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip", 256.f);

    if (!LLRHI::sGeomBudget || LLStartUp::getStartupState() < STATE_STARTED)
    {
        if (mLoadRadius >= 0.f)
        {
            // <plus> plus/version: the title keeps the version (was: setTitle(APP_NAME))
            plus::version::setTitleStatus("");
            // </plus>
        }
        mLoadRadius = -1.f;
        return;
    }
    // Start over after login and every teleport.
    if (!mLoadStarted || gAgent.getTeleportState() != LLAgent::TELEPORT_NONE)
    {
        mLoadStarted = true;
        mLoadRadius = START_RADIUS;
        mLoadQuiet = 0.f;
        mLoadStalled = 0.f;
    }
    if (mLoadRadius < 0.f)
    {
        return;
    }
    // Grow once the inside has stayed done for a second: nothing waiting to build
    // and few textures on screen still blurry. New objects' textures are requested
    // within that second (LLViewerTextureList::updateImagesUrgent). Requiring no
    // new object at all kept the radius still where updates keep arriving. If the
    // inside never settles, grow more slowly after a while anyway.
    dt = llclamp(dt, 0.f, 1.f);
    // Mesh objects inside whose mesh has not arrived build empty and want no
    // textures yet, so they look done: count them twice a second.
    static LLFrameTimer mesh_timer;
    static U32 meshes_pending = 0;
    if (mesh_timer.getElapsedTimeF32() > 0.5f)
    {
        mesh_timer.reset();
        meshes_pending = 0;
        const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
        for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
        {
            LLViewerObject* object = gObjectList.getObject(i);
            if (!object || object->getPCode() != LL_PCODE_VOLUME || !object->isMesh())
            {
                continue;
            }
            const LLVolume* volume = object->getVolume();
            if (!volume || volume->isMeshAssetLoaded() || volume->isMeshAssetUnavaliable())
            {
                continue;
            }
            const F32 distance = (object->getPositionAgent() - eye).length() - object->getScale().length() * 0.5f;
            if (distance <= mLoadRadius)
            {
                ++meshes_pending;
            }
        }
    }
    // The blurry count takes a full fetch pass to update; the creation backlog
    // is exact (the radius outran it while texture creation caught up).
    const bool done_now = mLoadWaitingInside == 0 && meshes_pending <= MESHES_DONE &&
                          LLViewerTextureList::sBlurryVisible <= BLURRY_DONE &&
                          LLViewerTextureList::sCreatePendingInside <= CREATE_DONE;
    mLoadBuiltInside = 0;
    mLoadQuiet = done_now ? mLoadQuiet + dt : 0.f;
    F32 growth = 0.f;
    if (mLoadQuiet >= QUIET_TIME)
    {
        growth = mLoadRadius * GROWTH_RATE;
        mLoadStalled = 0.f;
    }
    else
    {
        mLoadStalled += dt;
        if (mLoadStalled >= STALL_TIME)
        {
            growth = llmax(MIN_GROWTH, mLoadRadius * STALL_GROWTH);
        }
    }
    mLoadRadius += growth * dt;
    static S32 shown = -1;
    if (mLoadRadius >= far_clip)
    {
        mLoadRadius = -1.f;
        shown = -1;
        // <plus> plus/version: the title keeps the version and the place
        // (was: setTitle(APP_NAME), and "APP_NAME - loading nearby first ..." below)
        plus::version::setTitleStatus("");
        // </plus>
    }
    else if ((S32)mLoadRadius != shown)
    {
        shown = (S32)mLoadRadius;
        // <plus> plus/version: the title keeps the version and the place
        plus::version::setTitleStatus(llformat("loading nearby first (%dm / %.0fm)", shown, (F32)far_clip));
        // </plus>
    }
}

// <plus> research/295: whether the rebuild causes are counted this frame (the priority budget is on and the periodic logs are on)
bool LLPipeline::notesRebuildCauses() const
{
    return mPriorityRebuildBudget >= 0.f && LLRHI::sPeriodicLogs;
}

// Builds a group and adds its time to the frame's causes (why it had to build, which camera pass built it, which partition).
// Called for the priority queue's first loop (a group that waits is not here) and for postSort's drawable groups loop.
// A group with nothing dirty (that call would return at once) and a dead one are built without being counted:
// a count of groups is then a count of real builds.
static void rebuild_group_noting_cause(LLSpatialGroup* group, U32 forced_cause, LLPipeline::RebuildCauses& causes)
{
    static_assert(LLViewerRegion::NUM_PARTITIONS < LLPipeline::RC_TYPE_SLOTS, "a slot for each partition type and one for none");
    if (group->isDead())
    {
        // Its partition may be gone with its region (research/96): not looked at, not counted.
        group->rebuildGeom();
        return;
    }
    if (!group->hasState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::ALPHA_DIRTY))
    {
        group->rebuildGeom();   // nothing to build (or only its mesh, MESH_DIRTY): not a build
        return;
    }
    LLSpatialPartition* partition = group->getSpatialPartition();
    const U32 type = partition ? (U32)partition->mPartitionType : LLPipeline::RC_TYPE_SLOTS - 1;
    U32 cause = forced_cause;
    if (cause == LLPipeline::RC_COUNT)
    {
        cause = LLPipeline::RC_VOLUME_OTHER;
        if (partition && partition->asBridge())
        {
            cause = LLPipeline::RC_BRIDGE;
        }
        else if (partition && partition->mPartitionType != LLViewerRegion::PARTITION_VOLUME)
        {
            cause = LLPipeline::RC_NON_VOLUME;
        }
        else if (group->mRHIRiggedBatches)
        {
            cause = LLPipeline::RC_RIGGED;
        }
        else if (group->mRHIRebuildNow)
        {
            cause = LLPipeline::RC_REBUILD_NOW;
        }
        else if (group->hasState(LLSpatialGroup::ALPHA_DIRTY) && !group->hasState(LLSpatialGroup::GEOM_DIRTY))
        {
            cause = LLPipeline::RC_ALPHA_ONLY;
        }
    }
    // (Cube snapshots, the probes and the mirrors, build nothing here: postSort skips its rebuilds for them.)
    const U32 camera = LLPipeline::sRenderingHUDs ? LLPipeline::RCAM_HUD :
                       LLPipeline::sImpostorRender ? LLPipeline::RCAM_IMPOSTOR :
                       (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WATER0 || LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WATER1) ?
                           LLPipeline::RCAM_RAIN_MAP :
                       LLPipeline::sShadowRender ? LLPipeline::RCAM_SHADOW : LLPipeline::RCAM_MAIN;
    LLTimer timer;
    group->rebuildGeom();
    const F32 seconds = timer.getElapsedTimeF32();
    ++causes.count[cause];
    causes.seconds[cause] += seconds;
    ++causes.camera_count[camera];
    causes.camera_seconds[camera] += seconds;
    ++causes.type_count[type];
    causes.type_seconds[type] += seconds;
    if (seconds > causes.worst_seconds)
    {
        causes.worst_seconds = seconds;
        causes.worst_cause = cause;
        causes.worst_camera = camera;
        causes.worst_type = type;
    }
}
// </plus>

void LLPipeline::rebuildPriorityGroups()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("rebuildPriorityGroups");

    LLTimer update_timer;
    assertInitialized();

    gMeshRepo.notifyLoadedMeshes();

    mGroupQ1Locked = true;
    // Groups that must build do so now; never-built ones (waits_for_build_time)
    // build nearest the camera first within the frame's time, the rest stay queued.
    // LLRHI: built groups that may wait (may_wait_for_rebuild) build in queue order
    // while the queue's loop is within the frame's priority time; the rest keep their
    // place and draw their old batches meanwhile.
    LLSpatialGroup::sg_vector_t later;
    std::vector<std::pair<F32, LLSpatialGroup*>> waiting;
    LLVector4a eye;
    eye.load3(LLViewerCamera::getInstance()->getOrigin().mV);
    const bool priority_budget = mPriorityRebuildBudget >= 0.f;
    const bool note_causes = notesRebuildCauses();     // <plus> research/295: counted only when the periodic logs are on </plus>
    LLTimer priority_timer;
    bool priority_out_of_time = false;
    U32 priority_waiting = 0;
    // Iterate through all drawables on the priority build queue,
    for (LLSpatialGroup::sg_vector_t::iterator iter = mGroupQ1.begin();
         iter != mGroupQ1.end(); ++iter)
    {
        LLSpatialGroup* group = *iter;
        if (mGroupRebuildBudget >= 0.f && waits_for_build_time(group))
        {
            LLVector4a offset;
            offset.setSub(group->getBounds()[0], eye);
            waiting.emplace_back(offset.getLength3().getF32(), group);
            continue;
        }
        bool waitable = false;      // <plus> research/295 </plus>
        if (priority_budget && may_wait_for_rebuild(group))
        {
            // At least one a frame, so the queue always moves.
            priority_out_of_time = priority_out_of_time ||
                (mPriorityRebuiltAny &&
                 mPriorityRebuildSpent + priority_timer.getElapsedTimeF32() >= mPriorityRebuildBudget);
            if (priority_out_of_time)
            {
                later.push_back(group);     // keeps IN_BUILD_Q1 and its place
                ++priority_waiting;
                continue;
            }
            mPriorityRebuiltAny = true;
            ++mPriorityBuiltWaiting;
            waitable = true;    // <plus> research/295 </plus>
        }
        // <plus> research/295: the cause and the time of each group built in this loop
        if (note_causes)
        {
            rebuild_group_noting_cause(group, waitable ? (U32)RC_WAITABLE : (U32)RC_COUNT, mRebuildCauses);
        }
        else
        {
            group->rebuildGeom();
        }
        // </plus>
        group->clearState(LLSpatialGroup::IN_BUILD_Q1);
    }
    if (priority_budget)
    {
        mPriorityRebuildSpent += priority_timer.getElapsedTimeF32();
        mPriorityWaiting = priority_waiting;
    }
    std::sort(waiting.begin(), waiting.end(),
              [](const std::pair<F32, LLSpatialGroup*>& a, const std::pair<F32, LLSpatialGroup*>& b) { return a.first < b.first; });
    U32 waiting_inside = 0;
    for (const auto& entry : waiting)
    {
        LLSpatialGroup* group = entry.second;
        // Outside the loading radius (its nearest point), or out of time: next frame.
        const F32 group_radius = group->getBounds()[1].getLength3().getF32();
        const bool outside = mLoadRadius >= 0.f && entry.first - group_radius > mLoadRadius;
        if (outside || (mGroupRebuiltAny && mGroupRebuildSpent >= mGroupRebuildBudget))
        {
            waiting_inside += outside ? 0 : 1;
            later.push_back(group);     // keeps IN_BUILD_Q1
            continue;
        }
        LLTimer rebuild_timer;
        group->rebuildGeom();
        mGroupRebuildSpent += rebuild_timer.getElapsedTimeF32();
        mGroupRebuiltAny = true;
        ++mLoadBuiltInside;
        group->clearState(LLSpatialGroup::IN_BUILD_Q1);
    }

    if (mGroupRebuildBudget >= 0.f)
    {
        mLoadWaitingInside = waiting_inside;
    }

    mGroupSaveQ1 = mGroupQ1;
    mGroupQ1.swap(later);
    mGroupQ1Locked = false;

}

void LLPipeline::updateGeom(F32 max_dtime)
{
    LLTimer update_timer;
    LLPointer<LLDrawable> drawablep;

    LL_RECORD_BLOCK_TIME(FTM_GEO_UPDATE);
    if (gCubeSnapshot)
    {
        return;
    }

    assertInitialized();

    // notify various object types to reset internal cost metrics, etc.
    // for now, only LLVOVolume does this to throttle LOD changes
    LLVOVolume::preUpdateGeom();

    // Iterate through all drawables on the priority build queue,
    for (LLDrawable::drawable_list_t::iterator iter = mBuildQ1.begin();
         iter != mBuildQ1.end();)
    {
        LLDrawable::drawable_list_t::iterator curiter = iter++;
        LLDrawable* drawablep = *curiter;
        if (drawablep && !drawablep->isDead())
        {
            if (drawablep->isUnload())
            {
                drawablep->unload();
                drawablep->clearState(LLDrawable::FOR_UNLOAD);
            }

            if (updateDrawableGeom(drawablep))
            {
                drawablep->clearState(LLDrawable::IN_REBUILD_Q);
                mBuildQ1.erase(curiter);
            }
        }
        else
        {
            mBuildQ1.erase(curiter);
        }
    }

    updateMovedList(mMovedBridge);
}

void LLPipeline::markVisible(LLDrawable *drawablep, LLCamera& camera)
{
    if(drawablep && !drawablep->isDead())
    {
        if (drawablep->isSpatialBridge())
        {
            const LLDrawable* root = ((LLSpatialBridge*) drawablep)->mDrawable;
            llassert(root); // trying to catch a bad assumption

            if (root && //  // this test may not be needed, see above
                    root->getVObj()->isAttachment())
            {
                LLDrawable* rootparent = root->getParent();
                if (rootparent) // this IS sometimes NULL
                {
                    LLViewerObject *vobj = rootparent->getVObj();
                    llassert(vobj); // trying to catch a bad assumption
                    if (vobj) // this test may not be needed, see above
                    {
                        LLVOAvatar* av = vobj->asAvatar();
                        if (av &&
                            ((!sImpostorRender && av->isImpostor()) //ignore impostor flag during impostor pass
                             || av->isInMuteList()
                             || (LLVOAvatar::AOA_JELLYDOLL == av->getOverallAppearance() && !av->needsImpostorUpdate()) ))
                        {
                            return;
                        }
                    }
                }
            }
            sCull->pushBridge((LLSpatialBridge*) drawablep);
        }
        else
        {

            sCull->pushDrawable(drawablep);
        }

        drawablep->setVisible(camera);
    }
}

void LLPipeline::markMoved(LLDrawable *drawablep, bool damped_motion)
{
    if (!drawablep)
    {
        //LL_ERRS() << "Sending null drawable to moved list!" << LL_ENDL;
        return;
    }

    if (drawablep->isDead())
    {
        LL_WARNS() << "Marking NULL or dead drawable moved!" << LL_ENDL;
        return;
    }

    if (drawablep->getParent())
    {
        //ensure that parent drawables are moved first
        plus::activecensus::ViaChild via_child; // <plus> research/180 10.15: the parent goes on the list for its child (off: one flag test) </plus>
        markMoved(drawablep->getParent(), damped_motion);
    }

    assertInitialized();

    if (!drawablep->isState(LLDrawable::ON_MOVE_LIST))
    {
        if (drawablep->isSpatialBridge())
        {
            mMovedBridge.push_back(drawablep);
        }
        else
        {
            mMovedList.push_back(drawablep);
            plus::activecensus::marked(drawablep); // <plus> research/180 10.15: who put it on the list (off: one flag test) </plus>
        }
        drawablep->setState(LLDrawable::ON_MOVE_LIST);
    }
    if (! damped_motion)
    {
        drawablep->setState(LLDrawable::MOVE_UNDAMPED); // UNDAMPED trumps DAMPED
    }
    else if (drawablep->isState(LLDrawable::MOVE_UNDAMPED))
    {
        drawablep->clearState(LLDrawable::MOVE_UNDAMPED);
    }
}

void LLPipeline::markShift(LLDrawable *drawablep)
{
    if (!drawablep || drawablep->isDead() || !drawablep->getVObj())
    {
        return;
    }

    assertInitialized();

    if (!drawablep->isState(LLDrawable::ON_SHIFT_LIST))
    {
        drawablep->getVObj()->setChanged(LLXform::SHIFTED | LLXform::SILHOUETTE);
        if (drawablep->getParent())
        {
            markShift(drawablep->getParent());
        }
        mShiftList.push_back(drawablep);
        drawablep->setState(LLDrawable::ON_SHIFT_LIST);
    }
}

void LLPipeline::shiftObjects(const LLVector3 &offset)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    assertInitialized();

    LLRHI::clear(GL_DEPTH_BUFFER_BIT);     // <plus> research/207 6.3 </plus>
    gDepthDirty = true;

    LLVector4a offseta;
    offseta.load3(offset.mV);

    for (LLDrawable::drawable_vector_t::iterator iter = mShiftList.begin();
            iter != mShiftList.end(); iter++)
    {
        LLDrawable *drawablep = *iter;
        if (drawablep->isDead() || !drawablep->getVObj())
        {
            continue;
        }
        drawablep->shiftPos(offseta);
        drawablep->clearState(LLDrawable::ON_SHIFT_LIST);
    }
    mShiftList.resize(0);

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;
        for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
        {
            LLSpatialPartition* part = region->getSpatialPartition(i);
            if (part)
            {
                part->shift(offseta);
            }
        }
    }
    // <plus> plus/shadowreuse, research/188 (plus/cullinto/cullreuse): the octrees' bounds moved without an unbound
    plus::shadowreuse::noteShift();
    plus::cullreuse::noteShift();
    gLocalLightTable.shifted(); // research/203: the lights' positions are in the old agent frame
    // </plus>

    // LLRHI (fix-round22): the cached static shadow depth is in the old agent frame.
    for (ShadowCacheEntry& entry : mShadowCacheEntry)
    {
        entry.valid = false;
    }
    for (ShadowCacheEntry& entry : mProbeShadowCacheEntry)
    {
        entry.valid = false;
    }

    mReflectionMapManager.shift(offseta);

    LLHUDText::shiftAll(offset);
    LLHUDNameTag::shiftAll(offset);

    display_update_camera();
}

void LLPipeline::markTextured(LLDrawable *drawablep)
{
    if (drawablep && !drawablep->isDead() && assertInitialized())
    {
        mRetexturedList.insert(drawablep);
    }
}

void LLPipeline::markGLRebuild(LLGLUpdate* glu)
{
    if (glu && !glu->mInQ)
    {
        LLGLUpdate::sGLQ.push_back(glu);
        glu->mInQ = true;
    }
}

void LLPipeline::markPartitionMove(LLDrawable* drawable)
{
    if (!drawable->isState(LLDrawable::PARTITION_MOVE) &&
        !drawable->getPositionGroup().equals3(LLVector4a::getZero()))
    {
        drawable->setState(LLDrawable::PARTITION_MOVE);
        mPartitionQ.push_back(drawable);
    }
}

void LLPipeline::processPartitionQ()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    for (LLDrawable::drawable_list_t::iterator iter = mPartitionQ.begin(); iter != mPartitionQ.end(); ++iter)
    {
        LLDrawable* drawable = *iter;
        if (!drawable->isDead())
        {
            drawable->updateBinRadius();
            drawable->movePartition();
        }
        drawable->clearState(LLDrawable::PARTITION_MOVE);
    }

    mPartitionQ.clear();
}

void LLPipeline::markMeshDirty(LLSpatialGroup* group)
{
    mMeshDirtyGroup.push_back(group);
}

void LLPipeline::markRebuild(LLSpatialGroup* group)
{
    if (group && !group->isDead() && group->getSpatialPartition())
    {
        if (!group->hasState(LLSpatialGroup::IN_BUILD_Q1))
        {
            llassert_always(!mGroupQ1Locked);

            mGroupQ1.push_back(group);
            group->setState(LLSpatialGroup::IN_BUILD_Q1);
        }
    }
}

void LLPipeline::markRebuild(LLDrawable *drawablep, LLDrawable::EDrawableFlags flag)
{
    if (drawablep && !drawablep->isDead() && assertInitialized())
    {
        if (!drawablep->isState(LLDrawable::IN_REBUILD_Q))
        {
            mBuildQ1.push_back(drawablep);
            drawablep->setState(LLDrawable::IN_REBUILD_Q); // mark drawable as being in priority queue
        }

        if (flag & (LLDrawable::REBUILD_VOLUME | LLDrawable::REBUILD_POSITION))
        {
            drawablep->getVObj()->setChanged(LLXform::SILHOUETTE);
        }
        drawablep->setState(flag);
    }
}

void LLPipeline::stateSort(LLCamera& camera, LLCullResult &result)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("stateSort");

    //LLVertexBuffer::unbind();

    grabReferences(result);
    stateSortDrawableGroups(camera); // LLRHI: split in two
    stateSortVisible(camera);
}

// LLRHI: first half of stateSort. Marks what the drawable groups hold as
// visible (spatial bridges cull their own contents with this camera).
void LLPipeline::stateSortDrawableGroups(LLCamera& camera)
{
    for (LLCullResult::sg_iterator iter = sCull->beginDrawableGroups(); iter != sCull->endDrawableGroups(); ++iter)
    {
        LLSpatialGroup* group = *iter;
        if (group->isDead())
        {
            continue;
        }
        group->checkOcclusion();
        if (sUseOcclusion > 1 && group->isOcclusionState(LLSpatialGroup::OCCLUDED))
        {
            markOccluder(group);
        }
        else
        {
            group->setVisible();
            for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
            {
                LLDrawable* drawablep = (LLDrawable*)(*i)->getDrawable();
                plus::activecensus::CostScope spin_cost(drawablep, plus::activecensus::COST_CULL); // <plus> research/186 step 1 (off: one flag test) </plus>
                // <plus> plus/shadowbridge: in the sun cascades, a bridge's octree walked once per cascade set (research/178 C1b)
                if (plus::shadowbridge::markVisible(drawablep, camera, *sCull))
                {
                    continue;
                }
                // </plus>
                markVisible(drawablep, camera);
            }

            { //rebuild mesh as soon as we know it's visible
                group->rebuildMesh();
            }
        }
    }
}

// LLRHI: second half of stateSort: LOD, face pools, render maps.
void LLPipeline::stateSortVisible(LLCamera& camera)
{
    if (hasAnyRenderType(LLPipeline::RENDER_TYPE_AVATAR,
                      LLPipeline::RENDER_TYPE_CONTROL_AV,
                      LLPipeline::RENDER_TYPE_TERRAIN,
                      LLPipeline::RENDER_TYPE_TREE,
                      LLPipeline::RENDER_TYPE_SKY,
                      LLPipeline::RENDER_TYPE_VOIDWATER,
                      LLPipeline::RENDER_TYPE_WATER,
                      LLPipeline::END_RENDER_TYPES))
    {
        //clear faces from face pools
        gPipeline.resetDrawOrders();
    }

    if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot)
    {
        LLSpatialGroup* last_group = NULL;
        bool fov_changed = LLViewerCamera::getInstance()->isDefaultFOVChanged();
        for (LLCullResult::bridge_iterator i = sCull->beginVisibleBridge(); i != sCull->endVisibleBridge(); ++i)
        {
            LLCullResult::bridge_iterator cur_iter = i;
            LLSpatialBridge* bridge = *cur_iter;
            LLSpatialGroup* group = bridge->getSpatialGroup();

            if (last_group == NULL)
            {
                last_group = group;
            }

            if (!bridge->isDead() && group && !group->isOcclusionState(LLSpatialGroup::OCCLUDED))
            {
                stateSort(bridge, camera, fov_changed);
            }

            if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD &&
                last_group != group && last_group->changeLOD())
            {
                last_group->mLastUpdateDistance = last_group->mDistance;
            }

            last_group = group;
        }

        if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD &&
            last_group && last_group->changeLOD())
        {
            last_group->mLastUpdateDistance = last_group->mDistance;
        }
    }

    for (LLCullResult::sg_iterator iter = sCull->beginVisibleGroups(); iter != sCull->endVisibleGroups(); ++iter)
    {
        LLSpatialGroup* group = *iter;
        if (group->isDead())
        {
            continue;
        }
        group->checkOcclusion();
        if (sUseOcclusion > 1 && group->isOcclusionState(LLSpatialGroup::OCCLUDED))
        {
            markOccluder(group);
        }
        else
        {
            group->setVisible();
            stateSort(group, camera);

            { //rebuild mesh as soon as we know it's visible
                group->rebuildMesh();
            }
        }
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWABLE("stateSort"); // LL_RECORD_BLOCK_TIME(FTM_STATESORT_DRAWABLE);
        for (LLCullResult::drawable_iterator iter = sCull->beginVisibleList();
             iter != sCull->endVisibleList(); ++iter)
        {
            LLDrawable *drawablep = *iter;
            if (!drawablep->isDead())
            {
                stateSort(drawablep, camera);
            }
        }
    }

    postSort(camera);
}

void LLPipeline::stateSort(LLSpatialGroup* group, LLCamera& camera)
{
    if (group->changeLOD())
    {
        for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
        {
            LLDrawable* drawablep = (LLDrawable*)(*i)->getDrawable();
            stateSort(drawablep, camera);
        }

        if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot)
        { //avoid redundant stateSort calls
            group->mLastUpdateDistance = group->mDistance;
        }
    }
}

void LLPipeline::stateSort(LLSpatialBridge* bridge, LLCamera& camera, bool fov_changed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    plus::activecensus::CostScope spin_cost(bridge, plus::activecensus::COST_SORT); // <plus> research/186 step 1 (off: one flag test) </plus>
    if (bridge->getSpatialGroup()->changeLOD() || fov_changed)
    {
        bool force_update = false;
        plus::activecensus::lodUpdated(bridge); // <plus> research/186 step 1 (off: one flag test) </plus>
        bridge->updateDistance(camera, force_update);
    }
}

void LLPipeline::stateSort(LLDrawable* drawablep, LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (!drawablep
        || drawablep->isDead()
        || !hasRenderType(drawablep->getRenderType()))
    {
        return;
    }

    // SL-11353
    // ignore our own geo when rendering spotlight shadowmaps...
    //
    if (RenderSpotLight && drawablep == RenderSpotLight)
    {
        return;
    }

    if (LLSelectMgr::getInstance()->mHideSelectedObjects)
    {
        if (drawablep->getVObj().notNull() &&
            drawablep->getVObj()->isSelected())
        {
            return;
        }
    }

    if (drawablep->isAvatar())
    { //don't draw avatars beyond render distance or if we don't have a spatial group.
        if ((drawablep->getSpatialGroup() == NULL) ||
            (drawablep->getSpatialGroup()->mDistance > LLVOAvatar::sRenderDistance))
        {
            return;
        }

        LLVOAvatar* avatarp = (LLVOAvatar*) drawablep->getVObj().get();
        if (!avatarp->isVisible())
        {
            return;
        }
    }

    assertInitialized();

    if (hasRenderType(drawablep->mRenderType))
    {
        if (!drawablep->isState(LLDrawable::INVISIBLE|LLDrawable::FORCE_INVISIBLE))
        {
            drawablep->setVisible(camera, NULL, false);
        }
    }

    if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot)
    {
        //if (drawablep->isVisible()) isVisible() check here is redundant, if it wasn't visible, it wouldn't be here
        {
            if (!drawablep->isActive())
            {
                bool force_update = false;
                drawablep->updateDistance(camera, force_update);
            }
            else if (drawablep->isAvatar())
            {
                bool force_update = false;
                drawablep->updateDistance(camera, force_update); // calls vobj->updateLOD() which calls LLVOAvatar::updateVisibility()
            }
        }
    }

    if (!drawablep->getVOVolume())
    {
        for (LLDrawable::face_list_t::iterator iter = drawablep->mFaces.begin();
                iter != drawablep->mFaces.end(); iter++)
        {
            LLFace* facep = *iter;

            if (facep->hasGeometry())
            {
                if (facep->getPool())
                {
                    facep->getPool()->enqueue(facep);
                }
                else
                {
                    break;
                }
            }
        }
    }

    mNumVisibleFaces += drawablep->getNumFaces();
}


void forAllDrawables(LLCullResult::sg_iterator begin,
                     LLCullResult::sg_iterator end,
                     void (*func)(LLDrawable*))
{
    for (LLCullResult::sg_iterator i = begin; i != end; ++i)
    {
        LLSpatialGroup* group = *i;
        if (group->isDead())
        {
            continue;
        }
        for (LLSpatialGroup::element_iter j = group->getDataBegin(); j != group->getDataEnd(); ++j)
        {
            if((*j)->hasDrawable())
            {
                func((LLDrawable*)(*j)->getDrawable());
            }
        }
    }
}

void LLPipeline::forAllVisibleDrawables(void (*func)(LLDrawable*))
{
    forAllDrawables(sCull->beginDrawableGroups(), sCull->endDrawableGroups(), func);
    forAllDrawables(sCull->beginVisibleGroups(), sCull->endVisibleGroups(), func);
}

//function for creating scripted beacons
void renderScriptedBeacons(LLDrawable* drawablep)
{
    LLViewerObject *vobj = drawablep->getVObj();
    if (vobj
        && !vobj->isAvatar()
        && !vobj->getParent()
        && vobj->flagScripted())
    {
        if (gPipeline.sRenderBeacons)
        {
            gObjectList.addDebugBeacon(vobj->getPositionAgent(), "", LLColor4(1.f, 0.f, 0.f, 0.5f), LLColor4(1.f, 1.f, 1.f, 0.5f), LLPipeline::DebugBeaconLineWidth);
        }

        if (gPipeline.sRenderHighlight)
        {
            S32 face_id;
            S32 count = drawablep->getNumFaces();
            for (face_id = 0; face_id < count; face_id++)
            {
                LLFace * facep = drawablep->getFace(face_id);
                if (facep)
                {
                    gPipeline.mHighlightFaces.push_back(facep);
                }
            }
        }
    }
}

void renderScriptedTouchBeacons(LLDrawable *drawablep)
{
    LLViewerObject *vobj = drawablep->getVObj();
    if (vobj && !vobj->isAvatar() && !vobj->getParent() && vobj->flagScripted() && vobj->flagHandleTouch())
    {
        if (gPipeline.sRenderBeacons)
        {
            gObjectList.addDebugBeacon(vobj->getPositionAgent(), "", LLColor4(1.f, 0.f, 0.f, 0.5f), LLColor4(1.f, 1.f, 1.f, 0.5f),
                                       LLPipeline::DebugBeaconLineWidth);
        }

        if (gPipeline.sRenderHighlight)
        {
            S32 face_id;
            S32 count = drawablep->getNumFaces();
            for (face_id = 0; face_id < count; face_id++)
            {
                LLFace *facep = drawablep->getFace(face_id);
                if (facep)
                {
                    gPipeline.mHighlightFaces.push_back(facep);
                }
            }
        }
    }
}

void renderPhysicalBeacons(LLDrawable *drawablep)
{
    LLViewerObject *vobj = drawablep->getVObj();
    if (vobj &&
        !vobj->isAvatar()
        //&& !vobj->getParent()
        && vobj->flagUsePhysics())
    {
        if (gPipeline.sRenderBeacons)
        {
            gObjectList.addDebugBeacon(vobj->getPositionAgent(), "", LLColor4(0.f, 1.f, 0.f, 0.5f), LLColor4(1.f, 1.f, 1.f, 0.5f),
                                       LLPipeline::DebugBeaconLineWidth);
        }

        if (gPipeline.sRenderHighlight)
        {
            S32 face_id;
            S32 count = drawablep->getNumFaces();
            for (face_id = 0; face_id < count; face_id++)
            {
                LLFace *facep = drawablep->getFace(face_id);
                if (facep)
                {
                    gPipeline.mHighlightFaces.push_back(facep);
                }
            }
        }
    }
}

void renderMOAPBeacons(LLDrawable *drawablep)
{
    LLViewerObject *vobj = drawablep->getVObj();

    if (!vobj || vobj->isAvatar())
        return;

    bool beacon  = false;
    U8   tecount = vobj->getNumTEs();
    for (int x = 0; x < tecount; x++)
    {
        if (vobj->getTE(x)->hasMedia())
        {
            beacon = true;
            break;
        }
    }
    if (beacon)
    {
        if (gPipeline.sRenderBeacons)
        {
            gObjectList.addDebugBeacon(vobj->getPositionAgent(), "", LLColor4(1.f, 1.f, 1.f, 0.5f), LLColor4(1.f, 1.f, 1.f, 0.5f),
                                       LLPipeline::DebugBeaconLineWidth);
        }

        if (gPipeline.sRenderHighlight)
        {
            S32 face_id;
            S32 count = drawablep->getNumFaces();
            for (face_id = 0; face_id < count; face_id++)
            {
                LLFace *facep = drawablep->getFace(face_id);
                if (facep)
                {
                    gPipeline.mHighlightFaces.push_back(facep);
                }
            }
        }
    }
}

void renderParticleBeacons(LLDrawable *drawablep)
{
    // Look for attachments, objects, etc.
    LLViewerObject *vobj = drawablep->getVObj();
    if (vobj && vobj->isParticleSource())
    {
        if (gPipeline.sRenderBeacons)
        {
            LLColor4 light_blue(0.5f, 0.5f, 1.f, 0.5f);
            gObjectList.addDebugBeacon(vobj->getPositionAgent(), "", light_blue, LLColor4(1.f, 1.f, 1.f, 0.5f),
                                       LLPipeline::DebugBeaconLineWidth);
        }

        if (gPipeline.sRenderHighlight)
        {
            S32 face_id;
            S32 count = drawablep->getNumFaces();
            for (face_id = 0; face_id < count; face_id++)
            {
                LLFace *facep = drawablep->getFace(face_id);
                if (facep)
                {
                    gPipeline.mHighlightFaces.push_back(facep);
                }
            }
        }
    }
}

void renderSoundHighlights(LLDrawable *drawablep)
{
    // Look for attachments, objects, etc.
    LLViewerObject *vobj = drawablep->getVObj();
    if (vobj && vobj->isAudioSource())
    {
        if (gPipeline.sRenderHighlight)
        {
            S32 face_id;
            S32 count = drawablep->getNumFaces();
            for (face_id = 0; face_id < count; face_id++)
            {
                LLFace *facep = drawablep->getFace(face_id);
                if (facep)
                {
                    gPipeline.mHighlightFaces.push_back(facep);
                }
            }
        }
    }
}

namespace
{
    // LLRHI: the four side planes of a reflection probe face's view, in agent space
    // as LLCamera keeps them (a point is outside where the distance is > 0). The GPU
    // clips x and y against exactly these; near, far and the water clip plane are
    // left out, so nothing a draw could put on the face is lost.
    struct ProbeFaceView
    {
        F32 plane[4][4] = {};
        F32 origin[3] = {};

        void set(LLCamera& camera)
        {
            const U32 sides[4] = { LLCamera::AGENT_PLANE_LEFT, LLCamera::AGENT_PLANE_RIGHT,
                                   LLCamera::AGENT_PLANE_BOTTOM, LLCamera::AGENT_PLANE_TOP };
            for (U32 i = 0; i < 4; ++i)
            {
                const LLPlane& p = camera.getAgentPlane(sides[i]);
                for (U32 j = 0; j < 4; ++j)
                {
                    plane[i][j] = p[j];
                }
            }
            const LLVector3& o = camera.getOrigin();
            for (U32 j = 0; j < 3; ++j)
            {
                origin[j] = o.mV[j];
            }
        }

        // True when the box lies entirely outside one side plane, by more than a
        // margin far above the planes' float error (1 cm plus 1 mm per metre).
        bool outside(const LLVector4a& lo, const LLVector4a& hi) const
        {
            const F32* l = lo.getF32ptr();
            const F32* h = hi.getF32ptr();
            F32 c[3], r[3];
            F32 reach = 0.f;
            for (U32 j = 0; j < 3; ++j)
            {
                c[j] = 0.5f * (l[j] + h[j]);
                r[j] = 0.5f * (h[j] - l[j]);
                reach += fabsf(c[j] - origin[j]) + fabsf(r[j]);
            }
            const F32 margin = 0.01f + 0.001f * reach;
            for (U32 i = 0; i < 4; ++i)
            {
                const F32* p = plane[i];
                const F32 nearest = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + p[3] -
                                    fabsf(p[0] * r[0]) - fabsf(p[1] * r[1]) - fabsf(p[2] * r[2]);
                if (nearest > margin)
                {
                    return true;
                }
            }
            return false;
        }
    };

    struct ProbeCullStats
    {
        U32 faces = 0;
        U64 batches = 0;    // batches of the visible groups
        U64 tested = 0;     // static ones with bounds
        U64 skipped = 0;    // entirely outside the face
        F32 logged_at = 0.f;
    };
    ProbeCullStats sProbeCullStats;

    // LLRHI: postSort's render map for one range of the visible groups, built on a
    // cull thread (research/48 C4). A range writes only these lists and the probe
    // stamps of its own groups' batches; the main thread appends the ranges to
    // sCull in order afterwards.
    struct PostSortRange
    {
        std::vector<std::pair<U32, LLDrawInfo*> > infos;    // (render type, batch) in the order postSort pushes them
        std::vector<LLSpatialGroup*> alpha;                 // groups with PASS_ALPHA batches
        std::vector<LLSpatialGroup*> rigged_alpha;          // groups with PASS_ALPHA_RIGGED batches
        U32 indices = 0;                                    // for addTrianglesDrawn
        U64 batches = 0;                                    // sProbeCullStats
        U64 tested = 0;
        U64 skipped = 0;
        // Batches in infos per GPU probe list type (LLPipeline::rhiProbeTypeIndex),
        // and the groups the list draws: their type, with its batches before them.
        U32 listed[LLPipeline::RHI_PROBE_MAX_TYPES] = {};
        struct ProbeAt
        {
            LLSpatialGroup* group;
            U32 type;
            U32 before;
        };
        std::vector<ProbeAt> probe_at;
        // With the probe's static G-buffer cache: for each PASS_SIMPLE entry of
        // infos in order, its group, or null when the prefix may not take it.
        std::vector<LLSpatialGroup*> simple_groups;

        void clear()
        {
            infos.clear();
            alpha.clear();
            rigged_alpha.clear();
            simple_groups.clear();
            indices = 0;
            batches = tested = skipped = 0;
            memset(listed, 0, sizeof(listed));
            probe_at.clear();
        }
    };

    // Runs job(0) .. job(count - 1) on the cull threads and this one, and returns when all are done.
    void run_cull_jobs(U32 count, const std::function<void(U32)>& job);

    // LLRHI: a group whose batches the probe's static G-buffer cache may take
    // (research/44 P5): static (a region's volume partition, not a bridge's) and
    // its batches match its vertex buffers (not waiting for a rebuild, nor for
    // the in-place one at the end of postSort). Only reads the group.
    bool probe_static_group(LLSpatialGroup* group)
    {
        LLSpatialPartition* part = group->getSpatialPartition();
        return part && part->mPartitionType == LLViewerRegion::PARTITION_VOLUME && !part->asBridge() &&
               !group->hasState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::MESH_DIRTY);
    }
}

void LLPipeline::postSort(LLCamera &camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    assertInitialized();

    LL_PUSH_CALLSTACKS();

    if (!gCubeSnapshot)
    {
        // rebuild drawable geometry
        for (LLCullResult::sg_iterator i = sCull->beginDrawableGroups(); i != sCull->endDrawableGroups(); ++i)
        {
            LLSpatialGroup *group = *i;
            if (group->isDead())
            {
                continue;
            }
            if (!sUseOcclusion || !group->isOcclusionState(LLSpatialGroup::OCCLUDED))
            {
                if (mGroupRebuildBudget >= 0.f && waits_for_build_time(group))
                {
                    // Built nearest first by rebuildPriorityGroups below, within the frame's time.
                    markRebuild(group);
                }
                // <plus> research/295: counted when the periodic logs are on
                else if (notesRebuildCauses())
                {
                    rebuild_group_noting_cause(group, RC_DRAWABLE_GROUPS, mRebuildCauses);
                }
                // </plus>
                else
                {
                    group->rebuildGeom();
                }
            }
        }
        LL_PUSH_CALLSTACKS();
        // rebuild groups
        sCull->assertDrawMapsEmpty();

        rebuildPriorityGroups();
    }

    LL_PUSH_CALLSTACKS();

    // <plus> research/197 stage 2 (research/198 3.3): the retained scene's view from this cull (main, probe
    // face, mirror, water mirror; none for shadows and HUDs, or with the scene off): its visible resident groups below.
    // research/209 S7: a probe face or mirror the scene compares this time is sorted as LL sorts it (no batch
    // cull of the face, no probe list, no static prefix), so its render map is what the choice is compared with.
    const U32 scene_view = gSceneRecords.beginView(camera);
    const bool scene_compares = gSceneRecords.comparesView(scene_view);
    // </plus>

    // LLRHI: a reflection probe face leaves out the static batches that lie
    // entirely outside its view: they would not draw a single pixel there.
    sProbeCullSerial = 0;
    ProbeFaceView face_view;
    if (sProbeFaceRender && gCubeSnapshot && !sShadowRender && LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD &&
        LLRHI::sProbeFast && LLRHI::sGLDrawsDisabled && !scene_compares) // <plus> research/209 S7: !scene_compares </plus>
    {
        static U32 serial = 0;
        serial = serial + 1 ? serial + 1 : 1;
        sProbeCullSerial = serial;
        face_view.set(camera);

        ProbeCullStats& stats = sProbeCullStats;
        if (gFrameTimeSeconds - stats.logged_at >= 10.f)
        {
            if (stats.faces)
            {
                LL_INFOS("RHIProbe") << "probe face culling over " << stats.faces << " faces: batches " << stats.batches
                                     << ", with bounds " << stats.tested << ", outside the face " << stats.skipped << " ("
                                     << (stats.batches ? (U32)(stats.skipped * 100 / stats.batches) : 0) << "%)" << LL_ENDL;
            }
            stats = ProbeCullStats();
            stats.logged_at = gFrameTimeSeconds;
        }
        ++stats.faces;
    }

    // LLRHI: the face's static batches of PASS_SIMPLE (and with generated
    // commands, the other opaque G-buffer types) may go to the GPU probe list
    // (research/46 G5, research/50 step 4): then the render map leaves them
    // out (rhiProbeSkip, mRHIProbeTypes) and they are drawn in their places.
    // LLRHI: the realtime probe's static G-buffer cache takes the face's
    // PASS_SIMPLE prefix out of the render map (research/44 P5); the GPU probe
    // list leaves PASS_SIMPLE to it on such a face.
    // <plus> research/209 S3 (E2): never the static prefix and the retained scene's PASS_SIMPLE on
    // one face. The prefix is drawn by the simple pools on a cull result of its own
    // (probeStaticDraw), whose pushBatches reaches the scene's drawPass(PASS_SIMPLE) a second
    // time: with the pass cut it would draw the cut groups twice, once into the cache; and
    // drawPass draws the cut groups' records whatever the render map holds. So a face that
    // takes the prefix (a copy from the cache is cheaper than the scene's draws) keeps
    // PASS_SIMPLE out of the scene's cut (keepPass, before any group is marked), and drawPass
    // then has nothing of the pass in this view. Not on a face the scene compares (S7).
    const bool probe_static = !scene_compares && probeStaticBegin(camera);
    if (probe_static)
    {
        gSceneRecords.keepPass(scene_view, LLRenderPass::PASS_SIMPLE);
    }
    // </plus>
    rhiProbeFace(face_view.plane, face_view.origin, sProbeCullSerial, probe_static ? rhiProbeTypeBit(LLRenderPass::PASS_SIMPLE) : 0);
    const U32 probe_skip = rhiProbeSkip();

    // LLRHI: with the cull threads, the render map is built per range of groups on
    // them (research/48 C4). The rebuilds the loop below makes are made here first,
    // in its order, and the ranges' lists are appended to sCull in order, so the
    // render map and the alpha lists come out the same. LLRHI_CULL_THREADS=0, few
    // groups, or mPostSortGroupsOnly keep that loop.
    const bool threaded_render_map = LLRHI::sCullThreads && !mPostSortGroupsOnly && sCull->getVisibleGroupsSize() >= 128;
    if (threaded_render_map)
    {
        static std::vector<LLSpatialGroup*> groups;
        static std::vector<PostSortRange> ranges;
        static std::vector<U8> scene_drawn; // <plus> research/198 3.3: per group, the retained scene draws its target batches </plus>

        // 1. Here: the groups the loop below draws, rebuilt where it rebuilds them.
        groups.clear();
        scene_drawn.clear(); // <plus> research/198 3.3 </plus>
        for (LLCullResult::sg_iterator i = sCull->beginVisibleGroups(); i != sCull->endVisibleGroups(); ++i)
        {
            LLSpatialGroup *group = *i;
            if (sCull->endVisibleGroups() - i > 8)
            {
                _mm_prefetch((const char*)i[8], _MM_HINT_T0);
            }

            if (group->isDead())
            {
                continue;
            }

            if ((sUseOcclusion && group->isOcclusionState(LLSpatialGroup::OCCLUDED)) ||
                (RenderAutoHideSurfaceAreaLimit > 0.f &&
                 group->mSurfaceArea > RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
            {
                continue;
            }

            if (group->hasState(LLSpatialGroup::NEW_DRAWINFO) && group->hasState(LLSpatialGroup::GEOM_DIRTY) && !gCubeSnapshot)
            {  // no way this group is going to be drawable without a rebuild
                group->rebuildGeom();
            }

            if (sShadowRender && group->mRHIListKind != 2)
            {
                rhiListTrack(group); // LLRHI (fix-round22): as in the loop below (GPU shadow list, G2)
            }

            groups.push_back(group);
            // <plus> research/198 3.3 (1); research/206 5b: or a spot light shadow's group the scene draws (2: its batches cut)
            const bool scene_mark = gSceneRecords.markVisible(scene_view, group);
            scene_drawn.push_back((U8)(scene_mark ? 1 : gSceneShadow.spotOpen() && gSceneShadow.markSpotGroup(group) ? 2 : 0));
            // </plus>
        }

        // 2. On the cull threads: each range's batches. Reads the groups and their
        //    batches; writes only its own PostSortRange and its batches' probe stamps.
        const U32 group_count = (U32)groups.size();
        const U32 range_count = llclamp(group_count / 48u, 1u, 64u);
        if (ranges.size() < range_count)
        {
            ranges.resize(range_count);
        }
        const bool alpha_types = hasRenderType(LLPipeline::RENDER_TYPE_PASS_ALPHA);
        const bool count_triangles = !sShadowRender && !sReflectionRender && !gCubeSnapshot;
        const U32 probe_serial = sProbeCullSerial;
        run_cull_jobs(range_count, [&](U32 r)
        {
            PostSortRange& out = ranges[r];
            out.clear();
            const U32 end = (U32)((U64)group_count * (r + 1) / range_count);
            for (U32 g = (U32)((U64)group_count * r / range_count); g < end; ++g)
            {
                LLSpatialGroup* group = groups[g];
                const bool scene_group = scene_drawn[g] == 1; // <plus> research/198 3.3 </plus>
                const bool spot_group = scene_drawn[g] == 2;  // <plus> research/206 5b </plus>

                // LLRHI: static groups whose batches match their vertex buffers (not waiting for a rebuild).
                const bool face_cull = probe_serial &&
                                       group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_VOLUME &&
                                       !group->getSpatialPartition()->asBridge() &&
                                       !group->hasState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::MESH_DIRTY);
                LLSpatialGroup* static_group = probe_static && probe_static_group(group) ? group : nullptr;

                // Whether the group has alpha batches, seen on the way (the loop below looks them up).
                bool has_alpha = false;
                bool has_rigged_alpha = false;

                for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
                {
                    const U32 type = j->first;
                    has_alpha = has_alpha || type == LLRenderPass::PASS_ALPHA;
                    has_rigged_alpha = has_rigged_alpha || type == LLRenderPass::PASS_ALPHA_RIGGED;
                    if (!hasRenderType(type))
                    {
                        continue;
                    }
                    // <plus> research/198 3.3: the retained scene draws them at the pass's entry
                    if (scene_group && gSceneRecords.cutsPass(scene_view, type)) // research/198 5.5: the passes the scene draws in this view
                    {
                        continue;
                    }
                    if (spot_group && gSceneShadow.cutType(type)) // research/206 5b: the spot pass's scene draws them (drawNativeShadowCasters)
                    {
                        continue;
                    }
                    // </plus>
                    // LLRHI: the GPU probe list's types (research/46 G5, research/50 step 4).
                    const S32 probe_type = probe_skip ? rhiProbeTypeIndex(type) : -1;
                    if (probe_type >= 0 && group->mRHIProbeStamp == probe_skip && (group->mRHIProbeTypes & (1u << probe_type)))
                    {
                        out.probe_at.push_back({ group, type, out.listed[probe_type] });
                        continue;   // LLRHI: the GPU probe list draws them here (rhiProbeDraw)
                    }

                    LLSpatialGroup::drawmap_elem_t &src_vec = j->second;
                    for (LLSpatialGroup::drawmap_elem_t::iterator k = src_vec.begin(); k != src_vec.end(); ++k)
                    {
                        LLDrawInfo *info = *k;

                        if (probe_serial)
                        {
                            ++out.batches;
                            if (face_cull && info->mHasExtents && info->mModelMatrix && info->mAvatar.isNull())
                            {
                                // Bounds in region space; the region's matrix moves with region shifts.
                                LLVector4a origin, lo, hi;
                                origin.load3(info->mModelMatrix->mMatrix[3]);
                                lo.setAdd(info->mExtents[0], origin);
                                hi.setAdd(info->mExtents[1], origin);
                                ++out.tested;
                                if (face_view.outside(lo, hi))
                                {
                                    // Also for the alpha pool, which reads the group's batches directly.
                                    info->mProbeCullStamp = probe_serial;
                                    ++out.skipped;
                                    continue;
                                }
                            }
                        }

                        out.infos.emplace_back(type, info);
                        if (probe_static && type == LLRenderPass::PASS_SIMPLE)
                        {
                            out.simple_groups.push_back(static_group);
                        }
                        if (probe_type >= 0)
                        {
                            ++out.listed[probe_type];
                        }
                        if (count_triangles)
                        {
                            out.indices += info->mCount;
                        }
                    }
                }

                if (alpha_types)
                {
                    if (has_alpha)
                    {
                        out.alpha.push_back(group);
                    }
                    if (has_rigged_alpha)
                    {
                        out.rigged_alpha.push_back(group);
                    }
                }
            }
        });

        // 3. Here, in range order: the render map, the alpha groups and their
        //    distances (updateDistance may queue a rebuild, so it stays here).
        const bool update_alpha_distance = LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot;
        const bool alpha_pool = hasRenderType(LLDrawPool::POOL_ALPHA);
        for (U32 r = 0; r < range_count; ++r)
        {
            const PostSortRange& out = ranges[r];
            for (const PostSortRange::ProbeAt& at : out.probe_at)
            {
                rhiProbeAt(at.group, at.type, sCull->getRenderMapSize(at.type) + at.before);
            }
            size_t simple_entry = 0;
            for (const std::pair<U32, LLDrawInfo*>& entry : out.infos)
            {
                if (probe_static && entry.first == LLRenderPass::PASS_SIMPLE)
                {
                    LLSpatialGroup* group = simple_entry < out.simple_groups.size() ? out.simple_groups[simple_entry] : nullptr;
                    ++simple_entry;
                    if (probeStaticTake(group, entry.second))
                    {
                        continue;   // LLRHI: in the face's static prefix (probeStaticDraw)
                    }
                }
                sCull->pushDrawInfo(entry.first, entry.second);
            }
            if (out.indices)
            {
                addTrianglesDrawn((S32)out.indices);
            }
            if (probe_serial)
            {
                sProbeCullStats.batches += out.batches;
                sProbeCullStats.tested += out.tested;
                sProbeCullStats.skipped += out.skipped;
            }

            for (LLSpatialGroup* group : out.alpha)
            {  // store alpha groups for sorting
                if (update_alpha_distance)
                {
                    LLSpatialBridge *bridge = group->getSpatialPartition()->asBridge();
                    if (bridge)
                    {
                        LLCamera trans_camera = bridge->transformCamera(camera);
                        group->updateDistance(trans_camera);
                    }
                    else
                    {
                        group->updateDistance(camera);
                    }
                }

                if (alpha_pool)
                {
                    sCull->pushAlphaGroup(group);
                }
            }

            if (alpha_pool)
            {  // store rigged alpha groups for LLDrawPoolAlpha prepass
                for (LLSpatialGroup* group : out.rigged_alpha)
                {
                    sCull->pushRiggedAlphaGroup(group);
                }
            }
        }
    }

    // build render map (the old path; the threaded one above has built it already)
    for (LLCullResult::sg_iterator i = threaded_render_map ? sCull->endVisibleGroups() : sCull->beginVisibleGroups();
         i != sCull->endVisibleGroups(); ++i)
    {
        LLSpatialGroup *group = *i;

        if (group->isDead())
        {
            continue;
        }

        if ((sUseOcclusion && group->isOcclusionState(LLSpatialGroup::OCCLUDED)) ||
            (RenderAutoHideSurfaceAreaLimit > 0.f &&
             group->mSurfaceArea > RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
        {
            continue;
        }

        if (group->hasState(LLSpatialGroup::NEW_DRAWINFO) && group->hasState(LLSpatialGroup::GEOM_DIRTY) && !gCubeSnapshot)
        {  // no way this group is going to be drawable without a rebuild
            group->rebuildGeom();
        }

        if (sShadowRender && group->mRHIListKind != 2)
        {
            rhiListTrack(group); // LLRHI: registers its opaque casters in the GPU shadow list (G2)
        }

        if (mPostSortGroupsOnly)
        {
            // LLRHI: nobody reads this render map or these alpha groups (renderSunShadowCascades).
            continue;
        }
        const bool scene_group = gSceneRecords.markVisible(scene_view, group); // <plus> research/198 3.3 </plus>
        const bool spot_group = !scene_group && gSceneShadow.spotOpen() && gSceneShadow.markSpotGroup(group); // <plus> research/206 5b: cut </plus>

        // LLRHI: static groups whose batches match their vertex buffers (not waiting for a rebuild).
        const bool face_cull = sProbeCullSerial &&
                               group->getSpatialPartition()->mPartitionType == LLViewerRegion::PARTITION_VOLUME &&
                               !group->getSpatialPartition()->asBridge() &&
                               !group->hasState(LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::MESH_DIRTY);
        LLSpatialGroup* static_group = probe_static && probe_static_group(group) ? group : nullptr;

        for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
        {
            LLSpatialGroup::drawmap_elem_t &src_vec = j->second;
            if (!hasRenderType(j->first))
            {
                continue;
            }
            // <plus> research/198 3.3: the retained scene draws them at the pass's entry
            if (scene_group && gSceneRecords.cutsPass(scene_view, j->first)) // research/198 5.5: the passes the scene draws in this view
            {
                continue;
            }
            if (spot_group && gSceneShadow.cutType(j->first)) // research/206 5b: the spot pass's scene draws them (drawNativeShadowCasters)
            {
                continue;
            }
            // </plus>
            if (probe_skip && group->mRHIProbeStamp == probe_skip && (group->mRHIProbeTypes & rhiProbeTypeBit(j->first)))
            {
                rhiProbeAt(group, j->first, sCull->getRenderMapSize(j->first));
                continue;   // LLRHI: the GPU probe list draws them here (rhiProbeDraw)
            }

            for (LLSpatialGroup::drawmap_elem_t::iterator k = src_vec.begin(); k != src_vec.end(); ++k)
            {
                LLDrawInfo *info = *k;

                if (sProbeCullSerial)
                {
                    ++sProbeCullStats.batches;
                    if (face_cull && info->mHasExtents && info->mModelMatrix && info->mAvatar.isNull())
                    {
                        // Bounds in region space; the region's matrix moves with region shifts.
                        LLVector4a origin, lo, hi;
                        origin.load3(info->mModelMatrix->mMatrix[3]);
                        lo.setAdd(info->mExtents[0], origin);
                        hi.setAdd(info->mExtents[1], origin);
                        ++sProbeCullStats.tested;
                        if (face_view.outside(lo, hi))
                        {
                            // Also for the alpha pool, which reads the group's batches directly.
                            info->mProbeCullStamp = sProbeCullSerial;
                            ++sProbeCullStats.skipped;
                            continue;
                        }
                    }
                }

                if (probe_static && j->first == LLRenderPass::PASS_SIMPLE && probeStaticTake(static_group, info))
                {
                    continue;   // LLRHI: in the face's static prefix (probeStaticDraw)
                }
                sCull->pushDrawInfo(j->first, info);
                if (!sShadowRender && !sReflectionRender && !gCubeSnapshot)
                {
                    addTrianglesDrawn(info->mCount);
                }
            }
        }

        if (hasRenderType(LLPipeline::RENDER_TYPE_PASS_ALPHA))
        {
            LLSpatialGroup::draw_map_t::iterator alpha = group->mDrawMap.find(LLRenderPass::PASS_ALPHA);

            if (alpha != group->mDrawMap.end())
            {  // store alpha groups for sorting
                LLSpatialBridge *bridge = group->getSpatialPartition()->asBridge();
                if (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot)
                {
                    if (bridge)
                    {
                        LLCamera trans_camera = bridge->transformCamera(camera);
                        group->updateDistance(trans_camera);
                    }
                    else
                    {
                        group->updateDistance(camera);
                    }
                }

                if (hasRenderType(LLDrawPool::POOL_ALPHA))
                {
                    sCull->pushAlphaGroup(group);
                }
            }

            LLSpatialGroup::draw_map_t::iterator rigged_alpha = group->mDrawMap.find(LLRenderPass::PASS_ALPHA_RIGGED);

            if (rigged_alpha != group->mDrawMap.end())
            {  // store rigged alpha groups for LLDrawPoolAlpha prepass (skip distance update, rigged attachments use depth buffer)
                if (hasRenderType(LLDrawPool::POOL_ALPHA))
                {
                    sCull->pushRiggedAlphaGroup(group);
                }
            }
        }
    }

    // <plus> research/204 10 (vk-s4c2): the world pools' face lists of this view (stateSort made them;
    // the drawable groups were rebuilt above) to the scene, before endView hands it the view
    if (gSceneRecords.worldWanted(scene_view))
    {
        for (const auto& pool : mTerrainPools)
        {
            gSceneRecords.markWorldPool(scene_view, static_cast<LLFacePool*>(pool.second));
        }
        for (const auto& pool : mTreePools)
        {
            gSceneRecords.markWorldPool(scene_view, static_cast<LLFacePool*>(pool.second));
        }
        if (mWaterPool)
        {
            gSceneRecords.markWorldPool(scene_view, static_cast<LLFacePool*>(mWaterPool));
        }
    }
    // </plus>
    gSceneRecords.endView(scene_view); // <plus> research/198 3.3: the view and its visible groups to the scene </plus>
    plus::retained::census(camera, sCull); // <plus> research/195 S0: the main view's groups and batches counted (LLRHI_RETAINED_CENSUS) </plus>

    /*bool use_transform_feedback = gTransformPositionProgram.mProgramObject && !mMeshDirtyGroup.empty();

    if (use_transform_feedback)
    { //place a query around potential transform feedback code for synchronization
        mTransformFeedbackPrimitives = 0;

        if (!mMeshDirtyQueryObject)
        {
            glGenQueries(1, &mMeshDirtyQueryObject);
        }


        glBeginQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, mMeshDirtyQueryObject);
    }*/

    // pack vertex buffers for groups that chose to delay their updates
    {
        LL_PROFILE_GPU_ZONE("rebuildMesh");
        for (LLSpatialGroup::sg_vector_t::iterator iter = mMeshDirtyGroup.begin(); iter != mMeshDirtyGroup.end(); ++iter)
        {
            (*iter)->rebuildMesh();
        }
    }

    /*if (use_transform_feedback)
    {
        glEndQuery(GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    }*/

    mMeshDirtyGroup.clear();

    if (!sShadowRender)
    {
        // order alpha groups by distance
        std::sort(sCull->beginAlphaGroups(), sCull->endAlphaGroups(), LLSpatialGroup::CompareDepthGreater());

        // order rigged alpha groups by avatar attachment order
        std::sort(sCull->beginRiggedAlphaGroups(), sCull->endRiggedAlphaGroups(), LLSpatialGroup::CompareRenderOrder());
    }

    LL_PUSH_CALLSTACKS();
    // only render if the flag is set. The flag is only set if we are in edit mode or the toggle is set in the menus
    if (LLFloaterReg::instanceVisible("beacons") && !sShadowRender && !gCubeSnapshot)
    {
        if (sRenderScriptedTouchBeacons)
        {
            // Only show the beacon on the root object.
            forAllVisibleDrawables(renderScriptedTouchBeacons);
        }
        else if (sRenderScriptedBeacons)
        {
            // Only show the beacon on the root object.
            forAllVisibleDrawables(renderScriptedBeacons);
        }

        if (sRenderPhysicalBeacons)
        {
            // Only show the beacon on the root object.
            forAllVisibleDrawables(renderPhysicalBeacons);
        }

        if (sRenderMOAPBeacons)
        {
            forAllVisibleDrawables(renderMOAPBeacons);
        }

        if (sRenderParticleBeacons)
        {
            forAllVisibleDrawables(renderParticleBeacons);
        }

        // If god mode, also show audio cues
        if (sRenderSoundBeacons && gAudiop)
        {
            // Walk all sound sources and render out beacons for them. Note, this isn't done in the ForAllVisibleDrawables function, because
            // some are not visible.
            LLAudioEngine::source_map::iterator iter;
            for (iter = gAudiop->mAllSources.begin(); iter != gAudiop->mAllSources.end(); ++iter)
            {
                LLAudioSource *sourcep = iter->second;

                LLVector3d pos_global = sourcep->getPositionGlobal();
                LLVector3  pos        = gAgent.getPosAgentFromGlobal(pos_global);
                if (gPipeline.sRenderBeacons)
                {
                    // pos += LLVector3(0.f, 0.f, 0.2f);
                    gObjectList.addDebugBeacon(pos, "", LLColor4(1.f, 1.f, 0.f, 0.5f), LLColor4(1.f, 1.f, 1.f, 0.5f), DebugBeaconLineWidth);
                }
            }
            // now deal with highlights for all those seeable sound sources
            forAllVisibleDrawables(renderSoundHighlights);
        }
    }
    LL_PUSH_CALLSTACKS();
    // If managing your telehub, draw beacons at telehub and currently selected spawnpoint.
    if (LLFloaterTelehub::renderBeacons() && !sShadowRender && !gCubeSnapshot)
    {
        LLFloaterTelehub::addBeacons();
    }

    if (!sShadowRender && !gCubeSnapshot)
    {
        mSelectedFaces.clear();

        bool tex_index_changed = false;
        if (!gNonInteractive)
        {
            LLRender::eTexIndex tex_index = sRenderHighlightTextureChannel;
            setRenderHighlightTextureChannel(gFloaterTools->getPanelFace()->getTextureChannelToEdit());
            tex_index_changed = sRenderHighlightTextureChannel != tex_index;
        }

        // Draw face highlights for selected faces.
        if (LLSelectMgr::getInstance()->getTEMode())
        {
            struct f : public LLSelectedTEFunctor
            {
                virtual bool apply(LLViewerObject *object, S32 te)
                {
                    if (object->mDrawable)
                    {
                        LLFace *facep = object->mDrawable->getFace(te);
                        if (facep)
                        {
                            gPipeline.mSelectedFaces.push_back(facep);
                        }
                    }
                    return true;
                }
            } func;
            LLSelectMgr::getInstance()->getSelection()->applyToTEs(&func);

            if (tex_index_changed)
            {
                // Rebuild geometry for all selected faces with PBR textures
                for (const LLFace* face : gPipeline.mSelectedFaces)
                {
                    if (const LLViewerObject* vobj = face->getViewerObject())
                    {
                        if (const LLTextureEntry* tep = vobj->getTE(face->getTEOffset()))
                        {
                            if (tep->getGLTFRenderMaterial())
                            {
                                gPipeline.markRebuild(face->getDrawable(), LLDrawable::REBUILD_VOLUME);
                            }
                        }
                    }
                }
            }
        }
    }

    LLVertexBuffer::flushBuffers();
    // LLSpatialGroup::sNoDelete = false;
    LL_PUSH_CALLSTACKS();
}


void render_hud_elements()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI; //LL_RECORD_BLOCK_TIME(FTM_RENDER_UI);
    gPipeline.disableLights();

    LLGLSUIDefault gls_ui;

    //LLGLEnable stencil(GL_STENCIL_TEST);
    //glStencilFunc(GL_ALWAYS, 255, 0xFFFFFFFF);
    //glStencilMask(0xFFFFFFFF);
    //glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    gUIProgram.bind();
    gGL.color4f(1, 1, 1, 1);
    LLGLDepthTest depth(GL_TRUE, GL_FALSE);

    if (!LLPipeline::sReflectionRender && gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
    {
        gViewerWindow->renderSelections(false, false, false); // For HUD version in render_ui_3d()

        // Draw the tracking overlays
        LLTracker::render3D();

        if (LLWorld::instanceExists())
        {
            // Show the property lines
            LLWorld::getInstance()->renderPropertyLines();
        }
        LLViewerParcelMgr::getInstance()->render();
        LLViewerParcelMgr::getInstance()->renderParcelCollision();
    }
    else if (gForceRenderLandFence)
    {
        // This is only set when not rendering the UI, for parcel snapshots
        LLViewerParcelMgr::getInstance()->render();
    }
    else if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD))
    {
        LLHUDText::renderAllHUD();
    }

    gUIProgram.unbind();
}

static inline void bindHighlightProgram(LLGLSLShader& program)
{
    if ((LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_INTERFACE) > 0))
    {
        program.bind();
        gGL.diffuseColor4f(1, 1, 1, 0.5f);
    }
}

static inline void unbindHighlightProgram(LLGLSLShader& program)
{
    if (LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_INTERFACE) > 0)
    {
        program.unbind();
    }
}

void LLPipeline::renderSelectedFaces(const LLColor4& color)
{
    if (!mFaceSelectImagep)
    {
        mFaceSelectImagep = LLViewerTextureManager::getFetchedTexture(IMG_FACE_SELECT);
    }

    if (mFaceSelectImagep)
    {
        // Make sure the selection image gets downloaded and decoded
        mFaceSelectImagep->addTextureStats((F32)MAX_IMAGE_AREA);

        for (auto facep : mSelectedFaces)
        {
            if (!facep || !facep->getViewerObject())
            {
                LLSelectMgr::getInstance()->clearSelections();
                return;
            }
            if (!facep->getDrawable() || facep->getDrawable()->isDead())
            {
                LL_ERRS() << "Bad face on selection" << LL_ENDL;
                return;
            }

            facep->renderSelected(mFaceSelectImagep, color);
        }
    }
}

void LLPipeline::renderHighlights()
{
    assertInitialized();

    // Draw 3D UI elements here (before we clear the Z buffer in POOL_HUD)
    // Render highlighted faces.
    LLGLSPipelineAlpha gls_pipeline_alpha;
    disableLights();

    if (hasRenderDebugFeatureMask(RENDER_DEBUG_FEATURE_SELECTED))
    {
        bindHighlightProgram(gHighlightProgram);

        if (sRenderHighlightTextureChannel == LLRender::DIFFUSE_MAP ||
            sRenderHighlightTextureChannel == LLRender::BASECOLOR_MAP ||
            sRenderHighlightTextureChannel == LLRender::METALLIC_ROUGHNESS_MAP ||
            sRenderHighlightTextureChannel == LLRender::GLTF_NORMAL_MAP ||
            sRenderHighlightTextureChannel == LLRender::EMISSIVE_MAP ||
            sRenderHighlightTextureChannel == LLRender::NUM_TEXTURE_CHANNELS)
        {
            static const LLColor4 highlight_selected_color(1.f, 1.f, 1.f, 0.5f);
            renderSelectedFaces(highlight_selected_color);
        }

        // Paint 'em red!
        static const LLColor4 highlight_face_color(1.f, 0.f, 0.f, 0.5f);
        for (auto facep : mHighlightFaces)
        {
            facep->renderSelected(LLViewerTexture::sNullImagep, highlight_face_color);
        }

        unbindHighlightProgram(gHighlightProgram);
    }

    // Contains a list of the faces of objects that are physical or
    // have touch-handlers.
    mHighlightFaces.clear();

    if (hasRenderDebugFeatureMask(RENDER_DEBUG_FEATURE_SELECTED))
    {
        if (sRenderHighlightTextureChannel == LLRender::NORMAL_MAP)
        {
            static const LLColor4 highlight_normal_color(1.0f, 0.5f, 0.5f, 0.5f);
            bindHighlightProgram(gHighlightNormalProgram);
            renderSelectedFaces(highlight_normal_color);
            unbindHighlightProgram(gHighlightNormalProgram);
        }
        else if (sRenderHighlightTextureChannel == LLRender::SPECULAR_MAP)
        {
            static const LLColor4 highlight_specular_color(0.0f, 0.3f, 1.0f, 0.8f);
            bindHighlightProgram(gHighlightSpecularProgram);
            renderSelectedFaces(highlight_specular_color);
            unbindHighlightProgram(gHighlightSpecularProgram);
        }
    }
}

//debug use
U32 LLPipeline::sCurRenderPoolType = 0 ;

void LLPipeline::renderGeomDeferred(LLCamera& camera, bool do_occlusion)
{
    LLAppViewer::instance()->pingMainloopTimeout("Pipeline:RenderGeomDeferred");
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL; //LL_RECORD_BLOCK_TIME(FTM_RENDER_GEOMETRY);
    LL_PROFILE_GPU_ZONE("renderGeomDeferred");
    LLRHI::gpuMark("gbuffer");
    // <plus> skin mark (plus/skinsss): the main view's G-buffer pass leaves the skin stencil bit on skin
    plus::skinsss::GBufferScope skin_mark(camera);
    // </plus>

    llassert(!sRenderingHUDs);

    if (gUseWireframe)
    {
        LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    if (&camera == LLViewerCamera::getInstance())
    {   // a bit hacky, this is the start of the main render frame, figure out delta between last modelview matrix and
        // current modelview matrix
        glm::mat4 last_modelview = get_last_modelview();
        glm::mat4 cur_modelview = get_current_modelview();

        // goal is to have a matrix here that goes from the last frame's camera space to the current frame's camera space
        glm::mat4 m = glm::inverse(last_modelview);  // last camera space to world space
        m = cur_modelview * m; // world space to camera space

        glm::mat4 n = glm::inverse(m);

        gGLDeltaModelView = m;
        gGLInverseDeltaModelView = n;
        // <plus> slotpublish auto (research/180 8): the world is drawn after the login, so its count may start
        LLRHI::sWorldShown = LLRHI::sWorldShown || LLStartUp::getStartupState() >= STATE_STARTED;
        // </plus>
    }

    bool occlude = LLPipeline::sUseOcclusion > 1 && do_occlusion && !LLGLSLShader::sProfileEnabled;

    setupHWLights();

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("deferred pools");

        LLGLEnable cull(GL_CULL_FACE);

        for (pool_set_t::iterator iter = mPools.begin(); iter != mPools.end(); ++iter)
        {
            LLDrawPool *poolp = *iter;
            if (hasRenderType(poolp->getType()))
            {
                poolp->prerender();
            }
        }

        LLVertexBuffer::unbind();

        LLGLState::checkStates();

        if (LLViewerShaderMgr::instance()->mShaderLevel[LLViewerShaderMgr::SHADER_DEFERRED] > 1)
        {
            //update reflection probe uniform
            mReflectionMapManager.updateUniforms();
            mHeroProbeManager.updateUniforms();
        }

        U32 cur_type = 0;

        gGL.setColorMask(true, true);

        pool_set_t::iterator iter1 = mPools.begin();

        while ( iter1 != mPools.end() )
        {
            LLDrawPool *poolp = *iter1;

            cur_type = poolp->getType();

            if (mProbeStatic.collecting && cur_type > LLDrawPool::POOL_WL_SKY)
            {
                // LLRHI: a realtime probe face's static PASS_SIMPLE prefix, cached or
                // drawn, comes first after the sky (research/44 P5).
                probeStaticDraw();
            }

            if (occlude && cur_type >= LLDrawPool::POOL_GRASS)
            {
                llassert(!gCubeSnapshot); // never do occlusion culling on cube snapshots
                occlude = false;
                gGLLastMatrix = NULL;
                gGL.loadMatrix(gGLModelView);
                LLRHI::gpuMark("occlusion");
                doOcclusion(camera);
            }

            pool_set_t::iterator iter2 = iter1;
            if (hasRenderType(poolp->getType()) && poolp->getNumDeferredPasses() > 0)
            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("deferred pool render");
                LLRHI::gpuMark(gpu_pool_name(cur_type, false));

                gGLLastMatrix = NULL;
                gGL.loadMatrix(gGLModelView);

                for( S32 i = 0; i < poolp->getNumDeferredPasses(); i++ )
                {
                    LLVertexBuffer::unbind();
                    poolp->beginDeferredPass(i);
                    for (iter2 = iter1; iter2 != mPools.end(); iter2++)
                    {
                        LLDrawPool *p = *iter2;
                        if (p->getType() != cur_type)
                        {
                            break;
                        }

                        if ( !p->getSkipRenderFlag() ) { p->renderDeferred(i); }
                    }
                    poolp->endDeferredPass(i);
                    LLVertexBuffer::unbind();

                    LLGLState::checkStates();
                }
            }
            else
            {
                // Skip all pools of this type
                for (iter2 = iter1; iter2 != mPools.end(); iter2++)
                {
                    LLDrawPool *p = *iter2;
                    if (p->getType() != cur_type)
                    {
                        break;
                    }
                }
            }
            iter1 = iter2;
            stop_glerror();
        }

        if (mProbeStatic.collecting)
        {
            probeStaticDraw();  // LLRHI: no pool after the sky (nothing of the prefix draws then either)
        }

        plus::derez::render(); // <plus> derez: deleted objects' ghosts crumbling, the main view only (research/257) </plus>

        gGLLastMatrix = NULL;
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.loadMatrix(gGLModelView);

        gGL.setColorMask(true, false);

    } // Tracy ZoneScoped

    if (gUseWireframe)
    {
        LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

// Render all of our geometry that's required after our deferred pass.
// This is gonna be stuff like alpha, water, etc.
void LLPipeline::renderGeomPostDeferred(LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    LL_PROFILE_GPU_ZONE("renderGeomPostDeferred");

    if (gUseWireframe)
    {
        LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }

    U32 cur_type = 0;

    LLGLEnable cull(GL_CULL_FACE);

    bool done_atmospherics = LLPipeline::sRenderingHUDs; //skip atmospherics on huds
    bool done_water_haze = done_atmospherics;
    bool done_water_exclusion = false;

    // do water exclusion just before water pass.
    U32 water_exclusion_pass = LLDrawPool::POOL_WATEREXCLUSION;

    // do atmospheric haze just before post water alpha
    U32 atmospherics_pass = LLDrawPool::POOL_ALPHA_POST_WATER;

    if (LLPipeline::sUnderWaterRender)
    { // if under water, do atmospherics just before the water pass
        atmospherics_pass = LLDrawPool::POOL_WATER;
    }

    // do water haze just before pre water alpha
    U32 water_haze_pass = LLDrawPool::POOL_ALPHA_PRE_WATER;

    calcNearbyLights(camera);
    setupHWLights();

    gGL.setSceneBlendType(LLRender::BT_ALPHA);
    gGL.setColorMask(true, false);

    pool_set_t::iterator iter1 = mPools.begin();

    if (gDebugGL || gDebugPipeline)
    {
        LLGLState::checkStates(GL_FALSE);
    }

    // turn off atmospherics and water haze for low detail reflection probe
    static LLCachedControl<S32> probe_level(gSavedSettings, "RenderReflectionProbeLevel", 0);
    bool low_detail_probe = probe_level == 0 && gCubeSnapshot;
    done_atmospherics = done_atmospherics || low_detail_probe;
    done_water_haze   = done_water_haze || low_detail_probe;


    while ( iter1 != mPools.end() )
    {
        LLDrawPool *poolp = *iter1;

        cur_type = poolp->getType();

        if (cur_type >= water_exclusion_pass && !done_water_exclusion)
        { // do water exclusion against depth buffer before rendering alpha
            LLRHI::gpuMark("water");
            doWaterExclusionMask();
            done_water_exclusion = true;
        }

        if (cur_type >= atmospherics_pass && !done_atmospherics)
        { // do atmospherics against depth buffer before rendering alpha
            LLRHI::gpuMark("haze");
            doAtmospherics();
            done_atmospherics = true;
        }

        if (cur_type >= water_haze_pass && !done_water_haze)
        { // do water haze against depth buffer before rendering alpha
            LLRHI::gpuMark("haze");
            doWaterHaze();
            done_water_haze = true;
        }

        pool_set_t::iterator iter2 = iter1;
        if (hasRenderType(poolp->getType()) && poolp->getNumPostDeferredPasses() > 0)
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DRAWPOOL("deferred poolrender");
            LLRHI::gpuMark(gpu_pool_name(cur_type, true));

            gGLLastMatrix = NULL;
            gGL.loadMatrix(gGLModelView);

            for( S32 i = 0; i < poolp->getNumPostDeferredPasses(); i++ )
            {
                LLVertexBuffer::unbind();
                poolp->beginPostDeferredPass(i);
                for (iter2 = iter1; iter2 != mPools.end(); iter2++)
                {
                    LLDrawPool *p = *iter2;
                    if (p->getType() != cur_type)
                    {
                        break;
                    }

                    p->renderPostDeferred(i);
                }
                poolp->endPostDeferredPass(i);
                LLVertexBuffer::unbind();

                if (gDebugGL || gDebugPipeline)
                {
                    LLGLState::checkStates(GL_FALSE);
                }
            }
        }
        else
        {
            // Skip all pools of this type
            for (iter2 = iter1; iter2 != mPools.end(); iter2++)
            {
                LLDrawPool *p = *iter2;
                if (p->getType() != cur_type)
                {
                    break;
                }
            }
        }
        iter1 = iter2;
        stop_glerror();
    }

    gGLLastMatrix = NULL;
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.loadMatrix(gGLModelView);

    if (!gCubeSnapshot)
    {
        // GPU pass timing: highlights, debug displays and what else comes before the next mark.
        LLRHI::gpuMark(sRenderingHUDs ? "hud" : "after alpha");
        // debug displays
        renderHighlights();
        mHighlightFaces.clear();

        renderDebug();
    }

    if (gUseWireframe)
    {
        LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
}

void LLPipeline::renderGeomShadow(LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("renderGeomShadow");
    U32 cur_type = 0;

    LLGLEnable cull(GL_CULL_FACE);

    LLVertexBuffer::unbind();

    pool_set_t::iterator iter1 = mPools.begin();

    while ( iter1 != mPools.end() )
    {
        LLDrawPool *poolp = *iter1;

        cur_type = poolp->getType();

        pool_set_t::iterator iter2 = iter1;
        if (hasRenderType(poolp->getType()) && poolp->getNumShadowPasses() > 0)
        {
            poolp->prerender() ;

            gGLLastMatrix = NULL;
            gGL.loadMatrix(gGLModelView);

            for( S32 i = 0; i < poolp->getNumShadowPasses(); i++ )
            {
                LLVertexBuffer::unbind();
                poolp->beginShadowPass(i);
                for (iter2 = iter1; iter2 != mPools.end(); iter2++)
                {
                    LLDrawPool *p = *iter2;
                    if (p->getType() != cur_type)
                    {
                        break;
                    }

                    p->renderShadow(i);
                }
                poolp->endShadowPass(i);
                LLVertexBuffer::unbind();
            }
        }
        else
        {
            // Skip all pools of this type
            for (iter2 = iter1; iter2 != mPools.end(); iter2++)
            {
                LLDrawPool *p = *iter2;
                if (p->getType() != cur_type)
                {
                    break;
                }
            }
        }
        iter1 = iter2;
        stop_glerror();
    }

    gGLLastMatrix = NULL;
    gGL.loadMatrix(gGLModelView);
}


static U32 sIndicesDrawnCount = 0;

void LLPipeline::addTrianglesDrawn(S32 index_count)
{
    sIndicesDrawnCount += index_count;
}

void LLPipeline::recordTrianglesDrawn()
{
    assertInitialized();
    U32 count = sIndicesDrawnCount / 3;
    sIndicesDrawnCount = 0;
    add(LLStatViewer::TRIANGLES_DRAWN, LLUnits::Triangles::fromValue(count));
}

void LLPipeline::renderPhysicsDisplay()
{
    if (!hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES))
    {
        return;
    }

    gGL.flush();
    gDebugProgram.bind();

    LLGLEnable polygon_offset_line(GL_POLYGON_OFFSET_LINE);
    LLRHI::polygonOffset(3.f, 3.f);     // <plus> research/207 6.3 </plus>
    gGL.setLineWidth(3.f);
    LLGLEnable blend(GL_BLEND);
    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    for (int pass = 0; pass < 3; ++pass)
    {
        // pass 0 - depth write enabled, color write disabled, fill
        // pass 1 - depth write disabled, color write enabled, fill
        // pass 2 - depth write disabled, color write enabled, wireframe
        gGL.setColorMask(pass >= 1, false);
        LLGLDepthTest depth(GL_TRUE, pass == 0);

        bool wireframe = (pass == 2);

        if (wireframe)
        {
            LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_LINE);     // <plus> research/207 6.3 </plus>
        }

        for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
        {
            LLViewerRegion* region = *iter;
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (part)
                {
                    if (hasRenderType(part->mDrawableType))
                    {
                        part->renderPhysicsShapes(wireframe);
                    }
                }
            }
        }
        gGL.flush();

        if (wireframe)
        {
            LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_FILL);     // <plus> research/207 6.3 </plus>
        }
    }
    gGL.setLineWidth(1.f);
    gDebugProgram.unbind();

}

extern std::set<LLSpatialGroup*> visible_selected_groups;

void LLPipeline::renderDebug()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    assertInitialized();

    bool hud_only = hasRenderType(LLPipeline::RENDER_TYPE_HUD);

    if (!hud_only )
    {
        //Render any navmesh geometry
        LLPathingLib *llPathingLibInstance = LLPathingLib::getInstance();
        if ( llPathingLibInstance != NULL )
        {
            //character floater renderables

            LLHandle<LLFloaterPathfindingCharacters> pathfindingCharacterHandle = LLFloaterPathfindingCharacters::getInstanceHandle();
            if ( !pathfindingCharacterHandle.isDead() )
            {
                LLFloaterPathfindingCharacters *pathfindingCharacter = pathfindingCharacterHandle.get();

                if ( pathfindingCharacter->getVisible() || gAgentCamera.cameraMouselook() )
                {
                    gPathfindingProgram.bind();
                    gPathfindingProgram.uniform1f(sTint, 1.f);
                    gPathfindingProgram.uniform1f(sAmbiance, 1.f);
                    gPathfindingProgram.uniform1f(sAlphaScale, 1.f);

                    //Requried character physics capsule render parameters
                    LLUUID id;
                    LLVector3 pos;
                    LLQuaternion rot;

                    if ( pathfindingCharacter->isPhysicsCapsuleEnabled( id, pos, rot ) )
                    {
                        //remove blending artifacts
                        gGL.setColorMask(false, false);
                        llPathingLibInstance->renderSimpleShapeCapsuleID( gGL, id, pos, rot );
                        gGL.setColorMask(true, false);
                        LLGLEnable blend(GL_BLEND);
                        gPathfindingProgram.uniform1f(sAlphaScale, 0.90f);
                        llPathingLibInstance->renderSimpleShapeCapsuleID( gGL, id, pos, rot );
                        gPathfindingProgram.bind();
                    }
                }
            }


            //pathing console renderables
            LLHandle<LLFloaterPathfindingConsole> pathfindingConsoleHandle = LLFloaterPathfindingConsole::getInstanceHandle();
            if (!pathfindingConsoleHandle.isDead())
            {
                LLFloaterPathfindingConsole *pathfindingConsole = pathfindingConsoleHandle.get();

                if ( pathfindingConsole->getVisible() || gAgentCamera.cameraMouselook() )
                {
                    F32 ambiance = gSavedSettings.getF32("PathfindingAmbiance");

                    gPathfindingProgram.bind();

                    gPathfindingProgram.uniform1f(sTint, 1.f);
                    gPathfindingProgram.uniform1f(sAmbiance, ambiance);
                    gPathfindingProgram.uniform1f(sAlphaScale, 1.f);

                    if ( !pathfindingConsole->isRenderWorld() )
                    {
                        const LLColor4 clearColor = gSavedSettings.getColor4("PathfindingNavMeshClear");
                        gGL.setColorMask(true, true);
                        LLRHI::clearColor(clearColor.mV[0],clearColor.mV[1],clearColor.mV[2],0);     // <plus> research/207 6.3 </plus>
                        LLRHI::clear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT); // no stencil -- deprecated | GL_STENCIL_BUFFER_BIT);     // <plus> research/207 6.3 </plus>
                        gGL.setColorMask(true, false);
                        LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_FILL );     // <plus> research/207 6.3 </plus>
                    }

                    //NavMesh
                    if ( pathfindingConsole->isRenderNavMesh() )
                    {
                        gGL.setLineWidth(2.0f);
                        LLGLEnable cull(GL_CULL_FACE);
                        LLGLDisable blend(GL_BLEND);

                        if ( pathfindingConsole->isRenderWorld() )
                        {
                            LLGLEnable blend(GL_BLEND);
                            gPathfindingProgram.uniform1f(sAlphaScale, 0.66f);
                            llPathingLibInstance->renderNavMesh();
                        }
                        else
                        {
                            llPathingLibInstance->renderNavMesh();
                        }

                        //render edges
                        gPathfindingNoNormalsProgram.bind();
                        gPathfindingNoNormalsProgram.uniform1f(sTint, 1.f);
                        gPathfindingNoNormalsProgram.uniform1f(sAlphaScale, 1.f);
                        llPathingLibInstance->renderNavMeshEdges();
                        gPathfindingProgram.bind();

                        gGL.flush();
                        LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_FILL );     // <plus> research/207 6.3 </plus>
                        gGL.setLineWidth(1.0f);
                        gGL.flush();
                    }
                    //User designated path
                    if ( LLPathfindingPathTool::getInstance()->isRenderPath() )
                    {
                        //The path
                        gUIProgram.bind();
                        gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep);
                        llPathingLibInstance->renderPath();
                        gPathfindingProgram.bind();

                        //The bookends
                        //remove blending artifacts
                        gGL.setColorMask(false, false);
                        llPathingLibInstance->renderPathBookend( gGL, LLPathingLib::LLPL_START );
                        llPathingLibInstance->renderPathBookend( gGL, LLPathingLib::LLPL_END );

                        gGL.setColorMask(true, false);
                        //render the bookends
                        LLGLEnable blend(GL_BLEND);
                        gPathfindingProgram.uniform1f(sAlphaScale, 0.90f);
                        llPathingLibInstance->renderPathBookend( gGL, LLPathingLib::LLPL_START );
                        llPathingLibInstance->renderPathBookend( gGL, LLPathingLib::LLPL_END );
                        gPathfindingProgram.bind();
                    }

                    if ( pathfindingConsole->isRenderWaterPlane() )
                    {
                        LLGLEnable blend(GL_BLEND);
                        gPathfindingProgram.uniform1f(sAlphaScale, 0.90f);
                        llPathingLibInstance->renderSimpleShapes( gGL, gAgent.getRegion()->getWaterHeight() );
                    }
                //physics/exclusion shapes
                if ( pathfindingConsole->isRenderAnyShapes() )
                {
                        U32 render_order[] = {
                            1 << LLPathingLib::LLST_ObstacleObjects,
                            1 << LLPathingLib::LLST_WalkableObjects,
                            1 << LLPathingLib::LLST_ExclusionPhantoms,
                            1 << LLPathingLib::LLST_MaterialPhantoms,
                        };

                        U32 flags = pathfindingConsole->getRenderShapeFlags();

                        for (U32 i = 0; i < 4; i++)
                        {
                            if (!(flags & render_order[i]))
                            {
                                continue;
                            }

                            //turn off backface culling for volumes so they are visible when camera is inside volume
                            LLGLDisable cull(i >= 2 ? GL_CULL_FACE : 0);

                            gGL.flush();
                            LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_FILL );     // <plus> research/207 6.3 </plus>

                            //get rid of some z-fighting
                            LLGLEnable polyOffset(GL_POLYGON_OFFSET_FILL);
                            LLRHI::polygonOffset(1.0f, 1.0f);     // <plus> research/207 6.3 </plus>

                            //render to depth first to avoid blending artifacts
                            gGL.setColorMask(false, false);
                            llPathingLibInstance->renderNavMeshShapesVBO( render_order[i] );
                            gGL.setColorMask(true, false);

                            //get rid of some z-fighting
                            LLRHI::polygonOffset(0.f, 0.f);     // <plus> research/207 6.3 </plus>

                            LLGLEnable blend(GL_BLEND);

                            {
                                gPathfindingProgram.uniform1f(sAmbiance, ambiance);

                                { //draw solid overlay
                                    LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_LEQUAL);
                                    llPathingLibInstance->renderNavMeshShapesVBO( render_order[i] );
                                    gGL.flush();
                                }

                                LLGLEnable lineOffset(GL_POLYGON_OFFSET_LINE);
                                LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_LINE );     // <plus> research/207 6.3 </plus>

                                F32 offset = gSavedSettings.getF32("PathfindingLineOffset");

                                if (pathfindingConsole->isRenderXRay())
                                {
                                    gPathfindingProgram.uniform1f(sTint, gSavedSettings.getF32("PathfindingXRayTint"));
                                    gPathfindingProgram.uniform1f(sAlphaScale, gSavedSettings.getF32("PathfindingXRayOpacity"));
                                    LLGLEnable blend(GL_BLEND);
                                    LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_GREATER);

                                    LLRHI::polygonOffset(offset, -offset);     // <plus> research/207 6.3 </plus>

                                    if (gSavedSettings.getBOOL("PathfindingXRayWireframe"))
                                    { //draw hidden wireframe as darker and less opaque
                                        gPathfindingProgram.uniform1f(sAmbiance, 1.f);
                                        llPathingLibInstance->renderNavMeshShapesVBO( render_order[i] );
                                    }
                                    else
                                    {
                                        LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_FILL );     // <plus> research/207 6.3 </plus>
                                        gPathfindingProgram.uniform1f(sAmbiance, ambiance);
                                        llPathingLibInstance->renderNavMeshShapesVBO( render_order[i] );
                                        LLRHI::polygonMode(GL_FRONT_AND_BACK, GL_LINE);     // <plus> research/207 6.3 </plus>
                                    }
                                }

                                { //draw visible wireframe as brighter, thicker and more opaque
                                    LLRHI::polygonOffset(offset, offset);     // <plus> research/207 6.3 </plus>
                                    gPathfindingProgram.uniform1f(sAmbiance, 1.f);
                                    gPathfindingProgram.uniform1f(sTint, 1.f);
                                    gPathfindingProgram.uniform1f(sAlphaScale, 1.f);

                                    gGL.setLineWidth(gSavedSettings.getF32("PathfindingLineWidth"));
                                    LLGLDisable blendOut(GL_BLEND);
                                    llPathingLibInstance->renderNavMeshShapesVBO( render_order[i] );
                                    gGL.flush();
                                    gGL.setLineWidth(1.f);
                                }

                                LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_FILL );     // <plus> research/207 6.3 </plus>
                            }
                        }
                    }

                    LLRHI::polygonOffset(0.f, 0.f);     // <plus> research/207 6.3 </plus>

                    if ( pathfindingConsole->isRenderNavMesh() && pathfindingConsole->isRenderXRay() )
                    {   //render navmesh xray
                        F32 ambiance = gSavedSettings.getF32("PathfindingAmbiance");

                        LLGLEnable lineOffset(GL_POLYGON_OFFSET_LINE);
                        LLGLEnable polyOffset(GL_POLYGON_OFFSET_FILL);

                        F32 offset = gSavedSettings.getF32("PathfindingLineOffset");
                        LLRHI::polygonOffset(offset, -offset);     // <plus> research/207 6.3 </plus>

                        LLGLEnable blend(GL_BLEND);
                        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_GREATER);
                        gGL.flush();
                        gGL.setLineWidth(2.0f);
                        LLGLEnable cull(GL_CULL_FACE);

                        gPathfindingProgram.uniform1f(sTint, gSavedSettings.getF32("PathfindingXRayTint"));
                        gPathfindingProgram.uniform1f(sAlphaScale, gSavedSettings.getF32("PathfindingXRayOpacity"));

                        if (gSavedSettings.getBOOL("PathfindingXRayWireframe"))
                        { //draw hidden wireframe as darker and less opaque
                            LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_LINE );     // <plus> research/207 6.3 </plus>
                            gPathfindingProgram.uniform1f(sAmbiance, 1.f);
                            llPathingLibInstance->renderNavMesh();
                            LLRHI::polygonMode( GL_FRONT_AND_BACK, GL_FILL );     // <plus> research/207 6.3 </plus>
                        }
                        else
                        {
                            gPathfindingProgram.uniform1f(sAmbiance, ambiance);
                            llPathingLibInstance->renderNavMesh();
                        }

                        //render edges
                        gPathfindingNoNormalsProgram.bind();
                        gPathfindingNoNormalsProgram.uniform1f(sTint, gSavedSettings.getF32("PathfindingXRayTint"));
                        gPathfindingNoNormalsProgram.uniform1f(sAlphaScale, gSavedSettings.getF32("PathfindingXRayOpacity"));
                        llPathingLibInstance->renderNavMeshEdges();
                        gPathfindingProgram.bind();

                        gGL.flush();
                        gGL.setLineWidth(1.0f);
                    }

                    LLRHI::polygonOffset(0.f, 0.f);     // <plus> research/207 6.3 </plus>

                    gGL.flush();
                    gPathfindingProgram.unbind();
                }
            }
        }
    }

    gGLLastMatrix = NULL;
    gGL.loadMatrix(gGLModelView);
    gGL.setColorMask(true, false);


    if (!hud_only && !mDebugBlips.empty())
    { //render debug blips
        gUIProgram.bind();
        gGL.color4f(1, 1, 1, 1);

        gGL.getTexUnit(0)->bind(LLViewerFetchedTexture::sWhiteImagep, true);

        // <plus> research/207 6.3: glPointSize gone (Vulkan has no point size state; the backend's answer did nothing) </plus>
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

        gGL.begin(LLRender::POINTS);
        for (std::list<DebugBlip>::iterator iter = mDebugBlips.begin(); iter != mDebugBlips.end(); )
        {
            DebugBlip& blip = *iter;

            blip.mAge += gFrameIntervalSeconds.value();
            if (blip.mAge > 2.f)
            {
                mDebugBlips.erase(iter++);
            }
            else
            {
                iter++;
            }

            blip.mPosition.mV[2] += gFrameIntervalSeconds.value()*2.f;

            gGL.color4fv(blip.mColor.mV);
            gGL.vertex3fv(blip.mPosition.mV);
        }
        gGL.end();
        gGL.flush();
        // <plus> research/207 6.3: glPointSize gone (Vulkan has no point size state; the backend's answer did nothing) </plus>
    }

    // Debug stuff.
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_OCTREE |
        LLPipeline::RENDER_DEBUG_OCCLUSION |
        LLPipeline::RENDER_DEBUG_LIGHTS |
        LLPipeline::RENDER_DEBUG_BATCH_SIZE |
        LLPipeline::RENDER_DEBUG_UPDATE_TYPE |
        LLPipeline::RENDER_DEBUG_BBOXES |
        LLPipeline::RENDER_DEBUG_NORMALS |
        LLPipeline::RENDER_DEBUG_POINTS |
        LLPipeline::RENDER_DEBUG_TEXTURE_AREA |
        LLPipeline::RENDER_DEBUG_TEXTURE_ANIM |
        LLPipeline::RENDER_DEBUG_RAYCAST |
        LLPipeline::RENDER_DEBUG_AVATAR_VOLUME |
        LLPipeline::RENDER_DEBUG_AVATAR_JOINTS |
        LLPipeline::RENDER_DEBUG_AGENT_TARGET |
        LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA |
        LLPipeline::RENDER_DEBUG_TEXEL_DENSITY))
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("render debug bridges");

        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (part)
                {
                    if ((hud_only && (part->mDrawableType == RENDER_TYPE_HUD || part->mDrawableType == RENDER_TYPE_HUD_PARTICLES)) ||
                        (!hud_only && hasRenderType(part->mDrawableType)))
                    {
                        part->renderDebug();
                    }
                }
            }
        }

        for (LLCullResult::bridge_iterator i = sCull->beginVisibleBridge(); i != sCull->endVisibleBridge(); ++i)
        {
            LLSpatialBridge* bridge = *i;
            if (!bridge->isDead() && hasRenderType(bridge->mDrawableType))
            {
                gGL.pushMatrix();
                gGL.multMatrix((F32*)bridge->mDrawable->getRenderMatrix().mMatrix);
                bridge->renderDebug();
                gGL.popMatrix();
            }
        }
    }

    LL::GLTFSceneManager::instance().renderDebug();

    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_OCCLUSION))
    { //render visible selected group occlusion geometry
        gDebugProgram.bind();
        LLGLDepthTest depth(GL_TRUE, GL_FALSE);
        gGL.diffuseColor3f(1,0,1);
        for (std::set<LLSpatialGroup*>::iterator iter = visible_selected_groups.begin(); iter != visible_selected_groups.end(); ++iter)
        {
            LLSpatialGroup* group = *iter;

            LLVector4a fudge;
            fudge.splat(0.25f); //SG_OCCLUSION_FUDGE

            LLVector4a size;
            const LLVector4a* bounds = group->getBounds();
            size.setAdd(fudge, bounds[1]);

            drawBox(bounds[0], size);
        }
    }

    visible_selected_groups.clear();

    //draw reflection probes and links between them
    if (gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_REFLECTION_PROBES) && !hud_only)
    {
        mReflectionMapManager.renderDebug();
    }

    static LLCachedControl<bool> render_ref_probe_volumes(gSavedSettings, "RenderReflectionProbeVolumes");
    if (render_ref_probe_volumes && !hud_only)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("probe debug display");

        bindDeferredShader(gReflectionProbeDisplayProgram, NULL);
        mScreenTriangleVB->setBuffer();

        LLGLEnable blend(GL_BLEND);
        LLGLDepthTest depth(GL_FALSE);

        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        unbindDeferredShader(gReflectionProbeDisplayProgram);
    }

    gUIProgram.bind();

    if (hasRenderDebugMask(LLPipeline::RENDER_DEBUG_RAYCAST) && !hud_only)
    { //draw crosshairs on particle intersection
        if (gDebugRaycastParticle)
        {
            gDebugProgram.bind();

            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

            LLVector3 center(gDebugRaycastParticleIntersection.getF32ptr());
            LLVector3 size(0.1f, 0.1f, 0.1f);

            LLVector3 p[6];

            p[0] = center + size.scaledVec(LLVector3(1,0,0));
            p[1] = center + size.scaledVec(LLVector3(-1,0,0));
            p[2] = center + size.scaledVec(LLVector3(0,1,0));
            p[3] = center + size.scaledVec(LLVector3(0,-1,0));
            p[4] = center + size.scaledVec(LLVector3(0,0,1));
            p[5] = center + size.scaledVec(LLVector3(0,0,-1));

            gGL.begin(LLRender::LINES);
            gGL.diffuseColor3f(1.f, 1.f, 0.f);
            for (U32 i = 0; i < 6; i++)
            {
                gGL.vertex3fv(p[i].mV);
            }
            gGL.end();
            gGL.flush();

            gDebugProgram.unbind();
        }
    }

    if (hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !hud_only)
    {
        LLVertexBuffer::unbind();

        LLGLEnable blend(GL_BLEND);
        LLGLDepthTest depth(true, false);
        LLGLDisable cull(GL_CULL_FACE);

        gGL.color4f(1,1,1,1);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        F32 a = 0.1f;

        F32 col[] =
        {
            1,0,0,a,
            0,1,0,a,
            0,0,1,a,
            1,0,1,a,

            1,1,0,a,
            0,1,1,a,
            1,1,1,a,
            1,0,1,a,
        };

        for (U32 i = 0; i < 8; i++)
        {
            LLVector3* frust = mShadowCamera[i].mAgentFrustum;

            if (i > 3)
            { //render shadow frusta as volumes
                if (mShadowFrustPoints[i-4].empty())
                {
                    continue;
                }

                gGL.color4fv(col+(i-4)*4);

                gGL.begin(LLRender::TRIANGLE_STRIP);
                gGL.vertex3fv(frust[0].mV); gGL.vertex3fv(frust[4].mV);
                gGL.vertex3fv(frust[1].mV); gGL.vertex3fv(frust[5].mV);
                gGL.vertex3fv(frust[2].mV); gGL.vertex3fv(frust[6].mV);
                gGL.vertex3fv(frust[3].mV); gGL.vertex3fv(frust[7].mV);
                gGL.vertex3fv(frust[0].mV); gGL.vertex3fv(frust[4].mV);
                gGL.end();


                gGL.begin(LLRender::TRIANGLE_STRIP);
                gGL.vertex3fv(frust[0].mV);
                gGL.vertex3fv(frust[1].mV);
                gGL.vertex3fv(frust[3].mV);
                gGL.vertex3fv(frust[2].mV);
                gGL.end();

                gGL.begin(LLRender::TRIANGLE_STRIP);
                gGL.vertex3fv(frust[4].mV);
                gGL.vertex3fv(frust[5].mV);
                gGL.vertex3fv(frust[7].mV);
                gGL.vertex3fv(frust[6].mV);
                gGL.end();
            }


            if (i < 4)
            {

                //if (i == 0 || !mShadowFrustPoints[i].empty())
                {
                    //render visible point cloud
                    gGL.flush();
                    // <plus> research/207 6.3: glPointSize gone (Vulkan has no point size state; the backend's answer did nothing) </plus>
                    gGL.begin(LLRender::POINTS);

                    F32* c = col+i*4;
                    gGL.color3fv(c);

                    for (U32 j = 0; j < mShadowFrustPoints[i].size(); ++j)
                        {
                            gGL.vertex3fv(mShadowFrustPoints[i][j].mV);

                        }
                    gGL.end();

                    gGL.flush();
                    // <plus> research/207 6.3: glPointSize gone (Vulkan has no point size state; the backend's answer did nothing) </plus>

                    LLVector3* ext = mShadowExtents[i];
                    LLVector3 pos = (ext[0]+ext[1])*0.5f;
                    LLVector3 size = (ext[1]-ext[0])*0.5f;
                    drawBoxOutline(pos, size);

                    //render camera frustum splits as outlines
                    gGL.begin(LLRender::LINES);
                    gGL.vertex3fv(frust[0].mV); gGL.vertex3fv(frust[1].mV);
                    gGL.vertex3fv(frust[1].mV); gGL.vertex3fv(frust[2].mV);
                    gGL.vertex3fv(frust[2].mV); gGL.vertex3fv(frust[3].mV);
                    gGL.vertex3fv(frust[3].mV); gGL.vertex3fv(frust[0].mV);
                    gGL.vertex3fv(frust[4].mV); gGL.vertex3fv(frust[5].mV);
                    gGL.vertex3fv(frust[5].mV); gGL.vertex3fv(frust[6].mV);
                    gGL.vertex3fv(frust[6].mV); gGL.vertex3fv(frust[7].mV);
                    gGL.vertex3fv(frust[7].mV); gGL.vertex3fv(frust[4].mV);
                    gGL.vertex3fv(frust[0].mV); gGL.vertex3fv(frust[4].mV);
                    gGL.vertex3fv(frust[1].mV); gGL.vertex3fv(frust[5].mV);
                    gGL.vertex3fv(frust[2].mV); gGL.vertex3fv(frust[6].mV);
                    gGL.vertex3fv(frust[3].mV); gGL.vertex3fv(frust[7].mV);
                    gGL.end();
                }
            }

            /*gGL.flush();
             gGL.setLineWidth(16-i*2);
            for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
                    iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
            {
                LLViewerRegion* region = *iter;
                for (U32 j = 0; j < LLViewerRegion::NUM_PARTITIONS; j++)
                {
                    LLSpatialPartition* part = region->getSpatialPartition(j);
                    if (part)
                    {
                        if (hasRenderType(part->mDrawableType))
                        {
                            part->renderIntersectingBBoxes(&mShadowCamera[i]);
                        }
                    }
                }
            }
            gGL.flush();
             gGL.setLineWidth(1.f);*/
        }
    }

    if (mRenderDebugMask & RENDER_DEBUG_WIND_VECTORS)
    {
        gAgent.getRegion()->mWind.renderVectors();
    }

    if (mRenderDebugMask & RENDER_DEBUG_COMPOSITION)
    {
        // Debug composition layers
        F32 x, y;

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        if (gAgent.getRegion())
        {
            gGL.begin(LLRender::POINTS);
            // Draw the composition layer for the region that I'm in.
            for (x = 0; x <= 260; x++)
            {
                for (y = 0; y <= 260; y++)
                {
                    if ((x > 255) || (y > 255))
                    {
                        gGL.color4f(1.f, 0.f, 0.f, 1.f);
                    }
                    else
                    {
                        gGL.color4f(0.f, 0.f, 1.f, 1.f);
                    }
                    F32 z = gAgent.getRegion()->getCompositionXY((S32)x, (S32)y);
                    z *= 5.f;
                    z += 50.f;
                    gGL.vertex3f(x, y, z);
                }
            }
            gGL.end();
        }
    }

    gGL.flush();
    gUIProgram.unbind();
}

void LLPipeline::rebuildPools()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    assertInitialized();

    auto max_count = mPools.size();
    pool_set_t::iterator iter1 = mPools.upper_bound(mLastRebuildPool);
    while(max_count > 0 && mPools.size() > 0) // && num_rebuilds < MAX_REBUILDS)
    {
        if (iter1 == mPools.end())
        {
            iter1 = mPools.begin();
        }
        LLDrawPool* poolp = *iter1;

        if (poolp->isDead())
        {
            mPools.erase(iter1++);
            removeFromQuickLookup( poolp );
            if (poolp == mLastRebuildPool)
            {
                mLastRebuildPool = NULL;
            }
            delete poolp;
        }
        else
        {
            mLastRebuildPool = poolp;
            iter1++;
        }
        max_count--;
    }
}

void LLPipeline::addToQuickLookup( LLDrawPool* new_poolp )
{
    assertInitialized();

    switch( new_poolp->getType() )
    {
    case LLDrawPool::POOL_SIMPLE:
        if (mSimplePool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate simple pool." << LL_ENDL;
        }
        else
        {
            mSimplePool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_ALPHA_MASK:
        if (mAlphaMaskPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate alpha mask pool." << LL_ENDL;
            break;
        }
        else
        {
            mAlphaMaskPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        if (mFullbrightAlphaMaskPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate alpha mask pool." << LL_ENDL;
            break;
        }
        else
        {
            mFullbrightAlphaMaskPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_GRASS:
        if (mGrassPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate grass pool." << LL_ENDL;
        }
        else
        {
            mGrassPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_FULLBRIGHT:
        if (mFullbrightPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate simple pool." << LL_ENDL;
        }
        else
        {
            mFullbrightPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_GLOW:
        if (mGlowPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate glow pool." << LL_ENDL;
        }
        else
        {
            mGlowPool = (LLRenderPass*) new_poolp;
        }
        break;

    case LLDrawPool::POOL_TREE:
        mTreePools[ uintptr_t(new_poolp->getTexture()) ] = new_poolp ;
        break;

    case LLDrawPool::POOL_TERRAIN:
        mTerrainPools[ uintptr_t(new_poolp->getTexture()) ] = new_poolp ;
        break;

    case LLDrawPool::POOL_BUMP:
        if (mBumpPool)
        {
            llassert(0);
            LL_WARNS() << "Ignoring duplicate bump pool." << LL_ENDL;
        }
        else
        {
            mBumpPool = new_poolp;
        }
        break;
    case LLDrawPool::POOL_MATERIALS:
        if (mMaterialsPool)
        {
            llassert(0);
            LL_WARNS() << "Ignorning duplicate materials pool." << LL_ENDL;
        }
        else
        {
            mMaterialsPool = new_poolp;
        }
        break;
    case LLDrawPool::POOL_ALPHA_PRE_WATER:
        if( mAlphaPoolPreWater )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Alpha pre-water pool" << LL_ENDL;
        }
        else
        {
            mAlphaPoolPreWater = (LLDrawPoolAlpha*) new_poolp;
        }
        break;
    case LLDrawPool::POOL_ALPHA_POST_WATER:
        if (mAlphaPoolPostWater)
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Alpha post-water pool" << LL_ENDL;
        }
        else
        {
            mAlphaPoolPostWater = (LLDrawPoolAlpha*)new_poolp;
        }
        break;

    case LLDrawPool::POOL_AVATAR:
    case LLDrawPool::POOL_CONTROL_AV:
        break; // Do nothing

    case LLDrawPool::POOL_SKY:
        if( mSkyPool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Sky pool" << LL_ENDL;
        }
        else
        {
            mSkyPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_WATER:
        if( mWaterPool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Water pool" << LL_ENDL;
        }
        else
        {
            mWaterPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_WL_SKY:
        if( mWLSkyPool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate WLSky Pool" << LL_ENDL;
        }
        else
        {
            mWLSkyPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_GLTF_PBR:
        if( mPBROpaquePool )
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate PBR Opaque Pool" << LL_ENDL;
        }
        else
        {
            mPBROpaquePool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        if (mPBRAlphaMaskPool)
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate PBR Alpha Mask Pool" << LL_ENDL;
        }
        else
        {
            mPBRAlphaMaskPool = new_poolp;
        }
        break;

    case LLDrawPool::POOL_WATEREXCLUSION:
        if (mWaterExclusionPool)
        {
            llassert(0);
            LL_WARNS() << "LLPipeline::addPool(): Ignoring duplicate Water Exclusion Pool" << LL_ENDL;
        }
        else
        {
            mWaterExclusionPool = new_poolp;
        }
        break;

    default:
        llassert(0);
        LL_WARNS() << "Invalid Pool Type in  LLPipeline::addPool()" << LL_ENDL;
        break;
    }
}

void LLPipeline::removePool( LLDrawPool* poolp )
{
    assertInitialized();
    removeFromQuickLookup(poolp);
    mPools.erase(poolp);
    delete poolp;
}

void LLPipeline::removeFromQuickLookup( LLDrawPool* poolp )
{
    assertInitialized();
    switch( poolp->getType() )
    {
    case LLDrawPool::POOL_SIMPLE:
        llassert(mSimplePool == poolp);
        mSimplePool = NULL;
        break;

    case LLDrawPool::POOL_ALPHA_MASK:
        llassert(mAlphaMaskPool == poolp);
        mAlphaMaskPool = NULL;
        break;

    case LLDrawPool::POOL_FULLBRIGHT_ALPHA_MASK:
        llassert(mFullbrightAlphaMaskPool == poolp);
        mFullbrightAlphaMaskPool = NULL;
        break;

    case LLDrawPool::POOL_GRASS:
        llassert(mGrassPool == poolp);
        mGrassPool = NULL;
        break;

    case LLDrawPool::POOL_FULLBRIGHT:
        llassert(mFullbrightPool == poolp);
        mFullbrightPool = NULL;
        break;

    case LLDrawPool::POOL_WL_SKY:
        llassert(mWLSkyPool == poolp);
        mWLSkyPool = NULL;
        break;

    case LLDrawPool::POOL_GLOW:
        llassert(mGlowPool == poolp);
        mGlowPool = NULL;
        break;

    case LLDrawPool::POOL_TREE:
        #ifdef _DEBUG
            {
                bool found = mTreePools.erase( (uintptr_t)poolp->getTexture() );
                llassert( found );
            }
        #else
            mTreePools.erase( (uintptr_t)poolp->getTexture() );
        #endif
        break;

    case LLDrawPool::POOL_TERRAIN:
        #ifdef _DEBUG
            {
                bool found = mTerrainPools.erase( (uintptr_t)poolp->getTexture() );
                llassert( found );
            }
        #else
            mTerrainPools.erase( (uintptr_t)poolp->getTexture() );
        #endif
        break;

    case LLDrawPool::POOL_BUMP:
        llassert( poolp == mBumpPool );
        mBumpPool = NULL;
        break;

    case LLDrawPool::POOL_MATERIALS:
        llassert(poolp == mMaterialsPool);
        mMaterialsPool = NULL;
        break;

    case LLDrawPool::POOL_ALPHA_PRE_WATER:
        llassert( poolp == mAlphaPoolPreWater );
        mAlphaPoolPreWater = nullptr;
        break;

    case LLDrawPool::POOL_ALPHA_POST_WATER:
        llassert(poolp == mAlphaPoolPostWater);
        mAlphaPoolPostWater = nullptr;
        break;

    case LLDrawPool::POOL_AVATAR:
    case LLDrawPool::POOL_CONTROL_AV:
        break; // Do nothing

    case LLDrawPool::POOL_SKY:
        llassert( poolp == mSkyPool );
        mSkyPool = NULL;
        break;

    case LLDrawPool::POOL_WATER:
        llassert( poolp == mWaterPool );
        mWaterPool = NULL;
        break;

    case LLDrawPool::POOL_GLTF_PBR:
        llassert( poolp == mPBROpaquePool );
        mPBROpaquePool = NULL;
        break;

    case LLDrawPool::POOL_GLTF_PBR_ALPHA_MASK:
        llassert(poolp == mPBRAlphaMaskPool);
        mPBRAlphaMaskPool = NULL;
        break;

    case LLDrawPool::POOL_WATEREXCLUSION:
        llassert(poolp == mWaterExclusionPool);
        mWaterExclusionPool = nullptr;
        break;

    default:
        llassert(0);
        LL_WARNS() << "Invalid Pool Type in  LLPipeline::removeFromQuickLookup() type=" << poolp->getType() << LL_ENDL;
        break;
    }
}

void LLPipeline::resetDrawOrders()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    assertInitialized();
    // Iterate through all of the draw pools and rebuild them.
    for (pool_set_t::iterator iter = mPools.begin(); iter != mPools.end(); ++iter)
    {
        LLDrawPool *poolp = *iter;
        poolp->resetDrawOrders();
    }
}

//============================================================================
// Once-per-frame setup of hardware lights,
// including sun/moon, avatar backlight, and up to 6 local lights

void LLPipeline::setupAvatarLights(bool for_edit)
{
    assertInitialized();

    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsSky::ptr_t psky = environment.getCurrentSky();

    bool sun_up = environment.getIsSunUp();


    if (for_edit)
    {
        LLColor4 diffuse(1.f, 1.f, 1.f, 0.f);
        LLVector4 light_pos_cam(-8.f, 0.25f, 10.f, 0.f);  // w==0 => directional light
        LLMatrix4 camera_mat = LLViewerCamera::getInstance()->getModelview();
        LLMatrix4 camera_rot(camera_mat.getMat3());
        camera_rot.invert();
        LLVector4 light_pos = light_pos_cam * camera_rot;

        light_pos.normalize();

        LLLightState* light = gGL.getLight(1);

        mHWLightColors[1] = diffuse;

        light->setDiffuse(diffuse);
        light->setAmbient(LLColor4::black);
        light->setSpecular(LLColor4::black);
        light->setPosition(light_pos);
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);
        light->setSpotExponent(0.f);
        light->setSpotCutoff(180.f);
    }
    else if (gAvatarBacklight)
    {
        LLVector3 light_dir = sun_up ? LLVector3(mSunDir) : LLVector3(mMoonDir);
        LLVector3 opposite_pos = -light_dir;
        LLVector3 orthog_light_pos = light_dir % LLVector3::z_axis;
        LLVector4 backlight_pos = LLVector4(lerp(opposite_pos, orthog_light_pos, 0.3f), 0.0f);
        backlight_pos.normalize();

        LLColor4 light_diffuse = sun_up ? mSunDiffuse : mMoonDiffuse;

        LLColor4 backlight_diffuse(1.f - light_diffuse.mV[VRED], 1.f - light_diffuse.mV[VGREEN], 1.f - light_diffuse.mV[VBLUE], 1.f);
        F32 max_component = 0.001f;
        for (S32 i = 0; i < 3; i++)
        {
            if (backlight_diffuse.mV[i] > max_component)
            {
                max_component = backlight_diffuse.mV[i];
            }
        }
        F32 backlight_mag;
        if (LLEnvironment::instance().getIsSunUp())
        {
            backlight_mag = BACKLIGHT_DAY_MAGNITUDE_OBJECT;
        }
        else
        {
            backlight_mag = BACKLIGHT_NIGHT_MAGNITUDE_OBJECT;
        }
        backlight_diffuse *= backlight_mag / max_component;

        mHWLightColors[1] = backlight_diffuse;

        LLLightState* light = gGL.getLight(1);

        light->setPosition(backlight_pos);
        light->setDiffuse(backlight_diffuse);
        light->setAmbient(LLColor4::black);
        light->setSpecular(LLColor4::black);
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);
        light->setSpotExponent(0.f);
        light->setSpotCutoff(180.f);
    }
    else
    {
        LLLightState* light = gGL.getLight(1);

        mHWLightColors[1] = LLColor4::black;

        light->setDiffuse(LLColor4::black);
        light->setAmbient(LLColor4::black);
        light->setSpecular(LLColor4::black);
    }
}

// <plus> research/203: the distance part of calc_light_dist, one body for a
// light read from its object and one read from its record (the same bits)
static F32 light_dist(F32 radius, const LLVector3& position, bool active, const LLVector3& cam_pos)
{
    F32 dist = dist_vec(position, cam_pos);
    dist = llmax(dist - radius, 0.f);
    if (active)
    {
        // moving lights get a little higher priority (too much causes artifacts)
        dist = llmax(dist - radius * 0.25f, 0.f);
    }
    return dist;
}
// </plus>

static F32 calc_light_dist(LLVOVolume* light, const LLVector3& cam_pos, F32 max_dist)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    F32 inten = light->getLightIntensity();
    if (inten < .001f)
    {
        return max_dist;
    }
    bool selected = light->isSelected();
    if (selected)
    {
        return 0.f; // selected lights get highest priority
    }
    F32 radius = light->getLightRadius();
    // <plus> research/203: the rest in light_dist
    return light_dist(radius, light->getRenderPosition(), light->mDrawable.notNull() && light->mDrawable->isState(LLDrawable::ACTIVE),
                      cam_pos);
    // </plus>
}

// <plus> research/203 (lllocallights.h): the local light table
// calc_light_dist for a light read from its record. Records are read only for
// lights whose drawable is not active: no "moving lights" bonus.
static F32 calc_light_dist(const LLLocalLights::Values& light, const LLVector3& cam_pos, F32 max_dist)
{
    F32 inten = light.intensity;
    if (inten < .001f)
    {
        return max_dist;
    }
    bool selected = (light.flags & LLLocalLights::FLAG_SELECTED) != 0;
    if (selected)
    {
        return 0.f; // selected lights get highest priority
    }
    return light_dist(light.radius, LLVector3(light.render_position), false, cam_pos);
}

// What the light loops read of a light, from its object (the getters
// calc_light_dist and the light loop of renderDeferredLighting call).
static void read_local_light(const LLDrawable* drawable, LLVOVolume* volume, LLLocalLights::Values& out)
{
    const LLVector3 render_position = volume->getRenderPosition();
    const LLVector3& center = drawable->getPositionAgent();
    const LLColor3 color = volume->getLightLinearColor();
    for (U32 i = 0; i < 3; ++i)
    {
        out.render_position[i] = render_position.mV[i];
        out.center[i] = center.mV[i];
        out.color[i] = color.mV[i];
    }
    out.intensity = volume->getLightIntensity();
    out.radius = volume->getLightRadius();
    out.size = volume->getLightRadius() * 1.5f;
    out.falloff = volume->getLightFalloff(DEFERRED_LIGHT_FALLOFF);
    out.flags = (volume->isAttachment() ? LLLocalLights::FLAG_ATTACHMENT : 0u) | (volume->isHUDAttachment() ? LLLocalLights::FLAG_HUD : 0u) |
                (volume->getAvatar() ? LLLocalLights::FLAG_AVATAR : 0u) | (volume->isLightSpotlight() ? LLLocalLights::FLAG_SPOTLIGHT : 0u) |
                (volume->isSelected() ? LLLocalLights::FLAG_SELECTED : 0u);
}

// The values to read a light from, null: read its object (LLLocalLights::serve).
static const LLLocalLights::Values* serve_light(const LLDrawable* drawable, LLVOVolume* volume, bool reads, bool verifies, bool check,
                                                LLLocalLights::Counts& counts)
{
    return LLLocalLights::serve(
        gLocalLightTable.slots(), drawable, drawable->isState(LLDrawable::ACTIVE), reads, verifies, check, counts,
        [drawable, volume](LLLocalLights::Values& out) { read_local_light(drawable, volume, out); },
        [](const char* field, const LLLocalLights::Values& was, const LLLocalLights::Values& now)
        { gLocalLightTable.reportMismatch(field, was, now); });
}
// </plus>

void LLPipeline::calcNearbyLights(LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    assertInitialized();

    if (LLPipeline::sReflectionRender || gCubeSnapshot || LLPipeline::sRenderingHUDs || LLApp::isExiting())
    {
        return;
    }

    static LLCachedControl<S32> local_light_count(gSavedSettings, "RenderLocalLightCount", 256);

    if (local_light_count >= 1)
    {
        // mNearbyLight (and all light_set_t's) are sorted such that
        // begin() == the closest light and rbegin() == the farthest light
        const S32 MAX_LOCAL_LIGHTS = 6;
        LLVector3 cam_pos = camera.getOrigin();

        F32 max_dist;
        if (LLPipeline::sRenderDeferred)
        {
            max_dist = RenderFarClip;
        }
        else
        {
            max_dist = llmin(RenderFarClip, LIGHT_MAX_RADIUS * 4.f);
        }

        // <plus> research/203 (lllocallights.h): a light that does not move is
        // read from its record in the local light table (on; verify reads the
        // objects and compares each record). The first loop's answers go to
        // `updates`; the set then takes them in place while their order holds
        // (on), else its nodes move over (research/187 N1a: no allocation).
        gLocalLightTable.beginFrame();
        const bool table_reads = LLLocalLightTable::reads();
        const bool table_verifies = LLLocalLightTable::verifies();
        auto table_light = [&](const LLDrawable* drawable, LLVOVolume* volume) -> const LLLocalLights::Values*
        {
            if ((!table_reads && !table_verifies) || !volume)
            {
                return nullptr;
            }
            return serve_light(drawable, volume, table_reads, table_verifies, true, gLocalLightTable.counts());
        };
        typedef LLLocalLights::NearbyUpdate<light_set_t::iterator> NearbyUpdate;
        static std::vector<NearbyUpdate> updates;   // empty between calls
        static light_set_t moved_nodes;             // empty between calls
        static light_set_t found_nodes;             // empty between calls
        // </plus>

        ++sNearbyLightsVersion;     // LLRHI (research/52): the set is made again below

        // UPDATE THE EXISTING NEARBY LIGHTS
        for (light_set_t::iterator iter = mNearbyLights.begin();
            iter != mNearbyLights.end(); iter++)
        {
            const Light* light = &(*iter);
            LLDrawable* drawable = light->drawable;
            // <plus> research/203: its record (null: read the object); a record is no avatar's
            LLVOVolume* volight = drawable->getVOVolume(); // was below
            const LLLocalLights::Values* record = table_light(drawable, volight);
            const LLViewerObject *vobj = record ? nullptr : light->drawable->getVObj();
            // </plus>
            if(vobj && vobj->getAvatar()
               && (vobj->getAvatar()->isTooComplex() || vobj->getAvatar()->isInMuteList() || vobj->getAvatar()->isTooSlow())
               )
            {
                drawable->clearState(LLDrawable::NEARBY_LIGHT);
                updates.push_back({ iter, 0.f, 0.f, false }); // <plus> research/203 </plus>
                continue;
            }

            if (!volight || !drawable->isState(LLDrawable::LIGHT))
            {
                drawable->clearState(LLDrawable::NEARBY_LIGHT);
                updates.push_back({ iter, 0.f, 0.f, false }); // <plus> research/203 </plus>
                continue;
            }
            if (light->fade <= -LIGHT_FADE_TIME)
            {
                drawable->clearState(LLDrawable::NEARBY_LIGHT);
                updates.push_back({ iter, 0.f, 0.f, false }); // <plus> research/203 </plus>
                continue;
            }
            if (!sRenderAttachedLights && volight && (record ? (record->flags & LLLocalLights::FLAG_ATTACHMENT) != 0 : volight->isAttachment())) // <plus> research/203: record </plus>
            {
                drawable->clearState(LLDrawable::NEARBY_LIGHT);
                updates.push_back({ iter, 0.f, 0.f, false }); // <plus> research/203 </plus>
                continue;
            }

            F32 dist = record ? calc_light_dist(*record, cam_pos, max_dist) : calc_light_dist(volight, cam_pos, max_dist); // <plus> research/203: record </plus>
            F32 fade = light->fade;
            // actual fade gets decreased/increased by setupHWLights
            // light->fade value is 'time'.
            // >=0 and light will become visible as value increases
            // <0 and light will fade out
            if (dist < max_dist)
            {
                if (fade < 0)
                {
                    // mark light to fade in
                    // if fade was -LIGHT_FADE_TIME - it was fully invisible
                    // if fade -0 - it was fully visible
                    // visibility goes up from 0 to LIGHT_FADE_TIME.
                    fade += LIGHT_FADE_TIME;
                }
            }
            else
            {
                // mark light to fade out
                // visibility goes down from -0 to -LIGHT_FADE_TIME.
                if (fade >= LIGHT_FADE_TIME)
                {
                    fade = -0.0001f; // was fully visible
                }
                else if (fade >= 0)
                {
                    // 0.75 visible light should stay 0.75 visible, but should reverse direction
                    fade -= LIGHT_FADE_TIME;
                }
            }
            updates.push_back({ iter, dist, fade, true }); // <plus> research/203: was cur_nearby_lights.insert(Light(drawable, dist, fade)); </plus>
        }
        // <plus> research/203: was mNearbyLights = cur_nearby_lights;
        if (!LLPipeline::sRenderDeferred)
        {
            for (const NearbyUpdate& u : updates)
            {
                if (u.keep)
                {
                    moved_nodes.insert(Light(u.it->drawable, u.dist, u.fade));
                }
            }
            mNearbyLights = moved_nodes;
            moved_nodes.clear();
        }
        else if (table_reads && LLLocalLights::keysStayOrdered(updates))
        {
            LLLocalLights::applyInPlace(mNearbyLights, updates);
        }
        else
        {
            LLLocalLights::applyByNodes(mNearbyLights, moved_nodes, updates);
        }
        updates.clear();
        // </plus>

        // FIND NEW LIGHTS THAT ARE IN RANGE
        light_set_t& new_nearby_lights = found_nodes; // <plus> research/187 N1: was a new set each frame </plus>
        for (LLDrawable::ordered_drawable_set_t::iterator iter = mLights.begin();
             iter != mLights.end(); ++iter)
        {
            LLDrawable* drawable = *iter;
            LLVOVolume* light = drawable->getVOVolume();
            if (!light || drawable->isState(LLDrawable::NEARBY_LIGHT))
            {
                continue;
            }
            const LLLocalLights::Values* record = table_light(drawable, light); // <plus> research/203: its record (null: read the object) </plus>
            if (record ? (record->flags & LLLocalLights::FLAG_HUD) != 0 : light->isHUDAttachment()) // <plus> research/203: record </plus>
            {
                continue; // no lighting from HUD objects
            }
            if (!sRenderAttachedLights && light && (record ? (record->flags & LLLocalLights::FLAG_ATTACHMENT) != 0 : light->isAttachment())) // <plus> research/203: record </plus>
            {
                continue;
            }
            LLVOAvatar * av = record ? nullptr : light->getAvatar(); // <plus> research/203: a record is no avatar's </plus>
            if (av && (av->isTooComplex() || av->isInMuteList() || av->isTooSlow()))
            {
                // avatars that are already in the list will be removed by removeMutedAVsLights
                continue;
            }
            F32 dist = record ? calc_light_dist(*record, cam_pos, max_dist) : calc_light_dist(light, cam_pos, max_dist); // <plus> research/203: record </plus>
            if (dist >= max_dist)
            {
                continue;
            }
            new_nearby_lights.insert(Light(drawable, dist, 0.f));
            if (!LLPipeline::sRenderDeferred && new_nearby_lights.size() > (U32)MAX_LOCAL_LIGHTS)
            {
                new_nearby_lights.erase(--new_nearby_lights.end());
                const Light& last = *new_nearby_lights.rbegin();
                max_dist = last.dist;
            }
        }

        // INSERT ANY NEW LIGHTS
        // <plus> research/187 N1: (deferred) the nodes move over
        if (LLPipeline::sRenderDeferred)
        {
            while (!new_nearby_lights.empty())
            {
                light_set_t::node_type node = new_nearby_lights.extract(new_nearby_lights.begin());
                LLDrawable* drawable = node.value().drawable;
                mNearbyLights.insert(std::move(node));
                drawable->setState(LLDrawable::NEARBY_LIGHT);
            }
        }
        else
        // </plus>
        for (light_set_t::iterator iter = new_nearby_lights.begin();
             iter != new_nearby_lights.end(); iter++)
        {
            const Light* light = &(*iter);
            if (LLPipeline::sRenderDeferred || mNearbyLights.size() < (U32)MAX_LOCAL_LIGHTS)
            {
                mNearbyLights.insert(*light);
                ((LLDrawable*) light->drawable)->setState(LLDrawable::NEARBY_LIGHT);
            }
            else
            {
                // crazy cast so that we can overwrite the fade value
                // even though gcc enforces sets as const
                // (fade value doesn't affect sort so this is safe)
                Light* farthest_light = (const_cast<Light*>(&(*(mNearbyLights.rbegin()))));
                if (light->dist < farthest_light->dist)
                {
                    // mark light to fade out
                    // visibility goes down from -0 to -LIGHT_FADE_TIME.
                    //
                    // This is a mess, but for now it needs to be in sync
                    // with fade code above. Ex: code above detects distance < max,
                    // sets fade time to positive, this code then detects closer
                    // lights and sets fade time negative, fully compensating
                    // for the code above
                    if (farthest_light->fade >= LIGHT_FADE_TIME)
                    {
                        farthest_light->fade = -0.0001f; // was fully visible
                    }
                    else if (farthest_light->fade >= 0)
                    {
                        farthest_light->fade -= LIGHT_FADE_TIME;
                    }
                }
                else
                {
                    break; // none of the other lights are closer
                }
            }
        }
        new_nearby_lights.clear(); // <plus> research/187 N1 </plus>

        //mark nearby lights not-removable.
        for (light_set_t::iterator iter = mNearbyLights.begin();
             iter != mNearbyLights.end(); iter++)
        {
            const Light* light = &(*iter);
            ((LLViewerOctreeEntryData*) light->drawable)->setVisible();
        }
    }
}

void LLPipeline::setupHWLights()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
    assertInitialized();

    if (LLPipeline::sRenderingHUDs)
    {
        return;
    }

    F32 light_scale = 1.f;

    if (gCubeSnapshot)
    { //darken local lights when probe ambiance is above 1
        light_scale = mReflectionMapManager.mLightScale;
    }


    LLEnvironment& environment = LLEnvironment::instance();
    LLSettingsSky::ptr_t psky = environment.getCurrentSky();

    // Ambient
    LLColor4 ambient = psky->getTotalAmbient();

    gGL.setAmbientLightColor(ambient);

    bool sun_up  = environment.getIsSunUp();
    bool moon_up = environment.getIsMoonUp();

    // Light 0 = Sun or Moon (All objects)
    {
        LLVector4 sun_dir(environment.getSunDirection(), 0.0f);
        LLVector4 moon_dir(environment.getMoonDirection(), 0.0f);

        mSunDir.setVec(sun_dir);
        mMoonDir.setVec(moon_dir);

        mSunDiffuse.setVec(psky->getSunlightColor());
        mMoonDiffuse.setVec(psky->getMoonlightColor());

        F32 max_color = llmax(mSunDiffuse.mV[0], mSunDiffuse.mV[1], mSunDiffuse.mV[2]);
        if (max_color > 1.f)
        {
            mSunDiffuse *= 1.f/max_color;
        }
        mSunDiffuse.clamp();

        max_color = llmax(mMoonDiffuse.mV[0], mMoonDiffuse.mV[1], mMoonDiffuse.mV[2]);
        if (max_color > 1.f)
        {
            mMoonDiffuse *= 1.f/max_color;
        }
        mMoonDiffuse.clamp();

        // prevent underlighting from having neither lightsource facing us
        if (!sun_up && !moon_up)
        {
            mSunDiffuse.setVec(LLColor4(0.0, 0.0, 0.0, 1.0));
            mMoonDiffuse.setVec(LLColor4(0.0, 0.0, 0.0, 1.0));
            mSunDir.setVec(LLVector4(0.0, 1.0, 0.0, 0.0));
            mMoonDir.setVec(LLVector4(0.0, 1.0, 0.0, 0.0));
        }

        LLVector4 light_dir = sun_up ? mSunDir : mMoonDir;

        mHWLightColors[0] = sun_up ? mSunDiffuse : mMoonDiffuse;

        LLLightState* light = gGL.getLight(0);
        light->setPosition(light_dir);

        light->setSunPrimary(sun_up);
        light->setDiffuse(mHWLightColors[0]);
        light->setDiffuseB(mMoonDiffuse);
        light->setAmbient(psky->getTotalAmbient());
        light->setSpecular(LLColor4::black);
        light->setConstantAttenuation(1.f);
        light->setLinearAttenuation(0.f);
        light->setQuadraticAttenuation(0.f);
        light->setSpotExponent(0.f);
        light->setSpotCutoff(180.f);
    }

    // Light 1 = Backlight (for avatars)
    // (set by enableLightsAvatar)

    S32 cur_light = 2;

    // Nearby lights = LIGHT 2-7

    mLightMovingMask = 0;

    static LLCachedControl<S32> local_light_count(gSavedSettings, "RenderLocalLightCount", 256);

    if (local_light_count >= 1)
    {
        for (light_set_t::iterator iter = mNearbyLights.begin();
             iter != mNearbyLights.end(); ++iter)
        {
            LLDrawable* drawable = iter->drawable;
            LLVOVolume* light = drawable->getVOVolume();
            if (!light)
            {
                continue;
            }

            if (light->isAttachment())
            {
                if (!sRenderAttachedLights)
                {
                    continue;
                }
            }

            if (drawable->isState(LLDrawable::ACTIVE))
            {
                mLightMovingMask |= (1<<cur_light);
            }

            //send linear light color to shader
            LLColor4  light_color = light->getLightLinearColor() * light_scale;
            light_color.mV[3] = 0.0f;

            F32 fade = iter->fade;
            if (fade < LIGHT_FADE_TIME)
            {
                // fade in/out light
                if (fade >= 0.f)
                {
                    fade = fade / LIGHT_FADE_TIME;
                    ((Light*) (&(*iter)))->fade += gFrameIntervalSeconds.value();
                }
                else
                {
                    fade = 1.f + fade / LIGHT_FADE_TIME;
                    ((Light*) (&(*iter)))->fade -= gFrameIntervalSeconds.value();
                }
                fade = llclamp(fade,0.f,1.f);
                light_color *= fade;
            }

            if (light_color.magVecSquared() < 0.001f)
            {
                continue;
            }

            LLVector3 light_pos(light->getRenderPosition());
            LLVector4 light_pos_gl(light_pos, 1.0f);

            F32 adjusted_radius = light->getLightRadius() * (sRenderDeferred ? 1.5f : 1.0f);
            if (adjusted_radius <= 0.001f)
            {
                continue;
            }

            F32 x = (3.f * (1.f + (light->getLightFalloff() * 2.0f)));  // why this magic?  probably trying to match a historic behavior.
            F32 linatten = x / adjusted_radius;                         // % of brightness at radius

            mHWLightColors[cur_light] = light_color;
            LLLightState* light_state = gGL.getLight(cur_light);

            light_state->setPosition(light_pos_gl);
            light_state->setDiffuse(light_color);
            light_state->setAmbient(LLColor4::black);
            light_state->setConstantAttenuation(0.f);
            light_state->setSize(light->getLightRadius() * 1.5f);
            light_state->setFalloff(light->getLightFalloff(DEFERRED_LIGHT_FALLOFF));

            if (sRenderDeferred)
            {
                light_state->setLinearAttenuation(linatten);
                light_state->setQuadraticAttenuation(light->getLightFalloff(DEFERRED_LIGHT_FALLOFF) + 1.f); // get falloff to match for forward deferred rendering lights
            }
            else
            {
                light_state->setLinearAttenuation(linatten);
                light_state->setQuadraticAttenuation(0.f);
            }


            if (light->isLightSpotlight() // directional (spot-)light
                && (LLPipeline::sRenderDeferred || RenderSpotLightsInNondeferred)) // these are only rendered as GL spotlights if we're in deferred rendering mode *or* the setting forces them on
            {
                LLQuaternion quat = light->getRenderRotation();
                LLVector3 at_axis(0,0,-1); // this matches deferred rendering's object light direction
                at_axis *= quat;

                light_state->setSpotDirection(at_axis);
                light_state->setSpotCutoff(90.f);
                light_state->setSpotExponent(2.f);

                LLVector3 spotParams = light->getSpotLightParams();

                const LLColor4 specular(0.f, 0.f, 0.f, spotParams[2]);
                light_state->setSpecular(specular);
            }
            else // omnidirectional (point) light
            {
                light_state->setSpotExponent(0.f);
                light_state->setSpotCutoff(180.f);

                // we use specular.z = 1.0 as a cheap hack for the shaders to know that this is omnidirectional rather than a spotlight
                const LLColor4 specular(0.f, 0.f, 1.f, 0.f);
                light_state->setSpecular(specular);
            }
            cur_light++;
            if (cur_light >= 8)
            {
                break; // safety
            }
        }
    }
    for ( ; cur_light < 8 ; cur_light++)
    {
        mHWLightColors[cur_light] = LLColor4::black;
        LLLightState* light = gGL.getLight(cur_light);
        light->setSunPrimary(true);
        light->setDiffuse(LLColor4::black);
        light->setAmbient(LLColor4::black);
        light->setSpecular(LLColor4::black);
    }

    // Bookmark comment to allow searching for mSpecialRenderMode == 3 (avatar edit mode),
    // prev site of forward (non-deferred) character light injection, removed by SL-13522 09/20

    // Init GL state
    for (S32 i = 0; i < 8; ++i)
    {
        gGL.getLight(i)->disable();
    }
    mLightMask = 0;
}

void LLPipeline::enableLights(U32 mask)
{
    assertInitialized();

    if (mLightMask != mask)
    {
        stop_glerror();
        if (mask)
        {
            stop_glerror();
            for (S32 i=0; i<8; i++)
            {
                LLLightState* light = gGL.getLight(i);
                if (mask & (1<<i))
                {
                    light->enable();
                    light->setDiffuse(mHWLightColors[i]);
                }
                else
                {
                    light->disable();
                    light->setDiffuse(LLColor4::black);
                }
            }
            stop_glerror();
        }
        mLightMask = mask;
        stop_glerror();
    }
}

void LLPipeline::enableLightsDynamic()
{
    assertInitialized();
    U32 mask = 0xff & (~2); // Local lights
    enableLights(mask);

    if (isAgentAvatarValid())
    {
        if (gAgentAvatarp->mSpecialRenderMode == 0) // normal
        {
            gPipeline.enableLightsAvatar();
        }
        else if (gAgentAvatarp->mSpecialRenderMode == 2)  // anim preview
        {
            gPipeline.enableLightsAvatarEdit(LLColor4(0.7f, 0.6f, 0.3f, 1.f));
        }
    }
}

void LLPipeline::enableLightsAvatar()
{
    U32 mask = 0xff; // All lights
    setupAvatarLights(false);
    enableLights(mask);
}

void LLPipeline::enableLightsPreview()
{
    disableLights();

    LLColor4 ambient = PreviewAmbientColor;
    gGL.setAmbientLightColor(ambient);

    LLColor4 diffuse0 = PreviewDiffuse0;
    LLColor4 specular0 = PreviewSpecular0;
    LLColor4 diffuse1 = PreviewDiffuse1;
    LLColor4 specular1 = PreviewSpecular1;
    LLColor4 diffuse2 = PreviewDiffuse2;
    LLColor4 specular2 = PreviewSpecular2;

    LLVector3 dir0 = PreviewDirection0;
    LLVector3 dir1 = PreviewDirection1;
    LLVector3 dir2 = PreviewDirection2;

    dir0.normVec();
    dir1.normVec();
    dir2.normVec();

    LLVector4 light_pos(dir0, 0.0f);

    LLLightState* light = gGL.getLight(1);

    light->enable();
    light->setPosition(light_pos);
    light->setDiffuse(diffuse0);
    light->setAmbient(ambient);
    light->setSpecular(specular0);
    light->setSpotExponent(0.f);
    light->setSpotCutoff(180.f);

    light_pos = LLVector4(dir1, 0.f);

    light = gGL.getLight(2);
    light->enable();
    light->setPosition(light_pos);
    light->setDiffuse(diffuse1);
    light->setAmbient(ambient);
    light->setSpecular(specular1);
    light->setSpotExponent(0.f);
    light->setSpotCutoff(180.f);

    light_pos = LLVector4(dir2, 0.f);
    light = gGL.getLight(3);
    light->enable();
    light->setPosition(light_pos);
    light->setDiffuse(diffuse2);
    light->setAmbient(ambient);
    light->setSpecular(specular2);
    light->setSpotExponent(0.f);
    light->setSpotCutoff(180.f);
}


void LLPipeline::enableLightsAvatarEdit(const LLColor4& color)
{
    U32 mask = 0x2002; // Avatar backlight only, set ambient
    setupAvatarLights(true);
    enableLights(mask);

    gGL.setAmbientLightColor(color);
}

void LLPipeline::enableLightsFullbright()
{
    assertInitialized();
    U32 mask = 0x1000; // Non-0 mask, set ambient
    enableLights(mask);
}

void LLPipeline::disableLights()
{
    enableLights(0); // no lighting (full bright)
}

//============================================================================

class LLMenuItemGL;
class LLInvFVBridge;
struct cat_folder_pair;
class LLVOBranch;
class LLVOLeaf;

void LLPipeline::findReferences(LLDrawable *drawablep)
{
    assertInitialized();
    if (mLights.find(drawablep) != mLights.end())
    {
        LL_INFOS() << "In mLights" << LL_ENDL;
    }
    if (std::find(mMovedList.begin(), mMovedList.end(), drawablep) != mMovedList.end())
    {
        LL_INFOS() << "In mMovedList" << LL_ENDL;
    }
    if (std::find(mShiftList.begin(), mShiftList.end(), drawablep) != mShiftList.end())
    {
        LL_INFOS() << "In mShiftList" << LL_ENDL;
    }
    if (mRetexturedList.find(drawablep) != mRetexturedList.end())
    {
        LL_INFOS() << "In mRetexturedList" << LL_ENDL;
    }

    if (std::find(mBuildQ1.begin(), mBuildQ1.end(), drawablep) != mBuildQ1.end())
    {
        LL_INFOS() << "In mBuildQ1" << LL_ENDL;
    }

    S32 count;

    count = gObjectList.findReferences(drawablep);
    if (count)
    {
        LL_INFOS() << "In other drawables: " << count << " references" << LL_ENDL;
    }
}

bool LLPipeline::verify()
{
    bool ok = assertInitialized();
    if (ok)
    {
        for (pool_set_t::iterator iter = mPools.begin(); iter != mPools.end(); ++iter)
        {
            LLDrawPool *poolp = *iter;
            if (!poolp->verify())
            {
                ok = false;
            }
        }
    }

    if (!ok)
    {
        LL_WARNS() << "Pipeline verify failed!" << LL_ENDL;
    }
    return ok;
}

//////////////////////////////
//
// Collision detection
//
//

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 *  A method to compute a ray-AABB intersection.
 *  Original code by Andrew Woo, from "Graphics Gems", Academic Press, 1990
 *  Optimized code by Pierre Terdiman, 2000 (~20-30% faster on my Celeron 500)
 *  Epsilon value added by Klaus Hartmann. (discarding it saves a few cycles only)
 *
 *  Hence this version is faster as well as more robust than the original one.
 *
 *  Should work provided:
 *  1) the integer representation of 0.0f is 0x00000000
 *  2) the sign bit of the float is the most significant one
 *
 *  Report bugs: p.terdiman@codercorner.com
 *
 *  \param      aabb        [in] the axis-aligned bounding box
 *  \param      origin      [in] ray origin
 *  \param      dir         [in] ray direction
 *  \param      coord       [out] impact coordinates
 *  \return     true if ray intersects AABB
 */
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//#define RAYAABB_EPSILON 0.00001f
#define IR(x)   ((U32&)x)

bool LLRayAABB(const LLVector3 &center, const LLVector3 &size, const LLVector3& origin, const LLVector3& dir, LLVector3 &coord, F32 epsilon)
{
    bool Inside = true;
    LLVector3 MinB = center - size;
    LLVector3 MaxB = center + size;
    LLVector3 MaxT;
    MaxT.mV[VX]=MaxT.mV[VY]=MaxT.mV[VZ]=-1.0f;

    // Find candidate planes.
    for(U32 i=0;i<3;i++)
    {
        if(origin.mV[i] < MinB.mV[i])
        {
            coord.mV[i] = MinB.mV[i];
            Inside      = false;

            // Calculate T distances to candidate planes
            if(IR(dir.mV[i]))   MaxT.mV[i] = (MinB.mV[i] - origin.mV[i]) / dir.mV[i];
        }
        else if(origin.mV[i] > MaxB.mV[i])
        {
            coord.mV[i] = MaxB.mV[i];
            Inside      = false;

            // Calculate T distances to candidate planes
            if(IR(dir.mV[i]))   MaxT.mV[i] = (MaxB.mV[i] - origin.mV[i]) / dir.mV[i];
        }
    }

    // Ray origin inside bounding box
    if(Inside)
    {
        coord = origin;
        return true;
    }

    // Get largest of the maxT's for final choice of intersection
    U32 WhichPlane = 0;
    if(MaxT.mV[1] > MaxT.mV[WhichPlane])    WhichPlane = 1;
    if(MaxT.mV[2] > MaxT.mV[WhichPlane])    WhichPlane = 2;

    // Check final candidate actually inside box
    if(IR(MaxT.mV[WhichPlane])&0x80000000) return false;

    for(U32 i=0;i<3;i++)
    {
        if(i!=WhichPlane)
        {
            coord.mV[i] = origin.mV[i] + MaxT.mV[WhichPlane] * dir.mV[i];
            if (epsilon > 0)
            {
                if(coord.mV[i] < MinB.mV[i] - epsilon || coord.mV[i] > MaxB.mV[i] + epsilon)    return false;
            }
            else
            {
                if(coord.mV[i] < MinB.mV[i] || coord.mV[i] > MaxB.mV[i])    return false;
            }
        }
    }
    return true;    // ray hits box
}

//////////////////////////////
//
// Macros, functions, and inline methods from other classes
//
//

void LLPipeline::setLight(LLDrawable *drawablep, bool is_light)
{
    if (drawablep && assertInitialized())
    {
        if (is_light)
        {
            mLights.insert(drawablep);
            drawablep->setState(LLDrawable::LIGHT);
            gLocalLightTable.add(drawablep); // <plus> research/203: its record (marked: read at the next use) </plus>
        }
        else
        {
            drawablep->clearState(LLDrawable::LIGHT);
            mLights.erase(drawablep);
            gLocalLightTable.remove(drawablep); // <plus> research/203 </plus>
        }
    }
}

//static
void LLPipeline::toggleRenderType(U32 type)
{
    gPipeline.mRenderTypeEnabled[type] = !gPipeline.mRenderTypeEnabled[type];
    if (type == LLPipeline::RENDER_TYPE_WATER)
    {
        gPipeline.mRenderTypeEnabled[LLPipeline::RENDER_TYPE_VOIDWATER] = !gPipeline.mRenderTypeEnabled[LLPipeline::RENDER_TYPE_VOIDWATER];
    }
}

//static
void LLPipeline::toggleRenderTypeControl(U32 type)
{
    gPipeline.toggleRenderType(type);
}

//static
bool LLPipeline::hasRenderTypeControl(U32 type)
{
    return gPipeline.hasRenderType(type);
}

// Allows UI items labeled "Hide foo" instead of "Show foo"
//static
bool LLPipeline::toggleRenderTypeControlNegated(S32 type)
{
    return !gPipeline.hasRenderType(type);
}

//static
void LLPipeline::toggleRenderDebug(U64 bit)
{
    if (gPipeline.hasRenderDebugMask(bit))
    {
        LL_INFOS() << "Toggling render debug mask " << std::hex << bit << " off" << std::dec << LL_ENDL;
    }
    else
    {
        LL_INFOS() << "Toggling render debug mask " << std::hex << bit << " on" << std::dec << LL_ENDL;
    }
    gPipeline.mRenderDebugMask ^= bit;
}


//static
bool LLPipeline::toggleRenderDebugControl(U64 bit)
{
    return gPipeline.hasRenderDebugMask(bit);
}

//static
void LLPipeline::toggleRenderDebugFeature(U32 bit)
{
    gPipeline.mRenderDebugFeatureMask ^= bit;
}


//static
bool LLPipeline::toggleRenderDebugFeatureControl(U32 bit)
{
    return gPipeline.hasRenderDebugFeatureMask(bit);
}

void LLPipeline::setRenderDebugFeatureControl(U32 bit, bool value)
{
    if (value)
    {
        gPipeline.mRenderDebugFeatureMask |= bit;
    }
    else
    {
        gPipeline.mRenderDebugFeatureMask &= !bit;
    }
}

void LLPipeline::pushRenderDebugFeatureMask()
{
    mRenderDebugFeatureStack.push(mRenderDebugFeatureMask);
}

void LLPipeline::popRenderDebugFeatureMask()
{
    if (mRenderDebugFeatureStack.empty())
    {
        LL_ERRS() << "Depleted render feature stack." << LL_ENDL;
    }

    mRenderDebugFeatureMask = mRenderDebugFeatureStack.top();
    mRenderDebugFeatureStack.pop();
}

// static
void LLPipeline::setRenderScriptedBeacons(bool val)
{
    sRenderScriptedBeacons = val;
}

// static
void LLPipeline::toggleRenderScriptedBeacons()
{
    sRenderScriptedBeacons = !sRenderScriptedBeacons;
}

// static
bool LLPipeline::getRenderScriptedBeacons()
{
    return sRenderScriptedBeacons;
}

// static
void LLPipeline::setRenderScriptedTouchBeacons(bool val)
{
    sRenderScriptedTouchBeacons = val;
}

// static
void LLPipeline::toggleRenderScriptedTouchBeacons()
{
    sRenderScriptedTouchBeacons = !sRenderScriptedTouchBeacons;
}

// static
bool LLPipeline::getRenderScriptedTouchBeacons()
{
    return sRenderScriptedTouchBeacons;
}

// static
void LLPipeline::setRenderMOAPBeacons(bool val)
{
    sRenderMOAPBeacons = val;
}

// static
void LLPipeline::toggleRenderMOAPBeacons()
{
    sRenderMOAPBeacons = !sRenderMOAPBeacons;
}

// static
bool LLPipeline::getRenderMOAPBeacons()
{
    return sRenderMOAPBeacons;
}

// static
void LLPipeline::setRenderPhysicalBeacons(bool val)
{
    sRenderPhysicalBeacons = val;
}

// static
void LLPipeline::toggleRenderPhysicalBeacons()
{
    sRenderPhysicalBeacons = !sRenderPhysicalBeacons;
}

// static
bool LLPipeline::getRenderPhysicalBeacons()
{
    return sRenderPhysicalBeacons;
}

// static
void LLPipeline::setRenderParticleBeacons(bool val)
{
    sRenderParticleBeacons = val;
}

// static
void LLPipeline::toggleRenderParticleBeacons()
{
    sRenderParticleBeacons = !sRenderParticleBeacons;
}

// static
bool LLPipeline::getRenderParticleBeacons()
{
    return sRenderParticleBeacons;
}

// static
void LLPipeline::setRenderSoundBeacons(bool val)
{
    sRenderSoundBeacons = val;
}

// static
void LLPipeline::toggleRenderSoundBeacons()
{
    sRenderSoundBeacons = !sRenderSoundBeacons;
}

// static
bool LLPipeline::getRenderSoundBeacons()
{
    return sRenderSoundBeacons;
}

// static
void LLPipeline::setRenderBeacons(bool val)
{
    sRenderBeacons = val;
}

// static
void LLPipeline::toggleRenderBeacons()
{
    sRenderBeacons = !sRenderBeacons;
}

// static
bool LLPipeline::getRenderBeacons()
{
    return sRenderBeacons;
}

// static
void LLPipeline::setRenderHighlights(bool val)
{
    sRenderHighlight = val;
}

// static
void LLPipeline::toggleRenderHighlights()
{
    sRenderHighlight = !sRenderHighlight;
}

// static
bool LLPipeline::getRenderHighlights()
{
    return sRenderHighlight;
}

// static
void LLPipeline::setRenderHighlightTextureChannel(LLRender::eTexIndex channel)
{
    if (channel != sRenderHighlightTextureChannel)
    {
        sRenderHighlightTextureChannel = channel;
    }
}

LLVOPartGroup* LLPipeline::lineSegmentIntersectParticle(const LLVector4a& start, const LLVector4a& end, LLVector4a* intersection,
                                                        S32* face_hit)
{
    LLVector4a local_end = end;

    LLVector4a position;

    LLDrawable* drawable = NULL;

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;

        LLSpatialPartition* part = region->getSpatialPartition(LLViewerRegion::PARTITION_PARTICLE);
        if (part && hasRenderType(part->mDrawableType))
        {
            LLDrawable* hit = part->lineSegmentIntersect(start, local_end, true, false, true, false, face_hit, &position, NULL, NULL, NULL);
            if (hit)
            {
                drawable = hit;
                local_end = position;
            }
        }
    }

    LLVOPartGroup* ret = NULL;
    if (drawable)
    {
        //make sure we're returning an LLVOPartGroup
        llassert(drawable->getVObj()->getPCode() == LLViewerObject::LL_VO_PART_GROUP);
        ret = (LLVOPartGroup*) drawable->getVObj().get();
    }

    if (intersection)
    {
        *intersection = position;
    }

    return ret;
}

LLViewerObject* LLPipeline::lineSegmentIntersectInWorld(const LLVector4a& start, const LLVector4a& end,
                                                        bool pick_transparent,
                                                        bool pick_rigged,
                                                        bool pick_unselectable,
                                                        bool pick_reflection_probe,
                                                        S32* face_hit,
                                                        S32* gltf_node_hit,
                                                        S32* gltf_primitive_hit,
                                                        LLVector4a* intersection,         // return the intersection point
                                                        LLVector2* tex_coord,            // return the texture coordinates of the intersection point
                                                        LLVector4a* normal,               // return the surface normal at the intersection point
                                                        LLVector4a* tangent             // return the surface tangent at the intersection point
    )
{
    LLDrawable* drawable = NULL;

    LLVector4a local_end = end;

    LLVector4a position;

    sPickAvatar = false; //! LLToolMgr::getInstance()->inBuildMode();

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;

        for (U32 j = 0; j < LLViewerRegion::NUM_PARTITIONS; j++)
        {
            if ((j == LLViewerRegion::PARTITION_VOLUME) ||
                (j == LLViewerRegion::PARTITION_BRIDGE) ||
                (j == LLViewerRegion::PARTITION_AVATAR) || // for attachments
                (j == LLViewerRegion::PARTITION_CONTROL_AV) ||
                (j == LLViewerRegion::PARTITION_TERRAIN) ||
                (j == LLViewerRegion::PARTITION_TREE) ||
                (j == LLViewerRegion::PARTITION_GRASS))  // only check these partitions for now
            {
                LLSpatialPartition* part = region->getSpatialPartition(j);
                if (part && hasRenderType(part->mDrawableType))
                {
                    LLDrawable* hit = part->lineSegmentIntersect(start, local_end, pick_transparent, pick_rigged, pick_unselectable, pick_reflection_probe, face_hit, &position, tex_coord, normal, tangent);
                    if (hit)
                    {
                        drawable = hit;
                        local_end = position;
                    }
                }
            }
        }
    }

    if (!sPickAvatar)
    {
        //save hit info in case we need to restore
        //due to attachment override
        LLVector4a local_normal;
        LLVector4a local_tangent;
        LLVector2 local_texcoord;
        S32 local_face_hit = -1;

        if (face_hit)
        {
            local_face_hit = *face_hit;
        }
        if (tex_coord)
        {
            local_texcoord = *tex_coord;
        }
        if (tangent)
        {
            local_tangent = *tangent;
        }
        else
        {
            local_tangent.clear();
        }
        if (normal)
        {
            local_normal = *normal;
        }
        else
        {
            local_normal.clear();
        }

        const F32 ATTACHMENT_OVERRIDE_DIST = 0.1f;

        //check against avatars
        sPickAvatar = true;
        for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
                iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
        {
            LLViewerRegion* region = *iter;

            LLSpatialPartition* part = region->getSpatialPartition(LLViewerRegion::PARTITION_AVATAR);
            if (part && hasRenderType(part->mDrawableType))
            {
                LLDrawable* hit = part->lineSegmentIntersect(start, local_end, pick_transparent, pick_rigged, pick_unselectable, pick_reflection_probe, face_hit, &position, tex_coord, normal, tangent);
                if (hit)
                {
                    LLVector4a delta;
                    delta.setSub(position, local_end);

                    if (!drawable ||
                        !drawable->getVObj()->isAttachment() ||
                        delta.getLength3().getF32() > ATTACHMENT_OVERRIDE_DIST)
                    { //avatar overrides if previously hit drawable is not an attachment or
                      //attachment is far enough away from detected intersection
                        drawable = hit;
                        local_end = position;
                    }
                    else
                    { //prioritize attachments over avatars
                        position = local_end;

                        if (face_hit)
                        {
                            *face_hit = local_face_hit;
                        }
                        if (tex_coord)
                        {
                            *tex_coord = local_texcoord;
                        }
                        if (tangent)
                        {
                            *tangent = local_tangent;
                        }
                        if (normal)
                        {
                            *normal = local_normal;
                        }
                    }
                }
            }
        }
    }

    // check all avatar nametags (silly, isn't it?)
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = (LLVOAvatar*)character;
        if (avatar->mNameText.notNull() &&
            avatar->mNameText->lineSegmentIntersect(start, local_end, position))
        {
            drawable = avatar->mDrawable;
            local_end = position;
        }
    }

    S32 node_hit = -1;
    S32 primitive_hit = -1;
    LLDrawable* hit = LL::GLTFSceneManager::instance().lineSegmentIntersect(start, local_end, pick_transparent, pick_rigged, pick_unselectable, pick_reflection_probe, &node_hit, &primitive_hit, &position, tex_coord, normal, tangent);
    if (hit)
    {
        drawable = hit;
        local_end = position;
    }

    if (gltf_node_hit)
    {
        *gltf_node_hit = node_hit;
    }

    if (gltf_primitive_hit)
    {
        *gltf_primitive_hit = primitive_hit;
    }

    if (intersection)
    {
        *intersection = position;
    }

    return drawable ? drawable->getVObj().get() : NULL;
}

LLViewerObject* LLPipeline::lineSegmentIntersectInHUD(const LLVector4a& start, const LLVector4a& end,
                                                      bool pick_transparent,
                                                      S32* face_hit,
                                                      LLVector4a* intersection,         // return the intersection point
                                                      LLVector2* tex_coord,            // return the texture coordinates of the intersection point
                                                      LLVector4a* normal,               // return the surface normal at the intersection point
                                                      LLVector4a* tangent               // return the surface tangent at the intersection point
    )
{
    LLDrawable* drawable = NULL;

    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
            iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;

        bool toggle = false;
        if (!hasRenderType(LLPipeline::RENDER_TYPE_HUD))
        {
            toggleRenderType(LLPipeline::RENDER_TYPE_HUD);
            toggle = true;
        }

        LLSpatialPartition* part = region->getSpatialPartition(LLViewerRegion::PARTITION_HUD);
        if (part)
        {
            LLDrawable* hit = part->lineSegmentIntersect(start, end, pick_transparent, false, true, false, face_hit, intersection, tex_coord, normal, tangent);
            if (hit)
            {
                drawable = hit;
            }
        }

        if (toggle)
        {
            toggleRenderType(LLPipeline::RENDER_TYPE_HUD);
        }
    }
    return drawable ? drawable->getVObj().get() : NULL;
}

LLSpatialPartition* LLPipeline::getSpatialPartition(LLViewerObject* vobj)
{
    if (vobj)
    {
        LLViewerRegion* region = vobj->getRegion();
        if (region)
        {
            return region->getSpatialPartition(vobj->getPartitionType());
        }
    }
    return NULL;
}

void LLPipeline::resetVertexBuffers(LLDrawable* drawable)
{
    if (!drawable)
    {
        return;
    }

    for (S32 i = 0; i < drawable->getNumFaces(); i++)
    {
        LLFace* facep = drawable->getFace(i);
        if (facep)
        {
            facep->clearVertexBuffer();
        }
    }
}

void LLPipeline::renderObjects(U32 type, bool texture, bool batch_texture, bool rigged)
{
    assertInitialized();
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;

    if (rigged)
    {
        mSimplePool->pushRiggedBatches(type + 1, texture, batch_texture);
    }
    else
    {
        mSimplePool->pushBatches(type, texture, batch_texture);
    }

    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
}

void LLPipeline::renderGLTFObjects(U32 type, bool texture, bool rigged)
{
    assertInitialized();
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;

    if (rigged)
    {
        mSimplePool->pushRiggedGLTFBatches(type + 1, texture);
    }
    else
    {
        mSimplePool->pushGLTFBatches(type, texture);
    }

    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;

    if (!rigged)
    {
        LL::GLTFSceneManager::instance().renderOpaque();
    }
    else
    {
        LL::GLTFSceneManager::instance().render(true, true);
    }
}

// Currently only used for shadows -Cosmic,2023-04-19
void LLPipeline::renderAlphaObjects(bool rigged)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    assertInitialized();
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    S32 sun_up = LLEnvironment::instance().getIsSunUp() ? 1 : 0;
    U32 target_width = plus::shadowshift::ditherWidth(LLRenderTarget::sCurResX); // <plus> plus/shadowshift: the dither on the grid (was: sCurResX) </plus>
    U32 type = LLRenderPass::PASS_ALPHA;
    // for gDeferredShadowAlphaMaskProgram
    const LLVOAvatar* lastAvatar = nullptr;
    U64 lastMeshId = 0;
    bool skipLastSkin;
    // for gDeferredShadowGLTFAlphaBlendProgram
    const LLVOAvatar* lastAvatarGLTF = nullptr;
    U64 lastMeshIdGLTF = 0;
    bool skipLastSkinGLTF;
    auto* begin = gPipeline.beginRenderMap(type);
    auto* end = gPipeline.endRenderMap(type);

    for (LLCullResult::drawinfo_iterator i = begin; i != end; )
    {
        LLDrawInfo* pparams = *i;
        LLCullResult::increment_iterator(i, end);

        if (rigged != (pparams->mAvatar != nullptr))
        {
            // Pool contains both rigged and non-rigged DrawInfos. Only draw
            // the objects we're interested in in this pass.
            continue;
        }

        if (rigged)
        {
            if (!pparams->mGLTFMaterial && mRHINativeAlpha == 1) // LLRHI: drawn natively
            {
                continue;
            }
            LLRHI::SuppressTranslation untranslated(!pparams->mGLTFMaterial && mRHINativeAlpha == 2); // LLRHI
            if (pparams->mGLTFMaterial)
            {
                gDeferredShadowGLTFAlphaBlendProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);
                LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(ALPHA_BLEND_CUTOFF);
                LLRenderPass::pushRiggedGLTFBatch(*pparams, lastAvatarGLTF, lastMeshIdGLTF, skipLastSkinGLTF);
            }
            else
            {
                gDeferredShadowAlphaMaskProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);
                LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(ALPHA_BLEND_CUTOFF);
                if (mSimplePool->uploadMatrixPalette(pparams->mAvatar, pparams->mSkinInfo, lastAvatar, lastMeshId, skipLastSkin))
                {
                    mSimplePool->pushBatch(*pparams, true, true);
                }
            }
        }
        else
        {
            if (pparams->mGLTFMaterial)
            {
                gDeferredShadowGLTFAlphaBlendProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);
                LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(ALPHA_BLEND_CUTOFF);
                LLRenderPass::pushGLTFBatch(*pparams);
            }
            else
            {
                if (mRHINativeAlpha == 1) // LLRHI: drawn natively
                {
                    continue;
                }
                LLRHI::SuppressTranslation untranslated(mRHINativeAlpha == 2); // LLRHI
                gDeferredShadowAlphaMaskProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);
                LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(ALPHA_BLEND_CUTOFF);
                mSimplePool->pushBatch(*pparams, true, true);
            }
        }
    }

    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
}

// Currently only used for shadows -Cosmic,2023-04-19
void LLPipeline::renderMaskedObjects(U32 type, bool texture, bool batch_texture, bool rigged)
{
    assertInitialized();
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    if (rigged)
    {
        mAlphaMaskPool->pushRiggedMaskBatches(type+1, texture, batch_texture);
    }
    else
    {
        mAlphaMaskPool->pushMaskBatches(type, texture, batch_texture);
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
}

// Currently only used for shadows -Cosmic,2023-04-19
void LLPipeline::renderFullbrightMaskedObjects(U32 type, bool texture, bool batch_texture, bool rigged)
{
    assertInitialized();
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
    if (rigged)
    {
        mFullbrightAlphaMaskPool->pushRiggedMaskBatches(type+1, texture, batch_texture);
    }
    else
    {
        mFullbrightAlphaMaskPool->pushMaskBatches(type, texture, batch_texture);
    }
    gGL.loadMatrix(gGLModelView);
    gGLLastMatrix = NULL;
}

void apply_cube_face_rotation(U32 face)
{
    switch (face)
    {
        case 0:
            gGL.rotatef(90.f, 0, 1, 0);
            gGL.rotatef(180.f, 1, 0, 0);
        break;
        case 2:
            gGL.rotatef(-90.f, 1, 0, 0);
        break;
        case 4:
            gGL.rotatef(180.f, 0, 1, 0);
            gGL.rotatef(180.f, 0, 0, 1);
        break;
        case 1:
            gGL.rotatef(-90.f, 0, 1, 0);
            gGL.rotatef(180.f, 1, 0, 0);
        break;
        case 3:
            gGL.rotatef(90, 1, 0, 0);
        break;
        case 5:
            gGL.rotatef(180, 0, 0, 1);
        break;
    }
}

void LLPipeline::bindScreenToTexture()
{

}

static LLTrace::BlockTimerStatHandle FTM_RENDER_BLOOM("Bloom");

void LLPipeline::visualizeBuffers(LLRenderTarget* src, LLRenderTarget* dst, U32 bufferIndex)
{
    dst->bindTarget();
    const LLPassScope native_pass("visualize buffers", dst, { LLPassScope::attachment(src, bufferIndex) }); // LLRHI native (research/137 N12)
    gDeferredBufferVisualProgram.bind();
    gDeferredBufferVisualProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_BILINEAR, bufferIndex);

    static LLStaticHashedString mipLevel("mipLevel");
    if (RenderBufferVisualization != 4)
        gDeferredBufferVisualProgram.uniform1f(mipLevel, 0);
    else
        gDeferredBufferVisualProgram.uniform1f(mipLevel, 8);

    mScreenTriangleVB->setBuffer();
    mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    gDeferredBufferVisualProgram.unbind();
    dst->flush();
}

void LLPipeline::generateLuminance(LLRenderTarget* src, LLRenderTarget* dst)
{
    // luminance sample and mipmap generation
    {
        LL_PROFILE_GPU_ZONE("luminance sample");

        dst->bindTarget();
        const LLPassScope native_pass("luminance", dst, { src, &mGlow[1], LLPassScope::attachment(&mRT->deferredScreen, 2) }); // LLRHI native (research/137 N12)

        LLGLDepthTest depth(GL_FALSE, GL_FALSE);

        gLuminanceProgram.bind();

        static LLCachedControl<F32> diffuse_luminance_scale(gSavedSettings, "RenderDiffuseLuminanceScale", 1.0f);

        S32 channel = 0;
        channel = gLuminanceProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
        if (channel > -1)
        {
            src->bindTexture(0, channel, LLTexUnit::TFO_POINT);
        }

        channel = gLuminanceProgram.enableTexture(LLShaderMgr::DEFERRED_EMISSIVE);
        if (channel > -1)
        {
            mGlow[1].bindTexture(0, channel);
        }

        channel = gLuminanceProgram.enableTexture(LLShaderMgr::NORMAL_MAP);
        if (channel > -1)
        {
            // bind the normal map to get the environment mask
            mRT->deferredScreen.bindTexture(2, channel, LLTexUnit::TFO_POINT);
        }

        static LLStaticHashedString diffuse_luminance_scale_s("diffuse_luminance_scale");
        gLuminanceProgram.uniform1f(diffuse_luminance_scale_s, diffuse_luminance_scale);

        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        dst->flush();

        // note -- unbind AFTER the glGenerateMipMap so time in generatemipmap can be profiled under "Luminance"
        // also note -- keep an eye on the performance of glGenerateMipmap, might need to replace it with a mip generation shader
        gLuminanceProgram.unbind();
    }
}

void LLPipeline::generateExposure(LLRenderTarget* src, LLRenderTarget* dst, bool use_history) {
    // exposure sample
    {
        LL_PROFILE_GPU_ZONE("exposure sample");

        if (use_history)
        {
            // copy last frame's exposure into mLastExposure
            mLastExposure.bindTarget();
            const LLPassScope native_pass("exposure history", &mLastExposure, { dst }); // LLRHI native (research/137 N12)
            gCopyProgram.bind();
            gGL.getTexUnit(0)->bind(dst);

            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            mLastExposure.flush();
        }

        dst->bindTarget();
        const LLPassScope native_pass("exposure", dst, { src, use_history ? &mLastExposure : nullptr }); // LLRHI native (research/137 N12)

        LLGLDepthTest depth(GL_FALSE, GL_FALSE);

        LLGLSLShader* shader;
        if (use_history)
        {
            shader = &gExposureProgram;
        }
        else
        {
            shader = &gExposureProgramNoFade;
        }

        shader->bind();

        S32 channel = shader->enableTexture(LLShaderMgr::DEFERRED_EMISSIVE);
        if (channel > -1)
        {
            src->bindTexture(0, channel, LLTexUnit::TFO_TRILINEAR);
        }

        if (use_history)
        {
            channel = shader->enableTexture(LLShaderMgr::EXPOSURE_MAP);
            if (channel > -1)
            {
                mLastExposure.bindTexture(0, channel);
            }
        }

        static LLStaticHashedString dt("dt");
        static LLStaticHashedString noiseVec("noiseVec");
        static LLStaticHashedString dynamic_exposure_params("dynamic_exposure_params");
        static LLStaticHashedString dynamic_exposure_params2("dynamic_exposure_params2");
        static LLStaticHashedString dynamic_exposure_e("dynamic_exposure_enabled");
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
        static LLCachedControl<bool> dynamic_exposure_enabled(gSavedSettings, "RenderDynamicExposureEnabled", true);
        static LLCachedControl<F32> dynamic_exposure_coefficient(gSavedSettings, "RenderDynamicExposureCoefficient", 0.175f);
        static LLCachedControl<F32> dynamic_exposure_speed_error(gSavedSettings, "RenderDynamicExposureSpeedError", 0.1f);
        static LLCachedControl<F32> dynamic_exposure_speed_target(gSavedSettings, "RenderDynamicExposureSpeedTarget", 2.f);

        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();

        F32 probe_ambiance = LLEnvironment::instance().getCurrentSky()->getReflectionProbeAmbiance(should_auto_adjust());

        F32 exp_min = 1.f;
        F32 exp_max = 1.f;

        static LLCachedControl<bool> use_exposure_sky_settings(gSavedSettings, "RenderUseExposureSkySettings", false);

        if (use_exposure_sky_settings)
        {
            if (dynamic_exposure_enabled)
            {
                exp_min = sky->getHDROffset(should_auto_adjust()) - sky->getHDRMin(should_auto_adjust());
                exp_max = sky->getHDROffset(should_auto_adjust()) + sky->getHDRMax(should_auto_adjust());
            }
            else
            {
                exp_min = sky->getHDROffset(should_auto_adjust());
                exp_max = sky->getHDROffset(should_auto_adjust());
            }
        }
        else if (dynamic_exposure_enabled)
        {
            if (probe_ambiance > 0.f)
            {
                F32 hdr_scale = sqrtf(LLEnvironment::instance().getCurrentSky()->getGamma()) * 2.f;

                if (hdr_scale > 1.f)
                {
                    exp_min = 1.f / hdr_scale;
                    exp_max = hdr_scale;
                }
            }
        }

        shader->uniform1f(dt, gFrameIntervalSeconds);
        shader->uniform2f(noiseVec, ll_frand() * 2.0f - 1.0f, ll_frand() * 2.0f - 1.0f);
        shader->uniform4f(dynamic_exposure_params, dynamic_exposure_coefficient, exp_min, exp_max, dynamic_exposure_speed_error);
        shader->uniform4f(dynamic_exposure_params2, sky->getHDROffset(should_auto_adjust()), exp_min, exp_max, dynamic_exposure_speed_target);

        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        if (use_history)
        {
            gGL.getTexUnit(channel)->unbind(mLastExposure.getUsage());
        }
        shader->unbind();
        dst->flush();
    }
}

extern LLPointer<LLImageGL> gEXRImage;

void LLPipeline::tonemap(LLRenderTarget* src, LLRenderTarget* dst, bool gamma_correct)
{
    LL_PROFILE_GPU_ZONE("tonemap");

    dst->bindTarget();
    const LLPassScope native_pass("tonemap", dst, { src, &mExposureMap }); // LLRHI native (research/137 N12)
    // gamma correct lighting
    {
        static LLCachedControl<bool> buildNoPost(gSavedSettings, "RenderDisablePostProcessing", false);

        LLGLDepthTest depth(GL_FALSE, GL_FALSE);

        // Apply gamma correction to the frame here.

        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);

        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

        bool no_post = gSnapshotNoPost || psky->getReflectionProbeAmbiance(should_auto_adjust) == 0.f || (buildNoPost && gFloaterTools && gFloaterTools->isAvailable());
        LLGLSLShader* shader = nullptr;
        if(gamma_correct)
        {
            bool legacy_gamma = psky->getReflectionProbeAmbiance(should_auto_adjust) == 0.f;
            if(legacy_gamma)
            {
                shader = no_post ? &gNoPostTonemapLegacyGammaCorrectProgram : &gDeferredPostTonemapLegacyGammaCorrectProgram;
            }
            else
            {
                shader = no_post ? &gNoPostTonemapGammaCorrectProgram : &gDeferredPostTonemapGammaCorrectProgram;
            }
        }
        else
        {
            shader = no_post ? &gNoPostTonemapProgram : &gDeferredPostTonemapProgram;
        }

        shader->bind();

        S32 channel = 0;

        shader->bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_POINT);

        shader->bindTexture(LLShaderMgr::EXPOSURE_MAP, &mExposureMap);

        // <plus> plus/glare: the glare composite in the tone mapping (PlusGlareInTonemap, research/125 #7);
        // otherwise its uniforms say there is nothing to add
        plus::glare::bindTonemap(shader);
        // </plus>

        shader->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)src->getWidth(), (GLfloat)src->getHeight());

        static LLCachedControl<F32> exposure(gSavedSettings, "RenderExposure", 1.f);

        F32 e = llclamp(exposure(), 0.5f, 4.f);

        static LLStaticHashedString s_exposure("exposure");
        static LLStaticHashedString tonemap_mix("tonemap_mix");
        static LLStaticHashedString tonemap_type("tonemap_type");

        shader->uniform1f(s_exposure, e);

        static LLCachedControl<U32> tonemap_type_setting(gSavedSettings, "RenderTonemapType", 0U);
        shader->uniform1i(tonemap_type, tonemap_type_setting);
        shader->uniform1f(tonemap_mix, psky->getTonemapMix(should_auto_adjust()));

        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        // <plus> plus/glare: the texture units bindTonemap took
        plus::glare::unbindTonemap();
        // </plus>

        gGL.getTexUnit(channel)->unbind(src->getUsage());
        shader->unbind();
    }
    dst->flush();
}

void LLPipeline::gammaCorrect(LLRenderTarget* src, LLRenderTarget* dst)
{
    LL_PROFILE_GPU_ZONE("gamma correct");

    dst->bindTarget();
    const LLPassScope native_pass("gamma correct", dst, { src }); // LLRHI native (research/137 N12)
    // gamma correct lighting
    {
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);

        static LLCachedControl<bool> buildNoPost(gSavedSettings, "RenderDisablePostProcessing", false);
        static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);

        LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
        LLGLSLShader& shader = psky->getReflectionProbeAmbiance(should_auto_adjust) == 0.f ? gLegacyPostGammaCorrectProgram :
            gDeferredPostGammaCorrectProgram;

        shader.bind();
        shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_POINT);
        shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)src->getWidth(), (GLfloat)src->getHeight());

        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        shader.unbind();
    }
    dst->flush();
}

void LLPipeline::copyScreenSpaceReflections(LLRenderTarget* src, LLRenderTarget* dst)
{

    if (RenderScreenSpaceReflections && !gCubeSnapshot)
    {
        LL_PROFILE_GPU_ZONE("ssr copy");
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

        LLRenderTarget& depth_src = mRT->deferredScreen;

        dst->bindTarget();
        const LLPassScope native_pass("ssr copy", dst, { src, LLPassScope::depth(&depth_src) }); // LLRHI native (research/137 N12)
        dst->clear();
        gCopyDepthProgram.bind();

        S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
        S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);

        gGL.getTexUnit(diff_map)->bind(src);
        gGL.getTexUnit(depth_map)->bind(&depth_src, true);

        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        dst->flush();
    }
}

void LLPipeline::generateGlow(LLRenderTarget* src)
{
    LL_PROFILE_GPU_ZONE("glow generate");
    if (sRenderGlow)
    {
        mGlow[2].bindTarget();
        mGlow[2].clear();

        gGlowExtractProgram.bind();
        F32 maxAlpha = RenderGlowMaxExtractAlpha;
        F32 warmthAmount = RenderGlowWarmthAmount;
        LLVector3 lumWeights = RenderGlowLumWeights;
        LLVector3 warmthWeights = RenderGlowWarmthWeights;

        gGlowExtractProgram.uniform1f(LLShaderMgr::GLOW_MIN_LUMINANCE, 9999);
        gGlowExtractProgram.uniform1f(LLShaderMgr::GLOW_MAX_EXTRACT_ALPHA, maxAlpha);
        gGlowExtractProgram.uniform3f(LLShaderMgr::GLOW_LUM_WEIGHTS, lumWeights.mV[0], lumWeights.mV[1],
            lumWeights.mV[2]);
        gGlowExtractProgram.uniform3f(LLShaderMgr::GLOW_WARMTH_WEIGHTS, warmthWeights.mV[0], warmthWeights.mV[1],
            warmthWeights.mV[2]);
        gGlowExtractProgram.uniform1f(LLShaderMgr::GLOW_WARMTH_AMOUNT, warmthAmount);

        // <plus> plus/taa (research/240): with the scene jittered, each glow texel averages the whole of the picture's
        // pixels it covers (512 wide against ~1920: one tap would hit a thin glowing surface in one frame and miss it in
        // the next, and the wide, smooth glow blinked through TAA's clip). 0: one tap, as before.
        {
            static const LLStaticHashedString plus_glow_footprint("plus_glow_footprint");
            const bool plus_whole = plus::taa::jitteredThisFrame() && mGlow[2].getWidth() && mGlow[2].getHeight();
            gGlowExtractProgram.uniform2f(plus_glow_footprint,
                                          plus_whole ? (F32)src->getWidth() / (F32)mGlow[2].getWidth() : 0.f,
                                          plus_whole ? (F32)src->getHeight() / (F32)mGlow[2].getHeight() : 0.f);
        }
        // </plus>

        if (RenderGlowNoise)
        {
            S32 channel = gGlowExtractProgram.enableTexture(LLShaderMgr::GLOW_NOISE_MAP);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mTrueNoiseMap);
                gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
            }
            gGlowExtractProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                          (GLfloat)mGlow[2].getWidth(),
                                          (GLfloat)mGlow[2].getHeight());
        }

        {
            LLGLEnable blend_on(GL_BLEND);
            const LLPassScope native_pass("glow extract", &mGlow[2], { src, LLPassScope::texture(RenderGlowNoise ? mTrueNoiseMap : 0) }); // LLRHI native (research/137 N12)

            gGL.setSceneBlendType(LLRender::BT_ADD_WITH_ALPHA);

            gGlowExtractProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, src);

            gGL.color4f(1, 1, 1, 1);
            gPipeline.enableLightsFullbright();

            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            mGlow[2].flush();
        }

        gGlowExtractProgram.unbind();

        // power of two between 1 and 1024
        U32 glowResPow = RenderGlowResolutionPow;
        const U32 glow_res = llmax(1, llmin(1024, 1 << glowResPow));

        S32 kernel = RenderGlowIterations * 2;
        F32 delta = RenderGlowWidth / glow_res;
        // Use half the glow width if we have the res set to less than 9 so that it looks
        // almost the same in either case.
        if (glowResPow < 9)
        {
            delta *= 0.5f;
        }
        F32 strength = RenderGlowStrength;

        gGlowProgram.bind();
        gGlowProgram.uniform1f(LLShaderMgr::GLOW_STRENGTH, strength);

        for (S32 i = 0; i < kernel; i++)
        {
            mGlow[i % 2].bindTarget();
            const LLPassScope native_pass("glow blur", &mGlow[i % 2], { i == 0 ? &mGlow[2] : &mGlow[(i - 1) % 2] }); // LLRHI native (research/137 N12)
            mGlow[i % 2].clear();

            if (i == 0)
            {
                gGlowProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, &mGlow[2]);
            }
            else
            {
                gGlowProgram.bindTexture(LLShaderMgr::DIFFUSE_MAP, &mGlow[(i - 1) % 2]);
            }

            if (i % 2 == 0)
            {
                gGlowProgram.uniform2f(LLShaderMgr::GLOW_DELTA, delta, 0);
            }
            else
            {
                gGlowProgram.uniform2f(LLShaderMgr::GLOW_DELTA, 0, delta);
            }

            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            mGlow[i % 2].flush();
        }

        gGlowProgram.unbind();

    }
    else // !sRenderGlow, skip the glow ping-pong and just clear the result target
    {
        mGlow[1].bindTarget();
        mGlow[1].clear();
        mGlow[1].flush();
    }
}

void LLPipeline::applyCAS(LLRenderTarget* src, LLRenderTarget* dst)
{
    static LLCachedControl<F32> cas_sharpness(gSavedSettings, "RenderCASSharpness", 0.4f);
    LL_PROFILE_GPU_ZONE("cas");
    if (cas_sharpness == 0.0f || !gCASProgram.isComplete() || !gCASLegacyGammaProgram.isComplete())
    {
        gPipeline.copyRenderTarget(src, dst);
        return;
    }

    LLGLSLShader* sharpen_shader = &gCASProgram;
    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);

    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    bool legacy_gamma = psky->getReflectionProbeAmbiance(should_auto_adjust) == 0.f;
    if(legacy_gamma)
    {
        sharpen_shader = &gCASLegacyGammaProgram;
    }

    // Bind setup:
    dst->bindTarget();
    const LLPassScope native_pass("cas", dst, { src }); // LLRHI native (research/137 N12)

    sharpen_shader->bind();

    {
        static LLStaticHashedString cas_param_0("cas_param_0");
        static LLStaticHashedString cas_param_1("cas_param_1");
        static LLStaticHashedString out_screen_res("out_screen_res");

        varAU4(const0);
        varAU4(const1);
        CasSetup(const0, const1,
            cas_sharpness(),             // Sharpness tuning knob (0.0 to 1.0).
            (AF1)src->getWidth(), (AF1)src->getHeight(),  // Input size.
            (AF1)dst->getWidth(), (AF1)dst->getHeight()); // Output size.

        sharpen_shader->uniform4uiv(cas_param_0, 1, const0);
        sharpen_shader->uniform4uiv(cas_param_1, 1, const1);

        sharpen_shader->uniform2f(out_screen_res, (AF1)dst->getWidth(), (AF1)dst->getHeight());
    }

    sharpen_shader->bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_POINT);

    // Draw
    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

    sharpen_shader->unbind();

    dst->flush();
}

void LLPipeline::applyFXAA(LLRenderTarget* src, LLRenderTarget* dst)
{
    LL_PROFILE_GPU_ZONE("FXAA");
    {
        llassert(!gCubeSnapshot);
        bool multisample = plus::upscale::effectiveAAType(RenderFSAAType) == 1 && gFXAAProgram[0].isComplete() && mFXAAMap.isComplete(); // <plus> plus/upscale: at least FXAA while upscaled (was: RenderFSAAType == 1) </plus>

        // Present everything.
        if (multisample)
        {
            LL_PROFILE_GPU_ZONE("aa");
            S32 width = dst->getWidth();
            S32 height = dst->getHeight();

            // bake out texture2D with RGBL for FXAA shader
            mFXAAMap.bindTarget();
            const LLPassScope native_pass("fxaa luma", &mFXAAMap, { src }); // LLRHI native (research/137 N12)
            mFXAAMap.clear(GL_COLOR_BUFFER_BIT);

            LLGLSLShader* shader = &gGlowCombineFXAAProgram;
            shader->bind();

            S32 channel = shader->enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
            if (channel > -1)
            {
                src->bindTexture(0, channel, LLTexUnit::TFO_BILINEAR);
            }

            {
                LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_ALWAYS);
                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            }

            shader->disableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
            shader->unbind();

            mFXAAMap.flush();
            native_pass.close(); // LLRHI native (research/137 N12) fix-round6 3

            dst->bindTarget();
            const LLPassScope native_pass2("fxaa", dst, { &mFXAAMap, LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)

            static LLCachedControl<U32> aa_quality(gSavedSettings, "RenderFSAASamples", 0U);
            U32 fsaa_quality = std::clamp(aa_quality(), 0U, 3U);

            shader = &gFXAAProgram[fsaa_quality];
            shader->bind();

            channel = shader->enableTexture(LLShaderMgr::DIFFUSE_MAP, mFXAAMap.getUsage());
            if (channel > -1)
            {
                mFXAAMap.bindTexture(0, channel, LLTexUnit::TFO_BILINEAR);
            }

            gGLViewport[0] = gViewerWindow->getWorldViewRectRaw().mLeft;
            gGLViewport[1] = gViewerWindow->getWorldViewRectRaw().mBottom;
            gGLViewport[2] = gViewerWindow->getWorldViewRectRaw().getWidth();
            gGLViewport[3] = gViewerWindow->getWorldViewRectRaw().getHeight();

            // <plus> fxaa-origin: dst is a target of its own (the world's size, or the drawn size while upscaled), so draw
            // from its corner. The world's rect has an origin in the photo mode's detailed view, and drawing at that origin
            // left the picture shifted inside dst with the frame before FXAA showing in the rest (a double image). The four
            // gGLViewport lines above stay: dst->flush() goes back to the window with them, for the present pass.
            // (was: LLRHI::viewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]), and (0, 0) only while upscaled)
            LLRHI::viewport(0, 0, width, height);
            // </plus>

            F32 scale_x = (F32)width / mFXAAMap.getWidth();
            F32 scale_y = (F32)height / mFXAAMap.getHeight();
            shader->uniform2f(LLShaderMgr::FXAA_TC_SCALE, scale_x, scale_y);
            shader->uniform2f(LLShaderMgr::FXAA_RCP_SCREEN_RES, 1.f / width * scale_x, 1.f / height * scale_y);
            shader->uniform4f(LLShaderMgr::FXAA_RCP_FRAME_OPT, -0.5f / width * scale_x, -0.5f / height * scale_y,
                0.5f / width * scale_x, 0.5f / height * scale_y);
            shader->uniform4f(LLShaderMgr::FXAA_RCP_FRAME_OPT2, -2.f / width * scale_x, -2.f / height * scale_y,
                2.f / width * scale_x, 2.f / height * scale_y);

            {
                LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_ALWAYS);
                S32 depth_channel = shader->getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
                gGL.getTexUnit(depth_channel)->bind(&mRT->deferredScreen, true);

                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            }

            shader->unbind();
            dst->flush();
        }
        else {
            copyRenderTarget(src, dst);
        }
    }
}

void LLPipeline::generateSMAABuffers(LLRenderTarget* src)
{
    llassert(!gCubeSnapshot);
    bool multisample = RenderFSAAType == 2 && gSMAAEdgeDetectProgram[0].isComplete() && mFXAAMap.isComplete() && mSMAABlendBuffer.isComplete();

    // Present everything.
    if (multisample)
    {
        LL_PROFILE_GPU_ZONE("SMAA Edge");
        static LLCachedControl<U32> aa_quality(gSavedSettings, "RenderFSAASamples", 0U);
        U32 fsaa_quality = std::clamp(aa_quality(), 0U, 3U);

        S32 width = src->getWidth();
        S32 height = src->getHeight();

        float rt_metrics[] = { 1.f / width, 1.f / height, (float)width, (float)height };

        LLGLDepthTest    depth(GL_FALSE, GL_FALSE);

        static LLCachedControl<bool> use_sample(gSavedSettings, "RenderSMAAUseSample", false);
        //static LLCachedControl<bool> use_stencil(gSavedSettings, "RenderSMAAUseStencil", true);
        {
            //LLGLState stencil(GL_STENCIL_TEST, use_stencil);

            // Bind setup:
            LLRenderTarget& dest = mFXAAMap;
            LLGLSLShader& edge_shader = gSMAAEdgeDetectProgram[fsaa_quality];

            dest.bindTarget();
            const LLPassScope native_pass("smaa edges", &dest, { use_sample ? LLPassScope::texture(mSMAASampleMap) : LLPassScope::Input(src) }); // LLRHI native (research/137 N12)
            dest.clear(GL_COLOR_BUFFER_BIT);

            edge_shader.bind();
            edge_shader.uniform4fv(sSmaaRTMetrics, 1, rt_metrics);

            S32 channel = edge_shader.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
            if (channel > -1)
            {
                if (!use_sample)
                {
                    src->bindTexture(0, channel, LLTexUnit::TFO_BILINEAR);
                }
                else
                {
                    gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mSMAASampleMap);
                    gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
                }
                gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }

            //if (use_stencil)
            //{
            //    glStencilFunc(GL_ALWAYS, 1, 0xFF);
            //    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            //    glStencilMask(0xFF);
            //}
            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            edge_shader.unbind();
            dest.flush();

            gGL.getTexUnit(channel)->unbindFast(LLTexUnit::TT_TEXTURE);
        }

        {
            //LLGLState stencil(GL_STENCIL_TEST, use_stencil);

            // Bind setup:
            LLRenderTarget& dest = mSMAABlendBuffer;
            LLGLSLShader& blend_weights_shader = gSMAABlendWeightsProgram[fsaa_quality];

            dest.bindTarget();
            const LLPassScope native_pass("smaa weights", &dest, { &mFXAAMap, LLPassScope::texture(mSMAAAreaMap), LLPassScope::texture(mSMAASearchMap) }); // LLRHI native (research/137 N12)
            dest.clear(GL_COLOR_BUFFER_BIT);

            blend_weights_shader.bind();
            blend_weights_shader.uniform4fv(sSmaaRTMetrics, 1, rt_metrics);

            S32 edge_tex_channel = blend_weights_shader.enableTexture(LLShaderMgr::SMAA_EDGE_TEX, mFXAAMap.getUsage());
            if (edge_tex_channel > -1)
            {
                mFXAAMap.bindTexture(0, edge_tex_channel, LLTexUnit::TFO_BILINEAR);
                gGL.getTexUnit(edge_tex_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
            S32 area_tex_channel = blend_weights_shader.enableTexture(LLShaderMgr::SMAA_AREA_TEX, LLTexUnit::TT_TEXTURE);
            if (area_tex_channel > -1)
            {
                gGL.getTexUnit(area_tex_channel)->bindManual(LLTexUnit::TT_TEXTURE, mSMAAAreaMap);
                gGL.getTexUnit(area_tex_channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
                gGL.getTexUnit(area_tex_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }
            S32 search_tex_channel = blend_weights_shader.enableTexture(LLShaderMgr::SMAA_SEARCH_TEX, LLTexUnit::TT_TEXTURE);
            if (search_tex_channel > -1)
            {
                gGL.getTexUnit(search_tex_channel)->bindManual(LLTexUnit::TT_TEXTURE, mSMAASearchMap);
                gGL.getTexUnit(search_tex_channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
                gGL.getTexUnit(search_tex_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }

            //if (use_stencil)
            //{
            //    glStencilFunc(GL_EQUAL, 1, 0xFF);
            //    glStencilMask(0x00);
            //}
            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            //if (use_stencil)
            //{
            //    glStencilFunc(GL_ALWAYS, 0, 0xFF);
            //}
            blend_weights_shader.unbind();
            dest.flush();
            gGL.getTexUnit(edge_tex_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
            gGL.getTexUnit(area_tex_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
            gGL.getTexUnit(search_tex_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
        }
    }
}

void LLPipeline::applySMAA(LLRenderTarget* src, LLRenderTarget* dst)
{
    LL_PROFILE_GPU_ZONE("SMAA");
    llassert(!gCubeSnapshot);
    bool multisample = RenderFSAAType == 2 && gSMAAEdgeDetectProgram[0].isComplete() && mFXAAMap.isComplete() && mSMAABlendBuffer.isComplete();

    // Present everything.
    if (multisample)
    {
        static LLCachedControl<U32> aa_quality(gSavedSettings, "RenderFSAASamples", 0U);
        U32 fsaa_quality = std::clamp(aa_quality(), 0U, 3U);

        S32 width = src->getWidth();
        S32 height = src->getHeight();

        float rt_metrics[] = { 1.f / width, 1.f / height, (float)width, (float)height };

        LLGLDepthTest    depth(GL_FALSE, GL_FALSE);

        static LLCachedControl<bool> use_sample(gSavedSettings, "RenderSMAAUseSample", false);
        //static LLCachedControl<bool> use_stencil(gSavedSettings, "RenderSMAAUseStencil", true);

        {
            //LLGLDisable stencil(GL_STENCIL_TEST);

            // Bind setup:
            LLRenderTarget* bound_target = dst;
            LLGLSLShader& blend_shader = gSMAANeighborhoodBlendProgram[fsaa_quality];

            bound_target->bindTarget();
            const LLPassScope native_pass("smaa blend", bound_target, { src, &mSMAABlendBuffer }); // LLRHI native (research/137 N12)
            bound_target->clear(GL_COLOR_BUFFER_BIT);

            blend_shader.bind();
            blend_shader.uniform4fv(sSmaaRTMetrics, 1, rt_metrics);

            S32 diffuse_channel = blend_shader.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
            if(diffuse_channel > -1)
            {
                src->bindTexture(0, diffuse_channel, LLTexUnit::TFO_BILINEAR);
                gGL.getTexUnit(diffuse_channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
            }

            S32 blend_channel = blend_shader.enableTexture(LLShaderMgr::SMAA_BLEND_TEX);
            if (blend_channel > -1)
            {
                mSMAABlendBuffer.bindTexture(0, blend_channel, LLTexUnit::TFO_BILINEAR);
            }

            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            bound_target->flush();
            blend_shader.unbind();
            gGL.getTexUnit(diffuse_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
            gGL.getTexUnit(blend_channel)->unbindFast(LLTexUnit::TT_TEXTURE);
        }
    }
    else
    {
        copyRenderTarget(src, dst);
    }
}

void LLPipeline::copyRenderTarget(LLRenderTarget* src, LLRenderTarget* dst)
{

    LL_PROFILE_GPU_ZONE("copyRenderTarget");
    dst->bindTarget();
    const LLPassScope native_pass("copy", dst, { src, LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)

    gDeferredPostNoDoFProgram.bind();

    gDeferredPostNoDoFProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src);
    gDeferredPostNoDoFProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mRT->deferredScreen, true);

    {
        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gDeferredPostNoDoFProgram.unbind();

    dst->flush();
}

void LLPipeline::combineGlow(LLRenderTarget* src, LLRenderTarget* dst)
{
    LL_PROFILE_GPU_ZONE("glow combine");

    // Go ahead and do our glow combine here in our destination.  We blit this later into the front buffer.
    dst->bindTarget();
    const LLPassScope native_pass("glow combine", dst, { src, &mGlow[1] }); // LLRHI native (research/137 N12)

    {

        gGlowCombineProgram.bind();

        gGlowCombineProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src);
        gGlowCombineProgram.bindTexture(LLShaderMgr::DEFERRED_EMISSIVE, &mGlow[1]);

        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    dst->flush();
}

void LLPipeline::renderDoF(LLRenderTarget* src, LLRenderTarget* dst)
{
    LL_PROFILE_GPU_ZONE("dof");
    {
        bool dof_enabled =
            (RenderDepthOfFieldInEditMode || !LLToolMgr::getInstance()->inBuildMode()) &&
            RenderDepthOfField &&
            !gCubeSnapshot;

        gViewerWindow->setup3DViewport();

        if (dof_enabled)
        {
            LLGLDisable blend(GL_BLEND);

            // depth of field focal plane calculations
            static F32 current_distance = 16.f;
            static F32 start_distance = 16.f;
            static F32 transition_time = 1.f;

            LLVector3 focus_point;

            LLViewerObject* obj = LLViewerMediaFocus::getInstance()->getFocusedObject();
            if (obj && obj->mDrawable && obj->isSelected())
            { // focus on selected media object
                S32 face_idx = LLViewerMediaFocus::getInstance()->getFocusedFace();
                if (obj && obj->mDrawable)
                {
                    LLFace* face = obj->mDrawable->getFace(face_idx);
                    if (face)
                    {
                        focus_point = face->getPositionAgent();
                    }
                }
            }

            // <plus> research/218: focus lock (a face or the cursor), after the selected media and before the official sources
            if (focus_point.isExactlyZero())
            {
                plus::focuslock::focusPoint(focus_point);
            }
            // </plus>

            if (focus_point.isExactlyZero())
            {
                if (gAgent.isCameraExternallyDriven())
                { // focus on point under cursor
                    focus_point.set(gDebugRaycastIntersection.getF32ptr());
                }
                else if (gAgentCamera.cameraMouselook())
                { // focus on point under mouselook crosshairs
                    LLVector4a result;
                    result.clear();

                    gViewerWindow->cursorIntersect(-1, -1, 512.f, nullptr, -1, false, false, true, true, nullptr, nullptr, nullptr, &result);

                    focus_point.set(result.getF32ptr());
                }
                else
                {
                    // focus on alt-zoom target
                    LLViewerRegion* region = gAgent.getRegion();
                    if (region)
                    {
                        focus_point = LLVector3(gAgentCamera.getFocusGlobal() - region->getOriginGlobal());
                    }
                }
            }

            LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
            F32 target_distance = 16.f;
            if (!focus_point.isExactlyZero())
            {
                target_distance = LLViewerCamera::getInstance()->getAtAxis() * (focus_point - eye);
            }

            if (transition_time >= 1.f && fabsf(current_distance - target_distance) / current_distance > 0.01f)
            { // large shift happened, interpolate smoothly to new target distance
                transition_time = 0.f;
                start_distance = current_distance;
            }
            else if (transition_time < 1.f)
            { // currently in a transition, continue interpolating
                transition_time += 1.f / CameraFocusTransitionTime * gFrameIntervalSeconds.value();
                transition_time = llmin(transition_time, 1.f);

                F32 t = cosf(transition_time * F_PI + F_PI) * 0.5f + 0.5f;
                current_distance = start_distance + (target_distance - start_distance) * t;
            }
            else
            { // small or no change, just snap to target distance
                current_distance = target_distance;
            }

            // convert to mm
            F32 subject_distance = current_distance * 1000.f;
            F32 fnumber = CameraFNumber;
            F32 default_focal_length = CameraFocalLength;

            F32 fov = LLViewerCamera::getInstance()->getView();

            const F32 default_fov = CameraFieldOfView * F_PI / 180.f;

            // F32 aspect_ratio = (F32) mRT->screen.getWidth()/(F32)mRT->screen.getHeight();

            F32 dv = 2.f * default_focal_length * tanf(default_fov / 2.f);

            F32 focal_length = dv / (2 * tanf(fov / 2.f));

            // F32 tan_pixel_angle = tanf(LLDrawable::sCurPixelAngle);

            // from wikipedia -- c = |s2-s1|/s2 * f^2/(N(S1-f))
            // where     N = fnumber
            //           s2 = dot distance
            //           s1 = subject distance
            //           f = focal length
            //

            F32 blur_constant = focal_length * focal_length / (fnumber * (subject_distance - focal_length));
            blur_constant /= 1000.f; // convert to meters for shader
            F32 magnification = focal_length / (subject_distance - focal_length);

            { // build diffuse+bloom+CoF
                mRT->deferredLight.bindTarget();
                const LLPassScope native_pass("dof cof", &mRT->deferredLight, { src, LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)

                gDeferredCoFProgram.bind();

                gDeferredCoFProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, LLTexUnit::TFO_POINT);
                gDeferredCoFProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mRT->deferredScreen, true);

                gDeferredCoFProgram.uniform1f(LLShaderMgr::DEFERRED_DEPTH_CUTOFF, RenderEdgeDepthCutoff);
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DEFERRED_NORM_CUTOFF, RenderEdgeNormCutoff);
                gDeferredCoFProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DOF_FOCAL_DISTANCE, -subject_distance / 1000.f);
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DOF_BLUR_CONSTANT, blur_constant);
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DOF_TAN_PIXEL_ANGLE, tanf(1.f / LLDrawable::sCurPixelAngle));
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DOF_MAGNIFICATION, magnification);
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DOF_MAX_COF, CameraMaxCoF);
                gDeferredCoFProgram.uniform1f(LLShaderMgr::DOF_RES_SCALE, CameraDoFResScale);

                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                gDeferredCoFProgram.unbind();
                mRT->deferredLight.flush();
            }

            U32 dof_width = (U32)(mRT->screen.getWidth() * CameraDoFResScale);
            U32 dof_height = (U32)(mRT->screen.getHeight() * CameraDoFResScale);

            { // perform DoF sampling at half-res (preserve alpha channel)
                src->bindTarget();
                const LLPassScope native_pass("dof sample", src, { &mRT->deferredLight }); // LLRHI native (research/137 N12)
                LLRHI::viewport(0, 0, dof_width, dof_height);

                gGL.setColorMask(true, false);

                gDeferredPostProgram.bind();
                gDeferredPostProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, &mRT->deferredLight, LLTexUnit::TFO_POINT);

                gDeferredPostProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());
                gDeferredPostProgram.uniform1f(LLShaderMgr::DOF_MAX_COF, CameraMaxCoF);
                gDeferredPostProgram.uniform1f(LLShaderMgr::DOF_RES_SCALE, CameraDoFResScale);

                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

                gDeferredPostProgram.unbind();

                src->flush();
                gGL.setColorMask(true, true);
            }

            { // combine result based on alpha

                dst->bindTarget();
                const LLPassScope native_pass("dof combine", dst, { src, &mRT->deferredLight }); // LLRHI native (research/137 N12)
                LLRHI::viewport(0, 0, dst->getWidth(), dst->getHeight());

                gDeferredDoFCombineProgram.bind();
                gDeferredDoFCombineProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, LLTexUnit::TFO_POINT);
                gDeferredDoFCombineProgram.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &mRT->deferredLight, LLTexUnit::TFO_POINT);

                gDeferredDoFCombineProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)dst->getWidth(), (GLfloat)dst->getHeight());
                gDeferredDoFCombineProgram.uniform1f(LLShaderMgr::DOF_MAX_COF, CameraMaxCoF);
                gDeferredDoFCombineProgram.uniform1f(LLShaderMgr::DOF_RES_SCALE, CameraDoFResScale);
                gDeferredDoFCombineProgram.uniform1f(LLShaderMgr::DOF_WIDTH, (dof_width - 1) / (F32)src->getWidth());
                gDeferredDoFCombineProgram.uniform1f(LLShaderMgr::DOF_HEIGHT, (dof_height - 1) / (F32)src->getHeight());

                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

                gDeferredDoFCombineProgram.unbind();

                dst->flush();
            }
        }
        else
        {
            copyRenderTarget(src, dst);
        }
    }
}

void LLPipeline::renderFinalize()
{
    llassert(!gCubeSnapshot);
    LLVertexBuffer::unbind();
    LLGLState::checkStates();

    assertInitialized();

    LL_RECORD_BLOCK_TIME(FTM_RENDER_BLOOM);
    LL_PROFILE_GPU_ZONE("renderFinalize");
    // <plus> ground fog (plus/atmosphere): laid over mRT->screen first, unless it went in before the alpha
    plus::atmosphere::renderFog();
    // </plus>
    // <plus> volumetric light (plus/volumetric): adds the light scattered by the air to mRT->screen before tone mapping
    plus::volumetric::render();
    // </plus>
    LLRHI::gpuMark("tonemap");

    gGL.color4f(1, 1, 1, 1);
    LLGLDepthTest depth(GL_FALSE);
    LLGLDisable blend(GL_BLEND);
    LLGLDisable cull(GL_CULL_FACE);

    enableLightsFullbright();

    gGL.setColorMask(true, true);
    LLRHI::clearColor(0, 0, 0, 0);

    static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
    bool hdr = gGLManager.mGLVersion > 4.05f && has_hdr();
    if (hdr)
    {
        copyScreenSpaceReflections(&mRT->screen, &mSceneMap);

        generateLuminance(&mRT->screen, &mLuminanceMap);

        generateExposure(&mLuminanceMap, &mExposureMap);

        // <plus> rain (plus/rain): streaks and splashes laid over mRT->screen after the exposure is measured
        plus::rain::renderStreaks();
        // </plus>

        // <plus> fireflies (plus/atmosphere): added to mRT->screen after the exposure is measured, before the glare
        plus::atmosphere::renderFireflies();
        // </plus>

        // <plus> glare and lens flare (plus/glare): added to mRT->screen after the exposure is measured, before tone mapping
        plus::glare::render();
        // </plus>

        static LLCachedControl<F32> cas_sharpness(gSavedSettings, "RenderCASSharpness", 0.4f);
        // <plus> plus/upscale: FSR 1's RCAS sharpens instead while upscaled (research/165 stage 2; was: without the last term)
        bool apply_cas = cas_sharpness != 0.0f && gCASProgram.isComplete() && gCASLegacyGammaProgram.isComplete()
                         && !plus::upscale::replacesCAS();
        // </plus>

        tonemap(&mRT->screen, apply_cas ? &mRT->deferredLight : &mPostPingMap, !apply_cas);

        if (apply_cas)
        {
            // Gamma Corrects
            applyCAS(&mRT->deferredLight, &mPostPingMap);
        }
    }
    else
    {
        // <plus> glare (plus/glare): drawn only with HDR, so its targets go while it is off
        plus::glare::releaseWithoutHDR();
        // </plus>
        gammaCorrect(&mRT->screen, &mPostPingMap);
    }

    LLVertexBuffer::unbind();

    LLRHI::gpuMark("bloom");
    generateGlow(&mPostPingMap);

    LLRenderTarget* sourceBuffer = &mPostPingMap;
    LLRenderTarget* targetBuffer = &mPostPongMap;

    combineGlow(sourceBuffer, targetBuffer);
    std::swap(sourceBuffer, targetBuffer);

    gGLViewport[0] = gViewerWindow->getWorldViewRectRaw().mLeft;
    gGLViewport[1] = gViewerWindow->getWorldViewRectRaw().mBottom;
    gGLViewport[2] = gViewerWindow->getWorldViewRectRaw().getWidth();
    gGLViewport[3] = gViewerWindow->getWorldViewRectRaw().getHeight();
    LLRHI::viewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);

    if((RenderDepthOfFieldInEditMode || !LLToolMgr::getInstance()->inBuildMode()) &&
        RenderDepthOfField &&
        !gCubeSnapshot)
    {
        LLRHI::gpuMark("dof");
        renderDoF(sourceBuffer, targetBuffer);
        std::swap(sourceBuffer, targetBuffer);
    }

    LLRHI::gpuMark("aa");
    const U32 plus_aa_type = plus::upscale::effectiveAAType(RenderFSAAType); // <plus> plus/upscale: at least FXAA while upscaled (was: RenderFSAAType below) </plus>
     if (plus_aa_type == 1) // <plus> plus/upscale (was: RenderFSAAType == 1) </plus>
    {
        applyFXAA(sourceBuffer, targetBuffer);
        std::swap(sourceBuffer, targetBuffer);
    }
    else if (plus_aa_type == 2) // <plus> plus/upscale (was: RenderFSAAType == 2) </plus>
    {
        generateSMAABuffers(sourceBuffer);
        applySMAA(sourceBuffer, targetBuffer);
        std::swap(sourceBuffer, targetBuffer);
    }

    // <plus> plus/taa (research/185 §13): temporal anti-aliasing after FXAA/SMAA, which smooth the edges in the frame first;
    // TAA then evens them out over the frames. When it drew, source and target name its result and a spare.
    plus::taa::render(sourceBuffer, targetBuffer);
    // </plus>

    // <plus> upscale (plus/upscale, research/165): the drawn picture to the window's size (FSR 1 EASU, RCAS); from here on
    // (the look, the final pass, the HUD, the UI) everything is at the window's size. source and target then name it and a spare.
    plus::upscale::render(sourceBuffer, targetBuffer);
    // </plus>

    // <plus> look (plus/look): the colour grading LUT on the finished image, before the HUD and the UI
    if (plus::look::render(sourceBuffer, targetBuffer))
    {
        std::swap(sourceBuffer, targetBuffer);
    }
    // </plus>

    if (RenderBufferVisualization > -1)
    {
        switch (RenderBufferVisualization)
        {
        case 0:
        case 1:
        case 2:
        case 3:
            visualizeBuffers(&mRT->deferredScreen, sourceBuffer, RenderBufferVisualization);
            break;
        case 4:
            visualizeBuffers(&mLuminanceMap, sourceBuffer, 0);
            break;
        case 5:
        {
            if (RenderFSAAType > 0)
            {
                visualizeBuffers(&mFXAAMap, sourceBuffer, 0);
            }
            break;
        }
        case 6:
        {
            if (RenderFSAAType == 2)
            {
                visualizeBuffers(&mSMAABlendBuffer, sourceBuffer, 0);
            }
            break;
        }
        default:
            break;
        }
    }

    // Present the screen target.

    LLRHI::gpuMark("post");
    gDeferredPostNoDoFNoiseProgram.bind(); // Add noise as part of final render to screen pass to avoid damaging other post effects

    // Whatever is last in the above post processing chain should _always_ be rendered directly here.  If not, expect problems.
    gDeferredPostNoDoFNoiseProgram.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, sourceBuffer);
    gDeferredPostNoDoFNoiseProgram.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &mRT->deferredScreen, true);

    gDeferredPostNoDoFNoiseProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)sourceBuffer->getWidth(), (GLfloat)sourceBuffer->getHeight());
    // <plus> plus/taa (select-flicker): the depth for the HUD and UI at the unjittered pixel centres (0: as it is)
    {
        static LLStaticHashedString sDepthUnjitter("depth_unjitter");
        const glm::vec2 jitter = plus::taa::depthJitter();
        gDeferredPostNoDoFNoiseProgram.uniform2f(sDepthUnjitter, jitter.x, jitter.y);
    }
    // </plus>

    {
        LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_ALWAYS);
        const LLPassScope native_pass("present", LLRenderTarget::getCurrentBoundTarget(), { sourceBuffer, LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)
        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    }

    gDeferredPostNoDoFNoiseProgram.unbind();

    gGL.setSceneBlendType(LLRender::BT_ALPHA);

    if (hasRenderDebugMask(LLPipeline::RENDER_DEBUG_PHYSICS_SHAPES))
    {
        renderPhysicsDisplay();
    }

    /*if (LLRenderTarget::sUseFBO && !gCubeSnapshot)
    { // copy depth buffer from mRT->screen to framebuffer
        LLRenderTarget::copyContentsToFramebuffer(mRT->screen, 0, 0, mRT->screen.getWidth(), mRT->screen.getHeight(), 0, 0,
                                                  mRT->screen.getWidth(), mRT->screen.getHeight(),
                                                  GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT, GL_NEAREST);
    }*/

    LLVertexBuffer::unbind();

    LLGLState::checkStates();

    // flush calls made to "addTrianglesDrawn" so far to stats machinery
    recordTrianglesDrawn();
}

void LLPipeline::bindLightFunc(LLGLSLShader& shader)
{
    S32 channel = shader.enableTexture(LLShaderMgr::DEFERRED_LIGHTFUNC);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mLightFunc);
    }

    channel = shader.enableTexture(LLShaderMgr::DEFERRED_BRDF_LUT, LLTexUnit::TT_TEXTURE);
    if (channel > -1)
    {
        mPbrBrdfLut.bindTexture(0, channel);
    }
}

void LLPipeline::bindShadowMaps(LLGLSLShader& shader)
{
    for (U32 i = 0; i < 4; i++)
    {
        LLRenderTarget* shadow_target = getSunShadowTarget(i);
        if (shadow_target)
        {
            S32 channel = shader.enableTexture(LLShaderMgr::DEFERRED_SHADOW0 + i, LLTexUnit::TT_TEXTURE);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->bind(getSunShadowTarget(i), true);
            }
        }
    }

    for (U32 i = 4; i < 6; i++)
    {
        S32 channel = shader.enableTexture(LLShaderMgr::DEFERRED_SHADOW0 + i);
        if (channel > -1)
        {
            LLRenderTarget* shadow_target = getSpotShadowTarget(i - 4);
            if (shadow_target)
            {
                gGL.getTexUnit(channel)->bind(shadow_target, true);
            }
        }
    }
}

void LLPipeline::bindDeferredShaderFast(LLGLSLShader& shader)
{
    if (shader.mCanBindFast)
    { // was previously fully bound, use fast path
        shader.bind();
        bindLightFunc(shader);
        bindShadowMaps(shader);
        bindReflectionProbes(shader);
    }
    else
    { //wasn't previously bound, use slow path
        bindDeferredShader(shader);
        shader.mCanBindFast = true;
    }
}

void LLPipeline::bindDeferredShader(LLGLSLShader& shader, LLRenderTarget* light_target, LLRenderTarget* depth_target)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LLRenderTarget* deferred_target       = &mRT->deferredScreen;
    LLRenderTarget* deferred_light_target = &mRT->deferredLight;

    shader.bind();
    S32 channel = 0;
    channel = shader.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE, deferred_target->getUsage());
    if (channel > -1)
    {
        deferred_target->bindTexture(0,channel, LLTexUnit::TFO_POINT); // frag_data[0]
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    channel = shader.enableTexture(LLShaderMgr::DEFERRED_SPECULAR, deferred_target->getUsage());
    if (channel > -1)
    {
        deferred_target->bindTexture(1, channel, LLTexUnit::TFO_POINT); // frag_data[1]
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    channel = shader.enableTexture(LLShaderMgr::NORMAL_MAP, deferred_target->getUsage());
    if (channel > -1)
    {
        deferred_target->bindTexture(2, channel, LLTexUnit::TFO_POINT); // frag_data[2]
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    channel = shader.enableTexture(LLShaderMgr::DEFERRED_EMISSIVE, deferred_target->getUsage());
    if (channel > -1)
    {
        deferred_target->bindTexture(3, channel, LLTexUnit::TFO_POINT); // frag_data[3]
        gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
    }

    channel = shader.enableTexture(LLShaderMgr::DEFERRED_DEPTH, deferred_target->getUsage());
    if (channel > -1)
    {
        if (depth_target)
        {
            gGL.getTexUnit(channel)->bind(depth_target, true);
        }
        else
        {
            gGL.getTexUnit(channel)->bind(deferred_target, true);
        }
        stop_glerror();
    }

    channel = shader.enableTexture(LLShaderMgr::EXPOSURE_MAP);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bind(&mExposureMap);
    }

    if (shader.getUniformLocation(LLShaderMgr::VIEWPORT) != -1)
    {
        shader.uniform4f(LLShaderMgr::VIEWPORT, (F32) gGLViewport[0],
                                    (F32) gGLViewport[1],
                                    (F32) gGLViewport[2],
                                    (F32) gGLViewport[3]);
    }

    if (sReflectionRender && !shader.getUniformLocation(LLShaderMgr::MODELVIEW_MATRIX))
    {
        shader.uniformMatrix4fv(LLShaderMgr::MODELVIEW_MATRIX, 1, false, glm::value_ptr(mReflectionModelView));
    }

    channel = shader.enableTexture(LLShaderMgr::DEFERRED_NOISE);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mNoiseMap);
        gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
    }

    bindLightFunc(shader);

    stop_glerror();

    light_target = light_target ? light_target : deferred_light_target;
    channel = shader.enableTexture(LLShaderMgr::DEFERRED_LIGHT, light_target->getUsage());
    if (channel > -1)
    {
        if (light_target->isComplete())
        {
            light_target->bindTexture(0, channel, LLTexUnit::TFO_POINT);
        }
        else
        {
            gGL.getTexUnit(channel)->bindFast(LLViewerFetchedTexture::sWhiteImagep);
        }
    }

    stop_glerror();

    bindShadowMaps(shader);

    stop_glerror();

    F32 mat[16*6];
    for (U32 i = 0; i < 16; i++)
    {
        mat[i] = glm::value_ptr(mSunShadowMatrix[0])[i];
        mat[i+16] = glm::value_ptr(mSunShadowMatrix[1])[i];
        mat[i+32] = glm::value_ptr(mSunShadowMatrix[2])[i];
        mat[i+48] = glm::value_ptr(mSunShadowMatrix[3])[i];
        mat[i+64] = glm::value_ptr(mSunShadowMatrix[4])[i];
        mat[i+80] = glm::value_ptr(mSunShadowMatrix[5])[i];
    }

    shader.uniformMatrix4fv(LLShaderMgr::DEFERRED_SHADOW_MATRIX, 6, false, mat);

    stop_glerror();

    if (!LLPipeline::sReflectionProbesEnabled)
    {
        channel = shader.enableTexture(LLShaderMgr::ENVIRONMENT_MAP, LLTexUnit::TT_CUBE_MAP);
        if (channel > -1)
        {
            LLCubeMap* cube_map = gSky.mVOSkyp ? gSky.mVOSkyp->getCubeMap() : NULL;
            if (cube_map)
            {
                cube_map->enable(channel);
                cube_map->bind();
            }

            F32* m = gGLModelView;

            F32 mat[] = { m[0], m[1], m[2],
                          m[4], m[5], m[6],
                          m[8], m[9], m[10] };

            shader.uniformMatrix3fv(LLShaderMgr::DEFERRED_ENV_MAT, 1, true, mat);
        }
    }

    bindReflectionProbes(shader);

    /*if (gCubeSnapshot)
    { // we only really care about the first two values, but the shader needs increasing separation between clip planes
        shader.uniform4f(LLShaderMgr::DEFERRED_SHADOW_CLIP, 1.f, 64.f, 128.f, 256.f);
    }
    else*/
    {
        shader.uniform4fv(LLShaderMgr::DEFERRED_SHADOW_CLIP, 1, mSunClipPlanes.mV);
    }
    shader.uniform1f(LLShaderMgr::DEFERRED_SUN_WASH, RenderDeferredSunWash);
    shader.uniform1f(LLShaderMgr::DEFERRED_SHADOW_NOISE, RenderShadowNoise);
    shader.uniform1f(LLShaderMgr::DEFERRED_BLUR_SIZE, RenderShadowBlurSize);

    shader.uniform1f(LLShaderMgr::DEFERRED_SSAO_RADIUS, RenderSSAOScale);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSAO_MAX_RADIUS, (GLfloat)RenderSSAOMaxScale);

    F32 ssao_factor = RenderSSAOFactor;
    shader.uniform1f(LLShaderMgr::DEFERRED_SSAO_FACTOR, ssao_factor);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSAO_FACTOR_INV, 1.0f/ssao_factor);

    LLVector3 ssao_effect = RenderSSAOEffect;
    F32 matrix_diag = (ssao_effect[0] + 2.0f*ssao_effect[1])/3.0f;
    F32 matrix_nondiag = (ssao_effect[0] - ssao_effect[1])/3.0f;
    // This matrix scales (proj of color onto <1/rt(3),1/rt(3),1/rt(3)>) by
    // value factor, and scales remainder by saturation factor
    F32 ssao_effect_mat[] = {   matrix_diag, matrix_nondiag, matrix_nondiag,
                                matrix_nondiag, matrix_diag, matrix_nondiag,
                                matrix_nondiag, matrix_nondiag, matrix_diag};
    shader.uniformMatrix3fv(LLShaderMgr::DEFERRED_SSAO_EFFECT_MAT, 1, GL_FALSE, ssao_effect_mat);

    //F32 shadow_offset_error = 1.f + RenderShadowOffsetError * fabsf(LLViewerCamera::getInstance()->getOrigin().mV[2]);
    F32 shadow_bias_error = RenderShadowBiasError * fabsf(LLViewerCamera::getInstance()->getOrigin().mV[2])/3000.f;
    F32 shadow_bias       = RenderShadowBias + shadow_bias_error;
    // <plus> plus/shadowdist: finer sun maps, smaller bias (only ever smaller: the
    // shadow receiver cull's bound, from RenderShadowBias, still holds)
    shadow_bias *= plus::shadowdist::biasScale();
    // </plus>
    // <plus> plus/shadowdist: shadowUtil.glsl's normal offset, in texels (0: none; research/107 section 31)
    static LLStaticHashedString sPlusNormalOffset("plus_normal_offset_texels");
    shader.uniform1f(sPlusNormalOffset, plus::shadowdist::normalOffsetTexels());
    // </plus>

    shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (GLfloat)deferred_target->getWidth(), (GLfloat)deferred_target->getHeight());
    shader.uniform1f(LLShaderMgr::DEFERRED_NEAR_CLIP, LLViewerCamera::getInstance()->getNear()*2.f);
    shader.uniform1f (LLShaderMgr::DEFERRED_SHADOW_OFFSET, RenderShadowOffset); //*shadow_offset_error);
    shader.uniform1f(LLShaderMgr::DEFERRED_SHADOW_BIAS, shadow_bias);
    shader.uniform1f(LLShaderMgr::DEFERRED_SPOT_SHADOW_OFFSET, RenderSpotShadowOffset);
    shader.uniform1f(LLShaderMgr::DEFERRED_SPOT_SHADOW_BIAS, RenderSpotShadowBias);

    shader.uniform3fv(LLShaderMgr::DEFERRED_SUN_DIR, 1, mTransformedSunDir.mV);
    shader.uniform3fv(LLShaderMgr::DEFERRED_MOON_DIR, 1, mTransformedMoonDir.mV);
    shader.uniform2f(LLShaderMgr::DEFERRED_SHADOW_RES, (GLfloat)mRT->shadow[0].getWidth(), (GLfloat)mRT->shadow[0].getHeight());
    shader.uniform2f(LLShaderMgr::DEFERRED_PROJ_SHADOW_RES, (GLfloat)mSpotShadow[0].getWidth(), (GLfloat)mSpotShadow[0].getHeight());
    shader.uniform1f(LLShaderMgr::DEFERRED_DEPTH_CUTOFF, RenderEdgeDepthCutoff);
    shader.uniform1f(LLShaderMgr::DEFERRED_NORM_CUTOFF, RenderEdgeNormCutoff);

    shader.uniformMatrix4fv(LLShaderMgr::MODELVIEW_DELTA_MATRIX, 1, GL_FALSE, glm::value_ptr(gGLDeltaModelView));
    shader.uniformMatrix4fv(LLShaderMgr::INVERSE_MODELVIEW_DELTA_MATRIX, 1, GL_FALSE, glm::value_ptr(gGLInverseDeltaModelView));

    shader.uniform1i(LLShaderMgr::CUBE_SNAPSHOT, gCubeSnapshot ? 1 : 0);

    if (shader.getUniformLocation(LLShaderMgr::DEFERRED_NORM_MATRIX) >= 0)
    {
        glm::mat4 norm_mat = glm::transpose(glm::inverse(get_current_modelview()));
        shader.uniformMatrix4fv(LLShaderMgr::DEFERRED_NORM_MATRIX, 1, false, glm::value_ptr(norm_mat));
    }

    // auto adjust legacy sun color if needed
    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", false);
    static LLCachedControl<F32> auto_adjust_sun_color_scale(gSavedSettings, "RenderSkyAutoAdjustSunColorScale", 1.f);
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();
    LLColor3 sun_diffuse(mSunDiffuse.mV);
    if (should_auto_adjust && psky->canAutoAdjust())
    {
        sun_diffuse *= auto_adjust_sun_color_scale;
    }

    shader.uniform3fv(LLShaderMgr::SUNLIGHT_COLOR, 1, sun_diffuse.mV);
    shader.uniform3fv(LLShaderMgr::MOONLIGHT_COLOR, 1, mMoonDiffuse.mV);

    shader.uniform1f(LLShaderMgr::REFLECTION_PROBE_MAX_LOD, mReflectionMapManager.mMaxProbeLOD);
}


LLColor3 pow3f(LLColor3 v, F32 f)
{
    v.mV[0] = powf(v.mV[0], f);
    v.mV[1] = powf(v.mV[1], f);
    v.mV[2] = powf(v.mV[2], f);
    return v;
}

LLVector4 pow4fsrgb(LLVector4 v, F32 f)
{
    v.mV[0] = powf(v.mV[0], f);
    v.mV[1] = powf(v.mV[1], f);
    v.mV[2] = powf(v.mV[2], f);
    return v;
}

// LLRHI: batched point lights (research/47). In Vulkan mode the boxes of the
// point lights go out as one instanced draw that reads each light from the
// frame's table (the backend's light table rewrite of gDeferredLightProgram)
// instead of five uniforms and a draw per light. The same boxes in the same
// order: the same pixels, blended in the same order.
namespace
{
    // Three vec4s, as the rewritten pointLightV reads them.
    struct LightRow
    {
        F32 center_size[4];     // center (agent space), size (radius * 1.5)
        F32 color_falloff[4];   // linear color * light scale, falloff
        F32 fan[4];             // get_box_fan_indices in x
    };
    static_assert(sizeof(LightRow) == 48, "three vec4s");

    bool light_batch_ready(const LLGLSLShader& shader)
    {
        return LLRHI::sGLDrawsDisabled && LLRHI::sDirect && LLRHI::sDirectDraw && LLRHI::sTableRows &&
               LLRHI::sLightTableProgram && LLRHI::sLightTableProgram(shader.mProgramObject);
    }

    // One instanced draw of the rows' boxes (the bound program, depth and
    // blend state). False when it could not go out (draw them one by one).
    bool draw_light_rows(const std::vector<LightRow>& rows)
    {
        U32 base = 0;
        F32* dst = LLRHI::sTableRows((U32)rows.size() * 3, base);
        if (!dst)
        {
            return false;
        }
        memcpy(dst, rows.data(), rows.size() * sizeof(LightRow));
        gGL.syncMatrices();
        // No attributes: the corners come from gl_VertexIndex (the position
        // input gets GL's constant, unread).
        LLRHI::DirectDraw::Attribute none[1];
        LLRHI::DirectDraw draw;
        draw.attributes = none;
        draw.attribute_count = 0;
        draw.mode = GL_TRIANGLES;
        draw.first_vertex = 0;
        draw.vertex_count = 18;     // the box fan's 6 triangles
        draw.instance_count = (U32)rows.size();
        draw.table_base = base;
        return LLRHI::sDirectDraw(draw);
    }

    // research/52: LLRHI_LIGHT_FAST=0 turns off the savings below (each view
    // reads every light again; the fullscreen spot light program is bound
    // even with no such light), for comparisons.
    bool light_fast()
    {
        static const bool on = LLRHI::envDefaultOn("LLRHI_LIGHT_FAST");
        return on;
    }

    // What the light loop of renderDeferredLighting reads of an entry of
    // mNearbyLights. Reading it costs cache misses on every light (drawable,
    // object, light parameters: LLVOVolume::getLightRadius alone was 0.7% of
    // the main thread), and the probe faces of a frame read the same lights
    // again and again: they share it (research/52).
    struct NearbyLight
    {
        LLDrawable* drawable;
        LLVOVolume* volume;     // null: skipped (as getVOVolume() null)
        F32 center[3];          // getPositionAgent()
        F32 size;               // getLightRadius() * 1.5
        LLColor3 color;         // getLightLinearColor(), before the light scale
        F32 falloff;            // getLightFalloff(DEFERRED_LIGHT_FALLOFF)
        bool attachment;
        bool spotlight;
    };

    // The first `limit` entries of mNearbyLights, read now or, on a probe face
    // after the first of a frame, as the first face read them. Nothing moves
    // or changes a light while the probes render (objects move and lights
    // change in idle and the main view's updateGeom, and the main view reads
    // them afresh); a change of the set itself moves sNearbyLightsVersion.
    // <plus> research/203: a light that does not move from its record in the
    // local light table (on; calcNearbyLights counts and compares, not this)
    LLLocalLights::Counts sUncountedReads;
    // </plus>
    template <class LightSet>
    const std::vector<NearbyLight>& nearby_lights(const LightSet& lights, S32 limit)
    {
        static std::vector<NearbyLight> kept;
        static bool kept_shared = false;
        static U32 kept_frame = 0;
        static U32 kept_version = 0;
        static S32 kept_limit = 0;
        const bool shared = light_fast() && gCubeSnapshot;
        if (shared && kept_shared && kept_frame == LLFrameTimer::getFrameCount() && kept_version == sNearbyLightsVersion &&
            kept_limit == limit)
        {
            return kept;
        }
        kept.clear();
        const bool table_reads = LLLocalLightTable::reads(); // <plus> research/203 </plus>
        S32 count = 0;
        for (const auto& light : lights)
        {
            if (++count > limit)
            {   // stop collecting lights once we hit the limit
                break;
            }
            NearbyLight& entry = kept.emplace_back();
            entry.drawable = light.drawable;
            entry.volume = entry.drawable->getVOVolume();
            if (!entry.volume)
            {
                continue;
            }
            // <plus> research/203: from the record
            if (const LLLocalLights::Values* record =
                    table_reads ? serve_light(entry.drawable, entry.volume, true, false, false, sUncountedReads) : nullptr)
            {
                entry.attachment = (record->flags & LLLocalLights::FLAG_ATTACHMENT) != 0;
                for (U32 i = 0; i < 3; ++i)
                {
                    entry.center[i] = record->center[i];
                    entry.color.mV[i] = record->color[i];
                }
                entry.size = record->size;
                entry.falloff = record->falloff;
                entry.spotlight = (record->flags & LLLocalLights::FLAG_SPOTLIGHT) != 0;
                continue;
            }
            // </plus>
            LLVOVolume* volume = entry.volume;
            entry.attachment = volume->isAttachment();
            const LLVector3& position = entry.drawable->getPositionAgent();
            entry.center[0] = position.mV[0];
            entry.center[1] = position.mV[1];
            entry.center[2] = position.mV[2];
            entry.size = volume->getLightRadius() * 1.5f;
            entry.color = volume->getLightLinearColor();
            entry.falloff = volume->getLightFalloff(DEFERRED_LIGHT_FALLOFF);
            entry.spotlight = volume->isLightSpotlight();
        }
        kept_shared = shared;
        kept_frame = LLFrameTimer::getFrameCount();
        kept_version = sNearbyLightsVersion;
        kept_limit = limit;
        return kept;
    }
}

void LLPipeline::renderDeferredLighting()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("renderDeferredLighting");
    LLRHI::gpuMark("lighting");
    if (!sCull)
    {
        return;
    }

    llassert(!sRenderingHUDs);

    F32 light_scale = 1.f;

    if (gCubeSnapshot)
    { //darken local lights when probe ambiance is above 1
        light_scale = mReflectionMapManager.mLightScale;
    }

    LLRenderTarget *screen_target         = &mRT->screen;
    LLRenderTarget* deferred_light_target = &mRT->deferredLight;

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("deferred");
        LLViewerCamera *camera = LLViewerCamera::getInstance();

        if (gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_HUD))
        {
            gPipeline.toggleRenderType(LLPipeline::RENDER_TYPE_HUD);
        }

        gGL.setColorMask(true, true);

        // draw a cube around every light
        LLVertexBuffer::unbind();

        LLGLEnable cull(GL_CULL_FACE);
        LLGLEnable blend(GL_BLEND);

        glm::mat4 mat = get_current_modelview();

        setupHWLights();  // to set mSun/MoonDir;

        glm::vec4 tc(mSunDir);
        tc = mat * tc;
        mTransformedSunDir.set(tc);

        glm::vec4 tc_moon(mMoonDir);
        tc_moon = mat * tc_moon;
        mTransformedMoonDir.set(tc_moon);

        // <plus> wet look (plus/wetlook): the G-buffer made wet before anything reads it
        plus::wetlook::render();
        // </plus>

        plus::volclouds::updateShadowMap(); // <plus> plus/volclouds (research/186): the volumetric clouds' shadow map, a pass of its own, before the sun pass </plus>

        if ((RenderDeferredSSAO && !gCubeSnapshot) || RenderShadowDetail > 0)
        {
            LL_PROFILE_GPU_ZONE("sun program");
            LLRHI::gpuMark("sun light"); // <plus> plus/shadowpenumbra (research/187): the sun pass on its own in the GPU time (VKPerf) </plus>
            // <plus> plus/gtao: asked before the native pass opens (it may make the GTAO targets) - research/125 #6
            const bool gtao_replaces_ssao = !gCubeSnapshot && plus::gtao::replacesAmbientOcclusion();
            // </plus>
            deferred_light_target->bindTarget();
            const LLPassScope native_pass("sun light", deferred_light_target, { LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::depth(&mRT->deferredScreen), LLPassScope::texture(mNoiseMap), LLPassScope::depth(getSunShadowTarget(0)), LLPassScope::depth(getSunShadowTarget(1)), LLPassScope::depth(getSunShadowTarget(2)), LLPassScope::depth(getSunShadowTarget(3)), LLPassScope::depth(getSpotShadowTarget(0)), LLPassScope::depth(getSpotShadowTarget(1)), plus::volclouds::shadowTarget() }); // LLRHI native (research/137 N12); plus/volclouds: the clouds' shadow map
            {  // paint shadow/SSAO light map (direct lighting lightmap)
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - sun shadow");

                // <plus> plus/gtao: while GTAO overwrites the light map's g, the sun program without the
                // official SSAO (the probe one: sunLightF.glsl, the same r, b and a) - research/125 #6
                // (was: gCubeSnapshot ? gDeferredSunProbeProgram : gDeferredSunProgram)
                LLGLSLShader& sun_shader = (gCubeSnapshot || gtao_replaces_ssao) ? gDeferredSunProbeProgram : gDeferredSunProgram;
                // </plus>
                bindDeferredShader(sun_shader, deferred_light_target);
                plus::shadowpenumbra::bind(sun_shader); // <plus> plus/shadowpenumbra (research/187): the penumbra that grows with the distance from the caster </plus>
                plus::volclouds::bindShadow(sun_shader); // <plus> plus/volclouds (research/186): the volumetric clouds' shadow </plus>
                mScreenTriangleVB->setBuffer();
                LLRHI::clearColor(1, 1, 1, 1);
                deferred_light_target->clear(GL_COLOR_BUFFER_BIT);
                LLRHI::clearColor(0, 0, 0, 0);

                sun_shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES,
                                              (GLfloat)deferred_light_target->getWidth(),
                                              (GLfloat)deferred_light_target->getHeight());

                {
                    LLGLDisable   blend(GL_BLEND);
                    LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_ALWAYS);
                    mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                }

                plus::shadowpenumbra::unbind(sun_shader); // <plus> plus/shadowpenumbra </plus>
                plus::volclouds::unbindShadow(sun_shader); // <plus> plus/volclouds </plus>
                unbindDeferredShader(sun_shader);
            }
            deferred_light_target->flush();
        }

        LLRHI::gpuMark("light maps"); // <plus> plus/shadowpenumbra (research/187): GTAO, the blur, SSR and the soften pass, apart from the sun pass </plus>
        // <plus> GTAO and contact shadows (plus/gtao): ambient occlusion into the light map's g, contact shadows into its r
        plus::gtao::render();
        // softer face shadows (plus/skinfill): the ambient occlusion weakened on skin, before the blur
        plus::skinfill::renderLightMap();
        // </plus>

        if (RenderDeferredSSAO && !gCubeSnapshot)
        {
            // soften direct lighting lightmap
            LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - soften shadow");
            LL_PROFILE_GPU_ZONE("soften shadow");
            // blur lightmap
            screen_target->bindTarget();
            const LLPassScope native_pass("blur light x", screen_target, { deferred_light_target, LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)
            LLRHI::clearColor(1, 1, 1, 1);
            screen_target->clear(GL_COLOR_BUFFER_BIT);
            LLRHI::clearColor(0, 0, 0, 0);

            bindDeferredShader(gDeferredBlurLightProgram);

            // <plus> plus/shadowdist: the shadow softness, a blur of at most a set length in metres
            // (was: RenderShadowGaussian, RenderShadowBlurSize, RenderShadowBlurDistFactor)
            LLVector3 go = plus::shadowdist::blurGaussian(RenderShadowGaussian);
            const U32 kern_length = 4;
            F32       blur_size = plus::shadowdist::blurSize(RenderShadowBlurSize);
            F32       dist_factor = plus::shadowdist::blurDistFactor(RenderShadowBlurDistFactor, blur_size);
            // </plus>

            // sample symmetrically with the middle sample falling exactly on 0.0
            F32 x = 0.f;

            LLVector3 gauss[32];  // xweight, yweight, offset

            for (U32 i = 0; i < kern_length; i++)
            {
                gauss[i].mV[0] = llgaussian(x, go.mV[0]);
                gauss[i].mV[1] = llgaussian(x, go.mV[1]);
                gauss[i].mV[2] = x;
                x += 1.f;
            }

            gDeferredBlurLightProgram.uniform2f(sDelta, 1.f, 0.f);
            gDeferredBlurLightProgram.uniform1f(sDistFactor, dist_factor);
            gDeferredBlurLightProgram.uniform3fv(sKern, kern_length, gauss[0].mV);
            gDeferredBlurLightProgram.uniform1f(sKernScale, blur_size * (kern_length / 2.f - 0.5f));

            {
                LLGLDisable   blend(GL_BLEND);
                LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_ALWAYS);
                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            }

            screen_target->flush();
            native_pass.close(); // LLRHI native (research/137 N12) fix-round6 3
            unbindDeferredShader(gDeferredBlurLightProgram);

            bindDeferredShader(gDeferredBlurLightProgram, screen_target);

            deferred_light_target->bindTarget();
            const LLPassScope native_pass2("blur light y", deferred_light_target, { screen_target, LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)

            gDeferredBlurLightProgram.uniform2f(sDelta, 0.f, 1.f);

            {
                LLGLDisable   blend(GL_BLEND);
                LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_ALWAYS);
                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            }
            deferred_light_target->flush();
            unbindDeferredShader(gDeferredBlurLightProgram);
        }

        // <plus> screen space reflections (plus/ssr, research/129): project/graphics-care-package's separate SSR
        // passes (the depth pyramid, the trace, the roughness blur) into the buffer the lighting reads
        plus::ssr::render();
        // </plus>

        screen_target->bindTarget();
        // clear color buffer here - zeroing alpha (glow) is important or it will accumulate against sky
        LLRHI::clearColor(0, 0, 0, 0);
        screen_target->clear(GL_COLOR_BUFFER_BIT);

        if (RenderDeferredAtmospheric)
        {  // apply sunlight contribution
            LLGLSLShader &soften_shader = gDeferredSoftenProgram;

            LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - atmospherics");
            LL_PROFILE_GPU_ZONE("atmospherics");
            const LLPassScope native_pass("atmospherics", screen_target, { LLPassScope::attachment(&mRT->deferredScreen, 0), LLPassScope::attachment(&mRT->deferredScreen, 1), LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::attachment(&mRT->deferredScreen, 3), LLPassScope::depth(&mRT->deferredScreen), deferred_light_target, &mExposureMap, &mPbrBrdfLut }); // LLRHI native (research/137 N12)
            bindDeferredShader(soften_shader);

            static LLCachedControl<F32> ssao_scale(gSavedSettings, "RenderSSAOIrradianceScale", 0.5f);
            static LLCachedControl<F32> ssao_max(gSavedSettings, "RenderSSAOIrradianceMax", 0.25f);
            static LLStaticHashedString ssao_scale_str("ssao_irradiance_scale");
            static LLStaticHashedString ssao_max_str("ssao_irradiance_max");

            soften_shader.uniform1f(ssao_scale_str, ssao_scale);
            soften_shader.uniform1f(ssao_max_str, ssao_max);

            LLEnvironment &environment = LLEnvironment::instance();

            soften_shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
            soften_shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);

            soften_shader.uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);

            {
                LLGLDepthTest depth(GL_FALSE);
                LLGLDisable   blend(GL_BLEND);

                // full screen blit
                mScreenTriangleVB->setBuffer();
                mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
            }

            unbindDeferredShader(gDeferredSoftenProgram);
        }

        static LLCachedControl<S32> local_light_count(gSavedSettings, "RenderLocalLightCount", 256);
        static LLCachedControl<S32> probe_level(gSavedSettings, "RenderReflectionProbeLevel", 0);

        if (local_light_count > 0 && (!gCubeSnapshot || probe_level > 0))
        {
            LLRHI::gpuMark("lights");
            gGL.setSceneBlendType(LLRender::BT_ADD);
            std::list<LLVector4>        fullscreen_lights;
            LLDrawable::drawable_list_t spot_lights;
            LLDrawable::drawable_list_t fullscreen_spot_lights;
            LLSettingsSky::ptr_t        psky        = LLEnvironment::instance().getCurrentSky();

            if (!gCubeSnapshot)
            {
                for (U32 i = 0; i < 2; i++)
                {
                    mTargetShadowSpotLight[i] = NULL;
                }
            }

            std::list<LLVector4> light_colors;

            LLVertexBuffer::unbind();

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - local lights");
                LL_PROFILE_GPU_ZONE("local lights");
                const LLPassScope native_pass("local lights", screen_target, { LLPassScope::attachment(&mRT->deferredScreen, 0), LLPassScope::attachment(&mRT->deferredScreen, 1), LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::attachment(&mRT->deferredScreen, 3), LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)
                bindDeferredShader(gDeferredLightProgram);

                if (mCubeVB.isNull())
                {
                    mCubeVB = ll_create_cube_vb(LLVertexBuffer::MAP_VERTEX);
                }

                mCubeVB->setBuffer();

                LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                // LLRHI (research/47): the boxes collected and drawn as one after the loop.
                const bool batch_lights = light_batch_ready(gDeferredLightProgram);
                static std::vector<LightRow> light_rows;
                light_rows.clear();
                // mNearbyLights already includes distance calculation and excludes muted avatars.
                // It is calculated from mLights
                // mNearbyLights also provides fade value to gracefully fade-out out of range lights
                // LLRHI (research/52): the first local_light_count of them, read
                // once for the probe faces of a frame (nearby_lights).
                for (const NearbyLight& light : nearby_lights(mNearbyLights, local_light_count))
                {
                    LLDrawable * drawablep = light.drawable;
                    LLVOVolume * volume = light.volume;
                    if (!volume)
                    {
                        continue;
                    }

                    if (light.attachment)
                    {
                        if (!sRenderAttachedLights)
                        {
                            continue;
                        }
                    }

                    LLVector4a center;
                    center.load3(light.center);
                    const F32 *c = center.getF32ptr();
                    F32        s = light.size;

                    // send light color to shader in linear space
                    LLColor3 col = light.color * light_scale;

                    if (col.magVecSquared() < 0.001f)
                    {
                        continue;
                    }

                    if (s <= 0.001f)
                    {
                        continue;
                    }

                    LLVector4a sa;
                    sa.splat(s);
                    if (camera->AABBInFrustumNoFarClip(center, sa) == 0)
                    {
                        continue;
                    }

                    sVisibleLightCount++;

                    if (camera->getOrigin().mV[0] > c[0] + s + 0.2f || camera->getOrigin().mV[0] < c[0] - s - 0.2f ||
                        camera->getOrigin().mV[1] > c[1] + s + 0.2f || camera->getOrigin().mV[1] < c[1] - s - 0.2f ||
                        camera->getOrigin().mV[2] > c[2] + s + 0.2f || camera->getOrigin().mV[2] < c[2] - s - 0.2f)
                    {  // draw box if camera is outside box
                        if (light.spotlight)
                        {
                            volume->updateSpotLightPriority();
                            spot_lights.push_back(drawablep);
                            continue;
                        }

                        if (batch_lights)
                        {
                            LightRow& row = light_rows.emplace_back();
                            row.center_size[0] = c[0];
                            row.center_size[1] = c[1];
                            row.center_size[2] = c[2];
                            row.center_size[3] = s;
                            row.color_falloff[0] = col.mV[0];
                            row.color_falloff[1] = col.mV[1];
                            row.color_falloff[2] = col.mV[2];
                            row.color_falloff[3] = light.falloff;
                            row.fan[0] = (F32)get_box_fan_indices(camera, center);
                            row.fan[1] = row.fan[2] = row.fan[3] = 0.f;
                            continue;
                        }

                        gDeferredLightProgram.uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, c);
                        gDeferredLightProgram.uniform1f(LLShaderMgr::LIGHT_SIZE, s);
                        gDeferredLightProgram.uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, col.mV);
                        gDeferredLightProgram.uniform1f(LLShaderMgr::LIGHT_FALLOFF, light.falloff);
                        gDeferredLightProgram.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky->canAutoAdjust()) ? 1 : 0);

                        gGL.syncMatrices();

                        mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, get_box_fan_indices(camera, center));
                    }
                    else
                    {
                        if (light.spotlight)
                        {
                            volume->updateSpotLightPriority();
                            fullscreen_spot_lights.push_back(drawablep);
                            continue;
                        }

                        glm::vec3 tc(center);
                        tc = mul_mat4_vec3(mat, tc);

                        fullscreen_lights.push_back(LLVector4(tc.x, tc.y, tc.z, s));
                        light_colors.push_back(LLVector4(col.mV[0], col.mV[1], col.mV[2], light.falloff));
                    }
                }

                // Bookmark comment to allow searching for mSpecialRenderMode == 3 (avatar edit mode),
                // prev site of appended deferred character light, removed by SL-13522 09/20

                if (!light_rows.empty())
                {   // LLRHI: the collected boxes, in loop order
                    gDeferredLightProgram.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky->canAutoAdjust()) ? 1 : 0);
                    if (!draw_light_rows(light_rows))
                    {
                        // No room in the table (it grows for the next frame): one by
                        // one, as without batching (the program reads its uniforms then).
                        for (const LightRow& row : light_rows)
                        {
                            gDeferredLightProgram.uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, row.center_size);
                            gDeferredLightProgram.uniform1f(LLShaderMgr::LIGHT_SIZE, row.center_size[3]);
                            gDeferredLightProgram.uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, row.color_falloff);
                            gDeferredLightProgram.uniform1f(LLShaderMgr::LIGHT_FALLOFF, row.color_falloff[3]);
                            gGL.syncMatrices();
                            mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, (U32)row.fan[0]);
                        }
                    }
                    light_rows.clear();
                }

                unbindDeferredShader(gDeferredLightProgram);
            }

            if (!spot_lights.empty())
            {
                LLRHI::gpuMark("projectors"); // <plus> research/188 S0: "lights" now ends here; the projector boxes get their own GPU time </plus>
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - projectors");
                LL_PROFILE_GPU_ZONE("projectors");
                const LLPassScope native_pass("projectors", screen_target, { LLPassScope::attachment(&mRT->deferredScreen, 0), LLPassScope::attachment(&mRT->deferredScreen, 1), LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::attachment(&mRT->deferredScreen, 3), LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)
                LLGLDepthTest depth(GL_TRUE, GL_FALSE);
                bindDeferredShader(gDeferredSpotLightProgram);

                mCubeVB->setBuffer();

                gDeferredSpotLightProgram.enableTexture(LLShaderMgr::DEFERRED_PROJECTION);

                for (LLDrawable::drawable_list_t::iterator iter = spot_lights.begin(); iter != spot_lights.end(); ++iter)
                {
                    LLDrawable *drawablep = *iter;

                    LLVOVolume *volume = drawablep->getVOVolume();

                    LLVector4a center;
                    center.load3(drawablep->getPositionAgent().mV);
                    const F32* c = center.getF32ptr();
                    F32        s = volume->getLightRadius() * 1.5f;

                    sVisibleLightCount++;

                    setupSpotLight(gDeferredSpotLightProgram, drawablep);

                    // send light color to shader in linear space
                    LLColor3 col = volume->getLightLinearColor() * light_scale;

                    gDeferredSpotLightProgram.uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, c);
                    gDeferredSpotLightProgram.uniform1f(LLShaderMgr::LIGHT_SIZE, s);
                    gDeferredSpotLightProgram.uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, col.mV);
                    gDeferredSpotLightProgram.uniform1f(LLShaderMgr::LIGHT_FALLOFF, volume->getLightFalloff(DEFERRED_LIGHT_FALLOFF));
                    gDeferredSpotLightProgram.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky->canAutoAdjust()) ? 1 : 0);

                    gGL.syncMatrices();

                    mCubeVB->drawRange(LLRender::TRIANGLE_FAN, 0, 7, 8, get_box_fan_indices(camera, center));
                }
                gDeferredSpotLightProgram.disableTexture(LLShaderMgr::DEFERRED_PROJECTION);
                unbindDeferredShader(gDeferredSpotLightProgram);
            }

            {
                LLRHI::gpuMark("fullscreen lights"); // <plus> research/188 S0 </plus>
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("renderDeferredLighting - fullscreen lights");
                LLGLDepthTest depth(GL_FALSE);
                LL_PROFILE_GPU_ZONE("fullscreen lights");
                const LLPassScope native_pass("fullscreen lights", screen_target, { LLPassScope::attachment(&mRT->deferredScreen, 0), LLPassScope::attachment(&mRT->deferredScreen, 1), LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::attachment(&mRT->deferredScreen, 3), LLPassScope::depth(&mRT->deferredScreen) }); // LLRHI native (research/137 N12)

                U32 count = 0;

                const U32 max_count = LL_DEFERRED_MULTI_LIGHT_COUNT;
                LLVector4 light[max_count];
                LLVector4 col[max_count];

                F32 far_z = 0.f;

                while (!fullscreen_lights.empty())
                {
                    light[count] = fullscreen_lights.front();
                    fullscreen_lights.pop_front();
                    col[count] = light_colors.front();
                    light_colors.pop_front();

                    far_z = llmin(light[count].mV[2] - light[count].mV[3], far_z);
                    count++;
                    if (count == max_count || fullscreen_lights.empty())
                    {
                        U32 idx = count - 1;
                        bindDeferredShader(gDeferredMultiLightProgram[idx]);
                        gDeferredMultiLightProgram[idx].uniform1i(LLShaderMgr::MULTI_LIGHT_COUNT, count);
                        gDeferredMultiLightProgram[idx].uniform4fv(LLShaderMgr::MULTI_LIGHT, count, (GLfloat*)light);
                        gDeferredMultiLightProgram[idx].uniform4fv(LLShaderMgr::MULTI_LIGHT_COL, count, (GLfloat*)col);
                        gDeferredMultiLightProgram[idx].uniform1f(LLShaderMgr::MULTI_LIGHT_FAR_Z, far_z);
                        gDeferredMultiLightProgram[idx].uniform1i(LLShaderMgr::CLASSIC_MODE, (psky->canAutoAdjust()) ? 1 : 0);
                        far_z = 0.f;
                        count = 0;
                        mScreenTriangleVB->setBuffer();
                        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                        unbindDeferredShader(gDeferredMultiLightProgram[idx]);
                    }
                }

                // LLRHI (research/52): the program only for lights to draw (it is
                // bound and unbound, a full bindDeferredShader, on every view
                // although the camera is almost never inside a spot light).
                if (!fullscreen_spot_lights.empty() || !light_fast())
                {
                    bindDeferredShader(gDeferredMultiSpotLightProgram);

                    gDeferredMultiSpotLightProgram.enableTexture(LLShaderMgr::DEFERRED_PROJECTION);

                    mScreenTriangleVB->setBuffer();

                    for (LLDrawable::drawable_list_t::iterator iter = fullscreen_spot_lights.begin(); iter != fullscreen_spot_lights.end(); ++iter)
                    {
                        LLDrawable* drawablep = *iter;
                        LLVOVolume* volume = drawablep->getVOVolume();
                        LLVector3   center = drawablep->getPositionAgent();
                        F32         light_size_final = volume->getLightRadius() * 1.5f;
                        F32         light_falloff_final = volume->getLightFalloff(DEFERRED_LIGHT_FALLOFF);

                        sVisibleLightCount++;

                        glm::vec3 tc(center);
                        tc = mul_mat4_vec3(mat, tc);

                        setupSpotLight(gDeferredMultiSpotLightProgram, drawablep);

                        // send light color to shader in linear space
                        LLColor3 col = volume->getLightLinearColor() * light_scale;

                        gDeferredMultiSpotLightProgram.uniform3fv(LLShaderMgr::LIGHT_CENTER, 1, glm::value_ptr(tc));
                        gDeferredMultiSpotLightProgram.uniform1f(LLShaderMgr::LIGHT_SIZE, light_size_final);
                        gDeferredMultiSpotLightProgram.uniform3fv(LLShaderMgr::DIFFUSE_COLOR, 1, col.mV);
                        gDeferredMultiSpotLightProgram.uniform1f(LLShaderMgr::LIGHT_FALLOFF, light_falloff_final);
                        gDeferredMultiSpotLightProgram.uniform1i(LLShaderMgr::CLASSIC_MODE, (psky->canAutoAdjust()) ? 1 : 0);

                        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
                    }

                    gDeferredMultiSpotLightProgram.disableTexture(LLShaderMgr::DEFERRED_PROJECTION);
                    unbindDeferredShader(gDeferredMultiSpotLightProgram);
                }
                else if (sReflectionProbesEnabled)
                {
                    // What the skipped bind leaves behind for the next binds:
                    // bindReflectionProbes steps the SSR noise offset on every bind.
                    mPoissonOffset++;
                    if (mPoissonOffset > 128 - RenderScreenSpaceReflectionGlossySamples)
                    {
                        mPoissonOffset = 0;
                    }
                }
            }
        }

        gGL.setColorMask(true, true);
        LLRHI::gpuMark("lighting"); // <plus> research/188 S0: what follows the lights (skin, SSGI capture) is "lighting" again, not "fullscreen lights" </plus>
    }

    // <plus> skin (plus/skinsss): on the skin pixels of the lit screen, before the alpha geometry (stage 1: PlusSkinSssDebug)
    // softer face shadows (plus/skinfill): a fill light on skin, before the skin's scattering
    plus::skinfill::render();
    plus::skinsss::render();
    // screen space indirect light (plus/ssgi, research/164): the lit opaque pixels kept for the next frame's light
    plus::ssgi::capture();
    // </plus>

    {  // render non-deferred geometry (alpha, fullbright, glow)
        LLGLDisable blend(GL_BLEND);

        pushRenderTypeMask();
        andRenderTypeMask(LLPipeline::RENDER_TYPE_ALPHA,
                          LLPipeline::RENDER_TYPE_ALPHA_PRE_WATER,
                          LLPipeline::RENDER_TYPE_ALPHA_POST_WATER,
                          LLPipeline::RENDER_TYPE_FULLBRIGHT,
                          LLPipeline::RENDER_TYPE_VOLUME,
                          LLPipeline::RENDER_TYPE_GLOW,
                          LLPipeline::RENDER_TYPE_BUMP,
                          LLPipeline::RENDER_TYPE_GLTF_PBR,
                          LLPipeline::RENDER_TYPE_PASS_SIMPLE,
                          LLPipeline::RENDER_TYPE_PASS_ALPHA,
                          LLPipeline::RENDER_TYPE_PASS_ALPHA_MASK,
                          LLPipeline::RENDER_TYPE_PASS_BUMP,
                          LLPipeline::RENDER_TYPE_PASS_POST_BUMP,
                          LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT,
                          LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK,
                          LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_SHINY,
                          LLPipeline::RENDER_TYPE_PASS_GLOW,
                          LLPipeline::RENDER_TYPE_PASS_GLTF_GLOW,
                          LLPipeline::RENDER_TYPE_PASS_GRASS,
                          LLPipeline::RENDER_TYPE_PASS_SHINY,
                          LLPipeline::RENDER_TYPE_PASS_INVISIBLE,
                          LLPipeline::RENDER_TYPE_PASS_INVISI_SHINY,
                          LLPipeline::RENDER_TYPE_AVATAR,
                          LLPipeline::RENDER_TYPE_CONTROL_AV,
                          LLPipeline::RENDER_TYPE_ALPHA_MASK,
                          LLPipeline::RENDER_TYPE_FULLBRIGHT_ALPHA_MASK,
                          LLPipeline::RENDER_TYPE_TERRAIN,
                          LLPipeline::RENDER_TYPE_WATER,
                          LLPipeline::RENDER_TYPE_WATEREXCLUSION,
                          END_RENDER_TYPES);

        renderGeomPostDeferred(*LLViewerCamera::getInstance());
        popRenderTypeMask();
    }

    screen_target->flush();

    if (!gCubeSnapshot)
    {
        // this is the end of the 3D scene render, grab a copy of the modelview and projection
        // matrix for use in off-by-one-frame effects in the next frame
        for (U32 i = 0; i < 16; i++)
        {
            gGLLastModelView[i] = gGLModelView[i];
            gGLLastProjection[i] = gGLProjection[i];
        }
    }
    gGL.setColorMask(true, true);
}

void LLPipeline::doAtmospherics()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    if (sImpostorRender)
    { // do not attempt atmospherics on impostors
        return;
    }

    if (RenderDeferredAtmospheric)
    {
        {
            // copy depth buffer for use in haze shader (use water displacement map as temp storage)
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

            LLRenderTarget& src = gPipeline.mRT->screen;
            LLRenderTarget& depth_src = gPipeline.mRT->deferredScreen;
            LLRenderTarget& dst = gPipeline.mWaterDis;

            mRT->screen.flush();
            dst.bindTarget();
            const LLPassScope native_pass("haze depth copy", &dst, { &src, LLPassScope::depth(&depth_src) }); // LLRHI native (research/137 N12)
            gCopyDepthProgram.bind();

            S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
            S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);

            gGL.getTexUnit(diff_map)->bind(&src);
            gGL.getTexUnit(depth_map)->bind(&depth_src, true);

            gGL.setColorMask(false, false);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            dst.flush();
            mRT->screen.bindTarget();
        }

        LLGLEnable blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA, LLRender::BF_ZERO, LLRender::BF_SOURCE_ALPHA);
        gGL.setColorMask(true, true);

        // apply haze
        LLGLSLShader& haze_shader = gHazeProgram;

        LL_PROFILE_GPU_ZONE("haze");
        const LLPassScope native_pass("haze", &mRT->screen, { LLPassScope::attachment(&mRT->deferredScreen, 0), LLPassScope::attachment(&mRT->deferredScreen, 1), LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::attachment(&mRT->deferredScreen, 3), LLPassScope::depth(&mWaterDis) }); // LLRHI native (research/137 N12)
        bindDeferredShader(haze_shader, nullptr, &mWaterDis);

        LLEnvironment& environment = LLEnvironment::instance();
        haze_shader.uniform1i(LLShaderMgr::SUN_UP_FACTOR, environment.getIsSunUp() ? 1 : 0);
        haze_shader.uniform3fv(LLShaderMgr::LIGHTNORM, 1, environment.getClampedLightNorm().mV);

        haze_shader.uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);

        LLGLDepthTest depth(GL_FALSE);

        // full screen blit
        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

        unbindDeferredShader(haze_shader);

        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
}

void LLPipeline::doWaterHaze()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (sImpostorRender)
    { // do not attempt water haze on impostors
        return;
    }

    if (RenderDeferredAtmospheric)
    {
        // copy depth buffer for use in haze shader (use water displacement map as temp storage)
        {
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);

            LLRenderTarget& src = gPipeline.mRT->screen;
            LLRenderTarget& depth_src = gPipeline.mRT->deferredScreen;
            LLRenderTarget& dst = gPipeline.mWaterDis;

            mRT->screen.flush();
            dst.bindTarget();
            const LLPassScope native_pass("water haze depth copy", &dst, { &src, LLPassScope::depth(&depth_src) }); // LLRHI native (research/137 N12)
            gCopyDepthProgram.bind();

            S32 diff_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DIFFUSE_MAP);
            S32 depth_map = gCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);

            gGL.getTexUnit(diff_map)->bind(&src);
            gGL.getTexUnit(depth_map)->bind(&depth_src, true);

            gGL.setColorMask(false, false);
            gPipeline.mScreenTriangleVB->setBuffer();
            gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);

            dst.flush();
            mRT->screen.bindTarget();
        }

        LLGLEnable blend(GL_BLEND);
        gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_SOURCE_ALPHA, LLRender::BF_ZERO, LLRender::BF_SOURCE_ALPHA);

        gGL.setColorMask(true, true);

        // apply haze
        LLGLSLShader& haze_shader = gHazeWaterProgram;

        LL_PROFILE_GPU_ZONE("haze");
        const LLPassScope native_pass("water haze", &mRT->screen, { LLPassScope::attachment(&mRT->deferredScreen, 0), LLPassScope::attachment(&mRT->deferredScreen, 1), LLPassScope::attachment(&mRT->deferredScreen, 2), LLPassScope::attachment(&mRT->deferredScreen, 3), LLPassScope::depth(&mWaterDis), &mWaterExclusionMask }); // LLRHI native (research/137 N12)
        bindDeferredShader(haze_shader, nullptr, &mWaterDis);

        haze_shader.uniform4fv(LLShaderMgr::WATER_WATERPLANE, 1, LLDrawPoolAlpha::sWaterPlane.mV);

        static LLStaticHashedString above_water_str("above_water");
        haze_shader.uniform1i(above_water_str, sUnderWaterRender ? -1 : 1);

        haze_shader.bindTexture(LLShaderMgr::WATER_EXCLUSIONTEX, &mWaterExclusionMask);

        if (LLPipeline::sUnderWaterRender)
        {
            LLGLDepthTest depth(GL_FALSE);

            // full screen blit
            mScreenTriangleVB->setBuffer();
            mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        }
        else
        {
            //render water patches like LLDrawPoolWater does
            LLGLDepthTest depth(GL_TRUE, GL_FALSE);
            LLGLDisable   cull(GL_CULL_FACE);

            gGLLastMatrix = NULL;
            gGL.loadMatrix(gGLModelView);

            if (mWaterPool)
            {
                mWaterPool->pushFaceGeometry();
            }
        }

        unbindDeferredShader(haze_shader);


        gGL.setSceneBlendType(LLRender::BT_ALPHA);
    }
}

void LLPipeline::doWaterExclusionMask()
{
    mWaterExclusionMask.bindTarget();
    LLRHI::clearColor(1, 1, 1, 1);
    mWaterExclusionMask.clear();
    mWaterExclusionPool->render();

    mWaterExclusionMask.flush();
    LLRHI::clearColor(0, 0, 0, 0);
}

void LLPipeline::setupSpotLight(LLGLSLShader& shader, LLDrawable* drawablep)
{
    //construct frustum
    LLVOVolume* volume = drawablep->getVOVolume();
    LLVector3 params = volume->getSpotLightParams();

    F32 fov = params.mV[0];
    F32 focus = params.mV[1];

    LLVector3 pos = drawablep->getPositionAgent();
    LLQuaternion quat = volume->getRenderRotation();
    LLVector3 scale = volume->getScale();

    //get near clip plane
    LLVector3 at_axis(0,0,-scale.mV[2]*0.5f);
    at_axis *= quat;

    LLVector3 np = pos+at_axis;
    at_axis.normVec();

    //get origin that has given fov for plane np, at_axis, and given scale
    F32 dist = (scale.mV[1]*0.5f)/tanf(fov*0.5f);

    LLVector3 origin = np - at_axis*dist;

    //matrix from volume space to agent space
    LLMatrix4 light_mat(quat, LLVector4(origin,1.f));

    glm::mat4 light_to_agent(glm::make_mat4((F32*) light_mat.mMatrix));
    glm::mat4 light_to_screen = get_current_modelview() * light_to_agent;

    glm::mat4 screen_to_light = glm::inverse(light_to_screen);

    F32 s = volume->getLightRadius()*1.5f;
    F32 near_clip = dist;
    F32 width = scale.mV[VX];
    F32 height = scale.mV[VY];
    F32 far_clip = s+dist-scale.mV[VZ];

    F32 fovy = fov; // radians
    F32 aspect = width/height;

    glm::mat4 trans(0.5f, 0.0f, 0.0f, 0.0f,
                        0.0f, 0.5f, 0.0f, 0.0f,
                        0.0f, 0.0f, 0.5f, 0.0f,
                        0.5f, 0.5f, 0.5f, 1.0f);

    glm::vec3 p1(0, 0, -(near_clip+0.01f));
    glm::vec3 p2(0, 0, -(near_clip+1.f));

    glm::vec3 screen_origin(0, 0, 0);

    p1 = mul_mat4_vec3(light_to_screen, p1);
    p2 = mul_mat4_vec3(light_to_screen, p2);
    screen_origin = mul_mat4_vec3(light_to_screen, screen_origin);

    glm::vec3 n = p2-p1;
    n = glm::normalize(n);

    F32 proj_range = far_clip - near_clip;
    glm::mat4 light_proj = glm::perspective(fovy, aspect, near_clip, far_clip);
    screen_to_light = trans * light_proj * screen_to_light;
    shader.uniformMatrix4fv(LLShaderMgr::PROJECTOR_MATRIX, 1, false, glm::value_ptr(screen_to_light));
    shader.uniform1f(LLShaderMgr::PROJECTOR_NEAR, near_clip);
    shader.uniform3fv(LLShaderMgr::PROJECTOR_P, 1, glm::value_ptr(p1));
    shader.uniform3fv(LLShaderMgr::PROJECTOR_N, 1, glm::value_ptr(n));
    shader.uniform3fv(LLShaderMgr::PROJECTOR_ORIGIN, 1, glm::value_ptr(screen_origin));
    shader.uniform1f(LLShaderMgr::PROJECTOR_RANGE, proj_range);
    shader.uniform1f(LLShaderMgr::PROJECTOR_AMBIANCE, params.mV[2]);
    S32 s_idx = -1;

    for (U32 i = 0; i < 2; i++)
    {
        if (mShadowSpotLight[i] == drawablep)
        {
            s_idx = i;
        }
    }

    shader.uniform1i(LLShaderMgr::PROJECTOR_SHADOW_INDEX, s_idx);

    if (s_idx >= 0)
    {
        shader.uniform1f(LLShaderMgr::PROJECTOR_SHADOW_FADE, 1.f-mSpotLightFade[s_idx]);
    }
    else
    {
        shader.uniform1f(LLShaderMgr::PROJECTOR_SHADOW_FADE, 1.f);
    }

    // make sure we're not already targeting the same spot light with both shadow maps
    llassert(mTargetShadowSpotLight[0] != mTargetShadowSpotLight[1] || mTargetShadowSpotLight[0].isNull());

    if (!gCubeSnapshot)
    {
        LLDrawable* potential = drawablep;
        //determine if this light is higher priority than one of the existing spot shadows
        F32 m_pri = volume->getSpotLightPriority();

        for (U32 i = 0; i < 2; i++)
        {
            F32 pri = 0.f;

            if (mTargetShadowSpotLight[i].notNull())
            {
                pri = mTargetShadowSpotLight[i]->getVOVolume()->getSpotLightPriority();
            }

            if (m_pri > pri)
            {
                LLDrawable* temp = mTargetShadowSpotLight[i];
                mTargetShadowSpotLight[i] = potential;
                potential = temp;
                m_pri = pri;
            }
        }
    }

    // make sure we didn't end up targeting the same spot light with both shadow maps
    llassert(mTargetShadowSpotLight[0] != mTargetShadowSpotLight[1] || mTargetShadowSpotLight[0].isNull());

    LLViewerTexture* img = volume->getLightTexture();

    if (img == NULL)
    {
        img = LLViewerFetchedTexture::sWhiteImagep;
    }

    S32 channel = shader.enableTexture(LLShaderMgr::DEFERRED_PROJECTION);

    if (channel > -1)
    {
        if (img)
        {
            gGL.getTexUnit(channel)->bind(img);

            F32 lod_range = logf((F32)img->getWidth())/logf(2.f);

            shader.uniform1f(LLShaderMgr::PROJECTOR_FOCUS, focus);
            shader.uniform1f(LLShaderMgr::PROJECTOR_LOD, lod_range);
            shader.uniform1f(LLShaderMgr::PROJECTOR_AMBIENT_LOD, llclamp((proj_range-focus)/proj_range*lod_range, 0.f, 1.f));
        }
    }

}

void LLPipeline::unbindDeferredShader(LLGLSLShader &shader)
{
    LLRenderTarget* deferred_target       = &mRT->deferredScreen;
    LLRenderTarget* deferred_light_target = &mRT->deferredLight;

    stop_glerror();
    shader.disableTexture(LLShaderMgr::NORMAL_MAP, deferred_target->getUsage());
    shader.disableTexture(LLShaderMgr::DEFERRED_DIFFUSE, deferred_target->getUsage());
    shader.disableTexture(LLShaderMgr::DEFERRED_SPECULAR, deferred_target->getUsage());
    shader.disableTexture(LLShaderMgr::DEFERRED_EMISSIVE, deferred_target->getUsage());
    shader.disableTexture(LLShaderMgr::DEFERRED_BRDF_LUT);
    //shader.disableTexture(LLShaderMgr::DEFERRED_DEPTH, deferred_depth_target->getUsage());
    shader.disableTexture(LLShaderMgr::DEFERRED_DEPTH, deferred_target->getUsage());
    shader.disableTexture(LLShaderMgr::DEFERRED_LIGHT, deferred_light_target->getUsage());
    shader.disableTexture(LLShaderMgr::DIFFUSE_MAP);
    shader.disableTexture(LLShaderMgr::DEFERRED_BLOOM);

    for (U32 i = 0; i < 4; i++)
    {
        if (shader.disableTexture(LLShaderMgr::DEFERRED_SHADOW0+i) > -1)
        {
            LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
    }

    for (U32 i = 4; i < 6; i++)
    {
        if (shader.disableTexture(LLShaderMgr::DEFERRED_SHADOW0+i) > -1)
        {
            LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        }
    }

    shader.disableTexture(LLShaderMgr::DEFERRED_NOISE);
    shader.disableTexture(LLShaderMgr::DEFERRED_LIGHTFUNC);

    if (!LLPipeline::sReflectionProbesEnabled)
    {
        S32 channel = shader.disableTexture(LLShaderMgr::ENVIRONMENT_MAP, LLTexUnit::TT_CUBE_MAP);
        if (channel > -1)
        {
            LLCubeMap* cube_map = gSky.mVOSkyp ? gSky.mVOSkyp->getCubeMap() : NULL;
            if (cube_map)
            {
                cube_map->disable();
            }
        }
    }

    unbindReflectionProbes(shader);

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.getTexUnit(0)->activate();
    shader.unbind();
}

void LLPipeline::setEnvMat(LLGLSLShader& shader)
{
    F32* m = gGLModelView;

    F32 mat[] = { m[0], m[1], m[2],
                    m[4], m[5], m[6],
                    m[8], m[9], m[10] };

    shader.uniformMatrix3fv(LLShaderMgr::DEFERRED_ENV_MAT, 1, true, mat);
}

void LLPipeline::bindReflectionProbes(LLGLSLShader& shader)
{
    if (!sReflectionProbesEnabled)
    {
        return;
    }

    S32 channel = shader.enableTexture(LLShaderMgr::REFLECTION_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
    bool bound = false;
    if (channel > -1 && mReflectionMapManager.mTexture.notNull())
    {
        mReflectionMapManager.mTexture->bind(channel);
        bound = true;
    }

    channel = shader.enableTexture(LLShaderMgr::IRRADIANCE_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
    if (channel > -1 && mReflectionMapManager.mIrradianceMaps.notNull())
    {
        mReflectionMapManager.mIrradianceMaps->bind(channel);
        bound = true;
    }

    if (RenderMirrors)
    {
        channel = shader.enableTexture(LLShaderMgr::HERO_PROBE, LLTexUnit::TT_CUBE_MAP_ARRAY);
        if (channel > -1 && mHeroProbeManager.mTexture.notNull())
        {
            mHeroProbeManager.mTexture->bind(channel);
            bound = true;
        }
    }


    if (bound)
    {
        mReflectionMapManager.setUniforms();

        setEnvMat(shader);
    }

    // reflection probe shaders generally sample the scene map as well for SSR
    channel = shader.enableTexture(LLShaderMgr::SCENE_MAP);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bind(&mSceneMap);
    }


    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_ITR_COUNT, (GLfloat)RenderScreenSpaceReflectionIterations);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_DIST_BIAS, RenderScreenSpaceReflectionDistanceBias);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_RAY_STEP, RenderScreenSpaceReflectionRayStep);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_GLOSSY_SAMPLES, (GLfloat)RenderScreenSpaceReflectionGlossySamples);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_REJECT_BIAS, RenderScreenSpaceReflectionDepthRejectBias);
    mPoissonOffset++;

    if (mPoissonOffset > 128 - RenderScreenSpaceReflectionGlossySamples)
        mPoissonOffset = 0;

    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_NOISE_SINE, (GLfloat)mPoissonOffset);
    shader.uniform1f(LLShaderMgr::DEFERRED_SSR_ADAPTIVE_STEP_MULT, RenderScreenSpaceReflectionAdaptiveStepMultiplier);

    channel = shader.enableTexture(LLShaderMgr::SCENE_DEPTH);
    if (channel > -1)
    {
        gGL.getTexUnit(channel)->bind(&mSceneMap, true);
    }


    // <plus> plus/avatarrefl: your avatar mirrored in the floor or water (PLUS_AVATAR_REFL, research/127 stage 1)
    plus::avatarrefl::bind(shader);
    // </plus>

    // <plus> plus/ssr: on the main view's lighting, sceneMap is the separate SSR passes' buffer (research/129)
    plus::ssr::bind(shader);
    // plus/ssgi: on the main view's lighting, plusSsgiMap is this frame's indirect light (research/164)
    plus::ssgi::bind(shader);
    // </plus>
}

void LLPipeline::unbindReflectionProbes(LLGLSLShader& shader)
{
    S32 channel = shader.disableTexture(LLShaderMgr::REFLECTION_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
    if (channel > -1 && mReflectionMapManager.mTexture.notNull())
    {
        mReflectionMapManager.mTexture->unbind();
        if (channel == 0)
        {
            gGL.getTexUnit(channel)->enable(LLTexUnit::TT_TEXTURE);
        }
    }
}


inline float sgn(float a)
{
    if (a > 0.0F) return (1.0F);
    if (a < 0.0F) return (-1.0F);
    return (0.0F);
}

glm::mat4 look(const LLVector3 pos, const LLVector3 dir, const LLVector3 up)
{
    LLVector3 dirN;
    LLVector3 upN;
    LLVector3 lftN;

    lftN = dir % up;
    lftN.normVec();

    upN = lftN % dir;
    upN.normVec();

    dirN = dir;
    dirN.normVec();

    F32 ret[16];
    ret[ 0] = lftN[0];
    ret[ 1] = upN[0];
    ret[ 2] = -dirN[0];
    ret[ 3] = 0.f;

    ret[ 4] = lftN[1];
    ret[ 5] = upN[1];
    ret[ 6] = -dirN[1];
    ret[ 7] = 0.f;

    ret[ 8] = lftN[2];
    ret[ 9] = upN[2];
    ret[10] = -dirN[2];
    ret[11] = 0.f;

    ret[12] = -(lftN*pos);
    ret[13] = -(upN*pos);
    ret[14] = dirN*pos;
    ret[15] = 1.f;

    return glm::make_mat4(ret);
}

static LLTrace::BlockTimerStatHandle FTM_SHADOW_RENDER("Render Shadows");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_ALPHA("Alpha Shadow");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_SIMPLE("Simple Shadow");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_GEOM("Shadow Geom");

static LLTrace::BlockTimerStatHandle FTM_SHADOW_ALPHA_MASKED("Alpha Masked");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_ALPHA_BLEND("Alpha Blend");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_ALPHA_TREE("Alpha Tree");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_ALPHA_GRASS("Alpha Grass");
static LLTrace::BlockTimerStatHandle FTM_SHADOW_FULLBRIGHT_ALPHA_MASKED("Fullbright Alpha Masked");

// <plus> research/206: with the shadow sort's chunks below
namespace
{
    template <class Visit>
    void scene_layer_visit(Visit&& visit);
}
// </plus>

void LLPipeline::renderShadow(const glm::mat4& view, const glm::mat4& proj, LLCamera& shadow_cam, LLCullResult& result, bool depth_clamp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE; //LL_RECORD_BLOCK_TIME(FTM_SHADOW_RENDER);
    LL_PROFILE_GPU_ZONE("renderShadow");

    LLPipeline::sShadowRender = true;

    // disable occlusion culling during shadow render
    U32 saved_occlusion = sUseOcclusion;
    sUseOcclusion = 0;

    // LLRHI: resident groups of the GPU shadow list stay out of a sun shadow cull (research/46 G3).
    const U32 list_kinds = rhiListPhase(mRHIListSun);
    sRHIListCullKinds = list_kinds;
    updateCull(shadow_cam, result);
    sRHIListCullKinds = 0;
    sRHICoverage = nullptr; // LLRHI: a zero coverage view (generateSunShadow) is for the cull only (research/61)

    stateSort(shadow_cam, result);

    mRHIListPass = RHIListPass();
    mRHIListPass.volume = (list_kinds & 1) != 0;
    mRHIListPass.terrain = (list_kinds & 2) != 0;
    mRHIListPass.since_max = U32_MAX;
    renderShadowCasters(view, proj, shadow_cam, depth_clamp); // LLRHI: drawing half split out
    mRHIListPass = RHIListPass();

    // reset occlusion culling flag
    sUseOcclusion = saved_occlusion;
    LLPipeline::sShadowRender = false;
}

// LLRHI: the drawing half of renderShadow, over the current cull result.
void LLPipeline::renderShadowCasters(const glm::mat4& view, const glm::mat4& proj, LLCamera& shadow_cam, bool depth_clamp, bool draw_pools)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    // List of render pass types that use the prim volume as the shadow,
    // ignoring textures.
    static const U32 types[] = {
        LLRenderPass::PASS_SIMPLE,
        LLRenderPass::PASS_FULLBRIGHT,
        LLRenderPass::PASS_SHINY,
        LLRenderPass::PASS_BUMP,
        LLRenderPass::PASS_FULLBRIGHT_SHINY,
        LLRenderPass::PASS_MATERIAL,
        LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        LLRenderPass::PASS_SPECMAP,
        LLRenderPass::PASS_SPECMAP_EMISSIVE,
        LLRenderPass::PASS_NORMMAP,
        LLRenderPass::PASS_NORMMAP_EMISSIVE,
        LLRenderPass::PASS_NORMSPEC,
        LLRenderPass::PASS_NORMSPEC_EMISSIVE
    };

    LLGLEnable cull(GL_CULL_FACE);

    //enable depth clamping if available
    LLGLEnable clamp_depth(depth_clamp ? GL_DEPTH_CLAMP : 0);

    LLGLDepthTest depth_test(GL_TRUE, GL_TRUE, GL_LESS);

    // <plus> plus/shadowdist: a sun cascade's casters get a slope-scaled depth bias (Vulkan)
    plus::shadowdist::CasterBiasScope plus_caster_bias((S32)LLViewerCamera::sCurCameraID - (S32)LLViewerCamera::CAMERA_SUN_SHADOW0);
    // </plus>

    //generate shadow map
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(proj));
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(view));

    stop_glerror();
    gGLLastMatrix = NULL;

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

    stop_glerror();

    struct CompareVertexBuffer
    {
        bool operator()(const LLDrawInfo* const& lhs, const LLDrawInfo* const& rhs)
        {
            return lhs->mVertexBuffer > rhs->mVertexBuffer;
        }
    };


    // LLRHI: in Vulkan mode the non-rigged, non-GLTF casters are recorded natively.
    // GL still draws them when GL draws at all (mirror mode), just not translated.
    // Terrain too, when the pools are drawn (renderGeomShadow below).
    const bool native_terrain = draw_pools && LLRHI::sNativeTerrainShadows && hasRenderType(LLPipeline::RENDER_TYPE_TERRAIN);
    plus::shadowrest::TreePass rhi_tree_pass(draw_pools); // LLRHI native (research/137 N10): the tree pools' faces as packets
    const bool native = LLRHI::sNativeShadows && LLRHI::sDrawShadowPackets && drawNativeShadowCasters(view, proj, native_terrain, shadow_cam);
    rhi_tree_pass.drawn(native); // LLRHI native (research/137 N10)
    if (!native && (mRHIListPass.volume || mRHIListPass.terrain))
    {
        // LLRHI: the cull left resident groups to the GPU list and nothing drew them:
        // the list stops (missing from this one pass only).
        rhiListFailed();
    }
    // <plus> research/206: the native pass did not draw the casters its layer left to the retained
    // scene (a cascade's moving casters or cached depth, a spot pass): back into this pass's render
    // map, drawn below (and no more cuts)
    if (!native && gSceneShadow.layerCut())
    {
        U32 scene_batches = 0;
        scene_layer_visit([&scene_batches](U32 type, LLDrawInfo* info)
        {
            sCull->pushDrawInfo(type, info);
            ++scene_batches;
        });
        gSceneShadow.rescued(scene_batches, true);
    }
    // </plus>
    const bool gl_draws_native = native && !LLRHI::sGLDrawsDisabled;
    const bool native_rigged = native && LLRHI::sNativeRiggedShadows;
    auto covered = [native, gl_draws_native](auto&& draw)
    {
        if (!native)
        {
            draw();
        }
        else if (gl_draws_native)
        {
            LLRHI::SuppressTranslation untranslated;
            draw();
        }
    };

    LLVertexBuffer::unbind();
    for (int j = 0; j < 2; ++j) // 0 -- static, 1 -- rigged
    {
        bool rigged = j == 1;
        gDeferredShadowProgram.bind(rigged);

        gGL.diffuseColor4f(1, 1, 1, 1);

        S32 shadow_detail = RenderShadowDetail;

        // if not using VSM, disable color writes
        if (shadow_detail <= 2)
        {
            gGL.setColorMask(false, false);
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow simple"); //LL_RECORD_BLOCK_TIME(FTM_SHADOW_SIMPLE);
        LL_PROFILE_GPU_ZONE("shadow simple");
        gGL.getTexUnit(0)->disable();

        LLRHI::sDepthOnlyPass = true; // LLRHI: one depth-only shader for every opaque caster
        for (U32 type : types)
        {
            if (rigged && !native_rigged)
            {
                renderObjects(type, false, false, rigged);
            }
            else
            {
                covered([&]() { renderObjects(type, false, false, rigged); }); // LLRHI
            }
        }

        renderGLTFObjects(LLRenderPass::PASS_GLTF_PBR, false, rigged);
        LLRHI::sDepthOnlyPass = false; // LLRHI

        gGL.getTexUnit(0)->enable(LLTexUnit::TT_TEXTURE);
    }

    if (LLPipeline::sUseOcclusion > 1)
    { // do occlusion culling against non-masked only to take advantage of hierarchical Z
        LLRHI::sOcclusionScope = 1; // LLRHI
        doOcclusion(shadow_cam);
        LLRHI::sOcclusionScope = 0; // LLRHI
    }


    if (draw_pools) // LLRHI: the static shadow cache draws pools with the moving casters
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow geom");
        mRHINativeTerrain = native && native_terrain ? (gl_draws_native ? 2 : 1) : 0; // LLRHI
        renderGeomShadow(shadow_cam);
        mRHINativeTerrain = 0; // LLRHI
    }

    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow alpha");
        LL_PROFILE_GPU_ZONE("shadow alpha");
        const S32 sun_up = LLEnvironment::instance().getIsSunUp() ? 1 : 0;
        U32 target_width = plus::shadowshift::ditherWidth(LLRenderTarget::sCurResX); // <plus> plus/shadowshift: the dither on the grid (was: sCurResX) </plus>

        for (int i = 0; i < 2; ++i)
        {
            bool rigged = i == 1;

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow alpha masked");
                LL_PROFILE_GPU_ZONE("shadow alpha masked");
                gDeferredShadowAlphaMaskProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);
                if (rigged && !native_rigged)
                {
                    renderMaskedObjects(LLRenderPass::PASS_ALPHA_MASK, true, true, rigged);
                }
                else
                {
                    covered([&]() { renderMaskedObjects(LLRenderPass::PASS_ALPHA_MASK, true, true, rigged); }); // LLRHI
                }
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow alpha blend");
                LL_PROFILE_GPU_ZONE("shadow alpha blend");
                mRHINativeAlpha = native && (!rigged || native_rigged) ? (gl_draws_native ? 2 : 1) : 0; // LLRHI
                renderAlphaObjects(rigged);
                mRHINativeAlpha = 0;
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow fullbright alpha masked");
                LL_PROFILE_GPU_ZONE("shadow alpha masked");
                gDeferredShadowFullbrightAlphaMaskProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
                LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);
                if (rigged && !native_rigged)
                {
                    renderFullbrightMaskedObjects(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, true, true, rigged);
                }
                else
                {
                    covered([&]() { renderFullbrightMaskedObjects(LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK, true, true, rigged); }); // LLRHI
                }
            }

            {
                LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow alpha grass");
                LL_PROFILE_GPU_ZONE("shadow alpha grass");
                gDeferredTreeShadowProgram.bind(rigged);
                LLGLSLShader::sCurBoundShaderPtr->setMinimumAlpha(ALPHA_BLEND_CUTOFF);

                if (i == 0)
                {
                    covered([&]() { renderObjects(LLRenderPass::PASS_GRASS, true); }); // LLRHI
                }

                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_PIPELINE("shadow alpha material");
                    LL_PROFILE_GPU_ZONE("shadow alpha material");
                    auto material_masks = [&]()
                    {
                        renderMaskedObjects(LLRenderPass::PASS_NORMSPEC_MASK, true, false, rigged);
                        renderMaskedObjects(LLRenderPass::PASS_MATERIAL_ALPHA_MASK, true, false, rigged);
                        renderMaskedObjects(LLRenderPass::PASS_SPECMAP_MASK, true, false, rigged);
                        renderMaskedObjects(LLRenderPass::PASS_NORMMAP_MASK, true, false, rigged);
                    };
                    if (rigged && !native_rigged)
                    {
                        material_masks();
                    }
                    else
                    {
                        covered(material_masks); // LLRHI
                    }
                }
            }
        }

        for (int i = 0; i < 2; ++i)
        {
            bool rigged = i == 1;
            gDeferredShadowGLTFAlphaMaskProgram.bind(rigged);
            LLGLSLShader::sCurBoundShaderPtr->uniform1i(LLShaderMgr::SUN_UP_FACTOR, sun_up);
            LLGLSLShader::sCurBoundShaderPtr->uniform1f(LLShaderMgr::DEFERRED_SHADOW_TARGET_WIDTH, (float)target_width);

            gGL.loadMatrix(gGLModelView);
            gGLLastMatrix = NULL;

            U32 type = LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK;

            if (rigged)
            {
                mAlphaMaskPool->pushRiggedGLTFBatches(type + 1);
            }
            else
            {
                mAlphaMaskPool->pushGLTFBatches(type);
            }

            gGL.loadMatrix(gGLModelView);
            gGLLastMatrix = NULL;
        }
    }

    gDeferredShadowCubeProgram.bind();
    gGLLastMatrix = NULL;
    gGL.loadMatrix(gGLModelView);

    gGL.setColorMask(true, true);

    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
    gGLLastMatrix = NULL;
}

bool LLPipeline::getVisiblePointCloud(LLCamera& camera, LLVector3& min, LLVector3& max, std::vector<LLVector3>& fp, LLVector3 light_dir)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    //get point cloud of intersection of frust and min, max

    if (getVisibleExtents(camera, min, max))
    {
        return false;
    }

    //get set of planes on bounding box
    LLPlane bp[] = {
        LLPlane(min, LLVector3(-1,0,0)),
        LLPlane(min, LLVector3(0,-1,0)),
        LLPlane(min, LLVector3(0,0,-1)),
        LLPlane(max, LLVector3(1,0,0)),
        LLPlane(max, LLVector3(0,1,0)),
        LLPlane(max, LLVector3(0,0,1))};

    //potential points
    std::vector<LLVector3> pp;

    //add corners of AABB
    pp.push_back(LLVector3(min.mV[0], min.mV[1], min.mV[2]));
    pp.push_back(LLVector3(max.mV[0], min.mV[1], min.mV[2]));
    pp.push_back(LLVector3(min.mV[0], max.mV[1], min.mV[2]));
    pp.push_back(LLVector3(max.mV[0], max.mV[1], min.mV[2]));
    pp.push_back(LLVector3(min.mV[0], min.mV[1], max.mV[2]));
    pp.push_back(LLVector3(max.mV[0], min.mV[1], max.mV[2]));
    pp.push_back(LLVector3(min.mV[0], max.mV[1], max.mV[2]));
    pp.push_back(LLVector3(max.mV[0], max.mV[1], max.mV[2]));

    //add corners of camera frustum
    for (U32 i = 0; i < LLCamera::AGENT_FRUSTRUM_NUM; i++)
    {
        pp.push_back(camera.mAgentFrustum[i]);
    }


    //bounding box line segments
    U32 bs[] =
            {
        0,1,
        1,3,
        3,2,
        2,0,

        4,5,
        5,7,
        7,6,
        6,4,

        0,4,
        1,5,
        3,7,
        2,6
    };

    for (U32 i = 0; i < 12; i++)
    { //for each line segment in bounding box
        for (U32 j = 0; j < LLCamera::AGENT_PLANE_NO_USER_CLIP_NUM; j++)
        { //for each plane in camera frustum
            const LLPlane& cp = camera.getAgentPlane(j);
            const LLVector3& v1 = pp[bs[i*2+0]];
            const LLVector3& v2 = pp[bs[i*2+1]];
            LLVector3 n;
            cp.getVector3(n);

            LLVector3 line = v1-v2;

            F32 d1 = line*n;
            F32 d2 = -cp.dist(v2);

            F32 t = d2/d1;

            if (t > 0.f && t < 1.f)
            {
                LLVector3 intersect = v2+line*t;
                pp.push_back(intersect);
            }
        }
    }

    //camera frustum line segments
    const U32 fs[] =
    {
        0,1,
        1,2,
        2,3,
        3,0,

        4,5,
        5,6,
        6,7,
        7,4,

        0,4,
        1,5,
        2,6,
        3,7
    };

    for (U32 i = 0; i < 12; i++)
    {
        for (U32 j = 0; j < 6; ++j)
        {
            const LLVector3& v1 = pp[fs[i*2+0]+8];
            const LLVector3& v2 = pp[fs[i*2+1]+8];
            const LLPlane& cp = bp[j];
            LLVector3 n;
            cp.getVector3(n);

            LLVector3 line = v1-v2;

            F32 d1 = line*n;
            F32 d2 = -cp.dist(v2);

            F32 t = d2/d1;

            if (t > 0.f && t < 1.f)
            {
                LLVector3 intersect = v2+line*t;
                pp.push_back(intersect);
            }
        }
    }

    LLVector3 ext[] = { min-LLVector3(0.05f,0.05f,0.05f),
        max+LLVector3(0.05f,0.05f,0.05f) };

    for (U32 i = 0; i < pp.size(); ++i)
    {
        bool found = true;

        const F32* p = pp[i].mV;

        for (U32 j = 0; j < 3; ++j)
        {
            if (p[j] < ext[0].mV[j] ||
                p[j] > ext[1].mV[j])
            {
                found = false;
                break;
            }
        }

        for (U32 j = 0; j < LLCamera::AGENT_PLANE_NO_USER_CLIP_NUM; ++j)
        {
            const LLPlane& cp = camera.getAgentPlane(j);
            F32 dist = cp.dist(pp[i]);
            if (dist > 0.05f) //point is above some plane, not contained
            {
                found = false;
                break;
            }
        }

        if (found)
        {
            fp.push_back(pp[i]);
        }
    }

    if (fp.empty())
    {
        return false;
    }

    return true;
}

void LLPipeline::renderHighlight(const LLViewerObject* obj, F32 fade)
{
    if (obj && obj->getVolume())
    {
        for (LLViewerObject::child_list_t::const_iterator iter = obj->getChildren().begin(); iter != obj->getChildren().end(); ++iter)
        {
            renderHighlight(*iter, fade);
        }

        LLDrawable* drawable = obj->mDrawable;
        if (drawable)
        {
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                LLFace* face = drawable->getFace(i);
                if (face)
                {
                    face->renderSelected(LLViewerTexture::sNullImagep, LLColor4(1,1,1,fade));
                }
            }
        }
    }
}


LLRenderTarget* LLPipeline::getSunShadowTarget(U32 i)
{
    llassert(i < 4);
    return &mRT->shadow[i];
}

LLRenderTarget* LLPipeline::getSpotShadowTarget(U32 i)
{
    llassert(i < 2);
    return &mSpotShadow[i];
}

static LLTrace::BlockTimerStatHandle FTM_GEN_SUN_SHADOW("Gen Sun Shadow");
static LLTrace::BlockTimerStatHandle FTM_GEN_SUN_SHADOW_SPOT_RENDER("Spot Shadow Render");

// helper class for disabling occlusion culling for the current stack frame
class LLDisableOcclusionCulling
{
public:
    S32 mUseOcclusion;

    LLDisableOcclusionCulling()
    {
        mUseOcclusion = LLPipeline::sUseOcclusion;
        LLPipeline::sUseOcclusion = 0;
    }

    ~LLDisableOcclusionCulling()
    {
        LLPipeline::sUseOcclusion = mUseOcclusion;
    }
};

void LLPipeline::generateSunShadow(LLCamera& camera)
{
    if (!sRenderDeferred || RenderShadowDetail <= 0)
    {
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE; //LL_RECORD_BLOCK_TIME(FTM_GEN_SUN_SHADOW);
    LL_PROFILE_GPU_ZONE("generateSunShadow");
    LLRHI::gpuMark("shadow");

    LLDisableOcclusionCulling no_occlusion;

    bool skip_avatar_update = false;
    if (!isAgentAvatarValid() || gAgentCamera.getCameraAnimating() || gAgentCamera.getCameraMode() != CAMERA_MODE_MOUSELOOK || !LLVOAvatar::sVisibleInFirstPerson)
    {
        skip_avatar_update = true;
    }

    if (!skip_avatar_update)
    {
        gAgentAvatarp->updateAttachmentVisibility(CAMERA_MODE_THIRD_PERSON);
    }

    glm::mat4 last_modelview = get_last_modelview();
    glm::mat4 last_projection = get_last_projection();

    pushRenderTypeMask();
    andRenderTypeMask(LLPipeline::RENDER_TYPE_SIMPLE,
                    LLPipeline::RENDER_TYPE_ALPHA,
                    LLPipeline::RENDER_TYPE_ALPHA_PRE_WATER,
                    LLPipeline::RENDER_TYPE_ALPHA_POST_WATER,
                    LLPipeline::RENDER_TYPE_GRASS,
                    LLPipeline::RENDER_TYPE_GLTF_PBR,
                    LLPipeline::RENDER_TYPE_FULLBRIGHT,
                    LLPipeline::RENDER_TYPE_BUMP,
                    LLPipeline::RENDER_TYPE_VOLUME,
                    LLPipeline::RENDER_TYPE_AVATAR,
                    LLPipeline::RENDER_TYPE_CONTROL_AV,
                    LLPipeline::RENDER_TYPE_TREE,
                    LLPipeline::RENDER_TYPE_TERRAIN,
                    LLPipeline::RENDER_TYPE_WATER,
                    LLPipeline::RENDER_TYPE_VOIDWATER,
                    LLPipeline::RENDER_TYPE_PASS_ALPHA,
                    LLPipeline::RENDER_TYPE_PASS_ALPHA_MASK,
                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK,
                    LLPipeline::RENDER_TYPE_PASS_GRASS,
                    LLPipeline::RENDER_TYPE_PASS_SIMPLE,
                    LLPipeline::RENDER_TYPE_PASS_BUMP,
                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT,
                    LLPipeline::RENDER_TYPE_PASS_SHINY,
                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_SHINY,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_BLEND,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_MASK,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_EMISSIVE,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_BLEND,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_MASK,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_EMISSIVE,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_BLEND,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_MASK,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_EMISSIVE,
                    LLPipeline::RENDER_TYPE_PASS_ALPHA_MASK_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_ALPHA_MASK_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_SIMPLE_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_BUMP_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_SHINY_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_FULLBRIGHT_SHINY_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_MASK_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_BLEND_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_MASK_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_SPECMAP_EMISSIVE_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_BLEND_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_MASK_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMMAP_EMISSIVE_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_BLEND_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_MASK_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_NORMSPEC_EMISSIVE_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_GLTF_PBR,
                    LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_RIGGED,
                    LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK,
                    LLPipeline::RENDER_TYPE_PASS_GLTF_PBR_ALPHA_MASK_RIGGED,
                    END_RENDER_TYPES);

    gGL.setColorMask(false, false);

    LLEnvironment& environment = LLEnvironment::instance();

    // LLRHI: the sun's shadow passes below may leave resident groups to the GPU
    // shadow list (research/46 G3); the spot lights' further down do not.
    struct SunPasses
    {
        bool& flag;
        explicit SunPasses(bool& f) : flag(f) { flag = true; }
        ~SunPasses() { flag = false; }
    } sun_passes(mRHIListSun);

    //get sun view matrix

    //store current projection/modelview matrix
    glm::mat4 saved_proj = get_current_projection();
    glm::mat4 saved_view = get_current_modelview();
    glm::mat4 inv_view = glm::inverse(saved_view);

    glm::mat4 view[6];
    glm::mat4 proj[6];

    // <plus> plus/shadowdist: held while the light drifts (the stable cascades' grid does not turn every frame)
    LLVector3 caster_dir(plus::shadowdist::shadowCasterDir(LLVector3(environment.getIsSunUp() ? mSunDir : mMoonDir), environment.getIsSunUp()));
    // </plus>

    //put together a universal "near clip" plane for shadow frusta
    LLPlane shadow_near_clip;
    {
        LLVector3 p = camera.getOrigin(); // gAgent.getPositionAgent();
        p += caster_dir * RenderFarClip*2.f;
        shadow_near_clip.setVec(p, caster_dir);
    }

    LLVector3 lightDir = -caster_dir;
    lightDir.normVec();

    //create light space camera matrix
    LLVector3 at = lightDir;

    LLVector3 up = camera.getAtAxis();

    if (fabsf(up*lightDir) > 0.75f)
    {
        up = camera.getUpAxis();
    }

    up.normVec();
    at.normVec();


    LLCamera main_camera = camera;

    F32 near_clip = 0.f;
    {
        //get visible point cloud
        std::vector<LLVector3> fp;

        main_camera.calcAgentFrustumPlanes(main_camera.mAgentFrustum);

        LLVector3 min,max;
        getVisiblePointCloud(main_camera,min,max,fp);

        if (fp.empty())
        {
            if (!hasRenderDebugMask(RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
            {
                mShadowCamera[0] = main_camera;
                mShadowExtents[0][0] = min;
                mShadowExtents[0][1] = max;

                mShadowFrustPoints[0].clear();
                mShadowFrustPoints[1].clear();
                mShadowFrustPoints[2].clear();
                mShadowFrustPoints[3].clear();
            }
            popRenderTypeMask();

            if (!skip_avatar_update)
            {
                gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
            }

            return;
        }

        //get good split distances for frustum
        for (U32 i = 0; i < fp.size(); ++i)
        {
            glm::vec3 v(fp[i]);
            v = mul_mat4_vec3(saved_view, v);
            fp[i] = LLVector3(v);
        }

        min = fp[0];
        max = fp[0];

        //get camera space bounding box
        for (U32 i = 1; i < fp.size(); ++i)
        {
            update_min_max(min, max, fp[i]);
        }

        near_clip    = llclamp(-max.mV[2], 0.01f, 4.0f);
        // <plus> plus/shadowdist: how far the shadows reach (was: 512)
        F32 far_clip = llclamp(-min.mV[2]*2.f, 16.0f, plus::shadowdist::maxDistance(512.0f));
        // </plus>

        //far_clip = llmin(far_clip, 128.f);
        far_clip = llmin(far_clip, camera.getFar());

        F32 range = far_clip-near_clip;

        // <plus> plus/shadowdist: how the cascades share that distance (was: RenderShadowSplitExponent)
        LLVector3 split_exp = plus::shadowdist::splitExponent(RenderShadowSplitExponent);
        // </plus>

        F32 da = 1.f-llmax( fabsf(lightDir*up), fabsf(lightDir*camera.getLeftAxis()) );

        da = powf(da, split_exp.mV[2]);

        F32 sxp = split_exp.mV[1] + (split_exp.mV[0]-split_exp.mV[1])*da;

        for (U32 i = 0; i < 4; ++i)
        {
            F32 x = (F32)(i+1)/4.f;
            x = powf(x, sxp);
            mSunClipPlanes.mV[i] = near_clip+range*x;
        }

        mSunClipPlanes.mV[0] *= 1.25f; //bump back first split for transition padding
        // <plus> plus/shadowdist: the splits fitted to the subject (the shadow focus)
        plus::shadowdist::fitSplits(mSunClipPlanes, near_clip, far_clip, camera, lightDir);
        // </plus>
    }

    if (gCubeSnapshot)
    { // stretch clip planes for reflection probe renders to reduce number of shadow passes
        mSunClipPlanes.mV[1] = mSunClipPlanes.mV[2];
        mSunClipPlanes.mV[2] = mSunClipPlanes.mV[3];
        mSunClipPlanes.mV[3] *= 1.5f;
    }


    // convenience array of 4 near clip plane distances
    F32 dist[] = { near_clip, mSunClipPlanes.mV[0], mSunClipPlanes.mV[1], mSunClipPlanes.mV[2], mSunClipPlanes.mV[3] };

    std::vector<ShadowCascade> merged_cascades; // LLRHI
    // <plus> plus/shadowdist: the static shadow cache's depth was drawn without (or with) the casters' bias
    if (plus::shadowdist::casterBiasChanged())
    {
        for (ShadowCacheEntry& entry : mShadowCacheEntry)
        {
            entry.valid = false;
        }
    }
    // </plus>
    if (mSunDiffuse == LLColor4::black)
    { //sun diffuse is totally black shadows don't matter
        skipRenderingShadows();
    }
    else if (gCubeSnapshot && mSharedProbeShadows.active) // LLRHI: one set of maps for all faces of this probe
    {
        shareProbeShadows(camera, lightDir, shadow_near_clip, inv_view);
    }
    else
    {
        for (S32 j = 0; j < (gCubeSnapshot ? 2 : 4); j++)
        {
            if (!hasRenderDebugMask(RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
            {
                mShadowFrustPoints[j].clear();
            }

            LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SUN_SHADOW0+j);

            //restore render matrices
            set_current_modelview(saved_view);
            set_current_projection(saved_proj);

            LLVector3 eye = camera.getOrigin();
            llassert(eye.isFinite());

            //camera used for shadow cull/render
            LLCamera shadow_cam;

            //create world space camera frustum for this split
            shadow_cam = camera;
            shadow_cam.setFar(16.f);

            LLViewerCamera::updateFrustumPlanes(shadow_cam, false, false, true);

            LLVector3* frust = shadow_cam.mAgentFrustum;

            LLVector3 pn = shadow_cam.getAtAxis();

            LLVector3 min, max;

            //construct 8 corners of split frustum section
            for (U32 i = 0; i < 4; i++)
            {
                LLVector3 delta = frust[i+4]-eye;
                delta += (frust[i+4]-frust[(i+2)%4+4])*0.05f;
                delta.normVec();
                F32 dp = delta*pn;
                frust[i] = eye + (delta*dist[j]*0.75f)/dp;
                frust[i+4] = eye + (delta*dist[j+1]*1.25f)/dp;
            }

            shadow_cam.calcAgentFrustumPlanes(frust);
            shadow_cam.mFrustumCornerDist = 0.f;

            if (!gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
            {
                mShadowCamera[j] = shadow_cam;
            }

            std::vector<LLVector3> fp;

            if (!gPipeline.getVisiblePointCloud(shadow_cam, min, max, fp, lightDir)
                || j > RenderShadowSplits)
            {
                //no possible shadow receivers
                if (!gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
                {
                    mShadowExtents[j][0] = LLVector3();
                    mShadowExtents[j][1] = LLVector3();
                    mShadowCamera[j+4] = shadow_cam;
                }

                mRT->shadow[j].bindTarget();
                {
                    LLGLDepthTest depth(GL_TRUE);
                    mRT->shadow[j].clear();
                }
                mRT->shadow[j].flush();

                mShadowError.mV[j] = 0.f;
                mShadowFOV.mV[j] = 0.f;
                if (!gCubeSnapshot)
                {
                    mShadowCacheEntry[j].valid = false; // LLRHI
                }

                continue;
            }

            if (!gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
            {
                mShadowExtents[j][0] = min;
                mShadowExtents[j][1] = max;
                mShadowFrustPoints[j] = fp;
            }


            //find a good origin for shadow projection
            LLVector3 origin;

            //get a temporary view projection
            view[j] = look(camera.getOrigin(), lightDir, -up);

            std::vector<LLVector3> wpf;

            for (U32 i = 0; i < fp.size(); i++)
            {
                glm::vec3 p(fp[i]);
                p = mul_mat4_vec3(view[j], p);
                wpf.push_back(LLVector3(p));
            }

            min = wpf[0];
            max = wpf[0];

            for (U32 i = 0; i < fp.size(); ++i)
            { //get AABB in camera space
                update_min_max(min, max, wpf[i]);
            }

            // Construct a perspective transform with perspective along y-axis that contains
            // points in wpf
            //Known:
            // - far clip plane
            // - near clip plane
            // - points in frustum
            //Find:
            // - origin

            //get some "interesting" points of reference
            LLVector3 center = (min+max)*0.5f;
            LLVector3 size = (max-min)*0.5f;
            LLVector3 near_center = center;
            near_center.mV[1] += size.mV[1]*2.f;


            //put all points in wpf in quadrant 0, reletive to center of min/max
            //get the best fit line using least squares
            F32 bfm = 0.f;
            F32 bfb = 0.f;

            for (U32 i = 0; i < wpf.size(); ++i)
            {
                wpf[i] -= center;
                wpf[i].mV[0] = fabsf(wpf[i].mV[0]);
                wpf[i].mV[2] = fabsf(wpf[i].mV[2]);
            }

            if (!wpf.empty())
            {
                F32 sx = 0.f;
                F32 sx2 = 0.f;
                F32 sy = 0.f;
                F32 sxy = 0.f;

                for (U32 i = 0; i < wpf.size(); ++i)
                {
                    sx += wpf[i].mV[0];
                    sx2 += wpf[i].mV[0]*wpf[i].mV[0];
                    sy += wpf[i].mV[1];
                    sxy += wpf[i].mV[0]*wpf[i].mV[1];
                }

                bfm = (sy*sx-wpf.size()*sxy)/(sx*sx-wpf.size()*sx2);
                bfb = (sx*sxy-sy*sx2)/(sx*sx-bfm*sx2);
            }

            {
                // best fit line is y=bfm*x+bfb

                //find point that is furthest to the right of line
                F32 off_x = -1.f;
                LLVector3 lp;

                for (U32 i = 0; i < wpf.size(); ++i)
                {
                    //y = bfm*x+bfb
                    //x = (y-bfb)/bfm
                    F32 lx = (wpf[i].mV[1]-bfb)/bfm;

                    lx = wpf[i].mV[0]-lx;

                    if (off_x < lx)
                    {
                        off_x = lx;
                        lp = wpf[i];
                    }
                }

                //get line with slope bfm through lp
                // bfb = y-bfm*x
                bfb = lp.mV[1]-bfm*lp.mV[0];

                //calculate error
                mShadowError.mV[j] = 0.f;

                for (U32 i = 0; i < wpf.size(); ++i)
                {
                    F32 lx = (wpf[i].mV[1]-bfb)/bfm;
                    mShadowError.mV[j] += fabsf(wpf[i].mV[0]-lx);
                }

                mShadowError.mV[j] /= wpf.size();
                mShadowError.mV[j] /= size.mV[0];

                if (mShadowError.mV[j] > RenderShadowErrorCutoff)
                { //just use ortho projection
                    mShadowFOV.mV[j] = -1.f;
                    origin.clearVec();
                    proj[j] = glm::ortho(min.mV[0], max.mV[0],
                                        min.mV[1], max.mV[1],
                                        -max.mV[2], -min.mV[2]);
                }
                else
                {
                    //origin is where line x = 0;
                    origin.setVec(0,bfb,0);

                    F32 fovz = 1.f;
                    F32 fovx = 1.f;

                    LLVector3 zp;
                    LLVector3 xp;

                    for (U32 i = 0; i < wpf.size(); ++i)
                    {
                        LLVector3 atz = wpf[i]-origin;
                        atz.mV[0] = 0.f;
                        atz.normVec();
                        if (fovz > -atz.mV[1])
                        {
                            zp = wpf[i];
                            fovz = -atz.mV[1];
                        }

                        LLVector3 atx = wpf[i]-origin;
                        atx.mV[2] = 0.f;
                        atx.normVec();
                        if (fovx > -atx.mV[1])
                        {
                            fovx = -atx.mV[1];
                            xp = wpf[i];
                        }
                    }

                    fovx = acos(fovx);
                    fovz = acos(fovz);

                    F32 cutoff = llmin((F32) RenderShadowFOVCutoff, 1.4f);

                    mShadowFOV.mV[j] = fovx;

                    if (fovx < cutoff && fovz > cutoff)
                    {
                        //x is a good fit, but z is too big, move away from zp enough so that fovz matches cutoff
                        F32 d = zp.mV[2]/tan(cutoff);
                        F32 ny = zp.mV[1] + fabsf(d);

                        origin.mV[1] = ny;

                        fovz = 1.f;
                        fovx = 1.f;

                        for (U32 i = 0; i < wpf.size(); ++i)
                        {
                            LLVector3 atz = wpf[i]-origin;
                            atz.mV[0] = 0.f;
                            atz.normVec();
                            fovz = llmin(fovz, -atz.mV[1]);

                            LLVector3 atx = wpf[i]-origin;
                            atx.mV[2] = 0.f;
                            atx.normVec();
                            fovx = llmin(fovx, -atx.mV[1]);
                        }

                        fovx = acos(fovx);
                        fovz = acos(fovz);

                        mShadowFOV.mV[j] = cutoff;
                    }


                    origin += center;

                    F32 ynear = -(max.mV[1]-origin.mV[1]);
                    F32 yfar = -(min.mV[1]-origin.mV[1]);

                    if (ynear < 0.1f) //keep a sensible near clip plane
                    {
                        F32 diff = 0.1f-ynear;
                        origin.mV[1] += diff;
                        ynear += diff;
                        yfar += diff;
                    }

                    if (fovx > cutoff)
                    { //just use ortho projection
                        origin.clearVec();
                        mShadowError.mV[j] = -1.f;
                        proj[j] = glm::ortho(min.mV[0], max.mV[0],
                                min.mV[1], max.mV[1],
                                -max.mV[2], -min.mV[2]);
                    }
                    else
                    {
                        //get perspective projection
                        view[j] = glm::inverse(view[j]);
                        //llassert(origin.isFinite());

                        glm::vec3 origin_agent(origin);

                        //translate view to origin
                        origin_agent = mul_mat4_vec3(view[j], origin_agent);

                        eye = LLVector3(origin_agent);
                        //llassert(eye.isFinite());
                        if (!hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
                        {
                            mShadowFrustOrigin[j] = eye;
                        }

                        view[j] = look(LLVector3(origin_agent), lightDir, -up);

                        F32 fx = 1.f/tanf(fovx);
                        F32 fz = 1.f/tanf(fovz);

                        proj[j] = glm::mat4(-fx, 0, 0, 0,
                            0, (yfar + ynear) / (ynear - yfar), 0, -1.0f,
                            0, 0, -fz, 0,
                            0, (2.f * yfar * ynear) / (ynear - yfar), 0, 0);
                    }
                }
            }

            // <plus> plus/shadowdist: a stable cascade (whole texels, sizes and turns in steps) against shimmer
            if (plus::shadowdist::stabilizeCascade(j, fp, lightDir, camera, mRT->shadow[j].getWidth(), mRT->shadow[j].getHeight(),
                                                   view[j], proj[j]))
            {
                mShadowFOV.mV[j] = -1.f;    // orthographic
            }
            // </plus>

            //shadow_cam.setFar(128.f);
            shadow_cam.setOriginAndLookAt(eye, up, center);

            shadow_cam.setOrigin(0,0,0);

            set_current_modelview(view[j]);
            set_current_projection(proj[j]);

            LLViewerCamera::updateFrustumPlanes(shadow_cam, false, false, true);

            //shadow_cam.ignoreAgentFrustumPlane(LLCamera::AGENT_PLANE_NEAR);
            shadow_cam.getAgentPlane(LLCamera::AGENT_PLANE_NEAR).set(shadow_near_clip);

            // <plus> plus/shadowshift: the frame's PlusShadowShift and letting go, with the sun shadow cache off too
            if (!gCubeSnapshot)
            {
                plus::shadowshift::beginFrame();
            }
            // </plus>
            if (LLRHI::sCacheSunShadows && !gCubeSnapshot) // LLRHI: keep the cached projection when it still fits
            {
                reuseShadowCascade(j, view[j], proj[j], shadow_cam, fp, lightDir);
            }

            //translate and scale to from [-1, 1] to [0, 1]
            glm::mat4 trans(0.5f, 0.0f, 0.0f, 0.0f,
                            0.0f, 0.5f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.5f, 0.0f,
                            0.5f, 0.5f, 0.5f, 1.0f);

            set_current_modelview(view[j]);
            set_current_projection(proj[j]);

            set_last_modelview(mShadowModelview[j]);
            set_last_projection(mShadowProjection[j]);

            mShadowModelview[j] = view[j];
            mShadowProjection[j] = proj[j];
            // <plus> plus/shadowdist: the shadow detail's bias, a length along the light per cascade (was: proj[j])
            mSunShadowMatrix[j] = trans*plus::shadowdist::samplingProjection(j, proj[j], view[j], camera, dist[j], dist[j+1],
                                                                             lightDir, mRT->shadow[j])*view[j]*inv_view;
            // </plus>

            stop_glerror();

            if (LLRHI::sMergeShadowCascades) // LLRHI: drawn together after the loop
            {
                merged_cascades.push_back({ j, view[j], proj[j], shadow_cam });
                continue;
            }

            mRT->shadow[j].bindTarget();
            mRT->shadow[j].getViewport(gGLViewport);
            mRT->shadow[j].clear();

            {
                static LLCullResult result[4];
                // LLRHI: zero coverage cull in this cascade's texels (research/61)
                LLRHI::CoverageView& coverage = sRHICoverageViews[1 + j];
                coverage.active = false;
                if (!gCubeSnapshot && !gUseWireframe)
                {
                    rhi_coverage_set(coverage, proj[j] * view[j], mRT->shadow[j].getWidth(), mRT->shadow[j].getHeight(), true);
                }
                sRHICoverage = coverage.active ? &coverage : nullptr;
                renderShadow(view[j], proj[j], shadow_cam, result[j], true);
                sRHICoverage = nullptr;
            }

            mRT->shadow[j].flush();

            if (!gPipeline.hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
            {
                mShadowCamera[j+4] = shadow_cam;
            }
        }

        if (!merged_cascades.empty()) // LLRHI
        {
            // The camera whose pixels read these maps (receiver cull, research/64).
            mRHIShadowReceivers.valid = !gCubeSnapshot;
            mRHIShadowReceivers.view = saved_view;
            mRHIShadowReceivers.proj = saved_proj;
            mRHIShadowReceivers.origin = camera.getOrigin();
            renderSunShadowCascades(merged_cascades);
            mRHIShadowReceivers.valid = false;
        }
    }

    // <plus> frame cost detail (research/113, LLRHI_FRAME_COST=2)
    if (!gCubeSnapshot)
    {
        LLRHI::cpuDetailMark("shadow spot"); // spot light shadows, the rest of generateSunShadow
    }
    // </plus>
    mRHIListSun = false; // LLRHI: spot light shadows from here on

    //hack to disable projector shadows
    bool gen_shadow = RenderShadowDetail > 1;

    if (gen_shadow)
    {
        LLRHI::gpuMark("spot shadow"); // <plus> research/188 S0: the projector shadow maps apart from the sun's "shadow" </plus>
        if (!gCubeSnapshot) //skip updating spot shadow maps during cubemap updates
        {
            LLTrace::CountStatHandle<>* velocity_stat = LLViewerCamera::getVelocityStat();
            F32 fade_amt = gFrameIntervalSeconds.value()
                * (F32)llmax(LLTrace::get_frame_recording().getLastRecording().getSum(*velocity_stat) / LLTrace::get_frame_recording().getLastRecording().getDuration().value(), 1.0);

            // should never happen
            llassert(mTargetShadowSpotLight[0] != mTargetShadowSpotLight[1] || mTargetShadowSpotLight[0].isNull());

            //update shadow targets
            for (U32 i = 0; i < 2; i++)
            { //for each current shadow
                LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SPOT_SHADOW0 + i);

                if (mShadowSpotLight[i].notNull() &&
                    (mShadowSpotLight[i] == mTargetShadowSpotLight[0] ||
                        mShadowSpotLight[i] == mTargetShadowSpotLight[1]))
                { //keep this spotlight
                    mSpotLightFade[i] = llmin(mSpotLightFade[i] + fade_amt, 1.f);
                }
                else
                { //fade out this light
                    mSpotLightFade[i] = llmax(mSpotLightFade[i] - fade_amt, 0.f);

                    if (mSpotLightFade[i] == 0.f || mShadowSpotLight[i].isNull())
                    { //faded out, grab one of the pending spots (whichever one isn't already taken)
                        if (mTargetShadowSpotLight[0] != mShadowSpotLight[(i + 1) % 2])
                        {
                            mShadowSpotLight[i] = mTargetShadowSpotLight[0];
                        }
                        else
                        {
                            mShadowSpotLight[i] = mTargetShadowSpotLight[1];
                        }
                    }
                }
            }
        }

        // this should never happen
        llassert(mShadowSpotLight[0] != mShadowSpotLight[1] || mShadowSpotLight[0].isNull());

        for (S32 i = 0; i < 2; i++)
        {
            set_current_modelview(saved_view);
            set_current_projection(saved_proj);

            if (mShadowSpotLight[i].isNull())
            {
                continue;
            }

            LLVOVolume* volume = mShadowSpotLight[i]->getVOVolume();

            if (!volume)
            {
                mShadowSpotLight[i] = NULL;
                continue;
            }

            LLDrawable* drawable = mShadowSpotLight[i];

            LLVector3 params = volume->getSpotLightParams();
            F32 fov = params.mV[0];

            //get agent->light space matrix (modelview)
            LLVector3 center = drawable->getPositionAgent();
            LLQuaternion quat = volume->getRenderRotation();

            //get near clip plane
            LLVector3 scale = volume->getScale();
            LLVector3 at_axis(0, 0, -scale.mV[2] * 0.5f);
            at_axis *= quat;

            LLVector3 np = center + at_axis;
            at_axis.normVec();

            //get origin that has given fov for plane np, at_axis, and given scale
            F32 dist = (scale.mV[1] * 0.5f) / tanf(fov * 0.5f);

            LLVector3 origin = np - at_axis * dist;

            LLMatrix4 mat(quat, LLVector4(origin, 1.f));

            view[i + 4] = glm::make_mat4((F32*)mat.mMatrix);

            view[i + 4] = glm::inverse(view[i + 4]);

            //get perspective matrix
            F32 near_clip = dist + 0.01f;
            F32 width = scale.mV[VX];
            F32 height = scale.mV[VY];
            F32 far_clip = dist + volume->getLightRadius() * 1.5f;

            F32 fovy = fov; // radians
            F32 aspect = width / height;

            proj[i + 4] = glm::perspective(fovy, aspect, near_clip, far_clip);

            //translate and scale to from [-1, 1] to [0, 1]
            glm::mat4 trans(0.5f, 0.0f, 0.0f, 0.0f,
                            0.0f, 0.5f, 0.0f, 0.0f,
                            0.0f, 0.0f, 0.5f, 0.0f,
                            0.5f, 0.5f, 0.5f, 1.0f);

            set_current_modelview(view[i + 4]);
            set_current_projection(proj[i + 4]);

            mSunShadowMatrix[i + 4] = trans * proj[i + 4] * view[i + 4] * inv_view;

            set_last_modelview(mShadowModelview[i + 4]);
            set_last_projection(mShadowProjection[i + 4]);

            mShadowModelview[i + 4] = view[i + 4];
            mShadowProjection[i + 4] = proj[i + 4];

            if (!gCubeSnapshot) //skip updating spot shadow maps during cubemap updates
            {
                LLCamera shadow_cam = camera;
                shadow_cam.setFar(far_clip);
                shadow_cam.setOrigin(origin);

                LLViewerCamera::updateFrustumPlanes(shadow_cam, false, false, true);

                //

                mSpotShadow[i].bindTarget();
                mSpotShadow[i].getViewport(gGLViewport);
                mSpotShadow[i].clear();

                static LLCullResult result[2];

                LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SPOT_SHADOW0 + i);

                RenderSpotLight = drawable;

                // <plus> research/206 5b (H7): the groups the retained scene can draw, marked in its postSort and drawn
                // by its native pass (the scope ends with this block: nothing after renderShadow draws)
                LLSceneShadow::SpotScope scene_spot((U32)i, LLRHI::sNativeShadows && LLRHI::sDrawShadowPackets && LLRHI::sGLDrawsDisabled);
                // </plus>
                renderShadow(view[i + 4], proj[i + 4], shadow_cam, result[i], false);

                RenderSpotLight = nullptr;

                mSpotShadow[i].flush();
            }
        }
    }
    else
    { //no spotlight shadows
        mShadowSpotLight[0] = mShadowSpotLight[1] = NULL;
    }


    if (!CameraOffset)
    {
        set_current_modelview(saved_view);
        set_current_projection(saved_proj);
    }
    else
    {
        set_current_modelview(view[1]);
        set_current_projection(proj[1]);
        gGL.loadMatrix(glm::value_ptr(view[1]));
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.loadMatrix(glm::value_ptr(proj[1]));
        gGL.matrixMode(LLRender::MM_MODELVIEW);
    }
    gGL.setColorMask(true, true);

    set_last_modelview(last_modelview);
    set_last_projection(last_projection);

    popRenderTypeMask();

    if (!skip_avatar_update)
    {
        gAgentAvatarp->updateAttachmentVisibility(gAgentCamera.getCameraMode());
    }
}

void LLPipeline::renderGroups(LLRenderPass* pass, U32 type, bool texture)
{
    for (LLCullResult::sg_iterator i = sCull->beginVisibleGroups(); i != sCull->endVisibleGroups(); ++i)
    {
        LLSpatialGroup* group = *i;
        if (!group->isDead() &&
            (!sUseOcclusion || !group->isOcclusionState(LLSpatialGroup::OCCLUDED)) &&
            gPipeline.hasRenderType(group->getSpatialPartition()->mDrawableType) &&
            group->mDrawMap.find(type) != group->mDrawMap.end())
        {
            pass->renderGroup(group,type,texture);
        }
    }
}

void LLPipeline::renderRiggedGroups(LLRenderPass* pass, U32 type, bool texture)
{
    for (LLCullResult::sg_iterator i = sCull->beginVisibleGroups(); i != sCull->endVisibleGroups(); ++i)
    {
        LLSpatialGroup* group = *i;
        if (!group->isDead() &&
            (!sUseOcclusion || !group->isOcclusionState(LLSpatialGroup::OCCLUDED)) &&
            gPipeline.hasRenderType(group->getSpatialPartition()->mDrawableType) &&
            group->mDrawMap.find(type) != group->mDrawMap.end())
        {
            pass->renderRiggedGroup(group, type, texture);
        }
    }
}

void LLPipeline::profileAvatar(LLVOAvatar* avatar, bool profile_attachments)
{
    if (gGLManager.mGLVersion < 3.25f)
    { // profiling requires GL 3.3 or later
        return;
    }

    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    // don't continue to profile an avatar that is known to be too slow
    llassert(!avatar->isTooSlow());

    LLGLSLShader* cur_shader = LLGLSLShader::sCurBoundShaderPtr;

    mRT->deferredScreen.bindTarget();
    mRT->deferredScreen.clear();

    if (!profile_attachments)
    {
        // profile entire avatar all at once and readback asynchronously
        avatar->placeProfileQuery();

        LLTimer cpu_timer;

        generateImpostor(avatar, false, true);

        avatar->mCPURenderTime = (F32)cpu_timer.getElapsedTimeF32() * 1000.f;

        avatar->readProfileQuery(5); // allow up to 5 frames of latency
    }
    else
    {
        // profile attachments one at a time
        LLVOAvatar::attachment_map_t::iterator iter;
        LLVOAvatar::attachment_map_t::iterator begin = avatar->mAttachmentPoints.begin();
        LLVOAvatar::attachment_map_t::iterator end = avatar->mAttachmentPoints.end();

        for (iter = begin;
            iter != end;
            ++iter)
        {
            LLViewerJointAttachment* attachment = iter->second;
            for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                attachment_iter != attachment->mAttachedObjects.end();
                ++attachment_iter)
            {
                LLViewerObject* attached_object = attachment_iter->get();
                if (attached_object)
                {
                    // use gDebugProgram to do the GPU queries
                    gDebugProgram.clearStats();
                    gDebugProgram.placeProfileQuery(true);

                    generateImpostor(avatar, false, true, attached_object);
                    gDebugProgram.readProfileQuery(true, true);

                    attached_object->mGPURenderTime = gDebugProgram.mTimeElapsed / 1000000.f;
                }
            }
        }
    }

    mRT->deferredScreen.flush();

    if (cur_shader)
    {
        cur_shader->bind();
    }
}

void LLPipeline::generateImpostor(LLVOAvatar* avatar, bool preview_avatar, bool for_profile, LLViewerObject* specific_attachment)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("generateImpostor");
    LLRHI::GPUScope gpu_scope("impostor");
    LLGLState::checkStates();

    static LLCullResult result;
    result.clear();
    grabReferences(result);

    if (!avatar || avatar->isDead() || !avatar->mDrawable)
    {
        LL_WARNS_ONCE("AvatarRenderPipeline") << "Avatar is " << (avatar ? "not drawable" : "null") << LL_ENDL;
        return;
    }
    LL_DEBUGS_ONCE("AvatarRenderPipeline") << "Avatar " << avatar->getID() << " is drawable" << LL_ENDL;

    assertInitialized();

    // previews can't be muted or impostered
    bool visually_muted = !for_profile && !preview_avatar && avatar->isVisuallyMuted();
    LL_DEBUGS_ONCE("AvatarRenderPipeline") << "Avatar " << avatar->getID()
                              << " is " << ( visually_muted ? "" : "not ") << "visually muted"
                              << LL_ENDL;
    bool too_complex = !for_profile && !preview_avatar && avatar->isTooComplex();
    LL_DEBUGS_ONCE("AvatarRenderPipeline") << "Avatar " << avatar->getID()
                              << " is " << ( too_complex ? "" : "not ") << "too complex"
                              << LL_ENDL;

    pushRenderTypeMask();

    if (visually_muted || too_complex)
    {
        // only show jelly doll geometry
        andRenderTypeMask(LLPipeline::RENDER_TYPE_AVATAR,
                            LLPipeline::RENDER_TYPE_CONTROL_AV,
                            END_RENDER_TYPES);
    }
    else
    {
        //hide world geometry
        clearRenderTypeMask(
            RENDER_TYPE_SKY,
            RENDER_TYPE_WL_SKY,
            RENDER_TYPE_TERRAIN,
            RENDER_TYPE_GRASS,
            RENDER_TYPE_CONTROL_AV, // Animesh
            RENDER_TYPE_TREE,
            RENDER_TYPE_VOIDWATER,
            RENDER_TYPE_WATER,
            RENDER_TYPE_ALPHA_PRE_WATER,
            RENDER_TYPE_PASS_GRASS,
            RENDER_TYPE_HUD,
            RENDER_TYPE_PARTICLES,
            RENDER_TYPE_CLOUDS,
            RENDER_TYPE_HUD_PARTICLES,
            END_RENDER_TYPES
         );
    }

    if (specific_attachment && specific_attachment->isHUDAttachment())
    { //enable HUD rendering
        setRenderTypeMask(RENDER_TYPE_HUD, END_RENDER_TYPES);
    }

    S32 occlusion = sUseOcclusion;
    sUseOcclusion = 0;

    sReflectionRender = ! sRenderDeferred;

    sShadowRender = true;
    sImpostorRender = true;

    LLViewerCamera* viewer_camera = LLViewerCamera::getInstance();

    {
        markVisible(avatar->mDrawable, *viewer_camera);

        if (preview_avatar)
        {
            // Only show rigged attachments for preview
            // For the sake of performance and so that static
            // objects won't obstruct previewing changes
            LLVOAvatar::attachment_map_t::iterator iter;
            for (iter = avatar->mAttachmentPoints.begin();
                iter != avatar->mAttachmentPoints.end();
                ++iter)
            {
                LLViewerJointAttachment *attachment = iter->second;
                for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                    attachment_iter != attachment->mAttachedObjects.end();
                    ++attachment_iter)
                {
                    LLViewerObject* attached_object = attachment_iter->get();
                    if (attached_object)
                    {
                        if (attached_object->isRiggedMesh())
                        {
                            markVisible(attached_object->mDrawable->getSpatialBridge(), *viewer_camera);
                        }
                        else
                        {
                            // sometimes object is a linkset and rigged mesh is a child
                            LLViewerObject::const_child_list_t& child_list = attached_object->getChildren();
                            for (LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
                                iter != child_list.end(); iter++)
                            {
                                LLViewerObject* child = *iter;
                                if (child->isRiggedMesh())
                                {
                                    markVisible(attached_object->mDrawable->getSpatialBridge(), *viewer_camera);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        else
        {
            if (specific_attachment)
            {
                markVisible(specific_attachment->mDrawable->getSpatialBridge(), *viewer_camera);
            }
            else
            {
                LLVOAvatar::attachment_map_t::iterator iter;
                LLVOAvatar::attachment_map_t::iterator begin = avatar->mAttachmentPoints.begin();
                LLVOAvatar::attachment_map_t::iterator end = avatar->mAttachmentPoints.end();

                for (iter = begin;
                    iter != end;
                    ++iter)
                {
                    LLViewerJointAttachment* attachment = iter->second;
                    for (LLViewerJointAttachment::attachedobjs_vec_t::iterator attachment_iter = attachment->mAttachedObjects.begin();
                        attachment_iter != attachment->mAttachedObjects.end();
                        ++attachment_iter)
                    {
                        LLViewerObject* attached_object = attachment_iter->get();
                        if (attached_object)
                        {
                            markVisible(attached_object->mDrawable->getSpatialBridge(), *viewer_camera);
                        }
                    }
                }
            }
        }
    }

    stateSort(*LLViewerCamera::getInstance(), result);

    LLCamera camera = *viewer_camera;
    LLVector2 tdim;
    U32 resY = 0;
    U32 resX = 0;

    if (!preview_avatar)
    {
        const LLVector4a* ext = avatar->mDrawable->getSpatialExtents();
        LLVector3 pos(avatar->getRenderPosition()+avatar->getImpostorOffset());

        camera.lookAt(viewer_camera->getOrigin(), pos, viewer_camera->getUpAxis());

        LLVector4a half_height;
        half_height.setSub(ext[1], ext[0]);
        half_height.mul(0.5f);

        LLVector4a left;
        left.load3(camera.getLeftAxis().mV);
        left.mul(left);
        llassert(left.dot3(left).getF32() > F_APPROXIMATELY_ZERO);
        left.normalize3fast();

        LLVector4a up;
        up.load3(camera.getUpAxis().mV);
        up.mul(up);
        llassert(up.dot3(up).getF32() > F_APPROXIMATELY_ZERO);
        up.normalize3fast();

        tdim.mV[0] = fabsf(half_height.dot3(left).getF32());
        tdim.mV[1] = fabsf(half_height.dot3(up).getF32());

        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();

        F32 distance = (pos-camera.getOrigin()).length();
        F32 fov = atanf(tdim.mV[1]/distance)*2.f*RAD_TO_DEG;
        F32 aspect = tdim.mV[0]/tdim.mV[1];
        glm::mat4 persp = glm::perspective(glm::radians(fov), aspect, 1.f, 256.f);
        set_current_projection(persp);
        gGL.loadMatrix(glm::value_ptr(persp));

        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.pushMatrix();

        F32 ogl_mat[16];
        camera.getOpenGLTransform(ogl_mat);
        glm::mat4 mat = glm::make_mat4((GLfloat*) OGL_TO_CFR_ROTATION) * glm::make_mat4(ogl_mat);

        gGL.loadMatrix(glm::value_ptr(mat));
        set_current_modelview(mat);

        LLRHI::clearColor(0.0f,0.0f,0.0f,0.0f);
        gGL.setColorMask(true, true);

        // get the number of pixels per angle
        F32 pa = gViewerWindow->getWindowHeightRaw() / (RAD_TO_DEG * viewer_camera->getView());

        //get resolution based on angle width and height of impostor (double desired resolution to prevent aliasing)
        resY = llmin(nhpo2((U32) (fov*pa)), (U32) 512);
        resX = llmin(nhpo2((U32) (atanf(tdim.mV[0]/distance)*2.f*RAD_TO_DEG*pa)), (U32) 512);

        if (!for_profile)
        {
            if (!avatar->mImpostor.isComplete())
            {
                avatar->mImpostor.allocate(resX, resY, GL_RGBA, true);

                if (LLPipeline::sRenderDeferred)
                {
                    addDeferredAttachments(avatar->mImpostor, true);
                }

                gGL.getTexUnit(0)->bind(&avatar->mImpostor);
                gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_POINT);
                gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            }
            else if (resX != avatar->mImpostor.getWidth() || resY != avatar->mImpostor.getHeight())
            {
                avatar->mImpostor.resize(resX, resY);
            }

            avatar->mImpostor.bindTarget();
        }
    }

    F32 old_alpha = LLDrawPoolAvatar::sMinimumAlpha;

    if (visually_muted || too_complex)
    { //disable alpha masking for muted avatars (get whole skin silhouette)
        LLDrawPoolAvatar::sMinimumAlpha = 0.f;
    }

    if (preview_avatar || for_profile)
    {
        // previews and profiles don't care about imposters
        renderGeomDeferred(camera);
        renderGeomPostDeferred(camera);
    }
    else
    {
        avatar->mImpostor.clear();
        renderGeomDeferred(camera);

        renderGeomPostDeferred(camera);

        // Shameless hack time: render it all again,
        // this time writing the depth
        // values we need to generate the alpha mask below
        // while preserving the alpha-sorted color rendering
        // from the previous pass
        //
        sImpostorRenderAlphaDepthPass = true;
        // depth-only here...
        //
        gGL.setColorMask(false,false);
        renderGeomPostDeferred(camera);

        sImpostorRenderAlphaDepthPass = false;

    }

    LLDrawPoolAvatar::sMinimumAlpha = old_alpha;

    if (!for_profile)
    { //create alpha mask based on depth buffer (grey out if muted)
        if (LLPipeline::sRenderDeferred)
        {
            GLuint buff = GL_COLOR_ATTACHMENT0;
            LLRHI::drawBuffers(1, &buff);     // <plus> research/207 6.3 </plus>
        }

        LLGLDisable blend(GL_BLEND);

        if (visually_muted || too_complex)
        {
            gGL.setColorMask(true, true);
        }
        else
        {
            gGL.setColorMask(false, true);
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        LLGLDepthTest depth(GL_TRUE, GL_FALSE, GL_GREATER);

        gGL.flush();

        gGL.pushMatrix();
        gGL.loadIdentity();
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.pushMatrix();
        gGL.loadIdentity();

        static const F32 clip_plane = 0.99999f;

        gDebugProgram.bind();

        if (visually_muted)
        {   // Visually muted avatar
            LLColor4 muted_color(avatar->getMutedAVColor());
            LL_DEBUGS_ONCE("AvatarRenderPipeline") << "Avatar " << avatar->getID() << " MUTED set solid color " << muted_color << LL_ENDL;
            gGL.diffuseColor4fv( muted_color.mV );
        }
        else if (!preview_avatar)
        { //grey muted avatar
            LL_DEBUGS_ONCE("AvatarRenderPipeline") << "Avatar " << avatar->getID() << " MUTED set grey" << LL_ENDL;
            gGL.diffuseColor4fv(LLColor4::pink.mV );
        }

        gGL.begin(LLRender::TRIANGLES);
        {
            gGL.vertex3f(-1.f, -1.f, clip_plane);
            gGL.vertex3f(1.f, -1.f, clip_plane);
            gGL.vertex3f(1.f, 1.f, clip_plane);

            gGL.vertex3f(-1.f, -1.f, clip_plane);
            gGL.vertex3f(1.f, 1.f, clip_plane);
            gGL.vertex3f(-1.f, 1.f, clip_plane);
        }
        gGL.end();
        gGL.flush();

        gDebugProgram.unbind();

        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
    }

    if (!preview_avatar && !for_profile)
    {
        avatar->mImpostor.flush();
        avatar->setImpostorDim(tdim);
    }

    sUseOcclusion = occlusion;
    sReflectionRender = false;
    sImpostorRender = false;
    sShadowRender = false;
    popRenderTypeMask();

    if (!preview_avatar)
    {
        gGL.matrixMode(LLRender::MM_PROJECTION);
        gGL.popMatrix();
        gGL.matrixMode(LLRender::MM_MODELVIEW);
        gGL.popMatrix();
    }

    if (!preview_avatar && !for_profile)
    {
        avatar->mNeedsImpostorUpdate = false;
        avatar->mImpostorUnusable = false; // <plus> research/319 </plus>
        avatar->mImpostorWaitSince = 0.f; // <plus> research/319 </plus>
        avatar->cacheImpostorValues();
        avatar->mLastImpostorUpdateFrameTime = gFrameTimeSeconds;
    }

    LLVertexBuffer::unbind();
    LLGLState::checkStates();
}

bool LLPipeline::hasRenderBatches(const U32 type) const
{
    return sCull->getRenderMapSize(type) > 0;
}

LLCullResult::drawinfo_iterator LLPipeline::beginRenderMap(U32 type)
{
    return sCull->beginRenderMap(type);
}

LLCullResult::drawinfo_iterator LLPipeline::endRenderMap(U32 type)
{
    return sCull->endRenderMap(type);
}

LLCullResult::sg_iterator LLPipeline::beginAlphaGroups()
{
    return sCull->beginAlphaGroups();
}

LLCullResult::sg_iterator LLPipeline::endAlphaGroups()
{
    return sCull->endAlphaGroups();
}

LLCullResult::sg_iterator LLPipeline::beginRiggedAlphaGroups()
{
    return sCull->beginRiggedAlphaGroups();
}

LLCullResult::sg_iterator LLPipeline::endRiggedAlphaGroups()
{
    return sCull->endRiggedAlphaGroups();
}

bool LLPipeline::hasRenderType(const U32 type) const
{
    // STORM-365 : LLViewerJointAttachment::setAttachmentVisibility() is setting type to 0 to actually mean "do not render"
    // We then need to test that value here and return false to prevent attachment to render (in mouselook for instance)
    // TODO: reintroduce RENDER_TYPE_NONE in LLRenderTypeMask and initialize its mRenderTypeEnabled[RENDER_TYPE_NONE] to false explicitely
    return (type == 0 ? false : mRenderTypeEnabled[type]);
}

void LLPipeline::setRenderTypeMask(U32 type, ...)
{
    va_list args;

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        mRenderTypeEnabled[type] = true;
        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }
}

bool LLPipeline::hasAnyRenderType(U32 type, ...) const
{
    va_list args;

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        if (mRenderTypeEnabled[type])
        {
            va_end(args);
            return true;
        }
        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }

    return false;
}

void LLPipeline::pushRenderTypeMask()
{
    std::string cur_mask;
    cur_mask.assign((const char*) mRenderTypeEnabled, sizeof(mRenderTypeEnabled));
    mRenderTypeEnableStack.push(cur_mask);
}

void LLPipeline::popRenderTypeMask()
{
    if (mRenderTypeEnableStack.empty())
    {
        LL_ERRS() << "Depleted render type stack." << LL_ENDL;
    }

    memcpy(mRenderTypeEnabled, mRenderTypeEnableStack.top().data(), sizeof(mRenderTypeEnabled));
    mRenderTypeEnableStack.pop();
}

void LLPipeline::andRenderTypeMask(U32 type, ...)
{
    va_list args;

    bool tmp[NUM_RENDER_TYPES];
    for (U32 i = 0; i < NUM_RENDER_TYPES; ++i)
    {
        tmp[i] = false;
    }

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        if (mRenderTypeEnabled[type])
        {
            tmp[type] = true;
        }

        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }

    for (U32 i = 0; i < LLPipeline::NUM_RENDER_TYPES; ++i)
    {
        mRenderTypeEnabled[i] = tmp[i];
    }

}

void LLPipeline::clearRenderTypeMask(U32 type, ...)
{
    va_list args;

    va_start(args, type);
    while (type < END_RENDER_TYPES)
    {
        mRenderTypeEnabled[type] = false;

        type = va_arg(args, U32);
    }
    va_end(args);

    if (type > END_RENDER_TYPES)
    {
        LL_ERRS() << "Invalid render type." << LL_ENDL;
    }
}

void LLPipeline::setAllRenderTypes()
{
    for (U32 i = 0; i < NUM_RENDER_TYPES; ++i)
    {
        mRenderTypeEnabled[i] = true;
    }
}

void LLPipeline::clearAllRenderTypes()
{
    for (U32 i = 0; i < NUM_RENDER_TYPES; ++i)
    {
        mRenderTypeEnabled[i] = false;
    }
}

void LLPipeline::addDebugBlip(const LLVector3& position, const LLColor4& color)
{
    DebugBlip blip(position, color);
    mDebugBlips.push_back(blip);
}

void LLPipeline::hidePermanentObjects( std::vector<U32>& restoreList )
{
    //This method is used to hide any vo's from the object list that may have
    //the permanent flag set.

    U32 objCnt = gObjectList.getNumObjects();
    for (U32 i = 0; i < objCnt; ++i)
    {
        LLViewerObject* pObject = gObjectList.getObject(i);
        if ( pObject && pObject->flagObjectPermanent() )
        {
            LLDrawable *pDrawable = pObject->mDrawable;

            if ( pDrawable )
            {
                restoreList.push_back( i );
                hideDrawable( pDrawable );
            }
        }
    }

    skipRenderingOfTerrain( true );
}

void LLPipeline::restorePermanentObjects( const std::vector<U32>& restoreList )
{
    //This method is used to restore(unhide) any vo's from the object list that may have
    //been hidden because their permanency flag was set.

    std::vector<U32>::const_iterator itCurrent  = restoreList.begin();
    std::vector<U32>::const_iterator itEnd      = restoreList.end();

    U32 objCnt = gObjectList.getNumObjects();

    while ( itCurrent != itEnd )
    {
        U32 index = *itCurrent;
        LLViewerObject* pObject = NULL;
        if ( index < objCnt )
        {
            pObject = gObjectList.getObject( index );
        }
        if ( pObject )
        {
            LLDrawable *pDrawable = pObject->mDrawable;
            if ( pDrawable )
            {
                pDrawable->clearState( LLDrawable::FORCE_INVISIBLE );
                unhideDrawable( pDrawable );
            }
        }
        ++itCurrent;
    }

    skipRenderingOfTerrain( false );
}

void LLPipeline::skipRenderingOfTerrain( bool flag )
{
    pool_set_t::iterator iter = mPools.begin();
    while ( iter != mPools.end() )
    {
        LLDrawPool* pPool = *iter;
        U32 poolType = pPool->getType();
        if ( hasRenderType( pPool->getType() ) && poolType == LLDrawPool::POOL_TERRAIN )
        {
            pPool->setSkipRenderFlag( flag );
        }
        ++iter;
    }
}

void LLPipeline::hideObject( const LLUUID& id )
{
    LLViewerObject *pVO = gObjectList.findObject( id );

    if ( pVO )
    {
        LLDrawable *pDrawable = pVO->mDrawable;

        if ( pDrawable )
        {
            hideDrawable( pDrawable );
        }
    }
}

void LLPipeline::hideDrawable( LLDrawable *pDrawable )
{
    pDrawable->setState( LLDrawable::FORCE_INVISIBLE );
    markRebuild( pDrawable, LLDrawable::REBUILD_ALL);
    //hide the children
    LLViewerObject::const_child_list_t& child_list = pDrawable->getVObj()->getChildren();
    for ( LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
          iter != child_list.end(); iter++ )
    {
        LLViewerObject* child = *iter;
        LLDrawable* drawable = child->mDrawable;
        if ( drawable )
        {
            drawable->setState( LLDrawable::FORCE_INVISIBLE );
            markRebuild( drawable, LLDrawable::REBUILD_ALL);
        }
    }
}
void LLPipeline::unhideDrawable( LLDrawable *pDrawable )
{
    pDrawable->clearState( LLDrawable::FORCE_INVISIBLE );
    markRebuild( pDrawable, LLDrawable::REBUILD_ALL);
    //restore children
    LLViewerObject::const_child_list_t& child_list = pDrawable->getVObj()->getChildren();
    for ( LLViewerObject::child_list_t::const_iterator iter = child_list.begin();
          iter != child_list.end(); iter++)
    {
        LLViewerObject* child = *iter;
        LLDrawable* drawable = child->mDrawable;
        if ( drawable )
        {
            drawable->clearState( LLDrawable::FORCE_INVISIBLE );
            markRebuild( drawable, LLDrawable::REBUILD_ALL);
        }
    }
}
void LLPipeline::restoreHiddenObject( const LLUUID& id )
{
    LLViewerObject *pVO = gObjectList.findObject( id );

    if ( pVO )
    {
        LLDrawable *pDrawable = pVO->mDrawable;
        if ( pDrawable )
        {
            unhideDrawable( pDrawable );
        }
    }
}

void LLPipeline::skipRenderingShadows()
{
    LLGLDepthTest depth(GL_TRUE);

    for (S32 j = 0; j < 4; j++)
    {
        mRT->shadow[j].bindTarget();
        mRT->shadow[j].clear();
        mRT->shadow[j].flush();
    }
}

void LLPipeline::handleShadowDetailChanged()
{
    if (RenderShadowDetail > gSavedSettings.getS32("RenderShadowDetail"))
    {
        skipRenderingShadows();
    }
    else
    {
        LLViewerShaderMgr::instance()->setShaders();
    }
}

class LLOctreeDirty : public OctreeTraveler
{
public:
    virtual void visit(const OctreeNode* state)
    {
        LLSpatialGroup* group = (LLSpatialGroup*)state->getListener(0);

        if (group->getSpatialPartition()->mRenderByGroup)
        {
            group->setState(LLSpatialGroup::GEOM_DIRTY);
            gPipeline.markRebuild(group);
        }

        for (LLSpatialGroup::bridge_list_t::iterator i = group->mBridgeList.begin(); i != group->mBridgeList.end(); ++i)
        {
            LLSpatialBridge* bridge = *i;
            traverse(bridge->mOctree);
        }
    }
};

// Called from LLViewHighlightTransparent when "Highlight Transparent" is toggled
void LLPipeline::rebuildDrawInfo()
{
    const U32 types_to_traverse[] =
    {
        LLViewerRegion::PARTITION_VOLUME,
        LLViewerRegion::PARTITION_BRIDGE,
        LLViewerRegion::PARTITION_AVATAR
    };

    LLOctreeDirty dirty;
    for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
    {
        for (U32 type : types_to_traverse)
        {
            LLSpatialPartition* part = region->getSpatialPartition(type);
            dirty.traverse(part->mOctree);
        }
    }
}

void LLPipeline::rebuildTerrain()
{
    for (LLWorld::region_list_t::const_iterator iter = LLWorld::getInstance()->getRegionList().begin();
        iter != LLWorld::getInstance()->getRegionList().end(); ++iter)
    {
        LLViewerRegion* region = *iter;
        region->dirtyAllPatches();
    }
}

// ----------------------------------------------------------------------------
// LLRHI: sun shadow cascades sharing one state sort.
//
// Each cascade is still culled with its own camera (cheap octree traversal),
// but the expensive state sort and render map building runs once over the
// union of what the cascades see. Every cascade is then drawn with exactly
// the draw infos of the groups its own cull found, so the shadow maps are the
// same as drawing the cascades one by one. Objects drawn through face pools
// (avatars, terrain, trees) are drawn into every cascade; the GPU clips what
// lies outside.

namespace
{
    // A static shadow cache's numbers over a stats period (the sun's cascades, the probes' shared maps).
    struct ShadowCacheStats
    {
        U32 cascades_drawn = 0, cascades_reused = 0, static_redrawn = 0, frames = 0;
        F32 logged_at = 0.f;
        // Why static caches were redrawn.
        U32 redraw_projection = 0, redraw_contents = 0, redraw_cull = 0, redraw_age = 0, redraw_size = 0;
        // Why the projection changed. Sun cascades: sun, receivers outside, resolution.
        // Probe maps: sun, probe moved, other (splits, map size, draw distance).
        U32 projection_light = 0, projection_outside = 0, projection_span = 0;
        U32 projection_clip = 0;    // sun cascades: the water clip plane changed
        // <plus> plus/shadowdist (research/178 C1): where the first receiver outside the cached
        // cascade was: across the map (x, y), along the light (z only), or behind its eye
        U32 outside_xy = 0, outside_z = 0, outside_behind = 0;
        // </plus>
        // Draw infos per cascade, summed over the frames of a stats period.
        U64 static_infos = 0, joining_infos = 0, deferred_infos = 0;
        U64 moving_bridge = 0, moving_unsettled = 0, moving_rigged = 0, moving_animated = 0;
        U32 groups_seen = 0, groups_changed = 0;
        // LLRHI: the receiver cull (research/64): moving draw infos per cascade
        // (by its index) it tested and left out, and frames it was on, held off
        // by anisotropic shadow maps, or held off otherwise (switch on).
        U64 receiver_tested[4] = {}, receiver_skipped[4] = {};
        U32 receiver_cascades_inactive = 0;
        U32 receiver_frames_on = 0, receiver_frames_aniso = 0, receiver_frames_held = 0;
    };
    ShadowCacheStats sSunCacheStats, sProbeCacheStats;
    inline ShadowCacheStats& shadow_cache_stats(LLPipeline::EShadowCache cache)
    {
        return cache == LLPipeline::SHADOW_CACHE_PROBE ? sProbeCacheStats : sSunCacheStats;
    }
    // Native shadow packets of every shadow pass, and how many were merged into the one before.
    U64 sNativePackets = 0, sNativeMerged = 0;
    // LLRHI (research/66 step 2B): building those packets, main thread side
    // (the cull threads' part is in RHIPacketBuild). Since the last log.
    struct RHIPacketStats
    {
        U32 passes = 0;             // drawNativeShadowCasters calls
        U32 threaded = 0;           // of them, built with the cull threads
        U64 build_ns = 0;           // the main thread building packets (either way), up to sDrawShadowPackets
        U64 infos = 0;              // opaque batches handed to chunks
        U32 chunks = 0;
        U32 main_chunks = 0;        // chunks the main thread built itself
        U64 main_chunk_ns = 0;
        U64 impure_ns = 0;          // the rigged and alpha types (palettes, texture names), meanwhile
        U64 wait_ns = 0;            // waiting for chunks a cull thread was building
        U64 fold_ns = 0;            // putting them together under the list lock
        U32 fallbacks = 0;          // opaque batches with an avatar, built on the main thread
    };
    RHIPacketStats sRHIPacketStats;
    std::atomic<U64> sRHIPacketWorkerNs{ 0 };      // the cull threads building chunks
    std::atomic<U32> sRHIPacketWorkerChunks{ 0 };

    // A group joins the static cache once its casters have not changed for this long.
    constexpr F32 SHADOW_SETTLE_SECONDS = 3.f;
    // The one expression for "settled by time t": the cache and the check must agree bit for bit.
    inline bool settled_by(F32 changed_at, F32 t)
    {
        return t - changed_at > SHADOW_SETTLE_SECONDS;
    }

    inline U64 mix64(U64 x)    // splitmix64 finalizer
    {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x;
    }

    inline U64 shadow_tex_name(LLViewerTexture* texture)
    {
        LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
        return image ? (U64)image->getTexName() : 0;
    }

    inline U64 float_bits(F32 value)
    {
        U32 bits = 0;
        memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    // Render types whose shadow is the bare volume (shadowV, depth only):
    // their textures do not change the shadow map.
    bool shadow_ignores_textures(U32 type)
    {
        switch (type)
        {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_FULLBRIGHT:
        case LLRenderPass::PASS_SHINY:
        case LLRenderPass::PASS_BUMP:
        case LLRenderPass::PASS_FULLBRIGHT_SHINY:
        case LLRenderPass::PASS_MATERIAL:
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE:
        case LLRenderPass::PASS_SPECMAP:
        case LLRenderPass::PASS_SPECMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMMAP:
        case LLRenderPass::PASS_NORMMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMSPEC:
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE:
        case LLRenderPass::PASS_GLTF_PBR:
            return true;
        default:
            return false;
        }
    }

    // What the cached depth depends on, per draw info. salt: the group's rebuild
    // counts (a rebuild may reuse the addresses of the infos and buffers it frees;
    // an in-place mesh rebuild changes colors or positions under the same infos).
    // Cascades sum these: depth only, so the order things are drawn in does not
    // matter, and the sum does not depend on the order the cull visits groups in.
    U64 shadow_content_hash(LLDrawInfo* info, U32 type, U32 salt)
    {
        U64 h = (U64)(uintptr_t)info ^ ((U64)(uintptr_t)info->mVertexBuffer.get() << 7) ^ ((U64)info->mOffset << 21) ^
                ((U64)info->mCount << 37) ^ ((U64)type << 53) ^ (U64)(uintptr_t)info->mModelMatrix;
        // GLTF: the material decides double sidedness (and the alpha mask below).
        LLFetchedGLTFMaterial* material = info->mGLTFMaterial.get();
        h = mix64(h) ^ ((U64)salt << 32) ^ (U64)(uintptr_t)material;
        if (!shadow_ignores_textures(type))
        {
            // Alpha tested casters: the textures' alpha and the cutoff shape the shadow.
            h = mix64(h ^ shadow_tex_name(info->mTexture.get()));
            for (size_t i = 0; i < info->mTextureList.size() && i < 4; ++i)
            {
                h = mix64(h ^ shadow_tex_name(info->mTextureList[i].get()) ^ ((U64)(i + 1) << 56));
            }
            h = mix64(h ^ float_bits(info->mAlphaMaskCutoff));
            if (material)
            {
                h = mix64(h ^ float_bits(material->mAlphaCutoff) ^ (shadow_tex_name(material->mBaseColorTexture.get()) << 32));
            }
        }
        return mix64(h);
    }

    // Open addressing set of pointers; keeps its storage from frame to frame.
    class PointerSet
    {
    public:
        void reset(size_t expected)
        {
            size_t size = 256;
            while (size < expected * 2)
            {
                size <<= 1;
            }
            if (mSlots.size() < size)
            {
                mSlots.assign(size, nullptr);
            }
            else
            {
                mSlots.assign(mSlots.size(), nullptr);
            }
            mMask = mSlots.size() - 1;
        }
        // True when p was not in the set yet.
        bool insert(const void* p)
        {
            size_t i = (size_t)(mix64((U64)(uintptr_t)p) & mMask);
            while (mSlots[i])
            {
                if (mSlots[i] == p)
                {
                    return false;
                }
                i = (i + 1) & mMask;
            }
            mSlots[i] = p;
            return true;
        }

    private:
        std::vector<const void*> mSlots;
        size_t mMask = 0;
    };

    inline U32 bit_count(U32 mask)
    {
        U32 n = 0;
        for (; mask; mask &= mask - 1)
        {
            ++n;
        }
        return n;
    }

    // LLRHI: the static shadow cache's sort of a cascade set's groups on the
    // cull threads (LLRHI::sShadowSortThreads, research/57). Each chunk of
    // groups lists its draw infos in the order the serial sort visits them, so
    // the main thread can fill the per cascade lists in that same order without
    // touching a group or a draw info again.
    constexpr size_t SHADOW_SORT_CHUNK = 32;    // groups per chunk
    constexpr U8 SHADOW_SORT_MOVING = 0xff;     // ShadowSortEntry::cached: drawn every frame
    enum EShadowSortWhy : U8
    {
        SHADOW_SORT_BRIDGE,     // moving: on a spatial bridge
        SHADOW_SORT_RIGGED,     // moving: rigged
        SHADOW_SORT_ANIMATED,   // moving: animated texture
        SHADOW_SORT_STATIC      // static (moving while its group has not settled)
    };
    // <plus> research/206: a group the retained scene draws, and its cascades (sort positions' bits)
    struct ShadowSceneGroup
    {
        U32 id;
        U8 moving;              // its moving casters'
        U8 cached;              // 5b: the cached depths that take it
        U32 g;                  // 5c: its place in the sort's groups (gSceneShadow.noteGroups: its masks' textures)
    };
    // </plus>
    struct ShadowSortEntry
    {
        LLDrawInfo* info;
        U64 hash;               // shadow_content_hash, static infos only
        U32 type;
        U8 mask;                // cascades that saw the group
        U8 cached;              // static: cascades whose cached depth takes it (the rest: joining); or SHADOW_SORT_MOVING
        U8 why;                 // EShadowSortWhy
        U8 scene;               // <plus> research/206: cascades the retained scene draws it in (or would: verify): moving, in their moving passes; static (5b), in their cached depths </plus>
    };
    // Written by one thread at a time; aligned so two chunks never share a cache line.
    struct alignas(64) ShadowSortChunk
    {
        std::vector<ShadowSortEntry> entries;
        U64 fingerprint[4] = {};
        U64 joining_fingerprint[4] = {};
        U32 changed_mask = 0;
        U32 groups_seen = 0, groups_changed = 0;
        U32 plus_first_seen = 0;    // <plus> plus/shadowshift S4(b): groups settled at first sight </plus>
        U64 static_infos = 0, joining_infos = 0;
        U64 moving_bridge = 0, moving_unsettled = 0, moving_rigged = 0, moving_animated = 0;
        U64 receiver_tested[4] = {}, receiver_skipped[4] = {};     // per cascade position (research/64)
        std::vector<ShadowSceneGroup> scene_groups;     // <plus> research/206 </plus>
    };
    struct ShadowSortState
    {
        std::vector<ShadowSortChunk> chunks;    // keeps its storage from frame to frame
        size_t used = 0;
        // Cascade k's cached casters, still to be listed from the chunks (only
        // while renderSunShadowCascades runs; renderCachedShadowCascade fills them
        // when it redraws the cached depth).
        LLCullResult* lazy[4] = {};
    };
    ShadowSortState sShadowSort;

    // The cached casters of the cascade whose list target is, as the serial sort
    // would have pushed them, if they were left to be listed on demand.
    void shadow_sort_fill(LLCullResult* target)
    {
        for (U32 k = 0; k < 4; ++k)
        {
            if (!target || sShadowSort.lazy[k] != target)
            {
                continue;
            }
            sShadowSort.lazy[k] = nullptr;
            const U8 bit = (U8)(1u << k);
            const bool scene_cut = gSceneShadow.cutting(); // <plus> research/206 5b: the retained scene draws its groups' share </plus>
            U32 scene_batches = 0;                         // <plus> research/206 5b </plus>
            for (size_t c = 0; c < sShadowSort.used; ++c)
            {
                for (const ShadowSortEntry& entry : sShadowSort.chunks[c].entries)
                {
                    if (entry.cached != SHADOW_SORT_MOVING && (entry.cached & bit))
                    {
                        // <plus> research/206 5b: drawn by the retained scene in this redraw (CacheScope)
                        if (scene_cut && (entry.scene & bit))
                        {
                            ++scene_batches;
                            continue;
                        }
                        // </plus>
                        target->pushDrawInfo(entry.type, entry.info);
                    }
                }
            }
            gSceneShadow.noteCacheCut(scene_batches); // <plus> research/206 5b </plus>
            return;
        }
    }

    // <plus> plus/shadowshift (research/151): the sort's position k of a cascade's
    // cached casters (their bit in the entries) while they are still to be listed
    // (before shadow_sort_fill); -1 when the sort did not run in chunks.
    S32 shadow_sort_position(const LLCullResult* target)
    {
        for (U32 k = 0; k < 4; ++k)
        {
            if (target && sShadowSort.lazy[k] == target)
            {
                return (S32)k;
            }
        }
        return -1;
    }

    // Every settled static entry position k saw, cached or joining, with its hash.
    template <class Visit>
    void shadow_sort_visit_static(S32 k, Visit&& visit)
    {
        if (k < 0 || k > 3)
        {
            return;
        }
        const U8 bit = (U8)(1u << k);
        for (size_t c = 0; c < sShadowSort.used; ++c)
        {
            for (const ShadowSortEntry& entry : sShadowSort.chunks[c].entries)
            {
                if (entry.cached != SHADOW_SORT_MOVING && (entry.mask & bit))
                {
                    visit(entry.type, entry.info, entry.hash, (entry.cached & bit) != 0);
                }
            }
        }
    }
    // </plus>

    // <plus> research/206: the batches position k leaves to the retained scene (cut, or verifying: would
    // cut): its moving casters', or (5b) its cached depth's
    template <class Visit>
    void shadow_sort_visit_scene(U32 k, bool cached, Visit&& visit)
    {
        if (k > 3)
        {
            return;
        }
        const U8 bit = (U8)(1u << k);
        for (size_t c = 0; c < sShadowSort.used; ++c)
        {
            for (const ShadowSortEntry& entry : sShadowSort.chunks[c].entries)
            {
                const bool layer = cached ? entry.cached != SHADOW_SORT_MOVING && (entry.cached & bit) : entry.cached == SHADOW_SORT_MOVING;
                if (layer && (entry.scene & bit))
                {
                    visit(entry.type, entry.info);
                }
            }
        }
    }

    // The batches the native pass being built leaves to the retained scene (gSceneShadow's layer):
    // a sun cascade's moving casters or cached depth from the sort's entries, a spot pass's groups'
    // batches of the cut types (5b: as its postSort left them out of the render map).
    template <class Visit>
    void scene_layer_visit(Visit&& visit)
    {
        switch (gSceneShadow.layer())
        {
            case LLSceneShadow::LAYER_MOVING:
            case LLSceneShadow::LAYER_CACHE:
                shadow_sort_visit_scene(gSceneShadow.layerPosition(), gSceneShadow.layer() == LLSceneShadow::LAYER_CACHE, visit);
                break;
            case LLSceneShadow::LAYER_SPOT:
                for (LLSpatialGroup* group : gSceneShadow.spotGroups())
                {
                    for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
                    {
                        if (!gSceneShadow.cutType(j->first))
                        {
                            continue;
                        }
                        for (LLDrawInfo* info : j->second)
                        {
                            visit(j->first, info);
                        }
                    }
                }
                break;
            default:
                break;
        }
    }
    // </plus>

    // LLRHI: a few threads for the cull (research/48). run() hands the indices
    // 0..count-1 to them and to the calling thread, and returns when all are done
    // (fork and join: the main thread does nothing else meanwhile). If the threads
    // are slow to wake, the calling thread does the work itself.
    class CullWorkers
    {
    public:
        typedef std::function<void(U32)> job_t;

        static CullWorkers& instance()
        {
            // Never destroyed: the threads sleep on the condition variable until the process exits.
            static CullWorkers* workers = new CullWorkers();
            return *workers;
        }

        void run(U32 count, const job_t& job)
        {
            if (count <= 1 || mThreads == 0)
            {
                for (U32 i = 0; i < count; ++i)
                {
                    job(i);
                }
                return;
            }
            U32 generation;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                generation = ++mGeneration;
                mJob = &job;
                mCount = count;
                mRemaining = count;
                mNext.store((U64)generation << 32, std::memory_order_release);
            }
            mWake.notify_all();
            work(generation, count, job);
            std::unique_lock<std::mutex> lock(mMutex);
            mDone.wait(lock, [this] { return mRemaining == 0; });
            mJob = nullptr;
        }

        // LLRHI (research/66 step 2): fn(arg) on one of the threads, without
        // waiting for it (a thread takes it once it has nothing of run() to do).
        // False when there are no threads: the caller does it.
        bool post(void (*fn)(void*), void* arg)
        {
            if (mThreads == 0)
            {
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mTasks.push_back({ fn, arg });
            }
            mWake.notify_one();
            return true;
        }

        U32 threads() const { return mThreads; }

    private:
        CullWorkers()
        {
            const U32 cores = std::thread::hardware_concurrency();
            // <plus> research/184 A1: PlusCullThreads (1 to 4; 0: as before) (was: llmin(cores - 1, 3u))
            mThreads = cores > 1 ? plus::cullinto::cullThreads(cores) : 0;
            // </plus>
            for (U32 i = 0; i < mThreads; ++i)
            {
                std::thread([this] { loop(); }).detach();
            }
            LL_INFOS("RHIProfile") << "cull threads: " << mThreads << LL_ENDL;
        }

        // An index of this generation not yet handed out. The generation is in the
        // ticket, so a thread that wakes late can never take an index of a later run.
        bool claim(U32 generation, U32 count, U32& index)
        {
            U64 ticket = mNext.load(std::memory_order_acquire);
            for (;;)
            {
                if ((U32)(ticket >> 32) != generation || (U32)ticket >= count)
                {
                    return false;
                }
                if (mNext.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                {
                    index = (U32)ticket;
                    return true;
                }
            }
        }

        void work(U32 generation, U32 count, const job_t& job)
        {
            U32 index;
            while (claim(generation, count, index))
            {
                // LLRHI fix-threads4: an index that throws (bad_alloc, ...) counts as
                // done (logged): run()'s caller must not wait for it, and an exception
                // leaving a cull thread would end the process.
                try
                {
                    job(index);
                }
                catch (...)
                {
                    static std::atomic<U32> sFailures{ 0 };
                    if (sFailures.fetch_add(1, std::memory_order_relaxed) < 5)
                    {
                        try
                        {
                            LL_WARNS("RHIProfile") << "a cull job failed: its part of this frame's culling is missing" << LL_ENDL;
                        }
                        catch (...)
                        {
                        }
                    }
                }
                std::lock_guard<std::mutex> lock(mMutex);
                if (--mRemaining == 0)
                {
                    mDone.notify_all();
                }
            }
        }

        void loop()
        {
            set_thread_name("Cull");
            U32 seen = 0;
            for (;;)
            {
                const job_t* job = nullptr;
                U32 generation = 0, count = 0;
                Task task = {};
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mWake.wait(lock, [&] { return mGeneration != seen || !mTasks.empty(); });
                    if (mGeneration != seen)
                    {
                        // A run() first: its caller waits for it.
                        seen = generation = mGeneration;
                        job = mJob;
                        count = mCount;
                    }
                    else
                    {
                        task = mTasks.front();
                        mTasks.pop_front();
                    }
                }
                if (job)
                {
                    // job stays valid while an index of its generation is unfinished.
                    work(generation, count, *job);
                }
                else if (task.fn)
                {
                    // LLRHI fix-round7: the last net. Every posted task catches its
                    // own (whoever waits for it is told by the task); one that does
                    // not would end the process from this thread.
                    try
                    {
                        task.fn(task.arg);
                    }
                    catch (...)
                    {
                        static std::atomic<U32> sTaskFailures{ 0 };
                        if (sTaskFailures.fetch_add(1, std::memory_order_relaxed) < 5)
                        {
                            try
                            {
                                LL_WARNS("RHIProfile") << "a cull thread task failed" << LL_ENDL;
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                }
            }
        }

        U32 mThreads = 0;
        std::mutex mMutex;
        std::condition_variable mWake;
        struct Task
        {
            void (*fn)(void*);
            void* arg;
        };
        std::deque<Task> mTasks;    // post(), in order
        std::condition_variable mDone;
        const job_t* mJob = nullptr;
        U32 mGeneration = 0;
        U32 mCount = 0;
        U32 mRemaining = 0;
        std::atomic<U64> mNext{ 0 };
    };

    void run_cull_jobs(U32 count, const std::function<void(U32)>& job)
    {
        CullWorkers::instance().run(count, job);
    }

    // LLRHI::sPostWork (research/66 step 2).
    bool rhi_post_work(void (*fn)(void*), void* arg)
    {
        return LLRHI::sCullThreads && CullWorkers::instance().post(fn, arg); // LLRHI native (research/137 N11): none while the cull threads are off
    }

    // LLRHI native (research/137 N11): set once, before main: the draw lists'
    // cull thread work (LLRHI_DRAW_LISTS_THREADS) has it with the sun shadows
    // off too, and the translation thread, started later, reads it without a
    // lock. Every caller is behind its own switch. renderSunShadowCascades
    // then finds it set and leaves it.
    [[maybe_unused]] const bool sRHIPostWorkSet = (LLRHI::sPostWork = &rhi_post_work, true); // LLRHI native (research/137 N11)

    // LLRHI (research/66 step 2): a native shadow pass a cull thread may still be
    // recording reads the packets drawNativeShadowCasters built for it until the
    // join, so while LLRHI::sShadowRecordBin >= 0 each pass gets its own array.
    constexpr U32 RHI_PACKET_SETS = 8;  // a window's passes: 4 cascades x (cached depth + moving casters)
    U32 sRHIPacketSetsUsed = 0;         // arrays handed out since the last join

    // Every pass handed to a cull thread recorded; their packet arrays free again.
    void rhi_join_shadow_recording()
    {
        if (LLRHI::sJoinShadowRecording)
        {
            LLRHI::sJoinShadowRecording();
        }
        sRHIPacketSetsUsed = 0;
    }

    // ------------------------------------------------------------------------
    // LLRHI: the shadow receiver cull (LLRHI::sShadowReceiverCull, research/64).
    //
    // Who reads sun cascade k: sampleDirectionalShadow (class1/deferred/
    // shadowUtil.glsl), from the deferred sun light, the alpha, material, PBR
    // alpha, GLTF and water shaders of the main view, and from the alpha
    // shaders of this frame's impostor renders (which get the main view's
    // shadow matrices with their own eye space). A pixel at eye space P moves
    // to Q = P + light_dir * (1 - n.l) * 2 * shadow_offset, at most
    // 2 * shadow_offset away, and reads cascade k when Q's eye depth d is in
    //   k = 0: d < 1.25 c0       k = 1: 0.75 c0 < d < 1.25 c1
    //   k = 2: 0.75 c1 < d < 1.25 c2     k = 3: 0.75 c2 < d
    // and d < c3 for every k (c: mSunClipPlanes, the shader's shadow_clip).
    // In cascade k's NDC it reads texels whose centres are within 3.5 texels
    // of Q in x (x snapped with a jitter under a texel, taps at +-1.5, then
    // bilinear) and 2.5 in y, clamped to the edge, and compares them (LEQUAL)
    // with Q's depth plus 2 * shadow_bias. Light runs along NDC +z: the
    // texel does not change along it, the depth grows.
    //
    // A caster X changes such a read only if it covers one of those texels
    // with a depth below Q's reference: Q - X in K = [-mx,mx] x [-my,my] x
    // (-gamma, inf). With the receivers R (the frustum slices, moved by the
    // offset) and the caster's box both in NDC (both convex there: every w is
    // positive), a direction n with n.z >= 0 and
    //   min over X of n.X  >  max over Q of n.Q + mx|n.x| + my|n.y| + gamma n.z
    // separates them: the caster changes no read of cascade k. Receivers past
    // the map's edge read the edge's texels, so a caster reaching an edge that
    // some receiver is past is kept.
    constexpr F64 RECV_MIN_W = 1e-4;
    constexpr U32 RECV_MAX_PLANES = 64;
    constexpr U32 RECV_MAX_POINTS = 2 * 8 * 8;     // two slices, 8 corners, 8 offsets each

    struct RecvPlane
    {
        F64 n[3];
        F64 thr;
    };
    struct RecvCascade
    {
        bool active = false;
        glm::dmat4 to_clip;                 // agent space -> the cascade's clip space
        F64 mx = 0.0, my = 0.0;             // reach of a read around its receiver, NDC
        F64 cover_x = 0.0, cover_y = 0.0;   // slack around a caster's box that still covers nothing, NDC
        bool border[4] = {};                // +x, -x, +y, -y: receivers past this edge read its texels
        F64 strip_lo[4] = {}, strip_hi[4] = {};     // the caster range along that edge that may touch them
        RecvPlane planes[RECV_MAX_PLANES];
        U32 plane_count = 0;
    };
    struct RecvSetup
    {
        glm::dmat4 eye_to_clip;     // the main view's eye space -> the cascade's clip space
        glm::dmat4 agent_to_clip;
        glm::dmat4 inv_proj;        // the main view's
        glm::dvec3 down_eye;        // the way light travels, eye space, unit
        F64 lo = 0.0, hi = 0.0;     // band of Q's eye depth (lo below 0: no lower end)
        F64 offset = 0.0;           // |Q - P| at most
        F64 tan_impostor = 0.0;     // half angle tangent of this frame's impostor renders, 0: none
        F64 texel_x = 0.0, texel_y = 0.0;   // one texel, NDC
        F64 gamma = 0.0;            // a read's depth reference above Q's own, NDC
    };

    // Where the cascade's receivers read it; false (cascade not culled) when
    // that cannot be bounded.
    bool recv_build(RecvCascade& c, const RecvSetup& s)
    {
        c.active = false;
        c.plane_count = 0;
        // Eye depths of P: within offset of the band, in front of the eye.
        const F64 a = llmax(s.lo - s.offset, 0.0);
        const F64 b = s.hi + s.offset;
        if (!(b > a))
        {
            return false;
        }
        // Corners of the depth slices P lies in: the main view's frustum, and
        // the impostor renders' (their eye space comes with the main view's
        // shadow matrices; their frustum is symmetric with tangents at most
        // tan_impostor). 0..3 at depth a, 4..7 at depth b.
        static const F64 sx[4] = { -1.0, 1.0, 1.0, -1.0 };
        static const F64 sy[4] = { -1.0, -1.0, 1.0, 1.0 };
        glm::dvec3 slice[2][8];
        U32 slices = 1;
        for (U32 i = 0; i < 4; ++i)
        {
            const glm::dvec4 n = s.inv_proj * glm::dvec4(sx[i], sy[i], -1.0, 1.0);
            const glm::dvec4 f = s.inv_proj * glm::dvec4(sx[i], sy[i], 1.0, 1.0);
            if (n.w == 0.0 || f.w == 0.0)
            {
                return false;
            }
            const glm::dvec3 near_pt = glm::dvec3(n) / n.w;
            const glm::dvec3 far_pt = glm::dvec3(f) / f.w;
            const F64 dn = -near_pt.z;
            const F64 df = -far_pt.z;
            // A perspective view: the corner's edge is a ray from the eye.
            if (!(df > dn && dn > 0.0) || glm::length(near_pt - far_pt * (dn / df)) > 1e-4 * (1.0 + df))
            {
                return false;
            }
            slice[0][i] = far_pt * (a / df);
            slice[0][i + 4] = far_pt * (b / df);
        }
        if (s.tan_impostor > 0.0)
        {
            const F64 t = s.tan_impostor;
            for (U32 i = 0; i < 4; ++i)
            {
                slice[1][i] = glm::dvec3(sx[i] * t * a, sy[i] * t * a, -a);
                slice[1][i + 4] = glm::dvec3(sx[i] * t * b, sy[i] * t * b, -b);
            }
            slices = 2;
        }

        // R: every corner moved by up to offset along each eye axis (a cube
        // around it holds the ball Q lies in), in the cascade's NDC.
        glm::dvec3 r[RECV_MAX_POINTS];
        U32 count = 0;
        F64 lo3[3] = { DBL_MAX, DBL_MAX, DBL_MAX };
        F64 hi3[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
        F64 extent = 0.0;
        for (U32 q = 0; q < slices; ++q)
        {
            for (U32 i = 0; i < 8; ++i)
            {
                for (U32 o = 0; o < 8; ++o)
                {
                    const glm::dvec3 e = slice[q][i] + glm::dvec3((o & 1) ? s.offset : -s.offset,
                                                                  (o & 2) ? s.offset : -s.offset,
                                                                  (o & 4) ? s.offset : -s.offset);
                    const glm::dvec4 clip = s.eye_to_clip * glm::dvec4(e, 1.0);
                    if (!(clip.w > RECV_MIN_W))
                    {
                        return false;
                    }
                    const glm::dvec3 p = glm::dvec3(clip) / clip.w;
                    for (U32 k = 0; k < 3; ++k)
                    {
                        if (!std::isfinite(p[k]))
                        {
                            return false;
                        }
                        lo3[k] = llmin(lo3[k], p[k]);
                        hi3[k] = llmax(hi3[k], p[k]);
                        extent = llmax(extent, fabs(p[k]));
                    }
                    r[count++] = p;
                }
            }
        }

        // Light runs along NDC +z: x and y stay, the depth grows.
        {
            const glm::dvec3 e = slice[0][4];
            const glm::dvec4 c0 = s.eye_to_clip * glm::dvec4(e, 1.0);
            const glm::dvec4 c1 = s.eye_to_clip * glm::dvec4(e + s.down_eye, 1.0);
            if (!(c0.w > RECV_MIN_W) || !(c1.w > RECV_MIN_W))
            {
                return false;
            }
            const glm::dvec3 p0 = glm::dvec3(c0) / c0.w;
            const glm::dvec3 p1 = glm::dvec3(c1) / c1.w;
            if (!(fabs(p1.x - p0.x) + fabs(p1.y - p0.y) < 1e-4) || !(p1.z > p0.z))
            {
                return false;
            }
        }

        // Reach of a read around Q: 3.5 texels in x, 2.5 in y, plus a texel for
        // the rasteriser's vertex snapping and float error.
        c.mx = 4.5 * s.texel_x;
        c.my = 3.5 * s.texel_y;
        c.cover_x = s.texel_x;
        c.cover_y = s.texel_y;
        // The GPU's float error in its own transform of a receiver.
        const F64 eps = 1e-5 * (1.0 + extent);

        // Receivers past an edge (clamp to edge): reads of its texels, from
        // the receivers' range along it.
        c.border[0] = hi3[0] + c.mx > 1.0;
        c.border[1] = lo3[0] - c.mx < -1.0;
        c.border[2] = hi3[1] + c.my > 1.0;
        c.border[3] = lo3[1] - c.my < -1.0;
        for (U32 side = 0; side < 4; ++side)
        {
            const U32 along = side < 2 ? 1 : 0;     // the other coordinate
            const F64 reach = along == 1 ? c.my : c.mx;
            c.strip_lo[side] = llclamp(lo3[along] - reach, -1.0, 1.0) - reach - eps;
            c.strip_hi[side] = llclamp(hi3[along] + reach, -1.0, 1.0) + reach + eps;
        }

        auto add_plane = [&](F64 nx, F64 ny, F64 nz)
        {
            const F64 len = sqrt(nx * nx + ny * ny + nz * nz);
            if (!(len > 1e-12) || c.plane_count >= RECV_MAX_PLANES)
            {
                return;
            }
            nx /= len;
            ny /= len;
            nz /= len;
            if (nz < 0.0)
            {
                return;     // a box swept along +z enters every half space with n.z < 0
            }
            F64 support = -DBL_MAX;
            for (U32 i = 0; i < count; ++i)
            {
                support = llmax(support, nx * r[i].x + ny * r[i].y + nz * r[i].z);
            }
            RecvPlane& plane = c.planes[c.plane_count++];
            plane.n[0] = nx;
            plane.n[1] = ny;
            plane.n[2] = nz;
            plane.thr = support + c.mx * fabs(nx) + c.my * fabs(ny) + s.gamma * nz + eps;
        };

        // The light's own planes (n.z = 0): edges of R's 2D hull (monotone chain, counter-clockwise).
        {
            glm::dvec2 pts[RECV_MAX_POINTS];
            for (U32 i = 0; i < count; ++i)
            {
                pts[i] = glm::dvec2(r[i].x, r[i].y);
            }
            std::sort(pts, pts + count, [](const glm::dvec2& u, const glm::dvec2& v)
            {
                return u.x < v.x || (u.x == v.x && u.y < v.y);
            });
            glm::dvec2 hull[2 * RECV_MAX_POINTS];
            U32 h = 0;
            auto cross = [](const glm::dvec2& o, const glm::dvec2& u, const glm::dvec2& v)
            {
                return (u.x - o.x) * (v.y - o.y) - (u.y - o.y) * (v.x - o.x);
            };
            for (U32 i = 0; i < count; ++i)
            {
                while (h >= 2 && cross(hull[h - 2], hull[h - 1], pts[i]) <= 0.0)
                {
                    --h;
                }
                hull[h++] = pts[i];
            }
            const U32 lower = h + 1;
            for (S32 i = (S32)count - 2; i >= 0; --i)
            {
                while (h >= lower && cross(hull[h - 2], hull[h - 1], pts[i]) <= 0.0)
                {
                    --h;
                }
                hull[h++] = pts[i];
            }
            // hull[h - 1] repeats hull[0]: h - 1 edges.
            for (U32 i = 0; i + 1 < h; ++i)
            {
                const glm::dvec2 d = hull[i + 1] - hull[i];
                add_plane(d.y, -d.x, 0.0);  // outward for a counter-clockwise hull
            }
        }

        // The main slice's faces (where n.z > 0): near and far depth, the four sides.
        {
            glm::dvec3 v[8];
            glm::dvec3 centre(0.0);
            for (U32 i = 0; i < 8; ++i)
            {
                const glm::dvec4 clip = s.eye_to_clip * glm::dvec4(slice[0][i], 1.0);
                v[i] = glm::dvec3(clip) / clip.w;   // w > 0: the corner is the centre of its offsets
                centre += v[i] * 0.125;
            }
            static const U32 faces[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
                                             { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
            for (const U32* face : faces)
            {
                glm::dvec3 n(0.0);
                glm::dvec3 mid(0.0);
                for (U32 i = 0; i < 4; ++i)
                {
                    const glm::dvec3& p = v[face[i]];
                    const glm::dvec3& q = v[face[(i + 1) & 3]];
                    n.x += (p.y - q.y) * (p.z + q.z);   // Newell: copes with a face squeezed to an edge or a point
                    n.y += (p.z - q.z) * (p.x + q.x);
                    n.z += (p.x - q.x) * (p.y + q.y);
                    mid += p * 0.25;
                }
                if (glm::dot(n, mid - centre) < 0.0)
                {
                    n = -n;
                }
                add_plane(n.x, n.y, n.z);
            }
        }

        // The axes: all receivers' depths, and their x, y bounds.
        add_plane(0.0, 0.0, 1.0);
        add_plane(1.0, 0.0, 0.0);
        add_plane(-1.0, 0.0, 0.0);
        add_plane(0.0, 1.0, 0.0);
        add_plane(0.0, -1.0, 0.0);

        c.to_clip = s.agent_to_clip;
        c.active = true;
        return true;
    }

    // True when nothing the agent space box lo..hi draws into the cascade can
    // change what a receiver reads from it.
    bool recv_skip(const RecvCascade& c, const LLVector4a& lo, const LLVector4a& hi)
    {
        const F32* a = lo.getF32ptr();
        const F32* b = hi.getF32ptr();
        F64 p[8][3];
        F64 min_x = DBL_MAX, max_x = -DBL_MAX, min_y = DBL_MAX, max_y = -DBL_MAX, min_z = DBL_MAX;
        for (U32 i = 0; i < 8; ++i)
        {
            const glm::dvec4 clip = c.to_clip * glm::dvec4((i & 1) ? b[0] : a[0], (i & 2) ? b[1] : a[1], (i & 4) ? b[2] : a[2], 1.0);
            if (!(clip.w > RECV_MIN_W))
            {
                return false;   // at or behind the light's eye plane: no bound
            }
            for (U32 k = 0; k < 3; ++k)
            {
                p[i][k] = clip[k] / clip.w;
                if (!std::isfinite(p[i][k]))
                {
                    return false;
                }
            }
            min_x = llmin(min_x, p[i][0]);
            max_x = llmax(max_x, p[i][0]);
            min_y = llmin(min_y, p[i][1]);
            max_y = llmax(max_y, p[i][1]);
            min_z = llmin(min_z, p[i][2]);
        }
        // Covers no texel: off the map, or beyond its far plane (depth clamp
        // writes 1.0 there, and no texel holds more).
        if (max_x < -1.0 - c.cover_x || min_x > 1.0 + c.cover_x || max_y < -1.0 - c.cover_y || min_y > 1.0 + c.cover_y ||
            min_z > 1.0 + 1e-6)
        {
            return true;
        }
        // Reaches an edge that receivers past it read.
        if ((c.border[0] && max_x >= 1.0 - c.mx && min_y <= c.strip_hi[0] && max_y >= c.strip_lo[0]) ||
            (c.border[1] && min_x <= -1.0 + c.mx && min_y <= c.strip_hi[1] && max_y >= c.strip_lo[1]) ||
            (c.border[2] && max_y >= 1.0 - c.my && min_x <= c.strip_hi[2] && max_x >= c.strip_lo[2]) ||
            (c.border[3] && min_y <= -1.0 + c.my && min_x <= c.strip_hi[3] && max_x >= c.strip_lo[3]))
        {
            return false;
        }
        for (U32 i = 0; i < c.plane_count; ++i)
        {
            const RecvPlane& plane = c.planes[i];
            F64 m = DBL_MAX;
            for (U32 j = 0; j < 8 && m > plane.thr; ++j)
            {
                m = llmin(m, plane.n[0] * p[j][0] + plane.n[1] * p[j][1] + plane.n[2] * p[j][2]);
            }
            if (m > plane.thr)
            {
                return true;
            }
        }
        return false;
    }

    // LLRHI (research/107 section 27): an avatar's animation box
    // (getLastAnimExtents) is recalculated only every few frames
    // (LLVOAvatar::updateSpatialExtents: every 4 frames with up to 5 avatars
    // around, every 20 with 25) and only moved with the pelvis in between, so a
    // turn, a step or an animation change (an AO) takes the feet past it for
    // those frames. The receiver cull trusts it: grown to what the box covers
    // turned any way about its centre, and AVATAR_BOX_MARGIN more each way.
    constexpr F32 AVATAR_BOX_MARGIN = 0.5f;    // m

    // The agent's own rigged casters in this pass (plus/shadowdist's
    // diagnostics, PlusShadowDiagnostics): per cascade position, the groups
    // the receiver cull tested and those it kept. From the sort's threads.
    struct AgentRecvCounts
    {
        std::atomic<U32> tested[4];
        std::atomic<U32> kept[4];
    };
    AgentRecvCounts sAgentRecv;

    // The agent space box the frustum culls trust for a group's casters (a
    // bridge's own space taken to agent space), grown by the animation box of
    // every avatar its rigged batches skin to. Only reads.
    bool recv_group_box(LLSpatialGroup* group, LLVector4a& lo, LLVector4a& hi, bool* agent = nullptr)
    {
        const LLVector4a* ext = group->getObjectExtents();
        const F32* e0 = ext[0].getF32ptr();
        const F32* e1 = ext[1].getF32ptr();
        for (U32 i = 0; i < 3; ++i)
        {
            if (!llfinite(e0[i]) || !llfinite(e1[i]) || !(e0[i] <= e1[i]))
            {
                return false;
            }
        }
        LLSpatialPartition* part = group->getSpatialPartition();
        LLSpatialBridge* bridge = part ? part->asBridge() : nullptr;
        if (bridge)
        {
            if (!bridge->mDrawable)
            {
                return false;
            }
            LLMatrix4a mat;
            mat.loadu(bridge->mDrawable->getXform()->getWorldMatrix());
            LLVector4a out[2];
            matMulBoundBox(mat, ext, out);
            lo = out[0];
            hi = out[1];
        }
        else
        {
            lo = ext[0];
            hi = ext[1];
        }
        const LLVOAvatar* last = nullptr;
        for (LLSpatialGroup::draw_map_t::const_iterator i = group->mDrawMap.begin(); i != group->mDrawMap.end(); ++i)
        {
            for (const LLDrawInfo* info : i->second)
            {
                const LLVOAvatar* avatar = info->mAvatar.get();
                if (!avatar || avatar == last)
                {
                    continue;
                }
                last = avatar;
                const LLVector3* box = avatar->getLastAnimExtents();
                if (box[0].isExactlyZero() && box[1].isExactlyZero())
                {
                    return false;   // not known yet
                }
                if (agent && avatar == gAgentAvatarp.get())
                {
                    *agent = true;
                }
                // LLRHI (research/107 section 27): turned any way, plus the margin.
                const LLVector3 centre = (box[0] + box[1]) * 0.5f;
                const LLVector3 half = (box[1] - box[0]) * 0.5f;
                const F32 reach = sqrtf(half.mV[0] * half.mV[0] + half.mV[1] * half.mV[1]) + AVATAR_BOX_MARGIN;
                LLVector4a b0, b1;
                b0.set(centre.mV[0] - reach, centre.mV[1] - reach, box[0].mV[2] - AVATAR_BOX_MARGIN);
                b1.set(centre.mV[0] + reach, centre.mV[1] + reach, box[1].mV[2] + AVATAR_BOX_MARGIN);
                lo.setMin(lo, b0);
                hi.setMax(hi, b1);
            }
        }
        return true;
    }

    // The cascades (bits of mask, as the cull found them) the group's casters
    // drawn every frame still go to.
    U32 recv_keep_mask(LLSpatialGroup* group, U32 mask, const RecvCascade* recv, size_t count)
    {
        U32 test = 0;
        for (size_t k = 0; k < count; ++k)
        {
            if ((mask & (1u << k)) && recv[k].active)
            {
                test |= 1u << k;
            }
        }
        LLVector4a lo, hi;
        bool agent = false;
        if (!test || !recv_group_box(group, lo, hi, &agent))
        {
            return mask;
        }
        U32 keep = mask;
        for (size_t k = 0; k < count; ++k)
        {
            if ((test & (1u << k)) && recv_skip(recv[k], lo, hi))
            {
                keep &= ~(1u << k);
            }
        }
        if (agent)  // LLRHI: diagnostics (research/107 section 27)
        {
            for (size_t k = 0; k < count && k < 4; ++k)
            {
                if (test & (1u << k))
                {
                    sAgentRecv.tested[k].fetch_add(1, std::memory_order_relaxed);
                    sAgentRecv.kept[k].fetch_add((keep >> k) & 1u, std::memory_order_relaxed);
                }
            }
        }
        return keep;
    }
}

void LLPipeline::renderSunShadowCascades(std::vector<ShadowCascade>& cascades, EShadowCache cache)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("renderSunShadowCascades");

    static LLCullResult culled[4];
    static LLCullResult merged;
    static LLCullResult filtered[4];    // static cache on: the casters its depth holds
    static LLCullResult moving[4];      // static cache on: the casters drawn every frame
    static std::vector<std::pair<U32, LLDrawInfo*> > joining[4];   // settled since the cache was drawn
    static std::vector<LLSpatialGroup*> groups;
    static std::vector<U64> info_hashes;
    static PointerSet seen;
    ShadowCascadeCasters casters[4];
    // LLRHI: the probes' shared maps (shareProbeShadows) come here only with their cache on.
    const bool probe_cache = cache == SHADOW_CACHE_PROBE;
    const bool use_cache = probe_cache || (LLRHI::sCacheSunShadows && !gCubeSnapshot);
    ShadowCacheEntry* const cache_entries = probe_cache ? mProbeShadowCacheEntry : mShadowCacheEntry;
    ShadowCacheStats& stats = shadow_cache_stats(cache);
    const F32 now = gFrameTimeSeconds;

    LLPipeline::sShadowRender = true;
    U32 saved_occlusion = sUseOcclusion;
    sUseOcclusion = 0;
    for (U32 k = 0; k < 4; ++k)     // LLRHI: diagnostics (research/107 section 27)
    {
        sAgentRecv.tested[k].store(0, std::memory_order_relaxed);
        sAgentRecv.kept[k].store(0, std::memory_order_relaxed);
    }
    LLRHI::sShadowSkinned = LLRHI::ShadowSkinnedCounts();  // LLRHI: this call's native passes only
    // <plus> frame cost detail (research/113, LLRHI_FRAME_COST=2): the main view's sun cascades only
    if (!probe_cache && !gCubeSnapshot)
    {
        LLRHI::cpuDetailMark("shadow cull"); // GPU list phase, walks (research/178 C: then "shadow cascades", "shadow state sort")
    }
    // </plus>

    // LLRHI: resident groups of the GPU shadow list stay out of the culls (research/46 G3).
    mRHIListKinds = rhiListPhase(mRHIListSun);
    sRHIListCullKinds = mRHIListKinds;

    // Cull per cascade; remember which cascades saw each group (on the group, cleared below).
    groups.clear();
    const size_t count = llmin(cascades.size(), (size_t)4);
    const bool cull_threads = LLRHI::sCullThreads;

    // LLRHI: each main view cascade's zero coverage view, in its shadow map's texels
    // (research/61). Not the probes' shared maps.
    static std::vector<LLSpatialGroup*> coverage_skipped[4];
    LLRHI::CoverageView* coverage[4] = {};
    for (size_t k = 0; k < count; ++k)
    {
        coverage_skipped[k].clear();
        const S32 index = cascades[k].index;
        if (!probe_cache && !gCubeSnapshot && !gUseWireframe && index >= 0 && index < 4)
        {
            LLRHI::CoverageView& view = sRHICoverageViews[1 + index];
            rhi_coverage_set(view, cascades[k].proj * cascades[k].view, mRT->shadow[index].getWidth(),
                             mRT->shadow[index].getHeight(), true);
            coverage[k] = view.active ? &view : nullptr;
        }
    }
    if (cull_threads && count > 1)
    {
        // LLRHI: walk the region octrees for all cascades at once on the cull threads
        // (research/48). The walks only read the octrees: their bounds are made current
        // here first, and what markNotCulled does besides pushing is done below.
        static std::vector<LLSpatialPartition*> parts;
        parts.clear();
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            for (U32 i = 0; i < LLViewerRegion::NUM_PARTITIONS; i++)
            {
                LLSpatialPartition* part = region->getSpatialPartition(i);
                if (part && hasRenderType(part->mDrawableType))
                {
                    ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();
                    parts.push_back(part);
                }
            }
        }
        // <plus> plus/shadowreuse: the main view's walks are handed over while nothing they read changed
        plus::shadowreuse::beginWalks(!probe_cache && !gCubeSnapshot, parts, (U32)count, mRHIListResidency, sRHIListCullKinds);
        // </plus>
        for (size_t k = 0; k < count; ++k)
        {
            setCullClipPlane(cascades[k].camera);
            culled[k].clear();
            // <plus> plus/shadowreuse
            plus::shadowreuse::beginCascade((U32)k, cascades[k].index, cascades[k].camera, coverage[k]);
            // </plus>
        }
        CullWorkers::instance().run((U32)count, [&cascades, &coverage](U32 k)
        {
            for (size_t i = 0; i < parts.size(); ++i)
            {
                // <plus> plus/shadowreuse: parts[i]->cullShadowInto, or its last walk (was: the call alone)
                plus::shadowreuse::walk(k, i, parts[i], cascades[k].camera, culled[k], coverage[k], &coverage_skipped[k]);
                // </plus>
            }
        });
        // <plus> plus/shadowreuse
        plus::shadowreuse::endWalks();
        // </plus>
    }
    // <plus> frame cost detail (research/178 C): "shadow cull" was all three (walks / per cascade / state sort)
    if (!probe_cache && !gCubeSnapshot)
    {
        LLRHI::cpuDetailMark("shadow cascades"); // per cascade: marking the groups, the object cache, the bridges' culls
    }
    // </plus>
    // <plus> plus/shadowbridge: the bridges' octree walks kept for the cascades of this call (research/178 C1b)
    {
    plus::shadowbridge::CascadesScope bridge_scope(!probe_cache && !gCubeSnapshot);
    // </plus>
    for (size_t k = 0; k < count; ++k)
    {
        ShadowCascade& cascade = cascades[k];
        LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SUN_SHADOW0 + cascade.index);
        set_current_modelview(cascade.view);
        set_current_projection(cascade.proj);
        if (cull_threads && count > 1)
        {
            // The rest of updateCull, in its order: mark the groups the walk found
            // visible for this camera, the object cache (only rebounds for a shadow
            // camera), then the sky.
            grabReferences(culled[k]);
            for (LLCullResult::sg_iterator i = sCull->beginDrawableGroups(); i != sCull->endDrawableGroups(); ++i)
            {
                (*i)->setVisible();
            }
            for (LLCullResult::sg_iterator i = sCull->beginVisibleGroups(); i != sCull->endVisibleGroups(); ++i)
            {
                (*i)->setVisible();
            }
            for (LLSpatialGroup* group : coverage_skipped[k])
            {
                group->setVisible();    // LLRHI: left out by the zero coverage cull, still seen (research/61)
            }
            mNumVisibleNodes += sCull->getDrawableGroupsSize() + sCull->getVisibleGroupsSize();
            for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
            {
                if (LLVOCachePartition* vo_part = region->getVOCachePartition())
                {
                    vo_part->cull(cascade.camera, sUseOcclusion > 0);
                }
            }
            cullSky(cascade.camera);
        }
        else
        {
            sRHICoverage = coverage[k];     // LLRHI (research/61)
            updateCull(cascade.camera, culled[k]);
            sRHICoverage = nullptr;
        }
        // <plus> plus/shadowbridge: timed for its log, on and off
        {
            plus::shadowbridge::SortTimer bridge_timer(!probe_cache && !gCubeSnapshot);
        // </plus>
        stateSortDrawableGroups(cascade.camera);    // spatial bridges cull their contents with this camera
        // <plus> plus/shadowbridge
        }
        // </plus>
        for (LLCullResult::sg_iterator i = culled[k].beginVisibleGroups(); i != culled[k].endVisibleGroups(); ++i)
        {
            LLSpatialGroup* group = *i;
            if (!group->mRHIShadowMask)
            {
                groups.push_back(group);
            }
            group->mRHIShadowMask |= 1u << k;
        }
    }
    // <plus> plus/shadowbridge
    }
    // </plus>
    sRHIListCullKinds = 0;
    // <plus> frame cost detail (research/178 C)
    if (!probe_cache && !gCubeSnapshot)
    {
        LLRHI::cpuDetailMark("shadow state sort"); // the union of the cascades' groups, one state sort
    }
    // </plus>

    // Union of the cull results, then one state sort.
    merged.clear();
    {
        size_t expected = 0;
        for (size_t k = 0; k < count; ++k)
        {
            expected += culled[k].getDrawableGroupsSize() + culled[k].getVisibleListSize() + culled[k].getVisibleBridgeSize();
        }
        seen.reset(expected);
        for (LLSpatialGroup* group : groups)
        {
            merged.pushVisibleGroup(group);
        }
        for (size_t k = 0; k < count; ++k)
        {
            for (LLCullResult::sg_iterator i = culled[k].beginDrawableGroups(); i != culled[k].endDrawableGroups(); ++i)
            {
                if (seen.insert(*i)) merged.pushDrawableGroup(*i);
            }
            for (LLCullResult::drawable_iterator i = culled[k].beginVisibleList(); i != culled[k].endVisibleList(); ++i)
            {
                if (seen.insert(*i)) merged.pushDrawable(*i);
            }
            for (LLCullResult::bridge_iterator i = culled[k].beginVisibleBridge(); i != culled[k].endVisibleBridge(); ++i)
            {
                if (seen.insert(*i)) merged.pushBridge(*i);
            }
        }
    }
    LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SUN_SHADOW0 + cascades[0].index);
    grabReferences(merged);
    // LLRHI: the render maps are built per cascade below, straight from the groups.
    mPostSortGroupsOnly = cull_threads;
    stateSortVisible(cascades[0].camera);
    mPostSortGroupsOnly = false;

    // <plus> frame cost detail (research/113, LLRHI_FRAME_COST=2): the main view's sun cascades only
    if (!probe_cache && !gCubeSnapshot)
    {
        LLRHI::cpuDetailMark("shadow sort"); // receivers, fingerprints, static / joining / moving lists
    }
    // </plus>
    // LLRHI: the shadow receiver cull (research/64): where the main view's pixels
    // (and this frame's impostor renders) read each cascade. Only for the casters
    // drawn every frame: the cached depth stays what its fingerprint says.
    static RecvCascade recv[4];
    bool recv_any = false;
    for (RecvCascade& c : recv)
    {
        c.active = false;
    }
    if (use_cache && !probe_cache && LLRHI::sShadowReceiverCull)
    {
        // Anisotropic shadow maps: the read footprint follows screen derivatives (unbounded).
        const bool aniso = mRHISunShadowAniso > 1.f && !LLRHI::sShadowReceiverCullAniso;
        // Other cameras reading these maps: probes (their own maps, but this
        // path), the shadow camera debug view, appearance hints (next frame).
        bool held = gCubeSnapshot || !mRHIShadowReceivers.valid || mRT != &mMainRT || CameraOffset ||
                    LLVisualParamHint::anyInstances();
        // This frame's impostor renders (LLVOAvatar::updateImpostors, right after
        // the shadows) read the maps from their own eye space: a frustum looking
        // down -z like the main view's, with half tangents at most
        // |half extents| / distance (generateImpostor). Every impostor counts.
        F64 tan_impostor = 0.0;
        if (!aniso && !held)
        {
            for (LLCharacter* character : LLCharacter::sInstances)
            {
                LLVOAvatar* avatar = (LLVOAvatar*)character;
                if (avatar->isDead() || avatar->mDrawable.isNull() || !avatar->isImpostor())
                {
                    continue;
                }
                const LLVector4a* ext = avatar->mDrawable->getSpatialExtents();
                LLVector4a half;
                half.setSub(ext[1], ext[0]);
                half.mul(0.5f);
                const F64 radius = half.getLength3().getF32();
                const LLVector3 centre = avatar->getRenderPosition() + avatar->getImpostorOffset();
                const F64 distance = (centre - mRHIShadowReceivers.origin).length();
                if (!(distance > 0.5) || !llfinite(radius))
                {
                    held = true;
                    break;
                }
                tan_impostor = llmax(tan_impostor, radius / distance);
            }
            if (tan_impostor > 0.0)
            {
                tan_impostor = tan_impostor * 1.25 + 1e-3;  // room for extents that move before the render
                held = held || tan_impostor > 100.0;
            }
        }
        if (aniso)
        {
            ++stats.receiver_frames_aniso;
        }
        else if (held)
        {
            ++stats.receiver_frames_held;
        }
        else
        {
            ++stats.receiver_frames_on;
            RecvSetup setup;
            const glm::dmat4 main_view(mRHIShadowReceivers.view);
            const glm::dmat4 inv_view = glm::inverse(main_view);
            setup.inv_proj = glm::inverse(glm::dmat4(mRHIShadowReceivers.proj));
            // The shader's offset: light_dir * (1 - n.l) * shadow_offset * 2.
            setup.offset = 2.0 * fabs((F64)RenderShadowOffset) * (1.0 + 1e-4) + 1e-4;
            // <plus> plus/shadowdist: and shadowUtil.glsl's normal offset, any way (research/107 section 31)
            setup.offset += (F64)plus::shadowdist::receiverNormalOffsetMax() * (1.0 + 1e-4);
            // </plus>
            // The shader's reference: depth + 2 * shadow_bias in [0, 1], 4 * shadow_bias in NDC
            // (bindDeferredShader's shadow_bias, from the same camera height).
            // <plus> plus/shadowdist: the shader's bias as bindDeferredShader scales it (0 while the
            // shadow detail moves the receivers towards the light in the matrices instead: a
            // reference below Q's own, which (-gamma, inf) holds with any gamma >= 0)
            const F64 bias = ((F64)RenderShadowBias +
                              (F64)RenderShadowBiasError * fabs((F64)mRHIShadowReceivers.origin.mV[2]) / 3000.0) *
                             (F64)plus::shadowdist::biasScale();
            // </plus>
            setup.gamma = 4.0 * fabs(bias) + 1e-6;
            setup.tan_impostor = tan_impostor;
            const F64 split[4] = { mSunClipPlanes.mV[0], mSunClipPlanes.mV[1], mSunClipPlanes.mV[2], mSunClipPlanes.mV[3] };
            for (size_t k = 0; k < count; ++k)
            {
                const S32 index = cascades[k].index;
                // The shader's bands (shadow_clip * 0.75 / 1.25), all below shadow_clip.w.
                const U32 width = index >= 0 && index < 4 ? llmin(mRT->shadow[0].getWidth(), mRT->shadow[index].getWidth()) : 0;
                const U32 height = index >= 0 && index < 4 ? llmin(mRT->shadow[0].getHeight(), mRT->shadow[index].getHeight()) : 0;
                if (!width || !height)
                {
                    ++stats.receiver_cascades_inactive;
                    continue;
                }
                setup.lo = index == 0 ? -1.0 : 0.75 * split[index - 1];
                setup.hi = index == 3 ? split[3] : llmin(1.25 * split[index], split[3]);
                setup.texel_x = 2.0 / width;
                setup.texel_y = 2.0 / height;
                const glm::dmat4 view(cascades[k].view);
                setup.agent_to_clip = glm::dmat4(cascades[k].proj) * view;
                setup.eye_to_clip = setup.agent_to_clip * inv_view;
                // The way light travels: the cascade view's -z (look()), in the main view's eye space.
                const glm::dvec3 down(-view[0][2], -view[1][2], -view[2][2]);
                setup.down_eye = glm::normalize(glm::dmat3(main_view) * down);
                if (recv_build(recv[k], setup))
                {
                    recv_any = true;
                }
                else
                {
                    ++stats.receiver_cascades_inactive;
                }
            }
        }
    }

    // Per cascade render maps: the draw infos of the groups that cascade saw (same filters as postSort).
    // With the cache, a static group goes to the cascade's cached casters when it had settled by the
    // time the cache was drawn; one that settled later waits with the moving casters until the cache
    // is redrawn for another reason, instead of forcing a redraw itself.
    F32 cutoff[4];
    for (size_t k = 0; k < count; ++k)
    {
        filtered[k].clear();
        moving[k].clear();
        joining[k].clear();
        casters[k].cached = &filtered[k];
        casters[k].moving = &moving[k];
        casters[k].joining = &joining[k];
        const ShadowCacheEntry& entry = cache_entries[cascades[k].index];
        cutoff[k] = entry.valid && entry.reused ? entry.drawn_at : F32_MAX;
    }
    U32 changed_mask = 0;
    // <plus> plus/shadowshift: the A/B cue (shadowshift.toggle), the main view's sun cascades only; and
    // whether a group the sort sees for the first time counts as settled since its last build (S4(b),
    // research/158 section 3: the layer it is drawn in changes, the shadow map does not)
    const bool plus_first_seen = !probe_cache && !gCubeSnapshot && plus::shadowshift::beginSort();
    U32 plus_first_seen_groups = 0;
    // </plus>
    // <plus> plus/shadowfar: small moving casters far from the camera left out (research/178 C2)
    plus::shadowfar::beginSort(!probe_cache && !gCubeSnapshot);
    // </plus>
    // LLRHI: with the cache, the sort below in chunks of groups on the cull
    // threads (research/57). Each chunk does what the loop below does for its
    // groups, keeping the order; the main thread then fills the lists in order.
    const bool sort_threads = use_cache && LLRHI::sShadowSortThreads && LLRHI::sCullThreads;
    // <plus> research/206 (stage 5): the main view's sun cascades, sorted in chunks, drawn by the native
    // shadow passes: the groups the retained scene can draw are marked per cascade, in its moving
    // casters (bridges, 5a; static groups not settled, 5b) or its cached depth (settled static groups, 5b)
    S32 scene_indices[4] = { -1, -1, -1, -1 };
    for (size_t k = 0; k < count; ++k)
    {
        scene_indices[k] = cascades[k].index;
    }
    // research/208 13.6 (vk-s4a4): the probes' shared maps too with LLRHI_SCENE_SHADOW_PROBE=on (their call runs in a
    // probe update, gCubeSnapshot set; any other cube snapshot, as a water mirror's, still not)
    gSceneShadow.beginCascades(sort_threads && (probe_cache ? LLRHI::sceneShadowProbe() : !gCubeSnapshot) && LLRHI::sNativeShadows &&
                                   LLRHI::sDrawShadowPackets && LLRHI::sGLDrawsDisabled,
                               (U32)count, scene_indices, probe_cache);
    struct SceneShadowEnd
    {
        ~SceneShadowEnd() { gSceneShadow.endCascades(); }
    } scene_shadow_end;
    gSceneShadow.noteGroups(groups);
    const bool scene_cut = gSceneShadow.cutting();
    // </plus>
    plus::activecensus::shadowGroups(groups, !probe_cache && !gCubeSnapshot); // <plus> research/186 step 1 (off: one flag test) </plus>
    if (sort_threads)
    {
        const size_t chunk_count = (groups.size() + SHADOW_SORT_CHUNK - 1) / SHADOW_SORT_CHUNK;
        if (sShadowSort.chunks.size() < chunk_count)
        {
            sShadowSort.chunks.resize(chunk_count);
        }
        sShadowSort.used = chunk_count;
        auto sort_chunk = [&](U32 c)
        {
            ShadowSortChunk& chunk = sShadowSort.chunks[c];
            std::vector<ShadowSortEntry>& entries = chunk.entries;
            entries.clear();
            U64 fingerprint[4] = {}, joining_fingerprint[4] = {};
            U32 chunk_changed = 0, groups_seen = 0, groups_changed = 0;
            U32 first_seen = 0; // <plus> plus/shadowshift S4(b) </plus>
            U64 static_infos = 0, joining_infos = 0;
            U64 moving_bridge = 0, moving_unsettled = 0, moving_rigged = 0, moving_animated = 0;
            U64 receiver_tested[4] = {}, receiver_skipped[4] = {};
            plus::shadowfar::Count far_count; // <plus> plus/shadowfar (research/178 C2) </plus>
            chunk.scene_groups.clear(); // <plus> research/206 </plus>
            const size_t first = (size_t)c * SHADOW_SORT_CHUNK;
            const size_t last = llmin(first + SHADOW_SORT_CHUNK, groups.size());
            for (size_t g = first; g < last; ++g)
            {
                LLSpatialGroup* group = groups[g];
                const U32 mask = group->mRHIShadowMask;
                group->mRHIShadowMask = 0;
                if (group->isDead() ||
                    (RenderAutoHideSurfaceAreaLimit > 0.f &&
                     group->mSurfaceArea > RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
                {
                    continue;
                }
                const bool bridge = group->getSpatialPartition()->asBridge() != nullptr;
                // <plus> plus/shadowfar: a small moving caster far from the camera, not an avatar's (research/178 C2)
                if (bridge && plus::shadowfar::leaveOut(group, mask, far_count))
                {
                    continue;
                }
                // </plus>
                const size_t begin = entries.size();
                U64 group_hash = 0;
                for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
                {
                    if (!hasRenderType(j->first))
                    {
                        continue;
                    }
                    for (LLDrawInfo* info : j->second)
                    {
                        ShadowSortEntry entry;
                        entry.info = info;
                        entry.hash = 0;
                        entry.type = j->first;
                        entry.mask = (U8)mask;
                        entry.cached = SHADOW_SORT_MOVING;
                        entry.scene = 0; // <plus> research/206 </plus>
                        entry.why = bridge ? SHADOW_SORT_BRIDGE
                                  : info->mAvatar.notNull() ? SHADOW_SORT_RIGGED
                                  : info->mTextureMatrix ? SHADOW_SORT_ANIMATED
                                  : SHADOW_SORT_STATIC;
                        if (entry.why == SHADOW_SORT_STATIC)
                        {
                            entry.hash = shadow_content_hash(info, j->first, group->rhiBuilds());
                            group_hash += entry.hash;
                        }
                        entries.push_back(entry);
                    }
                }
                bool settled = true;
                U32 cached_mask = mask;
                if (!bridge)
                {
                    ++groups_seen;
                    if (group->mRHIShadowHash != group_hash)
                    {
                        // <plus> plus/shadowshift S4(b): seen for the first time, built long enough ago:
                        // its casters last changed when it was built (was: always now)
                        const bool settled_first = plus_first_seen && group->mRHIShadowHash == 0 && group->mRHIBuiltAt >= 0.f &&
                                                   settled_by(group->mRHIBuiltAt, now);
                        group->mRHIShadowHash = group_hash;
                        group->mRHIShadowChangedAt = settled_first ? group->mRHIBuiltAt : now;
                        if (settled_first)
                        {
                            ++first_seen;
                        }
                        else
                        {
                            chunk_changed |= mask;
                            ++groups_changed;
                        }
                        // </plus>
                    }
                    settled = settled_by(group->mRHIShadowChangedAt, now);
                    cached_mask = 0;
                    for (size_t k = 0; k < count; ++k)
                    {
                        if ((mask & (1u << k)) && settled_by(group->mRHIShadowChangedAt, cutoff[k]))
                        {
                            cached_mask |= 1u << k;
                        }
                    }
                }
                const U32 cascades_seen = bit_count(mask);
                U32 recv_keep = mask;           // LLRHI: cascades the moving casters go to (research/64)
                bool recv_done = !recv_any;
                // <plus> research/206: a group the retained scene can draw, by its visible bit in a pass's view,
                // so only where all its batches of the cut types are in that pass: in the moving passes a bridge's
                // (5a) or a static group's that has not settled (5b H6: all its batches are moving); in the
                // cached depths that take it a settled static group's whose batches of those types are all
                // static (5b H1: none rigged or animated, which are moving)
                const U32 scene_id = gSceneShadow.groupId(g);
                const bool scene_moving = scene_id && (bridge || !settled);
                bool scene_cached = scene_id && !bridge && settled;
                for (size_t e = begin; scene_cached && e < entries.size(); ++e)
                {
                    scene_cached = entries[e].why == SHADOW_SORT_STATIC || !gSceneShadow.cutType(entries[e].type);
                }
                U8 scene_mask = 0, scene_cached_mask = 0;
                // </plus>
                for (size_t e = begin; e < entries.size(); ++e)
                {
                    ShadowSortEntry& entry = entries[e];
                    if (entry.why != SHADOW_SORT_STATIC || !settled)
                    {
                        (entry.why == SHADOW_SORT_BRIDGE ? moving_bridge : entry.why == SHADOW_SORT_RIGGED ? moving_rigged
                         : entry.why == SHADOW_SORT_ANIMATED ? moving_animated : moving_unsettled) += cascades_seen;
                        if (!recv_done)
                        {
                            recv_keep = recv_keep_mask(group, mask, recv, count);
                            recv_done = true;
                        }
                        for (size_t k = 0; recv_any && k < count; ++k)
                        {
                            const U32 bit = 1u << k;
                            if ((mask & bit) && recv[k].active)
                            {
                                ++receiver_tested[k];
                                receiver_skipped[k] += (recv_keep & bit) ? 0 : 1;
                            }
                        }
                        entry.mask = (U8)recv_keep;
                        // <plus> research/206: its opaque types' batches go to the scene in the cascades it is drawn in
                        if (scene_moving && gSceneShadow.cutType(entry.type))
                        {
                            entry.scene = (U8)recv_keep;
                            scene_mask |= entry.scene;
                        }
                        // </plus>
                        continue;   // stays SHADOW_SORT_MOVING
                    }
                    entry.cached = (U8)cached_mask;
                    // <plus> research/206 5b: and into the cached depths that take it (when they are drawn again)
                    if (scene_cached && gSceneShadow.cutType(entry.type))
                    {
                        entry.scene = (U8)cached_mask;
                        scene_cached_mask |= entry.scene;
                    }
                    // </plus>
                    for (size_t k = 0; k < count; ++k)
                    {
                        const U32 bit = 1u << k;
                        if (!(mask & bit))
                        {
                            continue;
                        }
                        if (cached_mask & bit)
                        {
                            fingerprint[k] += entry.hash;
                            ++static_infos;
                        }
                        else
                        {
                            joining_fingerprint[k] += entry.hash;
                            ++joining_infos;
                        }
                    }
                }
                // <plus> research/206: the group's visible bit in those cascades' views
                if (scene_mask || scene_cached_mask)
                {
                    chunk.scene_groups.push_back({ scene_id, scene_mask, scene_cached_mask, (U32)g });
                }
                // </plus>
            }
            for (size_t k = 0; k < 4; ++k)
            {
                chunk.fingerprint[k] = fingerprint[k];
                chunk.joining_fingerprint[k] = joining_fingerprint[k];
            }
            plus::shadowfar::addCount(far_count); // <plus> plus/shadowfar </plus>
            chunk.changed_mask = chunk_changed;
            chunk.groups_seen = groups_seen;
            chunk.groups_changed = groups_changed;
            chunk.plus_first_seen = first_seen; // <plus> plus/shadowshift S4(b) </plus>
            chunk.static_infos = static_infos;
            chunk.joining_infos = joining_infos;
            chunk.moving_bridge = moving_bridge;
            chunk.moving_unsettled = moving_unsettled;
            chunk.moving_rigged = moving_rigged;
            chunk.moving_animated = moving_animated;
            for (size_t k = 0; k < 4; ++k)
            {
                chunk.receiver_tested[k] = receiver_tested[k];
                chunk.receiver_skipped[k] = receiver_skipped[k];
            }
        };
        if (chunk_count > 2)
        {
            CullWorkers::instance().run((U32)chunk_count, sort_chunk);
        }
        else
        {
            for (U32 c = 0; c < (U32)chunk_count; ++c)
            {
                sort_chunk(c);
            }
        }
        // In order: the moving and joining casters now, the cached ones when a
        // cascade's cached depth is redrawn (shadow_sort_fill).
        U32 scene_batches = 0; // <plus> research/206 </plus>
        for (size_t c = 0; c < chunk_count; ++c)
        {
            const ShadowSortChunk& chunk = sShadowSort.chunks[c];
            // <plus> research/206
            for (const ShadowSceneGroup& scene_group : chunk.scene_groups)
            {
                gSceneShadow.markGroup(scene_group.id, scene_group.moving, scene_group.cached, scene_group.g);
            }
            // </plus>
            changed_mask |= chunk.changed_mask;
            stats.groups_seen += chunk.groups_seen;
            stats.groups_changed += chunk.groups_changed;
            plus_first_seen_groups += chunk.plus_first_seen; // <plus> plus/shadowshift S4(b) </plus>
            stats.static_infos += chunk.static_infos;
            stats.joining_infos += chunk.joining_infos;
            stats.moving_bridge += chunk.moving_bridge;
            stats.moving_unsettled += chunk.moving_unsettled;
            stats.moving_rigged += chunk.moving_rigged;
            stats.moving_animated += chunk.moving_animated;
            for (size_t k = 0; k < count; ++k)
            {
                casters[k].fingerprint += chunk.fingerprint[k];
                casters[k].joining_fingerprint += chunk.joining_fingerprint[k];
                const S32 index = cascades[k].index;
                if (index >= 0 && index < 4)
                {
                    stats.receiver_tested[index] += chunk.receiver_tested[k];
                    stats.receiver_skipped[index] += chunk.receiver_skipped[k];
                }
            }
            for (const ShadowSortEntry& entry : chunk.entries)
            {
                for (size_t k = 0; k < count; ++k)
                {
                    const U32 bit = 1u << k;
                    if (!(entry.mask & bit))
                    {
                        continue;
                    }
                    if (entry.cached == SHADOW_SORT_MOVING)
                    {
                        // <plus> research/206: the retained scene draws it in this cascade's moving pass
                        if (entry.scene & bit)
                        {
                            ++scene_batches;
                            if (scene_cut)
                            {
                                continue;
                            }
                        }
                        // </plus>
                        moving[k].pushDrawInfo(entry.type, entry.info);
                    }
                    else if (!(entry.cached & bit))
                    {
                        joining[k].emplace_back(entry.type, entry.info);
                    }
                }
            }
        }
        for (size_t k = 0; k < 4; ++k)
        {
            sShadowSort.lazy[k] = k < count ? &filtered[k] : nullptr;
        }
        gSceneShadow.noteCut(scene_batches); // <plus> research/206 </plus>
    }
    else
    {
        plus::shadowfar::Count far_count; // <plus> plus/shadowfar (research/178 C2) </plus>
        for (LLSpatialGroup* group : groups)
        {
            const U32 mask = group->mRHIShadowMask;
            group->mRHIShadowMask = 0;
            if (group->isDead() ||
                (RenderAutoHideSurfaceAreaLimit > 0.f &&
                 group->mSurfaceArea > RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
            {
                continue;
            }
            // Moving: objects on spatial bridges (active objects, attachments).
            const bool bridge = group->getSpatialPartition()->asBridge() != nullptr;
            // <plus> plus/shadowfar: as in the chunks above (research/178 C2)
            if (bridge && plus::shadowfar::leaveOut(group, mask, far_count))
            {
                continue;
            }
            // </plus>
            // Groups whose contents changed in the last few seconds (loading, scripted color or
            // alpha changes rebuild them) are drawn with the moving casters, so they do not keep
            // invalidating the cache; once they settle they join it.
            bool settled = true;
            U32 cached_mask = mask;
            if (use_cache && !bridge)
            {
                info_hashes.clear();
                U64 group_hash = 0;
                for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
                {
                    if (!hasRenderType(j->first))
                    {
                        continue;
                    }
                    for (LLDrawInfo* info : j->second)
                    {
                        if (info->mAvatar.isNull() && !info->mTextureMatrix)
                        {
                            const U64 h = shadow_content_hash(info, j->first, group->rhiBuilds());
                            info_hashes.push_back(h);
                            group_hash += h;
                        }
                    }
                }
                ++stats.groups_seen;
                if (group->mRHIShadowHash != group_hash)
                {
                    // <plus> plus/shadowshift S4(b): as in the chunks above (was: always now)
                    const bool settled_first = plus_first_seen && group->mRHIShadowHash == 0 && group->mRHIBuiltAt >= 0.f &&
                                               settled_by(group->mRHIBuiltAt, now);
                    group->mRHIShadowHash = group_hash;
                    group->mRHIShadowChangedAt = settled_first ? group->mRHIBuiltAt : now;
                    if (settled_first)
                    {
                        ++plus_first_seen_groups;
                    }
                    else
                    {
                        changed_mask |= mask;
                        ++stats.groups_changed;
                    }
                    // </plus>
                }
                settled = settled_by(group->mRHIShadowChangedAt, now);
                cached_mask = 0;
                for (size_t k = 0; k < count; ++k)
                {
                    if ((mask & (1u << k)) && settled_by(group->mRHIShadowChangedAt, cutoff[k]))
                    {
                        cached_mask |= 1u << k;
                    }
                }
            }
            const U32 cascades_seen = bit_count(mask);
            size_t next_hash = 0;
            U32 recv_keep = mask;           // LLRHI: cascades the moving casters go to (research/64)
            bool recv_done = !recv_any;
            for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
            {
                if (!hasRenderType(j->first))
                {
                    continue;
                }
                for (LLDrawInfo* info : j->second)
                {
                    if (!use_cache)
                    {
                        for (size_t k = 0; k < count; ++k)
                        {
                            if (mask & (1u << k))
                            {
                                filtered[k].pushDrawInfo(j->first, info);
                            }
                        }
                        continue;
                    }
                    // Also moving: rigged meshes, and animated textures (their alpha mask changes shape).
                    const bool rigged = info->mAvatar.notNull();
                    const bool animated = info->mTextureMatrix != nullptr;
                    if (bridge || !settled || rigged || animated)
                    {
                        (bridge ? stats.moving_bridge : rigged ? stats.moving_rigged : animated ? stats.moving_animated : stats.moving_unsettled) += cascades_seen;
                        if (!bridge && !rigged && !animated)
                        {
                            ++next_hash;    // hashed above
                        }
                        if (!recv_done)
                        {
                            recv_keep = recv_keep_mask(group, mask, recv, count);
                            recv_done = true;
                        }
                        for (size_t k = 0; k < count; ++k)
                        {
                            const U32 bit = 1u << k;
                            const S32 index = cascades[k].index;
                            if (recv_any && (mask & bit) && recv[k].active && index >= 0 && index < 4)
                            {
                                ++stats.receiver_tested[index];
                                stats.receiver_skipped[index] += (recv_keep & bit) ? 0 : 1;
                            }
                            if (mask & recv_keep & bit)
                            {
                                moving[k].pushDrawInfo(j->first, info);
                            }
                        }
                        continue;
                    }
                    const U64 h = info_hashes[next_hash++];
                    for (size_t k = 0; k < count; ++k)
                    {
                        const U32 bit = 1u << k;
                        if (!(mask & bit))
                        {
                            continue;
                        }
                        if (cached_mask & bit)
                        {
                            filtered[k].pushDrawInfo(j->first, info);
                            casters[k].fingerprint += h;
                            ++stats.static_infos;
                        }
                        else
                        {
                            joining[k].emplace_back(j->first, info);
                            casters[k].joining_fingerprint += h;
                            ++stats.joining_infos;
                        }
                    }
                }
            }
        }
        plus::shadowfar::addCount(far_count); // <plus> plus/shadowfar </plus>
    }
    for (size_t k = 0; k < count; ++k)
    {
        casters[k].changed = (changed_mask & (1u << k)) != 0;
    }
    // <plus> plus/shadowshift S4(b)
    if (plus_first_seen_groups)
    {
        plus::shadowshift::noteFirstSeen(plus_first_seen_groups);
    }
    // </plus>

    // <plus> frame cost detail (research/113, LLRHI_FRAME_COST=2): the main view's sun cascades only
    if (!probe_cache && !gCubeSnapshot)
    {
        LLRHI::cpuDetailMark("shadow draw"); // cached depth copies, moving casters, GPU list passes, the join
    }
    // </plus>
    // Draw each cascade.
    // LLRHI (research/66 step 2): from here to the join below, the cascades'
    // native shadow passes may be recorded on the cull threads (LLRHI::
    // sShadowRecordBin). Those threads read the passes' packets (each pass its
    // own array, drawNativeShadowCasters) and the matrices they point at: a
    // drawable's world matrix (LLXformMatrix, written by LLDrawable::updateXform
    // / shiftPos in updateMove and the object update messages), a region's
    // render matrix (LLViewerRegion::updateRenderMatrix, on an origin change)
    // and a face's texture matrix (LLVOVolume::animateTextures and the update
    // message, in idle). Nothing this loop runs writes or frees any of them:
    // it binds targets, copies depth, sorts nothing, rebuilds nothing, and
    // the pools it draws (renderGeomShadow: trees, avatars, terrain) only read
    // them (an avatar's renderSkinned writes its own body meshes, which no
    // packet names). The threads touch no viewer object and no reference count.
    const bool record_threads = LLRHI::sShadowRecordThreads && cull_threads && LLRHI::sRecordSplice;
    if (record_threads && !LLRHI::sPostWork)
    {
        LLRHI::sPostWork = &rhi_post_work;
    }
    bool refreshed_one = false;     // at most one cache refresh by age per frame
    for (size_t k = 0; k < count; ++k)
    {
        ShadowCascade& cascade = cascades[k];
        LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SUN_SHADOW0 + cascade.index);
        set_current_modelview(cascade.view);
        set_current_projection(cascade.proj);
        LLRHI::sShadowRecordBin = record_threads ? llclamp(cascade.index, 0, 3) + (probe_cache ? 4 : 0) : -1;
        LLSceneShadow::CascadeScope scene_cascade((U32)k); // <plus> research/206 </plus>

        LLRenderTarget& shadow = mRT->shadow[cascade.index];
        if (use_cache)
        {
            renderCachedShadowCascade(cascade, casters[k], now, refreshed_one, cache);
        }
        else
        {
            shadow.bindTarget();
            shadow.getViewport(gGLViewport);
            shadow.clear();

            grabReferences(filtered[k]);
            mRHIListPass = RHIListPass();
            mRHIListPass.volume = (mRHIListKinds & 1) != 0;
            mRHIListPass.terrain = (mRHIListKinds & 2) != 0;
            mRHIListPass.since_max = U32_MAX;
            renderShadowCasters(cascade.view, cascade.proj, cascade.camera, true);
            mRHIListPass = RHIListPass();

            shadow.flush();
        }

        if (!hasRenderDebugMask(LLPipeline::RENDER_DEBUG_SHADOW_FRUSTA) && !gCubeSnapshot)
        {
            mShadowCamera[cascade.index + 4] = cascade.camera;
        }
    }
    // LLRHI (research/66 step 2): the passes recorded on the cull threads are
    // complete before anything can change what they read.
    LLRHI::sShadowRecordBin = -1;
    rhi_join_shadow_recording();

    sUseOcclusion = saved_occlusion;
    LLPipeline::sShadowRender = false;
    mRHIListKinds = 0;
    for (LLCullResult*& lazy : sShadowSort.lazy)
    {
        lazy = nullptr;     // LLRHI: the chunks' draw infos are this call's only
    }

    if (use_cache)
    {
        logShadowCacheStats(count, cache);
    }

    // <plus> plus/shadowdist: diagnostics of your avatar's shadow casters (PlusShadowDiagnostics)
    if (!probe_cache && !gCubeSnapshot)
    {
        U32 tested[4] = {}, kept[4] = {};
        S32 index[4] = { -1, -1, -1, -1 };
        for (size_t k = 0; k < count; ++k)
        {
            tested[k] = sAgentRecv.tested[k].load(std::memory_order_relaxed);
            kept[k] = sAgentRecv.kept[k].load(std::memory_order_relaxed);
            index[k] = cascades[k].index;
        }
        plus::shadowdist::noteAgentCasters(index, tested, kept);
    }
    // </plus>
}

// ----------------------------------------------------------------------------
// LLRHI: static sun shadow cache.

bool LLPipeline::reuseShadowCascade(S32 j, glm::mat4& view, glm::mat4& proj, LLCamera& shadow_cam,
                                    const std::vector<LLVector3>& fp, const LLVector3& light_dir)
{
    ShadowCacheEntry& entry = mShadowCacheEntry[j];
    entry.reused = false;
    // <plus> plus/shadowshift: a new frame's cascade (and the last frame's check, PlusShadowShiftVerify)
    plus::shadowshift::beginReuse(j);
    // </plus>
    if (!entry.valid || fp.empty())
    {
        return false;
    }
    // The cull will give the cached camera the water clip plane that applies now
    // (setCullClipPlane). The cached depth was drawn with the one then, and the
    // GPU shadow list's resident groups in it (G3) were culled with it too, which
    // the casters' fingerprint does not see: it must be the same plane.
    {
        LLCamera now_clip = entry.camera;
        setCullClipPlane(now_clip);
        const U32 planes = entry.camera.getAgentPlaneCount();
        if (now_clip.getAgentPlaneCount() != planes ||
            (planes > LLCamera::AGENT_PLANE_USER_CLIP &&
             !now_clip.getAgentPlane(LLCamera::AGENT_PLANE_USER_CLIP).equal(entry.camera.getAgentPlane(LLCamera::AGENT_PLANE_USER_CLIP))))
        {
            ++sSunCacheStats.projection_clip;
            plus::shadowshift::noteReject(j, plus::shadowshift::WHY_WATER); // <plus> plus/shadowshift </plus>
            return false;
        }
    }
    // The sun moves slowly with the day cycle: allow 0.05 degrees.
    LLVector3 a = light_dir, b = entry.light_dir;
    a.normVec();
    b.normVec();
    if (a * b < 0.99999962f)
    {
        ++sSunCacheStats.projection_light;
        plus::shadowshift::noteReject(j, plus::shadowshift::WHY_SUN); // <plus> plus/shadowshift </plus>
        return false;
    }
    // Every receiver must project inside the cached shadow map, and the receivers
    // must still span (almost) all of it, so no resolution is lost.
    glm::mat4 vp = entry.proj * entry.view;
    F32 min_x = 2.f, min_y = 2.f, max_x = -2.f, max_y = -2.f;
    bool outside = false;   // <plus> plus/shadowshift: was a return in the loop </plus>
    U32* outside_why = nullptr; // <plus> plus/shadowdist (research/178 C1): the counter of where it was </plus>
    for (const LLVector3& p : fp)
    {
        glm::vec4 c = vp * glm::vec4(p.mV[0], p.mV[1], p.mV[2], 1.f);
        if (c.w <= 0.f)
        {
            outside = true; // <plus> plus/shadowshift (was: ++projection_outside, return false) </plus>
            outside_why = &sSunCacheStats.outside_behind;   // <plus> plus/shadowdist (research/178 C1) </plus>
            break;          // <plus> plus/shadowshift </plus>
        }
        F32 x = c.x / c.w, y = c.y / c.w, z = c.z / c.w;
        if (x < -1.f || x > 1.f || y < -1.f || y > 1.f || z < -1.f || z > 1.f)
        {
            outside = true; // <plus> plus/shadowshift (was: ++projection_outside, return false) </plus>
            // <plus> plus/shadowdist (research/178 C1): across the map, or along the light only
            outside_why = (x < -1.f || x > 1.f || y < -1.f || y > 1.f) ? &sSunCacheStats.outside_xy : &sSunCacheStats.outside_z;
            // </plus>
            break;          // <plus> plus/shadowshift </plus>
        }
        min_x = llmin(min_x, x); max_x = llmax(max_x, x);
        min_y = llmin(min_y, y); max_y = llmax(max_y, y);
    }
    // <plus> plus/shadowshift: a receiver outside it: the same grid moved by whole texels, when it can be
    if (outside)
    {
        if (plus::shadowshift::accept(j, entry.view, entry.proj, view, proj, shadow_cam, fp, mRT->shadow[j].getWidth(),
                                      mRT->shadow[j].getHeight()))
        {
            entry.reused = true;    // renderCachedShadowCascade moves the cached depth (drawn_at stays)
            return true;
        }
        ++sSunCacheStats.projection_outside;
        if (outside_why)    // plus/shadowdist (research/178 C1): where it was
        {
            ++*outside_why;
        }
        return false;
    }
    // </plus>
    // <plus> plus/shadowdist: a stable cascade of the same size and turn has the same texels
    // wherever its centre is: any that still holds the receivers will do (no span test)
    const bool same_grid = plus::shadowdist::sameStableGrid(entry.view, entry.proj, view, proj);
    // </plus>
    if (!same_grid && (max_x - min_x < 1.9f || max_y - min_y < 1.9f))
    {
        ++sSunCacheStats.projection_span;
        return false;
    }
    view = entry.view;
    proj = entry.proj;
    shadow_cam = entry.camera;
    entry.reused = true;
    return true;
}

void LLPipeline::renderCachedShadowCascade(ShadowCascade& cascade, ShadowCascadeCasters& casters, F32 now, bool& refreshed_one,
                                           EShadowCache cache_set)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    const bool probe = cache_set == SHADOW_CACHE_PROBE;    // LLRHI: the probes' shared maps (shareProbeShadows)
    ShadowCacheEntry& entry = probe ? mProbeShadowCacheEntry[cascade.index] : mShadowCacheEntry[cascade.index];
    LLRenderTarget& shadow = mRT->shadow[cascade.index];
    LLRenderTarget& cache = probe ? mProbeShadowStaticCache[cascade.index] : mShadowStaticCache[cascade.index];
    ShadowCacheStats& stats = shadow_cache_stats(cache_set);

    // <plus> plus/shadowshift (research/151): this frame's window is the cached one moved by whole
    // texels (reuseShadowCascade): the cull's casters differ by what moved in and out, so the
    // fingerprint is not compared; drawShift checks what the window's two positions share instead.
    const S32 plus_index = probe ? -1 : cascade.index;     // the probes' maps have their own cache
    const S32 plus_position = probe ? -1 : shadow_sort_position(casters.cached);
    const bool plus_shifted = plus::shadowshift::beginDraw(plus_index, plus_position);
    const bool plus_reused = entry.reused && entry.valid;
    const bool plus_cull = !plus_shifted && entry.fingerprint != casters.fingerprint;
    const bool plus_list = entry.list_dirty;
    const bool plus_resized = cache.getWidth() != shadow.getWidth() || cache.getHeight() != shadow.getHeight();
    // </plus>
    bool redraw = !entry.reused || !entry.valid || plus_cull || entry.list_dirty; // <plus> plus_cull (was: the fingerprints differ) </plus>
    if (!entry.reused || !entry.valid)
    {
        ++stats.redraw_projection;
    }
    else if (plus_cull) // <plus> plus/shadowshift (was: entry.fingerprint != casters.fingerprint) </plus>
    {
        // A cached group changed this frame, or the cull found other static groups.
        ++(casters.changed ? stats.redraw_contents : stats.redraw_cull);
    }
    else if (entry.list_dirty)
    {
        ++stats.redraw_contents;    // LLRHI: a resident group in the cached depth changed (rhiListRevoke)
    }
    if (cache.getWidth() != shadow.getWidth() || cache.getHeight() != shadow.getHeight())
    {
        ++stats.redraw_size;
        cache.release();
        if (!cache.allocate(shadow.getWidth(), shadow.getHeight(), 0, true))
        {
            entry.valid = false;
            // LLRHI (fix-round22): no stale depth from an earlier frame in this map.
            shadow.bindTarget();
            shadow.getViewport(gGLViewport);
            shadow.clear();
            shadow.flush();
            return;
        }
        redraw = true;
    }
    // Refresh now and then regardless (one cascade per frame), so nothing stale lingers.
    if (!redraw && !refreshed_one && now - entry.drawn_at > 2.f)
    {
        ++stats.redraw_age;
        redraw = true;
        refreshed_one = true;
    }

    // <plus> plus/shadowshift: the cached depth moved by whole texels into the shadow map, the
    // static casters of the strips that came into view drawn over it, and copied back. Not
    // when the casters the two windows share differ (or the casters' bias clamp): redrawn whole.
    if (plus_shifted && !redraw)
    {
        plus::shadowshift::beginStrips(cascade.index, cascade.view, cascade.proj);
        shadow_sort_visit_static(plus_position, [](U32 type, LLDrawInfo* info, U64 hash, bool cached)
        {
            if (cached)
            {
                plus::shadowshift::noteCached(type, info, hash);
            }
        });
        if (plus::shadowshift::drawShift(cascade.index, shadow, cache, cascade.view, cascade.proj, cascade.camera, entry.list_serial))
        {
            // The new window holds the cached casters the cull found now; drawn_at (the
            // casters' cutoff) and the GPU list's serial stay those of the last whole redraw.
            entry.view = cascade.view;
            entry.proj = cascade.proj;
            entry.camera = cascade.camera;
            entry.fingerprint = casters.fingerprint;
        }
        else
        {
            ++stats.redraw_cull;
            redraw = true;
        }
    }
    // </plus>

    if (redraw)
    {
        // LLRHI: the cached casters, when the sort left them to be listed now (research/57).
        shadow_sort_fill(casters.cached);
        // Groups that settled since the last draw join the cache now.
        for (const std::pair<U32, LLDrawInfo*>& joiner : *casters.joining)
        {
            casters.cached->pushDrawInfo(joiner.first, joiner.second);
        }
        cache.bindTarget();
        cache.getViewport(gGLViewport);
        cache.clear();
        grabReferences(*casters.cached);
        // LLRHI: the GPU shadow list's resident volume groups go into the cached
        // depth too, all of them granted so far (research/46 G3).
        const U32 list_serial = LLRHI::sShadowList ? LLRHI::sShadowList->serial() : 0;
        mRHIListPass = RHIListPass();
        mRHIListPass.volume = (mRHIListKinds & 1) != 0;
        mRHIListPass.since_max = list_serial;
        LLSceneShadow::CacheScope scene_cache; // <plus> research/206 5b: the static groups the retained scene draws into it (to the end of the redraw: nothing after draws) </plus>
        renderShadowCasters(cascade.view, cascade.proj, cascade.camera, true, false);
        mRHIListPass = RHIListPass();
        cache.flush();
        // Only what this redraw really took from the list (none with volume groups off).
        entry.list_serial = (mRHIListKinds & 1) ? list_serial : 0;
        entry.list_dirty = false;

        entry.valid = true;
        entry.view = cascade.view;
        entry.proj = cascade.proj;
        entry.camera = cascade.camera;
        // <plus> plus/shadowdist: the held direction the cascades were made with
        entry.light_dir = -plus::shadowdist::shadowCasterDir(LLVector3(LLEnvironment::instance().getIsSunUp() ? mSunDir : mMoonDir),
                                                             LLEnvironment::instance().getIsSunUp());
        // </plus>
        entry.fingerprint = casters.fingerprint + casters.joining_fingerprint;
        entry.drawn_at = now;
        ++stats.static_redrawn;
        // <plus> research/206 5b: the retained scene drew records into it that its verify found different from
        // their batches: drawn again next frame (the usual way by then), not kept until the next reason
        if (gSceneShadow.cacheDiffered())
        {
            entry.valid = false;
        }
        // </plus>
        // <plus> plus/shadowshift: what the cached depth now holds, for the next move (and why it was drawn)
        if (plus::shadowshift::recording(plus_index, plus_position))
        {
            plus::shadowshift::beginContent(plus_index, cascade.view, cascade.proj, shadow.getWidth(), shadow.getHeight(),
                                            !plus_reused   ? plus::shadowshift::WHY_PROJECTION
                                            : plus_resized ? plus::shadowshift::WHY_TARGET
                                            : plus_cull    ? (casters.changed ? plus::shadowshift::WHY_CONTENTS : plus::shadowshift::WHY_CULL)
                                            : plus_list    ? plus::shadowshift::WHY_LIST
                                                           : plus::shadowshift::WHY_AGE);
            shadow_sort_visit_static(plus_position, [](U32, LLDrawInfo* info, U64 hash, bool)
            {
                plus::shadowshift::noteContent(info, hash);
            });
            plus::shadowshift::endContent();
        }
        // </plus>
    }
    else
    {
        // Not in the cached depth yet: drawn with the moving casters until the next redraw.
        for (const std::pair<U32, LLDrawInfo*>& joiner : *casters.joining)
        {
            casters.moving->pushDrawInfo(joiner.first, joiner.second);
        }
        stats.deferred_infos += casters.joining->size();
        ++stats.cascades_reused;
    }
    ++stats.cascades_drawn;

    // Shadow map = cached static depth, then the moving casters on top.
    shadow.bindTarget();
    shadow.getViewport(gGLViewport);
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        LLGLDisable cull(GL_CULL_FACE);
        // <plus> fragcolor-depth: depth only, the shadow map has no colour (was: gCopyDepthProgram, copyF.glsl's unused frag_color)
        gShadowCopyDepthProgram.bind();
        S32 depth_map = gShadowCopyDepthProgram.getTextureChannel(LLShaderMgr::DEFERRED_DEPTH);
        gGL.getTexUnit(depth_map)->bind(&cache, true);
        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        gGL.getTexUnit(depth_map)->unbind(LLTexUnit::TT_TEXTURE);
        gShadowCopyDepthProgram.unbind();
        // </plus>
    }
    grabReferences(*casters.moving);
    // LLRHI: resident groups granted after the cached depth was drawn, and terrain.
    mRHIListPass = RHIListPass();
    mRHIListPass.volume = (mRHIListKinds & 1) != 0 && entry.list_serial < U32_MAX;
    mRHIListPass.terrain = (mRHIListKinds & 2) != 0;
    mRHIListPass.since_min = entry.list_serial + 1;
    mRHIListPass.since_max = U32_MAX;
    LLSceneShadow::MovingScope scene_moving; // <plus> research/206: this cascade's moving casters (the scene's among them), to the end </plus>
    renderShadowCasters(cascade.view, cascade.proj, cascade.camera, true, true);
    mRHIListPass = RHIListPass();
    shadow.flush();
}

void LLPipeline::logShadowCacheStats(size_t cascades, EShadowCache cache)
{
    ShadowCacheStats& st = shadow_cache_stats(cache);
    const bool probe = cache == SHADOW_CACHE_PROBE;
    ++st.frames;
    if (gFrameTimeSeconds - st.logged_at >= 5.f)
    {
        const F64 per = 1.0 / llmax(st.cascades_drawn, 1u);     // per cascade drawn
        std::ostringstream line;
        if (probe)
        {
            // LLRHI: the probes' shared maps (shareProbeShadows); frames are probe updates.
            line << "probe shadow cache over " << st.frames << " probe updates: maps " << st.cascades_drawn << ", static reused "
                 << st.cascades_reused << ", static redrawn " << st.static_redrawn << " (projection " << st.redraw_projection << " [sun "
                 << st.projection_light << ", probe moved " << st.projection_outside << ", other " << st.projection_span
                 << "], contents ";
        }
        else
        {
            line << "sun shadow cache over " << st.frames << " frames: cascades " << st.cascades_drawn << ", static reused "
                 << st.cascades_reused << ", static redrawn " << st.static_redrawn << " (projection " << st.redraw_projection << " [sun "
                 << st.projection_light << ", outside " << st.projection_outside << ", resolution " << st.projection_span
                 << ", water plane " << st.projection_clip << "], contents ";
        }
        line << st.redraw_contents << ", cull " << st.redraw_cull << ", age " << st.redraw_age << ", size " << st.redraw_size
             << "); infos per " << (probe ? "map" : "cascade") << ": cached " << (U64)(st.static_infos * per) << ", joining "
             << (U64)(st.joining_infos * per) << " (deferred " << (U64)(st.deferred_infos * per) << "), moving bridge "
             << (U64)(st.moving_bridge * per) << " unsettled " << (U64)(st.moving_unsettled * per) << " rigged "
             << (U64)(st.moving_rigged * per) << " animated " << (U64)(st.moving_animated * per) << "; groups changed "
             << st.groups_changed << " of " << st.groups_seen << " seen";
        if (!probe)
        {
            // <plus> plus/shadowdist (research/178 C1): where the receivers left the cached cascades, and the depth range
            line << "; outside across " << st.outside_xy << ", along the light " << st.outside_z << ", behind " << st.outside_behind
                 << ", depth range in steps " << (plus::shadowdist::depthStepsActive() ? "on" : "off") << " (z hold "
                 << (plus::shadowdist::zHoldOn() ? "on" : "off") << ")";
            // </plus>
            line << "; native shadow packets per frame " << sNativePackets / llmax(st.frames, 1u) << ", merged "
                 << sNativeMerged / llmax(st.frames, 1u);
            sNativePackets = sNativeMerged = 0;
            {
                // LLRHI (research/66 step 2B): building them. "built in" is the
                // main thread's time either way (compare it across shadowpacket.toggle);
                // saved ~ the cull threads' chunks minus waiting for them and the fold.
                RHIPacketStats& ps = sRHIPacketStats;
                const F64 f = 1.0 / llmax(st.frames, 1u);
                const F64 to_ms = 1e-6 * f;
                const U64 worker_ns = sRHIPacketWorkerNs.exchange(0, std::memory_order_relaxed);
                const U32 worker_chunks = sRHIPacketWorkerChunks.exchange(0, std::memory_order_relaxed);
                line << "; packets built in " << ps.build_ns * to_ms << " ms a frame on the main thread (packet threads "
                     << (LLRHI::sShadowPacketThreads ? "on" : "off") << ": " << ps.threaded * f << " of " << ps.passes * f
                     << " passes a frame, " << ps.chunks * f << " chunks of " << (U64)(ps.infos * f) << " opaque batches; cull threads "
                     << worker_chunks * f << " chunks in " << worker_ns * to_ms << " ms, main " << ps.main_chunks * f << " in "
                     << ps.main_chunk_ns * to_ms << " ms; rigged and alpha types " << ps.impure_ns * to_ms << " ms, wait "
                     << ps.wait_ns * to_ms << " ms, fold " << ps.fold_ns * to_ms << " ms; main saved ~"
                     << ((F64)worker_ns - (F64)ps.wait_ns - (F64)ps.fold_ns) * to_ms << " ms";
                if (ps.fallbacks)
                {
                    line << "; batches with an avatar in an opaque type " << ps.fallbacks;
                }
                line << ")";
                ps = RHIPacketStats();
            }
            // LLRHI: the receiver cull (research/64).
            const F64 per_frame = 1.0 / llmax(st.frames, 1u);
            line << "; receiver cull " << (LLRHI::sShadowReceiverCull ? "on" : "off") << " (frames culling "
                 << st.receiver_frames_on << ", held by anisotropic shadow maps " << st.receiver_frames_aniso
                 << ", held otherwise " << st.receiver_frames_held << ", cascades unbounded "
                 << st.receiver_cascades_inactive << "), moving infos per frame tested/skipped";
            for (U32 j = 0; j < 4; ++j)
            {
                line << " sun" << j << " " << (U64)(st.receiver_tested[j] * per_frame) << "/"
                     << (U64)(st.receiver_skipped[j] * per_frame);
            }
        }
        LL_INFOS("RHIShadow") << line.str() << LL_ENDL;
        st = ShadowCacheStats();
        st.logged_at = gFrameTimeSeconds;
    }
}

// ----------------------------------------------------------------------------
// LLRHI: shared sun shadow maps for a multi-face probe update.

void LLPipeline::shareProbeShadows(LLCamera& camera, const LLVector3& light_dir, const LLPlane& shadow_near_clip, const glm::mat4& inv_view)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    SharedProbeShadows& shared = mSharedProbeShadows;
    // A 90 degree square face sees points up to 1/cos(54.7 deg) = 1.73 times their
    // view depth away; the shader picks a cascade up to 1.25 times its split.
    const F32 radius_scale = 1.25f * 1.7321f;

    // LLRHI: the static cache of the shared maps (research/45). Off: its depth images go.
    const bool use_cache = LLRHI::sCacheProbeShadows;
    if (!use_cache)
    {
        for (U32 j = 0; j < 2; ++j)
        {
            mProbeShadowCacheEntry[j].valid = false;
            if (mProbeShadowStaticCache[j].getWidth())
            {
                mProbeShadowStaticCache[j].release();
            }
        }
    }

    const LLVector3 origin = camera.getOrigin();
    if (!shared.rendered || dist_vec_squared(origin, shared.origin) > 1e-6f || shared.light_dir != light_dir)
    {
        // Split distances of this (first) face; every face uses them.
        shared.clip = mSunClipPlanes;
        F32 radius[2] = { shared.clip.mV[0] * radius_scale, shared.clip.mV[1] * radius_scale };

        // Two maps cover all six faces: scale the resolution so a face gets at
        // least the texels it had with its own maps (sqrt(6) ~ 2.5).
        // LLRHI (fix-round22): the face size comes from map 2, which
        // this never widens (maps 0 and 1 keep the last widening, so taking it from them
        // grew the maps on every probe update up to 4096).
        const LLRenderTarget& face_map = mRT->shadow[2];
        const U32 face_res = llmax(face_map.getWidth(), face_map.getHeight());
        if (!shared.rendered && face_res)
        {
            U32 want = llmin((U32)4096, (U32)(face_res * 5 / 2 + 15) & ~15u);
            for (U32 j = 0; j < 2; ++j)
            {
                LLRenderTarget& target = mRT->shadow[j];
                if (target.getWidth() < want || target.getHeight() < want)
                {
                    target.release();
                    if (target.allocate(want, want, 0, true))
                    {
                        gGL.getTexUnit(0)->bind(&target, true);
                        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_ANISOTROPIC);
                        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                        LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);     // <plus> research/207 6.3 </plus>
                        LLRHI::texParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);     // <plus> research/207 6.3 </plus>
                        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
                    }
                }
            }
        }

        // Light space axes that do not depend on the face.
        LLVector3 up = fabsf(light_dir.mV[2]) < 0.9f ? LLVector3(0.f, 0.f, 1.f) : LLVector3(1.f, 0.f, 0.f);

        static std::vector<ShadowCascade> cached_maps;     // LLRHI: with the cache, drawn together below
        cached_maps.clear();
        for (S32 j = 0; j < 2; ++j)
        {
            const F32 r = radius[j];
            // Eye fixed at the agent origin so the texel grid stays put as the probe moves.
            glm::mat4 view = look(LLVector3(0.f, 0.f, 0.f), light_dir, up);
            // Snap the center to the texel grid so the maps do not shimmer as probes move.
            const F32 texel = 2.f * r / (F32)llmax(1u, mRT->shadow[j].getWidth());
            glm::vec4 center = view * glm::vec4(origin.mV[0], origin.mV[1], origin.mV[2], 1.f);
            const F32 cx = floorf(center.x / texel) * texel, cy = floorf(center.y / texel) * texel;
            glm::mat4 proj = glm::ortho(cx - r, cx + r, cy - r, cy + r, -center.z - r, -center.z + r);

            LLCamera shadow_cam = camera;
            set_current_modelview(view);
            set_current_projection(proj);
            LLViewerCamera::updateFrustumPlanes(shadow_cam, false, false, true);
            shadow_cam.getAgentPlane(LLCamera::AGENT_PLANE_NEAR).set(shadow_near_clip);

            shared.view[j] = view;
            shared.proj[j] = proj;

            if (use_cache)
            {
                // LLRHI: the cached static depth fits only exactly the same map: the same
                // probe origin, sun (view), projection and cull planes (the near plane
                // follows the draw distance; the water plane, which the cull sets as here,
                // also limits what the GPU shadow list drew into it) as when it was drawn.
                ShadowCacheEntry& entry = mProbeShadowCacheEntry[j];
                entry.reused = false;
                if (entry.valid)
                {
                    setCullClipPlane(shadow_cam);
                    const U32 planes = shadow_cam.getAgentPlaneCount();
                    bool same_planes = entry.camera.getAgentPlaneCount() == planes;
                    for (U32 p = 0; p < planes; ++p)
                    {
                        same_planes = same_planes && entry.camera.getAgentPlane(p).equal(shadow_cam.getAgentPlane(p));
                    }
                    if (entry.camera.getOrigin() != origin)
                    {
                        ++sProbeCacheStats.projection_outside;
                    }
                    else if (entry.view != view)
                    {
                        ++sProbeCacheStats.projection_light;
                    }
                    else if (entry.proj != proj || !same_planes)
                    {
                        ++sProbeCacheStats.projection_span;
                    }
                    else
                    {
                        entry.reused = true;
                    }
                }
                cached_maps.push_back({ j, view, proj, shadow_cam });
                continue;
            }

            LLViewerCamera::sCurCameraID = (LLViewerCamera::eCameraID)(LLViewerCamera::CAMERA_SUN_SHADOW0 + j);
            mRT->shadow[j].bindTarget();
            mRT->shadow[j].getViewport(gGLViewport);
            mRT->shadow[j].clear();
            {
                static LLCullResult result[2];
                renderShadow(view, proj, shadow_cam, result[j], true);
            }
            mRT->shadow[j].flush();
        }
        if (!cached_maps.empty())
        {
            // LLRHI: both maps culled at once and sorted into cached (static, settled)
            // and moving casters; each map copies its cached depth and draws only the
            // moving ones on top, unless what its cached depth holds has changed.
            renderSunShadowCascades(cached_maps, SHADOW_CACHE_PROBE);
        }
        // Beyond the second split the faces are lit, as with their own maps.
        for (S32 j = 2; j < 4; ++j)
        {
            mRT->shadow[j].bindTarget();
            {
                LLGLDepthTest depth(GL_TRUE);
                mRT->shadow[j].clear();
            }
            mRT->shadow[j].flush();
        }
        shared.rendered = true;
        shared.origin = origin;
        shared.light_dir = light_dir;
    }

    // This face reads the shared maps.
    const glm::mat4 trans(0.5f, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.5f, 0.0f, 0.0f,
                          0.0f, 0.0f, 0.5f, 0.0f,
                          0.5f, 0.5f, 0.5f, 1.0f);
    mSunClipPlanes = shared.clip;
    for (S32 j = 0; j < 2; ++j)
    {
        mSunShadowMatrix[j] = trans * shared.proj[j] * shared.view[j] * inv_view;
    }
}

// ----------------------------------------------------------------------------
// LLRHI: GPU shadow list (research/46 G2, G3).
//
// Rows: a static volume group's opaque batches (the 13 shadowV render types,
// region space, not rigged) are registered while the CPU sorts the group for a
// shadow pass (rhiListTrack, from postSort); terrain faces while they are made
// into packets (rhiListTerrainRow). A batch with a live row is drawn from the
// list instead of as a packet (G2). Rows go with the group's draw map
// (clearDrawMap: rebuild, LOD change), with an in-place mesh rebuild
// (rebuildMesh) and when GL writes their buffers (the pool's callback).
//
// Residency (G3): a group all of whose shadow casters have rows (nothing the
// list cannot draw: alpha, GLTF, rigged), clean and unchanged for a few
// seconds, becomes resident at the start of a shadow pass (rhiListPhase). The
// sun shadow cameras' culls then leave it out; the list's compute shader tests
// its object bounds against the same planes the CPU cull would and draws its
// rows. It stops being resident as soon as anything about it changes: a row
// dropped, a dirty state, other bounds, the auto-hide limit, the switches.

namespace
{
    // As the static shadow cache's groups settle (SHADOW_SETTLE_SECONDS).
    constexpr F32 RHI_LIST_SETTLE_SECONDS = 3.f;
    // A group no shadow pass sorted for this long (out of view) gives its rows
    // and their pinned pool copies back (resident groups are culled by the
    // GPU, so they keep theirs).
    constexpr F32 RHI_LIST_UNSORTED_SECONDS = 30.f;
    // The group's draw infos or buffers are about to change.
    constexpr U32 RHI_LIST_DIRTY = LLSpatialGroup::GEOM_DIRTY | LLSpatialGroup::ALPHA_DIRTY | LLSpatialGroup::MESH_DIRTY |
                                   LLSpatialGroup::NEW_DRAWINFO | LLSpatialGroup::IN_BUILD_Q1 | LLSpatialGroup::IN_BUILD_Q2;
    constexpr U8 RHI_LIST_VOLUME = 1;
    constexpr U8 RHI_LIST_TERRAIN = 2;

    struct RHIListGroupInfo
    {
        LLSpatialGroup* group = nullptr;
        F32 complete_at = -1.f;         // frame time all its casters first had rows (-1: not all)
        U32 checked_builds = U32_MAX;   // rhiBuilds() when its draw map was last gone through to the end
        bool foreign = false;           // it has casters the list cannot draw
        U32 since = 0;                  // the serial it is resident with (0: not resident)
        LLVector4a bounds[2];           // object bounds (agent space) it was made resident with
        std::vector<std::pair<LLFace*, U64> > faces;    // terrain: face rows
        F32 sorted_at = 0.f;            // frame time a shadow pass last sorted it on the CPU
        bool has_rows = false;          // rows were added since its last clearGroup
    };
    // By group handle - 1. Never destroyed: groups go away at exit.
    std::vector<RHIListGroupInfo>& rhi_list_groups()
    {
        static std::vector<RHIListGroupInfo>* groups = new std::vector<RHIListGroupInfo>();
        return *groups;
    }
    bool sRHIListOn = false;            // G2, latched at the start of each shadow pass
    bool sRHIListCullOn = false;        // G3, the same
    bool sRHIListFailed = false;        // a pass could not draw the list
    U32 sRHIListAccepted = 0;           // passes with batches drawn from the list (since the last log)
    U32 sRHIListGrants = 0, sRHIListRevokes = 0, sRHIListDamaged = 0, sRHIListEvicted = 0;
    F32 sRHIListLoggedAt = 0.f;

    struct RHIListLock
    {
        LLRHI::ShadowList* list;
        explicit RHIListLock(LLRHI::ShadowList* l) : list(l)
        {
            if (list)
            {
                list->lock();
            }
        }
        ~RHIListLock()
        {
            if (list)
            {
                list->unlock();
            }
        }
    };

    // The render types drawNativeShadowCasters draws with shadowV (MODE_OPAQUE).
    bool rhi_list_opaque(U32 type)
    {
        switch (type)
        {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_FULLBRIGHT:
        case LLRenderPass::PASS_SHINY:
        case LLRenderPass::PASS_BUMP:
        case LLRenderPass::PASS_FULLBRIGHT_SHINY:
        case LLRenderPass::PASS_MATERIAL:
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE:
        case LLRenderPass::PASS_SPECMAP:
        case LLRenderPass::PASS_SPECMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMMAP:
        case LLRenderPass::PASS_NORMMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMSPEC:
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE:
            return true;
        default:
            return false;
        }
    }

    // Render types no shadow pass draws (renderShadowCasters). Anything else
    // that is not opaque keeps the group on the CPU.
    bool rhi_list_ignores(U32 type)
    {
        switch (type)
        {
        case LLRenderPass::PASS_INVISIBLE:
        case LLRenderPass::PASS_INVISI_SHINY:
        case LLRenderPass::PASS_POST_BUMP:
        case LLRenderPass::PASS_GLOW:
        case LLRenderPass::PASS_GLTF_GLOW:
            return true;
        default:
            return false;
        }
    }

    RHIListGroupInfo* rhi_list_info(LLSpatialGroup* group)
    {
        std::vector<RHIListGroupInfo>& infos = rhi_list_groups();
        const U32 slot = group->mRHIListGroup - 1;
        return group->mRHIListGroup && slot < infos.size() && infos[slot].group == group ? &infos[slot] : nullptr;
    }

    // The group's info, with a list handle made for it (list locked).
    RHIListGroupInfo* rhi_list_track(LLRHI::ShadowList* list, LLSpatialGroup* group, U8 kind)
    {
        if (RHIListGroupInfo* info = rhi_list_info(group))
        {
            return info;
        }
        const U32 handle = list->addGroup();
        if (!handle)
        {
            return nullptr;
        }
        std::vector<RHIListGroupInfo>& infos = rhi_list_groups();
        if (infos.size() < handle)
        {
            infos.resize(handle);
        }
        RHIListGroupInfo& info = infos[handle - 1];
        info = RHIListGroupInfo();
        info.group = group;
        group->mRHIListGroup = handle;
        group->mRHIListKind = kind;
        group->mRHIListResident = false;
        ++gPipeline.mRHIListResidency;  // the sun shadow culls read both (plus/shadowreuse)
        return &info;
    }

    // Its rows go (and their pins on the pool's copies); it is gone through again
    // when the CPU sorts it next (list locked, not resident).
    void rhi_list_drop_rows(LLRHI::ShadowList* list, RHIListGroupInfo& info)
    {
        list->clearGroup(info.group->mRHIListGroup);
        info.complete_at = -1.f;
        info.checked_builds = U32_MAX;
        info.foreign = false;
        info.faces.clear();
        info.has_rows = false;
    }

    bool rhi_list_autohidden(const LLSpatialGroup* group)
    {
        return LLPipeline::RenderAutoHideSurfaceAreaLimit > 0.f &&
               group->mSurfaceArea > LLPipeline::RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f);
    }

    // Nothing about what the CPU path would draw of the group is about to change.
    bool rhi_list_clean(LLSpatialGroup* group)
    {
        return !group->isDead() && !group->hasState(RHI_LIST_DIRTY) && !rhi_list_autohidden(group) &&
               group->getSpatialPartition() && group->getSpatialPartition()->mRegionp;
    }

    // LLRHI: rhiListPhase's scan, split (LLRHI::sShadowListThreads, research/57).
    // rhi_list_scan only reads, so the cull threads run it over all groups; the
    // main thread then goes through in order and does the whole step only for
    // the groups marked RHI_SCAN_ACT or whose rows the list dropped (takeDamaged).
    // The step changes nothing another group's scan reads.
    constexpr size_t RHI_LIST_SCAN_CHUNK = 256;     // groups per job
    constexpr size_t RHI_LIST_SCAN_MIN = 512;       // fewer: scanned on the main thread only
    constexpr U8 RHI_SCAN_TRACKED = 1;              // a group is there
    constexpr U8 RHI_SCAN_ACT = 2;                  // undamaged, it is still evicted, revoked or granted
    constexpr U8 RHI_SCAN_RESIDENT = 4;             // what is counted when nothing happens to it
    constexpr U8 RHI_SCAN_COMPLETE = 8;
    constexpr U8 RHI_SCAN_FOREIGN = 16;

    // The same conditions as the step in rhiListPhase, for a group whose rows were not dropped.
    U8 rhi_list_scan(const RHIListGroupInfo& info, F32 now, bool cull)
    {
        LLSpatialGroup* group = info.group;
        if (!group)
        {
            return 0;
        }
        const bool resident = group->mRHIListResident;
        bool act = !resident && info.has_rows && now - info.sorted_at > RHI_LIST_UNSORTED_SECONDS;
        if (!act)
        {
            const bool clean = rhi_list_clean(group);
            if (resident)
            {
                const LLVector4a* bounds = group->getObjectBounds();
                act = !cull || !clean || info.complete_at < 0.f || !bounds[0].equals3(info.bounds[0]) || !bounds[1].equals3(info.bounds[1]);
            }
            else
            {
                act = cull && clean && !info.foreign && info.complete_at >= 0.f && now - info.complete_at > RHI_LIST_SETTLE_SECONDS &&
                      (group->mRHIListKind != RHI_LIST_VOLUME || info.checked_builds == group->rhiBuilds());
            }
        }
        return (U8)(RHI_SCAN_TRACKED | (act ? RHI_SCAN_ACT : 0) | (resident ? RHI_SCAN_RESIDENT : 0) |
                    (info.complete_at >= 0.f ? RHI_SCAN_COMPLETE : 0) | (info.foreign ? RHI_SCAN_FOREIGN : 0));
    }

    // The batch as a row; false when the list can never draw it (no region
    // space bounds, rigged, another matrix).
    bool rhi_list_row(LLDrawInfo* info, const LLMatrix4& region_matrix, LLRHI::ShadowList::Row& row)
    {
        LLVertexBuffer* vb = info->mVertexBuffer.get();
        if (!vb || info->mAvatar.notNull() || !info->mHasExtents || info->mModelMatrix != &region_matrix)
        {
            return false;
        }
        row.vertex_buffer = vb->getGLBuffer();
        row.index_buffer = vb->getGLIndices();
        row.index_type = vb->getIndicesType();
        row.first_index = info->mOffset;
        row.index_count = info->mCount;
        row.position_offset = vb->getOffset(LLVertexBuffer::TYPE_VERTEX);
        row.vertex_count = vb->getNumVerts();
        row.index_total = vb->getNumIndices();
        row.model = (const F32*)region_matrix.mMatrix;
        for (U32 i = 0; i < 3; ++i)
        {
            row.lo[i] = info->mExtents[0][i];
            row.hi[i] = info->mExtents[1][i];
        }
        row.loose = false;
        row.pool = false;
        return true;
    }
}

void LLPipeline::rhiListTrack(LLSpatialGroup* group)
{
    if (!sRHIListOn || !LLRHI::sShadowList || group->mRHIListResident)
    {
        return;
    }
    LLSpatialPartition* part = group->getSpatialPartition();
    if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME || part->asBridge() || !part->mRegionp ||
        group->isDead() || group->hasState(RHI_LIST_DIRTY))
    {
        return;
    }
    // Gone through since its last rebuild (complete, or with casters the list cannot draw).
    RHIListGroupInfo* known = rhi_list_info(group);
    if (known)
    {
        known->sorted_at = gFrameTimeSeconds;
        if (known->checked_builds == group->rhiBuilds())
        {
            return;
        }
    }
    LLRHI::ShadowList* list = LLRHI::sShadowList;
    RHIListLock lock(list);
    RHIListGroupInfo* info = rhi_list_track(list, group, RHI_LIST_VOLUME);
    if (!info)
    {
        return;
    }
    info->sorted_at = gFrameTimeSeconds;
    const LLMatrix4& region_matrix = part->mRegionp->mRenderMatrix;
    bool complete = true;
    bool foreign = false;
    for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
    {
        if (!rhi_list_opaque(j->first))
        {
            foreign |= !rhi_list_ignores(j->first) && !j->second.empty();
            continue;
        }
        for (LLDrawInfo* batch : j->second)
        {
            if (!batch->mCount)
            {
                continue;   // draws nothing
            }
            if (batch->mRHIShadowRow && list->rowLive(batch->mRHIShadowRow))
            {
                continue;
            }
            batch->mRHIShadowRow = 0;
            LLRHI::ShadowList::Row row;
            if (!rhi_list_row(batch, region_matrix, row))
            {
                foreign = true;
                continue;
            }
            if (!row.vertex_buffer || !row.index_buffer)
            {
                complete = false;   // not in GL yet: again later
                continue;
            }
            batch->mRHIShadowRow = list->addRow(group->mRHIListGroup, row);
            if (!batch->mRHIShadowRow)
            {
                complete = false;   // the pool's copy budget or room, or the tables: again later
            }
            else
            {
                info->has_rows = true;
            }
        }
    }
    info->foreign = foreign;
    if (foreign || complete)
    {
        info->checked_builds = group->rhiBuilds();
    }
    if (complete && !foreign && info->complete_at < 0.f)
    {
        info->complete_at = gFrameTimeSeconds;
    }
}

// A terrain face's row (list locked); 0: draw it as a packet.
U64 LLPipeline::rhiListTerrainRow(LLFace* face)
{
    LLRHI::ShadowList* list = LLRHI::sShadowList;
    LLDrawable* drawable = face ? face->getDrawable() : nullptr;
    LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
    LLViewerRegion* region = drawable ? drawable->getRegion() : nullptr;
    LLVertexBuffer* vb = face ? face->getVertexBuffer() : nullptr;
    if (!list || !group || !region || !vb || group->isDead() || group->hasState(RHI_LIST_DIRTY) ||
        !group->getSpatialPartition() || group->getSpatialPartition()->mPartitionType != LLViewerRegion::PARTITION_TERRAIN ||
        !face->getIndicesCount() || !vb->getGLBuffer() || !vb->getGLIndices())
    {
        return 0;
    }
    RHIListGroupInfo* info = rhi_list_track(list, group, RHI_LIST_TERRAIN);
    if (!info)
    {
        return 0;
    }
    info->sorted_at = gFrameTimeSeconds;
    U64 handle = 0;
    for (size_t i = 0; i < info->faces.size(); ++i)
    {
        if (info->faces[i].first == face)
        {
            if (list->rowLive(info->faces[i].second))
            {
                handle = info->faces[i].second;
            }
            else
            {
                info->faces[i] = info->faces.back();
                info->faces.pop_back();
            }
            break;
        }
    }
    if (!handle)
    {
        // As rhi_add_terrain_packet: the face's indices, the region's render matrix.
        LLRHI::ShadowList::Row row;
        row.vertex_buffer = vb->getGLBuffer();
        row.index_buffer = vb->getGLIndices();
        row.index_type = vb->getIndicesType();
        row.first_index = (U32)face->getIndicesStart();
        row.index_count = face->getIndicesCount();
        row.position_offset = vb->getOffset(LLVertexBuffer::TYPE_VERTEX);
        row.vertex_count = vb->getNumVerts();
        row.index_total = vb->getNumIndices();
        row.model = (const F32*)region->mRenderMatrix.mMatrix;
        // A patch's extents may not hold all its vertices: no per-row test, the group's bounds decide.
        row.loose = true;
        row.pool = true;
        handle = list->addRow(group->mRHIListGroup, row);
        if (!handle)
        {
            return 0;
        }
        info->faces.emplace_back(face, handle);
        info->has_rows = true;
    }
    if (info->complete_at < 0.f)
    {
        // Complete when every face the CPU would put in the terrain pools (as
        // stateSort(LLDrawable*)) and draw has a live row.
        bool complete = true;
        for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd() && complete; ++i)
        {
            LLDrawable* d = (LLDrawable*)(*i)->getDrawable();
            if (!d || d->isDead())
            {
                continue;
            }
            for (LLFace* f : d->mFaces)
            {
                if (!f || !f->hasGeometry())
                {
                    continue;
                }
                if (!f->getPool())
                {
                    break;
                }
                bool found = false;
                for (const auto& known : info->faces)
                {
                    if (known.first == f && list->rowLive(known.second))
                    {
                        found = true;
                        break;
                    }
                }
                // A face that draws nothing needs no row.
                LLVertexBuffer* fvb = f->getVertexBuffer();
                if (!found && f->getIndicesCount() && fvb && fvb->getGLBuffer() && fvb->getGLIndices())
                {
                    complete = false;
                    break;
                }
            }
        }
        if (complete)
        {
            info->complete_at = gFrameTimeSeconds;
        }
    }
    return handle;
}

void LLPipeline::rhiListClear(LLSpatialGroup* group)
{
    LLRHI::ShadowList* list = LLRHI::sShadowList;
    RHIListGroupInfo* info = rhi_list_info(group);
    if (!list || !info)
    {
        return;
    }
    RHIListLock lock(list);
    if (group->mRHIListResident)
    {
        rhiListRevoke(group);
    }
    rhi_list_drop_rows(list, *info);
}

void LLPipeline::rhiListForget(LLSpatialGroup* group)
{
    LLRHI::ShadowList* list = LLRHI::sShadowList;
    RHIListGroupInfo* info = rhi_list_info(group);
    if (list && info)
    {
        RHIListLock lock(list);
        if (group->mRHIListResident)
        {
            rhiListRevoke(group);
        }
        list->removeGroup(group->mRHIListGroup);
        *info = RHIListGroupInfo();
    }
    group->mRHIListGroup = 0;
    group->mRHIListKind = 0;
    group->mRHIListResident = false;
    ++mRHIListResidency;
}

// Not resident any more (list locked). The cached sun shadow cascades drawn
// with it hold its old depth: those its bounds may reach are redrawn.
void LLPipeline::rhiListRevoke(LLSpatialGroup* group)
{
    RHIListGroupInfo* info = rhi_list_info(group);
    if (LLRHI::sShadowList)
    {
        LLRHI::sShadowList->revoke(group->mRHIListGroup);
    }
    group->mRHIListResident = false;
    ++mRHIListResidency;
    ++sRHIListRevokes;
    if (info)
    {
        info->sorted_at = gFrameTimeSeconds;    // the CPU sorts it from now on: not out of view
    }
    if (!info || !info->since)
    {
        return;
    }
    // Wider than the list's own test, which keeps groups a little beyond the planes.
    LLVector4a radius = info->bounds[1];
    LLVector4a pad;
    const F32 reach = fabsf(info->bounds[0][0]) + fabsf(info->bounds[0][1]) + fabsf(info->bounds[0][2]);
    pad.splat(0.05f + 1e-4f * reach);
    radius.add(pad);
    for (ShadowCacheEntry& entry : mShadowCacheEntry)
    {
        if (entry.valid && entry.list_serial >= info->since && entry.camera.AABBInFrustum(info->bounds[0], radius))
        {
            entry.list_dirty = true;
        }
    }
    for (ShadowCacheEntry& entry : mProbeShadowCacheEntry)  // the probes' shared maps likewise
    {
        if (entry.valid && entry.list_serial >= info->since && entry.camera.AABBInFrustum(info->bounds[0], radius))
        {
            entry.list_dirty = true;
        }
    }
    info->since = 0;
}

void LLPipeline::rhiListFailed()
{
    if (!sRHIListFailed)
    {
        LL_WARNS("RHIShadow") << "GPU shadow list could not draw a pass; shadow casters go back to the CPU" << LL_ENDL;
    }
    sRHIListFailed = true;
}

U32 LLPipeline::rhiListPhase(bool sun)
{
    LLRHI::ShadowList* list = LLRHI::sShadowList;
    const bool on = list && LLRHI::sGPUDriven && LLRHI::sGPUDrivenList && LLRHI::sNativeShadows && LLRHI::sDrawShadowPackets &&
                    !sRHIListFailed && !list->broken() && list->ready();
    const bool cull = on && LLRHI::sGPUDrivenCull && LLRHI::sGLDrawsDisabled;
    if (on != sRHIListOn || cull != sRHIListCullOn)
    {
        // The switches moved: nothing stays resident, and the cached cascades
        // are drawn again with whatever draws their casters now. With the list
        // off (or broken) its rows go too, so their pinned copies leave the pool.
        if (list)
        {
            RHIListLock lock(list);
            for (RHIListGroupInfo& info : rhi_list_groups())
            {
                if (!info.group)
                {
                    continue;
                }
                if (info.group->mRHIListResident)
                {
                    rhiListRevoke(info.group);
                }
                if (!on)
                {
                    rhi_list_drop_rows(list, info);
                }
            }
        }
        for (ShadowCacheEntry& entry : mShadowCacheEntry)
        {
            entry.valid = false;
        }
        for (ShadowCacheEntry& entry : mProbeShadowCacheEntry)
        {
            entry.valid = false;
        }
        LL_INFOS("RHIShadow") << "GPU shadow list " << (on ? "on" : "off") << ", GPU shadow cull " << (cull ? "on" : "off") << LL_ENDL;
        sRHIListOn = on;
        sRHIListCullOn = cull;
    }
    if (!on)
    {
        return 0;
    }

    if (cull)
    {
        // Object bounds as the cull will see them (each partition's cull rebounds its root first).
        for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
        {
            const U32 partitions[2] = { LLViewerRegion::PARTITION_VOLUME, LLViewerRegion::PARTITION_TERRAIN };
            for (U32 p : partitions)
            {
                LLSpatialPartition* part = region->getSpatialPartition(p);
                if (part && part->mOctree)
                {
                    ((LLSpatialGroup*)part->mOctree->getListener(0))->rebound();
                }
            }
        }
    }

    // GL writes that dropped rows, revocations, grants.
    const F32 now = gFrameTimeSeconds;
    U32 tracked = 0, resident = 0, complete = 0, foreign = 0;
    {
        RHIListLock lock(list);
        std::vector<RHIListGroupInfo>& infos = rhi_list_groups();
        // One group's step, after takeDamaged (damaged: rows of it were dropped).
        auto step = [&](RHIListGroupInfo& info, bool damaged)
        {
            LLSpatialGroup* group = info.group;
            if (damaged)
            {
                ++sRHIListDamaged;
                if (group->mRHIListResident)
                {
                    rhiListRevoke(group);
                }
                info.complete_at = -1.f;
                info.checked_builds = U32_MAX;  // gone through again when the CPU sorts it next
            }
            if (!group->mRHIListResident && info.has_rows && now - info.sorted_at > RHI_LIST_UNSORTED_SECONDS)
            {
                rhi_list_drop_rows(list, info);     // out of view: its pool copies go
                ++sRHIListEvicted;
            }
            const bool clean = rhi_list_clean(group);
            if (group->mRHIListResident)
            {
                const LLVector4a* bounds = group->getObjectBounds();
                if (!cull || !clean || info.complete_at < 0.f || !bounds[0].equals3(info.bounds[0]) || !bounds[1].equals3(info.bounds[1]))
                {
                    rhiListRevoke(group);
                }
            }
            else if (cull && clean && !info.foreign && info.complete_at >= 0.f && now - info.complete_at > RHI_LIST_SETTLE_SECONDS &&
                     (group->mRHIListKind != RHI_LIST_VOLUME || info.checked_builds == group->rhiBuilds()))
            {
                // Its cull bounds in the region's space (the list adds the region's matrix back).
                const LLVector4a* bounds = group->getObjectBounds();
                const LLMatrix4& m = group->getSpatialPartition()->mRegionp->mRenderMatrix;
                F32 center[3], radius[3];
                for (U32 j = 0; j < 3; ++j)
                {
                    center[j] = bounds[0][j] - m.mMatrix[3][j];
                    radius[j] = bounds[1][j];
                }
                const U32 since = list->grant(group->mRHIListGroup, center, radius, (const F32*)m.mMatrix);
                if (since)
                {
                    group->mRHIListResident = true;
                    ++mRHIListResidency;
                    info.since = since;
                    info.bounds[0] = bounds[0];
                    info.bounds[1] = bounds[1];
                    ++sRHIListGrants;
                }
            }
            resident += group->mRHIListResident ? 1 : 0;
            complete += info.complete_at >= 0.f ? 1 : 0;
            foreign += info.foreign ? 1 : 0;
        };
        const size_t count = infos.size();
        if (LLRHI::sShadowListThreads && LLRHI::sCullThreads && count >= RHI_LIST_SCAN_MIN)
        {
            // LLRHI: scanned on the cull threads, stepped in order here (research/57).
            static std::vector<U8> flags;
            static std::vector<U32> handles;
            flags.resize(count);
            handles.resize(count);
            const U32 jobs = (U32)((count + RHI_LIST_SCAN_CHUNK - 1) / RHI_LIST_SCAN_CHUNK);
            CullWorkers::instance().run(jobs, [&](U32 job)
            {
                const size_t first = (size_t)job * RHI_LIST_SCAN_CHUNK;
                const size_t last = llmin(first + RHI_LIST_SCAN_CHUNK, count);
                for (size_t i = first; i < last; ++i)
                {
                    flags[i] = rhi_list_scan(infos[i], now, cull);
                    handles[i] = infos[i].group ? infos[i].group->mRHIListGroup : 0;
                }
            });
            for (size_t i = 0; i < count; ++i)
            {
                const U8 f = flags[i];
                if (!(f & RHI_SCAN_TRACKED))
                {
                    continue;
                }
                ++tracked;
                const bool damaged = list->takeDamaged(handles[i]);
                if (damaged || (f & RHI_SCAN_ACT))
                {
                    step(infos[i], damaged);
                }
                else
                {
                    resident += (f & RHI_SCAN_RESIDENT) ? 1 : 0;
                    complete += (f & RHI_SCAN_COMPLETE) ? 1 : 0;
                    foreign += (f & RHI_SCAN_FOREIGN) ? 1 : 0;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < count; ++i)
            {
                RHIListGroupInfo& info = infos[i];
                if (!info.group)
                {
                    continue;
                }
                ++tracked;
                step(info, list->takeDamaged(info.group->mRHIListGroup));
            }
        }
    }
    if (now - sRHIListLoggedAt >= 10.f)
    {
        LL_INFOS("RHIShadow") << "GPU shadow list: groups " << tracked << ", complete " << complete << ", resident " << resident
                              << ", with casters it cannot draw " << foreign << "; since the last report granted " << sRHIListGrants
                              << ", revoked " << sRHIListRevokes << ", rows dropped by GL writes in " << sRHIListDamaged
                              << " groups, unsorted for " << (U32)RHI_LIST_UNSORTED_SECONDS << " s (rows given back) " << sRHIListEvicted
                              << ", passes drawing CPU-sorted rows " << sRHIListAccepted << LL_ENDL;
        sRHIListGrants = sRHIListRevokes = sRHIListDamaged = sRHIListAccepted = sRHIListEvicted = 0;
        sRHIListLoggedAt = now;
    }

    if (!cull || !sun)
    {
        return 0;
    }
    // The CPU path checks these per render type; the list draws a resident group whole.
    bool volume = hasRenderType(LLPipeline::RENDER_TYPE_VOLUME);
    for (U32 type = 0; type < LLRenderPass::NUM_RENDER_TYPES && volume; ++type)
    {
        if (rhi_list_opaque(type) && !hasRenderType(type))
        {
            volume = false;
        }
    }
    U32 kinds = volume ? RHI_LIST_VOLUME : 0;
    if (hasRenderType(LLPipeline::RENDER_TYPE_TERRAIN) && LLRHI::sNativeTerrainShadows)
    {
        kinds |= RHI_LIST_TERRAIN;
    }
    return kinds;
}

// ----------------------------------------------------------------------------
// LLRHI: shadow casters as native packets.

namespace
{
    U32 rhi_texture_name(LLViewerTexture* texture)
    {
        if (!texture)
        {
            return 0;
        }
        texture->setActive();
        LLImageGL* image = texture->getGLTexture();
        if (!image)
        {
            return 0;
        }
        if (!image->getTexName())
        {
            // Same as LLTexUnit::bindFast: get the image recreated (the draw gets the white stand-in).
            texture->forceImmediateUpdate();
        }
        return image->getTexName();
    }

    // p continues last: both opaque without a palette, the same buffers,
    // offsets and model matrix, and p's indices follow on from last's.
    inline bool rhi_continues(const LLRHI::ShadowPacket& last, const LLRHI::ShadowPacket& p)
    {
        return p.mode == LLRHI::ShadowPacket::MODE_OPAQUE && !p.palette && last.mode == LLRHI::ShadowPacket::MODE_OPAQUE &&
               !last.palette && last.vertex_buffer == p.vertex_buffer && last.index_buffer == p.index_buffer &&
               last.index_type == p.index_type && last.position_offset == p.position_offset && last.model == p.model &&
               last.first_index + last.index_count == p.first_index;
    }

    // Appends p, or extends the packet before when p continues it: an opaque
    // packet with the same buffers, offsets and model matrix whose indices follow
    // on. The same triangles in the same order, one draw instead of two.
    void rhi_push_packet(std::vector<LLRHI::ShadowPacket>& packets, const LLRHI::ShadowPacket& p)
    {
        ++sNativePackets;
        if (LLRHI::sMergeShadowPackets && !packets.empty() && rhi_continues(packets.back(), p))
        {
            packets.back().index_count += p.index_count;
            ++sNativeMerged;
            return;
        }
        packets.push_back(p);
    }

    // As LLDrawPoolTerrain::drawLoop: the face's indices, the region's render matrix as model.
    void rhi_add_terrain_packet(std::vector<LLRHI::ShadowPacket>& packets, LLFace* face)
    {
        LLVertexBuffer* vb = face ? face->getVertexBuffer() : nullptr;
        LLDrawable* drawable = face ? face->getDrawable() : nullptr;
        LLViewerRegion* region = drawable ? drawable->getRegion() : nullptr;
        if (!vb || !region || !face->getIndicesCount() || !vb->getGLBuffer() || !vb->getGLIndices())
        {
            return;
        }
        LLRHI::ShadowPacket p;
        p.vertex_buffer = vb->getGLBuffer();
        p.index_buffer = vb->getGLIndices();
        p.index_type = vb->getIndicesType();
        p.first_index = (U32)face->getIndicesStart();
        p.index_count = face->getIndicesCount();
        p.position_offset = vb->getOffset(LLVertexBuffer::TYPE_VERTEX);
        p.vertex_count = vb->getNumVerts();
        p.index_total = vb->getNumIndices();
        p.mode = LLRHI::ShadowPacket::MODE_OPAQUE;
        p.model = (const F32*)region->mRenderMatrix.mMatrix;
        rhi_push_packet(packets, p);
    }

    // What every packet of a batch takes from it and its vertex buffer. Reads
    // only (any thread): the batch, its buffer, its model matrix's address.
    inline void rhi_packet_fields(LLRHI::ShadowPacket& p, const LLDrawInfo* info, const LLVertexBuffer* vb, U32 mode, F32 cutoff)
    {
        p.vertex_buffer = vb->getGLBuffer();
        p.index_buffer = vb->getGLIndices();
        p.index_type = vb->getIndicesType();
        p.first_index = info->mOffset;
        p.index_count = info->mCount;
        p.position_offset = vb->getOffset(LLVertexBuffer::TYPE_VERTEX);
        p.vertex_count = vb->getNumVerts();
        p.index_total = vb->getNumIndices();
        p.mode = mode;
        p.alpha_cutoff = cutoff;
        p.model = info->mModelMatrix ? (const F32*)info->mModelMatrix->mMatrix : nullptr;
    }

    void rhi_add_packet(std::vector<LLRHI::ShadowPacket>& packets, LLDrawInfo* info, U32 mode, F32 cutoff, bool batched)
    {
        LLVertexBuffer* vb = info->mVertexBuffer.get();
        if (!vb || !info->mCount || !vb->getGLBuffer() || !vb->getGLIndices())
        {
            return;
        }
        LLRHI::ShadowPacket p;
        if (info->mAvatar.notNull())
        {
            // As LLRenderPass::uploadMatrixPalette: no skin data yet, no draw.
            if (!info->mSkinInfo || !vb->hasDataType(LLVertexBuffer::TYPE_WEIGHT4))
            {
                return;
            }
            const LLVOAvatar::MatrixPaletteCache& palette = info->mAvatar->updateSkinInfoMatrixPalette(info->mSkinInfo);
            if (palette.mMatrixPalette.empty())
            {
                return;
            }
            p.palette = palette.mGLMp.data();
            p.joint_count = (U32)palette.mMatrixPalette.size();
            p.weight_offset = vb->getOffset(LLVertexBuffer::TYPE_WEIGHT4);
            // LLRHI (research/56): the joints' row in the frame's palette table
            // instead of a copy per shadow pass (palette stays for the fallback).
            if (LLRHI::sPalettePrebuild && LLRenderPass::rhiPaletteRow(palette.mGLMp, palette.mRHIRow))
            {
                p.palette_row = palette.mRHIRow;
            }
            if (LLRHI::sAvatarStats)
            {
                LLRenderPass::rhiCountRigged(*info);
                ++LLRHI::sAvatarCounters.shadow_packets;
                LLRHI::sAvatarCounters.shadow_packet_rows += p.palette_row.frame != U32_MAX ? 1 : 0;
            }
        }
        rhi_packet_fields(p, info, vb, mode, cutoff);
        if (mode != LLRHI::ShadowPacket::MODE_OPAQUE)
        {
            if (vb->hasDataType(LLVertexBuffer::TYPE_TEXCOORD0))
            {
                p.texcoord_offset = vb->getOffset(LLVertexBuffer::TYPE_TEXCOORD0);
            }
            if (vb->hasDataType(LLVertexBuffer::TYPE_COLOR))
            {
                p.color_offset = vb->getOffset(LLVertexBuffer::TYPE_COLOR);
            }
            // As LLRenderPass::pushBatch: a texture list is indexed per vertex and
            // takes no texture matrix; a single texture may have one.
            if (batched && info->mTextureList.size() > 1)
            {
                for (size_t i = 0; i < info->mTextureList.size() && i < 4; ++i)
                {
                    p.textures[i] = rhi_texture_name(info->mTextureList[i].get());
                }
            }
            else
            {
                p.textures[0] = rhi_texture_name(info->mTexture.get());
                p.texture_matrix = info->mTextureMatrix ? (const F32*)info->mTextureMatrix->mMatrix : nullptr;
            }
        }
        rhi_push_packet(packets, p);
    }

    // ------------------------------------------------------------------------
    // LLRHI (research/66 step 2B): the opaque, non-rigged batches' packets
    // built on the cull threads (LLRHI::sShadowPacketThreads).
    //
    // Per batch, drawNativeShadowCasters and rhi_add_packet:
    //  - read: the batch (vertex buffer pointer, offset, count, model and
    //    texture matrix addresses, avatar pointer, cutoff, GPU list row), its
    //    vertex buffer's GL names, index type, offsets and sizes;
    //  - opaque types: the GPU list's rowLive (under the list lock) and the
    //    accept bit, or the packet;
    //  - rigged: LLVOAvatar::updateSkinInfoMatrixPalette (builds and caches the
    //    joints once a frame), LLRenderPass::rhiPaletteRow (the frame's palette
    //    table row, through the backend), the avatar counters;
    //  - alpha: rhi_texture_name (setActive, forceImmediateUpdate, GL name);
    //  - rhi_push_packet: the merge with the packet before; sNativePackets/Merged.
    // The opaque, non-rigged types (most batches) only read, but for rowLive
    // and the merge, which depend on what comes before. So a chunk of them (on
    // any thread) becomes candidates in order: packets that surely go in
    // (merged among themselves where they continue each other), and batches
    // with a GPU list row, whose packet (if any) goes in only when the row is
    // not live. The main thread meanwhile builds the rigged and alpha types'
    // packets in their order (palettes, table rows, texture calls in the same
    // order as before), then takes the list lock and puts everything together
    // in the original order: rowLive per candidate row, the merge at every
    // joint. A packet merged into the one before it ends where the later one
    // did, with the same buffers and matrix, so later merge decisions do not
    // change: the same packets in the same order as one at a time.
    enum : U8 { RHI_CAND_PACKET = 0, RHI_CAND_ROW = 1, RHI_CAND_MAIN = 2 };
    struct RHIPacketCand
    {
        LLRHI::ShadowPacket p;
        U64 row = 0;                    // ROW, MAIN: the batch's GPU list row (0: none)
        LLDrawInfo* info = nullptr;     // MAIN: the batch, built on the main thread (rhi_add_packet)
        U8 kind = RHI_CAND_PACKET;
        bool has_packet = false;        // ROW: p goes in when the row is not live
    };
    struct RHIPacketChunk
    {
        LLDrawInfo** begin = nullptr;   // a run of one render map
        LLDrawInfo** end = nullptr;
        U32 segment = 0;                // its type's index in drawNativeShadowCasters' opaque types
        std::vector<RHIPacketCand> out;
        U32 rows = 0;                   // ROW and MAIN candidates in out
        U64 pushes = 0, merges = 0;     // its PACKET candidates, counted as rhi_push_packet would
        bool failed = false;            // fix-round7: its build threw; the main thread builds it again
    };
    constexpr U32 RHI_PACKET_CHUNKS = 64;
    constexpr U64 RHI_PACKET_MIN_INFOS = 256;       // fewer opaque batches in a pass: built inline
    constexpr U64 RHI_PACKET_CHUNK_INFOS = 64;      // batches per chunk, at least
    constexpr U64 RHI_PACKET_TARGET_CHUNKS = 48;    // with 13 types' remainders, under RHI_PACKET_CHUNKS
    struct RHIPacketBuild
    {
        RHIPacketChunk chunks[RHI_PACKET_CHUNKS];
        // Written by the main thread before next publishes their generation;
        // read by whoever claims one of its chunks.
        bool merge = true;
        bool list = false;
        U32 generation = 0;             // main thread
        // (generation << 32) | (chunk count << 16) | next chunk to hand out.
        std::atomic<U64> next{ 0 };
        std::atomic<U32> remaining{ 0 };    // chunks of the generation not built yet
        std::mutex mutex;
        std::condition_variable done;
    };
    RHIPacketBuild& rhi_packet_build()
    {
        // Never destroyed: a late task may still look at it (and find nothing).
        static RHIPacketBuild* build = new RHIPacketBuild();
        return *build;
    }

    // A chunk of this generation not handed out yet.
    bool rhi_packet_claim(RHIPacketBuild& b, U32 generation, U32& index)
    {
        U64 ticket = b.next.load(std::memory_order_acquire);
        for (;;)
        {
            if ((U32)(ticket >> 32) != generation || (U32)(ticket & 0xffff) >= (U32)((ticket >> 16) & 0xffff))
            {
                return false;
            }
            if (b.next.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                index = (U32)(ticket & 0xffff);
                return true;
            }
        }
    }

    // Any thread: reads the batches and their vertex buffers only (no viewer
    // call, no reference count, no lock).
    void rhi_build_chunk(RHIPacketChunk& chunk, bool merge, bool list)
    {
        chunk.failed = false;
        chunk.out.clear();
        chunk.rows = 0;
        chunk.pushes = 0;
        chunk.merges = 0;
        bool extend = false;    // out.back() is a PACKET candidate the next packet may continue
        LLDrawInfo** const end = chunk.end;
        for (LLDrawInfo** i = chunk.begin; i != end; )
        {
            LLDrawInfo* info = *i;
            LLCullResult::increment_iterator(i, end);
            if (!info)
            {
                continue;
            }
            const U64 row = list ? info->mRHIShadowRow : 0;
            if (info->mAvatar.notNull())
            {
                // Not expected in a non-rigged type; its palette is the main thread's.
                RHIPacketCand& c = chunk.out.emplace_back();
                c.kind = RHI_CAND_MAIN;
                c.info = info;
                c.row = row;
                ++chunk.rows;
                extend = false;
                continue;
            }
            const LLVertexBuffer* vb = info->mVertexBuffer.get();
            const bool has_packet = vb && info->mCount && vb->getGLBuffer() && vb->getGLIndices();
            if (row)
            {
                RHIPacketCand& c = chunk.out.emplace_back();
                c.kind = RHI_CAND_ROW;
                c.row = row;
                c.has_packet = has_packet;
                if (has_packet)
                {
                    rhi_packet_fields(c.p, info, vb, LLRHI::ShadowPacket::MODE_OPAQUE, 0.f);
                }
                ++chunk.rows;
                extend = false;
                continue;
            }
            if (!has_packet)
            {
                continue;   // as rhi_add_packet: nothing pushed, the packet before stays the last
            }
            LLRHI::ShadowPacket p;
            rhi_packet_fields(p, info, vb, LLRHI::ShadowPacket::MODE_OPAQUE, 0.f);
            ++chunk.pushes;
            if (extend && merge && rhi_continues(chunk.out.back().p, p))
            {
                chunk.out.back().p.index_count += p.index_count;
                ++chunk.merges;
                continue;
            }
            RHIPacketCand& c = chunk.out.emplace_back();
            c.kind = RHI_CAND_PACKET;
            c.p = p;
            extend = true;
        }
    }

    // fix-round7: a chunk of a generation, on any thread. One that throws
    // (bad_alloc, ...) is marked and counts as done like the others, so the
    // main thread never waits for it; it builds the marked ones again after the
    // wait, when no thread touches the chunks (an exception there is the main
    // thread's own, as with the inline build).
    void rhi_try_build_chunk(RHIPacketChunk& chunk, bool merge, bool list)
    {
        try
        {
            rhi_build_chunk(chunk, merge, list);
        }
        catch (...)
        {
            chunk.failed = true;
            static std::atomic<U32> sFailures{ 0 };
            if (sFailures.fetch_add(1, std::memory_order_relaxed) < 5)
            {
                try
                {
                    LL_WARNS("RHIProfile") << "a shadow packet chunk failed: built again on the main thread after the others" << LL_ENDL;
                }
                catch (...)
                {
                }
            }
        }
    }

    // Builds chunks of the generation in arg until none is left (and wakes the
    // main thread after the last). Posted to the cull threads; a task that runs
    // after its generation is over finds nothing to claim.
    void rhi_packet_task(void* arg)
    {
        RHIPacketBuild& b = rhi_packet_build();
        const U32 generation = (U32)(uintptr_t)arg;
        U32 index = 0;
        U32 built = 0;
        std::chrono::steady_clock::time_point start;
        while (rhi_packet_claim(b, generation, index))
        {
            if (!built)
            {
                start = std::chrono::steady_clock::now();
            }
            rhi_try_build_chunk(b.chunks[index], b.merge, b.list);
            ++built;
            if (b.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                {
                    std::lock_guard<std::mutex> lock(b.mutex);     // the main thread is waiting or has not looked yet
                }
                b.done.notify_all();
            }
        }
        if (built)
        {
            sRHIPacketWorkerNs.fetch_add(
                (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count(),
                std::memory_order_relaxed);
            sRHIPacketWorkerChunks.fetch_add(built, std::memory_order_relaxed);
        }
    }
}

bool LLPipeline::drawNativeShadowCasters(const glm::mat4& view, const glm::mat4& proj, bool terrain, LLCamera& camera)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    using Packet = LLRHI::ShadowPacket;
    // LLRHI (research/66 step 2): while a cull thread may record the pass, the
    // packets stay until the join, so each pass gets its own array; the join
    // frees them all (set 0 is the one array outside such a window).
    static std::vector<Packet> packet_sets[RHI_PACKET_SETS];
    if (LLRHI::sShadowRecordBin < 0 ? sRHIPacketSetsUsed > 0 : sRHIPacketSetsUsed >= RHI_PACKET_SETS)
    {
        rhi_join_shadow_recording();
    }
    std::vector<Packet>& packets = packet_sets[LLRHI::sShadowRecordBin >= 0 ? sRHIPacketSetsUsed++ : 0];
    packets.clear();

    // LLRHI: GPU shadow list (research/46 G2): a batch with a live row is a bit
    // in the pass's accept set instead of a packet.
    // Locked only around the opaque and terrain loops: those make no texture
    // calls (texture threads may hold their own locks while they wait for this one).
    LLRHI::ShadowList* list = sRHIListOn ? LLRHI::sShadowList : nullptr;
    static std::vector<U32> accept;
    bool accepted = false;
    if (list)
    {
        accept.assign(((size_t)list->rowSpan() + 31) / 32, 0u);
    }
    auto accept_row = [&](U64 row)
    {
        const U32 slot = (U32)row - 1;
        if (accept.size() <= slot / 32)
        {
            accept.resize(slot / 32 + 1, 0u);
        }
        accept[slot / 32] |= 1u << (slot & 31);
        accepted = true;
    };
    // Visits a render map like LLRenderPass::pushBatches does.
    auto each = [this](U32 type, auto&& visit)
    {
        auto* end = endRenderMap(type);
        for (LLCullResult::drawinfo_iterator i = beginRenderMap(type); i != end; )
        {
            LLDrawInfo* info = *i;
            LLCullResult::increment_iterator(i, end);
            if (info)
            {
                visit(info);
            }
        }
    };

    // shadowV: the prim volume is the shadow, textures ignored.
    static const U32 opaque[] = {
        LLRenderPass::PASS_SIMPLE, LLRenderPass::PASS_FULLBRIGHT, LLRenderPass::PASS_SHINY, LLRenderPass::PASS_BUMP,
        LLRenderPass::PASS_FULLBRIGHT_SHINY, LLRenderPass::PASS_MATERIAL, LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        LLRenderPass::PASS_SPECMAP, LLRenderPass::PASS_SPECMAP_EMISSIVE, LLRenderPass::PASS_NORMMAP,
        LLRenderPass::PASS_NORMMAP_EMISSIVE, LLRenderPass::PASS_NORMSPEC, LLRenderPass::PASS_NORMSPEC_EMISSIVE };
    constexpr U32 OPAQUE_TYPES = (U32)(sizeof(opaque) / sizeof(opaque[0]));
    const bool rigged = LLRHI::sNativeRiggedShadows;  // rigged maps are type + 1
    const auto build_start = std::chrono::steady_clock::now();
    ++sRHIPacketStats.passes;

    // The opaque rigged map after its type (palettes: main thread only).
    auto rigged_opaque = [&](U32 type, std::vector<Packet>& out)
    {
        each(type + 1, [&](LLDrawInfo* info)
        {
            rhi_add_packet(out, info, Packet::MODE_OPAQUE, 0.f, false);
        });
    };
    // The alpha types, after all the opaque ones (texture names: main thread only).
    auto alpha_types = [&](std::vector<Packet>& out)
    {
        // shadowAlphaMask: alpha masked (texture batches), alpha blended (dithered).
        for (U32 type = LLRenderPass::PASS_ALPHA_MASK; type <= (U32)LLRenderPass::PASS_ALPHA_MASK + (rigged ? 1u : 0u); ++type)
        {
            each(type, [&](LLDrawInfo* info)
            {
                rhi_add_packet(out, info, Packet::MODE_ALPHA_MASK, info->mAlphaMaskCutoff, true);
            });
        }
        each(LLRenderPass::PASS_ALPHA, [&](LLDrawInfo* info)
        {
            if ((info->mAvatar.isNull() || rigged) && !info->mGLTFMaterial)
            {
                rhi_add_packet(out, info, Packet::MODE_ALPHA_MASK, ALPHA_BLEND_CUTOFF, true);
            }
        });
        for (U32 type = LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK; type <= (U32)LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK + (rigged ? 1u : 0u); ++type)
        {
            each(type, [&](LLDrawInfo* info)
            {
                rhi_add_packet(out, info, Packet::MODE_ALPHA_MASK_FULLBRIGHT, info->mAlphaMaskCutoff, true);
            });
        }
        // treeShadow: grass, and material alpha masks.
        each(LLRenderPass::PASS_GRASS, [&](LLDrawInfo* info)
        {
            rhi_add_packet(out, info, Packet::MODE_ALPHA_TEST, ALPHA_BLEND_CUTOFF, false);
        });
        static const U32 material_masks[] = {
            LLRenderPass::PASS_NORMSPEC_MASK, LLRenderPass::PASS_MATERIAL_ALPHA_MASK, LLRenderPass::PASS_SPECMAP_MASK, LLRenderPass::PASS_NORMMAP_MASK };
        for (U32 type : material_masks)
        {
            for (U32 variant = type; variant <= type + (rigged ? 1u : 0u); ++variant)
            {
                each(variant, [&](LLDrawInfo* info)
                {
                    rhi_add_packet(out, info, Packet::MODE_ALPHA_TEST, info->mAlphaMaskCutoff, false);
                });
            }
        }
    };

    // LLRHI (research/66 step 2B): the opaque, non-rigged types' batches in
    // chunks on the cull threads (rhi_build_chunk) while this thread builds the
    // rigged and alpha types' packets; then all of it in the original order.
    RHIPacketBuild* build = nullptr;
    U32 generation = 0;
    U32 chunk_count = 0;
    if (LLRHI::sShadowPacketThreads && LLRHI::sCullThreads && CullWorkers::instance().threads())
    {
        U64 total = 0;
        for (U32 type : opaque)
        {
            total += (U64)(endRenderMap(type) - beginRenderMap(type));
        }
        if (total >= RHI_PACKET_MIN_INFOS)
        {
            RHIPacketBuild& b = rhi_packet_build();
            const U64 per = llmax(RHI_PACKET_CHUNK_INFOS, (total + RHI_PACKET_TARGET_CHUNKS - 1) / RHI_PACKET_TARGET_CHUNKS);
            bool fits = true;
            for (U32 s = 0; s < OPAQUE_TYPES && fits; ++s)
            {
                LLDrawInfo** begin = beginRenderMap(opaque[s]);
                LLDrawInfo** const end = endRenderMap(opaque[s]);
                while (begin != end)
                {
                    if (chunk_count == RHI_PACKET_CHUNKS)
                    {
                        fits = false;   // cannot happen (at most 48 + 13 chunks); inline then
                        break;
                    }
                    const U64 n = llmin((U64)(end - begin), per);
                    RHIPacketChunk& chunk = b.chunks[chunk_count++];
                    chunk.begin = begin;
                    chunk.end = begin + n;
                    chunk.segment = s;
                    begin += n;
                }
            }
            if (fits && chunk_count > 1)
            {
                b.merge = LLRHI::sMergeShadowPackets;
                b.list = list != nullptr;
                b.remaining.store(chunk_count, std::memory_order_relaxed);
                generation = ++b.generation;
                b.next.store(((U64)generation << 32) | ((U64)chunk_count << 16), std::memory_order_release);
                const U32 tasks = llmin(CullWorkers::instance().threads(), chunk_count - 1);
                for (U32 t = 0; t < tasks; ++t)
                {
                    CullWorkers::instance().post(&rhi_packet_task, (void*)(uintptr_t)generation);
                }
                build = &b;
                ++sRHIPacketStats.threaded;
                sRHIPacketStats.chunks += chunk_count;
                sRHIPacketStats.infos += total;
            }
            else
            {
                chunk_count = 0;
            }
        }
    }
    if (build)
    {
        RHIPacketBuild& b = *build;
        // fix-round7: an exception leaving this block (bad_alloc on this thread
        // below) first hands out the chunks left and waits for the cull threads,
        // so none builds into the next call's generation. Nothing to do otherwise.
        struct JoinOnThrow
        {
            RHIPacketBuild& b;
            U32 generation;
            ~JoinOnThrow()
            {
                U32 index = 0;
                while (rhi_packet_claim(b, generation, index))
                {
                    b.remaining.fetch_sub(1, std::memory_order_acq_rel);
                }
                if (b.remaining.load(std::memory_order_acquire))
                {
                    std::unique_lock<std::mutex> lock(b.mutex);
                    b.done.wait(lock, [this] { return b.remaining.load(std::memory_order_acquire) == 0; });
                }
            }
        } join_on_throw{ b, generation };
        // Meanwhile: the rigged and alpha types, in their order.
        static std::vector<Packet> rigged_packets[OPAQUE_TYPES];
        static std::vector<Packet> alpha_packets;
        const auto impure_start = std::chrono::steady_clock::now();
        for (U32 s = 0; s < OPAQUE_TYPES; ++s)
        {
            rigged_packets[s].clear();
            if (rigged)
            {
                rigged_opaque(opaque[s], rigged_packets[s]);
            }
        }
        alpha_packets.clear();
        alpha_types(alpha_packets);
        // Then the chunks no cull thread has taken, then wait for the rest.
        const auto chunks_start = std::chrono::steady_clock::now();
        U32 index = 0;
        while (rhi_packet_claim(b, generation, index))
        {
            rhi_try_build_chunk(b.chunks[index], b.merge, b.list);   // fix-round7: marked, built again after the wait
            ++sRHIPacketStats.main_chunks;
            b.remaining.fetch_sub(1, std::memory_order_acq_rel);
        }
        const auto wait_start = std::chrono::steady_clock::now();
        if (b.remaining.load(std::memory_order_acquire))
        {
            std::unique_lock<std::mutex> lock(b.mutex);
            b.done.wait(lock, [&b] { return b.remaining.load(std::memory_order_acquire) == 0; });
        }
        // fix-round7: the chunks whose build threw, again here (the generation is over)
        for (U32 c = 0; c < chunk_count; ++c)
        {
            if (b.chunks[c].failed)
            {
                rhi_build_chunk(b.chunks[c], b.merge, b.list);
            }
        }
        const auto fold_start = std::chrono::steady_clock::now();
        {
            // In the original order: per opaque type its chunks, then its rigged
            // map; then the alpha types. Rows decided and packets merged here.
            RHIListLock list_lock(list);
            const bool merge = b.merge;
            size_t room = packets.size() + alpha_packets.size();
            for (U32 c = 0; c < chunk_count; ++c)
            {
                room += b.chunks[c].out.size();
            }
            for (const std::vector<Packet>& side : rigged_packets)
            {
                room += side.size();
            }
            packets.reserve(room);
            // A packet already counted; merged into the last when it continues it.
            auto append = [&](const Packet& p)
            {
                if (merge && !packets.empty() && rhi_continues(packets.back(), p))
                {
                    packets.back().index_count += p.index_count;
                    ++sNativeMerged;
                    return;
                }
                packets.push_back(p);
            };
            for (U32 c = 0, s = 0; s < OPAQUE_TYPES; ++s)
            {
                for (; c < chunk_count && b.chunks[c].segment == s; ++c)
                {
                    const RHIPacketChunk& chunk = b.chunks[c];
                    sNativePackets += chunk.pushes;
                    sNativeMerged += chunk.merges;
                    if (!chunk.rows)
                    {
                        // Packets only: just the first may continue the last
                        // (no later one continued the one before it).
                        for (size_t k = 0; k < chunk.out.size(); ++k)
                        {
                            if (k)
                            {
                                packets.push_back(chunk.out[k].p);
                            }
                            else
                            {
                                append(chunk.out[k].p);
                            }
                        }
                        continue;
                    }
                    for (const RHIPacketCand& cand : chunk.out)
                    {
                        if (cand.kind == RHI_CAND_PACKET)
                        {
                            append(cand.p);
                        }
                        else if (list && cand.row && list->rowLive(cand.row))
                        {
                            accept_row(cand.row);
                        }
                        else if (cand.kind == RHI_CAND_MAIN)
                        {
                            rhi_add_packet(packets, cand.info, Packet::MODE_OPAQUE, 0.f, false);
                            ++sRHIPacketStats.fallbacks;
                        }
                        else if (cand.has_packet)
                        {
                            ++sNativePackets;
                            append(cand.p);
                        }
                    }
                }
                // Counted when built; only the first may continue what is before.
                const std::vector<Packet>& side = rigged_packets[s];
                if (!side.empty())
                {
                    append(side[0]);
                    packets.insert(packets.end(), side.begin() + 1, side.end());
                }
            }
            if (!alpha_packets.empty())
            {
                append(alpha_packets[0]);
                packets.insert(packets.end(), alpha_packets.begin() + 1, alpha_packets.end());
            }
        }
        const auto fold_end = std::chrono::steady_clock::now();
        auto ns = [](std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
        {
            return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count();
        };
        sRHIPacketStats.impure_ns += ns(impure_start, chunks_start);
        sRHIPacketStats.main_chunk_ns += ns(chunks_start, wait_start);
        sRHIPacketStats.wait_ns += ns(wait_start, fold_start);
        sRHIPacketStats.fold_ns += ns(fold_start, fold_end);
    }
    else
    {
        for (U32 type : opaque)
        {
            {
                // Not rigged (those are type + 1): no palette or texture work in rhi_add_packet.
                RHIListLock list_lock(list);
                each(type, [&](LLDrawInfo* info)
                {
                    if (list && info->mRHIShadowRow && list->rowLive(info->mRHIShadowRow))
                    {
                        accept_row(info->mRHIShadowRow);
                        return;
                    }
                    rhi_add_packet(packets, info, Packet::MODE_OPAQUE, 0.f, false);
                });
            }
            if (rigged)
            {
                rigged_opaque(type, packets);
            }
        }
        alpha_types(packets);
    }

    // <plus> research/206 (stage 5): the casters this pass's layer left to the retained scene (a
    // cascade's moving casters or, 5b, its cached depth's static casters; a spot pass's): its view
    // chosen and its records made ready for this pass (drawn first, depth only). If the scene
    // cannot, they go into this pass as packets; and they are verified in turn.
    if (gSceneShadow.layerActive())
    {
        static std::vector<std::pair<U32, LLDrawInfo*> > scene_batches;
        scene_batches.clear();
        const glm::mat4 scene_view_proj = proj * view;
        const bool scene_drawn = gSceneShadow.drawLayer(glm::value_ptr(scene_view_proj), camera);
        const bool scene_rescue = gSceneShadow.layerCut() && !scene_drawn;
        const bool scene_verify = gSceneShadow.verifyDue();
        if (scene_rescue || scene_verify)
        {
            scene_layer_visit([](U32 type, LLDrawInfo* info) { scene_batches.emplace_back(type, info); });
        }
        if (scene_verify)
        {
            gSceneShadow.verifyLayer(scene_batches);
        }
        if (scene_rescue)
        {
            for (const std::pair<U32, LLDrawInfo*>& batch : scene_batches)
            {
                // 5c: a mask as alpha_types() makes its packet (PASS_ALPHA_MASK's and the fullbright one's with
                // the texture list, the legacy materials' without)
                const U32 mode = LLSceneShadow::casterMode(batch.first);
                const bool masked = mode != Packet::MODE_OPAQUE;
                rhi_add_packet(packets, batch.second, mode, masked ? batch.second->mAlphaMaskCutoff : 0.f, mode != Packet::MODE_ALPHA_TEST && masked);
            }
            gSceneShadow.rescued((U32)scene_batches.size(), false);
        }
    }
    // </plus>

    // LLDrawPoolTerrain::renderShadow: shadowV over the terrain pools' faces.
    if (terrain)
    {
        RHIListLock list_lock(list);
        for (const auto& entry : mTerrainPools)
        {
            LLFacePool* pool = static_cast<LLFacePool*>(entry.second);
            for (LLFace* face : pool->mDrawFace)
            {
                const U64 row = list ? rhiListTerrainRow(face) : 0;
                if (row)
                {
                    accept_row(row);
                    continue;
                }
                rhi_add_terrain_packet(packets, face);
            }
        }
    }

    // LLRHI native (research/137 N10): LLDrawPoolTree::renderShadow's faces (treeShadowF).
    for (const Packet& tree : plus::shadowrest::treePackets(mTreePools))
    {
        rhi_push_packet(packets, tree);
    }

    const glm::mat4 view_proj = proj * view;
    LLRHI::ShadowListPass pass;
    const bool gpu_groups = mRHIListPass.volume || (mRHIListPass.terrain && terrain);
    const bool use_list = list && (accepted || gpu_groups);
    if (use_list)
    {
        pass.accept = accept.data();
        pass.accept_words = (U32)accept.size();
        pass.volume = mRHIListPass.volume;
        pass.terrain = mRHIListPass.terrain;
        pass.pools = terrain;
        pass.since_min = mRHIListPass.since_min;
        pass.since_max = mRHIListPass.since_max;
        // The planes and masks LLCamera::AABBInFrustum tests (the shadow cull's).
        const U32 plane_count = llmin(camera.getAgentPlaneCount(), (U32)LLCamera::AGENT_PLANE_USER_CLIP_NUM);
        for (U32 i = 0; i < 8; ++i)
        {
            pass.masks[i] = 0xff;
            if (i < plane_count)
            {
                const LLPlane& p = camera.getAgentPlane(i);
                for (U32 j = 0; j < 4; ++j)
                {
                    pass.planes[i][j] = p[j];
                }
                pass.masks[i] = camera.getAgentPlaneMask(i);
            }
        }
        const U32 sides[4] = { LLCamera::AGENT_PLANE_LEFT, LLCamera::AGENT_PLANE_RIGHT, LLCamera::AGENT_PLANE_BOTTOM,
                               LLCamera::AGENT_PLANE_TOP };
        for (U32 i = 0; i < 4; ++i)
        {
            const LLPlane& p = camera.getAgentPlane(sides[i]);
            for (U32 j = 0; j < 4; ++j)
            {
                pass.sides[i][j] = p[j];
            }
        }
        const LLVector3& origin = camera.getOrigin();
        for (U32 j = 0; j < 3; ++j)
        {
            pass.origin[j] = origin.mV[j];
        }
        if (accepted)
        {
            sRHIListAccepted += 1;
        }
    }
    sRHIPacketStats.build_ns += (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - build_start).count();
    const F32 plus_dither_width = (F32)plus::shadowshift::ditherWidth(LLRenderTarget::sCurResX); // <plus> plus/shadowshift: the dither on the grid </plus>
    const bool drawn = LLRHI::sDrawShadowPackets(packets.data(), packets.size(), glm::value_ptr(view_proj), plus_dither_width, // <plus> (was: (F32)LLRenderTarget::sCurResX) </plus>
                                                 use_list ? &pass : nullptr);
    if (list && list->broken())
    {
        rhiListFailed();
    }
    return drawn;
}

// ----------------------------------------------------------------------------
// LLRHI: GPU probe list (research/46 G5, research/50 step 4).
//
// A reflection probe face draws the batches of a render type (PASS_SIMPLE,
// Deferred Diffuse, always; with LLRHI::sGPUDrivenDGC every type
// rhiProbeTypeIndex lists: the opaque G-buffer passes whose programs are on
// the shared layout) of its static volume groups from a GPU list instead of
// sorting and recording each one:
//  - A static group's batches of a type are registered as one block once its
//    draw map has been unchanged for a second (rhi_probe_usable): their
//    buffers copied into G4b's attribute pool, their region space bounds, the
//    GL names of their textures. PASS_SIMPLE's are read off the batches
//    (G5); the other types' are captured at the type's pool loop: its batches
//    are drawn the way the loop draws them while the backend records nothing
//    and keeps what each draw would push (its program, textures, material
//    row; LLRHI::ProbeList::beginCapture).
//  - postSort (rhiProbeFace, before the render map is built): the face's
//    visible groups in their order. Each static group with batches of a type
//    that has a live block whose textures are still the ones registered is
//    listed for that type: left out of the type's render map (mRHIProbeStamp,
//    mRHIProbeTypes), its textures marked used (setActive, as directTexture
//    would; the whole block's, so never fewer), and the render map position
//    its batches would have had recorded on the way (rhiProbeAt). The other
//    groups stay in the render map (research/51: one group the list cannot
//    take no longer keeps the whole face on the CPU).
//  - The type's pool loop (rhiProbeDraw) draws the render map with the listed
//    batches in their places, every batch by the loop's own body: the first
//    batch of each run of the list (G5: one pool chunk and vertex format; with
//    generated commands one vertex format and render state, any chunks) the
//    usual way, which opens a G4b draw run with the pass's bindings; the list
//    then writes the commands and rows of all the face's listed batches of
//    the type on the GPU (one dispatch, research/44 P1's test per batch) and
//    each run's other batches go out as one indirect draw (G5) or one
//    vkCmdExecuteGeneratedCommandsEXT (research/50 step 4) after its first.
//    Whatever cannot go that way is drawn on the CPU as before, in the same
//    order, with P1's test: the same draws in the same order either way.

namespace
{
    constexpr F32 RHI_PROBE_SETTLE_SECONDS = 1.f;      // draw map unchanged this long before a block is made
    constexpr F32 RHI_PROBE_RETRY_SECONDS = 1.f;       // after a refused block
    constexpr F32 RHI_PROBE_UNUSED_SECONDS = 30.f;     // a block no probe face used for this long gives its pool copies back
    constexpr U32 RHI_PROBE_REGISTER_PER_FRAME = 64;   // blocks made a frame

    // The render types the list may draw, by their index (rhiProbeTypeIndex).
    // Index 0 (PASS_SIMPLE) is G5's; the others need generated commands.
    const U32 RHI_PROBE_TYPE_LIST[] = {
        LLRenderPass::PASS_SIMPLE,
        LLRenderPass::PASS_ALPHA_MASK,
        LLRenderPass::PASS_MATERIAL,
        LLRenderPass::PASS_MATERIAL_ALPHA_MASK,
        LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE,
        LLRenderPass::PASS_SPECMAP,
        LLRenderPass::PASS_SPECMAP_MASK,
        LLRenderPass::PASS_SPECMAP_EMISSIVE,
        LLRenderPass::PASS_NORMMAP,
        LLRenderPass::PASS_NORMMAP_MASK,
        LLRenderPass::PASS_NORMMAP_EMISSIVE,
        LLRenderPass::PASS_NORMSPEC,
        LLRenderPass::PASS_NORMSPEC_MASK,
        LLRenderPass::PASS_NORMSPEC_EMISSIVE,
        LLRenderPass::PASS_GLTF_PBR,
        LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK,
    };
    constexpr U32 RHI_PROBE_TYPES = sizeof(RHI_PROBE_TYPE_LIST) / sizeof(RHI_PROBE_TYPE_LIST[0]);
    static_assert(RHI_PROBE_TYPES <= LLPipeline::RHI_PROBE_MAX_TYPES, "a bit and a counter per type");

    struct RHIProbeRun
    {
        U32 first = 0;      // batch index in the block
        U32 count = 0;
        U32 chunk = 0;
        U64 format = 0;
        U32 state = 0;      // captured blocks: the batches' render state (0 for PASS_SIMPLE's)
    };

    // A group's block of one render type.
    struct RHIProbeTypeInfo
    {
        U32 block = 0;                          // LLRHI::ProbeList block, 0: none
        U32 builds = 0;                         // rhiBuilds() the block was made for
        const LLMatrix4* model = nullptr;       // the region's render matrix it was made with
        std::vector<LLDrawInfo*> batches;       // its batches that draw, in draw map order
        std::vector<LLPipeline::RHIProbeTexture> textures;     // distinct textures they bind, with their GL names
        std::vector<RHIProbeRun> runs;
        U32 seen_builds = U32_MAX;              // rhiBuilds() last seen without a block, since seen_at
        F32 seen_at = 0.f;
        F32 retry_at = 0.f;
        U32 foreign_builds = U32_MAX;           // rhiBuilds() of a draw map the list cannot take
        F32 used_at = 0.f;
        U32 checked_frame = U32_MAX;            // the frame its block was last checked (textures, liveness)
        U32 face_index = U32_MAX;               // its place in the face's groups of the type when listed
        bool valid = false;
        bool pending = false;                   // queued for the face's capture at the type's loop
    };

    struct RHIProbeGroupInfo
    {
        LLSpatialGroup* group = nullptr;
        std::unique_ptr<RHIProbeTypeInfo> types[RHI_PROBE_TYPES];   // made on first use
    };

    std::vector<RHIProbeGroupInfo>& rhi_probe_groups()
    {
        static std::vector<RHIProbeGroupInfo>* groups = new std::vector<RHIProbeGroupInfo>();
        return *groups;
    }
    std::vector<U32> sRHIProbeFree;             // free info slots

    // The face whose postSort handed its static batches to the list.
    struct RHIProbeFaceState
    {
        bool active = false;
        LLCullResult* cull = nullptr;
        U32 serial = 0;
        bool generated = false;                 // drawn with device generated commands (research/50 step 4)
        ProbeFaceView view;
        U32 types = 0;                          // bits of the types with listed groups
        std::vector<U32> groups[RHI_PROBE_TYPES];       // info slots, in the render map's order
        std::vector<U32> positions[RHI_PROBE_TYPES];    // per group: the type's render map batches before it (U32_MAX: not reached)
        std::vector<U32> pending[RHI_PROBE_TYPES];      // info slots whose blocks the type's loop captures

        void reset()
        {
            active = false;
            cull = nullptr;
            serial = 0;
            generated = false;
            types = 0;
            for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
            {
                groups[t].clear();
                positions[t].clear();
                pending[t].clear();
            }
        }
    };
    RHIProbeFaceState sRHIProbeFace;
    bool sRHIProbeOn = false;
    bool sRHIProbeGenerated = false;
    U32 sRHIProbeRegisteredFrame = U32_MAX;
    U32 sRHIProbeRegistered = 0;
    F32 sRHIProbeSweptAt = 0.f;

    // Why a static group's batches of a type stayed in the render map of a face.
    enum EProbeLeft
    {
        LEFT_DIRTY,         // waiting for a rebuild (or no region, auto-hidden)
        LEFT_NEVER,         // a batch the list cannot take, until the draw map changes (refusals tell which)
        LEFT_SETTLING,      // draw map first seen or changed less than RHI_PROBE_SETTLE_SECONDS ago
        LEFT_RETRY,         // refused less than RHI_PROBE_RETRY_SECONDS ago
        LEFT_BUDGET,        // RHI_PROBE_REGISTER_PER_FRAME blocks tried this frame
        LEFT_REFUSED,       // refused now (refusals tell why)
        LEFT_GONE,          // its block died (GL wrote a buffer) or a texture's GL name changed
        LEFT_CAPTURE,       // queued for the capture at the type's loop (its block is made there)
        LEFT_REASONS
    };
    const char* const sProbeLeftNames[LEFT_REASONS] = { "rebuilding", "never", "settling", "retry wait", "frame budget", "refused",
                                                        "block gone or texture renamed", "being captured" };

    // Why a block was not made.
    enum EProbeRefusal
    {
        REFUSE_NO_BUFFER,
        REFUSE_RIGGED,
        REFUSE_NO_BOUNDS,       // no region space bounds
        REFUSE_MATRIX,          // not the region's matrix
        REFUSE_TEXTURE_MATRIX,  // texture animation
        REFUSE_UNITS,           // more textures than a row names
        REFUSE_NULL_TEXTURE,    // a batched unit without a texture (the draw keeps the unit's binding)
        REFUSE_ATTRIBUTES,      // the buffer has none of the program's attributes
        REFUSE_MAPPED,          // the buffer is mapped or has no GL buffers yet
        REFUSE_TEXTURE_NAME,    // a texture has no GL name yet, or its options are dirty
        REFUSE_TEXTURES,        // a captured type's textures the list cannot follow (media, null maps)
        REFUSE_CAPTURE,         // a batch drawn for the capture did not leave one direct draw
        REFUSE_BACKEND,         // the list did not take it (backend reasons)
        REFUSALS
    };
    const char* const sProbeRefusalNames[REFUSALS] = { "no buffer", "rigged", "no bounds", "not region space", "texture matrix",
                                                       "too many textures", "null texture", "no attributes",
                                                       "buffer mapped or not in GL", "texture not loaded", "textures", "capture",
                                                       "backend" };

    struct RHIProbeStats
    {
        U32 faces = 0, listed = 0, whole = 0, mixed = 0, groups = 0, gpu_draws = 0, cpu_batches = 0, indirect = 0, made = 0, refused = 0;
        U32 types_listed = 0, captured = 0;
        U32 left[LEFT_REASONS] = {};            // static groups left in the render map, summed over faces and types
        U32 refusals[REFUSALS] = {};
        std::map<std::string, U32> backend;     // REFUSE_BACKEND by the list's reason
        // <plus> research/209 E1: by type (RHI_PROBE_TYPE_LIST's index), for G5's retirement (209 3.1 C2, C4)
        U32 type_groups[RHI_PROBE_TYPES] = {};  // groups listed
        U32 type_gpu[RHI_PROBE_TYPES] = {};     // batches drawn by the list (indirect draws or executes)
        U32 type_cpu[RHI_PROBE_TYPES] = {};     // listed batches drawn on the CPU (each run's first, the face's outside, fallbacks)
        U32 type_scene[RHI_PROBE_TYPES] = {};   // static groups left to the retained scene, not made candidates (209 S2)
        // </plus>
        F32 logged_at = 0.f;
    };
    RHIProbeStats sRHIProbeStats;

    // <plus> research/209 E1: the per-type lines (RHIProbe) and the probe census.
    const char* const RHI_PROBE_TYPE_NAMES[] = {
        "simple", "alpha mask", "material", "material mask", "material emissive", "specmap", "specmap mask", "specmap emissive",
        "normmap", "normmap mask", "normmap emissive", "normspec", "normspec mask", "normspec emissive", "gltf", "gltf mask",
    };
    static_assert(sizeof(RHI_PROBE_TYPE_NAMES) / sizeof(RHI_PROBE_TYPE_NAMES[0]) == RHI_PROBE_TYPES, "a name per type");
    // PASS_GLTF_PBR, PASS_GLTF_PBR_ALPHA_MASK: the scene refuses them (REFUSED_GLTF; 209 3.1 C4).
    bool rhi_probe_gltf(U32 t)
    {
        return RHI_PROBE_TYPE_LIST[t] == LLRenderPass::PASS_GLTF_PBR || RHI_PROBE_TYPE_LIST[t] == LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK;
    }

    // The probe census (LLRHI_PROBE_CENSUS=1, off by default): each reflection
    // probe face's and mirror's (LLSceneRecords::currentView PROBE, WATER)
    // batches of the list's types, by who draws them, as postSort will hand
    // them out (before the face's P1 test): the retained scene (its view cuts
    // the type and the group is drawable now), the list (stamped), the render
    // map from a static group, or from a moving one (a bridge's, attachments).
    // "could" counts the batches of the types the scene draws (scopePass) in
    // groups it could draw now (resident and placed): what it takes once it
    // cuts these views (209 E2). A line per view every 10 seconds.
    const bool sRHIProbeCensus = !LLRHI::envUnset("LLRHI_PROBE_CENSUS") && LLRHI::envFirstChar("LLRHI_PROBE_CENSUS") != '0';
    enum ERHIProbeCensusWho
    {
        CENSUS_SCENE,
        CENSUS_LIST,
        CENSUS_LEFT,
        CENSUS_MOVING,
        CENSUS_WHO
    };
    struct RHIProbeCensus
    {
        U32 faces[LLRHI::SCENE_VIEW_COUNT] = {};
        U64 batches[LLRHI::SCENE_VIEW_COUNT][RHI_PROBE_TYPES][CENSUS_WHO] = {};
        U64 could[LLRHI::SCENE_VIEW_COUNT][RHI_PROBE_TYPES] = {};
        F32 logged_at = 0.f;
    };
    RHIProbeCensus sRHIProbeCensusCounts;

    // Runs f when it goes out of scope (rhiProbeFace's returns), when on.
    template <typename F>
    struct RHIProbeOnExit
    {
        bool on;
        F f;
        ~RHIProbeOnExit()
        {
            if (on)
            {
                f();
            }
        }
    };
    template <typename F>
    RHIProbeOnExit<F> rhi_probe_on_exit(bool on, F f)
    {
        return { on, f };
    }
    // </plus>

    template <size_t N>
    void log_probe_reasons(std::ostream& out, const U32 (&counts)[N], const char* const (&names)[N])
    {
        bool any = false;
        for (size_t r = 0; r < N; ++r)
        {
            if (counts[r])
            {
                out << (any ? ", " : " (") << names[r] << " " << counts[r];
                any = true;
            }
        }
        if (any)
        {
            out << ")";
        }
    }

    RHIProbeGroupInfo* rhi_probe_info(LLSpatialGroup* group)
    {
        std::vector<RHIProbeGroupInfo>& infos = rhi_probe_groups();
        const U32 slot = group->mRHIProbeGroup - 1;
        return group->mRHIProbeGroup && slot < infos.size() && infos[slot].group == group ? &infos[slot] : nullptr;
    }

    void rhi_probe_drop_block(RHIProbeTypeInfo& info)
    {
        if (info.block && LLRHI::sProbeList)
        {
            LLRHI::sProbeList->removeBlock(info.block);
        }
        info.block = 0;
        info.batches.clear();
        info.textures.clear();
        info.runs.clear();
        info.checked_frame = U32_MAX;
        info.valid = false;
        info.seen_builds = U32_MAX;
    }

    // All the group's blocks, or (captured_only) those of the captured types.
    void rhi_probe_drop_blocks(RHIProbeGroupInfo& info, bool captured_only)
    {
        for (U32 t = captured_only ? 1 : 0; t < RHI_PROBE_TYPES; ++t)
        {
            if (info.types[t])
            {
                rhi_probe_drop_block(*info.types[t]);
            }
        }
    }

    // A new face (or none): captures queued for the last one that never
    // reached their loop go back (a later face queues them again).
    void rhi_probe_face_reset()
    {
        std::vector<RHIProbeGroupInfo>& infos = rhi_probe_groups();
        for (U32 t = 1; t < RHI_PROBE_TYPES; ++t)
        {
            for (U32 slot : sRHIProbeFace.pending[t])
            {
                if (slot < infos.size() && infos[slot].group && infos[slot].types[t])
                {
                    infos[slot].types[t]->pending = false;
                }
            }
        }
        sRHIProbeFace.reset();
    }

    // A known texture of a block, once: the same texture with the same name, choice and source.
    void rhi_probe_add_texture(std::vector<LLPipeline::RHIProbeTexture>& known, const LLPipeline::RHIProbeTexture& t)
    {
        for (const LLPipeline::RHIProbeTexture& k : known)
        {
            if (k.texture == t.texture && k.name == t.name && k.choice == t.choice && k.chosen == t.chosen && k.source == t.source &&
                k.held == t.held)
            {
                return;
            }
        }
        known.push_back(t);
    }

    // What every listed batch is, whatever its type: a static region batch
    // drawn with the region's matrix and no texture matrix, its buffers in GL.
    // 1: yes; 0: not now; -1: never (until rebuilt). why: the reason when not.
    S32 rhi_probe_batch_common(LLDrawInfo* info, const LLMatrix4& region_matrix, EProbeRefusal& why)
    {
        LLVertexBuffer* vb = info->mVertexBuffer.get();
        why = !vb                                    ? REFUSE_NO_BUFFER
              : info->mAvatar.notNull()              ? REFUSE_RIGGED
              : !info->mHasExtents                   ? REFUSE_NO_BOUNDS
              : info->mModelMatrix != &region_matrix ? REFUSE_MATRIX
              : info->mTextureMatrix                 ? REFUSE_TEXTURE_MATRIX
                                                     : REFUSALS;
        if (why != REFUSALS)
        {
            return -1;
        }
        if (vb->isMapped() || !vb->getGLBuffer() || !vb->getGLIndices())
        {
            why = REFUSE_MAPPED;
            return 0;
        }
        return 1;
    }

    // The batch as the list takes it (as pushDirectBatch would hand it over
    // with Deferred Diffuse bound and batch_textures), with the textures it
    // names. 1: taken; 0: not now (a texture not ready); -1: never (until
    // rebuilt). why: the reason when not taken.
    S32 rhi_probe_batch(LLDrawInfo* info, const LLMatrix4& region_matrix, LLRHI::ProbeList::Batch& batch,
                        LLRHI::DirectDraw::Attribute* attributes, std::vector<LLPipeline::RHIProbeTexture>& textures,
                        EProbeRefusal& why)
    {
        const S32 common = rhi_probe_batch_common(info, region_matrix, why);
        if (common <= 0)
        {
            return common;
        }
        LLVertexBuffer* vb = info->mVertexBuffer.get();
        auto name = [&](LLViewerTexture* tex, U32& out)
        {
            out = LLTexUnit::directName(tex);
            if (out == LLRHI::DirectDraw::KEEP_UNIT)
            {
                return false;
            }
            LLPipeline::RHIProbeTexture t;
            t.texture = tex;
            t.name = out;
            rhi_probe_add_texture(textures, t);
            return true;
        };
        if (info->mTextureList.size() > 1)
        {
            if (info->mTextureList.size() > LLRHI::ProbeList::MAX_UNITS)
            {
                why = REFUSE_UNITS;
                return -1;
            }
            batch.texture_count = (U32)info->mTextureList.size();
            for (U32 i = 0; i < batch.texture_count; ++i)
            {
                if (info->mTextureList[i].isNull())
                {
                    why = REFUSE_NULL_TEXTURE;
                    return -1;      // the draw keeps the unit's binding
                }
                if (!name(info->mTextureList[i].get(), batch.textures[i]))
                {
                    why = REFUSE_TEXTURE_NAME;
                    return 0;
                }
            }
        }
        else
        {
            batch.texture_count = 1;
            batch.textures[0] = 0;  // unbound, as unbindFast leaves it
            if (info->mTexture.notNull() && !name(info->mTexture.get(), batch.textures[0]))
            {
                why = REFUSE_TEXTURE_NAME;
                return 0;
            }
        }
        LLRHI::DirectDraw& d = batch.draw;
        d.attribute_count = vb->getAttributes(gDeferredDiffuseProgram.mAttributeMask, attributes);
        d.attributes = attributes;
        d.vertex_buffer = vb->getGLBuffer();
        d.index_buffer = vb->getGLIndices();
        d.index_type = vb->getIndicesType();
        d.first_index = info->mOffset;
        d.index_count = info->mCount;
        d.buffer_vertices = vb->getNumVerts();
        d.buffer_indices = vb->getNumIndices();
        for (U32 i = 0; i < 3; ++i)
        {
            batch.lo[i] = info->mExtents[0][i];
            batch.hi[i] = info->mExtents[1][i];
        }
        if (!d.attribute_count)
        {
            why = REFUSE_ATTRIBUTES;
            return -1;
        }
        return 1;
    }

    void rhi_probe_set_runs(RHIProbeTypeInfo& info, const LLRHI::ProbeList::Run* runs, U32 run_count)
    {
        info.runs.clear();
        for (U32 i = 0; i < run_count; ++i)
        {
            RHIProbeRun run;
            run.first = runs[i].first;
            run.count = runs[i].count;
            run.chunk = runs[i].chunk;
            run.format = runs[i].format;
            run.state = runs[i].state;
            info.runs.push_back(run);
        }
    }

    // Makes the group's PASS_SIMPLE block (its batches that draw, in order; G5).
    // False: counted in the refusals.
    bool rhi_probe_register(RHIProbeTypeInfo& info, LLSpatialGroup* group, const LLMatrix4& region_matrix)
    {
        LLSpatialGroup::draw_map_t::iterator simple = group->mDrawMap.find(LLRenderPass::PASS_SIMPLE);
        if (simple == group->mDrawMap.end())
        {
            return false;
        }
        static std::vector<LLRHI::ProbeList::Batch> batches;
        static std::vector<LLRHI::DirectDraw::Attribute> attributes;
        static std::vector<LLRHI::ProbeList::Run> runs;
        static std::vector<LLDrawInfo*> infos;
        std::vector<LLPipeline::RHIProbeTexture> textures;
        batches.clear();
        infos.clear();
        U32 count = 0;
        for (LLDrawInfo* batch : simple->second)
        {
            count += batch->mCount ? 1 : 0;
        }
        if (!count)
        {
            return false;
        }
        // Room first: the batches point into it.
        attributes.resize((size_t)count * LLRHI::DirectDraw::MAX_ATTRIBUTES);
        batches.resize(count);
        U32 n = 0;
        for (LLDrawInfo* batch : simple->second)
        {
            if (!batch->mCount)
            {
                continue;   // pushBatch draws nothing
            }
            batches[n] = LLRHI::ProbeList::Batch();
            EProbeRefusal why = REFUSALS;
            const S32 taken = rhi_probe_batch(batch, region_matrix, batches[n], &attributes[(size_t)n * LLRHI::DirectDraw::MAX_ATTRIBUTES],
                                              textures, why);
            if (taken <= 0)
            {
                ++sRHIProbeStats.refusals[why < REFUSALS ? why : REFUSE_BACKEND];
                if (taken < 0)
                {
                    info.foreign_builds = group->rhiBuilds();
                }
                return false;
            }
            infos.push_back(batch);
            ++n;
        }
        runs.resize(count);
        U32 run_count = 0;
        const U32 block = LLRHI::sProbeList->addBlock(batches.data(), count, (const F32*)region_matrix.mMatrix, runs.data(), run_count);
        if (!block)
        {
            ++sRHIProbeStats.refusals[REFUSE_BACKEND];
            ++sRHIProbeStats.backend[LLRHI::sProbeList->refusal()];
            return false;
        }
        info.block = block;
        info.builds = group->rhiBuilds();
        info.model = &region_matrix;
        info.batches = infos;
        info.textures.swap(textures);
        rhi_probe_set_runs(info, runs.data(), run_count);
        info.checked_frame = U32_MAX;
        return true;
    }

    // Research/50 step 4: makes the group's block of a captured type, at the
    // type's pool loop (its program bound, the pass's state set), before the
    // loop draws anything: the loop's body draws each batch while the backend
    // captures instead of recording. False: counted in the refusals.
    bool rhi_probe_register_captured(RHIProbeTypeInfo& info, LLSpatialGroup* group, U32 type, LLPipeline::RHIProbeBody body,
                                     LLPipeline::RHIProbeTextures texture_list, void* context)
    {
        LLSpatialGroup::draw_map_t::iterator it = group->mDrawMap.find(type);
        LLSpatialPartition* part = group->getSpatialPartition();
        if (it == group->mDrawMap.end() || !part || !part->mRegionp || !body || !texture_list)
        {
            return false;
        }
        const LLMatrix4& region_matrix = part->mRegionp->mRenderMatrix;
        static std::vector<LLDrawInfo*> infos;
        static std::vector<LLRHI::ProbeList::Bounds> bounds;
        static std::vector<LLRHI::ProbeList::Run> runs;
        static std::vector<LLPipeline::RHIProbeTexture> found;
        std::vector<LLPipeline::RHIProbeTexture> textures;
        infos.clear();
        bounds.clear();
        for (LLDrawInfo* batch : it->second)
        {
            if (!batch->mCount)
            {
                continue;   // the loop draws nothing
            }
            EProbeRefusal why = REFUSALS;
            S32 taken = rhi_probe_batch_common(batch, region_matrix, why);
            if (taken > 0)
            {
                found.clear();
                taken = texture_list(*batch, context, found);
                why = taken < 0 ? REFUSE_TEXTURES : REFUSE_TEXTURE_NAME;
                for (const LLPipeline::RHIProbeTexture& t : found)
                {
                    rhi_probe_add_texture(textures, t);
                }
            }
            if (taken <= 0)
            {
                ++sRHIProbeStats.refusals[why < REFUSALS ? why : REFUSE_BACKEND];
                if (taken < 0)
                {
                    info.foreign_builds = group->rhiBuilds();
                }
                return false;
            }
            infos.push_back(batch);
            LLRHI::ProbeList::Bounds b;
            for (U32 i = 0; i < 3; ++i)
            {
                b.lo[i] = batch->mExtents[0][i];
                b.hi[i] = batch->mExtents[1][i];
            }
            bounds.push_back(b);
        }
        const U32 count = (U32)infos.size();
        if (!count)
        {
            return false;
        }
        // Each batch drawn as the loop draws it: one captured draw each.
        LLRHI::ProbeList* list = LLRHI::sProbeList;
        gGL.flush();    // pending immediate-mode vertices go out now, not into the capture
        list->beginCapture();
        bool complete = true;
        for (U32 i = 0; i < count && complete; ++i)
        {
            body(*infos[i], context);
            complete = list->captured() == i + 1;
        }
        runs.resize(count);
        U32 run_count = 0;
        const U32 block = complete ? list->addCapturedBlock(bounds.data(), count, (const F32*)region_matrix.mMatrix, runs.data(), run_count)
                                   : 0;
        list->endCapture();
        if (!block)
        {
            if (complete)
            {
                ++sRHIProbeStats.refusals[REFUSE_BACKEND];
                ++sRHIProbeStats.backend[list->refusal()];
            }
            else
            {
                ++sRHIProbeStats.refusals[REFUSE_CAPTURE];
            }
            return false;
        }
        info.block = block;
        info.builds = group->rhiBuilds();
        info.model = &region_matrix;
        info.batches = infos;
        info.textures.swap(textures);
        rhi_probe_set_runs(info, runs.data(), run_count);
        info.checked_frame = U32_MAX;
        return true;
    }

    // The block's textures still go out under the names registered (and a
    // texture picked by its discard level or taken from a material's slot is
    // still the one picked).
    bool rhi_probe_textures_valid(const RHIProbeTypeInfo& info)
    {
        for (const LLPipeline::RHIProbeTexture& t : info.textures)
        {
            // The slot first: a texture it no longer holds may be gone (the
            // entry's texture and choice are then not touched).
            if (t.source && t.source->get() != t.held)
            {
                return false;
            }
            if (t.choice && (t.choice->getDiscardLevel() <= 4) != t.chosen)
            {
                return false;
            }
            if (LLTexUnit::directName(t.texture) != t.name)
            {
                return false;
            }
        }
        return true;
    }

    // The group's info slot when the list draws its batches of type index t
    // as they are now; -1 when it does not (made, checked or queued on the
    // way: captured types are made by their pool loop), with why.
    S32 rhi_probe_usable(LLSpatialGroup* group, U32 t, EProbeLeft& why)
    {
        if (!rhi_list_clean(group))
        {
            why = LEFT_DIRTY;
            return -1;
        }
        LLViewerRegion* region = group->getSpatialPartition()->mRegionp;
        const LLMatrix4& region_matrix = region->mRenderMatrix;
        RHIProbeGroupInfo* ginfo = rhi_probe_info(group);
        if (!ginfo)
        {
            std::vector<RHIProbeGroupInfo>& infos = rhi_probe_groups();
            U32 slot;
            if (!sRHIProbeFree.empty())
            {
                slot = sRHIProbeFree.back();
                sRHIProbeFree.pop_back();
            }
            else
            {
                slot = (U32)infos.size();
                infos.emplace_back();
            }
            infos[slot] = RHIProbeGroupInfo();
            infos[slot].group = group;
            group->mRHIProbeGroup = slot + 1;
            ginfo = &infos[slot];
        }
        if (!ginfo->types[t])
        {
            ginfo->types[t].reset(new RHIProbeTypeInfo());
        }
        RHIProbeTypeInfo* info = ginfo->types[t].get();
        const S32 slot = (S32)(ginfo - rhi_probe_groups().data());
        const F32 now = gFrameTimeSeconds;
        const U32 builds = group->rhiBuilds();
        info->used_at = now;
        if (info->block && (info->builds != builds || info->model != &region_matrix))
        {
            rhi_probe_drop_block(*info);
        }
        if (!info->block)
        {
            if (info->foreign_builds == builds)
            {
                why = LEFT_NEVER;
                return -1;
            }
            if (info->seen_builds != builds)
            {
                info->seen_builds = builds;
                info->seen_at = now;
                why = LEFT_SETTLING;
                return -1;
            }
            if (now - info->seen_at < RHI_PROBE_SETTLE_SECONDS)
            {
                why = LEFT_SETTLING;
                return -1;
            }
            if (now < info->retry_at)
            {
                why = LEFT_RETRY;
                return -1;
            }
            if (sRHIProbeRegisteredFrame != LLFrameTimer::getFrameCount())
            {
                sRHIProbeRegisteredFrame = LLFrameTimer::getFrameCount();
                sRHIProbeRegistered = 0;
            }
            if (sRHIProbeRegistered >= RHI_PROBE_REGISTER_PER_FRAME)
            {
                why = LEFT_BUDGET;
                return -1;
            }
            ++sRHIProbeRegistered;
            if (t != 0)
            {
                // Made at the type's pool loop of this face (it draws them on the CPU meanwhile).
                if (!info->pending)
                {
                    info->pending = true;
                    sRHIProbeFace.pending[t].push_back((U32)slot);
                }
                why = LEFT_CAPTURE;
                return -1;
            }
            if (!rhi_probe_register(*info, group, region_matrix))
            {
                info->retry_at = now + RHI_PROBE_RETRY_SECONDS;
                ++sRHIProbeStats.refused;
                why = info->foreign_builds == builds ? LEFT_NEVER : LEFT_REFUSED;
                return -1;
            }
            ++sRHIProbeStats.made;
        }
        // Once a frame: the block still draws from what GL has, and every
        // texture its batches bind still goes out under the name registered.
        if (info->checked_frame != LLFrameTimer::getFrameCount())
        {
            info->checked_frame = LLFrameTimer::getFrameCount();
            info->valid = LLRHI::sProbeList->blockLive(info->block) && rhi_probe_textures_valid(*info);
            if (!info->valid)
            {
                rhi_probe_drop_block(*info);    // made again from the new names
                why = LEFT_GONE;
                return -1;
            }
        }
        if (!info->valid)
        {
            why = LEFT_GONE;
            return -1;
        }
        return slot;
    }

    // The batches of the face's list from position first, on the CPU as
    // postSort and the type's pool loop would draw them.
    void rhi_probe_draw_cpu(const std::vector<LLDrawInfo*>& batches, size_t first, size_t end, LLPipeline::RHIProbeBody body,
                            void* context)
    {
        const ProbeFaceView& view = sRHIProbeFace.view;
        for (size_t i = first; i < end; ++i)
        {
            LLDrawInfo* info = batches[i];
            // research/44 P1, as postSort tests a probe face's static batches.
            LLVector4a origin, lo, hi;
            origin.load3(info->mModelMatrix->mMatrix[3]);
            lo.setAdd(info->mExtents[0], origin);
            hi.setAdd(info->mExtents[1], origin);
            if (view.outside(lo, hi))
            {
                info->mProbeCullStamp = sRHIProbeFace.serial;
                continue;
            }
            body(*info, context);
            ++sRHIProbeStats.cpu_batches;
        }
    }

    // <plus> research/209 E1: the probe census of the view postSort is sorting
    // (sRHIProbeCensus; rhiProbeFace calls it once the list has chosen). Goes
    // through the visible groups with postSort's filters; only reads them.
    void rhi_probe_census(U32 view)
    {
        RHIProbeCensus& c = sRHIProbeCensusCounts;
        if (gFrameTimeSeconds - c.logged_at >= 10.f)
        {
            for (U32 v = 0; v < LLRHI::SCENE_VIEW_COUNT; ++v)
            {
                if (!c.faces[v])
                {
                    continue;
                }
                U64 all = 0, gltf = 0, list = 0, list_gltf = 0;
                std::ostringstream types;
                for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
                {
                    const U64* who = c.batches[v][t];
                    const U64 sum = who[CENSUS_SCENE] + who[CENSUS_LIST] + who[CENSUS_LEFT] + who[CENSUS_MOVING];
                    all += sum;
                    list += who[CENSUS_LIST];
                    if (rhi_probe_gltf(t))
                    {
                        gltf += sum;
                        list_gltf += who[CENSUS_LIST];
                    }
                    if (!sum)
                    {
                        continue;
                    }
                    types << "; " << RHI_PROBE_TYPE_NAMES[t]
                          << (LLSceneRecords::scopePass(RHI_PROBE_TYPE_LIST[t]) ? " (scene draws)" : " (scene refuses)") << ": scene "
                          << who[CENSUS_SCENE] << ", list " << who[CENSUS_LIST] << ", static left " << who[CENSUS_LEFT]
                          << ", moving " << who[CENSUS_MOVING] << ", could " << c.could[v][t];
                }
                static const char* const VIEW_NAMES[LLRHI::SCENE_VIEW_COUNT] = { "main", "probe", "hero mirror", "water mirror" }; // research/209 E2
                LL_INFOS("RHIProbe") << "probe census (research/209 E1) over " << c.faces[v] << " " << VIEW_NAMES[v]
                                     << " faces: batches " << all
                                     << ", gltf " << gltf << " (" << (all ? (U32)(gltf * 100 / all) : 0) << "%), the list's "
                                     << list << " of which gltf " << list_gltf << " ("
                                     << (list ? (U32)(list_gltf * 100 / list) : 0) << "%)" << types.str() << LL_ENDL;
            }
            c = RHIProbeCensus();
            c.logged_at = gFrameTimeSeconds;
        }
        if (view >= LLRHI::SCENE_VIEW_COUNT || !sCull)
        {
            return;
        }
        ++c.faces[view];

        // As postSort decides: the scene's cut first (its view's passes, its
        // drawable groups), then the list's stamp of this face.
        const bool scene_on = gSceneRecords.active();
        const U32 skip = gPipeline.rhiProbeSkip();
        for (LLCullResult::sg_iterator i = sCull->beginVisibleGroups(); i != sCull->endVisibleGroups(); ++i)
        {
            LLSpatialGroup* group = *i;
            if (group->isDead())
            {
                continue;
            }
            if ((LLPipeline::sUseOcclusion && group->isOcclusionState(LLSpatialGroup::OCCLUDED)) ||
                (LLPipeline::RenderAutoHideSurfaceAreaLimit > 0.f &&
                 group->mSurfaceArea > LLPipeline::RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
            {
                continue;
            }
            LLSpatialPartition* part = group->getSpatialPartition();
            const bool is_static = part && part->mPartitionType == LLViewerRegion::PARTITION_VOLUME && !part->asBridge();
            S32 drawable = -1;  // the scene could draw the group now (looked up once)
            for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
            {
                const S32 index = LLPipeline::rhiProbeTypeIndex(j->first);
                if (index < 0 || !gPipeline.hasRenderType(j->first) || j->second.empty())
                {
                    continue;
                }
                const U32 t = (U32)index;
                const U64 count = j->second.size();
                const bool scope = scene_on && LLSceneRecords::scopePass(j->first);
                if (scope && drawable < 0)
                {
                    drawable = gSceneRecords.shadowGroup(group) ? 1 : 0;
                }
                if (scope && drawable > 0)
                {
                    c.could[view][t] += count;
                }
                ERHIProbeCensusWho who = is_static ? CENSUS_LEFT : CENSUS_MOVING;
                if (scope && drawable > 0 && gSceneRecords.cutsPass(view, j->first))
                {
                    who = CENSUS_SCENE;
                }
                else if (skip && group->mRHIProbeStamp == skip && (group->mRHIProbeTypes & (1u << t)))
                {
                    who = CENSUS_LIST;
                }
                c.batches[view][t][who] += count;
            }
        }
    }
    // </plus>
}

S32 LLPipeline::rhiProbeTypeIndex(U32 type)
{
    for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
    {
        if (RHI_PROBE_TYPE_LIST[t] == type)
        {
            return (S32)t;
        }
    }
    return -1;
}

void LLPipeline::rhiProbeFace(const F32 sides[4][4], const F32 origin[3], U32 serial, U32 leave_types)
{
    RHIProbeFaceState& face = sRHIProbeFace;
    std::vector<RHIProbeGroupInfo>& infos = rhi_probe_groups();
    rhi_probe_face_reset();

    // <plus> research/209 E1: the probe census (LLRHI_PROBE_CENSUS=1) of a probe face or a mirror,
    // once the list has chosen (whichever return below; with the list off too)
    const U32 census_view = sRHIProbeCensus ? LLSceneRecords::currentView() : LLRHI::SCENE_VIEW_COUNT;
    const auto census = rhi_probe_on_exit(
        census_view != LLRHI::SCENE_VIEW_MAIN && census_view < LLRHI::SCENE_VIEW_COUNT && sCull && !mPostSortGroupsOnly, // research/209 E2: probe, mirror, water mirror
        [census_view]() { rhi_probe_census(census_view); });
    // </plus>

    LLRHI::ProbeList* list = LLRHI::sProbeList;
    // LLRHI (research/137, fix-round21): the generated commands turn the list on only
    // when they are ready, so a DGC switch with no extension leaves LLRHI_GPU_DRIVEN_PROBE=0
    // and gpuprobe.toggle off in charge.
    // LLRHI (research/137, fix-round22): ask for the switches first, so
    // ready() (which builds the list) runs only when PROBE or DGC is on.
    const bool want = LLRHI::sGPUDrivenProbe || LLRHI::sGPUDrivenDGC;
    const bool base = list && want && LLRHI::sGPUDriven && LLRHI::sDrawRuns && LLRHI::sDirect &&
                      LLRHI::sDirectDraw && list->ready();     // <plus> research/207 6.3 </plus>
    const bool generated = base && LLRHI::sGPUDrivenDGC && list->generatedReady();
    const bool on = base && (LLRHI::sGPUDrivenProbe || generated);
    if (on != sRHIProbeOn || generated != sRHIProbeGenerated)
    {
        // Off: the blocks go, so their pinned copies leave the pool. Without
        // generated commands the captured types' blocks go (nothing draws them).
        for (RHIProbeGroupInfo& info : infos)
        {
            if (info.group && (!on || !generated))
            {
                rhi_probe_drop_blocks(info, on);
            }
        }
        if (on != sRHIProbeOn)
        {
            LL_INFOS("RHIProbe") << "GPU probe list " << (on ? "on" : "off") << LL_ENDL;
        }
        if (generated != sRHIProbeGenerated)
        {
            LL_INFOS("RHIProbe") << "GPU probe list with generated commands " << (generated ? "on" : "off") << LL_ENDL;
        }
        sRHIProbeOn = on;
        sRHIProbeGenerated = generated;
    }
    if (!on || !serial || !sCull || mPostSortGroupsOnly)
    {
        return;
    }

    RHIProbeStats& stats = sRHIProbeStats;
    const F32 now = gFrameTimeSeconds;
    if (now - stats.logged_at >= 10.f)
    {
        if (stats.faces)
        {
            U32 left = 0;
            for (U32 count : stats.left)
            {
                left += count;
            }
            std::ostringstream left_why, refused_why;
            log_probe_reasons(left_why, stats.left, sProbeLeftNames);
            log_probe_reasons(refused_why, stats.refusals, sProbeRefusalNames);
            for (const auto& backend : stats.backend)
            {
                refused_why << ", backend: " << backend.first << " " << backend.second;
            }
            LL_INFOS("RHIProbe") << "GPU probe list over " << stats.faces << " faces: listed " << stats.listed << " (whole "
                                 << stats.whole << ", types " << stats.types_listed << "), mixed " << stats.mixed << ", groups "
                                 << stats.groups << ", static groups left in the render map " << left << left_why.str()
                                 << ", batches drawn by the list " << stats.gpu_draws << " (indirect draws or executes "
                                 << stats.indirect << "), on the CPU " << stats.cpu_batches << "; blocks made " << stats.made
                                 << " (captured " << stats.captured << "), refused " << stats.refused << refused_why.str() << LL_ENDL;
            // <plus> research/209 E1: by type (209 3.1 C2: the list's batches outside gltf go to 0 once
            // the scene cuts the probe faces; C4: gltf's share, which the scene refuses). "groups":
            // listed; "list"/"cpu": their batches by the list or on the CPU; "to the scene": static
            // groups the scene draws in the face, not made candidates (209 S2).
            U32 gpu = 0, gpu_gltf = 0;
            std::ostringstream by_type;
            for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
            {
                gpu += stats.type_gpu[t];
                gpu_gltf += rhi_probe_gltf(t) ? stats.type_gpu[t] : 0;
                if (stats.type_groups[t] || stats.type_gpu[t] || stats.type_cpu[t] || stats.type_scene[t])
                {
                    by_type << "; " << RHI_PROBE_TYPE_NAMES[t] << ": groups " << stats.type_groups[t] << ", list "
                            << stats.type_gpu[t] << ", cpu " << stats.type_cpu[t] << ", to the scene " << stats.type_scene[t];
                }
            }
            LL_INFOS("RHIProbe") << "GPU probe list by type over " << stats.faces << " faces: batches by the list " << gpu
                                 << ", gltf " << gpu_gltf << " (" << (gpu ? (U32)((U64)gpu_gltf * 100 / gpu) : 0) << "%)"
                                 << by_type.str() << LL_ENDL;
            // </plus>
        }
        stats = RHIProbeStats();
        stats.logged_at = now;
    }
    ++stats.faces;

    // Blocks no face used for a while give their pool copies back.
    if (now - sRHIProbeSweptAt >= 1.f)
    {
        sRHIProbeSweptAt = now;
        for (RHIProbeGroupInfo& info : infos)
        {
            for (U32 t = 0; info.group && t < RHI_PROBE_TYPES; ++t)
            {
                if (info.types[t] && info.types[t]->block && now - info.types[t]->used_at > RHI_PROBE_UNUSED_SECONDS)
                {
                    rhi_probe_drop_block(*info.types[t]);
                }
            }
        }
    }

    for (U32 i = 0; i < 4; ++i)
    {
        for (U32 j = 0; j < 4; ++j)
        {
            face.view.plane[i][j] = sides[i][j];
        }
    }
    for (U32 j = 0; j < 3; ++j)
    {
        face.view.origin[j] = origin[j];
    }

    // <plus> research/209 E1 (S2): the types whose batches the retained scene
    // draws in this view. postSort cuts them from its drawable groups before it
    // reads the list's stamp (so the list never drew them: their positions stay
    // U32_MAX); here those groups are not made candidates either (no block
    // made, checked, captured, nor its textures marked). A group this calls
    // the scene's that postSort then does not cut (made after the view began,
    // a texture waiting for its bind) is drawn from the render map: never
    // twice, never missing. None with the scene off, or while only the main
    // view is cut (stage 2d): the list goes on as before.
    const U32 scene_view = gSceneRecords.active() ? LLSceneRecords::currentView() : LLRHI::SCENE_VIEW_COUNT;
    U32 scene_types = 0;
    if (scene_view < LLRHI::SCENE_VIEW_COUNT)
    {
        for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
        {
            scene_types |= gSceneRecords.cutsPass(scene_view, RHI_PROBE_TYPE_LIST[t]) ? 1u << t : 0u;
        }
    }
    // </plus>

    // The groups postSort will go through, in its order, with its filters.
    bool mixed = false;     // a static group with batches of a listed type stays in the render map
    for (LLCullResult::sg_iterator i = sCull->beginVisibleGroups(); i != sCull->endVisibleGroups(); ++i)
    {
        LLSpatialGroup* group = *i;
        S32 scene_group = -1;   // <plus> research/209 E1: the scene could draw the group now (looked up once) </plus>
        if (group->isDead())
        {
            continue;
        }
        if ((sUseOcclusion && group->isOcclusionState(LLSpatialGroup::OCCLUDED)) ||
            (RenderAutoHideSurfaceAreaLimit > 0.f &&
             group->mSurfaceArea > RenderAutoHideSurfaceAreaLimit * llmax(group->mObjectBoxSize, 10.f)))
        {
            continue;
        }
        if (group->hasState(LLSpatialGroup::NEW_DRAWINFO) && group->hasState(LLSpatialGroup::GEOM_DIRTY) && !gCubeSnapshot)
        {
            continue;       // rebuilt on the way (not in a probe face): stays in the render map
        }
        LLSpatialPartition* part = group->getSpatialPartition();
        if (!part || part->mPartitionType != LLViewerRegion::PARTITION_VOLUME || part->asBridge())
        {
            continue;       // not static (a bridge's): stays in the render map
        }
        for (LLSpatialGroup::draw_map_t::iterator j = group->mDrawMap.begin(); j != group->mDrawMap.end(); ++j)
        {
            const S32 index = rhiProbeTypeIndex(j->first);
            if (index < 0 || (index > 0 && !generated) || !hasRenderType(j->first) || (leave_types & (1u << index)))
            {
                continue;
            }
            // <plus> research/209 E1 (S2): the scene draws the group's batches of the type in this view
            if (scene_types & (1u << index))
            {
                if (scene_group < 0)
                {
                    scene_group = gSceneRecords.shadowGroup(group) ? 1 : 0;
                }
                if (scene_group > 0)
                {
                    ++stats.type_scene[index];
                    continue;
                }
            }
            // </plus>
            bool draws = false;
            for (LLDrawInfo* batch : j->second)
            {
                draws = draws || batch->mCount;
            }
            if (!draws)
            {
                continue;
            }
            const U32 t = (U32)index;
            EProbeLeft why = LEFT_REASONS;
            const S32 slot = rhi_probe_usable(group, t, why);
            if (slot < 0)
            {
                mixed = true;   // stays in the render map; the others are listed in their places
                ++stats.left[why < LEFT_REASONS ? why : LEFT_DIRTY];
                continue;
            }
            face.groups[t].push_back((U32)slot);
        }
    }
    stats.mixed += mixed ? 1 : 0;

    bool pending = false;
    for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
    {
        pending = pending || !face.pending[t].empty();
        if (!face.groups[t].empty())
        {
            face.types |= 1u << t;
        }
    }
    if (!face.types && !pending)
    {
        return;
    }

    face.active = true;
    face.cull = sCull;
    face.serial = serial;
    face.generated = generated;
    for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
    {
        if (!(face.types & (1u << t)))
        {
            continue;
        }
        face.positions[t].assign(face.groups[t].size(), U32_MAX);
        for (size_t k = 0; k < face.groups[t].size(); ++k)
        {
            RHIProbeGroupInfo& info = infos[face.groups[t][k]];
            RHIProbeTypeInfo& type_info = *info.types[t];
            if (info.group->mRHIProbeStamp != serial)
            {
                info.group->mRHIProbeStamp = serial;
                info.group->mRHIProbeTypes = 0;
            }
            info.group->mRHIProbeTypes |= 1u << t;
            type_info.face_index = (U32)k;
            // What directTexture marks for the batches it draws (all of the block's: never fewer).
            for (const RHIProbeTexture& tex : type_info.textures)
            {
                tex.texture->setActive();
            }
        }
        ++stats.types_listed;
        stats.groups += (U32)face.groups[t].size();
        stats.type_groups[t] += (U32)face.groups[t].size(); // <plus> research/209 E1 </plus>
    }
    if (face.types)
    {
        ++stats.listed;
        stats.whole += mixed ? 0 : 1;
    }
}

void LLPipeline::rhiProbeAt(LLSpatialGroup* group, U32 type, U32 position)
{
    RHIProbeFaceState& face = sRHIProbeFace;
    const S32 index = rhiProbeTypeIndex(type);
    const RHIProbeGroupInfo* info = rhi_probe_info(group);
    if (index < 0 || !face.active || face.cull != sCull || !info || !info->types[index])
    {
        return;
    }
    const U32 t = (U32)index;
    const U32 k = info->types[t]->face_index;
    if (k < face.groups[t].size() && face.groups[t][k] == group->mRHIProbeGroup - 1 &&
        face.positions[t].size() == face.groups[t].size())
    {
        face.positions[t][k] = position;
    }
}

U32 LLPipeline::rhiProbeSkip() const
{
    return sRHIProbeFace.active && sRHIProbeFace.types && sRHIProbeFace.cull == sCull ? sRHIProbeFace.serial : 0;
}

bool LLPipeline::rhiProbeDraw(U32 type, bool listable, RHIProbeBody body, RHIProbeTextures textures, void* context)
{
    RHIProbeFaceState& face = sRHIProbeFace;
    const S32 index = rhiProbeTypeIndex(type);
    if (index < 0 || !face.active || face.cull != sCull || !body)
    {
        return false;
    }
    const U32 t = (U32)index;
    std::vector<RHIProbeGroupInfo>& infos = rhi_probe_groups();
    LLRHI::ProbeList* list = LLRHI::sProbeList;

    // Captures of the type's queued groups, before anything of the type is
    // drawn (they record nothing; the loop sets its state again per batch).
    if (!face.pending[t].empty())
    {
        for (U32 slot : face.pending[t])
        {
            if (slot >= infos.size() || !infos[slot].group || !infos[slot].types[t])
            {
                continue;
            }
            RHIProbeTypeInfo& info = *infos[slot].types[t];
            info.pending = false;
            if (!list || !listable || !face.generated || info.block)
            {
                continue;
            }
            if (rhi_probe_register_captured(info, infos[slot].group, type, body, textures, context))
            {
                ++sRHIProbeStats.made;
                ++sRHIProbeStats.captured;
            }
            else
            {
                info.retry_at = gFrameTimeSeconds + RHI_PROBE_RETRY_SECONDS;
                ++sRHIProbeStats.refused;
            }
        }
        face.pending[t].clear();
    }
    if (!(face.types & (1u << t)) || face.groups[t].empty())
    {
        return false;
    }

    // The listed groups postSort reached, in order: their batches, where they
    // go in the render map (a segment: the groups with no render map batch
    // between them), and the runs one indirect draw (G5) or one execute
    // (generated commands: any chunk) can take, merged across the blocks of a
    // segment.
    struct Segment
    {
        U32 position = 0;   // render map batches drawn before it
        U32 first = 0;      // its batches
        U32 end = 0;
        U32 run_first = 0;  // its runs
        U32 run_end = 0;
    };
    static std::vector<LLDrawInfo*> batches;
    static std::vector<RHIProbeRun> runs;       // first: position in batches
    static std::vector<U32> blocks;
    static std::vector<U32> slots;
    static std::vector<Segment> segments;
    batches.clear();
    runs.clear();
    blocks.clear();
    slots.clear();
    segments.clear();
    const bool generated = face.generated;
    const std::vector<U32>& groups = face.groups[t];
    const std::vector<U32>& positions = face.positions[t];
    for (size_t k = 0; k < groups.size(); ++k)
    {
        const U32 position = k < positions.size() ? positions[k] : U32_MAX;
        if (position == U32_MAX)
        {
            continue;   // postSort did not reach it: the render map would not have its batches either
        }
        const U32 slot = groups[k];
        const RHIProbeTypeInfo& info = *infos[slot].types[t];
        const U32 base = (U32)batches.size();
        if (segments.empty() || segments.back().position != position)
        {
            Segment segment;
            segment.position = position;
            segment.first = segment.end = base;
            segment.run_first = segment.run_end = (U32)runs.size();
            segments.push_back(segment);
        }
        Segment& segment = segments.back();
        batches.insert(batches.end(), info.batches.begin(), info.batches.end());
        blocks.push_back(info.block);
        slots.push_back(slot);
        for (const RHIProbeRun& r : info.runs)
        {
            if (runs.size() > segment.run_first && (generated || runs.back().chunk == r.chunk) && runs.back().format == r.format &&
                runs.back().state == r.state && runs.back().first + runs.back().count == base + r.first)
            {
                runs.back().count += r.count;
            }
            else
            {
                RHIProbeRun run = r;
                run.first = base + r.first;
                runs.push_back(run);
            }
        }
        segment.end = (U32)batches.size();
        segment.run_end = (U32)runs.size();
    }

    // The render map's batches up to a position, as the pool loop draws them.
    LLCullResult::drawinfo_iterator map = beginRenderMap(type);
    const size_t map_size = endRenderMap(type) - map;
    size_t drawn = 0;
    auto draw_map = [&](size_t to)
    {
        for (to = llmin(to, map_size); drawn < to; ++drawn)
        {
            LLDrawInfo* params = map[drawn];
            llassert(params);
            if (params)
            {
                body(*params, context);
            }
        }
    };

    const U32 gpu_before = sRHIProbeStats.gpu_draws;    // <plus> research/209 E1: the type's share of the counts </plus>
    const U32 cpu_before = sRHIProbeStats.cpu_batches;  // <plus> research/209 E1 </plus>
    U32 run_batches = 0;
    for (const RHIProbeRun& run : runs)
    {
        run_batches += run.count;
    }
    // PASS_SIMPLE's blocks are for pushBatches drawing with Deferred Diffuse
    // (G5); a captured block is checked against the bound program by the list.
    const bool gpu = list && listable && LLRHI::sDrawRuns && !runs.empty() && run_batches == batches.size() &&
                     (t != 0 ? generated : LLGLSLShader::sCurBoundShaderPtr == &gDeferredDiffuseProgram);
    bool tried = false;
    bool prepared = false;
    for (const Segment& segment : segments)
    {
        draw_map(segment.position);
        if (!gpu)
        {
            rhi_probe_draw_cpu(batches, segment.first, segment.end, body, context);
            continue;
        }
        for (U32 r = segment.run_first; r < segment.run_end; ++r)
        {
            const RHIProbeRun& run = runs[r];
            // The run's first batch the usual way: it opens a draw run with the
            // pass's bindings (outside the face, it draws no pixel).
            body(*batches[run.first], context);
            ++sRHIProbeStats.cpu_batches;
            if (!tried)
            {
                tried = true;
                // The face's matrices for every region its blocks use, as
                // pushDirectBatch makes them for the bound program.
                static std::vector<LLRHI::ProbeList::Model> models;
                static std::vector<LLRender::DirectMatrices> storage;
                models.clear();
                storage.clear();
                storage.reserve(slots.size());
                for (U32 slot : slots)
                {
                    const LLMatrix4* model = infos[slot].types[t]->model;
                    bool known = false;
                    for (const auto& m : models)
                    {
                        known |= m.model == (const F32*)model->mMatrix;
                    }
                    if (known)
                    {
                        continue;
                    }
                    storage.emplace_back();
                    LLRHI::ProbeList::Model m;
                    m.model = (const F32*)model->mMatrix;
                    glm::mat4 modelview = glm::make_mat4(gGLModelView);
                    modelview *= glm::make_mat4((const F32*)model->mMatrix);
                    gGL.fillDirectMatrices(modelview, storage.back(), m.matrices);
                    models.push_back(m);
                }
                LLRHI::ProbeList::Face request;
                request.blocks = blocks.data();
                request.block_count = (U32)blocks.size();
                request.models = models.data();
                request.model_count = (U32)models.size();
                for (U32 i = 0; i < 4; ++i)
                {
                    for (U32 j = 0; j < 4; ++j)
                    {
                        request.sides[i][j] = face.view.plane[i][j];
                    }
                }
                for (U32 j = 0; j < 3; ++j)
                {
                    request.origin[j] = face.view.origin[j];
                }
                request.generated = generated;
                prepared = list->prepare(request);
            }
            if (run.count < 2)
            {
                continue;
            }
            const bool listed = prepared && (generated ? list->drawSegment(run.first + 1, run.count - 1, run.format)
                                                       : list->drawRun(run.first + 1, run.count - 1, run.chunk, run.format));
            if (listed)
            {
                sRHIProbeStats.gpu_draws += run.count - 1;
                ++sRHIProbeStats.indirect;
            }
            else
            {
                rhi_probe_draw_cpu(batches, run.first + 1, run.first + run.count, body, context);
            }
        }
    }
    draw_map(map_size);
    // <plus> research/209 E1
    sRHIProbeStats.type_gpu[t] += sRHIProbeStats.gpu_draws - gpu_before;
    sRHIProbeStats.type_cpu[t] += sRHIProbeStats.cpu_batches - cpu_before;
    // </plus>
    return true;
}

void LLPipeline::rhiProbeClear(LLSpatialGroup* group)
{
    if (RHIProbeGroupInfo* info = rhi_probe_info(group))
    {
        rhi_probe_drop_blocks(*info, false);
    }
}

void LLPipeline::rhiProbeForget(LLSpatialGroup* group)
{
    if (RHIProbeGroupInfo* info = rhi_probe_info(group))
    {
        rhi_probe_drop_blocks(*info, false);
        // Not in a face's list any more (the face's render map goes with it too).
        // Its place in the face's lists goes (the slot may be handed out again);
        // the other listed groups are still drawn in their places.
        const U32 slot = group->mRHIProbeGroup - 1;
        for (U32 t = 0; t < RHI_PROBE_TYPES; ++t)
        {
            std::vector<U32>& groups = sRHIProbeFace.groups[t];
            std::vector<U32>& positions = sRHIProbeFace.positions[t];
            for (size_t k = 0; k < groups.size(); ++k)
            {
                if (groups[k] == slot)
                {
                    groups[k] = U32_MAX;
                    if (k < positions.size())
                    {
                        positions[k] = U32_MAX;     // rhiProbeDraw skips it
                    }
                }
            }
            std::vector<U32>& pending = sRHIProbeFace.pending[t];
            pending.erase(std::remove(pending.begin(), pending.end(), slot), pending.end());
        }
        *info = RHIProbeGroupInfo();
        sRHIProbeFree.push_back(slot);
    }
    group->mRHIProbeGroup = 0;
    group->mRHIProbeStamp = 0;
    group->mRHIProbeTypes = 0;
}

// ----------------------------------------------------------------------------
// LLRHI: static G-buffer cache of the realtime reflection probe's faces
// (research/44 P5). See ProbeStatic in pipeline.h.

namespace
{
    enum EProbeStaticClose
    {
        CLOSE_GROUP,        // a bridge's group, or one waiting for a rebuild
        CLOSE_BATCH,        // rigged, animated texture, no region matrix
        CLOSE_BUFFER,       // no vertex buffer, a mapped one, or one without an attribute the program reads
        CLOSE_TEXTURE,      // a texture without a name, with options to apply, media or a null list entry
        CLOSE_VERSIONS,     // the backend has no version for a texture or buffer (or it is rendered into)
        CLOSE_REASONS
    };
    const char* const sProbeStaticCloseNames[CLOSE_REASONS] = { "group", "batch", "buffer", "texture", "versions" };

    struct ProbeStaticStats
    {
        U32 faces = 0;          // faces that took a prefix
        U32 hits = 0;           // copied in from the cache
        U32 drawn = 0;          // drawn into the cache, then copied in
        U32 first = 0;          // of drawn: no cache yet (or a new size)
        U32 changed = 0;        // of drawn: the prefix or the camera changed
        U32 unsure = 0;         // of drawn: skipped draws, or a program or texture changed meanwhile (not kept)
        U32 fallback = 0;       // drawn straight into the G-buffer (no cache to use)
        U64 prefix = 0;         // prefix batches
        U64 simple = 0;         // all PASS_SIMPLE batches of those faces
        U32 closed[CLOSE_REASONS] = {};
        F32 logged_at = 0.f;
    };
    ProbeStaticStats sProbeStaticStats;

    inline void probe_static_mix(U64& hash, U64 value)
    {
        hash = LLRHI::fnv1aMix(hash, value);
    }

    inline void probe_static_mix_floats(U64& hash, const F32* values, U32 count)
    {
        hash = LLRHI::fnv1aFloats(hash, values, count);
    }

    // The GL name a direct draw of the batch binds for tex (LLTexUnit::directTexture),
    // 0 when the cache cannot vouch for what it shows.
    U32 probe_static_texture(LLViewerTexture* tex)
    {
        if (!tex || tex->getType() == LLViewerTexture::MEDIA_TEXTURE || tex->getType() == LLViewerTexture::DYNAMIC_TEXTURE)
        {
            return 0;
        }
        const U32 name = LLTexUnit::directName(tex);
        return name == LLRHI::DirectDraw::KEEP_UNIT ? 0 : name;
    }

    // What LLRenderPass::pushDirectBatch marks used for a PASS_SIMPLE batch
    // (directTexture's setActive), for a batch the cache shows instead.
    void probe_static_mark_used(LLDrawInfo* info)
    {
        if (!info->mCount)
        {
            return;     // pushBatch draws nothing
        }
        if (info->mTextureList.size() > 1)
        {
            for (LLPointer<LLViewerTexture>& tex : info->mTextureList)
            {
                if (tex.notNull())
                {
                    tex->setActive();
                }
            }
        }
        else if (info->mTexture.notNull())
        {
            info->mTexture->setActive();
        }
    }
}

void LLPipeline::beginProbeStatic()
{
    mProbeStatic.active = LLRHI::sProbeStaticCache;     // <plus> research/207 6.3 </plus>
    mProbeStatic.face = -1;
    if (!LLRHI::sProbeStaticCache)
    {
        releaseProbeStatic();   // switched off: give the memory back
    }
}

void LLPipeline::endProbeStatic()
{
    mProbeStatic.active = false;
    mProbeStatic.face = -1;
    // A prefix renderGeomDeferred did not draw would be missing from its face: none should be left.
    llassert(!mProbeStatic.collecting);
    mProbeStatic.collecting = false;
    mProbeStatic.batches.clear();
}

void LLPipeline::releaseProbeStatic()
{
    for (U32 f = 0; f < 6; ++f)
    {
        if (mProbeStaticTarget[f].isComplete())
        {
            mProbeStaticTarget[f].release();
        }
        mProbeStatic.faces[f] = ProbeStaticFace();
    }
}

bool LLPipeline::probeStaticBegin(LLCamera& camera)
{
    ProbeStatic& ps = mProbeStatic;
    static LLCachedControl<bool> has_emissive(gSavedSettings, "RenderEnableEmissiveBuffer", false);
    if (!ps.active || ps.face < 0 || ps.face >= 6 || !LLRHI::sProbeStaticCache ||     // <plus> research/207 6.3 </plus>
        !LLRHI::sObjectVersions || !LLRHI::sBackendCounters ||
        !sProbeFaceRender || !gCubeSnapshot || sShadowRender || LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD ||
        mPostSortGroupsOnly || gUseWireframe || mRT != &mAuxillaryRT || !sCull ||
        !hasRenderType(LLDrawPool::POOL_SIMPLE) || !hasRenderType(LLRenderPass::PASS_SIMPLE) ||
        !gDeferredProbeStaticCopyProgram.mProgramObject || !gDeferredDiffuseProgram.mProgramObject)
    {
        return false;
    }
    const LLRenderTarget& gbuffer = mRT->deferredScreen;
    // Six copies of the probe's G-buffer (probe resolution x 4, 512 px at the
    // default 128): about 44 MB at 512, 176 MB at 1024. Not above that.
    if (!gbuffer.isComplete() || gbuffer.getWidth() > 1024 || gbuffer.getHeight() > 1024 ||
        gbuffer.getNumTextures() != (has_emissive ? 4u : 3u))
    {
        return false;
    }

    // A prefix taken earlier must have been drawn by now (probeStaticDraw).
    llassert(!ps.collecting);

    U32 program_changes = 0, skips = 0, texture_versions = 0;
    LLRHI::sBackendCounters(program_changes, skips, texture_versions);

    U64 hash = LLRHI::FNV1A_BASIS_SHORT;
    probe_static_mix(hash, (U64)ps.face);
    probe_static_mix(hash, ((U64)gbuffer.getWidth() << 32) | gbuffer.getHeight());
    probe_static_mix(hash, gbuffer.getNumTextures());
    probe_static_mix(hash, program_changes);    // shaders reloaded
    probe_static_mix(hash, mHeroProbeManager.isMirrorPass() ? 1 : 0);
    // The face's camera (its matrices are compared again where it draws).
    probe_static_mix_floats(hash, camera.getOrigin().mV, 3);
    probe_static_mix_floats(hash, camera.getAtAxis().mV, 3);
    probe_static_mix_floats(hash, camera.getLeftAxis().mV, 3);
    probe_static_mix_floats(hash, camera.getUpAxis().mV, 3);
    const F32 lens[4] = { camera.getNear(), camera.getFar(), camera.getView(), camera.getAspect() };
    probe_static_mix_floats(hash, lens, 4);

    ps.collecting = true;
    ps.open = true;
    ps.cull = sCull;
    ps.hash = hash;
    ps.program_changes = program_changes;
    ps.texture_versions = texture_versions;
    ps.last_model = nullptr;
    ps.batches.clear();
    ps.simple = 0;
    ++sProbeStaticStats.faces;
    return true;
}

bool LLPipeline::probeStaticTake(LLSpatialGroup* group, LLDrawInfo* info)
{
    ProbeStatic& ps = mProbeStatic;
    if (!ps.collecting)
    {
        return false;
    }
    ++ps.simple;
    if (!ps.open)
    {
        return false;
    }
    auto stop = [&ps](EProbeStaticClose why)
    {
        ps.open = false;
        ++sProbeStaticStats.closed[why];
        return false;
    };
    if (!group)
    {
        return stop(CLOSE_GROUP);
    }
    if (info->mAvatar.notNull() || info->mTextureMatrix || !info->mModelMatrix)
    {
        return stop(CLOSE_BATCH);
    }

    // Everything a PASS_SIMPLE draw of the batch reads besides the face's
    // camera and the program: its buffers and textures (by version), ranges and
    // model matrix. Its place in the prefix goes in by the order of mixing.
    U64 hash = ps.hash;
    probe_static_mix(hash, (U64)(uintptr_t)info);
    if (info->mCount)
    {
        LLVertexBuffer* vb = info->mVertexBuffer;
        // Every attribute the program reads from the buffer (a missing one would
        // read a constant whatever was drawn before left).
        const U32 attributes = gDeferredDiffuseProgram.mAttributeMask;
        if (!vb || vb->isMapped() || !vb->getGLBuffer() || (vb->getTypeMask() & attributes) != attributes)
        {
            return stop(CLOSE_BUFFER);
        }
        constexpr U32 MAX_TEXTURES = 32;   // pushDirectBatch's MAX_UNITS
        U32 textures[MAX_TEXTURES];
        U32 texture_count = 0;
        if (info->mTextureList.size() > 1)
        {
            if (info->mTextureList.size() > MAX_TEXTURES)
            {
                return stop(CLOSE_TEXTURE);
            }
            for (const LLPointer<LLViewerTexture>& tex : info->mTextureList)
            {
                // A null entry keeps the unit's binding from whatever was drawn before.
                const U32 name = probe_static_texture(tex.get());
                if (!name)
                {
                    return stop(CLOSE_TEXTURE);
                }
                textures[texture_count++] = name;
            }
        }
        else if (info->mTexture.notNull())
        {
            const U32 name = probe_static_texture(info->mTexture.get());
            if (!name)
            {
                return stop(CLOSE_TEXTURE);
            }
            textures[texture_count++] = name;
        }
        probe_static_mix(hash, texture_count);  // none: unit 0 unbound
        const U32 buffers[2] = { vb->getGLBuffer(), vb->getGLIndices() };
        if (!LLRHI::sObjectVersions(textures, texture_count, buffers, buffers[1] ? 2 : 1, hash))
        {
            return stop(CLOSE_VERSIONS);
        }
        probe_static_mix(hash, ((U64)info->mOffset << 32) | info->mCount);
        probe_static_mix(hash, ((U64)info->mStart << 16) | info->mEnd);
        probe_static_mix(hash, ((U64)vb->getTypeMask() << 32) | vb->getIndicesType());
        probe_static_mix(hash, ((U64)vb->getNumVerts() << 32) | vb->getNumIndices());
        probe_static_mix(hash, (U64)(uintptr_t)info->mModelMatrix);
        if (info->mModelMatrix != ps.last_model)
        {
            // A region's matrix: its values change when the agent's region changes.
            probe_static_mix_floats(hash, (const F32*)info->mModelMatrix->mMatrix, 16);
            ps.last_model = info->mModelMatrix;
        }
    }
    ps.hash = hash;
    ps.batches.push_back(info);
    return true;
}

void LLPipeline::probeStaticDraw()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    LL_PROFILE_GPU_ZONE("probe static cache");
    ProbeStatic& ps = mProbeStatic;
    if (!ps.collecting)
    {
        return;
    }
    ps.collecting = false;

    ProbeStaticStats& stats = sProbeStaticStats;
    stats.prefix += ps.batches.size();
    stats.simple += ps.simple;
    if (gFrameTimeSeconds - stats.logged_at >= 10.f)
    {
        if (stats.faces)
        {
            std::ostringstream closed;
            for (U32 r = 0; r < CLOSE_REASONS; ++r)
            {
                closed << (r ? ", " : "") << sProbeStaticCloseNames[r] << " " << stats.closed[r];
            }
            LL_INFOS("RHIProbe") << "probe static cache over " << stats.faces << " faces: copied in " << stats.hits
                                 << ", drawn " << stats.drawn << " (first " << stats.first << ", changed " << stats.changed
                                 << ", not kept " << stats.unsure << "), straight " << stats.fallback << "; prefix batches "
                                 << stats.prefix << " of PASS_SIMPLE " << stats.simple << "; closed by " << closed.str() << LL_ENDL;
        }
        stats = ProbeStaticStats();
        stats.logged_at = gFrameTimeSeconds;
    }

    if (ps.batches.empty())
    {
        return;
    }

    // The simple pools draw the prefix as they would have drawn it from the
    // render map (the GPU probe list keeps out: another cull result).
    static LLCullResult prefix;
    auto draw_prefix = [this, &ps]()
    {
        prefix.clear();
        for (LLDrawInfo* info : ps.batches)
        {
            prefix.pushDrawInfo(LLRenderPass::PASS_SIMPLE, info);
        }
        LLCullResult* saved = sCull;
        sCull = &prefix;
        gGL.setColorMask(true, true);
        bool drew = false;
        for (pool_set_t::iterator iter1 = mPools.begin(); iter1 != mPools.end(); ++iter1)
        {
            LLDrawPool* poolp = *iter1;
            if (poolp->getType() != LLDrawPool::POOL_SIMPLE)
            {
                continue;
            }
            // The first simple pool runs the passes for all of them, as renderGeomDeferred does.
            if (poolp->getNumDeferredPasses() > 0)
            {
                gGLLastMatrix = NULL;
                gGL.loadMatrix(gGLModelView);
                for (S32 i = 0; i < poolp->getNumDeferredPasses(); i++)
                {
                    LLVertexBuffer::unbind();
                    poolp->beginDeferredPass(i);
                    for (pool_set_t::iterator iter2 = iter1; iter2 != mPools.end() && (*iter2)->getType() == LLDrawPool::POOL_SIMPLE; ++iter2)
                    {
                        if (!(*iter2)->getSkipRenderFlag())
                        {
                            (*iter2)->renderDeferred(i);
                        }
                    }
                    poolp->endDeferredPass(i);
                    LLVertexBuffer::unbind();
                }
                drew = true;
            }
            break;
        }
        sCull = saved;
        prefix.clear();
        gGLLastMatrix = NULL;
        gGL.loadMatrix(gGLModelView);
        return drew;
    };

    const S32 f = ps.face;
    LLRenderTarget& gbuffer = mRT->deferredScreen;
    if (f < 0 || f >= 6 || sCull != ps.cull || mRT != &mAuxillaryRT)
    {
        // Not drawn where it was taken: draw it here, where the render map had it.
        ++stats.fallback;
        draw_prefix();
        ps.batches.clear();
        return;
    }

    ProbeStaticFace& face = ps.faces[f];
    LLRenderTarget& target = mProbeStaticTarget[f];
    const glm::mat4& projection = gGL.getProjectionMatrix();
    const bool same_target = target.isComplete() && target.getWidth() == gbuffer.getWidth() &&
                             target.getHeight() == gbuffer.getHeight() && target.getNumTextures() == gbuffer.getNumTextures();
    // Nothing the hash read may have changed since postSort: the programs, and
    // the textures (a texture thread may upload one meanwhile; the buffers are
    // only written on this thread, and not between there and here).
    U32 programs_now = 0, skips_now = 0, textures_now = 0;
    LLRHI::sBackendCounters(programs_now, skips_now, textures_now);
    const bool settled = programs_now == ps.program_changes && textures_now == ps.texture_versions;
    const bool hit = settled && face.valid && same_target && face.hash == ps.hash && face.batches == (U32)ps.batches.size() &&
                     !memcmp(face.modelview, gGLModelView, sizeof(face.modelview)) &&
                     !memcmp(face.projection, glm::value_ptr(projection), sizeof(face.projection));

    if (hit)
    {
        ++stats.hits;
        for (LLDrawInfo* info : ps.batches)
        {
            probe_static_mark_used(info);
        }
    }
    else
    {
        if (!same_target)
        {
            target.release();
            face = ProbeStaticFace();
            if (!target.allocate(gbuffer.getWidth(), gbuffer.getHeight(), GL_RGBA, true) || !addDeferredAttachments(target) ||
                target.getNumTextures() != gbuffer.getNumTextures())
            {
                target.release();
                ++stats.fallback;
                draw_prefix();
                ps.batches.clear();
                return;
            }
            ++stats.first;
            if (f == 0)
            {
                LL_INFOS("RHIProbe") << "probe static cache: six faces of " << gbuffer.getWidth() << "x" << gbuffer.getHeight()
                                     << " with " << gbuffer.getNumTextures() << " color attachments" << LL_ENDL;
            }
        }
        else
        {
            ++(face.valid ? stats.changed : stats.first);
        }

        const U32 skips_before = skips_now;
        U32 programs_after = 0, skips_after = 0, textures_after = 0;

        // The prefix alone on a cleared target: covered pixels get a non-zero
        // G-buffer flag in the normal's alpha, the others keep 0 (and depth 1).
        target.bindTarget();
        LLRHI::clearColor(0.f, 0.f, 0.f, 0.f);
        target.clear();
        LLRHI::clearColor(1.f, 0.f, 1.f, 1.f);     // display_cube_face's (no wireframe here)
        const bool drew = draw_prefix();
        target.flush();     // back to the face's G-buffer

        // Kept only if it is what the hash describes: nothing skipped, no
        // program or texture changed from postSort until it was drawn.
        LLRHI::sBackendCounters(programs_after, skips_after, textures_after);
        face.valid = drew && skips_after == skips_before && programs_after == ps.program_changes &&
                     textures_after == ps.texture_versions;
        face.hash = ps.hash;
        face.batches = (U32)ps.batches.size();
        memcpy(face.modelview, gGLModelView, sizeof(face.modelview));
        memcpy(face.projection, glm::value_ptr(projection), sizeof(face.projection));
        if (!face.valid)
        {
            ++stats.unsure;
        }
        ++stats.drawn;
    }
    ps.batches.clear();

    // Copy in the covered pixels with their depth: the G-buffer as drawing the
    // prefix here, after the sky (whose depth was cleared), would leave it.
    {
        LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_ALWAYS);
        LLGLDisable cull(GL_CULL_FACE);
        LLGLDisable blend(GL_BLEND);
        gGL.setColorMask(true, true);
        LLGLSLShader& shader = gDeferredProbeStaticCopyProgram;
        shader.bind();
        const U32 uniforms[4] = { LLShaderMgr::DEFERRED_DIFFUSE, LLShaderMgr::DEFERRED_SPECULAR, LLShaderMgr::NORMAL_MAP,
                                  LLShaderMgr::DEFERRED_EMISSIVE };
        S32 channels[5] = { -1, -1, -1, -1, -1 };
        for (U32 i = 0; i < 4 && i < target.getNumTextures(); ++i)
        {
            channels[i] = shader.enableTexture(uniforms[i], target.getUsage());
            if (channels[i] > -1)
            {
                target.bindTexture(i, channels[i], LLTexUnit::TFO_POINT);
            }
        }
        channels[4] = shader.enableTexture(LLShaderMgr::DEFERRED_DEPTH, target.getUsage());
        if (channels[4] > -1)
        {
            gGL.getTexUnit(channels[4])->bind(&target, true);
        }
        mScreenTriangleVB->setBuffer();
        mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
        for (S32 channel : channels)
        {
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
            }
        }
        shader.unbind();
    }
    gGLLastMatrix = NULL;
    gGL.loadMatrix(gGLModelView);
}

// LLRHI (research/65): the texture list's fetch pass scans faces on the cull threads.
void rhi_run_cull_jobs(U32 count, const std::function<void(U32)>& job)
{
    run_cull_jobs(count, job);
}

// <plus> research/184 A1 (plus/cullinto): the cull threads besides the calling thread
U32 rhi_cull_threads()
{
    return CullWorkers::instance().threads();
}

//static
LLCullResult* LLPipeline::cullIntoResult()
{
    return sCull;
}
// </plus>

// LLRHI (research/63): the cull threads for the avatar keyframe samples
// (llvoavatar.cpp, rhi_prepare_avatar_motions). Main thread; fork and join.
void rhi_run_worker_jobs(U32 count, const std::function<void(U32)>& job)
{
    run_cull_jobs(count, job);
}
