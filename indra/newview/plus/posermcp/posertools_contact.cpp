/**
 * @file posertools_contact.cpp
 * @brief The contact tool of the Poser MCP (see posertools.cpp): make two body
 *        points touch, in rounds across frames.
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
// contact: make two body points touch (the maths is in contactsolver.cpp; the
// same rounds as tools/poser-mcp/contact.py, which drives reach instead).

namespace
{
    using plus::posermcp::ContactMode;
    using plus::posermcp::ContactShell;
    using plus::posermcp::ContactSide;
    using plus::posermcp::LimbGoal;
    using plus::posermcp::Meeting;
    using plus::posermcp::TrunkBend;

    // Skin points of hands and feet (the same table as NAMED_SITES in contactsolver.py).
    // They hang from the collision volumes L_HAND / L_FOOT (R_...): children of the wrist /
    // ankle, scaled by the shape and not moved by a mesh body's joint position overrides
    // (on one test avatar the drawn mFoot was 15 cm below the default while the mesh foot
    // followed the ankle, so a hand put on the bones held the air beyond the toes). The
    // offset is in the wrist's / ankle's axes (rest pose = body axes).
    struct NamedSite
    {
        const char* name;
        bool        hand;
        F32         offset[3];
        F32         normal[3];
    };
    constexpr NamedSite NAMED_SITES[] = {
        { "palm", true, { 0.f, 0.f, -0.025f }, { 0.f, 0.f, -1.f } },
        { "back_of_hand", true, { 0.f, 0.f, 0.025f }, { 0.f, 0.f, 1.f } },
        { "instep", false, { 0.01f, 0.f, 0.04f }, { 0.f, 0.f, 1.f } },
        { "sole", false, { 0.f, 0.f, -0.05f }, { 0.f, 0.f, -1.f } },
        { "heel", false, { -0.11f, 0.f, -0.02f }, { -1.f, 0.f, 0.f } },
    };

    // Whether a joint is on a hand (wrist, fingers) or a foot (ankle, foot, toes), and its side.
    bool handOrFoot(const std::string& joint, bool& hand, std::string& side)
    {
        side = boost::ends_with(joint, "Left") ? "Left" : boost::ends_with(joint, "Right") ? "Right" : "";
        if (side.empty())
            return false;
        if (boost::starts_with(joint, "mWrist") || boost::starts_with(joint, "mHand"))
        {
            hand = true;
            return true;
        }
        if (boost::starts_with(joint, "mAnkle") || boost::starts_with(joint, "mFoot") || boost::starts_with(joint, "mToe"))
        {
            hand = false;
            return true;
        }
        return false;
    }

    struct ContactEnd
    {
        std::string              joint;  // as asked
        std::string              anchor; // what the point hangs from (a joint or a collision volume)
        std::string              frame;  // the joint whose axes the offset is in
        LLVector3                offset;
        std::string              site;   // for the report
        std::optional<LLVector3> normal; // the skin's outward direction (frame axes), for grip
        std::optional<LLVector3> along;  // its long axis
        std::optional<LLVector3> bendToward;
    };

    struct ContactPair
    {
        ContactEnd  a, b;
        std::string mover = "auto";
        bool        grip  = false;
    };
}

namespace posertools_detail
{
    // Declared in posertools_impl.h: PlusPoserTools::contact() takes it.
    struct ContactJob
    {
        bool                     parsed = false;
        std::vector<ContactPair> pairs;
        F32                      tolerance   = 0.005f;
        F32                      comfort     = 0.92f;
        F32                      maxTrunkDeg = 25.f;
        F32                      gripRadius  = 0.045f;
        int                      maxRounds   = 8;
        bool                     trunk       = true;

        unsigned appliedAtCall = 0;   // progress.calls when the last change was applied
        double   appliedAtAge  = 0.0; // progress.firstCallAge then
        int      roundsLeft    = 0;
        int      gripStage     = 0;   // 0 contact, 1 contact again after turning the palms, 2 fingers curled
        size_t   passStart     = 0;   // index in distances where this pass of rounds began

        std::vector<F32>                   trunkUsedDeg;
        std::vector<json::array>           modes, distances;
        std::vector<std::set<std::string>> moved;
        std::vector<json::object>          last;
        std::vector<F32>                   before;
        json::array                        notes;
    };
}

namespace
{
    // The limb whose wrist / ankle is the joint or one of its parents.
    const Limb* limbCarrying(LLVOAvatar* avatar, const std::string& name)
    {
        for (LLJoint* joint = avatar->getJoint(name); joint; joint = joint->getParent())
        {
            for (const Limb& limb : limbs())
            {
                if (joint->getName() == limb.end)
                    return &limb;
            }
        }
        return nullptr;
    }

    bool hangsFrom(LLVOAvatar* avatar, const std::string& name, const std::string& ancestor)
    {
        for (LLJoint* joint = avatar->getJoint(name); joint; joint = joint->getParent())
        {
            if (joint->getName() == ancestor)
                return true;
        }
        return false;
    }

    // One side of a pair as drawn now (world space).
    struct BuiltEnd
    {
        LLVector3    point;
        const Limb*  limb = nullptr;
        TwoBoneInput input;
        LLVector3    offset; // point - wrist / ankle
        ContactSide  side;
    };

    BuiltEnd buildEnd(LLVOAvatar* avatar, const ContactEnd& end, const BodyFrame& frame)
    {
        LLJoint* anchor = avatar->getJoint(end.anchor);
        LLJoint* axes   = avatar->getJoint(end.frame);
        if (!anchor || !axes)
            throw ToolError(end.joint + " is not part of this avatar's skeleton");
        BuiltEnd out;
        out.point        = anchor->getWorldPosition() + end.offset * axes->getWorldRotation();
        out.side.point   = out.point;
        out.limb         = limbCarrying(avatar, end.frame);
        out.side.movable = out.limb != nullptr;
        if (!out.limb)
            return out;
        LLJoint* root   = avatar->getJoint(out.limb->root);
        LLJoint* middle = avatar->getJoint(out.limb->middle);
        LLJoint* tip    = avatar->getJoint(out.limb->end);
        if (!root || !middle || !tip || !root->getParent())
            throw ToolError(std::string(out.limb->name) + " is not part of this avatar's skeleton");
        out.input.root        = root->getWorldPosition();
        out.input.middle      = middle->getWorldPosition();
        out.input.end         = tip->getWorldPosition();
        out.input.rootWorld   = root->getWorldRotation();
        out.input.middleWorld = middle->getWorldRotation();
        out.input.parentWorld = root->getParent()->getWorldRotation();
        out.input.hinge       = out.limb->hinge;
        out.input.natural     = out.limb->natural;
        out.offset            = out.point - out.input.end;
        out.side.arm          = std::string(out.limb->name).find("arm") != std::string::npos;
        F32         rmin = 0.f, rmax = 0.f;
        const char* why  = "";
        if (!plus::posermcp::reachRange(out.input, rmin, rmax, why))
            throw ToolError(std::string(out.limb->name) + ": " + why);
        out.side.shell.centre   = out.input.root + out.offset;
        out.side.shell.minReach = rmin;
        out.side.shell.maxReach = rmax;

        // A standing leg: its ankle is (nearly) the lowest and well below the hip.
        if (!out.side.arm)
        {
            LLJoint* left  = avatar->getJoint("mAnkleLeft");
            LLJoint* right = avatar->getJoint("mAnkleRight");
            if (left && right)
            {
                const F32 z      = frame.pointToBody(out.input.end).mV[VZ];
                const F32 lowest = llmin(frame.pointToBody(left->getWorldPosition()).mV[VZ],
                                         frame.pointToBody(right->getWorldPosition()).mV[VZ]);
                const F32 leg    = (out.input.middle - out.input.root).magVec() + (out.input.end - out.input.middle).magVec();
                out.side.support = z <= lowest + 0.08f && frame.pointToBody(out.input.root).mV[VZ] - z >= 0.6f * leg;
            }
        }
        return out;
    }

    const char* modeName(ContactMode mode)
    {
        switch (mode)
        {
            case ContactMode::A:    return "a";
            case ContactMode::B:    return "b";
            case ContactMode::Both: return "both";
            case ContactMode::None: break;
        }
        return "none";
    }
}

std::optional<ToolResult> PlusPoserTools::contact(const json::object& args, const JobProgress& progress, ContactJob& job)
{
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (!canPose(floater, avatar))
        throw ToolError("You can only pose yourself or an animesh you own, in your current region.");
    requirePosing(floater, avatar);

    if (!job.parsed)
    {
        for (const auto& kv : args)
        {
            static const std::set<std::string> known{ "avatar", "contacts", "tolerance", "max_rounds", "comfort", "trunk",
                                                      "max_trunk_deg", "grip_radius" };
            if (!known.count(std::string(kv.key())))
                throw ToolError("unknown argument '" + std::string(kv.key()) + "'");
        }
        auto number = [&](const char* key, F32 fallback, F32 lo, F32 hi)
        {
            const F32 v = (F32)argNumber(args, key).value_or(fallback);
            if (v < lo || v > hi)
                throw ToolError(std::string(key) + " must be within " + std::to_string(lo) + " .. " + std::to_string(hi));
            return v;
        };
        job.tolerance   = number("tolerance", 0.005f, 0.001f, 0.05f);
        job.comfort     = number("comfort", 0.92f, 0.5f, 1.f);
        job.maxTrunkDeg = number("max_trunk_deg", 25.f, 0.f, 45.f);
        job.gripRadius  = number("grip_radius", 0.045f, 0.02f, 0.1f);
        job.maxRounds   = (int)number("max_rounds", 8.f, 1.f, 20.f);
        job.trunk       = argBool(args, "trunk", true);

        const json::value* raw = args.if_contains("contacts");
        if (!raw || !raw->is_array() || raw->get_array().empty() || raw->get_array().size() > 6)
            throw ToolError("'contacts' must be an array of 1-6 {a, b, mover?, offset?, grip?}");

        auto parse_end = [&](const json::value& v, const std::string& what) -> ContactEnd
        {
            ContactEnd end;
            json::object spec;
            if (v.is_string())
                spec["joint"] = v.get_string();
            else if (v.is_object())
                spec = v.get_object();
            else
                throw ToolError(what + " must be a joint name or {joint, site?, offset?, bend_toward?}");
            for (const auto& kv : spec)
            {
                if (kv.key() != "joint" && kv.key() != "site" && kv.key() != "offset" && kv.key() != "bend_toward")
                    throw ToolError("unknown field '" + std::string(kv.key()) + "' in " + what);
            }
            end.joint = requireJoint(floater, argString(spec, "joint", "")).jointName();
            if (const json::value* bend = spec.if_contains("bend_toward"); bend && !bend->is_null())
            {
                LLVector3 d = parseVec3(*bend, what + " bend_toward");
                if (d.normalize() < 0.001f)
                    throw ToolError(what + ": bend_toward must not be zero");
                end.bendToward = d;
            }
            const std::string site = argString(spec, "site", "surface");
            if (const json::value* off = spec.if_contains("offset"); off && !off->is_null())
            {
                end.offset = parseVec3(*off, what + " offset");
                if (end.offset.magVec() > 0.5f)
                    throw ToolError(what + ": offset must be within 0.5 m");
                end.anchor = end.frame = end.joint;
                end.site   = "offset";
                return end;
            }
            bool        hand = false;
            std::string side;
            const bool  limb_end = handOrFoot(end.joint, hand, side);
            if (site == "joint" || (site == "surface" && !limb_end))
            {
                end.anchor = end.frame = end.joint;
                end.site   = "joint";
                return end;
            }
            const std::string name = site == "surface" ? (hand ? "palm" : "instep") : site;
            const NamedSite*  found = nullptr;
            for (const NamedSite& s : NAMED_SITES)
            {
                if (name == s.name)
                    found = &s;
            }
            if (!found)
                throw ToolError(what + ": site must be surface, joint, palm, back_of_hand, instep, sole or heel");
            if (!limb_end || found->hand != hand)
                throw ToolError(what + ": site " + name + " is on a " + (found->hand ? "hand" : "foot") + "; " + end.joint +
                                " is not");
            end.anchor = std::string(hand ? (side == "Left" ? "L_HAND" : "R_HAND") : (side == "Left" ? "L_FOOT" : "R_FOOT"));
            end.frame  = std::string(hand ? "mWrist" : "mAnkle") + side;
            end.offset = LLVector3(found->offset);
            end.normal = LLVector3(found->normal);
            end.along  = hand ? LLVector3(0.f, side == "Left" ? 1.f : -1.f, 0.f) : LLVector3::x_axis;
            end.site   = name;
            return end;
        };

        int index = 0;
        for (const json::value& item : raw->get_array())
        {
            const std::string what = "contacts[" + std::to_string(index++) + "]";
            if (!item.is_object())
                throw ToolError(what + " must be an object {a, b, mover?, offset?, grip?}");
            const json::object& entry = item.get_object();
            for (const auto& kv : entry)
            {
                if (kv.key() != "a" && kv.key() != "b" && kv.key() != "mover" && kv.key() != "offset" && kv.key() != "grip")
                    throw ToolError("unknown field '" + std::string(kv.key()) + "' in " + what);
            }
            const json::value* va = entry.if_contains("a");
            const json::value* vb = entry.if_contains("b");
            if (!va || !vb)
                throw ToolError(what + ": a and b are required");
            ContactPair pair;
            pair.a = parse_end(*va, what + ".a");
            json::value b_value = *vb;
            if (const json::value* off = entry.if_contains("offset"); off && !off->is_null())
            {
                // the pair's offset is b's (in b's frame)
                json::object spec = b_value.is_object() ? b_value.get_object() : json::object{ { "joint", b_value } };
                if (!spec.contains("offset"))
                    spec["offset"] = *off;
                b_value = spec;
            }
            pair.b     = parse_end(b_value, what + ".b");
            pair.mover = argString(entry, "mover", "auto");
            if (pair.mover != "auto" && pair.mover != "a" && pair.mover != "b" && pair.mover != "both")
                throw ToolError(what + ": mover must be auto, a, b or both");
            pair.grip = argBool(entry, "grip", false);

            const Limb* la = limbCarrying(avatar, pair.a.frame);
            const Limb* lb = limbCarrying(avatar, pair.b.frame);
            if ((la && hangsFrom(avatar, pair.b.anchor, la->root)) || (lb && hangsFrom(avatar, pair.a.anchor, lb->root)))
                throw ToolError(what + ": " + pair.a.joint + " and " + pair.b.joint + " are on the same limb");
            if ((pair.mover == "a" || pair.mover == "both") && !la)
                throw ToolError(what + ": a (" + pair.a.joint + ") is not on a hand or foot (an arm below the elbow or a leg "
                                "below the knee), so it cannot move");
            if ((pair.mover == "b" || pair.mover == "both") && !lb)
                throw ToolError(what + ": b (" + pair.b.joint + ") is not on a hand or foot, so it cannot move");
            if (pair.grip && (!la || std::string(la->name).find("arm") == std::string::npos))
                throw ToolError(what + ": grip needs a hand as a");
            job.pairs.push_back(std::move(pair));
        }
        const size_t n = job.pairs.size();
        job.trunkUsedDeg.assign(n, 0.f);
        job.modes.assign(n, json::array());
        job.distances.assign(n, json::array());
        job.moved.assign(n, {});
        job.last.assign(n, json::object());
        job.roundsLeft = job.maxRounds;

        // A mesh body can move the foot joints away from the mesh foot (joint position
        // overrides); the surface sites do not depend on them.
        for (const char* side : { "Left", "Right" })
        {
            LLJoint* ankle = avatar->getJoint(std::string("mAnkle") + side);
            LLJoint* foot  = avatar->getJoint(std::string("mFoot") + side);
            if (!ankle || !foot)
                continue;
            const LLVector3 drawn = (foot->getWorldPosition() - ankle->getWorldPosition()) * ~ankle->getWorldRotation();
            const F32       off   = (drawn - LLVector3(0.112f, 0.f, -0.061f)).magVec();
            if (off > 0.03f)
                job.notes.push_back(json::string(std::string("mFoot") + side + " is drawn " +
                                                 std::to_string((int)std::lround(off * 100.f)) +
                                                 " cm away from the default foot (a joint position override?); the mesh foot "
                                                 "may not follow it - the instep / sole / heel sites hang from the foot's "
                                                 "collision volume instead"));
        }
        job.parsed = true;
    }

    // Wait until the skeleton shows what was applied last (at least two frames, at most ~4 s).
    const bool settled = posesSettled(floater);
    if (progress.calls < job.appliedAtCall + 2 || (!settled && progress.firstCallAge - job.appliedAtAge < 4.0))
        return std::nullopt;
    auto applied = [&]()
    {
        refreshWindow(floater, false);
        job.appliedAtCall = progress.calls;
        job.appliedAtAge  = progress.firstCallAge;
        return std::nullopt;
    };

    const BodyFrame frame = bodyFrame(avatar);
    const size_t    n     = job.pairs.size();

    // Measure and plan every pair from the skeleton as drawn.
    std::vector<BuiltEnd> ends_a, ends_b;
    std::vector<F32>      gaps;
    for (const ContactPair& pair : job.pairs)
    {
        ends_a.push_back(buildEnd(avatar, pair.a, frame));
        ends_b.push_back(buildEnd(avatar, pair.b, frame));
        gaps.push_back((ends_a.back().point - ends_b.back().point).magVec());
    }
    if (job.before.empty())
        job.before = gaps;
    for (size_t i = 0; i < n; ++i)
        job.distances[i].push_back(rounded(gaps[i], 4));

    bool all_touch = true;
    for (F32 g : gaps)
        all_touch = all_touch && g <= job.tolerance;
    bool stalled = false;
    if (!all_touch && job.distances[0].size() >= job.passStart + 3)
    {
        stalled = true;
        for (size_t i = 0; i < n; ++i)
        {
            const json::array& d = job.distances[i];
            const double       earlier = d[d.size() - 3].to_number<double>();
            if (gaps[i] > job.tolerance && earlier - gaps[i] >= 0.001)
                stalled = false;
        }
    }

    // Done with this pass: grip turns the palms and comes back, then curls the fingers.
    if (all_touch || stalled || job.roundsLeft <= 0)
    {
        bool any_grip = false;
        for (const ContactPair& pair : job.pairs)
            any_grip = any_grip || pair.grip;
        if (any_grip && job.gripStage == 0)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const ContactPair& pair = job.pairs[i];
                if (!pair.grip)
                    continue;
                const Limb& limb  = *ends_a[i].limb;
                LLJoint*    wrist = avatar->getJoint(limb.end);
                LLJoint*    hand  = wrist ? boneEnd(avatar, wrist) : nullptr;
                LLJoint*    axes  = avatar->getJoint(pair.b.frame);
                if (!wrist || !hand || !wrist->getParent() || !axes)
                    continue;
                // b's skin faces n; the palm faces it back, the fingers cross b's long axis.
                LLVector3 n_world = pair.b.normal ? *pair.b.normal * axes->getWorldRotation()
                                                  : wrist->getWorldPosition() - ends_b[i].point;
                n_world.normalize();
                LLVector3 hand_dir = hand->getWorldPosition() - wrist->getWorldPosition();
                hand_dir.normalize();
                LLVector3 want = hand_dir - n_world * (hand_dir * n_world);
                if (pair.b.along)
                {
                    LLVector3 across = n_world % (*pair.b.along * axes->getWorldRotation());
                    if (across.normalize() > 0.01f)
                        want = across * hand_dir >= 0.f ? across : -across;
                }
                if (want.normalize() < 0.01f)
                    continue;
                LLVector3 palm_local = pair.a.normal.value_or(-LLVector3::z_axis);
                LLVector3 bone_local = (hand->getWorldPosition() - wrist->getWorldPosition()) * ~wrist->getWorldRotation();
                bone_local.normalize();
                palm_local = palm_local - bone_local * (palm_local * bone_local);
                LLVector3 palm_world = -n_world - want * (-n_world * want);
                if (palm_local.normalize() < 0.01f || palm_world.normalize() < 0.01f)
                    continue;
                const LLQuaternion wrist_world = plus::posermcp::basisRotation(bone_local, palm_local, want, palm_world);
                applyRotation(floater, avatar, requireJoint(floater, limb.end), wrist_world * ~wrist->getParent()->getWorldRotation());
            }
            job.gripStage  = 1;
            job.roundsLeft = job.maxRounds;
            job.passStart  = job.distances[0].size(); // the palm moved: a new pass
            return applied(); // and contact again: the palm moved
        }
        if (any_grip && job.gripStage == 1)
        {
            for (const ContactPair& pair : job.pairs)
            {
                if (!pair.grip)
                    continue;
                bool        hand_side = false;
                std::string side;
                handOrFoot(pair.a.frame, hand_side, side);
                const F32 sign = side == "Left" ? -1.f : 1.f;
                for (const char* finger : { "Index", "Middle", "Ring", "Pinky" })
                {
                    for (int k = 1; k <= 3; ++k)
                    {
                        const std::string name = std::string("mHand") + finger + std::to_string(k) + side;
                        LLJoint*          j    = avatar->getJoint(name);
                        if (!j)
                            continue;
                        LLJoint*  next   = boneEnd(avatar, j);
                        const F32 length = next ? (next->getWorldPosition() - j->getWorldPosition()).magVec() : 0.03f;
                        F32       angle  = llclamp(length / job.gripRadius * RAD_TO_DEG, 15.f, 80.f);
                        if (k == 1)
                            angle = llmin(80.f, angle + 15.f); // round the edge of the palm
                        applyRotation(floater, avatar, requireJoint(floater, name),
                                      eulerDegreesToQuat(LLVector3(sign * angle, 0.f, 0.f)));
                    }
                }
            }
            job.gripStage = 2;
            return applied(); // report once the fingers have eased in
        }

        json::array report;
        for (size_t i = 0; i < n; ++i)
        {
            const ContactPair& pair = job.pairs[i];
            json::object       entry = job.last[i];
            entry["a"]                 = pair.a.joint;
            entry["b"]                 = pair.b.joint;
            entry["a_site"]            = pair.a.site;
            entry["b_site"]            = pair.b.site;
            entry["distance_before_m"] = rounded(job.before[i], 4);
            entry["distance_m"]        = rounded(gaps[i], 4);
            entry["touching"]          = gaps[i] <= job.tolerance;
            if (gaps[i] > job.tolerance)
                entry["short_by_m"] = rounded(gaps[i], 4);
            entry["a_point"]   = vecJson(frame.pointToBody(ends_a[i].point), 3);
            entry["b_point"]   = vecJson(frame.pointToBody(ends_b[i].point), 3);
            entry["modes"]     = job.modes[i];
            entry["distances_m"] = job.distances[i];
            json::array moved;
            for (const std::string& m : job.moved[i])
                moved.push_back(json::string(m));
            entry["moved"] = std::move(moved);
            if (job.trunkUsedDeg[i] > 0.f)
                entry["trunk_deg_total"] = rounded(job.trunkUsedDeg[i], 1);
            if (pair.grip)
                entry["grip"] = true;
            report.push_back(std::move(entry));
        }
        json::object result;
        result["contacts"]    = std::move(report);
        result["rounds"]      = (int)job.distances[0].size();
        result["tolerance_m"] = rounded(job.tolerance, 4);
        if (stalled)
            result["stopped"] = "no progress over two rounds";
        if (!job.notes.empty())
            result["notes"] = job.notes;
        if (!settled)
            result["note"] = "Some joints had not reached their targets when the skeleton was read; check with joint_positions.";
        result["next"] = "Check the contact from several sides including straight above: snapshot views [\"front\", \"left\", "
                         "\"right\", {\"view\":\"top\",\"elevation\":89}] with focus on the touching joint and a short distance "
                         "(0.5 m). Bones are not skin: if the mesh does not touch, give an offset or another site. Shape "
                         "the hand round what it holds (grip: true, or set_joints on the fingers).";
        return ToolResult::json(result);
    }

    // Plan this round, pairs in order. A limb moved by an earlier pair this round is not
    // moved again; one that holds another pair's contact is kept still when another can go.
    std::set<const Limb*>                    claimed;
    std::vector<std::pair<const Limb*, LimbGoal>> moves;
    std::optional<TrunkBend>                 turn;
    std::vector<std::vector<std::string>>    moved_now(n);
    for (size_t i = 0; i < n; ++i)
    {
        const ContactPair& pair = job.pairs[i];
        BuiltEnd&          a    = ends_a[i];
        BuiltEnd&          b    = ends_b[i];
        json::object       info;
        if (gaps[i] <= job.tolerance)
        {
            job.modes[i].push_back("touching");
            job.last[i] = info;
            continue;
        }
        ContactMode mode = ContactMode::None;
        if (pair.mover == "auto")
        {
            json::array kept;
            for (BuiltEnd* e : { &a, &b })
            {
                if (!e->limb)
                    continue;
                if (claimed.count(e->limb))
                    e->side.movable = false;
                for (size_t j = 0; j < n && !claimed.count(e->limb); ++j)
                {
                    if (j != i && (limbCarrying(avatar, job.pairs[j].a.frame) == e->limb ||
                                   limbCarrying(avatar, job.pairs[j].b.frame) == e->limb))
                        e->side.busy = true;
                }
            }
            const char* note = "";
            mode             = plus::posermcp::resolveAuto(a.side, b.side, job.comfort, job.tolerance, note);
            if (*note)
                info["note"] = note;
            for (BuiltEnd* e : { &a, &b })
            {
                const bool moves_e = mode == ContactMode::Both || (e == &a ? mode == ContactMode::A : mode == ContactMode::B);
                if (e->side.busy && !moves_e)
                    kept.push_back(json::string(e->limb->name));
            }
            if (!kept.empty())
                info["kept_still"] = std::move(kept);
        }
        else
        {
            mode = pair.mover == "a" ? ContactMode::A : pair.mover == "b" ? ContactMode::B : ContactMode::Both;
        }
        job.modes[i].push_back(modeName(mode));
        if (mode == ContactMode::None)
        {
            job.last[i] = info;
            continue;
        }
        Meeting meet = plus::posermcp::chooseMeeting(mode, a.point, b.point, a.side.shell, b.side.shell, job.comfort);
        info["planned_gap_m"] = rounded(meet.gap, 4);
        // One mover aims at the other point itself: the solve finds the nearest it can get
        // (the shell assumes the contact point's offset keeps its direction).
        if (mode == ContactMode::A)
            meet.goalA = b.point;
        else if (mode == ContactMode::B)
            meet.goalB = a.point;

        // Not enough even with both: bend the trunk a little, if an arm moves and the other
        // point does not hang from the chest itself (the turn would carry it along).
        if (meet.gap > job.tolerance && job.trunk && pair.mover == "auto" && !turn &&
            job.trunkUsedDeg[i] < job.maxTrunkDeg - 0.5f)
        {
            LLJoint* torso = avatar->getJoint("mTorso");
            LLJoint* chest = avatar->getJoint("mChest");
            for (int k = 0; k < 2 && torso && chest; ++k)
            {
                const BuiltEnd&   arm        = k == 0 ? a : b;
                const BuiltEnd&   other      = k == 0 ? b : a;
                const ContactEnd& other_spec = k == 0 ? pair.b : pair.a;
                const bool arm_moves = mode == ContactMode::Both || (k == 0 ? mode == ContactMode::A : mode == ContactMode::B);
                if (!arm.limb || !arm.side.arm || !arm_moves || hangsFrom(avatar, other_spec.anchor, "mChest"))
                    continue;
                const bool      other_moves = mode == ContactMode::Both;
                const LLVector3 goal  = other_moves ? other.side.shell.centre : other.point;
                const F32       reach = 0.98f * (arm.side.shell.maxReach + (other_moves ? other.side.shell.maxReach : 0.f));
                const TrunkBend bend  = plus::posermcp::trunkBend(torso->getWorldPosition(), chest->getWorldPosition(),
                                                                  arm.side.shell.centre, goal, reach,
                                                                  (job.maxTrunkDeg - job.trunkUsedDeg[i]) * DEG_TO_RAD);
                if (bend.angle * RAD_TO_DEG > 0.5f)
                {
                    turn = bend;
                    job.trunkUsedDeg[i] += bend.angle * RAD_TO_DEG;
                    info["trunk_deg"] = rounded(bend.angle * RAD_TO_DEG, 1);
                }
                break;
            }
            if (turn)
            {
                job.last[i] = info;
                continue;
            }
        }

        json::array limbs_moved;
        for (int k = 0; k < 2; ++k)
        {
            BuiltEnd&         e    = k == 0 ? a : b;
            const ContactEnd& spec = k == 0 ? pair.a : pair.b;
            const LLVector3&  goal = k == 0 ? meet.goalA : meet.goalB;
            const bool moves_e = mode == ContactMode::Both || (k == 0 ? mode == ContactMode::A : mode == ContactMode::B);
            if (!moves_e || !e.limb || (goal - e.point).magVec() < 1e-6f)
                continue;
            e.input.pole = spec.bendToward ? frame.dirToWorld(*spec.bendToward)
                                           : plus::posermcp::autoPole(e.input, frame.dirToWorld(e.limb->pole));
            e.input.pole.normalize();
            LimbGoal    solved;
            const char* why = "";
            if (!plus::posermcp::solveLimbToGoal(e.input, e.offset, goal, solved, why))
                throw ToolError(std::string(e.limb->name) + ": " + why);
            moves.emplace_back(e.limb, solved);
            claimed.insert(e.limb);
            moved_now[i].push_back(e.limb->name);
            limbs_moved.push_back(json::string(e.limb->name));
            if (solved.miss > job.tolerance)
                info[std::string(e.limb->name) + "_short_m"] = rounded(solved.miss, 4);
        }
        info["moved"] = std::move(limbs_moved);
        job.last[i]   = info;
    }
    --job.roundsLeft;

    if (turn)
    {
        // The trunk first; the limbs are planned again from the bent trunk next round.
        LLJoint* torso = avatar->getJoint("mTorso");
        LLJoint* chest = avatar->getJoint("mChest");
        if (!torso || !chest || !torso->getParent())
            throw ToolError("mTorso / mChest are not part of this avatar's skeleton");
        const LLQuaternion torso_world  = torso->getWorldRotation();
        const LLQuaternion torso_parent = torso->getParent()->getWorldRotation();
        LLQuaternion       new_torso, new_chest;
        plus::posermcp::trunkLocals(torso_world * ~torso_parent, torso_parent, chest->getWorldRotation() * ~torso_world, torso_world,
                                    turn->axis, turn->angle, new_torso, new_chest);
        applyRotation(floater, avatar, requireJoint(floater, "mTorso"), new_torso);
        applyRotation(floater, avatar, requireJoint(floater, "mChest"), new_chest);
        return applied();
    }
    if (moves.empty())
    {
        job.roundsLeft = 0; // nothing can move: report next frame
        return applied();
    }
    for (size_t i = 0; i < n; ++i)
        job.moved[i].insert(moved_now[i].begin(), moved_now[i].end());
    for (const auto& [limb, solved] : moves)
    {
        applyRotation(floater, avatar, requireJoint(floater, limb->root), solved.solved.rootLocal);
        applyRotation(floater, avatar, requireJoint(floater, limb->middle), solved.solved.middleLocal);
    }
    return applied();
}

// ---------------------------------------------------------------------------
// Tool table: contact

namespace plus::posermcp
{
    void appendContactTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        const json::object avatar_prop = prop("string", AVATAR_ARG_DOC);

        {
            Tool t;
            t.name        = "contact";
            t.title       = "Make two body points touch";
            t.description =
                "Makes two points of the body TOUCH (hand on foot, hand on hand, hand on chin / hip / knee, foot on knee), "
                "solving the arms and legs in rounds until the points meet: after aim has put in the whole pose from a "
                "photo, body proportions and the photo's depth guess leave hands and feet a few cm to 0.4 m apart; contact "
                "closes that. Each entry of contacts: {\"a\": joint or {\"joint\", \"site\"?, \"offset\"?, \"bend_toward\"?}, "
                "\"b\": the same, \"mover\": auto|a|b|both, \"offset\"?: [x,y,z] (= b's offset), \"grip\"?: true}. A point "
                "moves when it is on a hand or foot (wrist, fingers / ankle, foot, toes: the arm or leg moves); anything "
                "else (chin, head, hip, knee, chest) stays where it is. site: surface (default) puts the point on the skin "
                "of a hand (palm) or foot (instep = top of the foot) instead of inside at the joint; also palm, "
                "back_of_hand, instep, sole, heel; joint = the joint centre. Hand and foot sites hang from the collision "
                "volumes L_HAND / L_FOOT (a mesh body can move the foot JOINTS away from the mesh foot: the report notes "
                "it). offset: [x,y,z] meters from the joint in that joint's own axes (rest pose: +X forward, +Y the "
                "avatar's left, +Z up), for anywhere else on the skin (e.g. the side of the hip: b {\"joint\":\"mHipLeft\","
                "\"offset\":[0,0.12,0.05]}). mover auto (default): the arm rather than the leg, never a standing leg, never "
                "a limb that holds another contact of the same call when another can go; only one side when it reaches "
                "comfortably, else both move and meet in between, else the trunk (mTorso + mChest) bends a little "
                "(max_trunk_deg) and what is left is reported (short_by_m). bend_toward: where the elbow / knee points "
                "(body frame); default: where it points now. grip: afterwards the palm turns onto b's surface, the fingers "
                "across it, the hand comes back onto it, and the fingers curl round it (grip_radius). Contacts are solved "
                "in order, so list the most important first. Returns per contact: distance_before_m, distance_m, "
                "touching, the points (body frame), modes and distances per round, moved limbs, short_by_m, trunk_deg_total. "
                "CHECK THE MESH, not only the numbers: snapshot views front, left, right AND straight above "
                "({\"view\":\"top\",\"elevation\":89}) with focus on the joint and distance 0.5; if the skin does not "
                "touch, change site / offset and call contact again.";
            json::object end_props;
            end_props["joint"]       = prop("string", "Joint name (mWristLeft, mFootLeft, mFaceChin, mHipLeft, mKneeRight ...).");
            end_props["site"]        = enumProp({ "surface", "joint", "palm", "back_of_hand", "instep", "sole", "heel" },
                                                "Where on a hand / foot: default surface (hand: palm, foot: instep).");
            end_props["offset"]      = vec3Prop("[x, y, z] m from the joint in its own axes (overrides site).");
            end_props["bend_toward"] = vec3Prop("Body-frame direction for this limb's elbow / knee. Default: as now.");
            json::array end_kinds;
            end_kinds.push_back(prop("string", "Joint name."));
            end_kinds.push_back(objectSchema(std::move(end_props), { "joint" }));
            const json::object end_schema{ { "anyOf", std::move(end_kinds) } };

            json::object pair_props;
            pair_props["a"]      = end_schema;
            pair_props["b"]      = end_schema;
            pair_props["mover"]  = enumProp({ "auto", "a", "b", "both" }, "Which side moves. Default auto.");
            pair_props["offset"] = vec3Prop("b's offset (as b.offset).");
            pair_props["grip"]   = prop("boolean", "a is a hand: turn the palm onto b and curl the fingers round it. Default false.");
            json::object pair_schema = objectSchema(std::move(pair_props), { "a", "b" });

            json::object p;
            p["avatar"]        = avatar_prop;
            p["contacts"]      = json::object{ { "type", "array" }, { "items", std::move(pair_schema) }, { "minItems", 1 }, { "maxItems", 6 } };
            p["tolerance"]     = prop("number", "Meters; a pair touches when its points are this close. Default 0.005 (0.001-0.05).");
            p["max_rounds"]    = prop("integer", "Rounds of solve -> wait for the skeleton -> measure. Default 8 (1-20).");
            p["comfort"]       = prop("number", "Share of a limb's reach it uses before the other side comes too. Default 0.92.");
            p["trunk"]         = prop("boolean", "May bend mTorso + mChest when the limbs are too short (auto). Default true.");
            p["max_trunk_deg"] = prop("number", "Most the trunk may bend per contact, degrees. Default 25 (0-45).");
            p["grip_radius"]   = prop("number", "grip: radius (m) the fingers wrap round. Default 0.045 (0.02-0.1).");
            t.inputSchema      = objectSchema(std::move(p), { "contacts" });
            t.handler          = [run](const json::object& args) -> ToolResult
            {
                auto job = std::make_shared<ContactJob>();
                return run(withHistory("contact", args, [args, job](const JobProgress& progress) { return PlusPoserTools::contact(args, progress, *job); }), 90.0);
            };
            tools.push_back(std::move(t));
        }
    }
}
