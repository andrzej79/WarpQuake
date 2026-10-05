;
; r_draw060.s -- edge clipping and emission for the 68060
;
; R_ClipEdge with R_EmitEdge folded in, replacing both C functions in
; r_draw.c (built with WQ_ASM=1).  Together they were ~8.5% of the frame
; (hardware profile, 2026-10-04): every visible world and brush-model edge is
; clipped against the view's side planes, projected, and inserted into the
; edge lists of the scan lines it crosses.
;
; The float work is vbcc's own code for the C, operation for operation and
; with the same single-precision rounding points (the stores to float
; variables), so the result is the C's exactly; -crc checks it.  What changes
; is the frame around it, which was most of the cost besides the divides:
;
; - The C recursed: R_ClipEdge called itself once per clipping plane and
;   then called R_EmitEdge, each call saving and restoring up to 7 integer
;   and 5 FPU registers.  Every recursive call is a tail call, so here it is
;   a loop, and the emission is inline - one register save for the whole
;   edge.  Each level's clipped vertex goes into one of three buffers, never
;   one that pv0 or pv1 points into: after two clips both ends can be clip
;   vertices from earlier levels (two buffers were not enough - a third clip
;   overwrote the end it was computed from).
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1/fp0-fp1
; scratch, everything else preserved.
;

	machine	68060

	xref	_cacheoffset		; unsigned: edge cache state of this edge
	xref	_r_framecount
	xref	_r_leftclipped		; qboolean
	xref	_r_rightclipped
	xref	_r_leftenter		; mvertex_t: where the edge crosses the left ...
	xref	_r_leftexit
	xref	_r_rightenter		; ... and the right plane
	xref	_r_rightexit
	xref	_r_lastvertvalid	; qboolean: r_u1, r_v1, r_lzi1, r_ceilv1 are pv0's
	xref	_r_u1				; float: the last emitted vertex, projected
	xref	_r_v1
	xref	_r_lzi1
	xref	_r_ceilv1			; int
	xref	_r_nearzi			; float: nearest 1/z of the surface
	xref	_r_nearzionly		; qboolean: right edges only update r_nearzi
	xref	_r_emitted			; int
	xref	_r_pedge			; medge_t *: the edge's owner
	xref	_edge_p				; edge_t *: next free edge
	xref	_surface_p			; surf_t *: the surface being built
	xref	_surfaces
	xref	_newedges			; edge_t *[MAXHEIGHT]: edges starting per line
	xref	_removeedges		; edge_t *[MAXHEIGHT]: edges ending per line
	xref	_modelorg			; vec3_t: eye in the model's frame
	xref	_vright				; vec3_t: view axes
	xref	_vup
	xref	_vpn
	xref	_xscale				; float
	xref	_yscale
	xref	_xcenter
	xref	_ycenter
	xref	_r_refdef			; refdef_t; clamps at the RD_* offsets
	xref	_r_projverts		; projvert_t *: projected world vertices
	xref	_r_projvertbase		; mvertex_t *: the world's vertices ...
	xref	_r_projlimit		; ... and their size in bytes
	xref	_r_projstamp		; int: entries with this stamp are valid
	xref	_r_projhits			; unsigned long: profile counters
	xref	_r_projmisses

	xdef	_R_ClipEdge

; clipplane_t (r_local.h)
CP_DIST		equ	12
CP_NEXT		equ	16
CP_LEFTEDGE	equ	20			; byte
CP_RIGHTEDGE	equ	21			; byte

; refdef_t (render.h): the projection clamps
RD_FVRECTX_ADJ		equ	68
RD_FVRECTY_ADJ		equ	72
RD_VRECT_X_ADJ_SHIFT20	equ	76
RD_VRECTRIGHT_ADJ_SHIFT20	equ	80
RD_FVRECTRIGHT_ADJ	equ	84
RD_FVRECTBOTTOM_ADJ	equ	88

