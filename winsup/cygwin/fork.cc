/* fork.cc

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2004, 2005, 2006,
   2007, 2008, 2009, 2010, 2011 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "cygerrno.h"
#include "sigproc.h"
#include "pinfo.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "child_info.h"
#include "cygtls.h"
#include "tls_pbuf.h"
#include "dll_init.h"
#include "cygmalloc.h"
#include "ntdll.h"

#define NPIDS_HELD 4

/* Timeout to wait for child to start, parent to init child, etc.  */
/* FIXME: Once things stabilize, bump up to a few minutes.  */
#define FORK_WAIT_TIMEOUT (300 * 1000)     /* 300 seconds */

class frok
{
  bool load_dlls;
  child_info_fork ch;
  const char *errmsg;
  int child_pid;
  int this_errno;
  HANDLE hchild;
  int __stdcall parent (volatile char * volatile here);
  int __stdcall child (volatile char * volatile here);
  bool error (const char *fmt, ...);
  friend int fork ();
};

class lock_signals
{
  bool worked;
public:
  lock_signals ()
  {
    worked = sig_send (NULL, __SIGHOLD) == 0;
  }
  operator int () const
  {
    return worked;
  }
  void dont_bother ()
  {
    worked = false;
  }
  ~lock_signals ()
  {
    if (worked)
      sig_send (NULL, __SIGNOHOLD);
  }
};

class lock_pthread
{
  bool bother;
public:
  lock_pthread (): bother (1)
  {
    pthread::atforkprepare ();
  }
  void dont_bother ()
  {
    bother = false;
  }
  ~lock_pthread ()
  {
    if (bother)
      pthread::atforkparent ();
  }
};

class hold_everything
{
public: /* DELETEME*/
  bool& ischild;
  /* Note the order of the locks below.  It is important,
     to avoid races, that the lock order be preserved.

     pthread is first because it serves as a master lock
     against other forks being attempted while this one is active.

     signals is next to stop signal processing for the duration
     of the fork.

     process is last.  If it is put before signals, then a deadlock
     could be introduced if the process attempts to exit due to a signal. */
  lock_pthread pthread;
  lock_signals signals;
  lock_process process;

public:
  hold_everything (bool& x): ischild (x) {}
  operator int () const {return signals;}

  ~hold_everything()
  {
    if (ischild)
      {
	pthread.dont_bother ();
	process.dont_bother ();
	signals.dont_bother ();
      }
  }

};

static void
resume_child (HANDLE forker_finished)
{
  SetEvent (forker_finished);
  debug_printf ("signalled child");
  return;
}

/* Notify parent that it is time for the next step. */
static void __stdcall
sync_with_parent (const char *s, bool hang_self)
{
  debug_printf ("signalling parent: %s", s);
  fork_info->ready (false);
  if (hang_self)
    {
      HANDLE h = fork_info->forker_finished;
      /* Wait for the parent to fill in our stack and heap.
	 Don't wait forever here.  If our parent dies we don't want to clog
	 the system.  If the wait fails, we really can't continue so exit.  */
      DWORD psync_rc = WaitForSingleObject (h, FORK_WAIT_TIMEOUT);
      debug_printf ("awake");
      switch (psync_rc)
	{
	case WAIT_TIMEOUT:
	  api_fatal ("WFSO timed out %s", s);
	  break;
	case WAIT_FAILED:
	  if (GetLastError () == ERROR_INVALID_HANDLE &&
	      WaitForSingleObject (fork_info->forker_finished, 1) != WAIT_FAILED)
	    break;
	  api_fatal ("WFSO failed %s, fork_finished %p, %E", s,
		     fork_info->forker_finished);
	  break;
	default:
	  debug_printf ("no problems");
	  break;
	}
    }
}

bool
frok::error (const char *fmt, ...)
{
  DWORD exit_code = ch.exit_code;
  if (!exit_code && hchild)
    {
      exit_code = ch.proc_retry (hchild);
      if (!exit_code)
	return false;
    }
  if (exit_code != EXITCODE_FORK_FAILED)
    {
      va_list ap;
      static char buf[NT_MAX_PATH + 256];
      va_start (ap, fmt);
      __small_vsprintf (buf, fmt, ap);
      errmsg = buf;
    }
  return true;
}

