/**
 * @file phototools.h
 * @brief Photo tools: the list of the settings the tool shows (items), what
 *        the user changed, the pins and the user's own presets
 *        (research/photo-mode-ui.md in the sl-viewer repository).
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

#ifndef PLUS_PHOTOMODE_PHOTOTOOLS_H
#define PLUS_PHOTOMODE_PHOTOTOOLS_H

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "photomode.h"

class LLControlVariable;
class LLView;

namespace plus::photomode
{
    enum class Kind
    {
        Slider,
        Check,
        Combo,
        Text    // a box to type in (the real sky's time, catalog "text"): itemText() / setItemText()
    };

    struct Option
    {
        float       value;
        std::string label;
        bool        heading = false; // a title line over a group of the choices (the real sky's places): not chosen
    };

    // A feature a row needs besides settings (catalog "needs_feature"): what it is missing is more than a
    // setting of its own (the backend, safe mode, the settings of the features it builds on).
    enum class Feature
    {
        None,
        Gtao,   // plus::gtao::unavailableReason()
        Ssgi    // plus::ssgi::unavailableReason()
    };

    // Why a row of that feature cannot be used now: the name of a text (plus::gtao::reasonText(); the
    // same pointer while the reason stays: compare it to rewrite a tip only on a change), nullptr when
    // it can (and for Feature::None). Cheap. Main thread.
    const char* featureUnavailable(Feature feature);

    // What a row of the photo tools or the photo screen keeps for its feature (Item::feature): its
    // tick calls usable(), which is false while the feature cannot be used and puts the reason in
    // front of the row's own tip on view (and on headed, under a line of head), written only when the
    // reason changes. With Feature::None it is always true and writes nothing.
    struct FeatureGate
    {
        Feature     feature = Feature::None;
        std::string tip;                    // the row's own tip, as the views were made with
        std::string head;                   // the first line of headed's tip (the row's name)
        const char* shown = nullptr;        // the reason the views' tips have now

        bool usable(LLView* view, LLView* headed = nullptr);
    };

    // A row's catalog "needs" and "needs_value" (Item): the setting is value now (-1: is not 0). True with no setting.
    bool needsMet(const LLControlVariable* control, int value);

    // One thing the tool lets the user change: a setting of the viewer
    // (app_settings/plus/phototools/catalog.xml), or one of the seven looks
    // of photomode.h (Param) that stand for one or several settings. Nothing
    // is kept besides the setting itself: the value is read back from it.
    struct Item
    {
        std::string id;      // the setting's name, or "param:brightness" ...
        Kind        kind = Kind::Slider;
        std::string tab;     // home quality light color camera subject shoot (the ids: tabName() has the names shown)
        bool        detail = false;
        int         rebuild = 0; // 0 no; 1 the pipeline or shaders are rebuilt (seconds); 2 a target may be made (unverified)
        bool        look = true; // part of a look: compared and kept in the user's presets (catalog "look" 0: the
                                 // camera's tilt of the moment, a display switch)
        std::string label;   // in the language of the viewer
        float       min = 0.f, max = 1.f, step = 1.f;
        int         digits = 0;
        float       scale = 1.f; // shown value = value * scale
        std::vector<Option> options;
        Param       param = Param::Count; // set for the seven
        std::string setting;              // set for the others
        std::string composite;            // "lod": a choice that stands for several settings (phototools.cpp);
                                          // "look": the look list (PlusLook, photolook.h), a row of its own;
                                          // "starplace": a place of the real sky (PlusSkyStarsLat / Lon);
                                          // "startime": the real sky's time in the PC's time zone (PlusSkyStarsTime)
        // The row can be used only while this setting is needs_value (-1: is not 0); greyed out otherwise
        // (catalog "needs", "needs_value": the real sky's place under "real stars").
        std::string needs;
        int         needs_value = -1;
        // And only while that feature can be used at all (catalog "needs_feature": "gtao", "ssgi"):
        // greyed out with the reason in the tip otherwise (featureUnavailable()).
        Feature     feature = Feature::None;
        std::string note;   // the explanation shown when the mouse rests on the name or the control (tip_en, tip_ja of the catalog)
        std::string words;   // more for the search to find: both languages' labels and keywords, lower case
        // The settings the item writes: what "changed" and "undo" look at.
        std::vector<std::string> keys;
    };

    // A block of the tool that is more than a setting (the sky list, the water list), for the search:
    // an entry of catalog.xml with "place" instead of "setting".
    struct Place
    {
        std::string id;      // "sky", "water"
        std::string tab;
        std::string label;   // in the language of the viewer
        std::string words;   // both languages' labels and keywords, lower case
    };

    // A title line between the rows of a tab ("Rain and Snow"): an entry of catalog.xml with "heading"
    // instead of "setting". It stands above the item "before" names, or else above the next setting of
    // the file on the same tab; only among the main rows (not in "Details", the search, the pins).
    struct Heading
    {
        std::string tab;
        std::string before;  // the id of the item it stands above
        std::string label;   // in the language of the viewer
    };

    // All items, in the order of the tabs. Home has none of its own: its
    // sliders and pins are items of the other tabs.
    const std::vector<Item>& items();
    const Item*              findItem(const std::string& id);
    const std::vector<Place>& places();
    const std::vector<Heading>& headings();
    // The name shown for a tab id. The ids are kept from the first tabs, the names are not (2026-10-22):
    // "light" is Sky & Weather, "color" Light & Color, "subject" Avatar.
    std::string              tabName(const std::string& tab); // in the language of the viewer

    // The value of an item as a number (a check box is 0 or 1; a combo box its option's value). Not for
    // the look list, whose value is a look's id (photolook.h): 0, and setting it does nothing.
    float itemValue(const Item& item);
    void  setItemValue(const Item& item, float value);
    // The value a row's "D" button resets the item to, in the same unscaled units as Item::min/max
    // (ready for setItemValue(), and for Stack::slider()'s default_value, which scales it): a Param's
    // own fixed point (photomode::paramDefault()) for the seven looks, a plain setting's shipped
    // default (LLControlVariable::getDefault()), or none for an item with no single default (several
    // settings at once: "lod", the look list; a setting that does not exist).
    std::optional<float> itemDefaultValue(const Item& item);
    // A text item's (Kind::Text) text as shown, and what was typed: false when it is not one the item
    // takes (nothing is written).
    std::string itemText(const Item& item);
    bool        setItemText(const Item& item, const std::string& text);

    // --- What was changed ---------------------------------------------------
    // The values of the settings are kept the first time the tool opens in a session; an item is
    // "changed" when any setting it writes differs.
    void captureBaseline();
    bool isChanged(const Item& item);
    // The changes counted in one look at every item: all of them, and those of each tab. A setting that
    // two items write (the exposure under "Brightness" and its own row, the look under "Color Tone" and
    // the look list) is one change, counted on the first of them in the order of items() (the sliders of
    // photomode.h come first) and on that item's tab.
    struct ChangeCounts
    {
        int                        total = 0;
        std::map<std::string, int> byTab;
        int                        inTab(const std::string& tab) const;
    };
    ChangeCounts             changeCounts();
    std::vector<const Item*> changedItems();
    void resetItem(const Item& item);
    void resetAll();
    // While the compare button is held the settings are as they were; the ones that rebuild the
    // pipeline are left alone.
    void compareBegin();
    void compareEnd();
    bool comparing(); // between the two

    // --- Pins (the favourites of the home tab) ---------------------------------
    bool                     isPinned(const std::string& id);
    std::set<std::string>    pinnedIds(); // every pin at once (for many rows: the setting read one time)
    void                    setPinned(const std::string& id, bool pinned);
    std::vector<const Item*> pinnedItems();

    // --- The user's own presets --------------------------------------------------
    constexpr int MAX_MY_PRESETS = 32;
    std::vector<std::string> myPresetNames();
    // Saves the values of every item that does not rebuild the pipeline, under the name (an old one with it is replaced).
    // False for an empty name, and for a new name when MAX_MY_PRESETS are saved.
    bool saveMyPreset(const std::string& name);
    bool applyMyPreset(const std::string& name);
    bool deleteMyPreset(const std::string& name);
}

#endif // PLUS_PHOTOMODE_PHOTOTOOLS_H
