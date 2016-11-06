AC_INIT([autosleep], [0.1], [per.weijnitz@gmail.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign])
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([
 Makefile
])
PKG_PROG_PKG_CONFIG([0.22])
PKG_CHECK_MODULES([GTK], [gtk+-2.0 >= 2.24.10])
PKG_CHECK_MODULES([XI], [xi])
PKG_CHECK_MODULES([X11], [x11])
AC_OUTPUT
