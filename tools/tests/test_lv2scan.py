"""Tests for tools/lv2scan.py. Synthetic, hand-encoded images only."""
import pathlib
import sys

import pytest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import lv2scan as L  # noqa: E402
import synth as S    # noqa: E402

BASE = S.BASE


def merge(a: bytes, b: bytes) -> bytes:
    """Overlay two images built in disjoint regions."""
    assert len(a) == len(b)
    return bytes(x | y for x, y in zip(a, b))


def one(img):
    cands = L.find_candidates(img, BASE)
    assert len(cands) == 1
    return cands[0]


# --------------------------------------------------------------------------
# helpers and constants
# --------------------------------------------------------------------------

def test_synth_encoders_match_known_words():
    assert S.stdu_r1(-0x80) == 0xF821FF81
    assert S.ld(9, 0x1230, 2) == 0xE9221230
    assert S.lbz(0, 0xE8, 9) == 0x880900E8
    assert S.cmplwi(3, 0x054C) == 0x2803054C
    assert S.cmpwi(4, 0x0268) == 0x2C040268


def test_fnv_matches_c_vectors():
    # the same vectors as tests/test_lv2.c
    assert L.fnv1a64(b"") == 0xCBF29CE484222325
    assert L.fnv1a64(b"a") == 0xAF63DC4C8601EC8C
    assert L.fnv1a64(b"foobar") == 0x85944171F73967E8


def test_constants_match_kernel_h():
    assert L.NPP_SCAN_MAX_INSNS == 16
    assert L.NPP_SCAN_MAX_HITS == 8
    assert L.NPP_SCAN_PROLOGUE_RANGE == 0x800
    assert L.NPP_FP_LEN == 32
    assert L.NPP_INSN_LD_FROM_R2 == (0xFC1F0003, 0xE8020000)
    assert L.NPP_INSN_STDU_R1 == (0xFFFF0003, 0xF8210001)


# --------------------------------------------------------------------------
# find_toc
# --------------------------------------------------------------------------

def _qwords(size, pairs):
    img = bytearray(size)
    for off, val in pairs:
        img[off:off + 8] = val.to_bytes(8, "big")
    return bytes(img)


def test_find_toc_takes_first_pair_with_toc_in_window():
    start = BASE + 0x1000
    good = BASE + 0x8000
    img = _qwords(0x100, [
        (0x10, start), (0x18, BASE - 8),            # toc below the window
        (0x20, start), (0x28, BASE + S.WINDOW),     # toc at the bound: out
        (0x30, start), (0x38, good),                # first usable
        (0x40, start), (0x48, BASE + 0x9000),       # later: ignored
    ])
    assert L.find_toc(img, BASE, start) == good


def test_find_toc_ignores_unaligned_pairs():
    start = BASE + 0x1000
    img = _qwords(0x100, [(0x14, start), (0x1C, BASE + 0x8000)])
    assert L.find_toc(img, BASE, start) is None


def test_find_toc_none_when_absent():
    assert L.find_toc(bytes(0x100), BASE, BASE + 0x1000) is None
    # start as the very last qword: no toc follows
    img = _qwords(0x100, [(0xF8, BASE + 0x1000)])
    assert L.find_toc(img, BASE, BASE + 0x1000) is None


# --------------------------------------------------------------------------
# find_candidates
# --------------------------------------------------------------------------

def test_candidate_found():
    s = S.build_image()
    c = one(s.data)
    assert c.func_start == s.func_start
    assert c.slot_addr == s.slot == BASE + 0x8000 + 0x1230
    assert c.report_off == 0xE8
    assert c.fp_addr == s.ld_addr & ~7
    off = c.fp_addr - BASE
    assert c.fp_hash == L.fnv1a64(s.data[off:off + 32])
    assert isinstance(c.disasm, list)
    assert c.ld_addr == s.ld_addr
    assert c.report_insn_addr == s.report_addr


@pytest.mark.parametrize("missing", ["vid", "pid", "rep"])
def test_no_candidate_without_vid_pid(missing):
    kw = {"vid": 0x054C, "pid": 0x0268, "rep": 0xA1}
    kw[missing] = None
    assert L.find_candidates(S.build_image(**kw).data, BASE) == []


