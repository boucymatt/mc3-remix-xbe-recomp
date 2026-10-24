#!/usr/bin/env python3
"""Compare XboxXBERecomp F3 draws with xboxrecomp's [FTRACE] oracle.

The two renderers have deliberately different implementations.  This tool
normalizes only original Xbox/NV2A state, aligns draw sequences by their
geometry and texture signatures, and reports the first state divergence.
It can also rank likely projected-light passes in a fast capture alone.
"""

from __future__ import annotations

import argparse
import difflib
import json
import re
import struct
from pathlib import Path
from typing import Any


FAST_DRAW = re.compile(
    r"^(?P<seq>\d+) draw rt (?P<rt>[0-9A-F]+) ds (?P<ds>[0-9A-F]+) "
    r"prim (?P<prim>\d+) count (?P<count>\d+) vs (?P<vs>[0-9A-F]+) ps (?P<ps>[0-9A-F]+) "
    r"blend (?P<blend>\d+) (?P<src>[0-9A-F]+) (?P<dst>[0-9A-F]+) "
    r"zen (?P<ztest>\d+) (?P<zfunc>[0-9A-F]+) zwrite (?P<zwrite>\d+)"
)
FAST_TEX = re.compile(r"\| t(?P<stage>\d+) (?P<data>[0-9A-F]+) fmt (?P<fmt>[0-9A-F]+) (?P<w>\d+)x(?P<h>\d+)")
ORACLE_TEX = re.compile(r"\| t(?P<stage>\d+) (?P<data>[0-9A-F]+) (?P<w>\d+)x(?P<h>\d+) fmt (?P<fmt>[0-9A-F]+)")
ORACLE_DRAW = re.compile(
    r"\[FTRACE\]\s+(?P<path>PRG|FIX) prim (?P<prim>\d+) n (?P<count>\d+) "
    r"surf (?P<rt>[0-9A-F]+) (?P<w>\d+)x(?P<h>\d+)\+(?P<x>\d+)\+(?P<y>\d+) "
    r"pitch (?P<pitch>\d+) \| tex (?P<tex_valid>on|off) (?P<tex>[0-9A-F]+) "
    r"(?P<tw>\d+)x(?P<th>\d+) fmt (?P<fmt>[0-9A-F]+) \| blend (?P<blend>\d+) "
    r"(?P<src>[0-9A-F]+)/(?P<dst>[0-9A-F]+) eq (?P<op>[0-9A-F]+) \| "
    r"z test (?P<ztest>\d+) func (?P<zfunc>[0-9A-F]+) mask (?P<zwrite>\d+)"
    r"(?: \| tris (?P<tris>\d+) px (?P<pixels>\d+))?"
)


def hx(value: str) -> int:
    return int(value, 16)


def decimal(value: str) -> int:
    return int(value, 10)


def key_values(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-zA-Z_][a-zA-Z0-9_]*)=([^\s]+)", line))


