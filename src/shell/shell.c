/* ============================================================
 * shell.c - command interpreter (COMMAND.COM)
 *
 * A regular .COM program: no kernel access, everything goes
 * through INT 21h (dosapi.h). On startup it shrinks its memory
 * block to 64 KB so EXEC has room for children, clones its
 * environment into an own block (SET edits it, children get
 * copies from the kernel), runs AUTOEXEC.BAT from the boot
 * drive if present and drops into the prompt loop.
 *
 * Internal: DIR CD/CHDIR TYPE CLS ECHO SET VER SETVER MEM
 *           PAUSE REM COPY DEL/ERASE REN/RENAME MD/MKDIR
 *           RD/RMDIR DATE TIME EXIT and the drive-change
 *           form "X:".
 * Anything else is run as NAME.COM / NAME.EXE / NAME.BAT,
 * first in the current directory, then in each ;-separated
 * directory of the PATH environment variable.
 * ============================================================ */
#include "dosapi.h"
#include "../version.h"

#define ENV_PARAS   64   /* shell environment block: 1 KB */
#define BATCH_MAX   2048 /* batch file size limit (stack buf) */
#define BATCH_DEPTH 3

static u8   inbuf[131]; /* AH=0Ah buffer */
static char line[130];
static u8   tailbuf[130]; /* PSP-style command tail for EXEC */
static u8   dta[128];
static u8   iobuf[512];
static u16  env_seg;
static u8   echo_on = 1;
static u8   batch_depth = 0;

/* Set by the INT 23h handler in cstart.asm when Ctrl-C hits
 * while the shell itself is the current process; the command
 * loops poll it to abandon the work in progress. The flag
 * lives in cstart.asm so the startup links standalone. */
extern u8 shell_break_hit;

extern void break_handler(void); /* cstart.asm INT 23h handler */

/* clang-format off */
static void dos_setvect23(void (*h)(void)); /* 25h, AL=23h */
#pragma aux dos_setvect23 = \
	"mov ax,2523h"          \
	"int 21h"               \
	parm [dx] modify [ax];

/* Private multiplex call: kernel SETVER table -> ES:BX, CX =
 * entry count. Returns seg:off packed in DX:AX (0 = absent). */
static u32 setver_table_ptr(void); /* INT 2Fh, AX=F801h */
#pragma aux setver_table_ptr = \
	"xor bx,bx"                \
	"mov es,bx"                \
	"mov ax,0F801h"            \
	"int 2Fh"                  \
	"mov dx,es"                \
	"mov ax,bx"                \
	value [dx ax] modify [bx cx es];
/* clang-format on */

/* --- tiny string/print helpers (no C runtime) --- */

static u16 slen(const char *s) {
	u16 n = 0;
	while (s[n] != '\0') {
		n++;
	}
	return n;
}

