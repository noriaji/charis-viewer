/**
 * @file llvertexbuffer.cpp
 * @brief LLVertexBuffer implementation
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
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

#include "linden_common.h"

#include "llfasttimer.h"
#include "llsys.h"
#include "llvertexbuffer.h"
// #include "llrender.h"
#include "llrhivbcensus.h"   // <plus> research/293 branch 0 </plus>
#include "llglheaders.h"
#include "llrender.h"
#include "llvector4a.h"
#include "llshadermgr.h"
#include "llglslshader.h"
#include "llmemory.h"
#include <glm/gtc/type_ptr.hpp>

//Next Highest Power Of Two
//helper function, returns first number > v that is a power of 2, or v if v is already a power of 2
U32 nhpo2(U32 v)
{
    U32 r = 1;
    while (r < v) {
        r *= 2;
    }
    return r;
}

//which power of 2 is i?
//assumes i is a power of 2 > 0
U32 wpo2(U32 i)
{
    llassert(i > 0);
    llassert(nhpo2(i) == i);

    U32 r = 0;

    while (i >>= 1) ++r;

    return r;
}

struct CompareMappedRegion
{
    bool operator()(const LLVertexBuffer::MappedRegion& lhs, const LLVertexBuffer::MappedRegion& rhs)
    {
        return lhs.mStart < rhs.mStart;
    }
};

#define ENABLE_GL_WORK_QUEUE 0

#if ENABLE_GL_WORK_QUEUE

#define THREAD_COUNT 1

//============================================================================

// High performance WorkQueue for usage in real-time rendering work
class GLWorkQueue
{
public:
    using Work = std::function<void()>;

    GLWorkQueue();

    void post(const Work& value);

    size_t size();

    bool done();

    // Get the next element from the queue
    Work pop();

    void runOne();

    bool runPending();

    void runUntilClose();

    void close();

    bool isClosed();

    void syncGL();

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::queue<Work> mQueue;
    bool mClosed = false;
};

GLWorkQueue::GLWorkQueue()
{

}

void GLWorkQueue::syncGL()
{
    /*if (mSync)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        glWaitSync(mSync, 0, GL_TIMEOUT_IGNORED);
        mSync = 0;
    }*/
}

size_t GLWorkQueue::size()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    std::lock_guard<std::mutex> lock(mMutex);
    return mQueue.size();
}

bool GLWorkQueue::done()
{
    return size() == 0 && isClosed();
}

void GLWorkQueue::post(const GLWorkQueue::Work& value)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mQueue.push(std::move(value));
    }

    mCondition.notify_one();
}

// Get the next element from the queue
GLWorkQueue::Work GLWorkQueue::pop()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    // Lock the mutex
    {
        std::unique_lock<std::mutex> lock(mMutex);

        // Wait for a new element to become available or for the queue to close
        {
            mCondition.wait(lock, [=] { return !mQueue.empty() || mClosed; });
        }
    }

    Work ret;

    {
        std::lock_guard<std::mutex> lock(mMutex);

        // Get the next element from the queue
        if (mQueue.size() > 0)
        {
            ret = mQueue.front();
            mQueue.pop();
        }
        else
        {
            ret = []() {};
        }
    }

    return ret;
}

void GLWorkQueue::runOne()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    Work w = pop();
    w();
    //mSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void GLWorkQueue::runUntilClose()
{
    while (!isClosed())
    {
        runOne();
    }
}

void GLWorkQueue::close()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mClosed = true;
    }

    mCondition.notify_all();
}

bool GLWorkQueue::isClosed()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_THREAD;
    std::lock_guard<std::mutex> lock(mMutex);
    return mClosed;
}

#include "llwindow.h"

class LLGLWorkerThread : public LLThread
{
public:
    LLGLWorkerThread(const std::string& name, GLWorkQueue* queue, LLWindow* window)
        : LLThread(name)
    {
        mWindow = window;
        mContext = mWindow->createSharedContext();
        mQueue = queue;
    }

    void run() override
    {
        mWindow->makeContextCurrent(mContext);
        gGL.init(false);
        mQueue->runUntilClose();
        gGL.shutdown();
        mWindow->destroySharedContext(mContext);
    }

    GLWorkQueue* mQueue;
    LLWindow* mWindow;
    void* mContext = nullptr;
};


static LLGLWorkerThread* sVBOThread[THREAD_COUNT];
static GLWorkQueue* sQueue = nullptr;

#endif

//============================================================================
// Pool of reusable VertexBuffer state

// batch calls to glGenBuffers
static GLuint gen_buffer()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

    GLuint ret = 0;
    constexpr U32 pool_size = 4096;

    thread_local static GLuint sNamePool[pool_size];
    thread_local static U32 sIndex = 0;

    if (sIndex == 0)
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("gen buffer");
        sIndex = pool_size;
#if !LL_DARWIN
        if (!gGLManager.mIsAMD)
        {
            glGenBuffers(pool_size, sNamePool);
        }
        else
#endif
        { // work around for AMD driver bug
            for (U32 i = 0; i < pool_size; ++i)
            {
                glGenBuffers(1, sNamePool + i);
            }
        }
    }

    ret = sNamePool[--sIndex];
    return ret;
}

static void delete_buffers(S32 count, GLuint* buffers)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // wait a few frames before actually deleting the buffers to avoid
    // synchronization issues with the GPU
    constexpr U32 BUCKET_COUNT = 4;
    static std::vector<GLuint> sFreeList[BUCKET_COUNT];

    if (gGLManager.mInited)
    {
        // Move current frame to free list
        U32 idx = LLImageGL::sFrameCount % BUCKET_COUNT;

        for (S32 i = 0; i < count; ++i)
        {
            sFreeList[idx].push_back(buffers[i]);
        }

        // Clear frame -3 (equals +1), this idx will be written over on the next call
        idx = (LLImageGL::sFrameCount + 1) % BUCKET_COUNT;

        if (!sFreeList[idx].empty())
        {
            glDeleteBuffers((GLsizei)sFreeList[idx].size(), sFreeList[idx].data());
            sFreeList[idx].resize(0);
        }
    }
}


#define ANALYZE_VBO_POOL 0

// VBO Pool interface
class LLVBOPool
{
    public:
    virtual ~LLVBOPool() = default;
    virtual void allocate(GLenum type, U32 size, GLuint& name, U8*& data) = 0;
    virtual void free(GLenum type, U32 size, GLuint name, U8* data) = 0;
    virtual U64 getVramBytesUsed() = 0;
};

// VBO Pool for Apple GPUs (as in M1/M2 etc, not Intel macs)
// Effectively disables VBO pooling
class LLAppleVBOPool final: public LLVBOPool
{
public:
    U64 mAllocated = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated;
    }

    void allocate(GLenum type, U32 size, GLuint& name, U8*& data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        STOP_GLERROR;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(data == nullptr);  // non null data indicates a buffer that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mAllocated += size;

        { //allocate a new buffer
            LL_PROFILE_GPU_ZONE("vbo alloc");
            // ON OS X, we don't allocate a VBO until the last possible moment
            // in unmapBuffer
            data = (U8*) ll_aligned_malloc_16(size);
            STOP_GLERROR;
        }
    }

    void free(GLenum type, U32 size, GLuint name, U8* data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(size >= 2);

        if (data)
        {
            ll_aligned_free_16(data);
        }

        mAllocated -= size;
        STOP_GLERROR;
        if (name)
        {
            delete_buffers(1, &name);
        }
        STOP_GLERROR;
    }
};

// VBO Pool for GPUs that benefit from VBO pooling
class LLDefaultVBOPool final : public LLVBOPool
{
public:
    typedef std::chrono::steady_clock::time_point Time;
    struct Entry
    {
        U8* mData;
        GLuint mGLName;
        Time mAge;
    };

    ~LLDefaultVBOPool() override
    {
        clear();
    }

    typedef std::unordered_map<U32, std::list<Entry>> Pool;

    Pool mVBOPool;
    Pool mIBOPool;

    U32 mTouchCount = 0;

    U64 mDistributed = 0;
    U64 mAllocated = 0;
    U64 mReserved = 0;
    U32 mMisses = 0;
    U32 mHits = 0;

    U64 getVramBytesUsed() override
    {
        return mAllocated + mReserved;
    }

    // increase the size to some common value (e.g. a power of two) to increase hit rate
    void adjustSize(U32& size)
    {
        // size = nhpo2(size);  // (193/303)/580 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 66 percent. Hit rate: 77 percent

        //(245/276)/385 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 57 percent. Hit rate: 69 percent
        //(187/209)/397 MB (distributed/allocated)/reserved in VBO Pool. Overhead: 112 percent. Hit rate: 76 percent
        U32 block_size = llmax(nhpo2(size) / 8, (U32) 16);
        size += block_size - (size % block_size);
    }

