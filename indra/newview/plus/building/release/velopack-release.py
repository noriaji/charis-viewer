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
  <packId>-<packVersion>-full.nupkg       the update package (named as vpk names it; packVersion
                                          is Charis's version, 0.1.0-alpha.1.<build>: it must be
                                          the one plus/version/packversion.py makes from version.h)
  releases.win.json                       the update feed: each FileName becomes
      <PROJECT_URL>/releases/download/<tag>/<FileName> (llvelopack.cpp takes no
      other address); the Size and SHA256 of each package are checked against the
      file first
The package's application files (lib/app/) are checked against the staged
files, by the names the installer extracts them under (see check_package_files).
vpk's other outputs (RELEASES, assets.win.json) are left out; a Portable.zip
stops it (--noPortable is in the arguments). No delta packages: no earlier
release is put in vpk's output folder (the viewer's downloader does not apply
deltas yet).
Exit status: 0 = made, 1 = stopped (the reason is printed), 2 = wrong arguments.
"""
import argparse
import json
import mmap
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (OUTPUT_REL, SOURCE, VELOPACK_PACK_JSON, VPK_ADDED_ARGS, VPK_RUNTIME,  # noqa: E402
                           has_vpk_runtime, independence_constant, packversion_module, read_stamp, read_version,
                           sha256_file, tag_version, vpk_arg_value)
FEED = 'releases.win.json'        # Velopack's feed for its default Windows channel, "win"
# A package name llvelopack.cpp is_package_file_name() accepts: "<packId>-...nupkg" of
# letters, digits and ".-_+", no "..", at most 200 characters.
MAX_PACKAGE_NAME = 200
PACKAGE_CHARS = re.compile(r'[A-Za-z0-9._+-]+')
LEFT_OUT = ('RELEASES', 'assets.win.json')
# In a package, the application's files are under lib/app/. vpk 1.2.161 writes the entry
# names as the files are named (UTF-8, "\" made "/"; no percent-encoding: the staged
# "Colder%20Tones.xml" is the entry "lib/app/.../Colder%20Tones.xml"), and the installer
# (bundle.rs extract_lib_contents_to_path) extracts every entry LIB_ENTRY matches under the
# name left once its first match is cut, without decoding. So the names are compared as
# they are: decoding them would turn a staged "%20" into a space.
PACKAGE_APP_DIR = 'lib/app/'
LIB_ENTRY = re.compile(r'lib[\\/][^\\/]*[\\/]')
# What vpk adds to lib/app/ (WindowsPackCommandRunner.cs): the version marker and the updater,
# always; the launcher stub named after --packTitle, which the lib/app/ loop skips and the
# installer and the updater extract to the install root as <packTitle>.exe (install.rs and
# apply_windows_impl.rs call bundle.rs extract_stubs_to_dir).
VELOPACK_VERSION_FILE = 'sq.version'
VELOPACK_UPDATER = 'Squirrel.exe'
VELOPACK_REQUIRED_FILES = (VELOPACK_VERSION_FILE, VELOPACK_UPDATER)
STUB_SUFFIX = '_ExecutionStub.exe'
# Names the installer treats specially wherever they are (bundle.rs): the first entry ending in
# the updater's name is the updater, every name ending in the stub suffix is extracted next to
# the app as <name>.exe, the first entry ending in .nuspec becomes sq.version, and these regular
# expressions (Rust's: "." is any character) skip the stub and make a symbolic link. No staged
# file may have such a name.
SPECIAL_NAME = re.compile(r'(Squirrel\.exe|_ExecutionStub.exe|.__symlink|\.nuspec)$|^sq\.version$', re.I)
# What vpk puts outside lib/ (PackageBuilder.cs CreateReleasePackage): the nuspec (required),
# the content types, the relationships and the Setup.exe's icon. Any other entry stops.
PACKAGE_ROOT_FILES = ('[Content_Types].xml', '_rels/.rels', 'setup.ico')
# Info-ZIP's Unicode Path extra field: the installer's zip library (zip 8.6.0) takes the name
# from it, Python's zipfile does not, so a package with one could be checked under one name
# and extracted under another. vpk (.NET's ZipArchive) never writes it.
UNICODE_PATH_EXTRA = 0x7075
LOCAL_HEADER = struct.Struct('<4sHHHHHIIIHH')
LOCAL_HEADER_SIGNATURE = b'PK\x03\x04'
COMPARE_CHUNK = 1 << 20
# The end of central directory record and the zip64 ones (APPNOTE 4.3.14-16). zipfile reads the
# central directory by its size, the installer's zip library by the record's entry count: the
# two must agree.
END_RECORD = struct.Struct('<4sHHHHIIH')
END_RECORD_SIGNATURE = b'PK\x05\x06'
ZIP64_LOCATOR = struct.Struct('<4sIQI')
ZIP64_LOCATOR_SIGNATURE = b'PK\x06\x07'
ZIP64_END_RECORD = struct.Struct('<4sQHHIIQQQQ')
ZIP64_END_RECORD_SIGNATURE = b'PK\x06\x06'
MAX_COMMENT = 0xFFFF
# Velopack's Setup.exe carries the full package appended to its end; the 16 bytes before this
# signature hold the package's offset and length (SetupBundle.cs, little-endian int64s).
SETUP_BUNDLE_SIGNATURE = bytes((
    0x94, 0xf0, 0xb1, 0x7b, 0x68, 0x93, 0xe0, 0x29, 0x37, 0xeb, 0x34, 0xef, 0x53, 0xaa, 0xe7, 0xd4,
    0x2b, 0x54, 0xf5, 0x70, 0x7e, 0xf5, 0xd6, 0xf5, 0x78, 0x54, 0x98, 0x3e, 0x5e, 0x94, 0xed, 0x7d))


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
    # Without an architecture vpk puts the x86 Update.exe and stub into the package.
    if not has_vpk_runtime(args):
        raise Stop('%s: vpkArgs does not have --runtime %s once' % (path, VPK_RUNTIME))
    version = pack.get('packVersion')
    if not isinstance(version, str) or not version:
        raise Stop('%s: no packVersion' % path)
    if args.count('--packVersion') != 1 or vpk_arg_value(args, '--packVersion', path) != version:
        raise Stop('%s: vpkArgs does not have --packVersion %s once' % (path, version))
    # Charis's version (version.h of this checkout), not the official one: the installer shows
    # it, and the updater orders releases by it (plus/version/packversion.py).
    try:
        packversion_module().build_of(version, read_version(SOURCE))
    except ValueError as e:
        raise Stop('%s: %s' % (path, e))
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


def unsafe_app_name(rel):
    """Why rel (an entry name under lib/app/) is not a plain relative path the installer may
    write, or None."""
    if not rel or rel.startswith('/') or '\\' in rel or ':' in rel:
        return 'is not a relative path with /'
    if any(ord(c) < 32 or ord(c) == 127 for c in rel):
        return 'has a control character'
    if any(part in ('', '.', '..') for part in rel.split('/')):
        return 'has an empty, "." or ".." part'
    return None


def extra_ids(extra, where):
    """The ids of the fields in a zip extra block; Stop when the block is not whole fields."""
    ids, i = [], 0
    while i < len(extra):
        if i + 4 > len(extra):
            raise Stop('%s: a broken extra field' % where)
        field_id, size = struct.unpack_from('<HH', extra, i)
        i += 4 + size
        if i > len(extra):
            raise Stop('%s: a broken extra field' % where)
        ids.append(field_id)
    return ids


def check_entry_count(f, base, count):
    """Stop unless the end record (and the zip64 one, when there is one) gives count entries, on
    this disk and in all: the installer's zip library lists as many as it says."""
    f.seek(0, os.SEEK_END)
    size = f.tell()
    tail_start = max(0, size - END_RECORD.size - MAX_COMMENT)
    f.seek(tail_start)
    tail = f.read()
    at = tail.rfind(END_RECORD_SIGNATURE)
    if at < 0 or at + END_RECORD.size > len(tail):
        raise Stop('%s has no end of central directory record' % base)
    record = END_RECORD.unpack_from(tail, at)
    if at + END_RECORD.size + record[7] != len(tail):
        raise Stop('%s: the end of central directory record is not at the end' % base)
    short_counts, wide_counts = record[3:5], ()
    record_at = tail_start + at
    if record_at >= ZIP64_LOCATOR.size:
        f.seek(record_at - ZIP64_LOCATOR.size)
        locator = f.read(ZIP64_LOCATOR.size)
        if locator[:4] == ZIP64_LOCATOR_SIGNATURE:
            f.seek(ZIP64_LOCATOR.unpack(locator)[2])
            wide = f.read(ZIP64_END_RECORD.size)
            if len(wide) != ZIP64_END_RECORD.size or wide[:4] != ZIP64_END_RECORD_SIGNATURE:
                raise Stop('%s: the zip64 locator points at no zip64 end record' % base)
            wide_counts = ZIP64_END_RECORD.unpack(wide)[6:8]
    # With a zip64 record, the short counts may be 0xFFFF ("see the zip64 record").
    bad_short = [c for c in short_counts if c != count and not (wide_counts and c == 0xFFFF)]
    if bad_short or any(c != count for c in wide_counts):
        raise Stop('%s: the end records count %s entries, the central directory holds %d'
                   % (base, '/'.join(str(c) for c in short_counts + wide_counts), count))


