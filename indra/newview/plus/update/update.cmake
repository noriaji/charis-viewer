# -*- cmake -*-
# Update: installing a new Charis Viewer the user agreed to (research/326 in the
# sl-viewer repository): the state, the chip at the bottom right, and the backend
# that does the work (llvelopack.cpp registers one for a Setup.exe installation).
# Whether there is a newer version is plus/updatecheck's. Included by plus/plus.cmake.
# updatestate_test.cpp is a stand-alone test, not built here.

list(APPEND plus_SOURCE_FILES
    plus/update/update.cpp
    plus/update/updatestate.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/update/update.h
    plus/update/updatestate.h
    )
