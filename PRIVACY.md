# Charis Viewer Privacy Policy

Last updated: 2026-11-13

日本語は下にあります。

Charis Viewer ("the viewer") is an unofficial, third-party viewer for
Second Life. It is not provided or supported by Linden Lab, the makers of
Second Life, and it has no connection to Linden Lab.

## Summary

- The Charis Viewer developers run no server that collects data from the
  viewer. The viewer sends nothing about you to the developers.
- There is no crash reporter and no usage data (telemetry) of its own.
- Like any viewer, it talks to Second Life (Linden Lab) and sends Linden Lab
  the standard viewer statistics.
- Other connections happen only for the features listed below.

## By version

- **0.1.0-alpha1**: there is no update check. Everything else in this
  policy applies.
- **0.1.0-alpha2 and later**: the update check is on by default.
  - At start, at most once a day, the viewer reads one small file
    (`latest.json`) from raw.githubusercontent.com with a plain HTTP GET.
    If no answer comes, it tries again an hour later at the earliest.
  - Nothing about your account, avatar, computer or settings is sent. As
    with any web page, the site sees your IP address and the User-Agent,
    which holds the viewer's name and version (for example
    "Charis Viewer 0.1.0 alpha 2").
  - If a newer version is out, a notice is shown on the login screen (or
    in a small window after you enter the world, if the answer comes after
    the login has started). It can show the addresses of the download page
    and the release notes (as text, not links).
  - To turn it off: uncheck "Tell me when a new Charis Viewer is out" in
    Preferences > Setup, or press "Stop notices" on the notice band. When
    it is off, nothing is read.
  - The Setup.exe version can also download the update. By default it asks
    first and downloads only after you press Download; in Preferences >
    Setup you can choose "Only tell me", or to download without asking.
    It reads the package from the project's GitHub releases (github.com
    and its download servers, see the table below), in pieces, and checks
    its size and SHA-256 before using it. Nothing about you is sent beyond what any web
    download shows (IP address, User-Agent). You can cancel at any time.
  - The zip version never downloads or installs an update. The notice only
    opens the download page (or the release notes) in your web browser when
    you press its button.

## Where the viewer connects