    void allocate(GLenum type, U32 size, GLuint& name, U8*& data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(name == 0); // non zero name indicates a gl name that wasn't freed
        llassert(data == nullptr);  // non null data indicates a buffer that wasn't freed
        llassert(size >= 2);  // any buffer size smaller than a single index is nonsensical

        mDistributed += size;
        adjustSize(size);
        mAllocated += size;

        auto& pool = type == GL_ELEMENT_ARRAY_BUFFER ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);
        if (iter == pool.end())
        { // cache miss, allocate a new buffer
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo pool miss");
            LL_PROFILE_GPU_ZONE("vbo alloc");

            mMisses++;
            name = gen_buffer();
            LLVertexBuffer::bindGLBuffer(type, name); // LLRHI: GL's binding may follow later
            LLRHI::sBufferData(name, size, nullptr); // <plus> LLRHI: native buffer storage (research/207 T2: Vulkan only) </plus>

            data = (U8*)ll_aligned_malloc_16(size);
        }
        else
        {
            mHits++;
            llassert(mReserved >= size); // assert if accounting gets messed up
            mReserved -= size;

            std::list<Entry>& entries = iter->second;
            Entry& entry = entries.back();
            name = entry.mGLName;
            data = entry.mData;

            entries.pop_back();
            if (entries.empty())
            {
                pool.erase(iter);
            }
        }

        clean();
    }

    void free(GLenum type, U32 size, GLuint name, U8* data) override
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        llassert(type == GL_ARRAY_BUFFER || type == GL_ELEMENT_ARRAY_BUFFER);
        llassert(size >= 2);
        llassert(name != 0);
        llassert(data != nullptr);

        clean();

        llassert(mDistributed >= size);
        mDistributed -= size;
        adjustSize(size);
        llassert(mAllocated >= size);
        mAllocated -= size;
        mReserved += size;

        auto& pool = type == GL_ELEMENT_ARRAY_BUFFER ? mIBOPool : mVBOPool;

        Pool::iterator iter = pool.find(size);

        if (iter == pool.end())
        {
            std::list<Entry> newlist;
            newlist.push_front({ data, name, std::chrono::steady_clock::now() });
            pool[size] = newlist;
        }
        else
        {
            iter->second.push_front({ data, name, std::chrono::steady_clock::now() });
        }

    }

    // clean periodically (clean gets called for every alloc/free)
    void clean()
    {
        mTouchCount++;
        if (mTouchCount < 1024) // clean every 1k touches
        {
            return;
        }
        mTouchCount = 0;

        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;

        std::unordered_map<U32, std::list<Entry>>* pools[] = { &mVBOPool, &mIBOPool };

        using namespace std::chrono_literals;

        Time cutoff = std::chrono::steady_clock::now() - 5s;

        for (auto* pool : pools)
        {
            for (Pool::iterator iter = pool->begin(); iter != pool->end(); )
            {
                auto& entries = iter->second;

                while (!entries.empty() && entries.back().mAge < cutoff)
                {
                    LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vbo cache timeout");
                    auto& entry = entries.back();
                    ll_aligned_free_16(entry.mData);
                    delete_buffers(1, &entry.mGLName);
                    llassert(mReserved >= iter->first);
                    mReserved -= iter->first;
                    entries.pop_back();

                }

                if (entries.empty())
                {
                    iter = pool->erase(iter);
                }
                else
                {
                    ++iter;
                }
            }
        }

#if 0
        LL_INFOS() << llformat("(%d/%d)/%d MB (distributed/allocated)/total in VBO Pool. Overhead: %d percent. Hit rate: %d percent",
            mDistributed / 1000000,
            mAllocated / 1000000,
            (mAllocated + mReserved) / 1000000, // total bytes
            ((mAllocated+mReserved-mDistributed)*100)/llmax(mDistributed, (U64) 1), // overhead percent
            (mHits*100)/llmax(mMisses+mHits, (U32)1)) // hit rate percent
            << LL_ENDL;
#endif
    }

    void clear()
    {
        for (auto& entries : mIBOPool)
        {
            for (auto& entry : entries.second)
            {
                ll_aligned_free_16(entry.mData);
                delete_buffers(1, &entry.mGLName);
            }
        }

        for (auto& entries : mVBOPool)
        {
            for (auto& entry : entries.second)
            {
                ll_aligned_free_16(entry.mData);
                delete_buffers(1, &entry.mGLName);
            }
        }

        mReserved = 0;

        mIBOPool.clear();
        mVBOPool.clear();
    }
};

static LLVBOPool* sVBOPool = nullptr;

void LLVertexBufferData::drawWithMatrix()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    if (mTexName)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexName);
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mModelView));
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mProjection));
    gGL.matrixMode(LLRender::MM_TEXTURE0);
    gGL.pushMatrix();
    gGL.loadMatrix(glm::value_ptr(mTexture0));

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);

    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_PROJECTION);
    gGL.popMatrix();
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.popMatrix();
}

void LLVertexBufferData::draw()
{
    if (!mVB)
    {
        llassert(false);
        // Not supposed to happen, check buffer generation
        return;
    }

    if (mTexName)
    {
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, mTexName);
    }
    else
    {
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    }

    mVB->setBuffer();
    mVB->drawArrays(mMode, 0, mCount);
}

//============================================================================

//static
U64 LLVertexBuffer::getBytesAllocated()
{
    return sVBOPool ? sVBOPool->getVramBytesUsed() : 0;
}

//============================================================================
//
//static
U32 LLVertexBuffer::sGLRenderBuffer = 0;
U32 LLVertexBuffer::sGLRenderIndices = 0;
bool LLVertexBuffer::sGLBuffersStale = false;
U32 LLVertexBuffer::sLastMask = 0;
U32 LLVertexBuffer::sGLArrayMask = 0;
U32 LLVertexBuffer::sVertexCount = 0;


//NOTE: each component must be AT LEAST 4 bytes in size to avoid a performance penalty on AMD hardware
const U32 LLVertexBuffer::sTypeSize[LLVertexBuffer::TYPE_MAX] =
{
    sizeof(LLVector4), // TYPE_VERTEX,
    sizeof(LLVector4), // TYPE_NORMAL,
    sizeof(LLVector2), // TYPE_TEXCOORD0,
    sizeof(LLVector2), // TYPE_TEXCOORD1,
    sizeof(LLVector2), // TYPE_TEXCOORD2,
    sizeof(LLVector2), // TYPE_TEXCOORD3,
    sizeof(LLColor4U), // TYPE_COLOR,
    sizeof(LLColor4U), // TYPE_EMISSIVE, only alpha is used currently
    sizeof(LLVector4), // TYPE_TANGENT,
    sizeof(F32),       // TYPE_WEIGHT,
    sizeof(LLVector4), // TYPE_WEIGHT4,
    sizeof(LLVector4), // TYPE_CLOTHWEIGHT,
    sizeof(U64),       // TYPE_JOINT,
    sizeof(LLVector4), // TYPE_TEXTURE_INDEX (actually exists as position.w), no extra data, but stride is 16 bytes
};

static const std::string vb_type_name[] =
{
    "TYPE_VERTEX",
    "TYPE_NORMAL",
    "TYPE_TEXCOORD0",
    "TYPE_TEXCOORD1",
    "TYPE_TEXCOORD2",
    "TYPE_TEXCOORD3",
    "TYPE_COLOR",
    "TYPE_EMISSIVE",
    "TYPE_TANGENT",
    "TYPE_WEIGHT",
    "TYPE_WEIGHT4",
    "TYPE_CLOTHWEIGHT",
    "TYPE_JOINT"
    "TYPE_TEXTURE_INDEX",
    "TYPE_MAX",
    "TYPE_INDEX",
};

const U32 LLVertexBuffer::sGLMode[LLRender::NUM_MODES] =
{
    GL_TRIANGLES,
    GL_TRIANGLE_STRIP,
    GL_TRIANGLE_FAN,
    GL_POINTS,
    GL_LINES,
    GL_LINE_STRIP,
    GL_LINE_LOOP,
};

//static
void LLVertexBuffer::setupClientArrays(U32 data_mask)
{
    // <plus> LLRHI: called on every program bind. Draws go to the Vulkan
    // backend as DirectDraws, which carry their own attributes: nothing reads
    // GL's enabled arrays until a draw the backend declines, before which they
    // follow (ensureGLArrays). research/207 T2: Vulkan only, the GL loop is gone.
    sLastMask = data_mask;
    // </plus>
}

//static
void LLVertexBuffer::ensureGLArrays()
{
    if (sGLArrayMask != sLastMask) // <plus> research/207 T2 </plus>
    {
        for (U32 i = 0; i < TYPE_MAX; ++i)
        {
            const U32 mask = 1 << i;
            if ((sGLArrayMask ^ sLastMask) & mask)
            {
                if (sLastMask & mask)
                {
                    glEnableVertexAttribArray(i);
                }
                else
                {
                    glDisableVertexAttribArray(i);
                }
            }
        }
        sGLArrayMask = sLastMask;
    }
}