def check_entry_headers(f, info, where):
    """The entry's local header agrees with the central directory on the name, and neither
    header has a Unicode Path field (the installer would use that name instead)."""
    raw_name = info.orig_filename.encode('utf-8' if info.flag_bits & 0x800 else 'cp437')
    f.seek(info.header_offset)
    head = f.read(LOCAL_HEADER.size)
    if len(head) != LOCAL_HEADER.size or head[:4] != LOCAL_HEADER_SIGNATURE:
        raise Stop('%s: no local header' % where)
    name_len, extra_len = LOCAL_HEADER.unpack(head)[9:11]
    local_name, local_extra = f.read(name_len), f.read(extra_len)
    if local_name != raw_name:
        raise Stop('%s: the local header names %r' % (where, local_name))
    for ids in (extra_ids(info.extra, where + ' (central directory)'), extra_ids(local_extra, where + ' (local header)')):
        if UNICODE_PATH_EXTRA in ids:
            raise Stop('%s: a Unicode Path extra field (the installer would extract it under that name)' % where)


def package_entries(z, f, base, pack_id):
    """{name relative to the app folder: ZipInfo} of the entries the installer extracts, after
    checking every entry: headers (check_entry_headers), none it would extract from outside
    lib/app/, no unsafe name, no two one Windows folder cannot tell apart, and outside lib/
    only the nuspec (required) and PACKAGE_ROOT_FILES."""
    nuspec = pack_id + '.nuspec'
    packed, seen = {}, set()
    for info in z.infolist():
        name = info.orig_filename    # as stored: filename has "\" made "/" on Windows and is cut at a NUL
        where = '%s: entry %r' % (base, name)
        check_entry_headers(f, info, where)
        if name.casefold() in seen:
            raise Stop('%s is there twice (letter case aside)' % where)
        seen.add(name.casefold())
        if not LIB_ENTRY.search(name):
            if name != nuspec and name not in PACKAGE_ROOT_FILES:
                raise Stop('%s is outside lib/ and not one of vpk\'s own (%s, %s)'
                           % (where, nuspec, ', '.join(PACKAGE_ROOT_FILES)))
            continue
        if not name.startswith(PACKAGE_APP_DIR):
            raise Stop('%s would be extracted, but is not under %s' % (where, PACKAGE_APP_DIR))
        rel = name[len(PACKAGE_APP_DIR):]
        folder = rel.endswith('/')        # the installer skips folders
        why = unsafe_app_name(rel[:-1] if folder else rel) if rel else None
        if why:
            raise Stop('%s %s' % (where, why))
        if not folder and rel:
            packed[rel] = info
    if nuspec.casefold() not in seen:
        raise Stop('%s has no %s' % (base, nuspec))
    return packed


