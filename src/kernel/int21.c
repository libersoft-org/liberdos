/* ============================================================
 * int21.c - INT 21h DOS API dispatcher
 *
 * The dispatch table covers the classic DOS API: console I/O
 * (01h-0Ch), FCB functions (0Fh-29h), date/time (2Ah-2Dh),
 * vectors (25h/35h), handle file I/O (3Ch-46h, 57h, 68h),
 * memory (48h-4Ah), EXEC/terminate/TSR (4Bh-4Dh, 31h), find
 * (4Eh/4Fh), and the misc info functions DOS programs probe
 * (30h, 33h, 34h, 37h, 38h, 50h/51h/62h, 58h, 59h).
 *
 * The InDOS flag lives here; the dispatcher raises it on entry
 * and drops it on exit. EXEC and terminate leave through stack
 * switches that never return here, so proc.c clears the flag
 * itself before exec_launch()/term_return().
 * ============================================================ */
#include "kernel.h"

#define TABLE_SIZE 0x70

typedef void (*handler_t)(iregs __far *r);

static handler_t table[TABLE_SIZE];

u8         indos_flag = 0;
static u16 ext_err = 0; /* last error for function 59h */

/* --- Ctrl-C / Ctrl-Break detection ---
 * int23_pending makes the INT 21h stub epilogue invoke the
 * INT 23h handler on the caller's stack (see startup.asm);
 * break_abort marks the resulting 4C00h as a Ctrl-C exit;
 * ctrl_break_hit is latched by the INT 1Bh stub at IRQ time. */
u8 int23_pending = 0;
u8 break_abort = 0;
u8 ctrl_break_hit = 0;

void break_signal(void) {
	con_puts("^C\r\n");
	int23_pending = 1;
}

/* Returns nonzero when a break was seen (and signalled). The
 * pending ^C is consumed; Ctrl-Break also drops the 0000h word
 * the BIOS puts into the keyboard buffer. */
int break_check(void) {
	u16 k;
	if (ctrl_break_hit) {
		ctrl_break_hit = 0;
		if (con_peek() == 0) {
			(void)con_getc();
		}
		break_signal();
		return 1;
	}
	k = con_peek();
	if (k != 0xFFFF && (k & 0xFF) == 3) {
		(void)con_getc();
		break_signal();
		return 1;
	}
	return 0;
}

void set_al(iregs __far *r, u8 v) {
	r->ax = (u16)((r->ax & 0xFF00u) | v);
}

void int21_error(iregs __far *r, u16 code) {
	r->flags |= FL_CF;
	r->ax = code;
	ext_err = code;
}

/* --- 00h: terminate program (exit code 0) --- */
static void f_terminate(iregs __far *r) {
	proc_terminate(r, 0);
}

/* --- 4Ch: terminate with exit code AL --- */
static void f4c_exit(iregs __far *r) {
	proc_terminate(r, (u8)(r->ax & 0xFF));
}

/* --- 01h: character input with echo -> AL --- */
static void f01_input_echo(iregs __far *r) {
	u8 c;
	if (break_check()) {
		set_al(r, 3);
		return;
	}
	c = con_getc();
	if (c == 3) {
		break_signal();
		set_al(r, 3);
		return;
	}
	if (c == 0 && break_check()) { /* Ctrl-Break's 0000h word */
		set_al(r, 3);
		return;
	}
	con_putc(c);
	set_al(r, c);
}

/* --- 02h: character output (DL), returns AL = DL --- */
static void f02_output(iregs __far *r) {
	u8 c = (u8)(r->dx & 0xFF);
	(void)break_check();
	con_putc(c);
	set_al(r, c);
}

/* --- 06h: direct console I/O --- */
static void f06_direct(iregs __far *r) {
	u8 dl = (u8)(r->dx & 0xFF);
	if (dl == 0xFF) { /* input, non-blocking */
		if (con_kbhit()) {
			set_al(r, con_getc());
			r->flags &= (u16)~FL_ZF;
		} else {
			set_al(r, 0);
			r->flags |= FL_ZF;
		}
	} else { /* output */
		con_putc(dl);
		set_al(r, dl);
	}
}

/* --- 07h: character input without echo, no ^C check -> AL --- */
static void f07_input_raw(iregs __far *r) {
	set_al(r, con_getc());
}

