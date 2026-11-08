/**
 * @file posertools_snapshot.cpp
 * @brief The snapshot and camera tools of the Poser MCP (see posertools.cpp):
 *        the world view or off-screen views as PNG, the skeleton overlay.
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

namespace
{
    constexpr char SNAPSHOT_DIRECTORY[]  = "plus_poser_snapshots"; // in user_settings
    constexpr S32  SNAPSHOT_SLOTS        = 20;                     // rolling file names

    CameraShot currentShot()
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        CameraShot            shot;
        shot.origin = camera->getOrigin();
        shot.at     = camera->getAtAxis();
        shot.left   = camera->getLeftAxis();
        shot.up     = camera->getUpAxis();
        shot.view   = camera->getView();
        shot.aspect = camera->getAspect();
        return shot;
    }

    const char JOINTS_2D_DOC[] = "joints_2d: pixel position [x, y] of each joint in the image (origin top left, y down), "
                                 "only joints in front of the camera and inside the picture; it does not know whether the "
                                 "body hides a joint.";

    std::string encodePng(LLImageRaw* raw)
    {
        LLPointer<LLImagePNG> png = new LLImagePNG;
        if (!png->encode(raw, 0.f) || png->getDataSize() <= 0)
            throw ToolError("PNG encoding failed.");
        return std::string(reinterpret_cast<const char*>(png->getData()), (size_t)png->getDataSize());
    }

    // Also keeps every snapshot on disk (rolling names, so the folder stays small).
    // Returns the path, or "" when it could not be written.
    std::string saveSnapshotFile(const std::string& bytes)
    {
        static S32        next_slot = 0;
        const std::string dir       = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, SNAPSHOT_DIRECTORY);
        if (!gDirUtilp->fileExists(dir))
            LLFile::mkdir(dir);
        const std::string path = gDirUtilp->add(dir, llformat("poser_snapshot_%02d.png", next_slot));
        next_slot              = (next_slot + 1) % SNAPSHOT_SLOTS;

        llofstream file(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.is_open())
            return std::string();
        file.write(bytes.data(), (std::streamsize)bytes.size());
        file.close(); // the data reaches the disk here: a failure shows only now
        if (file.fail())
        {
            LLFile::remove(path); // a cut-off PNG is not left behind
            return std::string();
        }
        return path;
    }
}

std::optional<ToolResult> PlusPoserTools::snapshot(const json::object& args, const JobProgress& progress)
{
    if (!gViewerWindow)
        throw ToolError("No viewer window.");

    if (const json::value* views = args.if_contains("views"); views && !views->is_null())
        return snapshotViews(args, progress);
    for (const char* key : { "layout", "focus", "fov" })
    {
        if (const json::value* v = args.if_contains(key); v && !v->is_null())
            throw ToolError(std::string("'") + key + "' only applies together with 'views'");
    }

    const bool settle  = argBool(args, "settle", true);
    const bool show_ui = argBool(args, "show_ui", false);
    // The UI shows IMs, group chat, the inventory and other people's names: it leaves the viewer only when the
    // user allowed it (security audit A M3). The setting is not one the settings tools may change (PlusPoserMcp*).
    static LLCachedControl<bool> allow_ui(gSavedSettings, "PlusPoserMcpAllowUiSnapshot", false);
    if (show_ui && !allow_ui)
        throw ToolError("show_ui is off in this viewer: the UI can show IMs, chat and other private things. The user can allow "
                        "it in the viewer (Preferences > Charis > Allow snapshots with the UI). Take the snapshot without show_ui.");

    const S32 window_width  = llmax(1, gViewerWindow->getWorldViewWidthRaw());
    const S32 window_height = llmax(1, gViewerWindow->getWorldViewHeightRaw());

    std::optional<double> want_width  = argNumber(args, "width");
    std::optional<double> want_height = argNumber(args, "height");
    S32 width = 0, height = 0;
    // Clamped as doubles first (1e10 does not fit an S32: the cast would be undefined); what is
    // outside the allowed sizes is still refused below.
    auto pixels = [](double v) { return (S32)std::round(std::clamp(v, -1.0, 100000.0)); };
    if (want_width && want_height)
    {
        width  = pixels(std::trunc(*want_width));
        height = pixels(std::trunc(*want_height));
    }
    else if (want_width)
    {
        width  = pixels(std::trunc(*want_width));
        height = pixels(*want_width * window_height / window_width);
    }
    else if (want_height)
    {
        height = pixels(std::trunc(*want_height));
        width  = pixels(*want_height * window_width / window_height);
    }
    else
    {
        // Long side 1024 (or the window size if smaller), window aspect.
        const F32 scale = llmin(1.f, 1024.f / (F32)llmax(window_width, window_height));
        width           = llmax(1, (S32)llround(window_width * scale));
        height          = llmax(1, (S32)llround(window_height * scale));
    }
    if (width < 64 || height < 64 || width > 2048 || height > 2048)
        throw ToolError("width/height must be between 64 and 2048 pixels");
    const std::string skeleton_arg = argString(args, "skeleton", "none");
    if (skeleton_arg != "none" && skeleton_arg != "body" && skeleton_arg != "hands")
        throw ToolError("skeleton must be none, body or hands");
    const bool want_joints_2d = argBool(args, "joints_2d", skeleton_arg != "none");

    if (settle && isLoggedIn())
    {
        // Let the joints ease into the new pose and the camera finish moving
        // (at least two frames; at most ~4 s, e.g. when another animation
        // with a higher priority holds a joint).
        const bool ready = !gAgentCamera.getCameraAnimating() && posesSettled(poser());
        if (progress.calls < 2 || (!ready && progress.firstCallAge < 4.0))
            return std::nullopt;
    }


    // rawSnapshot() always switches the UI back on when it is done, which
    // would undo the user's "hide UI" (Ctrl+Alt+F1). Restore the old state.
    const bool ui_was_visible = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    const bool taken = gViewerWindow->rawSnapshot(raw, width, height, false /*keep_window_aspect*/, false /*is_texture*/, show_ui,
                                                  false /*show_hud*/, false /*do_rebuild*/, false /*no_post*/, false /*show_balance*/,
                                                  LLSnapshotModel::SNAPSHOT_TYPE_COLOR, 2048);
    if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI) != ui_was_visible)
        LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
    if (!taken)
        throw ToolError("The viewer could not take the snapshot.");
    if (raw->getWidth() != width || raw->getHeight() != height)
        raw->scale(width, height);

    std::optional<json::object> points;
    if ((skeleton_arg != "none" || want_joints_2d) && isLoggedIn())
    {
        LLVOAvatar*      avatar = resolveAvatar(args);
        const CameraShot shot   = currentShot();
        if (skeleton_arg != "none")
            drawSkeleton(raw, avatar, skeleton_arg == "hands", shot);
        if (want_joints_2d)
            points = joints2d(avatar, skeleton_arg == "hands", shot, width, height);
    }

    const std::string bytes = encodePng(raw);
    const std::string path  = saveSnapshotFile(bytes);

    json::object info;
    info["width"]  = width;
    info["height"] = height;
    if (!path.empty())
        info["path"] = path;
    info["settle_seconds"] = rounded(progress.firstCallAge, 2);
    if (points)
    {
        info["joints_2d"] = std::move(*points);
        info["joints_2d_note"] = JOINTS_2D_DOC;
    }

    ToolResult result;
    result.content.push_back(Content::makeImage(bytes, "image/png"));
    result.content.push_back(Content::makeText(json::serialize(info)));
    return result;
}

