AUTOMAKE_OPTIONS = foreign 1.11 no-dist-gzip dist-xz subdir-objects
ACLOCAL_AMFLAGS = -I m4

INCLUDES = -I$(top_srcdir)/include -I.
AM_CFLAGS = -ansi -pedantic -std=c99 -Wall -Wextra -Wno-long-long -O0 -g

SUBDIRS = docs

EXTRA_DIST = \
	AUTHORS README LICENSE \
	cubeb-uninstalled.pc.in \
	m4/as-ac-expand.m4 \
	m4/pkg.m4 \
	m4/ax_create_stdint_h.m4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = cubeb.pc

cubebincludedir = $(includedir)/cubeb
cubebinclude_HEADERS = include/cubeb/cubeb.h include/cubeb/cubeb-stdint.h

lib_LTLIBRARIES = src/libcubeb.la

src_libcubeb_la_SOURCES = \
	src/cubeb_alsa.c

src_libcubeb_la_LDFLAGS = -export-symbols-regex '^cubeb_'

check_PROGRAMS = test/test_sanity

test_test_sanity_SOURCES = test/test_sanity.c
test_test_sanity_LDADD = -lasound -lm src/libcubeb.la

TESTS = test/test_sanity

DISTCLEANFILES = include/cubeb/cubeb-stdint.h

dist-hook:
	find $(distdir) -type d -name '.git' | xargs rm -rf

debug:
	$(MAKE) all CFLAGS="@DEBUG@"

profile:
	$(MAKE) all CFLAGS="@PROFILE@"
