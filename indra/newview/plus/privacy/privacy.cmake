# -*- cmake -*-
# Keeps secrets (translation API keys) and Charis' own settings out of the
# ViewerStats report sent to Linden Lab at logout and quit (research/123 H2).
# Included by plus/plus.cmake. Hook: one call in send_viewer_stats() (llviewerstats.cpp).

list(APPEND plus_SOURCE_FILES
    plus/privacy/privacy.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/privacy/privacy.h
    )
