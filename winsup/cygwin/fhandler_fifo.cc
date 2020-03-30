/* fhandler_fifo.cc - See fhandler.h for a description of the fhandler classes.

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

#include "winsup.h"
#include <w32api/winioctl.h>
#include "miscfuncs.h"

#include "cygerrno.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "sigproc.h"
#include "cygtls.h"
#include "shared_info.h"
#include "ntdll.h"
#include "cygwait.h"

/*
   Overview:

     Currently a FIFO can be opened once for reading and multiple
     times for writing.  Any attempt to open the FIFO a second time
     for reading fails with EACCES (from STATUS_ACCESS_DENIED).

     When a FIFO is opened for reading,
     fhandler_fifo::create_pipe_instance is called to create the first
     instance of a Windows named pipe server (Windows terminology).  A
     "fifo_reader" thread is also started; it waits for pipe clients
     (Windows terminology again) to connect.  This happens every time
     a process opens the FIFO for writing.

     The fifo_reader thread creates new instances of the pipe server
     as needed, so that there is always an instance available for a
     writer to connect to.

     The reader maintains a list of "fifo_client_handlers", one for
     each pipe instance.  A fifo_client_handler manages the connection
     between the pipe instance and a writer connected to that pipe
     instance.

     TODO: Allow a FIFO to be opened multiple times for reading.
     Maybe this could be done by using shared memory, so that all
     readers could have access to the same list of writers.
*/


/* This is only to be used for writers.  When reading,
STATUS_PIPE_EMPTY simply means there's no data to be read. */
#define STATUS_PIPE_IS_CLOSED(status)	\
		({ NTSTATUS _s = (status); \
		   _s == STATUS_PIPE_CLOSING \
		   || _s == STATUS_PIPE_BROKEN \
		   || _s == STATUS_PIPE_EMPTY; })

#define STATUS_PIPE_NO_INSTANCE_AVAILABLE(status)	\
		({ NTSTATUS _s = (status); \
		   _s == STATUS_INSTANCE_NOT_AVAILABLE \
		   || _s == STATUS_PIPE_NOT_AVAILABLE \
		   || _s == STATUS_PIPE_BUSY; })

static NO_COPY fifo_reader_id_t null_fr_id = { .winpid = 0, .fh = NULL };

fhandler_fifo::fhandler_fifo ():
  fhandler_base (),
  read_ready (NULL), write_ready (NULL), cancel_evt (NULL), sync_thr (NULL),
  nhandlers (0), nconnected (0),
  reader (false), writer (false), duplexer (false),
  max_atomic_write (DEFAULT_PIPEBUFSIZE),
  shmem_handle (NULL), shmem (NULL), me (null_fr_id)
{
  pipe_name_buf[0] = L'\0';
  need_fork_fixup (true);
}

PUNICODE_STRING
fhandler_fifo::get_pipe_name ()
{
  if (!pipe_name_buf[0])
    {
      __small_swprintf (pipe_name_buf, L"%S-fifo.%08x.%016X",
			&cygheap->installation_key, get_dev (), get_ino ());
      RtlInitUnicodeString (&pipe_name, pipe_name_buf);
    }
  return &pipe_name;
}

inline PSECURITY_ATTRIBUTES
sec_user_cloexec (bool cloexec, PSECURITY_ATTRIBUTES sa, PSID sid)
{
  return cloexec ? sec_user_nih (sa, sid) : sec_user (sa, sid);
}

bool inline
fhandler_fifo::arm (HANDLE h)
{
#ifdef DEBUGGING
  const char *what;
  if (h == read_ready)
    what = "reader";
  else
    what = "writer";
  debug_only_printf ("arming %s", what);
#endif

  bool res = SetEvent (h);
  if (!res)
#ifdef DEBUGGING
    debug_printf ("SetEvent for %s failed, %E", what);
#else
    debug_printf ("SetEvent failed, %E");
#endif
  return res;
}

static HANDLE
create_event ()
{
  NTSTATUS status;
  OBJECT_ATTRIBUTES attr;
  HANDLE evt = NULL;

  InitializeObjectAttributes (&attr, NULL, 0, NULL, NULL);
  status = NtCreateEvent (&evt, EVENT_ALL_ACCESS, &attr,
			  NotificationEvent, FALSE);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return evt;
}


