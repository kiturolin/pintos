#include "thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "flags.h"
#include "init.h"
#include "interrupt.h"
#include "intr-stubs.h"
#include "list.h"
#include "malloc.h"
#include "palloc.h"
#include "stdbool.h"
#include "switch.h"
#include "synch.h"
#include "vaddr.h"
#include "fixed-point.h"
#include "../devices/timer.h"


#ifdef USERPROG
#include "../userprog/process.h"
#endif

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

struct list sleep_list;
/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };


/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

bool thread_pri_sch;
/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
int ready_threads;
int load_avg_fp;

int32_t time_to_wake = -1;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
static void thread_destroy_pwait_list(struct thread *);
static void thread_vma_init(struct thread *);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
void thread_iterate_ready_list(void);
int thread_update_ready_threads(void);
int thread_calc_priority(struct thread *);
int thread_calc_recent_cpu(struct thread *);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleep_list);
  if (thread_mlfqs)
    load_avg_fp = 0;
  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  sema_init(&initial_thread->exec_sema, 0);
  list_init(&initial_thread->pwait_list);
  list_init(&initial_thread->fd_list);
  thread_vma_init(initial_thread);
  initial_thread->current_fd = 1;
  initial_thread->status = THREAD_RUNNING;
  initial_thread->page_default_flags = 0;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

static void
thread_vma_init(struct thread *t)
{
  t->vma.loading_exe      = false;
  t->vma.code_seg_begin   = NULL;
  t->vma.code_seg_end     = NULL;
  t->vma.data_seg_begin   = NULL;
  t->vma.data_seg_end     = NULL;
  t->vma.stack_seg_begin  = NULL;
  t->vma.stack_seg_end    = NULL;
  t->vma.mapid            = 0;

  list_init(&t->vma.mmap_vma_list);
}

// 接受优先级捐赠，更改当前优先级
void
thread_receive_donation(struct thread *t, int priority)
{
  t->priority = priority;
}

// 将锁添加到线程的持有锁序列中，表明线程现在持有该锁
// 该函数应该在lock_acquire()的末尾处运行
void
thread_add_holding_lock(struct thread *t, struct lock *lock)
{

  ASSERT(t->lock_cnt < MAX_LOCKS); // 确保数组不会越界
  t->lock_holding[t->lock_cnt++] = lock;
  // 在序列末尾添加锁
}

// 线程释放锁时需要进行的操作：
// 1. 将锁从自己的lock_holding序列中移除
// 2. 将自己的优先级进行调整
void
thread_restore_priority(struct thread *t, struct lock *lock)
{
  // 当前必然持有锁
  ASSERT(t->lock_cnt > 0)

  bool lock_found = false;

  for (int j = 0; j < t->lock_cnt; j++)
  {
    if (lock == t->lock_holding[j])
    {
      // 将最后一个元素搬运到即将删掉的元素处
      t->lock_holding[j] = t->lock_holding[--t->lock_cnt];
      t->lock_holding[t->lock_cnt] = NULL;
      lock_found = true;
      // 清空最后一个元素
    }
  }

  ASSERT(lock_found)

  // 如果上面的操作移除的是该线程持有的最后一把锁，那么该线程的优先级
  // 调整为最开始的基准优先级
  if (t->lock_cnt == 0)
  {
    t->priority = t->base_priority;
    return ;
  }

  // 筛选出持有的锁当中优先级最高的那个，并将线程的优先级调整为该锁的优先级
  int max_priority = t->lock_holding[0]->priority;

  for (int i = 0; i < t->lock_cnt; i++)
  {
    if(t->lock_holding[i]->priority > max_priority)
      max_priority = t->lock_holding[i]->priority;
  }

  t->priority = max_priority; 
}

// 递归式的重设从当前线程开始的，所有线程的优先级
// 按照等待锁的顺序, lock_waiting是当前线程正在等待获取的锁
// 若当前线程是某个锁的持有者，lock_waiting应该为NULL
void
thread_recursive_set_priority(int priority)
{
  struct thread *cur = running_thread(); //thread_current() ?
  struct thread *t;

  if (cur->lock_waiting == NULL)
    return ;
  
  for (t = cur->lock_waiting->holder; t != NULL; t = t->lock_waiting->holder)
  {
    if (priority > t->priority)
    {
      if(t->lock_waiting != NULL)
        t->lock_waiting->priority = priority;

      thread_receive_donation(t, priority);
    }   
    //是否需要判断 存疑？

    if (t->lock_waiting == NULL)
      break;
  }

}

bool
thread_compare_priority(const struct list_elem *elem1, const struct list_elem *elem2, void *aux UNUSED);

/* 调用者为正在运行的线程，检查ready_list中是否存在优先级比自己高的线程
 * 如果有, 那么立即让出CPU
 * 注意! 我们要求ready_list是按照优先级降序排列的!
 * 运行该函数前需要保证ready_list是有序的!
 * */
