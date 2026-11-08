// BC7 encoder compute shader (research/88), included by llvktexcompress.cpp
// as a C++ string: raw string pieces (MSVC takes at most 16 KB per literal).
R"GLSL(
// BC7 encoder, written for this viewer: one invocation per 4x4 block.
// Opaque blocks: mode 6 (one subset, 4-bit indices), and when that leaves a
// visible error, mode 1 (two subsets, 3-bit indices) on the two partitions a
// principal-axis estimate ranks best. Blocks with alpha: mode 6, mode 5
// (colour and alpha on 2-bit indices of their own), mode 4 (colour on 3-bit
// indices, alpha on 2-bit), and when those leave a visible error, mode 7 (two
// subsets with alpha). Endpoints come from the principal axis of the texels,
// then two least-squares passes on the chosen indices. Errors are summed
// squared differences of the decoded 8-bit values.
//
// Everything above main() also builds as C++ (the CPU test of the encoder,
// research/88): INOUT, OUT and INOUTA stand for GLSL's parameter qualifiers.
//
// bc7-fault (research/88, section 11): a driver (NVIDIA 610.88) faulted the GPU in
// this shader. Every loop here has a fixed count, every array index and
// shift count is held in its range, and there are no large private arrays
// indexed by data: nothing a compiler may treat as undefined. The forms used
// up to 2026-11-02 stay for finding which change a driver needs: each bit of
// BC7_SAFE takes one group of the safe forms (0: all as before). Both give
// the same blocks, bit for bit (the CPU test compares them).
//   1: quant_channel and quant_pair: loops of a fixed count
//   2: assign: three candidates computed in place (no palette array)
//   4: indices held to their array, shift counts to 31, the push constants checked
//   8: best_shapes: the sums from the texels (no per-texel arrays)
#ifndef BC7_SAFE
#define BC7_SAFE 15
#endif
//
// bc7-fault2 (research/88, section 12): the safe forms still faulted the
// GPU on that driver. For finding the part it faults in, more forms, each a
// define the viewer puts after the #version (LLRHI_TEX_BCN_MODES, _KERNEL):
//   BC7_MODES: a bit for each BC7 mode the encoder may use (bit n: mode n of
//     1, 4, 5, 6 and 7; default all). A block no allowed mode fits is given
//     its mean colour.
//   BC7_PASS, BC7_PASSES: the encoder in one shader (BC7_PASS 0), or in
//     dispatches of their own with what is between them in the aux blocks:
//     pass 1 the one-subset modes, pass 2 the shape search and, of two passes,
//     the two-subset modes; of three, pass 3 the two-subset modes.
//   BC7_PX_SHARED: the block's texels in shared memory (a row an invocation).
//   BC7_ROLLED: the large loops kept as loops ([[dont_unroll]]).
//   BC7_STAGE (bc7-fast-start, research/88 section 13): one stage of the
//     encoder in six small pipelines (6, 4, 5, BC7_STAGE_SEARCH, 1, 7; see
//     "The encoder in stages" by main).
// All of them give the same blocks as the one shader with every mode.
#ifndef BC7_MODES
#define BC7_MODES 0xF2
#endif
#define BC7_MODE(n) ((BC7_MODES & (1 << (n))) != 0)
#ifndef BC7_PASS
#define BC7_PASS 0
#endif
#ifndef BC7_PASSES
#define BC7_PASSES 1
#endif
#define BC7_STAGE_SEARCH 100
#ifdef BC7_ROLLED
#define DONT_UNROLL [[dont_unroll]]
#else
#define DONT_UNROLL
#endif

#ifndef BC7_CPU_TEST
#define INOUT(T) inout T
#define OUT(T) out T
#define INOUTA inout
#endif

const uint WEIGHTS2[4] = { 0u, 21u, 43u, 64u };
const uint WEIGHTS3[8] = { 0u, 9u, 18u, 27u, 37u, 46u, 55u, 64u };
const uint WEIGHTS4[16] = { 0u, 4u, 9u, 13u, 17u, 21u, 26u, 30u, 34u, 38u, 43u, 47u, 51u, 55u, 60u, 64u };

// Two-subset partitions: bit i set = texel i (row i / 4, column i % 4) is in subset 1.
// ("partition" is a reserved word in GLSL: the encoder calls one a shape.)
const uint SHAPES2[64] = {
    0xCCCCu, 0x8888u, 0xEEEEu, 0xECC8u, 0xC880u, 0xFEECu, 0xFEC8u, 0xEC80u,
    0xC800u, 0xFFECu, 0xFE80u, 0xE800u, 0xFFE8u, 0xFF00u, 0xFFF0u, 0xF000u,
    0xF710u, 0x008Eu, 0x7100u, 0x08CEu, 0x008Cu, 0x7310u, 0x3100u, 0x8CCEu,
    0x088Cu, 0x3110u, 0x6666u, 0x366Cu, 0x17E8u, 0x0FF0u, 0x718Eu, 0x399Cu,
    0xAAAAu, 0xF0F0u, 0x5A5Au, 0x33CCu, 0x3C3Cu, 0x55AAu, 0x9696u, 0xA55Au,
    0x73CEu, 0x13C8u, 0x324Cu, 0x3BDCu, 0x6996u, 0xC33Cu, 0x9966u, 0x0660u,
    0x0272u, 0x04E4u, 0x4E40u, 0x2720u, 0xC936u, 0x936Cu, 0x39C6u, 0x639Cu,
    0x9336u, 0x9CC6u, 0x817Eu, 0xE718u, 0xCCF0u, 0x0FCCu, 0x7744u, 0xEE22u };

// The anchor texel of subset 1 in each shape (its index has one bit less).
const uint ANCHORS2[64] = {
    15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u,
    15u, 15u, 15u, 15u, 15u, 15u, 15u, 15u,
    15u,  2u,  8u,  2u,  2u,  8u,  8u, 15u,
     2u,  8u,  2u,  2u,  8u,  8u,  2u,  2u,
    15u, 15u,  6u,  8u,  2u,  8u, 15u, 15u,
     2u,  8u,  2u,  2u,  2u, 15u, 15u,  6u,
     6u,  2u,  6u,  8u, 15u, 15u,  2u,  2u,
    15u, 15u, 15u, 15u, 15u,  2u,  2u, 15u };

const uint ALL_TEXELS = 0xFFFFu;
const float NO_ERROR_YET = 1e30;
// The two-subset modes are tried only when the one-subset ones leave more
// squared error than this over the block (16 texels, about 2 per value:
// near 45 dB). Most blocks of a texture are smooth and stop there.
const float TWO_SUBSET_MIN_ERROR_RGB = 96.0;
const float TWO_SUBSET_MIN_ERROR_RGBA = 128.0;
// Least-squares passes after the principal-axis endpoints.
const int REFINE_PASSES = 2;