static void
set_pipe_non_blocking (HANDLE ph, bool nonblocking)
{
  NTSTATUS status;
  IO_STATUS_BLOCK io;
  FILE_PIPE_INFORMATION fpi;

  fpi.ReadMode = FILE_PIPE_MESSAGE_MODE;
  fpi.CompletionMode = nonblocking ? FILE_PIPE_COMPLETE_OPERATION
    : FILE_PIPE_QUEUE_OPERATION;
  status = NtSetInformationFile (ph, &io, &fpi, sizeof fpi,
				 FilePipeInformation);
  if (!NT_SUCCESS (status))
    debug_printf ("NtSetInformationFile(FilePipeInformation): %y", status);
}

NTSTATUS
fhandler_fifo::npfs_handle (HANDLE &nph)
{
  static NO_COPY SRWLOCK npfs_lock;
  static NO_COPY HANDLE npfs_dirh;

  NTSTATUS status = STATUS_SUCCESS;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;

  /* Lockless after first call. */
  if (npfs_dirh)
    {
      nph = npfs_dirh;
      return STATUS_SUCCESS;
    }
  AcquireSRWLockExclusive (&npfs_lock);
  if (!npfs_dirh)
    {
      InitializeObjectAttributes (&attr, &ro_u_npfs, 0, NULL, NULL);
      status = NtOpenFile (&npfs_dirh, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
			   &attr, &io, FILE_SHARE_READ | FILE_SHARE_WRITE,
			   0);
    }
  ReleaseSRWLockExclusive (&npfs_lock);
  if (NT_SUCCESS (status))
    nph = npfs_dirh;
  return status;
}

/* Called when a FIFO is first opened for reading and again each time
   a new client handler is needed.  Each pipe instance is created in
   blocking mode so that we can easily wait for a connection.  After
   it is connected, it is put in nonblocking mode. */
HANDLE
fhandler_fifo::create_pipe_instance (bool first)
{
  NTSTATUS status;
  HANDLE npfsh;
  HANDLE ph = NULL;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG hattr;
  ULONG sharing;
  ULONG nonblocking = FILE_PIPE_QUEUE_OPERATION;
  ULONG max_instances = -1;
  LARGE_INTEGER timeout;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return NULL;
    }
  access = GENERIC_READ | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES
    | SYNCHRONIZE;
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  hattr = openflags & O_CLOEXEC ? 0 : OBJ_INHERIT;
  if (first)
    hattr |= OBJ_CASE_INSENSITIVE;
  InitializeObjectAttributes (&attr, get_pipe_name (),
			      hattr, npfsh, NULL);
  timeout.QuadPart = -500000;
  status = NtCreateNamedPipeFile (&ph, access, &attr, &io, sharing,
				  first ? FILE_CREATE : FILE_OPEN, 0,
				  FILE_PIPE_MESSAGE_TYPE
				    | FILE_PIPE_REJECT_REMOTE_CLIENTS,
				  FILE_PIPE_MESSAGE_MODE,
				  nonblocking, max_instances,
				  DEFAULT_PIPEBUFSIZE, DEFAULT_PIPEBUFSIZE,
				  &timeout);
  if (!NT_SUCCESS (status))
    __seterrno_from_nt_status (status);
  return ph;
}

/* Connect to a pipe instance. */
NTSTATUS
fhandler_fifo::open_pipe (HANDLE& ph)
{
  NTSTATUS status;
  HANDLE npfsh;
  ACCESS_MASK access;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  ULONG sharing;

  status = npfs_handle (npfsh);
  if (!NT_SUCCESS (status))
    return status;
  access = GENERIC_WRITE | SYNCHRONIZE;
  InitializeObjectAttributes (&attr, get_pipe_name (),
			      openflags & O_CLOEXEC ? 0 : OBJ_INHERIT,
			      npfsh, NULL);
  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE;
  status = NtOpenFile (&ph, access, &attr, &io, sharing, 0);
  return status;
}

int
fhandler_fifo::add_client_handler ()
{
  int ret = -1;
  fifo_client_handler fc;
  HANDLE ph = NULL;
  bool first = (nhandlers == 0);

  if (nhandlers == MAX_CLIENTS)
    {
      set_errno (EMFILE);
      goto out;
    }
  ph = create_pipe_instance (first);
  if (!ph)
    goto out;
  else
    {
      ret = 0;
      fc.h = ph;
      fc_handler[nhandlers++] = fc;
    }
out:
  return ret;
}

void
fhandler_fifo::delete_client_handler (int i)
{
  fc_handler[i].close ();
  if (i < --nhandlers)
    memmove (fc_handler + i, fc_handler + i + 1,
	     (nhandlers - i) * sizeof (fc_handler[i]));
}

void
fhandler_fifo::record_connection (fifo_client_handler& fc)
{
  SetEvent (write_ready);
  fc.state = fc_connected;
  nconnected++;
  set_pipe_non_blocking (fc.h, true);
}

