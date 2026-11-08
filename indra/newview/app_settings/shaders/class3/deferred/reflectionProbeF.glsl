/**
 * @file class3/deferred/reflectionProbeF.glsl
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2022, Linden Research, Inc.
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

#define FLT_MAX 3.402823466e+38

#if defined(SSR)
float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, inout vec4 collectedColor, sampler2D source, float glossiness);
// <plus> fix-r11 (research/water-noise-2026-10-27): the water's screen space reflection
// without the grain while the camera moves (screenSpaceReflUtil.glsl). plusSSRDenoise is
// PlusSSRDenoise, set by the water's pool (0 on every other program, as never set).
float plusTapWaterSSR(vec2 tc, vec3 viewPos, vec3 n, vec3 up, inout vec4 collectedColor, float glossiness);
uniform float plusSSRDenoise;
bool plus_ssr_water = false;    // sampleReflectionProbesWater is sampling
// </plus>
#endif

uniform samplerCubeArray   reflectionProbes;
uniform samplerCubeArray   irradianceProbes;

uniform sampler2D sceneMap;
uniform int cube_snapshot;

// <plus> plus/ssr (research/129): screen space reflections traced in passes of
// their own (a port of project/graphics-care-package's). x 1 where sceneMap
// holds their buffer (the main view's opaque lighting only), else 0 and the
// official tracing runs; y the buffer's width over the screen's (below 1 it is
// upsampled with the depth as the guide).
#if defined(SSR) && defined(PLUS_SSR)
uniform vec4 plusSSRParams;
float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
#endif
// </plus>
uniform float max_probe_lod;

uniform bool transparent_surface;

uniform int classic_mode;

#define MAX_REFMAP_COUNT 256  // must match LL_MAX_REFLECTION_PROBE_COUNT

layout (std140) uniform ReflectionProbes
{
    // list of OBBs for user override probes
    // box is a set of 3 planes outward facing planes and the depth of the box along that plane
    // for each box refBox[i]...
    /// box[0..2] - plane 0 .. 2 in [A,B,C,D] notation
    //  box[3][0..2] - plane thickness
    mat4 refBox[MAX_REFMAP_COUNT];
    mat4 heroBox;
    // list of bounding spheres for reflection probes sorted by distance to camera (closest first)
    vec4 refSphere[MAX_REFMAP_COUNT];
    // extra parameters
    //  x - irradiance scale
    //  y - radiance scale
    //  z - fade in
    //  w - znear
    vec4 refParams[MAX_REFMAP_COUNT];
    vec4 heroSphere;
    // index  of cube map in reflectionProbes for a corresponding reflection probe
    // e.g. cube map channel of refSphere[2] is stored in refIndex[2]
    // refIndex.x - cubemap channel in reflectionProbes
    // refIndex.y - index in refNeighbor of neighbor list (index is ivec4 index, not int index)
    // refIndex.z - number of neighbors
    // refIndex.w - priority, if negative, this probe has a box influence
    ivec4 refIndex[MAX_REFMAP_COUNT];

    // neighbor list data (refSphere indices, not cubemap array layer)
    ivec4 refNeighbor[1024];

    ivec4 refBucket[256];

    // number of reflection probes present in refSphere
    int refmapCount;

    int heroShape;
    int heroMipCount;
    int heroProbeCount;
};

// Inputs
uniform mat3 env_mat;

// list of probeIndexes shader will actually use after "getRefIndex" is called
// (stores refIndex/refSphere indices, NOT rerflectionProbes layer)
int probeIndex[REF_SAMPLE_COUNT];

// number of probes stored in probeIndex
int probeInfluences = 0;

bool isAbove(vec3 pos, vec4 plane)
{
    return (dot(plane.xyz, pos) + plane.w) > 0;
}

bool sample_automatic = true;

// return true if probe at index i influences position pos
bool shouldSampleProbe(int i, vec3 pos)
{
    if (refIndex[i].w < 0)
    {
        vec4 v = refBox[i] * vec4(pos, 1.0);
        if (abs(v.x) > 1 ||
            abs(v.y) > 1 ||
            abs(v.z) > 1)
        {
            return false;
        }

        // never allow automatic probes to encroach on box probes
        sample_automatic = false;
    }
    else
    {
        if (refIndex[i].w == 0 && !sample_automatic)
        {
            return false;
        }

        vec3 delta = pos.xyz - refSphere[i].xyz;
        float d = dot(delta, delta);
        float r2 = refSphere[i].w;
        r2 *= r2;

        if (d > r2)
        { // outside bounding sphere
            return false;
        }
    }

    return true;
}

int getStartIndex(vec3 pos)
{
#if 1
    int idx = clamp(int(floor(-pos.z)), 0, 255);
    return clamp(refBucket[idx].x, 1, refmapCount+1);
#else
    return 1;
#endif
}

// call before sampleRef
// populate "probeIndex" with N probe indices that influence pos where N is REF_SAMPLE_COUNT
void preProbeSample(vec3 pos)
{
#if REFMAP_LEVEL > 0

    int start = getStartIndex(pos);

    // TODO: make some sort of structure that reduces the number of distance checks
    for (int i = start; i < refmapCount; ++i)
    {
        // found an influencing probe
        if (shouldSampleProbe(i, pos))
        {
            probeIndex[probeInfluences] = i;
            ++probeInfluences;

            int neighborIdx = refIndex[i].y;
            if (neighborIdx != -1)
            {
                int neighborCount = refIndex[i].z;

                int count = 0;
                while (count < neighborCount)
                {
                    // check up to REF_SAMPLE_COUNT-1 neighbors (neighborIdx is ivec4 index)

                    // sample refNeighbor[neighborIdx].x
                    int idx = refNeighbor[neighborIdx].x;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;
                    if (count == neighborCount)
                    {
                        break;
                    }

                    // sample refNeighbor[neighborIdx].y
                    idx = refNeighbor[neighborIdx].y;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;
                    if (count == neighborCount)
                    {
                        break;
                    }

                    // sample refNeighbor[neighborIdx].z
                    idx = refNeighbor[neighborIdx].z;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;
                    if (count == neighborCount)
                    {
                        break;
                    }

                    // sample refNeighbor[neighborIdx].w
                    idx = refNeighbor[neighborIdx].w;
                    if (shouldSampleProbe(idx, pos))
                    {
                        probeIndex[probeInfluences++] = idx;
                        if (probeInfluences == REF_SAMPLE_COUNT)
                        {
                            break;
                        }
                    }
                    count++;

                    ++neighborIdx;
                }

                break;
            }
        }
    }

    if (sample_automatic)
    { // probe at index 0 is a special probe for smoothing out automatic probes
        probeIndex[probeInfluences++] = 0;
    }
#else
    probeIndex[probeInfluences++] = 0;
#endif
}

// from https://www.scratchapixel.com/lessons/3d-basic-rendering/minimal-ray-tracer-rendering-simple-shapes/ray-sphere-intersection

// original reference implementation:
/*
bool intersect(const Ray &ray) const
{
        float t0, t1; // solutions for t if the ray intersects
#if 0
        // geometric solution
        Vec3f L = center - orig;
        float tca = L.dotProduct(dir);
        // if (tca < 0) return false;
        float d2 = L.dotProduct(L) - tca * tca;
        if (d2 > radius2) return false;
        float thc = sqrt(radius2 - d2);
        t0 = tca - thc;
        t1 = tca + thc;
#else
        // analytic solution
        Vec3f L = orig - center;
        float a = dir.dotProduct(dir);
        float b = 2 * dir.dotProduct(L);
        float c = L.dotProduct(L) - radius2;
        if (!solveQuadratic(a, b, c, t0, t1)) return false;
#endif
        if (t0 > t1) std::swap(t0, t1);

        if (t0 < 0) {
            t0 = t1; // if t0 is negative, let's use t1 instead
            if (t0 < 0) return false; // both t0 and t1 are negative
        }

        t = t0;

        return true;
} */

