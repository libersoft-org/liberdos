/* ============================================================
 * console.c - console input/output
 *
 * BIOS-backed: INT 10h teletype for the screen and BIOS
 * INT 16h for the keyboard. All screen output is mirrored
 * to COM1 so automated QEMU tests can read it from a file.
 * A proper CON device driver will replace this eventually.
 * ============================================================ */
#include "kernel.h"

#define COM1 0x3F8

/* --- BIOS wrappers (push bp: some BIOSes clobber BP) --- */
/* clang-format off */
static void bios_putc(u8 c);
#pragma aux bios_putc = \
	"push bp"            \
	"mov ah,0eh"         \
	"mov bx,7"           \
	"int 10h"            \
	"pop bp"             \
	parm [al] modify [ah bx];

static u16 bios_getkey(void);
#pragma aux bios_getkey = \
	"push bp"             \
	"xor ah,ah"           \
	"int 16h"             \
	"pop bp"              \
	value [ax];

static u16 bios_kbhit(void);
#pragma aux bios_kbhit = \
	"push bp"            \
	"mov ah,1"           \
	"int 16h"            \
	"setnz al"           \
	"mov ah,0"           \
	"pop bp"             \
	value [ax];
/* clang-format on */

/* DOS convention for extended keys: read returns 0 first, the
 * following read returns the scan code. */
static u8 pending_scan = 0;

static const char hexdig[] = "0123456789ABCDEF";

void serial_init(void) {
	outb(COM1 + 1, 0x00); /* disable UART interrupts */
	outb(COM1 + 3, 0x80); /* enable DLAB */
	outb(COM1 + 0, 0x01); /* divisor low: 115200 baud */
	outb(COM1 + 1, 0x00); /* divisor high */
	outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
}

void serial_putc(u8 c) {
	while ((inb(COM1 + 5) & 0x20) == 0) {
	} /* wait for THR empty */
	outb(COM1, c);
}

void serial_puts(const char *s) {
	while (*s != '\0') {
		serial_putc((u8)*s);
		s++;
	}
}

void serial_put_hex8(u8 v) {
	serial_putc((u8)hexdig[(v >> 4) & 0x0F]);
	serial_putc((u8)hexdig[v & 0x0F]);
}

void con_putc(u8 c) {
	bios_putc(c);
	serial_putc(c);
}

void con_puts(const char *s) {
	while (*s != '\0') {
		con_putc((u8)*s);
		s++;
	}
}

void con_put_hex8(u8 v) {
	con_putc((u8)hexdig[(v >> 4) & 0x0F]);
	con_putc((u8)hexdig[v & 0x0F]);
}

void con_put_hex16(u16 v) {
	con_put_hex8((u8)(v >> 8));
	con_put_hex8((u8)(v & 0xFF));
}

void con_put_dec(u16 v) {
	char buf[6];
	u16  i = 0;
	do {
		buf[i++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	while (i != 0) {
		con_putc((u8)buf[--i]);
	}
}

/* Blocking read of one key, no echo. Extended keys are returned
 * as 0 followed by the scan code on the next call (DOS style). */
u8 con_getc(void) {
	u16 k;
	u8  c;
	if (pending_scan != 0) {
		c = pending_scan;
		pending_scan = 0;
		return c;
	}
	k = bios_getkey();
	c = (u8)(k & 0xFF);
	if (c == 0x00 || c == 0xE0) { /* extended / grey key */
		pending_scan = (u8)(k >> 8);
		return 0;
	}
	return c;
}

/* Returns nonzero if a key is waiting. */
int con_kbhit(void) {
	if (pending_scan != 0) {
		return 1;
	}
	return (int)bios_kbhit();
}

/* Non-destructive peek: the waiting key (scan<<8 | char) or
 * FFFFh if the buffer is empty. A pending scan code counts as
 * the next key (it can never be a ^C). */
u16 con_peek(void) {
	if (pending_scan != 0) {
		return (u16)((u16)pending_scan << 8);
	}
	return kbd_peek();
}
