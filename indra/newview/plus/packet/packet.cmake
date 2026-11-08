# -*- cmake -*-
# Draw packets (research/180 stage 4 P1 in the sl-viewer repository): the
# legacy material loop's static batches handed to the backend as packets
# (LLRHI::sPacketDraw, llrhi.h) instead of one direct draw and its uniform
# calls per batch. Off by default (LLRHI_PACKETS). Included by plus/plus.cmake.
# Called from the "LLRHI native (research/180 stage 4)" line in
# lldrawpoolmaterials.cpp.

list(APPEND plus_SOURCE_FILES
    plus/packet/packetmaterials.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/packet/packet.h
    )
