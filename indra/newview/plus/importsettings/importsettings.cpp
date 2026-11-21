/**
 * @file importsettings.cpp
 * @brief The window that brings settings over from another viewer, shown at
 *        the first start and from Preferences (research/143).
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

// How it runs:
//   init: no settings.xml in our folder yet (the first start of this install)
//     -> PlusImportSettingsPending is set. Every start notes our folder for other
//        Charis Viewer installs (sources.cpp).
//   onUIReady -> PlusImportSettingsPending set: at the login screen (or once the
//     world is up, when the login started by itself) the window opens, if there
//     is a viewer to bring settings over from; closing it clears the setting.
//   Preferences > Setup "Bring settings over..." and Preferences > Charis open it at
//     any time. There it asks first: our settings are overwritten.
// The window lists the viewers found (sources.cpp) and takes a folder the user
// picks; what to bring over is chosen with check boxes (importer.cpp does it).
// The graphics box is ticked for another Charis Viewer only: the graphics
// settings of other viewers were made for their own renderers.


#include "llviewerprecompiledheaders.h"

#include "importcontroller.h"
#include "importrml.h"
#include "importsettings.h"
#include "plus.h"
#include "plus/safemode/safemode.h"

#include "llapp.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "lldir.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llstartup.h"
#include "llviewercontrol.h"

namespace
{
    using namespace plus::importsettings;

    constexpr char FLOATER_NAME[] = "plus_import_settings";
    constexpr char SETTING_PENDING[] = "PlusImportSettingsPending";

    // The XUI window (frozen: used in safe mode, where RmlUi is not started, and with PlusRmlUiWindows off; the choice is
    // made once, in init()). What it does is importcontroller.cpp's, shared with the RmlUi window (importrml.cpp,
    // research/386); this class is the view.
    class ImportFloater : public LLFloater, public ImportController::View
    {
        friend class ::LLFloaterReg;

    public:
        bool postBuild() override
        {
            mList = getChild<LLScrollListCtrl>("sources");
            mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { mController.onSelect(); });
            getChild<LLButton>("btn_browse")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mController.browse(); });
            getChild<LLButton>("btn_import")->setCommitCallback([this](LLUICtrl*, const LLSD&) { mController.onImport(); });
            getChild<LLButton>("btn_close")->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
            return true;
        }

        void onOpen(const LLSD&) override
        {
            mController.open();
        }

        void onClose(bool app_quitting) override
        {
            mController.onClose(app_quitting);
        }

        // ImportController::View
        std::string text(const char* id, const LLStringUtil::format_map_t& args) override { return getString(id, args); }
        void setIntro(const std::string& text) override { getChild<LLUICtrl>("intro")->setValue(text); }
        void setCloseLabel(const std::string& text) override { getChild<LLButton>("btn_close")->setLabel(text); }
        void setStatus(const std::string& text) override { getChild<LLUICtrl>("status")->setValue(text); }
        void setSources(const std::vector<Source>& sources, size_t selected) override
        {
            mList->deleteAllItems();
            for (size_t i = 0; i < sources.size(); ++i)
            {
                const Source& source = sources[i];
                LLSD row;
                row["value"] = (S32)i;
                row["columns"][0]["column"] = "name";
                row["columns"][0]["value"] = source.name;
                row["columns"][1]["column"] = "folder";
                row["columns"][1]["value"] = source.root;
                row["columns"][2]["column"] = "date";
                row["columns"][2]["value"] = ImportController::dateText(source.last_used);
                mList->addElement(row);
            }
            if (!sources.empty())
            {
                mList->selectByValue((S32)selected);
            }
        }
        int selectedSource() const override
        {
            const LLScrollListItem* item = mList->getFirstSelected();
            return item ? item->getValue().asInteger() : -1;
        }
        void setImportEnabled(bool enabled) override { getChild<LLButton>("btn_import")->setEnabled(enabled); }
        void setGraphicsChecked(bool checked) override { getChild<LLCheckBoxCtrl>("chk_graphics")->set(checked); }
        Choice choice() const override
        {
            Choice c;
            c.general = getChild<LLCheckBoxCtrl>("chk_general")->get();
            c.graphics = getChild<LLCheckBoxCtrl>("chk_graphics")->get();
            c.keys = getChild<LLCheckBoxCtrl>("chk_keys")->get();
            c.accounts = getChild<LLCheckBoxCtrl>("chk_accounts")->get();
            c.chat = getChild<LLCheckBoxCtrl>("chk_chat")->get();
            c.files = getChild<LLCheckBoxCtrl>("chk_files")->get();
            return c;
        }

    private:
        explicit ImportFloater(const LLSD& key) : LLFloater(key), mController(*this) {}

        LLScrollListCtrl* mList = nullptr;
        ImportController mController;
    };

    void showAtLoginScreen()
    {
        doOnIdleRepeating([]()
        {
            if (LLApp::isExiting())
            {
                return true;
            }
            const EStartupState state = LLStartUp::getStartupState();
            if (state != STATE_LOGIN_WAIT && state != STATE_STARTED)
            {
                return false;
            }
            if (findSources().empty())
            {
                LL_INFOS("ImportSettings") << "First start: no other viewer to bring settings over from" << LL_ENDL;
                gSavedSettings.setBOOL(SETTING_PENDING, false);
            }
            else
            {
                LLFloaterReg::showInstance(FLOATER_NAME);
            }
            return true;
        });
    }

    void init()
    {
        // settings.xml is written at the end of a session: without one, this install has never run
        const std::string settings_file = gSavedSettings.getString("ClientSettingsFile");
        if (!settings_file.empty() && !gDirUtilp->fileExists(settings_file))
        {
            gSavedSettings.setBOOL(SETTING_PENDING, true);
        }
        plus::importsettings::rememberThisInstall();

        if (plus::safemode::active() || !gSavedSettings.getBOOL("PlusRmlUiWindows"))
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_import_settings.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<ImportFloater>);
        }
        else
        {
            addRmlFloater(FLOATER_NAME);
        }
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.ImportSettings.Open",
            [](LLUICtrl*, const LLSD&) { LLFloaterReg::showInstance(FLOATER_NAME); });
    }

    void onUIReady()
    {
        if (gSavedSettings.getBOOL(SETTING_PENDING))
        {
            showAtLoginScreen();
        }
    }

    plus::FeatureRegistration sRegistration({ "importsettings", &init, &onUIReady });
}
