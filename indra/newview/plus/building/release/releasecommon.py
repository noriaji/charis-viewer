"""What the release tools in this folder share (imported by them; not run).

The tools run with python -I, which leaves the script's folder out of
sys.path: they add this folder themselves before importing this module.
"""
import hashlib
import http.client
import importlib.util
import os
import re
import sys
import urllib.parse
import urllib.request

# The packaging step names the viewer CharisViewer.exe; a build packaged before
# that has SecondLifeViewer.exe. The new name first.
VIEWER_EXE_NAMES = ('CharisViewer.exe', 'SecondLifeViewer.exe')

# Strings only a build with Tracy has (in the exe, slplugin.exe and the media
# plugins: they all use llcommon; none of the third-party dlls has one).
TRACY_STRINGS = (b'TRACY_PORT', b'TRACY_NO_EXIT', b'Tracy Profiler', b'Tracy Symbol Worker',
                 b'@tracy@@', b'tracy::')
# The ones of them that name symbols (for the pdb).
TRACY_SYMBOLS = (b'@tracy@@', b'tracy::')
# Strings only a build without LL_RELEASE_FOR_DOWNLOAD has: llappviewer.cpp
# errorCallback() and llcommon's lltrace.cpp.
DEVELOPER_STRINGS = (b'This is a developer-only notification',
                     b'Attempting to declare trace object after program initialization')

# The checkout these tools are in (autobuild.xml, indra/).
SOURCE = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', '..'))

# In the build folder: the record check-release-build.py writes when every check passes,
# and what it says for a release build (stamp_problems()).
STAMP = 'charis-release-build.txt'
RELEASE_STAMP = (('check', 'OK'), ('use_tracy', 'OFF'), ('release_for_download', 'ON'))
# The packaging step's output folder, in the build folder.
OUTPUT_REL = os.path.join('newview', 'RelWithDebInfo')
# In the output folder: the file list the packaging step writes for the installer
# (the release zip and the Setup.exe hold these files), and the arguments for vpk
# pack (viewer_manifest.py velopack_package_finish(), USE_VELOPACK on).
FILE_LIST = 'secondlife_setup_tmp.nsi'
VELOPACK_PACK_JSON = 'velopack-pack.json'
# In the build folder the release workflow's jobs hand over (job-handoff.py): the icon
# velopack-pack.json's --icon names (viewer_manifest.py: a file in the source tree that
# the configure step writes, which the package job's fresh checkout does not have).
# release-package.yml's vpk step passes $BUILD/<this> to velopack-release.py --icon: change both.
VELOPACK_ICON_REL = 'charis-velopack/icon.ico'
# vpk pack's arguments that velopack-release.py adds itself (never in velopack-pack.json).
VPK_ADDED_ARGS = ('--packDir', '--outputDir', '-p', '-o')
# vpk pack's --runtime in velopack-pack.json (viewer_manifest.py writes it: change both).
# Without the architecture vpk uses its x86 Update.exe and stub for the 64-bit viewer.
VPK_RUNTIME = 'win-x64'
# The pdb of each binary in the zip (paths relative to the build folder; <cfg> is the
# output folder's name), for the symbols zip.
PDBS = (('newview/<cfg>/secondlife-bin.pdb', '<exe>'),
        ('llplugin/slplugin/<cfg>/SLPlugin.pdb', 'slplugin.exe'),
        ('llwebrtc/<cfg>/llwebrtc.pdb', 'llwebrtc.dll'),
        ('media_plugins/cef/<cfg>/media_plugin_cef.pdb', 'llplugin/media_plugin_cef.dll'),
        ('media_plugins/libvlc/<cfg>/media_plugin_libvlc.pdb', 'llplugin/media_plugin_libvlc.dll'))

# In the file list: what a release leaves out (developer-only), the shader bundle (made
# after the packaging step, from this exe), and the fewest files a sound list names.
DEVELOPER_ONLY = ('tracy-profiler.exe', 'llplugin/media_plugin_example.dll')
BUNDLE_DIR = 'app_settings/vk_shader_bundle/'
BUNDLE_FILES = (BUNDLE_DIR + 'shaders.vkb', BUNDLE_DIR + 'pipeline_keys_seed.bin')
MIN_FILES = 1000       # the list has several thousand files; fewer means the package step went wrong

