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
rem Builds out\charis-motion.exe and puts charis-onnxruntime.dll (Microsoft's onnxruntime.dll under that name) beside it. Run "python -I fetch-onnxruntime.py" first.
rem charis-onnxruntime.dll is not linked (no import library): the exe loads it from its own folder at run time.
rem The exe is shipped next to the viewer, so it must not name the builder's folders: /d1trimfile cuts this folder
rem and ..\helper-common off the __FILE__ names, and /PDBALTPATH puts only the pdb's file name in the exe.
setlocal
call "%~dp0..\vsenv.bat" || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\helper-common") do set COMMON=%%~fI
set ORT=%HERE%third_party\onnxruntime-win-x64-1.23.2
if not exist "%ORT%\include\onnxruntime_cxx_api.h" (
    echo build: no ONNX Runtime headers, run "python -I fetch-onnxruntime.py" first 1>&2
    exit /b 1
)
if not exist "%HERE%out" mkdir "%HERE%out"
cl /nologo /O2 /MT /Zi /EHsc /std:c++17 /utf-8 /W4 /permissive- /guard:cf /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /d1trimfile:"%HERE%\" /d1trimfile:"%COMMON%\\" ^
   /I"%COMMON%" /I"%ORT%\include" /Fo"%HERE%out\\" /Fd"%HERE%out\charis-motion.pdb" ^
   "%HERE%src\charis_motion.cpp" "%HERE%src\soma_codec.cpp" /Fe"%HERE%out\charis-motion.exe" ^
   /link /GUARD:CF /DYNAMICBASE /NXCOMPAT /INCREMENTAL:NO /OPT:REF /OPT:ICF /PDBALTPATH:%%_PDB%% || exit /b 1
rem Microsoft's onnxruntime.dll is shipped as charis-onnxruntime.dll (same bytes, so its signature stays valid): Windows 11 has its own, older onnxruntime.dll in System32 that other programs load by name.
copy /y "%ORT%\lib\onnxruntime.dll" "%HERE%out\charis-onnxruntime.dll" >nul || exit /b 1
endlocal