static char upc(char c) {
	return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static int sequal_up(const char *a, const char *up) {
	u16 i = 0;
	while (a[i] != '\0' && up[i] != '\0') {
		if (upc(a[i]) != up[i]) {
			return 0;
		}
		i++;
	}
	return a[i] == '\0' && up[i] == '\0';
}

static void print(const char *s) {
	dos_write(1, s, slen(s));
}

static void crlf(void) {
	print("\r\n");
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

/* --- environment block (variables + double NUL) --- */

static void env_init(void) {
	u16 src = peekw(get_psp(), 0x2C);
	u16 o = 0;
	env_seg = dos_alloc(ENV_PARAS);
	if (env_seg == 0) {
		return;
	}
	if (src != 0) {
		while (peekb(src, o) != 0) {
			u8 c;
			do {
				c = peekb(src, o);
				pokeb(env_seg, o, c);
				o++;
			} while (c != 0);
		}
	}
	pokeb(env_seg, o, 0); /* list terminator */
}

/* offset of the list terminator (the second NUL of the pair) */
static u16 env_end(void) {
	u16 o = 0;
	while (peekb(env_seg, o) != 0) {
		while (peekb(env_seg, o) != 0) {
			o++;
		}
		o++;
	}
	return o;
}

/* does the env string at offset o read "NAME=" ? */
static int env_match(u16 o, const char *name) {
	u16 i = 0;
	while (name[i] != '\0') {
		if (upc((char)peekb(env_seg, (u16)(o + i))) != upc(name[i])) {
			return 0;
		}
		i++;
	}
	return peekb(env_seg, (u16)(o + i)) == (u8)'=';
}

static void env_set(const char *name, const char *value) {
	u16 o = 0;
	u16 end, need, i;
	if (env_seg == 0) {
		return;
	}
	while (peekb(env_seg, o) != 0) { /* remove existing */
		u16 start = o;
		while (peekb(env_seg, o) != 0) {
			o++;
		}
		o++;
		if (env_match(start, name)) {
			u16 n = (u16)(env_end() + 1 - o);
			for (i = 0; i < n; i++) {
				pokeb(env_seg, (u16)(start + i), peekb(env_seg, (u16)(o + i)));
			}
			o = start;
		}
	}
	if (value[0] == '\0') {
		return;
	}
	end = env_end();
	need = (u16)(slen(name) + 1 + slen(value) + 2);
	if ((u32)end + need > (u32)ENV_PARAS * 16) {
		print("Out of environment space\r\n");
		return;
	}
	for (i = 0; name[i] != '\0'; i++) {
		pokeb(env_seg, end++, (u8)upc(name[i]));
	}
	pokeb(env_seg, end++, (u8)'=');
	for (i = 0; value[i] != '\0'; i++) {
		pokeb(env_seg, end++, (u8)value[i]);
	}
	pokeb(env_seg, end++, 0);
	pokeb(env_seg, end, 0); /* new terminator */
}

/* copy the value of NAME into buf (max bytes incl. NUL); 1 = found */
static int env_get(const char *name, char *buf, u16 max) {
	u16 o = 0;
	if (env_seg == 0) {
		return 0;
	}
	while (peekb(env_seg, o) != 0) {
		u16 start = o;
		while (peekb(env_seg, o) != 0) {
			o++;
		}
		o++;
		if (env_match(start, name)) {
			u16 v = (u16)(start + slen(name) + 1);
			u16 n = 0;
			while (n + 1 < max) {
				u8 c = peekb(env_seg, (u16)(v + n));
				if (c == 0) {
					break;
				}
				buf[n++] = (char)c;
			}
			buf[n] = '\0';
			return 1;
		}
	}
	return 0;
}

/* --- internal commands --- */

static void print_drive_root(void) {
	dos_putc((char)('A' + dos_getdrive()));
	print(":\\");
}

static void print_prompt(void) {
	char cwd[68];
	print_drive_root();
	if (dos_getcwd(cwd) == 0) {
		print(cwd);
	}
	dos_putc('>');
}

static void cmd_cd(const char *arg) {
	char cwd[68];
	if (arg[0] == '\0') {
		print_drive_root();
		if (dos_getcwd(cwd) == 0) {
			print(cwd);
		}
		crlf();
		return;
	}
	if (dos_chdir(arg) != 0) {
		print("Invalid directory\r\n");
	}
}

static void cmd_type(const char *arg) {
	int h, n;
	if (arg[0] == '\0') {
		print("Required parameter missing\r\n");
		return;
	}
	h = dos_open(arg);
	if (h < 0) {
		print("File not found - ");
		print(arg);
		crlf();
		return;
	}
	while ((n = dos_read(h, iobuf, sizeof(iobuf))) > 0) {
		dos_write(1, iobuf, (u16)n);
		if (shell_break_hit) { /* ^C abandons the listing */
			break;
		}
	}
	dos_close(h);
	crlf();
}

static int has_wildcard(const char *s) {
	u16 i;
	for (i = 0; s[i] != '\0'; i++) {
		if (s[i] == '*' || s[i] == '?') {
			return 1;
		}
	}
	return 0;
}

static void cmd_dir(const char *arg) {
	char pat[80];
	char cwd[68];
	u16  count = 0;
	u32  total = 0;
	u16  i = 0;
	u8   dir_drive = 0; /* 36h: 0 = default */

	if (arg[0] == '\0') {
		pat[0] = '*';
		pat[1] = '.';
		pat[2] = '*';
		pat[3] = '\0';
	} else {
		while (arg[i] != '\0' && i < 74) {
			pat[i] = arg[i];
			i++;
		}
		pat[i] = '\0';
		if (i > 0 && (pat[i - 1] == '\\' || pat[i - 1] == ':')) {
			pat[i] = '*';
			pat[i + 1] = '.';
			pat[i + 2] = '*';
			pat[i + 3] = '\0';
		} else if (!has_wildcard(pat) && i < 74) {
			int a = dos_getattr(pat);
			if (a >= 0 && (a & 0x10)) { /* a directory: list inside */
				pat[i] = '\\';
				pat[i + 1] = '*';
				pat[i + 2] = '.';
				pat[i + 3] = '*';
				pat[i + 4] = '\0';
			}
		}
		if (pat[1] == ':') {
			dir_drive = (u8)(upc(pat[0]) - 'A' + 1);
		}
	}

	print(" Directory of ");
	if (arg[0] == '\0') {
		print_drive_root();
		if (dos_getcwd(cwd) == 0) {
			print(cwd);
		}
	} else {
		print(pat);
	}
	print("\r\n\r\n");

	dos_setdta(dta);
	if (dos_findfirst(pat, 0x10) != 0) {
		/* a plain name may be a directory: retry with \*.* */
		if (arg[0] != '\0' && !has_wildcard(pat)) {
			i = slen(pat);
			if (i < 74) {
				pat[i] = '\\';
				pat[i + 1] = '*';
				pat[i + 2] = '.';
				pat[i + 3] = '*';
				pat[i + 4] = '\0';
			}
			if (dos_findfirst(pat, 0x10) != 0) {
				print("File not found\r\n");
				return;
			}
		} else {
			print("File not found\r\n");
			return;
		}
	}
	do {
		u8          attr = dta[21];
		u32         size = *(u32 *)&dta[26];
		const char *name = (const char *)&dta[30];
		u16         col = 0;
		if (name[0] == '.') { /* "." and ".." entries */
			for (i = 0; name[i] != '\0'; i++) {
				dos_putc(name[i]);
				col++;
			}
			while (col < 13) {
				dos_putc(' ');
				col++;
			}
		} else {
			for (i = 0; name[i] != '\0' && name[i] != '.'; i++) {
				dos_putc(name[i]);
				col++;
			}
			while (col < 9) {
				dos_putc(' ');
				col++;
			}
			col = 0;
			if (name[i] == '.') {
				for (i++; name[i] != '\0'; i++) {
					dos_putc(name[i]);
					col++;
				}
			}
			while (col < 4) {
				dos_putc(' ');
				col++;
			}
		}
		if (attr & 0x10) {
			print("<DIR>");
		} else {
			print_u32_pad(size, 10);
			total += size;
			count++;
		}
		crlf();
	} while (dos_findnext() == 0);

	print_u32_pad(count, 9);
	print(" file(s) ");
	print_u32_pad(total, 10);
	print(" bytes\r\n");
	print_u32_pad(dos_freebytes(dir_drive), 20);
	print(" bytes free\r\n");
}

static void cmd_echo(const char *arg) {
	if (arg[0] == '\0') {
		print(echo_on ? "ECHO is on\r\n" : "ECHO is off\r\n");
		return;
	}
	if (sequal_up(arg, "ON")) {
		echo_on = 1;
		return;
	}
	if (sequal_up(arg, "OFF")) {
		echo_on = 0;
		return;
	}
	print(arg);
	crlf();
}

static void cmd_set(char *arg) {
	if (env_seg == 0) {
		print("No environment\r\n");
		return;
	}
	if (arg[0] == '\0') {
		u16 o = 0;
		while (peekb(env_seg, o) != 0) {
			while (peekb(env_seg, o) != 0) {
				dos_putc((char)peekb(env_seg, o));
				o++;
			}
			crlf();
			o++;
		}
		return;
	}
	{
		u16 i;
		for (i = 0; arg[i] != '\0'; i++) {
			if (arg[i] == '=') {
				arg[i] = '\0';
				env_set(arg, &arg[i + 1]);
				return;
			}
		}
	}
	print("Syntax: SET name=value\r\n");
}

static void cmd_ver(void) {
	u16 v = dos_version();
	print("\r\n" OS_NAME " [Version " OS_VERSION "]  (reports DOS ");
	dos_putc((char)('0' + (v & 0xFF)));
	print(".");
	dos_putc((char)('0' + ((v >> 8) / 10)));
	dos_putc((char)('0' + ((v >> 8) % 10)));
	print(")\r\n");
}

/* --- SETVER: per-program version table in the kernel ---
 * Entries are 16 bytes: name[14] (uppercase, NUL-padded; first
 * byte 0 = free slot), major, minor. The table is edited in
 * place; the kernel applies it at EXEC time (RAM-only, lasts
 * until reboot). */
#define SETVER_ENTRY 16
#define SETVER_NAME  14
#define SETVER_MAX   16

static void print_2d(u16 v); /* defined with DATE/TIME below */

static void setver_print_entry(u16 seg, u16 o) {
	u16 i;
	for (i = 0; i < SETVER_NAME && peekb(seg, (u16)(o + i)) != 0; i++) {
		dos_putc((char)peekb(seg, (u16)(o + i)));
	}
	for (; i < SETVER_NAME + 2; i++) {
		dos_putc(' ');
	}
	print_u32_pad(peekb(seg, (u16)(o + SETVER_NAME)), 0);
	dos_putc('.');
	print_2d(peekb(seg, (u16)(o + SETVER_NAME + 1)));
	crlf();
}

/* Find the entry matching the uppercase name, or -1. */
static int setver_find(u16 seg, u16 off, const char *name) {
	u16 e;
	for (e = 0; e < SETVER_MAX; e++) {
		u16 o = (u16)(off + e * SETVER_ENTRY);
		u16 i = 0;
		while (i < SETVER_NAME && (char)peekb(seg, (u16)(o + i)) == name[i]) {
			if (name[i] == '\0') {
				return (int)e;
			}
			i++;
		}
	}
	return -1;
}

static void cmd_setver(char *arg) {
	u32   tp = setver_table_ptr();
	u16   seg = (u16)(tp >> 16);
	u16   off = (u16)tp;
	char  name[SETVER_NAME];
	u16   i, nlen;
	char *p;

	if (seg == 0) {
		print("SETVER services not available\r\n");
		return;
	}

	if (arg[0] == '\0') { /* no argument: list the table */
		u16 e, shown = 0;
		for (e = 0; e < SETVER_MAX; e++) {
			u16 o = (u16)(off + e * SETVER_ENTRY);
			if (peekb(seg, o) != 0) {
				setver_print_entry(seg, o);
				shown++;
			}
		}
		if (shown == 0) {
			print("Version table is empty\r\n");
		}
		return;
	}

	/* split: program name, then version or /D */
	p = arg;
	while (*p != '\0' && *p != ' ' && *p != '\t') {
		p++;
	}
	nlen = (u16)(p - arg);
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (nlen == 0 || nlen >= SETVER_NAME || *p == '\0') {
		print("Syntax: SETVER [program.ext n.nn | program.ext /D]\r\n");
		return;
	}
	for (i = 0; i < nlen; i++) {
		name[i] = upc(arg[i]);
	}
	name[nlen] = '\0';

	if (p[0] == '/' && upc(p[1]) == 'D') { /* delete entry */
		int e = setver_find(seg, off, name);
		if (e < 0) {
			print("Entry not found\r\n");
			return;
		}
		pokeb(seg, (u16)(off + (u16)e * SETVER_ENTRY), 0);
		print("Version table updated\r\n");
		return;
	}

	{ /* parse "n" or "n.n" or "n.nn" */
		u16 major = 0, minor = 0;
		int e;
		if (*p < '0' || *p > '9') {
			print("Invalid version number\r\n");
			return;
		}
		while (*p >= '0' && *p <= '9') {
			major = (u16)(major * 10 + (u16)(*p - '0'));
			if (major > 255) {
				print("Invalid version number\r\n");
				return;
			}
			p++;
		}
		if (*p == '.') {
			p++;
			if (*p < '0' || *p > '9') {
				print("Invalid version number\r\n");
				return;
			}
			minor = (u16)(*p - '0') * 10; /* "3.1" means 3.10 */
			p++;
			if (*p >= '0' && *p <= '9') {
				minor = (u16)(minor + (u16)(*p - '0'));
				p++;
			}
		}
		if (*p != '\0') {
			print("Invalid version number\r\n");
			return;
		}

		e = setver_find(seg, off, name); /* update in place if present */
		if (e < 0) {
			for (e = 0; e < SETVER_MAX; e++) {
				if (peekb(seg, (u16)(off + (u16)e * SETVER_ENTRY)) == 0) {
					break;
				}
			}
			if (e == SETVER_MAX) {
				print("Version table is full\r\n");
				return;
			}
		}
		{
			u16 o = (u16)(off + (u16)e * SETVER_ENTRY);
			for (i = 0; i < SETVER_NAME; i++) {
				pokeb(seg, (u16)(o + i), i <= nlen ? (u8)name[i] : 0);
			}
			pokeb(seg, (u16)(o + SETVER_NAME), (u8)major);
			pokeb(seg, (u16)(o + SETVER_NAME + 1), (u8)minor);
		}
		print("Version table updated\r\n");
	}
}

static void cmd_mem(void) {
	print("Largest free memory block: ");
	print_u32_pad((u32)dos_largest_free() * 16, 0);
	print(" bytes\r\n");
}

static void cmd_pause(void) {
	print("Press any key to continue . . .");
	(void)dos_getch();
	crlf();
}

/* --- DATE / TIME --- */

/* Print a number as two digits with a leading zero. */
static void print_2d(u16 v) {
	dos_putc((char)('0' + (v / 10) % 10));
	dos_putc((char)('0' + v % 10));
}

/* Parse decimal digits, advancing *pp. Returns -1 on no digits. */
static int parse_num(const char **pp) {
	const char *p = *pp;
	int         v = 0;
	int         any = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p - '0');
		any = 1;
		p++;
	}
	*pp = p;
	return any ? v : -1;
}

