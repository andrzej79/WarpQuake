;
; d_polyse060.s -- the alias model span drawer for the 68060
;
; D_PolysetDrawSpans8, replacing the C in d_polyse.c (built with WQ_ASM=1):
; the inner loop that draws monsters, items and the weapon, one horizontal
; span at a time, z-tested and lit through the colormap.  It was ~4% of the
; frame in C (hardware profile, 2026-10-04).  Integer only, so it matches the
; C exactly; -crc checks it.
;
; The texture walk is the C's, in the 68k idiom used elsewhere here: the s
; and t fractions ride in the upper words of two registers, and the carries
; out of them (SUBX gives 0 or -1) step the texel pointer by one texel and by
; one skin row.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1 scratch.
;

	machine	68060

; 16 bpp (d_rgb.c): assembled again with PIX16 = 1 by d_polyse060rgb.s, this
; file gives D_PolysetDrawSpansRGB and D_PolysetRecursiveTriangleRGB, which
; light through the 16-bit colormap (acolormap, d_pcolormap) and write 16-bit
; pixels.  The rasterizer is not duplicated: it keeps computing 8-bit
; pointers from d_viewbuffer, which here count pixels, so a pixel's address
; is 2 * pdest - d_viewbuffer.
	ifnd	PIX16
PIX16	equ	0
	endif

	xref	_d_aspancount		; int: this triangle's span lengths ...
	xref	_errorterm			; ... stepped Bresenham-style
	xref	_erroradjustup
	xref	_erroradjustdown
	xref	_d_countextrastep
	xref	_ubasestep
	xref	_acolormap			; void *: the lighting colormap
	xref	_r_zistepx			; int: per-pixel steps
	xref	_r_lstepx
	xref	_a_ststepxwhole
	xref	_a_sstepxfrac
	xref	_a_tstepxfrac
	xref	_r_affinetridesc		; affinetridesc_t; skinwidth at ATD_SKINWIDTH
	xref	_wqc_count			; unsigned long[]: wq_prof.h work counters

	if PIX16
	xdef	_D_PolysetDrawSpansRGB
	xdef	_D_PolysetRecursiveTriangleRGB
	else
	xdef	_D_PolysetDrawSpans8
	xdef	_D_PolysetScanLeftEdge
	xdef	_D_PolysetRecursiveTriangle
	endif

	xref	_zspantable			; short *[]: z-buffer row starts
	xref	_d_scantable		; int []: frame row offsets
	xref	_d_viewbuffer		; byte *
	xref	_skintable			; byte *[]: skin row starts
	xref	_d_pcolormap		; byte *: the colormap row for this model's light

	xref	_d_pedgespanpackage	; spanpackage_t *: next package to fill
	xref	_d_pdest			; the left edge's state, stepped per line ...
	xref	_d_pz
	xref	_d_ptex
	xref	_d_sfrac
	xref	_d_tfrac
	xref	_d_light
	xref	_d_zi
	xref	_d_pdestextrastep	; ... by these when the error term overflows ...
	xref	_d_pzextrastep
	xref	_d_ptexextrastep
	xref	_d_sfracextrastep
	xref	_d_tfracextrastep
	xref	_d_lightextrastep
	xref	_d_ziextrastep
	xref	_d_pdestbasestep	; ... and by these otherwise
	xref	_d_pzbasestep
	xref	_d_ptexbasestep
	xref	_d_sfracbasestep
	xref	_d_tfracbasestep
	xref	_d_lightbasestep
	xref	_d_zibasestep

; spanpackage_t (d_polyse.c), 32 bytes
PK_PDEST	equ	0
PK_PZ		equ	4
PK_COUNT	equ	8
PK_PTEX		equ	12
PK_SFRAC	equ	16
PK_TFRAC	equ	20
PK_LIGHT	equ	24
PK_ZI		equ	28
PK_SIZE		equ	32
PK_END		equ	-999999		; count of the package that ends the list

