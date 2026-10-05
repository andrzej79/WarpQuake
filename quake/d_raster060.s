;
; d_raster060.s -- alias model triangle setup for the 68060
;
; D_DrawNonSubdiv, D_PolysetSetEdgeTable, D_RasterizeAliasPolySmooth,
; D_PolysetSetUpForLineScan and D_PolysetCalcGradients, replacing the C in
; d_polyse.c (built with WQ_ASM=1).  This is the per-triangle work for models
; drawn without subdivision (near ones, and always the weapon): back-face
; test, which edges are left and right, the s/t/light/1/z gradients, and the
; per-line steps along the left edge.  In C it was ~1.5 ms a frame
; (hardware profile, 2026-10-04), most of it vbcc keeping every value in
; globals across the calls between these five functions.
;
; Here the five are one call tree with the triangle in registers.  The
; outputs the span code reads (d_polyse060.s: D_PolysetScanLeftEdge,
; D_PolysetDrawSpans8) are still the C's globals.
;
; Exactness:
; - Everything but the gradients is integer, the C's expressions as written
;   (32-bit wrap-around, arithmetic shifts), so it is exact by construction.
; - The gradients are vbcc's float code for the C, operation for operation,
;   including where vbcc rounds to single by spilling a float variable to
;   the stack.  Here those round trips are FSMOVE (round to single, in a
;   register); it is the same rounding (none of these values can leave the
;   single exponent range: they are products of integers and of 1/denom).
; - FloorDivMod (mathlib.c, through floor() on doubles) is integer division
;   here: for these integer arguments both give the floor quotient and its
;   non-negative remainder.
; -crc checks all of it.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1/fp0-fp1
; scratch, everything else preserved.
;

	machine	68060

	xref	_r_affinetridesc	; affinetridesc_t
	xref	_a_spans			; spanpackage_t *: this triangle's packages
	xref	_d_pedgespanpackage
	xref	_r_p0				; int [6]: the triangle's vertices
	xref	_r_p1
	xref	_r_p2
	xref	_d_xdenom			; int: twice the triangle's signed area
	xref	_pedgetable			; edgetable *
	xref	_edgetables			; edgetable [12]
	xref	_adivtab			; adivtab_t [32*32]: small floor divisions
	xref	_d_viewbuffer		; byte *
	xref	_screenwidth		; int
	xref	_d_pzbuffer			; short *
	xref	_d_zwidth			; unsigned
	xref	_ubasestep			; int: the edge stepper's state
	xref	_errorterm
	xref	_erroradjustup
	xref	_erroradjustdown
	xref	_d_aspancount
	xref	_d_countextrastep
	xref	_d_pdest			; the left edge's starting state ...
	xref	_d_pz
	xref	_d_ptex
	xref	_d_sfrac
	xref	_d_tfrac
	xref	_d_light
	xref	_d_zi
	xref	_d_pdestextrastep	; ... and its steps per line
	xref	_d_pzextrastep
	xref	_d_ptexextrastep
	xref	_d_sfracextrastep
	xref	_d_tfracextrastep
	xref	_d_lightextrastep
	xref	_d_ziextrastep
	xref	_d_pdestbasestep
	xref	_d_pzbasestep
	xref	_d_ptexbasestep
	xref	_d_sfracbasestep
	xref	_d_tfracbasestep
	xref	_d_lightbasestep
	xref	_d_zibasestep
	xref	_r_lstepx			; int: the gradients, per pixel ...
	xref	_r_lstepy			; ... and per line
	xref	_r_sstepx
	xref	_r_sstepy
	xref	_r_tstepx
	xref	_r_tstepy
	xref	_r_zistepx
	xref	_r_zistepy
	xref	_a_sstepxfrac
	xref	_a_tstepxfrac
	xref	_a_ststepxwhole
	xref	_D_PolysetScanLeftEdge	; d_polyse060.s
	xref	_D_PolysetDrawSpans8

	xdef	_D_DrawNonSubdiv
	xdef	_D_PolysetSetEdgeTable
	xdef	_D_RasterizeAliasPolySmooth
	xdef	_D_PolysetSetUpForLineScan
	xdef	_D_PolysetCalcGradients

