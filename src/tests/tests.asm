; ============================================================
; tests.asm - unified acceptance test suite (.COM)
;
; Sections are named after the subsystem they exercise. They
; share the print helpers and one failure counter (BP); each
; test prints "NAME PASS/FAIL ...", the summary prints the
; total and the exit code is the number of failures.
;
; INT21 (DOS call surface):
;   45h DUP, 44h IOCTL devinfo, 2Ah get date, 34h InDOS flag.
; XMS (extended memory driver):
;   INT 2Fh 4300h/4310h detect + entry point, version, query
;   free, alloc/lock/unlock/free 2 MB, A20, EMM move roundtrip.
; MOUSE (PS/2 mouse driver):
;   INT 33h reset/detect, version (PS/2), set/get position,
;   show/hide cursor, then an ~8 s window polling motion and
;   button counters while the harness injects events through
;   the QEMU monitor.
; EMS (expanded memory driver):
;   INT 67h vector + "EMMXXXX0" device-header detect, version
;   4.0, page frame segment, alloc 2 pages, map/copy roundtrip
;   through two logical pages, release, and the
;   open "EMMXXXX0" + IOCTL detection path.
; ABSDISK (absolute disk read/write):
;   INT 25h boot sector of A: and C: (jmp opcode + AA55
;   signature, flags left on the stack), bad-drive error path,
;   and an INT 26h write roundtrip (read C: boot sector, write
;   it back, re-read and compare).
; CTRLC (Ctrl-C / INT 23h):
;   install a flag-setting IRET handler and poll AH=0Bh in an
;   ~8 s window while the harness sends Ctrl-C through the
;   QEMU monitor, then repeat with a RETF/CF-clear handler to
;   prove the continue path returns to the program.
; SHARE (built-in SHARE):
;   INT 2Fh AX=1000h install check.
; ============================================================
	org 0x100

start:
	xor bp, bp                  ; global failure counter

; ============================================================
; INT21 - DOS INT 21h surface
; ============================================================
int21_run:
; --- INT21: DUP stdout and write through the new handle ---
	mov bx, 1                   ; stdout
	mov ah, 0x45
	int 21h
	jc  .t1_fail
	mov bx, ax                  ; dup'ed handle
	mov dx, int21_msg_dup
	mov cx, int21_msg_dup_len
	mov ah, 0x40                ; write via the duplicate
	int 21h
	jc  .t1_fail
	cmp ax, int21_msg_dup_len
	jne .t1_fail
	mov ah, 0x3E                ; close the duplicate
	int 21h
	jmp .t2
.t1_fail:
	inc bp
	mov dx, int21_fail_dup
	call print

; --- INT21: IOCTL get device info on stdout ---
.t2:
	mov ax, 0x4400
	mov bx, 1
	int 21h
	jc  .t2_fail
	test dx, 0x80               ; bit 7: is a device
	jz  .t2_fail
	mov dx, int21_ok_ioctl
	call print
	jmp .t5
.t2_fail:
	inc bp
	mov dx, int21_fail_ioctl
	call print

; --- INT21: get date, print the year as decimal ---
.t5:
	mov ah, 0x2A
	int 21h                     ; CX = year
	cmp cx, 1980
	jb  .t5_fail
	mov dx, int21_ok_date
	call print
	mov ax, cx
	call print_dec
	mov dx, crlf
	call print
	jmp .t6
.t5_fail:
	inc bp
	mov dx, int21_fail_date
	call print

; --- INT21: InDOS flag address must be non-null ---
.t6:
	mov ah, 0x34
	int 21h
	mov ax, es
	or  ax, bx
	jz  .t6_fail
	mov dx, int21_ok_indos
	call print
	jmp xms_run
.t6_fail:
	inc bp
	mov dx, int21_fail_indos
	call print

