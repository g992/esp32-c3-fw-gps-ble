#!/usr/bin/env python3
"""
Helper utilities for building and validating OTA update streams.

Usage examples:
  # Print control-plane commands plus the first 5 chunk descriptors.
  python tools/ota_protocol_tester.py encode firmware.bin --chunks 5

  # Validate a log captured from the OTA status characteristic.
  python tools/ota_protocol_tester.py verify-log status.log --chunk-size 480
"""

from __future__ import annotations

import argparse
import hashlib
import json
from typing import Iterator, List, Optional, Tuple


_CRC32_POLY = 0xEDB88320


def _build_crc32_table() -> List[int]:
    table: List[int] = []
    for i in range(256):
        crc = i
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ _CRC32_POLY
            else:
                crc >>= 1
        table.append(crc & 0xFFFFFFFF)
    return table


_CRC32_TABLE = _build_crc32_table()


def _crc32_update(data: bytes, crc: int = 0xFFFFFFFF) -> int:
    value = crc
    for byte in data:
        value = ((value >> 8) ^ _CRC32_TABLE[(value ^ byte) & 0xFF]) & 0xFFFFFFFF
    return value


def compute_crc32(data: bytes) -> int:
    return _crc32_update(data) ^ 0xFFFFFFFF


def compute_file_digests(path: str) -> Tuple[int, str, int]:
    sha_ctx = hashlib.sha256()
    crc = 0xFFFFFFFF
    total_size = 0
    with open(path, "rb") as handle:
        while True:
            chunk = handle.read(131072)
            if not chunk:
                break
            total_size += len(chunk)
            sha_ctx.update(chunk)
            crc = _crc32_update(chunk, crc)
    return total_size, sha_ctx.hexdigest(), crc ^ 0xFFFFFFFF


def iter_chunks(path: str, chunk_size: int) -> Iterator[Tuple[int, bytes]]:
    with open(path, "rb") as handle:
        offset = 0
        while True:
            payload = handle.read(chunk_size)
            if not payload:
                break
            yield offset, payload
            offset += len(payload)


def handle_encode(args: argparse.Namespace) -> None:
    size, sha_hex, crc = compute_file_digests(args.firmware)
    sha_hex = sha_hex.upper()
    crc_hex = f"{crc:08X}"
    print(f"Firmware: {args.firmware}")
    print(f"Size    : {size} bytes")
    print(f"SHA256  : {sha_hex}")
    print(f"CRC32   : {crc_hex}")
    print()
    print("Control plane commands:")
    print(f"  CMD=START;SIZE={size};SHA256={sha_hex};CRC32={crc_hex}")
    print("  CMD=FINISH")
    print()
    total_chunks = (size + args.chunk_size - 1) // args.chunk_size or 1
    print(
        f"Chunk plan: {total_chunks} chunk(s) of up to {args.chunk_size} bytes each."
    )
    if args.chunks <= 0:
        return
    print(f"First {min(args.chunks, total_chunks)} chunk descriptors:")
    for index, (offset, payload) in enumerate(iter_chunks(args.firmware, args.chunk_size)):
        if index >= args.chunks:
            break
        crc_chunk = compute_crc32(payload)
        next_offset = offset + len(payload)
        print(
            f"  #{index:03d}: offset={offset:08d} len={len(payload):03d} crc={crc_chunk:08X} next={next_offset}"
        )
    print()
    sample_next = min(args.chunk_size, size)
    print(
        "Example chunk acknowledgement JSON that the firmware will emit "
        f"after the first packet:\n  "
        f"{{\"state\":\"chunk_ack\",\"next\":{sample_next},\"total\":{size}}}"
    )


def _parse_json(line: str) -> Optional[dict]:
    line = line.strip()
    if not line:
        return None
    start = line.find("{")
    end = line.rfind("}")
    if start == -1 or end == -1 or end < start:
        return None
    fragment = line[start : end + 1]
    return json.loads(fragment)


def handle_verify(args: argparse.Namespace) -> None:
    expected_total = args.expected_size
    expected_chunk_max = args.chunk_size
    next_offset = 0
    ready_seen = False
    last_ack_offset: Optional[int] = None
    errors: List[str] = []
    line_no = 0

    with open(args.log, "r", encoding="utf-8") as handle:
        for raw in handle:
            line_no += 1
            try:
                message = _parse_json(raw)
            except json.JSONDecodeError as exc:
                errors.append(f"[line {line_no}] invalid JSON: {exc}")
                continue
            if not message:
                continue
            state = message.get("state")
            if state == "receiving":
                total = message.get("total")
                if total is not None:
                    expected_total = expected_total or total
                received = message.get("received")
                if received is not None and received < next_offset:
                    errors.append(
                        f"[line {line_no}] receiving counter regressed ({received} < {next_offset})"
                    )
                continue
            if state == "chunk_ack":
                total = message.get("total")
                if total is not None:
                    expected_total = expected_total or total
                next_value = message.get("next")
                if not isinstance(next_value, int):
                    errors.append(f"[line {line_no}] chunk_ack missing 'next'")
                    continue
                if next_value <= (last_ack_offset or 0):
                    errors.append(
                        f"[line {line_no}] chunk_ack did not advance (next={next_value})"
                    )
                if expected_chunk_max and last_ack_offset is not None:
                    delta = next_value - last_ack_offset
                    if delta <= 0 or delta > expected_chunk_max:
                        errors.append(
                            f"[line {line_no}] chunk delta {delta} outside 1..{expected_chunk_max}"
                        )
                last_ack_offset = next_value
                next_offset = next_value
                continue
            if state == "ready":
                ready_seen = True
                continue
            if state == "error":
                errors.append(
                    f"[line {line_no}] device reported error: {message.get('message')}"
                )
                continue

    if expected_total is not None and last_ack_offset is not None:
        if last_ack_offset != expected_total:
            errors.append(
                f"Final acknowledged offset {last_ack_offset} does not match total {expected_total}"
            )
    if not ready_seen:
        errors.append("No 'ready' state observed in log")

    if errors:
        print("Verification failed:")
        for err in errors:
            print(f"  - {err}")
    else:
        print("Log verified successfully.")
        print(
            f"  Total bytes acknowledged: {last_ack_offset or 0} (expected {expected_total or 'unknown'})"
        )
        print("  Ready state observed at the end of the log.")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="OTA chunk/JSON tester")
    subparsers = parser.add_subparsers(dest="command", required=True)

    encode = subparsers.add_parser("encode", help="summarize a firmware image")
    encode.add_argument("firmware", help="path to the binary firmware file")
    encode.add_argument(
        "--chunk-size",
        type=int,
        default=480,
        help="chunk payload size in bytes (default: 480)",
    )
    encode.add_argument(
        "--chunks",
        type=int,
        default=5,
        help="number of chunk descriptors to print (default: 5, 0 to disable)",
    )
    encode.set_defaults(func=handle_encode)

    verify = subparsers.add_parser(
        "verify-log",
        help="validate a log captured from the OTA status characteristic",
    )
    verify.add_argument("log", help="path to a text file with JSON lines")
    verify.add_argument(
        "--chunk-size",
        type=int,
        default=480,
        help="maximum chunk payload; used when validating deltas",
    )
    verify.add_argument(
        "--expected-size",
        type=int,
        help="optional expected final firmware size",
    )
    verify.set_defaults(func=handle_verify)

    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
