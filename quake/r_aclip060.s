;
; r_aclip060.s -- alias model polygon clipping for the 68060
;
; R_AliasClip, replacing the C in r_aclip.c (built with WQ_ASM=1): one pass
; of Sutherland-Hodgman clipping of a model triangle's polygon against one
; screen edge (or the near plane).  It is nearly always the weapon, whose
; bottom leaves the view: ~0.5 ms a frame in C with the clip functions it
; calls through a pointer for every crossing edge (hardware profile,
; 2026-10-05).
;
; The four screen-edge clips are inline here, recognised by the function
; pointer the C passes; the near-plane clip (R_Alias_clip_z, which projects)
; is still called through it.  Their float work is vbcc's code for the C:
;
;   scale = (float)(bound - a->v[axis]) / (b->v[axis] - a->v[axis])
;   out->v[i] = a->v[i] + (b->v[i] - a->v[i]) * scale + 0.5
;
; with a the end with the larger v (the C's tie-break), scale kept in
; extended as vbcc keeps it, and the 0.5 a double.  So the vertices are the
; C's exactly; -crc checks it.
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1/fp0-fp1
; scratch, everything else preserved.
;

	machine	68060

	xref	_r_refdef			; refdef_t
	xref	_R_Alias_clip_left	; r_aclip.c: recognised and done inline ...
	xref	_R_Alias_clip_right
	xref	_R_Alias_clip_top
	xref	_R_Alias_clip_bottom	; ... or called (the z clip)

	xdef	_R_AliasClip

; finalvert_t (d_iface.h), 32 bytes
FV_U		equ	0
FV_V		equ	4
FV_FLAGS	equ	24
FV_SIZE		equ	32
; refdef_t (render.h)
RD_ALIASVRECT_X		equ	20
RD_ALIASVRECT_Y		equ	24
RD_ALIASVRECTRIGHT	equ	48
RD_ALIASVRECTBOTTOM	equ	52
ALIAS_LEFT_CLIP		equ	$01
ALIAS_TOP_CLIP		equ	$02
ALIAS_RIGHT_CLIP	equ	$04
ALIAS_BOTTOM_CLIP	equ	$08

	section	CODE,code

;------------------------------------------------------------------------------
; int R_AliasClip (finalvert_t *in, finalvert_t *out, int flag, int count,
;                  void (*clip)(finalvert_t *, finalvert_t *, finalvert_t *))
;
;   a4 = in   a3 = out (next free)   a5 = clip (the C function, z only)
;   d6 = flag   d5 = j (previous vertex, a pointer)   d3 = i (vertex left)
;   d2 = k (vertices out)   d7 = the screen edge: 0 = call clip, else the
;   axis (V_U or V_V offset + 1) in the low byte and the bound in d4
;------------------------------------------------------------------------------

	cnop	0,4
_R_AliasClip
	movem.l	d2-d7/a2-a5,-(sp)
	fmovem.x	fp2-fp3,-(sp)
.args	equ	10*4+2*12+4
	move.l	.args(sp),a4
	move.l	.args+4(sp),a3
	move.l	.args+8(sp),d6
	move.l	.args+12(sp),d3
	move.l	.args+16(sp),a5
	moveq	#0,d2
	tst.l	d3
	ble	.done

	; which clip: an inline screen edge (axis offset in d7, bound in d4) or
	; the C function
	moveq	#0,d7
	cmpa.l	#_R_Alias_clip_left,a5
	bne.s	.c1
	moveq	#FV_U+1,d7
	move.l	_r_refdef+RD_ALIASVRECT_X,d4
	bra.s	.c4
.c1	cmpa.l	#_R_Alias_clip_right,a5
	bne.s	.c2
	moveq	#FV_U+1,d7
	move.l	_r_refdef+RD_ALIASVRECTRIGHT,d4
	bra.s	.c4
.c2	cmpa.l	#_R_Alias_clip_top,a5
	bne.s	.c3
	moveq	#FV_V+1,d7
	move.l	_r_refdef+RD_ALIASVRECT_Y,d4
	bra.s	.c4
.c3	cmpa.l	#_R_Alias_clip_bottom,a5
	bne.s	.c4
	moveq	#FV_V+1,d7
	move.l	_r_refdef+RD_ALIASVRECTBOTTOM,d4
.c4
	fmove.d	#$3fe0000000000000,fp3	; 0.5
	move.l	d3,d0
	subq.l	#1,d0
	lsl.l	#5,d0
	lea	(a4,d0.l),a2		; in[j], j = count - 1
	move.l	a2,d5

.vertex
	; flags of in[j] and in[i]
	move.l	d5,a0
	move.l	FV_FLAGS(a0),d1
	and.l	d6,d1				; oldflags
	move.l	FV_FLAGS(a4),d0
	and.l	d6,d0				; flags
	beq.s	.notboth
	tst.l	d1
	bne	.next				; both out: nothing
.notboth
	eor.l	d0,d1
	beq	.nocross

	; the edge crosses: out[k] = the clip point of in[j] .. in[i]
	tst.l	d7
	bne.s	.inline
	move.l	d0,-(sp)
	move.l	a3,-(sp)
	move.l	a4,-(sp)
	move.l	d5,-(sp)
	jsr	(a5)
	lea	12(sp),sp
	move.l	(sp)+,d0
	bra.s	.clipped
.inline
	; a = the end with the larger v (pfv0 = in[j] if its v >= in[i]'s)
	move.l	d5,a0				; pfv0
	move.l	a4,a1				; pfv1
	move.l	FV_V(a0),d1
	cmp.l	FV_V(a1),d1
	bge.s	.aok
	exg	a0,a1
.aok
	; scale = (float)(bound - a->v[axis]) / (b->v[axis] - a->v[axis])
	move.l	d0,-(sp)
	move.l	d7,d0
	subq.l	#1,d0				; the axis offset
	move.l	d4,d1
	sub.l	(a0,d0.l),d1
	fmove.l	d1,fp1
	move.l	(a1,d0.l),d1
	sub.l	(a0,d0.l),d1
	fmove.l	d1,fp0
	fmove.x	fp1,fp2
	fdiv.x	fp0,fp2
	; out->v[i] = a->v[i] + (b->v[i] - a->v[i]) * scale + 0.5, i = 0..5
	moveq	#0,d0
.comp
	move.l	(a1,d0.l),d1
	sub.l	(a0,d0.l),d1
	fmove.l	d1,fp0
	fmul.x	fp2,fp0
	fmove.l	(a0,d0.l),fp1
	fadd.x	fp1,fp0
	fadd.x	fp3,fp0
	fintrz.x	fp0
	fmove.l	fp0,(a3,d0.l)
	addq.l	#4,d0
	cmp.l	#24,d0
	bne.s	.comp
	move.l	(sp)+,d0
.clipped
	; its flags, against the four screen edges
	moveq	#0,d1
	move.l	FV_U(a3),a0
	cmpa.l	_r_refdef+RD_ALIASVRECT_X,a0
	bge.s	.f1
	moveq	#ALIAS_LEFT_CLIP,d1
.f1	cmpa.l	_r_refdef+RD_ALIASVRECTRIGHT,a0
	ble.s	.f2
	addq.l	#ALIAS_RIGHT_CLIP,d1
.f2	move.l	FV_V(a3),a0
	cmpa.l	_r_refdef+RD_ALIASVRECT_Y,a0
	bge.s	.f3
	addq.l	#ALIAS_TOP_CLIP,d1
.f3	cmpa.l	_r_refdef+RD_ALIASVRECTBOTTOM,a0
	ble.s	.f4
	addq.l	#ALIAS_BOTTOM_CLIP,d1
.f4	move.l	d1,FV_FLAGS(a3)
	lea	FV_SIZE(a3),a3
	addq.l	#1,d2

.nocross
	; in[i] itself, if it is inside
	tst.l	d0
	bne.s	.next
	movem.l	(a4),d0-d1/a0-a1
	movem.l	d0-d1/a0-a1,(a3)
	movem.l	16(a4),d0-d1/a0-a1
	movem.l	d0-d1/a0-a1,16(a3)
	lea	FV_SIZE(a3),a3
	addq.l	#1,d2

.next
	move.l	a4,d5				; j = i
	lea	FV_SIZE(a4),a4
	subq.l	#1,d3
	bne	.vertex

.done
	move.l	d2,d0
	fmovem.x	(sp)+,fp2-fp3
	movem.l	(sp)+,d2-d7/a2-a5
	rts
