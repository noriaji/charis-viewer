/**
 * @file plus_j2c.cpp
 * @brief <plus> slj2k in front of OpenJPEG (see plus_j2c.h).
 *
 * Built for plain x64 (no /arch:AVX2, see CMakeLists.txt): it runs for every J2C decode, also on
 * CPUs without AVX2, where slj2k is never entered and OpenJPEG decodes as before.
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
#include "plus_j2c.h"

#include "slj2k.h"
#include "threadpool.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace
{
    std::atomic<S32> sMode{0};
    std::atomic<U32> sThreads{1};
    std::atomic<U32> sMinArea{1024 * 1024};
    // Bumped when the mode changes: each decode thread lets go of its workspace and pool at its
    // next decode (also when slj2k was switched off, so the memory does not stay with OpenJPEG).
    std::atomic<U32> sGeneration{0};

    // The scratch planes are large (about 30 MB for a 1024x1024 RGB image, some 19 MB at
    // KEEP_AREA), so a decode thread keeps them only for images up to KEEP_AREA pixels, and only
    // while all the decode threads together keep no more than KEEP_TOTAL_BYTES (the ImageDecode
    // pool may have many threads); otherwise they are freed after the decode.
    constexpr size_t KEEP_AREA        = 1024 * 512;
    constexpr size_t KEEP_TOTAL_BYTES = size_t(64) << 20;
    std::atomic<size_t> sKeptBytes{0};      // the kept workspaces of all the threads

    // Per decode thread.
    struct ThreadState
    {
        slj2k::Workspace workspace;
        size_t kept = 0;                    // of workspace, counted in sKeptBytes
        U32 generation = 0;
        // One small pool (the ImageDecode pool runs several decodes at once), made only when a big
        // image asks for more than one thread. Its destructor does not wait for the workers (they
        // are detached, see threadpool.h).
        std::unique_ptr<slj2k::ThreadPool> pool;
        U32 poolThreads = 0;

        ~ThreadState() { sKeptBytes -= kept; }

        void releaseWorkspace()
        {
            workspace.release();
            sKeptBytes -= kept;
            kept = 0;
        }

        // After a decode: the workspace stays when the image was small and the threads together
        // stay within the budget.
        void settleWorkspace(size_t area)
        {
            const size_t bytes = workspace.bytes();
            if (area > KEEP_AREA)
            {
                releaseWorkspace();
                return;
            }
            if (bytes > kept)
            {
                const size_t grow = bytes - kept;
                if (sKeptBytes.fetch_add(grow) + grow > KEEP_TOTAL_BYTES)
                {
                    sKeptBytes -= grow;
                    releaseWorkspace();
                    return;
                }
                kept = bytes;
            }
        }

        slj2k::ThreadPool* poolFor(U32 threads)
        {
            if (threads <= 1) return nullptr;
            if (!pool || poolThreads != threads)
            {
                pool = std::make_unique<slj2k::ThreadPool>(int(threads) - 1);
                poolThreads = threads;
            }
            return pool.get();
        }
    };

    ThreadState& threadState()
    {
        thread_local ThreadState state;
        return state;
    }

    struct OutputRequest
    {
        LLImageRaw* raw;
        S32 firstChannel;
        S32 maxChannels;
    };

    // Lets slj2k write straight into raw_image when every channel is wanted.
    U8* allocRaw(void* context, int width, int height, int channels)
    {
        OutputRequest& req = *static_cast<OutputRequest*>(context);
        if (req.firstChannel != 0 || channels > req.maxChannels || width > 0xffff || height > 0xffff || channels > 127)
        {
            return nullptr;
        }
        req.raw->resize(U16(width), U16(height), S8(channels));
        return req.raw->isBufferInvalid() ? nullptr : req.raw->getData();
    }

    U64 nowMicros()
    {
        using namespace std::chrono;
        return U64(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
    }

    std::atomic<U64> sCount[2], sMicros[2], sPixels[2];
    std::atomic<U64> sTotal{0};
    constexpr U64 LOG_EVERY = 256;

    // fix-logcache: the totals every LOG_EVERY decodes only in development
    // starts, read as LLRHI::sPeriodicLogs is (llrhi.h; this library does not
    // link llrhi): LLRHI_PERIODIC_LOGS, else LLRHI_VK_PERF or LLRHI_CUE_DIR set.
    // Otherwise at 256, 1024, 4096, ... decodes (a few lines a session).
    bool logEveryTime()
    {
        static const bool on = []()
        {
            const char* forced = std::getenv("LLRHI_PERIODIC_LOGS");
            if (forced && *forced)
            {
                return strcmp(forced, "0") != 0;
            }
            const char* perf = std::getenv("LLRHI_VK_PERF");
            const char* cue = std::getenv("LLRHI_CUE_DIR");
            return (perf && *perf) || (cue && *cue);
        }();
        return on;
    }

    bool logDue(U64 total)
    {
        if (total % LOG_EVERY != 0)
        {
            return false;
        }
        if (logEveryTime())
        {
            return true;
        }
        U64 step = total / LOG_EVERY;   // 1, 4, 16, ...: a power of 4
        return (step & (step - 1)) == 0 && (step & 0x5555555555555555ull) != 0;
    }

    // ------------------------------------------------------------------
    // screen(): what one texture's codestream may ask of a decoder. The packet headers of both
    // decoders look at every code block of a precinct for every packet, also the ones a bit or
    // two skips, so a header that asks for many layers of many tiny blocks costs hours on a
    // decode thread for a few hundred bytes of file. Textures are made by Kakadu or OpenJPEG
    // with 32x32 or 64x64 blocks and a handful of layers; the limits leave room for 4096x4096
    // RGBA with 32x32 blocks and 64 layers (about 6 million blocks looked at).
    constexpr U64 SCREEN_MAX_SIDE = MAX_IMAGE_SIZE;                     // 4096
    constexpr U64 SCREEN_MAX_COMPONENTS = MAX_IMAGE_COMPONENTS;         // 8
    constexpr U64 SCREEN_MAX_SAMPLES = U64(MAX_IMAGE_DATA_SIZE);        // 4096 x 4096 x 8
    constexpr U64 SCREEN_MAX_LAYERS = 64;
    constexpr U64 SCREEN_MAX_TILES = 4096;
    constexpr U64 SCREEN_MAX_BLOCKS = U64(1) << 19;                     // code blocks of all tiles and components
    constexpr U64 SCREEN_MAX_WORK = U64(1) << 24;                       // blocks and precincts looked at by all packets
    // Tier-1 runs every coding pass a packet header gives a block over all of the block's
    // samples, and a pass costs the same with no data behind it: a header with empty
    // contributions can ask for the most passes a band's magnitude bit planes allow (3 Mb - 2,
    // Mb at most 30 in both decoders) of every block for a few tens of KB of file. The samples
    // of each band times that bound: 2048x2048 with any count of components and planes stays
    // below the limit, as does 4096x4096 RGBA up to Mb 16 in every band (textures: 9 to 15).
    constexpr U64 SCREEN_MAX_PASS_SAMPLES = U64(3) << 30;
    constexpr U32 SCREEN_MAX_PLANES = 30;
    constexpr U32 SCREEN_BANDS = 97;                                    // 32 levels (T.800 A.6.1)

    // The costliest coding style of every COD and COC seen (any tile, any component).
    struct ScreenCoding
    {
        bool seen = false;
        U32 layers = 0;
        U32 levels = 0;
        U32 cbw = 10, cbh = 10;     // log2 of the smallest code block (10: the largest T.800 allows)
        U32 ppx = 15, ppy = 15;     // log2 of the smallest precinct (15: none given)
        U64 levelCounts = 0;        // bit N: a COD or COC with N levels (N >= 63 as 63)
        // Mb (guard bits + exponent - 1) of the QCD and QCC, by band in their order (LL, then
        // HL, LH, HH from the deepest level up), the largest of all of them.
        U8 bandPlanes[SCREEN_BANDS] = {};
        bool bandSeen[SCREEN_BANDS] = {};
        U32 maxPlanes = 0;          // of any band
        U32 allPlanes = 0;          // of every band (derived quantisation; a segment without exponents)
        bool quantSeen = false;
        U32 roiShift = 0;           // largest RGN shift (OpenJPEG decodes Mb plus it)
    };

    U32 screenBe16(const U8* p) { return (U32(p[0]) << 8) | U32(p[1]); }
    U32 screenBe32(const U8* p) { return (U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | U32(p[3]); }
    U64 screenCeilShift(U64 v, U32 shift) { return shift >= 63 ? (v ? 1 : 0) : (v + (U64(1) << shift) - 1) >> shift; }

    // SPcod / SPcoc: levels, code-block width and height, style, transform, then one precinct
    // size a resolution when `precincts`. `len` is what the data holds of it.
    void screenSPco(const U8* s, size_t len, bool precincts, ScreenCoding& c)
    {
        if (len < 3)
        {
            return;
        }
        const U32 levels = s[0];
        c.levels = llmax(c.levels, levels);
        c.levelCounts |= U64(1) << llmin<U32>(levels, 63);
        c.cbw = llmin(c.cbw, U32(s[1]) + 2);
        c.cbh = llmin(c.cbh, U32(s[2]) + 2);
        for (size_t i = 0; precincts && i <= levels && 5 + i < len; ++i)
        {
            c.ppx = llmin(c.ppx, U32(s[5 + i] & 0x0f));
            c.ppy = llmin(c.ppy, U32(s[5 + i] >> 4));
        }
        if (precincts && 5 + size_t(levels) >= len)
        {
            c.ppx = c.ppy = 0;      // sizes missing from the segment's own length: assume the smallest
        }
    }

    // Sqcd / Sqcc and the exponents after it, a band each: one byte without quantisation, two
    // bytes otherwise, the exponent in the top 5 bits of both; with derived quantisation only
    // the LL band's, the others' are no larger. `len` is what the data holds of it.
    void screenSPqc(const U8* s, size_t len, ScreenCoding& c)
    {
        if (len < 1)
        {
            return;
        }
        c.quantSeen = true;
        const U32 guard = s[0] >> 5;
        const U32 style = s[0] & 0x1f;
        const size_t step = style == 0 ? 1 : 2;
        if (len < 1 + step)
        {
            c.allPlanes = SCREEN_MAX_PLANES;    // no exponent: neither decoder can use it
            return;
        }
        for (size_t band = 0, i = 1; band < SCREEN_BANDS && i + step <= len; ++band, i += step)
        {
            const U32 sum = guard + (U32(s[i]) >> 3);
            const U32 mb = sum ? sum - 1 : 0;
            c.maxPlanes = llmax(c.maxPlanes, mb);
            if (style == 1)
            {
                c.allPlanes = llmax(c.allPlanes, mb);
                break;
            }
            c.bandPlanes[band] = U8(llmax<U32>(c.bandPlanes[band], mb));
            c.bandSeen[band] = true;
        }
    }

    // The samples of one component (w x h; with subsampling fewer) times the most passes the
    // planes of their band allow. Band i > 0 is at level N - (i - 1) / 3 of a component with N
    // levels (one more row and column for a grid that does not start at 0). Each level count a
    // COD or COC gives is a component's bands of its own: the largest of their sums is the bound
    // (not the fewest levels with the most bands, which counts bands at level 1 that no component
    // has: an image whose COC has fewer levels than its COD would be refused. research/275 L096).
    // Each tile has bands of its own, so with more than one tile every sample counts with the
    // most planes of any band.
    U64 screenPassSamples(const ScreenCoding& c, U64 w, U64 h, bool tiled)
    {
        const auto passes = [&c](U32 mb) {
            mb = c.quantSeen ? llmax(mb, c.allPlanes) : SCREEN_MAX_PLANES;
            return 3 * U64(llclamp<U32>(mb + c.roiShift, 1, SCREEN_MAX_PLANES)) - 2;
        };
        if (tiled)
        {
            return w * h * passes(c.maxPlanes);
        }
        const U64 counts = c.levelCounts ? c.levelCounts : U64(1);    // no COD: the whole image as one band
        U64 most = 0;
        for (U32 levels = 0; levels < 64; ++levels)
        {
            if (!(counts & (U64(1) << levels)))
            {
                continue;
            }
            U64 sum = 0;
            for (U32 band = 0; band <= 3 * levels && band < SCREEN_BANDS; ++band)
            {
                const U32 up = band ? (band - 1) / 3 : 0;
                const U32 level = levels - up;      // band 0 (LL) at levels; band i > 0 never below 1
                const U64 samples = (screenCeilShift(w, level) + 1) * (screenCeilShift(h, level) + 1);
                sum += llmin(samples, w * h) * passes(c.bandSeen[band] ? c.bandPlanes[band] : c.maxPlanes);
            }
            most = llmax(most, sum);
        }
        return most;
    }

    // One marker segment; `s` is its body (after the length), `len` what the data holds of it.
    void screenSegment(U32 marker, const U8* s, size_t len, U32 components, ScreenCoding& c)
    {
        const size_t compBytes = components < 257 ? 1 : 2;     // Ccoc, Cqcc, Crgn
        if (marker == 0xff5c)                 // QCD: Sqcd, SPqcd
        {
            screenSPqc(s, len, c);
            return;
        }
        if (marker == 0xff5d)                 // QCC: component, Sqcc, SPqcc
        {
            if (len > compBytes)
            {
                screenSPqc(s + compBytes, len - compBytes, c);
            }
            return;
        }
        if (marker == 0xff5e)                 // RGN: component, Srgn, SPrgn (the shift)
        {
            if (len >= compBytes + 2)
            {
                c.roiShift = llmax(c.roiShift, U32(s[compBytes + 1]));
            }
            return;
        }
        if (marker == 0xff52 && len >= 5)     // COD: Scod, progression, layers (2), MCT, SPcod
        {
            c.seen = true;
            c.layers = llmax(c.layers, screenBe16(s + 2));
            screenSPco(s + 5, len - 5, (s[0] & 1) != 0, c);
        }
        else if (marker == 0xff53)            // COC: component, Scoc, SPcoc
        {
            if (len > compBytes + 1)
            {
                screenSPco(s + compBytes + 1, len - compBytes - 1, (s[compBytes] & 1) != 0, c);
            }
        }
    }
}

namespace plus_j2c
{
    void setConfig(S32 mode, U32 threads, U32 minArea)
    {
        if (sMode.exchange(mode) != mode)
        {
            ++sGeneration;
        }
        sThreads = llclamp<U32>(threads, 1, MAX_THREADS);
        sMinArea = minArea;
    }

    bool screen(const U8* d, U32 size, const char*& why)
    {
        // SOC, SIZ: Lsiz, Rsiz, Xsiz, Ysiz, XOsiz, YOsiz, XTsiz, YTsiz, XTOsiz, YTOsiz, Csiz
        if (!d || size < 42 || screenBe16(d) != 0xff4f || screenBe16(d + 2) != 0xff51)
        {
            why = "not a codestream, or its header is cut short";
            return false;
        }
        const U64 xs = screenBe32(d + 8), ys = screenBe32(d + 12), xo = screenBe32(d + 16), yo = screenBe32(d + 20);
        const U64 xt = screenBe32(d + 24), yt = screenBe32(d + 28), xto = screenBe32(d + 32), yto = screenBe32(d + 36);
        const U32 components = screenBe16(d + 40);
        if (xo >= xs || yo >= ys || xs - xo > SCREEN_MAX_SIDE || ys - yo > SCREEN_MAX_SIDE)
        {
            why = "image size (0, or more than 4096 on a side)";
            return false;
        }
        const U64 w = xs - xo, h = ys - yo;
        if (components == 0 || components > SCREEN_MAX_COMPONENTS || w * h * components > SCREEN_MAX_SAMPLES)
        {
            why = "component count";
            return false;
        }
        // T.800 A.5.1: the first tile holds the image's first sample
        if (xt == 0 || yt == 0 || xto > xo || yto > yo || xto + xt <= xo || yto + yt <= yo)
        {
            why = "tiling";
            return false;
        }
        const U64 tiles = ((xs - xto + xt - 1) / xt) * ((ys - yto + yt - 1) / yt);
        if (tiles > SCREEN_MAX_TILES)
        {
            why = "more than 4096 tiles";
            return false;
        }

        // The coding styles of the main header, then of every tile-part header after it.
        // Coded data never holds 0xFF followed by a byte above 0x8F (T.800 A.1, B.10.1), so
        // the tile-parts are found by their SOT markers (0xFF90), not by Psot: a wrong Psot
        // would hide a tile-part a decoder finds again by its markers. A COD (0xFF52) can be
        // told from coded data only inside a header, whose segments are read by their lengths.
        ScreenCoding coding;
        size_t pos = 4 + size_t(screenBe16(d + 4));     // past SIZ
        while (pos + 4 <= size)
        {
            const U32 marker = screenBe16(d + pos);
            const U32 len = screenBe16(d + pos + 2);
            if ((marker & 0xff00) != 0xff00 || marker == 0xff90 || marker == 0xff93 || marker == 0xffd9 || len < 2)
            {
                break;      // the first tile-part (SOT), or the main header ends here
            }
            if (pos + 2 + size_t(len) > size)
            {
                pos = size; // cut off (a download in progress): neither decoder applies a segment it lacks
                break;      // the end of, and nothing comes after it yet
            }
            screenSegment(marker, d + pos + 4, llmin<size_t>(len - 2, size - pos - 4), components, coding);
            pos += 2 + size_t(len);
        }
        while (pos + 4 <= size)
        {
            if (d[pos] != 0xff || d[pos + 1] != 0x90)
            {
                ++pos;
                continue;
            }
            // SOT (Lsot 10: Isot, Psot, TPsot, TNsot), then segments up to SOD
            size_t q = pos + 12;
            while (q + 4 <= size && screenBe16(d + q) != 0xff93)
            {
                const U32 marker = screenBe16(d + q);
                const U32 len = screenBe16(d + q + 2);
                if ((marker & 0xff00) != 0xff00 || marker == 0xff90 || len < 2)
                {
                    break;
                }
                if (q + 2 + size_t(len) > size)
                {
                    break;      // a segment cut off, as above
                }
                screenSegment(marker, d + q + 4, llmin<size_t>(len - 2, size - q - 4), components, coding);
                q += 2 + size_t(len);
            }
            // fix-round7: on from just past this SOT, not from q: a false SOT in coded data with a
            // long "segment" would carry the scan past a real SOT and its header, which a decoder
            // finds by Psot. Bytes of a real header seen again only make the screen stricter.
            pos += 2;
        }
        if (!coding.seen)
        {
            why = "no coding style (COD) in the header";
            return false;
        }
        if (coding.layers > SCREEN_MAX_LAYERS)
        {
            why = "more than 64 quality layers";
            return false;
        }

        // Code blocks and precincts of one tile and component, with the costliest style (a tile
        // that is not on the block grid has one more row and column of each).
        const U64 tw = llmin(xt, w), th = llmin(yt, h);
        const U32 levels = coding.levels;
        U64 blocks = 0, precincts = 0;
        for (U32 r = 0; r <= levels; ++r)
        {
            const U64 rw = screenCeilShift(tw, levels - r), rh = screenCeilShift(th, levels - r);
            precincts += (screenCeilShift(rw, coding.ppx) + 1) * (screenCeilShift(rh, coding.ppy) + 1);
            // a code block is no larger than its precinct, at r > 0 than its half (T.800 B.7)
            const U32 cbw = llmin(coding.cbw, r ? (coding.ppx ? coding.ppx - 1 : 0) : coding.ppx);
            const U32 cbh = llmin(coding.cbh, r ? (coding.ppy ? coding.ppy - 1 : 0) : coding.ppy);
            const U64 bw = r ? screenCeilShift(rw, 1) : rw, bh = r ? screenCeilShift(rh, 1) : rh;
            blocks += (r ? 3 : 1) * (screenCeilShift(bw, cbw) + 1) * (screenCeilShift(bh, cbh) + 1);
            if (blocks > SCREEN_MAX_BLOCKS)
            {
                break;      // refused below; no product that could overflow
            }
        }
        const U64 units = tiles * components;   // at most 4096 x 8
        if (blocks > SCREEN_MAX_BLOCKS || blocks * units > SCREEN_MAX_BLOCKS)
        {
            why = "too many code blocks";
            return false;
        }
        if ((blocks + precincts) * units * llmax<U64>(coding.layers, 1) > SCREEN_MAX_WORK)
        {
            why = "too many code blocks and packets for its quality layers";
            return false;
        }
        if (screenPassSamples(coding, w, h, tiles > 1) * components > SCREEN_MAX_PASS_SAMPLES)
        {
            why = "too many coding passes for its size";
            return false;
        }
        return true;
    }

    bool decode(const U8* data, U32 size, S32 discard, LLImageRaw& raw_image, S32 first_channel, S32 max_channel_count)
    {
        // Every J2C decode comes here, so a thread lets go of what slj2k kept on its next decode
        // after the mode changed (switched off: OpenJPEG decodes from then on).
        ThreadState& state = threadState();
        const U32 generation = sGeneration.load();
        if (state.generation != generation)
        {
            state.generation = generation;
            state.releaseWorkspace();
            state.pool.reset();
            state.poolThreads = 0;
        }

        if (sMode.load() != 1 || !data || size < 24 || discard < 0 || first_channel < 0 || max_channel_count < 1 || !slj2k::cpuSupported())
        {
            return false;
        }

        OutputRequest request{&raw_image, first_channel, max_channel_count};
        slj2k::Options opt;
        opt.discard = discard;
        opt.workspace = &state.workspace;
        opt.allocOutput = &allocRaw;
        opt.allocContext = &request;

        // More than one thread only for big images. The size is read from the SIZ marker
        // (SOC, SIZ, Lsiz, Rsiz, then Xsiz and Ysiz at offsets 8 and 12).
        U32 threads = sThreads.load();
        if (threads > 1 && data[0] == 0xff && data[1] == 0x4f && data[2] == 0xff && data[3] == 0x51)
        {
            auto be32 = [data](int o) { return (U32(data[o]) << 24) | (U32(data[o + 1]) << 16) | (U32(data[o + 2]) << 8) | U32(data[o + 3]); };
            const U64 area = (U64(be32(8)) * U64(be32(12))) >> (2 * llmin(discard, 15));
            if (area < sMinArea.load()) threads = 1;
        }
        else
        {
            threads = 1;
        }
        slj2k::Image out;
        slj2k::Status st = slj2k::Status::NoMemory;
        try
        {
            opt.pool = state.poolFor(threads);
            st = slj2k::decode(data, size, opt, out);
        }
        catch (const std::exception& e)
        {
            LL_WARNS("Texture") << "slj2k threw " << e.what() << "; using OpenJPEG" << LL_ENDL;
        }
        if (st != slj2k::Status::Ok)
        {
            if (st == slj2k::Status::NoMemory) state.releaseWorkspace(); // leave the memory to OpenJPEG
            LL_DEBUGS("Texture") << "slj2k: falling back to OpenJPEG (status " << S32(st) << ": " << slj2k::lastUnsupportedReason() << ")" << LL_ENDL;
            return false;
        }
        state.settleWorkspace(size_t(out.width) * out.height);

        if (!out.inExternalBuffer)
        {
            // channel selection (or a size raw_image cannot take): copy what was asked for
            const S32 channels = llmin(S32(out.channels) - first_channel, max_channel_count);
            if (channels <= 0 || out.width > 0xffff || out.height > 0xffff)
            {
                return false;
            }
            raw_image.resize(U16(out.width), U16(out.height), S8(channels));
            U8* dst = raw_image.getData();
            if (!dst)
            {
                return false;
            }
            const U8* src = out.pixels.data() + first_channel;
            const size_t count = size_t(out.width) * out.height;
            if (channels == 1)
            {
                for (size_t i = 0; i < count; i++, src += out.channels) dst[i] = *src;
            }
            else
            {
                for (size_t i = 0; i < count; i++, src += out.channels, dst += channels)
                {
                    for (S32 c = 0; c < channels; c++) dst[c] = src[c];
                }
            }
        }
        return true;
    }

    Timer::Timer(const LLImageRaw& raw) : mRaw(raw), mStart(nowMicros()) {}

    Timer::~Timer()
    {
        const U32 which = mSlj2k ? 1 : 0;
        sCount[which]++;
        sMicros[which] += nowMicros() - mStart;
        sPixels[which] += U64(mRaw.getWidth()) * mRaw.getHeight();
        if (logDue(++sTotal))
        {
            const auto avgMs = [](U32 w) { return sCount[w] ? F64(sMicros[w]) / F64(sCount[w]) / 1000.0 : 0.0; };
            const auto mpixPerSec = [](U32 w) { return sMicros[w] ? F64(sPixels[w]) / F64(sMicros[w]) : 0.0; };
            LL_INFOS("Plus") << "J2C decode so far: OpenJPEG n=" << sCount[0] << " avg " << avgMs(0) << " ms (" << mpixPerSec(0)
                             << " Mpix/s); slj2k n=" << sCount[1] << " avg " << avgMs(1) << " ms (" << mpixPerSec(1) << " Mpix/s)" << LL_ENDL;
        }
    }
}
