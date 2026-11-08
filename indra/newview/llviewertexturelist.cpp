/**
 * @file llviewertexturelist.cpp
 * @brief Object for managing the list of images within a region
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include <sys/stat.h>

#include "llviewertexturelist.h"

#include "llagent.h"
#include "llgl.h" // fot gathering stats from GL
#include "llimagegl.h"
#include "llimagebmp.h"
#include "llimagej2c.h"
#include "llimagetga.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llimageworker.h"

#include "llsdserialize.h"
#include "llsys.h"
#include "llfilesystem.h"
#include "llxmltree.h"
#include "message.h"

#include "lldrawpoolbump.h" // to init bumpmap images
#include "lltexturecache.h"
#include "lltexturefetch.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewermedia.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "pipeline.h"
#include "llviewercamera.h"
#include "llappviewer.h"
#include "llrhi.h"
#include <algorithm>   // research/79 (census)
#include <cmath>
#include <sstream>
#include <unordered_set>
#include "llxuiparser.h"
#include "lltracerecording.h"
#include "llviewerdisplay.h"
#include "llviewerwindow.h"
#include "llprogressview.h"
// <plus>
#include "plus/tprelease/tprelease.h"
#include "plus/idleskip/idleskip.h"
#include "plus/texprio/texprio.h"   // research/285 (tex-view-priority)
#include "plus/texprio/fetchflow.h" // research/294 (tex-fetch-flow)
#include "plus/texprio/fetchretry.h"    // research/299 (tex-fetch-retry)
// </plus>

////////////////////////////////////////////////////////////////////////////

void (*LLViewerTextureList::sUUIDCallback)(void **, const LLUUID&) = NULL;

S32 LLViewerTextureList::sNumImages = 0;
S32 LLViewerTextureList::sBlurryVisible = 0;
S32 LLViewerTextureList::sCreatePending = 0;
S32 LLViewerTextureList::sCreatePendingInside = 0;

LLViewerTextureList gTextureList;

extern LLGLSLShader gCopyProgram;

ETexListType get_element_type(S32 priority)
{
    return (priority == LLViewerFetchedTexture::BOOST_ICON || priority == LLViewerFetchedTexture::BOOST_THUMBNAIL) ? TEX_LIST_SCALE : TEX_LIST_STANDARD;
}

///////////////////////////////////////////////////////////////////////////////

LLTextureKey::LLTextureKey()
: textureId(LLUUID::null),
textureType(TEX_LIST_STANDARD)
{
}

LLTextureKey::LLTextureKey(LLUUID id, ETexListType tex_type)
: textureId(id), textureType(tex_type)
{
}

///////////////////////////////////////////////////////////////////////////////

LLViewerTextureList::LLViewerTextureList()
    : mForceResetTextureStats(false),
    mInitialized(false)
{
}

void LLViewerTextureList::init()
{
    mInitialized = true ;
    sNumImages = 0;
    doPreloadImages();
}


void LLViewerTextureList::doPreloadImages()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_DEBUGS("ViewerImages") << "Preloading images..." << LL_ENDL;

    llassert_always(mInitialized) ;
    llassert_always(mImageList.empty()) ;
    llassert_always(mUUIDMap.empty()) ;

    // Set the "missing asset" image
    LLViewerFetchedTexture::sMissingAssetImagep = LLViewerTextureManager::getFetchedTextureFromFile("missing_asset.tga", FTT_LOCAL_FILE, MIPMAP_NO, LLViewerFetchedTexture::BOOST_UI);

    // Set the "white" image
    LLViewerFetchedTexture::sWhiteImagep = LLViewerTextureManager::getFetchedTextureFromFile("white.tga", FTT_LOCAL_FILE, MIPMAP_NO, LLViewerFetchedTexture::BOOST_UI);
    LLTexUnit::sWhiteTexture = LLViewerFetchedTexture::sWhiteImagep->getTexName();
    LLUIImageList* image_list = LLUIImageList::getInstance();

    // Set default particle texture
    LLViewerFetchedTexture::sDefaultParticleImagep = LLViewerTextureManager::getFetchedTextureFromFile("pixiesmall.j2c");

    // Set the default flat normal map
    // BLANK_OBJECT_NORMAL has a version on dataserver, but it has compression artifacts
    LLViewerFetchedTexture::sFlatNormalImagep =
        LLViewerTextureManager::getFetchedTextureFromFile("flatnormal.tga",
                                                          FTT_LOCAL_FILE,
                                                          MIPMAP_NO,
                                                          LLViewerFetchedTexture::BOOST_BUMP,
                                                          LLViewerTexture::FETCHED_TEXTURE,
                                                          0,
                                                          0,
                                                          BLANK_OBJECT_NORMAL);

    // PBR: irradiance
    LLViewerFetchedTexture::sDefaultIrradiancePBRp = LLViewerTextureManager::getFetchedTextureFromFile("default_irradiance.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);

    image_list->initFromFile();

    // turn off clamping and bilinear filtering for uv picking images
    //LLViewerFetchedTexture* uv_test = preloadUIImage("uv_test1.tga", LLUUID::null, false);
    //uv_test->setClamp(false, false);
    //uv_test->setMipFilterNearest(true, true);
    //uv_test = preloadUIImage("uv_test2.tga", LLUUID::null, false);
    //uv_test->setClamp(false, false);
    //uv_test->setMipFilterNearest(true, true);

    LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTextureFromFile("silhouette.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("world/NoEntryLines.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("world/NoEntryPassLines.png", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("transparent.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        0, 0, IMG_TRANSPARENT);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_WRAP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("alpha_gradient.tga", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        GL_ALPHA8, GL_ALPHA, IMG_ALPHA_GRAD);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        mImagePreloads.insert(image);
    }
    image = LLViewerTextureManager::getFetchedTextureFromFile("alpha_gradient_2d.j2c", FTT_LOCAL_FILE, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI, LLViewerTexture::FETCHED_TEXTURE,
        GL_ALPHA8, GL_ALPHA, IMG_ALPHA_GRAD_2D);
    if (image)
    {
        image->setAddressMode(LLTexUnit::TAM_CLAMP);
        mImagePreloads.insert(image);
    }
}

static std::string get_texture_list_name()
{
    if (LLGridManager::getInstance()->isInProductionGrid())
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE,
            "texture_list_" + gSavedSettings.getString("LoginLocation") + "." + gDirUtilp->getUserName() + ".xml");
    }
    else
    {
        const std::string& grid_id_str = LLGridManager::getInstance()->getGridId();
        const std::string& grid_id_lower = utf8str_tolower(grid_id_str);
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE,
            "texture_list_" + gSavedSettings.getString("LoginLocation") + "." + gDirUtilp->getUserName() + "." + grid_id_lower + ".xml");
    }
}

void LLViewerTextureList::doPrefetchImages()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    // todo: do not load without getViewerAssetUrl()
    // either fail login without caps or provide this
    // in some other way, textures won't load otherwise
    LLViewerFetchedTexture *imagep = findImage(DEFAULT_WATER_NORMAL, TEX_LIST_STANDARD);
    if (!imagep)
    {
        // add it to mImagePreloads only once
        imagep = LLViewerTextureManager::getFetchedTexture(DEFAULT_WATER_NORMAL, FTT_DEFAULT, MIPMAP_YES, LLViewerFetchedTexture::BOOST_UI);
        if (imagep)
        {
            imagep->setAddressMode(LLTexUnit::TAM_WRAP);
            mImagePreloads.insert(imagep);
        }
    }

    LLViewerTextureManager::getFetchedTexture(IMG_SHOT);
    LLViewerTextureManager::getFetchedTexture(IMG_SMOKE_POOF);
    LLViewerFetchedTexture::sSmokeImagep = LLViewerTextureManager::getFetchedTexture(IMG_SMOKE, FTT_DEFAULT, true, LLGLTexture::BOOST_UI);
    LLViewerFetchedTexture::sSmokeImagep->setNoDelete();

    LLStandardBumpmap::addstandard();

    if (LLAppViewer::instance()->getPurgeCache())
    {
        // cache was purged, no point
        return;
    }

    // Pre-fetch textures from last logout
    LLSD imagelist;
    std::string filename = get_texture_list_name();
    llifstream file;
    file.open(filename.c_str());
    if (file.is_open())
    {
        if ( ! LLSDSerialize::fromXML(imagelist, file) )
        {
            file.close();
            LL_WARNS() << "XML parse error reading texture list '" << filename << "'" << LL_ENDL;
            LL_WARNS() << "Removing invalid texture list '" << filename << "'" << LL_ENDL;
            LLFile::remove(filename);
            return;
        }
        file.close();
    }
    S32 texture_count = 0;
    for (LLSD::array_iterator iter = imagelist.beginArray();
         iter != imagelist.endArray(); ++iter)
    {
        LLSD imagesd = *iter;
        LLUUID uuid = imagesd["uuid"];
        S32 pixel_area = imagesd["area"];
        S32 texture_type = imagesd["type"];

        if((LLViewerTexture::FETCHED_TEXTURE == texture_type || LLViewerTexture::LOD_TEXTURE == texture_type))
        {
            LLViewerFetchedTexture* image = LLViewerTextureManager::getFetchedTexture(uuid, FTT_DEFAULT, MIPMAP_TRUE, LLGLTexture::BOOST_NONE, texture_type);
            if (image)
            {
                texture_count += 1;
                image->addTextureStats((F32)pixel_area);
            }
        }
    }
    LL_DEBUGS() << "fetched " << texture_count << " images from " << filename << LL_ENDL;
}

///////////////////////////////////////////////////////////////////////////////

LLViewerTextureList::~LLViewerTextureList()
{
}

void LLViewerTextureList::shutdown()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // clear out preloads
    mImagePreloads.clear();

    // Write out list of currently loaded textures for precaching on startup
    typedef std::set<std::pair<S32,LLViewerFetchedTexture*> > image_area_list_t;
    image_area_list_t image_area_list;
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); ++iter)
    {
        LLViewerFetchedTexture* image = *iter;
        if (!image->hasGLTexture() ||
            !image->getUseDiscard() ||
            image->needsAux() ||
            !image->getTargetHost().isInvalid() ||
            !image->getUrl().empty()
            )
        {
            continue; // avoid UI, baked, and other special images
        }
        if(!image->getBoundRecently())
        {
            continue ;
        }
        S32 desired = image->getDesiredDiscardLevel();
        if (desired >= 0 && desired < MAX_DISCARD_LEVEL)
        {
            S32 pixel_area = image->getWidth(desired) * image->getHeight(desired);
            image_area_list.insert(std::make_pair(pixel_area, image));
        }
    }

    LLSD imagelist;
    const S32 max_count = 1000;
    S32 count = 0;
    S32 image_type ;
    for (image_area_list_t::reverse_iterator riter = image_area_list.rbegin();
         riter != image_area_list.rend(); ++riter)
    {
        LLViewerFetchedTexture* image = riter->second;
        image_type = (S32)image->getType() ;
        imagelist[count]["area"] = riter->first;
        imagelist[count]["uuid"] = image->getID();
        imagelist[count]["type"] = image_type;
        if (++count >= max_count)
            break;
    }

    if (count > 0 && !gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "").empty())
    {
        std::string filename = get_texture_list_name();
        llofstream file;
        file.open(filename.c_str());
        LL_DEBUGS() << "saving " << imagelist.size() << " image list entries" << LL_ENDL;
        LLSDSerialize::toPrettyXML(imagelist, file);
    }

    //
    // Clean up "loaded" callbacks.
    //
    mCallbackList.clear();

    // Flush all of the references
    while (!mCreateTextureList.empty())
    {
        mCreateTextureList.front()->mCreatePending = false;
        mCreateTextureList.pop();
    }
    mFastCacheList.clear();

    mUUIDMap.clear();

    mImageList.clear();

    mInitialized = false ; //prevent loading textures again.
}

void LLViewerTextureList::dump()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_INFOS() << "LLViewerTextureList::dump()" << LL_ENDL;
    for (image_list_t::iterator it = mImageList.begin(); it != mImageList.end(); ++it)
    {
        LLViewerFetchedTexture* image = *it;

        LL_INFOS() << "priority " << image->getMaxVirtualSize()
        << " boost " << image->getBoostLevel()
        << " size " << image->getWidth() << "x" << image->getHeight()
        << " discard " << image->getDiscardLevel()
        << " desired " << image->getDesiredDiscardLevel()
        << " http://asset.siva.lindenlab.com/" << image->getID() << ".texture"
        << LL_ENDL;
    }
}

void LLViewerTextureList::destroyGL()
{
    LLImageGL::destroyGL();
}

/* Vertical tab container button image IDs
 Seem to not decode when running app in debug.

 const LLUUID BAD_IMG_ONE("1097dcb3-aef9-8152-f471-431d840ea89e");
 const LLUUID BAD_IMG_TWO("bea77041-5835-1661-f298-47e2d32b7a70");
 */

///////////////////////////////////////////////////////////////////////////////

LLViewerFetchedTexture* LLViewerTextureList::getImageFromFile(const std::string& filename,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   const LLUUID& force_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LL_PROFILE_ZONE_TEXT(filename.c_str(), filename.size());
    if(!mInitialized)
    {
        return NULL ;
    }

    std::string full_path = gDirUtilp->findSkinnedFilename("textures", filename);
    if (full_path.empty())
    {
        LL_WARNS() << "Failed to find local image file: " << filename << LL_ENDL;
        LLViewerTexture::EBoostLevel priority = LLGLTexture::BOOST_UI;
        return LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, FTT_DEFAULT, true, priority);
    }

    std::string url = "file://" + full_path;

    return getImageFromUrl(url, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, force_id);
}

LLViewerFetchedTexture* LLViewerTextureList::getImageFromUrl(const std::string& url,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   const LLUUID& force_id)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!mInitialized)
    {
        return NULL ;
    }

    // generate UUID based on hash of filename
    LLUUID new_id;
    if (force_id.notNull())
    {
        new_id = force_id;
    }
    else
    {
        new_id.generate(url);
    }

    LLPointer<LLViewerFetchedTexture> imagep = findImage(new_id, get_element_type(boost_priority));

    if (!imagep.isNull())
    {
        LLViewerFetchedTexture *texture = imagep.get();
        if (texture->getUrl().empty())
        {
            LL_WARNS() << "Requested texture " << new_id << " already exists but does not have a URL" << LL_ENDL;
        }
        else if (texture->getUrl() != url)
        {
            // This is not an error as long as the images really match -
            // e.g. could be two avatars wearing the same outfit.
            LL_DEBUGS("Avatar") << "Requested texture " << new_id
                                << " already exists with a different url, requested: " << url
                                << " current: " << texture->getUrl() << LL_ENDL;
        }

    }
    if (imagep.isNull())
    {
        switch(texture_type)
        {
        case LLViewerTexture::FETCHED_TEXTURE:
            imagep = new LLViewerFetchedTexture(url, f_type, new_id, usemipmaps);
            break ;
        case LLViewerTexture::LOD_TEXTURE:
            imagep = new LLViewerLODTexture(url, f_type, new_id, usemipmaps);
            break ;
        default:
            LL_ERRS() << "Invalid texture type " << texture_type << LL_ENDL ;
        }

        if (internal_format && primary_format)
        {
            imagep->setExplicitFormat(internal_format, primary_format);
        }

        addImage(imagep, get_element_type(boost_priority));

        if (boost_priority != 0)
        {
            if (boost_priority == LLViewerFetchedTexture::BOOST_UI)
            {
                imagep->dontDiscard();
            }
            if (boost_priority == LLViewerFetchedTexture::BOOST_ICON
                || boost_priority == LLViewerFetchedTexture::BOOST_THUMBNAIL)
            {
                // Agent and group Icons are downloadable content, nothing manages
                // icon deletion yet, so they should not persist
                imagep->dontDiscard();
                imagep->forceActive();
            }
            imagep->setBoostLevel(boost_priority);
        }
    }

    imagep->setGLTextureCreated(true);

    return imagep;
}

LLImageRaw* LLViewerTextureList::getRawImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    LLPointer<LLImageFormatted> image = LLImageFormatted::loadFromMemory(data, size, mimetype);

    if (image)
    {
        LLImageRaw* raw_image = new LLImageRaw();
        image->decode(raw_image, 0.f);
        return raw_image;
    }
    else
    {
        return nullptr;
    }
}

