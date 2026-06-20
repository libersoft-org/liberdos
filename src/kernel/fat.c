/* ============================================================
 * fat.c - FAT12/FAT16 filesystem driver, multi-volume
 *
 * The driver handles several volumes: A: is the
 * boot floppy (FAT12), C: the first FAT partition of the BIOS
 * hard disk 80h (usually FAT16). All entry points that take a
 * path select the volume from the "X:" prefix (or the default
 * drive); cluster-level helpers operate on the *selected*
 * volume, so handle-based callers (file.c) re-select with
 * fat_select() before touching cluster chains.
 *
 * FAT12 keeps the whole FAT cached in RAM (max 12 sectors;
 * only one FAT12 volume is supported - the floppy). FAT16 FATs
 * are too big for that, so they go through a one-sector
 * write-back cache. Directory access uses a single shared
 * sector buffer (disk_buf) keyed by (drive, LBA).
 *
 * Cluster numbering convention: cluster 0 = "the root
 * directory" (matches ".." entries pointing at root), and
 * fat_next() normalizes every end-of-chain/bad marker to
 * 0xFFFF regardless of FAT width. fat_set(cl, 0xFFFF) writes
 * the proper end marker for the volume.
 * ============================================================ */
#include "kernel.h"

/* Shared scratch sector buffer (also used by file.c reads). */
u8         disk_buf[512];
static u32 buf_lba = 0xFFFFFFFFUL;
static u8  buf_drv = 0xFF;

typedef struct volume {
	u8   present;
	u8   bios_drive;
	u8   fat16;
	u8   secs_per_clus;
	u8   num_fats;
	u16  root_entries;
	u16  secs_per_fat;
	u32  base_lba;   /* volume start (partition or 0) */
	u32  total_secs; /* volume size in sectors */
	u32  fat_lba;    /* absolute LBA of the first FAT */
	u32  root_lba;
	u32  data_lba;
	u16  max_cluster; /* highest valid cluster number */
	u16  cwd_cluster; /* 0 = root */
	char cwd[64];     /* canonical, no leading backslash */
} volume;

#define NUM_DRIVES 3 /* A:, B: (unused), C: */

static volume  vols[NUM_DRIVES];
static volume *v = &vols[0];  /* selected volume */
static u8      sel_drive = 0; /* index of *v */
static u8      cur_drive = 0; /* DOS default drive */

/* FAT12: whole-FAT cache (the boot floppy only). */
static u8      fatcache[12 * 512];
static volume *v12 = 0; /* volume owning fatcache */
static u8      fat12_dirty = 0;

/* FAT16: one-sector write-back cache. */
static u8      f16buf[512];
static volume *f16vol = 0;
static u32     f16lba = 0xFFFFFFFFUL; /* absolute LBA in FAT 0 */
static u8      f16dirty = 0;

/* --- volume selection --- */

int fat_select(u8 drv) {
	if (drv >= NUM_DRIVES || !vols[drv].present) {
		return -1;
	}
	v = &vols[drv];
	sel_drive = drv;
	return 0;
}

u8 fat_cur_drive(void) {
	return sel_drive;
}

u8 fat_default_drive(void) {
	return cur_drive;
}

int fat_set_default(u8 drv) {
	if (drv >= NUM_DRIVES || !vols[drv].present) {
		return -1;
	}
	cur_drive = drv;
	return 0;
}

int fat_drive_present(u8 drv) {
	return drv < NUM_DRIVES && vols[drv].present;
}

u8 fat_drive_count(void) {
	return NUM_DRIVES;
}

/* Per-drive info for INT 21h 1Bh/1Ch/36h. 0 = OK. */
int fat_drive_info(u8 drv, u16 *spc, u16 *clusters, u8 *media) {
	if (drv >= NUM_DRIVES || !vols[drv].present) {
		return -1;
	}
	*spc = vols[drv].secs_per_clus;
	*clusters = (u16)(vols[drv].max_cluster - 1);
	*media = vols[drv].fat16 ? 0xF8 : 0xF0;
	return 0;
}

/* --- shared sector buffer --- */

/* Load LBA (on the selected volume's disk) into disk_buf. */
int fat_load_sector(u32 lba) {
	if (lba == buf_lba && v->bios_drive == buf_drv) {
		return 0;
	}
	if (disk_read(v->bios_drive, lba, disk_buf) != 0) {
		buf_lba = 0xFFFFFFFFUL;
		return -1;
	}
	buf_lba = lba;
	buf_drv = v->bios_drive;
	return 0;
}

/* Write disk_buf to LBA; the buffer then caches that sector. */
int fat_store_buf(u32 lba) {
	if (disk_write(v->bios_drive, lba, disk_buf) != 0) {
		buf_lba = 0xFFFFFFFFUL;
		return -1;
	}
	buf_lba = lba;
	buf_drv = v->bios_drive;
	return 0;
}

/* Write a full sector from an external (user) buffer, keeping
 * the disk_buf cache coherent. */
