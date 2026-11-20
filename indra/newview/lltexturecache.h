/**
 * @file lltexturecache.h
 * @brief Object for managing texture cachees.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

#ifndef LL_LLTEXTURECACHE_H
#define LL_LLTEXTURECACHE_H

#include "lldir.h"
#include "llstl.h"
#include "llstring.h"
#include "lluuid.h"

#include "llworkerthread.h"

#include "plus/texcache/posfile.h"     // <plus> research/306 (P3a) </plus>

class LLImageFormatted;
class LLTextureCacheWorker;
class LLImageRaw;

class LLTextureCache : public LLWorkerThread
{
    friend class LLTextureCacheWorker;
    friend class LLTextureCacheRemoteWorker;
    friend class LLTextureCacheLocalFileWorker;

private:

#if LL_WINDOWS
#pragma pack(push,1)
#endif

    // Entries
    static const U32 sHeaderEncoderStringSize = 32;
    struct EntriesInfo
    {
        EntriesInfo() : mVersion(0.f), mAdressSize(0), mEntries(0) { memset(mEncoderVersion, 0, sHeaderEncoderStringSize); }
        F32 mVersion;
        U32 mAdressSize;
        char mEncoderVersion[sHeaderEncoderStringSize];
        U32 mEntries;
    };
    struct Entry
    {
            Entry() :
                mBodySize(0),
            mImageSize(0),
            mTime(0)
        {
        }
        Entry(const LLUUID& id, S32 imagesize, S32 bodysize, U32 time) :
            mID(id), mImageSize(imagesize), mBodySize(bodysize), mTime(time) {}
        void init(const LLUUID& id, U32 time) { mID = id, mImageSize = 0; mBodySize = 0; mTime = time; }
        Entry& operator=(const Entry& entry) {mID = entry.mID, mImageSize = entry.mImageSize; mBodySize = entry.mBodySize; mTime = entry.mTime; return *this;}
        LLUUID mID; // 16 bytes
        S32 mImageSize; // total size of image if known
        S32 mBodySize; // size of body file in body cache
        U32 mTime; // seconds since 1/1/1970
    };

#if LL_WINDOWS
#pragma pack(pop)
#endif

public:

    class Responder : public LLResponder
    {
    public:
        virtual void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal) = 0;
    };

    class ReadResponder : public Responder
    {
    public:
        ReadResponder();
        void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal);
        void setImage(LLImageFormatted* image) { mFormattedImage = image; }
    protected:
        LLPointer<LLImageFormatted> mFormattedImage;
        S32 mImageSize;
        bool mImageLocal;
    };

    class WriteResponder : public Responder
    {
        void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, bool imagelocal)
        {
            // not used
        }
    };

    LLTextureCache(bool threaded);
    ~LLTextureCache();

    /*virtual*/ size_t update(F32 max_time_ms);

    void purgeCache(ELLPath location, bool remove_dir = true);
    void setReadOnly(bool read_only) ;
    S64 initCache(ELLPath location, S64 maxsize, bool texture_cache_mismatch);

    handle_t readFromCache(const std::string& local_filename, const LLUUID& id, S32 offset, S32 size,
                           ReadResponder* responder);

    handle_t readFromCache(const LLUUID& id, S32 offset, S32 size,
                           ReadResponder* responder);
    bool readComplete(handle_t handle, bool abort);
    handle_t writeToCache(const LLUUID& id, const U8* data, S32 datasize, S32 imagesize, LLPointer<LLImageRaw> rawimage, S32 discardlevel,
                          WriteResponder* responder);
    LLPointer<LLImageRaw> readFromFastCache(const LLUUID& id, S32& discardlevel);
    bool writeComplete(handle_t handle, bool abort = false);
    void prioritizeWrite(handle_t handle);

    bool removeFromCache(const LLUUID& id);

    // For LLTextureCacheWorker::Responder
    LLTextureCacheWorker* getReader(handle_t handle);
    LLTextureCacheWorker* getWriter(handle_t handle);
    void lockWorkers() { mWorkersMutex.lock(); }
    void unlockWorkers() { mWorkersMutex.unlock(); }

    // debug
    S32 getNumReads() { return static_cast<S32>(mReaders.size()); }
    S32 getNumWrites() { return static_cast<S32>(mWriters.size()); }
    S64Bytes getUsage() { return S64Bytes(mTexturesSizeTotal); }
    S64Bytes getMaxUsage() { return S64Bytes(sCacheMaxTexturesSize); }
    U32 getEntries() { return mHeaderEntriesInfo.mEntries; }
    U32 getMaxEntries() { return sCacheMaxEntries; };
    bool isInCache(const LLUUID& id) ;
    // <plus> research/306 (P1): true only when the index could be looked at without waiting and has no entry for the
    // id (a read of it would find nothing); false: it has one, or the index is busy (the cache thread holds its
    // lock while it reads the entries in). Any thread.
    bool plusKnownMissing(const LLUUID& id);
    // </plus>
    bool isInLocal(const LLUUID& id) ; //not thread safe at the moment
    // <plus> research/306 (P3a/P3b): texture.entries, texture.cache and FastCache.cache kept open, read and written at
    // their positions (plus/texcache/posfile). Opened once at the end of initCache and closed in the destructor, never
    // in between; mUsePosFile only goes false (a wipe of the cache in purgeAllTextures, setReadOnly, a bad handle), and the old way is used from then on.
    bool plusKeepOpenOn() const { return mUsePosFile.load(std::memory_order_relaxed); }
    // </plus>
    LLMutex* getFastCacheMutex() { return &mFastCacheMutex; }
