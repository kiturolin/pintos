#include "page.h"
#include "frame.h"
#include <hash.h>
#include "../threads/malloc.h"
#include "../filesys/file.h"
#include "../filesys/filesys.h"
#include "../threads/synch.h"
#include "../userprog/pagedir.h"
#include <list.h>
#include "stdbool.h"
#include "virtual-memory.h"
#include <stdint.h>
#include <stdio.h>

#define USE_ADDR -1
#define USE_MAPID 0x0

struct hash process_list;
struct lock process_list_lock;

static struct mmap_vma_node *page_mmap_seek(struct thread *t, mapid_t mapid, const void *addr);
void page_free_multiple(struct thread *t, const void *begin, const void *end);

static unsigned 
page_process_hash_hash(const struct hash_elem *elem, void *aux UNUSED)
{
  struct process_node *node = hash_entry(elem, struct process_node, helem);
  return hash_bytes(&node->pid, sizeof(pid_t));
}

static bool 
page_process_hash_less(const struct hash_elem *e1, const struct hash_elem *e2, void *aux UNUSED)
{
  struct process_node *node1 = hash_entry(e1, struct process_node, helem);
  struct process_node *node2 = hash_entry(e2, struct process_node, helem);

  return (node1->pid < node2->pid ? true : false);
}

static unsigned 
page_hash_hash(const struct hash_elem *elem, void *aux UNUSED)
{
  struct page_node *node = hash_entry(elem, struct page_node, helem);
  return hash_bytes(&node->upage, sizeof(uintptr_t));
}

static bool 
page_hash_less(const struct hash_elem *e1, const struct hash_elem *e2, void *aux UNUSED)
{
  struct page_node *node1 = hash_entry(e1, struct page_node, helem);
  struct page_node *node2 = hash_entry(e2, struct page_node, helem);

  return (node1->upage < node2->upage ? true : false);
}

//在SPT的Process List中寻找Process node 
//返回指向process node的指针
static struct process_node *
find_process_node(struct thread *t)
{
  struct process_node *process; 
  struct process_node key_process;
  key_process.pid = t->tid;

  //寻找对应的process
  lock_acquire(&process_list_lock);
  struct hash_elem *process_helem = hash_find(&process_list, &key_process.helem);
  lock_release(&process_list_lock);

  if (process_helem == NULL)
  {
    // printf("find_process(): Cannot find process whose name is %s, pid is %d\n", t->name, t->tid);
    return NULL;
  }

  process = hash_entry(process_helem, struct process_node, helem); 
  return process;
}

// 在SPT的各个线程的pagelist中查找uaddr对应的page node 
// 返回指向page node的指针
static struct page_node *
find_page_node(struct process_node *pnode, const void *uaddr)
{
  struct page_node key_page;
  struct page_node *page;

  key_page.upage = pg_round_down(uaddr);

  struct hash_elem *page_helem = hash_find(&pnode->page_list, &key_page.helem);
  if (page_helem == NULL)
  { 
    //printf("find_page(): Cannot find page in process %d whose uaddr is %d\n", pnode->pid, (uint32_t)uaddr);
    return NULL;
  }
  page = hash_entry(page_helem, struct page_node, helem);

  return page;
}

void 
page_init()
{
  hash_init(&process_list, page_process_hash_hash, page_process_hash_less, NULL); 
  lock_init(&process_list_lock);
}

void 
page_process_init(struct thread *t)
{
  struct process_node *process_node;
  process_node = malloc(sizeof(struct process_node));

  if (process_node == NULL)
    PANIC("Cannot allocate memory to store a hash map for process!\n");

  process_node->pid = t->tid;

  hash_init(&process_node->page_list, page_hash_hash, page_hash_less , NULL); 

  lock_acquire(&process_list_lock);
  hash_insert(&process_list, &process_node->helem);
  lock_release(&process_list_lock);
}

