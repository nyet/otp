/*
 * %CopyrightBegin%
 * 
 * Copyright Ericsson AB 1996-2011. All Rights Reserved.
 * 
 * The contents of this file are subject to the Erlang Public License,
 * Version 1.1, (the "License"); you may not use this file except in
 * compliance with the License. You should have received a copy of the
 * Erlang Public License along with this software. If not, it can be
 * retrieved online at http://www.erlang.org/.
 * 
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 * 
 * %CopyrightEnd%
 */
/**
 *
 *  File:     heart.c
 *  Purpose:  Portprogram for supervision of the Erlang emulator.
 *
 *  Synopsis: heart
 *
 *  SPAWNING FROM ERLANG
 *
 *  This program is started from Erlang as follows,
 *
 *      Port = open_port({spawn, 'heart'}, [{packet, 2}]),
 *
 *  ROLE OF THIS PORT PROGRAM
 *
 *  This program is started by the Erlang emulator. It  communicates
 *  with the emulator through file descriptor 0 (standard input).
 *
 *  MESSAGE FORMAT
 *
 *  All messages have the following format (a value in parentheses
 *  indicate field length in bytes),
 *
 *      {Length(2), Operation(1)}
 *
 *  START ACK
 *
 *  When this program has started it sends an START ACK message to Erlang.
 *
 *  HEART_BEATING
 *
 *  This program expects a heart beat messages. If it does not receive a 
 *  heart beat message from Erlang within heart_beat_timeout seconds, it 
 *  reboots the system. The variable heart_beat_timeout is exported (so
 *  that it can be set from the shell in VxWorks, as is the variable
 *  heart_beat_report_delay). When using Solaris, the system is rebooted
 *  by executing the command stored in the environment variable
 *  HEART_COMMAND.
 *
 *  BLOCKING DESCRIPTORS
 *
 *  All file descriptors in this program are blocking. This can lead
 *  to deadlocks. The emulator reads and writes are blocking.
 *
 *  STANDARD INPUT, OUTPUT AND ERROR
 *
 *  This program communicates with Erlang through the standard
 *  input and output file descriptors (0 and 1). These descriptors
 *  (and the standard error descriptor 2) must NOT be closed
 *  explicitely by this program at termination (in UNIX it is
 *  taken care of by the operating system itself; in VxWorks
 *  it is taken care of by the spawn driver part of the Emulator).
 *
 *  END OF FILE
 *
 *  If a read from a file descriptor returns zero (0), it means
 *  that there is no process at the other end of the connection
 *  having the connection open for writing (end-of-file).
 *
 *  HARDWARE WATCHDOG
 *
 *  When used with VxWorks(with CPU40), the hardware
 *  watchdog is enabled, making sure that the system reboots
 *  even if the heart port program malfunctions or the system
 *  is completely overloaded.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#ifdef __WIN32__
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <process.h>
#endif
#ifdef VXWORKS
#include "sys.h"
#endif

/*
 * Implement time correction using times() call even on Linuxes 
 * that can simulate gethrtime with clock_gettime, no use implementing
 * a phony gethrtime in this file as the time questions are so infrequent.
 */
#if defined(CORRET_USING_TIMES) || defined(GETHRTIME_WITH_CLOCK_GETTIME)
#  define HEART_CORRECT_USING_TIMES 1
#endif

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

#include <stdarg.h>

#include <string.h>
#include <time.h>
#include <errno.h>

#ifdef VXWORKS
#  include <vxWorks.h>
#  include <ioLib.h>
#  include <selectLib.h>
#  include <netinet/in.h>
#  include <rebootLib.h>
#  include <sysLib.h> 
#  include <taskLib.h>
#  include <wdLib.h>
#  include <taskHookLib.h>
#  include <selectLib.h>
#endif
#if !defined(__WIN32__) && !defined(VXWORKS)
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <sys/time.h>
#  include <unistd.h>
#  include <signal.h>
#  if defined(HEART_CORRECT_USING_TIMES)
#    include <sys/times.h>
#    include <limits.h>
#  endif
#endif

