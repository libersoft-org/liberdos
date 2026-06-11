/* ============================================================
 * mouse.c - INT 33h mouse services over the PS/2 BIOS
 *
 * The BIOS pointing-device interface (INT 15h C2xxh) delivers
 * 3-byte PS/2 packets to a callback in startup.asm, which
 * forwards them here. State follows the Microsoft driver:
 * virtual 640x200 coordinate space in text mode, mickey
 * counters, press/release latches, show/hide depth counter and
 * an optional user event handler (function 0Ch). The cursor is
 * drawn in text modes only (attribute invert); graphics-mode
 * games read mickeys/buttons and draw their own.
 *
 * Dispatch runs on the CALLER's stack (see startup.asm) so a
 * user event handler may itself call INT 33h.
 * ============================================================ */
#include "kernel.h"

typedef signed char s8;
typedef short       s16;

/* Parameter block for mouse_call_user (startup.asm reads these
 * into registers before far-calling the user handler). */
u16 muh_mask;
u16 muh_buttons;
u16 muh_x;
u16 muh_y;
u16 muh_mx;
u16 muh_my;
u32 muh_handler;

static u8 present;

/* Everything functions 16h/17h must save/restore, in one blob. */
typedef struct mstate {
	s16 px, py; /* cursor position (virtual coords) */
	s16 min_x, max_x;
	s16 min_y, max_y;
	s16 mick_x, mick_y;   /* raw motion accumulators (fn 0Bh) */
	s16 frac_x, frac_y;   /* sub-pixel remainders */
	u16 ratio_x, ratio_y; /* mickeys per 8 pixels */
	s8  visible;          /* 0 = shown, negative = hide depth */
	u8  buttons;
	u16 press_cnt[3], press_x[3], press_y[3];
	u16 rel_cnt[3], rel_x[3], rel_y[3];
	u16 uh_mask;
	u32 uh_handler;
} mstate;

static mstate ms;

/* --- text mode cursor (attribute invert at the cell) --- */
static u8  cur_drawn;
static u16 cur_cell; /* cell index in video page 0 */
static u8  cur_attr; /* attribute byte under the cursor */

static u16 vid_seg(void) {
	return (u16)(peekb(0x40, 0x49) == 7 ? 0xB000 : 0xB800);
}

static u8 text_mode(void) {
	u8 m = peekb(0x40, 0x49);
	return (u8)(m <= 3 || m == 7);
}

static void cur_undraw(void) {
	if (cur_drawn) {
		pokeb(vid_seg(), cur_cell * 2 + 1, cur_attr);
		cur_drawn = 0;
	}
}

static void cur_draw(void) {
	u16 cols, cell;
	u8  a;
	if (ms.visible < 0 || !text_mode()) {
		return;
	}
	cols = peekw(0x40, 0x4A);
	cell = (u16)(ms.py / 8) * cols + (u16)(ms.px / 8);
	if (cur_drawn && cell == cur_cell) {
		return;
	}
	cur_undraw();
	a = peekb(vid_seg(), cell * 2 + 1);
	cur_attr = a;
	cur_cell = cell;
	pokeb(vid_seg(), cell * 2 + 1, (u8)((a >> 4) | (a << 4)));
	cur_drawn = 1;
}