static DWORD WINAPI
fifo_reader_thread (LPVOID param)
{
  fhandler_fifo *fh = (fhandler_fifo *) param;
  return fh->thread_func ();
}

DWORD
fhandler_fifo::thread_func ()
{
  HANDLE conn_evt;

  if (!(conn_evt = create_event ()))
    goto canceled;

  while (1)
    {
      fifo_reader_id_t cur_owner;

      owner_lock ();
      cur_owner = get_owner ();
      /* Has the owner been set yet? */
      if (!cur_owner)
	{
	  set_owner (me);
	  owner_unlock ();
	  continue;
	}
      /* If there's an owner but it's not me, wait until there's
	 something for me to do.  For now, I'll just wait until I'm
	 canceled. */
      else if (cur_owner != me)
	{
	  owner_unlock ();
	  WaitForSingleObject (cancel_evt, INFINITE);
	  goto canceled;
	}
      /* I'm the owner.  Listen for client connections. */
      else
	{
	  owner_unlock ();

	  /* All client handlers are in the fc_connected or fc_invalid
	     state.  Delete any invalid clients. */
	  fifo_client_lock ();
	  int i = 0;
	  while (i < nhandlers)
	    {
	      if (fc_handler[i].state == fc_invalid)
		delete_client_handler (i);
	      else
		i++;
	    }

	  /* Create a new client handler. */
	  if (add_client_handler () < 0)
	    {
	      fifo_client_unlock ();
	      goto canceled;
	    }

	  /* Listen for a writer to connect to the new client handler. */
	  fifo_client_handler& fc = fc_handler[nhandlers - 1];
	  fifo_client_unlock ();
	  SetEvent (listening_evt);
	  NTSTATUS status;
	  IO_STATUS_BLOCK io;

	  status = NtFsControlFile (fc.h, conn_evt, NULL, NULL, &io,
				    FSCTL_PIPE_LISTEN, NULL, 0, NULL, 0);
	  if (status == STATUS_PENDING)
	    {
	      HANDLE w[2] = { conn_evt, cancel_evt };
	      switch (WaitForMultipleObjects (2, w, false, INFINITE))
		{
		case WAIT_OBJECT_0:
		  status = io.Status;
		  break;
		case WAIT_OBJECT_0 + 1:
		  status = STATUS_THREAD_IS_TERMINATING;
		  break;
		default:
		  debug_printf ("WFMO failed, %E");
		  delete_client_handler (nhandlers - 1);
		  continue;	/* ?? */
		}
	    }
	  HANDLE ph = NULL;
	  int ps = -1;
	  bool cancel = false;

	  fifo_client_lock ();
	  switch (status)
	    {
	    case STATUS_SUCCESS:
	    case STATUS_PIPE_CONNECTED:
	      record_connection (fc);
	      ResetEvent (conn_evt);
	      break;
	    case STATUS_THREAD_IS_TERMINATING:
	      cancel = true;
	      /* Force NtFsControlFile to complete.  Otherwise the next
		 writer to connect might not be recorded in the client
		 handler list. */
	      status = open_pipe (ph);
	      if (NT_SUCCESS (status)
		  && (NT_SUCCESS (io.Status) || io.Status == STATUS_PIPE_CONNECTED))
		{
		  debug_printf ("successfully connected bogus client");
		  delete_client_handler (nhandlers - 1);
		}
	      else if ((ps = fc.pipe_state ()) == FILE_PIPE_CONNECTED_STATE
		       || ps == FILE_PIPE_INPUT_AVAILABLE_STATE)
		{
		  /* A connection was made under our nose. */
		  debug_printf ("recording connection before terminating");
		  record_connection (fc);
		}
	      else
		{
		  debug_printf ("failed to terminate NtFsControlFile cleanly");
		  delete_client_handler (nhandlers - 1);
		}
	      if (ph)
		NtClose (ph);
	      break;
	    default:
	      debug_printf ("NtFsControlFile status %y", status);
	      delete_client_handler (nhandlers - 1);
	      break;		/* ?? */
	    }
	  fifo_client_unlock ();
	  ResetEvent (listening_evt);
	  if (cancel)
	    goto canceled;
	}
    }
canceled:
  if (conn_evt)
    NtClose (conn_evt);
  /* automatically return the cygthread to the cygthread pool */
  _my_tls._ctinfo->auto_release ();
  return 0;
}