LLViewerFetchedTexture* LLViewerTextureList::getImageFromMemory(const U8* data, U32 size, std::string_view mimetype)
{
    LLPointer<LLImageRaw> raw_image = getRawImageFromMemory(data, size, mimetype);
    if (raw_image.notNull())
    {
        LLViewerFetchedTexture* imagep = new LLViewerFetchedTexture(raw_image, FTT_LOCAL_FILE, true);
        addImage(imagep, TEX_LIST_STANDARD);

        imagep->dontDiscard();
        imagep->setBoostLevel(LLViewerFetchedTexture::BOOST_PREVIEW);
        return imagep;
    }
    else
    {
        return nullptr;
    }
}

LLViewerFetchedTexture* LLViewerTextureList::getImage(const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   LLHost request_from_host)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!mInitialized)
    {
        return NULL ;
    }

    // Return the image with ID image_id
    // If the image is not found, creates new image and
    // enqueues a request for transmission

    if (image_id.isNull())
    {
        return (LLViewerTextureManager::getFetchedTexture(IMG_DEFAULT, FTT_DEFAULT, true, LLGLTexture::BOOST_UI));
    }

    LLPointer<LLViewerFetchedTexture> imagep = findImage(image_id, get_element_type(boost_priority));
    if (!imagep.isNull())
    {
        LLViewerFetchedTexture *texture = imagep.get();
        if (request_from_host.isOk() &&
            !texture->getTargetHost().isOk())
        {
            LL_WARNS() << "Requested texture " << image_id << " already exists but does not have a host" << LL_ENDL;
        }
        else if (request_from_host.isOk() &&
                 texture->getTargetHost().isOk() &&
                 request_from_host != texture->getTargetHost())
        {
            LL_WARNS() << "Requested texture " << image_id << " already exists with a different target host, requested: "
                    << request_from_host << " current: " << texture->getTargetHost() << LL_ENDL;
        }
        if (f_type != FTT_DEFAULT && imagep->getFTType() != f_type)
        {
            LL_WARNS() << "FTType mismatch: requested " << f_type << " image has " << imagep->getFTType() << LL_ENDL;
        }

    }
    if (imagep.isNull())
    {
        imagep = createImage(image_id, f_type, usemipmaps, boost_priority, texture_type, internal_format, primary_format, request_from_host) ;
    }

    imagep->setGLTextureCreated(true);

    return imagep;
}

//when this function is called, there is no such texture in the gTextureList with image_id.
LLViewerFetchedTexture* LLViewerTextureList::createImage(const LLUUID &image_id,
                                                   FTType f_type,
                                                   bool usemipmaps,
                                                   LLViewerTexture::EBoostLevel boost_priority,
                                                   S8 texture_type,
                                                   LLGLint internal_format,
                                                   LLGLenum primary_format,
                                                   LLHost request_from_host)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LLPointer<LLViewerFetchedTexture> imagep ;
    switch(texture_type)
    {
    case LLViewerTexture::FETCHED_TEXTURE:
        imagep = new LLViewerFetchedTexture(image_id, f_type, request_from_host, usemipmaps);
        break ;
    case LLViewerTexture::LOD_TEXTURE:
        imagep = new LLViewerLODTexture(image_id, f_type, request_from_host, usemipmaps);
        break ;
    default:
        LL_ERRS() << "Invalid texture type " << texture_type << LL_ENDL ;
    }

    if (internal_format && primary_format)
    {
        imagep->setExplicitFormat(internal_format, primary_format);
    }

    addImage(imagep, get_element_type(boost_priority));
    plus::texprio::fetchretry::noteCreated(imagep);     // <plus> research/299: made again for an id flagged missing before? </plus>

    if (boost_priority != 0)
    {
        if (boost_priority == LLViewerFetchedTexture::BOOST_UI)
        {
            imagep->dontDiscard();
        }
        if (boost_priority == LLViewerFetchedTexture::BOOST_ICON
            || boost_priority == LLViewerFetchedTexture::BOOST_THUMBNAIL)
        {
            // Agent and group Icons are downloadable content, nothing manages
            // icon deletion yet, so they should not persist.
            imagep->dontDiscard();
            imagep->forceActive();
        }
        imagep->setBoostLevel(boost_priority);
    }
    else
    {
        //by default, the texture can not be removed from memory even if it is not used.
        //here turn this off
        //if this texture should be set to NO_DELETE, call setNoDelete() afterwards.
        imagep->forceActive() ;
    }

        mFastCacheList.insert(imagep);
        imagep->setInFastCacheList(true);

    return imagep ;
}

void LLViewerTextureList::findTexturesByID(const LLUUID &image_id, std::vector<LLViewerFetchedTexture*> &output)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTextureKey search_key(image_id, TEX_LIST_STANDARD);
    uuid_map_t::iterator iter = mUUIDMap.lower_bound(search_key);
    while (iter != mUUIDMap.end() && iter->first.textureId == image_id)
    {
        output.push_back(iter->second);
        iter++;
    }
}

LLViewerFetchedTexture *LLViewerTextureList::findImage(const LLTextureKey &search_key)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    uuid_map_t::iterator iter = mUUIDMap.find(search_key);
    if (iter == mUUIDMap.end())
        return NULL;
    return iter->second;
}

LLViewerFetchedTexture *LLViewerTextureList::findImage(const LLUUID &image_id, ETexListType tex_type)
{
    return findImage(LLTextureKey(image_id, tex_type));
}

void LLViewerTextureList::addImageToList(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    assert_main_thread();
    llassert_always(mInitialized) ;
    llassert(image);
    if (image->isInImageList())
    {   // Flag is already set?
        LL_WARNS() << "LLViewerTextureList::addImageToList - image " << image->getID()  << " already in list" << LL_ENDL;
    }
    else
    {
        if (!(mImageList.insert(image)).second)
        {
            LL_WARNS() << "Error happens when insert image " << image->getID()  << " into mImageList!" << LL_ENDL ;
        }
        image->setInImageList(true);
    }
}

void LLViewerTextureList::removeImageFromList(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    assert_main_thread();
    llassert_always(mInitialized) ;
    llassert(image);

    size_t count = 0;
    if (image->isInImageList())
    {
        image->setInImageList(false);
        count = mImageList.erase(image) ;
        if(count != 1)
        {
            LL_INFOS() << "Image  " << image->getID()
                << " had mInImageList set but mImageList.erase() returned " << count
                << LL_ENDL;
        }
    }
    else
    {   // Something is wrong, image is expected in list or callers should check first
        LL_INFOS() << "Calling removeImageFromList() for " << image->getID()
            << " but doesn't have mInImageList set"
            << " ref count is " << image->getNumRefs()
            << LL_ENDL;
        uuid_map_t::iterator iter = mUUIDMap.find(LLTextureKey(image->getID(), (ETexListType)image->getTextureListType()));
        if(iter == mUUIDMap.end())
        {
            LL_INFOS() << "Image  " << image->getID() << " is also not in mUUIDMap!" << LL_ENDL ;
        }
        else if (iter->second != image)
        {
            LL_INFOS() << "Image  " << image->getID() << " was in mUUIDMap but with different pointer" << LL_ENDL ;
    }
        else
    {
            LL_INFOS() << "Image  " << image->getID() << " was in mUUIDMap with same pointer" << LL_ENDL ;
        }
        count = mImageList.erase(image) ;
        llassert(count != 0);
        if(count != 0)
        {   // it was in the list already?
            LL_WARNS() << "Image  " << image->getID()
                << " had mInImageList false but mImageList.erase() returned " << count
                << LL_ENDL;
        }
    }
}

void LLViewerTextureList::addImage(LLViewerFetchedTexture *new_image, ETexListType tex_type)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!new_image)
    {
        return;
    }
    //llassert(new_image);
    LLUUID image_id = new_image->getID();
    LLTextureKey key(image_id, tex_type);

    LLViewerFetchedTexture *image = findImage(key);
    if (image)
    {
        LL_INFOS() << "Image with ID " << image_id << " already in list" << LL_ENDL;
    }
    sNumImages++;

    addImageToList(new_image);
    mUUIDMap[key] = new_image;
    new_image->setTextureListType(tex_type);
    if (LLRHI::sTextureBudget)
    {
        mUrgentFetching.insert(key);    // first request without waiting for the round robin
    }
}


void LLViewerTextureList::deleteImage(LLViewerFetchedTexture *image)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if( image)
    {
        if (image->hasCallbacks())
        {
            mCallbackList.erase(image);
        }
        LLTextureKey key(image->getID(), (ETexListType)image->getTextureListType());
        llverify(mUUIDMap.erase(key) == 1);
        sNumImages--;
        removeImageFromList(image);
    }
}

///////////////////////////////////////////////////////////////////////////////


void LLViewerTextureList::updateImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    static bool cleared = false;
    if(gTeleportDisplay)
    {
        if(!cleared)
        {
            clearFetchingRequests();
            gPipeline.clearRebuildGroups();
            cleared = true;
            return;
        }
        // ARRIVING is a delay to let things decode, cache and process,
        // so process textures like normal despite gTeleportDisplay
        if (gAgent.getTeleportState() != LLAgent::TELEPORT_ARRIVING)
        {
            return;
        }
    }
    else
    {
        cleared = false;
    }

    LLAppViewer::getTextureFetch()->setTextureBandwidth((F32)LLTrace::get_frame_recording().getPeriodMeanPerSec(LLStatViewer::TEXTURE_NETWORK_DATA_RECEIVED).value());

    {
        using namespace LLStatViewer;
        sample(NUM_IMAGES, sNumImages);
        sample(NUM_RAW_IMAGES, LLImageRaw::sRawImageCount);
        sample(FORMATTED_MEM, F64Bytes(LLImageFormatted::sGlobalFormattedMemory));
    }

    if (LLRHI::sTextureBudget && !mCreateTextureList.empty())
    {
        // Decoded textures waiting for GL creation are the closest to showing.
        // With the fetch updates first they got a third of the time, and at a
        // heavy place hundreds of nearby textures waited minutes: twice the
        // time while any wait (still at most 50 ms), and they go first.
        max_time = llmin(max_time * 2.f, 0.050f);
    }

    // make sure each call below gets at least its "fair share" of time
    F32 min_time = max_time * 0.33f;
    F32 remaining_time = max_time;

    //loading from fast cache
    remaining_time -= updateImagesLoadingFastCache(remaining_time);
    remaining_time = llmax(remaining_time, min_time);

    if (LLRHI::sTextureBudget)
    {
        //handle results from decode threads
        remaining_time -= updateImagesCreateTextures(remaining_time * 0.6f);
        remaining_time = llmax(remaining_time, min_time);

        //dispatch to texture fetch threads
        remaining_time -= updateImagesUrgent(remaining_time * 0.5f);
        updateImagesFetchTextures(remaining_time);
    }
    else
    {
        //dispatch to texture fetch threads
        remaining_time -= updateImagesFetchTextures(remaining_time);
        remaining_time = llmax(remaining_time, min_time);

        //handle results from decode threads
        updateImagesCreateTextures(remaining_time);
    }

    bool didone = false;
    for (image_list_t::iterator iter = mCallbackList.begin();
        iter != mCallbackList.end(); )
    {
        //trigger loaded callbacks on local textures immediately
        LLViewerFetchedTexture* image = *iter++;
        if (!image->getUrl().empty())
        {
            // Do stuff to handle callbacks, update priorities, etc.
            didone = image->doLoadedCallbacks();
        }
        else if (!didone)
        {
            // Do stuff to handle callbacks, update priorities, etc.
            didone = image->doLoadedCallbacks();
        }
    }

    updateImagesUpdateStats();
    logTextureCensus();     // LLRHI (research/79)
}

// LLRHI (research/79): the texture census (LLRHI_TEX_CENSUS). Bytes are what
// the backend's images take (every level, RGB as RGBA) where it can tell
// (LLRHI::sTextureImageBytes), else the GL sizes with RGB as 4 bytes and a full
// mip chain. "need" is the discard level a texture's faces need at one texel
// per screen pixel (LLViewerFetchedTexture::mRhiScreenVSize), "unseen" the
// time since a face of it was last on screen (mRhiSeenTime).
namespace
{
    struct CensusSum
    {
        U32 count = 0;
        U64 bytes = 0;
        U64 excess = 0;     // what going down to the level named by the table would free
        void add(U64 b, U64 e = 0) { ++count; bytes += b; excess += e; }
    };

    std::string census_mb(U64 bytes)
    {
        return llformat("%.0f", bytes / (1024.0 * 1024.0));
    }

    // The bytes a texture of `bytes` frees going `levels` levels down.
    U64 census_freed(U64 bytes, S32 levels)
    {
        if (levels <= 0)
        {
            return 0;
        }
        return (U64)(bytes * (1.0 - std::pow(0.25, (F64)llmin(levels, 8))));
    }

    U64 census_estimate(const LLImageGL* gl)
    {
        // <plus> research/285: the same estimate as plus/texprio's counters (was: written out here)
        return plus::texprio::estimatedImageBytes(gl);
        // </plus>
    }

    const char* const CENSUS_KINDS[] = { "UI, HUD, preview, map", "avatar (bakes, avatar boosts)", "terrain", "sculpt map",
                                         "projector light", "legacy bump (standard maps, sources)", "rigged mesh", "particle", "mesh, diffuse",
                                         "mesh, normal/specular", "mesh, PBR", "prim, diffuse", "prim, normal/specular",
                                         "prim, PBR", "no faces", "other" };
    enum CensusKind { K_UI, K_AVATAR, K_TERRAIN, K_SCULPT, K_LIGHT, K_BUMP, K_RIGGED, K_PARTICLE, K_MESH_DIFFUSE,
                      K_MESH_MATERIAL, K_MESH_PBR, K_PRIM_DIFFUSE, K_PRIM_MATERIAL, K_PRIM_PBR, K_NO_FACES, K_OTHER, K_COUNT };

    const char* const CENSUS_REASONS[] = { "no image", "UI, HUD, preview, map", "not a LOD texture (never scaled down)",
                                           "boosted (avatar, terrain, sculpt...: never scaled down)", "no mips / don't discard",
                                           "used by over 1024 faces (full size)", "waiting to be scaled down (above desired)",
                                           "above screen need (camera boost)", "no faces looked at", "at or below screen need" };
    enum CensusReason { R_NO_IMAGE, R_UI, R_NOT_LOD, R_BOOSTED, R_NO_MIPS, R_MANY_FACES, R_ABOVE_DESIRED, R_ABOVE_NEED,
                        R_NO_SCAN, R_AT_NEED, R_COUNT };

    const char* const CENSUS_SEEN[] = { "on screen (2 s)", "unseen 2-15 s", "unseen 15-60 s", "unseen 60-300 s",
                                        "unseen over 300 s", "never seen", "no faces, or not looked at (boosted)" };
    enum CensusSeen { S_NOW, S_15, S_60, S_300, S_LONG, S_NEVER, S_NO_FACES, S_COUNT };

    const char* const CENSUS_SIZES[] = { "64 and under", "128", "256", "512", "1024", "2048 and up" };

    U32 census_size(S32 width, S32 height)
    {
        const S32 dim = llmax(width, height);
        return dim <= 64 ? 0 : dim <= 128 ? 1 : dim <= 256 ? 2 : dim <= 512 ? 3 : dim <= 1024 ? 4 : 5;
    }

    std::string census_table(const char* title, const char* const* names, const CensusSum* sums, U32 count, const char* excess)
    {
        std::ostringstream out;
        out << title << ":";
        for (U32 i = 0; i < count; ++i)
        {
            if (!sums[i].count)
            {
                continue;
            }
            out << "\n    " << names[i] << ": " << sums[i].count << " textures, " << census_mb(sums[i].bytes) << " MB";
            if (excess && sums[i].excess)
            {
                out << " (" << census_mb(sums[i].excess) << " MB " << excess << ")";
            }
        }
        return out.str();
    }
}