//static
void LLVertexBuffer::drawArrays(U32 mode, const std::vector<LLVector3>& pos)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    gGL.begin(mode);
    for (auto& v : pos)
    {
        gGL.vertex3fv(v.mV);
    }
    gGL.end();
    gGL.flush();
}

//static
void LLVertexBuffer::drawElements(U32 mode, const LLVector4a* pos, const LLVector2* tc, U32 num_indices, const U16* indicesp)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(LLGLSLShader::sCurBoundShaderPtr != NULL);

    STOP_GLERROR;

    gGL.syncMatrices();

    unbind();

    gGL.begin(mode);

    if (tc != nullptr)
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gGL.texCoord2fv(tc[idx].mV);
            gGL.vertex3fv(pos[idx].getF32ptr());
        }
    }
    else
    {
        for (U32 i = 0; i < num_indices; ++i)
        {
            U16 idx = indicesp[i];
            gGL.vertex3fv(pos[idx].getF32ptr());
        }
    }
    gGL.end();
    gGL.flush();
}

bool LLVertexBuffer::validateRange(U32 start, U32 end, U32 count, U32 indices_offset) const
{
    if (!gDebugGL)
    {
        return true;
    }

    llassert(start < mNumVerts);
    llassert(end < mNumVerts);

    if (start >= mNumVerts ||
        end >= mNumVerts)
    {
        LL_ERRS() << "Bad vertex buffer draw range: [" << start << ", " << end << "] vs " << mNumVerts << LL_ENDL;
    }

    if (indices_offset >= mNumIndices ||
        indices_offset + count > mNumIndices)
    {
        LL_ERRS() << "Bad index buffer draw range: [" << indices_offset << ", " << indices_offset+count << "]" << LL_ENDL;
    }

    {
#if 0  // not a reliable test for VBOs that are not backed by a CPU buffer
        U16* idx = (U16*) mMappedIndexData+indices_offset;
        for (U32 i = 0; i < count; ++i)
        {
            llassert(idx[i] >= start);
            llassert(idx[i] <= end);

            if (idx[i] < start || idx[i] > end)
            {
                LL_ERRS() << "Index out of range: " << idx[i] << " not in [" << start << ", " << end << "]" << LL_ENDL;
            }
        }

        LLVector4a* v = (LLVector4a*)mMappedData;

        for (U32 i = start; i <= end; ++i)
        {
            if (!v[i].isFinite3())
            {
                LL_ERRS() << "Non-finite vertex position data detected." << LL_ENDL;
            }
        }

        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;

        if (shader && shader->mFeatures.mIndexedTextureChannels > 1)
        {
            LLVector4a* v = (LLVector4a*) mMappedData;

            for (U32 i = start; i < end; i++)
            {
                U32 idx = (U32) (v[i][3]+0.25f);
                if (idx >= (U32)shader->mFeatures.mIndexedTextureChannels)
                {
                    LL_ERRS() << "Bad texture index found in vertex data stream." << LL_ENDL;
                }
            }
        }
#endif
    }

    return true;
}

#if LL_PROFILER_ENABLE_RENDER_DOC
void LLVertexBuffer::setLabel(const char* label) {
    LL_LABEL_OBJECT_GL(GL_BUFFER, mGLBuffer, strlen(label), label);
}
#endif

void LLVertexBuffer::clone(LLVertexBuffer& target) const
{
    target.mTypeMask = mTypeMask;
    target.mIndicesType = mIndicesType;
    target.mIndicesStride = mIndicesStride;
    if (target.getNumVerts() != getNumVerts() ||
        target.getNumIndices() != getNumIndices())
    {
        target.allocateBuffer(getNumVerts(), getNumIndices());
    }
    target.rhiLayoutChanged(); // LLRHI (research/197 1b): the index type may have changed without an allocation
}

bool LLVertexBuffer::sGLAttributesStale = false;

bool LLVertexBuffer::drawNative(U32 mode, U32 first, U32 count, bool indexed) const
{
    LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
    if (LLRHIVBCensus::sOn) // <plus> research/293 branch 0: counting only </plus>
    {
        censusDraw(mode, indexed, shader);
    }
    // <plus> research/207 T2: the hooks are always there; sDirect stays for the
    // cue file's A/B (direct.toggle, llviewerdisplay.cpp) until that goes too
    if (!LLRHI::sDirect || !shader || !mGLBuffer || (indexed && !mGLIndices))
    // </plus>
    {
        return false;
    }
    LLRHI::DirectDraw draw;
    draw.vertex_buffer = mGLBuffer;
    draw.mode = sGLMode[mode];
    if (indexed)
    {
        draw.index_buffer = mGLIndices;
        draw.index_type = mIndicesType;
        draw.first_index = first;
        draw.index_count = count;
        // The whole buffers, for the backend's attribute pool (LLRHI::sDrawRuns).
        draw.buffer_vertices = getNumVerts();
        draw.buffer_indices = getNumIndices();
        draw.vb = rhiDrawNumber(); // LLRHI (research/197 1b): the pair by its number
        // A buffer drawn in a draw run before goes by its template if the run
        // open now may take it (research/46 G4c); else the usual way below.
        if (LLRHI::sDrawTemplates && LLRHI::sDrawRuns && LLRHI::sDrawTemplate)
        {
            draw.draw_template = &mRHIDrawTemplate;
            if (mRHIDrawTemplate && LLRHI::sDrawTemplate(draw))
            {
                return true;
            }
        }
    }
    else
    {
        draw.first_vertex = first;
        draw.vertex_count = count;
    }
    LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
    draw.attribute_count = getAttributes(shader->mAttributeMask, attributes);
    draw.attributes = attributes;
    return LLRHI::sDirectDraw(draw);
}

//static
void LLVertexBuffer::bindGLBuffer(U32 target, U32 name)
{
    llassert(target == GL_ARRAY_BUFFER || target == GL_ELEMENT_ARRAY_BUFFER);
    // Vulkan draws name their buffers (drawNative) and contents go by name
    // (sBufferData / sBufferSubData), so nothing reads GL's binding until a
    // draw the backend declines (bindGLBuffers). research/207 T2: Vulkan only.
    sGLBuffersStale = true; // <plus> research/207 T2 </plus>
    if (target == GL_ELEMENT_ARRAY_BUFFER)
    {
        sGLRenderIndices = name;
    }
    else
    {
        sGLRenderBuffer = name;
    }
}

//static
void LLVertexBuffer::bindGLBuffers()
{
    if (sGLBuffersStale)
    {
        glBindBuffer(GL_ARRAY_BUFFER, sGLRenderBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sGLRenderIndices);
        sGLBuffersStale = false;
    }
}

void LLVertexBuffer::ensureGLAttributes() const
{
    bindGLBuffers(); // before the attribute pointers, which take GL's array buffer
    ensureGLArrays();
    if (sGLAttributesStale && LLGLSLShader::sCurBoundShaderPtr)
    {
        const_cast<LLVertexBuffer*>(this)->setupVertexBuffer();
        sGLAttributesStale = false;
    }
}

void LLVertexBuffer::drawRange(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    llassert(validateRange(start, end, count, indices_offset));
    llassert(mGLBuffer == sGLRenderBuffer);
    llassert(mGLIndices == sGLRenderIndices);
    gGL.syncMatrices();
    STOP_GLERROR;
    if (drawNative(mode, indices_offset, count, true)) // LLRHI
    {
        return;
    }
    ensureGLAttributes(); // LLRHI
    glDrawRangeElements(sGLMode[mode], start, end, count, mIndicesType,
        (GLvoid*) (indices_offset * (size_t) mIndicesStride));
    STOP_GLERROR;
}

void LLVertexBuffer::drawRangeFast(U32 mode, U32 start, U32 end, U32 count, U32 indices_offset) const
{
    if (drawNative(mode, indices_offset, count, true)) // LLRHI
    {
        return;
    }
    ensureGLAttributes(); // LLRHI
    glDrawRangeElements(sGLMode[mode], start, end, count, mIndicesType,
        (GLvoid*)(indices_offset * (size_t)mIndicesStride));
}


void LLVertexBuffer::draw(U32 mode, U32 count, U32 indices_offset) const
{
    drawRange(mode, 0, mNumVerts-1, count, indices_offset);
}


void LLVertexBuffer::drawArrays(U32 mode, U32 first, U32 count) const
{
    llassert(first + count <= mNumVerts);
    llassert(mGLBuffer == sGLRenderBuffer);
    llassert(mGLIndices == sGLRenderIndices);

    gGL.syncMatrices();
    STOP_GLERROR;
    if (drawNative(mode, first, count, false)) // LLRHI
    {
        return;
    }
    ensureGLAttributes(); // LLRHI
    glDrawArrays(sGLMode[mode], first, count);
    STOP_GLERROR;
}

