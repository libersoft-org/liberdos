/* ============================================================
 * dosapi.h - DOS INT 21h wrappers for user programs
 *
 * Standalone (no kernel headers, no C runtime): every DOS call
 * is a #pragma aux inline sequence. Error-returning calls use
 * the branchless sbb trick because pragma aux cannot contain
 * jump labels.
 * ============================================================ */
#ifndef DOSAPI_H
#define DOSAPI_H

/* clang-format off */  /* hand-aligned pragma aux blocks throughout */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define MK_FP(s, o) ((void __far *)(((u32)(s) << 16) | (u16)(o)))

/* --- far memory access (avoids the Watcom seg-0 MK_FP trap) --- */
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

u16 peekw(u16 seg, u16 off);
#pragma aux peekw = \
	"mov es,dx"        \
	"mov ax,es:[bx]"   \
	parm [dx] [bx] value [ax] modify [es];

/* In a .COM program DS == PSP segment for the whole run. */
u16 get_psp(void);
#pragma aux get_psp = \
	"mov ax,ds"       \
	value [ax];

/* --- output --- */
void dos_print(const char *s);          /* 09h: '$'-terminated */
#pragma aux dos_print = \
	"mov ah,9"          \
	"int 21h"           \
	parm [dx] modify [ax];

void dos_putc(char c);                  /* 02h */
#pragma aux dos_putc = \
	"mov ah,2"         \
	"int 21h"          \
	parm [dl] modify [ax];

/* --- input --- */
void dos_readline(void *buf);           /* 0Ah: [0]=max [1]=len */
#pragma aux dos_readline = \
	"mov ah,0ah"           \
	"int 21h"              \
	parm [dx] modify [ax];

u8 dos_getch(void);                     /* 08h: wait, no echo */
#pragma aux dos_getch = \
	"mov ah,8"          \
	"int 21h"           \
	value [al];

/* --- drive / directory --- */
u8 dos_getdrive(void);                  /* 19h: 0 = A: */
#pragma aux dos_getdrive = \
	"mov ah,19h"           \
	"int 21h"              \
	value [al];

void dos_setdrive(u8 d);                /* 0Eh */
#pragma aux dos_setdrive = \
	"mov ah,0eh"           \
	"int 21h"              \
	parm [dl] modify [ax];

int dos_getcwd(char *buf64);            /* 47h: no drive, no lead '\' */
#pragma aux dos_getcwd = \
	"mov ah,47h"         \
	"xor dl,dl"          \
	"int 21h"            \
	"sbb ax,ax"          \
	parm [si] value [ax] modify [dx];

int dos_chdir(const char *path);        /* 3Bh */
#pragma aux dos_chdir = \
	"mov ah,3bh"        \
	"int 21h"           \
	"sbb ax,ax"         \
	parm [dx] value [ax];

/* --- files --- */
int dos_open(const char *path);         /* 3Dh: handle or -1 */
#pragma aux dos_open = \
	"mov ax,3d00h"     \
	"int 21h"          \
	"sbb dx,dx"        \
	"or ax,dx"         \
	parm [dx] value [ax] modify [dx];

int dos_create(const char *path);       /* 3Ch: handle or -1 */
#pragma aux dos_create = \
	"mov ah,3ch"         \
	"xor cx,cx"          \
	"int 21h"            \
	"sbb dx,dx"          \
	"or ax,dx"           \
	parm [dx] value [ax] modify [cx dx];

int dos_unlink(const char *path);       /* 41h: 0 or DOS error code */
#pragma aux dos_unlink = \
	"mov ah,41h"         \
	"int 21h"            \
	"sbb dx,dx"          \
	"and ax,dx"          \
	parm [dx] value [ax] modify [dx];

int dos_rename(const char *oldp, const char *newp);     /* 56h */
#pragma aux dos_rename = \
	"push es"            \
	"mov ax,ds"          \
	"mov es,ax"          \
	"mov ah,56h"         \
	"int 21h"            \
	"sbb ax,ax"          \
	"pop es"             \
	parm [dx] [di] value [ax];

int dos_mkdir(const char *path);        /* 39h: 0 or -1 */
#pragma aux dos_mkdir = \
	"mov ah,39h"        \
	"int 21h"           \
	"sbb ax,ax"         \
	parm [dx] value [ax];

int dos_rmdir(const char *path);        /* 3Ah: 0 or -1 */
#pragma aux dos_rmdir = \
	"mov ah,3ah"        \
	"int 21h"           \
	"sbb ax,ax"         \
	parm [dx] value [ax];