void LLViewerTextureList::logTextureCensus()
{
    const U32 interval = LLRHI::sTexCensusSeconds;
    static LLFrameTimer census_timer;
    if (!interval || census_timer.getElapsedTimeF32() < (F32)interval)
    {
        return;
    }
    census_timer.reset();
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTimer took;

    // The textures of the list and their image sizes (one call to the backend).
    std::vector<LLViewerFetchedTexture*> textures;
    textures.reserve(mUUIDMap.size());
    std::vector<U32> names;
    names.reserve(mUUIDMap.size());
    std::unordered_set<const LLImageGL*> listed;
    for (auto& entry : mUUIDMap)
    {
        LLViewerFetchedTexture* tex = entry.second.get();
        if (!tex)
        {
            continue;
        }
        textures.push_back(tex);
        const LLImageGL* gl = tex->getGLTexture();
        names.push_back(gl ? gl->getTexName() : 0);
        if (gl)
        {
            listed.insert(gl);
        }
    }
    std::vector<U64> real(names.size(), 0);
    const bool exact = LLRHI::sTextureImageBytes != nullptr;
    if (exact && !names.empty())
    {
        LLRHI::sTextureImageBytes(names.data(), (U32)names.size(), real.data());
    }
    // research/88: which are stored BC7.
    std::unique_ptr<bool[]> compressed(new bool[llmax(names.size(), (size_t)1)]());
    if (LLRHI::sTextureCompressed && !names.empty())
    {
        LLRHI::sTextureCompressed(names.data(), (U32)names.size(), compressed.get());
    }

    const F32 now = gFrameTimeSeconds;
    CensusSum total, kinds[K_COUNT], reasons[R_COUNT], seen[S_COUNT], sizes[6], boosts[LLGLTexture::BOOST_MAX_LEVEL + 1];
    CensusSum bigger, unseen_drop, pending, compressed_kinds[K_COUNT];
    // <plus> research/285 (tex-view-priority): by view tier, and of those the ones with no image yet
    CensusSum views[plus::texprio::TIER_COUNT];
    U32 views_no_image[plus::texprio::TIER_COUNT] = {};
    // </plus>
    struct Top
    {
        U64 bytes;
        size_t index;
        U32 kind;
        U32 reason;
        S32 need;
    };
    std::vector<Top> top;

    for (size_t i = 0; i < textures.size(); ++i)
    {
        LLViewerFetchedTexture* tex = textures[i];
        const LLImageGL* gl = tex->getGLTexture();
        const bool has_image = gl && gl->getHasGLTexture();
        const U64 estimate = census_estimate(gl);
        const U64 bytes = exact ? real[i] : estimate;
        const S32 boost = tex->getBoostLevel();

        // Kind: by boost, list and use first, then by the faces (at most 64 looked at).
        U32 face_count = 0;
        bool rigged = false, particle = false, mesh = false, diffuse = false, material = false, pbr = false;
        for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
        {
            const S32 n = tex->getNumFaces(ch);
            face_count += n;
            const LLViewerTexture::ll_face_list_t* faces = tex->getFaceList(ch);
            for (S32 f = 0; f < llmin(n, 64); ++f)
            {
                const LLFace* face = (*faces)[f];
                LLViewerObject* objp = face ? face->getViewerObject() : nullptr;
                if (!objp)
                {
                    continue;
                }
                rigged |= face->isState(LLFace::RIGGED);
                particle |= objp->getPartitionType() == LLViewerRegion::PARTITION_PARTICLE;
                mesh |= objp->isMesh();
                diffuse |= ch == LLRender::DIFFUSE_MAP;
                material |= ch == LLRender::NORMAL_MAP || ch == LLRender::SPECULAR_MAP;
                pbr |= ch >= LLRender::BASECOLOR_MAP;
            }
        }
        const FTType ft = tex->getFTType();
        U32 kind = K_OTHER;
        if ((boost >= LLGLTexture::BOOST_HUD && boost < LLGLTexture::BOOST_MAX_LEVEL) || tex->getTextureListType() == TEX_LIST_SCALE || ft == FTT_MAP_TILE)
        {
            kind = K_UI;
        }
        else if (ft == FTT_SERVER_BAKE || ft == FTT_HOST_BAKE || boost == LLGLTexture::BOOST_AVATAR ||
                 boost == LLGLTexture::BOOST_AVATAR_BAKED || boost == LLGLTexture::BOOST_AVATAR_BAKED_SELF ||
                 boost == LLGLTexture::BOOST_AVATAR_SELF)
        {
            kind = K_AVATAR;
        }
        else if (boost == LLGLTexture::BOOST_TERRAIN)
        {
            kind = K_TERRAIN;
        }
        else if (tex->getNumVolumes(LLRender::SCULPT_TEX) > 0 || boost == LLGLTexture::BOOST_SCULPTED || tex->forSculpt())
        {
            kind = K_SCULPT;
        }
        else if (tex->getNumVolumes(LLRender::LIGHT_TEX) > 0)
        {
            kind = K_LIGHT;
        }
        else if (boost == LLGLTexture::BOOST_BUMP || boost >= LLGLTexture::BOOST_MAX_LEVEL)
        {
            kind = K_BUMP;
        }
        else if (face_count == 0)
        {
            kind = K_NO_FACES;
        }
        else if (rigged)
        {
            kind = K_RIGGED;
        }
        else if (particle)
        {
            kind = K_PARTICLE;
        }
        else if (diffuse)
        {
            kind = mesh ? K_MESH_DIFFUSE : K_PRIM_DIFFUSE;
        }
        else if (pbr)
        {
            kind = mesh ? K_MESH_PBR : K_PRIM_PBR;
        }
        else if (material)
        {
            kind = mesh ? K_MESH_MATERIAL : K_PRIM_MATERIAL;
        }

        // The level its faces need on screen (-1: not known).
        const S32 current = has_image ? gl->getDiscardLevel() : -1;
        const S32 desired = tex->getDesiredDiscardLevel();
        S32 need = -1;
        const F32 full_texels = (F32)tex->getFullWidth() * (F32)tex->getFullHeight();
        if (tex->mRhiScreenVSize > 0.f && full_texels > 0.f && has_image)
        {
            const F32 level = floorf(logf(full_texels / tex->mRhiScreenVSize) / logf(4.f));
            need = llclamp((S32)level, 0, (S32)gl->getMaxDiscardLevel());
        }

        U32 reason = R_AT_NEED;
        U64 excess = 0;
        if (!has_image)
        {
            reason = R_NO_IMAGE;
        }
        else if (kind == K_UI)
        {
            reason = R_UI;
        }
        else if (tex->getType() != LLViewerTexture::LOD_TEXTURE)
        {
            reason = R_NOT_LOD;
        }
        else if (boost >= LLGLTexture::BOOST_AVATAR_BAKED)
        {
            reason = R_BOOSTED;
        }
        else if (!tex->getUseDiscard())
        {
            reason = R_NO_MIPS;
        }
        else if (face_count > 1024)
        {
            reason = R_MANY_FACES;
        }
        else if (current >= 0 && current < desired)
        {
            reason = R_ABOVE_DESIRED;
            excess = census_freed(bytes, desired - current);
            if (tex->mDownScalePending)
            {
                pending.add(bytes, excess);
            }
        }
        else if (need >= 0 && current >= 0 && current < need)
        {
            reason = R_ABOVE_NEED;
            excess = census_freed(bytes, need - current);
        }
        else if (need < 0)
        {
            reason = R_NO_SCAN;
        }

        U32 seen_bucket = S_NO_FACES;
        F32 unseen = 0.f;
        if (face_count > 0 && tex->mRhiSeenTime > 0.f)
        {
            unseen = now - tex->mRhiSeenTime;
            seen_bucket = !tex->mRhiSeenEver ? S_NEVER : unseen <= 2.f ? S_NOW : unseen <= 15.f ? S_15 : unseen <= 60.f ? S_60
                        : unseen <= 300.f ? S_300 : S_LONG;
        }
        // What texture pressure stage 2 would free (2 levels down after 60 s unseen).
        U64 unseen_excess = 0;
        if (has_image && tex->getType() == LLViewerTexture::LOD_TEXTURE && boost == LLGLTexture::BOOST_NONE &&
            tex->getUseDiscard() && face_count > 0 && !tex->mRhiRigged && unseen >= 60.f)
        {
            unseen_excess = census_freed(bytes, 2);
            unseen_drop.add(bytes, unseen_excess);
        }

        total.add(bytes);
        kinds[kind].add(bytes, excess);
        // <plus> research/285
        const U32 tier = llmin((U32)tex->mRhiViewTier, (U32)plus::texprio::TIER_COUNT - 1);
        views[tier].add(bytes);
        views_no_image[tier] += (!has_image && !tex->isMissingAsset()) ? 1 : 0;
        // </plus>
        if (compressed[i])
        {
            compressed_kinds[kind].add(bytes);
        }
        reasons[reason].add(bytes, excess);
        seen[seen_bucket].add(bytes, unseen_excess);
        if (has_image)
        {
            sizes[census_size(gl->getWidth(), gl->getHeight())].add(bytes);
        }
        boosts[llclamp(boost, 0, (S32)LLGLTexture::BOOST_MAX_LEVEL)].add(bytes);
        if (exact && has_image && bytes > estimate * 2 && bytes - estimate > 256 * 1024)
        {
            bigger.add(bytes, bytes - estimate);
        }
        if (has_image)
        {
            top.push_back({ bytes, i, kind, reason, need });
        }
    }

    // Images not in the list (dynamic textures, bakes made here, bump maps
    // made from textures, render targets made through GL, UI made from files...).
    std::vector<const LLImageGL*> others;
    std::vector<U32> other_names;
    for (const LLImageGL* gl : LLImageGL::sImageList)
    {
        if (gl && gl->getHasGLTexture() && !listed.count(gl))
        {
            others.push_back(gl);
            other_names.push_back(gl->getTexName());
        }
    }
    std::vector<U64> other_real(other_names.size(), 0);
    if (exact && !other_names.empty())
    {
        LLRHI::sTextureImageBytes(other_names.data(), (U32)other_names.size(), other_real.data());
    }
    CensusSum other_total, other_sizes[6];
    for (size_t i = 0; i < others.size(); ++i)
    {
        const U64 bytes = exact ? other_real[i] : census_estimate(others[i]);
        other_total.add(bytes);
        other_sizes[census_size(others[i]->getWidth(), others[i]->getHeight())].add(bytes);
    }

    // The largest.
    constexpr size_t TOP = 15;
    std::partial_sort(top.begin(), top.begin() + llmin(TOP, top.size()), top.end(),
                      [](const Top& a, const Top& b) { return a.bytes > b.bytes; });
    top.resize(llmin(TOP, top.size()));

    static const char* const BOOST_NAMES[] = { "none", "avatar", "avatar baked", "terrain", "4", "5", "6", "7", "8", "9", "high",
                                               "sculpted", "bump", "unused", "selected", "avatar baked self", "avatar self",
                                               "super high", "HUD", "icon", "thumbnail", "UI", "preview", "map", "map visible",
                                               "local" };
    constexpr U32 BOOST_NAME_COUNT = sizeof(BOOST_NAMES) / sizeof(BOOST_NAMES[0]);

    std::ostringstream out;
    out << "texture census (research/79): " << total.count << " textures in the list, " << census_mb(total.bytes) << " MB"
        << (exact ? " (image sizes from the backend)" : " (estimated from GL sizes)") << "; not in the list " << other_total.count
        << " images, " << census_mb(other_total.bytes) << " MB; discard bias " << LLViewerTexture::sDesiredDiscardBias
        << ", texture pressure " << LLViewerTexture::sRhiTexPressure << ", " << llformat("%.1f", took.getElapsedTimeF32() * 1000.f)
        << " ms to count";
    out << "\n  " << census_table("by kind (above desired or screen need: what scaling down would free)", CENSUS_KINDS, kinds, K_COUNT,
                                  "above desired or need");
    out << "\n  " << census_table("by reason it is not smaller", CENSUS_REASONS, reasons, R_COUNT, "would be freed");
    out << "\n  " << census_table("by last on screen (faces only; freed: 2 levels down after 60 s)", CENSUS_SEEN, seen, S_COUNT,
                                  "freed");
    out << "\n  " << census_table("by size now", CENSUS_SIZES, sizes, 6, nullptr);
    // <plus> research/285 (tex-view-priority)
    out << "\n  by view (research/285, as of each texture's last visit):";
    for (U32 t = 0; t < plus::texprio::TIER_COUNT; ++t)
    {
        out << "\n    " << plus::texprio::tierName((U8)t) << ": " << views[t].count << " textures, " << census_mb(views[t].bytes)
            << " MB, " << views_no_image[t] << " with no image yet";
    }
    // </plus>
    if (LLRHI::sTexBCNReport)
    {
        out << "\n  " << LLRHI::sTexBCNReport();
        out << "\n  " << census_table("stored BC7, by kind", CENSUS_KINDS, compressed_kinds, K_COUNT, nullptr);
    }
    std::vector<const char*> boost_names(LLGLTexture::BOOST_MAX_LEVEL + 1);
    for (U32 b = 0; b <= (U32)LLGLTexture::BOOST_MAX_LEVEL; ++b)
    {
        boost_names[b] = b < BOOST_NAME_COUNT ? BOOST_NAMES[b] : "?";
    }
    out << "\n  " << census_table("by boost", boost_names.data(), boosts, LLGLTexture::BOOST_MAX_LEVEL + 1, nullptr);
    out << "\n  waiting in the scale-down queue: " << pending.count << " textures, " << census_mb(pending.bytes) << " MB";
    out << "\n  unseen 60 s and up (pressure stage 2 would take 2 levels): " << unseen_drop.count << " textures, "
        << census_mb(unseen_drop.bytes) << " MB, " << census_mb(unseen_drop.excess) << " MB freed";
    if (exact)
    {
        out << "\n  images over twice their GL size: " << bigger.count << " textures, " << census_mb(bigger.bytes) << " MB ("
            << census_mb(bigger.excess) << " MB over)";
    }
    out << "\n  " << census_table("not in the list, by size", CENSUS_SIZES, other_sizes, 6, nullptr);
    out << "\n  largest:";
    for (const Top& t : top)
    {
        LLViewerFetchedTexture* tex = textures[t.index];
        const LLImageGL* gl = tex->getGLTexture();
        const F32 unseen = tex->mRhiSeenTime > 0.f ? now - tex->mRhiSeenTime : -1.f;
        const S32 boost = tex->getBoostLevel();
        out << "\n    " << tex->getID().asString().substr(0, 8) << " " << CENSUS_KINDS[t.kind] << ", " << gl->getWidth() << "x"
            << gl->getHeight() << " of " << tex->getFullWidth() << "x" << tex->getFullHeight() << ", " << census_mb(t.bytes)
            << " MB, discard " << gl->getDiscardLevel() << " (desired " << tex->getDesiredDiscardLevel() << ", need " << t.need
            << "), boost " << ((U32)boost < BOOST_NAME_COUNT ? BOOST_NAMES[boost] : "?") << ", "
            << CENSUS_REASONS[t.reason] << llformat(", vsize %.0f^2, screen %.0f^2, nearest %.0f m, unseen %.0f s",
                                                    sqrtf(tex->getMaxVirtualSize()), sqrtf(tex->mRhiScreenVSize),
                                                    tex->mNearestFaceDistance < 1e6f ? tex->mNearestFaceDistance : -1.f, unseen);
    }
    if (LLRHI::sPoolDetail)
    {
        out << "\n  pools: " << LLRHI::sPoolDetail();
    }
    LL_INFOS("TexCensus") << out.str() << LL_ENDL;
}

void LLViewerTextureList::clearFetchingRequests()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (LLAppViewer::getTextureFetch()->getNumRequests() == 0)
    {
        return;
    }

    LLAppViewer::getTextureFetch()->deleteAllRequests();

    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); ++iter)
    {
        LLViewerFetchedTexture* imagep = *iter;
        imagep->forceToDeleteRequest() ;
    }
}

extern bool gCubeSnapshot;

#ifndef RHI_NO_INLINE
#if LL_WINDOWS
#define RHI_NO_INLINE __declspec(noinline)
#else
#define RHI_NO_INLINE __attribute__((noinline))
#endif
#endif

