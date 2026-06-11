/* ============================================================
 * file.c - SFT, per-process JFT, DTA and directory search
 *
 * The handle layer mirrors DOS: a global System
 * File Table (SFT) holds the open files with reference counts,
 * and each process maps its 20 handles to SFT slots through
 * the Job File Table at PSP:18h. DUP handles and EXEC handle
 * inheritance just bump SFT refcounts. While no process is
 * active (kernel context, PSP = 0) a private kernel JFT is
 * used.
 *
 * INT 21h functions implemented here:
 *   0Dh disk reset             41h delete
 *   0Eh select drive           42h seek
 *   19h get current drive      43h get/set attributes
 *   1Ah set DTA                44h IOCTL
 *   2Fh get DTA                45h dup handle
 *   36h get free space         46h force dup
 *   39h mkdir                  47h get current directory
 *   3Ah rmdir                  4Eh/4Fh FindFirst/FindNext
 *   3Bh chdir                  56h rename/move
 *   3Ch create/truncate        57h get/set file stamp
 *   3Dh open                   5Bh create new
 *   3Eh close                  68h commit
 *   3Fh read   40h write
 *
 * SFT slots 0-2 are the CON / AUX / NUL devices; the default
 * JFT maps handles 0-4 to 0,0,0,1,2 like DOS does.
 * ============================================================ */
#include "kernel.h"

#define NUM_SFT  20
#define JFT_SIZE 20
#define JFT_FREE 0xFF

#define DEV_FILE 0
#define DEV_CON  1
#define DEV_AUX  2
#define DEV_NUL  3 /* swallows writes, EOF on reads (also PRN) */
#define DEV_EMM                                  \
	4 /* "EMMXXXX0": exists only so programs can \
	   * open it to detect EMS; I/O behaves like \
	   * NUL */

typedef struct fhandle {
	u8  open; /* SFT refcount, 0 = slot free */
	u8  dev;
	u8  mode;        /* access bits of open AL: 0=read 1=write 2=rdwr */
	u8  smode;       /* full open AL incl. sharing bits (share.c) */
	u8  dirty;       /* size/cluster changed: flush on close */
	u8  drv;         /* volume the file lives on (0=A 2=C) */
	u16 dir_cluster; /* directory holding our dirent */
	u16 dir_index;   /* dirent index inside it */
	u16 first_cluster;
	u16 cur_cluster;     /* cluster containing cur_cluster_pos */
	u32 cur_cluster_pos; /* file pos of cur_cluster's first byte */
	u32 pos;
	u32 size;
	u16 ftime, fdate; /* stamp written back on flush */
} fhandle;

static fhandle sft[NUM_SFT];
static u8      kjft[JFT_SIZE]; /* JFT for kernel context (no PSP) */

static u16 dta_seg, dta_off;
static u8  default_dta[128];

/* Search state kept in the first 21 (reserved) bytes of the
 * DTA + the documented result record at offset 21. */
#pragma pack(push, 1)
typedef struct findstate {
	u8   pattern[11];
	u8   attr;
	u16  dir_cluster;
	u16  index;
	u8   drv;
	u8   unused[4];
	u8   res_attr; /* offset 21: documented DOS layout */
	u16  res_time;
	u16  res_date;
	u32  res_size;
	char res_name[13];
} findstate;
#pragma pack(pop)

/* CON cooked-mode line buffer for handle reads. */
static u8 line_buf[130];
static u8 line_len = 0;
static u8 line_pos = 0;

/* --- JFT primitives --- */

static u8 jft_get(u16 h) {
	u16 psp = proc_get_psp();
	if (psp != 0) {
		return peekb(psp, (u16)(0x18 + h));
	}
	return kjft[h];
}

static void jft_set(u16 h, u8 v) {
	u16 psp = proc_get_psp();
	if (psp != 0) {
		pokeb(psp, (u16)(0x18 + h), v);
	} else {
		kjft[h] = v;
	}
}

/* Resolve a handle to its SFT entry; 0 when invalid. */
static fhandle *handle_of(u16 h) {
	u8 s;
	if (h >= JFT_SIZE) {
		return 0;
	}
	s = jft_get(h);
	if (s >= NUM_SFT || sft[s].open == 0) {
		return 0;
	}
	return &sft[s];
}

static int find_free_jft(void) {
	u16 h;
	for (h = 0; h < JFT_SIZE; h++) {
		if (jft_get(h) == JFT_FREE) {
			return (int)h;
		}
	}
	return -1;
}

static int find_free_sft(void) {
	u16 s;
	for (s = 0; s < NUM_SFT; s++) {
		if (sft[s].open == 0) {
			return (int)s;
		}
	}
	return -1;
}

void file_init(void) {
	u16 i;
	for (i = 0; i < NUM_SFT; i++) {
		sft[i].open = 0;
	}
	sft[0].open = 1;
	sft[0].dev = DEV_CON;
	sft[1].open = 1;
	sft[1].dev = DEV_AUX;
	sft[2].open = 1;
	sft[2].dev = DEV_NUL;
	kjft[0] = 0;
	kjft[1] = 0;
	kjft[2] = 0; /* stdin/out/err */
	kjft[3] = 1; /* aux */
	kjft[4] = 2; /* prn -> NUL */
	for (i = 5; i < JFT_SIZE; i++) {
		kjft[i] = JFT_FREE;
	}
	dta_seg = get_cs();
	dta_off = (u16)default_dta;
}

/* EXEC inheritance: copy the parent JFT (or the kernel JFT)
 * into the child PSP and add a reference to every open slot. */
