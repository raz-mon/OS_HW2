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

int removeFirst(int *list, struct spinlock head_lock);

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

struct spinlock sleeping_head_lock;
struct spinlock zombie_head_lock;
struct spinlock unused_head_lock;

// Current number of cpu's
#ifdef numcpus
#define CPUS numcpus
#else
#define CPUS -1
#endif

int num_cpus = CPUS;


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

void increase_cpu_counter(struct cpu *c){
  int old;
  do{
    old = c->process_count;
  } while (cas(&c->process_count, old, old+1));
}

void decreace_cpu_counter(int cpu_index){
  int old;
  do{
    old = cpus[cpu_index].process_count;
    if(old < 1){ break;}   // Can't be under 0.
  } while (cas(&cpus[cpu_index].process_count, old, old - 1));
}

struct cpu*
find_least_used_cpu(void){
  struct cpu *winner = &cpus[0];                        // Just initialization. Will be ran over (for sure).
  int found = 0;
  uint64 min_process_count = 1844674407370955564;      // Initialized to maximum of uint64;
  for (struct cpu *c1 = cpus; c1 < &cpus[num_cpus]; c1++){
    if ((c1->process_count < min_process_count)){
      found = 1;      // Set found to true.
      winner = c1;
      min_process_count = c1->process_count;
    }
  }
  if (found)
    return winner;
  else{
    panic("Couldn't find a least used cpu - bug.");
    return NULL;
  }
}

// Steal a process from one of the cpu's running in the system, and return its index.
int 
steal_process(void){
  // Traverse the cpus array (array of cpus), until finding one with a non-empty ready-list.
  // Then, steal that process by removing it from it's ready-list, and changing it's cpu_num to -1 (will be changed to the right cpu
  // in the calling function). OR NOT. Can change this to perform all relevant procedures (sounds good!).
  int out;
  for (struct cpu *cp = cpus; cp < &cpus[num_cpus]; cp++){
    if (cp == mycpu())
      continue;
    out = removeFirst(&cp->first, cp->head_lock);
    if (out != -1)    // Managed to steal a link from the linked list.
      return out;
  }
  return -1;        // Didn't manage to steal a process.
}

// Add a link to a "linked-list" to the END of a linked-list.
// If successful, return the added index ("link"). Else --> Return -1.
void addLink(int *first_ind, int to_add, struct spinlock head_lock){
  // Get dummy-head of the list pointed at by 'first_ind'.
  acquire(&head_lock);
  
  // while(cas(head_lock, 0, 1))       // Busy-wait until the head is clear (not supposed to be long).
  //   ;;
  
  
  // Taking the lock of the added process.
  get_lock(to_add);
  int temp_ind = *first_ind;
  // Handle case of empty list (index=-1).

  if (*first_ind == -1){
    *first_ind = to_add;
    release_lock(to_add);
    release(&head_lock);
    return;
  }

  /*
  if (!cas(first_ind, -1, to_add)){
    // Succeeded to replace -1 with 'to_add', which is the new head of the list.
    release_lock(to_add);
    // Release dummy-head here.
    release(&head_lock);
    return;
  }
*/

  // If got here -> List is not empty, head is still locked (dummy).
  // Acquire first lock
  // printf("Taking %d\n", temp_ind);
  get_lock(temp_ind);
  // Release 'dummy-head' here.
  release(&head_lock);

  // while (cas(head_lock, 1, 0))
  //   printf("Wierd problem2!\n");

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
  // Releasing the list-lock of the added process.
  release_lock(to_add);
}

