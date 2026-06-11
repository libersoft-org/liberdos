/* ============================================================
 * kernel.h - common kernel types and helpers
 *
 * Freestanding (no C library). Watcom tiny memory model:
 * CS = DS = SS, near pointers everywhere, __far only for
 * access outside the kernel segment (IVT, user buffers).
 * ============================================================ */
#ifndef KERNEL_H
#define KERNEL_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

/* Segment the kernel relocates itself to right after boot (the
 * boot sector loads it at 0x1000). Keep in sync with KERNEL_SEG
 * in startup.asm. */
#define KERNEL_SEG 0x0060

/* Build a far pointer from segment:offset. NOTE: do not use with a
 * constant segment of 0 - Watcom treats such pointers as null-based
 * and may drop the segment override; use pokew/peekw for the IVT. */
#define MK_FP(seg, off) ((void __far *)(((u32)(u16)(seg) << 16) | (u16)(off)))

/* Far memory word access with an explicit segment register load.
 * Needed for IVT (segment 0) access, see the MK_FP note above. */
/* clang-format off */
void pokew(u16 seg, u16 off, u16 val);
#pragma aux pokew = \
	"mov es,dx"        \
	"mov es:[bx],ax"   \
	parm [dx] [bx] [ax] modify [es];

u16 peekw(u16 seg, u16 off);
#pragma aux peekw = \
	"mov es,dx"        \
	"mov ax,es:[bx]"   \
	parm [dx] [bx] value [ax] modify [es];

void pokeb(u16 seg, u16 off, u8 val);
#pragma aux pokeb = \
	"mov es,dx"        \
	"mov es:[bx],al"   \
	parm [dx] [bx] [al] modify [es];

u8 peekb(u16 seg, u16 off);
#pragma aux peekb = \
	"mov es,dx"        \
	"mov al,es:[bx]"   \
	parm [dx] [bx] value [al] modify [es];
/* clang-format on */

/* FLAGS bits (as stored in iregs.flags). */
#define FL_CF 0x0001
#define FL_ZF 0x0040

#define FP_SEG(p) ((u16)((u32)(void __far *)(p) >> 16))
#define FP_OFF(p) ((u16)(u32)(void __far *)(p))

/* DOS error codes (returned in AX with CF set). */
#define ERR_INVALID_FUNC      0x01
#define ERR_FILE_NOT_FOUND    0x02
#define ERR_PATH_NOT_FOUND    0x03
#define ERR_TOO_MANY_FILES    0x04
#define ERR_ACCESS_DENIED     0x05
#define ERR_INVALID_HANDLE    0x06
#define ERR_CURRENT_DIR       0x10
#define ERR_NO_MORE_FILES     0x12
#define ERR_SHARING_VIOLATION 0x20
#define ERR_LOCK_VIOLATION    0x21

/* FAT directory entry attributes. */
#define ATTR_READONLY 0x01
#define ATTR_HIDDEN   0x02
#define ATTR_SYSTEM   0x04
#define ATTR_VOLUME   0x08
#define ATTR_DIR      0x10
#define ATTR_ARCHIVE  0x20

/* Register frame built by the INT 21h stub in startup.asm on
 * the caller's stack; the dispatcher receives a far pointer to
 * it. Field order must match the push sequence there. */
typedef struct iregs {
	u16 ax, bx, cx, dx, si, di, bp, ds, es; /* pushed by the stub */
	u16 ip, cs, flags;                      /* pushed by the INT instruction */
} iregs;

/* On-disk FAT directory entry (32 bytes). Only the fields the
 * kernel uses are named; the create/access stamps are lumped
 * into "reserved" until something needs them. */
#pragma pack(push, 1)
typedef struct dirent83 {
	u8  name[11];
	u8  attr;
	u8  reserved[10];
	u16 time; /* last write time */
	u16 date; /* last write date */
	u16 cluster;
	u32 size;
} dirent83;
#pragma pack(pop)

/* --- port I/O and CPU intrinsics (inline, no code emitted elsewhere) --- */
/* clang-format off */
void outb(u16 port, u8 val);
#pragma aux outb = "out dx,al" parm [dx] [al];

u8 inb(u16 port);
#pragma aux inb = "in al,dx" parm [dx] value [al];

void cli(void);
#pragma aux cli = "cli";

void sti(void);
#pragma aux sti = "sti";

void hlt(void);
#pragma aux hlt = "hlt";

u16 get_cs(void);
#pragma aux get_cs = "mov ax,cs" value [ax];
/* clang-format on */

/* --- console.c --- */
void serial_init(void);
void serial_putc(u8 c);
void serial_puts(const char *s);
void serial_put_hex8(u8 v);
void con_putc(u8 c);
void con_puts(const char *s);
void con_put_hex8(u8 v);
void con_put_hex16(u16 v);
void con_put_dec(u16 v);
u8   con_getc(void);
int  con_kbhit(void);
u16  con_peek(void); /* waiting key or FFFFh (no dequeue) */
u16  kbd_peek(void); /* startup.asm: INT 16h AH=01h wrapper */

