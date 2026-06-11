/* ============================================================
 * xms.c - XMS 2.0 driver built into the kernel
 *
 * Provides the standard HIMEM.SYS interface: INT 2Fh AX=4300h
 * (installation check) and AX=4310h (entry point), with the
 * driver functions dispatched here from the far-call stub in
 * startup.asm. DOS extenders use this to allocate extended
 * memory.
 *
 * Extended memory is detected with INT 15h AX=E801h (fallback
 * AH=88h) and managed as a simple first-fit pool of KB-granular
 * blocks above the HMA (linear 0x110000). A20 is switched via
 * port 92h (fast A20, fine on QEMU and anything modern). The
 * Move function copies through BIOS INT 15h AH=87h, so the
 * kernel itself never leaves real mode.
 *
 * Version reports 2.0: callers stick to the 16-bit functions
 * 00h-11h and never expect the 3.0 super-extended calls.
 * ============================================================ */
#include "kernel.h"

#define XMS_NHANDLES  16
#define POOL_START_KB 1088UL /* 1 MB + 64 KB (leave the HMA alone) */

typedef struct xhandle {
	u8  used;
	u8  locks;
	u32 base_kb;
	u32 size_kb;
} xhandle;

static xhandle xh[XMS_NHANDLES];
static u32     pool_end_kb = POOL_START_KB; /* empty until init */
static u8      hma_used = 0;
static u8      a20_count = 0;

/* Register frame built by the XMS entry stub in startup.asm.
 * Field order must match the push sequence there. */
typedef struct xframe {
	u16 ax, bx, cx, dx, si, di, bp, ds, es;
} xframe;

/* XMS error codes (returned in BL with AX=0). */
#define XE_NOT_IMPL   0x80
#define XE_A20        0x82
#define XE_HMA_INUSE  0x91
#define XE_HMA_FREE   0x93
#define XE_OUT_OF_MEM 0xA0
#define XE_NO_HANDLES 0xA1
#define XE_BAD_HANDLE 0xA2
#define XE_BAD_SRC_H  0xA3
#define XE_BAD_SRC_O  0xA4
#define XE_BAD_DST_H  0xA5
#define XE_BAD_DST_O  0xA6
#define XE_BAD_LEN    0xA7
#define XE_NOT_LOCKED 0xAA
#define XE_LOCKED     0xAB

/* INT 15h AX=E801h: AX = KB between 1 MB and 16 MB (max 3C00h).
 * Returns 0 when the call is not supported (CF set). */
/* clang-format off */
static u16 bios_e801_kb(void);
#pragma aux bios_e801_kb = \
	"push bp"           \
	"mov ax,0E801h"     \
	"xor cx,cx"         \
	"xor dx,dx"         \
	"int 15h"           \
	"sbb bx,bx"         \
	"not bx"            \
	"and ax,bx"         \
	"pop bp"            \
	value [ax] modify [bx cx dx];

/* INT 15h AH=88h: AX = KB above 1 MB (old interface). */
static u16 bios_88_kb(void);
#pragma aux bios_88_kb = \
	"push bp"           \
	"mov ah,88h"        \
	"int 15h"           \
	"sbb bx,bx"         \
	"not bx"            \
	"and ax,bx"         \
	"pop bp"            \
	value [ax] modify [bx];

/* INT 15h AH=87h: copy CX words using the 6-descriptor GDT at
 * ES:SI. Returns the BIOS status (0 = OK). */
static u16 bios_move(void *gdtp, u16 words);
#pragma aux bios_move = \
	"push bp"           \
	"push es"           \
	"mov ax,ds"         \
	"mov es,ax"         \
	"mov ah,87h"        \
	"int 15h"           \
	"mov al,ah"         \
	"xor ah,ah"         \
	"pop es"            \
	"pop bp"            \
	parm [si] [cx] value [ax] modify [bx dx];
/* clang-format on */

/* --- A20 via port 92h --- */

static void a20_set(u8 on) {
	u8 v = inb(0x92);
	if (on) {
		v |= 0x02;
	} else {
		v &= 0xFD;
	}
	v &= 0xFE; /* never touch the fast-reset bit */
	outb(0x92, v);
}