// 根据uaddr创建一个Supplemental Page Table对象(Page Node)
// 完成初始化并将其加入对应进程持有的页面目录中
// 返回一个Page Node对象
struct page_node *
page_add_page(struct thread *t, const void *uaddr, uint32_t flags, enum location loc, enum role role)
{
  ASSERT(role != SEG_UNUSED);
  //保证添加的pte一定对应一个已经存在的页
  if (loc == LOC_MEMORY)
    ASSERT(lookup_page(t->pagedir, uaddr, false) != NULL);

  //准备用来查找的key结构体
  struct process_node *process_node;
  struct page_node *node;
  //为Page Node分配内存
  node = malloc(sizeof(struct page_node));
  if (node == NULL)
    PANIC("Cannot allocate memory to store a page in SPT!\n");

  //初始化Page Node
  node->owner = t->tid;
  node->upage = pg_round_down(uaddr);
  node->sharing = flags & PG_SHARING;
  node->loc = loc;
  node->frame_node = NULL;
  node->role = role;

  process_node = find_process_node(t); 
  ASSERT(process_node != NULL);
  //保证每个进程获取的内存页面不超过1024页(4GiB)
  ASSERT(process_node->page_list.elem_cnt <= 1024);
  //在Process Node的hashmap中插入Page Node
  bool success = (hash_insert(&process_node->page_list, &node->helem) == NULL) ? true : false;
  
  if (!success)
  {
    free(node);
    return NULL;
  }

  return node;
}

// 在SPT中寻找uaddr对应的页面对象(Page Node)
struct page_node *
page_seek(struct thread *t, const void *uaddr)
{
  struct process_node *process_node = find_process_node(t);
  if (process_node == NULL)
    return NULL;

  struct page_node *page_node = find_page_node(process_node, uaddr);
  return page_node;
}

//用于销毁进程持有的Pagelist的辅助函数
//用于释放物理页帧frame, 并释放Page Node的硬件资源(free(node))
static void
page_page_destructor(struct hash_elem *helem, void *aux)
{
  struct thread *t = aux;
  struct page_node *node = hash_entry(helem, struct page_node, helem);
  if(node->loc == LOC_MEMORY)
    frame_destroy_frame(node->frame_node);
  pagedir_clear_page(t->pagedir, node->upage);
  free(node);
}

//完全销毁一个进程持有的Page List, 释放其所有持有的页面
//释放所有页面对应的内存
//TODO: 当前未实现清除swap中的内容!!!!!!!
void 
page_destroy_pagelist(struct thread *t)
{
  struct process_node *process = find_process_node(t);
  ASSERT(process != NULL);
  //摧毁前传入辅助参数: 线程句柄
  process->page_list.aux = t;

  //摧毁该进程持有的pagelist
  hash_destroy(&process->page_list, page_page_destructor);

  // 在process_list中删除该process
  lock_acquire(&process_list_lock);
  hash_delete(&process_list, &process->helem);
  lock_release(&process_list_lock);
}

// 在page和frame之间建立链接, 把空闲的frame分配给一个Page node
void
page_assign_frame(struct page_node *pnode, struct frame_node *fnode)
{
  ASSERT(pnode->loc != LOC_MEMORY);
  ASSERT(pnode->frame_node == NULL)
  ASSERT(fnode->page_node == NULL);
  ASSERT(fnode->kaddr != NULL);

  pnode->frame_node = fnode;
  fnode->page_node  = pnode;

  pnode->loc = LOC_MEMORY;
}

// 直接为uaddr快速分配内存页面
// 从物理内存中获取一帧frame
// 并在当前进程的process page list中添加一个node 
// 再把它们链接起来
bool
page_get_page(struct thread *t, const void *uaddr, uint32_t flags, enum role role)
{
  // 若文件长度不是PGSIZE的整数倍, 你需要在最末尾的页面, 且不是文件的部分上填充0
  if (role == SEG_MMAP)
    flags |= FRM_ZERO;

  struct frame_node *fnode = frame_allocate_page(t->pagedir, uaddr, flags);
  struct page_node  *pnode = page_add_page(t, uaddr, flags, LOC_NOT_PRESENT, role);
  if (fnode == NULL)
    return false;

  page_assign_frame(pnode, fnode);
  if (role == SEG_MMAP)
  {
    // 必须整页读取文件
    uaddr = pg_round_down(uaddr);
    //  找到对应的文件node
    struct mmap_vma_node *mnode = page_mmap_seek(t, USE_ADDR, uaddr);     
    if(mnode == NULL)
      PANIC("Cannot find mmap mapping accroding to uaddr!\n");

    size_t filesize = mnode->mmap_seg_end - mnode->mmap_seg_begin;
    // 只读取一页的内容
    // 准备读取文件, 移动文件指针到pos位置
    size_t pos = uaddr - mnode->mmap_seg_begin;
    uint32_t read_bytes = filesize - pos >= PGSIZE ? PGSIZE : filesize - pos;
    file_seek(mnode->file, pos);
    if (file_read(mnode->file, (void *)uaddr, read_bytes) != read_bytes)
    {
      page_free_page(t, uaddr);
      PANIC("page_get_page(): read bytes for mmap file failed!\n");
    }
  }


  return true;
}

