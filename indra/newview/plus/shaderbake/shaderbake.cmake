# -*- cmake -*-
# --bake-shaders: the shaders of every graphics preset and its neighbours
# compiled into an empty cache folder for the shader bundle, without logging
# in (research/159 section 3.1). Included by plus/plus.cmake. Started from the
# <plus> hook in LLAppViewer::initConfiguration(); the grid is
# app_settings/plus/shaderbake/grid.xml.

list(APPEND plus_SOURCE_FILES
    plus/shaderbake/shaderbake.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/shaderbake/shaderbake.h
    )
