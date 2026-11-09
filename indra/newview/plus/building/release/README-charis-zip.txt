Charis Viewer ${shown} (Windows 64-bit)
build: ${commit}
release build (charis-build.sh, profiler off), checked ${checked}

======================================================================
日本語
======================================================================

カリス ビューワは Second Life 用のビューワです（アルファ版＝試験版）。
これは zip 版です。インストールせず、展開するだけで使えます。
自動では更新されません。新しい版は配布ページから入手して、新しいフォルダに展開してください
（自動で更新したい時はインストーラー版 Setup.exe を使ってください）。

■ 必要な物
- Windows 10 / 11（64 ビット）
- Vulkan 1.3 に対応した GPU と、新しいグラフィックドライバー
  目安: NVIDIA GeForce GTX 10 番台（Pascal 世代）以降、
        AMD Radeon RX 400 番台以降、Intel 第 6 世代 Core（Skylake）以降の内蔵 GPU
- Vulkan の拡張機能 depth clip control（VK_EXT_depth_clip_control）に対応したドライバー
  ドライバーが古いと、この機能に対応していないことがあります。GPU のメーカーのサイトで
  最新にしてください。Intel 第 6〜10 世代の内蔵 GPU は確かめていません。
  最新のドライバーでも対応していない場合があり、その時は起動できません。
  目安より古い GPU は対象外です。足りない時は、起動時に足りない機能を示す案内が出ます。

■ 展開
- zip を自分のユーザーのフォルダの中に展開してください（例: %LOCALAPPDATA%\Programs\CharisViewer）。
- C:\ などドライブの直下には置かないでください。同じ PC のほかのアカウントから、中の DLL を
  書き換えられることがあります。
- 公式ビューワのフォルダ（C:\Program Files\SecondLifeViewer など）には展開しないでください。
- インストーラー版の入れ先（%LOCALAPPDATA%\CharisViewerApp）にも展開しないでください。
- ダウンロードした zip は、展開する前に右クリック →「プロパティ」→「許可する」に
  チェックを入れておくと、起動時の警告が出にくくなります。
- 公式ビューワなどのポータブル版のフォルダ（.portable というファイルがある所）の中には展開しないでください。
  設定が下の「設定の場所」ではなく、その中の UserData に入り、消す時に見落としやすくなります。

■ 起動
- 展開したフォルダの ${exe} をダブルクリックします。
- 「Windows によって PC が保護されました」と出たら、「詳細情報」→「実行」で起動できます。
- ウイルス対策ソフトが、${exe} などをウイルスとして誤って検出することがあります
  （署名の無い、新しいプログラムで起きやすいためです）。配布ページから入手した物なら誤検出です。
  そのソフトの除外（例外）にフォルダを加えるか、ソフトのメーカーに誤検出として報告してください。
- 初めての起動は、描画の準備（シェーダーの変換）に時間がかかることがあります。

■ 設定の場所
- 設定・ログ・アカウントごとの設定: %APPDATA%\CharisViewer
- キャッシュ: %LOCALAPPDATA%\CharisViewer
- Vulkan のシェーダーの控え: キャッシュの中の vk_shader_cache（%LOCALAPPDATA%\CharisViewer\vk_shader_cache）
  以前の版が使っていた %LOCALAPPDATA%\SecondLife\vk_shader_cache は、もう使いません。消してかまいません。
- インストーラー版と同じ設定・キャッシュを使います。zip 版とインストーラー版を同時に起動しないでください。
- 初めて起動したとき、公式ビューワや Firestorm などがこの PC にあれば、ログイン画面で設定を引き継ぐか尋ねます。
  あとから［初期設定］→［セットアップ］の「他のビューワの設定を引き継ぐ…」でも引き継げます。
  相手のビューワのファイルは読むだけで、書き換えません。公式ビューワと同時に入れておいても大丈夫です。

■ ブラウザの SLURL リンク（secondlife://）
- カリス ビューワは、展開しただけでは secondlife:// のリンクを登録しません（公式ビューワなどの登録を
  勝手に変えません）。ブラウザのリンクをカリスで開きたい時は、［環境設定］→［セットアップ］の
  ［このビューワで開く］を押します。お使いの Windows ユーザーの設定だけを変え、管理者権限は要りません。
  ［元に戻す］で、それまでのビューワに戻せます。
- maps.secondlife.com の「この場所を訪問」などを押すと、ブラウザが「Charis Viewer を開きますか？」と
  確認します。「常に許可」にチェックを入れると、次からは聞かれません。
- カリスがすでに起動している時は、新しく起動せず、動いているカリスに場所を渡します（場所の情報が開き、
  テレポートはボタンを押した時だけ）。
- 登録したまま zip 版のフォルダを消す時は、先に［元に戻す］を押してください。

■ ボイスの字幕と翻訳
- 字幕と翻訳に使うヘルパー（charis-subtitle.exe・charis-translate.exe と、両方が使う vcomp140.dll）は、
  ${exe} と同じフォルダに入っています。
- Whisper と翻訳のモデルは、この zip に入っていません。字幕の窓のボタンで取得するか、
  %APPDATA%\CharisViewer\subtitle に手で置きます。

■ アンインストール
- 展開したフォルダを、フォルダごと削除してください。
- 設定も消すときは、上の「設定の場所」のフォルダも削除してください（インストーラー版も入れている時は、
  そちらの設定も消えます）。