// adapted -- assume that origin is inside sphere, return intersection of ray with edge of sphere
vec3 sphereIntersect(vec3 origin, vec3 dir, vec3 center, float radius2)
{
        float t0, t1; // solutions for t if the ray intersects

        vec3 L = center - origin;
        float tca = dot(L,dir);

        float d2 = dot(L,L) - tca * tca;

        float thc = sqrt(radius2 - d2);
        t0 = tca - thc;
        t1 = tca + thc;

        vec3 v = origin + dir * t1;
        return v;
}

void swap(inout float a, inout float b)
{
    float t = a;
    a = b;
    b = a;
}

// debug implementation, make no assumptions about origin
void sphereIntersectDebug(vec3 origin, vec3 dir, vec3 center, float radius2, float depth, inout vec4 col)
{
    float t[2]; // solutions for t if the ray intersects

    // geometric solution
    vec3 L = center - origin;
    float tca = dot(L, dir);
    // if (tca < 0) return false;
    float d2 = dot(L, L) - tca * tca;
    if (d2 > radius2) return;
    float thc = sqrt(radius2 - d2);
    t[0] = tca - thc;
    t[1] = tca + thc;

    for (int i = 0; i < 2; ++i)
    {
        if (t[i] > 0)
        {
            if (t[i] > depth)
            {
                float w = 0.125/((t[i]-depth)*0.125 + 1.0);
                col += vec4(0, 0, w, w)*(1.0-min(col.a, 1.0));
            }
            else
            {
                float w = 0.25;
                col += vec4(w,w,0,w)*(1.0-min(col.a, 1.0));
            }
        }
    }

}

// from https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/
/*
vec3 DirectionWS = normalize(PositionWS - CameraWS);
vec3 ReflDirectionWS = reflect(DirectionWS, NormalWS);

// Intersection with OBB convertto unit box space
// Transform in local unit parallax cube space (scaled and rotated)
vec3 RayLS = MulMatrix( float(3x3)WorldToLocal, ReflDirectionWS);
vec3 PositionLS = MulMatrix( WorldToLocal, PositionWS);

vec3 Unitary = vec3(1.0f, 1.0f, 1.0f);
vec3 FirstPlaneIntersect  = (Unitary - PositionLS) / RayLS;
vec3 SecondPlaneIntersect = (-Unitary - PositionLS) / RayLS;
vec3 FurthestPlane = max(FirstPlaneIntersect, SecondPlaneIntersect);
float Distance = min(FurthestPlane.x, min(FurthestPlane.y, FurthestPlane.z));

// Use Distance in WS directly to recover intersection
vec3 IntersectPositionWS = PositionWS + ReflDirectionWS * Distance;
vec3 ReflDirectionWS = IntersectPositionWS - CubemapPositionWS;

return texCUBE(envMap, ReflDirectionWS);
*/

// get point of intersection with given probe's box influence volume
// origin - ray origin in clip space
// dir - ray direction in clip space
// i - probe index in refBox/refSphere
// d - distance to nearest wall in clip space
// scale - scale of box, default 1.0
vec3 boxIntersect(vec3 origin, vec3 dir, mat4 i, out float d, float scale)
{
    // Intersection with OBB convert to unit box space
    // Transform in local unit parallax cube space (scaled and rotated)
    mat4 clipToLocal = i;

    vec3 RayLS = mat3(clipToLocal) * dir;
    vec3 PositionLS = (clipToLocal * vec4(origin, 1.0)).xyz;

    d = 1.0-max(max(abs(PositionLS.x), abs(PositionLS.y)), abs(PositionLS.z));

    vec3 Unitary = vec3(scale);
    vec3 FirstPlaneIntersect  = (Unitary - PositionLS) / RayLS;
    vec3 SecondPlaneIntersect = (-Unitary - PositionLS) / RayLS;
    vec3 FurthestPlane = max(FirstPlaneIntersect, SecondPlaneIntersect);
    float Distance = min(FurthestPlane.x, min(FurthestPlane.y, FurthestPlane.z));

    // Use Distance in CS directly to recover intersection
    vec3 IntersectPositionCS = origin + dir * Distance;

    return IntersectPositionCS;
}

vec3 boxIntersect(vec3 origin, vec3 dir, mat4 i, out float d)
{
    return boxIntersect(origin, dir, i, d, 1.0);
}

void debugBoxCol(vec3 ro, vec3 rd, float t, vec3 p, inout vec4 col)
{
    vec3 v = ro + rd * t;

    v -= ro;
    vec3 pos = p - ro;


    bool behind = dot(v,v) > dot(pos,pos);

    float w = 0.25;

    if (behind)
    {
        w *= 0.5;
        w /= (length(v)-length(pos))*0.5+1.0;
        col += vec4(0,0,w,w)*(1.0-min(col.a, 1.0));
    }
    else
    {
        col += vec4(w,w,0,w)*(1.0-min(col.a, 1.0));
    }
}

