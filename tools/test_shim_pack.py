"""Tests for tools/shim_pack.py - the kernel shim blob packer.

Run from the repository root:

    python -m unittest tools.test_shim_pack

Each test builds a tiny but structurally real ELF64 big-endian PPC64 image in
memory (one .text, an .opd with the three exported descriptors, a .toc and a
.data), feeds it to shim_pack, and checks the packed result.

Relocation policy under test (see shim_pack.py for the full rationale):
  * R_PPC64_ADDR64 -> rebased (offset recorded in the relocs list)
  * R_PPC64_TOC    -> rebased.  This is the *.TOC.* (TOC base) word of each
                      .opd descriptor: an absolute address that MUST get the
                      load base added, exactly as Task 4 found empirically.
                      (The task brief's one-line "drop R_PPC64_TOC" is a
                      misreading - dropping it leaves every descriptor's r2
                      pointing at link-time address 0; see the report.)
  * R_PPC64_TOC16* -> dropped (TOC-relative: an offset from r2, so it is
                      genuinely position independent)
  * R_PPC64_REL24  -> never rebased (PC-relative), but a REL24 that lands in
                      .opd (an ELFv1 function descriptor, not code) is
                      rewritten to branch to the descriptor's code entry, and
                      the pack fails if any is left pointing into .opd
  * anything else, any undefined symbol, or a TOC/data reference inside
    shim_pid_ok's code -> hard error (non-zero exit)
"""

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shim_pack  # noqa: E402  (deliberately from tools/, never a build dir)


# --- ELF constants -------------------------------------------------------
ET_EXEC = 2
EM_PPC64 = 21
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 1, 2, 3, 4, 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
STB_LOCAL, STB_GLOBAL = 0, 1
STT_NOTYPE, STT_OBJECT, STT_FUNC, STT_SECTION = 0, 1, 2, 3
SHN_UNDEF = 0

R_PPC64_ADDR64 = 38
R_PPC64_TOC = 51
R_PPC64_REL24 = 10
R_PPC64_TOC16_HA = 50
R_PPC64_TOC16_DS = 63
R_PPC64_ADDR32 = 1   # a type the packer must refuse


def _st_info(bind, typ):
    return (bind << 4) | (typ & 0xF)


class _StrTab(object):
    def __init__(self):
        self.buf = bytearray(b"\x00")
        self.off = {"": 0}

    def add(self, s):
        if s not in self.off:
            self.off[s] = len(self.buf)
            self.buf += s.encode("ascii") + b"\x00"
        return self.off[s]