protected:
    // Accessed by LLTextureCacheWorker
    std::string getLocalFileName(const LLUUID& id);
    std::string getTextureFileName(const LLUUID& id);
    void addCompleted(Responder* responder, bool success);

protected:
    //void setFileAPRPool(apr_pool_t* pool) { mFileAPRPool = pool ; }

private:
    void setDirNames(ELLPath location);
    void readHeaderCache();
    void clearCorruptedCache();
    void purgeAllTextures(bool purge_directories);
    void purgeTexturesLazy(F32 time_limit_sec);
    void purgeTextures(bool validate);
    LLAPRFile* openHeaderEntriesFile(bool readonly, S32 offset);
    void closeHeaderEntriesFile();
    void readEntriesHeader();
    void setEntriesHeader();
    void writeEntriesHeader();
    S32 openAndReadEntry(const LLUUID& id, Entry& entry, bool create);
    bool updateEntry(S32& idx, Entry& entry, S32 new_image_size, S32 new_body_size);
    void updateEntryTimeStamp(S32 idx, Entry& entry) ;
    U32 openAndReadEntries(std::vector<Entry>& entries);
    void writeEntriesAndClose(const std::vector<Entry>& entries);
    void readEntryFromHeaderImmediately(S32& idx, Entry& entry) ;
    void writeEntryToHeaderImmediately(S32& idx, Entry& entry, bool write_header = false) ;
    void removeEntry(S32 idx, Entry& entry, std::string& filename);
    void removeCachedTexture(const LLUUID& id) ;
    S32 getHeaderCacheEntry(const LLUUID& id, Entry& entry);
    S32 setHeaderCacheEntry(const LLUUID& id, Entry& entry, S32 imagesize, S32 datasize);
    void writeUpdatedEntries() ;
    void updatedHeaderEntriesFile() ;
    void lockHeaders();     // <plus> research/306 (diagnostics): defined in the .cpp, with the wait counted </plus>
    void unlockHeaders() { mHeaderMutex.unlock(); }

    void openFastCache(bool first_time = false);
    void closeFastCache(bool forced = false);
    bool writeToFastCache(LLUUID image_id, S32 cache_id, LLPointer<LLImageRaw> raw, S32 discardlevel);

