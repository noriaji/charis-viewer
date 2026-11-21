# -*- cmake -*-
# Pose consent (plus/poseconsent, research/336): the consent ledger for posing another avatar on one's own screen,
# compatible with Aurora. The state machine (poseconsentcore) has no viewer types; poseconsent.cpp ties it to
# the bridge (plus/lslbridge), the confirmation, PlusPoseAskPolicy and the block list; poseconsentfloater.cpp is
# the reminder of who may pose us. Included by plus/plus.cmake.

list(APPEND plus_SOURCE_FILES
    plus/poseconsent/poseblocklist.cpp
    plus/poseconsent/poseconsent.cpp
    plus/poseconsent/poseconsentcommon.cpp
    plus/poseconsent/posereflect.cpp
    plus/poseconsent/poseconsentcore.cpp
    plus/poseconsent/poseconsentfloater.cpp
    plus/poseconsent/poseconsentrml.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/poseconsent/poseblocklist.h
    plus/poseconsent/poseconsent.h
    plus/poseconsent/poseconsentcommon.h
    plus/poseconsent/poseconsentcore.h
    plus/poseconsent/posereflect.h
    plus/poseconsent/poseconsentui.h
    )

# poseconsent_test.cpp is a stand-alone test (cl /EHsc /std:c++17 poseconsent_test.cpp poseconsentcore.cpp), not built here.
