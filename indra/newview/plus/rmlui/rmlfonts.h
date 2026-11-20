/**
 * @file rmlfonts.h
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

// The XUI's sans serif font as RmlUi's family FAMILY: the files fonts.xml lists for "SansSerif" (regular and
// bold) and then for "default", in that order and for the current language (fonts.xml is read with the official
// parser, LLFontRegistry: the viewer's own registry is private to LLFontGL). Each file's bytes come from
// LLFontManager::loadFont, which keeps every font file in memory until shutdownGL: a file the XUI loaded is not
// read again, and RmlUi gets the memory as it is (Rml::LoadFontFace(Span), which must stay until Rml::Shutdown:
// shutdownViews, where RmlUi ends, comes before shutdownGL). The first regular face and the first bold one are
// the family, the others fallback faces for the characters it does not have. Entries with a character functor
// (the SVG emoji) are left out: RmlUi's engine does not draw SVG glyphs.

#ifndef PLUS_RMLFONTS_H
#define PLUS_RMLFONTS_H

#include "stdtypes.h"

namespace plus::rmlui::fonts
{
    constexpr char FAMILY[] = "Charis";

    // Registers the faces with RmlUi (after Rml::Initialise). Returns how many: 0 means RmlUi has no font.
    S32 registerFonts();
}

#endif // PLUS_RMLFONTS_H
