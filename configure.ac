dnl ------------------------------------------------
dnl Initialization and Versioning
dnl ------------------------------------------------

AC_INIT(libcubeb,[0.3git])

AC_CANONICAL_HOST
AC_CANONICAL_TARGET

AC_CONFIG_MACRO_DIR([m4])

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([src/cubeb_pulse.c])
AM_INIT_AUTOMAKE

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl Library versioning
dnl CURRENT, REVISION, AGE
dnl - library source changed -> increment REVISION
dnl - interfaces added/removed/changed -> increment CURRENT, REVISION = 0
dnl - interfaces added -> increment AGE
dnl - interfaces removed -> AGE = 0

CUBEB_CURRENT=0
CUBEB_REVISION=0
CUBEB_AGE=1
AC_SUBST(CUBEB_CURRENT)
AC_SUBST(CUBEB_REVISION)
AC_SUBST(CUBEB_AGE)


dnl --------------------------------------------------
dnl Check for programs
dnl --------------------------------------------------

dnl save $CFLAGS since AC_PROG_CC likes to insert "-g -O2"
dnl if $CFLAGS is blank
cflags_save="$CFLAGS"
AC_PROG_CC
AC_PROG_CPP
AC_PROG_CXX
CFLAGS="$cflags_save"

AM_PROG_CC_C_O
AC_LIBTOOL_WIN32_DLL
AM_PROG_LIBTOOL

AC_ARG_WITH([pulse],
            AS_HELP_STRING([--with-pulse], [with PulseAudio @<:@default=check@:>@]))
AS_IF([test "x$with_pulse" != xno],
      [PKG_CHECK_MODULES(PULSE, libpulse >= 0.9.16, [
       HAVE_PULSE=1
       PULSE_LIBS="-ldl"
       AC_DEFINE([USE_PULSE], [], [Use PulseAudio])
      ], [
       HAVE_PULSE=0
       AS_IF([test "x$with_pulse" = xyes], [AC_MSG_ERROR(PulseAudio not detected)])])])
AM_CONDITIONAL([PULSE], [test $HAVE_PULSE -eq 1])

AC_ARG_WITH([jack],
            AS_HELP_STRING([--with-jack], [with JACK @<:@default=check@:>@]))
AS_IF([test "x$with_jack" != xno],
      [PKG_CHECK_MODULES(JACK, jack >= 1.9.9, [
       HAVE_JACK=1
       NEED_SPEEX=1
       AC_DEFINE([USE_JACK], [], [Use JACK])
      ], [
       HAVE_JACK=0
       AS_IF([test "x$with_jack" = xyes], [AC_MSG_ERROR(JACK not detected)])])])
AM_CONDITIONAL([JACK], [test $HAVE_JACK -eq 1])

AC_ARG_WITH([alsa],
            AS_HELP_STRING([--with-alsa], [with ALSA @<:@default=check@:>@]))
AS_IF([test "x$with_alsa" != xno],
      [PKG_CHECK_MODULES(ALSA, alsa >= 1.0.17, [
       HAVE_ALSA=1
       AC_DEFINE([USE_ALSA], [], [Use ALSA])
      ], [
       HAVE_ALSA=0
       AS_IF([test "x$with_alsa" = xyes], [AC_MSG_ERROR(ALSA not detected)])])])
AM_CONDITIONAL([ALSA], [test $HAVE_ALSA -eq 1])

AC_ARG_WITH([audiounit],
            AS_HELP_STRING([--with-audiounit], [with AudioUnit @<:@default=check@:>@]))
AS_IF([test "x$with_audiounit" != xno],
      [AC_CHECK_HEADER([AudioUnit/AudioUnit.h], [
       HAVE_AUDIOUNIT=1
       AUDIOUNIT_LIBS="-framework CoreServices -framework AudioUnit -framework CoreAudio"
       AC_DEFINE([USE_AUDIOUNIT], [], [Use AudioUnit])
      ], [
       HAVE_AUDIOUNIT=0
       AS_IF([test "x$with_audiounit" = xyes], [AC_MSG_ERROR(AudioUnit not detected)])])])