static u8 a20_get(void) {
	return (u8)((inb(0x92) >> 1) & 1);
}

/* --- pool allocator (first fit over a sorted gap walk) --- */

/* Find the used block with the lowest base >= from. Returns the
 * handle index or -1 when no block lies above from. */
static int next_block_above(u32 from) {
	int best = -1;
	u16 i;
	for (i = 0; i < XMS_NHANDLES; i++) {
		if (xh[i].used && xh[i].base_kb >= from) {
			if (best < 0 || xh[i].base_kb < xh[best].base_kb) {
				best = (int)i;
			}
		}
	}
	return best;
}

/* First-fit: find a free gap of size_kb. 0 = no room. */
static u32 find_gap(u32 size_kb) {
	u32 cand = POOL_START_KB;
	for (;;) {
		int nb = next_block_above(cand);
		u32 limit = (nb < 0) ? pool_end_kb : xh[nb].base_kb;
		if (cand + size_kb <= limit) {
			return cand;
		}
		if (nb < 0) {
			return 0;
		}
		cand = xh[nb].base_kb + xh[nb].size_kb;
	}
}

static void query_free(u32 *largest, u32 *total) {
	u32 cand = POOL_START_KB;
	*largest = 0;
	*total = 0;
	for (;;) {
		int nb = next_block_above(cand);
		u32 limit = (nb < 0) ? pool_end_kb : xh[nb].base_kb;
		if (limit > cand) {
			u32 gap = limit - cand;
			*total += gap;
			if (gap > *largest) {
				*largest = gap;
			}
		}
		if (nb < 0) {
			return;
		}
		cand = xh[nb].base_kb + xh[nb].size_kb;
	}
}

/* --- function 0Bh: move memory block --- */

#pragma pack(push, 1)
typedef struct emm_move {
	u32 length;
	u16 src_handle;
	u32 src_off;
	u16 dst_handle;
	u32 dst_off;
} emm_move;
#pragma pack(pop)

/* GDT for INT 15h AH=87h: null, GDT alias, source, dest,
 * BIOS CS, BIOS SS (the BIOS fills the last two). */
static u8 move_gdt[48];

static void put_desc(u8 *d, u32 base) {
	d[0] = 0xFF;
	d[1] = 0xFF; /* limit 64 KB */
	d[2] = (u8)base;
	d[3] = (u8)(base >> 8);
	d[4] = (u8)(base >> 16);
	d[5] = 0x93; /* present, data, writable */
	d[6] = 0;
	d[7] = (u8)(base >> 24);
}

/* Resolve one side of the EMM struct to a linear address.
 * Returns 0 and sets *err on a bad handle/offset. */
static int emm_linear(u16 handle, u32 off, u32 len, u32 *lin, u8 err_h,
                      u8 err_o, u8 *err) {
	if (handle == 0) {
		u16 seg = (u16)(off >> 16);
		u16 o = (u16)off;
		*lin = ((u32)seg << 4) + o;
		return 1;
	}
	if (handle > XMS_NHANDLES || !xh[handle - 1].used) {
		*err = err_h;
		return 0;
	}
	if (off + len > xh[handle - 1].size_kb * 1024UL) {
		*err = err_o;
		return 0;
	}
	*lin = xh[handle - 1].base_kb * 1024UL + off;
	return 1;
}

static void do_move(xframe __far *r) {
	const emm_move __far *m = (const emm_move __far *)MK_FP(r->ds, r->si);
	u32                   len = m->length;
	u32                   src, dst;
	u8                    err = 0;

	if (len & 1) {
		r->ax = 0;
		r->bx = (r->bx & 0xFF00) | XE_BAD_LEN;
		return;
	}
	if (!emm_linear(m->src_handle, m->src_off, len, &src, XE_BAD_SRC_H,
	                XE_BAD_SRC_O, &err) ||
	    !emm_linear(m->dst_handle, m->dst_off, len, &dst, XE_BAD_DST_H,
	                XE_BAD_DST_O, &err)) {
		r->ax = 0;
		r->bx = (u16)((r->bx & 0xFF00) | err);
		return;
	}
	while (len != 0) {
		u32 chunk = len > 0x10000UL ? 0x10000UL : len;
		put_desc(move_gdt + 0x10, src);
		put_desc(move_gdt + 0x18, dst);
		if (bios_move(move_gdt, (u16)(chunk >> 1)) != 0) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | 0x82; /* general A20/move error */
			return;
		}
		src += chunk;
		dst += chunk;
		len -= chunk;
	}
	r->ax = 1;
}