void
thread_yield_on_priority (void)
{
  struct thread *cur = thread_current();
  struct thread *t;
  struct list_elem *e;

  enum intr_level old_level = intr_disable();
  
  //list_sort(&ready_list, thread_compare_priority, NULL);

  for (e = list_begin(&ready_list);  e != list_end(&ready_list); e = list_next(e))
  {
    t = list_entry(e, struct thread, elem);

    if (t->priority > cur->priority) 
    {
      thread_yield();
      break;
    }

    if (t->priority == cur->priority)
      break;
  }

  intr_set_level(old_level);
}

// 用于比较两个线程优先级的辅助函数
// 当elem1 对应线程的优先级严格大于elem2的线程优先级时. 返回true
bool
thread_compare_priority(const struct list_elem *elem1, const struct list_elem *elem2, void *aux UNUSED)
{
    ASSERT(elem1 != NULL);
    ASSERT(elem2 != NULL);
    
    struct thread *t1 = list_entry(elem1, struct thread, elem);
    struct thread *t2 = list_entry(elem2, struct thread, elem);

    return t1->priority > t2->priority ;
}

static void
thread_wakeUp(int32_t wakeTime)
{
    struct list_elem *e;
    int32_t first_min   = INT32_MAX;
    int32_t second_min  = INT32_MAX;

    // 要想通过bochs的测试, 必须加这个循环!
    // 你可以试试看不加的效果, 跑mlfqs-load-60会在t=118时卡死
    for (int i = 10; i > 0; i--);

    if (list_empty(&sleep_list))
        return ;

    for(e = list_begin(&sleep_list); e != list_end(&sleep_list); ){
        struct thread *t = list_entry(e, struct thread, sleep_elem);
        struct list_elem *next = e->next;
        if (t->wake_time < first_min)
        {
          second_min = first_min;
          first_min = t->wake_time;
        }
        else if (t->wake_time < second_min && t->wake_time != first_min)
          second_min = t->wake_time;

        if (t->wake_time == wakeTime)
        {
            list_remove(e);
            thread_unblock(t);    
        }

        e = next;
    }

    if (list_empty(&sleep_list))
      time_to_wake = -1;

    if (second_min != INT32_MAX)
    {
      time_to_wake = second_min;
      // printf("In thread_wakeUp, modified time_to_wake to: %d\n", time_to_wake);
    }
}

void
thread_tick (void) 
{
  struct thread *t = thread_current ();
  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (timer_ticks() == time_to_wake)
  {
    // printf("Call thread_wakeUp, now ticks: %d\n", time_to_wake);
    thread_wakeUp(time_to_wake);
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

// 释放等待队列的各个node的内存
// 不对pwait_list本身做remove等操作, 只是释放内存
static void
thread_destroy_pwait_list(struct thread *t)
{
  struct list_elem *e;
  struct pwait_node_ *node;

  for (e = list_begin(&t->pwait_list); e != list_end(&t->pwait_list); )
  {
    node = list_entry(e, struct pwait_node_, elem);

    node->child->pwait_node = NULL;
    // 必须在free前获取下一个节点! 否则e自己会被释放!
    e = list_next(e);
    free(node);
  }
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct thread *cur = thread_current();
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  // 每个线程的内存空间只能是4KB : 因为只给每个线程分配了一页内存!
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  // Project 2: USERPROG

  sema_init(&t->exec_sema, 0);
  list_init(&t->fd_list);
  t->current_fd = 1;
  t->exec_file = NULL;
  // 初始化wait()有关事宜
  list_init(&t->pwait_list);

  //初始化自己的node
  t->pwait_node = malloc(sizeof(struct pwait_node_));
  if (t->pwait_node == NULL)
    PANIC("pwait_node memory allocation failed!\n");

  sema_init(&t->pwait_node->sema, 0);
  t->pwait_node->child      =   t;
  t->pwait_node->child_pid  =   t->tid;
  t->pwait_node->waited     =   false;
  t->pwait_node->parent     =   cur;
  t->pwait_node->status     =   NOT_SPECIFIED;

  list_push_back(&(cur->pwait_list), &(t->pwait_node->elem));

  //Project 3: Virtual memory

  t->page_default_flags = 0;
  thread_vma_init(t);

  /* Add to run queue. */
  thread_unblock (t);
  thread_yield_on_priority();
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);
    

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem, thread_compare_priority, NULL);
  //list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());
  struct thread *t = thread_current();
  // 顺序很重要! 要在process_exit()之前清理pwait_list, 而不是之后!
  thread_destroy_pwait_list(t);
#ifdef USERPROG 
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&t->allelem);
  t->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_insert_ordered(&ready_list, &cur->elem, thread_compare_priority, NULL);
    //list_push_back (&ready_list, &cur->elem);


  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if(!thread_mlfqs)
  {
    struct thread *cur = thread_current();
  
    // 如果当前的priority与基准priority不一致，说明当前线程接受了捐赠
    // 当且仅当base_priority与priority一致时，才更改当前线程的priority
    if (cur->base_priority == cur->priority)
      cur->priority = new_priority;

    cur->base_priority = new_priority;

    thread_yield_on_priority();
  }
 }

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}
/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  thread_current()->nice = nice;
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current()->nice;
}