// How endpoints carry p-bits (the lowest bit of every channel of an endpoint).
const uint P_NONE = 0u;     // none
const uint P_EACH = 1u;     // one per endpoint
const uint P_SHARED = 2u;   // one for both endpoints of a subset
const uint P_ONE = 3u;      // one per endpoint, always 1 (opaque mode 6: alpha stays 255)

const vec4 CHAN_RGBA = vec4(1.0, 1.0, 1.0, 1.0);
const vec4 CHAN_RGB = vec4(1.0, 1.0, 1.0, 0.0);
const vec4 CHAN_ALPHA = vec4(0.0, 0.0, 0.0, 1.0);

#ifdef BC7_PX_SHARED
// The block's texels, 0..255: a row of 16 for each of the 64 invocations of a
// group (main's local_size_x), each reading and writing its own row only.
shared vec4 s_px[64][16];
#define g_px s_px[gl_LocalInvocationIndex]
#else
vec4 g_px[16];      // the block's texels, 0..255
#endif

uint weight(uint bits, uint index)
{
#if (BC7_SAFE & 4) != 0
    // Held to each table: a compiler may read all three before it picks one.
    return bits == 2u ? WEIGHTS2[index & 3u] : bits == 3u ? WEIGHTS3[index & 7u] : WEIGHTS4[index & 15u];
#else
    return bits == 2u ? WEIGHTS2[index] : bits == 3u ? WEIGHTS3[index] : WEIGHTS4[index];
#endif
}

uint interpolate(uint e0, uint e1, uint w)
{
    return ((64u - w) * e0 + w * e1 + 32u) >> 6u;
}

vec4 palette_entry(uvec4 d0, uvec4 d1, uint w)
{
    return vec4(float(interpolate(d0.x, d1.x, w)), float(interpolate(d0.y, d1.y, w)),
                float(interpolate(d0.z, d1.z, w)), float(interpolate(d0.w, d1.w, w)));
}

bool in_mask(uint mask, uint i)
{
    return (mask & (1u << i)) != 0u;
}

void put_bits(INOUT(uvec4) blk, INOUT(uint) pos, uint value, uint count)
{
    uint word = pos >> 5u;
    uint shift = pos & 31u;
#if (BC7_SAFE & 4) != 0
    // A block's 128 bits are words 0..3: the next word only when the value
    // reaches into it (a compiler may do the read of it either way).
    blk[word & 3u] |= value << shift;
    if (shift + count > 32u && word < 3u)
    {
        blk[(word + 1u) & 3u] |= value >> ((32u - shift) & 31u);
    }
#else
    blk[word] |= value << shift;
    if (shift + count > 32u)
    {
        blk[word + 1u] |= value >> (32u - shift);
    }
#endif
    pos += count;
}

// --- Endpoint codes ---------------------------------------------------------

// A total-bit value (5..8 bits) to 8 bits, its top bits repeated below.
uint expand_bits(uint v, uint total)
{
#if (BC7_SAFE & 4) != 0
    // (Shift counts under 32 also for a total the encoder never passes: both
    // sides of the choice may be computed.)
    return total >= 8u ? v : (v << ((8u - total) & 31u)) | (v >> ((2u * total - 8u) & 31u));
#else
    return total >= 8u ? v : (v << (8u - total)) | (v >> (2u * total - 8u));
#endif
}

uint decode_channel(uint code, uint bits, bool has_p, uint p)
{
    return has_p ? expand_bits((code << 1u) | p, bits + 1u) : expand_bits(code, bits);
}

// The code of `bits` bits (then the p-bit, when has_p) decoding nearest e.
uint quant_channel(float e, uint bits, bool has_p, uint p, OUT(float) err)
{
    uint total = bits + (has_p ? 1u : 0u);
    float v = e * float((1u << total) - 1u) / 255.0;
    int estimate = int(floor((has_p ? (v - float(p)) * 0.5 : v) + 0.5));
    int top = int((1u << bits) - 1u);
    uint best = 0u;
    err = NO_ERROR_YET;
#if (BC7_SAFE & 1) != 0
    // The codes next to the estimate, three every time. (For e in 0..255 the
    // estimate is within -1..top+1; held there whatever e is.)
    estimate = clamp(estimate, -1, top + 1);
    for (int step = -1; step <= 1; ++step)
    {
        uint q = uint(clamp(estimate + step, 0, top));
#else
    for (int c = estimate - 1; c <= estimate + 1; ++c)
    {
        uint q = uint(clamp(c, 0, top));
#endif
        float d = float(decode_channel(q, bits, has_p, p)) - e;
        if (d * d < err)
        {
            err = d * d;
            best = q;
        }
    }
    return best;
}

// RGB of rgb_bits and alpha of alpha_bits (0: not stored; RGB decodes 0,
// alpha 255) with the p-bit p when has_p. Returns the squared error.
float quant_endpoint(vec4 e, uint rgb_bits, uint alpha_bits, bool has_p, uint p, OUT(uvec4) code)
{
    float err = 0.0, ce;
    code = uvec4(0u);
    if (rgb_bits > 0u)
    {
        code.x = quant_channel(e.x, rgb_bits, has_p, p, ce); err += ce;
        code.y = quant_channel(e.y, rgb_bits, has_p, p, ce); err += ce;
        code.z = quant_channel(e.z, rgb_bits, has_p, p, ce); err += ce;
    }
    if (alpha_bits > 0u)
    {
        code.w = quant_channel(e.w, alpha_bits, has_p, p, ce); err += ce;
    }
    return err;
}

uvec4 decode_endpoint(uvec4 code, uint rgb_bits, uint alpha_bits, uint pmode, uint p)
{
    bool has_p = pmode != P_NONE;
    uvec4 d = uvec4(0u, 0u, 0u, 255u);
    if (rgb_bits > 0u)
    {
        d.x = decode_channel(code.x, rgb_bits, has_p, p);
        d.y = decode_channel(code.y, rgb_bits, has_p, p);
        d.z = decode_channel(code.z, rgb_bits, has_p, p);
    }
    if (alpha_bits > 0u)
    {
        d.w = decode_channel(code.w, alpha_bits, has_p, p);
    }
    return d;
}

// Both endpoints, with their p-bits as pmode has them.
void quant_pair(vec4 e0, vec4 e1, uint rgb_bits, uint alpha_bits, uint pmode,
                OUT(uvec4) c0, OUT(uvec4) c1, OUT(uint) p0, OUT(uint) p1)
{
    bool has_p = pmode != P_NONE;
    uint p_first = pmode == P_ONE ? 1u : 0u;
    uint p_last = pmode == P_EACH || pmode == P_SHARED ? 1u : p_first;
    float best0 = NO_ERROR_YET, best1 = NO_ERROR_YET, best_pair = NO_ERROR_YET;
    c0 = uvec4(0u);
    c1 = uvec4(0u);
    p0 = p_first;
    p1 = p_first;
#if (BC7_SAFE & 1) != 0
    // Both p-bit values, two passes every time; one pmode does not allow is skipped.
    for (uint p = 0u; p < 2u; ++p)
    {
        if (p < p_first || p > p_last)
        {
            continue;
        }
#else
    for (uint p = p_first; p <= p_last; ++p)
    {
#endif
        uvec4 a, b;
        float ea = quant_endpoint(e0, rgb_bits, alpha_bits, has_p, p, a);
        float eb = quant_endpoint(e1, rgb_bits, alpha_bits, has_p, p, b);
        if (pmode == P_SHARED)
        {
            if (ea + eb < best_pair)
            {
                best_pair = ea + eb;
                c0 = a; c1 = b; p0 = p; p1 = p;
            }
            continue;
        }
        if (ea < best0)
        {
            best0 = ea; c0 = a; p0 = p;
        }
        if (eb < best1)
        {
            best1 = eb; c1 = b; p1 = p;
        }
    }
}
)GLSL"
R"GLSL(
// --- Fitting ----------------------------------------------------------------

// Mean and principal direction (unit; zero when the texels are all alike) of
// the texels in mask, over the channels chan keeps (1 or 0 each).
void principal_line(uint mask, vec4 chan, OUT(vec4) mean, OUT(vec4) axis)
{
    vec4 sum = vec4(0.0);
    float n = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        if (in_mask(mask, i))
        {
            sum += g_px[i] * chan;
            n += 1.0;
        }
    }
    mean = sum / max(n, 1.0);
    // Power iteration, from the texel farthest from the mean.
    vec4 v = vec4(0.0);
    float far = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        vec4 d = g_px[i] * chan - mean;
        if (in_mask(mask, i) && dot(d, d) > far)
        {
            far = dot(d, d);
            v = d;
        }
    }
    axis = vec4(0.0);
    if (far <= 0.0)
    {
        return;
    }
    v = v / sqrt(far);
    DONT_UNROLL for (int iteration = 0; iteration < 4; ++iteration)
    {
        vec4 next = vec4(0.0);
        for (uint i = 0u; i < 16u; ++i)
        {
            if (in_mask(mask, i))
            {
                vec4 d = g_px[i] * chan - mean;
                next += d * dot(d, v);
            }
        }
        // bc7-break-fix: no break (an exit that depends on the data). NVIDIA
        // 610.88 and later hung the GPU in this loop's form with one, on blocks
        // of two values once the other modes were compiled in too (tools/bc7-probe).
        // The same v: once len is this small, the rounds left change nothing.
        float len = sqrt(dot(next, next));
        if (len > 1e-6)
        {
            v = next / len;
        }
    }
    axis = v;
}

