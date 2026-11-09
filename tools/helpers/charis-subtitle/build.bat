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
rem Builds out\charis-subtitle.exe, the speech recognition helper the viewer's voice subtitles start. Run
rem build-deps.bat first. The exe is shipped next to the viewer, so it must not name the builder's folders:
rem /d1trimfile cuts this folder off the __FILE__ names (build-deps.bat does the same for whisper.cpp and ggml), and
rem /PDBALTPATH puts only the pdb's file name in the exe.
setlocal
call "%~dp0..\vsenv.bat" || exit /b 1
set HERE=%~dp0
set WCPP=%HERE%third_party\whisper.cpp
set B=%HERE%build
if not exist "%HERE%out" mkdir "%HERE%out"
cl /nologo /O2 /arch:AVX2 /MD /Zi /EHsc /std:c++17 /utf-8 /W3 /openmp /D_CRT_SECURE_NO_WARNINGS ^
   /d1trimfile:"%HERE%\" ^
   /I"%WCPP%\include" /I"%WCPP%\ggml\include" /Fo"%HERE%out\\" /Fd"%HERE%out\charis-subtitle.pdb" ^
   "%HERE%src\charis_subtitle.cpp" /Fe"%HERE%out\charis-subtitle.exe" ^
   /link "%B%\src\Release\whisper.lib" "%B%\ggml\src\Release\ggml.lib" "%B%\ggml\src\Release\ggml-base.lib" ^
   "%B%\ggml\src\Release\ggml-cpu.lib" user32.lib advapi32.lib /PDBALTPATH:%%_PDB%%
endlocal