static void cmd_date(char *arg) {
	static const char *days[7] = {"Sun", "Mon", "Tue", "Wed",
	                              "Thu", "Fri", "Sat"};
	if (*arg == '\0') {
		u16 mmdd = dos_getdate_mmdd();
		u16 year = dos_getdate_year();
		u8  wd = dos_get_weekday();
		print("Current date is ");
		print(days[wd % 7]);
		print(" ");
		print_2d((u16)(mmdd >> 8));
		print("-");
		print_2d((u16)(mmdd & 0xFF));
		print("-");
		print_u32_pad(year, 0);
		crlf();
		return;
	}
	{
		const char *p = arg; /* mm-dd-yyyy */
		int         mon = parse_num(&p);
		int         day, year;
		if (mon < 0 || (*p != '-' && *p != '/')) {
			print("Invalid date\r\n");
			return;
		}
		p++;
		day = parse_num(&p);
		if (day < 0 || (*p != '-' && *p != '/')) {
			print("Invalid date\r\n");
			return;
		}
		p++;
		year = parse_num(&p);
		if (year >= 80 && year <= 99) {
			year += 1900;
		} else if (year >= 0 && year < 80) {
			year += 2000;
		}
		if (year < 0 ||
		    dos_setdate((u16)year, (u16)(((u16)mon << 8) | (u16)day)) != 0) {
			print("Invalid date\r\n");
		}
	}
}