def parse_fast(path: Path) -> list[dict[str, Any]]:
    draws: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = FAST_DRAW.match(raw)
        if match:
            g = match.groupdict()
            current = {
                "source": "XboxXBERecomp",
                "seq": decimal(g["seq"]),
                "rt": hx(g["rt"]),
                "ds": hx(g["ds"]),
                "prim": decimal(g["prim"]),
                "count": decimal(g["count"]),
                "vs": hx(g["vs"]),
                "ps": hx(g["ps"]),
                "blend": decimal(g["blend"]),
                "src": hx(g["src"]),
                "dst": hx(g["dst"]),
                "ztest": decimal(g["ztest"]),
                "zfunc": hx(g["zfunc"]),
                "zwrite": decimal(g["zwrite"]),
                "textures": {},
                "rc_stages": [],
                "ff_stages": [],
            }
            for tex in FAST_TEX.finditer(raw):
                t = tex.groupdict()
                current["textures"][decimal(t["stage"])] = {
                    "data": hx(t["data"]), "fmt": hx(t["fmt"]),
                    "w": decimal(t["w"]), "h": decimal(t["h"]),
                }
            draws.append(current)
            continue
        if current is None:
            continue
        stripped = raw.strip()
        if stripped.startswith("nv2a "):
            values = key_values(stripped)
            hex_fields = {"op", "blend_color", "afunc", "comb_count", "tex_modes", "alphakill", "proj"}
            dec_fields = {"atest", "aref", "scissor", "ps_prog"}
            for name in hex_fields:
                if name in values:
                    current[name] = hx(values[name])
            for name in dec_fields:
                if name in values:
                    current[name] = decimal(values[name])
            if "viewport" in values:
                current["viewport"] = [decimal(x) for x in values["viewport"].split(",")]
        elif stripped.startswith("rc final="):
            a, b = stripped.removeprefix("rc final=").split("/")
            current["final0"], current["final1"] = hx(a), hx(b)
        elif re.match(r"rc\d+ ", stripped):
            values = key_values(stripped)
            stage = decimal(re.match(r"rc(\d+)", stripped).group(1))  # type: ignore[union-attr]
            rgb_in, rgb_out = values["rgb"].split("/")
            a_in, a_out = values["alpha"].split("/")
            current["rc_stages"].append({
                "stage": stage, "rgb_in": hx(rgb_in), "rgb_out": hx(rgb_out),
                "a_in": hx(a_in), "a_out": hx(a_out),
            })
        elif re.match(r"ff\d+ ", stripped):
            values = key_values(stripped)
            current["ff_stages"].append({
                "color": [hx(x) for x in values["color"].split(",")],
                "alpha": [hx(x) for x in values["alpha"].split(",")],
                "result": hx(values["result"]),
            })
        elif stripped.startswith("stage"):
            sm = re.match(r"stage(\d+)", stripped)
            values = key_values(stripped)
            if sm and "data" in values:
                stage = decimal(sm.group(1))
                tex = current["textures"].setdefault(stage, {})
                for name in ("data", "format"):
                    if name in values:
                        tex["fmt" if name == "format" else name] = hx(values[name])
                if "size" in values:
                    tex["w"], tex["h"] = [decimal(x) for x in values["size"].split("x")]
    return draws


def parse_oracle(path: Path) -> list[dict[str, Any]]:
    draws: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = ORACLE_DRAW.search(raw)
        if match:
            g = match.groupdict()
            current = {
                "source": "xboxrecomp",
                "seq": len(draws) + 1,
                "path": g["path"],
                "rt": hx(g["rt"]),
                "prim": decimal(g["prim"]),
                "count": decimal(g["count"]),
                "blend": decimal(g["blend"]),
                "src": hx(g["src"]),
                "dst": hx(g["dst"]),
                "op": hx(g["op"]),
                "ztest": decimal(g["ztest"]),
                "zfunc": hx(g["zfunc"]),
                "zwrite": decimal(g["zwrite"]),
                "textures": {},
                "rc_stages": [],
            }
            if g["tex_valid"] == "on":
                current["textures"][0] = {
                    "data": hx(g["tex"]), "fmt": hx(g["fmt"]),
                    "w": decimal(g["tw"]), "h": decimal(g["th"]),
                }
            coverage = re.search(r"\| tris (\d+) px (\d+)", raw)
            if coverage:
                current["visible_tris"] = decimal(coverage.group(1))
                current["covered_pixels"] = decimal(coverage.group(2))
            draws.append(current)
            continue
        if current is None:
            continue
        if "[FTRACE]     rc stages " in raw:
            match = re.search(r"rc stages (\d+) ctl ([0-9A-F]+) prog ([0-9A-F]+) fin ([0-9A-F]+) ([0-9A-F]+)", raw)
            if match:
                current.update({
                    "comb_count": decimal(match.group(1)), "comb_control": hx(match.group(2)),
                    "tex_modes": hx(match.group(3)), "final0": hx(match.group(4)), "final1": hx(match.group(5)),
                })
                for tex in ORACLE_TEX.finditer(raw):
                    t = tex.groupdict()
                    current["textures"][decimal(t["stage"])] = {
                        "data": hx(t["data"]), "fmt": hx(t["fmt"]),
                        "w": decimal(t["w"]), "h": decimal(t["h"]),
                    }
        elif "[FTRACE]       s" in raw:
            match = re.search(r"s(\d+) icw ([0-9A-F]+) ([0-9A-F]+) ocw ([0-9A-F]+) ([0-9A-F]+)", raw)
            if match:
                current["rc_stages"].append({
                    "stage": decimal(match.group(1)), "rgb_in": hx(match.group(2)), "a_in": hx(match.group(3)),
                    "rgb_out": hx(match.group(4)), "a_out": hx(match.group(5)),
                })
    return draws


