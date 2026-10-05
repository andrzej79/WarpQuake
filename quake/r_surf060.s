;
; r_surf060.s -- the surface-cache block drawers for the 68060
;
; R_DrawSurfaceBlock8_mip0..3, replacing the C versions in r_surf.c (built
; with WQ_ASM=1).  They light one column of texture blocks into the surface
; cache: for each block, the four corner light values come from the light map
; and are interpolated down the rows and across each row, and every texel is
; replaced by colormap[(light & 0xFF00) + texel].  Integer only, so they match
; the C exactly; -crc checks it.
;
; Where the time went in the C (hardware profile, 2026-10-04): ~17 cycles a
; texel.  vbcc's inner loop was 10 instructions, reloading vid.colormap and
; masking the light and the texel separately for every texel.  Here a texel
; is 4 instructions, all in registers:
;
;   move.w  d2,d1        light's bits 8-15 land in d1 bits 8-15 ...
;   move.b  -(a5),d1     ... and the texel in bits 0-7: d1 = (light & 0xFF00)
;                        + texel, given d1's upper word is kept zero
;   add.l   d3,d2
;   move.b  (a1,d1.l),-(a4)
;
; Rows are drawn right to left, as the C does (it starts from the right edge's
; light), fully unrolled and software pipelined (see SurfBlock).
;
; Calling convention: vbcc's, no arguments (everything is in r_surf.c's
; globals, which id kept non-static for their x86 version of this file).
;

	machine	68060

	xref	_pbasesource		; unsigned char *: the block column's first texel
	xref	_prowdestbase		; void *: its first surface-cache byte
	xref	_r_lightptr		; unsigned *: light map, at the column's top-left
	xref	_r_lightwidth		; int: light map row length
	xref	_r_numvblocks		; int: blocks down the surface
	xref	_sourcetstep		; int: texture row length
	xref	_surfrowbytes		; int: surface cache row length
	xref	_r_sourcemax		; unsigned char *: end of the texture ...
	xref	_r_stepback		; int: ... and its size, to wrap around
	xref	_vid			; viddef_t; colormap at VID_COLORMAP

	xdef	_R_DrawSurfaceBlock8_mip0
	xdef	_R_DrawSurfaceBlock8_mip1
	xdef	_R_DrawSurfaceBlock8_mip2
	xdef	_R_DrawSurfaceBlock8_mip3

VID_COLORMAP	equ	4		; offsetof (viddef_t, colormap), vid.h

	section	CODE,code		; vbcc's name: one hunk with the C, the profiler's map covers it

;------------------------------------------------------------------------------
; SurfBlock size, shift: one mip level's drawer.  size = 16 >> mip is the
; block's width and height in texels, shift = 4 - mip divides by it.
;
;   a0 = psource (block row start)    a2 = prowdest (block row start)
;   a1 = colormap                     a3 = r_lightptr
;   a4 = dest, a5 = source: one row, walking right to left
;   a6 = rows left in the block       v_vblocks = blocks left in the column
;   d1, d0 = colormap indexes of two texels in flight (upper words 0)
;   d2 = light   d3 = lightstep (across a row)
;   d4 = lightleft   d5 = lightright   d6, d7 = their steps down the rows
;
; A row is software pipelined over the two index registers: a texel's index
; is built (Index) two texels before it is used (Store), so the 68060 never
; uses a register as an index right after loading it - that stall was most
; of the time in the first version (hardware profile, 2026-10-04).
;------------------------------------------------------------------------------

; Index d: the next texel's colormap index into d, light one step on.
; Ordered for the two pipelines: the MOVE.W pairs with the ADD (which
; changes d2 only after the MOVE.W has read it), the texel load with the
; Store before it.
Index	macro
	move.w	d2,\1				; light's bits 8-15 ...
	add.l	d3,d2
	move.b	-(a5),\1			; ... and the texel: (light & 0xFF00) + texel
	endm

; Store d: its lit texel into the surface cache
Store	macro
	move.b	(a1,\1.l),-(a4)
	endm

SurfBlock	macro
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	_pbasesource,a0
	move.l	_prowdestbase,a2
	move.l	_vid+VID_COLORMAP,a1
	move.l	_r_lightptr,a3
	moveq	#0,d0
	moveq	#0,d1
	move.l	_r_numvblocks,v_vblocks
	ble	.done\@

.vblock\@
	; the block's corner lights; the steps down its edges are unsigned in
	; the C (unsigned light map minus int), so a logical shift
	move.l	(a3),d4				; lightleft
	move.l	4(a3),d5			; lightright
	move.l	_r_lightwidth,d6
	lsl.l	#2,d6
	add.l	d6,a3				; r_lightptr += r_lightwidth
	move.l	(a3),d6
	sub.l	d4,d6
	lsr.l	#\2,d6				; lightleftstep
	move.l	4(a3),d7
	sub.l	d5,d7
	lsr.l	#\2,d7				; lightrightstep
	move.w	#\1,a6

.row\@
	move.l	d4,d3
	sub.l	d5,d3
	asr.l	#\2,d3				; lightstep = (lightleft - lightright) >> shift
	move.l	d5,d2				; light = lightright
	lea	\1(a0),a5
	lea	\1(a2),a4
	Index	d1				; texels size-1 and size-2 in flight
	Index	d0
	rept	(\1/2)-1
	Store	d1
	Index	d1
	Store	d0
	Index	d0
	endr
	Store	d1
	Store	d0
	add.l	_sourcetstep,a0
	add.l	d7,d5
	add.l	d6,d4
	add.l	_surfrowbytes,a2
	subq.w	#1,a6
	tst.w	a6
	bne	.row\@

	; wrap to the texture's top when the block column runs off its bottom
	cmp.l	_r_sourcemax,a0
	bcs.s	.inside\@
	sub.l	_r_stepback,a0
.inside\@
	subq.l	#1,v_vblocks
	bne	.vblock\@

	move.l	a3,_r_lightptr			; as the C leaves it
.done\@
	movem.l	(sp)+,d2-d7/a2-a6
	rts
	endm

	cnop	0,4
_R_DrawSurfaceBlock8_mip0
	SurfBlock	16,4

	cnop	0,4
_R_DrawSurfaceBlock8_mip1
	SurfBlock	8,3

	cnop	0,4
_R_DrawSurfaceBlock8_mip2
	SurfBlock	4,2

	cnop	0,4
_R_DrawSurfaceBlock8_mip3
	SurfBlock	2,1


	section	BSS,bss

v_vblocks	ds.l	1			; blocks left in the column

	end
