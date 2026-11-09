#!/usr/bin/env python3
"""Bakes the shader bundle (app_settings/vk_shader_bundle/shaders.vkb) of a
release build on a machine without a GPU: the viewer runs --bake-shaders on
Mesa's lavapipe, a software Vulkan driver. The SPIR-V in the bundle is
glslang's output and does not depend on the device; the device only answers
which sources are made. lavapipe answers as a hardware GPU does: a bake on it
gave the same keys with the same bytes (a few more keys from the start-up load).

  python -I bake-shaders-ci.py --build-dir DIR [options]
  (the release workflow runs bake-shaders-ci.sh, which runs this)

In the release workflow this runs in a job of its own (release-package.yml,
bake), on a copy of the release files the build job handed over
(job-handoff.py), with --bundle-out outside that copy: Mesa never runs where
the release is packed. The package job takes only the bundle and its manifest
and reads them back (shaderbundle.py check). The checks of steps 4 and 6 then
guard this job's copy: a bake that changed it, or a copy with a Mesa file in it,
is not trusted (exit 3); they are also what protects a bake run where the
release is packed (by hand).

Steps, each stopping the run when it fails; no bundle is put in place then.
Exit status: 0 with the bundle in place, 1 when a step failed, 2 for wrong
arguments, TAMPERED_EXIT (3) when what the release is made from changed while
Mesa ran or a Mesa file is in it (the workflow stops on 3 whatever the build is
for; on 1 it stops a release and lets a test build go on without the bundle).
  1. The build: DIR/charis-release-build.txt (check-release-build.py) says
     check=OK, no Tracy, LL_RELEASE_FOR_DOWNLOAD, and the exe's SHA-256 is the
     one it records; its commit= goes into the bundle's header. The Vulkan
     loader is in System32 (the viewer loads it from there only).
  2. Mesa MESA_VERSION from mesa-dist-win: the archive's size and SHA-256 are
     checked before anything in it is unpacked or loaded. It is unpacked with
     7-Zip (--seven-zip; not Windows' tar.exe, which reads this 7z on some
     Windows builds only): its listing first, where no member's name may leave
     the folder (releasecommon.checked_rel: no drive, ":", "..", device
     name) and lavapipe's dll and manifest
     must each be there once as a plain file; then only those two, into
     WORK/mesa, with 7-Zip's making of links and alternate data streams off
     (-snl- -snh- -sns-), which must then hold exactly them at the listed
     sizes (no link, reparse point or anything else).
  3. The Vulkan loader is pointed at lavapipe. A process that is not elevated:
     VK_DRIVER_FILES, for the viewer only. An elevated one ignores that
     variable: then, and only with --register-icd on a GitHub-hosted runner
     (runner_problems(): GITHUB_ACTIONS=true, RUNNER_ENVIRONMENT=github-hosted,
     RUNNER_NAME set, ImageOS win*, RUNNER_TEMP a folder that is not a drive's
     root, the manifest under it: a machine thrown away after the job), the
     manifest is registered in HKLM\\SOFTWARE\\Khronos\\Vulkan\\Drivers under
     its own path and removed at the end (a value of that same path left by a
     killed run is taken over and removed too). Elevated in any other case:
     stop. The registry is not read or written in any other case.
  4. Before anything of Mesa runs, release_inputs() records what the release
     is made from as far as this step sees it: every file of the output folder
     (size, SHA-256), the stamp and the pdb files outside it, and the sizes of
     GITHUB_ENV, GITHUB_PATH and GITHUB_OUTPUT; a file with more than one hard
     link stops it (3). The viewer bakes once a vendor, in a job object that
     ends whatever it started when it exits: --bake-shaders WORK/out
     --bake-shaders-vendors V --logfile ..., with APPDATA, LOCALAPPDATA, TEMP
     and TMP in WORK/env/V, none of the caller's DROPPED_ENV_PREFIXES variables
     (the runner's among them), and the GPU class pinned (START_CLASS_ARGS).
     Its report-V.txt must be V's and say status=ok, a gpu= with "llvmpipe",
     failed_steps=0 and no env= lines, and its log a start at graphics level
     EXPECT_START_LEVEL. Then release_inputs() must be as recorded (3 if not).
  5. The caches are packed (shaderbundle.py; a file it would leave out stops
     it), read back, and their manifest (every key and the SHA-256 of its
     contents) written to WORK/bundle-manifest.tsv; with --baseline, compared
     with a manifest of a bake on a hardware GPU (research/312 section 4.1).
  6. No Mesa file in the build's output folder (3 if there is);
     WORK/bake-record.txt says what was used and made; then the bundle is put
     in place.

Options
  --build-dir DIR       the build folder (has newview/RelWithDebInfo)
  --vendors LIST        nvidia,amd,other (default all three; intel is other).
                        With --pack-only, the vendors the reports must cover
  --work DIR            scratch folder (default RUNNER_TEMP/charis-bake). Must be
                        empty, new, or one this tool made (then the items it
                        makes there are removed)
  --mesa-archive FILE   use this copy of the archive instead of fetching it
                        (checked the same way; not inside --work)
  --seven-zip FILE      7-Zip's 7z.exe (default ProgramFiles\\7-Zip\\7z.exe, where
                        GitHub's Windows runner images have it). Without it the
                        run stops before Mesa is fetched (not with --pack-only)
  --bake-only           steps 1-4 only: WORK/out holds the cache and reports
                        (one job a vendor, then a --pack-only job)
  --pack-only DIR...    steps 5-6 only, from the WORK/out folders of
                        --bake-only runs (their reports are checked again)
  --baseline TSV        a manifest (shaderbundle.py manifest) to compare with
                        (not inside --work)
  --expect-caps TEXT    the caps= line the report must have (research/312
                        question 6; without this option a caps= line is only
                        recorded). The viewer writes no caps= line yet, so this
                        option always stops for now: use it after the bake-caps
                        branch
  --bundle-out FILE     where the bundle goes (default the output folder's
                        app_settings/vk_shader_bundle/shaders.vkb)
  --timeout-minutes N   for one vendor's viewer run (default 60, 1 to 300)
  --register-icd        allow step 3's registry entry (the release workflow
                        passes it; without it an elevated run stops)
  --install-vulkan-runtime
                        GitHub-hosted runner only (the conditions of step 3,
                        with WORK under RUNNER_TEMP): without vulkan-1.dll in
                        System32, run the Vulkan SDK's VulkanRT.exe (VULKAN_SDK;
                        size and SHA-256 checked, run from a copy in WORK)
  --allow-dev-build     trials only: an exe without the release stamp; needs
                        --commit when the build folder has no stamp
  --commit SHA          the commit for the header; must match the stamp's
                        commit= whenever the stamp has one

Network: the Mesa archive from github.com (step 2) only. The viewer starts as
always up to its login screen (step 4) and quits there.
"""
import argparse
import ctypes
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (MESA_FILE_NAMES, OUTPUT_REL, PDBS, STAMP, DownloadFailed, FileListError,  # noqa: E402
                           checked_rel, download, read_stamp, sha256_file, stamp_problems, viewer_exe)
