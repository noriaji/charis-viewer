#!/usr/bin/env bash
# Builds Charis Viewer the way the released binaries are built (Windows 64-bit).
# Steps, tools and versions: BUILDING.md next to this file.
#
#   indra/newview/plus/building/charis-build.sh [options] [configure | build | check] [-- check options]
#
#   (none)      configure, build, then check
#   configure   configure only (then the configure part of the check)
#   build       build only (after a configure), then check
#   check       check only (release/check-release-build.py on the build folder)
#
#   --build-dir NAME   the build folder: build-<something>, directly in the checkout
#                      (default build-vc170-64; .gitignore leaves build-* out). Another
#                      name builds next to it with a copy of autobuild.xml in that folder.
#   --velopack on|off  USE_VELOPACK (default off). On: the package step also writes
#                      velopack-pack.json, the arguments for vpk pack (run later, after
#                      the shader bundle; the package step does not need vpk).
#   -- ...             the rest goes to release/check-release-build.py (e.g.
#                      --accept-number-file, --allow-untracked PATH)
#
# Run from Git Bash (or another bash for Windows) with autobuild on PATH and:
#   VULKAN_SDK                the Vulkan SDK folder (1.4.363.0, e.g. C:/VulkanSDK/1.4.363.0)
#   AUTOBUILD_VARIABLES_FILE  the "variables" file of secondlife/build-variables with
#                             optimize-relwithdebinfo.patch applied
# release/setup-deps.py fetches and checks all three and writes an env.sh that sets them.
#
# Output: <build folder>/newview/RelWithDebInfo/ (CharisViewer.exe, or SecondLifeViewer.exe
# before the rename, and the files
# listed in secondlife_setup_tmp.nsi, which is what the release zip holds).
# In the build folder: charis-configure.log and charis-build.log (the output of
# the two steps), charis-build-start.txt (the commit the build started from) and,
# when every check passes, charis-release-build.txt (the record of the build).

set -euo pipefail

die() { echo "charis-build: $*" >&2; exit 1; }

step=all
build_name=build-vc170-64
velopack=off
check_args=()
while [ $# -gt 0 ]; do
    case "$1" in
        configure|build|check) step=$1 ;;
        --build-dir) [ $# -ge 2 ] || die "--build-dir needs a name"; build_name=$2; shift ;;
        --velopack) [ $# -ge 2 ] || die "--velopack needs on or off"; velopack=$2; shift ;;
        --) shift; check_args=("$@"); break ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) die "unknown argument: $1 (see --help)" ;;
    esac
    shift
