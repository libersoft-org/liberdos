/* ============================================================
 * fcb.c - FCB (File Control Block) API
 *
 * The CP/M-era file functions 0Fh-29h, still used by old DOS
 * utilities and some games. Internally each open FCB borrows a
 * regular kernel file handle, stashed in the FCB's reserved
 * area together with a magic byte that marks the FCB as open.
 *
 * Extended FCBs (first byte FFh, 7-byte prefix with an attr
 * byte) are accepted everywhere; the attr is honoured in the
 * find functions.
 *
 * FCB layout (offsets from the drive byte):
 *   0x00 drive (0 = default, 1 = A:)      0x14 date
 *   0x01 name[8]   0x09 ext[3]            0x16 time
 *   0x0C cur_block                        0x18 reserved[8]
 *   0x0E rec_size                         0x20 cur_rec
 *   0x10 file_size (u32)                  0x21 random (u32)
 *
 * reserved[0] = kernel handle, reserved[1] = 0xC9 open magic,
 * reserved[2..3] = search index, reserved[4..5] = search dir,
 * reserved[6] = search drive (1 = A:).
 * ============================================================ */
#include "kernel.h"

#define FCB_MAGIC 0xC9

#define F_DRIVE   0x00
#define F_NAME    0x01
#define F_CURBLK  0x0C
#define F_RECSZ   0x0E
#define F_SIZE    0x10
#define F_DATE    0x14
#define F_TIME    0x16
#define F_HANDLE  0x18 /* reserved[0] */
#define F_OPENMAG 0x19 /* reserved[1] */
#define F_FINDIDX 0x1A /* reserved[2..3] */
#define F_FINDDIR 0x1C /* reserved[4..5] */
#define F_FINDDRV 0x1E /* reserved[6] */
#define F_CURREC  0x20
#define F_RANDOM  0x21

/* Resolve DS:DX to the drive byte of the FCB; extended FCBs
 * (leading FFh) skip the 7-byte prefix. *attr gets the search
 * attribute (0 for normal FCBs). */
static u8 __far *fcb_at(iregs __far *r, u8 *attr) {
	u8 __far *p = (u8 __far *)MK_FP(r->ds, r->dx);
	*attr = 0;
	if (p[0] == 0xFF) {
		*attr = p[6];
		return p + 7;
	}
	return p;
}

/* Select the volume named by an FCB drive byte (0 = default,
 * 1 = A:). Returns the 1-based drive, or 0 when invalid. */
static u8 fcb_select(u8 dbyte) {
	u8 d = dbyte == 0 ? fat_default_drive() : (u8)(dbyte - 1);
	if (fat_select(d) != 0) {
		return 0;
	}
	return (u8)(d + 1);
}

/* Build an ASCIIZ "X:NAME.EXT" path from the FCB drive and
 * name fields. Returns 0, or -1 for an invalid drive number. */
static int fcb_path(const u8 __far *fcb, char *out) {
	u16 n = 0;
	u16 i;
	u8  d = fcb[F_DRIVE];
	if (d != 0) {
		if (!fat_drive_present((u8)(d - 1))) {
			return -1;
		}
		out[n++] = (char)('A' + d - 1);
		out[n++] = ':';
	}
	for (i = 0; i < 8 && fcb[F_NAME + i] != ' '; i++) {
		out[n++] = (char)fcb[F_NAME + i];
	}
	if (fcb[F_NAME + 8] != ' ') {
		out[n++] = '.';
		for (i = 8; i < 11 && fcb[F_NAME + i] != ' '; i++) {
			out[n++] = (char)fcb[F_NAME + i];
		}
	}
	out[n] = '\0';
	return n == 0 ? -1 : 0;
}

static u16 fcb_word(const u8 __far *f, u16 off) {
	return (u16)(f[off] | ((u16)f[off + 1] << 8));
}

static void fcb_setword(u8 __far *f, u16 off, u16 v) {
	f[off] = (u8)v;
	f[off + 1] = (u8)(v >> 8);
}

static u32 fcb_dword(const u8 __far *f, u16 off) {
	return (u32)fcb_word(f, off) | ((u32)fcb_word(f, (u16)(off + 2)) << 16);
}

static void fcb_setdword(u8 __far *f, u16 off, u32 v) {
	fcb_setword(f, off, (u16)v);
	fcb_setword(f, (u16)(off + 2), (u16)(v >> 16));
}

