#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  case T_PGFLT:
    void * va = (void *) rcr2();
    //pgdown so that it check if begging of page of this virtual address is less than the allocated page size
    va =  (void *)PGROUNDDOWN((uint)va);

    //first walkpagedir and see if this pagefault occured as a result of accessing guard page(page is present but not user accessible)
    pte_t *pte = walkpgdir(myproc()->pgdir, va,0);
    if(pte != 0 && *pte&PTE_P && !(*pte&PTE_U)){
        cprintf("guard page access\n");
        myproc()->killed = 1;
    }
    //page fault as a result of alloc on demand
    else if((uint)va < myproc()->sz){
      char * mem = kalloc();

      //can't grab new frame for to map to this virtual address
      if(mem == 0){
        cprintf("alloc on demand: out of memory\n");
        myproc()->killed = 1;
      }
      else{
        memset(mem, 0, PGSIZE);

        //can't grab new frame to store the new page table for the mapping
        if(mappages(myproc()->pgdir, (char*)va, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
          cprintf("alloc on demand: out of memory\n");
          kfree(mem);
          myproc()->killed = 1;
        }
        //sucessfully alloacted on demand
        else{
          switchuvm(myproc());
          return;
        }
      }
    }
    //actual segfault, so follow old behavior
    else{
      if(myproc() == 0 || (tf->cs&3) == 0){
        // In kernel, it must be our mistake.
        cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
                tf->trapno, cpuid(), tf->eip, rcr2());
        panic("trap");
      }
      // In user space, assume process misbehaved.
      cprintf("pid %d %s: trap %d err %d on cpu %d "
              "eip 0x%x addr 0x%x--kill proc\n",
              myproc()->pid, myproc()->name, tf->trapno,
              tf->err, cpuid(), tf->eip, rcr2());
      myproc()->killed = 1;
    }
  
    break;
  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
