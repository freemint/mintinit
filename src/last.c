/*
 * last.c	Re-implementation of the 'last' command, this time
 *		for Linux. Yes I know there is BSD last, but I
 *		just felt like writing this. No thanks :-).
 *		Also, this version gives lots more info (especially with -x)
 *
 * Author:	Miquel van Smoorenburg, miquels@cistron.nl
 *
 * Version:	@(#)last  2.74  12-Mar-1998  miquels@cistron.nl
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1998 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Changed:	Sat Dec 25 1999 Guido Flohr <guido@atari.org>
 *		- include <sys/socket.h>.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <utmp.h>
#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

char *Version = "@(#) last 2.73 25-Nov-1997 MvS";

#define CHOP_DOMAIN	0	/* Define to chop off local domainname. */

/*
 *	The NEW_UTMP code is a lot faster, but needs to be
 *	looked at carefully, especially with utmp file sizes
 *	of several MB's - turned off for now.
 */
#define NEW_UTMP	0

/* Double linked list of struct utmp's */
struct utmplist
{
  struct utmp ut;
  struct utmplist *next;
  struct utmplist *prev;
};
struct utmplist *utmplist = NULL;

/* Types of listing */
#define R_CRASH		1	/* No logout record, system boot in between */
#define R_DOWN		2	/* System brought down in decent way */
#define R_NORMAL	3	/* Normal */
#define R_NOW		4	/* Still logged in */
#define R_REBOOT	5	/* Reboot record. */

/* Global variables */
int maxrecs = 0;		/* Maximum number of records to list. */
int recsdone = 0;		/* Number of records listed */
int showhost = 1;		/* Show hostname too? */
int altlist = 0;		/* Show hostname at the end. */
int usedns = 0;			/* Use DNS to lookup the hostname. */
char **show = NULL;		/* What do they want us to show */
char *ufile;			/* Filename of this file */
FILE *fp;			/* Filepointer of wtmp file */
time_t lastdate;		/* Last date we've seen */
#if CHOP_DOMAIN
char hostname[256];		/* For gethostbyname() */
char *domainname;		/* Our domainname. */
#endif

/* Try to be smart about the location of the BTMP file */
#ifndef BTMP_FILE
#define BTMP_FILE getbtmp()
char *
getbtmp ()
{
  static char btmp[128];
  char *p;

  strcpy (btmp, WTMP_FILE);
  if ((p = strrchr (btmp, '/')) == NULL)
    p = btmp;
  else
    p++;
  *p = 0;
  strcat (btmp, "btmp");
  return (btmp);
}
#endif

/* Print a short date. */
char *
showdate ()
{
  char *s = ctime (&lastdate);
  s[16] = 0;
  return s;
}

/* SIGINT handler */
void
int_handler ()
{
  printf ("Interrupted %s\n", showdate ());
  exit (1);
}

/* SIGQUIT handler */
void
quit_handler ()
{
  printf ("Interrupted %s\n", showdate ());
  signal (SIGQUIT, quit_handler);
}

/* Get the basename of a filename */
char *
mybasename (char *s)
{
  char *p;

  if ((p = strrchr (s, '/')) != NULL)
    p++;
  else
    p = s;
  return (p);
}

/* Lookup a host with DNS. */
int
dns_lookup (char *result, char *org, unsigned int ip)
{
  struct hostent *h;

  /* Try to catch illegal IP numbers */
  if (ip == 0 || (int) ip == -1 || (ip >> 24) == 0 || (ip & 255) == 0)
    {
      strncpy (result, org, UT_HOSTSIZE);
      result[UT_HOSTSIZE] = 0;
      return 0;
    }

  if ((h = gethostbyaddr ((char *) &ip, 4, AF_INET)) == NULL)
    {
      strcpy (result, inet_ntoa (*(struct in_addr *) &ip));
      return 0;
    }
  strncpy (result, h->h_name, 256);
  result[255] = 0;
  return 0;
}