# The local translation and subtitle helpers (plus/subtitle helperPath(): next to the viewer exe only) and the
# OpenMP runtime both of them load (they are built with /openmp; the VC runtime the package step ships has no
# vcomp140.dll). Built from tools/helpers (not by the viewer's build or the release workflow), published once
# in a release of noriaji/charis-models and pinned by size and SHA-256 in
# release-package.yml: fetch-helpers.py takes them, make-release-zip.py --helpers puts them in the zip and the
# stage (so in the Setup.exe and the update package too). Not in the packaging step's list.
HELPER_FILES = ('charis-translate.exe', 'charis-subtitle.exe', 'vcomp140.dll')
# Where fetch-helpers.py may fetch from: the release page's host, and the hosts GitHub sends a release asset's
# download on to (release-assets... since 2025, objects... before). https only, no other port.
HELPER_URL_HOSTS = ('github.com',)
HELPER_REDIRECT_HOSTS = ('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com')

# Names of Mesa's Windows files (mesa-dist-win's x64 folder, and the OpenGL ones of other
# Mesa builds): the bake uses Mesa (bake-shaders-ci.py); none of them may be in a release.
MESA_FILE_NAMES = frozenset(n.lower() for n in (
    'vulkan_lvp.dll', 'lvp_icd.x86_64.json', 'vulkan_dzn.dll', 'dzn_icd.x86_64.json', 'VkLayer_MESA_anti_lag.dll',
    'VkLayer_MESA_anti_lag.json', 'clon12compiler.dll', 'openclon12.dll', 'd3d10warp.dll', 'dxil.dll',
    'spirv_to_dxil.dll', 'spirv2dxil.exe', 'libgallium_wgl.dll', 'opengl32.dll', 'libglapi.dll', 'osmesa.dll'))

INDEPENDENCE_H = os.path.join('indra', 'newview', 'plus', 'independence', 'independence.h')
VERSION_H = os.path.join('indra', 'newview', 'plus', 'version', 'version.h')

