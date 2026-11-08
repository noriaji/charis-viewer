# -*- cmake -*-
# The PC short of free memory: texture decodes wait and the textures go down
# (igpu-safe, 2026-11-03). Included by plus/plus.cmake. Hooks: one block in
# LLTextureFetchWorker::doWork() (DECODE_IMAGE), one line in
# LLViewerTexture::updateClass().

list(APPEND plus_SOURCE_FILES
    plus/memguard/memguard.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/memguard/memguard.h
    )
