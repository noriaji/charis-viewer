#!/usr/bin/env python3
"""Makes the release zip of a checked Charis Viewer release build (and, with
--symbols, the symbols zip), and lays the same files out in a folder, the
input of vpk pack (velopack-release.py), so the zip and the Setup.exe hold the
same files. Read-only on the build folder.

  python -I make-release-zip.py --build-dir DIR --out OUT --stage STAGE [options]
  python -I make-release-zip.py --build-dir DIR --out OUT --symbols-only [options]

  --build-dir DIR    the build folder charis-build.sh made and checked (its
                     charis-release-build.txt says check=OK)
  --out OUT          where the zips go (made if missing; existing zips of the same
                     name are refused)
  --stage STAGE      a new or empty folder: the files of the zip, without the
                     README (it is for the zip only), for vpk pack --packDir
  --commit SHA       the commit the build must be of (the stamp's commit)
  --tag TAG          the release tag; the version in the names is the tag without
                     "v" (default: the version of plus/version/version.h)
  --velopack on|off  the USE_VELOPACK the stamp must say (default: either)
  --allow-no-bundle  without the shader bundle (app_settings/vk_shader_bundle/
                     shaders.vkb in the output folder): a warning, not a stop
  --symbols          also make the symbols zip (the pdb files, for crash dumps)
  --symbols-only     only the symbols zip (no --stage: the release workflow makes it in
                     the build job, which has the pdb files, and the zip in the package
                     job, which has the shader bundle)

What goes in: the files the packaging step lists for the installer in the same
build (secondlife_setup_tmp.nsi), without the developer-only ones; all license
files in one folder, licenses/; the license texts of libraries linked into the
exe that no package brings (indra/llvulkan/thirdparty/*LICENSE*); the shader
bundle; README-charis.txt (README-charis-zip.txt next to this file).
Names: CharisViewer-<version>-win64.zip (-win64-symbols.zip), all under a folder
of the same name.

Stops before writing when the build is not a checked release build of that
commit, the exe changed after the check, the file list is older than the exe,
a listed name is not a plain relative path (no drive, leading /, "." or ".."),
a listed file is missing, is a link or resolves outside the output folder, a
name looks like a build intermediate, per-user
data or a key, a text file holds a private key, or two names differ only in
case. The private-word check of the source (names, local paths) is not here:
it is done before the source is published.
Exit status: 0 = made, 1 = stopped (the reason is printed), 2 = wrong arguments.
"""
import argparse
import os
import re
import shutil
import string
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (BUNDLE_DIR, BUNDLE_FILES, OUTPUT_REL, PDBS, RELEASE_STAMP, SOURCE,  # noqa: E402
                           FileListError, checked_rel, read_file_list, read_stamp, release_version, sha256_file,
                           tag_version, viewer_exe)
README_TEMPLATE = os.path.join(HERE, 'README-charis-zip.txt')
README_NAME = 'README-charis.txt'
REQUIRED = ('slplugin.exe', 'llwebrtc.dll', 'licenses.txt', 'licenses-charis.txt', 'ca-bundle.crt',
            'app_settings/settings.xml', 'app_settings/settings_plus.xml', 'character/avatar_lad.xml',
            'skins/default/xui/ja/strings.xml', 'llplugin/media_plugin_cef.dll', 'llplugin/dullahan_host.exe',
            'llplugin/libcef.dll', 'llplugin/locales/ja.pak', 'msvcp140.dll', 'vcruntime140.dll',
            'vcruntime140_1.dll')
REQUIRED_PREFIXES = ('app_settings/shaders/', 'app_settings/plus/', 'fonts/')
# Build intermediates, debug files, scripts, per-user data (settings, logs, caches),
# keys and credentials: never in a release.
BAD_NAME = re.compile(
    r'\.(pdb|obj|lib|exp|ilk|idb|iobj|ipdb|tlog|pch|nsi|bat|cmd|ps1|sh|log|dmp|mdmp|bak|tmp|orig|rej)$'
    r'|(^|/)(logs?|cache|cef_cache|vk_shader_cache|vk_shader_failed|user_settings|crashes|dumps|browser_profile)/'
    r'|(^|/)\.git/|\.(pem|key|p12|pfx|ppk|jks|keystore)$|(^|/)(id_rsa|id_ed25519|id_ecdsa)|(^|/)\.env($|\.)'
    r'|(^|/)\.netrc$|credential|password|(^|/)\.portable$|velopack-pack\.json$'
    r'|(^|/)(bin_conf\.dat|mfa_hash|stored_favorites[^/]*\.xml|teleport_history\.txt|typed_locations\.txt'
    r'|search_history\.txt)$|_resident/', re.I)
