/**
 * @file llvkmotion.cpp
 * @brief The G-buffer's motion vectors (research/373 section 2.2): the
 *        camera's motion filled in from the depth where nothing wrote its own,
 *        and a view of the vectors (LLRHI_MOTION_VIEW).
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

#include "llvkmotion.h"

#include "llrhimotion.h"
#include "llvkdevice.h"
#include "llvkfullscreen.h"
#include "llvkshader.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names (vkCmdClearAttachments goes through the recorder)

#include <cstring>
#include <string>
#include <vector>

namespace
{
    // The camera's pass: one triangle over the G-buffer, blended under w (Blend::UnderTargetAlpha): xy kept where a
    // thing wrote its own motion (w 1), the camera's where it did not (w 0; the attachment is cleared to 0 before the
    // G-buffer, plus/temporal). At the far depth (LLRHI::MOTION_FAR_NDC_Z and beyond: the sky dome, the sun, the moon
    // and the stars squashed to the far clip, and nothing drawn) the camera's motion is its turning only. Only xy is
    // written (z and w stay); the G-buffer's own four attachments not at all (their write masks are 0). No read of
    // the attachment written.
    const char* const CAMERA_FRAGMENT = R"GLSL(
layout(std140, set = 0, binding = 0) uniform MotionCamera
{
    mat4 inverse_projection;    // the clip space the G-buffer was drawn in (jittered) to the eye space
    mat4 projection;            // the eye space to this frame's clip space, without the jitter
    mat4 reproject;             // the eye space to the last frame's clip space, without the jitter
    vec4 params;                // x: 1 with a last frame, y: the NDC z from which a depth is the far one (LLRHI::MOTION_FAR_NDC_Z)
    vec2 depth_to_ndc;          // ndc z = stored depth * x + y
    ivec2 size;                 // the G-buffer's size
} u;

layout(set = 0, binding = 1) uniform sampler2D scene_depth;

layout(location = 4) out vec4 frag_motion;     // LLRHI::MOTION_ATTACHMENT

// UV either way: more is a point next to or behind the last camera; kept well inside half floats.
const float MAX_MOTION = 2.0;

vec2 uvOf(vec4 clip)
{
    return (clip.xy / clip.w) * 0.5 + 0.5;
}

// Where the surface seen at the pixel was in the last frame, less where it is now (both without the jitter).
vec2 cameraMotion(ivec2 pixel, float ndc_z, bool far_depth)
{
    if (u.params.x < 0.5)
    {
        return vec2(0.0);       // no last frame
    }
    vec2 uv = (vec2(pixel) + 0.5) / vec2(u.size);
    vec4 eye = u.inverse_projection * vec4(uv * 2.0 - 1.0, ndc_z, 1.0);
    if (!(abs(eye.w) > 1e-8))
    {
        return vec2(0.0);
    }
    eye /= eye.w;
    // The far depth is a direction: it moves with the camera's turning only.
    vec4 point = vec4(eye.xyz, far_depth ? 0.0 : 1.0);
    vec4 now = u.projection * point;
    vec4 last = u.reproject * point;
    if (!(now.w > 1e-4) || !(last.w > 1e-4))
    {
        return vec2(0.0);       // behind one of the cameras
    }
    vec2 motion = uvOf(last) - uvOf(now);
    if (any(isnan(motion)) || any(isinf(motion)))
    {
        return vec2(0.0);
    }
    return clamp(motion, vec2(-MAX_MOTION), vec2(MAX_MOTION));
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float ndc_z = texelFetch(scene_depth, pixel, 0).r * u.depth_to_ndc.x + u.depth_to_ndc.y;
    frag_motion = vec4(cameraMotion(pixel, ndc_z, ndc_z >= u.params.y), 0.0, 0.0);
}
)GLSL";

    // LLRHI_MOTION_VIEW: the vectors over the finished picture (most of the colour theirs, the picture faintly under it).
    const char* const VIEW_FRAGMENT = R"GLSL(
layout(std140, set = 0, binding = 0) uniform MotionView
{
    vec4 params;        // x: the mode (LLRHI::MotionView), y: the pixels of motion shown at full colour, zw: the target's size
    ivec2 size;         // the motion vectors' size (the drawn one)
    ivec2 unused;
} u;

layout(set = 0, binding = 1) uniform sampler2D motion;

layout(location = 0) out vec4 frag_color;

const float SHOWN = 0.85;

void main()
{
    vec2 uv = gl_FragCoord.xy / u.params.zw;
    ivec2 at = clamp(ivec2(uv * vec2(u.size)), ivec2(0), u.size - 1);
    vec4 m = texelFetch(motion, at, 0);
    vec3 colour;
    if (u.params.x < 1.5)
    {
        // Grey where nothing moves; red where the surface was to the right a frame ago, green where it was above.
        vec2 pixels = m.xy * vec2(u.size);
        colour = vec3(0.5 + 0.5 * clamp(pixels / u.params.y, -1.0, 1.0), 0.5);
    }
    else
    {
        colour = vec3(clamp(m.w, 0.0, 1.0), 0.0, clamp(m.z, 0.0, 1.0));     // an own motion in red, the composition mark in blue
    }
    frag_color = vec4(colour * SHOWN, SHOWN);   // premultiplied, laid over the picture
}
)GLSL";

    // The uniform blocks, as std140 lays them out.
    struct CameraUniforms
    {
        F32 inverse_projection[16];
        F32 projection[16];
        F32 reproject[16];
        F32 params[4];
        F32 depth_to_ndc[2];
        S32 size[2];
    };
    static_assert(sizeof(CameraUniforms) == 224, "CameraUniforms must match the std140 block MotionCamera");

    struct ViewUniforms
    {
        F32 params[4];
        S32 size[2];
        S32 unused[2];
    };
    static_assert(sizeof(ViewUniforms) == 32, "ViewUniforms must match the std140 block MotionView");

    // The G-buffer's attachments with the motion vectors last.
    constexpr U32 COLOR_COUNT = LLRHI::MOTION_ATTACHMENT + 1;
    static_assert(COLOR_COUNT <= LLVKMirror::MAX_COLORS, "the G-buffer with its motion vectors is a target the translation can bind");
    // The view: a motion of this many pixels a frame (or more) at full colour.
    constexpr F32 VIEW_FULL_PIXELS = 16.f;

    // The bound target (as beginPass gave it) is the G-buffer of width x height with its motion attachment: what the clear
    // and the camera's pass both draw on (the padded slot of a G-buffer without the attachment has no image).
    bool motionTargetFits(const LLVKMirror::Target& target, U32 width, U32 height)
    {
        return width && height && target.width == width && target.height == height && target.color_count == COLOR_COUNT &&
               target.colors[LLRHI::MOTION_ATTACHMENT] != VK_NULL_HANDLE;
    }

    class Renderer
    {
    public:
        bool camera(const LLRHI::MotionCameraPass& pass);
        bool view(const LLRHI::MotionViewPass& pass);

        // research/159 3.1 (--bake-shaders): the passes set up ahead of their first use.
        bool bake() { return ready(); }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        std::string mCameraSource;
        std::string mViewSource;
        LLVKFullscreen::Pass mCamera;
        LLVKFullscreen::Pass mView;
        // The textures each pass samples: kept so that no frame allocates them anew (as the wet look's).
        std::vector<LLVKTracker::GLName> mCameraSampled;
        std::vector<LLVKTracker::GLName> mViewSampled;
        bool mLogged = false;           // the first camera pass's line
        bool mWarnedTarget = false;     // a bound target of another size or make (warned once)
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mCameraSource = std::string("#version 450\n") + CAMERA_FRAGMENT;
            mViewSource = std::string("#version 450\n") + VIEW_FRAGMENT;
            const VkColorComponentFlags writes[COLOR_COUNT] = { 0, 0, 0, 0, VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT };
            const LLVKFullscreen::Blend under_w[COLOR_COUNT] = { LLVKFullscreen::Blend::Replace, LLVKFullscreen::Blend::Replace,
                                                                 LLVKFullscreen::Blend::Replace, LLVKFullscreen::Blend::Replace,
                                                                 LLVKFullscreen::Blend::UnderTargetAlpha };
            mReady = mCamera.initColors("motion camera", mCameraSource.c_str(), 1, COLOR_COUNT, writes, LLVKFullscreen::StencilTest(), under_w) &&
                     mView.init("motion view", mViewSource.c_str(), 1, LLVKFullscreen::Blend::PremultipliedOver);
            if (!mReady)
            {
                LL_WARNS("VKMotion") << "The motion vector passes could not be set up; the camera's motion stays 0" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::camera(const LLRHI::MotionCameraPass& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!pass.depth_texture || !pass.width || !pass.height || !ready())
        {
            return false;
        }
        CameraUniforms uniforms = {};
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        memcpy(uniforms.projection, pass.projection, sizeof(uniforms.projection));
        memcpy(uniforms.reproject, pass.reproject, sizeof(uniforms.reproject));
        uniforms.params[0] = pass.has_history ? 1.f : 0.f;
        uniforms.params[1] = LLRHI::MOTION_FAR_NDC_Z;
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)pass.width;
        uniforms.size[1] = (S32)pass.height;

        mCameraSampled.assign(1, pass.depth_texture);
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mCameraSampled, target);
        if (!cmd)
        {
            return false;
        }
        if (!motionTargetFits(target, pass.width, pass.height))
        {
            LLVKTracker::endPass();
            if (!mWarnedTarget)
            {
                mWarnedTarget = true;
                LL_WARNS("VKMotion") << "Motion vectors: the bound target is " << target.width << "x" << target.height << " with "
                                     << target.color_count << " attachments, not the G-buffer of " << pass.width << "x" << pass.height
                                     << " with " << COLOR_COUNT << "; not drawn" << LL_ENDL;
            }
            return false;
        }
        // The depth is the target's own (shared with the screen): attached read-only and sampled in that layout.
        const LLVKFullscreen::Image images[1] = { { pass.depth_texture, target.depth_read_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        const bool drawn = mCamera.draw(cmd, target, &uniforms, sizeof(uniforms), images, 1);
        LLVKTracker::endPass();
        if (drawn && !mLogged)
        {
            mLogged = true;
            LL_INFOS("VKMotion") << "Motion vectors: the camera's pass on " << pass.width << "x" << pass.height << ", format "
                                 << (U32)target.color_formats[LLRHI::MOTION_ATTACHMENT] << LL_ENDL;
        }
        return drawn;
    }

    bool Renderer::view(const LLRHI::MotionViewPass& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!pass.motion_texture || !pass.motion_width || !pass.motion_height || pass.mode == LLRHI::MOTION_VIEW_OFF ||
            pass.mode >= LLRHI::MOTION_VIEW_COUNT || !ready())
        {
            return false;
        }
        mViewSampled.assign(1, pass.motion_texture);
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass(mViewSampled, target);
        if (!cmd)
        {
            return false;
        }
        ViewUniforms uniforms = {};
        uniforms.params[0] = (F32)pass.mode;
        uniforms.params[1] = VIEW_FULL_PIXELS;
        uniforms.params[2] = (F32)llmax(target.width, 1u);
        uniforms.params[3] = (F32)llmax(target.height, 1u);
        uniforms.size[0] = (S32)pass.motion_width;
        uniforms.size[1] = (S32)pass.motion_height;
        const LLVKFullscreen::Image images[1] = { { pass.motion_texture } };
        const bool drawn = mView.draw(cmd, target, &uniforms, sizeof(uniforms), images, 1);
        LLVKTracker::endPass();
        return drawn;
    }

    // LLRHI::sMotionClear: the bound G-buffer's motion attachment to (0, 0, 0, 0), in its rendering scope (no pipeline).
    bool record_clear(U32 width, U32 height)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        static const std::vector<LLVKTracker::GLName> NOTHING_SAMPLED;
        LLVKMirror::Target target;
        VkCommandBuffer cmd = width && height ? LLVKTracker::beginPass(NOTHING_SAMPLED, target) : VK_NULL_HANDLE;
        if (!cmd)
        {
            return false;
        }
        const bool fits = motionTargetFits(target, width, height);
        if (fits)
        {
            VkClearAttachment attachment = {};
            attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachment.colorAttachment = LLRHI::MOTION_ATTACHMENT;      // clearValue: all 0
            VkClearRect rect = {};
            rect.rect = { { 0, 0 }, { width, height } };
            rect.layerCount = 1;
            vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
        }
        LLVKTracker::endPass();
        return fits;
    }

    bool record_camera(const LLRHI::MotionCameraPass& pass)
    {
        return sRenderer.camera(pass);
    }

    bool record_view(const LLRHI::MotionViewPass& pass)
    {
        return sRenderer.view(pass);
    }
}

void LLVKMotion::install()
{
    if (!LLRHI::sMotion)
    {
        LL_INFOS("VKMotion") << "Motion vectors off (LLRHI_TEMPORAL=0)" << LL_ENDL;
        return;
    }
    // The G-buffer with its motion vectors is bound as one target and its programs write five outputs (the device's
    // limits may be 4, Vulkan's least); the camera's pass blends the fifth attachment only, which needs
    // independentBlend (llvkdevice enables it where the GPU has it).
    const VkPhysicalDeviceLimits& limits = LLVKDevice::instance().getLimits();
    VkPhysicalDeviceFeatures features = {};
    vkGetPhysicalDeviceFeatures(LLVKDevice::instance().getPhysicalDevice(), &features);
    if (limits.maxColorAttachments < COLOR_COUNT || limits.maxFragmentOutputAttachments < COLOR_COUNT || !features.independentBlend)
    {
        LL_INFOS("VKMotion") << "Motion vectors off: the device has " << limits.maxColorAttachments << " colour attachments, "
                             << limits.maxFragmentOutputAttachments << " fragment outputs (" << COLOR_COUNT << " needed), independent blend "
                             << (features.independentBlend ? "on" : "off") << LL_ENDL;
        return;
    }
    LLRHI::sMotionClear = &record_clear;
    LLRHI::sMotionCamera = &record_camera;
    LLRHI::sMotionViewDraw = &record_view;
    LLVKShader::addBakeStep("motion", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
