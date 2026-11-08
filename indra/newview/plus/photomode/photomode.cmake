# -*- cmake -*-
# Photo mode: a card (a standard floater, so the detached-window feature takes
# it like any other) with one-press look presets, a few sliders, the sky and
# water picker, remembered camera positions and the shutter button
# (research/photo-mode-ui.md in the sl-viewer repository). Included by
# plus/plus.cmake. No official file is changed: the floater is registered from
# the feature's init and its menu items are in menu_plus.xml.

list(APPEND plus_SOURCE_FILES
    plus/photomode/photocamera.cpp
    plus/photomode/photoguide.cpp
    plus/photomode/photokeys.cpp
    plus/photomode/photolook.cpp
    plus/photomode/photomode.cpp
    plus/photomode/photorecipe.cpp
    plus/photomode/photoshot.cpp
    plus/photomode/photosky.cpp
    plus/photomode/phototools.cpp
    plus/photomode/phototools_blocks.cpp
    plus/photomode/phototools_env.cpp
    plus/photomode/phototoolsfloater.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/photomode/photocamera.h
    plus/photomode/photoguide.h
    plus/photomode/photokeys.h
    plus/photomode/photolook.h
    plus/photomode/photomode.h
    plus/photomode/photorecipe.h
    plus/photomode/photoshot.h
    plus/photomode/photosky.h
    plus/photomode/phototools.h
    plus/photomode/phototoolsfloater.h
    plus/photomode/phototoolsfloater_impl.h
    )
