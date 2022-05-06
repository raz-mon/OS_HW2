#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// Added (should this be here, or better in defs for example?):
#define NULL 0

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Added:
// cas function - Atomic Compare And Swap.
extern uint64 cas(volatile void *add, int expected, int newval);

// Three global lists (linked-lists. Their value is the index of the first process in the linked-list in the proc_table).
// Value of -1 is equivalent to NULL (empty linked list).
int sleeping = -1;
int zombie = -1;
int unused = -1;

/*
// Nice pattern for concurrent programming with cas.
public void add(T obj) {
  Link<T> oldVal;
  Link<T> newVal;
  do {
    oldVal = head.get();
    newVal = new Link<T>(obj,oldVal);
  } while (!head.cas(oldVal,newVal));
}
*/

// Get next process index (next link in linked-list).
// Notice that you must lock the process before calling, and release afterwards.
int getNext(int ind){
  return proc[ind].next;
}

void get_lock(int ind){
  acquire(&proc[ind].list_lock);
}

void release_lock(int ind){
  release(&proc[ind].list_lock);
}

void increase_cpu_counter(int cpu_index){
  int counts;
  do{
    counts = cpus[cpu_index].process_counter;
  } while (cas(&cpus[cpu_index].process_counter, counts, counts+1));
}

void decreace_cpu_counter(int cpu_index){
  int counts;
  do{
    counts = cpus[cpu_index].process_counter;
    if(counts < 1){ break;}   // Can't be under 0.
  } while (cas(&cpus[cpu_index].process_counter, counts, counts - 1));
}

// This function return the CPU INDEX which we can be stealing from
int steal_procces(){
  for(int i = 0; i < NCPU; i++){
    if(cpus[i].process_counter > 0){return i;}
  }
  return -1;
}

// Add a link to a "linked-list" to the END of a linked-list.
// If successful, return the added index ("link"). Else --> Return -1.
void addLink(int *first_ind, int to_add){
  // printf("Adding ind %d to a list\n", to_add);

  // printf("Taking %d\n", to_add);
  get_lock(to_add);
  int temp_ind = *first_ind;
  // Handle case of empty list (index=-1).
  if(temp_ind == -1){
    *first_ind = to_add;
    // printf("added process in index %d, to a list.\n", to_add);
    // printf("Releasing %d\n", to_add);
    release_lock(to_add);
    return;
  }
  // Acquire first lock
  // printf("Taking %d\n", temp_ind);
  get_lock(temp_ind);
  int next_ind = proc[temp_ind].next;
  while (next_ind != -1){
    // printf("Taking %d\n", next_ind);
    get_lock(next_ind);
    // printf("Releasing %d\n", temp_ind);
    release_lock(temp_ind);
    temp_ind = next_ind;
    next_ind = getNext(temp_ind);
  }
  proc[temp_ind].next = to_add;
  // printf("Releasing %d\n", temp_ind);
  release_lock(temp_ind);
  // printf("Releasing %d\n", to_add);
  release_lock(to_add);
}

// Remove the first "link" of the linked-list.
// Return the value of the first "link".
int removeFirst(int *first_p){
  // The first part is not fully synchronized - Think about a way to overcome this.
  // printf("\n\nEntered removeFirst\n\n");
  // Empty list case.
  if (*first_p == -1){
    // printf("Oh naaa... Sorry empty list!");
    return -1;
  }

  // Non-empty list case.
  int temp_ind = *first_p;
  // printf("Taking %d\n", temp_ind);
  get_lock(temp_ind);           // Take first node's lock.
  int next_ind = getNext(*first_p);     // No concurency problem here, since the first node is locked --> No-one can change his successor.
  if (next_ind != -1){            // List has more than one component.
    // printf("Taking %d\n", next_ind);
    get_lock(next_ind);
    *first_p = next_ind;
    proc[temp_ind].next = -1;     // No longer points at the next link (process).
    // printf("Releasing %d\n", temp_ind);
    release_lock(temp_ind);
    // printf("Releasing %d\n", next_ind);
    release_lock(next_ind);
    return temp_ind;
  }
  else{                           // 1-component list.
    *first_p = -1;                // Empty list.
    // printf("Releasing %d\n", temp_ind);
    release_lock(temp_ind);       // Release it's lock.
    return temp_ind;              // Return index removed.
  }
}

// Remove link with index (in the proc_table) ind from the list.
// Return 1 (true) for success, 0 (false) for failure.
int remove(int *first_p, int ind){
  // printf("\nEntered remove. Removing process %d from a list\n");
  // Handle empty list case.
  if(*first_p == -1){
    return -1;
  }

  // List is not empty.
  // printf("Taking %d\n", *first_p);
  get_lock(*first_p);                       // Get lock of first node.

  if (ind == *first_p){                 // The element we wish to extract from the list is the first element.
    if (getNext(*first_p) == -1){       // List of one element, which is the wanted element.
      int temp = *first_p;
      *first_p = -1;
      // printf("Releasing %d\n", temp);
      release_lock(temp);
      return 1;
    }
    else{                               // List of more than one element.
        int temp = *first_p;
        int temp2 = getNext(*first_p);
        // printf("Taking %d\n", temp2);
        get_lock(temp2);
        *first_p = temp2;
        proc[temp].next = -1;
        // printf("Releasing %d\n", temp);
        release_lock(temp);
        // printf("Releasing %d\n", temp2);
        release_lock(temp2);
        return 1;
    }
  }

  // Component to remove is not the first node.
  int prev = *first_p;
  int curr = getNext(prev);
  // The node to be removed is not the first node in the linked-list.
  while (curr != -1){
    // printf("Taking %d\n", curr);
    get_lock(curr);                     // Lock "current" node (process).
    if (curr == ind){

      // printf("\n\nfound it!!!@#!@#!@#!@#!\n\n");

      // Delete node from list.
      proc[prev].next = proc[curr].next;
      proc[curr].next = -1;
      // printf("Releasing %d\n", prev);
      release_lock(prev);
      // printf("Releasing %d\n", curr);
      release_lock(curr);
      return 1;
    }
    // printf("Releasing %d\n", prev);
    release_lock(prev);
    prev = curr;
    curr = getNext(curr);
  }
  // printf("Releasing %d\n", prev);
  release_lock(prev);
  return 0;                        // Node to remove not found (it's index).
}