int fat_write_ext(u32 lba, const void __far *buf) {
	if (disk_write(v->bios_drive, lba, buf) != 0) {
		return -1;
	}
	if (lba == buf_lba && v->bios_drive == buf_drv) {
		buf_lba = 0xFFFFFFFFUL;
	}
	return 0;
}

/* --- FAT16 sector cache --- */

static int f16_flush(void) {
	u16 f;
	if (!f16dirty || f16vol == 0) {
		return 0;
	}
	for (f = 0; f < f16vol->num_fats; f++) {
		if (disk_write(f16vol->bios_drive,
		               f16lba + (u32)f * f16vol->secs_per_fat, f16buf) != 0) {
			return -1;
		}
	}
	f16dirty = 0;
	return 0;
}

static int f16_load(u32 lba) {
	if (f16vol == v && f16lba == lba) {
		return 0;
	}
	if (f16_flush() != 0) {
		return -1;
	}
	if (disk_read(v->bios_drive, lba, f16buf) != 0) {
		f16vol = 0;
		return -1;
	}
	f16vol = v;
	f16lba = lba;
	return 0;
}

/* --- FAT entry access (on the selected volume) --- */

/* Next cluster after cl; end-of-chain/bad normalized to 0xFFFF. */
u16 fat_next(u16 cl) {
	if (v->fat16) {
		u16 val;
		if (f16_load(v->fat_lba + (cl >> 8)) != 0) {
			return 0xFFFF;
		}
		val = *(u16 *)(f16buf + ((u16)(cl & 0xFF) << 1));
		return val >= 0xFFF7 ? 0xFFFF : val;
	} else {
		u16 off = (u16)(cl + (cl >> 1));
		u16 val = *(u16 *)(fatcache + off);
		val = (cl & 1) ? (u16)(val >> 4) : (u16)(val & 0x0FFF);
		return val >= 0xFF7 ? 0xFFFF : val;
	}
}

/* Set the FAT entry of cl. val = 0xFFFF writes end-of-chain. */
void fat_set(u16 cl, u16 val) {
	if (v->fat16) {
		if (f16_load(v->fat_lba + (cl >> 8)) != 0) {
			return;
		}
		*(u16 *)(f16buf + ((u16)(cl & 0xFF) << 1)) = val;
		f16dirty = 1;
	} else {
		u16 off = (u16)(cl + (cl >> 1));
		u16 cur = *(u16 *)(fatcache + off);
		if (cl & 1) {
			cur = (u16)((cur & 0x000F) | (val << 4));
		} else {
			cur = (u16)((cur & 0xF000) | (val & 0x0FFF));
		}
		*(u16 *)(fatcache + off) = cur;
		fat12_dirty = 1;
	}
}

/* Allocate a free cluster, mark it end-of-chain and link it
 * after prev (if prev != 0). Returns 0 when the disk is full. */
u16 fat_alloc(u16 prev) {
	u16 cl;
	for (cl = 2; cl <= v->max_cluster; cl++) {
		if (fat_next(cl) == 0) {
			fat_set(cl, 0xFFFF);
			if (prev != 0) {
				fat_set(prev, cl);
			}
			return cl;
		}
	}
	return 0;
}

/* Free a whole cluster chain starting at cl. */
void fat_free_chain(u16 cl) {
	while (cl >= 2 && cl <= v->max_cluster) {
		u16 nx = fat_next(cl);
		fat_set(cl, 0);
		cl = nx;
	}
}

/* Flush both FAT caches to disk. 0 = OK. */
int fat_commit(void) {
	if (f16_flush() != 0) {
		return -1;
	}
	if (fat12_dirty && v12 != 0) {
		u16 f, i;
		for (f = 0; f < v12->num_fats; f++) {
			u32 base = v12->fat_lba + (u32)f * v12->secs_per_fat;
			for (i = 0; i < v12->secs_per_fat; i++) {
				if (disk_write(v12->bios_drive, base + i, fatcache + i * 512) !=
				    0) {
					return -1;
				}
			}
		}
		fat12_dirty = 0;
	}
	return 0;
}

u16 fat_cluster_bytes(void) {
	return (u16)(v->secs_per_clus * 512u);
}

u32 fat_cluster_lba(u16 cl) {
	return v->data_lba + (u32)(cl - 2) * v->secs_per_clus;
}

u16 fat_max_cluster(void) {
	return v->max_cluster;
}

u8 fat_secs_per_clus(void) {
	return v->secs_per_clus;
}

/* Zero every sector of a data cluster on disk. 0 = OK. */
int fat_zero_cluster(u16 cl) {
	u16 s;
	fmemset(disk_buf, 0, 512);
	buf_lba = 0xFFFFFFFFUL;
	for (s = 0; s < v->secs_per_clus; s++) {
		if (disk_write(v->bios_drive, fat_cluster_lba(cl) + s, disk_buf) != 0) {
			return -1;
		}
	}
	return 0;
}

/* --- mounting --- */

