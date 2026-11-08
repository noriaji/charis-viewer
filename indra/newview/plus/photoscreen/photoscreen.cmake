# -*- cmake -*-
# Photo screen: a whole-window mode for taking pictures (research/218 in the sl-viewer repository).
# This branch is the frame: entering and leaving with the UI hidden and restored, the world's
# rectangle (one <plus> hook in LLViewerWindow::updateWorldViewRect()), the detailed / simple
# switch and the entrances. Included by plus/plus.cmake. Settings: PlusPhotoScreenSimple.
# psui/: the widgets that draw the target picture's parts and their style table, the masks and
# icons (made by psui/make_ps_images.py) and the parts sample window (research/221, ps-style).
# psui/psstack: the column of widgets stacked from the top, the rows that follow values and the sub-tabs
# (the left panel's; the photo tools' from research/265).
# psui/pstabs: plus_ps_tabs, the official tab container with its tabs drawn in the psui style (the
# preferences' tabs and the Charis tab's sub-tabs, research/266 prefs-frame).
# photoconv*: the conversations tab (IM and group chats, research/247 first stage, ps-im).
# photocamctl: the camera tab's camera controls (orbit, zoom, pan, tilt; research/260, photo-camera).
# psui/tokens.txt is the UI's one table of values: psui/make_ui_tokens.py makes pstokens.{h,cpp}
# and the Charis skin (skins/charis) from it (research/239 (12), ui-skin).
# photoscreen_items.h: the one table of which photo tools' item the left and right panels and the simple view
# place by hand, and where (research/272 (A), refactor-ps-placement).
# psui/pscompat: the official widgets (LLButton, LLCheckBoxCtrl, LLSliderCtrl, LLSpinCtrl, LLTextBox,
# LLRadioGroup) drawn in this look, for the windows whose C++ looks widgets up by type, and the looks the
# psui widgets share with them (research/266 2.1, ui-compat).

list(APPEND plus_SOURCE_FILES
    plus/photoscreen/photodetail.cpp
    plus/photoscreen/photodetail_bottom.cpp
    plus/photoscreen/photodetail_center.cpp
    plus/photoscreen/photodetail_hud.cpp
    plus/photoscreen/photodetail_left.cpp
    plus/photoscreen/photodetail_right.cpp
    plus/photoscreen/photodetail_rows.cpp
    plus/photoscreen/composition.cpp
    plus/photoscreen/panel_simple.cpp
    plus/photoscreen/photoscreen.cpp
    plus/photoscreen/psui/psstyle.cpp
    plus/photoscreen/psui/pstokens.cpp
    plus/photoscreen/psui/pswidgets.cpp
    plus/photoscreen/psui/pscards.cpp
    plus/photoscreen/psui/pstabs.cpp
    plus/photoscreen/psui/psgallery.cpp
    plus/photoscreen/psui/psstack.cpp
    plus/photoscreen/psui/pscompat.cpp
    plus/photoscreen/photodetail_skywater.cpp
    plus/photoscreen/photoconv.cpp
    plus/photoscreen/photoconv_panel.cpp
    plus/photoscreen/photocamctl.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/photoscreen/photodetail.h
    plus/photoscreen/photodetail_bottom.h
    plus/photoscreen/photodetail_center.h
    plus/photoscreen/photodetail_hud.h
    plus/photoscreen/photodetail_layout.h
    plus/photoscreen/photodetail_left.h
    plus/photoscreen/photodetail_right.h
    plus/photoscreen/photodetail_rows.h
    plus/photoscreen/photodetail_state.h
    plus/photoscreen/composition.h
    plus/photoscreen/panel_simple.h
    plus/photoscreen/photoscreen.h
    plus/photoscreen/photoscreen_items.h
    plus/photoscreen/psui/psstyle.h
    plus/photoscreen/psui/pstokens.h
    plus/photoscreen/psui/pswidgets.h
    plus/photoscreen/psui/pstabs.h
    plus/photoscreen/psui/psstack.h
    plus/photoscreen/psui/pscompat.h
    plus/photoscreen/photodetail_skywater.h
    plus/photoscreen/photoconv.h
    plus/photoscreen/photoconv_panel.h
    plus/photoscreen/photocamctl.h
    )