// The ends of the texels' span along the line, clamped to 0..255.
void line_ends(uint mask, vec4 chan, vec4 mean, vec4 axis, OUT(vec4) e0, OUT(vec4) e1)
{
    float lo = NO_ERROR_YET, hi = -NO_ERROR_YET;
    for (uint i = 0u; i < 16u; ++i)
    {
        if (in_mask(mask, i))
        {
            float t = dot(g_px[i] * chan - mean, axis);
            lo = min(lo, t);
            hi = max(hi, t);
        }
    }
    e0 = clamp(mean + axis * lo, vec4(0.0), vec4(255.0));
    e1 = clamp(mean + axis * hi, vec4(0.0), vec4(255.0));
}

// Each texel of mask takes its nearest palette entry (indices of `bits` bits
// between the decoded endpoints), by the error over the channels chan keeps.
// Returns the summed squared error.
#if (BC7_SAFE & 2) != 0
float assign(uint mask, uvec4 d0, uvec4 d1, uint bits, vec4 chan, INOUTA uint idx[16])
{
    int top = int((1u << bits) - 1u);
    // The entries lie (to rounding) evenly on the line between the ends: the
    // texel's projection on it gives the nearest one, give or take one. The
    // three entries around it are made where they are tried (no palette held
    // in an array the data indexes); at the ends one of them repeats, which
    // changes nothing (only a smaller error replaces the best).
    vec4 start = vec4(d0) * chan;
    vec4 dir = (vec4(d1) - vec4(d0)) * chan;
    float len2 = dot(dir, dir);
    float total = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        if (!in_mask(mask, i))
        {
            continue;
        }
        float t = len2 > 0.0 ? clamp(dot(g_px[i] * chan - start, dir) / len2, 0.0, 1.0) : 0.0;
        int guess = clamp(int(floor(t * float(top) + 0.5)), 0, top);
        float best = NO_ERROR_YET;
        uint best_k = uint(clamp(guess - 1, 0, top));
        for (int step = -1; step <= 1; ++step)
        {
            uint k = uint(clamp(guess + step, 0, top));
            vec4 d = (palette_entry(d0, d1, weight(bits, k)) - g_px[i]) * chan;
            float e = dot(d, d);
            if (e < best)
            {
                best = e;
                best_k = k;
            }
        }
        idx[i] = best_k;
        total += best;
    }
    return total;
}
#else
float assign(uint mask, uvec4 d0, uvec4 d1, uint bits, vec4 chan, INOUTA uint idx[16])
{
    uint levels = 1u << bits;
    vec4 pal[16];
    for (uint k = 0u; k < levels; ++k)
    {
        pal[k] = palette_entry(d0, d1, weight(bits, k));
    }
    // The entries lie (to rounding) evenly on the line between the ends: the
    // texel's projection on it gives the nearest one, give or take one.
    vec4 start = vec4(d0) * chan;
    vec4 dir = (vec4(d1) - vec4(d0)) * chan;
    float len2 = dot(dir, dir);
    float top = float(levels - 1u);
    float total = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        if (!in_mask(mask, i))
        {
            continue;
        }
        float guess = len2 > 0.0 ? floor(clamp(dot(g_px[i] * chan - start, dir) / len2, 0.0, 1.0) * top + 0.5) : 0.0;
        uint first = uint(max(guess - 1.0, 0.0));
        uint last = uint(min(guess + 1.0, top));
        float best = NO_ERROR_YET;
        uint best_k = first;
        for (uint k = first; k <= last; ++k)
        {
            vec4 d = (pal[k] - g_px[i]) * chan;
            float e = dot(d, d);
            if (e < best)
            {
                best = e;
                best_k = k;
            }
        }
        idx[i] = best_k;
        total += best;
    }
    return total;
}
#endif