/* Parse the BPB at part_lba and fill vols[drv]. 0 = OK. */
static int mount_volume(u8 drv, u8 bios_drive, u32 part_lba) {
	volume *m = &vols[drv];
	u16     reserved, total16, i;
	u32     total, data_rel, clusters;
	u16     root_secs;

	m->present = 0;
	m->bios_drive = bios_drive;
	if (disk_read(bios_drive, part_lba, disk_buf) != 0) {
		return -1;
	}
	buf_lba = 0xFFFFFFFFUL; /* BPB parse trashes the buffer */
	m->secs_per_clus = disk_buf[13];
	reserved = *(u16 *)(disk_buf + 14);
	m->num_fats = disk_buf[16];
	m->root_entries = *(u16 *)(disk_buf + 17);
	total16 = *(u16 *)(disk_buf + 19);
	m->secs_per_fat = *(u16 *)(disk_buf + 22);
	total = total16 != 0 ? (u32)total16 : *(u32 *)(disk_buf + 32);
	if (bios_drive < 0x80) {
		disk_set_geometry(*(u16 *)(disk_buf + 24), *(u16 *)(disk_buf + 26));
	}
	if (m->secs_per_clus == 0 || m->secs_per_fat == 0 || m->num_fats == 0 ||
	    total == 0) {
		return -1;
	}
	root_secs = (u16)(((u32)m->root_entries * 32 + 511) / 512);
	m->base_lba = part_lba;
	m->total_secs = total;
	m->fat_lba = part_lba + reserved;
	m->root_lba = m->fat_lba + (u32)m->num_fats * m->secs_per_fat;
	m->data_lba = m->root_lba + root_secs;
	data_rel = (u32)reserved + (u32)m->num_fats * m->secs_per_fat + root_secs;
	clusters = (total - data_rel) / m->secs_per_clus;
	m->max_cluster = (u16)(clusters + 1);
	m->fat16 = clusters >= 4085;
	m->cwd_cluster = 0;
	m->cwd[0] = '\0';
	if (!m->fat16) {
		if (v12 != 0 || m->secs_per_fat > 12) {
			return -1; /* one cached FAT12 volume only */
		}
		for (i = 0; i < m->secs_per_fat; i++) {
			if (disk_read(bios_drive, m->fat_lba + i, fatcache + i * 512) !=
			    0) {
				return -1;
			}
		}
		v12 = m;
	}
	m->present = 1;
	return 0;
}

/* Mount the boot floppy as A:. 0 = OK. */
int fat_mount_floppy(u8 bios_drive) {
	if (mount_volume(0, bios_drive, 0) != 0) {
		return -1;
	}
	fat_select(0);
	return 0;
}

/* Probe BIOS disk 80h, find the first FAT partition in the MBR
 * and mount it as C:. Returns the volume size in MB, -1 = none. */
int fat_mount_hdd(void) {
	u16 i;
	u32 part_lba = 0;
	if (disk_read(0x80, 0, disk_buf) != 0) {
		return -1;
	}
	buf_lba = 0xFFFFFFFFUL;
	if (*(u16 *)(disk_buf + 510) != 0xAA55) {
		return -1;
	}
	for (i = 0; i < 4; i++) {
		const u8 *e = disk_buf + 0x1BE + i * 16;
		u8        type = e[4];
		if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0E) {
			part_lba = *(const u32 *)(e + 8);
			break;
		}
	}
	if (part_lba == 0) {
		return -1;
	}
	if (mount_volume(2, 0x80, part_lba) != 0) {
		return -1;
	}
	{
		volume *m = &vols[2];
		u32     kb = ((u32)(m->max_cluster - 1) * m->secs_per_clus) / 2;
		return (int)(kb / 1024);
	}
}

/* Compute the LBA holding directory entry #index. 0 = OK,
 * -1 = index past the end of the directory. */
static int fat_dir_lba(u16 dir_cluster, u16 index, u32 *lba) {
	u16 sec_idx = (u16)(index >> 4); /* 16 entries / sector */
	if (dir_cluster == 0) {
		if (index >= v->root_entries) {
			return -1;
		}
		*lba = v->root_lba + sec_idx;
	} else {
		u16 cl = dir_cluster;
		u16 skip = sec_idx / v->secs_per_clus;
		while (skip != 0) {
			cl = fat_next(cl);
			if (cl == 0xFFFF) {
				return -1;
			}
			skip--;
		}
		*lba = fat_cluster_lba(cl) + (sec_idx % v->secs_per_clus);
	}
	return 0;
}

/* Fetch directory entry #index of the directory at dir_cluster
 * (0 = root). 0 = OK, -1 = index past the end of the directory. */
int fat_dir_entry(u16 dir_cluster, u16 index, dirent83 *out) {
	u32 lba;
	u16 byte_off = (u16)((index & 15) * 32);
	if (fat_dir_lba(dir_cluster, index, &lba) != 0) {
		return -1;
	}
	if (fat_load_sector(lba) != 0) {
		return -1;
	}
	fmemcpy(out, disk_buf + byte_off, 32);
	return 0;
}

