/**
 * @file skymilky.h
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

#ifndef PLUS_SKYMILKY_H
#define PLUS_SKYMILKY_H

#include "llpointer.h"
#include "stdtypes.h"
#include "v3math.h"

#include <atomic>
#include <memory>
#include <thread>

class LLImageGL;

namespace plus::starfield
{
    namespace math
    {
        struct BakeResult;
    }

    // PlusSkyStarsMilkyWayHQ: the Milky Way as a picture of the whole sky (starfieldmath.h bake(),
    // an equirectangular RGBA32F in the star field's frame), made once on a thread of its own and
    // kept as a texture (RGBA16F, mipmapped, wrapping round the longitude) for skyMilkyF.glsl.
    //
    // Everything but the bake itself is the main thread's. want() starts the bake for a plane (the
    // galactic pole and centre in the star field's frame); poll() is called every frame while it
    // is wanted and uploads the picture when the bake is done (once). A plane asked for while a
    // bake runs is taken up when it ends. release() drops the texture and what is waiting, at once:
    // a bake still running is let finish and thrown away (a bake is a second at most; the
    // destructor, at the end of the program, waits for it).
    class MilkyBaker
    {
    public:
        MilkyBaker();
        ~MilkyBaker();
        MilkyBaker(const MilkyBaker&) = delete;
        MilkyBaker& operator=(const MilkyBaker&) = delete;

        void want(const LLVector3& pole, const LLVector3& centre);
        void poll();
        void release();

        // The texture of the plane asked for, or null until its bake is uploaded. A picture of
        // another plane (the frame changed, real sky to made-up or back) is kept until the new one
        // is there but not given out: it would stand in the wrong place in the sky.
        LLImageGL* texture() const { return mWanted == mShown ? mTexture.get() : nullptr; }

    private:
        struct Plane
        {
            LLVector3 pole;
            LLVector3 centre;
            bool valid = false;
            bool operator==(const Plane& other) const
            {
                return valid == other.valid && pole == other.pole && centre == other.centre;
            }
        };

        enum State : int { IDLE = 0, RUNNING = 1, DONE = 2, FAILED = 3 };

        void start(const Plane& plane);
        void finishThread();
        void upload();

        std::thread mThread;
        std::atomic<int> mState{ IDLE };
        std::unique_ptr<math::BakeResult> mResult;  // written by the thread before DONE, read after
        Plane mRunning;         // the plane of the bake in flight (or just done)
        Plane mQueued;          // asked for while it ran
        Plane mShown;           // the plane of mTexture
        Plane mWanted;          // the plane asked for last
        Plane mFailed;          // the plane a bake failed for: not tried again until another is asked
        bool mDiscard = false;  // the bake in flight is not wanted any more
        LLPointer<LLImageGL> mTexture;
    };
}

#endif // PLUS_SKYMILKY_H