/* Common tail of open/create: fill the FCB state fields. */
static void fcb_fill(u8 __far *f, int h) {
	u16 t, d;
	f[F_HANDLE] = (u8)h;
	f[F_OPENMAG] = FCB_MAGIC;
	fcb_setword(f, F_CURBLK, 0);
	fcb_setword(f, F_RECSZ, 128);
	fcb_setdword(f, F_SIZE, kfile_size(h));
	kfile_stamp(h, &t, &d);
	fcb_setword(f, F_TIME, t);
	fcb_setword(f, F_DATE, d);
	f[F_CURREC] = 0;
}

/* Open FCB -> kernel handle, or -1 when the FCB is not open. */
static int fcb_handle(const u8 __far *f) {
	if (f[F_OPENMAG] != FCB_MAGIC) {
		return -1;
	}
	return (int)f[F_HANDLE];
}

/* --- 0Fh: open --- */
void f0f_fcb_open(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	char      path[16];
	int       h;
	if (fcb_path(f, path) != 0 ||
	    (h = kfile_open((const char __far *)path)) < 0) {
		set_al(r, 0xFF);
		return;
	}
	fcb_fill(f, h);
	set_al(r, 0);
}

/* --- 16h: create (or truncate) --- */
void f16_fcb_create(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	char      path[16];
	int       h;
	if (fcb_path(f, path) != 0 ||
	    (h = kfile_create((const char __far *)path)) < 0) {
		set_al(r, 0xFF);
		return;
	}
	fcb_fill(f, h);
	set_al(r, 0);
}

/* --- 10h: close --- */
void f10_fcb_close(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	kfile_close(h);
	f[F_OPENMAG] = 0;
	set_al(r, 0);
}

/* --- record I/O common --- */

/* Transfer one record at "pos" between the file and the DTA.
 * Returns the DOS status code: 0 ok, 1 EOF/full, 3 partial. */
static u8 fcb_xfer(int h, u32 pos, u16 recsz, u8 writing) {
	u16 dseg, doff;
	u16 done;
	file_get_dta(&dseg, &doff);
	kfile_seek_set(h, pos);
	if (writing) {
		done = kfile_write(h, MK_FP(dseg, doff), recsz);
		return done == recsz ? 0 : 1; /* 1 = disk full */
	}
	done = kfile_read(h, MK_FP(dseg, doff), recsz);
	if (done == 0) {
		return 1; /* EOF, nothing read */
	}
	if (done < recsz) {
		fmemset(MK_FP(dseg, (u16)(doff + done)), 0, (u16)(recsz - done));
		return 3; /* partial, zero-padded */
	}
	return 0;
}

/* Sequential position = (cur_block * 128 + cur_rec) * rec_size. */
static u32 fcb_seq_pos(const u8 __far *f) {
	return ((u32)fcb_word(f, F_CURBLK) * 128 + f[F_CURREC]) *
	       fcb_word(f, F_RECSZ);
}

static void fcb_seq_advance(u8 __far *f) {
	u8 rec = (u8)(f[F_CURREC] + 1);
	if (rec >= 128) {
		rec = 0;
		fcb_setword(f, F_CURBLK, (u16)(fcb_word(f, F_CURBLK) + 1));
	}
	f[F_CURREC] = rec;
}

static void fcb_sync_size(u8 __far *f, int h) {
	fcb_setdword(f, F_SIZE, kfile_size(h));
}

/* --- 14h/15h: sequential read/write --- */
void f14_fcb_read(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	u8        st;
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	st = fcb_xfer(h, fcb_seq_pos(f), fcb_word(f, F_RECSZ), 0);
	if (st != 1) {
		fcb_seq_advance(f);
	}
	set_al(r, st);
}

void f15_fcb_write(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	u8        st;
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	st = fcb_xfer(h, fcb_seq_pos(f), fcb_word(f, F_RECSZ), 1);
	if (st == 0) {
		fcb_seq_advance(f);
		fcb_sync_size(f, h);
	}
	set_al(r, st);
}