static void cmd_time(char *arg) {
	if (*arg == '\0') {
		u16 hhmm = dos_gettime_hhmm();
		u16 sscc = dos_gettime_sscc();
		print("Current time is ");
		print_2d((u16)(hhmm >> 8));
		print(":");
		print_2d((u16)(hhmm & 0xFF));
		print(":");
		print_2d((u16)(sscc >> 8));
		crlf();
		return;
	}
	{
		const char *p = arg; /* hh:mm[:ss] */
		int         hour = parse_num(&p);
		int         min, sec = 0;
		if (hour < 0 || *p != ':') {
			print("Invalid time\r\n");
			return;
		}
		p++;
		min = parse_num(&p);
		if (min < 0) {
			print("Invalid time\r\n");
			return;
		}
		if (*p == ':') {
			p++;
			sec = parse_num(&p);
			if (sec < 0) {
				print("Invalid time\r\n");
				return;
			}
		}
		if (dos_settime((u16)(((u16)hour << 8) | (u16)min),
		                (u16)((u16)sec << 8)) != 0) {
			print("Invalid time\r\n");
		}
	}
}

/* --- file management commands --- */

/* Terminate the first token of arg and return the second one. */
static char *split_arg(char *arg) {
	char *p = arg;
	while (*p != '\0' && *p != ' ' && *p != '\t') {
		p++;
	}
	if (*p == '\0') {
		return p; /* no second token */
	}
	*p = '\0';
	p++;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	return p;
}

