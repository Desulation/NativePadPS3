"""Emulation test for the two assembly stubs of the kernel shim blob, shim_poweroff
and shim_teardown (shim/entry.c).

The stubs are the one part of the power-off hooks that cannot be compiled and run on
the host: they are hand-written assembly that adopts the blob's TOC, calls a C body,
and then tail-calls the kernel's original routine with every argument register the
original reads exactly as the kernel passed it (or, for shim_poweroff when the filter
says so, with r5..r7 replaced by the rewritten report id, buffer and length). This
test decodes the stubs from the REAL packed blob (the same bytes shim_pack.py exports,
with the relocations applied at a kernel-like load base) and runs them in a tiny
interpreter that knows exactly the instruction subset the stubs use; any other
instruction fails the test, so a changed stub cannot slip through unchecked.

The C bodies are modelled by a callback at each `bl`: it asserts the ABI state at the
call (r2 = the blob's TOC, r1 16-aligned, back chain intact, arguments as expected),
then does everything a compiled callee may legally do - overwrite the stub frame's
header and parameter area and the whole stack below r1, and clobber every volatile
register, LR and CTR.

At the final `bctr` the test checks that r1, r2 (the kernel's TOC), LR, r3, r4, r8 and
r9 equal their entry values; that r5..r7 are the originals (filter result 0) or the
rewritten o_rid / o_buf / o_len (result 1); that CTR is the original routine's
address from the blob's data block; that nothing at or above the entry r1 was
written; and that no non-volatile register (r13..r31) was touched. For shim_teardown
(entered by `bl` from two call sites and by a plain tail `b` from a third, where LR
is the outer caller's) r3..r10 and LR must be unchanged as well.

Needs capstone (see tools/requirements.txt) and shim/shim.elf (src/build.ps1
-Variant vsh); each missing piece skips the tests with a message.

Run from the repository root:   python -m unittest tools.test_shim_stubs
"""

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shim_pack  # noqa: E402  (deliberately from tools/, never a build dir)

try:
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_64, CS_MODE_BIG_ENDIAN
    _HAVE_CAPSTONE = True
except ImportError:   # pragma: no cover - depends on the machine
    _HAVE_CAPSTONE = False

_REAL_ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "shim", "shim.elf")

M64 = (1 << 64) - 1
BASE = 0x8000000000A00000            # where the blob is "loaded"; any 8-aligned kernel-like value
KERNEL_TOC = 0x80000000003759C0      # the caller's r2 (restored by the stubs before the tail call)
STACK = 0x8000000010F0F000           # the entry r1 (16-aligned)
# Offsets of orig_poweroff / orig_teardown in npp_shim_data_t; pinned against the
# real struct by the offsetof asserts in shim/entry.c.
OFF_ORIG_POWEROFF, OFF_ORIG_TEARDOWN = 984, 992
ORIG_POWEROFF = 0x8000000000102750   # kernel sender, as the plugin would store it
ORIG_TEARDOWN = 0x80000000001174FC
PO_CALL = 0x70                       # the call record in shim_poweroff's frame (SHIM_PO_CALL)
BCTR = 0x4E800420
SKIPPED = ("capstone not installed (pip install -r tools/requirements.txt)" if not _HAVE_CAPSTONE
           else "shim/shim.elf not built (run src/build.ps1 -Variant vsh)")


class _Cpu(object):
    """Registers, a sparse doubleword memory over the relocated blob image, and a
    record of everything the stub wrote."""

    def __init__(self, packed, regs, lr):
        self.p = packed
        img = bytearray(packed.blob)
        for off in packed.relocs:
            v = struct.unpack(">Q", img[off:off + 8])[0] + BASE
            img[off:off + 8] = struct.pack(">Q", v)
        # The data block's orig_* fields, written as the plugin does at install.
        struct.pack_into(">Q", img, packed.off_data + OFF_ORIG_POWEROFF, ORIG_POWEROFF)
        struct.pack_into(">Q", img, packed.off_data + OFF_ORIG_TEARDOWN, ORIG_TEARDOWN)
        self.img = bytes(img)
        self.r = [0] * 32
        for n, v in regs.items():
            self.r[n] = v
        self.entry_r = list(self.r)
        self.lr = self.entry_lr = lr
        self.ctr = 0
        self.cr7_eq = False
        self.mem = {}
        self.stores = []
        self.written = set()

    def load(self, a):
        if BASE <= a < BASE + len(self.img):
            return struct.unpack(">Q", self.img[a - BASE:a - BASE + 8])[0]
        return self.mem[a]

    def store(self, a, v):
        assert a % 8 == 0, "unaligned store at %#x" % a
        self.mem[a] = v & M64
        self.stores.append(a)

    def put(self, n, v):
        self.r[n] = v & M64
        self.written.add(n)


def _reg(s):
    assert s.startswith("r"), s
    return int(s[1:])


def _mem(s):
    disp, base = s.rstrip(")").split("(")
    return int(disp, 0), _reg(base)


