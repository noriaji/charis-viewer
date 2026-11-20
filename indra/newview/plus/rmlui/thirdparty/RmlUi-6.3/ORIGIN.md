# RmlUi 6.3 (plus/rmlui, research/364)

| | |
|---|---|
| Project | RmlUi, https://github.com/mikke89/RmlUi |
| Version | tag `6.3` (released 2026-08-22), commit `ba95ffe8bfb6370efb2cdcca927eaad4710c5413` (`git ls-remote https://github.com/mikke89/RmlUi refs/tags/6.3`, 2026-11-16) |
| Taken from | https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.tar.gz, 3,977,268 bytes, SHA-256 `d977298bb6147610e5984d5db85ddf284020d655a8713913f6982074f1dbdede` (2026-11-16; GitHub makes these archives again on demand, so their bytes may change: the files are checked by SHA256SUMS) |
| Licence | MIT (`LICENSE.txt`); the containers in `Include/RmlUi/Core/Containers/` (robin_hood, itlib) MIT too (`Include/RmlUi/Core/Containers/LICENSE.txt`). Their texts are in `plus/licenses-charis.txt` |
| Taken | `Include/` (all of it: Core's headers, and the Debugger, Lua, SVG and Lottie headers, which nothing includes), `Source/Core/` (all of it, its CMakeLists.txt files too, which are not used) and `LICENSE.txt`. Not taken: `Source/Debugger`, `Source/Lua`, `Source/SVG`, `Source/Lottie`, `Backends`, `Samples`, `Tests`, `Dependencies`, `CMake`, `Utilities` |
| Changed | nothing: every file is as released (`SHA256SUMS`, `sha256sum -c SHA256SUMS` in this folder). One file has CRLF line ends (`Source/Core/DecoratorGradient.cpp`); `../.gitattributes` keeps git from rewriting them |
| Built | by `plus/rmlui/rmlui.cmake` as the static library `plus_rmlui_core` (not with RmlUi's own CMake): `RMLUI_STATIC_LIB`, `RMLUI_FONT_ENGINE_FREETYPE` (the viewer's FreeType, 3p-freetype), `RMLUI_NO_FILE_INTERFACE_DEFAULT` (only plus/rmlui's file interface reads files), `RMLUI_VERSION="6.3"`, `/fp:precise` |

## Taking a newer version
1. Fetch the tag's archive, check the tag's commit with `git ls-remote`, note the archive's SHA-256.
2. Replace `Include/`, `Source/Core/` and `LICENSE.txt` here (in a folder named for the new version, and move
   `rmlui.cmake`'s paths and source list to it: the list is the `.cpp` files under `Source/Core/`).
3. Write `SHA256SUMS` again (`<hash> *<path>` for every file but this one and SHA256SUMS) and this file.
4. Compare `Source/Core/CMakeLists.txt` and `Source/Core/FontEngineDefault/CMakeLists.txt` with the definitions
   in `rmlui.cmake`, and the interfaces plus/rmlui implements (`RenderInterface.h`, `SystemInterface.h`,
   `FileInterface.h`, `TextInputHandler.h`, `TextInputContext.h`) with the last version.