; affinetridesc_t (d_iface.h)
ATD_PSKIN		equ	0
ATD_SKINWIDTH	equ	8
ATD_PTRIANGLES	equ	16
ATD_PFINALVERTS	equ	20
ATD_NUMTRIS		equ	24
ATD_SEAMFIX		equ	32
; finalvert_t, 32 bytes: int v[6] (u, v, s, t, light, 1/z), flags
FV_FLAGS		equ	24
FV_SIZE			equ	32
ALIAS_ONSEAM	equ	$20
; mtriangle_t, 16 bytes: facesfront, vertindex[3]
TRI_FRONT		equ	0
TRI_INDEX		equ	4
TRI_SIZE		equ	16
; a vertex, int [6]
V_U				equ	0
V_V				equ	4
V_S				equ	8
V_T				equ	12
V_LIGHT			equ	16
V_ZI			equ	20
; edgetable (d_polyse.c), 36 bytes
ET_NUMLEFT		equ	4
ET_LEFT0		equ	8
ET_LEFT1		equ	12
ET_LEFT2		equ	16
ET_NUMRIGHT		equ	20
ET_RIGHT0		equ	24
ET_RIGHT1		equ	28
ET_RIGHT2		equ	32
ET_SIZE			equ	36
; spanpackage_t (d_polyse.c), 32 bytes
PK_PDEST		equ	0
PK_PZ			equ	4
PK_COUNT		equ	8
PK_PTEX			equ	12
PK_SFRAC		equ	16
PK_TFRAC		equ	20
PK_LIGHT		equ	24
PK_ZI			equ	28
PK_SIZE			equ	32
PK_END			equ	-999999		; count of the package that ends the list

	section	CODE,code

;------------------------------------------------------------------------------
; void D_DrawNonSubdiv (void)
;
; For each front-facing triangle: its vertices into r_p0..r_p2 (the s of a
; back-side vertex on the skin seam moved to the skin's back half), then
; which edges are left and right, then the rasterizer.
;
;   a2 = the final vertices   a3 = the triangle   d7 = triangles left
;------------------------------------------------------------------------------

	cnop	0,4
_D_DrawNonSubdiv
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	_r_affinetridesc+ATD_PFINALVERTS,a2
	move.l	_r_affinetridesc+ATD_PTRIANGLES,a3
	move.l	_r_affinetridesc+ATD_NUMTRIS,d7
	ble	.done

.tri
	movem.l	TRI_INDEX(a3),d0-d2
	lsl.l	#5,d0				; * sizeof (finalvert_t)
	lsl.l	#5,d1
	lsl.l	#5,d2
	lea	(a2,d0.l),a4		; index0
	lea	(a2,d1.l),a5		; index1
	lea	(a2,d2.l),a6		; index2

	; d_xdenom = (v0 - v1) * (u0 - u2) - (u0 - u1) * (v0 - v2)
	move.l	V_V(a4),d0
	move.l	d0,d3
	sub.l	V_V(a5),d0			; v0 - v1
	sub.l	V_V(a6),d3			; v0 - v2
	move.l	V_U(a4),d1
	move.l	d1,d2
	sub.l	V_U(a6),d1			; u0 - u2
	sub.l	V_U(a5),d2			; u0 - u1
	muls.l	d1,d0
	muls.l	d3,d2
	sub.l	d2,d0
	bge	.next				; back-facing (or degenerate)
	move.l	d0,_d_xdenom

	movem.l	(a4),d0-d5
	movem.l	d0-d5,_r_p0
	movem.l	(a5),d0-d5
	movem.l	d0-d5,_r_p1
	movem.l	(a6),d0-d5
	movem.l	d0-d5,_r_p2

	tst.l	TRI_FRONT(a3)
	bne.s	.seamdone
	move.l	_r_affinetridesc+ATD_SEAMFIX,d0
	moveq	#ALIAS_ONSEAM,d1
	and.l	FV_FLAGS(a4),d1
	beq.s	.seam1
	add.l	d0,_r_p0+V_S
.seam1
	moveq	#ALIAS_ONSEAM,d1
	and.l	FV_FLAGS(a5),d1
	beq.s	.seam2
	add.l	d0,_r_p1+V_S
.seam2
	moveq	#ALIAS_ONSEAM,d1
	and.l	FV_FLAGS(a6),d1
	beq.s	.seamdone
	add.l	d0,_r_p2+V_S
.seamdone

	bsr.s	SetEdgeTable
	bsr	Rasterize

.next
	lea	TRI_SIZE(a3),a3
	subq.l	#1,d7
	bne	.tri

.done
	movem.l	(sp)+,d2-d7/a2-a6
	rts

;------------------------------------------------------------------------------
; void D_PolysetSetEdgeTable (void)
;
; pedgetable from the order of r_p0..r_p2's v (the C's decision tree; ties
; select the flat-top and flat-bottom tables).  Uses d0-d2, a0.
;------------------------------------------------------------------------------

	cnop	0,4