// 重新计算当前系统已经就绪的线程数量
int 
thread_update_ready_threads(void)
{
  // 包含当前正在运行的线程(THREAD_READY + THREAD_RUNNING)
  // 若当前线程为idle线程, 则说明没有正在运行的线程, 此时不+1
  if (strcmp((const char *)running_thread()->name, "idle"))
    ready_threads = (int)(list_size(&ready_list)) + 1;
  else
    ready_threads = (int)(list_size(&ready_list));
  // 若没有正在运行的线程, 则不+1

  return ready_threads;
}
// 计算系统平均负载, 应该每秒重新计算一次
int
thread_calc_sys_load_avg(void)
{
  thread_update_ready_threads();
  load_avg_fp = fp_add(
                fp_multiply(load_avg_fp, LOADAVG_COEFF_59_60), 
                fp_multiply_by_int(LOADAVG_COEFF_01_60, ready_threads)
                ); 
  return load_avg_fp;
}
/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Not yet implemented. */
  return fp_convert_to_int_rdn(fp_multiply_by_int(load_avg_fp, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  struct thread *cur = thread_current();
  return fp_convert_to_int_rdn(fp_multiply_by_int(cur->recent_cpu_fp, 100));
}

// 每个tick更新一次recent_cpu_fp(自增1)
void 
thread_update_cur_recent_cpu(void)
{
  struct thread *cur = thread_current();
  
  if (!strcmp((const char *)&cur->name, "idle"))
    return ;
  
  cur->recent_cpu_fp = fp_add_int(cur->recent_cpu_fp, 1);
}


// 重新计算单个线程的优先级
// 应该每4个tick运行一次.
int
thread_calc_priority(struct thread *t)
{
  t->priority = PRI_MAX - fp_convert_to_int_rdn(fp_divide_by_int(t->recent_cpu_fp, 4)) - (t->nice * 2);

  // ASSERT(t->priority >= PRI_MIN && t->priority <= PRI_MAX)
  if (!(t->priority >= PRI_MIN && t->priority <= PRI_MAX))
    return 0;

  return t->priority;
}
// 根据load_avg和nice重新计算单个线程的recent_cpu
int
thread_calc_recent_cpu(struct thread *t)
{
  int coeff_fp = fp_divide(
                  fp_multiply_by_int(load_avg_fp, 2), 
                  fp_add_int(
                        fp_multiply_by_int(load_avg_fp, 2), 1)
                  );  

  t->recent_cpu_fp = fp_add_int(
                        fp_multiply(coeff_fp, t->recent_cpu_fp), 
                        t->nice
                        );

  return t->recent_cpu_fp;
}

// 为每个线程重新计算recent_cpu
// 应该每秒运行一次
void
thread_calc_all_recent_cpu(void)
{
  struct list_elem *e;
  struct thread *t;

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
  {
    t = list_entry(e, struct thread, allelem);
    thread_calc_recent_cpu(t);
  }
}

// 为每个线程重新计算Priority
// 应该每4个tick运行一次
void 
thread_calc_all_priority(void)
{
  struct list_elem *e;
  struct thread *t;

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
  {
    t = list_entry(e, struct thread, allelem);
    thread_calc_priority(t);
  }
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      __asm__ volatile("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  // 在GCC内联汇编中，寄存器名称需要用两个百分号%%表示
  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  __asm__("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
    //当线程指针t不为空指针且t的magic number仍未被覆盖时, 返回true
    //若t->magic != THREAD_MAGIC 证明了可能有栈溢出发生!
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->intr_stack = NULL;
  t->magic = THREAD_MAGIC;

  t->wd = ROOT_DIR_SECTOR;

  t->base_priority = priority;
  t->priority = priority;
  t->lock_cnt = 0;
  memset(&t->lock_holding, 0, sizeof(t->lock_holding));
  
  t->nice = 0;
  t->recent_cpu_fp = 0;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);
  // size必须是8个字节的整数倍

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
    if (list_empty (&ready_list))
        return idle_thread;
    else
        return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

//当遵循最严格的优先级调度时, 没有必要切换到idle线程, 因为
//idle线程的优先级为0, main线程的优先级总比它高, 而且不可能存在main线程
//退出, idle线程存活的情况, 当main线程退出意味着系统关机
//当 当前线程的状态为阻塞或死亡（BLOCKED/DYING）时，无条件返回下一个线程继续运行
static struct thread *
next_thread_to_run_PriSch (void)
{
    struct thread *cur = running_thread(); 
   

    if (list_empty(&ready_list))
        return idle_thread;
    
    struct thread *next = list_entry(list_pop_front(&ready_list), struct thread, elem);

    if (next->priority >= cur->priority || cur->status == THREAD_DYING || cur->status == THREAD_BLOCKED )
      return next;
    else
      return cur;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *next;
  struct thread *cur = running_thread ();

  if (thread_pri_sch)
    next = next_thread_to_run_PriSch();
  else
    next = next_thread_to_run ();
  
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));


  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
