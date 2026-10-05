;
; r_edge060.s -- the edge-list scan core for the 68060
;
; R_GenerateSpans (with R_TrailingEdge and R_LeadingEdge's common case
; inline), R_StepActiveU, R_InsertNewEdges and R_RemoveEdges, replacing the C
; in r_edge.c (built with WQ_ASM=1).  Together they were ~11% of the frame on
; the 68060 (hardware profile, 2026-10-04): per scan line they walk the
; active edge list, keep it sorted in u, and turn the edges into spans of
; whichever surface is in front.
;
; Pure integer and pointer work, so the result is exactly the C's; -crc
; checks it.  The one float path - two brush-model surfaces with the same
; key, ordered by 1/z - is left to the C R_LeadingEdge; every other
; brush-model edge (doors, lifts, ammo and health boxes) is done here.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1 scratch.
;

	machine	68060

	xref	_surfaces			; surf_t *: surfaces[1] is the background
	xref	_span_p				; espan_t *: next free span
	xref	_current_iv			; int: the scan line
	xref	_edge_head			; edge_t
	xref	_edge_tail			; edge_t
	xref	_edge_aftertail		; edge_t
	xref	_edge_head_u_shift20	; int
	xref	_r_bmodelactive		; int
	xref	_R_LeadingEdge		; void (edge_t *): the C, for brush models
	xref	_R_CleanupSpan		; void (void)

	xdef	_R_GenerateSpans
	xdef	_R_StepActiveU
	xdef	_R_InsertNewEdges
	xdef	_R_RemoveEdges

; edge_t (r_shared.h)
E_U		equ	0
E_USTEP		equ	4
E_PREV		equ	8
E_NEXT		equ	12
E_SURF0		equ	16			; unsigned short: surface on the left
E_SURF1		equ	18			; unsigned short: surface on the right
E_NEXTREMOVE	equ	20

; surf_t (r_shared.h), 64 bytes
S_NEXT		equ	0
S_PREV		equ	4
S_SPANS		equ	8
S_KEY		equ	12
S_LAST_U	equ	16
S_SPANSTATE	equ	20
S_INSUBMODEL	equ	40
SURF_SHIFT	equ	6			; log2 (sizeof (surf_t))

; espan_t (r_shared.h), 16 bytes
SP_U		equ	0
SP_V		equ	4
SP_COUNT	equ	8
SP_PNEXT	equ	12

	section	CODE,code

;------------------------------------------------------------------------------
; EmitSpan surf: a span for surf from its last_u up to iu (d1), if it is not
; empty: u = last_u, count = iu - last_u, v = the scan line, pushed on the
; surface's span list.  Uses d3.
;
;   a3 = span_p (kept in a register for the whole scan, stored at the end)
;   d7 = current_iv
;------------------------------------------------------------------------------

EmitSpan	macro
	move.l	S_LAST_U(\1),d3
	cmp.l	d3,d1
	ble.s	.nospan\@
	move.l	d3,SP_U(a3)
	move.l	d7,SP_V(a3)
	neg.l	d3
	add.l	d1,d3
	move.l	d3,SP_COUNT(a3)
	move.l	S_SPANS(\1),SP_PNEXT(a3)
	move.l	a3,S_SPANS(\1)
	lea	16(a3),a3
.nospan\@
	endm

;------------------------------------------------------------------------------
; void R_GenerateSpans (void)
;
; One scan line: walk the u-sorted edges.  An edge with a left surface ends
; that surface (trailing); one with a right surface starts it (leading),
; inserted into the active surface stack by key; whenever the top surface
; changes, the old top gets a span up to here.
;
;   a2 = edge   a4 = &surfaces[1] (the background, head of the stack)
;   a5 = surfaces   a3 = span_p   d7 = current_iv
;   a0 = surf   a1 = surf2   d1 = iu = edge->u >> 20   d2 = key
;------------------------------------------------------------------------------

	cnop	0,4