def test_no_candidate_with_wrong_pid():
    assert L.find_candidates(S.build_image(pid=0x09CC).data, BASE) == []


def test_compares_must_be_in_the_same_function():
    # vid/pid/report-id compares in one function, the TOC load in the next
    a = S.build_image(func_off=0x1000, opd_off=0x400)
    b = S.build_image(func_off=0x1400, opd_off=0x420, vid=None, pid=None, rep=None)
    assert len(L.find_candidates(a.data, BASE)) == 1
    assert L.find_candidates(b.data, BASE) == []


@pytest.mark.parametrize("off", [0x08, 0x0C, 0x401, 0x500, 0x7FF0])
def test_no_candidate_when_report_off_out_of_range(off):
    assert L.find_candidates(S.build_image(report_off=off).data, BASE) == []


@pytest.mark.parametrize("off", [0x10, 0x400])
def test_report_off_range_is_inclusive(off):
    assert one(S.build_image(report_off=off).data).report_off == off


def test_report_use_must_be_through_the_loaded_register():
    s = S.build_image()
    img = bytearray(s.data)
    # lbz r0, 0xE8(r9) -> lbz r0, 0xE8(r10)
    a = s.report_addr - BASE
    img[a:a + 4] = S.lbz(0, 0xE8, 10).to_bytes(4, "big")
    assert L.find_candidates(bytes(img), BASE) == []


def test_report_off_is_the_first_use_in_range():
    # an out-of-range use first (offset 0 deref), then 0x20, then 0xE8
    s = S.build_image()
    img = bytearray(s.data)
    # a lwz r3, 0x20(r9) in front of the lbz r0, 0xE8(r9)
    a = s.report_addr - BASE
    img[a:a + 8] = S.lwz(3, 0x20, 9).to_bytes(4, "big") + S.lbz(0, 0xE8, 9).to_bytes(4, "big")
    assert one(bytes(img)).report_off == 0x20


def test_no_candidate_without_function_descriptor():
    assert L.find_candidates(S.build_image(with_opd=False).data, BASE) == []


def test_no_candidate_when_toc_outside_window():
    assert L.find_candidates(S.build_image(toc_in_window=False).data, BASE) == []


def test_no_candidate_for_unaligned_or_out_of_window_slot():
    # disp 0x1234: slot = toc + 0x1234 is only 4-aligned
    assert L.find_candidates(S.build_image(ld_disp=0x1234).data, BASE) == []
    # a negative displacement that leaves the window below BASE
    assert L.find_candidates(
        S.build_image(ld_disp=-0x2000, toc_off=0x1000).data, BASE) == []


def test_negative_displacement_is_sign_extended():
    s = S.build_image(ld_disp=-0x20)
    assert one(s.data).slot_addr == BASE + 0x8000 - 0x20


def test_fp_addr_rounds_ld_address_down_to_8():
    even = one(S.build_image(pad=0).data)
    odd = one(S.build_image(pad=1).data)
    assert even.ld_addr & 4 != odd.ld_addr & 4
    assert even.fp_addr == even.ld_addr & ~7
    assert odd.fp_addr == odd.ld_addr & ~7


def test_prologue_is_the_nearest_stdu_before_the_ld():
    s = S.build_image()
    assert one(s.data).func_start == s.func_start


def test_function_ends_at_next_prologue():
    # a second stdu between the compares and the ld splits the function
    s = S.build_image()
    img = bytearray(s.data)
    a = s.ld_addr - 4 - BASE
    img[a:a + 4] = S.stdu_r1(-0x40).to_bytes(4, "big")
    assert L.find_candidates(bytes(img), BASE) == []


