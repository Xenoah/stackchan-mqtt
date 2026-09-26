# リリース別スクリーンショット

各リリースタグの `src/WebPages.h` から Web ダッシュボードの HTML・CSS を取り出し、
Chrome で PC 幅（1040px）とスマートフォン幅（430px）の全ページを撮影しました。
表示する印刷状況は共通のサンプルデータです（62%、残り42分、155/250層）。
接続状態・ジョブ名・温度・AMS・実況ログもサンプルで、実機の通信や保存設定は使いません。

画像を開くと原寸で確認できます。

| リリース | PC | スマートフォン | 撮影元 |
| --- | --- | --- | --- |
| [v2.4.1](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.4.1) | [画像](v2.4.1/web-desktop.png) | [画像](v2.4.1/web-mobile.png) | [記録](v2.4.1/web-capture.json) |
| [v2.4.0](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.4.0) | [画像](v2.4.0/web-desktop.png) | [画像](v2.4.0/web-mobile.png) | [記録](v2.4.0/web-capture.json) |
| [v2.3.2](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.3.2) | [画像](v2.3.2/web-desktop.png) | [画像](v2.3.2/web-mobile.png) | [記録](v2.3.2/web-capture.json) |
| [v2.3.1](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.3.1) | [画像](v2.3.1/web-desktop.png) | [画像](v2.3.1/web-mobile.png) | [記録](v2.3.1/web-capture.json) |
| [v2.3.0](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.3.0) | [画像](v2.3.0/web-desktop.png) | [画像](v2.3.0/web-mobile.png) | [記録](v2.3.0/web-capture.json) |
| [v2.2.0](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.2.0) | [画像](v2.2.0/web-desktop.png) | [画像](v2.2.0/web-mobile.png) | [記録](v2.2.0/web-capture.json) |
| [v2.1.0](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.1.0) | [画像](v2.1.0/web-desktop.png) | [画像](v2.1.0/web-mobile.png) | [記録](v2.1.0/web-capture.json) |
| [v2.0.0](https://github.com/Xenoah/stackchan-mqtt/releases/tag/v2.0.0) | [画像](v2.0.0/web-desktop.png) | [画像](v2.0.0/web-mobile.png) | [記録](v2.0.0/web-capture.json) |

v2.1.0 以降の Web ダッシュボードのソースは共通です。各タグから個別に撮影していますが、
同じサンプルデータでは同じ画面になります。v2.0.0 にはモード表示・切り替えボタンがありません。

## 再撮影

Python 3、Node.js 22 以降、Chrome または Chromium が必要です。
リポジトリのルートで実行します。実機への書込みは不要です。

```powershell
git fetch --tags
python tools/screenshots/capture_web.py                  # 全8リリース
python tools/screenshots/capture_web.py --version v2.4.1 # 指定した版だけ
```

ブラウザを検出できない場合は `--browser "ブラウザの実行ファイルのパス"` を追加します。
撮影用サーバーは `127.0.0.1` だけで待ち受け、ブラウザは `.pio/screenshots/chrome-profile` の
専用プロファイルを使います。撮影終了時にサーバーとブラウザを終了します。

撮影時だけアニメーションを止め、進捗リング・バーの描画完了、画面幅、横方向のはみ出しを
確認してから PNG を保存します。タグの HTML・CSS ファイル自体は変更しません。
各 `web-capture.json` にタグのコミット、ソースと画像の SHA256、寸法、サンプルデータを記録しています。
フォントやブラウザのバージョンによって細部が変わることがあります。
