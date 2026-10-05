;
; r_bsp060.s -- the world BSP walk for the 68060
;
; R_RecursiveWorldNode, replacing the C in r_bsp.c (built with WQ_ASM=1):
; front-to-back through the world's BSP tree, culling nodes against the view
; frustum, marking the surfaces of visible leaves, and handing each node's
; surfaces that face the eye to R_RenderFace.  It was ~2 ms a frame in C
; (hardware profile, 2026-10-04): vbcc stored every corner coordinate of the
; box test to the stack and read it back, and saved its whole register set
; at every level of the recursion.
;
; Here a level of the recursion is a BSR with four registers pushed, and the
; back side's recursion is a branch (the C's second call is its last
; statement).  The float work is vbcc's code for the C, operation for
; operation: the box corners are shorts, exact as floats, so loading them
; straight into the FPU (FMOVE.W) is what vbcc's store-and-reload computes;
; -crc checks it.
;
; r_drawpolys and r_worldpolysbacktofront, the C's other two ways to draw a
; surface, are never set in this engine (d_init.c clears them); the walk
; always calls R_RenderFace.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1/fp0-fp1
; scratch, everything else preserved.
;

	machine	68060

	xref	_r_visframecount	; int: nodes in the PVS carry this
	xref	_r_framecount		; int
	xref	_r_currentkey		; int: surface sort key, front to back
	xref	_pfrustum_indexes	; int *[4]: per plane, the box corner indexes
	xref	_view_clipplanes	; clipplane_t [4]
	xref	_modelorg			; vec3_t: the eye
	xref	_cl					; client_state_t: worldmodel at CL_WORLDMODEL
	xref	_R_StoreEfrags		; r_efrag.c
	xref	_R_RenderFace		; r_draw.c

	xdef	_R_RecursiveWorldNode

; mnode_t / mleaf_t (model.h); the two share the first 24 bytes
N_CONTENTS		equ	0
N_VISFRAME		equ	4
N_MINMAXS		equ	8			; short [6]
N_PLANE			equ	24
N_CHILDREN		equ	28			; mnode_t *[2]
N_FIRSTSURF		equ	36			; unsigned short
N_NUMSURFS		equ	38			; unsigned short
L_EFRAGS		equ	28
L_FIRSTMARK		equ	32			; msurface_t **
L_NUMMARK		equ	36			; int
L_KEY			equ	40
CONTENTS_SOLID	equ	-2
; mplane_t
PL_DIST			equ	12
PL_TYPE			equ	16			; byte: 0, 1, 2 axial, else general
; clipplane_t, 24 bytes
CP_DIST			equ	12
CP_SIZE			equ	24
; msurface_t, 64 bytes
S_VISFRAME		equ	0
S_FLAGS			equ	16
S_SIZE			equ	64
SURF_PLANEBACK	equ	2
; client_state_t: cl.worldmodel; model_t: surfaces
CL_WORLDMODEL	equ	2692
M_SURFACES		equ	180

	section	CODE,code

