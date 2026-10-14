#!/usr/bin/env python3
"""Map guest addresses of one build of an executable to another (base default.xex ->
Title Update 4), for porting hints, hooks and notes.

Functions come from both images' .pdata (start, length). Each function's code is
normalized (branch displacements and 16-bit immediates of address-forming and memory
instructions masked) and the two function sequences are aligned with difflib, so runs of
unchanged functions match in order even where the update inserted or changed code. A code
address maps to the same offset in the matched function (only when both have the same
length). A function start the alignment left out goes where its callers' calls go (call
sites in matched functions of the same length), before similarity within the gap and the
code around it are tried. Data addresses map through the code that uses them: lis/addi (and lis + load or
store) pairs at the same offset of matched functions give anchor pairs, and an address
takes the shift of its nearest anchors when they agree.

Usage:
  tu_address_map.py OLD_XEX NEW_XEX ADDR...           print OLD -> NEW for each address
  tu_address_map.py OLD_XEX NEW_XEX --hints IN OUT    rewrite the 0x8xxxxxxx addresses of
                                                      a hint TOML / source file; names
                                                      like sub_XXXXXXXX follow them; lines
                                                      whose address does not map get a
                                                      "# UNMAPPED" comment
  tu_address_map.py OLD_XEX NEW_XEX --cpp SRC_DIR OUT_H
                                                      the hooks' table for a Title Update
                                                      build (forza-recomp/src/tu4_addresses.h):
                                                      #defines renaming each sub_XXXXXXXX the
                                                      code uses, and Tu4Address() for each
                                                      FORZA_ADDR(0x...); unmapped ones #error
  tu_address_map.py OLD_XEX NEW_XEX --stats
"""
import bisect
import difflib
import os
import pickle
import re
import struct
import sys
import hashlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xex_image  # noqa: E402

MASK16_OPS = {14, 15, 24, 25} | set(range(32, 56))  # addi addis ori oris, D-form loads/stores


def branch_target(addr, w):
    d = w & 0x03FFFFFC
    if d & 0x02000000:
        d -= 0x04000000
    return (addr + d) & 0xFFFFFFFF


def norm_word(w):
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op in MASK16_OPS:
        return w & 0xFFFF0000
    return w