// cribbed from https://iquilezles.org/articles/intersectors/
// axis aligned box centered at the origin, with size boxSize
void boxIntersectionDebug( in vec3 ro, in vec3 p, vec3 boxSize, inout vec4 col)
{
    vec3 rd = normalize(p-ro);

    vec3 m = 1.0/rd; // can precompute if traversing a set of aligned boxes
    vec3 n = m*ro;   // can precompute if traversing a set of aligned boxes
    vec3 k = abs(m)*boxSize;
    vec3 t1 = -n - k;
    vec3 t2 = -n + k;
    float tN = max( max( t1.x, t1.y ), t1.z );
    float tF = min( min( t2.x, t2.y ), t2.z );
    if( tN>tF || tF<0.0) return ; // no intersection

    float t = tN < 0 ? tF : tN;

    debugBoxCol(ro, rd, t, p, col);

    if (tN > 0) // eye is outside box, check backside, too
    {
        debugBoxCol(ro, rd, tF, p, col);
    }
}


void boxIntersectDebug(vec3 origin, vec3 pos, mat4 i, inout vec4 col)
{
    mat4 clipToLocal = i;

    // transform into unit cube space
    origin = (clipToLocal * vec4(origin, 1.0)).xyz;
    pos = (clipToLocal * vec4(pos, 1.0)).xyz;

    boxIntersectionDebug(origin, pos, vec3(1), col);
}


// get the weight of a sphere probe
//  pos - position to be weighted
//  dir - normal to be weighted
//  origin - center of sphere probe
//  r - radius of probe influence volume
// i - index of probe in refSphere
// dw - distance weight
float sphereWeight(vec3 pos, vec3 dir, vec3 origin, float r, vec4 i, out float dw)
{
    float r1 = r * 0.5; // 50% of radius (outer sphere to start interpolating down)
    vec3 delta = pos.xyz - origin;
    float d2 = max(length(delta), 0.001);

    float atten = 1.0 - max(d2 - r1, 0.0) / max((r - r1), 0.001);
    float w = 1.0 / d2;

    w *= i.z;

    dw = w * atten * max(r, 1.0)*4;

    w *= atten;

    return w;
}

// Tap a reflection probe
// pos - position of pixel
// dir - pixel normal
//  w - weight of sample (distance and angular attenuation)
//  dw - weight of sample (distance only)
// lod - which mip to sample (lower is higher res, sharper reflections)
// c - center of probe
// r2 - radius of probe squared
// i - index of probe
vec3 tapRefMap(vec3 pos, vec3 dir, out float w, out float dw, float lod, vec3 c, int i)
{
    // parallax adjustment
    vec3 v;

    if (refIndex[i].w < 0)
    {  // box probe
        float d = 0;
        v = boxIntersect(pos, dir, refBox[i], d);

        w = max(d, 0.001);
    }
    else
    { // sphere probe
        float r = refSphere[i].w;

        float rr = r * r;

        v = sphereIntersect(pos, dir, c,
        refIndex[i].w < 1 ? 4096.0*4096.0 : // <== effectively disable parallax correction for automatically placed probes to keep from bombing the world with obvious spheres
                rr);

        w = sphereWeight(pos, dir, refSphere[i].xyz, r, refParams[i], dw);
    }

    v -= c;
    vec3 d = normalize(v);

    v = env_mat * v;

    vec4 ret = textureLod(reflectionProbes, vec4(v.xyz, refIndex[i].x), lod) * refParams[i].y;

    return ret.rgb;
}

// Tap an irradiance map
// pos - position of pixel
// dir - pixel normal
// w - weight of sample (distance and angular attenuation)
// dw - weight of sample (distance only)
// i - index of probe
vec3 tapIrradianceMap(vec3 pos, vec3 dir, out float w, out float dw, vec3 c, int i, vec3 amblit)
{
    // parallax adjustment
    vec3 v;
    if (refIndex[i].w < 0)
    {
        float d = 0.0;
        v = boxIntersect(pos, dir, refBox[i], d, 3.0);
        w = max(d, 0.001);
    }
    else
    {
        float r = refSphere[i].w; // radius of sphere volume

        // pad sphere for manual probe extending into automatic probe space
        float rr = r * r;

        v = sphereIntersect(pos, dir, c,
        refIndex[i].w < 1 ? 4096.0*4096.0 : // <== effectively disable parallax correction for automatically placed probes to keep from bombing the world with obvious spheres
                rr);

        w = sphereWeight(pos, dir, refSphere[i].xyz, r, refParams[i], dw);
    }

    v -= c;
    v = env_mat * v;

    vec3 col = textureLod(irradianceProbes, vec4(v.xyz, refIndex[i].x), 0).rgb * refParams[i].x;

    col = mix(amblit, col, min(refParams[i].x, 1.0));

    return col;
}

vec3 sampleProbes(vec3 pos, vec3 dir, float lod)
{
    float wsum[2];
    wsum[0] = 0;
    wsum[1] = 0;

    float dwsum[2];
    dwsum[0] = 0;
    dwsum[1] = 0;

    vec3 col[2];
    col[0] = vec3(0);
    col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx)
    {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);

        if (p == 0 && !sample_automatic)
        {
            continue;
        }

        float w = 0;
        float dw = 0;
        vec3 refcol;

        {
            refcol = tapRefMap(pos, dir, w, dw, lod, refSphere[i].xyz, i);

            col[p] += refcol.rgb*w;
            wsum[p] += w;
            dwsum[p] += dw;
        }
    }

    // mix automatic and manual probes
    if (sample_automatic && wsum[0] > 0.0)
    { // some automatic probes were sampled
        col[0] *= 1.0/wsum[0];
        if (wsum[1] > 0.0)
        { //some manual probes were sampled, mix between the two
            col[1] *= 1.0/wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    }
    else if (wsum[1] > 0.0)
    {
        // manual probes were sampled but no automatic probes were
        col[1] *= 1.0/wsum[1];
        col[0] = vec3(0);
    }

    return col[1]+col[0];
}