/* Write directory entry #index back to disk. 0 = OK. */
int fat_dir_set(u16 dir_cluster, u16 index, const dirent83 *e) {
	u32 lba;
	u16 byte_off = (u16)((index & 15) * 32);
	if (fat_dir_lba(dir_cluster, index, &lba) != 0) {
		return -1;
	}
	if (fat_load_sector(lba) != 0) {
		return -1;
	}
	fmemcpy(disk_buf + byte_off, e, 32);
	return fat_store_buf(lba);
}

/* Append a zeroed cluster to the directory at dir_cluster
 * (must not be root). 0 = OK, -1 = disk full / I/O error. */
static int fat_dir_extend(u16 dir_cluster) {
	u16 last = dir_cluster;
	u16 nx;
	u16 fresh;
	while ((nx = fat_next(last)) >= 2 && nx != 0xFFFF) {
		last = nx;
	}
	fresh = fat_alloc(last);
	if (fresh == 0) {
		return -1;
	}
	return fat_zero_cluster(fresh);
}

/* Find a free directory slot (deleted or end-of-dir entry),
 * growing subdirectories when needed. 0 = OK, -1 = full. */
int fat_dir_alloc_slot(u16 dir_cluster, u16 *out_index) {
	u16      i = 0;
	dirent83 e;
	for (;;) {
		if (fat_dir_entry(dir_cluster, i, &e) != 0) {
			if (dir_cluster == 0) {
				return -1; /* root directory is full */
			}
			if (fat_dir_extend(dir_cluster) != 0) {
				return -1;
			}
			continue; /* entry i now exists, zeroed */
		}
		if (e.name[0] == 0x00 || e.name[0] == 0xE5) {
			*out_index = i;
			return 0;
		}
		i++;
	}
}

/* 11-char name match; '?' in pat matches any character. */
int fat_match11(const u8 *pat, const u8 *name) {
	u16 i;
	for (i = 0; i < 11; i++) {
		if (pat[i] != '?' && pat[i] != name[i]) {
			return 0;
		}
	}
	return 1;
}

/* Find name11 in the directory at dir_cluster. Skips deleted,
 * LFN and volume-label entries. 0 = found, -1 = not found.
 * The entry index is stored in *out_index for write-backs. */
int fat_dir_search_i(u16 dir_cluster, const u8 *name11, dirent83 *out,
                     u16 *out_index) {
	u16      i = 0;
	dirent83 e;
	while (fat_dir_entry(dir_cluster, i, &e) == 0) {
		if (e.name[0] == 0x00) {
			break; /* end-of-directory marker */
		}
		if (e.name[0] != 0xE5 && e.attr != 0x0F &&
		    (e.attr & ATTR_VOLUME) == 0 && fat_match11(name11, e.name)) {
			*out = e;
			*out_index = i;
			return 0;
		}
		i++;
	}
	return -1;
}

int fat_dir_search(u16 dir_cluster, const u8 *name11, dirent83 *out) {
	u16 idx;
	return fat_dir_search_i(dir_cluster, name11, out, &idx);
}

/* ============================================================
 * VFAT long-name primitives (DOS 7 / Windows 95 LFN support).
 * A long name is stored as a run of 0x0F "slot" entries that
 * physically precede the 8.3 entry, in reverse order (the slot
 * with bit 0x40 set comes first on disk and holds the last
 * name fragment). Each slot carries 13 UTF-16 units; we keep
 * only the low byte of each (OEM/ASCII).
 * ============================================================ */

/* Byte offsets of the 13 UTF-16 units inside a 32-byte slot
 * (name1[5] @1, name2[6] @14, name3[2] @28). */
static const u8 lfn_unit_off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

/* Checksum of an 8.3 name[11] tying long-name slots to their
 * short entry: rotate the running sum right, then add a byte. */
u8 fat_lfn_checksum(const u8 *name11) {
	u8  sum = 0;
	u16 i;
	for (i = 0; i < 11; i++) {
		sum = (u8)(((sum & 1) ? 0x80 : 0x00) + (sum >> 1) + name11[i]);
	}
	return sum;
}

/* Read one logical directory entry of dir_cluster starting at
 * *index: gather any preceding long-name slots, then the 8.3
 * entry. Fills *out83, writes the assembled long name (low byte
 * of each unit, ASCIZ) into longname (capacity lmax, always
 * terminated; empty when the entry has no valid long name) and
 * advances *index past the 8.3 entry. Deleted and orphaned
 * slots are skipped. 1 = entry, 0 = end of directory, -1 = I/O
 * error (folded into 0 here: callers stop on non-positive). */
