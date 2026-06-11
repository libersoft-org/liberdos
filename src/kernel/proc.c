/* ============================================================
 * proc.c - memory manager and processes
 *
 * MCB chain: DOS-compatible Memory Control Blocks. Each block
 * is preceded by a one-paragraph MCB: 'M' (more follow) or 'Z'
 * (last), owner PSP segment (0 = free), size in paragraphs,
 * owner name at offset 8 (DOS 4+).
 *
 * EXEC (4Bh AL=00h) loads .COM and MZ .EXE programs, builds a
 * PSP and jumps to the child. The parent's SS:SP (saved by the
 * INT 21h stub) is stored in the child PSP at offset 2Eh;
 * terminate (4Ch/00h/INT 20h) switches back to that stack and
 * unwinds through the parent's own INT 21h frame, so the
 * parent resumes right after its EXEC call. This nests, since
 * every child PSP carries its own parent's stack.
 *
 * INT 22h/23h/24h vectors are saved into the PSP at EXEC and
 * restored at terminate (DOS semantics); the terminate return
 * itself bypasses INT 22h and uses the saved stack directly.
 * ============================================================ */
#include "kernel.h"

#define OWNER_FREE   0
#define OWNER_SYSTEM 8

static u16 mcb_first;
static u16 current_psp = 0; /* 0 = kernel itself (no PSP) */
static u16 last_exit = 0;

u16 proc_get_psp(void) {
	return current_psp;
}

void proc_set_psp(u16 psp) {
	current_psp = psp;
}

/* Two env vars, each ASCIIZ; the list is closed by an extra
 * NUL written at copy time, then word 1 + program path follow.
 * The drive letters are patched to the boot drive at startup. */
static char master_env[] = "PATH=A:\\\0COMSPEC=A:\\COMMAND.COM";

/* Rewrite the master environment for a non-A: boot volume. */
void proc_set_boot_drive(char letter) {
	master_env[5] = letter;  /* PATH=X:\ */
	master_env[17] = letter; /* COMSPEC=X:\COMMAND.COM */
}

/* --- MCB primitives --- */

static u8 mtype(u16 m) {
	return peekb(m, 0);
}
static u16 mowner(u16 m) {
	return peekw(m, 1);
}
static u16 msize(u16 m) {
	return peekw(m, 3);
}
static u16 mnext(u16 m) {
	return (u16)(m + 1 + msize(m));
}

static void mcb_coalesce(void) {
	u16 m = mcb_first;
	for (;;) {
		u16 n;
		if (mtype(m) == 'Z') {
			break;
		}
		n = mnext(m);
		if (mowner(m) == OWNER_FREE && mowner(n) == OWNER_FREE) {
			pokew(m, 3, (u16)(msize(m) + 1 + msize(n)));
			pokeb(m, 0, mtype(n));
			continue; /* re-check the merged block */
		}
		m = n;
	}
}

/* First-fit allocation. Returns the block segment (MCB+1) or 0. */
static u16 mcb_alloc(u16 paras, u16 owner) {
	u16 m = mcb_first;
	for (;;) {
		if (mowner(m) == OWNER_FREE && msize(m) >= paras) {
			u16 sz = msize(m);
			if (sz > paras) { /* split off the remainder */
				u16 nm = (u16)(m + 1 + paras);
				pokeb(nm, 0, mtype(m));
				pokew(nm, 1, OWNER_FREE);
				pokew(nm, 3, (u16)(sz - paras - 1));
				pokeb(m, 0, 'M');
				pokew(m, 3, paras);
			}
			pokew(m, 1, owner);
			return (u16)(m + 1);
		}
		if (mtype(m) == 'Z') {
			return 0;
		}
		m = mnext(m);
	}
}

/* Find the MCB belonging to a block segment; 0 if invalid. */
static u16 mcb_find(u16 block) {
	u16 m = mcb_first;
	for (;;) {
		if ((u16)(m + 1) == block) {
			return m;
		}
		if (mtype(m) == 'Z') {
			return 0;
		}
		m = mnext(m);
	}
}

static u16 mcb_free(u16 block) {
	u16 m = mcb_find(block);
	if (m == 0) {
		return 0x09; /* invalid memory block */
	}
	pokew(m, 1, OWNER_FREE);
	mcb_coalesce();
	return 0;
}

