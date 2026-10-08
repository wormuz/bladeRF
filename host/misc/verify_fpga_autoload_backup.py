#!/usr/bin/env python3
"""Decode an xA4 flash_backup fpgaA4 image and compare its autoload RBF.

This is read-only. It verifies the FX3 LZMA block stream stored after the
256-byte autoload metadata page against a candidate raw RBF.
"""

from __future__ import annotations

import argparse
import hashlib
import lzma
import struct
from pathlib import Path

IMAGE_HEADER_SIZE = 226
PAGE_SIZE = 256
AUTOLOAD_PAGE_SIZE = 256
BLOCK_SIZE = 60 * 1024


def parse_flash_image(path: Path) -> bytes:
    blob = path.read_bytes()
    if len(blob) < IMAGE_HEADER_SIZE:
        raise ValueError("flash image is shorter than its fixed header")
    data = blob[IMAGE_HEADER_SIZE:]
    if len(data) != 0x290000:
        raise ValueError(f"expected A4 flash region 0x290000 bytes, got {len(data):#x}")
    return data


def read_binkv(page: bytes) -> dict[str, str]:
    fields: dict[str, str] = {}
    offset = 0
    while offset < len(page):
        size = page[offset]
        if size == 0xFF:
            break
        end = offset + size + 3
        if end > len(page) or size < 3:
            raise ValueError("invalid autoload key/value record")
        record = page[offset + 1:offset + 1 + size]
        # The trailing two bytes are the firmware's little-endian CRC.
        text = record.decode("ascii")
        key = next((item for item in ("FMT", "ULEN", "CLEN", "LEN")
                    if text.startswith(item)), None)
        if key is None:
            raise ValueError(f"unknown autoload record: {text!r}")
        fields[key] = text[len(key):]
        offset = end
    return fields


def decode_blocks(compressed: bytes, output_size: int) -> bytes:
    output = bytearray()
    offset = 0
    while len(output) < output_size:
        if offset + 4 > len(compressed):
            raise ValueError("truncated LZMA block length")
        frame_size = struct.unpack_from("<I", compressed, offset)[0]
        offset += 4
        if frame_size < 5 or offset + frame_size > len(compressed):
            raise ValueError("invalid or truncated LZMA block")
        frame = compressed[offset:offset + frame_size]
        offset += frame_size
        props = frame[:5]
        prop = props[0]
        pb = prop // 45
        lp = (prop - pb * 45) // 9
        lc = prop - pb * 45 - lp * 9
        dict_size = struct.unpack_from("<I", props, 1)[0]
        remaining = output_size - len(output)
        expected = min(BLOCK_SIZE, remaining)
        decoder = lzma.LZMADecompressor(
            format=lzma.FORMAT_RAW,
            filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dict_size,
                      "lc": lc, "lp": lp, "pb": pb}],
        )
        decoded = decoder.decompress(frame[5:], max_length=expected)
        if len(decoded) != expected:
            raise ValueError(
                f"LZMA block decoded {len(decoded)}/{expected} expected bytes")
        output.extend(decoded)
    if offset != len(compressed):
        raise ValueError(f"compressed stream has {len(compressed) - offset} trailing bytes")
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("flash_image", type=Path)
    parser.add_argument("candidate_rbf", type=Path)
    parser.add_argument("--output-rbf", type=Path,
                        help="write the decoded flash image for volatile inspection")
    args = parser.parse_args()

    flash_data = parse_flash_image(args.flash_image)
    fields = read_binkv(flash_data[:AUTOLOAD_PAGE_SIZE])
    if fields.get("FMT") != "LZMA":
        raise ValueError(f"unsupported autoload format: {fields.get('FMT')!r}")
    output_size = int(fields["ULEN"])
    compressed_size = int(fields["CLEN"])
    compressed = flash_data[PAGE_SIZE:PAGE_SIZE + compressed_size]
    if len(compressed) != compressed_size:
        raise ValueError("compressed autoload stream is truncated")

    decoded = decode_blocks(compressed, output_size)
    if args.output_rbf is not None:
        args.output_rbf.write_bytes(decoded)
    stored_sha = hashlib.sha256(decoded).hexdigest()
    candidate_sha = hashlib.sha256(args.candidate_rbf.read_bytes()).hexdigest()
    print(f"autoload_format={fields['FMT']} uncompressed_bytes={output_size} "
          f"compressed_bytes={compressed_size}")
    print(f"flash_rbf_sha256={stored_sha}")
    print(f"candidate_rbf_sha256={candidate_sha}")
    print(f"matches_candidate={stored_sha == candidate_sha}")
    return 0 if stored_sha == candidate_sha else 1


if __name__ == "__main__":
    raise SystemExit(main())
