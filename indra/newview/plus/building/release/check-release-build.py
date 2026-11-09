#!/usr/bin/env python3
"""Checks that a build folder holds a Charis Viewer release build, as
../charis-build.sh makes it, and writes a record of it (the stamp).

  python -I check-release-build.py --build-dir DIR [options]

  --build-dir DIR       the build folder (CMakeCache.txt, newview/RelWithDebInfo/)
  --source DIR          the checkout (default: the one this file is in)
  --velopack on|off     the USE_VELOPACK the build must have (default off)
  --configure-only      only what configure decides (CMakeCache, the projects,
                        the build number): run between configure and build
  --configure-log FILE  configure's output, for the build number
                        (default DIR/charis-configure.log)
  --accept-number-file  a build number from plus/version/build-number.txt is
                        fine: for a checkout without the git history whose
                        number file is kept up to date by whoever publishes it
  --allow-untracked P   untracked files under P (a path in the checkout) do not
                        make the checkout unclean (may be given more than once)
  --commit SHA          the full commit id the checkout is a copy of: required
                        when it has no git (the source zip), for the stamp
  --stamp FILE          the record, written when every check passes and removed
                        when one fails (default DIR/charis-release-build.txt)
  --no-stamp            neither write nor remove a stamp

What is checked (each line of the output says OK, NG or WARN):
  CMakeCache   no Tracy, the channel "Charis Viewer", RelWithDebInfo built /O2
               (build-variables with ../optimize-relwithdebinfo.patch), the
               source is this checkout, LL_RELEASE_FOR_DOWNLOAD on, USE_VELOPACK
               as asked, release linking (/debug /INCREMENTAL:NO /OPT:REF)
  projects     no TRACY_ENABLE, LL_RELEASE_FOR_DOWNLOAD=1 in the RelWithDebInfo
               definitions, LL_VULKAN=1 in the viewer (built with the Vulkan
               SDK), the viewer linked without incremental linking
  build number from git or CHARIS_BUILD_NUMBER (see --accept-number-file); not
               the fixed floor, which can be lower than a released number
  output       the viewer exe and the package step's file list are there and
               newer than the link, no Tracy files, no Tracy and no
               developer-only strings in any exe/dll, no Tracy symbols in the pdb
  velopack     with --velopack on: velopack-pack.json (the arguments for vpk
               pack) from the same package step, with the package id of
               plus/independence/independence.h, Charis's version of
               plus/version/version.h (plus/version/packversion.py), the viewer
               exe as the main exe, --noPortable and --runtime win-x64; with off,
               a left-over one is a WARN
  checkout     HEAD is the commit charis-build.sh started from, nothing changed
               or untracked, no source file changed after the build started.
               Without git: --commit is required, and only the last item is
               checked. A checkout with git where git fails is NG.
               If the check itself stops (an error), the stamp is removed.

The stamp (key=value lines) names the commit, the exe's SHA-256 and the result;
the packaging tools read it, and a later step may add lines to it.
Exit status: 0 = every check passed, 2 = a check failed, 1 = cannot check.
"""
import argparse
import datetime
import json
import mmap
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (DEVELOPER_STRINGS, FILE_LIST, OUTPUT_REL, SOURCE, STAMP,  # noqa: E402
                           TRACY_STRINGS, TRACY_SYMBOLS, VELOPACK_PACK_JSON, VIEWER_EXE_NAMES, VPK_RUNTIME,
                           has_vpk_runtime, independence_constant, packversion_module, read_text, read_version,
                           sha256_file, viewer_exe)

CHANNEL = 'Charis Viewer'
CONFIGURE_LOG = 'charis-configure.log'
START_MARK = 'charis-build-start.txt'      # charis-build.sh: commit=<HEAD when the build started>
PDB = 'secondlife-bin.pdb'
KEY_PROJECTS = (os.path.join('llcommon', 'llcommon.vcxproj'),
                os.path.join('newview', 'secondlife-bin.vcxproj'),
                os.path.join('llplugin', 'slplugin', 'SLPlugin.vcxproj'))
