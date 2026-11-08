#!/usr/bin/env python3
"""Fetches what a Charis Viewer release build needs besides Visual Studio 2022,
CMake, Python and Git, each at a fixed version and checked before it is used:

  autobuild        3.10.2 and its dependencies in a venv of its own: pip installs
                   only the wheels in requirements-autobuild.txt, each checked
                   against its SHA-256 (--require-hashes, --no-deps), and
                   "pip check" then confirms that no dependency is missing
  build-variables  secondlife/build-variables at BUILD_VARIABLES_COMMIT, with
                   ../optimize-relwithdebinfo.patch applied; the patched file is
                   checked against VARIABLES_SHA256
  Vulkan SDK       VULKAN_SDK_VERSION from LunarG: the installer's size and
                   SHA-256 are checked before it runs, and the SDK files the
                   build reads are checked after (the whole Include folder,
                   VULKAN_INCLUDE_SHA256, and the libraries, VULKAN_SDK_LIBS)

Needs Python 3.10 to 3.14 (the venv is made with the Python running this).

  python -I setup-deps.py --deps DIR [--vulkan-sdk SDK | --vulkan-installer FILE] [--steps LIST]

  --deps DIR        where everything goes (made if missing). Run again with the
                    same DIR: what is there and checks out is kept.
  --vulkan-sdk SDK  use this Vulkan SDK (already installed) instead of fetching
                    one; its Include folder and libraries must match
  --vulkan-installer FILE
                    install from this copy of the LunarG installer instead of
                    fetching it (fetched another way, e.g. with curl: LunarG
                    refuses Python's urllib); its size and SHA-256 must match
  --steps LIST      some of autobuild,build-variables,vulkan (default all three;
                    env.sh is written only when all three run)
  --print-vulkan-url
                    print the address of the Vulkan SDK installer and stop

Writes DIR/env.sh for bash: source it, then run ../charis-build.sh. It puts the
autobuild of DIR/venv first on PATH and sets AUTOBUILD_VARIABLES_FILE,
VULKAN_SDK and AUTOBUILD_INSTALLABLE_CACHE (DIR/autobuild-cache: the packages of
autobuild.xml are fetched there, and autobuild checks each one against the hash
in autobuild.xml, also when it is already in the cache).

The autobuild in DIR/venv gets one change: the output of the tools it runs is
decoded with errors="replace". On Windows with a non-UTF-8 code page (Japanese,
...) autobuild 3.10.2 otherwise stops with a UnicodeDecodeError. Only how the
output is read changes, not the build. The file is checked before (the 3.10.2
file) and after (the expected result).

The installer of the Vulkan SDK is run with --accept-licenses: running this
step accepts LunarG's license terms for the SDK. It is run with copy_only=1
(files only: no environment variables and no registry entries) and may ask for
administrator rights.

Network: github.com (build-variables), pypi.org and files.pythonhosted.org
(pip), sdk.lunarg.com (the Vulkan SDK installer; not with --vulkan-sdk or
--vulkan-installer). Nothing else.
Exit status: 0 = everything ready, 1 = a step failed, 2 = wrong arguments.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILDING = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import DownloadFailed, download, sha256_file  # noqa: E402

PYTHON_VERSIONS = ((3, 10), (3, 14))      # the oldest and newest the wheels in REQUIREMENTS cover
AUTOBUILD_VERSION = '3.10.2'
REQUIREMENTS = os.path.join(HERE, 'requirements-autobuild.txt')
# autobuild/autobuild_tool_source_environment.py: as in the 3.10.2 wheel, and
# after DECODE_FIX.
SOURCE_ENV_REL = os.path.join('Lib', 'site-packages', 'autobuild', 'autobuild_tool_source_environment.py')
SOURCE_ENV_SHA256 = 'a0d1d249bc66dbfe2e35ce9ab145e0bc637a8eba65e73a49adac57804602bd7f'
SOURCE_ENV_FIXED_SHA256 = '0c79774f1d8f709d4b1ff547407d736f56e6b30a5c129af0e0063bd3e625bfeb'
DECODE_FIX = (
    (b"            universal_newlines=True)\n",
     b"            universal_newlines=True, errors=\"replace\")\n"),
    (b"# don't pass text=True or universal_newlines=True: we want bytes",
     b"# don't pass text=True or universal_newlines=True, errors=\"replace\": we want bytes"),
    (b"'-format', 'json'], universal_newlines=True).rstrip()",
     b"'-format', 'json'], encoding=\"utf-8\", errors=\"replace\").rstrip()"),
    (b"script = subprocess.Popen(cmdline, text=True,\n",
     b"script = subprocess.Popen(cmdline, text=True, errors=\"replace\",\n"),
    (b"stdout=subprocess.PIPE, universal_newlines=True) \\",
     b"stdout=subprocess.PIPE, universal_newlines=True, errors=\"replace\") \\"),
)

BUILD_VARIABLES_URL = 'https://github.com/secondlife/build-variables'
BUILD_VARIABLES_COMMIT = 'f8fad0dbbf6c821e1ae6e254e1ed203e93815a94'
OPTIMIZE_PATCH = os.path.join(BUILDING, 'optimize-relwithdebinfo.patch')
# The line charis-build.sh checks for (the patch makes RelWithDebInfo /O2 /Ob2).
OPTIMIZED_LINE = 'LL_BUILD_WINDOWS_RELWITHDEBINFO_SWITCHES="/MD /O2 /Ob2 '
# "variables" at BUILD_VARIABLES_COMMIT with OPTIMIZE_PATCH applied (LF line ends).
VARIABLES_SHA256 = 'eaf34387d5519e9645ea97c654132957f957da1f801229831c49154c3cc8b646'

VULKAN_SDK_VERSION = '1.4.363.0'
VULKAN_INSTALLER = 'vulkansdk-windows-X64-%s.exe' % VULKAN_SDK_VERSION
VULKAN_INSTALLER_URL = 'https://sdk.lunarg.com/sdk/download/%s/windows/%s' % (VULKAN_SDK_VERSION, VULKAN_INSTALLER)
# LunarG publishes it at https://sdk.lunarg.com/sdk/sha/<version>/windows/<installer>.txt
VULKAN_INSTALLER_SHA256 = '94a82d378f7a5e3e54c9db7d2fb7016af136e14ac0a18dbf0f2f67a36352d141'
VULKAN_INSTALLER_SIZE = 303082720
# The SDK files the build reads (indra/llvulkan/llvulkan.cmake: the include
# folder and LL_VULKAN_LIBS), from an install of the installer above (core
# component only). The Include folder as one digest (include_digest()).
VULKAN_INCLUDE_SHA256 = '550550bcd7da2d6191c15d4e3b56ce6dc3350c07360c4032eab419eb051bdd9f'
VULKAN_SDK_LIBS = (
    ('Lib/vulkan-1.lib', '02caa31b8665a79f327cbfe97286fd7d0017c0b1f9b39eb78687b7a509de52b3'),
    ('Lib/glslang.lib', '8ae81d0469654c7bbac61273d228f4c859540fb54732fe613bbbf2a48c11c924'),
    ('Lib/glslang-default-resource-limits.lib', 'f58646f5916087960a79bf8b3bebab040ce5e4436326740e804ba1112ea10309'),
    ('Lib/MachineIndependent.lib', 'f561d6d323ccd6adb02a53827f4881035320bf394e40509a421b33bdf1c55883'),
    ('Lib/GenericCodeGen.lib', '6aa2dfee3a8916a4da590a2303a258d400f7d9889ada3021ba08c642a0d716ed'),
    ('Lib/OSDependent.lib', 'cbf825b28b6efc48a7aa770defc808014521ddb6bbbeae32d4850408dfd849af'),
    ('Lib/SPIRV.lib', '2bf8c6d6e9b1d7da34cff6c33c22c5e709c4b85a6705fa3081a1f35bfbec83f5'),
    ('Lib/SPIRV-Tools-opt.lib', '91c845c766d28fd60e44b5eb8c166fd4977a4b6aaaa555d4bed8252136df50b6'),
    ('Lib/SPIRV-Tools.lib', 'ef7a63ce25ca968b76ee187316d328546232f84a744a6a8721c9864ccfb741b1'),
)

STEPS = ('autobuild', 'build-variables', 'vulkan')


class StepFailed(Exception):
    pass


def say(text):
    print('setup-deps: ' + text, flush=True)


def run(cmd, **kw):
    """Runs a command (a list, no shell); StepFailed when it fails."""
    say('$ ' + ' '.join(cmd))
    try:
        subprocess.run(cmd, check=True, **kw)
    except (OSError, subprocess.CalledProcessError) as e:
        raise StepFailed('%s: %s' % (cmd[0], e))


def output(cmd):
    """Runs a command and returns its output (stripped); None when it fails."""
    try:
        r = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           encoding='utf-8', errors='replace')
    except (OSError, subprocess.CalledProcessError):
        return None
    return r.stdout.strip()


def forward(path):
    """C:\\a\\b -> C:/a/b (what CMake, autobuild and the Windows tools take)."""
    return os.path.abspath(path).replace('\\', '/')


def bash_path(path):
    """C:\\a\\b -> /c/a/b (for PATH in Git Bash)."""
    m = re.match(r'^([A-Za-z]):/(.*)$', forward(path))
    if not m:
        raise StepFailed('not a path on a drive letter: %s' % path)
    return '/%s/%s' % (m.group(1).lower(), m.group(2))


# ---- autobuild ---------------------------------------------------------------

def setup_autobuild(deps):
    venv = os.path.join(deps, 'venv')
    py = os.path.join(venv, 'Scripts', 'python.exe')
    if not os.path.isfile(py):
        run([sys.executable, '-m', 'venv', venv])
    # --isolated: no pip configuration files or PIP_* variables (another index, other options)
    pip = [py, '-I', '-m', 'pip', '--isolated', '--disable-pip-version-check', '--no-input']
    run(pip + ['install', '--require-hashes', '--only-binary', ':all:', '--no-deps', '-r', REQUIREMENTS])
    run(pip + ['check'])
    apply_decode_fix(os.path.join(venv, SOURCE_ENV_REL))
    version = output([py, '-I', '-c', 'from autobuild.version import AUTOBUILD_VERSION_STRING as v; print(v)'])
    if version != AUTOBUILD_VERSION:
        raise StepFailed('autobuild in %s is %r, not %s' % (venv, version, AUTOBUILD_VERSION))
    say('autobuild %s: %s' % (version, os.path.join(venv, 'Scripts')))
    return os.path.join(venv, 'Scripts')


def apply_decode_fix(path):
    """DECODE_FIX on the 3.10.2 file; nothing when it is already there."""
    if not os.path.isfile(path):
        raise StepFailed('no %s' % path)
    with open(path, 'rb') as f:
        data = f.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest == SOURCE_ENV_FIXED_SHA256:
        return
    if digest != SOURCE_ENV_SHA256:
        raise StepFailed('%s is not the file of autobuild %s (sha256 %s): not changed' % (path, AUTOBUILD_VERSION, digest))
    for old, new in DECODE_FIX:
        if data.count(old) != 1:
            raise StepFailed('%s: the line to change is not there once: %r' % (path, old))
        data = data.replace(old, new)
    if hashlib.sha256(data).hexdigest() != SOURCE_ENV_FIXED_SHA256:
        raise StepFailed('%s: the changed file is not the expected one: not written' % path)
    with open(path + '.tmp', 'wb') as f:
        f.write(data)
    os.replace(path + '.tmp', path)
    say('autobuild: output of the tools it runs decoded with errors="replace" (%s)' % path)


# ---- build-variables -----------------------------------------------------------

def setup_build_variables(deps):
    repo = os.path.join(deps, 'build-variables')
    git = ['git', '-C', repo]
    if not os.path.isdir(os.path.join(repo, '.git')):
        run(['git', 'init', '-q', repo])
    # The patch is LF, as the file in the repository: no CRLF in the checkout.
    run(git + ['config', 'core.autocrlf', 'false'])
    if output(git + ['cat-file', '-e', BUILD_VARIABLES_COMMIT + '^{commit}']) is None:
        run(git + ['fetch', '-q', '--depth', '1', BUILD_VARIABLES_URL, BUILD_VARIABLES_COMMIT])
    # -f: back to the commit's files (a patch from an earlier run is undone)
    run(git + ['checkout', '-q', '-f', '--detach', BUILD_VARIABLES_COMMIT])
    head = output(git + ['rev-parse', 'HEAD'])
    if head != BUILD_VARIABLES_COMMIT:
        raise StepFailed('build-variables is at %s, not %s' % (head, BUILD_VARIABLES_COMMIT))
    run(git + ['apply', '--whitespace=nowarn', OPTIMIZE_PATCH])
    variables = os.path.join(repo, 'variables')
    with open(variables, encoding='utf-8') as f:
        if not any(line.startswith(OPTIMIZED_LINE) for line in f):
            raise StepFailed('%s has no line starting %s after the patch' % (variables, OPTIMIZED_LINE))
    digest = sha256_file(variables)
    if digest != VARIABLES_SHA256:
        raise StepFailed('%s after the patch has sha256 %s, not %s' % (variables, digest, VARIABLES_SHA256))
    say('build-variables %s with %s' % (BUILD_VARIABLES_COMMIT[:10], os.path.basename(OPTIMIZE_PATCH)))
    return variables


# ---- Vulkan SDK ----------------------------------------------------------------

def include_digest(folder):
    """One SHA-256 over every file in folder: of the sorted lines
    "<path relative to folder, with /> <NUL> <sha256 of the file> <LF>". None without the folder."""
    if not os.path.isdir(folder):
        return None
    lines = []
    for top, dirs, names in os.walk(folder):
        for name in names:
            path = os.path.join(top, name)
            rel = os.path.relpath(path, folder).replace(os.sep, '/')
            lines.append(rel + '\0' + sha256_file(path) + '\n')
    return hashlib.sha256(''.join(sorted(lines)).encode('utf-8')).hexdigest()


def vulkan_sdk_problems(sdk):
    """What is missing or differs in sdk (empty: everything the build reads matches)."""
    problems = []
    include = include_digest(os.path.join(sdk, 'Include'))
    if include is None:
        problems.append('missing Include')
    elif include != VULKAN_INCLUDE_SHA256:
        problems.append('Include differs')
    for rel, digest in VULKAN_SDK_LIBS:
        path = os.path.join(sdk, rel)
        if not os.path.isfile(path):
            problems.append('missing %s' % rel)
        elif sha256_file(path) != digest:
            problems.append('%s differs' % rel)
    return problems


def given_installer(path):
    """The installer at path, when its size and SHA-256 are VULKAN_INSTALLER's."""
    path = os.path.abspath(path)
    if not os.path.isfile(path):
        raise StepFailed('--vulkan-installer: no file %s' % path)
    size, digest = os.path.getsize(path), sha256_file(path)
    if size != VULKAN_INSTALLER_SIZE or digest != VULKAN_INSTALLER_SHA256:
        raise StepFailed('--vulkan-installer %s: %d bytes, sha256 %s; expected %d bytes, sha256 %s (%s)'
                         % (path, size, digest, VULKAN_INSTALLER_SIZE, VULKAN_INSTALLER_SHA256, VULKAN_INSTALLER))
    say('Vulkan SDK installer %s (sha256 %s)' % (path, digest))
    return path


