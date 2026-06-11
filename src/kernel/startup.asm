; ============================================================
; startup.asm - kernel entry point and low-level stubs
;
; The kernel is a flat binary (wlink "format raw bin") in the
; Watcom TINY layout: code is compiled with -ms (small) and the
; linker trick below puts _TEXT into DGROUP, so a single 64 KB
; segment holds everything and CS = DS = SS = KERNEL_SEG (the
; boot sector loads the image at 0x1000 and the entry code
; relocates it down, see kentry). This is exactly how Watcom
; builds .COM programs - there is no -mt switch, tiny IS small
; plus this group arrangement.
;
; Image layout (ORDER directive in the generated kernel.lnk):
;   offset 0:  _TEXT  (this file first -> entry at 0, then C)
;              KDATA  (kernel asm data)
;              _DATA, CONST, CONST2  (C data)
;              _BSS   (C bss)
;              KSTACK (kernel stacks, zero-initialized)
;
; KSTACK is initialized (zeros) and ordered after _BSS, which
; forces wlink to emit the BSS range into the raw binary - no
; runtime BSS clearing is needed.
;
; No segment relocations anywhere: all segment registers are
; derived from CS at run time, all fixups are group-relative
; offsets. That is what lets wlink produce a raw binary.
; ============================================================
cpu 386
bits 16

extern _kernel_main             ; main.c,  void __cdecl kernel_main(u16)
extern _int21_dispatch          ; int21.c, void __cdecl int21_dispatch(iregs __far *)
extern _absdisk_dispatch        ; fat.c,   void __cdecl absdisk_dispatch(iregs __far *)
extern _absdisk_cf              ; fat.c,   u8: 1 = error -> live CF on return
extern _xms_dispatch            ; xms.c,   void __cdecl xms_dispatch(xframe __far *)
extern _ems_dispatch            ; ems.c,   void __cdecl ems_dispatch(iregs __far *)
extern _mouse_dispatch          ; mouse.c, void __cdecl mouse_dispatch(iregs __far *)
extern _mouse_event             ; mouse.c, void __cdecl mouse_event(u16, u16, u16)
extern _muh_mask                ; mouse.c, user event handler call parameters
extern _muh_buttons
extern _muh_x
extern _muh_y
extern _muh_mx
extern _muh_my
extern _muh_handler

segment _TEXT align=1 class=CODE

; ------------------------------------------------------------
; Kernel entry. Boot sector jumps to 0x1000:0000 with
; DL = boot drive. Linked first, so this is image offset 0.
;
; The kernel immediately relocates itself down to KERNEL_SEG
; (0x0060, phys 0x600 - right above the BIOS data area) and
; far-jumps there. The image has no segment fixups, so a plain
; copy works; loading low directly is impossible because the
; boot sector and its work buffers occupy 0x7C00+ while it is
; still reading the kernel. This frees the whole 0x600..0xFFFF
; region (~62 KB) for the conventional-memory arena.
;
; The jump leaves room for the "EMMXXXX0" signature at offset
; 000Ah: EMS detection reads the INT 67h vector and expects
; the EMM device name at vector_segment:000Ah. With the name
; right here the vector can simply use the kernel segment.
; ------------------------------------------------------------
KERNEL_SEG equ 0x0060           ; final kernel segment (keep in sync
                                ; with KERNEL_SEG in kernel.h)
..start:
	jmp kentry                  ; 3 bytes
	times 0x0A - ($ - $$) db 0  ; pad to offset 000Ah
	db 'EMMXXXX0'               ; EMM device name for EMS detection
kentry:
	cli                         ; the copy overwrites the boot stack:
	cld                         ; no IRQs until the new stack is live
	mov ax, cs
	mov ds, ax
	cmp ax, KERNEL_SEG          ; already low? (defensive)
	je .relocated
	mov ax, KERNEL_SEG
	mov es, ax
	xor si, si
	xor di, di
	mov cx, [_kernel_end_off]   ; whole image incl. KSTACK, in words
	shr cx, 1
	inc cx
	rep movsw                   ; DL (boot drive) survives the copy
	jmp KERNEL_SEG:.relocated
