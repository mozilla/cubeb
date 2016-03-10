AUTOMAKE_OPTIONS = foreign 1.10 dist-bzip2 subdir-objects
ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_builddir)/include -I. -I$(top_srcdir)/src
AM_CFLAGS = -ansi -std=gnu99 -Wall -Wextra -Wno-long-long -O2 -g -Wno-unused-parameter
AM_CXXFLAGS = -std=c++11

SUBDIRS = docs

EXTRA_DIST = \
	AUTHORS README LICENSE \
	cubeb-uninstalled.pc.in \
	m4/as-ac-expand.m4 \
	m4/pkg.m4

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = cubeb.pc

cubebincludedir = $(includedir)/cubeb
cubebinclude_HEADERS = include/cubeb/cubeb.h

lib_LTLIBRARIES = src/libcubeb.la

src_libcubeb_la_SOURCES = src/cubeb.c src/cubeb_panner.cpp

if PULSE
src_libcubeb_la_SOURCES += src/cubeb_pulse.c
endif
if JACK
src_libcubeb_la_SOURCES += src/cubeb_jack.cpp
endif
if ALSA
src_libcubeb_la_SOURCES += src/cubeb_alsa.c
endif
if AUDIOUNIT
src_libcubeb_la_SOURCES += src/cubeb_audiounit.c src/cubeb_osx_run_loop.c
endif
if AUDIOQUEUE
src_libcubeb_la_SOURCES += src/cubeb_audioqueue.c
endif
if WASAPI
src_libcubeb_la_SOURCES += src/cubeb_wasapi.cpp
endif
if WINMM
src_libcubeb_la_SOURCES += src/cubeb_winmm.c
endif
if DIRECTSOUND
src_libcubeb_la_SOURCES += src/cubeb_directsound.cpp
endif
if SNDIO
src_libcubeb_la_SOURCES += src/cubeb_sndio.c
endif
if OPENSL
src_libcubeb_la_SOURCES += src/cubeb_opensl.c
endif
if AUDIOTRACK
src_libcubeb_la_SOURCES += src/cubeb_audiotrack.c
endif
if KAI
src_libcubeb_la_SOURCES += src/cubeb_kai.c
endif

src_libcubeb_la_SOURCES += src/cubeb_resampler.cpp src/speex/resample.c

EXTRA_src_libcubeb_la_SOURCES = \
	src/cubeb.c \
	src/cubeb_jack.cpp \
	src/cubeb_alsa.c src/cubeb_pulse.c \
	src/cubeb_audiounit.c src/cubeb_audioqueue.c \
	src/cubeb_sndio.c src/cubeb_directsound.cpp \
	src/cubeb_winmm.c src/cubeb_wasapi.cpp src/speex/resample.c \
	src/cubeb_opensl.c src/cubeb_audiotrack.c \
	$(NULL)

src_libcubeb_la_LDFLAGS = -export-symbols-regex '^cubeb_' $(platform_lib) -no-undefined

check_PROGRAMS = test/test_sanity \
								 test/test_tone \
								 test/test_audio \
								 test/test_latency \
								 test/test_devices \
								 test/test_duplex \
								 test/test_resampler \
								 test/test_record \
								 test/test_utils \
								 test/test_ring_array\
								 $(NULL)

test_test_sanity_SOURCES = test/test_sanity.cpp
test_test_sanity_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_tone_SOURCES = test/test_tone.cpp
test_test_tone_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_audio_SOURCES = test/test_audio.cpp
test_test_audio_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_latency_SOURCES = test/test_latency.cpp
test_test_latency_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_devices_SOURCES = test/test_devices.cpp
test_test_devices_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_resampler_SOURCES = test/test_resampler.cpp
test_test_resampler_LDADD = -lm src/libcubeb.la $(platform_lib) src/cubeb_resampler.o src/speex/resample.lo

test_test_duplex_SOURCES = test/test_duplex.cpp
test_test_duplex_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_record_SOURCES = test/test_record.cpp
test_test_record_LDADD = -lm src/libcubeb.la $(platform_lib)

test_test_utils_SOURCES = test/test_utils.cpp

test_test_ring_array_SOURCES = test/test_ring_array.c

TESTS = $(check_PROGRAMS)

dist-hook:
	find $(distdir) -type d -name '.git' | xargs rm -rf

debug:
	$(MAKE) all CFLAGS="@DEBUG@"

profile:
	$(MAKE) all CFLAGS="@PROFILE@"