import shaderbundle  # noqa: E402

MESA_VERSION = '26.2.6'
MESA_ARCHIVE = 'mesa3d-%s-release-msvc.7z' % MESA_VERSION
MESA_URL = 'https://github.com/pal1000/mesa-dist-win/releases/download/%s/%s' % (MESA_VERSION, MESA_ARCHIVE)
MESA_SIZE = 55964101
MESA_SHA256 = 'b2e00615feaba45942343c265cc8c5f576142ca682c1ac6661922d4b72492877'
# What is unpacked: lavapipe needs only system dlls (its imports: kernel32, user32,
# gdi32, advapi32, shell32, ole32, ntdll).
MESA_ICD = 'x64/lvp_icd.x86_64.json'
MESA_DLL = 'x64/vulkan_lvp.dll'
MESA_MEMBERS = (MESA_ICD, MESA_DLL)
ICD_LIBRARY_PATH = '.\\vulkan_lvp.dll'
# The archive is unpacked with 7-Zip (--seven-zip, default default_seven_zip(): GitHub's Windows
# runner images carry it there). Not Windows' tar.exe: whether its libarchive reads a 7z with
# LZMA depends on the Windows build (it did on Windows 11; on windows-2022 it says "LZMA codec
# is unsupported"), so the same step passed on one machine and failed on another. There is no
# fallback to it: one unpacker everywhere, so a run by hand tests what the release does.
# In the 7-Zip listing (`l -slt`), the Attributes value is made of: a token of its Windows
# attribute letters (one a bit, from FILE_ATTRIBUTE_READONLY up, 7-Zip 22 and later's included),
# of which those for a folder, a reparse point (a link) and a device are refused; bits it has
# no letter for, as 8 hex digits; and a Unix mode token ("-rw-r--r--"; the first letter is the
# type). For the members unpacked, any other token is refused too (a form this tool does not
# know). A link the listing does not show is still refused after unpacking (unpacked_problems).
SEVEN_ZIP_ATTRIBS = re.compile(r'[RHS8DAdNTsLCOIEVvXPUMB]+')
SEVEN_ZIP_BAD_ATTRIBS = frozenset('DLd')
SEVEN_ZIP_HEX_ATTRIBS = re.compile(r'(?:0x)?[0-9A-Fa-f]{8}')
UNIX_MODE = re.compile(r'[-dlcbps][-rwxsStT]{9}')
# The Vulkan runtime (loader) installer in the Vulkan SDK the build uses (setup-deps.py's
# VULKAN_SDK_VERSION), for a runner without vulkan-1.dll in System32.
VULKAN_RT_SDK = '1.4.363.0'
VULKAN_RT_SIZE = 25699768
VULKAN_RT_SHA256 = 'f8862d5c9a3e09b6c41df74f4500e56b4ac5ce77cbca96478fe34a238a48c35d'
VULKAN_DRIVERS_KEY = r'SOFTWARE\Khronos\Vulkan\Drivers'
EXPECT_GPU = 'llvmpipe'         # in report-<vendor>.txt's gpu= (the renderer string, upper case there)

# The start-up load (before the bake's first set) runs with the level the GPU class
# gives, and that state carries into the bake's sets: a bake started at class 1 made
# a few hundred keys other than one started at class 2 or 5. The class comes from a
# timed memory benchmark, and lavapipe measures about 48 GB/s, at the class 1/2 line
# (RenderClass1MemoryBandwidth). With the line at 0.5 GB/s every lavapipe run is
# class 5, level 5 (featuretable.txt's Integrated mask, for a CPU device, holds it
# below 6). Its sets then match a hardware GPU's bake started at level 6; a start at
# level 2 also did, one at level 1 did not.
START_CLASS_ARGS = ['--set', 'RenderClass1MemoryBandwidth', '0.5']
EXPECT_START_LEVEL = '5'

VENDORS = ('nvidia', 'amd', 'other')        # grid.xml's, the names --bake-shaders-vendors takes
VENDOR_ALIASES = {'intel': 'other'}         # the bake's "other" sets the Intel flag
BUNDLE_REL = os.path.join('app_settings', 'vk_shader_bundle', 'shaders.vkb')
GRID_REL = os.path.join('app_settings', 'plus', 'shaderbake', 'grid.xml')
WORK_MARK = '.charis-bake-ci'
# What this tool makes in WORK (and removes there on its next run; nothing else is removed).
WORK_ITEMS = ('mesa-download', 'mesa', 'vulkanrt', 'env', 'out', 'shaders.vkb', 'shaders.vkb.tmp',
              'bundle-manifest.tsv', 'vulkaninfo-summary.txt', 'bake-record.txt')
# Left out of the viewer's environment: its own switches (a bake with them makes
# keys the default run misses), the Vulkan loader's (this tool picks the driver),
# Mesa's (this tool sets what lavapipe needs) and the runner's (GITHUB_ENV,
# GITHUB_PATH, GITHUB_OUTPUT: files that set the next steps' environment, and tokens).
DROPPED_ENV_PREFIXES = ('LLRHI_', 'VK_', 'MESA_', 'LP_', 'GALLIUM_', 'GITHUB_', 'ACTIONS_', 'RUNNER_')


class Stop(Exception):
    pass


class Tampered(Stop):
    """Something the release is made from changed while Mesa ran, or a Mesa file is in it:
    exit status TAMPERED_EXIT, which stops the workflow whatever the build is for."""


TAMPERED_EXIT = 3


def say(text):
    print('bake-shaders-ci: ' + text, flush=True)


def in_ci():
    return os.environ.get('GITHUB_ACTIONS') == 'true'


