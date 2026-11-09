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
rem
rem Builds out\charis-translate.exe, the local translation helper the viewer starts. Run build-deps.bat first.
rem The exe is shipped next to the viewer, so it must not name the builder's folders: /d1trimfile cuts this folder
rem off the __FILE__ names (build-deps.bat does the same for the libraries), and /PDBALTPATH puts only the pdb's
rem file name in the exe.
setlocal enabledelayedexpansion
call "%~dp0..\vsenv.bat" || exit /b 1
set HERE=%~dp0
set B=%HERE%build
if not exist "%HERE%out" mkdir "%HERE%out"
set LIBS="%B%\ct2-install\lib\ctranslate2.lib" "%B%\spm\src\Release\sentencepiece.lib" "%B%\ct2\third_party\cpu_features\Release\cpu_features.lib"
for /r "%B%\ct2\third_party\ruy" %%f in (*.lib) do set LIBS=!LIBS! "%%f"
cl /nologo /O2 /arch:AVX2 /MT /Zi /EHsc /std:c++17 /utf-8 /W3 /openmp /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX ^
   /d1trimfile:"%HERE%\" ^
   /I"%B%\ct2-install\include" /I"%HERE%third_party\sentencepiece\src" /Fo"%HERE%out\\" /Fd"%HERE%out\charis-translate.pdb" ^
   "%HERE%src\charis_translate.cpp" /Fe"%HERE%out\charis-translate.exe" ^
   /link !LIBS! user32.lib advapi32.lib /PDBALTPATH:%%_PDB%%
endlocal
