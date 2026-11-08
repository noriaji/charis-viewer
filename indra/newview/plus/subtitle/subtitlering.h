/**
 * @file subtitlering.h
 * @brief Subtitles (plus/subtitle, research/137): what the voice playout tap hands over, as 16 kHz mono.
 *
 * No viewer dependencies: subtitle_test.cpp builds this on its own.
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

#ifndef PLUS_SUBTITLE_RING_H
#define PLUS_SUBTITLE_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace plus
{
    namespace subtitle
    {
        constexpr int RATE = 16000;  // what the recognizer wants

        // One writer thread (the audio device's), one reader thread. Lock-free, never allocates after
        // construction. A full ring refuses new samples (the reader fell behind): they are counted.
        class Ring
        {
        public:
            explicit Ring(size_t capacity) : mBuf(capacity + 1) {}

            // Writer only. Returns how many samples were kept.
            size_t write(const int16_t* src, size_t n)
            {
                const size_t size = mBuf.size();
                const size_t head = mHead.load(std::memory_order_relaxed);
                const size_t tail = mTail.load(std::memory_order_acquire);
                const size_t room = (tail + size - head - 1) % size;
                const size_t take = n < room ? n : room;
                for (size_t i = 0; i < take; ++i)
                {
                    mBuf[(head + i) % size] = src[i];
                }
                mHead.store((head + take) % size, std::memory_order_release);
                if (take < n)
                {
                    mRefused.fetch_add(n - take, std::memory_order_relaxed);
                }
                return take;
            }

            // Reader only. Returns how many samples were copied (at most max).
            size_t read(int16_t* dst, size_t max)
            {
                const size_t size = mBuf.size();
                const size_t tail = mTail.load(std::memory_order_relaxed);
                const size_t head = mHead.load(std::memory_order_acquire);
                const size_t have = (head + size - tail) % size;
                const size_t take = have < max ? have : max;
                for (size_t i = 0; i < take; ++i)
                {
                    dst[i] = mBuf[(tail + i) % size];
                }
                mTail.store((tail + take) % size, std::memory_order_release);
                return take;
            }

            // Reader only: throw everything away (a new session starts from now).
            void clear()
            {
                mTail.store(mHead.load(std::memory_order_acquire), std::memory_order_release);
            }

            size_t refused() const { return mRefused.load(std::memory_order_relaxed); }

        private:
            std::vector<int16_t> mBuf;
            std::atomic<size_t> mHead{ 0 };
            std::atomic<size_t> mTail{ 0 };
            std::atomic<size_t> mRefused{ 0 };
        };

        // Interleaved 16 bit of any channel count and rate -> 16 kHz mono, one buffer at a time. Linear
        // interpolation: enough for speech. Keeps the fraction between calls, so the buffer size does not
        // matter. Writer thread only; no allocation when the output fits (out_capacity).
        class Converter
        {
        public:
            // Returns how many samples were written to out (at most out_capacity; the rest of the
            // input is dropped, which only happens if the caller sized out too small).
            size_t convert(const int16_t* in, size_t frames, int channels, int rate, int16_t* out, size_t out_capacity)
            {
                if (frames == 0 || channels <= 0 || rate <= 0)
                {
                    return 0;
                }
                if (rate != mRate)
                {
                    // device changed: start the conversion over
                    mRate = rate;
                    mPos = 1.0;  // the first output is the first input sample, not the one before it
                    mPrev = 0.0f;
                }
                const double step = double(rate) / RATE;
                size_t n = 0;
                // ext(0) is the last sample of the previous call, ext(i) is the mono mix of frame i-1
                auto mono = [&](size_t frame) {
                    int sum = 0;
                    for (int c = 0; c < channels; ++c)
                    {
                        sum += in[frame * channels + c];
                    }
                    return float(sum) / float(channels);
                };
                auto ext = [&](size_t i) { return i == 0 ? mPrev : mono(i - 1); };
                while (n < out_capacity)
                {
                    const size_t i = size_t(mPos);
                    if (i + 1 > frames)
                    {
                        break;
                    }
                    const double f = mPos - double(i);
                    const float v = float(ext(i) * (1.0 - f) + ext(i + 1) * f);
                    out[n++] = int16_t(v > 32767.0f ? 32767.0f : (v < -32768.0f ? -32768.0f : v));
                    mPos += step;
                }
                mPos -= double(frames);
                if (mPos < 0.0)
                {
                    mPos = 0.0;
                }
                mPrev = mono(frames - 1);
                return n;
            }

        private:
            int mRate = 0;
            double mPos = 1.0;
            float mPrev = 0.0f;
        };
    }
}

#endif // PLUS_SUBTITLE_RING_H
