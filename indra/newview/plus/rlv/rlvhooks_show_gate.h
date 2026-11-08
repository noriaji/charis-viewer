/**
 * @file rlvhooks_show_gate.h
 * @brief rlv-show hooks: E1-E3,E5 llui floater gate, URL entries, commands.xml, rlvui_show.cpp (toggle handlers), F1, F5 (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_gate.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".

// rlv-core calls this once, right after RlvHandler::setEnabled(true) (RlvUIEnabler exists then): puts the floater / side
// panel gates' hooks into RlvUIEnabler and registers rlv-show's toggle, exception and modifier handlers (rlvui_show.cpp).
// Does nothing with RLVa off or when already done.
void registerShowToggles();
// rlvui_show.cpp: the handlers themselves (called by registerShowToggles()).
void registerShowUIHandlers();
