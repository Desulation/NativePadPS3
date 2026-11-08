#!/usr/bin/env python3
"""lv2scan - offline candidate and pattern finder for NativePadPS3.

Input: raw big-endian LV2 window dumps (8 MiB, address 0x8000000000000000
upward) that you took from your own consoles with the debug-build dumper
(DUMP_LV2 = 1). Output: paste-ready C for `npp_kernel_table` (src/kernel.c)
and `npp_scan_pattern`.

Dumps are never committed (`*.bin` and `tools/dumps/` are git-ignored).

The rules below mirror src/kernel.c (`kernel_scan`, `find_prologue`,
`resolve_tocs`) so a record or pattern found here behaves identically in the
plugin:

  fp_addr    = address of the `ld rT, ds(r2)` instruction, rounded down to 8
  fp_hash    = FNV-1a 64 of the 32 bytes at fp_addr
  prologue   = nearest `stdu r1, ds(r1)` at or before the pattern start,
               at most 0x800 bytes back
  OPD        = first 8-aligned qword pair { func_start, toc } with toc inside
               the 8 MiB window
  slot_addr  = toc + (int16)(ld_word & 0xFFFC)
  report_off = immediate of the report-use instruction (never negative)
"""
import argparse
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

try:                                    # optional: only fills Candidate.disasm
    import capstone as _capstone
    _CS = _capstone.Cs(_capstone.CS_ARCH_PPC,
                       _capstone.CS_MODE_64 | _capstone.CS_MODE_BIG_ENDIAN)
except Exception:                       # not installed, or no PPC support
    _CS = None

# ---- constants shared with src/kernel.h and src/lv2.h --------------------

DEFAULT_BASE = 0x8000000000000000
WINDOW_SIZE = 8 * 1024 * 1024           # NPP_LV2_BOUND - NPP_LV2_BASE
NPP_FP_LEN = 32
NPP_SCAN_MAX_INSNS = 16
NPP_SCAN_MAX_HITS = 8
NPP_SCAN_PROLOGUE_RANGE = 0x800
NPP_REPORT_OFF_MAX = 0x7FF0     # kernel.h: largest usable report offset
NPP_INSN_LD_FROM_R2 = (0xFC1F0003, 0xE8020000)   # ld any, ds(r2)
NPP_INSN_STDU_R1 = (0xFFFF0003, 0xF8210001)      # stdu r1, ds(r1)

FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
M64 = 0xFFFFFFFFFFFFFFFF

def record_usable(slot_addr: int, report_off: int, base: int = DEFAULT_BASE) -> bool:
    """kernel_record_usable in src/kernel.c: slot 8-aligned inside
    [base, base + 8 MiB - 8], report_off 8-aligned and <= NPP_REPORT_OFF_MAX.
    A record failing this is refused by the plugin whatever its source."""
    return ((slot_addr & 7) == 0
            and base <= slot_addr <= base + WINDOW_SIZE - 8
            and (report_off & 7) == 0
            and 0 <= report_off <= NPP_REPORT_OFF_MAX)


# ---- search rules (host side only) ---------------------------------------

REQUIRED_COMPARES = (0x054C, 0x0268, 0x00A1)   # VID, PID, report id
REPORT_OFF_MIN, REPORT_OFF_MAX = 0x10, 0x400
MAX_FUNC_BYTES = 0x2000          # a function body is never searched further
REPORT_LOOKAHEAD_WORDS = 8       # the report use must fit the pattern window
WINDOW_BEFORE = WINDOW_AFTER = 8  # pattern window around the ld, in words


# ---- small helpers -------------------------------------------------------

def fnv1a64(data: bytes) -> int:
    h = FNV64_OFFSET
    for b in data:
        h = ((h ^ b) * FNV64_PRIME) & M64
    return h


def _simm16(v: int) -> int:
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def _words(img: bytes) -> Tuple[int, ...]:
    n = len(img) // 4
    return struct.unpack(">%dI" % n, img[:n * 4])


def _is_stdu(w: int) -> bool:
    return (w & NPP_INSN_STDU_R1[0]) == NPP_INSN_STDU_R1[1]


def _is_ld_r2(w: int) -> bool:
    return (w & NPP_INSN_LD_FROM_R2[0]) == NPP_INSN_LD_FROM_R2[1]


def _in_window(addr: int, base: int) -> bool:
    return base <= addr < base + WINDOW_SIZE


# ---- D-form decoding for the report-use rule ------------------------------