// Least-squares endpoints for the texels of mask, given their indices.
// Unchanged when every texel took the same weight.
void refine(uint mask, uint bits, uint idx[16], INOUT(vec4) e0, INOUT(vec4) e1)
{
    float a = 0.0, b = 0.0, c = 0.0;
    vec4 x0 = vec4(0.0), x1 = vec4(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        if (in_mask(mask, i))
        {
            float w = float(weight(bits, idx[i])) / 64.0;
            float u = 1.0 - w;
            a += u * u;
            b += u * w;
            c += w * w;
            x0 += g_px[i] * u;
            x1 += g_px[i] * w;
        }
    }
    float det = a * c - b * b;
    if (abs(det) < 1e-4)
    {
        return;
    }
    e0 = clamp((x0 * c - x1 * b) / det, vec4(0.0), vec4(255.0));
    e1 = clamp((x1 * a - x0 * b) / det, vec4(0.0), vec4(255.0));
}

// Endpoints and indices (`bits` each) for the texels of mask over the
// channels chan keeps: the best of the principal-axis fit and its
// least-squares refinements. The other texels' indices are left as they are.
// Returns the squared error.
float fit_line(uint mask, vec4 chan, uint rgb_bits, uint alpha_bits, uint pmode, uint bits,
               OUT(uvec4) code0, OUT(uvec4) code1, OUT(uint) p0, OUT(uint) p1, INOUTA uint idx[16])
{
    vec4 mean, axis, e0, e1;
    principal_line(mask, chan, mean, axis);
    line_ends(mask, chan, mean, axis, e0, e1);
    uint cur[16];
    for (uint i = 0u; i < 16u; ++i)
    {
        cur[i] = idx[i];
    }
    float best = NO_ERROR_YET;
    code0 = uvec4(0u);
    code1 = uvec4(0u);
    p0 = 0u;
    p1 = 0u;
    DONT_UNROLL for (int pass = 0; pass <= REFINE_PASSES; ++pass)
    {
        uvec4 c0, c1;
        uint q0, q1;
        quant_pair(e0, e1, rgb_bits, alpha_bits, pmode, c0, c1, q0, q1);
        float err = assign(mask, decode_endpoint(c0, rgb_bits, alpha_bits, pmode, q0),
                           decode_endpoint(c1, rgb_bits, alpha_bits, pmode, q1), bits, chan, cur);
        if (err < best)
        {
            best = err;
            code0 = c0;
            code1 = c1;
            p0 = q0;
            p1 = q1;
            for (uint i = 0u; i < 16u; ++i)
            {
                if (in_mask(mask, i))
                {
                    idx[i] = cur[i];
                }
            }
        }
        if (pass < REFINE_PASSES)
        {
            refine(mask, bits, cur, e0, e1);
        }
    }
    return best;
}

// The anchor texel's index has no top bit: when it would need one, the
// subset's endpoints swap and its indices flip.
void fix_anchor(uint mask, uint anchor, uint bits, INOUT(uvec4) c0, INOUT(uvec4) c1,
                INOUT(uint) p0, INOUT(uint) p1, INOUTA uint idx[16])
{
    uint top = (1u << bits) - 1u;
#if (BC7_SAFE & 4) != 0
    if (idx[anchor & 15u] <= (top >> 1u))
#else
    if (idx[anchor] <= (top >> 1u))
#endif
    {
        return;
    }
    uvec4 c = c0; c0 = c1; c1 = c;
    uint p = p0; p0 = p1; p1 = p;
    for (uint i = 0u; i < 16u; ++i)
    {
        if (in_mask(mask, i))
        {
            idx[i] = top - idx[i];
        }
    }
}

void put_indices(INOUT(uvec4) blk, INOUT(uint) pos, uint idx[16], uint bits, uint anchor1)
{
    for (uint i = 0u; i < 16u; ++i)
    {
        put_bits(blk, pos, idx[i], (i == 0u || i == anchor1) ? bits - 1u : bits);
    }
}
)GLSL"
R"GLSL(
// --- Modes ------------------------------------------------------------------

// Mode 6: one subset, RGBA of 7 bits and a p-bit per endpoint, 4-bit indices.
// Opaque blocks keep the p-bits at 1, so their alpha decodes to exactly 255.
uvec4 encode_mode6(bool opaque, OUT(float) err)
{
    uint idx[16];
    for (uint i = 0u; i < 16u; ++i)
    {
        idx[i] = 0u;
    }
    uvec4 c0, c1;
    uint p0, p1;
    err = fit_line(ALL_TEXELS, CHAN_RGBA, 7u, 7u, opaque ? P_ONE : P_EACH, 4u, c0, c1, p0, p1, idx);
    fix_anchor(ALL_TEXELS, 0u, 4u, c0, c1, p0, p1, idx);

    uvec4 blk = uvec4(0u);
    uint pos = 0u;
    put_bits(blk, pos, 1u << 6u, 7u);
    for (uint ch = 0u; ch < 4u; ++ch)
    {
        put_bits(blk, pos, c0[ch], 7u);
        put_bits(blk, pos, c1[ch], 7u);
    }
    put_bits(blk, pos, p0, 1u);
    put_bits(blk, pos, p1, 1u);
    put_indices(blk, pos, idx, 4u, 0u);
    return blk;
}

// Modes 5 and 4: one subset, colour and alpha each on indices of their own.
// Mode 5: RGB of 7 bits and alpha of 8, both on 2-bit indices. Mode 4 (index
// mode 1): RGB of 5 bits on 3-bit indices, alpha of 6 bits on 2-bit indices.
uvec4 encode_separate_alpha(uint mode, OUT(float) err)
{
    uint rgb_bits = mode == 5u ? 7u : 5u;
    uint alpha_bits = mode == 5u ? 8u : 6u;
    uint colour_index_bits = mode == 5u ? 2u : 3u;
    uint ci[16], ai[16];
    for (uint i = 0u; i < 16u; ++i)
    {
        ci[i] = 0u;
        ai[i] = 0u;
    }
    uvec4 c0, c1, a0, a1;
    uint unused0, unused1;
    err = fit_line(ALL_TEXELS, CHAN_RGB, rgb_bits, 0u, P_NONE, colour_index_bits, c0, c1, unused0, unused1, ci);
    err += fit_line(ALL_TEXELS, CHAN_ALPHA, 0u, alpha_bits, P_NONE, 2u, a0, a1, unused0, unused1, ai);
    fix_anchor(ALL_TEXELS, 0u, colour_index_bits, c0, c1, unused0, unused1, ci);
    fix_anchor(ALL_TEXELS, 0u, 2u, a0, a1, unused0, unused1, ai);

    uvec4 blk = uvec4(0u);
    uint pos = 0u;
    put_bits(blk, pos, 1u << mode, mode + 1u);
    put_bits(blk, pos, 0u, 2u);         // no channel rotation
    if (mode == 4u)
    {
        put_bits(blk, pos, 1u, 1u);     // index mode 1: colour on the 3-bit indices
    }
    for (uint ch = 0u; ch < 3u; ++ch)
    {
        put_bits(blk, pos, c0[ch], rgb_bits);
        put_bits(blk, pos, c1[ch], rgb_bits);
    }
    put_bits(blk, pos, a0.w, alpha_bits);
    put_bits(blk, pos, a1.w, alpha_bits);
    // The 2-bit index set comes first.
    if (mode == 5u)
    {
        put_indices(blk, pos, ci, 2u, 0u);
        put_indices(blk, pos, ai, 2u, 0u);
    }
    else
    {
        put_indices(blk, pos, ai, 2u, 0u);
        put_indices(blk, pos, ci, 3u, 0u);
    }
    return blk;
}

