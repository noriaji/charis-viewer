/**
 * @file posermotion.h
 * @brief "Motion Between Poses" (research/324): the feature's one model (machine M) and one helper client
 * (machine H), its settings, and its end at quit.
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

// Both are made on first use and never destroyed (as plus/subtitle's sharedTranslateFetch): at LLApp's "quitting"
// the client is stopped and the download cancelled (posermotion.cpp), so no thread or process is left for the
// static destructors. Main thread only. Nothing here runs until the window uses it: no timer, no thread, no helper,
// no network (a download runs only after the user confirmed it, with a 0.2 s timer of its own while it runs).

#ifndef PLUS_POSERMOTION_POSERMOTION_H
#define PLUS_POSERMOTION_POSERMOTION_H

#include "motionclient.h"
#include "motionmodel.h"

namespace plus::posermotion
{
    /// The model of the viewer (motionModelSpec() in motionModelFolder(), downloaded with subtitle::ModelDownload).
    MotionModel&  motionModel();
    /// The helper client of the viewer: charis-motion.exe next to the viewer, the model folder, PlusPoserMotionThreads,
    /// the memory check (MOTION_MIN_FREE_BYTES and memguard), and a "model" failure handed to motionModel().
    MotionClient& motionClient();

    /// PlusPoserMotionLog: one diagnostic line per request (frames, the helper's ms, the conversion's ms, the
    /// largest error at the ends). Cheap (a cached control).
    bool poserMotionLog();
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_POSERMOTION_H