static int sequal_nocase(const char *a, const char *b) {
	u16 i = 0;
	while (a[i] != '\0' && b[i] != '\0') {
		if (upc(a[i]) != upc(b[i])) {
			return 0;
		}
		i++;
	}
	return a[i] == '\0' && b[i] == '\0';
}

/* Length of the directory prefix (up to the last \ / or :). */
static u16 dir_prefix_len(const char *s) {
	u16 i;
	u16 n = 0;
	for (i = 0; s[i] != '\0'; i++) {
		if (s[i] == '\\' || s[i] == '/' || s[i] == ':') {
			n = (u16)(i + 1);
		}
	}
	return n;
}

static void cmd_copy(char *arg) {
	char *dst = split_arg(arg);
	char  dstbuf[80];
	int   hs, hd, n;
	u16   i;
	u8    eof = 0;
	u8    ok = 1;
	if (arg[0] == '\0' || dst[0] == '\0') {
		print("Required parameter missing\r\n");
		return;
	}
	i = slen(dst);
	if (i > 0 && (dst[i - 1] == '\\' || dst[i - 1] == ':')) {
		u16 j = dir_prefix_len(arg); /* dir target: keep the name */
		u16 k;
		for (k = 0; k < i && k < 78; k++) {
			dstbuf[k] = dst[k];
		}
		while (k < 78 && arg[j] != '\0') {
			dstbuf[k++] = arg[j++];
		}
		dstbuf[k] = '\0';
		dst = dstbuf;
	}
	if (sequal_nocase(arg, dst)) {
		print("File cannot be copied onto itself\r\n");
		return;
	}
	hs = dos_open(arg);
	if (hs < 0) {
		print("File not found - ");
		print(arg);
		crlf();
		return;
	}
	hd = dos_create(dst);
	if (hd < 0) {
		dos_close(hs);
		print("File creation error - ");
		print(dst);
		crlf();
		return;
	}
	while (!eof) {
		n = dos_read(hs, iobuf, sizeof(iobuf));
		if (n <= 0) {
			break;
		}
		for (i = 0; i < (u16)n; i++) {
			if (iobuf[i] == 0x1A) { /* ^Z ends COPY CON input */
				n = (int)i;
				eof = 1;
				break;
			}
		}
		if (n > 0 && dos_write(hd, iobuf, (u16)n) != n) {
			print("Insufficient disk space\r\n");
			ok = 0;
			break;
		}
	}
	dos_close(hd);
	dos_close(hs);
	if (ok) {
		print("        1 file(s) copied\r\n");
	}
}