/* --- 08h: character input without echo, with ^C check -> AL --- */
static void f08_input_noecho(iregs __far *r) {
	u8 c;
	if (break_check()) {
		set_al(r, 3);
		return;
	}
	c = con_getc();
	if (c == 3) {
		break_signal();
		set_al(r, 3);
		return;
	}
	if (c == 0 && break_check()) {
		set_al(r, 3);
		return;
	}
	set_al(r, c);
}

/* --- 09h: write string at DS:DX terminated by '$' --- */
static void f09_print(iregs __far *r) {
	const u8 __far *s = (const u8 __far *)MK_FP(r->ds, r->dx);
	(void)break_check();
	while (*s != '$') {
		con_putc(*s);
		s++;
	}
	set_al(r, (u8)'$');
}

/* --- 0Ah: buffered line input at DS:DX ---
 * buf[0] = max size (incl. CR), buf[1] = chars read (excl. CR),
 * data from buf[2], terminated with CR. */
static void f0a_readline(iregs __far *r) {
	u8 __far *buf = (u8 __far *)MK_FP(r->ds, r->dx);
	u8        max = buf[0];
	u8        n = 0;
	u8        c;
	if (max == 0) {
		return;
	}
	if (break_check()) {
		buf[1] = 0;
		buf[2] = (u8)'\r';
		return;
	}
	for (;;) {
		c = con_getc();
		if (c == 3) { /* Ctrl-C aborts the line */
			break_signal();
			buf[1] = 0;
			buf[2] = (u8)'\r';
			return;
		}
		if (c == '\r') {
			buf[1] = n;
			buf[2 + n] = (u8)'\r';
			con_putc((u8)'\r'); /* DOS echoes CR only */
			return;
		}
		if (c == 0x08) { /* backspace */
			if (n > 0) {
				n--;
				con_putc(0x08);
				con_putc((u8)' ');
				con_putc(0x08);
			}
			continue;
		}
		if (c == 0) {            /* swallow extended keys */
			if (break_check()) { /* Ctrl-Break's 0000h word */
				buf[1] = 0;
				buf[2] = (u8)'\r';
				return;
			}
			(void)con_getc();
			continue;
		}
		if (n < (u8)(max - 1)) {
			buf[2 + n] = c;
			n++;
			con_putc(c);
		} else {
			con_putc(0x07); /* bell: buffer full */
		}
	}
}

/* --- 0Bh: console input status -> AL = FFh / 00h --- */
static void f0b_status(iregs __far *r) {
	if (break_check()) {
		set_al(r, 0xFF);
		return;
	}
	set_al(r, con_kbhit() ? 0xFF : 0x00);
}

/* --- 0Ch: flush keyboard buffer, then run input function AL --- */
static void f0c_flush(iregs __far *r) {
	u8 sub = (u8)(r->ax & 0xFF);
	while (con_kbhit()) {
		(void)con_getc();
	}
	if (sub == 0x01 || sub == 0x06 || sub == 0x07 || sub == 0x08 ||
	    sub == 0x0A) {
		r->ax = (u16)(((u16)sub << 8) | sub);
		table[sub](r);
	} else {
		set_al(r, 0);
	}
}

/* --- 25h: set interrupt vector AL to DS:DX --- */
static void f25_setvect(iregs __far *r) {
	u16 e = (u16)((r->ax & 0xFF) << 2);
	cli();
	pokew(0, e, r->dx);
	pokew(0, (u16)(e + 2), r->ds);
	sti();
}

/* --- 30h: get DOS version -> AL = major, AH = minor (default
 * 7.0), BX = CX = 0. The version comes from the current PSP
 * (offset 40h), where EXEC may have stored a SETVER override
 * - see setver.c. --- */
static void f30_version(iregs __far *r) {
	u16 psp = proc_get_psp();
	r->ax = psp != 0 ? peekw(psp, 0x40) : DOS_REPORTED_VERSION;
	r->bx = 0;
	r->cx = 0;
}

/* --- 35h: get interrupt vector AL -> ES:BX --- */
static void f35_getvect(iregs __far *r) {
	u16 e = (u16)((r->ax & 0xFF) << 2);
	r->bx = peekw(0, e);
	r->es = peekw(0, (u16)(e + 2));
}

/* --- 1Bh/1Ch: FAT info -> AL=secs/clus CX=512 DX=clusters,
 * DS:BX -> media descriptor byte. 1Bh = default drive, 1Ch =
 * drive DL (0 = default). --- */
static u8 media_id = 0xF0;

