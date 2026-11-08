#!/usr/bin/env python3
"""Hands a checked release build from the build job of the release workflow to
its bake and package jobs (release-package.yml). The build folder (about 9 GB)
cannot be handed over whole; this is the part of it that the bake
(bake-shaders-ci.py) and the packaging (make-release-zip.py,
velopack-release.py, the build record) read. The bake runs Mesa, a third-party
driver, in a job of its own; the package job takes this tar again, checked by
the SHA-256 the build job gave as a job output, so nothing the bake job did can
change what is packed.

  python -I job-handoff.py pack --build-dir DIR --out TAR [--vulkan-sdk SDK]
  python -I job-handoff.py unpack TAR --sha256 HEX --into DIR

pack    one uncompressed tar (the artifact upload compresses it) of, under build/:
        the stamp (charis-release-build.txt with check=OK, use_tracy=OFF,
        release_for_download=ON, and the exe's SHA-256), the files of the packaging
        step's list (secondlife_setup_tmp.nsi, without the developer-only ones; a list
        that names a shader bundle stops: the bundle comes from the bake job), the list itself,
        velopack-pack.json and the icon its --icon names (as VELOPACK_ICON_REL; its
        arguments are checked here: velopack_items), and what the build record quotes
        (CMakeFiles/*/CMakeCXXCompiler.cmake, packages/installed-packages.xml).
        Two names that differ only in case are taken once when they are one file.
        No pdb (the symbols zip is made in the build job). With --vulkan-sdk, the
        SDK's Helpers/VulkanRT.exe under vulkan-sdk/ (for a runner without the Vulkan
        runtime; bake-shaders-ci.py checks it by size and SHA-256 before running it).
        Regular files only; stops on a link, a listed file that is missing or outside
        the output folder, or a Mesa file name. Modification times are kept (the
        packaging compares the exe's with the list's and the bundle's).
unpack  checks TAR's SHA-256 before reading anything in it, then unpacks it into DIR
        (new or empty): only regular files and folders under build/ or vulkan-sdk/,
        plain relative names (checked_rel: also no backslash, no part ending in a dot
        or space, no device name), none twice (ignoring case), each inside DIR once
        resolved, no Mesa file name; and tarfile's "data" filter when the Python has
        it. The build folder is then DIR/build.

Exit status: 0 = done, 1 = stopped (the reason is printed), 2 = wrong arguments.
No network.
"""
import argparse
import glob
import json
import os
import re
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (BUNDLE_FILES, FILE_LIST, MESA_FILE_NAMES, OUTPUT_REL, STAMP,  # noqa: E402
                           VELOPACK_ICON_REL, VELOPACK_PACK_JSON, VPK_ADDED_ARGS, FileListError, checked_rel,
                           read_file_list, read_stamp, sha256_file, stamp_problems, viewer_exe, vpk_arg_value)

BUILD_TOP = 'build'
SDK_TOP = 'vulkan-sdk'
VULKAN_RT_REL = 'Helpers/VulkanRT.exe'
# What the build record quotes (release-package.yml, "Source zip and build record"), in the build folder.
RECORD_GLOBS = ('CMakeFiles/*/CMakeCXXCompiler.cmake', 'packages/installed-packages.xml')


class Stop(Exception):
    pass


def say(text):
    print('job-handoff: ' + text, flush=True)


def regular_file(path, what):
    """path when it is a regular file that is not a link; Stop otherwise."""
    if os.path.islink(path) or not os.path.isfile(path):
        raise Stop('%s: %s is not a regular file' % (what, path))
    return path


def mesa_names(names):
    return sorted(n for n in names if n.rsplit('/', 1)[-1].lower() in MESA_FILE_NAMES)