_D_PolysetSetEdgeTable
SetEdgeTable
	lea	_edgetables,a0
	move.l	_r_p0+V_V,d0
	move.l	_r_p1+V_V,d1
	move.l	_r_p2+V_V,d2
	cmp.l	d1,d0
	blt.s	.index0
	bne.s	.index1
	; v0 == v1
	cmp.l	d2,d0
	bge.s	.t5
	lea	2*ET_SIZE(a0),a0
	move.l	a0,_pedgetable
	rts
.t5	lea	5*ET_SIZE(a0),a0
	move.l	a0,_pedgetable
	rts

.index1
	; v0 > v1: index 1
	cmp.l	d2,d0
	bne.s	.i1ne02
	lea	8*ET_SIZE(a0),a0
	move.l	a0,_pedgetable
	rts
.i1ne02
	cmp.l	d2,d1
	bne.s	.i1order
	lea	10*ET_SIZE(a0),a0
	move.l	a0,_pedgetable
	rts
.i1order
	lea	1*ET_SIZE(a0),a0
	bra.s	.order

.index0
	cmp.l	d2,d0
	bne.s	.i0ne02
	lea	9*ET_SIZE(a0),a0
	move.l	a0,_pedgetable
	rts
.i0ne02
	cmp.l	d2,d1
	bne.s	.order
	lea	11*ET_SIZE(a0),a0
	move.l	a0,_pedgetable
	rts

.order
	; + 2 if v0 > v2, + 4 if v1 > v2
	cmp.l	d2,d0
	ble.s	.o1
	lea	2*ET_SIZE(a0),a0
.o1	cmp.l	d2,d1
	ble.s	.o2
	lea	4*ET_SIZE(a0),a0
.o2	move.l	a0,_pedgetable
	rts

;------------------------------------------------------------------------------
; void D_PolysetSetUpForLineScan (fixed8_t startvertu, fixed8_t startvertv,
;                                 fixed8_t endvertu, fixed8_t endvertv)
;
; The edge stepper for one edge: ubasestep = floor (du / dv), erroradjustup
; its remainder, erroradjustdown = dv, errorterm = -1.  Small edges come
; from adivtab, as in the C.
;
; SetUpLineScan is the internal entry: d0 = du, d1 = dv; returns ubasestep
; in d0.  Uses d0-d1, a0 (not a1).
;------------------------------------------------------------------------------

	cnop	0,4
_D_PolysetSetUpForLineScan
	move.l	12(sp),d0
	sub.l	4(sp),d0
	move.l	16(sp),d1
	sub.l	8(sp),d1
SetUpLineScan
	move.l	#-1,_errorterm
	move.l	d1,_erroradjustdown
	move.l	d2,-(sp)
	; table if -15 <= du <= 16 and -15 <= dv <= 16 (as unsigned: d + 15 <= 31)
	moveq	#15,d2
	add.l	d1,d2				; dv + 15
	cmp.l	#31,d2
	bhi.s	.divide
	move.l	d0,a0
	lea	15(a0),a0			; du + 15
	cmp.l	#31,a0
	bhi.s	.divide
	move.l	a0,d0
	lsl.l	#5,d0
	add.l	d2,d0				; ((du + 15) << 5) + (dv + 15)
	lea	_adivtab,a0
	lea	(a0,d0.l*8),a0
	move.l	4(a0),_erroradjustup
	move.l	(a0),d0
	move.l	d0,_ubasestep
	move.l	(sp)+,d2
	rts
.divide
	; floor division: DIVSL truncates toward 0, so a negative quotient with
	; a remainder is one too big and the remainder one dv short (dv > 0)
	divsl.l	d1,d2:d0			; d0 = du / dv, d2 = du % dv
	tst.l	d2
	beq.s	.exact
	bpl.s	.exact
	subq.l	#1,d0
	add.l	d1,d2
.exact
	move.l	d2,_erroradjustup
	move.l	d0,_ubasestep
	move.l	(sp)+,d2
	rts