int
fhandler_fifo::create_shmem ()
{
  HANDLE sect;
  OBJECT_ATTRIBUTES attr;
  NTSTATUS status;
  LARGE_INTEGER size = { .QuadPart = sizeof (fifo_shmem_t) };
  SIZE_T viewsize = sizeof (fifo_shmem_t);
  PVOID addr = NULL;
  UNICODE_STRING uname;
  WCHAR shmem_name[MAX_PATH];

  __small_swprintf (shmem_name, L"fifo-shmem.%08x.%016X", get_dev (),
		    get_ino ());
  RtlInitUnicodeString (&uname, shmem_name);
  InitializeObjectAttributes (&attr, &uname, OBJ_INHERIT,
			      get_shared_parent_dir (), NULL);
  status = NtCreateSection (&sect, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY
			    | SECTION_MAP_READ | SECTION_MAP_WRITE,
			    &attr, &size, PAGE_READWRITE, SEC_COMMIT, NULL);
  if (status == STATUS_OBJECT_NAME_COLLISION)
    status = NtOpenSection (&sect, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY
			    | SECTION_MAP_READ | SECTION_MAP_WRITE, &attr);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  status = NtMapViewOfSection (sect, NtCurrentProcess (), &addr, 0, viewsize,
			       NULL, &viewsize, ViewShare, 0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      NtClose (sect);
      __seterrno_from_nt_status (status);
      return -1;
    }
  shmem_handle = sect;
  shmem = (fifo_shmem_t *) addr;
  return 0;
}

/* shmem_handle must be valid when this is called. */
int
fhandler_fifo::reopen_shmem ()
{
  NTSTATUS status;
  SIZE_T viewsize = sizeof (fifo_shmem_t);
  PVOID addr = NULL;

  status = NtMapViewOfSection (shmem_handle, NtCurrentProcess (), &addr, 0,
			       sizeof (fifo_shmem_t), NULL, &viewsize,
			       ViewShare, 0, PAGE_READWRITE);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      return -1;
    }
  shmem = (fifo_shmem_t *) addr;
  return 0;
}

int
fhandler_fifo::open (int flags, mode_t)
{
  if (flags & O_PATH)
    return open_fs (flags);

  /* Determine what we're doing with this fhandler: reading, writing, both */
  switch (flags & O_ACCMODE)
    {
    case O_RDONLY:
      reader = true;
      break;
    case O_WRONLY:
      writer = true;
      break;
    case O_RDWR:
      reader = true;
      duplexer = true;
      break;
    default:
      set_errno (EINVAL);
      goto err;
    }

  debug_only_printf ("reader %d, writer %d, duplexer %d", reader, writer, duplexer);
  set_flags (flags);
  if (reader && !duplexer)
    nohandle (true);

  /* Create control events for this named pipe */
  char char_sa_buf[1024];
  LPSECURITY_ATTRIBUTES sa_buf;
  sa_buf = sec_user_cloexec (flags & O_CLOEXEC, (PSECURITY_ATTRIBUTES) char_sa_buf,
		      cygheap->user.sid());

  char npbuf[MAX_PATH];
  __small_sprintf (npbuf, "r-event.%08x.%016X", get_dev (), get_ino ());
  if (!(read_ready = CreateEvent (sa_buf, true, false, npbuf)))
    {
      debug_printf ("CreateEvent for %s failed, %E", npbuf);
      __seterrno ();
      goto err;
    }
  npbuf[0] = 'w';
  if (!(write_ready = CreateEvent (sa_buf, true, false, npbuf)))
    {
      debug_printf ("CreateEvent for %s failed, %E", npbuf);
      __seterrno ();
      goto err_close_read_ready;
    }

  /* If we're reading, signal read_ready, create the shared memory,
     and start the fifo_reader_thread. */
  if (reader)
    {
      if (!arm (read_ready))
	{
	  __seterrno ();
	  goto err_close_write_ready;
	}
      if (create_shmem () < 0)
	goto err_close_write_ready;
      if (!(listening_evt = create_event ()))
	goto err_close_shmem;
      if (!(cancel_evt = create_event ()))
	goto err_close_listening_evt;
      if (!(sync_thr = create_event ()))
	goto err_close_cancel_evt;
      me.winpid = GetCurrentProcessId ();
      me.fh = this;
      new cygthread (fifo_reader_thread, this, "fifo_reader", sync_thr);
      reader_lock ();
      if (get_nreaders () > 0)
	{
	  set_errno (ENOTSUP);
	  reader_unlock ();
	  goto err_cancel_frt;
	}
      reader_unlock ();

      /* If we're a duplexer, we need a handle for writing. */
      if (duplexer)
	{
	  HANDLE ph = NULL;

	  /* Wait until the frt is listening. */
	  WaitForSingleObject (listening_evt, INFINITE);
	  NTSTATUS status = open_pipe (ph);
	  if (NT_SUCCESS (status))
	    {
	      set_handle (ph);
	      set_pipe_non_blocking (ph, flags & O_NONBLOCK);
	    }
	  else
	    {
	      __seterrno_from_nt_status (status);
	      goto err_cancel_frt;
	    }
	}

      /* Not a duplexer; wait for a writer to connect. */
      else if (!wait (write_ready))
	goto err_cancel_frt;

      reader_lock ();
      inc_nreaders ();
      reader_unlock ();
      goto success;
    }

  /* If we're writing, wait for read_ready and then connect to the
     pipe.  This should always succeed quickly if the reader's
     fifo_reader thread is running.  Then signal write_ready.  */
  if (writer)
    {
      while (1)
	{
	  if (wait (read_ready))
	    WaitForSingleObject (listening_evt, INFINITE);
	  else
	    goto err_close_write_ready;

	  NTSTATUS status = open_pipe (get_handle ());
	  if (NT_SUCCESS (status))
	    {
	      set_pipe_non_blocking (get_handle (), flags & O_NONBLOCK);
	      if (!arm (write_ready))
		{
		  __seterrno ();
		  goto err_close_write_ready;
		}
	      else
		goto success;
	    }
	  else if (STATUS_PIPE_NO_INSTANCE_AVAILABLE (status))
	    Sleep (1);
	  else
	    {
	      debug_printf ("create of writer failed");
	      __seterrno_from_nt_status (status);
	      goto err_close_write_ready;
	    }
	}
    }
success:
  return 1;
err_cancel_frt:
  cancel_reader_thread ();
  reader_lock ();
  if (get_nreaders () == 0)
    ResetEvent (read_ready);
  reader_unlock ();
  NtClose (sync_thr);
err_close_cancel_evt:
  NtClose (cancel_evt);
err_close_listening_evt:
  NtClose (listening_evt);
err_close_shmem:
  NtUnmapViewOfSection (NtCurrentProcess (), shmem);
  NtClose (shmem_handle);
err_close_write_ready:
  NtClose (write_ready);
err_close_read_ready:
  NtClose (read_ready);
err:
  if (get_handle ())
    NtClose (get_handle ());
  return 0;
}

