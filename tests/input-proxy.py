"""仅用于同桌面回环 CI：隔离副机注入的系统光标，避免反馈到主机。

源端原生拖拽、逻辑切屏坐标、拖拽协议与文件传输保持真实路径。
只将发往副机的 CINN/DMMV 物理位置改为主机光标中心。
格式依据 ProtocolTypes.cpp 和 PacketStreamFilter.cpp；可选通过 --tls-server-cert/--tls-client-cert 转发 mTLS。
"""

import argparse
import asyncio
import ssl
import struct


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


def build_tls_contexts(server_cert, client_cert):
    if not server_cert and not client_cert:
        return None, None
    if not server_cert or not client_cert:
        raise ValueError("Both --tls-server-cert and --tls-client-cert must be supplied together")

    strict_flag = getattr(ssl, "VERIFY_X509_STRICT", 0)

    server_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    server_ctx.load_cert_chain(certfile=server_cert, keyfile=server_cert)
    server_ctx.verify_flags &= ~strict_flag
    server_ctx.load_verify_locations(cafile=client_cert)
    server_ctx.verify_mode = ssl.CERT_REQUIRED

    client_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    client_ctx.check_hostname = False
    client_ctx.load_cert_chain(certfile=client_cert, keyfile=client_cert)
    client_ctx.verify_flags &= ~strict_flag
    client_ctx.load_verify_locations(cafile=server_cert)
    client_ctx.verify_mode = ssl.CERT_REQUIRED

    return server_ctx, client_ctx


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("x", type=int)
    parser.add_argument("y", type=int)
    parser.add_argument("--tls-server-cert")
    parser.add_argument("--tls-client-cert")
    args = parser.parse_args()

    center = struct.pack(">HH", args.x, args.y)
    server_ctx, client_ctx = build_tls_contexts(args.tls_server_cert, args.tls_client_cert)

    async def accept(reader, writer):
        upstream_writer = None
        try:
            upstream_reader, upstream_writer = await asyncio.open_connection(
                "127.0.0.1", 24800, ssl=client_ctx
            )
            await asyncio.gather(raw(reader, upstream_writer), frames(upstream_reader, writer, center))
        except (ConnectionError, OSError, asyncio.IncompleteReadError, ssl.SSLError):
            pass
        finally:
            writer.close()
            if upstream_writer is not None:
                upstream_writer.close()

    server = await asyncio.start_server(accept, "127.0.0.1", 24801, ssl=server_ctx)
    print("CONTROL_PROXY_READY", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
