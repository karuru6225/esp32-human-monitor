// ============================================================
// SpecificDeviceCSI: 送信専用ファームウェア（TX）— 診断版
//
// 【診断中】AP非接続のpromiscuous構成ではRX側でCSIコールバックが
// 一度も発火しない問題が起きたため、「STAがAP接続(アソシエート)
// 済みであることがCSI取得の必要条件では」という仮説を確かめる版。
// TX/RXとも実家WiFi APに接続した状態を維持しつつ、ESP-NOWで
// ユニキャスト送信する。原因が確認でき次第、AP非依存の構成に戻す
// か、この構成のまま清書するかを判断する。
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

#include "secrets.h"

// 診断用: 元祖ESP32はブロードキャストではCSIが取れない疑い(esp-csi issue #247)
// があるため、ブロードキャストではなくRX宛のユニキャストで送る版。
// 固定MACはTX/RXそれぞれ別の値にする（RX側がinfo->macで送信元を
// フィルタするための固定値でもある）。
static const uint8_t TX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t RX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x02};

static const int SEND_FREQUENCY_HZ = 100;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_err_t mac_err = esp_wifi_set_mac(WIFI_IF_STA, TX_MAC);
  if (mac_err != ESP_OK)
  {
    Serial.printf("esp_wifi_set_mac failed: %s\n", esp_err_to_name(mac_err));
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" connected!");

  esp_err_t err = esp_now_init();
  if (err != ESP_OK)
  {
    Serial.printf("esp_now_init failed: %d\n", err);
  }
  esp_now_set_pmk((uint8_t *)"pmk1234567890123");

  esp_now_peer_info_t peer = {};
  peer.channel = 0; // 0=現在STAが接続中のチャンネルを使う
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

  Serial.println("================ CSI SEND (AP-connected + unicast) ================");
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