void file_jft_inherit(u16 psp, u16 parent) {
	u16 i;
	for (i = 0; i < JFT_SIZE; i++) {
		u8 s = parent != 0 ? peekb(parent, (u16)(0x18 + i)) : kjft[i];
		if (s < NUM_SFT && sft[s].open != 0) {
			pokeb(psp, (u16)(0x18 + i), s);
			if (sft[s].open < 0xFF) {
				sft[s].open++;
			}
		} else {
			pokeb(psp, (u16)(0x18 + i), JFT_FREE);
		}
	}
}

static void file_flush(fhandle *f);

/* Drop one reference to an SFT slot; flush files at 0. */
static void sft_release(u8 s) {
	if (s >= NUM_SFT || sft[s].open == 0) {
		return;
	}
	if (sft[s].dev != DEV_FILE) {
		if (sft[s].open > 1) {
			sft[s].open--; /* devices never fully close */
		}
		return;
	}
	sft[s].open--;
	if (sft[s].open == 0) {
		file_flush(&sft[s]);
		share_file_closed(sft[s].drv, sft[s].dir_cluster, sft[s].dir_index);
	}
}

/* Process termination: release every handle the PSP still has. */
void file_jft_close_all(u16 psp) {
	u16 i;
	for (i = 0; i < JFT_SIZE; i++) {
		u8 s = peekb(psp, (u16)(0x18 + i));
		if (s != JFT_FREE) {
			sft_release(s);
			pokeb(psp, (u16)(0x18 + i), JFT_FREE);
		}
	}
}

void file_get_dta(u16 *seg, u16 *off) {
	*seg = dta_seg;
	*off = dta_off;
}

/* --- 0Dh: disk reset (flush the FAT cache) --- */
void f0d_diskreset(iregs __far *r) {
	(void)r;
	fat_commit();
}

/* --- 0Eh: select drive DL -> AL = number of logical drives --- */
void f0e_setdrive(iregs __far *r) {
	fat_set_default((u8)(r->dx & 0xFF)); /* invalid: silently kept */
	set_al(r, fat_drive_count());
}

/* --- 19h: get current drive -> AL --- */
void f19_getdrive(iregs __far *r) {
	set_al(r, fat_default_drive());
}

/* --- 1Ah: set DTA to DS:DX --- */
void f1a_setdta(iregs __far *r) {
	dta_seg = r->ds;
	dta_off = r->dx;
}

/* --- 2Fh: get DTA -> ES:BX --- */
void f2f_getdta(iregs __far *r) {
	r->es = dta_seg;
	r->bx = dta_off;
}

/* --- 36h: free space of drive DL (0=default, 1=A:, 3=C:).
 * AX=secs/clus BX=free CX=512 DX=total; AX=FFFF bad drive --- */
void f36_freespace(iregs __far *r) {
	u16 cl;
	u16 free_cnt = 0;
	u16 maxcl;
	u8  dl = (u8)(r->dx & 0xFF);
	u8  drv = dl == 0 ? fat_default_drive() : (u8)(dl - 1);
	if (fat_select(drv) != 0) {
		r->ax = 0xFFFF;
		return;
	}
	maxcl = fat_max_cluster();
	for (cl = 2; cl <= maxcl; cl++) {
		if (fat_next(cl) == 0) {
			free_cnt++;
		}
	}
	r->ax = fat_secs_per_clus();
	r->bx = free_cnt;
	r->cx = 512;
	r->dx = (u16)(maxcl - 1);
}

/* --- 3Bh: chdir to DS:DX --- */
void f3b_chdir(iregs __far *r) {
	u16 err = fat_chdir((const char __far *)MK_FP(r->ds, r->dx));
	if (err != 0) {
		int21_error(r, err);
	}
}

/* --- 47h: get current dir of drive DL into DS:SI (64 bytes) --- */
void f47_getcwd(iregs __far *r) {
	const char *p;
	u8 __far   *dst;
	u8          dl = (u8)(r->dx & 0xFF);
	u8          drv = dl == 0 ? fat_default_drive() : (u8)(dl - 1);
	p = fat_get_cwd(drv);
	if (p == 0) {
		int21_error(r, 0x0F); /* invalid drive */
		return;
	}
	dst = (u8 __far *)MK_FP(r->ds, r->si);
	while (*p != '\0') {
		*dst = (u8)*p;
		dst++;
		p++;
	}
	*dst = 0;
	r->ax = 0x0100; /* documented success quirk */
}

/* DOS device names match in any directory, extension ignored. */
static u8 device_of_name(const u8 *n) {
	if (n[3] == ' ' && n[4] == ' ' && n[5] == ' ' && n[6] == ' ' &&
	    n[7] == ' ') {
		if (n[0] == 'C' && n[1] == 'O' && n[2] == 'N') {
			return DEV_CON;
		}
		if (n[0] == 'A' && n[1] == 'U' && n[2] == 'X') {
			return DEV_AUX;
		}
		if (n[0] == 'P' && n[1] == 'R' && n[2] == 'N') {
			return DEV_NUL;
		}
		if (n[0] == 'N' && n[1] == 'U' && n[2] == 'L') {
			return DEV_NUL;
		}
	}
	if (n[0] == 'E' && n[1] == 'M' && n[2] == 'M' && n[3] == 'X' &&
	    n[4] == 'X' && n[5] == 'X' && n[6] == 'X' && n[7] == '0' &&
	    ems_avail()) {
		return DEV_EMM;
	}
	return DEV_FILE;
}

/* '?' left in a name11 means the caller passed a wildcard;
 * create/delete/rename/mkdir refuse those. */
static int name_has_wild(const u8 *n) {
	u16 i;
	for (i = 0; i < 11; i++) {
		if (n[i] == '?') {
			return 1;
		}
	}
	return 0;
}

/* Allocate a JFT slot + SFT slot pair. Returns the JFT handle
 * (>= 0) with *out pointing at the zeroed SFT entry, or -1. */
