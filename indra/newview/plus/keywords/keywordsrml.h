/**
 * @file keywordsrml.h
 * @brief Chat keyword alerts: the settings window made with RmlUi (research/380).
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

#ifndef PLUS_KEYWORDSRML_H
#define PLUS_KEYWORDSRML_H

namespace plus::keywords
{
    // Registers the RmlUi version of the window (floater_plus_keywords_rml.xml) under floater_name.
    // keywordsfloater.cpp chooses it or the frozen XUI version when the feature starts.
    void addRmlFloater(const char* floater_name);
}

#endif // PLUS_KEYWORDSRML_H