/* --- Ctrl-C / Ctrl-Break (int21.c + startup.asm) --- */
extern u8 int23_pending;  /* stub epilogue invokes INT 23h */
extern u8 break_abort;    /* terminate was caused by Ctrl-C */
extern u8 ctrl_break_hit; /* latched by the INT 1Bh stub */
void      break_signal(void);
int       break_check(void);

/* --- startup.asm exports --- */
extern u16 int21_user_ss; /* caller SS:SP of the INT 21h in progress */
extern u16 int21_user_sp;
extern u16 kernel_end_off; /* kernel image size in bytes */
extern u16 exec_cs, exec_ip, exec_ss, exec_sp, exec_psp;
extern u16 casemap_off;       /* far-callable RETF stub (country info) */
void       exec_launch(void); /* jump into child process, never returns */
void       term_return(void); /* resume parent from exec_ss:exec_sp */

/* --- util.c --- */
void fmemcpy(void __far *dst, const void __far *src, u16 n);
void fmemset(void __far *dst, u8 val, u16 n);
u16  fstrlen(const char __far *s);

/* --- disk.c --- */
void disk_init(u8 boot_drive);
void disk_set_geometry(u16 spt, u16 heads);
int  disk_read(u8 drv, u32 lba, void __far *buf);
int  disk_write(u8 drv, u32 lba, const void __far *buf);

/* --- fat.c --- */
extern u8    disk_buf[512];
int          fat_mount_floppy(u8 bios_drive);
int          fat_mount_hdd(void);     /* size in MB, -1 = none */
int          fat_select(u8 drv);      /* select volume for cluster ops */
u8           fat_cur_drive(void);     /* currently selected volume */
u8           fat_default_drive(void); /* DOS default drive */
int          fat_set_default(u8 drv);
int          fat_drive_present(u8 drv);
u8           fat_drive_count(void);
int          fat_drive_info(u8 drv, u16 *spc, u16 *clusters, u8 *media);
int          fat_load_sector(u32 lba);
int          fat_store_buf(u32 lba);
u16          fat_next(u16 cl);         /* end-of-chain -> 0xFFFF */
void         fat_set(u16 cl, u16 val); /* val 0xFFFF = end-of-chain */
u16          fat_alloc(u16 prev);
void         fat_free_chain(u16 cl);
int          fat_commit(void);
u16          fat_cluster_bytes(void);
u32          fat_cluster_lba(u16 cl);
u16          fat_max_cluster(void);
u8           fat_secs_per_clus(void);
int          fat_zero_cluster(u16 cl);
int          fat_dir_entry(u16 dir_cluster, u16 index, dirent83 *out);
int          fat_dir_set(u16 dir_cluster, u16 index, const dirent83 *e);
int          fat_dir_alloc_slot(u16 dir_cluster, u16 *out_index);
int          fat_match11(const u8 *pat, const u8 *name);
int          fat_dir_search(u16 dir_cluster, const u8 *name11, dirent83 *out);
int          fat_dir_search_i(u16 dir_cluster, const u8 *name11, dirent83 *out,
                              u16 *out_index);
u16          fat_resolve_dir(const char __far *path, u16 *dir_cl, u8 *last11);
u16          fat_chdir(const char __far *path);
const char  *fat_get_cwd(u8 drv);
u16          fat_cwd_cluster(void);
void __cdecl absdisk_dispatch(iregs __far *r); /* INT 25h/26h core */
extern u8    absdisk_cf; /* status for the stub's live CF */

/* --- clock.c --- */
void f2a_getdate(iregs __far *r);
void f2b_setdate(iregs __far *r);
void f2c_gettime(iregs __far *r);
void f2d_settime(iregs __far *r);
u16  clock_dos_date(void); /* FAT-packed current date */
u16  clock_dos_time(void); /* FAT-packed current time */

/* --- xms.c --- */
u16 xms_init(void);      /* detect ext memory -> pool KB */
u32 xms_reserve(u16 kb); /* kernel-internal block -> linear base, 0 = full */
int xms_copy(u32 dst, u32 src, u32 bytes); /* INT 15h/87h copy, even length */

/* --- ems.c --- */
u16 ems_init(void);  /* probe frame + reserve backing -> EMS KB, 0 = none */
int ems_avail(void); /* nonzero when ems_init succeeded */

/* --- mouse.c --- */
u16  mouse_init(void);      /* 0 = PS/2 mouse present */
u16  mouse_hw_init(void);   /* startup.asm: INT 15h C2xxh setup */
void mouse_call_user(void); /* startup.asm: far-call user handler */