_D_FORM_OPS = frozenset([14] + list(range(32, 46)) + list(range(48, 56)))
_DS_FORM_OPS = frozenset([58, 62])      # ld/ldu/lwa, std/stdu


def _d_form_imm(w: int, ra: int) -> Optional[int]:
    """Immediate of a D/DS-form instruction whose rA field is `ra`, else None.
    rA == 0 means "no base" in these encodings and is never accepted. A DS-form
    word is accepted only with the low two bits clear (ld, std): the C scan
    reads the immediate as simm16(word & 0xFFFF), so lwa/ldu/stdu would read
    differently there."""
    if ra == 0 or ((w >> 16) & 31) != ra:
        return None
    op = w >> 26
    if op in _D_FORM_OPS:
        return _simm16(w)
    if op in _DS_FORM_OPS and not w & 3:
        return _simm16(w)
    return None


def _first_report_use(words, j: int, end: int) -> Optional[Tuple[int, int]]:
    """For the ld at word index j: (index, offset) of the first later D-form
    instruction with rA == rT and an immediate in [0x10, 0x400] that the
    plugin can use (8-aligned, <= NPP_REPORT_OFF_MAX), looking at most
    REPORT_LOOKAHEAD_WORDS ahead and not past `end`. Uses that fail are
    skipped and the search goes on."""
    rt = (words[j] >> 21) & 31
    for k in range(j + 1, min(end, j + 1 + REPORT_LOOKAHEAD_WORDS)):
        imm = _d_form_imm(words[k], rt)
        if (imm is not None and REPORT_OFF_MIN <= imm <= REPORT_OFF_MAX
                and imm & 7 == 0 and imm <= NPP_REPORT_OFF_MAX):
            return k, imm
    return None


# ---- find_toc / find_candidates -------------------------------------------

def find_toc(img: bytes, base: int, func_start: int) -> Optional[int]:
    """TOC of the function at func_start: the second qword of the first
    8-aligned pair { func_start, toc } with toc inside the window (the same
    rule as resolve_tocs in src/kernel.c)."""
    needle = func_start.to_bytes(8, "big")
    pos = 0
    while True:
        i = img.find(needle, pos)
        if i < 0:
            return None
        pos = i + 1
        if (base + i) & 7 or i + 16 > len(img):
            continue
        toc = int.from_bytes(img[i + 8:i + 16], "big")
        if _in_window(toc, base):
            return toc


@dataclass
class Candidate:
    fp_addr: int
    fp_hash: int
    slot_addr: int
    report_off: int
    func_start: int
    disasm: List[str] = field(default_factory=list)
    # where the evidence lives; common_pattern aligns dumps on these
    ld_addr: int = 0
    report_insn_addr: int = 0


