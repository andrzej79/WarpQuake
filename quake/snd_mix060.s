;
; snd_mix060.s -- the sound mixer's two inner loops for the 68060
;
; SND_PaintChannelFrom8 and Snd_WriteLinearBlastStereo16, replacing the C in
; snd_mix.c (built with WQ_ASM=1): mixing an 8-bit sound into the paint
; buffer, and the paint buffer out to the 16-bit stereo ring, clamped.  At
; 22050 Hz they were ~1.5 ms of the ~1.8 a frame the mixer took in C
; (hardware profile, 2026-10-05).  Integer only, so the same samples:
; -sndtest -crc checks it (wq_prof.c).
;
; Calling convention: vbcc's, arguments on the stack, d0-d1/a0-a1 scratch.
;

	machine	68060

	xref	_paintbuffer			; portable_samplepair_t[512]: left, right ints
	xref	_snd_scaletable			; int[32][256]: sample * volume
	xref	_snd_p				; int *: the paint buffer, as ints
	xref	_snd_out			; short *: where in the ring
	xref	_snd_linear_count		; int: shorts to write (an even number)
	xref	_snd_vol			; int: master volume * 256

	xdef	_SND_PaintChannelFrom8
	xdef	_Snd_WriteLinearBlastStereo16

; channel_t (sound.h)
CH_LEFTVOL	equ	4
CH_RIGHTVOL	equ	8
CH_POS		equ	16
; sfxcache_t
SC_DATA		equ	20

	section	CODE,code

;------------------------------------------------------------------------------
; void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int count)
;
;   paintbuffer[i].left += lscale[sfx[i]]; .right += rscale[sfx[i]]
;
; with lscale, rscale the scale table rows for the channel's volumes (each
; clamped to 255 and written back, as the C does).  The next sample is loaded
; while the current one is used, so no index is used right after its load.
;
;   a0 = paintbuffer  a1 = sfx  a2 = lscale  a3 = rscale  d3 = count - 1
;------------------------------------------------------------------------------

	cnop	0,4
_SND_PaintChannelFrom8
	movem.l	d2-d3/a2-a3,-(sp)
.args	equ	4*4+4
	move.l	.args(sp),a0			; ch
	move.l	.args+4(sp),a1			; sc
	move.l	.args+8(sp),d3			; count
	move.l	CH_LEFTVOL(a0),d0
	cmp.l	#255,d0
	ble.s	.lok
	move.l	#255,d0
	move.l	d0,CH_LEFTVOL(a0)
.lok
	move.l	CH_RIGHTVOL(a0),d1
	cmp.l	#255,d1
	ble.s	.rok
	move.l	#255,d1
	move.l	d1,CH_RIGHTVOL(a0)
.rok
	asr.l	#3,d0
	asr.l	#3,d1
	moveq	#10,d2
	lsl.l	d2,d0				; row: (vol >> 3) * 256 ints
	lsl.l	d2,d1
	lea	_snd_scaletable,a2
	lea	(a2,d1.l),a3			; rscale
	add.l	d0,a2				; lscale
	move.l	CH_POS(a0),d0
	lea	SC_DATA(a1,d0.l),a1		; sfx = sc->data + ch->pos
	add.l	d3,CH_POS(a0)			; ch->pos += count
	lea	_paintbuffer,a0
	subq.l	#1,d3
	blt.s	.done
	moveq	#0,d0
	move.b	(a1)+,d0			; the first sample (read one ahead: the
.loop						; last pass reads a byte past the end)
	move.l	(a2,d0.l*4),d1
	move.l	(a3,d0.l*4),d2
	move.b	(a1)+,d0
	add.l	d1,(a0)+
	add.l	d2,(a0)+
	dbf	d3,.loop
.done
	movem.l	(sp)+,d2-d3/a2-a3
	rts

;------------------------------------------------------------------------------
; void Snd_WriteLinearBlastStereo16 (void)
;
;   snd_out[i] = clamp ((snd_p[i] * snd_vol) >> 8, -32768, 32767)
;
; for snd_linear_count values, a left and right pair per pass.
;
;   a0 = snd_p  a1 = snd_out  d2 = snd_vol  d3 = pairs - 1
;   d4 = 32767  d5 = -32768
;------------------------------------------------------------------------------

	cnop	0,4
_Snd_WriteLinearBlastStereo16
	movem.l	d2-d5,-(sp)
	move.l	_snd_p,a0
	move.l	_snd_out,a1
	move.l	_snd_vol,d2
	move.l	_snd_linear_count,d3
	asr.l	#1,d3
	subq.l	#1,d3
	blt.s	.bdone
	move.l	#32767,d4
	move.l	#-32768,d5
.pair
	move.l	(a0)+,d0
	move.l	(a0)+,d1
	muls.l	d2,d0
	muls.l	d2,d1
	asr.l	#8,d0
	asr.l	#8,d1
	cmp.l	d4,d0
	bgt.s	.lhi
	cmp.l	d5,d0
	blt.s	.llo
.lput
	cmp.l	d4,d1
	bgt.s	.rhi
	cmp.l	d5,d1
	blt.s	.rlo
.rput
	move.w	d0,(a1)+
	move.w	d1,(a1)+
	dbf	d3,.pair
.bdone
	movem.l	(sp)+,d2-d5
	rts
.lhi
	move.l	d4,d0
	bra.s	.lput
.llo
	move.l	d5,d0
	bra.s	.lput
.rhi
	move.l	d4,d1
	bra.s	.rput
.rlo
	move.l	d5,d1
	bra.s	.rput

	end
