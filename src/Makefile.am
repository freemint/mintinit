## Process this file with automake to create Makefile.in

CFLAGS = -Wall @CFLAGS@
DEFS = -D_GNU_SOURCE @DEFS@

if INSTALLUSR
  sbindir=/sbin
endif

noinst_HEADERS = init.h initreq.h initpaths.h reboot.h set.h
sbin_PROGRAMS = init runlevel halt shutdown killall5 sulogin
bin_PROGRAMS = utmpdump last wall mesg

init_SOURCES = init.c init_utmp.c
halt_SOURCES = halt.c utmp.c ifdown.c
shutdown_SOURCES = dowall.c utmp.c shutdown.c
wall_SOURCES = dowall.c wall.c

LIBS = @LIBS@

INCLUDES = -I$(top_srcdir) -I$(srcdir) -I$(top_srcdir)/lib -I..

install-exec-hook:
	ln -s last $(bindir)/lastb
	ln -s halt $(sbindir)/reboot
	ln -s halt $(sbindir)/poweroff
	ln -s killall5 $(sbindir)/pidof
	ln -s init $(sbindir)/telinit