// Modes 1 and 7: two subsets of a shape. Mode 1: RGB of 6 bits and a p-bit
// per subset, 3-bit indices. Mode 7: RGBA of 5 bits and a p-bit per
// endpoint, 2-bit indices.
uvec4 encode_two_subsets(uint mode, uint shape, OUT(float) err)
{
    bool alpha = mode == 7u;
    uint bits = alpha ? 5u : 6u;
    uint index_bits = alpha ? 2u : 3u;
    uint pmode = alpha ? P_EACH : P_SHARED;
    vec4 chan = alpha ? CHAN_RGBA : CHAN_RGB;
#if (BC7_SAFE & 4) != 0
    shape = shape & 63u;
#endif
    uint mask1 = SHAPES2[shape];
    uint mask0 = ~mask1 & ALL_TEXELS;
    uint anchor = ANCHORS2[shape];
    uint idx[16];
    for (uint i = 0u; i < 16u; ++i)
    {
        idx[i] = 0u;
    }
    uvec4 a0, a1, b0, b1;
    uint pa0, pa1, pb0, pb1;
    err = fit_line(mask0, chan, bits, alpha ? bits : 0u, pmode, index_bits, a0, a1, pa0, pa1, idx);
    err += fit_line(mask1, chan, bits, alpha ? bits : 0u, pmode, index_bits, b0, b1, pb0, pb1, idx);
    fix_anchor(mask0, 0u, index_bits, a0, a1, pa0, pa1, idx);
    fix_anchor(mask1, anchor, index_bits, b0, b1, pb0, pb1, idx);

    uvec4 blk = uvec4(0u);
    uint pos = 0u;
    put_bits(blk, pos, 1u << mode, mode + 1u);
    put_bits(blk, pos, shape, 6u);
    uint channels = alpha ? 4u : 3u;
    for (uint ch = 0u; ch < channels; ++ch)
    {
        put_bits(blk, pos, a0[ch], bits);
        put_bits(blk, pos, a1[ch], bits);
        put_bits(blk, pos, b0[ch], bits);
        put_bits(blk, pos, b1[ch], bits);
    }
    if (alpha)
    {
        put_bits(blk, pos, pa0, 1u);
        put_bits(blk, pos, pa1, 1u);
        put_bits(blk, pos, pb0, 1u);
        put_bits(blk, pos, pb1, 1u);
    }
    else
    {
        put_bits(blk, pos, pa0, 1u);
        put_bits(blk, pos, pb0, 1u);
    }
    put_indices(blk, pos, idx, index_bits, anchor);
    return blk;
}

// --- Choosing a shape ------------------------------------------------------

// The error a subset would keep fitted by its principal line with `levels`
// index levels: the scatter of its colours off the line, the spacing of the
// levels along it (variance / (levels - 1)^2), and alpha's spread on the same
// spacing (with_alpha). n texels; colour sums s, squares d and cross products
// o (xy, xz, yz); alpha sum sa and squares da.
float subset_estimate(float n, vec3 s, vec3 d, vec3 o, float sa, float da, float levels, bool with_alpha)
{
    if (n < 1.0)
    {
        return 0.0;
    }
    float spacing = (levels - 1.0) * (levels - 1.0);
    float alpha_term = with_alpha ? max(da - sa * sa / n, 0.0) / spacing : 0.0;
    vec3 dd = d - s * s / n;
    vec3 oo = o - vec3(s.x * s.y, s.x * s.z, s.y * s.z) / n;
    float trace = dd.x + dd.y + dd.z;
    // The largest eigenvalue, nearly: the Rayleigh quotient of the column of
    // the largest diagonal entry (more power iterations did not rank better).
    vec3 v = dd.x >= dd.y && dd.x >= dd.z ? vec3(dd.x, oo.x, oo.y) :
             dd.y >= dd.z ? vec3(oo.x, dd.y, oo.z) : vec3(oo.y, oo.z, dd.z);
    float len = sqrt(dot(v, v));
    if (len <= 1e-6)
    {
        return trace / spacing + alpha_term;
    }
    v = v / len;
    vec3 mv = vec3(dd.x * v.x + oo.x * v.y + oo.y * v.z,
                   oo.x * v.x + dd.y * v.y + oo.z * v.z,
                   oo.y * v.x + oo.z * v.y + dd.z * v.z);
    float lambda = dot(v, mv);
    return max(trace - lambda, 0.0) + lambda / spacing + alpha_term;
}

