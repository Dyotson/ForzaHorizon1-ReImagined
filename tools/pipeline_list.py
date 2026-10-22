#!/usr/bin/env python3
"""Turn the SDK's stored Vulkan pipeline descriptions into a list we can ship.

Phase 1 of the shader prebuild (#226, docs/shader_prebuild.md). A pipeline storage
file (<title>.fbo.vk.xpso / .fsi.vk.xpso) holds, per pipeline, the vertex and pixel
shader hashes, their translation modification bits and the render state. That is all
the list keeps. Shader microcode stays in the player's game files: the list only says
how to find each shader there (tools/fxobj_shaders.py):
  "file"   the shader is a blob in the game files with exactly this hash;
  "bound"  a vertex shader as the game's D3D runtime bound it to a vertex layout and a
           pixel shader: the blob with hash `base`, plus per changed instruction only
           the bound fields: vertex fetch layout (fetch slot, format, stride, offset,
           swizzle; FETCH_LAYOUT_MASK), export destination (EXPORT_MASK), or "nop"
           for an export the pixel shader does not read (replaced by ALU_NOP);
  "missing" not found in the game files (engine/runtime shaders): built in-game as now.

Usage:
  pipeline_list.py make XPSO... [--xsh XSH]... [--sources SRC]... -o LIST.json
  pipeline_list.py merge LIST... [--mixed-versions] -o LIST.json
  pipeline_list.py check LIST.json        verify it holds only hashes, fields and
                                          layout values (no microcode)
  pipeline_list.py stats XPSO...          version, layout, count, and triangle fans
                                          (records every warm-up rejects; see the doc)
  pipeline_list.py write-storage LIST.json --sources SRC... --out DIR
                                          experiment: rebuild the listed shaders from the
                                          player's files and write SDK storage files
                                          (.xsh, .xpso) that today's warm-up loads as is
--xsh and --sources (game data directories, executable image) enable shader recipes.
"""
import argparse
import json
import os
import re
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xxh3 import xxh3_64  # noqa: E402
from fxobj_shaders import (EXPORT_MASK, FETCH_LAYOUT_MASK, apply_binding,  # noqa: E402
                           derive_binding, patch_size)

LIST_FORMAT = 1
XPSO_MAGIC = 0x53504558  # 'XEPS'

# PipelineDescription (sdk/include/rex/graphics/vulkan/pipeline_cache.h) after the four
# u64 (VS hash, VS modification, PS hash, PS modification) and the u32 RenderPassKey.
# Packed bit fields: Clang/GCC (Linux, macOS) pack them back to back (58-byte
# description); MSVC (Windows) starts the 13 bits after the first 32 in a new u32
# (60 bytes). The bit positions are the same; only the padding differs.
STATE_FIELDS = [
    ("geometry_shader", 2), ("primitive_topology", 3), ("primitive_restart", 1),
    ("tessellation_mode", 2), ("depth_clamp_enable", 1), ("polygon_mode", 2),
    ("cull_front", 1), ("cull_back", 1), ("front_face_clockwise", 1),
    ("rasterizer_discard", 1), ("depth_write_enable", 1), ("depth_compare_op", 3),
    ("stencil_test_enable", 1), ("stencil_front_fail_op", 3), ("stencil_front_pass_op", 3),
    ("stencil_front_depth_fail_op", 3), ("stencil_front_compare_op", 3),
    ("stencil_back_fail_op", 3), ("stencil_back_pass_op", 3),
    ("stencil_back_depth_fail_op", 3), ("stencil_back_compare_op", 3),
    ("sample_rate_shading", 1),
]
RENDER_PASS_FIELDS = [
    ("msaa_samples", 2), ("depth_and_color_used", 5), ("depth_format", 1),
    ("color_0_view_format", 4), ("color_1_view_format", 4), ("color_2_view_format", 4),
    ("color_3_view_format", 4), ("color_rts_use_transfer_formats", 1),
]
RT_FIELDS = [
    ("src_color_blend_factor", 4), ("dst_color_blend_factor", 4), ("color_blend_op", 3),
    ("src_alpha_blend_factor", 4), ("dst_alpha_blend_factor", 4), ("alpha_blend_op", 3),
    ("color_write_mask", 4),
]
LAYOUTS = {"clang": 58, "msvc": 60}
TOPOLOGY_TRIANGLE_FAN = 5

def unpack_bits(value, fields):
    out, pos = {}, 0
    for name, width in fields:
        out[name] = (value >> pos) & ((1 << width) - 1)
        pos += width
    return out


def pack_bits(values, fields):
    v, pos = 0, 0
    for name, width in fields:
        v |= (values[name] & ((1 << width) - 1)) << pos
        pos += width
    return v


