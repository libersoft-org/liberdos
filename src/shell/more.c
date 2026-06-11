/* ============================================================
 * more.c - MORE.COM: page output one screenful at a time
 *
 * Usage: MORE file      (or MORE < file via handle 0 one day -
 * the shell has no pipes yet, so the file argument is the
 * normal route). Counts screen lines including wraps at column
 * 80; every 24 lines prints "-- More --" and waits for a key.
 * ============================================================ */
#include "dosapi.h"

static u8 iobuf[512];

static u8 lines;
static u8 col;

static void pause_prompt(void) {
	dos_print("-- More --$");
	dos_getch();
	dos_print("\r          \r$");
	lines = 0;
}

static void put(char c) {
	if (c == '\r') {
		dos_putc(c);
		col = 0;
		return;
	}
	if (c == '\n') {
		dos_putc(c);
		col = 0;
		lines++;
	} else {
		dos_putc(c);
		col++;
		if (col >= 80) {
			col = 0;
			lines++;
		}
	}
	if (lines >= 24) {
		pause_prompt();
	}
}

void shell_main(void) {
	u8    len = *(u8 *)0x80;
	char *tail = (char *)0x81;
	int   h, n, i;

	tail[len] = 0;
	while (*tail == ' ') {
		tail++;
	}

	if (*tail == 0) {
		h = 0; /* no argument: read stdin */
	} else {
		for (i = 0; tail[i] != 0 && tail[i] != ' '; i++) {
		}
		tail[i] = 0;
		h = dos_open(tail);
		if (h < 0) {
			dos_print("File not found\r\n$");
			dos_exit(1);
		}
	}

	lines = 0;
	col = 0;
	for (;;) {
		n = dos_read(h, iobuf, sizeof(iobuf));
		if (n <= 0) {
			break;
		}
		for (i = 0; i < n; i++) {
			if (iobuf[i] == 0x1A) { /* ^Z = DOS end of text */
				n = -1;
				break;
			}
			put((char)iobuf[i]);
		}
		if (n < 0) {
			break;
		}
	}
	if (h != 0) {
		dos_close(h);
	}
}
