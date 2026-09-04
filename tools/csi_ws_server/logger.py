"""SpecificDeviceCSIのRXシリアル出力(CSI_HEX,...)を複数台同時にCSVへ記録する。

ドリフト検証用：位置ラベルは付けず、複数RXの生データをタイムスタンプ付きで残す。
"""
import argparse
import csv
import datetime
import os
import threading

import serial

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")


def reader_thread(port, baud, label, out_path, stop_event):
    ser = serial.Serial(port, baud, timeout=1)
    print(f"[{label}] opened {port} @ {baud} -> {out_path}", flush=True)
    count = 0
    with open(out_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["host_timestamp", "seq", "rssi", "raw_hex"])
        while not stop_event.is_set():
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if not line.startswith("CSI_HEX,"):
                continue
            parts = line.split(",", 3)
            if len(parts) != 4:
                continue
            _, seq, rssi, raw_hex = parts
            ts = datetime.datetime.now().isoformat()
            writer.writerow([ts, seq, rssi, raw_hex])
            count += 1
            if count % 200 == 0:
                f.flush()
                print(f"[{label}] {count} rows", flush=True)
    ser.close()
    print(f"[{label}] closed, total {count} rows", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port1", required=True, help="RX1のCOMポート (例: COM13)")
    parser.add_argument("--port2", required=True, help="RX2のCOMポート (例: COM16)")
    parser.add_argument("--label1", default="rx1")
    parser.add_argument("--label2", default="rx2")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    os.makedirs(DATA_DIR, exist_ok=True)
    run_ts = datetime.datetime.now().strftime("%y%m%d-%H%M%S")

    stop_event = threading.Event()
    threads = []
    for port, label in [(args.port1, args.label1), (args.port2, args.label2)]:
        out_path = os.path.join(DATA_DIR, f"csi_{label}_{run_ts}.csv")
        t = threading.Thread(
            target=reader_thread,
            args=(port, args.baud, label, out_path, stop_event),
            daemon=True,
        )
        threads.append(t)
        t.start()

    print("Ctrl+Cで停止", flush=True)
    try:
        while True:
            for t in threads:
                t.join(timeout=1)
    except KeyboardInterrupt:
        print("stopping...", flush=True)
        stop_event.set()
        for t in threads:
            t.join(timeout=3)


if __name__ == "__main__":
    main()