; ============================================================
; XMS - extended memory driver
; ============================================================
xms_run:
; --- XMS: installation check ---
	mov ax, 0x4300
	int 2Fh
	cmp al, 0x80
	jne .t1_fail
	mov ax, 0x4310              ; get entry point
	int 2Fh
	mov [xms_off], bx
	mov [xms_seg], es
	mov ax, es
	or  ax, bx
	jz  .t1_fail
	mov dx, xms_ok_detect
	call print
	jmp .t2
.t1_fail:
	inc bp
	mov dx, xms_fail_detect
	call print
	jmp .done                   ; without a driver nothing else works

; --- XMS: version ---
.t2:
	xor ah, ah                  ; fn 00h
	call far [xms_off]
	cmp ax, 0x0200
	jne .t2_fail
	mov dx, xms_ok_ver
	call print
	jmp .t3
.t2_fail:
	inc bp
	mov dx, xms_fail_ver
	call print

; --- XMS: query free extended memory ---
.t3:
	mov ah, 0x08
	call far [xms_off]
	cmp ax, 4096                ; at least 4 MB in one block
	jb  .t3_fail
	push ax
	mov dx, xms_ok_free
	call print
	pop ax
	call print_dec
	mov dx, xms_msg_kb
	call print
	jmp .t4
.t3_fail:
	inc bp
	mov dx, xms_fail_free
	call print

; --- XMS: allocate 2 MB, lock it, check the address ---
.t4:
	mov ah, 0x09
	mov dx, 2048                ; 2 MB
	call far [xms_off]
	cmp ax, 1
	jne .t4_fail
	mov [xms_handle], dx
	mov ah, 0x0C                ; lock -> DX:BX = linear base
	call far [xms_off]
	cmp ax, 1
	jne .t4_fail
	cmp dx, 0x0011              ; base must be >= 110000h
	jb  .t4_fail
	mov ah, 0x0D                ; unlock
	mov dx, [xms_handle]
	call far [xms_off]
	cmp ax, 1
	jne .t4_fail
	mov dx, xms_ok_alloc
	call print
	jmp .t5
.t4_fail:
	inc bp
	mov dx, xms_fail_alloc
	call print

; --- XMS: A20 enable + query ---
.t5:
	mov ah, 0x05                ; local enable
	call far [xms_off]
	cmp ax, 1
	jne .t5_fail
	mov ah, 0x07                ; query
	call far [xms_off]
	cmp ax, 1
	jne .t5_fail
	mov dx, xms_ok_a20
	call print
	jmp .t6
.t5_fail:
	inc bp
	mov dx, xms_fail_a20
	call print

; --- XMS: move conv -> ext, scramble, move back, verify ---
.t6:
	push cs                     ; ES still holds the XMS entry segment
	pop  es                     ; from 4310h - stosb needs ES = CS!
	mov cx, 256                 ; fill the buffer with a pattern
	mov di, xms_buf
	mov al, 0xA5
.fill:
	stosb
	inc al
	loop .fill

	; EMM: buf -> ext block offset 0
	mov word [xms_emm_len], 256
	mov word [xms_emm_len+2], 0
	mov word [xms_emm_srch], 0  ; source: conventional
	mov word [xms_emm_srco], xms_buf ; offset = seg:off
	mov [xms_emm_srco+2], cs
	mov ax, [xms_handle]
	mov [xms_emm_dsth], ax
	mov word [xms_emm_dsto], 0
	mov word [xms_emm_dsto+2], 0
	mov ah, 0x0B
	mov si, xms_emm
	call far [xms_off]
	cmp ax, 1
	jne .t6_fail

	mov cx, 256                 ; wipe the buffer
	mov di, xms_buf
	xor al, al
.wipe:
	stosb
	loop .wipe

	; EMM: ext block -> buf
	mov ax, [xms_handle]
	mov [xms_emm_srch], ax
	mov word [xms_emm_srco], 0
	mov word [xms_emm_srco+2], 0
	mov word [xms_emm_dsth], 0
	mov word [xms_emm_dsto], xms_buf
	mov [xms_emm_dsto+2], cs
	mov ah, 0x0B
	mov si, xms_emm
	call far [xms_off]
	cmp ax, 1
	jne .t6_fail

	mov cx, 256                 ; verify the pattern
	mov si, xms_buf
	mov al, 0xA5