def velopack_items(output, files):
    """[(source path, name under the build folder)] for vpk pack in the package job: velopack-pack.json
    and the icon its --icon names (VELOPACK_ICON_REL; velopack-release.py --icon takes it there).
    Stops now, not hours later in the package job, when the arguments name a path that job would
    not have: --mainExe not a listed file, --icon not once or not a file, an argument
    velopack-release.py adds itself, or any other absolute path."""
    path = os.path.join(output, VELOPACK_PACK_JSON)
    try:
        with open(path, encoding='utf-8') as f:
            args = json.load(f).get('vpkArgs')
    except (OSError, ValueError, AttributeError) as e:
        raise Stop('cannot read %s (the stamp says use_velopack=ON): %s' % (path, e))
    if not isinstance(args, list) or not args or args[0] != 'pack' or not all(isinstance(x, str) for x in args):
        raise Stop('%s: vpkArgs is not ["pack", ...]' % path)
    added = [x for x in VPK_ADDED_ARGS if x in args]
    if added:
        raise Stop('%s: vpkArgs has %s (velopack-release.py adds it)' % (path, ', '.join(added)))
    main_exe = vpk_arg_value(args, '--mainExe', path)
    if main_exe not in files:
        raise Stop('%s: --mainExe %r is not a listed file' % (path, main_exe))
    if args.count('--icon') != 1:
        raise Stop('%s: vpkArgs does not have --icon once' % path)
    icon = vpk_arg_value(args, '--icon', path)
    regular_file(icon, '%s --icon' % VELOPACK_PACK_JSON)
    paths = [x for i, x in enumerate(args) if os.path.isabs(x) and args[i - 1] != '--icon']
    if paths:
        raise Stop('%s: vpkArgs names %s, which the package job would not have' % (path, ', '.join(paths)))
    return [(path, OUTPUT_REL.replace(os.sep, '/') + '/' + VELOPACK_PACK_JSON), (icon, VELOPACK_ICON_REL)]


def once_ignoring_case(items):
    """items without the later of two names that differ only in case when they are the same file
    (the list spells a folder two ways, as LICENSES/ and licenses/ on a Windows disk); Stop when two
    such names are different files (unpack would refuse the tar)."""
    kept, seen = [], {}
    for src, dst in items:
        first = seen.get(dst.lower())
        if first is None:
            seen[dst.lower()] = (src, dst)
            kept.append((src, dst))
        elif not os.path.samefile(first[0], src):
            raise Stop('%s and %s differ only in case but are different files' % (first[1], dst))
    if len(kept) != len(items):
        say('%d names spelled a second way (same file, ignoring case) taken once' % (len(items) - len(kept)))
    return kept


def handoff_items(build, vulkan_sdk):
    """[(source path, name in the tar)] of pack."""
    output = os.path.join(build, OUTPUT_REL)
    stamp = read_stamp(build)
    wrong = stamp_problems(stamp)
    if wrong:
        raise Stop('%s does not say check=OK, use_tracy=OFF, release_for_download=ON (%s)'
                   % (os.path.join(build, STAMP), ', '.join(wrong)))
    exe = viewer_exe(output)
    if not exe:
        raise Stop('no viewer exe in %s' % output)
    if stamp.get('exe_sha256') != sha256_file(exe):
        raise Stop('%s changed after the release check (its SHA-256 is not the stamp\'s)' % exe)
    files = read_file_list(output, exe)
    early = sorted(files & set(BUNDLE_FILES))
    if early:
        raise Stop('%s names %s: a shader bundle was in the build before the packaging step; the release '
                   'takes the bake job\'s only (build in a clean folder)' % (FILE_LIST, ', '.join(early)))
    root = os.path.realpath(output) + os.sep
    out_rel = OUTPUT_REL.replace(os.sep, '/')
    items = [(os.path.join(build, STAMP), STAMP)]
    for rel in sorted(files | {FILE_LIST}):
        path = os.path.join(output, *rel.split('/'))
        regular_file(path, FILE_LIST)
        if not os.path.realpath(path).startswith(root):
            raise Stop('%s resolves outside the output folder' % rel)
        items.append((path, out_rel + '/' + rel))
    if stamp.get('use_velopack') == 'ON':
        items += velopack_items(output, files)
    for pattern in RECORD_GLOBS:
        for path in sorted(glob.glob(os.path.join(build, *pattern.split('/')))):
            items.append((path, os.path.relpath(path, build).replace(os.sep, '/')))
    items = once_ignoring_case([(src, BUILD_TOP + '/' + dst) for src, dst in items])
    if vulkan_sdk:
        rt = os.path.join(vulkan_sdk, *VULKAN_RT_REL.split('/'))
        if os.path.isfile(rt):
            items.append((rt, SDK_TOP + '/' + VULKAN_RT_REL))
        else:
            say('no %s: the bake job has no Vulkan runtime installer to fall back on' % rt)
    leaked = mesa_names(dst for _, dst in items)
    if leaked:
        raise Stop('Mesa files in the build (they must not be released): %s' % ', '.join(leaked))
    for src, dst in items:
        regular_file(src, dst)
    return items


