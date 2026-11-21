// The BC7 verdict's block rule (research/395), included by llvktexcompress.cpp
// as a C++ string and built as C++ by the CPU test (tools/bc7-encoder-test:
// verdict_test.py): plain unsigned arithmetic only, so both see the same rule.
R"GLSL(
// One 4x4 block of the check: n texels inside the level (1..16), err the sum
// of the squared differences between the source and the decoded block over
// the 4n values (0..255), sq the sum of the source's squares and s0..s3 the
// sum of each channel. n * variance = n * sq - (s0*s0 + s1*s1 + s2*s2 + s3*s3);
// at most 16 * 4 * 255^2 < 2^32 in every term.
uint block_n_variance(uint n, uint sq, uint s0, uint s1, uint s2, uint s3)
{
    return n * sq - (s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
}

// Bad: the decoded block is worse than the source block painted with its
// own mean colour (plus 8 per value for the quantisation of a good one).
// A sound encoder always does better (a line of endpoints with 4-bit
// indices); a wrong block (saturated primaries, garbage) does not.
bool block_is_bad(uint n, uint err, uint sq, uint s0, uint s1, uint s2, uint s3)
{
    return n * err > block_n_variance(n, sq, s0, s1, s2, s3) + 32u * n * n;
}

// Flat-like: the block has contrast (variance over 64 a texel) and the decoded
// one keeps under a tenth of it: close to a mean-colour block (a counted
// signal, not a verdict by itself).
bool block_is_flat_like(uint n, uint err, uint sq, uint s0, uint s1, uint s2, uint s3)
{
    const uint v = block_n_variance(n, sq, s0, s1, s2, s3);
    return v > 64u * n * n && 10u * n * err >= 9u * v;
}
)GLSL"
