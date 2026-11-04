#!/usr/bin/env python3
"""Turn the linked kernel-shim ELF into the C header the plugin embeds.

    python -I tools/shim_pack.py shim/shim.elf src/shim_blob.h

The blob is a flat, position-independent PPC64 (ELFv1, 64-bit, big-endian)
image linked at address 0. The plugin copies it into a kernel allocation at
some run-time base B and then, for every offset in the emitted relocs list,
adds B to the 64-bit word there. This script works out that relocs list and
the handful of symbol offsets the plugin needs, and refuses anything that
cannot be loaded that way.

Relocation handling (verified against the real clang-15 / ld.lld build, see
.superpowers/sdd/.../task-5-report.md):

  R_PPC64_ADDR64  absolute 64-bit word          -> REBASE (into relocs list)
  R_PPC64_TOC     the .TOC. base word of each    -> REBASE (into relocs list)
                  .opd function descriptor; it
                  is an ABSOLUTE address, so it
                  must get the load base added
  R_PPC64_REL24   PC-relative branch             -> RETARGETED to code, then
                                                    not rebased (position
                                                    independent); see below
  R_PPC64_TOC16*  TOC-relative (offset from r2)  -> drop (position independent)

REL24 and the ELFv1 function descriptor: on ELFv1 a C function's symbol is the
address of its .opd DESCRIPTOR, not of its code, and ld.lld does not redirect a
REL24 against such a symbol to the code entry - the linked `bl` lands on the
descriptor (whose first bytes are data, e.g. 0 = an illegal instruction). So
for every REL24 whose target (symbol value + addend) falls inside .opd, this
script reads the descriptor's first word (the code entry; the image links at 0
so the link-time value is the offset) and rewrites the branch's LI field to
reach that code instead. A REL24 already aimed at code is left alone. After
that, any REL24 that still lands in .opd is a hard error.

  anything else, a relocation against an undefined symbol, or a TOC/data
  reference inside shim_pid_ok (which the kernel calls with ITS OWN r2, so the
  blob's TOC is not in r2 there) -> hard error, non-zero exit, no header.

NOTE on R_PPC64_TOC: the task brief's one-liner says to DROP it as "position
independent". That is a misreading - the .opd TOC word holds the absolute
address of the blob's TOC base, so dropping it would leave every descriptor's
r2 pointing at link-time address 0 once the blob is relocated. Task 4 found
this empirically and the brief's own rule ("any relocation that would need a
load base other than the ADDR64 list is an error") agrees: it needs the load
base, so it is rebased. See the task report for the evidence.

shim_output (the DS3-route output hook) is entered by a plain `bl` from the
kernel's code, not through a descriptor; it still has an .opd descriptor so the
code entry is found the same way as shim_pid_ok's, and it is exported as
DS45_SHIM_OFF_OUTPUT. Its stub adopts the blob TOC from a literal word in .text
(`.quad .TOC.@tocbase`, an aligned R_PPC64_TOC word): the generic absolute-word
rule above rebases it with the descriptors' words, wherever it sits.

Standard library only; it never imports anything from the directory of the ELF
it is handed.
"""

import os
import struct
import sys

# ELF / machine identity.
ET_EXEC = 2
EM_PPC64 = 21
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_RELA = 4
SHT_NOBITS = 8
SHF_ALLOC = 0x2
SHN_UNDEF = 0

# PowerPC64 relocation types.
R_PPC64_ADDR64 = 38
R_PPC64_TOC = 51
R_PPC64_REL24 = 10
STT_SECTION = 3
SHF_EXECINSTR = 0x4

# Absolute 64-bit words that take the load base.
_REBASE = frozenset((R_PPC64_ADDR64, R_PPC64_TOC))

# PC-relative: branches and relative data references.
_PCREL = frozenset((
    10,   # REL24
    11,   # REL14
    12,   # REL14_BRTAKEN
    13,   # REL14_BRNTAKEN
    26,   # REL32
    44,   # REL64
    249,  # REL16
    250,  # REL16_LO
    251,  # REL16_HI
    252,  # REL16_HA
))

# TOC-relative: a signed offset from r2, invariant under a load-base change.
_TOCREL = frozenset((
    47,   # TOC16
    48,   # TOC16_LO
    49,   # TOC16_HI
    50,   # TOC16_HA
    63,   # TOC16_DS
    64,   # TOC16_LO_DS
))

