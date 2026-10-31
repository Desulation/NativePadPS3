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
  * R_PPC64_REL24  -> dropped (PC-relative branch)
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
        self.text = bytearray(0x100)            # code bytes are opaque here
        self.opd_addr = 0x100
        self.toc_addr = 0x148
        self.toc = bytearray()
        self.data_addr = 0x150
        self.data = bytearray(144)              # ds45_shim_data image

        # Code layout inside .text (addr, size).
        self.code = {
            "shim_startup": (0x00, 0x40),
            "shim_input": (0x40, 0x40),
            "shim_pid_ok": (0x80, 0x10),
        }
        # Descriptors in .opd order: startup, input, pid_ok.
        self._desc_order = ["shim_startup", "shim_input", "shim_pid_ok"]

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
        # The four symbols the header is built from (global). A function's
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

        # Default relocation sets.
        self.rela = {self.I_RELA_TEXT: [], self.I_RELA_OPD: [],
                     self.I_RELA_TOC: []}
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


class PackSuccessTest(unittest.TestCase):
    def test_offsets_and_relocs(self):
        p = shim_pack.pack(ElfBuilder().build())
        self.assertEqual(p.off_startup, 0x100)
        self.assertEqual(p.off_input, 0x118)
        self.assertEqual(p.off_pid_ok, 0x80)   # a code address, not a descriptor
        self.assertEqual(p.off_data, 0x150)
        self.assertEqual(p.blob_len, 0x1E0)
        self.assertEqual(p.blob_len % 8, 0)
        self.assertEqual(len(p.blob), p.blob_len)
        # ADDR64 (3 entry words + 1 toc ptr) and TOC (3 descriptor words).
        self.assertEqual(
            p.relocs,
            [0x100, 0x108, 0x118, 0x120, 0x130, 0x138, 0x148])
        # The image carries the real descriptor entry words.
        self.assertEqual(struct.unpack(">Q", p.blob[0x100:0x108])[0], 0x00)
        self.assertEqual(struct.unpack(">Q", p.blob[0x130:0x138])[0], 0x80)

    def test_header_text(self):
        p = shim_pack.pack(ElfBuilder().build())
        h = shim_pack.render_header(p, "shim.elf")
        self.assertIn("#define DS45_SHIM_BLOB_LEN 480", h)
        self.assertIn("#define DS45_SHIM_RELOC_COUNT 7", h)
        self.assertIn("#define DS45_SHIM_OFF_DATA 0x150", h)
        self.assertIn("#define DS45_SHIM_OFF_STARTUP 0x100", h)
        self.assertIn("#define DS45_SHIM_OFF_INPUT 0x118", h)
        self.assertIn("#define DS45_SHIM_OFF_PID_OK 0x80", h)
        self.assertIn("static const uint8_t  ds45_shim_blob[]", h)
        self.assertIn("static const uint32_t ds45_shim_relocs[]", h)


class TocRebaseTest(unittest.TestCase):
    """R_PPC64_TOC (the .opd TOC-base word) must be REBASED, not dropped.

    Overrides the task brief's one-line 'drop R_PPC64_TOC': that word is the
    absolute .TOC. address; dropping it breaks the blob's r2. See report.
    """

    def test_toc_word_is_rebased(self):
        p = shim_pack.pack(ElfBuilder().build())
        # 0x108/0x120/0x138 are the descriptors' R_PPC64_TOC words.
        for toc_off in (0x108, 0x120, 0x138):
            self.assertIn(toc_off, p.relocs)


class TocRelativeDropTest(unittest.TestCase):
    """TOC-relative (R_PPC64_TOC16*) relocs are position independent: the
    packer must accept and DROP them, never error, and never rebase them."""

    def test_toc16_outside_pidok_is_dropped(self):
        b = ElfBuilder()
        b.add_text_reloc(0x10, R_PPC64_TOC16_DS, "ds45_shim_data")  # in startup
        b.add_text_reloc(0x14, R_PPC64_TOC16_HA, "ds45_shim_data")
        p = shim_pack.pack(b.build())
        self.assertNotIn(0x10, p.relocs)
        self.assertNotIn(0x14, p.relocs)
        # Unchanged from the clean build.
        self.assertEqual(
            p.relocs,
            [0x100, 0x108, 0x118, 0x120, 0x130, 0x138, 0x148])


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
            self.assertIn("#define DS45_SHIM_RELOC_COUNT 7", h)


if __name__ == "__main__":
    unittest.main()
