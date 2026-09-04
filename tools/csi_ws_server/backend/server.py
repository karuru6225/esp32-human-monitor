import asyncio
import json
import os

import serial
import websockets

PORT_NAME = os.environ.get("CSI_SERIAL_PORT", "/dev/ttyACM0")
BAUD = int(os.environ.get("CSI_SERIAL_BAUD", "2000000"))
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


async def serial_reader(loop):
    print(f"opening {PORT_NAME} @ {BAUD}", flush=True)
    ser = serial.Serial(PORT_NAME, BAUD, timeout=1)
    while True:
        line = await loop.run_in_executor(None, ser.readline)
        line = line.decode("utf-8", errors="replace").strip()
        if not line.startswith("CSI_HEX,"):
            continue
        parts = line.split(",", 3)
        if len(parts) != 4:
            continue
        try:
            seq = int(parts[1])
            rssi = int(parts[2])
            imag, real = decode_hex_to_iq(parts[3])
        except Exception:
            continue

        msg = json.dumps({"seq": seq, "rssi": rssi, "imag": imag, "real": real})
        for ws in list(clients):
            try:
                await ws.send(msg)
            except Exception:
                clients.discard(ws)


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
    loop = asyncio.get_event_loop()
    asyncio.create_task(serial_reader(loop))
    async with websockets.serve(handler, "0.0.0.0", WS_PORT):
        print(f"WebSocket server listening on :{WS_PORT}", flush=True)
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