//static
void LLVertexBuffer::initClass(LLWindow* window)
{
    llassert(sVBOPool == nullptr);

    // <plus> research/207 T2: llrender's resource classes (LLVertexBuffer,
    // LLImageGL, LLTexUnit, LLRenderTarget, LLGLSLShader) call the Vulkan
    // backend's hooks with no GL fallback: stop here, with the reason, rather
    // than at the first call through a null hook. The backend installs them all
    // from LLRHI::onGLFunctionsLoaded (LLGLManager::initGL, before this);
    // missing ones mean a switch that turned them off (LLRHI_DIRECT=0,
    // LLRHI_NATIVE_SHADER=0: retired with the GL paths; LLRHI_NATIVE_STATE=0
    // is gone, research/207 T4).
    {
        std::string missing;
        auto need = [&missing](bool there, const char* name)
        {
            if (!there)
            {
                missing += missing.empty() ? name : std::string(", ") + name;
            }
        };
        need(LLRHI::sDirect, "sDirect (LLRHI_DIRECT=0)");
        need(LLRHI::sDirectDraw, "sDirectDraw");
        need(LLRHI::sBufferData, "sBufferData");
        need(LLRHI::sBufferSubData, "sBufferSubData");
        need(LLRHI::sShaderProgram, "sShaderProgram (LLRHI_NATIVE_SHADER=0)");
        need(LLRHI::sUniformSlot, "sUniformSlot");
        need(LLRHI::sUseProgram, "sUseProgram");
        need(LLRHI::sSetUniform, "sSetUniform");
        need(LLRHI::sSetUniformMatrix, "sSetUniformMatrix");
        need(LLRHI::sActiveTexture, "sActiveTexture");
        need(LLRHI::sBindTexture, "sBindTexture");
        need(LLRHI::sTexParameters, "sTexParameters");
        need(LLRHI::sTexImage2D, "sTexImage2D");
        need(LLRHI::sCompressedTexImage2D, "sCompressedTexImage2D");
        need(LLRHI::sTexSubImage2D, "sTexSubImage2D");
        need(LLRHI::sGenerateMipmap, "sGenerateMipmap");
        need(LLRHI::sTexStorage2D, "sTexStorage2D");
        need(LLRHI::sCopyTexSubImage2D, "sCopyTexSubImage2D");
        need(LLRHI::sBindFramebuffer, "sBindFramebuffer");
        need(LLRHI::sDrawBuffers, "sDrawBuffers");
        need(LLRHI::sDrawBuffer, "sDrawBuffer");
        need(LLRHI::sReadBuffer, "sReadBuffer");
        need(LLRHI::sViewport, "sViewport");
        need(LLRHI::sScissor, "sScissor");
        need(LLRHI::sClear, "sClear");
        // research/207 T4: the state classes' (LLGLState, LLGLDepthTest,
        // LLRender's setters) and llrhigl.h's, with no GL fallback either
        need(LLRHI::sSetCap, "sSetCap");
        need(LLRHI::sBlendFunc, "sBlendFunc");
        need(LLRHI::sColorMask, "sColorMask");
        need(LLRHI::sDepthFunc, "sDepthFunc");
        need(LLRHI::sDepthMask, "sDepthMask");
        need(LLRHI::sLineWidth, "sLineWidth");
        need(LLRHI::sCullFace, "sCullFace");
        need(LLRHI::sPolygonOffset, "sPolygonOffset");
        need(LLRHI::sPolygonMode, "sPolygonMode");
        need(LLRHI::sClearColor, "sClearColor");
        if (!missing.empty())
        {
            LL_ERRS("RenderInit") << "The Vulkan backend did not install the render hooks llrender needs (research/207 T2): " << missing
                                  << LL_ENDL;
        }
    }
    // </plus>

#if LL_DARWIN || LL_ARM64
    if (gGLManager.mIsApple)
    {
        LL_INFOS() << "VBO Pooling Disabled" << LL_ENDL;
        sVBOPool = new LLAppleVBOPool();
    }
    else
#endif
    {
        LL_INFOS() << "VBO Pooling Enabled" << LL_ENDL;
        sVBOPool = new LLDefaultVBOPool();
    }

#if ENABLE_GL_WORK_QUEUE
    sQueue = new GLWorkQueue();

    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        sVBOThread[i] = new LLGLWorkerThread("VBO Worker", sQueue, window);
        sVBOThread[i]->start();
    }
#endif
}

//static
void LLVertexBuffer::unbind()
{
    STOP_GLERROR;
    bindGLBuffer(GL_ARRAY_BUFFER, 0); // LLRHI
    bindGLBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    STOP_GLERROR;
}

//static
void LLVertexBuffer::cleanupClass()
{
    unbind();

    delete sVBOPool;
    sVBOPool = nullptr;

#if ENABLE_GL_WORK_QUEUE
    sQueue->close();
    for (int i = 0; i < THREAD_COUNT; ++i)
    {
        sVBOThread[i]->shutdown();
        delete sVBOThread[i];
        sVBOThread[i] = nullptr;
    }

    delete sQueue;
    sQueue = nullptr;
#endif
}

//----------------------------------------------------------------------------

LLVertexBuffer::LLVertexBuffer(U32 typemask)
:   LLRefCount(),
    mTypeMask(typemask)
{
    //zero out offsets
    for (U32 i = 0; i < TYPE_MAX; i++)
    {
        mOffsets[i] = 0;
    }
}

// list of mapped buffers
// NOTE: must not be LLPointer<LLVertexBuffer> to avoid breaking non-ref-counted LLVertexBuffer instances
static std::vector<LLVertexBuffer*> sMappedBuffers;

//static
void LLVertexBuffer::flushBuffers()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    // must only be called from main thread
    for (auto& buffer : sMappedBuffers)
    {
        buffer->_unmapBuffer();
        buffer->mMapped = false;
    }

    sMappedBuffers.resize(0);
}

//static
U32 LLVertexBuffer::calcOffsets(const U32& typemask, U32* offsets, U32 num_vertices)
{
    U32 offset = 0;
    for (U32 i=0; i<TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            if (offsets && LLVertexBuffer::sTypeSize[i])
            {
                offsets[i] = offset;
                offset += LLVertexBuffer::sTypeSize[i]*num_vertices;
                offset = (offset + 0xF) & ~0xF;
            }
        }
    }

    offsets[TYPE_TEXTURE_INDEX] = offsets[TYPE_VERTEX] + 12;

    return offset;
}

//static
U32 LLVertexBuffer::calcVertexSize(const U32& typemask)
{
    U32 size = 0;
    for (U32 i = 0; i < TYPE_TEXTURE_INDEX; i++)
    {
        U32 mask = 1<<i;
        if (typemask & mask)
        {
            size += LLVertexBuffer::sTypeSize[i];
        }
    }

    return size;
}

// protected, use unref()
//virtual
LLVertexBuffer::~LLVertexBuffer()
{
    if (mRHIDrawTemplate && LLRHI::sDrawTemplateReleased)
    {
        LLRHI::sDrawTemplateReleased(mRHIDrawTemplate, &mRHIDrawTemplate);   // research/46 G4c
    }
    if (mMapped)
    { // is on the mapped buffer list but doesn't need to be flushed
        mMapped = false;
        unmapBuffer();
    }

    destroyGLBuffer();
    destroyGLIndices();
    if (mRHIRef.id && gGLManager.mInited) // <plus> LLRHI (research/197 1b): the number and its place go
    {
        LLRHI::Buffers::release(mRHIRef);
        mRHIRef = LLRHI::BufferRef();
    } // </plus>

    if (mMappedData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's vertices" << LL_ENDL;
    }
    if (mMappedIndexData)
    {
        LL_ERRS() << "Failed to clear vertex buffer's indices" << LL_ENDL;
    }
};

//----------------------------------------------------------------------------

void LLVertexBuffer::genBuffer(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sVBOPool);

    if (sVBOPool)
    {
        llassert(mSize == 0);
        llassert(mGLBuffer == 0);
        llassert(mMappedData == nullptr);

        mSize = size;
        sVBOPool->allocate(GL_ARRAY_BUFFER, mSize, mGLBuffer, mMappedData);
        mRHIBuffer = LLRHI::bufferHandle(mGLBuffer); // LLRHI native (research/137 N14)
    }
}

void LLVertexBuffer::genIndices(U32 size)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    llassert(sVBOPool);

    if (sVBOPool)
    {
        llassert(mIndicesSize == 0);
        llassert(mGLIndices == 0);
        llassert(mMappedIndexData == nullptr);
        mIndicesSize = size;
        sVBOPool->allocate(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize, mGLIndices, mMappedIndexData);
        mRHIIndices = LLRHI::bufferHandle(mGLIndices); // LLRHI native (research/137 N14)
    }
}

