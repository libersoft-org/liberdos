; ============================================================
; hello.asm - .COM test program for the EXEC test suite
;
; Prints a banner, echoes the command tail from PSP:80h and
; exits with code 42 via INT 21h AH=4Ch.
; ============================================================
[BITS 16]
[ORG 0x100]

start:
	mov dx, msg
	mov ah, 0x09
	int 0x21

	mov si, 0x80                ; command tail: length byte + text
	lodsb
	mov cl, al
	xor ch, ch
	jcxz .no_tail
.tail_loop:
	lodsb
	mov dl, al
	mov ah, 0x02
	int 0x21
	loop .tail_loop
.no_tail:
	mov dx, tail_end
	mov ah, 0x09
	int 0x21

	mov ax, 0x4C2A              ; exit, code 42
	int 0x21

msg      db 'HELLO.COM running, tail:[$'
tail_end db ']', 13, 10, '$'