.relocated:
	mov ax, cs                  ; tiny layout: one segment for everything
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, boot_stack_top
	sti
	xor dh, dh
	push dx                     ; arg: boot drive (cdecl)
	call _kernel_main
.hang:                          ; kernel_main should never return
	cli
	hlt
	jmp .hang

; ------------------------------------------------------------
; INT 21h entry stub.
;  1. push all registers on the CALLER's stack -> iregs frame
;  2. switch to the kernel INT 21h stack (SS = DS = kernel seg)
;  3. call C: int21_dispatch(iregs __far *frame)
;  4. switch back, pop (possibly modified) registers, iret
;
; Not re-entrant (single kernel stack) - DOS solves this with
; the InDOS flag.
; ------------------------------------------------------------
global _int21_entry_
_int21_entry_:
	push es
	push ds
	push bp
	push di
	push si
	push dx
	push cx
	push bx
	push ax
	mov bp, sp                  ; BP = frame offset on caller stack
	mov dx, ss                  ; DX = frame segment
	mov ax, cs
	mov ds, ax                  ; DS = kernel segment
	mov [_int21_user_ss], dx
	mov [_int21_user_sp], bp
	mov ss, ax                  ; (mov ss inhibits interrupts for 1 instr)
	mov sp, int21_stack_top
	sti
	cld
	push dx                     ; arg: far ptr to frame - segment
	push bp                     ;                         offset
	call _int21_dispatch
	cli
	mov ss, [_int21_user_ss]
	mov sp, [_int21_user_sp]
	cmp byte [_int23_pending], 0
	jne int21_break
	pop ax
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	iret

; Ctrl-C was seen during the dispatch: invoke the INT 23h
; handler on the caller's stack with the caller's registers,
; DOS style. The handler may return three ways:
;   IRET            -> continue, finish the original INT 21h
;   RETF, CF clear  -> same (the saved flags word is dropped)
;   RETF, CF set    -> abort the program (terminate, AH = 1)
; The paths are told apart by comparing SP (IRET pops one word
; more) - flags must not be touched before the CF test, so the
; handler's flags are parked in _int23_flags via PUSHF/POP mem.
int21_break:
	mov byte [_int23_pending], 0
	xor ax, ax                  ; fetch the live vector (DS = kernel)
	mov es, ax
	mov ax, [es:008Ch]
	mov [_int23_vec], ax
	mov ax, [es:008Eh]
	mov [_int23_vec+2], ax
	pop ax                      ; caller registers back
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	mov [cs:_int23_spchk], sp
	sti
	pushf                       ; INT-style frame for the handler
	call far [cs:_int23_vec]
	pushf                       ; capture the handler's flags
	pop word [cs:_int23_flags]
	cmp sp, [cs:_int23_spchk]
	je .continue                ; IRET return: stack is level again
	test byte [cs:_int23_flags], 1
	jnz .abort                  ; RETF + CF set
	popf                        ; RETF + CF clear: drop saved flags
.continue:
	iret                        ; finish the original INT 21h
.abort:
	popf                        ; level the stack
	mov byte [cs:_break_abort], 1
	mov ax, 4C00h
	int 21h                     ; terminate the process; no return

; ------------------------------------------------------------
; INT 1Bh - BIOS Ctrl-Break notification, raised from the IRQ1
; handler. Just latch a flag; the console functions poll it.
; ------------------------------------------------------------
_int1b_entry_:
	mov byte [cs:_ctrl_break_hit], 1
	iret

; ------------------------------------------------------------
; kbd_peek - non-destructive keyboard peek (INT 16h AH=01h).
; Returns AX = waiting key (scan<<8 | char) or FFFFh if none.
; Lives here because the ZF test needs a branch, which Watcom
; pragma aux bodies cannot express.
; ------------------------------------------------------------
global kbd_peek_
kbd_peek_:
	push bp                     ; some BIOSes clobber BP
	mov ah, 1
	int 16h
	jnz .have
	mov ax, 0FFFFh
.have:
	pop bp
	ret

; ------------------------------------------------------------
; INT 20h - terminate program (CP/M style). The frame pushed
; by INT is identical to INT 21h's, so set AH = 00h (terminate
; function) and reuse the whole INT 21h path.
; ------------------------------------------------------------
global _int20_entry_
_int20_entry_:
	xor ah, ah
	jmp _int21_entry_