■ ライセンス
カリス ビューワは、Linden Lab の公式 Second Life ビューワをもとにしており、
ライセンスは公式ビューワと同じ GNU LGPL 2.1 です。
- licenses.txt: 公式ビューワとそのライブラリの表記
- licenses-charis.txt: カリス ビューワで加わった物の表記（ほかのプロジェクトから取り入れたコード、
  Vulkan のライブラリ、licenses.txt に無い同梱ライブラリ、LGPL 2.1 の全文）
- licenses フォルダ: 同梱ライブラリごとのライセンス文
ソースコードは、この zip と同じページにある CharisViewer-${version}-src.zip です。
ビルドの手順はソースの indra/newview/plus/building/BUILDING.md にあります。
Second Life は Linden Research, Inc. の商標です。カリス ビューワは Linden Lab の製品ではありません。

======================================================================
English
======================================================================

Charis Viewer is a viewer for Second Life (alpha: a test release).
This is the zip: no installer, unpack it and run it.
It does not update itself: get a new version from the download page and unpack it to a new
folder (for automatic updates, use the installer, Setup.exe, instead).

* Requirements
- Windows 10 / 11 (64-bit)
- A GPU and a recent graphics driver with Vulkan 1.3
  Roughly: NVIDIA GeForce GTX 10 series (Pascal) or newer,
           AMD Radeon RX 400 series or newer, Intel 6th gen Core (Skylake) or newer integrated GPU
- A driver with the Vulkan extension depth clip control (VK_EXT_depth_clip_control)
  Older drivers may lack it: please update the driver from the GPU maker's website.
  Intel 6th to 10th gen Core integrated GPUs have not been checked: they may lack it
  even with the latest driver, and then the viewer cannot start.
  GPUs older than the above are not supported. When something is missing, the viewer says what at startup.

* Unpacking
- Unpack the zip to a folder inside your own user folder (for example %LOCALAPPDATA%\Programs\CharisViewer).
- Do not put it at the top of a drive (such as C:\): other accounts on the same PC may be able
  to replace the DLLs in it.
- Do not unpack it into the official viewer's folder (such as C:\Program Files\SecondLifeViewer).
- Do not unpack it into the installer's folder (%LOCALAPPDATA%\CharisViewerApp) either.
- Before unpacking a downloaded zip, right-click it -> Properties -> tick "Unblock":
  fewer warnings at start.
- Do not unpack it inside the folder of a portable viewer (one with a file named .portable):
  the settings then go to UserData there instead of "Where settings are kept" below, and are
  easy to miss when removing it.

* Starting
- Double-click ${exe} in the unpacked folder.
- If Windows says "Windows protected your PC", choose "More info" -> "Run anyway".
- Your antivirus may wrongly flag ${exe} or other files as a virus (this happens
  more often to new, unsigned programs). If you got it from the download page, it is a false
  positive: add the folder to the antivirus exclusions, or report it to the antivirus maker
  as a false positive.
- The first start can take a while (shaders are prepared for your GPU).

* Where settings are kept
- Settings, logs and per-account settings: %APPDATA%\CharisViewer
- Cache: %LOCALAPPDATA%\CharisViewer
- Vulkan shader cache: vk_shader_cache in the cache (%LOCALAPPDATA%\CharisViewer\vk_shader_cache)
  %LOCALAPPDATA%\SecondLife\vk_shader_cache, used by earlier versions, is no longer used: it can be deleted.
- The zip and the installer use the same settings and cache; do not run both at the same time.
- On the first start, if the official viewer, Firestorm or another viewer is on this PC,
  the login screen asks whether to bring its settings over. You can also do it later: Preferences ->
  Setup -> "Bring settings over from another viewer...". The other viewer's files are
  only read, never changed, and both viewers can be installed side by side.

* SLURL links in the browser (secondlife://)
- Unpacking Charis Viewer does not register secondlife:// links (it never changes what the official
  viewer or another viewer registered). To open the browser's links in Charis, press "Open them with
  this viewer" in Preferences > Setup. Only your Windows user's settings change; no administrator
  rights are needed. "Undo" puts back the viewer that opened them before.
- When you click "Visit this location" on maps.secondlife.com and the like, the browser asks
  whether to open Charis Viewer; tick "always allow" there to not be asked again.
- When Charis is already running, the link goes to the running one (the place's information opens;
  it teleports only when you press the button).
- Before deleting the folder of the zip, press "Undo" if you registered the links.

* Voice subtitles and translation
- The helpers the subtitles and the translation use (charis-subtitle.exe,
  charis-translate.exe and vcomp140.dll, which both use) are in the folder of ${exe}.
- The Whisper and translation models are not in this zip. Get them with the button in
  the subtitle window, or put them by hand in %APPDATA%\CharisViewer\subtitle.

* Uninstalling
- Delete the unpacked folder.
- To remove the settings too, also delete the folders under "Where settings are kept" (if the
  installer's copy is on this PC too, its settings go with them).

* License
Charis Viewer is based on the official Second Life viewer by Linden Lab and, like it,
is licensed under the GNU LGPL 2.1.
- licenses.txt: the notices of the official viewer and its libraries
- licenses-charis.txt: the notices for what Charis Viewer adds (code taken from other
  projects, the Vulkan libraries, bundled libraries not in licenses.txt, the LGPL 2.1 text)
- the licenses folder: the license file of each bundled library
The source code is CharisViewer-${version}-src.zip on the same page as this zip; the
build steps are in its indra/newview/plus/building/BUILDING.md.
Second Life is a trademark of Linden Research, Inc. Charis Viewer is not a Linden Lab product.
