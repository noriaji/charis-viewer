/**
 * @file llrhiscene.cpp
 * @brief The retained scene's switch and the backend's scene (research/197 stage 2).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#include "linden_common.h"

#include "llrhiscene.h"

#include <cstdlib>
#include <cstring>

namespace LLRHI
{
    Scene* sScene = nullptr;

    // Read once: the mode does not change while the viewer runs (auto moves
    // between its own states inside the viewer's scene records).
    SceneMode sceneMode()
    {
        static const SceneMode mode = []()
        {
            const char* env = std::getenv("LLRHI_SCENE");
            if (!env || !*env)
            {
                return SCENE_DEFAULT;
            }
            if (!strcmp(env, "verify"))
            {
                return SceneMode::Verify;
            }
            if (!strcmp(env, "auto"))
            {
                return SceneMode::Auto;
            }
            if (!strcmp(env, "on") || !strcmp(env, "1"))
            {
                return SceneMode::On;
            }
            return SceneMode::Off;
        }();
        return mode;
    }

    namespace
    {
        // A part's switch (research/205, 206, 209; research/218 vk-s2f: auto
        // is a mode of its own): off while LLRHI_SCENE is off, unset as the
        // part's default, auto read as verify while LLRHI_SCENE is verify.
        SceneMode part_mode(const char* name, SceneMode unset)
        {
            const SceneMode scene = sceneMode();
            if (scene == SceneMode::Off)
            {
                return SceneMode::Off;
            }
            const char* env = std::getenv(name);
            SceneMode asked = SceneMode::Off;
            if (!env || !*env)
            {
                asked = unset;
            }
            else if (!strcmp(env, "verify"))
            {
                asked = SceneMode::Verify;
            }
            else if (!strcmp(env, "auto"))
            {
                asked = SceneMode::Auto;
            }
            else if (!strcmp(env, "on") || !strcmp(env, "1"))
            {
                asked = SceneMode::On;
            }
            return asked == SceneMode::Auto && scene == SceneMode::Verify ? SceneMode::Verify : asked;
        }
    }

    // Read once, as sceneMode (research/205).
    SceneMode sceneAlphaMode()
    {
        static const SceneMode mode = part_mode("LLRHI_SCENE_ALPHA", SCENE_ALPHA_DEFAULT);
        return mode;
    }

    // research/206: read once, as sceneMode.
    SceneMode sceneShadowMode()
    {
        static const SceneMode mode = part_mode("LLRHI_SCENE_SHADOW", SCENE_SHADOW_DEFAULT);
        return mode;
    }

    // research/209: read once, as sceneMode.
    SceneMode sceneProbeMode()
    {
        static const SceneMode mode = part_mode("LLRHI_SCENE_PROBE", SCENE_PROBE_DEFAULT);
        return mode;
    }

    // research/219: read once, as sceneMode.
    SceneMode sceneRiggedMode()
    {
        static const SceneMode mode = part_mode("LLRHI_SCENE_RIGGED", SCENE_RIGGED_DEFAULT);
        return mode;
    }

    // research/204 10 (vk-s4c2): read once, as sceneMode.
    SceneMode sceneWorldMode()
    {
        static const SceneMode mode = part_mode("LLRHI_SCENE_WORLD", SCENE_WORLD_DEFAULT);
        return mode;
    }

    // ==== scene auto: begin ====
    SceneMode SceneAuto::step(U32 frame, bool differ)
    {
        if (now == SceneMode::Off)
        {
            return now;     // off for the run
        }
        if (differ)
        {
            now = SceneMode::Off;
            return now;
        }
        if (!counted_any || frame != last_frame)
        {
            counted_any = true;
            last_frame = frame;
            ++clean_frames;
        }
        if (now == SceneMode::Verify && clean_frames >= CLEAN_FRAMES)
        {
            now = SceneMode::On;
        }
        return now;
    }
    // ==== scene auto: end ====

    const char* sceneModeName(SceneMode mode)
    {
        switch (mode)
        {
            case SceneMode::Verify:
                return "verify";
            case SceneMode::Auto:
                return "auto";
            case SceneMode::On:
                return "on";
            default:
                return "off";
        }
    }
}