def same_bytes(a, b):
    """Whether the two readable streams hold the same bytes from here to their ends."""
    while True:
        x, y = a.read(COMPARE_CHUNK), b.read(COMPARE_CHUNK)
        if x != y:
            return False
        if not x:
            return True


def compare_entry(z, info, path, where):
    """Stop unless the entry's bytes are the file's (zipfile checks the CRC at the end)."""
    if info.file_size != os.path.getsize(path):
        raise Stop('%s has %d bytes, the staged file %d' % (where, info.file_size, os.path.getsize(path)))
    with z.open(info) as packed, open(path, 'rb') as staged:
        if not same_bytes(packed, staged):
            raise Stop('%s is not the staged file' % where)


def check_package_files(nupkg, stage, exclude, pack_id, stub):
    """The application files in the package (lib/app/) are the staged files, name for name and
    byte for byte: none missing (except what vpk's --exclude leaves out) and none added (except
    Velopack's own: VELOPACK_REQUIRED_FILES, which must be there, and the stub). With signing
    (vpk --signParams) the signed exe and dll would differ from the staged ones: this check
    then has to compare them without their signatures."""
    base = os.path.basename(nupkg)
    staged = staged_files(stage)
    taken = sorted(n for n in staged if SPECIAL_NAME.search(n))
    if taken:
        raise Stop('the stage has files named as the installer\'s own: %s' % taken[:10])
    try:
        with open(nupkg, 'rb') as f, zipfile.ZipFile(f) as z:
            check_entry_count(f, base, len(z.infolist()))
            packed = package_entries(z, f, base, pack_id)
            lacking = [n for n in VELOPACK_REQUIRED_FILES if n not in packed]
            if lacking:
                raise Stop('%s has no %s (vpk adds them)' % (base, ', '.join(lacking)))
            app = set(packed) - set(VELOPACK_REQUIRED_FILES) - {stub}
            missing = sorted(n for n in staged - app if not (exclude and exclude.fullmatch(n.rsplit('/', 1)[-1])))
            added = sorted(app - staged)
            if missing or added:
                raise Stop('%s does not hold the staged files: missing %s; added %s'
                           % (base, missing[:10] or 'none', added[:10] or 'none'))
            for rel in sorted(app):
                compare_entry(z, packed[rel], os.path.join(stage, *rel.split('/')), '%s: %s' % (base, rel))
    except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError, EOFError) as e:
        raise Stop('cannot read %s: %s' % (base, e))
    say('%s: %d application files, the staged ones byte for byte' % (base, len(app)))