/* --- 21h/22h: random read/write (random field not advanced) --- */
void f21_fcb_randread(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	u32       rec;
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	rec = fcb_dword(f, F_RANDOM);
	fcb_setword(f, F_CURBLK, (u16)(rec / 128));
	f[F_CURREC] = (u8)(rec % 128);
	set_al(r, fcb_xfer(h, rec * fcb_word(f, F_RECSZ), fcb_word(f, F_RECSZ), 0));
}

void f22_fcb_randwrite(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	u32       rec;
	u8        st;
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	rec = fcb_dword(f, F_RANDOM);
	fcb_setword(f, F_CURBLK, (u16)(rec / 128));
	f[F_CURREC] = (u8)(rec % 128);
	st = fcb_xfer(h, rec * fcb_word(f, F_RECSZ), fcb_word(f, F_RECSZ), 1);
	if (st == 0) {
		fcb_sync_size(f, h);
	}
	set_al(r, st);
}

/* --- 27h/28h: random block read/write, CX = record count --- */
void f27_fcb_blockread(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	u16       recsz, want, got = 0;
	u32       rec;
	u16       dseg, doff;
	u8        st = 0;
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	recsz = fcb_word(f, F_RECSZ);
	want = r->cx;
	rec = fcb_dword(f, F_RANDOM);
	file_get_dta(&dseg, &doff);
	kfile_seek_set(h, rec * recsz);
	while (got < want) {
		u16 done = kfile_read(h, MK_FP(dseg, doff), recsz);
		if (done == 0) {
			st = 1;
			break;
		}
		if (done < recsz) {
			fmemset(MK_FP(dseg, (u16)(doff + done)), 0, (u16)(recsz - done));
			got++;
			st = 3;
			break;
		}
		got++;
		doff += recsz;
	}
	rec += got;
	fcb_setdword(f, F_RANDOM, rec);
	fcb_setword(f, F_CURBLK, (u16)(rec / 128));
	f[F_CURREC] = (u8)(rec % 128);
	r->cx = got;
	set_al(r, st);
}

void f28_fcb_blockwrite(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	int       h = fcb_handle(f);
	u16       recsz, want, got = 0;
	u32       rec;
	u16       dseg, doff;
	u8        st = 0;
	if (h < 0) {
		set_al(r, 0xFF);
		return;
	}
	recsz = fcb_word(f, F_RECSZ);
	want = r->cx;
	rec = fcb_dword(f, F_RANDOM);
	if (want == 0) { /* CX=0: not supported, no-op */
		set_al(r, 0);
		return;
	}
	file_get_dta(&dseg, &doff);
	kfile_seek_set(h, rec * recsz);
	while (got < want) {
		if (kfile_write(h, MK_FP(dseg, doff), recsz) != recsz) {
			st = 1; /* disk full */
			break;
		}
		got++;
		doff += recsz;
	}
	rec += got;
	fcb_setdword(f, F_RANDOM, rec);
	fcb_setword(f, F_CURBLK, (u16)(rec / 128));
	f[F_CURREC] = (u8)(rec % 128);
	fcb_sync_size(f, h);
	r->cx = got;
	set_al(r, st);
}

/* --- 11h/12h: find first/next ---
 * Result in the DTA: drive byte + the 32-byte directory entry
 * (an "unopened FCB"). Search state lives in the search FCB's
 * reserved area, like DOS does it. */