// The two shapes with the smallest estimated error. The texels' products
// are taken once (around the block's mean, which keeps the sums small).
void best_shapes(float levels, bool with_alpha, OUT(uint) first, OUT(uint) second)
{
    vec4 mean = vec4(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        mean += g_px[i];
    }
    mean = mean / 16.0;
#if (BC7_SAFE & 8) != 0
    // Each texel's terms are worked out again where they are summed (a few
    // multiplications) rather than kept in five arrays of 16 per invocation.
    vec3 ts = vec3(0.0), td = vec3(0.0), to = vec3(0.0);
    float tsa = 0.0, tda = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        vec4 c = g_px[i] - mean;
        vec3 x = vec3(c.x, c.y, c.z);
        ts += x;
        td += x * x;
        to += vec3(c.x * c.y, c.x * c.z, c.y * c.z);
        tsa += c.w;
        tda += c.w * c.w;
    }
    float e_first = NO_ERROR_YET, e_second = NO_ERROR_YET;
    first = 0u;
    second = 1u;
    DONT_UNROLL for (uint shape = 0u; shape < 64u; ++shape)
    {
        uint mask = SHAPES2[shape];
        float n = 0.0, sa = 0.0, da = 0.0;
        vec3 s = vec3(0.0), d = vec3(0.0), o = vec3(0.0);
        for (uint i = 0u; i < 16u; ++i)
        {
            if (in_mask(mask, i))
            {
                vec4 c = g_px[i] - mean;
                vec3 x = vec3(c.x, c.y, c.z);
                n += 1.0;
                s += x;
                d += x * x;
                o += vec3(c.x * c.y, c.x * c.z, c.y * c.z);
                sa += c.w;
                da += c.w * c.w;
            }
        }
#else
    vec3 x[16], sq[16], cross[16];
    float a[16], asq[16];
    vec3 ts = vec3(0.0), td = vec3(0.0), to = vec3(0.0);
    float tsa = 0.0, tda = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        vec4 c = g_px[i] - mean;
        x[i] = vec3(c.x, c.y, c.z);
        sq[i] = x[i] * x[i];
        cross[i] = vec3(c.x * c.y, c.x * c.z, c.y * c.z);
        a[i] = c.w;
        asq[i] = c.w * c.w;
        ts += x[i];
        td += sq[i];
        to += cross[i];
        tsa += a[i];
        tda += asq[i];
    }
    float e_first = NO_ERROR_YET, e_second = NO_ERROR_YET;
    first = 0u;
    second = 1u;
    DONT_UNROLL for (uint shape = 0u; shape < 64u; ++shape)
    {
        uint mask = SHAPES2[shape];
        float n = 0.0, sa = 0.0, da = 0.0;
        vec3 s = vec3(0.0), d = vec3(0.0), o = vec3(0.0);
        for (uint i = 0u; i < 16u; ++i)
        {
            if (in_mask(mask, i))
            {
                n += 1.0;
                s += x[i];
                d += sq[i];
                o += cross[i];
                sa += a[i];
                da += asq[i];
            }
        }
#endif
        float err = subset_estimate(n, s, d, o, sa, da, levels, with_alpha) +
                    subset_estimate(16.0 - n, ts - s, td - d, to - o, tsa - sa, tda - da, levels, with_alpha);
        if (err < e_first)
        {
            e_second = e_first;
            second = first;
            e_first = err;
            first = shape;
        }
        else if (err < e_second)
        {
            e_second = err;
            second = shape;
        }
    }
}

)GLSL"
R"GLSL(
// --- The block ---------------------------------------------------------------

bool is_opaque()
{
    bool opaque = true;
    for (uint i = 0u; i < 16u; ++i)
    {
        opaque = opaque && g_px[i].w >= 255.0;
    }
    return opaque;
}

// A block BC7_MODES allows no mode for: mode 6 of the block's mean colour,
// every index 0.
uint flat_code(float v)
{
    return uint(clamp(floor(v * 127.0 / 255.0 + 0.5), 0.0, 127.0));
}

uvec4 flat_block()
{
    vec4 mean = vec4(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        mean += g_px[i];
    }
    mean = mean / 16.0;
    uvec4 code = uvec4(flat_code(mean.x), flat_code(mean.y), flat_code(mean.z), flat_code(mean.w));
    uvec4 blk = uvec4(0u);
    uint pos = 0u;
    put_bits(blk, pos, 1u << 6u, 7u);
    for (uint ch = 0u; ch < 4u; ++ch)
    {
        put_bits(blk, pos, code[ch], 7u);
        put_bits(blk, pos, code[ch], 7u);
    }
    put_bits(blk, pos, 1u, 1u);
    put_bits(blk, pos, 1u, 1u);
    return blk;
}

// The one-subset modes BC7_MODES allows for the block: the best of them, and
// its error in best (NO_ERROR_YET, no block, when it allows none for it).
uvec4 encode_one_subset(bool opaque, OUT(float) best)
{
    uvec4 blk = uvec4(0u);
    best = NO_ERROR_YET;
#if BC7_MODE(6)
    blk = encode_mode6(opaque, best);
#endif
#if BC7_MODE(4) || BC7_MODE(5)
    // Colour and alpha apart: blocks with alpha (opaque ones too when neither
    // mode 6 nor mode 1 is allowed).
    if (!opaque || !(BC7_MODE(6) || BC7_MODE(1)))
    {
        float err;
        uvec4 candidate;
        for (uint mode = 4u; mode <= 5u; ++mode)
        {
            if ((uint(BC7_MODES) & (1u << mode)) == 0u)
            {
                continue;
            }
            candidate = encode_separate_alpha(mode, err);
            if (err < best)
            {
                best = err;
                blk = candidate;
            }
        }
    }
#endif
    return blk;
}

// The two-subset mode BC7_MODES allows for the block (0: none): mode 1 for an
// opaque one, mode 7 for one with alpha (and an opaque one without mode 1).
uint two_subset_mode(bool opaque)
{
    if (opaque && BC7_MODE(1))
    {
        return 1u;
    }
    return BC7_MODE(7) ? 7u : 0u;
}

// The one-subset modes left more error than the two-subset ones are tried for.
bool wants_two_subsets(bool opaque, float best)
{
    return two_subset_mode(opaque) != 0u && best > (opaque ? TWO_SUBSET_MIN_ERROR_RGB : TWO_SUBSET_MIN_ERROR_RGBA);
}

// The two shapes the two-subset mode tries, by estimate.
void find_shapes(bool opaque, OUT(uint) first, OUT(uint) second)
{
    best_shapes(two_subset_mode(opaque) == 1u ? 8.0 : 4.0, !opaque, first, second);
}

// The two-subset mode on the two shapes: blk and best when one does better.
void encode_two_subset(bool opaque, uint first, uint second, INOUT(float) best, INOUT(uvec4) blk)
{
    uint mode = two_subset_mode(opaque);
    uint shapes[2];
    shapes[0] = first;
    shapes[1] = second;
    for (uint s = 0u; s < 2u; ++s)
    {
        float err;
        uvec4 candidate = encode_two_subsets(mode, shapes[s], err);
        if (err < best)
        {
            best = err;
            blk = candidate;
        }
    }
}

// The block in g_px as BC7: the mode with the least error (best: its error;
// NO_ERROR_YET for the mean colour of a block BC7_MODES allows no mode for).
uvec4 encode_block(OUT(float) best)
{
    bool opaque = is_opaque();
    uvec4 blk = encode_one_subset(opaque, best);
    if (wants_two_subsets(opaque, best))
    {
        uint first, second;
        find_shapes(opaque, first, second);
        encode_two_subset(opaque, first, second, best, blk);
    }
    return best < NO_ERROR_YET ? blk : flat_block();
}
)GLSL"
R"GLSL(
// --- The stages' work on the block in g_px (bc7-fast-start) -----------------
// BC7_STAGE's mains (by main below) read and write the blocks and the aux
// blocks around these; the CPU test runs these in the stages' order and
// compares the blocks with encode_block's. Each makes its mode's candidates
// as encode_block does; the caller keeps one only when its error is smaller,
// in the same order. (glslang leaves out the functions a shader never
// calls: the one-shader form's SPIR-V does not change by these.)

