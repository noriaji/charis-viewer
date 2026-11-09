@echo off
rem $LicenseInfo:firstyear=2026&license=viewerlgpl$
rem Second Life Viewer Source Code
rem
rem This library is free software; you can redistribute it and/or
rem modify it under the terms of the GNU Lesser General Public
rem License as published by the Free Software Foundation;
rem version 2.1 of the License only.
rem
rem This library is distributed in the hope that it will be useful,
rem but WITHOUT ANY WARRANTY; without even the implied warranty of
rem MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
rem Lesser General Public License for more details.
rem
rem You should have received a copy of the GNU Lesser General Public
rem License along with this library; if not, write to the Free Software
rem Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
rem $/LicenseInfo$
rem Puts Visual Studio's x64 developer environment into the caller.
rem   call "%~dp0vsenv.bat" || exit /b 1        (from tools\helpers\);  call "%~dp0..\vsenv.bat" from a subfolder
rem No setlocal here: the environment of vcvars64.bat must reach the caller.
rem cl already on PATH (a developer prompt): nothing to do. Else vcvars64.bat from tools\helpers\vsenv.py
rem (CHARIS_VCVARS, VCINSTALLDIR, vswhere: VS 2022 first, any edition, else the newest VS with the C++ tools).
where cl >nul 2>nul && exit /b 0
set "CHARIS_VSENV_BAT="
for /f "delims=" %%I in ('python -I "%~dp0vsenv.py"') do set "CHARIS_VSENV_BAT=%%I"
if not defined CHARIS_VSENV_BAT (
    echo vsenv: Visual Studio 2022 ^(C++ x64 tools^) not found. Install "Desktop development with C++", or set CHARIS_VCVARS to vcvars64.bat 1>&2
    exit /b 1
)
call "%CHARIS_VSENV_BAT%" >nul 2>nul
where cl >nul 2>nul || (
    echo vsenv: %CHARIS_VSENV_BAT% did not put cl on PATH 1>&2
    exit /b 1
)
exit /b 0
