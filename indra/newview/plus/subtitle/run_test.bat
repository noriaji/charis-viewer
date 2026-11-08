@echo off
rem Builds and runs subtitle_test.cpp (the ring and the converter) on its own. Needs MSVC 2022.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set OUT=%TEMP%\subtitle_test_out
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++17 /utf-8 /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\\" /Fe"%OUT%\subtitle_test.exe" "%~dp0subtitle_test.cpp" >nul || exit /b 1
"%OUT%\subtitle_test.exe"
endlocal