static s16 clamp(s16 v, s16 lo, s16 hi) {
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

static void reset_state(void) {
	u8 i;
	cur_undraw();
	ms.min_x = 0;
	ms.max_x = 639;
	ms.min_y = 0;
	ms.max_y = 199;
	ms.px = 320;
	ms.py = 100;
	ms.mick_x = ms.mick_y = 0;
	ms.frac_x = ms.frac_y = 0;
	ms.ratio_x = 8; /* MS defaults: 8 h / 16 v */
	ms.ratio_y = 16;
	ms.visible = -1;
	ms.buttons = 0;
	for (i = 0; i < 3; i++) {
		ms.press_cnt[i] = ms.press_x[i] = ms.press_y[i] = 0;
		ms.rel_cnt[i] = ms.rel_x[i] = ms.rel_y[i] = 0;
	}
	ms.uh_mask = 0;
	ms.uh_handler = 0;
}

/* Called by main.c at boot. Returns 0 when a PS/2 mouse is up. */
u16 mouse_init(void) {
	reset_state();
	present = (u8)(mouse_hw_init() == 0);
	return (u16)(present ? 0 : 1);
}

/* ------------------------------------------------------------
 * PS/2 packet from the BIOS callback. status bits: 0-2 buttons
 * L/R/M, 4 = X sign, 5 = Y sign. PS/2 Y grows upward, the DOS
 * coordinate space grows downward.
 * ------------------------------------------------------------ */
void __cdecl mouse_event(u16 status, u16 dx, u16 dy) {
	s16 sdx, sdy, step;
	u8  nb, i, diff;
	u16 events = 0;

	if (!present) {
		return;
	}
	sdx = (s16)(dx & 0xFF);
	if (status & 0x10) {
		sdx -= 256;
	}
	sdy = (s16)(dy & 0xFF);
	if (status & 0x20) {
		sdy -= 256;
	}
	sdy = (s16)-sdy;

	if (sdx != 0 || sdy != 0) {
		events |= 0x01;
		ms.mick_x += sdx;
		ms.mick_y += sdy;
		ms.frac_x += (s16)(sdx * 8);
		step = (s16)(ms.frac_x / (s16)ms.ratio_x);
		ms.frac_x -= (s16)(step * (s16)ms.ratio_x);
		ms.px = clamp((s16)(ms.px + step), ms.min_x, ms.max_x);
		ms.frac_y += (s16)(sdy * 8);
		step = (s16)(ms.frac_y / (s16)ms.ratio_y);
		ms.frac_y -= (s16)(step * (s16)ms.ratio_y);
		ms.py = clamp((s16)(ms.py + step), ms.min_y, ms.max_y);
		cur_draw();
	}

	nb = (u8)(status & 7);
	diff = (u8)(nb ^ ms.buttons);
	for (i = 0; i < 3; i++) {
		u8 bit = (u8)(1 << i);
		if (!(diff & bit)) {
			continue;
		}
		if (nb & bit) {
			ms.press_cnt[i]++;
			ms.press_x[i] = (u16)ms.px;
			ms.press_y[i] = (u16)ms.py;
			events |= (u16)(2 << (i * 2)); /* press bits 1/3/5 */
		} else {
			ms.rel_cnt[i]++;
			ms.rel_x[i] = (u16)ms.px;
			ms.rel_y[i] = (u16)ms.py;
			events |= (u16)(4 << (i * 2)); /* release bits 2/4/6 */
		}
	}
	ms.buttons = nb;

	if (ms.uh_handler != 0 && (events & ms.uh_mask) != 0) {
		muh_mask = events;
		muh_buttons = ms.buttons;
		muh_x = (u16)ms.px;
		muh_y = (u16)ms.py;
		muh_mx = (u16)ms.mick_x;
		muh_my = (u16)ms.mick_y;
		muh_handler = ms.uh_handler;
		mouse_call_user();
	}
}

static void state_copy_out(u16 seg, u16 off) {
	u8 __far *d = (u8 __far *)MK_FP(seg, off);
	const u8 *s = (const u8 *)&ms;
	u16       i;
	for (i = 0; i < sizeof(mstate); i++) {
		d[i] = s[i];
	}
}

static void state_copy_in(u16 seg, u16 off) {
	const u8 __far *s = (const u8 __far *)MK_FP(seg, off);
	u8             *d = (u8 *)&ms;
	u16             i;
	cur_undraw();
	for (i = 0; i < sizeof(mstate); i++) {
		d[i] = s[i];
	}
	cur_draw();
}

/* ------------------------------------------------------------
 * INT 33h dispatcher. Unknown functions leave registers as
 * they were (matching the Microsoft driver's behaviour).
 * ------------------------------------------------------------ */
void __cdecl mouse_dispatch(iregs __far *r) {
	u16 i;
	u32 old;

	switch (r->ax) {
	case 0x0000: /* reset and detect */
		if (!present) {
			r->ax = 0;
			return;
		}
		reset_state();
		r->ax = 0xFFFF;
		r->bx = 2; /* two-button mouse */
		return;
	case 0x0001: /* show cursor */
		if (ms.visible < 0) {
			ms.visible++;
		}
		cur_draw();
		return;
	case 0x0002: /* hide cursor */
		if (ms.visible == 0) {
			cur_undraw();
		}
		ms.visible--;
		return;
	case 0x0003: /* position and buttons */
		r->bx = ms.buttons;
		r->cx = (u16)ms.px;
		r->dx = (u16)ms.py;
		return;
	case 0x0004: /* set position */
		ms.px = clamp((s16)r->cx, ms.min_x, ms.max_x);
		ms.py = clamp((s16)r->dx, ms.min_y, ms.max_y);
		if (ms.visible == 0) {
			cur_draw();
		}
		return;
	case 0x0005: /* button press info */
		i = (u16)(r->bx & 3);
		if (i > 2) {
			i = 2;
		}
		r->ax = ms.buttons;
		r->bx = ms.press_cnt[i];
		r->cx = ms.press_x[i];
		r->dx = ms.press_y[i];
		ms.press_cnt[i] = 0;
		return;
	case 0x0006: /* button release info */
		i = (u16)(r->bx & 3);
		if (i > 2) {
			i = 2;
		}
		r->ax = ms.buttons;
		r->bx = ms.rel_cnt[i];
		r->cx = ms.rel_x[i];
		r->dx = ms.rel_y[i];
		ms.rel_cnt[i] = 0;
		return;
	case 0x0007: /* horizontal range */
		ms.min_x = (s16)r->cx;
		ms.max_x = (s16)r->dx;
		if (ms.min_x > ms.max_x) {
			s16 t = ms.min_x;
			ms.min_x = ms.max_x;
			ms.max_x = t;
		}
		ms.px = clamp(ms.px, ms.min_x, ms.max_x);
		return;
	case 0x0008: /* vertical range */
		ms.min_y = (s16)r->cx;
		ms.max_y = (s16)r->dx;
		if (ms.min_y > ms.max_y) {
			s16 t = ms.min_y;
			ms.min_y = ms.max_y;
			ms.max_y = t;
		}
		ms.py = clamp(ms.py, ms.min_y, ms.max_y);
		return;
	case 0x000B: /* read motion counters */
		r->cx = (u16)ms.mick_x;
		r->dx = (u16)ms.mick_y;
		ms.mick_x = 0;
		ms.mick_y = 0;
		return;
	case 0x000C: /* set user event handler */
		ms.uh_mask = r->cx;
		ms.uh_handler = ((u32)r->es << 16) | r->dx;
		return;
	case 0x000F: /* mickeys per 8 pixels */
		if (r->cx != 0) {
			ms.ratio_x = r->cx;
		}
		if (r->dx != 0) {
			ms.ratio_y = r->dx;
		}
		return;
	case 0x0010: /* conditional off region */
	case 0x0013: /* double-speed threshold */
	case 0x001C: /* set sample rate */
		return;  /* accepted, no effect */
	case 0x0014: /* exchange event handlers */
		old = ms.uh_handler;
		i = ms.uh_mask;
		ms.uh_mask = r->cx;
		ms.uh_handler = ((u32)r->es << 16) | r->dx;
		r->cx = i;
		r->dx = (u16)old;
		r->es = (u16)(old >> 16);
		return;
	case 0x0015: /* state buffer size */
		r->bx = (u16)sizeof(mstate);
		return;
	case 0x0016: /* save state to ES:DX */
		state_copy_out(r->es, r->dx);
		return;
	case 0x0017: /* restore state from ES:DX */
		state_copy_in(r->es, r->dx);
		return;
	case 0x0021: /* software reset */
		if (present) {
			reset_state();
			r->ax = 0xFFFF;
			r->bx = 2;
		}
		return;
	case 0x0024:        /* version / mouse type */
		r->bx = 0x0820; /* reports driver 8.20 */
		r->cx = 0x0400; /* PS/2 mouse */
		return;
	default:
		return;
	}
}
