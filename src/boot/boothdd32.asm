; ============================================================
; boothdd32.asm - FAT32 partition boot sector (hard disk)
;
; Finds KERNEL.SYS by walking the FAT32 root directory cluster
; chain (BPB_RootClus), then loads it at 0x1000:0000 following
; its cluster chain and jumps to it with DL = the BIOS boot
; drive. All disk access is EDD packet reads (AH=42h).
;
; The BPB holds placeholders; image.sh copies this sector into
; the image and mformat patches the real BPB over offsets 3..89.
; The code reads everything from its own in-memory copy.
;
; Memory map: 0x0600 free, 0x7C00 this sector, 0x7E00 FAT
; sector cache, 0x8000+ one root-dir cluster, 0x10000+ kernel.
; ============================================================
[BITS 16]
[ORG 0x7C00]
cpu 386

KERNEL_SEG  equ 0x1000
FAT_CACHE   equ 0x7E00          ; one FAT sector
CLUS_BUF    equ 0x8000          ; one root-directory cluster

	jmp short start
	nop

; --- BPB (FAT32 EBPB) - values patched in by mformat ---
%include "version.inc"
bpbOem              db OS_OEM     ; 0x03
bpbBytesPerSec      dw 512        ; 0x0B
bpbSecPerClus       db 8          ; 0x0D
bpbReservedSecs     dw 32         ; 0x0E
bpbNumFats          db 2          ; 0x10
bpbRootEntries      dw 0          ; 0x11 (0 for FAT32)
bpbTotalSecs        dw 0          ; 0x13
bpbMedia            db 0xF8       ; 0x15
bpbSecsPerFat16     dw 0          ; 0x16 (0 for FAT32)
bpbSecsPerTrack     dw 63         ; 0x18
bpbHeads            dw 16         ; 0x1A
bpbHiddenSecs       dd 63         ; 0x1C
bpbTotalSecs32      dd 0          ; 0x20
bpbFatSz32          dd 0          ; 0x24
bpbExtFlags         dw 0          ; 0x28
bpbFsVer            dw 0          ; 0x2A
bpbRootClus         dd 2          ; 0x2C
bpbFsInfo           dw 1          ; 0x30
bpbBackupBoot       dw 6          ; 0x32
bpbReserved12       times 12 db 0 ; 0x34
bpbDriveNum         db 0x80       ; 0x40
bpbReserved1        db 0          ; 0x41
bpbBootSig          db 0x29       ; 0x42
bpbVolumeId         dd 0          ; 0x43
bpbVolumeLabel      db '           ' ; 0x47
bpbFsType           db 'FAT32   '  ; 0x52
; code starts at 0x5A

start:
	jmp 0x0000:main             ; normalize CS:IP
main:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7C00
	sti
	cld
	mov [bpbDriveNum], dl       ; remember the boot drive

	; --- fat_lba = hidden + reserved ---
	mov eax, [bpbHiddenSecs]
	movzx ebx, word [bpbReservedSecs]
	add eax, ebx
	mov [fat_lba], eax

	; --- data_lba = fat_lba + num_fats * FATSz32 ---
	movzx ecx, byte [bpbNumFats]
	mov ebx, [bpbFatSz32]
.fats:
	add eax, ebx
	loop .fats
	mov [data_lba], eax

	; --- walk the root cluster chain for KERNEL.SYS ---
	mov eax, [bpbRootClus]
.root_clus:
	push eax
	xor bx, bx
	mov es, bx
	mov bx, CLUS_BUF
	call read_cluster           ; cluster EAX -> 0:CLUS_BUF
	mov di, CLUS_BUF
	movzx cx, byte [bpbSecPerClus]
	shl cx, 4                   ; entries per cluster = spc * 16
.scan:
	push cx
	push di
	mov si, kernel_name
	mov cx, 11
	repe cmpsb
	pop di
	pop cx
	je .found
	add di, 32
	loop .scan
	pop eax
	call next_cluster
	cmp eax, 0x0FFFFFF8
	jb .root_clus
	mov si, msg_no_kernel
	jmp fail

.found:
	add sp, 4                  ; drop saved root cluster
	movzx eax, word [di + 20]  ; first cluster: high word (0x14)
	shl eax, 16
	mov ax, [di + 26]          ; low word (0x1A)

	; --- follow the KERNEL.SYS chain, one cluster at a time ---
	mov bx, KERNEL_SEG
	mov es, bx
.load:
	push eax
	xor bx, bx
	call read_cluster          ; cluster EAX -> ES:0
	movzx cx, byte [bpbSecPerClus]
	shl cx, 5                  ; paragraphs per cluster (spc*512/16)
	mov ax, es
	add ax, cx
	mov es, ax
	pop eax
	call next_cluster
	cmp eax, 0x0FFFFFF8
	jb .load

	mov dl, [bpbDriveNum]
	jmp KERNEL_SEG:0

; ------------------------------------------------------------
; read_cluster: read all sectors of cluster EAX to ES:BX.
; Clobbers EAX (-> last LBA+1) and BX (-> +cluster bytes).
; ------------------------------------------------------------
read_cluster:
	sub eax, 2
	movzx ecx, byte [bpbSecPerClus]
	push cx
	mul ecx                    ; EAX = (cl-2) * spc
	add eax, [data_lba]
	pop cx                     ; CX = sectors per cluster
.rc:
	call read_sector
	inc eax
	add bx, 512
	loop .rc
	ret

; ------------------------------------------------------------
; next_cluster: EAX = cluster -> EAX = next FAT32 cluster
; (masked to 28 bits). Uses the one-sector FAT cache.
; ------------------------------------------------------------
next_cluster:
	push bx
	push es
	push edx
	mov edx, eax               ; keep the cluster
	shr eax, 7                 ; FAT sector index = cl / 128
	cmp ax, [fat_cached]
	je .nc_ok
	mov [fat_cached], ax
	add eax, [fat_lba]
	xor bx, bx
	mov es, bx                 ; segment 0
	mov bx, FAT_CACHE
	call read_sector
.nc_ok:
	mov bx, dx
	and bx, 127
	shl bx, 2                  ; dword offset within the FAT sector
	xor ax, ax
	mov es, ax
	mov eax, [FAT_CACHE + bx]
	and eax, 0x0FFFFFFF
	pop edx
	pop es
	pop bx
	ret

; ------------------------------------------------------------
; read_sector: EDD read of 1 sector. EAX = LBA, ES:BX = dest.
; ------------------------------------------------------------
read_sector:
	pushad
	mov [dap_lba], eax
	mov [dap_off], bx
	mov ax, es
	mov [dap_seg], ax
	mov si, dap
	mov dl, [bpbDriveNum]
	mov ah, 0x42
	int 0x13
	jc .error
	popad
	ret
.error:
	mov si, msg_disk_err
	; fall through to fail

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
	db 0x10
	db 0
	dw 1                        ; count
dap_off:
	dw 0
dap_seg:
	dw 0
dap_lba:
	dd 0
	dd 0

fat_cached    dw 0xFFFF
fat_lba       dd 0
data_lba      dd 0

kernel_name   db 'KERNEL  SYS'
msg_no_kernel db 'KERNEL.SYS not found', 13, 10, 0
msg_disk_err  db 'Disk read error', 13, 10, 0

	times 510-($-$$) db 0
	dw 0xAA55