BINARY_EXT = re.compile(r'\.(dll|exe|pak|bin|vkb|dat|j2c|tga|png|jpg|llm|ttf|ttc|otf|ico|bmp|wav|ogg|dic|aff)$', re.I)
PRIVATE_KEY = re.compile(rb'BEGIN [A-Z ]*PRIVATE KEY')
PDB_MAX_SKEW_S = 600   # a pdb further than this from its binary is from another link


class Stop(Exception):
    pass


def say(text):
    print('make-release-zip: ' + text, flush=True)


def warn(text):
    # "::warning::" makes it an annotation in GitHub Actions; elsewhere it is just a line.
    print('::warning::make-release-zip: ' + text, flush=True)


def check_stamp(build, output, commit, velopack):
    """The exe of a checked release build of commit; returns (exe path, stamp)."""
    stamp = read_stamp(build)
    if not stamp:
        raise Stop('not a checked release build: no charis-release-build.txt in %s (charis-build.sh)' % build)
    for key, want in RELEASE_STAMP:
        if stamp.get(key) != want:
            raise Stop('charis-release-build.txt says %s=%s, not %s' % (key, stamp.get(key), want))
    if velopack and stamp.get('use_velopack') != velopack.upper():
        raise Stop('charis-release-build.txt says use_velopack=%s, not %s' % (stamp.get('use_velopack'), velopack.upper()))
    if commit and stamp.get('commit') != commit:
        raise Stop('the build is of commit %s, not %s' % (stamp.get('commit'), commit))
    exe = viewer_exe(output)
    if not exe:
        raise Stop('no viewer exe in %s' % output)
    if sha256_file(exe) != stamp.get('exe_sha256'):
        raise Stop('%s changed after the check (its SHA-256 is not the stamp\'s): check again' % os.path.basename(exe))
    return exe, stamp


def add_bundle(files, output, exe, allow_missing):
    """The shader bundle (research/159: no release without it unless allowed). The usual
    order is package step -> bundle -> this, so the list may not name it yet."""
    for rel in BUNDLE_FILES:
        if os.path.isfile(os.path.join(output, rel)):
            files.add(rel)
    bundle = BUNDLE_FILES[0]
    if bundle not in files or not os.path.isfile(os.path.join(output, bundle)):
        files.difference_update([f for f in files if f.startswith(BUNDLE_DIR)])
        if not allow_missing:
            raise Stop('no shader bundle (%s) in the output folder' % bundle)
        warn('no shader bundle in this release: the first start compiles every shader (about a minute)')
        return False
    if os.path.getmtime(os.path.join(output, bundle)) < os.path.getmtime(exe):
        raise Stop('the shader bundle is older than %s (made for another build)' % os.path.basename(exe))
    return True


def split_licenses(files, output):
    """The license files leave the list; returns [(source path, name in the zip)] with all of
    them in licenses/ (the list spells the folder LICENSES/ for some lines), once each, and the
    license texts of the libraries the exe links that no package brings."""
    extra, seen = [], set()
    for rel in sorted(files, key=str.lower):
        head, sep, rest = rel.partition('/')
        if sep and head.lower() == 'licenses':
            files.discard(rel)
            dst = 'licenses/' + rest
            if dst.lower() not in seen:
                seen.add(dst.lower())
                extra.append((os.path.join(output, rel), dst))
    if 'licenses/vlc.txt' not in seen:
        raise Stop('no licenses/vlc.txt (did the packaging step\'s licenses block run?)')
    thirdparty = os.path.join(SOURCE, 'indra', 'llvulkan', 'thirdparty')
    for name in sorted(os.listdir(thirdparty)) if os.path.isdir(thirdparty) else ():
        path = os.path.join(thirdparty, name)
        if 'LICENSE' in name and os.path.isfile(path):
            dst = 'licenses/' + name
            if dst.lower() in seen:
                raise Stop('name clash in licenses/: %s' % dst)
            seen.add(dst.lower())
            extra.append((path, dst))
    return extra


