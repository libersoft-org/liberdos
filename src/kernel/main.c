/* ============================================================
 * main.c - kernel initialization
 *
 * Installs the INT 20h-24h/2Fh vectors, mounts the boot floppy
 * (and the first FAT partition of hard disk 80h as C:, when
 * present), processes A:\CONFIG.SYS (SHELL= is honoured,
 * DEVICE= gets a warning, the classic tuning keywords are
 * accepted silently) and launches the command interpreter. The
 * root shell is permanent: if it ever exits, it is restarted.
 * ============================================================ */
#include "kernel.h"
#include "../version.h"

/* Image offsets of the INT entry stubs, exported from
 * startup.asm KDATA. Segment for the IVT entries is CS. */
extern u16 int21_entry_off;
extern u16 int20_entry_off;
extern u16 int22_stub_off;
extern u16 int23_stub_off;
extern u16 int24_stub_off;
extern u16 int25_entry_off;
extern u16 int26_entry_off;
extern u16 int2f_entry_off;
extern u16 int33_entry_off;
extern u16 int67_entry_off;
extern u16 int1b_entry_off;

static char shell_path[64] = "A:\\COMMAND.COM";
static char config_path[16] = "A:\\CONFIG.SYS";
static char cfgbuf[1024];

/* The kernel launches the shell through the real INT 21h
 * instruction, exactly like a user program would. ES:BX = 0:0
 * means "no parameter block" (fresh master environment).
 * Returns 0 on success, the DOS error code on failure. */
/* clang-format off */
static u16 dos_exec_noblk(const char *path);
#pragma aux dos_exec_noblk = \
	"push es"          \
	"xor bx,bx"        \
	"mov es,bx"        \
	"mov ax,4b00h"     \
	"int 21h"          \
	"sbb dx,dx"        \
	"and ax,dx"        \
	"pop es"           \
	parm [dx] value [ax] modify [bx cx dx];
/* clang-format on */

static void install_vector(u8 vec, u16 off) {
	u16 e = (u16)((u16)vec << 2);
	cli();
	pokew(0, e, off);
	pokew(0, (u16)(e + 2), get_cs());
	sti();
}

/* True when the line starts with the keyword followed by '='. */
static int cfg_key(const char *line, const char *key, u16 *valpos) {
	u16 i = 0;
	while (key[i] != '\0') {
		char c = line[i];
		if (c >= 'a' && c <= 'z') {
			c -= 32;
		}
		if (c != key[i]) {
			return 0;
		}
		i++;
	}
	if (line[i] != '=') {
		return 0;
	}
	*valpos = (u16)(i + 1);
	return 1;
}

static void config_process(void) {
	static const char *ignored[] = {
	    "FILES",  "BUFFERS", "LASTDRIVE", "BREAK",     "FCBS",
	    "STACKS", "DOS",     "COUNTRY",   "SHELLHIGH", 0};
	int h = kfile_open(config_path);
	u16 n, pos;
	if (h < 0) {
		return; /* no CONFIG.SYS: fine */
	}
	n = kfile_read(h, cfgbuf, sizeof(cfgbuf) - 1);
	kfile_close(h);
	cfgbuf[n] = '\0';
	pos = 0;
	while (pos < n) {
		char *line = &cfgbuf[pos];
		u16   v, i;
		/* terminate this line, find the next one */
		while (pos < n && cfgbuf[pos] != '\r' && cfgbuf[pos] != '\n') {
			pos++;
		}
		while (pos < n && (cfgbuf[pos] == '\r' || cfgbuf[pos] == '\n')) {
			cfgbuf[pos] = '\0';
			pos++;
		}
		if (line[0] == '\0' || line[0] == ';') {
			continue;
		}
		if ((line[0] == 'R' || line[0] == 'r') &&
		    (line[1] == 'E' || line[1] == 'e') &&
		    (line[2] == 'M' || line[2] == 'm') &&
		    (line[3] == ' ' || line[3] == '\0')) {
			continue;
		}
		if (cfg_key(line, "SHELL", &v)) {
			for (i = 0; line[v + i] != '\0' && line[v + i] != ' ' &&
			            i < sizeof(shell_path) - 1;
			     i++) {
				shell_path[i] = line[v + i];
			}
			if (i > 0) {
				shell_path[i] = '\0';
			}
			continue;
		}
		if (cfg_key(line, "DEVICE", &v) || cfg_key(line, "DEVICEHIGH", &v)) {
			con_puts("[config] DEVICE drivers not supported: ");
			con_puts(&line[v]);
			con_puts("\r\n");
			continue;
		}
		for (i = 0; ignored[i] != 0; i++) {
			if (cfg_key(line, ignored[i], &v)) {
				break; /* accepted silently */
			}
		}
	}
}

