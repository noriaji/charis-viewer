# Charis Viewer helpers / ヘルパー

Three small programs the viewer starts next to itself, talking to them over their standard input and output
(plus/subtitle `helperprocess.h`). They run on the CPU and send nothing anywhere.

| Folder | Program | What for | Built with |
|---|---|---|---|
| `charis-translate/` | `charis-translate.exe` | local translation (chat, subtitles) | CTranslate2 v4.5.0, SentencePiece v0.2.0 |
| `charis-subtitle/` | `charis-subtitle.exe` | voice subtitles (speech recognition) | whisper.cpp (ggml) |
| `charis-motion/` | `charis-motion.exe` | the motion between two poses in the poser | ONNX Runtime 1.23.2 (`charis-onnxruntime.dll`, Microsoft's build, loaded at run time) |

`helper-common/` holds the small JSON and line reader headers `charis-motion` uses. The protocol of each helper is
written at the top of its `src/*.cpp`.

ビューワが自分の隣から起動する小さなプログラム 3 つです（標準入出力でやり取り、CPU だけで動き、どこにも送信しません）。
`charis-translate.exe` はローカル翻訳、`charis-subtitle.exe` はボイスの字幕（音声認識）、`charis-motion.exe` はポーズ編集での 2 つのポーズの間のモーション生成。
やり取りの決まりは各 `src/*.cpp` の頭にあります。`helper-common/` は `charis-motion` が使う JSON・行読みの小さなヘッダです。

## License / ライセンス

The helpers' own code is under the GNU LGPL 2.1 only, like the viewer (the header of each file). The libraries
built into them keep their own licenses (MIT, Apache 2.0, BSD); their notices are in
`indra/newview/plus/licenses-charis.txt` (section 3), which is shipped as `licenses-charis.txt` next to the viewer.
The translation, Whisper and motion models are not part of this repository or of the release: the viewer fetches them
when asked to, from https://github.com/noriaji/charis-models, where their licenses are listed.

Two parts of `charis-motion` are not LGPL: `src/soma_codec.h` and `src/soma_codec.cpp` are a port of NVIDIA MotionBricks'
pre- and post-processing (Apache License 2.0, NVIDIA's copyright and the "changed" note are in each file's head), and
the joint names and parents in `src/soma_tables.h` are NVIDIA MotionBricks' SOMA skeleton (CC BY 4.0, the credit is in
that file). NVIDIA's sample motions, the data made from them and the model are not in this repository.

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

### charis-motion

```
cd tools\helpers\charis-motion
python -I fetch-onnxruntime.py   REM Microsoft's onnxruntime-win-x64-1.23.2.zip from github.com (size and SHA-256 pinned in the script)
.\build.bat                      REM out\charis-motion.exe, and charis-onnxruntime.dll beside it
```
`charis-onnxruntime.dll` is not linked (there is no import library): the exe loads it from its own folder, and only from
there, at run time. It is Microsoft's signed `onnxruntime.dll` 1.23.2 under another name (the same bytes and
signature), so that Windows 11's own, older `System32\onnxruntime.dll` is never meant; it needs the Visual C++ runtime
(`msvcp140.dll`, `msvcp140_1.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) next to it, as the viewer ships it.
The model (`mmm_v6_soma_fp32.onnx`, 757,864,273 bytes) is fetched by the viewer; its file name and size are fixed in
the helper, so a new model needs a new helper build. The developers' tests (they need the model and reference data
that are not published) are not part of this repository.

### What the release ships / 配布物への入り方

The release workflow does not build the helpers: it takes the published builds from releases of
https://github.com/noriaji/charis-models, pinned by size and SHA-256 in `.github/workflows/release-package.yml`
(`HELPERS_URL`, `HELPER_*`; `indra/newview/plus/building/release/fetch-helpers.py`), with `vcomp140.dll`, the
OpenMP runtime the translation and subtitle helpers use (Microsoft Visual C++ 2022 redistributable), and
`charis-onnxruntime.dll` with its third-party notices for the motion helper. A new build goes to a new release with new
pins; a published file is never replaced. `/d1trimfile` and `/PDBALTPATH` in the bat files keep the builder's
folders out of the exes.

リリースのワークフローはヘルパーをビルドしません。charis-models のリリースに置いたビルドを、大きさと SHA-256 を固定して取り出し
（`release-package.yml` の `HELPERS_URL`・`HELPER_*`、`fetch-helpers.py`）、翻訳・字幕が使う OpenMP のランタイム `vcomp140.dll`、モーションのヘルパーが使う
`charis-onnxruntime.dll`（と第三者の表示）と一緒にビューワの隣に入れます。作り直したら新しいリリースに置いて固定値を変えます（公開したファイルは差し替えません）。
bat の `/d1trimfile`・`/PDBALTPATH` は、ビルドした PC のフォルダ名を exe に残さないためです。

The translation and subtitle helpers need a CPU with AVX2 (`/arch:AVX2`). / 翻訳・字幕のヘルパーは AVX2 のある CPU が要ります。
