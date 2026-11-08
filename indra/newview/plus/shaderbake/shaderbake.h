/**
 * @file shaderbake.h
 * @brief --bake-shaders: the viewer compiles the shaders of every graphics
 *        preset and its neighbours into an empty cache folder, without
 *        logging in, and quits (research/159 section 3.1).
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

#ifndef PLUS_SHADERBAKE_H
#define PLUS_SHADERBAKE_H

#include <string>

// tools/bake-shaders.sh runs the release exe with
//   --bake-shaders <folder> [--bake-shaders-vendors nvidia,amd,other]
// and packs <folder>/cache into app_settings/vk_shader_bundle/shaders.vkb.
namespace plus::shaderbake
{
    // The <plus> hook in LLAppViewer::initConfiguration(), once the command
    // line is applied (before the window and the Vulkan device are made):
    // the bake's folder (relative to the current folder, or absolute) and the
    // vendors (comma separated; empty: every one of the grid). From here on
    // the shader cache is <folder>/cache (LLRHI_VK_SHADER_CACHE), the shipped
    // bundle is not used (each build compiles or reads that cache), the
    // settings are saved to a file of the bake's own (never the user's
    // settings.xml), and once the login screen is up the bake runs, one set
    // of settings at each idle, then the viewer quits.
    void configure(const std::string& folder, const std::string& vendors);

    // True once configure() ran.
    bool active();
}

#endif // PLUS_SHADERBAKE_H