static void cmd_del(char *arg) {
	char path[80];
	u16  dirlen, i, j;
	u16  count = 0;
	if (arg[0] == '\0') {
		print("Required parameter missing\r\n");
		return;
	}
	if (!has_wildcard(arg)) {
		int e = dos_unlink(arg);
		if (e == 5) {
			print("Access denied - ");
			print(arg);
			crlf();
		} else if (e != 0) {
			print("File not found - ");
			print(arg);
			crlf();
		}
		return;
	}
	dirlen = dir_prefix_len(arg);
	if (sequal_nocase(&arg[dirlen], "*.*") ||
	    sequal_nocase(&arg[dirlen], "*")) {
		u8 c;
		print("All files in directory will be deleted!\r\n"
		      "Are you sure (Y/N)?");
		c = dos_getch();
		dos_putc((char)c);
		crlf();
		if (upc((char)c) != 'Y') {
			return;
		}
	}
	dos_setdta(dta);
	if (dos_findfirst(arg, 0) != 0) {
		print("File not found\r\n");
		return;
	}
	do {
		const char *name = (const char *)&dta[30];
		for (i = 0; i < dirlen && i < 66; i++) {
			path[i] = arg[i];
		}
		for (j = 0; name[j] != '\0' && i < 78; j++, i++) {
			path[i] = name[j];
		}
		path[i] = '\0';
		if (dos_unlink(path) == 0) {
			count++;
		}
	} while (dos_findnext() == 0);
	(void)count;
}

static void cmd_ren(char *arg) {
	char  newp[80];
	char *nn = split_arg(arg);
	u16   dirlen, i, j;
	if (arg[0] == '\0' || nn[0] == '\0') {
		print("Required parameter missing\r\n");
		return;
	}
	/* REN keeps the file in its directory: combine old dir + new name */
	dirlen = dir_prefix_len(arg);
	for (i = 0; i < dirlen && i < 66; i++) {
		newp[i] = arg[i];
	}
	for (j = 0; nn[j] != '\0' && i < 78; j++, i++) {
		newp[i] = nn[j];
	}
	newp[i] = '\0';
	if (dos_rename(arg, newp) != 0) {
		print("Duplicate file name or file not found\r\n");
	}
}

static void cmd_md(const char *arg) {
	if (arg[0] == '\0') {
		print("Required parameter missing\r\n");
		return;
	}
	if (dos_mkdir(arg) != 0) {
		print("Unable to create directory\r\n");
	}
}

static void cmd_rd(const char *arg) {
	if (arg[0] == '\0') {
		print("Required parameter missing\r\n");
		return;
	}
	if (dos_rmdir(arg) != 0) {
		print("Invalid path, not directory,\r\n"
		      "or directory not empty\r\n");
	}
}

/* --- external programs and batch files --- */

static void execute(char *ln);

static void run_batch(const char *path) {
	char buf[BATCH_MAX];
	char bline[130];
	int  h, n;
	u16  p = 0;

	if (batch_depth >= BATCH_DEPTH) {
		print("Batch files nested too deep\r\n");
		return;
	}
	h = dos_open(path);
	if (h < 0) {
		print("Batch file not found - ");
		print(path);
		crlf();
		return;
	}
	n = dos_read(h, buf, BATCH_MAX - 1);
	dos_close(h);
	if (n < 0) {
		n = 0;
	}
	buf[n] = '\0';

	batch_depth++;
	while (buf[p] != '\0') {
		u16   i = 0;
		char *cmd;
		u8    quiet = 0;
		while (buf[p] != '\0' && buf[p] != '\r' && buf[p] != '\n' && i < 128) {
			bline[i++] = buf[p++];
		}
		while (buf[p] == '\r' || buf[p] == '\n') {
			p++;
		}
		bline[i] = '\0';
		cmd = bline;
		while (*cmd == ' ' || *cmd == '\t') {
			cmd++;
		}
		if (*cmd == '@') {
			quiet = 1;
			cmd++;
		}
		if (*cmd == '\0') {
			continue;
		}
		if (echo_on && !quiet) {
			print_prompt();
			print(cmd);
			crlf();
		}
		execute(cmd);
		if (shell_break_hit) { /* ^C abandons the batch job */
			break;
		}
	}
	batch_depth--;
}