class ElfBuilder(object):
    """Assemble a minimal ELF64 BE PPC64 executable with chosen relocs."""

    # Fixed section indices.
    I_NULL, I_TEXT, I_OPD, I_TOC, I_DATA = 0, 1, 2, 3, 4
    I_SYMTAB, I_STRTAB = 5, 6
    I_RELA_TEXT, I_RELA_OPD, I_RELA_TOC = 7, 8, 9
    I_SHSTRTAB = 10
    NSECT = 11

    def __init__(self):
        # Allocatable section payloads and virtual addresses.
        self.text_addr = 0x0
        self.text = bytearray(0x180)            # code bytes are opaque here
        self.opd_addr = 0x180
        self.toc_addr = 0x228
        self.toc = bytearray()
        self.data_addr = 0x230
        self.data = bytearray(144)              # ds45_shim_data image

        # Code layout inside .text (addr, size).
        self.code = {
            "shim_event": (0x00, 0x40),
            "shim_input": (0x40, 0x40),
            "shim_pid_ok": (0x80, 0x10),
            "shim_output": (0x90, 0x30),    # stub; its TOC literal is at 0xB8
            "shim_enable": (0xC0, 0x40),
            "shim_poweroff": (0x100, 0x40),  # stub; its TOC literal is at 0x138
            "shim_teardown": (0x140, 0x40),  # stub; its TOC literal is at 0x178
        }
        # Descriptors in .opd order: event, enable, input, pid_ok, output,
        # poweroff, teardown.
        self._desc_order = ["shim_event", "shim_enable", "shim_input",
                            "shim_pid_ok", "shim_output", "shim_poweroff",
                            "shim_teardown"]

        self.sym = []     # list of dicts
        self.symidx = {}  # name -> index
        self._add_sym("", 0, 0, STT_NOTYPE, STB_LOCAL, SHN_UNDEF)
        # Section symbols (local), useful as reloc targets.
        self._add_sym(".text", self.text_addr, 0, STT_SECTION, STB_LOCAL,
                      self.I_TEXT)
        self._add_sym(".opd", self.opd_addr, 0, STT_SECTION, STB_LOCAL,
                      self.I_OPD)
        self._add_sym(".data", self.data_addr, 0, STT_SECTION, STB_LOCAL,
                      self.I_DATA)
        # The seven symbols the header is built from (global). A function's
        # value is its .opd descriptor address (the ELFv1 convention).
        for i, n in enumerate(self._desc_order):
            _addr, size = self.code[n]
            self._add_sym(n, self.opd_addr + 0x18 * i,
                          size, STT_FUNC, STB_GLOBAL, self.I_OPD)
        self._add_sym("ds45_shim_data", self.data_addr, len(self.data),
                      STT_OBJECT, STB_GLOBAL, self.I_DATA)

        # .opd descriptors: [entry=code addr, toc=arbitrary, env=0].
        self.opd = bytearray()
        for n in self._desc_order:
            addr, _ = self.code[n]
            self.opd += struct.pack(">QQQ", addr, 0x8148, 0)

        # .toc: one 8-byte pointer to ds45_shim_data.
        self.toc += struct.pack(">Q", self.data_addr)

        # Default relocation sets. Each of the output, power-off and teardown
        # stubs' blob-TOC literal is an aligned 8-byte word inside .text carrying
        # an R_PPC64_TOC relocation (`.quad .TOC.@tocbase`), rebased like the
        # descriptor words.
        self.rela = {self.I_RELA_TEXT: [(0xB8, 0, R_PPC64_TOC, 0),
                                        (0x138, 0, R_PPC64_TOC, 0),
                                        (0x178, 0, R_PPC64_TOC, 0)],
                     self.I_RELA_OPD: [], self.I_RELA_TOC: []}
        for i, _n in enumerate(self._desc_order):
            base = self.opd_addr + 0x18 * i
            self.rela[self.I_RELA_OPD].append(
                (base, self.symidx[".text"], R_PPC64_ADDR64, self.code[_n][0]))
            self.rela[self.I_RELA_OPD].append(
                (base + 8, 0, R_PPC64_TOC, 0))
        self.rela[self.I_RELA_TOC].append(
            (self.toc_addr, self.symidx["ds45_shim_data"], R_PPC64_ADDR64, 0))

    # -- symbol / descriptor helpers --
    def _add_sym(self, name, value, size, typ, bind, shndx):
        self.symidx[name] = len(self.sym)
        self.sym.append(dict(name=name, value=value, size=size, typ=typ,
                             bind=bind, shndx=shndx))

    # -- public knobs --
    def add_text_reloc(self, offset, rtype, sym_name=None, addend=0):
        sidx = 0 if sym_name is None else self.symidx[sym_name]
        self.rela[self.I_RELA_TEXT].append((offset, sidx, rtype, addend))

    def add_undef_symbol(self, name):
        self._add_sym(name, 0, 0, STT_NOTYPE, STB_GLOBAL, SHN_UNDEF)
        return self.symidx[name]

    def pid_ok_code(self):
        return self.code["shim_pid_ok"]

    # -- assembly --
    def build(self):
        shstr = _StrTab()
        names = ["", ".text", ".opd", ".toc", ".data", ".symtab", ".strtab",
                 ".rela.text", ".rela.opd", ".rela.toc", ".shstrtab"]
        name_off = [shstr.add(n) for n in names]

        strtab = _StrTab()
        sym_bytes = bytearray()
        for s in self.sym:
            sym_bytes += struct.pack(
                ">IBBHQQ", strtab.add(s["name"]),
                _st_info(s["bind"], s["typ"]), 0, s["shndx"],
                s["value"], s["size"])

        def rela_bytes(idx):
            b = bytearray()
            for (off, sidx, rtype, add) in self.rela[idx]:
                b += struct.pack(">QQq", off, (sidx << 32) | rtype, add)
            return bytes(b)

        payloads = {
            self.I_TEXT: bytes(self.text),
            self.I_OPD: bytes(self.opd),
            self.I_TOC: bytes(self.toc),
            self.I_DATA: bytes(self.data),
            self.I_SYMTAB: bytes(sym_bytes),
            self.I_STRTAB: bytes(strtab.buf),
            self.I_RELA_TEXT: rela_bytes(self.I_RELA_TEXT),
            self.I_RELA_OPD: rela_bytes(self.I_RELA_OPD),
            self.I_RELA_TOC: rela_bytes(self.I_RELA_TOC),
            self.I_SHSTRTAB: bytes(shstr.buf),
        }

        meta = {
            self.I_TEXT: dict(type=SHT_PROGBITS, flags=SHF_ALLOC | SHF_EXECINSTR,
                              addr=self.text_addr, link=0, info=0, align=8,
                              entsize=0),
            self.I_OPD: dict(type=SHT_PROGBITS, flags=SHF_ALLOC | SHF_WRITE,
                             addr=self.opd_addr, link=0, info=0, align=8,
                             entsize=0),
            self.I_TOC: dict(type=SHT_PROGBITS, flags=SHF_ALLOC | SHF_WRITE,
                             addr=self.toc_addr, link=0, info=0, align=8,
                             entsize=0),
            self.I_DATA: dict(type=SHT_PROGBITS, flags=SHF_ALLOC | SHF_WRITE,
                              addr=self.data_addr, link=0, info=0, align=8,
                              entsize=0),
            self.I_SYMTAB: dict(type=SHT_SYMTAB, flags=0, addr=0,
                                link=self.I_STRTAB, info=4, align=8,
                                entsize=24),
            self.I_STRTAB: dict(type=SHT_STRTAB, flags=0, addr=0, link=0,
                                info=0, align=1, entsize=0),
            self.I_RELA_TEXT: dict(type=SHT_RELA, flags=0, addr=0,
                                   link=self.I_SYMTAB, info=self.I_TEXT,
                                   align=8, entsize=24),
            self.I_RELA_OPD: dict(type=SHT_RELA, flags=0, addr=0,
                                  link=self.I_SYMTAB, info=self.I_OPD,
                                  align=8, entsize=24),
            self.I_RELA_TOC: dict(type=SHT_RELA, flags=0, addr=0,
                                  link=self.I_SYMTAB, info=self.I_TOC,
                                  align=8, entsize=24),
            self.I_SHSTRTAB: dict(type=SHT_STRTAB, flags=0, addr=0, link=0,
                                  info=0, align=1, entsize=0),
        }

        # Lay out: ehdr(64), section payloads (8-aligned), section headers.
        ehsize = 64
        shentsize = 64
        pos = ehsize
        offsets = {}
        blob = bytearray()
        blob += b"\x00" * ehsize
        for idx in range(1, self.NSECT):
            pos = (pos + 7) & ~7
            while len(blob) < pos:
                blob += b"\x00"
            offsets[idx] = pos
            blob += payloads[idx]
            pos = len(blob)

        pos = (pos + 7) & ~7
        while len(blob) < pos:
            blob += b"\x00"
        shoff = pos

        # Section header table.
        def shdr(name_o, typ, flags, addr, off, size, link, info, align, ent):
            return struct.pack(">IIQQQQIIQQ", name_o, typ, flags, addr, off,
                               size, link, info, align, ent)

        sh = bytearray()
        sh += shdr(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)  # NULL
        for idx in range(1, self.NSECT):
            m = meta[idx]
            sh += shdr(name_off[idx], m["type"], m["flags"], m["addr"],
                       offsets[idx], len(payloads[idx]), m["link"], m["info"],
                       m["align"], m["entsize"])
        blob += sh

        # ELF header.
        e_ident = bytes([0x7F]) + b"ELF" + bytes([2, 2, 1, 0]) + b"\x00" * 8
        ehdr = e_ident + struct.pack(
            ">HHIQQQIHHHHHH", ET_EXEC, EM_PPC64, 1,
            0,          # e_entry
            0,          # e_phoff
            shoff,      # e_shoff
            0,          # e_flags
            ehsize, 0, 0, shentsize, self.NSECT, self.I_SHSTRTAB)
        blob[0:ehsize] = ehdr
        return bytes(blob)