/* --- file.c --- */
void file_init(void);
void file_jft_inherit(u16 psp, u16 parent);
void file_jft_close_all(u16 psp);

/* --- setver.c: per-program reported version table --- */
#define SETVER_MAX 16
typedef struct setver_entry {
	char name[14]; /* uppercase "NAME.EXT", NUL-padded; [0]==0 = free */
	u8   major;
	u8   minor;
} setver_entry; /* 16 bytes - layout shared with the shell */
extern setver_entry setver_table[SETVER_MAX];
void                setver_apply(u16 psp, const char __far *path);

/* --- share.c: built-in SHARE (locks + sharing modes) --- */
u16  share_lock(u8 drv, u16 dcl, u16 idx, u16 psp, u32 start, u32 len);
u16  share_unlock(u8 drv, u16 dcl, u16 idx, u16 psp, u32 start, u32 len);
u16  share_io_check(u8 drv, u16 dcl, u16 idx, u16 psp, u32 start, u32 len);
void share_file_closed(u8 drv, u16 dcl, u16 idx);
void share_psp_closed(u16 psp);
int  share_mode_compat(u8 existing, u8 newmode);
void f5c_lock(iregs __far *r);
void file_get_dta(u16 *seg, u16 *off);
void f0d_diskreset(iregs __far *r);
void f0e_setdrive(iregs __far *r);
void f19_getdrive(iregs __far *r);
void f1a_setdta(iregs __far *r);
void f2f_getdta(iregs __far *r);
void f36_freespace(iregs __far *r);
void f39_mkdir(iregs __far *r);
void f3a_rmdir(iregs __far *r);
void f3b_chdir(iregs __far *r);
void f3c_create(iregs __far *r);
void f3d_open(iregs __far *r);
void f3e_close(iregs __far *r);
void f3f_read(iregs __far *r);
void f40_write(iregs __far *r);
void f41_unlink(iregs __far *r);
void f42_seek(iregs __far *r);
void f43_attrib(iregs __far *r);
void f44_ioctl(iregs __far *r);
void f45_dup(iregs __far *r);
void f46_forcedup(iregs __far *r);
void f47_getcwd(iregs __far *r);
void f4e_findfirst(iregs __far *r);
void f4f_findnext(iregs __far *r);
void f56_rename(iregs __far *r);
void f57_filetimes(iregs __far *r);
void f5b_createnew(iregs __far *r);
void f68_commit(iregs __far *r);

/* path-based cores shared with the FCB layer */
u16 file_unlink_path(const char __far *path); /* 0 or DOS error */
u16 file_rename_path(const char __far *oldp, const char __far *newp);

/* kernel-internal file access (used by EXEC and FCBs) */
int  kfile_open(const char __far *path);   /* handle or -(DOS error) */
int  kfile_create(const char __far *path); /* handle or -(DOS error) */
void kfile_close(int h);
u16  kfile_read(int h, void __far *dst, u16 n);
u16  kfile_write(int h, const void __far *src, u16 n);
void kfile_seek_set(int h, u32 pos);
u32  kfile_size(int h);
void kfile_stamp(int h, u16 *time, u16 *date);

/* --- fcb.c --- */
void f0f_fcb_open(iregs __far *r);
void f10_fcb_close(iregs __far *r);
void f11_fcb_findfirst(iregs __far *r);
void f12_fcb_findnext(iregs __far *r);
void f13_fcb_delete(iregs __far *r);
void f14_fcb_read(iregs __far *r);
void f15_fcb_write(iregs __far *r);
void f16_fcb_create(iregs __far *r);
void f17_fcb_rename(iregs __far *r);
void f21_fcb_randread(iregs __far *r);
void f22_fcb_randwrite(iregs __far *r);
void f27_fcb_blockread(iregs __far *r);
void f28_fcb_blockwrite(iregs __far *r);
void f29_fcb_parse(iregs __far *r);

/* --- proc.c --- */
void proc_init(void);
void proc_set_boot_drive(char letter);        /* patch master env */
void proc_terminate(iregs __far *r, u8 code); /* never returns */
u16  mcb_largest(void);
u16  proc_get_psp(void);
void proc_set_psp(u16 psp);
void f31_tsr(iregs __far *r); /* never returns */
void f48_alloc(iregs __far *r);
void f49_free(iregs __far *r);
void f4a_resize(iregs __far *r);
void f4b_exec(iregs __far *r);
void f4d_exitcode(iregs __far *r);

/* --- int21.c --- */
extern u8    indos_flag; /* nonzero while inside INT 21h */
void         int21_init(void);
void __cdecl int21_dispatch(iregs __far *r);
void         set_al(iregs __far *r, u8 v);
void         int21_error(iregs __far *r, u16 code);

#endif /* KERNEL_H */
