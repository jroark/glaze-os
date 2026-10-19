/* The romfs archive, linked into its own page-aligned section. */
	.section .romfs, "a"
	.balign	4096
	.incbin	"out/romfs.img"
