/* ============================================================
 * lfn.c - long file name (VFAT) support, INT 21h AX=71xx
 *
 * DOS 7 (Windows 95) added the LFN API: a multiplexed set of
 * INT 21h functions under AH=71h that operate on long file
 * names stored as VFAT directory slots (attribute 0x0F)
 * preceding each 8.3 entry. This module dispatches AL to the
 * individual subfunctions; the on-disk VFAT mechanics (slot
 * parsing, name assembly, checksum, multi-slot allocation,
 * short-name aliasing) live in fat.c.
 *
 * When a subfunction is not supported the call returns CF=1
 * with AX=7100h, the documented "LFN not present" reply, so
 * clients fall back to the classic 8.3 functions.
 * ============================================================ */
#include "kernel.h"

#define LFN_NAME_MAX    260 /* WIN32_FIND_DATA name field size */
#define LFN_MAX_HANDLES 4

/* WIN32_FIND_DATA as filled by 714Eh/714Fh at ES:DI (318 bytes). */
#pragma pack(push, 1)
typedef struct win_find_data {
	u32  attr;
	u32  ctime_lo, ctime_hi; /* creation FILETIME */
	u32  atime_lo, atime_hi; /* last-access FILETIME */
	u32  wtime_lo, wtime_hi; /* last-write FILETIME */
	u32  size_hi;
	u32  size_lo;
	u32  reserved0;
	u32  reserved1;
	char name[LFN_NAME_MAX]; /* long name, ASCIZ */
	char alt_name[14];       /* 8.3 alias, ASCIZ */
} win_find_data;
#pragma pack(pop)

/* Active find scan, keyed by the handle returned in AX. */
typedef struct lfn_find {
	u8   in_use;
	u8   drv;
	u8   attr; /* search attribute mask */
	u16  dir_cluster;
	u16  index;                 /* next directory entry to read */
	char pattern[LFN_NAME_MAX]; /* last-component pattern, upcased */
} lfn_find;

static lfn_find find_slots[LFN_MAX_HANDLES];

/* Scratch buffers kept in BSS rather than on the small kernel
 * stack (INT 21h is serialized, so static scratch is safe).
 * s_path/s_path2 hold path copies; s_name/s_short hold the name
 * assembled during a lookup or scan; s_tmp is find-first scratch.
 * Their uses never overlap within a single INT 21h call. */
static char     s_path[LFN_NAME_MAX];
static char     s_path2[LFN_NAME_MAX];
static char     s_name[LFN_NAME_MAX];
static char     s_short[16];
static lfn_find s_tmp;

/* "function not supported" -> CF set, AX=7100h (LFN absent) */
static void lfn_unsupported(iregs __far *r) {
	r->flags |= FL_CF;
	r->ax = 0x7100;
}

