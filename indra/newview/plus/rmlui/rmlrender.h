/**
 * @file rmlrender.h
 * @brief RmlUi's render interface on the official UI path: kept vertex buffers, packed runs, replay (research/364 2.1).
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

// Way B of research/364 2.1: nothing here adds an LLRHI hook, a pass, a target or a barrier. A geometry
// RmlUi compiles keeps its vertices (RmlUi guarantees them until ReleaseGeometry); Context::Render only
// records (geometry, translation, texture, scissor, transform) into the host's DrawList, and draw() then
// draws the list's runs with the UI shader (gUIProgram) and LLVertexBuffer::drawRange, the way
// LLFontVertexBuffer draws kept text: the same recording stream, target and pass as the XUI around it.
//   runs     consecutive commands with the same texture, scissor and transform are packed into one
//            LLVertexBuffer (translation baked in) and drawn at once. The runs are made again only when
//            the recorded list differs from the last one (compared by serial numbers, not by the
//            vertices) or the floater's opacity changed; then a packed run with the same contents as one
//            of the last build (geometries, translations, opacity: runContent) takes its buffer, so a
//            small change copies only the runs it touches. A run of one command draws the geometry's own
//            buffer (made the first time it is drawn so) with its translation in the model view.
//   replay   a frame with nothing to update (rmlhost.cpp) does not call Context::Render: draw() draws the
//            last runs again. Releasing a geometry or texture a list uses makes the list invalid, so the
//            host renders again before drawing (a released geometry is never drawn).
//   colours  RmlUi's vertices and generated textures (the glyphs) are premultiplied sRGB: blended with
//            ONE, ONE_MINUS_SOURCE_ALPHA and written as they are (the UI is drawn after the tone mapping,
//            in the window's encoded values). Loaded textures (UI images) are not premultiplied: their runs
//            are always packed, with the vertex colours taken back to straight alpha, and blended with
//            SOURCE_ALPHA, ONE_MINUS_SOURCE_ALPHA (no copy of the image).
// Main thread only.

#ifndef PLUS_RMLRENDER_H
#define PLUS_RMLRENDER_H

#include "rmlcommon.h"

#include "llpointer.h"

#include <vector>

class LLVertexBuffer;

namespace plus::rmlui
{
    struct Geometry;
    struct Texture;

    // Where a host is on the screen while it draws (rmlhost.cpp fills it each frame).
    struct DrawFrame
    {
        // The window pixel of the context's top-left corner, and window pixels per context pixel (1, but the
        // zoom of a tiled snapshot): for the scissor, which the model view matrix does not move.
        F32 left_px = 0.f;
        F32 top_px = 0.f;
        F32 zoom_x = 1.f;
        F32 zoom_y = 1.f;
        // The host's own clip in window pixels (its LLLocalClipRect): every scissor of the document stays inside.
        S32 clip_x = 0;
        S32 clip_y = 0;
        S32 clip_w = 0;
        S32 clip_h = 0;
        // The floater's opacity (LLViewDrawContext::mAlpha), multiplied into the vertex colours.
        F32 alpha = 1.f;
    };

    // What one host's last Context::Render recorded, and the runs made of it.
    class DrawList
    {
    public:
        DrawList();
        ~DrawList();
        DrawList(const DrawList&) = delete;
        DrawList& operator=(const DrawList&) = delete;

        // The recording draws what the document shows: recorded, and no geometry or texture of it released since.
        bool valid() const { return mValid; }

        // The renderer's (rmlrender.cpp): one RenderGeometry call, and one draw made of them.
        struct Command
        {
            Geometry* geometry = nullptr;       // its own buffer is made on it (buildRuns)
            U64 geometry_serial = 0;
            Rml::Vector2f translation;
            const Texture* texture = nullptr;   // null: untextured
            U64 texture_serial = 0;
            S32 scissor = -1;                   // index into the scissors, -1: none
            S32 transform = -1;                 // index into the transforms, -1: none
        };

        struct Run
        {
            LLPointer<LLVertexBuffer> buffer;
            U32 vertices = 0;
            U32 indices = 0;
            Rml::Vector2f translation;          // not baked in (a run of one command drawing its geometry's buffer)
            const Texture* texture = nullptr;
            S32 scissor = -1;
            S32 transform = -1;
            bool straight = false;              // a loaded (not premultiplied) texture
            // A packed run's contents: its commands' geometries and translations, the opacity and the colour form
            // (a hash of them, and how many commands). A new run with the same takes this run's buffer (buildRuns).
            U64 content = 0;
            U32 commands = 0;                   // 0: not packed (a geometry's own buffer)
        };

    private:
        friend class Renderer;

        // The recording, and the one being recorded (swapped in when it differs).
        std::vector<Command> mCommands;
        std::vector<Rml::Rectanglei> mScissors;
        std::vector<Rml::Matrix4f> mTransforms;
        std::vector<Command> mNextCommands;
        std::vector<Rml::Rectanglei> mNextScissors;
        std::vector<Rml::Matrix4f> mNextTransforms;
        // What the recording uses, sorted (a release finds its users by binary search).
        std::vector<const Geometry*> mUsedGeometry;
        std::vector<const Texture*> mUsedTextures;

        std::vector<Run> mRuns;
        std::vector<Run> mOldRuns;  // the last runs while new ones are made (their buffers taken when the same)
        F32 mRunsAlpha = -1.f;      // the opacity the runs' colours were made with
        bool mRunsValid = false;    // the runs are made of mCommands
        bool mValid = false;
        bool mKnown = false;        // in the renderer's list of lists
    };

    class Renderer final : public Rml::RenderInterface
    {
    public:
        Renderer();
        ~Renderer() override;

        Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
        void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
        void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
        Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
        Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
        void ReleaseTexture(Rml::TextureHandle texture) override;
        void EnableScissorRegion(bool enable) override;
        void SetScissorRegion(Rml::Rectanglei region) override;
        void SetTransform(const Rml::Matrix4f* transform) override;

        // A Context::Render between these is recorded into list; nothing is drawn while recording. false: a
        // geometry or texture the recording names was released while it ran (a glyph texture made again): the
        // list is invalid, render again.
        void beginRecording(DrawList& list);
        bool endRecording();
        // Draws list (its runs made again first when needed). Inside the host's draw scope (rmlhost.cpp): the
        // UI shader bound, the model view at the context's top-left with y down, the host's clip set.
        void draw(DrawList& list, const DrawFrame& frame);
        // list goes away (its host is destroyed): no release looks at it any more.
        void forget(DrawList& list);

        // The geometries and textures RmlUi holds (for the "RmlUi:" line).
        U32 geometryCount() const { return mGeometryCount; }
        U32 textureCount() const { return mTextureCount; }
        U64 textureBytes() const { return mTextureBytes; }

    private:
        // How makeBuffer writes the vertices.
        struct BufferStyle
        {
            bool bake_translation = false;  // each command's translation added to its vertices (a packed run)
            F32 alpha = 1.f;                // the floater's opacity, multiplied into the colours
            bool straight = false;          // the colours taken back to straight alpha (a loaded texture's run)
        };

        void buildRuns(DrawList& list, F32 alpha);
        // run (packed, its content and commands set) takes the buffer of an old run with the same; false: none.
        bool takeOldBuffer(DrawList& list, DrawList::Run& run);
        LLPointer<LLVertexBuffer> makeBuffer(const DrawList::Command* commands, size_t count, const BufferStyle& style,
                                             U32& vertices, U32& indices);
        void setScissor(const DrawFrame& frame, const Rml::Rectanglei* region) const;
        void invalidateUsers(const Geometry* geometry, const Texture* texture);
        Texture* newTexture();

        std::vector<DrawList*> mLists;
        DrawList* mRecording = nullptr;
        bool mRecordingSpoiled = false;     // something the recording names was released while it ran
        bool mScissorOn = false;
        Rml::Rectanglei mScissor;
        S32 mScissorIndex = -1;     // mScissor's index in the recording, -1: not looked up since it was set
        bool mTransformOn = false;
        Rml::Matrix4f mTransform;
        S32 mTransformIndex = -1;
        U64 mNextSerial = 1;
        U32 mGeometryCount = 0;
        U32 mTextureCount = 0;
        U64 mTextureBytes = 0;
    };
}

#endif // PLUS_RMLRENDER_H