int __stdcall
frok::child (volatile char * volatile here)
{
  HANDLE& hParent = ch.parent;
  extern void fixup_lockf_after_fork ();
  extern void fixup_hooks_after_fork ();
  extern void fixup_timers_after_fork ();

  /* NOTE: Logically this belongs in dll_list::load_after_fork, but by
     doing it here, before the first sync_with_parent, we can exploit
     the existing retry mechanism in hopes of getting a more favorable
     address space layout next time. */
  dlls.reserve_space ();

  sync_with_parent ("after longjmp", true);
  debug_printf ("child is running.  pid %d, ppid %d, stack here %p",
		myself->pid, myself->ppid, __builtin_frame_address (0));
  sigproc_printf ("hParent %p, load_dlls %d", hParent, load_dlls);

  /* If we've played with the stack, stacksize != 0.  That means that
     fork() was invoked from other than the main thread.  Make sure that
     the threadinfo information is properly set up.  */
  if (fork_info->stackaddr)
    {
      _main_tls = &_my_tls;
      _main_tls->init_thread (NULL, NULL);
      _main_tls->local_clib = *_impure_ptr;
      _impure_ptr = &_main_tls->local_clib;
    }

  set_cygwin_privileges (hProcToken);
  clear_procimptoken ();
  cygheap->user.reimpersonate ();

#ifdef DEBUGGING
  if (GetEnvironmentVariableA ("FORKDEBUG", NULL, 0))
    try_to_debug ();
  char buf[80];
  /* This is useful for debugging fork problems.  Use gdb to attach to
     the pid reported here. */
  if (GetEnvironmentVariableA ("CYGWIN_FORK_SLEEP", buf, sizeof (buf)))
    {
      small_printf ("Sleeping %d after fork, pid %u\n", atoi (buf), GetCurrentProcessId ());
      Sleep (atoi (buf));
    }
#endif

  MALLOC_CHECK;

  /* Incredible but true:  If we use sockets and SYSV IPC shared memory,
     there's a good chance that a duplicated socket in the child occupies
     memory which is needed to duplicate shared memory from the parent
     process, if the shared memory hasn't been duplicated already.
     The same goes very likely for "normal" mmap shared memory, too, but
     with SYSV IPC it was the first time observed.  So, *never* fixup
     fdtab before fixing up shared memory. */
  if (fixup_shms_after_fork ())
    api_fatal ("recreate_shm areas after fork failed");

  MALLOC_CHECK;

  /* If we haven't dynamically loaded any dlls, just signal
     the parent.  Otherwise, load all the dlls, tell the parent
      that we're done, and wait for the parent to fill in the.
      loaded dlls' data/bss. */
  if (!load_dlls)
    {
      cygheap->fdtab.fixup_after_fork (hParent);
      sync_with_parent ("performed fork fixup", false);
    }
  else
    {
      dlls.load_after_fork (hParent);
      cygheap->fdtab.fixup_after_fork (hParent);
      sync_with_parent ("loaded dlls", true);
    }

  init_console_handler (myself->ctty > 0);
  ForceCloseHandle1 (fork_info->forker_finished, forker_finished);

  pthread::atforkchild ();
  fixup_timers_after_fork ();
  cygbench ("fork-child");
  ld_preload ();
  fixup_hooks_after_fork ();
  _my_tls.fixup_after_fork ();
  cygwin_finished_initializing = true;
  return 0;
}

#define NO_SLOW_PID_REUSE
#ifndef NO_SLOW_PID_REUSE
static void
slow_pid_reuse (HANDLE h)
{
  static NO_COPY HANDLE last_fork_procs[NPIDS_HELD];
  static NO_COPY unsigned nfork_procs;

  if (nfork_procs >= (sizeof (last_fork_procs) / sizeof (last_fork_procs [0])))
    nfork_procs = 0;
  /* Keep a list of handles to child processes sitting around to prevent
     Windows from reusing the same pid n times in a row.  Having the same pids
     close in succesion confuses bash.  Keeping a handle open will stop
     windows from reusing the same pid.  */
  if (last_fork_procs[nfork_procs])
    ForceCloseHandle1 (last_fork_procs[nfork_procs], fork_stupidity);
  if (DuplicateHandle (GetCurrentProcess (), h,
		       GetCurrentProcess (), &last_fork_procs[nfork_procs],
		       0, FALSE, DUPLICATE_SAME_ACCESS))
    ProtectHandle1 (last_fork_procs[nfork_procs], fork_stupidity);
  else
    {
      last_fork_procs[nfork_procs] = NULL;
      system_printf ("couldn't create last_fork_proc, %E");
    }
  nfork_procs++;
}
#endif

