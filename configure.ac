dnl ------------------------------------------------
dnl Initialization and Versioning
dnl ------------------------------------------------

AC_INIT(libcubeb,[0.1git])

AC_CANONICAL_HOST
AC_CANONICAL_TARGET

AC_CONFIG_MACRO_DIR([m4])

AM_CONFIG_HEADER([config.h])
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
CFLAGS="$cflags_save"

AM_PROG_CC_C_O
AC_LIBTOOL_WIN32_DLL
AM_PROG_LIBTOOL

dnl Choose the target platform audio API
AC_CHECK_HEADERS([alsa/asoundlib.h pulse/pulseaudio.h \
                  AudioToolbox/AudioToolbox.h \
                  windows.h dsound.h sndio.h])
platform_api="none"
platform_lib="none"
if test "x$ac_cv_header_pulse_pulseaudio_h" = "xyes"; then
  platform_api="pulse"
  platform_lib="-lpulse"
elif test "x$ac_cv_header_alsa_asoundlib_h" = "xyes"; then
  platform_api="alsa"
  platform_lib="-lasound"
elif test "x$ac_cv_header_AudioToolbox_AudioToolbox_h" = "xyes"; then
  platform_api="coreaudio"
  platform_lib="-framework AudioToolbox"
elif test "x$ac_cv_header_sndio_h" = "xyes"; then
  platform_api="sndio"
  platform_lib="-lsndio -lm"
elif test "x$ac_cv_header_windows_h" = "xyes"; then
#  if test "x$ac_cv_header_dsound_h" = "xyes"; then
#    platform_api="dsound"
#  else
    platform_api="wave"
#  fi
fi
AM_CONDITIONAL([PULSE], [test ${platform_api} = "pulse"])
AM_CONDITIONAL([ALSA], [test ${platform_api} = "alsa"])
AM_CONDITIONAL([COREAUDIO], [test ${platform_api} = "coreaudio"])
AM_CONDITIONAL([DSOUND], [test ${platform_api} = "dsound"])
AM_CONDITIONAL([WAVE], [test ${platform_api} = "wave"])
AM_CONDITIONAL([SNDIO], [test ${platform_api} = "sndio"])

dnl Check for doxygen
AC_ARG_ENABLE([doc],
	AS_HELP_STRING([--enable-doc], [Build API documentation]),
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

AC_MSG_RESULT([
------------------------------------------------------------------------
  $PACKAGE $VERSION:  Automatic configuration OK.

  General configuration:

    Platform API target: ........ ${platform_api}
    API Documentation: .......... ${doc_build}

  Installation paths:

    libcubeb: .................... ${LIBDIR}
    C header files: .............. ${INCLUDEDIR}/cubeb
    Documentation: ............... ${DOCDIR}

  Building:

    Type 'make' to compile $PACKAGE.

    Type 'make install' to install $PACKAGE.

  Example programs will be built but not installed.
------------------------------------------------------------------------
])

