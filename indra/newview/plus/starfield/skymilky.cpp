/**
 * @file skymilky.cpp
 * @brief The baked Milky Way's picture: made on another thread, kept as a texture (research/224-star-field-v2.md).
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

#include "llviewerprecompiledheaders.h"

#include "skymilky.h"
#include "starfieldmath.h"

#include "llimagegl.h"
#include "llrender.h"
#include "llrhigl.h"

#include <new>

namespace plus::starfield
{
    namespace
    {
        math::Vec3 toMath(const LLVector3& v) { return { v.mV[VX], v.mV[VY], v.mV[VZ] }; }
    }

    MilkyBaker::MilkyBaker() = default;

    MilkyBaker::~MilkyBaker()
    {
        // At the end of the program (or when the sky object goes): a bake in flight is a second at most.
        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    void MilkyBaker::want(const LLVector3& pole, const LLVector3& centre)
    {
        Plane plane;
        plane.pole = pole;
        plane.centre = centre;
        plane.valid = true;
        mWanted = plane;

        if (plane == mShown && mTexture.notNull())
        {
            mQueued = Plane();          // what is on show is what is wanted
            if (mState.load() != IDLE)
            {
                mDiscard = true;        // a bake of another plane, in flight or done, is not taken up
            }
            return;
        }
        if (plane == mFailed)
        {
            return;                     // it failed once; not again until another plane is asked
        }
        if (mState.load() != IDLE)
        {
            // in flight, done or failed, not yet taken by poll()
            if (plane == mRunning)
            {
                mDiscard = false;
                mQueued = Plane();
            }
            else
            {
                mQueued = plane;        // after the one in flight
                mDiscard = true;
            }
            return;
        }
        start(plane);
    }

    void MilkyBaker::start(const Plane& plane)
    {
        finishThread();
        mRunning = plane;
        mDiscard = false;
        mQueued = Plane();
        mResult.reset();
        mState.store(RUNNING);

        math::BakeParams params;
        params.pole = toMath(plane.pole);
        params.centre = toMath(plane.centre);
        params.threads = 1;     // the bake is background work: one core, the picture is the same

        try
        {
            mThread = std::thread([this, params]()
            {
                int state = FAILED;
                try
                {
                    std::unique_ptr<math::BakeResult> result(new math::BakeResult(math::bake(params)));
                    // a picture of the size asked, 4 floats a texel
                    if (result->width >= 2 && result->height >= 2
                        && result->rgba.size() == (size_t)result->width * (size_t)result->height * 4)
                    {
                        mResult = std::move(result);
                        state = DONE;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    // the picture is too large for the memory left: no Milky Way picture
                }
                catch (...)
                {
                }
                mState.store(state, std::memory_order_release);
            });
        }
        catch (...)
        {
            // no thread to be had (std::system_error) or any other failure: no picture, nothing else
            LL_WARNS("Plus") << "the Milky Way's picture cannot be baked (the thread could not be started)" << LL_ENDL;
            mState.store(FAILED);
        }
    }

    void MilkyBaker::finishThread()
    {
        if (mThread.joinable())
        {
            mThread.join();
        }
    }

    void MilkyBaker::poll()
    {
        const int state = mState.load(std::memory_order_acquire);
        if (state == RUNNING || state == IDLE)
        {
            return;
        }
        finishThread();     // the thread has set its state: it is leaving, the join is at once
        if (state == FAILED)
        {
            if (!mDiscard)
            {
                mFailed = mRunning;
                LL_WARNS("Plus") << "the Milky Way's picture was not made; the soft patches are not drawn in its place" << LL_ENDL;
            }
        }
        else if (!mDiscard)
        {
            upload();
        }
        mResult.reset();
        mState.store(IDLE);
        mDiscard = false;
        if (mQueued.valid)
        {
            const Plane next = mQueued;
            mQueued = Plane();
            start(next);
        }
    }

    void MilkyBaker::upload()
    {
        const math::BakeResult& picture = *mResult;
        const U32 width = (U32)picture.width;
        const U32 height = (U32)picture.height;

        // The way gEXRImage is made (llreflectionmapmanager.cpp): a float texture, mipmapped,
        // trilinear; here it wraps round the longitude.
        U32 texName = 0;
        LLImageGL::generateTextures(1, &texName);
        LLPointer<LLImageGL> image = new LLImageGL(texName, 4, GL_TEXTURE_2D, GL_RGBA16F, GL_RGBA16F, GL_FLOAT, LLTexUnit::TAM_WRAP);
        image->setHasMipMaps(true);
        image->setUseMipMaps(true);
        image->setFilteringOption(LLTexUnit::TFO_TRILINEAR);

        gGL.getTexUnit(0)->bind(image);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, picture.rgba.data());
        LLImageGLMemory::alloc_tex_image(width, height, GL_RGBA16F, 1);
        glGenerateMipmap(GL_TEXTURE_2D);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        mTexture = image;       // the one before (if any) goes with its last reference
        mShown = mRunning;
        LL_INFOS("Plus") << "the Milky Way's picture is up (" << width << " x " << height << ")" << LL_ENDL;
    }

    void MilkyBaker::release()
    {
        mTexture = nullptr;
        mWanted = Plane();
        mShown = Plane();
        mFailed = Plane();
        mQueued = Plane();
        const int state = mState.load(std::memory_order_acquire);
        if (state == RUNNING)
        {
            mDiscard = true;            // let it finish; poll() throws it away
            return;
        }
        if (state == DONE || state == FAILED)
        {
            finishThread();
            mResult.reset();
            mState.store(IDLE);
        }
        mDiscard = false;
    }
}