off_t
fhandler_fifo::lseek (off_t offset, int whence)
{
  debug_printf ("(%D, %d)", offset, whence);
  set_errno (ESPIPE);
  return -1;
}

bool
fhandler_fifo::wait (HANDLE h)
{
#ifdef DEBUGGING
  const char *what;
  if (h == read_ready)
    what = "reader";
  else
    what = "writer";
#endif
  /* Set the wait to zero for non-blocking I/O-related events. */
  DWORD wait = ((h == read_ready || h == write_ready)
		&& get_flags () & O_NONBLOCK) ? 0 : INFINITE;

  debug_only_printf ("waiting for %s", what);
  /* Wait for the event.  Set errno, as appropriate if something goes wrong. */
  switch (cygwait (h, wait))
    {
    case WAIT_OBJECT_0:
      debug_only_printf ("successfully waited for %s", what);
      return true;
    case WAIT_SIGNALED:
      debug_only_printf ("interrupted by signal while waiting for %s", what);
      set_errno (EINTR);
      return false;
    case WAIT_CANCELED:
      debug_only_printf ("cancellable interruption while waiting for %s", what);
      pthread::static_cancel_self ();	/* never returns */
      break;
    case WAIT_TIMEOUT:
      if (h == write_ready)
	{
	  debug_only_printf ("wait timed out waiting for write but will still open reader since non-blocking mode");
	  return true;
	}
      else
	{
	  set_errno (ENXIO);
	  return false;
	}
      break;
    default:
      debug_only_printf ("unknown error while waiting for %s", what);
      __seterrno ();
      return false;
   }
}

