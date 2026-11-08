# -*- cmake -*-
# The agent's own avatar first after login (research/122-self-avatar-first-idea.md
# in the sl-viewer repository): a log of what the cloud waits for, and the
# PlusSelfFirst switch. Included by plus/plus.cmake. Hooks: llviewertexture.cpp,
# lltexturefetch.cpp (three), llmeshrepository.cpp (two).

list(APPEND plus_SOURCE_FILES
    plus/selffirst/selffirst.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/selffirst/selffirst.h
    )
