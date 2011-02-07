/*
 * shutdown.c	Shut the system down.
 *
 * Usage:	shutdown [-krhfnc] time [warning message]
 *		  -k: don't really shutdown, only warn.
 *		  -r: reboot after shutdown.
 *		  -h: halt after shutdown.
 *		  -f: do a 'fast' reboot (skip fsck).
 *		  -F: Force fsck on reboot.
 *		  -n: do not go through init but do it ourselves.
 *		  -c: cancel an already running shutdown.
 *		  -t secs: delay between SIGTERM and SIGKILL for init.
 *
 * Author:	Miquel van Smoorenburg, miquels@cistron.nl
 *
 * Version:	@(#)shutdown  2.73  25-Nov-1997  MvS
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1997 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Modified:	17 Oct 1999, Guido Flohr
 *		Rehacked for MiNT.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <utmp.h>
#include <time.h>
#include <sys/time.h>
#include "initpaths.h"
#include "reboot.h"

char *Version = "shutdown (mintinit Version " VERSION ")";

#define MESSAGELEN	256

int dontshut = 0;		/* Don't shutdown, only warn    */
char down_level[2];		/* What runlevel to go to.      */
int dosync = 1;			/* Sync before reboot or halt   */
int fastboot = 0;		/* Do a 'fast' reboot           */
int forcefsck = 0;		/* Force fsck on reboot         */
char message[MESSAGELEN];	/* Warning message      */
char *sltime = 0;		/* Sleep time                   */
char newstate[64];		/* What are we gonna do         */
int doself = 0;			/* Don't use init               */

/* From "wall.c" */
extern void wall (char *, int, int);

/* From "utmp.c" */
extern void write_wtmp (char *user, char *id, int pid, int type, char *line);

/*
 * Sleep without being interrupted.
 */
void
hardsleep (int secs)
{
#ifndef __MINT__
  struct timespec ts, rem;

  ts.tv_sec = secs;
  ts.tv_nsec = 0;

  while (nanosleep (&ts, &rem) < 0 && errno == EINTR)
    ts = rem;
#else
  struct timeval then;
  struct timeval now;
  struct timeval rem;
  struct timeval null_timeval = { 0, 0 };
  sigset_t set, saved;

  /* Block all signals.  */
  sigfillset (&set);
  sigprocmask (SIG_BLOCK, &set, &saved);
  gettimeofday (&then, NULL);
  then.tv_sec += secs;

  do
    {
      gettimeofday (&now, NULL);
      timersub (&then, &now, &rem);
      if (timercmp (&rem, &null_timeval, <=))
	break;
      if (select (0, NULL, NULL, NULL, &rem) == 0)
	break;
    }
  while (1);

  /* Unblock signals.  */
  sigprocmask (SIG_SETMASK, &saved, NULL);
#endif
}

/*
 * Break off an already running shutdown.
 */
void
stopit ()
{
  unlink (NOLOGIN);
  unlink (FASTBOOT);
  unlink (FORCEFSCK);
  unlink (SDPID);
  printf ("\r\nShutdown cancelled.\r\n");
  exit (0);
}

/*
 * Show usage message.
 */
void
usage ()
{
  fprintf (stderr,
	   "Usage:\t  shutdown [-krhfnc] [-t secs] time [warning message]\n"
	   "\t\t  -k:      don't really shutdown, only warn.\n"
	   "\t\t  -r:      reboot after shutdown.\n"
	   "\t\t  -h:      halt after shutdown.\n"
	   "\t\t  -f:      do a 'fast' reboot (skip fsck).\n"
	   "\t\t  -F:      Force fsck on reboot.\n"
	   "\t\t  -n:      do not go through \"init\" but go down real fast.\n"
	   "\t\t  -c:      cancel a running shutdown.\n"
	   "\t\t  -t secs: delay between warning and kill signal.\n"
	   "\t\t  ** the \"time\" argument is mandatory! (try \"now\") **\n");
  exit (1);
}

/*
 * Tell everyone the system is going down in 'mins' minutes.
 */
void
warn (mins)
     int mins;
{
  char buf[MESSAGELEN + 64];
  int len;

  strcpy (buf, message);
  len = strlen (buf);

  if (mins == 0)
    sprintf (buf + len, "\rThe system is going down %s NOW !!\r\n", newstate);
  else
    sprintf (buf + len,
	     "\rThe system is going DOWN %s in %d minutes !!\r\n", newstate,
	     mins);
  wall (buf, 1, 0);
}