def check_setup_bundle(setup, nupkg):
    """Stop unless the Setup.exe carries exactly this package, appended to its end. With signing,
    the certificate is appended after the package and offset + length != len(m) always stops:
    then the PE security directory has to be read and that tail allowed."""
    base, size = os.path.basename(setup), os.path.getsize(nupkg)
    with open(setup, 'rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m, open(nupkg, 'rb') as p:
        at = m.find(SETUP_BUNDLE_SIGNATURE)          # the first one, as SetupBundle.IsBundle reads it
        if at < 16:
            raise Stop('%s has no package bundle header' % base)
        offset, length = struct.unpack_from('<qq', m, at - 16)
        if length != size or offset <= at or offset + length != len(m):
            raise Stop('%s: the bundle header says %d bytes at %d; the package has %d bytes and the file ends at %d'
                       % (base, length, offset, size, len(m)))
        m.seek(offset)                               # the package runs to the end (checked above)
        if not same_bytes(m, p):
            raise Stop('%s does not carry %s' % (base, os.path.basename(nupkg)))
    say('%s carries %s' % (base, os.path.basename(nupkg)))


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
        stub = (vpk_arg_value(args, '--packTitle', 'vpkArgs') or pack_id) + STUB_SUFFIX
        for name in packages:
            check_package_files(os.path.join(vpk_out, name), stage, exclude, pack_id, stub)
        full = [n for n in packages if n.endswith('-full.nupkg')]
        if len(full) != 1:
            raise Stop('%s has %d full packages, not 1' % (FEED, len(full)))
        check_setup_bundle(setup, os.path.join(vpk_out, full[0]))
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