;------------------------------------------------------------------------------
; void D_PolysetCalcGradients (int skinwidth)
;
; The per-pixel (x) and per-line (y) steps of light, s, t and 1/z across the
; triangle r_p0..r_p2, from the plane through them:
;
;   step_x = (t1 * p01_minus_p21 - t0 * p11_minus_p21) * xstepdenominv
;   step_y = (t1 * p00_minus_p20 - t0 * p10_minus_p20) * ystepdenominv
;
; with t0, t1 the value's differences to vertex 2; ceil() for the light,
; truncation for the rest.  vbcc's operation order throughout (see the file
; header), with these registers:
;
;   fp4 = p00_minus_p20   fp3 = p10_minus_p20   fp6 = p11_minus_p21
;   fp5 = xstepdenominv   v_p01 = p01_minus_p21 (a float in memory, as vbcc
;   keeps it; exact - a screen coordinate difference)   fp0, fp1 = t0, t1
;   fp2, fp7 = scratch
;
; ystepdenominv = -xstepdenominv is not kept: x * -y is -(x * y) exactly, so
; each y step is its product with xstepdenominv, negated.
;
; CalcGradients is the internal entry: d0 = skinwidth.  Uses d0-d1, fp0-fp7;
; the caller saves fp2-fp7.
;------------------------------------------------------------------------------

; Ceil a, then truncate it to an int at dest (vbcc's inline ceil: truncate,
; and add 1 if that went down).  Uses fp1.
CeilTo	macro
	fmove.x	\1,fp1
	fintrz.x	\1
	fcmp.x	fp1,\1
	fboge	.c\@
	fadd.s	#$3f800000,\1
.c\@
	fintrz.x	\1
	fmove.l	\1,\2
	endm

; The s or t gradients: \1 = offset of the value in a vertex, \2 / \3 the x
; and y step variables.  vbcc's code for these rounds to single at each of
; its spills: x = single (xinv * single (single (t1 * p01) - single (t0 *
; p11))), y unrounded.
STGradients	macro
	move.l	_r_p0+\1,d0
	sub.l	_r_p2+\1,d0
	fmove.l	d0,fp0				; t0
	move.l	_r_p1+\1,d0
	sub.l	_r_p2+\1,d0
	fmove.l	d0,fp1				; t1
	fmove.x	fp0,fp2
	fmul.x	fp6,fp2
	fsmove.x	fp2,fp2			; single (t0 * p11)
	fmove.x	fp1,fp7
	fmul.s	v_p01,fp7
	fsmove.x	fp7,fp7			; single (t1 * p01)
	fsub.x	fp2,fp7
	fsmove.x	fp7,fp7
	fmul.x	fp5,fp7
	fsmove.x	fp7,fp7
	fintrz.x	fp7
	fmove.l	fp7,\2
	fmul.x	fp4,fp1				; t1 * p00
	fmul.x	fp3,fp0				; t0 * p10
	fneg.x	fp0
	fadd.x	fp1,fp0
	fmul.x	fp5,fp0
	fneg.x	fp0					; * ystepdenominv
	fintrz.x	fp0
	fmove.l	fp0,\3
	endm

	cnop	0,4
_D_PolysetCalcGradients
	move.l	4(sp),d0
	fmovem.x	fp2-fp7,-(sp)
	bsr.s	CalcGradients
	fmovem.x	(sp)+,fp2-fp7
	rts