int fat_dir_next_lfn(u16 dir_cluster, u16 *index, dirent83 *out83,
                     char *longname, u16 lmax) {
	static u8 buf[260];     /* assembled name, by ordinal slot */
	u16       i = *index;
	u16       len = 0;      /* assembled length so far */
	u8        have = 0;     /* a valid slot run is in progress */
	u8        sum = 0;      /* checksum the run claims */
	dirent83  e;
	longname[0] = '\0';
	for (;;) {
		const u8 *raw = (const u8 *)&e;
		if (fat_dir_entry(dir_cluster, i, &e) != 0) {
			*index = i;
			return 0;
		}
		if (raw[0] == 0x00) { /* end-of-directory marker */
			*index = i;
			return 0;
		}
		if (raw[0] == 0xE5) { /* deleted: drop any pending run */
			have = 0;
			len = 0;
			i++;
			continue;
		}
		if (e.attr == ATTR_LFN) {
			u8  seq = raw[0];
			u8  ord = (u8)(seq & 0x1F);
			u16 base;
			u8  k;
			if (seq & 0x40) { /* last slot (first on disk): restart */
				have = 1;
				len = 0;
				sum = raw[13];
			}
			if (have && raw[13] == sum && ord >= 1 &&
			    (u16)(ord * 13) <= sizeof(buf)) {
				base = (u16)((ord - 1) * 13);
				for (k = 0; k < 13; k++) {
					u16 off = lfn_unit_off[k];
					u16 unit = (u16)(raw[off] | (raw[off + 1] << 8));
					if (unit == 0x0000 || unit == 0xFFFF) {
						break;
					}
					buf[base + k] = (u8)unit;
					if ((u16)(base + k + 1) > len) {
						len = (u16)(base + k + 1);
					}
				}
			} else {
				have = 0; /* orphaned / mismatched slot */
			}
			i++;
			continue;
		}
		/* regular 8.3 entry: emit it, with the long name if the
		 * run is complete and its checksum matches this entry */
		*out83 = e;
		if (have && len > 0 && fat_lfn_checksum(e.name) == sum) {
			u16 n = len < (u16)(lmax - 1) ? len : (u16)(lmax - 1);
			u16 j;
			for (j = 0; j < n; j++) {
				longname[j] = (char)buf[j];
			}
			longname[n] = '\0';
		}
		*index = (u16)(i + 1);
		return 1;
	}
}

static u8 up_fat(u8 c) {
	return c >= 'a' && c <= 'z' ? (u8)(c - 32) : c;
}

/* Map a character to a valid 8.3 character, upper-casing and
 * replacing anything illegal with '_'. */
static u8 short_char(u8 c) {
	c = up_fat(c);
	if (c < 0x20 || c == ' ' || c == '"' || c == '*' || c == '+' ||
	    c == ',' || c == '.' || c == '/' || c == ':' || c == ';' ||
	    c == '<' || c == '=' || c == '>' || c == '?' || c == '[' ||
	    c == '\\' || c == ']' || c == '|') {
		return '_';
	}
	return c;
}

/* If name fits the 8.3 form, fill out11 (space-padded, upper-
 * cased) and return 1; otherwise return 0 (a long name is
 * required). Lower-case 8.3 names are accepted and upper-cased
 * (case is not preserved for short names). */
int fat_is_short_name(const char *name, u8 *out11) {
	u16 i, len = 0, base, ext;
	int dot = -1;
	for (i = 0; name[i] != '\0'; i++) {
		len++;
	}
	if (len == 0 || len > 12) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		if (name[i] == '.') {
			dot = (int)i;
		}
	}
	if (dot < 0) {
		base = len;
		ext = 0;
	} else {
		base = (u16)dot;
		ext = (u16)(len - dot - 1);
	}
	if (base == 0 || base > 8 || ext > 3) {
		return 0;
	}
	for (i = 0; i < len; i++) {
		u8 c = (u8)name[i];
		if (c == '.') {
			if ((int)i != dot) {
				return 0; /* more than one dot */
			}
			continue;
		}
		if (c == ' ' || c == '+' || c == ',' || c == ';' || c == '=' ||
		    c == '[' || c == ']') {
			return 0;
		}
	}
	for (i = 0; i < 11; i++) {
		out11[i] = ' ';
	}
	for (i = 0; i < base; i++) {
		out11[i] = up_fat((u8)name[i]);
	}
	for (i = 0; i < ext; i++) {
		out11[8 + i] = up_fat((u8)name[dot + 1 + i]);
	}
	return 1;
}

/* Generate a unique 8.3 alias (BASE~N.EXT) for a long name in
 * dir_cluster, avoiding collisions with existing entries. */
