// ============================================================
// SpecificDeviceCSI: 受信専用ファームウェア（RX）— 診断版
//
// AP非接続・promiscuousモードでTX（csi_tx.cpp）からのESP-NOW
// ユニキャストを受信してCSIが取れるか確認する。取得したCSIは
// USBシリアルへCSV形式でダンプする
// （PC側の受信ツールは別途必要、tools/csi_listenerとは別経路）。
//
// 元祖ESP32(ATOM Lite)ではCSIコールバックがほぼ発火しないことを
// 確認済み（#22）。このファイルはチップ非依存のAPIのみ使っており
// （wifi_csi_config_tの構造体はESP32無印/S3/C3で共通）、
// env側でboardをAtomS3 Lite(esp32-s3-devkitc-1)等に切り替えれば
// そのまま使い回してチップ差の検証に使える（`env:atoms3-lite-rx`）。
// ============================================================
#include <Arduino.h>
#include <WiFi.h>

extern "C"
{
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
}

// TX_MAC: TX側（csi_tx.cpp）が自分のSTA MACをこの値に上書きしているので、
//         info->macがこれと一致するパケットだけをTX由来のCSIとして扱う。
// RX_MAC: 自分（RX）のSTA MAC。TXはここ宛にユニキャストしてくる。
static const uint8_t TX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t RX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x02};

static const uint8_t WIFI_CHANNEL = 11;

static uint32_t s_count = 0;
static uint32_t s_total_cb = 0;
static uint8_t s_last_unmatched_mac[6] = {0};
static uint32_t s_promisc_total = 0;
static uint32_t s_promisc_matched = 0;

// CSI経路と切り離して「そもそも電波を受信できているか」を確認するための
// promiscuous生受信コールバック（デバッグ用、切り分けが済んだら削除する）
void wifi_promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  s_promisc_total++;
  // 802.11フレームのaddr2(送信元MAC)はMACヘッダのoffset10から6byte
  if (pkt->rx_ctrl.sig_len >= 16 && memcmp(pkt->payload + 10, TX_MAC, 6) == 0)
  {
    s_promisc_matched++;
  }
}

void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
  if (!info || !info->buf)
  {
    return;
  }

  s_total_cb++;

  // TX由来（TX_MAC）以外のパケットは無視
  if (memcmp(info->mac, TX_MAC, 6) != 0)
  {
    memcpy(s_last_unmatched_mac, info->mac, 6);
    return;
  }

  const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;

  if (s_count == 0)
  {
    Serial.println("================ CSI RECV ================");
    Serial.println("type,id,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,"
                    "aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,"
                    "secondary_channel,local_timestamp,ant,sig_len,len,first_word,data");
  }

  Serial.printf("CSI_DATA,%lu," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                (unsigned long)s_count, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate,
                rx_ctrl->sig_mode, rx_ctrl->mcs, rx_ctrl->cwb, rx_ctrl->smoothing,
                rx_ctrl->not_sounding, rx_ctrl->aggregation, rx_ctrl->stbc,
                rx_ctrl->fec_coding, rx_ctrl->sgi, rx_ctrl->noise_floor, rx_ctrl->ampdu_cnt,
                rx_ctrl->channel, rx_ctrl->secondary_channel, rx_ctrl->timestamp,
                rx_ctrl->ant, rx_ctrl->sig_len, info->len, info->first_word_invalid);

  Serial.printf(",\"[%d", info->buf[0]);
  for (int i = 1; i < info->len; i++)
  {
    Serial.printf(",%d", info->buf[i]);
  }
  Serial.println("]\"");

  s_count++;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_err_t mac_err = esp_wifi_set_mac(WIFI_IF_STA, RX_MAC);
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
  memcpy(peer.peer_addr, TX_MAC, 6);
  err = esp_now_add_peer(&peer);
  if (err != ESP_OK)
  {
    Serial.printf("esp_now_add_peer failed: %d\n", err);
  }

  // 注: 公式サンプルにあるesp_now_set_peer_rate_config()（送信レートの
  // 明示指定）は、このプラットフォーム(espressif32@6.13.0)がバンドルする
  // esp-idfのESP-NOW APIには存在しない（より新しいidfで追加されたAPI）
  // ため省略。デフォルトレートで動作させる。

  // ---- CSI設定（AP非接続なのでpromiscuousモード必須）----
  esp_wifi_set_promiscuous_rx_cb(wifi_promisc_rx_cb); // デバッグ用

  wifi_csi_config_t csi_config = {
      .lltf_en = true,
      .htltf_en = true,
      .stbc_htltf2_en = true,
      .ltf_merge_en = true,
      .channel_filter_en = false, // 切り分けのため一旦無効化（要検証後に戻す）
      .manu_scale = false,
      .shift = false,
  };
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

  esp_wifi_set_promiscuous(true);

  uint8_t own_mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, own_mac);
  uint8_t actual_channel = 0;
  wifi_second_chan_t actual_second_chan;
  esp_wifi_get_channel(&actual_channel, &actual_second_chan);
  bool promisc_en = false;
  esp_wifi_get_promiscuous(&promisc_en);
  Serial.println("================ CSI RECV: boot done (AP非接続 + unicast) ================");
  Serial.printf("wifi_channel(actual)=%d(2nd=%d) own_mac=%02X:%02X:%02X:%02X:%02X:%02X filter_mac(tx)=%02X:%02X:%02X:%02X:%02X:%02X promiscuous=%d\n",
                actual_channel, actual_second_chan,
                own_mac[0], own_mac[1], own_mac[2], own_mac[3], own_mac[4], own_mac[5],
                TX_MAC[0], TX_MAC[1], TX_MAC[2], TX_MAC[3], TX_MAC[4], TX_MAC[5],
                promisc_en ? 1 : 0);
}

void loop()
{
  static uint32_t last_count = 0;
  static uint32_t last_total = 0;
  static uint32_t last_promisc = 0;
  static uint32_t last_promisc_matched = 0;
  delay(2000);
  Serial.printf("[heartbeat] csi_matched=%lu(+%lu) csi_total_cb=%lu(+%lu) promisc_total=%lu(+%lu) promisc_matched=%lu(+%lu) last_unmatched_mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                (unsigned long)s_count, (unsigned long)(s_count - last_count),
                (unsigned long)s_total_cb, (unsigned long)(s_total_cb - last_total),
                (unsigned long)s_promisc_total, (unsigned long)(s_promisc_total - last_promisc),
                (unsigned long)s_promisc_matched, (unsigned long)(s_promisc_matched - last_promisc_matched),
                s_last_unmatched_mac[0], s_last_unmatched_mac[1], s_last_unmatched_mac[2],
                s_last_unmatched_mac[3], s_last_unmatched_mac[4], s_last_unmatched_mac[5]);
  last_count = s_count;
  last_total = s_total_cb;
  last_promisc = s_promisc_total;
  last_promisc_matched = s_promisc_matched;
}
