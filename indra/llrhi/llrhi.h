/**
 * @file llrhi.h
 * @brief Render hardware interface: backend selection and the hook into GL loading.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLRHI_H
#define LL_LLRHI_H

#include "stdtypes.h"
#include "llrhitexanim.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Everything in llrhi lives outside the upstream files. Upstream code reaches
// it through a handful of hook calls only; keep it that way so weekly merges
// from secondlife/viewer stay cheap.
namespace LLRHI
{
    // Hook: called by LLGLManager::initGL() once every GL entry point has been
    // loaded through LL_GET_PROC_ADDRESS. On Windows every GL function the
    // viewer calls is a function pointer, so this is where a backend (or a
    // diagnostic layer) can redirect them without touching call sites.
    void onGLFunctionsLoaded();
    // Hook: called by LLGLManager::shutdownGL() (viewer exit, context switch).
    void onGLShutdown();

    // research/207 T3: the GPU that renders, as LLGLManager::initGL fills
    // gGLManager from it (no GL strings or limits to ask). The strings keep
    // the forms of the GL drivers the viewer parses and matches: the vendor
    // ("NVIDIA Corporation", "ATI Technologies Inc.", "Intel"), the renderer,
    // the version "4.6.0 <driver>" (parse_gl_version), the shading language
    // "4.60" (parse_glsl_version); the GPU table (llfeaturemanager) matches
    // the vendor and renderer. Asked before onGLFunctionsLoaded (the Vulkan
    // device is made with the window). False: no device.
    struct DeviceInfo
    {
        std::string vendor;
        std::string renderer;
        std::string version;
        std::string glsl_version;
        U32 vendor_id = 0;
        U32 vram_mb = 0;                    // the GPU that renders (NVML, DXGI or the device heaps)
        bool shared_memory = false;         // no VRAM of its own (integrated, virtual, CPU): vram_mb is a share of the PC's memory (igpu-safe)
        S32 max_texture_units = 16;
        S32 max_samples = 1;
        S32 max_color_texture_samples = 1;
        S32 max_depth_texture_samples = 1;
        S32 max_integer_samples = 1;
        S32 max_sample_mask_words = 1;
        S32 max_varying_vectors = 16;
        S32 max_uniform_block_size = 16384;
        S32 max_texture_size = 4096;
        S32 max_elements_vertices = 1048576;    // the GL drivers' recommendation (NVIDIA's): the viewer batches by it
        S32 max_elements_indices = 1048576;
        F32 line_width_range[2] = { 1.f, 1.f };
        F32 max_anisotropy = 1.f;
    };
    bool deviceInfo(DeviceInfo& out);
    // Hook: called by display() when it draws nothing this loop (the window
    // minimized or hidden) and returns before its swap. The Vulkan frame ends
    // without being shown and the next begins, so what waits for the frame's
    // end is freed meanwhile (fix-pipeleak). Main thread.
    void onFrameNotShown();
    // Whether draws and clears made now reach a frame (research/261). With
    // Vulkan, none is open before the first present (gPipeline.init and the
    // shaders' first load run under the splash screen): a draw or clear made
    // then is dropped. After the first present a frame is always open (one
    // ends and the next begins at each present, minimized or not). Always
    // true with GL. Main thread.
    bool frameRecording();
    // Hook (fix-round107): called by a shader reload (plus/shaderreload, at
    // the head of LLViewerShaderMgr::setShaders and of each pass of a
    // parallel reload) before the old render targets go. The open Vulkan
    // frame ends unshown and every frame in flight completes, so what they
    // deferred (the buffers a previous pass or the caller released) is freed
    // now, not after the reload: the old and new render targets of two or
    // three reloads in a row no longer add up past the VRAM budget. Logs the
    // VRAM before and after. Main thread, outside a frame's draws only (as
    // onFrameNotShown: never inside display()'s rendering or a snapshot).
    void freeDeferred();
    // <dir>/<name>.toggle (dir: LLRHI_CUE_DIR) flips a diagnostics switch while
    // the viewer runs; the file is deleted once seen. Called from the main
    // thread before the frame's draws (llviewerdisplay's poll_rhi_toggles, once
    // a second), so a switch never changes in the middle of a frame's recording.
    void pollCueToggles(const std::string& dir);
    // The cue files pollCueToggles takes and what each switch is now (vk-mcp2:
    // the MCP tools cue_list and cue_toggle, plus/posermcp/debugtools.cpp).
    // available: false where the cue would leave the switch unchanged (its
    // hook was not installed at startup). Main thread, as pollCueToggles.
    struct CueToggleState
    {
        const char* file;
        const char* label;
        bool on;
        bool available;
    };
    void cueToggleStates(std::vector<CueToggleState>& out);

    // The cue files of the features outside pollCueToggles (plus/<feature>;
    // research/273). The folder: LLRHI_CUE_DIR as UTF-8 (LLStringUtil::getenv,
    // as LLFile takes it), read once; empty without it (the downloadable
    // viewer: no cues). Main thread.
    const std::string& cueDir();
    // True when <cueDir()>/<file> was there and is removed now: the cue is
    // taken. False without LLRHI_CUE_DIR (nothing is looked at).
    bool takeCue(const char* file);
    // A feature's look for its cues at most once every `seconds` of frame time
    // (LLFrameTimer::getElapsedSeconds): due() is true when a look is due and
    // starts the wait for the next one; never without LLRHI_CUE_DIR. Kept in a
    // function's static, main thread.
    class CuePoll
    {
    public:
        constexpr explicit CuePoll(F64 seconds) : mSeconds(seconds) {}
        bool due();

    private:
        F64 mSeconds;
        F64 mNext = 0.0;
    };

    // How the switches (LLRHI_* and the like) read the environment
    // (research/273). Each switch reads once, in a static initializer or a
    // function's static, never on every call. std::getenv: the C runtime's copy
    // made at the start, which keeps a safe start's values after plus/safemode
    // takes them out of the block child processes get (research/254 9.4); not
    // GetEnvironmentVariableW or LLStringUtil::getenv (research/149 (P)).
    //   envDefaultOn: on unless set to "0" (unset or empty: on).
    //   envSet: off unless set to something other than "" and "0".
    //   envIs: set to exactly `value`.
    //   envUnset: unset or empty.
    //   envFirstChar: the value's first character, '\0' when unset or empty
    //   (the switches that read only that: "0..." off, "1..." on).
    //   envNumber: a whole number, decimal (a leading 0 is not octal) or hex
    //   after 0x, from min to max, into out. False, out untouched (the
    //   default): unset or empty, or anything else, which logs one warning
    //   naming the switch and the value kept. A static initializer may read it:
    //   a refusal before logging starts is held until envLoggingStarts()
    //   (LLRHI::onGLFunctionsLoaded) logs it.
    bool envDefaultOn(const char* name);
    bool envSet(const char* name);
    bool envIs(const char* name, const char* value);
    bool envUnset(const char* name);
    char envFirstChar(const char* name);
    bool envNumber(const char* name, U64 min, U64 max, U64& out);
    void envLoggingStarts();

    // research/303: whether a window (an HWND; Windows only, true elsewhere) is on a display and its session has an
    // input desktop (not while the computer is locked, a remote desktop is disconnected or another user is
    // switched to), and, with remote_session_blocks, not in a remote desktop session at all (its display comes
    // and goes with the connection). The swapchain asks with true (a lost surface is waited for then);
    // plus/background with false (a connected remote session shows what the viewer draws). A window that is
    // gone counts as true (it does not come back). The window's own state (minimized, hidden) is the
    // caller's.
    bool windowOnADisplay(void* window, bool remote_session_blocks);

    // Hook flag: set by LLPipeline::renderShadow around the opaque sun shadow
    // casters, which all use one depth-only shader (position x MVP). A Vulkan
    // backend records those draws through a dedicated path instead of
    // translating GL state draw by draw. Harmless when nothing reads it.
    extern bool sDepthOnlyPass;

    // Hook flag: whose occlusion boxes are being drawn (LLPipeline::doOcclusion):
    // 0 the main camera, 1 a sun shadow camera, 2 reflection probes.
    extern U32 sOcclusionScope;

    // Sun shadow cascades share one state sort (LLPipeline::renderSunShadowCascades).
    // Same shadow maps, less CPU. LLRHI_SHADOW_MERGE=0/1 (default off while being validated).
    extern bool sMergeShadowCascades;

    // Static sun shadow cache (LLPipeline::renderCachedShadowCascade): static casters
    // are redrawn only when the cascade's projection or their contents change.
    // LLRHI_SHADOW_CACHE=0 turns it off (on by default; it turns on sMergeShadowCascades too).
    extern bool sCacheSunShadows;

    // The realtime reflection probe's six faces share two sun shadow maps around
    // the probe (LLPipeline::shareProbeShadows). On unless LLRHI_PROBE_SHADOW_SHARE=0.
    extern bool sShareProbeShadows;

    // Static cache for those shared maps (LLPipeline::shareProbeShadows), as the
    // sun's cascades have: each map keeps the depth of its static casters and,
    // while the probe, the sun and the map's projection stay exactly the same,
    // an update copies it and draws only the moving casters. Two depth images of
    // the shared maps' size (4096^2: about 128 MB of VRAM).
    // On unless LLRHI_PROBE_SHADOW_CACHE=0.
    extern bool sCacheProbeShadows;

    // Reflection probe faces leave out the static batches that lie entirely
    // outside the face's view (LLPipeline::postSort); same pixels, fewer draws.
    // Vulkan mode only. On unless LLRHI_PROBE_FAST=0 (research/44).
    extern bool sProbeFast;

    // The alpha shader (alphaF.glsl) as the Vulkan backend builds it takes the
    // sun shadow only for quads that see the sun and stops discarded fragments
    // (which Vulkan's discard keeps running as helpers) once nothing needs their
    // derivatives; same pixels (research/54). Vulkan mode only, and only with
    // the device features it needs. On unless LLRHI_ALPHA_FAST=0.
    extern bool sAlphaFast;

    // Research/58: a texture's bindless slot, once resolved in a frame, serves
    // every later draw of that frame that samples it (also through a texture
    // unit, and into any render target it is not attached to) while its
    // version stays; before, only draws into the same render target resolution
    // reused it. Same slots. Vulkan mode only. On unless LLRHI_SLOT_CACHE=0;
    // slotcache.toggle flips it.
    extern bool sSlotCache;
    // speed-pass-units (research/167 C2b): a direct draw whose program's
    // descriptor sampler units were bound again since its pass cache entry
    // (another program bound other textures there and then these back) still
    // hits when the samplers read the same textures at the same versions, as
    // when a texture elsewhere changed. Same images, same sets. Vulkan mode
    // only. On unless LLRHI_PASS_UNITS=0; passunits.toggle flips it.
    extern bool sPassUnitsRecheck;
    // slotpublish (research/180 stage 1): the backend keeps "GL texture name
    // -> bindless slot" while it stays true (llvkgl_descriptors.cpp, the slot
    // publication) and a draw's sampler takes the slot from it instead of
    // resolving the texture again. sSlotPublishVerify: resolves the usual way
    // and draws with that, comparing the publication; mismatches and broken
    // invariants are counted (VKPerf "published [...]"), the first logged.
    // LLRHI_SLOT_PUBLISH: 0 off, 1 on, verify (or LLRHI_SLOT_PUBLISH_VERIFY=1)
    // verify only; unset (auto): verify first, then on once enough draws
    // compared with no mismatch, off if any (sSlotPublishAuto, cancelled by
    // a cue). slotpublish.toggle / slotpublishverify.toggle flip them.
    extern bool sSlotPublish;
    extern bool sSlotPublishVerify;
    extern bool sSlotPublishAuto;
    // True once the main view's G-buffer pass ran after the login completed
    // (STATE_STARTED, set by LLPipeline::renderGeomDeferred). The auto mode
    // counts its frames and draws from here only: the login screen's frames
    // are verified (a mismatch still turns it off) but do not turn it on.
    extern bool sWorldShown;
    // research/218 vk-s7f (research/207 7.1): the material loop's batches
    // drawn by name with a texture still given by its GL name (not a texture
    // number, not the white texture). Taken by the VKPerf "old paths" line.
    extern std::atomic<U32> sOldMaterialNames;
    // research/289 (the materialize integration check's L10): the backend's failed tries since
    // the start to make an image (vmaCreateImage failed: VRAM short, or another reason) of a
    // texture given its pixels (sTextureImageFailures: it keeps its GL name with no image,
    // drawn white; tried again at its next resolution, each try counted), and of any other
    // (sTargetImageFailures: render targets, layered and depth images, the 1 x 1 stand-ins;
    // every texture while LLRHI_TEX_LAZY_ATTACH=0). Counts only failures of ensure_image when
    // it creates an image. Rebuilds that keep the original image (the attachable form, BC7 to
    // plain, the move of compaction) are not counted. Only grow. Taken by PlusMaterializeLog's
    // line (plus/materialize: gray image_failed, target_failed).
    extern std::atomic<U32> sTextureImageFailures;
    extern std::atomic<U32> sTargetImageFailures;
    // Research/58: the attribute pool (G4b) keeps the node of a pair GL wrote
    // or deleted as an empty entry until its next sweep, so the tickets of the
    // other pairs (draw templates) stay valid instead of being looked up
    // again after every such write. Same placements. On unless
    // LLRHI_POOL_LAZY_FORGET=0; poollazy.toggle flips it.
    extern bool sPoolLazyForget;
    // The blended legacy material shader (materialF.glsl, Material Shader N with
    // DIFFUSE_ALPHA_MODE 1) as the Vulkan backend builds it works out its alpha
    // before the probes, haze and sun shadow: a quad whose fragments all end at
    // alpha 0 skips them (and still writes its alpha 0 color, OIT node and depth
    // as before), and the sun shadow is taken only for quads where a fragment's
    // color depends on it. Same pixels (research/59). Vulkan mode only, and only
    // with the device features sAlphaFast needs. On unless LLRHI_MATERIAL_FAST=0.
    extern bool sMaterialFast;

    // The reflection probe library (class3 reflectionProbeF.glsl) as the Vulkan
    // backend builds it takes the ambient, glossy and legacy probe sums in one
    // walk over the probes instead of one walk each; each sum gets the same
    // terms in the same order, so the same pixels (research/59). Vulkan mode
    // only. On unless LLRHI_PROBE_FUSE=0.
    extern bool sProbeFuse;

    // Sun shadow cascades are culled on worker threads at the same time
    // (LLPipeline::renderSunShadowCascades), and their shared state sort skips
    // the render map nobody reads. Same cull results, same order. On unless
    // LLRHI_CULL_THREADS=0 (research/48).
    extern bool sCullThreads;
    // cull-par (research/184 A1): the main camera's octree walks split below each
    // partition's root, the subtrees walked on the cull threads (plus/cullinto in
    // newview). Same lists, same order. sCullParVerify: walks both ways and
    // compares (what is drawn comes from the usual walk); mismatches are counted
    // (RHIProfile "cull par"), the first logged. LLRHI_CULL_PAR: unset or 0 off
    // (the default: research/184 9, the gain was 0.02 to 0.10 ms), 1 on, verify
    // (verify only), auto: verify first, then on once enough walks compared with
    // no mismatch, off at the first (sCullParAuto, cancelled by a cue).
    // cullpar.toggle / cullparverify.toggle flip them. Needs sCullThreads.
    extern bool sCullPar;
    extern bool sCullParVerify;
    extern bool sCullParAuto;
    // cull-reuse (research/188): while the main camera stands still (the same
    // planes, origin and corner distance as at its cull before) a partition
    // whose octree did not change (same root, clean, same unbound count) has
    // its walk replayed from the trace of the walk before: the frustum tests
    // and the descent are skipped, the occlusion checks and markNotCulled run
    // as before (plus/cullinto/cullreuse in newview). Same lists, same order.
    // sCullReuseVerify: every traced or replayed walk is done again as before
    // with the same occlusion answers and compared (what is drawn comes from
    // the walk as before); mismatches are counted (VKPerf "cull reuse"), the
    // first logged. LLRHI_CULL_REUSE: unset or auto (the default) verify
    // first, on once enough replays of the world compared with no mismatch,
    // off at the first (sCullReuseAuto, cancelled by a cue); 0 off, 1 on,
    // verify (verify only); LLRHI_CULL_REUSE_VERIFY=1 also verify only.
    // cullreuse.toggle / cullreuseverify.toggle flip them.
    extern bool sCullReuse;
    extern bool sCullReuseVerify;
    extern bool sCullReuseAuto;
    // The static shadow cache's per-frame sort of the cascades' groups (draw
    // info fingerprints, static / joining / moving) runs in chunks of groups on
    // the cull threads, and a cascade's cached casters are listed only when its
    // cached depth is redrawn. Same lists, same order, same fingerprints. On
    // unless LLRHI_SHADOW_SORT_THREADS=0; needs sCullThreads (research/57).
    extern bool sShadowSortThreads;
    // The GPU shadow list's per-pass scan of its groups (LLPipeline::rhiListPhase)
    // finds the groups with something to do on the cull threads; the main thread
    // then handles those in the same order. On unless LLRHI_SHADOW_LIST_THREADS=0;
    // needs sCullThreads (research/57).
    extern bool sShadowListThreads;
    // The realtime reflection probe's six faces walk the octrees once for all
    // six (LLPipeline::updateCull, getVisibleExtents); each face still gets
    // exactly the groups and shadow point set its own walk would find.
    // Vulkan mode only. On unless LLRHI_PROBE_SHARED_CULL=0 (research/44, P2).
    extern bool sProbeSharedCull;
    // The realtime reflection probe's faces keep the G-buffer of their static
    // PASS_SIMPLE prefix (the batches of static groups the face draws first) and
    // copy it in instead of drawing those batches again while nothing in it
    // changed (LLPipeline::probeStaticDraw). Vulkan mode only. Off unless
    // LLRHI_PROBE_STATIC_CACHE=1 (research/44, P5).
    extern bool sProbeStaticCache;
    // Mixes into hash the versions of the GL objects a draw reads: each
    // texture's (it changes whenever the texture's image, contents, levels or
    // sampling parameters may change) and each buffer's (whenever its data is
    // written). Versions are unique across objects and names. False when one has
    // none yet, or is a texture something renders into (its contents change
    // without a new version). Null: not available.
    extern bool (*sObjectVersions)(const U32* textures, U32 texture_count, const U32* buffers, U32 buffer_count, U64& hash);
    // Counters that only grow: programs linked or deleted, draws or copies the
    // backend skipped (table full, unsupported input, ...), and texture versions
    // handed out (any texture changed: texture threads upload at any time).
    // Null: not available.
    extern void (*sBackendCounters)(U32& program_changes, U32& skips, U32& texture_versions);
    // The backend's device memory (VRAM against the driver's budget, its own
    // pools), for the texture memory log (research/54). Render thread. Null:
    // not available.
    extern std::string (*sMemoryReport)();
    // Set by the Vulkan backend (research/109): a short line for the GPU
    // fault report's last frame events (snapshots, resizes, texture
    // pressure). Copied into a fixed ring; any thread. Null in GL.
    extern void (*sNoteEvent)(const char* text);
    // Set by the Vulkan backend (research/109): images made (llvkgl textures,
    // OIT) and released since the start, and textures encoded BC7, for the
    // log lines after a snapshot. Render thread. Null in GL.
    extern void (*sResourceCounts)(U64& images_made, U64& images_released, U64& bc7_encoded);
    // Writes every live VMA allocation (name, size) to path, for finding what
    // keeps growing (research/75: VRAM growth bug). Null: not available.
    // Expensive (walks every block); call it rarely.
    extern void (*sDumpAllocations)(const std::string& path);
    // research/78: the backend's device-local memory in use, the driver's
    // budget for it (VK_EXT_memory_budget) and the bytes its texture images
    // take (every mip level, RGB stored as RGBA, as allocated). False when the
    // driver gives no budget. Cheap (no driver call each time). Null: not
    // available (GL, or Vulkan with GL drawing).
    extern bool (*sVRAMBudget)(U64& texture_bytes, U64& usage, U64& budget);
    // research/78: with sVRAMBudget, the texture budget (discard bias) counts
    // textures at what they take in Vulkan, once, and keeps them under the
    // driver's budget less what everything else in VRAM takes and a margin.
    // Upstream counts GL sizes twice against a share of the VRAM size, which
    // under Vulkan let VRAM run past the driver's budget (4-5 fps). On unless
    // LLRHI_VK_BUDGET_REAL=0; vrambudget.toggle.
    extern bool sVRAMBudgetReal;
    // research/79: what each named texture's image takes (allocation size,
    // every level), written to bytes[i] for names[i] (0: no image). One call
    // for many names (one lock). Main thread. Null: not available.
    extern void (*sTextureImageBytes)(const U32* names, U32 count, U64* bytes);
    // research/79: one line per VMA pool (kind, size class, bytes used, block
    // bytes, blocks, allocations), for the texture census. Null: not available.
    extern std::string (*sPoolDetail)();
    // research/79: texture pressure. While the real budget (sVRAMBudget) is
    // short, textures are held no sharper than the screen can show (the
    // camera boost limited), and once the discard bias is at its top and VRAM
    // still runs over, textures no face of which has been on screen for a
    // while go down further, the longest unseen first. Nothing changes while
    // VRAM has room. On unless LLRHI_TEX_PRESSURE=0; texpressure.toggle.
    extern bool sTexPressure;
    // research/79: LLRHI_TEX_CENSUS=1 (every 120 s) or =<seconds>: the texture
    // census in the log (what holds texture memory and why). 0: off.
    extern U32 sTexCensusSeconds;
    // research/245: the GL buffers' Vulkan copies are given back while VRAM is
    // short. Freed buffers left the GL buffer pools' blocks part full (a heavy
    // place left by a teleport: 977 MB used in 3168 MB of blocks for minutes),
    // VRAM stayed past the driver's budget and the frame paged at 1-5 fps.
    // While VRAM is short (from 92% of the driver's budget, or earlier while
    // the discard bias is up, on down to 64 MB under that start, while the
    // bias is up to 85% or 64 MB under the start, the lower, until nothing
    // more is to be given back: llvkgl's vram_compaction_check), every buffer of the
    // blocks least in use (what was drawn within sBufferCompactIdleSeconds at
    // most half the block) drops its Vulkan copy (each keeps its CPU copy):
    // one drawn lately is made again at its next draw in a fuller block, the
    // others only when drawn again, and the emptied blocks go back to the
    // driver (the copies are made again at most 64 MB a frame). Set by
    // plus/tplag when PlusBufferCompact (on by default since research/285) or
    // PlusBufferCompactIdle changes (main thread; read at the start of a
    // frame). LLRHI_VK_BUFFER_COMPACT=0: never, whatever the setting.
    extern std::atomic<bool> sBufferCompact;
    // Undrawn this long (0 to 60 s), a buffer does not count as in use when its
    // block is weighed. 0: every buffer counts as in use, so only blocks at
    // most half full are emptied (all their buffers go, drawn or not).
    extern std::atomic<F32> sBufferCompactIdleSeconds;
    struct BufferCompactCounts
    {
        U64 passes = 0;         // passes that dropped copies
        U64 buffers = 0;        // copies dropped
        U64 drawn_bytes = 0;    // of those, drawn in the last 2 s (made again at the next draw)
        U64 other_bytes = 0;    // the rest (made again only when drawn)
        // Now (not totals): the sized pools, bytes allocated in bytes of blocks.
        U64 gl_pool_used = 0, gl_pool_blocks = 0;
        U64 texture_pool_used = 0, texture_pool_blocks = 0;
    };
    // Totals since the start and the pools now (any thread; null: not available).
    extern void (*sBufferCompactCounts)(BufferCompactCounts& out);
    // research/285 X2: the same for the texture pools, while sBufferCompact is
    // on. Images freed as textures went down a size class or away left the
    // texture pools' blocks part full (2028 MB used in 3304 MB after an
    // event's peak) and VRAM past the driver's budget, the discard bias held
    // at 4. While VRAM is short (as sBufferCompact's, the same state), the
    // textures of the blocks at most half in use get new images in fuller blocks of their
    // pool, their texels copied on the GPU (BC7 stays BC7), so the emptied
    // blocks go back to the driver. Render targets, textures waiting for or in
    // a BC7 encoding and textures written in the last second or so stay. On
    // unless LLRHI_TEX_COMPACT=0 (LLRHI_VK_BUFFER_COMPACT=0 does not stop it);
    // texcompact.toggle (read at the start of a frame).
    extern bool sTexCompact;
    // research/285 (compact-hysteresis): while the discard bias or the texture
    // pressure is up, the VRAM use (bytes, as sVRAMBudget's usage) below which
    // they come down: the textures under 90% of their limit, that is the
    // driver's budget less the margin less a ninth of the textures
    // (LLViewerTexture::updateClass). 0: not known, or neither is up. Both
    // compactions start 128 MB under it if that is below the 92% where they
    // start otherwise, and run on to 64 MB under that start or to 85% of the
    // budget, the lower (without it they stopped above it, and the bias stayed
    // at 4). Main thread writes, the backend reads.
    extern std::atomic<U64> sVRAMBiasFallBytes;
    // research/285 (compact-hysteresis): the VRAM use (bytes, as sVRAMBudget's
    // usage) over which the discard bias rises for VRAM: the driver's budget
    // less the texture limit's margin (LLViewerTexture::updateClass:
    // LLRHI_VK_BUDGET_HEADROOM, or 5% of the budget within 256 to 512 MB).
    // 0: not known. The texture compaction moves no large image where its new
    // memory (before the old block goes, frames later) would pass it. Main
    // thread writes, the backend reads.
    extern std::atomic<U64> sVRAMBiasRiseBytes;

    // Geometry work per frame follows the frame instead of growing with it:
    // 10% of the last frame, 2..10 ms, for object creation and for building
    // visible groups that were never built (display(), LLPipeline::postSort).
    // Upstream gives creation 5% of the frame without a cap and builds every
    // visible group, so slow frames build more and get slower. New objects
    // appear later; nothing already shown changes. On unless LLRHI_GEOM_BUDGET=0.
    extern bool sGeomBudget;
    // With sGeomBudget: after a login or a teleport, never-built objects build
    // only within a radius around the camera that starts at 15 m and grows by 20%
    // a second once, for 1 s running, nothing inside waited to build or for its
    // mesh and few on-screen textures were still blurry. After 10 s without such growth it
    // grows by 5% a second (at least 2 m/s) anyway, so everything still gets
    // built. The draw distance setting is not touched; the window title shows
    // the radius while it grows.

    // Built static groups marked for a rebuild (an object joined them, its mesh
    // arrived, its shape or texture changed) rebuild in queue order while the
    // frame's priority rebuilds stay within 10% of the last frame (2..10 ms, at
    // least one a frame); the rest keep their place and draw their old batches
    // until their turn (LLPipeline::rebuildPriorityGroups). A group whose old
    // batches may point at something gone, rigged ones, alpha re-sorts and every
    // other partition still rebuild at once. A change may show a few frames later
    // while loading; the image once the queue is empty is the same. On unless
    // LLRHI_PRIORITY_BUDGET=0.
    extern bool sPriorityBudget;
    // LLMeshRepository::notifyLoadedMeshes reads the clock once for all pending
    // requests' score ages instead of once each, keeps the sorted front of the
    // pending requests while no score could change and no request was added, and
    // drops the sent requests from the front at once. The same requests are sent
    // (in the order std::partial_sort gives, up to ties). On unless
    // LLRHI_MESH_NOTIFY_FAST=0.
    extern bool sMeshNotifyFast;

    // Texture work keeps upstream's 50 ms per second at low frame rates instead
    // of a fixed 5 ms per frame (up to 50 ms a frame below 10 fps), and the fetch
    // pass visits about every texture once a second instead of 5% of them per
    // frame (at 1 fps a texture waited 20 s between updates). Unchanged at 20 fps
    // and above. On unless LLRHI_TEXTURE_BUDGET=0.
    extern bool sTextureBudget;
    // The fetch pass's texture visits (LLViewerTextureList::updateImagesFetchTextures)
    // scan the faces of a block of 128 textures on the cull threads first (the
    // virtual size, nearest face and on-screen flag of each); the main thread
    // then makes the face updates the serial loop would have made, in the same
    // order, and does the rest of each visit as before. Same numbers, same
    // order (research/65). A texture with a rigged or media face due for its
    // pixel area update is visited the serial way. On unless
    // LLRHI_TEXTURE_VISIT_THREADS=0; texturevisit.toggle flips it. Needs sCullThreads.
    extern bool sTextureVisitThreads;
    // LLRHI_TEXTURE_VISIT_STATS=1 (or texturevisitstats.toggle): every 5 s a
    // RHITexVisit log line with the fetch pass's textures due, visited and
    // planned per frame, faces scanned, how often it ran out of time, and its ms.
    extern bool sTextureVisitStats;
    // idle-skip (research/178 G1): while the camera has stood still for a
    // second, no texture pressure, a discard bias of 1 and no teleport window
    // (plus/tprelease), a texture that has what it wants (no fetch, at its
    // desired level or sharper, unboosted, no light map use) is visited one
    // round of the fetch pass in TEXTURE_PACE_ROUNDS (about every 4 s) instead
    // of every round (about once a second). The camera moving puts every
    // texture back on the round. The RHITexVisit line counts the visits left
    // out ("paced") and the paced textures whose visit then wanted them
    // sharper ("late"). On unless LLRHI_TEXTURE_VISIT_PACE=0;
    // texturepace.toggle flips it.
    extern bool sTextureVisitPace;

    // --- Objects nobody sees (idle-skip, research/178 D1, D2) --------------
    // plus/idleskip: an object no camera took in the last 3 frames steps its
    // texture animation (sIdleSkipTexAnim) or its spin, llTargetOmega
    // (sIdleSkipOmega), one frame in 4 instead of every frame; both are
    // functions of time, so the next step lands where it would have been. On
    // unless LLRHI_IDLE_SKIP_TEXANIM=0 / LLRHI_IDLE_SKIP_OMEGA=0;
    // idleskiptexanim.toggle / idleskipomega.toggle flip them.
    // LLRHI_IDLE_SKIP_STATS=1 (idleskipstats.toggle): an IdleSkip line every 5 s.
    extern bool sIdleSkipTexAnim;
    extern bool sIdleSkipOmega;
    extern bool sIdleSkipStats;
    // --- Linksets kept static (plus/activecensus, research/180 10.12) -------
    // R1: when a parent arrives after its children (the orphan reunion in
    // LLViewerObjectList::findOrphans), upstream marks the parent moved, and
    // its move makes the whole linkset active for good even though nothing
    // moved. sActiveOrphan: a static root parent whose position, rotation
    // and scale are already the object's (no velocity, spin, selection,
    // attachment, flex, animesh) is left static, as when the parent came
    // first. Off unless LLRHI_ACTIVE_ORPHAN=1 (activeorphan.toggle).
    // sActiveOrphanVerify (on unless LLRHI_ACTIVE_ORPHAN_VERIFY=0,
    // activeorphanverify.toggle): 30 frames after each such reunion, the root's
    // and the children's matrices are compared with the object's values (what
    // upstream's move would have set); the ActiveCensus line counts them.
    // R0, the census of why drawables became active, runs with
    // LLRHI_FRAME_COST=2 (sFrameCostDetail).
    extern bool sActiveOrphan;
    extern bool sActiveOrphanVerify;
    // research/180 10.15: upstream's LLDrawable::updateMove makes every moved
    // drawable active (its linkset for good), even when the move changes
    // nothing (a full update re-sending the same position, a parent put on the
    // move list by its child). sMoveSame: a static volume whose drawable
    // already has the object's position, rotation and scale (no velocity,
    // spin, physics, selection, attachment, flex, animesh, link change) stays
    // static; the rest of upstream's move runs. Off unless LLRHI_MOVE_SAME=1
    // (movesame.toggle). sMoveSameVerify (on unless LLRHI_MOVE_SAME_VERIFY=0,
    // movesameverify.toggle): right after each such move and 30 frames on,
    // the drawable's world position and rotation against the object's.
    extern bool sMoveSame;
    extern bool sMoveSameVerify;
    // research/180 9 1: "seen" answered from the object's memo for the two
    // frames a recent frame number keeps it seen (the culls only move those
    // numbers up). On unless LLRHI_IDLE_SKIP_MEMO=0 (idleskipmemo.toggle).
    // LLRHI_IDLE_SKIP_MEMO_VERIFY=1 (idleskipmemoverify.toggle): answered the
    // full way and compared (the IdleSkip line, every 5 s while it is on).
    extern bool sIdleSkipMemo;
    // research/179 (cpu-present): LLVOVolume::animateTextures builds a face's
    // texture matrix only when its five values (offset, scale, rotation)
    // differ from the face before; else it copies that face's (the same
    // arithmetic on the same values). On unless LLRHI_TEXANIM_MEMO=0 (or
    // texanimmemo.toggle). LLRHI_TEXANIM_MEMO_VERIFY=1 (texanimmemoverify.toggle)
    // builds every one, compares with the copy and counts differences (IdleSkip
    // line; must stay 0).
    extern bool sTexAnimMemo;
    extern bool sTexAnimMemoVerify;
    // research/194 11 (plus/texanim, llrhitexanim.h): a smooth texture
    // scroll's face matrices hold its description; the backend and llface.cpp
    // turn it into the matrix where they read it (at sTexAnimTime), and
    // LLViewerTextureAnim::updateClass leaves the animation out.
    // LLRHI_TEXANIM_GPU: auto (unset, the default: verify first, then on
    // once enough faces compared with no mismatch, off for the run at the
    // first); 0 off; 1 on; verify (upstream steps every animation, and each
    // step's face matrices are compared with the description's).
    // texanimgpu.toggle / texanimgpuverify.toggle flip them (a cue ends
    // auto). Descriptions are turned into matrices whatever these say.
    extern bool sTexAnimGpu;
    extern bool sTexAnimGpuVerify;
    extern bool sTexAnimGpuAuto;
    // The frame time (LLFrameTimer::getElapsedSeconds) animations are drawn
    // at: written at the top of LLViewerTextureAnim::updateClass, so it
    // stands still while that is not called (Animate Textures off, the
    // freeze), as upstream's matrices do. Read by any thread while drawing.
    extern double sTexAnimTime;
    // Descriptions turned into matrices by the backend (any thread; the log line).
    extern std::atomic<U32> sTexAnimDecodes;
    // m, or (when m is a texture animation's description) its matrix at
    // sTexAnimTime written into `scratch` (16 floats). Where the backend turns
    // a texture matrix into GPU data; the caller keeps m as its cache key.
    inline const F32* texAnimMatrix(const F32* m, F32* scratch)
    {
        if (!m || !LLRHITexAnim::isDescription(m))
        {
            return m;
        }
        LLRHITexAnim::matrixAt(m, sTexAnimTime, scratch);
        sTexAnimDecodes.fetch_add(1, std::memory_order_relaxed);
        return scratch;
    }
    extern bool sIdleSkipMemoVerify;
    // research/180 10.18 (seenby): the idle skip asks "seen" only when the
    // answer decides something. A hidden object's turn frame (the one in four
    // it steps anyway) steps without asking, and a child of a linkset whose
    // root's memo says seen is seen without reading the child's drawable
    // (the root found by the object's parent, already at hand). On unless
    // LLRHI_SEEN_BY_MEMO=0 (seenbymemo.toggle). Checked by
    // LLRHI_IDLE_SKIP_MEMO_VERIFY=1 (the root's answer against the full way).
    extern bool sSeenByMemo;
    // research/193 (idle-cheap) A: the idle objects segment of FrameCost
    // (gestures, agent position, the object list's update: every active
    // object's and avatar's idle update, flexible objects, texture
    // animations) cut into its steps by plus/idlecheap: time (TSC) and calls
    // a frame per step, a VKPerf line every 5 s. Off unless
    // LLRHI_IDLE_CENSUS=1 (idlecensus.toggle); off, a step is one flag test.
    extern bool sIdleCensus;
    // research/193 (idle-cheap) B: LLVOAvatar::visualParamWeightsAreDefault
    // (every frame for each avatar not fully loaded and each name tag; it
    // walks the avatar's ~250 parameters in a std::map with a dynamic_cast
    // each) answered from a list of the tweakable parameters made once per
    // avatar, the one off its default found last time looked at first.
    // Same answer (a list of the same parameters, read the same way).
    // LLRHI_VPARAM_MEMO: auto (unset, the default: verify first, then on
    // once enough answers compared with no mismatch, off for the run at the
    // first); 0 off; 1 on; verify (both ways each call, upstream's answer
    // used, mismatches counted in the census line or a line of their own).
    // vparammemo.toggle / vparammemoverify.toggle flip them (a cue ends auto).
    extern bool sVParamMemo;
    extern bool sVParamMemoVerify;
    extern bool sVParamMemoAuto;

    // --- Memos of per-frame work whose inputs stay the same (research/187) ---
    // N1, research/197 stage 3c (research/203, lllocallights.h; it took over
    // plus/lightsmemo): calcNearbyLights and the light loop of
    // renderDeferredLighting read a light that does not move from its record
    // in the local light table, read again only when the light changed, and
    // the nearby set takes its new distances in place while their order holds.
    // LLRHI_LIGHT_TABLE: auto (unset, the default: verify first, then on after
    // 600 frames with comparisons and no difference, off for the run at the
    // first); 0 / off; 1 / on; verify (the loops read the objects as before
    // and compare each record they would have read; LightTable line, must
    // stay 0). lighttable.toggle / lighttableverify.toggle flip them (a cue
    // ends auto).
    extern bool sLightTable;
    extern bool sLightTableVerify;
    extern bool sLightTableAuto;
    // N4 (plus/envmemo): LLEnvironment::update marks the shaders dirty as
    // before; a dirty shader bound later gets nothing when the sky and water
    // uniforms are what they were when it last had them, else
    // only the values that changed since it last had them. On unless
    // LLRHI_ENV_UNIFORM_MEMO=0 (envuniformmemo.toggle).
    // LLRHI_ENV_UNIFORM_MEMO_VERIFY=1 (envuniformmemoverify.toggle) compares
    // what each bound shader holds with every value (EnvMemo line; must stay 0).
    extern bool sEnvUniformMemo;
    extern bool sEnvUniformMemoVerify;

    // --- Alpha glow pass (alpha-cpu, research/178 F0) -----------------------
    // plus/alphapass: the alpha pool's glow pass leaves out the batches no face
    // of which glows (their glow draw adds 0; upstream draws every batch of a
    // group with one glowing face twice). Not in OIT sessions. Off unless
    // LLRHI_ALPHA_GLOW_SKIP=1 (measured slower, research/178 s11);
    // alphaglowskip.toggle flips it.
    // LLRHI_ALPHA_PASS_STATS=1 (alphapassstats.toggle): an AlphaPass line every
    // 5 s (batches, program binds, glow draws, CPU ms, and the verify count of
    // glowing faces in left out batches, which must stay 0).
    extern bool sAlphaGlowSkip;
    extern bool sAlphaPassStats;

    // --- Texture creation (research/70) -------------------------------------
    // Native uploads (sTexImage2D and the others below) convert the client
    // pixels straight into the staging memory the copy reads, instead of into
    // a buffer of their own that is then copied there: no allocation, one
    // pass. Same bytes. On unless LLRHI_TEX_DIRECT=0; texdirect.toggle.
    extern bool sTexDirect;
    // LLViewerTextureList::updateImagesCreateTextures creates up to 16 textures
    // (or 8 MB of pixels) and then finishes them together: the passes over
    // their pixels that need no GL state (the conversion into the staging
    // memory their copy was already recorded from, the pick mask, the copy for
    // the alpha analysis) run at once on the cull threads, then each texture is
    // finished (postCreateTexture) in the same order as before. Same commands
    // in the same order, same staging memory, same bytes, all before the frame
    // is submitted; a texture is never created later than in the serial loop.
    // Each texture's raw image lives until it is finished. On unless
    // LLRHI_TEX_BATCH=0; texbatch.toggle. Uses the cull threads when sCullThreads.
    extern bool sTexBatch;
    // A texture image given pixels (a fetched texture) is made without
    // attachment usage until it is attached to a framebuffer; then it is made
    // again with it and its texels copied over (same texels). On NVIDIA an
    // attachable image of 1024x1024 or more prefers memory of its own: one
    // kernel allocation per texture while loading. On unless
    // LLRHI_TEX_LAZY_ATTACH=0; texlazyattach.toggle (new images only).
    extern bool sTexLazyAttach;
    // LLRHI_TEX_STATS=1 (or texstats.toggle): every 5 s a RHITexCreate log line
    // with the textures created per frame, the MB converted (and how much of it
    // on the threads), and the main thread's ms in the creation loop.
    extern bool sTexStats;
    // While true on a thread, a native upload made by that thread may leave
    // the conversion of its pixels into the staging memory to sTexConvertJoin
    // (the copy command is recorded at once, as before). Whoever sets it keeps
    // the pixels alive and unchanged until sTexConvertJoin has returned.
    extern thread_local bool tTexConvertDefer;
    // A fork and join over count jobs (each index once, any thread), such as
    // the cull threads'. Null: run them on the calling thread.
    typedef void (*RunJobs)(U32 count, const std::function<void(U32)>& job);
    // Set by the Vulkan backend. Main thread: writes the pixels left so far,
    // together with the caller's extra_count jobs (extra), in one fork and join
    // on run, and returns when all are done.
    extern void (*sTexConvertJoin)(RunJobs run, U32 extra_count, const std::function<void(U32)>& extra);
    // Bytes the native uploads converted (all, and the part left to
    // sTexConvertJoin), and the part not joined yet. Counted by the backend.
    extern std::atomic<U64> sTexConvertBytes;
    extern std::atomic<U64> sTexConvertDeferredBytes;
    extern std::atomic<U64> sTexConvertPendingBytes;

    // --- Texture block compression (research/88) ---------------------------
    // The Vulkan backend keeps the textures the viewer lets it (tTexCompress
    // when they are made: the colour, normal and specular maps of objects) as
    // BC7 in VRAM, a quarter of RGBA8: they are made plain as before, and a
    // few a frame are encoded on the GPU into BC7 images that take their
    // place. Devices with BC7 only. On unless
    // LLRHI_TEX_BCN=0; bcn.toggle (for textures made from then on).
    extern bool sTexBCN;
    // While true on a thread, a texture image specified by that thread (level
    // 0 with pixels) may be stored compressed.
    extern thread_local bool tTexCompress;
    struct TexCompressScope
    {
        explicit TexCompressScope(bool allow) : mSaved(tTexCompress) { tTexCompress = allow; }
        ~TexCompressScope() { tTexCompress = mSaved; }
        bool mSaved;
    };
    // research/88: whether each named texture is stored compressed now
    // (out[i] for names[i]). Main thread. Null: not available.
    extern void (*sTextureCompressed)(const U32* names, U32 count, bool* out);
    // research/285 (compact-hysteresis): the texture `name` is queued for a
    // smaller image (LLViewerLODTexture::scaleDown; hold false once
    // LLViewerTextureList's scale-down queues took it). The backend's texture
    // pool compaction leaves a held texture where it is: its image is about
    // to be replaced, a copy of it would be in vain. Main thread. Null: no
    // backend.
    extern void (*sTextureMoveHold)(U32 name, bool hold);
    // research/88: one line on the compressed textures (count, MB against
    // plain, the queue, the quality checks). Null: not available.
    extern std::string (*sTexBCNReport)();

    // A skinning palette's row in the backend's frame palette table (research/56,
    // sPalettePrebuild below).
    struct PaletteRow
    {
        U32 base = 0;           // first vec4 of the row
        U32 room = 0;           // vec4s reserved from base
        U32 frame = U32_MAX;    // the backend frame whose table holds it; U32_MAX: none
    };

    // --- Native shadow casters ---------------------------------------------
    // In Vulkan mode the viewer hands shadow casters over as packets instead of
    // drawing them through GL (LLPipeline::renderShadowCasters).
    struct ShadowPacket
    {
        // Mirrors shadowV, shadowAlphaMaskF, shadowAlphaMaskF with IS_FULLBRIGHT, treeShadowF.
        enum Mode { MODE_OPAQUE = 0, MODE_ALPHA_MASK = 1, MODE_ALPHA_MASK_FULLBRIGHT = 2, MODE_ALPHA_TEST = 3 };
        U32 vertex_buffer = 0;      // GL buffer names
        U32 index_buffer = 0;
        U32 index_type = 0;         // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
        U32 first_index = 0;
        U32 index_count = 0;
        U32 position_offset = 0;    // LLVector4a positions (texture index in w)
        U32 texcoord_offset = U32_MAX;  // vec2 texcoord0, U32_MAX: none
        U32 color_offset = U32_MAX;     // RGBA8 color, U32_MAX: none
        U32 textures[4] = {};       // GL texture names (indexed by the vertex texture index)
        U32 mode = MODE_OPAQUE;
        F32 alpha_cutoff = 0.f;
        const F32* model = nullptr;             // column-major 4x4, null: identity
        const F32* texture_matrix = nullptr;    // column-major 4x4, null: identity
        // Rigged casters: joint matrices as uploaded to matrixPalette (mat3x4, 12
        // floats each), and the weight4 array. palette null: not rigged.
        const F32* palette = nullptr;
        U32 joint_count = 0;
        U32 weight_offset = U32_MAX;
        // sPalettePrebuild: the same joints' row in the frame's palette table,
        // read instead of copying palette (frame U32_MAX: none, copy palette).
        PaletteRow palette_row;
        // Vertices and indices the buffers hold (LLVertexBuffer::getNumVerts /
        // getNumIndices), so the backend can copy the pair whole (sGPUDriven).
        // 0: unknown (the packet is drawn from the GL buffers).
        U32 vertex_count = 0;
        U32 index_total = 0;
        // research/137 N7: this caster's culling. CULL_INHERIT: the pass's (the
        // GL state when it began, as every caster before); 0: none (a
        // double-sided material: LLGLDisable cull_face); GL_BACK or GL_FRONT.
        // A caster with culling of its own is never pooled (sGPUDriven draws
        // the pool with the pass's), and merging packets (pipeline.cpp's
        // rhi_continues) must keep casters of another culling apart.
        static constexpr U32 CULL_INHERIT = 0xffffffffu;
        U32 cull = CULL_INHERIT;
    };
    // --- GPU shadow list (research/46, stages G2 and G3) --------------------
    // Static opaque shadow casters (the region space batches of static volume
    // groups, terrain faces) are registered once as rows of GPU tables, their
    // buffers copied into the G1 geometry pool. Each shadow pass picks its rows
    // with a compute shader and draws them with one DrawIndexedIndirectCount per
    // pool buffer and index size. Rows belong to a spatial group:
    //  - G2: the CPU still culls and sorts. For a registered batch it sets the
    //    row's bit in the pass's accept set instead of building a packet.
    //  - G3: a resident group (every shadow caster of it registered, unchanged
    //    for a while) is left out of the sun shadow cameras' CPU cull; the
    //    compute shader tests its bounds against the camera's planes the way the
    //    CPU cull would, and draws its rows.
    // A row goes when GL writes or deletes either of its buffers (the pool's
    // copies follow GL) and when its group's draw map is cleared (rebuild, LOD
    // change). Call between lock() and unlock(), on the render thread.
    class ShadowList
    {
    public:
        struct Row
        {
            U32 vertex_buffer = 0;      // GL buffer names
            U32 index_buffer = 0;
            U32 index_type = 0;         // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
            U32 first_index = 0;
            U32 index_count = 0;
            U32 position_offset = 0;    // LLVector4a positions
            U32 vertex_count = 0;       // what the buffers hold (copied whole into the pool)
            U32 index_total = 0;
            const F32* model = nullptr; // the region's render matrix (column-major 4x4), never null
            F32 lo[3] = {};             // bounds of the batch's vertices in model space
            F32 hi[3] = {};
            bool loose = false;         // the bounds may miss vertices (terrain): no per-row test
            bool pool = false;          // drawn with the face pools (terrain)
        };
        virtual ~ShadowList() = default;
        virtual void lock() = 0;
        virtual void unlock() = 0;
        // The device, the shaders and the tables are there.
        virtual bool ready() = 0;
        // A pass could not draw its rows: stop using the list.
        virtual bool broken() const = 0;
        // A group handle; 0 when the table is full.
        virtual U32 addGroup() = 0;
        // Drops the group's rows; it is no longer resident.
        virtual void clearGroup(U32 group) = 0;
        // clearGroup, and the handle goes.
        virtual void removeGroup(U32 group) = 0;
        // A row of the group; 0 when it could not be registered (draw it as a packet).
        virtual U64 addRow(U32 group, const Row& row) = 0;
        virtual bool rowLive(U64 row) = 0;
        // True once after rows of the group were dropped (GL wrote their buffers).
        virtual bool takeDamaged(U32 group) = 0;
        // Makes the group resident with its cull bounds (model space center and
        // half size). Returns the serial it became resident with (> 0), 0 when refused.
        virtual U32 grant(U32 group, const F32 center[3], const F32 radius[3], const F32* model) = 0;
        virtual void revoke(U32 group) = 0;
        // The last serial grant handed out.
        virtual U32 serial() = 0;
        // Row handles stay below this: the bits an accept set needs.
        virtual U32 rowSpan() = 0;
    };
    // Set by the Vulkan backend when the device can do it.
    extern ShadowList* sShadowList;
    // What a shadow pass takes from the list.
    struct ShadowListPass
    {
        const U32* accept = nullptr;        // bit per row: registered rows the CPU sorted into this pass
        U32 accept_words = 0;
        bool volume = false;                // G3: resident volume groups whose serial is in [since_min, since_max]
        bool terrain = false;               // G3: resident terrain groups (pool rows, when pools)
        bool pools = false;                 // the pass draws the face pools
        U32 since_min = 0;
        U32 since_max = 0;
        // The shadow camera as its CPU cull tests group bounds (LLCamera::AABBInFrustum):
        // agent planes (normal, d) and their octant masks (>= 8: not tested).
        F32 planes[8][4] = {};
        U8 masks[8] = {};
        // Its left, right, bottom and top planes and origin, for leaving out rows
        // that lie entirely outside the view (as research/44 P1).
        F32 sides[4][4] = {};
        F32 origin[3] = {};
    };
    // Set by the Vulkan backend. Records the packets (and the list's rows when
    // list is not null) into the current draw framebuffer; false when it could
    // not (draw them through GL then).
    extern bool (*sDrawShadowPackets)(const ShadowPacket* packets, size_t count, const F32* view_proj, F32 target_width,
                                      const ShadowListPass* list);
    // Native shadow casters wanted (LLRHI_NATIVE_SHADOWS, default on with Vulkan).
    extern bool sNativeShadows;
    // Rigged casters too (LLRHI_NATIVE_RIGGED, default on).
    extern bool sNativeRiggedShadows;
    // Terrain casters too (LLDrawPoolTerrain::renderShadow; LLRHI_NATIVE_TERRAIN, default on).
    extern bool sNativeTerrainShadows;
    // Opaque packets that continue the one before (same buffers and model matrix,
    // the next index range) are merged into one draw (LLRHI_SHADOW_PACKET_MERGE, default on).
    extern bool sMergeShadowPackets;
    // GPU-driven drawing (research/46), stage G1: opaque shadow casters draw from
    // copies of their vertex and index buffers in a few large pool buffers, many
    // draws per vkCmdDrawIndexedIndirect instead of one command each. The same
    // triangles into the same depth. On unless LLRHI_GPU_DRIVEN=0 (the everyday
    // configuration since 2026-10-16; =0 turns off the whole family below).
    extern bool sGPUDriven;
    // Stage G2 (ShadowList): registered static batches go to the GPU list
    // instead of packets. With sGPUDriven; on unless LLRHI_GPU_DRIVEN_LIST=0.
    extern bool sGPUDrivenList;
    // Stage G3: the sun shadow cameras leave resident groups to the GPU list.
    // With sGPUDrivenList; on unless LLRHI_GPU_DRIVEN_CULL=0.
    extern bool sGPUDrivenCull;
    // GPU-driven drawing, stage G4b (research/46): consecutive G-buffer draws of
    // the programs LLViewerShaderMgr marks (LLRHI_DRAW_ROWS) that differ only in
    // their vertex/index buffers and push constants go out as one indirect draw
    // (vkCmdDrawIndexedIndirectCount) from a pool of their vertex attributes,
    // each draw's push constants a row of the frame's table (per-instance
    // data). The same draws in the same order with the same values.
    // LLRHI_DRAW_RUNS=1 / 0 decides; unset, it follows sGPUDriven (on by
    // default). drawruns.toggle flips it (programs are marked at startup only).
    extern bool sDrawRuns;
    // Set by the Vulkan backend: of its linked programs, how many carry the
    // draw runs mark (marked), read their push constants from a row when a
    // run says so (rows), and are on the shared pipeline layout (shared,
    // research/50 §8). LLViewerShaderMgr logs them after loading the shaders.
    extern void (*sDrawRunPrograms)(U32& marked, U32& rows, U32& shared);
    // research/111 B-SC: a shader reload with the backend's SPIR-V compiled on
    // several threads. Runs `reload` (LLViewerShaderMgr::setShaders) twice:
    // first with every link's compile handed to the compile workers (the
    // programs stay unbuilt, GL links as usual), then for real (each link
    // finds its compile done, or waits for it). False: nothing ran (GL, no
    // shader cache folder, LLRHI_SHADER_COMPILE_THREADS=1, or called from
    // inside a pass of a parallel reload already running): reload the usual
    // way. Main thread. Null: not available.
    extern bool (*sParallelShaderReload)(void (*reload)());
    // research/111 B-PW: the shader prewarm. On unless LLRHI_SHADER_PREWARM=0.
    extern bool sShaderPrewarm;
    // research/111 B-PW: compiles on low priority threads, into the shader
    // cache only, this session's programs as they would be with each of
    // `targets` in place of `current` as LLGLSLShader::sGlobalDefines (the
    // defines every feature library file gets: what a graphics preset
    // changes), and the OIT variants made in earlier sessions. A new call
    // replaces the work not started yet. Main thread. Null: not available.
    extern void (*sPrewarmShaders)(const std::map<std::string, std::string>& current,
                                   const std::vector<std::map<std::string, std::string>>& targets);
    // research/159 3.1 (--bake-shaders, plus/shaderbake): what one step of the
    // shader bake built.
    struct ShaderBakeCounts
    {
        U32 builds = 0;         // the backend's shader builds
        U32 compiled = 0;       // of them, compiled (the rest were in the cache already)
        U32 cache_hits = 0;
        U32 failed_programs = 0;    // OIT variants and passes that did not build
        U32 oit_variants = 0;   // OIT variants made (every alpha program's)
        U32 fixed_passes = 0;   // the backend's own passes set up
        F64 seconds = 0.0;
    };
    // research/159 3.1: one step of the shader bake, into the shader cache.
    // Runs `reload` (LLViewerShaderMgr::setShaders) as sParallelShaderReload
    // does (plainly with one compile thread), then, with oit, the OIT variant
    // of every linked program that has one (not only those made before), and
    // with fixed, the backend's own passes (the full-screen passes, OIT, the
    // occlusion marks, the GPU lists, BC7), all ahead of their first use.
    // False: no shader cache folder (nothing would be kept). Main thread.
    // Null: not available.
    extern bool (*sBakeShaders)(void (*reload)(), bool oit, bool fixed, ShaderBakeCounts& out);
    // research/250: the background shader reload. plus/shaderreload runs
    // LLViewerShaderMgr::setShaders once as a plan (LLGLSLShader::sPlan:
    // every createShader hands its program's sources here and leaves the
    // shader drawn with as it is), the backend builds them on low priority
    // threads while the frames go on with the shaders in use, and a plain
    // setShaders at the head of a later frame (the commit) links with the
    // builds instead of making them. Main thread unless said. Changes no draw
    // state: not a draw list hook (an object, as ShadowList).
    struct ShaderReloadProgress
    {
        U32 planned = 0;        // programs handed over by the plan
        U32 built = 0;          // of them, done (built or not)
        U32 failed = 0;         // of them, not built
        U32 thrown = 0;         // of the failed, those whose build threw (made the usual way, it would again)
        U32 threads = 0;        // the build threads
        U32 compiled = 0;       // the builds compiled (the rest from the cache or the bundle)
        U32 cache_hits = 0;
        U32 bundle_hits = 0;
        F64 compile_ms = 0.0;   // summed over threads
        F64 seconds = 0.0;      // from begin() to the last build done (or to now)
    };
    class ShaderReloadBuilder
    {
    public:
        virtual ~ShaderReloadBuilder() = default;
        // A new plan: what an earlier one built or still builds is dropped.
        virtual void begin() = 0;
        // In the plan pass (LLGLSLShader::planShader): the sources GL has
        // attached to gl_program (a program of the plan's own) go to the
        // build threads. name: the shader's, for the log. False: not taken.
        virtual bool plan(U32 gl_program, const char* name) = 0;
        // The plan pass is over (every program handed over).
        virtual void planned() = 0;
        // How far the build is. Any thread.
        virtual void progress(ShaderReloadProgress& out) = 0;
        // The names of the programs that did not build, ", " between them.
        virtual std::string failures() = 0;
        // Around the commit's setShaders: its links take the builds of the
        // same sources. commitEnd drops what was not taken; made: the
        // setShaders ran (got past its early returns), then it logs what was
        // taken and starts what follows a reload (the OIT variants' prewarm).
        virtual void commitBegin() = 0;
        virtual void commitEnd(bool made) = 0;
        // Drops the plan: queued builds, results, those still running when
        // they end (a newer request, a reload made now, the logout).
        virtual void cancel() = 0;
    };
    extern ShaderReloadBuilder* sShaderReloadBuilder;   // null: none (GL)
    // Stage G4c (research/46 §11): draw templates. A batch that joined a draw
    // run keeps a template in the backend (its pool placement, vertex format
    // and program), named by a slot the batch owns (LLDrawInfo /
    // LLVertexBuffer::mRHIDrawTemplate, DirectDraw::draw_template). Later, the
    // viewer offers the batch by its template (sDrawTemplate) before laying out
    // its attributes: when the open run may take it, the backend appends its
    // indirect command and row without the rest of the draw path; otherwise
    // the batch goes the usual way. Needs sDrawRuns; on unless
    // LLRHI_DRAW_TEMPLATES=0, drawtemplates.toggle flips it.
    extern bool sDrawTemplates;
    // Stage G5 (research/46): the reflection probe faces draw the opaque
    // PASS_SIMPLE batches of static groups from a GPU list. A group's batches
    // are registered once (vertices in G4b's attribute pool, region space
    // bounds, texture names); each face hands the list its static groups in
    // draw order, a compute shader drops the batches that lie outside the
    // face (research/44 P1) and writes the indirect commands and each draw's
    // row (G4b), and the face skips their per-batch sort and recording. The
    // same draws in the same order with the same values. With sGPUDriven and
    // sDrawRuns; on unless LLRHI_GPU_DRIVEN_PROBE=0, gpuprobe.toggle.
    extern bool sGPUDrivenProbe;
    // Research/50 step 4: the probe list for every opaque G-buffer program on
    // the shared layout (not only PASS_SIMPLE), each face's batches of a render
    // type drawn by one vkCmdExecuteGeneratedCommandsEXT per vertex format and
    // render state (VK_EXT_device_generated_commands: the GPU binds each
    // batch's pool chunk). Batches of the other types are registered from what
    // their pool's own draw would push (a capture). With sGPUDriven and
    // sDrawRuns. Who wants it is sGPUDrivenDGCChoice: the device decides once
    // at startup (LLVKDevice::createDevice, with the chosen GPU's vendor) and
    // sets this to that choice; it enables the extension when the choice and
    // sGPUDriven are on and the GPU has it (gpudgc.toggle then flips the use);
    // without it the faces fall back to G5 / the CPU.
    extern bool sGPUDrivenDGC;
    // Hardware ray tracing (research/230): the device enabled VK_KHR_ray_query and
    // the acceleration structure extension. A capability only; true says the GPU
    // can, not that anything is built. rayTracingStatus is "on" or why not.
    extern bool sRayTracing;
    const char* rayTracingStatus();
    void setRayTracingStatus(bool on, const std::string& why);   // the device, once
    // research/230 R2: what the viewer wants of ray tracing this frame, asked once a
    // frame at the submission (main thread; null while the viewer set none: then
    // nothing is built). on: the person turned it on (plus::raytracing::enabled).
    // Off, the backend frees what it built. range: metres around the camera whose
    // static opaque geometry goes into the acceleration structure. camera: the
    // camera in the scene's space (agent space). opaque_passes: a bit per scene
    // pass (render type, SCENE_MAX_PASSES) whose records are opaque and static.
    struct RayTracingWanted
    {
        bool on = false;
        F32 range = 64.f;
        F32 camera[3] = {};
        U32 opaque_passes[4] = {};
    };
    extern bool (*sRayTracingSource)(RayTracingWanted& out);
    // dgc-nvidia-only (research/124 V5): checked on the RTX 2080 only, so in
    // the alpha DGC is on by default on NVIDIA GPUs only (vendor 0x10DE).
    // LLRHI_GPU_DRIVEN_DGC=0/1 (development) wins; otherwise the setting
    // PlusGpuDrivenDGC (plus/gpudgc, read before the window and the device
    // are made): 0 auto, 1 on, 2 off. Read once, at device creation.
    enum class DGCChoice : U8
    {
        Auto,       // NVIDIA's GPUs only (the default)
        On,         // PlusGpuDrivenDGC 1: any GPU that has it
        Off,        // PlusGpuDrivenDGC 2
        EnvOn,      // LLRHI_GPU_DRIVEN_DGC set, not 0
        EnvOff,     // LLRHI_GPU_DRIVEN_DGC=0
    };
    extern DGCChoice sGPUDrivenDGCChoice;
    // fix-round9b 4: set once generated commands have been recorded this run
    // (vkCmdExecuteGeneratedCommandsEXT). Read when the device is lost: plus/gpudgc then turns DGC off for
    // the next start.
    extern std::atomic<bool> sGPUDrivenDGCUsed;

    // --- Texture numbers (research/197 stage 1a) -----------------------------
    // A texture's number for its whole life (LLImageGL::rhiTex). The image
    // behind it changes (another discard level, an image made on the image
    // thread, freed under VRAM pressure) while the number stays; the backend
    // keeps "number -> its image now, and the bindless slot that image samples
    // from while that stays true" (llvkgl_texref.cpp). Draws name a texture by
    // its number (a draw handle, below) instead of by its GL name, and the
    // backend takes the slot from the number's entry: no name to read in the
    // viewer, no table by name in the backend. 0: none.
    struct TexRef
    {
        U32 id = 0;
    };
    // The sampler a texture's options make: LLImageGL's address mode, and its
    // filter option with or without mips. The backend's sampler table turns
    // samplerIndex(address, filter) into sampler parameters, the anisotropy
    // level (Textures::setAnisotropy) for every texture at once.
    enum SamplerAddress : U32
    {
        SAMPLER_WRAP = 0,       // LLTexUnit::TAM_WRAP
        SAMPLER_MIRROR,         // TAM_MIRROR
        SAMPLER_CLAMP,          // TAM_CLAMP
        SAMPLER_ADDRESS_COUNT
    };
    enum SamplerFilter : U32
    {
        // LLTexUnit::eTextureFilterOptions * 2 + (1 with mips)
        SAMPLER_POINT = 0, SAMPLER_POINT_MIPS,
        SAMPLER_BILINEAR, SAMPLER_BILINEAR_MIPS,
        SAMPLER_TRILINEAR, SAMPLER_TRILINEAR_MIPS,
        SAMPLER_ANISOTROPIC, SAMPLER_ANISOTROPIC_MIPS,
        SAMPLER_FILTER_COUNT
    };
    constexpr U32 SAMPLER_COUNT = SAMPLER_ADDRESS_COUNT * SAMPLER_FILTER_COUNT;
    struct TextureDesc
    {
        // The backend's image: the texture's GL name until the translation
        // layer goes (research/197 stage 7); 0: none yet.
        U32 image = 0;
    };
    struct Textures
    {
        // A new number for a texture (any thread; the backend locks). {0}
        // without a backend: the texture is then named by its GL name.
        static TexRef create(const TextureDesc& desc);
        // The number's texture has another image now (0: none). Draws from
        // here on sample it; what the number published goes.
        static void replaceImage(TexRef ref, U32 image);
        // The texture is gone. The number is given out again only after the
        // frames that may still name it.
        static void retire(TexRef ref);
        // A pure function: the sampler number of an address mode and a filter.
        static constexpr U16 samplerIndex(U32 address, U32 filter)
        {
            return (U16)(address * SAMPLER_FILTER_COUNT + filter);
        }
        // The anisotropy level of SAMPLER_ANISOTROPIC* (0: the device has
        // none): the sampler table's, for every texture at once.
        static void setAnisotropy(F32 level);
        // Verify (sTexHandlesVerify): the number's image must be `image`,
        // the GL name the texture has now. Counted by the backend. False:
        // it is not (or no backend): the draw then names it by `image`, as
        // before, so verify never draws another image.
        static bool verify(TexRef ref, U32 image, U16 sampler);
        // The number's sampler (LLImageGL::rhiSampler: its address mode,
        // filter and mips) is another now. Any thread (LLImageGL::setImage
        // runs on the image thread too). The retained scene's records hold
        // samplers by value and are described again (research/198 3.1); the
        // listener must only note the number (it may run off the main thread).
        typedef void (*SamplerListener)(TexRef ref);
        static void samplerChanged(TexRef ref);
        static void setSamplerListener(SamplerListener listener);

        // Set by the Vulkan backend (all null: no numbers, draws go by name).
        struct Backend
        {
            TexRef (*create)(const TextureDesc& desc) = nullptr;
            void (*replaceImage)(TexRef ref, U32 image) = nullptr;
            void (*retire)(TexRef ref) = nullptr;
            void (*setAnisotropy)(F32 level) = nullptr;
            bool (*verify)(TexRef ref, U32 image, U16 sampler) = nullptr;
        };
        static void install(const Backend& backend);
    };
    // A draw handle: what DirectDraw::textures and PacketEntry::names hold
    // per unit. A GL name, or (the TexRef switch on) a texture's number and
    // its sampler under TEX_HANDLE_BIT. Bit 30 is 0 in a handle, so
    // DirectDraw::KEEP_UNIT (all bits) is never one; GL names never reach
    // bit 31.
    constexpr U32 TEX_HANDLE_BIT = 0x80000000u;
    constexpr U32 TEX_HANDLE_ID_BITS = 22;
    constexpr U32 TEX_HANDLE_ID_MASK = (1u << TEX_HANDLE_ID_BITS) - 1;
    constexpr U32 TEX_HANDLE_SAMPLER_MASK = 0xffu;
    inline U32 texHandle(TexRef ref, U16 sampler)
    {
        return TEX_HANDLE_BIT | (((U32)sampler & TEX_HANDLE_SAMPLER_MASK) << TEX_HANDLE_ID_BITS) | (ref.id & TEX_HANDLE_ID_MASK);
    }
    inline bool isTexHandle(U32 value) { return (value & 0xc0000000u) == TEX_HANDLE_BIT; }
    inline TexRef texHandleRef(U32 handle) { return TexRef{ handle & TEX_HANDLE_ID_MASK }; }
    inline U16 texHandleSampler(U32 handle) { return (U16)((handle >> TEX_HANDLE_ID_BITS) & TEX_HANDLE_SAMPLER_MASK); }
    // LLRHI_TEX_HANDLES: on (unset, the default since research/218 vk-s7f);
    // 0 off (draws name textures by GL name, as before); 1 on; verify; auto
    // (verify first, then on once enough of the world's draws compared with
    // no mismatch, off for the run at the first: the default before). Verify: draws hand numbers over, the backend resolves each the
    // usual way from the number's image and compares the slot the number
    // published, and the viewer compares each number's image with the GL
    // name the texture has (a VKPerf "numbers [...]" count).
    // texhandles.toggle / texhandlesverify.toggle flip them (a cue ends auto).
    extern bool sTexHandles;
    extern bool sTexHandlesVerify;
    extern bool sTexHandlesAuto;

    // research/293 branch 1 (vb-src-streams): the attribute pool keeps its streams by the LL array an
    // attribute reads (DirectDraw::Attribute::source) instead of by the shader's location, so an array
    // is copied once however many locations read it (TEXTURE_INDEX reads the positions' w: no stream of
    // its own). On unless LLRHI_VB_SOURCE_STREAMS=0 (the pool by location and stride, as before). Read
    // once at start; no cue flips it (the pool's streams would change under the records).
    extern bool sVBSourceStreams;

    // --- Direct draws --------------------------------------------------------
    // In Vulkan mode the shared batch loops of the draw pools (LLRenderPass::
    // pushBatch and friends) hand each draw over as a DirectDraw instead of
    // binding its textures, setting up vertex attributes and syncing matrices
    // through GL. Everything else (program, render state, the pool's own
    // uniforms and texture units) is the GL state the viewer set, as the
    // translation layer recorded it; shaders stay the translated upstream ones.
    struct DirectDraw
    {
        // One vertex attribute, as glVertexAttrib(I)Pointer would get it.
        struct Attribute
        {
            U32 location = 0;       // LLVertexBuffer::AttributeType
            U32 size = 0;           // components
            U32 type = 0;           // GL_FLOAT, GL_UNSIGNED_BYTE, ...
            bool normalized = false;
            bool integer = false;   // glVertexAttribIPointer
            U32 stride = 0;
            U32 offset = 0;         // bytes into the vertex buffer
            // research/293 branch 1: the LL array (LLVertexBuffer::AttributeType) this attribute reads, and
            // where in that array's stream the attribute starts (bytes): TEXTURE_INDEX reads the w of the
            // positions (source TYPE_VERTEX, 12). SOURCE_AS_LOCATION (the default): the array of its own
            // location, offset 0 (what every attribute did before). `offset` above stays the place in the
            // GL buffer; the array starts at offset - source_offset. Read, through streamKey() and
            // streamShift() (they test sVBSourceStreams), by the attribute pool, the draw runs, draw lists,
            // the scene, the probe lists' stream tables and the draw list's attribute compare.
            static constexpr U32 SOURCE_AS_LOCATION = 0xffffffffu;
            U32 source = SOURCE_AS_LOCATION;
            U32 source_offset = 0;
            // The pool stream's key (the location where the pool keeps streams by location), and the bytes
            // to add to the stream's binding offset.
            U32 streamKey() const { return sVBSourceStreams && source != SOURCE_AS_LOCATION ? source : location; }
            U32 streamShift() const { return sVBSourceStreams && source != SOURCE_AS_LOCATION ? source_offset : 0; }
        };
        static constexpr U32 MAX_ATTRIBUTES = 16;
        U32 vertex_buffer = 0;      // GL buffer names
        U32 index_buffer = 0;
        U32 index_type = 0;         // GL_UNSIGNED_SHORT / GL_UNSIGNED_INT
        U32 first_index = 0;
        U32 index_count = 0;        // 0: not indexed, draws vertex_count vertices from first_vertex
        U32 first_vertex = 0;
        U32 vertex_count = 0;
        U32 mode = 0x0004;          // GL primitive mode (GL_TRIANGLES)
        const Attribute* attributes = nullptr;
        U32 attribute_count = 0;
        // GL texture names (or draw handles, research/197 1a) for texture units
        // 0..texture_count-1; KEEP_UNIT keeps the unit's GL binding.
        static constexpr U32 KEEP_UNIT = 0xffffffffu;
        const U32* textures = nullptr;
        U32 texture_count = 0;
        // What LLRender::syncMatrices would upload for this draw (column-major);
        // null leaves the program's uniform as GL set it.
        const F32* modelview = nullptr;             // mat4
        const F32* modelview_projection = nullptr;  // mat4
        const F32* normal = nullptr;                // mat3, 9 floats
        const F32* inverse_modelview = nullptr;     // mat4
        const F32* texture_matrix0 = nullptr;       // mat4
        // Non-zero and the same as an earlier direct draw's: the same matrices
        // (the translation layer may reuse that draw's uniform copy).
        U32 matrix_id = 0;
        // Instances (gl_InstanceIndex 0..instance_count-1).
        U32 instance_count = 1;
        // Programs that read per-instance rows from the frame's table (sTableRows,
        // the light table rewrite): the first row's vec4 index, pushed with the
        // draw. NO_TABLE: none (such a program reads its uniforms instead).
        static constexpr U32 NO_TABLE = 0xffffffffu;
        U32 table_base = NO_TABLE;
        // Vertices and indices the vertex and index buffers hold (LLVertexBuffer::
        // getNumVerts / getNumIndices), so the backend can copy the pair whole
        // (sDrawRuns). 0: unknown (the draw reads the GL buffers).
        U32 buffer_vertices = 0;
        U32 buffer_indices = 0;
        // The batch's draw template slot (sDrawTemplates): 0 or the id the
        // backend wrote there when the batch joined a draw run. Null: none.
        U32* draw_template = nullptr;
        // research/197 stage 1b: the vertex buffer's number (LLRHI::BufferRef,
        // llrhiresource.h), sent while LLRHI::sVBNative; 0: the pair goes by
        // its GL names. The names and sizes above stay (the backend checks
        // the number's place was made for them).
        U32 vb = 0;
    };
    // Set by the Vulkan backend. Records the draw with the current GL state;
    // false when it could not (the caller draws it through GL then).
    extern bool (*sDirectDraw)(const DirectDraw& draw);
    // Set by the Vulkan backend (sDrawTemplates): appends the batch whose
    // template *draw.draw_template names to the open draw run. The draw comes
    // without attributes (the template has them). False: nothing was recorded;
    // the caller lays out the attributes and calls sDirectDraw.
    extern bool (*sDrawTemplate)(const DirectDraw& draw);
    // Set by the Vulkan backend: the slot holding template `id` goes away.
    extern void (*sDrawTemplateReleased)(U32 id, const U32* slot);

    // --- Draw packets (research/180 stage 4, P1) -----------------------------
    // The legacy material loop's static batches (plus/packet, called from
    // LLDrawPoolMaterials::renderDeferred) go to the backend as one packet per
    // stretch of batches instead of one draw (and its uniform calls) each. A
    // batch's draw template (sDrawTemplates) gets a record beside it the first
    // time the usual way draws it with a PacketNote: its textures' names and
    // the bindless slots they were published under (with the publication's
    // generation, which every clear advances), and its uniform values as a
    // patch of the material row. Later the packet's entry only checks the
    // record against the batch (names, publication, values, buffers, pool
    // placement) and writes its row and indirect command: one lock, one
    // look at the run and the pass state, and one gather of the pass's words
    // per packet, none per entry. A packet sets no uniform: the program's
    // values stay what the usual way's last batch set (and the viewer's
    // uniform cache with them).
    // ---- research/180 stage 4: the packet entry (tools/selftest sim_packet extracts it) ----
    static constexpr U32 MAX_PACKET_UNITS = 3;      // the normal, specular and diffuse maps
    static constexpr U32 MAX_PACKET_UNIFORMS = 4;   // the specular color, environment intensity, alpha cutoff, emissive
    static constexpr U32 PACKET_VALUES = 8;         // their floats (4 + 1 + 1 + 1, one spare)
    // One batch, as LLRenderPass::pushDirectBatch would describe it.
    struct PacketEntry
    {
        U32* owner = nullptr;           // the batch's draw template slot (LLDrawInfo::mRHIDrawTemplate)
        U32 vertex_buffer = 0;          // GL names and sizes, as DirectDraw has them
        U32 index_buffer = 0;
        U32 index_type = 0;
        U32 buffer_vertices = 0;
        U32 buffer_indices = 0;
        U32 first_index = 0;
        U32 index_count = 0;
        U32 names[MAX_PACKET_UNITS] = {};   // the GL texture name per Packet::units
        F32 values[PACKET_VALUES] = {};     // the uniform values, Packet::uniforms' floats in order
        // P1d: the entry's own pass matrices (Packet::matrices' fields; an
        // object's short stretch inside a packet of another model matrix),
        // nullptr: the packet's. Kept by the caller until the packet is drawn.
        const DirectDraw* matrices = nullptr;
    };
    // A uniform the loop sets per batch: its native slot (LLGLSLShader's) and
    // how many floats of PacketEntry::values it takes from `first`.
    struct PacketUniform
    {
        S32 slot = -1;
        U32 components = 0;
        U32 first = 0;
    };
    enum PacketStop : U32
    {
        PACKET_DONE = 0,    // every entry drawn
        PACKET_REFUSED,     // nothing of this loop can go by packet (the program, the pass, a capture): draw the rest the usual way
        PACKET_RUN,         // the open run cannot take the entry (none open, another program or state, its chunk, full)
        PACKET_OWNER,       // no template for the batch, or another batch's, or other buffers
        PACKET_PROGRAM,     // the template is of another program or vertex format
        PACKET_PART,        // no record beside the template yet
        PACKET_NAME,        // a texture name differs from the record's
        PACKET_TEXTURE,     // a texture's publication differs from the record's (cleared, moved, not published)
        PACKET_PATCH,       // the uniform values differ from the record's
        PACKET_POOL,        // the pool's placement of the buffers is gone
        PACKET_ROW,         // the frame's table is full
        PACKET_STOP_COUNT
    };
    // ---- end of the packet entry ----
    struct Packet
    {
        const PacketEntry* entries = nullptr;
        U32 count = 0;
        U32 units[MAX_PACKET_UNITS] = {};       // texture units, per PacketEntry::names
        U32 unit_count = 0;
        PacketUniform uniforms[MAX_PACKET_UNIFORMS];
        U32 uniform_count = 0;
        // The pass's matrices with the entries' model matrix (modelview,
        // modelview_projection, normal, inverse_modelview, matrix_id; nothing
        // else is read): the entries' model matrix (their region's or
        // object's), made once per model matrix a frame; where it changes the
        // caller hands over a new packet, except for an entry with matrices
        // of its own (PacketEntry::matrices, P1d), which only its matrix row
        // word differs by (nothing else of the head depends on them).
        const DirectDraw* matrices = nullptr;
        U32 loop = 0;               // the loop it belongs to (verify: its head is compared within the loop only)
        bool verify = false;        // only resolve the head and keep it for verify (draws nothing)
        // Out: why it stopped at entries[returned count] (PACKET_DONE: all drawn).
        U32 stop = PACKET_DONE;
    };
    // Set by the Vulkan backend. Draws the
    // entries from the first, in order, and returns how many it drew; the
    // caller draws the one it stopped at the usual way (with a PacketNote)
    // and hands the rest over again. Main thread, the program bound.
    extern U32 (*sPacketDraw)(Packet& packet);
    // Set by the caller around the usual way's draw of an entry (the same
    // thread): the backend writes or updates the entry's record when the draw
    // joins a run (and, verify, first compares the record with what the usual
    // way resolved). Null otherwise.
    struct PacketNote
    {
        const Packet* packet = nullptr;
        const PacketEntry* entry = nullptr;
        bool verify = false;
    };
    extern thread_local const PacketNote* tPacketNote;
    // LLRHI_PACKETS: auto (unset, the default: verify first, then on once
    // enough batches of the world compared with no mismatch while the slot
    // publication takes, off at the first mismatch); 0 off; 1 on; verify
    // (draws the usual way, comparing every batch the packet would have drawn
    // with what the usual way resolved, a VKPerf count of the mismatches);
    // census (counts only, research/180 10.4 P1-0: how many batches a packet
    // could take). packets.toggle / packetsverify.toggle /
    // packetscensus.toggle flip them (a cue ends auto; packets.toggle from
    // auto is off). Off: the loop's one test per call.
    extern bool sPackets;
    extern bool sPacketsVerify;
    extern bool sPacketsCensus;
    extern bool sPacketsAuto;
    // P1c (research/180 10.10): packets also take the batches with a model
    // matrix of their object's (isActive, ANIMATED_CHILD: the render or world
    // matrix), a packet per stretch of one object's batches. On unless
    // LLRHI_PACKET_OBJECTS=0; packetobjects.toggle flips it.
    extern bool sPacketObjects;
    // P1d (research/180 10.11): an object's stretch shorter than a packet
    // pays for (fewer than 3 batches in a row) joins the packet around it
    // with matrices of its own (PacketEntry::matrices) instead of a cut and
    // the usual way. The order of the draws stays. On unless
    // LLRHI_PACKET_SHORT=0; packetshort.toggle flips it.
    extern bool sPacketShort;
    // P1-0: the material loop's static batches by what keeps them from a
    // packet (plus/packet counts, the backend's VKPerf "packets [...]" reads
    // and clears). Main thread.
    struct PacketCensus
    {
        U64 loops = 0;              // material loop calls counted
        U64 batches = 0;            // their batches
        U64 no_template = 0;        // no draw template yet (mRHIDrawTemplate 0)
        U64 avatar = 0;             // rigged (mAvatar)
        U64 texture_matrix = 0;     // texture animation (mTextureMatrix)
        U64 model_matrix = 0;       // a model matrix that is not a region's (isActive, ANIMATED_CHILD), not taken (P1c off)
        U64 region_matrix = 0;      // eligible with their region's matrix (static: registerFace's region_space)
        // P1c: the batches with an object's matrix (not a region's), taken or not, by why the
        // object has one (LLDrawInfo::mRHIMatrixKind, registerFace) and how it moves.
        U64 object_matrix = 0;      // eligible with their object's matrix (P1c on)
        U64 short_stretch = 0;      // an object's stretch shorter than a packet pays for: the usual way
        U64 short_taken = 0;        // P1d: such a stretch's batches taken with matrices of their own (census: would be)
        U64 objects = 0;            // batches with an object's matrix (all of the below are of these)
        U64 animated_child = 0;     // ANIMATED_CHILD (the world matrix)
        U64 active_root = 0;        // isActive, the root (its render matrix: its own)
        U64 active_child = 0;       // isActive, a child (its render matrix: the parent's)
        U64 attachment = 0;         // of an attachment
        U64 flexible = 0;           // flexible
        U64 animesh = 0;            // an animated object
        U64 omega = 0;              // with an angular velocity (llTargetOmega)
        U64 kind_unknown = 0;       // not made by registerFace (no kind)
        // Verify and census only (a table of the matrices' words by address):
        U64 moving = 0;             // the matrix changed in the last MOVING_FRAMES frames
        U64 still = 0;              // not changed for MOVING_FRAMES frames or more
        U64 returnable = 0;         // still, and nothing keeps it active (not attachment, flexible, animesh, omega)
        U64 matrix_cuts = 0;        // on: packets cut where the region's matrix changes (the head made again)
        U64 matrix_sets = 0;        // the pass's matrices made (once per model matrix, view, projection and the shader's matrix uniforms a frame)
        U64 matrix_sets_full = 0;   // of them, made again past the frame's sets (P1d: a short stretch's then goes the usual way)
        U64 unnamed = 0;            // a texture without a GL name yet (or its sampler options to set)
        U64 mapped = 0;             // the vertex buffer mapped
        U64 eligible = 0;           // none of the above: a packet may take them (f = eligible / batches)
        U64 value_sets = 0;         // distinct uniform value sets among the eligible, summed over the frames
        U64 loop_ticks = 0;         // __rdtsc ticks in the packet loop (on, verify)
        U64 loop_batches = 0;       // its batches
        // research/180 10.20: what else keeps a batch from a packet.
        U64 eye_attachment = 0;     // a rigid eye attachment while the skin is marked (plus/skinsss marks its draw)
    };
    extern PacketCensus sPacketCensus;

    // --- GPU probe list (research/46, stage G5) -----------------------------
    // Blocks: a static group's batches of one render type, registered together
    // in their draw order. A block's batches all draw with one program (the
    // pool's) and come back as runs of batches that share a pool chunk and a
    // vertex format (one indirect draw can only read one set of buffers).
    // A block dies when GL writes or deletes a buffer of its batches (the pool
    // drops the copy): blockLive turns false. Render thread; every call takes
    // the translation layer's lock itself.
    class ProbeList
    {
    public:
        static constexpr U32 MAX_UNITS = 8;     // texture units a batch names
        struct Batch
        {
            DirectDraw draw;                    // as LLRenderPass::pushDirectBatch makes it (buffers, range, attributes)
            U32 textures[MAX_UNITS] = {};       // GL names per unit (0: none, the white stand-in)
            U32 texture_count = 0;
            F32 lo[3] = {};                     // bounds of its vertices in the model's space
            F32 hi[3] = {};
        };
        struct Run
        {
            U32 first = 0;                      // batch index in the block
            U32 count = 0;
            U32 chunk = 0;
            U64 format = 0;
            U32 state = 0;                      // captured blocks: the batches' render state (0 for addBlock's)
        };
        // The model matrices a face draws its blocks with, as pushDirectBatch
        // would hand them over (modelview, MVP, normal, inverse modelview).
        struct Model
        {
            const F32* model = nullptr;         // the region's render matrix (column-major 4x4)
            DirectDraw matrices;                // only its matrix pointers
        };
        // One face's draw.
        struct Face
        {
            const U32* blocks = nullptr;        // block handles in draw order
            U32 block_count = 0;
            const Model* models = nullptr;      // every model the blocks use
            U32 model_count = 0;
            // The face's left, right, bottom and top agent planes and origin (research/44 P1).
            F32 sides[4][4] = {};
            F32 origin[3] = {};
            // Drawn with drawSegment (device generated commands) instead of drawRun.
            bool generated = false;
        };
        virtual ~ProbeList() = default;
        // The device, the shader and the tables are there (made on the first call).
        virtual bool ready() = 0;
        // A block of `count` batches drawn with `model` (never null). Returns its
        // handle and fills runs (at most `count`, run_count of them); 0 when it
        // could not be registered now (the pool's room or copy budget: try later).
        virtual U32 addBlock(const Batch* batches, U32 count, const F32* model, Run* runs, U32& run_count) = 0;
        // Why the last addBlock returned 0 (a static string).
        virtual const char* refusal() const = 0;
        virtual void removeBlock(U32 block) = 0;
        virtual bool blockLive(U32 block) = 0;
        // After the face's first batch went out as a draw run (it is the run's last
        // draw, its program bound): records the compute pass that writes every
        // block batch's indirect command (the first ones of runs included, unused)
        // and row. False: draw them all on the CPU.
        virtual bool prepare(const Face& face) = 0;
        // After a run's first batch went out as a draw run: the run's other
        // batches, `count` from batch `first` of the face's prepared order (all
        // blocks' batches in a row), one indirect draw. False: nothing recorded.
        virtual bool drawRun(U32 first, U32 count, U32 chunk, U64 format) = 0;

        // --- Research/50 step 4 (device generated commands) ---------------------
        // The device can draw a face's batches with device generated commands.
        virtual bool generatedReady() = 0;
        // After a segment's first batch went out as a draw run (Face::generated
        // was prepared): the segment's other batches (one vertex format and
        // render state, any pool chunks), `count` from batch `first`, one
        // vkCmdExecuteGeneratedCommandsEXT. False: nothing recorded.
        virtual bool drawSegment(U32 first, U32 count, U64 format) = 0;
        // Capture: between beginCapture and endCapture, direct draws are not
        // recorded; each is kept as what it would have pushed (program, pool
        // placement, the texture names its samplers read, its material row).
        // The viewer draws a group's batches of one render type the way their
        // pool does, one draw each (captured() counts them), then makes the
        // block from them. Only while nothing else is drawn.
        virtual void beginCapture() = 0;
        virtual U32 captured() = 0;
        virtual void endCapture() = 0;
        struct Bounds
        {
            F32 lo[3] = {};
            F32 hi[3] = {};
        };
        // A block of the `count` draws captured so far (in order; bounds per
        // draw, in the model's space), as addBlock. 0: not now.
        virtual U32 addCapturedBlock(const Bounds* bounds, U32 count, const F32* model, Run* runs, U32& run_count) = 0;
    };
    // Set by the Vulkan backend when it can draw such lists.
    extern ProbeList* sProbeList;
    // Direct draws wanted (LLRHI_DIRECT=0 turns them off).
    extern bool sDirect;

    // --- Batched point lights (research/47) ----------------------------------
    // LLPipeline::renderDeferredLighting draws the point lights' boxes as one
    // instanced direct draw that reads each light (center, size, color,
    // falloff, box fan) from the frame's table, instead of five uniforms and a
    // draw per light. Same boxes in the same order. Vulkan mode only; on
    // unless LLRHI_LIGHT_BATCH=0 (LLViewerShaderMgr then leaves the light
    // program unmarked and the backend does not rewrite it).
    extern bool sLightBatch;
    // Set by the Vulkan backend: whether the GL program was translated to read
    // its light from the table (it still reads its uniforms for draws without
    // a table_base). Null: no backend.
    extern bool (*sLightTableProgram)(U32 gl_program);
    // Set by the Vulkan backend: room for `vec4s` vec4s in the frame's table;
    // base: the first one's index (DirectDraw::table_base). Null when the
    // table is full this frame (it grows for the next) or there is none.
    extern F32* (*sTableRows)(U32 vec4s, U32& base);

    // --- Order-independent transparency (research/41) ------------------------
    // LLDrawPoolAlpha::renderPostDeferred draws the main view's above-water
    // alpha between sOITBegin and sOITResolve: every fragment of a blended
    // draw into the bound target becomes a node of its pixel's list (with
    // the draw's blend state) instead of being blended in draw order, and the
    // resolve sorts each pixel's nodes by depth and blends them back to front
    // over the target. Draws whose program has no OIT variant draw as before.
    // The viewer's RenderOITMode sets it (setOITMode); LLRHI_OIT=1 / 0
    // overrides that, and oit.toggle flips it, for tests.
    extern bool sOIT;
    // Hybrid OIT (RenderOITMode 1, or LLRHI_OIT_HYBRID=1; oithybrid.toggle
    // flips it, from the next session on): only the avatars' alpha is drawn
    // into the lists everywhere. The alpha drawn after sOITWorld becomes
    // nodes only at pixels with avatar nodes, and elsewhere blends into the
    // target as without OIT. Off by default: full OIT is the default
    // (RenderOITMode 2 in settings.xml and the beautiful preset; the light
    // and standard presets choose hybrid; research/55).
    extern bool sOITHybrid;
    // The user's mode (RenderOITMode): 0 off, 1 hybrid, 2 full OIT. Sets
    // sOIT and sOITHybrid, except one whose env var (LLRHI_OIT,
    // LLRHI_OIT_HYBRID) is set: that one stays as the env var and its cue
    // toggle make it. Main thread, outside a frame's draws (a session reads
    // both when it begins).
    void setOITMode(U32 mode);
    // LLRHI_OIT is set and keeps OIT off (0, empty; plus/safemode sets 0):
    // neither the settings nor a snapshot's full OIT turn it on (oit.toggle,
    // for tests, still can).
    bool oitForcedOff();
    // Programs linked while it is set keep their sources for an OIT variant
    // (the Vulkan backend's memory; one linked without them always draws as
    // without OIT). From LLRHI_OIT being set at startup; the viewer sets it
    // before the shaders load when the settings may use OIT, and reloads the
    // shaders when it sets it later.
    extern bool sOITSources;
    // Set by the Vulkan backend: whether the device can do OIT (fragment
    // stores, r32ui image atomics, and its setup has not failed). Null in GL.
    extern bool (*sOITSupported)();
    // Set by the Vulkan backend, for snapshots (research/41): called after a
    // snapshot's render, before its pixels are read. Waits for the GPU and
    // reads the frame's session counts; true when one ran out of nodes and
    // the buffer grew (up to LLRHI_OIT_MAX_MB), or the overflow image was
    // just made at the largest size: render it again for an exact image.
    // False when it fit, there was no session, or it cannot get better.
    extern bool (*sOITSnapshotCheck)();
    // Set by the Vulkan backend. Opens a session on the bound draw
    // framebuffer; false: none (draw the alpha the usual way). depth_a,
    // depth_b: the projection's [2][2] and [3][2] (column-major elements 10
    // and 14), for the distance from the eye of a depth. hybrid: the session
    // is a hybrid one (the caller calls sOITWorld between the halves).
    extern bool (*sOITBegin)(F32 depth_a, F32 depth_b, bool hybrid);
    // Hybrid sessions: every avatar alpha draw has been made; the draws
    // from here on are the rest of the alpha.
    extern void (*sOITWorld)();
    // Inside a session: whether draws of the GL program make nodes (it has
    // an OIT variant; made now if not yet). Draws whose program does not
    // draw into the target as before.
    extern bool (*sOITVariant)(U32 gl_program);
    // Composites the session's nodes into its target and closes it.
    extern void (*sOITResolve)();
    // The priority of the draws the alpha pool makes now (inside a session):
    // nodes whose distances lie within the backend's tolerance (one surface,
    // as clothing layers) are blended by it, the higher on top, instead of
    // by depth. Frame-stable per batch (LLDrawPoolAlpha's oit_priority).
    extern U32 sOITPriority;

    // --- Native buffer data (research/39 N4) --------------------------------
    // Set by the Vulkan backend: a GL buffer's contents by name, without binding
    // it or going through glBufferData / glBufferSubData. data null: storage only.
    extern void (*sBufferData)(U32 name, size_t size, const void* data);
    extern void (*sBufferSubData)(U32 name, size_t offset, size_t size, const void* data);

    // --- Native shader API (research/39 N4) ----------------------------------
    // LLGLSLShader binds its program and sets its uniforms through these instead
    // of glUseProgram / glUniform*, which the translation layer would otherwise
    // catch one by one (function pointer, lock, current program, location map).
    // All null unless the Vulkan backend translates programs and GL needs no
    // program state of its own; callers use GL whenever a pointer, handle or
    // slot is missing, or a setter returns false.
    struct ShaderProgram;   // opaque: the backend's translated program
    // The backend's program for a linked GL program; null when there is none
    // (not translated, failed to build). Valid until the program is relinked
    // or deleted: ask again after linking, drop it before glDeleteProgram.
    extern ShaderProgram* (*sShaderProgram)(U32 gl_program);
    // A dense slot for a uniform location of the program (as glGetUniformLocation
    // returned it, after that call); -1 when the backend does not know it.
    extern S32 (*sUniformSlot)(ShaderProgram* program, S32 gl_location);
    // glUseProgram(gl_program); program may be null (GL program 0, or one the
    // backend does not translate).
    extern void (*sUseProgram)(ShaderProgram* program, U32 gl_program);
    // glUniform{1,2,3,4}{i,f,ui}v on the given program (not necessarily the
    // bound one), 4-byte components. Sampler uniforms take texture units.
    // False: the slot is no longer valid (relinked); use GL.
    extern bool (*sSetUniform)(ShaderProgram* program, S32 slot, U32 count, U32 components, const void* data);
    // glUniformMatrix{2,3,4,3x4}fv: columns x rows floats per matrix.
    extern bool (*sSetUniformMatrix)(ShaderProgram* program, S32 slot, U32 count, U32 columns, U32 rows, bool transpose, const F32* data);
    // Native shader API wanted (LLRHI_NATIVE_SHADER=0 turns it off).
    extern bool sNativeShader;

    // --- Prebuilt skinning palettes (research/56, item 2) --------------------
    // LLVOAvatar builds each avatar x skin palette once a frame, when its first
    // pass asks for it (upstream does so too); the backend copies it into the
    // frame's palette table right then (sPlacePalette), once. From then on every
    // pass names that row instead of uploading the joints again:
    // LLRenderPass::uploadMatrixPalette hands the bound program the row
    // (sUsePaletteRow) instead of glUniformMatrix3x4fv, and rigged native
    // shadow packets carry it (ShadowPacket::palette_row) instead of a 5 KB copy
    // into the shadow pass's ring. No pass compares, hashes or copies the joints
    // again. The same values in the same table layout: the same pixels. Vulkan
    // mode with the native shader API only. On unless LLRHI_PALETTE_PREBUILD=0;
    // paletteprebuild.toggle flips it.
    extern bool sPalettePrebuild;
    // Set by the Vulkan backend. Keeps row when it already is this frame's;
    // else copies `vec4s` vec4s of data (mat3x4 columns, as glUniformMatrix3x4fv
    // takes them untransposed) into the frame's table with room for the largest
    // palette a skinning shader reads, and fills row. False: no row (no table,
    // the frame's table is full, too long a palette): upload the joints.
    extern bool (*sPlacePalette)(const F32* data, U32 vec4s, PaletteRow& row);
    // Set by the Vulkan backend with the native shader API: the program's draws
    // read their palette from row, as if its matrixPalette uniform had been set
    // to the row's joints, until that uniform is set again. False: it cannot
    // (not this frame's row, no mat3x4 palette read from the table, more room
    // than the row has): set the uniform.
    extern bool (*sUsePaletteRow)(ShaderProgram* program, const PaletteRow& row);

    // --- Avatar crowd counters (research/56) ---------------------------------
    // LLRHI_AVATAR_STATS=1 (or avatarstats.toggle): an "RHIAvatar" log line
    // every 5 s (LLVOAvatar::updateImpostors, once a frame) with these per
    // frame and the backend's part (sAvatarBackendReport). Off: no site counts
    // or reads a clock (each tests the flag first). Main thread only.
    extern bool sAvatarStats;
    struct AvatarCounters
    {
        // Where a rigged draw went (LLViewerCamera::sCurCameraID and the pass flags).
        enum Pass
        {
            PASS_WORLD, PASS_SUN0, PASS_SUN1, PASS_SUN2, PASS_SUN3, PASS_SPOT0, PASS_SPOT1, PASS_WATER,
            PASS_PROBE, PASS_PROBE_SHADOW, PASS_MIRROR, PASS_IMPOSTOR, PASS_COUNT
        };
        U64 rigged_draws[PASS_COUNT] = {};
        U64 skinned_vertices[PASS_COUNT] = {};  // the draws' vertex ranges (mEnd - mStart + 1)
        U32 frames = 0;
        U32 palette_builds = 0;                 // LLVOAvatar::updateSkinInfoMatrixPalette builds
        U64 palette_build_ns = 0;
        U32 palette_uploads = 0;                // joints set as the bound program's uniform
        U32 palette_row_uses = 0;               // rows named instead (sUsePaletteRow)
        U32 palette_places = 0;                 // rows copied into the table (sPlacePalette)
        U64 palette_place_ns = 0;
        U32 shadow_packets = 0;                 // rigged native shadow packets
        U32 shadow_packet_rows = 0;             // of which carry a row
        U32 impostor_updates = 0;               // LLPipeline::generateImpostor calls
        U32 updates_full = 0, updates_impostor = 0;             // LLVOAvatar::idleUpdate calls
        U64 update_ns_full = 0, update_ns_impostor = 0;
        U64 character_ns_full = 0, character_ns_impostor = 0;   // of which updateCharacter
        // Research/63 (sAvatarThreads): the keyframe samples taken ahead before
        // the avatar updates, outside idleUpdate (its main thread wall time),
        // the motions and joints sampled, and the motions that used them.
        U64 motion_prepass_ns = 0;
        U32 motion_prepass_motions = 0;
        U32 motion_prepass_joints = 0;
        U32 motion_prepass_hits = 0;
        U32 motion_prepass_misses = 0;
        U32 motion_prepass_mismatches = 0;  // sAvatarThreadsValidate: joints whose samples differed
    };
    extern AvatarCounters sAvatarCounters;

    // --- Avatar update (research/63) -----------------------------------------
    // Keyframe values worked out ahead on the cull threads: before the serial
    // avatar updates (LLViewerObjectList::update), the threads sample the
    // curves of each visible avatar's active keyframe motions at the time this
    // frame's update is expected to use; LLKeyframeMotion::applyKeyframes takes
    // them only for exactly that time, list and usage, else samples itself.
    // The same function makes the values either way: the same joint values in
    // the same order. On unless LLRHI_AVATAR_THREADS=0; avatarthreads.toggle.
    extern bool sAvatarThreads;
    // LLRHI_AVATAR_THREADS=2: also check. Each motion that would use samples
    // samples again on the main thread, uses its own values, and counts the
    // joints whose samples differ in any bit (the RHIAvatar line's "check
    // mismatches", with LLRHI_AVATAR_STATS). Costs the sampling twice.
    extern bool sAvatarThreadsValidate;
    // LLPoseBlender::addMotion finds a joint's blender through the joint
    // state's cache and tells whether it is already active by a flag, instead
    // of two map finds and a search of the active list per joint state. The
    // same blenders in the same order. On unless LLRHI_AVATAR_BLEND_LOOKUP=0;
    // avatarblend.toggle.
    extern bool sAvatarBlendLookup;
    // Set by the Vulkan backend: its part of the line per frame over `frames`
    // (palette uniform writes and table rows with their µs, the native shadow
    // palette copies, rigged draws that went into draw runs and templates);
    // resets its counters. Null: none.
    extern std::string (*sAvatarBackendReport)(U32 frames);

    // --- Native texture binding (research/39 N4) ----------------------------
    // In Vulkan mode LLTexUnit binds textures and sets their sampling
    // parameters through these instead of glActiveTexture / glBindTexture /
    // glTexParameter*. Each is exactly what the GL calls it replaces would do
    // to the backend's state (per thread, like a GL context), without a GL
    // entry point per call. Set by the Vulkan backend; null otherwise (call GL).
    // glActiveTexture(GL_TEXTURE0 + unit).
    extern void (*sActiveTexture)(U32 unit);
    // glActiveTexture(GL_TEXTURE0 + unit) then glBindTexture(gl_target, gl_name).
    extern void (*sBindTexture)(U32 unit, U32 gl_target, U32 gl_name);
    // glTexParameteri/f(gl_target, pname, value) for each, on the active unit's
    // texture, as GL does. Textures change version only when a value changed.
    struct TexParameter
    {
        U32 pname = 0;  // GL_TEXTURE_WRAP_S, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAX_ANISOTROPY, ...
        F32 value = 0.f;
    };
    extern void (*sTexParameters)(U32 gl_target, const TexParameter* params, U32 count);

    // --- Native texture upload (research/39 N4) -----------------------------
    // In Vulkan mode LLImageGL creates and fills textures through these
    // instead of glTexImage2D / glCompressedTexImage2D / glTexSubImage2D /
    // glGenerateMipmap. The texture is named (no bind or active unit needed);
    // gl_target is what the GL call would take (GL_TEXTURE_2D or a cube map
    // face). Pixels are read with the current GL unpack state (row length,
    // alignment). Each returns false and does nothing when GL has to make the
    // call (an image without pixels, a pixel unpack buffer bound, a texture of
    // another kind): the caller then calls GL on the bound texture. Set by the
    // Vulkan backend while GL gets no texture data; null otherwise (call GL).
    extern bool (*sTexImage2D)(U32 gl_name, U32 gl_target, S32 level, U32 internal_format, S32 width, S32 height,
                               U32 format, U32 type, const void* pixels);
    extern bool (*sCompressedTexImage2D)(U32 gl_name, U32 gl_target, S32 level, U32 internal_format, S32 width, S32 height,
                                         S32 size, const void* data);
    extern bool (*sTexSubImage2D)(U32 gl_name, U32 gl_target, S32 level, S32 x, S32 y, S32 width, S32 height,
                                  U32 format, U32 type, const void* pixels);
    extern bool (*sGenerateMipmap)(U32 gl_name, U32 gl_target);
    // glCopyTexSubImage3D(gl_target, level, xoffset, yoffset, zoffset, x, y,
    // width, height) into the texture gl_name, from the bound read framebuffer
    // (reflection probe cube map arrays). Same rules as above: false and
    // nothing done when GL has to make the call on the bound texture.
    extern bool (*sCopyTexSubImage3D)(U32 gl_name, U32 gl_target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset,
                                      S32 x, S32 y, S32 width, S32 height);

    // --- Native occlusion queries and uniform buffers (research/42) ----------
    // Set by the Vulkan backend; null otherwise (call GL). Each does what its
    // GL wrapper does (the same tracking, lock and GL forwarding) without the
    // GL entry point. Arguments are GL enums and names. Callers use llrhigl.h.
    extern void (*sBeginQuery)(U32 target, U32 name);                  // glBeginQuery
    extern void (*sEndQuery)(U32 target);                              // glEndQuery
    extern void (*sGetQueryObjectuiv)(U32 name, U32 pname, U32* params);  // glGetQueryObjectuiv
    extern void (*sBindBufferBase)(U32 target, U32 index, U32 buffer); // glBindBufferBase
    // glTexImage2D without pixels for a texture GL never needs storage for (not
    // a framebuffer attachment; LLImageGL::scaleDown): only the backend's image
    // is (re)created, GL gets nothing. False: call GL.
    extern bool (*sTexStorage2D)(U32 gl_name, U32 gl_target, S32 level, U32 internal_format, S32 width, S32 height,
                                 U32 format, U32 type);
    // glCopyTexSubImage2D from the current read framebuffer into the named
    // texture (not the bound one). False: call GL on the bound texture.
    extern bool (*sCopyTexSubImage2D)(U32 gl_name, U32 gl_target, S32 level, S32 xoffset, S32 yoffset,
                                      S32 x, S32 y, S32 width, S32 height);
    // texture-leak (2026-10-27): LLImageGL::scaleDown's swap in one call. The
    // texture gl_name (2D) gets a new level 0 of width x height, copied from
    // the current read framebuffer's (0, 0) corner, only when that copy can be
    // made; false and nothing changed otherwise (the old image stays: never an
    // image without texels shown in its place). Null: the two calls above.
    extern bool (*sTexScaleDown)(U32 gl_name, U32 gl_target, U32 internal_format, S32 width, S32 height);
    // glPixelStorei(GL_UNPACK_ROW_LENGTH / GL_UNPACK_ALIGNMENT, value) for the
    // native uploads above: same per-thread state as GL's, without the GL entry
    // point (GL is brought up to date before a GL call that reads it). Other
    // parameters go to GL.
    extern void (*sPixelStorei)(U32 pname, S32 value);

    // --- Native state and render targets (research/39 N4) -------------------
    // Set by the Vulkan backend's install, always (research/207 T4: the switch
    // LLRHI_NATIVE_STATE=0 and the GL fallbacks are gone; LLVertexBuffer::initClass
    // stops without them). The viewer's state classes (LLGLState, LLGLDepthTest,
    // LLRender's blend, color mask, line width and cull face, LLRenderTarget)
    // call these: the backend tracks the state directly. Arguments are GL enums
    // and values. The callers' state caches (the copy) are the only writers of
    // the backend's state (research/207 4.1; the copy watch of LLRHI_PASS_STATE=
    // verify checks it). Code outside those classes calls them through llrhigl.h
    // (LLRHI::scissor, LLRHI::viewport, ...).
    extern void (*sSetCap)(U32 cap, bool on);                                       // glEnable / glDisable
    extern void (*sBlendFunc)(U32 src_rgb, U32 dst_rgb, U32 src_a, U32 dst_a);      // glBlendFunc(Separate)
    extern void (*sColorMask)(bool r, bool g, bool b, bool a);
    extern void (*sDepthFunc)(U32 func);
    extern void (*sDepthMask)(bool write);
    extern void (*sLineWidth)(F32 width);
    extern void (*sCullFace)(U32 mode);                                             // glCullFace
    extern void (*sViewport)(S32 x, S32 y, S32 width, S32 height);
    extern void (*sScissor)(S32 x, S32 y, S32 width, S32 height);
    extern void (*sPolygonOffset)(F32 factor, F32 units);
    extern void (*sPolygonMode)(U32 face, U32 mode);                                // glPolygonMode
    extern void (*sClearColor)(F32 r, F32 g, F32 b, F32 a);
    extern void (*sBindFramebuffer)(U32 target, U32 framebuffer);
    extern void (*sDrawBuffers)(S32 count, const U32* buffers);
    extern void (*sDrawBuffer)(U32 buffer);
    extern void (*sReadBuffer)(U32 buffer);
    extern void (*sClear)(U32 mask);

    // GL draws do not reach the driver (Vulkan presents alone). Set with
    // sVulkanGL; false only in the GPU benchmark process (plain GL).
    extern bool sGLDrawsDisabled;
    // Set by the Vulkan backend's install (from onGLFunctionsLoaded, so before
    // any shader loads): GL calls are translated to Vulkan. LLViewerShaderMgr
    // marks programs for the backend's rewrites (LLRHI_DRAW_ROWS,
    // LLRHI_LIGHT_TABLE) only then. There is no GL context (research/117
    // G6): the translation answers every GL call itself.
    extern bool sVulkanGL;
    // Why Vulkan could not start (research/90 G1): the GPU or driver lacks
    // what the renderer needs, or it has it but setting up the frame or the
    // window's swapchain failed (a driver problem, not a GPU to replace).
    // fix-submitfail C: or, while running, the GPU stopped (device lost: a
    // driver reset or update, a GPU fault) or the window's surface went away;
    // nothing would render or show again.
    // MissingFeatures (research/124 V1): the GPU has Vulkan 1.3 but lacks a
    // feature the renderer requires (such as VK_EXT_depth_clip_control); the
    // reasons then have "<GPU>: missing <feature> (<what has it>), ..." for
    // each such GPU, beside any other GPU's line.
    // SubmitFailed (fix-round6 1): while running, the GPU did not take a
    // submission (out of GPU or system memory, or another error, the device
    // not lost); what it carried is lost, so the viewer cannot go on.
    enum class VulkanUnavailable { Unsupported, SetupFailed, DeviceLost, SurfaceLost, MissingFeatures, SubmitFailed };
    // Vulkan could not start on this PC, or stopped (DeviceLost, SurfaceLost, SubmitFailed):
    // called with why and the reasons, one per line (for Unsupported and
    // MissingFeatures, LLVKDevice's report); tells the user and ends the viewer, never returns
    // (there is no OpenGL renderer to go back to, research/85). Set by the
    // viewer (plus/vkcheck) before the window is created. Called on the main thread.
    extern void (*sVulkanUnavailableExit)(VulkanUnavailable why, const std::string& reasons);
    // research/117 (GL stage 1, G6): the viewer makes no GL pixel format or
    // context and loads no GL driver; every GL function comes from the Vulkan
    // translation's table (LLVKGL::getProcAddress). So the GPU benchmark
    // LLFeatureManager::loadGPUClass takes the GPU class from (gpu_benchmark,
    // timed with GL queries in a child process) is made on the Vulkan device
    // in this process instead: the same draws, timed with timestamps. GB/s as
    // gpu_benchmark reports it, -1 when it could not run. Main thread, after
    // the window is made. Set by the Vulkan backend.
    extern F32 (*sGPUBenchmark)();
    // Detached UI windows (research/87, plus/detach): another top-level window
    // of the viewer (an HWND) shows gl_texture, width x height pixels stored
    // GL-style, with the frame being recorded. The backend copies the texture
    // at once and presents the copy on that window with the frame, without
    // vsync; the window gets its own swapchain at the first call. Main thread,
    // while a frame is recorded, with a framebuffer bound that does not hold
    // gl_texture. False: not shown this frame. Set by the Vulkan backend.
    extern bool (*sPresentWindow)(void* window, U32 gl_texture, U32 width, U32 height);
    // The window is about to be destroyed: its swapchain and copy go first
    // (waits for the GPU). Main thread. Set by the Vulkan backend.
    extern void (*sForgetWindow)(void* window);
    // Diagnostics (research/105): the bound draw framebuffer as the backend
    // resolves it and the state a clear or draw into it would use, in words.
    // Main thread. Set by the Vulkan backend.
    extern std::string (*sDescribeDrawTarget)();
    // While set, GL draws are not translated to Vulkan (the native path
    // already drew them).
    extern bool sSuppressTranslation;
    struct SuppressTranslation
    {
        explicit SuppressTranslation(bool on = true) : mOn(on) { if (mOn) sSuppressTranslation = true; }
        ~SuppressTranslation() { if (mOn) sSuppressTranslation = false; }
        bool mOn;
    };

    // research/180 9 3: a static draw pool's batch loop holds the Vulkan
    // backend's lock (LLVKTracker::lock(), llvktracker.h) from start to end,
    // so the takes of its uniform, texture and draw calls inside are nested
    // ones (a count in the holder's thread, no interlocked operation): some
    // 20 to 30 thousand takes a frame in a heavy place. Taken only on the
    // main thread. The texture threads that need the lock meanwhile wait for the loop's
    // end (a few ms at most); the main thread waits on nothing of theirs in
    // these loops. Off unless LLRHI_TRACKER_HOLD=1 (trackerhold.toggle, read
    // at each loop's start). A TrackerHold line every 10 s with the periodic
    // logs: loops, holds, held ms (longest), and the takes of any thread that
    // had to wait for the lock (with it on or off: compare).
    extern bool sTrackerHold;
    struct TrackerHold
    {
        explicit TrackerHold(bool on = true);
        ~TrackerHold();
        TrackerHold(const TrackerHold&) = delete;
        TrackerHold& operator=(const TrackerHold&) = delete;
        bool mHeld;
    };

    // --- GPU pass timing (research/53) --------------------------------------
    // LLRHI_GPU_TIMING=1 (or gputiming.toggle): the Vulkan backend writes a GPU
    // timestamp at each gpuMark and the 5 s VKPerf report gets a line
    // "GPU ms: shadow 3.10, gbuffer 2.00, ..., total 15.20": the average GPU
    // time per frame of each name. A mark starts a segment that lasts until the
    // next mark, so each name gets the GPU work recorded after it; marking the
    // name already open costs nothing. While a GPUScope is open its name goes
    // in front ("probe shadow" for the shadow maps of a probe face). Names and
    // scopes are kept by pointer: string literals only. Off by default: a mark
    // is then one flag test.
    extern bool sGPUTiming;
    // --- Periodic diagnostics lines (fix-logcache) --------------------------
    // The lines written every few seconds whatever happens: the VKPerf report
    // (every 5 s, 4-6 KB), the bindless table, RHIShadow, "VKPerf Skin:", the
    // J2C decode totals, the ocean's shore field. On for development starts:
    // LLRHI_VK_PERF or LLRHI_CUE_DIR set (tools/launch.ps1 and daily.ps1 always
    // set LLRHI_CUE_DIR), or LLRHI_PERIODIC_LOGS=1; LLRHI_PERIODIC_LOGS=0 turns
    // them off there too; periodiclogs.toggle flips it (not the J2C totals:
    // llimagej2coj reads the variables itself). Off in a plain start (the
    // downloaded viewer): they made SecondLife.log grow by about 10 MB an
    // hour. Off, the counters still start over, the VKPerf reports' once a
    // minute instead of every 5 s (nothing else reads them); a line that
    // reports trouble (the bindless table full) still comes, at most once a
    // minute.
    extern bool sPeriodicLogs;
    // research/276 stage 1 (plus/fastexit, PlusExitTimeLog): set on the main thread
    // when an exit is measured (false otherwise, and again when the quit is
    // cancelled); LLRHIVKMirror::onGLShutdown then writes its steps as ExitTime
    // lines. The clock's start, in std::chrono::steady_clock nanoseconds since its
    // epoch, so a line carries the time since the quit began. Main thread only.
    extern bool sExitTimeLog;
    extern U64 sExitTimeStartNs;
    // research/285 (compact-hysteresis): the viewer is quitting (set by
    // plus/fastexit when a quit begins, measured or not: requestQuit,
    // forceQuit, cleanup; cleared when the quit is cancelled). The backend's
    // VRAM compactions stop: nothing is given back or copied again while the
    // viewer waits to close. Main thread writes, the backend reads.
    extern std::atomic<bool> sQuitting;
    extern const char* sGPUScope;
    extern void (*sGPUMark)(const char* scope, const char* name);   // set by the Vulkan backend
    // --- Frame cost (research/83) -------------------------------------------
    // LLRHI_FRAME_COST=1: every 5 s a "FrameCost" line: the main thread's
    // time per frame split at the gpuMark names (plus the cpuMark ones below),
    // the steps of the Vulkan frame end and start (waits included), mid-frame
    // GPU syncs, and with GPU timing (turned on with it) how long the GPU idles
    // between frames. =2: the same without GPU timestamps, and the main
    // thread's segments cut finer at the cpuDetailMark names (idle's steps,
    // the sun shadows' steps and what display() does between them and the
    // G-buffer, research/113). Off by default: a mark is then one flag test,
    // as before.
    extern bool sFrameCost;
    extern bool sFrameCostDetail;   // LLRHI_FRAME_COST=2
    // research/208 S0: the render thread census (LLRHI_RENDERTHREAD_CENSUS=1,
    // cue rtcensus.toggle; llrhiframe.h) cuts the main thread's frame at the
    // same marks, the detail ones included, without GPU timestamps.
    extern bool sRenderThreadCensus;
    // A main-thread segment that writes no GPU timestamp (main loop steps).
    extern void (*sCPUMark)(const char* name);  // set by the Vulkan backend
    inline void cpuMark(const char* name)
    {
        if ((sFrameCost || sRenderThreadCensus) && sCPUMark)
        {
            sCPUMark(name);
        }
    }
    // A finer main-thread segment, only with LLRHI_FRAME_COST=2.
    inline void cpuDetailMark(const char* name)
    {
        if (sFrameCostDetail || sRenderThreadCensus)
        {
            cpuMark(name);
        }
    }
    // Immediate mode (gGL.begin/vertex/end: the UI, text, debug shapes) while
    // sFrameCost is on, main thread only; the FrameCost line gives them per
    // frame (research/95).
    struct ImmediateCounts
    {
        U32 flushes = 0;                // LLRender::flush calls that drew
        U32 texture_flushes = 0;        // flushes ended by binding another texture
        U32 same_texture_flushes = 0;   // flushes ended by binding the texture already bound
        U32 cache_misses = 0;           // vertex buffers made for them (LLRender's VB cache)
    };
    extern ImmediateCounts sImmediate;
    inline void gpuMark(const char* name)
    {
        if ((sGPUTiming || sFrameCost || sRenderThreadCensus) && sGPUMark)
        {
            sGPUMark(sGPUScope, name);
        }
    }
    // Opens a scope and starts its "other" segment (work in it no mark names);
    // closing it goes back to the outer scope's "other" segment.
    struct GPUScope
    {
        explicit GPUScope(const char* scope) : mPrevious(sGPUScope) { sGPUScope = scope; gpuMark("other"); }
        ~GPUScope() { sGPUScope = mPrevious; gpuMark("other"); }
        GPUScope(const GPUScope&) = delete;
        GPUScope& operator=(const GPUScope&) = delete;
        const char* mPrevious;
    };
    // While GPU timing is on, the alpha pool splits its segment by kind of
    // draw ("alpha rigged", "alpha particles", "alpha glow", "prewater ...",
    // research/54). LLRHI_GPU_TIMING_ALPHA=0 (or gputimingalpha.toggle) keeps
    // the one "alpha" segment: each switch between kinds is a timestamp, which
    // waits for the work before it, so compare the totals both ways.
    extern bool sGPUTimingAlpha;

    // --- Frame pacing (research/94) -----------------------------------------
    // With VSync on (FIFO), the frame loop starts the next frame only once the
    // present before the last one is on screen (VK_KHR_present_wait), instead
    // of running ahead until every swapchain image is queued and then stalling
    // for several refreshes. Frames come at the display's even rhythm and
    // input is at most two refreshes old. VSync off and GPUs without the
    // extension are unaffected. On unless LLRHI_FRAME_PACING=0 (or
    // framepacing.toggle).
    extern bool sFramePacing;
    // While the GPU sets the pace (still on the frame before last when the
    // CPU is done) and the display has caught up, the wait is skipped: it
    // would only leave the GPU idle (research/103 F1). On unless
    // LLRHI_FRAME_PACING_GPU_BOUND=0 (or pacinggpu.toggle): always wait.
    extern bool sFramePacingGPUBound;

    // --- The main window's present without VSync (research/179 6.6 4) ------
    // While the CPU sets the pace, vkQueuePresentKHR took about 1.2 ms of the
    // frame's thread (0.4 ms while the GPU does). LLRHI_PRESENT_THREAD (on
    // unless 0, since 2026-10-26, research/179 9; presentthread.toggle flips
    // it): the present goes to the present thread the
    // detached windows use, and the next frame waits for it only before its
    // own acquire (FrameCost: "main present thread"). Only while the swapchain
    // does not wait for the display (VSync off): with VSync, frame pacing reads
    // the presents right away.
    extern bool sPresentThread;
    // LLRHI_PRESENT_MODE=mailbox (or presentmailbox.toggle): without VSync,
    // MAILBOX before IMMEDIATE (no tearing, the newest frame shows at the
    // vertical blank, and presents still never wait for the display). The
    // swapchain is made again at the switch. Off (IMMEDIATE first) unless set.
    // LLRHI_PRESENT_IMAGES=N: the swapchain's image count (default: the
    // surface's minimum + 1), read at startup only.
    extern bool sPresentMailbox;

    // --- Alpha census (research/54) -----------------------------------------
    // LLRHI_ALPHA_CENSUS=1 (or alphacensus.toggle): LLDrawPoolAlpha names each
    // main-view alpha draw (sCensusTag, 0: none) and logs every 10 s the draws
    // that cost the most. While a tag is set the Vulkan backend wraps each draw
    // it records in a pipeline statistics query (fragment shader invocations,
    // primitives), which needs the device feature: the device enables it only
    // when LLRHI_ALPHA_CENSUS is set at startup (the toggle alone gets the
    // CPU-side estimate only). Tagged draws never join draw runs.
    extern bool sAlphaCensus;
    extern U32 sCensusTag;
    struct CensusCount
    {
        U32 tag;
        U32 queries;            // draws counted
        U64 primitives;         // input assembly primitives
        U64 clipped;            // primitives out of clipping (sent to the rasterizer)
        U64 fragments;          // fragment shader invocations
    };
    // Set by the Vulkan backend: adds what was read back since the last call
    // (summed per tag, in no order) to counts; frames: the frames read that
    // counted a draw; dropped: draws not counted (the frame's queries ran out).
    // False: no counters (no backend or no pipeline statistics).
    extern bool (*sCensusTake)(std::vector<CensusCount>& counts, U32& frames, U32& dropped);

    // --- Zero coverage cull (research/61) -----------------------------------
    // Every target the viewer draws the world into has one sample per pixel at
    // its centre (no MSAA: anti-aliasing is FXAA/SMAA after the fact), no pass
    // jitters its projection, and triangles are filled (no conservative
    // rasterisation, no wide lines or points). A draw whose projected bounds
    // hold no pixel centre of the pass's viewport therefore writes no fragment.
    // The main camera's cull and the sun shadow cascades' culls leave such
    // static volume groups out (LLPipeline::markNotCulled,
    // LLSpatialPartition::cullShadowInto); they are still marked visible, so
    // everything else about them goes on as before. Same image.
    // On unless LLRHI_ZERO_COVERAGE_CULL=0; coverage.toggle flips it.
    extern bool sZeroCoverageCull;
    // LLRHI_TINY_STATS=1 (or tinystats.toggle): every 5 s a log line per pass
    // (main, sun0..3) with the static volume groups the cull found and their
    // batches, and how many of them have projected bounds holding no pixel
    // centre, at most 1x1 and at most 2x2 centres (each including the smaller).
    extern bool sTinyStats;

    // --- Recording splices (research/66 step 1) ----------------------------
    // A worker thread may fill a private packet stream that the recording
    // worker replays where the main thread spliced it in (LLVKRecorder::
    // acquire/splice/Fill): the same commands in the same order. Nothing
    // splices unless a user asks (sSpliceTest; research/66 step 2). Off: every
    // user records inline as before. On unless LLRHI_RECORD_SPLICE=0;
    // recordsplice.toggle flips it.
    extern bool sRecordSplice;
    // research/208 S2: the render thread (LLRHI_RENDER_THREAD, llrhiframe.h)
    // is used while this is true (default); renderthread.toggle flips it, so
    // one session can compare the thread with the main thread's recording.
    // Nothing changes while LLRHI_RENDER_THREAD is off.
    extern bool sRenderThread;
    // Self-test of the splice on a real workload: the draws of the first
    // sSpliceTestPasses native shadow passes of each frame are recorded into a
    // stream on the main thread and spliced in where they would have gone
    // (same image; the VKPerf line counts the spliced packets). Off unless
    // LLRHI_SPLICE_TEST=N (N passes a frame, 1 when not a number);
    // splicetest.toggle flips it.
    extern bool sSpliceTest;
    extern U32 sSpliceTestPasses;

    // --- Native shadow passes recorded on the cull threads (research/66 step 2)
    // Each native shadow pass of the sun cascades (and of the probes' shared
    // maps) is prepared on the main thread (buffers, texture slots, the GPU
    // list's compute, beginPass, pipelines, skinning palettes) and its draw
    // commands are recorded by a cull thread into a spliced stream (LLVKRecorder):
    // the same commands in the same order. The packets stay as built on the
    // main thread. On unless LLRHI_SHADOW_RECORD_THREADS=0; shadowrecord.toggle
    // flips it. Needs sCullThreads and sRecordSplice.
    extern bool sShadowRecordThreads;
    // Set by LLPipeline::renderSunShadowCascades around its cascades: >= 0 while
    // a native shadow pass may be recorded on a cull thread, the counters' bin
    // (cascade index, + 4 for the probes' shared maps). The caller then keeps
    // the packets it hands to sDrawShadowPackets, and the model and texture
    // matrices they point at, alive and unchanged until it calls
    // sJoinShadowRecording. -1 otherwise (recorded at once, as before).
    extern S32 sShadowRecordBin;
    // Set by the viewer (LLPipeline's cull threads): runs fn(arg) on one of
    // them without waiting for it. False when there are none, or while
    // sCullThreads is off (do it yourself). Set once during the viewer's
    // static initialization (research/137 N11), so it never changes while
    // the backend's threads run.
    extern bool (*sPostWork)(void (*fn)(void*), void* arg);
    // Set by the Vulkan backend: returns once every pass handed to a cull thread
    // since the last call has been recorded (taking over those no thread has
    // started). Main thread.
    extern void (*sJoinShadowRecording)();
    // Native shadow packets built on the cull threads (research/66 step 2B):
    // drawNativeShadowCasters builds the packets of the opaque, non-rigged
    // render types (most of them) in chunks on the cull threads while the main
    // thread builds the rest (rigged: skinning palettes and their table rows;
    // alpha: texture names), then puts them together in the original order,
    // deciding the GPU list rows (rowLive) and the merges under the list lock:
    // the same packets in the same order. On unless
    // LLRHI_SHADOW_PACKET_THREADS=0; shadowpacket.toggle flips it. Needs sCullThreads.
    extern bool sShadowPacketThreads;
    struct CoverageCounts
    {
        U64 groups = 0, draws = 0;              // tested (draws: batches in their draw maps)
        U64 zero_groups = 0, zero_draws = 0;    // no pixel centre
        U64 one_groups = 0, one_draws = 0;      // at most 1x1
        U64 two_groups = 0, two_draws = 0;      // at most 2x2
        U64 skipped_groups = 0, skipped_draws = 0;  // left out by the cull
        U32 frames = 0;                         // passes counted
    };
    // One pass's view: what the box test needs, and its counts.
    struct CoverageView
    {
        F32 view_proj[16] = {};     // agent space to clip space, column-major (projection * modelview)
        F32 width = 0.f;            // its viewport, which is the whole render target, in pixels
        F32 height = 0.f;
        bool active = false;        // test (and count) the groups of this pass's cull
        bool cull = false;          // leave out those with no pixel centre
        F32 jitter = 0.f;           // pixels the view's draws may be moved by (TAA's jitter, research/185 §13): counted that much wider
        CoverageCounts counts;
    };
    // The pixel centres along x and along y that the agent space box lo..hi can
    // cover in the view: what its corners project to, grown by the float error
    // of this projection and the GPU's and by the rasteriser's vertex snapping,
    // counted inside the viewport (at most 3: 3 means 3 or more). False when
    // that is unknown (a corner at or behind the eye plane): treat it as large.
    bool coverage(const CoverageView& view, const F32* lo, const F32* hi, U32& nx, U32& ny);

    // --- Shadow receiver cull (research/64) ---------------------------------
    // The main view reads sun cascade k only for pixels whose eye depth (after
    // the shader's normal offset) lies in k's band of shadowUtil.glsl, and only
    // texels within a few of the pixel's own (bilinear PCF, five taps, x
    // jitter). A caster drawn every frame (bridges, rigged, animated, not yet
    // settled) whose box, swept along the light, cannot reach a texel such a
    // pixel (or an impostor render of this frame) reads with a depth in front
    // of it is left out of cascade k (LLPipeline::renderSunShadowCascades).
    // Same image while the shadow maps are sampled without anisotropy; with
    // anisotropic filtering (RenderAnisotropicLevel above 1) the footprint
    // follows screen derivatives and is not bounded, so the cull holds off
    // then. On unless LLRHI_SHADOW_RECEIVER_CULL=0; receivercull.toggle.
    extern bool sShadowReceiverCull;
    // Cull even while the shadow maps are sampled anisotropically (not proven
    // exact then: a few silhouette pixels may differ; chosen 2026-10-11). On
    // unless LLRHI_SHADOW_RECEIVER_CULL_ANISO=0; receivercullaniso.toggle.
    extern bool sShadowReceiverCullAniso;

    // --- The retired translation thread (research/118, research/207 T1) -----
    // Always null: the translation thread is gone. Only LLTexUnit's
    // direct_handle (llrender.cpp) still asks; it goes with that file's
    // stage 7 work.
    extern void (*sTranslateFlush)();

    // --- Sun shadow walks kept while their inputs stay the same (research/113)
    // The main view's sun cascades walk every region's octrees on the cull
    // threads each frame. A partition's walk is handed over from the last
    // frame instead when nothing it reads changed: the cascade's camera
    // planes, its zero coverage view, the GPU shadow list's residency, a
    // region crossing, and the partition's octree (same root group, clean,
    // with the same unbound count). The same groups in the same order, so the
    // same image; the groups' state is still updated every frame as before
    // (plus/shadowreuse). On unless LLRHI_SHADOW_SAME_INPUT=0;
    // shadowsameinput.toggle.
    extern bool sShadowSameInput;

    // --- Sun shadow casters' depth bias (plus/shadowdist, research/107 section 20)
    // While active (set around the casters of the main view's sun cascades),
    // every draw's depth bias is this one, whatever GL's polygon offset says:
    // the slope-scaled bias pushes surfaces the light grazes (the ground)
    // back in the map, so the receivers need only a texel or so of their own
    // bias. Vulkan only (the translated draws' and the native passes' dynamic
    // state, LLVKTracker::beginPass); GL ignores it. Main thread.
    struct ShadowDepthBias
    {
        bool active = false;
        F32 constant = 0.f;     // vkCmdSetDepthBias depthBiasConstantFactor
        F32 slope = 0.f;        // depthBiasSlopeFactor
        F32 clamp = 0.f;        // depthBiasClamp (0 unless sShadowDepthBiasClamp)
    };
    extern ShadowDepthBias sShadowDepthBias;
    // The device clamps depth bias (the depthBiasClamp feature is on; set by the backend).
    extern bool sShadowDepthBiasClamp;

    // Skinned casters of the native shadow passes, counted by the backend for
    // plus/shadowdist's diagnostics (research/107 section 27); read and reset
    // by them. Main thread.
    struct ShadowSkinnedCounts
    {
        U32 casters = 0;        // skinned casters drawn
        U32 unresolved = 0;     // not drawn: their vertex or index buffer is not on the GPU
        U32 no_palette = 0;     // not drawn: no room for their joint palette
        U32 no_weights = 0;     // not drawn: rigged, without skin weights
    };
    extern ShadowSkinnedCounts sShadowSkinned;
}

#endif // LL_LLRHI_H