; edge_t (r_shared.h)
E_U		equ	0
E_USTEP		equ	4
E_NEXT		equ	12
E_SURF0		equ	16
E_SURF1		equ	18
E_NEXTREMOVE	equ	20
E_NEARZI	equ	24
E_OWNER		equ	28
E_SIZE		equ	32

; projvert_t (r_local.h)
PV_U		equ	0
PV_V		equ	4
PV_LZI		equ	8
PV_STAMP	equ	12

FULLY_CLIPPED_CACHED	equ	$80000000
FRAMECOUNT_MASK		equ	$7FFFFFFF

	section	CODE,code

;------------------------------------------------------------------------------
; ClipVert from, to, buffer: the clipped vertex from + f * (to - from) into
; buffer, f = d0 / (d0 - d1) with d0 in fp3 and d1 in fp4 - vbcc's sequence.
; Uses fp0-fp2.
;------------------------------------------------------------------------------

ClipVert	macro
	fmove.x	fp3,fp1
	fsub.x	fp4,fp1
	fmove.x	fp1,fp0
	fmove.x	fp3,fp1
	fdiv.x	fp0,fp1				; f
	fmove.s	(\2),fp2
	fmove.s	(\1),fp0
	fsub.x	fp0,fp2
	fmul.x	fp1,fp2
	fadd.x	fp0,fp2
	fmove.s	fp2,(\3)
	fmove.s	4(\2),fp2
	fmove.s	4(\1),fp0
	fsub.x	fp0,fp2
	fmul.x	fp1,fp2
	fadd.x	fp0,fp2
	fmove.s	fp2,4(\3)
	fmove.s	8(\2),fp2
	fmove.s	8(\1),fp0
	fsub.x	fp0,fp2
	fmul.x	fp2,fp1
	fadd.x	fp0,fp1
	fmove.s	fp1,8(\3)
	endm

; CopyVert buffer, dest: a 12-byte vertex (r_leftexit = clipvert ...)
CopyVert	macro
	move.l	(\1),\2
	move.l	4(\1),4+\2
	move.l	8(\1),8+\2
	endm

;------------------------------------------------------------------------------
; Ceil fpN, dN: (int) ceil (x) the way vbcc inlines it (math_060.h).
; Uses fp1.
;------------------------------------------------------------------------------

Ceil	macro
	fmove.x	\1,fp1
	fintrz.x	\1
	fcmp.x	fp1,\1
	fboge	.c\@
	fadd.s	#$3f800000,\1
.c\@
	fintrz.x	\1,\1
	fmove.l	\1,\2
	endm

; ProjectInto vertex, out: R_ProjectVertex (r_draw.c), vbcc's code for it -
; local = vertex - modelorg and transformed = its dot products with vright,
; vup, vpn, each rounded to a float as the C's vec3_t locals are (v_loc,
; v_tr), z clamped to NEAR_CLIP, then u, v clamped to the view and 1/z,
; stored as floats at out.  Registers renamed from vbcc's so as not to touch
; fp2, fp4, fp5 (the first vertex): fp6 = z, fp3 = 1/z, fp7 = v.
; Uses fp0, fp1, fp3, fp6, fp7.
ProjectInto	macro
	fmove.s	(\1),fp0
	fsub.s	_modelorg,fp0
	fmove.s	fp0,v_loc
	fmove.s	4(\1),fp0
	fsub.s	4+_modelorg,fp0
	fmove.s	fp0,v_loc+4
	fmove.s	8(\1),fp0
	fsub.s	8+_modelorg,fp0
	fmove.s	fp0,v_loc+8
	fmove.s	_vright,fp0
	fmul.s	v_loc,fp0
	fmove.s	4+_vright,fp1
	fmul.s	v_loc+4,fp1
	fadd.x	fp1,fp0
	fmove.s	8+_vright,fp1
	fmul.s	v_loc+8,fp1
	fadd.x	fp0,fp1
	fmove.s	fp1,v_tr
	fmove.s	_vup,fp0
	fmul.s	v_loc,fp0
	fmove.s	4+_vup,fp1
	fmul.s	v_loc+4,fp1
	fadd.x	fp1,fp0
	fmove.s	8+_vup,fp1
	fmul.s	v_loc+8,fp1
	fadd.x	fp0,fp1
	fmove.s	fp1,v_tr+4
	fmove.s	_vpn,fp0
	fmul.s	v_loc,fp0
	fmove.s	4+_vpn,fp1
	fmul.s	v_loc+4,fp1
	fadd.x	fp1,fp0
	fmove.s	8+_vpn,fp1
	fmul.s	v_loc+8,fp1
	fadd.x	fp0,fp1
	fmove.s	fp1,v_tr+8
	fmove.s	v_tr+8,fp0
	fcmp.d	#$3f847ae147ae147b,fp0		; NEAR_CLIP 0.01
	fbge	.nc\@
	move.l	#$3c23d70a,v_tr+8		; 0.01f
