#!/usr/bin/env python3
import socket
import struct
import time

from proto import location_pb2 as proto

HOST = "gps.local"
PORT = 8887
HEARTBEAT = b"\x01"
HEARTBEAT_INTERVAL = 1.0
SOCKET_TIMEOUT = 0.2


def read_frame(sock):
    header = sock.recv(4)
    if len(header) < 4:
        return None
    (length,) = struct.unpack(">I", header)
    payload = bytearray()
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            return None
        payload.extend(chunk)
    return bytes(payload)


def main():
    with socket.create_connection((HOST, PORT), timeout=5) as sock:
        sock.settimeout(SOCKET_TIMEOUT)
        last_beat = time.monotonic()
        while True:
            now = time.monotonic()
            if now - last_beat >= HEARTBEAT_INTERVAL:
                sock.sendall(HEARTBEAT)
                last_beat = now

            try:
                frame = read_frame(sock)
            except socket.timeout:
                continue

            if frame is None:
                print("Connection closed by server")
                break

            response = proto.ServerResponse()
            response.ParseFromString(frame)

            if response.HasField("location_update"):
                loc = response.location_update
                print(
                    f"fix lat={loc.latitude:.6f} lon={loc.longitude:.6f} "
                    f"alt={loc.altitude:.1f}m speed={loc.speed:.2f}m/s "
                    f"heading={loc.bearing:.1f}° sats={loc.satellites} "
                    f"age={loc.location_age:.2f}s"
                )
            elif response.HasField("status"):
                print(f"status: {response.status}")
            else:
                print("Received empty response")


if __name__ == "__main__":
    main()
