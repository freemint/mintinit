/* This file is based on Miquel van Smoorenburg's System-V init clone
 * for linux but heavily rehacked by Guido Flohr for MiNT.
 */
/*
 * Init               A System-V Init Clone.
 *
 * Usage:       /sbin/init
 *                   init [0123456SsQqAaBbCc]
 *                telinit [0123456SsQqAaBbCc]
 *
 * Version:     @(#)init.c  2.74  08-Mar-1998  MvS
 *
 *              This file is part of the sysvinit suite,
 *              Copyright 1991-1998 Miquel van Smoorenburg.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Modified:    21 Feb 1998, Al Viro:
 *              'U' flag added to telinit. It forces init to re-exec itself
 *              (passing its state through exec, certainly).
 *              May be useful for smoother (heh) upgrades.
 *              24 Feb 1998, AV:
 *              did_boot made global and added to state - thanks, Miquel.
 *              Yet another file descriptors leak - close state pipe if 
 *              re_exec fails.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <termios.h>
#include <utmp.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/syslog.h>
#include <sys/time.h>
#include <paths.h>
#include <termios.h>
#include <mintbind.h>

#include "init.h"
#include "initreq.h"
#include "initpaths.h"
#include "reboot.h"
#include "set.h"

#define DEV_INITCTL "/dev/initctl"

#ifndef SA_RESTART
#define SA_RESTART 0
#endif

/* Set a signal handler. */
#define SETSIG(sa, sig, fun)	sa.sa_handler = fun; \
				sa.sa_flags = SA_RESTART; \
				sigaction(sig, &sa, NULL);

/* Version information */
char *Version = "mintinit " VERSION;
char *bootmsg = "version " VERSION " booting";
#define E_VERSION "INIT_VERSION=mintinit-" VERSION

CHILD *family = NULL;		/* The linked list of all entries */
CHILD *newFamily = NULL;	/* The list after inittab re-read */

CHILD ch_emerg = {		/* Emergency shell */
  0, 0, 0, 0, 0,
  "~~",
  "S",
  3,
  "/sbin/sulogin",
  NULL,
  NULL
};

/* Dummy request to interrupt select, see comment in check_init_fifo.  */
static struct init_request dummy_request = { INIT_MAGIC, INIT_CMD_DUMMY, 0 };

char runlevel = 'S';		/* The current run level */
char thislevel = 'S';		/* The current runlevel */
char prevlevel = 'N';		/* Previous runlevel */
int dfl_level = 0;		/* Default runlevel */
int got_cont = 0;		/* Set if we received the SIGCONT signal */
int emerg_shell = 0;		/* Start emergency shell? */
unsigned int got_signals;	/* Set if we received a signal. */
int wrote_reboot = 1;		/* Set when we wrote the reboot record */
int sltime = 5;			/* Sleep time between TERM and KILL */
char *argv0;			/* First arguments; show up in ps listing */
int maxproclen;			/* Maximal length of argv[0] */
struct utmp utproto;		/* Only used for sizeof(utproto.ut_id) */
char *console_dev = CONSOLE;	/* Console device. */
int wrote_iosave = 0;		/* Did we write ioctl.save yet? */
int pipe_fd = -1;		/* /dev/initctl */
int did_boot = 0;		/* Did we already do BOOT* stuff? */

/*      Used by re-exec part */
int reload = 0;			/* Should we do initialization stuff? */
char *myname = "/sbin/init";	/* What should we exec */
int oops_error;			/* Used by some of the re-exec code. */
char *Signature = "12567362";	/* Signature for re-exec fd */

/* Macro to see if this is a special action */
#define ISPOWER(i) ((i) == POWERWAIT || (i) == POWERFAIL || \
		    (i) == POWEROKWAIT || (i) == POWERFAILNOW || \
		    (i) == CTRLALTDEL)

/* ascii values for the `action' field. */
struct actions
{
  char *name;
  int act;
} actions[] =
{

  {
  "respawn", RESPAWN},
  {
  "wait", WAIT},
  {
  "once", ONCE},
  {
  "boot", BOOT},
  {
  "bootwait", BOOTWAIT},
  {
  "powerfail", POWERFAIL},
  {
  "powerfailnow", POWERFAILNOW},
  {
  "powerwait", POWERWAIT},
  {
  "powerokwait", POWEROKWAIT},
  {
  "ctrlaltdel", CTRLALTDEL},
  {
  "off", OFF},
  {
  "ondemand", ONDEMAND},
  {
  "initdefault", INITDEFAULT},
  {
  "sysinit", SYSINIT},
  {
  "kbrequest", KBREQUEST},
  {
NULL, 0},};

/*
 *    State parser token table (see receive_state)
 */
struct
{
  char name[4];
  int cmd;
} cmds[] =
{

  {
  "VER", C_VER},
  {
  "END", C_END},
  {
  "REC", C_REC},
  {
  "EOR", C_EOR},
  {
  "LEV", C_LEV},
  {
  "FL ", C_FLAG},
  {
  "AC ", C_ACTION},
  {
  "CMD", C_PROCESS},
  {
  "PID", C_PID},
  {
  "EXS", C_EXS},
  {
  "-RL", D_RUNLEVEL},
  {
  "-TL", D_THISLEVEL},
  {
  "-PL", D_PREVLEVEL},
  {
  "-SI", D_GOTSIGN},
  {
  "-WR", D_WROTE_REBOOT},
  {
  "-ST", D_SLTIME},
  {
  "-DB", D_DIDBOOT},
  {
  "", 0}
};
struct
{
  char *name;
  int mask;
} flags[] =
{

  {
  "RU", RUNNING},
  {
  "DE", DEMAND},
  {
  "XD", XECUTED},
  {
  NULL, 0}
};

/*
 *    Send the state info of the previous running init to
 *      the new one, in a version-independant way.
 */
void
send_state (int fd)
{
  FILE *fp;
  CHILD *p;
  int i, val;

  fp = fdopen (fd, "w");

  fprintf (fp, "VER%s\n", Version);
  fprintf (fp, "-RL%c\n", runlevel);
  fprintf (fp, "-TL%c\n", thislevel);
  fprintf (fp, "-PL%c\n", prevlevel);
  fprintf (fp, "-SI%u\n", got_signals);
  fprintf (fp, "-WR%d\n", wrote_reboot);
  fprintf (fp, "-ST%d\n", sltime);
  fprintf (fp, "-DB%d\n", did_boot);

  for (p = family; p; p = p->next)
    {
      fprintf (fp, "REC%s\n", p->id);
      fprintf (fp, "LEV%s\n", p->rlevel);
      for (i = 0, val = p->flags; flags[i].mask; i++)
	if (val & flags[i].mask)
	  {
	    val &= ~flags[i].mask;
	    fprintf (fp, "FL %s\n", flags[i].name);
	  }
      fprintf (fp, "PID%d\n", p->pid);
      fprintf (fp, "EXS%u\n", p->exstat);
      for (i = 0; actions[i].act; i++)
	if (actions[i].act == p->action)
	  {
	    fprintf (fp, "AC %s\n", actions[i].name);
	    break;
	  }
      fprintf (fp, "CMD%s\n", p->process);
      fprintf (fp, "EOR\n");
    }
  fprintf (fp, "END\n");
  fclose (fp);
}

/*
 *    Read a string from a file descriptor.
 *      FIXME: why not use fgets() ?
 */
static int
get_string (char *p, int size, FILE * f)
{
  int c;

  while ((c = getc (f)) != EOF && c != '\n')
    {
      if (--size > 0)
	*p++ = c;
    }
  *p = '\0';
  return (c != EOF) && (size > 0);
}

/*
 *    Read trailing data from the state pipe until we see a newline.
 */
static int
get_void (FILE * f)
{
  int c;

  while ((c = getc (f)) != EOF && c != '\n');

  return (c != EOF);
}

/*
 *    Read the next "command" from the state pipe.
 */
static int
get_cmd (FILE * f)
{
  char cmd[4] = "   ";
  int i;

  if (fread (cmd, 1, 3, f) != 3)
    return C_EOF;

  for (i = 0; cmds[i].cmd && strcmp (cmds[i].name, cmd) != 0; i++);
  return cmds[i].cmd;
}

/*
 *    Read a CHILD * from the state pipe.
 */