.check:
	cmp [si], al
	jne .t6_fail
	inc si
	inc al
	loop .check
	mov dx, xms_ok_move
	call print
	jmp .t7
.t6_fail:
	inc bp
	mov dx, xms_fail_move
	call print

; --- XMS: free the block ---
.t7:
	mov ah, 0x0A
	mov dx, [xms_handle]
	call far [xms_off]
	cmp ax, 1
	jne .t7_fail
	mov dx, xms_ok_freeblk
	call print
	jmp .done
.t7_fail:
	inc bp
	mov dx, xms_fail_freeblk
	call print
.done:

; ============================================================
; MOUSE - PS/2 mouse driver
; ============================================================
mouse_run:
; --- MOUSE: reset and detect ---
	xor ax, ax
	int 33h
	cmp ax, 0xFFFF
	jne .t1_fail
	cmp bx, 2
	jne .t1_fail
	mov dx, mouse_ok_detect
	call print
	jmp .t2
.t1_fail:
	inc bp
	mov dx, mouse_fail_detect
	call print
	jmp .done                   ; no driver - nothing else to test

; --- MOUSE: version / mouse type ---
.t2:
	mov ax, 0x0024
	int 33h
	cmp bh, 8                   ; reports driver 8.x
	jne .t2_fail
	cmp ch, 4                   ; PS/2 type
	jne .t2_fail
	mov dx, mouse_ok_ver
	call print
	jmp .t3
.t2_fail:
	inc bp
	mov dx, mouse_fail_ver
	call print

; --- MOUSE: set position + read back, show/hide cursor ---
.t3:
	mov ax, 0x0004
	mov cx, 320
	mov dx, 96
	int 33h
	mov ax, 0x0001              ; show cursor
	int 33h
	mov ax, 0x0002              ; hide cursor
	int 33h
	mov ax, 0x0003
	int 33h
	cmp cx, 320
	jne .t3_fail
	cmp dx, 96
	jne .t3_fail
	mov dx, mouse_ok_pos
	call print
	jmp .t4
.t3_fail:
	inc bp
	mov dx, mouse_fail_pos
	call print

; --- MOUSE: injected motion + button (harness drives QEMU) ---
.t4:
	mov dx, mouse_wait_msg
	call print
	mov ax, 0x000B              ; flush motion counters
	int 33h
	mov ax, 0x0005              ; flush press counter, button 0
	xor bx, bx
	int 33h
	xor si, si                  ; SI = movement seen
	xor di, di                  ; DI = button press seen
	mov ax, 0x40
	mov es, ax
	mov ax, [es:0x6C]           ; BIOS tick count (18.2 Hz)
	mov [mouse_tick0], ax
.poll:
	mov ax, 0x000B
	int 33h
	mov ax, cx
	or  ax, dx
	jz  .no_move
	mov si, 1
.no_move:
	mov ax, 0x0005
	xor bx, bx
	int 33h
	test bx, bx
	jz  .no_btn
	mov di, 1
.no_btn:
	mov ax, si
	and ax, di
	jnz .t4_eval                ; both seen - stop early
	mov ax, [es:0x6C]
	sub ax, [mouse_tick0]
	cmp ax, 150                 ; ~8 seconds
	jb  .poll
.t4_eval:
	test si, si
	jz  .t4m_fail
	mov dx, mouse_ok_move
	call print
	jmp .t4b
.t4m_fail:
	inc bp
	mov dx, mouse_fail_move
	call print
.t4b:
	test di, di
	jz  .t4b_fail
	mov dx, mouse_ok_btn
	call print
	jmp .done
.t4b_fail:
	inc bp
	mov dx, mouse_fail_btn
	call print
.done:

; ============================================================
; EMS - expanded memory driver (INT 67h)
; ============================================================
ems_run:
; --- EMS: detect via vector segment : 000Ah = "EMMXXXX0" ---
	xor ax, ax
	mov es, ax
	mov ax, [es:0x67*4]         ; vector offset
	mov bx, [es:0x67*4+2]       ; vector segment
	mov cx, ax
	or  cx, bx
	jz  .t1_fail                ; vector empty - no driver
	mov es, bx
	mov si, ems_devname
	mov di, 0x0A                ; device name in the EMM header
	mov cx, 8
.cmp:
	mov al, [si]
	cmp al, [es:di]
	jne .t1_fail
	inc si
	inc di
	loop .cmp
	mov dx, ems_ok_detect
	call print
	jmp .t2
.t1_fail:
	inc bp
	mov dx, ems_fail_detect
	call print
	jmp ems_done                ; no driver - nothing else to test

; --- EMS: version 46h = 4.0 ---
.t2:
	mov ah, 0x46
	int 67h
	test ah, ah
	jnz .t2_fail
	cmp al, 0x40
	jne .t2_fail
	mov dx, ems_ok_ver
	call print
	jmp .t3
.t2_fail:
	inc bp
	mov dx, ems_fail_ver
	call print

; --- EMS: page frame segment 41h ---
.t3:
	mov ah, 0x41
	int 67h
	test ah, ah
	jnz .t3_fail
	mov [ems_frame], bx
	test bx, bx
	jz  .t3_fail
	mov dx, ems_ok_frame
	call print
	jmp .t4
.t3_fail:
	inc bp
	mov dx, ems_fail_frame
	call print
	jmp ems_done                ; no frame - mapping tests pointless

; --- EMS: free pages + allocate 2 pages ---
.t4:
	mov ah, 0x42
	int 67h
	test ah, ah
	jnz .t4_fail
	mov [ems_free0], bx         ; free pages before the alloc
	test bx, bx
	jz  .t4_fail
	mov ah, 0x43
	mov bx, 2
	int 67h
	test ah, ah
	jnz .t4_fail
	mov [ems_handle], dx
	mov dx, ems_ok_alloc
	call print
	jmp .t5
.t4_fail:
	inc bp
	mov dx, ems_fail_alloc
	call print
	jmp ems_done

; --- EMS: map/copy roundtrip across two logical pages ---
.t5:
	mov dx, [ems_handle]
	mov ax, 0x4400              ; map logical 0 -> physical 0
	xor bx, bx
	int 67h
	test ah, ah
	jnz .t5_fail
	mov es, [ems_frame]
	mov word [es:0], 0xBEEF     ; pattern A into logical page 0
	mov word [es:0x3FFE], 0xBEEF
	mov dx, [ems_handle]
	mov ax, 0x4400              ; map logical 1 -> physical 0
	mov bx, 1
	int 67h
	test ah, ah
	jnz .t5_fail
	mov es, [ems_frame]
	mov word [es:0], 0xCAFE     ; pattern B into logical page 1
	mov dx, [ems_handle]
	mov ax, 0x4400              ; back to logical 0
	xor bx, bx
	int 67h
	test ah, ah
	jnz .t5_fail
	mov es, [ems_frame]
	cmp word [es:0], 0xBEEF     ; pattern A must have survived
	jne .t5_fail
	cmp word [es:0x3FFE], 0xBEEF
	jne .t5_fail
	mov dx, [ems_handle]
	mov ax, 0x4400              ; and logical 1 again
	mov bx, 1
	int 67h
	test ah, ah
	jnz .t5_fail
	mov es, [ems_frame]
	cmp word [es:0], 0xCAFE
	jne .t5_fail
	mov dx, ems_ok_map
	call print
	jmp .t6
.t5_fail:
	inc bp
	mov dx, ems_fail_map
	call print