def decode_description(d, layout):
    vs, vsm, ps, psm, rpk = struct.unpack_from("<QQQQI", d, 0)
    if layout == "clang":
        state = int.from_bytes(d[36:42], "little")
        rt_at = 42
    else:
        state = int.from_bytes(d[36:40], "little") | (int.from_bytes(d[40:44], "little") << 32)
        rt_at = 44
    p = {
        "vs": f"{vs:016X}", "vs_mod": f"{vsm:016X}",
        "ps": f"{ps:016X}", "ps_mod": f"{psm:016X}",
        "render_pass": unpack_bits(rpk, RENDER_PASS_FIELDS),
        "state": unpack_bits(state, STATE_FIELDS),
        "render_targets": [unpack_bits(struct.unpack_from("<I", d, rt_at + 4 * i)[0], RT_FIELDS)
                           for i in range(4)],
    }
    return p


def encode_description(p, layout):
    """Native PipelineDescription bytes (for re-creating a storage file locally)."""
    head = struct.pack("<QQQQI", int(p["vs"], 16), int(p["vs_mod"], 16), int(p["ps"], 16),
                       int(p["ps_mod"], 16), pack_bits(p["render_pass"], RENDER_PASS_FIELDS))
    state = pack_bits(p["state"], STATE_FIELDS)
    if layout == "clang":
        mid = state.to_bytes(6, "little")
    else:
        mid = (state & 0xFFFFFFFF).to_bytes(4, "little") + (state >> 32).to_bytes(4, "little")
    rts = b"".join(struct.pack("<I", pack_bits(rt, RT_FIELDS)) for rt in p["render_targets"])
    return head + mid + rts


def read_xpso(path):
    """(header dict, layout, [description bytes]) of a pipeline storage file."""
    with open(path, "rb") as f:
        d = f.read()
    magic, api, version_be = struct.unpack_from("<III", d, 0)
    if magic != XPSO_MAGIC:
        raise ValueError(f"{path}: not a pipeline storage file")
    header = {"api": "fsi" if api else "fbo", "version": f"{struct.unpack('>I', d[8:12])[0]:08X}"}
    body = d[12:]
    best = None
    for layout, size in LAYOUTS.items():
        entry = 8 + size
        n = len(body) // entry
        ok = 0
        for i in range(n):
            e = body[i * entry:(i + 1) * entry]
            if struct.unpack_from("<Q", e)[0] != xxh3_64(e[8:]):
                break
            ok += 1
        if best is None or ok > best[1]:
            best = (layout, ok, entry)
    layout, n, entry = best
    return header, layout, [body[i * entry + 8:(i + 1) * entry] for i in range(n)]


def pipeline_key(p):
    return json.dumps(p, sort_keys=True)


def partition_of(path):
    """Where the SDK keeps this file: 'ui' or 'scene' with split rendering, else 'main'."""
    parts = os.path.normpath(path).split(os.sep)
    i = parts.index("shaders") if "shaders" in parts else len(parts)
    parent = parts[i - 1] if i else ""
    return parent if parent in ("ui", "scene") else "main"


def make_shader_recipes(hashes_types, xsh_paths, sources):
    import fxobj_shaders as F
    stored = {}
    for p in xsh_paths:
        stored.update(F.read_xsh(p))
    shaders, _ = F.collect(sources) if sources else ({}, None)
    by_size = {}
    for s in shaders.values():
        if s.type == F.TYPE_VS:
            by_size.setdefault(len(s.ucode), []).append(s)
    recipes = {}
    for h in sorted(hashes_types):
        key = f"{h:016X}"
        if h in shaders:
            recipes[key] = {"type": F.TYPE_NAMES[shaders[h].type], "from": "file"}
            continue
        if h not in stored:
            recipes[key] = {"from": "missing", "why": "not in the given shader storage"}
            continue
        t, u = stored[h]
        best = None
        if t == F.TYPE_VS:
            for base in by_size.get(len(u), ()):
                patch = derive_binding(base.ucode, u)
                if patch is not None and (best is None or patch_size(patch) < patch_size(best[1])):
                    best = (base, patch)
        if best is not None:
            recipes[key] = dict({"type": "vs", "from": "bound", "base": f"{best[0].hash:016X}"},
                                **best[1])
            assert xxh3_64(apply_binding(best[0].ucode, best[1])) == h
            continue
        recipes[key] = {"type": F.TYPE_NAMES[t], "from": "missing",
                        "why": "not in the game files"}
    return recipes