VIEWER_PROJECT = os.path.join('newview', 'secondlife-bin.vcxproj')
RELEASE_LINK_FLAGS = ('/debug', '/INCREMENTAL:NO', '/OPT:REF')
# Files whose changes do not change the build (no-git checkouts only).
NOT_SOURCE = re.compile(r'(^|/)__pycache__/|\.pyc$')


class Checks:
    """Collects the results; ok stays True while no check failed."""

    def __init__(self):
        self.ok = True

    def passed(self, text):
        print('OK: ' + text, flush=True)

    def failed(self, text):
        print('NG: ' + text, flush=True)
        self.ok = False

    def warn(self, text):
        print('WARN: ' + text, flush=True)

    def expect(self, condition, good, bad):
        if condition:
            self.passed(good)
        else:
            self.failed(bad)


def norm(path):
    return os.path.normcase(os.path.abspath(path)).replace('\\', '/')


def read_cache(path):
    """CMakeCache.txt as {name: value} (the type is left out)."""
    cache = {}
    for line in read_text(path).splitlines():
        m = re.match(r'^([^#/][^:=]*):[A-Z]+=(.*)$', line)
        if m:
            cache[m.group(1)] = m.group(2)
    return cache


def first(pattern, text):
    m = re.search(pattern, text, re.S)
    return m.group(1) if m else None


def rwdi_group(text):
    """The project's <ItemDefinitionGroup> for RelWithDebInfo|x64 ('' if none)."""
    return first(r'<ItemDefinitionGroup\b[^>]*RelWithDebInfo\|x64[^>]*>(.*?)</ItemDefinitionGroup>', text) or ''


def rwdi_defines(text):
    """The RelWithDebInfo compiler definitions; None when the project compiles nothing."""
    compile_block = first(r'<ClCompile>(.*?)</ClCompile>', rwdi_group(text))
    defs = first(r'<PreprocessorDefinitions>(.*?)</PreprocessorDefinitions>', compile_block or '')
    if defs is None:
        return None
    return {d.strip() for d in defs.split(';') if d.strip() and d.strip() != '%(PreprocessorDefinitions)'}


def rwdi_link(text):
    return first(r'<Link>(.*?)</Link>', rwdi_group(text)) or ''


# ---- CMakeCache and projects (decided by configure) ------------------------------------

def check_cache(c, build, source, velopack):
    path = os.path.join(build, 'CMakeCache.txt')
    if not os.path.isfile(path):
        c.failed('no CMakeCache.txt in %s (not configured)' % build)
        return
    cache = read_cache(path)

    def is_(name, want, why=''):
        have = cache.get(name)
        c.expect(have == want, 'CMakeCache: %s=%s' % (name, want),
                 'CMakeCache: %s is %s, not %s%s' % (name, 'missing' if have is None else repr(have), want, why))

    is_('USE_TRACY', 'OFF')
    is_('VIEWER_CHANNEL', CHANNEL)
    is_('CHARIS_RELEASE_FOR_DOWNLOAD', 'ON', ' (LL_RELEASE_FOR_DOWNLOAD: without it, a developer build)')
    is_('USE_VELOPACK', velopack.upper())
    env = cache.get('LL_BUILD_RELWITHDEBINFO_ENV', '')
    c.expect(re.search(r'(^|\s)/O2(\s|$)', env), 'CMakeCache: RelWithDebInfo is /O2',
             'CMakeCache: no /O2 in LL_BUILD_RELWITHDEBINFO_ENV (apply optimize-relwithdebinfo.patch to build-variables)')
    home = cache.get('CMAKE_HOME_DIRECTORY', '')
    c.expect(home and norm(home) == norm(os.path.join(source, 'indra')),
             'CMakeCache: the source is %s' % home,
             'CMakeCache: the source is %r, not %s' % (home, os.path.join(source, 'indra')))
    for kind in ('EXE', 'SHARED', 'MODULE'):
        name = 'CMAKE_%s_LINKER_FLAGS_RELWITHDEBINFO' % kind
        flags = cache.get(name, '').split()
        c.expect(all(f in flags for f in RELEASE_LINK_FLAGS), 'CMakeCache: %s=%s' % (name, ' '.join(flags)),
                 'CMakeCache: %s is %r, not a release link (%s)' % (name, ' '.join(flags), ' '.join(RELEASE_LINK_FLAGS)))


