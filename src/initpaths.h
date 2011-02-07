/*
 * paths.h	Paths of files that init and related utilities need.
 *
 * Version:	@(#) paths.h 1.71 28-Dec-1995
 *
 * Author:	Miquel van Smoorenburg, <miquels@cistron.nl>
 *
 *		This file is part of the sysvinit suite,
 *		Copyright 1991-1997 Miquel van Smoorenburg.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#define INITLVL		"/etc/initrunlvl"	/* COMPAT: New runlevel for init */
#define INITLVL2	"/var/log/initrunlvl"	/* COMPAT: New runlevel for init */
/* ifdef __MINT__: Hm, /var/run/powerstatus would make more sense to me.  */
#define PWRSTAT		"/etc/powerstatus"	/* COMPAT: SIGPWR reason (OK/BAD) */
#define VT_MASTER	"/dev/tty0"		/* Virtual console master */
#define CONSOLE		"/dev/console"		/* Logical system console */
#define SECURETTY	"/etc/securetty"	/* List of root terminals */
#define SDALLOW		"/etc/shutdown.allow"	/* Users allowed to shutdown */
#define INITTAB		"/etc/inittab"		/* Location of inittab */
#define INIT		"/sbin/init"		/* Location of init itself. */
#define NOLOGIN		"/etc/nologin"		/* Stop user logging in. */
#define FASTBOOT	"/etc/fastboot"		/* Enable fast boot. */
#define FORCEFSCK	"/etc/forcefsck"	/* Force fsck on boot */
#define SDPID		"/var/run/shutdown.pid"	/* PID of shutdown program */
#define IOSAVE		"/etc/ioctl.save"	/* termios settings for SU */
#define SHELL		"/bin/sh"		/* Default shell */
#define INITSCRIPT	"/etc/initscript"	/* Initscript. */
#if 0
#define HALTSCRIPT1	"/etc/init.d/halt"	/* Called by "fast" shutdown */
#define HALTSCRIPT2	"/etc/rc.d/rc.0"	/* Called by "fast" shutdown */
#define REBOOTSCRIPT1	"/etc/init.d/reboot"	/* Ditto. */
#define REBOOTSCRIPT2	"/etc/rc.d/rc.6"	/* Ditto. */
#endif
