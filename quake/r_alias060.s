;
; r_alias060.s -- alias model vertex transform for the 68060
;
; R_AliasTransformAndProjectFinalVerts, replacing the C in r_alias.c (built
; with WQ_ASM=1): every vertex of a model that needs no clipping is
; transformed into view space, projected, and lit.  The model pipeline was
; ~1.7 ms more than ClickBOOM's (attach profile, 2026-10-04); this is the
; part of it that runs once per vertex.
;
; The float work is vbcc's own code for the C, operation for operation: each
; output is ((v0*m0 + v1*m1) + v2*m2) + m3 in extended precision, the
; products exact (a byte times a float), so the result is the C's exactly;
; -crc checks it.  What changes is how the operands get there:
;
; - The three vertex bytes are converted once, through r_bytefloat[] (the
;   256 byte values as floats), instead of nine times through FMOVE.L from a
;   data register - an integer source costs the FPU three extra cycles.
; - The light comes from r_alightcache[], per normal index (r_alias.c);
;   only a miss runs the C's dot product.
; - The 37-cycle divide for 1/z overlaps the integer work of the vertex
;   (the s/t copy and the light lookup): the 68060 FPU is not pipelined,
;   but the integer pipes keep going until the next FP instruction.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1/fp0-fp1
; scratch, everything else preserved.
;

	machine	68060

	xref	_r_apverts			; trivertx_t *: this frame's vertices
	xref	_r_anumverts		; int
	xref	_aliastransform		; float [3][4]
	xref	_aliasxcenter		; float
	xref	_aliasycenter
	xref	_r_bytefloat		; float [256]
	xref	_r_alightcache		; alightcache_t [162]: {stamp, light}
	xref	_r_alightstamp		; int
	xref	_r_avertexnormals	; float [162][3]
	xref	_r_plightvec		; vec3_t
	xref	_r_ambientlight		; int
	xref	_r_shadelight		; float

	xref	_aliasxscale		; float
	xref	_aliasyscale
	xref	_ziscale
	xref	_r_refdef			; refdef_t

	xdef	_R_AliasTransformAndProjectFinalVerts
	xdef	_R_AliasTransformClipVerts

; finalvert_t (d_iface.h), 32 bytes: int v[6] (u, v, s, t, light, 1/z), flags
FV_U		equ	0
FV_V		equ	4
FV_S		equ	8
FV_T		equ	12
FV_LIGHT	equ	16
FV_ZI		equ	20
FV_FLAGS	equ	24
FV_SIZE		equ	32
; stvert_t (modelgen.h), 12 bytes: onseam, s, t
ST_ONSEAM	equ	0
ST_S		equ	4
ST_T		equ	8
ST_SIZE		equ	12
; trivertx_t: v[3] bytes, lightnormalindex
TV_NORMAL	equ	3

	section	CODE,code

;------------------------------------------------------------------------------
; void R_AliasTransformAndProjectFinalVerts (finalvert_t *fv,
;                                            stvert_t *pstverts)
;
;   a0 = fv   a1 = pverts   a2 = pstverts   a3 = r_bytefloat
;   a4 = r_alightcache   a5 = aliastransform
;   d7 = vertices left   d6 = r_alightstamp   a6, d1, d4 = light lookup
;   fp0-fp2 = the vertex as floats   fp3, fp4 = accumulators   fp5 = 1.0
;   fp6 = 1/z
;------------------------------------------------------------------------------

; One row of the transform: fpacc = ((v0*m0 + v1*m1) + v2*m2) + m3, for row
; \1 (byte offset of the row).  The C's order: each product into a register,
; the second added to the first, then the third, then the constant.
TransformRow	macro
	fmove.s	\1+0(a5),\2
	fmul.x	fp0,\2
	fmove.s	\1+4(a5),fp4
	fmul.x	fp1,fp4
	fadd.x	fp4,\2
	fmove.s	\1+8(a5),fp4
	fmul.x	fp2,fp4
	fadd.x	fp4,\2
	fadd.s	\1+12(a5),\2
	endm

	cnop	0,4
_R_AliasTransformAndProjectFinalVerts
	movem.l	d2-d7/a2-a6,-(sp)
	fmovem.x	fp2-fp7,-(sp)
.args	equ	11*4+6*12+4		; past the saves and the return address
	move.l	.args(sp),a0
	move.l	.args+4(sp),a2
	move.l	_r_apverts,a1
	lea	_r_bytefloat,a3
	lea	_r_alightcache,a4
	lea	_aliastransform,a5
	move.l	_r_alightstamp,d6
	move.l	_r_anumverts,d7
	ble	.done
	fmove.s	#$3f800000,fp5		; 1.0
	moveq	#0,d0
	moveq	#0,d2
	moveq	#0,d3