ssize_t __reg3
fhandler_fifo::raw_write (const void *ptr, size_t len)
{
  ssize_t ret = -1;
  size_t nbytes = 0;
  ULONG chunk;
  NTSTATUS status = STATUS_SUCCESS;
  IO_STATUS_BLOCK io;
  HANDLE evt = NULL;

  if (!len)
    return 0;

  if (len <= max_atomic_write)
    chunk = len;
  else if (is_nonblocking ())
    chunk = len = max_atomic_write;
  else
    chunk = max_atomic_write;

  /* Create a wait event if the FIFO is in blocking mode. */
  if (!is_nonblocking () && !(evt = CreateEvent (NULL, false, false, NULL)))
    {
      __seterrno ();
      return -1;
    }

  /* Write in chunks, accumulating a total.  If there's an error, just
     return the accumulated total unless the first write fails, in
     which case return -1. */
  while (nbytes < len)
    {
      ULONG_PTR nbytes_now = 0;
      size_t left = len - nbytes;
      ULONG len1;
      DWORD waitret = WAIT_OBJECT_0;

      if (left > chunk)
	len1 = chunk;
      else
	len1 = (ULONG) left;
      nbytes_now = 0;
      status = NtWriteFile (get_handle (), evt, NULL, NULL, &io,
			    (PVOID) ptr, len1, NULL, NULL);
      if (evt && status == STATUS_PENDING)
	{
	  waitret = cygwait (evt);
	  if (waitret == WAIT_OBJECT_0)
	    status = io.Status;
	}
      if (waitret == WAIT_CANCELED)
	status = STATUS_THREAD_CANCELED;
      else if (waitret == WAIT_SIGNALED)
	status = STATUS_THREAD_SIGNALED;
      else if (isclosed ())  /* A signal handler might have closed the fd. */
	{
	  if (waitret == WAIT_OBJECT_0)
	    set_errno (EBADF);
	  else
	    __seterrno ();
	}
      else if (NT_SUCCESS (status))
	{
	  nbytes_now = io.Information;
	  /* NtWriteFile returns success with # of bytes written == 0
	     if writing on a non-blocking pipe fails because the pipe
	     buffer doesn't have sufficient space. */
	  if (nbytes_now == 0)
	    set_errno (EAGAIN);
	  ptr = ((char *) ptr) + chunk;
	  nbytes += nbytes_now;
	}
      else if (STATUS_PIPE_IS_CLOSED (status))
	{
	  set_errno (EPIPE);
	  raise (SIGPIPE);
	}
      else
	__seterrno_from_nt_status (status);
      if (nbytes_now == 0)
	len = 0;		/* Terminate loop. */
      if (nbytes > 0)
	ret = nbytes;
    }
  if (evt)
    NtClose (evt);
  if (status == STATUS_THREAD_SIGNALED && ret < 0)
    set_errno (EINTR);
  else if (status == STATUS_THREAD_CANCELED)
    pthread::static_cancel_self ();
  return ret;
}

/* A FIFO open for reading is at EOF if no process has it open for
   writing.  We test this by checking nconnected.  But we must take
   account of the possible delay from the time of connection to the
   time the connection is recorded by the fifo_reader thread. */
bool
fhandler_fifo::hit_eof ()
{
  bool eof;
  bool retry = true;

repeat:
      fifo_client_lock ();
      eof = (nconnected == 0);
      fifo_client_unlock ();
      if (eof && retry)
	{
	  retry = false;
	  /* Give the fifo_reader thread time to catch up. */
	  Sleep (1);
	  goto repeat;
	}
  return eof;
}

void __reg3
fhandler_fifo::raw_read (void *in_ptr, size_t& len)
{
  if (!len)
    return;

  owner_lock ();
  if (get_owner () != me)
    {
      owner_unlock ();
      set_errno (ENOTSUP);
      goto errout;
    }
  owner_unlock ();

  while (1)
    {
      if (hit_eof ())
	{
	  len = 0;
	  return;
	}

      /* Poll the connected clients for input. */
      fifo_client_lock ();
      for (int i = 0; i < nhandlers; i++)
	if (fc_handler[i].state == fc_connected)
	  {
	    NTSTATUS status;
	    IO_STATUS_BLOCK io;
	    size_t nbytes = 0;

	    status = NtReadFile (get_fc_handle (i), NULL, NULL, NULL,
				 &io, in_ptr, len, NULL, NULL);
	    switch (status)
	      {
	      case STATUS_SUCCESS:
	      case STATUS_BUFFER_OVERFLOW:
		/* io.Information is supposedly valid. */
		nbytes = io.Information;
		if (nbytes > 0)
		  {
		    len = nbytes;
		    fifo_client_unlock ();
		    return;
		  }
		break;
	      case STATUS_PIPE_EMPTY:
		break;
	      case STATUS_PIPE_BROKEN:
		/* Client has disconnected.  Mark the client handler
		   to be deleted when it's safe to do that. */
		fc_handler[i].state = fc_invalid;
		nconnected--;
		break;
	      default:
		debug_printf ("NtReadFile status %y", status);
		__seterrno_from_nt_status (status);
		fc_handler[i].state = fc_invalid;
		nconnected--;
		fifo_client_unlock ();
		goto errout;
	      }
	  }
      fifo_client_unlock ();
      if (is_nonblocking ())
	{
	  set_errno (EAGAIN);
	  goto errout;
	}
      else
	{
	  /* Allow interruption. */
	  DWORD waitret = cygwait (NULL, cw_nowait, cw_cancel | cw_sig_eintr);
	  if (waitret == WAIT_CANCELED)
	    pthread::static_cancel_self ();
	  else if (waitret == WAIT_SIGNALED)
	    {
	      if (_my_tls.call_signal_handler ())
		continue;
	      else
		{
		  set_errno (EINTR);
		  goto errout;
		}
	    }
	}
      /* We might have been closed by a signal handler or another thread. */
      if (isclosed ())
	{
	  set_errno (EBADF);
	  goto errout;
	}
      /* Don't hog the CPU. */
      Sleep (1);
    }
errout:
  len = (size_t) -1;
}