; --- EMS: release + device-open EMS detection ---
.t6:
	mov dx, [ems_handle]
	mov ah, 0x45                ; release handle
	int 67h
	test ah, ah
	jnz .t6_fail
	mov ah, 0x42                ; free pages must be back
	int 67h
	mov ax, bx
	cmp ax, [ems_free0]
	jne .t6_fail
	mov ax, 0x3D00              ; open "EMMXXXX0" (device detect)
	mov dx, ems_devname
	int 21h
	jc  .t6_fail
	mov bx, ax
	mov ax, 0x4400              ; IOCTL get device info
	int 21h
	jc  .t6_close
	test dx, 0x80               ; bit 7: character device
	jz  .t6_close
	mov ah, 0x3E                ; close
	int 21h
	mov dx, ems_ok_rel
	call print
	jmp ems_done
.t6_close:
	mov ah, 0x3E
	int 21h
.t6_fail:
	inc bp
	mov dx, ems_fail_rel
	call print
ems_done:

; ============================================================
; ABSDISK - INT 25h/26h absolute disk read/write
; ============================================================
absdisk_run:
; --- ABSDISK: INT 25h sector 0 of A: = floppy boot sector ---
	mov al, 0                   ; drive A:
	mov cx, 1                   ; one sector
	xor dx, dx                  ; logical sector 0
	mov bx, absdisk_buf
	int 25h
	pop si                      ; the INT-pushed flags stay on the stack
	jc  .t1_fail
	mov al, [absdisk_buf]       ; boot sector starts with a jmp
	cmp al, 0xEB
	je  .t1_sig
	cmp al, 0xE9
	jne .t1_fail
.t1_sig:
	cmp word [absdisk_buf+510], 0xAA55
	jne .t1_fail
	mov dx, absdisk_ok_flop
	call print
	jmp .t2
.t1_fail:
	inc bp
	mov dx, absdisk_fail_flop
	call print

; --- ABSDISK: INT 25h sector 0 of C: = partition boot sector ---
.t2:
	mov al, 2                   ; drive C:
	mov cx, 1
	xor dx, dx
	mov bx, absdisk_buf
	int 25h
	pop si
	jc  .t2_fail
	cmp word [absdisk_buf+510], 0xAA55
	jne .t2_fail
	mov dx, absdisk_ok_hdd
	call print
	jmp .t3
.t2_fail:
	inc bp
	mov dx, absdisk_fail_hdd
	call print
	jmp .t4                     ; no C: boot sector - skip the write test

; --- ABSDISK: INT 26h roundtrip: write the same sector back ---
.t3:
	mov al, 2
	mov cx, 1
	xor dx, dx
	mov bx, absdisk_buf         ; unmodified C: boot sector from the read
	int 26h
	pop si
	jc  .t3_fail
	mov word [absdisk_buf+510], 0 ; spoil the copy, then re-read
	mov al, 2
	mov cx, 1
	xor dx, dx
	mov bx, absdisk_buf
	int 25h
	pop si
	jc  .t3_fail
	cmp word [absdisk_buf+510], 0xAA55
	jne .t3_fail
	mov dx, absdisk_ok_write
	call print
	jmp .t4
.t3_fail:
	inc bp
	mov dx, absdisk_fail_write
	call print

; --- ABSDISK: invalid drive must set CF and return an error ---
.t4:
	mov al, 4                   ; drive E: does not exist
	mov cx, 1
	xor dx, dx
	mov bx, absdisk_buf
	int 25h
	pop si
	jnc .t4_fail
	test ax, ax                 ; error code must be nonzero
	jz  .t4_fail
	mov dx, absdisk_ok_bad
	call print
	jmp absdisk_done
.t4_fail:
	inc bp
	mov dx, absdisk_fail_bad
	call print
absdisk_done:

