AC_PREREQ([2.69])
AC_INIT([FreeCLI], [0.1.0], [your@email.com])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_CONFIG_SRCDIR([src/ui/main.c])

# Checks for programs
AC_PROG_CC
AC_PROG_INSTALL

# Checks for libraries
AC_CHECK_LIB([ncurses], [initscr], [], [
  AC_MSG_ERROR([ncurses library not found])
])
AC_CHECK_LIB([menu], [new_menu], [], [
  AC_MSG_ERROR([menu library not found])
])
AC_CHECK_LIB([form], [new_form], [], [
  AC_MSG_ERROR([form library not found])
])
AC_CHECK_LIB([panel], [new_panel], [], [
  AC_MSG_ERROR([panel library not found])
])

# Checks for header files
AC_CHECK_HEADERS([ncurses.h menu.h form.h panel.h])

# ZeroMQ — comm bus transport (required)
PKG_CHECK_MODULES([ZMQ], [libzmq >= 4.0.0], [],
  [AC_MSG_ERROR([libzmq >= 4.0.0 not found. Install libzmq3-dev (Debian/Ubuntu) or zeromq (FreeBSD/macOS)])])

# Optional: Check unit test framework
PKG_CHECK_MODULES([CHECK], [check >= 0.9.6],
  [have_check=yes],
  [have_check=no; AC_MSG_WARN([Check not found — unit tests disabled])])
AM_CONDITIONAL([HAVE_CHECK], [test "x$have_check" = "xyes"])

AC_CONFIG_FILES([Makefile src/Makefile tests/Makefile])
AC_OUTPUT