_R_GenerateSpans
	movem.l	d2-d7/a2-a6,-(sp)
	clr.l	_r_bmodelactive
	move.l	_surfaces,a5
	lea	64(a5),a4			; the background surface ...
	move.l	a4,S_NEXT(a4)			; ... alone in the stack
	move.l	a4,S_PREV(a4)
	move.l	_edge_head_u_shift20,S_LAST_U(a4)
	move.l	_span_p,a3
	move.l	_current_iv,d7
	move.l	_edge_head+E_NEXT,a2
	cmpa.l	#_edge_tail,a2
	beq	.done

.edge
	moveq	#0,d0
	move.w	E_SURF0(a2),d0
	beq.s	.leading

	; trailing edge: surf = surfaces + surfs[0]; it goes away once its
	; span state drops to 0 (an inverted span counts the other way)
	lsl.l	#SURF_SHIFT,d0
	lea	(a5,d0.l),a0
	subq.l	#1,S_SPANSTATE(a0)
	bne.s	.trailed
	tst.l	S_INSUBMODEL(a0)
	beq.s	.t_world
	subq.l	#1,_r_bmodelactive
.t_world
	cmpa.l	S_NEXT(a4),a0
	bne.s	.t_unlink
	; the top is going away: its span ends here, the one below starts
	move.l	E_U(a2),d1
	moveq	#20,d0
	asr.l	d0,d1				; iu
	EmitSpan	a0
	move.l	S_NEXT(a0),a1
	move.l	d1,S_LAST_U(a1)
.t_unlink
	move.l	S_PREV(a0),a1
	move.l	S_NEXT(a0),a6
	move.l	a6,S_NEXT(a1)
	move.l	a1,S_PREV(a6)
.trailed
	moveq	#0,d0
	move.w	E_SURF1(a2),d0
	beq.s	.next
	bra.s	.lead_surf

.leading
	move.w	E_SURF1(a2),d0
	beq.s	.next
.lead_surf
	; leading edge: surf = surfaces + surfs[1]
	lsl.l	#SURF_SHIFT,d0
	lea	(a5,d0.l),a0
	tst.l	S_INSUBMODEL(a0)
	bne	.bmodel
	addq.l	#1,S_SPANSTATE(a0)
	moveq	#1,d0
	cmp.l	S_SPANSTATE(a0),d0
	bne.s	.next				; not the start of a span (inverted)

	move.l	S_NEXT(a4),a1			; surf2 = the current top
	move.l	S_KEY(a0),d2
	cmp.l	S_KEY(a1),d2
	blt	.newtop				; in front of the top
.search
	; find the first surface it is not behind (equal keys: the one
	; already active stays in front, for a world surface)
	move.l	S_NEXT(a1),a1
	cmp.l	S_KEY(a1),d2
	bge.s	.search
	bra	.insert

.newtop
	; it obscures the current top: the top's span ends here
	move.l	E_U(a2),d1
	moveq	#20,d0
	asr.l	d0,d1				; iu
	EmitSpan	a1
	move.l	d1,S_LAST_U(a0)

.insert
	; insert surf before surf2
	move.l	a1,S_NEXT(a0)
	move.l	S_PREV(a1),a6
	move.l	a6,S_PREV(a0)
	move.l	a0,S_NEXT(a6)
	move.l	a0,S_PREV(a1)

.next
	move.l	E_NEXT(a2),a2
	cmpa.l	#_edge_tail,a2
	bne	.edge

.done
	move.l	a3,_span_p
	jsr	_R_CleanupSpan
	movem.l	(sp)+,d2-d7/a2-a6
	rts

.bmodel
	; A brush model's surface: as a world one, but it counts in
	; r_bmodelactive, and where it meets an active surface with the same key
	; (two brush models in one leaf) the order is decided by 1/z, in float -
	; that rare case goes to the C.  So first find the place without
	; changing anything: the C may still have to do it all.
	tst.l	S_SPANSTATE(a0)
	bne	.bm_inside			; not the start of a span: just count it
	move.l	S_NEXT(a4),a1
	move.l	S_KEY(a0),d2
	cmp.l	S_KEY(a1),d2
	blt	.bm_newtop
	beq	.bm_c
.bm_search
	move.l	S_NEXT(a1),a1
	cmp.l	S_KEY(a1),d2
	bgt	.bm_search
	beq	.bm_c
	move.l	#1,S_SPANSTATE(a0)
	addq.l	#1,_r_bmodelactive
	bra	.insert
