# -*- cmake -*-
# Charis Viewer's own build number: CHARIS_BUILD_NUMBER, which
# indra/cmake/BuildVersion.cmake (a <plus> hook) makes the viewer's
# VIEWER_VERSION_REVISION, so every place that shows, sends or stores the build
# (LL_VIEWER_VERSION_BUILD, LLVersionInfo::getBuild(): "0.1.0.N" at login and in
# user agents, the About box, the window title, crash data, packages-info.txt,
# the exe's version resource, viewer_version.txt) uses it.
#
# The number is the count of the commits since Charis branched from the
# official viewer (CHARIS_BUILD_BASE below): every commit in the history of the
# source being built that the base does not have, Charis's own and the
# official ones a later official release merge brings in. So:
#   - the same commit always gives the same number (on any branch or clone);
#   - a later commit gives a larger one (it has every commit of the earlier one
#     and more), also through merges and fast-forwards;
#   - it does not depend on the official build numbers (the environment
#     variables "revision" and AUTOBUILD_BUILD_ID, autobuild's UTC date and
#     time "yyDDDHHMM", are not read any more), nor on the commit count of the
#     whole official history.
#
# Where the number comes from, first found wins:
#   1. the environment variable CHARIS_BUILD_NUMBER (1 to 9 digits, no leading
#      zero), to set it by hand;
#   2. git: the count above, when git is found and the base is in the history
#      of the checkout (not in a shallow clone that cut it off);
#   3. build-number.txt next to this file, which tools/make-source-zip.sh puts
#      in the source zip (it has no .git);
#   4. CHARIS_BUILD_NUMBER_FLOOR below, with a warning.
#
# The number is worked out at configure time. With git, a new commit (or a
# checkout of another one) re-runs configure on the next build, so the
# number follows it without "configure" being asked for.
#
# Included by indra/cmake/BuildVersion.cmake (in indra/, before newview): once.

include_guard(GLOBAL)

# The official commit Charis branched from: the parent of the first Charis
# commit (9156c518fc, 2026-10-08), on the official develop branch of
# 2026-10-07. Never change it: the number counts from here.
set(CHARIS_BUILD_BASE 7dd6de6120ce7a80b0a290d34a701e8717d03962)

# The number for a copy with neither git history nor build-number.txt: the
# count of vk-main when this was last set (vk-main 56af3e89a1, 2026-10-30).
# Raise it before each release (to the release's own number), so that such a
# build never shows a number below one already handed out.
set(CHARIS_BUILD_NUMBER_FLOOR 1967)

# A build number as the C++ code takes it (LL_VIEWER_VERSION_BUILD is pasted in
# as a literal): digits only, no leading zero (that would be octal), at most 9
# digits.
function(charis_valid_build_number text result)
    string(LENGTH "${text}" length)
    if ("${text}" MATCHES "^[1-9][0-9]*$" AND length LESS_EQUAL 9)
        set(${result} TRUE PARENT_SCOPE)
    else ()
        set(${result} FALSE PARENT_SCOPE)
    endif ()
endfunction()

set(CHARIS_BUILD_NUMBER "")

# 1. by hand
if (DEFINED ENV{CHARIS_BUILD_NUMBER})
    charis_valid_build_number("$ENV{CHARIS_BUILD_NUMBER}" charis_build_ok)
    if (charis_build_ok)
        set(CHARIS_BUILD_NUMBER "$ENV{CHARIS_BUILD_NUMBER}")
        message(STATUS "Charis build number ${CHARIS_BUILD_NUMBER} (from the environment, CHARIS_BUILD_NUMBER)")
    else ()
        message(WARNING "CHARIS_BUILD_NUMBER='$ENV{CHARIS_BUILD_NUMBER}' is not a build number (1 to 9 digits, no leading zero); ignored")
    endif ()
endif ()

# 2. git
if ("${CHARIS_BUILD_NUMBER}" STREQUAL "")
    find_package(Git QUIET)
    if (GIT_FOUND)
        # The base must be an ancestor of HEAD: not so when it is missing (a
        # shallow clone) or when the source sits inside another repository.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" merge-base --is-ancestor ${CHARIS_BUILD_BASE} HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
            RESULT_VARIABLE charis_git_rc
            OUTPUT_QUIET ERROR_QUIET)
        if (charis_git_rc EQUAL 0)
            # base..HEAD, not "HEAD ^base": a git.cmd runs through cmd.exe, which eats the ^ and counts the
            # whole history (research/275 L017)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-list --count ${CHARIS_BUILD_BASE}..HEAD
                WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                RESULT_VARIABLE charis_git_rc
                OUTPUT_VARIABLE charis_git_count
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            charis_valid_build_number("${charis_git_count}" charis_build_ok)
            if (charis_git_rc EQUAL 0 AND charis_build_ok)
                set(CHARIS_BUILD_NUMBER ${charis_git_count})
                message(STATUS "Charis build number ${CHARIS_BUILD_NUMBER} (from git: commits since the branch from the official viewer)")

                # Re-run configure when HEAD moves: HEAD itself (a checkout), the
                # branch it is on, and the HEAD reflog (every commit, merge,
                # reset or checkout in this working tree). Files that are not
                # there (a packed branch, no reflog) are left out.
                set(charis_git_watch HEAD logs/HEAD)
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                    OUTPUT_VARIABLE charis_git_branch
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                if ("${charis_git_branch}" MATCHES "^refs/heads/")
                    list(APPEND charis_git_watch "${charis_git_branch}")
                endif ()
                foreach (charis_git_name ${charis_git_watch})
                    execute_process(
                        COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path ${charis_git_name}
                        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                        OUTPUT_VARIABLE charis_git_file
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
                    if (IS_ABSOLUTE "${charis_git_file}" AND EXISTS "${charis_git_file}")
                        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${charis_git_file}")
                    endif ()
                endforeach ()
            endif ()
        endif ()
    endif ()
endif ()

# 3. the source zip
set(charis_build_file "${CMAKE_CURRENT_LIST_DIR}/build-number.txt")
if ("${CHARIS_BUILD_NUMBER}" STREQUAL "" AND EXISTS "${charis_build_file}")
    file(STRINGS "${charis_build_file}" charis_build_text LIMIT_COUNT 1 LIMIT_INPUT 64)
    string(STRIP "${charis_build_text}" charis_build_text)
    charis_valid_build_number("${charis_build_text}" charis_build_ok)
    if (charis_build_ok)
        set(CHARIS_BUILD_NUMBER ${charis_build_text})
        message(STATUS "Charis build number ${CHARIS_BUILD_NUMBER} (from plus/version/build-number.txt)")
    else ()
        message(WARNING "plus/version/build-number.txt does not hold a build number; ignored")
    endif ()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${charis_build_file}")
endif ()

# 4. the floor
if ("${CHARIS_BUILD_NUMBER}" STREQUAL "")
    set(CHARIS_BUILD_NUMBER ${CHARIS_BUILD_NUMBER_FLOOR})
    message(WARNING "Charis build number: no git history from the official branch point and no "
                    "plus/version/build-number.txt; using ${CHARIS_BUILD_NUMBER} (CHARIS_BUILD_NUMBER_FLOOR). "
                    "Set the environment variable CHARIS_BUILD_NUMBER to choose it.")
endif ()

unset(charis_build_ok)
unset(charis_build_file)
unset(charis_build_text)
unset(charis_git_rc)
unset(charis_git_count)
unset(charis_git_branch)
unset(charis_git_watch)
unset(charis_git_name)
unset(charis_git_file)
