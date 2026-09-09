#!/usr/bin/env python3
"""whm_bridge - UDP broadcast -> WebSocket mirror for the WHM fleet.
Dumb pipe: decodes nothing; the browser viewer parses. Also a packet
logger. Run on any LAN host, open tools/whm_viewer.html, connect."""
# ---- editable vars ---------------------------------------------------
UDP_PORT = 7777          # whm-link broadcast port
WS_PORT  = 8777          # viewer connects to ws://<this-host>:8777
BIND     = "0.0.0.0"
LOG      = None          # e.g. "capture.jsonl" to record everything
# ----------------------------------------------------------------------
import sys, json, time, asyncio, socket

def _dep_check():
    try:
        import websockets  # noqa: F401
        return True
    except ImportError:
        print("missing dependency: websockets")
        print("  install:  python3 -m pip install websockets")
        return False

async def main():
    import websockets
    clients = set()
    logf = open(LOG, "a") if LOG else None

    class Proto(asyncio.DatagramProtocol):
        def datagram_received(self, data, addr):
            msg = json.dumps({"t": round(time.time(), 3),
                              "ip": addr[0], "port": addr[1],
                              "n": len(data), "hex": data.hex()})
            if logf: logf.write(msg + "\n")
            dead = []
            for ws in clients:
                try: asyncio.create_task(ws.send(msg))
                except Exception: dead.append(ws)
            for ws in dead: clients.discard(ws)

    loop = asyncio.get_running_loop()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try: sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except (AttributeError, OSError): pass
    sock.bind((BIND, UDP_PORT))
    await loop.create_datagram_endpoint(Proto, sock=sock)

    async def handler(ws):
        clients.add(ws)
        try:
            async for _ in ws: pass
        finally:
            clients.discard(ws)

    async with websockets.serve(handler, BIND, WS_PORT):
        print(f"whm_bridge: UDP :{UDP_PORT} -> ws://<host>:{WS_PORT}"
              f"  (clients see raw sealed frames; viewer parses)")
        if LOG: print(f"whm_bridge: logging to {LOG}")
        await asyncio.Future()

if __name__ == "__main__":
    if not _dep_check(): sys.exit(1)
    try: asyncio.run(main())
    except KeyboardInterrupt: print("\nwhm_bridge: bye")
