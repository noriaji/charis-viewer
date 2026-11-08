/**
 * @file posertools_impl.h
 * @brief Shared by posertools.cpp and posertools_{snapshot,aim,contact}.cpp only
 *        (not a public header: posertools.h is). The PlusPoserTools class,
 *        the argument and schema helpers and the types its members pass around.
 *
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

#ifndef PLUS_POSERMCP_POSERTOOLS_IMPL_H
#define PLUS_POSERMCP_POSERTOOLS_IMPL_H

// The same headers, in the same order, as the single posertools.cpp had.
#include "posertools.h"
#include "mcpschema.h"

#include "aimsolver.h"
#include "contactsolver.h"
#include "posehistorytools.h"
#include "posesolvetools.h"
#include "reachsolver.h"

#include "poser/fsfloaterposer.h"
#include "poser/fsposeranimator.h"
#include "poser/fsposingmotion.h"
#include "version/version.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"
#include "llrhihash.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llcharacter.h"
#include "llcontrolavatar.h"
#include "lldiriterator.h"
#include "llfloaterreg.h"
#include "llimagepng.h"
#include "v4coloru.h"
#include "llmutelist.h"
#include "llrender.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "pipeline.h"

#include <boost/algorithm/string/predicate.hpp>

#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace json = boost::json;
using plus::mcp::Content;
using plus::mcp::Tool;
using plus::mcp::ToolResult;
using plus::posermcp::AIM_HINGE_BACK_DEG;
using plus::posermcp::AIM_WARN_ANGLE_DEG;
using plus::posermcp::AIM_WARN_TWIST_DEG;
using plus::posermcp::AimBone;
using plus::posermcp::AimJoint;
using plus::posermcp::AimMethod;
using plus::posermcp::AimResult;
using plus::posermcp::AimTwist;
using plus::posermcp::JobProgress;
using plus::posermcp::MainThreadRunner;
using plus::posermcp::solveAim;
using plus::posermcp::solveTwoBone;
using plus::posermcp::TwoBoneInput;
using plus::posermcp::TwoBoneResult;

// What used to be the file's anonymous namespace and is used by more than one of
// the posertools*.cpp files. The definitions are in posertools.cpp unless noted.
namespace posertools_detail
{
    constexpr size_t MAX_JOINTS_PER_CALL = 200;

    using plus::mcp::ToolError;      // mcpschema.h (research/275 L110)

    // ---- argument parsing (throws ToolError with a message for the model) ----

    std::string              argString(const json::object& args, const char* key, const std::string& fallback);
    bool                     argBool(const json::object& args, const char* key, bool fallback);
    std::optional<double>    argNumber(const json::object& args, const char* key);
    std::vector<std::string> argStringList(const json::object& args, const char* key);
    LLVector3                parseVec3(const json::value& v, const std::string& what);
    double                   rounded(double value, int decimals);
    json::array              vecJson(const LLVector3& v, int decimals);
    F32                      wrapDegrees(F32 degrees);
    LLVector3                quatToEulerDegrees(const LLQuaternion& q);
    LLQuaternion             eulerDegreesToQuat(const LLVector3& degrees);
    LLVector3                quatToEulerRadians(const LLQuaternion& q);
    bool                     groupMatches(const std::string& group, E_BoneTypes type);
    void                     checkGroup(const std::string& group);

    // ---- JSON schema helpers ----

    using plus::mcp::prop;           // mcpschema.h
    json::object enumProp(std::initializer_list<const char*> values, const std::string& description);
    json::object vec3Prop(const std::string& description);
    using plus::mcp::objectSchema;   // mcpschema.h

    // Documentation shared by the tools (shown to the model in tools/list).
    extern const char AVATAR_ARG_DOC[];
    extern const char ROTATION_FRAME_DOC[];
    extern const char JOINT_NAMES_DOC[];

    // The camera a snapshot was rendered with (agent frame), for projecting
    // joints into the image.
    struct CameraShot
    {
        LLVector3 origin;
        LLVector3 at;
        LLVector3 left;
        LLVector3 up;
        F32       view   = 1.f; // vertical field of view, radians
        F32       aspect = 1.f; // of the rendered window
    };

    // ---- the body frame and the limbs (definitions under "World-space readout and IK") ----

    // The avatar's body frame: origin at the root joint, +X forward, +Y left, +Z up.
    struct BodyFrame
    {
        LLVector3    origin;
        LLQuaternion rotation; // body -> world

        LLVector3 pointToBody(const LLVector3& world) const { return (world - origin) * ~rotation; }
        LLVector3 pointToWorld(const LLVector3& body) const { return body * rotation + origin; }
        LLVector3 dirToBody(const LLVector3& world) const { return world * ~rotation; }
        LLVector3 dirToWorld(const LLVector3& body) const { return body * rotation; }
    };

    BodyFrame bodyFrame(LLVOAvatar* avatar);
    LLJoint*  boneEnd(LLVOAvatar* avatar, LLJoint* joint);

    struct Limb
    {
        const char* name;
        const char* root;    // shoulder / hip
        const char* middle;  // elbow / knee
        const char* end;     // wrist / ankle
        LLVector3   hinge;   // the middle joint's bending axis (local)
        F32         natural; // sign of the bend angle that bends the natural way
        LLVector3   pole;    // default direction the elbow/knee points to (body frame)
    };

    const std::vector<Limb>& limbs();

    struct ContactJob; // the state of a contact call across frames (posertools_contact.cpp)
}

// Found from every posertools*.cpp as the anonymous namespace was.
using namespace posertools_detail;

class PlusPoserTools
{
public:
    static ToolResult status(const json::object& args);
    static ToolResult listAvatars(const json::object& args);
    static ToolResult startPosing(const json::object& args);
    static ToolResult stopPosing(const json::object& args);
    static ToolResult listJoints(const json::object& args);
    static ToolResult getPose(const json::object& args);
    static ToolResult setJoints(const json::object& args);
    static ToolResult resetJoint(const json::object& args);
    static ToolResult resetAll(const json::object& args);
    static ToolResult savePose(const json::object& args);
    static ToolResult loadPose(const json::object& args);
    static ToolResult listPoses(const json::object& args);
    static std::optional<ToolResult> snapshot(const json::object& args, const JobProgress& progress);
    static ToolResult camera(const json::object& args);
    static std::optional<ToolResult> jointPositions(const json::object& args, const JobProgress& progress);
    static std::optional<ToolResult> reach(const json::object& args, const JobProgress& progress);
    static std::optional<ToolResult> aim(const json::object& args, const JobProgress& progress);
    static std::optional<ToolResult> contact(const json::object& args, const JobProgress& progress, ContactJob& job);

    // The pose history (posehistorytools.cpp) reads and writes the whole pose here.
    static bool        captureSelf(plus::posermcp::history::Pose& out);
    static std::size_t applySelf(const plus::posermcp::history::Pose& pose);

    // solve_pose and nudge (posesolvetools.cpp) read the skeleton and apply here.
    static void          captureForSolve(const json::object& args, const std::vector<std::string>& wanted,
                                         plus::posermcp::SolveCapture& out);
    static std::uint64_t fingerprintOf(const json::object& args);
    static bool          settledNow();
    static void          applySolved(const json::object& args, const std::vector<std::pair<std::string, LLQuaternion>>& rotations,
                                     const std::optional<LLVector3>& pelvisPosition);
    static std::vector<std::pair<std::string, LLVector3>> drawnAt(const json::object& args, const std::vector<std::string>& names);

private:
    static bool          waitForSettle(const JobProgress& progress, bool& settled);
    static void          drawSkeleton(LLImageRaw* raw, LLVOAvatar* avatar, bool hands, const CameraShot& shot);
    static json::object  joints2d(LLVOAvatar* avatar, bool hands, const CameraShot& shot, S32 width, S32 height);
    static std::optional<ToolResult> snapshotViews(const json::object& args, const JobProgress& progress);
    static void          applyRotation(FSFloaterPoser* floater, LLVOAvatar* avatar, const FSPoserAnimator::FSPoserJoint& joint,
                                       const LLQuaternion& new_rotation);
    static bool          isLoggedIn();
    static void          requireLoggedIn();
    static FSFloaterPoser* poser();
    static LLVOAvatar*   findAvatar(const LLUUID& id);
    static LLVOAvatar*   resolveAvatar(const json::object& args);
    static bool          canPose(FSFloaterPoser* floater, LLVOAvatar* avatar);
    static void          requirePosing(FSFloaterPoser* floater, LLVOAvatar* avatar);
    static const FSPoserAnimator::FSPoserJoint& requireJoint(FSFloaterPoser* floater, const std::string& name);
    static FSJointPose*  jointPose(FSFloaterPoser* floater, LLVOAvatar* avatar, const FSPoserAnimator::FSPoserJoint& joint);
    static void          refreshWindow(FSFloaterPoser* floater, bool avatars_changed);
    static json::object  avatarJson(FSFloaterPoser* floater, LLVOAvatar* avatar);
    static bool          posesSettled(FSFloaterPoser* floater);
};

namespace plus::posermcp
{
    // Wraps a plain (one-frame) tool implementation into a main-thread job.
    std::function<ToolResult(const json::object&)> simpleTool(const MainThreadRunner& run, ToolResult (*impl)(const json::object&),
                                                              double timeout);

    // Parts of the tool table, in this order after list_poses (makePoserTools).
    void appendSnapshotTools(std::vector<Tool>& tools, const MainThreadRunner& run); // snapshot, camera (posertools_snapshot.cpp)
    void appendAimTools(std::vector<Tool>& tools, const MainThreadRunner& run);      // joint_positions, reach, aim (posertools_aim.cpp)
    void appendContactTools(std::vector<Tool>& tools, const MainThreadRunner& run);  // contact (posertools_contact.cpp)
}

#endif // PLUS_POSERMCP_POSERTOOLS_IMPL_H
