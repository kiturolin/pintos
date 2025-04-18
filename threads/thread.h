#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H


#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "synch.h"
#include "../devices/block.h"
#include "../filesys/filesys.h"
#include "../filesys/file.h"


/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef unsigned int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#define MAX_LOCKS 10
#define NOT_SPECIFIED -2

struct pwait_node_ 
{
  struct thread *parent;
  struct thread *child;
  uint32_t child_pid;
  int32_t status;
  bool waited;
  struct semaphore sema;
  struct list_elem elem;
};

struct fd_node
{
  uint32_t fd;
  int32_t mapid;
  struct file *file;
  struct list_elem elem;
};

struct mmap_vma_node
{
  uint32_t fd;
  struct file *file;
  int32_t mapid;
  void *mmap_seg_begin;
  void *mmap_seg_end;
  struct list_elem elem;
};

struct vma 
{
  bool loading_exe;
  uint8_t mapid;

  void *code_seg_begin;
  void *code_seg_end;

  void *data_seg_begin;
  void *data_seg_end;

  void *stack_seg_begin;
  void *stack_seg_end;

  struct list mmap_vma_list;
};


/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    uint8_t *intr_stack;
    int priority;                       /* Priority. */
    int base_priority;
    int64_t wake_time;
    block_sector_t wd;
    struct list_elem allelem;           /* List element for all threads list. */
    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */
    struct list_elem sleep_elem;
//#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
    struct vma vma;
    struct file* exec_file;
    struct pwait_node_ *pwait_node; 
    struct list pwait_list;
    struct semaphore exec_sema;
//#endif
    struct list fd_list;
    uint32_t current_fd;
    struct lock* lock_waiting;
    struct lock* lock_holding[MAX_LOCKS]; 
    int lock_cnt;
    int nice;
    int recent_cpu_fp;
    uint32_t page_default_flags;
    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;
extern int load_avg_fp;
extern int ready_threads;
extern int32_t time_to_wake;
extern struct list sleep_list;
extern bool thread_pri_sch;
extern struct list ready_list;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
struct thread *running_thread (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

void thread_add_holding_lock(struct thread *t, struct lock *lock);
void thread_receive_donation(struct thread *t, int priority);
void thread_restore_priority(struct thread *t, struct lock *lock);
void thread_yield_on_priority (void);
bool thread_compare_priority (const struct list_elem *elem1, const struct list_elem *elem2, void *aux UNUSED);
int thread_get_priority (void);
void thread_set_priority (int);
void thread_recursive_set_priority(int);

int thread_calc_sys_load_avg(void);
void thread_update_cur_recent_cpu(void);
void thread_calc_all_recent_cpu(void);
void thread_calc_all_priority(void);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
