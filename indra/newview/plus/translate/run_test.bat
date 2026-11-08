@echo off
rem Builds and runs translate_test.cpp (the language detection and the text splitting) on its own. Needs MSVC 2022.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set OUT=%TEMP%\translate_test_out
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /std:c++17 /utf-8 /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\\" /Fe"%OUT%\translate_test.exe" "%~dp0translate_test.cpp" >nul || exit /b 1
"%OUT%\translate_test.exe"
endlocal