void dos_close(int h);                  /* 3Eh */
#pragma aux dos_close = \
	"mov ah,3eh"        \
	"int 21h"           \
	parm [bx] modify [ax];

int dos_read(int h, void *buf, u16 n);  /* 3Fh: bytes or -1 */
#pragma aux dos_read = \
	"mov ah,3fh"       \
	"int 21h"          \
	"sbb dx,dx"        \
	"or ax,dx"         \
	parm [bx] [dx] [cx] value [ax] modify [dx];

int dos_write(int h, const void *buf, u16 n);   /* 40h */
#pragma aux dos_write = \
	"mov ah,40h"        \
	"int 21h"           \
	"sbb dx,dx"         \
	"or ax,dx"          \
	parm [bx] [dx] [cx] value [ax] modify [dx];

/* --- DTA / FindFirst / FindNext --- */
void dos_setdta(void *dta);             /* 1Ah */
#pragma aux dos_setdta = \
	"mov ah,1ah"         \
	"int 21h"            \
	parm [dx] modify [ax];

int dos_findfirst(const char *pattern, u16 attr);   /* 4Eh */
#pragma aux dos_findfirst = \
	"mov ah,4eh"            \
	"int 21h"               \
	"sbb ax,ax"             \
	parm [dx] [cx] value [ax];

int dos_findnext(void);                 /* 4Fh */
#pragma aux dos_findnext = \
	"mov ah,4fh"           \
	"int 21h"              \
	"sbb ax,ax"            \
	value [ax];

/* --- free disk space: secs/clus * bytes/sec * free clusters.
 * DL: 0 = default drive, 1 = A:, 3 = C:. Both muls stay in 16
 * bits for any sane cluster size, the final product is
 * returned 32-bit in DX:AX. Invalid drive (AX=FFFFh) yields 0
 * via the branch-free sbb/and mask. --- */
u32 dos_freebytes(u8 dl);               /* 36h */
#pragma aux dos_freebytes = \
	"mov ah,36h"            \
	"int 21h"               \
	"cmp ax,0FFFFh"         \
	"sbb si,si"             \
	"and ax,si"             \
	"and bx,si"             \
	"mul cx"                \
	"mul bx"                \
	parm [dx] value [dx ax] modify [bx cx si];

/* --- 36h raw views: (total << 16) | free clusters, and
 * (secs/clus << 16) | bytes/sec. 0 = invalid drive. --- */
u32 dos_clusters(u8 dl);                /* 36h: total:free */
#pragma aux dos_clusters = \
	"mov ah,36h"           \
	"int 21h"              \
	"cmp ax,0FFFFh"        \
	"sbb si,si"            \
	"xchg ax,bx"           \
	"and ax,si"            \
	"and dx,si"            \
	parm [dx] value [dx ax] modify [bx cx si];

u32 dos_clussize(u8 dl);                /* 36h: spc:bps */
#pragma aux dos_clussize = \
	"mov ah,36h"           \
	"int 21h"              \
	"cmp ax,0FFFFh"        \
	"sbb si,si"            \
	"mov dx,ax"            \
	"mov ax,cx"            \
	"and ax,si"            \
	"and dx,si"            \
	parm [dx] value [dx ax] modify [bx cx si];

/* --- file attributes --- */
int dos_getattr(const char *path);      /* 4300h: attr or -1 */
#pragma aux dos_getattr = \
	"mov ax,4300h"        \
	"int 21h"             \
	"sbb si,si"           \
	"mov ax,cx"           \
	"or ax,si"            \
	parm [dx] value [ax] modify [cx si];

int dos_setattr(const char *path, u16 attr);    /* 4301h: 0 or -1 */
#pragma aux dos_setattr = \
	"mov ax,4301h"        \
	"int 21h"             \
	"sbb ax,ax"           \
	parm [dx] [cx] value [ax];

/* --- memory --- */
u16 dos_alloc(u16 paras);               /* 48h: seg or 0 */
#pragma aux dos_alloc = \
	"mov ah,48h"        \
	"int 21h"           \
	"sbb dx,dx"         \
	"not dx"            \
	"and ax,dx"         \
	parm [bx] value [ax] modify [dx];

void dos_free(u16 seg);                 /* 49h */
#pragma aux dos_free = \
	"push es"          \
	"mov es,ax"        \
	"mov ah,49h"       \
	"int 21h"          \
	"pop es"           \
	parm [ax] modify [ax];