int __stdcall
frok::parent (volatile char * volatile stack_here)
{
  HANDLE forker_finished;
  DWORD rc;
  child_pid = -1;
  this_errno = 0;
  bool fix_impersonation = false;
  pinfo child;

  int c_flags = GetPriorityClass (GetCurrentProcess ());
  debug_printf ("priority class %d", c_flags);

  errmsg = NULL;
  hchild = NULL;

  /* If we don't have a console, then don't create a console for the
     child either.  */
  HANDLE console_handle = CreateFile ("CONOUT$", GENERIC_WRITE,
				      FILE_SHARE_READ | FILE_SHARE_WRITE,
				      &sec_none_nih, OPEN_EXISTING,
				      FILE_ATTRIBUTE_NORMAL, NULL);

  if (console_handle != INVALID_HANDLE_VALUE)
    CloseHandle (console_handle);
  else
    c_flags |= DETACHED_PROCESS;

  /* Some file types (currently only sockets) need extra effort in the
     parent after CreateProcess and before copying the datastructures
     to the child. So we have to start the child in suspend state,
     unfortunately, to avoid a race condition. */
  if (cygheap->fdtab.need_fixup_before ())
    c_flags |= CREATE_SUSPENDED;

  /* Remember if we need to load dynamically linked dlls.
     We do this here so that this information will be available
     in the parent and, when the stack is copied, in the child. */
  load_dlls = dlls.reload_on_fork && dlls.loaded_dlls;

  forker_finished = CreateEvent (&sec_all, FALSE, FALSE, NULL);
  if (forker_finished == NULL)
    {
      this_errno = geterrno_from_win_error ();
      error ("unable to allocate forker_finished event");
      return -1;
    }

  ProtectHandleINH (forker_finished);

  ch.forker_finished = forker_finished;

  ch.stackbottom = _tlsbase;
  ch.stacktop = (void *) _tlstop;
  ch.stackaddr = 0;
  ch.guardsize = 0;
  if (&_my_tls != _main_tls)
    {
      /* We have not been started from the main thread.  Fetch the
	 information required to set up the thread stack identically
	 in the child. */
      PTEB teb = NtCurrentTeb ();
      if (!teb->DeallocationStack)
	{
	  /* Pthread with application-provided stack.  Don't set up a
	     PAGE_GUARD page.  guardsize == -1 is used in alloc_stack_hard_way
	     to recognize this type of stack. */
	  ch.stackaddr = _my_tls.tid->attr.stackaddr;
	  ch.guardsize = (size_t) -1;
	}
      else
	{
	  ch.stackaddr = teb->DeallocationStack;
	  /* If it's a pthread, fetch guardsize from thread attributes. */
	  if (_my_tls.tid)
	    ch.guardsize = _my_tls.tid->attr.guardsize;
	}
    }
  debug_printf ("stack - bottom %p, top %p, addr %p, guardsize %p",
		ch.stackbottom, ch.stacktop, ch.stackaddr, ch.guardsize);

  PROCESS_INFORMATION pi;
  STARTUPINFOW si;

  memset (&si, 0, sizeof (si));
  si.cb = sizeof si;

  si.lpReserved2 = (LPBYTE) &ch;
  si.cbReserved2 = sizeof (ch);

  syscall_printf ("CreateProcessW (%W, %W, 0, 0, 1, %p, 0, 0, %p, %p)",
		  myself->progname, myself->progname, c_flags, &si, &pi);
  bool locked = __malloc_lock ();
  time_t start_time = time (NULL);

  /* Remove impersonation */
  cygheap->user.deimpersonate ();
  fix_impersonation = true;
  ch.refresh_cygheap ();

  while (1)
    {
      hchild = NULL;
      rc = CreateProcessW (myself->progname, /* image to run */
			   myself->progname, /* what we send in arg0 */
			   &sec_none_nih,
			   &sec_none_nih,
			   TRUE,	  /* inherit handles from parent */
			   c_flags,
			   NULL,	  /* environment filled in later */
			   0,	  /* use current drive/directory */
			   &si,
			   &pi);

      if (rc)
	debug_printf ("forked pid %u", pi.dwProcessId);
      else
	{
	  this_errno = geterrno_from_win_error ();
	  error ("CreateProcessW failed for '%W'", myself->progname);
	  memset (&pi, 0, sizeof (pi));
	  goto cleanup;
	}

      if (cygheap->fdtab.need_fixup_before ())
	{
	  cygheap->fdtab.fixup_before_fork (pi.dwProcessId);
	  ResumeThread (pi.hThread);
	}

      CloseHandle (pi.hThread);
      hchild = pi.hProcess;

      /* Protect the handle but name it similarly to the way it will
	 be called in subproc handling. */
      ProtectHandle1 (hchild, childhProc);

      strace.write_childpid (pi.dwProcessId);

      /* Wait for subproc to initialize itself. */
      if (!ch.sync (pi.dwProcessId, hchild, FORK_WAIT_TIMEOUT))
	{
	  if (!error ("forked process died unexpectedly, retry %d, exit code %d",
		      ch.retry, ch.exit_code))
	    continue;
	  this_errno = EAGAIN;
	  goto cleanup;
	}
      break;
    }

  /* Restore impersonation */
  cygheap->user.reimpersonate ();
  fix_impersonation = false;

  child_pid = cygwin_pid (pi.dwProcessId);
  child.init (child_pid, 1, NULL);

  if (!child)
    {
      this_errno = get_errno () == ENOMEM ? ENOMEM : EAGAIN;
      syscall_printf ("pinfo failed");
      goto cleanup;
    }

  child->start_time = start_time; /* Register child's starting time. */
  child->nice = myself->nice;

  /* Initialize things that are done later in dll_crt0_1 that aren't done
     for the forkee.  */
  wcscpy (child->progname, myself->progname);

  /* Fill in fields in the child's process table entry.  */
  child->dwProcessId = pi.dwProcessId;
  child.hProcess = hchild;

  /* Hopefully, this will succeed.  The alternative to doing things this
     way is to reserve space prior to calling CreateProcess and then fill
     it in afterwards.  This requires more bookkeeping than I like, though,
     so we'll just do it the easy way.  So, terminate any child process if
     we can't actually record the pid in the internal table. */
  if (!child.remember (false))
    {
      TerminateProcess (hchild, 1);
      this_errno = EAGAIN;
#ifdef DEBUGGING0
      error ("child remember failed");
#endif
      goto cleanup;
    }

#ifndef NO_SLOW_PID_REUSE
  slow_pid_reuse (hchild);
#endif

  /* CHILD IS STOPPED */
  debug_printf ("child is alive (but stopped)");


  /* Initialize, in order: stack, dll data, dll bss.
     data, bss, heap were done earlier (in dcrt0.cc)
     Note: variables marked as NO_COPY will not be copied since they are
     placed in a protected segment.  */

  MALLOC_CHECK;
  const void *impure_beg;
  const void *impure_end;
  const char *impure;
  if (&_my_tls == _main_tls)
    impure_beg = impure_end = impure = NULL;
  else
    {
      impure = "impure";
      impure_beg = _impure_ptr;
      impure_end = _impure_ptr + 1;
    }
  rc = child_copy (hchild, true,
		   "stack", stack_here, ch.stackbottom,
		   impure, impure_beg, impure_end,
		   NULL);

  __malloc_unlock ();
  locked = false;
  MALLOC_CHECK;
  if (!rc)
    {
      this_errno = get_errno ();
      error ("pid %u, exitval %p", pi.dwProcessId, ch.exit_code);
      goto cleanup;
    }

  /* Now fill data/bss of any DLLs that were linked into the program. */
  for (dll *d = dlls.istart (DLL_LINK); d; d = dlls.inext ())
    {
      debug_printf ("copying data/bss of a linked dll");
      if (!child_copy (hchild, true,
		       "linked dll data", d->p.data_start, d->p.data_end,
		       "linked dll bss", d->p.bss_start, d->p.bss_end,
		       NULL))
	{
	  this_errno = get_errno ();
	  error ("couldn't copy linked dll data/bss");
	  goto cleanup;
	}
    }

  /* Start thread, and then wait for it to reload dlls.  */
  resume_child (forker_finished);
  if (!ch.sync (child->pid, hchild, FORK_WAIT_TIMEOUT))
    {
      this_errno = EAGAIN;
      error ("died waiting for dll loading");
      goto cleanup;
    }

  /* If DLLs were loaded in the parent, then the child has reloaded all
     of them and is now waiting to have all of the individual data and
     bss sections filled in. */
  if (load_dlls)
    {
      /* CHILD IS STOPPED */
      /* write memory of reloaded dlls */
      for (dll *d = dlls.istart (DLL_LOAD); d; d = dlls.inext ())
	{
	  debug_printf ("copying data/bss for a loaded dll");
	  if (!child_copy (hchild, true,
			   "loaded dll data", d->p.data_start, d->p.data_end,
			   "loaded dll bss", d->p.bss_start, d->p.bss_end,
			   NULL))
	    {
	      this_errno = get_errno ();
#ifdef DEBUGGING
	      error ("copying data/bss for a loaded dll");
#endif
	      goto cleanup;
	    }
	}
      /* Start the child up again. */
      resume_child (forker_finished);
    }

  ForceCloseHandle (forker_finished);
  forker_finished = NULL;

  return child_pid;

/* Common cleanup code for failure cases */
cleanup:
  if (fix_impersonation)
    cygheap->user.reimpersonate ();
  if (locked)
    __malloc_unlock ();

  /* Remember to de-allocate the fd table. */
  if (hchild && !child.hProcess)
    ForceCloseHandle1 (hchild, childhProc);
  if (forker_finished)
    ForceCloseHandle (forker_finished);
  debug_printf ("returning -1");
  return -1;
}