def pack(a):
    build = os.path.abspath(a.build_dir)
    out = os.path.abspath(a.out)
    if os.path.lexists(out):
        raise Stop('exists: %s' % out)
    items = handoff_items(build, a.vulkan_sdk)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    part = out + '.part'
    total = 0
    try:
        with tarfile.open(part, 'w', format=tarfile.PAX_FORMAT) as tar:
            for src, dst in items:
                info = tar.gettarinfo(src, dst)
                info.uid = info.gid = 0
                info.uname = info.gname = ''
                with open(src, 'rb') as f:
                    tar.addfile(info, f)
                total += info.size
        os.replace(part, out)
    finally:
        if os.path.exists(part):
            os.remove(part)
    say('%s: %d files, %.0f MiB, sha256 %s' % (out, len(items), total / 2**20, sha256_file(out)))
    return 0


def checked_members(tar):
    """The members of the tar, refused unless each is a regular file or a folder with a plain
    relative name under BUILD_TOP or SDK_TOP, once (ignoring case)."""
    members, seen = [], set()
    for m in tar.getmembers():
        try:
            name = checked_rel(m.name.rstrip('/') if m.isdir() else m.name, 'the tar')
        except FileListError as e:
            raise Stop(str(e))
        if not (m.isfile() or m.isdir()):
            raise Stop('the tar has %s, which is not a regular file or a folder' % name)
        if name.split('/')[0] not in (BUILD_TOP, SDK_TOP):
            raise Stop('the tar has %s, outside %s/ and %s/' % (name, BUILD_TOP, SDK_TOP))
        if name.lower() in seen:
            raise Stop('the tar has %s twice' % name)
        seen.add(name.lower())
        members.append(m)
    leaked = mesa_names(m.name for m in members if m.isfile())
    if leaked:
        raise Stop('Mesa files in the tar: %s' % ', '.join(leaked))
    if not any(m.isfile() and m.name == BUILD_TOP + '/' + STAMP for m in members):
        raise Stop('the tar has no %s/%s' % (BUILD_TOP, STAMP))
    return members


def unpack(a):
    path = os.path.abspath(a.tar)
    into = os.path.abspath(a.into)
    if not re.fullmatch(r'[0-9a-f]{64}', a.sha256 or ''):
        raise Stop('--sha256 is not 64 lower-case hex digits')
    regular_file(path, 'the tar')
    got = sha256_file(path)
    if got != a.sha256:
        raise Stop('%s has sha256 %s, not %s (not the tar the build job made)' % (path, got, a.sha256))
    if os.path.lexists(into) and (not os.path.isdir(into) or os.listdir(into)):
        raise Stop('--into %s is not a new or empty folder' % into)
    # The "data" filter when this Python has it (3.12 in the workflow). Without it, what keeps
    # every file inside DIR is checked_members (no backslash, '..', drive or device name; regular
    # files and folders only) and the resolved path of each member, checked here.
    extract_args = {'filter': 'data'} if hasattr(tarfile, 'data_filter') else {}
    os.makedirs(into, exist_ok=True)
    root = os.path.realpath(into) + os.sep
    with tarfile.open(path, 'r:') as tar:
        members = checked_members(tar)
        outside = [m.name for m in members
                   if not os.path.realpath(os.path.join(into, *m.name.rstrip('/').split('/'))).startswith(root)]
        if outside:
            raise Stop('the tar has names that resolve outside %s: %s' % (into, ', '.join(outside[:5])))
        tar.extractall(into, members=members, **extract_args)
    files = sum(1 for m in members if m.isfile())
    say('%s (sha256 %s): %d files into %s' % (path, got, files, into))
    return 0


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('pack')
    p.add_argument('--build-dir', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--vulkan-sdk')
    p = sub.add_parser('unpack')
    p.add_argument('tar')
    p.add_argument('--sha256', required=True)
    p.add_argument('--into', required=True)
    a = ap.parse_args()
    try:
        return pack(a) if a.command == 'pack' else unpack(a)
    except (Stop, ValueError, OSError, tarfile.TarError) as e:
        say('STOPPED: %s' % e)
        if os.environ.get('GITHUB_ACTIONS') == 'true':
            print('::error::job-handoff: %s' % str(e).replace('\n', ' '), flush=True)
        return 1


if __name__ == '__main__':
    sys.exit(main())
