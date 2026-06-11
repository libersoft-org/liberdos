/* ============================================================
 * attrib.c - ATTRIB.COM: show or change file attributes
 *
 * Usage: ATTRIB [+R|-R] [+A|-A] [+S|-S] [+H|-H] [file]
 * Without flags it lists matching files (wildcards OK) with
 * their R/A/S/H letters, DOS style. With flags it applies the
 * changes to every match. No argument means *.*.
 * ============================================================ */
#include "dosapi.h"

static u8   dta[128];
static char pattern[80];
static char fullname[80];
static u8   dir_len; /* directory part length in pattern */

static u16 set_mask;
static u16 clr_mask;

static void print(const char *s) {
	while (*s != 0) {
		dos_putc(*s++);
	}
}

static u16 flag_bit(char c) {
	switch (c) {
	case 'R':
	case 'r':
		return 0x01;
	case 'H':
	case 'h':
		return 0x02;
	case 'S':
	case 's':
		return 0x04;
	case 'A':
	case 'a':
		return 0x20;
	default:
		return 0;
	}
}

static void build_fullname(const char *name) {
	u8 i;
	for (i = 0; i < dir_len; i++) {
		fullname[i] = pattern[i];
	}
	while (*name != 0) {
		fullname[i++] = *name++;
	}
	fullname[i] = 0;
}

static void show_one(const char *name, u8 attr) {
	dos_putc((char)((attr & 0x20) ? 'A' : ' '));
	dos_putc(' ');
	dos_putc((char)((attr & 0x04) ? 'S' : ' '));
	dos_putc((char)((attr & 0x02) ? 'H' : ' '));
	dos_putc((char)((attr & 0x01) ? 'R' : ' '));
	print("     ");
	print(name);
	print("\r\n");
}

void shell_main(void) {
	u8    len = *(u8 *)0x80;
	char *tail = (char *)0x81;
	char *p;
	u8    i, got_file = 0;
	int   r;

	tail[len] = 0;
	p = tail;
	for (;;) {
		while (*p == ' ') {
			p++;
		}
		if (*p == 0) {
			break;
		}
		if (*p == '+' || *p == '-') {
			u16 bit = flag_bit(p[1]);
			if (bit == 0) {
				print("Invalid switch\r\n");
				dos_exit(1);
			}
			if (*p == '+') {
				set_mask |= bit;
			} else {
				clr_mask |= bit;
			}
			p += 2;
			continue;
		}
		for (i = 0; p[i] != 0 && p[i] != ' '; i++) {
			pattern[i] = p[i];
		}
		pattern[i] = 0;
		p += i;
		got_file = 1;
	}
	if (!got_file) {
		pattern[0] = '*';
		pattern[1] = '.';
		pattern[2] = '*';
		pattern[3] = 0;
	}

	/* directory prefix = everything up to the last \ or : */
	dir_len = 0;
	for (i = 0; pattern[i] != 0; i++) {
		if (pattern[i] == '\\' || pattern[i] == ':') {
			dir_len = (u8)(i + 1);
		}
	}

	dos_setdta(dta);
	r = dos_findfirst(pattern, 0x07); /* include R/H/S files */
	if (r < 0) {
		print("File not found\r\n");
		dos_exit(1);
	}
	while (r == 0) {
		const char *name = (const char *)&dta[30];
		u8          attr = dta[21];
		if (!(attr & 0x10)) { /* skip directories */
			if (set_mask == 0 && clr_mask == 0) {
				show_one(name, attr);
			} else {
				build_fullname(name);
				attr = (u8)((attr | set_mask) & ~clr_mask);
				if (dos_setattr(fullname, attr) < 0) {
					print("Access denied - ");
					print(fullname);
					print("\r\n");
				}
			}
		}
		r = dos_findnext();
	}
}
