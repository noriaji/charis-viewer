/**
 * @file posertools.cpp
 * @brief The MCP tools that drive the Poser (plus/poser). See posertools.h.
 *        Here: the shared helpers, the posing tools (status .. list_poses),
 *        the pose history and solve plumbing, and the tool table. snapshot and
 *        camera are in posertools_snapshot.cpp, joint_positions / reach / aim in
 *        posertools_aim.cpp, contact in posertools_contact.cpp.
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

// All viewer work happens on the main thread (inside the steps handed to the
// MainThreadRunner); the handlers themselves run on connection threads.
//
// Avatars are looked up by UUID at the start of every step and never kept
// across steps: an avatar or animesh can disappear between two frames.
//
// The poser itself is reused, not reimplemented: the posing state lives in
// the poser window's FSPoserAnimator (the window instance is created hidden if
// it is not open), and saving/loading/permissions use the window's own
// functions. PlusPoserTools is a friend of FSFloaterPoser and FSPoserAnimator
// for that.

#include "llviewerprecompiledheaders.h"

#include "posertools_impl.h"

namespace posertools_detail
{
    constexpr char POSER_FLOATER[]       = "fs_poser";
    constexpr char POSE_SUBDIRECTORY[]   = "poses";               // same as the poser window
    constexpr F32  NEARBY_METERS         = 50.f;                   // same as the poser window
    constexpr F32  MAX_POSITION_OFFSET   = 1.5f;                   // the poser window's slider range
    constexpr F32  MIN_SCALE_OFFSET      = -0.9f;
    constexpr F32  MAX_SCALE_OFFSET      = 1.5f;

    // ---- argument parsing (throws ToolError with a message for the model) ----

    bool toNumber(const json::value& v, double& out)
    {
        if (v.is_double())
            out = v.get_double();
        else if (v.is_int64())
            out = static_cast<double>(v.get_int64());
        else if (v.is_uint64())
            out = static_cast<double>(v.get_uint64());
        else
            return false;
        // Finite and within F32: the poser keeps F32, where 1e39 would become inf.
        return std::isfinite(out) && std::fabs(out) <= std::numeric_limits<F32>::max();
    }

    std::string argString(const json::object& args, const char* key, const std::string& fallback)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return fallback;
        if (!v->is_string())
            throw ToolError(std::string("'") + key + "' must be a string");
        return std::string(v->get_string());
    }

    bool argBool(const json::object& args, const char* key, bool fallback)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return fallback;
        if (!v->is_bool())
            throw ToolError(std::string("'") + key + "' must be true or false");
        return v->get_bool();
    }

    std::optional<double> argNumber(const json::object& args, const char* key)
    {
        const json::value* v = args.if_contains(key);
        if (!v || v->is_null())
            return std::nullopt;
        double out = 0.0;
        if (!toNumber(*v, out))
            throw ToolError(std::string("'") + key + "' must be a finite number");
        return out;
    }

    std::vector<std::string> argStringList(const json::object& args, const char* key)
    {
        std::vector<std::string> out;
        const json::value*       v = args.if_contains(key);
        if (!v || v->is_null())
            return out;
        if (v->is_string())
        {
            out.emplace_back(v->get_string());
            return out;
        }
        if (!v->is_array())
            throw ToolError(std::string("'") + key + "' must be an array of strings");
        for (const json::value& item : v->get_array())
        {
            if (!item.is_string())
                throw ToolError(std::string("'") + key + "' must be an array of strings");
            out.emplace_back(item.get_string());
        }
        return out;
    }

    // [x, y, z] or {"x":..,"y":..,"z":..}
    LLVector3 parseVec3(const json::value& v, const std::string& what)
    {
        double x = 0, y = 0, z = 0;
        if (v.is_array() && v.get_array().size() == 3)
        {
            const json::array& a = v.get_array();
            if (toNumber(a[0], x) && toNumber(a[1], y) && toNumber(a[2], z))
                return LLVector3((F32)x, (F32)y, (F32)z);
        }
        else if (v.is_object())
        {
            const json::object& o = v.get_object();
            const json::value*  vx = o.if_contains("x");
            const json::value*  vy = o.if_contains("y");
            const json::value*  vz = o.if_contains("z");
            if (vx && vy && vz && toNumber(*vx, x) && toNumber(*vy, y) && toNumber(*vz, z))
                return LLVector3((F32)x, (F32)y, (F32)z);
        }
        throw ToolError(what + " must be [x, y, z] with three finite numbers");
    }

    double rounded(double value, int decimals)
    {
        const double factor = std::pow(10.0, decimals);
        double       r      = std::round(value * factor) / factor;
        return r == 0.0 ? 0.0 : r; // no "-0"
    }

    json::array vecJson(const LLVector3& v, int decimals)
    {
        return json::array{ rounded(v.mV[VX], decimals), rounded(v.mV[VY], decimals), rounded(v.mV[VZ], decimals) };
    }

    F32 wrapDegrees(F32 degrees)
    {
        F32 result = fmodf(degrees, 360.f);
        if (result > 180.f)
            result -= 360.f;
        else if (result <= -180.f)
            result += 360.f;
        return result;
    }

    // Euler convention of the poser and of LLMatrix3(roll, pitch, yaw).
    LLVector3 quatToEulerDegrees(const LLQuaternion& q)
    {
        F32 roll = 0.f, pitch = 0.f, yaw = 0.f;
        q.getEulerAngles(&roll, &pitch, &yaw);
        return LLVector3(roll, pitch, yaw) * RAD_TO_DEG;
    }

    LLQuaternion eulerDegreesToQuat(const LLVector3& degrees)
    {
        LLQuaternion q(LLMatrix3(degrees.mV[VX] * DEG_TO_RAD, degrees.mV[VY] * DEG_TO_RAD, degrees.mV[VZ] * DEG_TO_RAD));
        q.normalize();
        return q;
    }

    LLVector3 quatToEulerRadians(const LLQuaternion& q)
    {
        F32 roll = 0.f, pitch = 0.f, yaw = 0.f;
        q.getEulerAngles(&roll, &pitch, &yaw);
        return LLVector3(roll, pitch, yaw);
    }

    const char* groupName(E_BoneTypes type)
    {
        switch (type)
        {
            case WHOLEAVATAR:
            case BODY:        return "body";
            case FACE:        return "face";
            case HANDS:       return "hands";
            case MISC:        return "misc";
            case COL_VOLUMES: return "collision";
        }
        return "misc";
    }

    bool groupMatches(const std::string& group, E_BoneTypes type)
    {
        return group == "all" || group == groupName(type);
    }

    void checkGroup(const std::string& group)
    {
        static const std::vector<std::string> groups{ "body", "face", "hands", "misc", "collision", "all" };
        if (std::find(groups.begin(), groups.end(), group) == groups.end())
            throw ToolError("group must be one of body, face, hands, misc, collision, all");
    }

    // The poser window's rules too: save_pose takes the names the window saves by
    // (plus::isSafeFileStem); list_poses and load_pose take the names it lists
    // (plus::isLoadableFileStem, looser), so every name list_poses gives loads, and a pose
    // saved in the window by a Japanese name, or by Firestorm, is listed and loaded here.
    bool isValidPoseName(const std::string& name)
    {
        return plus::isSafeFileStem(name);
    }

    bool isLoadablePoseName(const std::string& name)
    {
        return plus::isLoadableFileStem(name);
    }

    std::string poseDirectory()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SUBDIRECTORY);
    }

    std::string posePath(const std::string& name, const char* extension)
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, POSE_SUBDIRECTORY, name + extension);
    }

    // ---- JSON schema helpers ----

    json::object enumProp(std::initializer_list<const char*> values, const std::string& description)
    {
        json::array list;
        for (const char* v : values)
            list.push_back(json::string(v));
        return json::object{ { "type", "string" }, { "enum", std::move(list) }, { "description", description } };
    }

    json::object vec3Prop(const std::string& description)
    {
        return json::object{ { "type", "array" },
                             { "items", json::object{ { "type", "number" } } },
                             { "minItems", 3 },
                             { "maxItems", 3 },
                             { "description", description } };
    }

    const char AVATAR_ARG_DOC[] =
        "\"self\" (default) for your own avatar, or the UUID of an animesh you own (see list_avatars).";
}

// ---------------------------------------------------------------------------
// Documentation shared by the tools (shown to the model in tools/list).

namespace posertools_detail
{
    const char ROTATION_FRAME_DOC[] =
        "ROTATION FRAME. A joint's rotation is its LOCAL rotation relative to its parent joint, measured from the "
        "default skeleton pose (rest pose = T-pose: standing straight, arms straight out to the sides, legs straight, "
        "facing forward). In the rest pose every joint's axes match the avatar body: +X = forward (where the avatar "
        "faces), +Y = the avatar's LEFT, +Z = up. Rotations are [x, y, z] in degrees: x = roll about X, y = pitch about "
        "Y, z = yaw about Z; right-handed (positive = counter-clockwise when looking from the + end of the axis). "
        "Combined angles are applied in intrinsic X, Y, Z order (roll, then pitch about the rolled Y, then yaw). Prefer "
        "changing one axis at a time. Children follow their parent: rotating mShoulderLeft moves the whole left arm. "
        "Left/right mirror images have x and z negated and y equal.\n"
        "SIGN CHEAT SHEET (derived from the axes; confirm with a snapshot): mShoulderLeft x=-80 lowers the left arm to "
        "the side, x=+80 raises it overhead (mShoulderRight: x=+80 lowers, x=-80 raises). mShoulderLeft z=+N swings "
        "the left arm backward, z=-N forward (mShoulderRight: z=-N backward, z=+N forward). mElbowLeft z=-N bends the "
        "left forearm forward (mElbowRight z=+N). mHipLeft/mHipRight y=-N lifts the thigh forward, y=+N backward. "
        "mKneeLeft/mKneeRight y=+N bends the knee (shin goes back). mAnkle y=+N points the toes down. mHead/mNeck "
        "y=+N looks down, z=+N turns to the avatar's left, x=+N tilts toward the right shoulder. mTorso/mChest y=+N "
        "bends forward. mFaceJaw y=+N opens the mouth. Fingers (palms face down in the rest pose): mHandIndex1Left "
        "x=-N curls toward the palm (right hand x=+N); thumbs are angled, check with a snapshot.";

    const char JOINT_NAMES_DOC[] =
        "JOINT NAMES (Second Life skeleton, case-insensitive). Body: mPelvis (root: rotating it turns/tilts the whole "
        "avatar; its position offset moves the whole avatar), mTorso (lower spine), mChest (upper spine), mNeck, "
        "mHead, mCollarLeft/Right (clavicle), mShoulderLeft/Right (upper arm), mElbowLeft/Right (forearm), "
        "mWristLeft/Right (hand), mHipLeft/Right (thigh), mKneeLeft/Right (shin), mAnkleLeft/Right (foot), "
        "mFootLeft/Right, mToeLeft/Right. Bento hands: mHand{Thumb|Index|Middle|Ring|Pinky}{1|2|3}{Left|Right}, "
        "1 = knuckle at the palm, 3 = fingertip segment. Bento face: mFaceRoot, mFaceJaw, mFaceChin, "
        "mFaceTeethUpper/Lower, mFaceTongueBase/Tip, mFaceLip{Upper|Lower}{Left|Center|Right}, "
        "mFaceLipCorner{Left|Right}, mFaceEyeLid{Upper|Lower}{Left|Right}, mEyeLeft/mEyeRight, "
        "mFaceEyebrow{Outer|Center|Inner}{Left|Right}, mFaceForehead*, mFaceCheek*, mFaceNose*, mFaceEar*. Misc: "
        "mSpine1-4 (extra spine links, usually leave alone), mSkull, mTail1-6, mWing*, mHindLimb*, mGroin. Collision "
        "volumes (UPPER CASE, e.g. CHEST, BELLY, L_UPPER_ARM) only take position/scale and reshape the body; "
        "normally leave them alone. Use list_joints for the exact list.";
}

namespace plus::posermcp
{
    std::string serverInstructions()
    {
        return std::string(
                   "Poses avatars in Second Life through this viewer's Poser (your own avatar, or animesh you own, in the "
                   "current region). Everything is local to this viewer: other people see the pose only if it is saved "
                   "and played as an animation, so experiment freely.\n"
                   "RECOMMENDED LOOP: status -> start_posing -> camera (frame the part you work on) -> set_joints -> "
                   "snapshot -> look at the image -> correct with set_joints (mode \"delta\" for small corrections) -> "
                   "snapshot again ... -> save_pose. Change a few joints per step and check each step with a snapshot; "
                   "use get_pose to read current angles. undo returns to the pose before a call (reset_joint / "
                   "reset_all go back to the starting pose).\n") +
               historyInstructions() + solveInstructions() +
               std::string(
                   "PLACE HANDS AND FEET WITH reach (IK: give the point, it solves shoulder/elbow or hip/knee; bend_toward "
                   "picks where the elbow/knee points) instead of guessing arm and leg angles. CHECK WITH NUMBERS: "
                   "joint_positions gives joint positions and bone directions in the avatar's body frame, so contact "
                   "(hand on chin, foot on floor) and alignment (vertical shin) can be verified without an image.\n"
                   "SET POSTURE WITH aim: give bone directions (body-frame unit vectors, e.g. upper arm, forearm, thigh, "
                   "shin, spine, head) for the whole body in one call instead of Euler angles; give an arm's or leg's two "
                   "bones together so the elbow/knee bends naturally. CHECK FROM SEVERAL SIDES WITH snapshot views (e.g. "
                   "views [\"front\",\"left\",\"back\"], skeleton \"body\"): rendered off-screen in one call, the user's "
                   "camera does not move. TO COPY A PHOTO: turn its keypoints into bone directions -> aim (from_tpose) -> "
                   "snapshot views from the photo's angle (joints_2d gives pixel positions to compare) -> fix with aim, "
                   "contact to make hands and feet touch what they touch in the photo (hand on foot, hand on chin ...: "
                   "it moves the arms / legs until the skin points meet; check from above too), set_joints for fingers "
                   "and twists.\n"
                   "aim KEEPS THE CURRENT TWIST of each bone, so a mistake (a wrong roll_toward) stays in the pose: to "
                   "start over, call reset_all (to \"tpose\") first, then aim. Do not give roll_toward to elbows, knees, "
                   "wrists or ankles from a photo: SL has no forearm / shin twist joint, so a twist there folds the mesh "
                   "(elbows and knees ignore it). A nearly straight arm or leg (less than 15 degrees between its bones) "
                   "gets the rest pose's twist; set a standing foot flat with mAnkle direction [0.88,0,-0.48] + "
                   "roll_toward [0,0,1] and mFoot direction [1,0,0]. Read aim's warnings (joints turned or twisted far "
                   "enough to fold the mesh, elbows / knees that can not reach the direction).\n"
                   "A POSE IS NOT DONE UNTIL IT IS DONE TO THE FINGERTIPS. Like arms and legs, always decide: each "
                   "hand's shape (fingers: relaxed curl, fist, flat together, pointing, gripping...), each wrist's "
                   "angle (where the fingers and palm point), the head and neck, and the feet/toes. from_tpose (and "
                   "reset_all to=\"tpose\") leaves the fingers straight and spread like a starfish, which almost never "
                   "looks natural; reach and set_joints on arms never touch the fingers. Check the hands in a close "
                   "snapshot (camera focus left_hand/right_hand) before save_pose; save_pose warns about hands that "
                   "are still flat.\n") +
               ROTATION_FRAME_DOC + "\n" + JOINT_NAMES_DOC;
    }
}

bool PlusPoserTools::isLoggedIn()
{
    return LLStartUp::getStartupState() == STATE_STARTED && isAgentAvatarValid() && gAgent.getRegion();
}

void PlusPoserTools::requireLoggedIn()
{
    if (!isLoggedIn())
        throw ToolError("The viewer is not logged in to a region yet.");
}

FSFloaterPoser* PlusPoserTools::poser()
{
    // The posing state lives in the poser window's animator. Creating the
    // instance does not open the window.
    FSFloaterPoser* floater = LLFloaterReg::getTypedInstance<FSFloaterPoser>(POSER_FLOATER);
    if (!floater)
        throw ToolError("The poser is not available in this viewer build.");
    return floater;
}

LLVOAvatar* PlusPoserTools::findAvatar(const LLUUID& id)
{
    if (id.isNull())
        return nullptr;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        if (character->getID() != id)
            continue;
        // The ones list_avatars offers: the own avatar, or an animesh that is not muted
        // (other people's avatars are neither posed nor read here).
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead())
            continue;
        if (avatar->isSelf())
            return avatar;
        if (avatar->isControlAvatar() && !LLMuteList::getInstance()->isMuted(avatar->getID()))
            return avatar;
    }
    return nullptr;
}

LLVOAvatar* PlusPoserTools::resolveAvatar(const json::object& args)
{
    requireLoggedIn();

    const std::string which = argString(args, "avatar", "self");
    if (which.empty() || boost::iequals(which, "self"))
        return gAgentAvatarp.get();

    if (!LLUUID::validate(which))
        throw ToolError("'avatar' must be \"self\" or a UUID from list_avatars");

    LLVOAvatar* avatar = findAvatar(LLUUID(which));
    if (!avatar)
        throw ToolError("No animesh " + which + " is around (other people's avatars are not offered); call list_avatars.");
    return avatar;
}

bool PlusPoserTools::canPose(FSFloaterPoser* floater, LLVOAvatar* avatar)
{
    return floater->couldAnimateAvatar(avatar) && floater->havePermissionToAnimateAvatar(avatar);
}

void PlusPoserTools::requirePosing(FSFloaterPoser* floater, LLVOAvatar* avatar)
{
    if (!floater->mPoserAnimator.isPosingAvatar(avatar))
        throw ToolError("Not posing this avatar; call start_posing first.");
}

const FSPoserAnimator::FSPoserJoint& PlusPoserTools::requireJoint(FSFloaterPoser* floater, const std::string& name)
{
    const FSPoserAnimator::FSPoserJoint* joint = floater->mPoserAnimator.getPoserJointByName(name);
    if (!joint)
        throw ToolError("Unknown joint '" + name + "'; see list_joints for the names.");
    return *joint;
}

FSJointPose* PlusPoserTools::jointPose(FSFloaterPoser* floater, LLVOAvatar* avatar, const FSPoserAnimator::FSPoserJoint& joint)
{
    FSPosingMotion* motion = floater->mPoserAnimator.getPosingMotion(avatar);
    return motion ? motion->getJointPoseByJointName(joint.jointName()) : nullptr;
}

void PlusPoserTools::refreshWindow(FSFloaterPoser* floater, bool avatars_changed)
{
    // Keep the poser window in step with what we changed, if it is open.
    if (!floater->getVisible())
        return;

    if (avatars_changed)
    {
        floater->onAvatarsRefresh();
        floater->onAvatarSelect();
    }
    floater->refreshTextHighlightingOnJointScrollLists();
    floater->onJointTabSelect();
}

json::object PlusPoserTools::avatarJson(FSFloaterPoser* floater, LLVOAvatar* avatar)
{
    json::object entry;
    entry["id"] = avatar->getID().asString();
    if (avatar->isSelf())
    {
        entry["kind"] = "self";
        entry["name"] = avatar->getFullname();
    }
    else
    {
        entry["kind"] = "animesh";
        entry["name"] = floater->getControlAvatarName(dynamic_cast<LLControlAvatar*>(avatar));
    }
    if (isAgentAvatarValid())
    {
        const F32 distance = (avatar->getCharacterPosition() - gAgentAvatarp->getCharacterPosition()).magVec();
        entry["distance_m"] = rounded(distance, 1);
    }
    entry["can_pose"] = canPose(floater, avatar);
    entry["posing"]   = floater->mPoserAnimator.isPosingAvatar(avatar);
    return entry;
}

ToolResult PlusPoserTools::status(const json::object&)
{
    json::object result;
    result["viewer"]    = plus::version::displayVersion();
    result["logged_in"] = isLoggedIn();
    result["poser_window_open"] = LLFloaterReg::instanceVisible(POSER_FLOATER);
    if (gViewerWindow)
    {
        result["world_view_size"] = json::array{ gViewerWindow->getWorldViewWidthRaw(), gViewerWindow->getWorldViewHeightRaw() };
    }

    if (isLoggedIn())
    {
        // RLVa @showloc: the region is not told (plus/rlv, research/248 3.4)
        const std::string& region = gAgent.getRegion()->getName();
        result["region"] = plus::rlv::enabled() ? plus::rlv::regionNameForPlus(region) : region;
        FSFloaterPoser* floater = poser();
        result["self"]          = avatarJson(floater, gAgentAvatarp.get());

        json::array posing;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
            if (avatar && !avatar->isDead() && floater->mPoserAnimator.isPosingAvatar(avatar))
                posing.push_back(json::string(avatar->getID().asString()));
        }
        result["posing"]      = std::move(posing);
        result["camera_mode"] = gAgentCamera.cameraMouselook() ? "mouselook" : "third_person";
    }
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::listAvatars(const json::object&)
{
    requireLoggedIn();
    FSFloaterPoser* floater = poser();

    json::array list;
    list.push_back(avatarJson(floater, gAgentAvatarp.get()));
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead() || avatar->isSelf() || !avatar->isControlAvatar())
            continue;
        if ((avatar->getCharacterPosition() - gAgentAvatarp->getCharacterPosition()).magVec() >= NEARBY_METERS)
            continue;
        if (LLMuteList::getInstance()->isMuted(avatar->getID()))
            continue;
        list.push_back(avatarJson(floater, avatar));
    }

    json::object result;
    result["avatars"] = std::move(list);
    result["note"]    = "Only yourself and animesh you own (can_pose) can be posed; other people's avatars cannot.";
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::startPosing(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    const bool from_tpose   = argBool(args, "from_tpose", false);

    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");

    FSPoserAnimator& animator = floater->mPoserAnimator;
    const bool was_posing     = animator.isPosingAvatar(avatar);
    if (!was_posing)
        animator.tryPosingAvatar(avatar);
    if (!animator.isPosingAvatar(avatar))
        throw ToolError("The poser could not start on this avatar.");

    if (from_tpose)
        animator.setAllAvatarStartingRotationsToZero(avatar);

    refreshWindow(floater, true);

    json::object result;
    result["posing"]          = true;
    result["already_posing"]  = was_posing;
    result["from_tpose"]      = from_tpose;
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::stopPosing(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();

    const bool was_posing = floater->mPoserAnimator.isPosingAvatar(avatar);
    if (was_posing)
        floater->mPoserAnimator.stopPosingAvatar(avatar);
    refreshWindow(floater, true);

    json::object result;
    result["posing"]     = false;
    result["was_posing"] = was_posing;
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::listJoints(const json::object& args)
{
    FSFloaterPoser*   floater = poser();
    const std::string group   = argString(args, "group", "body");
    checkGroup(group);

    // The skeleton (for parents) is optional: joint names are known without it.
    LLVOAvatar* avatar = nullptr;
    if (isLoggedIn())
        avatar = resolveAvatar(args);
    const bool posing = avatar && floater->mPoserAnimator.isPosingAvatar(avatar);

    json::array joints;
    for (const FSPoserAnimator::FSPoserJoint& joint : floater->mPoserAnimator.PoserJoints)
    {
        if (!groupMatches(group, joint.boneType()))
            continue;

        json::object entry;
        entry["name"]  = joint.jointName();
        std::string title = floater->tryGetString("title_" + joint.jointName());
        if (!title.empty() && title != joint.jointName())
            entry["title"] = title;
        entry["group"] = groupName(joint.boneType());
        if (!joint.mirrorJointName().empty())
            entry["mirror"] = joint.mirrorJointName();

        if (avatar)
        {
            LLJoint* ll_joint = avatar->getJoint(joint.jointName());
            if (ll_joint && ll_joint->getParent())
                entry["parent"] = ll_joint->getParent()->getName();
        }

        const bool cv = joint.boneType() == COL_VOLUMES;
        entry["rotation"] = !cv;
        entry["position"] = true;
        entry["scale"]    = true;
        if (posing)
            entry["enabled"] = floater->mPoserAnimator.isPosingAvatarJoint(avatar, joint);
        joints.push_back(std::move(entry));
    }

    json::object result;
    result["group"]  = group;
    result["joints"] = std::move(joints);
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::getPose(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    requirePosing(floater, avatar);
    FSPoserAnimator& animator = floater->mPoserAnimator;

    std::vector<std::string> names        = argStringList(args, "joints");
    const std::string        group        = argString(args, "group", "body");
    const bool               changed_only = argBool(args, "changed_only", false);
    checkGroup(group);

    std::vector<const FSPoserAnimator::FSPoserJoint*> selected;
    if (!names.empty())
    {
        if (names.size() > MAX_JOINTS_PER_CALL)
            throw ToolError("Too many joints in one call");
        for (const std::string& name : names)
            selected.push_back(&requireJoint(floater, name));
    }
    else
    {
        for (const FSPoserAnimator::FSPoserJoint& joint : animator.PoserJoints)
        {
            if (groupMatches(group, joint.boneType()))
                selected.push_back(&joint);
        }
    }

    json::array joints;
    for (const FSPoserAnimator::FSPoserJoint* joint : selected)
    {
        FSJointPose* pose = jointPose(floater, avatar, *joint);
        if (!pose)
            continue; // not in this avatar's skeleton

        const bool changed = animator.hasJointBeenChanged(avatar, *joint);
        if (changed_only && !changed)
            continue;

        json::object entry;
        entry["joint"] = joint->jointName();
        if (joint->boneType() != COL_VOLUMES)
            entry["rotation"] = vecJson(quatToEulerDegrees(pose->getTargetRotation()), 2);
        entry["position"] = vecJson(pose->getPublicPosition(), 4);
        entry["scale"]    = vecJson(pose->getPublicScale(), 4);
        entry["enabled"]  = animator.isPosingAvatarJoint(avatar, *joint);
        entry["changed"]  = changed;
        joints.push_back(std::move(entry));
    }

    json::object result;
    result["avatar"] = avatar->getID().asString();
    result["joints"] = std::move(joints);
    result["units"]  = "rotation: local Euler degrees [x,y,z] (see ROTATION FRAME); position: offset in meters from the "
                       "joint's starting position, in the parent's frame; scale: offset added to the starting scale";
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::setJoints(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);
    FSPoserAnimator& animator = floater->mPoserAnimator;

    const std::string mode = argString(args, "mode", "absolute");
    if (mode != "absolute" && mode != "delta")
        throw ToolError("mode must be \"absolute\" or \"delta\"");
    const bool delta  = mode == "delta";
    const bool mirror = argBool(args, "mirror", false);

    const json::value* raw_joints = args.if_contains("joints");
    if (!raw_joints || !raw_joints->is_array() || raw_joints->get_array().empty())
        throw ToolError("'joints' must be a non-empty array of {joint, rotation?, position?, scale?}");
    if (raw_joints->get_array().size() > MAX_JOINTS_PER_CALL)
        throw ToolError("Too many joints in one call");

    struct Change
    {
        const FSPoserAnimator::FSPoserJoint* joint = nullptr;
        std::optional<LLVector3>             rotation;
        std::optional<LLVector3>             position;
        std::optional<LLVector3>             scale;
    };

    // Validate everything before touching the pose.
    std::vector<Change> changes;
    for (const json::value& item : raw_joints->get_array())
    {
        if (!item.is_object())
            throw ToolError("each entry of 'joints' must be an object");
        const json::object& entry = item.get_object();

        Change change;
        change.joint = &requireJoint(floater, argString(entry, "joint", ""));
        const std::string& name = change.joint->jointName();

        for (const auto& kv : entry)
        {
            if (kv.key() != "joint" && kv.key() != "rotation" && kv.key() != "position" && kv.key() != "scale")
                throw ToolError("unknown field '" + std::string(kv.key()) + "' for " + name);
        }

        if (const json::value* v = entry.if_contains("rotation"); v && !v->is_null())
        {
            if (change.joint->boneType() == COL_VOLUMES)
                throw ToolError(name + " is a collision volume: it takes position/scale only");
            LLVector3 r = parseVec3(*v, name + " rotation");
            for (int i = 0; i < 3; ++i)
            {
                if (delta && fabsf(r.mV[i]) > 360.f)
                    throw ToolError(name + " delta rotation must be within +-360 degrees");
                if (!delta)
                    r.mV[i] = wrapDegrees(r.mV[i]);
            }
            change.rotation = r;
        }
        if (const json::value* v = entry.if_contains("position"); v && !v->is_null())
            change.position = parseVec3(*v, name + " position");
        if (const json::value* v = entry.if_contains("scale"); v && !v->is_null())
            change.scale = parseVec3(*v, name + " scale");

        if (!change.rotation && !change.position && !change.scale)
            throw ToolError(name + ": give rotation, position and/or scale");
        if (!jointPose(floater, avatar, *change.joint))
            throw ToolError(name + " is not part of this avatar's skeleton");
        changes.push_back(change);
    }

    auto listed = [&](const std::string& name)
    {
        return std::any_of(changes.begin(), changes.end(), [&](const Change& c) { return boost::iequals(c.joint->jointName(), name); });
    };

    json::array report;
    std::vector<std::string> notes;
    for (const Change& change : changes)
    {
        const FSPoserAnimator::FSPoserJoint* joint = change.joint;
        FSJointPose* pose = jointPose(floater, avatar, *joint);
        if (!pose)
            continue;

        // A joint the user switched off in the window is switched back on.
        animator.setPosingAvatarJoint(avatar, *joint, true);

        // Mirror onto the opposite joint unless the call sets that one itself.
        const FSPoserAnimator::FSPoserJoint* opposite =
            joint->mirrorJointName().empty() ? nullptr : animator.getPoserJointByName(joint->mirrorJointName());
        const bool mirror_this = mirror && opposite && !listed(opposite->jointName()) && jointPose(floater, avatar, *opposite);
        if (mirror_this)
            animator.setPosingAvatarJoint(avatar, *opposite, true);

        json::object entry;
        entry["joint"] = joint->jointName();

        if (change.rotation)
        {
            const LLQuaternion old_rotation = pose->getTargetRotation();
            // delta: applied after the current rotation, about the parent's axes
            const LLQuaternion new_rotation =
                delta ? old_rotation * eulerDegreesToQuat(*change.rotation) : eulerDegreesToQuat(*change.rotation);
            const LLQuaternion change_rotation = new_rotation * ~old_rotation;

            // The poser's own path: absolute rotation in the bone frame, with the
            // animation's base rotation zeroed so the value is the whole local rotation.
            animator.setJointRotation(avatar, joint, quatToEulerRadians(new_rotation), quatToEulerRadians(change_rotation),
                                      mirror_this ? MIRROR : NONE, POSER_FRAME_BONE, SWAP_NOTHING, NEGATE_NOTHING, true,
                                      ABSOLUTE_ROT);
            entry["rotation"] = vecJson(quatToEulerDegrees(pose->getTargetRotation()), 2);
            if (mirror_this)
                entry["mirrored_to"] = opposite->jointName();
        }

        if (change.position)
        {
            LLVector3 position = delta ? animator.getJointPosition(avatar, *joint) + *change.position : *change.position;
            LLVector3 clamped  = position;
            clamped.clamp(-MAX_POSITION_OFFSET, MAX_POSITION_OFFSET);
            if (clamped != position)
                notes.push_back(joint->jointName() + " position clamped to +-1.5 m");
            animator.setJointPosition(avatar, joint, clamped, POSER_FRAME_BONE, NONE);
            entry["position"] = vecJson(pose->getPublicPosition(), 4);
        }

        if (change.scale)
        {
            LLVector3 scale   = delta ? animator.getJointScale(avatar, *joint) + *change.scale : *change.scale;
            LLVector3 clamped = scale;
            clamped.clamp(MIN_SCALE_OFFSET, MAX_SCALE_OFFSET);
            if (clamped != scale)
                notes.push_back(joint->jointName() + " scale offset clamped to [-0.9, 1.5]");
            animator.setJointScale(avatar, joint, clamped, POSER_FRAME_BONE, NONE);
            entry["scale"] = vecJson(pose->getPublicScale(), 4);
        }

        report.push_back(std::move(entry));
    }

    refreshWindow(floater, false);

    json::object result;
    result["mode"]   = mode;
    result["joints"] = std::move(report);
    if (!notes.empty())
    {
        json::array list;
        for (const std::string& note : notes)
            list.push_back(json::string(note));
        result["notes"] = std::move(list);
    }
    result["next"] = "Take a snapshot to check the result.";
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::resetJoint(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    requirePosing(floater, avatar);

    std::vector<std::string> names = argStringList(args, "joints");
    if (names.empty())
        throw ToolError("'joints' must list at least one joint");
    if (names.size() > MAX_JOINTS_PER_CALL)
        throw ToolError("Too many joints in one call");
    const bool mirror = argBool(args, "mirror", false);

    std::vector<const FSPoserAnimator::FSPoserJoint*> joints;
    for (const std::string& name : names)
        joints.push_back(&requireJoint(floater, name));

    json::array done;
    for (const FSPoserAnimator::FSPoserJoint* joint : joints)
    {
        floater->mPoserAnimator.resetJoint(avatar, *joint, mirror ? MIRROR : NONE);
        done.push_back(json::string(joint->jointName()));
    }
    refreshWindow(floater, false);

    json::object result;
    result["reset"]  = std::move(done);
    result["mirror"] = mirror;
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::resetAll(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    requirePosing(floater, avatar);
    FSPoserAnimator& animator = floater->mPoserAnimator;

    const std::string to = argString(args, "to", "start");
    if (to == "start")
    {
        for (const FSPoserAnimator::FSPoserJoint& joint : animator.PoserJoints)
            animator.resetJoint(avatar, joint, NONE);
    }
    else if (to == "tpose")
    {
        animator.setAllAvatarStartingRotationsToZero(avatar);
    }
    else
    {
        throw ToolError("'to' must be \"start\" or \"tpose\"");
    }
    refreshWindow(floater, true);

    json::object result;
    result["reset_to"] = to;
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::savePose(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    requirePosing(floater, avatar);

    const std::string name = argString(args, "name", "");
    if (!isValidPoseName(name))
        throw ToolError("'name' must be a file name of 1-200 bytes: no path (\\ / :), none of * ? \" < > |, no '..', "
                        "no dot or space at either end, not a device name such as CON");
    const bool bvh       = argBool(args, "bvh", false);
    const bool overwrite = argBool(args, "overwrite", false);

    const std::string xml_path = posePath(name, ".xml");
    if (gDirUtilp->fileExists(xml_path) && !overwrite)
        throw ToolError("A pose named '" + name + "' exists; pass overwrite=true to replace it.");

    floater->createUserPoseDirectoryIfNeeded();
    if (!floater->savePoseToXml(avatar, name))
        throw ToolError("Saving the pose failed (see the viewer log).");

    json::object result;
    result["saved"] = xml_path;
    if (bvh)
    {
        if (floater->savePoseToBvh(avatar, name))
            result["bvh"] = posePath(name, ".bvh");
        else
            result["bvh_error"] = "BVH export failed (see the viewer log)";
    }
    if (floater->getVisible())
        floater->refreshPoseScroll(floater->mPosesScrollList);

    // A hand whose fingers are all still at zero is the flat, spread T-pose hand.
    json::array flat_hands;
    for (const char* side : { "Left", "Right" })
    {
        bool any_finger = false, all_flat = true;
        for (const char* finger : { "Thumb", "Index", "Middle", "Ring", "Pinky" })
        {
            for (int segment = 1; segment <= 3; ++segment)
            {
                const FSPoserAnimator::FSPoserJoint* joint =
                    floater->mPoserAnimator.getPoserJointByName(std::string("mHand") + finger + std::to_string(segment) + side);
                FSJointPose* pose = joint ? jointPose(floater, avatar, *joint) : nullptr;
                if (!pose)
                    continue;
                any_finger       = true;
                const F32 cosine = fabsf(pose->getTargetRotation().mQ[VW]);
                if (2.f * acosf(llmin(cosine, 1.f)) > 2.f * DEG_TO_RAD)
                    all_flat = false;
            }
        }
        if (any_finger && all_flat)
            flat_hands.push_back(json::string(std::string(side) == "Left" ? "left" : "right"));
    }
    if (!flat_hands.empty())
    {
        result["flat_hands"] = std::move(flat_hands);
        result["warning"]    = "Saved, but these hands still have the T-pose's straight, spread fingers. Unless that is "
                               "intended, shape the fingers (and the wrist) with set_joints and save again.";
    }
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::loadPose(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);

    const std::string name = argString(args, "name", "");
    if (!isLoadablePoseName(name))
        throw ToolError("'name' must be a pose name from list_poses");
    if (!gDirUtilp->fileExists(posePath(name, ".xml")))
        throw ToolError("No pose named '" + name + "'; see list_poses.");

    const std::string what = argString(args, "what", "all");
    E_LoadPoseMethods method;
    if (what == "all")
        method = ROT_POS_AND_SCALES;
    else if (what == "rotation")
        method = ROTATIONS;
    else if (what == "position")
        method = POSITIONS;
    else if (what == "scale")
        method = SCALES;
    else if (what == "rot_pos")
        method = ROTATIONS_AND_POSITIONS;
    else if (what == "rot_scale")
        method = ROTATIONS_AND_SCALES;
    else if (what == "pos_scale")
        method = POSITIONS_AND_SCALES;
    else
        throw ToolError("'what' must be all, rotation, position, scale, rot_pos, rot_scale or pos_scale");

    if (!floater->loadPoseFromXml(avatar, name, method))
        throw ToolError("Loading the pose failed (see the viewer log).");
    refreshWindow(floater, true);

    json::object result;
    result["loaded"] = name;
    result["what"]   = what;
    return ToolResult::json(result);
}

ToolResult PlusPoserTools::listPoses(const json::object&)
{
    json::array       poses;
    const std::string dir = poseDirectory();
    if (gDirUtilp->fileExists(dir))
    {
        std::vector<std::string> names;
        std::string              file;
        LLDirIterator            iter(dir, "*.xml");
        while (iter.next(file))
        {
            const std::string name = gDirUtilp->getBaseFileName(file, true);
            if (name != file && isLoadablePoseName(name)) // as the poser window lists them (not ".xml" itself): only names load_pose takes
                names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (const std::string& name : names)
            poses.push_back(json::string(name));
    }

    json::object result;
    result["folder"] = dir;
    result["poses"]  = std::move(poses);
    return ToolResult::json(result);
}

bool PlusPoserTools::posesSettled(FSFloaterPoser* floater)
{
    // The posing motion eases every joint toward its target (25% per frame);
    // the pose is settled when the joints are within half a degree / 1 mm.
    const F32 max_angle = 0.5f * DEG_TO_RAD;
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(character);
        if (!avatar || avatar->isDead())
            continue;
        FSPosingMotion* motion = floater->mPoserAnimator.getPosingMotion(avatar);
        if (!motion || motion->isStopped())
            continue;

        for (const FSPoserAnimator::FSPoserJoint& joint : floater->mPoserAnimator.PoserJoints)
        {
            FSJointPose* pose = motion->getJointPoseByJointName(joint.jointName());
            if (!pose || !motion->currentlyPosingJoint(pose))
                continue;
            LLJoint* ll_joint = pose->getJointState()->getJoint();
            if (!ll_joint)
                continue;

            const F32 cosine = fabsf(dot(ll_joint->getRotation(), pose->getTargetRotation()));
            if (2.f * acosf(llmin(cosine, 1.f)) > max_angle)
                return false;
            if ((ll_joint->getPosition() - pose->getTargetPosition()).magVec() > 0.001f)
                return false;
        }
    }
    return true;
}

bool PlusPoserTools::waitForSettle(const JobProgress& progress, bool& settled)
{
    // At least two frames, at most ~4 s (a higher-priority animation can hold a joint).
    settled = posesSettled(poser());
    return progress.calls >= 2 && (settled || progress.firstCallAge >= 4.0);
}

void PlusPoserTools::applyRotation(FSFloaterPoser* floater, LLVOAvatar* avatar, const FSPoserAnimator::FSPoserJoint& joint,
                                   const LLQuaternion& new_rotation)
{
    FSJointPose* pose = jointPose(floater, avatar, joint);
    if (!pose)
        throw ToolError(joint.jointName() + " is not part of this avatar's skeleton");
    FSPoserAnimator& animator = floater->mPoserAnimator;
    animator.setPosingAvatarJoint(avatar, joint, true);
    const LLQuaternion change_rotation = new_rotation * ~pose->getTargetRotation();
    animator.setJointRotation(avatar, &joint, quatToEulerRadians(new_rotation), quatToEulerRadians(change_rotation), NONE,
                              POSER_FRAME_BONE, SWAP_NOTHING, NEGATE_NOTHING, true, ABSOLUTE_ROT);
}

// ---------------------------------------------------------------------------
// The body frame and the limbs, for the world-space readout and IK
// (posertools_aim.cpp), contact (posertools_contact.cpp), snapshot views and
// the solve plumbing below.

namespace posertools_detail
{
    BodyFrame bodyFrame(LLVOAvatar* avatar)
    {
        LLJoint* root = avatar->getRootJoint();
        if (!root)
            throw ToolError("This avatar has no skeleton yet.");
        return BodyFrame{ root->getWorldPosition(), root->getWorldRotation() };
    }

    // The joint a bone points at, for the body joints; other joints use their
    // longest non-collision child.
    std::string boneEndName(const std::string& name)
    {
        static const std::map<std::string, std::string> ends{
            { "mPelvis", "mTorso" },          { "mTorso", "mChest" },           { "mChest", "mNeck" },
            { "mNeck", "mHead" },             { "mHead", "mSkull" }, // up (its longest child is an eye)
            { "mCollarLeft", "mShoulderLeft" }, { "mCollarRight", "mShoulderRight" },
            { "mShoulderLeft", "mElbowLeft" }, { "mShoulderRight", "mElbowRight" }, { "mElbowLeft", "mWristLeft" },
            { "mElbowRight", "mWristRight" }, { "mWristLeft", "mHandMiddle1Left" }, { "mWristRight", "mHandMiddle1Right" },
            { "mHipLeft", "mKneeLeft" },      { "mHipRight", "mKneeRight" },    { "mKneeLeft", "mAnkleLeft" },
            { "mKneeRight", "mAnkleRight" },  { "mAnkleLeft", "mFootLeft" },    { "mAnkleRight", "mFootRight" },
            { "mFootLeft", "mToeLeft" },      { "mFootRight", "mToeRight" },
        };
        auto it = ends.find(name);
        return it != ends.end() ? it->second : std::string();
    }

    LLJoint* boneEnd(LLVOAvatar* avatar, LLJoint* joint)
    {
        const std::string mapped = boneEndName(joint->getName());
        if (!mapped.empty())
        {
            if (LLJoint* end = avatar->getJoint(mapped))
                return end;
        }
        LLJoint* best        = nullptr;
        F32      best_length = 0.001f;
        for (LLJoint* child : joint->mChildren)
        {
            if (!child || child->getName().empty() || child->getName()[0] != 'm')
                continue; // collision volumes are upper case
            const F32 length = child->getPosition().magVec();
            if (length > best_length)
            {
                best        = child;
                best_length = length;
            }
        }
        return best;
    }

    const std::vector<Limb>& limbs()
    {
        static const std::vector<Limb> list{
            { "left_arm", "mShoulderLeft", "mElbowLeft", "mWristLeft", LLVector3::z_axis, -1.f, LLVector3(-0.3f, 0.4f, -1.f) },
            { "right_arm", "mShoulderRight", "mElbowRight", "mWristRight", LLVector3::z_axis, 1.f, LLVector3(-0.3f, -0.4f, -1.f) },
            { "left_leg", "mHipLeft", "mKneeLeft", "mAnkleLeft", LLVector3::y_axis, 1.f, LLVector3(1.f, 0.15f, 0.f) },
            { "right_leg", "mHipRight", "mKneeRight", "mAnkleRight", LLVector3::y_axis, 1.f, LLVector3(1.f, -0.15f, 0.f) },
        };
        return list;
    }
}

// ---------------------------------------------------------------------------
// Pose history: the whole pose of the own avatar, read and written for
// posehistorytools.cpp (undo / redo).

namespace
{
    namespace history = plus::posermcp::history;

    history::Quat historyQuat(const LLQuaternion& q) { return history::Quat{ q.mQ[VX], q.mQ[VY], q.mQ[VZ], q.mQ[VW] }; }

    LLQuaternion viewerQuat(const history::Quat& q)
    {
        LLQuaternion result(q.x, q.y, q.z, q.w);
        result.normalize();
        return result;
    }

    history::Vec3 historyVec(const LLVector3& v) { return history::Vec3{ v.mV[VX], v.mV[VY], v.mV[VZ] }; }
    LLVector3     viewerVec(const history::Vec3& v) { return LLVector3(v.x, v.y, v.z); }

    // Below this a saved value counts as restored (float noise of the poser's
    // own quaternion products).
    constexpr double RESTORE_ANGLE_DEG = 0.005;
    constexpr F32    RESTORE_OFFSET    = 1e-6f;
}

bool PlusPoserTools::captureSelf(history::Pose& out)
{
    out.joints.clear();
    if (!isLoggedIn())
        return false;
    // Never create the poser window here (this runs a few times a second): no
    // window instance means nobody is posing.
    FSFloaterPoser* floater = LLFloaterReg::findTypedInstance<FSFloaterPoser>(POSER_FLOATER);
    if (!floater)
        return false;
    LLVOAvatar*      avatar   = gAgentAvatarp.get();
    FSPoserAnimator& animator = floater->mPoserAnimator;
    if (!animator.isPosingAvatar(avatar))
        return false;
    FSPosingMotion* motion = animator.getPosingMotion(avatar);
    if (!motion)
        return false;

    out.joints.reserve(animator.PoserJoints.size());
    for (const FSPoserAnimator::FSPoserJoint& joint : animator.PoserJoints)
    {
        FSJointPose* pose = motion->getJointPoseByJointName(joint.jointName());
        if (!pose)
            continue; // not in this skeleton
        history::JointState state;
        state.name     = joint.jointName();
        state.target   = historyQuat(pose->getTargetRotation());
        state.own      = historyQuat(pose->getPublicRotation());
        state.position = historyVec(pose->getPublicPosition());
        state.scale    = historyVec(pose->getPublicScale());
        state.enabled  = motion->currentlyPosingJoint(pose);
        state.rotation = joint.boneType() != COL_VOLUMES;
        out.joints.push_back(std::move(state));
    }
    return true;
}

std::size_t PlusPoserTools::applySelf(const history::Pose& pose)
{
    requireLoggedIn();
    FSFloaterPoser* floater = poser();
    LLVOAvatar*     avatar  = gAgentAvatarp.get();
    requirePosing(floater, avatar);
    FSPoserAnimator& animator = floater->mPoserAnimator;

    // Straight into each joint's pose (not through setJointRotation), so the
    // values come back exactly and world-locked children are not counter-rotated.
    std::size_t written = 0;
    for (const history::JointState& saved : pose.joints)
    {
        const FSPoserAnimator::FSPoserJoint* joint = animator.getPoserJointByName(saved.name);
        FSJointPose*                         jp    = joint ? jointPose(floater, avatar, *joint) : nullptr;
        if (!jp)
            continue; // a joint this skeleton does not have

        bool changed = false;
        if (joint->boneType() != COL_VOLUMES)
        {
            const LLQuaternion target  = viewerQuat(saved.target);
            const LLQuaternion current = jp->getTargetRotation();
            if (history::angleDeg(historyQuat(current), saved.target) > RESTORE_ANGLE_DEG)
            {
                // target = own * base. Keep the joint's base (the animation the
                // poser started from) when it is the one the pose was saved
                // with; otherwise zero it like set_joints absolute does.
                const LLQuaternion own_saved  = viewerQuat(saved.own);
                const LLQuaternion base_now   = ~jp->getPublicRotation() * current;
                const LLQuaternion base_saved = ~own_saved * target;
                if (jp->isBaseRotationZero())
                    jp->setPublicRotation(true, jp->userHasSetBaseRotationToZero(), POSER_CHANGE_DEFAULT, target);
                else if (history::angleDeg(historyQuat(base_now), historyQuat(base_saved)) <= RESTORE_ANGLE_DEG)
                    jp->setPublicRotation(false, false, POSER_CHANGE_DEFAULT, own_saved);
                else
                    jp->setPublicRotation(true, true, POSER_CHANGE_DEFAULT, target);
                changed = true;
            }
        }

        const LLVector3 position = viewerVec(saved.position);
        if (dist_vec(jp->getPublicPosition(), position) > RESTORE_OFFSET)
        {
            jp->setPublicPosition(position);
            changed = true;
        }
        const LLVector3 scale = viewerVec(saved.scale);
        if (dist_vec(jp->getPublicScale(), scale) > RESTORE_OFFSET)
        {
            jp->setPublicScale(scale);
            changed = true;
        }
        if (animator.isPosingAvatarJoint(avatar, *joint) != saved.enabled)
        {
            animator.setPosingAvatarJoint(avatar, *joint, saved.enabled);
            changed = true;
        }
        if (changed)
            ++written;
    }
    refreshWindow(floater, false);
    return written;
}

namespace plus::posermcp
{
    bool        captureSelfPose(history::Pose& out) { return PlusPoserTools::captureSelf(out); }
    std::size_t applySelfPose(const history::Pose& pose) { return PlusPoserTools::applySelf(pose); }
}

// ---------------------------------------------------------------------------
// solve_pose and nudge (posesolvetools.cpp): the skeleton as drawn in, the
// solved rotations out.

namespace
{
    namespace solver = plus::posermcp::solver;
}

void PlusPoserTools::captureForSolve(const json::object& args, const std::vector<std::string>& wanted, plus::posermcp::SolveCapture& out)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);
    LLJoint* root = avatar->getRootJoint();
    if (!root)
        throw ToolError("This avatar has no skeleton yet.");
    const BodyFrame frame = bodyFrame(avatar);

    // the wanted joints and their ancestors below the root joint (the body frame)
    std::set<LLJoint*> chosen;
    for (const std::string& name : wanted)
    {
        LLJoint* joint = avatar->getJoint(name);
        for (LLJoint* j = joint; j && j != root; j = j->getParent())
        {
            if (!chosen.insert(j).second)
                break; // its ancestors are in already
        }
    }
    auto depth = [root](LLJoint* joint)
    {
        int d = 0;
        for (LLJoint* j = joint; j && j != root; j = j->getParent())
            ++d;
        return d;
    };
    std::vector<std::pair<int, LLJoint*>> ordered;
    for (LLJoint* j : chosen)
        ordered.push_back({ depth(j), j });
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second->getName() < b.second->getName(); });
    if (ordered.empty() || ordered.front().second->getName() != "mPelvis" || (ordered.size() > 1 && ordered[1].first == ordered.front().first))
        throw ToolError("This avatar's skeleton does not hang from mPelvis as expected.");

    solver::Skeleton& sk = out.skeleton;
    sk                   = solver::Skeleton();
    std::map<LLJoint*, int> index;
    for (const auto& [d, joint] : ordered)
    {
        LLJoint* parent = joint->getParent();
        const bool top  = index.empty();
        index[joint]    = (int)sk.names.size();
        sk.names.push_back(joint->getName());
        sk.parent.push_back(top ? -1 : index.at(parent));
        sk.local.push_back(solver::fromLL(joint->getRotation()));
        // the offset from the parent in the parent's frame, as drawn (shape, mesh joint
        // position overrides and the parent's scale included)
        sk.offset.push_back(top ? solver::Vec3()
                                : solver::fromLL((joint->getWorldPosition() - parent->getWorldPosition()) * ~parent->getWorldRotation()));
        if (top)
            sk.rootPos = solver::fromLL(frame.pointToBody(joint->getWorldPosition()));
    }
    FSJointPose* pelvis = jointPose(floater, avatar, requireJoint(floater, "mPelvis"));
    sk.pelvisOffset     = pelvis ? solver::fromLL(pelvis->getPublicPosition()) : solver::Vec3();
    out.fingerprint     = fingerprintOf(args);
    out.settled         = posesSettled(floater);
}

std::uint64_t PlusPoserTools::fingerprintOf(const json::object& args)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    FSPosingMotion* motion  = floater->mPoserAnimator.getPosingMotion(avatar);
    if (!motion)
        return 0;
    std::uint64_t hash = LLRHI::FNV1A_BASIS_SHORT; // FNV-1a over the targets
    auto          mix  = [&hash](const void* data, size_t size) { hash = LLRHI::fnv1a(hash, data, size); };
    for (const FSPoserAnimator::FSPoserJoint& joint : floater->mPoserAnimator.PoserJoints)
    {
        FSJointPose* pose = motion->getJointPoseByJointName(joint.jointName());
        if (!pose)
            continue;
        const LLQuaternion q = pose->getTargetRotation();
        const LLVector3    p = pose->getPublicPosition();
        const LLVector3    s = pose->getPublicScale();
        const bool         e = motion->currentlyPosingJoint(pose);
        mix(q.mQ, sizeof(q.mQ));
        mix(p.mV, sizeof(p.mV));
        mix(s.mV, sizeof(s.mV));
        mix(&e, sizeof(e));
    }
    return hash;
}

bool PlusPoserTools::settledNow() { return posesSettled(poser()); }

void PlusPoserTools::applySolved(const json::object& args, const std::vector<std::pair<std::string, LLQuaternion>>& rotations,
                                 const std::optional<LLVector3>& pelvisPosition)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);
    FSPoserAnimator& animator = floater->mPoserAnimator;
    for (const auto& [name, rotation] : rotations)
    {
        const FSPoserAnimator::FSPoserJoint* joint = animator.getPoserJointByName(name);
        if (!joint || joint->boneType() == COL_VOLUMES || !jointPose(floater, avatar, *joint))
            continue; // not a joint the poser turns on this skeleton
        applyRotation(floater, avatar, *joint, rotation);
    }
    if (pelvisPosition)
    {
        const FSPoserAnimator::FSPoserJoint& joint = requireJoint(floater, "mPelvis");
        LLVector3                            p     = *pelvisPosition;
        p.clamp(-MAX_POSITION_OFFSET, MAX_POSITION_OFFSET);
        animator.setPosingAvatarJoint(avatar, joint, true);
        animator.setJointPosition(avatar, &joint, p, POSER_FRAME_BONE, NONE);
    }
    refreshWindow(floater, false);
}

std::vector<std::pair<std::string, LLVector3>> PlusPoserTools::drawnAt(const json::object& args, const std::vector<std::string>& names)
{
    LLVOAvatar*                                    avatar = resolveAvatar(args);
    const BodyFrame                                frame  = bodyFrame(avatar);
    std::vector<std::pair<std::string, LLVector3>> out;
    for (const std::string& name : names)
    {
        if (LLJoint* joint = avatar->getJoint(name))
            out.push_back({ name, frame.pointToBody(joint->getWorldPosition()) });
    }
    return out;
}

namespace plus::posermcp
{
    void captureSolveSkeleton(const json::object& args, const std::vector<std::string>& wanted, SolveCapture& out)
    {
        PlusPoserTools::captureForSolve(args, wanted, out);
    }
    std::uint64_t poseFingerprint(const json::object& args) { return PlusPoserTools::fingerprintOf(args); }
    bool          posesSettledNow() { return PlusPoserTools::settledNow(); }
    void          applySolvedPose(const json::object& args, const std::vector<std::pair<std::string, LLQuaternion>>& rotations,
                                  const std::optional<LLVector3>& pelvisPosition)
    {
        PlusPoserTools::applySolved(args, rotations, pelvisPosition);
    }
    std::vector<std::pair<std::string, LLVector3>> drawnPositions(const json::object& args, const std::vector<std::string>& names)
    {
        return PlusPoserTools::drawnAt(args, names);
    }
}

namespace plus::posermcp
{
    std::function<ToolResult(const json::object&)> simpleTool(const MainThreadRunner& run, ToolResult (*impl)(const json::object&),
                                                              double timeout)
    {
        return [run, impl, timeout](const json::object& args) -> ToolResult
        {
            return run([impl, args](const JobProgress&) -> std::optional<ToolResult> { return impl(args); }, timeout);
        };
    }
}

// ---------------------------------------------------------------------------
// Tool table

namespace plus::posermcp
{
    std::vector<Tool> makePoserTools(const MainThreadRunner& run)
    {
        // simpleTool() for a tool that changes the pose: the pose before it goes into the history.
        auto changing = [run](const char* name, ToolResult (*impl)(const json::object&), double timeout)
        {
            return [run, name, impl, timeout](const json::object& args) -> ToolResult
            {
                return run(withHistory(name, args, [impl, args](const JobProgress&) -> std::optional<ToolResult> { return impl(args); }),
                           timeout);
            };
        };

        const json::object avatar_prop = prop("string", AVATAR_ARG_DOC);

        std::vector<Tool> tools;

        {
            Tool t;
            t.name        = "status";
            t.title       = "Viewer status";
            t.description = "Viewer version, whether you are logged in, region, your avatar id, which avatars are being "
                            "posed, whether the poser window is open, and the world view size in pixels. Call this first.";
            t.readOnly    = true;
            t.inputSchema = objectSchema({});
            t.handler     = simpleTool(run, &PlusPoserTools::status, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "list_avatars";
            t.title       = "List poseable avatars";
            t.description = "Lists yourself and nearby animesh (within 50 m) with id, name, distance, can_pose and posing. "
                            "Only yourself and animesh you own can be posed.";
            t.readOnly    = true;
            t.inputSchema = objectSchema({});
            t.handler     = simpleTool(run, &PlusPoserTools::listAvatars, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "start_posing";
            t.title       = "Start posing";
            t.description = "Starts the poser on an avatar. The current animation pose is captured as the starting pose "
                            "(reset_all to=\"start\" returns to it). from_tpose=true starts from the T-pose instead (all "
                            "rotations zero), which is the clean base for building a pose from scratch.";
            json::object p;
            p["avatar"]     = avatar_prop;
            p["from_tpose"] = prop("boolean", "Start from the T-pose (all joint rotations zero). Default false.");
            t.inputSchema   = objectSchema(std::move(p));
            t.handler       = changing("start_posing", &PlusPoserTools::startPosing, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "stop_posing";
            t.title       = "Stop posing";
            t.description = "Stops the poser on an avatar; it returns to its normal animations. Unsaved changes are lost "
                            "(save_pose first if needed).";
            json::object p;
            p["avatar"]   = avatar_prop;
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = simpleTool(run, &PlusPoserTools::stopPosing, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "list_joints";
            t.title       = "List joints";
            t.description = std::string("Lists the poser's joints of a group: name, title, group, mirror (the opposite "
                                        "side's joint), parent (from the avatar's skeleton), whether rotation/position/"
                                        "scale apply, and enabled (posing that joint).\n") + JOINT_NAMES_DOC;
            t.readOnly    = true;
            json::object p;
            p["avatar"]   = avatar_prop;
            p["group"]    = enumProp({ "body", "face", "hands", "misc", "collision", "all" }, "Joint group. Default body.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = simpleTool(run, &PlusPoserTools::listJoints, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "get_pose";
            t.title       = "Read the pose";
            t.description = std::string("Reads the current pose: for each joint its rotation [x,y,z] in degrees (local, "
                                        "see below), position offset (m) and scale offset, enabled, and changed (edited "
                                        "since posing started). Pick joints by name or by group (default body).\n") +
                            ROTATION_FRAME_DOC;
            t.readOnly    = true;
            json::object p;
            p["avatar"]       = avatar_prop;
            p["joints"]       = json::object{ { "type", "array" }, { "items", json::object{ { "type", "string" } } },
                                              { "description", "Joint names; overrides group." } };
            p["group"]        = enumProp({ "body", "face", "hands", "misc", "collision", "all" }, "Default body.");
            p["changed_only"] = prop("boolean", "Only joints edited since posing started. Default false.");
            t.inputSchema     = objectSchema(std::move(p));
            t.handler         = simpleTool(run, &PlusPoserTools::getPose, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "set_joints";
            t.title       = "Set joint rotations/positions/scales";
            t.description = std::string(
                                "Sets several joints at once. Each entry: {\"joint\": name, \"rotation\": [x,y,z] degrees, "
                                "\"position\": [x,y,z] meters, \"scale\": [x,y,z]} (each part optional). mode \"absolute\" "
                                "(default): rotation is the joint's full local rotation, position/scale are offsets from "
                                "the starting pose (position within +-1.5 m, scale offset within -0.9..1.5). mode "
                                "\"delta\": rotation is applied on top of the current one about the parent's axes, "
                                "position/scale are added. mirror=true also gives the opposite-side joint (Left<->Right) "
                                "the mirror-image rotation (positions and scales are not mirrored), unless that joint is "
                                "in the same call. Joints switched off in the poser "
                                "window are switched back on. All entries are validated before anything changes. Then "
                                "take a snapshot to check.\n") +
                            ROTATION_FRAME_DOC;
            json::object entry_props;
            entry_props["joint"]    = prop("string", "Joint name, e.g. mShoulderLeft.");
            entry_props["rotation"] = vec3Prop("[x, y, z] degrees (roll about X, pitch about Y, yaw about Z).");
            entry_props["position"] = vec3Prop("[x, y, z] meters, offset in the parent's frame.");
            entry_props["scale"]    = vec3Prop("[x, y, z] offset added to the starting scale (0 = unchanged).");
            json::object entry = objectSchema(std::move(entry_props), { "joint" });

            json::object p;
            p["avatar"] = avatar_prop;
            p["joints"] = json::object{ { "type", "array" }, { "items", std::move(entry) }, { "minItems", 1 },
                                        { "maxItems", (int)MAX_JOINTS_PER_CALL } };
            p["mode"]   = enumProp({ "absolute", "delta" }, "Default absolute.");
            p["mirror"] = prop("boolean", "Also give the opposite-side joint the mirror-image rotation. Default false.");
            t.inputSchema = objectSchema(std::move(p), { "joints" });
            t.handler     = changing("set_joints", &PlusPoserTools::setJoints, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "reset_joint";
            t.title       = "Reset joints";
            t.description = "Returns the listed joints to how they were when posing started (rotation, position, scale). "
                            "mirror=true also resets the opposite-side joints.";
            json::object p;
            p["avatar"] = avatar_prop;
            p["joints"] = json::object{ { "type", "array" }, { "items", json::object{ { "type", "string" } } }, { "minItems", 1 } };
            p["mirror"] = prop("boolean", "Default false.");
            t.inputSchema = objectSchema(std::move(p), { "joints" });
            t.handler     = changing("reset_joint", &PlusPoserTools::resetJoint, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "reset_all";
            t.title       = "Reset the whole pose";
            t.description = "to=\"start\" (default): every joint back to the pose captured when posing started. "
                            "to=\"tpose\": all rotations to zero (T-pose); positions and scales are kept.";
            json::object p;
            p["avatar"]   = avatar_prop;
            p["to"]       = enumProp({ "start", "tpose" }, "Default start.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = changing("reset_all", &PlusPoserTools::resetAll, 15.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "save_pose";
            t.title       = "Save the pose";
            t.description = "Saves the pose as <name>.xml in the poser's pose folder (user_settings/poses, the same list "
                            "the poser window loads from). bvh=true also writes <name>.bvh, which can be uploaded as an "
                            "animation. Refuses to replace an existing pose unless overwrite=true.";
            json::object p;
            p["avatar"]    = avatar_prop;
            p["name"]      = prop("string", "A file name of 1-200 bytes (UTF-8): no folders (\\ / :), none of * ? \" < > |, no '..', no dot or space at either end.");
            p["bvh"]       = prop("boolean", "Also export BVH. Default false.");
            p["overwrite"] = prop("boolean", "Replace an existing pose of the same name. Default false.");
            t.inputSchema  = objectSchema(std::move(p), { "name" });
            t.handler      = simpleTool(run, &PlusPoserTools::savePose, 30.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "load_pose";
            t.title       = "Load a pose";
            t.description = "Loads a saved pose (see list_poses) onto an avatar that is being posed. what selects the "
                            "parts: all (default), rotation, position, scale, rot_pos, rot_scale, pos_scale.";
            json::object p;
            p["avatar"]   = avatar_prop;
            p["name"]     = prop("string", "Pose name without .xml.");
            p["what"]     = enumProp({ "all", "rotation", "position", "scale", "rot_pos", "rot_scale", "pos_scale" }, "Default all.");
            t.inputSchema = objectSchema(std::move(p), { "name" });
            t.handler     = changing("load_pose", &PlusPoserTools::loadPose, 30.0);
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "list_poses";
            t.title       = "List saved poses";
            t.description = "Lists the poses saved in the poser's pose folder.";
            t.readOnly    = true;
            t.inputSchema = objectSchema({});
            t.handler     = simpleTool(run, &PlusPoserTools::listPoses, 15.0);
            tools.push_back(std::move(t));
        }

        appendSnapshotTools(tools, run); // snapshot, camera
        appendAimTools(tools, run);      // joint_positions, reach, aim
        appendContactTools(tools, run);  // contact
        appendSolveTools(tools, run);    // solve_pose, nudge
        appendHistoryTools(tools, run);  // undo, redo, history, mark, compare
        return tools;
    }
}
