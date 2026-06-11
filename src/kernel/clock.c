/* ============================================================
 * clock.c - date and time
 *
 * INT 21h functions 2Ah-2Dh backed by the BIOS RTC (INT 1Ah).
 * The RTC is read on every call - simple and always right;
 * SET writes go back into the RTC, so they persist the same
 * way real DOS behaves on AT machines.
 *
 * Also provides the FAT timestamp packers used by file.c for
 * created/modified files.
 * ============================================================ */
#include "kernel.h"

/* INT 1Ah AH=02h/04h: RTC time and date in BCD. Watcom pragma
 * aux cannot return arbitrary register pairs reliably, so each
 * half is fetched with its own call (CF -> masked to 0). */
/* clang-format off */
static u16 bios_rtc_time_cx(void);      /* CH=hour CL=min */
#pragma aux bios_rtc_time_cx = \
	"push bp"        \
	"mov ah,2"       \
	"int 1ah"        \
	"sbb ax,ax"      \
	"not ax"         \
	"and cx,ax"      \
	"pop bp"         \
	value [cx] modify [ax dx];

static u16 bios_rtc_time_dx(void);      /* DH=sec DL=DST flag */
#pragma aux bios_rtc_time_dx = \
	"push bp"        \
	"mov ah,2"       \
	"int 1ah"        \
	"sbb ax,ax"      \
	"not ax"         \
	"and dx,ax"      \
	"pop bp"         \
	value [dx] modify [ax cx];

static u16 bios_rtc_date_cx(void);      /* CH=century CL=year */
#pragma aux bios_rtc_date_cx = \
	"push bp"        \
	"mov ah,4"       \
	"int 1ah"        \
	"sbb ax,ax"      \
	"not ax"         \
	"and cx,ax"      \
	"pop bp"         \
	value [cx] modify [ax dx];

static u16 bios_rtc_date_dx(void);      /* DH=month DL=day */
#pragma aux bios_rtc_date_dx = \
	"push bp"        \
	"mov ah,4"       \
	"int 1ah"        \
	"sbb ax,ax"      \
	"not ax"         \
	"and dx,ax"      \
	"pop bp"         \
	value [dx] modify [ax cx];

static void bios_rtc_set_time(u16 cx, u16 dx);
#pragma aux bios_rtc_set_time = \
	"push bp"        \
	"mov ah,3"       \
	"int 1ah"        \
	"pop bp"         \
	parm [cx] [dx] modify [ax];

static void bios_rtc_set_date(u16 cx, u16 dx);
#pragma aux bios_rtc_set_date = \
	"push bp"        \
	"mov ah,5"       \
	"int 1ah"        \
	"pop bp"         \
	parm [cx] [dx] modify [ax];
/* clang-format on */

static u8 bcd2bin(u8 b) {
	return (u8)((b >> 4) * 10 + (b & 15));
}

static u8 bin2bcd(u8 v) {
	return (u8)(((v / 10) << 4) | (v % 10));
}

/* Current date as binary year/month/day. */
static void clock_get_date(u16 *year, u8 *month, u8 *day) {
	u16 cx = bios_rtc_date_cx();
	u16 dx = bios_rtc_date_dx();
	*year = (u16)(bcd2bin((u8)(cx >> 8)) * 100 + bcd2bin((u8)cx));
	*month = bcd2bin((u8)(dx >> 8));
	*day = bcd2bin((u8)dx);
	if (*year < 1980 || *month == 0 || *month > 12 || *day == 0) {
		*year = 1980; /* RTC stopped or nonsense */
		*month = 1;
		*day = 1;
	}
}

static void clock_get_time(u8 *hour, u8 *min, u8 *sec) {
	u16 cx = bios_rtc_time_cx();
	u16 dx = bios_rtc_time_dx();
	*hour = bcd2bin((u8)(cx >> 8));
	*min = bcd2bin((u8)cx);
	*sec = bcd2bin((u8)(dx >> 8));
}

/* Sakamoto's day-of-week, 0 = Sunday. */
static u8 day_of_week(u16 y, u8 m, u8 d) {
	static const u8 t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
	if (m < 3) {
		y--;
	}
	return (u8)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

/* --- 2Ah: get date -> CX=year DH=month DL=day AL=weekday --- */
void f2a_getdate(iregs __far *r) {
	u16 year;
	u8  month, day;
	clock_get_date(&year, &month, &day);
	r->cx = year;
	r->dx = (u16)((month << 8) | day);
	set_al(r, day_of_week(year, month, day));
}

/* --- 2Bh: set date CX=year DH=month DL=day -> AL=0/FFh --- */
void f2b_setdate(iregs __far *r) {
	u16 year = r->cx;
	u8  month = (u8)(r->dx >> 8);
	u8  day = (u8)(r->dx & 0xFF);
	if (year < 1980 || year > 2099 || month == 0 || month > 12 || day == 0 ||
	    day > 31) {
		set_al(r, 0xFF);
		return;
	}
	bios_rtc_set_date(
	    (u16)((bin2bcd((u8)(year / 100)) << 8) | bin2bcd((u8)(year % 100))),
	    (u16)((bin2bcd(month) << 8) | bin2bcd(day)));
	set_al(r, 0);
}

/* --- 2Ch: get time -> CH=hour CL=min DH=sec DL=1/100s --- */
void f2c_gettime(iregs __far *r) {
	u8  hour, min, sec;
	u16 ticks = peekw(0x40, 0x6C); /* hundredths from the tick count */
	clock_get_time(&hour, &min, &sec);
	r->cx = (u16)((hour << 8) | min);
	r->dx = (u16)((sec << 8) | (u8)((ticks * 5) % 100));
}

/* --- 2Dh: set time CH=hour CL=min DH=sec -> AL=0/FFh --- */
void f2d_settime(iregs __far *r) {
	u8 hour = (u8)(r->cx >> 8);
	u8 min = (u8)(r->cx & 0xFF);
	u8 sec = (u8)(r->dx >> 8);
	if (hour > 23 || min > 59 || sec > 59) {
		set_al(r, 0xFF);
		return;
	}
	bios_rtc_set_time((u16)((bin2bcd(hour) << 8) | bin2bcd(min)),
	                  (u16)(bin2bcd(sec) << 8));
	set_al(r, 0);
}

/* --- FAT timestamp packers (for file create/modify) --- */

u16 clock_dos_date(void) {
	u16 year;
	u8  month, day;
	clock_get_date(&year, &month, &day);
	return (u16)(((year - 1980) << 9) | (month << 5) | day);
}

u16 clock_dos_time(void) {
	u8 hour, min, sec;
	clock_get_time(&hour, &min, &sec);
	return (u16)((hour << 11) | (min << 5) | (sec >> 1));
}
