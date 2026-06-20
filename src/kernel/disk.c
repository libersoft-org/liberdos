/* ============================================================
 * disk.c - BIOS INT 13h sector layer (floppy CHS + HDD LBA)
 *
 * Floppies (BIOS drive < 0x80) go through the classic AH=02/03
 * CHS interface with LBA->CHS conversion and a 3x retry with
 * controller reset. Hard disks use the EDD packet interface
 * (AH=42h/43h), so no geometry games are needed.
 *
 * 64 KB DMA boundary note: BIOS transfers fail if the buffer
 * crosses a physical 64 KB boundary. The kernel runs at phys
 * 0x600 (KERNEL_SEG), so its buffers stay below the boundary
 * at 0x10000 until the image grows past ~61 KB. Revisit then.
 * ============================================================ */
#include "kernel.h"

static u8  floppy_drive;
static u16 secs_per_track = 18;
static u16 num_heads = 2;

/* INT 13h AH=02h: read 1 sector. Returns 0 on success, BIOS
 * status otherwise (CF trick: sbb turns CF into a 0/FFFF mask). */
/* clang-format off */
static u16 bios_read1(u16 cylsec, u16 headdrv, u16 seg, u16 off);
#pragma aux bios_read1 = \
	"push es"            \
	"mov es,si"          \
	"mov ax,0201h"       \
	"int 13h"            \
	"sbb si,si"          \
	"and ax,si"          \
	"pop es"             \
	parm [cx] [dx] [si] [bx] value [ax] modify [si];

/* INT 13h AH=03h: write 1 sector. Same calling scheme. */
static u16 bios_write1(u16 cylsec, u16 headdrv, u16 seg, u16 off);
#pragma aux bios_write1 = \
	"push es"            \
	"mov es,si"          \
	"mov ax,0301h"       \
	"int 13h"            \
	"sbb si,si"          \
	"and ax,si"          \
	"pop es"             \
	parm [cx] [dx] [si] [bx] value [ax] modify [si];

static void bios_reset(u8 drv);
#pragma aux bios_reset = \
	"xor ah,ah"          \
	"int 13h"            \
	parm [dl] modify [ax];

/* INT 13h AH=42h/43h: EDD read/write with a disk address
 * packet at the given segment:SI. AL = 42h or 43h, DL = drive. */
static u16 bios_edd(u8 fn, u8 drv, u16 dap_seg, u16 dap_off);
#pragma aux bios_edd = \
	"push bp"           \
	"push ds"           \
	"mov ds,cx"         \
	"mov ah,al"         \
	"xor al,al"         \
	"int 13h"           \
	"sbb bx,bx"         \
	"and ax,bx"         \
	"pop ds"            \
	"pop bp"            \
	parm [al] [dl] [cx] [si] value [ax] modify [bx];
/* clang-format on */

/* EDD disk address packet. */
#pragma pack(push, 1)
typedef struct dap {
	u8  size; /* 0x10 */
	u8  zero;
	u16 count; /* sectors */
	u16 buf_off;
	u16 buf_seg;
	u32 lba_lo;
	u32 lba_hi;
} dap;
#pragma pack(pop)

void disk_init(u8 boot_drive) {
	floppy_drive = boot_drive;
}

void disk_set_geometry(u16 spt, u16 heads) {
	if (spt != 0) {
		secs_per_track = spt;
	}
	if (heads != 0) {
		num_heads = heads;
	}
}

static int floppy_io(u8 write, u32 lba, void __far *buf) {
	u16 track = (u16)(lba / secs_per_track);
	u16 sec = (u16)(lba % secs_per_track) + 1; /* 1-based */
	u16 cyl = track / num_heads;
	u16 head = track % num_heads;
	u16 cylsec = (u16)((cyl << 8) | sec);            /* CH=cyl, CL=sec */
	u16 headdrv = (u16)((head << 8) | floppy_drive); /* DH=head, DL=drive */
	int tries;
	for (tries = 0; tries < 3; tries++) {
		u16 st = write ? bios_write1(cylsec, headdrv, FP_SEG(buf), FP_OFF(buf))
		               : bios_read1(cylsec, headdrv, FP_SEG(buf), FP_OFF(buf));
		if (st == 0) {
			return 0;
		}
		bios_reset(floppy_drive);
	}
	return -1;
}

static int hdd_io(u8 write, u8 drv, u32 lba, void __far *buf) {
	int         tries;
	void __far *low_buf = MK_FP(DISK_LOW_SEG, 0); /* 512-byte sector */
	dap __far  *dp = (dap __far *)MK_FP(DISK_LOW_SEG, 512);
	if (write) {
		fmemcpy(low_buf, buf, 512);
	}
	dp->size = 0x10;
	dp->zero = 0;
	dp->count = 1;
	dp->buf_off = 0;
	dp->buf_seg = DISK_LOW_SEG;
	dp->lba_lo = lba;
	dp->lba_hi = 0;
	for (tries = 0; tries < 3; tries++) {
		if (bios_edd(write ? 0x43 : 0x42, drv, DISK_LOW_SEG, 512) == 0) {
			if (!write) {
				fmemcpy(buf, low_buf, 512);
			}
			return 0;
		}
		bios_reset(drv);
	}
	return -1;
}

/* Read one 512-byte sector of BIOS drive drv at LBA. 0 = OK. */
int disk_read(u8 drv, u32 lba, void __far *buf) {
	if (drv >= 0x80) {
		return hdd_io(0, drv, lba, buf);
	}
	return floppy_io(0, lba, buf);
}

/* Write one 512-byte sector. 0 = OK. */
int disk_write(u8 drv, u32 lba, const void __far *buf) {
	if (drv >= 0x80) {
		return hdd_io(1, drv, lba, (void __far *)buf);
	}
	return floppy_io(1, lba, (void __far *)buf);
}