bool LLVertexBuffer::createGLBuffer(U32 size)
{
    if (mGLBuffer || mMappedData)
    {
        destroyGLBuffer();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genBuffer(size);

    if (!mMappedData)
    {
        success = false;
    }
    return success;
}

bool LLVertexBuffer::createGLIndices(U32 size)
{
    if (mGLIndices)
    {
        destroyGLIndices();
    }

    if (size == 0)
    {
        return true;
    }

    bool success = true;

    genIndices(size);

    if (!mMappedIndexData)
    {
        success = false;
    }
    return success;
}

void LLVertexBuffer::destroyGLBuffer()
{
    if (mGLBuffer || mMappedData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sVBOPool);
        if (sVBOPool)
        {
            sVBOPool->free(GL_ARRAY_BUFFER, mSize, mGLBuffer, mMappedData);
        }

        mSize = 0;
        mGLBuffer = 0;
        mRHIBuffer = nullptr; // LLRHI native (research/137 N14)
        mMappedData = nullptr;
    }
}

void LLVertexBuffer::destroyGLIndices()
{
    if (mGLIndices || mMappedIndexData)
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
        //llassert(sVBOPool);
        if (sVBOPool)
        {
            sVBOPool->free(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize, mGLIndices, mMappedIndexData);
        }

        mIndicesSize = 0;
        mGLIndices = 0;
        mRHIIndices = nullptr; // LLRHI native (research/137 N14)
        mMappedIndexData = nullptr;
    }
}

bool LLVertexBuffer::updateNumVerts(U32 nverts)
{
    llassert(nverts >= 0);

    bool success = true;

    U32 needed_size = calcOffsets(mTypeMask, mOffsets, nverts);

    if (needed_size != mSize)
    {
        success &= createGLBuffer(needed_size);
    }

    llassert(mSize == needed_size);
    mNumVerts = nverts;
    return success;
}

bool LLVertexBuffer::updateNumIndices(U32 nindices)
{
    llassert(nindices >= 0);

    bool success = true;

    U32 needed_size = sizeof(U16) * nindices;

    if (needed_size != mIndicesSize)
    {
        success &= createGLIndices(needed_size);
    }

    llassert(mIndicesSize == needed_size);
    mNumIndices = nindices;
    return success;
}

bool LLVertexBuffer::allocateBuffer(U32 nverts, U32 nindices)
{
    if (nverts < 0 || nindices < 0)
    {
        LL_ERRS() << "Bad vertex buffer allocation: " << nverts << " : " << nindices << LL_ENDL;
    }

    bool success = true;

    // <plus> research/293 branch 0: what was there before, for the census
    const U32 old_verts = mNumVerts, old_size = mSize, old_index_size = mIndicesSize;
    // </plus>
    success &= updateNumVerts(nverts);
    success &= updateNumIndices(nindices);
    rhiLayoutChanged(); // LLRHI (research/197 1b): the number (made now) and its place
    if (LLRHIVBCensus::sOn) // <plus> research/293 branch 0 </plus>
    {
        censusAllocated(old_verts, old_size, old_index_size);
    }

    return success;
}

// <plus> LLRHI (research/197 stage 1b): the vertex buffer's number
LLRHI::BufferLayout LLVertexBuffer::rhiLayout() const
{
    LLRHI::BufferLayout now;
    now.vertex_buffer = mGLBuffer;
    now.index_buffer = mGLIndices;
    now.vertices = mNumVerts;
    now.indices = mNumIndices;
    now.index_type = mIndicesType;
    return now;
}

// The buffer was allocated again or its index type changed: a number for its
// first allocation (it keeps it from then on), else the place the backend
// keeps for the number goes if it was made for another layout (made again at
// the next draw, from GL's copy). Nothing while the backend keeps no places
// by number (it dropped them all when it stopped).
void LLVertexBuffer::rhiLayoutChanged()
{
    if (!mRHIRef.id)
    {
        if (mGLBuffer) // research/207 T2: Vulkan only
        {
            mRHIRef = LLRHI::Buffers::create();
        }
        if (LLRHIVBCensus::sOn) // research/293 branch 0
        {
            censusShape();
        }
        return;
    }
    if (LLRHI::sVBNative)
    {
        LLRHI::Buffers::layout(mRHIRef, rhiLayout());
    }
    if (LLRHIVBCensus::sOn) // research/293 branch 0
    {
        censusShape();
    }
}
// </plus>

// <plus> research/293 branch 0 (LLRHI_VB_CENSUS): counting only, nothing here changes the buffer.
// The buffer's shape now, for the census (its number's record and the owner marks of its GL names).
void LLVertexBuffer::censusShape()
{
    LLRHIVBCensus::setTypeSizes(sTypeSize, TYPE_MAX);   // kept the first time only
    if (!mRHIRef.id)
    {
        return;
    }
    LLRHIVBCensus::Shape shape;
    shape.type_mask = mTypeMask;
    shape.vertices = mNumVerts;
    shape.indices = mNumIndices;
    shape.index_type = mIndicesType;
    shape.vertex_bytes = mSize;
    shape.index_bytes = mIndicesSize;
    shape.vertex_name = mGLBuffer;
    shape.index_name = mGLIndices;
    LLRHIVBCensus::layout(mRHIRef.id, shape);
}

// allocateBuffer's outcome: which side was made again (F7), and when only the vertex side was and it
// shrank, whether an index kept from before names a vertex past the new count.
void LLVertexBuffer::censusAllocated(U32 old_verts, U32 old_size, U32 old_index_size)
{
    using namespace LLRHIVBCensus;
    count(C_ALLOCS);
    if (sSnapshot)
    {
        count(C_SNAP_ALLOCS);
    }
    if (!mNumIndices)
    {
        count(C_ALLOC_NO_INDEX);
    }
    const bool vertex_side = mSize != old_size;
    const bool index_side = mIndicesSize != old_index_size;
    if (!old_size && !old_index_size)
    {
        count(C_ALLOC_FIRST);
    }
    else if (vertex_side && index_side)
    {
        count(C_ALLOC_BOTH);
    }
    else if (vertex_side && mIndicesSize)
    {
        // Only the vertex side again, with the indices kept (a buffer without indices has nothing kept).
        count(C_ALLOC_VERTICES_ONLY);
        if (mNumVerts < old_verts)
        {
            count(C_ALLOC_VERTICES_SHRUNK);
            if (indexCopyUnknown(mRHIRef.id))
            {
                count(C_ALLOC_KEPT_INDEX_UNKNOWN);  // written by setIndexData: mMappedIndexData is not a copy of them
            }
            else if (mMappedIndexData)
            {
                const U32 units = mIndicesSize / (mIndicesType == GL_UNSIGNED_INT ? 4 : 2);
                U32 highest = 0;
                if (mIndicesType == GL_UNSIGNED_INT)
                {
                    for (U32 i = 0; i < units; ++i) highest = llmax(highest, ((const U32*)mMappedIndexData)[i]);
                }
                else
                {
                    for (U32 i = 0; i < units; ++i) highest = llmax(highest, (U32)((const U16*)mMappedIndexData)[i]);
                }
                if (highest >= mNumVerts)
                {
                    count(C_ALLOC_KEPT_INDEX_OVER);
                }
            }
        }
    }
    else if (index_side && mSize)
    {
        count(C_ALLOC_INDICES_ONLY);
    }
}

// A draw of this buffer was asked for: its number's last-drawn time, and why it will not be taken here.
void LLVertexBuffer::censusDraw(U32 mode, bool indexed, const LLGLSLShader* shader) const
{
    using namespace LLRHIVBCensus;
    if (!mRHIRef.id)
    {
        return;
    }
    drawn(mRHIRef.id);
    if (!LLRHI::sDirect)
    {
        refused(mRHIRef.id, REFUSE_NOT_DIRECT);
    }
    else if (!shader)
    {
        refused(mRHIRef.id, REFUSE_NO_SHADER);
    }
    else if (!mGLBuffer || (indexed && !mGLIndices))
    {
        refused(mRHIRef.id, REFUSE_NO_BUFFER);
    }
    else if (indexed && (mode == LLRender::TRIANGLE_FAN || mode == LLRender::LINE_LOOP))
    {
        count(C_FAN_INDEXED_DRAWS);     // drawn, from the CPU copy of the indices: not a refusal
    }
}
// </plus>

//----------------------------------------------------------------------------

// if no gap between region and given range exists, expand region to cover given range and return true
// otherwise return false
bool expand_region(LLVertexBuffer::MappedRegion& region, U32 start, U32 end)
{

    if (end < region.mStart ||
        start > region.mEnd)
    { //gap exists, do not merge
        return false;
    }

    region.mStart = llmin(region.mStart, start);
    region.mEnd = llmax(region.mEnd, end);

    return true;
}


