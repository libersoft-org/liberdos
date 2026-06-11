/* ============================================================
 * setver.c - per-program reported DOS version table (SETVER)
 *
 * Some programs refuse to run unless INT 21h fn 30h reports a
 * specific DOS version. The table below maps program names to
 * version overrides; EXEC consults it after building the PSP
 * and stores the result in the PSP version word (offset 40h),
 * which fn 30h reports back. The table lives in kernel data
 * and is exposed to the shell's SETVER command through the
 * private multiplex call INT 2Fh AX=F801h (-> ES:BX, CX =
 * entry count); both sides agree on the 16-byte entry layout.
 * RAM-only: edits last until reboot.
 * ============================================================ */
#include "kernel.h"

setver_entry setver_table[SETVER_MAX]; /* BSS: zeroed at boot */

static u8 up(u8 c) {
	return c >= 'a' && c <= 'z' ? (u8)(c - 32) : c;
}

/* Apply a version override to a freshly built PSP. "path" is
 * the full EXEC path; only the final name component is matched
 * (case-insensitive) against the table. */
void setver_apply(u16 psp, const char __far *path) {
	const char __far *base = path;
	u16               i;
	for (i = 0; path[i] != '\0'; i++) {
		if (path[i] == '\\' || path[i] == '/' || path[i] == ':') {
			base = &path[i + 1];
		}
	}
	for (i = 0; i < SETVER_MAX; i++) {
		const setver_entry *e = &setver_table[i];
		u16                 k;
		if (e->name[0] == '\0') {
			continue;
		}
		for (k = 0; k < sizeof(e->name); k++) {
			if (e->name[k] != (char)up((u8)base[k])) {
				break;
			}
			if (e->name[k] == '\0') {
				pokew(psp, 0x40, (u16)(e->major | ((u16)e->minor << 8)));
				return;
			}
		}
	}
}