/* --- the dispatcher, far-called via the stub in startup.asm --- */

void __cdecl xms_dispatch(xframe __far *r) {
	u8  fn = (u8)(r->ax >> 8);
	u16 i;

	switch (fn) {
	case 0x00: /* get version */
		r->ax = 0x0200;
		r->bx = 0x0001;
		r->dx = 1; /* HMA exists */
		return;
	case 0x01: /* request HMA */
		if (hma_used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_HMA_INUSE;
		} else {
			hma_used = 1;
			r->ax = 1;
		}
		return;
	case 0x02: /* release HMA */
		if (!hma_used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_HMA_FREE;
		} else {
			hma_used = 0;
			r->ax = 1;
		}
		return;
	case 0x03: /* global enable A20 */
	case 0x05: /* local enable A20 */
		a20_set(1);
		if (a20_count < 0xFF) {
			a20_count++;
		}
		r->ax = 1;
		return;
	case 0x04: /* global disable A20 */
	case 0x06: /* local disable A20 */
		if (a20_count != 0) {
			a20_count--;
		}
		if (a20_count == 0) {
			a20_set(0);
		}
		r->ax = 1;
		return;
	case 0x07: /* query A20 */
		r->ax = a20_get();
		r->bx &= 0xFF00; /* BL = 0 */
		return;
	case 0x08: { /* query free extended */
		u32 largest, total;
		query_free(&largest, &total);
		r->ax = (u16)largest;
		r->dx = (u16)total;
		r->bx = (u16)((r->bx & 0xFF00) | (largest == 0 ? XE_OUT_OF_MEM : 0));
		return;
	}
	case 0x09: { /* allocate block, DX = KB */
		u32 base;
		int h = -1;
		for (i = 0; i < XMS_NHANDLES; i++) {
			if (!xh[i].used) {
				h = (int)i;
				break;
			}
		}
		if (h < 0) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_NO_HANDLES;
			return;
		}
		base = find_gap(r->dx);
		if (base == 0 && r->dx != 0) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_OUT_OF_MEM;
			return;
		}
		if (base == 0) {
			base = POOL_START_KB; /* zero-length block */
		}
		xh[h].used = 1;
		xh[h].locks = 0;
		xh[h].base_kb = base;
		xh[h].size_kb = r->dx;
		r->ax = 1;
		r->dx = (u16)(h + 1); /* handles are index+1 */
		return;
	}
	case 0x0A: /* free block, DX = handle */
		if (r->dx == 0 || r->dx > XMS_NHANDLES || !xh[r->dx - 1].used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_BAD_HANDLE;
			return;
		}
		if (xh[r->dx - 1].locks != 0) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_LOCKED;
			return;
		}
		xh[r->dx - 1].used = 0;
		r->ax = 1;
		return;
	case 0x0B: /* move block */
		do_move(r);
		return;
	case 0x0C: { /* lock block, DX = handle */
		u32 lin;
		if (r->dx == 0 || r->dx > XMS_NHANDLES || !xh[r->dx - 1].used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_BAD_HANDLE;
			return;
		}
		if (xh[r->dx - 1].locks < 0xFF) {
			xh[r->dx - 1].locks++;
		}
		lin = xh[r->dx - 1].base_kb * 1024UL;
		r->dx = (u16)(lin >> 16);
		r->bx = (u16)lin;
		r->ax = 1;
		return;
	}
	case 0x0D: /* unlock block */
		if (r->dx == 0 || r->dx > XMS_NHANDLES || !xh[r->dx - 1].used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_BAD_HANDLE;
			return;
		}
		if (xh[r->dx - 1].locks == 0) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_NOT_LOCKED;
			return;
		}
		xh[r->dx - 1].locks--;
		r->ax = 1;
		return;
	case 0x0E: { /* get handle info */
		u16 freeh = 0;
		if (r->dx == 0 || r->dx > XMS_NHANDLES || !xh[r->dx - 1].used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_BAD_HANDLE;
			return;
		}
		for (i = 0; i < XMS_NHANDLES; i++) {
			if (!xh[i].used) {
				freeh++;
			}
		}
		r->bx = (u16)(((u16)xh[r->dx - 1].locks << 8) | (u8)freeh);
		r->dx = (u16)xh[r->dx - 1].size_kb;
		r->ax = 1;
		return;
	}
	case 0x0F: { /* reallocate, BX = new KB */
		u16      newkb = r->bx;
		xhandle *x;
		if (r->dx == 0 || r->dx > XMS_NHANDLES || !xh[r->dx - 1].used) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_BAD_HANDLE;
			return;
		}
		x = &xh[r->dx - 1];
		if (x->locks != 0) {
			r->ax = 0;
			r->bx = (r->bx & 0xFF00) | XE_LOCKED;
			return;
		}
		if ((u32)newkb <= x->size_kb) {
			x->size_kb = newkb; /* shrink in place */
			r->ax = 1;
			return;
		}
		/* grow: only when the space right behind the block is free */
		{
			u32 need_end = x->base_kb + newkb;
			u32 old_size = x->size_kb;
			int nb;
			x->size_kb = 0; /* exclude self from the scan */
			nb = next_block_above(x->base_kb + 1);
			x->size_kb = old_size;
			if (need_end <= ((nb < 0) ? pool_end_kb : xh[nb].base_kb)) {
				x->size_kb = newkb;
				r->ax = 1;
			} else {
				r->ax = 0;
				r->bx = (r->bx & 0xFF00) | XE_OUT_OF_MEM;
			}
		}
		return;
	}
	case 0x10: /* request UMB: none */
		r->ax = 0;
		r->bx = (r->bx & 0xFF00) | 0xB1;
		r->dx = 0;
		return;
	case 0x11: /* release UMB */
		r->ax = 0;
		r->bx = (r->bx & 0xFF00) | 0xB2;
		return;
	default:
		r->ax = 0;
		r->bx = (r->bx & 0xFF00) | XE_NOT_IMPL;
		return;
	}
}