private:
    // Internal
    LLMutex mWorkersMutex;
    LLMutex mHeaderMutex;
    LLMutex mHeaderIDMapMutex; // To avoid deadlocks, never lock mFastCacheMutex after mHeaderIDMapMutex.
    LLMutex mListMutex;
    LLMutex mFastCacheMutex;
    LLAPRFile* mHeaderAPRFile;
    LLVolatileAPRPool* mFastCachePoolp;

    // mLocalAPRFilePoolp is not thread safe and is meant only for workers
    // howhever mHeaderEntriesFileName is accessed not from workers' threads
    // so it needs own pool (not thread safe by itself, relies onto header's mutex)
    LLVolatileAPRPool*   mHeaderAPRFilePoolp;

    typedef std::map<handle_t, LLTextureCacheWorker*> handle_map_t;
    handle_map_t mReaders;
    handle_map_t mWriters;

    typedef std::vector<handle_t> handle_list_t;
    handle_list_t mPrioritizeWriteList;

    typedef std::vector<std::pair<LLPointer<Responder>, bool> > responder_list_t;
    responder_list_t mCompletedList;

    bool mReadOnly;

    // HEADERS (Include first mip)
    std::string mHeaderEntriesFileName;
    std::filesystem::path mHeaderEntriesFilePath;
    std::filesystem::path mHeaderDataFilePath;
    std::string mFastCacheFileName;
    EntriesInfo mHeaderEntriesInfo;
    std::set<S32> mFreeList; // deleted entries
    std::set<LLUUID> mLRU;
    typedef std::map<LLUUID, S32> id_map_t;
    id_map_t mHeaderIDMap;

    LLAPRFile*   mFastCachep;
    LLFrameTimer mFastCacheTimer;
    U8*          mFastCachePadBuffer;

    // <plus> research/306 (P3a)
    static constexpr S64 POS_NOT_USED = -100;       // plusPosIO: not done, do it the old way
    plus::texcache::PosFile mPosEntries;            // texture.entries (under mHeaderMutex)
    plus::texcache::PosFile mPosHeader;             // texture.cache (no lock: a record is one entry's)
    plus::texcache::PosFile mPosFast;               // FastCache.cache (written by the cache thread, read by the main thread)
    std::atomic<bool> mUsePosFile{ false };
    void plusOpenPosFiles();
    void plusDropPosFiles(const char* where, bool trouble);
    S64 plusPosIO(const plus::texcache::PosFile& file, bool write, U64 offset, void* buffer, S32 bytes, const char* what);
    bool plusReadEntry(S32& idx, Entry& entry);
    bool plusWriteEntry(S32& idx, Entry& entry, bool write_header);
    S64 plusHeaderRead(S64 offset, void* buffer, S64 bytes);
    S64 plusHeaderWrite(S64 offset, const void* buffer, S64 bytes);
    LLPointer<LLImageRaw> plusReadFastCache(U32 offset, S32& discardlevel, bool& handled);
    // </plus>

    // <plus> research/337 (C): the entry read back from texture.entries at the index's position is checked to be this
    // texture's. A read-only start (a second one) has the index as it was when it started, and the first one may have
    // given that position to another texture since; the fast cache (read at the same position, on the main thread) is
    // checked against the entry too in a read-only start. Counted (the cache thread under mHeaderMutex, the main
    // thread) and logged from update (main thread), once a PLUS_INDEX_LOG_SECONDS window: a read-only start the first
    // PLUS_INDEX_LOG_FIRST windows it reads in, then only windows where something did not match; a writer only when one
    // did not match.
    static constexpr F64 PLUS_INDEX_LOG_SECONDS = 20.0;
    static constexpr U32 PLUS_INDEX_LOG_FIRST = 3;
    std::atomic<U32> mPlusIndexReads{ 0 };          // entries read back for a read or a removal (this window)
    std::atomic<U32> mPlusIndexWrites{ 0 };         // entries read back for a write (setHeaderCacheEntry; a writer only)
    std::atomic<U32> mPlusIndexOther{ 0 };          // ... of both, that were another texture's
    std::atomic<U32> mPlusIndexFreed{ 0 };          // ... that were this texture's but no longer valid (read-only: let go of, not removed)
    std::atomic<U32> mPlusFastChecked{ 0 };         // read-only: fast cache records looked at (main thread)
    std::atomic<U32> mPlusFastOther{ 0 };           // ... whose entry was not this texture's any more (not used)
    U64 mPlusIndexOtherTotal = 0;                   // main thread (the log)
    U64 mPlusIndexFreedTotal = 0;                   // main thread (the log)
    U64 mPlusFastOtherTotal = 0;                    // main thread (the log)
    U32 mPlusIndexWindowsLogged = 0;                // main thread: read-only windows logged with nothing found
    F64 mPlusIndexLogAt = 0.0;                      // main thread, LLFrameTimer::getTotalSeconds; 0: the first window not started
    bool plusDropStaleEntry(const LLUUID& id, S32 idx, const Entry& entry);
    bool plusFastEntryIsFor(const LLUUID& id, U32 idx);
    void plusForgetTexture(const LLUUID& id);
    void plusLogIndexChecks();
    // </plus>

    // BODIES (TEXTURES minus headers)
    std::string mTexturesDirName;
    typedef std::map<LLUUID,S32> size_map_t;
    size_map_t mTexturesSizeMap;
    S64 mTexturesSizeTotal;
    LLAtomicBool mDoPurge;

    typedef std::map<S32, Entry> idx_entry_map_t;
    idx_entry_map_t mUpdatedEntryMap;
    typedef std::vector<std::pair<S32, Entry> > idx_entry_vector_t;
    idx_entry_vector_t mPurgeEntryList;

    // Statics
    static F32 sHeaderCacheVersion;
    static U32 sHeaderCacheAddressSize;
    static std::string sHeaderCacheEncoderVersion;
    static U32 sCacheMaxEntries;
    static S64 sCacheMaxTexturesSize;
};

extern const S32 TEXTURE_CACHE_ENTRY_SIZE;

#endif // LL_LLTEXTURECACHE_H
