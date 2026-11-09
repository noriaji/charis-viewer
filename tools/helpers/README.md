# Charis Viewer helpers / ヘルパー

Two small programs the viewer starts next to itself, talking to them over their standard input and output
(plus/subtitle `helperprocess.h`). They run on the CPU and send nothing anywhere.

| Folder | Program | What for | Built with |
|---|---|---|---|
| `charis-translate/` | `charis-translate.exe` | local translation (chat, subtitles) | CTranslate2 v4.5.0, SentencePiece v0.2.0 |
| `charis-subtitle/` | `charis-subtitle.exe` | voice subtitles (speech recognition) | whisper.cpp (ggml) |

The protocol of each is written at the top of its `src/*.cpp`.

ビューワが自分の隣から起動する小さなプログラム 2 つです（標準入出力でやり取り、CPU だけで動き、どこにも送信しません）。
`charis-translate.exe` はローカル翻訳、`charis-subtitle.exe` はボイスの字幕（音声認識）。やり取りの決まりは各 `src/*.cpp` の頭にあります。

## License / ライセンス

The helpers' own code is under the GNU LGPL 2.1 only, like the viewer (the header of each file). The libraries
built into them keep their own licenses (MIT, Apache 2.0, BSD); their notices are in
`indra/newview/plus/licenses-charis.txt` (section 3), which is shipped as `licenses-charis.txt` next to the viewer.
The translation and Whisper models are not part of this repository or of the release: the viewer fetches them when
asked to, from https://github.com/noriaji/charis-models, where their licenses are listed.

ヘルパー自身のコードはビューワと同じ GNU LGPL 2.1 only です。中に入るライブラリはそれぞれのライセンス（MIT・Apache 2.0・BSD）のままで、
表記は `indra/newview/plus/licenses-charis.txt` の第 3 節（配布物の `licenses-charis.txt`）にあります。モデルはこのリポジトリにも配布物にも
入りません（ビューワが求められた時に取得します）。

## Building / ビルド

Windows x64, Visual Studio 2022 with "Desktop development with C++" (MSVC 14.44, the toolset of the release
workflow), CMake 3.21 or later, Git, Python 3. `vsenv.bat` here finds Visual Studio (or set `CHARIS_VCVARS` to
its `vcvars64.bat`). The third-party sources go into `<helper>/third_party/`, the builds into `build/` and `out/`
(all ignored by Git).

### charis-translate

```
cd tools\helpers\charis-translate
git clone --depth 1 --branch v4.5.0 --recurse-submodules --shallow-submodules https://github.com/OpenNMT/CTranslate2 third_party/CTranslate2
git clone --depth 1 --branch v0.2.0 https://github.com/google/sentencepiece third_party/sentencepiece
.\build-deps.bat      REM CTranslate2 (CPU, RUY int8, no MKL/oneDNN) and SentencePiece, static (/MT)
.\build.bat           REM out\charis-translate.exe
```
CTranslate2 v4.5.0 is commit 383d063daf0bf338a22b491864cb2018eb8efd15 (its submodules: ruy 363f2522,
cpu_features 8a494eb1, spdlog 76fb40d9); SentencePiece v0.2.0 is commit 17d7580d6407802f85855d2cc9190634e2c95624.

Test (`python test_helper.py`): needs models in `models\pack\<from>-<to>\` (a CTranslate2 model with
`source.spm` and `target.spm`), for example the ones the viewer fetched (its `subtitle\translate` folder in the
viewer's settings folder) copied there.

### charis-subtitle

```
cd tools\helpers\charis-subtitle
git clone https://github.com/ggml-org/whisper.cpp third_party/whisper.cpp
git -C third_party/whisper.cpp checkout 231638a0717b7822784297e5980965ffcec05810
.\build-deps.bat      REM whisper.cpp and ggml, static (AVX2, FMA, F16C; no native tuning)
.\build.bat           REM out\charis-subtitle.exe
```
whisper.cpp 231638a0717b7822784297e5980965ffcec05810 (2026-10-19; version 1.9.5-dev, ggml 0.27.0) is the
commit the released helper is built from.

Test (`python test_helper.py FILE.wav [--lang ja] [--realtime]`): needs a Whisper model at
`models\ggml-small.bin` (the one the voice subtitles window fetches, or `--model PATH`).

### What the release ships / 配布物への入り方

The release workflow does not build the helpers: it takes the published builds from a release of
https://github.com/noriaji/charis-models, pinned by size and SHA-256 in `.github/workflows/release-package.yml`
(`HELPERS_URL`, `HELPER_*`; `indra/newview/plus/building/release/fetch-helpers.py`), with `vcomp140.dll`, the
OpenMP runtime both use (Microsoft Visual C++ 2022 redistributable). A new build goes to a new release with new
pins; a published file is never replaced. `/d1trimfile` and `/PDBALTPATH` in the bat files keep the builder's
folders out of the exes.

リリースのワークフローはヘルパーをビルドしません。charis-models のリリースに置いたビルドを、大きさと SHA-256 を固定して取り出し
（`release-package.yml` の `HELPERS_URL`・`HELPER_*`、`fetch-helpers.py`）、両方が使う OpenMP のランタイム `vcomp140.dll` と一緒に
ビューワの隣に入れます。作り直したら新しいリリースに置いて固定値を変えます（公開したファイルは差し替えません）。
bat の `/d1trimfile`・`/PDBALTPATH` は、ビルドした PC のフォルダ名を exe に残さないためです。

Both helpers need a CPU with AVX2 (`/arch:AVX2`). / どちらも AVX2 のある CPU が要ります。
