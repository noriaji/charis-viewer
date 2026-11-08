/**
 * @file phototools.cpp
 * @brief Photo tools: the items, what was changed, the pins and the user's own presets.
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

// The list of settings is a data file (app_settings/plus/phototools/catalog.xml), so a setting
// can be added, moved to another tab or marked as one that rebuilds the pipeline without a
// build. The tool keeps no values of its own: every item reads and writes the viewer's own
// settings (gSavedSettings), so the settings of the Preferences window and of the Charis menu
// stay what they are. Only the pins and the user's presets are saved (PlusPhotoPins,
// PlusPhotoMyPresets).

#include "llviewerprecompiledheaders.h"

#include "phototools.h"
#include "photorecipe.h"
#include "plus.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "lldir.h"
#include "llevents.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llthread.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llview.h"
#include "plus/gtao/gtao.h"
#include "plus/gtao/gtao_ui.h"
#include "plus/posermcp/settingstools.h"
#include "plus/presets/presets.h"
#include "plus/realstars/realstars.h"
#include "plus/ssgi/ssgi.h"

namespace plus::photomode
{
    namespace
    {
        constexpr char CATALOG_FILE[]     = "plus/phototools/catalog.xml"; // in app_settings/
        constexpr char SETTING_PINS[]     = "PlusPhotoPins";
        constexpr char SETTING_PRESETS[]  = "PlusPhotoMyPresets";
        // a volume factor up to this counts as "normal", above it "gentle": the quality levels' own (Firestorm's
        // 2.0 at most, featuretable.txt), so that every level is "normal" (the detail goes up only for the photos)
        constexpr double NORMAL_LOD_MAX   = 2.0;
        constexpr double GENTLE_LOD       = 3.0;  // what "gentle" sets (the viewer takes at most 4)
        constexpr double LOD_TOLERANCE    = 0.001; // a factor read back as 2.0000001 is still "normal"

        bool gentleLod(double factor)
        {
            return factor > NORMAL_LOD_MAX + LOD_TOLERANCE;
        }

        bool japanese()
        {
            return LLUI::getLanguage() == "ja";
        }

        const std::string& pick(const std::string& en, const std::string& ja)
        {
            return (japanese() && !ja.empty()) ? ja : en;
        }

        // What the search looks in besides the label shown: the labels and keywords of both languages
        // (so "water" finds the water in Japanese too, and "海" the sea of the water list).
        std::string searchWords(const LLSD& entry)
        {
            std::string words = entry["label_en"].asString() + " | " + entry["label_ja"].asString() + " | " +
                                entry["keywords_en"].asString() + " | " + entry["keywords_ja"].asString();
            LLStringUtil::toLower(words);
            return words;
        }

        // The catalog's "needs_feature" (phototools.h Feature); an unknown name (of another version) needs nothing.
        Feature featureOf(const LLSD& entry)
        {
            const std::string name = entry["needs_feature"].asString();
            return name == "gtao" ? Feature::Gtao : name == "ssgi" ? Feature::Ssgi : Feature::None;
        }

        std::vector<Place>   sPlaces;   // filled with the items (buildItems())
        std::vector<Heading> sHeadings; // ... and so are these

        struct ParamDef
        {
            Param       param;
            const char* id;
            const char* tab;
            const char* en;
            const char* ja;
            std::vector<std::string> keys;
            const char* tip_en;
            const char* tip_ja;
        };

        // In the order of their rows on the Light & Color tab ("color"): the light (direction, shadows, glare),
        // then the brightness and the colors. The catalog's lens flare and star rays stand before the
        // brightness ("before" in catalog.xml), after the glare.
        const std::vector<ParamDef>& paramDefs()
        {
            static const std::vector<ParamDef> defs = {
                { Param::Sun,        "param:sun",        "color",  "Light Direction",  "光の向き",     {}, // the sky, not a setting
                  "0: the sun behind the camera (front light). 180: the sun in front of the camera (backlight). Set from where the camera looks now.",
                  "0 度は太陽がカメラの後ろ（順光）、180 度は正面（逆光）です。今カメラが向いている方角から数えます。" },
                { Param::Shadow,     "param:shadow",     "color",  "Shadow Depth",     "影の濃さ",     { "RenderSkyAmbientScale" },
                  "Raise it for darker, more contrasty shadows. Lower it to lift the shadows and make them soft.",
                  "上げると影が濃く、メリハリが出ます。下げると影が明るく浮いて、やわらかい絵になります。" },
                { Param::Glare,      "param:glare",      "color",  "Glare",            "グレア",       { "PlusGlare" },
                  "The glow around bright lights. 0: off, 1: weak, 2: medium, 3: strong. Strong glare can wash out a bright scene.",
                  "明るい光のまわりのにじみです。0 でなし、1 弱、2 中、3 強。強いと明るい場面は白っぽくなります。" },
                { Param::Brightness, "param:brightness", "color",  "Brightness",       "明るさ",       { "RenderExposure" },
                  "Raise it for a brighter picture, lower it for a darker one.",
                  "上げると全体が明るく、下げると暗くなります。白飛びや黒つぶれが出ない範囲で。" },
                { Param::Contrast,   "param:contrast",   "color",  "Contrast",         "コントラスト", { "RenderTonemapMix" },
                  "Raise it for a stronger difference between light and dark. Lower it for a flat, soft picture.",
                  "上げると明暗の差がはっきりして力強く、下げると平らでやわらかい絵になります。" },
                { Param::Tint,       "param:tint",       "color",  "Color Tone",       "色味",         { "PlusLook", "PlusLookStrength" },
                  "Left: cool and bluish. Right: warm and reddish. The middle leaves the colors as they are.",
                  "左は青みの寒色、右は赤みの暖色にします。真ん中なら色はそのままです。" },
                { Param::Blur,       "param:blur",       "camera", "Background Blur",  "背景ぼかし",   { "CameraFNumber" },
                  "Raise it to blur the background more and make the subject stand out. Needs Depth of Field (on the Camera tab).",
                  "上げるほど背景が大きくぼけて、被写体が引き立ちます。「被写界深度」をオンにしている時だけ使えます（カメラのタブ）。" },
            };
            return defs;
        }

        std::vector<Item> buildItems()
        {
            std::vector<Item> items;

            for (const ParamDef& def : paramDefs())
            {
                Item item;
                item.id     = def.id;
                item.kind   = Kind::Slider;
                item.tab    = def.tab;
                item.label  = pick(def.en, def.ja);
                const ParamRange& limits = range(def.param);
                item.min    = limits.min;
                item.max    = limits.max;
                item.step   = 1.f;
                item.digits = 0;
                item.param  = def.param;
                item.keys   = def.keys;
                item.note   = pick(def.tip_en, def.tip_ja);
                items.push_back(item);
            }

            // Detail (LOD): normal / not by distance / gentle. Three ways to set RenderDynamicLOD and
            // RenderVolumeLODFactor (research/photo-tools-settings.md). Not rebuilding anything: both
            // settings are read where the level of detail is worked out, the next time it is.
            {
                Item item;
                item.id        = "composite:lod";
                item.composite = "lod";
                item.kind      = Kind::Combo;
                item.tab       = "quality";
                item.label     = pick("Detail", "ディテール");
                item.options   = { { 0.f, pick("Normal", "通常") },
                                   { 1.f, pick("Not by distance", "距離で変えない") },
                                   { 2.f, pick("Gentle", "緩やか") } };
                item.keys      = { "RenderDynamicLOD", "RenderVolumeLODFactor" };
                item.note      = pick("Not by distance: things keep the detail they have close up when the camera moves away. Gentle: the detail drops at a longer distance. Both draw more, so the picture can get slower.",
                                      "距離で変えない: カメラを離しても、近くで見た時の細かさのままにします。緩やか: 遠くまで細かいまま。どちらも細かいまま描くので、重くなることがあります。");
                if (gSavedSettings.controlExists("RenderDynamicLOD") && gSavedSettings.controlExists("RenderVolumeLODFactor"))
                {
                    items.push_back(item);
                }
            }

            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, CATALOG_FILE);
            llifstream        file(path.c_str());
            LLSD              catalog;
            // (a catalog nested too deep is not read: the official reader recurses as it frees one, fix-round101)
            if (!file.is_open() || !plus::xmlNestingOk(path) || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(catalog, file)
                || !catalog.isArray())
            {
                LL_WARNS("Plus") << "Cannot read " << path << "; the photo tools show their own sliders only" << LL_ENDL;
                return items;
            }
            std::vector<std::pair<std::string, std::string>> moves;
            std::vector<size_t> waiting; // headings without "before": they take the next setting of their tab
            for (const LLSD& entry : llsd::inArray(catalog))
            {
                if (entry.has("heading"))
                {
                    Heading heading;
                    heading.tab    = entry["tab"].asString();
                    heading.before = entry["before"].asString();
                    heading.label  = pick(entry["label_en"].asString(), entry["label_ja"].asString());
                    if (heading.before.empty())
                    {
                        waiting.push_back(sHeadings.size());
                    }
                    sHeadings.push_back(heading);
                    continue;
                }
                if (entry.has("composite"))
                {
                    // A row that is more than one setting (the real sky's place and time): its kind and
                    // its settings ("keys") from the file, its choices and its reading and writing here.
                    Item item;
                    item.composite = entry["composite"].asString();
                    item.id        = "composite:" + item.composite;
                    for (const LLSD& key : llsd::inArray(entry["keys"]))
                    {
                        item.keys.push_back(key.asString());
                    }
                    const bool known   = item.composite == "starplace" || item.composite == "startime";
                    const bool present = !item.keys.empty() && std::all_of(item.keys.begin(), item.keys.end(),
                        [](const std::string& key) { return gSavedSettings.controlExists(key); });
                    if (!known || !present)
                    {
                        continue; // of another version
                    }
                    item.kind        = item.composite == "startime" ? Kind::Text : Kind::Combo;
                    item.tab         = entry["tab"].asString();
                    item.detail      = entry["detail"].asBoolean();
                    item.look        = !entry.has("look") || entry["look"].asBoolean();
                    item.label       = pick(entry["label_en"].asString(), entry["label_ja"].asString());
                    item.words       = searchWords(entry);
                    item.note        = pick(entry["tip_en"].asString(), entry["tip_ja"].asString());
                    item.needs       = entry["needs"].asString();
                    item.needs_value = entry.has("needs_value") ? entry["needs_value"].asInteger() : -1;
                    item.feature     = featureOf(entry);
                    if (item.composite == "starplace")
                    {
                        // by region, a heading line over each (a value of its own below 0, not chosen)
                        size_t count = 0;
                        const plus::realstars::Place* places = plus::realstars::places(count);
                        int region = -1;
                        for (size_t i = 0; i < count; ++i)
                        {
                            if ((int)places[i].region != region)
                            {
                                region = (int)places[i].region;
                                const plus::realstars::RegionName& name = plus::realstars::REGIONS[region];
                                item.options.push_back({ (float)(-1 - region), "― " + pick(name.en, name.ja) + " ―", true });
                            }
                            item.options.push_back({ (float)i, pick(places[i].en, places[i].ja) });
                        }
                        // last: the latitude and longitude rows as they are set (by hand)
                        item.options.push_back({ (float)count, pick("Custom (latitude, longitude)", "自分で指定（緯度・経度）") });
                    }
                    items.push_back(item);
                    if (!entry["before"].asString().empty())
                    {
                        moves.emplace_back(item.id, entry["before"].asString());
                    }
                    continue;
                }
                if (entry.has("place"))
                {
                    Place place;
                    place.id    = entry["place"].asString();
                    place.tab   = entry["tab"].asString();
                    place.label = pick(entry["label_en"].asString(), entry["label_ja"].asString());
                    place.words = searchWords(entry);
                    sPlaces.push_back(place);
                    continue;
                }
                const std::string setting = entry["setting"].asString();
                if (setting.empty() || !gSavedSettings.controlExists(setting))
                {
                    continue; // a setting of another version
                }
                Item item;
                item.id      = setting;
                item.setting = setting;
                item.keys    = { setting };
                const std::string kind = entry["kind"].asString();
                item.kind    = kind == "check" ? Kind::Check : ((kind == "combo" || kind == "look") ? Kind::Combo : Kind::Slider);
                item.needs   = entry["needs"].asString();
                item.needs_value = entry.has("needs_value") ? entry["needs_value"].asInteger() : -1;
                item.feature = featureOf(entry);
                if (kind == "look")
                {
                    item.composite = "look"; // its choices are the looks of the folders, not options of the file
                }
                item.tab     = entry["tab"].asString();
                item.detail  = entry["detail"].asBoolean();
                item.rebuild = entry["rebuild"].asInteger();
                item.look    = !entry.has("look") || entry["look"].asBoolean();
                item.label   = pick(entry["label_en"].asString(), entry["label_ja"].asString());
                item.words   = searchWords(entry);
                item.note    = pick(entry["tip_en"].asString(), entry["tip_ja"].asString()); // empty: no explanation
                item.min     = (float)entry["min"].asReal();
                item.max     = entry.has("max") ? (float)entry["max"].asReal() : 1.f;
                item.step    = entry.has("step") ? (float)entry["step"].asReal() : 1.f;
                // LLSliderCtrl::setPrecision() stops the viewer (LL_ERRS) outside 0..10
                item.digits  = llclamp((int)entry["digits"].asInteger(), 0, 10);
                item.scale   = entry.has("scale") ? (float)entry["scale"].asReal() : 1.f;
                // fix-round101: numbers the sliders can use (the shipped file's all are): the floater divides by the
                // scale, and LLSlider takes fmod() by the step (0 wrote NaN into the setting)
                if (!std::isfinite(item.scale) || item.scale == 0.f)
                {
                    item.scale = 1.f;
                }
                item.min = std::isfinite(item.min) ? item.min : 0.f;
                item.max = std::isfinite(item.max) ? llmax(item.max, item.min) : item.min;
                if (!std::isfinite(item.step) || item.step <= 0.f)
                {
                    item.step = (item.max - item.min) / 100.f;
                    if (!std::isfinite(item.step) || item.step <= 0.f)
                    {
                        item.step = 1.f;
                    }
                }
                for (const LLSD& option : llsd::inArray(entry["options"]))
                {
                    item.options.push_back({ (float)std::atof(option["value"].asString().c_str()),
                                             pick(option["label_en"].asString(), option["label_ja"].asString()) });
                }
                if (item.kind == Kind::Combo && item.options.empty() && item.composite.empty())
                {
                    continue;
                }
                items.push_back(item);
                for (auto at = waiting.begin(); at != waiting.end();)
                {
                    if (sHeadings[*at].tab == item.tab)
                    {
                        sHeadings[*at].before = item.id;
                        at = waiting.erase(at);
                    }
                    else
                    {
                        ++at;
                    }
                }
                if (!entry["before"].asString().empty())
                {
                    moves.emplace_back(item.id, entry["before"].asString());
                }
            }
            // an entry can ask to stand before another row (the depth of field switch stands above the blur)
            for (const auto& [id, before] : moves)
            {
                const auto from = std::find_if(items.begin(), items.end(), [&id](const Item& item) { return item.id == id; });
                const auto to   = std::find_if(items.begin(), items.end(), [&before](const Item& item) { return item.id == before; });
                if (from != items.end() && to != items.end() && from > to)
                {
                    Item moved = *from;
                    items.erase(from);
                    const auto at = std::find_if(items.begin(), items.end(), [&before](const Item& item) { return item.id == before; });
                    items.insert(at, moved);
                }
            }
            return items;
        }

        LLSD currentOf(const std::string& key)
        {
            LLControlVariable* control = gSavedSettings.getControl(key);
            return control ? control->getValue() : LLSD();
        }

        void writeTo(const std::string& key, const LLSD& value)
        {
            if (LLControlVariable* control = gSavedSettings.getControl(key))
            {
                control->setValue(value);
            }
        }

        LLSD savedOf(const std::string& key)
        {
            LLControlVariable* control = gSavedSettings.getControl(key);
            return control ? control->getSaveValue() : LLSD();
        }

        // fix-round87: puts back a value in use and a saved value remembered together. A value in use
        // that was not the saved one (an unsaved value: a test value of the MCP tools, posermcp/settingstools.cpp)
        // goes back as an unsaved value again, so it never becomes the user's saved setting.
        void putBack(const std::string& key, const LLSD& value, const LLSD& saved)
        {
            LLControlVariable* control = gSavedSettings.getControl(key);
            if (!control)
            {
                return;
            }
            if (llsd_equals(value, saved))
            {
                control->setValue(value); // the plain case: the saved value, no unsaved one over it
                return;
            }
            if (!llsd_equals(control->getSaveValue(), saved))
            {
                control->setValue(saved);
            }
            control->setValue(value, false);
        }

        std::map<std::string, LLSD> sBaseline;
        std::map<std::string, LLSD> sBaselineSaved;   // the saved values then (fix-round87)
        bool                        sBaselineTaken = false;

        void putBackBaseline(const std::string& key)
        {
            const auto it = sBaseline.find(key);
            if (it == sBaseline.end())
            {
                return;
            }
            const auto saved = sBaselineSaved.find(key);
            putBack(key, it->second, saved != sBaselineSaved.end() ? saved->second : it->second);
        }

        // The settings of the items that do not rebuild the pipeline: what compare and the
        // user's presets move. Not the items that are no part of a look (Item::look).
        std::vector<std::string> quietKeys()
        {
            std::set<std::string> loud, quiet;
            for (const Item& item : items())
            {
                for (const std::string& key : item.keys)
                {
                    (item.rebuild > 0 || !item.look ? loud : quiet).insert(key);
                }
            }
            std::vector<std::string> keys;
            for (const std::string& key : quiet)
            {
                if (!loud.count(key))
                {
                    keys.push_back(key);
                }
            }
            return keys;
        }

        std::map<std::string, LLSD> sCompareKept;      // the user's values, given back at the end
        std::map<std::string, LLSD> sCompareKeptSaved; // their saved values (fix-round87)
        std::map<std::string, LLSD> sCompareWritten;   // what the compare wrote in their place
        std::set<std::string>       sCompareWasTest;   // kept values that were the MCP tools' test values (fix-round100)
        bool                        sComparing = false;
    }

    const std::vector<Item>& items()
    {
        static const std::vector<Item> sItems = buildItems();
        return sItems;
    }

    const std::vector<Place>& places()
    {
        items(); // read with them
        return sPlaces;
    }

    const std::vector<Heading>& headings()
    {
        items(); // read with them
        return sHeadings;
    }

    const Item* findItem(const std::string& id)
    {
        for (const Item& item : items())
        {
            if (item.id == id)
            {
                return &item;
            }
        }
        return nullptr;
    }

    const char* featureUnavailable(Feature feature)
    {
        switch (feature)
        {
            case Feature::Gtao:
                return plus::gtao::unavailableReason();
            case Feature::Ssgi:
                return plus::ssgi::unavailableReason();
            default:
                return nullptr;
        }
    }

    bool needsMet(const LLControlVariable* control, int value)
    {
        if (!control)
        {
            return true;
        }
        const int now = control->getValue().asInteger();
        return value < 0 ? now != 0 : now == value;
    }

    bool FeatureGate::usable(LLView* view, LLView* headed)
    {
        if (feature == Feature::None)
        {
            return true;
        }
        const char* reason = featureUnavailable(feature);
        if (reason != shown)
        {
            shown = reason;
            const std::string now = plus::gtao::tipWithReason(reason, tip);
            if (view)
            {
                view->setToolTip(now);
            }
            if (headed)
            {
                headed->setToolTip(head.empty() || now.empty() ? head + now : head + "\n" + now);
            }
        }
        return !reason;
    }

    std::string tabName(const std::string& tab)
    {
        struct Name { const char* id; const char* en; const char* ja; };
        static const Name names[] = {
            // the ids are kept from the first tabs (catalog.xml, TABS of phototoolsfloater.cpp); the names changed
            { "home", "Home", "ホーム" }, { "quality", "Quality", "画質" }, { "light", "Sky & Weather", "空と天気" },
            { "color", "Light & Color", "光と色" }, { "camera", "Camera", "カメラ" }, { "subject", "Avatar", "アバター" },
            { "shoot", "Shoot", "撮影" }
        };
        for (const Name& name : names)
        {
            if (tab == name.id)
            {
                return japanese() ? name.ja : name.en;
            }
        }
        return tab;
    }

    float itemValue(const Item& item)
    {
        if (item.composite == "look")
        {
            return 0.f; // a look's id, not a number (photolook.h)
        }
        if (item.composite == "lod")
        {
            if (!currentOf("RenderDynamicLOD").asBoolean())
            {
                return 1.f;
            }
            return gentleLod(currentOf("RenderVolumeLODFactor").asReal()) ? 2.f : 0.f;
        }
        if (item.composite == "starplace")
        {
            // the place whose latitude and longitude the settings are, or the last choice (custom):
            // a hand-typed latitude or longitude shows "Custom", a place chosen shows its values
            return (float)plus::realstars::placeIndexOf(currentOf("PlusSkyStarsLat").asReal(),
                                                        currentOf("PlusSkyStarsLon").asReal());
        }
        if (item.kind == Kind::Text)
        {
            return 0.f; // a text (itemText)
        }
        if (item.param != Param::Count)
        {
            return value(item.param);
        }
        return (float)currentOf(item.setting).asReal();
    }

    std::string itemText(const Item& item)
    {
        if (item.composite == "startime")
        {
            return plus::realstars::localTextOf(currentOf("PlusSkyStarsTime").asString());
        }
        return currentOf(item.setting).asString();
    }

    bool setItemText(const Item& item, const std::string& text)
    {
        if (item.composite == "startime")
        {
            std::string utc;
            if (!plus::realstars::utcOfLocalText(text, utc))
            {
                return false;
            }
            writeTo("PlusSkyStarsTime", LLSD(utc));
            return true;
        }
        return false;
    }

    void setItemValue(const Item& item, float v)
    {
        if (item.composite == "look")
        {
            return; // set by its own row (applyLook)
        }
        if (item.composite == "lod")
        {
            const int choice = (int)ll_round(v);
            const auto original = sBaseline.find("RenderVolumeLODFactor");
            if (choice == 1)
            {
                writeTo("RenderDynamicLOD", LLSD(false));
            }
            else if (choice == 2)
            {
                writeTo("RenderDynamicLOD", LLSD(true));
                if (!gentleLod(currentOf("RenderVolumeLODFactor").asReal()))
                {
                    writeTo("RenderVolumeLODFactor", LLSD((LLSD::Real)GENTLE_LOD));
                }
            }
            else
            {
                // normal: dynamic again, the factor as it was before the tool; without one, or when that was already a
                // gentle one, the quality level's own (at most NORMAL_LOD_MAX)
                writeTo("RenderDynamicLOD", LLSD(true));
                if (original != sBaseline.end() && !gentleLod(original->second.asReal()))
                {
                    writeTo("RenderVolumeLODFactor", original->second);
                }
                else
                {
                    const double level = plus::presets::graphicsLevelValue("RenderVolumeLODFactor", 1.f);
                    writeTo("RenderVolumeLODFactor", LLSD((LLSD::Real)llclamp(level, 0.0, NORMAL_LOD_MAX)));
                }
            }
            return;
        }
        if (item.composite == "starplace")
        {
            size_t count = 0;
            const plus::realstars::Place* places = plus::realstars::places(count);
            const int choice = (int)ll_round(v);
            if (choice >= 0 && (size_t)choice < count)
            {
                writeTo("PlusSkyStarsLat", LLSD((LLSD::Real)places[choice].lat));
                writeTo("PlusSkyStarsLon", LLSD((LLSD::Real)places[choice].lon));
            }
            // the rows of the latitude and longitude show the values at the next refresh (refreshRows:
            // only what changed is set). "Custom" and the headings (below 0) write nothing.
            return;
        }
        if (item.kind == Kind::Text)
        {
            return; // set by setItemText()
        }
        if (item.param != Param::Count)
        {
            plus::photomode::setValue(item.param, v);
            return;
        }
        LLControlVariable* control = gSavedSettings.getControl(item.setting);
        if (!control)
        {
            return;
        }
        if (control->isType(TYPE_BOOLEAN))
        {
            control->setValue(LLSD(v > 0.5f));
        }
        else if (control->isType(TYPE_F32))
        {
            control->setValue(LLSD((LLSD::Real)v));
        }
        else
        {
            control->setValue(LLSD((LLSD::Integer)ll_round(v)));
        }
    }

    std::optional<float> itemDefaultValue(const Item& item)
    {
        if (!item.composite.empty() || item.kind == Kind::Text)
        {
            return std::nullopt;   // several settings at once ("lod", the look list, a real place), or typed text
        }
        if (item.param != Param::Count)
        {
            const float v = paramDefault(item.param);
            return v == UNSET ? std::optional<float>() : std::optional<float>(v);
        }
        if (item.setting.empty())
        {
            return std::nullopt;
        }
        LLControlVariable* control = gSavedSettings.getControl(item.setting);
        return control ? std::optional<float>(float(control->getDefault().asReal())) : std::nullopt;
    }

    // --- What was changed ---------------------------------------------------

    void captureBaseline()
    {
        if (sBaselineTaken)
        {
            return;
        }
        sBaselineTaken = true;
        rememberRecipesAsOpened(); // what a recipe wrote before this is part of the values as the tools opened
        for (const Item& item : items())
        {
            for (const std::string& key : item.keys)
            {
                if (!sBaseline.count(key))
                {
                    // fix-round100: a test value of the MCP tools in use is not the user's: their saved
                    // value is (the MCP takes the test value back when it stops; a reset after that
                    // would bring it back otherwise).
                    const LLSD current  = currentOf(key);
                    sBaselineSaved[key] = savedOf(key);
                    sBaseline[key]      = plus::posermcp::isTestValue(key, current) ? sBaselineSaved[key] : current;
                }
            }
        }
    }

    namespace
    {
        // The settings of an item that differ from the baseline.
        std::vector<std::string> changedKeys(const Item& item)
        {
            std::vector<std::string> changed;
            for (const std::string& key : item.keys)
            {
                const auto it = sBaseline.find(key);
                if (it != sBaseline.end() && !llsd_equals(currentOf(key), it->second))
                {
                    changed.push_back(key);
                }
            }
            return changed;
        }
    }

    bool isChanged(const Item& item)
    {
        if (item.param == Param::Sun)
        {
            return sunChanged(); // the sky, not a setting
        }
        if (changedKeys(item).empty())
        {
            return false;
        }
        if (item.param == Param::Tint)
        {
            // The tint writes PlusLook, which the look list writes too: a look chosen there is no tint
            // (the slider reads 0 before and after). Changed only when one of the tint's own looks is in it
            // now or was at the start.
            const auto base = sBaseline.find("PlusLook");
            return isTintLook(currentOf("PlusLook").asString()) ||
                   (base != sBaseline.end() && isTintLook(base->second.asString()));
        }
        return true;
    }

    int ChangeCounts::inTab(const std::string& tab) const
    {
        const auto it = byTab.find(tab);
        return it != byTab.end() ? it->second : 0;
    }

    ChangeCounts changeCounts()
    {
        // One change for each setting that differs, not for each item that shows it: an item counts only
        // when it has a changed setting no item before it counted (the sun has no setting: its own id).
        ChangeCounts          counts;
        std::set<std::string> counted;
        for (const Item& item : items())
        {
            if (!isChanged(item))
            {
                continue;
            }
            std::vector<std::string> keys = changedKeys(item);
            if (keys.empty())
            {
                keys.push_back(item.id);
            }
            bool fresh = false;
            for (const std::string& key : keys)
            {
                fresh = counted.insert(key).second || fresh;
            }
            if (fresh)
            {
                ++counts.total;
                ++counts.byTab[item.tab];
            }
        }
        return counts;
    }

    std::vector<const Item*> changedItems()
    {
        std::vector<const Item*> changed;
        for (const Item& item : items())
        {
            if (isChanged(item))
            {
                changed.push_back(&item);
            }
        }
        return changed;
    }

    void resetItem(const Item& item)
    {
        if (item.param == Param::Sun)
        {
            restoreSun();
            return;
        }
        for (const std::string& key : item.keys)
        {
            putBackBaseline(key);
        }
    }

    void resetAll()
    {
        // what a preset set besides the rows (the screen effects of a look): back first, the rows' baseline over it
        givePresetSettingsBack();
        // and what a one-press recipe wrote after the tools opened (research/253 9.3): back. What it wrote before
        // stays its own to give back when it is turned off
        letGoOfRecipes([]
        {
            for (const auto& entry : sBaseline)
            {
                putBackBaseline(entry.first);
            }
        });
        // The light direction is the personal sky, not a setting: the presets that turn the sun froze it
        // (a day cycle stopped, the land's sky copied into a personal one). The environment as it was.
        restoreSun();
        gSavedSettings.setString("PlusPhotoPreset", "");
    }

    void compareBegin()
    {
        if (sComparing)
        {
            return;
        }
        // A compare still on when the viewer quits (a disconnection, the main window closed) ends
        // before the settings are saved (LLAppViewer::cleanup()): the compare's values are not kept.
        static bool sQuitHooked = false;
        if (!sQuitHooked)
        {
            sQuitHooked = true;
            LLEventPumps::instance().obtain("LLApp").listen("PlusPhotoCompare", [](const LLSD& event)
            {
                if (event["status"].asString() == "quitting" && on_main_thread())
                {
                    compareEnd();
                }
                return false;
            });
        }
        sComparing = true;
        sCompareKept.clear();
        sCompareKeptSaved.clear();
        sCompareWritten.clear();
        sCompareWasTest.clear();
        for (const std::string& key : quietKeys())
        {
            const auto it = sBaseline.find(key);
            LLControlVariable* control = it != sBaseline.end() ? gSavedSettings.getControl(key).get() : nullptr;
            if (control)
            {
                sCompareKept[key]      = control->getValue();
                sCompareKeptSaved[key] = control->getSaveValue();
                if (plus::posermcp::isTestValue(key, control->getValue()))
                {
                    sCompareWasTest.insert(key);
                }
                // fix-round87: shown, not saved - the user's saved value stays under it (a settings.xml saved
                // meanwhile keeps theirs), and compareEnd() puts back each value as it was, saved or not.
                control->setValue(it->second, false);
                sCompareWritten[key] = control->getValue();   // as the control holds it
            }
        }
    }

    void compareEnd()
    {
        if (!sComparing)
        {
            return;
        }
        sComparing = false;
        for (const auto& [key, value] : sCompareKept)
        {
            // Changed elsewhere meanwhile (the Look menu, Preferences, the V/B/R/T keys): the newer
            // value stays. Only what the compare wrote goes back.
            const auto written = sCompareWritten.find(key);
            if (written != sCompareWritten.end() && !llsd_equals(currentOf(key), written->second))
            {
                continue;
            }
            const auto  saved       = sCompareKeptSaved.find(key);
            const LLSD& saved_value = saved != sCompareKeptSaved.end() ? saved->second : value;
            // fix-round100: a test value of the MCP tools that the MCP let go of meanwhile (it stopped)
            // is not brought back: the user's saved value is.
            const bool let_go = sCompareWasTest.count(key) && !plus::posermcp::isTestValue(key, value);
            putBack(key, let_go ? saved_value : value, saved_value);
        }
        sCompareKept.clear();
        sCompareKeptSaved.clear();
        sCompareWritten.clear();
        sCompareWasTest.clear();
    }

    bool comparing()
    {
        return sComparing;
    }

    // --- Pins ----------------------------------------------------------------

    bool isPinned(const std::string& id)
    {
        for (const LLSD& pin : llsd::inArray(gSavedSettings.getLLSD(SETTING_PINS)))
        {
            if (pin.asString() == id)
            {
                return true;
            }
        }
        return false;
    }

    std::set<std::string> pinnedIds()
    {
        std::set<std::string> ids;
        for (const LLSD& pin : llsd::inArray(gSavedSettings.getLLSD(SETTING_PINS)))
        {
            ids.insert(pin.asString());
        }
        return ids;
    }

    void setPinned(const std::string& id, bool pinned)
    {
        LLSD kept = LLSD::emptyArray();
        for (const LLSD& pin : llsd::inArray(gSavedSettings.getLLSD(SETTING_PINS)))
        {
            if (pin.asString() != id)
            {
                kept.append(pin);
            }
        }
        if (pinned)
        {
            kept.append(id);
        }
        gSavedSettings.setLLSD(SETTING_PINS, kept);
    }

    std::vector<const Item*> pinnedItems()
    {
        std::vector<const Item*> pinned;
        for (const LLSD& pin : llsd::inArray(gSavedSettings.getLLSD(SETTING_PINS)))
        {
            if (const Item* item = findItem(pin.asString()))
            {
                pinned.push_back(item);
            }
        }
        return pinned;
    }

    // --- The user's presets --------------------------------------------------

    std::vector<std::string> myPresetNames()
    {
        std::vector<std::string> names;
        for (const LLSD& preset : llsd::inArray(gSavedSettings.getLLSD(SETTING_PRESETS)))
        {
            names.push_back(preset["name"].asString());
        }
        return names;
    }

    bool saveMyPreset(const std::string& name)
    {
        if (name.empty())
        {
            return false;
        }
        LLSD values;
        for (const std::string& key : quietKeys())
        {
            if (gSavedSettings.controlExists(key))
            {
                values[key] = currentOf(key);
            }
        }
        LLSD entry;
        entry["name"]   = name;
        entry["values"] = values;

        LLSD kept = LLSD::emptyArray();
        for (const LLSD& preset : llsd::inArray(gSavedSettings.getLLSD(SETTING_PRESETS)))
        {
            if (preset["name"].asString() != name)
            {
                kept.append(preset);
            }
        }
        if (kept.size() >= MAX_MY_PRESETS)
        {
            return false;
        }
        kept.append(entry);
        gSavedSettings.setLLSD(SETTING_PRESETS, kept);
        return true;
    }

    bool applyMyPreset(const std::string& name)
    {
        // what a built-in preset set besides the rows is the user's own preset's from now on: nothing to give back
        forgetPresetSettings();
        for (const LLSD& preset : llsd::inArray(gSavedSettings.getLLSD(SETTING_PRESETS)))
        {
            if (preset["name"].asString() == name)
            {
                // Only what a preset is saved with now: one saved by an earlier build may hold settings
                // that rebuild the pipeline (PlusSkinFill) or are no part of a look (PlusCameraRoll).
                const std::vector<std::string> keys = quietKeys();
                const std::set<std::string>    quiet(keys.begin(), keys.end());
                for (const auto& [key, saved] : llsd::inMap(preset["values"]))
                {
                    if (quiet.count(key))
                    {
                        writeTo(key, saved);
                    }
                }
                gSavedSettings.setString("PlusPhotoPreset", "");
                return true;
            }
        }
        return false;
    }

    bool deleteMyPreset(const std::string& name)
    {
        LLSD kept = LLSD::emptyArray();
        bool found = false;
        for (const LLSD& preset : llsd::inArray(gSavedSettings.getLLSD(SETTING_PRESETS)))
        {
            if (preset["name"].asString() == name)
            {
                found = true;
            }
            else
            {
                kept.append(preset);
            }
        }
        if (found)
        {
            gSavedSettings.setLLSD(SETTING_PRESETS, kept);
        }
        return found;
    }
}
