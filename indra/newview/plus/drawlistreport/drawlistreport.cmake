# -*- cmake -*-
# The draw lists' viewer-side counts on the VKPerf line (research/137 N7 in
# the sl-viewer repository): plus/drawlist's and plus/alphalist's takeStats,
# handed to LLRHI::DrawList::setOwnerReport. Included by plus/plus.cmake. No
# <plus> hook: it registers itself (plus::FeatureRegistration).

list(APPEND plus_SOURCE_FILES
    plus/drawlistreport/drawlistreport.cpp
    )