def test_ld_beyond_prologue_range_is_not_a_candidate():
    far = [S.NOP] * (L.NPP_SCAN_PROLOGUE_RANGE // 4)
    s = S.build_image(pad=len(far))
    assert L.find_candidates(s.data, BASE) == []


# --------------------------------------------------------------------------
# common_pattern
# --------------------------------------------------------------------------

def _pair(**kw):
    s = S.build_image(**kw)
    return s, (s.data, one(s.data))


def test_common_pattern_masks_out_displacement():
    sa, a = _pair(ld_disp=0x1230)
    sb, b = _pair(ld_disp=0x2468, toc_off=0x9000)
    pat = L.common_pattern([a, b])
    mask, value = pat[pat.toc_ld_index]
    assert mask & 0xFFFC == 0
    assert (mask & 0xFFFF0003) == 0xFFFF0003    # opcode, rT, rA, xo kept
    assert value == S.ld(9, 0, 2)


def test_common_pattern_masks_displacement_even_for_a_single_dump():
    _, a = _pair()
    pat = L.common_pattern([a])
    mask, value = pat[pat.toc_ld_index]
    assert mask == 0xFFFF0003 and value == S.ld(9, 0, 2)


def test_common_pattern_keeps_equal_words_exactly():
    _, a = _pair(ld_disp=0x1230)
    _, b = _pair(ld_disp=0x2468)
    pat = L.common_pattern([a, b])
    # the deref ld r9,0(r9) is identical in both dumps
    assert pat[pat.toc_ld_index + 1] == (0xFFFFFFFF, S.ld(9, 0, 9))


def test_common_pattern_report_off_index_and_masking():
    _, a = _pair(report_off=0xE8)
    _, b = _pair(report_off=0xF0)
    pat = L.common_pattern([a, b])
    assert pat.report_off_index == pat.toc_ld_index + 2
    mask, value = pat[pat.report_off_index]
    assert mask & 0xFFFF == 0
    assert value == S.lbz(0, 0, 9)


def test_common_pattern_keeps_report_off_when_dumps_agree():
    _, a = _pair(report_off=0xE8)
    _, b = _pair(report_off=0xE8, ld_disp=0x2468)
    pat = L.common_pattern([a, b])
    assert pat[pat.report_off_index] == (0xFFFFFFFF, S.lbz(0, 0xE8, 9))


def test_common_pattern_masks_relocatable_words():
    # bl targets and r2-relative displacements are per-kernel, not code shape
    _, a = _pair(bl_rel=0x2000)
    _, b = _pair(bl_rel=0x2008)
    pat = L.common_pattern([a, b])
    mask, value = pat[pat.toc_ld_index - 2]       # the bl, 2 words before the ld
    assert mask == 0xFC000003 and value == 0x48000001


def test_common_pattern_aligns_on_the_ld_not_on_fp_addr():
    # ld in the even half in one dump, odd half in the other
    _, a = _pair(pad=0)
    _, b = _pair(pad=1, ld_disp=0x2468)
    ca, cb = a[1], b[1]
    assert ca.ld_addr & 4 != cb.ld_addr & 4
    pat = L.common_pattern([a, b])
    mask, value = pat[pat.toc_ld_index]
    assert value == S.ld(9, 0, 2)


def test_common_pattern_rejects_dumps_with_different_layout():
    _, a = _pair()
    # move the report use one word further from the ld in dump b
    s = S.build_image(ld_disp=0x2468)
    img = bytearray(s.data)
    ra = s.report_addr - BASE
    img[ra:ra + 8] = S.NOP.to_bytes(4, "big") + S.lbz(0, 0xE8, 9).to_bytes(4, "big")
    cb = one(bytes(img))
    with pytest.raises(ValueError):
        L.common_pattern([a, (bytes(img), cb)])


def test_common_pattern_trims_to_the_c_limit():
    tail = [S.addi(5 + i, 0, 0x100 + i) for i in range(10)]
    s = S.build_image(extra_after=tail)
    pat = L.common_pattern([(s.data, one(s.data))])
    assert 1 <= len(pat) <= L.NPP_SCAN_MAX_INSNS
    assert 0 <= pat.toc_ld_index < len(pat)
    assert 0 <= pat.report_off_index < len(pat)
    assert pat[pat.toc_ld_index][1] == S.ld(9, 0, 2)
    assert pat[pat.report_off_index][1] == S.lbz(0, 0xE8, 9)


def test_common_pattern_drops_dead_edges_first():
    # complementary words after the blr share no bit: empty masks at the tail
    a = S.build_image(extra_after=[0xAAAAAAAA] * 4, ld_disp=0x1230)
    b = S.build_image(extra_after=[0x55555555] * 4, ld_disp=0x2468)
    pat = L.common_pattern([(a.data, one(a.data)), (b.data, one(b.data))])
    assert len(pat) == 13                       # 8 before, the ld, 4 after
    assert pat[-1][0] != 0 and pat[0][0] != 0
    assert pat.toc_ld_index == 8


def test_common_pattern_requires_input():
    with pytest.raises(ValueError):
        L.common_pattern([])


def test_pattern_matches_every_input_dump():
    sa, a = _pair(ld_disp=0x1230, report_off=0xE8)
    sb, b = _pair(ld_disp=0x2468, toc_off=0x9000, report_off=0xF0, pad=1)
    pat = L.common_pattern([a, b])
    for s, (img, c) in ((sa, a), (sb, b)):
        res = L.simulate_scan(img, BASE, pat)
        assert res.raw_hits == 1
        assert res.error is None
        assert (res.slot_addr, res.report_off) == (c.slot_addr, c.report_off)
        assert (res.fp_addr, res.fp_hash) == (c.fp_addr, c.fp_hash)


# --------------------------------------------------------------------------
# simulate_scan (the Python mirror of kernel_scan)
# --------------------------------------------------------------------------

def test_simulate_counts_every_raw_hit():
    # two copies of the function, same TOC and slot
    a = S.build_image(func_off=0x1000, opd_off=0x400)
    b = S.build_image(func_off=0x3000, opd_off=0x420)
    img = merge(a.data, b.data)
    pat = L.common_pattern([(a.data, one(a.data))])
    res = L.simulate_scan(img, BASE, pat)
    assert res.raw_hits == 2
    assert res.error is None and res.slot_addr == a.slot


def test_simulate_rejects_hits_that_disagree():
    a = S.build_image(func_off=0x1000, opd_off=0x400, ld_disp=0x1230)
    b = S.build_image(func_off=0x3000, opd_off=0x420, ld_disp=0x1238)
    img = merge(a.data, b.data)
    pat = L.common_pattern([(a.data, one(a.data))])
    res = L.simulate_scan(img, BASE, pat)
    assert res.raw_hits == 2
    assert res.error is not None and res.fp_addr is None


def test_simulate_ambiguous_beyond_max_hits():
    parts = []
    for i in range(L.NPP_SCAN_MAX_HITS + 1):
        parts.append(S.build_image(func_off=0x1000 + 0x200 * i,
                                   opd_off=0x400 + 0x10 * i))
    img = parts[0].data
    for p in parts[1:]:
        img = merge(img, p.data)
    pat = L.common_pattern([(parts[0].data, one(parts[0].data))])
    res = L.simulate_scan(img, BASE, pat)
    assert res.raw_hits == L.NPP_SCAN_MAX_HITS + 1
    assert res.error is not None and "ambiguous" in res.error


def test_simulate_drops_negative_report_offsets_without_counting():
    a = S.build_image(report_off=0xE8)
    # the same function with a negative report immediate elsewhere in the image
    b = S.build_image(func_off=0x3000, opd_off=0x420, report_off=-8)
    # the report immediate is wildcarded only if the dumps differed on it
    c = S.build_image(report_off=0xF0, ld_disp=0x1238)
    pat = L.common_pattern([(a.data, one(a.data)), (c.data, one(c.data))])
    res = L.simulate_scan(merge(a.data, b.data), BASE, pat)
    assert res.raw_hits == 1


def test_simulate_no_prologue_or_descriptor_is_a_miss():
    a = S.build_image(with_opd=False)
    full = S.build_image()
    pat = L.common_pattern([(full.data, one(full.data))])
    res = L.simulate_scan(a.data, BASE, pat)
    assert res.raw_hits == 1 and res.error is not None


def test_simulate_invalid_pattern():
    res = L.simulate_scan(bytes(64), BASE, L.Pattern([], 0, 0))
    assert res.raw_hits == 0 and res.error


# --------------------------------------------------------------------------
# C emitters
# --------------------------------------------------------------------------

def test_emit_c_entry_format():
    c = L.Candidate(
        fp_addr=0x800000000012C340,
        fp_hash=0x0123456789ABCDEF,
        slot_addr=0x800000000034C530,
        report_off=0xE8,
        func_start=0x8000000000100000,
        disasm=[],
    )
    assert L.emit_c_entry("DEX 4.92", c) == (
        '\t{ "DEX 4.92", 0x800000000012C340ULL, 0x0123456789ABCDEFULL, '
        '0x800000000034C530ULL, 0xE8 },'
    )


def test_emit_c_entry_escapes_the_name():
    c = L.Candidate(0x8000000000000100, 1, 0x8000000000000200, 0x10, 0, [])
    out = L.emit_c_entry('a"b\\c', c)
    assert out.startswith('\t{ "a\\"b\\\\c", ')
    with pytest.raises(ValueError):
        L.emit_c_entry("bad\nname", c)


def test_emit_c_pattern_shape():
    s = S.build_image()
    pat = L.common_pattern([(s.data, one(s.data))])
    text = L.emit_c_pattern(pat)
    assert "static const npp_insn_match_t npp_scan_insns[] = {" in text
    assert "const npp_scan_pattern_t npp_scan_pattern = {" in text
    ld_line = "{ 0xFFFF0003u, 0x%08Xu }" % S.ld(9, 0, 2)
    assert ld_line in text
    tail = "npp_scan_insns, %d, %d, %d };" % (
        len(pat), pat.toc_ld_index, pat.report_off_index)
    assert tail in text
    assert text.count("u },") == len(pat)       # one initializer per word


def test_emit_c_pattern_rejects_what_the_c_scan_rejects():
    with pytest.raises(ValueError):
        L.emit_c_pattern(L.Pattern([], 0, 0))
    too_long = L.Pattern([(1, 1)] * (L.NPP_SCAN_MAX_INSNS + 1), 0, 0)
    with pytest.raises(ValueError):
        L.emit_c_pattern(too_long)
    with pytest.raises(ValueError):
        L.emit_c_pattern(L.Pattern([(1, 1)] * 4, 4, 0))
    with pytest.raises(ValueError):
        L.emit_c_pattern(L.Pattern([(1, 1)] * 4, 0, -1))


def test_emit_c_pattern_accepts_plain_list_with_indices():
    text = L.emit_c_pattern([(0xFFFFFFFF, 0x60000000)] * 3,
                            toc_ld_index=1, report_off_index=2)
    assert "npp_scan_insns, 3, 1, 2 };" in text


# --------------------------------------------------------------------------
# CLI (a dump is exactly the 8 MiB window, so these images are full size)
# --------------------------------------------------------------------------

def big(**kw):
    return S.build_image(size=S.WINDOW, **kw)


def test_cli_two_dumps(tmp_path, capsys):
    a = big(ld_disp=0x1230, report_off=0xE8)
    b = big(ld_disp=0x2468, toc_off=0x9000, report_off=0xF0)
    pa, pb = tmp_path / "a.bin", tmp_path / "b.bin"
    pa.write_bytes(a.data)
    pb.write_bytes(b.data)
    rc = L.main([str(pa), str(pb), "--name", "FW A", "--name", "FW B"])
    out = capsys.readouterr().out
    assert rc == 0
    assert '{ "FW A", ' in out and '{ "FW B", ' in out
    assert "npp_scan_pattern" in out
    assert out.count("raw hits: 1") == 2
    # the verdict is printed before the pattern C
    assert out.index("raw hits: 1") < out.index("npp_scan_pattern")


def test_cli_single_dump_default_name(tmp_path, capsys):
    p = tmp_path / "console1.bin"
    p.write_bytes(big().data)
    assert L.main([str(p)]) == 0
    assert '{ "console1", ' in capsys.readouterr().out


def test_cli_no_candidate_is_an_error(tmp_path, capsys):
    p = tmp_path / "x.bin"
    p.write_bytes(bytes(S.WINDOW))
    assert L.main([str(p)]) == 1


def test_cli_several_candidates_need_a_pick(tmp_path, capsys):
    a = big(func_off=0x1000, opd_off=0x400)
    b = big(func_off=0x3000, opd_off=0x420, report_off=0x20)
    p = tmp_path / "m.bin"
    p.write_bytes(merge(a.data, b.data))
    assert L.main([str(p)]) == 2
    assert L.main([str(p), "--pick", "1"]) == 0


def test_cli_name_count_must_match(tmp_path):
    p = tmp_path / "x.bin"
    p.write_bytes(big().data)
    assert L.main([str(p), "--name", "a", "--name", "b"]) == 2


def test_cli_rejects_a_dump_of_the_wrong_size(tmp_path, capsys):
    p = tmp_path / "short.bin"
    p.write_bytes(S.build_image().data)             # 64 KiB
    assert L.main([str(p)]) == 2
    cap = capsys.readouterr()
    assert "short.bin" in cap.err and str(S.WINDOW) in cap.err
    assert "npp_scan_pattern" not in cap.out


def test_cli_reports_a_missing_file_without_a_traceback(tmp_path, capsys):
    rc = L.main([str(tmp_path / "nope.bin")])
    cap = capsys.readouterr()
    assert rc == 2
    assert "nope.bin" in cap.err and "Traceback" not in cap.err


def test_cli_has_no_base_option(tmp_path):
    # the plugin always scans from NPP_LV2_BASE, so the tool does too
    p = tmp_path / "x.bin"
    p.write_bytes(big().data)
    with pytest.raises(SystemExit) as e:
        L.main([str(p), "--base", "0x8000000000000000"])
    assert e.value.code == 2


def test_cli_prints_no_pattern_when_the_scan_fails(tmp_path, capsys):
    # two functions, the same report offset, different slots: the pattern
    # (displacement masked) hits both and the C scan would refuse the result
    a = big(func_off=0x1000, opd_off=0x400, ld_disp=0x1230)
    b = big(func_off=0x3000, opd_off=0x420, ld_disp=0x1238)
    p = tmp_path / "m.bin"
    p.write_bytes(merge(a.data, b.data))
    rc = L.main([str(p), "--pick", "0"])
    cap = capsys.readouterr()
    assert rc == 1
    assert "scan FAILS" in cap.out and "raw hits: 2" in cap.out
    assert "npp_scan_pattern" not in cap.out
    assert "npp_scan_insns" not in cap.out



# --------------------------------------------------------------------------
# local dumps (never committed: tools/dumps/ is git-ignored)
# --------------------------------------------------------------------------

DUMPS = sorted((TOOLS / "dumps").glob("*.bin"))


@pytest.mark.skipif(not DUMPS, reason="no tools/dumps/*.bin")
def test_runs_on_local_dumps():
    chosen = []
    for p in DUMPS:
        img = p.read_bytes()
        cands = L.find_candidates(img, BASE)
        for c in cands:
            assert (c.fp_addr & 7) == 0
            assert L.emit_c_entry(p.stem, c).startswith("\t{ ")
        if len(cands) == 1:
            chosen.append((img, cands[0]))
    if chosen:
        pat = L.common_pattern(chosen)
        assert 1 <= len(pat) <= L.NPP_SCAN_MAX_INSNS
        L.emit_c_pattern(pat)


# --------------------------------------------------------------------------
# capstone is optional
# --------------------------------------------------------------------------

def test_works_without_capstone(monkeypatch):
    monkeypatch.setattr(L, "_CS", None)
    c = one(S.build_image().data)
    assert c.disasm == []
    assert c.slot_addr == BASE + 0x8000 + 0x1230


@pytest.mark.skipif(L._CS is None, reason="capstone not installed")
def test_disasm_filled_with_capstone():
    c = one(S.build_image().data)
    assert any("ld r9" in line for line in c.disasm)


# --------------------------------------------------------------------------
# what the plugin can use (kernel_record_usable) and what C reads
# --------------------------------------------------------------------------

def _patch_report_use(s, *words):
    """Replace the report-use word (and the nop after it) of a Synth image."""
    img = bytearray(s.data)
    a = s.report_addr - BASE
    for i, w in enumerate(words):
        img[a + 4 * i:a + 4 * i + 4] = w.to_bytes(4, "big")
    return bytes(img)


def test_record_usable_mirrors_c():
    ok = BASE + 0x1000
    assert L.record_usable(ok, 0xE8)
    assert L.record_usable(ok, L.NPP_REPORT_OFF_MAX)
    assert not L.record_usable(ok, 0x21)                  # unaligned offset
    assert not L.record_usable(ok, L.NPP_REPORT_OFF_MAX + 8)
    assert not L.record_usable(ok + 4, 0xE8)              # unaligned slot
    assert not L.record_usable(BASE - 8, 0xE8)
    assert not L.record_usable(BASE + S.WINDOW - 4, 0xE8)
    assert L.record_usable(BASE + S.WINDOW - 8, 0xE8)


def test_unaligned_report_off_is_not_a_candidate():
    assert L.find_candidates(S.build_image(report_off=0x21).data, BASE) == []


def test_unaligned_use_is_skipped_and_a_later_aligned_use_is_picked():
    s = S.build_image()
    img = _patch_report_use(s, S.lbz(0, 0x21, 9), S.lbz(0, 0x28, 9))
    c = one(img)
    assert c.report_off == 0x28
    assert c.report_insn_addr == s.report_addr + 4


def test_ds_form_use_with_nonzero_low_bits_is_skipped():
    lwa = 0xE8000002 | (0 << 21) | (9 << 16) | 0xE8          # lwa r0,0xE8(r9)
    s = S.build_image()
    assert L.find_candidates(_patch_report_use(s, lwa), BASE) == []
    # an aligned use after it is still found
    img = _patch_report_use(s, lwa, S.lbz(0, 0x30, 9))
    assert one(img).report_off == 0x30
    # ld (xo 0) is accepted
    ld_use = S.ld(0, 0xE8, 9)
    assert one(_patch_report_use(s, ld_use)).report_off == 0xE8


@pytest.mark.parametrize("off", [0x21, 0x7FF8])
def test_simulate_scan_rejects_an_unusable_record(off):
    a = S.build_image(report_off=0xE8)
    b = S.build_image(report_off=0xF0, ld_disp=0x2468)
    pat = L.common_pattern([(a.data, one(a.data)), (b.data, one(b.data))])
    bad = S.build_image(report_off=off)         # matches the wildcarded pattern
    res = L.simulate_scan(bad.data, BASE, pat)
    assert res.raw_hits == 1
    assert res.error and "unusable" in res.error
    assert res.slot_addr is None


def _early_ld_image(ld_gap, **kw):
    """A function whose TOC ld is `ld_gap` words after the stdu, compares
    after it (so it still qualifies), in an otherwise empty image."""
    img = bytearray(0x10000)
    func_off = 0x1000
    words = [S.stdu_r1(-0x80)] + [S.NOP] * (ld_gap - 1) + [
        S.ld(9, 0x1230, 2), S.ld(9, 0, 9), S.lbz(0, 0xE8, 9),
        S.cmplwi(3, 0x054C), S.cmpwi(4, 0x0268), S.cmplwi(5, 0xA1), S.BLR]
    for i, w in enumerate(words):
        img[func_off + 4 * i:func_off + 4 * i + 4] = w.to_bytes(4, "big")
    img[0x400:0x408] = (BASE + func_off).to_bytes(8, "big")
    img[0x408:0x410] = (BASE + 0x8000).to_bytes(8, "big")
    return bytes(img)


@pytest.mark.parametrize("gap", [1, 3, 8])
def test_pattern_never_starts_before_the_prologue(gap):
    img = _early_ld_image(gap)
    c = one(img)
    pat = L.common_pattern([(img, c)])
    # insns[0] is at ld - toc_ld_index words: it may not precede the stdu
    assert pat.toc_ld_index <= gap
    res = L.simulate_scan(img, BASE, pat)
    assert res.error is None and res.raw_hits == 1
    assert (res.slot_addr, res.report_off) == (c.slot_addr, c.report_off)


def test_pattern_clips_to_the_nearest_dump_prologue():
    # one dump with the ld 1 word after its stdu, one with plenty of room
    early = _early_ld_image(1)
    roomy = S.build_image(ld_disp=0x2468)
    pe, pr = (early, one(early)), (roomy.data, one(roomy.data))
    # the report use is 2 words after the ld in both
    pat = L.common_pattern([pe, pr])
    assert pat.toc_ld_index == 1
    for img, c in (pe, pr):
        assert L.simulate_scan(img, BASE, pat).error is None