void __cdecl kernel_main(u16 boot_drive) {
	int hdd_mb;
	u8  hdd_boot = (u8)(boot_drive >= 0x80);
	serial_init();
	con_puts("\r\n" OS_NAME " " OS_VERSION "\r\n");
	con_puts("[boot] drive ");
	con_put_hex8((u8)boot_drive);
	con_puts("\r\n");

	int21_init();
	install_vector(0x20, int20_entry_off);
	install_vector(0x21, int21_entry_off);
	install_vector(0x22, int22_stub_off);
	install_vector(0x23, int23_stub_off);
	install_vector(0x24, int24_stub_off);
	install_vector(0x25, int25_entry_off);
	install_vector(0x26, int26_entry_off);
	install_vector(0x2F, int2f_entry_off);
	install_vector(0x33, int33_entry_off);
	install_vector(0x1B, int1b_entry_off); /* BIOS Ctrl-Break latch */

	con_puts("[xms] ");
	con_put_dec(xms_init());
	con_puts(" KB extended memory\r\n");

	{
		u16 ems_kb = ems_init();
		if (ems_kb != 0) {
			/* The kernel runs in the HMA, so INT 67h cannot point at
			 * the kernel segment directly: EMS clients read the
			 * "EMMXXXX0" signature at vector_seg:000Ah, which in the
			 * HMA would land in ROM (0xFFFF:000Ah). Install a tiny low
			 * stub instead - a far JMP to the HMA handler plus the
			 * signature - and aim INT 67h at it. */
			static const char emm_sig[8] = {'E', 'M', 'M', 'X',
			                                'X', 'X', 'X', '0'};
			u16               i;
			pokeb(EMS_STUB_SEG, 0x00, 0xEA); /* JMP FAR off:seg */
			pokew(EMS_STUB_SEG, 0x01, int67_entry_off);
			pokew(EMS_STUB_SEG, 0x03, get_cs()); /* HMA kernel segment */
			for (i = 0; i < 8; i++) {
				pokeb(EMS_STUB_SEG, (u16)(0x0A + i), (u8)emm_sig[i]);
			}
			pokew(0, 0x67 * 4, 0x0000); /* INT 67h -> EMS_STUB_SEG:0 */
			pokew(0, 0x67 * 4 + 2, EMS_STUB_SEG);
			con_puts("[ems] ");
			con_put_dec(ems_kb);
			con_puts(" KB expanded memory, frame D000\r\n");
		}
	}

	if (mouse_init() == 0) {
		con_puts("[mouse] PS/2 mouse on INT 33h\r\n");
	}

	disk_init(hdd_boot ? 0 : (u8)boot_drive);
	if (fat_mount_floppy(hdd_boot ? 0 : (u8)boot_drive) != 0 && !hdd_boot) {
		con_puts("[kernel] boot floppy mount FAILED, halting\r\n");
		for (;;) {
			hlt();
		}
	}
	hdd_mb = fat_mount_hdd();
	if (hdd_mb >= 0) {
		con_puts("[disk] C: FAT16 ");
		con_put_dec((u16)hdd_mb);
		con_puts(" MB\r\n");
	}
	if (hdd_boot) {
		if (hdd_mb < 0) {
			con_puts("[kernel] boot hard disk mount FAILED, halting\r\n");
			for (;;) {
				hlt();
			}
		}
		fat_set_default(2); /* boot volume = C: */
		shell_path[0] = 'C';
		config_path[0] = 'C';
		proc_set_boot_drive('C');
	}
	file_init();
	proc_init();
	config_process();
	con_puts("[kernel] boot volume mounted, ");
	con_put_dec((u16)(mcb_largest() >> 6));
	con_puts(" KB free\r\n");

	for (;;) {
		if (dos_exec_noblk(shell_path) != 0) {
			con_puts("[kernel] cannot load shell, halting\r\n");
			for (;;) {
				hlt();
			}
		}
		con_puts("\r\n[kernel] shell exited, restarting\r\n");
	}
}
