/*
 * sulogin	This program gives Linux machines a reasonable
 *		secure way to boot single user. It forces the
 *		user to supply the root password before a
 *		shell is started.
 *
 *		If the password file is in any way corrupt
 *		a root password is not needed.
 *
 *		If there is a shadow password file and the
 *		encrypted root password is "x" the shadow
 *		password will be used.
 *
 * Version:	@(#)sulogin 2.57 26-Nov-1997 MvS.
 *
 * Changed:	Sat Dec 25 1999, Guido Flohr <guido@atari.org>
 *		- Minor modifications for MiNT.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <termios.h>
#include <sys/ioctl.h>
#if defined(__GLIBC__) || defined(__MINT__)
#  include <crypt.h>
#endif

#define CHECK_DES	1
#define CHECK_MD5	1

#define PASSWD	"/etc/passwd"
#define SHADOW	"/etc/shadow"
#define BINSH	"/bin/sh"

char *Version = "@(#)sulogin 2.57 26-Nov-1997 MvS.";

#ifdef __MINT__
# define getpgid(i) (getpgrp())
# define getsid getpgid
#endif

int timeout = 0;

#if 0
/*
 *	Fix the tty modes and set reasonable defaults.
 *	(I'm not sure if this is needed under Linux, but..)
 */
void
fixtty (void)
{
  struct termios tty;

  tcgetattr (0, &tty);

  /*
   *      Set or adjust tty modes.
   */
  tty.c_iflag &= ~(INLCR | IGNCR | IUCLC);
  tty.c_iflag |= ICRNL;
  tty.c_oflag &= ~(OCRNL | OLCUC | ONOCR | ONLRET | OFILL);
  tty.c_oflag |= OPOST | ONLCR;
  tty.c_cflag |= CLOCAL;
  tty.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE;

  /*
   *      Set the most important characters */
  */tty.c_cc[VINTR] = 3;
  tty.c_cc[VQUIT] = 28;
  tty.c_cc[VERASE] = 127;
  tty.c_cc[VKILL] = 24;
  tty.c_cc[VEOF] = 4;
  tty.c_cc[VTIME] = 0;
  tty.c_cc[VMIN] = 1;
  tty.c_cc[VSTART] = 17;
  tty.c_cc[VSTOP] = 19;
  tty.c_cc[VSUSP] = 26;

  tcsetattr (0, TCSANOW, &tty);
}
#endif


/*
 *	Called at timeout.
 */
void
alrm_handler ()
{
  signal (SIGALRM, alrm_handler);
}

/*
 *	See if an encrypted password is valid. The encrypted
 *	password is checked for traditional-style DES and
 *	FreeBSD-style MD5 encryption.
 */
int
valid (char *pass)
{
  char *s;

  if (pass[0] == 0)
    return 1;
#if CHECK_MD5
  /*
   *      3 bytes for the signature $1$
   *      up to 8 bytes for the salt
   *      $
   *      the MD5 hash (128 bits or 16 bytes) encoded in base64 = 22 bytes
   */
  if (strncmp (pass, "$1$", 3) == 0)
    {
      for (s = pass + 3; *s && *s != '$'; s++)
	;
      if (*s++ != '$')
	return 0;
      if (strlen (s) != 22)
	return 0;

      return 1;
    }
#endif
#if CHECK_DES
  if (strlen (pass) != 13)
    return 0;
  for (s = pass; *s; s++)
    {
      if ((*s < '0' || *s > '9') &&
	  (*s < 'a' || *s > 'z') &&
	  (*s < 'A' || *s > 'Z') && *s != '.' && *s != '/')
	return 0;
    }
#endif
  return 1;
}

/*
 *	Set a variable if the value is not NULL.
 */
void
set (char **var, char *val)
{
  if (val)
    *var = val;
}

/*
 *	Get the root password entry.
 */
struct passwd *
getrootpwent ()
{
  static struct passwd pwd;
  FILE *fp;
  static char line[256];
  static char sline[256];
  char *p;

  /*
   *      Fill out some defaults in advance.
   */
  pwd.pw_name = "root";
  pwd.pw_passwd = "";
  pwd.pw_gecos = "Super User";
  pwd.pw_dir = "/";
  pwd.pw_shell = "";
  pwd.pw_uid = 0;
  pwd.pw_gid = 0;

  if ((fp = fopen (PASSWD, "r")) == NULL)
    {
      perror (PASSWD);
      return &pwd;
    }

  /*
   *      Find root in the password file.
   */
  while ((p = fgets (line, 256, fp)) != NULL)
    {
      if (strncmp (line, "root:", 5) != 0)
	continue;
      p += 5;
      set (&pwd.pw_passwd, strsep (&p, ":"));
      (void) strsep (&p, ":");
      (void) strsep (&p, ":");
      set (&pwd.pw_gecos, strsep (&p, ":"));
      set (&pwd.pw_dir, strsep (&p, ":"));
      set (&pwd.pw_shell, strsep (&p, "\n"));
      p = line;
      break;
    }
  fclose (fp);

  /*
   *      If the encrypted password is valid
   *      or not found, return.
   */
  if (p == NULL)
    {
      fprintf (stderr, "%s: no entry for root\n", PASSWD);
      return &pwd;
    }
  if (valid (pwd.pw_passwd))
    return &pwd;

  /*
   *      The password is invalid. If there is a
   *      shadow password, try it.
   */
  strcpy (pwd.pw_passwd, "");
  if ((fp = fopen (SHADOW, "r")) == NULL)
    {
      fprintf (stderr, "%s: root password invalid\n", PASSWD);
      return &pwd;
    }
  while ((p = fgets (sline, 256, fp)) != NULL)
    {
      if (strncmp (sline, "root:", 5) != 0)
	continue;
      p += 5;
      set (&pwd.pw_passwd, strsep (&p, ":"));
      break;
    }
  fclose (fp);

  /*
   *      If the password is still invalid,
   *      NULL it, and return.
   */
  if (p == NULL)
    {
      fprintf (stderr, "%s: no entry for root\n", SHADOW);
      strcpy (pwd.pw_passwd, "");
    }
  if (!valid (pwd.pw_passwd))
    {
      fprintf (stderr, "%s: root password invalid\n", SHADOW);
      strcpy (pwd.pw_passwd, "");
    }
  return &pwd;
}

