/*
 * Fake cygwin pty   --  fakecygpty --
 *
 *        Copyright (C) 2005 Kyotaro Horiguchi <horiguti@meaodwy.org>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either versions 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with fakecygpty, see the file COPYING.  If not, write to the
 * Free Software Foundation Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

/*
 * HISTORY
 * -------
 *
 *    09 Jun, 2005 : Version 1.0.0 - first release.
 *    15 Jun, 2005 : Version 1.0.1 - bug fix and change coding style.
 *    15 May, 2009 : Version 1.0.2 - Work around for Windows 7 RC
 *    25 Jan, 2014 : Version 1.1.0 - fix tty attribute like emacs tty.
 *                                   accept SIGWINCH signal for resize.
 *                                   transport some signals to child pid.
 */

/*
 * COMPILATION
 * -------
 * gcc -o fakecygpty.exe fakecygpty.c
 * Note: You must compile it in Cygwin environment. NOT in MinGW32!
 *
 */

/*
 * HOW TO RESIZE TTY WINDOW SIZE?
 * -------
 * Send SIGWINCH signal by sigqueue() with sigval stored window size.
 * window size format: high-16bit => rows, low-16bit => cols
 *
 * ex) set window size cols=140 rows=32 by C code.
 *
 *   union sigval sigval;
 *   pid_t pid = <fakecygpty's pid>;
 *   int cols = 140, rows = 32;
 *
 *   sigval.sival_int = rows << 16 + 0xFFFF & cols;
 *   sigqueue(pid, SIGWINCH, sigval);
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <pty.h>

#define BUFSIZE		 1024	/* size of communication buffer */
#define COMMAND_PREFIX	 "f_"
#define MY_NAME "fakecygpty"
#define PTY_HOLD_MODE "*pty_holder*"

/* prototypes */
void exec_target(char* argv[]);

void init_tty_attribute(struct termios *tm);
void setup_tty_attributes(struct termios *tm);
char *real_command_name(char* my_name);

BOOL WINAPI ctrl_handler(DWORD e);

ssize_t safe_read(int fd, void *buf, size_t count);
ssize_t safe_write_full(int fd, void *buf, size_t count);

void signal_pass_handler(int signum, siginfo_t *info, void *unused);
void sigwinch_handler(int signum, siginfo_t *info, void *unused);
void setup_signal_handlers();

void resize_window(int window_size_info);

void pty_holder(void);

/* signal trapping descriptor */
struct sigtrap_desc {
  int signum;
  void  (*action)(int, siginfo_t *, void *);
};

/* global variables */
int child_pid;		/* pid of child proces  */
int masterfd;		/* fd of pty served to child process */

volatile sig_atomic_t sig_winch_caught = FALSE; /* flag for SIGWINCH caught */
volatile sig_atomic_t sig_window_size = -1;     /* window size info */

/* signals requiring to trap */
struct sigtrap_desc sigtrap_descs[] =
  {
    { SIGHUP,   signal_pass_handler },
    { SIGINT,   signal_pass_handler },
    { SIGQUIT,  signal_pass_handler },
    { SIGALRM,  signal_pass_handler },
    { SIGTERM,  signal_pass_handler },
    { SIGWINCH, sigwinch_handler    },
    { SIGUSR1,  signal_pass_handler },
    { SIGUSR2,  signal_pass_handler },
    { SIGTSTP,  signal_pass_handler },
    { SIGCONT,  signal_pass_handler }
  };

#define SIGTRAP_COUNT (sizeof(sigtrap_descs)/sizeof(struct sigtrap_desc))

/* Create pty and fork/exec target process */
/* This function sets child_pid and masterfd */
void
exec_target(char* argv[])
{
  int pid;
  struct termios tm;

  setup_tty_attributes(&tm);

  pid = forkpty(&masterfd, NULL, &tm, NULL);

  if (pid < 0)
    {
      perror("Failed to fork");
      return;
    }

  if (pid == 0)
    {
      if (strcmp(argv[0], PTY_HOLD_MODE) == 0)
	{
	  pty_holder();
	  exit(0);
	}
      else
	{
	  execvp(argv[0], argv);

	  fprintf(stderr, "Failed to execute \"%s\": %s\n", argv[0], strerror(errno));
	  exit(1);
	}
    }

  child_pid = pid;

  return;
}