extern "C" int
fork ()
{
  frok grouped;

  debug_printf ("entering");
  grouped.load_dlls = 0;

  int res;
  bool ischild = false;

  myself->set_has_pgid_children ();

  if (grouped.ch.parent == NULL)
    return -1;
  if (grouped.ch.subproc_ready == NULL)
    {
      system_printf ("unable to allocate subproc_ready event, %E");
      return -1;
    }

  {
    hold_everything held_everything (ischild);
    /* This tmp_pathbuf constructor is required here because the below setjmp
       magic will otherwise not restore the original buffer count values in
       the thread-local storage.  A process forking too deeply will run into
       the problem to be out of temporary TLS path buffers. */
    tmp_pathbuf tp;

    if (!held_everything)
      {
	if (exit_state)
	  Sleep (INFINITE);
	set_errno (EAGAIN);
	return -1;
      }

    /* Put the dll list in topological dependency ordering, in
       hopes that the child will have a better shot at loading dlls
       properly if it only has to deal with one at a time.  */
    dlls.topsort ();

    ischild = !!setjmp (grouped.ch.jmp);

    volatile char * volatile esp;
    __asm__ volatile ("movl %%esp,%0": "=r" (esp));

    if (!ischild)
      res = grouped.parent (esp);
    else
      {
	res = grouped.child (esp);
	in_forkee = false;
	ischild = true;	/* might have been reset by fork mem copy */
      }
  }

  MALLOC_CHECK;
  if (ischild)
    {
      myself->process_state |= PID_ACTIVE;
      myself->process_state &= ~(PID_INITIALIZING | PID_EXITED | PID_REAPED);
    }
  else if (res < 0)
    {
      if (!grouped.errmsg)
	syscall_printf ("fork failed - child pid %d, errno %d", grouped.child_pid, grouped.this_errno);
      else
	{
	  char buf[strlen (grouped.errmsg) + sizeof ("child %d - , errno 4294967295  ")];
	  strcpy (buf, "child %d - ");
	  strcat (buf, grouped.errmsg);
	  strcat (buf, ", errno %d");
	  system_printf (buf, grouped.child_pid, grouped.this_errno);
	}

      set_errno (grouped.this_errno);
    }
  syscall_printf ("%R = fork()", res);
  return res;
}
#ifdef DEBUGGING
void
fork_init ()
{
}
#endif /*DEBUGGING*/


