"""Build a reversible dry-road tune sidecar from the user's own archive."""
import argparse
import re
import struct
import zlib
from pathlib import Path

from generate_headlight_anchors import entries

FIELD = re.compile(rb'(\bm_roadShiniess[ \t]+)([0-9]+(?:\.[0-9]+)?)(?=[ \t\r\n])')


def make_patch(data):
    def zero(match):
        value = match[2]
        return match[1] + b'0' + b' ' * (len(value) - 1)
    patched, count = FIELD.subn(zero, data)
    if count != 1 or len(patched) != len(data):
        raise ValueError('expected exactly one road-shininess field')
    # The tune parser ignores indentation. Move it to trailing whitespace so
    # the modified stream still fits the archive's original compressed slot.
    compact = re.sub(rb'(?m)^[ \t]+', b'', patched)
    return compact.ljust(len(data), b' ')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--out', type=Path, default=Path('assets/mc3_road_override.bin'))
    args = parser.parse_args()
    patches = []
    with args.archive.open('rb') as archive:
        for name, offset, size, packed in list(entries(archive)):
            if not name.startswith('tune/') or not name.endswith('.parfileio'):
                continue
            archive.seek(offset)
            original = archive.read(packed)
            data = zlib.decompress(original, -15) if packed != size else original
            if len(data) != size:
                raise ValueError(f'bad entry size: {name}')
            if b'm_roadShiniess' not in data:
                continue
            modified = make_patch(data)
            if packed == size:
                replacement = modified
            else:
                candidates = []
                for level in range(1, 10):
                    for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
                        compressor = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
                        candidates.append(compressor.compress(modified) + compressor.flush())
                replacement = min(candidates, key=len)
                if len(replacement) > packed:
                    raise ValueError(f'replacement grows: {name}')
                replacement = replacement.ljust(packed, b'\0')
                if zlib.decompress(replacement, -15) != modified:
                    raise ValueError(f'round trip failed: {name}')
            patches.append((offset, original, replacement))
    if not patches:
        raise ValueError('no road-shininess tunes found')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('wb') as out:
        out.write(b'MC3R1' + struct.pack('<I', len(patches)))
        for offset, original, replacement in sorted(patches):
            out.write(struct.pack('<QI', offset, len(original)) + original + replacement)
    print(f'Wrote {len(patches)} dry-road tune overrides to {args.out}; archive unchanged')


if __name__ == '__main__':
    main()