.bm_newtop
	move.l	#1,S_SPANSTATE(a0)
	addq.l	#1,_r_bmodelactive
	bra	.newtop
.bm_inside
	addq.l	#1,S_SPANSTATE(a0)
	bra	.next
.bm_c
	move.l	a3,_span_p
	move.l	a2,-(sp)
	jsr	_R_LeadingEdge
	addq.l	#4,sp
	move.l	_span_p,a3
	bra	.next

;------------------------------------------------------------------------------
; void R_StepActiveU (edge_t *pedge)
;
; Step every active edge one scan line on (u += u_step) and keep the list
; sorted: an edge that passes the one before it is pulled out and walked
; back to its place.  The list ends in edge_tail, then edge_aftertail.
;
;   a0 = pedge   a1 = prev, then pwedge   a2 = pnext_edge   d0 = u
;------------------------------------------------------------------------------

	cnop	0,4
_R_StepActiveU
	move.l	4(sp),a0
	movem.l	a2-a3,-(sp)
.step
	move.l	E_U(a0),d0
	add.l	E_USTEP(a0),d0
	move.l	d0,E_U(a0)
	move.l	E_PREV(a0),a1
	cmp.l	E_U(a1),d0
	blt.s	.pushback
	move.l	E_NEXT(a0),a0
	bra.s	.step

.pushback
	cmpa.l	#_edge_aftertail,a0
	beq.s	.out
	; pull it out (prev is a1) ...
	move.l	E_NEXT(a0),a2			; pnext_edge
	move.l	a1,E_PREV(a2)
	move.l	a2,E_NEXT(a1)
	; ... find the first edge back from prev->prev whose u is not greater ...
	move.l	E_PREV(a1),a1
.back
	cmp.l	E_U(a1),d0
	bge.s	.found
	move.l	E_PREV(a1),a1
	bra.s	.back
.found
	; ... and put it back after that one (a1 = pwedge)
	move.l	E_NEXT(a1),a3
	move.l	a3,E_NEXT(a0)
	move.l	a1,E_PREV(a0)
	move.l	a0,E_PREV(a3)
	move.l	a0,E_NEXT(a1)
	move.l	a2,a0				; continue with pnext_edge
	cmpa.l	#_edge_tail,a0
	bne.s	.step
.out
	movem.l	(sp)+,a2-a3
	rts

;------------------------------------------------------------------------------
; void R_InsertNewEdges (edge_t *edgestoadd, edge_t *edgelist)
;
; Merge the u-sorted new edges of this scan line into the active list:
; each goes before the first active edge whose u is not less than its own.
; The search resumes where the last insertion was.
;------------------------------------------------------------------------------

	cnop	0,4
_R_InsertNewEdges
	move.l	4(sp),a0			; edgestoadd
	move.l	8(sp),a1			; edgelist
	move.l	a2,-(sp)
.add
	move.l	E_NEXT(a0),-(sp)		; next_edge
	move.l	E_U(a0),d0
.search_in
	cmp.l	E_U(a1),d0
	ble.s	.addedge
	move.l	E_NEXT(a1),a1
	bra.s	.search_in
.addedge
	; insert edgestoadd before edgelist
	move.l	a1,E_NEXT(a0)
	move.l	E_PREV(a1),a2
	move.l	a2,E_PREV(a0)
	move.l	a0,E_NEXT(a2)
	move.l	a0,E_PREV(a1)
	move.l	(sp)+,a0
	move.l	a0,d0
	bne.s	.add
	move.l	(sp)+,a2
	rts

;------------------------------------------------------------------------------
; void R_RemoveEdges (edge_t *pedge)
;
; Unlink the edges that end on this scan line (chained by nextremove).
;------------------------------------------------------------------------------

	cnop	0,4
_R_RemoveEdges
	move.l	4(sp),a0
.remove
	move.l	E_NEXT(a0),a1
	move.l	E_PREV(a0),d0
	move.l	d0,E_PREV(a1)
	move.l	d0,a1
	move.l	E_NEXT(a0),E_NEXT(a1)
	move.l	E_NEXTREMOVE(a0),d0
	move.l	d0,a0
	bne.s	.remove
	rts

	end