; ============================================================
; CTRLC - Ctrl-C / INT 23h (harness sends Ctrl-C twice)
; ============================================================
ctrlc_run:
; --- CTRLC: IRET handler fires on ^C during AH=0Bh polling ---
	mov ax, 0x2523              ; install handler 1 (DS = CS)
	mov dx, ctrlc_h1
	int 21h
	mov dx, ctrlc_wait_msg
	call print
	mov ax, 0x40
	mov es, ax
	mov ax, [es:0x6C]           ; BIOS tick count (18.2 Hz)
	mov [ctrlc_tick0], ax
.poll1:
	mov ah, 0x0B                ; ^C check happens in here
	int 21h
	cmp byte [ctrlc_flag], 0
	jne .t1_pass
	mov ax, [es:0x6C]
	sub ax, [ctrlc_tick0]
	cmp ax, 150                 ; ~8 seconds
	jb  .poll1
	inc bp
	mov dx, ctrlc_fail_fire
	call print
	jmp .t2
.t1_pass:
	mov dx, ctrlc_ok_fire
	call print

; --- CTRLC: RETF/CF-clear handler: program must continue ---
.t2:
	mov ax, 0x2523              ; install handler 2
	mov dx, ctrlc_h2
	int 21h
	mov dx, ctrlc_wait_msg
	call print
	mov ax, [es:0x6C]
	mov [ctrlc_tick0], ax
.poll2:
	mov ah, 0x0B
	int 21h
	cmp byte [ctrlc_flag2], 0
	jne .t2_pass                ; we are still alive after RETF
	mov ax, [es:0x6C]
	sub ax, [ctrlc_tick0]
	cmp ax, 150
	jb  .poll2
	inc bp
	mov dx, ctrlc_fail_cont
	call print
	jmp ctrlc_done
.t2_pass:
	mov dx, ctrlc_ok_cont
	call print
ctrlc_done:

; ============================================================
; SHARE - built-in SHARE (install check)
; ============================================================
share_run:
; --- SHARE: INT 2Fh AX=1000h -> AL=FFh (SHARE installed) ---
	mov ax, 0x1000
	int 2Fh
	cmp al, 0xFF
	je  .t1_pass
	inc bp
	mov dx, share_fail_inst
	call print
	jmp share_done
.t1_pass:
	mov dx, share_ok_inst
	call print
share_done:

; ============================================================
; SETVER - private multiplex services + per-PSP version
; ============================================================
; --- SETVER: INT 2Fh AX=F800h install check -> AL=FFh ---
	mov ax, 0F800h
	int 2Fh
	cmp al, 0FFh
	jne .t1_fail
	mov dx, setver_ok_inst
	call print
	jmp .t2
.t1_fail:
	inc bp
	mov dx, setver_fail_inst
	call print

; --- SETVER: INT 2Fh AX=F801h -> ES:BX table ptr, CX=16 ---
.t2:
	xor bx, bx
	mov es, bx
	mov ax, 0F801h
	int 2Fh
	mov ax, es
	test ax, ax
	jz  .t2_fail
	cmp cx, 16
	jne .t2_fail
	mov dx, setver_ok_tab
	call print
	jmp .t3
.t2_fail:
	inc bp
	mov dx, setver_fail_tab
	call print

; --- SETVER: fn 30h reports the PSP version word (offset 40h):
;            patch own PSP to 3.30, check, restore 5.00 ---
.t3:
	mov ah, 0x51                ; current PSP -> BX
	int 21h
	mov es, bx
	mov word [es:0x40], 0x1E03  ; 3.30 (AL=3, AH=30)
	mov ah, 0x30
	int 21h
	mov word [es:0x40], 0x0005  ; restore before judging
	cmp ax, 0x1E03
	jne .t3_fail
	mov ah, 0x30                ; default must be back
	int 21h
	cmp ax, 0x0005
	jne .t3_fail
	mov dx, setver_ok_psp
	call print
	jmp setver_done
.t3_fail:
	inc bp
	mov dx, setver_fail_psp
	call print
setver_done:
	jmp tests_summary

; --- CTRLC INT 23h handlers ---
ctrlc_h1:                       ; continue via IRET
	mov byte [cs:ctrlc_flag], 1
	iret
