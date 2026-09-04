/*
 * SpecificDeviceCSI RX - 素のESP-IDF版（Arduino frameworkが原因かどうかの切り分け用）
 *
 * Espressif公式 esp-csi の examples/get-started/csi_recv をベースに、
 * ESP32無印向けに簡略化（S3/C3/C5/C6/C61分岐・ゲイン補正コンポーネントは
 * 対象外なので削除）。Arduinoのsetup()/loop()・WiFi.mode()等は一切経由せず、
 * app_main()から素のesp_wifi_*系APIだけを呼ぶ。
 *
 * TXは既存の src/csi_tx.cpp（Arduino版、env:m5atom-lite-tx）をそのまま使う
 * ——promiscuousモードで拾う側なので、TXの宛先が誰かは無関係。
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"

#define WIFI_CHANNEL 11

/* TX側（src/csi_tx.cpp）が自分のSTA MACをこの値に上書きしているので、
   info->macがこれと一致するパケットだけをTX由来のCSIとして扱う。 */
static const uint8_t TX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t RX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x02};

static const char *TAG = "csi_recv_idf";

static volatile uint32_t s_csi_total_cb = 0;
static volatile uint32_t s_csi_matched = 0;
static volatile uint32_t s_promisc_total = 0;
static volatile uint32_t s_promisc_matched = 0;
static uint8_t s_last_unmatched_mac[6] = {0};

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW40));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_BELOW));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, RX_MAC));
}

static void wifi_esp_now_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));

    esp_now_peer_info_t peer = {
        .channel = WIFI_CHANNEL,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, TX_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* 公式サンプル通りのレート指定（RX側にもある。見落としていたので追加） */
    esp_now_rate_config_t rate_config = {
        .phymode = WIFI_PHY_MODE_HT40,
        .rate = WIFI_PHY_RATE_MCS0_LGI,
        .ersu = false,
        .dcm = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rate_config));
}

static void wifi_promisc_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    s_promisc_total++;
    /* 802.11フレームのaddr2(送信元MAC)はMACヘッダのoffset10から6byte */
    if (pkt->rx_ctrl.sig_len >= 16 && memcmp(pkt->payload + 10, TX_MAC, 6) == 0) {
        s_promisc_matched++;
    }
}

static void wifi_csi_rx_cb(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        return;
    }
    s_csi_total_cb++;

    if (memcmp(info->mac, TX_MAC, 6) != 0) {
        memcpy(s_last_unmatched_mac, info->mac, 6);
        return;
    }
    s_csi_matched++;
}

static void wifi_csi_init(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_promisc_rx_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_rx_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();
    wifi_esp_now_init();
    wifi_csi_init();

    uint8_t own_mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, own_mac);
    uint8_t actual_channel = 0;
    wifi_second_chan_t actual_second_chan;
    esp_wifi_get_channel(&actual_channel, &actual_second_chan);

    ESP_LOGI(TAG, "================ CSI RECV (pure ESP-IDF, no Arduino) ================");
    ESP_LOGI(TAG, "wifi_channel=%d(2nd=%d) own_mac=" MACSTR " filter_mac(tx)=" MACSTR,
             actual_channel, actual_second_chan, MAC2STR(own_mac), MAC2STR(TX_MAC));

    uint32_t last_matched = 0, last_total = 0, last_promisc = 0, last_promisc_matched = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "[heartbeat] csi_matched=%lu(+%lu) csi_total_cb=%lu(+%lu) promisc_total=%lu(+%lu) promisc_matched=%lu(+%lu) last_unmatched_mac=" MACSTR,
                 (unsigned long)s_csi_matched, (unsigned long)(s_csi_matched - last_matched),
                 (unsigned long)s_csi_total_cb, (unsigned long)(s_csi_total_cb - last_total),
                 (unsigned long)s_promisc_total, (unsigned long)(s_promisc_total - last_promisc),
                 (unsigned long)s_promisc_matched, (unsigned long)(s_promisc_matched - last_promisc_matched),
                 MAC2STR(s_last_unmatched_mac));
        last_matched = s_csi_matched;
        last_total = s_csi_total_cb;
        last_promisc = s_promisc_total;
        last_promisc_matched = s_promisc_matched;
    }
}
