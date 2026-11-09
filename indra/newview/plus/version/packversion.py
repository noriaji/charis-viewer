"""Charis Viewer's version as the installer and the update packages carry it
(Velopack's packVersion), made from plus/version/version.h, the only place the
numbers are set, and Charis's own build number. Not run: viewer_manifest.py
(the package step) loads this file by its path, plus/building/release/
releasecommon.py imports it (standard library only).

    alpha 1, build 3245     0.1.0-alpha.1.3245
    alpha 2, build 3300     0.1.0-alpha.2.3300
    beta (no number), 3400  0.1.0-beta.0.3400
    release (STAGE "")      0.1.0                (the build number is left out)
    no build number         0.1.0-alpha.1        (a build without one)

SemVer 2.0.0 (semver.org, section 11) orders these as people expect, which
Velopack's updater uses to tell whether a release is newer (vpk's
SemanticVersion.CompareTo, the Rust library's semver crate):
    0.1.0-alpha.1.N < 0.1.0-alpha.2.M < 0.1.0-beta.0.K < 0.1.0-rc.1.L < 0.1.0 < 0.1.1-alpha.1.J
  - the stage, the stage number and the build are separate dot identifiers: the
    numbers compare as numbers ("alpha.10" after "alpha.2"; "alpha10" would come
    before "alpha2"), the stage names as ASCII ("alpha" < "beta" < "rc", the order
    of plus/updatecheck's stageRank());
  - the stage number is always there with a stage (0 for "beta" without one), so
    the build is always the third identifier and never compared with a stage number;
  - a release has no pre-release part, so it comes after all of its pre-releases.
    It has no build number either: SemVer's "+build" does not count in the order,
    and one release is built once (a second build of the same release needs a new
    PATCH).
What vpk 1.2.161 takes (NugetUtil.ThrowIfVersionNotSemverCompliant, SemanticVersion.
TryParse): exactly three numbers before the "-", at least 0.0.1; its comparison
reads a numeric identifier with int.TryParse, so every number here stays within
MAX_NUMBER. Only ASCII digits, lower-case letters, "." and "-" are used: the nupkg
is named "<packId>-<packVersion>-full.nupkg", which llvelopack.cpp's
is_package_file_name() and velopack-release.py check.
"""
import re

STAGES = ('alpha', 'beta', 'rc')          # plus/updatecheck/updateversion.h stageRank()
MAX_NUMBER = 2147483647                   # vpk's SemanticVersion: int (Int32)


class VersionError(ValueError):
    """version.h or a version made from it is not what this file expects (says why)."""


def _constant(text, path, kind, name):
    if kind == 'char':
        pat = r'^[ \t]*inline constexpr char %s\[\] = "([^"\\\n]*)";' % re.escape(name)
    else:
        pat = r'^[ \t]*inline constexpr int %s = ([0-9]+);' % re.escape(name)
    found = re.findall(pat, text, re.M)
    if len(found) != 1:
        raise VersionError('%s: %s is there %d times, not once' % (path, name, len(found)))
    return found[0]


def read_version_h(path):
    """{'number': (major, minor, patch), 'stage': 'alpha', 'stage_number': 1,
    'product_name': 'Charis Viewer'} from version.h. VersionError when a constant is
    missing, twice, or out of what pack_version() accepts."""
    try:
        with open(path, encoding='utf-8') as f:
            text = f.read()
    except OSError as e:
        raise VersionError('cannot read %s: %s' % (path, e))
    info = {
        'number': tuple(int(_constant(text, path, 'int', n)) for n in ('MAJOR', 'MINOR', 'PATCH')),
        'stage': _constant(text, path, 'char', 'STAGE'),
        'stage_number': int(_constant(text, path, 'int', 'STAGE_NUMBER')),
        'product_name': _constant(text, path, 'char', 'PRODUCT_NAME'),
    }
    pack_version(info)          # stops on a stage or a number it cannot carry
    name = info['product_name']
    # Setup.exe's title, and vpk names the launcher stub "<title>.exe" after it: a Windows file name.
    if (not re.fullmatch(r'[^<>:"/\\|?*\x00-\x1f\x7f]{1,64}', name) or name != name.strip()
            or name.endswith('.') or re.fullmatch(r'(con|prn|aux|nul|com[0-9]|lpt[0-9])(\..*)?', name, re.I)):
        raise VersionError('%s: PRODUCT_NAME %r is empty or not a plain Windows file name' % (path, name))
    return info


def _number(value, what):
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= MAX_NUMBER:
        raise VersionError('%s %r is not a whole number from 0 to %d' % (what, value, MAX_NUMBER))
    return value


def build_number(text):
    """The build number of viewer_version.txt's fourth part ("3245") as an int; None for
    "" or None (a build without one). VersionError for anything but ASCII digits within
    MAX_NUMBER (leading zeros are dropped: SemVer allows none)."""
    if text is None or text == '':
        return None
    if not isinstance(text, str) or not re.fullmatch(r'[0-9]+', text, re.ASCII):
        raise VersionError('build number %r is not ASCII digits' % (text,))
    return _number(int(text), 'build number')


def pack_version(info, build=None):
    """Velopack's packVersion for version.h's info (read_version_h()) and an int build
    number or None (see the top of this file)."""
    major, minor, patch = (_number(n, name) for n, name in zip(info['number'], ('MAJOR', 'MINOR', 'PATCH')))
    if (major, minor, patch) < (0, 0, 1):
        raise VersionError('version %d.%d.%d is below 0.0.1 (vpk refuses it)' % (major, minor, patch))
    stage, stage_number = info['stage'], _number(info['stage_number'], 'STAGE_NUMBER')
    if stage and stage not in STAGES:
        raise VersionError('STAGE %r is not one of %s or "" (lower case: SemVer orders them as ASCII)'
                           % (stage, ', '.join(STAGES)))
    if build is not None:
        _number(build, 'build number')
    version = '%d.%d.%d' % (major, minor, patch)
    if not stage:
        return version
    labels = [stage, str(stage_number)] + ([str(build)] if build is not None else [])
    return version + '-' + '.'.join(labels)


def build_of(pack, info):
    """The build number in pack (a packVersion) when it is the one pack_version() makes
    for info with some build number or none: an int, or None without one. VersionError
    when pack is not such a version (another version.h, or not made by pack_version())."""
    base = pack_version(info)
    if not isinstance(pack, str):
        raise VersionError('packVersion %r is not text' % (pack,))
    if pack == base:
        return None
    if info['stage'] and pack.startswith(base + '.'):
        build = build_number(pack[len(base) + 1:])
        if build is not None and pack_version(info, build) == pack:
            return build
    wanted = ('%s or %s.<build>' % (base, base)) if info['stage'] else base
    raise VersionError('packVersion %r is not %s (plus/version/version.h, packversion.py)' % (pack, wanted))

