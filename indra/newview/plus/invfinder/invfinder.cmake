# -*- cmake -*-
# Inventory finder (plus/invfinder): search over the whole inventory for an AI
# assistant (MCP) and, later, a search field. Included by plus/plus.cmake.
# invindex.* and invargs.* have no viewer dependencies (tested standalone, see
# tools/invfinder in the sl-viewer repository). The tools are served by the one
# MCP server of plus/posermcp (posermcp.cpp).

list(APPEND plus_SOURCE_FILES
    plus/invfinder/invargs.cpp
    plus/invfinder/invindex.cpp
    plus/invfinder/invsource.cpp
    plus/invfinder/invtools.cpp
    plus/invfinder/invview.cpp
    )

list(APPEND plus_HEADER_FILES
    plus/invfinder/invargs.h
    plus/invfinder/invindex.h
    plus/invfinder/invsource.h
    plus/invfinder/invtools.h
    plus/invfinder/invview.h
    )