; ------------------------------------------------------------
; exec_launch - start a child process. proc.c fills the exec_*
; variables; this never returns. All values are loaded into
; registers first because switching DS away from the kernel
; segment makes the variables unreachable.
; ------------------------------------------------------------
global exec_launch_
exec_launch_:
	cli
	mov bx, [_exec_cs]
	mov cx, [_exec_ip]
	mov si, [_exec_ss]
	mov di, [_exec_sp]
	mov dx, [_exec_psp]
	mov ss, si
	mov sp, di
	push bx                     ; child CS
	push cx                     ; child IP
	mov ds, dx
	mov es, dx
	xor ax, ax                  ; AX = 0: FCB drive specifiers "valid"
	xor bx, bx
	xor cx, cx
	xor si, si
	xor di, di
	xor bp, bp
	cld
	sti
	retf

; ------------------------------------------------------------
; term_return - resume the parent after a child terminates.
; proc.c sets exec_ss/exec_sp to the parent's SS:SP saved at
; EXEC time; the parent stack still holds its complete iregs
; frame, so the epilogue mirrors the INT 21h stub.
; ------------------------------------------------------------
global term_return_
term_return_:
	cli
	mov ss, [_exec_ss]
	mov sp, [_exec_sp]
	pop ax
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	iret

; ------------------------------------------------------------
; INT 25h/26h - absolute disk read/write. Same stack-switch
; pattern as INT 21h, but with the documented return quirk:
; the handler exits with RETF, leaving the flags word the INT
; instruction pushed ON the caller's stack (the caller pops or
; adds sp,2), and the status is carried in the LIVE flags
; (CF set = error, AX = code). absdisk_cf -> CF via SHR, done
; while DS still addresses the kernel; the register pops that
; follow do not touch flags.
;
; AH is undefined on entry for these vectors, so the stub uses
; it as the read/write selector inside the saved frame (the
; dispatcher reads it from r->ax).
; ------------------------------------------------------------
_int25_entry_:
	mov ah, 0                   ; read
	jmp short absdisk_common
_int26_entry_:
	mov ah, 1                   ; write
absdisk_common:
	push es
	push ds
	push bp
	push di
	push si
	push dx
	push cx
	push bx
	push ax                     ; AH = selector, AL = drive
	mov bp, sp                  ; BP:DX = far ptr to the frame
	mov dx, ss
	mov ax, cs
	mov ds, ax                  ; DS = kernel segment
	mov [_absdisk_user_ss], dx
	mov [_absdisk_user_sp], bp
	mov ss, ax
	mov sp, absdisk_stack_top
	cld
	push dx                     ; arg: far ptr to frame
	push bp
	call _absdisk_dispatch
	cli
	mov ss, [_absdisk_user_ss]
	mov sp, [_absdisk_user_sp]
	shr byte [_absdisk_cf], 1   ; CF = status bit (DS = kernel here)
	pop ax                      ; pops do not modify flags
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	sti                         ; STI does not modify CF
	retf                        ; flags word stays on the stack

; ------------------------------------------------------------
; Default INT 22h/23h/24h handlers and the country-info case
; map routine. INT 22h is a return address, never an
; interrupt: if a program jumps there, terminate it cleanly.
; INT 23h (Ctrl-Break): abort the program (RETF with CF set,
; the DOS default). INT 24h (critical error): AL = 3 -> fail
; the operation. Casemap: identity, RETF.
; ------------------------------------------------------------
_int22_stub_:
	mov ax, 4C00h
	int 21h
_int23_stub_:
	stc
	retf
_int24_stub_:
	mov al, 3
	iret
_casemap_:
	retf

