# esp32-human-monitor

Home AssistantとESP32を使った、部屋の活動ログ・人感センシングの検証プロジェクト。

## 背景・目的

- Home Assistant（Synology NAS上のDocker）で部屋の状態を監視し、「いつ何をしていたか」を活動ログとして可視化したい
- 当初はmmWaveレーダー（Aqara FP2 / Seeed XIAOシリーズ）を検討したが、技適確認の手間や、玄関付近にしかないモーションセンサーではリビングでの在宅勤務検知ができないという課題があった
- 代替として、WiFi CSI（Channel State Information）によるセンシングを検証中。専用レーダーHWが不要で、既存のESP32-S3ボードで技適の心配なく試せるのが利点

## 現在の方針

- **RouterCSI（本命・実運用に近いルート）**：PlatformIO + Arduino frameworkの自作ファームウェア（`src/main.cpp`）。ESP32がWiFiルーターに接続したまま自分でトラフィックを生成し、そのCSIを取得する方式。シリアル出力＋WiFi UDP（`tools/csi_listener`でPCへサブキャリア別振幅を送信）で検証中。単一スカラー(rssi/avg_amp)だけでなく、サブキャリア単位への分解・フレネルゾーン理論に基づくノード配置最適化まで進んでいる（現状は`docs/handoff.md`、検証の詳細は`docs/research_log.md`）
- **SpecificDeviceCSI（AP非依存ルート、実装完了）**：専用送信機(TX)と受信機(RX)のペアで、ルーターを介さずCSIを取得する方式。`idf_csi_test/`（RX）・`idf_csi_test_tx/`（TX）で**素のESP-IDF実装**として動作確認済み——Arduino frameworkでは動かない既知の制約があり（`docs/research_log.md` `#22`〜`#24`）、TX/RXともM5Stack ATOM Lite（ESP32無印）だけで構成できることも確認済み（`#26`）。リアルタイム可視化ツール`tools/csi_ws_server/`（WebSocket+docker-compose）もあり、`docker compose up`で起動してブラウザで見れる
- **参考/代替ルート**：ESPHome + `espectre`外部コンポーネントを使う方法も`esphome/`以下に残してある（HA連携が最初から付いてくる分、検証後にすぐ使える）

## 検証に使うハードウェア

- car-iot用に作った開発機（ESP32-S3、自作基板・PCBアンテナ）→ 検証済み・不採用。信号品質は市販品より明確に劣ることが判明（詳細は`docs/research_log.md`）
- Seeed Studio XIAO ESP32C3（市販品、外付けアンテナ）→ 追加投入済み。`platformio.ini`の`env:xiao-c3`
- M5AtomS3 Lite（ESP32-S3、市販品・内蔵3Dアンテナ、本来はSesame5⇔HA連携の本番機）→ 一時的にCSIテスト用ファームを書き込んで検証に使用中。**検証後は元のファームに書き戻す必要あり**
- M5Stack NanoC6（ESP32-C6）→ **採用断念（2026-08-31）**。PlatformIOの標準ビルド環境がESP32-C6のArduinoフレームワークに未対応で、回避策も環境破壊のリスクが高く見合わないと判断（詳細は`docs/handoff.md`）
- M5Stack ATOM Lite（ESP32無印、内蔵3Dアンテナ）3台→ NanoC6の代替として追加購入・実機検証済み。RouterCSI方式でのチップ非依存性・rssi移動平均による移動方向判定を確認（`#21`）、SpecificDeviceCSI方式でもTX/RXとも問題なく動作しATOM Liteだけで構成可能と確認済み（`#26`）。TXはPC非接続の単体運用が可能（起動確認用にGPIO27の内蔵RGB LEDを緑点灯）

## ディレクトリ構成