// Remove the first "link" of the linked-list.
// Return the value of the first "link".
int removeFirst(int *first_p, struct spinlock head_lock){
  // Get the dummy head lock (critical sections - dealing with the head of the list, until first link is held).
  acquire(&head_lock);
  
  // printf("\n\nEntered removeFirst\n\n");
  // Empty list case.
  if (*first_p == -1){
    // printf("Tried to extract a link from an empty list.\n");
    // cas(head_lock, 1, 0);   // Release the head_lock.
    release(&head_lock);
    
    // while (cas(head_lock, 1, 0))
    //   ;; //printf("Wierd problem3!\n");
    return -1;
  }

  // Non-empty list case.
  int temp_ind = *first_p;
  // printf("Taking %d\n", temp_ind);
  get_lock(temp_ind);           // Take first node's lock.
  
  
  // while (cas(head_lock, 1, 0))   // cas should return false here (because it succeeded!!).
  //   printf("Wierd problem4!\n");

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
    
    
    // Release dummy head lock. First lock is already obtained (lock held).
    release(&head_lock);
    
    
    return temp_ind;
  }
  else{                           // 1-component list.
    *first_p = -1;                // Empty list.
    // printf("Releasing %d\n", temp_ind);
    release_lock(temp_ind);       // Release it's lock.
    
    
    // Release dummy head lock. First lock is already obtained (lock held).
    release(&head_lock);
    
    
    return temp_ind;              // Return index removed.
  }
}

// Remove link with index (in the proc_table) ind from the list.
// Return 1 (true) for success, -1 (false) for failure.
int remove(int *first_p, int ind, struct spinlock head_lock){
  // Get dummy head lock
  acquire(&head_lock);

  // printf("\nEntered remove. Removing process %d from a list\n");
  // Handle empty list case.
  if(*first_p == -1){
    // printf("Tried to extract a link from an empty list.\n");
    release(&head_lock);
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

      // Release the head-lock. The first link is already held, so no problem letting it go.
      release(&head_lock);

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

        // Release the head-lock. The first link is already held, so no problem letting it go.
        release(&head_lock);

        return 1;
    }
  }

  // Release the head-lock. The first link is already held, so no problem letting it go.
  release(&head_lock);

  // Component to remove is not the first node.
  int prev = *first_p;
  int curr = getNext(prev);
  // The node to be removed is not the first node in the linked-list.
  while (curr != -1){
    // printf("Taking %d\n", curr);
    get_lock(curr);                     // Lock "current" node (process).
    if (curr == ind){

      // Delete node from list.
      proc[prev].next = proc[curr].next;
      proc[curr].next = -1;
      // printf("Releasing %d\n", prev);
      release_lock(prev);
      // printf("Releasing %d\n", curr);
      release_lock(curr);

      // Temp: Delete this.
      // release(&head_lock);

      return 1;
    }
    // printf("Releasing %d\n", prev);
    release_lock(prev);
    prev = curr;
    curr = getNext(curr);
  }
  // printf("Releasing %d\n", prev);
  release_lock(prev);

  // Temp: Delete this.
  // release(&head_lock);

  return -1;                        // Node to remove not found (it's index).
}
 
/*
// Search the list pointed at by first_p for the index search_for, return the index of the process if found, -1 otherwise
int search_list(int *first_p, int search_for, struct spinlock head_lock){
  acquire(&head_lock);
  
  // Empty list
  if (*first_p == -1){
    release(&head_lock);
    return -1;
  }

  // Non-empty list
  int curr = *first_p;
  get_lock(curr);
  if (curr == search_for){
    release(curr);
    release(&head_lock);
    return 
  }
  int next = getNext(curr);
  while (next != -1){

  }

}*/


// Print the linked-list, pointed at by first. Used for debugging purposses.
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
  printf("number of cpus running: : %d\n", num_cpus);

  // Initialize cpus 'special' fields.
  int j = 0;
  for (struct cpu *cp = cpus; cp < &cpus[num_cpus]; cp++){
    cp->first = -1;
    initlock(&cp->head_lock, "head_lock");
    
    // cp->first_head_lock = 0;       // Initialized as not locked. Locked = 0 = false. 1 is true --> locked.
    cp->process_count = 0;
    cp->cpu_num = j;
    j++;
  }

  // Initialize head-locks of global lists (sleeping, zombie and unused).
  initlock(&sleeping_head_lock, "sleeping_head_lock");
  initlock(&zombie_head_lock, "zombie_head_lock");
  initlock(&unused_head_lock, "unused_head_lock");

  // mycpu()->first = -1;                      // Initialize the 'first' field of the first cpu (applied to cpu 0 only!).
  // mycpu()->cpu_id = cpuid();

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
      addLink(&unused, p->ind, unused_head_lock);      // Add link to the unused list, if this is not the init proc which is used.
  }
  // printf("unused list: \n");
  // printList(&unused);
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
  // new implementation (using cas).
  int old;
  do{
    old = nextpid;
  } while (cas(&nextpid, old, old+1));
  return old;

