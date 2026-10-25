/*
 * gork.lds -- for `ld -r`: gork's half of the barebox build as one object,
 * gork.o.  Its variables, data and bss alike, lie between __gork_data_start
 * and __gork_data_end, for glue.c to put back before each run; the bss is
 * zeros in the image so (a few kilobytes).  barebox's own link takes the
 * sections as .text*, .rodata* and .data*.
 */
SECTIONS
{
	.text.gork : { *(.text .text.*) }
	.rodata.gork : { *(.rodata .rodata.*) }
	.data.gork : ALIGN(16) {
		__gork_data_start = .;
		*(.data .data.*)
		*(.bss .bss.*)
		. = ALIGN(16);
		__gork_data_end = .;
	}
}