def _disasm(img: bytes, base: int, lo: int, hi: int) -> List[str]:
    """Lines for the words in [lo, hi) (absolute addresses). Empty without
    capstone."""
    if _CS is None:
        return []
    out = []
    for a in range(max(lo, base), min(hi, base + len(img) // 4 * 4), 4):
        raw = img[a - base:a - base + 4]
        ins = next(_CS.disasm(raw, a), None)
        if ins is None:
            text = ".long 0x%08X" % int.from_bytes(raw, "big")
        else:
            text = ("%s %s" % (ins.mnemonic, ins.op_str)).strip()
        out.append("0x%016X: %s" % (a, text))
    return out


def find_candidates(img: bytes, base: int = DEFAULT_BASE) -> List[Candidate]:
    """Every `ld rT, ds(r2)` in a function that compares against 0x054C,
    0x0268 and 0xA1 and soon after uses rT as the base of an access at a
    plausible report offset. Functions are delimited by `stdu r1`."""
    words = _words(img)
    n = len(words)
    starts = [i for i, w in enumerate(words) if _is_stdu(w)]
    max_words = MAX_FUNC_BYTES // 4
    out: List[Candidate] = []

    for si, s in enumerate(starts):
        end = min(starts[si + 1] if si + 1 < len(starts) else n, s + max_words)

        compares = set()
        lds = []
        for j in range(s + 1, end):
            w = words[j]
            if (w >> 26) in (10, 11):               # cmpli / cmpi
                compares.add(w & 0xFFFF)
            elif _is_ld_r2(w):
                lds.append(j)
        if not lds or not all(c in compares for c in REQUIRED_COMPARES):
            continue

        func_start = base + 4 * s
        toc = None
        for j in lds:
            if (j - s) * 4 > NPP_SCAN_PROLOGUE_RANGE:
                continue
            use = _first_report_use(words, j, end)
            if use is None:
                continue
            if toc is None:
                toc = find_toc(img, base, func_start)
                if toc is None:
                    break
            slot = (toc + _simm16(words[j] & 0xFFFC)) & M64
            if not record_usable(slot, use[1], base):
                continue
            ld_addr = base + 4 * j
            fp = ld_addr & ~7
            fo = fp - base
            if fo < 0 or fo + NPP_FP_LEN > len(img):
                continue
            k, off = use
            out.append(Candidate(
                fp_addr=fp,
                fp_hash=fnv1a64(img[fo:fo + NPP_FP_LEN]),
                slot_addr=slot,
                report_off=off,
                func_start=func_start,
                disasm=_disasm(img, base, ld_addr - 4 * WINDOW_BEFORE,
                               ld_addr + 4 * (WINDOW_AFTER + 1)),
                ld_addr=ld_addr,
                report_insn_addr=base + 4 * k,
            ))
    return out


# ---- common_pattern -------------------------------------------------------

class Pattern(list):
    """list of (mask, value) that maps one to one onto the C
    npp_insn_match_t array, plus the two indices of npp_scan_pattern_t."""

    def __init__(self, insns=(), toc_ld_index: int = 0, report_off_index: int = 0):
        super().__init__(insns)
        self.toc_ld_index = toc_ld_index
        self.report_off_index = report_off_index


def _word_at(img: bytes, base: int, addr: int) -> Optional[int]:
    o = addr - base
    if o < 0 or o + 4 > len(img):
        return None
    return int.from_bytes(img[o:o + 4], "big")


def _relocatable_mask(w: int) -> int:
    """Bits of w that are code shape rather than per-kernel addresses, as an
    upper bound on the mask: branch targets and r2-relative (TOC)
    displacements are dropped."""
    op = w >> 26
    if op == 18:                                   # b / bl / ba / bla
        return 0xFC000003
    if op == 16:                                   # bc: keep BO, BI, AA, LK
        return 0xFFFF0003
    ra = (w >> 16) & 31
    if ra == 2:
        if op == 14 or 32 <= op <= 55:             # D-form off the TOC
            return 0xFFFF0000
        if op in (58, 62):                         # DS-form off the TOC
            return 0xFFFF0003
    return 0xFFFFFFFF


def _imm_mask(w: int) -> int:
    """Mask covering the immediate field of a D/DS-form word."""
    return 0xFFFC if (w >> 26) in _DS_FORM_OPS else 0xFFFF


def _popcount(x: int) -> int:
    return bin(x).count("1")


def common_pattern(per_dump, base: int = DEFAULT_BASE) -> Pattern:
    """Mask pattern common to several dumps.

    per_dump: [(image, Candidate), ...]. The window is WINDOW_BEFORE words
    before to WINDOW_AFTER words after the candidate's `ld` (17 words); each
    word's mask is the set of bits equal in every dump, further cleared for
    per-kernel address bits (branch targets, r2-relative displacements), and
    for the TOC `ld` always clears its displacement. The report-use word's
    immediate is cleared when the dumps disagree on it.

    The window is aligned on the `ld` instruction itself, not on fp_addr, so
    a kernel that puts the ld in the other half of a qword still lines up.

    The result is at most NPP_SCAN_MAX_INSNS long: words with an empty mask
    are dropped from both ends first; if it is still too long, the end word
    carrying fewer fixed bits goes (a tie drops the earlier word) until it
    fits. Neither index word is ever dropped. Words before any dump's
    function prologue (its `stdu r1`) are excluded first, because the C scan
    finds the prologue from the first pattern word backwards. ValueError if they cannot both
    fit, or the dumps disagree on the distance between them.
    """
    if not per_dump:
        raise ValueError("common_pattern needs at least one dump")

    rel_report = None
    anchors = []
    clip = -WINDOW_BEFORE          # first usable word, relative to the ld
    for img, c in per_dump:
        ld_addr = c.ld_addr or _derive_ld_addr(img, base, c)
        rep_addr = c.report_insn_addr or _derive_report_addr(img, base, ld_addr)
        rel = (rep_addr - ld_addr) // 4
        if rel_report is None:
            rel_report = rel
        elif rel != rel_report:
            raise ValueError("dumps disagree on the distance from the TOC ld "
                             "to the report use (%d vs %d words)" % (rel_report, rel))
        anchors.append(ld_addr)
        if base <= c.func_start < ld_addr:
            # the C scan looks for the prologue from insns[0] backwards, so
            # insns[0] may not lie before this dump's own stdu
            clip = max(clip, (c.func_start - ld_addr) // 4)

    span = WINDOW_BEFORE + 1 + WINDOW_AFTER
    toc_i = WINDOW_BEFORE
    rep_i = WINDOW_BEFORE + rel_report
    if not 0 <= rep_i < span:
        raise ValueError("report use is outside the pattern window")

    masks, values = [], []
    for i in range(span):
        ws = [_word_at(img, base, a + 4 * (i - WINDOW_BEFORE))
              for (img, _), a in zip(per_dump, anchors)]
        if i - WINDOW_BEFORE < clip or any(w is None for w in ws):
            masks.append(0)
            values.append(0)
            continue
        m = 0xFFFFFFFF
        for w in ws[1:]:
            m &= ~(ws[0] ^ w) & 0xFFFFFFFF
        for w in ws:
            m &= _relocatable_mask(w)
        if i == toc_i:
            m &= ~0xFFFC & 0xFFFFFFFF
        if i == rep_i and any(w != ws[0] for w in ws[1:]):
            m &= ~_imm_mask(ws[0]) & 0xFFFFFFFF
        masks.append(m)
        values.append(ws[0] & m)

    keep_lo, keep_hi = min(toc_i, rep_i), max(toc_i, rep_i)
    if keep_hi - keep_lo + 1 > NPP_SCAN_MAX_INSNS:
        raise ValueError("TOC ld and report use are %d words apart; the C scan "
                         "allows %d" % (keep_hi - keep_lo + 1, NPP_SCAN_MAX_INSNS))
    lo, hi = 0, span - 1
    while lo < keep_lo and masks[lo] == 0:
        lo += 1
    while hi > keep_hi and masks[hi] == 0:
        hi -= 1
    while hi - lo + 1 > NPP_SCAN_MAX_INSNS:
        if lo >= keep_lo:
            hi -= 1
        elif hi <= keep_hi:
            lo += 1
        elif _popcount(masks[lo]) <= _popcount(masks[hi]):
            lo += 1
        else:
            hi -= 1

    return Pattern([(masks[i], values[i]) for i in range(lo, hi + 1)],
                   toc_ld_index=toc_i - lo, report_off_index=rep_i - lo)


def _derive_ld_addr(img: bytes, base: int, c: Candidate) -> int:
    hits = [a for a in (c.fp_addr, c.fp_addr + 4)
            if (w := _word_at(img, base, a)) is not None and _is_ld_r2(w)]
    if len(hits) != 1:
        raise ValueError("cannot tell which word at 0x%X is the TOC ld" % c.fp_addr)
    return hits[0]


def _derive_report_addr(img: bytes, base: int, ld_addr: int) -> int:
    words = _words(img)
    j = (ld_addr - base) // 4
    use = _first_report_use(words, j, len(words))
    if use is None:
        raise ValueError("no report use after the TOC ld at 0x%X" % ld_addr)
    return base + 4 * use[0]


# ---- simulate_scan: the Python mirror of kernel_scan ------------------------

@dataclass
class ScanResult:
    raw_hits: int = 0                  # pattern hits with a non-negative offset
    fp_addr: Optional[int] = None
    fp_hash: Optional[int] = None
    slot_addr: Optional[int] = None
    report_off: Optional[int] = None
    error: Optional[str] = None


def _pattern_valid(pat) -> bool:
    n = len(pat)
    return (0 < n <= NPP_SCAN_MAX_INSNS
            and 0 <= pat.toc_ld_index < n and 0 <= pat.report_off_index < n)


def simulate_scan(img: bytes, base: int, pat: Pattern) -> ScanResult:
    """What src/kernel.c kernel_scan would do with `pat` on this image."""
    if not _pattern_valid(pat):
        return ScanResult(error="invalid pattern")
    words = _words(img)
    n, count = len(words), len(pat)
    last = n - count
    if last < 0:
        return ScanResult(error="image shorter than the pattern")

    # pass 1: candidate positions through the most selective word
    best = max(range(count), key=lambda i: _popcount(pat[i][0]))
    bm, bv = pat[best]
    positions = [j - best for j, w in enumerate(words)
                 if w & bm == bv and 0 <= j - best <= last]

    hits = []                          # (word index of insns[0], disp, off)
    for p in positions:
        if any(words[p + i] & m != v for i, (m, v) in enumerate(pat)):
            continue
        off = _simm16(words[p + pat.report_off_index])
        if off < 0:
            continue                   # dropped, not counted
        disp = _simm16(words[p + pat.toc_ld_index] & 0xFFFC)
        hits.append((p, disp, off))

    res = ScanResult(raw_hits=len(hits))
    if not hits:
        res.error = "no hits"
        return res
    if len(hits) > NPP_SCAN_MAX_HITS:
        res.error = "ambiguous: %d hits, limit %d" % (len(hits), NPP_SCAN_MAX_HITS)
        return res

    found = None
    for p, disp, off in hits:
        start = _find_prologue(words, p)
        if start is None:
            continue
        toc = find_toc(img, base, base + 4 * start)
        if toc is None:
            continue
        slot = (toc + disp) & M64
        if slot & 7 or not (base <= slot <= base + WINDOW_SIZE - 8):
            continue
        fp = (base + 4 * (p + pat.toc_ld_index)) & ~7
        if fp > base + WINDOW_SIZE - NPP_FP_LEN:
            continue
        fo = fp - base
        if fo + NPP_FP_LEN > len(img):
            continue
        if found is not None:
            if (slot, off) != (found[2], found[3]):
                res.error = "hits disagree on (slot, report_off)"
                return res
            continue
        found = (fp, fnv1a64(img[fo:fo + NPP_FP_LEN]), slot, off)
    if found is None:
        res.error = "no hit resolves (prologue, descriptor, slot or fingerprint)"
        return res
    if not record_usable(found[2], found[3], base):
        # kernel_resolve_from refuses a scan result kernel_record_usable rejects
        res.error = ("record unusable: slot 0x%X report_off 0x%X (needs 8-aligned, "
                     "report_off <= 0x%X)" % (found[2], found[3], NPP_REPORT_OFF_MAX))
        return res
    res.fp_addr, res.fp_hash, res.slot_addr, res.report_off = found
    return res


def _find_prologue(words, p: int) -> Optional[int]:
    """Word index of the nearest stdu r1 at or before word p, at most
    NPP_SCAN_PROLOGUE_RANGE bytes back and never before the image."""
    lo = p - NPP_SCAN_PROLOGUE_RANGE // 4
    if lo < 0:
        lo = 0
    a = p
    while True:
        if _is_stdu(words[a]):
            return a
        if a <= lo:
            return None
        a -= 1


# ---- C emitters --------------------------------------------------------------

def _c_string(s: str) -> str:
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= ord(ch) < 0x7F:
            out.append(ch)
        else:
            raise ValueError("name has a character that is not printable ASCII")
    return '"%s"' % "".join(out)


def emit_c_entry(name: str, c: Candidate) -> str:
    """One npp_kernel_t initializer, in the style of the entries in the
    npp_kernel_table in src/kernel.c."""
    return "\t{ %s, 0x%016XULL, 0x%016XULL, 0x%016XULL, 0x%X }," % (
        _c_string(name), c.fp_addr, c.fp_hash, c.slot_addr, c.report_off)


def emit_c_pattern(pattern, toc_ld_index: Optional[int] = None,
                   report_off_index: Optional[int] = None,
                   insns_name: str = "npp_scan_insns") -> str:
    """C for the scan pattern: an array of npp_insn_match_t and the
    npp_scan_pattern definition that replaces the empty one in kernel.c."""
    if toc_ld_index is None:
        toc_ld_index = pattern.toc_ld_index
    if report_off_index is None:
        report_off_index = pattern.report_off_index
    n = len(pattern)
    if not 0 < n <= NPP_SCAN_MAX_INSNS:
        raise ValueError("pattern length %d, the C scan needs 1..%d"
                         % (n, NPP_SCAN_MAX_INSNS))
    if not 0 <= toc_ld_index < n:
        raise ValueError("toc_ld_index %d outside the pattern" % toc_ld_index)
    if not 0 <= report_off_index < n:
        raise ValueError("report_off_index %d outside the pattern" % report_off_index)

    lines = ["static const npp_insn_match_t %s[] = {" % insns_name]
    for i, (m, v) in enumerate(pattern):
        note = ""
        if i == toc_ld_index:
            note = "   /* ld rT, ds(r2): TOC slot */"
        elif i == report_off_index:
            note = "   /* report offset */"
        lines.append("\t{ 0x%08Xu, 0x%08Xu },%s" % (m, v, note))
    lines.append("};")
    lines.append("const npp_scan_pattern_t npp_scan_pattern = "
                 "{ %s, %d, %d, %d };" % (insns_name, n, toc_ld_index, report_off_index))
    return "\n".join(lines)


# ---- CLI -----------------------------------------------------------------------

def _hex(x):
    return "none" if x is None else "0x%X" % x


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Find NativePadPS3 kernel records and a common scan pattern in "
                    "LV2 dumps. Output is C, with diagnostics as // comments.")
    ap.add_argument("dumps", nargs="+", metavar="DUMP")
    ap.add_argument("--name", action="append", default=[],
                    help="table name for each DUMP, in order "
                         "(default: the file name without extension)")
    ap.add_argument("--pick", action="append", type=int, default=[],
                    help="which candidate to use, for each DUMP in order, "
                         "when a dump has several (0-based)")
    args = ap.parse_args(argv)

    if args.name and len(args.name) != len(args.dumps):
        print("lv2scan: %d --name for %d dumps" % (len(args.name), len(args.dumps)),
              file=sys.stderr)
        return 2
    if args.pick and len(args.pick) != len(args.dumps):
        print("lv2scan: %d --pick for %d dumps" % (len(args.pick), len(args.dumps)),
              file=sys.stderr)
        return 2
    names = args.name or [os.path.splitext(os.path.basename(p))[0]
                          for p in args.dumps]

    images, chosen = [], []
    status = 0
    base = DEFAULT_BASE            # the plugin always scans from NPP_LV2_BASE
    for k, path in enumerate(args.dumps):
        try:
            with open(path, "rb") as f:
                img = f.read()
        except OSError as e:
            print("lv2scan: cannot read %s: %s" % (path, e.strerror or e),
                  file=sys.stderr)
            return 2
        if len(img) != WINDOW_SIZE:
            print("lv2scan: %s is %d bytes, a dump is exactly %d (the 8 MiB LV2 "
                  "window)" % (path, len(img), WINDOW_SIZE), file=sys.stderr)
            return 2
        images.append(img)
        cands = find_candidates(img, base)
        print("// %s: %d candidate(s)" % (names[k], len(cands)))
        for i, c in enumerate(cands):
            print("//   [%d] func 0x%X  ld 0x%X  slot 0x%X  report_off 0x%X"
                  % (i, c.func_start, c.ld_addr, c.slot_addr, c.report_off))
            for line in c.disasm:
                print("//       " + line)
        if not cands:
            print("lv2scan: no candidate in %s" % path, file=sys.stderr)
            status = max(status, 1)
            chosen.append(None)
            continue
        pick = args.pick[k] if args.pick else (0 if len(cands) == 1 else None)
        if pick is None:
            print("lv2scan: %s has %d candidates; choose one with --pick "
                  "(one value per dump)" % (path, len(cands)), file=sys.stderr)
            status = max(status, 2)
            chosen.append(None)
        elif not 0 <= pick < len(cands):
            print("lv2scan: --pick %d out of range for %s" % (pick, path),
                  file=sys.stderr)
            status = max(status, 2)
            chosen.append(None)
        else:
            chosen.append(cands[pick])
    if status:
        return status

    print("\n// npp_kernel_table entries")
    for name, c in zip(names, chosen):
        print(emit_c_entry(name, c))

    try:
        pat = common_pattern(list(zip(images, chosen)), base)
        text = emit_c_pattern(pat)
    except ValueError as e:
        print("lv2scan: %s" % e, file=sys.stderr)
        return 1

    # the verdict comes before the pattern, and a pattern that does not
    # resolve every dump to its table entry is not printed at all
    print("\n// the pattern against each dump (the C scan gives up above "
          "%d raw hits)" % NPP_SCAN_MAX_HITS)
    for name, img, c in zip(names, images, chosen):
        r = simulate_scan(img, base, pat)
        line = "// %s: raw hits: %d" % (name, r.raw_hits)
        if r.error:
            line += ", scan FAILS: %s" % r.error
            status = 1
        else:
            agrees = ((r.fp_addr, r.fp_hash, r.slot_addr, r.report_off) ==
                      (c.fp_addr, c.fp_hash, c.slot_addr, c.report_off))
            line += ", resolves slot %s report_off %s%s" % (
                _hex(r.slot_addr), _hex(r.report_off),
                "" if agrees else "  MISMATCH with the table entry")
            if not agrees:
                status = 1
        print(line)
    if status:
        print("lv2scan: the pattern does not resolve every dump; not printed",
              file=sys.stderr)
        return status

    print("\n// scan pattern, common to %d dump(s)" % len(images))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
