# WiFi CSI 人感検証手順（ESPHome版・参考）

購入前に「そもそもリビングで反応するか」を確認するための、ESPHome + espectre を使ったテスト手順です。
現在の本命ルートはPlatformIO単体版（../src/main.cpp）ですが、ESPHome側の構成もこちらに残しておきます。

## 事前準備

1. **M5AtomS3 Liteの現在の設定をバックアップ**（Sesame5連携用の本番設定）。ESPHome Dashboardで既存デバイスのYAMLをエクスポート/コピーしておく。
2. `secrets.yaml` に以下を用意（WiFi・MQTT情報に置き換え）：
   ```yaml
   wifi_ssid: "your-ssid"
   wifi_password: "your-password"
   mqtt_broker: "snas"   # NAS上のMosquittoホスト名/IP
   mqtt_username: "your-mqtt-user"
   mqtt_password: "your-mqtt-password"
   ```

## 書き込み

- `esphome/csi_test_atoms3lite.yaml` → M5AtomS3 Liteへ（`esphome run csi_test_atoms3lite.yaml`）
- `esphome/csi_test_cariot_devboard.yaml` → car-iot開発機へ

書き込み後、Home Assistantに以下のエンティティが出てくる想定：
`sensor.movement_score` / `sensor.breathing_rate` / `binary_sensor.presence_detected` / `binary_sensor.motion_detected`

## 検証手順

1. **在室・作業状態**：デスクで5〜10分実際に作業し、movement_score/presence_detectedの動きを見る
2. **在室・静止状態**：座って動かず5分ほど。presence_detectedがPIRのようにOFFへ落ちてしまわないか確認
3. **不在**：部屋を出て5分。presence_detectedがOFFになるか、誤反応しないか
4. **通過**：別の場所を素通りしただけでデスク側が反応してしまわないか（誤検知範囲の把握）

## 判断基準

- 作業中と静止時でmovement_scoreに有意な差が出るか
- presence_detectedが座っている間ONを維持できるか
- 誤検知の頻度

## 注意点

- ESP32-S3同士のペア構成（Level 2）はMACスタベーションのため非推奨。今回はセルフセンシング（Level 1）を使用。
- 将来ペア構成で精度を上げたい場合はESP32-C6/C5系ボードを検討。
- 検証後、M5AtomS3 Liteは元のSesame5連携用設定に書き戻すこと。
