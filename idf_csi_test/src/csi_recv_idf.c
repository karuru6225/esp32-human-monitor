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
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"

#define WIFI_CHANNEL 11

/* CSIコールバックはWiFiタスクから呼ばれるため、その場でprintfすると
   タスクウォッチドッグに引っかかる（公式ドキュメント通り）。コールバックは
   キューに積むだけにして、実際のシリアル出力は別の低優先度タスクで行う。 */
#define CSI_QUEUE_LEN 20
#define CSI_BUF_MAX 512

typedef struct {
    uint32_t seq;
    int8_t rssi;
    uint16_t len;
    int8_t buf[CSI_BUF_MAX];
} csi_sample_t;

static QueueHandle_t s_csi_queue;
static volatile uint32_t s_csi_dropped = 0;

/* シリアル出力の実効スループットが低いこと(#25参照)が分かったので、
   受信自体は毎回(最大100Hz)拾いつつ、AVERAGE_WINDOW回分をバイト単位で
   単純平均してから1本だけキューに送る。出力レートは
   (TXの送信レート/AVERAGE_WINDOW)になる。加算・除算はCSIコールバック
   (WiFiタスク)の中で行うが、平均計算はAVERAGE_WINDOW回に1回だけなので
   毎回printfするより大幅に軽い。 */
#define AVERAGE_WINDOW 10
static int32_t s_accum_sum[CSI_BUF_MAX];
static int32_t s_accum_rssi_sum;
static int s_accum_count;
static uint16_t s_accum_len;
static uint32_t s_accum_seq;

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

    /* TX(csi_send_idf.c)がESP-NOWペイロード先頭に積んでいるcount値をseqとして使う。
       +15はESP-NOWのベンダー情報要素ヘッダ分のオフセット（公式サンプルと同じ）。 */
    uint32_t seq = 0;
    if (info->payload && info->payload_len >= 19) {
        memcpy(&seq, info->payload + 15, sizeof(seq));
    }

    uint16_t len = info->len < CSI_BUF_MAX ? info->len : CSI_BUF_MAX;
    if (s_accum_count == 0) {
        s_accum_len = len;
        s_accum_seq = seq;
    }
    /* サンプルごとにlenが微妙に変わることがあるので、短い方に合わせる */
    uint16_t use_len = len < s_accum_len ? len : s_accum_len;
    for (uint16_t i = 0; i < use_len; i++) {
        s_accum_sum[i] += info->buf[i];
    }
    s_accum_rssi_sum += info->rx_ctrl.rssi;
    s_accum_count++;

    if (s_accum_count >= AVERAGE_WINDOW) {
        csi_sample_t sample;
        sample.seq = s_accum_seq;
        sample.rssi = (int8_t)(s_accum_rssi_sum / s_accum_count);
        sample.len = s_accum_len;
        for (uint16_t i = 0; i < s_accum_len; i++) {
            sample.buf[i] = (int8_t)(s_accum_sum[i] / s_accum_count);
        }

        if (xQueueSend(s_csi_queue, &sample, 0) != pdTRUE) {
            s_csi_dropped++;
        }

        memset(s_accum_sum, 0, (size_t)s_accum_len * sizeof(int32_t));
        s_accum_rssi_sum = 0;
        s_accum_count = 0;
    }
}

/* 生バイト列をそのままhex文字列にする。10進変換(除算・符号判定)すら省き、
   1バイトあたりテーブル引き2回だけにする——今回のボトルネック
   （printfのvfprintf経由の書式化、特に%f）を避けるための最も軽い方法。
   振幅計算(sqrt)や符号解釈はPC側に委ねる（公式サンプルも生のI/Q値を送る）。 */
static const char s_hex_chars[17] = "0123456789abcdef";

static inline void byte_to_hex(uint8_t b, char *out)
{
    out[0] = s_hex_chars[(b >> 4) & 0xF];
    out[1] = s_hex_chars[b & 0xF];
}

/* 実際の出力は低優先度タスクで行う。1行分をバッファに組み立てて
   fwrite一発で出すことで、printfの反復呼び出しオーバーヘッドを避ける。
   フォーマット: CSI_HEX,<seq>,<rssi>,<生バイト列のhex文字列>\n
   （バイト列はimag,real,imag,real...の並び。1バイト=hex2文字固定なので
   区切り文字は不要、PC側で2文字ずつ切ってsigned int8として解釈する） */
static char s_line_buf[4096];

static void csi_print_task(void *arg)
{
    csi_sample_t sample;
    while (1) {
        if (xQueueReceive(s_csi_queue, &sample, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int pos = snprintf(s_line_buf, sizeof(s_line_buf), "CSI_HEX,%lu,%d,",
                            (unsigned long)sample.seq, sample.rssi);
        for (int i = 0; i < sample.len && pos < (int)sizeof(s_line_buf) - 3; i++) {
            byte_to_hex((uint8_t)sample.buf[i], s_line_buf + pos);
            pos += 2;
        }
        s_line_buf[pos++] = '\n';
        fwrite(s_line_buf, 1, pos, stdout);
    }
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

    s_csi_queue = xQueueCreate(CSI_QUEUE_LEN, sizeof(csi_sample_t));
    /* CPU0はWiFiタスク+IDLE0がいるので、印字タスクはCPU1に固定して
       CPU0のIDLE0がウォッチドッグをリセットする時間を奪わないようにする */
    xTaskCreatePinnedToCore(csi_print_task, "csi_print", 4096, NULL, 5, NULL, 1);

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
        ESP_LOGI(TAG, "[heartbeat] csi_matched=%lu(+%lu) csi_total_cb=%lu(+%lu) promisc_total=%lu(+%lu) promisc_matched=%lu(+%lu) dropped=%lu last_unmatched_mac=" MACSTR,
                 (unsigned long)s_csi_matched, (unsigned long)(s_csi_matched - last_matched),
                 (unsigned long)s_csi_total_cb, (unsigned long)(s_csi_total_cb - last_total),
                 (unsigned long)s_promisc_total, (unsigned long)(s_promisc_total - last_promisc),
                 (unsigned long)s_promisc_matched, (unsigned long)(s_promisc_matched - last_promisc_matched),
                 (unsigned long)s_csi_dropped, MAC2STR(s_last_unmatched_mac));
        last_matched = s_csi_matched;
        last_total = s_csi_total_cb;
        last_promisc = s_promisc_total;
        last_promisc_matched = s_promisc_matched;
    }
}
