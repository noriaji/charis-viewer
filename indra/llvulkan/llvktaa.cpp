/**
 * @file llvktaa.cpp
 * @brief Temporal anti-aliasing (research/185): the last frame's picture
 *        brought to this frame by the camera's motion (from the scene depth),
 *        clipped to the colours around the pixel, and mixed in. With the
 *        projection jittered, this frame's colour is made at the pixel's
 *        unmoved centre. No velocity buffer.
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

#include "llvktaa.h"

#include "llrhitaa.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvktracker.h"

#include <cmath>
#include <cstring>

namespace
{
    // The pixel's colour clipped into the box of the 3x3 around it (mean +/-
    // CLIP_SIGMA standard deviations, and this frame's colour) is what the
    // history may be: a surface that was not there a frame ago (or moved) is
    // taken by the new picture.
    //
    // Flicker that comes round with the jitter (research/250): where a thing is
    // finer than a pixel (a gap or a bar narrower than one, a lattice seen at a
    // slant, two faces in one plane), the jitter shows a whole 3x3 one side of
    // it in some frames and the other side in others. The box then holds only
    // this frame's side, the history was clipped to it each frame, and the
    // pass showed the jittered picture itself: a flicker the picture without
    // TAA does not have. In a still scene the jitter's round (8 points) brings
    // the same colours back every 8 frames, which nothing that moves or
    // changes does. A second target (the memory) keeps, per pixel, whether
    // the colour was above or below its running mean (the result's alpha) in
    // each of the last 8 frames and how many frames in a row agreed with the
    // round before; the 3x3's nearest surface, so that a thing coming in
    // front clears it at once; and for how many rounds in a row the clip has
    // moved the history by much (a fine pattern whose box holds the history
    // never flickered, and a change clips once, not every round). Where the
    // camera stands, that surface is still the nearest, the colour has been
    // on both sides of the mean within the round, the round has repeated for
    // a while and the clip has thrown much away for two rounds and more, the
    // history is kept by hold.x instead of being clipped (and mixed in more
    // slowly). A colour that turns against the round (the light changed, a
    // thing went by at the same distance) takes that away: at once when it
    // turns by much.
    const char* const TAA_FRAGMENT = R"GLSL(
layout(std140, set = 0, binding = 0) uniform Taa
{
    mat4 reproject;             // this frame's eye space to the last frame's clip space
    mat4 inverse_projection;    // this frame's clip space to its eye space
    vec4 params;                // x: weight of the history where still, y: where moving, z: 1 with a history, w: pixels of motion that count as moving
    vec2 depth_to_ndc;          // ndc z = stored depth * x + y
    ivec2 size;                 // the picture's size
    vec4 jitter;                // xy: a pixel shows the unjittered view at its centre minus xy (pixels); z: 1 when jittered
    vec4 hold;                  // x: how much of the history is kept where the flicker comes round with the jitter (0..1), y: the history's weight there once settled,
                                // z: 1 where this frame's jitter point follows the history's (0: the round broke, the memory begins again), w: the point (0..7)
} u;

layout(set = 0, binding = 1) uniform sampler2D source_color;
layout(set = 0, binding = 2) uniform sampler2D history_color;      // rgb: the last result; a: the running mean of its colour's luma
layout(set = 0, binding = 3) uniform sampler2D scene_depth;
layout(set = 0, binding = 4) uniform sampler2D history_memory;     // the flicker's memory (an integer in R32F)

layout(location = 0) out vec4 frag_color;
layout(location = 1) out float frag_memory;

const float CLIP_SIGMA = 1.25;
// Where the picture moves (research/278): the box is narrowed to this many sigmas, and a history that had to be
// clipped far (it is of another picture) weighs this much less at the most. Both by the same measure of the motion.
const float CLIP_SIGMA_MOVING = 0.8;
const float CLIPPED_FAR_DROP = 0.35;
// How far the clip moved the history, in colour (not in boxes: in a flat sky or a glow the box is a point, and a step
// or two of the dither is already several boxes).
const float CLIPPED_FAR_FROM = 16.0 / 255.0;
const float CLIPPED_FAR_FULL = 64.0 / 255.0;
// The camera's motion at the pixel (pixels) up to which a flicker's history is kept fully, and from which not at all:
// only a camera that stands (a slow turn carries the picture over the pixels' borders).
const float STILL_PIXELS = 0.02;
const float MOVING_PIXELS = 0.1;
// The 3x3's nearest surface against the last frame's: the same up to the first, another from the second.
const float SAME_DEPTH = 0.035;
const float OTHER_DEPTH = 0.07;

// The memory, an integer below 2^24 (which a float holds exactly):
//   this round's clip << 23 | the rounds << 20 | the nearest surface's level + 1 << 12 | the confidence << 8 | the signs.
// The level is the distance in 1/20 of an octave, from 1/4 m (1) to 1024 m (241), 0 for the sky; stored one up, so that
// 0 is nothing remembered. A sign is whether the colour was above the running mean; bit 0 is this frame's, bit 7 the one
// a round of the jitter ago (the same point of it: in a still scene, the same colour). The confidence (0..15) counts the
// frames in a row whose sign was the round's. The rounds (0..7) count the rounds in a row in which the clip moved the
// history by LARGE_CLIP or more at least once (this round's clip: it did so in the round going on).
const float LEVELS_PER_OCTAVE = 20.0;
const float LEVEL_OF_1M = 41.0;
const float LEVEL_LAST = 241.0;
const uint MAX_CONFIDENCE = 15u;
const uint MAX_ROUNDS = 7u;
// The confidence from which the history is kept, fully from the second; and over which its weight rises to hold.y.
const float HOLD_FROM = 3.0;
const float HOLD_FULL = 6.0;
const float SETTLE_FROM = 8.0;
const float SETTLE_FULL = 15.0;
// The rounds from which the history is kept, fully from the second.
const float ROUNDS_FROM = 1.0;
const float ROUNDS_FULL = 2.0;
// A round counts where the clip moved the history by LARGE_CLIP; once kept, also where the colour still swings
// KEPT_SWING from its mean (the kept history may then sit inside the box); and where it swings SWING (two faces in one
// plane whose 3x3 is mixed: the box holds the history, but the colour leaps).
const float LARGE_CLIP = 8.0 / 255.0;
const float KEPT_SWING = 16.0 / 255.0;
const float SWING = 48.0 / 255.0;
// A colour this near the mean says nothing; one that turns against the round by less than OVERTURN (a colour near the
// mean, the mean's own ripple) neither. A turn costs MISMATCH_COST of the confidence, all of it from CHANGE.
const float NOISE = 2.0 / 255.0;
const float OVERTURN = 8.0 / 255.0;
const float CHANGE = 24.0 / 255.0;
const uint MISMATCH_COST = 8u;
// The running mean's weight: steady once the round repeats, quicker while learning (it centres within a round).
const float MEAN_STEADY = 0.95;
const float MEAN_LEARNING = 0.8;
// The history this far from the running mean (the hold has just begun): its usual weight, to get there quickly.
const float CATCH_FROM = 4.0 / 255.0;
const float CATCH_FULL = 12.0 / 255.0;

float luma(vec3 colour)
{
    return dot(colour, vec3(0.2126, 0.7152, 0.0722));
}

// The inverse of the distance along the view of a stored depth (1 / metres).
float inverseDistance(float stored)
{
    vec4 eye = u.inverse_projection * vec4(0.0, 0.0, stored * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
    return abs(eye.w / eye.z);
}

// Where the surface seen at the pixel at was a frame ago (uv). The sky (the far plane) moves with the camera's
// turning only. False behind the last frame's camera.
bool lastUV(ivec2 at, float stored, out vec2 previous_uv)
{
    vec2 uv = (vec2(at) + 0.5) / vec2(u.size);
    vec4 clip = vec4(uv * 2.0 - 1.0, stored * u.depth_to_ndc.x + u.depth_to_ndc.y, 1.0);
    vec4 eye = u.inverse_projection * clip;
    eye /= eye.w;
    bool sky = stored >= 1.0;
    vec4 previous_clip = u.reproject * vec4(eye.xyz, sky ? 0.0 : 1.0);
    previous_uv = vec2(0.0);
    if (!(previous_clip.w > 1e-4))
    {
        return false;
    }
    previous_uv = (previous_clip.xy / previous_clip.w) * 0.5 + 0.5;
    return true;
}

// The history at uv, by Catmull-Rom through five bilinear taps (the corners
// left out), so that it does not blur a little more each frame.
vec3 historyAt(vec2 uv)
{
    vec2 position = uv * vec2(u.size);
    vec2 center = floor(position - 0.5) + 0.5;
    vec2 f = position - center;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    vec2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    vec2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    vec2 w3 = 0.5 * f3 - 0.5 * f2;
    vec2 w12 = w1 + w2;
    vec2 inv_size = 1.0 / vec2(u.size);
    vec2 tc0 = (center - 1.0) * inv_size;
    vec2 tc3 = (center + 2.0) * inv_size;
    vec2 tc12 = (center + w2 / max(w12, vec2(1e-5))) * inv_size;

    float wa = w12.x * w0.y;
    float wb = w0.x * w12.y;
    float wc = w12.x * w12.y;
    float wd = w3.x * w12.y;
    float we = w12.x * w3.y;
    vec3 sum = textureLod(history_color, vec2(tc12.x, tc0.y), 0.0).rgb * wa +
               textureLod(history_color, vec2(tc0.x, tc12.y), 0.0).rgb * wb +
               textureLod(history_color, tc12, 0.0).rgb * wc +
               textureLod(history_color, vec2(tc3.x, tc12.y), 0.0).rgb * wd +
               textureLod(history_color, vec2(tc12.x, tc3.y), 0.0).rgb * we;
    return max(sum / max(wa + wb + wc + wd + we, 1e-5), vec3(0.0));
}

ivec2 clampedPixel(ivec2 pixel)
{
    return clamp(pixel, ivec2(0), u.size - 1);
}

// The nearest depth of the 3x3 around the pixel, and where it is (a tie: the pixel itself).
void nearestAround(ivec2 pixel, out float nearest, out ivec2 nearest_at)
{
    nearest = 2.0;
    nearest_at = pixel;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float d = texelFetch(scene_depth, clampedPixel(pixel + ivec2(x, y)), 0).r;
            if (d < nearest || (d == nearest && x == 0 && y == 0))
            {
                nearest = d;
                nearest_at = clampedPixel(pixel + ivec2(x, y));
            }
        }
    }
}

// Where the pixel's surface was a frame ago (uv), as the nearest surface of the 3x3 moved: a pixel that sees the far
// side of a thin gap in one frame and the bars the next keeps one motion (the bars') while the camera moves.
// False when that was off the last frame's picture or behind its camera.
bool previousUV(ivec2 pixel, ivec2 nearest_at, float nearest, out vec2 previous_uv)
{
    vec2 nearest_last;
    previous_uv = vec2(0.0);
    if (!lastUV(nearest_at, nearest, nearest_last))
    {
        return false;
    }
    previous_uv = nearest_last + vec2(pixel - nearest_at) / vec2(u.size);
    return all(greaterThanEqual(previous_uv, vec2(0.0))) && all(lessThanEqual(previous_uv, vec2(1.0)));
}

// The colours of the 3x3 around the pixel: their mean and standard deviation. Jittered, this frame's colour at the
// pixel's centre is made from them too: a sample's weight falls with its distance from the centre (a Gaussian
// close to Blackman-Harris).
void coloursAround(ivec2 pixel, inout vec3 current, out vec3 mean, out vec3 deviation)
{
    vec3 mean_square = vec3(0.0);
    vec3 centred = vec3(0.0);
    float centred_weight = 0.0;
    mean = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec3 c = texelFetch(source_color, clampedPixel(pixel + ivec2(x, y)), 0).rgb;
            mean += c;
            mean_square += c * c;
            vec2 d = vec2(float(x), float(y)) - u.jitter.xy;
            float w = exp(-2.29 * dot(d, d));
            centred += c * w;
            centred_weight += w;
        }
    }
    if (u.jitter.z > 0.5)
    {
        current = centred / centred_weight;
    }
    mean /= 9.0;
    deviation = sqrt(max(mean_square / 9.0 - mean * mean, vec3(0.0)));
}

)GLSL"
// (two pieces: one literal may be at most 16 KB for MSVC)
R"GLSL(
// The 3x3's nearest surface (its stored depth) as a level of the memory.
uint surfaceLevel(float nearest)
{
    if (nearest >= 1.0)
    {
        return 0u;          // the sky
    }
    float distance = 1.0 / max(inverseDistance(nearest), 1e-6);
    return uint(clamp(round(log2(distance) * LEVELS_PER_OCTAVE) + LEVEL_OF_1M, 1.0, LEVEL_LAST));
}

// 1 where the nearest surface is the remembered one, 0 where it is another (the sky against a surface too).
float sameSurface(uint level, uint remembered)
{
    if (level == 0u || remembered == 0u)
    {
        return level == remembered ? 1.0 : 0.0;
    }
    float ratio = exp2((float(level) - float(remembered)) / LEVELS_PER_OCTAVE);
    return 1.0 - smoothstep(SAME_DEPTH, OTHER_DEPTH, abs(ratio - 1.0));
}

struct Memory
{
    bool known;
    uint signs;
    uint confidence;
    uint level;
    uint rounds;
    bool clipped_this_round;
};

Memory decodeMemory(float value)
{
    float rounded = round(value);
    uint bits = (rounded >= 0.0 && rounded < 16777216.0) ? uint(rounded) : 0u;     // also a NaN
    uint stored_level = (bits >> 12) & 255u;
    if (stored_level == 0u)
    {
        return Memory(false, 0u, 0u, 0u, 0u, false);
    }
    return Memory(true, bits & 255u, (bits >> 8) & 15u, stored_level - 1u, (bits >> 20) & 7u, ((bits >> 23) & 1u) != 0u);
}

float encodeMemory(Memory memory)
{
    return float((memory.clipped_this_round ? 1u << 23 : 0u) | (memory.rounds << 20) | ((memory.level + 1u) << 12) |
                 (memory.confidence << 8) | memory.signs);
}

// This frame's sign against the round's (bit 7: the same point of the jitter a round ago), into the signs and the
// confidence. difference: the colour's brightness less its running mean.
void addSign(inout Memory memory, float difference)
{
    float size = abs(difference);
    uint round_ago = (memory.signs >> 7) & 1u;
    uint sign = difference > 0.0 ? 1u : 0u;
    bool turned = sign != round_ago;
    if (size < NOISE || (turned && size < OVERTURN))
    {
        sign = round_ago;   // no evidence either way: the round's sign is kept
    }
    else if (!turned)
    {
        memory.confidence = min(memory.confidence + 1u, MAX_CONFIDENCE);
    }
    else
    {
        memory.confidence = (size >= CHANGE || memory.confidence < MISMATCH_COST) ? 0u : memory.confidence - MISMATCH_COST;
    }
    memory.signs = ((memory.signs << 1) | sign) & 255u;
}

// This frame's part in the rounds (large: the clip threw much away); a round is closed at its first point.
void addToRound(inout Memory memory, bool large)
{
    if (uint(u.hold.w) == 0u)
    {
        memory.rounds = memory.clipped_this_round ? min(memory.rounds + 1u, MAX_ROUNDS) : 0u;
        memory.clipped_this_round = large;
    }
    else
    {
        memory.clipped_this_round = memory.clipped_this_round || large;
    }
}

// What the flicker's memory lets this frame do, and what it leaves for the next.
struct Hold
{
    float amount;   // how much of the history is kept beyond the box (0..1)
    float blend;    // the history's weight where it is kept
    float mean;     // the running mean for the next frame (the result's alpha)
    float memory;   // the memory for the next frame
};

// history_seen: the history brought here; history_clipped: it clipped to this frame's box.
Hold flickerHold(ivec2 previous_pixel, float nearest, float motion, vec3 current, vec3 history_seen, vec3 history_clipped)
{
    float brightness = luma(current);
    Hold hold = Hold(0.0, u.params.x, brightness, 0.0);
    float still = 1.0 - smoothstep(STILL_PIXELS, MOVING_PIXELS, motion);
    if (!(u.hold.x > 0.0 && still > 0.0))
    {
        return hold;        // off, or the camera moves: nothing kept, nothing remembered
    }
    Memory memory = decodeMemory(texelFetch(history_memory, previous_pixel, 0).r);
    // The jitter's round broke (a frame without TAA between): the signs are of other points, so begin again.
    memory.known = memory.known && u.hold.z > 0.5;
    uint level = surfaceLevel(nearest);
    float same = memory.known ? sameSurface(level, memory.level) : 0.0;
    // Another surface nearest (a thing came in front): the confidence, the rounds and the running mean begin again.
    Memory next = Memory(true, memory.known ? memory.signs : 0u, 0u, level, 0u, false);
    if (same > 0.0)
    {
        next.confidence = memory.confidence;
        next.rounds = memory.rounds;
        next.clipped_this_round = memory.clipped_this_round;
    }
    float running = same > 0.0 ? texelFetch(history_color, previous_pixel, 0).a : brightness;
    float difference = brightness - running;
    addSign(next, difference);

    // Kept where the colour was on both sides of its mean within the round (it flickers), the round repeated, and the
    // clip threw much away round after round (the remembered rounds: a change of this frame does not open it).
    float trust = float(next.confidence);
    bool flickers = next.signs != 0u && next.signs != 255u;
    hold.amount = flickers ? clamp(u.hold.x, 0.0, 1.0) * smoothstep(HOLD_FROM, HOLD_FULL, trust) *
                             smoothstep(ROUNDS_FROM, ROUNDS_FULL, float(next.rounds)) * still * same : 0.0;
    float settled = mix(u.params.x, max(u.params.x, u.hold.y), smoothstep(SETTLE_FROM, SETTLE_FULL, trust));
    hold.blend = mix(settled, u.params.x, smoothstep(CATCH_FROM, CATCH_FULL, abs(luma(history_seen) - running)));
    hold.mean = running + difference * (trust >= HOLD_FULL ? 1.0 - MEAN_STEADY : 1.0 - MEAN_LEARNING);

    float swing = abs(difference);
    addToRound(next, abs(luma(history_seen) - luma(history_clipped)) >= LARGE_CLIP ||
                     (float(next.rounds) >= ROUNDS_FULL && swing >= KEPT_SWING) || swing >= SWING);
    hold.memory = encodeMemory(next);
    return hold;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 current = texelFetch(source_color, pixel, 0);
    float nearest;
    ivec2 nearest_at;
    vec2 previous_uv;
    // Without a history (or one to bring here): the picture as it is, and nothing remembered.
    frag_memory = 0.0;
    if (u.params.z < 0.5)
    {
        frag_color = vec4(current.rgb, 0.0);
        return;
    }
    nearestAround(pixel, nearest, nearest_at);
    if (!previousUV(pixel, nearest_at, nearest, previous_uv))
    {
        frag_color = vec4(current.rgb, 0.0);
        return;
    }
    float motion = length(previous_uv * vec2(u.size) - (vec2(pixel) + 0.5));
    // 0 where the camera stands (up to MOVING_PIXELS: nothing below changes), 1 from u.params.w pixels a frame; it
    // rises quickly, so that a slow turn already sheds the history (a trail of a few frames is a smear of the picture).
    float ramp = clamp((motion - MOVING_PIXELS) / max(u.params.w - MOVING_PIXELS, 1e-3), 0.0, 1.0);
    // The first tenth of the way is eased in: a motion that wavers about MOVING_PIXELS must not swing the weight.
    float moving = sqrt(ramp) * smoothstep(0.0, 0.1, ramp);

    vec3 mean;
    vec3 deviation;
    coloursAround(pixel, current.rgb, mean, deviation);
    // This frame's colour is always inside (research/246): a bright dot of one pixel among eight dark ones lies
    // beyond mean + 1.25 sigma, and its history was cut below what the frame itself shows.
    float sigma = mix(CLIP_SIGMA, CLIP_SIGMA_MOVING, moving);
    vec3 box_min = min(mean - sigma * deviation, current.rgb);
    vec3 box_max = max(mean + sigma * deviation, current.rgb);
    vec3 box_centre = 0.5 * (box_min + box_max);

    // The history clipped towards the box's centre, to its surface.
    vec3 history_seen = historyAt(previous_uv);
    vec3 history = history_seen;
    vec3 offset = history_seen - box_centre;
    // At least two steps of an 8-bit picture either way (research/240): in a smooth area (a glow's halo, the sky's
    // dithered clouds) the box is almost a point, and a change of a step or two each frame went through it unsmoothed.
    vec3 extent = max(0.5 * (box_max - box_min), vec3(2.0 / 255.0));
    vec3 scaled = abs(offset / extent);
    float outside = max(max(scaled.x, scaled.y), scaled.z);
    if (outside > 1.0)
    {
        history = box_centre + offset / outside;
    }
    vec3 clipped_by = abs(history_seen - history);
    float clipped_far = max(max(clipped_by.x, clipped_by.y), clipped_by.z);

    // A flicker that comes round with the jitter keeps its history (research/250).
    Hold hold = flickerHold(clampedPixel(ivec2(previous_uv * vec2(u.size))), nearest, motion, current.rgb, history_seen, history);
    history = mix(history, history_seen, hold.amount);

    // The less it moved, the more of the history there is; a kept flicker more still, once it has settled.
    float weight = mix(u.params.x, u.params.y, moving);
    weight = mix(weight, max(weight, hold.blend), hold.amount);
    // Where the picture moves and the history had to be clipped far, it is of another picture: less of it.
    weight *= 1.0 - moving * CLIPPED_FAR_DROP * smoothstep(CLIPPED_FAR_FROM, CLIPPED_FAR_FULL, clipped_far);

    frag_color = vec4(mix(current.rgb, history, weight), hold.mean);
    frag_memory = hold.memory;
}
)GLSL";

    // The uniform block Taa, as std140 lays it out.
    struct Uniforms
    {
        F32 reproject[16];
        F32 inverse_projection[16];
        F32 params[4];
        F32 depth_to_ndc[2];
        S32 size[2];
        F32 jitter[4];
        F32 hold[4];
    };
    static_assert(sizeof(Uniforms) == 192, "Uniforms must match the std140 block Taa");
    // The memory keeps one sign per point of the jitter's round, in 8 bits.
    static_assert(LLRHI::TAA_JITTER_ROUND == 8, "the TAA's flicker memory keeps 8 signs");

    // A picture that moved this many pixels (or more) in a frame has the moving weight.
    constexpr F32 MOTION_FULL_PIXELS = 2.f;

    // The images the pass samples, and the attachments it writes (the result, the memory).
    constexpr U32 IMAGE_COUNT = 4;
    constexpr U32 COLOR_COUNT = 2;

    class Renderer
    {
    public:
        bool draw(const LLRHI::TaaPass& pass);

        // research/159 3.1 (--bake-shaders): the pass set up ahead of its first use.
        bool bake() { return ready(); }

    private:
        bool ready();

        bool mTried = false;
        bool mReady = false;
        std::string mSource;
        LLVKFullscreen::Pass mPass;
        bool mLogged = false;           // the first draw's line (plus/taa logs each new size of its targets)
        bool mWarnedTarget = false;     // a bound target of another size or make (warned once)
    };

    Renderer sRenderer;

    bool Renderer::ready()
    {
        if (!mTried)
        {
            mTried = true;
            mSource = std::string("#version 450\n") + TAA_FRAGMENT;
            const VkColorComponentFlags writes[COLOR_COUNT] = { LLVKFullscreen::WRITE_RGBA, VK_COLOR_COMPONENT_R_BIT };
            mReady = mPass.initColors("taa", mSource.c_str(), IMAGE_COUNT, COLOR_COUNT, writes);
            if (!mReady)
            {
                LL_WARNS("VKTaa") << "The TAA pass could not be set up; the picture is left as it is" << LL_ENDL;
            }
        }
        return mReady;
    }

    bool Renderer::draw(const LLRHI::TaaPass& pass)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (!pass.source_texture || !pass.depth_texture || !pass.width || !pass.height || !ready())
        {
            return false;
        }
        const bool history = pass.history_texture != 0 && pass.history_memory_texture != 0;

        Uniforms uniforms = {};
        memcpy(uniforms.reproject, pass.reproject, sizeof(uniforms.reproject));
        memcpy(uniforms.inverse_projection, pass.inverse_projection, sizeof(uniforms.inverse_projection));
        uniforms.params[0] = llclamp(pass.blend_still, 0.f, 0.99f);
        uniforms.params[1] = llclamp(pass.blend_moving, 0.f, uniforms.params[0]);
        uniforms.params[2] = history ? 1.f : 0.f;
        uniforms.params[3] = MOTION_FULL_PIXELS;
        LLVKFullscreen::depthToNDC(uniforms.depth_to_ndc);
        uniforms.size[0] = (S32)pass.width;
        uniforms.size[1] = (S32)pass.height;
        // Jittered by at most half a pixel each way (plus/taa); more would be a wrong value, taken as none.
        const bool jittered = pass.jittered && fabsf(pass.jitter_pixels[0]) <= 0.5f && fabsf(pass.jitter_pixels[1]) <= 0.5f;
        uniforms.jitter[0] = jittered ? pass.jitter_pixels[0] : 0.f;
        uniforms.jitter[1] = jittered ? pass.jitter_pixels[1] : 0.f;
        uniforms.jitter[2] = jittered ? 1.f : 0.f;
        // research/250: a flicker that comes round with the jitter keeps its history, and the history's weight there.
        uniforms.hold[0] = llclamp(pass.flicker_keep, 0.f, 1.f);
        uniforms.hold[1] = llclamp(pass.flicker_blend, uniforms.params[0], 0.99f);
        const bool round_goes_on = jittered && pass.jitter_round_goes_on && pass.jitter_point < LLRHI::TAA_JITTER_ROUND;
        uniforms.hold[2] = round_goes_on ? 1.f : 0.f;
        uniforms.hold[3] = round_goes_on ? (F32)pass.jitter_point : 0.f;

        // Without a history the other images are only there to be bound: the picture itself.
        const LLVKTracker::GLName history_texture = history ? pass.history_texture : pass.source_texture;
        const LLVKTracker::GLName memory_texture = history ? pass.history_memory_texture : pass.source_texture;
        LLVKMirror::Target target;
        VkCommandBuffer cmd = LLVKTracker::beginPass({ pass.source_texture, history_texture, pass.depth_texture, memory_texture }, target);
        if (!cmd)
        {
            return false;
        }
        if (target.width != pass.width || target.height != pass.height || target.color_count != COLOR_COUNT)
        {
            LLVKTracker::endPass();
            if (!mWarnedTarget)
            {
                mWarnedTarget = true;
                LL_WARNS("VKTaa") << "TAA: the bound target is " << target.width << "x" << target.height << " with "
                                  << target.color_count << " attachments, not the " << pass.width << "x" << pass.height
                                  << " with " << COLOR_COUNT << " it was called for; not drawn" << LL_ENDL;
            }
            return false;
        }
        const LLVKFullscreen::Image images[IMAGE_COUNT] = { { pass.source_texture }, { history_texture }, { pass.depth_texture },
                                                            { memory_texture } };
        const bool drawn = mPass.draw(cmd, target, &uniforms, sizeof(uniforms), images, IMAGE_COUNT);
        LLVKTracker::endPass();
        if (drawn && !mLogged)
        {
            mLogged = true;
            LL_INFOS("VKTaa") << "TAA: " << pass.width << "x" << pass.height << ", target formats " << (U32)target.color_formats[0]
                              << " and " << (U32)target.color_formats[1] << LL_ENDL;
        }
        return drawn;
    }

    bool record_taa(const LLRHI::TaaPass& pass)
    {
        return sRenderer.draw(pass);
    }
}

void LLVKTaa::install()
{
    if (!LLRHI::sTaa)
    {
        LL_INFOS("VKTaa") << "TAA off (LLRHI_TAA=0)" << LL_ENDL;
        return;
    }
    LLRHI::sTaaDraw = &record_taa;
    LLVKShader::addBakeStep("taa", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return sRenderer.bake(); });   // research/159 3.1
}