done
case "$velopack" in on|off) ;; *) die "--velopack is on or off, not $velopack" ;; esac
case "$build_name" in
    build-*/*|build-*\\*|build-*" "*) die "--build-dir is a folder name directly in the checkout, without spaces: $build_name" ;;
    build-?*) ;;
    *) die "--build-dir is build-<something> (a folder git ignores), not $build_name" ;;
esac

here="$(cd "$(dirname "$0")" && pwd)"
top="$(cd "$here/../../../.." && pwd)"    # the checkout: autobuild.xml, indra/
[ -f "$top/autobuild.xml" ] || die "no autobuild.xml in $top"
case "$top" in
    *" "*) die "the source path has a space in it ($top): rc.exe fails on such paths; use a path without spaces (or subst a drive letter)" ;;
esac
bdir="$top/$build_name"
python="${PYTHON:-python}"
native() { cygpath -m "$1" 2>/dev/null || printf '%s\n' "$1"; }

run_check() {   # run_check [more options]: release/check-release-build.py on this build folder
    "$python" -I "$(native "$here/release/check-release-build.py")" --build-dir "$(native "$bdir")" \
        --source "$(native "$top")" --velopack "$velopack" "$@" ${check_args[@]+"${check_args[@]}"}
}

if [ "$step" = check ]; then
    [ -d "$bdir" ] || die "no build folder $bdir"
    run_check
    exit
fi

# Visual Studio 2022, 64-bit, English tool output (autobuild reads it).
export AUTOBUILD_VSVER=170 AUTOBUILD_ADDRSIZE=64 VSLANG=1033
# The sources are UTF-8; MSVC on a non-UTF-8 Windows code page (Japanese, ...) reads
# them in that code page otherwise. The released binaries are built with this.
case " ${CL:-} " in *" -utf-8 "*) ;; *) export CL="${CL:+$CL }-utf-8" ;; esac
export PYTHONUTF8=1                  # the build scripts read UTF-8 files with the default encoding
export MSBUILDDISABLENODEREUSE=1     # idle MSBuild nodes otherwise keep build files locked

: "${VULKAN_SDK:?set VULKAN_SDK to the Vulkan SDK folder (1.4.363.0): without it the viewer is built without Vulkan}"
[ -f "$VULKAN_SDK/Include/vulkan/vulkan.h" ] || die "no Include/vulkan/vulkan.h under VULKAN_SDK=$VULKAN_SDK"
[ -f "$VULKAN_SDK/Lib/glslang.lib" ] || die "no Lib/glslang.lib under VULKAN_SDK=$VULKAN_SDK (install the SDK with its libraries)"
grep -qE '^#define VK_HEADER_VERSION 363([^0-9]|$)' "$VULKAN_SDK/Include/vulkan/vulkan_core.h" \
    || die "VULKAN_SDK=$VULKAN_SDK is not the 1.4.363 SDK the releases are built with (VK_HEADER_VERSION in vulkan_core.h)"

: "${AUTOBUILD_VARIABLES_FILE:?set AUTOBUILD_VARIABLES_FILE to secondlife/build-variables/variables (patched, see BUILDING.md)}"
[ -f "$AUTOBUILD_VARIABLES_FILE" ] || die "no file: $AUTOBUILD_VARIABLES_FILE"
grep -q '^LL_BUILD_WINDOWS_RELWITHDEBINFO_SWITCHES="/MD /O2 /Ob2 ' "$AUTOBUILD_VARIABLES_FILE" \
    || die "RelWithDebInfo is not /O2 /Ob2 in $AUTOBUILD_VARIABLES_FILE: apply $here/optimize-relwithdebinfo.patch"

command -v autobuild >/dev/null 2>&1 || die "autobuild not found (release/setup-deps.py, or pip install autobuild)"

# The Python the build runs its scripts with (viewer_manifest.py imports llsd, which
# comes with autobuild). Left to itself, CMake takes the Python in the registry first
# (indra/cmake/Python.cmake), which may not have llsd: the package step then fails
# late in the build. Configure gives it $python (the python on PATH, which env.sh
# makes autobuild's venv, or the environment variable PYTHON), and stops first when
# that one cannot import llsd.
# python_llsd PY: prints PY's full path when it imports llsd; its error shows when not.
# -X utf8: -I ignores PYTHONUTF8, and the path must not come out in the ANSI code page.
python_llsd() { "$1" -I -X utf8 -c 'import llsd, sys; print(sys.executable)'; }
command -v "$python" >/dev/null 2>&1 \
    || die "python not found: $python (on PATH, or set the environment variable PYTHON)"
build_python=""
if [ "$step" != build ]; then
    build_python=$(python_llsd "$python") \
        || die "$python cannot import llsd (above; the package step needs it): source env.sh of release/setup-deps.py, or pip install llsd"
    build_python=$(native "$build_python")
fi

cd "$top"
mkdir -p "$bdir"
# The exe is about to change: the record of the last checked build no longer holds.
rm -f "$bdir/charis-release-build.txt"
# The commit the build starts from; its time is when the build started. The check
# compares both with the checkout afterwards.
commit=""
[ -e "$top/.git" ] && commit=$(git -C "$top" rev-parse HEAD 2>/dev/null || true)
printf 'commit=%s\n' "$commit" > "$bdir/charis-build-start.txt"

# The configuration autobuild reads. For another build folder: a copy of
# autobuild.xml in it with the Windows build_directory changed to it. The copy
# must be named autobuild.xml: autobuild passes only the file's name on to the
# "autobuild install" that CMake runs from indra/ (Prebuilt.cmake), which looks
# for that name from indra/ upwards and finds the checkout's autobuild.xml (the
# same packages). The folder must be directly in the checkout: configure is
# given ..\indra.
config_args=()
if [ "$build_name" != build-vc170-64 ]; then
    config="$bdir/autobuild.xml"
    config_args=(--config-file "$(native "$config")")
    "$python" -I - "$(native "$top/autobuild.xml")" "$(native "$config")" "$(native "$bdir")" <<'PY'
import os, sys
src, dst, bdir = sys.argv[1:4]
data = open(src, "rb").read()
old = b"<string>build-vc${AUTOBUILD_VSVER|170}-$AUTOBUILD_ADDRSIZE</string>"
if data.count(old) != 1:
    sys.exit("autobuild.xml: the Windows build_directory is there %d times, not once" % data.count(old))
data = data.replace(old, b"<string>" + bdir.encode() + b"</string>")
if not os.path.exists(dst) or open(dst, "rb").read() != data:
    open(dst + ".tmp", "wb").write(data)
    os.replace(dst + ".tmp", dst)
PY
fi

if [ "$step" != build ]; then
    # RelWithDebInfoOS: CMake's RelWithDebInfo, open-source packages (OpenJPEG, OpenAL).
    # USE_TRACY=OFF: no profiler in a release. The channel is the viewer's name.
    # CHARIS_RELEASE_FOR_DOWNLOAD=ON: LL_RELEASE_FOR_DOWNLOAD as in the official releases
    # (indra/cmake/00-Common.cmake); without it, a developer build (Develop menu,
    # several viewers at once, the developer-only box on a fatal error).
    # Linking as the official Release: no incremental linking (every call would go
    # through a jump table) and /OPT:REF. /debug stays: the pdb is for crash reports.
    # /OPT:ICF is not added.
    link="/debug /INCREMENTAL:NO /OPT:REF"
    autobuild configure -A 64 -c RelWithDebInfoOS ${config_args[@]+"${config_args[@]}"} -- \
        -DUSE_TRACY=OFF -DVIEWER_CHANNEL="Charis Viewer" -DCHARIS_RELEASE_FOR_DOWNLOAD=ON \
        -DUSE_VELOPACK="${velopack^^}" \
        -DPython3_EXECUTABLE="$build_python" -DPYTHON_EXECUTABLE="$build_python" \
        -DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO="$link" \
        -DCMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO="$link" \
        -DCMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO="$link" 2>&1 | tee "$bdir/charis-configure.log" \
        || die "configure failed ($bdir/charis-configure.log)"
    run_check --configure-only || die "not configured as a release (above): not building"
fi
if [ "$step" != configure ]; then
    # The python this build folder was configured with (a folder configured before
    # the python was given, or a venv moved since, would fail in the package step).
    cached=$(sed -n 's/^PYTHON_EXECUTABLE:FILEPATH=//p' "$bdir/CMakeCache.txt" 2>/dev/null | tr -d '\r' || true)
    [ -n "$cached" ] || die "no PYTHON_EXECUTABLE in $bdir/CMakeCache.txt: configure first (charis-build.sh configure)"
    python_llsd "$cached" >/dev/null \
        || die "the build's python $cached (CMakeCache.txt) cannot import llsd (above): configure again (charis-build.sh configure)"
    rc=0
    autobuild build -A 64 -c RelWithDebInfoOS ${config_args[@]+"${config_args[@]}"} --no-configure 2>&1 \
        | tee "$bdir/charis-build.log" || rc=$?
    # Right after a configure, the compilers that make the precompiled headers can run
    # out of virtual memory (C1076, C3859) when many run at once; a second run gets
    # through. Only then, once.
    # (The tail goes to a variable first: with pipefail, "tail | grep -q" can fail
    # when grep stops reading early.)
    last=""
    if [ $rc != 0 ]; then last=$(tail -300 "$bdir/charis-build.log"); fi
    if [ $rc != 0 ] && grep -qE 'error C1076|error C3859' <<<"$last"; then
        echo "charis-build: the precompiled header compiler ran out of memory: building once more"
        rc=0
        autobuild build -A 64 -c RelWithDebInfoOS ${config_args[@]+"${config_args[@]}"} --no-configure 2>&1 \
            | tee -a "$bdir/charis-build.log" || rc=$?
    fi
    [ $rc = 0 ] || die "the build failed (exit $rc; $bdir/charis-build.log)"
    run_check
fi
