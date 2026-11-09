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
rem Builds CTranslate2 (CPU, static, no MKL/oneDNN: RUY int8) and SentencePiece (static) into build\.
rem Sources: third_party\ (not part of this repository; README.md says which tags to fetch).
rem CL=/d1trimfile:<this folder>: the __FILE__ names compiled into the libraries (their asserts and CHECKs, which end
rem up in charis-translate.exe) start at this folder, not at the builder's path. --clean-first: every object is
rem compiled again with it (MSBuild does not see a change of CL).
setlocal
call "%~dp0..\vsenv.bat" || exit /b 1
set HERE=%~dp0
set CL=/d1trimfile:"%HERE%\" %CL%
cmake -S "%HERE%third_party\sentencepiece" -B "%HERE%build\spm" -G "Visual Studio 17 2022" -A x64 -DSPM_ENABLE_SHARED=OFF -DSPM_BUILD_TEST=OFF -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
cmake --build "%HERE%build\spm" --config Release --target sentencepiece-static --clean-first -- /m || exit /b 1
cmake -S "%HERE%third_party\CTranslate2" -B "%HERE%build\ct2" -G "Visual Studio 17 2022" -A x64 -DBUILD_SHARED_LIBS=OFF -DBUILD_CLI=OFF -DWITH_MKL=OFF -DWITH_DNNL=OFF -DWITH_RUY=ON -DOPENMP_RUNTIME=COMP -DWITH_CUDA=OFF -DCMAKE_INSTALL_PREFIX="%HERE%build\ct2-install" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 || exit /b 1
cmake --build "%HERE%build\ct2" --config Release --target install --clean-first -- /m || exit /b 1
endlocal
