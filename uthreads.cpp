#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <sys/time.h>
#include <iostream>
#include <deque>
#include <map>
#include <set>
#include <algorithm>
#include "uthreads.h"

// ------------------------------ DEFINITIONS ------------------------------ //

#define JB_SP 6
#define JB_PC 7

#define AVAILABLE 1
#define UNAVAILABLE -1

typedef unsigned long address_t;

// --------------------------- GLOBAL VARIABLES --------------------------- //
struct itimerval running_timer;
struct sigaction sa_timer = {0};
struct itimerval clock_timer;
struct sigaction sa_clock = {0};
sigjmp_buf env[MAX_THREAD_NUM];
int available_ids[MAX_THREAD_NUM];
char *stacks[MAX_THREAD_NUM];
int quantums[MAX_THREAD_NUM] = {0};
int running_thread;
std::deque<int> ready_deque;
std::set<int> blocked_set;
std::map<int, int> sleeping_threads;
int total_quantums;

// ------------------------------- FUNCTIONS ------------------------------- //

void jump_to_thread ()
{
  if (setitimer (ITIMER_VIRTUAL, &running_timer, nullptr))
  {
    std::cerr << "system error: setitimer error." << std::endl;
  }

  sigsetjmp(env[running_thread], 1);

  // take the first ready thread and put it in running
  running_thread = ready_deque.front ();
  ready_deque.pop_front ();

  quantums[running_thread]++;

  std::cout << "running thread: " << running_thread << std::endl;

  siglongjmp (env[running_thread], 1);
}

int get_tid ()
{
  for (int i = 1; i < MAX_THREAD_NUM; i++)
  {
    int status = available_ids[i];
    if (status == AVAILABLE)
    {
      return i;
    }
  }
  return -1;
}

address_t translate_address (address_t addr)
{
  address_t ret;
  asm volatile("xor    %%fs:0x30,%0\n"
               "rol    $0x11,%0\n"
  : "=g" (ret)
  : "0" (addr));
  return ret;
}

bool is_valid_tid (int tid)
{
  if ((tid < 0) | (tid >= MAX_THREAD_NUM) | (available_ids[tid] == AVAILABLE))
  {
    return false;
  }
  return true;
}

void timer_handler (int sig)
{
  ready_deque.push_back(running_thread);
  jump_to_thread ();
}

void terminate_main ()
{
  for (auto stack: stacks)
  {
    delete[] stack;
  }
  exit (0);
}

void clock_handler (int sig)
{
  total_quantums++;

  for (auto &entry: sleeping_threads)
  {
    entry.second--;
    if (entry.second == 0)
    {
      sleeping_threads.erase (entry.first);
      ready_deque.push_back (entry.first);
    }
  }
}

// ---------------------------------- API ---------------------------------- //

int uthread_init (int quantum_usecs)
{
  if(quantum_usecs <= 0) {
    std::cerr << "thread library error: quantum should be a positive number" << std::endl;
    return -1;
  }
  running_thread = 0;
  available_ids[0] = UNAVAILABLE;
  for(int i = 1; i < MAX_THREAD_NUM; i++) {
    available_ids[i] = AVAILABLE;
  }

  // create a running_timer for the running state
  sa_timer.sa_handler = &timer_handler;
  if (sigaction (SIGVTALRM, &sa_timer, nullptr) < 0)
  {
    std::cerr << "system error: sigaction error." << std::endl;
  }

  running_timer.it_interval.tv_sec = 0;
  running_timer.it_interval.tv_usec = quantum_usecs;

  // Start a virtual running_timer. It counts down whenever this process is executing.
  if (setitimer (ITIMER_VIRTUAL, &running_timer, nullptr))
  {
    std::cerr << "system error: setitimer error." << std::endl;
  }

  sa_clock.sa_handler = &clock_handler;
  if (sigaction (SIGVTALRM, &sa_clock, nullptr) < 0)
  {
    std::cerr << "system error: sigaction error." << std::endl;
  }

  clock_timer.it_interval.tv_sec = 0;
  clock_timer.it_interval.tv_usec = quantum_usecs;

  // Start a virtual running_timer. It counts down whenever this process is executing.
  if (setitimer (ITIMER_VIRTUAL, &clock_timer, nullptr))
  {
    std::cerr << "system error: setitimer error." << std::endl;
  }

  sigaddset(&env[running_thread]->__saved_mask, SIGVTALRM);
  total_quantums = 1;

  return 0;
}