vec3 sampleProbeAmbient(vec3 pos, vec3 dir, vec3 amblit)
{
    // modified copy/paste of sampleProbes follows, will likely diverge from sampleProbes further
    // as irradiance map mixing is tuned independently of radiance map mixing
    float wsum[2];
    wsum[0] = 0;
    wsum[1] = 0;

    float dwsum[2];
    dwsum[0] = 0;
    dwsum[1] = 0;

    vec3 col[2];
    col[0] = vec3(0);
    col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx)
    {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);

        if (p == 0 && !sample_automatic)
        {
            continue;
        }

        {
            float w = 0;
            float dw = 0;

            vec3 refcol = tapIrradianceMap(pos, dir, w, dw, refSphere[i].xyz, i, amblit);

            col[p] += refcol*w;
            wsum[p] += w;
            dwsum[p] += dw;
        }
    }

    // mix automatic and manual probes
    if (sample_automatic && wsum[0] > 0.0)
    { // some automatic probes were sampled
        col[0] *= 1.0/wsum[0];
        if (wsum[1] > 0.0)
        { //some manual probes were sampled, mix between the two
            col[1] *= 1.0/wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    }
    else if (wsum[1] > 0.0)
    {
        // manual probes were sampled but no automatic probes were
        col[1] *= 1.0/wsum[1];
        col[0] = vec3(0);
    }

    return col[1]+col[0];
}

// LLRHI (research/59): only the Vulkan backend defines LLRHI_PROBE_FUSE; GL
// compiles this file as it was. Then sampleProbeAmbient and sampleProbes
// (glossy, and legacy at lod 0, both along the same reflected direction) go
// through probeIndex in one walk instead of one walk each: every sum gets
// the same terms in the same order as in its own walk (the skip test reads
// sample_automatic, which preProbeSample has settled), and ends as there,
// so the results are the same. The taps of one probe then share the walk,
// the probe's loads and its sphere weight (and the glossy and legacy taps
// the parallax, which only the lod tells apart).
#ifdef LLRHI_PROBE_FUSE
// The end of sampleProbes / sampleProbeAmbient: mix automatic and manual probes.
vec3 probeSumsResult(float wsum[2], float dwsum[2], vec3 col[2])
{
    // mix automatic and manual probes
    if (sample_automatic && wsum[0] > 0.0)
    { // some automatic probes were sampled
        col[0] *= 1.0/wsum[0];
        if (wsum[1] > 0.0)
        { //some manual probes were sampled, mix between the two
            col[1] *= 1.0/wsum[1];
            col[1] = mix(col[0], col[1], min(dwsum[1], 1.0));
            col[0] = vec3(0);
        }
    }
    else if (wsum[1] > 0.0)
    {
        // manual probes were sampled but no automatic probes were
        col[1] *= 1.0/wsum[1];
        col[0] = vec3(0);
    }

    return col[1]+col[0];
}

// ambient: ambenv = sampleProbeAmbient(pos, norm, amblit)
// gloss: glossenv = sampleProbes(pos, dir, gloss_lod)
// legacy: legacyenv = sampleProbes(pos, dir, 0.0)
void sampleProbesFused(vec3 pos, vec3 norm, vec3 amblit, vec3 dir, float gloss_lod, bool ambient, bool gloss, bool legacy,
                       inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv)
{
    float a_wsum[2];
    float a_dwsum[2];
    vec3 a_col[2];
    float g_wsum[2];
    float g_dwsum[2];
    vec3 g_col[2];
    float l_wsum[2];
    float l_dwsum[2];
    vec3 l_col[2];
    a_wsum[0] = 0;
    a_wsum[1] = 0;
    a_dwsum[0] = 0;
    a_dwsum[1] = 0;
    a_col[0] = vec3(0);
    a_col[1] = vec3(0);
    g_wsum[0] = 0;
    g_wsum[1] = 0;
    g_dwsum[0] = 0;
    g_dwsum[1] = 0;
    g_col[0] = vec3(0);
    g_col[1] = vec3(0);
    l_wsum[0] = 0;
    l_wsum[1] = 0;
    l_dwsum[0] = 0;
    l_dwsum[1] = 0;
    l_col[0] = vec3(0);
    l_col[1] = vec3(0);

    for (int idx = 0; idx < probeInfluences; ++idx)
    {
        int i = probeIndex[idx];
        int p = clamp(abs(refIndex[i].w), 0, 1);

        if (p == 0 && !sample_automatic)
        {
            continue;
        }

        if (ambient)
        {
            float w = 0;
            float dw = 0;

            vec3 refcol = tapIrradianceMap(pos, norm, w, dw, refSphere[i].xyz, i, amblit);

            a_col[p] += refcol*w;
            a_wsum[p] += w;
            a_dwsum[p] += dw;
        }

        if (gloss)
        {
            float w = 0;
            float dw = 0;
            vec3 refcol;

            refcol = tapRefMap(pos, dir, w, dw, gloss_lod, refSphere[i].xyz, i);

            g_col[p] += refcol.rgb*w;
            g_wsum[p] += w;
            g_dwsum[p] += dw;
        }

        if (legacy)
        {
            float w = 0;
            float dw = 0;
            vec3 refcol;

            refcol = tapRefMap(pos, dir, w, dw, 0.0, refSphere[i].xyz, i);

            l_col[p] += refcol.rgb*w;
            l_wsum[p] += w;
            l_dwsum[p] += dw;
        }
    }

    if (ambient)
    {
        ambenv = probeSumsResult(a_wsum, a_dwsum, a_col);
    }
    if (gloss)
    {
        glossenv = probeSumsResult(g_wsum, g_dwsum, g_col);
    }
    if (legacy)
    {
        legacyenv = probeSumsResult(l_wsum, l_dwsum, l_col);
    }
}
#endif

#if defined(HERO_PROBES)

uniform vec4 clipPlane;
uniform samplerCubeArray   heroProbes;

