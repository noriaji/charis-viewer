/**
 * @file localmeshrml.cpp
 * @brief The local mesh window made with RmlUi (research/386): the content is app_settings/plus/rmlui/localmesh.rml.
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

// The same window as localmeshfloater.cpp (frozen: safe mode and PlusRmlUiWindows off). The tools and the target are
// localmeshsession.cpp's. The file list is an RmlList with five columns; the file's meshes a <select>, its log a read-only
// text. What changes (a file added, loaded, removed; the selected object) is looked at and put into the document in
// beforeUpdate() (RmlUi's own turn), never inside an event. Every text that comes from outside (a file's name, a mesh's
// name, the log, the object's name) goes in as plain text.

#include "llviewerprecompiledheaders.h"

#include "localmesh.h"
#include "localmeshrml.h"
#include "localmeshsession.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlparts.h"
#include "plus/rmlui/rmlsettings.h"

#include "llfloaterreg.h"
#include "llviewercontrol.h"

namespace plus::localmesh
{
    namespace
    {
        using plus::rmlui::RmlList;
        using plus::rmlui::SettingsModel;

        constexpr size_t LOG_MAX_BYTES = 65536;     // the XUI text editor's max_length

        class LocalMeshRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~LocalMeshRmlFloater() override
            {
                closeDocument();    // before the list and the model go
                mChangedConnection.disconnect();
            }

            bool postBuild() override
            {
                mChangedConnection = setChangedCallback([this]() { mFilesDirty = true; wake(); });
                return RmlFloater::postBuild();
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                mTools.open();
                mFilesDirty = true;
                wake();
            }

            void onClose(bool app_quitting) override
            {
                mTools.close(app_quitting);
                RmlFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mTracker.update(false))     // (nothing changed: a few compares)
                {
                    mTargetDirty = true;
                    wake();
                }
                RmlFloater::draw();
            }

        protected:
            void beforeLoad(Rml::Context& context) override
            {
                Rml::DataModelConstructor model = mModel.create(context, "localmesh");
                if (!model)
                {
                    return;
                }
                using Kind = SettingsModel::Kind;
                const std::vector<SettingsModel::Entry> entries = {
                    { "auto_reload", &gSavedSettings, "PlusLocalMeshAutoReload", Kind::Bool, 0 },
                    { "file_size",   &gSavedSettings, "PlusLocalMeshFileSize",   Kind::Bool, 0 },
                };
                mModel.bind(model, entries);
            }

            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                mList.attach(document, "list", false);
                mList.attachHead(document, "list_head");
                mList.setColumns({
                    { "status", getString("col_status"), std::string(), 70, true, RmlList::SortKind::Text },
                    { "name",   getString("col_name"),   std::string(), 0,  true, RmlList::SortKind::Text },
                    { "lods",   getString("col_lods"),   getString("tip_lods"),  55, true, RmlList::SortKind::Text },
                    { "meshes", getString("col_meshes"), std::string(), 55, true, RmlList::SortKind::Number },
                    { "shown",  getString("col_shown"),  getString("tip_shown"), 55, true, RmlList::SortKind::Number },
                });
                for (const char* event : { "click", "dblclick", "keydown", "mousedown" })
                {
                    view->listen(document.GetElementById("list"), event);
                }
                view->listen(mList.headElement(), "click");
                for (const char* id : { "add", "reload", "remove", "show", "take_off" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                mFilesDirty = true;
                mTargetDirty = true;
                mSelectedKey.clear();
            }

            void beforeUpdate() override
            {
                mModel.beforeUpdate();
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                bool file_changed = false;
                if (mFilesDirty)
                {
                    mFilesDirty = false;
                    rebuildFiles();
                    file_changed = true;
                }
                mList.apply();
                const std::string key = currentKey();
                if (file_changed || key != mSelectedKey)
                {
                    mSelectedKey = key;
                    onFileSelected(*document);
                    mTracker.update(true);
                    mTargetDirty = true;
                }
                if (mTargetDirty)
                {
                    mTargetDirty = false;
                    updateTarget(*document);
                }
            }

            void afterUpdate() override
            {
                mList.afterUpdate();
            }

            // In RmlUi's event: no element is removed here (the rows change in beforeUpdate).
            void onEvent(Rml::Event& event) override
            {
                const Rml::Element* element = event.GetCurrentElement();
                if (!element)
                {
                    return;
                }
                const std::string id = element->GetId();
                if (id == "list" || id == "list_head")
                {
                    mList.handleEvent(event);   // the selection is looked at in beforeUpdate
                    mTargetDirty = true;
                    return;
                }
                if (event.GetId() != Rml::EventId::Click || plus::rmlui::isElementDisabled(element))
                {
                    return;
                }
                const LLUUID file(currentKey());
                if (id == "add")
                {
                    addFilesFromPicker();
                }
                else if (id == "reload")
                {
                    reloadFile(file);
                }
                else if (id == "remove")
                {
                    removeFile(file);
                }
                else if (id == "show")
                {
                    if (const FileInfo* info = findFile(mInfos, file))
                    {
                        showOnTarget(*info, meshIndex(), mTracker.state().id, gSavedSettings.getBOOL("PlusLocalMeshFileSize"));
                    }
                }
                else if (id == "take_off")
                {
                    takeOffTarget(mTracker.state().id);
                }
            }

            void unbind() override
            {
                mChangedConnection.disconnect();
                mModel.unbind();
                mList.detach();
            }

        private:
            explicit LocalMeshRmlFloater(const LLSD& key)
            :   RmlFloater(key),
                mModel([this]() { wake(); })
            {
            }

            void wake()
            {
                if (LLRmlView* view = host())
                {
                    view->markDirty();
                }
            }

            // The key (the file's id) of the selected row, "" when none.
            std::string currentKey() const
            {
                const std::vector<std::string> keys = mList.selectedKeys();
                return keys.empty() ? std::string() : keys.front();
            }

            // The mesh chosen in the select (-1: none).
            int meshIndex() const
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                Rml::Element* element = document ? document->GetElementById("meshes") : nullptr;
                auto* box = element ? rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element) : nullptr;
                return box ? box->GetSelection() : -1;
            }

            // The files into the list; the file just added is selected, else the one that was, else the first.
            void rebuildFiles()
            {
                const std::string selected = currentKey();
                const size_t old_count = mInfos.size();
                mInfos = files();

                std::vector<RmlList::Row> rows;
                rows.reserve(mInfos.size());
                for (const FileInfo& info : mInfos)
                {
                    RmlList::Row row;
                    row.key = info.id.asString();
                    row.cells.resize(5);
                    row.cells[0].text = getString(info.status == Status::Loading  ? "status_loading"
                                                  : info.status == Status::Active ? "status_active"
                                                                                  : "status_error");
                    row.cells[1].text = info.name;
                    row.cells[2].text = lodsText(info);
                    row.cells[3].text = std::to_string(info.meshes.size());
                    row.cells[3].sort_value = (double)info.meshes.size();
                    row.cells[4].text = std::to_string(info.shownOn);
                    row.cells[4].sort_value = (double)info.shownOn;
                    rows.push_back(std::move(row));
                }
                mList.setRows(std::move(rows));

                if (mInfos.size() > old_count && !mInfos.empty())
                {
                    mList.setSelectedKeys({ mInfos.back().id.asString() });
                    mList.ensureVisible(mInfos.back().id.asString());
                }
                else if (!findFile(mInfos, LLUUID(selected)) && !mInfos.empty())
                {
                    mList.setSelectedKeys({ mInfos.front().id.asString() });
                }
            }

            // The selected file's meshes and log.
            void onFileSelected(Rml::ElementDocument& document)
            {
                const FileInfo* info = findFile(mInfos, LLUUID(mSelectedKey));
                if (auto* box = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document.GetElementById("meshes")))
                {
                    const int old_mesh = box->GetSelection();
                    const LLUUID old_file = mMeshesFile;
                    mMeshesFile = info ? info->id : LLUUID::null;
                    std::vector<plus::rmlui::SelectOption> options;
                    if (info)
                    {
                        for (size_t i = 0; i < info->meshes.size(); ++i)
                        {
                            options.push_back({ std::to_string(i), info->meshes[i] });
                        }
                    }
                    const bool same_file = old_file == mMeshesFile && old_mesh >= 0 && old_mesh < (int)options.size();
                    plus::rmlui::fillSelect(*box, options, same_file ? std::to_string(old_mesh) : std::string("0"));
                }
                std::string text;
                if (info)
                {
                    for (const std::string& line : fileLog(info->id))
                    {
                        text += line + "\n";
                    }
                }
                // The log is the textarea readonly (the host's part); as the XUI editor it is cut to max_length.
                if (LLRmlView* view = host())
                {
                    plus::rmlui::setLogText(*view, document.GetElementById("log"),
                                            utf8str_truncate(text, (S32)LOG_MAX_BYTES), plus::rmlui::Follow::Keep);
                }
            }

            // The selected object, what can be done with it, and the buttons.
            void updateTarget(Rml::ElementDocument& document)
            {
                const TargetState& target = mTracker.state();
                std::string text;
                if (!target.can_show)
                {
                    text = getString("why_" + target.why_not);
                }
                else
                {
                    LLStringUtil::format_map_t args;
                    args["[NAME]"] = target.name.empty() ? getString("unnamed") : target.name;
                    text = getString(target.shown ? "target_shown" : "target", args);
                }
                if (Rml::Element* line = document.GetElementById("target"))
                {
                    const std::string rml = plus::rmlui::escapeRml(text);
                    if (line->GetInnerRML() != rml)
                    {
                        line->SetInnerRML(rml);
                    }
                }
                const FileInfo* info = findFile(mInfos, LLUUID(mSelectedKey));
                const bool loading = info && info->status == Status::Loading;
                const bool active = info && info->status == Status::Active;
                auto set = [&](const char* id, bool enabled)
                {
                    plus::rmlui::setElementDisabled(document.GetElementById(id), !enabled);
                };
                set("reload", info && !loading);
                set("remove", info && !loading);
                set("meshes", active);
                set("show", target.can_show && active && meshIndex() >= 0);
                set("take_off", target.shown);
            }

            SettingsModel mModel;
            plus::rmlui::RmlList mList;
            ToolSession mTools;
            TargetTracker mTracker;
            std::vector<FileInfo> mInfos;
            std::string mSelectedKey;           // the file the meshes and the log are of
            LLUUID mMeshesFile;
            bool mFilesDirty = true;
            bool mTargetDirty = true;
            boost::signals2::connection mChangedConnection;
        };
    }

    void addRmlFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_local_mesh_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<LocalMeshRmlFloater>);
    }
}
