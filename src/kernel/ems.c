/* ============================================================
 * ems.c - EMS 4.0 driver (LIM expanded memory, INT 67h)
 *
 * Copy-based EMM: the real EMM386 remaps pages with the MMU
 * from V86 mode; this kernel stays in real mode, so "mapping"
 * a 16 KB logical page into the page frame physically copies
 * it between conventional memory and an extended-memory
 * backing store (reserved from the XMS pool, moved with the
 * same INT 15h AH=87h path as XMS Move). Slower than paging
 * but indistinguishable to programs that follow the API.
 *
 * The page frame lives at segment D000h. On QEMU's i440FX
 * that region is RAM hidden behind the PAM registers of the
 * host bridge; ems_init unlocks it via PCI config (the same
 * trick UMBPCI.SYS uses) and verifies it with a write test.
 * No RAM there (option ROM, other chipset) = EMS disabled.
 *
 * Detection works both ways games expect it:
 *  - INT 67h vector segment : 000Ah = "EMMXXXX0" (the device
 *    header in startup.asm; some games check this)
 *  - open "EMMXXXX0" + IOCTL 4400h/4407h (file.c serves the
 *    name as a character device; other games do this)
 * ============================================================ */
#include "kernel.h"

#define EMS_FRAME_SEG  0xD000
#define EMS_PHYS_PAGES 4
#define EMS_NHANDLES   32
#define EMS_BACKING_KB 4096U /* 256 pages of 16 KB */
#define EMS_PAGE_KB    16U
#define EMS_UNMAPPED   0xFFFF

/* EMS status codes (returned in AH). */
#define EE_OK         0x00
#define EE_SW_MALF    0x80
#define EE_BAD_HANDLE 0x83
#define EE_BAD_FUNC   0x84
#define EE_NO_HANDLES 0x85
#define EE_NO_TOTAL   0x87
#define EE_NO_FREE    0x88
#define EE_ZERO_PAGES 0x89
#define EE_BAD_LPAGE  0x8A
#define EE_BAD_PPAGE  0x8B
#define EE_SAVED      0x8D /* save area already holds this handle */
#define EE_NOT_SAVED  0x8E /* restore without a prior save */
#define EE_BAD_SUBFN  0x8F

typedef struct ehandle {
	u8  used;
	u16 npages;
	u16 first; /* first backing page index */
	u8  name[8];
} ehandle;

static ehandle eh[EMS_NHANDLES];
static u32     backing_base;    /* linear base of the XMS reservation */
static u16     total_pages;     /* size of the backing store */
static u16     used_pages;      /* currently allocated */
static u8      ems_present = 0; /* set by ems_init on success */

/* Backing pages are handed out as one contiguous run per handle
 * (first-fit, like the XMS pool). page_owner[] marks each backing
 * page free (0xFF) or owned by a handle index. */
static u8 page_owner[EMS_BACKING_KB / EMS_PAGE_KB];

/* What logical page (handle, page) each physical frame page
 * holds. EMS_UNMAPPED handle = nothing mapped. */
typedef struct emap {
	u16 handle; /* handle index, EMS_UNMAPPED = empty */
	u16 lpage;
} emap;

static emap cur_map[EMS_PHYS_PAGES];

/* Function 47h/48h: one save slot per handle, as the spec says
 * (a second 47h without a 48h is an error). */
static emap saved_map[EMS_NHANDLES][EMS_PHYS_PAGES];
static u8   saved_used[EMS_NHANDLES];

/* --- PAM unlock helper (startup.asm; needs OUT DX,EAX) --- */
extern u16 pci_pam_unlock(void);

/* Try to turn segment D000h into writable RAM. On the i440FX
 * the 64 KB at D0000h are PAM3 (D0000-D7FFF) and PAM4
 * (D8000-DFFFF); each nibble 3 = read+write enable. Verified
 * afterwards with a write test, so a wrong chipset just fails
 * the probe instead of breaking anything. */
static int frame_unlock(void) {
	u16 off;
	/* an option ROM at D000h means the area is taken: leave it */
	if (peekw(EMS_FRAME_SEG, 0) == 0xAA55) {
		return -1;
	}
	if (pci_pam_unlock() != 0x3333) {
		return -1; /* not a PAM-style host bridge */
	}
	/* write test across all four 16 KB pages */
	for (off = 0; off < 4; off++) {
		u16 o = (u16)(off * 0x4000U);
		pokew(EMS_FRAME_SEG, o, 0x55AA);
		pokew(EMS_FRAME_SEG, (u16)(o + 0x3FFE), (u16)(0x1234 + off));
		if (peekw(EMS_FRAME_SEG, o) != 0x55AA ||
		    peekw(EMS_FRAME_SEG, (u16)(o + 0x3FFE)) != (u16)(0x1234 + off)) {
			return -1;
		}
	}
	return 0;
}

