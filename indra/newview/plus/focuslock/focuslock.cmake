# -*- cmake -*-
# Focus lock: the depth of field's focal point follows a face (research/218
# in the sl-viewer repository). Included by plus/plus.cmake. Applied through
# the <plus> hook in LLPipeline::renderDoF() (pipeline.cpp); the picker is in
# the photo tools' Avatar tab (plus/photomode/phototoolsfloater.cpp).

list(APPEND plus_SOURCE_FILES
    plus/focuslock/focuslock.cpp
    plus/focuslock/focushint.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/focuslock/focuslock.h
    plus/focuslock/focushint.h
    )