class Image:
    def __init__(self, path):
        self.path = path
        self.base, self.data = xex_image.load_image(path)
        self.sections = xex_image.sections(self.base, self.data)
        pd = next(s for s in self.sections if s[0] == ".pdata")
        raw = self.data[pd[1] - self.base:pd[2] - self.base]
        funcs = {}
        for i in range(len(raw) // 8):
            start, flags = struct.unpack_from(">II", raw, i * 8)
            if start and self.base <= start < self.base + len(self.data):
                funcs[start] = ((flags >> 8) & 0x3FFFFF) * 4
        self.funcs = sorted(funcs.items())
        self.starts = [a for a, _ in self.funcs]

    def word(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def code(self, start, length):
        return struct.unpack_from(f">{length // 4}I", self.data, start - self.base)

    def norm(self, start, length):
        out = [norm_word(w) for w in self.code(start, length)]
        return hashlib.sha1(struct.pack(f">{len(out)}I", *out)).digest()[:8]

    def leaf_length(self, start, limit=64):
        for k in range(limit):
            w = self.word(start + 4 * k)
            if w == 0x4E800020 or (w >> 26 == 18 and not w & 3):
                return 4 * (k + 1)
        return 0

    def norm_text(self):
        """Normalized words of the executable sections: [(start address, bytes)]."""
        out = []
        for name, start, end, exe in self.sections:
            if exe:
                words = self.code(start, (end - start) & ~3)
                out.append((start, struct.pack(f">{len(words)}I", *map(norm_word, words))))
        return out

    def containing(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            start, length = self.funcs[i]
            if addr < start + length:
                return i
        return None

    def is_code(self, addr):
        return any(s[1] <= addr < s[2] and s[3] for s in self.sections)

    def abs_refs(self, start, length):
        """(offset, absolute address) of lis + addi/ori/load/store pairs."""
        hi = {}
        out = []
        for k, w in enumerate(self.code(start, length)):
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:  # lis
                hi[rd] = (imm << 16) & 0xFFFFFFFF
                continue
            if ra in hi and (op == 14 or op in range(32, 56)):
                out.append((k * 4, (hi[ra] + simm) & 0xFFFFFFFF))
                if op == 14 and rd != ra:
                    pass
            elif ra in hi and op == 24:
                out.append((k * 4, hi[ra] | imm))
            if op in (14, 15, 24, 31) or op in range(32, 40):
                hi.pop(rd, None)  # rd overwritten (approximate)
        return out


class AddressMap:
    def __init__(self, old_path, new_path):
        self.old = Image(old_path)
        self.new = Image(new_path)
        a = [self.old.norm(s, n) + n.to_bytes(4, "big") for s, n in self.old.funcs]
        b = [self.new.norm(s, n) + n.to_bytes(4, "big") for s, n in self.new.funcs]
        sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
        self.fmap = {}
        for i, j, n in sm.get_matching_blocks():
            for k in range(n):
                self.fmap[i + k] = j + k
        # Functions between two matched runs with the same count and lengths: changed code.
        blocks = sm.get_matching_blocks()
        for (i1, j1, n1), (i2, j2, _) in zip(blocks, blocks[1:]):
            gi, gj = i1 + n1, j1 + n1
            if i2 - gi == j2 - gj and i2 > gi:
                for k in range(i2 - gi):
                    if self.old.funcs[gi + k][1] == self.new.funcs[gj + k][1]:
                        self.fmap[gi + k] = gj + k
        # The rest of each gap: functions the update changed. Pair each with the most similar
        # function of the same gap (normalized instructions), in order.
        self.fuzzy = {}
        for (i1, j1, n1), (i2, j2, _) in zip(blocks, blocks[1:]):
            gi, gj = i1 + n1, j1 + n1
            olds = [i for i in range(gi, i2) if i not in self.fmap]
            news = [j for j in range(gj, j2) if j not in self.fmap.values()] if olds else []
            if not olds or not news or len(olds) * len(news) > 4000:
                continue
            nw = {j: [norm_word(w) for w in self.new.code(*self.new.funcs[j])] for j in news}
            lo = 0
            for i in olds:
                ow = [norm_word(w) for w in self.old.code(*self.old.funcs[i])]
                best, best_j = 0.6, None
                for j in news:
                    if j < lo:
                        continue
                    sm2 = difflib.SequenceMatcher(None, ow, nw[j], autojunk=False)
                    if sm2.real_quick_ratio() > best and sm2.quick_ratio() > best:
                        r = sm2.ratio()
                        if r > best:
                            best, best_j = r, j
                if best_j is not None:
                    self.fuzzy[i] = best_j
                    lo = best_j + 1
        anchors = {}
        for i, j in self.fmap.items():
            (s1, n1), (s2, n2) = self.old.funcs[i], self.new.funcs[j]
            if n1 != n2:
                continue
            r1 = dict(self.old.abs_refs(s1, n1))
            for off, v2 in self.new.abs_refs(s2, n2):
                v1 = r1.get(off)
                if v1 is not None and not self.old.is_code(v1):
                    anchors.setdefault(v1, set()).add(v2 - v1)
        self.anchors = sorted((k, v.pop()) for k, v in anchors.items() if len(v) == 1)
        self.akeys = [k for k, _ in self.anchors]
        self.new_text = self.new.norm_text()

    def context_map(self, addr):
        """Find the instructions around addr (normalized) once in the new code."""
        sec = next((s for s in self.old.sections if s[3] and s[1] <= addr < s[2]), None)
        if sec is None:
            return None
        for before, after in ((8, 8), (0, 12), (12, 1), (4, 6), (0, 6)):
            lo = max(sec[1], addr - 4 * before)
            hi = min(sec[2] & ~3, addr + 4 * after)
            words = self.old.code(lo, hi - lo)
            pat = struct.pack(f">{len(words)}I", *map(norm_word, words))
            hits = []
            for start, text in self.new_text:
                pos = text.find(pat)
                while pos != -1 and len(hits) < 2:
                    if pos % 4 == 0:
                        hits.append(start + pos + (addr - lo))
                    pos = text.find(pat, pos + 1)
            if len(hits) == 1:
                return hits[0]
            if len(hits) > 1:
                continue
        return None

    def call_vote(self, addr):
        """A function's new address from its callers: each call site in a matched function
        of the same length (so at the same offset) names the new function it calls."""
        if getattr(self, "_callers", None) is None:
            self._callers = {}
            for s, n in self.old.funcs:
                for k, w in enumerate(self.old.code(s, n)):
                    if w >> 26 == 18 and not w & 2:
                        self._callers.setdefault(branch_target(s + 4 * k, w), []).append(s + 4 * k)
        votes = {}
        for site in self._callers.get(addr, ()):
            i = self.old.containing(site)
            j = self.fmap.get(i) if i is not None else None
            if j is None or self.old.funcs[i][1] != self.new.funcs[j][1]:
                continue
            new_site = self.new.funcs[j][0] + site - self.old.funcs[i][0]
            w = self.new.word(new_site)
            if w >> 26 == 18 and not w & 2:
                t = branch_target(new_site, w)
                votes[t] = votes.get(t, 0) + 1
        if not votes:
            return None
        best = max(votes, key=votes.get)
        n, total = votes[best], sum(votes.values())
        return best if n >= 2 and 5 * n >= 4 * total else None

    def map(self, addr):
        """(new address, how) or (None, why)."""
        if self.old.is_code(addr):
            i = self.old.containing(addr)
            j = self.fmap.get(i) if i is not None else None
            if j is not None:
                (s1, n1), (s2, n2) = self.old.funcs[i], self.new.funcs[j]
                if addr == s1 or n1 == n2:
                    return s2 + (addr - s1), "code"
            # A function start the alignment left out: its callers say where it went (a
            # similar function nearby, or the same code at the same offset, can be another).
            new = self.call_vote(addr) if i is None or addr == self.old.funcs[i][0] else None
            if new is not None:
                k = self.new.containing(new)
                n2 = self.new.funcs[k][1] if k is not None and self.new.funcs[k][0] == new else 0
                n1 = self.old.funcs[i][1] if i is not None else 0
                if not n1 or not n2:  # no .pdata entry (a leaf): up to its first blr or b
                    n1, n2 = self.old.leaf_length(addr), self.new.leaf_length(new)
                if n2 and n1 == n2 and self.old.norm(addr, n1) == self.new.norm(new, n2):
                    return new, "code (by its callers)"
                return new, "code (by its callers; the function changed: check it)"
            if i is not None and i in self.fuzzy:
                j = self.fuzzy[i]
                (s1, n1), (s2, n2) = self.old.funcs[i], self.new.funcs[j]
                if addr == s1:
                    return s2, "code (changed function; check it)"
                ow = [norm_word(w) for w in self.old.code(s1, n1)]
                nw = [norm_word(w) for w in self.new.code(s2, n2)]
                k = (addr - s1) // 4
                for a, b, n in difflib.SequenceMatcher(None, ow, nw, autojunk=False).get_matching_blocks():
                    if a <= k < a + n and n >= 4:
                        return s2 + 4 * (b + k - a) + (addr - s1) % 4, "code (in a changed function; check it)"
            new = self.context_map(addr)
            if new is not None:
                return new, "code (by context: the function changed or has no .pdata entry; check it)"
            if i is None:
                return None, "code outside any .pdata function, context not found"
            return None, f"function {self.old.funcs[i][0]:#x} changed or removed"
        k = bisect.bisect_right(self.akeys, addr)
        lo = self.anchors[k - 1] if k else None
        hi = self.anchors[k] if k < len(self.anchors) else None
        if lo and lo[0] == addr:
            return addr + lo[1], "data (referenced)"
        if lo and hi and lo[1] == hi[1]:
            return addr + lo[1], "data (between anchors)"
        if lo and addr - lo[0] < 0x100:
            return addr + lo[1], f"data (near {lo[0]:#x})"
        return None, "data without anchors that agree"


def cached_map(old, new):
    key = hashlib.sha1()
    for p in (old, new, os.path.abspath(__file__)):
        st = os.stat(p)
        key.update(f"{os.path.abspath(p)}:{st.st_size}:{st.st_mtime_ns}".encode())
    cache = os.path.join(os.environ.get("XDG_CACHE_HOME", os.path.expanduser("~/.cache")),
                         "forza-tu-map-" + key.hexdigest()[:16] + ".pickle")
    try:
        with open(cache, "rb") as f:
            return pickle.load(f)
    except (OSError, pickle.PickleError, EOFError, AttributeError):
        pass
    m = AddressMap(old, new)
    try:
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        with open(cache, "wb") as f:
            pickle.dump(m, f)
    except OSError:
        pass
    return m


ADDR_RE = re.compile(r"(?<![0-9A-Za-z_])(0x|sub_|loc_|_)(8[0-9A-Fa-f]{7})(?![0-9A-Fa-f])")


def rewrite(m, text):
    out = []
    unmapped = 0
    for line in text.splitlines(keepends=True):
        bad = []

        def sub(mo):
            prefix, hexs = mo.group(1), mo.group(2)
            addr = int(hexs, 16)
            if not (m.old.base <= addr < m.old.base + len(m.old.data)):
                return mo.group(0)
            new, why = m.map(addr)
            if new is None:
                bad.append(f"{addr:#x}: {why}")
                return mo.group(0)
            return prefix + (f"{new:08X}" if hexs.isupper() or prefix != "0x" else f"{new:08x}")

        new_line = ADDR_RE.sub(sub, line)
        if bad:
            unmapped += 1
            nl = "\n" if new_line.endswith("\n") else ""
            new_line = new_line.rstrip("\n") + "  # UNMAPPED " + "; ".join(bad) + nl
        out.append(new_line)
    return "".join(out), unmapped


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def cpp_header(m, src_dir, out_h):
    names, addrs = set(), set()
    for root, _, files in os.walk(src_dir):
        for f in files:
            if f.endswith((".cpp", ".h")) and f != os.path.basename(out_h):
                code = strip_comments(open(os.path.join(root, f), encoding="utf-8").read())
                names |= {int(x, 16) for x in re.findall(r"sub_(8[0-9A-F]{7})\b", code)}
                addrs |= {int(x, 16) for x in re.findall(r"FORZA_ADDR\((0x8[0-9A-Fa-f]{7})\)", code)}
    lines = ["// Generated by tools/tu_address_map.py --cpp: the disc executable's guest addresses",
             "// used by the hooks, mapped to Title Update 4's (src/platform/guest_addr.h).",
             "// Entries marked \"check\" were found by the code around them: the update changed",
             "// that function, so the hook's assumptions need a look.",
             "#pragma once", "", "#include <cstdint>", ""]
    bad = 0
    targets = {m.map(a)[0] for a in names}
    for a in sorted(names):
        if a in targets:  # #define chains would rename twice
            lines.append(f'#error "sub_{a:08X} is also the new name of another function"')
            bad += 1
        new, why = m.map(a)
        if new is None:
            lines.append(f'#error "sub_{a:08X} has no Title Update 4 equivalent ({why})"')
            bad += 1
            continue
        note = "  // check" if "check" in why else ""
        lines.append(f"#define sub_{a:08X} sub_{new:08X}{note}")
        lines.append(f"#define __imp__sub_{a:08X} __imp__sub_{new:08X}")
    lines += ["", "namespace forza {", "constexpr uint32_t Tu4Address(uint32_t a) {", "  switch (a) {"]
    for a in sorted(addrs):
        new, why = m.map(a)
        if new is None:
            lines.append(f"    // {a:#010x}: no Title Update 4 equivalent ({why})")
            bad += 1
            continue
        note = "  // check" if "check" in why else ""
        lines.append(f"    case 0x{a:08X}: return 0x{new:08X};  // {why.split(' (')[0]}{note}")
    lines += ["    default: return 0;  // not in the table: rerun tools/tu_address_map.py --cpp",
              "  }", "}", "}  // namespace forza", ""]
    with open(out_h, "w") as f:
        f.write("\n".join(lines))
    print(f"{out_h}: {len(names)} functions, {len(addrs)} addresses, {bad} unmapped")
    return 1 if bad else 0


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    m = cached_map(argv[1], argv[2])
    if argv[3] == "--stats":
        same = sum(1 for i, j in m.fmap.items() if m.old.funcs[i][1] == m.new.funcs[j][1])
        print(f"old {len(m.old.funcs)} functions, new {len(m.new.funcs)}; matched {len(m.fmap)} "
              f"({same} with the same length); data anchors {len(m.anchors)}")
        return 0
    if argv[3] == "--cpp":
        return cpp_header(m, argv[4], argv[5])
    if argv[3] == "--hints":
        text = open(argv[4]).read()
        new, unmapped = rewrite(m, text)
        with open(argv[5], "w") as f:
            f.write(new)
        print(f"{argv[5]}: {unmapped} line(s) with unmapped addresses")
        return 0
    for a in argv[3:]:
        addr = int(a.replace("sub_", "0x"), 16)
        new, why = m.map(addr)
        print(f"{addr:#010x} -> {new:#010x} ({why})" if new is not None else f"{addr:#010x} -> ? ({why})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