def _bl_targets(packed, entry):
    """Code addresses (blob offsets) the stub at `entry` calls with `bl`: every
    link-form I-branch from the entry up to the stub's final bctr."""
    out = set()
    at = entry
    while True:
        w = struct.unpack(">I", packed.blob[at:at + 4])[0]
        if w == BCTR:
            return out
        if (w >> 26) == 18 and (w & 1):
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            out.add(at + li)
        at += 4
        if at > entry + 0x200:
            raise AssertionError("no bctr within 0x200 bytes of the stub at %#x" % entry)


def _toc_link(packed, entry):
    """The blob-relative .TOC. value the stub's literal holds (the one relocated
    doubleword between this stub's final bctr and the end of its window)."""
    at = entry
    while struct.unpack(">I", packed.blob[at:at + 4])[0] != BCTR:
        at += 4
    lits = [r for r in packed.relocs if at < r <= at + 8]
    assert len(lits) == 1, (hex(at), lits)
    return struct.unpack(">Q", packed.blob[lits[0]:lits[0] + 8])[0]


def _run(packed, cs, entry, regs, lr, callee):
    """Interpret the stub at blob offset `entry` until its bctr. `callee(cpu, target,
    ret)` models the C body at each bl. Returns the cpu."""
    cpu = _Cpu(packed, regs, lr)
    targets = _bl_targets(packed, entry)
    pc = BASE + entry
    for _ in range(500):
        off = pc - BASE
        ins = next(cs.disasm(cpu.img[off:off + 4], pc))
        m, ops = ins.mnemonic, [o.strip() for o in ins.op_str.split(",")]
        nxt = pc + 4
        if m == "mflr":
            cpu.put(_reg(ops[0]), cpu.lr)
        elif m == "mtlr":
            cpu.lr = cpu.r[_reg(ops[0])]
        elif m == "mtctr":
            cpu.ctr = cpu.r[_reg(ops[0])]
        elif m == "stdu":
            d, b = _mem(ops[1])
            ea = (cpu.r[b] + d) & M64
            cpu.store(ea, cpu.r[_reg(ops[0])])      # back chain: rs is r1 itself, the old value
            cpu.put(b, ea)
        elif m == "std":
            d, b = _mem(ops[1])
            cpu.store((cpu.r[b] + d) & M64, cpu.r[_reg(ops[0])])
        elif m == "ld":
            d, b = _mem(ops[1])
            assert b != 0, "ld with base r0 is not modelled"
            cpu.put(_reg(ops[0]), cpu.load((cpu.r[b] + d) & M64))
        elif m == "mr":
            cpu.put(_reg(ops[0]), cpu.r[_reg(ops[1])])
        elif m == "addi":
            assert _reg(ops[1]) != 0, "addi with ra = r0 is li, not modelled"
            cpu.put(_reg(ops[0]), cpu.r[_reg(ops[1])] + int(ops[2], 0))
        elif m == "nop":
            pass
        elif m == "bdnzl":                         # bcl 20,31,$+4: capture the PC in LR
            cpu.lr = pc + 4
        elif m == "cmpwi":
            assert ops[0] == "cr7", ins.op_str
            cpu.cr7_eq = (cpu.r[_reg(ops[1])] & 0xFFFFFFFF) == int(ops[2], 0)
            cpu.written.add("cr7")
        elif m == "beq":
            assert ops[0] == "cr7", ins.op_str
            if cpu.cr7_eq:
                nxt = int(ops[1], 0)
        elif m == "b":
            nxt = int(ops[0], 0)
        elif m == "bl":
            tgt = int(ops[0], 0)
            assert tgt - BASE in targets, "bl to %#x is not the stub's C body" % tgt
            callee(cpu, nxt)
        elif m == "bctr":
            return cpu
        else:
            raise AssertionError("unmodelled instruction %s %s at %#x" % (m, ins.op_str, pc))
        pc = nxt
        if pc < BASE:                              # capstone prints blob-relative targets
            pc += BASE
    raise AssertionError("stub did not reach its bctr within 500 steps")


def _model_callee(toc, entry_r1, check_args, body, result):
    """The C body at a bl: assert the ABI state, let `body` write results, then do
    what a compiled callee may do to everything it does not own."""
    def callee(cpu, ret):
        assert cpu.r[2] == BASE + toc, "r2 is not the blob's TOC at the call"
        assert cpu.r[1] % 16 == 0, "r1 not 16-aligned at the call"
        assert cpu.load(cpu.r[1]) == entry_r1, "back chain is not the caller's r1"
        check_args(cpu)
        body(cpu)
        # The callee may overwrite the stub frame's header and parameter area
        # (r1+0 .. r1+0x6F) and everything below r1. The back-chain doubleword at
        # r1+0 is rewritten with its own value (a callee does not change it).
        for a in range(cpu.r[1], cpu.r[1] + PO_CALL, 8):
            cpu.mem[a] = 0xBAD0BAD0BAD0BAD0
        for a in range(cpu.r[1] - 0x400, cpu.r[1], 8):
            cpu.mem[a] = 0xBAD1BAD1BAD1BAD1
        cpu.mem[cpu.r[1]] = entry_r1
        # ... and clobber every volatile register, LR and CTR.
        for n in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
            cpu.r[n] = 0xC10BBE40C10BBE40 + n
        cpu.r[3] = result
        cpu.lr = cpu.ctr = 0xDEADDEADDEADDEAD
    return callee