CalcGradients
	move.l	d0,-(sp)			; skinwidth, for the end
	move.l	_r_p0+V_U,d0
	sub.l	_r_p2+V_U,d0
	fmove.l	d0,fp4				; p00_minus_p20
	move.l	_r_p0+V_V,d0
	sub.l	_r_p2+V_V,d0
	fmove.l	d0,fp0
	fmove.s	fp0,v_p01			; p01_minus_p21
	move.l	_r_p1+V_U,d0
	sub.l	_r_p2+V_U,d0
	fmove.l	d0,fp3				; p10_minus_p20
	move.l	_r_p1+V_V,d0
	sub.l	_r_p2+V_V,d0
	fmove.l	d0,fp6				; p11_minus_p21
	fmove.l	_d_xdenom,fp0
	fmove.s	#$3f800000,fp5
	fdiv.x	fp0,fp5				; xstepdenominv = 1.0 / d_xdenom

	; light: t0, t1 are floats in memory in vbcc's code (exact: light is
	; 16 bits)
	move.l	_r_p0+V_LIGHT,d0
	sub.l	_r_p2+V_LIGHT,d0
	fmove.l	d0,fp7				; t0
	move.l	_r_p1+V_LIGHT,d0
	sub.l	_r_p2+V_LIGHT,d0
	fmove.l	d0,fp2				; t1
	fmove.x	fp2,fp0
	fmul.s	v_p01,fp0			; t1 * p01
	fmove.x	fp7,fp1
	fmul.x	fp6,fp1				; t0 * p11
	fneg.x	fp1
	fadd.x	fp0,fp1
	fmul.x	fp5,fp1
	fmove.x	fp1,fp0
	CeilTo	fp0,_r_lstepx
	fmove.x	fp2,fp0
	fmul.x	fp4,fp0				; t1 * p00
	fmove.x	fp7,fp1
	fmul.x	fp3,fp1				; t0 * p10
	fneg.x	fp1
	fadd.x	fp0,fp1
	fmul.x	fp5,fp1
	fneg.x	fp1					; * ystepdenominv
	fmove.x	fp1,fp0
	CeilTo	fp0,_r_lstepy

	STGradients	V_S,_r_sstepx,_r_sstepy
	STGradients	V_T,_r_tstepx,_r_tstepy

	; 1/z: x = xinv * (single (t1 * p01) - t0 * p11), no other rounding
	move.l	_r_p0+V_ZI,d0
	sub.l	_r_p2+V_ZI,d0
	fmove.l	d0,fp0				; t0
	move.l	_r_p1+V_ZI,d0
	sub.l	_r_p2+V_ZI,d0
	fmove.l	d0,fp1				; t1
	fmove.x	fp1,fp7
	fmul.s	v_p01,fp7
	fsmove.x	fp7,fp7			; single (t1 * p01)
	fmul.x	fp0,fp6				; p11 * t0
	fneg.x	fp6
	fadd.x	fp7,fp6
	fmul.x	fp5,fp6				; (vbcc: into fp5; xinv is still needed)
	fintrz.x	fp6
	fmove.l	fp6,_r_zistepx
	fmul.x	fp4,fp1
	fmul.x	fp3,fp0
	fneg.x	fp0
	fadd.x	fp1,fp0
	fmul.x	fp5,fp0
	fneg.x	fp0
	fintrz.x	fp0
	fmove.l	fp0,_r_zistepy

	; a_sstepxfrac, a_tstepxfrac, a_ststepxwhole
	move.l	(sp)+,d0			; skinwidth
	move.l	_r_tstepx,d1
	swap	d1
	ext.l	d1					; r_tstepx >> 16
	muls.l	d1,d0
	move.l	_r_sstepx,d1
	swap	d1
	ext.l	d1					; r_sstepx >> 16
	add.l	d1,d0
	move.l	d0,_a_ststepxwhole
	moveq	#0,d0
	move.w	_r_sstepx+2,d0		; & 0xFFFF
	move.l	d0,_a_sstepxfrac
	move.w	_r_tstepx+2,d0
	move.l	d0,_a_tstepxfrac
	rts

;------------------------------------------------------------------------------
; void D_RasterizeAliasPolySmooth (void)
;
; Rasterize the triangle r_p0..r_p2 by pedgetable: the gradients, then the
; left edge (one or two sections) into span packages, then the right edge,
; which D_PolysetDrawSpans8 steps while it draws the packages.
;
;   a2 = left top   a4 = left bottom   a3 = right top   a5 = right bottom
;   a6 = pedgetable   d6 = initial left height   d7 = initial right height
;------------------------------------------------------------------------------

	cnop	0,4