; ------------------------------------------------------------
; INT 2Fh multiplex handler. The XMS install check
; and entry point are answered, plus the SHARE install check
; (AX=1000h, AL=FFh: the file-sharing layer is built into the
; kernel - see share.c) and the private SETVER table services
; (AH=F8h, OEM-reserved range: AL=00h install check -> AL=FFh,
; AL=01h -> ES:BX = the version table, CX = entry count; the
; shell's SETVER command edits the table in place - setver.c).
; Everything else falls through with all registers unchanged,
; which is the correct "not here" reply for multiplex queries
; (e.g. DPMI 1687h keeps AX nonzero).
; ------------------------------------------------------------
_int2f_entry_:
	cmp ax, 4300h               ; XMS installation check
	je  .inst
	cmp ax, 4310h               ; XMS get entry point
	je  .entry
	cmp ax, 1000h               ; SHARE installation check
	je  .share
	cmp ax, 0F800h              ; SETVER services install check
	je  .sv_inst
	cmp ax, 0F801h              ; SETVER get table -> ES:BX, CX
	je  .sv_table
	iret
.sv_inst:
	mov al, 0FFh                ; SETVER services present
	iret
.sv_table:
	mov bx, cs
	mov es, bx
	mov bx, _setver_table
	mov cx, 16                  ; SETVER_MAX (kernel.h)
	iret
.share:
	mov al, 0FFh                ; SHARE present (built in)
	iret
.inst:
	mov al, 80h                 ; XMS driver present
	iret
.entry:
	mov bx, cs
	mov es, bx
	mov bx, _xms_entry_
	iret

; ------------------------------------------------------------
; XMS driver entry point, far-called by clients with AH = the
; function code. Same stack-switch pattern as the INT 21h stub:
; push an xframe on the caller's stack, run the C dispatcher on
; a private kernel stack, pop the (modified) registers back.
; The 5-byte hookable prologue is required by the XMS spec.
; ------------------------------------------------------------
_xms_entry_:
	jmp short .go               ; 2 bytes
	nop                         ; +3 = the documented 5-byte prologue
	nop
	nop
.go:
	push es
	push ds
	push bp
	push di
	push si
	push dx
	push cx
	push bx
	push ax
	mov bp, sp                  ; BP:DX = far pointer to the frame
	mov dx, ss
	mov ax, cs
	mov ds, ax
	mov [_xms_user_ss], dx
	mov [_xms_user_sp], bp
	mov ss, ax
	mov sp, xms_stack_top
	cld
	push dx
	push bp
	call _xms_dispatch
	mov ss, [_xms_user_ss]      ; (mov ss inhibits interrupts for 1 instr)
	mov sp, [_xms_user_sp]
	pop ax
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	retf

; ------------------------------------------------------------
; pci_pam_unlock - open the UMB RAM at D0000-DFFFF on Intel
; 440FX-class host bridges: PAM3 (reg 5Ch, D0000-D7FFF) and
; PAM4 (reg 5Dh, D8000-DFFFF) set to 33h = read+write enable
; for both 16 KB halves. Uses PCI config mechanism #1 (the
; 32-bit address write to CF8h needs a true OUT DX,EAX, which
; is why this lives here and not in a Watcom pragma).
; Returns AX = PAM3 | PAM4<<8 read back (FFFFh = no PCI).
; ------------------------------------------------------------
global pci_pam_unlock_
pci_pam_unlock_:
	push dx
	mov dx, 0xCF8               ; config address: bus 0 dev 0 fn 0 reg 5Ch
	mov eax, 0x8000005C
	out dx, eax
	mov dx, 0xCFC               ; config data, byte lane 0 = reg 5Ch
	mov al, 0x33
	out dx, al                  ; PAM3
	inc dx                      ; byte lane 1 = reg 5Dh
	out dx, al                  ; PAM4
	dec dx
	in  al, dx                  ; read back PAM3
	mov ah, al
	inc dx
	in  al, dx                  ; read back PAM4
	xchg al, ah                 ; AL = PAM3, AH = PAM4
	pop dx
	ret

; ------------------------------------------------------------
; INT 67h entry (EMS driver, ems.c). Same stack-switch pattern
; as INT 33h; interrupts stay disabled (the 16 KB page copies
; run through INT 15h AH=87h, which is CLI inside the BIOS
; anyway). EMS detection finds the "EMMXXXX0" signature at
; vector_segment:000Ah - see the kernel entry at offset 0.
; ------------------------------------------------------------
_int67_entry_:
	push es
	push ds
	push bp
	push di
	push si
	push dx
	push cx
	push bx
	push ax
	mov bp, sp                  ; BP:DX = far ptr to the frame
	mov dx, ss
	mov ax, cs
	mov ds, ax                  ; DS = kernel segment
	mov [_ems_user_ss], dx
	mov [_ems_user_sp], bp
	mov ss, ax
	mov sp, ems_stack_top
	cld
	push dx                     ; arg: far ptr to frame
	push bp
	call _ems_dispatch
	mov ss, [_ems_user_ss]      ; (mov ss inhibits interrupts for 1 instr)
	mov sp, [_ems_user_sp]
	pop ax
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	iret