def check_names(files, extra, output, exe_name):
    names = sorted(files) + [dst for _, dst in extra]
    missing = [f for f in sorted(files) if not os.path.isfile(os.path.join(output, f))]
    if missing:
        raise Stop('listed but not in the output folder: %s' % ', '.join(missing[:20]))
    # Only the output folder's own files: no links, nothing that resolves outside it.
    root = os.path.realpath(output) + os.sep
    outside = [f for f in sorted(files) if os.path.islink(os.path.join(output, f))
               or not os.path.realpath(os.path.join(output, f)).startswith(root)]
    outside += [dst for src, dst in extra if os.path.islink(src)]
    if outside:
        raise Stop('links or files outside the output folder: %s' % ', '.join(outside[:20]))
    for rel in (exe_name,) + REQUIRED:
        if rel not in files and rel not in names:
            raise Stop('not in the file list: %s' % rel)
    for prefix in REQUIRED_PREFIXES:
        if not any(f.startswith(prefix) for f in files):
            raise Stop('nothing under %s in the file list' % prefix)
    bad = [n for n in names if BAD_NAME.search(n)]
    if bad:
        raise Stop('names that look like build intermediates, per-user data or keys: %s' % ', '.join(bad[:20]))
    lower = {}
    for n in names:
        lower.setdefault(n.lower(), []).append(n)
    clash = [v for v in lower.values() if len(v) > 1]
    if clash:
        raise Stop('names that differ only in case: %s' % clash[:10])


def check_private_keys(items):
    """Text files only: TLS libraries carry the PEM header as a string."""
    keys = []
    for src, dst in items:
        if not BINARY_EXT.search(dst):
            with open(src, 'rb') as f:
                if PRIVATE_KEY.search(f.read()):
                    keys.append(dst)
    if keys:
        raise Stop('files with a private key: %s' % ', '.join(keys))