def on_hosted_runner():
    """A GitHub-hosted runner: a fresh machine for one job, thrown away after it."""
    return in_ci() and os.environ.get('RUNNER_ENVIRONMENT') == 'github-hosted'


def system32():
    return os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32')


def is_elevated():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except (AttributeError, OSError):
        raise Stop('not Windows (the bake runs the Windows build)')


def is_inside(path, folder):
    path, folder = os.path.normcase(os.path.abspath(path)), os.path.normcase(os.path.abspath(folder))
    try:
        return os.path.commonpath([path, folder]) == folder
    except ValueError:      # other drives
        return False


# ---- 1. the build ------------------------------------------------------------

def check_build(a):
    """(output folder, exe, commit)."""
    build = os.path.abspath(a.build_dir)
    output = os.path.join(build, OUTPUT_REL)
    exe = viewer_exe(output)
    if not exe:
        raise Stop('no viewer exe in %s' % output)
    if not os.path.isfile(os.path.join(output, GRID_REL)):
        raise Stop('%s has no bake mode (no %s)' % (exe, GRID_REL))
    stamp = read_stamp(build)
    stamp_commit = stamp.get('commit', '')
    if a.commit and stamp_commit and a.commit != stamp_commit:
        raise Stop('--commit %s is not the commit of %s (%s)' % (a.commit, os.path.join(build, STAMP), stamp_commit))
    if a.allow_dev_build:
        commit = a.commit or stamp_commit
        if not re.fullmatch(r'[0-9a-f]{40}', commit):
            raise Stop('--allow-dev-build needs --commit <full commit id> (got %r)' % commit)
        say('trial with a build that is not checked for release: %s' % exe)
        return output, exe, commit
    wrong = stamp_problems(stamp)
    if wrong:
        raise Stop('%s does not say check=OK, use_tracy=OFF, release_for_download=ON (%s): run '
                   'check-release-build.py first' % (os.path.join(build, STAMP), ', '.join(wrong)))
    if stamp.get('exe_sha256') != sha256_file(exe):
        raise Stop('%s changed after check-release-build.py checked it' % exe)
    if not re.fullmatch(r'[0-9a-f]{40}', stamp_commit):
        raise Stop('the stamp has no full commit= (%r)' % stamp_commit)
    return output, exe, stamp_commit


def parse_vendors(text):
    vendors = []
    for name in text.split(','):
        name = VENDOR_ALIASES.get(name.strip().lower(), name.strip().lower())
        if not name:
            continue
        if name not in VENDORS:
            raise Stop('unknown vendor %r (%s; intel is other)' % (name, ', '.join(VENDORS)))
        if name not in vendors:
            vendors.append(name)
    if not vendors:
        raise Stop('no vendor')
    return vendors


def prepare_work(path):
    """Makes the scratch folder. One that holds files must carry this tool's mark;
    only the items this tool makes there (WORK_ITEMS) are removed."""
    path = os.path.abspath(path)
    if os.path.isdir(path) and os.listdir(path):
        if not os.path.isfile(os.path.join(path, WORK_MARK)):
            raise Stop('%s is not empty and was not made by this tool: give a new or empty --work' % path)
        for name in WORK_ITEMS:
            item = os.path.join(path, name)
            if os.path.isdir(item) and not os.path.islink(item):
                shutil.rmtree(item)
            elif os.path.lexists(item):
                os.remove(item)
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, WORK_MARK), 'w', encoding='utf-8') as f:
        f.write('scratch folder of bake-shaders-ci.py: what it makes here is removed on its next run\n')
    return path


# ---- 2. Mesa and the Vulkan runtime ------------------------------------------

def default_seven_zip():
    return os.path.join(os.environ.get('ProgramW6432') or os.environ.get('ProgramFiles') or r'C:\Program Files',
                        '7-Zip', '7z.exe')


def name_problem(name):
    """Why an archive member's name (7-Zip lists it with backslashes) is not a plain relative path
    that stays in the folder it is unpacked into (releasecommon.checked_rel: no drive or ':', no
    leading slash, no empty, . or .. part, no device name); '' if it is one."""
    try:
        checked_rel(name.replace('\\', '/'), 'the 7-Zip listing')
    except FileListError as e:
        return str(e)
    return ''


def parse_7z_listing(text):
    """The members of `7z l -slt` output: one {key: value} a member, each starting at its Path line.
    The archive's own block (before the '----------' line) is not a member."""
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.strip() == '----------')
    except StopIteration:
        raise Stop('the 7-Zip listing has no "----------" line (not the `l -slt` form this tool reads)')
    members = []
    for line in lines[start + 1:]:
        key, sep, value = line.partition(' =')
        if not sep or not key or key != key.strip():
            continue        # a blank line between members, or a line that is not "Key = value"
        value = value[1:] if value.startswith(' ') else value
        if key == 'Path':
            members.append({})
        elif not members:
            raise Stop('the 7-Zip listing has a %r line before the first Path' % key)
        members[-1][key] = value
    return members


def listed_file_problem(member):
    """Why a member of the 7-Zip listing is not a plain file with a size ('' if it is)."""
    if member.get('Folder', '-') not in ('-', ''):
        return 'a folder'
    if member.get('Anti') == '+':
        return 'an anti-item'
    for key in ('Symbolic Link', 'Hard Link', 'Link'):
        if member.get(key):
            return 'a link (%s)' % key
    for token in member.get('Attributes', '').split():
        if UNIX_MODE.fullmatch(token):
            if token[0] != '-':
                return 'not a plain file (mode %s)' % token
        elif SEVEN_ZIP_ATTRIBS.fullmatch(token):
            if set(token) & SEVEN_ZIP_BAD_ATTRIBS:
                return 'a folder, link or device (attributes %s)' % token
        elif not SEVEN_ZIP_HEX_ATTRIBS.fullmatch(token):
            return 'attributes this tool does not know (%s)' % token
    if not re.fullmatch(r'[0-9]+', member.get('Size', '')):
        return 'no size'
    return ''


