# -*- cmake -*-
# RLVa (RestrainedLove API) support, research/248. Included by plus/plus.cmake.
# The code follows Firestorm's rlv*.cpp / rlv*.h (see ORIGIN.md). Hooks in official files:
# llviewermessage.cpp (process_chat_from_simulator), llstartup.cpp, llappviewer.cpp (cleanup), llui/llchat.h,
# llvoavatarself.cpp (attachObject, detachObject, detachAttachmentIntoInventory), and the chat / IM / teleport / permission
# / offer files of rlv-comm (llviewermessage, llimprocessing, llimview, llagent, llfloaterimnearbychat, ...).
# Each rlv-* branch adds its own files below (re-run CMake configure after that).

list(APPEND plus_SOURCE_FILES
    plus/rlv/rlvactions.cpp
    plus/rlv/rlvattachpt.cpp
    plus/rlv/rlvcommon.cpp
    plus/rlv/rlvexperience.cpp
    plus/rlv/rlvforcewear.cpp
    plus/rlv/rlvhandler.cpp
    plus/rlv/rlvhelper.cpp
    plus/rlv/rlvhooks_comm.cpp
    plus/rlv/rlvhooks_core.cpp
    plus/rlv/rlvhooks_locks.cpp
    plus/rlv/rlvhooks_ui.cpp
    plus/rlv/rlvhooks_wear.cpp
    plus/rlv/rlvhooks_show_gate.cpp
    plus/rlv/rlvhooks_show_list.cpp
    plus/rlv/rlvhooks_show_floater.cpp
    plus/rlv/rlvhooks_show_panel1.cpp
    plus/rlv/rlvhooks_show_panel2.cpp
    plus/rlv/rlvhooks_show_misc.cpp
    plus/rlv/rlvhooks_show_plusfx.cpp
    plus/rlv/rlvui_show.cpp
    plus/rlv/rlvinventory.cpp
    plus/rlv/rlvlocks.cpp
    plus/rlv/rlvnames.cpp
    plus/rlv/rlvplus.cpp
    plus/rlv/rlvui.cpp
    plus/rlv/rlvwearquery.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/rlv/rlvactions.h
    plus/rlv/rlvattachpt.h
    plus/rlv/rlvcommon.h
    plus/rlv/rlvdefines.h
    plus/rlv/rlvexperience.h
    plus/rlv/rlvforcewear.h
    plus/rlv/rlvhandler.h
    plus/rlv/rlvhelper.h
    plus/rlv/rlvhooks.h
    plus/rlv/rlvhooks_show_gate.h
    plus/rlv/rlvhooks_show_list.h
    plus/rlv/rlvhooks_show_floater.h
    plus/rlv/rlvhooks_show_panel1.h
    plus/rlv/rlvhooks_show_panel2.h
    plus/rlv/rlvhooks_show_misc.h
    plus/rlv/rlvhooks_show_plusfx.h
    plus/rlv/rlvinventory.h
    plus/rlv/rlvlocks.h
    plus/rlv/rlvmodifiers.h
    plus/rlv/rlvui.h
    )