/* Build the PSP-style tail at tailbuf: DOS programs expect a
 * leading blank before the arguments. */
static void build_tail(const char *args) {
	u8  n = 0;
	u16 i;
	if (args[0] != '\0') {
		tailbuf[1] = (u8)' ';
		n = 1;
		for (i = 0; args[i] != '\0' && n < 126; i++) {
			tailbuf[1 + n] = (u8)args[i];
			n++;
		}
	}
	tailbuf[0] = n;
	tailbuf[1 + n] = 0x0D;
}

static int ends_with_up(const char *s, const char *up) {
	u16 sl = slen(s);
	u16 ul = slen(up);
	u16 i;
	if (sl < ul) {
		return 0;
	}
	for (i = 0; i < ul; i++) {
		if (upc(s[sl - ul + i]) != up[i]) {
			return 0;
		}
	}
	return 1;
}

/* try DIR\WORD with each extension; 0 = ran, 1 = not found,
 * 2 = found but failed (message already printed) */
static int try_run(const char *dir, const char *word, int has_ext) {
	const char *exts[3];
	char        path[80];
	execblk     pb;
	u16         dlen = slen(dir);
	u16         wlen = slen(word);
	u16         i, e, base;

	if ((u16)(dlen + wlen) > 74) { /* path[] would overflow */
		return 1;
	}

	pb.env_seg = env_seg;
	pb.tail_off = (u16)tailbuf;
	pb.tail_seg = get_psp();
	pb.fcb1_off = 0;
	pb.fcb1_seg = 0;
	pb.fcb2_off = 0;
	pb.fcb2_seg = 0;

	exts[0] = has_ext ? "" : ".COM";
	exts[1] = ".EXE";
	exts[2] = ".BAT";
	for (e = 0; e < (has_ext ? 1u : 3u); e++) {
		const char *ext = exts[e];
		u16         rc;
		base = 0;
		for (i = 0; i < dlen; i++) {
			path[base++] = upc(dir[i]);
		}
		if (base != 0 && path[base - 1] != '\\' && path[base - 1] != ':') {
			path[base++] = '\\';
		}
		for (i = 0; i < wlen; i++) {
			path[base++] = upc(word[i]);
		}
		for (; *ext != '\0'; ext++) {
			path[base++] = *ext;
		}
		path[base] = '\0';

		if ((!has_ext && e == 2) || ends_with_up(path, ".BAT")) {
			int h = dos_open(path);
			if (h >= 0) {
				dos_close(h);
				run_batch(path);
				return 0;
			}
			continue;
		}
		rc = dos_exec(path, &pb);
		if (rc == 0) {
			return 0;
		}
		if (rc != 2 && rc != 3) { /* found but failed */
			print("Cannot execute ");
			print(path);
			crlf();
			return 2;
		}
	}
	return 1;
}

static void run_external(const char *word, const char *args) {
	char dirs[128];
	u16  i;
	int  has_ext = 0;
	int  has_path = 0;
	int  rc;

	for (i = 0; word[i] != '\0'; i++) {
		if (word[i] == '.') {
			has_ext = 1;
		} else if (word[i] == '\\') {
			has_ext = 0;
			has_path = 1;
		} else if (word[i] == ':') {
			has_path = 1;
		}
	}
	if (i > 70) {
		print("Bad command or file name\r\n");
		return;
	}

	build_tail(args);

	rc = try_run("", word, has_ext); /* current directory */
	if (rc == 1 && !has_path && env_get("PATH", dirs, sizeof(dirs))) {
		u16 p = 0;
		while (rc == 1 && dirs[p] != '\0') {
			char one[68];
			u16  n = 0;
			while (dirs[p] != '\0' && dirs[p] != ';') {
				if (n < sizeof(one) - 1) {
					one[n++] = dirs[p];
				}
				p++;
			}
			one[n] = '\0';
			if (dirs[p] == ';') {
				p++;
			}
			if (n != 0) {
				rc = try_run(one, word, has_ext);
			}
		}
	}
	if (rc == 1) {
		print("Bad command or file name\r\n");
	}
}