static CHILD *
get_record (FILE * f)
{
  int cmd;
  char s[32];
  int i;
  CHILD *p;

  do
    {
      switch (cmd = get_cmd (f))
	{
	case C_END:
	  get_void (f);
	  return NULL;
	case 0:
	  get_void (f);
	  break;
	case C_REC:
	  break;
	case D_RUNLEVEL:
	  fscanf (f, "%c\n", &runlevel);
	  break;
	case D_THISLEVEL:
	  fscanf (f, "%c\n", &thislevel);
	  break;
	case D_PREVLEVEL:
	  fscanf (f, "%c\n", &prevlevel);
	  break;
	case D_GOTSIGN:
	  fscanf (f, "%u\n", &got_signals);
	  break;
	case D_WROTE_REBOOT:
	  fscanf (f, "%d\n", &wrote_reboot);
	  break;
	case D_SLTIME:
	  fscanf (f, "%d\n", &sltime);
	  break;
	case D_DIDBOOT:
	  fscanf (f, "%d\n", &did_boot);
	  break;
	default:
	  if (cmd > 0 || cmd == C_EOF)
	    {
	      oops_error = -1;
	      return NULL;
	    }
	}
    }
  while (cmd != C_REC);

  if ((p = (CHILD *) malloc (sizeof (CHILD))) == NULL)
    {
      oops_error = -1;
      return NULL;
    }
  memset (p, 0, sizeof (CHILD));
  get_string (p->id, sizeof (p->id), f);
  do
    switch (cmd = get_cmd (f))
      {
      case 0:
      case C_EOR:
	get_void (f);
	break;
      case C_PID:
	fscanf (f, "%d\n", &(p->pid));
	break;
      case C_EXS:
	fscanf (f, "%u\n", &(p->exstat));
	break;
      case C_LEV:
	get_string (p->rlevel, sizeof (p->rlevel), f);
	break;
      case C_PROCESS:
	get_string (p->process, sizeof (p->process), f);
	break;
      case C_FLAG:
	get_string (s, 32, f);
	for (i = 0; flags[i].name; i++)
	  {
	    if (strcmp (flags[i].name, s) == 0)
	      break;
	  }
	p->flags |= flags[i].mask;
	break;
      case C_ACTION:
	get_string (s, 32, f);
	for (i = 0; actions[i].name; i++)
	  {
	    if (strcmp (actions[i].name, s) == 0)
	      break;
	  }
	p->action = actions[i].act ? : OFF;
	break;
      default:
	free (p);
	oops_error = -1;
	return NULL;
      }
  while (cmd != C_EOR);

  return p;
}

/*
 *    Read the complete state info from the state pipe.
 *      Returns 0 on success
 */
int
receive_state (int fd)
{
  FILE *f;
  char old_version[256];
  CHILD **pp;

  f = fdopen (fd, "r");

  if (get_cmd (f) != C_VER)
    return -1;
  get_string (old_version, 256, f);
  oops_error = 0;
  for (pp = &family; (*pp = get_record (f)) != NULL; pp = &((*pp)->next));
  fclose (f);
  return oops_error;
}

/*
 *    Set the process title. We do not check for overflow of
 *      the stack space since we know there is plenty for
 *      our needs and we'll never use more than 10 bytes anyway.
 */
int
setproctitle (char *fmt, ...)
{
  va_list ap;
  int len;
  char buf[256];

  buf[0] = 0;

  va_start (ap, fmt);
  len = vsprintf (buf, fmt, ap);
  va_end (ap);

  memset (argv0, 0, maxproclen + 1);
  strncpy (argv0, buf, maxproclen);

  return len;
}

/*
 *    Open a device with retries.
 */
int
serial_open (char *dev, int mode)
{
  int f, fd;
  int m;

  /*
   *    Open device in nonblocking mode.
   */
  m = mode | O_NONBLOCK;

  /*
   *    Retry the open five times.
   */
  for (f = 0; f < 5; f++)
    if ((fd = open (dev, m)) >= 0)
      break;
  if (fd < 0)
    return (fd);

  /*
   *    Set original flags.
   */
  if (m != mode)
    fcntl (fd, F_SETFL, mode);
  return (fd);
}

/*
 *    We got a signal (HUP PWR WINCH ALRM INT)
 */
void
signal_handler (int sig)
{
  /* Write the dummy request into our fifo.  */
  int fd = open (INIT_FIFO, O_WRONLY | O_NDELAY);
  (void) write (fd, &dummy_request, sizeof dummy_request);
  (void) close (fd);

  ADDSET (got_signals, sig);
}

/*
 *    SIGCHLD: one of our children has died.
 */
void
chld_handler ()
{
  CHILD *ch;
  int pid, st;

  /* Write the dummy request into our fifo.  */
  int fd = open (INIT_FIFO, O_WRONLY | O_NDELAY);
  (void) write (fd, &dummy_request, sizeof dummy_request);
  (void) close (fd);

  /*
   *    Find out which process(es) this was (were)
   */
  while ((pid = waitpid (-1, &st, WNOHANG)) != 0)
    {
      if (errno == ECHILD)
	break;
      for (ch = family; ch; ch = ch->next)
	if (ch->pid == pid && (ch->flags & RUNNING))
	  {
#if DEBUG
	    ilog (L_VB, "chld_handler: marked %d as zombie", ch->pid);
#endif
	    ADDSET (got_signals, SIGCHLD);
	    ch->exstat = st;
	    ch->flags |= ZOMBIE;
	    if (ch->new)
	      {
		ch->new->exstat = st;
		ch->new->flags |= ZOMBIE;
	      }
	    break;
	  }
#if DEBUG
      if (ch == NULL)
	log (L_VB, "chld_handler: unknown child %d exited.", pid);
#endif
    }
}

/*
 *    Linux ignores all signals sent to init when the
 *      SIG_DFL handler is installed. Therefore we must catch SIGTSTP
 *      and SIGCONT, or else they won't work....
 *
 *      The SIGCONT handler
 */
void
cont_handler ()
{
  got_cont = 1;
}

/*
 *    OOPS: segmentation violation!
 */
void
segv_handler ()
{
  ilog (L_VB, "PANIC: segmentation violation! giving up..");
  while (1)
    pause ();
}

/*
 *    The SIGSTOP & SIGTSTP handler
 */
void
stop_handler ()
{
  got_cont = 0;
  while (!got_cont)
    pause ();
  got_cont = 0;
}

/*
 *    Set terminal settings to reasonable defaults
 */