def setup_vulkan(deps, given, installer_copy):
    if given:
        sdk = os.path.abspath(given)
        problems = vulkan_sdk_problems(sdk)
        if problems:
            raise StepFailed('--vulkan-sdk %s is not the %s SDK the releases are built with: %s'
                             % (sdk, VULKAN_SDK_VERSION, ', '.join(problems)))
        say('Vulkan SDK %s: %s' % (VULKAN_SDK_VERSION, sdk))
        return sdk
    sdk = os.path.join(deps, 'VulkanSDK', VULKAN_SDK_VERSION)
    if os.path.isdir(sdk):
        problems = vulkan_sdk_problems(sdk)
        if problems:
            raise StepFailed('%s is there but does not match (%s): remove it and run again'
                             % (sdk, ', '.join(problems)))
        say('Vulkan SDK %s: %s' % (VULKAN_SDK_VERSION, sdk))
        return sdk
    if installer_copy:
        installer = given_installer(installer_copy)
    else:
        installer = os.path.join(deps, 'downloads', VULKAN_INSTALLER)
        try:
            download(VULKAN_INSTALLER_URL, installer, VULKAN_INSTALLER_SIZE, VULKAN_INSTALLER_SHA256, say)
        except DownloadFailed as e:
            raise StepFailed(str(e))
    os.makedirs(os.path.dirname(sdk), exist_ok=True)
    run([installer, '--root', forward(sdk), '--accept-licenses', '--default-answer',
         '--confirm-command', 'install', 'copy_only=1'])
    problems = vulkan_sdk_problems(sdk)
    if problems:
        raise StepFailed('the Vulkan SDK installed in %s does not match: %s' % (sdk, ', '.join(problems)))
    say('Vulkan SDK %s: %s' % (VULKAN_SDK_VERSION, sdk))
    return sdk