static u16 mcb_resize(u16 block, u16 newp, u16 *maxout) {
	u16 m = mcb_find(block);
	u16 cur, avail, n;
	if (m == 0) {
		return 0x09;
	}
	mcb_coalesce();
	cur = msize(m);
	avail = cur;
	if (mtype(m) != 'Z') {
		n = mnext(m);
		if (mowner(n) == OWNER_FREE) {
			avail = (u16)(cur + 1 + msize(n));
		}
	}
	if (newp > avail) {
		*maxout = avail;
		return 0x08; /* insufficient memory */
	}
	if (avail != cur) { /* absorb the free neighbour */
		n = mnext(m);
		pokeb(m, 0, mtype(n));
		pokew(m, 3, avail);
	}
	if (msize(m) > newp) { /* give back the excess */
		u16 nm = (u16)(m + 1 + newp);
		pokeb(nm, 0, mtype(m));
		pokew(nm, 1, OWNER_FREE);
		pokew(nm, 3, (u16)(msize(m) - newp - 1));
		pokeb(m, 0, 'M');
		pokew(m, 3, newp);
		mcb_coalesce();
	}
	return 0;
}

u16 mcb_largest(void) {
	u16 best = 0;
	u16 m = mcb_first;
	mcb_coalesce();
	for (;;) {
		if (mowner(m) == OWNER_FREE && msize(m) > best) {
			best = msize(m);
		}
		if (mtype(m) == 'Z') {
			return best;
		}
		m = mnext(m);
	}
}

static void mcb_free_owner(u16 psp) {
	u16 m = mcb_first;
	for (;;) {
		if (mowner(m) == psp) {
			pokew(m, 1, OWNER_FREE);
		}
		if (mtype(m) == 'Z') {
			break;
		}
		m = mnext(m);
	}
	mcb_coalesce();
}

static void mcb_set_owner(u16 block, u16 owner) {
	pokew((u16)(block - 1), 1, owner);
}

/* Store the program base name (up to 8 chars) in the MCB. */
static void mcb_set_name(u16 block, const char __far *path) {
	const char __far *p = path;
	const char __far *base = path;
	u16               i;
	u16               m = (u16)(block - 1);
	while (*p != '\0') {
		if (*p == '\\' || *p == '/' || *p == ':') {
			base = p + 1;
		}
		p++;
	}
	for (i = 0; i < 8; i++) {
		char c = base[i];
		if (c == '\0' || c == '.') {
			break;
		}
		if (c >= 'a' && c <= 'z') {
			c -= 32;
		}
		pokeb(m, (u16)(8 + i), (u8)c);
	}
	for (; i < 8; i++) {
		pokeb(m, (u16)(8 + i), 0);
	}
}

void proc_init(void) {
	u16 first = (u16)(KERNEL_SEG + ((kernel_end_off + 15) >> 4));
	u16 top = (u16)(peekw(0x40, 0x13) << 6); /* BDA KB count -> paras */
	mcb_first = first;
	pokeb(first, 0, 'Z');
	pokew(first, 1, OWNER_FREE);
	pokew(first, 3, (u16)(top - first - 1));
}

/* --- PSP construction --- */

