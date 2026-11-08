/**
 * @file class1/deferred/cloudShadowMapF.glsl
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

// plus/volclouds (research/186 chapter 16.3, research/244, research/253 3.6): the volumetric clouds'
// shadow map. A square of the ground's plane around the camera (east across, north up, 2 *
// plus_cloud_map_radius metres wide, centred plus_cloud_map_centre east and north of the camera: where
// the way to the light crosses the layer) gets, for each texel, how much of the light the clouds above
// it take out at most: 1 - exp(-optical depth) of the column there. cloudsF.glsl (VOLUMETRIC) draws the
// same clouds from the same uv (the 2D clouds', around the camera), and the functions below are a copy
// of its own: keep the two in step.

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;
uniform float blend_factor;
uniform vec3 cloud_pos_density1;
uniform vec3 cloud_pos_density2;
uniform float cloud_scale;
uniform float cloud_variance;
uniform float cloud_shadow;

uniform vec4 plus_cloud_params;     // w: the layer's height the 2D clouds stand for (the sky's max_y, m)
uniform vec3 plus_cloud_shape;      // x: the camera's height in the 2D dome, a share of its radius; z: the dome's radius
uniform float plus_cloud_map_radius;
uniform vec2 plus_cloud_map_centre;   // east, north of the camera (m), on a whole number of texels

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

void main()
{
    if (cloud_scale < 0.001)
    {
        frag_color = vec4(0.0);
        return;
    }
    float texels = float(textureSize(cloud_noise_texture, 0).x);
    float href = plus_cloud_params.w;
    vec2 off = (vary_fragcoord - 0.5) * 2.0 * plus_cloud_map_radius + plus_cloud_map_centre;   // east, north of the camera (m)
    vec2 h = vec2(off.y, off.x);                                       // the dome's x is the north, its z the east
    vec3 place = plusCloudPlace(h, href);
    vec2 uv = place.xy;
    // a map texel's footprint in the uv (the map is 256 wide: MAP_SIZE in plus/volclouds/volclouds.cpp);
    // the patterns are averaged over it
    float texel = 2.0 * plus_cloud_map_radius / 256.0;
    float foot = max(length(plusCloudPlace(h + vec2(texel, 0.0), href).xy - uv), length(plusCloudPlace(h + vec2(0.0, texel), href).xy - uv));
    float lod = log2(max(foot * texels, 1e-6));
    float field = plusCloudField(uv, plusCloudDisturb(uv, lod), 2.0 * (cloud_shadow - 0.25), lod, lod + 4.0);
    // the column's optical depth (with 2D's altitude blend in its direction, as the clouds drawn):
    // the march in cloudsF.glsl sums its density up to it
    float tau = field > 0.0 ? plusCloudColumn(field, place.z).x : 0.0;
    frag_color = vec4(1.0 - exp(-tau), 0.0, 0.0, 1.0);
}