.vertex
	; the three bytes as floats
	move.b	(a1),d0
	move.b	1(a1),d2
	move.b	2(a1),d3
	fmove.s	(a3,d0.l*4),fp0
	fmove.s	(a3,d2.l*4),fp1
	fmove.s	(a3,d3.l*4),fp2

	; 1/z first, so its divide overlaps the integer work below
	TransformRow	32,fp3
	fmove.x	fp5,fp6
	fdiv.x	fp3,fp6

	; s, t and the seam flag (integer, under the divide)
	move.l	ST_S(a2),FV_S(a0)
	move.l	ST_T(a2),FV_T(a0)
	move.l	ST_ONSEAM(a2),FV_FLAGS(a0)
	lea	ST_SIZE(a2),a2

	; the light, cached per normal index
	moveq	#0,d1
	move.b	TV_NORMAL(a1),d1
	lea	(a4,d1.l*8),a6
	cmp.l	(a6),d6
	bne	.lightmiss
	move.l	4(a6),FV_LIGHT(a0)
.lit
	addq.l	#4,a1

	; fv->v[5] = (int)zi; then u and v, projected: (row * zi) + center
	fintrz.x	fp6,fp3
	fmove.l	fp3,FV_ZI(a0)
	TransformRow	0,fp3
	fmul.x	fp6,fp3
	fadd.s	_aliasxcenter,fp3
	fintrz.x	fp3
	fmove.l	fp3,FV_U(a0)
	TransformRow	16,fp3
	fmul.x	fp6,fp3
	fadd.s	_aliasycenter,fp3
	fintrz.x	fp3
	fmove.l	fp3,FV_V(a0)

	lea	FV_SIZE(a0),a0
	subq.l	#1,d7
	bne	.vertex

.done
	fmovem.x	(sp)+,fp2-fp7
	movem.l	(sp)+,d2-d7/a2-a6
	rts

.lightmiss
	bsr	VertexLightMiss
	move.l	d1,FV_LIGHT(a0)
	bra	.lit

;------------------------------------------------------------------------------
; VertexLightMiss: a normal not seen yet for this entity - the C's light,
; into the cache entry.  In: d1 = the normal index, a6 = its r_alightcache
; entry, d6 = r_alightstamp.  Out: d1 = the light.  Uses d4, fp3, fp4.
;
;   lightcos = (plv0*n0 + plv1*n1) + plv2*n2
;   temp = ambient; if (lightcos < 0) { temp += (int)(shadelight * lightcos);
;   if (temp < 0) temp = 0; }
;------------------------------------------------------------------------------

VertexLightMiss
	move.l	a6,-(sp)
	move.l	d1,d4
	add.l	d1,d1
	add.l	d4,d1				; index * 3 ...
	lea	_r_avertexnormals,a6
	lea	(a6,d1.l*4),a6		; ... floats
	fmove.s	_r_plightvec,fp3
	fmul.s	(a6),fp3
	fmove.s	_r_plightvec+4,fp4
	fmul.s	4(a6),fp4
	fadd.x	fp4,fp3
	fmove.s	_r_plightvec+8,fp4
	fmul.s	8(a6),fp4
	fadd.x	fp3,fp4
	move.l	(sp)+,a6
	move.l	_r_ambientlight,d1
	ftst.x	fp4
	fbge	.lightpos
	fmove.s	_r_shadelight,fp3
	fmul.x	fp4,fp3
	fintrz.x	fp3
	fmove.l	fp3,d4
	add.l	d4,d1
	bge.s	.lightpos
	moveq	#0,d1
.lightpos
	move.l	d6,(a6)
	move.l	d1,4(a6)
	rts

;------------------------------------------------------------------------------
; void R_AliasTransformClipVerts (finalvert_t *fv, auxvert_t *av,
;                                 stvert_t *pstverts, int numverts)
;
; The vertex loop of R_AliasPreparePoints (models that need clipping, which
; is nearly always the weapon): R_AliasTransformFinalVert, then either the
; z-clip flag or R_AliasProjectFinalVert and the four screen-edge flags.
; r_apverts ends past the vertices, as in the C.
;
; The view-space position goes through av->fv[] (floats in memory, rounded
; there as in the C) and the projection reads it back from there.
;
;   a0 = fv   a1 = pverts   a2 = pstverts   a3 = r_bytefloat   a4 = av
;   a5 = aliastransform   d7 = vertices left   d6 = r_alightstamp
;   a6, d1, d4 = light lookup   fp5 = 1.0   fp6 = 1/z   fp7 = 5.0, the
;   z clip plane
;------------------------------------------------------------------------------

