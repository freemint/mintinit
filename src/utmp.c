/*
 * utmp.c	Routines to read/write the utmp and wtmp files.
 *		Basically just wrappers around the library routines.
 *
 * Version:	@(#)utmp.c  1.22  22-Jan-1998  miquels@cistron.nl
 *
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <utmp.h>

#include "init.h"
#include "initreq.h"
#include "initpaths.h"

#if 0				/* Done by autoconf instead.  --gfl.  */
#if defined(__GLIBC__)
#  if (__GLIBC__ == 2) && (__GLIBC_MINOR__ == 0) && defined(__powerpc__)
#    define HAVE_UPDWTMP 0
#  else
#    define HAVE_UPDWTMP 1
#  endif
#else
#  define HAVE_UPDWTMP 0
#endif
#endif

/*
 *	Log an event ONLY in the wtmp file (reboot, runlevel)
 */
void
write_wtmp (char *user,		/* name of user */
	    char *id,		/* inittab ID */
	    int pid,		/* PID of process */
	    int type,		/* TYPE of entry */
	    char *line)		/* Which line is this */
{
  int fd;
  struct utmp utmp;

  /*
   *      Try to open the wtmp file. Note that we even try
   *      this if we have updwtmp() so we can see if the
   *      wtmp file is accessible.
   */
  if ((fd = open (WTMP_FILE, O_WRONLY | O_APPEND)) < 0)
    return;

#ifdef INIT_MAIN
  /*
   *      Note if we are going to write a boot record.
   */
  if (type == BOOT_TIME)
    wrote_reboot++;

  /*
   *      See if we need to write a reboot record. The reason that
   *      we are being so paranoid is that when we first tried to
   *      write the reboot record, /var was possibly not mounted
   *      yet. As soon as we can open WTMP we write a delayed boot record.
   */
  if (wrote_reboot == 0 && type != BOOT_TIME)
    write_wtmp ("reboot", "~~", 0, BOOT_TIME, "~");
#endif

  /*
   *      Zero the fields and enter new fields.
   */
  memset (&utmp, 0, sizeof (utmp));
#if defined(__GLIBC__) || defined(__MINT__)
  gettimeofday (&utmp.ut_tv, NULL);
#else
  time (&utmp.ut_time);
#endif
  utmp.ut_pid = pid;
  utmp.ut_type = type;
  strncpy (utmp.ut_name, user, sizeof (utmp.ut_name));
  strncpy (utmp.ut_id, id, sizeof (utmp.ut_id));
  strncpy (utmp.ut_line, line, sizeof (utmp.ut_line));

#if HAVE_UPDWTMP
  updwtmp (WTMP_FILE, &utmp);
#else
  write (fd, (char *) &utmp, sizeof (utmp));
#endif
  close (fd);
}

/*
 *	Proper implementation of utmp handling. This part uses the
 *	library functions for utmp stuff, and doesn't worry about
 *	cleaning up if other programs mess up utmp.
 */
void
write_utmp_wtmp (char *user,	/* name of user */
		 char *id,	/* inittab ID */
		 int pid,	/* PID of process */
		 int type,	/* TYPE of entry */
		 char *line)	/* LINE if used. */
{
  struct utmp utmp;
  struct utmp tmp;
  struct utmp *utmptr;
  int fd;

  /*
   *      For backwards compatibility we just return
   *      if user == NULL (means : clean up utmp file).
   */
  if (user == NULL)
    return;

  /*
   *      Fill out an utmp struct.
   */
  memset (&utmp, 0, sizeof (utmp));
  utmp.ut_type = type;
  utmp.ut_pid = pid;
  strncpy (utmp.ut_id, id, sizeof (utmp.ut_id));
#if defined(__GLIBC__) || defined(__MINT__)
  gettimeofday (&utmp.ut_tv, NULL);
#else
  time (&utmp.ut_time);
#endif
  strncpy (utmp.ut_user, user, UT_NAMESIZE);
  if (line)
    strncpy (utmp.ut_line, line, UT_LINESIZE);

  /*
   *      We might need to find the existing entry first, to
   *      find the tty of the process (for wtmp accounting).
   */
  if (type == DEAD_PROCESS)
    {
      /*
       *      Find existing entry for the tty line.
       */
      setutent ();
      tmp = utmp;
      if ((utmptr = getutid (&tmp)) != NULL)
	strncpy (utmp.ut_line, utmptr->ut_line, UT_LINESIZE);
    }

  /*
   *      Update existing utmp file.
   */
  setutent ();
  pututline (&utmp);
  endutent ();

  /*
   *      Write the wtmp record if we can open the wtmp file.
   */
  if ((fd = open (WTMP_FILE, O_WRONLY | O_APPEND)) >= 0)
    {
#ifdef INIT_MAIN
      /* See if we need to write a boot record */
      if (wrote_reboot == 0 && type != BOOT_TIME)
	{
	  write_wtmp ("reboot", "~~", 0, BOOT_TIME, "~");
	  wrote_reboot++;
	}
#endif
#if HAVE_UPDWTMP
      updwtmp (WTMP_FILE, &utmp);
#else
      (void) write (fd, (char *) &utmp, sizeof (struct utmp));
#endif
      (void) close (fd);
    }
}
