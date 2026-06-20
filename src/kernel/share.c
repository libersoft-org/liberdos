/* ============================================================
 * share.c - built-in SHARE: file region locks + sharing modes
 *
 * The classic DOS SHARE.EXE is a TSR that hooks the kernel's
 * file layer; here the same functionality is native. Region
 * locks (INT 21h/5Ch) live in a fixed table keyed by the
 * file's identity (volume + dirent location) and the owning
 * PSP. The open-mode compatibility matrix for sharing modes
 * (deny all/write/read/none, compatibility) is checked by
 * file.c at open time through share_mode_compat().
 *
 * The INT 2Fh AX=1000h install check is answered in
 * startup.asm with AL=FFh, so programs that probe for SHARE
 * before using locks find it loaded.
 * ============================================================ */
#include "kernel.h"

#define MAX_LOCKS 32

typedef struct lockent {
	u8     used;
	u8     drv; /* file identity: volume + dirent location */
	clus_t dir_cluster;
	u16    dir_index;
	u16    psp; /* owner */
	u32    start;
	u32    len;
} lockent;

static lockent locks[MAX_LOCKS];

static int same_file(const lockent *l, u8 drv, clus_t dcl, u16 idx) {
	return l->drv == drv && l->dir_cluster == dcl && l->dir_index == idx;
}

static int ranges_overlap(u32 s1, u32 l1, u32 s2, u32 l2) {
	return s1 < s2 + l2 && s2 < s1 + l1;
}

/* Lock a region. Any overlap with an existing lock on the same
 * file - even our own - is a violation, like DOS. */
u16 share_lock(u8 drv, clus_t dcl, u16 idx, u16 psp, u32 start, u32 len) {
	int i;
	int slot = -1;
	if (len == 0) {
		return 0; /* zero-length lock: accepted no-op */
	}
	for (i = 0; i < MAX_LOCKS; i++) {
		if (!locks[i].used) {
			slot = i;
			continue;
		}
		if (same_file(&locks[i], drv, dcl, idx) &&
		    ranges_overlap(locks[i].start, locks[i].len, start, len)) {
			return ERR_LOCK_VIOLATION;
		}
	}
	if (slot < 0) {
		return ERR_LOCK_VIOLATION; /* table full */
	}
	locks[slot].used = 1;
	locks[slot].drv = drv;
	locks[slot].dir_cluster = dcl;
	locks[slot].dir_index = idx;
	locks[slot].psp = psp;
	locks[slot].start = start;
	locks[slot].len = len;
	return 0;
}

/* Unlock: the region must exactly match a lock we own. */
u16 share_unlock(u8 drv, clus_t dcl, u16 idx, u16 psp, u32 start, u32 len) {
	int i;
	for (i = 0; i < MAX_LOCKS; i++) {
		if (locks[i].used && locks[i].psp == psp &&
		    same_file(&locks[i], drv, dcl, idx) && locks[i].start == start &&
		    locks[i].len == len) {
			locks[i].used = 0;
			return 0;
		}
	}
	return ERR_LOCK_VIOLATION;
}

/* Read/write into a region locked by another process is a lock
 * violation; our own locks never block us. */
u16 share_io_check(u8 drv, clus_t dcl, u16 idx, u16 psp, u32 start, u32 len) {
	int i;
	if (len == 0) {
		return 0;
	}
	for (i = 0; i < MAX_LOCKS; i++) {
		if (locks[i].used && locks[i].psp != psp &&
		    same_file(&locks[i], drv, dcl, idx) &&
		    ranges_overlap(locks[i].start, locks[i].len, start, len)) {
			return ERR_LOCK_VIOLATION;
		}
	}
	return 0;
}

/* Last reference to the file went away: drop its locks. */
void share_file_closed(u8 drv, clus_t dcl, u16 idx) {
	int i;
	for (i = 0; i < MAX_LOCKS; i++) {
		if (locks[i].used && same_file(&locks[i], drv, dcl, idx)) {
			locks[i].used = 0;
		}
	}
}

/* Process terminated: drop every lock it still owns. */
void share_psp_closed(u16 psp) {
	int i;
	for (i = 0; i < MAX_LOCKS; i++) {
		if (locks[i].used && locks[i].psp == psp) {
			locks[i].used = 0;
		}
	}
}

/* Sharing-mode compatibility of a new open against an existing
 * open of the same file. Mode bytes are the full open AL:
 * access in bits 0-2 (0=read 1=write 2=rdwr), sharing in bits
 * 4-6 (0=compat 1=deny all 2=deny write 3=deny read
 * 4=deny none). Returns nonzero when the two may coexist. */
int share_mode_compat(u8 existing, u8 newmode) {
	u8 esh = (u8)((existing >> 4) & 7);
	u8 nsh = (u8)((newmode >> 4) & 7);
	u8 eac = (u8)(existing & 7);
	u8 nac = (u8)(newmode & 7);
	if (esh == 0 || nsh == 0) {
		return esh == 0 && nsh == 0; /* compat only pairs with compat */
	}
	/* existing's deny vs new access */
	if (esh == 1) {
		return 0; /* deny all */
	}
	if (esh == 2 && nac != 0) {
		return 0; /* deny write vs write/rdwr */
	}
	if (esh == 3 && nac != 1) {
		return 0; /* deny read vs read/rdwr */
	}
	/* new's deny vs existing access */
	if (nsh == 1) {
		return 0;
	}
	if (nsh == 2 && eac != 0) {
		return 0;
	}
	if (nsh == 3 && eac != 1) {
		return 0;
	}
	return 1;
}