static void f1b_fatinfo(iregs __far *r) {
	u8  drv = fat_default_drive();
	u16 spc, clusters;
	if ((r->ax >> 8) == 0x1C) {
		u8 dl = (u8)(r->dx & 0xFF);
		if (dl != 0) {
			drv = (u8)(dl - 1);
		}
	}
	if (fat_drive_info(drv, &spc, &clusters, &media_id) != 0) {
		set_al(r, 0xFF); /* invalid drive */
		return;
	}
	set_al(r, (u8)spc);
	r->cx = 512;
	r->dx = clusters;
	r->ds = get_cs();
	r->bx = (u16)&media_id;
}

/* --- 2Eh/54h: verify flag (always off) --- */
static void f2e_setverify(iregs __far *r) {
	(void)r; /* accepted, writes unverified */
}

static void f54_getverify(iregs __far *r) {
	set_al(r, 0);
}

/* --- 33h: Ctrl-Break flag & friends --- */
static void f33_break(iregs __far *r) {
	u8 sub = (u8)(r->ax & 0xFF);
	switch (sub) {
	case 0x00: /* get: BREAK is off */
	case 0x02: /* swap: old value in DL */
		r->dx = (u16)(r->dx & 0xFF00u);
		return;
	case 0x01: /* set: accepted, ignored */
		return;
	case 0x05: /* boot drive: A: = 1 */
		r->dx = (u16)((r->dx & 0xFF00u) | 1);
		return;
	case 0x06:          /* true DOS version */
		r->bx = DOS_REPORTED_VERSION;
		r->dx = 0;
		return;
	default:
		int21_error(r, ERR_INVALID_FUNC);
		return;
	}
}

/* --- 34h: InDOS flag address -> ES:BX --- */
static void f34_indos(iregs __far *r) {
	r->es = get_cs();
	r->bx = (u16)&indos_flag;
}

/* --- 37h: switch character -> DL = '/' --- */
static void f37_switchar(iregs __far *r) {
	u8 sub = (u8)(r->ax & 0xFF);
	if (sub == 0x00) {
		r->dx = (u16)((r->dx & 0xFF00u) | '/');
		set_al(r, 0);
	} else if (sub == 0x01) {
		set_al(r, 0); /* accepted, ignored */
	} else {
		set_al(r, 0xFF);
	}
}

/* --- 38h: country info (US format) into DS:DX, BX = 1 --- */
static void f38_country(iregs __far *r) {
	u8 __far *p;
	if (r->dx == 0xFFFF) {
		return; /* "set country": accepted */
	}
	p = (u8 __far *)MK_FP(r->ds, r->dx);
	fmemset(p, 0, 34);
	p[0x00] = 0;       /* date format: USA m/d/y */
	p[0x02] = (u8)'$'; /* currency symbol, ASCIZ */
	p[0x07] = (u8)','; /* thousands separator */
	p[0x09] = (u8)'.'; /* decimal separator */
	p[0x0B] = (u8)'-'; /* date separator */
	p[0x0D] = (u8)':'; /* time separator */
	p[0x10] = 2;       /* currency digits */
	p[0x11] = 0;       /* 12-hour clock */
	pokew(r->ds, (u16)(r->dx + 0x12), casemap_off);
	pokew(r->ds, (u16)(r->dx + 0x14), get_cs());
	p[0x16] = (u8)';'; /* data list separator */
	r->bx = 1;         /* country code: USA */
}

/* --- 50h/51h/62h: set/get current PSP --- */
static void f50_setpsp(iregs __far *r) {
	proc_set_psp(r->bx);
}

static void f51_getpsp(iregs __far *r) {
	r->bx = proc_get_psp();
}

/* --- 58h: allocation strategy (first fit, no UMBs) --- */
static void f58_strategy(iregs __far *r) {
	u8 sub = (u8)(r->ax & 0xFF);
	if (sub == 0x00) {
		r->ax = 0; /* first fit low */
	} else if (sub == 0x01) {
		r->ax = 0; /* set: accepted */
	} else if (sub == 0x02) {
		set_al(r, 0); /* UMB link: off */
	} else if (sub == 0x03) {
		r->ax = 0; /* set UMB link: accepted */
	} else {
		int21_error(r, ERR_INVALID_FUNC);
	}
}

