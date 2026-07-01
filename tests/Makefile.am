if HAVE_CHECK

AM_CPPFLAGS = -I$(top_srcdir)/src/core

TESTS = test_utf8
check_PROGRAMS = test_utf8

test_utf8_SOURCES = test_utf8.c $(top_srcdir)/src/core/utf8.c
test_utf8_CFLAGS  = $(CHECK_CFLAGS)
test_utf8_LDADD   = $(CHECK_LIBS)

endif
