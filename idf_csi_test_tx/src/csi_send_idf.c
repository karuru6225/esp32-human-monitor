/*
 * SpecificDeviceCSI TX - 素のESP-IDF版
 *
 * Espressif公式 esp-csi の examples/get-started/csi_send をベースに、
 * ESP32無印向けに簡略化。Arduinoのsetup()/loop()・WiFi.mode()等は
 * 一切経由せず、app_main()から素のesp_wifi_*系APIだけを呼ぶ。
 *
 * ArduinoのESP-NOW実装(framework-arduinoespressif32)には無い
 * esp_now_set_peer_rate_config()（送信レート明示指定）が、こちらの
 * espidf側では使えるので、公式サンプル通りHT40/MCS0_LGIを指定する。
 * RXは idf_csi_test/（AtomS3 Lite, ESP32-S3）を使う。
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
#define SEND_FREQUENCY_HZ 100

/* RX側（idf_csi_test/src/csi_recv_idf.c）がinfo->macでフィルタする値と一致させる */
static const uint8_t TX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t RX_MAC[6] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x02};

static const char *TAG = "csi_send_idf";

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
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, TX_MAC));
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
    memcpy(peer.peer_addr, RX_MAC, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* 公式サンプル通りのレート指定。Arduino版では使えなかったAPI */
    esp_now_rate_config_t rate_config = {
        .phymode = WIFI_PHY_MODE_HT40,
        .rate = WIFI_PHY_RATE_MCS0_LGI,
        .ersu = false,
        .dcm = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rate_config));
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

    ESP_LOGI(TAG, "================ CSI SEND (pure ESP-IDF, rate_config set) ================");
    ESP_LOGI(TAG, "wifi_channel=%d send_frequency=%dHz tx_mac=" MACSTR " rx_mac=" MACSTR,
             WIFI_CHANNEL, SEND_FREQUENCY_HZ, MAC2STR(TX_MAC), MAC2STR(RX_MAC));

    for (uint32_t count = 0;; ++count) {
        esp_err_t err = esp_now_send(RX_MAC, (const uint8_t *)&count, sizeof(count));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_now_send error: %s", esp_err_to_name(err));
        }
        if (count % SEND_FREQUENCY_HZ == 0) {
            ESP_LOGI(TAG, "sent count=%lu", (unsigned long)count);
        }
        vTaskDelay(pdMS_TO_TICKS(1000 / SEND_FREQUENCY_HZ));
    }
}