static int handle_alloc(fhandle **out) {
	int h = find_free_jft();
	int s = find_free_sft();
	if (h < 0 || s < 0) {
		return -1;
	}
	fmemset(&sft[s], 0, sizeof(fhandle));
	sft[s].open = 1;
	jft_set((u16)h, (u8)s);
	*out = &sft[s];
	return h;
}

/* Nonzero when an open SFT entry references the same file and
 * its sharing mode conflicts with a new open using newmode
 * (full AL). newmode 0xFF = any open entry conflicts (create
 * truncating, delete, rename paths). */
static int share_conflict(u8 drv, u16 dcl, u16 idx, u8 newmode) {
	u16 s;
	for (s = 0; s < NUM_SFT; s++) {
		if (sft[s].open == 0 || sft[s].dev != DEV_FILE) {
			continue;
		}
		if (sft[s].drv != drv || sft[s].dir_cluster != dcl ||
		    sft[s].dir_index != idx) {
			continue;
		}
		if (newmode == 0xFF || !share_mode_compat(sft[s].smode, newmode)) {
			return 1;
		}
	}
	return 0;
}

/* Core of open: resolve, device check, allocate handle.
 * Returns handle (>= 0) or -(DOS error code). mode is the
 * full open AL: access bits 0-2, sharing mode bits 4-6. */
static int open_core(const char __far *path, u8 mode) {
	u8       name11[11];
	u16      dir_cl;
	u16      err;
	u16      idx;
	dirent83 e;
	int      h;
	u8       dev;
	u8       access = (u8)(mode & 7);
	fhandle *f;

	err = fat_resolve_dir(path, &dir_cl, name11);
	if (err != 0) {
		return -(int)err;
	}
	dev = device_of_name(name11);
	if (dev == DEV_FILE) {
		if (fat_dir_search_i(dir_cl, name11, &e, &idx) != 0) {
			return -(int)ERR_FILE_NOT_FOUND;
		}
		if ((e.attr & ATTR_DIR) != 0) {
			return -(int)ERR_ACCESS_DENIED;
		}
		if (access != 0 && (e.attr & ATTR_READONLY) != 0) {
			return -(int)ERR_ACCESS_DENIED;
		}
		if (share_conflict(fat_cur_drive(), dir_cl, idx, mode)) {
			return -(int)ERR_SHARING_VIOLATION;
		}
	}
	h = handle_alloc(&f);
	if (h < 0) {
		return -(int)ERR_TOO_MANY_FILES;
	}
	f->dev = dev;
	f->mode = access;
	f->smode = mode;
	f->drv = fat_cur_drive();
	if (dev == DEV_FILE) {
		f->dir_cluster = dir_cl;
		f->dir_index = idx;
		f->first_cluster = e.cluster;
		f->cur_cluster = e.cluster;
		f->size = e.size;
		f->ftime = e.time;
		f->fdate = e.date;
	}
	return h;
}

/* Core of create: truncate an existing file or build a fresh
 * dirent. Returns a read/write handle or -(DOS error code).
 * With fail_exists (5Bh) an existing file is an error. */
static int create_core(const char __far *path, u8 attr, u8 fail_exists) {
	u8       name11[11];
	u16      dir_cl;
	u16      err;
	u16      idx;
	dirent83 e;
	int      h;
	u8       dev;
	fhandle *f;

	err = fat_resolve_dir(path, &dir_cl, name11);
	if (err != 0) {
		return -(int)err;
	}
	if (name_has_wild(name11)) {
		return -(int)ERR_PATH_NOT_FOUND;
	}
	dev = device_of_name(name11);
	if (dev != DEV_FILE) {
		h = handle_alloc(&f);
		if (h < 0) {
			return -(int)ERR_TOO_MANY_FILES;
		}
		f->dev = dev;
		f->mode = 2;
		f->smode = 2;
		return h;
	}
	attr = (u8)((attr & 0x27) | ATTR_ARCHIVE); /* RO/HID/SYS/ARCH only */
	if (fat_dir_search_i(dir_cl, name11, &e, &idx) == 0) {
		if (fail_exists) {
			return -(int)0x50; /* file already exists */
		}
		if ((e.attr & (ATTR_DIR | ATTR_READONLY)) != 0) {
			return -(int)ERR_ACCESS_DENIED;
		}
		if (share_conflict(fat_cur_drive(), dir_cl, idx, 0xFF)) {
			return -(int)ERR_SHARING_VIOLATION; /* open file: no truncate */
		}
		if (e.cluster != 0) {
			fat_free_chain(e.cluster);
		}
	} else {
		if (fat_dir_alloc_slot(dir_cl, &idx) != 0) {
			return -(int)ERR_ACCESS_DENIED; /* directory full */
		}
		fmemset(&e, 0, sizeof(e));
		fmemcpy(e.name, name11, 11);
	}
	e.attr = attr;
	e.cluster = 0;
	e.size = 0;
	e.time = clock_dos_time();
	e.date = clock_dos_date();
	if (fat_dir_set(dir_cl, idx, &e) != 0 || fat_commit() != 0) {
		return -(int)ERR_ACCESS_DENIED;
	}
	h = handle_alloc(&f);
	if (h < 0) {
		return -(int)ERR_TOO_MANY_FILES;
	}
	f->dev = DEV_FILE;
	f->mode = 2;
	f->smode = 2;
	f->drv = fat_cur_drive();
	f->dir_cluster = dir_cl;
	f->dir_index = idx;
	f->ftime = e.time;
	f->fdate = e.date;
	return h;
}