# ---- env.sh --------------------------------------------------------------------

def write_env(deps, scripts, variables, sdk):
    lines = ['# Made by setup-deps.py: source it in bash before charis-build.sh.']
    values = (('PATH', bash_path(scripts), True),
              ('AUTOBUILD_VARIABLES_FILE', forward(variables), False),
              ('VULKAN_SDK', forward(sdk), False),
              ('AUTOBUILD_INSTALLABLE_CACHE', forward(os.path.join(deps, 'autobuild-cache')), False))
    for name, value, prepend in values:
        if "'" in value:
            raise StepFailed('a quote in a path: %s' % value)
        lines.append("export %s='%s'%s" % (name, value, ':"$PATH"' if prepend else ''))
    os.makedirs(os.path.join(deps, 'autobuild-cache'), exist_ok=True)
    env = os.path.join(deps, 'env.sh')
    with open(env + '.tmp', 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(env + '.tmp', env)
    say('wrote %s' % env)


def parse_steps(text):
    steps = [s.strip() for s in text.split(',') if s.strip()]
    unknown = [s for s in steps if s not in STEPS]
    if unknown or not steps:
        raise argparse.ArgumentTypeError('steps are some of %s, not %s' % (','.join(STEPS), text))
    return steps


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--print-vulkan-url', action='store_true')
    ap.add_argument('--deps')
    sdk_from = ap.add_mutually_exclusive_group()
    sdk_from.add_argument('--vulkan-sdk')
    sdk_from.add_argument('--vulkan-installer')
    ap.add_argument('--steps', type=parse_steps, default=list(STEPS))
    a = ap.parse_args()
    if a.print_vulkan_url:
        print(VULKAN_INSTALLER_URL)
        return 0
    if not a.deps:
        ap.error('--deps is required')
    deps = os.path.abspath(a.deps)
    low, high = PYTHON_VERSIONS
    if not low <= sys.version_info[:2] <= high:
        say('Python %d.%d: needs %d.%d to %d.%d' % (sys.version_info[:2] + low + high))
        return 1
    if (a.vulkan_sdk or a.vulkan_installer) and 'vulkan' not in a.steps:
        ap.error('--vulkan-sdk or --vulkan-installer without the vulkan step')
    if shutil.which('git') is None and 'build-variables' in a.steps:
        say('git not found (needed for build-variables)')
        return 1
    os.makedirs(deps, exist_ok=True)
    try:
        scripts = setup_autobuild(deps) if 'autobuild' in a.steps else None
        variables = setup_build_variables(deps) if 'build-variables' in a.steps else None
        sdk = setup_vulkan(deps, a.vulkan_sdk, a.vulkan_installer) if 'vulkan' in a.steps else None
        if scripts and variables and sdk:
            write_env(deps, scripts, variables, sdk)
        else:
            say('env.sh not written: it is written when all three steps run')
    except StepFailed as e:
        say('FAILED: %s' % e)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
