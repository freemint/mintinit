/*
 * init.h	Several defines and declarations to be
 *		included by all modules of the init program.
 *
 * Version:	@(#)init.h  2.74  09-Mar-1998  miquels@cistron.nl
 *
 * Modified:	Re-exec patch; 24 Feb 1998, Al Viro.
 */

/* Standard configuration */
#define CHANGE_WAIT 0			/* Change runlevel while
					   waiting for a process to exit? */
/* Debug and test modes */
#define DEBUG	   0			/* Debug code off */
#define INITDEBUG  0			/* Fork at startup to debug init. */

/* Some constants */
#define INITPID	   1			/* pid of first process */
#define PIPE_FD    10			/* Fileno of initfifo. */
#define STATE_PIPE 11			/* used to pass state through exec */

/* Failsafe configuration */
#define MAXSPAWN   10			/* Max times respawned in.. */
#define TESTTIME   120			/* this much seconds */
#define SLEEPTIME  300			/* Disable time */

/* Default path inherited by every child if it's not set. */
#define PATH_DFL   "PATH=/usr/local/sbin:/sbin:/bin:/usr/sbin:/usr/bin"


/* Prototypes. */
void write_utmp_wtmp(char *user, char *id, int pid, int type, char *line);
void write_wtmp(char *user, char *id, int pid, int type, char *line);
void log(int loglevel, char *fmt, ...);
void set_term(int how);
void print(char *fmt);

/* Actions to be taken by init */
#define RESPAWN			1
#define WAIT			2
#define ONCE			3
#define	BOOT			4
#define BOOTWAIT		5
#define POWERFAIL		6
#define POWERWAIT		7
#define POWEROKWAIT		8
#define CTRLALTDEL		9
#define OFF		       10
#define	ONDEMAND	       11
#define	INITDEFAULT	       12
#define SYSINIT		       13
#define POWERFAILNOW           14
#define KBREQUEST               15

/* Information about a process in the in-core inittab */
typedef struct _child_ {
  int flags;			/* Status of this entry */
  int exstat;			/* Exit status of process */
  int pid;			/* Pid of this process */
  time_t tm;			/* When respawned last */
  int count;			/* Times respawned in the last 2 minutes */
  char id[8];			/* Inittab id (must be unique) */
  char rlevel[12];		/* run levels */
  int action;			/* what to do (see list below) */
  char process[128];		/* The command line */
  struct _child_ *new;		/* New entry (after inittab re-read) */
  struct _child_ *next;		/* For the linked list */
} CHILD;

/* Values for the 'flags' field */
#define RUNNING			2	/* Process is still running */
#define KILLME			4	/* Kill this process */
#define DEMAND			8	/* "runlevels" a b c */
#define FAILING			16	/* process respawns rapidly */
#define WAITING			32	/* We're waiting for this process */
#define ZOMBIE			64	/* This process is already dead */
#define XECUTED		128	/* Set if spawned once or more times */

/* Log levels. */
#define L_CO	1		/* Log on the console. */
#define L_SY	2		/* Log with syslog() */
#define L_VB	(L_CO|L_SY)	/* Log with both. */

#ifndef NO_PROCESS
#  define NO_PROCESS 0
#endif

/*
 *	Global variables.
 */
extern CHILD *family;
extern int wrote_reboot;

/* Tokens in state parser */
#define C_VER		1
#define	C_END		2
#define C_REC		3
#define	C_EOR		4
#define	C_LEV		5
#define C_FLAG		6
#define	C_ACTION	7
#define C_PROCESS	8
#define C_PID		9
#define C_EXS	       10
#define C_EOF          -1
#define D_RUNLEVEL     -2
#define D_THISLEVEL    -3
#define D_PREVLEVEL    -4
#define D_GOTSIGN      -5
#define D_WROTE_REBOOT -6
#define D_SLTIME       -7
#define D_DIDBOOT      -8