ATD_SKINWIDTH	equ	8			; offsetof (affinetridesc_t, skinwidth)
WQC_POLY_PIXELS	equ	3*4			; wqc_count[] index, wq_prof.h

	section	CODE,code

;------------------------------------------------------------------------------
; void D_PolysetDrawSpans8 (spanpackage_t *pspanpackage)
;
; Per package (span): its length is d_aspancount - count, and d_aspancount
; then steps on for the next one.  Per pixel:
;
;   if ((zi >> 16) >= *pz) { *pdest = colormap[*ptex + (light & 0xFF00)];
;                            *pz = zi >> 16; }
;   then every pointer and value one pixel on
;
; The z test comes first and only a visible pixel reads its texel (a skin
; read is often a cache miss, and much of a model is hidden).  The texel
; pointer steps by a 4-entry table, v_steps, indexed by the two fraction
; carries the way ClickBOOM's loop does it: ADD sets X from the s fraction,
; SUBX makes it 0/-1, ADD of the t fraction sets X again, ADDX doubles and
; adds it - so -2 * s carry + t carry, -2 .. 1.  (The package pointer lives
; in v_pkg during a span, freeing a register for the light step.)  -0.14 ms
; a frame on hardware against the previous loop (2026-10-05).
;
;   a0 = pdest   a1 = ptex   a2 = pz   a3 = acolormap   a4 = zistepx
;   a5 = lstepx   a6 = v_steps + 8   d7 = sstep fraction << 16
;   d0 = pixels left   d1 = colormap index (upper word 0)
;   d2 = zi   d3 = light   d4 = sfrac << 16   d5 = tfrac << 16   d6 = scratch
;   v_tstep: the t fraction step << 16
;------------------------------------------------------------------------------

	cnop	0,4
	if PIX16
_D_PolysetDrawSpansRGB
	else
_D_PolysetDrawSpans8
	endif
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	4+11*4(sp),a2
	move.l	a2,v_pkg
	move.l	_acolormap,a3
	move.l	_r_zistepx,a4
	move.l	_r_lstepx,a5
	; the texel step table, indexed by -2s + t for the two fraction carries
	; (s carry: one texel on, t carry: one skin row on)
	move.l	_a_ststepxwhole,d0
	move.l	_r_affinetridesc+ATD_SKINWIDTH,d1
	lea	v_steps+8,a6
	move.l	d0,(a6)				; [0]: no carry
	add.l	d1,d0
	move.l	d0,4(a6)			; [1]: t
	addq.l	#1,d0
	move.l	d0,-4(a6)			; [-1]: s and t
	sub.l	d1,d0
	move.l	d0,-8(a6)			; [-2]: s
	move.l	_a_sstepxfrac,d7
	swap	d7
	clr.w	d7				; sstep fraction << 16
	move.l	_a_tstepxfrac,d0
	swap	d0
	clr.w	d0
	move.l	d0,v_tstep			; tstep fraction << 16
	moveq	#0,d1

.package
	move.l	v_pkg,a2
	move.l	_d_aspancount,d0
	sub.l	PK_COUNT(a2),d0
	ble.s	.nocount
	add.l	d0,_wqc_count+WQC_POLY_PIXELS
.nocount
	move.l	_errorterm,d6
	add.l	_erroradjustup,d6
	blt.s	.basestep
	sub.l	_erroradjustdown,d6
	move.l	d6,_errorterm
	move.l	_d_countextrastep,d6
	add.l	d6,_d_aspancount
	bra.s	.stepped
.basestep
	move.l	d6,_errorterm
	move.l	_ubasestep,d6
	add.l	d6,_d_aspancount