/* --- command line parsing and dispatch --- */

static void execute(char *ln) {
	char *p = ln;
	char *word;
	char *arg;
	char  wordup[80];
	u16   i;

	while (*p == ' ' || *p == '\t') {
		p++;
	}
	if (*p == '\0') {
		return;
	}
	/* "ECHO." prints the rest verbatim (incl. empty line) */
	if (upc(p[0]) == 'E' && upc(p[1]) == 'C' && upc(p[2]) == 'H' &&
	    upc(p[3]) == 'O' && p[4] == '.') {
		print(p + 5);
		crlf();
		return;
	}
	/* drive change: "X:" */
	if (p[1] == ':' && p[2] == '\0') {
		char d = upc(p[0]);
		if (d >= 'A' && d <= 'Z') {
			dos_setdrive((u8)(d - 'A'));
			if (dos_getdrive() != (u8)(d - 'A')) {
				print("Invalid drive specification\r\n");
			}
		} else {
			print("Invalid drive specification\r\n");
		}
		return;
	}

	word = p;
	while (*p != '\0' && *p != ' ' && *p != '\t') {
		p++;
	}
	if (*p != '\0') {
		*p = '\0'; /* terminate the command word */
		p++;
	}
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	arg = p; /* arguments, leading blanks gone */

	for (i = 0; word[i] != '\0' && i < 79; i++) {
		wordup[i] = upc(word[i]);
	}
	wordup[i] = '\0';

	if (sequal_up(wordup, "DIR")) {
		cmd_dir(arg);
		return;
	}
	if (sequal_up(wordup, "CD") || sequal_up(wordup, "CHDIR")) {
		cmd_cd(arg);
		return;
	}
	if (sequal_up(wordup, "TYPE")) {
		cmd_type(arg);
		return;
	}
	if (sequal_up(wordup, "CLS")) {
		bios_cls();
		return;
	}
	if (sequal_up(wordup, "ECHO")) {
		cmd_echo(arg);
		return;
	}
	if (sequal_up(wordup, "SET")) {
		cmd_set(arg);
		return;
	}
	if (sequal_up(wordup, "VER")) {
		cmd_ver();
		return;
	}
	if (sequal_up(wordup, "SETVER")) {
		cmd_setver(arg);
		return;
	}
	if (sequal_up(wordup, "MEM")) {
		cmd_mem();
		return;
	}
	if (sequal_up(wordup, "DATE")) {
		cmd_date(arg);
		return;
	}
	if (sequal_up(wordup, "TIME")) {
		cmd_time(arg);
		return;
	}
	if (sequal_up(wordup, "PAUSE")) {
		cmd_pause();
		return;
	}
	if (sequal_up(wordup, "REM")) {
		return;
	}
	if (sequal_up(wordup, "EXIT")) {
		dos_exit(0);
	}
	if (sequal_up(wordup, "COPY")) {
		cmd_copy(arg);
		return;
	}
	if (sequal_up(wordup, "DEL") || sequal_up(wordup, "ERASE")) {
		cmd_del(arg);
		return;
	}
	if (sequal_up(wordup, "REN") || sequal_up(wordup, "RENAME")) {
		cmd_ren(arg);
		return;
	}
	if (sequal_up(wordup, "MD") || sequal_up(wordup, "MKDIR")) {
		cmd_md(arg);
		return;
	}
	if (sequal_up(wordup, "RD") || sequal_up(wordup, "RMDIR")) {
		cmd_rd(arg);
		return;
	}

	run_external(wordup, arg);
}

/* --- entry point --- */

void shell_main(void) {
	dos_resize(get_psp(), 0x1000); /* keep 64 KB, free the rest */
	env_init();
	dos_setvect23(break_handler); /* Ctrl-C: prompt-aware handler */

	print("\r\n" OS_NAME " Command Interpreter [Version " OS_VERSION "]\r\n");

	{
		char ab[16] = "A:\\AUTOEXEC.BAT";
		int  h;
		ab[0] = (char)('A' + dos_getdrive()); /* boot volume */
		h = dos_open(ab);
		if (h >= 0) {
			dos_close(h);
			run_batch(ab);
		}
	}

	for (;;) {
		u8  n;
		u16 i;
		shell_break_hit = 0;
		crlf();
		print_prompt();
		inbuf[0] = 127;
		dos_readline(inbuf);
		dos_putc('\n'); /* AH=0Ah echoes CR only */
		n = inbuf[1];
		for (i = 0; i < n; i++) {
			line[i] = (char)inbuf[2 + i];
		}
		line[n] = '\0';
		execute(line);
	}
}
