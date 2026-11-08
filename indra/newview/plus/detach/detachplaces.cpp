/**
 * @file detachplaces.cpp
 * @brief Where each detached floater's window was: kept across sessions.
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

#include "detachplaces.h"

#include "llsdutil.h"
#include "llviewercontrol.h"

namespace
{
    constexpr char SETTING[] = "PlusDetachedFloaters";

    // The setting's keys, per floater.
    constexpr char KEY_DETACHED[] = "detached";
    constexpr char KEY_OPEN[] = "open";
    constexpr char KEY_RECT[] = "rect";         // [left, top, right, bottom]
    constexpr char KEY_MONITOR[] = "monitor";

    using plus::detach::ScreenRect;

    // The setting, read once and again after someone else wrote it (a settings
    // import, Debug Settings): our own writes keep the cache.
    struct Cache
    {
        bool loaded = false;
        bool writing = false;   // set() is writing the setting: its change signal is ours
        bool watching = false;
        LLSD map;
        std::vector<std::string> detached;
    };
    Cache sCache;

    void refreshDetachedNames()
    {
        sCache.detached.clear();
        for (const auto& [name, place] : llsd::inMap(sCache.map))
        {
            if (place[KEY_DETACHED].asBoolean())
            {
                sCache.detached.push_back(name);
            }
        }
    }

    LLSD& map()
    {
        if (!sCache.watching)
        {
            if (LLControlVariable* control = gSavedSettings.getControl(SETTING))
            {
                sCache.watching = true;
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                {
                    if (!sCache.writing)
                    {
                        sCache.loaded = false; // read again at the next use
                    }
                });
            }
        }
        if (!sCache.loaded)
        {
            sCache.loaded = true;
            sCache.map = gSavedSettings.getLLSD(SETTING);
            if (!sCache.map.isMap())
            {
                sCache.map = LLSD::emptyMap();
            }
            refreshDetachedNames();
        }
        return sCache.map;
    }

    LLSD rectToLLSD(const ScreenRect& rect)
    {
        LLSD array = LLSD::emptyArray();
        array.append(rect.left);
        array.append(rect.top);
        array.append(rect.right);
        array.append(rect.bottom);
        return array;
    }

    ScreenRect rectFromLLSD(const LLSD& array)
    {
        ScreenRect rect;
        if (array.isArray() && array.size() == 4)
        {
            rect.left = array[0].asInteger();
            rect.top = array[1].asInteger();
            rect.right = array[2].asInteger();
            rect.bottom = array[3].asInteger();
        }
        return rect;
    }
}

namespace plus::detach::places
{
    Place get(const std::string& name)
    {
        Place place;
        const LLSD& all = map();
        const LLSD entry = all.has(name) ? all[name] : LLSD();
        if (entry.isMap())
        {
            place.detached = entry[KEY_DETACHED].asBoolean();
            place.open = entry[KEY_OPEN].asBoolean();
            place.rect = rectFromLLSD(entry[KEY_RECT]);
            place.monitor = entry[KEY_MONITOR].asString();
        }
        return place;
    }

    void set(const std::string& name, const Place& place)
    {
        if (name.empty())
        {
            return;
        }
        LLSD entry = LLSD::emptyMap();
        entry[KEY_DETACHED] = place.detached;
        entry[KEY_OPEN] = place.open;
        if (!place.rect.empty())
        {
            entry[KEY_RECT] = rectToLLSD(place.rect);
            entry[KEY_MONITOR] = place.monitor;
        }
        LLSD& all = map();
        if (all.has(name) && llsd_equals(all[name], entry))
        {
            return;     // nothing new to write
        }
        all[name] = entry;
        sCache.writing = true;
        gSavedSettings.setLLSD(SETTING, all);
        sCache.writing = false;
        refreshDetachedNames();
    }

    const std::vector<std::string>& detachedNames()
    {
        map();
        return sCache.detached;
    }

    bool keptOnScreen(const Place& place, ScreenRect& out)
    {
        Monitor monitor;
        if (place.rect.empty() || place.monitor.empty() || !OsWindow::findMonitor(place.monitor, monitor))
        {
            return false;
        }
        const S32 middle_x = place.rect.left + place.rect.width() / 2;
        const S32 middle_y = place.rect.top + place.rect.height() / 2;
        const ScreenRect& on = monitor.bounds;
        if (middle_x < on.left || middle_x >= on.right || middle_y < on.top || middle_y >= on.bottom)
        {
            return false;
        }
        out = place.rect.fittedInto(monitor.work);
        return !out.empty();
    }
}