.stepped
	tst.l	d0
	ble	.nextpackage

	if PIX16
	move.l	PK_PDEST(a2),d6
	add.l	d6,d6
	sub.l	_d_viewbuffer,d6
	move.l	d6,a0				; 2 * pdest - d_viewbuffer
	else
	move.l	PK_PDEST(a2),a0
	endif
	move.l	PK_PTEX(a2),a1
	move.l	PK_SFRAC(a2),d4
	swap	d4				; sfrac < 0x10000: its upper word is 0
	move.l	PK_TFRAC(a2),d5
	swap	d5
	move.l	PK_LIGHT(a2),d3
	move.l	PK_ZI(a2),d2
	move.l	PK_PZ(a2),a2

.pixel
	; z test first; only a visible pixel reads its texel
	move.l	d2,d6
	swap	d6				; zi >> 16
	cmp.w	(a2)+,d6
	blt.s	.hidden
	move.w	d3,d1				; light's bits 8-15 ...
	move.b	(a1),d1				; ... and the texel
	move.w	d6,-2(a2)
	if PIX16
	move.w	(a3,d1.l*2),(a0)
.hidden
	addq.l	#2,a0
	else
	move.b	(a3,d1.l),(a0)
.hidden
	addq.l	#1,a0
	endif
	add.l	a4,d2				; zi
	add.l	a5,d3				; light
	add.l	d7,d4				; s fraction: X = its carry
	subx.l	d6,d6				; -s carry
	add.l	v_tstep,d5			; t fraction: X = its carry
	addx.l	d6,d6				; -2 s carry + t carry
	adda.l	(a6,d6.l*4),a1
	subq.l	#1,d0
	bne.s	.pixel

.nextpackage
	move.l	v_pkg,a2
	lea	PK_SIZE(a2),a2
	move.l	a2,v_pkg
	cmp.l	#PK_END,PK_COUNT(a2)
	bne	.package

	movem.l	(sp)+,d2-d7/a2-a6
	rts

;------------------------------------------------------------------------------
; void D_PolysetScanLeftEdge (int height)
;
; Walk a triangle's left edge down height scan lines, writing each line's
; starting state into a span package for D_PolysetDrawSpans8, and stepping
; Bresenham-style (the "extra" steps when the error term overflows, the
; "base" steps otherwise).  The C kept all of it in globals; here it is in
; registers, written back at the end.
;
;   a0 = package   a1 = pdest   a2 = pz   a3 = ptex   d0 = lines left
;   d1 = errorterm   d2 = aspancount   d3 = sfrac   d4 = tfrac
;   d5 = light   d6 = zi   d7 = skinwidth
;
; Fraction carries: sfrac + step < 0x20000, so after SWAP its low word is the
; carry (0 or 1) for ADDA.W, and CLR.W + SWAP leave sfrac & 0xFFFF.  The t
; carry is bit 16, BCLR tests and clears it.
;------------------------------------------------------------------------------

; Step kind: one line on by the "kind" steps (extra or base) - the C's two
; branches, identical but for the step variables.
EdgeStep	macro
	add.l	_d_pdest\1step,a1
	add.l	_d_pz\1step,a2			; pz is a short *: the step
	add.l	_d_pz\1step,a2			; counts shorts, twice in bytes
	add.l	\2,d2				; aspancount
	add.l	_d_ptex\1step,a3
	add.l	_d_sfrac\1step,d3
	swap	d3
	adda.w	d3,a3				; ptex += sfrac >> 16
	clr.w	d3
	swap	d3				; sfrac &= 0xFFFF
	add.l	_d_tfrac\1step,d4
	bclr	#16,d4
	beq.s	.t\@
	adda.l	d7,a3				; t carried: one skin row on
.t\@
	add.l	_d_light\1step,d5
	add.l	_d_zi\1step,d6
	endm

	if PIX16=0
	cnop	0,4
_D_PolysetScanLeftEdge
	movem.l	d2-d7/a2-a3,-(sp)
	move.l	4+8*4(sp),d0
	move.l	_d_pedgespanpackage,a0
	move.l	_d_pdest,a1
	move.l	_d_pz,a2
	move.l	_d_ptex,a3
	move.l	_errorterm,d1
	move.l	_d_aspancount,d2
	move.l	_d_sfrac,d3
	move.l	_d_tfrac,d4
	move.l	_d_light,d5
	move.l	_d_zi,d6
	move.l	_r_affinetridesc+ATD_SKINWIDTH,d7

