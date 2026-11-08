/**
 * @file llimageworker.cpp
 * @brief Base class for images.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
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

#include "llimageworker.h"
#include "llimagedxt.h"
#include "threadpool.h"

#include <chrono>   // <plus> research/294: the decode stats </plus>

// <plus> research/294 (tex-fetch-flow)
LLImageDecodeThread::Stats LLImageDecodeThread::sStats;
// </plus>

/*--------------------------------------------------------------------------*/
class ImageRequest
{
public:
    ImageRequest(const LLPointer<LLImageFormatted>& image,
                 S32 discard,
                 bool needs_aux,
                 const LLPointer<LLImageDecodeThread::Responder>& responder,
                 U32 request_id);
    virtual ~ImageRequest();

    /*virtual*/ bool processRequest();
    /*virtual*/ void finishRequest(bool completed);

    // <plus> research/294: around processRequest(), in the pool's thread
    void noteStart();
    void noteEnd();
    // </plus>

private:
    // LLPointers stored in ImageRequest MUST be LLPointer instances rather
    // than references: we need to increment the refcount when storing these.
    // input
    LLPointer<LLImageFormatted> mFormattedImage;
    S32 mDiscardLevel;
    U32 mRequestId;
    bool mNeedsAux;
    // output
    LLPointer<LLImageRaw> mDecodedImageRaw;
    LLPointer<LLImageRaw> mDecodedImageAux;
    bool mDecodedRaw;
    bool mDecodedAux;
    LLPointer<LLImageDecodeThread::Responder> mResponder;
    // <plus> research/294: posted (epoch: not counted), and started by a thread of the pool
    std::chrono::steady_clock::time_point mPosted;
    std::chrono::steady_clock::time_point mStarted;
    // </plus>
    std::string mErrorString;};


//----------------------------------------------------------------------------

// MAIN THREAD
LLImageDecodeThread::LLImageDecodeThread(bool /*threaded*/)
    : mDecodeCount(0)
{
    mThreadPool = std::make_unique<LL::ThreadPool>("ImageDecode", 8);
    mThreadPool->start();
    sStats.threads.store((U32)mThreadPool->getWidth(), std::memory_order_relaxed);  // <plus> research/294 </plus>
}

//virtual
LLImageDecodeThread::~LLImageDecodeThread()
{}

// MAIN THREAD
// virtual
size_t LLImageDecodeThread::update(F32 max_time_ms)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    return getPending();
}

size_t LLImageDecodeThread::getPending()
{
    return mThreadPool->getQueue().size();
}

LLImageDecodeThread::handle_t LLImageDecodeThread::decodeImage(
    const LLPointer<LLImageFormatted>& image,
    S32 discard,
    bool needs_aux,
    const LLPointer<LLImageDecodeThread::Responder>& responder)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    U32 decode_id = ++mDecodeCount;
    if (decode_id == 0)
        decode_id = ++mDecodeCount;

    // Instantiate the ImageRequest right in the lambda, why not?
    bool posted = mThreadPool->getQueue().post(
        [req = ImageRequest(image, discard, needs_aux, responder, decode_id)]
        () mutable
        {
            req.noteStart();    // <plus> research/294 </plus>
            auto done = req.processRequest();
            req.noteEnd();      // <plus> research/294 </plus>
            req.finishRequest(done);
        });
    if (! posted)
    {
        LL_DEBUGS() << "Tried to start decoding on shutdown" << LL_ENDL;
        return 0;
    }

    return decode_id;
}

void LLImageDecodeThread::shutdown()
{
    mThreadPool->close();
}

LLImageDecodeThread::Responder::~Responder()
{
}

//----------------------------------------------------------------------------

ImageRequest::ImageRequest(const LLPointer<LLImageFormatted>& image,
                           S32 discard,
                           bool needs_aux,
                           const LLPointer<LLImageDecodeThread::Responder>& responder,
                           U32 request_id)
    : mFormattedImage(image),
      mDiscardLevel(discard),
      mNeedsAux(needs_aux),
      mDecodedRaw(false),
      mDecodedAux(false),
      mResponder(responder),
      mRequestId(request_id)
{
    // <plus> research/294
    if (LLImageDecodeThread::sStats.on.load(std::memory_order_relaxed))
    {
        mPosted = std::chrono::steady_clock::now();
    }
    // </plus>
}