; ------------------------------------------------------------
; INT 33h - mouse services. Same stack-switch pattern as the
; INT 21h stub, but interrupts stay DISABLED for the whole
; dispatch (the functions are tiny): with IF=0 the IRQ12 event
; path can never interrupt a dispatch, so the dedicated stack
; and the saved-SS:SP pair cannot be clobbered by a user event
; callback that itself calls INT 33h.
; ------------------------------------------------------------
global _int33_entry_
_int33_entry_:
	push es
	push ds
	push bp
	push di
	push si
	push dx
	push cx
	push bx
	push ax
	mov bp, sp                  ; BP:DX = far ptr to the frame
	mov dx, ss
	mov ax, cs
	mov ds, ax                  ; DS = kernel segment
	mov [_int33_user_ss], dx
	mov [_int33_user_sp], bp
	mov ss, ax
	mov sp, mouse_stack_top
	cld
	push dx                     ; arg: far ptr to frame
	push bp
	call _mouse_dispatch
	mov ss, [_int33_user_ss]    ; (mov ss inhibits interrupts for 1 instr)
	mov sp, [_int33_user_sp]
	pop ax
	pop bx
	pop cx
	pop dx
	pop si
	pop di
	pop bp
	pop ds
	pop es
	iret

; ------------------------------------------------------------
; PS/2 mouse BIOS callback (INT 15h C207h). The BIOS pushes
; four words - status, dX, dY, dZ - and far-calls this routine
; on every packet. Forward to C: mouse_event(status, dx, dy).
; ------------------------------------------------------------
_mouse_bios_cb_:
	pusha
	push ds
	push es
	mov bp, sp                  ; args above: 2 regs*2 + pusha 16 + ret 4
	mov ax, cs
	mov ds, ax
	cld
	push word [bp+26]           ; dY   (pushed 3rd by the BIOS)
	push word [bp+28]           ; dX   (pushed 2nd)
	push word [bp+30]           ; status (pushed 1st = highest address)
	call _mouse_event
	add sp, 6
	pop es
	pop ds
	popa
	retf

; ------------------------------------------------------------
; mouse_hw_init - set up the PS/2 pointing device via INT 15h:
; C205h init (3-byte packets), C203h resolution, C207h install
; _mouse_bios_cb_ as the device driver, C200h enable stream.
; Returns AX = 0 on success, 1 if any step fails (no mouse).
; ------------------------------------------------------------
global mouse_hw_init_
mouse_hw_init_:
	push es
	push bp                     ; some BIOSes trash BP across INT 15h
	push bx
	push cx
	push dx
	mov ax, 0C205h              ; initialize, BH = data package size
	mov bh, 3
	int 15h
	jc  .fail
	mov ax, 0C203h              ; resolution BH = 3 (8 counts/mm)
	mov bh, 3
	int 15h
	jc  .fail
	mov ax, cs                  ; install device driver far ptr ES:BX
	mov es, ax
	mov bx, _mouse_bios_cb_
	mov ax, 0C207h
	int 15h
	jc  .fail
	mov ax, 0C200h              ; enable, BH = 1
	mov bh, 1
	int 15h
	jc  .fail
	xor ax, ax
.done:
	pop dx
	pop cx
	pop bx
	pop bp
	pop es
	ret
.fail:
	mov ax, 1
	jmp .done

; ------------------------------------------------------------
; mouse_call_user - invoke the INT 33h 0Ch user event handler.
; mouse.c fills the muh_* variables, then calls this. Registers
; per spec: AX = event mask, BX = buttons, CX = X, DX = Y,
; SI/DI = raw mickey counts.
; ------------------------------------------------------------
global mouse_call_user_
mouse_call_user_:
	pusha
	push ds
	push es
	mov ax, [_muh_mask]
	mov bx, [_muh_buttons]
	mov cx, [_muh_x]
	mov dx, [_muh_y]
	mov si, [_muh_mx]
	mov di, [_muh_my]
	call far [_muh_handler]
	pop es
	pop ds
	popa
	ret

