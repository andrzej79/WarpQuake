;
; r_dlight060.s -- dynamic lights into a surface's light map, for the 68060
;
; R_AddDynamicLights, replacing the C in r_surf.c (built with WQ_ASM=1): each
; dynamic light (muzzle flash, rocket, explosion) that touches the surface
; being cached adds light to the blocklights[] samples near it.  The C did
; the per-sample work in float - a float-to-int truncation for each axis
; distance, the distance back to float for the compare, and the sample
; converted to float and back for the add: ~0.3 ms a frame (hardware
; profile, 2026-10-05).
;
; Per light, the float part is vbcc's code for the C, operation for
; operation (rad and minlight kept in extended, impact[] and local[] stored
; as floats).  Per sample it is integer, and the same numbers:
;
; - sd = (int)(local[0] - s*16): local[0] - 16s is exact in extended, and
;   its truncation is floor(local[0]) - 16s when it is >= 0, ceil(local[0])
;   - 16s when it is not; likewise td.
; - dist < minlight, with dist an integer: dist < ceil(minlight).
; - blocklights += (rad - dist) * 256: (rad - dist) * 256 = R - 256 dist with
;   R = 256 rad, exact, so its truncation is floor(R) - 256 dist; the C's
;   extended add of the sample (< 2^20) can round only below 2^-44, which
;   changes the truncated sum only when R's fraction is within 2^-44 of 1.
;   A light with such an R takes a copy of the C's float loop.
;
; -crc checks it (the demos have muzzle flashes, rockets and explosions).
;
; Calling convention: vbcc's, no arguments; d0-d1/a0-a1/fp0-fp1 scratch.
;

	machine	68060

	xref	_r_drawsurf			; drawsurf_t: surf at DS_SURF
	xref	_cl_dlights			; dlight_t [32]
	xref	_blocklights		; unsigned [18*18]

	xdef	_R_AddDynamicLights

DS_SURF		equ	8			; offsetof (drawsurf_t, surf)
; msurface_t (model.h)
MS_DLIGHTBITS	equ	8
MS_PLANE		equ	12
MS_TEXMINS		equ	44		; short [2]
MS_EXTENTS		equ	48		; short [2]
MS_TEXINFO		equ	52
; dlight_t (client.h), 32 bytes
DL_RADIUS	equ	12
DL_MINLIGHT	equ	24
DL_SIZE		equ	32
PL_DIST		equ	12			; mplane_t

; the frame (a6)
F_IMPACT	equ	0			; float [3]
F_LOCAL0	equ	12			; float
F_LOCAL1	equ	16
F_SMAX		equ	20
F_TMAX		equ	24
F_FL1		equ	28			; floor and ceil of local[1]
F_CL1		equ	32
F_SD		equ	36			; int [18]: |sd| per s, for the current light
FRAME		equ	36+18*4

	section	CODE,code

;------------------------------------------------------------------------------
; FloorCeil float-in-memory: \2 = floor, \3 = ceil of the float at \1.
; Uses fp0, fp1.
;------------------------------------------------------------------------------
FloorCeil	macro
	fmove.s	\1,fp0
	fintrz.x	fp0,fp1
	fmove.l	fp1,\2
	move.l	\2,\3
	fcmp.x	fp1,fp0
	fbeq	.fc\@
	fbgt	.up\@
	subq.l	#1,\2				; below its truncation: negative
	bra.s	.fc\@
.up\@
	addq.l	#1,\3
.fc\@
	endm

	cnop	0,4
_R_AddDynamicLights
	movem.l	d2-d7/a2-a6,-(sp)
	fmovem.x	fp2-fp5,-(sp)
	lea	-FRAME(sp),sp
	move.l	sp,a6
	fmove.s	#$43800000,fp5		; 256.0
	move.l	_r_drawsurf+DS_SURF,a5
	move.w	MS_EXTENTS(a5),d0
	ext.l	d0
	asr.l	#4,d0
	addq.l	#1,d0
	move.l	d0,F_SMAX(a6)
	move.w	MS_EXTENTS+2(a5),d0
	ext.l	d0
	asr.l	#4,d0
	addq.l	#1,d0
	move.l	d0,F_TMAX(a6)
	move.l	MS_TEXINFO(a5),a4
	moveq	#0,d5				; lnum

