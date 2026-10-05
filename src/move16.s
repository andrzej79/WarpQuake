;
; move16.s -- MOVE16 copies for the 68040/68060
;
; Warp's RTG memory is not cached and is optimised for burst writes, which a
; MOVE16 produces: one 16-byte line per instruction, read as a burst from the
; source and written as a burst to the destination, without allocating the
; destination line in the data cache.  Both addresses must be 16-byte
; aligned; the low four bits are ignored by the CPU, not faulted.
;
; Calling convention: vbcc's, arguments on the stack.
;

	machine	68060

	xdef	_qgMove16Copy
	xdef	_qgMove16Rows

	section	CODE,code

;------------------------------------------------------------------------------
; void qgMove16Copy (const void *src, void *dst, ULONG bytes)
;
; bytes is a multiple of 64 (four lines per loop pass).
;------------------------------------------------------------------------------

	cnop	0,4
_qgMove16Copy
	move.l	4(sp),a0			; src
	move.l	8(sp),a1			; dst
	move.l	12(sp),d0			; bytes
	lsr.l	#6,d0
	beq.s	.done
	subq.l	#1,d0
.loop
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
	move16	(a0)+,(a1)+
	dbf	d0,.loop
	; dbf counts 16 bits: more than 64K passes (4 MB) continue here
	sub.l	#$10000,d0
	bpl.s	.loop
.done
	rts

;------------------------------------------------------------------------------
; void qgMove16Rows (const UBYTE *src, UBYTE *dst, ULONG width, ULONG height,
;                    ULONG dstBytesPerRow)
;
; A frame into the screen: height rows of width bytes, src rows packed
; (modulo = width), dst rows dstBytesPerRow apart.  width is a multiple of 16
; and the rows of both are 16-byte aligned.
;------------------------------------------------------------------------------

	cnop	0,4
_qgMove16Rows
	movem.l	d2-d3/a2,-(sp)
	move.l	12+4(sp),a0			; src
	move.l	12+8(sp),a2			; dst row
	move.l	12+12(sp),d2			; width
	move.l	12+16(sp),d1			; height
	move.l	12+20(sp),d3			; dstBytesPerRow
	lsr.l	#4,d2				; lines per row
	subq.l	#1,d2
	subq.l	#1,d1
	bmi.s	.rdone
.row
	move.l	a2,a1
	move.l	d2,d0
.line
	move16	(a0)+,(a1)+
	dbf	d0,.line
	add.l	d3,a2
	dbf	d1,.row
.rdone
	movem.l	(sp)+,d2-d3/a2
	rts

	end
