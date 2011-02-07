/*
 * dowall.c	Write to all users on the system.
 *
 * Author:	Miquel van Smoorenburg, miquels@cistron.nl
 * 
 * Version:	@(#)dowall.c  2.73  25-Nov-1997  miquels@cistron.nl
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1997 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <utmp.h>
#include <pwd.h>
#include <fcntl.h>
#include <signal.h>
#include <setjmp.h>

static jmp_buf jbuf;

#define AEROSMITH

/*
 *	Alarm handler
 */
 /*ARGSUSED*/ static void
handler (arg)
     int arg;
{
  signal (SIGALRM, handler);
  longjmp (jbuf, 1);
}

/*
 *	Wall function.
 */
void
wall (char *text, int fromshutdown, int remote)
{
  FILE *tp;
  char line[81];
  char term[32];
  static char *user, ttynm[16], *date;
  static int fd, init = 0;
  struct passwd *pwd;
  char *tty, *p;
  time_t t;
  struct utmp *utmp;
  int uid;

  setutent ();

  if (init == 0)
    {
      uid = getuid ();
      if ((pwd = getpwuid (uid)) == (struct passwd *) 0 &&
	  (uid != 0 || fromshutdown))
	{
	  fprintf (stderr, "You don't exist. Go away.\n");
	  exit (1);
	}
      user = pwd ? pwd->pw_name : "root";
      if ((p = ttyname (0)) != (char *) 0)
	{
	  if ((tty = strrchr (p, '/')) != NULL)
	    tty++;
	  else
	    tty = p;
	  sprintf (ttynm, "(%s) ", tty);
	}
      else
	ttynm[0] = 0;
      init++;
      signal (SIGALRM, handler);
    }

  /* Get the time */
  time (&t);
  date = ctime (&t);
  for (p = date; *p && *p != '\n'; p++)
    ;
  *p = 0;

  if (remote)
    {
      sprintf (line, "\007\r\nRemote broadcast message %s...\r\n\r\n", date);
    }
  else
    {
      sprintf (line,
	       "\007\r\nBroadcast message from %s %s%s...\r\n\r\n",
	       user, ttynm, date);
    }

  /*
   *      Fork to avoid us hanging in a write()
   */
  if (fork () != 0)
    return;

  siginterrupt (SIGALRM, 1);
  while ((utmp = getutent ()) != NULL)
    {
      if (utmp->ut_type != USER_PROCESS || utmp->ut_user[0] == 0)
	continue;
      sprintf (term, "/dev/%s", utmp->ut_line);

      /*
       *      Sometimes the open/write hangs in spite of the O_NDELAY
       */
      alarm (2);
#ifdef O_NDELAY
      /*
       *      Open it non-delay
       */
      if (setjmp (jbuf) == 0 && (fd = open (term, O_WRONLY | O_NDELAY)) > 0)
	{
	  if ((tp = fdopen (fd, "w")) != NULL)
	    {
	      fputs (line, tp);
	      fputs (text, tp);
#ifdef AEROSMITH
	      if (fromshutdown && !strcmp (utmp->ut_user, "tyler"))
		fputs ("Oh hello Mr. Tyler - going DOWN?\r\n", tp);
#endif
	      fclose (tp);
	    }
	  else
	    close (fd);
	  fd = -1;
	  alarm (0);
	}
      if (fd >= 0)
	close (fd);
#else
      if (setjmp (jbuf) == 0 && (tp = fopen (term, "w")) != NULL)
	{
	  fputs (line, tp);
	  fputs (text, tp);
	  alarm (0);
	  fclose (tp);
	  tp = NULL;
	}
      if (tp != NULL)
	fclose (tp);
#endif
      alarm (0);
    }
  endutent ();
  exit (0);
}