def cmd_make(a):
    pipelines, versions, hashes = {}, set(), {}
    for path in a.xpso:
        header, layout, descs = read_xpso(path)
        versions.add((header["api"], header["version"]))
        part = partition_of(path)
        for d in descs:
            p = decode_description(d, layout)
            k = pipeline_key(p)
            entry = pipelines.setdefault(k, dict(p, partitions=[]))
            if part not in entry["partitions"]:
                entry["partitions"].append(part)
            hashes[int(p["vs"], 16)] = 0
            if int(p["ps"], 16):
                hashes[int(p["ps"], 16)] = 1
    out = {
        "format": LIST_FORMAT,
        "title": "4D5309C9",
        "storage_versions": sorted(f"{api}:{v}" for api, v in versions),
        "pipelines": sorted(pipelines.values(), key=pipeline_key),
    }
    if a.xsh:
        out["shaders"] = make_shader_recipes(hashes, a.xsh, a.sources or [])
    write_list(a.output, out)
    print(f"{len(out['pipelines'])} pipelines from {len(a.xpso)} files -> {a.output}")
    if "shaders" in out:
        c = Counter(r["from"] for r in out["shaders"].values())
        print("shaders:", dict(c))


def write_list(path, data):
    with open(path, "w") as f:
        json.dump(data, f, indent=0, sort_keys=True)
        f.write("\n")


def cmd_merge(a):
    pipelines, shaders, versions = {}, {}, set()
    for path in a.lists:
        with open(path) as f:
            lst = json.load(f)
        if lst.get("format") != LIST_FORMAT:
            raise SystemExit(f"{path}: list format {lst.get('format')}")
        versions.update(lst["storage_versions"])
        for p in lst["pipelines"]:
            parts = p.pop("partitions", [])
            e = pipelines.setdefault(pipeline_key(p), dict(p, partitions=[]))
            e["partitions"] = sorted(set(e["partitions"]) | set(parts))
        for h, r in lst.get("shaders", {}).items():
            old = shaders.get(h)
            if old is None or old["from"] == "missing":
                shaders[h] = r
            elif r["from"] != "missing" and r != old:
                raise SystemExit(f"conflicting recipes for shader {h}")
    if len(versions) > 1 and not a.mixed_versions:
        raise SystemExit(f"lists from different SDK storage versions {sorted(versions)}: "
                         "descriptions may not mean the same (--mixed-versions to merge anyway)")
    out = {"format": LIST_FORMAT, "title": "4D5309C9", "storage_versions": sorted(versions),
           "pipelines": sorted(pipelines.values(), key=pipeline_key), "shaders": shaders}
    write_list(a.output, out)
    print(f"{len(out['pipelines'])} pipelines, {len(shaders)} shaders -> {a.output}")


HEX16 = re.compile(r"^[0-9A-F]{16}$")


def cmd_check(a):
    with open(a.list) as f:
        lst = json.load(f)
    problems = []

    def bits_ok(values, fields, where):
        names = {n: w for n, w in fields}
        if set(values) != set(names):
            problems.append(f"{where}: unexpected fields {sorted(set(values) ^ set(names))}")
        for n, v in values.items():
            if not isinstance(v, int) or not 0 <= v < (1 << names.get(n, 0)):
                problems.append(f"{where}: {n}={v!r} out of range")

    allowed_top = {"format", "title", "storage_versions", "pipelines", "shaders"}
    if set(lst) - allowed_top:
        problems.append(f"unexpected top-level keys {sorted(set(lst) - allowed_top)}")
    for i, p in enumerate(lst["pipelines"]):
        w = f"pipeline {i}"
        if set(p) != {"vs", "vs_mod", "ps", "ps_mod", "render_pass", "state", "render_targets",
                      "partitions"}:
            problems.append(f"{w}: unexpected keys {sorted(p)}")
        for k in ("vs", "vs_mod", "ps", "ps_mod"):
            if not HEX16.match(str(p.get(k))):
                problems.append(f"{w}: {k} is not a 64-bit value")
        bits_ok(p["render_pass"], RENDER_PASS_FIELDS, w)
        bits_ok(p["state"], STATE_FIELDS, w)
        for rt in p["render_targets"]:
            bits_ok(rt, RT_FIELDS, w)
    for h, r in lst.get("shaders", {}).items():
        w = f"shader {h}"
        if not HEX16.match(h):
            problems.append(f"{w}: bad hash")
        if r["from"] == "bound":
            if not HEX16.match(r["base"]):
                problems.append(f"{w}: bad base hash")
            for e in r.get("fetch", ()):
                if len(e) != 4 or any(v & ~FETCH_LAYOUT_MASK[k] for k, v in enumerate(e[1:])):
                    problems.append(f"{w}: fetch entry {e} has bits outside the vertex layout")
            for e in r.get("export", ()):
                if len(e) != 2 or e[1] & ~EXPORT_MASK:
                    problems.append(f"{w}: export entry {e} has bits outside the export fields")
            if not all(isinstance(k, int) for k in r.get("nop", ())):
                problems.append(f"{w}: nop entries must be instruction indices")
        elif r["from"] not in ("file", "missing"):
            problems.append(f"{w}: unknown source {r['from']}")
        extra = set(r) - {"type", "from", "base", "fetch", "export", "nop", "why"}
        if extra:
            problems.append(f"{w}: unexpected keys {sorted(extra)}")
    size = os.path.getsize(a.list)
    print(f"{a.list}: {len(lst['pipelines'])} pipelines, {len(lst.get('shaders', {}))} shaders, "
          f"{size} bytes")
    if problems:
        for p in problems[:50]:
            print("PROBLEM:", p)
        raise SystemExit(1)
    print("ok: only hashes, render state, vertex layout and export fields")


