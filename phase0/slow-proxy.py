"""仅用于 CI：限速 LocalSend 上行，让回切测试发生在传输过程中。"""

import asyncio
import sys


async def relay(reader, writer, throttle=False):
    forwarded = 0
    try:
        while chunk := await reader.read(32768):
            if throttle:
                await asyncio.sleep(0.01)
            writer.write(chunk)
            await writer.drain()
            old = forwarded
            forwarded += len(chunk)
            if throttle and old < 1024 * 1024 <= forwarded:
                print("PROXY_UPLOAD_STARTED", flush=True)
    finally:
        writer.close()


async def accept(reader, writer):
    try:
        upstream_reader, upstream_writer = await asyncio.open_connection("127.0.0.1", 53318)
        await asyncio.gather(relay(reader, upstream_writer, True), relay(upstream_reader, writer))
    except (ConnectionError, OSError):
        writer.close()


async def main():
    server = await asyncio.start_server(accept, "127.0.0.1", 53320)
    print("PROXY_READY", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