// Map for data access
U8* LLVertexBuffer::mapVertexBuffer(LLVertexBuffer::AttributeType type, U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();

    if (count == -1)
    {
        count = mNumVerts - index;
    }

#if LL_DARWIN || LL_ARM64
    // Region tracking not needed on apple silicon - it recreates entire buffer
    // While mIsApple can be encountered under windows, this is a
    // macOS OpenGL behavior workaround. LL_ARM64 check might be not needed
    if (!gGLManager.mIsApple)
#endif
    {
        U32 start = mOffsets[type] + sTypeSize[type] * index;
        U32 end = start + sTypeSize[type] * count-1;

        bool flagged = false;
        // flag region as mapped
        for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
        {
            MappedRegion& region = mMappedVertexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedVertexRegions.push_back({ start, end });
        }
    }
    return mMappedData+mOffsets[type]+sTypeSize[type]*index;
}


U8* LLVertexBuffer::mapIndexBuffer(U32 index, S32 count)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_VERTEX;
    _mapBuffer();

    if (count == -1)
    {
        count = mNumIndices-index;
    }

#if LL_DARWIN || LL_ARM64
    if (!gGLManager.mIsApple)
#endif
    {
        U32 start = sizeof(U16) * index;
        U32 end = start + sizeof(U16) * count-1;

        bool flagged = false;
        // flag region as mapped
        for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
        {
            MappedRegion& region = mMappedIndexRegions[i];
            if (expand_region(region, start, end))
            {
                flagged = true;
                break;
            }
        }

        if (!flagged)
        {
            //didn't expand an existing region, make a new one
            mMappedIndexRegions.push_back({ start, end });
        }
    }

    return mMappedIndexData + sizeof(U16)*index;
}

// flush the given byte range
//  target -- "target" parameter for glBufferSubData
//  start -- first byte to copy
//  end -- last byte to copy (NOT last byte + 1)
//  data -- data to be flushed
//  dst -- mMappedData or mMappedIndexData
// <plus> research/293 branch 0 (LLRHI_VB_CENSUS): one write counted; counting only.
enum class CensusSource { Mapped, SetData };  // where a write's bytes come from (setXData's source is not the mapped data)
static void censusWrite(U32 number, bool indices, U32 bytes, CensusSource source)
{
    using namespace LLRHIVBCensus;
    count(C_WRITES);
    if (indices && source == CensusSource::SetData)
    {
        indexSetByData(number);     // from here mMappedIndexData is not a copy of the indices
    }
    count(C_WRITE_BYTES, bytes);
    if (indices)
    {
        count(C_WRITE_INDEX_BYTES, bytes);
    }
    if (source == CensusSource::SetData)
    {
        count(C_WRITE_SETX_BYTES, bytes);
    }
    if (sSnapshot)
    {
        count(C_SNAP_WRITES);
        count(C_SNAP_WRITE_BYTES, bytes);
    }
}
// </plus>

void LLVertexBuffer::flush_vbo(GLenum target, U32 start, U32 end, void* data, U8* dst)
{
#if LL_DARWIN || LL_ARM64
    if (gGLManager.mIsApple)
    {
        // on OS X, flush_vbo doesn't actually write to the GL buffer, so be sure to call
        // _mapBuffer to tag the buffer for flushing to GL
        _mapBuffer();
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("vb memcpy");
        STOP_GLERROR;
        // copy into mapped buffer
        memcpy(dst+start, data, end-start+1);
    }
    else
#endif
    {
        llassert(target == GL_ARRAY_BUFFER ? sGLRenderBuffer == mGLBuffer : sGLRenderIndices == mGLIndices);

        // skip mapped data and stream to GPU: native, by name, in one piece
        // (<plus> research/207 T2: Vulkan only, the GL blocks of 64 KB are gone </plus>)
        if (end != 0)
        {
            // <plus> research/293 branch 0: a write counted (setXData's source is not the mapped data)
            if (LLRHIVBCensus::sOn)
            {
                censusWrite(mRHIRef.id, target == GL_ELEMENT_ARRAY_BUFFER, end - start + 1, data != dst + start ? CensusSource::SetData : CensusSource::Mapped);
            }
            // </plus>
            rhiWrite(target == GL_ELEMENT_ARRAY_BUFFER, start, end - start + 1, data); // LLRHI (research/197 1b): the number's place first
            LLRHI::sBufferSubData(target == GL_ARRAY_BUFFER ? mGLBuffer : mGLIndices, start, end - start + 1, data);
        }
    }
}

void LLVertexBuffer::unmapBuffer()
{
    flushBuffers();
}

void LLVertexBuffer::_mapBuffer()
{
    if (!mMapped)
    {
        mMapped = true;
        sMappedBuffers.push_back(this);
    }
}

void LLVertexBuffer::_unmapBuffer()
{
    STOP_GLERROR;
    if (!mMapped)
    {
        return;
    }

    if (unmapNative()) // LLRHI native (research/137 N14)
    {
        return;
    }

    struct SortMappedRegion
    {
        bool operator()(const MappedRegion& lhs, const MappedRegion& rhs)
        {
            return lhs.mStart < rhs.mStart;
        }
    };

#if LL_DARWIN || LL_ARM64
    if (gGLManager.mIsApple)
    {
        STOP_GLERROR;
        if (mMappedData)
        {
            if (mGLBuffer)
            {
                delete_buffers(1, &mGLBuffer);
            }
            mGLBuffer = gen_buffer();
            glBindBuffer(GL_ARRAY_BUFFER, mGLBuffer);
            sGLRenderBuffer = mGLBuffer;
            glBufferData(GL_ARRAY_BUFFER, mSize, mMappedData, GL_STATIC_DRAW);
        }
        else if (mGLBuffer != sGLRenderBuffer)
        {
            glBindBuffer(GL_ARRAY_BUFFER, mGLBuffer);
            sGLRenderBuffer = mGLBuffer;
        }
        STOP_GLERROR;

        if (mMappedIndexData)
        {
            if (mGLIndices)
            {
                delete_buffers(1, &mGLIndices);
            }

            mGLIndices = gen_buffer();
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices);
            sGLRenderIndices = mGLIndices;

            glBufferData(GL_ELEMENT_ARRAY_BUFFER, mIndicesSize, mMappedIndexData, GL_STATIC_DRAW);
        }
        else if (mGLIndices != sGLRenderIndices)
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices);
            sGLRenderIndices = mGLIndices;
        }
        STOP_GLERROR;
    }
    else
#endif // LL_DARWIN || LL_ARM64
    {
        if (!mMappedVertexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - vertex");

            if (sGLRenderBuffer != mGLBuffer)
            {
                bindGLBuffer(GL_ARRAY_BUFFER, mGLBuffer); // LLRHI
            }

            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedVertexRegions.begin(), mMappedVertexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedVertexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedVertexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(GL_ARRAY_BUFFER, start, end, (U8*)mMappedData + start, mMappedData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(GL_ARRAY_BUFFER, start, end, (U8*)mMappedData + start, mMappedData);
            mMappedVertexRegions.clear();
        }

        if (!mMappedIndexRegions.empty())
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - index");

            if (mGLIndices != sGLRenderIndices)
            {
                bindGLBuffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices); // LLRHI
            }
            U32 start = 0;
            U32 end = 0;

            std::sort(mMappedIndexRegions.begin(), mMappedIndexRegions.end(), SortMappedRegion());

            for (U32 i = 0; i < mMappedIndexRegions.size(); ++i)
            {
                const MappedRegion& region = mMappedIndexRegions[i];
                if (region.mStart == end + 1)
                {
                    end = region.mEnd;
                }
                else
                {
                    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
                    start = region.mStart;
                    end = region.mEnd;
                }
            }

            flush_vbo(GL_ELEMENT_ARRAY_BUFFER, start, end, (U8*)mMappedIndexData + start, mMappedIndexData);
            mMappedIndexRegions.clear();
        }
    }
}