/* --- backing store allocation (contiguous runs, first fit) --- */

static int backing_alloc(u16 npages, u16 *first) {
	u16 i, run = 0;
	for (i = 0; i < total_pages; i++) {
		if (page_owner[i] == 0xFF) {
			run++;
			if (run == npages) {
				*first = (u16)(i - npages + 1);
				return 0;
			}
		} else {
			run = 0;
		}
	}
	return -1;
}

static u32 backing_lin(u16 page) {
	return backing_base + (u32)page * (EMS_PAGE_KB * 1024UL);
}

static u32 frame_lin(u16 ppage) {
	return ((u32)EMS_FRAME_SEG << 4) + (u32)ppage * (EMS_PAGE_KB * 1024UL);
}

/* --- mapping (the copy that replaces MMU page flips) --- */

/* Flush a physical page back to its backing page, then mark it
 * empty. No-op when nothing is mapped. */
static int phys_flush(u16 ppage) {
	emap *m = &cur_map[ppage];
	if (m->handle != EMS_UNMAPPED) {
		u16 bp = (u16)(eh[m->handle].first + m->lpage);
		if (xms_copy(backing_lin(bp), frame_lin(ppage), EMS_PAGE_KB * 1024UL) !=
		    0) {
			return -1;
		}
		m->handle = EMS_UNMAPPED;
	}
	return 0;
}

/* Map logical page lpage of handle h into physical page ppage.
 * lpage 0xFFFF unmaps. Returns an EMS status code. */
static u8 map_page(u16 h, u16 lpage, u16 ppage) {
	if (ppage >= EMS_PHYS_PAGES) {
		return EE_BAD_PPAGE;
	}
	if (lpage == EMS_UNMAPPED) {
		return phys_flush(ppage) != 0 ? EE_SW_MALF : EE_OK;
	}
	if (h >= EMS_NHANDLES || !eh[h].used) {
		return EE_BAD_HANDLE;
	}
	if (lpage >= eh[h].npages) {
		return EE_BAD_LPAGE;
	}
	if (cur_map[ppage].handle == h && cur_map[ppage].lpage == lpage) {
		return EE_OK; /* already there: skip the copy roundtrip */
	}
	if (phys_flush(ppage) != 0) {
		return EE_SW_MALF;
	}
	if (xms_copy(frame_lin(ppage), backing_lin((u16)(eh[h].first + lpage)),
	             EMS_PAGE_KB * 1024UL) != 0) {
		return EE_SW_MALF;
	}
	cur_map[ppage].handle = h;
	cur_map[ppage].lpage = lpage;
	return EE_OK;
}

/* Validate the handle in DX. Returns the handle index or -1
 * (with AH already set in the frame). */
static int handle_arg(iregs __far *r) {
	u16 h = r->dx;
	if (h == 0 || h > EMS_NHANDLES || !eh[h - 1].used) {
		r->ax = (u16)(EE_BAD_HANDLE << 8) | (r->ax & 0xFF);
		return -1;
	}
	return (int)(h - 1);
}

static void set_ah(iregs __far *r, u8 status) {
	r->ax = (u16)((u16)status << 8) | (r->ax & 0xFF);
}

/* --- function 44h core, shared with 50h (map multiple) --- */

static void fn_map(iregs __far *r, u16 lpage, u16 ppage) {
	int h = handle_arg(r);
	if (h < 0) {
		return;
	}
	set_ah(r, map_page((u16)h, lpage, ppage));
}

/* --- the dispatcher, called from the INT 67h stub --- */

