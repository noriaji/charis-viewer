/**
 * @file class1\deferred\cloudsF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
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
/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[4];

/////////////////////////////////////////////////////////////////////////
// The fragment shader for the sky
/////////////////////////////////////////////////////////////////////////

// In
in vec3 pos;

in vec2 vary_texcoord0;
in vec2 vary_texcoord1;
in vec2 vary_texcoord2;
in vec2 vary_texcoord3;

uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;
uniform float blend_factor;
uniform vec3 cloud_pos_density1;
uniform vec3 cloud_pos_density2;
uniform float cloud_scale;
uniform float cloud_variance;

uniform vec3 camPosLocal;

uniform vec3 lightnorm;
uniform vec3 sunlight_color;
uniform vec3 moonlight_color;
uniform int sun_up_factor;
uniform vec3 ambient_color;
uniform vec3 blue_horizon;
uniform vec3 blue_density;
uniform float haze_horizon;
uniform float haze_density;

uniform float cloud_shadow;
uniform float density_multiplier;
uniform float max_y;

uniform vec3 glow;
uniform float sun_moon_glow_factor;

uniform vec3 cloud_color;

// <plus> plus/nightclouds (research/176 section 10): under a classic sky that the auto-adjust
// (RenderSkyAutoAdjustLegacy) puts through the tone mapping, the clouds at night are lifted
// back towards the brightness that sky shows them at without it (the ACES curve crushes the
// dark grey to black). Only the clouds' pixels change. 0: as official (by day, at sunset,
// under an HDR sky, with the auto-adjust off, PlusNightCloudLift 0).
uniform float plus_cloud_night;
// plus/skyscreen: what the scene does to the sky's colour before the screen (skyscreen.h)
uniform float plus_sky_exposure;    // RenderExposure, or 0 under "no post": no exposure, no curve
uniform float plus_sky_hdr_scale;   // sky_hdr_scale
uniform float plus_sky_tonemap_mix; // the curve's share
uniform float plus_sky_aces;        // 1: the curve is ACES Hill
uniform sampler2D exposureMap;

// plus/skyscreen (a copy of starsF.glsl's: keep the two in step): the linear light to write
// for the linear brightness d (0 .. 0.95) to come out on the screen.
float plusSkyLight(float d)
{
    float x = d;
    if (plus_sky_exposure > 0.0)
    {
        float m = plus_sky_tonemap_mix * plus_sky_aces;
        if (m > 0.0)
        {
            // the start: ACES alone (at or above the root for the mixed curve)
            float qa = 1.0 - 0.983729 * d;     // > 0: d <= 0.95
            float qb = 0.0245786 - 0.4329510 * d;
            float qc = 0.000090537 + 0.238081 * d;
            x = (-qb + sqrt(qb * qb + 4.0 * qa * qc)) / (2.0 * qa);
            for (int i = 0; i < 4; ++i)
            {
                float na = x * (x + 0.0245786) - 0.000090537;
                float nb = x * (0.983729 * x + 0.4329510) + 0.238081;
                float f = (1.0 - m) * x + m * na / nb - d;
                float slope = (1.0 - m) + m * ((2.0 * x + 0.0245786) * nb - na * (2.0 * 0.983729 * x + 0.4329510)) / (nb * nb);
                x = max(x - f / max(slope, 1e-3), 0.0);
            }
        }
        x /= max(plus_sky_exposure * texture(exposureMap, vec2(0.5, 0.5)).r, 1e-4);
    }
    return x / max(plus_sky_hdr_scale, 1e-3);
}

vec3 plusToLinear(vec3 c)
{
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, vec3(lessThan(c, vec3(0.04045))));
}

// linear light to sRGB for light that may be above 1 (skyRainbowF.glsl, linked into the clouds' programs)
vec3 plusToSrgb(vec3 c);

// plus/rainbow (research/255): the bow, as the sky's (skyRainbowF.glsl)
vec3 plusRainbowOver(vec3 shown, vec3 view_dir);
// </plus>

vec4 cloudNoise(vec2 uv)
{
   vec4 a = texture(cloud_noise_texture, uv);
   vec4 b = texture(cloud_noise_texture_next, uv);
   vec4 cloud_noise_sample = mix(a, b, blend_factor);
   return cloud_noise_sample;
}

#if defined(VOLUMETRIC)
// <plus> plus/volclouds (research/186 chapter 16, research/244): the layer is a slab at the world
// altitude [max_y, max_y + thickness], marched along the dome pixel's view ray. Its colours, haze,
// night lift and G-buffer output are the 2D clouds' own; only the shape (alpha1) and the sun's way
// through it (alpha2 in 2D) are worked out along the ray.
// Where the clouds are and how much (vol-clouds-fix2): each place on the layer takes the 2D clouds'
// uv of its direction (plusCloudPlace), and each column lets through what the 2D clouds do there, so the
// sky has the 2D clouds' cover; the layer adds a height (a dome over the field's peaks) and a top
// brighter than its base (the base is lit as 2D, vol-clouds-fix3). The density functions are copied in cloudShadowMapF.glsl: keep the two in step.
// The light (vol-clouds-fix3): each step is lit as the 2D clouds light that place (their colours,
// haze and self shadow alpha2, with its texture), the sun's share raised towards a cloud's top
// (plus_cloud_shape.w), never above 2D's sun + ambient.
uniform vec4 plus_cloud_params;     // x: thickness (m), y: steps, z: the fine pattern's share in the tops (PlusCloudGrain), w: the layer's height the 2D clouds stand for (the sky's max_y, m)
uniform vec4 plus_cloud_shape;      // x: the camera's height in the 2D dome, a share of its radius; y: the relief of the tops (PlusCloudDetail); z: the dome's radius; w: the shading (PlusCloudShade)
uniform float plus_cloud_alt;       // the camera's altitude (m)

// The noise image's first channel at a mip level (below 0: the image itself, so a footprint's level
// can be taken before a pattern's frequency is added to it); the second image only while the sky is
// blending to it.
float plusCloudNoise(vec2 uv, float lod)
{
    // textureLod: the march and the cover branch per pixel, where a quad's derivatives would be undefined
    lod = max(lod, 0.0);
    float a = textureLod(cloud_noise_texture, uv, lod).x;
    if (blend_factor <= 0.0)
    {
        return a;
    }
    return mix(a, textureLod(cloud_noise_texture_next, uv, lod).x, blend_factor);
}

// The 2D clouds' uv of a place on the layer (xy) and their altitude blend in its direction (z). h: its
// offset from the camera on the ground's plane in the dome's axes (x the north, z the east; m); href:
// the layer's height the 2D clouds stand for (the sky's max_y). The 2D clouds give a direction the uv
// of the dome where it meets it, seen from the camera at plus_cloud_shape.x of the dome's radius above
// its centre (cloudsV.glsl, llvowlsky.cpp); the place gets the uv of its own direction at href, so the
// clouds stand where the 2D clouds are, as large and as many (the whole sky is a third of the image
// under the default sky). The blend is 2D's altitude_blend_factor, clamp((rel_pos.y + 512) / max_y),
// with rel_pos.y the dome point's height over the camera + 50 (plus_cloud_shape.z: the dome's radius
// in its own units): 2D's clouds are that much fainter, more so near the horizon and under a high max_y.
vec3 plusCloudPlace(vec2 h, float href)
{
    float c = plus_cloud_shape.x;
    float inv = inversesqrt(dot(h, h) + href * href);
    float se = href * inv;                                      // the sine of the place's elevation
    float s = sqrt(c * c * se * se + 1.0 - c * c) - c * se;     // the way to the dome (its radius 1)
    vec2 dome = h * (s * inv);                                  // where it meets the dome (x, z)
    float scale = max(cloud_scale, 0.001);                      // (main discards below it)
    float blend = clamp((s * plus_cloud_shape.z * se + 562.0) / href, 0.0, 1.0);
    return vec3(vec2(0.5 - 1.0 / scale, 0.5) + vec2(dome.y, -dome.x) / (2.0 * scale), blend);
}

// The 2D clouds' disturbance at uv (cloud_variance; lod: the mip level of a footprint at one period
// per uv, below 0 for a footprint under a texel): xy moves the shape, z is what is left of the cover's density. 2D takes it per pixel at its
// uv2/uv4, offset towards the sun by 0.0125 (left out: a hundredth of the pattern).
vec3 plusCloudDisturb(vec2 uv, float lod)
{
    if (cloud_variance == 0.0)
    {
        return vec3(0.0, 0.0, 1.0);
    }
    float k = cloud_variance * (1.0 - cloud_scale * 0.25);
    vec2 uv3 = uv * 16.0;
    vec2 d1 = vec2(plusCloudNoise(uv / 8.0, lod - 3.0), plusCloudNoise((uv3 + uv) / 16.0, lod)) * k;
    vec2 d2 = vec2(plusCloudNoise((uv + uv3) / 4.0, lod + 2.0), plusCloudNoise((uv3 + uv) / 8.0, lod + 1.0)) * k;
    float dv = min(1.0, (d1.x * 2.0 + d1.y * 2.0 + d2.x + d2.y) * 4.0);
    return vec3(d1 * 0.2, 1.0 - dv * dv);
}

// The cover field's large shape at uv, without the fine pattern (cloud_density: 2 * (cloud_shadow -
// 0.25); disturb: plusCloudDisturb's).
float plusCloudBase(vec2 uv, vec3 disturb, float cloud_density, float lod_shape)
{
    return plusCloudNoise(uv + cloud_pos_density1.xy + disturb.xy, lod_shape) - 0.5 + cloud_density * disturb.z;
}

// The cover field at uv: the large shape and the fine pattern, the 2D clouds' alpha1 before its gain
// and its clamp (> 0: cloud).
float plusCloudAddDetail(vec2 uv, float base, float lod_detail)
{
    if (base + 0.5 * abs(cloud_pos_density2.z) <= 0.0)
    {
        return base;    // no cloud however the fine pattern falls: its tap is not needed
    }
    return base + (plusCloudNoise(uv * 16.0 + cloud_pos_density2.xy, lod_detail) - 0.5) * cloud_pos_density2.z;
}

float plusCloudField(vec2 uv, vec3 disturb, float cloud_density, float lod_shape, float lod_detail)
{
    return plusCloudAddDetail(uv, plusCloudBase(uv, disturb, cloud_density, lod_shape), lod_detail);
}

// The field at which a cloud reaches the top of the layer (the noise's top is about 0.5 above its middle)
const float PLUS_CLOUD_FIELD_FULL = 0.5;

// The top of a column as a share of the layer's thickness for a field: a dome over the field's peaks
// (f (2 - f) of the field's share of PLUS_CLOUD_FIELD_FULL: round on top, sloping at the rims, a flat base).
float plusCloudHeight(float field)
{
    float f = clamp(field / PLUS_CLOUD_FIELD_FULL, 0.0, 1.0);
    return f * (2.0 - f);
}

// A column of the cover field (field > 0; blend: plusCloudPlace's z there): x its optical depth
// straight up, so that it lets through what the 2D clouds do (1 - exp(-x) = 0.98 of their alpha1, with
// its gain, smoothing and altitude blend); y its top (plusCloudHeight).
vec2 plusCloudColumn(float field, float blend)
{
    float x = clamp(field * 10.0 * cloud_pos_density1.z, 0.0, 1.0);
    float s = 1.0 - x * x;
    float alpha = (1.0 - s * s) * blend;
    return vec2(-log(1.0 - 0.98 * alpha), plusCloudHeight(field));
}

// The optical depth of a column (tau in all, its top top > 0 metres up) from the layer's base up to z
// metres: its density is even, thinning over the top's last fifth to 0 at the top.
float plusCloudDepth(float z, float tau, float top)
{
    float u = clamp(z, 0.0, top);
    float w = max(u - 0.8 * top, 0.0);     // into the thinning fifth
    return tau / (0.9 * top) * (u - w * w / (0.4 * top));
}

// The 2D clouds' share of the sun at uv (1 - their alpha2, the self shadow): the shape 0.0125 uv
// towards the sun (cloudsV.glsl's vary_texcoord1), gain 2.5 (cloud_density: with the disturbance).
float plusCloudSunShare(vec2 uv, float cloud_density, float lod)
{
    float a2 = plusCloudNoise(uv + vec2(lightnorm.x, lightnorm.z) * 0.0125 + cloud_pos_density1.xy, lod) - 0.5;
    a2 = clamp((a2 + cloud_density) * 2.5 * cloud_pos_density1.z, 0.0, 1.0);
    return (1.0 - a2) * (1.0 - a2);
}

// The share of the sun at hrel of a cloud's height (0 its base, 1 its top), for the 2D clouds' sun
// share there: 2D's at the base (seen from below, the base is most of what shows), rising to 1.5
// times at the top at shade 1 (PlusCloudShade); at most 1, so never brighter than 2D's sun + ambient
// (the ambient light, and the haze in it, stay 2D's).
float plusCloudSunLight(float sun_share, float hrel, float shade)
{
    return min(sun_share * (1.0 + shade * 0.5 * hrel), 1.0);
}
// </plus>
#endif

void main()
{
#if defined(VOLUMETRIC) // <plus> plus/volclouds: how far the 2D uv moves to the next pixel, taken before any branch (derivatives)
    vec3 pix_dir = normalize(pos.xyz - camPosLocal.xyz);
    vec2 pix_uv_at = plusCloudPlace(pix_dir.xz * (plus_cloud_params.w / max(pix_dir.y, 0.005)), plus_cloud_params.w).xy;
    // the finer of the two screen axes, at most 4 to 1 (the 2D clouds' anisotropic filtering takes up to
    // 16, but textureLod is isotropic: a finer level would skip texels along the other axis and shimmer)
    float pix_uv_x = length(dFdx(pix_uv_at));
    float pix_uv_y = length(dFdy(pix_uv_at));
    float pix_uv = max(min(pix_uv_x, pix_uv_y), max(pix_uv_x, pix_uv_y) / 4.0);
#endif // </plus>
    if (cloud_scale < 0.001)
    {
        discard;
    }

    // Set variables
    vec2 uv1 = vary_texcoord0.xy;
    vec2 uv2 = vary_texcoord1.xy;
    vec2 uv3 = vary_texcoord2.xy;
    vec2 uv4 = vary_texcoord3.xy;

    // Get relative position
    vec3 rel_pos = pos.xyz - camPosLocal.xyz + vec3(0, 50, 0);

    float altitude_blend_factor = clamp((rel_pos.y + 512.0) / max_y, 0.0, 1.0);

#if defined(VOLUMETRIC)
    // <plus> plus/volclouds (research/186 16.2): where the view ray crosses the slab
    vec3 vdir = pix_dir;    // pos - camPosLocal without the +50 of rel_pos: a world-fixed layer
    float slab_thick = plus_cloud_params.x;
    float slab_lo = max(max_y, 100.0);   // the CPU's floor
    float slab_hi = slab_lo + slab_thick;
    if (vdir.y < 0.005 || plus_cloud_alt >= slab_hi)
    {
        discard;    // not towards the slab, or the camera above it (the dome is the upper half)
    }
    // no cap on the distance: the uv of plusCloudPlace stays within the 2D clouds' (a third of the
    // image), and the haze below fades the far clouds as it does the 2D ones
    float t_lo = (max(slab_lo, plus_cloud_alt) - plus_cloud_alt) / vdir.y;
    float t_hi = (slab_hi - plus_cloud_alt) / vdir.y;
    if (t_lo >= t_hi)
    {
        discard;
    }
    // the haze and colours below are the 2D clouds' own for this pixel: rel_pos stretched to max_y as
    // 2D does (rel_pos.y > 0: vdir.y >= 0.005 above); altitude_blend_factor: each column takes 2D's in
    // its own direction (plusCloudPlace)
    rel_pos *= (max(max_y, 1.0) / rel_pos.y);   // (a max_y of 0 would make it 0: NaN in normalize)
    // </plus>
#else // <plus> VOLUMETRIC </plus>
    // Set altitude
    if (rel_pos.y > 0)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0)
    {
        altitude_blend_factor = 0; // SL-11589 Fix clouds drooping below horizon
        rel_pos *= (-32000. / rel_pos.y);
    }
#endif // <plus> VOLUMETRIC </plus>

    // Can normalize then
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    // Initialize temp variables
    vec3 sunlight = sunlight_color;
    vec3 light_atten;

    // Sunlight attenuation effect (hue and brightness) due to atmosphere
    // this is used later for sunlight modulation at various altitudes
    light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

    // Calculate relative weights
    vec3 combined_haze = abs(blue_density) + vec3(abs(haze_density));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    // Distance
    float density_dist = rel_pos_len * density_multiplier;

    // Transparency (-> combined_haze)
    // ATI Bugfix -- can't store combined_haze*density_dist in a variable because the ati
    // compiler gets confused.
    combined_haze = exp(-combined_haze * density_dist);

    // Compute haze glow
    float haze_glow = 1.0 - dot(rel_pos_norm, lightnorm.xyz);
    // haze_glow is 0 at the sun and increases away from sun
    haze_glow = max(haze_glow, .001);
    // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
    haze_glow *= glow.x;
    // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
    haze_glow = pow(haze_glow, glow.z);
    // glow.z should be negative, so we're doing a sort of (1 / "angle") function

    haze_glow *= sun_moon_glow_factor;

    // Add "minimum anti-solar illumination"
    // For sun, add to glow.  For moon, remove glow entirely. SL-13768
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (haze_glow + 0.25);

    // Increase ambient when there are more clouds
    vec3 tmpAmbient = ambient_color;
    tmpAmbient += (1. - tmpAmbient) * cloud_shadow * 0.5;

    // Dim sunlight by cloud shadow percentage
    sunlight *= (1. - cloud_shadow);

    // Haze color below cloud
    vec3 additiveColorBelowCloud =
        (blue_horizon * blue_weight * (sunlight + tmpAmbient) + (haze_horizon * haze_weight) * (sunlight * haze_glow + tmpAmbient));

    // CLOUDS
    sunlight = sunlight_color;
    off_axis = 1.0 / max(1e-6, lightnorm.y * 2.);
    sunlight *= exp(-light_atten * off_axis);

    // Cloud color out
    vec3 cloudColorSun = (sunlight * haze_glow) * cloud_color;
    vec3 cloudColorAmbient = tmpAmbient * cloud_color;

    // Attenuate cloud color by atmosphere
    combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds
    cloudColorSun *= combined_haze;
    cloudColorAmbient *= combined_haze;
    vec3 oHazeColorBelowCloud = additiveColorBelowCloud * (1. - combined_haze);

    // Make a nice cloud density based on the cloud_shadow value that was passed in.
    float cloudDensity = 2. * (cloud_shadow - 0.25);

    // Combine these to minimize register use
    cloudColorAmbient += oHazeColorBelowCloud;

    vec3 color;          // <plus> volclouds: declared here for the two ways below (was: with their use) </plus>
    float alpha1;        // <plus> </plus>

#if defined(VOLUMETRIC)
    // <plus> plus/volclouds (research/186 16.2, research/244): march the slab. alpha1 is the opacity
    // along the ray, color the light the clouds send, each step lit as the 2D clouds light that place,
    // its sun brighter towards a cloud's top.
    {
        float texels = float(textureSize(cloud_noise_texture, 0).x);
        float href = plus_cloud_params.w;
        float steps = plus_cloud_params.y;
        float dt = (t_hi - t_lo) / steps;
        // each step takes the optical depth of its stretch of height in the column at its middle, the
        // stretches end to end: no noise on the start (the uv moves little along a ray)
        float dz = dt * vdir.y;
        float t = t_lo + 0.5 * dt;
        // the disturbance moves slowly: once per ray, at its middle
        vec3 disturb = plusCloudDisturb(plusCloudPlace(vdir.xz * (0.5 * (t_lo + t_hi)), href).xy, log2(max(pix_uv * texels, 1e-6)));
        // billows only where the clouds are seen from the side, low in the sky (elevation below 12 to 27
        // degrees): seen from below, lumps on the tops show as a mottle the 2D clouds do not have (fix4)
        float relief = plus_cloud_shape.y * (1.0 - smoothstep(0.2, 0.45, vdir.y));
        float image_mean = plusCloudNoise(vec2(0.5), 16.0);     // the image's mean (its smallest mip)
        // the tops (vol-clouds-fix4): the large shape with the fine pattern's mean, and only grain of
        // the fine pattern itself; the 2D clouds hide it inside a cloud (their alpha1 is 1 there), a top
        // that follows it in full is a sky of small round lumps
        float grain = plus_cloud_params.z;
        float detail_mean = (image_mean - 0.5) * cloud_pos_density2.z;
        float shade = plus_cloud_shape.w;
        float cloud_density = cloudDensity * disturb.z;     // 2D's, with the disturbance
        // a step's footprint in the uv: the pixel's, or a quarter of the steps' spacing where that is
        // longer (the uv moves little along a ray; the patterns stay as sharp as the 2D clouds')
        vec2 uv_prev = plusCloudPlace(vdir.xz * (t + dt), href).xy;
        float trans = 1.0;
        vec3 acc = vec3(0.0);
        for (int i = 0; i < 32; ++i)
        {
            if (float(i) >= steps || trans < 0.02)
            {
                break;
            }
            float z = plus_cloud_alt + vdir.y * t - slab_lo;    // above the base (m)
            vec3 place = plusCloudPlace(vdir.xz * t, href);
            vec2 uv = place.xy;
            // the footprint's level at one period per uv, below 0 under a texel: each pattern adds its
            // frequency (16: +4, 64: +6) before plusCloudNoise stops it at the image itself
            float lod = log2(max(max(pix_uv, 0.25 * length(uv - uv_prev)) * texels, 1e-6));
            uv_prev = uv;
            float base = plusCloudBase(uv, disturb, cloudDensity, lod);
            float field = plusCloudAddDetail(uv, base, lod + 4.0);
            if (field > 0.0)
            {
                vec2 column = plusCloudColumn(field, place.z);
                // a little height where only the fine pattern makes a cloud (its opacity is the 2D one)
                float top = max(plusCloudHeight(mix(base + detail_mean, field, grain)), 0.05) * slab_thick;
                if (relief > 0.0)
                {
                    // billows on the tops: the image at 64 periods per uv (a level coarser than its footprint);
                    // kept within the slab, so that the column keeps its whole depth
                    top = min(top * max(1.0 + relief * (plusCloudNoise(uv * 64.0 + cloud_pos_density2.xy * 4.0, lod + 7.0) - image_mean), 0.0), slab_thick);
                }
                // a step counts its height, not its length: a column lets through what the 2D clouds
                // do at every angle (a sheet seen low is not made thicker than 2D shows it)
                float tau = top > 0.0 ? plusCloudDepth(z + 0.5 * dz, column.x, top) - plusCloudDepth(z - 0.5 * dz, column.x, top) : 0.0;
                if (tau > 0.0)
                {
                    float sun = plusCloudSunLight(plusCloudSunShare(uv, cloud_density, lod), clamp(z / top, 0.0, 1.0), shade);
                    vec3 src = cloudColorSun * sun + cloudColorAmbient;
                    float a = 1.0 - exp(-tau);
                    acc += trans * a * src;
                    trans *= 1.0 - a;
                }
            }
            t += dt;
        }
        alpha1 = (1.0 - trans) * smoothstep(0.005, 0.03, vdir.y);
        color = acc / max(1.0 - trans, 1e-4);
    }
    // </plus>
#else // <plus> VOLUMETRIC </plus>
    // Cloud Fragment
    vec2 disturbance  = vec2(cloudNoise(uv1 / 8.0f).x, cloudNoise((uv3 + uv1) / 16.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);
    vec2 disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0f).x, cloudNoise((uv4 + uv2) / 8.0f).x) * cloud_variance * (1.0f - cloud_scale * 0.25f);

    // Offset texture coords
    uv1 += cloud_pos_density1.xy + (disturbance * 0.2);    //large texture, visible density
    uv2 += cloud_pos_density1.xy;   //large texture, self shadow
    uv3 += cloud_pos_density2.xy;   //small texture, visible density
    uv4 += cloud_pos_density2.xy;   //small texture, self shadow

    float density_variance = min(1.0, (disturbance.x* 2.0 + disturbance.y* 2.0 + disturbance2.x + disturbance2.y) * 4.0);

    cloudDensity *= 1.0 - (density_variance * density_variance);

    // Compute alpha1, the main cloud opacity

    alpha1 = (cloudNoise(uv1).x - 0.5) + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z;  // <plus> was: float alpha1 </plus>
    alpha1 = min(max(alpha1 + cloudDensity, 0.) * 10 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha1 = 1. - alpha1 * alpha1;
    alpha1 = 1. - alpha1 * alpha1;

    alpha1 *= altitude_blend_factor;
    alpha1 = clamp(alpha1, 0.0, 1.0);

    // Compute alpha2, for self shadowing effect
    // (1 - alpha2) will later be used as percentage of incoming sunlight
    float alpha2 = (cloudNoise(uv2).x - 0.5);
    alpha2 = min(max(alpha2 + cloudDensity, 0.) * 2.5 * cloud_pos_density1.z, 1.);

    // And smooth
    alpha2 = 1. - alpha2;
    alpha2 = 1. - alpha2 * alpha2;

    // Combine
    color = (cloudColorSun*(1.-alpha2) + cloudColorAmbient);
#endif // <plus> VOLUMETRIC </plus>
    color.rgb = clamp(color.rgb, vec3(0), vec3(1));
    color.rgb *= 2.0;

    // <plus> plus/nightclouds: what the classic sky shows (the colour as sRGB, clamped to 1:
    // "no post"), written so that it comes out so through the scale, the exposure and the curve;
    // only ever brighter than the official cloud
    if (plus_cloud_night > 0.0)
    {
        vec3 shown = min(plusToLinear(min(color.rgb, vec3(1.0))), vec3(0.95));
        vec3 kept = plusToSrgb(vec3(plusSkyLight(shown.r), plusSkyLight(shown.g), plusSkyLight(shown.b)));
        color.rgb = mix(color.rgb, max(color.rgb, kept), plus_cloud_night);
    }
    // </plus>

    // <plus> plus/rainbow (research/255): the rain under the clouds makes the bow in front of them. The same
    // bow as the sky's goes onto their colour: the two blend to the sky's and the clouds' light plus the bow,
    // so that where the clouds cover the sky it stays
    if (alpha1 > 0.0)
    {
        color.rgb = plusRainbowOver(color.rgb, rel_pos_norm);
    }
    // </plus>

    /// Gamma correct for WL (soft clip effect).

    frag_data[1] = vec4(0.0,0.0,0.0,0.0);
    frag_data[2] = vec4(0,0,0,GBUFFER_FLAG_SKIP_ATMOS);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(color.rgb, alpha1);
#else
    frag_data[0] = vec4(color.rgb, alpha1);
#endif
}

