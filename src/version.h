/* ============================================================
 * version.h - single source of the OS name and version
 *
 * C code includes this header; build.ps1 and image.ps1
 * extract OS_NAME / OS_VERSION from it with a regex (build.ps1
 * also generates build\version.inc so boot.asm gets the padded
 * BPB strings). The name lives in exactly one place: here.
 * ============================================================ */
#ifndef VERSION_H
#define VERSION_H

#define OS_NAME    "LiberDOS"
#define OS_VERSION "0.1"

#endif
