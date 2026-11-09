/**
 * @file photokeys.cpp
 * @brief Photo tools: keys for the camera's tilt (plus/camroll) and the composition guide.
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

// A held key is called once a frame (KEYSTATE_DOWN the first frame, KEYSTATE_LEVEL after it), so the
// tilt grows by its speed times the frame's time, with the same short ease-in as the official camera
// keys (get_orbit_rate() in llviewerinput.cpp): a tap moves it a fraction of a degree, a hold turns it
// at the full speed. What the keys did shows for a moment in the middle of the screen (photoguide.cpp).
//
// Existing users: the viewer reads the user's key_bindings.xml instead of the default one when there is
// one, so the default keys of new commands never reach them. addDefaultKeys() puts them in that file once.

#include "llviewerprecompiledheaders.h"

#include "photokeys.h"
#include "photoguide.h"
#include "phototools.h"

#include "plus/camroll/camroll.h"
#include "plus/photoscreen/photoscreen.h"
#include "plus/plus.h"

#include <cerrno>
#include <cmath>
#include <iterator>
#include <set>
#include <sstream>

#include "llappviewer.h"     // gFrameCount
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfile.h"
#include "llframetimer.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerinput.h"
#include "llxmlnode.h"
#include "llxuiparser.h"

namespace plus::photomode
{
    namespace
    {
        constexpr char SETTING_ROLL_SPEED[]   = "PlusCameraRollKeySpeed";
        constexpr char SETTING_GUIDE_ON[]     = "PlusPhotoGuideOn";
        constexpr char SETTING_GUIDE_KIND[]   = "PlusPhotoGuideKind";
        constexpr char SETTING_KEYS_DONE[]    = "PlusPhotoKeysCommands";
        constexpr char SETTING_KEYS_VERSION[] = "PlusPhotoKeysVersion"; // the old marker, read once (readDone)

        // The commands with a default key in app_settings/key_bindings.xml. A new one gets its default key
        // into the user's file once (addDefaultKeys), without bringing back the keys of the others.
        constexpr const char* COMMANDS[] = { "roll_left", "roll_right", "roll_reset", "photo_guide_toggle", "photo_guide_next",
                                             "camera_path_add", "camera_path_play", "photo_screen_toggle" };

        // The commands the flycam's mode gets too, remembered apart (FLYCAM_PREFIX before the name):
        // photo_screen_toggle came to the flycam after it had come to the other modes. A user's file gets the key the
        // user has for it in the third person mode (addDefaultKeys), not the default.
        constexpr const char* FLYCAM_COMMANDS[] = { "photo_screen_toggle" };
        constexpr char        FLYCAM_PREFIX[]   = "flycam:";

        // The commands that PlusPhotoKeysVersion 1 stood for, before each command was remembered on its own.
        constexpr const char* COMMANDS_OF_VERSION_1[] = { "roll_left", "roll_right", "roll_reset", "photo_guide_toggle", "photo_guide_next" };

        constexpr F32 FAST       = 3.f;   // with Shift
        constexpr F32 EASE_TIME  = 0.25f; // as NUDGE_TIME and ORBIT_NUDGE_RATE of llviewerinput.cpp
        constexpr F32 EASE_START = 0.05f;

        // The last press of photo_screen_toggle that toggles (keyPhotoScreenToggle), until
        // takePhotoScreenKeyPress() takes it from the gestures. Main thread.
        struct TakenPress
        {
            bool mTaken = false;
            KEY  mKey   = KEY_NONE;
            U32  mFrame = 0;
        };
        TakenPress sTakenPress;

        // The keys reach here as WASD does, only through LLViewerInput::scanKey, which skips a key that
        // LLViewerWindow::handleKey took (mKeyHandledByUI). It takes every key while any UI control has
        // the keyboard focus: a text control keeps the letters for its typing, and the other controls (an
        // inventory or people list, which searches by the typed letters, a button, a slider) fall through
        // to "don't pass keys on to world when something in ui has focus" (childHasKeyboardFocus of the
        // root view). Only with the focus in the world do these keys act (fix-round104). This adds one thing: the numeric keypad goes past the UI to the world even in a
        // text field (mKeysSkippedByUI), and a key rebound to the keypad stays the text's here.
        bool worldHasKeyboard()
        {
            return !plus::textHasKeyboard();
        }

        // The keys are bound without a mask, which the official matching lets through with any modifier;
        // Ctrl and Alt are the menus' (a hidden or disabled item lets the key fall through to here), so
        // only the key alone or with Shift acts. Returning true then takes nothing from anyone: the scan keys'
        // result is not used (LLViewerWindow::handleScanKey), and a binding of the same key with Ctrl or Alt
        // is sorted before the one without a mask and called instead of it.
        bool plainKey()
        {
            return !gKeyboard || !(gKeyboard->currentMask(false) & (MASK_CONTROL | MASK_ALT));
        }

        bool keyIsOurs()
        {
            return worldHasKeyboard() && plainKey();
        }

        // The official default keys of roll_reset are Ctrl+Shift+Q and Ctrl+Shift+E (app_settings/key_bindings.xml,
        // kept for the users who bring the official key settings), and the key settings show them as this
        // command. No menu has those keys; with exactly Ctrl+Shift held the key was bound with them, as the
        // bindings are matched with the most modifiers first (compare_key_by_mask in llviewerinput.cpp).
        bool resetKeyIsOurs()
        {
            if (keyIsOurs())
            {
                return true;
            }
            return worldHasKeyboard() && gKeyboard
                && (gKeyboard->currentMask(false) & (MASK_CONTROL | MASK_ALT | MASK_SHIFT)) == (MASK_CONTROL | MASK_SHIFT);
        }

        F32 easeIn()
        {
            const F32 held = gKeyboard ? gKeyboard->getCurKeyElapsedTime() : EASE_TIME;
            return held < EASE_TIME ? EASE_START + held * (1.f - EASE_START) / EASE_TIME : 1.f;
        }

        void showRoll(F32 degrees)
        {
            if (fabsf(degrees) < 0.05f)
            {
                showGuideMessage(LLTrans::getString("PlusPhotoKeyLevel"));
                return;
            }
            LLStringUtil::format_map_t args;
            args["[DEGREES]"] = llformat("%+.1f", degrees);
            showGuideMessage(LLTrans::getString("PlusPhotoKeyRoll", args));
        }

        void showGuide()
        {
            if (!gSavedSettings.getBOOL(SETTING_GUIDE_ON))
            {
                showGuideMessage(LLTrans::getString("PlusPhotoKeyGuideOff"));
                return;
            }
            // the kind's name as the photo tools show it (app_settings/plus/phototools/catalog.xml)
            const S32   kind = gSavedSettings.getS32(SETTING_GUIDE_KIND);
            std::string name = llformat("%d", kind);
            if (const Item* item = findItem(SETTING_GUIDE_KIND))
            {
                for (const Option& option : item->options)
                {
                    if ((S32)option.value == kind)
                    {
                        name = option.label;
                        break;
                    }
                }
            }
            LLStringUtil::format_map_t args;
            args["[KIND]"] = name;
            showGuideMessage(LLTrans::getString("PlusPhotoKeyGuide", args));
        }

        bool roll(EKeystate state, F32 direction)
        {
            if (state == KEYSTATE_UP || !keyIsOurs())
            {
                return true;
            }
            static LLCachedControl<F32> speed(gSavedSettings, SETTING_ROLL_SPEED, 15.f);
            F32 rate = llmax((F32)speed, 0.f) * easeIn();
            if (gKeyboard && (gKeyboard->currentMask(false) & MASK_SHIFT))
            {
                rate *= FAST;
            }
            const F32 now  = plus::camroll::currentDegrees();
            // a camera path's wider tilt (camroll.h) is not pulled in to the keys' +-45
            const F32 next = plus::camroll::stepped(now, direction * rate * LLFrameTimer::getFrameDeltaTimeF32(),
                                                    plus::camroll::MAX_DEGREES);
            if (next != now)
            {
                plus::camroll::setDegrees(next);
            }
            showRoll(next);
            return true;
        }

        // A binding's key and mask as the viewer reads them (the file may spell them either way).
        bool keyOf(const LLViewerInput::KeyBinding& binding, KEY& key, MASK& mask)
        {
            const std::string& text = binding.key.getValue();
            if (text.empty() || text == "NONE" || !LLKeyboard::keyFromString(text, &key) || key == KEY_NONE)
            {
                return false;
            }
            mask = MASK_NONE;
            LLKeyboard::maskFromString(binding.mask.getValue(), &mask);
            return true;
        }

        // The commands whose default keys were put into the user's file already (or did not need to be).
        std::set<std::string> readDone()
        {
            std::set<std::string> done;
            std::istringstream    words(gSavedSettings.getString(SETTING_KEYS_DONE));
            for (std::string word; words >> word;)
            {
                done.insert(word);
            }
            if (done.empty() && gSavedSettings.controlExists(SETTING_KEYS_VERSION) && gSavedSettings.getS32(SETTING_KEYS_VERSION) >= 1)
            {
                done.insert(std::begin(COMMANDS_OF_VERSION_1), std::end(COMMANDS_OF_VERSION_1));
            }
            return done;
        }

        void writeDone()
        {
            std::string text;
            for (const char* command : COMMANDS)
            {
                text += text.empty() ? "" : " ";
                text += command;
            }
            for (const char* command : FLYCAM_COMMANDS)
            {
                text += text.empty() ? "" : " ";
                text += FLYCAM_PREFIX;
                text += command;
            }
            gSavedSettings.setString(SETTING_KEYS_DONE, text);
            if (gSavedSettings.controlExists(SETTING_KEYS_VERSION))
            {
                gSavedSettings.setS32(SETTING_KEYS_VERSION, 0); // the list takes its place; clearing the list starts over
            }
        }

        size_t countBindings(const LLViewerInput::Keys& keys)
        {
            return keys.first_person.bindings.numValidElements() + keys.third_person.bindings.numValidElements()
                 + keys.sitting.bindings.numValidElements() + keys.edit_avatar.bindings.numValidElements()
                 + keys.flycam.bindings.numValidElements();
        }

        // Written as the key settings write it (LLKeyConflictHandler::saveToSettings); true only when all of
        // it reached the disk.
        bool writeKeys(const std::string& file, const LLViewerInput::Keys& keys)
        {
            LLXMLNodePtr output_node = new LLXMLNode("keys", false);
            LLXUIParser  write_parser;
            write_parser.writeXUI(output_node, keys);
            if (output_node->isNull())
            {
                return false;
            }
            LLFILE* fp = LLFile::fopen(file, LLFILE_MODE("w"));
            if (!fp)
            {
                return false;
            }
            LLXMLNode::writeHeaderToFile(fp);
            output_node->writeToFile(fp);
            const bool write_failed = ferror(fp) != 0;
            const bool close_failed = fclose(fp) != 0;
            return !write_failed && !close_failed;
        }

        // The written file reads back whole: valid, with every binding that was written.
        bool readsBack(const std::string& file, size_t bindings)
        {
            LLViewerInput::Keys keys;
            LLSimpleXUIParser   parser;
            return parser.readXUI(file, keys) && keys.validateBlock() && countBindings(keys) == bindings;
        }

        // The bindings of defaults (the default file's mode, or another mode of the user's) for the commands in fresh
        // that the user's mode lacks, each where its key is free there; false when none was added.
        bool addMissing(const LLViewerInput::KeyMode& defaults, LLViewerInput::KeyMode& user, const std::set<std::string>& fresh)
        {
            std::set<std::string> decided; // commands the user has any binding for, kept as they are
            for (const LLViewerInput::KeyBinding& binding : user.bindings)
            {
                decided.insert(binding.command.getValue());
            }
            bool added = false;
            for (const LLViewerInput::KeyBinding& binding : defaults.bindings)
            {
                const std::string& command = binding.command.getValue();
                KEY  key;
                MASK mask;
                if (!fresh.count(command) || decided.count(command) || !keyOf(binding, key, mask))
                {
                    continue;
                }
                bool taken = false;
                for (const LLViewerInput::KeyBinding& other : user.bindings)
                {
                    KEY  other_key;
                    MASK other_mask;
                    if (keyOf(other, other_key, other_mask) && other_key == key && other_mask == mask)
                    {
                        taken = true;
                        break;
                    }
                }
                if (!taken)
                {
                    user.bindings.add(binding);
                    added = true;
                    LL_INFOS("PlusPhotoKeys") << "Key " << binding.key.getValue() << " (" << binding.mask.getValue()
                                              << ") added for " << command << LL_ENDL;
                }
            }
            return added;
        }
    }

    bool keyRollLeft(EKeystate state)
    {
        return roll(state, -1.f);
    }

    bool keyRollRight(EKeystate state)
    {
        return roll(state, 1.f);
    }

    bool keyRollReset(EKeystate state)
    {
        if (state == KEYSTATE_DOWN && resetKeyIsOurs())
        {
            plus::camroll::setDegrees(0.f);
            showRoll(plus::camroll::currentDegrees()); // the picture's tilt: a flycam's own roll (a game pad's) stays
        }
        return true;
    }

    bool keyGuideToggle(EKeystate state)
    {
        if (state == KEYSTATE_DOWN && keyIsOurs())
        {
            gSavedSettings.setBOOL(SETTING_GUIDE_ON, !gSavedSettings.getBOOL(SETTING_GUIDE_ON));
            showGuide();
        }
        return true;
    }

    bool keyGuideNext(EKeystate state)
    {
        if (state == KEYSTATE_DOWN && keyIsOurs())
        {
            // a hidden guide is shown first as it is; after that each press is the next kind
            if (gSavedSettings.getBOOL(SETTING_GUIDE_ON))
            {
                const S32 count = (S32)GuideKind::Count;
                const S32 kind  = llclamp(gSavedSettings.getS32(SETTING_GUIDE_KIND), 0, count - 1);
                gSavedSettings.setS32(SETTING_GUIDE_KIND, (kind + 1) % count);
            }
            else
            {
                gSavedSettings.setBOOL(SETTING_GUIDE_ON, true);
            }
            showGuide();
        }
        return true;
    }

    bool keyPhotoScreenToggle(EKeystate state)
    {
        // A global key (llviewerinput.cpp): called before the UI sees the key, so also from a text field. The
        // windows and bars are hidden or shown on the next idle, not inside the key's handling.
        // A held key comes again as a DOWN with each of the system's repeats (the global keys are not told a
        // repeat): one toggle per press. The keyboard marks those repeats itself before calling us (it keeps
        // the key's level, and forgets it when the window loses the focus), so a slow first toggle (the photo
        // screen built for the first time) cannot let a repeat through as a second press.
        if (state == KEYSTATE_DOWN && gKeyboard && !gKeyboard->getKeyRepeated(gKeyboard->currentKey()) && plainKey())
        {
            doOnIdleOneTime([]() { plus::photoscreen::toggle(); });
            // This press is the photo screen's: LLViewerWindow::handleKey keeps it from the gestures
            // (takePhotoScreenKeyPress). Not when the toggle would do nothing (not in the world, quitting).
            if (plus::photoscreen::isActive() || plus::photoscreen::canEnter())
            {
                sTakenPress = { true, gKeyboard->currentKey(), gFrameCount };
            }
        }
        return true;
    }

    bool takePhotoScreenKeyPress()
    {
        // The press recorded above for this same key down: the global keys are called with the keyboard's
        // own key (gKeyboard->currentKey(), before the key_bindings.xml remap of LLViewerInput::handleKey),
        // in the same frame and just before the UI. A press that something before the gestures took (a web or
        // media window, the focused control, the tool, the focus picking), a key the UI skips (the numeric
        // keypad, mKeysSkippedByUI) or a Return left to its WM_CHAR never asks here: its record is forgotten
        // by the frame and key check. The modifiers are checked as when it was recorded (plainKey), so a
        // Ctrl or Alt press of the same key in the same frame keeps its gesture. A press is taken only once.
        const bool taken = sTakenPress.mTaken && gKeyboard && sTakenPress.mKey == gKeyboard->currentKey()
                           && sTakenPress.mFrame == gFrameCount && plainKey();
        sTakenPress.mTaken = false;
        return taken;
    }

    void addDefaultKeys()
    {
        if (!gSavedSettings.controlExists(SETTING_KEYS_DONE))
        {
            return;
        }
        // Each command once: a key the user takes away later stays away, also when a new command comes.
        const std::set<std::string> done = readDone();
        std::set<std::string>       fresh;
        for (const char* command : COMMANDS)
        {
            if (!done.count(command))
            {
                fresh.insert(command);
            }
        }
        std::set<std::string> fresh_flycam;
        for (const char* command : FLYCAM_COMMANDS)
        {
            if (!done.count(std::string(FLYCAM_PREFIX) + command))
            {
                fresh_flycam.insert(command);
            }
        }
        if (fresh.empty() && fresh_flycam.empty())
        {
            return;
        }

        const std::string user_file = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "key_bindings.xml");
        if (!gDirUtilp->fileExists(user_file))
        {
            writeDone(); // the default file is in use, with these keys in it
            return;
        }
        const std::string default_file = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "key_bindings.xml");

        LLViewerInput::Keys defaults;
        LLViewerInput::Keys user;
        LLSimpleXUIParser   default_parser;
        LLSimpleXUIParser   user_parser;
        if (!default_parser.readXUI(default_file, defaults) || !defaults.validateBlock()
            || !user_parser.readXUI(user_file, user) || !user.validateBlock())
        {
            // not marked done: tried again the next time
            LL_WARNS("PlusPhotoKeys") << "Could not read the key bindings; the photo keys are not added to " << user_file << LL_ENDL;
            return;
        }

        bool added = false;
        // only the modes the user's file has: a mode it lacks has no keys at all, and stays so
        if (defaults.third_person.isProvided() && user.third_person.isProvided())
        {
            added = addMissing(defaults.third_person, user.third_person, fresh) || added;
        }
        if (defaults.sitting.isProvided() && user.sitting.isProvided())
        {
            added = addMissing(defaults.sitting, user.sitting, fresh) || added;
        }
        if (defaults.first_person.isProvided() && user.first_person.isProvided())
        {
            added = addMissing(defaults.first_person, user.first_person, fresh) || added;
        }
        if (defaults.edit_avatar.isProvided() && user.edit_avatar.isProvided())
        {
            added = addMissing(defaults.edit_avatar, user.edit_avatar, fresh) || added;
        }
        // The flycam takes these as the user has them in the third person mode (sitting when that has none), not
        // the defaults: a key the user took away there stays away here, one moved to another key is that key here.
        // (After the modes above: a key just added there comes along.)
        if (user.flycam.isProvided())
        {
            if (user.third_person.isProvided())
            {
                added = addMissing(user.third_person, user.flycam, fresh_flycam) || added;
            }
            if (user.sitting.isProvided())
            {
                added = addMissing(user.sitting, user.flycam, fresh_flycam) || added;   // skips what the flycam has now
            }
        }
        if (!added)
        {
            writeDone();
            return;
        }

        // The user's file is replaced only by a whole one: written beside it, read back, then moved over it.
        // A cut-off file would leave the session without any key (loadBindingsXML resets them first).
        // the file as it was just before this change (so a later new command's run keeps the newest one)
        const std::string backup = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "key_bindings_before_photo_keys.xml");
        const plus::FileWrite wrote = plus::replaceFile(user_file, [&](const std::string& temp_file)
        {
            if (!writeKeys(temp_file, user) || !readsBack(temp_file, countBindings(user)))
            {
                LL_WARNS("PlusPhotoKeys") << "Could not write the key bindings whole; " << user_file << " is left as it was" << LL_ENDL;
                return plus::FileWrite::cannot_write;
            }
            if (!LLFile::copy(user_file, backup))
            {
                LL_WARNS("PlusPhotoKeys") << "Could not copy " << user_file << " to " << backup << "; it is left as it was" << LL_ENDL;
                return plus::FileWrite::cannot_write;
            }
            return plus::FileWrite::written;
        });
        if (wrote == plus::FileWrite::cannot_replace)
        {
            LL_WARNS("PlusPhotoKeys") << "Could not replace " << user_file << "; it is left as it was" << LL_ENDL;
        }
        if (wrote != plus::FileWrite::written)
        {
            return;
        }
        writeDone();

        if (gViewerInput.loadBindingsXML(user_file) > 0)
        {
            LL_INFOS("PlusPhotoKeys") << "The photo keys were added to " << user_file << " (the old file: " << backup << ")" << LL_ENDL;
            return;
        }
        // as LLAppViewer::loadKeyBindings: never a session without keys
        LL_WARNS("PlusPhotoKeys") << "Could not load " << user_file << "; the old file is used for this session" << LL_ENDL;
        if (gViewerInput.loadBindingsXML(backup) <= 0)
        {
            gViewerInput.loadBindingsXML(default_file);
        }
    }
}