_D_RasterizeAliasPolySmooth
	movem.l	d2-d7/a2-a6,-(sp)
	bsr.s	Rasterize
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; The internal entry: may use every register but d7/a2-a3 (D_DrawNonSubdiv's
; loop state), which it saves.
Rasterize
	movem.l	d7/a2-a3,-(sp)
	fmovem.x	fp2-fp7,-(sp)
	move.l	_pedgetable,a6
	move.l	ET_LEFT0(a6),a2
	move.l	ET_LEFT1(a6),a4
	move.l	ET_RIGHT0(a6),a3
	move.l	ET_RIGHT1(a6),a5
	move.l	V_V(a4),d6
	sub.l	V_V(a2),d6			; initialleftheight
	move.l	V_V(a5),d7
	sub.l	V_V(a3),d7			; initialrightheight

	move.l	_r_affinetridesc+ATD_SKINWIDTH,d0
	bsr	CalcGradients
	fmovem.x	(sp)+,fp2-fp7

	move.l	_a_spans,_d_pedgespanpackage

	; the top (and possibly only) section of the left edge, starting at
	; the vertex's s and t fractions
	move.l	V_S(a2),d4
	and.l	#$ffff,d4
	move.l	V_T(a2),d5
	and.l	#$ffff,d5
	bsr	LeftSection

	; the bottom section, if any: from the middle vertex, fractions 0 (as
	; in the C)
	cmp.l	#2,ET_NUMLEFT(a6)
	bne.s	.right
	move.l	a4,a2
	move.l	ET_LEFT2(a6),a4
	move.l	V_V(a4),d6
	sub.l	V_V(a2),d6
	moveq	#0,d4
	moveq	#0,d5
	bsr	LeftSection

.right
	; the top section of the right edge: DrawSpans8 steps it (from
	; d_aspancount 0) as it draws the packages up to the section's end
	move.l	_a_spans,a0
	move.l	a0,_d_pedgespanpackage
	move.l	V_U(a5),d0
	sub.l	V_U(a3),d0
	move.l	V_V(a5),d1
	sub.l	V_V(a3),d1
	bsr	SetUpLineScan
	clr.l	_d_aspancount
	addq.l	#1,d0
	move.l	d0,_d_countextrastep
	move.l	_a_spans,a0
	move.l	d7,d0
	lsl.l	#5,d0				; * sizeof (spanpackage_t)
	lea	(a0,d0.l),a1		; a_spans + initialrightheight
	move.l	PK_COUNT(a1),d2		; originalcount
	move.l	#PK_END,PK_COUNT(a1)
	move.l	a1,-(sp)
	move.l	a0,-(sp)
	jsr	_D_PolysetDrawSpans8
	addq.l	#4,sp
	move.l	(sp)+,a1

	; the bottom section of the right edge, if any
	cmp.l	#2,ET_NUMRIGHT(a6)
	bne.s	.done
	move.l	d2,PK_COUNT(a1)		; pstart->count = originalcount
	move.l	V_U(a5),d0
	sub.l	V_U(a3),d0
	move.l	d0,_d_aspancount
	move.l	a5,a3
	move.l	ET_RIGHT2(a6),a5
	move.l	V_V(a5),d6
	sub.l	V_V(a3),d6			; height
	move.l	V_U(a5),d0
	sub.l	V_U(a3),d0
	move.l	d6,d1
	bsr	SetUpLineScan
	addq.l	#1,d0
	move.l	d0,_d_countextrastep
	add.l	d7,d6
	lsl.l	#5,d6
	move.l	_a_spans,a0
	move.l	#PK_END,PK_COUNT(a0,d6.l)
	move.l	a1,-(sp)
	jsr	_D_PolysetDrawSpans8
	addq.l	#4,sp

.done
	movem.l	(sp)+,d7/a2-a3
	rts

;------------------------------------------------------------------------------
; LeftSection: one section of the left edge, from vertex a2 down d6 lines
; to vertex a4, into span packages from d_pedgespanpackage on.  d4/d5 = the
; starting s/t fractions, a3 = the right edge's top vertex.
;
; The starting state into the C's globals, then either the one package (a
; section one line high) or the stepper's per-line steps and
; D_PolysetScanLeftEdge.  Uses d0-d5, a0-a1.
;------------------------------------------------------------------------------

	cnop	0,4