AM_CONDITIONAL([AUDIOUNIT], [test $HAVE_AUDIOUNIT -eq 1])

AC_ARG_WITH([audioqueue],
            AS_HELP_STRING([--with-audioqueue], [with AudioQueue @<:@default=check@:>@]))
AS_IF([test "x$with_audioqueue" != xno],
      [AC_CHECK_HEADER([AudioToolbox/AudioToolbox.h], [
       HAVE_AUDIOQUEUE=1
       AUDIOQUEUE_LIBS="-framework CoreServices -framework AudioToolbox"
       AC_DEFINE([USE_AUDIOQUEUE], [], [Use AudioQueue])
      ], [
       HAVE_AUDIOQUEUE=0
       AS_IF([test "x$with_audioqueue" = xyes], [AC_MSG_ERROR(AudioQueue not detected)])])])
AM_CONDITIONAL([AUDIOQUEUE], [test $HAVE_AUDIOQUEUE -eq 1])

AC_ARG_WITH([sndio],
            AS_HELP_STRING([--with-sndio], [with sndio @<:@default=check@:>@]))
AS_IF([test "x$with_sndio" != xno],
      [AC_CHECK_HEADER([sndio.h], [
       HAVE_SNDIO=1
       SNDIO_LIBS="-lsndio"
       AC_DEFINE([USE_SNDIO], [], [Use sndio])
      ], [
       HAVE_SNDIO=0
       AS_IF([test "x$with_sndio" = xyes], [AC_MSG_ERROR(sndio not detected)])])])
AM_CONDITIONAL([SNDIO], [test $HAVE_SNDIO -eq 1])

AC_ARG_WITH([opensl],
            AS_HELP_STRING([--with-opensl], [with OpenSL @<:@default=check@:>@]))
AS_IF([test "x$with_opensl" != xno],
      [AC_CHECK_HEADER([SLES/OpenSLES.h], [
       HAVE_OPENSL=1
       AC_DEFINE([USE_OPENSL], [], [Use OpenSL])
      ], [
       HAVE_OPENSL=0
       AS_IF([test "x$with_opensl" = xyes], [AC_MSG_ERROR(OpenSL not detected)])])])
AM_CONDITIONAL([OPENSL], [test $HAVE_OPENSL -eq 1])

AC_ARG_WITH([audiotrack],
            AS_HELP_STRING([--with-audiotrack], [with OpenSL @<:@default=check@:>@]))
AS_IF([test "x$with_audiotrack" != xno],
      [AC_CHECK_HEADER([android/log.h], [
       HAVE_AUDIOTRACK=1
       AC_DEFINE([USE_AUDIOTRACK], [], [Use AudioTrack])
      ], [
       HAVE_AUDIOTRACK=0
       AS_IF([test "x$with_audiotrack" = xyes], [AC_MSG_ERROR(AudioTrack not detected)])])])
AM_CONDITIONAL([AUDIOTRACK], [test $HAVE_AUDIOTRACK -eq 1])

#AC_ARG_WITH([directsound],
#            AS_HELP_STRING([--with-directsound], [with DirectSound @<:@default=check@:>@]))
#AS_IF([test "x$with_directsound" != xno],
#      [AC_CHECK_HEADER([dsound.h], [
#       HAVE_DIRECTSOUND=1
#       DIRECTSOUND_LIBS="-ldsound"
#       AC_DEFINE([USE_DIRECTSOUND], [], [Use DirectSound])
#      ], [
       HAVE_DIRECTSOUND=0
#       AS_IF([test "x$with_directsound" = xyes], [AC_MSG_ERROR(DirectSound not detected)])
#      ], [#include <windows.h>])])
AM_CONDITIONAL([DIRECTSOUND], [test $HAVE_DIRECTSOUND -eq 1])

AC_ARG_WITH([wasapi],
            AS_HELP_STRING([--with-wasapi], [with WASAPI @<:@default=check@:>@]))
