/* ============================================================
 * chkdsk.c - CHKDSK.COM: report drive, file and memory usage
 *
 * Usage: CHKDSK [d:]. Walks the whole directory tree with
 * FindFirst/FindNext (one DTA per recursion level on the
 * stack, restored after each descent), then prints a DOS-style
 * report from INT 21h 36h totals plus the BIOS memory size and
 * the largest free arena. Read-only: it checks nothing it
 * could repair, like the real one pretended to.
 * ============================================================ */
#include "dosapi.h"

static u32  n_files, n_dirs, file_bytes;
static char path[128];

static void print(const char *s) {
	while (*s != 0) {
		dos_putc(*s++);
	}
}

static void print_u32_pad(u32 v, u8 width) {
	char b[10];
	u8   n = 0;
	do {
		b[n++] = (char)('0' + (u8)(v % 10));
		v /= 10;
	} while (v != 0);
	while (width > n) {
		dos_putc(' ');
		width--;
	}
	while (n != 0) {
		dos_putc(b[--n]);
	}
}

static u8 is_dot(const char *n) {
	if (n[0] != '.') {
		return 0;
	}
	return (u8)(n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

static void walk(u8 len) {
	u8  dta_local[64];
	int r;

	path[len + 0] = '*';
	path[len + 1] = '.';
	path[len + 2] = '*';
	path[len + 3] = 0;
	dos_setdta(dta_local);
	r = dos_findfirst(path, 0x17); /* dirs + R/H/S files */
	while (r == 0) {
		const char *name = (const char *)&dta_local[30];
		u8          attr = dta_local[21];
		if (attr & 0x10) {
			if (!is_dot(name) && len < 100) {
				u8 nl = len;
				u8 i;
				n_dirs++;
				for (i = 0; name[i] != 0; i++) {
					path[nl++] = name[i];
				}
				path[nl++] = '\\';
				walk(nl);
				dos_setdta(dta_local); /* descent moved the DTA */
			}
		} else {
			n_files++;
			file_bytes += *(u32 *)&dta_local[26];
		}
		r = dos_findnext();
	}
}

void shell_main(void) {
	u8    len = *(u8 *)0x80;
	char *tail = (char *)0x81;
	u8    dl;
	u32   cl, cs, clusbytes;

	dos_resize(get_psp(), 0x1000); /* a .COM owns all memory: keep
	                                * 64 KB so "bytes free" is real */
	tail[len] = 0;
	while (*tail == ' ') {
		tail++;
	}
	if (*tail != 0 && tail[1] == ':') {
		char c = *tail;
		if (c >= 'a' && c <= 'z') {
			c = (char)(c - 32);
		}
		dl = (u8)(c - 'A' + 1);
	} else {
		dl = (u8)(dos_getdrive() + 1);
	}

	cs = dos_clussize(dl); /* spc:bps */
	cl = dos_clusters(dl); /* total:free */
	if (cs == 0) {
		print("Invalid drive specification\r\n");
		dos_exit(1);
	}
	clusbytes = (u32)(u16)(cs >> 16) * (u16)cs;

	path[0] = (char)('A' + dl - 1);
	path[1] = ':';
	path[2] = '\\';
	n_files = 0;
	n_dirs = 0;
	file_bytes = 0;
	walk(3);

	print("\r\n");
	print_u32_pad((u32)(cl >> 16) * clusbytes, 12);
	print(" bytes total disk space\r\n");
	print_u32_pad(n_dirs * clusbytes, 12);
	print(" bytes in ");
	print_u32_pad(n_dirs, 0);
	print(" directories\r\n");
	print_u32_pad(file_bytes, 12);
	print(" bytes in ");
	print_u32_pad(n_files, 0);
	print(" user files\r\n");
	print_u32_pad((u32)(u16)cl * clusbytes, 12);
	print(" bytes available on disk\r\n\r\n");

	print_u32_pad(clusbytes, 12);
	print(" bytes in each allocation unit\r\n");
	print_u32_pad((u32)(cl >> 16), 12);
	print(" total allocation units on disk\r\n");
	print_u32_pad((u32)(u16)cl, 12);
	print(" available allocation units on disk\r\n\r\n");

	print_u32_pad((u32)peekw(0x40, 0x13) * 1024, 12);
	print(" total bytes memory\r\n");
	print_u32_pad((u32)dos_largest_free() * 16, 12);
	print(" bytes free\r\n");
}