def check_projects(c, build):
    tracy, no_release = [], []
    for rel in KEY_PROJECTS:
        path = os.path.join(build, rel)
        if not os.path.isfile(path):
            c.failed('no %s' % path)
            return
        text = read_text(path)
        if 'TRACY_ENABLE' in text:
            tracy.append(os.path.basename(rel))
        # The Release block of a project has LL_RELEASE_FOR_DOWNLOAD from
        # build-variables in any build: only the RelWithDebInfo block counts.
        if 'LL_RELEASE_FOR_DOWNLOAD=1' not in (rwdi_defines(text) or ()):
            no_release.append(os.path.basename(rel))
    c.expect(not tracy, 'projects: no TRACY_ENABLE', 'projects with TRACY_ENABLE: ' + ' '.join(tracy))
    c.expect(not no_release, 'projects (RelWithDebInfo): LL_RELEASE_FOR_DOWNLOAD=1',
             'projects without LL_RELEASE_FOR_DOWNLOAD=1 in RelWithDebInfo: ' + ' '.join(no_release))

    viewer = os.path.join(build, VIEWER_PROJECT)
    if os.path.isfile(viewer):
        text = read_text(viewer)
        # indra/llvulkan/llvulkan.cmake defines it only when it found the Vulkan SDK.
        c.expect('LL_VULKAN=1' in (rwdi_defines(text) or ()),
                 '%s (RelWithDebInfo): LL_VULKAN=1 (with Vulkan)' % os.path.basename(viewer),
                 '%s (RelWithDebInfo): no LL_VULKAN=1 (configured without the Vulkan SDK)' % os.path.basename(viewer))
        link = rwdi_link(text)
        incremental_off = ("<LinkIncremental Condition=\"'$(Configuration)|$(Platform)'=='RelWithDebInfo|x64'\">"
                           "false<") in text
        c.expect(incremental_off and '<OptimizeReferences>true<' in link and '<GenerateDebugInformation>true<' in link,
                 '%s (RelWithDebInfo): no incremental link, OptimizeReferences, pdb' % os.path.basename(viewer),
                 '%s (RelWithDebInfo): not a release link (LinkIncremental=false, OptimizeReferences, '
                 'GenerateDebugInformation)' % os.path.basename(viewer))

    # Other C++ projects (directories that do not read 00-Common.cmake): a warning only.
    missing, count = [], 0
    packages = norm(os.path.join(build, 'packages'))
    for top, dirs, files in os.walk(build):
        if norm(top) == packages:
            dirs[:] = []
            continue
        for name in files:
            if name.endswith('.vcxproj'):
                defs = rwdi_defines(read_text(os.path.join(top, name)))
                if defs:
                    count += 1
                    if 'LL_RELEASE_FOR_DOWNLOAD=1' not in defs:
                        missing.append(name[:-len('.vcxproj')])
    if missing:
        c.warn('C++ projects without LL_RELEASE_FOR_DOWNLOAD=1: ' + ' '.join(sorted(missing)))
    elif count:
        c.passed('all %d C++ projects have LL_RELEASE_FOR_DOWNLOAD=1' % count)
    else:
        c.failed('no C++ project with RelWithDebInfo definitions in %s' % build)


