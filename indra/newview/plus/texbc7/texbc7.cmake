# -*- cmake -*-
# The setting PlusTexBC7GPU: BC7 texture compression on the GPU, on or off
# (research/370 in the sl-viewer repository). Included by plus/plus.cmake.
# No <plus> hook in an official file: it registers itself (plus::FeatureRegistration).

list(APPEND plus_SOURCE_FILES
    plus/texbc7/texbc7.cpp
    )