static void fcb_find(iregs __far *r, u8 first) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	u8        pat[11];
	u16       idx;
	clus_t    dir_cl;
	dirent83  e;
	u16       dseg, doff;
	u16       i;

	if (first) {
		u8 d1 = fcb_select(f[F_DRIVE]);
		if (d1 == 0) {
			set_al(r, 0xFF);
			return;
		}
		f[F_FINDDRV] = d1;
		for (i = 0; i < 11; i++) { /* copy + uppercase the pattern */
			u8 c = f[F_NAME + i];
			if (c == '*') { /* normalised by parse, but allow */
				c = '?';
			}
			if (c >= 'a' && c <= 'z') {
				c = (u8)(c - 32);
			}
			pat[i] = c;
		}
		fmemcpy((u8 __far *)f + F_NAME, pat, 11);
		idx = 0;
		dir_cl = fat_cwd_cluster();
		fcb_setword(f, F_FINDDIR, dir_cl);
	} else {
		if (fcb_select(f[F_FINDDRV]) == 0) {
			set_al(r, 0xFF);
			return;
		}
		idx = fcb_word(f, F_FINDIDX);
		dir_cl = fcb_word(f, F_FINDDIR);
		fmemcpy(pat, (const u8 __far *)f + F_NAME, 11);
	}

	while (fat_dir_entry(dir_cl, idx, &e) == 0) {
		if (e.name[0] == 0x00) {
			break;
		}
		idx++;
		if (e.name[0] == 0xE5 || e.attr == 0x0F ||
		    (e.attr & ATTR_VOLUME) != 0) {
			continue;
		}
		if ((e.attr & ATTR_DIR) != 0 && (attr & ATTR_DIR) == 0) {
			continue;
		}
		if ((e.attr & (ATTR_HIDDEN | ATTR_SYSTEM)) != 0 &&
		    (attr & (ATTR_HIDDEN | ATTR_SYSTEM)) == 0) {
			continue;
		}
		if (!fat_match11(pat, e.name)) {
			continue;
		}
		fcb_setword(f, F_FINDIDX, idx);
		file_get_dta(&dseg, &doff);
		pokeb(dseg, doff, f[F_FINDDRV]); /* drive byte, 1-based */
		fmemcpy(MK_FP(dseg, (u16)(doff + 1)), &e, 32);
		set_al(r, 0);
		return;
	}
	fcb_setword(f, F_FINDIDX, idx);
	set_al(r, 0xFF);
}

void f11_fcb_findfirst(iregs __far *r) {
	fcb_find(r, 1);
}

void f12_fcb_findnext(iregs __far *r) {
	fcb_find(r, 0);
}

/* --- 13h: delete (wildcards allowed) --- */
void f13_fcb_delete(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	u8        pat[11];
	u16       idx = 0;
	clus_t    dir_cl;
	dirent83  e;
	u8        any = 0;
	u16       i;
	if (fcb_select(f[F_DRIVE]) == 0) {
		set_al(r, 0xFF);
		return;
	}
	for (i = 0; i < 11; i++) {
		u8 c = f[F_NAME + i];
		if (c == '*') {
			c = '?';
		}
		if (c >= 'a' && c <= 'z') {
			c = (u8)(c - 32);
		}
		pat[i] = c;
	}
	dir_cl = fat_cwd_cluster();
	while (fat_dir_entry(dir_cl, idx, &e) == 0) {
		if (e.name[0] == 0x00) {
			break;
		}
		if (e.name[0] != 0xE5 && e.attr != 0x0F &&
		    (e.attr & (ATTR_VOLUME | ATTR_DIR | ATTR_READONLY)) == 0 &&
		    fat_match11(pat, e.name)) {
			if (dirent_cluster(&e) != 0) {
				fat_free_chain(dirent_cluster(&e));
			}
			e.name[0] = 0xE5;
			fat_dir_set(dir_cl, idx, &e);
			any = 1;
		}
		idx++;
	}
	fat_commit();
	set_al(r, any ? 0 : 0xFF);
}

/* --- 17h: rename. New name at FCB offset +11h (drive byte +
 * 11-char pattern); '?' keeps the old character. --- */
void f17_fcb_rename(iregs __far *r) {
	u8        attr;
	u8 __far *f = fcb_at(r, &attr);
	u8        pat[11], newp[11];
	u16       idx = 0;
	clus_t    dir_cl;
	dirent83  e, probe;
	u8        any = 0;
	u16       i;
	if (fcb_select(f[F_DRIVE]) == 0) {
		set_al(r, 0xFF);
		return;
	}
	for (i = 0; i < 11; i++) {
		u8 c = f[F_NAME + i];
		if (c == '*') {
			c = '?';
		}
		if (c >= 'a' && c <= 'z') {
			c = (u8)(c - 32);
		}
		pat[i] = c;
		c = f[0x11 + 1 + i]; /* second FCB name field */
		if (c == '*') {
			c = '?';
		}
		if (c >= 'a' && c <= 'z') {
			c = (u8)(c - 32);
		}
		newp[i] = c;
	}
	dir_cl = fat_cwd_cluster();
	while (fat_dir_entry(dir_cl, idx, &e) == 0) {
		if (e.name[0] == 0x00) {
			break;
		}
		if (e.name[0] != 0xE5 && e.attr != 0x0F &&
		    (e.attr & (ATTR_VOLUME | ATTR_DIR)) == 0 &&
		    fat_match11(pat, e.name)) {
			u8 dst[11];
			for (i = 0; i < 11; i++) {
				dst[i] = newp[i] == '?' ? e.name[i] : newp[i];
			}
			if (fat_dir_search(dir_cl, dst, &probe) != 0) {
				fmemcpy(e.name, dst, 11);
				fat_dir_set(dir_cl, idx, &e);
				any = 1;
			}
		}
		idx++;
	}
	fat_commit();
	set_al(r, any ? 0 : 0xFF);
}

