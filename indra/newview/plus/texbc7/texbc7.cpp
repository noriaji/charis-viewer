/**
 * @file texbc7.cpp
 * @brief The setting PlusTexBC7GPU: BC7 texture compression on the GPU, on or
 *        off, also while the viewer runs (research/370).
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

// Some GPUs and drivers turn the colours of a texture into coloured blocks
// when the encoder (llvulkan/llvktexcompress) makes it BC7 (a quarter of the
// memory). The user can switch the encoder off without an environment
// variable: this setting is LLRHI::sTexBCN's second source (the first is
// LLRHI_TEX_BCN, for development, which wins when it is set).
//
// What a change does (LLRHI::sTexBCN is read at every step; research/370):
//  - on -> off: no new texture is made BC7 (rhi_compressible, bcn_shape,
//    bcn_candidate); the textures still queued for the encoder leave the queue
//    plain (compress_queued_textures). An encoding the frame recorded already
//    (compress_texture) is swapped in at once, so a texture recorded in the
//    frame of the change is BC7. One submitted to the compute queue is taken
//    in only if the switch is on when it finishes (finish_async_bcn asks
//    bcn_candidate): otherwise it is discarded and the texture stays plain
//    (no VRAM is left behind). The textures already BC7 stay BC7 (plain again
//    only when loaded again).
//  - off -> on: the textures created from then on are candidates; those made
//    while it was off were not marked (bcn_wanted) and stay plain until they
//    are loaded again. The encoder is built on its own threads at the first
//    frame that wants it (LLVKTexCompress::prepare).
// A GPU/driver on the encoder's known-bad list stays off whatever this says.
// A shader bake (--bake-shaders, plus/shaderbake) does not take the setting:
// it bakes with the defaults, so the BC7 encoder's shaders are in the bundle
// whatever the setting of the PC that bakes (llvkgl_linkbuild.cpp reads sTexBCN).

#include "llviewerprecompiledheaders.h"

#include "llcontrol.h"
#include "lluictrl.h"
#include "llrhi.h"
#include "llviewercontrol.h"

#include "plus.h"
#include "plus/shaderbake/shaderbake.h"

namespace
{
    constexpr char SETTING[] = "PlusTexBC7GPU";
    constexpr char ENVIRONMENT[] = "LLRHI_TEX_BCN";

    // LLRHI_TEX_BCN decides (as it did before the setting): a set, non-empty value.
    // Read once, at init (the signal is connected or not by it); the UI asks the same answer.
    bool sFromEnvironment = false;

    bool fromEnvironment()
    {
        return sFromEnvironment;
    }

    bool setting()
    {
        LLControlVariable* control = gSavedSettings.getControl(SETTING);
        return !control || control->getValue().asBoolean();
    }

    // Main thread (the setting's signal, init). A plain bool, as the cue toggles of
    // llrhi.cpp flip it (llrhi.h says so): a render thread reading it meanwhile sees
    // the old or the new value, and every reader copes with either (see above).
    void apply()
    {
        const bool on = setting();
        if (LLRHI::sTexBCN == on)
        {
            return;
        }
        LLRHI::sTexBCN = on;
        LL_INFOS("Plus") << SETTING << ": BC7 texture compression " << (on ? "on" : "off") << LL_ENDL;
    }

    void init()
    {
        // Preferences > Charis > Graphics: skins/default/xui/<lang>/panel_preferences_charis.xml (c_bc7_gpu;
        // plus/prefscharis reads Is and Choosable).
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.TexBC7.Is",
            [](LLUICtrl*, const LLSD&) { return fromEnvironment() ? LLRHI::sTexBCN : setting(); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.TexBC7.Toggle",
            [](LLUICtrl*, const LLSD&) { gSavedSettings.setBOOL(SETTING, !setting()); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.TexBC7.Choosable",
            [](LLUICtrl*, const LLSD&) { return !fromEnvironment(); });

        sFromEnvironment = !LLRHI::envUnset(ENVIRONMENT);
        if (sFromEnvironment)
        {
            LL_INFOS("Plus") << SETTING << " not used: " << ENVIRONMENT << " is set" << LL_ENDL;
            return;
        }
        // A bake takes the defaults whatever this PC's settings.xml says (the bake loads it too).
        // The UI is greyed out there, as with the environment variable.
        if (plus::shaderbake::active())
        {
            sFromEnvironment = true;
            LL_INFOS("Plus") << SETTING << " not used: the shader bake bakes with the BC7 encoder's shaders in" << LL_ENDL;
            return;
        }
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING).get())
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { apply(); });
        }
        apply();
    }

    plus::FeatureRegistration sRegistration({ "texbc7", &init });
}