// LLRHI (research/65): the fetch pass's texture visits.
// The face loop of updateImageDecodePriority (scanFaces) only reads, except for
// faces no texture visit touched for 10 frames: the loop as it was calls
// calcPixelArea on them right there. A planned scan (LLRHI_TEXTURE_VISIT_THREADS)
// runs on the cull threads for a block of textures before the main thread
// visits them; for such a face it works out what calcPixelArea would
// (LLFace::calcPixelAreaPure, the code calcPixelArea itself runs for it) and
// leaves the writes to the main thread (FaceCommit), which makes them in the
// visiting order and skips the ones an earlier visit of this frame already
// made. What an earlier visit writes to a face is what the later scan worked
// out from the same state, and nothing else a visit changes is read by the
// scans, so every texture gets the same numbers as in the serial loop. A
// texture with such a face that is rigged or has media is visited the serial way.

void rhi_run_cull_jobs(U32 count, const std::function<void(U32)>& job);   // pipeline.cpp

namespace
{
    constexpr F32 BIAS_TRS_OUT_OF_SCREEN = 1.5f;
    constexpr F32 BIAS_TRS_ON_SCREEN = 1.f;
    constexpr U32 MAX_FACES_TO_CHECK = 1024;
    // only call calcPixelArea at most once every 10 frames for a given face
    constexpr U32 FACE_UPDATE_FRAMES = 10;

    // Planned visits: textures per block (scanned together), per cull job, and
    // the fewest a frame's pass must have to plan at all.
    constexpr size_t VISIT_BLOCK = 128;
    constexpr U32 VISIT_CHUNK = 4;
    constexpr size_t VISIT_MIN_ENTRIES = 32;

    // LLRHI_TEXTURE_VISIT_STATS: sums over 5 s.
    struct VisitStats
    {
        U64 frames = 0;
        U64 textures = 0;       // in the list (mUUIDMap)
        U64 due = 0;            // handed to the fetch pass (with a GL texture)
        U64 visited = 0;        // reached before the time ran out
        U64 out_of_time = 0;    // frames the pass stopped on the time limit
        U64 planned = 0;        // visits with a planned scan
        U64 planned_unvisited = 0;  // <plus> research/179 cpu-present: scanned for a block, but the time ran out first </plus>
        U64 serial = 0;        // visits the serial way (not planned, or a rigged / media face to update)
        U64 scanned = 0;        // faces looked at
        U64 committed = 0;      // face updates written from a planned scan
        U64 urgent = 0;         // visits of updateImagesUrgent
        U64 paced = 0;          // <plus> idle-skip: settled textures left out of their round (LLRHI::sTextureVisitPace) </plus>
        U64 pace_late = 0;      // <plus> paced textures whose next visit wanted them sharper </plus>
        U64 pace_frames = 0;    // <plus> frames the fetch pass paced (camera still, no pressure) </plus>
        U64 pace_load_wait = 0; // <plus> idle-skip2: frames the pace waited for the world to settle </plus>
        U64 pace_loading = 0;   // <plus> frames something was loading (plus::idleskip::loadingNow) </plus>
        U64 pace_late_load = 0; // <plus> of pace_late, in frames waiting for the world to settle </plus>
        F64 copy_ms = 0.0;
        F64 plan_ms = 0.0;
        F64 loop_ms = 0.0;      // the fetch pass's loop (planning included)
        F64 urgent_ms = 0.0;
    };
    VisitStats sVisitStats;

    // Once a frame, after the fetch pass.
    void log_visit_stats()
    {
        static F32 logged_at = 0.f;
        if (!LLRHI::sTextureVisitStats)
        {
            sVisitStats = VisitStats();
            logged_at = gFrameTimeSeconds;
            return;
        }
        ++sVisitStats.frames;
        if (gFrameTimeSeconds - logged_at < 5.f)
        {
            return;
        }
        logged_at = gFrameTimeSeconds;
        const VisitStats& s = sVisitStats;
        const F64 f = (F64)llmax(s.frames, (U64)1);
        LL_INFOS("RHITexVisit") << llformat("texture visits over %llu frames, per frame: textures %.0f, due %.1f, visited %.1f "
                                            "(planned %.1f, serial %.1f), out of time in %.0f%% of frames (planned, not reached %.1f), faces %.0f " // <plus> research/179 cpu-present: planned, not reached </plus>
                                            "(updates written %.0f), urgent %.1f; ms: loop %.3f (planning %.3f), copy %.3f, "
                                            "urgent %.3f; threads %s",
                                            (unsigned long long)s.frames, s.textures / f, s.due / f, s.visited / f, s.planned / f,
                                            s.serial / f, 100.0 * s.out_of_time / f,
                                            s.planned_unvisited / f, // <plus> research/179 cpu-present </plus>
                                            s.scanned / f, s.committed / f, s.urgent / f,
                                            s.loop_ms / f, s.plan_ms / f, s.copy_ms / f, s.urgent_ms / f,
                                            (LLRHI::sTextureVisitThreads && LLRHI::sCullThreads) ? "on" : "off") << LL_ENDL;
        // <plus> idle-skip (research/178 G1): late is a sum over the 5 s, the rest per frame
        // idle-skip2: "waiting for loads" = frames not paced until the world settles,
        // "loading" = frames something was loading, "late while waiting" = late in those frames
        LL_INFOS("RHITexVisit") << llformat("texture visit pace [%s]: paced in %.0f%% of frames, left out %.1f, late %llu "
                                            "(while waiting %llu); waiting for loads in %.0f%% of frames, loading in %.0f%%",
                                            LLRHI::sTextureVisitPace ? "on" : "off", 100.0 * s.pace_frames / f, s.paced / f,
                                            (unsigned long long)s.pace_late, (unsigned long long)s.pace_late_load,
                                            100.0 * s.pace_load_wait / f, 100.0 * s.pace_loading / f) << LL_ENDL;
        // </plus>
        sVisitStats = VisitStats();
    }

    // <plus> idle-skip (research/178 G1, LLRHI::sTextureVisitPace): while the
    // camera stands still and nothing presses on texture memory, a texture
    // that has what it wants is visited one fetch pass round in
    // TEXTURE_PACE_ROUNDS. Its visit would find the same faces at the same
    // sizes (the camera has not moved; a moving object brings it back within
    // a few seconds), wants nothing fetched (it is at its desired level) and,
    // at a discard bias of 1 with no pressure, resets nothing. What waits: the
    // lazy flush of an unused one (unused ones are not paced), a texture
    // pressure stage's "last on screen" (pacing stops when pressure starts),
    // the teleport release (pacing stops for its window).
    constexpr U8 TEXTURE_PACE_ROUNDS = 4;
    constexpr F32 PACE_STILL_SECONDS = 1.f;
    constexpr F32 PACE_MOVE_METERS = 0.05f;
    constexpr F32 PACE_TURN_COS = 0.99996f;         // about 0.5 degrees
    constexpr F32 PACE_VIEW_CHANGE = 0.001f;        // field of view, relative

    struct CameraWatch
    {
        bool valid = false;
        LLVector3 origin;       // where it was when it last moved
        LLVector3 at;
        F32 view = 0.f;
        F32 moved_at = 0.f;
    };
    CameraWatch sCameraWatch;

    // Once a frame: whether the camera has stood (within the margins above)
    // for PACE_STILL_SECONDS.
    bool camera_still()
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3& origin = camera->getOrigin();
        const LLVector3& at = camera->getAtAxis();
        const F32 view = camera->getView();
        CameraWatch& w = sCameraWatch;
        const F32 now = gFrameTimeSeconds;
        if (!w.valid || dist_vec_squared(origin, w.origin) > PACE_MOVE_METERS * PACE_MOVE_METERS
            || at * w.at < PACE_TURN_COS || fabsf(view - w.view) > PACE_VIEW_CHANGE * llmax(w.view, 0.01f))
        {
            w.valid = true;
            w.origin = origin;
            w.at = at;
            w.view = view;
            w.moved_at = now;
            return false;
        }
        return now - w.moved_at >= PACE_STILL_SECONDS;
    }

    bool sPaceLoadWait = false;     // this frame: the pace is on and waits for the world to settle

    // Whether this frame's fetch pass paces at all. Not while the world loads
    // (idle-skip2): a new object's faces make a settled texture want more on
    // its next visit (plus::idleskip::loadSettled, a few quiet seconds first).
    bool pace_frame()
    {
        const bool still = camera_still();  // follows the camera while off too
        const bool settled = LLRHI::sTextureVisitPace && plus::idleskip::loadSettled();  // counts while the camera moves
        sPaceLoadWait = LLRHI::sTextureVisitPace && !settled;
        return settled && still && LLViewerTexture::sDesiredDiscardBias <= 1.f
            && LLViewerTexture::sRhiTexPressure == 0 && !plus::tprelease::windowOpen();
    }

    // A texture that has what it wants. In the copy out of mUUIDMap: the map
    // and the list hold it (2 references), so more is someone using it.
    bool pace_settled(LLViewerFetchedTexture* imagep)
    {
        const S32 discard = imagep->getDiscardLevel();
        return imagep->getNumRefs() > 2 && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE
            && imagep->isInImageList() && !imagep->hasFetcher() && !imagep->isMissingAsset()
            && !imagep->mDownScalePending && !imagep->mCreatePending && imagep->mRhiSeenTime > 0.f
            && discard >= 0 && discard <= imagep->getDesiredDiscardLevel()
            && imagep->getNumVolumes(LLRender::LIGHT_TEX) == 0;
    }

    // In the copy: true to leave it out of this round (counts its rounds out).
    bool pace_out(LLViewerFetchedTexture* imagep)
    {
        if (!pace_settled(imagep))
        {
            return false;   // visited; the visit sets mRhiPaceSkips back to 0
        }
        if (imagep->mRhiPaceSkips + 1 >= TEXTURE_PACE_ROUNDS)
        {
            return false;   // its round
        }
        ++imagep->mRhiPaceSkips;
        return true;
    }
    // </plus>

    // LLRHI_TEX_STATS (research/70): texture creation, sums over 5 s.
    constexpr size_t TEX_BATCH_TEXTURES = 16;              // textures per batch (LLRHI::sTexBatch)
    constexpr U64 TEX_BATCH_BYTES = 8 * 1024 * 1024;       // or pixel passes waiting
    struct TexCreateStats
    {
        U64 frames = 0;         // calls of updateImagesCreateTextures
        U64 pending = 0;        // waiting for creation when the loop began
        U64 created = 0;        // createTexture calls
        U64 redundant = 0;      // skipped (the GL texture was good enough)
        U64 batches = 0;        // fork and joins
        U64 out_of_time = 0;    // calls that stopped on the time limit
        U64 converted = 0;      // bytes converted by the native uploads in the loop
        U64 deferred = 0;       // of which on the threads
        F64 loop_ms = 0.0;      // the creation loop, main thread (joins included)
        F64 join_ms = 0.0;      // the joins
    };
    TexCreateStats sTexCreateStats;

    // Once a call of updateImagesCreateTextures.
    void log_tex_create_stats()
    {
        static F32 logged_at = 0.f;
        if (!LLRHI::sTexStats)
        {
            sTexCreateStats = TexCreateStats();
            logged_at = gFrameTimeSeconds;
            return;
        }
        ++sTexCreateStats.frames;
        if (gFrameTimeSeconds - logged_at < 5.f)
        {
            return;
        }
        logged_at = gFrameTimeSeconds;
        const TexCreateStats& s = sTexCreateStats;
        const F64 f = (F64)llmax(s.frames, (U64)1);
        const F64 mb = 1024.0 * 1024.0;
        LL_INFOS("RHITexCreate") << llformat("texture creation over %llu frames, per frame: waiting %.1f, created %.2f "
                                             "(redundant %.2f), batches %.2f, out of time in %.0f%% of frames, "
                                             "converted %.2f MB (on threads %.2f MB); ms: loop %.3f (joins %.3f); batch %s, direct %s",
                                             (unsigned long long)s.frames, s.pending / f, s.created / f, s.redundant / f,
                                             s.batches / f, 100.0 * s.out_of_time / f, s.converted / f / mb, s.deferred / f / mb,
                                             s.loop_ms / f, s.join_ms / f,
                                             LLRHI::sTexBatch ? (LLRHI::sCullThreads ? "on" : "on, no threads") : "off",
                                             LLRHI::sTexDirect ? "on" : "off") << LL_ENDL;
        sTexCreateStats = TexCreateStats();
    }

    // The texture-space part of a face's virtual size for this texture. Not
    // inlined: the serial loop and the planned scan run the same instructions
    // (the viewer builds with /fp:fast).
    // <plus> research/285 (tex-view-priority): unbiased, the same without the bias; split: the bias (the
    // face's own divisor, plus::texprio::faceDivisor) always
    RHI_NO_INLINE F32 face_virtual_size(F32 pixel_area, bool in_frustum, F32 importance, const LLTextureEntry* te,
                                        F32 bias, F32 scale_min, F32 scale_max, F32 desired_bias, F32 camera_boost,
                                        bool split, F32& unbiased)
    // </plus>
    {
        F32 vsize = pixel_area;

        // Scale desired texture resolution higher or lower depending on texture scale
        //
        // Minimum usage examples: a 1024x1024 texture with aplhabet (texture atlas),
        // runing string shows one letter at a time. If texture has ten 100px symbols
        // per side, minimal scale is (100/1024)^2 = 0.0095
        //
        // Maximum usage examples: huge chunk of terrain repeats texture
        // TODO: make this work with the GLTF texture transforms
        F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
        min_scale = llclamp(min_scale * min_scale, scale_min, scale_max);
        vsize /= min_scale;
        unbiased = vsize;   // <plus> research/285 </plus>

        // apply bias to offscreen faces all the time, but only to onscreen faces when bias is large
        // use mImportanceToCamera to make bias switch a bit more gradual
        if (split || !in_frustum || desired_bias > 1.9f + importance / 2.f)     // <plus> research/285: split || </plus>
        {
            vsize /= bias;
        }

        // boost resolution of textures that are important to the camera
        if (in_frustum)
        {
            vsize *= llmax(importance*camera_boost, 1.f);
            unbiased *= llmax(importance*camera_boost, 1.f);    // <plus> research/285 </plus>
        }
        return vsize;
    }

    RHI_NO_INLINE F32 face_distance(const LLVector3& center, const LLVector3& eye)
    {
        return dist_vec(center, eye);
    }

    // research/79: the face's area in texels at one texel per screen pixel:
    // its pixel area over the repeats (as face_virtual_size), without the
    // camera boost and the bias.
    F32 face_screen_size(F32 pixel_area, const LLTextureEntry* te, F32 scale_min, F32 scale_max)
    {
        F32 min_scale = te ? llmin(fabsf(te->getScaleS()), fabsf(te->getScaleT())) : 1.f;
        min_scale = llclamp(min_scale * min_scale, scale_min, scale_max);
        return pixel_area / min_scale;
    }

    // research/79: whether the face's drawable was visible this frame or the
    // last: to the main camera (after occlusion) or to a shadow camera (its
    // stateSort marks drawables visible too), so what casts a shadow into view
    // counts as seen. Reads only (the cull threads call it while the main
    // thread waits): LLDrawable::isVisible would write.
    bool face_seen(const LLFace* face)
    {
        const LLDrawable* drawable = face->getDrawable();
        if (!drawable)
        {
            return false;
        }
        const S32 frame = LLDrawable::getCurrentFrame();
        if (frame - (S32)drawable->getVisible() < 3)
        {
            return true;
        }
        const LLViewerOctreeGroup* group = drawable->getGroup();
        return group && frame - group->getVisible(LLViewerCamera::CAMERA_WORLD) < 3;
    }

    // research/79: the camera boost under texture pressure (stage 1 and up):
    // a face's texels at most this many times its screen pixels.
    F32 pressure_boost()
    {
        static const F32 boost = []()
        {
            const char* env = std::getenv("LLRHI_TEX_PRESSURE_BOOST");
            return env && *env ? llmax((F32)atof(env), 1.f) : 2.f;
        }();
        return boost;
    }
}

struct LLViewerTextureList::FaceScanContext
{
    LLFace::PixelAreaCamera camera;     // planned scans only
    LLVector3 eye;
    F32 scale_min;
    F32 scale_max;
    F32 camera_boost;
    F32 desired_bias;
    F32 max_virtual_size;
    F32 frame_time;
    U32 frame;
    plus::texprio::ScanValues view;     // <plus> research/285 (tex-view-priority) </plus>