def cmd_write_storage(a):
    """Write <title>.xsh and <title>.<api>.vk.xpso for the listed pipelines whose shaders
    the given sources (the player's own game files) provide. Local use only: the .xsh
    holds microcode."""
    import fxobj_shaders as F
    with open(a.list) as f:
        lst = json.load(f)
    shaders, _ = F.collect(a.sources)
    built, lost = {}, Counter()
    for key, r in lst.get("shaders", {}).items():
        h = int(key, 16)
        if r["from"] == "file" and h in shaders:
            built[h] = (shaders[h].type, shaders[h].ucode)
        elif r["from"] == "bound" and int(r["base"], 16) in shaders:
            u = apply_binding(shaders[int(r["base"], 16)].ucode, r)
            if xxh3_64(u) == h:
                built[h] = (F.TYPE_VS, u)
            else:
                lost["bound shader did not match its hash"] += 1
        else:
            lost[f"{r['from']} shader not available"] += 1
    os.makedirs(a.out, exist_ok=True)
    title = lst.get("title", "4D5309C9")
    with open(os.path.join(a.out, f"{title}.xsh"), "wb") as f:
        f.write(struct.pack("<I", 0x48534558) + struct.pack(">I", 0x20201219))
        for h, (t, u) in sorted(built.items()):
            f.write(struct.pack("<QI", h, (len(u) // 4) | (t << 31)) + u)
    n = 0
    with open(os.path.join(a.out, f"{title}.{a.api}.vk.xpso"), "wb") as f:
        f.write(struct.pack("<II", XPSO_MAGIC, 1 if a.api == "fsi" else 0) +
                struct.pack(">I", int(a.version, 16)))
        for p in lst["pipelines"]:
            p = {k: v for k, v in p.items() if k != "partitions"}
            if int(p["vs"], 16) not in built or (int(p["ps"], 16) and int(p["ps"], 16) not in built):
                lost["pipeline without its shaders"] += 1
                continue
            d = encode_description(p, a.layout)
            f.write(struct.pack("<Q", xxh3_64(d)) + d)
            n += 1
    print(f"{len(built)} shaders, {n} pipelines -> {a.out} ({a.layout} layout, version {a.version})")
    if lost:
        print("skipped:", dict(lost))


def cmd_stats(a):
    for path in a.xpso:
        header, layout, descs = read_xpso(path)
        topo = Counter()
        for d in descs:
            p = decode_description(d, layout)
            topo[p["state"]["primitive_topology"]] += 1
        fans = topo.get(TOPOLOGY_TRIANGLE_FAN, 0)
        print(f"{path}: {header['api']} v{header['version']} {layout} {len(descs)} pipelines, "
              f"{fans} triangle-fan (rejected at load)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("make")
    p.add_argument("xpso", nargs="+")
    p.add_argument("--xsh", action="append")
    p.add_argument("--sources", action="append")
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("merge")
    p.add_argument("lists", nargs="+")
    p.add_argument("--mixed-versions", action="store_true")
    p.add_argument("-o", "--output", required=True)
    p = sub.add_parser("check")
    p.add_argument("list")
    p = sub.add_parser("stats")
    p.add_argument("xpso", nargs="+")
    p = sub.add_parser("write-storage")
    p.add_argument("list")
    p.add_argument("--sources", action="append", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--layout", choices=LAYOUTS, default="msvc" if sys.platform == "win32" else "clang")
    p.add_argument("--api", choices=("fbo", "fsi"), default="fbo")
    p.add_argument("--version", default="20261018",
                   help="max(PipelineDescription::kVersion, Modification::kVersion) of the SDK")
    a = ap.parse_args()
    {"make": cmd_make, "merge": cmd_merge, "check": cmd_check, "stats": cmd_stats,
     "write-storage": cmd_write_storage}[a.cmd](a)


if __name__ == "__main__":
    main()
