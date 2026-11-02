/*
 * DS45PAD kernel shim - linker script for the flat, position-independent blob.
 *
 * The blob is linked at address 0: every absolute 64-bit word the link leaves
 * behind (the .opd descriptors' entry and TOC words, and the .toc entries that
 * hold the addresses of globals) is therefore an offset from the start of the
 * image, and tools/shim_pack.py records those offsets so the plugin can add the
 * real kernel load address to each one after it copies the image into kernel
 * memory.
 *
 * Everything the blob needs - code, function descriptors, the TOC and the data
 * object - lives inside the single image; there are no PT_LOAD gaps and no
 * dynamic section. The link keeps relocations (ld.lld --emit-relocs); we must
 * NOT define `.TOC.` here, lld sets it itself for the ELFv1 ABI.
 *
 * Licensed under GPLv2 - see LICENSE.
 */

ENTRY(shim_event)

SECTIONS
{
	. = 0;

	.text : {
		*(.text)
		*(.text.*)
	}
	. = ALIGN(8);

	/* Function descriptors: { entry, toc, env } per exported function. */
	.opd : {
		*(.opd)
	}
	. = ALIGN(8);

	/* The TOC / GOT: 8-byte absolute pointers to the blob's own globals. */
	.toc : {
		*(.got)
		*(.toc)
		*(.toc.*)
		*(.got.*)
	}
	. = ALIGN(8);

	.data : {
		*(.data)
		*(.data.*)
		*(.sdata)
		*(.sdata.*)
		*(.rodata)
		*(.rodata.*)
	}
	. = ALIGN(8);

	.bss : {
		*(.bss)
		*(.bss.*)
		*(.sbss)
		*(.sbss.*)
		*(COMMON)
	}
	. = ALIGN(8);

	_shim_end = .;

	/* Nothing below belongs in a flat kernel blob. */
	/DISCARD/ : {
		*(.comment)
		*(.note)
		*(.note.*)
		*(.eh_frame)
		*(.eh_frame_hdr)
		*(.gnu.attributes)
		*(.glink)
		*(.branch_lt)
		*(.interp)
		*(.dynamic)
		*(.dynsym)
		*(.dynstr)
		*(.hash)
		*(.gnu.hash)
	}
}