/*
  // old implementation (using pidlock).
  int pid;
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
  struct proc *p;
  // Added
  int ind = -1;
  do{
    ind = removeFirst(&unused, unused_head_lock);
  } while (ind == -1);
  p = &proc[ind];
  acquire(&p->lock);
  goto found;
 
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

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  // Added
  // Remove from ZOMBIE list
  if (remove(&zombie, p->ind, zombie_head_lock) == -1)
    printf("Zombie proc not found (not necessarily a problem..).\n");
  
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

  p->cpu_num = 0;
  // Add to UNUSED list
  p->state = UNUSED;
  addLink(&unused, p->ind, unused_head_lock);
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
  p->cpu_num = 0;
  cpus[p->cpu_num].process_count = 1;     // Initialize process_count of the first cpu with 1 (init-proc).
  // add p to cpu runnable list
  // Note: The process was already removed from the 'unused' list in 'allocproc'.
  addLink(&cpus[p->cpu_num].first, p->ind, cpus[p->cpu_num].head_lock);                 // Add this link to this cpu's list.
  release(&p->lock);
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

  #ifdef OFF
  np->cpu_num = p->cpu_num;                     // Same cpu-num as the father process.
  addLink(&cpus[np->cpu_num].first, np->ind, cpus[np->cpu_num].head_lock);   // Adding process link to the father linked-list (after changing to RUNNABLE).
  increase_cpu_counter(&cpus[np->cpu_num]);
  #endif

  #ifdef ON
  // Find cpu with least process_count, add the new process to it's ready-list and incement it's counter.
  struct cpu *least_used_cpu = find_least_used_cpu();
  np->cpu_num = least_used_cpu->cpu_num;
  addLink(&least_used_cpu->first, np->ind, least_used_cpu->head_lock);
  increase_cpu_counter(least_used_cpu);
  #endif
  
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
      return;
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
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  // Added
  // add p to the zombie list
  addLink(&zombie, p->ind, zombie_head_lock);
  // End of addition.

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
  // printf("entered scheduler\n");
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  /*
  if (cpuid() != 0){
    // c->first = -1;              // Initialize 'first' field of other cpus (0 was initialized in procinit).
    // c->process_count = 0;       // Initialize 'process_count' of other cpus (0 was initialized in procinit).
    // c->cpu_id = cpuid();        // Initialize cpu_id of other cpus (0 was initialized in procinit).
  }
  */

 // IF BLNCFLG=OFF:
#ifdef OFF
  int ind;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    while (c->first != -1)       // Ready list of the cpu not empty.
    {
      ind = removeFirst(&c->first, c->head_lock);      
      p = &(proc[ind]);
      acquire(&p->lock);
      if (p->state == ZOMBIE) 
        panic("Running a zombie!!!\n");
      p->state = RUNNING;
      c->proc = p;
      swtch(&c->context, &p->context);
      // Process is done running for now.
      c->proc = 0;
      release(&p->lock);
    }
  }
#endif
// Wsl2 - Check it out.

#ifdef ON
  int stealed_ind;
  int ind;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    // while (c->first != -1)       // Ready list of the cpu not empty.
    if (c->first != -1)
    {
      ind = removeFirst(&c->first, c->head_lock);
      if (ind != -1){           // No-one stole the only process in the list (if there was one..).
        p = &(proc[ind]);
        acquire(&p->lock);
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        
        // Process is done running for now.
        c->proc = 0;
        release(&p->lock);
      }
    }
    
    else{                         // Steal a process from another cpu.
      // cpu_id = steal_procces();
      stealed_ind = steal_process();
      if (stealed_ind != -1){           // Managed to steal a process ;)
        p = &proc[stealed_ind];
        acquire(&p->lock);
        p->cpu_num = c->cpu_num;
        // addLink(&c->first, stealed_ind);
        increase_cpu_counter(c);
        // Run the process.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Finished running this process.
        c->proc = 0;
        release(&p->lock);
      }
    
    }
  }
