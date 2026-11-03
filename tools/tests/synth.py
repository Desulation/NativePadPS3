"""Synthetic LV2-like images for the lv2scan tests.

Everything here is hand-encoded PowerPC words and invented addresses: no
kernel bytes, no dumps. The images are tiny (64 KiB by default); the LV2
window (8 MiB) only matters for address range checks, not for image size.
"""
from dataclasses import dataclass

BASE = 0x8000000000000000
WINDOW = 8 * 1024 * 1024

NOP = 0x60000000
BLR = 0x4E800020
MFLR_R0 = 0x7C0802A6
MR_R31_R3 = 0x7C7F1B78


def stdu_r1(ds):
    return 0xF8210001 | (ds & 0xFFFC)


def ld(rt, ds, ra):
    return 0xE8000000 | (rt << 21) | (ra << 16) | (ds & 0xFFFC)


def std(rs, ds, ra):
    return 0xF8000000 | (rs << 21) | (ra << 16) | (ds & 0xFFFC)


def lbz(rt, d, ra):
    return 0x88000000 | (rt << 21) | (ra << 16) | (d & 0xFFFF)


def lwz(rt, d, ra):
    return 0x80000000 | (rt << 21) | (ra << 16) | (d & 0xFFFF)


def addi(rt, ra, imm):
    return 0x38000000 | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def cmplwi(ra, imm):
    return 0x28000000 | (ra << 16) | (imm & 0xFFFF)


def cmpwi(ra, imm):
    return 0x2C000000 | (ra << 16) | (imm & 0xFFFF)


def bne(rel):
    return 0x40820000 | (rel & 0xFFFC)


def bl(rel):
    return 0x48000001 | (rel & 0x03FFFFFC)


@dataclass
class Synth:
    data: bytes
    func_start: int
    ld_addr: int
    report_addr: int
    toc: int
    slot: int
    ld_disp: int
    report_off: int


def func_words(ld_disp, report_off, *, vid=0x054C, pid=0x0268, rep=0xA1,
               pad=0, bl_rel=0x2000, extra_after=()):
    """One function: prologue, three compares, a TOC load, a deref, a use.

    Returns (words, ld_index, report_index). `pad` nops go right before the
    `ld`, moving it between the two halves of a qword.
    """
    w = [
        stdu_r1(-0x80),           # 0: prologue
        MFLR_R0,                  # 1
        std(0, 0x90, 1),          # 2
        MR_R31_R3,                # 3
    ]
    if vid is not None:
        w.append(cmplwi(3, vid))
        w.append(bne(0x40))
    if pid is not None:
        w.append(cmpwi(4, pid))
        w.append(bne(0x3C))
    if rep is not None:
        w.append(cmplwi(5, rep))
        w.append(bne(0x38))
    w += [addi(3, 1, 0x70), bl(bl_rel), addi(4, 0, 0)]
    w += [NOP] * pad
    ld_index = len(w)
    w.append(ld(9, ld_disp, 2))   # the TOC slot load
    w.append(ld(9, 0, 9))         # deref: offset 0, outside [0x10, 0x400]
    report_index = len(w)
    w.append(lbz(0, report_off, 9))
    w += [NOP, BLR]
    w += list(extra_after)
    return w, ld_index, report_index


def build_image(*, ld_disp=0x1230, report_off=0xE8, func_off=0x1000,
                toc_off=0x8000, size=0x10000, opd_off=0x400,
                toc_in_window=True, with_opd=True, **func_kw):
    """An image with one function and (optionally) its function descriptor."""
    img = bytearray(size)
    words, ld_i, rep_i = func_words(ld_disp, report_off, **func_kw)
    for i, word in enumerate(words):
        img[func_off + 4 * i:func_off + 4 * i + 4] = word.to_bytes(4, "big")
    func_start = BASE + func_off
    toc = BASE + toc_off if toc_in_window else BASE + WINDOW + 0x1000
    if with_opd:
        img[opd_off:opd_off + 8] = func_start.to_bytes(8, "big")
        img[opd_off + 8:opd_off + 16] = toc.to_bytes(8, "big")
    return Synth(
        data=bytes(img),
        func_start=func_start,
        ld_addr=func_start + 4 * ld_i,
        report_addr=func_start + 4 * rep_i,
        toc=toc,
        slot=toc + ld_disp,
        ld_disp=ld_disp,
        report_off=report_off,
    )