/* --- 59h: extended error info --- */
static void f59_exterr(iregs __far *r) {
	r->ax = ext_err;
	r->bx = 0x0101; /* class: resource, action: retry */
	r->cx = (u16)((r->cx & 0x00FFu) | 0x0100); /* locus: unknown */
}

/* 5Ch lock/unlock region lives in file.c (handle access). */

void int21_init(void) {
	int i;
	for (i = 0; i < TABLE_SIZE; i++) {
		table[i] = 0;
	}
	table[0x00] = f_terminate;
	table[0x01] = f01_input_echo;
	table[0x02] = f02_output;
	table[0x06] = f06_direct;
	table[0x07] = f07_input_raw;
	table[0x08] = f08_input_noecho;
	table[0x09] = f09_print;
	table[0x0A] = f0a_readline;
	table[0x0B] = f0b_status;
	table[0x0C] = f0c_flush;
	table[0x0D] = f0d_diskreset;
	table[0x0E] = f0e_setdrive;
	table[0x0F] = f0f_fcb_open;
	table[0x10] = f10_fcb_close;
	table[0x11] = f11_fcb_findfirst;
	table[0x12] = f12_fcb_findnext;
	table[0x13] = f13_fcb_delete;
	table[0x14] = f14_fcb_read;
	table[0x15] = f15_fcb_write;
	table[0x16] = f16_fcb_create;
	table[0x17] = f17_fcb_rename;
	table[0x19] = f19_getdrive;
	table[0x1A] = f1a_setdta;
	table[0x1B] = f1b_fatinfo;
	table[0x1C] = f1b_fatinfo;
	table[0x21] = f21_fcb_randread;
	table[0x22] = f22_fcb_randwrite;
	table[0x25] = f25_setvect;
	table[0x27] = f27_fcb_blockread;
	table[0x28] = f28_fcb_blockwrite;
	table[0x29] = f29_fcb_parse;
	table[0x2A] = f2a_getdate;
	table[0x2B] = f2b_setdate;
	table[0x2C] = f2c_gettime;
	table[0x2D] = f2d_settime;
	table[0x2E] = f2e_setverify;
	table[0x2F] = f2f_getdta;
	table[0x30] = f30_version;
	table[0x31] = f31_tsr;
	table[0x33] = f33_break;
	table[0x34] = f34_indos;
	table[0x35] = f35_getvect;
	table[0x36] = f36_freespace;
	table[0x37] = f37_switchar;
	table[0x38] = f38_country;
	table[0x39] = f39_mkdir;
	table[0x3A] = f3a_rmdir;
	table[0x3B] = f3b_chdir;
	table[0x3C] = f3c_create;
	table[0x3D] = f3d_open;
	table[0x3E] = f3e_close;
	table[0x3F] = f3f_read;
	table[0x40] = f40_write;
	table[0x41] = f41_unlink;
	table[0x42] = f42_seek;
	table[0x43] = f43_attrib;
	table[0x44] = f44_ioctl;
	table[0x45] = f45_dup;
	table[0x46] = f46_forcedup;
	table[0x47] = f47_getcwd;
	table[0x48] = f48_alloc;
	table[0x49] = f49_free;
	table[0x4A] = f4a_resize;
	table[0x4B] = f4b_exec;
	table[0x4C] = f4c_exit;
	table[0x4D] = f4d_exitcode;
	table[0x4E] = f4e_findfirst;
	table[0x4F] = f4f_findnext;
	table[0x50] = f50_setpsp;
	table[0x51] = f51_getpsp;
	table[0x54] = f54_getverify;
	table[0x56] = f56_rename;
	table[0x57] = f57_filetimes;
	table[0x58] = f58_strategy;
	table[0x59] = f59_exterr;
	table[0x5B] = f5b_createnew;
	table[0x5C] = f5c_lock;
	table[0x62] = f51_getpsp;
	table[0x68] = f68_commit;
}

void __cdecl int21_dispatch(iregs __far *r) {
	u8 fn = (u8)(r->ax >> 8);
	indos_flag = 1;
	r->flags &= (u16)~FL_CF; /* default: success */
	if (fn < TABLE_SIZE && table[fn] != 0) {
		table[fn](r);
		indos_flag = 0;
		return;
	}
	serial_puts("[int21] unimplemented AH=");
	serial_put_hex8(fn);
	serial_puts("\r\n");
	r->flags |= FL_CF;
	r->ax = 0x0001; /* error: invalid function */
	ext_err = 0x0001;
	indos_flag = 0;
}
