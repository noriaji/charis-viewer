# -*- cmake -*-
# Subtitles (plus/subtitle, research/137): the voice you hear, written out by Whisper (CPU only, in the
# helper charis-subtitle.exe from tools/whisper-sub). Included by plus/plus.cmake. The window is
# registered from the feature's init and its menu item is in menu_plus.xml; the one official hook is
# llwebrtc's playout tap (indra/llwebrtc, marked <plus>).

list(APPEND plus_SOURCE_FILES
    plus/subtitle/helperprocess.cpp
    plus/subtitle/streamtap.cpp
    plus/subtitle/subtitleclient.cpp
    plus/subtitle/subtitledownload.cpp
    plus/subtitle/subtitlefloater.cpp
    plus/subtitle/translateclient.cpp
    plus/subtitle/translatefetch.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/subtitle/helperprocess.h
    plus/subtitle/streamtap.h
    plus/subtitle/subtitleclient.h
    plus/subtitle/subtitledownload.h
    plus/subtitle/subtitledialect.h
    plus/subtitle/subtitlehash.h
    plus/subtitle/subtitlering.h
    plus/subtitle/subtitlespeaker.h
    plus/subtitle/subtitletranslate.h
    plus/subtitle/translatearchive.h
    plus/subtitle/translateclient.h
    plus/subtitle/translatefetch.h
    )