#define HEART_COMMAND_ENV    "HEART_COMMAND"

#define MSG_HDR_SIZE        2
#define MSG_HDR_PLUS_OP_SIZE 3
#define MSG_BODY_SIZE      2048
#define MSG_TOTAL_SIZE     2050

unsigned char cmd[MSG_BODY_SIZE];

struct msg {
  unsigned short len;
  unsigned char op;
  unsigned char fill[MSG_BODY_SIZE]; /* one too many */
};

/* operations */
#define  HEART_ACK    1
#define  HEART_BEAT   2
#define  SHUT_DOWN    3
#define  SET_CMD      4
#define  CLEAR_CMD    5
#define  GET_CMD      6
#define  HEART_CMD    7


/*  Maybe interesting to change */

/* Times in seconds */
#define  HEART_BEAT_BOOT_DELAY       60  /* 1 minute */
#define  SELECT_TIMEOUT               5  /* Every 5 seconds we reset the
					    watchdog timer */

/* heart_beat_timeout is the maximum gap in seconds between two
   consecutive heart beat messages from Erlang, and HEART_BEAT_BOOT_DELAY
   is the the extra delay that wd_keeper allows for, to give heart a
   chance to reboot in the "normal" way before the hardware watchdog
   enters the scene. heart_beat_report_delay is the time allowed for reporting
   before rebooting under VxWorks. */

int heart_beat_timeout = 60;
int heart_beat_report_delay = 30;
int heart_beat_boot_delay = HEART_BEAT_BOOT_DELAY;
/* All current platforms have a process identifier that
   fits in an unsigned long and where 0 is an impossible or invalid value */
unsigned long heart_beat_kill_pid = 0;

#define VW_WD_TIMEOUT (heart_beat_timeout+heart_beat_report_delay+heart_beat_boot_delay)
#define SOL_WD_TIMEOUT (heart_beat_timeout+heart_beat_boot_delay)

/* reasons for reboot */
#define  R_TIMEOUT          1
#define  R_CLOSED           2
#define  R_ERROR            3
#define  R_SHUT_DOWN        4


/*  macros */

#define  NULLFDS  ((fd_set *) NULL)
#define  NULLTV   ((struct timeval *) NULL)

/*  prototypes */

static int message_loop(int,int);
static void do_terminate(int);
static int notify_ack(int);
static int heart_cmd_reply(int, char *);
static int write_message(int, struct msg *);
static int read_message(int, struct msg *);
static int read_skip(int, char *, int, int);
static int read_fill(int, char *, int);
static void print_error(const char *,...);
static void debugf(const char *,...);
static void init_timestamp(void);
static time_t timestamp(time_t *);

#ifdef __WIN32__
static BOOL enable_privilege(void);
static BOOL do_shutdown(int);
static void print_last_error(void);
static HANDLE start_reader_thread(void);
static DWORD WINAPI reader(LPVOID);
static int test_win95(void);
#define read _read
#define write _write
#endif

/*  static variables */

static char program_name[256];
static int erlin_fd = 0, erlout_fd = 1; /* std in and out */
static int debug_on = 0;
#ifdef __WIN32__
static HANDLE hreader_thread;
static HANDLE hevent_dataready;
static struct msg m, *mp = &m;
static int   tlen;			/* total message length */
static FILE* conh;

#endif

static int
is_env_set(char *key)
{
#ifdef __WIN32__
    char buf[1];
    DWORD sz = (DWORD) sizeof(buf);
    SetLastError(0);
    sz = GetEnvironmentVariable((LPCTSTR) key, (LPTSTR) buf, sz);
    return sz || GetLastError() != ERROR_ENVVAR_NOT_FOUND; 
#else
    return getenv(key) != NULL;
#endif
}

