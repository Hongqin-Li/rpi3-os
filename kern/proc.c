#include "proc.h"

#include "string.h"
#include "types.h"
#include "memlayout.h"
#include "list.h"
#include "console.h"
#include "mm.h"
#include "vm.h"
#include "spinlock.h"

#include "sd.h"
#include "debug.h"
#include "file.h"
#include "log.h"

extern void trapret();
extern void swtch(struct context **old, struct context *new);

static void forkret();
static void user_init();
static void idle_init();

#define SQSIZE  0x100    /* Must be power of 2. */
#define HASH(x) ((((int)(x)) >> 5) & (SQSIZE - 1))

struct cpu cpu[NCPU];

struct {
  struct list_head slpque[SQSIZE];
  struct list_head sched_que;
  struct spinlock lock;
} ptable;

struct proc *initproc;

void
proc_init()
{
    list_init(&ptable.sched_que);
    for (int i = 0; i < SQSIZE; i++)
        list_init(&ptable.slpque[i]);
    // FIXME: 
    user_init();
}

// TODO: use kmalloc
/*
 * Look in the process table for an UNUSED proc.
 * If found, change state to EMBRYO and initialize
 * state required to run in the kernel.
 * Otherwise return 0.
 */
static struct proc *
proc_alloc()
{
    static struct proc proc[NPROC];
    struct proc *p;
    int found = 0;

    acquire(&ptable.lock);
    for (p = proc; p < &proc[NPROC]; p++) {
        if (p->state == UNUSED) {
            found = 1;
            break;
        }
    }

    if (!found || !(p->kstack = kalloc())) {
        release(&ptable.lock);
        return 0;
    }

    memset(p, 0, sizeof(p));

    p->state = EMBRYO;
    p->pid = p - proc;

    void *sp = p->kstack + PGSIZE;
    assert(sizeof(*p->tf) == 19*16 && sizeof(*p->context) == 8*16);

    sp -= sizeof(*p->tf);
    p->tf = sp;
    /* No user stack for init process. */
    p->tf->spsr = p->tf->sp = 0;

    sp -= sizeof(*p->context);
    p->context = sp;
    p->context->lr0 = (uint64_t)forkret;
    p->context->lr = (uint64_t)trapret;

    list_init(&p->child);
   
    release(&ptable.lock);
    return p;
}

/* Initialize per-cpu idle process. */
static void
idle_init()
{
    cprintf("- idle init\n");

    struct proc *p;
    if (!(p = proc_alloc()))
        panic("idle init: failed\n");

    p->pgdir = vm_init();
    void *va = kalloc();
    uvm_map(p->pgdir, 0, PGSIZE, V2P(va));

    extern char ispin[], eicode[];
    memmove(va, ispin, eicode - ispin);
    assert((size_t)(eicode - ispin) <= PGSIZE);
    p->stksz = 0;
    p->sz = PGSIZE;
    p->base = 0;

    p->tf->elr = 0;

    thiscpu()->idle = p;
}

/*
 * Set up the first user process. Specifically, we need to:
 * - alloc a new proc.
 * - move the code snippet in initcode.S into its virtual memory.
 * - set up link register in trap frame.
 * - mark as RUNNABLE so that our scheduler can swtch to it.
 */
static void
user_init()
{
    cprintf("- user init\n");

    struct proc *p = proc_alloc();
    if (p == 0)
        panic("user init: failed\n");

    if ((p->pgdir = vm_init()) == 0)
        panic("user init: failed\n");

    assert(!initproc);
    initproc = p;

    void *va = kalloc();
    uvm_map(p->pgdir, 0, PGSIZE, V2P(va));

    extern char icode[], eicode[];
    memmove(va, icode, eicode - icode);
    assert((size_t)(eicode - icode) <= PGSIZE);
    p->stksz = 0;
    p->sz = PGSIZE;
    p->base = 0;

    p->tf->elr = 0;

    safestrcpy(p->name, "icode", sizeof(p->name));
    p->cwd = namei("/");

    acquire(&ptable.lock);
    list_push_back(&ptable.sched_que, &p->link);
    release(&ptable.lock);
}

