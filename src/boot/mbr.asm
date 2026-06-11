; ============================================================
; mbr.asm - master boot record bootstrap (LBA 0 of the disk)
;
; Relocates itself from 0x7C00 to 0x0600, finds the active
; entry in the partition table, loads that partition's boot
; sector to 0x7C00 with an EDD packet read (AH=42h) and jumps
; to it with DL = boot drive and DS:SI = partition entry.
;
; image.ps1 copies only bytes 0..445 of this image into the
; disk; the partition table and the 55AA signature stay owned
; by the script.
; ============================================================
[BITS 16]
[ORG 0x0600]

start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7C00
	sti
	cld
	mov si, 0x7C00              ; copy ourselves out of the way
	mov di, 0x0600
	mov cx, 256
	rep movsw
	jmp 0x0000:reloc            ; continue in the relocated copy

reloc:
	mov si, 0x0600 + 0x1BE      ; partition table, 4 entries
	mov cx, 4
.scan:
	cmp byte [si], 0x80         ; active flag
	je .found
	add si, 16
	loop .scan
	mov si, msg_none
	jmp fail

.found:
	mov eax, [si + 8]           ; partition start LBA
	mov [dap_lba], eax
	push si                     ; DS:SI = partition entry (convention)
	mov si, dap
	mov ah, 0x42                ; EDD read, DL = boot drive from the BIOS
	int 0x13
	pop si
	jc .diskerr
	cmp word [0x7DFE], 0xAA55   ; boot sector signature
	jne .badvbr
	jmp 0x0000:0x7C00

.diskerr:
	mov si, msg_disk
	jmp fail
.badvbr:
	mov si, msg_vbr
	; fall through to fail

; --- fail: print DS:SI message, halt ---
fail:
	lodsb
	test al, al
	jz .hang
	mov ah, 0x0E
	mov bx, 0x0007
	int 0x10
	jmp fail
.hang:
	hlt
	jmp .hang

dap:
	db 0x10, 0                  ; packet size, reserved
	dw 1                        ; sector count
	dw 0x7C00                   ; destination offset
	dw 0                        ; destination segment
dap_lba:
	dd 0                        ; LBA low (patched at run time)
	dd 0                        ; LBA high

msg_none db 'No bootable partition', 13, 10, 0
msg_disk db 'MBR disk read error', 13, 10, 0
msg_vbr  db 'Missing boot signature', 13, 10, 0

	times 446-($-$$) db 0       ; code must fit before the table
	times 510-($-$$) db 0
	dw 0xAA55
