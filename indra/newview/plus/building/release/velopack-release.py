#!/usr/bin/env python3
"""Makes the installer (Velopack's Setup.exe) and the update packages of a
Charis Viewer release from the folder make-release-zip.py staged, and writes
the update feed with the addresses of the release's assets.

  python -I velopack-release.py --build-dir DIR --stage STAGE --out OUT --vpk VPK --tag TAG [--icon ICO]

  --build-dir DIR  the build folder (USE_VELOPACK on): its velopack-pack.json
                   holds the arguments for vpk pack (viewer_manifest.py)
  --stage STAGE    the files of the zip (make-release-zip.py --stage): vpk pack's
                   --packDir. Never the build folder
  --out OUT        where the release assets go (the folder make-release-zip.py
                   wrote the zips to)
  --vpk VPK        the vpk program
  --tag TAG        the release tag (v0.1.0-alpha1): the version in the Setup.exe's
                   name, and the folder of the feed's addresses
  --icon ICO       the Setup.exe's icon instead of the file velopack-pack.json's
                   --icon names (that one is in the source tree of the build: the
                   release workflow's package job passes the copy job-handoff.py
                   handed over). Without it, that file must be there

Writes to OUT:
  CharisViewer-<version>-win64-Setup.exe  vpk's <packId>-win-Setup.exe, renamed
  <packId>-<packVersion>-full.nupkg       the update package (named as vpk names it)
  releases.win.json                       the update feed: each FileName becomes
      <PROJECT_URL>/releases/download/<tag>/<FileName> (llvelopack.cpp takes no
      other address); the Size and SHA256 of each package are checked against the
      file first
The package's application files (lib/app/) are checked against the staged
files. vpk's other outputs (RELEASES, assets.win.json) are left out; a Portable.zip
stops it (--noPortable is in the arguments). No delta packages: no earlier
release is put in vpk's output folder (the viewer's downloader does not apply
deltas yet).
Exit status: 0 = made, 1 = stopped (the reason is printed), 2 = wrong arguments.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from urllib.parse import unquote

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (OUTPUT_REL, SOURCE, VELOPACK_PACK_JSON, VPK_ADDED_ARGS, independence_constant,  # noqa: E402
                           read_stamp, sha256_file, tag_version, vpk_arg_value)
FEED = 'releases.win.json'        # Velopack's feed for its default Windows channel, "win"
# A package name llvelopack.cpp is_package_file_name() accepts: "<packId>-...nupkg" of
# letters, digits and ".-_+", no "..", at most 200 characters.
MAX_PACKAGE_NAME = 200
PACKAGE_CHARS = re.compile(r'[A-Za-z0-9._+-]+')
LEFT_OUT = ('RELEASES', 'assets.win.json')
# In a package, the application's files are under lib/app/; Velopack adds its own
# there (the version marker) [assumed from Velopack's layout: check on the first run].
PACKAGE_APP_DIR = 'lib/app/'
VELOPACK_OWN_FILES = {'sq.version'}


class Stop(Exception):
    pass


def say(text):
    print('velopack-release: ' + text, flush=True)


def is_package_name(name, pack_id):
    return (name.startswith(pack_id + '-') and name.endswith('.nupkg') and len(name) <= MAX_PACKAGE_NAME
            and len(name) > len(pack_id) + 1 + len('.nupkg') and '..' not in name
            and PACKAGE_CHARS.fullmatch(name) is not None)


def read_pack_args(build, pack_id):
    """vpkArgs of velopack-pack.json, checked: vpk pack for pack_id, without the two
    arguments added here. Returns (args, packVersion, the --exclude pattern or None)."""
    path = os.path.join(build, OUTPUT_REL, VELOPACK_PACK_JSON)
    try:
        with open(path, encoding='utf-8') as f:
            pack = json.load(f)
    except (OSError, ValueError) as e:
        raise Stop('cannot read %s (USE_VELOPACK on?): %s' % (path, e))
    args = pack.get('vpkArgs')
    if not isinstance(args, list) or not args or args[0] != 'pack' or not all(isinstance(x, str) for x in args):
        raise Stop('%s: vpkArgs is not ["pack", ...]' % path)
    if pack.get('packId') != pack_id or args.count('--packId') != 1 or vpk_arg_value(args, '--packId', path) != pack_id:
        raise Stop('%s: the package id is not %s (plus/independence/independence.h)' % (path, pack_id))
    for added in VPK_ADDED_ARGS:
        if added in args:
            raise Stop('%s: vpkArgs already has %s' % (path, added))
    if '--noPortable' not in args:
        raise Stop('%s: vpkArgs has no --noPortable' % path)
    version = pack.get('packVersion')
    if not isinstance(version, str) or not version:
        raise Stop('%s: no packVersion' % path)
    pattern = vpk_arg_value(args, '--exclude', path)
    try:
        exclude = re.compile(pattern) if pattern is not None else None
    except re.error as e:
        raise Stop('%s: the --exclude pattern %r is not a regular expression: %s' % (path, pattern, e))
    return args, version, exclude


def with_icon(args, icon, where):
    """args with --icon's value replaced by icon (when given); Stop unless args name the icon once
    and the icon used is a regular file."""
    if args.count('--icon') != 1:
        raise Stop('%s: vpkArgs does not have --icon once' % where)
    args = list(args)
    i = args.index('--icon')
    if i + 1 >= len(args):
        raise Stop('%s: vpkArgs ends with --icon, without its value' % where)
    if icon:
        args[i + 1] = os.path.abspath(icon)
    if os.path.islink(args[i + 1]) or not os.path.isfile(args[i + 1]):
        raise Stop('no icon for the Setup.exe at %s%s' % (args[i + 1], '' if icon else ' (pass --icon)'))
    return args


def run_vpk(vpk, args, stage, vpk_out):
    cmd = [vpk] + args + ['--packDir', stage, '--outputDir', vpk_out]
    say('$ ' + ' '.join('"%s"' % c if (' ' in c or not c) else c for c in cmd))
    try:
        subprocess.run(cmd, check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        raise Stop('vpk pack failed: %s' % e)


def rewrite_feed(vpk_out, pack_id, pack_version, base_url):
    """The feed with absolute FileNames under base_url; returns (feed, package names)."""
    path = os.path.join(vpk_out, FEED)
    try:
        with open(path, encoding='utf-8-sig') as f:
            feed = json.load(f)
    except (OSError, ValueError) as e:
        raise Stop('cannot read vpk\'s %s: %s' % (FEED, e))
    assets = feed.get('Assets') if isinstance(feed, dict) else None
    if not isinstance(assets, list) or not assets:
        raise Stop('%s has no Assets' % FEED)
    names = []
    for asset in assets:
        name = asset.get('FileName') if isinstance(asset, dict) else None
        if not isinstance(name, str) or not is_package_name(name, pack_id):
            raise Stop('%s: FileName %r is not a %s-*.nupkg name' % (FEED, name, pack_id))
        if asset.get('PackageId') != pack_id or asset.get('Version') != pack_version:
            raise Stop('%s: %s is %s %s, not %s %s (only this release\'s packages belong in the feed)'
                       % (FEED, name, asset.get('PackageId'), asset.get('Version'), pack_id, pack_version))
        file_path = os.path.join(vpk_out, name)
        if not os.path.isfile(file_path):
            raise Stop('%s names %s, which vpk did not write' % (FEED, name))
        size, digest = asset.get('Size'), asset.get('SHA256')
        if not isinstance(size, int) or size != os.path.getsize(file_path):
            raise Stop('%s: Size of %s is %r, the file has %d bytes' % (FEED, name, size, os.path.getsize(file_path)))
        # The viewer takes only packages with a SHA-256 (no SHA-1 fallback).
        if not isinstance(digest, str) or digest.lower() != sha256_file(file_path):
            raise Stop('%s: SHA256 of %s is %r, not the file\'s' % (FEED, name, digest))
        asset['FileName'] = '%s/%s' % (base_url, name)
        names.append(name)
    return feed, names


def staged_files(stage):
    """The files under stage as paths with /."""
    names = set()
    for top, _, files in os.walk(stage):
        for name in files:
            names.add(os.path.relpath(os.path.join(top, name), stage).replace(os.sep, '/'))
    return names


def check_package_files(nupkg, stage, exclude):
    """The application files in the package (lib/app/) are the staged files: none missing
    (except what vpk's --exclude leaves out) and none added (except Velopack's own)."""
    try:
        with zipfile.ZipFile(nupkg) as z:
            # NuGet packages may percent-encode names (OPC); decoding a plain name changes nothing
            packed = set(unquote(n[len(PACKAGE_APP_DIR):]) for n in z.namelist()
                         if n.startswith(PACKAGE_APP_DIR) and not n.endswith('/'))
    except (OSError, zipfile.BadZipFile) as e:
        raise Stop('cannot read %s: %s' % (os.path.basename(nupkg), e))
    staged = staged_files(stage)
    missing = sorted(n for n in staged - packed if not (exclude and exclude.fullmatch(n.rsplit('/', 1)[-1])))
    added = sorted(n for n in packed - staged if n not in VELOPACK_OWN_FILES)
    if missing or added:
        raise Stop('%s does not hold the staged files: missing %s; added %s'
                   % (os.path.basename(nupkg), missing[:10] or 'none', added[:10] or 'none'))
    say('%s: %d application files, the staged ones' % (os.path.basename(nupkg), len(packed)))


def make(a):
    pack_id = independence_constant(SOURCE, 'VELOPACK_PACK_ID')
    project_url = independence_constant(SOURCE, 'PROJECT_URL')
    if not pack_id or not project_url:
        raise Stop('VELOPACK_PACK_ID or PROJECT_URL is not once in plus/independence/independence.h')
    version = tag_version(a.tag, SOURCE)
    build = os.path.abspath(a.build_dir)
    stamp = read_stamp(build)
    if stamp.get('check') != 'OK' or stamp.get('use_velopack') != 'ON':
        raise Stop('the build\'s charis-release-build.txt does not say check=OK and use_velopack=ON')
    stage, out = os.path.abspath(a.stage), os.path.abspath(a.out)
    n_stage, n_build = os.path.normcase(stage), os.path.normcase(build)
    if n_stage == n_build or n_stage.startswith(n_build + os.sep) or n_build.startswith(n_stage + os.sep):
        raise Stop('--stage is the build folder, in it or above it: vpk packs the staged zip files, '
                   'never the build folder')
    if not os.path.isdir(stage) or not os.listdir(stage):
        raise Stop('--stage %s is missing or empty (make-release-zip.py --stage)' % stage)
    args, pack_version, exclude = read_pack_args(build, pack_id)
    args = with_icon(args, a.icon, os.path.join(build, OUTPUT_REL, VELOPACK_PACK_JSON))
    setup_name = 'CharisViewer-%s-win64-Setup.exe' % version
    base_url = '%s/releases/download/%s' % (project_url, a.tag)

    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as vpk_out:
        run_vpk(a.vpk, args, stage, vpk_out)
        produced = sorted(os.listdir(vpk_out))
        say('vpk wrote: %s' % ', '.join(produced))
        if any('portable' in n.lower() for n in produced):
            raise Stop('vpk made a portable zip, which is not released (--noPortable)')
        setup = os.path.join(vpk_out, '%s-win-Setup.exe' % pack_id)
        if not os.path.isfile(setup):
            raise Stop('vpk wrote no %s-win-Setup.exe' % pack_id)
        feed, packages = rewrite_feed(vpk_out, pack_id, pack_version, base_url)
        for name in packages:
            check_package_files(os.path.join(vpk_out, name), stage, exclude)
        unknown = [n for n in produced if n not in packages + [FEED, os.path.basename(setup)] + list(LEFT_OUT)]
        if unknown:
            raise Stop('vpk wrote files this script does not know: %s' % ', '.join(unknown))
        targets = [setup_name, FEED] + packages
        clash = [n for n in targets if os.path.exists(os.path.join(out, n))]
        if clash:
            raise Stop('exists in %s: %s' % (out, ', '.join(clash)))
        shutil.move(setup, os.path.join(out, setup_name))
        for name in packages:
            shutil.move(os.path.join(vpk_out, name), os.path.join(out, name))
        with open(os.path.join(out, FEED), 'w', encoding='utf-8', newline='\n') as f:
            json.dump(feed, f, indent=2)
            f.write('\n')
    say('%s, %s, %s (addresses under %s)' % (setup_name, ', '.join(packages), FEED, base_url))
    return 0


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    for name in ('--build-dir', '--stage', '--out', '--vpk', '--tag'):
        ap.add_argument(name, required=True)
    ap.add_argument('--icon')
    a = ap.parse_args()
    try:
        return make(a)
    except (Stop, ValueError, OSError) as e:
        say('STOPPED: %s' % e)
        return 1


if __name__ == '__main__':
    sys.exit(main())