// LLRHI native (research/137 N14) begin
// _unmapBuffer with LLRHI_NATIVE_RESOURCES (llrhiresource.h): the same ranges
// in the same order (the regions sorted, joined where one starts right after
// the last, the empty first range skipped as flush_vbo skips a range ending
// at 0), but a buffer with the backend's handle hands all of them over in one
// LLRHI::sBufferWriteRanges call instead of one sBufferSubData each. A buffer
// without a handle goes range by range through flush_vbo, as before. The
// bindings are kept as _unmapBuffer keeps them. False: the hooks are not in,
// or GL gets the data (the caller does it the old way).
// A copy of _unmapBuffer's two loops (tools/shadowed-functions.json): when
// upstream changes them, change this as well. It runs before _unmapBuffer's
// Apple branch, which keeps no regions: the hooks are set by the Windows
// Vulkan backend only.
bool LLVertexBuffer::unmapNative()
{
    if (!LLRHI::sBufferWriteRanges) // <plus> research/207 T2: the rest are always there </plus>
    {
        return false;
    }

    struct SortMappedRegion
    {
        bool operator()(const MappedRegion& lhs, const MappedRegion& rhs)
        {
            return lhs.mStart < rhs.mStart;
        }
    };

    // flush_vbo's ranges for one buffer's regions, in _unmapBuffer's order.
    // This call's own (not a static one): nothing handed to the backend can
    // be changed by another unmap meanwhile.
    std::vector<LLRHI::BufferRange> ranges;
    auto gather = [&ranges](std::vector<MappedRegion>& regions)
    {
        ranges.clear();
        std::sort(regions.begin(), regions.end(), SortMappedRegion());
        U32 start = 0;
        U32 end = 0;
        auto add = [&ranges](U32 range_start, U32 range_end)
        {
            if (range_end != 0)
            {
                LLRHI::BufferRange range;
                range.offset = range_start;
                range.size = range_end - range_start + 1;
                ranges.push_back(range);
            }
        };
        for (U32 i = 0; i < regions.size(); ++i)
        {
            const MappedRegion& region = regions[i];
            if (region.mStart == end + 1)
            {
                end = region.mEnd;
            }
            else
            {
                add(start, end);
                start = region.mStart;
                end = region.mEnd;
            }
        }
        add(start, end);
        regions.clear();
    };
    auto write = [this, &ranges](GLenum target, U32 name, LLRHI::BufferHandle* handle, U8* data)
    {
        if (handle)
        {
            if (!ranges.empty())
            {
                // <plus> LLRHI (research/197 1b): the number's place first
                for (const LLRHI::BufferRange& range : ranges)
                {
                    if (LLRHIVBCensus::sOn) // research/293 branch 0
                    {
                        censusWrite(mRHIRef.id, target == GL_ELEMENT_ARRAY_BUFFER, range.size, CensusSource::Mapped);
                    }
                    rhiWrite(target == GL_ELEMENT_ARRAY_BUFFER, range.offset, range.size, data + range.offset);
                }
                // </plus>
                LLRHI::sBufferWriteRanges(handle, name, data, ranges.data(), (U32)ranges.size());
            }
            return;
        }
        for (const LLRHI::BufferRange& range : ranges)
        {
            flush_vbo(target, range.offset, range.offset + range.size - 1, data + range.offset, data);
        }
    };

    if (!mMappedVertexRegions.empty())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - vertex");
        if (sGLRenderBuffer != mGLBuffer)
        {
            bindGLBuffer(GL_ARRAY_BUFFER, mGLBuffer);
        }
        gather(mMappedVertexRegions);
        write(GL_ARRAY_BUFFER, mGLBuffer, mRHIBuffer, mMappedData);
    }

    if (!mMappedIndexRegions.empty())
    {
        LL_PROFILE_ZONE_NAMED_CATEGORY_VERTEX("unmapBuffer - index");
        if (mGLIndices != sGLRenderIndices)
        {
            bindGLBuffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices);
        }
        gather(mMappedIndexRegions);
        write(GL_ELEMENT_ARRAY_BUFFER, mGLIndices, mRHIIndices, mMappedIndexData);
    }
    return true;
}
// LLRHI native (research/137 N14) end

//----------------------------------------------------------------------------