; ------------------------------------------------------------
; Watcom 32-bit math helpers. The C runtime normally provides
; these, but we link with -zl (no default libraries).
;
; __U4M: DX:AX * CX:BX -> DX:AX   (also valid for signed __I4M,
;        the low 32 bits of the product are the same)
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
; Kernel asm data.
; ------------------------------------------------------------
segment KDATA align=2 class=KDATA

global _int21_entry_off
global _int20_entry_off
_int21_entry_off  dw _int21_entry_ ; image offsets of the stubs, for the
_int20_entry_off  dw _int20_entry_ ; IVT install code in main.c

global _int22_stub_off
global _int23_stub_off
global _int24_stub_off
global _casemap_off
_int22_stub_off   dw _int22_stub_
_int23_stub_off   dw _int23_stub_
_int24_stub_off   dw _int24_stub_
_casemap_off      dw _casemap_

global _int2f_entry_off
_int2f_entry_off  dw _int2f_entry_

global _int25_entry_off
global _int26_entry_off
_int25_entry_off  dw _int25_entry_
_int26_entry_off  dw _int26_entry_

global _int1b_entry_off
_int1b_entry_off  dw _int1b_entry_

extern _int23_pending           ; int21.c: ^C seen, invoke INT 23h
extern _break_abort             ; int21.c: termination via Ctrl-C
extern _ctrl_break_hit          ; int21.c: latched by INT 1Bh
extern _setver_table            ; setver.c: version override table
_int23_vec        dd 0          ; scratch for the INT 23h call
_int23_spchk      dw 0          ; SP before PUSHF (IRET/RETF test)
_int23_flags      dw 0          ; handler's returned flags

_absdisk_user_ss  dw 0          ; caller stack of the INT 25h/26h
_absdisk_user_sp  dw 0

global _int33_entry_off
_int33_entry_off  dw _int33_entry_

global _int67_entry_off
_int67_entry_off  dw _int67_entry_ ; INT 67h handler image offset

global _ems_user_ss
global _ems_user_sp
_ems_user_ss      dw 0          ; caller stack of the current INT 67h
_ems_user_sp      dw 0

global _xms_user_ss
global _xms_user_sp
_xms_user_ss      dw 0          ; caller stack of the current XMS call
_xms_user_sp      dw 0

global _int21_user_ss
global _int21_user_sp
_int21_user_ss    dw 0          ; caller stack of the current INT 21h
_int21_user_sp    dw 0

_int33_user_ss    dw 0          ; caller stack of the current INT 33h
_int33_user_sp    dw 0

global _kernel_end_off
_kernel_end_off   dw absdisk_stack_top ; image size = first free byte offset

global _exec_cs
global _exec_ip
global _exec_ss
global _exec_sp
global _exec_psp
_exec_cs          dw 0          ; exec_launch / term_return parameters
_exec_ip          dw 0
_exec_ss          dw 0
_exec_sp          dw 0
_exec_psp         dw 0

; ------------------------------------------------------------
; Kernel stacks. Initialized (zeros) so that wlink emits them -
; and everything before them, including _BSS - into the binary.
; ------------------------------------------------------------
segment KSTACK align=2 class=KSTACK

boot_stack      times 1024 db 0 ; stack for kernel_main
boot_stack_top:
int21_stack     times 1024 db 0 ; stack for INT 20h/21h dispatch
int21_stack_top:
xms_stack       times 512 db 0  ; stack for XMS far calls
xms_stack_top:
mouse_stack     times 512 db 0  ; stack for INT 33h dispatch
mouse_stack_top:
ems_stack       times 512 db 0  ; stack for INT 67h dispatch
ems_stack_top:
absdisk_stack   times 512 db 0  ; stack for INT 25h/26h dispatch
absdisk_stack_top:

; The tiny-model trick: _TEXT joins DGROUP, the group starts at
; image offset 0, so group-relative data offsets equal image
; offsets and DS = SS = CS works with no segment fixups.
group DGROUP _TEXT KDATA KSTACK