void tapHeroProbe(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness)
{
    float clipDist = dot(pos.xyz, clipPlane.xyz) + clipPlane.w;
    float w = 0;
    float dw = 0;
    float falloffMult = 10;
    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);
    if (heroShape < 1)
    {
        float d = 0;
        boxIntersect(pos, norm, heroBox, d, 1.0);

        w = max(d, 0);
    }
    else
    {
        float r = heroSphere.w;

        w = sphereWeight(pos, refnormpersp, heroSphere.xyz, r, vec4(1), dw);
    }

    clipDist = clipDist * 0.95 + 0.05;
    clipDist = clamp(clipDist * falloffMult, 0, 1);
    w = clamp(w * falloffMult * clipDist, 0, 1);
    w = mix(0, w, clamp(glossiness - 0.75, 0, 1) * 4); // We only generate a quarter of the mips for the hero probes.  Linearly interpolate between normal probes and hero probes based upon glossiness.
    glossenv = mix(glossenv, textureLod(heroProbes, vec4(env_mat * refnormpersp, 0), (1.0-glossiness)*heroMipCount).xyz, w);
}

#else

void tapHeroProbe(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness)
{
}

#endif

// <plus> avatar reflection (plus/avatarrefl, research/127 stage 1): your avatar
// mirrored in the floor or the water under it. Only while PlusAvatarReflection
// is on, with Vulkan; GL never gets the define. The avatars were drawn from
// the eye mirrored in the plane, into a target of their own: a pixel of the
// plane and its reflection lie on the same ray from that eye, so the pixel's
// position projected with that camera finds the reflection.
#ifdef PLUS_AVATAR_REFL

uniform sampler2D plusAvatarReflMap;    // the avatars, lit (linear, as the probes)
uniform sampler2D plusAvatarReflDepth;  // their depth; 1 where nothing was drawn
uniform mat4 plusAvatarReflMat;         // eye space -> the mirrored camera's clip space
uniform vec4 plusAvatarReflPlane;       // the plane in eye space: xyz normal, w distance
uniform vec4 plusAvatarReflParams;      // x weight (0 off), y the plane's thickness (m), z the least normal match,
                                        // w 1 where the water's pixels take it too (0: a floor, the water left out)
bool plus_avatar_refl_water = false;    // sampleReflectionProbesWater is sampling

void tapAvatarReflection(inout vec3 glossenv, vec3 pos, vec3 norm, float glossiness)
{
    float strength = plusAvatarReflParams.x;
    if (cube_snapshot == 1 || strength <= 0.0 || (plus_avatar_refl_water && plusAvatarReflParams.w < 0.5))
    {
        return;
    }

    // Only on the plane (a floor's normal must face the same way; the water's waves need not).
    float dist = dot(pos, plusAvatarReflPlane.xyz) + plusAvatarReflPlane.w;
    if (abs(dist) > plusAvatarReflParams.y || dot(norm, plusAvatarReflPlane.xyz) < plusAvatarReflParams.z)
    {
        return;
    }

    // A sharp picture: full from a glossiness of 0.9 (where the screen space
    // reflections start), none at 0.7 and below.
    float w = strength * clamp((glossiness - 0.7) * 5.0, 0.0, 1.0);
    if (w <= 0.0)
    {
        return;
    }

    vec4 clip = plusAvatarReflMat * vec4(pos, 1.0);
    if (clip.w <= 0.0)
    {
        return;
    }
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.0))))
    {
        return;     // outside the picture (a snapshot of another shape)
    }
    float plane_depth = ndc.z * 0.5 + 0.5;

    // The four texels around the point, each only where an avatar was drawn
    // beyond the plane (not a foot or leg under it, in the floor or the
    // water): a soft edge without the background's colour.
    ivec2 size = textureSize(plusAvatarReflDepth, 0);
    vec2 texel = (ndc.xy * 0.5 + 0.5) * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(texel));
    vec2 f = texel - vec2(base);
    vec3 col = vec3(0);
    float cover = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 offset = ivec2(i & 1, i >> 1);
        ivec2 tc = clamp(base + offset, ivec2(0), size - 1);
        float depth = texelFetch(plusAvatarReflDepth, tc, 0).r;
        if (depth < 1.0 && depth + 0.0001 >= plane_depth)
        {
            vec2 bilinear = mix(1.0 - f, f, vec2(offset));
            float tw = bilinear.x * bilinear.y;
            col += texelFetch(plusAvatarReflMap, tc, 0).rgb * tw;
            cover += tw;
        }
    }
    if (cover > 0.0)
    {
        glossenv = mix(glossenv, col / cover, w * cover);
    }
}

#endif
// </plus>

// <plus> water mirror (plus/avatarrefl/watermirror, research/139): the world
// drawn again from the eye mirrored in the region's water, as the water before
// PBR reflected it. Only while PlusWaterMirror is on, with Vulkan; GL never
// gets the define. Only on the water (sampleReflectionProbesWater): a pixel of
// the water and its reflection lie on the same ray from the mirrored eye, so
// the pixel's position projected with that camera finds the reflection; the
// waves' normal moves the point a little, as the old water's distortion did.
#ifdef PLUS_WATER_MIRROR

uniform sampler2D plusWaterMirrorMap;   // the mirrored world, lit (linear, as the probes)
uniform sampler2D plusWaterMirrorDepth; // its depth; 1 where only the sky was drawn
uniform mat4 plusWaterMirrorMat;        // eye space -> the mirrored camera's clip space
uniform vec4 plusWaterMirrorPlane;      // the water in eye space: xyz up, w distance
uniform vec4 plusWaterMirrorParams;     // x weight (0 off), y the waves' strength
uniform vec4 plusWaterMirrorFilter;     // xy one texel of the picture (uv), z PlusWaterMirrorAA, w the picture's texels per screen pixel

bool plus_water_mirror_here = false;    // sampleReflectionProbesWater is sampling

// The anti-aliasing of the read (PlusWaterMirrorAA), on the picture itself:
// no pass or target of its own. The picture is drawn without anti-aliasing
// (a probe face's rules), at a share of the screen (PlusWaterMirrorScale), so
// its edges are steps of one texel, two screen pixels at the default half.
float plusWaterMirrorLuma(vec3 c)
{
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    return l / (1.0 + l);   // the HDR picture compressed, so bright and dark edges count alike
}