AS_IF([test "x$with_wasapi" != xno],
      [AC_CHECK_HEADERS([audioclient.h], [
       HAVE_WASAPI=1
       NEED_SPEEX=1
       AC_DEFINE([USE_WASAPI], [], [Use WASAPI])
       WASAPI_LIBS="-lole32"
      ], [
       HAVE_WASAPI=0
       AS_IF([test "x$with_wasapi" = xyes], [AC_MSG_ERROR(WASAPI not detected)])
      ], [#include <windows.h>])])
AM_CONDITIONAL([WASAPI], [test $HAVE_WASAPI -eq 1])

AC_ARG_WITH([winmm],
            AS_HELP_STRING([--with-winmm], [with WinMM @<:@default=check@:>@]))
AS_IF([test "x$with_winmm" != xno],
      [AC_CHECK_HEADERS([mmsystem.h], [
       HAVE_WINMM=1
       DIRECTSOUND_LIBS="-lwinmm"
       AC_DEFINE([USE_WINMM], [], [Use WinMM])
      ], [
       HAVE_WINMM=0
       AS_IF([test "x$with_winmm" = xyes], [AC_MSG_ERROR(WinMM not detected)])
      ], [#include <windows.h>])])
AM_CONDITIONAL([WINMM], [test $HAVE_WINMM -eq 1])

AM_CONDITIONAL([SPEEX], [test $NEED_SPEEX -eq 1])

AS_IF([test $NEED_SPEEX -eq 1],
      [
       AC_DEFINE([OUTSIDE_SPEEX], [], [Tell the speex resampler no to expect speex headers.])
       AC_DEFINE([RANDOM_PREFIX], [speex], [Required by the speex resampler.])
       AC_DEFINE([FLOATING_POINT], [], [Tell the resampler to only enable floating point support])
       AC_DEFINE([EXPORT], [], [Tell the resampler to export the symbols by default.])
      ])

platform_lib="$PULSE_LIBS $JACK_LIBS $ALSA_LIBS $AUDIOUNIT_LIBS $AUDIOQUEUE_LIBS $SNDIO_LIBS $OPENSL_LIBS $DIRECTSOUND_LIBS $WINMM_LIBS $WASAPI_LIBS"

dnl Check for doxygen
AC_ARG_ENABLE([doc],
	AS_HELP_STRING([--enable-doc], [build API documentation]),
	[ac_enable_doc=$enableval], [ac_enable_doc=auto])

if test "x$ac_enable_doc" != "xno"; then
	AC_CHECK_PROG(HAVE_DOXYGEN, doxygen, true, false)

	if test "x$HAVE_DOXYGEN" = "xfalse" -a "x$ac_enable_doc" = "xyes"; then
		AC_MSG_ERROR([*** API documentation explicitly requested but Doxygen not found])
	fi
else
	HAVE_DOXYGEN=false
fi
AM_CONDITIONAL(HAVE_DOXYGEN,$HAVE_DOXYGEN)
if test $HAVE_DOXYGEN = "false"; then
        AC_MSG_WARN([*** doxygen not found, API documentation will not be built])
fi

# Generate portable stdint.h replacement
AX_CREATE_STDINT_H(include/cubeb/cubeb-stdint.h)

# Test whenever ld supports -version-script
AC_PROG_LD
AC_PROG_LD_GNU
AC_MSG_CHECKING([how to control symbol export])

dnl --------------------------------------------------
dnl Do substitutions
dnl --------------------------------------------------

AC_SUBST(DEBUG)
AC_SUBST(PROFILE)
AC_SUBST(platform_lib)

AC_OUTPUT([
  Makefile
  docs/Makefile
  docs/Doxyfile
  cubeb.pc
  cubeb-uninstalled.pc
])

AS_AC_EXPAND(LIBDIR, ${libdir})
AS_AC_EXPAND(INCLUDEDIR, ${includedir})
AS_AC_EXPAND(BINDIR, ${bindir})
AS_AC_EXPAND(DOCDIR, ${docdir})

if test $HAVE_DOXYGEN = "false"; then
  doc_build="no"
else
  doc_build="yes"
fi
