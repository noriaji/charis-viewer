/**
 * @file class3/deferred/screenSpaceReflUtil.glsl
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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

uniform sampler2D sceneMap;
uniform sampler2D sceneDepth;

uniform vec2 screen_res;
uniform mat4 projection_matrix;
//uniform float zNear;
//uniform float zFar;
uniform mat4 inv_proj;
uniform mat4 modelview_delta;  // should be transform from last camera space to current camera space
uniform mat4 inv_modelview_delta;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

float random (vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123); //simple random function
}

// Based off of https://github.com/RoundedGlint585/ScreenSpaceReflection/
// A few tweaks here and there to suit our needs.

vec2 generateProjectedPosition(vec3 pos)
{
    vec4 samplePosition = projection_matrix * vec4(pos, 1.f);
    samplePosition.xy = (samplePosition.xy / samplePosition.w) * 0.5 + 0.5;
    return samplePosition.xy;
}

bool isBinarySearchEnabled = true;
bool isAdaptiveStepEnabled = true;
bool isExponentialStepEnabled = true;
bool debugDraw = false;

uniform float iterationCount;
uniform float rayStep;
uniform float distanceBias;
uniform float depthRejectBias;
uniform float glossySampleCount;
uniform float adaptiveStepMultiplier;
uniform float noiseSine;

float epsilon = 0.1;

// <plus> ssr-skip-self (research/210 A2): your avatar's reflection in the floor
// (plus/avatarrefl, reflectionProbeF.glsl tapAvatarReflection) shows your
// avatar on its plane; the scene copy the rays hit holds your avatar too (the
// camera's side, a frame late), and both showed (research/210 D1). On the
// plane the rays' hits on your avatar (in its box) are dropped, by the
// reflection's own weight at the pixel. Elsewhere (walls, windows, other
// floors) the rays still show you; other avatars are never dropped.
#ifdef PLUS_AVATAR_REFL
uniform vec4 plusAvatarReflPlane;       // as reflectionProbeF.glsl's
uniform vec4 plusAvatarReflParams;      // as reflectionProbeF.glsl's: x the reflection's weight (0 off), y thickness, z least normal match
uniform vec4 plusAvatarReflSelf;        // x 1 where the program's pixels take the reflection (0: the water under a floor's)
uniform mat4 plusAvatarReflSelfBox;     // the last frame's eye space -> your avatar's box (inside: -1..1 on each axis)
vec3 plus_ssr_hit = vec3(0);            // where traceScreenRay hit (the last frame's eye space)

// How much of a hit on your avatar to drop at this pixel (0..1).
float plusSelfSkip(vec3 viewPos, vec3 n, float glossiness)
{
    float strength = plusAvatarReflParams.x * plusAvatarReflSelf.x;
    if (strength <= 0.0)
    {
        return 0.0;
    }
    float dist = dot(viewPos, plusAvatarReflPlane.xyz) + plusAvatarReflPlane.w;
    if (abs(dist) > plusAvatarReflParams.y || dot(n, plusAvatarReflPlane.xyz) < plusAvatarReflParams.z)
    {
        return 0.0;
    }
    return strength * clamp((glossiness - 0.7) * 5.0, 0.0, 1.0);
}

// A hit's share: 1, or 1 - skip inside your avatar's box.
float plusSelfKeep(float skip, vec3 hit)
{
    if (skip <= 0.0)
    {
        return 1.0;
    }
    vec3 b = (plusAvatarReflSelfBox * vec4(hit, 1.0)).xyz;
    return all(lessThanEqual(abs(b), vec3(1.0))) ? 1.0 - skip : 1.0;
}
#endif
// </plus>

float getLinearDepth(vec2 tc)
{
    float depth = texture(sceneDepth, tc).r;

    vec4 pos = getPositionWithDepth(tc, depth);

    return -pos.z;
}

bool traceScreenRay(vec3 position, vec3 reflection, out vec4 hitColor, out float hitDepth, float depth, sampler2D textureFrame)
{
    // transform position and reflection into same coordinate frame as the sceneMap and sceneDepth
    reflection += position;
    position = (inv_modelview_delta * vec4(position, 1)).xyz;
    reflection = (inv_modelview_delta * vec4(reflection, 1)).xyz;
    reflection -= position;

    depth = -position.z;

    vec3 step = rayStep * reflection;
    vec3 marchingPosition = position + step;
    float delta;
    float depthFromScreen;
    vec2 screenPosition;
    bool hit = false;
    hitColor = vec4(0);

    int i = 0;
    if (depth > depthRejectBias)
    {
        for (; i < iterationCount && !hit; i++)
        {
            screenPosition = generateProjectedPosition(marchingPosition);
            if (screenPosition.x > 1 || screenPosition.x < 0 ||
                screenPosition.y > 1 || screenPosition.y < 0)
            {
                hit = false;
                break;
            }
            depthFromScreen = getLinearDepth(screenPosition);
            delta = abs(marchingPosition.z) - depthFromScreen;

            if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
            {
                break;
            }

            if (abs(delta) < distanceBias)
            {
                vec4 color = vec4(1);
                if(debugDraw)
                    color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                hitColor = texture(sceneMap, screenPosition) * color;
                hitDepth = depthFromScreen;
                hit = true;
                // <plus> ssr-skip-self
#ifdef PLUS_AVATAR_REFL
                plus_ssr_hit = marchingPosition;
#endif
                // </plus>
                break;
            }
            if (isBinarySearchEnabled && delta > 0)
            {
                break;
            }
            if (isAdaptiveStepEnabled)
            {
                float directionSign = sign(abs(marchingPosition.z) - depthFromScreen);
                //this is sort of adapting step, should prevent lining reflection by doing sort of iterative converging
                //some implementation doing it by binary search, but I found this idea more cheaty and way easier to implement
                step = step * (1.0 - rayStep * max(directionSign, 0.0));
                marchingPosition += step * (-directionSign);
            }
            else
            {
                marchingPosition += step;
            }

            if (isExponentialStepEnabled)
            {
                step *= adaptiveStepMultiplier;
            }
        }
        if(isBinarySearchEnabled)
        {
            for(; i < iterationCount && !hit; i++)
            {
                step *= 0.5;
                marchingPosition = marchingPosition - step * sign(delta);

                screenPosition = generateProjectedPosition(marchingPosition);
                if (screenPosition.x > 1 || screenPosition.x < 0 ||
                    screenPosition.y > 1 || screenPosition.y < 0)
                {
                    hit = false;
                    break;
                }
                depthFromScreen = getLinearDepth(screenPosition);
                delta = abs(marchingPosition.z) - depthFromScreen;

                if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
                {
                    break;
                }

                if (abs(delta) < distanceBias && depthFromScreen != (depth - distanceBias))
                {
                    vec4 color = vec4(1);
                    if(debugDraw)
                        color = vec4( 0.5+ sign(delta)/2,0.3,0.5- sign(delta)/2, 0);
                    hitColor = texture(sceneMap, screenPosition) * color;
                    hitDepth = depthFromScreen;
                    hit = true;
                    // <plus> ssr-skip-self
#ifdef PLUS_AVATAR_REFL
                    plus_ssr_hit = marchingPosition;
#endif
                    // </plus>
                    break;
                }
            }
        }
    }

    return hit;
}

// <plus> fix-round102: const, not uniform. Vulkan GLSL drops a uniform's
// initializer, the array became a zero-filled member of the default uniform block
// (nothing in C++ sets it), and getPoissonSample() returned (-1,-1,-1) for every
// sample: glossy SSR taps all leaned one way and cross() with a parallel ray gave
// normalize(0) = NaN. GL honoured the initializer; const gives the same values.
// (was: uniform vec3 POISSON3D_SAMPLES[128] = vec3[128]( )
const vec3 POISSON3D_SAMPLES[128] = vec3[128](
// </plus>
    vec3(0.5433144, 0.1122154, 0.2501391),
    vec3(0.6575254, 0.721409, 0.16286),
    vec3(0.02888453, 0.05170321, 0.7573566),
    vec3(0.06635678, 0.8286457, 0.07157445),
    vec3(0.8957489, 0.4005505, 0.7916042),
    vec3(0.3423355, 0.5053263, 0.9193521),
    vec3(0.9694794, 0.9461077, 0.5406441),
    vec3(0.9975473, 0.02789414, 0.7320132),
    vec3(0.07781899, 0.3862341, 0.918594),
    vec3(0.4439073, 0.9686955, 0.4055861),
    vec3(0.9657035, 0.6624081, 0.7082613),
    vec3(0.7712346, 0.07273269, 0.3292839),
    vec3(0.2489169, 0.2550394, 0.1950516),
    vec3(0.7249326, 0.9328285, 0.3352458),
    vec3(0.6028461, 0.4424961, 0.5393377),
    vec3(0.2879795, 0.7427881, 0.6619173),
    vec3(0.3193627, 0.0486145, 0.08109283),
    vec3(0.1233155, 0.602641, 0.4378719),
    vec3(0.9800708, 0.211729, 0.6771586),
    vec3(0.4894537, 0.3319927, 0.8087631),
    vec3(0.4802743, 0.6358885, 0.814935),
    vec3(0.2692913, 0.9911493, 0.9934899),
    vec3(0.5648789, 0.8553897, 0.7784553),
    vec3(0.8497344, 0.7870212, 0.02065313),
    vec3(0.7503014, 0.2826185, 0.05412734),
    vec3(0.8045461, 0.6167251, 0.9532926),
    vec3(0.04225039, 0.2141281, 0.8678675),
    vec3(0.07116079, 0.9971236, 0.3396397),
    vec3(0.464099, 0.480959, 0.2775862),
    vec3(0.6346927, 0.31871, 0.6588384),
    vec3(0.449012, 0.8189669, 0.2736875),
    vec3(0.452929, 0.2119148, 0.672004),
    vec3(0.01506042, 0.7102436, 0.9800494),
    vec3(0.1970513, 0.4713539, 0.4644522),
    vec3(0.13715, 0.7253224, 0.5056525),
    vec3(0.9006432, 0.5335414, 0.02206874),
    vec3(0.9960898, 0.7961011, 0.01468861),
    vec3(0.3386469, 0.6337739, 0.9310676),
    vec3(0.1745718, 0.9114985, 0.1728188),
    vec3(0.6342545, 0.5721557, 0.4553517),
    vec3(0.1347412, 0.1137158, 0.7793725),
    vec3(0.3574478, 0.3448052, 0.08741581),
    vec3(0.7283059, 0.4753885, 0.2240275),
    vec3(0.8293507, 0.9971212, 0.2747005),
    vec3(0.6501846, 0.000688076, 0.7795712),
    vec3(0.01149416, 0.4930083, 0.792608),
    vec3(0.666189, 0.1875442, 0.7256873),
    vec3(0.8538797, 0.2107637, 0.1547532),
    vec3(0.5826825, 0.9750752, 0.9105834),
    vec3(0.8914346, 0.08266425, 0.5484225),
    vec3(0.4374518, 0.02987111, 0.7810078),
    vec3(0.2287418, 0.1443802, 0.1176908),
    vec3(0.2671157, 0.8929081, 0.8989366),
    vec3(0.5425819, 0.5524959, 0.6963879),
    vec3(0.3515188, 0.8304397, 0.0502702),
    vec3(0.3354864, 0.2130747, 0.141169),
    vec3(0.9729427, 0.3509927, 0.6098799),
    vec3(0.7585629, 0.7115368, 0.9099342),
    vec3(0.0140543, 0.6072157, 0.9436461),
    vec3(0.9190664, 0.8497264, 0.1643751),
    vec3(0.1538157, 0.3219983, 0.2984214),
    vec3(0.8854713, 0.2968667, 0.8511457),
    vec3(0.1910622, 0.03047311, 0.3571215),
    vec3(0.2456353, 0.5568692, 0.3530164),
    vec3(0.6927255, 0.8073994, 0.5808484),
    vec3(0.8089353, 0.8969175, 0.3427134),
    vec3(0.194477, 0.7985603, 0.8712182),
    vec3(0.7256182, 0.5653068, 0.3985921),
    vec3(0.9889427, 0.4584851, 0.8363391),
    vec3(0.5718582, 0.2127113, 0.2950557),
    vec3(0.5480209, 0.0193435, 0.2992659),
    vec3(0.6598953, 0.09478426, 0.92187),
    vec3(0.1385615, 0.2193868, 0.205245),
    vec3(0.7623423, 0.1790726, 0.1508465),
    vec3(0.7569032, 0.3773386, 0.4393887),
    vec3(0.5842971, 0.6538072, 0.5224424),
    vec3(0.9954313, 0.5763943, 0.9169143),
    vec3(0.001311183, 0.340363, 0.1488652),
    vec3(0.8167927, 0.4947158, 0.4454727),
    vec3(0.3978434, 0.7106082, 0.002727509),
    vec3(0.5459411, 0.7473233, 0.7062873),
    vec3(0.4151598, 0.5614617, 0.4748358),
    vec3(0.4440694, 0.1195122, 0.9624678),
    vec3(0.1081301, 0.4813806, 0.07047641),
    vec3(0.2402785, 0.3633997, 0.3898734),
    vec3(0.2317942, 0.6488295, 0.4221864),
    vec3(0.01145542, 0.9304277, 0.4105759),
    vec3(0.3563728, 0.9228861, 0.3282344),
    vec3(0.855314, 0.6949819, 0.3175117),
    vec3(0.730832, 0.01478493, 0.5728671),
    vec3(0.9304829, 0.02653277, 0.712552),
    vec3(0.4132186, 0.4127623, 0.6084146),
    vec3(0.7517329, 0.9978395, 0.1330464),
    vec3(0.5210338, 0.4318751, 0.9721575),
    vec3(0.02953994, 0.1375937, 0.9458942),
    vec3(0.1835506, 0.9896691, 0.7919457),
    vec3(0.3857062, 0.2682322, 0.1264563),
    vec3(0.6319699, 0.8735335, 0.04390657),
    vec3(0.5630485, 0.3339024, 0.993995),
    vec3(0.90701, 0.1512893, 0.8970422),
    vec3(0.3027443, 0.1144253, 0.1488708),
    vec3(0.9149003, 0.7382028, 0.7914025),
    vec3(0.07979286, 0.6892691, 0.2866171),
    vec3(0.7743186, 0.8046008, 0.4399814),
    vec3(0.3128662, 0.4362317, 0.6030678),
    vec3(0.1133721, 0.01605821, 0.391872),
    vec3(0.5185481, 0.9210006, 0.7889017),
    vec3(0.8217013, 0.325305, 0.1668191),
    vec3(0.8358996, 0.1449739, 0.3668382),
    vec3(0.1778213, 0.5599256, 0.1327691),
    vec3(0.06690693, 0.5508637, 0.07212365),
    vec3(0.9750564, 0.284066, 0.5727578),
    vec3(0.4350255, 0.8949825, 0.03574753),
    vec3(0.8931149, 0.9177974, 0.8123496),
    vec3(0.9055127, 0.989903, 0.813235),
    vec3(0.2897243, 0.3123978, 0.5083504),
    vec3(0.1519223, 0.3958645, 0.2640327),
    vec3(0.6840154, 0.6463035, 0.2346607),
    vec3(0.986473, 0.8714055, 0.3960275),
    vec3(0.6819352, 0.4169535, 0.8379834),
    vec3(0.9147297, 0.6144146, 0.7313942),
    vec3(0.6554981, 0.5014008, 0.9748477),
    vec3(0.9805915, 0.1318207, 0.2371372),
    vec3(0.5980836, 0.06796348, 0.9941338),
    vec3(0.6836596, 0.9917196, 0.2319056),
    vec3(0.5276511, 0.2745509, 0.5422578),
    vec3(0.829482, 0.03758276, 0.1240466),
    vec3(0.2698198, 0.0002266169, 0.3449324)
);

vec3 getPoissonSample(int i) {
    return POISSON3D_SAMPLES[i] * 2 - 1;
}

float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness)
{
#ifdef TRANSPARENT_SURFACE
collectedColor = vec4(1, 0, 1, 1);
    return 0.0;
#endif
    collectedColor = vec4(0);
    int hits = 0;

    float depth = -viewPos.z;

    vec3 rayDirection = normalize(reflect(viewPos, normalize(n)));

    vec2 uv2 = tc * screen_res;
    float c = (uv2.x + uv2.y) * 0.125;
    float jitter = mod( c, 1.0);

    vec2 screenpos = 1 - abs(tc * 2 - 1);
    float vignette = clamp((abs(screenpos.x) * abs(screenpos.y)) * 16,0, 1);
    vignette *= clamp((dot(normalize(viewPos), n) * 0.5 + 0.5) * 5.5 - 0.8, 0, 1);

    float zFar = 128.0;
    vignette *= clamp(1.0+(viewPos.z/zFar), 0.0, 1.0);

    vignette *= clamp(glossiness * 3 - 1.7, 0, 1);

    vec4 hitpoint;

    // <plus> ssr-skip-self: the hits' shares (the hits' count while none is dropped)
    float plus_hit_w = 0.0;
#ifdef PLUS_AVATAR_REFL
    float plus_skip = plusSelfSkip(viewPos, normalize(n), glossiness);
#endif
    // </plus>

    glossiness = 1 - glossiness;

    totalSamples = int(max(glossySampleCount, glossySampleCount * glossiness * vignette));

    totalSamples = max(totalSamples, 1);
    if (glossiness < 0.35)
    {
        if (vignette > 0)
        {
            for (int i = 0; i < totalSamples; i++)
            {
                vec3 firstBasis = normalize(cross(getPoissonSample(i), rayDirection));
                vec3 secondBasis = normalize(cross(rayDirection, firstBasis));
                vec2 coeffs = vec2(random(tc + vec2(0, i)) + random(tc + vec2(i, 0)));
                vec3 reflectionDirectionRandomized = rayDirection + ((firstBasis * coeffs.x + secondBasis * coeffs.y) * glossiness);

                //float hitDepth;

                bool hit = traceScreenRay(viewPos, normalize(reflectionDirectionRandomized), hitpoint, depth, depth, source);

                hitpoint.a = 0;

                if (hit)
                {
                    // <plus> ssr-skip-self: less of a hit on your avatar where your reflection shows you
                    float plus_keep = 1.0;
#ifdef PLUS_AVATAR_REFL
                    plus_keep = plusSelfKeep(plus_skip, plus_ssr_hit);
#endif
                    plus_hit_w += plus_keep;
                    hitpoint *= plus_keep;
                    // </plus>
                    ++hits;
                    collectedColor += hitpoint;
                    collectedColor.a += 1;
                }
            }

            if (hits > 0 && plus_hit_w > 0.0)   // <plus> ssr-skip-self: && plus_hit_w > 0.0 </plus>
            {
                collectedColor /= plus_hit_w;   // <plus> ssr-skip-self: was hits </plus>
            }
            else
            {
                collectedColor = vec4(0);
            }
        }
    }
    float hitAlpha = plus_hit_w;    // <plus> ssr-skip-self: was hits </plus>
    hitAlpha /= totalSamples;
    collectedColor.a = hitAlpha * vignette;
    return hits;
}

// <plus> fix-r11 (research/water-noise-2026-10-27, PlusSSRDenoise): the water's
// screen space reflection without the grain that comes while the camera moves.
// tapScreenSpaceReflection above, on the water: each pixel's rays leaned by a
// hash of tc (the waves' distorted screen position, so it changes every frame),
// rays into the water hit the water itself (its last frame's
// colour, the sun's glints, seen from the last frame's eye), and a hit at the
// screen's edge (the last frame's, which moves) came and went at once. Here:
// the same number of rays on a fixed ring around the mirror direction (no
// noise, nothing that changes with time), the same vignette and mean spread,
// rays into the water and hits on the water dropped (the probes and the water
// mirror stay there), hits faded near the screen's edge, and the hits averaged
// by 1 / (1 + luma) so a single bright glint does not stand out. Same tracing,
// same cost (fewer rays traced).

// traceScreenRay as it is; also gives where the hit is on the screen and the
// position seen there (the last frame's camera space).
bool plusTraceWaterRay(vec3 position, vec3 reflection, out vec4 hitColor, out vec2 hitScreen, out vec3 hitSurface)
{
    reflection += position;
    position = (inv_modelview_delta * vec4(position, 1)).xyz;
    reflection = (inv_modelview_delta * vec4(reflection, 1)).xyz;
    reflection -= position;

    float depth = -position.z;

    vec3 step = rayStep * reflection;
    vec3 marchingPosition = position + step;
    float delta = 0.0;
    float depthFromScreen = 0.0;
    vec2 screenPosition = vec2(0.0);
    bool hit = false;
    hitColor = vec4(0);
    hitScreen = vec2(0);
    hitSurface = vec3(0);

    int i = 0;
    if (depth > depthRejectBias)
    {
        for (; i < iterationCount && !hit; i++)
        {
            screenPosition = generateProjectedPosition(marchingPosition);
            if (screenPosition.x > 1 || screenPosition.x < 0 ||
                screenPosition.y > 1 || screenPosition.y < 0)
            {
                break;
            }
            depthFromScreen = getLinearDepth(screenPosition);
            delta = abs(marchingPosition.z) - depthFromScreen;

            if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
            {
                break;
            }

            if (abs(delta) < distanceBias)
            {
                hit = true;
                break;
            }
            if (delta > 0)
            {
                break;  // isBinarySearchEnabled
            }
            // isAdaptiveStepEnabled, isExponentialStepEnabled
            float directionSign = sign(abs(marchingPosition.z) - depthFromScreen);
            step = step * (1.0 - rayStep * max(directionSign, 0.0));
            marchingPosition += step * (-directionSign);
            step *= adaptiveStepMultiplier;
        }
        for (; i < iterationCount && !hit; i++)
        {
            step *= 0.5;
            marchingPosition = marchingPosition - step * sign(delta);

            screenPosition = generateProjectedPosition(marchingPosition);
            if (screenPosition.x > 1 || screenPosition.x < 0 ||
                screenPosition.y > 1 || screenPosition.y < 0)
            {
                break;
            }
            depthFromScreen = getLinearDepth(screenPosition);
            delta = abs(marchingPosition.z) - depthFromScreen;

            if (depth < depthFromScreen + epsilon && depth > depthFromScreen - epsilon)
            {
                break;
            }

            if (abs(delta) < distanceBias && depthFromScreen != (depth - distanceBias))
            {
                hit = true;
                break;
            }
        }
    }

    if (hit)
    {
        hitColor = texture(sceneMap, screenPosition);
        hitScreen = screenPosition;
        hitSurface = getPositionWithDepth(screenPosition, texture(sceneDepth, screenPosition).r).xyz;
    }
    return hit;
}

// tapScreenSpaceReflection's water case (glossiness as given there; up: the
// water's up in this frame's eye space, or 0 when not known). The return and
// collectedColor as tapScreenSpaceReflection's (a: the share of the rays that
// hit, times the vignette).
float plusTapWaterSSR(vec2 tc, vec3 viewPos, vec3 n, vec3 up, inout vec4 collectedColor, float glossiness)
{
    collectedColor = vec4(0);

    vec3 rayDirection = normalize(reflect(viewPos, normalize(n)));

    // The vignette exactly as tapScreenSpaceReflection's.
    vec2 screenpos = 1 - abs(tc * 2 - 1);
    float vignette = clamp((abs(screenpos.x) * abs(screenpos.y)) * 16, 0, 1);
    vignette *= clamp((dot(normalize(viewPos), n) * 0.5 + 0.5) * 5.5 - 0.8, 0, 1);
    float zFar = 128.0;
    vignette *= clamp(1.0 + (viewPos.z / zFar), 0.0, 1.0);
    vignette *= clamp(glossiness * 3 - 1.7, 0, 1);

    float rough = 1.0 - glossiness;
    if (rough >= 0.35 || vignette <= 0.0)
    {
        return 0.0;
    }
#ifdef PLUS_AVATAR_REFL
    float self_skip = plusSelfSkip(viewPos, normalize(n), glossiness);  // ssr-skip-self
#endif

    bool has_up = dot(up, up) > 0.25;
    vec3 up_eye = has_up ? normalize(up) : vec3(0, 0, 1);
    vec3 up_last = normalize(mat3(inv_modelview_delta) * up_eye);   // the ray is traced in the last frame's camera space
    vec3 start_last = (inv_modelview_delta * vec4(viewPos, 1)).xyz;

    // The ring: around the mirror direction, the same for every pixel and
    // frame. Its radius is the mean lean of the official rays (|b1 + b2| = 1.41
    // times 0 to 2 times the roughness); the basis turns smoothly with the ray
    // (the screen's x is never along a reflection off the water).
    vec3 side = cross(rayDirection, vec3(1, 0, 0));
    if (dot(side, side) < 1e-4)
    {
        side = cross(rayDirection, vec3(0, 1, 0));
    }
    vec3 b1 = normalize(side);
    vec3 b2 = cross(b1, rayDirection);
    int count = clamp(int(glossySampleCount), 1, 16);
    float radius = count > 1 ? 1.41421356 * rough : 0.0;

    vec3 sum = vec3(0);
    float sum_w = 0.0;
    float cover = 0.0;
    for (int i = 0; i < count; i++)
    {
        float a = (float(i) + 0.5) * (6.2831853 / float(count));
        vec3 dir = normalize(rayDirection + (b1 * cos(a) + b2 * sin(a)) * radius);
        if (has_up && dot(dir, up_eye) <= 0.0)
        {
            continue;   // into the water: it would only find the water
        }

        vec4 hit_col;
        vec2 hit_screen;
        vec3 hit_surface;
        if (!plusTraceWaterRay(viewPos, dir, hit_col, hit_screen, hit_surface))
        {
            continue;
        }
        if (has_up && dot(hit_surface - start_last, up_last) < 0.05 + 0.002 * abs(hit_surface.z))
        {
            continue;   // the water itself (or something at its level): the last frame's water, glints and all
        }

        vec2 edge = min(hit_screen, 1.0 - hit_screen);
        float e = clamp(min(edge.x, edge.y) * 10.0, 0.0, 1.0);    // fades over the last 10 % to the screen's edge
#ifdef PLUS_AVATAR_REFL
        e *= plusSelfKeep(self_skip, hit_surface);  // ssr-skip-self: your avatar, where your reflection shows you
#endif
        if (e <= 0.0)
        {
            continue;
        }
        float luma = dot(hit_col.rgb, vec3(0.2126, 0.7152, 0.0722));
        float w = e / (1.0 + max(luma, 0.0));
        sum += hit_col.rgb * w;
        sum_w += w;
        cover += e;
    }

    if (sum_w > 0.0)
    {
        collectedColor.rgb = sum / sum_w;
    }
    collectedColor.a = cover / float(count) * vignette;
    return cover;
}
// </plus>
