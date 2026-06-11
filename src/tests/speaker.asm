; ============================================================
; speaker.asm - PC speaker test (.COM)
;
; Plays one octave of the C major scale (C4..C5) on the PC
; speaker: PIT channel 2 programmed per note (mode 3, square
; wave), gated through port 61h bits 0-1. Note timing uses the
; BIOS tick counter at 0040:006Ch (18.2 Hz), so it also
; exercises the timer IRQ path. Exits with code 0.
; ============================================================
cpu 386
org 0x100

PIT_HZ equ 1193182
%define N(f) (PIT_HZ / (f))

start:
	mov dx, msg
	mov ah, 0x09
	int 0x21

	mov si, notes
.next:
	lodsw                       ; AX = PIT divisor, 0 = end of tune
	test ax, ax
	jz .done
	call note_on
	mov bx, 6                   ; ~330 ms per note
	call wait_ticks
	call note_off
	mov bx, 1                   ; short gap between notes
	call wait_ticks
	jmp .next
.done:
	mov dx, done_msg
	mov ah, 0x09
	int 0x21
	mov ax, 0x4C00
	int 0x21

; --- start a square wave, AX = PIT divisor ---
note_on:
	mov bx, ax
	mov al, 0xB6                ; channel 2, lo/hi byte, mode 3
	out 0x43, al
	mov al, bl
	out 0x42, al                ; divisor low byte
	mov al, bh
	out 0x42, al                ; divisor high byte
	in al, 0x61
	or al, 0x03                 ; gate channel 2 + speaker on
	out 0x61, al
	ret

; --- silence the speaker ---
note_off:
	in al, 0x61
	and al, 0xFC
	out 0x61, al
	ret

; --- busy-wait BX changes of the BIOS tick count (55 ms each) ---
wait_ticks:
	push es
	xor ax, ax
	mov es, ax
.tick:
	mov ax, [es:0x046C]
.same:
	cmp ax, [es:0x046C]
	je .same
	dec bx
	jnz .tick
	pop es
	ret

msg      db 'SPEAKER: playing C major (do-re-mi...) on the PC speaker', 13, 10, '$'
done_msg db 'SPEAKER: done', 13, 10, '$'

notes:
	dw N(262), N(294), N(330), N(349) ; C4 D4 E4 F4
	dw N(392), N(440), N(494), N(523) ; G4 A4 B4 C5
	dw 0