#endif 
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
  addLink(&cpus[p->cpu_num].first, p->ind, cpus[p->cpu_num].head_lock);
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


  acquire(&p->lock);  //DOC: sleeplock1

  release(lk);
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // Added
  // add p to the sleeping list
  addLink(&sleeping, p->ind, sleeping_head_lock);
  // End of addition.

  sched();

  // Finished sleeping. Return to this line.
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


/*
  int first = sleeping

  int curr = sleeping;
  int next = getNext(curr);
  while (curr != -1){
    p = &proc[curr];
    curr = getNext(curr);
    if (p->chan == chan){
      p->state = RUNNABLE;
        //Added
        // remove p from sleeping
      if (remove(&sleeping, p->ind, sleeping_head_lock) != -1){

        #ifdef OFF
        addLink(&cpus[p->cpu_num].first, p->ind, cpus[p->cpu_num].head_lock);
        increase_cpu_counter(&cpus[p->cpu_num]);
        #endif

        #ifdef ON
        struct cpu *winner;
        // add p to the ready-list (runnable-list) of the cpu with the lowest process_count.
        winner = find_least_used_cpu();
        // Add the process to the cpu with the lowest process_count, and increase its process_count.
        addLink(&winner->first, p->ind, winner->head_lock);
        // Old line (bug I think)
        // addLink(&winner->first, p->ind, cpus[p->cpu_num].head_lock);
        increase_cpu_counter(winner);
        #endif

      }
      // else{
      //   printf("Sleeping process not found in sleeping (Someone else took it?)\n");
      // }
    }
  }
*/

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        //Added
        // remove p from sleeping
        if (remove(&sleeping, p->ind, sleeping_head_lock) != -1){

          #ifdef OFF
          addLink(&cpus[p->cpu_num].first, p->ind, cpus[p->cpu_num].head_lock);
          increase_cpu_counter(&cpus[p->cpu_num]);
          #endif

          #ifdef ON
          struct cpu *winner;
          // add p to the ready-list (runnable-list) of the cpu with the lowest process_count.
          winner = find_least_used_cpu();
          // Add the process to the cpu with the lowest process_count, and increase its process_count.
          addLink(&winner->first, p->ind, winner->head_lock);
          // Old line (bug I think)
          // addLink(&winner->first, p->ind, cpus[p->cpu_num].head_lock);
          increase_cpu_counter(winner);
          #endif
  
        }
        else{
          printf("Problem!@#$ Sleeping process not found in sleeping (Someone else took it?)\n");
        }
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
        // Added (Should we also use ifdef here to perform optimization? I think so.)
        if (remove(&sleeping, p->ind, sleeping_head_lock) != -1)
          addLink(&cpus[p->cpu_num].first, p->ind, cpus[p->cpu_num].head_lock);
        else
          printf("Problem!@#$ Sleeping process not found in sleeping 2(Someone else took it?)\n");
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
  yield();                  // Yield the cpu, so the process is added to the ready-list of the appropriate cpu.
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

// Return the process_count of the cpu with id cpu_num, or (-1) if there is no such cpu in the system.
int
cpu_process_count(int cpu_num){
  for (struct cpu *ct = cpus; ct < &cpus[NCPU]; ct++){
    if (ct->cpu_num == cpu_num)
      return ct->process_count;
  }
  return -1;
}

// Check the LinkedList implementation
void
check_LL(void)
{
  printf("checking LL implementation...\n");
  struct spinlock newList_head_lock;
  
  // int newList_head_lock = 0;
  initlock(&newList_head_lock, "newListHeadLock");
  int newList;
  remove(&unused, 55, unused_head_lock);
  newList = 55;

  remove(&unused, 56, unused_head_lock);
  addLink(&newList, 56, newList_head_lock);
  
  remove(&unused, 57, unused_head_lock);
  addLink(&newList, 57, newList_head_lock);

  printList(&newList);
  
  int removed = removeFirst(&newList, newList_head_lock);
  printf("removed link with proc ind: %d\n", removed);

  printList(&newList);
  
  removed = remove(&newList, 57, newList_head_lock);
  printf("removed link with proc ind: %d\n", 57);
  
  printList(&newList);

  return;
  
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