/*
 *	Ask for the password. Note that there is no
 *	default timeout as we normally skip this during boot.
 */
char *
getpasswd ()
{
  struct termios old, tty;
  static char pass[16];
  char *ret = pass;
  int i;

  printf ("Give root password for maintenance\n");
  printf ("(or type Control-D for normal startup): ");
  fflush (stdout);

  tcgetattr (0, &old);
  tcgetattr (0, &tty);
#ifndef IUCLC
# define IUCLC 0
#endif
  tty.c_iflag &= ~(IUCLC | IXON | IXOFF | IXANY);
  tty.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL | TOSTOP);
  tcsetattr (0, TCSANOW, &tty);

  pass[15] = 0;
  if (timeout)
    alarm (timeout);
  if (read (0, pass, 15) <= 0)
    ret = NULL;
  else
    {
      for (i = 0; i < 15; i++)
	if (pass[i] == '\r' || pass[i] == '\n')
	  {
	    pass[i] = 0;
	    break;
	  }
    }
  alarm (0);
  tcsetattr (0, TCSANOW, &old);
  printf ("\n");

  return ret;
}

/*
 *	Password was OK, execute a shell.
 */
void
sushell (struct passwd *pwd)
{
  char shell[64];
  char home[64];
  char *p;
  char *sushell;

  /*
   *      Set directory and shell.
   */
  (void) chdir (pwd->pw_dir);
  if ((p = getenv ("SUSHELL")) != NULL)
    sushell = p;
  else if ((p = getenv ("sushell")) != NULL)
    sushell = p;
  else
    {
      if (pwd->pw_shell[0])
	sushell = pwd->pw_shell;
      else
	sushell = BINSH;
    }
  if ((p = strrchr (sushell, '/')) == NULL)
    p = sushell;
  else
    p++;
  strcpy (shell, "-");
  strcat (shell, p);

  /*
   *      Set some important environment variables.
   */
  getcwd (home, 64);
  setenv ("HOME", home, 1);
  setenv ("LOGNAME", "root", 1);
  setenv ("USER", "root", 1);
  setenv ("SHELL", sushell, 1);

  /*
   *      Try to execute a shell.
   */
  execl (sushell, shell, NULL);
  perror (sushell);
  setenv ("SHELL", BINSH, 1);
  execl (BINSH, "-sh", NULL);
  perror (BINSH);
}

void
usage (void)
{
  fprintf (stderr, "Usage: sulogin [-t timeout] [tty device]\n");
}

int
main (int argc, char **argv)
{
  char *tty = NULL;
  char *p;
  struct passwd *pwd;
  int c, fd;
  pid_t pid, pgrp, ppgrp, ttypgrp;

  /*
   *      See if we have a timeout flag.
   */
  opterr = 0;
  while ((c = getopt (argc, argv, "t:")) != EOF)
    switch (c)
      {
      case 't':
	timeout = atoi (optarg);
	break;
      default:
	usage ();
	/* Do not exit! */
	break;
      }

  siginterrupt (SIGALRM, 1);
  signal (SIGALRM, alrm_handler);

  /*
   *      See if we need to open an other tty device.
   */
  if (optind < argc)
    tty = argv[optind];
  if (tty)
    {
      if ((fd = open (tty, O_RDWR)) >= 0)
	{

	  /*
	   *      Only go through this trouble if the new
	   *      tty doesn't fall in this process group.
	   */
	  pid = getpid ();
	  pgrp = getpgrp ();
	  pgrp = getpgid (0);
	  ppgrp = getpgid (getppid ());
	  ioctl (fd, TIOCGPGRP, &ttypgrp);

	  if (pgrp != ttypgrp && ppgrp != ttypgrp)
	    {
	      if (pid != getsid (0))
		{
		  if (pid == getpgid (0))
		    setpgid (0, getpgid (getppid ()));
		  setsid ();
		}

	      signal (SIGHUP, SIG_IGN);
	      ioctl (0, TIOCNOTTY, (char *) 1);
	      signal (SIGHUP, SIG_DFL);
	      close (0);
	      close (1);
	      close (2);
	      close (fd);
	      fd = open (tty, O_RDWR);
	      ioctl (0, TIOCSCTTY, (char *) 1);
	      dup (fd);
	      dup (fd);
	    }
	  else
	    close (fd);
	}
      else
	perror (tty);
    }

  /*
   *      Get the root password.
   */
  pwd = getrootpwent ();

  /*
   *      Ask for the password.
   */
  while (1)
    {
      if ((p = getpasswd ()) == NULL)
	break;
      if (pwd->pw_passwd[0] == 0 ||
	  strcmp (crypt (p, pwd->pw_passwd), pwd->pw_passwd) == 0)
	sushell (pwd);
      printf ("Login incorrect.\n");
    }

  /*
   *      User gave Control-D.
   */
  return 0;
}