/* --- 3Ch: create/truncate DS:DX, CX = attributes --- */
void f3c_create(iregs __far *r) {
	int h = create_core((const char __far *)MK_FP(r->ds, r->dx),
	                    (u8)(r->cx & 0xFF), 0);
	if (h < 0) {
		int21_error(r, (u16)(-h));
		return;
	}
	r->ax = (u16)h;
}

/* --- 5Bh: create new file, fail if it exists --- */
void f5b_createnew(iregs __far *r) {
	int h = create_core((const char __far *)MK_FP(r->ds, r->dx),
	                    (u8)(r->cx & 0xFF), 1);
	if (h < 0) {
		int21_error(r, (u16)(-h));
		return;
	}
	r->ax = (u16)h;
}

/* --- 3Dh: open DS:DX, AL = access + sharing mode --- */
void f3d_open(iregs __far *r) {
	int h =
	    open_core((const char __far *)MK_FP(r->ds, r->dx), (u8)(r->ax & 0xFF));
	if (h < 0) {
		int21_error(r, (u16)(-h));
		return;
	}
	r->ax = (u16)h;
}

/* Write the handle's size/cluster/stamp back into its dirent. */
static void file_flush(fhandle *f) {
	dirent83 e;
	if (!f->dirty) {
		return;
	}
	fat_select(f->drv);
	if (fat_dir_entry(f->dir_cluster, f->dir_index, &e) == 0) {
		e.cluster = f->first_cluster;
		e.size = f->size;
		e.time = f->ftime;
		e.date = f->fdate;
		e.attr |= ATTR_ARCHIVE;
		fat_dir_set(f->dir_cluster, f->dir_index, &e);
	}
	fat_commit();
	f->dirty = 0;
}