void init_tty_attribute(struct termios *tm)
{
  /*
   * initial values from fhandler_termios::tcinit()
   *  in cygwin/winsup/cygwin/fhandler_termios.cc
   */
  tm->c_iflag = BRKINT | ICRNL | IXON;
  tm->c_oflag = OPOST | ONLCR;
  tm->c_lflag = B38400 | CS8 | CREAD;
  tm->c_cflag = ISIG | ICANON | ECHO | IEXTEN;

  tm->c_cc[VDISCARD] = CFLUSH;
  tm->c_cc[VEOL]     = CEOL;
  tm->c_cc[VEOL2]    = CEOL2;
  tm->c_cc[VEOF]     = CEOF;
  tm->c_cc[VERASE]   = CERASE;
  tm->c_cc[VINTR]    = CINTR;
  tm->c_cc[VKILL]    = CKILL;
  tm->c_cc[VLNEXT]   = CLNEXT;
  tm->c_cc[VMIN]     = 1;
  tm->c_cc[VQUIT]    = CQUIT;
  tm->c_cc[VREPRINT] = CRPRNT;
  tm->c_cc[VSTART]   = CSTART;
  tm->c_cc[VSTOP]    = CSTOP;
  tm->c_cc[VSUSP]    = CSUSP;
  tm->c_cc[VSWTC]    = CSWTCH;
  tm->c_cc[VTIME]    = 0;
  tm->c_cc[VWERASE]  = CWERASE;

  tm->c_ispeed = tm->c_ospeed = B38400;
}

void
setup_tty_attributes (struct termios *tm)
{
  init_tty_attribute(tm);

  /* setup values from child_setup_tty() in emacs/src/sysdep.c */
  tm->c_iflag &= ~(IUCLC | ISTRIP);
  tm->c_iflag |= IGNCR;
  tm->c_oflag &= ~(ONLCR | OLCUC | TAB3);
  tm->c_oflag |= OPOST;
  tm->c_lflag &= ~ECHO;
  tm->c_lflag |= ISIG | ICANON;
  tm->c_cc[VERASE] = _POSIX_VDISABLE;
  tm->c_cc[VKILL] = _POSIX_VDISABLE;
  tm->c_cc[VEOF] = CTRL('D');
}

char *
real_command_name(char* my_name)
{
  char *p;

  /* Assume mutlbyte characters do not occur here */
  p = strrchr(my_name, '/');
  if (p == NULL)
    {
      p = strrchr(my_name, '\\');

      if (p == NULL)
	p = my_name;
      else
	p++;
    }
  else
    p++;

  if (strcmp(p, MY_NAME) == 0)
    {
      return NULL;    /* I am invoked as explicit wrapper program */
    }

  if (strncmp(p, COMMAND_PREFIX, strlen (COMMAND_PREFIX)) != 0)
    {
      fprintf(stderr, "Illegal program name format. \"%s\"\n", my_name);
      exit(1);
    }

  return p + strlen(COMMAND_PREFIX);
}

/* Signal handler for convert SIGINT into ^C on pty */
/* This seems not able to be done within cygwin POSIX framework */
BOOL WINAPI
ctrl_handler(DWORD e)
{
  switch (e)
    {
    case CTRL_C_EVENT:
      write(masterfd, "\003", 1);
      return TRUE;

    case CTRL_CLOSE_EVENT:
      kill(child_pid, SIGKILL);
      return FALSE;
    }
  return FALSE;
}

ssize_t safe_read(int fd, void *buf, size_t count)
{
  ssize_t ret;

  do {
    ret = read(fd, buf, count);
  } while(ret < 0 && errno == EINTR);

  return ret;
}

ssize_t safe_write_full(int fd, void *buf, size_t count)
{
  ssize_t ret;

  do {
    ret = write(fd, buf, count);
    if (ret > 0) 
      {
	buf += ret;
	count -= ret;
      }
  } while(count > 0 || (ret < 0 && errno == EINTR));

  return ret;
}

void sigwinch_handler(int signum, siginfo_t *info, void *unused)
{
  sig_winch_caught = TRUE;
  if (info->si_code == SI_QUEUE) 
    sig_window_size = info->si_value.sival_int;
  else
    sig_window_size = -1;
}

void signal_pass_handler(int signum, siginfo_t *info, void *unused)
{
  union sigval sigval;
  int saved_errno;
  
  saved_errno = errno;
  if (info->si_code == SI_QUEUE) 
    {
      sigval = info->si_value;
      sigqueue(child_pid, signum, sigval);
      
    }
  else
    {
      kill(child_pid, signum);
    }
  errno = saved_errno;
}

