/**
 * @file timeconvfloater.cpp
 * @brief Time converter window (research/173).
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

// How it works
// ------------
// The window holds one moment (mUtc, minutes since 1970 in UTC). Up to four rows show it as a
// wall clock in a zone: SL time (the US Pacific), UTC, and one or two places of choice (the
// clock of this PC, a named place, or UTC+N). A row's fields are year, month, day, hour and
// minute. Typing in a field reads the row; when the five fields are a real date and time, the
// row sets the moment and the other rows are rewritten. The row being typed in is not touched,
// so the cursor stays. The up/down keys and the mouse wheel step a field by one (carrying into
// the next), and a date and time pasted into any field fills the whole row.
//
// "Follow the clock" (on by default) moves the moment with the clock; any edit turns it off.
// The window draws only when it is open; each frame it compares one integer (the minute) and
// writes text only where it changed.
//
// The arithmetic is timeconvmath.h (tested alone by timeconv_test.cpp).

#include "llviewerprecompiledheaders.h"

#include "plus.h"
#include "timeconv.h"
#include "timeconvmath.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llclipboard.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "lltextbox.h"
#include "lltimer.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"

#include <cstdlib>
#include <ctime>
#include <functional>

namespace plus::timeconv
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_time_conv";

        constexpr char SETTING_ZONE2[] = "PlusTimeConvZone2";
        constexpr char SETTING_ZONE3[] = "PlusTimeConvZone3";
        constexpr char SETTING_ROWS[]  = "PlusTimeConvRows";
        constexpr char SETTING_TICK[]  = "PlusTimeConvTick";

        constexpr int MAX_ROWS = 4;
        enum Field { F_YEAR, F_MONTH, F_DAY, F_HOUR, F_MINUTE, F_COUNT };
        constexpr const char* FIELD_NAMES[F_COUNT] = { "y", "mo", "d", "h", "mi" };

        // UTC-12 .. UTC+14 in whole hours, for a place that has no name of its own.
        constexpr int FIXED_HOURS_MIN = -12, FIXED_HOURS_MAX = 14;

        // ---- the zone of a row ---------------------------------------------------------------

        struct RowZone
        {
            bool pc = false;               // the clock of this PC (its offset is asked of the OS)
            Zone zone = fixedZone("utc", 0);
        };

        // The offset of this PC from UTC at a moment, in minutes (summer time of the PC included).
        int pcOffsetAt(Minutes utc)
        {
            const std::time_t t = static_cast<std::time_t>(utc * 60);
            std::tm local{};
#ifdef _WIN32
            if (localtime_s(&local, &t) != 0)
            {
                return 0;
            }
#else
            if (!localtime_r(&t, &local))
            {
                return 0;
            }
#endif
            Civil c;
            c.year   = local.tm_year + 1900;
            c.month  = local.tm_mon + 1;
            c.day    = local.tm_mday;
            c.hour   = local.tm_hour;
            c.minute = local.tm_min;
            return static_cast<int>(wallMinutes(c) - utc);
        }

        Civil toLocal(const RowZone& rz, Minutes utc)
        {
            return rz.pc ? civilFromWallMinutes(utc + pcOffsetAt(utc)) : utcToLocal(rz.zone, utc);
        }

        Minutes toUtc(const RowZone& rz, const Civil& c, Note* note)
        {
            if (note)
            {
                *note = Note::None;
            }
            if (!rz.pc)
            {
                return localToUtc(rz.zone, c, note);
            }
            // The PC's offset depends on the moment: ask at the guess, then again at the answer.
            const Minutes w = wallMinutes(c);
            const Minutes guess = w - pcOffsetAt(w);
            return w - pcOffsetAt(guess);
        }

        int offsetAt(const RowZone& rz, Minutes utc)
        {
            return rz.pc ? pcOffsetAt(utc) : offsetMinutes(rz.zone, utc);
        }

        // "PDT", "JST", "UTC+9" : what the clock is called, for the copied text.
        std::string zoneName(const RowZone& rz, Minutes utc)
        {
            if (!rz.pc)
            {
                const std::string abbr = abbrText(rz.zone, isDst(rz.zone, utc));
                if (!abbr.empty())
                {
                    return abbr;
                }
            }
            return offsetText(offsetAt(rz, utc));
        }

        // The id of a combo item: "pc", a name of zones() or "u<minutes>" (UTC+N).
        bool resolveZone(const std::string& id, RowZone& out)
        {
            out = RowZone();
            if (id == "pc")
            {
                out.pc = true;
                return true;
            }
            if (id.size() > 1 && id[0] == 'u')
            {
                const int minutes = std::atoi(id.c_str() + 1);
                if (minutes < FIXED_HOURS_MIN * 60 || minutes > FIXED_HOURS_MAX * 60)
                {
                    return false;
                }
                out.zone = fixedZone("fixed", minutes);
                return true;
            }
            for (const Zone& z : zones())
            {
                if (id == z.id)
                {
                    out.zone = z;
                    return true;
                }
            }
            return false;
        }

        void setIfChanged(LLLineEditor* ed, const std::string& text)
        {
            if (ed->getText() != text)
            {
                ed->setText(text);
            }
        }

        void setIfChanged(LLTextBox* tb, const std::string& text)
        {
            if (tb->getText() != text)
            {
                tb->setText(text);
            }
        }

        void copyText(const std::string& text)
        {
            const LLWString w = utf8str_to_wstring(text);
            LLClipboard::instance().copyToClipboard(w, 0, static_cast<S32>(w.length()));
        }

        // ---- a field of the window -----------------------------------------------------------

        // A number field: the up/down keys and the wheel step it, a pasted date and time goes to
        // the window as a whole.
        class TcField : public LLLineEditor
        {
        public:
            // A param block of its own: the widget registry maps each Params type to one tag, and
            // LLLineEditor::Params is "line_editor" already (a second tag on it crashes at start).
            // The defaults still come from widgets/line_editor.xml through the base block.
            struct Params : public LLInitParam::Block<Params, LLLineEditor::Params> {};

            std::function<void(int)>                 onStep;  // +1 or -1
            std::function<bool(const std::string&)>  onPaste; // the clipboard's text; true: taken

            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (mask == MASK_NONE && (key == KEY_UP || key == KEY_DOWN) && onStep)
                {
                    onStep(key == KEY_UP ? 1 : -1);
                    return true;
                }
                return LLLineEditor::handleKeyHere(key, mask);
            }

            bool handleScrollWheel(S32 x, S32 y, S32 clicks) override
            {
                if (!onStep || clicks == 0)
                {
                    return LLLineEditor::handleScrollWheel(x, y, clicks);
                }
                onStep(clicks < 0 ? 1 : -1); // wheel up (negative) raises
                return true;
            }

            void paste() override
            {
                if (onPaste)
                {
                    LLWString w;
                    if (LLClipboard::instance().pasteFromClipboard(w) && onPaste(wstring_to_utf8str(w)))
                    {
                        return;
                    }
                }
                LLLineEditor::paste();
            }

        protected:
            friend class ::LLUICtrlFactory;
            explicit TcField(const Params& p) : LLLineEditor(p) {}
        };

        LLDefaultChildRegistry::Register<TcField> sTcFieldRegistration("plus_tc_field");

        // ---- the window ----------------------------------------------------------------------

        class TimeConvFloater : public LLFloater
        {
        public:
            explicit TimeConvFloater(const LLSD& key) : LLFloater(key) {}

            bool postBuild() override
            {
                mNormalColor = LLUIColor(LLColor4(1.f, 1.f, 1.f, 1.f));
                for (int r = 0; r < MAX_ROWS; ++r)
                {
                    Row& row = mRow[r];
                    row.panel = getChild<LLPanel>("row" + std::to_string(r));
                    for (int f = 0; f < F_COUNT; ++f)
                    {
                        TcField* ed = dynamic_cast<TcField*>(row.panel->getChild<LLLineEditor>(FIELD_NAMES[f]));
                        row.field[f] = ed;
                        if (f == 0 && r == 0)
                        {
                            mNormalColor = LLUIColor(ed->getFgColor());
                        }
                        ed->setPrevalidate(&LLTextValidate::validateNonNegativeS32NoSpace);
                        ed->setKeystrokeCallback([this, r](LLLineEditor*, void*) { onEdit(r); }, nullptr);
                        ed->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { onCommit(r); });
                        ed->onStep = [this, r, f](int delta) { onStep(r, f, delta); };
                        ed->onPaste = [this, r](const std::string& text) { return onPasteText(r, text); };
                    }
                    row.info = row.panel->getChild<LLTextBox>("info");
                    row.copy = row.panel->getChild<LLButton>("copy");
                    row.copy->setClickedCallback([this, r](LLUICtrl*, const LLSD&) { copyText(rowText(r)); });
                    if (r >= 2)
                    {
                        row.combo = row.panel->getChild<LLComboBox>("zone");
                        fillZoneCombo(row.combo);
                        if (!row.combo->selectByValue(LLSD(gSavedSettings.getString(r == 2 ? SETTING_ZONE2 : SETTING_ZONE3))))
                        {
                            row.combo->selectByValue(LLSD(r == 2 ? "pc" : "et"));
                        }
                        row.combo->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { onZoneChanged(r); });
                    }
                    updateRowZone(r);
                }
                // SL time and UTC rows
                mRow[0].zone.zone = zones()[ZONE_SL];
                mRow[1].zone.zone = zones()[ZONE_UTC];

                mNote    = getChild<LLTextBox>("note");
                mSummary = getChild<LLTextBox>("summary");
                mTickBox = getChild<LLCheckBoxCtrl>("tick");
                mAddRow  = getChild<LLButton>("add_row");
                mRow[3].panel->getChild<LLButton>("remove")->setClickedCallback(
                    [this](LLUICtrl*, const LLSD&) { setRows(3); });
                mAddRow->setClickedCallback([this](LLUICtrl*, const LLSD&) { setRows(4); });

                getChild<LLButton>("now")->setClickedCallback([this](LLUICtrl*, const LLSD&) { followClock(true); });
                mTickBox->setCommitCallback([this](LLUICtrl*, const LLSD& v) { followClock(v.asBoolean()); });
                getChild<LLButton>("plus_hour")->setClickedCallback([this](LLUICtrl*, const LLSD&) { shift(60); });
                getChild<LLButton>("minus_hour")->setClickedCallback([this](LLUICtrl*, const LLSD&) { shift(-60); });
                getChild<LLButton>("plus_day")->setClickedCallback([this](LLUICtrl*, const LLSD&) { shift(1440); });
                getChild<LLButton>("minus_day")->setClickedCallback([this](LLUICtrl*, const LLSD&) { shift(-1440); });
                getChild<LLButton>("copy_all")->setClickedCallback([this](LLUICtrl*, const LLSD&) { copyText(mSummary->getText()); });

                mUtc = now();
                mTick = gSavedSettings.getBOOL(SETTING_TICK);
                mTickBox->setValue(mTick);
                setRows(gSavedSettings.getS32(SETTING_ROWS) >= MAX_ROWS ? MAX_ROWS : 3);
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                if (mTick)
                {
                    mUtc = now();
                }
                refreshAll(-1);
            }

            void draw() override
            {
                if (mTick)
                {
                    const Minutes n = now();
                    if (n != mUtc)
                    {
                        mUtc = n;
                        refreshAll(-1);
                    }
                }
                LLFloater::draw();
            }

        private:
            struct Row
            {
                LLPanel*    panel = nullptr;
                TcField*    field[F_COUNT] = {};
                LLTextBox*  info  = nullptr;
                LLComboBox* combo = nullptr;
                LLButton*   copy  = nullptr;
                RowZone     zone;
            };

            static Minutes now() { return static_cast<Minutes>(time_corrected()) / 60; }

            void fillZoneCombo(LLComboBox* combo)
            {
                combo->add(getString("zone_pc"), LLSD("pc"));
                for (size_t i = 2; i < zones().size(); ++i) // after SL time and UTC
                {
                    const std::string id = zones()[i].id;
                    combo->add(getString("zone_" + id), LLSD(id));
                }
                for (int h = FIXED_HOURS_MIN; h <= FIXED_HOURS_MAX; ++h)
                {
                    combo->add(offsetText(h * 60), LLSD("u" + std::to_string(h * 60)));
                }
            }

            void updateRowZone(int r)
            {
                Row& row = mRow[r];
                if (r < 2 || !row.combo)
                {
                    return;
                }
                if (!resolveZone(row.combo->getSelectedValue().asString(), row.zone))
                {
                    resolveZone("pc", row.zone);
                }
            }

            void setRows(int rows)
            {
                mRows = rows;
                mRow[3].panel->setVisible(rows >= 4);
                mAddRow->setVisible(rows < 4);
                gSavedSettings.setS32(SETTING_ROWS, rows);
                refreshAll(-1);
            }

            void followClock(bool on)
            {
                mTick = on;
                mTickBox->setValue(on);
                gSavedSettings.setBOOL(SETTING_TICK, on);
                if (on)
                {
                    mUtc = now();
                }
                mNote->setText(std::string());
                refreshAll(-1);
            }

            void stopFollowing()
            {
                if (mTick)
                {
                    mTick = false;
                    mTickBox->setValue(false);
                }
            }

            void shift(int minutes)
            {
                stopFollowing();
                mUtc += minutes;
                mNote->setText(std::string());
                refreshAll(-1);
            }

            std::string rowName(int r)
            {
                if (r == 0) return getString("name_sl");
                if (r == 1) return "UTC";
                return mRow[r].combo ? mRow[r].combo->getSelectedItemLabel() : std::string();
            }

            // Reads the fields of a row. Returns whether they make a real date and time; marks
            // the wrong fields red when asked.
            bool readRow(int r, Civil& c, bool mark)
            {
                Row& row = mRow[r];
                int v[F_COUNT] = {};
                bool ok[F_COUNT];
                for (int f = 0; f < F_COUNT; ++f)
                {
                    const std::string t = row.field[f]->getText();
                    ok[f] = !t.empty();
                    v[f]  = ok[f] ? std::atoi(t.c_str()) : 0;
                }
                ok[F_YEAR]   = ok[F_YEAR] && v[F_YEAR] >= YEAR_MIN && v[F_YEAR] <= YEAR_MAX;
                ok[F_MONTH]  = ok[F_MONTH] && v[F_MONTH] >= 1 && v[F_MONTH] <= 12;
                const int dim = (ok[F_YEAR] && ok[F_MONTH]) ? daysInMonth(v[F_YEAR], v[F_MONTH]) : 31;
                ok[F_DAY]    = ok[F_DAY] && v[F_DAY] >= 1 && v[F_DAY] <= dim;
                ok[F_HOUR]   = ok[F_HOUR] && v[F_HOUR] >= 0 && v[F_HOUR] <= 23;
                ok[F_MINUTE] = ok[F_MINUTE] && v[F_MINUTE] >= 0 && v[F_MINUTE] <= 59;
                bool all = true;
                for (int f = 0; f < F_COUNT; ++f)
                {
                    all = all && ok[f];
                    if (mark)
                    {
                        row.field[f]->setFgColor(ok[f] ? mNormalColor : LLUIColor(LLColor4(1.f, 0.35f, 0.35f, 1.f)));
                    }
                }
                if (all)
                {
                    c.year = v[F_YEAR];
                    c.month = v[F_MONTH];
                    c.day = v[F_DAY];
                    c.hour = v[F_HOUR];
                    c.minute = v[F_MINUTE];
                }
                return all;
            }

            // A wall clock in the row, as the moment. Turns the clock off and rewrites the other rows.
            void applyRow(int r, const Civil& c, bool rewriteThisRow)
            {
                stopFollowing();
                Note note = Note::None;
                mUtc = toUtc(mRow[r].zone, c, &note);
                mNote->setText(std::string());
                if (note != Note::None)
                {
                    LLStringUtil::format_map_t args;
                    args["[WHERE]"] = rowName(r);
                    args["[TIME]"]  = two(c.hour) + ":" + two(c.minute);
                    const Civil fixed = toLocal(mRow[r].zone, mUtc);
                    args["[FIXED]"] = two(fixed.hour) + ":" + two(fixed.minute);
                    mNote->setText(getString(note == Note::Gap ? "note_gap" : "note_overlap", args));
                }
                // While typing, the row stays as typed even when its clock does not exist (02:30 at the
                // start of summer time): the commit (leaving the field) writes it as 03:30.
                refreshAll(rewriteThisRow ? -1 : r);
            }

            void onEdit(int r)
            {
                Civil c;
                if (!readRow(r, c, true))
                {
                    return; // half typed, or wrong: the others stay as they are
                }
                applyRow(r, c, false);
            }

            void onCommit(int r)
            {
                Civil c;
                if (!readRow(r, c, true))
                {
                    mNote->setText(getString("note_range"));
                    return;
                }
                applyRow(r, c, true); // also writes this row whole ("7" becomes "07")
            }

            void onStep(int r, int field, int delta)
            {
                Civil c;
                if (!readRow(r, c, false))
                {
                    c = toLocal(mRow[r].zone, mUtc);
                }
                switch (field)
                {
                case F_YEAR:   c = addMonths(c, 12 * delta); break;
                case F_MONTH:  c = addMonths(c, delta); break;
                case F_DAY:    c = addWallMinutes(c, 1440LL * delta); break;
                case F_HOUR:   c = addWallMinutes(c, 60LL * delta); break;
                default:       c = addWallMinutes(c, delta); break;
                }
                if (!validCivil(c))
                {
                    return; // past 1970 or 2100
                }
                applyRow(r, c, true);
            }

            bool onPasteText(int r, const std::string& text)
            {
                Parsed p;
                if (!parseDateTime(text, p))
                {
                    return false;
                }
                if (!p.hasDate && p.numbers < 2 && !p.ampm)
                {
                    return false; // one plain number: an ordinary paste into the field
                }
                Civil c;
                if (!readRow(r, c, false))
                {
                    c = toLocal(mRow[r].zone, mUtc);
                }
                if (p.hasDate)
                {
                    c.year = p.year;
                    c.month = p.month;
                    c.day = p.day;
                }
                if (p.hasTime)
                {
                    c.hour = p.hour;
                    c.minute = p.minute;
                }
                if (!validCivil(c))
                {
                    return false;
                }
                applyRow(r, c, true);
                return true;
            }

            void onZoneChanged(int r)
            {
                updateRowZone(r);
                gSavedSettings.setString(r == 2 ? SETTING_ZONE2 : SETTING_ZONE3, mRow[r].combo->getSelectedValue().asString());
                mNote->setText(std::string());
                refreshAll(-1);
            }

            std::string infoText(int r)
            {
                const RowZone& rz = mRow[r].zone;
                if (r == 1)
                {
                    return "UTC";
                }
                const int off = offsetAt(rz, mUtc);
                if (rz.pc)
                {
                    return offsetText(off) + " (" + getString("info_pc") + ")";
                }
                const std::string abbr = abbrText(rz.zone, isDst(rz.zone, mUtc));
                const std::string ot   = offsetText(off);
                return abbr.empty() || abbr == ot ? ot : abbr + " " + ot;
            }

            // "2026-10-24 23:00 JST"
            std::string rowText(int r)
            {
                return civilText(toLocal(mRow[r].zone, mUtc)) + " " + zoneName(mRow[r].zone, mUtc);
            }

            void refreshRow(int r, bool writeFields)
            {
                Row& row = mRow[r];
                if (writeFields)
                {
                    const Civil c = toLocal(row.zone, mUtc);
                    setIfChanged(row.field[F_YEAR],   std::to_string(c.year));
                    setIfChanged(row.field[F_MONTH],  two(c.month));
                    setIfChanged(row.field[F_DAY],    two(c.day));
                    setIfChanged(row.field[F_HOUR],   two(c.hour));
                    setIfChanged(row.field[F_MINUTE], two(c.minute));
                    for (int f = 0; f < F_COUNT; ++f)
                    {
                        row.field[f]->setFgColor(mNormalColor);
                    }
                }
                setIfChanged(row.info, infoText(r));
            }

            // "SL 2026/10/24 07:00 PDT = UTC 14:00 = JST 23:00" (a date after the time when it differs)
            void updateSummary()
            {
                std::string& s = mSummaryBuf; // kept, so the text is built in the same memory each time
                s.clear();
                int baseMonth = 0, baseDay = 0;
                for (int r = 0; r < mRows; ++r)
                {
                    const Civil c = toLocal(mRow[r].zone, mUtc);
                    if (r == 0)
                    {
                        baseMonth = c.month;
                        baseDay = c.day;
                        s = getString("name_sl") + " " + std::to_string(c.year) + "/" + two(c.month) + "/" + two(c.day) + " " +
                            two(c.hour) + ":" + two(c.minute) + " " + zoneName(mRow[r].zone, mUtc);
                        continue;
                    }
                    s += " = " + zoneName(mRow[r].zone, mUtc) + " " + two(c.hour) + ":" + two(c.minute);
                    if (c.month != baseMonth || c.day != baseDay)
                    {
                        s += " (" + std::to_string(c.month) + "/" + std::to_string(c.day) + ")";
                    }
                }
                setIfChanged(mSummary, s);
            }

            // skipRow: the row being typed in, whose fields are left alone.
            void refreshAll(int skipRow)
            {
                for (int r = 0; r < mRows; ++r)
                {
                    refreshRow(r, r != skipRow);
                }
                updateSummary();
            }

            Row         mRow[MAX_ROWS];
            int         mRows = 3;
            Minutes     mUtc  = 0;
            bool        mTick = true;
            LLTextBox*  mNote    = nullptr;
            LLTextBox*  mSummary = nullptr;
            LLCheckBoxCtrl* mTickBox = nullptr;
            LLButton*   mAddRow  = nullptr;
            LLUIColor   mNormalColor;
            std::string mSummaryBuf;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_time_conv.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<TimeConvFloater>);
        }

        plus::FeatureRegistration sRegistration({ "timeconv", &init, {} });
    }

    void setupStatusBarClock(LLTextBox* clock)
    {
        clock->setClickedCallback([](void*) { LLFloaterReg::toggleInstanceOrBringToFront(LLSD(FLOATER_NAME)); }, nullptr);
    }

    void addClockTooltipHint(LLTextBox* clock)
    {
        clock->setToolTip(clock->getToolTip() + "\n" + LLTrans::getString("PlusTimeConvClockHint"));
    }
}