void __cdecl ems_dispatch(iregs __far *r) {
	u8  fn = (u8)(r->ax >> 8);
	u16 i;

	if (!ems_present) {
		set_ah(r, 0x84); /* no EMM: nothing is implemented */
		return;
	}
	switch (fn) {
	case 0x40: /* get manager status */
		set_ah(r, EE_OK);
		return;
	case 0x41: /* get page frame segment */
		r->bx = EMS_FRAME_SEG;
		set_ah(r, EE_OK);
		return;
	case 0x42: /* get unallocated page count */
		r->bx = (u16)(total_pages - used_pages);
		r->dx = total_pages;
		set_ah(r, EE_OK);
		return;
	case 0x43: { /* allocate BX pages -> handle in DX */
		u16 first;
		int h = -1;
		if (r->bx == 0) {
			set_ah(r, EE_ZERO_PAGES);
			return;
		}
		if (r->bx > total_pages) {
			set_ah(r, EE_NO_TOTAL);
			return;
		}
		for (i = 0; i < EMS_NHANDLES; i++) {
			if (!eh[i].used) {
				h = (int)i;
				break;
			}
		}
		if (h < 0) {
			set_ah(r, EE_NO_HANDLES);
			return;
		}
		if (backing_alloc(r->bx, &first) != 0) {
			set_ah(r, EE_NO_FREE);
			return;
		}
		eh[h].used = 1;
		eh[h].npages = r->bx;
		eh[h].first = first;
		for (i = 0; i < r->bx; i++) {
			page_owner[first + i] = (u8)h;
		}
		used_pages += r->bx;
		saved_used[h] = 0;
		fmemset(eh[h].name, 0, 8);
		r->dx = (u16)(h + 1);
		set_ah(r, EE_OK);
		return;
	}
	case 0x44: /* map logical BX into physical AL of handle DX */
		fn_map(r, r->bx, (u16)(r->ax & 0xFF));
		return;
	case 0x45: { /* release handle DX */
		int h = handle_arg(r);
		if (h < 0) {
			return;
		}
		/* drop any current mappings of this handle (no flush) */
		for (i = 0; i < EMS_PHYS_PAGES; i++) {
			if (cur_map[i].handle == (u16)h) {
				cur_map[i].handle = EMS_UNMAPPED;
			}
		}
		for (i = 0; i < eh[h].npages; i++) {
			page_owner[eh[h].first + i] = 0xFF;
		}
		used_pages -= eh[h].npages;
		eh[h].used = 0;
		saved_used[h] = 0;
		set_ah(r, EE_OK);
		return;
	}
	case 0x46: /* get version: 4.0 in BCD */
		r->ax = (u16)(EE_OK << 8) | 0x40;
		return;
	case 0x47: { /* save page map for handle DX */
		int h = handle_arg(r);
		if (h < 0) {
			return;
		}
		if (saved_used[h]) {
			set_ah(r, EE_SAVED);
			return;
		}
		for (i = 0; i < EMS_PHYS_PAGES; i++) {
			saved_map[h][i] = cur_map[i];
		}
		saved_used[h] = 1;
		set_ah(r, EE_OK);
		return;
	}
	case 0x48: { /* restore page map for handle DX */
		int h = handle_arg(r);
		u8  st;
		if (h < 0) {
			return;
		}
		if (!saved_used[h]) {
			set_ah(r, EE_NOT_SAVED);
			return;
		}
		for (i = 0; i < EMS_PHYS_PAGES; i++) {
			emap *s = &saved_map[h][i];
			st = map_page(s->handle,
			              s->handle == EMS_UNMAPPED ? EMS_UNMAPPED : s->lpage,
			              i);
			if (st != EE_OK) {
				set_ah(r, st);
				return;
			}
		}
		saved_used[h] = 0;
		set_ah(r, EE_OK);
		return;
	}
	case 0x4B: { /* get open handle count */
		u16 n = 0;
		for (i = 0; i < EMS_NHANDLES; i++) {
			if (eh[i].used) {
				n++;
			}
		}
		r->bx = n;
		set_ah(r, EE_OK);
		return;
	}
	case 0x4C: { /* get pages of handle DX */
		int h = handle_arg(r);
		if (h < 0) {
			return;
		}
		r->bx = eh[h].npages;
		set_ah(r, EE_OK);
		return;
	}
	case 0x4D: { /* get all handles' pages -> table at ES:DI */
		u16 __far *t = (u16 __far *)MK_FP(r->es, r->di);
		u16        n = 0;
		for (i = 0; i < EMS_NHANDLES; i++) {
			if (eh[i].used) {
				t[n * 2] = (u16)(i + 1);
				t[n * 2 + 1] = eh[i].npages;
				n++;
			}
		}
		r->bx = n;
		set_ah(r, EE_OK);
		return;
	}
	case 0x4E: { /* get/set the complete page map */
		u8 sub = (u8)(r->ax & 0xFF);
		if (sub == 0x00 || sub == 0x02) { /* get -> ES:DI */
			emap __far *d = (emap __far *)MK_FP(r->es, r->di);
			for (i = 0; i < EMS_PHYS_PAGES; i++) {
				d[i] = cur_map[i];
			}
		}
		if (sub == 0x01 || sub == 0x02) { /* set <- DS:SI */
			const emap __far *s = (const emap __far *)MK_FP(r->ds, r->si);
			for (i = 0; i < EMS_PHYS_PAGES; i++) {
				u8 st = map_page(
				    s[i].handle,
				    s[i].handle == EMS_UNMAPPED ? EMS_UNMAPPED : s[i].lpage, i);
				if (st != EE_OK) {
					set_ah(r, st);
					return;
				}
			}
		}
		if (sub == 0x03) { /* get save array size -> AL */
			r->ax = (u16)(EE_OK << 8) | (u8)(EMS_PHYS_PAGES * sizeof(emap));
			return;
		}
		if (sub > 0x03) {
			set_ah(r, EE_BAD_SUBFN);
			return;
		}
		set_ah(r, EE_OK);
		return;
	}
	case 0x50: { /* map multiple pages (EMS 4.0) */
		const u16 __far *p = (const u16 __far *)MK_FP(r->ds, r->si);
		int              h = handle_arg(r);
		u8               sub = (u8)(r->ax & 0xFF);
		if (h < 0) {
			return;
		}
		if (sub != 0x00) { /* only "by page number" supported */
			set_ah(r, EE_BAD_SUBFN);
			return;
		}
		for (i = 0; i < r->cx; i++) {
			u8 st = map_page((u16)h, p[i * 2], p[i * 2 + 1]);
			if (st != EE_OK) {
				set_ah(r, st);
				return;
			}
		}
		set_ah(r, EE_OK);
		return;
	}
	case 0x51: { /* reallocate handle DX to BX pages */
		int h = handle_arg(r);
		u16 first;
		if (h < 0) {
			return;
		}
		if (r->bx == eh[h].npages) {
			set_ah(r, EE_OK);
			return;
		}
		if (r->bx < eh[h].npages) { /* shrink: free the tail */
			for (i = r->bx; i < eh[h].npages; i++) {
				page_owner[eh[h].first + i] = 0xFF;
			}
			used_pages -= (u16)(eh[h].npages - r->bx);
			eh[h].npages = r->bx;
			set_ah(r, EE_OK);
			return;
		}
		/* grow: move to a fresh run, copy the old content */
		if (backing_alloc(r->bx, &first) != 0) {
			set_ah(r, EE_NO_FREE);
			return;
		}
		if (xms_copy(backing_lin(first), backing_lin(eh[h].first),
		             (u32)eh[h].npages * EMS_PAGE_KB * 1024UL) != 0) {
			set_ah(r, EE_SW_MALF);
			return;
		}
		for (i = 0; i < eh[h].npages; i++) {
			page_owner[eh[h].first + i] = 0xFF;
		}
		for (i = 0; i < r->bx; i++) {
			page_owner[first + i] = (u8)h;
		}
		used_pages += (u16)(r->bx - eh[h].npages);
		eh[h].first = first;
		eh[h].npages = r->bx;
		set_ah(r, EE_OK);
		return;
	}
	case 0x53: { /* get (AL=0) / set (AL=1) handle name */
		int h = handle_arg(r);
		if (h < 0) {
			return;
		}
		if ((r->ax & 0xFF) == 0x00) {
			fmemcpy(MK_FP(r->es, r->di), eh[h].name, 8);
		} else if ((r->ax & 0xFF) == 0x01) {
			fmemcpy(eh[h].name, MK_FP(r->ds, r->si), 8);
		} else {
			set_ah(r, EE_BAD_SUBFN);
			return;
		}
		set_ah(r, EE_OK);
		return;
	}
	default:
		set_ah(r, EE_BAD_FUNC);
		return;
	}
}

/* For file.c: the "EMMXXXX0" device only exists when EMS is up. */
int ems_avail(void) {
	return ems_present;
}

/* Probe the page frame, reserve the backing store. Returns the
 * EMS size in KB for the boot banner, 0 = not available. Call
 * after xms_init, before installing INT 67h (main.c installs
 * the vector only when this succeeds). */
u16 ems_init(void) {
	u16 i;
	if (frame_unlock() != 0) {
		return 0;
	}
	backing_base = xms_reserve(EMS_BACKING_KB);
	if (backing_base == 0) {
		return 0;
	}
	total_pages = EMS_BACKING_KB / EMS_PAGE_KB;
	used_pages = 0;
	for (i = 0; i < EMS_NHANDLES; i++) {
		eh[i].used = 0;
		saved_used[i] = 0;
	}
	for (i = 0; i < total_pages; i++) {
		page_owner[i] = 0xFF;
	}
	for (i = 0; i < EMS_PHYS_PAGES; i++) {
		cur_map[i].handle = EMS_UNMAPPED;
	}
	ems_present = 1;
	return EMS_BACKING_KB;
}