// 1: edge smoothing as FXAA's (Lottes' FXAA 2, on the picture's texels): the
// contrast of the four diagonal neighbours finds an edge and its direction,
// and the read is spread along the edge. Away from edges the read is one tap
// (or the four's mean when the picture is larger than the screen).
vec3 plusWaterMirrorEdgeSmooth(vec2 uv, vec2 texel, bool downsample)
{
    vec3 nw = textureLod(plusWaterMirrorMap, uv + vec2(-0.5, -0.5) * texel, 0.0).rgb;
    vec3 ne = textureLod(plusWaterMirrorMap, uv + vec2( 0.5, -0.5) * texel, 0.0).rgb;
    vec3 sw = textureLod(plusWaterMirrorMap, uv + vec2(-0.5,  0.5) * texel, 0.0).rgb;
    vec3 se = textureLod(plusWaterMirrorMap, uv + vec2( 0.5,  0.5) * texel, 0.0).rgb;
    vec3 m = downsample ? (nw + ne + sw + se) * 0.25 : textureLod(plusWaterMirrorMap, uv, 0.0).rgb;

    float l_nw = plusWaterMirrorLuma(nw);
    float l_ne = plusWaterMirrorLuma(ne);
    float l_sw = plusWaterMirrorLuma(sw);
    float l_se = plusWaterMirrorLuma(se);
    float l_m = plusWaterMirrorLuma(m);
    float l_min = min(l_m, min(min(l_nw, l_ne), min(l_sw, l_se)));
    float l_max = max(l_m, max(max(l_nw, l_ne), max(l_sw, l_se)));
    if (l_max - l_min < max(0.0312, l_max * 0.125))
    {
        return m;   // no edge here
    }

    vec2 dir = vec2(-((l_nw + l_ne) - (l_sw + l_se)), (l_nw + l_sw) - (l_ne + l_se));
    float reduce = max((l_nw + l_ne + l_sw + l_se) * (0.25 / 8.0), 1.0 / 128.0);
    float rcp_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp_min, vec2(-4.0), vec2(4.0)) * texel;    // along the edge, up to 4 texels

    vec3 a = 0.5 * (textureLod(plusWaterMirrorMap, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
                    textureLod(plusWaterMirrorMap, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 b = a * 0.5 + 0.25 * (textureLod(plusWaterMirrorMap, uv - dir * 0.5, 0.0).rgb +
                               textureLod(plusWaterMirrorMap, uv + dir * 0.5, 0.0).rgb);
    float l_b = plusWaterMirrorLuma(b);
    return (l_b < l_min || l_b > l_max) ? a : b;
}

// The colour read: one bilinear tap (PlusWaterMirrorAA 0 at up to the
// screen's size, as before), the edge smoothing (1), or four taps on a
// rotated grid (2, soft; also 0 above the screen's size, as the box of the
// screen pixel's texels: the supersampling's resolve).
vec3 plusWaterMirrorColor(vec2 uv)
{
    int aa = int(plusWaterMirrorFilter.z + 0.5);
    float texels_per_pixel = plusWaterMirrorFilter.w;
    bool downsample = texels_per_pixel > 1.0;
    if (aa == 1)
    {
        return plusWaterMirrorEdgeSmooth(uv, plusWaterMirrorFilter.xy, downsample);
    }
    if (aa == 0 && !downsample)
    {
        return textureLod(plusWaterMirrorMap, uv, 0.0).rgb;
    }
    // The screen pixel's share of the picture (at least one texel); 2 reaches a little wider.
    vec2 texel = plusWaterMirrorFilter.xy * max(texels_per_pixel, 1.0);
    vec2 o1 = aa == 2 ? vec2(0.1875, 0.5625) : vec2(0.25, 0.25);
    vec2 o2 = aa == 2 ? vec2(0.5625, -0.1875) : vec2(0.25, -0.25);
    return 0.25 * (textureLod(plusWaterMirrorMap, uv + o1 * texel, 0.0).rgb +
                   textureLod(plusWaterMirrorMap, uv - o1 * texel, 0.0).rgb +
                   textureLod(plusWaterMirrorMap, uv + o2 * texel, 0.0).rgb +
                   textureLod(plusWaterMirrorMap, uv - o2 * texel, 0.0).rgb);
}

// Where the mirror drew something (1) or only its sky (0), as a share of the
// four nearest texels, so the mirror's objects meet the screen space
// reflections without steps (with PlusWaterMirrorAA; 0 takes the nearest).
float plusWaterMirrorGeom(vec2 uv, ivec2 size)
{
    ivec2 hi = size - 1;
    if (plusWaterMirrorFilter.z < 0.5)
    {
        return texelFetch(plusWaterMirrorDepth, clamp(ivec2(uv * vec2(size)), ivec2(0), hi), 0).r < 1.0 ? 1.0 : 0.0;
    }
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    float g00 = texelFetch(plusWaterMirrorDepth, clamp(i, ivec2(0), hi), 0).r < 1.0 ? 1.0 : 0.0;
    float g10 = texelFetch(plusWaterMirrorDepth, clamp(i + ivec2(1, 0), ivec2(0), hi), 0).r < 1.0 ? 1.0 : 0.0;
    float g01 = texelFetch(plusWaterMirrorDepth, clamp(i + ivec2(0, 1), ivec2(0), hi), 0).r < 1.0 ? 1.0 : 0.0;
    float g11 = texelFetch(plusWaterMirrorDepth, clamp(i + ivec2(1, 1), ivec2(0), hi), 0).r < 1.0 ? 1.0 : 0.0;
    return mix(mix(g00, g10, f.x), mix(g01, g11, f.x), f.y);
}

// The mirror's colour at this pixel and how much of it to take (w: 0 off the
// water, outside the picture or on rough water). geom is 1 where the mirror
// drew something there (terrain, plants, objects, avatars), 0 for its sky.
void plusWaterMirrorSample(vec3 pos, vec3 norm, float glossiness, out vec3 col, out float w, out float geom)
{
    col = vec3(0);
    w = 0.0;
    geom = 0.0;
    float strength = plusWaterMirrorParams.x;
    if (!plus_water_mirror_here || cube_snapshot == 1 || strength <= 0.0)
    {
        return;
    }

    // The region's water only (another region's water may lie at another height).
    float dist = dot(pos, plusWaterMirrorPlane.xyz) + plusWaterMirrorPlane.w;
    if (abs(dist) > 0.1)
    {
        return;
    }

    // A sharp picture: full from a glossiness of 0.9, none at 0.7 and below
    // (as the avatar reflection).
    float k = strength * clamp((glossiness - 0.7) * 5.0, 0.0, 1.0);
    if (k <= 0.0)
    {
        return;
    }

    vec4 clip = plusWaterMirrorMat * vec4(pos, 1.0);
    if (clip.w <= 0.0)
    {
        return;
    }
    vec2 ndc = clip.xy / clip.w;
    if (any(greaterThan(abs(ndc), vec2(1.0))))
    {
        return;     // outside the picture (a snapshot of another shape)
    }

    // The waves: the normal's tilt along the water moves the point read, by
    // about the same share of the picture as the old water at any distance
    // (projected, a metre shrinks with the distance; the sqrt leaves the old
    // water's 1 / sqrt(distance)).
    vec3 tilt = norm - plusWaterMirrorPlane.xyz * dot(norm, plusWaterMirrorPlane.xyz);
    vec4 wavy = plusWaterMirrorMat * vec4(pos + tilt * (plusWaterMirrorParams.y * sqrt(max(length(pos), 1.0))), 1.0);
    if (wavy.w > 0.0)
    {
        ndc = clamp(wavy.xy / wavy.w, vec2(-1.0), vec2(1.0));
    }

    vec2 uv = ndc * 0.5 + 0.5;
    col = plusWaterMirrorColor(uv);
    w = k;
    geom = plusWaterMirrorGeom(uv, textureSize(plusWaterMirrorDepth, 0));
}

#endif
// </plus>

// <plus> plus/ssr: the separate SSR passes' buffer (premultiplied by the hit's
// weight, the colour compressed by c / (1 + luma)), as GCP's
// reflectionProbeF.glsl reads its own (sampleSSRBilateral, the decode in
// doProbeSample). Already blurred by the roughness and faded towards the
// trace's limit, so read at level 0.
#if defined(SSR) && defined(PLUS_SSR)
vec4 plusSampleSSR(vec2 tc, vec3 pos)
{
    if (plusSSRParams.y > 0.999)
    {
        return textureLod(sceneMap, tc, 0.0);
    }
    // The four texels around tc, each by how close the depth it was traced
    // from is to this pixel's (no reflection bleeding across an edge).
    float ref_z = -pos.z;
    vec2 size = vec2(textureSize(sceneMap, 0));
    vec2 p = tc * size - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    ivec2 ibase = ivec2(base);
    ivec2 imax = ivec2(size) - 1;
    vec4 sum = vec4(0.0);
    float weight_sum = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 offset = ivec2(i & 1, i >> 1);
        ivec2 texel = clamp(ibase + offset, ivec2(0), imax);
        vec2 uv = (vec2(texel) + 0.5) / size;
        float tap_z = -getPositionWithDepth(uv, getDepth(uv)).z;
        float rel_diff = abs(tap_z - ref_z) / max(ref_z, 0.5);
        vec2 bilinear = mix(1.0 - f, f, vec2(offset));
        // The floor keeps plain bilinear where every tap is off.
        float w = bilinear.x * bilinear.y * (exp2(-24.0 * rel_diff) + 1e-3);
        sum += texelFetch(sceneMap, texel, 0) * w;
        weight_sum += w;
    }
    return sum / max(weight_sum, 1e-6);
}

// The reflection's colour and how much of it to mix in.
vec4 plusSSR(vec2 tc, vec3 pos)
{
    vec4 ssr = plusSampleSSR(tc, pos);
    if (ssr.a <= 0.001)
    {
        return vec4(0.0);
    }
    ssr.rgb /= ssr.a;
    float l = dot(ssr.rgb, vec3(0.2126, 0.7152, 0.0722));
    ssr.rgb /= max(1.0 - l, 0.02);  // GCP: cap the recovery, emissive hits blow out otherwise
    return vec4(ssr.rgb, min(ssr.a, 1.0));
}
#endif
// </plus>

void doProbeSample(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit)
{
    // TODO - don't hard code lods
    float reflection_lods = max_probe_lod;

    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

    ambenv = amblit;

#ifdef LLRHI_PROBE_FUSE
    float lod = (1.0-glossiness)*reflection_lods;
    vec3 unused_legacyenv = vec3(0);    // not glossenv again: the copy-out order of two inouts is undefined
    sampleProbesFused(pos, norm, amblit, normalize(refnormpersp), lod, classic_mode == 0, true, false, ambenv, glossenv, unused_legacyenv);
#else
    if (classic_mode == 0)
        ambenv = sampleProbeAmbient(pos, norm, amblit);

    float lod = (1.0-glossiness)*reflection_lods;
    glossenv = sampleProbes(pos, normalize(refnormpersp), lod);
#endif

    // <plus> water mirror (plus/avatarrefl/watermirror): in place of the probes on the water; the
    // screen space reflections below may still cover its sky (what the mirror's mode leaves out)
#ifdef PLUS_WATER_MIRROR
    vec3 plus_mirror_col;
    float plus_mirror_w;
    float plus_mirror_geom;
    plusWaterMirrorSample(pos, norm, glossiness, plus_mirror_col, plus_mirror_w, plus_mirror_geom);
    glossenv = mix(glossenv, plus_mirror_col, plus_mirror_w);
#endif
    // </plus>

#if defined(SSR)
    // <plus> plus/ssr: the main view's opaque lighting reads the separate passes' reflection (GCP: up to roughness 0.7)
#ifdef PLUS_SSR
    if (plusSSRParams.x > 0.0 && !transparent)
    {
        vec4 ssr = plusSSR(tc, pos);
        glossenv = mix(glossenv, ssr.rgb, ssr.a);
    }
    else
#endif
    // </plus>
    if (cube_snapshot != 1 && glossiness >= 0.9)
    {
        vec4 ssr = vec4(0);
        if (transparent)
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, 1);
            ssr.a *= glossiness;
        }
        // <plus> fix-r11: the water (PlusSSRDenoise); up is the world's up in eye space (env_mat: eye to world)
        else if (plus_ssr_water && plusSSRDenoise > 0.5)
        {
            plusTapWaterSSR(tc, pos, norm, vec3(0, 0, 1) * env_mat, ssr, glossiness);
        }
        // </plus>
        else
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, glossiness);
        }


        glossenv = mix(glossenv, ssr.rgb, ssr.a);
    }