/* Detect extended memory; returns the pool size in KB (for the
 * boot banner). Call before installing INT 2Fh. */
u16 xms_init(void) {
	u16 kb = bios_e801_kb();
	u16 i;
	if (kb == 0) {
		kb = bios_88_kb();
	}
	for (i = 0; i < XMS_NHANDLES; i++) {
		xh[i].used = 0;
	}
	/* memory above 1 MB; the first 64 KB belong to the HMA */
	if (kb > 64) {
		pool_end_kb = 1024UL + kb;
	} else {
		pool_end_kb = POOL_START_KB;
	}
	return (u16)(pool_end_kb - POOL_START_KB);
}

/* Reserve kb of extended memory for another kernel subsystem
 * (the EMS backing store). Grabs a handle and locks it so XMS
 * clients can never free or move the block. Returns the linear
 * base address, 0 when the pool is too small. */
u32 xms_reserve(u16 kb) {
	u32 base;
	u16 i;
	for (i = 0; i < XMS_NHANDLES; i++) {
		if (!xh[i].used) {
			break;
		}
	}
	if (i == XMS_NHANDLES) {
		return 0;
	}
	base = find_gap(kb);
	if (base == 0) {
		return 0;
	}
	xh[i].used = 1;
	xh[i].locks = 1; /* permanently locked */
	xh[i].base_kb = base;
	xh[i].size_kb = kb;
	return base * 1024UL;
}

/* Copy bytes between linear addresses through INT 15h AH=87h
 * (the same path as XMS Move). Length must be even. Returns 0
 * on success. */
int xms_copy(u32 dst, u32 src, u32 bytes) {
	while (bytes != 0) {
		u32 chunk = bytes > 0x10000UL ? 0x10000UL : bytes;
		put_desc(move_gdt + 0x10, src);
		put_desc(move_gdt + 0x18, dst);
		if (bios_move(move_gdt, (u16)(chunk >> 1)) != 0) {
			return -1;
		}
		src += chunk;
		dst += chunk;
		bytes -= chunk;
	}
	return 0;
}
