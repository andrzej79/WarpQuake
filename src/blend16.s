;
; blend16.s -- the 16 bpp flash blend (amiga_vid.c), for big-endian 5-6-5 and
; 5-5-5 pixels
;
; Each pixel is moved toward the blend colour c by alpha a (in 1/32ths):
;
;   out = (p * (32 - a) + c * a) / 32, per channel
;
; One multiply does all three channels: the pixel is spread over a longword,
; green in the upper word and red/blue in the lower one,
;
;   x = (p | p << 16) & mask      mask = $07E0F81F (5-6-5), $03E07C1F (5-5-5)
;
; so that every field has at least 5 free bits above it and x * 32 cannot
; carry from one field into the next.  Then (x * (32 - a) + spread(c) * a)
; >> 5, masked again, folds back into a pixel: its lower word OR its upper.
;
; Two pixels a longword, each with its own multiply; the two halves are
; interleaved so the 68060 can pair them.
;

	machine	68060

	xdef	_qgBlend16

	section	CODE,code

;------------------------------------------------------------------------------
; void qgBlend16 (const UWORD *src, UWORD *dst, ULONG pixels, const ULONG *params)
;
; pixels is even and both pointers are word aligned (longword aligned is
; faster).  params: mask, spread(c) * a, 32 - a.
;
;   a0 = src  a1 = dst  d7 = pairs left
;   d4 = mask  d5 = spread(c) * a  d6 = 32 - a
;   d0 = first pixel, spread   d2 = second pixel, spread   d1, d3 = scratch
;------------------------------------------------------------------------------

	cnop	0,4
_qgBlend16
	movem.l	d2-d7/a2,-(sp)
.args	equ	7*4+4
	move.l	.args(sp),a0
	move.l	.args+4(sp),a1
	move.l	.args+8(sp),d7
	move.l	.args+12(sp),a2
	move.l	(a2)+,d4			; mask
	move.l	(a2)+,d5			; spread(c) * a
	move.l	(a2),d6				; 32 - a
	lsr.l	#1,d7
	beq.s	.done
.pair
	move.l	(a0)+,d0			; p0:p1
	move.l	d0,d1
	move.l	d0,d2
	swap	d1				; p1:p0
	swap	d2
	move.w	d0,d2				; p1:p1
	move.w	d1,d0				; p0:p0
	and.l	d4,d0				; spread
	and.l	d4,d2
	mulu.l	d6,d0				; * (32 - a)
	mulu.l	d6,d2
	add.l	d5,d0				; + spread(c) * a
	add.l	d5,d2
	lsr.l	#5,d0				; / 32
	lsr.l	#5,d2
	and.l	d4,d0
	and.l	d4,d2
	move.l	d0,d1				; fold: lower word | upper word
	move.l	d2,d3
	swap	d1
	swap	d3
	or.w	d1,d0				; q0
	or.w	d3,d2				; q1
	swap	d0
	move.w	d2,d0				; q0:q1
	move.l	d0,(a1)+
	subq.l	#1,d7
	bne.s	.pair
.done
	movem.l	(sp)+,d2-d7/a2
	rts

	end