.light
	move.l	MS_DLIGHTBITS(a5),d0
	btst	d5,d0
	beq	.next				; not lit by this light

	; rad = radius - fabs (dist), dist = origin . normal - plane dist
	move.l	d5,d0
	lsl.l	#5,d0
	lea	_cl_dlights,a0
	add.l	d0,a0
	move.l	MS_PLANE(a5),a1
	fmove.s	DL_RADIUS(a0),fp3
	fmove.s	(a1),fp0
	fmul.s	(a0),fp0
	fmove.s	4(a1),fp1
	fmul.s	4(a0),fp1
	fadd.x	fp1,fp0
	fmove.s	8(a1),fp1
	fmul.s	8(a0),fp1
	fadd.x	fp1,fp0
	fmove.x	fp0,fp2
	fsub.s	PL_DIST(a1),fp2		; dist
	fmove.x	fp2,fp0
	fabs.x	fp0
	fmove.x	fp3,fp1
	fneg.x	fp0
	fadd.x	fp1,fp0
	fmove.x	fp0,fp3				; rad
	fmove.s	DL_MINLIGHT(a0),fp4
	fcmp.x	fp3,fp4
	fbgt	.next				; rad < minlight
	fneg.x	fp4
	fadd.x	fp3,fp4				; minlight = rad - minlight

	; impact[i] = origin[i] - normal[i] * dist, stored as floats
	fmove.x	fp2,fp1
	fmul.s	(a1),fp1
	fneg.x	fp1
	fadd.s	(a0),fp1
	fmove.s	fp1,F_IMPACT(a6)
	fmove.x	fp2,fp1
	fmul.s	4(a1),fp1
	fneg.x	fp1
	fadd.s	4(a0),fp1
	fmove.s	fp1,F_IMPACT+4(a6)
	fmove.x	fp2,fp1
	fmul.s	8(a1),fp1
	fneg.x	fp1
	fadd.s	8(a0),fp1
	fmove.s	fp1,F_IMPACT+8(a6)

	; local[0..1] = impact . vecs[i] + vecs[i][3] - texturemins[i]
	fmove.s	F_IMPACT(a6),fp1
	fmul.s	(a4),fp1
	fmove.s	F_IMPACT+4(a6),fp0
	fmul.s	4(a4),fp0
	fadd.x	fp0,fp1
	fmove.s	F_IMPACT+8(a6),fp0
	fmul.s	8(a4),fp0
	fadd.x	fp0,fp1
	fadd.s	12(a4),fp1
	fmove.s	fp1,F_LOCAL0(a6)
	fmove.s	F_IMPACT(a6),fp1
	fmul.s	16(a4),fp1
	fmove.s	F_IMPACT+4(a6),fp0
	fmul.s	20(a4),fp0
	fadd.x	fp0,fp1
	fmove.s	F_IMPACT+8(a6),fp0
	fmul.s	24(a4),fp0
	fadd.x	fp0,fp1
	fadd.s	28(a4),fp1
	fmove.s	fp1,F_LOCAL1(a6)
	fmove.w	MS_TEXMINS(a5),fp1
	fmove.s	F_LOCAL0(a6),fp0
	fsub.x	fp1,fp0
	fmove.s	fp0,F_LOCAL0(a6)
	fmove.w	MS_TEXMINS+2(a5),fp1
	fmove.s	F_LOCAL1(a6),fp0
	fsub.x	fp1,fp0
	fmove.s	fp0,F_LOCAL1(a6)

	; R = rad * 256 (exact); a fraction within 2^-44 of 1: the float loop
	fmove.x	fp3,fp0
	fmul.x	fp5,fp0
	fintrz.x	fp0,fp1
	fmove.l	fp1,d7				; floor (R): R >= 0
	fsub.x	fp1,fp0
	fcmp.d	#$3feffffffffffe00,fp0	; 1 - 2^-44
	fbgt	.slowlight

	; ceil (minlight): minlight >= 0
	fintrz.x	fp4,fp1
	fmove.l	fp1,d6
	fcmp.x	fp1,fp4
	fble	.cmok
	addq.l	#1,d6
.cmok

	; |sd| for every s, into F_SD
	FloorCeil	F_LOCAL0(a6),d2,d3
	move.l	F_SMAX(a6),d4
	lea	F_SD(a6),a0
	moveq	#0,d1				; 16 s
.sdloop
	move.l	d2,d0
	cmp.l	d1,d2
	bge.s	.sdpos
	move.l	d3,d0				; local[0] - 16s < 0: from the ceiling
.sdpos
	sub.l	d1,d0
	bge.s	.sdabs
	neg.l	d0
