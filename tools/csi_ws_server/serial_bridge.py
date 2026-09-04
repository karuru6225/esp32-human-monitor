"""COMポート(FTDI経由など、WSL2のDockerから直接アクセスできない機器向け)の
生データをTCPへ中継するブリッジ。Windowsローカルでこれだけ動かし、
backend(Docker)はhost.docker.internal経由でTCPに接続してCSIデータを読む。

WSL2標準カーネルにftdi_sioドライバが無くDocker側にCOMポートを直接
usbipdでattachできない問題への回避策（docs/research_log.md参照）。
"""
import argparse
import socket
import threading

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="COMポート (例: COM13)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--tcp-port", type=int, default=9000)
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=1)
    print(f"opened {args.port} @ {args.baud}, listening TCP :{args.tcp_port}", flush=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.tcp_port))
    srv.listen(1)

    while True:
        conn, addr = srv.accept()
        print(f"client connected: {addr}", flush=True)
        try:
            while True:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    conn.sendall(data)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            print(f"client disconnected: {type(e).__name__}: {e}", flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    main()
