/*
 * mesg.c	The "mesg" utility. Gives / restrict access to
 *		your terminal by others.
 *
 * Usage:	mesg [y|n].
 *		Without arguments prints out the current settings.
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1996 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <grp.h>

char *Version = "@(#) mesg 1.3 13-Aug-1996 MvS";

/*
 *	Here we check if this system has a "tty" group
 *	for the tty device. If it does, we set the modes
 *	to -rw--w--- instead if -rw--w--w.
 */
int
hasttygrp (struct stat *st)
{
  struct group *gr;

  if ((gr = getgrgid (st->st_gid)) == NULL)
    return 0;
  if (strcmp (gr->gr_name, "tty") != 0)
    return 0;

  return 1;
}

int
main (int argc, char **argv)
{
  struct stat st;
  unsigned int ttymode;

  if (!isatty (0))
    {
      /* Or should we look in /var/run/utmp? */
      fprintf (stderr, "stdin: is not a tty");
      return (1);
    }

  if (fstat (0, &st) < 0)
    {
      perror ("fstat");
      return (1);
    }

  ttymode = hasttygrp (&st) ? 020 : 022;

  if (argc < 2)
    {
      printf ("is %s\n", ((st.st_mode & ttymode) == ttymode) ? "y" : "n");
      return (0);
    }
  if (argc > 2 || (argv[1][0] != 'y' && argv[1][0] != 'n'))
    {
      fprintf (stderr, "Usage: mesg [y|n]\n");
      return (1);
    }
  if (argv[1][0] == 'y')
    st.st_mode |= ttymode;
  else
    st.st_mode &= ~(ttymode);
  fchmod (0, st.st_mode);
  return (0);
}