static char *
get_env(char *key)
{
#ifdef __WIN32__
    DWORD size = 32;
    char *value = NULL;
    while (1) {
	DWORD nsz;
	if (value)
	    free(value);
	value = malloc(size);
	if (!value) {
	    print_error("Failed to allocate memory. Terminating...");
	    exit(1);
	}
	SetLastError(0);
	nsz = GetEnvironmentVariable((LPCTSTR) key, (LPTSTR) value, size);
	if (nsz == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
	    free(value);
	    return NULL;
	}
	if (nsz <= size)
	    return value;
	size = nsz;
    }
#else
    return getenv(key);
#endif
}

static void
free_env_val(char *value)
{
#ifdef __WIN32__
    if (value)
	free(value);
#endif
}

/*
 *  main
 */
static void get_arguments(int argc, char** argv) {
    int i = 1;
    int h;
    int w;
    unsigned long p;

    while (i < argc) {
	switch (argv[i][0]) {
	case '-':
	    switch (argv[i][1]) {
	    case 'h':
		if (strcmp(argv[i], "-ht") == 0) 
		    if (sscanf(argv[i+1],"%i",&h) ==1)
			if ((h > 10) && (h <= 65535)) {
			    heart_beat_timeout = h;
			    fprintf(stderr,"heart_beat_timeout = %d\n",h);
			    i++;
			}
		break;
	    case 'w':
		if (strcmp(argv[i], "-wt") == 0)
		    if (sscanf(argv[i+1],"%i",&w) ==1)
			if ((w > 10) && (w <= 65535)) {
			    heart_beat_boot_delay = w;
			    fprintf(stderr,"heart_beat_boot_delay = %d\n",w);
			    i++;
			}
		break;
	    case 'p':
		if (strcmp(argv[i], "-pid") == 0)
		    if (sscanf(argv[i+1],"%lu",&p) ==1){
			heart_beat_kill_pid = p;
			fprintf(stderr,"heart_beat_kill_pid = %lu\n",p);
			i++;
		    }
		break;
#ifdef __WIN32__
	    case 's':
		if (strcmp(argv[i], "-shutdown") == 0){
		    do_shutdown(1);
		    exit(0);
		}
		break;
#endif
	    default:
		;
	    }
	    break;
	default:
	    ;
	}
	i++;
    }
    debugf("arguments -ht %d -wt %d -pid %lu\n",h,w,p);
}

int
main(int argc, char **argv)
{
    get_arguments(argc,argv);
    if (is_env_set("HEART_DEBUG"))
	debug_on=1;
#ifdef __WIN32__
    if (debug_on) {
	if(!is_env_set("ERLSRV_SERVICE_NAME")) {
	    /* this redirects stderr to a separate console (for debugging purposes)*/
	    erlin_fd = _dup(0);
	    erlout_fd = _dup(1);
	    AllocConsole();
	    conh = freopen("CONOUT$","w",stderr);
	    if (conh != NULL)
		fprintf(conh,"console alloced\n");
	}
	debugf("stderr\n");
    }
    _setmode(erlin_fd,_O_BINARY);
    _setmode(erlout_fd,_O_BINARY);
#endif
    strncpy(program_name, argv[0], sizeof(program_name));
    program_name[sizeof(program_name)-1] = '\0';
    notify_ack(erlout_fd);
    cmd[0] = '\0';
    do_terminate(message_loop(erlin_fd,erlout_fd));
    return 0;
}

/*
 * message loop
 */