    // Main thread.
    static FaceScanContext current(bool planned)
    {
        static LLCachedControl<F32> texture_scale_min(gSavedSettings, "TextureScaleMinAreaFactor", 0.0095f);
        static LLCachedControl<F32> texture_scale_max(gSavedSettings, "TextureScaleMaxAreaFactor", 25.f);
        static LLCachedControl<F32> texture_camera_boost(gSavedSettings, "TextureCameraBoost", 8.f);
        FaceScanContext context;
        if (planned)
        {
            context.camera = LLFace::PixelAreaCamera::current();
        }
        context.eye = LLViewerCamera::getInstance()->getOrigin();
        context.scale_min = texture_scale_min();
        context.scale_max = texture_scale_max();
        context.camera_boost = texture_camera_boost();
        context.desired_bias = LLViewerTexture::sDesiredDiscardBias;
        context.max_virtual_size = LLViewerFetchedTexture::sMaxVirtualSize;
        context.frame_time = gFrameTimeSeconds;
        context.frame = gFrameCount;
        context.view = plus::texprio::scanValues();     // <plus> research/285 </plus>
        return context;
    }
};

struct LLViewerTextureList::FaceScan
{
    F32 max_vsize = 0.f;
    F32 nearest = F32_MAX;
    U32 face_count = 0;
    U32 scanned = 0;        // faces looked at (counters)
    bool on_screen = false;
    // research/79: the largest face_screen_size, a face on screen, a rigged
    // face, and whether every face was looked at.
    F32 max_screen = 0.f;
    bool seen = false;
    bool rigged = false;
    bool complete = true;
    plus::texprio::FaceView view;   // <plus> research/285 (tex-view-priority): its faces against the view cone </plus>
};

// A face a planned scan found due for calcPixelArea, with what calcPixelArea
// would write. area: it works the area out; otherwise it returns at once, and
// only the face's frustum flag and touch frame change.
struct LLViewerTextureList::FaceCommit
{
    LLFace* face;
    F32 pixel_area;
    F32 importance;
    bool area;
    bool in_view = false;   // <plus> research/285: with area, LLFace::noteInView </plus>
};

struct LLViewerTextureList::VisitPlan
{
    FaceScan scan;
    U32 job = 0;            // its commits are sVisitCommits[job][first, first + count)
    U32 first = 0;
    U32 count = 0;
    S32 boost = 0;
    bool faces = false;     // scanned (not boosted)
    bool ready = false;     // false: visit the serial way
};

namespace
{
    std::vector<LLViewerTextureList::VisitPlan> sVisitPlans;
    std::vector<std::vector<LLViewerTextureList::FaceCommit> > sVisitCommits;
}

// The face loop of updateImageDecodePriority. commits null: the loop as it was
// (main thread). Otherwise it only reads (any thread, the main thread waiting)
// and returns false if a face due for calcPixelArea is rigged or has media.
//static
RHI_NO_INLINE bool LLViewerTextureList::scanFaces(LLViewerFetchedTexture* imagep, const FaceScanContext& context,
                                                  FaceScan& scan, std::vector<FaceCommit>* commits)
{
    F32 max_vsize = 0.f;
    bool on_screen = false;

    U32 face_count = 0;
    U32 scanned = 0;
    F32 nearest = F32_MAX;
    F32 max_screen = 0.f;   // research/79
    bool seen = false;
    bool rigged = false;
    bool complete = true;
    plus::texprio::FaceView view;   // <plus> research/285 </plus>

    // get adjusted bias based on image resolution
    LLImageGL* img = imagep->getGLTexture();
    F32 max_discard = F32(img ? img->getMaxDiscardLevel() : MAX_DISCARD_LEVEL);
    F32 bias = llclamp(max_discard - 2.f, 1.f, context.desired_bias);

    // convert bias into a vsize scaler
    bias = (F32) llroundf(powf(4, bias - 1.f));

    for (U32 i = 0; i < LLRender::NUM_TEXTURE_CHANNELS; ++i)
    {
        face_count += imagep->getNumFaces(i);
        S32 faces_to_check = (face_count > MAX_FACES_TO_CHECK) ? 0 : imagep->getNumFaces(i);
        if (face_count > MAX_FACES_TO_CHECK)
        {
            complete = false;
        }

        for (S32 fi = 0; fi < faces_to_check; ++fi)
        {
            LLFace* face = (*(imagep->getFaceList(i)))[fi];

            if (face && face->getViewerObject())
            {
                ++scanned;
                // <plus> research/285: the distance kept (was: nearest = llmin(nearest, face_distance(...)))
                const F32 distance = face_distance(face->mCenterAgent, context.eye);
                nearest = llmin(nearest, distance);
                // </plus>

                bool in_frustum;
                F32 pixel_area;
                F32 importance;
                // <plus> research/285: a planned scan's own view cone result for a face whose area it works out
                // (-1: none, the face's own flag then)
                S8 planned_in_view = -1;
                // </plus>
                if ((context.frame - face->mLastTextureUpdate) > FACE_UPDATE_FRAMES)
                {
                    if (!commits)
                    { // this helps eliminate redundant calls to calcPixelArea for faces that have multiple textures
                      // assigned to them, such as is the case with GLTF materials or Blinn-Phong materials
                        F32 radius;
                        F32 cos_angle_to_view_dir;
                        face->mInFrustum = face->calcPixelArea(cos_angle_to_view_dir, radius);
                        face->mLastTextureUpdate = context.frame;
                        in_frustum = face->mInFrustum;
                        pixel_area = face->getPixelArea();
                        importance = face->mImportanceToCamera;
                    }
                    else
                    {
                        FaceCommit commit;
                        commit.face = face;
                        if (face->pixelAreaFresh(context.frame_time))
                        { // calcPixelArea returns true at once
                            commit.area = false;
                            commit.pixel_area = face->getPixelArea();
                            commit.importance = face->mImportanceToCamera;
                        }
                        else if (face->isState(LLFace::RIGGED) || face->hasMedia())
                        {
                            return false;
                        }
                        else
                        {
                            F32 radius;
                            F32 cos_angle_to_view_dir;
                            // <plus> research/285: in_view
                            face->calcPixelAreaPure(context.camera, commit.pixel_area, commit.importance, cos_angle_to_view_dir, radius,
                                                    commit.in_view);
                            planned_in_view = commit.in_view ? 1 : 0;
                            // </plus>
                            commit.area = true;
                        }
                        commits->push_back(commit);
                        in_frustum = true;
                        pixel_area = commit.pixel_area;
                        importance = commit.importance;
                    }
                }
                else
                {
                    in_frustum = face->mInFrustum;
                    pixel_area = face->getPixelArea();
                    importance = face->mImportanceToCamera;
                }
                on_screen |= in_frustum;

                S32 te_offset = face->getTEOffset();  // offset is -1 if not inited
                LLViewerObject* objp = face->getViewerObject();
                const LLTextureEntry* te = (te_offset < 0 || te_offset >= objp->getNumTEs()) ? nullptr : objp->getTE(te_offset);
                // <plus> research/285: whether the face touches the view cone, and when it last did (what
                // calcPixelArea above, an earlier visit or, for a planned scan's own result, the commit writes:
                // LLFace::noteInView). A HUD attachment's faces are in view, at the camera (their place is in HUD
                // space, not the world's). The distance to the face's near side (a large face can be close).
                const bool hud = objp->isHUDAttachment();
                const bool in_view = hud || (planned_in_view < 0 ? face->mRhiInView : planned_in_view > 0);
                const F32 in_view_time = hud || (planned_in_view >= 0 && (in_view || face->mRhiInView)) ? context.frame_time
                                                                                                         : face->mRhiInViewTime;
                const F32 near_side = hud ? 0.f : llmax(distance - face->mBoundingSphereRadius, 0.f);
                // unbiased; with the bias split, the face's own divisor by view (in the cone or within the grace:
                // none; past it: more than the bias, research/285 §2.3)
                const bool in_grace = plus::texprio::inGrace(context.view, context.frame_time, in_view, in_view_time);
                const F32 face_bias = context.view.split ? plus::texprio::faceDivisor(context.view, in_grace, near_side) : bias;
                F32 unbiased;
                F32 vsize = face_virtual_size(pixel_area, in_frustum, importance, te, face_bias, context.scale_min, context.scale_max,
                                              context.desired_bias, context.camera_boost, context.view.split, unbiased);
                // </plus>

                max_vsize = llmax(max_vsize, vsize);

                // research/79
                max_screen = llmax(max_screen, face_screen_size(pixel_area, te, context.scale_min, context.scale_max));
                // <plus> research/285: face_seen asked once, only when its answer is used (was: seen = seen ||
                // face_seen(face) || objp->isAttachment();)
                const bool attachment = objp->isAttachment();
                const bool visible = attachment || ((!seen || in_view) && face_seen(face));
                seen = seen || visible;   // attachments: as on screen (impostors draw them unseen)
                // </plus>
                rigged = rigged || face->isState(LLFace::RIGGED);
                // <plus> research/285 (tex-view-priority): drawn, for a face in view (attachments by their place
                // alone: an avatar drawn as an impostor draws them unseen)
                // research/296 (tp-release, diagnostics): the grace cut by distance, a face that may have a stale centre
                // (attachments, avatars and moving drawables) apart
                // (read only while the diagnostics count: far_cut is F32_MAX otherwise, and the drawable is not touched)
                const bool far_diag = context.view.far_cut < F32_MAX;
                const bool moving = far_diag && (attachment || objp->isAvatar() || (face->getDrawable() && face->getDrawable()->isActive()));
                plus::texprio::FaceInput face_input;
                face_input.in_view = in_view;
                face_input.in_view_time = in_view_time;
                face_input.in_grace = in_grace;
                face_input.near_side = near_side;
                face_input.drawn = in_view && visible;
                face_input.unbiased = unbiased;
                face_input.moving = moving;
                face_input.would_grace = far_diag ? plus::texprio::inGraceFar(context.view, context.frame_time, face_input) : in_grace;
                face_input.hud = hud;
                face_input.far_cut = context.view.far_cut;
                plus::texprio::addFace(view, face_input);
                // </plus>

                // addTextureStats limits size to sMaxVirtualSize
                if (max_vsize >= context.max_virtual_size
                    && (on_screen || context.desired_bias <= BIAS_TRS_ON_SCREEN))
                {
                    complete = false;
                    break;
                }
            }
        }

        if (max_vsize >= context.max_virtual_size
            && (on_screen || context.desired_bias <= BIAS_TRS_ON_SCREEN))
        {
            complete = false;
            break;
        }
    }

    scan.max_vsize = max_vsize;
    scan.max_screen = max_screen;
    scan.seen = seen;
    scan.rigged = rigged;
    scan.complete = complete;
    view.all_scanned = complete && face_count > 0 && scanned == face_count;     // <plus> research/296 </plus>
    scan.view = view;   // <plus> research/285 </plus>
    scan.nearest = nearest;
    scan.face_count = face_count;
    scan.scanned = scanned;
    scan.on_screen = on_screen;
    return true;
}

void LLViewerTextureList::finishFaceScan(LLViewerFetchedTexture* imagep, const FaceScan& scan)
{
    F32 max_vsize = scan.max_vsize;
    F32 nearest = scan.nearest;
    if (scan.face_count > MAX_FACES_TO_CHECK)
    { // this texture is used in so many places we should just boost it and not bother checking its vsize
        // this is especially important because the above is not time sliced and can hit multiple ms for a single texture
        max_vsize = MAX_IMAGE_AREA;
        nearest = 0.f;
    }
    imagep->mNearestFaceDistance = nearest;

    // research/79: what the faces need on screen, and when one was last seen
    // (from the first visit on: a texture is not "unseen" before it was looked at).
    imagep->mRhiScreenVSize = (scan.complete && scan.face_count > 0) ? scan.max_screen : 0.f;
    imagep->mRhiRigged = scan.rigged;
    if (scan.seen || imagep->mRhiSeenTime <= 0.f)
    {
        imagep->mRhiSeenTime = llmax((F32)gFrameTimeSeconds, 0.001f);
    }
    imagep->mRhiSeenEver = imagep->mRhiSeenEver || scan.seen;
    const bool pressure = LLViewerTexture::sRhiTexPressure >= 1;
    // <plus> research/285 (tex-view-priority): the view tier, the size in its band, the level needed in view
    // (with the pressure cap below for an unboosted LOD texture), when a face was last in view
    const bool capped = pressure && imagep->mRhiScreenVSize > 0.f && imagep->getType() == LLViewerTexture::LOD_TEXTURE
                        && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE;
    plus::texprio::onFaceScan(imagep, scan.view, scan.face_count, scan.face_count > MAX_FACES_TO_CHECK || !scan.complete,
                              nearest, capped ? imagep->mRhiScreenVSize * pressure_boost() : F32_MAX);
    // </plus>

    if (imagep->getType() == LLViewerTexture::LOD_TEXTURE && imagep->getBoostLevel() == LLViewerTexture::BOOST_NONE)
    { // conditionally reset max virtual size for unboosted LOD_TEXTURES
      // this is an alternative to decaying mMaxVirtualSize over time
      // that keeps textures from continously downrezzing and uprezzing in the background

        // <plus> research/285 (tex-view-priority): with the bias split, the bias's reset only off screen (on
        // screen keeps what its faces asked for; the pressure cap's reset below stays), and on screen too while
        // the last resort lowers it (also with texture pressure off)
        const plus::texprio::ScanValues& view = plus::texprio::scanValues();
        const bool bias_resets = !view.split || view.on_divisor > 1.f || imagep->mRhiViewTier == plus::texprio::TIER_OFF;
        // </plus>
        if ((bias_resets && LLViewerTexture::sDesiredDiscardBias > BIAS_TRS_OUT_OF_SCREEN) ||    // <plus> research/285: bias_resets && </plus>
            (!scan.on_screen && LLViewerTexture::sDesiredDiscardBias > BIAS_TRS_ON_SCREEN) ||
            (pressure && imagep->mRhiScreenVSize > 0.f))    // research/79: so the cap below takes
        {
            imagep->mMaxVirtualSize = 0.f;
        }

        // research/79: under texture pressure no sharper than the screen can
        // show, with a margin: the level kept has at least pressure_boost (2)
        // texels per screen pixel, 1.4 across (the face's size is from its
        // bounding sphere, larger than the face). The camera boost (up to 8
        // times the pixels for near faces at the middle of the screen, 2.8
        // texels across) keeps a level more that the GPU does not sample
        // there (it picks the level by texels per pixel).
        if (pressure && imagep->mRhiScreenVSize > 0.f)
        {
            max_vsize = llmin(max_vsize, imagep->mRhiScreenVSize * pressure_boost());
        }
    }

    // <plus> research/285 (tex-view-priority): no image yet: at least the floor (its first level is asked for)
    max_vsize = plus::texprio::floorSize(imagep, max_vsize, nearest);
    // </plus>
    // <plus> teleport release (plus/tprelease): the old region's textures go down after a teleport
    plus::tprelease::onFaceScan(imagep, nearest, scan.complete && scan.face_count > 0 && scan.scanned == scan.face_count, scan.seen);
    // </plus>
    imagep->addTextureStats(max_vsize);
}

void LLViewerTextureList::updateImageDecodePriority(LLViewerFetchedTexture* imagep, bool flush_images)
{
    llassert(!gCubeSnapshot);

    if (imagep->getBoostLevel() < LLViewerFetchedTexture::BOOST_HIGH)  // don't bother checking face list for boosted textures
    {
        LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
        FaceScan scan;
        scanFaces(imagep, FaceScanContext::current(false), scan, nullptr);
        sVisitStats.scanned += scan.scanned;
        finishFaceScan(imagep, scan);
    }

#if 0
    imagep->setDebugText(llformat("%d/%d - %d/%d -- %d/%d",
        (S32)sqrtf(max_vsize),
        (S32)sqrtf(imagep->mMaxVirtualSize),
        imagep->getDiscardLevel(),
        imagep->getDesiredDiscardLevel(),
        imagep->getWidth(),
        imagep->getFullWidth()));
#endif

    finishImageDecodePriority(imagep, flush_images);
}

