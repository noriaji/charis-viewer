# Building Charis Viewer / カリス ビューワのビルド

How the released Windows binaries are built. Charis Viewer is built like the
official Second Life viewer (https://github.com/secondlife/viewer, README,
"Build Instructions"), with the differences below.

配布しているバイナリ（Windows 64 ビット）の作り方です。公式ビューワの手順
（https://github.com/secondlife/viewer の README「Build Instructions」）と同じで、
違いは下のとおりです。

## Quick start / 手順の要約

On a Windows PC with Visual Studio 2022 (it brings CMake), Python 3, Git and Git Bash
(the first rows of the table below), from Git Bash, in a clone of the public
repository in a folder without spaces:

Visual Studio 2022（CMake は付いてくる）・Python 3・Git・Git Bash（下の表の上の行）を入れた Windows で、
Git Bash から、空白を含まないフォルダに clone した公開リポジトリで:

```
mkdir -p C:/charis-deps
curl -fL -o C:/charis-deps/vulkansdk-windows-X64-1.4.363.0.exe "$(python -I indra/newview/plus/building/release/setup-deps.py --print-vulkan-url)"
python -I indra/newview/plus/building/release/setup-deps.py --deps C:/charis-deps --vulkan-installer C:/charis-deps/vulkansdk-windows-X64-1.4.363.0.exe
source C:/charis-deps/env.sh
indra/newview/plus/building/charis-build.sh -- --accept-number-file
```

LunarG refuses downloads by Python's urllib (HTTP 403), so the Vulkan SDK
installer is fetched with `curl` first (or use `--vulkan-sdk <folder>` for an
installed 1.4.363.0, below). The repository's history starts at its first public
commit, so the build number comes from `build-number.txt` (`--accept-number-file`,
item 3 below).
LunarG は Python の urllib からの取得を断る（HTTP 403）ので、Vulkan SDK のインストーラーは先に `curl` で取ります
（入れてある 1.4.363.0 を使うなら `--vulkan-sdk <フォルダ>`。下の 1）。このリポジトリの履歴は最初の公開のコミットから
始まるので、ビルド番号は `build-number.txt` から取ります（`--accept-number-file`。下の 3）。

