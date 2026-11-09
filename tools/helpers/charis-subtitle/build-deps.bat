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
rem Builds whisper.cpp and ggml (CPU, static) into build\ for build.bat.
rem Source: third_party\whisper.cpp (not part of this repository; README.md says which commit to fetch).
rem CL=/d1trimfile:<this folder>: the __FILE__ names compiled into the libraries (GGML_ASSERT and the like, which end
rem up in charis-subtitle.exe) start at this folder, not at the builder's path. --clean-first: every object is
rem compiled again with it (MSBuild does not see a change of CL).
setlocal
call "%~dp0..\vsenv.bat" || exit /b 1
set HERE=%~dp0
set CL=/d1trimfile:"%HERE%\" %CL%
cmake -S "%HERE%third_party\whisper.cpp" -B "%HERE%build" -G "Visual Studio 17 2022" -A x64 -DWHISPER_BUILD_TESTS=OFF -DGGML_NATIVE=OFF -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DBUILD_SHARED_LIBS=OFF || exit /b 1
cmake --build "%HERE%build" --config Release --target whisper-cli --clean-first -- /m || exit /b 1
endlocal
