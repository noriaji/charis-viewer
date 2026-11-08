# -*- cmake -*-
# Translation (plus/translate, research/228): "this PC" as a service of the chat translation (local models through the helper
# charis-translate.exe from tools/translate-helper), the language detection and the text splitting it needs. Included by
# plus/plus.cmake. The official hooks are in lltranslate.cpp (marked <plus>). The helper, the model fetching and the dialects live
# in plus/subtitle (they were made for the subtitles first).

list(APPEND plus_SOURCE_FILES
    plus/translate/localtranslate.cpp
    plus/translate/translatesettings.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/translate/langdetect.h
    plus/translate/localtranslate.h
    plus/translate/textsplit.h
    plus/translate/translatesettings.h
    )
