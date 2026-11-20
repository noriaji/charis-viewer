/**
 * @file rmlsettings.h
 * @brief SettingsModel: settings as an RmlUi data model, in place of control_name (research/380 2).
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

// The bridge a window's data model keeps to the settings (what an XUI window does with control_name). A window
// lists {variable, settings group, setting, kind}; the document binds the variables (data-checked, data-attr-value,
// data-style-background-color, and the conditions of data-attrif-disabled / data-class-off).
//
//   Bool    data-checked: the box writes the setting (the model's setter), a change of the setting shows in the box.
//   String  data-attr-value: shown from the setting; written only when the window says the text is committed
//           (commitText: Enter; endEdit: the focus left), and only when it differs from what the field was given.
//           While the field has the focus (beginEdit .. endEdit) a change from outside is kept back, so a
//           typed (and IME composed) text is not replaced under the user's hands; at endEdit the user's text wins
//           when it was changed, else the kept value shows.
//   Colour  data-style-background-color: "#rrggbb" of the setting, read only (the colour picker writes it, rmlcolour.h).
//
// Nothing runs per frame: the setting's commit signal marks the variable (and wakes the host), beforeUpdate()
// (the host's before_update hook, outside RmlUi's Update) dirties the marked variables. The signals are cut
// by unbind(), which the window calls before its document goes (the model's handle is not used after it).
// A write of the bridge's own does not mark its variable again (the value is already the new one); the other
// listeners of the setting still hear it.

#ifndef PLUS_RMLSETTINGS_H
#define PLUS_RMLSETTINGS_H

#include "rmlcommon.h"

#include "llcontrol.h"

#include <boost/signals2/connection.hpp>

#include <functional>
#include <string>
#include <vector>

namespace plus::rmlui
{
    // "#rrggbb" of a colour (RCSS / data-style-background-color), alpha left out.
    std::string hexColour(const LLColor4& colour);

    class SettingsModel
    {
    public:
        enum class Kind { Bool, String, Colour };

        struct Entry
        {
            const char* variable;       // the name in the document
            LLControlGroup* group;      // gSavedSettings or gSavedPerAccountSettings
            const char* setting;
            Kind kind;
            S32 max_chars;              // String: the field's maxlength in characters (0: none)
        };

        // wake: the host's markDirty (something changed outside RmlUi's input).
        explicit SettingsModel(std::function<void()> wake);
        ~SettingsModel();
        SettingsModel(const SettingsModel&) = delete;
        SettingsModel& operator=(const SettingsModel&) = delete;

        // The host's before_load hook: makes the data model (null constructor when RmlUi refuses). Once only.
        Rml::DataModelConstructor create(Rml::Context& context, const char* model_name);
        // Binds the variables to the settings and connects their signals. A missing setting is warned once and
        // its variable keeps its default and is never written.
        void bind(Rml::DataModelConstructor& model, const std::vector<Entry>& entries);
        // Cuts the signals and forgets the model. Idempotent.
        void unbind();
        // Dirties the variables marked since the last call (the host's before_update hook).
        void beforeUpdate();
        // For the window's own variables of the same model.
        Rml::DataModelHandle handle() const { return mHandle; }

        void beginEdit(const std::string& variable);
        // The focus left the field: its text is committed when the user changed it.
        void endEdit(const std::string& variable, const std::string& text);
        // Enter in the field.
        void commitText(const std::string& variable, const std::string& text);
        // Esc in the field (the XUI line editor's revert_on_esc): the text the field was last given, which it goes back
        // to without committing anything. False: no such text variable.
        bool revertText(const std::string& variable, std::string& text) const;

    private:
        struct Slot
        {
            Entry entry;
            std::string variable;
            LLControlVariablePtr control;
            bool flag = false;              // Bool: the value
            std::string shown;              // String / Colour: what the document was given
            std::string latest;             // String: the setting's value now (shown, or kept back)
            bool held = false;              // String: the field has the focus
            bool dirty = false;
            boost::signals2::scoped_connection connection;
        };
        class Applying;

        Slot* find(const std::string& variable);
        const Slot* find(const std::string& variable) const;
        void read(Slot& slot);              // the setting's value into the slot (bind)
        void onChanged(size_t index);
        void write(Slot& slot, const LLSD& value);
        void setBool(size_t index, bool value);
        void markDirty(Slot& slot);
        static std::string limited(const std::string& text, S32 max_chars);

        std::function<void()> mWake;
        Rml::DataModelHandle mHandle;
        std::vector<Slot> mSlots;           // fixed after bind (the lambdas keep indices)
        bool mAnyDirty = false;
        S32 mApplying = -1;                 // the slot being written by the bridge itself
    };
}

#endif // PLUS_RMLSETTINGS_H