/* Show one line of information on screen */
int
list (struct utmp *p, time_t t, int what)
{
  char logintime[32];
  char logouttime[32];
  char length[32];
  char final[128];
  char domain[256];
  time_t secs, tmp;
  int mins, hours, days;
  char *s, **walk;

  /* uucp and ftp have special-type entries */
  if (strncmp (p->ut_line, "ftp", 3) == 0)
    p->ut_line[3] = 0;
  if (strncmp (p->ut_line, "uucp", 4) == 0)
    p->ut_line[4] = 0;

  /* Is this something we wanna show? */
  if (show)
    {
      for (walk = show; *walk; walk++)
	{
	  if (strncmp (p->ut_name, *walk, 8) == 0 ||
	      strncmp (p->ut_line, *walk, 12) == 0 ||
	      strncmp (p->ut_line + 3, *walk, 9) == 0)
	    break;
	}
      if (*walk == NULL)
	return (0);
    }

  /* Calculate times */
  tmp = (time_t) p->ut_time;
  strcpy (logintime, ctime (&tmp));
  logintime[16] = 0;
  lastdate = p->ut_time;
  sprintf (logouttime, "- %s", ctime (&t) + 11);
  logouttime[7] = 0;
  secs = t - p->ut_time;
  mins = (secs / 60) % 60;
  hours = (secs / 3600) % 24;
  days = secs / 86400;
  if (days)
    sprintf (length, "(%d+%02d:%02d)", days, hours, mins);
  else
    sprintf (length, " (%02d:%02d)", hours, mins);

  switch (what)
    {
    case R_CRASH:
      sprintf (logouttime, "- crash");
      break;
    case R_DOWN:
      sprintf (logouttime, "- down ");
      break;
    case R_NOW:
      length[0] = 0;
      sprintf (logouttime, "  still");
      sprintf (length, "logged in");
      break;
    case R_REBOOT:
      logouttime[0] = 0;
      length[0] = 0;
      break;
    case R_NORMAL:
      break;
    }

  /* Look up host with DNS if needed. */
  if (usedns)
    dns_lookup (domain, p->ut_host, p->ut_addr);
  else
    {
      strncpy (domain, p->ut_host, UT_HOSTSIZE);
      domain[UT_HOSTSIZE] = 0;
    }

  if (showhost)
    {
#if CHOP_DOMAIN
      /* See if this is in our domain. */
      if (!usedns && (s = strchr (p->ut_host, '.')) != NULL &&
	  strcmp (s + 1, domainname) == 0)
	*s = 0;
#endif
      if (!altlist)
	{
	  sprintf (final, "%-8.8s %-12.12s %-16.16s %s %s %s\n",
		   p->ut_name, p->ut_line,
		   domain, logintime, logouttime, length);
	}
      else
	{
	  sprintf (final, "%-8.8s %-12.12s %-16.16s %-7.7s %-12.12s %s\n",
		   p->ut_name, p->ut_line,
		   logintime, logouttime, length, domain);
	}
    }
  else
    sprintf (final, "%-8.8s %-12.12s %s %s %s\n", p->ut_name, p->ut_line,
	     logintime, logouttime, length);

  /* Print out "final" string safely. */
  for (s = final; *s; s++)
    {
      if (*s == '\n' || (*s >= 32 && (unsigned char) *s <= 128))
	putchar (*s);
      else
	putchar ('*');
    }

  recsdone++;
  if (maxrecs && recsdone >= maxrecs)
    return (1);
  return (0);
}

/* show usage */
void
usage (char *s)
{
  fprintf (stderr,
	   "Usage: %s [-num | -n num] [-f file] [-R] [-x] [username..] [tty..]\n",
	   s);
  exit (1);
}

#ifdef NEW_UTMP

#define UBUFS 512

/*
 * This is pretty complicated - so this is what's going on:
 *
 * - pos, off and sz are in units of sizeof(struct utmp).
 * - we read a block of size (UBUFS * sizeof(struct utmp))
 * - pos: starting position in the file from where the block was read.
 * - off: end of the block minus one struct utmp, counting down.
 */
