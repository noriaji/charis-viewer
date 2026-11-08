/**
 * @file llrhi.cpp
 * @brief Render hardware interface: backend selection and the hook into GL loading.
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

#include "llrhi.h"
#include "llfile.h"
#include "llframetimer.h"      // CuePoll
#include "llrhidrawlist.h"
#include "llrhiglstats.h"
#include "llrhiimmediate.h"     // research/137 N9: immediatering.toggle
#include "llrhiocclusion.h"     // research/137 N8: occlusionbatch.toggle
#include "llrhiprofiler.h"
#include "llrhiresource.h"     // research/197 stage 1b: vbhandles.toggle
#include "llrhishaderdump.h"
#include "llrhiupscale.h"      // research/165: upscale.toggle
#include "llrhiuploadtrace.h"
#include "llstring.h"
#include "llrhivkmirror.h"
#if LL_WINDOWS
#include "llwin32headers.h"     // FindFirstFileW (pollCueToggles), GetCommandLineW
#endif

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// research/303: see llrhi.h.
bool LLRHI::windowOnADisplay(void* window, bool remote_session_blocks)
{
#if LL_WINDOWS
    const HWND hwnd = (HWND)window;
    if (!hwnd || !IsWindow(hwnd))
    {
        return true;
    }
    if (!MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) || (remote_session_blocks && GetSystemMetrics(SM_REMOTESESSION)))
    {
        return false;
    }
    const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!desktop)
    {
        return false;
    }
    CloseDesktop(desktop);
#else
    (void)window;
    (void)remote_session_blocks;
#endif
    return true;
}

// The switches' readings (llrhi.h). std::getenv only (research/149 (P)).
bool LLRHI::envDefaultOn(const char* name)
{
    const char* env = std::getenv(name);
    return !env || strcmp(env, "0");
}

bool LLRHI::envSet(const char* name)
{
    const char* env = std::getenv(name);
    return env && *env && strcmp(env, "0");
}

bool LLRHI::envIs(const char* name, const char* value)
{
    const char* env = std::getenv(name);
    return env && !strcmp(env, value);
}

bool LLRHI::envUnset(const char* name)
{
    const char* env = std::getenv(name);
    return !env || !*env;
}

char LLRHI::envFirstChar(const char* name)
{
    const char* env = std::getenv(name);
    return env ? *env : '\0';
}

namespace
{
    // envNumber's refusals. Most switches are read in static initializers,
    // before logging starts: those are held until envLoggingStarts() logs them.
    // A function's static, so that a static initializer of any file finds it made.
    struct EnvRefusals
    {
        std::mutex mutex;
        bool logging = false;
        std::vector<std::string> held;
    };
    EnvRefusals& env_refusals()
    {
        static EnvRefusals refusals;
        return refusals;
    }
}

bool LLRHI::envNumber(const char* name, U64 min, U64 max, U64& out)
{
    const char* env = std::getenv(name);
    if (!env || !*env)
    {
        return false;
    }
    // strtoull alone would take leading spaces, a sign (a "-1" wrapped round),
    // a tail ("10abc" as 10) and a second 0x after the first: the value must be
    // digits of its base only, all of it.
    const bool hex = env[0] == '0' && (env[1] == 'x' || env[1] == 'X');
    const char* digits = hex ? env + 2 : env;
    bool whole = *digits != '\0';
    for (const char* c = digits; whole && *c; ++c)
    {
        whole = hex ? std::isxdigit((unsigned char)*c) != 0 : (*c >= '0' && *c <= '9');
    }
    errno = 0;
    const unsigned long long number = whole ? std::strtoull(digits, nullptr, hex ? 16 : 10) : 0;
    if (!whole || errno == ERANGE || number < min || number > max)
    {
        std::string line = std::string(name) + "=" + env + " is not a whole number from " + std::to_string(min) + " to " +
                           std::to_string(max) + ": " + std::to_string(out) + " kept";
        EnvRefusals& refusals = env_refusals();
        std::lock_guard<std::mutex> lock(refusals.mutex);
        if (refusals.logging)
        {
            LL_WARNS("RHI") << line << LL_ENDL;
        }
        else
        {
            refusals.held.push_back(std::move(line));
        }
        return false;
    }
    out = (U64)number;
    return true;
}

void LLRHI::envLoggingStarts()
{
    EnvRefusals& refusals = env_refusals();
    std::lock_guard<std::mutex> lock(refusals.mutex);
    refusals.logging = true;
    for (const std::string& line : refusals.held)
    {
        LL_WARNS("RHI") << line << LL_ENDL;
    }
    refusals.held.clear();
}

namespace
{
    // Unset, empty or "auto": the switches below with an auto mode (verify first).
    bool auto_env(const char* name)
    {
        return LLRHI::envUnset(name) || LLRHI::envIs(name, "auto");
    }
}

bool LLRHI::sDepthOnlyPass = false;
U32 LLRHI::sOcclusionScope = 0;
bool (*LLRHI::sDrawShadowPackets)(const LLRHI::ShadowPacket*, size_t, const F32*, F32, const LLRHI::ShadowListPass*) = nullptr;
LLRHI::ShadowList* LLRHI::sShadowList = nullptr;
LLRHI::ProbeList* LLRHI::sProbeList = nullptr;
bool LLRHI::sGLDrawsDisabled = false;
bool LLRHI::sVulkanGL = false;
void (*LLRHI::sVulkanUnavailableExit)(LLRHI::VulkanUnavailable, const std::string&) = nullptr;
bool (*LLRHI::sPresentWindow)(void*, U32, U32, U32) = nullptr;
F32 (*LLRHI::sGPUBenchmark)() = nullptr;
void (*LLRHI::sForgetWindow)(void*) = nullptr;
std::string (*LLRHI::sDescribeDrawTarget)() = nullptr;
bool (*LLRHI::sDirectDraw)(const LLRHI::DirectDraw&) = nullptr;
void (*LLRHI::sActiveTexture)(U32) = nullptr;
void (*LLRHI::sBindTexture)(U32, U32, U32) = nullptr;
void (*LLRHI::sTexParameters)(U32, const LLRHI::TexParameter*, U32) = nullptr;
bool (*LLRHI::sTexImage2D)(U32, U32, S32, U32, S32, S32, U32, U32, const void*) = nullptr;
bool (*LLRHI::sCompressedTexImage2D)(U32, U32, S32, U32, S32, S32, S32, const void*) = nullptr;
bool (*LLRHI::sTexSubImage2D)(U32, U32, S32, S32, S32, S32, S32, U32, U32, const void*) = nullptr;
bool (*LLRHI::sGenerateMipmap)(U32, U32) = nullptr;
bool (*LLRHI::sCopyTexSubImage3D)(U32, U32, S32, S32, S32, S32, S32, S32, S32, S32) = nullptr;
void (*LLRHI::sBeginQuery)(U32, U32) = nullptr;
void (*LLRHI::sEndQuery)(U32) = nullptr;
void (*LLRHI::sGetQueryObjectuiv)(U32, U32, U32*) = nullptr;
void (*LLRHI::sBindBufferBase)(U32, U32, U32) = nullptr;
bool (*LLRHI::sTexStorage2D)(U32, U32, S32, U32, S32, S32, U32, U32) = nullptr;
bool (*LLRHI::sCopyTexSubImage2D)(U32, U32, S32, S32, S32, S32, S32, S32, S32) = nullptr;
bool (*LLRHI::sTexScaleDown)(U32, U32, U32, S32, S32) = nullptr;
void (*LLRHI::sPixelStorei)(U32, S32) = nullptr;
bool LLRHI::sDirect = LLRHI::envDefaultOn("LLRHI_DIRECT");
bool LLRHI::sLightBatch = LLRHI::envDefaultOn("LLRHI_LIGHT_BATCH");
bool (*LLRHI::sLightTableProgram)(U32) = nullptr;
// Off unless set (and not 0) until the viewer's settings apply (setOITMode).
bool LLRHI::sOIT = LLRHI::envSet("LLRHI_OIT");
// Off unless set (and not 0): full OIT is the default (research/55: at
// Le'eaf, the heaviest alpha, it costs 3 ms of GPU to the hybrid's 1).
bool LLRHI::sOITHybrid = LLRHI::envSet("LLRHI_OIT_HYBRID");
// Kept from the start only when LLRHI_OIT turns OIT on (LLRHI_OIT=0 forces it
// off, so no variant would ever be used); a setting that may use OIT sets it
// later (handleRenderOITChanged, which asks oitForcedOff too).
bool LLRHI::sOITSources = LLRHI::envSet("LLRHI_OIT");

bool LLRHI::oitForcedOff()
{
    // As setOITMode reads it: set (also empty) wins over the settings, and
    // only a value envSet takes turns OIT on.
    static const bool off = std::getenv("LLRHI_OIT") != nullptr && !LLRHI::envSet("LLRHI_OIT");
    return off;
}
bool (*LLRHI::sOITSupported)() = nullptr;
bool (*LLRHI::sOITSnapshotCheck)() = nullptr;

void LLRHI::setOITMode(U32 mode)
{
    // The env vars win (tests): read once, as the flags' own defaults are.
    static const bool oit_env = std::getenv("LLRHI_OIT") != nullptr;
    static const bool hybrid_env = std::getenv("LLRHI_OIT_HYBRID") != nullptr;
    if (!oit_env)
    {
        sOIT = mode != 0;
    }
    if (!hybrid_env)
    {
        sOITHybrid = mode != 2;
    }
}
bool (*LLRHI::sOITBegin)(F32, F32, bool) = nullptr;
void (*LLRHI::sOITWorld)() = nullptr;
bool (*LLRHI::sOITVariant)(U32) = nullptr;
U32 LLRHI::sOITPriority = 0;
void (*LLRHI::sOITResolve)() = nullptr;
F32* (*LLRHI::sTableRows)(U32, U32&) = nullptr;
bool LLRHI::sSuppressTranslation = false;
void (*LLRHI::sBufferData)(U32, size_t, const void*) = nullptr;
void (*LLRHI::sBufferSubData)(U32, size_t, size_t, const void*) = nullptr;

LLRHI::ShaderProgram* (*LLRHI::sShaderProgram)(U32) = nullptr;
S32 (*LLRHI::sUniformSlot)(LLRHI::ShaderProgram*, S32) = nullptr;
void (*LLRHI::sUseProgram)(LLRHI::ShaderProgram*, U32) = nullptr;
bool (*LLRHI::sSetUniform)(LLRHI::ShaderProgram*, S32, U32, U32, const void*) = nullptr;
bool (*LLRHI::sSetUniformMatrix)(LLRHI::ShaderProgram*, S32, U32, U32, U32, bool, const F32*) = nullptr;
bool LLRHI::sNativeShader = LLRHI::envDefaultOn("LLRHI_NATIVE_SHADER");
// On unless set to 0 (research/56): the same values in the same table.
bool LLRHI::sPalettePrebuild = LLRHI::envDefaultOn("LLRHI_PALETTE_PREBUILD");
bool (*LLRHI::sPlacePalette)(const F32*, U32, LLRHI::PaletteRow&) = nullptr;
bool (*LLRHI::sUsePaletteRow)(LLRHI::ShaderProgram*, const LLRHI::PaletteRow&) = nullptr;
// Off unless set (and not 0): the avatar crowd counters (research/56).
bool LLRHI::sAvatarStats = LLRHI::envSet("LLRHI_AVATAR_STATS");
LLRHI::AvatarCounters LLRHI::sAvatarCounters;
// On unless set to 0 (research/63): the same joint values in the same order.
bool LLRHI::sAvatarThreads = LLRHI::envDefaultOn("LLRHI_AVATAR_THREADS");
bool LLRHI::sAvatarThreadsValidate = LLRHI::envIs("LLRHI_AVATAR_THREADS", "2");
bool LLRHI::sAvatarBlendLookup = LLRHI::envDefaultOn("LLRHI_AVATAR_BLEND_LOOKUP");
std::string (*LLRHI::sAvatarBackendReport)(U32) = nullptr;

void (*LLRHI::sSetCap)(U32, bool) = nullptr;
void (*LLRHI::sBlendFunc)(U32, U32, U32, U32) = nullptr;
void (*LLRHI::sColorMask)(bool, bool, bool, bool) = nullptr;
void (*LLRHI::sDepthFunc)(U32) = nullptr;
void (*LLRHI::sDepthMask)(bool) = nullptr;
void (*LLRHI::sLineWidth)(F32) = nullptr;
void (*LLRHI::sCullFace)(U32) = nullptr;
void (*LLRHI::sViewport)(S32, S32, S32, S32) = nullptr;
void (*LLRHI::sScissor)(S32, S32, S32, S32) = nullptr;
void (*LLRHI::sPolygonOffset)(F32, F32) = nullptr;
void (*LLRHI::sPolygonMode)(U32, U32) = nullptr;
void (*LLRHI::sClearColor)(F32, F32, F32, F32) = nullptr;
void (*LLRHI::sBindFramebuffer)(U32, U32) = nullptr;
void (*LLRHI::sDrawBuffers)(S32, const U32*) = nullptr;
void (*LLRHI::sDrawBuffer)(U32) = nullptr;
void (*LLRHI::sReadBuffer)(U32) = nullptr;
void (*LLRHI::sClear)(U32) = nullptr;
bool LLRHI::sNativeRiggedShadows = LLRHI::envDefaultOn("LLRHI_NATIVE_RIGGED");
bool LLRHI::sNativeShadows = LLRHI::envDefaultOn("LLRHI_NATIVE_SHADOWS");
bool LLRHI::sNativeTerrainShadows = LLRHI::envDefaultOn("LLRHI_NATIVE_TERRAIN");
bool LLRHI::sMergeShadowPackets = LLRHI::envDefaultOn("LLRHI_SHADOW_PACKET_MERGE");
// On unless LLRHI_GPU_DRIVEN=0 (the everyday configuration, decided 2026-10-16;
// the whole GPU-driven family below and DRAW_TEMPLATES / PROBE_SHADOW_CACHE).
bool LLRHI::sGPUDriven = LLRHI::envDefaultOn("LLRHI_GPU_DRIVEN");
bool LLRHI::sGPUDrivenList = LLRHI::envDefaultOn("LLRHI_GPU_DRIVEN_LIST");
bool LLRHI::sGPUDrivenCull = LLRHI::envDefaultOn("LLRHI_GPU_DRIVEN_CULL");
bool LLRHI::sGPUDrivenProbe = LLRHI::envDefaultOn("LLRHI_GPU_DRIVEN_PROBE");
// The device sets it to sGPUDrivenDGCChoice's answer at startup; until then,
// the environment's value or on.
bool LLRHI::sGPUDrivenDGC = LLRHI::envDefaultOn("LLRHI_GPU_DRIVEN_DGC");
// Auto unless LLRHI_GPU_DRIVEN_DGC is set; plus/gpudgc then hands over the
// setting PlusGpuDrivenDGC (before the device is made).
LLRHI::DGCChoice LLRHI::sGPUDrivenDGCChoice = LLRHI::envUnset("LLRHI_GPU_DRIVEN_DGC") ? LLRHI::DGCChoice::Auto
                                              : LLRHI::envSet("LLRHI_GPU_DRIVEN_DGC") ? LLRHI::DGCChoice::EnvOn
                                                                                      : LLRHI::DGCChoice::EnvOff;
std::atomic<bool> LLRHI::sGPUDrivenDGCUsed{ false };

// Set once by LLVKDevice::createDevice (research/230 R1); main thread reads.
bool LLRHI::sRayTracing = false;
static std::string sRayTracingStatus = "not checked";
const char* LLRHI::rayTracingStatus() { return sRayTracingStatus.c_str(); }
bool (*LLRHI::sRayTracingSource)(LLRHI::RayTracingWanted&) = nullptr;
void LLRHI::setRayTracingStatus(bool on, const std::string& why) { sRayTracing = on; sRayTracingStatus = why; }
// Stage G4b (research/46): LLRHI_DRAW_RUNS=1 / 0 decides; unset, it follows
// LLRHI_GPU_DRIVEN (on by default).
bool LLRHI::sDrawRuns = LLRHI::envUnset("LLRHI_DRAW_RUNS") ? LLRHI::sGPUDriven : LLRHI::envSet("LLRHI_DRAW_RUNS");
// Stage G4c (research/46 §11): on unless LLRHI_DRAW_TEMPLATES=0; only acts while
// sDrawRuns is on.
bool LLRHI::sDrawTemplates = LLRHI::envDefaultOn("LLRHI_DRAW_TEMPLATES");
bool (*LLRHI::sDrawTemplate)(const LLRHI::DirectDraw&) = nullptr;
void (*LLRHI::sDrawTemplateReleased)(U32, const U32*) = nullptr;
// research/180 stage 4 P1 (packet-p1): LLRHI_PACKETS auto (unset, the default; packet-p1c) / 0 / 1 / verify / census.
U32 (*LLRHI::sPacketDraw)(LLRHI::Packet&) = nullptr;
thread_local const LLRHI::PacketNote* LLRHI::tPacketNote = nullptr;
bool LLRHI::sPackets = LLRHI::envIs("LLRHI_PACKETS", "1") || LLRHI::envIs("LLRHI_PACKETS", "on") || auto_env("LLRHI_PACKETS");
bool LLRHI::sPacketsVerify = LLRHI::envIs("LLRHI_PACKETS", "verify") || auto_env("LLRHI_PACKETS");
bool LLRHI::sPacketsCensus = LLRHI::envIs("LLRHI_PACKETS", "census");
bool LLRHI::sPacketsAuto = auto_env("LLRHI_PACKETS");
bool LLRHI::sPacketObjects = LLRHI::envDefaultOn("LLRHI_PACKET_OBJECTS");
bool LLRHI::sPacketShort = LLRHI::envDefaultOn("LLRHI_PACKET_SHORT");
LLRHI::PacketCensus LLRHI::sPacketCensus;
void (*LLRHI::sDrawRunPrograms)(U32&, U32&, U32&) = nullptr;
bool (*LLRHI::sParallelShaderReload)(void (*)()) = nullptr;
// research/111 B-PW: on unless LLRHI_SHADER_PREWARM=0.
bool LLRHI::sShaderPrewarm = LLRHI::envDefaultOn("LLRHI_SHADER_PREWARM");
void (*LLRHI::sPrewarmShaders)(const std::map<std::string, std::string>&, const std::vector<std::map<std::string, std::string>>&) = nullptr;
bool (*LLRHI::sBakeShaders)(void (*)(), bool, bool, LLRHI::ShaderBakeCounts&) = nullptr;   // research/159 3.1
LLRHI::ShaderReloadBuilder* LLRHI::sShaderReloadBuilder = nullptr;   // research/250

// On unless set to 0: checked at sui and the heavy place (research/19, 20).
bool LLRHI::sShareProbeShadows = LLRHI::envDefaultOn("LLRHI_PROBE_SHADOW_SHARE");
// On unless LLRHI_PROBE_SHADOW_CACHE=0 (research/45): costs two depth images of
// the shared maps' size.
bool LLRHI::sCacheProbeShadows = LLRHI::envDefaultOn("LLRHI_PROBE_SHADOW_CACHE");
bool LLRHI::sProbeFast = LLRHI::envDefaultOn("LLRHI_PROBE_FAST");
bool LLRHI::sAlphaFast = LLRHI::envDefaultOn("LLRHI_ALPHA_FAST");
bool LLRHI::sSlotCache = LLRHI::envDefaultOn("LLRHI_SLOT_CACHE");
bool LLRHI::sPassUnitsRecheck = LLRHI::envDefaultOn("LLRHI_PASS_UNITS");
namespace
{
    // research/180 stage 1: LLRHI_SLOT_PUBLISH unset (or "auto"): verify first (sSlotPublishAuto).
    bool slot_publish_auto_env()
    {
        return auto_env("LLRHI_SLOT_PUBLISH") && !LLRHI::envIs("LLRHI_SLOT_PUBLISH_VERIFY", "1");
    }
}
bool LLRHI::sSlotPublish = LLRHI::envDefaultOn("LLRHI_SLOT_PUBLISH");
bool LLRHI::sSlotPublishVerify = LLRHI::envIs("LLRHI_SLOT_PUBLISH_VERIFY", "1") || LLRHI::envIs("LLRHI_SLOT_PUBLISH", "verify") || slot_publish_auto_env();
bool LLRHI::sSlotPublishAuto = slot_publish_auto_env();
bool LLRHI::sWorldShown = false;
bool LLRHI::sPoolLazyForget = LLRHI::envDefaultOn("LLRHI_POOL_LAZY_FORGET");
bool LLRHI::sVBSourceStreams = LLRHI::envDefaultOn("LLRHI_VB_SOURCE_STREAMS");     // research/293 branch 1
// research/197 stage 1a: LLRHI_TEX_HANDLES on (unset, the default since
// research/218 vk-s7f; was auto) / 0 / 1 / verify / auto (verify first, as before).
namespace
{
    LLRHI::Textures::Backend sTexturesBackend;
}
bool LLRHI::sTexHandles = LLRHI::envUnset("LLRHI_TEX_HANDLES") || LLRHI::envIs("LLRHI_TEX_HANDLES", "1") || LLRHI::envIs("LLRHI_TEX_HANDLES", "on") ||
                          LLRHI::envIs("LLRHI_TEX_HANDLES", "verify") || LLRHI::envIs("LLRHI_TEX_HANDLES", "auto");
bool LLRHI::sTexHandlesVerify = LLRHI::envIs("LLRHI_TEX_HANDLES", "verify") || LLRHI::envIs("LLRHI_TEX_HANDLES", "auto");
bool LLRHI::sTexHandlesAuto = LLRHI::envIs("LLRHI_TEX_HANDLES", "auto");

LLRHI::TexRef LLRHI::Textures::create(const TextureDesc& desc)
{
    return sTexturesBackend.create ? sTexturesBackend.create(desc) : TexRef();
}

void LLRHI::Textures::replaceImage(TexRef ref, U32 image)
{
    if (ref.id && sTexturesBackend.replaceImage)
    {
        sTexturesBackend.replaceImage(ref, image);
    }
}

void LLRHI::Textures::retire(TexRef ref)
{
    if (ref.id && sTexturesBackend.retire)
    {
        sTexturesBackend.retire(ref);
    }
}

void LLRHI::Textures::setAnisotropy(F32 level)
{
    if (sTexturesBackend.setAnisotropy)
    {
        sTexturesBackend.setAnisotropy(level);
    }
}

bool LLRHI::Textures::verify(TexRef ref, U32 image, U16 sampler)
{
    return sTexturesBackend.verify && sTexturesBackend.verify(ref, image, sampler);
}

void LLRHI::Textures::install(const Backend& backend)
{
    sTexturesBackend = backend;
}

// research/197 stage 2 (research/198 3.1): who hears that a number's sampler changed.
namespace
{
    std::atomic<LLRHI::Textures::SamplerListener> sSamplerListener{ nullptr };
}

void LLRHI::Textures::samplerChanged(TexRef ref)
{
    const SamplerListener listener = sSamplerListener.load(std::memory_order_acquire);
    if (ref.id && listener)
    {
        listener(ref);
    }
}

void LLRHI::Textures::setSamplerListener(SamplerListener listener)
{
    sSamplerListener.store(listener, std::memory_order_release);
}
bool LLRHI::sMaterialFast = LLRHI::envDefaultOn("LLRHI_MATERIAL_FAST");
bool LLRHI::sProbeFuse = LLRHI::envDefaultOn("LLRHI_PROBE_FUSE");
bool LLRHI::sCullThreads = LLRHI::envDefaultOn("LLRHI_CULL_THREADS");
// research/184 A1: LLRHI_CULL_PAR=auto: verify first (sCullParAuto). Unset: off
// (research/184 9: cull -0.02 to -0.10 ms, less than the auto mode's verify costs).
bool LLRHI::sCullPar = LLRHI::envSet("LLRHI_CULL_PAR");
bool LLRHI::sCullParVerify = LLRHI::envIs("LLRHI_CULL_PAR", "verify") || LLRHI::envIs("LLRHI_CULL_PAR", "auto");
bool LLRHI::sCullParAuto = LLRHI::envIs("LLRHI_CULL_PAR", "auto");
namespace
{
    // research/188: LLRHI_CULL_REUSE unset or auto: verify first (sCullReuseAuto);
    // LLRHI_CULL_REUSE_VERIFY=1 (not 0): verify only.
    bool cull_reuse_auto_env()
    {
        return auto_env("LLRHI_CULL_REUSE") && !LLRHI::envSet("LLRHI_CULL_REUSE_VERIFY");
    }
}
bool LLRHI::sCullReuse = LLRHI::envDefaultOn("LLRHI_CULL_REUSE") || LLRHI::envSet("LLRHI_CULL_REUSE_VERIFY");
bool LLRHI::sCullReuseVerify = LLRHI::envIs("LLRHI_CULL_REUSE", "verify") || LLRHI::envSet("LLRHI_CULL_REUSE_VERIFY") || cull_reuse_auto_env();
bool LLRHI::sCullReuseAuto = cull_reuse_auto_env();
bool LLRHI::sShadowSortThreads = LLRHI::envDefaultOn("LLRHI_SHADOW_SORT_THREADS");
bool LLRHI::sShadowListThreads = LLRHI::envDefaultOn("LLRHI_SHADOW_LIST_THREADS");
bool LLRHI::sProbeSharedCull = LLRHI::envDefaultOn("LLRHI_PROBE_SHARED_CULL");
// Off unless LLRHI_PROBE_STATIC_CACHE is set (and not 0) while being validated.
bool LLRHI::sProbeStaticCache = LLRHI::envSet("LLRHI_PROBE_STATIC_CACHE");
bool (*LLRHI::sObjectVersions)(const U32*, U32, const U32*, U32, U64&) = nullptr;
void (*LLRHI::sBackendCounters)(U32&, U32&, U32&) = nullptr;
std::string (*LLRHI::sMemoryReport)() = nullptr;
void (*LLRHI::sNoteEvent)(const char*) = nullptr;
void (*LLRHI::sResourceCounts)(U64&, U64&, U64&) = nullptr;
void (*LLRHI::sDumpAllocations)(const std::string&) = nullptr;
bool (*LLRHI::sVRAMBudget)(U64&, U64&, U64&) = nullptr;
bool LLRHI::sVRAMBudgetReal = LLRHI::envDefaultOn("LLRHI_VK_BUDGET_REAL");
void (*LLRHI::sTextureImageBytes)(const U32*, U32, U64*) = nullptr;
std::string (*LLRHI::sPoolDetail)() = nullptr;
std::atomic<bool> LLRHI::sBufferCompact{ false };     // research/245: plus/tplag sets it
std::atomic<F32> LLRHI::sBufferCompactIdleSeconds{ 20.f };
void (*LLRHI::sBufferCompactCounts)(LLRHI::BufferCompactCounts&) = nullptr;
bool LLRHI::sTexCompact = LLRHI::envDefaultOn("LLRHI_TEX_COMPACT");     // research/285 X2
std::atomic<U64> LLRHI::sVRAMBiasFallBytes{ 0 };                        // research/285 compact-hysteresis
std::atomic<U64> LLRHI::sVRAMBiasRiseBytes{ 0 };                        // research/285 compact-hysteresis
bool LLRHI::sTexPressure = LLRHI::envDefaultOn("LLRHI_TEX_PRESSURE");
U32 LLRHI::sTexCensusSeconds = []() -> U32
{
    U64 value = 0;
    LLRHI::envNumber("LLRHI_TEX_CENSUS", 0, U32_MAX, value);
    return value == 0 ? 0 : value == 1 ? 120 : value < 10 ? 10 : (U32)value;
}();
bool LLRHI::sTextureBudget = LLRHI::envDefaultOn("LLRHI_TEXTURE_BUDGET");
bool LLRHI::sTextureVisitThreads = LLRHI::envDefaultOn("LLRHI_TEXTURE_VISIT_THREADS");
// Off unless set (and not 0).
bool LLRHI::sTextureVisitStats = LLRHI::envSet("LLRHI_TEXTURE_VISIT_STATS");
// idle-skip (research/178 G1, D1, D2): on unless set to 0; the counters off unless set.
bool LLRHI::sTextureVisitPace = LLRHI::envDefaultOn("LLRHI_TEXTURE_VISIT_PACE");
bool LLRHI::sIdleSkipTexAnim = LLRHI::envDefaultOn("LLRHI_IDLE_SKIP_TEXANIM");
bool LLRHI::sIdleSkipOmega = LLRHI::envDefaultOn("LLRHI_IDLE_SKIP_OMEGA");
bool LLRHI::sIdleSkipStats = LLRHI::envSet("LLRHI_IDLE_SKIP_STATS");
// research/180 10.12 R1: the orphan reunion leaves a still static parent static; off unless set to 1.
bool LLRHI::sActiveOrphan = LLRHI::envIs("LLRHI_ACTIVE_ORPHAN", "1");
bool LLRHI::sActiveOrphanVerify = LLRHI::envDefaultOn("LLRHI_ACTIVE_ORPHAN_VERIFY");
// research/180 10.15: a static drawable whose move changes nothing stays static; off unless set to 1.
bool LLRHI::sMoveSame = LLRHI::envIs("LLRHI_MOVE_SAME", "1");
bool LLRHI::sMoveSameVerify = LLRHI::envDefaultOn("LLRHI_MOVE_SAME_VERIFY");
std::atomic<U32> LLRHI::sOldMaterialNames{ 0 };  // research/218 vk-s7f
std::atomic<U32> LLRHI::sTextureImageFailures{ 0 };  // research/289 (materialize integration check L10)
std::atomic<U32> LLRHI::sTargetImageFailures{ 0 };
// research/180 9 3: the tracker hold off unless set.
bool LLRHI::sTrackerHold = LLRHI::envSet("LLRHI_TRACKER_HOLD");
// research/180 9 1: the seen memo on unless set to 0; its verify off unless set.
bool LLRHI::sIdleSkipMemo = LLRHI::envDefaultOn("LLRHI_IDLE_SKIP_MEMO");
bool LLRHI::sIdleSkipMemoVerify = LLRHI::envSet("LLRHI_IDLE_SKIP_MEMO_VERIFY");
// research/180 10.18 (seenby): on unless set to 0.
bool LLRHI::sSeenByMemo = LLRHI::envDefaultOn("LLRHI_SEEN_BY_MEMO");
// research/193 (idle-cheap): the idle census off unless set; the parameter
// memo LLRHI_VPARAM_MEMO auto (unset, the default) / 0 / 1 / verify.
bool LLRHI::sIdleCensus = LLRHI::envSet("LLRHI_IDLE_CENSUS");
bool LLRHI::sVParamMemo = LLRHI::envIs("LLRHI_VPARAM_MEMO", "1") || LLRHI::envIs("LLRHI_VPARAM_MEMO", "on") || auto_env("LLRHI_VPARAM_MEMO");
bool LLRHI::sVParamMemoVerify = LLRHI::envIs("LLRHI_VPARAM_MEMO", "verify") || auto_env("LLRHI_VPARAM_MEMO");
bool LLRHI::sVParamMemoAuto = auto_env("LLRHI_VPARAM_MEMO");
// research/179 (cpu-present): the texture animation matrix copy on unless set
// to 0; its verify off unless set.
bool LLRHI::sTexAnimMemo = LLRHI::envDefaultOn("LLRHI_TEXANIM_MEMO");
bool LLRHI::sTexAnimMemoVerify = LLRHI::envSet("LLRHI_TEXANIM_MEMO_VERIFY");
// research/194 11: the texture scroll descriptions LLRHI_TEXANIM_GPU auto
// (unset, the default) / 0 / 1 / verify.
bool LLRHI::sTexAnimGpu = LLRHI::envIs("LLRHI_TEXANIM_GPU", "1") || LLRHI::envIs("LLRHI_TEXANIM_GPU", "on") || auto_env("LLRHI_TEXANIM_GPU");
bool LLRHI::sTexAnimGpuVerify = LLRHI::envIs("LLRHI_TEXANIM_GPU", "verify") || auto_env("LLRHI_TEXANIM_GPU");
bool LLRHI::sTexAnimGpuAuto = auto_env("LLRHI_TEXANIM_GPU");
double LLRHI::sTexAnimTime = 0.0;
std::atomic<U32> LLRHI::sTexAnimDecodes{ 0 };
// research/203 (stage 3c): the local light table LLRHI_LIGHT_TABLE auto
// (unset, the default) / 0 / off / 1 / on / verify.
bool LLRHI::sLightTable = LLRHI::envIs("LLRHI_LIGHT_TABLE", "1") || LLRHI::envIs("LLRHI_LIGHT_TABLE", "on") || auto_env("LLRHI_LIGHT_TABLE");
bool LLRHI::sLightTableVerify = LLRHI::envIs("LLRHI_LIGHT_TABLE", "verify") || auto_env("LLRHI_LIGHT_TABLE");
bool LLRHI::sLightTableAuto = auto_env("LLRHI_LIGHT_TABLE");
// research/187 N4: the memo on unless set to 0; its verify off unless set.
bool LLRHI::sEnvUniformMemo = LLRHI::envDefaultOn("LLRHI_ENV_UNIFORM_MEMO");
bool LLRHI::sEnvUniformMemoVerify = LLRHI::envSet("LLRHI_ENV_UNIFORM_MEMO_VERIFY");
// alpha-cpu (research/178 F0): off unless set (heavy-extreme: alpha +0.4 ms
// with it on, research/178 s11); the counters off unless set.
bool LLRHI::sAlphaGlowSkip = LLRHI::envSet("LLRHI_ALPHA_GLOW_SKIP");
bool LLRHI::sAlphaPassStats = LLRHI::envSet("LLRHI_ALPHA_PASS_STATS");
bool LLRHI::sTexDirect = LLRHI::envDefaultOn("LLRHI_TEX_DIRECT");
bool LLRHI::sTexBatch = LLRHI::envDefaultOn("LLRHI_TEX_BATCH");
bool LLRHI::sTexLazyAttach = LLRHI::envDefaultOn("LLRHI_TEX_LAZY_ATTACH");
// Off unless set (and not 0).
bool LLRHI::sTexStats = LLRHI::envSet("LLRHI_TEX_STATS");
thread_local bool LLRHI::tTexConvertDefer = false;
void (*LLRHI::sTexConvertJoin)(LLRHI::RunJobs, U32, const std::function<void(U32)>&) = nullptr;
std::atomic<U64> LLRHI::sTexConvertBytes{ 0 };
std::atomic<U64> LLRHI::sTexConvertDeferredBytes{ 0 };
std::atomic<U64> LLRHI::sTexConvertPendingBytes{ 0 };
bool LLRHI::sTexBCN = LLRHI::envDefaultOn("LLRHI_TEX_BCN");
thread_local bool LLRHI::tTexCompress = false;
void (*LLRHI::sTextureCompressed)(const U32*, U32, bool*) = nullptr;
void (*LLRHI::sTextureMoveHold)(U32, bool) = nullptr;     // research/285 compact-hysteresis
std::string (*LLRHI::sTexBCNReport)() = nullptr;
bool LLRHI::sGeomBudget = LLRHI::envDefaultOn("LLRHI_GEOM_BUDGET");
bool LLRHI::sPriorityBudget = LLRHI::envDefaultOn("LLRHI_PRIORITY_BUDGET");
bool LLRHI::sMeshNotifyFast = LLRHI::envDefaultOn("LLRHI_MESH_NOTIFY_FAST");
bool LLRHI::sCacheSunShadows = LLRHI::envDefaultOn("LLRHI_SHADOW_CACHE");
bool LLRHI::sMergeShadowCascades = LLRHI::sCacheSunShadows || LLRHI::envSet("LLRHI_SHADOW_MERGE");

// Off unless set (and not 0): timestamps cost a little GPU time (research/53).
bool LLRHI::sGPUTiming = LLRHI::envSet("LLRHI_GPU_TIMING");
// fix-logcache: on for development starts (llrhi.h): LLRHI_PERIODIC_LOGS decides;
// unset, on when LLRHI_VK_PERF or LLRHI_CUE_DIR is set.
bool LLRHI::sPeriodicLogs = LLRHI::envUnset("LLRHI_PERIODIC_LOGS") ? !LLRHI::envUnset("LLRHI_VK_PERF") || !LLRHI::envUnset("LLRHI_CUE_DIR")
                                                                   : LLRHI::envSet("LLRHI_PERIODIC_LOGS");
// research/276 stage 1: set by plus/fastexit (llrhi.h).
bool LLRHI::sExitTimeLog = false;
U64 LLRHI::sExitTimeStartNs = 0;
std::atomic<bool> LLRHI::sQuitting{ false };    // research/285 compact-hysteresis: plus/fastexit sets it
const char* LLRHI::sGPUScope = nullptr;
void (*LLRHI::sGPUMark)(const char*, const char*) = nullptr;
// Off unless set (and not 0): frame cost accounting (research/83).
bool LLRHI::sFrameCost = LLRHI::envSet("LLRHI_FRAME_COST");
// =2: CPU only, with the finer segments (research/113).
bool LLRHI::sFrameCostDetail = LLRHI::envIs("LLRHI_FRAME_COST", "2");
// research/208 S0: the render thread census (llrhiframe.h), timing only.
bool LLRHI::sRenderThreadCensus = LLRHI::envSet("LLRHI_RENDERTHREAD_CENSUS");
void (*LLRHI::sCPUMark)(const char*) = nullptr;
LLRHI::ImmediateCounts LLRHI::sImmediate;
// On unless 0: only matters while GPU timing is on.
bool LLRHI::sGPUTimingAlpha = LLRHI::envDefaultOn("LLRHI_GPU_TIMING_ALPHA");
// On unless 0: VSync frame pacing (research/94).
bool LLRHI::sFramePacing = LLRHI::envDefaultOn("LLRHI_FRAME_PACING");
bool LLRHI::sFramePacingGPUBound = LLRHI::envDefaultOn("LLRHI_FRAME_PACING_GPU_BOUND");
// research/179 6.6 4 (cpu-present): on unless LLRHI_PRESENT_THREAD=0 (the default since 2026-10-26,
// research/179 9: light -0.76 ms, heavy-leeaf-2 no change, no validation warning, resize and minimize fine).
bool LLRHI::sPresentThread = LLRHI::envDefaultOn("LLRHI_PRESENT_THREAD");
bool LLRHI::sPresentMailbox = LLRHI::envIs("LLRHI_PRESENT_MODE", "mailbox");

// Off unless set (and not 0): the alpha census (research/54).
bool LLRHI::sAlphaCensus = LLRHI::envSet("LLRHI_ALPHA_CENSUS");
U32 LLRHI::sCensusTag = 0;
bool (*LLRHI::sCensusTake)(std::vector<LLRHI::CensusCount>&, U32&, U32&) = nullptr;

// On unless set to 0: the same image by construction (research/61).
bool LLRHI::sZeroCoverageCull = LLRHI::envDefaultOn("LLRHI_ZERO_COVERAGE_CULL");
bool LLRHI::sShadowReceiverCull = LLRHI::envDefaultOn("LLRHI_SHADOW_RECEIVER_CULL");
// Anisotropic shadow maps: not proven exact (research/64), on by choice.
// On unless LLRHI_SHADOW_RECEIVER_CULL_ANISO=0: chosen 2026-10-11,
// with anisotropic shadow maps a few silhouette pixels may differ (research/64).
bool LLRHI::sShadowReceiverCullAniso = LLRHI::envDefaultOn("LLRHI_SHADOW_RECEIVER_CULL_ANISO");
// On unless set to 0: the same groups in the same order by construction (research/113).
bool LLRHI::sShadowSameInput = LLRHI::envDefaultOn("LLRHI_SHADOW_SAME_INPUT");

// The sun shadow casters' depth bias (plus/shadowdist): set by the pipeline around them.
LLRHI::ShadowDepthBias LLRHI::sShadowDepthBias;
bool LLRHI::sShadowDepthBiasClamp = false;
LLRHI::ShadowSkinnedCounts LLRHI::sShadowSkinned;

// Off unless set (and not 0): counting reads every tested group's draw map.
bool LLRHI::sTinyStats = LLRHI::envSet("LLRHI_TINY_STATS");
// On unless set to 0: nothing splices unless asked (research/66 step 1).
bool LLRHI::sRecordSplice = LLRHI::envDefaultOn("LLRHI_RECORD_SPLICE");
// research/208 S2: the render thread's use, while LLRHI_RENDER_THREAD asks for it (renderthread.toggle).
bool LLRHI::sRenderThread = true;
// Off unless set (and not 0): the splice self-test (research/66 step 1).
bool LLRHI::sSpliceTest = LLRHI::envSet("LLRHI_SPLICE_TEST");
U32 LLRHI::sSpliceTestPasses = []()
{
    const char* env = std::getenv("LLRHI_SPLICE_TEST");
    const int passes = env ? atoi(env) : 0;
    return passes > 0 ? (U32)passes : 1u;
}();
// On unless set to 0: the same commands, recorded on the cull threads (research/66 step 2).
bool LLRHI::sShadowRecordThreads = LLRHI::envDefaultOn("LLRHI_SHADOW_RECORD_THREADS");
S32 LLRHI::sShadowRecordBin = -1;
void (*LLRHI::sTranslateFlush)() = nullptr;     // research/207 T1: always null
bool (*LLRHI::sPostWork)(void (*)(void*), void*) = nullptr;
void (*LLRHI::sJoinShadowRecording)() = nullptr;
// On unless set to 0: the same packets, the opaque ones built on the cull threads (research/66 step 2B).
bool LLRHI::sShadowPacketThreads = LLRHI::envDefaultOn("LLRHI_SHADOW_PACKET_THREADS");

namespace
{
    // Pixel centres k + 0.5 (0 <= k < size) inside [lo, hi], at most 3.
    U32 centres_in(F32 lo, F32 hi, F32 size)
    {
        // Clamped first, so that huge (or infinite) bounds convert safely.
        lo = llclamp(lo, -1.f, size + 1.f);
        hi = llclamp(hi, -1.f, size + 1.f);
        const S32 first = llmax((S32)ceilf(lo - 0.5f), 0);
        const S32 last = llmin((S32)floorf(hi - 0.5f), (S32)size - 1);
        return last < first ? 0 : (U32)llmin(last - first + 1, 3);
    }
}

bool LLRHI::coverage(const CoverageView& view, const F32* lo, const F32* hi, U32& nx, U32& ny)
{
    nx = ny = 3;
    const F32* m = view.view_proj;
    // The box, grown by the float error of its own agent space corners (the
    // extents are sums of region offsets and transformed face extents), and the
    // largest coordinate magnitudes along each axis.
    F32 b0[3], d[3], a[3];
    for (U32 i = 0; i < 3; ++i)
    {
        a[i] = llmax(fabsf(lo[i]), fabsf(hi[i]));
        const F32 pad = 2e-6f * a[i] + 1e-6f;
        b0[i] = lo[i] - pad;
        d[i] = (hi[i] + pad) - b0[i];
        a[i] += pad;
    }
    // Bounds on the magnitudes of the terms of each clip coordinate, for every corner.
    const F32 ax = fabsf(m[0]) * a[0] + fabsf(m[4]) * a[1] + fabsf(m[8]) * a[2] + fabsf(m[12]);
    const F32 ay = fabsf(m[1]) * a[0] + fabsf(m[5]) * a[1] + fabsf(m[9]) * a[2] + fabsf(m[13]);
    const F32 aw = fabsf(m[3]) * a[0] + fabsf(m[7]) * a[1] + fabsf(m[11]) * a[2] + fabsf(m[15]);
    if (!(ax < F32_MAX) || !(ay < F32_MAX) || !(aw < F32_MAX))
    {
        return false;   // also NaN
    }
    // Clip x, y, w of the low corner and the steps along the box's edges.
    const F32 base[3] = { m[0] * b0[0] + m[4] * b0[1] + m[8] * b0[2] + m[12],
                          m[1] * b0[0] + m[5] * b0[1] + m[9] * b0[2] + m[13],
                          m[3] * b0[0] + m[7] * b0[1] + m[11] * b0[2] + m[15] };
    const F32 step[3][3] = { { m[0] * d[0], m[1] * d[0], m[3] * d[0] },
                             { m[4] * d[1], m[5] * d[1], m[7] * d[1] },
                             { m[8] * d[2], m[9] * d[2], m[11] * d[2] } };
    F32 min_x = F32_MAX, max_x = -F32_MAX, min_y = F32_MAX, max_y = -F32_MAX;
    for (U32 c = 0; c < 8; ++c)
    {
        F32 cx = base[0], cy = base[1], cw = base[2];
        for (U32 i = 0; i < 3; ++i)
        {
            if (c & (1u << i))
            {
                cx += step[i][0];
                cy += step[i][1];
                cw += step[i][2];
            }
        }
        // Every corner in front of the eye plane by far more than the float
        // error of w (so that the error estimate below holds): then the box's
        // projection lies inside its corners' (clipping only takes parts away).
        // Otherwise unknown; NaN lands here too.
        if (!(cw > 1e-3f * aw))
        {
            return false;
        }
        const F32 inv = 1.f / cw;
        const F32 px = cx * inv;
        const F32 py = cy * inv;
        // The GPU forms the same point as (projection * modelview * region
        // matrix) * region space vertex: allow a few dozen float steps of the
        // magnitudes involved, for its rounding and this one's.
        const F32 ex = 4e-6f * (ax + fabsf(px) * aw) * inv;
        const F32 ey = 4e-6f * (ay + fabsf(py) * aw) * inv;
        min_x = llmin(min_x, px - ex);
        max_x = llmax(max_x, px + ex);
        min_y = llmin(min_y, py - ey);
        max_y = llmax(max_y, py + ey);
    }
    // Window coordinates: (ndc * 0.5 + 0.5) * size. The pixel grid is the same
    // seen from either end of y, so a flipped viewport counts the same. The
    // margin: vertex snapping (Vulkan and GL guarantee at least 4 bits of
    // sub-pixel precision, an error of at most 1/32) and the float error of
    // the window mapping. Plus the jitter the draws may be moved by (research/185 §13).
    const F32 w = view.width;
    const F32 h = view.height;
    const F32 jitter = view.jitter > 0.f ? llmin(view.jitter, 1.f) : 0.f;
    const F32 margin_x = 1.f / 16.f + 1e-5f * w + jitter;
    const F32 margin_y = 1.f / 16.f + 1e-5f * h + jitter;
    nx = centres_in((min_x * 0.5f + 0.5f) * w - margin_x, (max_x * 0.5f + 0.5f) * w + margin_x, w);
    ny = centres_in((min_y * 0.5f + 0.5f) * h - margin_y, (max_y * 0.5f + 0.5f) * h + margin_y, h);
    return true;
}

void LLRHI::onGLShutdown()
{
    LLRHIVKMirror::onGLShutdown();
}

void LLRHI::onFrameNotShown()
{
    LLRHIVKMirror::onFrameNotShown();
}

bool LLRHI::frameRecording()
{
    return !sVulkanGL || LLRHIVKMirror::frameRecording();
}

LLRHI::TrackerHold::TrackerHold(bool on)
    : mHeld(LLRHIVKMirror::holdTracker(on && sTrackerHold && sDirect && sDirectDraw))
{
}

LLRHI::TrackerHold::~TrackerHold()
{
    if (mHeld)
    {
        LLRHIVKMirror::releaseTracker();
    }
}

void LLRHI::freeDeferred()
{
    LLRHIVKMirror::freeDeferred();
}

// The cue toggles of pollCueToggles, outside it so that cueToggleStates lists
// them too (vk-mcp2: the MCP tools cue_list and cue_toggle).
namespace LLRHI
{
namespace
{
    // A plain switch: <dir>/<file> flips *flag.
    struct Toggle
    {
        const char* file;
        bool* flag;
        const char* label;
    };
    const Toggle sCueToggles[] =
    {
        { "shadowmerge.toggle",   &sMergeShadowCascades, "shadow cascade merge" },
        { "nativeshadows.toggle", &sNativeShadows,       "native shadows" },
        { "nativerigged.toggle",  &sNativeRiggedShadows, "native rigged shadows" },
        { "paletteprebuild.toggle", &sPalettePrebuild,   "prebuilt skinning palettes (research/56)" },
        { "avatarstats.toggle",   &sAvatarStats,         "avatar crowd counters (research/56)" },
        { "avatarthreads.toggle", &sAvatarThreads,       "keyframe samples on the cull threads (research/63)" },
        { "avatarblend.toggle",   &sAvatarBlendLookup,   "pose blender lookups (research/63)" },
        { "probeshadow.toggle",   &sShareProbeShadows,   "probe shadow sharing" },
        { "probeshadowcache.toggle", &sCacheProbeShadows, "probe shadow cache" },
        { "probefast.toggle",     &sProbeFast,           "probe face culling" },
        { "cullthreads.toggle",   &sCullThreads,         "cull threads" },
        { "cullpar.toggle",       &sCullPar,             "main camera cull walks on the cull threads by subtree (research/184 A1)" },
        { "cullparverify.toggle", &sCullParVerify,       "main camera cull walks by subtree verify, comparing only (research/184 A1)" },
        { "cullreuse.toggle",     &sCullReuse,           "main camera cull walks replayed while the camera stands still (research/188)" },
        { "cullreuseverify.toggle", &sCullReuseVerify,   "main camera cull walk replay verify, comparing only (research/188)" },
        { "shadowsort.toggle",    &sShadowSortThreads,   "shadow cache sort on the cull threads" },
        { "shadowlist.toggle",    &sShadowListThreads,   "GPU shadow list scan on the cull threads" },
        { "probeshared.toggle",   &sProbeSharedCull,     "probe shared cull" },
        { "probestatic.toggle",   &sProbeStaticCache,    "probe static G-buffer cache" },
        { "texturebudget.toggle", &sTextureBudget,       "texture budget" },
        { "vrambudget.toggle",    &sVRAMBudgetReal,      "texture budget from the driver's VRAM budget (research/78)" },
        { "texpressure.toggle",   &sTexPressure,         "texture pressure while VRAM is short (research/79)" },
        { "texcompact.toggle",    &sTexCompact,          "texture pool blocks emptied into fuller ones while VRAM is short, with PlusBufferCompact (research/285 X2)" },
        { "texturevisit.toggle",  &sTextureVisitThreads, "texture visit face scans on the cull threads (research/65)" },
        { "texturevisitstats.toggle", &sTextureVisitStats, "texture visit counters (research/65)" },
        { "texturepace.toggle",   &sTextureVisitPace,    "settled textures visited every 4th round while the camera stands still (research/178 G1)" },
        { "idleskiptexanim.toggle", &sIdleSkipTexAnim,   "texture animations of unseen objects stepped one frame in 4 (research/178 D1)" },
        { "idleskipomega.toggle", &sIdleSkipOmega,       "spins of unseen objects stepped one frame in 4 (research/178 D2)" },
        { "idleskipstats.toggle", &sIdleSkipStats,       "idle skip counters (research/178 D)" },
        { "activeorphan.toggle",  &sActiveOrphan,        "orphan reunion leaves a still static parent static (research/180 10.12 R1)" },
        { "activeorphanverify.toggle", &sActiveOrphanVerify, "orphan reunion kept static verify, comparing only (research/180 10.12 R1)" },
        { "movesame.toggle",      &sMoveSame,            "a static drawable whose move changes nothing stays static (research/180 10.15)" },
        { "movesameverify.toggle", &sMoveSameVerify,     "move same verify, comparing only (research/180 10.15)" },
        { "idleskipmemo.toggle",  &sIdleSkipMemo,        "idle skip seen memo (research/180 9 1)" },
        { "idleskipmemoverify.toggle", &sIdleSkipMemoVerify, "idle skip seen memo verify, comparing only (research/180 9 1)" },
        { "seenbymemo.toggle",    &sSeenByMemo,          "idle skip asks seen only when it decides: turn frames, the root's memo (research/180 10.18)" },
        { "idlecensus.toggle",    &sIdleCensus,          "idle objects census, timing only (research/193 idle-cheap A)" },
        { "vparammemo.toggle",    &sVParamMemo,          "avatar default parameter check from a kept list (research/193 idle-cheap B)" },
        { "vparammemoverify.toggle", &sVParamMemoVerify, "avatar default parameter check verify, comparing only (research/193 idle-cheap B)" },
        { "texanimmemo.toggle",   &sTexAnimMemo,         "texture animation matrices copied from the face before (research/179 cpu-present)" },
        { "texanimmemoverify.toggle", &sTexAnimMemoVerify, "texture animation matrix copy verify, comparing only (research/179 cpu-present)" },
        { "texanimgpu.toggle",    &sTexAnimGpu,          "texture scrolls' matrices worked out where they are drawn (research/194 11)" },
        { "texanimgpuverify.toggle", &sTexAnimGpuVerify, "texture scroll description verify, comparing only (research/194 11)" },
        { "lighttable.toggle",    &sLightTable,          "local light table: lights that do not move read from their records (research/203)" },
        { "lighttableverify.toggle", &sLightTableVerify, "local light table verify, comparing only (research/203)" },
        { "envuniformmemo.toggle", &sEnvUniformMemo,     "sky and water uniforms sent again only when they change (research/187 N4)" },
        { "envuniformmemoverify.toggle", &sEnvUniformMemoVerify, "sky and water uniform memo verify, comparing only (research/187 N4)" },
        { "trackerhold.toggle",   &sTrackerHold,         "backend lock held across static batch loops (research/180 9 3)" },
        { "packets.toggle",       &sPackets,             "draw packets for the legacy material loop (research/180 stage 4 P1)" },
        { "packetsverify.toggle", &sPacketsVerify,       "draw packets verify, comparing only (research/180 stage 4 P1)" },
        { "packetscensus.toggle", &sPacketsCensus,       "draw packet census, counting only (research/180 stage 4 P1-0)" },
        { "packetobjects.toggle", &sPacketObjects,       "draw packets take the batches with their object's matrix (research/180 stage 4 P1c)" },
        { "packetshort.toggle",   &sPacketShort,         "draw packets take an object's short stretch with its own matrices (research/180 stage 4 P1d)" },
        { "alphaglowskip.toggle", &sAlphaGlowSkip,       "alpha glow pass without the batches that have no glow (research/178 F0)" },
        { "alphapassstats.toggle", &sAlphaPassStats,     "alpha pass counters and glow skip verify (research/178 F0)" },
        { "texdirect.toggle",     &sTexDirect,           "texture pixels converted straight into staging (research/70)" },
        { "texbatch.toggle",      &sTexBatch,            "texture creation in batches on the cull threads (research/70)" },
        { "texlazyattach.toggle", &sTexLazyAttach,       "texture images attachable only once attached (research/70)" },
        { "texstats.toggle",      &sTexStats,            "texture creation counters (research/70)" },
        { "bcn.toggle",           &sTexBCN,              "BC7 texture compression, new textures (research/88)" },
        { "geombudget.toggle",    &sGeomBudget,          "geometry rebuild budget" },
        { "prioritybudget.toggle", &sPriorityBudget,     "priority rebuild budget (research/69)" },
        { "meshnotify.toggle",    &sMeshNotifyFast,      "mesh notify with one clock read and a kept sort (research/69)" },
        { "shadowcache.toggle",   &sCacheSunShadows,     "sun shadow cache" },
        { "drawruns.toggle",      &sDrawRuns,            "G-buffer draw runs" },
        { "drawtemplates.toggle", &sDrawTemplates,       "draw templates (G4c)" },
        { "gpudriven.toggle",     &sGPUDriven,           "GPU-driven shadow casters" },
        { "gpulist.toggle",       &sGPUDrivenList,       "GPU shadow list (G2)" },
        { "gpucull.toggle",       &sGPUDrivenCull,       "GPU shadow cull (G3)" },
        { "gpuprobe.toggle",      &sGPUDrivenProbe,      "GPU probe list (G5)" },
        { "gpudgc.toggle",        &sGPUDrivenDGC,        "GPU probe list with generated commands (research/50 step 4)" },
        { "oit.toggle",           &sOIT,                 "order-independent transparency (research/41)" },
        { "oithybrid.toggle",     &sOITHybrid,           "hybrid OIT (off: full OIT)" },
        { "gputiming.toggle",     &sGPUTiming,           "GPU pass timing (research/53)" },
        { "upscale.toggle",       &sUpscale,             "upscaling of the main view, PlusUpscale (research/165)" },
        { "gputimingalpha.toggle", &sGPUTimingAlpha,     "GPU timing of the alpha pool by kind (research/54)" },
        { "framecost.toggle",     &sFrameCost,           "frame cost accounting (research/83)" },
        { "rtcensus.toggle",      &sRenderThreadCensus,  "render thread census, timing only (research/208 S0)" },
        { "periodiclogs.toggle",  &sPeriodicLogs,        "periodic diagnostics lines: VKPerf, bindless, RHIShadow, Skin (fix-logcache)" },
        { "framepacing.toggle",   &sFramePacing,         "VSync frame pacing (research/94)" },
        { "pacinggpu.toggle",     &sFramePacingGPUBound, "frame pacing skips the wait while the GPU sets the pace (research/103 F1)" },
        { "presentthread.toggle", &sPresentThread,       "main window present on the present thread without VSync (research/179 6.6 4)" },
        { "presentmailbox.toggle", &sPresentMailbox,     "MAILBOX before IMMEDIATE without VSync, swapchain made again (research/179 6.6 4)" },
        { "alphacensus.toggle",   &sAlphaCensus,         "alpha census (research/54)" },
        { "slotcache.toggle",     &sSlotCache,           "texture slot cache across render targets (research/58)" },
        { "passunits.toggle",     &sPassUnitsRecheck,    "pass cache hits through units bound again to the same textures (research/167 C2b)" },
        { "slotpublish.toggle",   &sSlotPublish,         "texture slot publication (research/180 stage 1)" },
        { "slotpublishverify.toggle", &sSlotPublishVerify, "texture slot publication verify, comparing only (research/180 stage 1)" },
        { "poollazy.toggle",      &sPoolLazyForget,      "attribute pool lazy forget (research/58)" },
        { "coverage.toggle",      &sZeroCoverageCull,    "zero coverage cull (research/61)" },
        { "tinystats.toggle",     &sTinyStats,           "tiny object stats (research/61)" },
        { "receivercull.toggle",  &sShadowReceiverCull,  "shadow receiver cull (research/64)" },
        { "receivercullaniso.toggle", &sShadowReceiverCullAniso, "shadow receiver cull with anisotropic shadow maps (research/64)" },
        { "shadowsameinput.toggle", &sShadowSameInput,   "sun shadow walks kept while their inputs stay the same (research/113)" },
        { "recordsplice.toggle",  &sRecordSplice,        "recording splices (research/66)" },
        { "renderthread.toggle",  &sRenderThread,        "render thread records the retained scene's passes, with LLRHI_RENDER_THREAD (research/208 S2)" },
        { "splicetest.toggle",    &sSpliceTest,          "splice self-test on native shadow passes (research/66)" },
        { "shadowrecord.toggle",  &sShadowRecordThreads, "native shadow passes recorded on the cull threads (research/66 step 2)" },
        { "shadowpacket.toggle",  &sShadowPacketThreads, "native shadow packets built on the cull threads (research/66 step 2B)" },
        { "texhandles.toggle",    &sTexHandles,          "draws name textures by number (research/197 stage 1a)" },
        { "texhandlesverify.toggle", &sTexHandlesVerify, "texture numbers verify, comparing only (research/197 stage 1a)" },
        { "vbhandles.toggle",     &sVBHandles,           "draws name vertex buffers by number (research/197 stage 1b)" },
        { "vbhandlesverify.toggle", &sVBHandlesVerify,   "vertex buffer numbers verify, comparing only (research/197 stage 1b)" },
    };

    // The draw lists' switches (research/137 N7; what each does: pollCueToggles).
    enum : int { LIST_MODE = -1, LIST_REFERENCE = -2, LIST_THREADS = -3 };
    struct ListToggle
    {
        const char* file;
        int which;          // a DrawList::Kind, or LIST_MODE / LIST_REFERENCE / LIST_THREADS
        const char* label;
    };
    const ListToggle sListToggles[] =
    {
        { "drawlists.toggle",         LIST_MODE,                     "draw lists (research/137, LLRHI_DRAW_LISTS)" },
        { "drawlistsref.toggle",      LIST_REFERENCE,                "draw lists' reference path (LLRHI_DRAW_LISTS=2; off: the light path, =1)" },
        { "drawlistsopaque.toggle",   DrawList::KIND_OPAQUE,         "opaque pools' draw lists (research/137 N2)" },
        { "drawlistsalpha.toggle",    DrawList::KIND_ALPHA,          "alpha pool's draw lists (research/137 N3)" },
        { "drawlistsmaterial.toggle", DrawList::KIND_MATERIAL,       "legacy material and bump pools' draw lists (research/137 N4)" },
        { "drawliststhreads.toggle",  LIST_THREADS,                  "draw lists on the cull threads (research/137 N11, LLRHI_DRAW_LISTS_THREADS)" },
    };
}
}

const std::string& LLRHI::cueDir()
{
    static const std::string dir = LLStringUtil::getenv("LLRHI_CUE_DIR");    // UTF-8, as LLFile takes it
    return dir;
}

bool LLRHI::takeCue(const char* file)
{
    const std::string& dir = cueDir();
    if (dir.empty())
    {
        return false;
    }
    const std::string cue = dir + "/" + file;
    return LLFile::isfile(cue) && LLFile::remove(cue) == 0;
}

bool LLRHI::CuePoll::due()
{
    if (cueDir().empty())
    {
        return false;
    }
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < mNext)
    {
        return false;
    }
    mNext = now + mSeconds;
    return true;
}

void LLRHI::pollCueToggles(const std::string& dir)
{
#if LL_WINDOWS
    // research/83: one directory lookup instead of a file check per toggle
    // (about 60 checks once a second: a short hitch); nothing more while no
    // .toggle file is there. LLRHI_CUE_SCAN=0 checks each file as before.
    static const bool scan = LLRHI::envDefaultOn("LLRHI_CUE_SCAN");
    if (scan)
    {
        WIN32_FIND_DATAW found;
        const HANDLE find = FindFirstFileW(ll_convert_string_to_wide(dir + "/*.toggle").c_str(), &found);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;     // no cue file (or no directory)
        }
        FindClose(find);
    }
#endif
    for (const Toggle& toggle : sCueToggles)
    {
        const std::string cue = dir + "/" + toggle.file;
        if (!LLFile::isfile(cue) || LLFile::remove(cue) != 0)
        {
            continue;
        }
        *toggle.flag = !*toggle.flag;
        if (toggle.flag == &sVParamMemo || toggle.flag == &sVParamMemoVerify)
        {
            sVParamMemoAuto = false;    // research/193: a cue ends auto
        }
        if (toggle.flag == &sTexAnimGpu || toggle.flag == &sTexAnimGpuVerify)
        {
            sTexAnimGpuAuto = false;    // research/194: a cue ends auto
        }
        if (toggle.flag == &sLightTable || toggle.flag == &sLightTableVerify)
        {
            sLightTableAuto = false;    // research/203: a cue ends auto
        }
        if (toggle.flag == &sTexHandles || toggle.flag == &sTexHandlesVerify)
        {
            sTexHandlesAuto = false;    // research/197 stage 1a: a cue ends auto
        }
        if (toggle.flag == &sVBHandles || toggle.flag == &sVBHandlesVerify)
        {
            sVBHandlesAuto = false;     // research/197 stage 1b: a cue ends auto
        }
        if (toggle.flag == &sCacheSunShadows)
        {
            sMergeShadowCascades |= sCacheSunShadows;   // the cache needs merged cascades
        }
        LL_INFOS("RHIProfile") << toggle.label << " " << (*toggle.flag ? "on" : "off") << LL_ENDL;
    }

    // research/137 N7: the draw lists' switches (llrhidrawlist.h). The mode
    // is a number: drawlists.toggle turns it off and back to what it was (1
    // when it started off), drawlistsref.toggle swaps the light path (1) and
    // the reference path (2) while it is on. The kinds are flags;
    // drawliststhreads.toggle turns LLRHI_DRAW_LISTS_THREADS off and back to
    // what it was (N11; both parts when it started off). No list is
    // open here (before the frame's draws).
    // Where a switch cannot take effect it stays as it is and says why
    // ("unchanged", as N8 and N9 do): every one without the backend's
    // hook (LLRHI_DIRECT=0 at startup: LLVKDrawList::install sets none), and
    // drawliststhreads.toggle turning on without its needs (the light path,
    // LLRHI_DRAW_LISTS=1, and the cull threads); turning it off always goes.
    // The files: sListToggles above.
    static U32 mode_on = DrawList::sMode ? DrawList::sMode : 1;     // what drawlists.toggle turns back on
    // What drawliststhreads.toggle turns back on (both parts when it started off).
    static U32 threads_on = DrawList::sThreads ? DrawList::sThreads : DrawList::THREADS_BUILD | DrawList::THREADS_RECORD;
    for (const ListToggle& toggle : sListToggles)
    {
        const std::string cue = dir + "/" + toggle.file;
        if (!LLFile::isfile(cue) || LLFile::remove(cue) != 0)
        {
            continue;
        }
        if (!DrawList::sDrawList)
        {
            LL_INFOS("RHIProfile") << toggle.label << ": not installed (LLRHI_DIRECT=0 at startup), unchanged" << LL_ENDL;
            continue;
        }
        if (toggle.which == LIST_THREADS && !DrawList::sThreads && (DrawList::sMode != 1 || !sCullThreads))
        {
            std::string needs;
            if (DrawList::sMode != 1)
            {
                needs = "the light path (LLRHI_DRAW_LISTS=1, now " + std::to_string(DrawList::sMode) + ")";
            }
            if (!sCullThreads)
            {
                needs += (needs.empty() ? "" : " and ");
                needs += "the cull threads (cullthreads.toggle)";
            }
            LL_INFOS("RHIProfile") << toggle.label << ": needs " << needs << ", unchanged" << LL_ENDL;
            continue;
        }
        if (toggle.which == LIST_MODE)
        {
            if (DrawList::sMode)
            {
                mode_on = DrawList::sMode;
                DrawList::sMode = 0;
            }
            else
            {
                DrawList::sMode = mode_on;
            }
            const bool any_kind = DrawList::sKind[DrawList::KIND_OPAQUE] || DrawList::sKind[DrawList::KIND_ALPHA] ||
                                  DrawList::sKind[DrawList::KIND_MATERIAL];
            LL_INFOS("RHIProfile") << toggle.label << " " << (DrawList::sMode ? "on" : "off") << " (LLRHI_DRAW_LISTS="
                                   << DrawList::sMode << ")"
                                   << (DrawList::sMode && !any_kind ? "; no kind is on (drawlistsopaque / alpha / material.toggle): no list opens" : "")
                                   << LL_ENDL;
        }
        else if (toggle.which == LIST_REFERENCE)
        {
            if (!DrawList::sMode)
            {
                LL_INFOS("RHIProfile") << toggle.label << ": draw lists are off (drawlists.toggle), unchanged" << LL_ENDL;
                continue;
            }
            DrawList::sMode = DrawList::sMode == 1 ? 2 : 1;
            mode_on = DrawList::sMode;
            LL_INFOS("RHIProfile") << toggle.label << " " << (DrawList::sMode == 2 ? "on" : "off") << " (LLRHI_DRAW_LISTS="
                                   << DrawList::sMode << ")" << LL_ENDL;
        }
        else if (toggle.which == LIST_THREADS)
        {
            // Between frames: no segment or chunk is in flight (each is
            // recorded before its frame ends), so the next list takes it.
            if (DrawList::sThreads)
            {
                threads_on = DrawList::sThreads;
                DrawList::sThreads = 0;
            }
            else
            {
                DrawList::sThreads = threads_on;
            }
            LL_INFOS("RHIProfile") << toggle.label << " " << (DrawList::sThreads ? "on" : "off") << " (LLRHI_DRAW_LISTS_THREADS="
                                   << DrawList::sThreads << ")" << LL_ENDL;
        }
        else
        {
            bool& kind = DrawList::sKind[toggle.which];
            kind = !kind;
            LL_INFOS("RHIProfile") << toggle.label << " " << (kind ? "on" : "off")
                                   << (kind && !DrawList::sMode ? "; draw lists are off (drawlists.toggle): no list opens" : "") << LL_ENDL;
        }
    }

    // research/137 N9: the immediate mode ring (llrhiimmediate.h; LLRender::
    // flush reads the switch at each flush, on the main thread). Only where
    // the backend installed its hook at startup (direct draws on).
    {
        const std::string cue = dir + "/immediatering.toggle";
        if (LLFile::isfile(cue) && LLFile::remove(cue) == 0)
        {
            if (!sImmediateDraw)
            {
                LL_INFOS("RHIProfile") << "immediate mode ring (research/137 N9): not installed (LLRHI_DIRECT=0 at startup), unchanged"
                                       << LL_ENDL;
            }
            else
            {
                sImmediateRing = !sImmediateRing;
                LL_INFOS("RHIProfile") << "immediate mode ring (research/137 N9, LLRHI_IMMEDIATE_RING) " << (sImmediateRing ? "on" : "off")
                                       << LL_ENDL;
            }
        }
    }

    // research/137 N8: the occlusion batch (llrhiocclusion.h; Batch::open
    // and getQueryObjectuiv read the mode). Only where the backend installed
    // its hooks at startup (LLVKOcclusion::install). Between frames, here: no batch is open, and a
    // query's answer is the same by either way (read_answers).
    {
        const std::string cue = dir + "/occlusionbatch.toggle";
        if (LLFile::isfile(cue) && LLFile::remove(cue) == 0)
        {
            if (!sOcclusionBoxes)
            {
                LL_INFOS("RHIProfile") << "occlusion batch (research/137 N8): not installed, unchanged" << LL_ENDL;
            }
            else
            {
                Occlusion::sMode = Occlusion::sMode ? 0 : 1;
                LL_INFOS("RHIProfile") << "occlusion batch (research/137 N8, LLRHI_OCCLUSION_BATCH) " << (Occlusion::sMode ? "on" : "off")
                                       << LL_ENDL;
            }
        }
    }

    // research/179 A: where the occlusion marks are stored (llrhiocclusion.h).
    // The backend takes the switch at each frame slot's first box; the
    // queries already marked keep the way they were marked (VisibilityBuffer::store).
    {
        const std::string cue = dir + "/occlusionstore.toggle";
        if (LLFile::isfile(cue) && LLFile::remove(cue) == 0)
        {
            const U32 now = Occlusion::sStoreMode.load(std::memory_order_relaxed) == Occlusion::STORE_HOST ? Occlusion::STORE_DEVICE
                                                                                                            : Occlusion::STORE_HOST;
            Occlusion::sStoreMode.store(now, std::memory_order_relaxed);
            LL_INFOS("RHIProfile") << "occlusion marks (research/179 A, LLRHI_OCCLUSION_STORE) in "
                                   << (now == Occlusion::STORE_HOST ? "host memory (the old way)" : "device memory") << LL_ENDL;
        }
    }
}

void LLRHI::cueToggleStates(std::vector<CueToggleState>& out)
{
    out.clear();
    for (const Toggle& toggle : sCueToggles)
    {
        out.push_back({ toggle.file, toggle.label, *toggle.flag, true });
    }
    // The switches pollCueToggles flips by their own rules (available: false
    // where it leaves them unchanged, "not installed").
    const bool lists = DrawList::sDrawList != nullptr;
    for (const ListToggle& toggle : sListToggles)
    {
        bool on = false;
        switch (toggle.which)
        {
            case LIST_MODE:      on = DrawList::sMode != 0; break;
            case LIST_REFERENCE: on = DrawList::sMode == 2; break;
            case LIST_THREADS:   on = DrawList::sThreads != 0; break;
            default:             on = DrawList::sKind[toggle.which]; break;
        }
        out.push_back({ toggle.file, toggle.label, on, lists });
    }
    out.push_back({ "immediatering.toggle", "immediate mode ring (research/137 N9, LLRHI_IMMEDIATE_RING)", sImmediateRing,
                    sImmediateDraw != nullptr });
    out.push_back({ "occlusionbatch.toggle", "occlusion batch (research/137 N8, LLRHI_OCCLUSION_BATCH)", Occlusion::sMode != 0,
                    sOcclusionBoxes != nullptr });
    out.push_back({ "occlusionstore.toggle", "occlusion marks in device memory (research/179 A, LLRHI_OCCLUSION_STORE; off: host memory)",
                    Occlusion::sStoreMode.load(std::memory_order_relaxed) != Occlusion::STORE_HOST, true });
}

void LLRHI::onGLFunctionsLoaded()
{
    envLoggingStarts();     // the switches' refusals so far (read before logging started)
    const char* profile = std::getenv("LLRHI_PROFILE");
    if (profile && *profile && strcmp(profile, "0"))
    {
        LLRHIProfiler::install(atoi(profile) > 1 ? atoi(profile) : 10);
    }

    // Vulkan renders; without it the viewer cannot run (research/85 stage 0).
    LL_INFOS("RHI") << "Render backend: Vulkan, no OpenGL context (research/117)" << LL_ENDL;
    LLRHIVKMirror::install(LLRHI::envSet("LLRHI_VK_VALIDATION"));

    // After the Vulkan layer: its install replaces glShaderSource/glAttachShader/
    // glLinkProgram outright, so a dump wrapper installed earlier was dropped.
    // A folder: UTF-8 (LLStringUtil::getenv), as LLFile and llofstream take it.
    const std::string dump = LLStringUtil::getenv("LLRHI_DUMP_SHADERS");
    if (!dump.empty())
    {
        LLRHIShaderDump::install(dump);
    }

    // Last, so it wraps everything above and counts what the viewer issues even
    // when the Vulkan layer stops passing draws on to the GL driver.
    if (LLRHI::envSet("LLRHI_TRACE") || LLRHIUploadTrace::enabled())
    {
        LLRHIGLStats::install();
    }
}