_DROP = _PCREL | _TOCREL

# What may never appear inside shim_pid_ok's code: anything that uses the blob
# TOC base (rebased words, TOC-relative loads) or an absolute data address.
_TOC_OR_DATA = _REBASE | _TOCREL


class PackError(Exception):
    """A reason the ELF cannot be turned into a loadable blob."""


class Packed(object):
    def __init__(self, blob, relocs, off_data, off_event, off_enable,
                 off_input, off_pid_ok, off_output):
        self.blob = blob
        self.blob_len = len(blob)
        self.relocs = relocs
        self.reloc_count = len(relocs)
        self.off_data = off_data
        self.off_event = off_event
        self.off_enable = off_enable
        self.off_input = off_input
        self.off_pid_ok = off_pid_ok
        self.off_output = off_output


def _cstr(buf, off):
    end = buf.find(b"\x00", off)
    if end < 0:
        end = len(buf)
    return buf[off:end].decode("ascii", "replace")


def pack(data):
    """Parse the linked ELF and return a Packed, or raise PackError."""
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise PackError("not an ELF file")
    if data[4] != 2:
        raise PackError("not ELF64 (the kernel blob needs 64-bit pointers)")
    if data[5] != 2:
        raise PackError("not big-endian")

    (e_type, e_machine, _e_version, _e_entry, _e_phoff, e_shoff, _e_flags,
     _e_ehsize, _e_phentsize, _e_phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack(">HHIQQQIHHHHHH", data[16:64])

    if e_machine != EM_PPC64:
        raise PackError("not PPC64 (e_machine=%d)" % e_machine)
    if e_type != ET_EXEC:
        raise PackError("expected a linked executable (e_type=%d)" % e_type)
    if e_shoff == 0 or e_shentsize != 64 or e_shnum == 0:
        raise PackError("no usable section header table")

    sects = []
    for i in range(e_shnum):
        o = e_shoff + i * 64
        if o + 64 > len(data):
            raise PackError("section header table runs past end of file")
        f = struct.unpack(">IIQQQQIIQQ", data[o:o + 64])
        sects.append(dict(name=f[0], type=f[1], flags=f[2], addr=f[3],
                          offset=f[4], size=f[5], link=f[6], info=f[7]))

    shstr = b""
    if e_shstrndx < len(sects):
        s = sects[e_shstrndx]
        shstr = data[s["offset"]:s["offset"] + s["size"]]
    for s in sects:
        s["sname"] = _cstr(shstr, s["name"]) if shstr else ""

    # --- symbols --------------------------------------------------------
    symtab = next((s for s in sects if s["type"] == SHT_SYMTAB), None)
    if symtab is None:
        raise PackError("no symbol table")
    if symtab["link"] >= len(sects):
        raise PackError("symbol table has no string table")
    strs_sec = sects[symtab["link"]]
    strs = data[strs_sec["offset"]:strs_sec["offset"] + strs_sec["size"]]

    syms = []
    for i in range(symtab["size"] // 24):
        o = symtab["offset"] + i * 24
        st_name, st_info, _other, st_shndx, st_value, st_size = struct.unpack(
            ">IBBHQQ", data[o:o + 24])
        syms.append(dict(name=_cstr(strs, st_name), shndx=st_shndx,
                         value=st_value, size=st_size, typ=st_info & 0xF))

    # --- image: every SHF_ALLOC section, laid out at its virtual address -
    image_len = 0
    for s in sects:
        if s["flags"] & SHF_ALLOC:
            image_len = max(image_len, s["addr"] + s["size"])
    image_len = (image_len + 7) & ~7            # DS45_SHIM_BLOB_LEN % 8 == 0
    image = bytearray(image_len)
    for s in sects:
        if (s["flags"] & SHF_ALLOC) and s["type"] == SHT_PROGBITS:
            image[s["addr"]:s["addr"] + s["size"]] = \
                data[s["offset"]:s["offset"] + s["size"]]
        # SHT_NOBITS (.bss) stays zero, which is what the plugin expects.

    # --- relocations ----------------------------------------------------
    rebase = []
    all_relocs = []
    rel24 = []          # (branch offset, target = symbol value + addend)
    for s in sects:
        if s["type"] != SHT_RELA:
            continue
        for i in range(s["size"] // 24):
            o = s["offset"] + i * 24
            r_offset, r_info, r_add = struct.unpack(">QQq", data[o:o + 24])
            sym_i = r_info >> 32
            rtype = r_info & 0xFFFFFFFF
            all_relocs.append((r_offset, rtype))

            if rtype == R_PPC64_REL24:
                base = 0
                if 0 < sym_i < len(syms):
                    sy = syms[sym_i]
                    base = sy["value"]
                    if (sy["typ"] == STT_SECTION and sy["shndx"] != SHN_UNDEF
                            and sy["shndx"] < len(sects)):
                        base = sects[sy["shndx"]]["addr"]
                rel24.append((r_offset, base + r_add))

            if sym_i != 0:
                if sym_i >= len(syms):
                    raise PackError("relocation symbol index %d out of range"
                                    % sym_i)
                if syms[sym_i]["shndx"] == SHN_UNDEF:
                    raise PackError(
                        "relocation against undefined symbol %r - the blob "
                        "must be fully self-contained" % syms[sym_i]["name"])

            if rtype in _REBASE:
                if r_offset % 8 != 0 or r_offset + 8 > image_len:
                    raise PackError(
                        "absolute relocation at 0x%x is not an aligned word "
                        "inside the image" % r_offset)
                rebase.append(r_offset)
            elif rtype in _DROP:
                pass
            else:
                raise PackError(
                    "unsupported relocation type %d at 0x%x - it may need a "
                    "load base this packer does not handle" % (rtype, r_offset))

    # --- REL24 -> code --------------------------------------------------
    opd = next((s for s in sects if s["sname"] == ".opd"), None)
    exec_secs = [s for s in sects
                 if (s["flags"] & SHF_ALLOC) and (s["flags"] & SHF_EXECINSTR)]

    def in_opd(addr):
        return opd is not None and opd["addr"] <= addr < opd["addr"] + opd["size"]

    def in_text(addr):
        return any(s["addr"] <= addr < s["addr"] + s["size"] for s in exec_secs)

    for (o, target) in rel24:
        if o % 4 != 0 or o + 4 > image_len:
            raise PackError("REL24 at 0x%x is not an aligned instruction "
                            "inside the image" % o)
        if not in_opd(target):
            continue                          # already aimed at code
        if target + 8 > image_len:
            raise PackError("REL24 at 0x%x targets a descriptor outside the "
                            "image" % o)
        code = struct.unpack(">Q", bytes(image[target:target + 8]))[0]
        if not in_text(code) or code % 4 != 0:
            raise PackError(
                "REL24 at 0x%x targets descriptor 0x%x whose entry 0x%x is "
                "not in an executable section" % (o, target, code))
        delta = code - o
        if not (-0x2000000 <= delta < 0x2000000):
            raise PackError("REL24 at 0x%x cannot reach code 0x%x (distance "
                            "%d does not fit 26 bits)" % (o, code, delta))
        word = struct.unpack(">I", bytes(image[o:o + 4]))[0]
        if (word >> 26) != 18 or (word & 2):
            raise PackError("REL24 at 0x%x is not a relative b/bl "
                            "instruction (0x%08x)" % (o, word))
        word = (word & 0xFC000003) | (delta & 0x03FFFFFC)
        image[o:o + 4] = struct.pack(">I", word)

    # Guard: after retargeting, no branch named by a REL24 may still land in
    # .opd (it would execute descriptor data).
    for (o, _target) in rel24:
        word = struct.unpack(">I", bytes(image[o:o + 4]))[0]
        li = word & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        if in_opd(o + li):
            raise PackError("REL24 at 0x%x still branches into .opd (0x%x)"
                            % (o, o + li))

    # --- exported offsets ----------------------------------------------
    def find(name):
        for sym in syms:
            if sym["name"] == name and sym["shndx"] != SHN_UNDEF:
                return sym
        raise PackError("missing required symbol %r" % name)

    off_event = find("shim_event")["value"]          # .opd descriptor address
    off_enable = find("shim_enable")["value"]        # .opd descriptor address
    off_input = find("shim_input")["value"]          # .opd descriptor address
    off_data = find("ds45_shim_data")["value"]       # data object address
    pidok = find("shim_pid_ok")

    desc = pidok["value"]                            # .opd descriptor address
    if desc + 8 > len(image):
        raise PackError("shim_pid_ok descriptor is outside the image")
    off_pid_ok = struct.unpack(">Q", bytes(image[desc:desc + 8]))[0]
    pid_size = pidok["size"]
    if off_pid_ok + pid_size > len(image):
        raise PackError("shim_pid_ok code is outside the image")

    # shim_pid_ok runs on the kernel's r2, so its code may not touch the TOC
    # or any absolute data address.
    for (roff, rtype) in all_relocs:
        if rtype in _TOC_OR_DATA and off_pid_ok <= roff < off_pid_ok + pid_size:
            raise PackError(
                "shim_pid_ok uses the TOC/data (relocation type %d at 0x%x); "
                "it must run with the kernel's r2" % (rtype, roff))

    # shim_output is entered by a plain `bl` from the kernel, not through its
    # descriptor; the descriptor exists only so its code entry can be read here,
    # exactly as for shim_pid_ok. The stub keeps its blob-TOC literal in .text
    # (an aligned R_PPC64_TOC word); the relocation loop above already rebases it
    # like any other absolute word.
    output = find("shim_output")
    desc = output["value"]                           # .opd descriptor address
    if desc + 8 > len(image):
        raise PackError("shim_output descriptor is outside the image")
    off_output = struct.unpack(">Q", bytes(image[desc:desc + 8]))[0]
    if not in_text(off_output) or off_output % 4 != 0:
        raise PackError(
            "shim_output descriptor 0x%x holds entry 0x%x, which is not an "
            "aligned address in an executable section" % (desc, off_output))

    return Packed(bytes(image), sorted(set(rebase)), off_data, off_event,
                  off_enable, off_input, off_pid_ok, off_output)


def render_header(packed, source):
    out = []
    w = out.append
    w("/*")
    w(" * DS45PAD kernel shim blob - GENERATED by tools/shim_pack.py from %s."
      % source)
    w(" *")
    w(" * Do NOT edit and do NOT commit this file: every VSH plugin build")
    w(" * regenerates it from the freshly linked shim ELF.")
    w(" */")
    w("#ifndef DS45_SHIM_BLOB_H")
    w("#define DS45_SHIM_BLOB_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("#define DS45_SHIM_BLOB_LEN %d" % packed.blob_len)
    w("static const uint8_t  ds45_shim_blob[] = {")
    row = []
    for b in packed.blob:
        row.append("0x%02x," % b)
        if len(row) == 12:
            w("\t" + " ".join(row))
            row = []
    if row:
        w("\t" + " ".join(row))
    w("};")
    w("")
    w("#define DS45_SHIM_RELOC_COUNT %d" % packed.reloc_count)
    w("static const uint32_t ds45_shim_relocs[] = {")
    row = []
    for off in packed.relocs:
        row.append("0x%x," % off)
        if len(row) == 8:
            w("\t" + " ".join(row))
            row = []
    if row:
        w("\t" + " ".join(row))
    w("};")
    w("")
    w("#define DS45_SHIM_OFF_DATA 0x%x /* ds45_shim_data_t */" % packed.off_data)
    w("#define DS45_SHIM_OFF_EVENT 0x%x /* descriptor of shim_event */"
      % packed.off_event)
    w("#define DS45_SHIM_OFF_ENABLE 0x%x /* descriptor of shim_enable */"
      % packed.off_enable)
    w("#define DS45_SHIM_OFF_INPUT 0x%x /* descriptor of shim_input */"
      % packed.off_input)
    w("#define DS45_SHIM_OFF_PID_OK 0x%x /* entry (code) of shim_pid_ok */"
      % packed.off_pid_ok)
    w("#define DS45_SHIM_OFF_OUTPUT 0x%x /* entry (code) of shim_output, reached by bl */"
      % packed.off_output)
    w("")
    w("#endif /* DS45_SHIM_BLOB_H */")
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: shim_pack.py <in.elf> <out.h>\n")
        return 2
    in_path, out_path = argv
    try:
        with open(in_path, "rb") as f:
            data = f.read()
        packed = pack(data)
    except PackError as e:
        sys.stderr.write("shim_pack: error: %s\n" % e)
        return 1
    except OSError as e:
        sys.stderr.write("shim_pack: %s\n" % e)
        return 1
    header = render_header(packed, os.path.basename(in_path))
    with open(out_path, "w", newline="\n") as f:
        f.write(header)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
