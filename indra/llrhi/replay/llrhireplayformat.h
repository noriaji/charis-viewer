/**
 * @file llrhireplayformat.h
 * @brief research/271: the capture file's binary format (.crp), and a writer and reader for it.
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

#ifndef LL_LLRHIREPLAYFORMAT_H
#define LL_LLRHIREPLAYFORMAT_H

// research/271 P1b stage 2 (first entry): one call into a recorded llrhi entry point (research/271
// 2's boundary) is one record: which entry (by the small id this file's caller assigns; the
// generator's manifest, indra/llrhi/replay/entry_manifest.json, will assign these once more than
// one entry is wired), which thread, where in the call order, and its argument bytes (the
// caller already flattened pointers and arrays into them: research/271 3.2-3's rule "a type
// table of how many bytes to copy" applies before a record reaches here, not in this file).
//
// A frame boundary (present_frame, research/271 3.2-4) is its own record with no payload, so a
// reader can tell where one frame's calls end without re-deriving it from the entries recorded.
//
// This is deliberately small: no resource dump, no header fields beyond a magic and a version,
// no identity table (research/271 3.3) yet. Those are for the next entries wired (research/271
// 271-frame-replay-design.md's stage 9 table, P1c); this format only needs to grow a new field
// after what is here, never change what is here, so an old capture stays readable (the version
// is there so a reader can still tell when it cannot).
//
// Not yet handled here, carried over from the design until more entries are wired:
//  - research/271 3.3: names/numbers written in a record (a GL name, a TexRef) are not yet
//    translated through an identity table on replay; a record's bytes are read back as given.
//  - research/271 3.6 point 9 (M4): a record's sequence number must be taken only after the
//    real entry point has been called (not before), so two threads racing for the same number
//    cannot deadlock against a lock a called-through entry point might itself take. This file's
//    Writer does not take any lock of its own; the caller (llrhireplaycapture.h) owns that.
//    So with two threads the records are in the file in the order they got the caller's lock,
//    which is not always their sequence order (a number is taken before the lock): a reader
//    that replays them puts them in sequence order first (Reader hands them out as written;
//    charis-replay --dump shows that order).

#include "linden_common.h"

#include "llfile.h"   // llifstream/llofstream: a plain std::fstream mishandles a non-ASCII
                      // path on Windows (llfile.h's own comment; realstarcat.h hit this too)

#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace LLRHIReplay
{
    // Bumped whenever a record's shape changes in a way an old reader could not skip past
    // (adding a new entry id needs no bump: unknown ids are read as "count of bytes", skippable).
    constexpr U32 FORMAT_VERSION = 1;
    constexpr char MAGIC[4] = { 'C', 'R', 'P', '1' };

    // Reserved entry id: a frame boundary (present_frame, research/271 3.2-4). No payload.
    constexpr U32 ENTRY_FRAME_BOUNDARY = 0xfffffffeu;

    // The U64 fields first and the two U32 fields last: 24 bytes with no padding between or
    // after them (two 8-byte fields need no alignment padding before them, and 4+4 bytes of
    // U32 fill the rest exactly), so the aggregate-initialized record() below leaves no
    // uninitialized padding bytes to write to the file (a plain memcpy of a struct with
    // mid-struct padding could otherwise carry a few bytes of whatever was on the stack).
    struct RecordHeader
    {
        U64 sequence = 0;         // research/271 3.6 M4: taken after calling through, not before
        U64 thread_hash = 0;      // std::hash<std::thread::id>; research/271 3.3b's per-thread
                                  // table (a small index, not a hash) is for when replay runs
                                  // more than one thread, not yet built
        U32 entry_id = 0;         // research/271 2's boundary; ENTRY_FRAME_BOUNDARY for a frame end
        U32 payload_bytes = 0;
    };
    // The promise above, held at compile time: the file's records are these bytes as they are.
    static_assert(sizeof(RecordHeader) == 24, "RecordHeader: 24 bytes, no padding");
    static_assert(offsetof(RecordHeader, sequence) == 0 && offsetof(RecordHeader, thread_hash) == 8 &&
                      offsetof(RecordHeader, entry_id) == 16 && offsetof(RecordHeader, payload_bytes) == 20,
                  "RecordHeader: the fields where the format has them");

    // Writes records to a file as they come, through two buffers of FLUSH_BYTES: one filled
    // while a thread of the writer's own writes the other out (review replay-p0 low: the render
    // thread no longer writes 4 MB to the disk under the capture's lock), so memory stays bounded
    // however long a capture runs, and a record only waits while the disk is slower than the
    // capture. Without the thread (it could not be started) the buffer is written where it fills.
    // The file holds whole records only up to the size limit: a record that would take it past
    // max_bytes is refused (record() returns false, limitReached() is true) and the caller
    // stops; what is in the file then is a complete, readable capture of everything before it.
    // The file is truncated when it opens (not ios::app): a path reused from an earlier capture
    // must not get the new one appended after the old one, which Reader could not tell apart
    // from a single, longer capture.
    // Not thread-safe: the caller serializes writes (research/271 3.6 M4).
    class Writer
    {
    public:
        static constexpr size_t FLUSH_BYTES = 4u << 20;

        // Never throws: a buffer that cannot be had, or a file that cannot be opened, leaves
        // ok() false, and every record() then returns false.
        Writer(const std::string& path, U64 max_bytes) : mMaxBytes(max_bytes)
        {
            try
            {
                mBuffer.reserve(FLUSH_BYTES);
                mWriting.reserve(FLUSH_BYTES);
            }
            catch (const std::bad_alloc&)
            {
                mFailed = true;
                return;
            }
            mFile.open(path, std::ios::binary | std::ios::trunc);
            if (!mFile)
            {
                mFailed = true;
                return;
            }
            const U32 version = FORMAT_VERSION;
            append(MAGIC, sizeof(MAGIC));
            append(&version, sizeof(version));
            try
            {
                mThread = std::thread([this] { writeLoop(); });
            }
            catch (...)
            {
                // (system_error, bad_alloc) written where the buffer fills instead, as before the thread
            }
        }

        ~Writer()
        {
            close();
        }

        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // False: nothing more can be recorded (the file did not open, a write failed, memory ran
        // out, the size limit was reached, or it is closed). The two below tell the size limit
        // and running out of memory apart from a write error (close() still writes out every
        // whole record buffered before either of those).
        bool ok() const { return !mFailed && !mOutOfMemory && !mClosed && !mLimitReached; }
        bool limitReached() const { return mLimitReached; }
        bool outOfMemory() const { return mOutOfMemory; }

        // False when the record was not written (see ok()); the caller stops capturing then.
        bool record(U32 entry_id, U64 sequence, U64 thread_hash, const void* payload, U32 payload_bytes)
        {
            if (!ok())
            {
                return false;
            }
            const U64 bytes = sizeof(RecordHeader) + (U64)payload_bytes;
            if (mBytes + bytes > mMaxBytes)
            {
                mLimitReached = true;
                return false;
            }
            if (mBuffer.size() + bytes > mBuffer.capacity() && !flush())
            {
                return false;
            }
            const RecordHeader header{ sequence, thread_hash, entry_id, payload_bytes };
            const size_t before = mBuffer.size();
            if (!append(&header, sizeof(header)) || (payload_bytes && !append(payload, payload_bytes)))
            {
                mBytes -= mBuffer.size() - before;
                mBuffer.resize(before);   // no half record left behind (shrinking does not throw)
                return false;
            }
            ++mRecords;
            return true;
        }

        bool frameBoundary(U64 sequence, U64 thread_hash)
        {
            return record(ENTRY_FRAME_BOUNDARY, sequence, thread_hash, nullptr, 0);
        }

        U64 recordCount() const { return mRecords; }

        // Writes what is still buffered and closes the file. Safe to call more than once (the
        // second call returns the first one's result). False if any write failed, including the
        // last flush the stream does as it closes.
        bool close()
        {
            if (!mClosed)
            {
                mClosed = true;
                if (!mFailed)   // the file opened (and nothing failed): mFile is the thread's while it writes
                {
                    flush();
                }
                stopThread();   // after the last buffer it was handed is written
                if (mFile.is_open())
                {
                    mFile.close();
                    if (mFile.fail())
                    {
                        mFailed = true;
                    }
                }
            }
            return !mFailed;
        }

    private:
        // The buffer to the file: handed to the thread once the one handed before is written
        // (the two swap, their room kept), or written here without the thread. False: a write
        // failed (this one without the thread, an earlier one with it).
        bool flush()
        {
            if (mBuffer.empty())
            {
                return true;
            }
            if (!mThread.joinable())
            {
                mFile.write(reinterpret_cast<const char*>(mBuffer.data()), (std::streamsize)mBuffer.size());
                mBuffer.clear();
                if (!mFile)
                {
                    mFailed = true;
                    return false;
                }
                return true;
            }
            {
                std::unique_lock<std::mutex> lock(mIoMutex);
                mIoDone.wait(lock, [this] { return !mHanded; });
                if (mWriteFailed)
                {
                    mFailed = true;
                    return false;
                }
                mBuffer.swap(mWriting);     // mBuffer: the one just written, empty
                mHanded = true;
            }
            mIoWork.notify_one();
            return true;
        }

        void writeLoop()
        {
            std::unique_lock<std::mutex> lock(mIoMutex);
            for (;;)
            {
                mIoWork.wait(lock, [this] { return mHanded || mStop; });
                if (!mHanded)
                {
                    return;     // stopped, everything handed written
                }
                lock.unlock();
                // Only this thread reads mWriting and the file while mHanded is true.
                mFile.write(reinterpret_cast<const char*>(mWriting.data()), (std::streamsize)mWriting.size());
                const bool written = !mFile.fail();
                lock.lock();
                mWriteFailed = mWriteFailed || !written;
                mWriting.clear();
                mHanded = false;
                mIoDone.notify_all();
            }
        }

        void stopThread()
        {
            if (!mThread.joinable())
            {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mIoMutex);
                mStop = true;
            }
            mIoWork.notify_one();
            mThread.join();
            if (mWriteFailed)
            {
                mFailed = true;
            }
        }

        // The buffer was reserved up front and flushed before it would overflow, so this only
        // grows it for a record larger than FLUSH_BYTES (none so far); a growth that fails stops
        // the capture instead of throwing into the caller (the render thread, for sDirectDraw).
        bool append(const void* data, size_t bytes)
        {
            const U8* p = reinterpret_cast<const U8*>(data);
            try
            {
                mBuffer.insert(mBuffer.end(), p, p + bytes);
            }
            catch (const std::bad_alloc&)
            {
                mOutOfMemory = true;
                return false;
            }
            mBytes += bytes;
            return true;
        }

        llofstream mFile;
        std::vector<U8> mBuffer;    // being filled
        U64 mBytes = 0;             // every byte taken (written, being written, buffered): what the size limit counts
        U64 mMaxBytes = 0;
        U64 mRecords = 0;
        bool mFailed = false;       // the file did not open, or a write to it failed
        bool mOutOfMemory = false;
        bool mLimitReached = false;
        bool mClosed = false;

        // The writing thread's: under mIoMutex, but mWriting and the file while mHanded are the thread's alone.
        std::thread mThread;
        std::mutex mIoMutex;
        std::condition_variable mIoWork;    // a buffer handed, or stop
        std::condition_variable mIoDone;    // the buffer handed is written
        std::vector<U8> mWriting;           // handed to the thread (empty, its room kept, when not)
        bool mHanded = false;
        bool mWriteFailed = false;
        bool mStop = false;
    };

    // Reads records back in the order Writer wrote them. next() returns the header and leaves
    // the payload for takePayload() (called at most once per record, before the next next()),
    // or skips it (by seeking) when not taken. A file is outside data (it may be damaged or made
    // up): every size read from it is checked against what is left of the file before it is
    // used, so no allocation or seek can be larger than the file itself.
    class Reader
    {
    public:
        explicit Reader(const std::string& path) : mFile(path, std::ios::binary)
        {
            if (!mFile || !mFile.seekg(0, std::ios::end))
            {
                return;
            }
            const std::streamoff size = (std::streamoff)mFile.tellg();
            if (size < 0 || !mFile.seekg(0, std::ios::beg))
            {
                return;
            }
            mSize = (U64)size;
            char magic[sizeof(MAGIC)];
            U32 version = 0;
            if (!mFile.read(magic, sizeof(magic)) || std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 ||
                !mFile.read(reinterpret_cast<char*>(&version), sizeof(version)) || version != FORMAT_VERSION)
            {
                mFile.setstate(std::ios::failbit);
                return;
            }
            mOffset = sizeof(MAGIC) + sizeof(version);
            mOk = true;
        }

        // Only valid once the file opened and its header checked out; false afterwards once a
        // record was found cut short or damaged.
        bool ok() const { return mOk; }

        // False at the end of the file, with ok() still true when the file ended cleanly after
        // a whole record, false when it ended partway through one (or on a read error). A
        // payload the caller did not take (takePayload()) after the previous next() is skipped
        // here.
        bool next(RecordHeader& out)
        {
            if (!mOk)
            {
                return false;
            }
            if (mPendingPayload)
            {
                // next() checked this payload fits in the file before handing its header out.
                if (!mFile.seekg((std::streamoff)mPendingPayload, std::ios::cur))
                {
                    return fail();
                }
                mOffset += mPendingPayload;
                mPendingPayload = 0;
            }
            if (mOffset == mSize)
            {
                return false;   // a clean end: the last record was whole
            }
            RecordHeader header;
            if (mSize - mOffset < sizeof(header) || !mFile.read(reinterpret_cast<char*>(&header), sizeof(header)))
            {
                return fail();  // the header itself is cut short
            }
            mOffset += sizeof(header);
            if (header.payload_bytes > mSize - mOffset)
            {
                return fail();  // the payload runs past the end of the file
            }
            out = header;
            mPendingPayload = header.payload_bytes;
            return true;
        }

        // The current record's payload (empty if it had none, or if already taken). Bounded by
        // the file's size (next() checked it), and an allocation that still fails is reported
        // as a damaged file (ok() false) rather than thrown.
        std::vector<U8> takePayload()
        {
            std::vector<U8> payload;
            if (mPendingPayload)
            {
                const U32 bytes = mPendingPayload;
                mPendingPayload = 0;
                try
                {
                    payload.resize(bytes);
                }
                catch (const std::bad_alloc&)
                {
                    fail();
                    return {};
                }
                if (!mFile.read(reinterpret_cast<char*>(payload.data()), bytes))
                {
                    fail();
                    return {};
                }
                mOffset += bytes;
            }
            return payload;
        }

    private:
        bool fail()
        {
            mOk = false;
            mPendingPayload = 0;
            return false;
        }

        llifstream mFile;
        U64 mSize = 0;          // the file's size when it was opened
        U64 mOffset = 0;        // where the next read starts
        bool mOk = false;
        U32 mPendingPayload = 0;
    };
}

#endif // LL_LLRHIREPLAYFORMAT_H
