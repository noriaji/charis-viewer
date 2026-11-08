/**
 * Menus of RLVa (Charis, research/248 7.2): the callbacks Firestorm registers in llviewermenu.cpp and the
 * "RLVa" menu. The rest of the settings UI is in skins/default/xui/<lang>/panel_preferences_charis.xml.
 */

#include "llviewerprecompiledheaders.h"

#include "plus/plus.h"

#include "lluictrl.h"

#include "rlvcommon.h"

namespace
{
    constexpr char RLVA_MENU_FILE[]   = "menu_plus_rlva.xml";  // skins/<skin>/xui/<lang>/
    constexpr char RLVA_MENU_TARGET[] = "RLVa Main";           // the empty menu of menu_viewer.xml

    // The same names as Firestorm's menu XML uses, so menu items taken over from it work unchanged.
    void init()
    {
        LLUICtrl::EnableCallbackRegistry::Registrar& enable = LLUICtrl::EnableCallbackRegistry::defaultRegistrar();
        enable.add("RLV.MainToggleVisible", [](LLUICtrl* ctrl, const LLSD&) { return rlvMenuMainToggleVisible(ctrl); });
        enable.add("RLV.CanShowName",       [](LLUICtrl*, const LLSD&) { return rlvMenuCanShowName(); });
        enable.add("RLV.EnableIfNot",       [](LLUICtrl*, const LLSD& param) { return rlvMenuEnableIfNot(param); });
    }

    // The menu bar exists: fill the RLVa menu (hidden until RLVa is on), then put it where RLVaTopLevelMenu says.
    void onUIReady()
    {
        plus::addMenuItemsFromFile(RLVA_MENU_FILE, RLVA_MENU_TARGET);
        rlvMenuToggleVisible();
    }

    plus::FeatureRegistration sRegistration({ "rlvui", &init, &onUIReady });
}