def check_build_number(c, log, accept_file):
    """The line indra/newview/plus/version/buildnumber.cmake prints at configure time.
    Returns the number (None when not found)."""
    if not os.path.isfile(log):
        c.failed('no configure output %s (configure with charis-build.sh)' % log)
        return None
    text = read_text(log)
    found = re.findall(r'Charis build number ([0-9]+) \(([^)\r\n]*)', text)
    if not found:
        # The floor prints a CMake warning only.
        if 'CHARIS_BUILD_NUMBER_FLOOR' in text:
            c.failed('build number: the fixed floor (CHARIS_BUILD_NUMBER_FLOOR), which can be lower than a '
                     'released number: clone with the whole history, or set CHARIS_BUILD_NUMBER')
        else:
            c.failed('no build number line in %s (buildnumber.cmake not read?)' % log)
        return None
    number, origin = found[-1]
    good = origin.startswith('from git') or origin.startswith('from the environment')
    if accept_file and origin.startswith('from plus/version/build-number.txt'):
        good = True
    c.expect(good, 'build number %s (%s)' % (number, origin),
             'build number %s (%s): not from git or CHARIS_BUILD_NUMBER: clone with the whole history '
             '(git fetch --unshallow), set CHARIS_BUILD_NUMBER, or --accept-number-file' % (number, origin))
    return number


# ---- output ---------------------------------------------------------------------