void setup_signal_handlers()
{
  struct sigaction newsig;
  int i;
  
  newsig.sa_flags = SA_SIGINFO;
  sigemptyset(&newsig.sa_mask);

  for (i = 0; i < SIGTRAP_COUNT; i++)
    {
      newsig.sa_sigaction = sigtrap_descs[i].action;
      if (sigaction(sigtrap_descs[i].signum, &newsig, NULL) < 0)
	fprintf(stderr, "Failed to sigaction on %d: %s\n", 
		sigtrap_descs[i].signum, strerror(errno));
    }
}

void resize_window(int window_size_info)
{
  struct winsize w;
  int ret;

  if (window_size_info >= 0) 
    {
      /* size info: high-16bit => rows, low-16bit => cols */
      w.ws_row = window_size_info >> 16;
      w.ws_col = window_size_info & 0xFFFF;

      do {
	ret = ioctl(masterfd, TIOCSWINSZ, &w);
      } while (ret < 0 && errno == EINTR);

      if (ret == 0)
	kill(child_pid, SIGWINCH);
    }
}

void pty_holder(void)
{
  struct termios tm;
  struct sigaction newsig;

  /* echo on */
  if (tcgetattr(0, &tm) == 0) {
    tm.c_lflag |= ECHO;
    tcsetattr(0, TCSANOW, &tm);
  }

  /* ignore some signals */
  memset(&newsig, 0, sizeof(newsig));
  newsig.sa_handler = SIG_IGN;
  sigemptyset(&newsig.sa_mask);

  sigaction(SIGINT, &newsig, NULL);
  sigaction(SIGQUIT, &newsig, NULL);
  sigaction(SIGTSTP, &newsig, NULL);

  /* do nothing */
  while (1)
    sleep(1);
}

int
main(int argc, char* argv[])
{
  fd_set sel, sel0;
  int status;
  char* newarg0;

  /* SIGINT and SIGBREAK are indistinctive under cygwin environment. */
  /* Using Win32API to handle SIGINT.                              */
  SetConsoleCtrlHandler(ctrl_handler, TRUE);

  if (argc < 1)
    {
      fputs("Unable to get arg[0].", stderr);
      exit(1);
    }

  newarg0 = real_command_name(argv[0]);

  if (newarg0)
    argv[0] = newarg0;
  else if (argc >=2)
    argv++;
  else
    argv[0] = PTY_HOLD_MODE;

  if (isatty(0))
    {
      if (strcmp(argv[0], PTY_HOLD_MODE) == 0)
	exit(1);
      execvp(argv[0], argv);
      fprintf(stderr, "Failed to execute \"%s\": %s\n", argv[0], strerror(errno));
      exit(1);
    }
  
  exec_target(argv); /* This sets globals masterfd, child_pid */

  setup_signal_handlers();

  FD_ZERO(&sel0);
  FD_SET(masterfd, &sel0);
  FD_SET(0, &sel0);

  /* communication loop */
  while (1)
    {
      char buf[BUFSIZE];
      int ret;

      if (sig_winch_caught == TRUE)
	{
	  sig_winch_caught = FALSE;
	  resize_window(sig_window_size);
	}

      sel = sel0;
      if (select (FD_SETSIZE, &sel, NULL, NULL, NULL) <= 0) 
	{
	  if(errno == EINTR)
	    continue;
	  else
	    break;
	}
      
      if (FD_ISSET(masterfd, &sel))
	{
	  ret = safe_read(masterfd, buf, BUFSIZE);
	  if (ret > 0)
	    safe_write_full(1, buf, ret);
	  else
	    break;
	}
      else if (FD_ISSET(0, &sel))
	{
	  ret = safe_read(0, buf, BUFSIZE);
	  if (ret > 0)
	    safe_write_full(masterfd, buf, ret);
	  else
	    {
	      FD_CLR(0, &sel0);
	      close(masterfd);
	    }
	}
    }

  while(waitpid(child_pid, &status, 0) < 0 && errno == EINTR)
    ;
  
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  else if(WIFSIGNALED(status)) /* ntemacs cannot distinct killed by signal */
    return 0x80 +  WTERMSIG(status); 
  
  return 0;
}