def stage_files(items, stage):
    if os.path.isdir(stage) and os.listdir(stage):
        raise Stop('--stage %s is not empty' % stage)
    for src, dst in items:
        path = os.path.join(stage, *dst.split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        shutil.copy2(src, path)


def write_zip(out, prefix, items):
    """items: [(source path, name under prefix/)]. Written to .part, then renamed."""
    part = out + '.part'
    try:
        with zipfile.ZipFile(part, 'w', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
            for src, dst in items:
                z.write(src, prefix + '/' + dst)
        os.replace(part, out)
    finally:
        if os.path.exists(part):
            os.remove(part)


def verify_zip(out, prefix, expected, must_have):
    with zipfile.ZipFile(out) as z:
        names = [i.filename for i in z.infolist() if not i.is_dir()]
        size = sum(i.file_size for i in z.infolist())
    def inside(n):
        if not n.startswith(prefix + '/'):
            return False
        try:
            checked_rel(n[len(prefix) + 1:], 'zip')
        except FileListError:
            return False
        return True
    outside = [n for n in names if not inside(n)]
    problems = []
    if outside:
        problems.append('outside %s/ or not a plain path: %s' % (prefix, outside[:5]))
    if len(names) != expected:
        problems.append('%d files, expected %d' % (len(names), expected))
    have = set(n[len(prefix) + 1:] for n in names)
    problems += ['not in the zip: %s' % m for m in must_have if m not in have]
    if any(n.startswith(prefix + '/LICENSES/') for n in names):
        problems.append('LICENSES/ (upper case) in the zip')
    if problems:
        os.remove(out)
        raise Stop('%s: %s (removed)' % (out, '; '.join(problems)))
    say('%s: %d files, %.0f MiB unpacked, %.0f MiB packed'
        % (out, len(names), size / 2**20, os.path.getsize(out) / 2**20))


def readme_text(exe_name, version, shown, stamp):
    with open(README_TEMPLATE, encoding='utf-8') as f:
        template = string.Template(f.read())
    text = template.substitute(shown=shown, version=version, exe=exe_name,
                               commit=stamp['commit'], checked=stamp.get('checked', '?'))
    return '﻿' + text.replace('\r\n', '\n').replace('\n', '\r\n')


def symbol_items(build, output, exe_name):
    items, cfg = [], os.path.basename(output)
    for pdb_rel, bin_rel in PDBS:
        pdb = os.path.join(build, *pdb_rel.replace('<cfg>', cfg).split('/'))
        binary = os.path.join(output, *bin_rel.replace('<exe>', exe_name).split('/'))
        if not os.path.isfile(pdb):
            warn('no pdb: %s' % pdb)
            continue
        skew = abs(os.path.getmtime(pdb) - os.path.getmtime(binary)) if os.path.isfile(binary) else None
        if skew is None or skew > PDB_MAX_SKEW_S:
            warn('%s may not match %s (%s)' % (os.path.basename(pdb), bin_rel,
                                               'no binary' if skew is None else '%.0f s apart' % skew))
        items.append((pdb, os.path.basename(pdb)))
    if not items:
        raise Stop('no pdb found for the symbols zip')
    return items


def make_symbols(build, output, exe_name, name, out):
    sym_path = os.path.join(out, name + '-symbols.zip')
    if os.path.exists(sym_path):
        raise Stop('exists: %s' % sym_path)
    sym = symbol_items(build, output, exe_name)
    write_zip(sym_path, name + '-symbols', sym)
    verify_zip(sym_path, name + '-symbols', len(sym), [])


def make(a):
    build = os.path.abspath(a.build_dir)
    output = os.path.join(build, OUTPUT_REL)
    exe, stamp = check_stamp(build, output, a.commit, a.velopack)
    exe_name = os.path.basename(exe)
    file_version, shown, _ = release_version(SOURCE)
    version = tag_version(a.tag, SOURCE) if a.tag else file_version
    if version != file_version:
        shown = version
        warn('the tag\'s version %s is not version.h\'s %s (a test tag?)' % (version, file_version))
    name = 'CharisViewer-%s-win64' % version
    out = os.path.abspath(a.out)
    if a.symbols_only:
        os.makedirs(out, exist_ok=True)
        make_symbols(build, output, exe_name, name, out)
        say('Charis Viewer %s (%s): symbols zip only' % (shown, stamp['commit'][:10]))
        return 0

    files = read_file_list(output, exe)
    has_bundle = add_bundle(files, output, exe, a.allow_no_bundle)
    extra = split_licenses(files, output)
    check_names(files, extra, output, exe_name)
    items = [(os.path.join(output, *f.split('/')), f) for f in sorted(files)] + extra
    check_private_keys(items)

    os.makedirs(out, exist_ok=True)
    zip_path = os.path.join(out, name + '.zip')
    sym_path = os.path.join(out, name + '-symbols.zip')
    for path in (zip_path, sym_path) if a.symbols else (zip_path,):
        if os.path.exists(path):
            raise Stop('exists: %s' % path)

    stage_files(items, os.path.abspath(a.stage))
    say('staged %d files in %s' % (len(items), a.stage))
    with tempfile.TemporaryDirectory() as tmp:
        readme = os.path.join(tmp, README_NAME)
        with open(readme, 'w', encoding='utf-8', newline='') as f:
            f.write(readme_text(exe_name, version, shown, stamp))
        write_zip(zip_path, name, items + [(readme, README_NAME)])
    must_have = [README_NAME, exe_name, 'slplugin.exe', 'licenses.txt', 'licenses-charis.txt',
                 'licenses/vlc.txt', 'llplugin/dullahan_host.exe'] + ([BUNDLE_FILES[0]] if has_bundle else [])
    verify_zip(zip_path, name, len(items) + 1, must_have)

    if a.symbols:
        make_symbols(build, output, exe_name, name, out)
    say('Charis Viewer %s (%s), shader bundle: %s' % (shown, stamp['commit'][:10], 'yes' if has_bundle else 'NO'))
    return 0


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--stage')
    ap.add_argument('--commit')
    ap.add_argument('--tag')
    ap.add_argument('--velopack', choices=('on', 'off'))
    ap.add_argument('--allow-no-bundle', action='store_true')
    ap.add_argument('--symbols', action='store_true')
    ap.add_argument('--symbols-only', action='store_true')
    a = ap.parse_args()
    if bool(a.stage) == a.symbols_only:
        ap.error('give --stage (the zip) or --symbols-only, one of them')
    try:
        return make(a)
    except (Stop, ValueError, OSError) as e:
        say('STOPPED: %s' % e)
        return 1


if __name__ == '__main__':
    sys.exit(main())