def files_holding(path, needles):
    """The needles found in the file (memory-mapped; empty files hold none)."""
    if os.path.getsize(path) == 0:
        return []
    with open(path, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        return [n for n in needles if m.find(n) != -1]


def check_output(c, build):
    out = os.path.join(build, OUTPUT_REL)
    exe = viewer_exe(out)
    if not exe:
        c.failed('no viewer exe (%s) in %s' % (' or '.join(VIEWER_EXE_NAMES), out))
        return None
    if all(os.path.isfile(os.path.join(out, n)) for n in VIEWER_EXE_NAMES):
        c.failed('both %s and the old %s: the old one was running and could not be removed; close it and '
                 'remove it' % VIEWER_EXE_NAMES)
    file_list = os.path.join(out, FILE_LIST)
    if not os.path.isfile(file_list):
        c.failed('no %s (the package step did not run)' % FILE_LIST)
    elif os.path.getmtime(exe) > os.path.getmtime(file_list):
        c.failed('%s is older than %s (the package step did not run after the last link)'
                 % (FILE_LIST, os.path.basename(exe)))
    else:
        c.passed('%s and %s' % (os.path.basename(exe), FILE_LIST))

    # tracy-profiler.exe: the packaging step adds it (and lists it) when Tracy is on.
    tracy_files = []
    for name in os.listdir(out):
        if name.lower().startswith('tracy'):
            tracy_files.append(name)
        sub = os.path.join(out, name)
        if os.path.isdir(sub):
            tracy_files += [name + '/' + n for n in os.listdir(sub) if n.lower().startswith('tracy')]
    c.expect(not tracy_files, 'no Tracy files in the output', 'Tracy files in the output: ' + ' '.join(tracy_files))
    if os.path.isfile(file_list):
        c.expect('tracy' not in read_text(file_list).lower(), '%s: no tracy' % FILE_LIST,
                 '%s lists tracy' % FILE_LIST)

    binaries = []
    for top, dirs, files in os.walk(out):
        binaries += [os.path.join(top, n) for n in files if n.lower().endswith(('.exe', '.dll'))]
    tracy, developer = [], []
    for path in binaries:
        found = files_holding(path, TRACY_STRINGS + DEVELOPER_STRINGS)
        rel = os.path.relpath(path, out)
        if any(n in TRACY_STRINGS for n in found):
            tracy.append(rel)
        if any(n in DEVELOPER_STRINGS for n in found):
            developer.append(rel)
    c.expect(not tracy, 'no Tracy strings in %d exe/dll files' % len(binaries),
             'Tracy strings in: ' + ' '.join(tracy))
    c.expect(not developer, 'no developer-build strings in the exe/dll files (LL_RELEASE_FOR_DOWNLOAD)',
             'developer-build strings (no LL_RELEASE_FOR_DOWNLOAD) in: ' + ' '.join(developer))

    # The pdb has every function and type name (the exe has no COFF symbol table).
    pdb = os.path.join(out, PDB)
    if os.path.isfile(pdb):
        c.expect(not files_holding(pdb, TRACY_SYMBOLS), '%s: no tracy:: symbols' % PDB, '%s has tracy:: symbols' % PDB)
    else:
        c.warn('no %s (its check skipped)' % PDB)
    return exe


def check_velopack(c, build, source, exe, velopack):
    """With USE_VELOPACK on, the package step wrote velopack-pack.json (the arguments for vpk pack,
    run after the shader bundle) for this exe, with the package id llvelopack.cpp uses."""
    pack_json = os.path.join(build, OUTPUT_REL, VELOPACK_PACK_JSON)
    if velopack != 'on':
        if os.path.isfile(pack_json):
            c.warn('%s is left from an earlier package step with USE_VELOPACK on; do not make a Setup.exe '
                   'from this output' % VELOPACK_PACK_JSON)
        return
    if not os.path.isfile(pack_json):
        c.failed('no %s (USE_VELOPACK is on, but the package step did not write it)' % VELOPACK_PACK_JSON)
        return
    if exe and os.path.getmtime(exe) > os.path.getmtime(pack_json):
        c.failed('%s is older than %s (the package step did not run after the last link)'
                 % (VELOPACK_PACK_JSON, os.path.basename(exe)))
        return
    try:
        with open(pack_json, encoding='utf-8') as f:
            pack = json.load(f)
    except (OSError, ValueError) as err:
        c.failed('%s cannot be read: %s' % (VELOPACK_PACK_JSON, err))
        return
    args = pack.get('vpkArgs') if isinstance(pack, dict) else None
    if not isinstance(args, list) or not all(isinstance(x, str) for x in args):
        c.failed('%s has no vpkArgs list' % VELOPACK_PACK_JSON)
        return

    def arg(name):
        return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None

    want_id = independence_constant(source, 'VELOPACK_PACK_ID')
    c.expect(want_id is not None and arg('--packId') == want_id == pack.get('packId'),
             '%s: package id %s (as in llvelopack.cpp)' % (VELOPACK_PACK_JSON, want_id),
             '%s: package id %r, independence.h says %r' % (VELOPACK_PACK_JSON, arg('--packId'), want_id))
    # Charis's version of version.h (plus/version/packversion.py), not the official one
    pack_version = pack.get('packVersion')
    try:
        pack_build = packversion_module().build_of(pack_version, read_version(source))
        problem = None if arg('--packVersion') == pack_version else '--packVersion is %r' % arg('--packVersion')
    except ValueError as err:
        pack_build, problem = None, str(err)
    c.expect(problem is None,
             '%s: version %s (version.h, build %s)' % (VELOPACK_PACK_JSON, pack_version, pack_build),
             '%s: version %r: %s' % (VELOPACK_PACK_JSON, pack_version, problem))
    exe_name = os.path.basename(exe) if exe else None
    c.expect(arg('--mainExe') == exe_name, '%s: main exe %s' % (VELOPACK_PACK_JSON, exe_name),
             '%s: main exe %r, the build has %r' % (VELOPACK_PACK_JSON, arg('--mainExe'), exe_name))
    c.expect('--noPortable' in args, '%s: no Portable.zip (--noPortable)' % VELOPACK_PACK_JSON,
             '%s: --noPortable missing (a Portable.zip would be made)' % VELOPACK_PACK_JSON)
    c.expect(has_vpk_runtime(args),
             '%s: --runtime %s (the 64-bit Update.exe and stub)' % (VELOPACK_PACK_JSON, VPK_RUNTIME),
             '%s: --runtime is %r, not %s once (vpk would pack its x86 Update.exe)'
             % (VELOPACK_PACK_JSON, arg('--runtime'), VPK_RUNTIME))
    c.expect(not any(x in args for x in ('--packDir', '--outputDir')),
             '%s: no --packDir/--outputDir (the release tools add them)' % VELOPACK_PACK_JSON,
             '%s: --packDir or --outputDir is given' % VELOPACK_PACK_JSON)


# ---- the checkout -----------------------------------------------------------------

def git(source, *args):
    try:
        r = subprocess.run(('git', '-C', source) + args, check=True, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return None
    return r.stdout


def has_git(source):
    """True when source is the top of a git work tree (not a folder inside another one).
    --show-cdup is empty at the top; --show-toplevel would resolve a subst drive."""
    up = git(source, 'rev-parse', '--show-cdup')
    return up is not None and up.strip() == b''


def read_start_mark(build):
    """(path, commit) of charis-build.sh's start mark; (None, None) without one."""
    path = os.path.join(build, START_MARK)
    if not os.path.isfile(path):
        return None, None
    m = re.search(r'^commit=([0-9a-f]*)\s*$', read_text(path), re.M)
    return path, (m.group(1) if m else '')


def source_files(source, with_git):
    """The source files the build reads, relative to the checkout."""
    if with_git:
        listed = git(source, 'ls-files', '-z', '--', 'indra', 'scripts', 'autobuild.xml') or b''
        return [p for p in listed.decode('utf-8', 'replace').split('\0') if p]
    files = ['autobuild.xml']
    for sub in ('indra', 'scripts'):
        for top, dirs, names in os.walk(os.path.join(source, sub)):
            files += [os.path.relpath(os.path.join(top, n), source).replace('\\', '/') for n in names]
    return [f for f in files if not NOT_SOURCE.search(f)]


def check_checkout(c, build, source, exe, allow_untracked, given_commit):
    """Returns the commit for the stamp ('' when there is none: then a check failed)."""
    mark, mark_commit = read_start_mark(build)
    with_git = has_git(source)
    commit = ''
    if with_git:
        head = git(source, 'rev-parse', 'HEAD')
        status = git(source, 'status', '--porcelain')
        if head is None or status is None:
            c.failed('git rev-parse HEAD or git status failed in %s: the checkout cannot be checked' % source)
            return ''
        commit = head.decode('ascii', 'replace').strip()
        if mark_commit is not None:
            c.expect(mark_commit == commit, 'HEAD is the commit the build started from (%s)' % commit[:10],
                     'HEAD moved since the build started (%s -> %s): build again' % (mark_commit[:10] or '?', commit[:10]))
        prefixes = tuple(p.replace('\\', '/').rstrip('/') + '/' for p in allow_untracked)
        dirty = [line for line in status.decode('utf-8', 'replace').splitlines()
                 if line and not (prefixes and line.startswith('?? ') and line[3:].startswith(prefixes))]
        # Untracked files count too: the build picks some up by pattern (plus/*/*.cmake).
        c.expect(not dirty, 'the checkout is clean (%s)' % commit[:10],
                 'changed or untracked files in the checkout (the first 20):\n  ' + '\n  '.join(dirty[:20]))
    elif os.path.exists(os.path.join(source, '.git')) or mark_commit:
        # A git checkout (or a build started from one) where git does not answer
        # now: nothing about the checkout can be told, so no record.
        c.failed('%s has git (%s) but git does not work there now: the checkout cannot be checked'
                 % (source, '.git' if os.path.exists(os.path.join(source, '.git')) else 'the build started from commit '
                    + mark_commit[:10]))
        return ''
    elif not given_commit:
        c.failed('%s is not a git checkout: give the commit it is a copy of with --commit' % source)
        return ''
    else:
        commit = given_commit
        c.warn('%s is not a git checkout: the stamp says commit=%s (--commit); changes since that commit '
               'cannot be told' % (source, commit))

    # A file touched after the build started (even if put back since) means the
    # exe may not be of these sources. Without a start mark: newer than the exe.
    ref = mark or exe
    if ref:
        since = os.path.getmtime(ref)
        newer = []
        for rel in source_files(source, with_git):
            try:
                if os.path.getmtime(os.path.join(source, rel)) > since:
                    newer.append(rel)
            except OSError:
                pass
        what = 'after the build started' if mark else 'after %s' % os.path.basename(exe)
        c.expect(not newer, 'no source file changed %s' % what,
                 'source files changed %s (the first 10; build again):\n  %s' % (what, '\n  '.join(newer[:10])))
    return commit


# ---- stamp ------------------------------------------------------------------------

def write_stamp(path, fields):
    with open(path + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        f.writelines('%s=%s\n' % kv for kv in fields)
    os.replace(path + '.tmp', path)


def commit_id(text):
    """--commit: a full commit id (40 lower-case hex digits)."""
    if not re.fullmatch(r'[0-9a-f]{40}', text):
        raise argparse.ArgumentTypeError('not a full commit id (40 lower-case hex digits): %r' % text)
    return text


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--source', default=SOURCE)
    ap.add_argument('--velopack', choices=('on', 'off'), default='off')
    ap.add_argument('--configure-only', action='store_true')
    ap.add_argument('--configure-log')
    ap.add_argument('--accept-number-file', action='store_true')
    ap.add_argument('--allow-untracked', action='append', default=[])
    ap.add_argument('--commit', type=commit_id)
    ap.add_argument('--stamp')
    ap.add_argument('--no-stamp', action='store_true')
    a = ap.parse_args()
    build = os.path.abspath(a.build_dir)
    source = os.path.abspath(a.source)
    if not os.path.isdir(build):
        print('no build folder: %s' % build)
        return 1
    stamp = None if a.no_stamp or a.configure_only else os.path.abspath(a.stamp or os.path.join(build, STAMP))
    try:
        return check(a, build, source, stamp)
    except BaseException:
        # Whatever stopped the check (a bug, an unreadable file, Ctrl+C): the
        # record of an earlier check no longer holds.
        if stamp and os.path.isfile(stamp):
            os.remove(stamp)
        print('RELEASE-CHECK: NG (the check stopped; no release may be made from this output)')
        raise


def check(a, build, source, stamp):
    c = Checks()
    print('---- release build check of %s (%s)' % (build, datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')))
    check_cache(c, build, source, a.velopack)
    check_projects(c, build)
    number = check_build_number(c, a.configure_log or os.path.join(build, CONFIGURE_LOG), a.accept_number_file)
    if a.configure_only:
        print('CONFIGURE-CHECK: %s' % ('OK' if c.ok else 'NG (not configured as a release: do not build)'))
        return 0 if c.ok else 2
    exe = check_output(c, build)
    check_velopack(c, build, source, exe, a.velopack)
    commit = check_checkout(c, build, source, exe, a.allow_untracked, a.commit)

    if c.ok and exe and commit:
        if stamp:
            write_stamp(stamp, (
                ('commit', commit),
                ('exe_sha256', sha256_file(exe)),
                ('checked', datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')),
                ('channel', CHANNEL),
                ('use_tracy', 'OFF'),
                ('release_for_download', 'ON'),
                ('use_velopack', a.velopack.upper()),
                ('build_number', number),
                ('check', 'OK'),
            ))
            print('RELEASE-CHECK: OK (wrote %s)' % stamp)
        else:
            print('RELEASE-CHECK: OK')
        return 0
    if stamp and os.path.isfile(stamp):
        os.remove(stamp)
    print('RELEASE-CHECK: NG (no release may be made from this output)')
    return 2


if __name__ == '__main__':
    sys.exit(main())