# A release tag: v<major>.<minor>.<patch> (ASCII digits, no leading zeros), then
# optionally "-" and letters, digits and dots (v0.1.0-alpha1, v0.1.0-ci1). It ends up
# in asset names and in the addresses of the update feed, which the viewer refuses
# with "..", '%', '?' or '#'.
TAG_RE = re.compile(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?', re.ASCII)


def sha256_file(path):
    """The file's SHA-256 as 64 lower-case hex digits."""
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def viewer_exe(folder):
    """The viewer exe in folder (the first of VIEWER_EXE_NAMES there); None if none."""
    for name in VIEWER_EXE_NAMES:
        path = os.path.join(folder, name)
        if os.path.isfile(path):
            return path
    return None


def read_text(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def independence_constant(source, name):
    """A plain "..." constant of plus/independence/independence.h (viewer_manifest.py reads it the same way)."""
    text = read_text(os.path.join(source, INDEPENDENCE_H))
    found = re.findall(r'^[ \t]*inline constexpr char %s\[\] = "([^"\\\n]+)";' % re.escape(name), text, re.M)
    return found[0] if len(found) == 1 else None


def read_stamp(build):
    """The stamp of the build folder as {key: value}; {} without one."""
    path = os.path.join(build, STAMP)
    if not os.path.isfile(path):
        return {}
    fields = {}
    for line in read_text(path).splitlines():
        key, sep, value = line.partition('=')
        if sep:
            fields.setdefault(key.strip(), value.strip())
    return fields


class FileListError(ValueError):
    """The packaging step's file list cannot be used (the message says why)."""


def nsi_file_name(line):
    """The path of a 'File <path>' or 'File "<path>"' line of the .nsi (with /); None for other
    lines. viewer_manifest.py writes the plain form (File OpenAL32.dll) and the quoted one.
    A File line of another form (File "a.dll" extra) stops: it would be left out silently."""
    text = line.rstrip('\r\n')
    m = re.match(r'^[ \t]*File[ \t]+(?:"([^"\r\n]*)"|([^"\r\n]*?))[ \t]*$', text)
    if not m:
        if re.match(r'^[ \t]*File\b', text):
            raise FileListError('%s: a File line this script cannot read: %r' % (FILE_LIST, text))
        return None
    return (m.group(1) if m.group(1) is not None else m.group(2)).replace('\\', '/')


# A name Windows takes for a device (CON, NUL, COM1, LPT1.txt, "con .txt" ...): the device
# name, then optionally spaces, then optionally a dot and anything after it.
WINDOWS_DEVICE = re.compile(r'(con|prn|aux|nul|conin\$|conout\$|com[0-9¹²³]|lpt[0-9¹²³]) *(\..*)?',
                            re.I)


def checked_rel(name, where):
    """name when it is a plain relative path with / between its parts, the same on Windows: no
    drive or ':', no leading /, no backslash, no empty, . or .. part, no part ending in a dot or a
    space (Windows drops them) and no device name, with or without spaces and an extension after
    it (WINDOWS_DEVICE)."""
    parts = name.split('/')
    if (not name or name.startswith('/') or ':' in name or '\\' in name
            or any(p in ('', '.', '..') or p != p.rstrip(' .') or WINDOWS_DEVICE.fullmatch(p) for p in parts)):
        raise FileListError('%s: %r is not a plain relative path' % (where, name))
    return name


def read_file_list(output, exe):
    """The files of the packaging step's list (paths relative to output, with /), without
    DEVELOPER_ONLY. FileListError when the list is missing, older than exe, too short or
    names something that is not a plain relative path."""
    nsi = os.path.join(output, FILE_LIST)
    if not os.path.isfile(nsi):
        raise FileListError('no %s in %s (the packaging step has not run)' % (FILE_LIST, output))
    if os.path.getmtime(exe) > os.path.getmtime(nsi):
        raise FileListError('%s is older than %s: the packaging step did not run after the last link'
                            % (FILE_LIST, os.path.basename(exe)))
    files = set()
    with open(nsi, encoding='utf-8', errors='replace') as f:
        for lineno, line in enumerate(f, 1):
            name = nsi_file_name(line)
            if name is not None:
                files.add(checked_rel(name, '%s line %d' % (FILE_LIST, lineno)))
    files -= set(DEVELOPER_ONLY)
    if len(files) < MIN_FILES:
        raise FileListError('only %d files listed in %s (expected several thousand)' % (len(files), FILE_LIST))
    return files


def stamp_problems(stamp):
    """The RELEASE_STAMP fields the stamp gets wrong, as 'key=value' ('(none)' when missing);
    empty for a checked release build."""
    return ['%s=%s' % (k, stamp.get(k, '(none)')) for k, v in RELEASE_STAMP if stamp.get(k) != v]


def vpk_arg_value(args, name, where):
    """The value after name in vpk's arguments (None without name); ValueError when name is the
    last one."""
    if name not in args:
        return None
    i = args.index(name)
    if i + 1 >= len(args):
        raise ValueError('%s: vpkArgs ends with %s, without its value' % (where, name))
    return args[i + 1]


def has_vpk_runtime(args):
    """Whether the only runtime argument in vpk's arguments is "--runtime VPK_RUNTIME": no other
    spelling of it (-r, -r:…, -r=…, -r…, --runtime=…, --runtime:…, any letter case) and no
    response file (@…), which could carry one."""
    def runtime_like(arg):
        a = arg.lower()
        return a.startswith('--runtime') or (a.startswith('-r') and not a.startswith('--'))
    if any(a.startswith('@') for a in args) or [a for a in args if runtime_like(a)] != ['--runtime']:
        return False
    i = args.index('--runtime')
    return i + 1 < len(args) and args[i + 1] == VPK_RUNTIME


# plus/version/packversion.py: version.h's numbers and Velopack's packVersion made from them
# (viewer_manifest.py loads the same file). Loaded when first used, never at import: the release
# workflow's bake job checks out only the bake tools, this file among them, and not plus/version
# (release-package.yml sparse-checkout; the selftest sparse-imports checks it).
PACKVERSION_PY = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'version', 'packversion.py')
_packversion = None


def packversion_module():
    """plus/version/packversion.py as a module, loaded once (without writing __pycache__)."""
    global _packversion
    if _packversion is None:
        spec = importlib.util.spec_from_file_location('charis_packversion', PACKVERSION_PY)
        module = importlib.util.module_from_spec(spec)
        keep = sys.dont_write_bytecode
        sys.dont_write_bytecode = True
        try:
            spec.loader.exec_module(module)
        finally:
            sys.dont_write_bytecode = keep
        _packversion = module
    return _packversion


def read_version(source):
    """version.h's numbers (packversion.read_version_h(): number, stage, stage_number,
    product_name). ValueError when a constant is missing or cannot be carried."""
    return packversion_module().read_version_h(os.path.join(source, VERSION_H))


def release_version(source):
    """(file name version, shown version, (major, minor, patch)) from plus/version/version.h:
    ('0.1.0-alpha1', '0.1.0 alpha 1', (0, 1, 0)). ValueError when a constant is missing.
    Velopack's packVersion is packversion_module().pack_version() ('0.1.0-alpha.1.<build>')."""
    info = read_version(source)
    number, stage, stage_number = info['number'], info['stage'], info['stage_number']
    version = shown = '%d.%d.%d' % number
    if stage:
        version, shown = version + '-' + stage, shown + ' ' + stage
        if stage_number:
            version, shown = version + str(stage_number), shown + ' ' + str(stage_number)
    return version, shown, number


def tag_version(tag, source):
    """The version in asset names for a release tag: the tag without its "v" (v0.1.0-ci1 ->
    0.1.0-ci1). ValueError when the tag is not TAG_RE or its major.minor.patch is not version.h's."""
    m = TAG_RE.fullmatch(tag or '')
    if not m:
        raise ValueError('tag %r is not v<major>.<minor>.<patch>[-<letters, digits, dots>]' % tag)
    _, _, number = release_version(source)
    if tuple(int(g) for g in m.groups()[:3]) != number:
        raise ValueError('tag %s is not version %d.%d.%d of %s' % ((tag,) + number + (VERSION_H,)))
    return tag[1:]


class DownloadFailed(Exception):
    pass


class DownloadMismatch(DownloadFailed):
    """What was fetched is not the pinned file (another size or SHA-256): not a network failure."""


def url_host_problem(url, hosts):
    """Why url is not an https URL on one of hosts (port 443 or none, no user name), or None."""
    try:
        parts = urllib.parse.urlsplit(url)
        port = parts.port
    except ValueError as e:
        return 'not a URL (%s)' % e
    if parts.scheme.lower() != 'https':
        return 'not https'
    if parts.username is not None or parts.password is not None:
        return 'has a user name'
    if port not in (None, 443):
        return 'port %s' % port
    if (parts.hostname or '').lower() not in hosts:
        return 'host %s is not one of %s' % (parts.hostname, ', '.join(hosts))
    return None


class HttpsOnlyRedirects(urllib.request.HTTPRedirectHandler):
    """Follows redirects only to https URLs (and, when hosts is given, only to those hosts)."""

    def __init__(self, hosts=None):
        super().__init__()
        self.hosts = tuple(hosts) if hosts else None

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not newurl.lower().startswith('https://'):
            raise DownloadFailed('refused a redirect from %s to %s (not https)' % (req.full_url, newurl))
        if self.hosts is not None:
            why = url_host_problem(newurl, self.hosts)
            if why:
                raise DownloadFailed('refused a redirect from %s to %s (%s)' % (req.full_url, newurl, why))
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def copy_checked(stream, part, size, digest, url):
    """Copies stream to the file part, reading at most size + 1 bytes; DownloadMismatch unless it held exactly
    size bytes with that SHA-256."""
    h = hashlib.sha256()
    got = 0
    with open(part, 'wb') as f:
        for block in iter(lambda: stream.read(1 << 20), b''):
            got += len(block)
            if got > size:
                break
            h.update(block)
            f.write(block)
    if got != size or h.hexdigest() != digest:
        raise DownloadMismatch('%s: %s bytes, sha256 %s; expected %d bytes, sha256 %s: removed'
                               % (url, got if got <= size else 'more than %d' % size, h.hexdigest(), size, digest))


def download(url, path, size, digest, say=print, hosts=None, redirect_hosts=None):
    """Fetches url to path unless a file with that size and SHA-256 is there (https only,
    the size and SHA-256 checked before the file is put in place). With hosts, url must be on
    one of them and every redirect on one of redirect_hosts (default: hosts). say(text) tells
    progress; DownloadFailed when it fails (DownloadMismatch: fetched, but not that file)."""
    if hosts:
        why = url_host_problem(url, hosts)
        if why:
            raise DownloadFailed('refused to fetch %s (%s)' % (url, why))
    if os.path.isfile(path) and os.path.getsize(path) == size and sha256_file(path) == digest:
        say('have %s' % path)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    part = path + '.part'
    say('fetching %s' % url)
    done = False
    try:
        opener = urllib.request.build_opener(HttpsOnlyRedirects(redirect_hosts or hosts))
        with opener.open(url, timeout=120) as r:
            copy_checked(r, part, size, digest, url)
        os.replace(part, path)
        done = True
    except (OSError, http.client.HTTPException) as e:
        raise DownloadFailed('fetching %s: %s' % (url, e))
    finally:
        if not done and os.path.exists(part):
            os.remove(part)
    say('fetched %s (sha256 %s)' % (path, digest))
