/**
 * @file rmlsettings.cpp
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

#include "llviewerprecompiledheaders.h"

#include "rmlsettings.h"

#include "v4color.h"

#include <cstdio>

namespace plus::rmlui
{
    std::string hexColour(const LLColor4& colour)
    {
        auto byte = [](F32 v) { return llclamp(ll_round(v * 255.f), 0, 255); };
        char text[8];
        snprintf(text, sizeof(text), "#%02x%02x%02x", byte(colour.mV[VRED]), byte(colour.mV[VGREEN]), byte(colour.mV[VBLUE]));
        return text;
    }

    // Marks which slot the bridge itself writes: its signal then does not mark the variable again.
    class SettingsModel::Applying
    {
    public:
        Applying(SettingsModel& model, S32 index) : mModel(model), mPrevious(model.mApplying) { model.mApplying = index; }
        ~Applying() { mModel.mApplying = mPrevious; }
        Applying(const Applying&) = delete;
        Applying& operator=(const Applying&) = delete;

    private:
        SettingsModel& mModel;
        S32 mPrevious;
    };

    SettingsModel::SettingsModel(std::function<void()> wake)
    :   mWake(std::move(wake))
    {
    }

    SettingsModel::~SettingsModel()
    {
        unbind();
    }

    Rml::DataModelConstructor SettingsModel::create(Rml::Context& context, const char* model_name)
    {
        if (mHandle || !mSlots.empty())
        {
            return Rml::DataModelConstructor();     // once only
        }
        return context.CreateDataModel(model_name);
    }

    std::string SettingsModel::limited(const std::string& text, S32 max_chars)
    {
        if (max_chars <= 0)
        {
            return text;
        }
        S32 count = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80 && ++count > max_chars)
            {
                return text.substr(0, i);
            }
        }
        return text;
    }

    void SettingsModel::read(Slot& slot)
    {
        const LLSD value = slot.control->getValue();
        switch (slot.entry.kind)
        {
        case Kind::Bool:
            slot.flag = value.asBoolean();
            break;
        case Kind::String:
            slot.latest = slot.shown = value.asString();
            break;
        case Kind::Colour:
            slot.shown = hexColour(LLColor4(value));
            break;
        }
    }

    void SettingsModel::bind(Rml::DataModelConstructor& model, const std::vector<Entry>& entries)
    {
        if (!model || !mSlots.empty())
        {
            return;
        }
        mSlots.reserve(entries.size());     // the lambdas below keep indices into it: it never grows again
        for (const Entry& entry : entries)
        {
            const size_t index = mSlots.size();
            Slot& slot = mSlots.emplace_back();
            slot.entry = entry;
            slot.variable = entry.variable;
            slot.control = entry.group->getControl(entry.setting);
            if (slot.control.isNull())
            {
                LL_WARNS("RmlUi") << "RmlUi: the setting " << entry.setting << " of " << entry.variable << " does not exist" << LL_ENDL;
            }
            else
            {
                read(slot);
                slot.connection = slot.control->getSignal()->connect(
                    [this, index](LLControlVariable*, const LLSD&, const LLSD&) { onChanged(index); });
            }
            if (entry.kind == Kind::Bool)
            {
                model.BindFunc(slot.variable, [this, index](Rml::Variant& value) { value = mSlots[index].flag; },
                               [this, index](const Rml::Variant& value) { setBool(index, value.Get<bool>(false)); });
            }
            else
            {
                model.BindFunc(slot.variable, [this, index](Rml::Variant& value) { value = mSlots[index].shown; });
            }
        }
        mHandle = model.GetModelHandle();
    }

    void SettingsModel::unbind()
    {
        for (Slot& slot : mSlots)
        {
            slot.connection.disconnect();
            slot.dirty = false;
            slot.held = false;
        }
        mAnyDirty = false;
        mHandle = Rml::DataModelHandle();
    }

    SettingsModel::Slot* SettingsModel::find(const std::string& variable)
    {
        for (Slot& slot : mSlots)
        {
            if (slot.variable == variable)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    const SettingsModel::Slot* SettingsModel::find(const std::string& variable) const
    {
        return const_cast<SettingsModel*>(this)->find(variable);
    }

    bool SettingsModel::revertText(const std::string& variable, std::string& text) const
    {
        const Slot* slot = find(variable);
        if (!slot || slot->entry.kind != Kind::String)
        {
            return false;
        }
        text = limited(slot->shown, slot->entry.max_chars);
        return true;
    }

    void SettingsModel::markDirty(Slot& slot)
    {
        slot.dirty = true;
        mAnyDirty = true;
        if (mWake)
        {
            mWake();
        }
    }

    void SettingsModel::beforeUpdate()
    {
        if (!mAnyDirty)
        {
            return;
        }
        mAnyDirty = false;
        for (Slot& slot : mSlots)
        {
            if (slot.dirty)
            {
                slot.dirty = false;
                if (mHandle)
                {
                    mHandle.DirtyVariable(slot.variable);
                }
            }
        }
    }

    void SettingsModel::onChanged(size_t index)
    {
        if (index >= mSlots.size() || static_cast<S32>(index) == mApplying)
        {
            return;
        }
        Slot& slot = mSlots[index];
        const LLSD value = slot.control->getValue();
        switch (slot.entry.kind)
        {
        case Kind::Bool:
        {
            const bool now = value.asBoolean();
            if (now != slot.flag)
            {
                slot.flag = now;
                markDirty(slot);
            }
            break;
        }
        case Kind::String:
        {
            const std::string now = value.asString();
            if (now == slot.latest)
            {
                break;
            }
            slot.latest = now;
            if (!slot.held)     // a field with the focus keeps its text: endEdit decides
            {
                slot.shown = now;
                markDirty(slot);
            }
            break;
        }
        case Kind::Colour:
        {
            const std::string now = hexColour(LLColor4(value));
            if (now != slot.shown)
            {
                slot.shown = now;
                markDirty(slot);
            }
            break;
        }
        }
    }

    void SettingsModel::write(Slot& slot, const LLSD& value)
    {
        if (slot.control.isNull())
        {
            return;
        }
        Applying applying(*this, static_cast<S32>(&slot - mSlots.data()));
        slot.control->setValue(value);
    }

    void SettingsModel::setBool(size_t index, bool value)
    {
        if (index >= mSlots.size() || mSlots[index].flag == value)
        {
            return;
        }
        mSlots[index].flag = value;
        write(mSlots[index], LLSD(value));
    }

    void SettingsModel::beginEdit(const std::string& variable)
    {
        if (Slot* slot = find(variable))
        {
            slot->held = slot->entry.kind == Kind::String;
        }
    }

    void SettingsModel::commitText(const std::string& variable, const std::string& text)
    {
        Slot* slot = find(variable);
        if (!slot || slot->entry.kind != Kind::String || text == limited(slot->shown, slot->entry.max_chars))
        {
            return;     // not changed by the user (an over-long setting shows cut and is not written back cut)
        }
        slot->shown = slot->latest = text;
        write(*slot, LLSD(text));
    }

    void SettingsModel::endEdit(const std::string& variable, const std::string& text)
    {
        Slot* slot = find(variable);
        if (!slot || slot->entry.kind != Kind::String)
        {
            return;
        }
        slot->held = false;
        if (text != limited(slot->shown, slot->entry.max_chars))
        {
            commitText(variable, text);
        }
        else if (slot->latest != slot->shown)
        {
            slot->shown = slot->latest;     // what changed outside meanwhile shows now
            markDirty(*slot);
        }
    }
}