/*
 * Create the /etc/nologin file.
 */
void
donologin ()
{
  FILE *fp;
  time_t t;

  time (&t);
  t += 300;

  unlink (NOLOGIN);
  if ((fp = fopen (NOLOGIN, "w")) != NULL)
    {
      fprintf (fp, "\rThe system is going down on %s\r\n", ctime (&t));
      if (message[0])
	fputs (message, fp);
      fclose (fp);
    }
}

/* Kill all processes, call /etc/init.d/halt (if present) */
void
fastdown ()
{
  int do_halt = (down_level[0] == '0');
  int i;
#if 0
  char cmd[128];
  char *script;

  /* Currently, the halt script is either init.d/halt OR rc.d/rc.0,
   * likewise for the reboot script. Test for the presence
   * of either.
   */
  if (do_halt)
    {
      if (access (HALTSCRIPT1, X_OK) == 0)
	script = HALTSCRIPT1;
      else
	script = HALTSCRIPT2;
    }
  else
    {
      if (access (REBOOTSCRIPT1, X_OK) == 0)
	script = REBOOTSCRIPT1;
      else
	script = REBOOTSCRIPT2;
    }
#endif

  /* First close all files. */
  for (i = 0; i < 3; i++)
    if (!isatty (i))
      close (i);
  for (i = 3; i < 20; i++)
    close (i);
  close (255);

  /* First idle init. */
  if (kill (1, SIGTSTP) < 0)
    {
      fprintf (stderr, "shutdown: can't idle init.\r\n");
      exit (1);
    }

  /* Kill all processes. */
  fprintf (stderr, "shutdown: sending all processes the TERM signal...\r\n");
  (void) kill (-1, SIGTERM);
  if (sltime)
    sleep (atoi (sltime));
  else
    sleep (3);
  fprintf (stderr, "shutdown: sending all processes the KILL signal.\r\n");
  (void) kill (-1, SIGKILL);

#if 0
  /* See if we can run /etc/init.d/halt */
  if (access (script, X_OK) == 0)
    {
      sprintf (cmd, "%s fast", script);
      system (cmd);
      fprintf (stderr,
	       "shutdown: %s returned - falling back on default routines\r\n",
	       script);
    }
#endif

  /* script failed or not present: do it ourself. */
  sleep (1);			/* Give init the chance to collect zombies. */
  setenv ("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);

  /* Record the fact that we're going down */
  write_wtmp ("shutdown", "~~", 0, RUN_LVL, "~~");

  /* This is for those who have quota installed. */
  system ("accton 2>/dev/null");
  system ("quotaoff -a 2>/dev/null");

  fprintf (stderr, "shutdown: turning off swap\r\n");
  system ("swapoff -a");
  fprintf (stderr, "shutdown: unmounting all file systems\r\n");
  system ("umount -a");

  /* We're done, halt or reboot now. */
  if (do_halt)
    {
      fprintf (stderr,
	       "The system is halted. Press CTRL-ALT-DEL or turn off power\r\n");
      init_reboot (BMAGIC_HALT);
      exit (0);
    }
  fprintf (stderr, "Please stand by while rebooting the system.\r\n");
  init_reboot (BMAGIC_REBOOT);
  exit (0);
}

/*
 * Go to runlevel 0, 1 or 6.
 */
void
shutdown ()
{
  char *args[8];
  int argp = 0;

  /* Warn for the last time (hehe) */
  warn (0);
  if (dontshut)
    {
      hardsleep (1);
      stopit ();
    }

  /* See if we have to do it ourself. */
  if (doself)
    fastdown ();

  /* Create the arguments for init. */
  args[argp++] = INIT;
  if (sltime)
    {
      args[argp++] = "-t";
      args[argp++] = sltime;
    }
  args[argp++] = down_level;
  args[argp] = (char *) NULL;

  unlink (SDPID);
  unlink (NOLOGIN);

  /* Now execute init to change runlevel. */
  execv (INIT, args);

  /* Oops - failed. */
  fprintf (stderr, "\rshutdown: cannot execute %s\r\n", INIT);
  unlink (FASTBOOT);
  unlink (FORCEFSCK);
  unlink (NOLOGIN);
  exit (1);
}

/*
 * Main program.
 * Process the options and do the final countdown.
 */
int
main (argc, argv)
     int argc;
     char **argv;
{
  extern int getopt ();
  extern int optind;
  int c, i, wt, hours, mins;
  struct tm *lt;
  time_t t;
  char *sp;
  char *when = NULL;
  int didnolog = 0;
  int cancel = 0;
  int pid = 0;
  FILE *fp;
  char *downusers[32];
  char buf[128];
  struct stat st;
  struct utmp *ut;
  int user_ok = 0;
  struct sigaction sa;

  /* We can be installed setuid root (executable for a special group) */
  setuid (geteuid ());

  if (getuid () != 0)
    {
      fprintf (stderr, "shutdown: must be root.\n");
      exit (1);
    }
  strcpy (down_level, "1");

  /* See if we have a controlling terminal. */
  if (tcgetpgrp (0) != getpgrp () && getppid () == 1 &&
      (fp = fopen (SDALLOW, "r")) != NULL)
    {

      /* No controlling terminal, we were called from init. */
      i = 0;
      /* Read /etc/shutdown.allow. */
      while (fgets (buf, 128, fp))
	{
	  if (buf[0] == '#' || buf[0] == '\n')
	    continue;
	  if (i > 31)
	    continue;
	  for (sp = buf; *sp; sp++)
	    if (*sp == '\n')
	      *sp = 0;
	  downusers[i++] = strdup (buf);
	}
      if (i < 32)
	downusers[i] = 0;
      fclose (fp);

      /* Now walk through /var/run/utmp to find logged in users. */
      while (!user_ok && (ut = getutent ()) != NULL)
	{

	  /* See if this is a user process on a VC. */
	  if (ut->ut_type != USER_PROCESS)
	    continue;
	  sprintf (buf, "/dev/%s", ut->ut_line);
	  if (stat (buf, &st) < 0)
	    continue;
	  if ((st.st_rdev & 0xFFC0) != 0x0400)
	    continue;

	  /* Root is always OK. */
	  if (strcmp (ut->ut_user, "root") == 0)
	    {
	      user_ok++;
	      break;
	    }

	  /* See if this is an allowed user. */
	  for (i = 0; i < 32 && downusers[i]; i++)
	    if (!strncmp (downusers[i], ut->ut_user, UT_NAMESIZE))
	      {
		user_ok++;
		break;
	      }
	}
      endutent ();

      /* See if user was allowed. */
      if (!user_ok)
	{
	  if ((fp = fopen (CONSOLE, "w")) != NULL)
	    {
	      fprintf (fp, "\rshutdown: no authorized users logged in.\r\n");
	      fclose (fp);
	    }
	  exit (1);
	}
    }

  /* Process the options. */
  while ((c = getopt (argc, argv, "cqkrhnfFyt:g:i:")) != EOF)
    {
      switch (c)
	{
	case 'c':		/* Cancel an already running shutdown. */
	  cancel = 1;
	  break;
	case 'k':		/* Don't really shutdown, only warn. */
	  dontshut = 1;
	  break;
	case 'r':		/* Automatic reboot */
	  down_level[0] = '6';
	  break;
	case 'h':		/* Halt after shutdown */
	  down_level[0] = '0';
	  break;
	case 'f':		/* Don't perform fsck after next boot */
	  fastboot = 1;
	  break;
	case 'F':		/* Force fsck after next boot */
	  forcefsck = 1;
	  break;
	case 'n':		/* Don't switch runlevels. */
	  doself = 1;
	  break;
	case 't':		/* Delay between TERM and KILL */
	  sltime = optarg;
	  break;
	case 'y':		/* Ignored for sysV compatibility */
	  break;
	case 'g':		/* sysv style to specify time. */
	  when = optarg;
	  down_level[0] = '0';
	  break;
	case 'i':		/* Level to go to. */
	  if (!strchr ("0156aAbBcCsS", optarg[0]))
	    {
	      fprintf (stderr, "shutdown: `%s': bad runlevel\n", optarg);
	      exit (1);
	    }
	  down_level[0] = optarg[0];
	  break;
	default:
	  usage ();
	  break;
	}
    }
  /* Read pid of running shutdown from a file */
  if ((fp = fopen (SDPID, "r")) != NULL)
    {
      fscanf (fp, "%d", &pid);
      fclose (fp);
    }

  /* Read remaining words, skip time if needed. */
  message[0] = 0;
  for (c = optind + (!cancel && !when); c < argc; c++)
    {
      if (strlen (message) + strlen (argv[c]) + 4 > MESSAGELEN)
	break;
      strcat (message, argv[c]);
      strcat (message, " ");
    }
  if (message[0])
    strcat (message, "\r\n");

  /* See if we want to run or cancel. */
  if (cancel)
    {
      if (pid <= 0)
	{
	  fprintf (stderr,
		   "shutdown: cannot find pid of running shutdown.\n");
	  exit (1);
	}
      if (kill (pid, SIGINT) < 0)
	{
	  fprintf (stderr, "shutdown: not running.\n");
	  exit (1);
	}
      if (message[0])
	wall (message, 1, 0);
      exit (0);
    }

  /* Check syntax. */
  if (when == NULL)
    {
      if (optind == argc)
	usage ();
      when = argv[optind++];
    }

  /* See if we are already running. */
  if (pid > 0 && kill (pid, 0) == 0)
    {
      fprintf (stderr, "\rshutdown: already running.\r\n");
      exit (1);
    }

  /* Extra check. */
  if (doself && down_level[0] != '0' && down_level[0] != '6')
    {
      fprintf (stderr,
	       "shutdown: can only use \"-n\" for halt or reboot.\r\n");
      exit (1);
    }

  /* Tell users what we're gonna do. */
  switch (down_level[0])
    {
    case '0':
      strcpy (newstate, "for system halt");
      break;
    case '6':
      strcpy (newstate, "for reboot");
      break;
    case '1':
      strcpy (newstate, "to maintenance mode");
      break;
    default:
      sprintf (newstate, "to runlevel %s", down_level);
      break;
    }

  /* Create a new PID file. */
  unlink (SDPID);
  umask (022);
  if ((fp = fopen (SDPID, "w")) != NULL)
    {
      fprintf (fp, "%d\n", getpid ());
      fclose (fp);
    }
  else if (errno != EROFS)
    fprintf (stderr, "shutdown: warning: cannot open %s\n", SDPID);

  /*
   *    Catch some common signals.
   */
  signal (SIGQUIT, SIG_IGN);
  signal (SIGCHLD, SIG_IGN);
  signal (SIGHUP, SIG_IGN);
  signal (SIGTSTP, SIG_IGN);
  signal (SIGTTIN, SIG_IGN);
  signal (SIGTTOU, SIG_IGN);

  sa.sa_handler = stopit;
#ifndef SA_RESTART
# define SA_RESTART 0
#endif

  sa.sa_flags = SA_RESTART;
  sigaction (SIGINT, &sa, NULL);

  /* Go to the root directory */
  chdir ("/");
  if (fastboot)
    close (open (FASTBOOT, O_CREAT | O_RDWR, 0644));
  if (forcefsck)
    close (open (FORCEFSCK, O_CREAT | O_RDWR, 0644));

  /* Alias now and take care of old '+mins' notation. */
  if (!strcmp (when, "now"))
    strcpy (when, "0");
  if (when[0] == '+')
    when++;

  /* Decode shutdown time. */
  if (strchr (when, ':') == NULL)
    {
      /* Time in minutes. */
      wt = atoi (when);
      if (wt == 0 && when[0] != '0')
	usage ();
    }
  else
    {
      /* Time in hh:mm format. */
      if (sscanf (when, "%d:%2d", &hours, &mins) != 2)
	usage ();
      if (hours > 23 || mins > 59)
	usage ();
      time (&t);
      lt = localtime (&t);
      wt = (60 * hours + mins) - (60 * lt->tm_hour + lt->tm_min);
      if (wt < 0)
	wt += 1440;
    }
  /* Shutdown NOW if time == 0 */
  if (wt == 0)
    shutdown ();

  /* Give warnings on regular intervals and finally shutdown. */
  if (wt < 15 && wt != 10 && wt != 5 && wt != 1)
    warn (wt);
  while (wt)
    {
      if (wt <= 5 && !didnolog)
	{
	  donologin ();
	  didnolog++;
	}
      if (wt == 15 || wt == 10 || wt == 5 || wt == 1)
	warn (wt);
      hardsleep (60);
      wt--;
    }
  shutdown ();
  return (0);			/* Never happens */
}