/* --- 3Eh: close handle BX --- */
void f3e_close(iregs __far *r) {
	u16 h = r->bx;
	if (handle_of(h) == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	sft_release(jft_get(h));
	jft_set(h, JFT_FREE);
}

/* --- 45h: duplicate handle BX -> AX (shares file position) --- */
void f45_dup(iregs __far *r) {
	fhandle *f = handle_of(r->bx);
	int      h2;
	u8       s;
	if (f == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	s = jft_get(r->bx);
	h2 = find_free_jft();
	if (h2 < 0) {
		int21_error(r, ERR_TOO_MANY_FILES);
		return;
	}
	jft_set((u16)h2, s);
	if (sft[s].open < 0xFF) {
		sft[s].open++;
	}
	r->ax = (u16)h2;
}

/* --- 46h: force handle CX to refer to handle BX --- */
void f46_forcedup(iregs __far *r) {
	fhandle *f = handle_of(r->bx);
	u8       s;
	if (f == 0 || r->cx >= JFT_SIZE) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	if (r->cx == r->bx) {
		return;
	}
	s = jft_get(r->bx);
	if (handle_of(r->cx) != 0) {
		sft_release(jft_get(r->cx));
	}
	jft_set(r->cx, s);
	if (sft[s].open < 0xFF) {
		sft[s].open++;
	}
}

/* --- 5Ch: lock (AL=0) / unlock (AL=1) region of handle BX,
 * CX:DX = start offset, SI:DI = length (share.c table) --- */
void f5c_lock(iregs __far *r) {
	fhandle *f = handle_of(r->bx);
	u32      start, len;
	u16      err;
	u8       sub = (u8)(r->ax & 0xFF);
	if (f == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	if (sub > 1) {
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
	if (f->dev != DEV_FILE) {
		return; /* device handles: accepted no-op */
	}
	start = ((u32)r->cx << 16) | r->dx;
	len = ((u32)r->si << 16) | r->di;
	if (sub == 0) {
		err = share_lock(f->drv, f->dir_cluster, f->dir_index, proc_get_psp(),
		                 start, len);
	} else {
		err = share_unlock(f->drv, f->dir_cluster, f->dir_index, proc_get_psp(),
		                   start, len);
	}
	if (err != 0) {
		int21_error(r, err);
	}
}

/* Cooked CON read: returns at most one line per call, echoes,
 * line ends with CR LF; leftover bytes go to the next read. */
static u16 con_cooked_read(u8 __far *dst, u16 count) {
	u16 done = 0;
	if (count == 0) {
		return 0;
	}
	if (line_pos >= line_len) {
		u8 n = 0;
		if (break_check()) {
			return 0;
		}
		for (;;) {
			u8 c = con_getc();
			if (c == 3) { /* Ctrl-C aborts the line */
				break_signal();
				return 0;
			}
			if (c == '\r') {
				con_putc('\r');
				con_putc('\n');
				line_buf[n++] = '\r';
				line_buf[n++] = '\n';
				break;
			}
			if (c == 0x08) {
				if (n > 0) {
					n--;
					con_putc(0x08);
					con_putc(' ');
					con_putc(0x08);
				}
				continue;
			}
			if (c == 0) {
				if (break_check()) { /* Ctrl-Break's 0000h word */
					return 0;
				}
				(void)con_getc(); /* swallow extended key */
				continue;
			}
			if (n < 127) {
				line_buf[n++] = c;
				con_putc(c);
			}
		}
		line_len = n;
		line_pos = 0;
	}
	while (done < count && line_pos < line_len) {
		dst[done++] = line_buf[line_pos++];
	}
	return done;
}

static u16 file_do_read(fhandle *f, u8 __far *dst, u16 count) {
	u16 done = 0;
	u16 cb;
	u32 remain;

	if (f->pos >= f->size) {
		return 0;
	}
	fat_select(f->drv);
	cb = fat_cluster_bytes();
	remain = f->size - f->pos;
	if ((u32)count > remain) {
		count = (u16)remain;
	}
	if (f->pos < f->cur_cluster_pos) { /* seeked backwards */
		f->cur_cluster = f->first_cluster;
		f->cur_cluster_pos = 0;
	}
	while (f->cur_cluster_pos + cb <= f->pos) { /* seeked forwards */
		u16 nx = fat_next(f->cur_cluster);
		if (nx == 0xFFFF) {
			return done;
		}
		f->cur_cluster = nx;
		f->cur_cluster_pos += cb;
	}
	while (done < count) {
		u16 in_clus = (u16)(f->pos - f->cur_cluster_pos);
		u16 sec = (u16)(in_clus >> 9);
		u16 off = (u16)(in_clus & 511);
		u16 n;
		if (fat_load_sector(fat_cluster_lba(f->cur_cluster) + sec) != 0) {
			break;
		}
		n = (u16)(512 - off);
		if (n > (u16)(count - done)) {
			n = (u16)(count - done);
		}
		fmemcpy(dst + done, disk_buf + off, n);
		f->pos += n;
		done += n;
		if (f->pos - f->cur_cluster_pos >= cb && done < count) {
			u16 nx = fat_next(f->cur_cluster);
			if (nx == 0xFFFF) {
				break;
			}
			f->cur_cluster = nx;
			f->cur_cluster_pos += cb;
		}
	}
	return done;
}

/* --- 3Fh: read CX bytes from handle BX into DS:DX -> AX --- */
void f3f_read(iregs __far *r) {
	fhandle  *f = handle_of(r->bx);
	u8 __far *dst = (u8 __far *)MK_FP(r->ds, r->dx);
	if (f == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	switch (f->dev) {
	case DEV_CON:
		r->ax = con_cooked_read(dst, r->cx);
		return;
	case DEV_AUX:
	case DEV_NUL:
	case DEV_EMM:
		r->ax = 0; /* EOF */
		return;
	default:
		if (share_io_check(f->drv, f->dir_cluster, f->dir_index, proc_get_psp(),
		                   f->pos, r->cx) != 0) {
			int21_error(r, ERR_LOCK_VIOLATION);
			return;
		}
		r->ax = file_do_read(f, dst, r->cx);
		return;
	}
}

/* Write count bytes at f->pos, allocating clusters as needed.
 * Data is staged through disk_buf (read-modify-write for
 * partial sectors). Returns bytes written (may be short when
 * the disk fills up). */
static u16 file_do_write(fhandle *f, const u8 __far *src, u16 count) {
	u16 done = 0;
	u16 cb;

	fat_select(f->drv);
	cb = fat_cluster_bytes();
	if (f->first_cluster == 0) { /* fresh/truncated file */
		u16 cl = fat_alloc(0);
		if (cl == 0) {
			return 0; /* disk full */
		}
		f->first_cluster = cl;
		f->cur_cluster = cl;
		f->cur_cluster_pos = 0;
		f->dirty = 1;
	}
	if (f->pos < f->cur_cluster_pos) { /* seeked backwards */
		f->cur_cluster = f->first_cluster;
		f->cur_cluster_pos = 0;
	}
	while (f->cur_cluster_pos + cb <= f->pos) {
		u16 nx = fat_next(f->cur_cluster);
		if (nx == 0xFFFF) { /* extend chain (seek past EOF) */
			nx = fat_alloc(f->cur_cluster);
			if (nx == 0) {
				return 0;
			}
			f->dirty = 1;
		}
		f->cur_cluster = nx;
		f->cur_cluster_pos += cb;
	}
	while (done < count) {
		u16 in_clus = (u16)(f->pos - f->cur_cluster_pos);
		u16 sec = (u16)(in_clus >> 9);
		u16 off = (u16)(in_clus & 511);
		u32 lba = fat_cluster_lba(f->cur_cluster) + sec;
		u16 n = (u16)(512 - off);
		if (n > (u16)(count - done)) {
			n = (u16)(count - done);
		}
		if (n != 512 && fat_load_sector(lba) != 0) {
			break; /* partial sector: RMW */
		}
		fmemcpy(disk_buf + off, src + done, n);
		if (fat_store_buf(lba) != 0) {
			break;
		}
		f->pos += n;
		done += n;
		if (f->pos > f->size) {
			f->size = f->pos;
			f->dirty = 1;
		}
		if (f->pos - f->cur_cluster_pos >= cb && done < count) {
			u16 nx = fat_next(f->cur_cluster);
			if (nx == 0xFFFF) {
				nx = fat_alloc(f->cur_cluster);
				if (nx == 0) {
					break;
				}
				f->dirty = 1;
			}
			f->cur_cluster = nx;
			f->cur_cluster_pos += cb;
		}
	}
	if (done != 0) {
		f->ftime = clock_dos_time();
		f->fdate = clock_dos_date();
		f->dirty = 1;
	}
	return done;
}

/* AH=40h with CX=0: truncate the file at the current position
 * (shrink only; extending writes zero bytes is not supported). */
static void file_truncate(fhandle *f) {
	u16 cb;
	if (f->pos >= f->size) {
		return;
	}
	fat_select(f->drv);
	cb = fat_cluster_bytes();
	if (f->pos == 0) {
		if (f->first_cluster != 0) {
			fat_free_chain(f->first_cluster);
			f->first_cluster = 0;
		}
	} else {
		u16 keep = (u16)((f->pos + cb - 1) / cb); /* clusters kept */
		u16 cl = f->first_cluster;
		u16 i, nx;
		for (i = 1; i < keep; i++) {
			nx = fat_next(cl);
			if (nx == 0xFFFF) {
				break;
			}
			cl = nx;
		}
		nx = fat_next(cl);
		fat_set(cl, 0xFFFF);
		if (nx >= 2 && nx != 0xFFFF) {
			fat_free_chain(nx);
		}
	}
	f->size = f->pos;
	f->cur_cluster = f->first_cluster;
	f->cur_cluster_pos = 0;
	f->ftime = clock_dos_time();
	f->fdate = clock_dos_date();
	f->dirty = 1;
	file_flush(f);
}

/* --- 40h: write CX bytes from DS:DX to handle BX -> AX --- */
void f40_write(iregs __far *r) {
	fhandle        *f = handle_of(r->bx);
	u16             n = r->cx;
	const u8 __far *src = (const u8 __far *)MK_FP(r->ds, r->dx);
	u16             i;
	if (f == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	switch (f->dev) {
	case DEV_CON:
		(void)break_check(); /* ^C aborts console writes via INT 23h */
		for (i = 0; i < n; i++) {
			con_putc(src[i]);
		}
		r->ax = n;
		return;
	case DEV_AUX:
		for (i = 0; i < n; i++) {
			serial_putc(src[i]);
		}
		r->ax = n;
		return;
	case DEV_NUL:
		r->ax = n;
		return;
	default:
		if (f->mode == 0) {
			int21_error(r, ERR_ACCESS_DENIED); /* opened read-only */
			return;
		}
		if (share_io_check(f->drv, f->dir_cluster, f->dir_index, proc_get_psp(),
		                   f->pos, n) != 0) {
			int21_error(r, ERR_LOCK_VIOLATION);
			return;
		}
		if (n == 0) {
			file_truncate(f);
			r->ax = 0;
			return;
		}
		r->ax = file_do_write(f, src, n);
		return;
	}
}

/* --- 42h: seek handle BX, AL=whence, CX:DX=offset -> DX:AX --- */
void f42_seek(iregs __far *r) {
	fhandle *f = handle_of(r->bx);
	u8       whence = (u8)(r->ax & 0xFF);
	u32      off = ((u32)r->cx << 16) | r->dx;
	u32      newpos;
	if (f == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	if (whence == 0) {
		newpos = off;
	} else if (whence == 1) {
		newpos = f->pos + off;
	} else if (whence == 2) {
		newpos = f->size + off;
	} else {
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
	f->pos = newpos; /* past EOF allowed, like DOS */
	r->dx = (u16)(newpos >> 16);
	r->ax = (u16)newpos;
}

/* --- 43h: get (AL=0) / set (AL=1) attributes of DS:DX --- */
void f43_attrib(iregs __far *r) {
	u8       name11[11];
	u16      dir_cl, idx, err;
	dirent83 e;
	u8       sub = (u8)(r->ax & 0xFF);
	err = fat_resolve_dir((const char __far *)MK_FP(r->ds, r->dx), &dir_cl,
	                      name11);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (fat_dir_search_i(dir_cl, name11, &e, &idx) != 0) {
		int21_error(r, ERR_FILE_NOT_FOUND);
		return;
	}
	if (sub == 0) {
		r->cx = e.attr;
		return;
	}
	if (sub != 1) {
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
	/* directory/volume bits cannot be changed */
	e.attr = (u8)((e.attr & (ATTR_DIR | ATTR_VOLUME)) | ((u8)r->cx & 0x27));
	if (fat_dir_set(dir_cl, idx, &e) != 0 || fat_commit() != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
	}
}

/* --- 44h: IOCTL --- */
void f44_ioctl(iregs __far *r) {
	u8       sub = (u8)(r->ax & 0xFF);
	fhandle *f;
	switch (sub) {
	case 0x00: /* get device info -> DX */
		f = handle_of(r->bx);
		if (f == 0) {
			int21_error(r, ERR_INVALID_HANDLE);
			return;
		}
		switch (f->dev) {
		case DEV_CON:
			r->dx = 0x80D3;
			break;
		case DEV_AUX:
			r->dx = 0x8080;
			break;
		case DEV_NUL:
			r->dx = 0x8084;
			break;
		case DEV_EMM:
			r->dx = 0xC080; /* char device, supports IOCTL */
			break;
		default: /* file: drive 0, bit6 = clean */
			r->dx = (u16)(f->dirty ? 0x0000 : 0x0040);
			break;
		}
		return;
	case 0x01: /* set device info: accept */
		if (handle_of(r->bx) == 0) {
			int21_error(r, ERR_INVALID_HANDLE);
		}
		return;
	case 0x06: /* input status */
		f = handle_of(r->bx);
		if (f == 0) {
			int21_error(r, ERR_INVALID_HANDLE);
			return;
		}
		if (f->dev == DEV_CON) {
			set_al(r, con_kbhit() ? 0xFF : 0x00);
		} else if (f->dev == DEV_FILE) {
			set_al(r, f->pos < f->size ? 0xFF : 0x00);
		} else {
			set_al(r, 0x00);
		}
		return;
	case 0x07: /* output status: always ready */
		if (handle_of(r->bx) == 0) {
			int21_error(r, ERR_INVALID_HANDLE);
			return;
		}
		set_al(r, 0xFF);
		return;
	case 0x08: /* removable? AX=0 = removable */
	{
		u8 bl = (u8)r->bx;
		u8 drv = bl == 0 ? fat_default_drive() : (u8)(bl - 1);
		if (!fat_drive_present(drv)) {
			int21_error(r, 0x0F);
			return;
		}
		r->ax = (u16)(drv >= 2 ? 1 : 0); /* C: fixed, A:/B: removable */
		return;
	}
	case 0x0E: /* logical drive map: 1 letter */
		set_al(r, 0);
		return;
	default:
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
}

/* --- 57h: get (AL=0) / set (AL=1) file stamp of handle BX --- */
void f57_filetimes(iregs __far *r) {
	fhandle *f = handle_of(r->bx);
	u8       sub = (u8)(r->ax & 0xFF);
	if (f == 0 || f->dev != DEV_FILE) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	if (sub == 0) {
		r->cx = f->ftime;
		r->dx = f->fdate;
		return;
	}
	if (sub != 1) {
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
	f->ftime = r->cx;
	f->fdate = r->dx;
	f->dirty = 1;
}

/* --- 68h: commit file (flush handle BX to disk) --- */
void f68_commit(iregs __far *r) {
	fhandle *f = handle_of(r->bx);
	if (f == 0) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	if (f->dev == DEV_FILE) {
		file_flush(f);
	}
}

/* --- path-based cores (shared with the FCB layer) --- */

u16 file_unlink_path(const char __far *path) {
	u8       name11[11];
	u16      dir_cl, idx, err;
	dirent83 e;
	err = fat_resolve_dir(path, &dir_cl, name11);
	if (err != 0) {
		return err;
	}
	if (name_has_wild(name11)) {
		return ERR_PATH_NOT_FOUND;
	}
	if (fat_dir_search_i(dir_cl, name11, &e, &idx) != 0) {
		return ERR_FILE_NOT_FOUND;
	}
	if ((e.attr & (ATTR_DIR | ATTR_READONLY)) != 0) {
		return ERR_ACCESS_DENIED;
	}
	if (e.cluster != 0) {
		fat_free_chain(e.cluster);
	}
	e.name[0] = 0xE5;
	if (fat_dir_set(dir_cl, idx, &e) != 0 || fat_commit() != 0) {
		return ERR_ACCESS_DENIED;
	}
	return 0;
}

u16 file_rename_path(const char __far *oldp, const char __far *newp) {
	u8       old11[11], new11[11];
	u16      odir, ndir, oidx, nidx, err;
	dirent83 e, probe;
	err = fat_resolve_dir(oldp, &odir, old11);
	if (err == 0) {
		err = fat_resolve_dir(newp, &ndir, new11);
	}
	if (err != 0) {
		return err;
	}
	if (name_has_wild(old11) || name_has_wild(new11)) {
		return ERR_PATH_NOT_FOUND;
	}
	if (fat_dir_search_i(odir, old11, &e, &oidx) != 0) {
		return ERR_FILE_NOT_FOUND;
	}
	if (fat_dir_search(ndir, new11, &probe) == 0) {
		return ERR_ACCESS_DENIED; /* target exists */
	}
	if (odir == ndir) {
		fmemcpy(e.name, new11, 11); /* rename in place */
		if (fat_dir_set(odir, oidx, &e) != 0) {
			return ERR_ACCESS_DENIED;
		}
		return 0;
	}
	if (fat_dir_alloc_slot(ndir, &nidx) != 0) {
		return ERR_ACCESS_DENIED;
	}
	fmemcpy(e.name, new11, 11);
	if (fat_dir_set(ndir, nidx, &e) != 0) {
		return ERR_ACCESS_DENIED;
	}
	if ((e.attr & ATTR_DIR) != 0) { /* fix ".." of moved dir */
		dirent83 dd;
		if (fat_dir_entry(e.cluster, 1, &dd) == 0 && dd.name[0] == '.' &&
		    dd.name[1] == '.') {
			dd.cluster = ndir;
			fat_dir_set(e.cluster, 1, &dd);
		}
	}
	e.name[0] = 0xE5; /* drop the old entry */
	if (fat_dir_set(odir, oidx, &e) != 0 || fat_commit() != 0) {
		return ERR_ACCESS_DENIED;
	}
	return 0;
}

/* --- 41h: delete file at DS:DX (no wildcards) --- */
void f41_unlink(iregs __far *r) {
	u16 err = file_unlink_path((const char __far *)MK_FP(r->ds, r->dx));
	if (err != 0) {
		int21_error(r, err);
	}
}

/* --- 56h: rename/move DS:DX -> ES:DI --- */
void f56_rename(iregs __far *r) {
	u16 err = file_rename_path((const char __far *)MK_FP(r->ds, r->dx),
	                           (const char __far *)MK_FP(r->es, r->di));
	if (err != 0) {
		int21_error(r, err);
	}
}

/* --- 39h: create directory at DS:DX --- */
void f39_mkdir(iregs __far *r) {
	u8       name11[11];
	u16      dir_cl, idx, err, cl;
	dirent83 e;
	err = fat_resolve_dir((const char __far *)MK_FP(r->ds, r->dx), &dir_cl,
	                      name11);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (name_has_wild(name11) || device_of_name(name11) != DEV_FILE) {
		int21_error(r, ERR_PATH_NOT_FOUND);
		return;
	}
	if (fat_dir_search(dir_cl, name11, &e) == 0) {
		int21_error(r, ERR_ACCESS_DENIED); /* already exists */
		return;
	}
	if (fat_dir_alloc_slot(dir_cl, &idx) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	cl = fat_alloc(0);
	if (cl == 0 || fat_zero_cluster(cl) != 0) {
		int21_error(r, ERR_ACCESS_DENIED); /* disk full */
		return;
	}
	fmemset(&e, 0, sizeof(e)); /* "." entry */
	fmemset(e.name, ' ', 11);
	e.name[0] = '.';
	e.attr = ATTR_DIR;
	e.time = clock_dos_time();
	e.date = clock_dos_date();
	e.cluster = cl;
	if (fat_dir_set(cl, 0, &e) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	e.name[1] = '.';    /* ".." entry */
	e.cluster = dir_cl; /* 0 = root, per spec */
	if (fat_dir_set(cl, 1, &e) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	fmemcpy(e.name, name11, 11); /* parent entry */
	e.cluster = cl;
	if (fat_dir_set(dir_cl, idx, &e) != 0 || fat_commit() != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
	}
}

/* --- 3Ah: remove empty directory at DS:DX --- */
void f3a_rmdir(iregs __far *r) {
	u8       name11[11];
	u16      dir_cl, idx, err, i;
	dirent83 e, scan;
	err = fat_resolve_dir((const char __far *)MK_FP(r->ds, r->dx), &dir_cl,
	                      name11);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (name_has_wild(name11)) {
		int21_error(r, ERR_PATH_NOT_FOUND);
		return;
	}
	if (fat_dir_search_i(dir_cl, name11, &e, &idx) != 0) {
		int21_error(r, ERR_PATH_NOT_FOUND);
		return;
	}
	if ((e.attr & ATTR_DIR) == 0 || (e.attr & ATTR_READONLY) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	if (e.cluster == fat_cwd_cluster()) {
		int21_error(r, ERR_CURRENT_DIR);
		return;
	}
	for (i = 2; fat_dir_entry(e.cluster, i, &scan) == 0; i++) {
		if (scan.name[0] == 0x00) {
			break;
		}
		if (scan.name[0] != 0xE5) {
			int21_error(r, ERR_ACCESS_DENIED); /* not empty */
			return;
		}
	}
	fat_free_chain(e.cluster);
	e.name[0] = 0xE5;
	if (fat_dir_set(dir_cl, idx, &e) != 0 || fat_commit() != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
	}
}

/* Convert "FOO     BAR" to "FOO.BAR" ASCIIZ in the result. */
static void find_store_result(findstate __far *fs, const dirent83 *e) {
	u16 i;
	u16 n = 0;
	fs->res_attr = e->attr;
	fs->res_time = e->time;
	fs->res_date = e->date;
	fs->res_size = e->size;
	for (i = 0; i < 8 && e->name[i] != ' '; i++) {
		fs->res_name[n++] = (char)e->name[i];
	}
	if (e->name[8] != ' ') {
		fs->res_name[n++] = '.';
		for (i = 8; i < 11 && e->name[i] != ' '; i++) {
			fs->res_name[n++] = (char)e->name[i];
		}
	}
	fs->res_name[n] = '\0';
}

/* DOS search rule: skip the entry if it has hidden/system/
 * volume/dir bits that are not present in the search mask. */
static int find_attr_ok(u8 entry_attr, u8 mask) {
	u8 special =
	    (u8)(entry_attr & (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME | ATTR_DIR));
	return (u8)(special & (u8)~mask) == 0;
}

static void find_continue(iregs __far *r, findstate __far *fs) {
	dirent83 e;
	u8       pat[11];
	u16      idx = fs->index;
	if (fat_select(fs->drv) != 0) {
		int21_error(r, ERR_NO_MORE_FILES);
		return;
	}
	fmemcpy(pat, fs->pattern, 11);
	while (fat_dir_entry(fs->dir_cluster, idx, &e) == 0) {
		if (e.name[0] == 0x00) {
			break;
		}
		idx++;
		if (e.name[0] == 0xE5 || e.attr == 0x0F) {
			continue;
		}
		if (!fat_match11(pat, e.name)) {
			continue;
		}
		if (!find_attr_ok(e.attr, fs->attr)) {
			continue;
		}
		fs->index = idx;
		find_store_result(fs, &e);
		return;
	}
	fs->index = idx;
	int21_error(r, ERR_NO_MORE_FILES);
}

/* --- 4Eh: FindFirst, DS:DX = pattern path, CX = attr mask --- */
void f4e_findfirst(iregs __far *r) {
	findstate __far *fs = (findstate __far *)MK_FP(dta_seg, dta_off);
	u8               name11[11];
	u16              dir_cl;
	u16              err;
	err = fat_resolve_dir((const char __far *)MK_FP(r->ds, r->dx), &dir_cl,
	                      name11);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	fmemcpy(fs->pattern, name11, 11);
	fs->attr = (u8)(r->cx & 0xFF);
	fs->dir_cluster = dir_cl;
	fs->index = 0;
	fs->drv = fat_cur_drive();
	find_continue(r, fs);
}

/* --- 4Fh: FindNext, continues from the DTA state --- */
void f4f_findnext(iregs __far *r) {
	findstate __far *fs = (findstate __far *)MK_FP(dta_seg, dta_off);
	find_continue(r, fs);
}

/* --- kernel-internal file access (EXEC loader, FCB layer) --- */

int kfile_open(const char __far *path) {
	int h = open_core(path, 2);
	if (h == -(int)ERR_ACCESS_DENIED) {
		h = open_core(path, 0); /* read-only file: degrade */
	}
	return h;
}

int kfile_create(const char __far *path) {
	return create_core(path, 0, 0);
}

void kfile_close(int h) {
	if (h >= 0 && handle_of((u16)h) != 0) {
		sft_release(jft_get((u16)h));
		jft_set((u16)h, JFT_FREE);
	}
}

u16 kfile_read(int h, void __far *dst, u16 n) {
	fhandle *f = handle_of((u16)h);
	if (f == 0) {
		return 0;
	}
	return file_do_read(f, (u8 __far *)dst, n);
}

u16 kfile_write(int h, const void __far *src, u16 n) {
	fhandle *f = handle_of((u16)h);
	if (f == 0) {
		return 0;
	}
	return file_do_write(f, (const u8 __far *)src, n);
}

void kfile_seek_set(int h, u32 pos) {
	fhandle *f = handle_of((u16)h);
	if (f != 0) {
		f->pos = pos;
	}
}

u32 kfile_size(int h) {
	fhandle *f = handle_of((u16)h);
	return f != 0 ? f->size : 0;
}

void kfile_stamp(int h, u16 *time, u16 *date) {
	fhandle *f = handle_of((u16)h);
	*time = f != 0 ? f->ftime : 0;
	*date = f != 0 ? f->fdate : 0;
}
