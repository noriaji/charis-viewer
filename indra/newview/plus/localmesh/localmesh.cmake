# -*- cmake -*-
# Local mesh (plus/localmesh, from Firestorm's Local Mesh by Vaalith Jinn): a .dae / .gltf /
# .glb file on this computer shown on a mesh object of the world, for this viewer only,
# without uploading it (research/140). Included by plus/plus.cmake. The window is registered
# from the feature's init and its menu item is in menu_plus.xml; the official hooks
# (plus::localmesh::holdsUpdates(), simulatorUpdateType()) are listed by plus/plus-hooks.sh.

list(APPEND plus_SOURCE_FILES
    plus/localmesh/localmesh.cpp
    plus/localmesh/localmeshdae.cpp
    plus/localmesh/localmeshfloater.cpp
    plus/localmesh/localmeshgltf.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/localmesh/localmesh.h
    plus/localmesh/localmeshread.h
    )

# The DAE reader reads the text through libxml2 (the COLLADA DOM's own XML library, already
# linked through ll::colladadom) before the DOM gets it; its headers are in a folder of their
# own (the static library's define is in the source, so the precompiled header is not at odds
# with it). Appended: plus.cmake appends newview and plus/ after it.
set_property(SOURCE plus/localmesh/localmeshdae.cpp
    APPEND PROPERTY INCLUDE_DIRECTORIES "${LIBS_PREBUILT_DIR}/include/libxml2")
