/**
 * @file independence.cpp
 * @brief Keeps Charis Viewer apart from the official Second Life viewer:
 *        own user folders, own cache, own channel (research/114).
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

// The settings of the official viewer (and of Firestorm and other viewers) are
// not copied by themselves any more: the first start offers to bring them over
// at the login screen, and Preferences > Setup at any time (plus/importsettings,
// research/143). A first start without that begins with the defaults.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "independence.h"

#include "lldir.h"
#include "llversioninfo.h"

namespace
{
    void init()
    {
        LL_INFOS("Independence") << "User folder " << gDirUtilp->getOSUserAppDir()
                                 << ", channel '" << LLVersionInfo::instance().getChannel() << "'" << LL_ENDL;
    }

    // init() runs before LLAppViewer::initCache() applies CacheLocation, so the
    // cache folder is logged here, once the UI exists.
    void onUIReady()
    {
        LL_INFOS("Independence") << "Cache " << gDirUtilp->getCacheDir() << LL_ENDL;
    }

    plus::FeatureRegistration sRegistration({ "independence", &init, &onUIReady });
}

void plus::independence::initAppDirs()
{
    gDirUtilp->initAppDirs(APP_DIR_NAME);

    // Before the exec marker records our version (processMarkerFiles()):
    // the command line (--channel) can still change it later.
    LLVersionInfo::instance().resetChannel(CHANNEL);
}
