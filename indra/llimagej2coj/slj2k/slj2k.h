/**
 * @file slj2k.h
 *
 * slj2k is Charis Viewer's own JPEG 2000 decoder, written from ITU-T T.800; it
 * holds no OpenJPEG source code. Where it follows OpenJPEG (constants and order
 * of operations, so that the pixels agree), the comments say so
 * (licenses-charis.txt, section 1).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

// slj2k: a JPEG 2000 decoder restricted to what Second Life textures use.
//
// Supported: single tile, 8-bit unsigned components (1-8), LRCP/RLCP/RPCL/PCRL/CPRL with one
// precinct per resolution, no SOP/EPH, default code-block style, 5/3 reversible (qstyle 0) or
// 9/7 irreversible (qstyle 1/2) wavelets, RCT/ICT, resolution reduction (discard levels) and
// truncated codestreams.  Anything else returns Status::Unsupported so the caller can fall back.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace slj2k
{

class ThreadPool;

// Scratch memory that can be kept between decodes: coefficient planes and wavelet buffers are
// large, and re-using them avoids page faults on every call.  One Workspace per calling thread;
// it is not thread safe.
class Workspace
{
public:
    Workspace() = default;
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    ~Workspace();
    // 64-byte aligned block of at least `bytes` for the given slot; nullptr when the allocation
    // fails.  The contents are not cleared (left from an earlier decode, or never written): the
    // decoder writes every sample before reading it and clears the row padding itself.
    void* get(int slot, size_t bytes);
    void release();
    // The bytes held now (all slots).
    size_t bytes() const;

private:
    struct Block
    {
        void* p = nullptr;
        size_t bytes = 0;
    };
    Block mBlocks[32];
};

enum class Status
{
    Ok,
    Unsupported,  // valid stream using a feature this decoder does not implement
    Corrupt,      // header unreadable or no usable data
    NoMemory      // image too large for the scratch memory
};

struct Options
{
    int discard = 0;            // number of highest resolution levels to skip
    ThreadPool* pool = nullptr; // optional: parallel tier-1 / wavelet / output
    Workspace* workspace = nullptr;  // optional: reuse scratch memory between calls
    // Optional destination for the pixels (width * height * channels bytes, known from the header
    // and `discard`); when it is too small or null, Image::pixels is resized instead.
    uint8_t* outputBuffer = nullptr;
    size_t outputCapacity = 0;
    // Optional: called once the output size is known, right before the pixels are written; returns
    // the destination (width * height * channels bytes) or nullptr to use Image::pixels instead.
    // Never called when decode() fails, so the caller's buffer is untouched in that case.
    uint8_t* (*allocOutput)(void* context, int width, int height, int channels) = nullptr;
    void* allocContext = nullptr;
};

// Output in the layout llimagej2coj hands to LLImageRaw: interleaved, bottom row first.
struct Image
{
    int width = 0, height = 0, channels = 0;
    std::vector<uint8_t> pixels;  // empty when Options::outputBuffer received the data
    bool inExternalBuffer = false;
};

// Wall-clock split of one decode() call, in milliseconds.
struct StageTimes
{
    double parse = 0;    // marker parsing and structure setup
    double tier2 = 0;    // packet headers
    double tier1 = 0;    // code-block decoding and dequantisation
    double dwt = 0;      // inverse wavelet
    double output = 0;   // inverse component transform, level shift, interleave
};

// Why the last decode() on this thread returned Unsupported (diagnostics only).
const char* lastUnsupportedReason();

// True when the CPU and the OS can run the AVX2 kernels (AVX2, FMA, BMI1/2, LZCNT, YMM state
// saved).  decode() returns Unsupported without touching them otherwise.
bool cpuSupported();

Status decode(const uint8_t* data, size_t size, const Options& opt, Image& out, StageTimes* times = nullptr);

}  // namespace slj2k
