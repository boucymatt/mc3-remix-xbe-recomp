"""Build a reversible, profile-gated MC3 motion-blur tune override.

The sidecar is generated from the user's own Xbox Remix assets_x.dat. It holds
the original compressed archive entry and an equally sized replacement whose
blur parameters are zero / out of range. The archive itself is never edited.
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

from generate_headlight_anchors import entries


NAME = "tune/vehicle/default.mcsenseofspeed"
REPLACEMENTS = {
    b"m_sstBlurScale 0.089999": b"m_sstBlurScale 0.000000",
    b"m_sstBlurAlpha 0.862548": b"m_sstBlurAlpha 0.000000",
    b"g_motionBlurFadeStart 0.006998": b"g_motionBlurFadeStart 9.000000",
    b"g_motionBlurFadeEnd 0.018998": b"g_motionBlurFadeEnd 9.999999",
}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--out", type=Path, default=Path("assets/mc3_motion_blur_override.bin"))
    args = parser.parse_args()
    with args.archive.open("rb") as archive:
        hits = [e for e in entries(archive) if e[0] == NAME]
        if len(hits) != 1:
            raise ValueError(f"expected one {NAME}, found {len(hits)}")
        _, offset, size, compressed_size = hits[0]
        archive.seek(offset)
        original = archive.read(compressed_size)
    data = zlib.decompress(original, -15) if compressed_size != size else original
    if len(data) != size:
        raise ValueError("bad uncompressed entry size")
    for old, new in REPLACEMENTS.items():
        if len(old) != len(new) or data.count(old) != 1:
            raise ValueError(f"expected exactly one equal-length {old!r}")
        data = data.replace(old, new)
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    patched = compressor.compress(data) + compressor.flush()
    if len(patched) > compressed_size:
        raise ValueError(f"replacement grows from {compressed_size} to {len(patched)}")
    patched = patched.ljust(compressed_size, b"\0")
    if zlib.decompress(patched, -15) != data:
        raise ValueError("replacement failed round trip")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(b"MC3B1" + struct.pack("<QI", offset, compressed_size) + original + patched)
    print(f"wrote {args.out}: archive offset {offset}, {compressed_size} bytes; source archive untouched")


if __name__ == "__main__":
    main()