ToolResult PlusPoserTools::camera(const json::object& args)
{
    if (argBool(args, "reset", false))
    {
        requireLoggedIn();
        gAgentCamera.resetView();
        return ToolResult::json(json::object{ { "camera", "reset to the default view behind your avatar" } });
    }

    LLVOAvatar*       avatar = resolveAvatar(args);
    const std::string view   = argString(args, "view", "front");
    const std::string focus  = argString(args, "focus", "body");

    auto joint_position = [&](const std::string& name) -> std::optional<LLVector3>
    {
        LLJoint* joint = avatar->getJoint(name);
        if (!joint)
            return std::nullopt;
        return joint->getWorldPosition();
    };
    auto midpoint = [&](const char* a, const char* b) -> std::optional<LLVector3>
    {
        auto pa = joint_position(a), pb = joint_position(b);
        if (!pa || !pb)
            return std::nullopt;
        return (*pa + *pb) * 0.5f;
    };

    // Focus point and the half-height of what should fit in the picture.
    std::optional<LLVector3> target;
    F32                      half_extent = 0.3f;
    if (focus == "body")
    {
        auto head = joint_position("mHead"), feet = midpoint("mAnkleLeft", "mAnkleRight");
        if (head && feet)
        {
            target      = (*head + *feet) * 0.5f;
            half_extent = fabsf(head->mV[VZ] - feet->mV[VZ]) * 0.5f + 0.3f;
        }
    }
    else if (focus == "upper_body")
    {
        target      = joint_position("mChest");
        half_extent = 0.55f;
    }
    else if (focus == "lower_body")
    {
        target      = midpoint("mKneeLeft", "mKneeRight");
        half_extent = 0.6f;
    }
    else if (focus == "face" || focus == "head")
    {
        target = joint_position("mHead");
        if (target)
            target->mV[VZ] += 0.07f;
        half_extent = focus == "face" ? 0.16f : 0.25f;
    }
    else if (focus == "hands")
    {
        auto left = joint_position("mWristLeft"), right = joint_position("mWristRight");
        if (left && right)
        {
            target      = (*left + *right) * 0.5f;
            half_extent = (*left - *right).magVec() * 0.5f + 0.2f;
        }
    }
    else if (focus == "left_hand" || focus == "right_hand")
    {
        target      = joint_position(focus == "left_hand" ? "mWristLeft" : "mWristRight");
        half_extent = 0.15f;
    }
    else if (focus == "feet")
    {
        target      = midpoint("mAnkleLeft", "mAnkleRight");
        half_extent = 0.35f;
    }
    else
    {
        FSFloaterPoser* floater = poser();
        const FSPoserAnimator::FSPoserJoint* joint = floater->mPoserAnimator.getPoserJointByName(focus);
        if (!joint)
            throw ToolError("focus must be body, upper_body, lower_body, face, head, hands, left_hand, right_hand, feet or a joint name");
        target      = joint_position(joint->jointName());
        half_extent = 0.3f;
    }
    if (!target)
        throw ToolError("This avatar's skeleton has no joint for focus '" + focus + "'");

    // Directions relative to where the avatar's body (root) faces.
    const LLQuaternion body = avatar->getRenderRotation();
    LLVector3          forward = LLVector3::x_axis * body;
    forward.mV[VZ]             = 0.f;
    if (forward.normalize() < 0.01f)
        forward = LLVector3::x_axis;
    const LLVector3 left = LLVector3::z_axis % forward; // up x forward = left

    LLVector3 horizontal;
    F32       elevation_default = 10.f;
    if (view == "front")
        horizontal = forward;
    else if (view == "back")
        horizontal = -forward;
    else if (view == "left")
        horizontal = left;
    else if (view == "right")
        horizontal = -left;
    else if (view == "front_left")
        horizontal = forward + left;
    else if (view == "front_right")
        horizontal = forward - left;
    else if (view == "back_left")
        horizontal = -forward + left;
    else if (view == "back_right")
        horizontal = -forward - left;
    else if (view == "top")
    {
        horizontal        = forward;
        elevation_default = 85.f;
    }
    else
        throw ToolError("view must be front, back, left, right, front_left, front_right, back_left, back_right or top");
    horizontal.normalize();

    const F32 elevation = llclamp((F32)argNumber(args, "elevation").value_or(elevation_default), -80.f, 89.f) * DEG_TO_RAD;
    const LLVector3 direction = horizontal * cosf(elevation) + LLVector3::z_axis * sinf(elevation);

    F32 distance = 0.f;
    if (auto d = argNumber(args, "distance"))
    {
        distance = (F32)*d;
        if (distance < 0.2f || distance > 30.f)
            throw ToolError("distance must be between 0.2 and 30 meters");
    }
    else
    {
        const F32 fov = llmax(0.2f, LLViewerCamera::getInstance()->getView()); // vertical, radians
        distance      = llclamp(half_extent / tanf(fov * 0.5f) * 1.1f, 0.3f, 30.f);
    }

    const LLVector3 camera_position = *target + direction * distance;

    if (gAgentCamera.cameraMouselook())
        gAgentCamera.changeCameraToDefault();
    gAgentCamera.setFocusOnAvatar(false, false);
    gAgentCamera.setCameraPosAndFocusGlobal(gAgent.getPosGlobalFromAgent(camera_position), gAgent.getPosGlobalFromAgent(*target),
                                            avatar->getID());
    gAgentCamera.stopCameraAnimation(); // jump, so the next snapshot sees the new view

    json::object result;
    result["view"]       = view;
    result["focus"]      = focus;
    result["distance_m"] = rounded(distance, 2);
    result["elevation_deg"] = rounded(elevation * RAD_TO_DEG, 1);
    result["note"] = "Camera stays here until you move it (Esc or camera reset:true returns it behind your avatar).";
    return ToolResult::json(result);
}

namespace
{
    // A plain 5x7 font for the view labels: upper case, digits and a little
    // punctuation (lower case is drawn as upper case, anything else as '?').
    struct Glyph
    {
        char        c;
        const char* rows[7];
    };