;------------------------------------------------------------------------------
; void R_RecursiveWorldNode (mnode_t *node, int clipflags)
;
; Per level:  a4 = node   d3 = clipflags (planes the node may cross)
;             d5 = side (0: eye in front of the node's plane)
;             d6 = which surfaces face the eye: 1 front, 2 back, 0 neither
;                  (the eye within BACKFACE_EPSILON of the plane)
; For the whole walk: d7 = r_visframecount
;------------------------------------------------------------------------------

; ClipPlane i: the C's test of the node's box against frustum plane i, if
; clipflags still has it.  Off the plane's back by its nearest corner: the
; node is not drawn.  In front by its farthest: plane i is dropped for the
; subtree.  d = (x*n0 + y*n1) + z*n2, then -dist + d, as vbcc computes it.
ClipPlane	macro
	btst	#\1,d3
	beq	.cp\@
	move.l	_pfrustum_indexes+\1*4,a2
	lea	_view_clipplanes+\1*CP_SIZE,a0
	; the reject corner (indexes 0-2)
	move.l	(a2),d0
	fmove.w	N_MINMAXS(a4,d0.l*2),fp1
	fmul.s	(a0),fp1
	move.l	4(a2),d0
	fmove.w	N_MINMAXS(a4,d0.l*2),fp0
	fmul.s	4(a0),fp0
	fadd.x	fp0,fp1
	move.l	8(a2),d0
	fmove.w	N_MINMAXS(a4,d0.l*2),fp0
	fmul.s	8(a0),fp0
	fadd.x	fp0,fp1
	fmove.s	CP_DIST(a0),fp0
	fneg.x	fp0
	fadd.x	fp1,fp0
	ftst.x	fp0
	fble	.ret				; d <= 0: entirely off this plane
	; the accept corner (indexes 3-5)
	move.l	12(a2),d0
	fmove.w	N_MINMAXS(a4,d0.l*2),fp1
	fmul.s	(a0),fp1
	move.l	16(a2),d0
	fmove.w	N_MINMAXS(a4,d0.l*2),fp0
	fmul.s	4(a0),fp0
	fadd.x	fp0,fp1
	move.l	20(a2),d0
	fmove.w	N_MINMAXS(a4,d0.l*2),fp0
	fmul.s	8(a0),fp0
	fadd.x	fp0,fp1
	fmove.s	CP_DIST(a0),fp0
	fneg.x	fp0
	fadd.x	fp1,fp0
	ftst.x	fp0
	fblt	.cp\@
	bclr	#\1,d3				; d >= 0: entirely on screen for this plane
.cp\@
	endm

; Surfaces face the eye when their SURF_PLANEBACK bit equals \1 (0 or 2):
; those visible this frame go to R_RenderFace (surf, clipflags).
FaceLoop	macro
.fl\@
	moveq	#SURF_PLANEBACK,d0
	and.l	S_FLAGS(a2),d0
	cmp.l	#\1,d0
	bne	.fn\@
	cmp.l	S_VISFRAME(a2),d4
	bne	.fn\@
	move.l	d3,-(sp)
	move.l	a2,-(sp)
	jsr	_R_RenderFace
	addq.l	#8,sp
.fn\@
	lea	S_SIZE(a2),a2
	subq.l	#1,d2
	bne	.fl\@
	endm

	cnop	0,4
_R_RecursiveWorldNode
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	4+11*4(sp),a4
	move.l	8+11*4(sp),d3
	move.l	_r_visframecount,d7
	bsr	Node
	movem.l	(sp)+,d2-d7/a2-a6
	rts

Node
	moveq	#CONTENTS_SOLID,d0
	cmp.l	N_CONTENTS(a4),d0
	beq	.ret
	cmp.l	N_VISFRAME(a4),d7
	bne	.ret

	tst.l	d3
	beq	.inside
	ClipPlane	0
	ClipPlane	1
	ClipPlane	2
	ClipPlane	3
.inside

	tst.l	N_CONTENTS(a4)
	bge	.node

	; a leaf: mark its surfaces visible, store its entity fragments, and
	; give it the next sort key
	move.l	L_FIRSTMARK(a4),a0
	move.l	L_NUMMARK(a4),d0
	beq	.marked
	move.l	_r_framecount,d1
.mark
	move.l	(a0)+,a1
	move.l	d1,S_VISFRAME(a1)
	subq.l	#1,d0
	bne	.mark
.marked
	tst.l	L_EFRAGS(a4)
	beq	.noefrags
	pea	L_EFRAGS(a4)
	jsr	_R_StoreEfrags
	addq.l	#4,sp
.noefrags
	move.l	_r_currentkey,L_KEY(a4)
	addq.l	#1,_r_currentkey
.ret
	rts

.node
	; dot = the eye's distance from the node's plane
	move.l	N_PLANE(a4),a0
	move.b	PL_TYPE(a0),d0
	beq	.px
	subq.b	#1,d0
	beq	.py
	subq.b	#1,d0
	beq	.pz
	fmove.s	_modelorg,fp0
	fmul.s	(a0),fp0
	fmove.s	_modelorg+4,fp1
	fmul.s	4(a0),fp1
	fadd.x	fp1,fp0
	fmove.s	_modelorg+8,fp1
	fmul.s	8(a0),fp1
	fadd.x	fp1,fp0
	fsub.s	PL_DIST(a0),fp0
	bra	.dot
.px	fmove.s	_modelorg,fp0
	fsub.s	PL_DIST(a0),fp0
	bra	.dot
.py	fmove.s	_modelorg+4,fp0
	fsub.s	PL_DIST(a0),fp0
	bra	.dot
.pz	fmove.s	_modelorg+8,fp0
	fsub.s	PL_DIST(a0),fp0
.dot
	moveq	#0,d5				; side: 1 if dot < 0
	ftst.x	fp0
	fbnlt	.front
	moveq	#1,d5
.front
	; (the comparisons branch on NaN as vbcc's do)
	moveq	#2,d6				; facing: back surfaces if dot < -epsilon ...
	fcmp.d	#$bf847ae147ae147b,fp0	; -0.01 (BACKFACE_EPSILON)
	fbnge	.facing
	moveq	#1,d6				; ... front ones if dot > epsilon
	fcmp.d	#$3f847ae147ae147b,fp0	; 0.01
	fble	.notfront
	bra	.facing
.notfront
	moveq	#0,d6
.facing

	; the side the eye is on first
	movem.l	d3/d5-d6/a4,-(sp)
	move.l	N_CHILDREN(a4,d5.l*4),a4
	bsr	Node
	movem.l	(sp)+,d3/d5-d6/a4

	; then this node's surfaces that face the eye
	moveq	#0,d2
	move.w	N_NUMSURFS(a4),d2
	beq	.back
	tst.l	d6
	beq	.keyed
	move.l	_cl+CL_WORLDMODEL,a0
	move.l	M_SURFACES(a0),a2
	moveq	#0,d0
	move.w	N_FIRSTSURF(a4),d0
	lsl.l	#6,d0				; * sizeof (msurface_t)
	add.l	d0,a2
	move.l	_r_framecount,d4
	cmp.l	#2,d6
	beq	.backfaces
	FaceLoop	0
	bra	.keyed
.backfaces
	FaceLoop	SURF_PLANEBACK
.keyed
	addq.l	#1,_r_currentkey	; all surfaces of a node share a key

.back
	; and the far side: the C's last statement, so a branch, not a call
	moveq	#1,d0
	sub.l	d5,d0
	move.l	N_CHILDREN(a4,d0.l*4),a4
	bra	Node
