# esp32-human-monitor

Home AssistantとESP32を使った、部屋の活動ログ・人感センシングの検証プロジェクト。

## 背景・目的

- Home Assistant（Synology NAS上のDocker）で部屋の状態を監視し、「いつ何をしていたか」を活動ログとして可視化したい
- 当初はmmWaveレーダー（Aqara FP2 / Seeed XIAOシリーズ）を検討したが、技適確認の手間や、玄関付近にしかないモーションセンサーではリビングでの在宅勤務検知ができないという課題があった
- 代替として、WiFi CSI（Channel State Information）によるセンシングを検証中。専用レーダーHWが不要で、既存のESP32-S3ボードで技適の心配なく試せるのが利点

## 現在の方針

- **本命ルート**：PlatformIOでEspressif公式`esp-csi`を参考にした自作ファームウェア（`src/main.cpp`）。MQTT等の外部連携はまだ入れず、シリアル出力＋WiFi UDP（`tools/csi_listener`でPCへサブキャリア別振幅を送信）で「CSIが実際に動きに反応するか」を検証する段階。単一スカラー(rssi/avg_amp)だけでなく、サブキャリア単位への分解・フレネルゾーン理論に基づくノード配置最適化まで進んでいる（現状は`docs/handoff.md`、検証の詳細は`docs/research_log.md`）
- **参考/代替ルート**：ESPHome + `espectre`外部コンポーネントを使う方法も`esphome/`以下に残してある（HA連携が最初から付いてくる分、検証後にすぐ使える）

## 検証に使うハードウェア

- car-iot用に作った開発機（ESP32-S3、自作基板・PCBアンテナ）→ 検証済み・不採用。信号品質は市販品より明確に劣ることが判明（詳細は`docs/research_log.md`）
- Seeed Studio XIAO ESP32C3（市販品、外付けアンテナ）→ 追加投入済み。`platformio.ini`の`env:xiao-c3`
- M5AtomS3 Lite（ESP32-S3、市販品・内蔵3Dアンテナ、本来はSesame5⇔HA連携の本番機）→ 一時的にCSIテスト用ファームを書き込んで検証に使用中。**検証後は元のファームに書き戻す必要あり**
- M5Stack NanoC6（ESP32-C6）→ **採用断念（2026-08-31）**。PlatformIOの標準ビルド環境がESP32-C6のArduinoフレームワークに未対応で、回避策も環境破壊のリスクが高く見合わないと判断（詳細は`docs/handoff.md`）
- M5Stack ATOM Lite（ESP32無印、内蔵3Dアンテナ）→ NanoC6の代替として3台追加購入・実機検証済み（`env:m5atom-lite`）。同一機種3台構成でのチップ非依存性・rssi移動平均による移動方向判定を確認（詳細は`docs/research_log.md` `#21`）

## ディレクトリ構成

```
.
├── platformio.ini          # PlatformIO設定（arduino framework）
│                            #   env:esp32-s3, env:xiao-c3, env:m5atom-lite → RouterCSI(main.cpp)
│                            #   env:m5atom-lite-tx, env:m5atom-lite-rx → SpecificDeviceCSI診断用
├── src/
│   ├── main.cpp               # RouterCSI用ファームウェア（シリアル出力＋WiFi UDP、本命ルート）
│   ├── csi_tx.cpp             # SpecificDeviceCSI送信専用（診断版）
│   ├── csi_rx.cpp             # SpecificDeviceCSI受信専用（診断版）
│   └── secrets.h.example      # WiFi認証情報のテンプレート（secrets.hにコピーして使う）
├── esphome/
│   ├── csi_test_atoms3lite.yaml
│   └── csi_test_cariot_devboard.yaml
├── tools/
│   ├── csi_listener/          # PC側受信スクリプト(Docker化)
│   └── position_reporter/     # 位置報告用Androidアプリ(Flutter)
└── docs/
    ├── verification_procedure.md   # ESPHome版の検証手順（参考）
    ├── handoff.md                  # 現状把握・次のアクション（引き継ぎ用）
    ├── research_log.md             # 検証結果・経緯の詳細ログ
    └── images/                     # 検証結果のグラフ画像
```

## 技術メモ

- 本命は自分でトラフィックを生成して自分で受信する「RouterCSI（ルーターCSI方式）」構成。複数ノードでの精度向上（ゾーン分離・多層融合）は、独立したノードを増やして結果を融合することが本質
- SpecificDeviceCSI（送受信ペア構成、専用デバイスCSI方式）はATOM Lite（ESP32無印）で実装・実機検証したが、**AP接続していないとCSIハードウェアがほぼ動かず、AP接続してもアソシエート中でない第三者デバイスからのフレームはCSIをほとんど拾えないという、元祖ESP32チップ固有と思われる制約に当たった**（公式ドキュメント未記載、詳細は`docs/research_log.md` `#22`）。次に試すなら公式のCSI性能ランキング（C5>C6>C3≈S3>ESP32）で元祖ESP32より上位、かつ手持ちのAtomS3 Lite(S3)・XIAO C3(C3)から

## 次のステップ

詳細な優先度つきリストは`docs/handoff.md`の「次のアクション候補」を参照。要点のみ：

0. M5AtomS3 Liteへの本番ファーム書き戻し（Sesame5⇔HA連携の中継機能が止まっている）
1. SpecificDeviceCSIが元祖ESP32で機能しない問題への対応方針を決める（AtomS3/C3での再検証 or InterDeviceCSIの検証 or この方式自体を諦める）
2. `diff_score`をサブキャリア0〜50帯（Legacy LTF）限定に改修し、動き検知の精度改善を実機で確認
3. 経路遮蔽仮説・C3サブキャリア形状によるドリフト補正の複数回再現性確認
4. 実用に足る精度が見えたら、Home Assistant連携（ESPHome版へ移行 or 自前でMQTT実装）を検討

セットアップ手順：`src/secrets.h.example`を`secrets.h`にコピーしてWiFi認証情報を入れてからビルドする（`secrets.h`は`.gitignore`済み）。