#endif

    // <plus> water mirror (plus/avatarrefl/watermirror): over the screen space reflections where
    // the mirror drew something (sharper, and not cut at the screen's edge)
#ifdef PLUS_WATER_MIRROR
    glossenv = mix(glossenv, plus_mirror_col, plus_mirror_w * plus_mirror_geom);
#endif
    // </plus>

    // <plus> avatar reflection (plus/avatarrefl): after the screen space reflections, before the mirror
#ifdef PLUS_AVATAR_REFL
    tapAvatarReflection(glossenv, pos, norm, glossiness);
#endif
    // </plus>

    tapHeroProbe(glossenv, pos, norm, glossiness);
}

void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit)
{
    preProbeSample(pos);
    doProbeSample(ambenv, glossenv, tc, pos, norm, glossiness, transparent, amblit);
}

void sampleReflectionProbesWater(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, vec3 amblit)
{
    // don't sample automatic probes for water
    sample_automatic = false;
    preProbeSample(pos);
    sample_automatic = true;
    // always include void probe on water
    probeIndex[probeInfluences++] = 0;

    // <plus> water mirror (plus/avatarrefl/watermirror): only the water reads it
#ifdef PLUS_WATER_MIRROR
    plus_water_mirror_here = true;
#endif
    // </plus>
    // <plus> fix-r11: the water's screen space reflection (PlusSSRDenoise)
#if defined(SSR)
    plus_ssr_water = true;
#endif
    // </plus>
    // <plus> avatar reflection (plus/avatarrefl): a floor's reflection leaves the water out
#ifdef PLUS_AVATAR_REFL
    plus_avatar_refl_water = true;
#endif
    // </plus>
    doProbeSample(ambenv, glossenv, tc, pos, norm, glossiness, false, amblit);
    // <plus> fix-r11
#if defined(SSR)
    plus_ssr_water = false;
#endif
    // </plus>
    // <plus> avatar reflection (plus/avatarrefl)
#ifdef PLUS_AVATAR_REFL
    plus_avatar_refl_water = false;
#endif
    // </plus>
    // <plus> water mirror (plus/avatarrefl/watermirror)
#ifdef PLUS_WATER_MIRROR
    plus_water_mirror_here = false;
#endif
    // </plus>
}

