/**
 * @file rmlcommon.h
 * @brief RmlUi's headers for plus/rmlui, and what its parts share (research/364).
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

// How plus/rmlui fits together (research/364 section 2):
//   rmlhost.h    LLRmlView, the XUI tag plus_rml_view: one RmlUi context with one document filling an
//                LLFloater's content. Input, focus, size and the per-frame "update only when something changed".
//   rmlrender.h  Rml::RenderInterface on the official path (way B): geometry kept in LLVertexBuffers, drawn
//                with the UI shader and drawRange; runs of the same texture and scissor packed into one
//                draw; a frame where nothing changed draws the last list again.
//   rmlsystem.h  Rml::SystemInterface and Rml::FileInterface (files only under app_settings/plus/rmlui/),
//                RmlUi's start and end (the first host starts it, the last one ends it 60 s later), the
//                "RmlUi:" log line.
//   rmlfonts.h   the fonts of fonts.xml handed to RmlUi's FreeType engine from LLFontManager's memory.
//   rmlime.h     the bridge between the window's IME (LLPreeditor) and RmlUi's text fields.
//   rmldemo.h    the development window Plus.RmlDemo and its XUI twin.
// RmlUi is used on the main thread only. The IME's queries that come on the window thread are answered
// from a copy the main thread writes (rmlime.h).

#ifndef PLUS_RMLCOMMON_H
#define PLUS_RMLCOMMON_H

#ifndef RMLUI_STATIC_LIB
#define RMLUI_STATIC_LIB 1  // rmlui.cmake builds RmlUi as a static library and defines this for the viewer too
#endif

#include "thirdparty/RmlUi-6.3/Include/RmlUi/Core.h"
#include "thirdparty/RmlUi-6.3/Include/RmlUi/Core/TextInputContext.h"   // not in Core.h
#include "thirdparty/RmlUi-6.3/Include/RmlUi/Core/TextInputHandler.h"

namespace plus::rmlui
{
    // The folder of the documents, style sheets and their files, under app_settings (rmlsystem.cpp's file
    // interface reads nothing else).
    constexpr char DOCUMENT_FOLDER[] = "plus/rmlui";

    // A texture source naming a UI image (textures.xml, plus::declareUIImage) rather than a file:
    // "ui-image/Plus_PS_Icon_check" in RCSS (decorator: image(ui-image/...)). rmlrender.cpp's LoadTexture
    // takes only these.
    constexpr char UI_IMAGE_PREFIX[] = "ui-image/";
}

#endif // PLUS_RMLCOMMON_H