ctrlc_h2:                       ; continue via RETF with CF clear
	mov byte [cs:ctrlc_flag2], 1
	clc
	retf

; ============================================================
; summary + exit (code = failure count)
; ============================================================
tests_summary:
	test bp, bp
	jnz .fails
	mov dx, sum_ok
	call print
	jmp .exit
.fails:
	mov dx, sum_head
	call print
	mov ax, bp
	call print_dec
	mov dx, sum_tail
	call print
.exit:
	mov ax, bp
	mov ah, 0x4C
	int 21h

; --- print '$'-terminated string at DX ---
print:
	push ax
	mov ah, 0x09
	int 21h
	pop ax
	ret

; --- print AX as unsigned decimal ---
print_dec:
	push bx
	push cx
	push dx
	mov bx, 10
	xor cx, cx
.div:
	xor dx, dx
	div bx
	push dx
	inc cx
	test ax, ax
	jnz .div
.out:
	pop dx
	add dl, '0'
	mov ah, 0x02
	int 21h
	loop .out
	pop dx
	pop cx
	pop bx
	ret

crlf           db 13, 10, '$'

int21_msg_dup  db 'INT21 PASS dup write', 13, 10
int21_msg_dup_len equ $ - int21_msg_dup
int21_fail_dup db 'INT21 FAIL dup', 13, 10, '$'
int21_ok_ioctl db 'INT21 PASS ioctl devinfo', 13, 10, '$'
int21_fail_ioctl db 'INT21 FAIL ioctl', 13, 10, '$'
int21_ok_date  db 'INT21 PASS date year=', '$'
int21_fail_date db 'INT21 FAIL date', 13, 10, '$'
int21_ok_indos db 'INT21 PASS indos ptr', 13, 10, '$'
int21_fail_indos db 'INT21 FAIL indos', 13, 10, '$'

xms_ok_detect  db 'XMS PASS detect', 13, 10, '$'
xms_fail_detect db 'XMS FAIL detect', 13, 10, '$'
xms_ok_ver     db 'XMS PASS version 2.00', 13, 10, '$'
xms_fail_ver   db 'XMS FAIL version', 13, 10, '$'
xms_ok_free    db 'XMS PASS free ext: ', '$'
xms_msg_kb     db ' KB', 13, 10, '$'
xms_fail_free  db 'XMS FAIL query free', 13, 10, '$'
xms_ok_alloc   db 'XMS PASS alloc+lock 2MB', 13, 10, '$'
xms_fail_alloc db 'XMS FAIL alloc/lock', 13, 10, '$'
xms_ok_a20     db 'XMS PASS a20 enable', 13, 10, '$'
xms_fail_a20   db 'XMS FAIL a20', 13, 10, '$'
xms_ok_move    db 'XMS PASS move roundtrip', 13, 10, '$'
xms_fail_move  db 'XMS FAIL move', 13, 10, '$'
xms_ok_freeblk db 'XMS PASS free block', 13, 10, '$'
xms_fail_freeblk db 'XMS FAIL free block', 13, 10, '$'

mouse_ok_detect db 'MOUSE PASS detect (FFFF, 2 buttons)', 13, 10, '$'
mouse_fail_detect db 'MOUSE FAIL detect', 13, 10, '$'
mouse_ok_ver   db 'MOUSE PASS version (8.x, PS/2)', 13, 10, '$'
mouse_fail_ver db 'MOUSE FAIL version', 13, 10, '$'
mouse_ok_pos   db 'MOUSE PASS set/get position', 13, 10, '$'
mouse_fail_pos db 'MOUSE FAIL set/get position', 13, 10, '$'
mouse_wait_msg db 'MOUSE WAIT injecting events...', 13, 10, '$'
mouse_ok_move  db 'MOUSE PASS motion counters', 13, 10, '$'
mouse_fail_move db 'MOUSE FAIL motion counters', 13, 10, '$'
mouse_ok_btn   db 'MOUSE PASS button press', 13, 10, '$'
mouse_fail_btn db 'MOUSE FAIL button press', 13, 10, '$'