# Fixture addresses: the .opd descriptors (24 bytes each, in _desc_order) and
# the three stubs' TOC literals in .text.
D_EVENT, D_ENABLE, D_INPUT, D_PIDOK = 0x180, 0x198, 0x1B0, 0x1C8
D_OUTPUT, D_POWEROFF, D_TEARDOWN = 0x1E0, 0x1F8, 0x210
LIT_OUTPUT, LIT_POWEROFF, LIT_TEARDOWN = 0xB8, 0x138, 0x178

# The relocs a clean ElfBuilder image yields: the three stubs' TOC literals in
# .text, the seven descriptors' entry (ADDR64) and TOC words, and the .toc
# pointer to ds45_shim_data.
_CLEAN_RELOCS = [0xB8, 0x138, 0x178,
                 0x180, 0x188, 0x198, 0x1A0, 0x1B0, 0x1B8, 0x1C8, 0x1D0,
                 0x1E0, 0x1E8, 0x1F8, 0x200, 0x210, 0x218, 0x228]


class PackSuccessTest(unittest.TestCase):
    def test_offsets_and_relocs(self):
        p = shim_pack.pack(ElfBuilder().build())
        self.assertEqual(p.off_event, D_EVENT)
        self.assertEqual(p.off_enable, D_ENABLE)
        self.assertEqual(p.off_input, D_INPUT)
        self.assertEqual(p.off_pid_ok, 0x80)   # a code address, not a descriptor
        self.assertEqual(p.off_output, 0x90)   # a code address, not a descriptor
        self.assertEqual(p.off_poweroff, 0x100)
        self.assertEqual(p.off_teardown, 0x140)
        self.assertEqual(p.off_data, 0x230)
        self.assertEqual(p.blob_len, 0x2C0)
        self.assertEqual(p.blob_len % 8, 0)
        self.assertEqual(len(p.blob), p.blob_len)
        # ADDR64 (7 entry words + 1 toc ptr), TOC (7 descriptor words) and the
        # three stubs' TOC literals inside .text.
        self.assertEqual(p.relocs, _CLEAN_RELOCS)
        # The image carries the real descriptor entry words.
        self.assertEqual(struct.unpack(">Q", p.blob[D_EVENT:D_EVENT + 8])[0], 0x00)
        self.assertEqual(struct.unpack(">Q", p.blob[D_ENABLE:D_ENABLE + 8])[0], 0xC0)
        self.assertEqual(struct.unpack(">Q", p.blob[D_PIDOK:D_PIDOK + 8])[0], 0x80)
        self.assertEqual(struct.unpack(">Q", p.blob[D_OUTPUT:D_OUTPUT + 8])[0], 0x90)
        self.assertEqual(struct.unpack(">Q", p.blob[D_POWEROFF:D_POWEROFF + 8])[0], 0x100)
        self.assertEqual(struct.unpack(">Q", p.blob[D_TEARDOWN:D_TEARDOWN + 8])[0], 0x140)

    def test_header_text(self):
        p = shim_pack.pack(ElfBuilder().build())
        h = shim_pack.render_header(p, "shim.elf")
        self.assertIn("#define DS45_SHIM_BLOB_LEN 704", h)
        self.assertIn("#define DS45_SHIM_RELOC_COUNT 18", h)
        self.assertIn("#define DS45_SHIM_OFF_DATA 0x230", h)
        self.assertIn("#define DS45_SHIM_OFF_EVENT 0x180", h)
        self.assertIn("#define DS45_SHIM_OFF_ENABLE 0x198", h)
        self.assertIn("#define DS45_SHIM_OFF_INPUT 0x1b0", h)
        self.assertIn("#define DS45_SHIM_OFF_PID_OK 0x80", h)
        # The output stub's offset is its code entry, read from the descriptor.
        self.assertIn("#define DS45_SHIM_OFF_OUTPUT 0x90", h)
        self.assertIn("#define DS45_SHIM_OFF_POWEROFF 0x100", h)
        self.assertIn("#define DS45_SHIM_OFF_TEARDOWN 0x140", h)
        # The v2 start-up hook is gone.
        self.assertNotIn("DS45_SHIM_OFF_STARTUP", h)
        self.assertIn("static const uint8_t  ds45_shim_blob[]", h)
        self.assertIn("static const uint32_t ds45_shim_relocs[]", h)