.line
	move.l	a1,PK_PDEST(a0)
	move.l	a2,PK_PZ(a0)
	move.l	d2,PK_COUNT(a0)
	move.l	a3,PK_PTEX(a0)
	move.l	d3,PK_SFRAC(a0)
	move.l	d4,PK_TFRAC(a0)
	move.l	d5,PK_LIGHT(a0)
	move.l	d6,PK_ZI(a0)
	lea	PK_SIZE(a0),a0

	add.l	_erroradjustup,d1
	blt.s	.base
	EdgeStep	extra,_d_countextrastep
	sub.l	_erroradjustdown,d1
	bra.s	.stepped
.base
	EdgeStep	base,_ubasestep
.stepped
	subq.l	#1,d0
	bne	.line

	move.l	a0,_d_pedgespanpackage
	move.l	a1,_d_pdest
	move.l	a2,_d_pz
	move.l	a3,_d_ptex
	move.l	d1,_errorterm
	move.l	d2,_d_aspancount
	move.l	d3,_d_sfrac
	move.l	d4,_d_tfrac
	move.l	d5,_d_light
	move.l	d6,_d_zi
	movem.l	(sp)+,d2-d7/a2-a3
	rts
	endif	; PIX16=0: D_PolysetScanLeftEdge

;------------------------------------------------------------------------------
; void D_PolysetRecursiveTriangle (int *lp1, int *lp2, int *lp3)
;
; Subdivision rasterizer for small or distant models: split the longest
; edge at its midpoint, plot the midpoint (on all but leading edges), and
; recurse on the two halves, until every edge spans at most one pixel.
; Vertices are int [6]: x, y, s, t, light, zi (the midpoint's light is never
; computed, as in the C - it is not used here).
;
; The recursion uses a private convention: lp1, lp2, lp3 in a0, a1, a2, a
; 36-byte frame per level (new[6], then the three pointers), nothing saved.
; The C wrapper saves the registers once.
;------------------------------------------------------------------------------

V_X	equ	0
V_Y	equ	4
V_S	equ	8
V_T	equ	12
V_ZI	equ	20
RT_NEW	equ	0				; frame: new[6] ...
RT_LP1	equ	24				; ... and the three vertices
RT_LP2	equ	28
RT_LP3	equ	32
RT_SIZE	equ	36

; IfFar a, b, label: branch to label unless -1 <= a - b <= 1 (as unsigned:
; a - b + 1 <= 2).  Uses d0.
IfFar	macro
	move.l	\1,d0
	sub.l	\2,d0
	addq.l	#1,d0
	cmp.l	#2,d0
	bhi	\3
	endm

	cnop	0,4
	if PIX16
_D_PolysetRecursiveTriangleRGB
	else
_D_PolysetRecursiveTriangle
	endif
	movem.l	d2-d3/a2,-(sp)
	movem.l	4+3*4(sp),a0-a2
	bsr.s	.tri
	movem.l	(sp)+,d2-d3/a2
	rts

.tri
	IfFar	V_X(a1),V_X(a0),.split
	IfFar	V_Y(a1),V_Y(a0),.split
	IfFar	V_X(a2),V_X(a1),.split2
	IfFar	V_Y(a2),V_Y(a1),.split2
	IfFar	V_X(a0),V_X(a2),.split3
	IfFar	V_Y(a0),V_Y(a2),.split3
	rts					; the whole triangle is filled

.split3
	; split the lp3 - lp1 edge: lp1, lp2, lp3 = lp3, lp1, lp2
	move.l	a0,d0
	move.l	a2,a0
	move.l	a1,a2
	move.l	d0,a1
	bra.s	.split