def member_problems(members, wanted):
    """(problems, {wanted name: bytes}) for unpacking `wanted` from the listed members: no member's name
    may leave the folder (name_problem), and each wanted name (compared as 7-Zip does on Windows: either
    slash, any case) must be there once, as a plain file."""
    problems = []
    found = {w: [] for w in wanted}
    by_key = {w.replace('\\', '/').lower(): w for w in wanted}
    for member in members:
        name = member.get('Path', '')
        why = name_problem(name)
        if why:
            problems.append(why)
        w = by_key.get(name.replace('\\', '/').lower())
        if w:
            found[w].append(member)
    sizes = {}
    for w, listed in found.items():
        if len(listed) != 1:
            problems.append('%s is there %d times, not once' % (w, len(listed)))
            continue
        why = listed_file_problem(listed[0])
        if why:
            problems.append('%s: %s' % (w, why))
        else:
            sizes[w] = int(listed[0]['Size'])
    return problems, sizes


def seven_zip_list_command(seven_zip, archive):
    return [seven_zip, 'l', '-t7z', '-slt', '-sccUTF-8', '--', archive]


def seven_zip_extract_command(seven_zip, archive, dest, members):
    """Unpacks exactly `members` (archive paths, no wildcard) of the 7z archive into dest with their
    paths. 7-Zip's x makes symbolic links, hard links and alternate data streams by default, so
    -snl- -snh- -sns- turn that off: a link or stream in the archive is not made as one. `--` ends
    the switches before the paths."""
    return [seven_zip, 'x', '-t7z', '-y', '-bd', '-r-', '-snl-', '-snh-', '-sns-', '-sccUTF-8', '-o' + dest,
            '--', archive] + [m.replace('/', '\\') for m in members]