// Stage 6, the first: the block and its error so far; with no mode for it
// yet, the mean colour's block and NO_ERROR_YET (as encode_block ends).
uvec4 stage_first(OUT(float) best)
{
    bool opaque = is_opaque();
    uvec4 blk = uvec4(0u);
    best = NO_ERROR_YET;
#if BC7_MODE(6)
    blk = encode_mode6(opaque, best);
#endif
    return best < NO_ERROR_YET ? blk : flat_block();
}

// Stages 4 and 5: mode's candidate and its error, when encode_one_subset
// tries that mode for the block (colour and alpha apart: blocks with alpha,
// opaque ones too when neither mode 6 nor mode 1 is allowed); else false.
bool stage_separate_alpha(uint mode, OUT(uvec4) candidate, OUT(float) err)
{
    candidate = uvec4(0u);
    err = NO_ERROR_YET;
    if ((uint(BC7_MODES) & (1u << mode)) == 0u || (is_opaque() && (BC7_MODE(6) || BC7_MODE(1))))
    {
        return false;
    }
    candidate = encode_separate_alpha(mode, err);
    return true;
}

// The search stage: one shape's estimated error for the block, best_shapes'
// sums for that shape (its SAFE 8 form, the same order). Written again
// rather than shared with best_shapes: a shared function would change the
// one-shader form's SPIR-V, and so maybe its blocks on a GPU whose compiler
// then fuses otherwise.
float shape_error(uint shape)
{
    bool opaque = is_opaque();
    float levels = two_subset_mode(opaque) == 1u ? 8.0 : 4.0;
    bool with_alpha = !opaque;
    vec4 mean = vec4(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        mean += g_px[i];
    }
    mean = mean / 16.0;
    vec3 ts = vec3(0.0), td = vec3(0.0), to = vec3(0.0);
    float tsa = 0.0, tda = 0.0;
    for (uint i = 0u; i < 16u; ++i)
    {
        vec4 c = g_px[i] - mean;
        vec3 x = vec3(c.x, c.y, c.z);
        ts += x;
        td += x * x;
        to += vec3(c.x * c.y, c.x * c.z, c.y * c.z);
        tsa += c.w;
        tda += c.w * c.w;
    }
    uint mask = SHAPES2[shape & 63u];
    float n = 0.0, sa = 0.0, da = 0.0;
    vec3 s = vec3(0.0), d = vec3(0.0), o = vec3(0.0);
    for (uint i = 0u; i < 16u; ++i)
    {
        if (in_mask(mask, i))
        {
            vec4 c = g_px[i] - mean;
            vec3 x = vec3(c.x, c.y, c.z);
            n += 1.0;
            s += x;
            d += x * x;
            o += vec3(c.x * c.y, c.x * c.z, c.y * c.z);
            sa += c.w;
            da += c.w * c.w;
        }
    }
    return subset_estimate(n, s, d, o, sa, da, levels, with_alpha) +
           subset_estimate(16.0 - n, ts - s, td - d, to - o, tsa - sa, tda - da, levels, with_alpha);
}

// The search stage: the two shapes with the smallest errors, picked in
// shape order as best_shapes picks them. The errors are passed by value
// (the search stage's copy of its shared memory): on AMD the copy stays in
// registers (no scratch) and the whole encoder ran 9% faster than with the
// 64 errors read from shared memory one by one in the loop (tools/bc7-probe,
// 1024x1024 eight times: 291 against 317 ms; the same blocks).
void pick_shapes(float errors[64], OUT(uint) first, OUT(uint) second)
{
    float e_first = NO_ERROR_YET, e_second = NO_ERROR_YET;
    first = 0u;
    second = 1u;
    for (uint shape = 0u; shape < 64u; ++shape)
    {
        float e = errors[shape];
        if (e < e_first)
        {
            e_second = e_first;
            second = first;
            e_first = e;
            first = shape;
        }
        else if (e < e_second)
        {
            e_second = e;
            second = shape;
        }
    }
}

// Stages 1 and 7: mode's candidate on one shape and its error, when the
// block's two-subset mode is this one (encode_two_subset's); else false.
bool stage_two_subsets(uint mode, uint shape, OUT(uvec4) candidate, OUT(float) err)
{
    candidate = uvec4(0u);
    err = NO_ERROR_YET;
    if (two_subset_mode(is_opaque()) != mode)
    {
        return false;
    }
    candidate = encode_two_subsets(mode, shape, err);
    return true;
}
#ifndef BC7_CPU_TEST
// One level of an R8G8B8A8_UNORM texture into BC7 blocks, rows of blocks
// one after the other from blocks[first_block]. A dispatch takes the level's
// blocks from base on (bc7-fault: a level is split into several dispatches).
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D u_source;
#if BC7_PASS == 0 && !defined(BC7_STAGE)
layout(std430, set = 0, binding = 1) writeonly buffer Blocks { uvec4 blocks[]; };
#else
// A pass (or stage) after the first reads what the one before wrote (a barrier between them).
layout(std430, set = 0, binding = 1) buffer Blocks { uvec4 blocks[]; };
#endif
layout(push_constant) uniform Params
{
    uint level;
    uint width;         // of the level
    uint height;
    uint blocks_x;
    uint block_count;
    uint first_block;
    uint flags;
    uint base;          // the first of the level's blocks this dispatch encodes
    // The passes (bc7-fault2): the level's first aux block. One a block: the
    // error so far (its float's bits), the two shapes, and 1 while the
    // two-subset modes are still to be tried.
    uint aux_block;
} params;

// LLRHI_TEX_BCN_TINT=1: compressed textures show with less green (which ones were compressed).
const uint FLAG_TINT = 1u;

)GLSL"
R"GLSL(
#ifdef BC7_STAGE
// --- The encoder in stages (bc7-fast-start, research/88 section 13) --------
// One small pipeline a stage, each run over every level, a barrier between
// them, in this order: 6, 4, 5 (the one-subset modes), BC7_STAGE_SEARCH
// (whether a two-subset mode is tried, and on which two shapes), 1, 7 (the
// two-subset modes, each run twice: a shape a run). n: mode n's stage,
// nothing but that mode's code. The AMD driver's compiler took 8 minutes
// and over 11 GB for the whole encoder in one pipeline (most of it for
// best_shapes' loop over the 64 shapes); the stages take 1 to 5 s and at
// most 2 GB each. Between stages, the aux block of each block: the error so
// far (its float's bits), the two shapes, and 1 when a two-subset mode is to
// be tried. Every stage leaves a finished block, so the first stages alone
// are an encoder of fewer modes (the viewer starts with mode 6's while the
// others are made). The blocks are the one shader's: each stage makes its
// mode's candidates as encode_block does and keeps one only when its error
// is smaller, in the same order.
#if BC7_STAGE == BC7_STAGE_SEARCH && defined(BC7_PX_SHARED)
#error "the search stage keeps 64 invocations' texels: no room for BC7_PX_SHARED"
#endif
// Stages 1 and 7: the dispatch for the second of the two shapes.
const uint FLAG_SECOND_SHAPE = 2u;