void fat_make_shortname(u16 dir_cluster, const char *longname, u8 *out11) {
	u8       base[8], ext[3];
	u16      bl = 0, el = 0, len = 0, i, n;
	int      dot = -1;
	dirent83 e;
	for (i = 0; longname[i] != '\0'; i++) {
		len++;
	}
	for (i = 0; i < len; i++) {
		if (longname[i] == '.') {
			dot = (int)i;
		}
	}
	for (i = 0; i < len && bl < 8; i++) {
		u8 c = (u8)longname[i];
		if ((int)i == dot) {
			break;
		}
		if (c == ' ' || c == '.') {
			continue;
		}
		base[bl++] = short_char(c);
	}
	if (dot >= 0) {
		for (i = (u16)(dot + 1); i < len && el < 3; i++) {
			u8 c = (u8)longname[i];
			if (c == ' ' || c == '.') {
				continue;
			}
			ext[el++] = short_char(c);
		}
	}
	if (bl == 0) {
		base[bl++] = '_';
	}
	for (n = 1; n <= 999; n++) {
		u8  num[3];
		u16 nd = 0, keep, t = n;
		u8  tmp[3], td = 0;
		while (t != 0) {
			tmp[td++] = (u8)('0' + t % 10);
			t /= 10;
		}
		while (td != 0) {
			num[nd++] = tmp[--td];
		}
		keep = (u16)(8 - 1 - nd);
		if (keep > bl) {
			keep = bl;
		}
		for (i = 0; i < 11; i++) {
			out11[i] = ' ';
		}
		for (i = 0; i < keep; i++) {
			out11[i] = base[i];
		}
		out11[keep] = '~';
		for (i = 0; i < nd; i++) {
			out11[keep + 1 + i] = num[i];
		}
		for (i = 0; i < el; i++) {
			out11[8 + i] = ext[i];
		}
		if (fat_dir_search(dir_cluster, out11, &e) != 0) {
			return; /* unique */
		}
	}
}

/* Find a contiguous run of count free directory slots (deleted
 * or end-of-directory), growing subdirectories as needed.
 * 0 = OK (*first = first slot index), -1 = directory full. */
int fat_dir_alloc_run(u16 dir_cluster, u16 count, u16 *first) {
	u16      i = 0, run = 0, start = 0;
	dirent83 e;
	for (;;) {
		if (fat_dir_entry(dir_cluster, i, &e) != 0) {
			if (dir_cluster == 0) {
				return -1; /* root directory is full */
			}
			if (fat_dir_extend(dir_cluster) != 0) {
				return -1;
			}
			continue; /* entry i now exists, zeroed */
		}
		if (e.name[0] == 0x00 || e.name[0] == 0xE5) {
			if (run == 0) {
				start = i;
			}
			run++;
			if (run == count) {
				*first = start;
				return 0;
			}
		} else {
			run = 0;
		}
		i++;
	}
}

/* Allocate a run for the long-name slots of longname plus the
 * trailing 8.3 entry, write the slots (checksummed against
 * short11), and return in *short_index where the caller must
 * write the 8.3 entry. Does not commit. 0 = OK, -1 = full. */
int fat_alloc_lfn_entry(u16 dir_cluster, const char *longname,
                        const u8 *short11, u16 *short_index) {
	u16 len = 0, nslots, first, i;
	u8  sum;
	for (i = 0; longname[i] != '\0'; i++) {
		len++;
	}
	nslots = (u16)((len + 12) / 13);
	if (nslots == 0) {
		nslots = 1;
	}
	if (fat_dir_alloc_run(dir_cluster, (u16)(nslots + 1), &first) != 0) {
		return -1;
	}
	sum = fat_lfn_checksum(short11);
	for (i = 0; i < nslots; i++) {
		dirent83 slot;
		u8      *raw = (u8 *)&slot;
		u16      ordn = (u16)(nslots - i); /* slot first+i holds this ordinal */
		u16      base = (u16)((ordn - 1) * 13);
		u16      k;
		fmemset(&slot, 0, 32);
		raw[0] = (u8)(ordn | (i == 0 ? 0x40 : 0)); /* first on disk = last */
		raw[11] = ATTR_LFN;
		raw[13] = sum;
		for (k = 0; k < 13; k++) {
			u16 off = lfn_unit_off[k];
			u16 idx = (u16)(base + k);
			u16 unit;
			if (idx < len) {
				unit = (u8)longname[idx];
			} else if (idx == len) {
				unit = 0x0000;
			} else {
				unit = 0xFFFF;
			}
			raw[off] = (u8)(unit & 0xFF);
			raw[off + 1] = (u8)(unit >> 8);
		}
		if (fat_dir_set(dir_cluster, (u16)(first + i), &slot) != 0) {
			return -1;
		}
	}
	*short_index = (u16)(first + nslots);
	return 0;
}

/* Mark the long-name slots immediately preceding short_index as
 * deleted (0xE5). Stops at the first non-LFN / free entry. */
void fat_delete_lfn_slots(u16 dir_cluster, u16 short_index) {
	dirent83 e;
	u16      i = short_index;
	while (i > 0) {
		i--;
		if (fat_dir_entry(dir_cluster, i, &e) != 0) {
			break;
		}
		if (e.attr != ATTR_LFN || e.name[0] == 0xE5 || e.name[0] == 0x00) {
			break;
		}
		e.name[0] = 0xE5;
		if (fat_dir_set(dir_cluster, i, &e) != 0) {
			break;
		}
	}
}


/* Parse one path component at *pp into an 11-char 8.3 name
 * (uppercased, '*' expanded to '?'s). Advances *pp past the
 * component and one trailing separator. 0 = OK, -1 = empty. */
