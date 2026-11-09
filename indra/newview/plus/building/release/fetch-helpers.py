#!/usr/bin/env python3
"""Takes the local translation and subtitle helpers (and the OpenMP runtime they load) from
their pinned release, checks each against its pinned size and SHA-256, and puts them in a
new folder under the names the viewer looks for (releasecommon.HELPER_FILES), for
make-release-zip.py --helpers.

  python -I fetch-helpers.py --base URL --out DIR --pin NAME "ASSET SIZE SHA256" [--pin ...]

  --base URL   the release's download address: https://github.com/<owner>/<repo>/releases/download/<tag>
  --out DIR    a new or empty folder (made if missing): the files go here as NAME
  --pin NAME "ASSET SIZE SHA256"
               one for each of releasecommon.HELPER_FILES, once each: the file NAME is the
               release asset ASSET (letters, digits, ". _ -"), SIZE bytes (at most 64 MiB)
               with that SHA-256 (64 lower-case hex digits)

Each asset is fetched from <base>/<ASSET>: https only, from github.com, following redirects
only to GitHub's download hosts (releasecommon.HELPER_REDIRECT_HOSTS), reading no more than
SIZE bytes, with a 120 s timeout per read; it is put in place only when its size and SHA-256
are the pinned ones. Then each must be a 64-bit Windows program (x64 PE; an .exe not a dll,
the .dll a dll), and the folder must hold exactly these files.
Network: github.com and GitHub's download hosts only.
Exit status: 0 = all fetched and checked; 1 = not fetched (network, HTTP error, a redirect
elsewhere: nothing is left in DIR); 3 = fetched, but a file is not the pinned one (another
size or SHA-256, or not an x64 program: nothing is left in DIR); 2 = wrong arguments.
"""
import argparse
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import (HELPER_FILES, HELPER_REDIRECT_HOSTS, HELPER_URL_HOSTS, DownloadFailed,  # noqa: E402
                           DownloadMismatch, download, url_host_problem)

MAX_HELPER_SIZE = 64 << 20
BASE_RE = re.compile(r'https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/releases/download/[A-Za-z0-9_.-]+')
ASSET_RE = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.-]*')
SHA256_RE = re.compile(r'[0-9a-f]{64}')
PE_MACHINE_X64 = 0x8664
PE_FILE_DLL = 0x2000


class Stop(Exception):
    pass


class NotPinned(Stop):
    """A fetched file is not the pinned one (exit 3)."""


def say(text):
    print('fetch-helpers: ' + text, flush=True)


def check_base(base):
    """base when it is a GitHub release download address (no trailing /, no '..'); Stop otherwise."""
    if not BASE_RE.fullmatch(base or '') or '..' in base:
        raise Stop('--base %r is not https://github.com/<owner>/<repo>/releases/download/<tag>' % base)
    why = url_host_problem(base, HELPER_URL_HOSTS)
    if why:
        raise Stop('--base %s: %s' % (base, why))
    return base


def parse_pins(pins):
    """[(NAME, "ASSET SIZE SHA256")] -> {NAME: (ASSET, SIZE, SHA256)}, checked: every HELPER_FILES name once and
    no other, each asset a plain name and used once, 0 < SIZE <= MAX_HELPER_SIZE, SHA256 64 lower-case hex digits."""
    out, assets = {}, set()
    for name, spec in pins:
        if name not in HELPER_FILES:
            raise Stop('--pin %r: not one of %s' % (name, ', '.join(HELPER_FILES)))
        if name in out:
            raise Stop('--pin %s twice' % name)
        fields = (spec or '').split()
        if len(fields) != 3:
            raise Stop('--pin %s %r: not "ASSET SIZE SHA256"' % (name, spec))
        asset, size, digest = fields
        if not ASSET_RE.fullmatch(asset) or '..' in asset:
            raise Stop('--pin %s: asset %r is not a plain file name' % (name, asset))
        if asset.lower() in assets:
            raise Stop('--pin %s: asset %s is pinned twice' % (name, asset))
        if not re.fullmatch(r'[1-9][0-9]{0,9}', size) or int(size) > MAX_HELPER_SIZE:
            raise Stop('--pin %s: size %r is not 1 to %d bytes' % (name, size, MAX_HELPER_SIZE))
        if not SHA256_RE.fullmatch(digest):
            raise Stop('--pin %s: %r is not a SHA-256 (64 lower-case hex digits)' % (name, digest))
        assets.add(asset.lower())
        out[name] = (asset, int(size), digest)
    missing = [n for n in HELPER_FILES if n not in out]
    if missing:
        raise Stop('no --pin for %s' % ', '.join(missing))
    return out


