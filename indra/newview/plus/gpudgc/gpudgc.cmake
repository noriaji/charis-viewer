# -*- cmake -*-
# DGC (VK_EXT_device_generated_commands) by the setting PlusGpuDrivenDGC:
# auto (NVIDIA's GPUs only), on or off, read before the Vulkan device is made
# (dgc-nvidia-only; research/124 V5 in the sl-viewer repository). Included by
# plus/plus.cmake. No <plus> hook: it registers itself (plus::FeatureRegistration).
# gpudgc.h: plus/vkcheck's exit turns DGC off after a lost device (fix-round9b 4).

list(APPEND plus_SOURCE_FILES
    plus/gpudgc/gpudgc.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/gpudgc/gpudgc.h
    )