.nc\@
	fmove.s	v_tr+8,fp0
	fmove.x	fp0,fp6
	fmove.d	#$3ff0000000000000,fp0
	fdiv.x	fp6,fp0				; 1.0 / z
	fmove.x	fp0,fp3
	fmove.s	_xscale,fp0
	fmul.x	fp3,fp0
	fmul.s	v_tr,fp0
	fmove.s	_xcenter,fp1
	fadd.x	fp0,fp1				; u
	fcmp.s	RD_FVRECTX_ADJ+_r_refdef,fp1
	fbge	.ul\@
	fmove.s	RD_FVRECTX_ADJ+_r_refdef,fp1
.ul\@
	fcmp.s	RD_FVRECTRIGHT_ADJ+_r_refdef,fp1
	fble	.uh\@
	fmove.s	RD_FVRECTRIGHT_ADJ+_r_refdef,fp1
.uh\@
	fmove.s	_yscale,fp0
	fmul.x	fp3,fp0
	fmul.s	v_tr+4,fp0
	fmove.s	_ycenter,fp7
	fsub.x	fp0,fp7				; v
	fcmp.s	RD_FVRECTY_ADJ+_r_refdef,fp7
	fbge	.vl\@
	fmove.s	RD_FVRECTY_ADJ+_r_refdef,fp7
.vl\@
	fcmp.s	RD_FVRECTBOTTOM_ADJ+_r_refdef,fp7
	fble	.vh\@
	fmove.s	RD_FVRECTBOTTOM_ADJ+_r_refdef,fp7
.vh\@
	fmove.s	fp1,(\2)
	fmove.s	fp7,4(\2)
	fmove.s	fp3,8(\2)
	endm

; Projected vertex: a0 = R_ProjectedVertex (vertex) - its entry in
; r_projverts, projected now unless already done under this modelorg/view
; (stamp), or v_ptemp for a clipped vertex.  An entry is 24 bytes, twice an
; mvertex_t, so its offset is twice the vertex's.  Uses d0 and ProjectInto's.
Projected	macro
	move.l	\1,d0
	sub.l	_r_projvertbase,d0
	cmp.l	_r_projlimit,d0
	bcc	.temp\@				; not a world vertex (or below the array)
	add.l	d0,d0
	move.l	_r_projverts,a0
	add.l	d0,a0
	move.l	PV_STAMP(a0),d0
	cmp.l	_r_projstamp,d0
	bne	.miss\@
	addq.l	#1,_r_projhits
	bra	.done\@
.miss\@
	ProjectInto	\1,a0
	move.l	_r_projstamp,PV_STAMP(a0)
	addq.l	#1,_r_projmisses
	bra	.done\@
.temp\@
	lea	v_ptemp,a0
	ProjectInto	\1,a0
.done\@
	endm

