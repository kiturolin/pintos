#include "directory.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys.h"
#include "inode.h"
#include "../threads/malloc.h"
#include "stdbool.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
    // in_use的意思是, 这个entry是否指向了一个已经存在的, 有效的文件? 这个entry是否被用掉了?
    // 而不是 "文件是否正在被使用" !
  };

// 返回路径中最后一个文件的inode的sector编号
// 若未找到, 则返回0
// Example: path_ = /path/to/some/file/ 
// 返回file的inode所在的sector编号
block_sector_t
dir_parse(block_sector_t wd, const char *path_)
{
  // 如果传入的空path或path指针为0
  if (!path_ || !strlen(path_))
    return wd;

  uint32_t path_len = strlen(path_) + 1; 
  char path[64];
  char *token, *save_ptr;
  struct dir *dir = NULL;

  strlcpy(path, path_, path_len);

  // 如果path的首位为"/"
  bool relative = (*path != '/');
  block_sector_t ret = relative ? 0 : ROOT_DIR_SECTOR;
  // 是否为相对路径
  struct inode *inode = relative ? inode_open(wd) : inode_open(ROOT_DIR_SECTOR);
  if (inode == NULL) goto done;
  dir = dir_open(inode);
  if (dir == NULL) goto done;

  // 逐个解码path
  // 如果path = "/"(根目录), 不会进入for循环
  // strtok_r的实现保证了不会有任何token的长度为0, 可以自适应"a///b/c"的情况
  for (token = strtok_r (path, "/", &save_ptr); token != NULL;
        token = strtok_r (NULL, "/", &save_ptr))
  {
    // 前一次循环无法打开dir, 而又运行到了这一次循环
    // 说明path没到头, 而上一个token已经不是directory了(因为dir是NULL),
    // 是错误的, 应该返回0
    if(dir == NULL) goto done;
    // 每次lookup都会打开新的inode, 如果能找到文件的话
    dir_lookup(dir, token, &inode); 
    // dir已经在lookup中用完了, 需要手动关闭
    // 注意, dir_close也同时关闭了旧的inode
    dir_close(dir);
    dir = NULL;
    // 如果在path中找不到名为token的文件, 那么也返回0
    if (inode == NULL) goto done;
    ret = inode->sector;
    // 这里的inode是在lookup中找到的新inode
    dir = dir_open(inode);
  }
  // 得到返回值
  if (dir) dir_close(dir);
  return ret;
done:
  if (dir) dir_close(dir);
  return 0;
}

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, block_sector_t prev, const char *name, size_t entry_cnt)
{
  if(!inode_create (sector, entry_cnt * sizeof (struct dir_entry), true))
    return false;
  struct inode *new_inode = inode_open(sector);
  ASSERT(new_inode != NULL);
  struct dir *new_dir = dir_open(new_inode);
  struct dir *prev_dir = dir_open(inode_open(prev));
  dir_add(new_dir, ".", sector);
  dir_add(new_dir, "..", prev);
  dir_add(prev_dir, name, sector);
  return true;
}


/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
// 为dir分配内存空间, 并将传入的inode设置为dir的inode
// 如果打开的不是一个dir, 返回NULL
struct dir *
dir_open (struct inode *inode) 
{
  // calloc(a, b)分配a * b bytes的内存
  struct dir *dir = calloc (1, sizeof *dir);

  if (inode != NULL && dir != NULL && inode_is_dir(inode))
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
// 这英文原版注释写的什么玩意儿???
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
// 返回指向一个inode的新dir对象
// 可以有多个dir对象指向同一个inode!
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
// 以ep和ofsp作为"返回值" 分别指向一个dir_entry对象和该entry在inode中的offset
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  // 以一个dir_entry的大小为步进, 逐个读取dir中的entry, 并比较文件名
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
// "返回"一个打开的inode, 指向找到的文件
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    // 打开的是dir_entry指向的文件的inode, 而非指向dir的inode!
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  // 将offset更新到下一个空闲的slot (尚未被使用的dir_entry空位)
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  // 如果inode写入失败了, 怎么回收entry的数据?
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
// 从目录中移除一个文件, 先找到文件, 再打开文件对应的inode, 销毁文件inode中的内容
// 再销毁dir_entry ("销毁"指的是将in_use位置低后写回文件)
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  // 将in_use位置低后写回文件
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
// 获得dir中下一个文件的名字, 手动更新dir的offset
// 除了此处和dir_open(), 不准在其他地方修改dir->pos!
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          if (!strcmp(e.name, ".") || !strcmp(e.name, ".."))
            continue;

          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

bool
dir_is_empty(struct dir *dir)
{
  char *name = malloc(NAME_MAX + 1);
  bool is_empty = dir_readdir(dir, name);
  free(name);
  return is_empty;
}
