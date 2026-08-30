// ============================================================
// WiFi CSI 単体検証用（MQTTなし・シリアル出力のみ）
// car-iot開発機（ESP32-S3）想定
//
// 注意：これはEspressif公式 esp-csi の get-started サンプルを参考にした
// 簡易版で、コンパイル確認まではできていません。esp-idf/Arduinoコアの
// バージョンによって wifi_csi_config_t / wifi_csi_info_t のフィールド名が
// 変わることがあるので、コンパイルエラーが出たら esp_wifi_types.h の
// 実際の定義を見て調整してください。
//
// [MACフィルタ] info->mac（CSIデータの送信元MAC）が接続中APのBSSIDと
// 一致するパケットだけを採用するようにしてある。同一チャンネル上の
// 他端末の通信を拾ってノイズになっている可能性を減らすための実験。
// filt=採用数/受信総数 をシリアル出力に追加したので、フィルタでどれだけ
// 弾かれているか確認できる。
// ============================================================
#include <WiFi.h>
#include <WiFiUdp.h>
#include <string.h>

extern "C"
{
#include "esp_wifi.h"
}

#include "secrets.h"

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

WiFiUDP udp;
IPAddress gatewayIP;

#define CSI_MAX_LEN 400
static int8_t prev_buf[CSI_MAX_LEN];
static int prev_len = 0;

// 接続中APのBSSID（送信元MACフィルタ用）
static uint8_t ap_bssid[6] = {0};
static uint32_t csi_count_total = 0;
static uint32_t csi_count_accepted = 0;

// CSIコールバック：パケットを受信するたびに呼ばれる
void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
  if (!info || !info->buf)
  {
    return;
  }

  csi_count_total++;

  // 接続中APのBSSID以外から届いたと思われるパケットは除外
  // （同じチャンネル上の他端末の通信ノイズを取り除く）
  if (memcmp(info->mac, ap_bssid, 6) != 0)
  {
    return;
  }
  csi_count_accepted++;

  int8_t *csi_data = info->buf;
  int len = info->len;
  if (len > CSI_MAX_LEN)
  {
    len = CSI_MAX_LEN;
  }

  // 参考値：振幅の絶対値平均（空間方向の平均。動きにはあまり反応しない）
  long sum = 0;
  for (int i = 0; i < len; i++)
  {
    sum += abs(csi_data[i]);
  }
  float avg_amplitude = len > 0 ? (float)sum / len : 0;

  // 本命：前回パケットとの差分スコア（時間方向の変化=動きの指標）
  float diff_score = 0;
  if (prev_len == len && len > 0)
  {
    long diff_sum = 0;
    for (int i = 0; i < len; i++)
    {
      diff_sum += abs((int)csi_data[i] - (int)prev_buf[i]);
    }
    diff_score = (float)diff_sum / len;
  }

  if (len > 0)
  {
    memcpy(prev_buf, csi_data, len);
    prev_len = len;
  }

  Serial.printf("rssi=%d, len=%d, avg_amp=%.2f, diff_score=%.2f, filt=%lu/%lu\n",
                info->rx_ctrl.rssi, len, avg_amplitude, diff_score,
                (unsigned long)csi_count_accepted, (unsigned long)csi_count_total);
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected!");
  gatewayIP = WiFi.gatewayIP();
  Serial.print("Gateway IP: ");
  Serial.println(gatewayIP);

  // 接続中APのBSSIDを保存（CSIコールバックでの送信元フィルタに使う）
  uint8_t *bssid_ptr = WiFi.BSSID();
  if (bssid_ptr)
  {
    memcpy(ap_bssid, bssid_ptr, 6);
  }
  Serial.printf("AP BSSID: %02X:%02X:%02X:%02X:%02X:%02X\n",
                ap_bssid[0], ap_bssid[1], ap_bssid[2],
                ap_bssid[3], ap_bssid[4], ap_bssid[5]);

  // ---- CSI設定 ----
  wifi_csi_config_t csi_config = {
      .lltf_en = true,
      .htltf_en = true,
      .stbc_htltf2_en = true,
      .ltf_merge_en = true,
      .channel_filter_en = false,
      .manu_scale = false,
      .shift = false,
  };

  esp_err_t err;
  err = esp_wifi_set_csi_config(&csi_config);
  if (err != ESP_OK)
  {
    Serial.printf("esp_wifi_set_csi_config failed: %d\n", err);
  }

  err = esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL);
  if (err != ESP_OK)
  {
    Serial.printf("esp_wifi_set_csi_rx_cb failed: %d\n", err);
  }

  err = esp_wifi_set_csi(true);
  if (err != ESP_OK)
  {
    Serial.printf("esp_wifi_set_csi failed: %d\n", err);
  }

  udp.begin(12345);
}

void loop()
{
  // ゲートウェイに向けて定期的に空パケットを送り、応答フレームを誘発する
  // （これがないとCSIキャプチャの機会が少なすぎる可能性が高い）
  udp.beginPacket(gatewayIP, 9); // discardポート
  udp.write((uint8_t)0);
  udp.endPacket();

  delay(50); // だいたい20パケット/秒くらいのペース
}