;------------------------------------------------------------------------------
; FreeBuffer: a3 = the first of the three v_cv buffers that neither pv0 (a5)
; nor pv1 (a6) points into.  Uses a0.
;------------------------------------------------------------------------------

FreeBuffer	macro
	lea	v_cv,a3
	cmpa.l	a3,a5
	beq.s	.used\@
	cmpa.l	a3,a6
	bne.s	.free\@
.used\@
	lea	12(a3),a3
	cmpa.l	a3,a5
	beq.s	.used2\@
	cmpa.l	a3,a6
	bne.s	.free\@
.used2\@
	lea	12(a3),a3
	cmpa.l	a3,a5
	beq.s	.used\@			; (cannot happen twice: two pointers,
	cmpa.l	a3,a6				;  three buffers)
	beq.s	.used\@
.free\@
	endm

;------------------------------------------------------------------------------
; void R_ClipEdge (mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip)
;
;   a5 = pv0   a6 = pv1   a4 = clip   fp5 = 0.0 (while clipping)
;   a3 = the buffer this level's clipped vertex goes into (FreeBuffer)
;------------------------------------------------------------------------------

	cnop	0,4
_R_ClipEdge
	; Only what the clip loop needs is saved here (FMOVEM costs 1+3n
	; cycles); an edge that survives clipping saves the rest in .emit.
	movem.l	a3-a6,-(sp)
	fmovem.x	fp2-fp5,-(sp)
	move.l	4+4*4+4*12(sp),a5
	move.l	8+4*4+4*12(sp),a6
	move.l	12+4*4+4*12(sp),a4
	fmove.s	#$00000000,fp5
	move.l	a4,d0
	beq	.emit

.plane
	; d0, d1: the two ends' distances to this plane (kept extended, as vbcc
	; keeps them in registers)
	fmove.s	(a5),fp1
	fmove.s	(a4),fp0
	fmul.x	fp0,fp1
	fmove.s	4(a4),fp2
	fmul.s	4(a5),fp2
	fadd.x	fp2,fp1
	fmove.s	8(a4),fp2
	fmul.s	8(a5),fp2
	fadd.x	fp2,fp1
	fmove.x	fp1,fp3
	fsub.s	CP_DIST(a4),fp3			; d0
	fmul.s	(a6),fp0
	fmove.s	4(a4),fp1
	fmul.s	4(a6),fp1
	fadd.x	fp1,fp0
	fmove.s	8(a4),fp1
	fmul.s	8(a6),fp1
	fadd.x	fp1,fp0
	fmove.x	fp0,fp4
	fsub.s	CP_DIST(a4),fp4			; d1
	fcmp.x	fp3,fp5
	fbgt	.p0clipped
	fcmp.x	fp4,fp5
	fble	.nextplane			; both ends in front: unclipped here

	; only pv1 is clipped: clip it, then go on with pv0 .. clipvert
	move.l	#$7fffffff,_cacheoffset		; clipped edges are not cached
	FreeBuffer
	ClipVert	a5,a6,a3
	tst.b	CP_LEFTEDGE(a4)
	beq	.notleft1
	move.l	#1,_r_leftclipped
	CopyVert	a3,_r_leftexit
	bra	.go1
.notleft1
	tst.b	CP_RIGHTEDGE(a4)
	beq	.go1
	move.l	#1,_r_rightclipped
	CopyVert	a3,_r_rightexit
.go1
	move.l	a3,a6				; pv1 = clipvert
	bra	.nextlevel

.p0clipped
	fcmp.x	fp4,fp5
	fble	.only0
	; both ends clipped: done; a fully clipped edge is cached
	tst.l	_r_leftclipped
	bne	.done
	move.l	_r_framecount,d0
	and.l	#FRAMECOUNT_MASK,d0
	or.l	#FULLY_CLIPPED_CACHED,d0
	move.l	d0,_cacheoffset
	bra	.done