class TocRebaseTest(unittest.TestCase):
    """R_PPC64_TOC (the .opd TOC-base word) must be REBASED, not dropped.

    Overrides the task brief's one-line 'drop R_PPC64_TOC': that word is the
    absolute .TOC. address; dropping it breaks the blob's r2. See report.
    """

    def test_toc_word_is_rebased(self):
        p = shim_pack.pack(ElfBuilder().build())
        # The descriptors' R_PPC64_TOC words (entry + 8).
        for toc_off in (D_EVENT + 8, D_ENABLE + 8, D_INPUT + 8, D_PIDOK + 8,
                        D_OUTPUT + 8, D_POWEROFF + 8, D_TEARDOWN + 8):
            self.assertIn(toc_off, p.relocs)


class OutputStubTest(unittest.TestCase):
    """The output stub is entered by a plain `bl` and adopts the blob TOC from a
    literal word in .text; the packer must rebase that word and export the
    stub's code entry."""

    def test_literal_in_text_is_rebased(self):
        p = shim_pack.pack(ElfBuilder().build())
        self.assertIn(0xB8, p.relocs)
        self.assertEqual(p.relocs, sorted(set(p.relocs)))

    def test_literal_as_addr64_is_rebased_too(self):
        b = ElfBuilder()
        b.rela[b.I_RELA_TEXT].append((0xA0, b.symidx["ds45_shim_data"],
                                      R_PPC64_ADDR64, 0))
        p = shim_pack.pack(b.build())
        self.assertIn(0xA0, p.relocs)

    def test_misaligned_literal_errors(self):
        b = ElfBuilder()
        b.rela[b.I_RELA_TEXT].append((0xBC, 0, R_PPC64_TOC, 0))
        with self.assertRaises(shim_pack.PackError):
            shim_pack.pack(b.build())

    def test_output_entry_is_the_descriptor_code_word(self):
        b = ElfBuilder()
        b.opd[0x60:0x68] = struct.pack(">Q", 0x94)          # output descriptor
        p = shim_pack.pack(b.build())
        self.assertEqual(p.off_output, 0x94)

    def test_output_entry_outside_text_errors(self):
        for bad in (D_EVENT + 0x10, 0x2000, 0x92):  # in .opd, outside, misaligned
            b = ElfBuilder()
            b.opd[0x60:0x68] = struct.pack(">Q", bad)
            with self.assertRaises(shim_pack.PackError):
                shim_pack.pack(b.build())