int __reg2
fhandler_fifo::fstatvfs (struct statvfs *sfs)
{
  if (get_flags () & O_PATH)
    /* We already have a handle. */
    {
      HANDLE h = get_handle ();
      if (h)
	return fstatvfs_by_handle (h, sfs);
    }

  fhandler_disk_file fh (pc);
  fh.get_device () = FH_FS;
  return fh.fstatvfs (sfs);
}

void
fifo_client_handler::close ()
{
  if (!NtClose (h))
    debug_printf ("closing handle %p failed, %E", h);
}

int
fifo_client_handler::pipe_state ()
{
  IO_STATUS_BLOCK io;
  FILE_PIPE_LOCAL_INFORMATION fpli;
  NTSTATUS status;

  status = NtQueryInformationFile (h, &io, &fpli,
				   sizeof (fpli), FilePipeLocalInformation);
  if (!NT_SUCCESS (status))
    {
      debug_printf ("NtQueryInformationFile status %y", status);
      __seterrno_from_nt_status (status);
      return -1;
    }
  else if (fpli.ReadDataAvailable > 0)
    return FILE_PIPE_INPUT_AVAILABLE_STATE;
  else
    return fpli.NamedPipeState;
}

void
fhandler_fifo::cancel_reader_thread ()
{
  if (cancel_evt)
    SetEvent (cancel_evt);
  if (sync_thr)
    {
      WaitForSingleObject (sync_thr, INFINITE);
      NtClose (sync_thr);
    }
}

int
fhandler_fifo::close ()
{
  if (reader)
    {
      cancel_reader_thread ();
      if (cancel_evt)
	NtClose (cancel_evt);
      fifo_client_lock ();
      for (int i = 0; i < nhandlers; i++)
	fc_handler[i].close ();
      fifo_client_unlock ();
      reader_lock ();
      if (read_ready && dec_nreaders () == 0)
	ResetEvent (read_ready);
      reader_unlock ();
      owner_lock ();
      if (get_owner () == me)
	set_owner (null_fr_id);
      owner_unlock ();
      if (shmem)
	NtUnmapViewOfSection (NtCurrentProcess (), shmem);
      if (shmem_handle)
	NtClose (shmem_handle);
    }
  if (read_ready)
      NtClose (read_ready);
  if (write_ready)
    NtClose (write_ready);
  return fhandler_base::close ();
}

/* If we have a write handle (i.e., we're a duplexer or a writer),
   keep the nonblocking state of the windows pipe in sync with our
   nonblocking state. */
int
fhandler_fifo::fcntl (int cmd, intptr_t arg)
{
  if (cmd != F_SETFL || nohandle () || (get_flags () & O_PATH))
    return fhandler_base::fcntl (cmd, arg);

  const bool was_nonblocking = is_nonblocking ();
  int res = fhandler_base::fcntl (cmd, arg);
  const bool now_nonblocking = is_nonblocking ();
  if (now_nonblocking != was_nonblocking)
    set_pipe_non_blocking (get_handle (), now_nonblocking);
  return res;
}