// 释放页面
void
page_free_page(struct thread *t, const void *uaddr)
{
  struct page_node *page_node = page_seek(t, uaddr);
  ASSERT(page_node != NULL);

  struct process_node *process_node = find_process_node(t);
  ASSERT(process_node != NULL);

  struct hash_elem *helem = hash_delete(&process_node->page_list, &page_node->helem);
  ASSERT(helem != NULL);

  page_page_destructor(&page_node->helem, (void *)t);
}

//释放一整段内存上的page
void
page_free_multiple(struct thread *t, const void *begin, const void *end)
{
  ASSERT(begin < end);
  begin = pg_round_down(begin);

  while(begin < end)
  {
    page_free_page(t, begin);
    begin = ((uint8_t *)begin) + PGSIZE;
  }
}

// 双模式查找, 可提供mapid或addr.
static struct mmap_vma_node *
page_mmap_seek(struct thread *t, mapid_t mapid, const void *addr)
{
  struct list *mmap_list = &t->vma.mmap_vma_list;
  struct list_elem *e;
  struct mmap_vma_node *mnode;

  for (e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e))
  {
    mnode = list_entry(e, struct mmap_vma_node, elem);
    if (mnode->mapid == mapid)
      return mnode;
    if (addr >= mnode->mmap_seg_begin && addr < mnode->mmap_seg_end)
      return mnode;
  }
  
  return NULL;
}

//检查传入的uaddr在数据段中的合法性, 返回其位于哪个数据段(role)
//若uaddr不合法, 返回SEG_UNUSED
enum role
page_check_role(struct thread *t, const void *uaddr)
{
  // 注意! 地址不包含end!
  if (uaddr >= t->vma.code_seg_begin && uaddr < t->vma.code_seg_end)
    return SEG_CODE;

  // 栈空间最大不超过8Mib (0x00800000 bytes)
  // 任何试图在0xbf800000 到 PHYS_BASE之间的uaddr都会被视为正常的栈增长
  if (uaddr >= (void *)0xbf800000 && uaddr < t->vma.stack_seg_end)
    return SEG_STACK;
  
  if (page_mmap_seek(t, USE_ADDR, uaddr) != NULL)
    return SEG_MMAP;

  return SEG_UNUSED;
}

// 监测需要mmap的内存区域与其他内存区域是否有重叠
static bool
page_mmap_overlap(struct thread *t, void *addr, size_t filesize)
{
  void *begin = addr;
  void *end   = (uint8_t *)(addr) + filesize;

  if (!(begin >= t->vma.code_seg_end && end <= t->vma.code_seg_begin))
    return false;

  struct list *mmap_list = &t->vma.mmap_vma_list;
  struct list_elem *e;
  struct mmap_vma_node *mnode;

  for (e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e))
  {
    mnode = list_entry(e, struct mmap_vma_node, elem);
    if (!(begin >= mnode->mmap_seg_end || end <= mnode->mmap_seg_begin))
      return false;
  }

  return true;
}

inline static mapid_t
page_allocate_mapid(struct thread *t)
{
  return (++t->vma.mapid);
}

mapid_t
page_mmap_map(struct thread *t, struct file *file, void *addr)
{
  // fd是否为0和1的检查需要在syscall中做！
  if (pg_ofs(addr) != 0 || addr == NULL)
    return -1;

  struct mmap_vma_node *node;
  node = malloc(sizeof(struct mmap_vma_node));
  if (node == NULL)
  {
    printf("page_mmap_create(): Cannot allocate memory for mmap_vma_node!\n");
    return -1;
  }

  size_t filesize = file_length(file);
  if (filesize == 0)
    return -1;

  if (page_mmap_overlap(t, addr, filesize))
  {
    node->mapid           = page_allocate_mapid(t);
    node->mmap_seg_begin  = addr;
    node->mmap_seg_end    = (uint8_t *)(addr) + filesize;
    node->file            = file;
  }
  else
    return -1;

  list_push_back(&t->vma.mmap_vma_list, &node->elem);

  return node->mapid;
}

void
page_mmap_unmap(struct thread *t, mapid_t mapid)
{
  struct mmap_vma_node *mnode = page_mmap_seek(t, mapid, USE_MAPID);
  if (mnode == NULL)
    return ;

  page_free_multiple(t, mnode->mmap_seg_begin, mnode->mmap_seg_end); 
  list_remove(&mnode->elem);
  file_close(mnode->file);
  free(mnode);
}
