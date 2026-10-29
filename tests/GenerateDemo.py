"""Generate an original synthetic test-chart PNG, with no model or vendor asset.

The RGB formula matches GradientFixture in ProviderTests.cpp. This is a decoder
test illustration, not a screenshot of ZBrush or evidence of Explorer behavior.
Uses only the Python standard library; optional for building the native code.
"""
from pathlib import Path
import struct
import zlib


def chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
        ">I", zlib.crc32(kind + payload) & 0xFFFFFFFF
    )


def main() -> None:
    rows = bytearray()
    for y in range(96):
        rows.append(0)  # PNG filter: None
        for x in range(96):
            rows.extend((x * 255 // 95, y * 255 // 95, ((x // 12 + y // 12) % 2) * 150 + 40))
    image = b"\x89PNG\r\n\x1a\n"
    image += chunk(b"IHDR", struct.pack(">IIBBBBB", 96, 96, 8, 2, 0, 0, 0))
    image += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    image += chunk(b"IEND", b"")
    destination = Path(__file__).resolve().parent.parent / "docs" / "assets" / "synthetic-preview.png"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as output:
        output.write(image)
    print("Generated docs/assets/synthetic-preview.png from the test-chart formula.")


if __name__ == "__main__":
    main()
