; ============================================================
; hellox.asm - MZ .EXE test program (nasm -f obj + wlink)
;
; Deliberately uses a separate data segment loaded via a
; segment-immediate, which forces a relocation entry in the MZ
; header - this exercises the EXEC relocation logic. Exits
; with code 7.
; ============================================================
cpu 386
bits 16

segment code class=CODE

..start:
	mov ax, data                ; segment fixup -> MZ relocation
	mov ds, ax
	mov dx, msg
	mov ah, 0x09
	int 0x21
	mov ax, 0x4C07              ; exit, code 7
	int 0x21

segment data class=DATA

msg db 'HELLOX.EXE running (MZ header, relocation applied)', 13, 10, '$'

segment stk stack class=STACK

	resb 256