1. `release/setup-deps.py` fetches autobuild, build-variables and the Vulkan
   SDK at the versions below and checks each of them (pinned hashes, a fixed
   commit, the installer's SHA-256), then writes `env.sh`. Run again with the
   same `--deps` folder, it keeps what is there and checks out. With
   `--vulkan-sdk <folder>` it uses an installed Vulkan SDK instead (its files
   must be those of 1.4.363.0), or with `--vulkan-installer <file>` it installs from
   an installer you fetched yourself (its size and SHA-256 are checked). The Vulkan SDK installer is run with
   `--accept-licenses`: running it this way means you accept LunarG's license
   terms for the SDK. It may ask for administrator rights; it is run with
   `copy_only=1` (files only, no environment variables or registry entries).
   `release/setup-deps.py` が autobuild・build-variables・Vulkan SDK を下の版で取得し、
   それぞれを確かめて（ハッシュ・固定のコミット・インストーラーの SHA-256）、`env.sh` を書きます。
   同じ `--deps` で 2 回目を実行すると、あって正しい物はそのまま使います。`--vulkan-sdk <フォルダ>` で
   入れてある Vulkan SDK を使えます（1.4.363.0 のファイルと一致すること）。`--vulkan-installer <ファイル>` なら、
   自分で取ったインストーラーから入れます（大きさと SHA-256 を確かめる）。Vulkan SDK の
   インストーラーは `--accept-licenses` で実行します。これで LunarG の SDK のライセンスに同意したことに
   なります。管理者の権限を求めることがあります（`copy_only=1` で実行: ファイルだけ、
   環境変数・レジストリは書かない）。
2. `env.sh` puts that autobuild first on `PATH` and sets `AUTOBUILD_VARIABLES_FILE`,
   `VULKAN_SDK` and `AUTOBUILD_INSTALLABLE_CACHE` (the packages of
   `autobuild.xml` are fetched into the `--deps` folder; autobuild checks each
   one against its hash in `autobuild.xml`).
   `env.sh` は、その autobuild を `PATH` の先頭に置き、`AUTOBUILD_VARIABLES_FILE`・`VULKAN_SDK`・
   `AUTOBUILD_INSTALLABLE_CACHE` を設定します（`autobuild.xml` のパッケージは `--deps` のフォルダに
   取得され、autobuild が `autobuild.xml` のハッシュで照合します）。
3. `charis-build.sh` configures, builds and checks the result (below). The
   check, by default, wants a git clone with the history from the official
   viewer (the build number, item 6 below). Otherwise add check options after `--`:
   a clone of the public repository (its history does not reach the official
   viewer; `build-number.txt` is in every commit): `charis-build.sh -- --accept-number-file`;
   a copy without git (the source zip): `charis-build.sh -- --accept-number-file --commit <the full commit id it is a copy of>`.
   Without `build-number.txt`, set `CHARIS_BUILD_NUMBER` before configuring
   instead (and leave out `--accept-number-file`); with neither, configure takes
   the floor and the check stops before building.
   `charis-build.sh` が configure・ビルド・結果の確認を行います（下）。確認は、既定では公式からの履歴を持つ git の clone を
   前提にしています（ビルド番号。下の 6）。そうでない時は `--` の後に確認のオプションを足します:
   公開リポジトリの clone（履歴が公式まで届かない。`build-number.txt` はどのコミットにも入っている）は
   `charis-build.sh -- --accept-number-file`、
   git の無い写し（ソース zip）は `charis-build.sh -- --accept-number-file --commit <元のコミットの完全な ID>`。
   `build-number.txt` が無い時は、代わりに configure の前に `CHARIS_BUILD_NUMBER` を設定します（`--accept-number-file` は
   付けない）。どちらも無いと、configure は下限の番号を取り、確認がビルドの前に止めます。

The full build takes about 20 minutes on a PC with 16 threads and 32 GB of
memory (longer with fewer), plus 4 to 5 minutes for the first configure (the
packages are fetched), and about 10 GB of disk (the build folder with the pdb,
and the packages); keep 20 GB free.
フルビルドは 16 スレッド・メモリ 32 GB の PC で約 20 分（スレッドが少ないともっと掛かる）、初回の configure は
パッケージの取得込みで 4〜5 分、ディスクは約 10 GB（pdb を含むビルドフォルダとパッケージ）。20 GB 空けておきます。

## Tools / 必要な物

| | Version used for the releases / 配布版で使った版 |
|---|---|
| Windows | 10 / 11, 64-bit |
| Visual Studio 2022 (Desktop development with C++) | MSVC 19.44 (toolset v143) |
| CMake | the one Visual Studio 2022 brings (3.31.6-msvc6 for the releases; autobuild finds it, no separate CMake is needed) |
| Python 3 | 3.10 to 3.14 (the autobuild wheels `setup-deps.py` installs cover these) |
| Git, Git Bash | to fetch build-variables and to run `charis-build.sh` |
| Vulkan SDK (LunarG, https://vulkan.lunarg.com/) | **1.4.363.0** (headers, `vulkan-1.lib`, glslang and SPIRV-Tools static libraries) — `setup-deps.py` |
| autobuild | 3.10.2 (`release/requirements-autobuild.txt`) — `setup-deps.py` |
| build-variables (https://github.com/secondlife/build-variables) | commit `f8fad0dbbf6c821e1ae6e254e1ed203e93815a94`, with `optimize-relwithdebinfo.patch` — `setup-deps.py` |

## Differences from the official build / 公式の手順との違い

1. **Vulkan SDK.** `VULKAN_SDK` must point at the SDK folder when configuring
   (`indra/llvulkan/llvulkan.cmake`). Without it the viewer is built without
   its Vulkan renderer.
   configure の時に環境変数 `VULKAN_SDK` が SDK のフォルダを指している必要があります。無いと
   Vulkan の描画なしで組まれます。
2. **Optimized RelWithDebInfo.** The official build-variables build
   RelWithDebInfo without optimization (`/Od /Ob0`). The releases are built
   with `/O2 /Ob2`: `optimize-relwithdebinfo.patch` (this folder) is applied to
   build-variables, and `AUTOBUILD_VARIABLES_FILE` points at its `variables`.
   By hand:
   公式の build-variables では RelWithDebInfo が最適化なし（`/Od /Ob0`）です。配布版は
   `/O2 /Ob2` で組んでいます。このフォルダの `optimize-relwithdebinfo.patch` を
   build-variables に当て、`AUTOBUILD_VARIABLES_FILE` にその `variables` を指定します。手でするなら:
   ```
   git clone https://github.com/secondlife/build-variables
   cd build-variables
   git checkout f8fad0dbbf6c821e1ae6e254e1ed203e93815a94
   git apply <viewer>/indra/newview/plus/building/optimize-relwithdebinfo.patch
   ```
3. **Configuration and options.** Configuration `RelWithDebInfoOS`, with
   `-DUSE_TRACY=OFF -DVIEWER_CHANNEL="Charis Viewer" -DCHARIS_RELEASE_FOR_DOWNLOAD=ON -DUSE_VELOPACK=OFF`
   and release linking (`/debug /INCREMENTAL:NO /OPT:REF` for
   `CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO`, `..._SHARED_...` and `..._MODULE_...`).
   `CHARIS_RELEASE_FOR_DOWNLOAD` defines `LL_RELEASE_FOR_DOWNLOAD`, as the official releases do
   (`indra/cmake/00-Common.cmake`); without it you get a developer build
   (Develop menu, several viewers at once, the developer-only box on a fatal error).
   Without incremental linking, calls do not go through a jump table; the pdb stays for crash reports.
   構成は `RelWithDebInfoOS`、オプションは `-DUSE_TRACY=OFF -DVIEWER_CHANNEL="Charis Viewer" -DCHARIS_RELEASE_FOR_DOWNLOAD=ON -DUSE_VELOPACK=OFF`
   と配布用のリンク（`/debug /INCREMENTAL:NO /OPT:REF`。EXE・SHARED・MODULE の RelWithDebInfo）です。
   `CHARIS_RELEASE_FOR_DOWNLOAD` で、公式の配布版と同じく `LL_RELEASE_FOR_DOWNLOAD` が付きます。無いと開発用の
   ビルドになります（Develop メニューが出る、複数同時に起動できる、など）。増分リンクをしないと関数の呼び出しが
   ジャンプ表を通りません。pdb はクラッシュの解析のために残します。
4. **Environment.** `AUTOBUILD_VSVER=170`, `AUTOBUILD_ADDRSIZE=64`, `VSLANG=1033`
   (English tool output, which autobuild reads),
   `CL=-utf-8` (the sources are UTF-8; needed on Windows with a non-UTF-8 code
   page such as Japanese, and set for the releases), `PYTHONUTF8=1`.
   On Windows with a non-UTF-8 code page, autobuild 3.10.2 may fail to decode
   the output of the tools it runs; `setup-deps.py` changes its autobuild to
   decode it with `errors="replace"` (output only, no effect on the build).
   The build's Python scripts run with the `python` on `PATH` (or the one the
   environment variable `PYTHON` names), given to CMake as both
   `Python3_EXECUTABLE` and `PYTHON_EXECUTABLE`; it must import `llsd`, which
   comes with autobuild (the venv of `env.sh`). Otherwise CMake takes the Python
   in the registry, and the package step fails when that one has no `llsd`.
   `VSLANG=1033`（道具の出力を英語に）。日本語版 Windows では `CL=-utf-8` と `PYTHONUTF8=1` が要ります（配布版はこれで組んで
   います）。autobuild 3.10.2 が道具の出力（cp932 など）を読めずに止まることがあるため、`setup-deps.py` が入れる
   autobuild は出力の読み取りを `errors="replace"` にしています（ビルドの中身には影響しません）。
   ビルド中の Python スクリプトは `PATH` の `python`（または環境変数 `PYTHON` の物）で動かします。CMake に
   `Python3_EXECUTABLE` と `PYTHON_EXECUTABLE` の両方として渡します。`llsd` を import できること（autobuild に付いて
   くる。`env.sh` の venv）。渡さないとレジストリの Python が選ばれ、そこに `llsd` が無いとパッケージの段で止まります。
5. **A path without spaces.** The resource compiler fails on source paths with
   spaces; put the source in a folder without spaces (or `subst` a drive letter
   to it).
   ソースは空白を含まないパスに置きます（rc.exe が失敗するため。`subst` でドライブ文字を
   割り当ててもよい）。
6. **Build number.** Charis's build number is the count of the commits since
   it branched from the official viewer (`indra/newview/plus/version/buildnumber.cmake`;
   shown as "rev N"), not autobuild's `AUTOBUILD_BUILD_ID` or the official `revision`. The source
   zip has no git history and carries the number in
   `indra/newview/plus/version/build-number.txt`. To set it yourself, set the
   environment variable `CHARIS_BUILD_NUMBER` before configuring. Clone with the whole
   history (no `--depth`; in GitHub Actions, `actions/checkout` with `fetch-depth: 0`): a shallow
   clone does not reach the branch point, and the number falls back to a fixed floor (`CHARIS_BUILD_NUMBER_FLOOR`) with a warning,
   which can be lower than a number already released.
   ビルド番号は、公式から分かれてからのコミット数です（画面では「rev N」。`AUTOBUILD_BUILD_ID` や公式の
   `revision` は使いません）。ソース zip には git の履歴が無いため、番号は
   `indra/newview/plus/version/build-number.txt` に入っています。自分で決める時は、
   configure の前に環境変数 `CHARIS_BUILD_NUMBER` を設定します。clone は履歴を全部取ります
   （`--depth` を付けない。GitHub Actions なら `actions/checkout` に `fetch-depth: 0`）。浅い clone は
   分かれた所まで届かず、番号は警告つきで決まった下限（`CHARIS_BUILD_NUMBER_FLOOR`）になり、
   配った番号より小さくなることがあります。

`charis-build.sh` in this folder checks all of this, runs the two autobuild
steps and checks the result (environment variables 1, 2 and autobuild from
`env.sh` or set by you; 3 and 4 by the script). Configuring with a bare `cmake`
call, or skipping `charis-build.sh`, bypasses these checks (Vulkan SDK version,
optimization flags, `LL_RELEASE_FOR_DOWNLOAD`, ...): the build may come out
different from the released binaries without any error.
このフォルダの `charis-build.sh` が上をまとめて確かめ、autobuild の 2 段を実行し、結果を確かめます
（1・2 の環境変数と autobuild は `env.sh` か自分で設定、3・4 はスクリプトが設定）。`charis-build.sh` を通さず
素の `cmake` で configure すると、これらの確認（Vulkan SDK のバージョン、最適化フラグ、
`LL_RELEASE_FOR_DOWNLOAD` など）が働かず、エラーも出ないまま配布版と違う組み方になります。

```
charis-build.sh [--build-dir NAME] [--velopack on|off] [configure | build | check] [-- check options]
```

| | |
|---|---|
| (no step) | configure, build, check / configure・ビルド・確認 |
| `configure` | configure, then the configure part of the check (stops before building when it is not a release configuration) / configure と、その分の確認（配布用でなければビルドの前に止まる） |
| `build` | build (after a configure), then check / ビルド（configure の後）と確認 |
| `check` | check the build folder only / ビルドフォルダの確認だけ |
| `--build-dir NAME` | the build folder, directly in the checkout (default `build-vc170-64`) / ビルドフォルダ（ソースの直下。既定 `build-vc170-64`） |
| `--velopack on\|off` | `USE_VELOPACK` (default `off`); `on`: the package step also writes `velopack-pack.json`, the arguments for `vpk pack`, which is run later, after the shader bundle (the package step does not need `vpk`) / 既定 `off`。`on` はパッケージの段で `velopack-pack.json`（`vpk pack` の引数）も書く。`vpk pack` はシェーダーの束を作った後に走らせる（パッケージの段に `vpk` は要らない） |
| `-- ...` | passed to `release/check-release-build.py`, e.g. `--accept-number-file` (a checkout without git history whose `build-number.txt` is kept up to date), `--commit <id>` (a copy without git) / `release/check-release-build.py` へ渡す。例 `--accept-number-file`（git の履歴が無く、`build-number.txt` を毎回入れているソース）、`--commit <ID>`（git の無い写し） |

## The check / 確認

`release/check-release-build.py` (run by `charis-build.sh`, or by itself with
`--build-dir`) prints one OK / NG / WARN line per item and ends with
`RELEASE-CHECK: OK` or `NG`:
`release/check-release-build.py`（`charis-build.sh` が実行。単独なら `--build-dir` を付けて）が、
項目ごとに OK・NG・WARN の行を出し、最後に `RELEASE-CHECK: OK` か `NG` を出します:

- CMakeCache: no Tracy, the channel, `/O2`, the source folder, `CHARIS_RELEASE_FOR_DOWNLOAD`, `USE_VELOPACK`, release linking
- the projects: no `TRACY_ENABLE`, `LL_RELEASE_FOR_DOWNLOAD=1`, `LL_VULKAN=1` (built with the Vulkan SDK), no incremental linking
- the build number: from git or `CHARIS_BUILD_NUMBER`, not the floor
- the output: the viewer exe and the package step's file list, no Tracy files or strings, no developer-build strings, no Tracy symbols in the pdb
- Velopack (`--velopack on`): `velopack-pack.json` from the same package step, with the package id of `plus/independence/independence.h`, the viewer exe, `--noPortable` and `--runtime win-x64` (with `off`, a left-over one is a WARN)
- the checkout: the commit the build started from, nothing changed or untracked, no source file changed during the build (a copy without git needs `--commit`; a checkout with git where git fails is NG)

When every item passes, it writes `charis-release-build.txt` in the build
folder (the commit, the exe's SHA-256, the build number, the options). A
release is made only from a build with that record.
すべて通ると、ビルドフォルダに `charis-release-build.txt`（コミット、exe の SHA-256、ビルド番号、オプション）を
書きます。配布物はこの記録のあるビルドからだけ作ります。

## Output / 出力

`<build folder>/newview/RelWithDebInfo/`. The release zip holds the files the
packaging step lists for the installer (`secondlife_setup_tmp.nsi` in that
folder) except `llplugin/media_plugin_example.dll`, with `licenses-charis.txt`
and the `licenses/` folder (the license file of each bundled package), and a
README. The viewer is `CharisViewer.exe` (the build links `secondlife-bin.exe` and the
packaging step copies it to that name; `SecondLifeViewer.exe` in builds packaged
before the rename).
In the build folder: `charis-configure.log`, `charis-build.log` (the output
of the two steps), `charis-build-start.txt` and `charis-release-build.txt`.

出力は `<ビルドフォルダ>/newview/RelWithDebInfo/` です。配布 zip には、パッケージの段が
インストーラー用に書き出す一覧（同じフォルダの `secondlife_setup_tmp.nsi`）のファイル
（`llplugin/media_plugin_example.dll` を除く）と、`licenses-charis.txt`、`licenses/`
フォルダ（同梱ライブラリごとのライセンス文）、README が入っています。ビューワは `CharisViewer.exe` です
（名前を変える前のビルドでは `SecondLifeViewer.exe`）。
ビルドフォルダには `charis-configure.log`・`charis-build.log`（2 段の出力）・`charis-build-start.txt`・
`charis-release-build.txt` ができます。

## Release workflow / リリースのワークフロー（GitHub Actions）

`.github/workflows/release.yml` builds a release on GitHub's Windows runners and
`release-package.yml` (called by it) does the work, in three jobs. **build**:
`setup-deps.py`, `charis-build.sh --velopack on -- --accept-number-file`, the release
check, the symbols zip (`release/make-release-zip.py --symbols-only`), and the
hand-over (`release/job-handoff.py`: one tar of the listed release files without a
shader bundle, the file list, the stamp, `velopack-pack.json` and the icon it names; no pdb), passed on as
artifacts kept for 1 day with their SHA-256 as job outputs. **bake** (a job of its own,
with the bake tools only, no packaging tool, no secret): the shader bundle
(`release/bake-shaders-ci.sh`: the viewer bakes it on Mesa's lavapipe, a software
Vulkan driver fetched from github.com and checked by SHA-256; on the throwaway runner
only, it is registered for the Vulkan loader in HKLM and the Vulkan runtime is
installed if missing; if the job's copy of the build changes while Mesa runs, every
build stops; any other failure stops a release, and a test build goes on without the
bundle, with a warning; the bake record is kept as an artifact for 3 days); only the
bundle and its manifest leave the job. **package** (Mesa never runs here): takes the
bundle first and reads it back without running anything in it (`release/shaderbundle.py
check`: size, form, keys in place, an allow list of what glslang puts in the SPIR-V,
reflections and GL views the viewer reads, this commit, the manifest), then the tar and
the symbols zip only when their SHA-256 are the build job's, then
`release/make-release-zip.py` (the zip, and the same files staged for vpk),
`release/velopack-release.py` (`vpk pack`: the Setup.exe with the icon the build job
handed over, the full package, and `releases.win.json` with the addresses of this tag's
assets), the source zip (`git archive`), a build record and `SHA256SUMS.txt`, whose
SHA-256 goes to the publish job as a job output (and to the run's summary).
`release.yml` の呼ぶ `release-package.yml` が 3 つのジョブで作ります。**build**: 依存の取得・ビルドと確認・symbols zip
（`make-release-zip.py --symbols-only`）・受け渡し（`job-handoff.py`。配布のファイル〈束なし〉・ファイルの一覧・スタンプ・
`velopack-pack.json` とそれが指すアイコンの tar。pdb は入れない）を成果物（1 日）に、SHA-256 をジョブの出力に。**bake**（専用のジョブ。焼き込みの
道具だけで、包む道具もシークレットも持たない）: シェーダーの束（`release/bake-shaders-ci.sh`。github.com から取って SHA-256 で
照合した Mesa の lavapipe〈ソフトの Vulkan〉でビューワが焼く。使い捨てのランナーの上でだけ、Vulkan ローダー向けに HKLM へ登録し、
無ければ Vulkan のランタイムを入れる。焼いている間にこのジョブのビルドの写しが変われば、どのビルドも止まる。それ以外の失敗は
リリースなら止まり、試験ビルドは警告つきで束なし。焼き込みの記録は成果物として 3 日残る）。ジョブから出るのは束と manifest だけ。
**package**（Mesa は一度も動かない）: 束を最初に取り、中身を動かさずに読み直して確かめ（`shaderbundle.py check`: 大きさ・形式・
キーの位置・SPIR-V の許可リスト・reflection と GL view・コミット・manifest）、tar と symbols zip は build の SHA-256 と一致した時だけ
受け取り、zip・vpk（build から渡したアイコンの Setup.exe、更新のパッケージ、このタグの資産を指す `releases.win.json`）・ソース zip・
ビルドの記録・`SHA256SUMS.txt` を作ります。`SHA256SUMS.txt` の SHA-256 はジョブの出力で公開のジョブへ（実行の要約にも出す）。

| Trigger / きっかけ | What happens / 中身 |
|---|---|
| a tag `v*` / タグ | waits for approval in the environment `release` (the job stops if the environment requires no reviewer), builds (read-only) and hands the assets to the publish job as an artifact kept for 1 day; the publish job checks every name and SHA-256, that the tag still names the built commit and that the commit is on the default branch, attests the provenance and makes a **draft** release; publishing it is done by hand / 環境 `release` の承認（承認者が設定されていなければ止まる）→ ビルド（読み取りのみ）→ 資産を成果物（1 日）で公開のジョブへ → 名前と SHA-256、タグが今もビルドしたコミットを指すか、既定のブランチにあるかを確かめ、来歴を記録して Release を**下書き**で作る。公開は手で |
| workflow_dispatch / 手動 | a test build: the same steps, no release; the zip is kept as an artifact for 3 days / 試験ビルド。Release は作らず、zip を 3 日だけ成果物に |

Permissions: `permissions: {}` at the top; the approval job `actions: read` (to read
the environment's rules), the build, bake and package jobs `contents: read`, and only the publish job, which
runs nothing from the build, `contents: write`, `id-token: write` and
`attestations: write`. Only `GITHUB_TOKEN`; no secrets (the releases are not signed).
Only GitHub's own actions, each pinned by a commit SHA. Caches (the autobuild packages,
the checked Vulkan SDK) are written by these runs only.
権限は冒頭 `{}`、ジョブごとに最小。ビルドはタグでも読み取りだけで、書き込みと来歴の権限はビルドの物を
何も走らせない公開のジョブだけが持ちます。シークレットは使いません（署名なし）。アクションは GitHub 製だけを
コミット SHA で固定。キャッシュを書くのはタグと手動の実行だけです。

The Vulkan SDK comes from LunarG's own address with `curl` as it is, checked by size
and SHA-256 (`setup-deps.py --vulkan-installer`), then cached; a run with the SDK in
the cache (checked again file by file) does not download it. When LunarG refuses the
download and the cache is empty (an entry unused for 7 days is dropped), the run stops:
run it again later.
Vulkan SDK は LunarG のアドレスから `curl`（そのまま）で取り、大きさと SHA-256 を確かめてからキャッシュに入れます。
キャッシュにある時（毎回ファイルごとに照合）は取りに行きません。LunarG に断られ、キャッシュも空（7 日使わないと
消える）なら止まります。時間をおいて実行し直してください。

Before the first run / 最初に動かす前に:

1. Every `uses:` is pinned to the full commit SHA of the version in its comment, and
   `VPK_VERSION` / `VPK_NUPKG_SHA512` are set in `release-package.yml`. To move to a
   new version: take the SHA with `gh api repos/actions/<name>/git/ref/tags/<version>`
   (for an annotated tag, the commit it points to); for vpk, use the version of the
   Velopack library in `autobuild.xml` and the `packageHash` of its nuget.org catalog
   entry. `python -I indra/newview/plus/building/release/check-workflows.py` must print
   `WORKFLOW-CHECK: OK` (without `--allow-todo`).
   各 `uses:` はコメントの版のコミット SHA に固定済み、`VPK_VERSION`・`VPK_NUPKG_SHA512` も設定済み。
   版を上げる時は上の方法で取り直し、`check-workflows.py`（`--allow-todo` なし）で `WORKFLOW-CHECK: OK`。
2. Repository settings: Actions allowed for GitHub's own actions only; the default
   `GITHUB_TOKEN` read-only; approval for workflows from outside collaborators; the
   environment `release` with the maintainer as required reviewer and `v*` tags as
   its only deployment refs (the approval job checks that a reviewer is required);
   "Prevent self-review" stays off (the reviewer and the one who tags are the same
   maintainer, who could not approve otherwise); a ruleset for `v*` tags that lets only
   the maintainer create, update or delete them and blocks force pushes; Immutable
   releases on (a published release's assets and tag cannot be changed afterwards;
   drafts can).
   リポジトリの設定: GitHub 製のアクションだけを許可、既定の `GITHUB_TOKEN` は読み取りのみ、外部の協力者の実行は
   承認が要る、環境 `release`（必須の承認者、対象は `v*` のタグだけ。承認のジョブが承認者の有無を確かめる）。
   環境の「Prevent self-review」はオフのまま（承認者とタグを打つ人が同じ管理者なので、オンだと承認できない）。
   `v*` のタグのルールセット（作成・更新・削除は管理者だけ、force push の禁止）、Immutable releases を有効に
   （公開した Release の資産とタグは後から変えられない。下書きは変えられる）。

   Workflow artifacts of a public repository can be downloaded by anyone signed in to
   GitHub: the tag's assets (about 1.3 GB, kept 1 day), the jobs' hand-overs (the
   build's tar and symbols zip, the bundle; 1 day) and a test build's zip (3 days) are in
   effect public before anyone has looked at the draft.
   公開リポジトリの成果物は、GitHub にログインした誰でも取れます。タグの資産（約 1.3 GB、1 日）、ジョブ間の受け渡し
   （ビルドの tar と symbols zip、束。1 日）と試験の zip（3 日）は、下書きを人が見る前に実質公開されます。
3. `indra/newview/plus/version/build-number.txt` is in the published tree (the
   workflow stops without it). `.gitignore` excepts it from `build-*`, so `git add`
   takes it.
   公開する木に `build-number.txt` が入っていること（無いとワークフローが止まる）。`.gitignore` の `build-*` から
   外してあるので `git add` で入ります。

## Not in the source / ソースに含まれない物

The voice subtitles (plus/subtitle) start a separate helper program,
`charis-subtitle.exe`, with a Whisper model. Neither is part of this source or
of the release zip.
ボイスの字幕（plus/subtitle）は、別のヘルパー `charis-subtitle.exe` と Whisper の
モデルを使います。どちらもこのソースと配布 zip には入っていません。
