/*
 * wall.c	Write to all users logged in.
 *
 * Usage:	wall [text]
 *
 * Version:	@(#)wall  2.73  25-Nov-1997  MvS
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1997 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

char *Version = "@(#) wall 2.73 25-Nov-1997 MvS";
#define MAXLEN 4096

extern void wall (char *, int, int);

int
main (int argc, char **argv)
{
  char buf[MAXLEN];
  char line[83];
  int f, ch;
  int len = 0;
  int remote = 0;
  char *p;

  buf[0] = 0;

  while ((ch = getopt (argc, argv, "n")) != EOF)
    switch (ch)
      {
      case 'n':
	/*
	 *      Undocumented option for suppressing
	 *      banner from rpc.rwalld.
	 */
	remote = 1;
	break;
      default:
	fprintf (stderr, "usage: wall [message]\n");
	return 1;
	break;
      }

  if ((argc - optind) > 0)
    {
      for (f = optind; f < argc; f++)
	{
	  len += strlen (argv[f]) + 1;
	  if (len >= MAXLEN)
	    break;
	  strcat (buf, argv[f]);
	  strcat (buf, " ");
	}
      strcat (buf, "\r\n");
    }
  else
    {
      while (fgets (line, 80, stdin))
	{
	  /*
	   *      Make sure that line ends in \r\n
	   */
	  for (p = line; *p && *p != '\n'; p++)
	    ;
	  strcpy (p, "\r\n");
	  len += strlen (line);
	  if (len >= MAXLEN)
	    break;
	  strcat (buf, line);
	}
    }

  wall (buf, 0, remote);

  return 0;
}