```
.
├── platformio.ini          # RouterCSI用PlatformIO設定（arduino framework）
│                            #   env:esp32-s3, env:xiao-c3, env:m5atom-lite → RouterCSI(main.cpp)
│                            #   env:m5atom-lite-tx/-rx, env:atoms3-lite-tx/-rx → SpecificDeviceCSI診断用(Arduino版、非推奨)
├── src/
│   ├── main.cpp               # RouterCSI用ファームウェア（シリアル出力＋WiFi UDP、本命ルート）
│   ├── csi_tx.cpp             # SpecificDeviceCSI送信専用（Arduino版、動作しない診断コード。素のIDF版を使うこと）
│   └── secrets.h.example      # WiFi認証情報のテンプレート（secrets.hにコピーして使う）
├── idf_csi_test/            # SpecificDeviceCSI受信(RX)、素のESP-IDF実装（独立プロジェクト）
├── idf_csi_test_tx/         # SpecificDeviceCSI送信(TX)、素のESP-IDF実装（独立プロジェクト）
├── esphome/
│   ├── csi_test_atoms3lite.yaml
│   └── csi_test_cariot_devboard.yaml
├── tools/
│   ├── csi_listener/          # RouterCSI用PC側受信スクリプト(Docker化)
│   ├── csi_ws_server/          # SpecificDeviceCSIのリアルタイム可視化(WebSocket+docker-compose)
│   └── position_reporter/     # 位置報告用Androidアプリ(Flutter)
└── docs/
    ├── verification_procedure.md   # ESPHome版の検証手順（参考）
    ├── handoff.md                  # 現状把握・次のアクション（引き継ぎ用）
    ├── research_log.md             # 検証結果・経緯の詳細ログ
    └── images/                     # 検証結果のグラフ画像
```

## 技術メモ

- RouterCSIは自分でトラフィックを生成して自分で受信する構成。複数ノードでの精度向上（ゾーン分離・多層融合）は、独立したノードを増やして結果を融合することが本質
- SpecificDeviceCSI（送受信ペア構成）は当初Arduino frameworkで実装したところ、**AP非接続だとCSIコールバックがほぼ発火しない問題**にぶつかった（`#22`）。チップを変えても再現し（`#23`）、原因を素のESP-IDF実装への組み直しで切り分けたところ、**チップの制約ではなくArduino frameworkのesp_now実装に送信レート明示指定API(`esp_now_set_peer_rate_config`)が存在しないことが主因**と判明（`#24`）。素のESP-IDFなら元祖ESP32だけでTX/RXとも問題なく動作する（`#26`）。**SpecificDeviceCSIを実装するなら`framework=espidf`が前提**（Arduinoでは動かない）
- SpecificDeviceCSIのRXはUSBシリアルでPCへCSVを流す設計。全サブキャリア出力だとシリアル出力チャネル自体がボトルネックになるため（`#25`）、CSIコールバック内で複数サンプルを単純平均してから間引いて出力している
- Windows + Docker Desktopでシリアルポート（COMポート）をコンテナに渡すには`usbipd-win`が必要（`usbipd bind`→`usbipd attach --wsl`、手順の詳細は`docs/research_log.md` `#27`）

## 次のステップ

詳細な優先度つきリストは`docs/handoff.md`の「次のアクション候補」を参照。要点のみ：

0. M5AtomS3 Liteへの本番ファーム書き戻し（Sesame5⇔HA連携の中継機能が止まっている）
1. SpecificDeviceCSIで実際の位置推定を試す（ラベル付きデータ収集→diff_score等の指標がRouterCSIと同様に効くか検証。TX1台+RX複数台のブロードキャスト型構成への発展も視野）
2. `diff_score`をサブキャリア0〜50帯（Legacy LTF）限定に改修し、動き検知の精度改善を実機で確認
3. 経路遮蔽仮説・C3サブキャリア形状によるドリフト補正の複数回再現性確認
4. 実用に足る精度が見えたら、Home Assistant連携（ESPHome版へ移行 or 自前でMQTT実装）を検討

セットアップ手順：`src/secrets.h.example`を`secrets.h`にコピーしてWiFi認証情報を入れてからビルドする（`secrets.h`は`.gitignore`済み）。
