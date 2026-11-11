"""仅用于同桌面回环 CI：隔离副机注入的系统光标，避免反馈到主机。

源端原生拖拽、逻辑切屏坐标、拖拽协议与文件传输保持真实路径。
只将发往副机的 CINN/DMMV 物理位置改为主机光标中心。
格式依据 ProtocolTypes.cpp 和 PacketStreamFilter.cpp；测试使用明文协议。
"""

import asyncio
import struct
import sys


async def raw(reader, writer):
    try:
        while chunk := await reader.read(65536):
            writer.write(chunk)
            await writer.drain()
    finally:
        writer.close()


async def frames(reader, writer, center):
    try:
        while True:
            header = await reader.readexactly(4)
            size = struct.unpack(">I", header)[0]
            payload = await reader.readexactly(size)
            if payload[:4] in (b"CINN", b"DMMV") and len(payload) >= 8:
                payload = payload[:4] + center + payload[8:]
            writer.write(header + payload)
            await writer.drain()
    finally:
        writer.close()


async def main():
    center = struct.pack(">HH", int(sys.argv[1]), int(sys.argv[2]))

    async def accept(reader, writer):
        try:
            upstream_reader, upstream_writer = await asyncio.open_connection("127.0.0.1", 24800)
            await asyncio.gather(raw(reader, upstream_writer), frames(upstream_reader, writer, center))
        except (ConnectionError, OSError, asyncio.IncompleteReadError):
            writer.close()

    server = await asyncio.start_server(accept, "127.0.0.1", 24801)
    print("CONTROL_PROXY_READY", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
