#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "off_t.h"
#include "../threads/synch.h"
#include "../devices/block.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */

/* Block device that contains the file system. */
struct block *fs_device;
extern struct lock filesys_lock;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (block_sector_t dir_sector, const char *name, off_t initial_size) ;
struct file *filesys_open (block_sector_t dir_sector, const char *name);
bool filesys_remove (block_sector_t dir_sector, const char *name);

#endif /* filesys/filesys.h */