// Scans the faces of entries [begin, end) on the cull threads into
// sVisitPlans[0, end - begin). The main thread waits (and takes jobs too).
void LLViewerTextureList::planVisits(const std::vector<LLPointer<LLViewerFetchedTexture> >& entries, size_t begin, size_t end)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    const U32 count = (U32)(end - begin);
    const U32 jobs = (count + VISIT_CHUNK - 1) / VISIT_CHUNK;
    sVisitPlans.resize(count);
    if (sVisitCommits.size() < jobs)
    {
        sVisitCommits.resize(jobs);
    }
    const FaceScanContext context = FaceScanContext::current(true);
    rhi_run_cull_jobs(jobs, [&](U32 job)
    {
        // Raw pointers only: LLRefCount is not atomic. entries keeps the textures alive.
        std::vector<FaceCommit>& commits = sVisitCommits[job];
        commits.clear();
        const U32 last = llmin((job + 1) * VISIT_CHUNK, count);
        for (U32 i = job * VISIT_CHUNK; i < last; ++i)
        {
            VisitPlan& plan = sVisitPlans[i];
            LLViewerFetchedTexture* imagep = entries[begin + i].get();
            plan.scan = FaceScan();
            plan.job = job;
            plan.first = (U32)commits.size();
            plan.count = 0;
            plan.boost = imagep->getBoostLevel();
            plan.faces = plan.boost < LLViewerFetchedTexture::BOOST_HIGH;
            if (imagep->getNumRefs() <= 1)
            { // deleted as a side effect of an earlier visit: the main thread skips it
                plan.ready = false;
                continue;
            }
            plan.ready = !plan.faces || scanFaces(imagep, context, plan.scan, &commits);
            if (!plan.ready)
            {
                commits.resize(plan.first);
            }
            plan.count = (U32)commits.size() - plan.first;
        }
    });
}

// updateImageDecodePriority with the scan of sVisitPlans[index].
void LLViewerTextureList::visitPlanned(LLViewerFetchedTexture* imagep, size_t index)
{
    const VisitPlan& plan = sVisitPlans[index];
    if (!plan.ready || plan.boost != imagep->getBoostLevel())
    {
        ++sVisitStats.serial;
        updateImageDecodePriority(imagep);
        return;
    }
    llassert(!gCubeSnapshot);
    ++sVisitStats.planned;
    sVisitStats.scanned += plan.scan.scanned;

    // What calcPixelArea would have written when the serial loop reached the
    // face, unless a visit before this one in this frame already did.
    const std::vector<FaceCommit>& commits = sVisitCommits[plan.job];
    for (U32 i = plan.first; i < plan.first + plan.count; ++i)
    {
        const FaceCommit& commit = commits[i];
        LLFace* face = commit.face;
        if ((gFrameCount - face->mLastTextureUpdate) > FACE_UPDATE_FRAMES)
        {
            if (commit.area)
            {
                face->mPixelArea = commit.pixel_area;
                face->mLastPixelAreaUpdate = LLFace::nextPixelAreaUpdate();
                face->mImportanceToCamera = commit.importance;
                face->noteInView(commit.in_view, gFrameTimeSeconds);    // <plus> research/285 </plus>
            }
            face->mInFrustum = true;
            face->mLastTextureUpdate = gFrameCount;
            ++sVisitStats.committed;
        }
    }

    if (plan.faces)
    {
        finishFaceScan(imagep, plan.scan);
    }
    finishImageDecodePriority(imagep, true);
}

void LLViewerTextureList::finishImageDecodePriority(LLViewerFetchedTexture* imagep, bool flush_images)
{
    // make sure to addTextureStats for any spotlights that are using this texture
    for (S32 vi = 0; vi < imagep->getNumVolumes(LLRender::LIGHT_TEX); ++vi)
    {
        LLVOVolume* volume = (*imagep->getVolumeList(LLRender::LIGHT_TEX))[vi];
        volume->updateSpotLightPriority();
    }

    F32 max_inactive_time = 20.f; // inactive time before deleting saved raw image
    S32 min_refs = 3; // 1 for mImageList, 1 for mUUIDMap, and 1 for "entries" in updateImagesFetchTextures

    F32 lazy_flush_timeout = 30.f; // delete unused images after 30 seconds
    // <plus> teleport release (plus/tprelease): the old region's unused textures go sooner after a teleport
    lazy_flush_timeout = plus::tprelease::lazyFlushTimeout(imagep, lazy_flush_timeout);
    // </plus>

    //
    // Flush formatted images using a lazy flush
    //
    S32 num_refs = imagep->getNumRefs();
    if (num_refs <= min_refs && flush_images)
    {
        if (imagep->getLastReferencedTimer()->getElapsedTimeF32() > lazy_flush_timeout)
        {
            // Remove the unused image from the image list
            deleteImage(imagep);
            return;
        }
    }
    else
    {
        // still referenced outside of image list, reset timer
        imagep->getLastReferencedTimer()->reset();

        if (imagep->hasSavedRawImage())
        {
            if (imagep->getElapsedLastReferencedSavedRawImageTime() > max_inactive_time)
            {
                imagep->destroySavedRawImage();
            }
        }

        if (imagep->isDeleted())
        {
            return;
        }
    }

    if (!imagep->isInImageList())
    {
        return;
    }
    if (imagep->isInFastCacheList())
    {
        return; //wait for loading from the fast cache.
    }

    imagep->processTextureStats();
}

F32 LLViewerTextureList::updateImagesCreateTextures(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (gGLManager.mIsDisabled) return 0.0f;

    //
    // Create GL textures for all textures that need them (images which have been
    // decoded, but haven't been pushed into GL).
    //

    // Counted before this frame's creation (below, when ordering the queue).
    sCreatePendingInside = (S32)mCreateTextureList.size();
    plus::texprio::flow::noteCreateQueue(mCreateTextureList.size());   // <plus> research/294 (tex-fetch-flow) </plus>
    if (LLRHI::sTextureBudget && mCreateTextureList.size() > 1)
    {
        // Boosted textures (HUD, UI, selected) first, then the nearest: in
        // arrival order they waited behind every texture decoded before them,
        // and by size on screen large far faces (ground, big buildings) went
        // before the small things in front. Not a full sort: thousands can be
        // waiting at a heavy place, and this runs every frame outside the
        // creation time.
        constexpr size_t NEAREST_FIRST = 1024;
        std::vector<LLPointer<LLViewerFetchedTexture>> pending;
        pending.reserve(mCreateTextureList.size());
        while (!mCreateTextureList.empty())
        {
            pending.push_back(std::move(mCreateTextureList.front()));
            mCreateTextureList.pop();
        }
        auto boosted_end = std::stable_partition(pending.begin(), pending.end(),
            [](const LLPointer<LLViewerFetchedTexture>& image)
            {
                LLViewerFetchedTexture* p = image.get();
                return p && p->getBoostLevel() >= LLViewerTexture::BOOST_HIGH;
            });
        const size_t take = llmin(NEAREST_FIRST, (size_t)(pending.end() - boosted_end));
        // <plus> research/285 (tex-view-priority): near on screen, the rest on screen, off screen, each by size
        const bool by_view = plus::texprio::fetchOn();
        // </plus>
        std::partial_sort(boosted_end, boosted_end + take, pending.end(),
            [by_view](const LLPointer<LLViewerFetchedTexture>& lhs, const LLPointer<LLViewerFetchedTexture>& rhs) // <plus> research/285: by_view </plus>
            {
                LLViewerFetchedTexture* a = lhs.get();
                LLViewerFetchedTexture* b = rhs.get();
                if (!a || !b)
                {
                    return a && !b;
                }
                // <plus> research/285
                if (by_view)
                {
                    const F32 order_a = plus::texprio::createOrder(a);
                    const F32 order_b = plus::texprio::createOrder(b);
                    if (order_a != order_b)
                    {
                        return order_a > order_b;
                    }
                }
                // </plus>
                if (a->mNearestFaceDistance != b->mNearestFaceDistance)
                {
                    return a->mNearestFaceDistance < b->mNearestFaceDistance;
                }
                return a->getMaxVirtualSize() > b->getMaxVirtualSize();
            });
        sCreatePendingInside = 0;
        for (auto& image : pending)
        {
            if (image && image->mNearestFaceDistance <= gPipeline.mLoadRadius)
            {
                ++sCreatePendingInside;
            }
            mCreateTextureList.push(std::move(image));
        }
    }

    LLTimer create_timer;

    auto finish_texture = [](LLViewerFetchedTexture* imagep)
    {
        imagep->postCreateTexture();
        imagep->mCreatePending = false;

        if (imagep->hasGLTexture() && imagep->getDiscardLevel() < imagep->getDesiredDiscardLevel() &&
           (imagep->getDesiredDiscardLevel() <= MAX_DISCARD_LEVEL))
        {
            // NOTE: this may happen if the desired discard reduces while a decode is in progress and does not
            // necessarily indicate a problem, but if log occurrences excede that of dsiplay_stats: FPS,
            // something has probably gone wrong.
            LL_WARNS_ONCE("Texture") << "Texture will be downscaled immediately after loading." << LL_ENDL;
            imagep->scaleDown();
        }
    };

    // LLRHI (research/70): textures created but not finished yet. Their pixel
    // passes run together in flush_batch, then each is finished in order; the
    // list holds them (and so their raw images) until then.
    const bool batch = LLRHI::sTexBatch && LLRHI::sTexConvertJoin;
    std::vector<LLPointer<LLViewerFetchedTexture>> created;
    const size_t waiting = mCreateTextureList.size();
    const U64 converted_before = LLRHI::sTexConvertBytes.load(std::memory_order_relaxed);
    const U64 deferred_before = LLRHI::sTexConvertDeferredBytes.load(std::memory_order_relaxed);
    U32 created_count = 0, redundant_count = 0, batches = 0;
    F64 join_ms = 0.0;
    bool out_of_time = false;
    auto flush_batch = [&]()
    {
        if (created.empty())
        {
            return;
        }
        LLTimer join_timer;
        LLImageGL::finishDeferredUploads(LLRHI::sCullThreads ? &rhi_run_cull_jobs : nullptr);
        join_ms += join_timer.getElapsedTimeF64() * 1000.0;
        ++batches;
        for (LLPointer<LLViewerFetchedTexture>& image : created)
        {
            finish_texture(image);
        }
        created.clear();
    };

    while (!mCreateTextureList.empty())
    {
        // Hold a smart pointer to keep the texture alive throughout processing,
        // even if side effects (e.g. pipeline rebuilds, GL operations) indirectly
        // cause other references to be released. (see: #5426)
        LLPointer<LLViewerFetchedTexture> imagep = mCreateTextureList.front();
        mCreateTextureList.pop();

        if (!imagep)
        {
            continue;
        }

        llassert(imagep->mCreatePending);

        // desired discard may change while an image is being decoded. If the texture in VRAM is sufficient
        // for the current desired discard level, skip the texture creation.  This happens more often than it probably
        // should
        bool redundant_load = imagep->hasGLTexture() && imagep->getDiscardLevel() <= imagep->getDesiredDiscardLevel();

        if (!redundant_load)
        {
            ++created_count;
            if (batch)
            {
                LLImageGL::deferUploads(true);
            }
            imagep->createTexture();
            if (batch)
            {
                LLImageGL::deferUploads(false);
            }
        }
        else
        {
            ++redundant_count;
        }

        if (batch)
        {
            created.push_back(imagep);
            if (created.size() >= TEX_BATCH_TEXTURES || LLImageGL::deferredUploadBytes() >= TEX_BATCH_BYTES)
            {
                flush_batch();
            }
        }
        else
        {
            finish_texture(imagep);
        }

        if (create_timer.getElapsedTimeF32() > max_time)
        {
            out_of_time = true;
            break;
        }
    }
    flush_batch();

    if (LLRHI::sTexStats)
    {
        TexCreateStats& stats = sTexCreateStats;
        stats.pending += waiting;
        stats.created += created_count;
        stats.redundant += redundant_count;
        stats.batches += batches;
        stats.out_of_time += out_of_time ? 1 : 0;
        stats.converted += LLRHI::sTexConvertBytes.load(std::memory_order_relaxed) - converted_before;
        stats.deferred += LLRHI::sTexConvertDeferredBytes.load(std::memory_order_relaxed) - deferred_before;
        stats.loop_ms += create_timer.getElapsedTimeF64() * 1000.0;
        stats.join_ms += join_ms;
    }
    log_tex_create_stats();

    sCreatePending = (S32)mCreateTextureList.size();

    if ((!mDownScaleQueue.empty() || !mDownScaleQueueOff.empty()) && gPipeline.mDownResMap.isComplete())     // <plus> research/285: || the off-screen queue </plus>
    {
        LLGLDisable blend(GL_BLEND);
        gGL.setColorMask(true, true);

        // just in case we downres textures, bind downresmap and copy program
        gPipeline.mDownResMap.bindTarget();
        gCopyProgram.bind();
        gPipeline.mScreenTriangleVB->setBuffer();

        // give time to downscaling first -- if mDownScaleQueue is not empty, we're running out of memory and need
        // to free up memory by discarding off screen textures quickly

        // do at least 5 and make sure we don't get too far behind even if it violates
        // the time limit.  If we don't downscale quickly the viewer will hit swap and may
        // freeze.
        S32 min_count = (S32)mCreateTextureList.size() / 20 + 5;

        create_timer.reset();
        // <plus> research/285 (tex-view-priority): the off-screen queue first, then the rest; the time and the
        // fewest to do count across both (was: the loop below over mDownScaleQueue alone). False: out of time.
        auto scale_down_queue = [&](image_queue_t& queue)
        {
            while (!queue.empty())
            {
                LLViewerFetchedTexture* image = queue.front();
                llassert(image->mDownScalePending);

                LLImageGL* img = image->getGLTexture();
                if (img && img->getHasGLTexture())
                {
                    img->scaleDown(image->getDesiredDiscardLevel(), gPipeline.mScreenTriangleVB); // LLRHI: drawn through LLVertexBuffer
                }
                if (img && LLRHI::sTextureMoveHold)
                {
                    LLRHI::sTextureMoveHold(img->getTexName(), false);  // research/285 (compact-hysteresis): taken
                }

                image->mDownScalePending = false;
                queue.pop();

                if (create_timer.getElapsedTimeF32() > max_time && --min_count <= 0)
                {
                    return false;
                }
            }
            return true;
        };
        if (scale_down_queue(mDownScaleQueueOff))
        {
            scale_down_queue(mDownScaleQueue);
        }
        // </plus>

        gCopyProgram.unbind();
        gPipeline.mDownResMap.flush();
    }

    return create_timer.getElapsedTimeF32();
}

F32 LLViewerTextureList::updateImagesLoadingFastCache(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (gGLManager.mIsDisabled) return 0.0f;
    if(mFastCacheList.empty())
    {
        return 0.f;
    }

    //
    // loading texture raw data from the fast cache directly.
    //

    LLTimer timer;
    image_list_t::iterator enditer = mFastCacheList.begin();
    {
        // Prelock fast cache mutex to avoid waiting multiple times.
        LLMutexTrylock fast_cache_lock(LLAppViewer::getTextureCache()->getFastCacheMutex());
        if (!fast_cache_lock.isLocked())
        {
            // Cache is busy, skip this update cycle to avoid blocking the main thread.
            //
            // Generally fast cache operations are brief and rare in comparison to writing
            // main texture body, but if disk is busy, it can get stuck for multiple
            // seconds, waiting for that long is not practical.
            // But some variant of a timed try lock for 0.1ms or less might be optimal.
            return 0.0f;
        }
        for (image_list_t::iterator iter = mFastCacheList.begin();
            iter != mFastCacheList.end();)
        {
            image_list_t::iterator curiter = iter++;
            enditer = iter;
            LLViewerFetchedTexture* imagep = *curiter;
            imagep->loadFromFastCache();
            if (timer.getElapsedTimeF32() > max_time)
                break;
        }
    }
    mFastCacheList.erase(mFastCacheList.begin(), enditer);
    return timer.getElapsedTimeF32();
}