.only0
	; only pv0 is clipped: clip it, then go on with clipvert .. pv1
	clr.l	_r_lastvertvalid
	move.l	#$7fffffff,_cacheoffset
	FreeBuffer
	ClipVert	a5,a6,a3
	tst.b	CP_LEFTEDGE(a4)
	beq	.notleft0
	move.l	#1,_r_leftclipped
	CopyVert	a3,_r_leftenter
	bra	.go0
.notleft0
	tst.b	CP_RIGHTEDGE(a4)
	beq	.go0
	move.l	#1,_r_rightclipped
	CopyVert	a3,_r_rightenter
.go0
	move.l	a3,a5				; pv0 = clipvert

.nextlevel
.nextplane
	move.l	CP_NEXT(a4),a4
	move.l	a4,d0
	bne	.plane

;------------------------------------------------------------------------------
; R_EmitEdge (pv0 = a5, pv1 = a6), vbcc's code with the arguments in
; registers and its locals in v_loc / v_tr / v_u; the vertices through
; Projected (the projection cache):
;   fp5 = u0  fp4 = v0  fp2 = lzi0  d2 = ceilv0  (the first vertex)
;------------------------------------------------------------------------------

.emit
	movem.l	d2-d5/a2,-(sp)
	fmovem.x	fp6-fp7,-(sp)
	tst.l	_r_lastvertvalid
	beq	.project0
	fmove.s	_r_u1,fp5
	fmove.s	_r_v1,fp4
	fmove.s	_r_lzi1,fp2
	move.l	_r_ceilv1,d2
	bra	.vert1

.project0
	Projected	a5
	fmove.s	PV_U(a0),fp5			; u0
	fmove.s	PV_V(a0),fp4			; v0
	fmove.s	PV_LZI(a0),fp2			; lzi0
	fmove.x	fp4,fp0
	Ceil	fp0,d2				; ceilv0

.vert1
	Projected	a6
	move.l	PV_U(a0),_r_u1
	move.l	PV_V(a0),_r_v1
	move.l	PV_LZI(a0),_r_lzi1
	fcmp.s	_r_lzi1,fp2			; lzi0 = max (lzi0, r_lzi1)
	fbge	.lzimax
	fmove.s	_r_lzi1,fp2
.lzimax
	fcmp.s	_r_nearzi,fp2			; for mipmap finding
	fble	.nearzi
	fmove.s	fp2,_r_nearzi
.nearzi
	tst.l	_r_nearzionly			; right edges: only the 1/z
	bne	.emitdone
	move.l	#1,_r_emitted
	fmove.s	_r_v1,fp0
	Ceil	fp0,d0
	move.l	d0,_r_ceilv1
	cmp.l	d0,d2
	bne	.notflat
	; a horizontal edge: cached as fully clipped (unless it was clipped)
	cmp.l	#$7fffffff,_cacheoffset
	beq	.emitdone
	move.l	#FRAMECOUNT_MASK,d0
	and.l	_r_framecount,d0
	or.l	#FULLY_CLIPPED_CACHED,d0
	move.l	d0,_cacheoffset
	bra	.emitdone

.notflat
	; side = ceilv0 > r_ceilv1; create the edge
	move.l	_edge_p,a3
	lea	E_SIZE(a3),a0
	move.l	a0,_edge_p
	move.l	_r_pedge,E_OWNER(a3)
	fmove.s	fp2,E_NEARZI(a3)
	move.l	_surface_p,d0
	sub.l	_surfaces,d0
	asr.l	#6,d0				; surface_p - surfaces
	cmp.l	_r_ceilv1,d2
	bgt	.leading

	; trailing edge (go from p1 to p2)
	move.l	d2,d3				; v
	move.l	_r_ceilv1,d4
	subq.l	#1,d4				; v2
	move.w	d0,E_SURF0(a3)
	clr.w	E_SURF1(a3)
	fmove.s	_r_u1,fp0
	fsub.x	fp5,fp0
	fmove.s	_r_v1,fp1
	fsub.x	fp4,fp1
	fmove.x	fp0,fp6
	fdiv.x	fp1,fp6				; u_step
	fmove.l	d2,fp1
	fsub.x	fp4,fp1
	fmul.x	fp6,fp1
	fadd.x	fp5,fp1
	fmove.s	fp1,v_u				; u
	bra	.fixed

