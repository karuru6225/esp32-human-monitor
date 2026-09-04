// ============================================================
// SpecificDeviceCSI: 送信専用ファームウェア（TX）— 診断版
//
// AP非接続、ESP-NOWでRX宛のユニキャストを送り続けるだけの構成。
// 元祖ESP32(RX)でCSIコールバックがほぼ発火しない問題を確認済み
// （#22）。この版はRXのチップを変えて同じ条件で切り分けるための
// ベースライン（TXは元祖ESP32=ATOM Liteのまま固定し、RXだけ
// AtomS3 Lite(ESP32-S3)等に差し替えて検証する）。
//
// 対象: M5Stack ATOM Lite（ESP32無印）。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>

extern "C"
{
#include "esp_now.h"
#include "esp_wifi.h"
}

// 固定MACはTX/RXそれぞれ別の値にする（RX側がinfo->macで送信元を
// フィルタするための固定値でもある）。
static const uint8_t TX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t RX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x02};

static const uint8_t WIFI_CHANNEL = 11;
static const int SEND_FREQUENCY_HZ = 100;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t mac_err = esp_wifi_set_mac(WIFI_IF_STA, TX_MAC);
  if (mac_err != ESP_OK)
  {
    Serial.printf("esp_wifi_set_mac failed: %s\n", esp_err_to_name(mac_err));
  }

  esp_err_t err = esp_now_init();
  if (err != ESP_OK)
  {
    Serial.printf("esp_now_init failed: %d\n", err);
  }
  esp_now_set_pmk((uint8_t *)"pmk1234567890123");

  esp_now_peer_info_t peer = {};
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  memcpy(peer.peer_addr, RX_MAC, 6);
  err = esp_now_add_peer(&peer);
  if (err != ESP_OK)
  {
    Serial.printf("esp_now_add_peer failed: %d\n", err);
  }

  // 注: 公式サンプルにあるesp_now_set_peer_rate_config()（送信レートの
  // 明示指定）は、このプラットフォーム(espressif32@6.13.0)がバンドルする
  // esp-idfのESP-NOW APIには存在しない（より新しいidfで追加されたAPI）
  // ため省略。デフォルトレートで動作させる。

  uint8_t actual_channel = 0;
  wifi_second_chan_t actual_second_chan;
  esp_wifi_get_channel(&actual_channel, &actual_second_chan);

  Serial.println("================ CSI SEND (AP非接続 + unicast) ================");
  Serial.printf("wifi_channel(actual)=%d(2nd=%d) send_frequency=%dHz tx_mac=%02X:%02X:%02X:%02X:%02X:%02X rx_mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                actual_channel, actual_second_chan, SEND_FREQUENCY_HZ,
                TX_MAC[0], TX_MAC[1], TX_MAC[2], TX_MAC[3], TX_MAC[4], TX_MAC[5],
                RX_MAC[0], RX_MAC[1], RX_MAC[2], RX_MAC[3], RX_MAC[4], RX_MAC[5]);
}

void loop()
{
  static uint32_t count = 0;

  esp_err_t err = esp_now_send(RX_MAC, (const uint8_t *)&count, sizeof(count));
  if (err != ESP_OK)
  {
    Serial.printf("esp_now_send error: %d\n", err);
  }

  if (count % SEND_FREQUENCY_HZ == 0)
  {
    // 1秒おきに生存確認ログ（毎回出すとシリアルが埋まるため間引き）
    Serial.printf("sent count=%lu\n", (unsigned long)count);
  }

  count++;
  delayMicroseconds(1000000 / SEND_FREQUENCY_HZ);
}
