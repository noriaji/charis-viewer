# -*- cmake -*-
# The start-up check of the Windows DLL the release no longer ships
# (d3dcompiler_47.dll: Web pages and media find it in System32; research/268).
# Included by plus/plus.cmake. No hook: a plus::FeatureRegistration.

list(APPEND plus_SOURCE_FILES
    plus/licenseslim/licenseslim.cpp
    )
