/*
 * utmpdump	Simple program to dump UTMP and WTMP files in
 *		raw format, so they can be examined.
 *
 * Version:	@(#)utmpdump.c  13-Aug-1996  1.00  miquels@cistron.nl
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1996 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Modified:	Guido Flohr <gufl0000@stud.uni-sb.de> for MiNT.
 */

#include <stdio.h>
#include <utmp.h>
#include <time.h>

void
dump (fp)
     FILE *fp;
{
  struct utmp ut;
  int f;
  time_t tm;

  while (fread (&ut, sizeof (struct utmp), 1, fp) == 1)
    {
      for (f = 0; f < 12; f++)
	if (ut.ut_line[f] == ' ')
	  ut.ut_line[f] = '_';
      for (f = 0; f < 8; f++)
	if (ut.ut_name[f] == ' ')
	  ut.ut_name[f] = '_';
      tm = ut.ut_time;
      printf ("[%d] [%05d] [%-4.4s] [%-8.8s] [%-12.12s] [%-15.15s]\n",
	      ut.ut_type, (int) ut.ut_pid, ut.ut_id, ut.ut_user,
	      ut.ut_line, 4 + ctime (&tm));
    }
}

int
main (argc, argv)
     int argc;
     char **argv;
{
  int f;
  FILE *fp;

  if (argc < 2)
    {
      argc = 2;
      argv[1] = UTMP_FILE;
    }

  for (f = 1; f < argc; f++)
    {
      if (strcmp (argv[f], "-") == 0)
	{
	  printf ("Utmp dump of stdin\n");
	  /* There is no manpage for this program.  Write at least
	     a header.  */
	  printf ("Type PID     ID     User       Line           Time\n");
	  dump (stdin);
	}
      else if ((fp = fopen (argv[f], "r")) != NULL)
	{
	  printf ("Utmp dump of %s\n", argv[f]);
	  printf ("Type PID     ID     User       Line           Time\n");
	  dump (fp);
	  fclose (fp);
	}
      else
	perror (argv[f]);
    }
  return (0);
}