@unittest.skipUnless(_HAVE_CAPSTONE and os.path.exists(_REAL_ELF), SKIPPED)
class StubEmulationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(_REAL_ELF, "rb") as f:
            cls.p = shim_pack.pack(f.read())
        cls.cs = Cs(CS_ARCH_PPC, CS_MODE_64 | CS_MODE_BIG_ENDIAN)
        cls.toc = _toc_link(cls.p, cls.p.off_poweroff)

    @staticmethod
    def _nonvolatiles(base=0x1300):
        return {n: base + n for n in range(13, 32)}

    def _assert_common(self, cpu):
        entry_r1 = cpu.entry_r[1]
        self.assertEqual(cpu.r[1], entry_r1, "r1 not restored")
        self.assertEqual(cpu.r[2], KERNEL_TOC, "r2 (the kernel TOC) not restored")
        self.assertEqual(cpu.lr, cpu.entry_lr, "LR not restored")
        self.assertEqual([hex(a) for a in cpu.stores if a >= entry_r1], [],
                         "wrote at or above the entry r1")
        self.assertFalse([n for n in cpu.written if isinstance(n, int) and n > 12],
                         "touched a non-volatile register: %s" % sorted(cpu.written, key=str))
        for n in range(13, 32):
            self.assertEqual(cpu.r[n], cpu.entry_r[n], "r%d changed" % n)

    def test_both_stubs_share_one_toc(self):
        self.assertEqual(_toc_link(self.p, self.p.off_teardown), self.toc)

    def _poweroff(self, result):
        regs = {1: STACK, 2: KERNEL_TOC, 3: 5, 4: 3, 5: 0xF4, 6: 0x8000000004000100, 7: 4,
                8: 0x1122, 9: 0x8000000004000200, 10: 0x1010, 11: 0x1111, 12: 0x1212}
        regs.update(self._nonvolatiles())
        lr = 0x80000000000CA9C0
        new = (0x08, BASE + 0x1000, 46)          # the filter's o_rid / o_buf / o_len

        def check_args(cpu):
            rec = cpu.r[1] + PO_CALL
            self.assertEqual(cpu.r[3], rec, "r3 is not the address of the call record")
            want = [regs[n] for n in (3, 4, 5, 6, 7, 8, 9)]
            got = [cpu.load(rec + 8 * i) for i in range(7)]
            self.assertEqual([hex(x) for x in got], [hex(x) for x in want])

        def body(cpu):
            if result:
                rec = cpu.r[3]
                for i, v in enumerate(new):
                    cpu.mem[rec + 56 + 8 * i] = v

        callee = _model_callee(self.toc, STACK, check_args, body, result)
        cpu = _run(self.p, self.cs, self.p.off_poweroff, regs, lr, callee)
        self._assert_common(cpu)
        self.assertEqual(hex(cpu.ctr), hex(ORIG_POWEROFF))
        for n in (3, 4, 8, 9):
            self.assertEqual(cpu.r[n], regs[n], "r%d not restored" % n)
        want = new if result else (regs[5], regs[6], regs[7])
        self.assertEqual([hex(cpu.r[n]) for n in (5, 6, 7)], [hex(v) for v in want])

    def test_poweroff_filter_result_0_passes_the_original_arguments(self):
        self._poweroff(0)

    def test_poweroff_filter_result_1_substitutes_the_rewritten_report(self):
        self._poweroff(1)

    def _teardown(self, lr):
        regs = {1: STACK, 2: KERNEL_TOC, 3: 0x0000A1B2C3D4E5F6, 4: 1, 5: 0x5151, 6: 0x6161,
                7: 0x7171, 8: 0x8181, 9: 0x9191, 10: 0xA1A1, 11: 0xB1B1, 12: 0xC1C1}
        regs.update(self._nonvolatiles())

        def check_args(cpu):
            self.assertEqual([hex(cpu.r[3]), hex(cpu.r[4]), hex(cpu.r[5])],
                             [hex(regs[3]), hex(regs[4]), hex(lr)],
                             "the C body must see (address, flag, entry LR)")

        callee = _model_callee(self.toc, STACK, check_args, lambda cpu: None, 0xFEEDFEEDFEEDFEED)
        cpu = _run(self.p, self.cs, self.p.off_teardown, regs, lr, callee)
        self._assert_common(cpu)
        self.assertEqual(hex(cpu.ctr), hex(ORIG_TEARDOWN))
        for n in range(3, 11):
            self.assertEqual(cpu.r[n], regs[n], "r%d not restored" % n)

    def test_teardown_entered_by_bl(self):
        self._teardown(0x80000000000F4D98)       # LR = the instruction after a bl site

    def test_teardown_entered_by_tail_branch(self):
        self._teardown(0x80000000000F5A00)       # LR = the outer caller's, unrelated to the site


if __name__ == "__main__":
    unittest.main()