extern "C" int
vfork ()
{
  debug_printf ("stub called");
  return fork ();
}

/* Copy memory from one process to another. */

bool
child_copy (HANDLE hp, bool write, ...)
{
  va_list args;
  va_start (args, write);
  static const char *huh[] = {"read", "write"};

  char *what;
  while ((what = va_arg (args, char *)))
    {
      char *low = va_arg (args, char *);
      char *high = va_arg (args, char *);
      DWORD todo = high - low;
      char *here;

      for (here = low; here < high; here += todo)
	{
	  DWORD done = 0;
	  if (here + todo > high)
	    todo = high - here;
	  int res;
	  if (write)
	    res = WriteProcessMemory (hp, here, here, todo, &done);
	  else
	    res = ReadProcessMemory (hp, here, here, todo, &done);
	  debug_printf ("%s - hp %p low %p, high %p, res %d", what, hp, low, high, res);
	  if (!res || todo != done)
	    {
	      if (!res)
		__seterrno ();
	      /* If this happens then there is a bug in our fork
		 implementation somewhere. */
	      system_printf ("%s %s copy failed, %p..%p, done %d, windows pid %u, %E",
			    what, huh[write], low, high, done, myself->dwProcessId);
	      goto err;
	    }
	}
    }

  va_end (args);
  debug_printf ("done");
  return true;

 err:
  va_end (args);
  TerminateProcess (hp, 1);
  set_errno (EAGAIN);
  return false;
}
