/**
 * @file packet.h
 * @brief Draw packets (research/180 stage 4 P1): the legacy material loop's
 *        static batches handed to the backend as packets (LLRHI::sPacketDraw).
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

#ifndef PLUS_PACKET_PACKET_H
#define PLUS_PACKET_PACKET_H

#include "stdtypes.h"

class LLDrawable;
class LLDrawInfo;
class LLGLSLShader;

// The "LLRHI native (research/180 stage 4)" line in lldrawpoolmaterials.cpp.
//
// drawMaterials is a copy of the official loop of
// LLDrawPoolMaterials::renderDeferred for a static pass (registered in
// tools/shadowed-functions.json: an upstream change of the loop shows at the
// merge). A batch a packet may take (it has a draw template, no avatar, no
// texture matrix, every texture with a GL name, its vertex buffer not mapped;
// its model matrix its region's, or, P1c, its object's in a stretch of that
// object's batches long enough, or, P1d, a shorter one inside the packet
// around it with matrices of its own) becomes a packet entry; the others, and every
// entry the backend stops at, go through the official loop's body for one
// batch (its uniform calls, binds and draw, as the official loop would make
// them at that point), entries collected before them handed over first, so
// the draws keep their order. An entry sets no uniform and binds nothing:
// the official body's "last" values stay those of the batch it drew last,
// which is what the program and the viewer's uniform cache hold.
//
// Returns false, having done nothing, when packets are off (LLRHI_PACKETS,
// one test), in census mode (it counts, research/180 10.4 P1-0) or when the
// pass cannot take packets (no backend hook, GL draws, a uniform the backend
// does not take natively): the official loop runs then. True: every batch of
// the range was drawn. Verify (LLRHI_PACKETS=verify): every batch goes the
// official way; those a packet would have taken carry their entry
// (LLRHI::PacketNote), which the backend compares with what the usual way
// resolved. Main thread, the pass's program bound, its uniforms and diffuse
// unit reset as the official code does before the loop.
namespace plus
{
namespace packet
{
    bool drawMaterials(LLGLSLShader* shader, LLDrawInfo** begin, LLDrawInfo** end, S32 diffuse_channel, S32 spec_channel,
                       S32 norm_channel);

    // LLDrawInfo::mRHIMatrixKind (research/180 10.10 P1c, the census only:
    // nothing draws by it): which branch of registerFace chose the batch's
    // model matrix, and what keeps an object's drawable active.
    enum MatrixKind : U8
    {
        KIND_REGION = 1 << 0,           // static: the region's render matrix
        KIND_ANIMATED_CHILD = 1 << 1,   // ANIMATED_CHILD: its world matrix
        KIND_ACTIVE_ROOT = 1 << 2,      // isActive, a root: its render matrix
        KIND_ACTIVE_CHILD = 1 << 3,     // isActive, a child: its parent's world matrix
        KIND_ATTACHMENT = 1 << 4,
        KIND_FLEXIBLE = 1 << 5,
        KIND_ANIMESH = 1 << 6,
        KIND_OMEGA = 1 << 7,            // an angular velocity (llTargetOmega)
    };
    // The kind of a face's new batch (registerFace, when it makes one); 0 for a rigged face.
    U8 matrixKind(const LLDrawable& drawable, bool rigged, bool region_space);
}
}

#endif // PLUS_PACKET_PACKET_H