static int
message_loop(erlin_fd, erlout_fd)
     int   erlin_fd, erlout_fd;
{
  int   i;
  time_t now, last_received;
#ifdef __WIN32__
  DWORD wresult;
#else
  fd_set read_fds;
  int   max_fd;
  struct timeval timeout;
  int   tlen;			/* total message length */
  struct msg m, *mp = &m;
#endif
  
  init_timestamp();
  timestamp(&now);
  last_received = now;
#ifdef __WIN32__
  hevent_dataready = CreateEvent(NULL,FALSE,FALSE,NULL);
  hreader_thread = start_reader_thread();
#else
  max_fd = erlin_fd;
#endif

  while (1) {
#ifdef __WIN32__
	wresult = WaitForSingleObject(hevent_dataready,SELECT_TIMEOUT*1000+ 2);
	if (wresult == WAIT_FAILED) {
		print_last_error();
		return R_ERROR;
	}

	if (wresult == WAIT_TIMEOUT) {
		debugf("wait timed out\n");
		i = 0;
	} else {
		debugf("wait ok\n");
		i = 1;
	}
#else
    FD_ZERO(&read_fds);         /* ZERO on each turn */
    FD_SET(erlin_fd, &read_fds);
    timeout.tv_sec = SELECT_TIMEOUT;  /* On Linux timeout is modified 
					 by select */
    timeout.tv_usec = 0;
    if ((i = select(max_fd + 1, &read_fds, NULLFDS, NULLFDS, &timeout)) < 0) {
      print_error("error in select.");
      return R_ERROR;
    }
#endif
    /*
     * Maybe heart beat time-out
     * If we havn't got anything in 60 seconds we reboot, even if we may
     * have got something in the last 5 seconds. We may end up here if
     * the system clock is adjusted with more than 55 seconds, but we
     * regard this as en error and reboot anyway.
     */
    timestamp(&now);
    if (now > last_received + heart_beat_timeout) {
		print_error("heart-beat time-out.");
		return R_TIMEOUT;
    }
    /*
     * Do not check fd-bits if select timeout
     */
    if (i == 0) {
      continue;
    }
    /*
     * Message from ERLANG
     */
#ifdef __WIN32__
	if (wresult == WAIT_OBJECT_0) {
		if (tlen < 0) {
#else
    if (FD_ISSET(erlin_fd, &read_fds)) {
		if ((tlen = read_message(erlin_fd, mp)) < 0) {
#endif
			print_error("error in read_message.");
			return R_ERROR;
		}
		if ((tlen > MSG_HDR_SIZE) && (tlen <= MSG_TOTAL_SIZE)) {
			switch (mp->op) {
			case HEART_BEAT:
				timestamp(&last_received);
#ifdef USE_WATCHDOG
				/* reset the hardware watchdog timer */
				wd_reset();
#endif
				break;
			case SHUT_DOWN:
				return R_SHUT_DOWN;
			case SET_CMD:
				/* override the HEART_COMMAND_ENV command */
			        memcpy(&cmd, &(mp->fill[0]), 
				       tlen-MSG_HDR_PLUS_OP_SIZE);
			        cmd[tlen-MSG_HDR_PLUS_OP_SIZE] = '\0';
			        notify_ack(erlout_fd);
			        break;
			case CLEAR_CMD:
				/* use the HEART_COMMAND_ENV command */
				cmd[0] = '\0';
				notify_ack(erlout_fd);
				break;
			case GET_CMD:
				/* send back command string */
			        {
				    char *env = NULL;
				    char *command
					= (cmd[0]
					   ? (char *)cmd
					   : (env = get_env(HEART_COMMAND_ENV)));
				    /* Not set and not in env  return "" */
				    if (!command) command = "";
				    heart_cmd_reply(erlout_fd, command);
				    free_env_val(env);
				}
			        break;
			default:
				/* ignore all other messages */
				break;
			}
		} else if (tlen == 0) {
		/* Erlang has closed its end */
		print_error("Erlang has closed.");
		return R_CLOSED;
    }
		/* Junk erroneous messages */
    }
  }
}

#if defined(__WIN32__)
static void 
kill_old_erlang(void){
    HANDLE erlh;
    DWORD exit_code;
    if(heart_beat_kill_pid != 0){
	if((erlh = OpenProcess(PROCESS_TERMINATE | 
			       SYNCHRONIZE | 
			       PROCESS_QUERY_INFORMATION ,
			       FALSE,
			       (DWORD) heart_beat_kill_pid)) == NULL){
	    return;
	}
	if(!TerminateProcess(erlh, 1)){
	    CloseHandle(erlh);
	    return;
	}
	if(WaitForSingleObject(erlh,5000) != WAIT_OBJECT_0){
	    print_error("Old process did not die, "
			"WaitForSingleObject timed out.");
	    CloseHandle(erlh);
	    return;
	}
	if(!GetExitCodeProcess(erlh, &exit_code)){
	    print_error("Old process did not die, "
			"GetExitCodeProcess failed.");
	}
	CloseHandle(erlh);
    }
}
#elif !defined(VXWORKS)
/* Unix eh? */
static void 
kill_old_erlang(void){
    pid_t pid;
    int i;
    int res;
    if(heart_beat_kill_pid != 0){
	pid = (pid_t) heart_beat_kill_pid;
	res = kill(pid,SIGKILL);
	for(i=0; i < 5 && res == 0; ++i){
	    sleep(1);
	    res = kill(pid,SIGKILL);
	}
	if(errno != ESRCH){
	    print_error("Unable to kill old process, "
			"kill failed (tried multiple times).");
	}
    }
}
#endif /* Not on VxWorks */

#ifdef __WIN32__
void win_system(char *command)
{
    char *comspec;
    char * cmdbuff;
    char * extra = " /C ";
    char *env;
    STARTUPINFO start;
    SECURITY_ATTRIBUTES attr;
    PROCESS_INFORMATION info;

    if (!debug_on || test_win95()) {
	system(command);
	return;
    }
    comspec = env = get_env("COMSPEC");
    if (!comspec)
	comspec = "CMD.EXE";
    cmdbuff = malloc(strlen(command) + strlen(comspec) + strlen(extra) + 1);
    if (!cmdbuff) {
	print_error("Failed to allocate memory. Terminating...");
	exit(1);
    }
    strcpy(cmdbuff, comspec);
    strcat(cmdbuff, extra);
    strcat(cmdbuff, command);
    free_env_val(env);

    debugf("running \"%s\"\r\n", cmdbuff);

    memset (&start, 0, sizeof (start));
    start.cb = sizeof (start);
    start.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    start.wShowWindow = SW_HIDE;
    start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    start.hStdOutput = GetStdHandle(STD_ERROR_HANDLE);
    start.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    attr.nLength = sizeof(attr);
    attr.lpSecurityDescriptor = NULL;
    attr.bInheritHandle = TRUE;

    fflush(stderr);

    if (!CreateProcess(NULL,
		       cmdbuff,
		       &attr,
		       NULL,
		       TRUE,
		       0,
		       NULL,
		       NULL,
		       &start,
		       &info)) {
	debugf("Could not create process for the command %s.\r\n", cmdbuff);
    }
    WaitForSingleObject(info.hProcess,INFINITE);
    free(cmdbuff);
}
#endif /* defined(__WIN32__) */

/*
 * do_terminate
 */
static void 
do_terminate(reason)
  int reason;
{
  /*
    When we get here, we have HEART_BEAT_BOOT_DELAY secs to finish
    (plus heart_beat_report_delay if under VxWorks), so we don't need
    to call wd_reset().
    */
  
  switch (reason) {
  case R_SHUT_DOWN:
    break;
  case R_TIMEOUT:
  case R_ERROR:
  case R_CLOSED:
  default:
#if defined(__WIN32__) /* Not VxWorks */
    {
      if(!cmd[0]) {
	char *command = get_env(HEART_COMMAND_ENV);
	if(!command)
	  print_error("Would reboot. Terminating.");
	else {
	  kill_old_erlang();
	  /* High prio combined with system() works badly indeed... */
	  SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
	  win_system(command);
	  print_error("Executed \"%s\". Terminating.",command);
	}
	free_env_val(command);
      }
      else {
	kill_old_erlang();
	/* High prio combined with system() works badly indeed... */
	SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
	win_system(&cmd[0]);
	print_error("Executed \"%s\". Terminating.",cmd);
      }
    }

#else
    {
      if(!cmd[0]) {
	char *command = get_env(HEART_COMMAND_ENV);
	if(!command)
	  print_error("Would reboot. Terminating.");
	else {
	  kill_old_erlang();
	  /* suppress gcc warning with 'if' */
	  if(system(command));
	  print_error("Executed \"%s\". Terminating.",command);
	}
	free_env_val(command);
      }
      else {
	kill_old_erlang();
	/* suppress gcc warning with 'if' */
	if(system((char*)&cmd[0]));
	print_error("Executed \"%s\". Terminating.",cmd);
      }
    }
    break;
#endif
  } /* switch(reason) */
}

/*
 * notify_ack
 *
 * Sends an HEART_ACK.
 */
static int
notify_ack(fd)
  int   fd;
{
  struct msg m;
  
  m.op = HEART_ACK;
  m.len = htons(1);
  return write_message(fd, &m);
}


/*
 * send back current command
 *
 * Sends an HEART_CMD.
 */
static int
heart_cmd_reply(int fd, char *s)
{
  struct msg m;
  int len = strlen(s);

  /* if s >= MSG_BODY_SIZE, return a write
   * failure immediately.
   */
  if (len >= sizeof(m.fill))
      return -1;

  m.op = HEART_CMD;
  m.len = htons(len + 1);	/* Include Op */
  strcpy((char*)m.fill, s);

  return write_message(fd, &m);
}


/*
 *  write_message
 *
 *  Writes a message to a blocking file descriptor. Returns the total
 *  size of the message written (always > 0), or -1 if error.
 *
 *  A message which is too short or too long, is not written. The return
 *  value is then MSG_HDR_SIZE (2), as if the message had been written.
 *  Is this really necessary? Can't we assume that the length is ok?
 *  FIXME.
 */
static int
write_message(fd, mp)
  int   fd;
  struct msg *mp;
{
  int   len;
  char* tmp;

  tmp = (char*) &(mp->len);
  len = (*tmp * 256) + *(tmp+1); 
  if ((len == 0) || (len > MSG_BODY_SIZE)) {
    return MSG_HDR_SIZE;
  }				/* cc68k wants (char *) */
  if (write(fd, (char *) mp, len + MSG_HDR_SIZE) != len + MSG_HDR_SIZE) {
    return -1;
  }
  return len + MSG_HDR_SIZE;
}

/*
 *  read_message
 *
 *  Reads a message from a blocking file descriptor. Returns the total
 *  size of the message read (> 0), 0 if eof, and < 0 if error.
 *
 *  Note: The return value MSG_HDR_SIZE means a message of total size
 *  MSG_HDR_SIZE, i.e. without even an operation field.
 *
 *  If the size of the message is larger than MSG_TOTAL_SIZE, the total
 *  number of bytes read is returned, but the buffer contains a truncated
 *  message.
 */
static int
read_message(fd, mp)
  int   fd;
  struct msg *mp;
{
  int   rlen, i;
  unsigned char* tmp;

  if ((i = read_fill(fd, (char *) mp, MSG_HDR_SIZE)) != MSG_HDR_SIZE) {
    /* < 0 is an error; = 0 is eof */
    return i;
  }

  tmp = (unsigned char*) &(mp->len);
  rlen = (*tmp * 256) + *(tmp+1);
  if (rlen == 0) {
    return MSG_HDR_SIZE;
  }
  if (rlen > MSG_BODY_SIZE) {
    if ((i = read_skip(fd, (((char *) mp) + MSG_HDR_SIZE),
		       MSG_BODY_SIZE, rlen)) != rlen) {
      return i;
    } else {
      return rlen + MSG_HDR_SIZE;
    }
  }
  if ((i = read_fill(fd, ((char *) mp + MSG_HDR_SIZE), rlen)) != rlen) {
    return i;
  }
  return rlen + MSG_HDR_SIZE;
}

/*
 *  read_fill
 *
 *  Reads len bytes into buf from a blocking fd. Returns total number of
 *  bytes read (i.e. len) , 0 if eof, or < 0 if error. len must be > 0.
 */
static int
read_fill(fd, buf, len)
  int   fd, len;
  char *buf;
{
  int   i, got = 0;

  do {
    if ((i = read(fd, buf + got, len - got)) <= 0) {
      return i;
    }
    got += i;
  } while (got < len);
  return len;
}

/*
 *  read_skip
 *
 *  Reads len bytes into buf from a blocking fd, but puts not more than
 *  maxlen bytes in buf. Returns total number of bytes read ( > 0),
 *  0 if eof, or < 0 if error. len > maxlen > 0 must hold.
 */
static int
read_skip(fd, buf, maxlen, len)
  int   fd, maxlen, len;
  char *buf;
{
  int   i, got = 0;
  char  c;

  if ((i = read_fill(fd, buf, maxlen)) <= 0) {
    return i;
  }
  do {
    if ((i = read(fd, &c, 1)) <= 0) {
      return i;
    }
    got += i;
  } while (got < len - maxlen);
  return len;
}

/*
 *  print_error
 */
static void
print_error(const char *format,...)
{
  va_list args;
  time_t now;
  char *timestr;

  va_start(args, format);
  time(&now);
  timestr = ctime(&now);
  fprintf(stderr, "%s: %.*s: ", program_name, (int) strlen(timestr)-1, timestr);
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\r\n");
}

static void
debugf(const char *format,...)
{
  va_list args;

  if (!debug_on) return;
  va_start(args, format);
  fprintf(stderr, "Heart: ");
  vfprintf(stderr, format, args);
  va_end(args);
  fprintf(stderr, "\r\n");
}

#ifdef __WIN32__
void print_last_error() {
	LPVOID lpMsgBuf;
	FormatMessage( 
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL,
		GetLastError(),
		MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), /* Default language */
		(LPTSTR) &lpMsgBuf,
		0,
		NULL 
	);

	/* Display the string.*/
	fprintf(stderr,"GetLastError:%s\n",lpMsgBuf);

	/* Free the buffer. */
	LocalFree( lpMsgBuf );
}

static int test_win95(void)
{
    OSVERSIONINFO osinfo;
    osinfo.dwOSVersionInfoSize=sizeof(OSVERSIONINFO);
    GetVersionEx(&osinfo);
    if (osinfo.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) 
	return 1;
    else
	return 0;
}

static BOOL enable_privilege() {
	HANDLE ProcessHandle;
	DWORD DesiredAccess = TOKEN_ADJUST_PRIVILEGES;
	HANDLE TokenHandle;
	TOKEN_PRIVILEGES Tpriv;
	LUID luid;
	ProcessHandle = GetCurrentProcess();
	OpenProcessToken(ProcessHandle, DesiredAccess, &TokenHandle);
	LookupPrivilegeValue(0,SE_SHUTDOWN_NAME,&luid);
	Tpriv.PrivilegeCount = 1;
	Tpriv.Privileges[0].Luid = luid;
	Tpriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	return AdjustTokenPrivileges(TokenHandle,FALSE,&Tpriv,0,0,0);
}

static BOOL do_shutdown(int really_shutdown) {
    if (test_win95()) {
    	if (ExitWindowsEx(EWX_REBOOT,0)) {
		return TRUE;
	} else {
		print_last_error();
		return FALSE;
	}
    } else {
	enable_privilege();
	if (really_shutdown) {
	    if (InitiateSystemShutdown(NULL,"shutdown by HEART",10,TRUE,TRUE))
		return TRUE;
	} else if (InitiateSystemShutdown(NULL,
					  "shutdown by HEART\n"
					  "will be interrupted",
					  30,TRUE,TRUE)) {
	    AbortSystemShutdown(NULL);
	    return TRUE;
	}
	return FALSE;
    }
}

DWORD WINAPI reader(LPVOID lpvParam) {

	while (1) {
		debugf("reader is reading\n");
		tlen = read_message(erlin_fd, mp);
		debugf("reader setting event\n");
		SetEvent(hevent_dataready);
		if(tlen == 0)
		    break;
	}
	return 0;
}

HANDLE start_reader_thread(void) {
	DWORD tid;
	HANDLE thandle;
	if ((thandle = (HANDLE) 
	     _beginthreadex(NULL,0,reader,NULL,0,&tid)) == NULL) {
		print_last_error();
		exit(1);
	}
	return thandle;
}
#endif

#if defined(__WIN32__)

#  define TICK_MASK 0x7FFFFFFFUL

void init_timestamp(void)
{
}

time_t timestamp(time_t *res)
{
    static time_t extra = 0;
    static unsigned last_ticks = 0;
    unsigned this_ticks;
    time_t r;

    this_ticks = GetTickCount() & TICK_MASK;

    if (this_ticks < last_ticks) {
	extra += (time_t) ((TICK_MASK + 1) / 1000);
    }

    last_ticks = this_ticks;

    r = ((time_t) (this_ticks / 1000)) + extra;

    if (res != NULL)
	*res = r;
    return r;
}

#elif defined(VXWORKS)

static WDOG_ID watchdog_id;
static volatile unsigned elapsed;
static WIND_TCB *this_task;
/* A simple variable is enough to lock the time update, as the
   watchdog is run at interrupt level and never preempted. */
static volatile int lock_time; 

static void my_delete_hook(WIND_TCB *tcb) 
{ 
    if (tcb == this_task) {
	wdDelete(watchdog_id);
	watchdog_id = NULL;
	taskDeleteHookDelete((FUNCPTR) &my_delete_hook);
    }
}

static void my_wd_routine(int count)
{
    if (watchdog_id != NULL) {
	++count;
	if (!lock_time) {
	    elapsed += count;
	    count = 0;
	}
	wdStart(watchdog_id, sysClkRateGet(), 
		(FUNCPTR) &my_wd_routine, count);
    }
}

void init_timestamp(void)
{
    lock_time = 0;
    elapsed = 0;
    watchdog_id = wdCreate();
    this_task = (WIND_TCB *) taskIdSelf();
    taskDeleteHookAdd((FUNCPTR) &my_delete_hook);
    wdStart(watchdog_id, sysClkRateGet(), 
	    (FUNCPTR) &my_wd_routine, 0);
}

time_t timestamp(time_t *res)
{
    time_t r;
    ++lock_time;
    r = (time_t) elapsed;
    --lock_time;
    if (res != NULL)
	*res = r;
    return r;
}
   
#elif defined(HAVE_GETHRTIME) 

void init_timestamp(void)
{
}

time_t timestamp(time_t *res)
{
    hrtime_t ht = gethrtime();
    time_t r = (time_t) (ht / 1000000000);
    if (res != NULL)
	*res = r;
    return r;
}

#elif defined(HEART_CORRECT_USING_TIMES)

#  ifdef NO_SYSCONF
#    include <sys/param.h>
#    define TICKS_PER_SEC()	HZ
#  else
#    define TICKS_PER_SEC()	sysconf(_SC_CLK_TCK)
#  endif

#  define TICK_MASK 0x7FFFFFFFUL

static unsigned tps;

void init_timestamp(void)
{
    tps = TICKS_PER_SEC();
}

time_t timestamp(time_t *res)
{
    static time_t extra = 0;
    static clock_t last_ticks = 0;
    clock_t this_ticks;
    struct tms dummy;
    time_t r;

    this_ticks = (times(&dummy) & TICK_MASK);

    if (this_ticks < last_ticks) {
	extra += (time_t) ((TICK_MASK + 1) / tps);
    }

    last_ticks = this_ticks;

    r = ((time_t) (this_ticks / tps)) + extra;

    if (res != NULL)
	*res = r;
    return r;
}

#else

void init_timestamp(void)
{
}

time_t timestamp(time_t *res)
{
    return time(res);
}

#endif
