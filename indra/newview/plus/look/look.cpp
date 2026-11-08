/**
 * @file look.cpp
 * @brief Looks: colour grading of the finished image with a LUT, picked from
 *        the Charis menu. Registers with the plus foundation
 *        (research/102-community-looks.md, stage 1a).
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

// A look is a colour grading LUT applied to the finished image, after tone
// mapping and anti-aliasing and before the HUD and the UI: the same values
// ReShade applies its LUTs to, so LUTs made for ReShade, Photoshop or
// DaVinci Resolve work as their authors meant. The LUT is per pixel; the
// screen effects (lookfx.h) take their places and sizes from the whole
// picture, so the screen and snapshots look the same, also large ones drawn
// in tiles (one vignette, not one per tile; the moving noise held still for
// the snapshot). Only what reads neighbouring pixels (painted styles,
// halation, colour fringes) cannot reach across a tile's edge. Vulkan only
// (llvulkan/llvklook.cpp); with GL nothing is drawn. No look selected (the
// default): the pass is not recorded at all.
//
// Looks are a separate axis from the graphics presets (plus/presets): a
// preset says how much to draw, a look how it should feel. Switching presets
// leaves the look as it is, and the presets' files do not list it.
//
// Where looks come from:
//  - shipped: app_settings/plus/looks/<id>/look.xml with its LUT (made for
//    Charis by tools/looks/make_looks.py in the sl-viewer repository, CC0);
//    the id is the folder's name.
//  - the user's: <user data>/look/ (opened from the menu). A folder with a
//    look.xml and its LUT, or a .cube or .png file right in look/; the id is
//    "user:" and the folder's or the file's name.
//
// look.xml (LLSD XML, a map; only "lut" is required):
//   format       integer   1 (this version)
//   name         string, or a map of language codes ("en", "ja", ...) to strings
//   description  the same (for the look list of stage 1b)
//   author       string
//   license      string (e.g. "CC0-1.0")
//   lut          string    the LUT's file name in the folder (.cube or .png)
//   lut_strip    integer   which strip of an image of stacked strips (0: the top one)
//   strength     real      0..1, the look's own strength (PlusLookStrength scales it)
//   order        integer   where a shipped look sits in the menu (smaller first)
//   thumbnail    string    an example picture's file name (stage 1b)
//   finish       map       reserved for the finishing pack (vignette, grain,
//                          ...; research/89) and stage 2; ignored for now
// Keys a viewer does not know are ignored, so later ones can be added.
//
// Settings (settings_plus.xml): PlusLook (the id, empty: none),
// PlusLookStrength (0..100 %), PlusLookSnapshotOnly.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "look.h"
#include "lookfx.h"
#include "lutfile.h"

#include "presets/presets.h"

#include "llcallbacklist.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llimagegl.h"
#include "llimagepng.h"
#include "llmenugl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhilook.h"
#include "llsdserialize.h"
#include "llui.h"
#include "lluictrl.h"
#include "lluictrlfactory.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewermenu.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern bool gSnapshotNoPost;    // llviewerdisplay.cpp: the snapshot is taken without post processing

namespace
{
    constexpr char SETTING_LOOK[]           = "PlusLook";
    constexpr char SETTING_STRENGTH[]       = "PlusLookStrength";
    constexpr char SETTING_SNAPSHOT_ONLY[]  = "PlusLookSnapshotOnly";

    constexpr char LOOK_FILE[]              = "look.xml";
    constexpr char USER_FOLDER[]            = "look";       // in the user's data folder
    constexpr char USER_ID_PREFIX[]         = "user:";
    constexpr S32  LOOK_FORMAT              = 1;            // look.xml's "format" this viewer writes and reads
    constexpr S32  DEFAULT_ORDER            = 1000;

    constexpr S64    MAX_LOOK_FILE_BYTES    = 64 * 1024;
    constexpr S64    MAX_PNG_FILE_BYTES     = 16 * 1024 * 1024;
    constexpr size_t MAX_USER_LOOKS         = 100;
    constexpr U32    MAX_STRENGTH_PERCENT   = 100;

    // Menu: the "Look" submenu of menu_plus.xml. The shipped looks go before
    // the first anchor, the user's before the second.
    constexpr char MENU_PLUS[]              = "Plus";       // the Charis menu (menu_viewer.xml)
    constexpr char MENU_LOOK[]              = "Look";
    constexpr char MENU_SHIPPED_END[]       = "LookShippedEnd";
    constexpr char MENU_USER_END[]          = "LookUserNone";
    constexpr char USER_ITEM_INDENT[]       = "   ";       // the user's looks sit under a heading

    struct Look
    {
        std::string id;
        LLSD name;              // a string, or language code -> string
        std::string fallback;   // the label when there is no name
        std::string lut_path;
        F32 strength = 1.f;     // the look's own, 0..1
        U32 strip = 0;
        S32 order = DEFAULT_ORDER;
    };

    struct Catalogue
    {
        bool scanned = false;
        std::vector<Look> shipped;
        std::vector<Look> user;
    };

    Catalogue& catalogue()
    {
        static Catalogue catalogue;
        return catalogue;
    }

    // The LUT of the selected look as a texture (the strip): an 8-bit LLImageGL
    // made at idle from an LLImageRaw, the way the viewer makes its other
    // textures, never in the middle of drawing a frame.
    struct LoadedLut
    {
        std::string id;                 // the look it was made for; empty: none
        LLPointer<LLImageGL> image;     // null with an id: that look's LUT could not be used (not tried again until reloaded)
        LLPointer<LLImageRaw> pixels;   // the strip's pixels, kept as long as the image (an upload may read them later in the frame)
        U32 size = 0;
        F32 domain_min[3] = {};
        F32 domain_max[3] = {};
    };

    // On the heap and never destroyed: a static destructor would release the
    // texture without a GL context.
    LoadedLut& loadedLut()
    {
        static LoadedLut* lut = new LoadedLut();
        return *lut;
    }

    std::vector<LLHandle<LLView>> sMenuItems;   // the items the menu was given for the looks
    std::set<std::string> sWarnedMissing;       // looks named by PlusLook but not found (warned once)
    unsigned sGeneration = 0;                   // counts the reads of the folders (plus::look::generation())

    // ------------------------------------------------------------------
    // Finding looks

    std::string shippedFolder()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", "looks");
    }

    std::string userFolder()
    {
        return gDirUtilp->add(gDirUtilp->getOSUserAppDir(), USER_FOLDER);
    }

    std::string lowerExtension(const std::string& name)
    {
        std::string extension = gDirUtilp->getExtension(name);
        LLStringUtil::toLower(extension);
        return extension;
    }

    bool isLutFile(const std::string& name)
    {
        const std::string extension = lowerExtension(name);
        return extension == "cube" || extension == "png";
    }

    // A file name in the look's own folder: no path, nothing that leaves the folder.
    bool isPlainFileName(const std::string& name)
    {
        return !name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..";
    }

    // A quick look at a LUT file before it is listed: a .cube of a size the
    // reader takes, a PNG whose header gives a strip's size (the 24 bytes of
    // its signature and IHDR). The whole file is read when the look is chosen.
    bool lutFileUsable(const std::string& path, std::string& why)
    {
        const S64 bytes = LLFile::size(path);
        const bool cube = lowerExtension(path) == "cube";
        const S64 max_bytes = cube ? (S64)plus::look::MAX_CUBE_FILE_BYTES : MAX_PNG_FILE_BYTES;
        if (bytes <= 0 || bytes > max_bytes)
        {
            why = bytes <= 0 ? "missing or empty" : "too large";
            return false;
        }
        if (cube)
        {
            return true;
        }
        constexpr char PNG_SIGNATURE[] = "\x89PNG\r\n\x1a\n";
        constexpr size_t HEADER_BYTES = 24;
        char header[HEADER_BYTES] = {};
        llifstream file(path.c_str(), std::ios::binary);
        if (!file.read(header, HEADER_BYTES) || memcmp(header, PNG_SIGNATURE, 8) || memcmp(header + 12, "IHDR", 4))
        {
            why = "not a PNG image";
            return false;
        }
        auto big_endian = [&](size_t at)
        {
            const U8* b = (const U8*)header + at;
            return ((U32)b[0] << 24) | ((U32)b[1] << 16) | ((U32)b[2] << 8) | (U32)b[3];
        };
        plus::look::StripLayout layout;
        if (!plus::look::stripLayout(big_endian(16), big_endian(20), layout))
        {
            why = std::to_string(big_endian(16)) + "x" + std::to_string(big_endian(20)) + " is not a LUT strip";
            return false;
        }
        return true;
    }

    std::vector<std::string> folderEntries(const std::string& folder)
    {
        std::vector<std::string> names;
        if (LLFile::isdir(folder))
        {
            LLDirIterator iter(folder, "*");
            std::string name;
            while (iter.next(name))
            {
                names.push_back(name);
            }
        }
        std::sort(names.begin(), names.end(),
                  [](const std::string& a, const std::string& b) { return LLStringUtil::compareDictInsensitive(a, b) < 0; });
        return names;
    }

    // Reads folder/look.xml into look (its id is set by the caller).
    bool readLookFile(const std::string& folder, Look& look)
    {
        const std::string path = gDirUtilp->add(folder, LOOK_FILE);
        const S64 bytes = LLFile::size(path);
        if (bytes <= 0 || bytes > MAX_LOOK_FILE_BYTES)
        {
            LL_WARNS("Plus") << "Look: " << path << " is empty or too large; skipped" << LL_ENDL;
            return false;
        }
        llifstream file(path.c_str());
        LLSD definition;
        if (!file.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(definition, file) || !definition.isMap())
        {
            LL_WARNS("Plus") << "Look: cannot read " << path << "; skipped" << LL_ENDL;
            return false;
        }
        if (definition["format"].asInteger() > LOOK_FORMAT)
        {
            LL_INFOS("Plus") << "Look: " << path << " is of a newer format (" << definition["format"].asInteger()
                             << "); reading what this viewer knows" << LL_ENDL;
        }
        const std::string lut = definition["lut"].asString();
        if (!isPlainFileName(lut) || !isLutFile(lut))
        {
            LL_WARNS("Plus") << "Look: " << path << " names no .cube or .png LUT in its folder; skipped" << LL_ENDL;
            return false;
        }
        look.lut_path = gDirUtilp->add(folder, lut);
        std::string why;
        if (!lutFileUsable(look.lut_path, why))
        {
            LL_WARNS("Plus") << "Look: " << look.lut_path << ": " << why << "; skipped" << LL_ENDL;
            return false;
        }
        look.name = definition["name"];
        look.strength = definition.has("strength") ? llclamp((F32)definition["strength"].asReal(), 0.f, 1.f) : 1.f;
        look.strip = (U32)llmax(0, definition["lut_strip"].asInteger());
        look.order = definition.has("order") ? definition["order"].asInteger() : DEFAULT_ORDER;
        return true;
    }

    void scanShipped(std::vector<Look>& looks)
    {
        const std::string folder = shippedFolder();
        for (const std::string& name : folderEntries(folder))
        {
            Look look;
            look.id = name;
            look.fallback = name;
            const std::string path = gDirUtilp->add(folder, name);
            if (LLFile::isdir(path) && readLookFile(path, look))
            {
                looks.push_back(look);
            }
        }
        std::stable_sort(looks.begin(), looks.end(), [](const Look& a, const Look& b) { return a.order < b.order; });
    }

    void scanUser(std::vector<Look>& looks)
    {
        const std::string folder = userFolder();
        for (const std::string& name : folderEntries(folder))
        {
            if (looks.size() >= MAX_USER_LOOKS)
            {
                LL_WARNS("Plus") << "Look: more than " << MAX_USER_LOOKS << " looks in " << folder << "; the rest are not listed" << LL_ENDL;
                break;
            }
            Look look;
            look.id = USER_ID_PREFIX + name;
            const std::string path = gDirUtilp->add(folder, name);
            if (LLFile::isdir(path))
            {
                look.fallback = name;
                if (LLFile::isfile(gDirUtilp->add(path, LOOK_FILE)) && readLookFile(path, look))
                {
                    looks.push_back(look);
                }
            }
            else if (isLutFile(name))
            {
                std::string why;
                if (!lutFileUsable(path, why))
                {
                    LL_WARNS("Plus") << "Look: " << path << ": " << why << "; skipped" << LL_ENDL;
                    continue;
                }
                look.fallback = gDirUtilp->getBaseFileName(name, true);
                look.lut_path = path;
                looks.push_back(look);
            }
        }
    }

    // Reads the folders on first use and again when the list is reloaded.
    void scanLooks()
    {
        Catalogue& c = catalogue();
        c.shipped.clear();
        c.user.clear();
        scanShipped(c.shipped);
        scanUser(c.user);
        c.scanned = true;
        ++sGeneration;
        LL_INFOS("Plus") << "Looks: " << c.shipped.size() << " shipped, " << c.user.size() << " in " << userFolder() << LL_ENDL;
    }

    const Look* findLook(const std::string& id)
    {
        Catalogue& c = catalogue();
        if (!c.scanned)
        {
            scanLooks();
        }
        for (const std::vector<Look>* looks : { &c.shipped, &c.user })
        {
            for (const Look& look : *looks)
            {
                if (look.id == id)
                {
                    return &look;
                }
            }
        }
        return nullptr;
    }

    // The name in the UI's language, else in English, else the first given, else the fallback.
    std::string lookLabel(const Look& look)
    {
        if (look.name.isString() && !look.name.asString().empty())
        {
            return look.name.asString();
        }
        if (look.name.isMap())
        {
            for (const std::string& language : { LLUI::getLanguage(), std::string("en") })
            {
                if (look.name.has(language) && !look.name[language].asString().empty())
                {
                    return look.name[language].asString();
                }
            }
            if (look.name.beginMap() != look.name.endMap())
            {
                return look.name.beginMap()->second.asString();
            }
        }
        return look.fallback;
    }

    // ------------------------------------------------------------------
    // The LUT

    bool readFile(const std::string& path, S64 max_bytes, std::string& text, std::string& error)
    {
        const S64 bytes = LLFile::size(path);
        if (bytes <= 0 || bytes > max_bytes)
        {
            error = bytes <= 0 ? "missing or empty" : "larger than " + std::to_string(max_bytes >> 20) + " MB";
            return false;
        }
        llifstream file(path.c_str(), std::ios::binary);
        if (!file.is_open())
        {
            error = "cannot open";
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    }

    bool readPngStrip(const Look& look, plus::look::LutTable& table, bool& flipped, std::string& error)
    {
        const S64 bytes = LLFile::size(look.lut_path);
        if (bytes <= 0 || bytes > MAX_PNG_FILE_BYTES)
        {
            error = bytes <= 0 ? "missing or empty" : "larger than " + std::to_string(MAX_PNG_FILE_BYTES >> 20) + " MB";
            return false;
        }
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (!png->load(look.lut_path))
        {
            error = "not a PNG image";
            return false;
        }
        // The header is read: check the size before decoding.
        plus::look::StripLayout layout;
        if (!plus::look::stripLayout((unsigned)png->getWidth(), (unsigned)png->getHeight(), layout))
        {
            error = std::to_string(png->getWidth()) + "x" + std::to_string(png->getHeight()) +
                    " is not a LUT strip (N tiles of NxN side by side: 256x16, 1024x32, 4096x64)";
            return false;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        if (!png->decode(raw, 0.f) || !raw->getData())
        {
            error = "cannot decode the PNG image";
            return false;
        }
        // LLImageRaw keeps the bottom row first.
        return plus::look::parseStrip(raw->getData(), (unsigned)raw->getWidth(), (unsigned)raw->getHeight(),
                                      (unsigned)raw->getComponents(), true, look.strip, table, flipped, error);
    }

    void releaseLut()
    {
        loadedLut() = LoadedLut();      // the LLImageGL deletes its texture
    }

    // The table as an 8-bit strip texture: resampled to 16, 32 or 64 (the
    // strip's sides must be powers of two for LLImageGL), bilinear, clamped.
    bool uploadLut(const plus::look::LutTable& read)
    {
        const plus::look::LutTable table = plus::look::resample(read, plus::look::gpuSize(read.size));
        const std::vector<U8> strip = plus::look::toStrip8(table);
        LoadedLut& lut = loadedLut();
        // Row 0 of the raw image is row 0 of the texture: green 0, as the pass reads it.
        lut.pixels = new LLImageRaw((U16)(table.size * table.size), (U16)table.size, 4);
        if (!lut.pixels->getData() || lut.pixels->getDataSize() != (S32)strip.size())
        {
            lut.pixels = nullptr;
            return false;
        }
        memcpy(lut.pixels->getData(), strip.data(), strip.size());
        lut.image = new LLImageGL(false, false);        // no mipmaps, never compressed
        if (!lut.image->createGLTexture(0, lut.pixels) || !lut.image->getTexName())
        {
            lut.image = nullptr;
            lut.pixels = nullptr;
            return false;
        }
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bind(lut.image.get());
        unit->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
        unit->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        unit->unbind(LLTexUnit::TT_TEXTURE);
        lut.size = table.size;
        for (U32 c = 0; c < 3; ++c)
        {
            lut.domain_min[c] = table.domain_min[c];
            lut.domain_max[c] = table.domain_max[c];
        }
        return true;
    }

    // Reads the look's LUT file and makes its texture.
    void loadLut(const Look& look)
    {
        plus::look::LutTable table;
        std::string error;
        bool flipped = false;
        bool read = false;
        if (lowerExtension(look.lut_path) == "cube")
        {
            std::string text;
            read = readFile(look.lut_path, (S64)plus::look::MAX_CUBE_FILE_BYTES, text, error) && plus::look::parseCube(text, table, error);
        }
        else
        {
            read = readPngStrip(look, table, flipped, error);
        }
        if (!read)
        {
            LL_WARNS("Plus") << "Look " << look.id << ": cannot use " << look.lut_path << ": " << error << LL_ENDL;
            return;
        }
        if (!uploadLut(table))
        {
            LL_WARNS("Plus") << "Look " << look.id << ": cannot make the LUT texture" << LL_ENDL;
            return;
        }
        LL_INFOS("Plus") << "Look " << look.id << ": " << look.lut_path << ", " << table.size << "^3 (on the GPU "
                         << loadedLut().size << "^3, 8-bit)" << (flipped ? ", green bottom to top (read flipped)" : "")
                         << ", largest change from neutral " << plus::look::distanceFromNeutral(table) << LL_ENDL;
        if (plus::look::looksLogEncoded(table.title + " " + look.lut_path))
        {
            LL_WARNS("Plus") << "Look " << look.id << ": this LUT may be made for log camera footage; "
                             << "the viewer's image is not log encoded, so its colours will be off" << LL_ENDL;
        }
    }

    // At idle (never while a frame is drawn): the selected look's texture
    // made when the selection changed, released when none is selected.
    void prepareSelected()
    {
        const std::string id = gSavedSettings.getString(SETTING_LOOK);
        if (id == loadedLut().id)
        {
            return;
        }
        releaseLut();
        if (id.empty())
        {
            return;
        }
        loadedLut().id = id;
        if (const Look* look = findLook(id))
        {
            loadLut(*look);
        }
        else if (sWarnedMissing.insert(id).second)
        {
            LL_WARNS("Plus") << "Look " << id << " (PlusLook) is not in the look folders" << LL_ENDL;
        }
    }

    // ------------------------------------------------------------------
    // Menu: Charis > Look

    S32 itemIndex(LLMenuGL* menu, const std::string& name)
    {
        for (U32 i = 0; i < menu->getItemCount(); ++i)
        {
            if (menu->getItem((S32)i)->getName() == name)
            {
                return (S32)i;
            }
        }
        return -1;
    }

    void addLookItem(LLMenuGL* menu, const Look& look, const std::string& before, const std::string& indent)
    {
        LLMenuItemCheckGL::Params p;
        p.name = "LookItem" + std::to_string(sMenuItems.size());
        p.label = indent + lookLabel(look);
        p.on_click.function_name = "Plus.Look.Select";
        p.on_click.parameter = look.id;
        p.on_check.function_name = "Plus.Look.IsSelected";
        p.on_check.parameter = look.id;
        LLMenuItemCheckGL* item = LLUICtrlFactory::create<LLMenuItemCheckGL>(p);
        if (!item)
        {
            return;
        }
        const S32 index = itemIndex(menu, before);
        if (index >= 0)
        {
            menu->insert(index, item, false);
        }
        else
        {
            menu->addChild(item);
        }
        sMenuItems.push_back(item->getHandle());
    }

    void rebuildMenu()
    {
        LLMenuGL* plus_menu = gMenuBarView ? gMenuBarView->findChildMenuByName(MENU_PLUS, false) : nullptr;
        LLMenuGL* menu = plus_menu ? plus_menu->findChildMenuByName(MENU_LOOK, true) : nullptr;
        if (!menu)
        {
            LL_WARNS("Plus") << "No \"" << MENU_LOOK << "\" menu in menu_plus.xml; looks are not listed" << LL_ENDL;
            return;
        }
        for (const LLHandle<LLView>& handle : sMenuItems)
        {
            if (LLView* item = handle.get())
            {
                menu->removeChild(item);
                delete item;
            }
        }
        sMenuItems.clear();

        const Catalogue& c = catalogue();
        for (const Look& look : c.shipped)
        {
            addLookItem(menu, look, MENU_SHIPPED_END, "");
        }
        for (const Look& look : c.user)
        {
            addLookItem(menu, look, MENU_USER_END, USER_ITEM_INDENT);
        }
        menu->needsArrange();
    }

    void select(const LLSD& id)
    {
        gSavedSettings.setString(SETTING_LOOK, id.asString());
    }

    bool isSelected(const LLSD& id)
    {
        return gSavedSettings.getString(SETTING_LOOK) == id.asString();
    }

    void openLookFolder()
    {
        const std::string folder = userFolder();
        if (!LLFile::isdir(folder))
        {
            LLFile::mkdir(folder);
            if (!LLFile::isdir(folder))
            {
                LL_WARNS("Plus") << "Look: could not make " << folder << LL_ENDL;
                return;
            }
        }
        plus::openFolder(folder);
    }

    // The lists again from the folders, and the LUT in use read again (an
    // edited file shows at once). The menu is rebuilt at the next idle, not
    // inside the click on one of its items.
    void reloadLooks()
    {
        scanLooks();
        releaseLut();
        sWarnedMissing.clear();
        doOnIdleOneTime(&rebuildMenu);
        doOnIdleOneTime(&prepareSelected);
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusLook*).
        // Menu items: skins/default/xui/<lang>/menu_plus.xml (the "Look" submenu). The photo tools list
        // the looks too (plus/photomode/photolook.cpp, through entries() and reload() below), with the
        // strength and "only in snapshots" (app_settings/plus/phototools/catalog.xml).
        // Drawn from the <plus> hook in LLPipeline::renderFinalize().
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Look.Select",
                                                                 [](LLUICtrl*, const LLSD& param) { select(param); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Look.IsSelected",
                                                                 [](LLUICtrl*, const LLSD& param) { return isSelected(param); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Look.OpenFolder",
                                                                 [](LLUICtrl*, const LLSD&) { openLookFolder(); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Look.Reload", [](LLUICtrl*, const LLSD&) { reloadLooks(); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Look.HasNoUserLooks",
                                                                 [](LLUICtrl*, const LLSD&) { return catalogue().user.empty(); });
        // The "Your Looks" heading and the empty-list hint are not commands.
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Look.Never", [](LLUICtrl*, const LLSD&) { return false; });

        // A new selection gets its texture at the next idle.
        if (LLControlVariable* look = gSavedSettings.getControl(SETTING_LOOK).get())
        {
            look->getCommitSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { doOnIdleOneTime(&prepareSelected); });
        }
    }

    void onUIReady()
    {
        if (!catalogue().scanned)
        {
            scanLooks();
        }
        rebuildMenu();
        prepareSelected();      // the look saved from the last session
    }

    plus::FeatureRegistration sRegistration({ "look", &init, &onUIReady });
}

namespace plus::look
{
    std::vector<Entry> entries()
    {
        Catalogue& c = catalogue();
        if (!c.scanned)
        {
            scanLooks();
        }
        std::vector<Entry> list;
        list.reserve(c.shipped.size() + c.user.size());
        for (const Look& look : c.shipped)
        {
            list.push_back({ look.id, lookLabel(look), false });
        }
        for (const Look& look : c.user)
        {
            list.push_back({ look.id, lookLabel(look), true });
        }
        return list;
    }

    std::string labelOf(const std::string& id)
    {
        const Look* look = id.empty() ? nullptr : findLook(id);
        return look ? lookLabel(*look) : std::string();
    }

    void reload()
    {
        reloadLooks();
    }

    void openUserFolder()
    {
        openLookFolder();
    }

    unsigned generation()
    {
        return sGeneration;
    }

    void prepareSelectedNow()
    {
        prepareSelected(); // the one queued by the setting's change finds it made and does nothing
    }

    bool render(LLRenderTarget* source, LLRenderTarget* target)
    {
        static LLCachedControl<std::string> selected(gSavedSettings, SETTING_LOOK, "");
        static LLCachedControl<U32> strength_percent(gSavedSettings, SETTING_STRENGTH, MAX_STRENGTH_PERCENT);
        static LLCachedControl<bool> snapshot_only(gSavedSettings, SETTING_SNAPSHOT_ONLY, false);

        if (!LLRHI::sLook || !LLRHI::sLookGrade || gSnapshotNoPost)
        {
            return false;
        }

        // The LUT: the texture is made at idle (prepareSelected); until it is
        // there for the selected look, only the effects (if any) are drawn.
        const std::string& id = selected();
        const LoadedLut& lut = loadedLut();
        F32 strength = 0.f;
        if (!id.empty() && lut.id == id && !lut.image.isNull() && !(snapshot_only && !plus::presets::snapshotInProgress()))
        {
            const Look* look = findLook(id);
            strength = look ? look->strength * (F32)llmin((U32)strength_percent, MAX_STRENGTH_PERCENT) / (F32)MAX_STRENGTH_PERCENT
                            : 0.f;
        }
        const bool with_lut = strength > 0.f;
        LLRHI::LookFx fx;
        const bool with_fx = currentFx(fx);
        if ((!with_lut && !with_fx) || !source || !target || !source->isComplete() || !target->isComplete() ||
            source->getWidth() != target->getWidth() || source->getHeight() != target->getHeight())
        {
            return false;
        }

        LLRHI::gpuMark("look");
        LLRHI::LookGrade pass;
        pass.source_texture = source->getTexture();
        if (with_lut)
        {
            pass.lut_texture = lut.image->getTexName();
            pass.lut_size = lut.size;
            for (U32 c = 0; c < 3; ++c)
            {
                pass.domain_min[c] = lut.domain_min[c];
                pass.domain_max[c] = lut.domain_max[c];
            }
            pass.strength = strength;
        }
        pass.fx = fx;
        pass.width = target->getWidth();
        pass.height = target->getHeight();
        // A large snapshot drawn in tiles (LLViewerWindow::rawSnapshot when
        // targets of its size cannot be made; the camera's zoom is above 1
        // only then): the tile's place in the whole picture, which the
        // camera's zoomed projection (LLViewerCamera::setPerspective) puts at
        // (column, row) * tile size from the bottom left, the picture being
        // zoom times the tile (fix-round6 4).
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 zoom = camera->getZoomFactor();
        if (zoom > 1.f)
        {
            const S32 columns = llceil(zoom);
            const S32 row = camera->getZoomSubRegion() / columns;
            const S32 column = camera->getZoomSubRegion() - row * columns;
            pass.image_offset[0] = (F32)(column * (S32)pass.width);
            pass.image_offset[1] = (F32)(row * (S32)pass.height);
            pass.image_size[0] = zoom * (F32)pass.width;
            pass.image_size[1] = zoom * (F32)pass.height;
        }
        // The miniature's blur, the rings of clarity and texture and the soft glow's taps read between pixels
        // (research/236 §2.2, 249 §2.1, 286): the source filtered bilinearly, whatever filter the pass before left on it. Not set back:
        // whatever reads the source next binds it with LLGLSLShader::bindTexture, which sets its own filter
        // (bilinear by default).
        if (fx.miniature_blur > 0.f || fx.localContrast() || fx.softGlow())
        {
            LLTexUnit* unit = gGL.getTexUnit(0);
            unit->bind(source);
            unit->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
            unit->unbind(LLTexUnit::TT_TEXTURE);
        }
        target->bindTarget();
        const bool drawn = LLRHI::sLookGrade(pass);
        target->flush();
        return drawn;
    }
}
