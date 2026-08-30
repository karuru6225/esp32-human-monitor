# esp32-human-monitor

Home AssistantとESP32を使った、部屋の活動ログ・人感センシングの検証プロジェクト。

## 背景・目的

- Home Assistant（Synology NAS上のDocker）で部屋の状態を監視し、「いつ何をしていたか」を活動ログとして可視化したい
- 当初はmmWaveレーダー（Aqara FP2 / Seeed XIAOシリーズ）を検討したが、技適確認の手間や、玄関付近にしかないモーションセンサーではリビングでの在宅勤務検知ができないという課題があった
- 代替として、WiFi CSI（Channel State Information）によるセンシングを検証中。専用レーダーHWが不要で、既存のESP32-S3ボードで技適の心配なく試せるのが利点

## 現在の方針

- **本命ルート**：PlatformIOでEspressif公式`esp-csi`を参考にした自作ファームウェア（`src/main.cpp`）。MQTT等の外部連携はまだ入れず、シリアル出力＋WiFi UDP（`tools/csi_listener`でPCへサブキャリア別振幅を送信）で「CSIが実際に動きに反応するか」を検証する段階。単一スカラー(rssi/avg_amp)だけでなく、サブキャリア単位への分解・フレネルゾーン理論に基づくノード配置最適化まで進んでいる（詳細は`docs/handoff.md`）
- **参考/代替ルート**：ESPHome + `espectre`外部コンポーネントを使う方法も`esphome/`以下に残してある（HA連携が最初から付いてくる分、検証後にすぐ使える）

## 検証に使うハードウェア

- car-iot用に作った開発機（ESP32-S3、自作基板・PCBアンテナ）→ 検証済み。信号品質は市販品より明確に劣ることが判明（詳細は`docs/handoff.md`）
- Seeed Studio XIAO ESP32C3（市販品、外付けアンテナ）→ 追加投入済み。`platformio.ini`の`env:xiao-c3`
- M5AtomS3 Lite（ESP32-S3、市販品・外付けアンテナ、本来はSesame5⇔HA連携の本番機）→ 一時的にCSIテスト用ファームを書き込んで検証に使用中。**検証後は元のファームに書き戻す必要あり**

## ディレクトリ構成

```
.
├── platformio.ini          # PlatformIO設定（env:esp32-s3, env:xiao-c3 / arduino framework）
├── src/
│   ├── main.cpp              # CSI検証用ファームウェア（シリアル出力＋WiFi UDP）
│   └── secrets.h.example     # WiFi認証情報のテンプレート（secrets.hにコピーして使う）
├── esphome/
│   ├── csi_test_atoms3lite.yaml
│   └── csi_test_cariot_devboard.yaml
├── tools/
│   ├── csi_listener/          # PC側受信スクリプト(Docker化)
│   └── position_reporter/     # 位置報告用Androidアプリ(Flutter)
└── docs/
    ├── verification_procedure.md   # ESPHome版の検証手順（参考）
    └── handoff.md                  # 検証結果・経緯の詳細記録
```

## 技術メモ

- ESP32-S3は送受信を同時に行うペア構成（Level 2）だとMACスタベーションが起きやすいため、まずは自分でトラフィックを生成して自分で受信する「セルフセンシング（Level 1）」構成で検証している
- 複数ノードでの精度向上（ゾーン分離・多層融合）は、Level1/Level2の選択とは別軸の話で、独立したノードを増やして結果を融合することが本質
- 将来ペア構成（TX専用+RX専用）でしっかり作り込みたくなったら、ESP32-S3ではなくESP32-C6/C5系のボードが推奨（TX+RX同時実行に強い）

## 次のステップ

詳細な優先度つきリストは`docs/handoff.md`の「次のアクション候補」を参照。要点のみ：

1. `diff_score`をサブキャリア0〜50帯（Legacy LTF）限定に改修し、動き検知の精度改善を実機で確認
2. 「移動方向（サブキャリアの乱れの順序）＋静止位置（ノード間rssi差分）」を組み合わせたトラッキングの試作
3. 経路遮蔽仮説の複数回再現性確認
4. 実用に足る精度が見えたら、Home Assistant連携（ESPHome版へ移行 or 自前でMQTT実装）を検討

セットアップ手順：`src/secrets.h.example`を`secrets.h`にコピーしてWiFi認証情報を入れてからビルドする（`secrets.h`は`.gitignore`済み）。