.split2
	; split the lp2 - lp3 edge: lp1, lp2, lp3 = lp2, lp3, lp1
	move.l	a0,d0
	move.l	a1,a0
	move.l	a2,a1
	move.l	d0,a2
.split
	; new = (lp1 + lp2) >> 1, for x, y, s, t and zi
	lea	-RT_SIZE(sp),sp
	movem.l	a0-a2,RT_LP1(sp)
	move.l	V_X(a0),d0
	add.l	V_X(a1),d0
	asr.l	#1,d0
	move.l	d0,RT_NEW+V_X(sp)
	move.l	V_Y(a0),d1
	add.l	V_Y(a1),d1
	asr.l	#1,d1
	move.l	d1,RT_NEW+V_Y(sp)
	move.l	V_S(a0),d2
	add.l	V_S(a1),d2
	asr.l	#1,d2
	move.l	d2,RT_NEW+V_S(sp)
	move.l	V_T(a0),d3
	add.l	V_T(a1),d3
	asr.l	#1,d3
	move.l	d3,RT_NEW+V_T(sp)
	move.l	V_ZI(a0),a2
	add.l	V_ZI(a1),a2			; (a2 is saved in the frame)
	move.l	a2,d1
	asr.l	#1,d1
	move.l	d1,RT_NEW+V_ZI(sp)

	; plot the midpoint, unless this is a leading edge
	move.l	V_Y(a1),d1
	cmp.l	V_Y(a0),d1
	bgt.s	.nodraw				; lp2 below lp1
	bne.s	.draw
	move.l	V_X(a1),d1
	cmp.l	V_X(a0),d1
	blt.s	.nodraw				; same line, lp2 left of lp1
.draw
	; z = new.zi >> 16, tested against zspantable[y][x]
	move.l	RT_NEW+V_ZI(sp),d1
	swap	d1
	ext.l	d1				; (an arithmetic >> 16)
	move.l	RT_NEW+V_Y(sp),d2		; y
	lea	_zspantable,a0
	move.l	(a0,d2.l*4),a0
	lea	(a0,d0.l*2),a0			; + x shorts
	move.w	(a0),d3
	ext.l	d3
	cmp.l	d3,d1
	blt.s	.nodraw
	move.w	d1,(a0)
	; pix = d_pcolormap[skintable[t >> 16][s >> 16]]
	move.l	RT_NEW+V_T(sp),d3
	swap	d3
	ext.l	d3
	lea	_skintable,a0
	move.l	(a0,d3.l*4),a0
	move.l	RT_NEW+V_S(sp),d3
	swap	d3
	ext.l	d3
	moveq	#0,d1
	move.b	(a0,d3.l),d1
	move.l	_d_pcolormap,a0
	if PIX16
	move.w	(a0,d1.l*2),d1
	else
	move.b	(a0,d1.l),d1
	endif
	; d_viewbuffer[d_scantable[y] + x] = pix
	lea	_d_scantable,a0
	add.l	(a0,d2.l*4),d0
	move.l	_d_viewbuffer,a0
	if PIX16
	move.w	d1,(a0,d0.l*2)
	else
	move.b	d1,(a0,d0.l)
	endif

.nodraw
	; recurse on (lp3, lp1, new), then (lp3, new, lp2)
	move.l	RT_LP3(sp),a0
	move.l	RT_LP1(sp),a1
	lea	RT_NEW(sp),a2
	bsr	.tri
	move.l	RT_LP3(sp),a0
	lea	RT_NEW(sp),a1
	move.l	RT_LP2(sp),a2
	bsr	.tri
	lea	RT_SIZE(sp),sp
	rts

	section	BSS,bss

	cnop	0,4
v_pkg	ds.l	1				; the span package being drawn
v_steps	ds.l	4				; texel steps by carries: -2s + t = -2..1
v_tstep	ds.l	1				; a_tstepxfrac << 16

	end