sum_ok         db 'TESTS: all 32 passed', 13, 10, '$'
sum_head       db 'TESTS: ', '$'
sum_tail       db ' FAILED', 13, 10, '$'

mouse_tick0    dw 0

xms_off        dw 0
xms_seg        dw 0
xms_handle     dw 0

xms_emm:                        ; EMM move structure
xms_emm_len    dd 0
xms_emm_srch   dw 0
xms_emm_srco   dd 0
xms_emm_dsth   dw 0
xms_emm_dsto   dd 0

xms_buf        times 256 db 0

ems_devname    db 'EMMXXXX0', 0
ems_ok_detect  db 'EMS PASS detect (EMMXXXX0)', 13, 10, '$'
ems_fail_detect db 'EMS FAIL detect', 13, 10, '$'
ems_ok_ver     db 'EMS PASS version 4.0', 13, 10, '$'
ems_fail_ver   db 'EMS FAIL version', 13, 10, '$'
ems_ok_frame   db 'EMS PASS page frame', 13, 10, '$'
ems_fail_frame db 'EMS FAIL page frame', 13, 10, '$'
ems_ok_alloc   db 'EMS PASS alloc 2 pages', 13, 10, '$'
ems_fail_alloc db 'EMS FAIL alloc', 13, 10, '$'
ems_ok_map     db 'EMS PASS map roundtrip', 13, 10, '$'
ems_fail_map   db 'EMS FAIL map roundtrip', 13, 10, '$'
ems_ok_rel     db 'EMS PASS release + device open', 13, 10, '$'
ems_fail_rel   db 'EMS FAIL release/device', 13, 10, '$'
ems_frame      dw 0
ems_free0      dw 0
ems_handle     dw 0

absdisk_ok_flop db 'ABSDISK PASS read A: boot', 13, 10, '$'
absdisk_fail_flop db 'ABSDISK FAIL read A:', 13, 10, '$'
absdisk_ok_hdd db 'ABSDISK PASS read C: boot', 13, 10, '$'
absdisk_fail_hdd db 'ABSDISK FAIL read C:', 13, 10, '$'
absdisk_ok_write db 'ABSDISK PASS write roundtrip', 13, 10, '$'
absdisk_fail_write db 'ABSDISK FAIL write', 13, 10, '$'
absdisk_ok_bad db 'ABSDISK PASS bad drive error', 13, 10, '$'
absdisk_fail_bad db 'ABSDISK FAIL bad drive', 13, 10, '$'
absdisk_buf    times 512 db 0

ctrlc_wait_msg db 'CTRLC WAIT send Ctrl-C...', 13, 10, '$'
ctrlc_ok_fire  db 'CTRLC PASS int23 handler fired', 13, 10, '$'
ctrlc_fail_fire db 'CTRLC FAIL int23 handler', 13, 10, '$'
ctrlc_ok_cont  db 'CTRLC PASS retf continue path', 13, 10, '$'
ctrlc_fail_cont db 'CTRLC FAIL retf continue', 13, 10, '$'
ctrlc_flag     db 0
ctrlc_flag2    db 0
ctrlc_tick0    dw 0

share_ok_inst  db 'SHARE PASS installed', 13, 10, '$'
share_fail_inst db 'SHARE FAIL install check', 13, 10, '$'

setver_ok_inst db 'SETVER PASS services installed', 13, 10, '$'
setver_fail_inst db 'SETVER FAIL install check', 13, 10, '$'
setver_ok_tab  db 'SETVER PASS table ptr (16 entries)', 13, 10, '$'
setver_fail_tab db 'SETVER FAIL table ptr', 13, 10, '$'
setver_ok_psp  db 'SETVER PASS psp version word (3.30/5.00)', 13, 10, '$'
setver_fail_psp db 'SETVER FAIL psp version word', 13, 10, '$'
