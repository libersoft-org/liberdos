/* ============================================================
 * util.c - small freestanding helpers (no C library)
 * ============================================================ */
#include "kernel.h"

void fmemcpy(void __far *dst, const void __far *src, u16 n) {
	u8 __far       *d = (u8 __far *)dst;
	const u8 __far *s = (const u8 __far *)src;
	while (n != 0) {
		*d = *s;
		d++;
		s++;
		n--;
	}
}

void fmemset(void __far *dst, u8 val, u16 n) {
	u8 __far *d = (u8 __far *)dst;
	while (n != 0) {
		*d = val;
		d++;
		n--;
	}
}

u16 fstrlen(const char __far *s) {
	u16 n = 0;
	while (s[n] != '\0') {
		n++;
	}
	return n;
}