| Where | When | What |
|---|---|---|
| Second Life (Linden Lab) servers | while you use the viewer, as with any viewer | login, regions, assets (textures, meshes, sounds), inventory, map, search, voice, chat and IM. Your user name and password go only to Linden Lab's login service. |
| Viewer statistics to Linden Lab (the official ViewerStats) | once just after login, every 5 minutes, at logout and at quit, as the official viewer | the same form as the official viewer, for example: viewer name and version, language, operating system, CPU, memory, graphics card and driver, the computer identifiers the official viewer sends, game pad and 3D mouse information, performance numbers, distance moved, number of regions visited, translation use counts and the service name, and how often windows were used. Charis Viewer removes from it: secrets (setting names with key, token, secret, password or api), every text and free-form setting, all Charis settings, and the use of Charis windows. Uncheck "Send the settings I changed with the viewer statistics" (Preferences > Privacy, setting `PlusSendSettingsStats`) to send no settings at all. The rest is as the official viewer and cannot be turned off. The official asset and outfit-change measurements (ViewerMetrics) and avatar render information (AvatarRenderInfo) are also sent, as the official viewer does. Linden Lab handles all this under its own privacy policy (https://lindenlab.com/privacy). |
| raw.githubusercontent.com | update check (alpha2 and later, see above) | a GET of one file |
| github.com (the project's releases: the update list `releases.win.json` and the package) and its download servers (objects.githubusercontent.com, release-assets.githubusercontent.com) | only the Setup.exe version, to download an update: after you press Download (the default), or without asking if you chose that in Preferences > Setup | a normal download (the package in pieces, or whole if the server does not give pieces) (your IP address and the User-Agent) |
| huggingface.co and the download server it redirects to | only when you press "Get the model" (speech recognition model for voice subtitles) | a normal download (your IP address) |
| github.com (releases of noriaji/charis-models) and the download server it redirects to | only when you press "Get the models" in the translation settings or "Get translation models" in the subtitle window (local translation) | a normal download (your IP address) |
| The online translation service you choose (Microsoft Azure, DeepL or Google; these are official viewer features) | only when you turn on chat translation, or subtitle translation with "Translate: service", and the translation service (Preferences > Chat) is Azure, Google or DeepL (translation is off by default) | the text to translate (for subtitles: what other people said in voice, as recognized), and your API key |
| Web pages, media and music streams | web pages in the viewer, links you open, media and music set on land and objects, as the official viewer | what any web browser or media player sends to that site |
| Local MCP server (poser and inventory remote control) | only when you turn it on (off by default) | it listens on 127.0.0.1 (your own computer) only, with a token. The program you connect can get: your avatar name and region name, the names of you and nearby animesh, inventory item and folder names with their creators' names, and pictures of the world (pictures of the viewer's windows, which can show IM, only if you allow it with `PlusPoserMcpAllowUiSnapshot`). It can move the camera, read and change viewer settings (not secrets, paths or the MCP server's own settings) and pose your avatar. If that program uses a cloud service, this data, including other people's names (creators) and other avatars seen in the pictures, goes to that service. |

Downloaded models are checked against a fixed SHA-256 before use. Speech
recognition for voice subtitles runs on your computer. Subtitle
translation with "Translate: this PC", and the translation service
"This PC (local AI; nothing is sent anywhere)", both send nothing. Only
with an online service is the text sent (see the table).

## What the viewer does not do

- No crash reporter, no telemetry or usage data of its own.
- No hidden connections: every connection Charis adds to the official
  viewer is in the table above.
- Charis adds nothing to what Second Life itself shares with other
  residents. One exception you choose: if you turn on RLVa (off by
  default), the viewer takes commands only from your own objects
  (llOwnerSay) and answers their questions as the RLV rules say (for
  example the viewer version, what you wear, your inventory folders, what
  you sit on). The answers are said on a chat channel, so other scripts
  nearby can hear them too. When another resident asks by IM, the viewer
  answers @version with the viewer and RLV versions automatically, and
  answers @list only after you allow it (setting `RLVaEnableIMQuery`, on
  by default while RLVa is on).
- The developers do not sell or share your data (they receive none).

## What is stored on your computer

In `%APPDATA%\CharisViewer` and `%LOCALAPPDATA%\CharisViewer` (the program
itself is in `%LOCALAPPDATA%\CharisViewerApp` when installed with Setup.exe):

- settings, saved user names, and your password only if you choose
  "Remember password" (stored the same way as the official viewer),
- the API key of online translation, if you enter one (in the settings,
  as the official viewer),
- chat and IM logs if logging is on, and the viewer's log files, including
  `static_debug_info.log`, which holds your login name and the region
  name (the logged copy of the statistics leaves
  out the MAC address and the disk serial number),
- caches of textures, meshes, inventory and shaders,
- files of Charis features: photo presets, camera positions, poses,
  AO sets, the outfit backup, downloaded subtitle and translation models,
  the MCP server's connection file with its token
  (`user_settings\plus_poser_mcp.json`), and the MCP pictures
  (`user_settings\plus_poser_snapshots\poser_snapshot_NN.png`, at most 20,
  overwritten in turn).

Voice subtitles are shown on screen and not written to a file. When you
import settings from another viewer, saved logins (user names and
passwords) are not copied. These files stay on your computer unless you
send them yourself. Logs can contain your user name, region
names and chat; check them before you share them.

## Third parties

Linden Lab, GitHub, Hugging Face, the online translation service you
choose and the sites you open have their own privacy policies. Charis
Viewer does not control what they do with the data they receive.

## Children

Use of Second Life follows Linden Lab's Terms of Service, including its
age rules. Charis Viewer collects no data of its own from anyone.

## Changes

Changes to this policy are made in this file, with a new date at the top.
The history of the file is on GitHub.

## Contact

The project is at https://github.com/noriaji/charis-viewer. For questions
about this policy, write to charisviewer@gmail.com.

---

# Charis Viewer プライバシーポリシー

最終更新: 2026-11-13

Charis Viewer（以下「本ビューワ」）は、Second Life 用の非公式（サードパーティー）
のビューワです。Second Life を作っている Linden Lab が提供・サポートするもので
はなく、Linden Lab とは関係がありません。

## 要約

- 開発者は、本ビューワからデータを集めるサーバーを持っていません。本ビューワ
  から開発者へ、あなたについての情報は何も送りません。
- クラッシュレポーターも、独自の利用状況の送信（テレメトリ）もありません。
- どのビューワとも同じく Second Life（Linden Lab）と通信し、標準のビューワの
  統計を Linden Lab に送ります。
- そのほかの通信は、下に挙げる機能の時だけです。

## 版ごとの違い

- **0.1.0-alpha1**: 更新確認はありません。それ以外はこのポリシーのとおりです。
- **0.1.0-alpha2 以降**: 更新確認が既定でオンです。
  - 起動時に 1 日 1 回まで、raw.githubusercontent.com の小さなファイル
    （`latest.json`）を 1 つ、ふつうの HTTP の GET で読みます。答えが無かった
    時は、早くても 1 時間後にもう一度試します。
  - アカウント・アバター・PC・設定の情報は送りません。ふつうの Web ページと
    同じく、相手には IP アドレスと、ビューワの名前と版を含む User-Agent
    （例: 「Charis Viewer 0.1.0 alpha 2」）が見えます。
  - 新しい版があれば、ログイン画面でお知らせします（ログインを始めた後に答え
    が来た時は、ワールドに入ってから小さな窓で出します）。ダウンロードページと
    リリースノートのアドレスを（リンクではなく文字で）載せることがあります。
  - 止め方: 環境設定 > セットアップの「Charis Viewer の新しい版を知らせる」を
    オフにするか、お知らせの帯の「通知を止める」を押します。オフの間は何も
    読みません。
  - Setup.exe 版は更新のダウンロードもできます。既定ではまず尋ね、［ダウンロード］
    を押してから取ります（環境設定 > セットアップで「知らせるだけ」や、尋ねずに
    自動でダウンロードする設定にもできます）。本ビューワのプロジェクトの GitHub
    リリース（github.com とその配信サーバー。下の表のとおり）からパッケージを
    小分けに読み、使う前に大きさと SHA-256 を確かめます。ふつうの
    Web のダウンロードで相手に見える物（IP アドレス、User-Agent）以外は、
    あなたの情報を送りません。いつでも中止できます。
  - zip 版は更新をダウンロード・インストールしません。お知らせのボタンを
    押した時に、ダウンロードページ（またはリリースノート）をブラウザで開くだけです。

## 本ビューワが接続する先

| 接続先 | いつ | 何を |
|---|---|---|
| Second Life（Linden Lab）のサーバー | 使っている間（どのビューワとも同じ） | ログイン、リージョン、アセット（テクスチャ・メッシュ・音）、インベントリ、地図、検索、ボイス、チャット・IM。ユーザー名とパスワードは Linden Lab のログインサービスにだけ送ります。 |
| Linden Lab へのビューワの統計（公式の ViewerStats） | 公式と同じく、ログイン直後に 1 回、その後 5 分ごと、ログアウト時と終了時 | 公式と同じ形。例えば: ビューワ名と版、言語、OS、CPU、メモリ、グラフィックカードとドライバー、公式が送る PC の識別子、ゲームパッド・3D マウスの情報、性能の数値、移動した距離、訪れたリージョンの数、翻訳の使用数とサービス名、窓の使用回数。本ビューワは次を取り除きます: 秘密（key・token・secret・password・api を名前に含む設定）、文字列など自由入力の設定すべて、Charis の設定すべて、Charis の窓の使用回数。環境設定 > プライバシーの「変更した設定をビューワの統計に含めて送る」（設定 `PlusSendSettingsStats`）をオフにすると、設定は一切送りません。それ以外は公式と同じで、オフにはできません。公式のアセット取得と着替えの計測（ViewerMetrics）、アバターの描画情報（AvatarRenderInfo）も、公式と同じく送ります。Linden Lab が自社のプライバシーポリシー（https://lindenlab.com/privacy）のもとで扱います。 |
| raw.githubusercontent.com | 更新確認（alpha2 以降、上のとおり） | ファイル 1 つの GET |
| github.com（このプロジェクトの Releases: 更新の一覧 `releases.win.json` とパッケージ）とその配信サーバー（objects.githubusercontent.com、release-assets.githubusercontent.com） | Setup.exe 版が更新をダウンロードする時だけ。［ダウンロード］を押した後（既定）、または環境設定 > セットアップで尋ねずに自動でダウンロードする設定を選んだ時 | ふつうのダウンロード（パッケージは小分け。サーバーが対応しない時は丸ごと）（IP アドレスと User-Agent） |
| huggingface.co とそのリダイレクト先の配信サーバー | 「モデルを取得」（ボイスの字幕の音声認識モデル）を押した時だけ | ふつうのダウンロード（IP アドレス） |
| github.com（noriaji/charis-models の Releases）とそのリダイレクト先の配信サーバー | 翻訳設定の「モデルを取得」か、字幕の窓の「翻訳モデルを取得」（ローカル翻訳）を押した時だけ | ふつうのダウンロード（IP アドレス） |
| 選んだオンライン翻訳サービス（Microsoft Azure・DeepL・Google。これらは公式の機能） | チャットの翻訳、または字幕の翻訳（「翻訳: サービス」）をオンにし、かつ翻訳サービス（環境設定 > チャット）が Azure・Google・DeepL の時だけ（翻訳は既定オフ） | 翻訳する文（字幕なら、ボイスで他の人が話したことを聞き取った文）と API キー |
| Web ページ・メディア・音楽のストリーム | ビューワ内の Web ページ、開いたリンク、土地や物に設定されたメディアと音楽（公式と同じ） | ふつうのブラウザやプレーヤーがそのサイトに送るもの |
| ローカル MCP サーバー（ポーザーとインベントリの遠隔操作） | オンにした時だけ（既定オフ） | 127.0.0.1（自分の PC）だけで、合言葉つきで待ち受けます。つないだプログラムは、自分のアバター名とリージョン名、自分と近くのアニメッシュの名前、インベントリのアイテム・フォルダーの名前とその作者名、ワールドの画像（IM などが映るビューワの窓の画像は `PlusPoserMcpAllowUiSnapshot` で許可した時だけ）を受け取れます。カメラの操作、設定の読み書き（秘密・パス・MCP 自身の設定は除く）、ポーズの操作ができます。そのプログラムがクラウドのサービスを使うなら、他の人の名前（作者名）や画像に映った他のアバターを含め、これらがそのサービスに渡ります。 |

取得したモデルは、使う前に固定の SHA-256 で確かめます。ボイスの字幕の聞き
取りは PC の中で動きます。字幕の「翻訳: この PC」と、翻訳サービスの「この PC（ローカル AI。文字は外へ
出ません）」は、どちらも何も送りません。文が送られるのは、オンラインのサービス
を使う時だけです（上の表）。

## しないこと

- クラッシュレポーター、独自のテレメトリ・利用状況の送信はありません。
- 隠れた通信はありません。公式ビューワに Charis が足した通信は、すべて上の
  表にあります。
- Second Life 自体がほかの住人と共有する情報のほかに、Charis が足して送る
  ものはありません。ただし、ご自身で RLVa（既定オフ）をオンにした時は、自分の
  持ち物の llOwnerSay からだけ命令を受け、RLV の決まりに従ってその問い合わせ
  に答えます（例: ビューワの版、着ている物、インベントリのフォルダー、座って
  いる対象）。答えはチャットのチャンネルで言うので、近くの他のスクリプトも
  聞くことができます。また、ほかの住人が IM で尋ねた時は、@version にビューワ
  と RLV の版を自動で答え、@list はご本人が許可した時だけ答えます（設定
  `RLVaEnableIMQuery`。RLVa がオンの間は既定でオン）。
- 開発者はデータを受け取らないので、売ったり渡したりもしません。

## PC に保存するもの

`%APPDATA%\CharisViewer` と `%LOCALAPPDATA%\CharisViewer` に保存します
（Setup.exe で入れた場合、プログラム本体は `%LOCALAPPDATA%\CharisViewerApp`）。

- 設定、保存したユーザー名、「パスワードを記憶」を選んだ時だけパスワード
  （保存のしかたは公式ビューワと同じ）
- 入力した時は、オンライン翻訳の API キー（設定ファイルの中。公式と同じ）
- 記録をオンにした時のチャット・IM の記録と、ビューワのログ（ログイン名と
  リージョン名が入る `static_debug_info.log` を含む。
  ログに書く統計の写しからは、MAC アドレスとディスクのシリアル番号を除きます）
- テクスチャ・メッシュ・インベントリ・シェーダーのキャッシュ
- Charis の機能のファイル: 撮影のプリセット、カメラ位置、ポーズ、AO のセット、
  アウトフィットの控え、取得した字幕・翻訳のモデル、MCP サーバーの接続ファイル
  と合言葉（`user_settings\plus_poser_mcp.json`）、MCP が撮った画像
  （`user_settings\plus_poser_snapshots\poser_snapshot_NN.png`。最大 20 枚を順に上書き）

ボイスの字幕は画面に出すだけで、ファイルには書きません。ほかのビューワから
設定を取り込む時も、保存したログイン情報（ユーザー名とパスワード）は写しま
せん。これらのファイルは、ご自身で送らない限り PC の外に出ません。ログには
ユーザー名・リージョン名・チャットが入ることがあるので、人に渡す
前に中身を確かめてください。

## サードパーティー

Linden Lab、GitHub、Hugging Face、選んだオンライン翻訳サービス、開いたサイト
には、それぞれのプライバシーポリシーがあります。それらが受け取ったデータを
どう扱うかは、本ビューワでは決められません。

## 子どもの利用

Second Life の利用は、年齢の決まりを含め Linden Lab の利用規約に従います。
本ビューワは、どなたからも独自にデータを集めません。

## 変更

このポリシーを変える時は、このファイルを書き換え、冒頭の日付を新しくします。
変更の履歴は GitHub で見られます。

## 連絡先

プロジェクトのページは https://github.com/noriaji/charis-viewer です。この
ポリシーについてのお問い合わせは、charisviewer@gmail.com へどうぞ。