static void psp_build(u16 psp, u16 memtop, u16 env, u16 parent) {
	u16 i;
	fmemset(MK_FP(psp, 0), 0, 256);
	pokeb(psp, 0x00, 0xCD); /* INT 20h */
	pokeb(psp, 0x01, 0x20);
	pokew(psp, 0x02, memtop); /* first segment beyond block */
	pokeb(psp, 0x05, 0x9A);   /* CP/M far-call stub (decorative) */
	pokew(psp, 0x06, 0xFEF0);
	pokew(psp, 0x08, 0xF000);
	pokew(psp, 0x0A, peekw(0, 0x88)); /* saved INT 22h */
	pokew(psp, 0x0C, peekw(0, 0x8A));
	pokew(psp, 0x0E, peekw(0, 0x8C)); /* saved INT 23h */
	pokew(psp, 0x10, peekw(0, 0x8E));
	pokew(psp, 0x12, peekw(0, 0x90)); /* saved INT 24h */
	pokew(psp, 0x14, peekw(0, 0x92));
	pokew(psp, 0x16, parent);
	for (i = 0; i < 20; i++) { /* JFT: filled by file_jft_inherit */
		pokeb(psp, (u16)(0x18 + i), 0xFF);
	}
	pokew(psp, 0x2C, env);
	pokew(psp, 0x32, 20);     /* JFT size */
	pokew(psp, 0x34, 0x0018); /* JFT pointer -> PSP:18h */
	pokew(psp, 0x36, psp);
	pokew(psp, 0x38, 0xFFFF); /* previous PSP: none */
	pokew(psp, 0x3A, 0xFFFF);
	pokew(psp, 0x40, 0x0005); /* DOS version to report */
	pokeb(psp, 0x50, 0xCD);   /* INT 21h; RETF */
	pokeb(psp, 0x51, 0x21);
	pokeb(psp, 0x52, 0xCB);
	pokeb(psp, 0x5C, 0); /* blank FCB1/FCB2 */
	fmemset(MK_FP(psp, 0x5D), ' ', 11);
	pokeb(psp, 0x6C, 0);
	fmemset(MK_FP(psp, 0x6D), ' ', 11);
	pokeb(psp, 0x80, 0); /* empty command tail */
	pokeb(psp, 0x81, 0x0D);
}

/* Size of the variable area incl. the final list terminator. */
static u16 env_vars_size(u16 seg) {
	u16 o = 0;
	while (peekb(seg, o) != 0) {
		while (peekb(seg, o) != 0) {
			o++;
		}
		o++;
	}
	return (u16)(o + 1);
}

/* Child environment block: a copy of the parent's variables
 * (or the master set when src == 0), then word 1 + program
 * path - each child owns its env copy, DOS-style. */
static u16 env_build(u16 src, const char __far *path) {
	u16 plen = fstrlen(path);
	u16 vsz = src != 0 ? env_vars_size(src) : (u16)(sizeof(master_env) + 1);
	u16 total = (u16)(vsz + 2 + plen + 1);
	u16 paras = (u16)((total + 15) >> 4);
	u16 seg = mcb_alloc(paras, OWNER_SYSTEM);
	u16 o;
	if (seg == 0) {
		return 0;
	}
	if (src != 0) {
		fmemcpy(MK_FP(seg, 0), MK_FP(src, 0), vsz);
	} else {
		fmemcpy(MK_FP(seg, 0), master_env, sizeof(master_env));
		pokeb(seg, sizeof(master_env), 0); /* end of variable list */
	}
	o = vsz;
	pokew(seg, o, 1); /* one extra string follows */
	o += 2;
	fmemcpy(MK_FP(seg, o), path, (u16)(plen + 1));
	return seg;
}

/* --- INT 21h handlers --- */

/* 48h: allocate BX paragraphs -> AX = block segment */
void f48_alloc(iregs __far *r) {
	u16 owner = current_psp != 0 ? current_psp : OWNER_SYSTEM;
	u16 seg = mcb_alloc(r->bx, owner);
	if (seg == 0) {
		r->bx = mcb_largest();
		int21_error(r, 0x08);
		return;
	}
	r->ax = seg;
}

/* 49h: free block at ES */
void f49_free(iregs __far *r) {
	u16 err = mcb_free(r->es);
	if (err != 0) {
		int21_error(r, err);
	}
}

/* 4Ah: resize block at ES to BX paragraphs */
void f4a_resize(iregs __far *r) {
	u16 maxp = 0;
	u16 err = mcb_resize(r->es, r->bx, &maxp);
	if (err != 0) {
		r->bx = maxp;
		int21_error(r, err);
	}
}

/* 4Dh: get exit code of last terminated child */
void f4d_exitcode(iregs __far *r) {
	r->ax = last_exit; /* AH = 0: normal termination */
}

#pragma pack(push, 1)
typedef struct execblk {
	u16 env_seg;
	u16 tail_off, tail_seg;
	u16 fcb1_off, fcb1_seg;
	u16 fcb2_off, fcb2_seg;
} execblk;

typedef struct mzhdr {
	u16 sig, lastp, pages, nreloc, hparas, minp, maxp;
	u16 ss, sp, csum, ip, cs, reloff, ovl;
} mzhdr;
#pragma pack(pop)