template <class T,LLVertexBuffer::AttributeType type> struct VertexBufferStrider
{
    typedef LLStrider<T> strider_t;
    static bool get(LLVertexBuffer& vbo,
                    strider_t& strider,
                    S32 index, S32 count)
    {
        if (type == LLVertexBuffer::TYPE_INDEX)
        {
            U8* ptr = vbo.mapIndexBuffer(index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapIndexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(0);
            return true;
        }
        else if (vbo.hasDataType(type))
        {
            U32 stride = LLVertexBuffer::sTypeSize[type];

            U8* ptr = vbo.mapVertexBuffer(type, index, count);

            if (ptr == NULL)
            {
                LL_WARNS() << "mapVertexBuffer failed!" << LL_ENDL;
                return false;
            }

            strider = (T*)ptr;
            strider.setStride(stride);
            return true;
        }
        else
        {
            LL_ERRS() << "VertexBufferStrider could not find valid vertex data." << LL_ENDL;
        }
        return false;
    }
};

bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getVertexStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_VERTEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getIndexStrider(LLStrider<U16>& strider, U32 index, S32 count)
{
    llassert(mIndicesStride == 2); // cannot access 32-bit indices with U16 strider
    llassert(mIndicesType == GL_UNSIGNED_SHORT);
    return VertexBufferStrider<U16,TYPE_INDEX>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord0Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD0>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord1Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD1>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTexCoord2Strider(LLStrider<LLVector2>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector2,TYPE_TEXCOORD2>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getNormalStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a, TYPE_NORMAL>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector3>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector3,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getTangentStrider(LLStrider<LLVector4a>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4a,TYPE_TANGENT>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getColorStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_COLOR>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getEmissiveStrider(LLStrider<LLColor4U>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLColor4U,TYPE_EMISSIVE>::get(*this, strider, index, count);
}
bool LLVertexBuffer::getWeightStrider(LLStrider<F32>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<F32,TYPE_WEIGHT>::get(*this, strider, index, count);
}

bool LLVertexBuffer::getWeight4Strider(LLStrider<LLVector4>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4,TYPE_WEIGHT4>::get(*this, strider, index, count);
}

bool LLVertexBuffer::getClothWeightStrider(LLStrider<LLVector4>& strider, U32 index, S32 count)
{
    return VertexBufferStrider<LLVector4,TYPE_CLOTHWEIGHT>::get(*this, strider, index, count);
}

//----------------------------------------------------------------------------


// Set for rendering
void LLVertexBuffer::setBuffer()
{
    STOP_GLERROR;

    if (mMapped)
    {
        LL_WARNS_ONCE() << "Missing call to unmapBuffer or flushBuffers" << LL_ENDL;
        _unmapBuffer();
    }

    // no data may be pending
    llassert(mMappedVertexRegions.empty());
    llassert(mMappedIndexRegions.empty());

    // a shader must be bound
    llassert(LLGLSLShader::sCurBoundShaderPtr);

    U32 data_mask = LLGLSLShader::sCurBoundShaderPtr->mAttributeMask;

    // this Vertex Buffer must provide all necessary attributes for currently bound shader
    llassert_msg((data_mask & mTypeMask) == data_mask,
        "Attribute mask mismatch! mTypeMask should be a superset of data_mask.  data_mask: 0x"
                << std::hex << data_mask << " mTypeMask: 0x" << mTypeMask << " Missing: 0x" << (data_mask & ~mTypeMask) <<  std::dec
                << " shader: " << LLGLSLShader::sCurBoundShaderPtr->mName << " verts: " << mNumVerts); // <plus> which program (fix-round106) </plus>

    // LLRHI: draws go to the Vulkan backend, the attribute pointers travel
    // with each draw (drawNative); GL's are set only before a draw the backend
    // declines (ensureGLAttributes). research/207 T2: Vulkan only.
    if (sGLRenderBuffer != mGLBuffer)
    {
        bindGLBuffer(GL_ARRAY_BUFFER, mGLBuffer); // LLRHI: GL's binding may follow later
        sGLAttributesStale = true; // <plus> research/207 T2 </plus>
        sLastMask = data_mask;
    }
    else if (sLastMask != data_mask)
    {
        sGLAttributesStale = true; // <plus> research/207 T2 </plus>
        sLastMask = data_mask;
    }

    if (mGLIndices != sGLRenderIndices)
    {
        bindGLBuffer(GL_ELEMENT_ARRAY_BUFFER, mGLIndices);
    }

    STOP_GLERROR;
}


// virtual (default)
U32 LLVertexBuffer::getAttributes(U32 data_mask, LLRHI::DirectDraw::Attribute* out) const
{
    return getAttributeLayout(data_mask, out, mOffsets); // <plus> research/198 5.5: shared with the scene's layouts </plus>
}

// <plus> research/198 5.5: getAttributes' layout for any buffer (offsets null: all 0)
// static
U32 LLVertexBuffer::getAttributeLayout(U32 data_mask, LLRHI::DirectDraw::Attribute* out, const U32* offsets)
{
    auto offset_of = [offsets](AttributeType type) { return offsets ? offsets[type] : 0u; };
    U32 count = 0;
    auto add = [&](AttributeType loc, U32 size, U32 type, bool normalized, bool integer, U32 stride, U32 offset)
    {
        LLRHI::DirectDraw::Attribute& a = out[count++];
        a.location = loc;
        a.size = size;
        a.type = type;
        a.normalized = normalized;
        a.integer = integer;
        a.stride = stride;
        a.offset = offset;
        a.source = LLRHI::DirectDraw::Attribute::SOURCE_AS_LOCATION;    // research/293 branch 1: set below by the ones that read another array
        a.source_offset = 0;
    };

    if (data_mask & MAP_NORMAL)
    {
        add(TYPE_NORMAL, 3, GL_FLOAT, false, false, sTypeSize[TYPE_NORMAL], offset_of(TYPE_NORMAL));
    }
    if (data_mask & MAP_TEXCOORD3)
    {
        add(TYPE_TEXCOORD3, 2, GL_FLOAT, false, false, sTypeSize[TYPE_TEXCOORD3], offset_of(TYPE_TEXCOORD3));
    }
    if (data_mask & MAP_TEXCOORD2)
    {
        add(TYPE_TEXCOORD2, 2, GL_FLOAT, false, false, sTypeSize[TYPE_TEXCOORD2], offset_of(TYPE_TEXCOORD2));
    }
    if (data_mask & MAP_TEXCOORD1)
    {
        add(TYPE_TEXCOORD1, 2, GL_FLOAT, false, false, sTypeSize[TYPE_TEXCOORD1], offset_of(TYPE_TEXCOORD1));
    }
    if (data_mask & MAP_TANGENT)
    {
        add(TYPE_TANGENT, 4, GL_FLOAT, false, false, sTypeSize[TYPE_TANGENT], offset_of(TYPE_TANGENT));
    }
    if (data_mask & MAP_TEXCOORD0)
    {
        add(TYPE_TEXCOORD0, 2, GL_FLOAT, false, false, sTypeSize[TYPE_TEXCOORD0], offset_of(TYPE_TEXCOORD0));
    }
    if (data_mask & MAP_COLOR)
    {
        //bind emissive instead of color pointer if emissive is present
        const U32 offset = (data_mask & MAP_EMISSIVE) ? offset_of(TYPE_EMISSIVE) : offset_of(TYPE_COLOR);
        add(TYPE_COLOR, 4, GL_UNSIGNED_BYTE, true, false, sTypeSize[TYPE_COLOR], offset);
    }
    if (data_mask & MAP_EMISSIVE)
    {
        add(TYPE_EMISSIVE, 4, GL_UNSIGNED_BYTE, true, false, sTypeSize[TYPE_EMISSIVE], offset_of(TYPE_EMISSIVE));

        if (!(data_mask & MAP_COLOR))
        { //map emissive to color channel when color is not also being bound to avoid unnecessary shader swaps
            add(TYPE_COLOR, 4, GL_UNSIGNED_BYTE, true, false, sTypeSize[TYPE_EMISSIVE], offset_of(TYPE_EMISSIVE));
        }
    }
    if (data_mask & MAP_WEIGHT)
    {
        add(TYPE_WEIGHT, 1, GL_FLOAT, false, false, sTypeSize[TYPE_WEIGHT], offset_of(TYPE_WEIGHT));
    }
    if (data_mask & MAP_WEIGHT4)
    {
        add(TYPE_WEIGHT4, 4, GL_FLOAT, false, false, sTypeSize[TYPE_WEIGHT4], offset_of(TYPE_WEIGHT4));
    }
    if (data_mask & MAP_JOINT)
    {
        add(TYPE_JOINT, 4, GL_UNSIGNED_SHORT, false, true, sTypeSize[TYPE_JOINT], offset_of(TYPE_JOINT));
    }
    if (data_mask & MAP_CLOTHWEIGHT)
    {
        add(TYPE_CLOTHWEIGHT, 4, GL_FLOAT, true, false, sTypeSize[TYPE_CLOTHWEIGHT], offset_of(TYPE_CLOTHWEIGHT));
    }
    if (data_mask & MAP_TEXTURE_INDEX)
    {
        add(TYPE_TEXTURE_INDEX, 1, GL_UNSIGNED_INT, false, true, sTypeSize[TYPE_VERTEX], offset_of(TYPE_VERTEX) + 12);
        // research/293 branch 1: it reads the w of the positions (the attribute pool keeps no stream of its own for it)
        out[count - 1].source = TYPE_VERTEX;
        out[count - 1].source_offset = 12;
    }
    if (data_mask & MAP_VERTEX)
    {
        add(TYPE_VERTEX, 3, GL_FLOAT, false, false, sTypeSize[TYPE_VERTEX], offset_of(TYPE_VERTEX));
    }
    return count;
}
// </plus>

void LLVertexBuffer::setupVertexBuffer()
{
    STOP_GLERROR;
    LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
    const U32 count = getAttributes(LLGLSLShader::sCurBoundShaderPtr->mAttributeMask, attributes);
    for (U32 i = 0; i < count; ++i)
    {
        const LLRHI::DirectDraw::Attribute& a = attributes[i];
        void* ptr = (void*)(size_t)a.offset;
        if (a.integer)
        {
            glVertexAttribIPointer(a.location, a.size, a.type, a.stride, ptr);
        }
        else
        {
            glVertexAttribPointer(a.location, a.size, a.type, a.normalized ? GL_TRUE : GL_FALSE, a.stride, ptr);
        }
    }
    STOP_GLERROR;
}

void LLVertexBuffer::setPositionData(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, 0, sizeof(LLVector4a) * getNumVerts()-1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + sTypeSize[TYPE_TEXCOORD0] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + sTypeSize[TYPE_TEXCOORD1] * getNumVerts() - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_COLOR], mOffsets[TYPE_COLOR] + sTypeSize[TYPE_COLOR] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + sTypeSize[TYPE_NORMAL] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + sTypeSize[TYPE_TANGENT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + sTypeSize[TYPE_WEIGHT4] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_JOINT], mOffsets[TYPE_JOINT] + sTypeSize[TYPE_JOINT] * getNumVerts() - 1, (U8*) data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data)
{
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(U16) * getNumIndices() - 1, (U8*) data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data)
{
    if (mIndicesType != GL_UNSIGNED_INT)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndicesType = GL_UNSIGNED_INT;
        mIndicesStride = 4;
        mNumIndices /= 2;
        rhiLayoutChanged(); // LLRHI (research/197 1b)
        if (LLRHIVBCensus::sOn) LLRHIVBCensus::count(LLRHIVBCensus::C_SWITCH32_FULL); // <plus> research/293 branch 0 </plus>
    }
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(U32) * getNumIndices() - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setPositionData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, offset * sizeof(LLVector4a), (offset + count) * sizeof(LLVector4a) - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setNormalData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_NORMAL] + offset * sTypeSize[TYPE_NORMAL], mOffsets[TYPE_NORMAL] + (offset + count) * sTypeSize[TYPE_NORMAL] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord0Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD0] + offset * sTypeSize[TYPE_TEXCOORD0], mOffsets[TYPE_TEXCOORD0] + (offset + count) * sTypeSize[TYPE_TEXCOORD0] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTexCoord1Data(const LLVector2* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TEXCOORD1] + offset * sTypeSize[TYPE_TEXCOORD1], mOffsets[TYPE_TEXCOORD1] + (offset + count) * sTypeSize[TYPE_TEXCOORD1] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setColorData(const LLColor4U* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_COLOR] + offset * sTypeSize[TYPE_COLOR], mOffsets[TYPE_COLOR] + (offset + count) * sTypeSize[TYPE_COLOR] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setTangentData(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_TANGENT] + offset * sTypeSize[TYPE_TANGENT], mOffsets[TYPE_TANGENT] + (offset + count) * sTypeSize[TYPE_TANGENT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setWeight4Data(const LLVector4a* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_WEIGHT4] + offset * sTypeSize[TYPE_WEIGHT4], mOffsets[TYPE_WEIGHT4] + (offset + count) * sTypeSize[TYPE_WEIGHT4] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setJointData(const U64* data, U32 offset, U32 count)
{
    flush_vbo(GL_ARRAY_BUFFER, mOffsets[TYPE_JOINT] + offset * sTypeSize[TYPE_JOINT], mOffsets[TYPE_JOINT] + (offset + count) * sTypeSize[TYPE_JOINT] - 1, (U8*)data, mMappedData);
}

void LLVertexBuffer::setIndexData(const U16* data, U32 offset, U32 count)
{
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, offset * sizeof(U16), (offset + count) * sizeof(U16) - 1, (U8*)data, mMappedIndexData);
}

void LLVertexBuffer::setIndexData(const U32* data, U32 offset, U32 count)
{
    if (mIndicesType != GL_UNSIGNED_INT)
    { // HACK -- vertex buffers are initialized as 16-bit indices, but can be switched to 32-bit indices
        mIndicesType = GL_UNSIGNED_INT;
        mIndicesStride = 4;
        mNumIndices /= 2;
        rhiLayoutChanged(); // LLRHI (research/197 1b)
        if (LLRHIVBCensus::sOn) LLRHIVBCensus::count(LLRHIVBCensus::C_SWITCH32_PARTIAL); // <plus> research/293 branch 0 </plus>
    }
    flush_vbo(GL_ELEMENT_ARRAY_BUFFER, offset * sizeof(U32), (offset + count) * sizeof(U32) - 1, (U8*)data, mMappedIndexData);
}




