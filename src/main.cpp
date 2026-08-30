// ============================================================
// WiFi CSI 検証用ファームウェア（MQTTなし）
// env:esp32-s3（car-iot機・M5AtomS3 Lite）/ env:xiao-c3（XIAO ESP32C3）共用
//
// Espressif公式 esp-csi の get-started サンプルを参考に作成。
// シリアルに rssi/avg_amp/diff_score/filt を出力するほか、
// サブキャリアごとの振幅をWiFi UDPでPC(tools/csi_listener)へ送信する。
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
#include <math.h>

extern "C"
{
#include "esp_wifi.h"
}

#include "secrets.h"

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;

// サブキャリア別振幅データの送信先（PC側の受信スクリプト、tools/csi_udp_listener.py）
// 環境に合わせてPCのLAN IPをsecrets.hのPC_IP_ADDRに設定すること
const char *PC_IP = PC_IP_ADDR;
const uint16_t PC_PORT = 5005;

WiFiUDP udp;
WiFiUDP dataUdp;
IPAddress gatewayIP;

#define CSI_MAX_LEN 400
static int8_t prev_buf[CSI_MAX_LEN];
static int prev_len = 0;

// サブキャリア別振幅の最大本数（len=400バイト / 2 = 200サブキャリア分の余裕を見た値）
#define MAX_SUBCARRIERS 200
static float subcarrier_amp[MAX_SUBCARRIERS];
static uint32_t udp_seq = 0;

// PCへ送るパケットのヘッダ（tools/csi_listener/listener.py 側のstruct.unpack形式と対応）
// device_mac：自機のMACアドレス。Dockerのポート公開(NAT)経由だと送信元IPが
// ブリッジのゲートウェイに化けて複数台を区別できないため、ペイロード側で識別する
struct __attribute__((packed)) CsiUdpHeader
{
  uint8_t device_mac[6];
  uint32_t seq;
  int16_t rssi;
  uint16_t num_subcarriers;
};

static uint8_t own_mac[6] = {0};

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

  // サブキャリアごとの振幅を計算してPCへUDP送信（位置特定に使えるか検証用）
  // bufは (Q, I) が交互に並んでいる: buf[2n]=虚部, buf[2n+1]=実部
  int num_subcarriers = len / 2;
  if (num_subcarriers > MAX_SUBCARRIERS)
  {
    num_subcarriers = MAX_SUBCARRIERS;
  }
  for (int i = 0; i < num_subcarriers; i++)
  {
    float q = (float)csi_data[2 * i];
    float ii = (float)csi_data[2 * i + 1];
    subcarrier_amp[i] = sqrtf(q * q + ii * ii);
  }

  CsiUdpHeader hdr;
  memcpy(hdr.device_mac, own_mac, 6);
  hdr.seq = udp_seq++;
  hdr.rssi = info->rx_ctrl.rssi;
  hdr.num_subcarriers = (uint16_t)num_subcarriers;

  dataUdp.beginPacket(PC_IP, PC_PORT);
  dataUdp.write((uint8_t *)&hdr, sizeof(hdr));
  dataUdp.write((uint8_t *)subcarrier_amp, num_subcarriers * sizeof(float));
  dataUdp.endPacket();
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

  // 自機のMACアドレスを保存（PCへのUDP送信時に複数台を区別するため）
  WiFi.macAddress(own_mac);
  Serial.printf("Own MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                own_mac[0], own_mac[1], own_mac[2],
                own_mac[3], own_mac[4], own_mac[5]);

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