void LLViewerTextureList::forceImmediateUpdate(LLViewerFetchedTexture* imagep)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!imagep || gCubeSnapshot)
    {
        return ;
    }

    imagep->processTextureStats();

    return ;
}

F32 LLViewerTextureList::updateImagesFetchTextures(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    typedef std::vector<LLPointer<LLViewerFetchedTexture> > entries_list_t;
    entries_list_t entries;

    // update N textures at beginning of mImageList
    U32 update_count = 0;
    static const S32 MIN_UPDATE_COUNT = gSavedSettings.getS32("TextureFetchUpdateMinCount");       // default: 32

    // NOTE:  a texture may be deleted as a side effect of some of these updates
    // Deletion rules check ref count, so be careful not to hold any LLPointer references to the textures here other than the one in entries.

    //update MIN_UPDATE_COUNT or 5% of other textures, whichever is greater
    update_count = llmax((U32) MIN_UPDATE_COUNT, (U32) mUUIDMap.size()/20);
    if (LLRHI::sTextureBudget)
    {
        // About once a second for every texture, whatever the frame rate (5% a frame at 20 fps).
        const F32 interval = llclamp((F32)gFrameIntervalSeconds.value(), 0.05f, 1.f);
        update_count = llmax((U32) MIN_UPDATE_COUNT, (U32)(mUUIDMap.size() * interval));
    }
    if (LLViewerTexture::sDesiredDiscardBias > 1.f
        && LLViewerTexture::sBiasTexturesUpdated < (U32)mUUIDMap.size())
    {
        // We are over memory target. Bias affects discard rates, so update
        // existing textures agresively to free memory faster.
        update_count = (S32)(update_count * LLViewerTexture::sDesiredDiscardBias);

        // This isn't particularly precise and can overshoot, but it doesn't need
        // to be, just making sure it did a full circle and doesn't get stuck updating
        // at bias = 4 with 4 times the rate permanently.
        LLViewerTexture::sBiasTexturesUpdated += update_count;
    }
    update_count = llmin(update_count, (U32) mUUIDMap.size());

    // <plus> idle-skip (research/178 G1)
    const bool pace = pace_frame();
    sVisitStats.pace_frames += pace ? 1 : 0;
    sVisitStats.pace_load_wait += sPaceLoadWait ? 1 : 0;
    sVisitStats.pace_loading += (LLRHI::sTextureVisitPace && plus::idleskip::loadingNow()) ? 1 : 0;
    LLTextureKey pace_last_key = mLastUpdateKey;
    // </plus>
    // <plus> research/285: a texture of the round looked at (visited, or left out by the pace); at the round's end
    // the blurry count and plus/texprio's round counters (was: this block written out in both places)
    auto count_round = [this]()
    {
        if (++mBlurryVisited >= mUUIDMap.size())
        {
            sBlurryVisible = mBlurryCounting;
            mBlurryCounting = 0;
            mBlurryVisited = 0;
            plus::texprio::endRound();
        }
    };
    // </plus>

    LLTimer copy_timer;
    { // copy entries out of UUID map to avoid iterator invalidation from deletion inside updateImageDecodeProiroty or updateFetch below
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("vtluift - copy");

        // copy entries out of UUID map for updating
        entries.reserve(update_count);
        uuid_map_t::iterator iter = mUUIDMap.upper_bound(mLastUpdateKey);
        while (update_count-- > 0)
        {
            if (iter == mUUIDMap.end())
            {
                iter = mUUIDMap.begin();
            }

            if (iter->second->getGLTexture())
            {
                // <plus> idle-skip: a settled texture sits out this round (not blurry: counted as looked at)
                if (pace && pace_out(iter->second))
                {
                    ++sVisitStats.paced;
                    plus::texprio::noteVisit(iter->second);     // research/285: counted as visited
                    count_round();
                }
                else
                // </plus>
                entries.push_back(iter->second);
            }
            // <plus> idle-skip: where this round's copy ended
            pace_last_key = iter->first;
            // </plus>
            ++iter;
        }
    }

    const F64 copy_ms = copy_timer.getElapsedTimeF64() * 1000.0;

    LLTimer timer;

    // LLRHI (research/65): the faces of the next block of textures are scanned
    // on the cull threads before the main thread visits them (same numbers).
    const bool planned = LLRHI::sTextureVisitThreads && LLRHI::sCullThreads && entries.size() >= VISIT_MIN_ENTRIES;
    size_t plan_begin = 0;
    size_t plan_end = 0;
    F64 plan_ms = 0.0;
    size_t visited = 0;
    bool out_of_time = false;

    for (size_t e = 0; e < entries.size(); ++e)
    {
        LLPointer<LLViewerFetchedTexture>& imagep = entries[e];
        if (planned && e == plan_end)
        {
            const F64 plan_start = timer.getElapsedTimeF64();
            plan_begin = e;
            plan_end = llmin(e + VISIT_BLOCK, entries.size());
            planVisits(entries, plan_begin, plan_end);
            plan_ms += (timer.getElapsedTimeF64() - plan_start) * 1000.0;
        }
        ++visited;

        mLastUpdateKey = LLTextureKey(imagep->getID(), (ETexListType)imagep->getTextureListType());

        if (imagep->getNumRefs() > 1) // make sure this image hasn't been deleted before attempting to update (may happen as a side effect of some other image updating)
        {
            const S32 desired_before = imagep->getDesiredDiscardLevel(); // <plus> idle-skip </plus>
            if (planned)
            {
                visitPlanned(imagep, e - plan_begin);
            }
            else
            {
                ++sVisitStats.serial;
                updateImageDecodePriority(imagep);
            }
            imagep->updateFetch();
            // <plus> idle-skip: a paced texture's visit; late if it now wants more than it had
            if (imagep->mRhiPaceSkips)
            {
                const U64 late = imagep->getDesiredDiscardLevel() < desired_before ? 1 : 0;
                sVisitStats.pace_late += late;
                sVisitStats.pace_late_load += sPaceLoadWait ? late : 0;
                imagep->mRhiPaceSkips = 0;
            }
            // </plus>

            if (LLRHI::sTextureBudget && imagep->getNumRefs() > 1 && imagep->isInImageList() && !imagep->isMissingAsset())
            {
                // Hand textures that need following to updateImagesUrgent.
                const S32 discard = imagep->getDiscardLevel();
                if (imagep->getBoostLevel() >= LLViewerTexture::BOOST_HIGH && (discard < 0 || discard > imagep->getDesiredDiscardLevel()))
                {
                    mUrgentBoosted.insert(mLastUpdateKey);
                }
                else if (imagep->hasFetcher())
                {
                    mUrgentFetching.insert(mLastUpdateKey);
                }
            }

            constexpr F32 MIN_VISIBLE_AREA = 64.f * 64.f;
            const S32 discard = imagep->getDiscardLevel();
            if (imagep->getBoostLevel() < LLViewerTexture::BOOST_HIGH && imagep->getMaxVirtualSize() >= MIN_VISIBLE_AREA &&
                !imagep->isMissingAsset() && (discard < 0 || discard > imagep->getDesiredDiscardLevel() + 1))
            {
                ++mBlurryCounting;
            }
            plus::texprio::noteVisit(imagep);   // <plus> research/285 </plus>
        }
        count_round();  // <plus> research/285 (was: the block of count_round written out) </plus>

        if (timer.getElapsedTimeF32() > max_time)
        {
            out_of_time = e + 1 < entries.size();
            break;
        }
    }

    // <plus> idle-skip: the next round starts after this round's copy, past paced
    // textures at its end (a loop cut short starts again where it stopped)
    if (pace && !out_of_time && !mUUIDMap.empty())
    {
        mLastUpdateKey = pace_last_key;
    }
    // </plus>

    const F32 elapsed = timer.getElapsedTimeF32();
    sVisitStats.textures += mUUIDMap.size();
    sVisitStats.due += entries.size();
    sVisitStats.visited += visited;
    sVisitStats.out_of_time += out_of_time ? 1 : 0;
    sVisitStats.planned_unvisited += plan_end > visited ? plan_end - visited : 0; // <plus> research/179 cpu-present </plus>
    sVisitStats.copy_ms += copy_ms;
    sVisitStats.plan_ms += plan_ms;
    sVisitStats.loop_ms += elapsed * 1000.0;
    log_visit_stats();
    return elapsed;
}

F32 LLViewerTextureList::updateImagesUrgent(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!LLRHI::sTextureBudget)
    {
        mUrgentBoosted.clear();
        mUrgentFetching.clear();
        return 0.f;
    }

    // Every boosted texture, then fetching ones from where the last frame stopped.
    constexpr size_t MAX_FETCHING = 256;
    std::vector<std::pair<LLTextureKey, LLPointer<LLViewerFetchedTexture>>> entries;
    entries.reserve(mUrgentBoosted.size() + llmin(mUrgentFetching.size(), MAX_FETCHING));
    for (auto it = mUrgentBoosted.begin(); it != mUrgentBoosted.end(); )
    {
        auto found = mUUIDMap.find(*it);
        if (found == mUUIDMap.end() || !found->second->getGLTexture())
        {
            it = mUrgentBoosted.erase(it);
            continue;
        }
        entries.emplace_back(*it, found->second);
        ++it;
    }
    const size_t boosted_count = entries.size();
    auto it = mUrgentFetching.upper_bound(mLastUrgentKey);
    for (size_t taken = 0; taken < llmin(mUrgentFetching.size(), MAX_FETCHING) && !mUrgentFetching.empty(); ++taken)
    {
        if (it == mUrgentFetching.end())
        {
            it = mUrgentFetching.begin();
        }
        auto found = mUUIDMap.find(*it);
        if (found == mUUIDMap.end() || !found->second->getGLTexture())
        {
            it = mUrgentFetching.erase(it);
            continue;
        }
        if (mUrgentBoosted.count(*it) == 0)
        {
            entries.emplace_back(*it, found->second);
        }
        ++it;
    }

    LLTimer timer;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const LLTextureKey& key = entries[i].first;
        LLViewerFetchedTexture* imagep = entries[i].second.get();
        if (i >= boosted_count)
        {
            mLastUrgentKey = key;
        }
        if (imagep->getNumRefs() > 1)  // as in updateImagesFetchTextures
        {
            ++sVisitStats.urgent;
            updateImageDecodePriority(imagep);
            imagep->updateFetch();
        }
        // Still urgent? (deleted textures: dropped once they leave mUUIDMap)
        const bool alive = imagep->getNumRefs() > 1 && imagep->isInImageList() && !imagep->isMissingAsset();
        const S32 discard = imagep->getDiscardLevel();
        const bool boosted = alive && imagep->getBoostLevel() >= LLViewerTexture::BOOST_HIGH &&
                             (discard < 0 || discard > imagep->getDesiredDiscardLevel());
        const bool fetching = alive && !boosted && imagep->hasFetcher();
        if (boosted)
        {
            mUrgentBoosted.insert(key);
        }
        else
        {
            mUrgentBoosted.erase(key);
        }
        if (fetching)
        {
            mUrgentFetching.insert(key);
        }
        else
        {
            mUrgentFetching.erase(key);
        }
        if (timer.getElapsedTimeF32() > max_time)
        {
            break;
        }
    }
    const F32 elapsed = timer.getElapsedTimeF32();
    sVisitStats.urgent_ms += elapsed * 1000.0;
    return elapsed;
}

void LLViewerTextureList::updateImagesUpdateStats()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (mForceResetTextureStats)
    {
        for (image_list_t::iterator iter = mImageList.begin();
             iter != mImageList.end(); )
        {
            LLViewerFetchedTexture* imagep = *iter++;
            imagep->resetTextureStats();
        }
        mForceResetTextureStats = false;
    }
}

void LLViewerTextureList::decodeAllImages(F32 max_time)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLTimer timer;

    //loading from fast cache
    max_time -= updateImagesLoadingFastCache(max_time);

    // Update texture stats and priorities
    std::vector<LLPointer<LLViewerFetchedTexture> > image_list;
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        image_list.push_back(imagep);
        imagep->setInImageList(false) ;
    }

    llassert_always(image_list.size() == mImageList.size()) ;
    mImageList.clear();
    for (std::vector<LLPointer<LLViewerFetchedTexture> >::iterator iter = image_list.begin();
         iter != image_list.end(); ++iter)
    {
        LLViewerFetchedTexture* imagep = *iter;
        imagep->processTextureStats();
        addImageToList(imagep);
    }
    image_list.clear();

    // Update fetch (decode)
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        imagep->updateFetch();
    }
    std::shared_ptr<LL::WorkQueue> main_queue = LLImageGLThread::sEnabledTextures ? LL::WorkQueue::getInstance("mainloop") : NULL;
    // Run threads
    size_t fetch_pending = 0;
    while (1)
    {
        LLAppViewer::instance()->getTextureCache()->update(1); // unpauses the texture cache thread
        LLAppViewer::instance()->getImageDecodeThread()->update(1); // unpauses the image thread
        fetch_pending = LLAppViewer::instance()->getTextureFetch()->update(1); // unpauses the texture fetch thread

        if (LLImageGLThread::sEnabledTextures)
        {
            main_queue->runFor(std::chrono::milliseconds(1));
            fetch_pending += main_queue->size();
        }

        if (fetch_pending == 0 || timer.getElapsedTimeF32() > max_time)
        {
            break;
        }
    }
    // Update fetch again
    for (image_list_t::iterator iter = mImageList.begin();
         iter != mImageList.end(); )
    {
        LLViewerFetchedTexture* imagep = *iter++;
        imagep->updateFetch();
    }
    max_time -= timer.getElapsedTimeF32();
    max_time = llmax(max_time, .001f);
    F32 create_time = updateImagesCreateTextures(max_time);

    LL_DEBUGS("ViewerImages") << "decodeAllImages() took " << timer.getElapsedTimeF32() << " seconds. "
    << " fetch_pending " << fetch_pending
    << " create_time " << create_time
    << LL_ENDL;
}

bool LLViewerTextureList::createUploadFile(LLPointer<LLImageRaw> raw_image,
                                           const std::string& out_filename,
                                           const S32 max_image_dimentions,
                                           const S32 min_image_dimentions)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;

    LLImageDataSharedLock lock(raw_image);

    // make a copy, since convertToUploadFile scales raw image
    LLPointer<LLImageRaw> scale_image = new LLImageRaw(
        raw_image->getData(),
        raw_image->getWidth(),
        raw_image->getHeight(),
        raw_image->getComponents());

    LLPointer<LLImageJ2C> compressedImage = LLViewerTextureList::convertToUploadFile(scale_image, max_image_dimentions);
    if (compressedImage->getWidth() < min_image_dimentions || compressedImage->getHeight() < min_image_dimentions)
    {
        std::string reason = llformat("Images below %d x %d pixels are not allowed. Actual size: %d x %dpx",
                                      min_image_dimentions,
                                      min_image_dimentions,
                                      compressedImage->getWidth(),
                                      compressedImage->getHeight());
        compressedImage->setLastError(reason);
        return false;
    }
    if (compressedImage.isNull())
    {
        compressedImage->setLastError("Couldn't convert the image to jpeg2000.");
        LL_INFOS() << "Couldn't convert to j2c, file : " << out_filename << LL_ENDL;
        return false;
    }
    if (!compressedImage->save(out_filename))
    {
        compressedImage->setLastError("Couldn't create the jpeg2000 image for upload.");
        LL_INFOS() << "Couldn't create output file : " << out_filename << LL_ENDL;
        return false;
    }
    return true;
}

