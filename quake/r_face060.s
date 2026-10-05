;
; r_face060.s -- R_RenderFace for the 68060
;
; R_RenderFace, replacing the C in r_draw.c (built with WQ_ASM=1): one world
; surface into the edge lists.  Each of its edges is either reused (an edge
; already emitted this frame by the surface on its other side, or one known
; to be off screen) or clipped and emitted by R_ClipEdge (r_draw060.s); a
; surface that produced any edge gets its surf_t, with the 1/z gradients of
; its plane.  It was ~1.6 ms a frame in C (hardware profile, 2026-10-04),
; spread evenly over the edge loop: vbcc reloaded currententity->model,
; the surfedges pointer and r_pedge from memory for every edge, and called
; R_EmitCachedEdge for the reused ones.
;
; Here the loop keeps the model's arrays in registers and does the reuse
; inline.  The float work at the end (the plane's gradients) is vbcc's code
; for the C, operation for operation, including its single-precision stores
; of p_normal; -crc checks it.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1/fp0-fp1
; scratch, everything else preserved.
;

	machine	68060

	xref	_surface_p			; surf_t *: next free surface ...
	xref	_surf_max			; ... and the end of them
	xref	_surfaces			; surf_t *: index base for edge surfs[]
	xref	_r_outofsurfaces	; int
	xref	_edge_p				; edge_t *: next free edge ...
	xref	_edge_max			; ... and the end of them
	xref	_r_edges			; edge_t *: cached edge offsets count from here
	xref	_r_outofedges		; int
	xref	_c_faceclip			; int
	xref	_view_clipplanes	; clipplane_t [4]
	xref	_r_emitted			; int
	xref	_r_nearzi			; float
	xref	_r_nearzionly		; qboolean
	xref	_r_lastvertvalid	; qboolean
	xref	_r_pedge			; medge_t *
	xref	_cacheoffset		; unsigned: R_ClipEdge's verdict on this edge
	xref	_r_leftclipped		; qboolean
	xref	_r_rightclipped
	xref	_r_leftenter		; mvertex_t
	xref	_r_leftexit
	xref	_r_rightenter
	xref	_r_rightexit
	xref	_r_pcurrentvertbase	; mvertex_t *
	xref	_currententity		; entity_t *
	xref	_insubmodel			; qboolean
	xref	_r_framecount		; int
	xref	_r_polycount		; int
	xref	_r_currentkey		; int
	xref	_vright				; vec3_t
	xref	_vup
	xref	_vpn
	xref	_modelorg
	xref	_xscaleinv			; float
	xref	_yscaleinv
	xref	_xcenter
	xref	_ycenter
	xref	_R_ClipEdge			; r_draw060.s

	xdef	_R_RenderFace

; msurface_t (model.h)
MS_PLANE		equ	12
MS_FLAGS		equ	16
MS_FIRSTEDGE	equ	20
MS_NUMEDGES		equ	24
; medge_t: unsigned short v[2], unsigned cachededgeoffset
ME_V0			equ	0
ME_V1			equ	2
ME_CACHED		equ	4
FULLY_CLIPPED_CACHED	equ	31		; its bit number; the rest is a frame count
; edge_t (r_local.h), 32 bytes
E_SURFS			equ	16			; unsigned short [2]
E_NEARZI		equ	24
E_OWNER			equ	28
E_SIZE			equ	32
; entity_t: model; model_t: edges, surfedges
ENT_MODEL		equ	132
MOD_EDGES		equ	156
MOD_SURFEDGES	equ	188
; clipplane_t: next
CP_NEXT			equ	16
CP_SIZE			equ	24
; surf_t (r_shared.h), 64 bytes
SF_SPANS		equ	8
SF_KEY			equ	12
SF_SPANSTATE	equ	20
SF_FLAGS		equ	24
SF_DATA			equ	28
SF_ENTITY		equ	32
SF_NEARZI		equ	36
SF_INSUBMODEL	equ	40
SF_ZIORIGIN		equ	44
SF_ZISTEPU		equ	48
SF_ZISTEPV		equ	52
SF_SIZE			equ	64
; mplane_t
PL_DIST			equ	12

	section	CODE,code

;------------------------------------------------------------------------------
; void R_RenderFace (msurface_t *fa, int clipflags)
;
;   a3 = fa   a2 = pclip (the clip planes clipflags asks for, chained)
;   a4 = the model's edges   a5 = this surface's next surfedge
;   d4 = edges left   d5 = r_framecount   d6 = makeleftedge, d7 = makerightedge
;   frame: TEDGE (a medge_t for the extra edges), PN (p_normal, 3 floats)
;------------------------------------------------------------------------------

TEDGE	equ	0
PN		equ	8
FRAME	equ	20

; One edge, its vertices from \1 and \2 (ME_V0/ME_V1), r_pedge in a0.
; The cached-edge tests, then R_ClipEdge.
Edge	macro
	move.l	a0,_r_pedge
	tst.l	_insubmodel
	bne	.clip\@
	move.l	ME_CACHED(a0),d0
	bpl	.notfull\@
	; fully clipped already: still this frame?
	bclr	#FULLY_CLIPPED_CACHED,d0
	cmp.l	d5,d0
	bne	.clip\@
	clr.l	_r_lastvertvalid
	bra	.next
.notfull\@
	; emitted before this frame's edges ran past it, and still its own?
	move.l	_edge_p,d1
	move.l	_r_edges,a1
	sub.l	a1,d1
	cmp.l	d0,d1
	bls	.clip\@
	add.l	d0,a1
	cmp.l	E_OWNER(a1),a0
	bne	.clip\@
	bsr	EmitCached
	clr.l	_r_lastvertvalid
	bra	.next
.clip\@
	move.l	_edge_p,d0
	sub.l	_r_edges,d0
	move.l	d0,_cacheoffset
	clr.l	_r_leftclipped
	clr.l	_r_rightclipped
	move.l	_r_pcurrentvertbase,a1
	moveq	#0,d0
	move.w	\2(a0),d0
	mulu.w	#12,d0
	moveq	#0,d1
	move.w	\1(a0),d1
	mulu.w	#12,d1
	move.l	a2,-(sp)
	pea	(a1,d0.l)
	pea	(a1,d1.l)
	jsr	_R_ClipEdge
	lea	12(sp),sp
	move.l	_r_pedge,a0
	move.l	_cacheoffset,ME_CACHED(a0)
	or.l	_r_leftclipped,d6
	or.l	_r_rightclipped,d7
	move.l	#1,_r_lastvertvalid
	bra	.next
	endm

	cnop	0,4
_R_RenderFace
	lea	-FRAME(sp),sp
	fmovem.x	fp2,-(sp)
	movem.l	d2-d7/a2-a6,-(sp)
.args	equ	11*4+12+FRAME+4
	move.l	.args(sp),a3
	move.l	.args+4(sp),d2		; clipflags
	lea	11*4+12(sp),a6		; the frame

	; out of surfaces, or of edges (this one's and four spare)?
	move.l	_surface_p,d0
	cmp.l	_surf_max,d0
	bcs	.surfok
	addq.l	#1,_r_outofsurfaces
	bra	.done
.surfok
	move.l	MS_NUMEDGES(a3),d4
	move.l	d4,d0
	lsl.l	#5,d0				; * sizeof (edge_t)
	add.l	_edge_p,d0
	add.l	#4*E_SIZE,d0
	cmp.l	_edge_max,d0
	bcs	.edgeok
	add.l	d4,_r_outofedges
	bra	.done
.edgeok
	addq.l	#1,_c_faceclip

	; the clip planes, chained 0 -> 3 for those clipflags asks for
	sub.l	a2,a2
	lea	_view_clipplanes+4*CP_SIZE,a0
	moveq	#3,d1
.planes
	lea	-CP_SIZE(a0),a0
	btst	d1,d2
	beq	.noplane
	move.l	a2,CP_NEXT(a0)
	move.l	a0,a2
.noplane
	dbra	d1,.planes

	clr.l	_r_emitted
	clr.l	_r_nearzi
	clr.l	_r_nearzionly
	clr.l	_r_lastvertvalid
	moveq	#0,d6
	moveq	#0,d7
	move.l	_currententity,a0
	move.l	ENT_MODEL(a0),a0
	move.l	MOD_EDGES(a0),a4
	move.l	MOD_SURFEDGES(a0),a5
	move.l	MS_FIRSTEDGE(a3),d0
	lea	(a5,d0.l*4),a5
	move.l	_r_framecount,d5
	tst.l	d4
	ble	.edgesdone

.edge
	move.l	(a5)+,d0			; lindex: > 0 forward, else backward
	ble	.backward
	lea	(a4,d0.l*8),a0
	Edge	ME_V0,ME_V1
.backward
	neg.l	d0
	lea	(a4,d0.l*8),a0
	Edge	ME_V1,ME_V0
.next
	subq.l	#1,d4
	bne	.edge
.edgesdone

	; a clip off the left edge: that edge too
	tst.l	d6
	beq	.noleft
	lea	TEDGE(a6),a0
	move.l	a0,_r_pedge
	clr.l	_r_lastvertvalid
	move.l	CP_NEXT(a2),-(sp)
	pea	_r_leftenter
	pea	_r_leftexit
	jsr	_R_ClipEdge
	lea	12(sp),sp
.noleft
	; a clip off the right edge: the right r_nearzi
	tst.l	d7
	beq	.noright
	lea	TEDGE(a6),a0
	move.l	a0,_r_pedge
	clr.l	_r_lastvertvalid
	move.l	#1,_r_nearzionly
	move.l	_view_clipplanes+1*CP_SIZE+CP_NEXT,-(sp)
	pea	_r_rightenter
	pea	_r_rightexit
	jsr	_R_ClipEdge
	lea	12(sp),sp
.noright

	; no edges made it out: no surface
	tst.l	_r_emitted
	beq	.done

	addq.l	#1,_r_polycount
	move.l	_surface_p,a0
	move.l	a3,SF_DATA(a0)
	move.l	_r_nearzi,SF_NEARZI(a0)
	move.l	MS_FLAGS(a3),SF_FLAGS(a0)
	move.l	_insubmodel,SF_INSUBMODEL(a0)
	clr.l	SF_SPANSTATE(a0)
	move.l	_currententity,SF_ENTITY(a0)
	move.l	_r_currentkey,d0
	move.l	d0,SF_KEY(a0)
	addq.l	#1,d0
	move.l	d0,_r_currentkey
	clr.l	SF_SPANS(a0)

	; p_normal = the plane's normal in view space (stored as floats, as
	; the C's vec3_t is), then distinv and the 1/z gradients
	move.l	MS_PLANE(a3),a1
	fmove.s	(a1),fp0
	fmove.s	_vright,fp1
	fmul.x	fp0,fp1
	fmove.s	_vright+4,fp2
	fmul.s	4(a1),fp2
	fadd.x	fp2,fp1
	fmove.s	_vright+8,fp2
	fmul.s	8(a1),fp2
	fadd.x	fp1,fp2
	fmove.s	fp2,PN(a6)
	fmove.s	_vup,fp1
	fmul.x	fp0,fp1
	fmove.s	_vup+4,fp2
	fmul.s	4(a1),fp2
	fadd.x	fp2,fp1
	fmove.s	_vup+8,fp2
	fmul.s	8(a1),fp2
	fadd.x	fp1,fp2
	fmove.s	fp2,PN+4(a6)
	fmove.s	_vpn,fp1
	fmul.x	fp0,fp1
	fmove.s	_vpn+4,fp2
	fmul.s	4(a1),fp2
	fadd.x	fp2,fp1
	fmove.s	_vpn+8,fp2
	fmul.s	8(a1),fp2
	fadd.x	fp1,fp2
	fmove.s	fp2,PN+8(a6)
	; distinv = 1.0 / (dist - modelorg . normal)
	fmul.s	_modelorg,fp0
	fmove.s	_modelorg+4,fp1
	fmul.s	4(a1),fp1
	fadd.x	fp1,fp0
	fmove.s	_modelorg+8,fp1
	fmul.s	8(a1),fp1
	fadd.x	fp1,fp0
	fneg.x	fp0
	fadd.s	PL_DIST(a1),fp0
	fmove.x	fp0,fp1
	fmove.s	#$3f800000,fp0
	fdiv.x	fp1,fp0
	; d_zistepu = p_normal[0] * xscaleinv * distinv
	fmove.s	_xscaleinv,fp1
	fmul.s	PN(a6),fp1
	fmul.x	fp0,fp1
	fmove.s	fp1,SF_ZISTEPU(a0)
	; d_zistepv = -p_normal[1] * yscaleinv * distinv
	fneg.s	PN+4(a6),fp1
	fmul.s	_yscaleinv,fp1
	fmul.x	fp0,fp1
	fmove.s	fp1,SF_ZISTEPV(a0)
	; d_ziorigin = p_normal[2] * distinv - xcenter * d_zistepu
	;              - ycenter * d_zistepv
	fmul.s	PN+8(a6),fp0
	fmove.s	_xcenter,fp1
	fmul.s	SF_ZISTEPU(a0),fp1
	fneg.x	fp1
	fadd.x	fp0,fp1
	fmove.s	_ycenter,fp0
	fmul.s	SF_ZISTEPV(a0),fp0
	fneg.x	fp0
	fadd.x	fp1,fp0
	fmove.s	fp0,SF_ZIORIGIN(a0)
	lea	SF_SIZE(a0),a0
	move.l	a0,_surface_p

.done
	movem.l	(sp)+,d2-d7/a2-a6
	fmovem.x	(sp)+,fp2
	lea	FRAME(sp),sp
	rts

;------------------------------------------------------------------------------
; EmitCached: R_EmitCachedEdge for the edge at a1 (r_edges + its cached
; offset) - it now borders this surface too.  Uses d0, fp0.
;------------------------------------------------------------------------------

EmitCached
	move.l	_surface_p,d0
	sub.l	_surfaces,d0
	asr.l	#6,d0				; / sizeof (surf_t)
	tst.w	E_SURFS(a1)
	bne	.second
	move.w	d0,E_SURFS(a1)
	bra	.near
.second
	move.w	d0,E_SURFS+2(a1)
.near
	fmove.s	E_NEARZI(a1),fp0	; for mipmap finding
	fcmp.s	_r_nearzi,fp0
	fble	.emitted
	move.l	E_NEARZI(a1),_r_nearzi
.emitted
	move.l	#1,_r_emitted
	rts