class PoweroffTeardownStubTest(unittest.TestCase):
    """shim_poweroff and shim_teardown are entered by plain `bl` / `b` from the
    kernel's code and adopt the blob TOC from a literal word in .text, exactly
    like shim_output; the packer exports their code entries and rebases the
    literals."""

    def test_poweroff_and_teardown_exports(self):
        p = shim_pack.pack(ElfBuilder().build())
        # Code addresses (read from the descriptors' first word), 4-aligned.
        self.assertEqual(p.off_poweroff, 0x100)
        self.assertEqual(p.off_teardown, 0x140)
        self.assertEqual(p.off_poweroff % 4, 0)
        self.assertEqual(p.off_teardown % 4, 0)
        h = shim_pack.render_header(p, "shim.elf")
        self.assertIn("#define DS45_SHIM_OFF_POWEROFF 0x100 ", h)
        self.assertIn("#define DS45_SHIM_OFF_TEARDOWN 0x140 ", h)
        # The TOC literals are rebased like the shim_output one.
        for lit in (LIT_OUTPUT, LIT_POWEROFF, LIT_TEARDOWN):
            self.assertIn(lit, p.relocs)
            self.assertEqual(lit % 8, 0)
        self.assertEqual(p.relocs, sorted(set(p.relocs)))

    def test_entries_are_the_descriptor_code_words(self):
        b = ElfBuilder()
        b.opd[0x78:0x80] = struct.pack(">Q", 0x104)         # poweroff descriptor
        b.opd[0x90:0x98] = struct.pack(">Q", 0x144)         # teardown descriptor
        p = shim_pack.pack(b.build())
        self.assertEqual(p.off_poweroff, 0x104)
        self.assertEqual(p.off_teardown, 0x144)

    def test_entry_outside_text_errors(self):
        for desc_off in (0x78, 0x90):                       # poweroff, teardown
            for bad in (D_EVENT + 0x10, 0x2000, 0x102):     # in .opd, outside, misaligned
                b = ElfBuilder()
                b.opd[desc_off:desc_off + 8] = struct.pack(">Q", bad)
                with self.assertRaises(shim_pack.PackError):
                    shim_pack.pack(b.build())

    def test_missing_symbol_errors(self):
        for name in ("shim_poweroff", "shim_teardown"):
            b = ElfBuilder()
            b.sym[b.symidx[name]]["name"] = name + "_renamed"
            with self.assertRaises(shim_pack.PackError):
                shim_pack.pack(b.build())

    def test_misaligned_literal_errors(self):
        for lit in (LIT_POWEROFF, LIT_TEARDOWN):
            b = ElfBuilder()
            b.rela[b.I_RELA_TEXT].append((lit + 4, 0, R_PPC64_TOC, 0))
            with self.assertRaises(shim_pack.PackError):
                shim_pack.pack(b.build())