bool LLViewerTextureList::createUploadFile(const std::string& filename,
                                         const std::string& out_filename,
                                         const U8 codec,
                                         const S32 max_image_dimentions,
                                         const S32 min_image_dimentions,
                                         bool force_square)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    try
    {
    // Load the image
    LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(codec);
    if (image.isNull())
    {
        LL_WARNS() << "Couldn't open the image to be uploaded." << LL_ENDL;
        return false;
    }
    if (!image->load(filename))
    {
        image->setLastError("Couldn't load the image to be uploaded.");
        return false;
    }

    // calcDataSizeJ2C assumes maximum size is 2048 and for bigger images can
    // assign discard to bring imige to needed size, but upload does the scaling
    // as needed, so just reset discard.
    // Assume file is full and has 'discard' 0 data.
    // Todo: probably a better idea to have some setMaxDimentions in J2C
    // called when loading from a local file
    image->setDiscardLevel(0);

    // Decompress or expand it in a raw image structure
    LLPointer<LLImageRaw> raw_image = new LLImageRaw;
    if (!image->decode(raw_image, 0.0f))
    {
        image->setLastError("Couldn't decode the image to be uploaded.");
        return false;
    }
    // Check the image constraints
    if ((image->getComponents() != 3) && (image->getComponents() != 4))
    {
        image->setLastError("Image files with less than 3 or more than 4 components are not supported.");
        return false;
    }
    if (image->getWidth() < min_image_dimentions || image->getHeight() < min_image_dimentions)
    {
        std::string reason = llformat("Images below %d x %d pixels are not allowed. Actual size: %d x %dpx",
            min_image_dimentions,
            min_image_dimentions,
            image->getWidth(),
            image->getHeight());
        image->setLastError(reason);
        return false;
    }
    // Convert to j2c (JPEG2000) and save the file locally
    LLPointer<LLImageJ2C> compressedImage = convertToUploadFile(raw_image, max_image_dimentions, force_square);
    if (compressedImage.isNull())
    {
        image->setLastError("Couldn't convert the image to jpeg2000.");
        LL_INFOS() << "Couldn't convert to j2c, file : " << filename << LL_ENDL;
        return false;
    }
    if (!compressedImage->save(out_filename))
    {
        image->setLastError("Couldn't create the jpeg2000 image for upload.");
        LL_INFOS() << "Couldn't create output file : " << out_filename << LL_ENDL;
        return false;
    }
    // Test to see if the encode and save worked
    LLPointer<LLImageJ2C> integrity_test = new LLImageJ2C;
    if (!integrity_test->loadAndValidate( out_filename ))
    {
        image->setLastError("The created jpeg2000 image is corrupt.");
        LL_INFOS() << "Image file : " << out_filename << " is corrupt" << LL_ENDL;
        return false;
    }
    }
    catch (...)
    {
        LOG_UNHANDLED_EXCEPTION("");
        return false;
    }
    return true;
}

// note: modifies the argument raw_image!!!!
LLPointer<LLImageJ2C> LLViewerTextureList::convertToUploadFile(LLPointer<LLImageRaw> raw_image, const S32 max_image_dimentions, bool force_square, bool force_lossless)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLImageDataLock lock(raw_image);

    if (force_square)
    {
        S32 biggest_side = llmax(raw_image->getWidth(), raw_image->getHeight());
        S32 square_size = raw_image->biasedDimToPowerOfTwo(biggest_side, max_image_dimentions);

        raw_image->scale(square_size, square_size);
    }
    else
    {
        raw_image->biasedScaleToPowerOfTwo(max_image_dimentions);
    }
    LLPointer<LLImageJ2C> compressedImage = new LLImageJ2C();

    if (force_lossless ||
        (gSavedSettings.getBOOL("LosslessJ2CUpload") &&
            (raw_image->getWidth() * raw_image->getHeight() <= LL_IMAGE_REZ_LOSSLESS_CUTOFF * LL_IMAGE_REZ_LOSSLESS_CUTOFF)))
    {
        compressedImage->setReversible(true);
    }


    if (gSavedSettings.getBOOL("Jpeg2000AdvancedCompression"))
    {
        // This test option will create jpeg2000 images with precincts for each level, RPCL ordering
        // and PLT markers. The block size is also optionally modifiable.
        // Note: the images hence created are compatible with older versions of the viewer.
        // Read the blocks and precincts size settings
        S32 block_size = gSavedSettings.getS32("Jpeg2000BlocksSize");
        S32 precinct_size = gSavedSettings.getS32("Jpeg2000PrecinctsSize");
        LL_INFOS() << "Advanced JPEG2000 Compression: precinct = " << precinct_size << ", block = " << block_size << LL_ENDL;
        compressedImage->initEncode(*raw_image, block_size, precinct_size, 0);
    }

    if (!compressedImage->encode(raw_image, 0.0f))
    {
        LL_INFOS() << "convertToUploadFile : encode returns with error!!" << LL_ENDL;
        // Clear up the pointer so we don't leak that one
        compressedImage = NULL;
    }

    return compressedImage;
}

///////////////////////////////////////////////////////////////////////////////

// We've been that the asset server does not contain the requested image id.
// static
void LLViewerTextureList::processImageNotInDatabase(LLMessageSystem *msg,void **user_data)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    LLUUID image_id;
    msg->getUUIDFast(_PREHASH_ImageID, _PREHASH_ID, image_id);

    LLViewerFetchedTexture* image = gTextureList.findImage( image_id, TEX_LIST_STANDARD);
    if( image )
    {
        LL_WARNS() << "Image not in db" << LL_ENDL;
        image->setIsMissingAsset();
    }

    image = gTextureList.findImage(image_id, TEX_LIST_SCALE);
    if (image)
    {
        LL_WARNS() << "Icon not in db" << LL_ENDL;
        image->setIsMissingAsset();
    }
}


///////////////////////////////////////////////////////////////////////////////

// explicitly cleanup resources, as this is a singleton class with process
// lifetime so ability to perform std::map operations in destructor is not
// guaranteed.
void LLUIImageList::cleanUp()
{
    LLUIImage::cleanupClass();
    mUIImages.clear();
    mUITextureList.clear() ;
}

LLUIImagePtr LLUIImageList::getUIImageByID(const LLUUID& image_id, S32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // use id as image name
    std::string image_name = image_id.asString();

    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(image_name);
    if (found_it != mUIImages.end())
    {
        return found_it->second;
    }

    const bool use_mips = false;
    const LLRect scale_rect = LLRect::null;
    const LLRect clip_rect = LLRect::null;
    return loadUIImageByID(image_id, use_mips, scale_rect, clip_rect, (LLViewerTexture::EBoostLevel)priority);
}

LLUIImagePtr LLUIImageList::getUIImage(const std::string& image_name, S32 priority)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(image_name);
    if (found_it != mUIImages.end())
    {
        return found_it->second;
    }

    const bool use_mips = false;
    const LLRect scale_rect = LLRect::null;
    const LLRect clip_rect = LLRect::null;
    return loadUIImageByName(image_name, image_name, use_mips, scale_rect, clip_rect, (LLViewerTexture::EBoostLevel)priority);
}

LLUIImagePtr LLUIImageList::loadUIImageByName(const std::string& name, const std::string& filename,
                                              bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLViewerTexture::EBoostLevel boost_priority,
                                              LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (boost_priority == LLGLTexture::BOOST_NONE)
    {
        boost_priority = LLGLTexture::BOOST_UI;
    }
    LLViewerFetchedTexture* imagep = LLViewerTextureManager::getFetchedTextureFromFile(filename, FTT_LOCAL_FILE, MIPMAP_NO, boost_priority);
    return loadUIImage(imagep, name, use_mips, scale_rect, clip_rect, scale_style);
}

LLUIImagePtr LLUIImageList::loadUIImageByID(const LLUUID& id,
                                            bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLViewerTexture::EBoostLevel boost_priority,
                                            LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (boost_priority == LLGLTexture::BOOST_NONE)
    {
        boost_priority = LLGLTexture::BOOST_UI;
    }
    LLViewerFetchedTexture* imagep = LLViewerTextureManager::getFetchedTexture(id, FTT_DEFAULT, MIPMAP_NO, boost_priority);
    return loadUIImage(imagep, id.asString(), use_mips, scale_rect, clip_rect, scale_style);
}

LLUIImagePtr LLUIImageList::loadUIImage(LLViewerFetchedTexture* imagep, const std::string& name, bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect,
                                        LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if (!imagep) return NULL;

    imagep->setAddressMode(LLTexUnit::TAM_CLAMP);

    //don't compress UI images
    imagep->getGLTexture()->setAllowCompression(false);

    LLUIImagePtr new_imagep = new LLUIImage(name, imagep);
    new_imagep->setScaleStyle(scale_style);

    if (imagep->getBoostLevel() != LLGLTexture::BOOST_ICON
        && imagep->getBoostLevel() != LLGLTexture::BOOST_THUMBNAIL
        && imagep->getBoostLevel() != LLGLTexture::BOOST_PREVIEW)
    {
        // Don't add downloadable content into this list
        // all UI images are non-deletable and list does not support deletion
        imagep->setNoDelete();
        mUIImages.insert(std::make_pair(name, new_imagep));
        mUITextureList.push_back(imagep);
    }

    //Note:
    //Some other textures such as ICON also through this flow to be fetched.
    //But only UI textures need to set this callback.
    if(imagep->getBoostLevel() == LLGLTexture::BOOST_UI)
    {
        LLUIImageLoadData* datap = new LLUIImageLoadData;
        datap->mImageName = name;
        datap->mImageScaleRegion = scale_rect;
        datap->mImageClipRegion = clip_rect;

        imagep->setLoadedCallback(onUIImageLoaded, 0, false, false, datap, NULL);
    }
    return new_imagep;
}

LLUIImagePtr LLUIImageList::preloadUIImage(const std::string& name, const std::string& filename, bool use_mips, const LLRect& scale_rect, const LLRect& clip_rect, LLUIImage::EScaleStyle scale_style)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // look for existing image
    uuid_ui_image_map_t::iterator found_it = mUIImages.find(name);
    if (found_it != mUIImages.end())
    {
        // image already loaded!
        LL_ERRS() << "UI Image " << name << " already loaded." << LL_ENDL;
    }

    return loadUIImageByName(name, filename, use_mips, scale_rect, clip_rect, LLGLTexture::BOOST_UI, scale_style);
}

//static
void LLUIImageList::onUIImageLoaded( bool success, LLViewerFetchedTexture *src_vi, LLImageRaw* src, LLImageRaw* src_aux, S32 discard_level, bool final, void* user_data )
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    if(!success || !user_data)
    {
        return;
    }

    LLUIImageLoadData* image_datap = (LLUIImageLoadData*)user_data;
    std::string ui_image_name = image_datap->mImageName;
    LLRect scale_rect = image_datap->mImageScaleRegion;
    LLRect clip_rect = image_datap->mImageClipRegion;
    if (final)
    {
        delete image_datap;
    }

    LLUIImageList* instance = getInstance();

    uuid_ui_image_map_t::iterator found_it = instance->mUIImages.find(ui_image_name);
    if (found_it != instance->mUIImages.end())
    {
        LLUIImagePtr imagep = found_it->second;

        // for images grabbed from local files, apply clipping rectangle to restore original dimensions
        // from power-of-2 gl image
        if (success && imagep.notNull() && src_vi && (src_vi->getUrl().compare(0, 7, "file://")==0))
        {
            F32 full_width = (F32)src_vi->getFullWidth();
            F32 full_height = (F32)src_vi->getFullHeight();
            F32 clip_x = (F32)src_vi->getOriginalWidth() / full_width;
            F32 clip_y = (F32)src_vi->getOriginalHeight() / full_height;
            if (clip_rect != LLRect::null)
            {
                imagep->setClipRegion(LLRectf(llclamp((F32)clip_rect.mLeft / full_width, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mTop / full_height, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mRight / full_width, 0.f, 1.f),
                                            llclamp((F32)clip_rect.mBottom / full_height, 0.f, 1.f)));
            }
            else
            {
                imagep->setClipRegion(LLRectf(0.f, clip_y, clip_x, 0.f));
            }
            if (scale_rect != LLRect::null)
            {
                imagep->setScaleRegion(
                    LLRectf(llclamp((F32)scale_rect.mLeft / (F32)imagep->getWidth(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mTop / (F32)imagep->getHeight(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mRight / (F32)imagep->getWidth(), 0.f, 1.f),
                        llclamp((F32)scale_rect.mBottom / (F32)imagep->getHeight(), 0.f, 1.f)));
            }

            imagep->onImageLoaded();
        }
    }
}

namespace LLInitParam
{
    template<>
    struct TypeValues<LLUIImage::EScaleStyle> : public TypeValuesHelper<LLUIImage::EScaleStyle>
    {
        static void declareValues()
        {
            declare("scale_inner",  LLUIImage::SCALE_INNER);
            declare("scale_outer",  LLUIImage::SCALE_OUTER);
        }
    };
}

struct UIImageDeclaration : public LLInitParam::Block<UIImageDeclaration>
{
    Mandatory<std::string>      name;
    Optional<std::string>       file_name;
    Optional<bool>              preload;
    Optional<LLRect>            scale;
    Optional<LLRect>            clip;
    Optional<bool>              use_mips;
    Optional<LLUIImage::EScaleStyle> scale_type;

    UIImageDeclaration()
    :   name("name"),
        file_name("file_name"),
        preload("preload", false),
        scale("scale"),
        clip("clip"),
        use_mips("use_mips", false),
        scale_type("scale_type", LLUIImage::SCALE_INNER)
    {}
};

struct UIImageDeclarations : public LLInitParam::Block<UIImageDeclarations>
{
    Mandatory<S32>  version;
    Multiple<UIImageDeclaration> textures;

    UIImageDeclarations()
    :   version("version"),
        textures("texture")
    {}
};

bool LLUIImageList::initFromFile()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_TEXTURE;
    // Look for textures.xml in all the right places. Pass
    // constraint=LLDir::ALL_SKINS because we want to overlay textures.xml
    // from all the skins directories.
    std::vector<std::string> textures_paths =
        gDirUtilp->findSkinnedFilenames(LLDir::TEXTURES, "textures.xml", LLDir::ALL_SKINS);
    std::vector<std::string>::const_iterator pi(textures_paths.begin()), pend(textures_paths.end());
    if (pi == pend)
    {
        LL_WARNS() << "No textures.xml found in skins directories" << LL_ENDL;
        return false;
    }

    // The first (most generic) file gets special validations
    LLXMLNodePtr root;
    if (!LLXMLNode::parseFile(*pi, root, NULL))
    {
        LL_WARNS() << "Unable to parse UI image list file " << *pi << LL_ENDL;
        return false;
    }
    if (!root->hasAttribute("version"))
    {
        LL_WARNS() << "No valid version number in UI image list file " << *pi << LL_ENDL;
        return false;
    }

    UIImageDeclarations images;
    LLXUIParser parser;
    parser.readXUI(root, images, *pi);

    // add components defined in the rest of the skin paths
    while (++pi != pend)
    {
        LLXMLNodePtr update_root;
        if (LLXMLNode::parseFile(*pi, update_root, NULL))
        {
            parser.readXUI(update_root, images, *pi);
        }
    }

    if (!images.validateBlock()) return false;

    std::map<std::string, UIImageDeclaration> merged_declarations;
    for (LLInitParam::ParamIterator<UIImageDeclaration>::const_iterator image_it = images.textures.begin();
        image_it != images.textures.end();
        ++image_it)
    {
        merged_declarations[image_it->name].overwriteFrom(*image_it);
    }

    enum e_decode_pass
    {
        PASS_DECODE_NOW,
        PASS_DECODE_LATER,
        NUM_PASSES
    };

    for (S32 cur_pass = PASS_DECODE_NOW; cur_pass < NUM_PASSES; cur_pass++)
    {
        for (std::map<std::string, UIImageDeclaration>::const_iterator image_it = merged_declarations.begin();
            image_it != merged_declarations.end();
            ++image_it)
        {
            const UIImageDeclaration& image = image_it->second;
            std::string file_name = image.file_name.isProvided() ? image.file_name() : image.name();

            // load high priority textures on first pass (to kick off decode)
            enum e_decode_pass decode_pass = image.preload ? PASS_DECODE_NOW : PASS_DECODE_LATER;
            if (decode_pass != cur_pass)
            {
                continue;
            }
            preloadUIImage(image.name, file_name, image.use_mips, image.scale, image.clip, image.scale_type);
        }

        if (!gSavedSettings.getBOOL("NoPreload"))
        {
            if (cur_pass == PASS_DECODE_NOW)
            {
                // init fetching and decoding of preloaded images
                gTextureList.decodeAllImages(9.f);
            }
            else
            {
                // decodeAllImages needs two passes to refresh stats and priorities on second pass
                gTextureList.decodeAllImages(1.f);
            }
        }
    }
    return true;
}