/*
 * Per-CPU process scheduler.
 * Each CPU calls scheduler() after setting itself up.
 * Scheduler never returns. It loops, doing:
 * - choose a process to run
 * - swtch to start running that process
 * - eventually that process transfers control
 *   via swtch back to the scheduler.
 */
void
scheduler()
{
    idle_init();
    for (struct proc *p; ; ) {
        acquire(&ptable.lock);
        struct list_head *head = &ptable.sched_que;
        if (list_empty(head)) {
            p = thiscpu()->idle;
            // cprintf("- scheduler: cpu %d to idle\n", cpuid());
        } else {
            p = container_of(list_front(head), struct proc, link);
            list_pop_front(head);
            // cprintf("- scheduler: cpu %d to pid %d\n", cpuid(), p->pid);
        }
        uvm_switch(p->pgdir);
        thiscpu()->proc = p;
        swtch(&thiscpu()->scheduler, p->context);
        // cprintf("- scheduler: cpu %d back to scheduler from pid %d\n", cpuid(), p->pid);
        thiscpu()->proc = 0;
        release(&ptable.lock);
    }
}


/*
 * A fork child's very first scheduling by scheduler()
 * will swtch here. "Return" to user space.
 */
static void
forkret()
{
    static int first = 1;
    release(&ptable.lock);
    if (first && thisproc() != thiscpu()->idle) {
        first = 0;
        iinit(ROOTDEV);
        initlog(ROOTDEV);
        cprintf("- initlog done!\n");
    }
}

/* Give up CPU. */
void
yield()
{
    struct proc *p = thisproc();
    acquire(&ptable.lock);
    if (p != thiscpu()->idle)
        list_push_back(&ptable.sched_que, &p->link);
    swtch(&p->context, thiscpu()->scheduler);
    release(&ptable.lock);
}

/*
 * Atomically release lock and sleep on chan.
 * Reacquires lock when awakened.
 */
void
sleep(void *chan, struct spinlock *lk)
{
    struct proc *p = thisproc();
    int i = HASH(chan);
    assert(i < SQSIZE);

    if (lk != &ptable.lock) {
        acquire(&ptable.lock);
        release(lk);
    }

    p->chan = chan;
    list_push_back(&ptable.slpque[i], &p->link);

    // cprintf("- cpu %d: sleep pid %d on chan 0x%p\n", cpuid(), p->pid, chan);
    swtch(&thisproc()->context, thiscpu()->scheduler);
    // cprintf("- cpu %d: wake on chan 0x%p\n", cpuid(), chan);

    if (lk != &ptable.lock) {
        acquire(lk);
        release(&ptable.lock);
    }
}

/*
 * Wake up all processes sleeping on chan.
 * The ptable lock must be held.
 */
static void
wakeup1(void *chan)
{
    struct list_head *q = &ptable.slpque[HASH(chan)];
    struct proc *p, *np;

    LIST_FOREACH_ENTRY_SAFE(p, np, q, link) {
        // cprintf("- wakeup1: try pid %d\n", p->pid);
        if (p->chan == chan) {
            // cprintf("- wakeup1: pid %d\n", p->pid);
            list_drop(&p->link);
            list_push_back(&ptable.sched_que, &p->link);
        }
    }

}

/* Wake up all processes sleeping on chan. */
void
wakeup(void *chan)
{
    // cprintf("- wakeup: chan 0x%p\n", chan);
    acquire(&ptable.lock);
    wakeup1(chan);
    release(&ptable.lock);
}