void debugTapRefMap(vec3 pos, vec3 dir, float depth, int i, inout vec4 col)
{
    vec3 origin = vec3(0,0,0);

    bool manual_probe = abs(refIndex[i].w) > 0;

    if (manual_probe)
    {
        if (refIndex[i].w < 0)
        {
            boxIntersectDebug(origin, pos, refBox[i], col);
        }
        else
        {
            float r = refSphere[i].w; // radius of sphere volume
            float rr = r * r; // radius squared

            float t = 0.0;

            sphereIntersectDebug(origin, dir, refSphere[i].xyz, rr, depth, col);
        }
    }
}

vec4 sampleReflectionProbesDebug(vec3 pos)
{
    vec4 col = vec4(0,0,0,0);

    vec3 dir = normalize(pos);

    float d = length(pos);

    for (int i = 1; i < refmapCount; ++i)
    {
        debugTapRefMap(pos, dir, d, i, col);
    }

#if 0 //debug getStartIndex
    col.g = float(getStartIndex(pos));

    col.g /= 255.0;
    col.rb = vec2(0);
    col.a = 1.0;
#endif

    return col;
}

void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit)
{
    float reflection_lods = max_probe_lod;
    preProbeSample(pos);

    vec3 refnormpersp = reflect(pos.xyz, norm.xyz);

    ambenv = amblit;

#ifdef LLRHI_PROBE_FUSE
    sampleProbesFused(pos, norm, amblit, normalize(refnormpersp), (1.0-glossiness)*reflection_lods,
                      classic_mode == 0, glossiness > 0.0, envIntensity > 0.0, ambenv, glossenv, legacyenv);
#else
    if (classic_mode == 0)
        ambenv = sampleProbeAmbient(pos, norm, amblit);

    if (glossiness > 0.0)
    {
        float lod = (1.0-glossiness)*reflection_lods;
        glossenv = sampleProbes(pos, normalize(refnormpersp), lod);

    }

    if (envIntensity > 0.0)
    {
        legacyenv = sampleProbes(pos, normalize(refnormpersp), 0.0);
    }
#endif

#if defined(SSR)
    // <plus> plus/ssr: as in doProbeSample
#ifdef PLUS_SSR
    if (plusSSRParams.x > 0.0 && !transparent)
    {
        vec4 ssr = plusSSR(tc, pos);
        glossenv = mix(glossenv, ssr.rgb, ssr.a);
        legacyenv = mix(legacyenv, ssr.rgb, ssr.a);
    }
    else
#endif
    // </plus>
    if (cube_snapshot != 1)
    {
        vec4 ssr = vec4(0);

        if (transparent)
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, 1);
            ssr.a *= glossiness;
        }
        else
        {
            tapScreenSpaceReflection(1, tc, pos, norm, ssr, sceneMap, glossiness);
        }

        glossenv = mix(glossenv, ssr.rgb, ssr.a);
        legacyenv = mix(legacyenv, ssr.rgb, ssr.a);
    }
#endif

    // <plus> avatar reflection (plus/avatarrefl): after the screen space reflections, before the mirror
#ifdef PLUS_AVATAR_REFL
    tapAvatarReflection(glossenv, pos, norm, glossiness);
    tapAvatarReflection(legacyenv, pos, norm, 1.0);
#endif
    // </plus>

    tapHeroProbe(glossenv, pos, norm, glossiness);
    tapHeroProbe(legacyenv, pos, norm, 1.0);

    glossenv = clamp(glossenv, vec3(0), vec3(10));
}

void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm)
{
    glossenv *= 0.5; // fudge darker
    float fresnel = clamp(1.0+dot(normalize(pos.xyz), norm.xyz), 0.3, 1.0);
    fresnel *= fresnel;
    fresnel *= spec.a;
    glossenv *= spec.rgb*fresnel;
    glossenv *= vec3(1.0) - color; // fake energy conservation
    color.rgb += glossenv*0.5;
}

 void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity)
 {
    vec3 reflected_color = legacyenv;
    vec3 lookAt = normalize(pos);
    float fresnel = 1.0+dot(lookAt, norm.xyz);
    fresnel *= fresnel;
    fresnel = min(fresnel+envIntensity, 1.0);
    reflected_color *= (envIntensity*fresnel);
    color = mix(color.rgb, reflected_color*0.5, envIntensity);
 }

