/* -*- sh -*- */

/*
 * Linker script for the BASIC-OS platform: flat binary at 0x700000
 * (module header 'PCBM' first, see mod_head.h of PC BASIC)
 */

SECTIONS {
	_max_align = 32;
	. = 0x700000;

	.header : {
		KEEP(*(.header))
	}

	. = ALIGN ( _max_align );
	.text : {
		_text = .;
		*(.text)
		*(.text.*)
		_etext = .;
	}

	. = ALIGN ( _max_align );
	.rodata : {
		_rodata = .;
		*(.srodata)
		*(.srodata.*)
		*(.rodata)
		*(.rodata.*)
		_erodata = .;
	}

	. = ALIGN ( _max_align );
	.data : {
		_data = .;
		*(.sdata)
		*(.sdata.*)
		*(.data)
		*(.data.*)
		KEEP(*(SORT(.tbl.*)))
		KEEP(*(.provided))
		KEEP(*(.provided.*))
		_edata = .;
	}

	. = ALIGN ( _max_align );
	.bss : {
		_bss = .;
		*(.sbss)
		*(.sbss.*)
		*(.bss)
		*(.bss.*)
		*(COMMON)
		_ebss = .;
	}
	. = ALIGN ( 16 );
	__mod_end = .;

	.weak 0x0 : {
		_weak = .;
		*(.weak)
		*(.weak.*)
		_eweak = .;
	}
	_assert = ASSERT ( ( _weak == _eweak ), ".weak is non-zero length" );

	/DISCARD/ : {
		*(.comment)
		*(.comment.*)
		*(.rel)
		*(.rel.*)
		*(.discard)
		*(.discard.*)
		*(.sbat)
		*(.sbat.*)
		*(.note*)
		*(.eh_frame*)
	}
}