int uthread_spawn (thread_entry_point entry_point)
{
  sigprocmask(SIG_BLOCK, &env[running_thread]->__saved_mask, nullptr);

  if (entry_point == nullptr)
  {
    std::cerr << "thread library error: NULL entry point" << std::endl;
    return -1;
  }

  int tid = get_tid ();
  if (tid == -1)
  {
    std::cerr
        << "thread library error: The number of concurrent threads exceeded the limit"
        << std::endl;
    return -1;
  }

  ready_deque.push_back (tid);
  available_ids[tid] = UNAVAILABLE;

  char *stack = new char[STACK_SIZE];

  stacks[tid] = stack;

  address_t sp = (address_t) stack + STACK_SIZE - sizeof (address_t);
  auto pc = (address_t) entry_point;
  sigsetjmp(env[tid], 1);
  (env[tid]->__jmpbuf)[JB_SP] = translate_address (sp);
  (env[tid]->__jmpbuf)[JB_PC] = translate_address (pc);
  sigemptyset (&env[tid]->__saved_mask);

  //ADDS THE TIMER SIGNAL TO THE SIGNAL MASK IN ORDER TO IGNORE IT
  sigaddset(&env[running_thread]->__saved_mask, SIGVTALRM);

  sigprocmask(SIG_UNBLOCK, &env[running_thread]->__saved_mask, nullptr);

  return tid;
}

int uthread_terminate (int tid)
{
  sigprocmask(SIG_BLOCK, &env[running_thread]->__saved_mask, nullptr);

  if (is_valid_tid (tid))
  {
    available_ids[tid] = AVAILABLE;
  }
  else
  {
    std::cerr << "thread library error: No such thread" << std::endl;
    return -1;
  }

  // check if the tid is in the ready queue or the blocked, and if so removes it

  if (blocked_set.find (tid) != blocked_set.end ())
  {
    blocked_set.erase (tid);
  }

  if (std::find (ready_deque.begin (), ready_deque.end (), tid) !=
      ready_deque.end ())
  {
    ready_deque.erase (std::find (ready_deque.begin (), ready_deque.end (), tid));
  }

  if (tid == 0)
  {
    terminate_main ();
  }
  free (stacks[tid]);
  jump_to_thread ();

  sigprocmask(SIG_UNBLOCK, &env[running_thread]->__saved_mask, nullptr);

  return 0;
}

int uthread_block (int tid)
{
  sigprocmask(SIG_BLOCK, &env[running_thread]->__saved_mask, nullptr);

  //CHECKS IF THE TID IS EVEN VALID
  if (!is_valid_tid (tid))
  {
    std::cerr << "thread library error: invalid tid" << std::endl;
    return -1;
  }
  //CHECKS IF IT ISN'T ALREADY BLOCKED
  if (blocked_set.find (tid) != blocked_set.end ())
  {
    std::cerr << "thread library error: tid is already blocked" << std::endl;
    return -1;
  }
  blocked_set.insert (tid);
  ready_deque.erase (std::find (ready_deque.begin (), ready_deque.end (), tid));
  if (running_thread == tid)
  {
    jump_to_thread ();
  }
  else
  {
    ready_deque.erase (std::find (ready_deque.begin (), ready_deque.end (), tid));
  }

  sigprocmask(SIG_UNBLOCK, &env[running_thread]->__saved_mask, nullptr);

  return 0;
}

int uthread_resume (int tid)
{
  sigprocmask(SIG_BLOCK, &env[running_thread]->__saved_mask, nullptr);

  if (blocked_set.find (tid) == blocked_set.end ())
  {
    std::cerr << "The provided tid doesn't belong to a blocked thread." << std::endl;
    return -1;
  }
  blocked_set.erase (tid);
  ready_deque.push_back (tid);

  sigprocmask(SIG_UNBLOCK, &env[running_thread]->__saved_mask, nullptr);

  return 0;
}

int uthread_sleep (int num_quantums)
{
  sigprocmask(SIG_BLOCK, &env[running_thread]->__saved_mask, nullptr);

  if (running_thread == 0)
  {
    std::cerr << "Cannot call uthread_sleep from the main thread" << std::endl;
    return -1;
  }

  sleeping_threads.insert (std::make_pair(uthread_get_tid (), num_quantums));
  jump_to_thread ();

  sigprocmask(SIG_UNBLOCK, &env[running_thread]->__saved_mask, nullptr);

  return 0;
}

int uthread_get_tid ()
{
  return running_thread;
}

int uthread_get_total_quantums ()
{
  return total_quantums;
}

int uthread_get_quantums (int tid)
{
  return quantums[tid];
}

