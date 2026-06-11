; ============================================================
; tsr.asm - TSR test (.COM)
;
; Hooks INT 78h (an unused vector) to point at its resident
; handler, prints a message and terminates via INT 21h AH=31h
; keeping only the PSP + resident code. "MEM" in the shell
; should show ~few hundred bytes less free afterwards, and the
; INT 78h vector points into the resident segment.
; ============================================================
	org 0x100

start:
	; install resident handler at INT 78h
	mov ax, 0x2578
	mov dx, handler
	int 21h

	mov ah, 0x09
	mov dx, msg
	int 21h

	; keep PSP (16 paras) + code up to resident_end
	mov dx, (resident_end - start + 0x100 + 15) / 16 + 16
	mov ax, 0x3100              ; TSR, exit code 0
	int 21h

; --- resident part: INT 78h increments a counter and returns ---
handler:
	inc word [cs:counter]
	iret
counter dw 0
resident_end:

; --- transient part (discarded after TSR) ---
msg     db 'TSR installed on INT 78h.', 13, 10, '$'