def texture0(draw: dict[str, Any]) -> dict[str, Any]:
    return draw.get("textures", {}).get(0, {})


def anchor(draw: dict[str, Any]) -> tuple[int, ...]:
    tex = texture0(draw)
    return (
        draw.get("prim", -1), draw.get("count", -1), tex.get("fmt", -1),
        tex.get("w", -1), tex.get("h", -1), draw.get("blend", -1),
    )


COMPARE_FIELDS = (
    "prim", "count", "blend", "src", "dst", "op", "ztest", "zfunc", "zwrite",
    "comb_count", "tex_modes", "final0", "final1",
)


def differences(fast: dict[str, Any], oracle: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for field in COMPARE_FIELDS:
        if field in fast and field in oracle and fast[field] != oracle[field]:
            result[field] = {"fast": fast[field], "oracle": oracle[field]}
    ft, ot = texture0(fast), texture0(oracle)
    for field in ("fmt", "w", "h"):
        if field in ft and field in ot and ft[field] != ot[field]:
            result[f"texture0.{field}"] = {"fast": ft[field], "oracle": ot[field]}
    if fast.get("rc_stages") and oracle.get("rc_stages") and fast["rc_stages"] != oracle["rc_stages"]:
        result["rc_stages"] = {"fast": fast["rc_stages"], "oracle": oracle["rc_stages"]}
    return result


def projected_light_score(draw: dict[str, Any]) -> int:
    tex = texture0(draw)
    score = 0
    score += 3 if draw.get("blend") else 0
    score += 2 if draw.get("zwrite") == 0 else 0
    score += 1 if draw.get("src") in (1, 0x302) else 0
    score += 1 if draw.get("dst") in (1, 0x303) else 0
    score += 2 if tex else 0
    score += 1 if draw.get("proj", 0) or draw.get("ps_prog", 0) else 0
    score += 1 if 3 <= draw.get("count", 0) <= 128 else 0
    return score


def is_headlight_cone(draw: dict[str, Any]) -> bool:
    """Match the MC3 mcSpotlight pass established by the xboxrecomp oracle.

    Addresses are deliberately excluded: allocator placement differs between
    runs and runtimes.  The full NV2A state plus the 64x64 DXT5 input is stable.
    """
    tex = texture0(draw)
    stages = draw.get("rc_stages", [])
    stage0 = stages[0] if stages else {}
    state_match = (
        draw.get("prim") == 5
        and draw.get("blend") == 1
        and draw.get("src") == 0x302
        and draw.get("dst") == 1
        and draw.get("ztest") == 1
        and draw.get("zfunc") == 0x203
        and draw.get("zwrite") == 0
        and tex.get("fmt") == 0x0F
        and tex.get("w") == 64
        and tex.get("h") == 64
    )
    if not state_match:
        return False
    fixed = draw.get("ff_stages", [])
    if draw.get("ps_prog") == 0 and len(fixed) == 1:
        return fixed[0] == {
            "color": [4, 1, 2, 1], "alpha": [4, 1, 2, 1], "result": 1,
        }
    return (
        draw.get("comb_count") == 1
        and draw.get("tex_modes") == 1
        and draw.get("final0") == 0x0000000C
        and draw.get("final1") == 0x00001C80
        and stage0.get("rgb_in") == 0x04080000
        and stage0.get("a_in") == 0x14180000
        and stage0.get("rgb_out") == 0x00000C00
        and stage0.get("a_out") == 0x00000C00
    )


def bmp_alpha_stats(path: Path) -> dict[str, Any] | None:
    """Read the fourth byte of the runtime's top-down 32-bit BMP verbatim.

    Many BMP readers expose alpha as 255 for BITMAPINFOHEADER files even when
    the stored byte contains the decoded DXT5 alpha, so this intentionally
    avoids an image library.
    """
    if not path.is_file():
        return None
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        return None
    offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bits = struct.unpack_from("<H", data, 28)[0]
    if bits != 32 or width <= 0 or height == 0:
        return None
    pixels = abs(width) * abs(height)
    end = offset + pixels * 4
    if end > len(data):
        return None
    alpha = data[offset + 3:end:4]
    hist = [0] * 256
    for value in alpha:
        hist[value] += 1
    used = [i for i, n in enumerate(hist) if n]
    return {
        "path": str(path.resolve()),
        "width": width,
        "height": abs(height),
        "minimum": used[0],
        "maximum": used[-1],
        "unique_values": len(used),
        "mean": round(sum(i * n for i, n in enumerate(hist)) / pixels, 4),
        "transparent_pixels": hist[0],
        "opaque_pixels": hist[255],
    }


def headlight_report(draws: list[dict[str, Any]], capture_dir: Path | None = None) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for draw in draws:
        if not is_headlight_cone(draw):
            continue
        item = compact(draw)
        tex = texture0(draw)
        if capture_dir is not None and "data" in tex:
            stats = bmp_alpha_stats(capture_dir / f"tex_{tex['data']:08X}.bmp")
            if stats:
                item["texture_alpha"] = stats
        result.append(item)
    return result


def compact(draw: dict[str, Any]) -> dict[str, Any]:
    keys = ("seq", "prim", "count", "rt", "blend", "src", "dst", "op", "ztest", "zfunc", "zwrite",
            "atest", "afunc", "aref", "ps_prog", "comb_count", "tex_modes", "alphakill", "proj",
            "visible_tris", "covered_pixels")
    out = {key: draw[key] for key in keys if key in draw}
    out["texture0"] = texture0(draw)
    return out


def compare(fast: list[dict[str, Any]], oracle: list[dict[str, Any]]) -> dict[str, Any]:
    matcher = difflib.SequenceMatcher(a=[anchor(x) for x in fast], b=[anchor(x) for x in oracle], autojunk=False)
    aligned: list[dict[str, Any]] = []
    first: dict[str, Any] | None = None
    exact = 0
    for block in matcher.get_matching_blocks():
        for offset in range(block.size):
            fi, oi = block.a + offset, block.b + offset
            delta = differences(fast[fi], oracle[oi])
            if delta:
                item = {"fast_index": fi, "oracle_index": oi, "differences": delta,
                        "fast": compact(fast[fi]), "oracle": compact(oracle[oi])}
                aligned.append(item)
                if first is None:
                    first = item
            else:
                exact += 1
    return {
        "fast_draws": len(fast), "oracle_draws": len(oracle),
        "aligned_by_geometry": exact + len(aligned), "exact_state_matches": exact,
        "state_mismatches": len(aligned), "first_state_divergence": first,
        "alignment_ratio": matcher.ratio(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fast", type=Path, help="XboxXBERecomp gpu_captures/.../frame.txt")
    parser.add_argument("oracle", type=Path, nargs="?", help="xboxrecomp stderr log containing [FTRACE]")
    parser.add_argument("--out", type=Path, help="write JSON report here")
    parser.add_argument("--candidates", type=int, default=20, help="number of projected-light candidates to retain")
    args = parser.parse_args()

    fast = parse_fast(args.fast)
    candidates = sorted(fast, key=projected_light_score, reverse=True)[: max(args.candidates, 0)]
    report: dict[str, Any] = {
        "fast_capture": str(args.fast.resolve()),
        "fast_draws": len(fast),
        "headlight_cone_signature": {
            "description": "MC3 mcSpotlight draw: triangle list, 64x64 DXT5, SRC_ALPHA+ONE, LEQUAL/no-zwrite, v0*t0",
            "fast_matches": headlight_report(fast, args.fast.resolve().parent),
        },
        "projected_light_candidates": [dict(score=projected_light_score(x), **compact(x)) for x in candidates],
    }
    if args.oracle:
        oracle = parse_oracle(args.oracle)
        report["oracle_trace"] = str(args.oracle.resolve())
        report["headlight_cone_signature"]["oracle_matches"] = headlight_report(oracle)
        report["comparison"] = compare(fast, oracle)
    text = json.dumps(report, indent=2, sort_keys=False)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if fast else 2


if __name__ == "__main__":
    raise SystemExit(main())
