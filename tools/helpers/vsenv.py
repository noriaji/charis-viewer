# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Second Life Viewer Source Code
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation;
# version 2.1 of the License only.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
# $/LicenseInfo$

"""Where Visual Studio's vcvars64.bat is, for the helpers' build.bat.

The tools used to name the Community edition's path; Professional, Enterprise and Build Tools live elsewhere.
Order:
  1. CHARIS_VCVARS (a vcvars64.bat)
  2. VCINSTALLDIR (already inside a developer prompt) -> <VCINSTALLDIR>\\Auxiliary\\Build\\vcvars64.bat
  3. vswhere.exe: VS 2022 (17.x, the version the build uses: AUTOBUILD_VSVER=170) with the x64 C++ tools,
     else the newest VS with them
Not found -> VsNotFound with what was tried.

    from vsenv import find_vcvars, vc_env
    python -I tools/helpers/vsenv.py    prints the vcvars64.bat path (exit 1 when not found)
"""
import os
import subprocess
import sys

VC_COMPONENT = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
VCVARS_REL = os.path.join('VC', 'Auxiliary', 'Build', 'vcvars64.bat')


class VsNotFound(Exception):
    pass


def _vswhere():
    pf86 = os.environ.get('ProgramFiles(x86)') or r'C:\Program Files (x86)'
    return os.path.join(pf86, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')


def _vswhere_paths(exe, version=None):
    args = [exe, '-products', '*', '-requires', VC_COMPONENT, '-property', 'installationPath', '-nologo']
    args += ['-version', version] if version else ['-latest']
    try:
        out = subprocess.run(args, capture_output=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    text = out.decode('utf-8', errors='replace') if out else ''
    paths = [l.strip() for l in text.splitlines() if l.strip()]
    if version:
        paths.sort(reverse=True)   # several 17.x editions: a stable choice
    return paths


_FOUND = None


def find_vcvars():
    """The path of vcvars64.bat, or raise VsNotFound."""
    global _FOUND
    if _FOUND:
        return _FOUND
    tried = []
    env = os.environ.get('CHARIS_VCVARS')
    if env:
        if os.path.exists(env):
            _FOUND = env
            return env
        tried.append('CHARIS_VCVARS=%s（無い）' % env)
    vcdir = os.environ.get('VCINSTALLDIR')
    if vcdir:
        p = os.path.join(vcdir, 'Auxiliary', 'Build', 'vcvars64.bat')
        if os.path.exists(p):
            _FOUND = p
            return p
        tried.append('VCINSTALLDIR=%s' % vcdir)
    exe = _vswhere()
    if os.path.exists(exe):
        for version in ('[17.0,18.0)', None):
            for inst in _vswhere_paths(exe, version):
                p = os.path.join(inst, VCVARS_REL)
                if os.path.exists(p):
                    _FOUND = p
                    return p
                tried.append(p)
        tried.append('vswhere: C++ の x64 ツール（%s）の入った VS が見つからない' % VC_COMPONENT)
    else:
        tried.append('vswhere.exe が無い: %s' % exe)
    raise VsNotFound('Visual Studio の vcvars64.bat が見つからない（Visual Studio 2022 の「C++ によるデスクトップ開発」を入れるか、'
                     'CHARIS_VCVARS に vcvars64.bat のパスを設定）。試した所: ' + ' / '.join(tried))


_ENV = None


def vc_env():
    """The environment after vcvars64.bat (captured once; VSLANG=1033 for English messages)."""
    global _ENV
    if _ENV is not None:
        return _ENV
    vcvars = find_vcvars()
    base = dict(os.environ, VSLANG='1033')
    r = subprocess.run('"%s" >nul && set' % vcvars, shell=True, capture_output=True, env=base)
    env = {}
    for line in r.stdout.decode('mbcs', errors='replace').splitlines():
        if '=' in line and not line.startswith('='):
            k, v = line.split('=', 1)
            env[k] = v
    if 'VCToolsInstallDir' not in env:
        raise VsNotFound('vcvars64.bat が開発者環境を作れなかった: %s' % vcvars)
    _ENV = env
    return env


if __name__ == '__main__':
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    try:
        print(find_vcvars())
    except VsNotFound as e:
        print(e, file=sys.stderr)
        sys.exit(1)