static void str_copy(char *dst, const char *src) {
	u16 i = 0;
	while (src[i] != '\0') {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static char up_c(char c) {
	return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

/* Case-insensitive wildcard match: '*' matches any run, '?'
 * matches one character. "*.*" is treated as "*". */
static int lfn_match(const char *pat, const char *name) {
	const char *star = 0;
	const char *ss = 0;
	if (pat[0] == '*' && pat[1] == '.' && pat[2] == '*' && pat[3] == '\0') {
		return 1;
	}
	while (*name != '\0') {
		char pc = up_c(*pat);
		char nc = up_c(*name);
		if (pc == '*') {
			star = pat++;
			ss = name;
		} else if (pc == '?' || pc == nc) {
			pat++;
			name++;
		} else if (star != 0) {
			pat = star + 1;
			name = ++ss;
		} else {
			return 0;
		}
	}
	while (*pat == '*') {
		pat++;
	}
	return *pat == '\0';
}

/* Format an 8.3 name[11] into "NAME.EXT" (ASCIZ). */
static void name83_to_str(const u8 *name11, char *out) {
	u16 i, n = 0;
	for (i = 0; i < 8 && name11[i] != ' '; i++) {
		out[n++] = (char)name11[i];
	}
	if (name11[8] != ' ') {
		out[n++] = '.';
		for (i = 8; i < 11 && name11[i] != ' '; i++) {
			out[n++] = (char)name11[i];
		}
	}
	out[n] = '\0';
}

/* DOS attribute filter, matching file.c's find_attr_ok: an
 * entry passes only if its hidden/system/volume/dir bits are
 * all permitted by the mask. */
static int attr_ok(u8 eattr, u8 mask) {
	u8 special =
	    (u8)(eattr & (ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME | ATTR_DIR));
	return (special & (u8)~mask) == 0;
}

/* Full 32x32 -> 64 unsigned multiply, no library helpers. */
static void mul32(u32 x, u32 y, u32 *lo, u32 *hi) {
	u16 xl = (u16)x, xh = (u16)(x >> 16);
	u16 yl = (u16)y, yh = (u16)(y >> 16);
	u32 ll = (u32)xl * yl;
	u32 lh = (u32)xl * yh;
	u32 hl = (u32)xh * yl;
	u32 hh = (u32)xh * yh;
	u32 mid = (ll >> 16) + (lh & 0xFFFF) + (hl & 0xFFFF);
	*lo = (ll & 0xFFFF) | (mid << 16);
	*hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}

static int is_leap(u16 y) {
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Seconds from 1980-01-01 for a FAT-packed date/time pair. */
static u32 dos_secs_1980(u16 date, u16 time) {
	static const u8 mdays[12] = {31, 28, 31, 30, 31, 30,
	                             31, 31, 30, 31, 30, 31};
	u16 year = (u16)(1980 + (date >> 9));
	u16 mon = (u16)((date >> 5) & 0x0F);
	u16 day = (u16)(date & 0x1F);
	u16 hour = (u16)(time >> 11);
	u16 min = (u16)((time >> 5) & 0x3F);
	u16 sec = (u16)((time & 0x1F) * 2);
	u32 days = 0;
	u16 y, m;
	if (mon < 1) {
		mon = 1;
	}
	if (day < 1) {
		day = 1;
	}
	for (y = 1980; y < year; y++) {
		days += is_leap(y) ? 366 : 365;
	}
	for (m = 1; m < mon; m++) {
		days += mdays[m - 1];
		if (m == 2 && is_leap(year)) {
			days++;
		}
	}
	days += (u32)(day - 1);
	return days * 86400UL + (u32)hour * 3600UL + (u32)min * 60UL + sec;
}

/* Convert a FAT date/time into a 64-bit Windows FILETIME
 * (100ns units since 1601-01-01). */
static void dos_to_filetime(u16 date, u16 time, u32 *lo, u32 *hi) {
	u32 secs = dos_secs_1980(date, time);
	u32 tlo = 3370100608UL; /* low 32 of 11960035200 (1601->1980) */
	u32 thi = 2;            /* high 32 of that offset */
	tlo += secs;
	if (tlo < secs) {
		thi++;
	}
	mul32(tlo, 10000000UL, lo, hi);
	*hi += thi * 10000000UL;
}

/* Fill the time fields of a find-data record. dos_fmt selects
 * the DOS date/time layout (SI bit 0) over Windows FILETIME. */
static void fill_times(win_find_data *fd, u16 ftime, u16 fdate, u8 dos_fmt) {
	u32 lo, hi;
	if (dos_fmt) {
		lo = ((u32)fdate << 16) | ftime;
		hi = 0;
	} else {
		dos_to_filetime(fdate, ftime, &lo, &hi);
	}
	fd->ctime_lo = fd->atime_lo = fd->wtime_lo = lo;
	fd->ctime_hi = fd->atime_hi = fd->wtime_hi = hi;
}

/* Copy a far ASCIZ path into a near buffer (truncating). */
static void path_copy(const char __far *src, char *dst, u16 max) {
	u16 i;
	for (i = 0; (u16)(i + 1) < max && src[i] != '\0'; i++) {
		dst[i] = (char)src[i];
	}
	dst[i] = '\0';
}

/* Advance one logical entry in a scan, fill *fd if a match is
 * found. Returns 1 = match stored, 0 = end of directory. */
static int find_scan(lfn_find *fs, win_find_data *fd, u8 dos_fmt) {
	dirent83 e;
	char    *longname = s_name;
	char    *shortstr = s_short;
	if (fat_select(fs->drv) != 0) {
		return 0;
	}
	while (fat_dir_next_lfn(fs->dir_cluster, &fs->index, &e, longname,
	                        LFN_NAME_MAX) == 1) {
		if (e.name[0] == 0xE5 || e.attr == ATTR_LFN) {
			continue; /* defensive: skip stray slots */
		}
		if (!attr_ok(e.attr, fs->attr)) {
			continue;
		}
		name83_to_str(e.name, shortstr);
		if (longname[0] == '\0') {
			str_copy(longname, shortstr);
		}
		if (!lfn_match(fs->pattern, longname) &&
		    !lfn_match(fs->pattern, shortstr)) {
			continue;
		}
		fd->attr = e.attr;
		fd->size_lo = e.size;
		fd->size_hi = 0;
		fd->reserved0 = 0;
		fd->reserved1 = 0;
		fill_times(fd, e.time, e.date, dos_fmt);
		str_copy(fd->name, longname);
		str_copy(fd->alt_name, shortstr);
		return 1;
	}
	return 0;
}

/* Resolve a path into its directory cluster and the (long)
 * last-component pattern. Returns 0 or a DOS error code. */
static u16 find_open(const char __far *path, lfn_find *fs) {
	char *near_path = s_path;
	u16   dir_cl;
	u8    last11[11];
	u16   err;
	char *p;
	char *base;
	err = fat_resolve_dir(path, &dir_cl, last11);
	if (err != 0) {
		return err;
	}
	path_copy(path, near_path, LFN_NAME_MAX);
	base = near_path;
	for (p = near_path; *p != '\0'; p++) {
		if (*p == '\\' || *p == '/' || *p == ':') {
			base = p + 1;
		}
	}
	fs->drv = fat_cur_drive();
	fs->dir_cluster = dir_cl;
	fs->index = 0;
	str_copy(fs->pattern, base);
	if (fs->pattern[0] == '\0') {
		fs->pattern[0] = '*';
		fs->pattern[1] = '\0';
	}
	return 0;
}

/* --- 714Eh: find first long-name match ---
 * DS:DX = ASCIZ path/pattern, ES:DI = WIN32_FIND_DATA,
 * CX = attribute mask, SI = date/time format (bit 0 = DOS).
 * Returns AX = search handle, CX = Unicode flags (0). */
static void f71_findfirst(iregs __far *r) {
	static win_find_data fd;
	u16                  err, h;
	u8                   dos_fmt = (u8)(r->si & 1);
	s_tmp.attr = (u8)(r->cx & 0xFF);
	err = find_open((const char __far *)MK_FP(r->ds, r->dx), &s_tmp);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	for (h = 0; h < LFN_MAX_HANDLES; h++) {
		if (!find_slots[h].in_use) {
			break;
		}
	}
	if (h == LFN_MAX_HANDLES) {
		int21_error(r, ERR_TOO_MANY_FILES);
		return;
	}
	if (!find_scan(&s_tmp, &fd, dos_fmt)) {
		int21_error(r, ERR_NO_MORE_FILES);
		return;
	}
	s_tmp.in_use = 1;
	find_slots[h] = s_tmp;
	fmemcpy(MK_FP(r->es, r->di), &fd, sizeof(fd));
	r->ax = h;
	r->cx = 0;
}

/* --- 714Fh: find next ---
 * BX = handle, ES:DI = WIN32_FIND_DATA, SI = date/time format. */
static void f71_findnext(iregs __far *r) {
	static win_find_data fd;
	u8                   dos_fmt = (u8)(r->si & 1);
	if (r->bx >= LFN_MAX_HANDLES || !find_slots[r->bx].in_use) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	if (!find_scan(&find_slots[r->bx], &fd, dos_fmt)) {
		int21_error(r, ERR_NO_MORE_FILES);
		return;
	}
	fmemcpy(MK_FP(r->es, r->di), &fd, sizeof(fd));
	r->ax = 0;
}

/* --- 71A1h: find close --- BX = handle. */
static void f71_findclose(iregs __far *r) {
	if (r->bx >= LFN_MAX_HANDLES || !find_slots[r->bx].in_use) {
		int21_error(r, ERR_INVALID_HANDLE);
		return;
	}
	find_slots[r->bx].in_use = 0;
	r->ax = 0;
}

/* ============================================================
 * Write path: long names are resolved to a short 8.3 alias and
 * the operation is delegated to the existing 8.3 handler. New
 * long names get VFAT slots written ahead of the 8.3 entry.
 * Directory components of a path are assumed to be 8.3.
 * ============================================================ */

static int ci_equal(const char *a, const char *b) {
	u16 i = 0;
	while (a[i] != '\0' && b[i] != '\0') {
		if (up_c(a[i]) != up_c(b[i])) {
			return 0;
		}
		i++;
	}
	return a[i] == '\0' && b[i] == '\0';
}

/* Resolve path to (parent dir cluster, near copy, last-component
 * pointer into the copy). Returns 0 or a DOS error code. */
static u16 lfn_split(const char __far *path, u16 *dir_cl, char *near_path,
                     char **base) {
	u8    last11[11];
	u16   err;
	char *p;
	err = fat_resolve_dir(path, dir_cl, last11);
	if (err != 0) {
		return err;
	}
	path_copy(path, near_path, LFN_NAME_MAX);
	*base = near_path;
	for (p = near_path; *p != '\0'; p++) {
		if (*p == '\\' || *p == '/' || *p == ':') {
			*base = p + 1;
		}
	}
	return 0;
}

/* Find longname in dir_cl on drive drv. 0 = found (*e83, 8.3
 * entry index *short_index), -1 = not found. */
static int lfn_lookup(u8 drv, u16 dir_cl, const char *longname, dirent83 *e83,
                      u16 *short_index) {
	u16      idx = 0;
	dirent83 e;
	char    *ln = s_name;
	char    *ss = s_short;
	if (fat_select(drv) != 0) {
		return -1;
	}
	while (fat_dir_next_lfn(dir_cl, &idx, &e, ln, LFN_NAME_MAX) == 1) {
		if (e.name[0] == 0xE5 || e.attr == ATTR_LFN ||
		    (e.attr & ATTR_VOLUME) != 0) {
			continue;
		}
		name83_to_str(e.name, ss);
		if (ln[0] == '\0') {
			str_copy(ln, ss);
		}
		if (ci_equal(longname, ln) || ci_equal(longname, ss)) {
			*e83 = e;
			*short_index = (u16)(idx - 1);
			return 0;
		}
	}
	return -1;
}

/* Overwrite the last component of near_path (at base) with the
 * 8.3 alias and return a far pointer to the rebuilt 8.3 path. */
static const char __far *rebuild_short(char *near_path, char *base,
                                       const u8 *name11) {
	name83_to_str(name11, base);
	return (const char __far *)MK_FP(get_cs(), (u16)near_path);
}

/* --- 716Ch: extended open/create ---
 * BX = access mode, CX = attributes, DX = action (1 open, 2
 * replace, 0x10 create), DS:SI = path. Returns AX = handle,
 * CX = action taken (1 opened, 2 created, 3 truncated). */
static void f71_open(iregs __far *r) {
	char             *near_path = s_path;
	char             *base;
	dirent83          e;
	u8                short11[11];
	u16               dir_cl, err, sidx;
	u16               access = r->bx;
	u16               attrib = r->cx;
	u16               action = r->dx;
	u8                drv;
	const char __far *path = (const char __far *)MK_FP(r->ds, r->si);
	err = lfn_split(path, &dir_cl, near_path, &base);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	drv = fat_cur_drive();
	if (lfn_lookup(drv, dir_cl, base, &e, &sidx) == 0) {
		/* exists: replace (truncate) or open */
		(void)rebuild_short(near_path, base, e.name);
		r->ds = get_cs();
		r->dx = (u16)near_path;
		if (action & 0x02) {
			r->cx = attrib;
			f3c_create(r); /* truncate */
			if (!(r->flags & FL_CF)) {
				r->cx = 3;
			}
		} else if (action & 0x01) {
			r->ax = (u16)(0x3D00 | (access & 0x7F));
			f3d_open(r);
			if (!(r->flags & FL_CF)) {
				r->cx = 1;
			}
		} else {
			int21_error(r, 0x50); /* exists but no open/replace allowed */
		}
		return;
	}
	if (!(action & 0x10)) {
		int21_error(r, ERR_FILE_NOT_FOUND);
		return;
	}
	if (fat_is_short_name(base, short11)) {
		r->ds = get_cs();
		r->dx = (u16)near_path;
		r->cx = attrib;
		f3c_create(r);
		if (!(r->flags & FL_CF)) {
			r->cx = 2;
		}
		return;
	}
	fat_make_shortname(dir_cl, base, short11);
	if (fat_alloc_lfn_entry(dir_cl, base, short11, &sidx) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	fmemset(&e, 0, sizeof(e));
	fmemcpy(e.name, short11, 11);
	e.attr = (u8)((attrib & 0x27) | ATTR_ARCHIVE);
	e.cluster = 0;
	e.size = 0;
	e.time = clock_dos_time();
	e.date = clock_dos_date();
	if (fat_dir_set(dir_cl, sidx, &e) != 0 || fat_commit() != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	(void)rebuild_short(near_path, base, short11);
	r->ds = get_cs();
	r->dx = (u16)near_path;
	r->ax = (u16)(0x3D00 | (access & 0x7F) | 0x02);
	f3d_open(r);
	if (!(r->flags & FL_CF)) {
		r->cx = 2;
	}
}

/* --- 7139h: make directory (DS:DX = path) --- */
static void f71_mkdir(iregs __far *r) {
	char             *near_path = s_path;
	char             *base;
	dirent83          e;
	u8                short11[11];
	u16               dir_cl, err, sidx, cl;
	const char __far *path = (const char __far *)MK_FP(r->ds, r->dx);
	err = lfn_split(path, &dir_cl, near_path, &base);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (lfn_lookup(fat_cur_drive(), dir_cl, base, &e, &sidx) == 0) {
		int21_error(r, ERR_ACCESS_DENIED); /* already exists */
		return;
	}
	if (fat_is_short_name(base, short11)) {
		r->ds = get_cs();
		r->dx = (u16)near_path;
		f39_mkdir(r);
		return;
	}
	fat_make_shortname(dir_cl, base, short11);
	if (fat_alloc_lfn_entry(dir_cl, base, short11, &sidx) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	cl = fat_alloc(0);
	if (cl == 0 || fat_zero_cluster(cl) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
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
	fmemcpy(e.name, short11, 11); /* parent entry at the 8.3 slot */
	e.attr = ATTR_DIR;
	e.cluster = cl;
	if (fat_dir_set(dir_cl, sidx, &e) != 0 || fat_commit() != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
	}
}

/* --- 713Ah: remove directory (DS:DX = path) --- */
static void f71_rmdir(iregs __far *r) {
	char             *near_path = s_path;
	char             *base;
	dirent83          e;
	u16               dir_cl, err, sidx;
	u8                drv;
	const char __far *path = (const char __far *)MK_FP(r->ds, r->dx);
	err = lfn_split(path, &dir_cl, near_path, &base);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	drv = fat_cur_drive();
	if (lfn_lookup(drv, dir_cl, base, &e, &sidx) != 0 ||
	    (e.attr & ATTR_DIR) == 0) {
		int21_error(r, ERR_PATH_NOT_FOUND);
		return;
	}
	(void)rebuild_short(near_path, base, e.name);
	r->ds = get_cs();
	r->dx = (u16)near_path;
	f3a_rmdir(r);
	if (!(r->flags & FL_CF)) {
		fat_select(drv);
		fat_delete_lfn_slots(dir_cl, sidx);
		fat_commit();
	}
}

/* --- 713Bh: change directory (DS:DX = path) --- */
static void f71_chdir(iregs __far *r) {
	char             *near_path = s_path;
	char             *base;
	dirent83          e;
	u16               dir_cl, err, sidx;
	const char __far *path = (const char __far *)MK_FP(r->ds, r->dx);
	err = lfn_split(path, &dir_cl, near_path, &base);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (lfn_lookup(fat_cur_drive(), dir_cl, base, &e, &sidx) != 0 ||
	    (e.attr & ATTR_DIR) == 0) {
		int21_error(r, ERR_PATH_NOT_FOUND);
		return;
	}
	(void)rebuild_short(near_path, base, e.name);
	if (fat_chdir((const char __far *)MK_FP(get_cs(), (u16)near_path)) != 0) {
		int21_error(r, ERR_PATH_NOT_FOUND);
	}
}

/* --- 7141h: delete file (DS:DX = path) --- */
static void f71_delete(iregs __far *r) {
	char             *near_path = s_path;
	char             *base;
	dirent83          e;
	u16               dir_cl, err, sidx;
	u8                drv;
	const char __far *path = (const char __far *)MK_FP(r->ds, r->dx);
	err = lfn_split(path, &dir_cl, near_path, &base);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	drv = fat_cur_drive();
	if (lfn_lookup(drv, dir_cl, base, &e, &sidx) != 0) {
		int21_error(r, ERR_FILE_NOT_FOUND);
		return;
	}
	if (e.attr & ATTR_DIR) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	(void)rebuild_short(near_path, base, e.name);
	r->ds = get_cs();
	r->dx = (u16)near_path;
	f41_unlink(r);
	if (!(r->flags & FL_CF)) {
		fat_select(drv);
		fat_delete_lfn_slots(dir_cl, sidx);
		fat_commit();
	}
}

/* --- 7143h: get/set attributes (BL = subfunction, DS:DX = path,
 * CX = attributes for set) --- */
static void f71_attr(iregs __far *r) {
	char             *near_path = s_path;
	char             *base;
	dirent83          e;
	u16               dir_cl, err, sidx;
	u8                sub = (u8)(r->bx & 0xFF);
	const char __far *path = (const char __far *)MK_FP(r->ds, r->dx);
	err = lfn_split(path, &dir_cl, near_path, &base);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (lfn_lookup(fat_cur_drive(), dir_cl, base, &e, &sidx) != 0) {
		int21_error(r, ERR_FILE_NOT_FOUND);
		return;
	}
	(void)rebuild_short(near_path, base, e.name);
	r->ds = get_cs();
	r->dx = (u16)near_path;
	r->ax = (u16)(0x4300 | sub);
	f43_attrib(r);
}

/* --- 7147h: get current directory (DL = drive, DS:SI = buf) ---
 * The cwd is stored as 8.3 text, returned verbatim. */
static void f71_getcwd(iregs __far *r) {
	f47_getcwd(r);
}

/* --- 7156h: rename / move (DS:DX = old, ES:DI = new) --- */
static void f71_rename(iregs __far *r) {
	char    *oldp = s_path, *newp = s_path2;
	char    *obase, *nbase;
	dirent83 oe, ne;
	u8       short11[11];
	u16      odir, ndir, err, osidx, nsidx;
	u8       drv;
	err = lfn_split((const char __far *)MK_FP(r->ds, r->dx), &odir, oldp,
	                &obase);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	drv = fat_cur_drive();
	if (lfn_lookup(drv, odir, obase, &oe, &osidx) != 0) {
		int21_error(r, ERR_FILE_NOT_FOUND);
		return;
	}
	err = lfn_split((const char __far *)MK_FP(r->es, r->di), &ndir, newp,
	                &nbase);
	if (err != 0) {
		int21_error(r, err);
		return;
	}
	if (lfn_lookup(fat_cur_drive(), ndir, nbase, &ne, &nsidx) == 0) {
		int21_error(r, ERR_ACCESS_DENIED); /* target exists */
		return;
	}
	fat_select(drv);
	if (fat_is_short_name(nbase, short11)) {
		if (fat_dir_alloc_slot(ndir, &nsidx) != 0) {
			int21_error(r, ERR_ACCESS_DENIED);
			return;
		}
	} else {
		fat_make_shortname(ndir, nbase, short11);
		if (fat_alloc_lfn_entry(ndir, nbase, short11, &nsidx) != 0) {
			int21_error(r, ERR_ACCESS_DENIED);
			return;
		}
	}
	ne = oe; /* keep cluster/size/attr/time, change name */
	fmemcpy(ne.name, short11, 11);
	if (fat_dir_set(ndir, nsidx, &ne) != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
		return;
	}
	fat_delete_lfn_slots(odir, osidx);
	if (fat_dir_entry(odir, osidx, &oe) == 0) {
		oe.name[0] = 0xE5; /* delete old 8.3 entry */
		fat_dir_set(odir, osidx, &oe);
	}
	if (fat_commit() != 0) {
		int21_error(r, ERR_ACCESS_DENIED);
	}
}

/* --- 71A0h: get volume info (DS:DX = root, ES:DI = name buf,
 * CX = buf size). Reports FAT with LFN support. --- */
static void f71_volinfo(iregs __far *r) {
	u8 __far *p = (u8 __far *)MK_FP(r->es, r->di);
	if (r->cx >= 4) {
		p[0] = 'F';
		p[1] = 'A';
		p[2] = 'T';
		p[3] = '\0';
	}
	r->bx = 0;   /* file system flags */
	r->cx = 255; /* max filename component length */
	r->dx = 260; /* max path length */
	r->ax = 0;
}

/* --- 71h: long file name API dispatcher (subfunction in AL) ---
 * DS/ES are saved and restored around every subfunction: the
 * write handlers temporarily repoint DS at the kernel segment to
 * delegate to the 8.3 handlers, and that change must not leak
 * back to the caller through the iregs frame. No 71xx function
 * returns data in a segment register, so restoring is safe. */
void f71_lfn(iregs __far *r) {
	u16 sds = r->ds;
	u16 ses = r->es;
	switch (r->ax & 0xFF) {
	case 0x39:
		f71_mkdir(r);
		break;
	case 0x3A:
		f71_rmdir(r);
		break;
	case 0x3B:
		f71_chdir(r);
		break;
	case 0x41:
		f71_delete(r);
		break;
	case 0x43:
		f71_attr(r);
		break;
	case 0x47:
		f71_getcwd(r);
		break;
	case 0x4E:
		f71_findfirst(r);
		break;
	case 0x4F:
		f71_findnext(r);
		break;
	case 0x56:
		f71_rename(r);
		break;
	case 0x6C:
		f71_open(r);
		break;
	case 0xA0:
		f71_volinfo(r);
		break;
	case 0xA1:
		f71_findclose(r);
		break;
	default:
		lfn_unsupported(r);
		break;
	}
	r->ds = sds;
	r->es = ses;
}