/* --- 29h: parse filename DS:SI into FCB ES:DI ---
 * AL bits: 1 = skip leading separators, 2/4/8 = leave drive/
 * name/ext unchanged when missing. Returns AL = 1 when the
 * name contains wildcards, 0 when not, FFh on invalid drive;
 * DS:SI advanced past the parsed text. */
void f29_fcb_parse(iregs __far *r) {
	const u8 __far *s = (const u8 __far *)MK_FP(r->ds, r->si);
	u8 __far       *f = (u8 __far *)MK_FP(r->es, r->di);
	u8              opts = (u8)(r->ax & 0xFF);
	u8              wild = 0;
	u8              ret = 0;
	u16             i;

	if (opts & 0x01) { /* skip blanks and separators */
		while (*s == ' ' || *s == '\t' || *s == ',' || *s == ';' || *s == '=') {
			s++;
		}
	} else {
		while (*s == ' ' || *s == '\t') {
			s++;
		}
	}

	if (s[0] != '\0' && s[1] == ':') { /* drive specified */
		u8 d = s[0];
		if (d >= 'a' && d <= 'z') {
			d = (u8)(d - 32);
		}
		if (d < 'A' || d > 'Z') {
			ret = 0xFF;
		} else {
			f[F_DRIVE] = (u8)(d - 'A' + 1);
			if (!fat_drive_present((u8)(d - 'A'))) {
				ret = 0xFF; /* parse anyway, flag invalid */
			}
		}
		s += 2;
	} else if ((opts & 0x02) == 0) {
		f[F_DRIVE] = 0;
	}

	if (*s == '.' || *s == '\0' || *s == ' ') {
		if ((opts & 0x04) == 0) { /* no name present */
			fmemset((u8 __far *)f + F_NAME, ' ', 8);
		}
	} else {
		i = 0;
		for (;;) {
			u8 c = *s;
			if (c == '\0' || c == '.' || c == ' ' || c == '\t' || c == ',' ||
			    c == ';' || c == '=' || c == '\\' || c == '/' || c == ':') {
				break;
			}
			if (c >= 'a' && c <= 'z') {
				c = (u8)(c - 32);
			}
			if (c == '*') {
				while (i < 8) {
					f[F_NAME + i] = '?';
					i++;
				}
				wild = 1;
				s++;
				continue;
			}
			if (c == '?') {
				wild = 1;
			}
			if (i < 8) {
				f[F_NAME + i] = c;
				i++;
			}
			s++;
		}
		while (i < 8) {
			f[F_NAME + i] = ' ';
			i++;
		}
	}

	if (*s == '.') {
		s++;
		i = 0;
		for (;;) {
			u8 c = *s;
			if (c == '\0' || c == '.' || c == ' ' || c == '\t' || c == ',' ||
			    c == ';' || c == '=' || c == '\\' || c == '/' || c == ':') {
				break;
			}
			if (c >= 'a' && c <= 'z') {
				c = (u8)(c - 32);
			}
			if (c == '*') {
				while (i < 3) {
					f[F_NAME + 8 + i] = '?';
					i++;
				}
				wild = 1;
				s++;
				continue;
			}
			if (c == '?') {
				wild = 1;
			}
			if (i < 3) {
				f[F_NAME + 8 + i] = c;
				i++;
			}
			s++;
		}
		while (i < 3) {
			f[F_NAME + 8 + i] = ' ';
			i++;
		}
	} else if ((opts & 0x08) == 0) {
		fmemset((u8 __far *)f + F_NAME + 8, ' ', 3);
	}

	fcb_setword(f, F_CURBLK, 0);
	fcb_setword(f, F_RECSZ, 0);
	if (ret != 0xFF && wild) {
		ret = 1;
	}
	set_al(r, ret);
	r->si = FP_OFF(s);
}
