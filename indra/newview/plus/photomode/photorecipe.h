/**
 * @file photorecipe.h
 * @brief Settings written as they are and given back later (shared by the
 *        photo presets and the one-press recipes), and the one-press recipe
 *        "sunbeams through clouds" (research/253-cloud-light-shafts.md 9 in
 *        the sl-viewer repository).
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

#ifndef PLUS_PHOTOMODE_PHOTORECIPE_H
#define PLUS_PHOTOMODE_PHOTORECIPE_H

#include <functional>
#include <map>
#include <string>

#include "llsd.h"

namespace plus::photomode
{
    // --- Settings written and given back ---------------------------------------
    // A setting as it was before it was first written, and what was written (as the control holds it).
    struct SettingWrite
    {
        LLSD before;
        LLSD wrote;
    };
    // Where the writes are kept is the caller's: the presets keep theirs for the session (photomode.cpp),
    // the recipe below in a saved setting, so that it can be given back after a restart.
    using SettingWrites = std::map<std::string, SettingWrite>;

    // Writes `value` into the setting and keeps what it was before; a key written already keeps the
    // `before` of its first write. False when there is no such setting (a branch not merged): nothing written.
    bool writeRemembered(SettingWrites& writes, const std::string& key, const LLSD& value);
    // The setting exists and still holds what was written.
    bool stillAsWritten(const std::string& key, const SettingWrite& write);
    // The setting exists and holds something else now (changed by hand, the Charis menu, the photo tools).
    bool changedSince(const std::string& key, const SettingWrite& write);
    // The setting back as it was, when it still holds what was written. One changed since is the user's and stays.
    void giveBack(const std::string& key, const SettingWrite& write);

    // --- The one-press recipe "sunbeams through clouds" (research/253 9) ----------
    // One switch, PlusCloudShaftsScene (the photo tools' Sky & Weather tab, the photo screen, the Charis
    // menu), puts on together what the shafts of light through gaps in the clouds need: the volumetric
    // clouds, their shadows at 0.7 or more, the shafts at 1.5 or more, the volumetric light at a density of
    // 1.5 or more and medium quality or better. Only what is below that is written, and what it was before
    // is kept in PlusCloudShaftsSceneBefore (saved). Off gives back each one that still holds what the
    // switch wrote. Unlike the photo presets it does set settings of the quality levels (the volumetric
    // clouds and light), so it is a switch of its own and not one of them (photomode.cpp, the top).
    //
    // The switch shows whether all of it is on, made again from the values whenever one of them changes
    // (and at the start: it is not saved), so the three places always agree. Applying a quality level gives
    // back what the switch wrote besides the level's own settings and forgets the rest: the level's
    // picture is what was asked for. The recipe watches its settings from the feature's init (photorecipe.cpp).

    // The photo tools opening (their values to reset to are taken then): what the recipes have written so far
    // (put on before the tools opened, in this session or a session before) is part of what Reset All goes
    // back to, and stays theirs to give back.
    void rememberRecipesAsOpened();

    // Reset All of the photo tools (research/253 9.3). What the recipes wrote after the tools opened is given
    // back; then `putRowsBack` puts the rows back to their values as the tools opened, a recipe's switch row
    // too, which is not taken as the user's (the switch then shows what the values say, not a recipe put on
    // again). What they wrote before the tools opened and holds it after the rows are back is kept, so that
    // turning the switch off later still gives it back.
    void letGoOfRecipes(const std::function<void()>& putRowsBack);
}

#endif // PLUS_PHOTOMODE_PHOTORECIPE_H