// Block b of the level into g_px: main's loop (SAFE 4 form). Written again
// rather than shared with main, as shape_error is best_shapes' sums: a
// shared function would change the one-shader form's SPIR-V, and so maybe
// its blocks on a GPU whose compiler then fuses otherwise.
void load_block(uint b)
{
    uint blocks_x = max(params.blocks_x, 1u);
    uint last_x = max(params.width, 1u) - 1u;
    uint last_y = max(params.height, 1u) - 1u;
    uint bx = b % blocks_x;
    uint by = b / blocks_x;
    for (uint i = 0u; i < 16u; ++i)
    {
        ivec2 xy = ivec2(int(min(bx * 4u + (i & 3u), last_x)), int(min(by * 4u + (i >> 2u), last_y)));
        vec4 c = floor(texelFetch(u_source, xy, int(params.level)) * 255.0 + vec4(0.5));
        if ((params.flags & FLAG_TINT) != 0u)
        {
            c.y = floor(c.y * 0.5);
        }
        g_px[i] = c;
    }
}

#if BC7_STAGE == BC7_STAGE_SEARCH
// One block a group (base + the group's index), one shape an invocation (its
// index in the group); the group's first invocation picks the two best
// shapes (pick_shapes).
shared float s_shape_error[64];

void main()
{
    uint shape = gl_LocalInvocationIndex & 63u;
    uint b = params.base + gl_WorkGroupID.x;
    // No return before the barrier: every invocation of the group reaches it.
    bool in_level = b < params.block_count;
    bool searching = false;
    uvec4 aux = uvec4(0u);
    if (in_level)
    {
        aux = blocks[params.aux_block + b];
        float best = uintBitsToFloat(aux.x);
        // Most blocks stop here: under both thresholds, no texel is read.
        if (best > min(TWO_SUBSET_MIN_ERROR_RGB, TWO_SUBSET_MIN_ERROR_RGBA))
        {
            load_block(b);
            searching = wants_two_subsets(is_opaque(), best);
        }
    }
    s_shape_error[shape] = searching ? shape_error(shape) : NO_ERROR_YET;
    barrier();
    if (in_level && shape == 0u)
    {
        uint first = 0u;
        uint second = 1u;
        if (searching)
        {
            pick_shapes(s_shape_error, first, second);
        }
        blocks[params.aux_block + b] = uvec4(aux.x, first, second, searching ? 1u : 0u);
    }
}
#else
void main()
{
    uint b = params.base + gl_GlobalInvocationID.x;
    if (b >= params.block_count)
    {
        return;
    }
    uint out_block = params.first_block + b;
    uint aux_block = params.aux_block + b;
#if BC7_STAGE == 6
    // The first stage: the block and its error so far, whatever comes after.
    load_block(b);
    float best;
    blocks[out_block] = stage_first(best);
    blocks[aux_block] = uvec4(floatBitsToUint(best), 0u, 0u, 0u);
#else
    uvec4 aux = blocks[aux_block];
    float err;
    uvec4 candidate;
#if BC7_STAGE == 4 || BC7_STAGE == 5
    load_block(b);
    stage_separate_alpha(uint(BC7_STAGE), candidate, err);
#else
    // The blocks the search marked, on one of the two shapes: the stage runs
    // twice, the first shape, then the second (FLAG_SECOND_SHAPE), each kept
    // only when better (half the code of both in one: the compile's time and memory).
    if (aux.w == 0u)
    {
        return;     // (most blocks: no texel read)
    }
    load_block(b);
    stage_two_subsets(uint(BC7_STAGE), (params.flags & FLAG_SECOND_SHAPE) != 0u ? aux.z : aux.y, candidate, err);
#endif
    if (err < uintBitsToFloat(aux.x))
    {
        blocks[out_block] = candidate;
        blocks[aux_block] = uvec4(floatBitsToUint(err), aux.y, aux.z, aux.w);
    }
#endif
}
#endif
#else
void main()
{
    uint b = params.base + gl_GlobalInvocationID.x;
    if (b >= params.block_count)
    {
        return;
    }
    uint out_block = params.first_block + b;
#if BC7_PASS >= 2
    uvec4 aux = blocks[params.aux_block + b];
    if (aux.w == 0u)
    {
        return;     // done in the passes before
    }
#endif
#if (BC7_SAFE & 4) != 0
    // (Never 0 from the encoder; no division by 0 or wrapped edge if one were.)
    uint blocks_x = max(params.blocks_x, 1u);
    uint last_x = max(params.width, 1u) - 1u;
    uint last_y = max(params.height, 1u) - 1u;
#else
    uint blocks_x = params.blocks_x;
    uint last_x = params.width - 1u;
    uint last_y = params.height - 1u;
#endif
    uint bx = b % blocks_x;
    uint by = b / blocks_x;
    for (uint i = 0u; i < 16u; ++i)
    {
        // Texels past the level's edge (levels under 4 texels) repeat the edge.
        ivec2 xy = ivec2(int(min(bx * 4u + (i & 3u), last_x)), int(min(by * 4u + (i >> 2u), last_y)));
        vec4 c = floor(texelFetch(u_source, xy, int(params.level)) * 255.0 + vec4(0.5));
        if ((params.flags & FLAG_TINT) != 0u)
        {
            c.y = floor(c.y * 0.5);
        }
        g_px[i] = c;
    }
#if BC7_PASS == 0
    float err;
    blocks[out_block] = encode_block(err);
#elif BC7_PASS == 1
    bool opaque = is_opaque();
    float best;
    uvec4 blk = encode_one_subset(opaque, best);
    bool more = wants_two_subsets(opaque, best);
    blocks[out_block] = more || best < NO_ERROR_YET ? blk : flat_block();
    blocks[params.aux_block + b] = uvec4(floatBitsToUint(best), 0u, 0u, more ? 1u : 0u);
#elif BC7_PASS == 2 && BC7_PASSES == 3
    uint first, second;
    find_shapes(is_opaque(), first, second);
    blocks[params.aux_block + b] = uvec4(aux.x, first, second, 1u);
#else
    // The two-subset modes: pass 2 of 2 (after the shape search), pass 3 of 3
    // (on the shapes pass 2 found).
    bool opaque = is_opaque();
    float best = uintBitsToFloat(aux.x);
    uvec4 blk = blocks[out_block];
#if BC7_PASS == 2
    uint first, second;
    find_shapes(opaque, first, second);
#else
    uint first = aux.y;
    uint second = aux.z;
#endif
    encode_two_subset(opaque, first, second, best, blk);
    blocks[out_block] = best < NO_ERROR_YET ? blk : flat_block();
#endif
}
#endif
#endif
)GLSL"