def is_link_or_reparse(st):
    return stat.S_ISLNK(st.st_mode) or bool(getattr(st, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def unpacked_problems(dest, sizes):
    """Why dest does not hold exactly `sizes` ({archive path: bytes}) as plain files: something else
    there, a file missing or of another size, a link or reparse point (not gone into), a hard link."""
    want = {os.path.normcase(os.path.normpath(name)): (name, size) for name, size in sizes.items()}
    folders = set()
    for rel in want:
        rel = os.path.dirname(rel)
        while rel:
            folders.add(rel)
            rel = os.path.dirname(rel)
    problems, seen, stack = [], set(), ['']
    while stack:
        here = stack.pop()
        with os.scandir(os.path.join(dest, here)) as entries:
            for entry in entries:
                rel = os.path.normcase(os.path.join(here, entry.name))
                st = os.lstat(entry.path)     # not entry.stat(): on Windows its st_nlink is always 0
                if is_link_or_reparse(st):
                    problems.append('%s is a link or reparse point' % rel)
                elif stat.S_ISDIR(st.st_mode):
                    if rel in folders:
                        stack.append(rel)
                    else:
                        problems.append('%s: a folder that was not asked for' % rel)
                elif not stat.S_ISREG(st.st_mode) or rel not in want:
                    problems.append('%s was not asked for' % rel)
                elif st.st_size != want[rel][1]:
                    problems.append('%s is %d bytes, not the listed %d' % (rel, st.st_size, want[rel][1]))
                elif st.st_nlink > 1:
                    problems.append('%s has %d hard links' % (rel, st.st_nlink))
                else:
                    seen.add(rel)
    problems += ['%s was not unpacked' % name for rel, (name, _) in sorted(want.items()) if rel not in seen]
    return problems


def command_failure(e):
    """A failed subprocess.run as text, with the end of what it wrote to stderr when that was captured."""
    err = getattr(e, 'stderr', None)
    if isinstance(err, bytes) and err.strip():
        return '%s (%s)' % (e, err.decode('utf-8', errors='replace').strip()[-400:])
    return str(e)


def unpack_lavapipe(seven_zip, archive, dest):
    """MESA_MEMBERS of archive into dest (made here) with 7-Zip: the listing is checked first
    (member_problems), then only those members are unpacked, then dest must hold exactly them at
    the listed sizes (unpacked_problems). Any failure stops."""
    try:
        listing = subprocess.run(seven_zip_list_command(seven_zip, archive), check=True, timeout=600,
                                 capture_output=True)
    except (OSError, subprocess.SubprocessError) as e:
        raise Stop('listing %s with %s: %s' % (archive, seven_zip, command_failure(e)))
    problems, sizes = member_problems(parse_7z_listing(listing.stdout.decode('utf-8', errors='replace')),
                                      MESA_MEMBERS)
    if problems:
        raise Stop('%s is not unpacked: %s' % (archive, '; '.join(problems[:10])))
    os.makedirs(dest)
    try:
        subprocess.run(seven_zip_extract_command(seven_zip, archive, dest, MESA_MEMBERS), check=True, timeout=600)
    except (OSError, subprocess.SubprocessError) as e:
        raise Stop('unpacking %s with %s: %s' % (archive, seven_zip, command_failure(e)))
    problems = unpacked_problems(dest, sizes)
    if problems:
        raise Stop('%s after unpacking %s: %s' % (dest, archive, '; '.join(problems[:10])))


def fetch_mesa(work, given, seven_zip):
    """The checked archive's lavapipe, unpacked in WORK/mesa: the manifest's path."""
    seven_zip = os.path.abspath(seven_zip)
    if not os.path.isfile(seven_zip):
        raise Stop('no 7-Zip at %s: it unpacks the Mesa archive (GitHub\'s Windows runner images have it '
                   'there). Install 7-Zip or give --seven-zip' % seven_zip)
    if given:
        archive = os.path.abspath(given)
        if not os.path.isfile(archive) or os.path.getsize(archive) != MESA_SIZE or sha256_file(archive) != MESA_SHA256:
            raise Stop('%s is not %s (%d bytes, sha256 %s)' % (archive, MESA_ARCHIVE, MESA_SIZE, MESA_SHA256))
    else:
        archive = os.path.join(work, 'mesa-download', MESA_ARCHIVE)
        try:
            download(MESA_URL, archive, MESA_SIZE, MESA_SHA256, say)
        except DownloadFailed as e:
            raise Stop(str(e))
    say('Mesa %s: %s (%d bytes, sha256 %s)' % (MESA_VERSION, archive, MESA_SIZE, MESA_SHA256))
    dest = os.path.join(work, 'mesa')
    unpack_lavapipe(seven_zip, archive, dest)
    icd, dll = (os.path.normpath(os.path.join(dest, member)) for member in MESA_MEMBERS)
    if not os.path.isfile(icd) or not os.path.isfile(dll):
        raise Stop('%s has no %s or %s' % (archive, MESA_ICD, MESA_DLL))
    try:
        with open(icd, encoding='utf-8') as f:
            library = json.load(f)['ICD']['library_path']
    except (OSError, ValueError, KeyError, TypeError) as e:
        raise Stop('%s: not a Vulkan driver manifest (%s)' % (icd, e))
    if library != ICD_LIBRARY_PATH:
        raise Stop('%s names %r, not %r next to it' % (icd, library, ICD_LIBRARY_PATH))
    say('lavapipe: %s (sha256 %s), %s (sha256 %s)' % (dll, sha256_file(dll), icd, sha256_file(icd)))
    return icd


def is_vulkan_rt(path):
    return os.path.isfile(path) and os.path.getsize(path) == VULKAN_RT_SIZE and sha256_file(path) == VULKAN_RT_SHA256


def ensure_vulkan_runtime(work, install):
    """The viewer loads the Vulkan loader from System32 only. Without one there: with install, on a
    GitHub-hosted runner, the Vulkan SDK's runtime installer (VULKAN_SDK/Helpers/VulkanRT.exe) is
    checked, copied into a new folder in WORK, checked again and run silently from there;
    otherwise stop."""
    loader = os.path.join(system32(), 'vulkan-1.dll')
    if os.path.isfile(loader):
        say('Vulkan loader: %s' % loader)
        return
    if not install:
        raise Stop('no %s (the viewer loads the Vulkan loader from System32 only): install the Vulkan '
                   'runtime, or pass --install-vulkan-runtime on a GitHub-hosted runner' % loader)
    folder = os.path.join(work, 'vulkanrt')
    problems = runner_problems(folder)
    if problems:
        raise Stop('--install-vulkan-runtime changes System32: only on a GitHub-hosted runner (%s)'
                   % '; '.join(problems))
    sdk = os.environ.get('VULKAN_SDK', '')
    source = os.path.join(sdk, 'Helpers', 'VulkanRT.exe')
    if not sdk or not is_vulkan_rt(source):
        raise Stop('no Vulkan runtime installer of SDK %s at %r (VULKAN_SDK/Helpers/VulkanRT.exe, %d bytes, '
                   'sha256 %s)' % (VULKAN_RT_SDK, source, VULKAN_RT_SIZE, VULKAN_RT_SHA256))
    os.makedirs(folder)
    installer = os.path.join(folder, 'VulkanRT.exe')
    shutil.copyfile(source, installer)
    if not is_vulkan_rt(installer):
        raise Stop('the copy %s is not the checked installer' % installer)
    say('installing the Vulkan runtime: %s /S' % installer)
    try:
        subprocess.run([installer, '/S'], check=True, timeout=600, cwd=folder)
    except (OSError, subprocess.SubprocessError) as e:
        raise Stop('%s: %s' % (installer, e))
    if not os.path.isfile(loader):
        raise Stop('%s ran, but there is still no %s' % (installer, loader))
    say('Vulkan loader: %s (installed)' % loader)


# ---- 3. the Vulkan driver ----------------------------------------------------

def runner_problems(path):
    """Why this is not a GitHub-hosted runner to change the machine on (HKLM, System32) with path
    (what goes there) under its RUNNER_TEMP; empty: it is."""
    problems = []
    if not on_hosted_runner():
        problems.append('not GITHUB_ACTIONS=true with RUNNER_ENVIRONMENT=github-hosted')
    if not os.environ.get('RUNNER_NAME'):
        problems.append('no RUNNER_NAME')
    if not os.environ.get('ImageOS', '').lower().startswith('win'):
        problems.append('ImageOS is %r, not win*' % os.environ.get('ImageOS', ''))
    temp = os.environ.get('RUNNER_TEMP', '')
    if not temp or not os.path.isdir(temp):
        problems.append('RUNNER_TEMP %r is not a folder' % temp)
    elif os.path.dirname(os.path.abspath(temp)) == os.path.abspath(temp):
        problems.append('RUNNER_TEMP %r is the root of a drive' % temp)
    elif not is_inside(path, temp):
        problems.append('%s is not under RUNNER_TEMP' % path)
    return problems


class LavapipeDriver:
    """Points the Vulkan loader at lavapipe while the block runs (step 3 of the module notes)."""

    def __init__(self, icd, register_allowed):
        self.icd = icd
        self.register_allowed = register_allowed
        self.mode = None
        self.registered = False     # this run's value is in HKLM: remove it at the end

    def __enter__(self):
        try:
            if not is_elevated():
                self.mode = 'VK_DRIVER_FILES'
            else:
                if not self.register_allowed:
                    raise Stop('elevated: the Vulkan loader ignores VK_DRIVER_FILES here, and this tool writes the '
                               'registry only with --register-icd on a throwaway runner. Run it from a shell that '
                               'is not elevated')
                problems = runner_problems(self.icd)
                if problems:
                    raise Stop('--register-icd, but not on a GitHub-hosted runner: %s' % '; '.join(problems))
                self.mode = 'registry'
                self._register()
            say('Vulkan driver: lavapipe through %s' % self.mode)
        except BaseException:
            self._unregister()
            raise
        return self

    def _register(self):
        import winreg
        access = winreg.KEY_QUERY_VALUE | winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY
        with winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE, VULKAN_DRIVERS_KEY, 0, access) as key:
            names, i = [], 0
            while True:
                try:
                    names.append(winreg.EnumValue(key, i)[0])
                except OSError:
                    break
                i += 1
            # The same name as this run's value: left by a run that was killed (same WORK); it is
            # this run's now, removed at the end. Nothing else there is touched.
            left = [n for n in names if os.path.normcase(n) == os.path.normcase(self.icd)]
            self.registered = True      # set first: a failure below still removes the value
            winreg.SetValueEx(key, self.icd, 0, winreg.REG_DWORD, 0)
        others = [n for n in names if n not in left]
        say('HKLM\\%s: %s %s; other drivers there: %s'
            % (VULKAN_DRIVERS_KEY, self.icd, 'was there from an earlier run (kept, removed at the end)' if left
               else 'added', ', '.join(others) or 'none'))

    def _unregister(self):
        if not self.registered:
            return
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, VULKAN_DRIVERS_KEY, 0,
                                winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as key:
                winreg.DeleteValue(key, self.icd)
            self.registered = False
            say('HKLM\\%s: %s removed' % (VULKAN_DRIVERS_KEY, self.icd))
        except OSError as e:
            say('warning: could not remove %s from HKLM\\%s (%s); the runner is thrown away'
                % (self.icd, VULKAN_DRIVERS_KEY, e))

    def env(self):
        return {'VK_DRIVER_FILES': self.icd} if self.mode == 'VK_DRIVER_FILES' else {}

    def __exit__(self, *exc):
        self._unregister()
        return False


def dropped_env():
    return sorted(k for k in os.environ if k.upper().startswith(DROPPED_ENV_PREFIXES))


def viewer_env(work, name, driver_env):
    """The caller's environment without DROPPED_ENV_PREFIXES, user folders in WORK/env/<name>."""
    dropped = dropped_env()
    env = {k: v for k, v in os.environ.items() if k not in dropped}
    home = os.path.join(work, 'env', name)
    for var, sub in (('APPDATA', 'Roaming'), ('LOCALAPPDATA', 'Local'), ('TEMP', 'Temp'), ('TMP', 'Temp')):
        env[var] = os.path.join(home, sub)
        os.makedirs(env[var], exist_ok=True)
    env['CHARIS_SAFE_MODE_NO_PROMPT'] = '1'     # no safe-mode question after an unclean quit
    env['MESA_SHADER_CACHE_DISABLE'] = 'true'
    env.update(driver_env)
    return env


def host_report():
    """The session and screen the viewer's window gets (runners may run in session 0)."""
    session = ctypes.c_ulong(0)
    ctypes.windll.kernel32.ProcessIdToSessionId(os.getpid(), ctypes.byref(session))
    user32 = ctypes.windll.user32
    say('host: session %d, screen %dx%d, %s CPUs, elevated %s, GitHub-hosted runner %s'
        % (session.value, user32.GetSystemMetrics(0), user32.GetSystemMetrics(1), os.cpu_count(),
           is_elevated(), on_hosted_runner()))


def vulkaninfo(work, env):
    """System32's vulkaninfo --summary (the runtime installs it) with lavapipe as the driver, when it is
    there. Stops when it runs but lists no lavapipe: the bake would not use it."""
    tool = os.path.join(system32(), 'vulkaninfo.exe')
    if not os.path.isfile(tool):
        say('no %s: the viewer\'s report decides' % tool)
        return
    try:
        r = subprocess.run([tool, '--summary'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=300, encoding='utf-8', errors='replace')
    except (OSError, subprocess.SubprocessError) as e:
        say('vulkaninfo did not run (%s): the viewer\'s report decides' % e)
        return
    path = os.path.join(work, 'vulkaninfo-summary.txt')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(r.stdout)
    for line in r.stdout.splitlines():
        if re.search(r'deviceName|deviceType|apiVersion|driverName|driverInfo|Vulkan Instance Version', line):
            say('  ' + line.strip())
    if r.returncode == 0 and EXPECT_GPU not in r.stdout.lower():
        raise Stop('the Vulkan loader lists no lavapipe (%s): see %s' % (tool, path))


# ---- 4. the bake -------------------------------------------------------------

def release_inputs(build, output):
    """What the release is made from, as far as this step can see it, {name: value}: every file
    under the output folder (size, SHA-256), the stamp and the pdb files outside the output folder
    (size, SHA-256; None when missing), and the sizes of the files the runner reads the next
    steps' environment from (GITHUB_ENV, GITHUB_PATH, GITHUB_OUTPUT). Tampered when one of the
    files has more than one hard link (it could be changed through another name)."""
    items = {}

    def add(name, path):
        if not os.path.isfile(path):
            items[name] = None
            return
        st = os.stat(path)
        if st.st_nlink > 1:
            raise Tampered('%s has %d hard links' % (path, st.st_nlink))
        items[name] = (st.st_size, sha256_file(path))

    for top, _, names in os.walk(output):
        for n in names:
            path = os.path.join(top, n)
            add('output/' + os.path.relpath(path, output).replace(os.sep, '/'), path)
    add('build/' + STAMP, os.path.join(build, STAMP))
    cfg = os.path.basename(output)
    for pdb_rel, _ in PDBS:
        path = os.path.join(build, *pdb_rel.replace('<cfg>', cfg).split('/'))
        if not is_inside(path, output):
            add('build/' + pdb_rel, path)
    for var in ('GITHUB_ENV', 'GITHUB_PATH', 'GITHUB_OUTPUT'):
        path = os.environ.get(var)
        if path:
            items['runner/' + var] = os.path.getsize(path) if os.path.isfile(path) else None
    return items


def check_unchanged(build, output, before):
    """Tampered when release_inputs differ from before: added, changed or removed."""
    after = release_inputs(build, output)
    changed = sorted(set(before) ^ set(after) | {p for p in set(before) & set(after) if before[p] != after[p]})
    if changed:
        raise Tampered('what the release is made from changed while Mesa ran (%d: %s): not packed or placed'
                       % (len(changed), ', '.join(changed[:10])))
    say('unchanged by the bake: %d files of the output folder, the stamp, the pdb files, the runner\'s files'
        % sum(1 for k in after if k.startswith('output/')))


def read_report(path):
    """report-<vendor>.txt as {key: value}, the env= lines as a list under 'env'."""
    fields = {'env': []}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            key, sep, value = line.rstrip('\r\n').partition('=')
            if not sep:
                continue
            if key == 'env':
                fields['env'].append(value)
            else:
                fields.setdefault(key, value)
    return fields


def start_level(log):
    """The graphics level the viewer started with (LLFeatureManager's "Applying Recommended Features
    for level N"); '?' when the log does not say. Stop when it says two different levels."""
    levels = []
    try:
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.search(r'Applying Recommended Features for level (\d+)', line)
                if m:
                    levels.append(m.group(1))
    except OSError:
        pass
    if len(set(levels)) > 1:
        raise Stop('%s: the viewer applied more than one recommended level (%s)' % (log, ', '.join(levels)))
    return levels[0] if levels else '?'


def check_report(path, vendor, expect_caps):
    """The report's fields; Stop unless it is vendor's and the bake finished on lavapipe with every
    step made."""
    if not os.path.isfile(path):
        raise Stop('no %s: the viewer stopped before its login screen (its log is next to it)' % path)
    r = read_report(path)
    name = os.path.basename(path)
    if r.get('vendors') != vendor:
        raise Stop('%s: vendors=%s, not %s' % (name, r.get('vendors', '(none)'), vendor))
    if r.get('status') != 'ok':
        raise Stop('%s: status=%s, error=%s' % (name, r.get('status', '(none)'), r.get('error', '')))
    if EXPECT_GPU not in r.get('gpu', '').lower():
        raise Stop('%s: baked on %r, not lavapipe' % (name, r.get('gpu', '')))
    if r.get('failed_steps') != '0':
        raise Stop('%s: failed_steps=%s (passes lavapipe could not set up: their keys would be missing)'
                   % (name, r.get('failed_steps', '(none)')))
    if r['env']:
        raise Stop('%s: the viewer saw %s' % (name, ', '.join(r['env'])))
    if expect_caps is not None and r.get('caps') != expect_caps:
        raise Stop('%s: caps=%s, not %s' % (name, r.get('caps', '(none)'), expect_caps))
    log = os.path.join(os.path.dirname(path), 'viewer-%s.log' % vendor)
    r['start_level'] = start_level(log)
    if r['start_level'] != EXPECT_START_LEVEL:
        raise Stop('%s: the viewer started at graphics level %s, not %s (%s): the bake\'s sets would differ '
                   '(START_CLASS_ARGS)' % (name, r['start_level'], EXPECT_START_LEVEL, log))
    return r


class _IoCounters(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                                                   'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]


class _BasicLimits(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                ('LimitFlags', ctypes.c_ulong), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', ctypes.c_ulong),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', ctypes.c_ulong), ('SchedulingClass', ctypes.c_ulong)]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [('BasicLimitInformation', _BasicLimits), ('IoInfo', _IoCounters),
                ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]


JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
PROCESS_SET_QUOTA_AND_TERMINATE = 0x0100 | 0x0001


def run_in_job(cmd, cwd, env, timeout_s):
    """Runs cmd in a Windows job object and returns its exit status: when it exits (or runs out
    of time: TimeoutExpired), every process it started is ended too (the job is closed with
    KILL_ON_JOB_CLOSE), so nothing of it is left running when the build is checked and packed."""
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateJobObjectW.restype = ctypes.c_void_p
    kernel32.OpenProcess.restype = ctypes.c_void_p
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        raise OSError('CreateJobObject failed (%d)' % ctypes.get_last_error())
    try:
        limits = _ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel32.SetInformationJobObject(ctypes.c_void_p(job), JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                                                ctypes.byref(limits), ctypes.sizeof(limits)):
            raise OSError('SetInformationJobObject failed (%d)' % ctypes.get_last_error())
        proc = subprocess.Popen(cmd, cwd=cwd, env=env)
        handle = kernel32.OpenProcess(PROCESS_SET_QUOTA_AND_TERMINATE, False, proc.pid)
        assigned = bool(handle) and kernel32.AssignProcessToJobObject(ctypes.c_void_p(job), ctypes.c_void_p(handle))
        if handle:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
        if not assigned:
            proc.kill()
            proc.wait()
            raise OSError('the viewer could not be put in a job object (%d)' % ctypes.get_last_error())
        try:
            return proc.wait(timeout=timeout_s)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            raise
    finally:
        kernel32.TerminateJobObject(ctypes.c_void_p(job), 1)    # what the viewer started and left running
        kernel32.CloseHandle(ctypes.c_void_p(job))


def bake(exe, output, out, vendor, env, timeout_s, expect_caps):
    report = os.path.join(out, 'report-%s.txt' % vendor)
    if os.path.exists(report):
        os.remove(report)
    log = os.path.join(out, 'viewer-%s.log' % vendor)
    say('%s: baking (log %s)' % (vendor, log))
    started = time.monotonic()
    try:
        code = run_in_job([exe, '--bake-shaders', out, '--bake-shaders-vendors', vendor, '--logfile', log]
                          + START_CLASS_ARGS, output, env, timeout_s)
    except subprocess.TimeoutExpired:
        raise Stop('%s: the viewer ran longer than %d minutes (stopped)' % (vendor, timeout_s // 60))
    except OSError as e:
        raise Stop('%s: the viewer did not start: %s' % (vendor, e))
    if code:
        say('%s: the viewer exited with %d (its report decides)' % (vendor, code))
    r = check_report(report, vendor, expect_caps)
    say('%s: %s sets, %s builds (%s compiled), %s OIT variants, %s passes, %s s in the bake, %d s in all; gpu %s; '
        'start-up level %s' % (vendor, r.get('sets'), r.get('builds'), r.get('compiled'), r.get('oit_variants'),
                               r.get('fixed_passes'), r.get('seconds'), time.monotonic() - started, r.get('gpu'),
                               r['start_level']))
    return r


def bake_all(a, work, output, exe):
    """Steps 2 to 4: the out folder and the reports by vendor."""
    host_report()
    ensure_vulkan_runtime(work, a.install_vulkan_runtime)
    if dropped_env():
        say('left out of the viewer environment: %s' % ', '.join(dropped_env()))
    icd = fetch_mesa(work, a.mesa_archive, a.seven_zip)
    out = os.path.join(work, 'out')
    os.makedirs(out, exist_ok=True)
    build = os.path.abspath(a.build_dir)
    before = release_inputs(build, output)      # before anything of Mesa runs
    reports = {}
    with LavapipeDriver(icd, a.register_icd) as driver:
        vulkaninfo(work, viewer_env(work, 'vulkaninfo', driver.env()))
        for vendor in a.vendor_list:
            env = viewer_env(work, vendor, driver.env())
            reports[vendor] = bake(exe, output, out, vendor, env, a.timeout_minutes * 60, a.expect_caps)
        mode = driver.mode
    check_unchanged(build, output, before)
    return [out], reports, mode


def reports_of(out_dirs, vendors, expect_caps):
    """--pack-only: the reports of the --bake-only runs, checked again; together exactly vendors,
    each once."""
    reports = {}
    for out in out_dirs:
        names = sorted(n for n in os.listdir(out) if re.fullmatch(r'report-[a-z-]+\.txt', n))
        if not names:
            raise Stop('%s has no report-<vendor>.txt (not the out folder of a --bake-only run)' % out)
        for name in names:
            vendor = name[len('report-'):-len('.txt')]
            if vendor in reports:
                raise Stop('%s: a second report for %s' % (out, vendor))
            reports[vendor] = check_report(os.path.join(out, name), vendor, expect_caps)
    if sorted(reports) != sorted(vendors):
        raise Stop('the reports are for %s, not %s (--vendors)' % (', '.join(sorted(reports)), ', '.join(vendors)))
    return reports


# ---- 5, 6. the bundle --------------------------------------------------------

def pack_bundle(cache_dirs, work, commit, baseline):
    """WORK/shaders.vkb and WORK/bundle-manifest.tsv; (bundle, header, baseline result)."""
    bundle = os.path.join(work, 'shaders.vkb')
    started = time.monotonic()
    summary = shaderbundle.pack(cache_dirs, bundle, commit)
    if summary['left_out']:
        raise Stop('cache files the bundle would leave out: %s' % ', '.join(
            '%s %d' % kv for kv in sorted(summary['left_out'].items())))
    header, programs, views = shaderbundle.read_bundle(bundle)
    if header['commit'] != commit or not header['programs'] or not header['views']:
        raise Stop('the bundle read back is not right: %s' % shaderbundle.describe(header))
    manifest = os.path.join(work, 'bundle-manifest.tsv')
    with open(manifest, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(shaderbundle.manifest_lines(header, programs, views)) + '\n')
    say('bundle (%d s): %s' % (time.monotonic() - started, shaderbundle.describe(header).replace('\n', '; ')))
    result = 'none'
    if baseline:
        ok, counts, differ = shaderbundle.compare_manifests(manifest, baseline)
        result = ', '.join('%s %d' % kv for kv in sorted(counts.items()))
        say('against %s: %s' % (baseline, result))
        if not ok:
            raise Stop('the bundle differs from the baseline (%s%s)' % (result, ': ' + ', '.join(differ[:5]) if differ else ''))
        result = 'OK: ' + result
    return bundle, header, result


def mesa_files_in(folder):
    found = []
    for top, _, names in os.walk(folder):
        found += [os.path.join(top, n) for n in names if n.lower() in MESA_FILE_NAMES]
    return found


def place(bundle, target):
    os.makedirs(os.path.dirname(target), exist_ok=True)
    temp = target + '.tmp'
    try:
        shutil.copyfile(bundle, temp)
        os.replace(temp, target)
    finally:
        if os.path.exists(temp):
            os.remove(temp)
    say('placed %s' % target)


def write_record(work, commit, mode, reports, target, header, baseline, seconds):
    """WORK/bake-record.txt (key=value lines), also printed."""
    fields = [('commit', commit), ('mesa', '%s %s sha256 %s' % (MESA_VERSION, MESA_ARCHIVE, MESA_SHA256)),
              ('driver', mode), ('vendors', ','.join(sorted(reports)))]
    for vendor, r in sorted(reports.items()):
        fields.append(('report_' + vendor, 'gpu %s; start-up level %s; sets %s; builds %s; compiled %s; '
                       'failed_steps %s; seconds %s; caps %s'
                       % (r.get('gpu'), r['start_level'], r.get('sets'), r.get('builds'), r.get('compiled'),
                          r.get('failed_steps'), r.get('seconds'), r.get('caps', '(none)'))))
    fields += [('bundle', target), ('bundle_sha256', header['sha256']), ('bundle_bytes', header['bytes']),
               ('programs', header['programs']), ('views', header['views']), ('modules', header['modules']),
               ('glslang', header['glslang']), ('manifest', os.path.join(work, 'bundle-manifest.tsv')),
               ('baseline', baseline), ('minutes', '%.1f' % (seconds / 60))]
    path = os.path.join(work, 'bake-record.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.writelines('%s=%s\n' % kv for kv in fields)
    say('record %s:' % path)
    for kv in fields:
        print('  %s=%s' % kv, flush=True)


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--vendors', default=','.join(VENDORS))
    ap.add_argument('--work')
    ap.add_argument('--mesa-archive')
    ap.add_argument('--seven-zip', default=default_seven_zip())
    steps = ap.add_mutually_exclusive_group()
    steps.add_argument('--bake-only', action='store_true')
    steps.add_argument('--pack-only', nargs='+', metavar='DIR')
    ap.add_argument('--baseline')
    ap.add_argument('--expect-caps')
    ap.add_argument('--bundle-out')
    ap.add_argument('--timeout-minutes', type=int, default=60)
    ap.add_argument('--register-icd', action='store_true')
    ap.add_argument('--install-vulkan-runtime', action='store_true')
    ap.add_argument('--allow-dev-build', action='store_true')
    ap.add_argument('--commit')
    a = ap.parse_args()
    started = time.monotonic()
    try:
        if not 1 <= a.timeout_minutes <= 300:
            raise Stop('--timeout-minutes must be 1 to 300')
        a.vendor_list = parse_vendors(a.vendors)
        work = a.work or (os.path.join(os.environ['RUNNER_TEMP'], 'charis-bake') if os.environ.get('RUNNER_TEMP') else None)
        if not work:
            raise Stop('--work is needed outside a GitHub runner')
        inputs = [('--pack-only', d) for d in a.pack_only or []] + \
            [(opt, p) for opt, p in (('--mesa-archive', a.mesa_archive), ('--baseline', a.baseline)) if p]
        inside = ['%s %s' % (opt, p) for opt, p in inputs if is_inside(p, work)]
        if inside:
            raise Stop('inputs inside --work (they could be removed there): %s' % ', '.join(inside))
        output, exe, commit = check_build(a)
        work = prepare_work(work)
        if a.pack_only:
            out_dirs = [os.path.abspath(d) for d in a.pack_only]
            reports, mode = reports_of(out_dirs, a.vendor_list, a.expect_caps), 'none (packed only)'
        else:
            out_dirs, reports, mode = bake_all(a, work, output, exe)
            if a.bake_only:
                say('baked: %s (pack it with --pack-only)' % out_dirs[0])
                return 0
        bundle, header, baseline = pack_bundle([os.path.join(d, 'cache') for d in out_dirs], work, commit, a.baseline)
        leaked = mesa_files_in(output)
        if leaked:
            raise Tampered('Mesa files in the build\'s output folder (they must not be released): %s' % ', '.join(leaked))
        target = os.path.abspath(a.bundle_out or os.path.join(output, BUNDLE_REL))
        write_record(work, commit, mode, reports, target, header, baseline, time.monotonic() - started)
        place(bundle, target)
    except (Stop, shaderbundle.Stop, OSError) as e:
        tampered = isinstance(e, Tampered)
        say('%s: %s' % ('TAMPERED' if tampered else 'STOP', e))
        if in_ci():
            print('::error::bake-shaders-ci: %s' % str(e).replace('\n', ' '), flush=True)
        return TAMPERED_EXIT if tampered else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