// <plus> research/294 (tex-fetch-flow)
namespace
{
    void raiseMax(std::atomic<U64>& slot, U64 value)
    {
        U64 seen = slot.load(std::memory_order_relaxed);
        while (value > seen && !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed))
        {
        }
    }
}

void ImageRequest::noteStart()
{
    if (mPosted == std::chrono::steady_clock::time_point())
    {
        return;
    }
    mStarted = std::chrono::steady_clock::now();
    const U64 waited = (U64)std::chrono::duration_cast<std::chrono::microseconds>(mStarted - mPosted).count();
    LLImageDecodeThread::sStats.wait_micros.fetch_add(waited, std::memory_order_relaxed);
    raiseMax(LLImageDecodeThread::sStats.wait_max_micros, waited);
}

void ImageRequest::noteEnd()
{
    if (mPosted == std::chrono::steady_clock::time_point())
    {
        return;
    }
    const U64 ran = (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - mStarted).count();
    LLImageDecodeThread::sStats.count.fetch_add(1, std::memory_order_relaxed);
    LLImageDecodeThread::sStats.run_micros.fetch_add(ran, std::memory_order_relaxed);
    raiseMax(LLImageDecodeThread::sStats.run_max_micros, ran);
}
// </plus>

ImageRequest::~ImageRequest()
{
    mDecodedImageRaw = NULL;
    mDecodedImageAux = NULL;
    mFormattedImage = NULL;
}

//----------------------------------------------------------------------------


// Returns true when done, whether or not decode was successful.
bool ImageRequest::processRequest()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    if (mFormattedImage.isNull())
        return true;

    const F32 decode_time_slice = 0.f; //disable time slicing
    bool done = true;

    LLImageDataLock lockFormatted(mFormattedImage);
    LLImageDataLock lockDecodedRaw(mDecodedImageRaw);
    LLImageDataLock lockDecodedAux(mDecodedImageAux);

    if (!mDecodedRaw)
    {
        // Decode primary channels
        if (mDecodedImageRaw.isNull())
        {
            // parse formatted header
            if (!mFormattedImage->updateData())
            {
                return true; // done (failed)
            }
            if ((mFormattedImage->getWidth() * mFormattedImage->getHeight() * mFormattedImage->getComponents()) == 0)
            {
                return true; // done (failed)
            }
            if (mDiscardLevel >= 0)
            {
                mFormattedImage->setDiscardLevel(mDiscardLevel);
            }
            mDecodedImageRaw = new LLImageRaw(mFormattedImage->getWidth(),
                                              mFormattedImage->getHeight(),
                                              mFormattedImage->getComponents());
        }
        done = mFormattedImage->decode(mDecodedImageRaw, decode_time_slice);
        // some decoders are removing data when task is complete and there were errors
        mDecodedRaw = done && mDecodedImageRaw->getData();

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }
    if (done && mNeedsAux && !mDecodedAux && mFormattedImage.notNull())
    {
        // Decode aux channel
        if (!mDecodedImageAux)
        {
            mDecodedImageAux = new LLImageRaw(mFormattedImage->getWidth(),
                                              mFormattedImage->getHeight(),
                                              1);
        }
        done = mFormattedImage->decodeChannels(mDecodedImageAux, decode_time_slice, 4, 4);
        mDecodedAux = done && mDecodedImageAux->getData();

        // Pick up errors from decoding
        mErrorString = LLImage::getLastThreadError();
    }

    return done;
}

void ImageRequest::finishRequest(bool completed)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mResponder.notNull())
    {
        bool success = completed && mDecodedRaw && (!mNeedsAux || mDecodedAux);
        mResponder->completed(success, mErrorString, mDecodedImageRaw, mDecodedImageAux, mRequestId);
    }
    // Will automatically be deleted
}