/* Read "count" file bytes to a far destination in 512-byte
 * chunks, advancing the segment to dodge 64 KB offset wrap. */
static int load_image(int h, u16 base_seg, u32 count) {
	u32 done = 0;
	while (done < count) {
		u32         left = count - done;
		u16         chunk = left > 512 ? 512 : (u16)left;
		void __far *dst =
		    MK_FP((u16)(base_seg + (u16)(done >> 4)), (u16)(done & 15));
		if (kfile_read(h, dst, chunk) != chunk) {
			return -1;
		}
		done += chunk;
	}
	return 0;
}

/* 4Bh AL=00h: load and execute program */
void f4b_exec(iregs __far *r) {
	const char __far    *path = (const char __far *)MK_FP(r->ds, r->dx);
	const execblk __far *pb = (r->es == 0 && r->bx == 0)
	                              ? 0
	                              : (const execblk __far *)MK_FP(r->es, r->bx);
	int                  h;
	u16                  err = 0;
	u16                  env_seg = 0;
	u16                  psp = 0;
	u16                  paras = 0;
	u16                  cs = 0, ip = 0, ss = 0, sp = 0;
	mzhdr                hdr;
	u16                  hn;

	if ((r->ax & 0xFF) != 0x00) {
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
	h = kfile_open(path);
	if (h < 0) {
		int21_error(r, (u16)(-h));
		return;
	}
	hn = kfile_read(h, &hdr, sizeof(hdr));

	env_seg = env_build(pb != 0 ? pb->env_seg : 0, path);
	if (env_seg == 0) {
		err = 0x08;
		goto fail;
	}

	if (hn >= 2 && hdr.sig == 0x5A4D) {
		/* --- MZ .EXE --- */
		u32 image =
		    (u32)(hdr.pages - 1) * 512 + (hdr.lastp != 0 ? hdr.lastp : 512);
		u32 loadb = image - (u32)hdr.hparas * 16;
		u16 loadp = (u16)((loadb + 15) >> 4);
		u16 need = (u16)(16 + loadp + hdr.minp);
		u32 want = (u32)16 + loadp + hdr.maxp;
		u16 largest = mcb_largest();
		u16 take;
		u16 i;
		if (hdr.maxp == 0) {
			want = need; /* load-high request: give minimum */
		}
		if (largest < need) {
			err = 0x08;
			goto fail;
		}
		take = want > largest ? largest : (u16)want;
		psp = mcb_alloc(take, 1);
		if (psp == 0) {
			err = 0x08;
			goto fail;
		}
		paras = take;
		kfile_seek_set(h, (u32)hdr.hparas * 16);
		if (load_image(h, (u16)(psp + 16), loadb) != 0) {
			err = 0x0B; /* invalid format */
			goto fail;
		}
		kfile_seek_set(h, hdr.reloff);
		for (i = 0; i < hdr.nreloc; i++) {
			u16        re[2];
			u16 __far *tp;
			if (kfile_read(h, re, 4) != 4) {
				err = 0x0B;
				goto fail;
			}
			tp = (u16 __far *)MK_FP((u16)(psp + 16 + re[1]), re[0]);
			*tp += (u16)(psp + 16);
		}
		cs = (u16)(psp + 16 + hdr.cs);
		ip = hdr.ip;
		ss = (u16)(psp + 16 + hdr.ss);
		sp = hdr.sp;
	} else {
		/* --- .COM --- */
		u32 size = kfile_size(h);
		u16 largest = mcb_largest();
		u32 avail;
		if (largest < 0x11) { /* PSP + at least one para */
			err = 0x08;
			goto fail;
		}
		psp = mcb_alloc(largest, 1);
		if (psp == 0) {
			err = 0x08;
			goto fail;
		}
		paras = largest;
		avail = (u32)paras * 16 - 0x100 - 2;
		if (size > avail) {
			err = 0x08;
			goto fail;
		}
		kfile_seek_set(h, 0);
		if (load_image(h, (u16)(psp + 16), size) != 0) {
			err = 0x0B;
			goto fail;
		}
		cs = psp;
		ip = 0x100;
		ss = psp;
		sp = paras >= 0x1000 ? 0xFFFE : (u16)(paras * 16 - 2);
		pokew(psp, sp, 0); /* RET from COM -> PSP:0 -> INT 20h */
	}
	kfile_close(h);

	mcb_set_owner(psp, psp);
	mcb_set_owner(env_seg, psp);
	mcb_set_name(psp, path);
	psp_build(psp, (u16)(psp + paras), env_seg, current_psp);
	setver_apply(psp, path); /* per-program version override */
	file_jft_inherit(psp, current_psp);

	if (pb != 0) {
		if (pb->tail_seg != 0 || pb->tail_off != 0) {
			const u8 __far *t =
			    (const u8 __far *)MK_FP(pb->tail_seg, pb->tail_off);
			u8 len = t[0];
			if (len > 126) {
				len = 126;
			}
			fmemcpy(MK_FP(psp, 0x80), t, (u16)(len + 1));
			pokeb(psp, (u16)(0x81 + len), 0x0D);
		}
		if (pb->fcb1_seg != 0 || pb->fcb1_off != 0) {
			fmemcpy(MK_FP(psp, 0x5C), MK_FP(pb->fcb1_seg, pb->fcb1_off), 16);
		}
		if (pb->fcb2_seg != 0 || pb->fcb2_off != 0) {
			fmemcpy(MK_FP(psp, 0x6C), MK_FP(pb->fcb2_seg, pb->fcb2_off), 16);
		}
	}

	/* parent return context -> child PSP (offset 2Eh) */
	pokew(psp, 0x2E, int21_user_sp);
	pokew(psp, 0x30, int21_user_ss);

	current_psp = psp;
	exec_cs = cs;
	exec_ip = ip;
	exec_ss = ss;
	exec_sp = sp;
	exec_psp = psp;
	indos_flag = 0; /* leaving DOS via launch */
	exec_launch();  /* never returns */

fail:
	kfile_close(h);
	if (psp != 0) {
		mcb_free(psp);
	}
	if (env_seg != 0) {
		mcb_free(env_seg);
	}
	int21_error(r, err);
}

/* Restore INT 22h/23h/24h from the PSP and switch back to the
 * parent's saved stack. Shared by terminate and TSR. */
static void proc_unwind(u16 psp) {
	pokew(0, 0x88, peekw(psp, 0x0A));
	pokew(0, 0x8A, peekw(psp, 0x0C));
	pokew(0, 0x8C, peekw(psp, 0x0E));
	pokew(0, 0x8E, peekw(psp, 0x10));
	pokew(0, 0x90, peekw(psp, 0x12));
	pokew(0, 0x92, peekw(psp, 0x14));
	exec_sp = peekw(psp, 0x2E); /* parent stack from EXEC time */
	exec_ss = peekw(psp, 0x30);
	current_psp = peekw(psp, 0x16);
	indos_flag = 0; /* leaving DOS via term_return */
	term_return();  /* never returns */
}

/* Terminate the current process (4Ch, 00h, INT 20h). */
void proc_terminate(iregs __far *r, u8 code) {
	u16 psp = current_psp;
	(void)r;
	last_exit = code; /* AH = 0: normal termination */
	if (break_abort) {
		break_abort = 0;
		last_exit = 0x0100; /* AH = 1: Ctrl-C termination */
	}
	if (psp == 0) {
		con_puts("\r\n[kernel] root process exited, system halted.\r\n");
		for (;;) {
			hlt();
		}
	}
	file_jft_close_all(psp);
	share_psp_closed(psp); /* drop region locks it still owns */
	mcb_free_owner(psp);
	proc_unwind(psp);
}

/* 31h: terminate and stay resident. DX = paragraphs to keep
 * (PSP included, min 6). Memory is not freed and handles stay
 * open; the environment block stays owned by the TSR too. */
void f31_tsr(iregs __far *r) {
	u16 psp = current_psp;
	u16 keep = r->dx < 6 ? 6 : r->dx;
	u16 maxp;
	if (psp == 0) {
		proc_terminate(r, (u8)(r->ax & 0xFF));
		return;
	}
	mcb_resize(psp, keep, &maxp);
	last_exit = (u16)(0x0300 | (r->ax & 0xFF)); /* AH = 3: TSR */
	proc_unwind(psp);
}
