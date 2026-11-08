/**
 * @file posertools_aim.cpp
 * @brief The joint_positions, reach and aim tools of the Poser MCP (see
 *        posertools.cpp): the skeleton read in the body frame, IK, bone directions.
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

#include "llviewerprecompiledheaders.h"

#include "posertools_impl.h"

// ---------------------------------------------------------------------------
// World-space readout and IK. Both read the skeleton as it is drawn, so they
// first wait for the joints to ease into their targets. (The body frame and
// the limbs are in posertools.cpp.)

std::optional<ToolResult> PlusPoserTools::jointPositions(const json::object& args, const JobProgress& progress)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();

    std::vector<std::string> names = argStringList(args, "joints");
    const std::string        group = argString(args, "group", "body");
    checkGroup(group);
    if (names.size() > MAX_JOINTS_PER_CALL)
        throw ToolError("Too many joints in one call");
    std::vector<std::string> selected;
    for (const std::string& name : names)
        selected.push_back(requireJoint(floater, name).jointName());
    if (names.empty())
    {
        for (const FSPoserAnimator::FSPoserJoint& joint : floater->mPoserAnimator.PoserJoints)
        {
            if (groupMatches(group, joint.boneType()))
                selected.push_back(joint.jointName());
        }
    }

    bool settled = true;
    if (argBool(args, "settle", true) && !waitForSettle(progress, settled))
        return std::nullopt;

    const BodyFrame frame = bodyFrame(avatar);
    json::array     joints;
    for (const std::string& name : selected)
    {
        LLJoint* joint = avatar->getJoint(name);
        if (!joint)
            continue;
        const LLVector3 position = joint->getWorldPosition();

        json::object entry;
        entry["joint"]    = name;
        entry["position"] = vecJson(frame.pointToBody(position), 3);
        if (LLJoint* end = boneEnd(avatar, joint))
        {
            LLVector3 bone = end->getWorldPosition() - position;
            const F32 length = bone.normalize();
            if (length > 0.001f)
            {
                const LLVector3 direction = frame.dirToBody(bone);
                entry["bone_to"]   = end->getName();
                entry["direction"] = vecJson(direction, 3);
                entry["length"]    = rounded(length, 3);
                // Angle below the horizontal plane of the body: 90 = straight down.
                entry["down_deg"] = rounded(asinf(llclamp(-direction.mV[VZ], -1.f, 1.f)) * RAD_TO_DEG, 1);
            }
        }
        joints.push_back(std::move(entry));
    }

    json::object result;
    result["avatar"] = avatar->getID().asString();
    result["joints"] = std::move(joints);
    result["frame"]  = "Body frame of the avatar as drawn: origin at the root joint (between the hips at rest), +X forward, "
                       "+Y the avatar's left, +Z up, meters. direction is the unit vector from the joint to bone_to; "
                       "down_deg is how far the bone points below horizontal (90 straight down, -90 straight up).";
    if (!settled)
        result["note"] = "Some joints had not reached their targets yet (another animation may hold them).";
    return ToolResult::json(result);
}

std::optional<ToolResult> PlusPoserTools::reach(const json::object& args, const JobProgress& progress)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);

    const json::value* raw = args.if_contains("limbs");
    if (!raw || !raw->is_array() || raw->get_array().empty() || raw->get_array().size() > 4)
        throw ToolError("'limbs' must be an array of 1-4 {limb, target, relative_to?, bend_toward?}");

    struct Request
    {
        const Limb*              limb = nullptr;
        LLVector3                target;
        std::string              relative_to;
        std::optional<LLVector3> bend_toward;
    };
    std::vector<Request> requests;
    for (const json::value& item : raw->get_array())
    {
        if (!item.is_object())
            throw ToolError("each entry of 'limbs' must be an object");
        const json::object& entry = item.get_object();
        for (const auto& kv : entry)
        {
            if (kv.key() != "limb" && kv.key() != "target" && kv.key() != "relative_to" && kv.key() != "bend_toward")
                throw ToolError("unknown field '" + std::string(kv.key()) + "' in limbs");
        }

        Request request;
        const std::string name = argString(entry, "limb", "");
        for (const Limb& limb : limbs())
        {
            if (name == limb.name)
                request.limb = &limb;
        }
        if (!request.limb)
            throw ToolError("limb must be left_arm, right_arm, left_leg or right_leg");
        for (const Request& other : requests)
        {
            if (other.limb == request.limb)
                throw ToolError(name + " is listed twice");
        }

        const json::value* target = entry.if_contains("target");
        if (!target)
            throw ToolError(name + ": target is required");
        request.target      = parseVec3(*target, name + " target");
        request.relative_to = argString(entry, "relative_to", "");
        if (!request.relative_to.empty())
            request.relative_to = requireJoint(floater, request.relative_to).jointName();
        if (const json::value* bend = entry.if_contains("bend_toward"); bend && !bend->is_null())
        {
            LLVector3 direction = parseVec3(*bend, name + " bend_toward");
            if (direction.normalize() < 0.001f)
                throw ToolError(name + ": bend_toward must not be zero");
            request.bend_toward = direction;
        }
        requests.push_back(request);
    }

    // Read the skeleton only once it shows the current targets.
    bool settled = true;
    if (!waitForSettle(progress, settled))
        return std::nullopt;

    const BodyFrame frame = bodyFrame(avatar);
    json::array     report;
    for (const Request& request : requests)
    {
        const Limb& limb   = *request.limb;
        LLJoint*    root   = avatar->getJoint(limb.root);
        LLJoint*    middle = avatar->getJoint(limb.middle);
        LLJoint*    end    = avatar->getJoint(limb.end);
        if (!root || !middle || !end || !root->getParent())
            throw ToolError(std::string(limb.name) + " is not part of this avatar's skeleton");

        LLVector3 target = request.target;
        if (!request.relative_to.empty())
        {
            LLJoint* anchor = avatar->getJoint(request.relative_to);
            if (!anchor)
                throw ToolError(request.relative_to + " is not part of this avatar's skeleton");
            target = frame.pointToBody(anchor->getWorldPosition()) + request.target;
        }
        const LLVector3 target_world = frame.pointToWorld(target);

        LLVector3 pole = frame.dirToWorld(request.bend_toward.value_or(limb.pole));
        pole.normVec();

        TwoBoneInput input;
        input.root        = root->getWorldPosition();
        input.middle      = middle->getWorldPosition();
        input.end         = end->getWorldPosition();
        input.rootWorld   = root->getWorldRotation();
        input.middleWorld = middle->getWorldRotation();
        input.parentWorld = root->getParent()->getWorldRotation();
        input.target      = target_world;
        input.pole        = pole;
        input.hinge       = limb.hinge;
        input.natural     = limb.natural;
        TwoBoneResult solved;
        const char*   why = "";
        if (!solveTwoBone(input, solved, why))
            throw ToolError(std::string(limb.name) + ": " + why);

        applyRotation(floater, avatar, requireJoint(floater, limb.root), solved.rootLocal);
        applyRotation(floater, avatar, requireJoint(floater, limb.middle), solved.middleLocal);

        json::object entry;
        entry["limb"]        = limb.name;
        entry["target"]      = vecJson(target, 3);
        entry["reached"]     = solved.reachable;
        if (solved.distance > solved.maxReach)
            entry["short_by_m"] = rounded(solved.distance - solved.maxReach, 3);
        entry["end_position"] = vecJson(frame.pointToBody(solved.reachedPoint), 3);
        entry["bend_point"]   = vecJson(frame.pointToBody(solved.bendPoint), 3);
        entry["bend_deg"]     = rounded(fabsf(solved.bendAngle) * RAD_TO_DEG, 1);
        if (solved.error > 0.05f)
            entry["direction_error"] = rounded(solved.error, 3);
        entry[limb.root]   = vecJson(quatToEulerDegrees(solved.rootLocal), 2);
        entry[limb.middle] = vecJson(quatToEulerDegrees(solved.middleLocal), 2);
        report.push_back(std::move(entry));
    }

    refreshWindow(floater, false);

    json::object result;
    result["limbs"] = std::move(report);
    if (!settled)
        result["note"] = "Some joints had not reached their targets when the skeleton was read; check with joint_positions.";
    result["next"] = "Check with joint_positions or a snapshot. The wrist/ankle angle and the fingers were not changed: shape the hands now.";
    return ToolResult::json(result);
}

// ---------------------------------------------------------------------------
// aim: point bones along body-frame directions (the maths is in aimsolver.cpp).

namespace
{
    // The reference side axis of a joint for roll_toward, in its own frame (= the
    // body frame in the rest pose). Same table as side_axis() in ik_check.py.
    // Upper arms and thighs do not use it: their roll_toward is the forearm / shin.
    LLVector3 aimSideAxis(const std::string& name)
    {
        auto starts = [&](const char* prefix) { return boost::starts_with(name, prefix); };
        if (name == "mPelvis" || name == "mTorso" || name == "mChest" || name == "mNeck" || name == "mHead" || starts("mSpine"))
            return LLVector3::x_axis; // front
        if (starts("mCollar"))
            return LLVector3::z_axis; // top
        if (starts("mShoulder"))
            return LLVector3::x_axis; // the side the elbow bends toward (used only with toward)
        if (starts("mElbow") || starts("mWrist") || starts("mHand"))
            return -LLVector3::z_axis; // palm side
        if (starts("mHip"))
            return -LLVector3::x_axis; // the side the knee bends toward (used only with toward)
        if (starts("mKnee"))
            return LLVector3::x_axis; // where the toes point
        return LLVector3::z_axis;     // ankles, feet, toes: top of the foot; others: up
    }

    AimTwist parseTwist(const std::string& value)
    {
        if (value == "auto")
            return AimTwist::Auto;
        if (value == "keep")
            return AimTwist::Keep;
        if (value == "rest")
            return AimTwist::Rest;
        throw ToolError("twist must be auto, keep or rest");
    }
}

std::optional<ToolResult> PlusPoserTools::aim(const json::object& args, const JobProgress& progress)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);
    FSPoserAnimator& animator = floater->mPoserAnimator;

    const AimTwist     default_twist = parseTwist(argString(args, "twist", "auto"));
    const json::value* raw           = args.if_contains("bones");
    if (!raw || !raw->is_array() || raw->get_array().empty())
        throw ToolError("'bones' must be a non-empty array of {joint, direction, toward?, roll_toward?, twist?}");
    if (raw->get_array().size() > MAX_JOINTS_PER_CALL)
        throw ToolError("Too many bones in one call");

    struct Request
    {
        const FSPoserAnimator::FSPoserJoint* joint = nullptr;
        LLVector3                            direction; // body frame, unit
        std::string                          toward;
        std::optional<LLVector3>             roll;      // body frame
        AimTwist                             twist = AimTwist::Auto;
    };
    std::vector<Request> requests;
    for (const json::value& item : raw->get_array())
    {
        if (!item.is_object())
            throw ToolError("each entry of 'bones' must be an object");
        const json::object& entry = item.get_object();
        for (const auto& kv : entry)
        {
            if (kv.key() != "joint" && kv.key() != "direction" && kv.key() != "toward" && kv.key() != "roll_toward" &&
                kv.key() != "twist")
                throw ToolError("unknown field '" + std::string(kv.key()) + "' in bones");
        }

        Request request;
        request.joint           = &requireJoint(floater, argString(entry, "joint", ""));
        const std::string& name = request.joint->jointName();
        if (request.joint->boneType() == COL_VOLUMES)
            throw ToolError(name + " is a collision volume: it has no bone to aim");
        if (request.joint->boneType() == FACE || boost::starts_with(name, "mFace") || boost::starts_with(name, "mEye"))
            throw ToolError(name + ": face joints can not be aimed; use set_joints");
        if (!jointPose(floater, avatar, *request.joint))
            throw ToolError(name + " is not part of this avatar's skeleton");
        for (const Request& other : requests)
        {
            if (other.joint == request.joint)
                throw ToolError(name + " is listed twice");
        }

        const json::value* direction = entry.if_contains("direction");
        if (!direction || direction->is_null())
            throw ToolError(name + ": direction is required");
        request.direction = parseVec3(*direction, name + " direction");
        if (request.direction.normalize() < 1e-6f)
            throw ToolError(name + ": direction must not be zero");

        request.toward = argString(entry, "toward", "");
        if (!request.toward.empty())
        {
            if (const FSPoserAnimator::FSPoserJoint* known = animator.getPoserJointByName(request.toward))
                request.toward = known->jointName(); // its exact spelling
        }
        if (const json::value* roll = entry.if_contains("roll_toward"); roll && !roll->is_null())
        {
            LLVector3 toward = parseVec3(*roll, name + " roll_toward");
            if (toward.normalize() < 1e-6f)
                throw ToolError(name + ": roll_toward must not be zero");
            request.roll = toward;
        }
        const json::value* twist = entry.if_contains("twist");
        request.twist = twist && !twist->is_null() ? parseTwist(argString(entry, "twist", "auto")) : default_twist;
        requests.push_back(request);
    }

    // Read the skeleton only once it shows the current targets.
    bool settled = true;
    if (argBool(args, "settle", true) && !waitForSettle(progress, settled))
        return std::nullopt;

    // The part of the skeleton the solver needs: every aimed joint and its ancestors,
    // with the local rotations as drawn.
    const BodyFrame          frame = bodyFrame(avatar);
    std::vector<AimJoint>    joints;
    std::map<LLJoint*, int>  index;
    auto add_chain = [&](LLJoint* joint) -> int
    {
        std::vector<LLJoint*> chain;
        for (LLJoint* j = joint; j && index.find(j) == index.end(); j = j->getParent())
            chain.push_back(j);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            LLJoint* j = *it;
            LLJoint* p = j->getParent();
            AimJoint entry;
            entry.parent      = p ? index.at(p) : -1;
            entry.local       = j->getRotation();
            entry.parentWorld = p ? p->getWorldRotation() : ~j->getRotation() * j->getWorldRotation();
            index[j]          = (int)joints.size();
            joints.push_back(entry);
        }
        return index.at(joint);
    };

    std::vector<AimBone>     bones;
    std::vector<LLJoint*>    ends;
    std::vector<std::string> roll_from(requests.size());
    for (const Request& request : requests)
    {
        const std::string& name  = request.joint->jointName();
        LLJoint*           joint = avatar->getJoint(name);
        if (!joint || !joint->getParent())
            throw ToolError(name + " is not part of this avatar's skeleton");

        LLJoint* end = nullptr;
        if (!request.toward.empty())
        {
            end = avatar->getJoint(request.toward);
            if (!end || end == joint)
                throw ToolError(name + ": toward must name a joint below it ('" + request.toward + "' is not)");
        }
        else
        {
            end = boneEnd(avatar, joint);
        }
        if (end)
        {
            // The end must be below the joint, with no other aimed joint in between
            // (that one's new rotation would move the end).
            LLJoint* between = end->getParent();
            for (; between && between != joint; between = between->getParent())
            {
                for (const Request& other : requests)
                {
                    if (other.joint->jointName() == between->getName())
                        throw ToolError(name + ": " + between->getName() + " lies between it and " + end->getName() +
                                        " and is aimed in the same call; aim " + name + " toward " + between->getName());
                }
            }
            if (!between)
                throw ToolError(name + ": " + end->getName() + " is not below it");
        }

        AimBone bone;
        bone.joint = add_chain(joint);
        if (end)
        {
            bone.bone = (end->getWorldPosition() - joint->getWorldPosition()) * ~joint->getWorldRotation();
        }
        else
        {
            // A fingertip: it continues the bone that leads to it.
            LLJoint* parent = joint->getParent();
            bone.bone       = (joint->getWorldPosition() - parent->getWorldPosition()) * ~parent->getWorldRotation();
        }
        if (bone.bone.magVec() < 1e-4f)
            throw ToolError(name + " has no bone to aim (its end is on the joint)");
        bone.direction = frame.dirToWorld(request.direction);
        bone.twist     = request.twist;
        bone.side      = aimSideAxis(name);
        if (request.roll)
        {
            bone.roll       = true;
            bone.rollToward = frame.dirToWorld(*request.roll);
        }
        for (const Limb& limb : limbs())
        {
            if (name == limb.root && end && end->getName() == limb.middle)
            {
                if (LLJoint* tip = avatar->getJoint(limb.end))
                {
                    bone.limb         = true;
                    bone.childBone    = (tip->getWorldPosition() - end->getWorldPosition()) * ~end->getWorldRotation();
                    bone.childHinge   = limb.hinge;
                    bone.childNatural = limb.natural;
                }
            }
            if (name == limb.middle && end && end->getName() == limb.end)
            {
                bone.hinge     = true;
                bone.hingeAxis = limb.hinge;
                bone.natural   = limb.natural;
            }
        }
        bones.push_back(bone);
        ends.push_back(end);
    }

    // An upper arm / thigh without roll_toward takes the forearm / shin direction of
    // the same call, so that the elbow / knee bends about its hinge only.
    for (size_t i = 0; i < bones.size(); ++i)
    {
        if (!bones[i].limb || bones[i].roll || bones[i].twist != AimTwist::Auto)
            continue;
        for (const Limb& limb : limbs())
        {
            if (requests[i].joint->jointName() != limb.root)
                continue;
            for (size_t j = 0; j < bones.size(); ++j)
            {
                if (requests[j].joint->jointName() == limb.middle && ends[j] && ends[j]->getName() == limb.end)
                {
                    bones[i].roll       = true;
                    bones[i].rollToward = bones[j].direction;
                    roll_from[i]        = limb.middle;
                }
            }
        }
    }

    std::vector<AimResult> results;
    std::string            why;
    int                    bad = -1;
    if (!solveAim(joints, bones, results, why, bad))
        throw ToolError((bad >= 0 && bad < (int)requests.size() ? requests[bad].joint->jointName() + ": " : std::string()) + why);

    for (size_t i = 0; i < requests.size(); ++i)
        applyRotation(floater, avatar, *requests[i].joint, results[i].local);
    refreshWindow(floater, false);

    json::array report;
    json::array warnings;
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const AimResult&   solved = results[i];
        const std::string& name   = requests[i].joint->jointName();
        json::object       entry;
        entry["joint"]     = name;
        entry["bone_to"]   = ends[i] ? ends[i]->getName() : std::string("(fingertip: continues the bone leading to it)");
        entry["rotation"]  = vecJson(quatToEulerDegrees(solved.local), 2);
        entry["direction"] = vecJson(frame.dirToBody(solved.achieved), 3);
        entry["error_deg"] = rounded(solved.error * RAD_TO_DEG, 2);
        switch (solved.method)
        {
            case AimMethod::Swing:
                entry["method"] = "swing";
                break;
            case AimMethod::Side:
                entry["method"]         = "roll";
                entry["roll_error_deg"] = rounded(solved.rollError * RAD_TO_DEG, 2);
                break;
            case AimMethod::Limb:
                entry["method"]         = "limb_roll";
                entry["bend_deg"]       = rounded(fabsf(solved.bend) * RAD_TO_DEG, 1);
                entry["child_miss_deg"] = rounded(solved.rollError * RAD_TO_DEG, 2);
                break;
            case AimMethod::Hinge:
                entry["method"]   = "hinge";
                entry["bend_deg"] = rounded(fabsf(solved.bend) * RAD_TO_DEG, 1);
                if (solved.hingeMiss > 0.f)
                    entry["hinge_miss_deg"] = rounded(solved.hingeMiss * RAD_TO_DEG, 1);
                break;
        }
        entry["angle_deg"] = rounded(solved.angle * RAD_TO_DEG, 1);
        entry["twist_deg"] = rounded(solved.twist * RAD_TO_DEG, 1);
        if (!roll_from[i].empty())
            entry["roll_from"] = roll_from[i];
        std::string note;
        if (solved.rollIgnored)
            note = "hinge joint: roll_toward ignored (an elbow / knee only bends; the upper arm's / thigh's limb_roll "
                   "sets the twist, the hand / foot is set with mWrist / mAnkle)";
        if (solved.note)
            note += (note.empty() ? "" : "; ") + std::string(solved.note);
        if (!note.empty())
            entry["note"] = note;
        report.push_back(std::move(entry));

        // Warnings that the mesh may fold or the bone is not where asked.
        if (solved.hingeClamped)
            warnings.push_back(json::string(name + ": the direction would bend it backwards; the bend was limited to " +
                                            std::to_string((int)AIM_HINGE_BACK_DEG) + " degrees the wrong way"));
        if (solved.hingeMiss > 5.f * DEG_TO_RAD)
            warnings.push_back(json::string(name + ": an elbow / knee only bends about its hinge, so the bone points " +
                                            std::to_string(ll_round(solved.hingeMiss * RAD_TO_DEG)) +
                                            " degrees off the asked direction (the closest the hinge reaches). Aim the "
                                            "upper arm / thigh in the same call, or check the direction"));
        if (solved.warnAngle)
            warnings.push_back(json::string(name + ": rotated " + std::to_string(ll_round(solved.angle * RAD_TO_DEG)) +
                                            " degrees from its rest rotation (more than " + std::to_string((int)AIM_WARN_ANGLE_DEG) +
                                            "): the mesh may fold. Check it in a snapshot (for mPelvis a lying or "
                                            "upside-down body is fine); after a mistake, reset_all first"));
        if (solved.warnTwist)
            warnings.push_back(json::string(name + ": twisted " + std::to_string(ll_round(solved.twist * RAD_TO_DEG)) +
                                            " degrees about its own bone (more than " + std::to_string((int)AIM_WARN_TWIST_DEG) +
                                            "): the mesh may twist off. Drop roll_toward or reset_all and aim again"));
    }

    json::object result;
    result["bones"] = std::move(report);
    if (!warnings.empty())
        result["warnings"] = std::move(warnings);
    if (!settled)
        result["note"] = "Some joints had not reached their targets when the skeleton was read; check with joint_positions.";
    result["next"] = "Check with snapshot (views for several sides at once) or joint_positions. Bones were solved parents "
                     "first; a later change to a parent (set_joints, aim) moves its children's bones too.";
    return ToolResult::json(result);
}

// ---------------------------------------------------------------------------
// Tool table: joint_positions, reach, aim

namespace plus::posermcp
{
    void appendAimTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        const json::object avatar_prop = prop("string", AVATAR_ARG_DOC);

        {
            Tool t;
            t.name        = "joint_positions";
            t.title       = "Where the joints are";
            t.description = std::string("Reads where joints are as drawn, in the avatar's BODY FRAME (origin at the root "
                                        "joint between the hips, +X forward, +Y the avatar's left, +Z up, meters), and "
                                        "where each bone points: direction (unit vector to bone_to, e.g. mShoulderLeft -> "
                                        "mElbowLeft), length, down_deg (90 = straight down). Use it to check a pose with "
                                        "numbers instead of guessing from images: a shin is vertical when its direction is "
                                        "about [0,0,-1], a hand touches the chin when mWristRight is a few cm from mFaceChin. "
                                        "Waits for the joints to ease into the latest pose first (up to ~4 s). The avatar "
                                        "does not need to be posed.\n") +
                            JOINT_NAMES_DOC;
            t.readOnly    = true;
            json::object p;
            p["avatar"]   = avatar_prop;
            p["joints"]   = json::object{ { "type", "array" }, { "items", json::object{ { "type", "string" } } },
                                          { "description", "Joint names; overrides group." } };
            p["group"]    = enumProp({ "body", "face", "hands", "misc", "collision", "all" }, "Default body.");
            p["settle"]   = prop("boolean", "Wait for the pose to settle first. Default true.");
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = [run](const json::object& args) -> ToolResult
            {
                return run([args](const JobProgress& progress) { return PlusPoserTools::jointPositions(args, progress); }, 30.0);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "reach";
            t.title       = "Move a hand or foot to a point (IK)";
            t.description =
                "Inverse kinematics for arms and legs: puts the wrist (arm) or ankle (leg) at a target point and solves the "
                "shoulder+elbow or hip+knee rotations, so you do not have to work out Euler angles. Each entry: {\"limb\": "
                "left_arm|right_arm|left_leg|right_leg, \"target\": [x,y,z] meters in the BODY FRAME (see joint_positions: "
                "origin at the root between the hips, +X forward, +Y the avatar's left, +Z up), \"relative_to\": optional "
                "joint name - then target is an offset (body-frame axes) from that joint's current position, e.g. "
                "{\"limb\":\"right_arm\",\"target\":[0.05,0,-0.08],\"relative_to\":\"mFaceChin\"}, \"bend_toward\": optional "
                "[x,y,z] body-frame direction the elbow/knee should point (default: elbows down and slightly back/out, "
                "knees forward). e.g. elbows sticking out sideways: left_arm bend_toward [0,1,0], right_arm [0,-1,0]; "
                "knees spread outward in a squat: left_leg [0.5,1,0], right_leg [0.5,-1,0]}. The wrist/ankle keeps its "
                "own local rotation and the fingers are not touched: shape the hand with set_joints afterwards. Out-of-reach targets straighten the limb toward the "
                "target (reached=false, short_by_m). Move the pelvis/spine first (set_joints), then reach: targets "
                "are read from the skeleton as drawn. Check with joint_positions or snapshot.";
            json::object entry_props;
            entry_props["limb"]        = enumProp({ "left_arm", "right_arm", "left_leg", "right_leg" }, "Which limb.");
            entry_props["target"]      = vec3Prop("[x, y, z] meters, body frame (or offset from relative_to).");
            entry_props["relative_to"] = prop("string", "Optional joint name; target becomes an offset from it.");
            entry_props["bend_toward"] = vec3Prop("Optional [x, y, z] body-frame direction for the elbow/knee.");
            json::object entry = objectSchema(std::move(entry_props), { "limb", "target" });

            json::object p;
            p["avatar"]   = avatar_prop;
            p["limbs"]    = json::object{ { "type", "array" }, { "items", std::move(entry) }, { "minItems", 1 }, { "maxItems", 4 } };
            t.inputSchema = objectSchema(std::move(p), { "limbs" });
            t.handler     = [run](const json::object& args) -> ToolResult
            {
                return run(withHistory("reach", args, [args](const JobProgress& progress) { return PlusPoserTools::reach(args, progress); }), 30.0);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "aim";
            t.title       = "Point bones in directions";
            t.description = std::string(
                "Sets joint rotations from BONE DIRECTIONS instead of Euler angles: each entry says where a bone should "
                "point, as a unit vector in the BODY FRAME (as joint_positions: +X forward, +Y the avatar's left, +Z up), "
                "and the joint's rotation is solved. The bone of a joint runs from it to its child: mShoulderLeft = upper "
                "arm (-> mElbowLeft), mElbowLeft = forearm, mWristLeft = hand (-> mHandMiddle1Left), mHipLeft = thigh, "
                "mKneeLeft = shin, mAnkleLeft = foot (-> mFootLeft), mFootLeft -> mToeLeft, mPelvis -> mTorso, mTorso -> "
                "mChest, mChest -> mNeck, mNeck -> mHead, mHead -> mSkull (up), mCollarLeft -> mShoulderLeft, fingers "
                "mHandIndex1Left -> 2 -> 3 (the tip segment continues the bone leading to it). toward picks another child "
                "(e.g. mChest toward mCollarLeft). The direction field of joint_positions is exactly this, so its output "
                "can be fed back. Whole body in ONE call: entries are solved parents first whatever their order, each "
                "with the new rotations of the bones above it, so every bone ends up pointing where asked. Turning a "
                "parent later (set_joints, another aim) moves the children's bones with it.\n"
                "TWIST about the bone: by default the smallest turn from the bone's current orientation (its twist is "
                "KEPT, also a wrong one from an earlier call: to start over call reset_all to \"tpose\" first). Elbows "
                "and knees only bend about their hinge (twist auto): a direction the hinge can not reach is projected "
                "onto what it reaches and the miss is reported (hinge_miss_deg, warnings), never turned freely. An "
                "upper arm / thigh aimed in the same call as its forearm / shin is twisted so that the elbow / knee bends "
                "naturally (limb_roll): give both and the limb comes out right. A nearly straight arm / leg (the two "
                "bones less than 15 degrees apart) has no such twist: the upper arm / thigh gets the rest pose's twist; "
                "set the hand with mWrist and a foot with mAnkle and mFoot (a standing foot flat: mAnkle [0.88,0,-0.48] "
                "roll_toward [0,0,1], mFoot [1,0,0]). roll_toward [x,y,z] sets the twist explicitly: the joint's reference "
                "side turns toward it - upper arm / thigh: the forearm / shin direction; trunk (mPelvis, mTorso, mChest, "
                "mNeck, mHead): where the front faces; hand and fingers: where the palm faces; foot and toes: where the "
                "top of the foot faces; collar: up. Elbows and knees IGNORE roll_toward (SL has no forearm / shin twist "
                "joint; a twist there folds the mesh). AVOID roll_toward on wrists and ankles from a photo (the palm / "
                "foot direction is a depth guess and a wrong twist folds the mesh), except a standing foot set flat. "
                "twist \"keep\" (smallest turn) or \"rest\" (smallest turn from the rest pose) override the default per "
                "entry or for all (on an elbow / knee they turn it freely). Joint limits are not applied (as in the "
                "poser).\n"
                "AIM vs REACH: aim places bone DIRECTIONS (posture, copying a photo or a reference); reach places a "
                "hand or foot at a POINT (contact: hand on hip, foot on a step). FROM A PHOTO: turn keypoint positions "
                "into bone directions (child minus joint, in the body frame), aim the whole body in one call from "
                "start_posing from_tpose, compare with snapshot views (skeleton + joints_2d), then fix details with aim, "
                "reach (contacts) and set_joints (fingers, small twists). Returns per bone: rotation (Euler, as "
                "set_joints), the achieved direction, error_deg, method (swing / roll / limb_roll / hinge, bend_deg, "
                "hinge_miss_deg), angle_deg (turn from rest) and twist_deg (about its own bone), notes; and warnings: "
                "a joint turned more than 120 degrees or twisted more than 90 about its bone (the mesh may fold: check "
                "it; not for hinge bends or limb_roll upper arms / thighs, whose turn and twist are the pose), an elbow "
                "/ knee that misses by more than 5 degrees or would bend "
                "backwards.\n") +
                ROTATION_FRAME_DOC;
            json::object entry_props;
            entry_props["joint"]       = prop("string", "Joint whose bone is aimed, e.g. mShoulderLeft (upper arm).");
            entry_props["direction"]   = vec3Prop("[x, y, z] body-frame direction for the bone (any length).");
            entry_props["toward"]      = prop("string", "Optional child joint the bone runs to (default: see the description).");
            entry_props["roll_toward"] = vec3Prop("Optional [x, y, z] body-frame direction for the joint's reference side "
                                                  "(upper arm / thigh: the forearm / shin direction). Ignored on elbows / "
                                                  "knees; avoid on wrists / ankles (folds the mesh).");
            entry_props["twist"]       = enumProp({ "auto", "keep", "rest" }, "Twist rule for this bone. Default: the call's twist.");
            json::object entry = objectSchema(std::move(entry_props), { "joint", "direction" });

            json::object p;
            p["avatar"] = avatar_prop;
            p["bones"]  = json::object{ { "type", "array" }, { "items", std::move(entry) }, { "minItems", 1 },
                                        { "maxItems", (int)MAX_JOINTS_PER_CALL } };
            p["twist"]  = enumProp({ "auto", "keep", "rest" }, "Default twist rule. Default auto.");
            p["settle"] = prop("boolean", "Wait for the pose to settle before reading the skeleton. Default true.");
            t.inputSchema = objectSchema(std::move(p), { "bones" });
            t.handler     = [run](const json::object& args) -> ToolResult
            {
                return run(withHistory("aim", args, [args](const JobProgress& progress) { return PlusPoserTools::aim(args, progress); }), 30.0);
            };
            tools.push_back(std::move(t));
        }
    }
}
