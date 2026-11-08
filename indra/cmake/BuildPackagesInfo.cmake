# -*- cmake -*-
# Construct the version and copyright information based on package data.
include(Python)
include(FindAutobuild)
# <plus> Charis numbers for the first line of packages-info.txt (About > Licenses):
# "Charis Viewer 0.1.0.N", not the official 26.5.0.N (research/123 M1)
include(${CMAKE_SOURCE_DIR}/newview/plus/version/numbers.cmake)
# </plus>

# packages-formatter.py runs autobuild install --versions, which needs to know
# the build_directory, which (on Windows) depends on AUTOBUILD_ADDRSIZE.
# Within an autobuild build, AUTOBUILD_ADDRSIZE is already set. But when
# building in an IDE, it probably isn't. Set it explicitly using
# run_build_test.py.
add_custom_command(OUTPUT packages-info.txt
  COMMENT "Generating packages-info.txt for the about box"
  MAIN_DEPENDENCY ${CMAKE_SOURCE_DIR}/../autobuild.xml
  DEPENDS ${CMAKE_SOURCE_DIR}/../scripts/packages-formatter.py
          ${CMAKE_SOURCE_DIR}/newview/luau.txt
          ${CMAKE_SOURCE_DIR}/../autobuild.xml
  COMMAND ${PYTHON_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/cmake/run_build_test.py -DAUTOBUILD_ADDRSIZE=${ADDRESS_SIZE} -DAUTOBUILD=${AUTOBUILD_EXECUTABLE} -DPYTHONIOENCODING=utf-8
          ${PYTHON_EXECUTABLE}
  # <plus> CHARIS_VERSION_* in place of VIEWER_SHORT_VERSION
          ${CMAKE_SOURCE_DIR}/../scripts/packages-formatter.py "${VIEWER_CHANNEL}" "${CHARIS_VERSION_MAJOR}.${CHARIS_VERSION_MINOR}.${CHARIS_VERSION_PATCH}.${VIEWER_VERSION_REVISION}" "${AUTOBUILD_INSTALL_DIR}" > packages-info.txt
  # </plus>
  )