void
set_term (int how)
{
  struct termios tty;
  int fd;
  int fd2 = -1;
  int restore_ok = 0;

  if ((fd = serial_open (console_dev, O_RDWR | O_NOCTTY)) < 0)
    {
      ilog (L_VB, "can't open %s", console_dev);
      return;
    }
  /*
   *    Do we want to save or restore modes.
   */
  if (how > 0)
    {
      /*
       *    Save the terminal settings.
       */
      if (tcgetattr (fd, &tty) != 0)
	{
	  ilog (L_VB, "can't get terminal attributes of %s: %s\n",
	       strerror (errno));
	  (void) close (fd);
	  (void) close (fd2);
	  return;
	}

      if ((fd2 = open (IOSAVE, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
	{
	  if (errno != EROFS)
	    ilog (L_VB, "can't open(%s, O_WRONLY): %s",
		 IOSAVE, strerror (errno));
	  (void) close (fd);
	  (void) close (fd2);
	  return;
	}
      (void) write (fd2, &tty, sizeof (struct termios));
      wrote_iosave = 1;
      (void) close (fd);
      (void) close (fd2);
      return;
    }
  /*
   *    Restore the terminal settings.
   */
  if (wrote_iosave && (fd2 = open (IOSAVE, O_RDONLY)) >= 0)
    {
      if (read (fd2, &tty, sizeof (tty)) == sizeof (tty))
	restore_ok = 1;
      close (fd2);
    }
  if (restore_ok == 0)
    {
      /*
       *    No old settings - create some resonable defaults.
       */
      if (tcgetattr (fd, &tty) != 0)
	{
	  ilog (L_VB, "can't get terminal attributes of %s: %s\n",
	       strerror (errno));
	  memset (&tty, 0, sizeof tty);
	  return;
	}

      tty.c_cflag = CS8 | CREAD | PARENB;

      tty.c_cc[VEOF] = 4;
      tty.c_cc[VEOL] = 13;
      tty.c_cc[VERASE] = 8;
      tty.c_cc[VINTR] = 3;
      tty.c_cc[VKILL] = 21;
      tty.c_cc[VQUIT] = 28;
      tty.c_cc[VSUSP] = 26;
      tty.c_cc[VSTART] = 17;
      tty.c_cc[VSTOP] = 19;
      tty.c_cc[VLNEXT] = 22;
      tty.c_cc[VWERASE] = 23;
      tty.c_cc[VDSUSP] = 25;
      tty.c_cc[VREPRINT] = 18;
      tty.c_cc[VFLUSHO] = 15;
    }
  /*
   *    Set pre and post processing
   */
  tty.c_iflag = BRKINT | IGNPAR | ICRNL | IXON;
  tty.c_oflag = OPOST;
  tty.c_lflag = ECHOE | ECHONL | ECHO | ICANON | ISIG | IEXTEN;

  /*
   *    Now set the terminal line.
   */
  (void) tcsetattr (fd, TCSANOW, &tty);
  (void) close (fd);
}

/*
 *    Print to the system console
 */
void
print (char *s)
{
  int fd;

  if ((fd = serial_open (console_dev, O_WRONLY | O_NOCTTY | O_NDELAY)) >= 0)
    {
      write (fd, s, strlen (s));
      close (fd);
    }
}

/*
 *    Sleep a number of seconds
 */
void
do_sleep (int sec)
{
  int old_alrm;
  int f;
  int ga;

  ga = ISMEMBER (got_signals, SIGALRM);

  /*
   *    Save old alarm time and wait 'sec' seconds.
   */
  old_alrm = alarm (0);
  for (f = 0; f < sec; f++)
    {
      DELSET (got_signals, SIGALRM);
      alarm (1);
      while (ISMEMBER (got_signals, SIGALRM) == 0)
	pause ();
    }

  /*
   *    Reset old values of got_alrm flag and the timer
   */
  if (ga)
    ADDSET (got_signals, SIGALRM);
  else
    DELSET (got_signals, SIGALRM);
  if (old_alrm)
    alarm (old_alrm - sec > 0 ? old_alrm - sec : 1);
}

/*
 *    Log something to a logfile and the console.
 */
void
ilog (int loglevel, char *s, ...)
{
  va_list va_alist;
  char buf[256];

  va_start (va_alist, s);
  vsprintf (buf, s, va_alist);
  va_end (va_alist);

  if (loglevel & L_SY)
    {
      /*
       *    Re-etablish connection with syslogd every time.
       */
      openlog ("init", 0, LOG_DAEMON);
      syslog (LOG_INFO, buf);
      /* closelog(); NOT needed with recent libc's. */
    }
  /*
   *    And log to the console.
   */
  if (loglevel & L_CO)
    {
      print ("\rINIT: ");
      print (buf);
      print ("\r\n");
    }
}


/*
 *    See if one character of s2 is in s1
 */
int
any (char *s1, char *s2)
{
  while (*s2)
    if (strchr (s1, *s2++) != NULL)
      return (1);
  return (0);
}


/*
 *    Fork and execute.
 *
 *      This function is too long and indents too deep.
 *
 */
int
spawn (CHILD * ch, int *res)
{
  char *args[16];		/* Argv array */
  char buf[136];		/* Line buffer */
  int f, st;			/* Scratch variables */
  char *ptr;			/* Ditto */
  time_t t;			/* System time */
  int oldAlarm;			/* Previous alarm value */
  char *proc = ch->process;	/* Command line */
  char i_lvl[] = "RUNLEVEL=x";	/* Runlevel in environment. */
  char i_prev[] = "PREVLEVEL=x";	/* Previous runlevel. */
  char i_cons[32];		/* console device. */
  pid_t pid, pgrp;		/* child, console process group. */
  sigset_t nmask, omask;	/* For blocking SIGCHLD */

  *res = -1;

  /* Skip '+' if it's there */
  if (proc[0] == '+')
    proc++;

  ch->flags |= XECUTED;

  if (ch->action == RESPAWN || ch->action == ONDEMAND)
    {
      /* Is the date stamp from less than 2 minutes ago? */
      time (&t);
      if (ch->tm + TESTTIME > t)
	{
	  ch->count++;
	}
      else
	{
	  ch->count = 0;
	  ch->tm = t;
	}

      /* Do we try to respawn too fast? */
      if (ch->count >= MAXSPAWN)
	{

	  ilog (L_VB, "Id \"%s\" respawning too fast: disabled for %d minutes",
	       ch->id, SLEEPTIME / 60);
	  ch->flags &= ~RUNNING;
	  ch->flags |= FAILING;

	  /* Remember the time we stopped */
	  ch->tm = t;

	  /* Try again in 5 minutes */
	  oldAlarm = alarm (0);
	  if (oldAlarm > SLEEPTIME || oldAlarm <= 0)
	    oldAlarm = SLEEPTIME;
	  alarm (oldAlarm);
	  return (-1);
	}
    }
  /* See if there is an "initscript" (except in single user mode). */
  if (access (INITSCRIPT, R_OK) == 0 && runlevel != 'S')
    {
      /* Build command line using "initscript" */
      args[1] = SHELL;
      args[2] = INITSCRIPT;
      args[3] = ch->id;
      args[4] = ch->rlevel;
      args[5] = "unknown";
      for (f = 0; actions[f].name; f++)
	{
	  if (ch->action == actions[f].act)
	    {
	      args[5] = actions[f].name;
	      break;
	    }
	}
      args[6] = proc;
      args[7] = NULL;
    }
  else if (any (proc, "~`!$^&*()=|\\{}[];\"'<>?"))
    {
      /* See if we need to fire off a shell for this command */
      /* Give command line to shell */
      args[1] = SHELL;
      args[2] = "-c";
      strcpy (buf, "exec ");
      strcat (buf, proc);
      args[3] = buf;
      args[4] = NULL;
    }
  else
    {
      /* Split up command line arguments */
      strcpy (buf, proc);
      ptr = buf;
      for (f = 1; f < 15; f++)
	{
	  /* Skip white space */
	  while (*ptr == ' ' || *ptr == '\t')
	    ptr++;
	  args[f] = ptr;

	  /* May be trailing space.. */
	  if (*ptr == 0)
	    break;

	  /* Skip this `word' */
	  while (*ptr && *ptr != ' ' && *ptr != '\t' && *ptr != '#')
	    ptr++;

	  /* If end-of-line, break */
	  if (*ptr == '#' || *ptr == 0)
	    {
	      f++;
	      *ptr = 0;
	      break;
	    }
	  /* End word with \0 and continue */
	  *ptr++ = 0;
	}
      args[f] = NULL;
    }
  args[0] = args[1];
  while (1)
    {
      /*
       *    Block sigchild while forking.
       */
      sigemptyset (&nmask);
      sigaddset (&nmask, SIGCHLD);
      sigprocmask (SIG_BLOCK, &nmask, &omask);

      if ((pid = fork ()) == 0)
	{

	  close (0);
	  close (1);
	  close (2);
	  if (pipe_fd >= 0)
	    close (pipe_fd);

	  sigprocmask (SIG_SETMASK, &omask, NULL);

	  /* Now set RUNLEVEL and PREVLEVEL */
	  sprintf (i_cons, "CONSOLE=%s", console_dev);
	  i_lvl[9] = thislevel;
	  i_prev[10] = prevlevel;
	  putenv (i_lvl);
	  putenv (i_prev);
	  putenv (i_cons);
	  putenv (E_VERSION);

	  /*
	   *    In sysinit, boot, bootwait or single user mode:
	   *      any wait-type subprocess gets the console
	   *      as its controlling tty (if not in use yet).
	   */
	  if (strchr ("*#sS", runlevel) && ch->flags & WAITING)
	    {
	      /*
	       *    We fork once extra. This is so that we can
	       *      wait and change the process group and session
	       *      of the console after exit of the leader.
	       */
	      setsid ();

	      f = serial_open (console_dev, O_RDWR | O_NOCTTY);
#ifdef TIOCSCTTY
	      (void) ioctl (f, TIOCSCTTY, 0);
#else
	      dup2 (f, -1);
	      f = dup2 (f, 0);
#endif
	      dup2 (f, 1);
	      dup2 (f, 2);

	      if ((pid = fork ()) < 0)
		{
		  ilog (L_VB, "cannot fork");
		  exit (1);
		}
	      if (pid > 0)
		{
		  /*
		   *    Ignore keyboard signals etc.
		   */
		  signal (SIGINT, SIG_IGN);
		  signal (SIGQUIT, SIG_IGN);
		  signal (SIGTSTP, SIG_IGN);
		  signal (SIGCHLD, SIG_DFL);

		  while (waitpid (pid, &st, 0) != pid);
		  pgrp = 0;
		  (void) ioctl (f, TIOCGPGRP, &pgrp);
		  if (pgrp != getpid ())
		    exit (0);
		  /*
		   *    De-attach session from controlling tty.
		   *      Or actually, the other way around.
		   *      This _without_ SIGHUP etc.
		   */
		  if ((pid = fork ()) < 0)
		    {
		      ilog (L_VB, "cannot fork");
		      exit (1);
		    }
		  if (pid == 0)
		    {
		      setsid ();
#ifdef TIOCSCTTY
		      (void) ioctl (f, TIOCSCTTY, (void *) 1);
#else
		      close (0);
		      close (1);
		      close (2);
		      dup2 (f, -1);
#endif
		      exit (0);
		    }
		  while (waitpid (pid, &st, 0) != pid);
		  exit (0);
		}
	      /* Set ioctl settings to default ones */
	      set_term (0);
	    }
	  else
	    {
	      if ((f = serial_open (console_dev, O_RDWR | O_NOCTTY)) < 0)
		{
		  ilog (L_VB, "open(%s): %s", console_dev, strerror (errno));
		  f = open ("/dev/null", O_RDWR);
		}
	      dup (f);
	      dup (f);
	      setsid ();
	    }
	  /* Reset all the signals */
	  for (f = 1; f < NSIG; f++)
	    signal (f, SIG_DFL);
	  execvp (args[1], args + 1);
	  /*
	   *    Is this a bug in execvp? It does _not_ execute shell
	   *      scripts (/etc/rc !), so we try again with
	   *      'sh -c exec ...'
	   */
	  if (errno == ENOEXEC)
	    {
	      args[1] = SHELL;
	      args[2] = "-c";
	      strcpy (buf, "exec ");
	      strcat (buf, proc);
	      args[3] = buf;
	      args[4] = NULL;
	      execvp (args[1], args + 1);
	    }
	  ilog (L_VB, "cannot execute \"%s\" (%s)", args[1], strerror (errno));
	  exit (1);
	}
      *res = pid;
      sigprocmask (SIG_SETMASK, &omask, NULL);

#if DEBUG
      ilog (L_VB, "Started id %s (pid %d)", ch->id, pid);
#endif

      if (pid == -1)
	{
	  ilog (L_VB, "cannot fork, retry..", NULL, NULL);
	  do_sleep (5);
	  continue;
	}
      return (pid);
    }
}

/*
 *    Start a child running!
 */
void
startup (CHILD * ch)
{
  /*
   *    See if it's disabled
   */
  if (ch->flags & FAILING)
    return;

  switch (ch->action)
    {

    case SYSINIT:
    case BOOTWAIT:
    case WAIT:
    case POWERWAIT:
    case POWERFAILNOW:
    case POWEROKWAIT:
    case CTRLALTDEL:
      if (!(ch->flags & XECUTED))
	ch->flags |= WAITING;
    case KBREQUEST:
    case BOOT:
    case POWERFAIL:
    case ONCE:
      if (ch->flags & XECUTED)
	break;
    case ONDEMAND:
    case RESPAWN:
      ch->flags |= RUNNING;
      if (spawn (ch, &(ch->pid)) < 0)
	break;
      /*
       *    Do NOT log if process field starts with '+'
       *      FIXME: that's for compatibility with *very*
       *      old getties - probably it can be taken out.
       */
      if (ch->process[0] != '+')
	write_utmp_wtmp ("", ch->id, ch->pid, INIT_PROCESS, "");
      break;
    }
}

/*
 *    My version of strtok(3).
 */
char *
get_part (char *str, int tok)
{
  static char *s;
  char *p, *q;

  if (str != NULL)
    s = str;
  if (s == NULL || *s == 0)
    return (NULL);
  q = p = s;
  while (*p != tok && *p)
    p++;
  if (*p == tok)
    *p++ = 0;
  s = p;

  return q;
}

/*
 *    Read the inittab file.
 */
void
read_inittab (void)
{
  FILE *fp;			/* The INITTAB file */
  char buf[256];		/* Line buffer */
  char err[64];			/* Error message. */
  char *id, *rlevel, *action, *process;	/* Fields of a line */
  char *p;
  CHILD *ch, *old, *i;		/* Pointers to CHILD structure */
  CHILD *head = NULL;		/* Head of linked list */
  int lineNo = 0;		/* Line number in INITTAB file */
  int actionNo;			/* Decoded action field */
  int f;			/* Counter */
  int round;			/* round 0 for SIGTERM, round 1 for SIGKILL */
  int foundOne = 0;		/* No killing no sleep */
  int talk;			/* Talk to the user */
  int done = 0;			/* Ready yet? */
  sigset_t nmask, omask;	/* For blocking SIGCHLD. */
#ifdef INITLVL
  struct stat st;		/* To stat INITLVL */
#endif

#if DEBUG
  if (newFamily != NULL)
    {
      ilog (L_VB, "PANIC newFamily != NULL");
      exit (1);
    }
  ilog (L_VB, "Reading inittab");
#endif

  /*
   *  Open INITTAB and real line by line.
   */
  if ((fp = fopen (INITTAB, "r")) == NULL)
    ilog (L_VB, "No inittab file found");

  while (!done)
    {
      /*
       *    Add single user shell entry at the end.
       */
      if (fp == NULL || fgets (buf, 255, fp) == NULL)
	{
	  done = 1;
	  /*
	   *    See if we have a single user entry.
	   */
	  for (old = newFamily; old; old = old->next)
	    if (strcmp (old->rlevel, "S") == 0)
	      break;
	  if (old == NULL)
	    sprintf (buf, "~~:S:wait:%s\n", SHELL);
	  else
	    continue;
	}
      lineNo++;
      /*
       *    Skip comments and empty lines
       */
      for (p = buf; *p == ' ' || *p == '\t'; p++);
      if (*p == '#' || *p == '\n')
	continue;

      /*
       *    Decode the fields
       */
      id = get_part (p, ':');
      rlevel = get_part (NULL, ':');
      action = get_part (NULL, ':');
      process = get_part (NULL, '\n');

      /*
       *    Check if syntax is OK. Be very verbose here, to
       *      avoid newbie postings on comp.os.linux.setup :)
       */
      err[0] = 0;
      if (!id || !*id)
	strcpy (err, "missing id field");
      if (!rlevel)
	strcpy (err, "missing runlevel field");
      if (!process)
	strcpy (err, "missing process field");
      if (!action || !*action)
	strcpy (err, "missing action field");
      if (id && strlen (id) > sizeof (utproto.ut_id))
	sprintf (err, "id field too long (max %d characters)",
		 (int) sizeof (utproto.ut_id));
      if (rlevel && strlen (rlevel) > 11)
	strcpy (err, "rlevel field too long (max 11 characters)");
      if (process && strlen (process) > 127)
	strcpy (err, "process field too long");
      if (action && strlen (action) > 32)
	strcpy (err, "action field too long");
      if (err[0] != 0)
	{
	  ilog (L_VB, "%s[%d]: %s", INITTAB, lineNo, err);
#if DEBUG
	  ilog (L_VB, "%s:%s:%s:%s", id, rlevel, action, process);
#endif
	  continue;
	}
      /*
       *    Decode the "action" field
       */
      actionNo = -1;
      for (f = 0; actions[f].name; f++)
	if (strcasecmp (action, actions[f].name) == 0)
	  {
	    actionNo = actions[f].act;
	    break;
	  }
      if (actionNo == -1)
	{
	  ilog (L_VB, "%s[%d]: %s: unknown action field",
	       INITTAB, lineNo, action);
	  continue;
	}
      /*
       *    See if the id field is unique
       */
      for (old = newFamily; old; old = old->next)
	{
	  if (strcmp (old->id, id) == 0 && strcmp (id, "~~"))
	    {
	      ilog (L_VB, "%s[%d]: duplicate ID field \"%s\"",
		   INITTAB, lineNo, id);
	      break;
	    }
	}
      if (old)
	continue;

      /*
       *    Allocate a CHILD structure
       */
      if ((ch = malloc (sizeof (CHILD))) == NULL)
	{
	  ilog (L_VB, "out of memory");
	  continue;
	}
      memset (ch, 0, sizeof (CHILD));

      /*
       *    And fill it in.
       */
      ch->action = actionNo;
      strncpy (ch->id, id, sizeof (utproto.ut_id) + 1);	/* Hack for different libs. */
      strncpy (ch->process, process, 127);
      if (rlevel[0])
	{
	  for (f = 0; f < 11 && rlevel[f]; f++)
	    {
	      ch->rlevel[f] = rlevel[f];
	      if (ch->rlevel[f] == 's')
		ch->rlevel[f] = 'S';
	    }
	  strncpy (ch->rlevel, rlevel, 11);
	}
      else
	{
	  strcpy (ch->rlevel, "0123456789");
	  if (ISPOWER (ch->action))
	    strcpy (ch->rlevel, "S0123456789");
	}
      /*
       *    We have the fake runlevel '#' for SYSINIT  and
       *      '*' for BOOT and BOOTWAIT.
       */
      if (ch->action == SYSINIT)
	strcpy (ch->rlevel, "#");
      if (ch->action == BOOT || ch->action == BOOTWAIT)
	strcpy (ch->rlevel, "*");

      /*
       *    Now add it to the linked list. Special for powerfail.
       */
      if (ISPOWER (ch->action))
	{

	  /*
	   *    Disable by default
	   */
	  ch->flags |= XECUTED;

	  /*
	   *    Tricky: insert at the front of the list..
	   */
	  old = NULL;
	  for (i = newFamily; i; i = i->next)
	    {
	      if (!ISPOWER (i->action))
		break;
	      old = i;
	    }
	  /*
	   *    Now add after entry "old"
	   */
	  if (old)
	    {
	      ch->next = i;
	      old->next = ch;
	      if (i == NULL)
		head = ch;
	    }
	  else
	    {
	      ch->next = newFamily;
	      newFamily = ch;
	      if (ch->next == NULL)
		head = ch;
	    }
	}
      else
	{
	  /*
	   *    Just add at end of the list
	   */
	  if (ch->action == KBREQUEST)
	    ch->flags |= XECUTED;
	  ch->next = NULL;
	  if (head)
	    head->next = ch;
	  else
	    newFamily = ch;
	  head = ch;
	}

      /*
       *    Walk through the old list comparing id fields
       */
      for (old = family; old; old = old->next)
	if (strcmp (old->id, ch->id) == 0)
	  {
	    old->new = ch;
	    break;
	  }
    }
  /*
   *  We're done.
   */
  if (fp)
    fclose (fp);

  /*
   *  Loop through the list of children, and see if they need to
   *    be killed. 
   */

#if DEBUG
  ilog (L_VB, "Checking for children to kill");
#endif
  for (round = 0; round < 2; round++)
    {
      talk = 1;
      for (ch = family; ch; ch = ch->next)
	{
	  ch->flags &= ~KILLME;

	  /*
	   *    Is this line deleted?
	   */
	  if (ch->new == NULL)
	    ch->flags |= KILLME;

	  /*
	   *    If the entry has changed, kill it anyway. Note that
	   *      we do not check ch->process, only the "action" field.
	   *      This way, you can turn an entry "off" immidiately, but
	   *      changes in the command line will only become effective
	   *      after the running version has exited.
	   */
	  if (ch->new && ch->action != ch->new->action)
	    ch->flags |= KILLME;

	  /*
	   *    Only BOOT processes may live in all levels
	   */
	  if (ch->action != BOOT && strchr (ch->rlevel, runlevel) == NULL)
	    {
	      /*
	       *    Ondemand procedures live always,
	       *      except in single user
	       */
	      if (runlevel == 'S' || !(ch->flags & DEMAND))
		ch->flags |= KILLME;
	    }
	  /*
	   *    Now, if this process may live note so in the new list
	   */
	  if ((ch->flags & KILLME) == 0)
	    {
	      ch->new->flags = ch->flags;
	      ch->new->pid = ch->pid;
	      ch->new->exstat = ch->exstat;
	      continue;
	    }
	  /*
	   *    Is this process still around?
	   */
	  if ((ch->flags & RUNNING) == 0)
	    {
	      ch->flags &= ~KILLME;
	      continue;
	    }
#if DEBUG
	  ilog (L_VB, "Killing \"%s\"", ch->process);
#endif

	  switch (round)
	    {
	    case 0:		/* Send TERM signal */
	      if (talk)
		ilog (L_CO, "Sending processes the TERM signal");
	      kill (-(ch->pid), SIGTERM);
	      foundOne = 1;
	      break;
	    case 1:		/* Send KILL signal and collect status */
	      if (talk)
		ilog (L_CO, "Sending processes the KILL signal");
	      kill (-(ch->pid), SIGKILL);
	      break;
	    }
	  talk = 0;

	}
      /*
       *        See if we have to wait 5 seconds
       */
      if (foundOne && round == 0)
	{
	  /*
	   *    Yup, but check every second if we still have children.
	   */
	  for (f = 0; f < sltime; f++)
	    {
	      for (ch = family; ch; ch = ch->next)
		{
		  if (!(ch->flags & KILLME))
		    continue;
		  if ((ch->flags & RUNNING) && !(ch->flags & ZOMBIE))
		    break;
		}
	      if (ch == NULL)
		{
		  /*
		   *    No running children, skip SIGKILL
		   */
		  round = 1;
		  foundOne = 0;	/* Skip the sleep below. */
		  break;
		}
	      do_sleep (1);
	    }
	}
    }

  /*
   *  Now give all processes the chance to die and collect exit statuses.
   */
  if (foundOne)
    do_sleep (1);
  for (ch = family; ch; ch = ch->next)
    if (ch->flags & KILLME)
      {
	if (!(ch->flags & ZOMBIE))
	  ilog (L_CO, "Pid %d [id %s] seems to hang", ch->pid, ch->id);
	else
	  {
#if DEBUG
	    ilog (L_VB, "Updating utmp for pid %d [id %s]", ch->pid, ch->id);
#endif
	    ch->flags &= ~RUNNING;
	    if (ch->process[0] != '+')
	      write_utmp_wtmp ("", ch->id, ch->pid, DEAD_PROCESS, NULL);
	  }
      }
  /*
   *  Both rounds done; clean up the list.
   */
  sigemptyset (&nmask);
  sigaddset (&nmask, SIGCHLD);
  sigprocmask (SIG_BLOCK, &nmask, &omask);
  for (ch = family; ch; ch = old)
    {
      old = ch->next;
      free (ch);
    }
  family = newFamily;
  for (ch = family; ch; ch = ch->next)
    ch->new = NULL;
  newFamily = NULL;
  sigprocmask (SIG_SETMASK, &omask, NULL);

#ifdef INITLVL
  /*
   *  Dispose of INITLVL file.
   */
  if (lstat (INITLVL, &st) >= 0 && S_ISLNK (st.st_mode))
    {
      /*
       *    INITLVL is a symbolic link, so just truncate the file.
       */
      close (open (INITLVL, O_WRONLY | O_TRUNC));
    }
  else
    {
      /*
       *    Delete INITLVL file.
       */
      unlink (INITLVL);
    }
#endif
#ifdef INITLVL2
  /*
   *  Dispose of INITLVL2 file.
   */
  if (lstat (INITLVL2, &st) >= 0 && S_ISLNK (st.st_mode))
    {
      /*
       *    INITLVL2 is a symbolic link, so just truncate the file.
       */
      close (open (INITLVL2, O_WRONLY | O_TRUNC));
    }
  else
    {
      /*
       *    Delete INITLVL2 file.
       */
      unlink (INITLVL2);
    }
#endif
}

/*
 *    Walk through the family list and start up children.
 *      The entries that do not belong here at all are removed
 *      from the list.
 */
void
start_if_needed (void)
{
  CHILD *ch;			/* Pointer to child */
  int delete;			/* Delete this entry from list? */

#if DEBUG
  ilog (L_VB, "Checking for children to start");
#endif
  for (ch = family; ch; ch = ch->next)
    {

#if DEBUG
      if (ch->rlevel[0] == 'C')
	{
	  ilog (L_VB, "%s: flags %d", ch->process, ch->flags);
	}
#endif

      /* Are we waiting for this process? Then quit here. */
      if (ch->flags & WAITING)
	break;

      /* Already running? OK, don't touch it */
      if (ch->flags & RUNNING)
	continue;

      /* See if we have to start it up */
      delete = 1;
      if (strchr (ch->rlevel, runlevel) ||
	  ((ch->flags & DEMAND) && !strchr ("#*Ss", runlevel)))
	{
	  startup (ch);
	  delete = 0;
	}
      if (delete)
	{
	  /* FIXME: is this OK? */
	  ch->flags &= ~(RUNNING | WAITING);
	  if (!ISPOWER (ch->action) && ch->action != KBREQUEST)
	    ch->flags &= ~XECUTED;
	  ch->pid = 0;
	}
      else
	/* Do we have to wait for this process? */
      if (ch->flags & WAITING)
	break;
    }
  /* Done. */
}

/*
 *    Ask the user on the console for a runlevel
 */
int
ask_runlevel ()
{
  int lvl = -1;
  char buf[8];
  int fd;

  set_term (0);
  fd = serial_open (console_dev, O_RDWR | O_NOCTTY);

  if (fd < 0)
    return ('S');

  while (!strchr ("0123456789S", lvl))
    {
      write (fd, "\nEnter runlevel: ", 17);
      buf[0] = 0;
      read (fd, buf, 8);
      if (buf[0] != 0 && (buf[1] == '\r' || buf[1] == '\n'))
	lvl = buf[0];
      if (islower (lvl))
	lvl = toupper (lvl);
    }
  close (fd);
  return lvl;
}

/*
 *    Search the INITTAB file for the 'initdefault' field, with the default
 *      runlevel. If this fails, ask the user to supply a runlevel.
 */
int
get_init_default (void)
{
  CHILD *ch;
  int lvl = -1;
  char *p;

  /*
   *    Look for initdefault.
   */
  for (ch = family; ch; ch = ch->next)
    if (ch->action == INITDEFAULT)
      {
	p = ch->rlevel;
	while (*p)
	  {
	    if (*p > lvl)
	      lvl = *p;
	    p++;
	  }
	break;
      }
  /*
   *    See if level is valid
   */
  if (lvl > 0)
    {
      if (islower (lvl))
	lvl = toupper (lvl);
      if (strchr ("0123456789S", lvl) == NULL)
	{
	  ilog (L_VB, "Initdefault level '%c' is invalid", lvl);
	  lvl = 0;
	}
    }
  /*
   *    Ask for runlevel on console if needed.
   */
  if (lvl <= 0)
    lvl = ask_runlevel ();

  /*
   *    Log the fact that we have a runlevel now.
   */
  return lvl;
}


/*
 *    We got signaled.
 *
 *      Do actions for the new level. If we are compatible with
 *      the "old" INITLVL and arg == 0, try to read the new
 *      runlevel from that file first.
 */
int
read_level (int arg)
{
  unsigned char foo = 'X';	/* Contents of INITLVL */
  int st;			/* Sleep time */
  CHILD *ch;			/* Walk through list */
  FILE *fp;			/* File pointer for INITLVL */
  int ok = 1;
  struct stat stt;

  if (arg)
    foo = arg;

#ifdef INITLVL
  ok = 0;

  if (arg == 0)
    {
      fp = NULL;
      if (stat (INITLVL, &stt) != 0 || stt.st_size != 0L)
	fp = fopen (INITLVL, "r");
#ifdef INITLVL2
      if (fp == NULL && (stat (INITLVL2, &stt) != 0 || stt.st_size != 0L))
	fp = fopen (INITLVL2, "r");
#endif
      if (fp == NULL)
	{
	  /* INITLVL file is empty or not there - act as 'init q' */
	  ilog (L_SY, "Re-reading inittab");
	  return (runlevel);
	}
      ok = fscanf (fp, "%c %d", &foo, &st);
      fclose (fp);
    }
  else
    {
      /* We go to the new runlevel passed as an argument. */
      foo = arg;
      ok = 1;
    }
  if (ok == 2)
    sltime = st;

#endif /* INITLVL */

  if (islower (foo))
    foo = toupper (foo);
  if (ok < 1 || ok > 2 || strchr ("QS0123456789ABCU", foo) == NULL)
    {
      ilog (L_VB, "bad runlevel: %c", foo);
      return (runlevel);
    }
  /* Be verbose 'bout it all */
  switch (foo)
    {
    case 'S':
      ilog (L_VB, "Going single user");
      break;
    case 'Q':
      ilog (L_SY, "Re-reading inittab");
      break;
    case 'A':
    case 'B':
    case 'C':
      ilog (L_SY, "Activating demand-procedures for '%c'", foo);
      break;
    case 'U':
      ilog (L_SY, "Trying to re-exec init");
      return 'U';
    default:
      ilog (L_VB, "Switching to runlevel: %c", foo);
    }

  if (foo == 'Q')
    return (runlevel);

  /* Check if this is a runlevel a, b or c */
  if (strchr ("ABC", foo))
    {
      if (runlevel == 'S')
	return (runlevel);

      /* Read inittab again first! */
      read_inittab ();

      /* Mark those special tasks */
      for (ch = family; ch; ch = ch->next)
	if (strchr (ch->rlevel, foo) != NULL ||
	    strchr (ch->rlevel, tolower (foo)) != NULL)
	  {
	    ch->flags |= DEMAND;
	    ch->flags &= ~XECUTED;
#if DEBUG
	    ilog (L_VB, "Marking (%s) as ondemand, flags %d",
		 ch->id, ch->flags);
#endif
	  }
      return (runlevel);
    }
  /* Store both the old and the new runlevel. */
  write_utmp_wtmp ("runlevel", "~~", foo + 256 * runlevel, RUN_LVL, "~");
  thislevel = foo;
  prevlevel = runlevel;
  return (foo);
}


/*
 *    This procedure is called after every signal (SIGHUP, SIGALRM..)
 *
 *      Only clear the 'failing' flag if the process is sleeping
 *      longer than 5 minutes, or inittab was read again due
 *      to user interaction.
 */
void
fail_check (void)
{
  time_t t;			/* System time */
  CHILD *ch;			/* Pointer to child structure */
  time_t next_alarm = 0;	/* When to set next alarm */

  time (&t);

  for (ch = family; ch; ch = ch->next)
    {

      if (ch->flags & FAILING)
	{
	  /* Can we free this sucker? */
	  if (ch->tm + SLEEPTIME < t)
	    {
	      ch->flags &= ~FAILING;
	      ch->count = 0;
	      ch->tm = 0;
	    }
	  else
	    {
	      /* No, we'll look again later */
	      if (next_alarm == 0 || ch->tm + SLEEPTIME > next_alarm)
		next_alarm = ch->tm + SLEEPTIME;
	    }
	}
    }
  if (next_alarm)
    {
      next_alarm -= t;
      if (next_alarm < 1)
	next_alarm = 1;
      alarm (next_alarm);
    }
}

/* Set all 'Fail' timers to 0 */
void
fail_cancel (void)
{
  CHILD *ch;

  for (ch = family; ch; ch = ch->next)
    {
      ch->count = 0;
      ch->tm = 0;
      ch->flags &= ~FAILING;
    }
}

/*
 *    Start up powerfail entries.
 */
void
do_power_fail (int pwrstat)
{
  CHILD *ch;

  /*
   *    Tell powerwait & powerfail entries to start up
   */
  for (ch = family; ch; ch = ch->next)
    {
      if (pwrstat == 'O')
	{
	  /*
	   *    The power is OK again.
	   */
	  if (ch->action == POWEROKWAIT)
	    ch->flags &= ~XECUTED;
	}
      else if (pwrstat == 'L')
	{
	  /*
	   *    Low battery, shut down now.
	   */
	  if (ch->action == POWERFAILNOW)
	    ch->flags &= ~XECUTED;
	}
      else
	{
	  /*
	   *    Power is failing, shutdown imminent
	   */
	  if (ch->action == POWERFAIL || ch->action == POWERWAIT)
	    ch->flags &= ~XECUTED;
	}
    }
}

/*
 *    Check for state-pipe presence
 */
int
check_pipe (int fd)
{
  struct timeval t;
  fd_set s;
  char signature[8];

  FD_ZERO (&s);
  FD_SET (fd, &s);
  t.tv_sec = t.tv_usec = 0;

  if (select (fd + 1, &s, NULL, NULL, &t) != 1)
    return 0;
  if (read (fd, signature, 8) != 8)
    return 0;
  return strcmp (Signature, signature) == 0;
}

/*
 *     Make a state-pipe.
 */
int
make_pipe (int fd)
{
  int fds[2];

  pipe (fds);
  dup2 (fds[0], fd);
  close (fds[0]);
  fcntl (fds[1], F_SETFD, 1);
  fcntl (fd, F_SETFD, 0);
  write (fds[1], Signature, 8);

  return fds[1];
}

/*
 *    Attempt to re-exec.
 */
void
re_exec (void)
{
  sigset_t mask, oldset;
  pid_t pid;
  int fd;
  CHILD *ch;

  if (strchr ("S12345", runlevel) == NULL)
    return;
  closelog ();			/* AAAARRRGH! It _is_ needed */

  /*
   *    Reset the alarm, and block all signals.
   */
  alarm (0);
  sigfillset (&mask);
  sigprocmask (SIG_BLOCK, &mask, &oldset);

  /*
   *    construct a pipe fd --> STATE_PIPE and write a signature
   */
  fd = make_pipe (STATE_PIPE);

  /* 
   * It's a backup day today, so I'm pissed off.  Being a BOFH, however, 
   * does have it's advantages...
   */
  fail_cancel ();
  close (pipe_fd);
  pipe_fd = -1;
  DELSET (got_signals, SIGCHLD);
  DELSET (got_signals, SIGHUP);
  DELSET (got_signals, SIGUSR1);

  /*
   *    That should be cleaned.
   */
  for (ch = family; ch; ch = ch->next)
    if (ch->flags & ZOMBIE)
      {
#if DEBUG
	log (L_VB, "Child died, PID= %d", ch->pid);
#endif
	ch->flags &= ~(RUNNING | ZOMBIE | WAITING);
	if (ch->process[0] != '+')
	  write_utmp_wtmp ("", ch->id, ch->pid, DEAD_PROCESS, NULL);
      }
  if ((pid = fork ()) > 0)
    {
      /*
       *    Yup. _Parent_ exec's ...
       */
      execl (myname, myname, NULL);
    }
  else if (pid == 0)
    {
      /*
       *    ... while child sends her the
       *      state information and dies
       */
      send_state (fd);
      exit (0);
    }
  /*
   *    We shouldn't be here, something failed. 
   *      Bitch, close the state pipe, unblock signals and return.
   */
  close (fd);
  close (STATE_PIPE);
  sigprocmask (SIG_SETMASK, &oldset, NULL);
  ilog (L_CO, "Attempt to re-exec failed");
}


/*
 *    We got a change runlevel request through the
 *      init.fifo. Process it.
 */
void
fifo_new_level (int level)
{
  int oldlevel;
#if CHANGE_WAIT
  CHILD *ch;
#endif

  if (level == runlevel)
    return;

#if CHANGE_WAIT
  /* Are we waiting for a child? */
  for (ch = family; ch; ch = ch->next)
    if (ch->flags & WAITING)
      break;
  if (ch == NULL)
#endif
    {
      /* We need to go into a new runlevel */
      oldlevel = runlevel;
      runlevel = read_level (level);
      if (runlevel == 'U')
	{
	  runlevel = oldlevel;
	  re_exec ();
	}
      else
	{
	  if (oldlevel != 'S' && runlevel == 'S')
	    set_term (0);
	  if (runlevel == '6' || runlevel == '0' || runlevel == '1')
	    set_term (0);
	  read_inittab ();
	  fail_cancel ();
	  setproctitle ("init [%c]", runlevel);
	}
    }
}

/*
 *    Read from the init FIFO. Processes like telnetd and rlogind can
 *      ask us to create login processes on their behalf.
 *
 *      FIXME: this needs to be finished. NOT that it is buggy, but we need
 *             to add the telnetd/rlogind stuff so people can start using it.
 */
void
check_init_fifo (void)
{
  struct init_request request;
  int n;
  fd_set fds;
  int quit = 0;

  /* Try to open /dev/initctl */
  if (pipe_fd < 0)
    {
      if ((pipe_fd = open (INIT_FIFO, O_RDWR | O_NONBLOCK)) >= 0)
	{
	  /* Don't use fd's 0, 1 or 2. */
	  (void) dup2 (pipe_fd, PIPE_FD);
	  close (pipe_fd);
	  pipe_fd = PIPE_FD;
	}
    }
  /* Wait for data to appear, _if_ the pipe was opened. */
  if (pipe_fd >= 0)
    while (!quit)
      {
	/* Problem: Linux will return with -EINTR from select
	   if a signal was caught during select.  This makes
	   it possible to give an infinite timeout.  Under MiNT
	   we cannot select and wait for a signal at once.  

	   As a kludge, every signal handler writes into our 
	   fifo a dummy request so that select will return 
	   after the signal was handled.  */

	/* Do select, return on EINTR. */
	FD_ZERO (&fds);
	FD_SET (pipe_fd, &fds);
	n = select (pipe_fd + 1, &fds, NULL, NULL, NULL);
	if (n < 0)
	  {
	    if (errno == EINTR)
	      return;
	    continue;
	  }
	else
	  {
	    continue;		/* Timeout.  */
	  }

	/* Read the data, return on EINTR. */
	n = read (pipe_fd, &request, sizeof (request));
	if (n == 0)
	  {
	    /*
	     *    End of file. This can't happen under Linux (because
	     *      the pipe is opened O_RDWR - see select() in the
	     *      kernel) but you never know...
	     */
	    close (pipe_fd);
	    pipe_fd = -1;
	    return;
	  }
	if (n <= 0)
	  {
	    if (errno == EINTR)
	      return;
	    ilog (L_VB, "error reading initrequest");
	    continue;
	  }
	/* Process request. */
	if (request.magic != INIT_MAGIC || n != sizeof (request))
	  {
	    ilog (L_VB, "got bogus initrequest");
	    continue;
	  }
	switch (request.cmd)
	  {
	  case INIT_CMD_DUMMY:
	    return;		/* FIXME: Better continue instead?  */
	  case INIT_CMD_RUNLVL:
	    sltime = request.sleeptime;
	    fifo_new_level (request.runlevel);
	    quit = 1;
	    break;
	  case INIT_CMD_POWERFAIL:
	    sltime = request.sleeptime;
	    do_power_fail ('F');
	    quit = 1;
	    break;
	  case INIT_CMD_POWERFAILNOW:
	    sltime = request.sleeptime;
	    do_power_fail ('L');
	    quit = 1;
	    break;
	  case INIT_CMD_POWEROK:
	    sltime = request.sleeptime;
	    do_power_fail ('O');
	    quit = 1;
	    break;
	  default:
	    ilog (L_VB, "got unimplemented initrequest.");
	    break;
	  }
      }
  /* We come here if the pipe couldn't be opened. */
  if (pipe_fd < 0)
    pause ();

}


/*
 *    This function is used in the transition
 *      sysinit (-> single user) boot -> multi-user.
 */
void
boot_transitions ()
{
  CHILD *ch;
  static int newlevel = 0;
  int loglevel;
  int oldlevel;
  static int warn = 1;

  /* Check if there is something to wait for! */
  for (ch = family; ch; ch = ch->next)
    if ((ch->flags & RUNNING) && ch->action != BOOT)
      break;

  if (ch == NULL)
    {
      /* No processes left in this level, proceed to next level. */
      loglevel = -1;
      oldlevel = 'N';
      switch (runlevel)
	{
	case '#':		/* SYSINIT -> BOOT */
#if DEBUG
	  ilog (L_VB, "SYSINIT -> BOOT");
#endif
	  /* Save tty modes. */
	  set_term (1);

	  /* Write a boot record. */
	  wrote_reboot = 0;
	  write_wtmp ("reboot", "~~", 0, BOOT_TIME, "~");

	  /* Get our run level */
	  newlevel = dfl_level ? dfl_level : get_init_default ();
	  if (newlevel == 'S')
	    {
	      runlevel = newlevel;
	      /* Not really 'S' but show anyway. */
	      setproctitle ("init [S]");
	    }
	  else
	    runlevel = '*';
	  break;
	case '*':		/* BOOT -> NORMAL */
#if DEBUG
	  ilog (L_VB, "BOOT -> NORMAL");
#endif
	  /* Save tty modes. */
	  set_term (1);

	  if (runlevel != newlevel)
	    loglevel = newlevel;
	  runlevel = newlevel;
	  did_boot = 1;
	  warn = 1;
	  break;
	case 'S':		/* Ended SU mode */
	case 's':
#if DEBUG
	  ilog (L_VB, "END SU MODE");
#endif
	  /* Save tty modes. */
	  set_term (1);

	  newlevel = get_init_default ();
	  if (!did_boot && newlevel != 'S')
	    runlevel = '*';
	  else
	    {
	      if (runlevel != newlevel)
		loglevel = newlevel;
	      runlevel = newlevel;
	      oldlevel = 'S';
	    }
	  warn = 1;
	  for (ch = family; ch; ch = ch->next)
	    if (strcmp (ch->rlevel, "S") == 0)
	      ch->flags &= ~(FAILING | WAITING | XECUTED);
	  break;
	default:
	  if (warn)
	    ilog (L_VB, "no more processes left in this runlevel");
	  warn = 0;
	  loglevel = -1;
	  if (got_signals == 0)
	    check_init_fifo ();
	  break;
	}
      if (loglevel > 0)
	{
	  ilog (L_VB, "Entering runlevel: %c", runlevel);
	  write_utmp_wtmp ("runlevel", "~~", runlevel + 256 * oldlevel,
			   RUN_LVL, "~");
	  thislevel = runlevel;
	  prevlevel = oldlevel;
	  setproctitle ("init [%c]", runlevel);
	}
    }
}

/*
 *    Init got hit by a signal. See which signal it is,
 *      and act accordingly.
 */
void
process_signals ()
{
  int pwrstat;
  int oldlevel;
  int fd;
  CHILD *ch;
  char c;

#if defined(SIGPWR)
  if (ISMEMBER (got_signals, SIGPWR))
    {
#if DEBUG
      ilog (L_VB, "got SIGPWR");
#endif
      /* See _what_ kind of SIGPWR this is. */
      pwrstat = 0;
      if ((fd = open (PWRSTAT, O_RDONLY)) >= 0)
	{
	  c = 0;
	  read (fd, &c, 1);
	  pwrstat = c;
	  close (fd);
	  unlink (PWRSTAT);
	  do_power_fail (pwrstat);
	}
      DELSET (got_signals, SIGPWR);
    }
#endif /* def SIGPWR */

  if (ISMEMBER (got_signals, SIGINT))
    {
#if DEBUG
      ilog (L_VB, "got SIGINT");
#endif
      /* Tell ctrlaltdel entry to start up */
      for (ch = family; ch; ch = ch->next)
	if (ch->action == CTRLALTDEL)
	  ch->flags &= ~XECUTED;
      DELSET (got_signals, SIGINT);
    }
  if (ISMEMBER (got_signals, SIGWINCH))
    {
#if DEBUG
      ilog (L_VB, "got SIGWINCH");
#endif
      /* Tell kbrequest entry to start up */
      for (ch = family; ch; ch = ch->next)
	if (ch->action == KBREQUEST)
	  ch->flags &= ~XECUTED;
      DELSET (got_signals, SIGWINCH);
    }
  if (ISMEMBER (got_signals, SIGALRM))
    {
#if DEBUG
      ilog (L_VB, "got SIGALRM");
#endif
      /* The timer went off: check it out */
      DELSET (got_signals, SIGALRM);
    }
  if (ISMEMBER (got_signals, SIGCHLD))
    {
#if DEBUG
      ilog (L_VB, "got SIGCHLD");
#endif
      /* First set flag to 0 */
      DELSET (got_signals, SIGCHLD);

      /* See which child this was */
      for (ch = family; ch; ch = ch->next)
	if (ch->flags & ZOMBIE)
	  {
#if DEBUG
	    ilog (L_VB, "Child died, PID= %d", ch->pid);
#endif
	    ch->flags &= ~(RUNNING | ZOMBIE | WAITING);
	    if (ch->process[0] != '+')
	      write_utmp_wtmp ("", ch->id, ch->pid, DEAD_PROCESS, NULL);
	  }
    }
  if (ISMEMBER (got_signals, SIGHUP))
    {
#if DEBUG
      ilog (L_VB, "got SIGHUP");
#endif
#if CHANGE_WAIT
      /* Are we waiting for a child? */
      for (ch = family; ch; ch = ch->next)
	if (ch->flags & WAITING)
	  break;
      if (ch == NULL)
#endif
	{
	  /* We need to go into a new runlevel */
	  oldlevel = runlevel;
#ifdef INITLVL
	  runlevel = read_level (0);
#endif
	  if (runlevel == 'U')
	    {
	      runlevel = oldlevel;
	      re_exec ();
	    }
	  else
	    {
	      if (oldlevel != 'S' && runlevel == 'S')
		set_term (0);
	      if (runlevel == '6' || runlevel == '0' || runlevel == '1')
		set_term (0);
	      read_inittab ();
	      fail_cancel ();
	      setproctitle ("init [%c]", runlevel);
	      DELSET (got_signals, SIGHUP);
	    }
	}
    }
  if (ISMEMBER (got_signals, SIGUSR1))
    {
      /*
       *    SIGUSR1 means close and reopen /dev/initctl
       */
#if DEBUG
      ilog (L_VB, "got SIGHUP");
#endif
      close (pipe_fd);
      pipe_fd = -1;
      DELSET (got_signals, SIGUSR1);
    }
}

/*
 *    The main loop
 */
int
init_main ()
{
  int f, st;
  CHILD *ch;
  sigset_t sgt;
  struct sigaction sa;
  char *s;

  if (!reload)
    {

#if INITDEBUG
      /*
       * Fork so we can debug the init process.
       */
      if ((f = fork ()) > 0)
	{
	  while (wait (&st) != f);
	  write (1, "PRNT: init killed.\r\n", 20);
	  while (1)
	    pause ();
	}
#endif

      /*
       *    Tell the kernel to send us SIGINT when CTRL-ALT-DEL
       *      is pressed, and that we want to handle keyboard signals.
       */
      init_reboot (BMAGIC_SOFT);
#ifndef __MINT__
      if ((f = open (VT_MASTER, O_RDWR | O_NOCTTY)) >= 0)
	{
	  (void) ioctl (f, KDSIGACCEPT, SIGWINCH);
	  close (f);
	}
      else
	(void) ioctl (0, KDSIGACCEPT, SIGWINCH);
#endif

      /*
       *    Ignore all signals.
       */
      for (f = 1; f <= NSIG; f++)
	signal (f, SIG_IGN);
    }
  SETSIG (sa, SIGALRM, signal_handler);
  SETSIG (sa, SIGHUP, signal_handler);
  SETSIG (sa, SIGINT, signal_handler);
  SETSIG (sa, SIGCHLD, chld_handler);
#if defined(SIGPWR)
  SETSIG (sa, SIGPWR, signal_handler);
#endif /* def SIGPWR */
  SETSIG (sa, SIGWINCH, signal_handler);
  SETSIG (sa, SIGUSR1, signal_handler);
  SETSIG (sa, SIGSTOP, stop_handler);
  SETSIG (sa, SIGTSTP, stop_handler);
  SETSIG (sa, SIGCONT, cont_handler);
  SETSIG (sa, SIGSEGV, segv_handler);

  /*
   *  NOTE: instead of calling siginterrupt, we could just as
   *    well install the signal handler without SA_RESTART in the
   *    first place.
   */
  siginterrupt (SIGALRM, 1);
  siginterrupt (SIGHUP, 1);
  siginterrupt (SIGINT, 1);
#if defined(SIGPWR)
  siginterrupt (SIGPWR, 1);
#endif /* def SIGPWR */
  siginterrupt (SIGUSR1, 1);
  siginterrupt (SIGWINCH, 1);

  if (!reload)
    {

      /* Close whatever files are open, and reset the console. */
      close (0);
      close (1);
      close (2);
      if ((s = getenv ("CONSOLE")) != NULL)
	console_dev = s;
      set_term (0);
      setsid ();

      /*
       *    Set default PATH variable (for ksh)
       */
      if (getenv ("PATH") == NULL)
	putenv (PATH_DFL);

      /*
       *    Initialize /var/run/utmp (only works if /var is on
       *      root and mounted rw)
       */
      (void) close (open (UTMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644));

      /*
       *    Say hello to the world
       */
      ilog (L_CO, bootmsg);

      /*
       *    See if we have to start an emergency shell.
       */
      if (emerg_shell)
	{
	  SETSIG (sa, SIGCHLD, SIG_DFL);
	  if (spawn (&ch_emerg, &f) > 0)
	    {
	      while (wait (&st) != f);
	    }
	  SETSIG (sa, SIGCHLD, chld_handler);
	}
      /*
       *    Start normal boot procedure.
       */
      runlevel = '#';
      read_inittab ();

    }
  else
    {
      /*
       *    Restart: unblock signals and let the show go on
       */
      ilog (L_CO, bootmsg);
      sigfillset (&sgt);
      sigprocmask (SIG_UNBLOCK, &sgt, NULL);
    }
  start_if_needed ();

  while (1)
    {

      /* See if we need to make the boot transitions. */
      boot_transitions ();
#if DEBUG
      ilog (L_VB, "init_main: waiting..");
#endif

      /* Check if there are processes to be waited on. */
      for (ch = family; ch; ch = ch->next)
	if ((ch->flags & RUNNING) && ch->action != BOOT)
	  break;

#if CHANGE_WAIT
      /* Wait until we get hit by some signal. */
      while (ch != NULL && got_signals == 0)
	{
	  if (ISMEMBER (got_signals, SIGHUP))
	    {
	      /* See if there are processes to be waited on. */
	      for (ch = family; ch; ch = ch->next)
		if (ch->flags & WAITING)
		  break;
	    }
	  if (ch != NULL)
	    check_init_fifo ();
	}
#else /* CHANGE_WAIT */
      if (ch != NULL && got_signals == 0)
	check_init_fifo ();
#endif /* CHANGE_WAIT */

      /* Check the 'failing' flags */
      fail_check ();

      /* Process any signals. */
      process_signals ();

      /* See what we need to start up (again) */
      start_if_needed ();
    }
  /*NOTREACHED */
}

/*
 * Tell the user about the syntax we expect.
 */
void
Usage (char *s)
{
  fprintf (stderr, "Usage: %s 0123456SsQqAaBbCcUu\n", s);
  exit (1);
}


void
create_pipe ()
{
  (void) symlink (INIT_FIFO, DEV_INITCTL);
}

/*
 * Main entry for init and telinit.
 */
int
main (int argc, char *argv[])
{
  struct init_request request;
  char *p;
  FILE *fp;
  int f, fd;

  /* Get my own name */
  if ((p = strrchr (argv[0], '/')) != NULL)
    p++;
  else
    p = argv[0];


  /*
   *  Is this telinit or init ?
   */
#ifndef TEST
  if (getpid () == INITPID || !strcmp (p, "init.new") || !strcmp (p, "sh"))
#endif
    {
      /*
       *    Check for re-exec
       */
      if (check_pipe (STATE_PIPE))
	{

	  receive_state (STATE_PIPE);

	  myname = strdup (argv[0]);
	  argv0 = argv[0];
	  maxproclen = strlen (argv[0]);
	  reload = 1;
	  wrote_iosave = 1;

	  init_main ();
	}
      /* Check command line arguments */
      maxproclen = strlen (argv[0]) + 1;
      for (f = 1; f < argc; f++)
	{
	  if (!strcmp (argv[f], "single") || !strcmp (argv[f], "-s"))
	    dfl_level = 'S';
	  else if (!strcmp (argv[f], "-a") || !strcmp (argv[f], "auto"))
	    putenv ("AUTOBOOT=YES");
	  else if (!strcmp (argv[f], "-b") || !strcmp (argv[f], "emergency"))
	    emerg_shell = 1;
	  else if (strchr ("0123456789sS", argv[f][0])
		   && strlen (argv[f]) == 1)
	    dfl_level = argv[f][0];
	  /* "init u" in the very beginning makes no sence */
	  if (dfl_level == 's')
	    dfl_level = 'S';
	  maxproclen += strlen (argv[f]) + 1;
	}
      maxproclen--;

      /* Start booting. */
      argv0 = argv[0];
      argv[1] = NULL;
      setproctitle ("init boot");
      init_main (dfl_level);
    }
  /* Nope, this is a change-run-level init */
  while ((f = getopt (argc, argv, "t:")) != EOF)
    {
      switch (f)
	{
	case 't':
	  sltime = atoi (optarg);
	  break;
	default:
	  Usage (p);
	  break;
	}
    }

  if (geteuid () != 0)
    {
      fprintf (stderr, "init: must be superuser.\n");
      exit (1);
    }
  /* Check syntax. */
  if (argc - optind != 1 || strlen (argv[optind]) != 1)
    Usage (p);
  if (!strchr ("0123456789SsQqAaBbCcUu", argv[optind][0]))
    Usage (p);

  umask (022);

  /* Open the fifo and write a command. */
  memset (&request, 0, sizeof (request));
  request.magic = INIT_MAGIC;
  request.cmd = INIT_CMD_RUNLVL;
  request.runlevel = argv[optind][0];
  request.sleeptime = sltime;

  /* Make sure we don't hang on opening /etc/init.fifo */
  signal (SIGALRM, signal_handler);
  siginterrupt (SIGALRM, 1);
  alarm (3);
  if ((fd = open (INIT_FIFO, O_WRONLY)) >= 0 &&
      write (fd, &request, sizeof (request)) == sizeof (request))
    {
      close (fd);
      alarm (0);
      return 0;
    }
#ifndef INITLVL
  perror (INIT_FIFO);
  exit (1);
#endif
  /* Fallthrough to the old method. */

#ifdef INITLVL
  /* Don't know why this is necessary.  It sometimes is.  */
  close (open (INITLVL, O_WRONLY, 0644));

  /* Now write the new runlevel. */
  if ((fp = fopen (INITLVL, "w")) == NULL)
    {
      fprintf (stderr, "%s: cannot create %s\n", p, INITLVL);
      exit (1);
    }
  fprintf (fp, "%s %d", argv[optind], sltime);
  fclose (fp);

  /* And tell init about the pending runlevel change. */
  if (kill (INITPID, SIGHUP) < 0)
    perror (p);

  exit (0);
#endif
}
