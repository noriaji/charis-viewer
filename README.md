# Charis Viewer

An unofficial, third-party viewer for Second Life with a Vulkan renderer,
based on the official Second Life viewer by Linden Research, Inc.

This software is not provided or supported by Linden Lab, the makers of
Second Life. "Second Life" and "Linden Lab" are trademarks of Linden
Research, Inc.

日本語は下にあります。

## What is different from the official viewer

- Rendering is Vulkan only (OpenGL is removed). Windows 10/11 64-bit and a
  GPU and driver with Vulkan 1.3 and VK_EXT_depth_clip_control are needed
  (roughly NVIDIA GeForce GTX 10 series or later). The viewer tells you at
  start if your GPU cannot run it.
- A faster renderer (a retained scene, GPU-driven culling and more).
- Photo tools: looks (LUTs), tone maps, a cross filter, depth of field and
  more, with presets.
- Features ported from Firestorm (poser, AO, area search, radar and more),
  each with its origin noted in `indra/newview/plus/<feature>/ORIGIN.md` or
  `SOURCE.md`.
- The code outside rendering is the official viewer's, taken in with each
  official Second Life Release (not the develop branch). Charis Viewer's own
  features sit apart in `indra/newview/plus/`.
- The release notes of each release list the changes.

## How it is made

Charis Viewer is developed by people together with AI coding assistants.
Every change is reviewed and tested by the developers before release.

## Network connections

Charis Viewer connects to the same Second Life services as the official
viewer (login, regions, assets, maps, voice, web pages in the viewer).
What it adds:

| What | When | Default |
|---|---|---|
| Speech recognition model download from huggingface.co (subtitles, Whisper) | when you press Download | off |
| Translation model download from the project's GitHub releases | when you press Download | off |
| Update check (one small file, `latest.json`, from raw.githubusercontent.com; see [PRIVACY.md](PRIVACY.md)) | at start, once a day | on (a notice only; turn it off in Preferences > Setup) |
| Update download (Setup.exe version only, from the project's GitHub releases) | after you press Download (or without asking, if you chose that) | ask first |
| Online translation (Azure, DeepL or Google; an official feature) | subtitles with online translation | off |
| Statistics to the region (the official ViewerStats) | once just after login, then every 5 minutes, at logout and at quit (as the official viewer) | on, as in the official viewer, without secrets, free text or Charis settings |
| Local MCP server on 127.0.0.1 (a local remote control for the poser) | when you turn it on | off |

There is no crash reporter, no telemetry of its own, and nothing is sent to
other residents. An update is downloaded only in the Setup.exe version (the zip
version never downloads one), and by default only after you press Download;
you can choose to only be told, or to download without asking. Downloaded
models are checked against a fixed SHA-256, and updates against the size
and SHA-256 in the release.

## Building

See [indra/newview/plus/building/BUILDING.md](indra/newview/plus/building/BUILDING.md)
(Windows).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GNU Lesser General Public License 2.1 only ([LICENSE](LICENSE)), as the
official viewer. Notices for the parts Charis Viewer adds or ports:
[indra/newview/plus/licenses-charis.txt](indra/newview/plus/licenses-charis.txt).

## Notes for readers of the source

Comments that cite `research/NNN` refer to internal design notes that are
not published. Changes to official files are marked with `<plus>` comments.

---

# Charis Viewer（日本語）

Vulkan で描画する、Second Life の非公式（サードパーティー）ビューワです。
Linden Research, Inc. の公式 Second Life ビューワを元にしています。

このソフトウェアは、Second Life を作っている Linden Lab が提供・サポート
するものではありません。「Second Life」「Linden Lab」は Linden Research,
Inc. の商標です。

## 公式ビューワとの違い

- 描画は Vulkan のみ（OpenGL は削除）。Windows 10/11 64 ビット、Vulkan 1.3
  と VK_EXT_depth_clip_control に対応した GPU とドライバーが必要です（目安:
  NVIDIA GeForce GTX 10 番台以降）。動かせない GPU では起動時に案内が出ます。
- 速い描画（保持型のシーン、GPU によるカリングなど）。
- 撮影ツール: ルック（LUT）、トーンマップ、クロスフィルター、被写界深度など。
  プリセットつき。
- Firestorm から移植した機能（ポーザー、AO、エリアサーチ、レーダーなど）。
  移植元は `indra/newview/plus/<機能>/ORIGIN.md` か `SOURCE.md` に書いています。
- 描画以外のコードは公式ビューワのもので、公式の正式リリース（develop では
  なく）ごとに取り込みます。Charis 独自の機能は `indra/newview/plus/` に
  分けて載せています。
- 変更点は各リリースのリリースノートにあります。

## 作り方

人間と AI（コーディング支援）で開発しています。すべての変更は、公開前に
開発者がレビューし、試しています。

## 通信

公式ビューワと同じ Second Life のサービス（ログイン、リージョン、アセット、
地図、ボイス、ビューワ内の Web ページ）につなぎます。Charis が足す通信は
上の英語の表のとおりです（更新確認は既定オンで、新しい版を知らせるだけです。更新のダウンロードは
既定で「尋ねる」。そのほかは既定オフか、公式と同じ統計のみ）。詳しくは [PRIVACY.md](PRIVACY.md)。
統計は公式と同じく、ログイン直後に 1 回、その後 5 分ごと、ログアウト時と
終了時に送ります。
クラッシュレポーターと独自の利用状況の送信はなく、他の住人へ何かを送ることも
ありません。更新をダウンロードするのは Setup.exe 版だけで（zip 版は取りません）、
既定では［ダウンロード］を押してからです。「知らせるだけ」や、尋ねずに自動で
ダウンロードする設定にも変えられます。ダウンロードしたモデルは固定の SHA-256 で、
更新はリリースに書かれた大きさと SHA-256 で確かめます。

## ビルド

[indra/newview/plus/building/BUILDING.md](indra/newview/plus/building/BUILDING.md)
（Windows）を見てください。

## ライセンス

公式ビューワと同じ GNU LGPL 2.1 only（[LICENSE](LICENSE)）。Charis が足した・
移植した部分の表記は
[indra/newview/plus/licenses-charis.txt](indra/newview/plus/licenses-charis.txt)
にあります。

## ソースを読む方へ

コメントの `research/NNN` は、公開していない設計メモの番号です。公式
ファイルを変えた所には `<plus>` の印を付けています。