class TocRelativeDropTest(unittest.TestCase):
    """TOC-relative (R_PPC64_TOC16*) relocs are position independent: the
    packer must accept and DROP them, never error, and never rebase them."""

    def test_toc16_outside_pidok_is_dropped(self):
        b = ElfBuilder()
        b.add_text_reloc(0x10, R_PPC64_TOC16_DS, "ds45_shim_data")  # in event
        b.add_text_reloc(0x14, R_PPC64_TOC16_HA, "ds45_shim_data")
        p = shim_pack.pack(b.build())
        self.assertNotIn(0x10, p.relocs)
        self.assertNotIn(0x14, p.relocs)
        # Unchanged from the clean build.
        self.assertEqual(p.relocs, _CLEAN_RELOCS)


def _bl(frm, to):
    """Encode `bl to` placed at address `frm`."""
    return struct.pack(">I", 0x48000001 | ((to - frm) & 0x03FFFFFC))


def _decode_bl(blob, at):
    w = struct.unpack(">I", blob[at:at + 4])[0]
    assert (w >> 26) == 18 and (w & 1) == 1 and (w & 2) == 0, hex(w)
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return at + li


class Rel24RetargetTest(unittest.TestCase):
    """A REL24 aimed at an .opd descriptor must end up aimed at its code."""

    OPD_LO, OPD_HI = 0x180, 0x228

    def test_named_function_symbol_is_retargeted(self):
        b = ElfBuilder()
        # lld left the branch on the descriptor of shim_input.
        b.text[0x10:0x14] = _bl(0x10, D_INPUT)
        b.add_text_reloc(0x10, R_PPC64_REL24, "shim_input")
        p = shim_pack.pack(b.build())
        self.assertEqual(_decode_bl(p.blob, 0x10), 0x40)    # shim_input code
        self.assertNotIn(0x10, p.relocs)                   # PC-relative

    def test_section_symbol_with_addend_is_retargeted(self):
        b = ElfBuilder()
        b.text[0x14:0x18] = _bl(0x14, D_ENABLE)            # .opd+0x18 = enable
        b.add_text_reloc(0x14, R_PPC64_REL24, ".opd", 0x18)
        p = shim_pack.pack(b.build())
        self.assertEqual(_decode_bl(p.blob, 0x14), 0xC0)    # shim_enable code
        self.assertNotIn(0x14, p.relocs)

    def test_branch_to_text_is_left_alone(self):
        b = ElfBuilder()
        b.text[0x18:0x1C] = _bl(0x18, 0x40)
        b.add_text_reloc(0x18, R_PPC64_REL24, ".text", 0x40)
        p = shim_pack.pack(b.build())
        self.assertEqual(_decode_bl(p.blob, 0x18), 0x40)
        self.assertEqual(p.relocs, _CLEAN_RELOCS)

    def test_retarget_keeps_other_instruction_bits(self):
        b = ElfBuilder()
        b.text[0x1C:0x20] = _bl(0x1C, D_INPUT)             # bl, LK=1
        b.add_text_reloc(0x1C, R_PPC64_REL24, "shim_input")
        p = shim_pack.pack(b.build())
        w = struct.unpack(">I", p.blob[0x1C:0x20])[0]
        self.assertEqual(w >> 26, 18)
        self.assertEqual(w & 3, 1)                          # AA=0, LK=1

    def test_descriptor_entry_outside_text_errors(self):
        b = ElfBuilder()
        b.text[0x10:0x14] = _bl(0x10, D_INPUT)
        b.add_text_reloc(0x10, R_PPC64_REL24, "shim_input")
        # shim_input's descriptor (index 2, .opd+0x30) -> an address in .opd.
        b.opd[0x30:0x38] = struct.pack(">Q", D_EVENT + 0x10)
        with self.assertRaises(shim_pack.PackError):
            shim_pack.pack(b.build())

    def test_branch_still_into_opd_is_a_guard_error(self):
        # The reloc claims the target is .text, but the instruction really
        # branches into .opd: the final guard must refuse it.
        b = ElfBuilder()
        b.text[0x10:0x14] = _bl(0x10, D_INPUT)
        b.add_text_reloc(0x10, R_PPC64_REL24, ".text", 0x40)
        with self.assertRaises(shim_pack.PackError):
            shim_pack.pack(b.build())