LeftSection
	move.l	V_U(a2),d0
	sub.l	V_U(a3),d0
	move.l	d0,_d_aspancount
	move.l	d4,_d_sfrac
	move.l	d5,_d_tfrac
	move.l	V_LIGHT(a2),_d_light
	move.l	V_ZI(a2),_d_zi

	; d_ptex = pskin + (s >> 16) + (t >> 16) * skinwidth
	move.l	V_T(a2),d0
	swap	d0
	ext.l	d0
	muls.l	_r_affinetridesc+ATD_SKINWIDTH,d0
	move.l	V_S(a2),d1
	swap	d1
	ext.l	d1
	add.l	d1,d0
	add.l	_r_affinetridesc+ATD_PSKIN,d0
	move.l	d0,_d_ptex

	; d_pdest = d_viewbuffer + ystart * screenwidth + u
	; d_pz = d_pzbuffer + ystart * d_zwidth + u
	move.l	V_V(a2),d0			; ystart
	move.l	d0,d1
	muls.l	_screenwidth,d0
	add.l	V_U(a2),d0
	add.l	_d_viewbuffer,d0
	move.l	d0,_d_pdest
	muls.l	_d_zwidth,d1
	add.l	V_U(a2),d1
	add.l	d1,d1				; shorts
	add.l	_d_pzbuffer,d1
	move.l	d1,_d_pz

	cmp.l	#1,d6
	bne.s	.scan
	; one line: just the one package
	move.l	_d_pedgespanpackage,a0
	move.l	_d_pdest,PK_PDEST(a0)
	move.l	d1,PK_PZ(a0)
	move.l	_d_aspancount,PK_COUNT(a0)
	move.l	_d_ptex,PK_PTEX(a0)
	move.l	d4,PK_SFRAC(a0)
	move.l	d5,PK_TFRAC(a0)
	move.l	_d_light,PK_LIGHT(a0)
	move.l	_d_zi,PK_ZI(a0)
	lea	PK_SIZE(a0),a0
	move.l	a0,_d_pedgespanpackage
	rts

.scan
	move.l	V_U(a4),d0
	sub.l	V_U(a2),d0
	move.l	V_V(a4),d1
	sub.l	V_V(a2),d1
	bsr	SetUpLineScan		; d0 = ubasestep

	move.l	_d_zwidth,d1
	add.l	d0,d1
	move.l	d1,_d_pzbasestep
	addq.l	#1,d1
	move.l	d1,_d_pzextrastep
	move.l	_screenwidth,d1
	add.l	d0,d1
	move.l	d1,_d_pdestbasestep
	addq.l	#1,d1
	move.l	d1,_d_pdestextrastep

	; working_lstepx: r_lstepx, one less for a negative ubasestep
	move.l	_r_lstepx,d4
	tst.l	d0
	bge.s	.lpos
	subq.l	#1,d4
.lpos
	move.l	d0,d1
	addq.l	#1,d1
	move.l	d1,_d_countextrastep

	; sb = r_sstepy + r_sstepx * ubasestep, tb likewise; the extra steps
	; are one more x step (r_sstepy + r_sstepx * (ubasestep + 1))
	move.l	_r_sstepx,d2
	muls.l	d0,d2
	add.l	_r_sstepy,d2		; sb
	move.l	_r_tstepx,d3
	muls.l	d0,d3
	add.l	_r_tstepy,d3		; tb
	move.l	d2,d1
	and.l	#$ffff,d1
	move.l	d1,_d_sfracbasestep
	move.l	d3,d1
	and.l	#$ffff,d1
	move.l	d1,_d_tfracbasestep
	move.l	d3,d1
	swap	d1
	ext.l	d1					; tb >> 16
	muls.l	_r_affinetridesc+ATD_SKINWIDTH,d1
	move.l	d2,d5
	swap	d5
	ext.l	d5					; sb >> 16
	add.l	d5,d1
	move.l	d1,_d_ptexbasestep

	add.l	_r_sstepx,d2		; se
	add.l	_r_tstepx,d3		; te
	move.l	d2,d1
	and.l	#$ffff,d1
	move.l	d1,_d_sfracextrastep
	move.l	d3,d1
	and.l	#$ffff,d1
	move.l	d1,_d_tfracextrastep
	swap	d3
	ext.l	d3
	muls.l	_r_affinetridesc+ATD_SKINWIDTH,d3
	swap	d2
	ext.l	d2
	add.l	d2,d3
	move.l	d3,_d_ptexextrastep

	; light and 1/z
	move.l	d4,d1
	muls.l	d0,d1
	add.l	_r_lstepy,d1
	move.l	d1,_d_lightbasestep
	add.l	d4,d1
	move.l	d1,_d_lightextrastep
	move.l	_r_zistepx,d1
	muls.l	d0,d1
	add.l	_r_zistepy,d1
	move.l	d1,_d_zibasestep
	add.l	_r_zistepx,d1
	move.l	d1,_d_ziextrastep

	move.l	d6,-(sp)
	jsr	_D_PolysetScanLeftEdge
	addq.l	#4,sp
	rts

	section	BSS,bss

	cnop	0,4
v_p01	ds.l	1				; float: p01_minus_p21
