/**
 * @file rmlrender.cpp
 * @brief RmlUi's render interface on the official UI path (research/364 2.1, way B).
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

#include "rmlrender.h"

#include "rmlsystem.h"

#include "hbxxh.h"
#include "llimage.h"
#include "llimagegl.h"
#include "llrender.h"
#include "llrhigl.h"
#include "llui.h"
#include "llviewertexture.h"
#include "lluiimage.h"
#include "llvertexbuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace plus::rmlui
{
    struct Geometry
    {
        Rml::Span<const Rml::Vertex> vertices;  // RmlUi keeps them valid and unchanged until ReleaseGeometry
        Rml::Span<const int> indices;
        U64 serial = 0;
        LLPointer<LLVertexBuffer> buffer;       // its own, made the first time it is drawn as a run of one
        U32 buffer_vertices = 0;
        U32 buffer_indices = 0;
    };

    struct Texture
    {
        LLPointer<LLImageGL> image;             // generated (the glyphs): premultiplied
        LLPointer<LLUIImage> ui_image;          // loaded (a UI image): straight alpha, owned by the UI image list
        U64 serial = 0;
        U64 bytes = 0;                          // what the generated image holds (0 for a UI image)
        bool premultiplied = true;
        // A UI image: the part of the GL texture the picture fills, from the left and the bottom (the texture is
        // kept bottom-up, and a local file is padded to a power of two with its picture in the lower left). 1 x 1: all.
        F32 uv_x = 1.f;
        F32 uv_y = 1.f;
    };

    namespace
    {
        constexpr U32 BUFFER_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_COLOR;
        // A run packs commands up to the vertices 16 bit indices reach; one geometry above it gets 32 bit indices.
        constexpr size_t MAX_16BIT_VERTICES = 65536;
        constexpr S32 MAX_TEXTURE_SIDE = 8192;
        constexpr size_t MAX_UI_IMAGE_NAME = 64;

        // makeBuffer's arrays (main thread), grown and kept while RmlUi runs: no allocation once they are big enough.
        std::vector<LLVector4a> sPositions;
        std::vector<LLVector2> sTexCoords;
        std::vector<LLColor4U> sColors;
        std::vector<U16> sIndices16;
        std::vector<U32> sIndices32;

        template <typename T>
        void releaseArray(std::vector<T>& array)
        {
            std::vector<T>().swap(array);
        }

        U8 scaled(U8 value, F32 factor)
        {
            return (U8)llclamp(ll_round((F32)value * factor), 0, 255);
        }

        // A vertex colour for the run: premultiplied (times the opacity), or taken back to straight alpha for a
        // loaded texture's SOURCE_ALPHA blend.
        LLColor4U vertexColour(const Rml::ColourbPremultiplied& c, F32 alpha, bool straight)
        {
            if (straight)
            {
                if (c.alpha == 0)
                {
                    return LLColor4U(0, 0, 0, 0);
                }
                auto unmultiplied = [&c](U8 v) { return (U8)llmin(255, ((S32)v * 255 + c.alpha / 2) / (S32)c.alpha); };
                return LLColor4U(unmultiplied(c.red), unmultiplied(c.green), unmultiplied(c.blue), scaled(c.alpha, alpha));
            }
            if (alpha >= 1.f)
            {
                return LLColor4U(c.red, c.green, c.blue, c.alpha);
            }
            return LLColor4U(scaled(c.red, alpha), scaled(c.green, alpha), scaled(c.blue, alpha), scaled(c.alpha, alpha));
        }

        bool sameCommand(const DrawList::Command& a, const DrawList::Command& b)
        {
            return a.geometry_serial == b.geometry_serial && a.texture_serial == b.texture_serial && a.scissor == b.scissor &&
                   a.transform == b.transform && a.translation == b.translation;
        }

        // Packed into one run: the same texture, scissor and transform.
        bool sameState(const DrawList::Command& a, const DrawList::Command& b)
        {
            return a.texture == b.texture && a.scissor == b.scissor && a.transform == b.transform;
        }

        template <typename T>
        S32 indexOf(std::vector<T>& list, const T& value)
        {
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (list[i] == value)
                {
                    return (S32)i;
                }
            }
            list.push_back(value);
            return (S32)list.size() - 1;
        }

        // What a packed run's buffer holds: each command's geometry (by serial: a geometry never changes) and
        // translation (baked in), the opacity and the colour form. A 64 bit hash of them.
        U64 runContent(const DrawList::Command* commands, size_t count, F32 alpha, bool straight)
        {
            HBXXH64 hash;
            for (size_t c = 0; c < count; ++c)
            {
                hash.update(&commands[c].geometry_serial, sizeof(commands[c].geometry_serial));
                hash.update(&commands[c].texture_serial, sizeof(commands[c].texture_serial));    // (its picture's place in it)
                hash.update(&commands[c].translation.x, sizeof(F32));
                hash.update(&commands[c].translation.y, sizeof(F32));
            }
            hash.update(&alpha, sizeof(alpha));
            const U8 form = straight ? 1 : 0;
            hash.update(&form, sizeof(form));
            hash.finalize();
            return hash.digest();
        }

        template <typename T>
        void sortUnique(std::vector<const T*>& items)
        {
            std::sort(items.begin(), items.end());
            items.erase(std::unique(items.begin(), items.end()), items.end());
        }

        template <typename T>
        bool uses(const std::vector<const T*>& sorted, const T* item)
        {
            return item && std::binary_search(sorted.begin(), sorted.end(), item);
        }
    }

    // ---- DrawList -------------------------------------------------------------------------------------

    DrawList::DrawList() = default;
    DrawList::~DrawList() = default;

    // ---- Renderer -------------------------------------------------------------------------------------

    Renderer::Renderer() = default;

    Renderer::~Renderer()
    {
        // Rml::Shutdown has released every geometry and texture by now (its render manager reports leaks).
        if (mGeometryCount || mTextureCount)
        {
            LL_WARNS("RmlUi") << "RmlUi left " << mGeometryCount << " geometries and " << mTextureCount << " textures" << LL_ENDL;
        }
        releaseArray(sPositions);
        releaseArray(sTexCoords);
        releaseArray(sColors);
        releaseArray(sIndices16);
        releaseArray(sIndices32);
    }

    Rml::CompiledGeometryHandle Renderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            return 0;   // nothing to draw: RmlUi draws nothing for it
        }
        Geometry* geometry = new Geometry;
        geometry->vertices = vertices;
        geometry->indices = indices;
        geometry->serial = mNextSerial++;
        ++mGeometryCount;
        Stats& counts = stats();
        ++counts.compiles;
        counts.compile_bytes += vertices.size() * sizeof(Rml::Vertex) + indices.size() * sizeof(int);
        return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry);
    }

    void Renderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture_handle)
    {
        ++stats().render_geometry;
        if (!mRecording || !handle)
        {
            if (!mRecording)
            {
                LL_WARNS_ONCE("RmlUi") << "RmlUi drew outside a host's recording (ignored)" << LL_ENDL;
            }
            return;
        }
        Geometry* geometry = reinterpret_cast<Geometry*>(handle);
        const Texture* texture = reinterpret_cast<const Texture*>(texture_handle);
        DrawList& list = *mRecording;
        if (mScissorOn && mScissorIndex < 0)
        {
            mScissorIndex = indexOf(list.mNextScissors, mScissor);
        }
        if (mTransformOn && mTransformIndex < 0)
        {
            mTransformIndex = indexOf(list.mNextTransforms, mTransform);
        }
        DrawList::Command command;
        command.geometry = geometry;
        command.geometry_serial = geometry->serial;
        command.translation = translation;
        command.texture = texture;
        command.texture_serial = texture ? texture->serial : 0;
        command.scissor = mScissorOn ? mScissorIndex : -1;
        command.transform = mTransformOn ? mTransformIndex : -1;
        list.mNextCommands.push_back(command);
    }

    void Renderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
    {
        Geometry* geometry = reinterpret_cast<Geometry*>(handle);
        if (!geometry)
        {
            return;
        }
        invalidateUsers(geometry, nullptr);
        delete geometry;    // its buffer goes with it unless a run still holds it (the run's list is invalid now)
        if (mGeometryCount)
        {
            --mGeometryCount;
        }
    }

    Texture* Renderer::newTexture()
    {
        Texture* texture = new Texture;
        texture->serial = mNextSerial++;
        ++mTextureCount;
        return texture;
    }

    Rml::TextureHandle Renderer::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
    {
        // Only UI images ("ui-image/<name>"): no file is read for a texture.
        const size_t prefix = sizeof(UI_IMAGE_PREFIX) - 1;
        const std::string name = source.compare(0, prefix, UI_IMAGE_PREFIX) == 0 ? source.substr(prefix) : std::string();
        if (!isPlainName(name, MAX_UI_IMAGE_NAME))
        {
            LL_WARNS_ONCE("RmlUi") << "RmlUi texture refused (only " << UI_IMAGE_PREFIX << "<UI image name>)" << LL_ENDL;
            return 0;
        }
        LLPointer<LLUIImage> image = LLUI::getUIImage(name);
        if (image.isNull() || image->getImage().isNull() || image->getWidth() <= 0 || image->getHeight() <= 0)
        {
            LL_WARNS_ONCE("RmlUi") << "RmlUi texture: no UI image " << name << LL_ENDL;
            return 0;
        }
        texture_dimensions = Rml::Vector2i(image->getWidth(), image->getHeight());
        // A local file is padded to a power of two (llviewertexture.cpp expandToPowerOfTwo) and the UI image list sets
        // its clip region from the original and the full size once it is loaded (llviewertexturelist.cpp
        // onUIImageLoaded). The picture's share is taken from the sizes themselves (not the clip region, which is
        // not set yet while the texture loads, and has no getter): not loaded yet, no texture (nothing is drawn for
        // that image, and RmlUi does not ask again; the UI images of the windows are loaded by the time one opens).
        F32 uv_x = 1.f;
        F32 uv_y = 1.f;
        if (LLViewerFetchedTexture* fetched = dynamic_cast<LLViewerFetchedTexture*>(image->getImage().get()))
        {
            if (fetched->getUrl().compare(0, 7, "file://") == 0)
            {
                const S32 original_w = fetched->getOriginalWidth();
                const S32 original_h = fetched->getOriginalHeight();
                const S32 full_w = fetched->getFullWidth();
                const S32 full_h = fetched->getFullHeight();
                if (original_w <= 0 || original_h <= 0 || original_w > full_w || original_h > full_h)
                {
                    LL_WARNS_ONCE("RmlUi") << "RmlUi texture: the UI image " << name << " is not loaded yet" << LL_ENDL;
                    return 0;
                }
                uv_x = (F32)original_w / (F32)full_w;
                uv_y = (F32)original_h / (F32)full_h;
                texture_dimensions = Rml::Vector2i(original_w, original_h);
            }
        }
        Texture* texture = newTexture();
        texture->uv_x = uv_x;
        texture->uv_y = uv_y;
        texture->ui_image = image;
        texture->premultiplied = false;
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    Rml::TextureHandle Renderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions)
    {
        const S32 width = dimensions.x;
        const S32 height = dimensions.y;
        if (width <= 0 || height <= 0 || width > MAX_TEXTURE_SIDE || height > MAX_TEXTURE_SIDE ||
            source.size() < (size_t)width * (size_t)height * 4)
        {
            LL_WARNS_ONCE("RmlUi") << "RmlUi texture of " << width << "x" << height << " refused" << LL_ENDL;
            return 0;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw((U16)width, (U16)height, 4);
        if (!raw->getData())
        {
            return 0;
        }
        std::memcpy(raw->getData(), source.data(), (size_t)width * (size_t)height * 4);
        LLPointer<LLImageGL> image = new LLImageGL(raw.get(), false, false);
        if (!image->getTexName())
        {
            LL_WARNS_ONCE("RmlUi") << "RmlUi texture of " << width << "x" << height << " could not be made" << LL_ENDL;
            return 0;
        }
        gGL.getTexUnit(0)->bind(image.get());
        image->setFilteringOption(LLTexUnit::TFO_BILINEAR);
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        Texture* texture = newTexture();
        texture->image = image;
        texture->bytes = (U64)width * (U64)height * 4;
        mTextureBytes += texture->bytes;
        return reinterpret_cast<Rml::TextureHandle>(texture);
    }

    void Renderer::ReleaseTexture(Rml::TextureHandle handle)
    {
        Texture* texture = reinterpret_cast<Texture*>(handle);
        if (!texture)
        {
            return;
        }
        invalidateUsers(nullptr, texture);
        mTextureBytes -= llmin(mTextureBytes, texture->bytes);
        delete texture;
        if (mTextureCount)
        {
            --mTextureCount;
        }
    }

    void Renderer::EnableScissorRegion(bool enable)
    {
        mScissorOn = enable;
        mScissorIndex = -1;
    }

    void Renderer::SetScissorRegion(Rml::Rectanglei region)
    {
        mScissor = region;
        mScissorIndex = -1;
    }

    void Renderer::SetTransform(const Rml::Matrix4f* transform)
    {
        mTransformOn = transform != nullptr;
        if (transform)
        {
            mTransform = *transform;
        }
        mTransformIndex = -1;
    }

    void Renderer::beginRecording(DrawList& list)
    {
        if (!list.mKnown)
        {
            mLists.push_back(&list);
            list.mKnown = true;
        }
        mRecording = &list;
        mRecordingSpoiled = false;
        list.mNextCommands.clear();
        list.mNextScissors.clear();
        list.mNextTransforms.clear();
        // RmlUi's render manager starts every Context::Render from no scissor and no transform (ResetState).
        mScissorOn = false;
        mTransformOn = false;
        mScissorIndex = -1;
        mTransformIndex = -1;
    }

    bool Renderer::endRecording()
    {
        if (!mRecording)
        {
            return false;
        }
        DrawList& list = *mRecording;
        mRecording = nullptr;
        if (mRecordingSpoiled)
        {
            // Something it names was released during the recording: thrown away, the list stays invalid.
            mRecordingSpoiled = false;
            list.mNextCommands.clear();
            list.mValid = false;
            list.mRunsValid = false;
            return false;
        }
        const bool same = list.mValid && list.mNextCommands.size() == list.mCommands.size() &&
                          std::equal(list.mNextCommands.begin(), list.mNextCommands.end(), list.mCommands.begin(), sameCommand) &&
                          list.mNextScissors == list.mScissors && list.mNextTransforms == list.mTransforms;
        if (!same)
        {
            list.mCommands.swap(list.mNextCommands);
            list.mScissors.swap(list.mNextScissors);
            list.mTransforms.swap(list.mNextTransforms);
            list.mRunsValid = false;
            list.mUsedGeometry.clear();
            list.mUsedTextures.clear();
            for (const DrawList::Command& command : list.mCommands)
            {
                list.mUsedGeometry.push_back(command.geometry);
                if (command.texture)
                {
                    list.mUsedTextures.push_back(command.texture);
                }
            }
            sortUnique(list.mUsedGeometry);
            sortUnique(list.mUsedTextures);
        }
        list.mValid = true;
        return true;
    }

    void Renderer::forget(DrawList& list)
    {
        if (mRecording == &list)
        {
            mRecording = nullptr;
            mRecordingSpoiled = false;
        }
        mLists.erase(std::remove(mLists.begin(), mLists.end(), &list), mLists.end());
        list.mKnown = false;
        list.mValid = false;
        list.mRunsValid = false;
        list.mRuns.clear();
        list.mOldRuns.clear();
    }

    void Renderer::invalidateUsers(const Geometry* geometry, const Texture* texture)
    {
        if (mRecording && !mRecordingSpoiled)
        {
            // Released while Context::Render runs (a glyph texture made again): what was recorded may name it.
            for (const DrawList::Command& command : mRecording->mNextCommands)
            {
                if ((geometry && command.geometry == geometry) || (texture && command.texture == texture))
                {
                    mRecordingSpoiled = true;
                    break;
                }
            }
        }
        for (DrawList* list : mLists)
        {
            if (list->mValid && (uses(list->mUsedGeometry, geometry) || uses(list->mUsedTextures, texture)))
            {
                list->mValid = false;
                list->mRunsValid = false;
            }
        }
    }

    LLPointer<LLVertexBuffer> Renderer::makeBuffer(const DrawList::Command* commands, size_t count, const BufferStyle& style,
                                                   U32& vertices, U32& indices)
    {
        vertices = 0;
        indices = 0;
        size_t total_vertices = 0;
        size_t total_indices = 0;
        for (size_t c = 0; c < count; ++c)
        {
            total_vertices += commands[c].geometry->vertices.size();
            total_indices += commands[c].geometry->indices.size();
        }
        if (!total_vertices || !total_indices || total_vertices > U32_MAX / 2 || total_indices > U32_MAX / 4)
        {
            return nullptr;
        }
        const bool wide = total_vertices > MAX_16BIT_VERTICES;
        sPositions.resize(total_vertices);
        sTexCoords.resize(total_vertices);
        sColors.resize(total_vertices);
        if (wide)
        {
            sIndices32.resize(total_indices);
        }
        else
        {
            sIndices16.resize(total_indices);
        }

        size_t v = 0;
        size_t n = 0;
        for (size_t c = 0; c < count; ++c)
        {
            const Geometry& geometry = *commands[c].geometry;
            const F32 tx = style.bake_translation ? commands[c].translation.x : 0.f;
            const F32 ty = style.bake_translation ? commands[c].translation.y : 0.f;
            const size_t geometry_vertices = geometry.vertices.size();
            // RmlUi's texture coordinates are the picture's: v 0 at its top. A UI image is a GL texture kept
            // bottom-up with its picture in the lower left (padded to a power of two: the official LLUIImage::draw
            // takes its clip region as the uv rectangle). So u runs 0..uv_x and v runs uv_y..0 over the picture.
            // The generated textures (the glyphs) are the rows RmlUi gave, top first: as they are.
            const Texture* source = commands[c].texture;
            const bool ui_image = source && source->ui_image.notNull();
            const F32 uv_x = ui_image ? source->uv_x : 1.f;
            const F32 uv_y = ui_image ? source->uv_y : 1.f;
            for (size_t k = 0; k < geometry_vertices; ++k)
            {
                const Rml::Vertex& vertex = geometry.vertices[k];
                sPositions[v + k].set(vertex.position.x + tx, vertex.position.y + ty, 0.f, 1.f);
                if (ui_image)
                {
                    sTexCoords[v + k].set(vertex.tex_coord.x * uv_x, uv_y * (1.f - vertex.tex_coord.y));
                }
                else
                {
                    sTexCoords[v + k].set(vertex.tex_coord.x, vertex.tex_coord.y);
                }
                sColors[v + k] = vertexColour(vertex.colour, style.alpha, style.straight);
            }
            for (size_t k = 0; k < geometry.indices.size(); ++k)
            {
                const int index = geometry.indices[k];
                if (index < 0 || (size_t)index >= geometry_vertices)
                {
                    LL_WARNS_ONCE("RmlUi") << "RmlUi geometry with an index out of its vertices (not drawn)" << LL_ENDL;
                    return nullptr;
                }
                if (wide)
                {
                    sIndices32[n + k] = (U32)(v + (size_t)index);
                }
                else
                {
                    sIndices16[n + k] = (U16)(v + (size_t)index);
                }
            }
            v += geometry_vertices;
            n += geometry.indices.size();
        }

        LLPointer<LLVertexBuffer> buffer = new LLVertexBuffer(BUFFER_MASK);
        // 32 bit indices take two of the 16 bit slots the buffer is made with (LLVertexBuffer::setIndexData(const U32*)).
        if (!buffer->allocateBuffer((U32)total_vertices, (U32)(wide ? total_indices * 2 : total_indices)))
        {
            LL_WARNS_ONCE("RmlUi") << "RmlUi: a vertex buffer of " << total_vertices << " vertices could not be made" << LL_ENDL;
            return nullptr;
        }
        buffer->setBuffer();
        buffer->setPositionData(sPositions.data());
        buffer->setTexCoord0Data(sTexCoords.data());
        buffer->setColorData(sColors.data());
        if (wide)
        {
            buffer->setIndexData(sIndices32.data());
        }
        else
        {
            buffer->setIndexData(sIndices16.data());
        }
        LLVertexBuffer::unbind();
        vertices = (U32)total_vertices;
        indices = (U32)total_indices;
        return buffer;
    }

    void Renderer::buildRuns(DrawList& list, F32 alpha)
    {
        // The last runs stay in mOldRuns while the new ones are made: a packed run with the same contents takes
        // its buffer instead of copying the vertices again (a caret's blink changes one run, not all of them).
        list.mOldRuns.clear();
        list.mOldRuns.swap(list.mRuns);
        const std::vector<DrawList::Command>& commands = list.mCommands;
        const bool faded = alpha < 1.f;
        size_t i = 0;
        while (i < commands.size())
        {
            const DrawList::Command& first = commands[i];
            size_t run_vertices = first.geometry->vertices.size();
            size_t j = i + 1;
            while (j < commands.size() && sameState(first, commands[j]) &&
                   run_vertices + commands[j].geometry->vertices.size() <= MAX_16BIT_VERTICES)
            {
                run_vertices += commands[j].geometry->vertices.size();
                ++j;
            }
            DrawList::Run run;
            run.texture = first.texture;
            run.scissor = first.scissor;
            run.transform = first.transform;
            run.straight = first.texture && !first.texture->premultiplied;
            if (j - i == 1 && !faded && !run.straight)
            {
                Geometry& geometry = *first.geometry;
                if (geometry.buffer.isNull())
                {
                    geometry.buffer = makeBuffer(&first, 1, BufferStyle(), geometry.buffer_vertices, geometry.buffer_indices);
                }
                run.buffer = geometry.buffer;
                run.vertices = geometry.buffer_vertices;
                run.indices = geometry.buffer_indices;
                run.translation = first.translation;
            }
            else
            {
                run.commands = (U32)(j - i);
                run.content = runContent(&commands[i], j - i, alpha, run.straight);
                if (!takeOldBuffer(list, run))
                {
                    BufferStyle style;
                    style.bake_translation = true;
                    style.alpha = alpha;
                    style.straight = run.straight;
                    run.buffer = makeBuffer(&commands[i], j - i, style, run.vertices, run.indices);
                    ++stats().run_packs;
                }
            }
            if (run.buffer.notNull())
            {
                list.mRuns.push_back(std::move(run));
            }
            i = j;
        }
        list.mOldRuns.clear();  // the old buffers no new run took go
        list.mRunsAlpha = alpha;
        list.mRunsValid = true;
        ++stats().run_builds;
    }

    bool Renderer::takeOldBuffer(DrawList& list, DrawList::Run& run)
    {
        for (DrawList::Run& old : list.mOldRuns)
        {
            if (old.buffer.notNull() && old.commands == run.commands && old.content == run.content &&
                old.straight == run.straight)
            {
                run.buffer = std::move(old.buffer);     // (old.buffer is null now: not taken twice)
                run.vertices = old.vertices;
                run.indices = old.indices;
                ++stats().run_reuses;
                return true;
            }
        }
        return false;
    }

    void Renderer::setScissor(const DrawFrame& frame, const Rml::Rectanglei* region) const
    {
        if (!region)
        {
            LLRHI::scissor(frame.clip_x, frame.clip_y, frame.clip_w, frame.clip_h);
            return;
        }
        // RmlUi's region: context pixels from the top-left (y down); the window's: from the bottom-left.
        const S32 x0 = llmax(frame.clip_x, (S32)std::floor(frame.left_px + (F32)region->Left() * frame.zoom_x));
        const S32 x1 = llmin(frame.clip_x + frame.clip_w, (S32)std::ceil(frame.left_px + (F32)region->Right() * frame.zoom_x));
        const S32 y0 = llmax(frame.clip_y, (S32)std::floor(frame.top_px - (F32)region->Bottom() * frame.zoom_y));
        const S32 y1 = llmin(frame.clip_y + frame.clip_h, (S32)std::ceil(frame.top_px - (F32)region->Top() * frame.zoom_y));
        LLRHI::scissor(x0, y0, llmax(0, x1 - x0), llmax(0, y1 - y0));
    }

    void Renderer::draw(DrawList& list, const DrawFrame& frame)
    {
        if (!list.mValid)
        {
            return;     // the host renders again first (rmlhost.cpp): a released geometry is never drawn
        }
        const F32 alpha = llclamp(frame.alpha, 0.f, 1.f);
        if (!list.mRunsValid || alpha != list.mRunsAlpha)
        {
            buildRuns(list, alpha);
        }

        LLTexUnit* unit = gGL.getTexUnit(0);
        const Texture* bound = nullptr;
        bool texture_set = false;
        S32 scissor = -2;   // none set yet
        S32 blend = -1;
        Stats& counts = stats();
        for (DrawList::Run& run : list.mRuns)
        {
            if (!texture_set || run.texture != bound)
            {
                if (run.texture && run.texture->image.notNull())
                {
                    unit->bind(run.texture->image.get());
                }
                else if (run.texture && run.texture->ui_image.notNull() && run.texture->ui_image->getImage().notNull())
                {
                    unit->bind(run.texture->ui_image->getImage().get());
                }
                else
                {
                    unit->unbind(LLTexUnit::TT_TEXTURE);    // the white texture: the vertex colour alone
                }
                bound = run.texture;
                texture_set = true;
            }
            const S32 want_blend = run.straight ? 1 : 0;
            if (want_blend != blend)
            {
                if (run.straight)
                {
                    gGL.blendFunc(LLRender::BF_SOURCE_ALPHA, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
                }
                else
                {
                    gGL.blendFunc(LLRender::BF_ONE, LLRender::BF_ONE_MINUS_SOURCE_ALPHA);
                }
                blend = want_blend;
            }
            if (run.scissor != scissor)
            {
                setScissor(frame, run.scissor >= 0 ? &list.mScissors[run.scissor] : nullptr);
                scissor = run.scissor;
            }
            const bool translated = run.translation.x != 0.f || run.translation.y != 0.f;
            const bool moved = run.transform >= 0 || translated;
            if (moved)
            {
                gGL.pushMatrix();
                if (run.transform >= 0)
                {
                    gGL.multMatrix(list.mTransforms[run.transform].data());
                }
                if (translated)
                {
                    gGL.translatef(run.translation.x, run.translation.y, 0.f);
                }
            }
            run.buffer->setBuffer();
            run.buffer->drawRange(LLRender::TRIANGLES, 0, run.vertices - 1, run.indices, 0);
            if (moved)
            {
                gGL.popMatrix();
            }
            ++counts.runs;
        }
    }
}