struct utmp *
get_one (FILE * fp)
{
  static int pos = -1;
  static int off = -1;
  static int first = 1;
  size_t sz;
  static struct utmp buf[UBUFS];
  struct utmp *u;

  if (first)
    {
      fseek (fp, 0, SEEK_END);
      pos = ftell (fp) / sizeof (struct utmp);
      first = 0;
      if (pos == 0)
	return NULL;
    }
  if (pos < 0)
    return NULL;

  if (off < pos)
    {
      /* Search back */
      sz = pos;
      if (sz > UBUFS)
	sz = UBUFS;
      off = pos - 1;
      pos -= sz;
      if (pos < 0 || sz == 0)
	return NULL;
      if (fseek (fp, pos * sizeof (struct utmp), SEEK_SET) < 0)
	{
	  perror ("fseek");
	  pos = -1;
	  return NULL;
	}
      if (fread (buf, sizeof (struct utmp), sz, fp) != sz)
	{
	  perror ("fread");
	  pos = -1;
	  return NULL;
	}
    }
  u = buf + (off - pos);
  off--;
  return u;
}
#endif /* NEW_UTMP */

int
main (int argc, char **argv)
{
  struct utmp ut;		/* Current utmp entry */
#if NEW_UTMP
  struct utmp *u;
#endif
  struct utmplist *p;		/* Pointer into utmplist */
  struct utmplist *next;	/* Pointer into utmplist */
  time_t lastboot = 0;		/* Last boottime */
  time_t lastrch = 0;		/* Last run level change */
  time_t lastdown;		/* Last downtime */
  time_t begintime;		/* When wtmp begins */
  int whydown = 0;		/* Why we went down: crash or shutdown */
  int c, x;			/* Scratch */
  char *progname;		/* Name of this program */
  struct stat st;		/* To stat the [uw]tmp file */
  int quit = 0;			/* Flag */
  int lastb = 0;		/* Is this 'lastb' ? */
  int extended = 0;		/* Lots of info. */
  int down;			/* Down flag */
  char *altufile = NULL;	/* Alternate wtmp */

  progname = mybasename (argv[0]);

  /* Process the arguments. */
  while ((c = getopt (argc, argv, "f:n:Rxad0123456789")) != EOF)
    switch (c)
      {
      case 'R':
	showhost = 0;
	break;
      case 'x':
	extended = 1;
	break;
      case 'n':
	maxrecs = atoi (optarg);
	break;
      case 'f':
	if ((altufile = malloc (strlen (optarg) + 1)) == NULL)
	  {
	    fprintf (stderr, "%s: out of memory\n", progname);
	    exit (1);
	  }
	strcpy (altufile, optarg);
	break;
      case 'd':
	usedns++;
	break;
      case 'a':
	altlist++;
	break;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
	maxrecs = 10 * maxrecs + c - '0';
	break;
      default:
	usage (progname);
	break;
      }
  if (optind < argc)
    show = argv + optind;

  /* Which file do we want to read? */
  if (strcmp (progname, "lastb") == 0)
    {
      ufile = BTMP_FILE;
      lastb = 1;
    }
  else if (altufile)
    ufile = altufile;
  else
    ufile = WTMP_FILE;
  time (&lastdown);
  lastrch = lastdown;

  /* Fill in 'lastdate' */
  lastdate = lastdown;

#if CHOP_DOMAIN
  /* Find out domainname. */
  (void) gethostname (hostname, 256);
  if ((domainname = strchr (hostname, '.')) != NULL)
    domainname++;
  if (domainname == NULL || domainname[0] == 0)
    {
      (void) getdomainname (hostname, 256);
      domainname = hostname;
      if (strcmp (domainname, "(none)") == 0 || domainname[0] == 0)
	domainname = NULL;
    }
#endif

  /* Install signal handlers */
  signal (SIGINT, int_handler);
  signal (SIGQUIT, quit_handler);

  /* Open the utmp file */
  if ((fp = fopen (ufile, "r")) == NULL)
    {
      x = errno;
      fprintf (stderr, "%s: %s: %s\n", progname, ufile, strerror (errno));
      if (altufile == NULL && x == ENOENT)
	fprintf (stderr,
		 "Perhaps this file was removed by the operator to prevent logging %s info.\n",
		 progname);
      exit (1);
    }

  /* Read first struture to capture the time field */
  if (fread (&ut, sizeof (struct utmp), 1, fp) == 1)
    begintime = ut.ut_time;
  else
    {
      fstat (fileno (fp), &st);
      begintime = st.st_ctime;
    }

#if !NEW_UTMP
  /* Go to end of file minus one structure */
  fseek (fp, -1L * sizeof (struct utmp), SEEK_END);
#endif

  /* Read struct after struct */
#if !NEW_UTMP
  while (!quit && fread (&ut, sizeof (struct utmp), 1, fp))
    {
#else
  while (!quit && (u = get_one (fp)) != NULL)
    {
      ut = *u;
#endif
      if (ut.ut_time)
	lastdate = ut.ut_time;
      if (lastb)
	quit = list (&ut, ut.ut_time, R_NORMAL);
      else
	{
	  /* BSD does things quite different. Try to be
	   * smart and accept both BSD and sysv records.
	   */
	  down = 0;
	  if (strncmp (ut.ut_line, "~", 1) == 0)
	    {
	      /* Halt/reboot/shutdown etc */
	      if (strncmp (ut.ut_user, "shutdown", 8) == 0)
		{
		  if (extended)
		    {
		      strcpy (ut.ut_line, "system down");
		      quit = list (&ut, lastdown, R_NORMAL);
		    }
		  lastdown = lastrch = ut.ut_time;
		  down = 1;
		}
	      if (strncmp (ut.ut_user, "reboot", 6) == 0)
		{
		  strcpy (ut.ut_line, "system boot");
		  quit = list (&ut, lastdown, R_REBOOT);
		  lastdown = ut.ut_time;
		  down = 1;
		}
	      if (strncmp (ut.ut_user, "runlevel", 7) == 0)
		{
		  x = ut.ut_pid & 255;
		  if (extended)
		    {
		      sprintf (ut.ut_line, "(to lvl %c)", x);
		      quit = list (&ut, lastrch, R_NORMAL);
		    }
		  if (x == '0' || x == '6')
		    {
		      lastdown = ut.ut_time;
		      down = 1;
		    }
		  lastrch = ut.ut_time;
		}
	      if (down)
		{
		  lastboot = ut.ut_time;
		  whydown = (ut.ut_type == RUN_LVL) ? R_DOWN : R_CRASH;

		  /* Delete the list; it's meaningless now */
		  for (p = utmplist; p; p = next)
		    {
		      next = p->next;
		      free (p);
		    }
		  utmplist = NULL;
		}
	    }
	  else
	    {
	      /*
	       *      Allright, is this a login or a logout?
	       *
	       *      FIXME: the check on DEAD_PROCESS (added 07-Feb-1997) might
	       *      break some very old BSD code..
	       */
	      if (ut.ut_name[0] != 0 && strcmp (ut.ut_name, "LOGIN") != 0 &&
		  ut.ut_type != DEAD_PROCESS)
		{
		  /*
		   *      This is a login
		   *      Walk through list to see when logged out
		   */
		  for (p = utmplist; p; p = next)
		    {
		      next = p->next;
		      if (strncmp (p->ut.ut_line, ut.ut_line, 12) == 0)
			{
			  /* Show it */
			  quit = list (&ut, p->ut.ut_time, R_NORMAL);
			  /* Delete it from the list */
			  if (p->next)
			    p->next->prev = p->prev;
			  if (p->prev)
			    p->prev->next = p->next;
			  else
			    utmplist = p->next;
			  free (p);
			  break;
			}
		    }
		  /* Not found? Then crashed, down or still logged in */
		  if (p == NULL)
		    {
		      if (lastboot == 0)
			quit = list (&ut, time (NULL), R_NOW);
		      else
			quit = list (&ut, lastboot, whydown);
		    }
		}
	      /* Get some memory */
	      if ((p = malloc (sizeof (struct utmplist))) == NULL)
		{
		  fprintf (stderr, "%s: out of memory\n", progname);
		  exit (1);
		}
	      /* Fill in structure */
	      memcpy (&p->ut, &ut, sizeof (struct utmp));
	      p->next = utmplist;
	      p->prev = NULL;
	      if (utmplist)
		utmplist->prev = p;
	      utmplist = p;
	    }
	}
#if !NEW_UTMP
      /* Position the file pointer 2 structures back */
      if (fseek (fp, -2 * sizeof (struct utmp), SEEK_CUR) < 0)
	break;
#endif
    }
  printf ("\n%s begins %s", mybasename (ufile), ctime (&begintime));

  fclose (fp);
  /* Should we free memory here? Nah. */
  return (0);
}
