import asyncio
import json
import os

import websockets

# CSI_SOURCE=serial: CSI_SERIAL_PORTのシリアルデバイスを直接読む(要device passthrough)
# CSI_SOURCE=tcp(既定): serial_bridge.py等がTCPで中継する生データを読む
#   (WSL2標準カーネルにftdi_sioが無くFTDI機器をコンテナへ直接渡せない場合の回避策)
#   複数ノードを同時購読できる。CSI_TCP_SOURCES="ラベル:host:port,ラベル:host:port,..."
SOURCE = os.environ.get("CSI_SOURCE", "tcp")
TCP_SOURCES = os.environ.get(
    "CSI_TCP_SOURCES", "A:host.docker.internal:9000,B:host.docker.internal:9001"
)
SERIAL_PORT = os.environ.get("CSI_SERIAL_PORT", "/dev/ttyACM0")
SERIAL_BAUD = int(os.environ.get("CSI_SERIAL_BAUD", "2000000"))
SERIAL_LABEL = os.environ.get("CSI_SERIAL_LABEL", "A")
WS_PORT = int(os.environ.get("CSI_WS_PORT", "8765"))

clients = set()


def decode_hex_to_iq(hex_str):
    raw = bytes.fromhex(hex_str)
    imag, real = [], []
    for i in range(0, len(raw) - 1, 2):
        a = raw[i]
        b = raw[i + 1]
        if a > 127:
            a -= 256
        if b > 127:
            b -= 256
        imag.append(a)
        real.append(b)
    return imag, real


async def broadcast_line(node, line):
    line = line.decode("utf-8", errors="replace").strip()
    if not line.startswith("CSI_HEX,"):
        return
    parts = line.split(",", 3)
    if len(parts) != 4:
        return
    try:
        seq = int(parts[1])
        rssi = int(parts[2])
        imag, real = decode_hex_to_iq(parts[3])
    except Exception:
        return

    msg = json.dumps({"node": node, "seq": seq, "rssi": rssi, "imag": imag, "real": real})
    for ws in list(clients):
        try:
            await ws.send(msg)
        except Exception:
            clients.discard(ws)


async def serial_reader(loop):
    import serial

    print(f"[{SERIAL_LABEL}] opening {SERIAL_PORT} @ {SERIAL_BAUD}", flush=True)
    ser = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=1)
    while True:
        line = await loop.run_in_executor(None, ser.readline)
        await broadcast_line(SERIAL_LABEL, line)


async def tcp_reader(node, host, port):
    print(f"[{node}] connecting to {host}:{port}", flush=True)
    while True:
        try:
            reader, _writer = await asyncio.open_connection(host, port)
            print(f"[{node}] tcp source connected", flush=True)
            while True:
                line = await reader.readline()
                if not line:
                    print(f"[{node}] tcp source closed, retrying in 2s", flush=True)
                    break
                await broadcast_line(node, line)
        except Exception as e:
            print(f"[{node}] tcp source error: {e}, retrying in 2s", flush=True)
        await asyncio.sleep(2)


async def handler(websocket):
    clients.add(websocket)
    print(f"client connected ({len(clients)} total)", flush=True)
    try:
        async for _ in websocket:
            pass  # クライアントからは何も受け取らない、接続維持だけ
    finally:
        clients.discard(websocket)
        print(f"client disconnected ({len(clients)} total)", flush=True)


async def main():
    if SOURCE == "tcp":
        for entry in TCP_SOURCES.split(","):
            entry = entry.strip()
            if not entry:
                continue
            node, host, port = entry.split(":")
            asyncio.create_task(tcp_reader(node, host, int(port)))
    else:
        loop = asyncio.get_event_loop()
        asyncio.create_task(serial_reader(loop))
    async with websockets.serve(handler, "0.0.0.0", WS_PORT):
        print(f"WebSocket server listening on :{WS_PORT}", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