static int parse_component(const char __far **pp, u8 *out11) {
	const char __far *p = *pp;
	u16               i;
	char              c;
	for (i = 0; i < 11; i++) {
		out11[i] = ' ';
	}
	if (*p == '.') {
		out11[0] = '.'; /* "." and ".." entries */
		p++;
		if (*p == '.') {
			out11[1] = '.';
			p++;
		}
	} else {
		i = 0;
		for (;;) {
			c = *p;
			if (c == '\0' || c == '\\' || c == '/' || c == '.') {
				break;
			}
			if (c >= 'a' && c <= 'z') {
				c -= 32;
			}
			if (c == '*') {
				while (i < 8) {
					out11[i++] = '?';
				}
				p++;
				continue;
			}
			if (i < 8) {
				out11[i++] = (u8)c; /* chars past 8 are ignored */
			}
			p++;
		}
		if (i == 0) {
			return -1;
		}
		if (c == '.') {
			p++;
			i = 8;
			for (;;) {
				c = *p;
				if (c == '\0' || c == '\\' || c == '/' || c == '.') {
					break;
				}
				if (c >= 'a' && c <= 'z') {
					c -= 32;
				}
				if (c == '*') {
					while (i < 11) {
						out11[i++] = '?';
					}
					p++;
					continue;
				}
				if (i < 11) {
					out11[i++] = (u8)c;
				}
				p++;
			}
		}
	}
	if (*p == '\\' || *p == '/') {
		p++;
	}
	*pp = p;
	return 0;
}

/* Select the volume named by the "X:" prefix (or the default
 * drive) and advance *pp past it. 0 = OK or DOS error. */
static u16 select_by_prefix(const char __far **pp) {
	const char __far *p = *pp;
	if (p[0] != '\0' && p[1] == ':') {
		char d = p[0];
		if (d >= 'a') {
			d = (char)(d - 32);
		}
		if (d < 'A' || fat_select((u8)(d - 'A')) != 0) {
			return ERR_PATH_NOT_FOUND;
		}
		*pp = p + 2;
	} else {
		fat_select(cur_drive);
	}
	return 0;
}

/* Resolve all but the last component of path. Returns the
 * containing directory's cluster in *dir_cl and the final
 * component (may contain '?') in last11. Leaves the path's
 * volume selected. 0 = OK or DOS error. */
u16 fat_resolve_dir(const char __far *path, u16 *dir_cl, u8 *last11) {
	u16               cl;
	u8                comp[11];
	const char __far *p = path;
	dirent83          e;
	u16               err;

	err = select_by_prefix(&p);
	if (err != 0) {
		return err;
	}
	if (*p == '\\' || *p == '/') {
		cl = 0;
		p++;
	} else {
		cl = v->cwd_cluster;
	}
	if (*p == '\0') {
		return ERR_PATH_NOT_FOUND;
	}
	for (;;) {
		if (parse_component(&p, comp) != 0) {
			return ERR_PATH_NOT_FOUND;
		}
		if (*p == '\0') {
			fmemcpy(last11, comp, 11);
			*dir_cl = cl;
			return 0;
		}
		if (comp[0] == '.' && comp[1] == ' ') {
			continue; /* "." - stay */
		}
		if (fat_dir_search(cl, comp, &e) != 0) {
			return ERR_PATH_NOT_FOUND;
		}
		if ((e.attr & ATTR_DIR) == 0) {
			return ERR_PATH_NOT_FOUND;
		}
		cl = e.cluster; /* ".." stores 0 for root - works */
	}
}

/* Change the current directory of the path's volume (an "X:"
 * prefix changes that drive's cwd without switching drives,
 * like DOS). Returns 0 or a DOS error code. */
u16 fat_chdir(const char __far *path) {
	char              newpath[64];
	u16               np = 0;
	u16               cl;
	u16               i;
	const char __far *p = path;
	u8                comp[11];
	dirent83          e;
	u16               err;

	err = select_by_prefix(&p);
	if (err != 0) {
		return err;
	}
	if (*p == '\0') {
		return ERR_PATH_NOT_FOUND;
	}
	if (*p == '\\' || *p == '/') {
		cl = 0;
		p++;
	} else {
		cl = v->cwd_cluster;
		for (np = 0; v->cwd[np] != '\0'; np++) {
			newpath[np] = v->cwd[np];
		}
	}
	while (*p != '\0') {
		if (parse_component(&p, comp) != 0) {
			return ERR_PATH_NOT_FOUND;
		}
		if (comp[0] == '.' && comp[1] == ' ') {
			continue; /* "." - no-op */
		}
		if (comp[0] == '.' && comp[1] == '.') {
			if (cl == 0) {
				return ERR_PATH_NOT_FOUND;
			}
			if (fat_dir_search(cl, comp, &e) != 0) {
				return ERR_PATH_NOT_FOUND;
			}
			cl = e.cluster;
			while (np > 0 && newpath[np - 1] != '\\') {
				np--; /* pop last component text */
			}
			if (np > 0) {
				np--; /* and its backslash */
			}
			continue;
		}
		if (fat_dir_search(cl, comp, &e) != 0) {
			return ERR_PATH_NOT_FOUND;
		}
		if ((e.attr & ATTR_DIR) == 0) {
			return ERR_PATH_NOT_FOUND;
		}
		cl = e.cluster;
		if (np > 0 && np < 63) {
			newpath[np++] = '\\';
		}
		for (i = 0; i < 8 && comp[i] != ' '; i++) {
			if (np < 63) {
				newpath[np++] = (char)comp[i];
			}
		}
		if (comp[8] != ' ') {
			if (np < 63) {
				newpath[np++] = '.';
			}
			for (i = 8; i < 11 && comp[i] != ' '; i++) {
				if (np < 63) {
					newpath[np++] = (char)comp[i];
				}
			}
		}
	}
	newpath[np] = '\0';
	v->cwd_cluster = cl;
	for (i = 0; i <= np; i++) {
		v->cwd[i] = newpath[i];
	}
	return 0;
}

