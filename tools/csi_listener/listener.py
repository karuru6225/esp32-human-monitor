import socket
import struct
import csv
import time
import os
import threading
import json

PORT = int(os.environ.get("LISTEN_PORT", 5005))
POS_PORT = int(os.environ.get("POS_PORT", 5006))
OUT_PATH = os.environ.get("OUT_PATH", "/data/csi_subcarrier_log.csv")
POS_OUT_PATH = os.environ.get("POS_OUT_PATH", "/data/position_log.csv")


# device_mac(6bytes), seq(uint32), rssi(int16), num_subcarriers(uint16) — main.cppのCsiUdpHeaderと対応
# Docker越しだと送信元IPがブリッジのゲートウェイに化けて複数台を区別できないため、
# ペイロードに埋め込んだMACアドレスで機器を識別する
HEADER_FMT = "<6sIhH"
HEADER_LEN = struct.calcsize(HEADER_FMT)


def mac_to_str(raw: bytes) -> str:
    return ":".join(f"{b:02X}" for b in raw)


def csi_listener():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    print(f"[csi] Listening on UDP :{PORT}, writing to {OUT_PATH}", flush=True)

    header_written = os.path.exists(OUT_PATH) and os.path.getsize(OUT_PATH) > 0

    with open(OUT_PATH, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        while True:
            data, addr = sock.recvfrom(4096)
            mac_raw, seq, rssi, num_sc = struct.unpack_from(HEADER_FMT, data, 0)
            amps = struct.unpack_from(f"<{num_sc}f", data, HEADER_LEN)
            recv_t = time.time()
            if not header_written:
                writer.writerow(["recv_time", "device_mac", "seq", "rssi"] + [f"sc{i}" for i in range(num_sc)])
                header_written = True
            writer.writerow([f"{recv_t:.3f}", mac_to_str(mac_raw), seq, rssi] + [f"{a:.2f}" for a in amps])
            f.flush()


def position_listener():
    # スマホの位置報告アプリ（tools/position_reporter）からのJSON UDPを受信してログに残す
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", POS_PORT))
    print(f"[pos] Listening on UDP :{POS_PORT}, writing to {POS_OUT_PATH}", flush=True)

    header_written = os.path.exists(POS_OUT_PATH) and os.path.getsize(POS_OUT_PATH) > 0

    with open(POS_OUT_PATH, "a", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        if not header_written:
            writer.writerow(["recv_time", "label", "x", "y", "client_time", "addr"])
            f.flush()
        while True:
            data, addr = sock.recvfrom(1024)
            recv_t = time.time()
            try:
                obj = json.loads(data.decode("utf-8"))
                label = obj.get("label", "")
                x = obj.get("x", "")
                y = obj.get("y", "")
                client_time = obj.get("client_time", "")
            except Exception:
                label = data.decode("utf-8", errors="replace")
                x = ""
                y = ""
                client_time = ""
            writer.writerow([f"{recv_t:.3f}", label, x, y, client_time, addr[0]])
            f.flush()
            print(f"[pos] {label} x={x} y={y} @ {client_time} from {addr[0]}", flush=True)


if __name__ == "__main__":
    t1 = threading.Thread(target=csi_listener, daemon=True)
    t2 = threading.Thread(target=position_listener, daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