int dos_resize(u16 seg, u16 paras);     /* 4Ah */
#pragma aux dos_resize = \
	"push es"            \
	"mov es,ax"          \
	"mov ah,4ah"         \
	"int 21h"            \
	"sbb ax,ax"          \
	"pop es"             \
	parm [ax] [bx] value [ax];

u16 dos_largest_free(void);             /* 48h probe: fail -> BX = max */
#pragma aux dos_largest_free = \
	"mov ah,48h"               \
	"mov bx,0ffffh"            \
	"int 21h"                  \
	value [bx] modify [ax];

/* --- processes --- */

/* EXEC parameter block (4Bh AL=00h) */
#pragma pack(push, 1)
typedef struct execblk {
	u16 env_seg;
	u16 tail_off, tail_seg;
	u16 fcb1_off, fcb1_seg;
	u16 fcb2_off, fcb2_seg;
} execblk;
#pragma pack(pop)

/* returns 0 on success, DOS error code on failure */
u16 dos_exec(const char *path, execblk *pb);
#pragma aux dos_exec = \
	"push es"          \
	"push ds"          \
	"pop es"           \
	"mov ax,4b00h"     \
	"int 21h"          \
	"sbb dx,dx"        \
	"and ax,dx"        \
	"pop es"           \
	parm [dx] [bx] value [ax] modify [cx dx];

u16 dos_exitcode(void);                 /* 4Dh */
#pragma aux dos_exitcode = \
	"mov ah,4dh"           \
	"int 21h"              \
	value [ax];

void dos_exit(u8 code);                 /* 4Ch, never returns */
#pragma aux dos_exit = \
	"mov ah,4ch"       \
	"int 21h"          \
	parm [al] aborts;

u16 dos_version(void);                  /* 30h: AL.AH */
#pragma aux dos_version = \
	"mov ax,3000h"        \
	"int 21h"             \
	value [ax] modify [bx cx];

/* --- date and time (2Ah-2Dh) ---
 * NOTE: pragma aux register-pair returns are unreliable, so
 * each register is fetched with its own INT 21h call. */

u16 dos_getdate_year(void);             /* 2Ah: CX = year */
#pragma aux dos_getdate_year = \
	"mov ah,2ah"               \
	"int 21h"                  \
	value [cx] modify [ax dx];

u16 dos_getdate_mmdd(void);             /* 2Ah: DH = month, DL = day */
#pragma aux dos_getdate_mmdd = \
	"mov ah,2ah"               \
	"int 21h"                  \
	value [dx] modify [ax cx];

u8 dos_get_weekday(void);               /* 2Ah: AL = 0 (Sun) .. 6 (Sat) */
#pragma aux dos_get_weekday = \
	"mov ah,2ah"              \
	"int 21h"                 \
	value [al] modify [cx dx];

u8 dos_setdate(u16 year, u16 mmdd);     /* 2Bh: AL = 0 ok, FFh invalid */
#pragma aux dos_setdate = \
	"mov ah,2bh"          \
	"int 21h"             \
	parm [cx] [dx] value [al];

u16 dos_gettime_hhmm(void);             /* 2Ch: CH = hour, CL = min */
#pragma aux dos_gettime_hhmm = \
	"mov ah,2ch"               \
	"int 21h"                  \
	value [cx] modify [ax dx];

u16 dos_gettime_sscc(void);             /* 2Ch: DH = sec, DL = 1/100 s */
#pragma aux dos_gettime_sscc = \
	"mov ah,2ch"               \
	"int 21h"                  \
	value [dx] modify [ax cx];

u8 dos_settime(u16 hhmm, u16 sscc);     /* 2Dh: AL = 0 ok, FFh invalid */
#pragma aux dos_settime = \
	"mov ah,2dh"          \
	"int 21h"             \
	parm [cx] [dx] value [al];

/* --- BIOS console helpers --- */
void bios_cls(void);                    /* scroll-clear + home cursor */
#pragma aux bios_cls = \
	"push bp"          \
	"mov ax,0600h"     \
	"mov bh,07h"       \
	"xor cx,cx"        \
	"mov dx,184fh"     \
	"int 10h"          \
	"mov ah,02h"       \
	"xor bh,bh"        \
	"xor dx,dx"        \
	"int 10h"          \
	"pop bp"           \
	modify [ax bx cx dx];

/* clang-format on */

#endif /* DOSAPI_H */
