"""Identify light-glare textures from owned assets; never embed game data in source."""
import argparse
import struct
import zlib
from pathlib import Path
from generate_headlight_anchors import entries

NAMES = {'fx_hlight_glow.tex', 'fx_hlight_star.tex', 'fx_rearlight_glow.tex',
         'fx_rearlight_glow2.tex', 'fx_glowstar.tex', 'fx_glow01.tex', 'fx_glow02.tex',
         'fxltglow.tex', 'fxltglow_dry_weather.tex', 'fx_starpattern.tex'}
BEAMS = {'fx_glowcone.tex'}


def texture_hash(data):
    # Same byte hash as gpu::Hash. The signature file stays in the build directory.
    mask = (1 << 64) - 1
    k0, k1 = 0x9E3779B97F4A7C15, 0xE7037ED1A0B428DB
    def mix(a, b):
        product = a * b
        return (product & mask) ^ (product >> 64)
    seed = 1469598103934665603 ^ k0
    i = 0
    while i + 16 <= len(data):
        x, y = struct.unpack_from('<QQ', data, i)
        seed = mix(x ^ k1, y ^ seed)
        i += 16
    if i < len(data):
        r = len(data) - i
        x = int.from_bytes(data[i:i + min(r, 8)], 'little')
        y = int.from_bytes(data[-8:], 'little') if r >= 8 else 0
        seed = mix(x ^ k1, y ^ seed ^ r)
    return mix(seed ^ k0, len(data) ^ k1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    records = set()
    with args.archive.open('rb') as archive:
        for name, offset, size, packed in list(entries(archive)):
            if not name.startswith('texture_x/') or Path(name).name not in NAMES | BEAMS:
                continue
            archive.seek(offset)
            data = archive.read(packed)
            if packed != size:
                data = zlib.decompress(data, -15)
            width, height, fmt, levels, _, _, _ = struct.unpack_from('<7H', data)
            if fmt not in (22, 26):
                raise ValueError(f'Unsupported light texture format: {name}')
            at = 14
            for _ in range(levels):
                length = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * (8 if fmt == 22 else 16)
                payload = data[at:at + length]
                if len(payload) != length:
                    raise ValueError(f'Truncated light texture: {name}')
                records.add((width, height, 0x0C if fmt == 22 else 0x0F, length, texture_hash(payload), 2 if Path(name).name in BEAMS else 1))
                at += length
                width, height = max(1, width // 2), max(1, height // 2)
    if not records:
        raise ValueError('No light-glare textures found')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(b'MC3L2' + struct.pack('<I', len(records)) +
                           b''.join(struct.pack('<IIIIQQ', *r) for r in sorted(records)))
    print(f'Wrote {len(records)} local light-glare signatures to {args.output}')


if __name__ == '__main__':
    main()