// FIXME: not tested.
// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork()
{
    struct proc *cp = thisproc();
    struct proc *np = proc_alloc();

    if ((np->pgdir = uvm_copy(cp->pgdir)) == 0) {
        kfree(np->kstack);
        np->kstack = 0;

        acquire(&ptable.lock);
        np->state = UNUSED;
        release(&ptable.lock);

        return -1;
    }

    np->base = cp->base;
    np->sz = cp->sz;
    np->stksz = cp->stksz;

    memmove(np->tf, cp->tf, sizeof(*np->tf));

    // Fork returns 0 in the child.
    np->tf->x[0] = 0;

    for (int i = 0; i < NOFILE; i++)
        if (cp->ofile[i])
            np->ofile[i] = filedup(cp->ofile[i]);
    np->cwd = idup(cp->cwd);

    int pid = np->pid;
    np->parent = cp;

    acquire(&ptable.lock);
    list_push_back(&cp->child, &np->clink);
    list_push_back(&ptable.sched_que, &np->link);
    release(&ptable.lock);

    return pid;
}


// FIXME: not tested.
// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait()
{   
    struct proc *cp = thisproc();

    struct list_head *q = &cp->child;
    struct proc *p, *np;

    acquire(&ptable.lock);
    while (!list_empty(q)) {
        LIST_FOREACH_ENTRY_SAFE(p, np, q, clink) {
            if (p->state == ZOMBIE) {
                assert(p->parent == cp);
                // cprintf("wait: weap proc %d\n", p->pid);

                list_drop(&p->clink);

                kfree(p->kstack);
                vm_free(p->pgdir);
                p->state = UNUSED;

                int pid = p->pid;
                release(&ptable.lock);
                return pid;
            }
        }
        sleep(cp, &ptable.lock);
    }
    release(&ptable.lock);
    return -1;
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
    panic("not implemented");
//   struct proc *p;

//   acquire(&ptable.lock);
//   for(p = &proc; p < &proc[NPROC]; p++){
//     if(p->pid == pid){
//       p->killed = 1;
//       // Wake process from sleep if necessary.
//       if(p->state == SLEEPING)
//         p->state = RUNNABLE;
//       release(&ptable.lock);
//       return 0;
//     }
//   }
//   release(&ptable.lock);
//   return -1;
}

// FIXME: not tested.
// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(int code)
{
    struct proc *cp = thisproc();

    if (cp == initproc)
        panic("init exiting");

    // Close all open files.
    for (int fd = 0; fd < NOFILE; fd++) {
        if (cp->ofile[fd]) {
            fileclose(cp->ofile[fd]);
            cp->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(cp->cwd);
    end_op();
    cp->cwd = 0;

    acquire(&ptable.lock);

    // Parent might be sleeping in wait().
    wakeup1(cp->parent);

    // Pass abandoned children to init.
    struct list_head *q = &cp->child;
    struct proc *p, *np;
    LIST_FOREACH_ENTRY_SAFE(p, np, q, clink) {
        assert(p->parent == cp);
        // cprintf("exit: pass child %d to init\n", p->pid);
        p->parent = initproc;

        list_drop(&p->clink);
        list_push_back(&initproc->child, &p->clink);
        if (p->state == ZOMBIE)
            wakeup1(initproc);
    }
    assert(list_empty(q));
 
    // Jump into the scheduler, never to return.
    cp->state = ZOMBIE;
    swtch(&cp->context, thiscpu()->scheduler);
    panic("zombie exit");
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
    panic("not implemented");
//   static char *states[] = {
//   [UNUSED]    "unused",
//   [EMBRYO]    "embryo",
//   [SLEEPING]  "sleep ",
//   [RUNNABLE]  "runble",
//   [RUNNING]   "run   ",
//   [ZOMBIE]    "zombie"
//   };
//   int i;
//   struct proc *p;
//   char *state;
//   uint pc[10];

//   for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
//     if(p->state == UNUSED)
//       continue;
//     if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
//       state = states[p->state];
//     else
//       state = "???";
//     cprintf("%d %s %s", p->pid, state, p->name);
//     if(p->state == SLEEPING){
//       getcallerpcs((uint*)p->context->ebp+2, pc);
//       for(i=0; i<10 && pc[i] != 0; i++)
//         cprintf(" %p", pc[i]);
//     }
//     cprintf("\n");
//   }
}