def pe_problem(path, want_dll):
    """Why the file is not an x64 PE image of the wanted kind (dll or not), or None."""
    with open(path, 'rb') as f:
        head = f.read(4096)
    if len(head) < 0x40 or head[:2] != b'MZ':
        return 'no MZ header'
    pe = struct.unpack_from('<I', head, 0x3c)[0]
    if pe + 24 > len(head) or head[pe:pe + 4] != b'PE\0\0':
        return 'no PE header'
    machine, = struct.unpack_from('<H', head, pe + 4)
    characteristics, = struct.unpack_from('<H', head, pe + 22)
    if machine != PE_MACHINE_X64:
        return 'machine 0x%04x, not x64' % machine
    if bool(characteristics & PE_FILE_DLL) != want_dll:
        return 'a dll' if not want_dll else 'not a dll'
    return None


def prepare_out(out):
    if os.path.islink(out) or (os.path.exists(out) and not os.path.isdir(out)):
        raise Stop('--out %s is not a folder' % out)
    if os.path.isdir(out) and os.listdir(out):
        raise Stop('--out %s is not empty' % out)
    os.makedirs(out, exist_ok=True)


def fetch(base, pins, out, get=download):
    """Fetches and checks every pinned file into out (see the module's text). get is download's stand-in for the
    selftest. On any failure out is emptied before the exception leaves."""
    prepare_out(out)
    ok = False
    try:
        for name in HELPER_FILES:
            asset, size, digest = pins[name]
            url = '%s/%s' % (base, asset)
            try:
                get(url, os.path.join(out, name), size, digest, say, hosts=HELPER_URL_HOSTS,
                    redirect_hosts=HELPER_REDIRECT_HOSTS)
            except DownloadMismatch as e:
                raise NotPinned(str(e))
            except DownloadFailed as e:
                raise Stop(str(e))
            why = pe_problem(os.path.join(out, name), name.lower().endswith('.dll'))
            if why:
                raise NotPinned('%s (%s) is not a 64-bit Windows %s: %s'
                                % (name, asset, 'dll' if name.lower().endswith('.dll') else 'program', why))
        found = sorted(os.listdir(out))
        if found != sorted(HELPER_FILES) or any(os.path.islink(os.path.join(out, n))
                                                or not os.path.isfile(os.path.join(out, n)) for n in found):
            raise NotPinned('%s holds %s, not exactly %s as plain files' % (out, found, sorted(HELPER_FILES)))
        ok = True
    finally:
        if not ok:
            for name in os.listdir(out):
                path = os.path.join(out, name)
                if os.path.isdir(path) and not os.path.islink(path):
                    shutil.rmtree(path)
                else:
                    os.remove(path)
    for name in HELPER_FILES:
        asset, size, digest = pins[name]
        say('%s: %s, %d bytes, sha256 %s' % (name, asset, size, digest))


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--base', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--pin', nargs=2, action='append', default=[], metavar=('NAME', '"ASSET SIZE SHA256"'))
    a = ap.parse_args()
    try:
        base = check_base(a.base)
        pins = parse_pins(a.pin)
    except Stop as e:
        say('STOPPED: %s' % e)
        return 2
    try:
        fetch(base, pins, os.path.abspath(a.out))
    except NotPinned as e:
        say('STOPPED (not the pinned file): %s' % e)
        return 3
    except (Stop, OSError) as e:
        say('STOPPED: %s' % e)
        return 1
    say('%d files in %s' % (len(HELPER_FILES), a.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