// Print the linked-list, pointed at by first.
void printList(int *first){
  // No locking at the moment. So this can show false results.
  int temp = *first;
  get_lock(temp);
  int next; 
  for (;;){
    printf("%d, ", temp);
    if (getNext(temp) != -1){
      next = getNext(temp);
      get_lock(next);
      release_lock(temp);
      temp = next;
    }
    else{
      release_lock(temp);
      break;
    }
  }
  printf("\n");
}

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  mycpu()->first = -1;                      // Initialize the 'first' field of the first cpu (applied to cpu 0 only!).

  struct proc *p;
  // Added
  int i = 0;
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      // Added
      initlock(&p->list_lock, "list_lock");
      p->kstack = KSTACK((int) (p - proc));
      // Added
      p->next = -1;
      p->ind = i;
      p->cpu_num = 0;
      i++;
      addLink(&unused, p->ind);      // Add link to the unused list, if this is not the init proc which is used.
  }
  // printf("unused list: \n");
  // printList(&unused);
  printf("Finished procinit.\n");
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  // int pid;
  
  // new implementation (using cas).
  int old;
  do{
    old = nextpid;
  } while (cas(&nextpid, old, old+1));
  return old;

/*
  // old implementation (using pidlock).
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);
  return pid;
*/
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  printf("Entered allocproc\n");
  struct proc *p;
  // Added
  int index = removeFirst(&unused);
  if(index == -1){return 0;}        // Unused is empty.
  else{
    p = &proc[index];
    acquire(&p->lock);
    goto found;
  }

  /*
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;
  */
found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  printf("Exiting allocproc\n");
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  // Added
  // Remove from ZOMBIE list
  remove(&zombie, p->ind);
  // Add to UNUSED list
  addLink(&unused, p->ind);
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");
  
  p->state = RUNNABLE;
  //Added
  p->cpu_num = cpuid();
  cpus[p->cpu_num].process_counter = 1;
  // add p to cpu runnable list
  // Note: The process was already removed from the 'unused' list in 'allocproc'.
  addLink(&cpus[p->cpu_num].first, p->ind);                 // Add this link to this cpu's list.
  release(&p->lock);

  printf("Finished userinit.\n");
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  //Added
  np->cpu_num = p->cpu_num;
  addLink(&cpus[np->cpu_num].first, np->ind);
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  // Added
  // add p to the zombie list
  addLink(&zombie, p->ind);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  printf("entered scheduler\n");
  struct proc *p;
  struct cpu *c = mycpu();
  if (cpuid() != 0)
    c->first = -1;              // Initialize 'first' field of other cpus (0 was initialized in procinit).
  
  c->proc = 0;
  for(;;){
    printf("Entered for loop\n");
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    printf("entered scheduler loop\n");
    printf("schedulr cpu first index of cpu %d: %d\n", cpuid(), c->first);
    
    int index;
    while (c->first != -1)      // Otherwise no process to run in it's list.
    {
      printf("schedulr entered while\n");
      index = removeFirst(&(c->first));
      printf("schedulr remove index: %d\n", index);
      p = &(proc[index]);
      acquire(&(p->lock));
      if(p->state != RUNNABLE){release(&p->lock); break;}
      p->state = RUNNING;
      c->proc = p;
      // release_lock(index);   // What is this for?
      printf("schedulr reached here 4\n");
      swtch(&c->context, &p->context);
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      //Should the release lock be here?
      release(&p->lock);
    }
   
   
   
    // For part 4.
    /*
    int cpu_index;
    if(c->process_counter == 0){
      cpu_index = steal_procces();
      addLink(&c->first, removeFirst(&cpus[cpu_index].first));
      decreace_cpu_counter(cpu_index);
      increase_cpu_counter(cpuid());
    }
    */


    
/*
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  */
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  //Added
  // add p to the cpu runnable list
  addLink(&cpus[p->cpu_num].first, p->ind);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  // Added
  // add p to the sleeping list
  addLink(&sleeping, p->ind);

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        //Added
        // remove p from sleeping
        remove(&sleeping, p->ind);
        // add p to cpu's runnable list
        addLink(&cpus[p->cpu_num].first, p->ind);
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
        // Added
        remove(&sleeping, p->ind);
        addLink(&cpus[p->ind].first, p->ind);
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Added:

// Set the cpu of the current process to cpu_num (first argument).
// Return cpu_num if successive, and a negative number otherwise.
int
set_cpu(int cpu_num)
{
  struct proc *p = myproc();
  p->cpu_num = cpu_num;
  return cpu_num;
}

// Get the cpu of the current process.
// Return cpu_num if successive, and a negative number otherwise.
int
get_cpu(void)
{
  struct proc *p = myproc();
  return p->cpu_num;
}

// Check the LinkedList implementation
void
check_LL(void)
{
  printf("checking LL implementation...\n");
  sleeping = 1;
  addLink(&sleeping, 2);
  /*
  addLink(&sleeping, 3);
  printList(&sleeping);
  removeFirst(&sleeping);
  printList(&sleeping);
  remove(&sleeping, 3);
  printList(&sleeping);
  */
}

// End of addition.


// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}
