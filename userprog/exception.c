#include "exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "gdt.h"
#include "syscall.h"
#include "../threads/interrupt.h"
#include "../threads/thread.h"
#include "../threads/vaddr.h"
#include "../vm/frame.h"
#include "../vm/page.h"


/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);

      struct semaphore *sema = NULL;
      struct thread *cur = thread_current();
      if (cur->pwait_node != NULL)
      {
        cur->pwait_node->status = -1;
        sema = &cur->pwait_node->sema;
      }
     
      if (sema != NULL)
        sema_up(sema);

      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  bool from_user_vm; /* é æPage Faultçå°åæ¥èªç¨æ·åå­ç©ºé´ */
  void *fault_addr;  /* Fault address. */
  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  __asm__ ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  struct thread *cur = thread_current();
  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;
  from_user_vm = is_user_vaddr(fault_addr);

  enum role role = SEG_UNUSED;

  if (fault_addr == cur->vma.code_seg_end && cur->vma.loading_exe)
    role = SEG_CODE;
  else if (page_check_role(cur, fault_addr) == SEG_STACK)
    role = SEG_STACK;
  else if (page_check_role(cur, fault_addr) == SEG_MMAP)
    role = SEG_MMAP;
  else
    syscall_exit(f, -1);   
  
  // 用户空间发生的错误且addr指向了内核空间: 一定是不合法的访问! 杀死进程
  if (!from_user_vm && user)
    syscall_exit(f, -1);

  // 如果尝试向未分配的栈区域中读取数据, 必然是错误的!
  // 能进入page fault handler就说明其访问了未分配的区域
  if (role == SEG_STACK && !write)
    syscall_exit(f, -1);

  if (from_user_vm)
  {
    struct page_node *page = page_seek(cur, fault_addr);    
    // 如果没有在SPT里找到page:
    if (page == NULL)
    {
      // 分配页面
      page_get_page(cur, fault_addr, cur->page_default_flags, role);
      // 更新进程的VMA
      switch(role)
      {
        case SEG_STACK:
          // 注意! 栈是向下生长的!
          cur->vma.stack_seg_begin = (uint8_t *)(cur->vma.stack_seg_begin) - PGSIZE;
          break;
        case SEG_CODE:
          cur->vma.code_seg_end = (uint8_t *)(cur->vma.code_seg_end) + PGSIZE;
          break;
        case SEG_MMAP:
          //do nothing
          //因为mmap的VMA不需要更新
          break;
        case SEG_UNUSED:
          NOT_REACHED();
          break;
        default:
          // TODO: 实现mmap的vma更新 
          break;
      }
      // 返回原位继续执行
      return ;
    }
    else 
    {
      // TODO: 从swap中拉取内存页面
      // 如果用户进程在页面存在的情况下, 向只读内存区域进行读取, 一定是不合法的访问!
      if (write && user)
        syscall_exit(f, -1);
    }
  }
  

  // /* Count page faults. */
  page_fault_cnt++;

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */
  printf ("Page fault at %p: %s error %s page in %s context.\n",
          fault_addr,
          not_present ? "not present" : "rights violation",
          write ? "writing" : "reading",
          user ? "user" : "kernel");
  kill (f);
}