int
fhandler_fifo::dup (fhandler_base *child, int flags)
{
  int i = 0;
  fhandler_fifo *fhf = NULL;

  if (get_flags () & O_PATH)
    return fhandler_base::dup (child, flags);

  if (fhandler_base::dup (child, flags))
    goto err;

  fhf = (fhandler_fifo *) child;
  if (!DuplicateHandle (GetCurrentProcess (), read_ready,
			GetCurrentProcess (), &fhf->read_ready,
			0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
    {
      __seterrno ();
      goto err;
    }
  if (!DuplicateHandle (GetCurrentProcess (), write_ready,
			GetCurrentProcess (), &fhf->write_ready,
			0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
    {
      __seterrno ();
      goto err_close_read_ready;
    }
  if (reader)
    {
      if (!DuplicateHandle (GetCurrentProcess (), shmem_handle,
			    GetCurrentProcess (), &fhf->shmem_handle,
			    0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
	{
	  __seterrno ();
	  goto err_close_write_ready;
	}
      if (fhf->reopen_shmem () < 0)
	goto err_close_shmem_handle;
      if (!(fhf->listening_evt = create_event ()))
	{
	  __seterrno ();
	  goto err_close_shmem;
	}
      if (!(fhf->cancel_evt = create_event ()))
	{
	  __seterrno ();
	  goto err_close_listening_evt;
	}
      if (!(fhf->sync_thr = create_event ()))
	{
	  __seterrno ();
	  goto err_close_cancel_evt;
	}
      fifo_client_lock ();
      for (i = 0; i < nhandlers; i++)
	{
	  if (!DuplicateHandle (GetCurrentProcess (), fc_handler[i].h,
				GetCurrentProcess (),
				&fhf->fc_handler[i].h,
				0, !(flags & O_CLOEXEC), DUPLICATE_SAME_ACCESS))
	    {
	      __seterrno ();
	      break;
	    }
	}
      if (i < nhandlers)
	{
	  fifo_client_unlock ();
	  goto err_close_handlers;
	}
      fifo_client_unlock ();
      reader_lock ();
      inc_nreaders ();
      reader_unlock ();
      fhf->me.fh = fhf;
      new cygthread (fifo_reader_thread, fhf, "fifo_reader", fhf->sync_thr);
    }
  return 0;
err_close_handlers:
  NtClose (fhf->sync_thr);
  for (int j = 0; j < i; j++)
    fhf->fc_handler[j].close ();
err_close_cancel_evt:
  NtClose (fhf->cancel_evt);
err_close_listening_evt:
  NtClose (fhf->listening_evt);
err_close_shmem:
  NtUnmapViewOfSection (NtCurrentProcess (), fhf->shmem);
err_close_shmem_handle:
  NtClose (fhf->shmem_handle);
err_close_write_ready:
  NtClose (fhf->write_ready);
err_close_read_ready:
  NtClose (fhf->read_ready);
err:
  return -1;
}

void
fhandler_fifo::fixup_after_fork (HANDLE parent)
{
  fhandler_base::fixup_after_fork (parent);
  fork_fixup (parent, read_ready, "read_ready");
  fork_fixup (parent, write_ready, "write_ready");
  if (reader)
    {
      fork_fixup (parent, shmem_handle, "shmem_handle");
      /* The child needs its own view of shared memory. */
      if (reopen_shmem () < 0)
	api_fatal ("Can't reopen shared memory during fork, %E");
      fifo_client_lock ();
      for (int i = 0; i < nhandlers; i++)
	fork_fixup (parent, fc_handler[i].h, "fc_handler[].h");
      fifo_client_unlock ();
      if (!(cancel_evt = create_event ()))
	api_fatal ("Can't create reader thread cancel event during fork, %E");
      if (!(sync_thr = create_event ()))
	api_fatal ("Can't create reader thread sync event during fork, %E");
      reader_lock ();
      inc_nreaders ();
      reader_unlock ();
      me.winpid = GetCurrentProcessId ();
      new cygthread (fifo_reader_thread, this, "fifo_reader", sync_thr);
    }
}

void
fhandler_fifo::fixup_after_exec ()
{
  fhandler_base::fixup_after_exec ();
  if (reader && !close_on_exec ())
    {
      /* The child needs its own view of shared memory. */
      if (reopen_shmem () < 0)
	api_fatal ("Can't reopen shared memory during exec, %E");
      me.winpid = GetCurrentProcessId ();
      if (!(cancel_evt = create_event ()))
	api_fatal ("Can't create reader thread cancel event during exec, %E");
      if (!(sync_thr = create_event ()))
	api_fatal ("Can't create reader thread sync event during exec, %E");
      new cygthread (fifo_reader_thread, this, "fifo_reader", sync_thr);
    }
}

void
fhandler_fifo::set_close_on_exec (bool val)
{
  fhandler_base::set_close_on_exec (val);
  set_no_inheritance (read_ready, val);
  set_no_inheritance (write_ready, val);
  fifo_client_lock ();
  for (int i = 0; i < nhandlers; i++)
    set_no_inheritance (fc_handler[i].h, val);
  fifo_client_unlock ();
}
