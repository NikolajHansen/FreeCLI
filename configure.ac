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

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
