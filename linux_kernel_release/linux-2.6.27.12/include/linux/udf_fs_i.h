/*
 * udf_fs_i.h
 *
 * This file is intended for the Linux kernel/module. 
 *
 * COPYRIGHT
 *	This file is distributed under the terms of the GNU General Public
 *	License (GPL). Copies of the GPL can be obtained from:
 *		ftp://prep.ai.mit.edu/pub/gnu/GPL
 *	Each contributing author retains all rights to their own work.
 */
#ifndef _UDF_FS_I_H
#define _UDF_FS_I_H 1

/* exported IOCTLs, we have 'l', 0x40-0x7f */
#define UDF_GETEASIZE   _IOR('l', 0x40, int)
#define UDF_GETEABLOCK  _IOR('l', 0x41, void *)
#define UDF_GETVOLIDENT _IOR('l', 0x42, void *)
#define UDF_RELOCATE_BLOCKS _IOWR('l', 0x43, long)
#ifdef CONFIG_MV88DE3010_BERLIN_UDF_SPEEDUP 
#define CONFIG_MV88DE3010_BERLIN_UDF_GET_LBA4FILE
#endif
#ifdef CONFIG_MV88DE3010_BERLIN_UDF_GET_LBA4FILE
#define UDF_GETFILELBA  _IOR('l', 0x44, void *)
#endif
#endif /* _UDF_FS_I_H */