    const Glyph FONT[] = {
        { ' ', { ".....", ".....", ".....", ".....", ".....", ".....", "....." } },
        { 'A', { ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
        { 'B', { "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####." } },
        { 'C', { ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###." } },
        { 'D', { "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####." } },
        { 'E', { "#####", "#....", "#....", "####.", "#....", "#....", "#####" } },
        { 'F', { "#####", "#....", "#....", "####.", "#....", "#....", "#...." } },
        { 'G', { ".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####" } },
        { 'H', { "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#" } },
        { 'I', { ".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###." } },
        { 'J', { "..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.." } },
        { 'K', { "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#" } },
        { 'L', { "#....", "#....", "#....", "#....", "#....", "#....", "#####" } },
        { 'M', { "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#" } },
        { 'N', { "#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#" } },
        { 'O', { ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
        { 'P', { "####.", "#...#", "#...#", "####.", "#....", "#....", "#...." } },
        { 'Q', { ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#" } },
        { 'R', { "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#" } },
        { 'S', { ".####", "#....", "#....", ".###.", "....#", "....#", "####." } },
        { 'T', { "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.." } },
        { 'U', { "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###." } },
        { 'V', { "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.." } },
        { 'W', { "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#." } },
        { 'X', { "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#" } },
        { 'Y', { "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.." } },
        { 'Z', { "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####" } },
        { '0', { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." } },
        { '1', { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." } },
        { '2', { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" } },
        { '3', { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." } },
        { '4', { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." } },
        { '5', { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." } },
        { '6', { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." } },
        { '7', { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." } },
        { '8', { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." } },
        { '9', { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." } },
        { '-', { ".....", ".....", ".....", "#####", ".....", ".....", "....." } },
        { '_', { ".....", ".....", ".....", ".....", ".....", ".....", "#####" } },
        { '.', { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." } },
        { ',', { ".....", ".....", ".....", ".....", ".##..", "..#..", ".#..." } },
        { ':', { ".....", ".##..", ".##..", ".....", ".##..", ".##..", "....." } },
        { '+', { ".....", "..#..", "..#..", "#####", "..#..", "..#..", "....." } },
        { '=', { ".....", ".....", "#####", ".....", "#####", ".....", "....." } },
        { '/', { ".....", "....#", "...#.", "..#..", ".#...", "#....", "....." } },
        { '(', { "...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#." } },
        { ')', { ".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..." } },
        { '?', { ".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.." } },
    };

    const Glyph& glyphFor(char c)
    {
        const char upper = (char)std::toupper(static_cast<unsigned char>(c));
        for (const Glyph& glyph : FONT)
        {
            if (glyph.c == upper)
                return glyph;
        }
        return FONT[sizeof(FONT) / sizeof(FONT[0]) - 1]; // '?'
    }

    // Plain drawing into a snapshot (rows bottom-up, like the GL read-back).
    struct Canvas
    {
        LLImageRaw* raw;

        // A label with its top left corner at (x, y), y counted from the TOP of the
        // image, on a black box. Characters are 5x7 cells scaled by `scale`.
        void label(S32 x, S32 y, const std::string& text, S32 scale, const LLColor4U& color) const
        {
            const LLColor4U box(0, 0, 0, 255);
            const S32       pad   = scale;
            const S32       box_w = (S32)text.size() * 6 * scale - scale + 2 * pad;
            const S32       box_h = 7 * scale + 2 * pad;
            const S32       top   = raw->getHeight() - 1 - y; // raw row of the top edge
            for (S32 dy = 0; dy < box_h; ++dy)
                for (S32 dx = 0; dx < box_w; ++dx)
                    dot(x + dx, top - dy, box);
            for (size_t i = 0; i < text.size(); ++i)
            {
                const Glyph& glyph = glyphFor(text[i]);
                const S32    left  = x + pad + (S32)i * 6 * scale;
                for (S32 row = 0; row < 7; ++row)
                {
                    for (S32 col = 0; col < 5; ++col)
                    {
                        if (glyph.rows[row][col] != '#')
                            continue;
                        for (S32 sy = 0; sy < scale; ++sy)
                            for (S32 sx = 0; sx < scale; ++sx)
                                dot(left + col * scale + sx, top - pad - row * scale - sy, color);
                    }
                }
            }
        }

        void dot(S32 x, S32 y, const LLColor4U& color) const
        {
            if (x < 0 || y < 0 || x >= raw->getWidth() || y >= raw->getHeight())
                return;
            const S32 components = raw->getComponents();
            U8*       pixel      = raw->getData() + ((size_t)y * raw->getWidth() + x) * components;
            for (S32 i = 0; i < llmin(components, 3); ++i)
                pixel[i] = color.mV[i];
        }

        void disc(F32 cx, F32 cy, F32 radius, const LLColor4U& color) const
        {
            const S32 r = (S32)ceilf(radius);
            // fix-round53 5: a joint just in front of the camera projects far outside, past
            // what S32 holds. Brought to just outside the image (no dot of it falls inside).
            if (!std::isfinite(cx) || !std::isfinite(cy))
                return;
            const S32 x = (S32)llround(llclamp(cx, -(F32)(r + 1), (F32)(raw->getWidth() + r + 1)));
            const S32 y = (S32)llround(llclamp(cy, -(F32)(r + 1), (F32)(raw->getHeight() + r + 1)));
            for (S32 dy = -r; dy <= r; ++dy)
            {
                for (S32 dx = -r; dx <= r; ++dx)
                {
                    if (dx * dx + dy * dy <= radius * radius)
                        dot(x + dx, y + dy, color);
                }
            }
        }

        void line(const LLVector2& a, const LLVector2& b, F32 radius, const LLColor4U& color) const
        {
            const F32 length = (b - a).length();
            const S32 steps  = llclamp((S32)ceilf(length), 1, 8192);
            for (S32 i = 0; i <= steps; ++i)
            {
                const LLVector2 p = a + (b - a) * ((F32)i / (F32)steps);
                disc(p.mV[VX], p.mV[VY], radius, color);
            }
        }
    };

    // Image coordinates (x right, y UP: LLImageRaw rows) of a point (agent frame)
    // in a snapshot rendered with this camera. rawSnapshot takes the centered crop
    // of the view with the image's aspect (scaled), so the vertical field of view
    // is that of the camera when the image is not wider than the window, else the
    // horizontal one is.
    std::optional<LLVector2> projectToImage(const CameraShot& shot, const LLVector3& point, S32 width, S32 height)
    {
        const LLVector3 v     = point - shot.origin;
        const F32       depth = v * shot.at;
        if (depth < 0.05f)
            return std::nullopt; // behind the camera
        const F32 tan_half = tanf(shot.view * 0.5f);
        const F32 x        = -(v * shot.left) / (depth * tan_half); // in units of the vertical half-view
        const F32 y        = (v * shot.up) / (depth * tan_half);

        const F32 window_aspect = llmax(0.01f, shot.aspect);
        const F32 image_aspect  = (F32)width / (F32)height;
        F32       x_span = image_aspect, y_span = 1.f;
        if (image_aspect > window_aspect)
        {
            x_span = window_aspect;
            y_span = window_aspect / image_aspect;
        }
        return LLVector2((x / x_span + 1.f) * 0.5f * width, (y / y_span + 1.f) * 0.5f * height);
    }

    // The bones the skeleton overlay draws (and whose joints joints_2d reports).
    std::vector<std::pair<std::string, std::string>> skeletonBones(bool hands)
    {
        std::vector<std::pair<std::string, std::string>> bones{
            { "mPelvis", "mTorso" }, { "mTorso", "mChest" }, { "mChest", "mNeck" }, { "mNeck", "mHead" },
        };
        for (const char* side : { "Left", "Right" })
        {
            const std::string s(side);
            const std::vector<std::string> arm{ "mChest", "mCollar" + s, "mShoulder" + s, "mElbow" + s, "mWrist" + s };
            const std::vector<std::string> leg{ "mPelvis", "mHip" + s, "mKnee" + s, "mAnkle" + s, "mFoot" + s, "mToe" + s };
            for (size_t i = 1; i < arm.size(); ++i)
                bones.emplace_back(arm[i - 1], arm[i]);
            for (size_t i = 1; i < leg.size(); ++i)
                bones.emplace_back(leg[i - 1], leg[i]);
            if (hands)
            {
                for (const char* finger : { "Thumb", "Index", "Middle", "Ring", "Pinky" })
                {
                    std::string previous = "mWrist" + s;
                    for (int segment = 1; segment <= 3; ++segment)
                    {
                        const std::string joint = std::string("mHand") + finger + std::to_string(segment) + s;
                        bones.emplace_back(previous, joint);
                        previous = joint;
                    }
                }
            }
            else
            {
                bones.emplace_back("mWrist" + s, "mHandMiddle1" + s);
            }
        }
        return bones;
    }

    std::vector<std::string> skeletonPoints(bool hands)
    {
        std::vector<std::string> points;
        for (const auto& [from, to] : skeletonBones(hands))
        {
            points.push_back(from);
            points.push_back(to);
        }
        std::sort(points.begin(), points.end());
        points.erase(std::unique(points.begin(), points.end()), points.end());
        return points;
    }
}

json::object PlusPoserTools::joints2d(LLVOAvatar* avatar, bool hands, const CameraShot& shot, S32 width, S32 height)
{
    json::object points;
    for (const std::string& name : skeletonPoints(hands))
    {
        LLJoint* joint = avatar->getJoint(name);
        if (!joint)
            continue;
        const std::optional<LLVector2> p = projectToImage(shot, joint->getWorldPosition(), width, height);
        if (!p || p->mV[VX] < 0.f || p->mV[VY] < 0.f || p->mV[VX] > (F32)width || p->mV[VY] > (F32)height)
            continue;
        points[name] = json::array{ rounded(p->mV[VX], 1), rounded((F32)height - p->mV[VY], 1) }; // y down
    }
    return points;
}

void PlusPoserTools::drawSkeleton(LLImageRaw* raw, LLVOAvatar* avatar, bool hands, const CameraShot& shot)
{
    if (!raw || !avatar || raw->getComponents() < 3)
        return;

    // Left side blue, right side red, spine yellow (the avatar's own left/right).
    const LLColor4U left(40, 170, 255, 255), right(255, 70, 70, 255), center(255, 220, 0, 255), outline(0, 0, 0, 255);
    const LLColor4U white(255, 255, 255, 255);

    const std::vector<std::pair<std::string, std::string>> bones = skeletonBones(hands);
    const S32 width  = raw->getWidth();
    const S32 height = raw->getHeight();
    const F32 scale  = llmax(1.f, llmin(width, height) / 512.f);
    auto      where  = [&](const std::string& name) -> std::optional<LLVector2>
    {
        LLJoint* joint = avatar->getJoint(name);
        return joint ? projectToImage(shot, joint->getWorldPosition(), width, height) : std::nullopt;
    };
    auto color_of = [&](const std::string& name)
    { return boost::icontains(name, "Left") ? left : boost::icontains(name, "Right") ? right : center; };

    Canvas canvas{ raw };
    for (int pass = 0; pass < 2; ++pass) // black outline first, then the colors
    {
        for (const auto& [from, to] : bones)
        {
            auto a = where(from), b = where(to);
            if (!a || !b)
                continue;
            const bool fine = boost::icontains(to, "mHand");
            const F32  r    = (fine ? 0.6f : 1.2f) * scale;
            canvas.line(*a, *b, pass == 0 ? r + 1.f : r, pass == 0 ? outline : color_of(to));
        }
    }
    for (const std::string& name : skeletonPoints(hands))
    {
        if (auto p = where(name))
        {
            const F32 r = (boost::icontains(name, "mHand") ? 1.3f : 2.5f) * scale;
            canvas.disc(p->mV[VX], p->mV[VY], r + 1.f, outline);
            canvas.disc(p->mV[VX], p->mV[VY], r, boost::iequals(name, "mHead") ? white : color_of(name));
        }
    }
}

// ---------------------------------------------------------------------------
// snapshot with views: several cameras rendered off-screen in one main-thread
// step, the user's camera and screen untouched.

namespace
{
    constexpr size_t MAX_VIEWS     = 9;
    constexpr S32    FRAME_MIN     = 128;
    constexpr S32    FRAME_MAX     = 1024;
    constexpr S32    COMPOSITE_MAX = 4096;
    constexpr S32    FRAME_GAP     = 4;

    struct ViewPreset
    {
        const char* name;
        F32         azimuth;   // degrees around the avatar from its front, + toward its left
        F32         elevation; // degrees above the horizontal
    };

    const ViewPreset VIEW_PRESETS[] = {
        { "front", 0.f, 10.f },  { "front_left", 45.f, 10.f },   { "left", 90.f, 10.f },
        { "back_left", 135.f, 10.f }, { "back", 180.f, 10.f },  { "back_right", -135.f, 10.f },
        { "right", -90.f, 10.f }, { "front_right", -45.f, 10.f }, { "top", 0.f, 85.f },
    };

    const ViewPreset* findPreset(const std::string& name)
    {
        for (const ViewPreset& preset : VIEW_PRESETS)
        {
            if (name == preset.name)
                return &preset;
        }
        return nullptr;
    }

    struct ViewSpec
    {
        std::string        name;
        F32                azimuth   = 0.f;
        F32                elevation = 10.f;
        std::optional<F32> distance;
        F32                fov = 60.f; // vertical, degrees
        std::string        focus;
    };

    // Restores everything about the viewer camera and the render state that the
    // off-screen views change, also when a view throws. The user's agent camera
    // (gAgentCamera) is never touched; the next frame puts the viewer camera where
    // it says anyway, but the frame being prepared must not see the views' camera.
    class CameraRestorer
    {
    public:
        CameraRestorer()
            : mCamera(*LLViewerCamera::getInstance()),
              mZoom(LLViewerCamera::getInstance()->getZoomFactor()),
              mSubregion(LLViewerCamera::getInstance()->getZoomSubRegion()),
              mModelview(get_current_modelview()),
              mProjection(get_current_projection()),
              mUiVisible(gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI))
        {
        }

        ~CameraRestorer()
        {
            LLViewerCamera* camera = LLViewerCamera::getInstance();
            static_cast<LLCamera&>(*camera) = mCamera; // position, axes, field of view, planes
            camera->setZoomParameters(mZoom, mSubregion);
            if (gViewerWindow)
            {
                // rawSnapshot leaves the world view at the whole window (UI hidden)
                // until the next layout pass; keep the camera's shape matching it,
                // as after a plain snapshot.
                camera->setViewHeightInPixels(gViewerWindow->getWorldViewHeightRaw());
                camera->setAspect(gViewerWindow->getWorldViewAspectRatio());
            }
            set_current_modelview(mModelview);
            set_current_projection(mProjection);
            // rawSnapshot always switches the UI back on (it may have been hidden, Ctrl+Alt+F1).
            if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI) != mUiVisible)
                LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }

        CameraRestorer(const CameraRestorer&)            = delete;
        CameraRestorer& operator=(const CameraRestorer&) = delete;

    private:
        LLCamera  mCamera;
        F32       mZoom;
        S16       mSubregion;
        glm::mat4 mModelview;
        glm::mat4 mProjection;
        bool      mUiVisible;
    };

    // Copies a frame (rows bottom-up) into the composite with its top left corner
    // at (x, y), y counted from the top.
    void blit(LLImageRaw* dst, const LLImageRaw* src, S32 x, S32 y)
    {
        const S32 w = src->getWidth(), h = src->getHeight(), c = dst->getComponents();
        if (src->getComponents() != c || x < 0 || y < 0 || x + w > dst->getWidth() || y + h > dst->getHeight())
            throw ToolError("Internal error while composing the views.");
        for (S32 row = 0; row < h; ++row)
        {
            const S32 dst_row = dst->getHeight() - y - h + row;
            memcpy(dst->getData() + ((size_t)dst_row * dst->getWidth() + x) * c, src->getData() + (size_t)row * w * c,
                   (size_t)w * c);
        }
    }

    std::string upperCase(std::string text)
    {
        for (char& c : text)
            c = (char)std::toupper(static_cast<unsigned char>(c));
        return text;
    }
}

std::optional<ToolResult> PlusPoserTools::snapshotViews(const json::object& args, const JobProgress& progress)
{
    // Everything is validated on every call (cheap) so that errors come before
    // the wait for the pose to settle.
    LLVOAvatar*     avatar  = resolveAvatar(args);
    FSFloaterPoser* floater = poser();
    if (argBool(args, "show_ui", false))
        throw ToolError("show_ui does not work with views: they are rendered off-screen without the UI.");
    if (avatar->isSelf() && gAgentCamera.cameraMouselook())
        throw ToolError("Your avatar is not drawn in mouselook; leave mouselook (Esc) for views.");

    const bool        settle       = argBool(args, "settle", true);
    const std::string skeleton_arg = argString(args, "skeleton", "none");
    if (skeleton_arg != "none" && skeleton_arg != "body" && skeleton_arg != "hands")
        throw ToolError("skeleton must be none, body or hands");
    const bool want_joints_2d = argBool(args, "joints_2d", skeleton_arg != "none");
    const bool hands          = skeleton_arg == "hands";

    const std::string default_focus = argString(args, "focus", "body");
    F32               default_fov   = llclamp(LLViewerCamera::getInstance()->getView() * RAD_TO_DEG, 10.f, 120.f);
    if (auto fov = argNumber(args, "fov"))
    {
        if (*fov < 10.0 || *fov > 120.0)
            throw ToolError("fov must be between 10 and 120 degrees");
        default_fov = (F32)*fov;
    }

    const json::value& raw_views = args.at("views");
    if (!raw_views.is_array() || raw_views.get_array().empty() || raw_views.get_array().size() > MAX_VIEWS)
        throw ToolError("'views' must be an array of 1-9 view names or {azimuth, elevation, ...} objects");

    std::vector<ViewSpec> specs;
    for (const json::value& item : raw_views.get_array())
    {
        ViewSpec spec;
        spec.focus = default_focus;
        spec.fov   = default_fov;
        if (item.is_string())
        {
            spec.name                 = std::string(item.get_string());
            const ViewPreset* preset  = findPreset(spec.name);
            if (!preset)
                throw ToolError("unknown view '" + spec.name + "' (front, back, left, right, front_left, front_right, "
                                "back_left, back_right, top, or an object with azimuth/elevation)");
            spec.azimuth   = preset->azimuth;
            spec.elevation = preset->elevation;
        }
        else if (item.is_object())
        {
            const json::object& o = item.get_object();
            for (const auto& kv : o)
            {
                static const std::vector<std::string> keys{ "name", "view", "azimuth", "elevation", "distance", "fov", "focus" };
                if (std::find(keys.begin(), keys.end(), std::string(kv.key())) == keys.end())
                    throw ToolError("unknown field '" + std::string(kv.key()) + "' in views");
            }
            const std::string view   = argString(o, "view", "front");
            const ViewPreset* preset = findPreset(view);
            if (!preset)
                throw ToolError("unknown view '" + view + "'");
            spec.azimuth   = preset->azimuth;
            spec.elevation = preset->elevation;
            if (auto azimuth = argNumber(o, "azimuth"))
                spec.azimuth = wrapDegrees((F32)*azimuth);
            if (auto elevation = argNumber(o, "elevation"))
            {
                if (*elevation < -80.0 || *elevation > 89.0)
                    throw ToolError("elevation must be between -80 and 89 degrees");
                spec.elevation = (F32)*elevation;
            }
            if (auto distance = argNumber(o, "distance"))
            {
                if (*distance < 0.2 || *distance > 30.0)
                    throw ToolError("distance must be between 0.2 and 30 meters");
                spec.distance = (F32)*distance;
            }
            if (auto fov = argNumber(o, "fov"))
            {
                if (*fov < 10.0 || *fov > 120.0)
                    throw ToolError("fov must be between 10 and 120 degrees");
                spec.fov = (F32)*fov;
            }
            spec.focus = argString(o, "focus", default_focus);
            const bool custom_angle = o.contains("azimuth") || o.contains("elevation");
            spec.name  = argString(o, "name", custom_angle ? llformat("az%d el%d", (S32)llround(spec.azimuth),
                                                                      (S32)llround(spec.elevation))
                                                           : view);
            if (spec.name.empty() || spec.name.size() > 32)
                throw ToolError("a view name must be 1-32 characters");
        }
        else
        {
            throw ToolError("each view must be a name or an object");
        }
        specs.push_back(spec);
    }

    // Frame size, layout and the size of the whole picture.
    const std::string layout = argString(args, "layout", "auto");
    if (layout != "auto" && layout != "row" && layout != "grid" && layout != "separate")
        throw ToolError("layout must be auto, row, grid or separate");
    std::optional<double> want_width  = argNumber(args, "width");
    std::optional<double> want_height = argNumber(args, "height");
    const double          view_w      = want_width.value_or(want_height.value_or(512.0)); // one given: square
    const double          view_h      = want_height.value_or(want_width.value_or(512.0));
    if (view_w < FRAME_MIN || view_h < FRAME_MIN || view_w > FRAME_MAX || view_h > FRAME_MAX)
        throw ToolError("with views, width/height are the size of each view: 128-1024 pixels");
    const S32 frame_w = (S32)view_w;
    const S32 frame_h = (S32)view_h;
    const S32 n    = (S32)specs.size();
    S32       cols = n, rows = 1;
    if (layout == "grid" || (layout == "auto" && n > 3))
    {
        cols = (S32)ceilf(sqrtf((F32)n));
        rows = (n + cols - 1) / cols;
    }
    const bool separate    = layout == "separate";
    const S32  composite_w = cols * frame_w + (cols - 1) * FRAME_GAP;
    const S32  composite_h = rows * frame_h + (rows - 1) * FRAME_GAP;
    if (!separate && (composite_w > COMPOSITE_MAX || composite_h > COMPOSITE_MAX))
        throw ToolError(llformat("the views would make a %dx%d picture (at most %d); use smaller width/height, layout grid "
                                 "or separate", composite_w, composite_h, COMPOSITE_MAX));

    // What each focus frames: the points that must fit, and a margin (m).
    auto framing = [&](const std::string& focus, std::vector<LLVector3>& points, F32& margin)
    {
        points.clear();
        margin   = 0.f;
        auto add = [&](const std::string& name)
        {
            if (LLJoint* joint = avatar->getJoint(name))
                points.push_back(joint->getWorldPosition());
        };
        auto add_hand = [&](const std::string& side)
        {
            add("mWrist" + side);
            for (const char* finger : { "Thumb", "Index", "Middle", "Ring", "Pinky" })
                for (int segment = 1; segment <= 3; ++segment)
                    add(std::string("mHand") + finger + std::to_string(segment) + side);
        };
        auto add_cube = [&](const LLVector3& center, F32 half)
        {
            for (int i = 0; i < 8; ++i)
                points.push_back(center + LLVector3(i & 1 ? half : -half, i & 2 ? half : -half, i & 4 ? half : -half));
        };
        auto head_top = [&]()
        {
            if (LLJoint* skull = avatar->getJoint("mSkull"))
                points.push_back(skull->getWorldPosition() + LLVector3::z_axis * 0.06f);
        };

        if (focus == "body")
        {
            for (const std::string& name : skeletonPoints(false))
                add(name);
            head_top();
            margin = 0.1f;
        }
        else if (focus == "upper_body")
        {
            for (const char* name : { "mTorso", "mChest", "mNeck", "mHead" })
                add(name);
            for (const char* side : { "Left", "Right" })
                for (const char* joint : { "mCollar", "mShoulder", "mElbow", "mWrist", "mHandMiddle1" })
                    add(std::string(joint) + side);
            head_top();
            margin = 0.08f;
        }
        else if (focus == "lower_body")
        {
            add("mPelvis");
            for (const char* side : { "Left", "Right" })
                for (const char* joint : { "mHip", "mKnee", "mAnkle", "mFoot", "mToe" })
                    add(std::string(joint) + side);
            margin = 0.08f;
        }
        else if (focus == "face" || focus == "head")
        {
            if (LLJoint* head = avatar->getJoint("mHead"))
                add_cube(head->getWorldPosition() + LLVector3::z_axis * 0.07f, focus == "face" ? 0.13f : 0.2f);
        }
        else if (focus == "hands")
        {
            add_hand("Left");
            add_hand("Right");
            margin = 0.05f;
        }
        else if (focus == "left_hand" || focus == "right_hand")
        {
            add_hand(focus == "left_hand" ? "Left" : "Right");
            margin = 0.05f;
        }
        else if (focus == "feet")
        {
            for (const char* side : { "Left", "Right" })
                for (const char* joint : { "mAnkle", "mFoot", "mToe" })
                    add(std::string(joint) + side);
            margin = 0.08f;
        }
        else
        {
            const FSPoserAnimator::FSPoserJoint* joint = floater->mPoserAnimator.getPoserJointByName(focus);
            if (!joint)
                throw ToolError("focus must be body, upper_body, lower_body, face, head, hands, left_hand, right_hand, "
                                "feet or a joint name");
            if (LLJoint* ll_joint = avatar->getJoint(joint->jointName()))
                add_cube(ll_joint->getWorldPosition(), 0.25f);
        }
        if (points.empty())
            throw ToolError("This avatar's skeleton has no joint for focus '" + focus + "'");
    };
    {
        std::vector<LLVector3> points;
        F32                    margin = 0.f;
        for (const ViewSpec& spec : specs)
            framing(spec.focus, points, margin);
    }

    if (settle)
    {
        // At least two frames; at most ~4 s (a higher-priority animation can hold a
        // joint). The user's camera does not matter here.
        if (progress.calls < 2 || (!posesSettled(floater) && progress.firstCallAge < 4.0))
            return std::nullopt;
    }

    // Directions relative to where the avatar's body (root) faces, as camera does.
    const LLQuaternion body    = avatar->getRenderRotation();
    LLVector3          forward = LLVector3::x_axis * body;
    forward.mV[VZ]             = 0.f;
    if (forward.normalize() < 0.01f)
        forward = LLVector3::x_axis;
    const LLVector3 avatar_left = LLVector3::z_axis % forward;

    // The views render the whole window (no UI) and keep its centered crop.
    const F32 window_aspect = (F32)llmax(1, gViewerWindow->getWindowWidthRaw()) / (F32)llmax(1, gViewerWindow->getWindowHeightRaw());
    const F32 frame_aspect  = (F32)frame_w / (F32)frame_h;
    const BodyFrame frame   = bodyFrame(avatar);
    const S32 label_scale   = frame_w >= 384 ? 2 : 1;

    std::vector<LLPointer<LLImageRaw>> images;
    json::array                        metas;
    {
        CameraRestorer restore;
        for (const ViewSpec& spec : specs)
        {
            std::vector<LLVector3> points;
            F32                    margin = 0.f;
            framing(spec.focus, points, margin);
            LLVector3 low = points.front(), high = points.front();
            for (const LLVector3& p : points)
            {
                for (int i = 0; i < 3; ++i)
                {
                    low.mV[i]  = llmin(low.mV[i], p.mV[i]);
                    high.mV[i] = llmax(high.mV[i], p.mV[i]);
                }
            }
            const LLVector3 center = (low + high) * 0.5f;

            const F32 azimuth   = spec.azimuth * DEG_TO_RAD;
            const F32 elevation = spec.elevation * DEG_TO_RAD;
            LLVector3 horizontal = forward * cosf(azimuth) + avatar_left * sinf(azimuth);
            horizontal.normalize();
            const LLVector3 toward_camera = horizontal * cosf(elevation) + LLVector3::z_axis * sinf(elevation);
            const LLVector3 at            = -toward_camera;
            LLVector3       left_axis     = LLVector3::z_axis % (-horizontal); // camera's left, level
            left_axis.normalize();
            LLVector3 up_axis = at % left_axis;
            up_axis.normalize();

            // Vertical field of view of the frame = spec.fov. A frame wider than the
            // window keeps the window's horizontal view instead, so widen the camera.
            const F32 tan_v    = tanf(spec.fov * DEG_TO_RAD * 0.5f);
            const F32 tan_h    = tan_v * frame_aspect;
            const F32 cam_view = frame_aspect > window_aspect ? 2.f * atanf(tan_v * frame_aspect / window_aspect)
                                                              : spec.fov * DEG_TO_RAD;

            F32 distance = 0.f;
            if (spec.distance)
            {
                distance = *spec.distance;
            }
            else
            {
                // Fit every point (and its margin) into the frame both ways.
                F32 need = 0.3f;
                for (const LLVector3& p : points)
                {
                    const LLVector3 rel   = p - center;
                    const F32       along = rel * at; // > 0: beyond the center, seen from the camera
                    const F32       side  = fabsf(rel * left_axis) + margin;
                    const F32       upward = fabsf(rel * up_axis) + margin;
                    need = llmax(need, llmax(side / tan_h - along, upward / tan_v - along));
                    need = llmax(need, 0.15f - along);
                }
                distance = llclamp(need * 1.03f, 0.3f, 30.f);
            }
            const LLVector3 origin = center + toward_camera * distance;

            LLViewerCamera* camera = LLViewerCamera::getInstance();
            camera->setOrigin(origin);
            camera->setAxes(at, left_axis, up_axis);
            camera->setViewNoBroadcast(cam_view);

            LLPointer<LLImageRaw> raw = new LLImageRaw;
            if (!gViewerWindow->rawSnapshot(raw, frame_w, frame_h, false /*keep_window_aspect*/, false /*is_texture*/,
                                            false /*show_ui*/, false /*show_hud*/, false /*do_rebuild*/, false /*no_post*/,
                                            false /*show_balance*/, LLSnapshotModel::SNAPSHOT_TYPE_COLOR, 2048))
                throw ToolError("The viewer could not take the snapshot of view '" + spec.name + "'.");
            if (raw->getWidth() != frame_w || raw->getHeight() != frame_h)
                raw->scale(frame_w, frame_h);
            if (raw->getComponents() != 3)
                throw ToolError("The snapshot of view '" + spec.name + "' has an unexpected format.");

            const CameraShot shot = currentShot();
            json::object     meta;
            meta["name"]          = spec.name;
            meta["azimuth_deg"]   = rounded(spec.azimuth, 1);
            meta["elevation_deg"] = rounded(spec.elevation, 1);
            meta["distance_m"]    = rounded(distance, 3);
            meta["fov_deg"]       = rounded(spec.fov, 1);
            meta["focus"]         = spec.focus;
            meta["camera"]        = vecJson(frame.pointToBody(origin), 3);
            meta["look_at"]       = vecJson(frame.pointToBody(center), 3);
            meta["size"]          = json::array{ frame_w, frame_h };
            if (skeleton_arg != "none")
                drawSkeleton(raw, avatar, hands, shot);
            if (want_joints_2d)
                meta["joints_2d"] = joints2d(avatar, hands, shot, frame_w, frame_h);
            Canvas{ raw }.label(4 * label_scale, 4 * label_scale, upperCase(spec.name), label_scale, LLColor4U(255, 255, 255, 255));

            images.push_back(raw);
            metas.push_back(std::move(meta));
        }
    } // the user's camera is back here

    ToolResult   result;
    json::object info;
    json::array  paths;
    if (separate)
    {
        for (size_t i = 0; i < images.size(); ++i)
        {
            const std::string bytes = encodePng(images[i]);
            const std::string path  = saveSnapshotFile(bytes);
            paths.push_back(json::string(path)); // "" where saving failed: paths[i] stays image i
            metas[i].as_object()["image"] = (S32)i;
            result.content.push_back(Content::makeImage(bytes, "image/png"));
        }
    }
    else
    {
        LLPointer<LLImageRaw> composite = new LLImageRaw((U16)composite_w, (U16)composite_h, 3);
        if (!composite->getData())
            throw ToolError("Not enough memory for the views picture.");
        composite->clear(24, 24, 24);
        for (size_t i = 0; i < images.size(); ++i)
        {
            const S32 x = ((S32)i % cols) * (frame_w + FRAME_GAP);
            const S32 y = ((S32)i / cols) * (frame_h + FRAME_GAP);
            blit(composite, images[i], x, y);
            metas[i].as_object()["offset"] = json::array{ x, y };
        }
        const std::string bytes = encodePng(composite);
        const std::string path  = saveSnapshotFile(bytes);
        if (!path.empty())
            paths.push_back(json::string(path));
        info["image_size"] = json::array{ composite_w, composite_h };
        result.content.push_back(Content::makeImage(bytes, "image/png"));
    }

    info["layout"]         = separate ? "separate" : (rows == 1 ? "row" : "grid");
    info["frame_size"]     = json::array{ frame_w, frame_h };
    info["views"]          = std::move(metas);
    info["paths"]          = std::move(paths);
    info["settle_seconds"] = rounded(progress.firstCallAge, 2);
    info["frames"] = "camera / look_at are in the avatar's body frame (as joint_positions); offset is where the view's "
                     "top left corner is in the combined picture. The views were rendered off-screen: your camera did not move.";
    if (want_joints_2d)
        info["joints_2d_note"] = JOINTS_2D_DOC;
    result.content.push_back(Content::makeText(json::serialize(info)));
    return result;
}

// ---------------------------------------------------------------------------
// Tool table: snapshot, camera

namespace plus::posermcp
{
    void appendSnapshotTools(std::vector<Tool>& tools, const MainThreadRunner& run)
    {
        const json::object avatar_prop = prop("string", AVATAR_ARG_DOC);

        {
            Tool t;
            t.name        = "snapshot";
            t.title       = "Snapshot of the world view";
            t.description =
                "Renders the current view (without UI and HUD by default) and returns it as a PNG image, plus its size and "
                "the file it was also saved to. By default it first waits until the joints have eased into the latest pose "
                "and the camera stopped (up to ~4 s). Default size: long side 1024 px with the window's aspect; give width "
                "and/or height (64-2048) to change it. Use camera first to frame the avatar, then inspect the image and "
                "adjust with set_joints. skeleton=\"body\" overlays the bones (left blue, right red), useful in dark scenes, "
                "under clothes, and to tell left from right.\n"
                "VIEWS (preferred for checking a pose): views=[\"front\",\"left\",\"back\"] renders the avatar from several "
                "sides in one call, OFF-SCREEN: the user's camera and screen do not move (no need for camera). Each view "
                "is a name (front, back, left, right, front_left, front_right, back_left, back_right, top; left = the "
                "avatar's left side) or {\"azimuth\": deg around the avatar from its front, + toward its left, "
                "\"elevation\": deg above horizontal (default 10), \"distance\": m (default: fit the focus), \"fov\": "
                "vertical deg, \"focus\": as in camera, \"view\": a name to start from, \"name\": label}. With views, "
                "width/height are the size of each view (default 512x512, 128-1024), and layout auto (default: a row for "
                "up to 3 views, else a grid) / row / grid makes one picture with a label in each view, separate returns "
                "one image per view. Keep the whole picture within about 1500 px for your own eyes (models see larger "
                "images scaled down). The text part lists each view's camera. joints_2d (default on with skeleton) adds "
                "the pixel position of the main joints in each view, for comparing with keypoints from a photo.";
            t.readOnly    = true;
            json::object p;
            p["width"]    = prop("integer", "Image width in pixels (64-2048); with views: each view's width (128-1024).");
            p["height"]   = prop("integer", "Image height in pixels (64-2048); with views: each view's height (128-1024).");
            p["show_ui"]  = prop("boolean", "Include the viewer UI (limited to the window size). Default false. Not with views. "
                                            "Refused unless the user allowed it in the viewer (the UI can show IMs and chat).");
            p["settle"]   = prop("boolean", "Wait for the pose (and, without views, the camera) to settle first. Default true.");
            p["skeleton"] = enumProp({ "none", "body", "hands" },
                                     "Draw the skeleton over the image: body = spine, arms, legs; hands = also every "
                                     "finger. Blue = the avatar's LEFT side, red = its RIGHT side, yellow = spine, white "
                                     "dot = head. Lines are drawn on top even where the body hides them. Default none.");
            p["joints_2d"] = prop("boolean", "Also return the pixel position [x, y] (origin top left) of the joints the "
                                             "skeleton overlay uses (hands: with fingers). Default: on when skeleton is not none.");
            json::object view_props;
            view_props["name"]      = prop("string", "Label (1-32 characters). Default: the view name, or az/el.");
            view_props["view"]      = enumProp({ "front", "back", "left", "right", "front_left", "front_right", "back_left",
                                                 "back_right", "top" },
                                               "Start from this direction. Default front.");
            view_props["azimuth"]   = prop("number", "Degrees around the avatar from its front, + toward the avatar's left (90 = left side, 180 = back).");
            view_props["elevation"] = prop("number", "Degrees above the horizontal (-80..89). Default 10 (top: 85).");
            view_props["distance"]  = prop("number", "Meters from the focus (0.2-30). Default: fit the focus.");
            view_props["fov"]       = prop("number", "Vertical field of view in degrees (10-120). Default: the snapshot's fov.");
            view_props["focus"]     = prop("string", "Like camera: body, upper_body, lower_body, face, head, hands, left_hand, "
                                                     "right_hand, feet or a joint name. Default: the snapshot's focus.");
            json::array view_kinds;
            view_kinds.push_back(enumProp({ "front", "back", "left", "right", "front_left", "front_right", "back_left", "back_right",
                                            "top" },
                                          "A named view."));
            view_kinds.push_back(objectSchema(std::move(view_props)));
            p["views"]  = json::object{ { "type", "array" },
                                        { "items", json::object{ { "anyOf", std::move(view_kinds) } } },
                                        { "minItems", 1 },
                                        { "maxItems", (int)MAX_VIEWS },
                                        { "description", "Render these views off-screen instead of the current view." } };
            p["layout"] = enumProp({ "auto", "row", "grid", "separate" }, "With views: how to return them. Default auto.");
            p["focus"]  = prop("string", "With views: default focus of every view. Default body.");
            p["fov"]    = prop("number", "With views: default vertical field of view in degrees (10-120). Default: the "
                                         "current camera's.");
            p["avatar"] = avatar_prop;
            t.inputSchema = objectSchema(std::move(p));
            t.handler     = [run](const json::object& args) -> ToolResult
            {
                return run([args](const JobProgress& progress) { return PlusPoserTools::snapshot(args, progress); }, 45.0);
            };
            tools.push_back(std::move(t));
        }
        {
            Tool t;
            t.name        = "camera";
            t.title       = "Point the camera at an avatar";
            t.description = "Moves the camera to look at an avatar from a side relative to where its body faces: front, "
                            "back, left (the avatar's left side), right, front_left, front_right, back_left, back_right, "
                            "top. focus frames a part: body (default, whole avatar), upper_body, lower_body, face, head, "
                            "hands, left_hand, right_hand, feet, or any joint name. distance (m) is chosen to fit the "
                            "part unless given; elevation is the camera's angle above the horizontal (default 10 deg). "
                            "reset=true puts the camera back behind your avatar. Follow with snapshot.";
            json::object p;
            p["avatar"]    = avatar_prop;
            p["view"]      = enumProp({ "front", "back", "left", "right", "front_left", "front_right", "back_left", "back_right", "top" },
                                      "Default front.");
            p["focus"]     = prop("string", "body, upper_body, lower_body, face, head, hands, left_hand, right_hand, feet or a joint name.");
            p["distance"]  = prop("number", "Meters from the focus point (0.2-30). Default: fit the part.");
            p["elevation"] = prop("number", "Degrees above the horizontal (-80..89). Default 10 (top: 85).");
            p["reset"]     = prop("boolean", "Return the camera to its default view. Other fields are ignored.");
            t.inputSchema  = objectSchema(std::move(p));
            t.handler      = simpleTool(run, &PlusPoserTools::camera, 15.0);
            tools.push_back(std::move(t));
        }
    }
}