/* Current directory text of a drive (0 = A:). 0 when invalid. */
const char *fat_get_cwd(u8 drv) {
	if (drv >= NUM_DRIVES || !vols[drv].present) {
		return 0;
	}
	return vols[drv].cwd;
}

u16 fat_cwd_cluster(void) {
	return v->cwd_cluster;
}

/* --- INT 25h/26h: absolute disk read/write ---
 *
 * AL = drive (0=A), CX = sector count, DX = first logical
 * sector, DS:BX = buffer. Logical sectors are volume-relative
 * (sector 0 = the volume's boot sector). CX = FFFFh selects
 * the packet form with DS:BX -> abs_packet, mandatory for
 * volumes of 64K+ sectors (> 32 MB) - the old form then fails
 * with AX = 0207h, exactly like DOS 4+.
 *
 * The stub in startup.asm returns with RETF leaving the
 * INT-pushed flags on the caller's stack and the live CF
 * carrying the status (the documented quirk of these two
 * vectors); it reads the status from absdisk_cf. */

#pragma pack(push, 1)
typedef struct abs_packet {
	u32 sector;
	u16 count;
	u32 buf; /* far pointer, offset:segment */
} abs_packet;
#pragma pack(pop)

u8 absdisk_cf; /* 1 = error, the stub turns it into CF */

/* Drop every cache that could shadow a raw write, and reload
 * the FAT12 whole-FAT cache when its volume was hit. */
static void absdisk_invalidate(volume *m) {
	u16 i;
	buf_lba = 0xFFFFFFFFUL;
	f16vol = 0;
	f16lba = 0xFFFFFFFFUL;
	if (v12 == m) {
		for (i = 0; i < m->secs_per_fat; i++) {
			if (disk_read(m->bios_drive, m->fat_lba + i, fatcache + i * 512) !=
			    0) {
				break;
			}
		}
		fat12_dirty = 0;
	}
}

void __cdecl absdisk_dispatch(iregs __far *r) {
	u8      drv = (u8)(r->ax & 0xFF);
	u8      write = (u8)(r->ax >> 8); /* stub: 0 = INT 25h, 1 = INT 26h */
	u32     sector;
	u16     count;
	u16     bseg, boff;
	u16     i;
	volume *m;

	absdisk_cf = 1;
	if (drv >= NUM_DRIVES || !vols[drv].present) {
		r->ax = 0x0101; /* unknown unit */
		return;
	}
	m = &vols[drv];
	if (r->cx == 0xFFFF) {
		const abs_packet __far *p =
		    (const abs_packet __far *)MK_FP(r->ds, r->bx);
		sector = p->sector;
		count = p->count;
		boff = (u16)p->buf;
		bseg = (u16)(p->buf >> 16);
	} else {
		if (m->total_secs > 0xFFFFUL) {
			r->ax = 0x0207; /* big volume needs the packet form */
			return;
		}
		sector = r->dx;
		count = r->cx;
		bseg = r->ds;
		boff = r->bx;
	}
	if (count == 0 || sector >= m->total_secs ||
	    count > m->total_secs - sector) {
		r->ax = 0x0408; /* sector not found */
		return;
	}
	/* flush dirty FAT cache so raw reads see current data */
	if (fat_commit() != 0) {
		r->ax = 0x8080;
		return;
	}
	for (i = 0; i < count; i++) {
		u32 lba = m->base_lba + sector + i;
		int st = write ? disk_write(m->bios_drive, lba, MK_FP(bseg, boff))
		               : disk_read(m->bios_drive, lba, MK_FP(bseg, boff));
		if (st != 0) {
			if (write) {
				absdisk_invalidate(m);
			}
			r->ax = 0x8080; /* attachment failed to respond */
			return;
		}
		bseg += 32; /* 512 bytes, immune to offset wrap */
	}
	if (write) {
		absdisk_invalidate(m);
	}
	absdisk_cf = 0;
}