.sdabs
	move.l	d0,(a0)+
	add.l	#16,d1
	subq.l	#1,d4
	bne.s	.sdloop

	; the samples, row by row
	FloorCeil	F_LOCAL1(a6),d2,d3
	move.l	d2,F_FL1(a6)
	move.l	d3,F_CL1(a6)
	lea	_blocklights,a1
	moveq	#0,d1				; 16 t
	move.l	F_TMAX(a6),a3
.trow
	; d4 = |td|, d3 = |td| >> 1
	move.l	F_FL1(a6),d4
	cmp.l	d1,d4
	bge.s	.tdpos
	move.l	F_CL1(a6),d4		; local[1] - 16t < 0: from the ceiling
.tdpos
	sub.l	d1,d4
	bge.s	.tdabs
	neg.l	d4
.tdabs
	move.l	d4,d3
	asr.l	#1,d3
	lea	F_SD(a6),a0
	move.l	F_SMAX(a6),a2
.sample
	; dist = sd > td ? sd + (td >> 1) : td + (sd >> 1)
	move.l	(a0)+,d0			; |sd|
	cmp.l	d0,d4
	bge.s	.tbig
	add.l	d3,d0
	bra.s	.dist
.tbig
	move.l	d0,d2
	asr.l	#1,d2
	move.l	d4,d0
	add.l	d2,d0
.dist
	; if (dist < minlight) blocklights[] += floor (R) - 256 dist
	cmp.l	d6,d0
	bge.s	.unlit
	lsl.l	#8,d0
	move.l	d7,d2
	sub.l	d0,d2
	add.l	d2,(a1)
.unlit
	addq.l	#4,a1
	subq.l	#1,a2
	tst.l	a2
	bne.s	.sample
	add.l	#16,d1
	subq.l	#1,a3
	tst.l	a3
	bne.s	.trow

.next
	addq.l	#1,d5
	cmp.l	#32,d5
	blt	.light

	lea	FRAME(sp),sp
	fmovem.x	(sp)+,fp2-fp5
	movem.l	(sp)+,d2-d7/a2-a6
	rts

	; The C's float loop (vbcc's code), for a light whose R has a fraction
	; within 2^-44 of 1.  fp3 = rad, fp4 = minlight, fp5 = 256.
.slowlight
	lea	_blocklights,a1
	moveq	#0,d4				; t
.sl_t
	move.l	d4,d0
	lsl.l	#4,d0
	fmove.l	d0,fp1
	fneg.x	fp1
	fadd.s	F_LOCAL1(a6),fp1
	fintrz.x	fp1,fp1
	fmove.l	fp1,d1				; td
	tst.l	d1				; (FMOVE does not set the integer flags)
	bge.s	.sl_tp
	neg.l	d1
.sl_tp
	moveq	#0,d3				; s
.sl_s
	move.l	d3,d0
	lsl.l	#4,d0
	fmove.l	d0,fp1
	fneg.x	fp1
	fadd.s	F_LOCAL0(a6),fp1
	fintrz.x	fp1,fp1
	fmove.l	fp1,d2				; sd
	tst.l	d2				; (FMOVE does not set the integer flags)
	bge.s	.sl_sp
	neg.l	d2
.sl_sp
	cmp.l	d2,d1
	bge.s	.sl_else
	move.l	d1,d0
	asr.l	#1,d0
	add.l	d2,d0
	fmove.l	d0,fp2
	bra.s	.sl_cmp
.sl_else
	move.l	d2,d0
	asr.l	#1,d0
	add.l	d1,d0
	fmove.l	d0,fp2
.sl_cmp
	fcmp.x	fp2,fp4
	fble	.sl_next
	move.l	d4,d0
	muls.l	F_SMAX(a6),d0
	add.l	d3,d0
	lea	(a1,d0.l*4),a0
	fmove.x	fp3,fp1
	fsub.x	fp2,fp1
	fmul.x	fp5,fp1
	fmove.l	(a0),fp0
	tst.l	(a0)
	bge.s	.sl_pos
	fadd.d	#4294967296,fp0
.sl_pos
	fadd.x	fp0,fp1
	fcmp.d	#$41e0000000000000,fp1
	fbge	.sl_big
	fintrz.x	fp1
	fmove.l	fp1,d0
	bra.s	.sl_store
.sl_big
	fsub.d	#$41e0000000000000,fp1
	fintrz.x	fp1
	fmove.l	fp1,d0
	bchg	#31,d0
.sl_store
	move.l	d0,(a0)
.sl_next
	addq.l	#1,d3
	cmp.l	F_SMAX(a6),d3
	blt	.sl_s
	addq.l	#1,d4
	cmp.l	F_TMAX(a6),d4
	blt	.sl_t
	bra	.next
