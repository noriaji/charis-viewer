/**
 * @file rmlfonts.cpp
 * @brief The fonts of fonts.xml handed to RmlUi's FreeType engine from LLFontManager's memory (research/364 2.3, F1).
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

#include "rmlcommon.h"
#include "rmlfonts.h"

#include "llfile.h"
#include "llfontfreetype.h"
#include "llfontgl.h"
#include "llfontregistry.h"

#include <algorithm>
#include <string>
#include <vector>

namespace plus::rmlui::fonts
{
    namespace
    {
        // The size LLFontRegistry gives the descriptors it reads from fonts.xml (s_template_string, llfontregistry.cpp).
        constexpr char TEMPLATE_SIZE[] = "TEMPLATE";

        struct Face
        {
            std::string path;
            S32 face_index = 0;
            bool bold = false;
        };

        bool isCollection(const font_file_info_vec_t& collections, const std::string& name)
        {
            return std::any_of(collections.begin(), collections.end(),
                               [&name](const LLFontFileInfo& info) { return info.FileName == name; });
        }

        // The file's first place in the font folders, in the order LLFontRegistry::createFont looks: the same path,
        // so LLFontManager finds the memory the XUI's font loaded under it.
        std::string findFontFile(const std::string& name)
        {
            for (const std::string& folder : { LLFontGL::getFontPathLocal(), LLFontGL::getFontPathSystem() })
            {
                const std::string path = folder + name;
                if (LLFile::isfile(path))
                {
                    return path;
                }
            }
            return std::string();
        }

        void collect(const LLFontDescriptor* desc, bool bold, std::vector<Face>& faces)
        {
            if (!desc)
            {
                return;
            }
            for (const LLFontFileInfo& file : desc->getFontFiles())
            {
                if (file.CharFunctor)
                {
                    continue;   // an entry for some characters only (the SVG emoji)
                }
                const std::string path = findFontFile(file.FileName);
                if (path.empty())
                {
                    continue;   // not on this PC (fonts.xml lists other systems' fonts too)
                }
                // A collection (load_collection) is the first face here: RmlUi loads a variable font's weights itself.
                const S32 face_index = isCollection(desc->getFontCollectionFiles(), file.FileName) ? 0 : file.mFaceIndex;
                const bool known = std::any_of(faces.begin(), faces.end(), [&](const Face& face) {
                    return face.path == path && face.face_index == face_index && face.bold == bold;
                });
                if (!known)
                {
                    faces.push_back({ path, face_index, bold });
                }
            }
        }
    }

    S32 registerFonts()
    {
        if (!gFontManagerp)
        {
            return 0;
        }
        LLFontRegistry registry(false);     // reads fonts.xml only: no font is made with it
        if (!registry.parseFontInfo("fonts.xml"))
        {
            LL_WARNS("RmlUi") << "RmlUi: fonts.xml could not be read" << LL_ENDL;
            return 0;
        }
        std::vector<Face> faces;
        collect(registry.getClosestFontTemplate(LLFontDescriptor("SansSerif", TEMPLATE_SIZE, 0)), false, faces);
        collect(registry.getClosestFontTemplate(LLFontDescriptor("SansSerif", TEMPLATE_SIZE, LLFontGL::BOLD)), true, faces);
        collect(registry.getMatchingFontDesc(LLFontDescriptor("default", TEMPLATE_SIZE, 0)), false, faces);

        S32 registered = 0;
        S32 fallbacks = 0;
        std::string regular_path;   // the family's regular face (a variable font: its bold is in it)
        bool have_bold = false;
        for (const Face& face : faces)
        {
            const bool family_face = face.bold ? !have_bold : regular_path.empty();
            if (face.bold && (!family_face || face.path == regular_path))
            {
                have_bold = have_bold || face.path == regular_path;
                continue;   // the regular faces stand in for the characters the bold one lacks
            }
            long size = 0;
            const U8* data = gFontManagerp->loadFont(face.path, size);
            if (!data || size <= 0)
            {
                continue;
            }
            // A fallback gets a family of its own: in FAMILY it could be taken for the regular face.
            const std::string family = family_face ? std::string(FAMILY) : std::string(FAMILY) + "Fallback" + std::to_string(++fallbacks);
            const Rml::Style::FontWeight weight = face.bold ? Rml::Style::FontWeight::Bold : Rml::Style::FontWeight::Auto;
            if (Rml::LoadFontFace(Rml::Span<const Rml::byte>(data, (size_t)size), family, Rml::Style::FontStyle::Normal, weight,
                                  !family_face, face.face_index))
            {
                ++registered;
                if (family_face && face.bold)
                {
                    have_bold = true;
                }
                else if (family_face)
                {
                    regular_path = face.path;
                }
            }
        }
        return registered;
    }
}
