; ============================================================
; cstart.asm - .COM startup for OpenWatcom C programs
;
; Same idea as the kernel: small model (-ms) + everything in
; one DGROUP = Watcom tiny layout, linked with wlink
; The loader gives CS=DS=ES=SS=PSP and SP near the top of the
; segment, so there is nothing to set up - just call the C
; entry point.
;
; Also carries the 32-bit math helpers that the Watcom runtime
; would normally provide (we compile with -zl).
; ============================================================
cpu 386
bits 16

extern shell_main_              ; shell.c, void shell_main(void)

; ------------------------------------------------------------
; PSP placeholder: 256 reserved bytes at the front of DGROUP.
; It pins the group base to PSP:0 while the code starts at
; group offset 100h - exactly how DOS lays out a .COM image,
; so every DGROUP-relative data offset is valid with DS = PSP.
; wlink ("format dos com") requires the entry point at 100h
; and does not write the sub-100h range into the output file.
; ------------------------------------------------------------
segment PSPSEG align=16 class=PSP

	resb 0x100

segment _TEXT align=1 class=CODE

..start:
	call shell_main_
	mov ax, 0x4C00              ; exit(0) if shell_main returns
	int 0x21

; ------------------------------------------------------------
; INT 23h (Ctrl-C / Ctrl-Break) handler, installed by shell.c
; via AX=2523h. Children inherit it through the PSP vector
; save/restore done by EXEC and terminate. Two cases:
;   current PSP == shell  -> latch a flag for the command
;                            loops and continue (IRET)
;   current PSP != shell  -> abort the running child program
;                            (RETF with CF set, the DOS way)
; A .COM runs with CS = its own PSP segment, so CS is the
; shell's PSP for the comparison and the cs: data override.
; ------------------------------------------------------------
global break_handler_
break_handler_:
	push ax
	push bx
	mov ah, 51h                 ; BX = current PSP
	int 21h
	mov ax, cs
	cmp ax, bx
	pop bx
	pop ax
	jne .child
	mov byte [cs:_shell_break_hit], 1
	iret                        ; shell itself: flag it, carry on
.child:
	stc
	retf                        ; abort the child program

; ------------------------------------------------------------
; Watcom 32-bit math helpers (no default libraries).
; __U4M: DX:AX * CX:BX -> DX:AX  (low 32 bits == signed case)
; __U4D: DX:AX / CX:BX -> quotient DX:AX, remainder CX:BX
; ------------------------------------------------------------
global __U4M
global __I4M
__U4M:
__I4M:
	push si
	push cx
	push bx
	mov si, ax                  ; SI = a_lo
	xchg ax, dx                 ; AX = a_hi
	mul bx                      ; AX = a_hi * b_lo (low word)
	xchg ax, cx                 ; CX = partial high, AX = b_hi
	mul si                      ; AX = b_hi * a_lo (low word)
	add cx, ax                  ; CX = sum of cross products
	mov ax, si
	mul bx                      ; DX:AX = a_lo * b_lo
	add dx, cx
	pop bx
	pop cx
	pop si
	ret

global __U4D
__U4D:
	test cx, cx
	jnz .big
	; 16-bit divisor: two-step DIV
	mov cx, ax                  ; CX = lo dividend
	mov ax, dx
	xor dx, dx
	div bx                      ; AX = q_hi, DX = partial rem
	xchg ax, cx                 ; CX = q_hi, AX = lo dividend
	div bx                      ; AX = q_lo, DX = rem
	mov bx, dx                  ; remainder -> CX:BX = 0:rem
	mov dx, cx                  ; quotient  -> DX:AX
	xor cx, cx
	ret
.big:
	; full 32-bit shift-subtract division, 32 iterations
	push si
	push di
	push bp
	xor si, si                  ; R = SI:DI = 0
	xor di, di
	mov bp, 32
.loop:
	shl ax, 1                   ; shift R:Q left one bit
	rcl dx, 1
	rcl di, 1
	rcl si, 1
	cmp si, cx                  ; R >= divisor?
	jb  .next
	ja  .sub
	cmp di, bx
	jb  .next
.sub:
	sub di, bx
	sbb si, cx
	inc ax                      ; quotient bit
.next:
	dec bp
	jnz .loop
	mov cx, si                  ; remainder -> CX:BX
	mov bx, di
	pop bp
	pop di
	pop si
	ret

; ------------------------------------------------------------
; Class layout. wlink keeps classes in first-appearance order,
; so declaring these (empty) segments here pins the image to
; CODE, DATA, BSS, ZEND without an ORDER directive - which
; must not be used with "format dos com", as it would reset
; the group base and break the implicit 0x100 origin.
; ZEND is an initialized zero word behind _BSS: it forces wlink
; to emit the BSS range into the .COM file, so the image
; arrives pre-zeroed and needs no runtime clearing.
; ------------------------------------------------------------
segment _DATA align=2 class=DATA

; Set by break_handler_ when Ctrl-C hits while this program is
; the current process. Lives here (not in shell.c) so cstart
; links standalone into the other .COM utilities too.
global _shell_break_hit
_shell_break_hit db 0

segment _BSS align=2 class=BSS

segment ZEND align=1 class=ZEND

	dw 0

group DGROUP PSPSEG _TEXT _DATA _BSS ZEND