_REAL_ELF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "shim", "shim.elf")


@unittest.skipUnless(os.path.exists(_REAL_ELF),
                     "shim/shim.elf not built (run src/build.ps1 -Variant vsh)")
class RealElfRel24Test(unittest.TestCase):
    """Over the real linked blob: no direct branch in the code section may
    land in .opd, and every REL24 the link recorded as aimed at a descriptor
    now decodes to a branch into code."""

    def test_no_internal_branch_lands_in_opd(self):
        with open(_REAL_ELF, "rb") as f:
            data = f.read()
        p = shim_pack.pack(data)
        # Section bounds straight from the ELF section headers.
        e_shoff = struct.unpack(">Q", data[0x28:0x30])[0]
        e_shnum, e_shstrndx = struct.unpack(">HH", data[0x3C:0x40])
        secs = []
        for i in range(e_shnum):
            f = struct.unpack(">IIQQQQIIQQ", data[e_shoff + 64 * i:e_shoff + 64 * i + 64])
            secs.append(f)
        stro = secs[e_shstrndx][4]

        def name(f):
            end = data.index(b"\0", stro + f[0])
            return data[stro + f[0]:end].decode()
        by = {name(f): f for f in secs}
        text_lo, text_hi = by[".text"][3], by[".text"][3] + by[".text"][5]
        opd_lo, opd_hi = by[".opd"][3], by[".opd"][3] + by[".opd"][5]
        self.assertLess(text_hi, opd_hi)
        # Every REL24 relocation site: the packed branch must be code.
        n_rel24 = 0
        for f in secs:
            if f[1] != SHT_RELA:
                continue
            for i in range(f[5] // 24):
                o = f[4] + i * 24
                r_off, r_info, _add = struct.unpack(">QQq", data[o:o + 24])
                if (r_info & 0xFFFFFFFF) != R_PPC64_REL24:
                    continue
                n_rel24 += 1
                tgt = _decode_branch(p.blob, r_off)
                self.assertTrue(text_lo <= tgt < text_hi,
                                "REL24 at 0x%x -> 0x%x not in .text" % (r_off, tgt))
                self.assertFalse(opd_lo <= tgt < opd_hi)
        self.assertGreater(n_rel24, 0)


@unittest.skipUnless(os.path.exists(_REAL_ELF),
                     "shim/shim.elf not built (run src/build.ps1 -Variant vsh)")
class RealElfOutputStubTest(unittest.TestCase):
    """Over the real linked blob: shim_output exports a code address that starts
    with `mflr r0`, and the stub's TOC literal (an aligned word inside .text
    after the stub) is in the relocation list."""

    def test_stub_entry_and_literal(self):
        with open(_REAL_ELF, "rb") as f:
            p = shim_pack.pack(f.read())
        self.assertEqual(p.off_output % 4, 0)
        self.assertEqual(struct.unpack(">I", p.blob[p.off_output:p.off_output + 4])[0],
                         0x7C0802A6)                  # mflr 0
        lits = [r for r in p.relocs if p.off_output < r < p.off_output + 0x100]
        self.assertEqual(len(lits), 1, lits)
        self.assertEqual(lits[0] % 8, 0)


@unittest.skipUnless(os.path.exists(_REAL_ELF),
                     "shim/shim.elf not built (run src/build.ps1 -Variant vsh)")
class RealElfPoweroffTeardownStubTest(unittest.TestCase):
    """Over the real linked blob: shim_poweroff and shim_teardown export code
    addresses that start with `mflr 0`, and each stub has exactly one TOC
    literal (an aligned word inside .text after it) in the relocation list."""

    def test_stub_entries_and_literals(self):
        with open(_REAL_ELF, "rb") as f:
            data = f.read()
        p = shim_pack.pack(data)
        entries = sorted([p.off_output, p.off_poweroff, p.off_teardown])
        self.assertEqual(len(set(entries)), 3)
        for e in (p.off_poweroff, p.off_teardown):
            self.assertEqual(e % 4, 0)
            self.assertEqual(struct.unpack(">I", p.blob[e:e + 4])[0],
                             0x7C0802A6)                  # mflr 0
        # Between one stub's entry and the next stub's entry (for the last stub:
        # the start of .opd, which follows all of .text) lies exactly one
        # rebased word: that stub's literal.
        opd_lo = _section_addr(data, ".opd")
        self.assertGreater(opd_lo, entries[-1])
        for i, e in enumerate(entries):
            hi = entries[i + 1] if i + 1 < len(entries) else opd_lo
            lits = [r for r in p.relocs if e < r < hi]
            self.assertEqual(len(lits), 1, (hex(e), lits))
            self.assertEqual(lits[0] % 8, 0)


def _section_addr(data, wanted):
    """sh_addr of the named section of the ELF image `data` (blob offsets are
    section addresses; the blob starts at the first byte of .text, address 0)."""
    e_shoff = struct.unpack(">Q", data[0x28:0x30])[0]
    e_shnum, e_shstrndx = struct.unpack(">HH", data[0x3C:0x40])
    secs = [struct.unpack(">IIQQQQIIQQ", data[e_shoff + 64 * i:e_shoff + 64 * i + 64])
            for i in range(e_shnum)]
    stro = secs[e_shstrndx][4]
    for f in secs:
        end = data.index(b"\0", stro + f[0])
        if data[stro + f[0]:end].decode() == wanted:
            return f[3]
    raise AssertionError("no section %s" % wanted)


def _decode_branch(blob, at):
    w = struct.unpack(">I", blob[at:at + 4])[0]
    assert (w >> 26) == 18, hex(w)
    li = w & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return at + li


class ErrorTest(unittest.TestCase):
    def _expect_error(self, builder):
        data = builder.build()
        with self.assertRaises(shim_pack.PackError):
            shim_pack.pack(data)
        # main() must map the failure to a non-zero exit.
        with tempfile.TemporaryDirectory() as d:
            elf = os.path.join(d, "x.elf")
            out = os.path.join(d, "shim_blob.h")
            with open(elf, "wb") as f:
                f.write(data)
            self.assertNotEqual(shim_pack.main([elf, out]), 0)
            self.assertFalse(os.path.exists(out))

    def test_rel24_to_undefined_symbol_errors(self):
        b = ElfBuilder()
        b.add_undef_symbol("missing_helper")
        b.add_text_reloc(0x10, R_PPC64_REL24, "missing_helper")
        self._expect_error(b)

    def test_toc16_ha_inside_pid_ok_errors(self):
        b = ElfBuilder()
        addr, size = b.pid_ok_code()
        b.add_text_reloc(addr + 4, R_PPC64_TOC16_HA, "ds45_shim_data")
        self._expect_error(b)

    def test_unknown_reloc_type_errors(self):
        b = ElfBuilder()
        b.add_text_reloc(0x10, R_PPC64_ADDR32, ".text")
        self._expect_error(b)

    def test_each_required_symbol_is_required(self):
        # shim_event and shim_enable (the two completion hooks), shim_input,
        # shim_pid_ok and the data object: renaming any one away is an error.
        for name in ("shim_event", "shim_enable", "shim_input", "shim_pid_ok",
                     "shim_output", "shim_poweroff", "shim_teardown",
                     "ds45_shim_data"):
            b = ElfBuilder()
            b.sym[b.symidx[name]]["name"] = name + "_renamed"
            self._expect_error(b)

    def test_startup_symbol_is_not_needed(self):
        # The v2 start-up hook is gone: an image without it packs.
        b = ElfBuilder()
        self.assertNotIn("shim_startup", b.symidx)
        shim_pack.pack(b.build())


class MainTest(unittest.TestCase):
    def test_main_writes_header_file(self):
        data = ElfBuilder().build()
        with tempfile.TemporaryDirectory() as d:
            elf = os.path.join(d, "shim.elf")
            out = os.path.join(d, "shim_blob.h")
            with open(elf, "wb") as f:
                f.write(data)
            self.assertEqual(shim_pack.main([elf, out]), 0)
            with open(out, "r") as f:
                h = f.read()
            self.assertIn("#define DS45_SHIM_OFF_PID_OK 0x80", h)
            self.assertIn("#define DS45_SHIM_OFF_OUTPUT 0x90", h)
            self.assertIn("#define DS45_SHIM_OFF_POWEROFF 0x100", h)
            self.assertIn("#define DS45_SHIM_OFF_TEARDOWN 0x140", h)
            self.assertIn("#define DS45_SHIM_RELOC_COUNT 18", h)


if __name__ == "__main__":
    unittest.main()
