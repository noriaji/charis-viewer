/**
 * @file importcontroller.h
 * @brief The bring-settings-over window's work, apart from how it is shown (shared by the XUI and the RmlUi window).
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

// What importsettings.cpp describes (the list of viewers, the folder the user picks, the check boxes, the confirmation,
// the progress) is here; a window (importsettings.cpp: the XUI one, frozen; importrml.cpp: the RmlUi one, research/386)
// gives it a View: the list, the status text, the check boxes and the buttons' state. The window calls open(),
// onClose(), onSelect() (the list's selection changed), browse() and onImport(). Main thread only.
// The copying (importer.cpp) goes on when the window closes and reports to the window that is open then (sOpen).

#ifndef PLUS_IMPORTCONTROLLER_H
#define PLUS_IMPORTCONTROLLER_H

#include "importsettings.h"

#include <memory>
#include <string>
#include <vector>

#include "llstring.h"

namespace plus::importsettings
{
    class ImportController
    {
    public:
        class View
        {
        public:
            virtual ~View() = default;
            // One of the window's <string>s (args: [FOLDER] [NAME] [COPIED] [TOTAL] [SETTINGS] [FILES] [KEPT] [OVER]
            // [ERRORS]).
            virtual std::string text(const char* id, const LLStringUtil::format_map_t& args) = 0;
            virtual void setIntro(const std::string& text) = 0;         // the first start's own introduction
            virtual void setCloseLabel(const std::string& text) = 0;    // "Start fresh" at the first start
            virtual void setStatus(const std::string& text) = 0;
            // The list of viewers (the rows' texts are the sources' name, folder and last-used date) with the row
            // selected (an index into sources).
            virtual void setSources(const std::vector<Source>& sources, size_t selected) = 0;
            // The row selected in the list: an index into the sources last given, or -1.
            virtual int selectedSource() const = 0;
            virtual void setImportEnabled(bool enabled) = 0;
            virtual void setGraphicsChecked(bool checked) = 0;
            // The six check boxes.
            virtual Choice choice() const = 0;
        };

        explicit ImportController(View& view);
        ~ImportController();
        ImportController(const ImportController&) = delete;
        ImportController& operator=(const ImportController&) = delete;

        void open();
        // The window is closed (not when the viewer quits: it is offered again at the next start).
        void onClose(bool app_quitting);
        // The selection of the list changed (or the state of the copying did).
        void onSelect();
        void browse();
        void onImport();

        // A viewer's name as a date text ("2026-11-20 14:03", or empty for 0): for the list's third column.
        static std::string dateText(std::time_t time);

    private:
        void onProgress(U32 copied, U32 total);
        void onDone(const std::string& name, const Result& result);
        void refreshList(size_t selected);
        const Source* selected() const;
        void onPicked(const std::string& folder);
        void runImport(const Source& source, const Choice& chosen);
        static void startImport(const Source& source, const Choice& chosen, ImportController* self);

        View& mView;
        std::vector<Source> mSources;
        bool mFirstStart = false;
        std::string mGraphicsFor;       // the viewer the graphics box was last set for (its folder)
        bool mGraphicsSet = false;
        // Callbacks that come later (the folder picker, the confirmation, the frame after "Bring over") hold a weak
        // pointer to this and do nothing when the window has gone.
        std::shared_ptr<char> mAlive = std::make_shared<char>(0);
    };
}

#endif // PLUS_IMPORTCONTROLLER_H