AV_SIZE		equ	12
RD_ALIASVRECT_X		equ	20		; refdef_t (render.h): aliasvrect.x
RD_ALIASVRECT_Y		equ	24
RD_ALIASVRECTRIGHT	equ	48
RD_ALIASVRECTBOTTOM	equ	52
ALIAS_LEFT_CLIP		equ	$01		; r_shared.h
ALIAS_TOP_CLIP		equ	$02
ALIAS_RIGHT_CLIP	equ	$04
ALIAS_BOTTOM_CLIP	equ	$08
ALIAS_Z_CLIP		equ	$10

	cnop	0,4
_R_AliasTransformClipVerts
	movem.l	d2-d7/a2-a6,-(sp)
	fmovem.x	fp2-fp7,-(sp)
.args	equ	11*4+6*12+4
	move.l	.args(sp),a0
	move.l	.args+4(sp),a4
	move.l	.args+8(sp),a2
	move.l	.args+12(sp),d7
	move.l	_r_apverts,a1
	lea	_r_bytefloat,a3
	lea	_aliastransform,a5
	move.l	_r_alightstamp,d6
	tst.l	d7
	ble	.done
	fmove.s	#$3f800000,fp5		; 1.0
	fmove.s	#$40a00000,fp7		; ALIAS_Z_CLIP_PLANE
	moveq	#0,d0
	moveq	#0,d2
	moveq	#0,d3

.vertex
	move.b	(a1),d0
	move.b	1(a1),d2
	move.b	2(a1),d3
	fmove.s	(a3,d0.l*4),fp0
	fmove.s	(a3,d2.l*4),fp1
	fmove.s	(a3,d3.l*4),fp2
	TransformRow	0,fp3
	fmove.s	fp3,(a4)
	TransformRow	16,fp3
	fmove.s	fp3,4(a4)
	TransformRow	32,fp3
	fmove.s	fp3,8(a4)

	move.l	ST_S(a2),FV_S(a0)
	move.l	ST_T(a2),FV_T(a0)
	move.l	ST_ONSEAM(a2),FV_FLAGS(a0)
	lea	ST_SIZE(a2),a2
	moveq	#0,d1
	move.b	TV_NORMAL(a1),d1
	lea	_r_alightcache,a6
	lea	(a6,d1.l*8),a6
	cmp.l	(a6),d6
	bne	.lightmiss
	move.l	4(a6),FV_LIGHT(a0)
.lit
	addq.l	#4,a1

	; behind the z clip plane (or NaN): flag it, no projection
	fcmp.s	8(a4),fp7
	fble	.project
	moveq	#ALIAS_Z_CLIP,d1
	or.l	d1,FV_FLAGS(a0)
	bra	.next

.project
	fmove.x	fp5,fp6
	fdiv.s	8(a4),fp6			; zi = 1.0 / av->fv[2]
	fmove.s	_ziscale,fp3
	fmul.x	fp6,fp3
	fintrz.x	fp3
	fmove.l	fp3,FV_ZI(a0)
	fmove.s	_aliasxscale,fp3
	fmul.s	(a4),fp3
	fmul.x	fp6,fp3
	fadd.s	_aliasxcenter,fp3
	fintrz.x	fp3
	fmove.l	fp3,d0
	move.l	d0,FV_U(a0)
	fmove.s	_aliasyscale,fp3
	fmul.s	4(a4),fp3
	fmul.x	fp6,fp3
	fadd.s	_aliasycenter,fp3
	fintrz.x	fp3
	fmove.l	fp3,d1
	move.l	d1,FV_V(a0)

	; the screen-edge flags
	moveq	#0,d4
	cmp.l	_r_refdef+RD_ALIASVRECT_X,d0
	bge.s	.l
	moveq	#ALIAS_LEFT_CLIP,d4
.l	cmp.l	_r_refdef+RD_ALIASVRECTRIGHT,d0
	ble.s	.r
	addq.l	#ALIAS_RIGHT_CLIP,d4
.r	cmp.l	_r_refdef+RD_ALIASVRECT_Y,d1
	bge.s	.t
	addq.l	#ALIAS_TOP_CLIP,d4
.t	cmp.l	_r_refdef+RD_ALIASVRECTBOTTOM,d1
	ble.s	.b
	addq.l	#ALIAS_BOTTOM_CLIP,d4
.b	or.l	d4,FV_FLAGS(a0)
	moveq	#0,d0				; the byte loads need its upper bits 0

.next
	lea	FV_SIZE(a0),a0
	lea	AV_SIZE(a4),a4
	subq.l	#1,d7
	bne	.vertex
	move.l	a1,_r_apverts

.done
	fmovem.x	(sp)+,fp2-fp7
	movem.l	(sp)+,d2-d7/a2-a6
	rts

.lightmiss
	bsr	VertexLightMiss
	move.l	d1,FV_LIGHT(a0)
	bra	.lit