.leading
	; leading edge (go from p2 to p1)
	move.l	d2,d4
	subq.l	#1,d4				; v2
	move.l	_r_ceilv1,d3			; v
	clr.w	E_SURF0(a3)
	move.w	d0,E_SURF1(a3)
	fmove.s	_r_u1,fp1
	fmove.x	fp5,fp3
	fsub.x	fp1,fp3
	fmove.s	_r_v1,fp0
	fmove.x	fp4,fp2
	fsub.x	fp0,fp2
	fmove.x	fp3,fp6
	fdiv.x	fp2,fp6				; u_step
	fmove.l	d3,fp2
	fneg.x	fp0
	fadd.x	fp2,fp0
	fmul.x	fp6,fp0
	fadd.x	fp1,fp0
	fmove.s	fp0,v_u				; u

.fixed
	; to 12.20 fixed point, u biased up by just under one
	fmove.x	fp6,fp1
	fmul.s	#$49800000,fp1			; * 0x100000
	fintrz.x	fp1,fp1
	fmove.l	fp1,E_USTEP(a3)
	fmove.s	v_u,fp1
	fmul.s	#$49800000,fp1
	fadd.s	#$497ffff0,fp1			; + 0xFFFFF
	fintrz.x	fp1,fp1
	fmove.l	fp1,d1
	cmp.l	RD_VRECT_X_ADJ_SHIFT20+_r_refdef,d1
	bge	.ulo
	move.l	RD_VRECT_X_ADJ_SHIFT20+_r_refdef,d1
.ulo
	cmp.l	RD_VRECTRIGHT_ADJ_SHIFT20+_r_refdef,d1
	ble	.uhi
	move.l	RD_VRECTRIGHT_ADJ_SHIFT20+_r_refdef,d1
.uhi
	move.l	d1,E_U(a3)

	; sort it into newedges[v] (trailers after leaders at the same u)
	tst.w	E_SURF0(a3)
	beq	.ucheck
	addq.l	#1,d1				; u_check
.ucheck
	lea	_newedges,a0
	lea	(a0,d3.l*4),a0			; &newedges[v]
	move.l	(a0),d0
	beq	.head
	move.l	d0,a2
	cmp.l	E_U(a2),d1
	ble	.head
.walk
	move.l	E_NEXT(a2),d0
	beq	.insert
	move.l	d0,a1
	cmp.l	E_U(a1),d1
	ble	.insert
	move.l	a1,a2
	bra	.walk
.insert
	move.l	E_NEXT(a2),E_NEXT(a3)
	move.l	a3,E_NEXT(a2)
	bra	.remove
.head
	move.l	(a0),E_NEXT(a3)
	move.l	a3,(a0)
.remove
	lea	_removeedges,a0
	lea	(a0,d4.l*4),a0
	move.l	(a0),E_NEXTREMOVE(a3)
	move.l	a3,(a0)

.emitdone
	fmovem.x	(sp)+,fp6-fp7
	movem.l	(sp)+,d2-d5/a2
.done
	fmovem.x	(sp)+,fp2-fp5
	movem.l	(sp)+,a3-a6
	rts

;------------------------------------------------------------------------------

	section	BSS,bss

	cnop	0,4
v_cv	ds.l	9				; three clip vertex buffers, 12 bytes each
v_loc	ds.l	3				; local[] of the vertex being projected
v_tr	ds.l	3				; transformed[]
v_u	ds.l	1				; the edge's u before fixing
v_ptemp	ds.l	3				; a clipped vertex, projected

	end
