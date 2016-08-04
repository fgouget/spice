AC_PREREQ([2.57])

# Follow the libtool manual for the so version:
# http://www.gnu.org/software/libtool/manual/html_node/Updating-version-info.html
# - If the library source code has changed at all since the last update,
#   then increment revision (‘c:r:a’ becomes ‘c:r+1:a’).
# - If any interfaces have been added, removed, or changed since the last update,
#   increment current, and set revision to 0.
# - If any interfaces have been added since the last public release,
#   then increment age.
# - If any interfaces have been removed or changed since the last public release,
#   then set age to 0.
#
dnl TODO see server/spice-char.h TODO comment for API breakage
m4_define([SPICE_CURRENT], [13])
m4_define([SPICE_REVISION], [5])
m4_define([SPICE_AGE], [12])

AC_INIT(spice, [m4_esyscmd(build-aux/git-version-gen .tarball-version)],
        [spice-devel@lists.freedesktop.org], spice)

major=`echo $PACKAGE_VERSION | cut -d. -f1`
minor=`echo $PACKAGE_VERSION | cut -d. -f2`
micro=`echo $PACKAGE_VERSION | cut -d. -f3`
git=`echo $PACKAGE_VERSION | cut -d. -f4`
if test x"$git" != x ; then
    micro=$(($micro+1))
fi
SPICE_SERVER_VERSION=`printf "0x%02x%02x%02x" $major $minor $micro`
AC_SUBST(SPICE_SERVER_VERSION)

AC_CONFIG_MACRO_DIR([m4])
m4_include([subprojects/spice-common/m4/spice-deps.m4])
AM_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR(.)

AM_INIT_AUTOMAKE([dist-bzip2 no-dist-gzip subdir-objects])
AM_MAINTAINER_MODE

AC_PROG_CC
AC_PROG_CC_C99
if test x"$ac_cv_prog_cc_c99" = xno; then
    AC_MSG_ERROR([C99 compiler is required.])
fi
AC_PROG_INSTALL
AC_CANONICAL_HOST
LT_INIT([disable-static])
AM_PROG_CC_C_O
AC_C_BIGENDIAN
PKG_PROG_PKG_CONFIG

AC_CHECK_HEADERS([sys/time.h execinfo.h linux/sockios.h pthread_np.h])
AC_CHECK_DECL([TCP_KEEPIDLE], [have_tcp_keepidle="yes"],,
              [#include <netinet/tcp.h>])
AS_IF([test "x$have_tcp_keepidle" = "xyes"],
      [AC_DEFINE([HAVE_TCP_KEEPIDLE],1,[Define to 1 if <netinet/tcp.h> has a TCP_KEEPIDLE definition])],
)
AC_FUNC_ALLOCA

SPICE_LT_VERSION=m4_format("%d:%d:%d", SPICE_CURRENT, SPICE_REVISION, SPICE_AGE)
AC_SUBST(SPICE_LT_VERSION)

# Check for the CPU we are using
case $host_cpu in
  x86_64)
        ;;
  *)
        SPICE_WARNING([spice-server on non-x86_64 architectures has not been extensively tested])
esac

dnl =========================================================================
dnl Check optional features
SPICE_CHECK_SMARTCARD
SPICE_EXTRA_CHECKS

AC_ARG_ENABLE(gstreamer,
              AS_HELP_STRING([--enable-gstreamer=@<:@auto/0.10/1.0/yes/no@:>@],
                             [Enable GStreamer support]),,
              [enable_gstreamer="auto"])

if test "x$enable_gstreamer" != "xno" && test "x$enable_gstreamer" != "x0.10"; then
    SPICE_CHECK_GSTREAMER(GSTREAMER_1_0, 1.0, [gstreamer-1.0 gstreamer-base-1.0 gstreamer-app-1.0 gstreamer-video-1.0],
        [enable_gstreamer="1.0"
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0, [gst-plugins-base 1.0], [appsrc videoconvert appsink])
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0, [gstreamer-libav 1.0], [avenc_mjpeg])
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0, [gst-plugins-good 1.0], [vp8enc vp9enc])
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_1_0, [gst-plugins-ugly 1.0], [x264enc])
         ],
         [if test "x$enable_gstreamer" = "x1.0"; then
              AC_MSG_ERROR([GStreamer 1.0 support requested but not found. You may set GSTREAMER_1_0_CFLAGS and GSTREAMER_1_0_LIBS to avoid the need to call pkg-config.])
          fi
    ])
fi

if test "x$enable_gstreamer" != "xno" && test "x$enable_gstreamer" != "x1.0"; then
    SPICE_CHECK_GSTREAMER(GSTREAMER_0_10, 0.10, [gstreamer-0.10 gstreamer-base-0.10 gstreamer-app-0.10 gstreamer-video-0.10],
        [enable_gstreamer="0.10"
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_0_10, [gst-plugins-base 0.10], [appsrc appsink])
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_0_10, [gstreamer-ffmpeg 0.10], [ffmpegcolorspace ffenc_mjpeg])
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_0_10, [gst-plugins-bad 0.10], [vp8enc])
         SPICE_CHECK_GSTREAMER_ELEMENTS($GST_INSPECT_0_10, [gst-plugins-ugly 0.10], [x264enc])
        ],
        [if test "x$enable_gstreamer" = "x0.10"; then
             AC_MSG_ERROR([GStreamer 0.10 support requested but not found. You may set GSTREAMER_0_10_CFLAGS and GSTREAMER_0_10_LIBS to avoid the need to call pkg-config.])
         fi
    ])
fi
AM_CONDITIONAL(HAVE_GSTREAMER, test "x$have_gstreamer_0_10" = "xyes" || test "x$have_gstreamer_1_0" = "xyes")
AM_CONDITIONAL(HAVE_GSTREAMER_1_0, test "x$have_gstreamer_1_0" = "xyes")

AS_IF([test "x$enable_gstreamer" = "xyes"],
      [AC_MSG_ERROR("GStreamer support requested but not found")],
      [test "x$enable_gstreamer" = "xauto"],
      [enable_gstreamer="no"
])
AS_IF([test x"$missing_gstreamer_elements" = xyes],
    [SPICE_WARNING([The GStreamer video encoder can be built but may not work.])
])

if test "x$have_gstreamer_0_10" = "xyes" || test "x$have_gstreamer_1_0" = "xyes"; then
    PKG_CHECK_MODULES(ORC, orc-0.4)
    AC_SUBST(ORC_CFLAGS)
    AC_SUBST(ORC_LIBS)
fi

dnl Check for the presence of Valgrind and do the plumbing to allow
dnl the running of "make check-valgrind".
AX_VALGRIND_DFLT(memcheck, on)
AX_VALGRIND_DFLT(helgrind, off)
AX_VALGRIND_DFLT(drd, off)
AX_VALGRIND_DFLT(sgcheck, off)

AX_VALGRIND_CHECK

SPICE_CHECK_LZ4
SPICE_CHECK_SASL
SPICE_CHECK_RECORDER
AM_CONDITIONAL(HAVE_SASL, test "x$have_sasl" = "xyes")

dnl =========================================================================
dnl Check deps
AC_CONFIG_SUBDIRS([subprojects/spice-common])
COMMON_CFLAGS='-I ${top_srcdir}/subprojects/spice-common/ -I ${top_builddir}/subprojects/spice-common/'
COMMON_CFLAGS="$COMMON_CFLAGS -DG_LOG_DOMAIN=\\\"Spice\\\""
AC_SUBST(COMMON_CFLAGS)

SPICE_COMMON_DIR='${top_builddir}/subprojects/spice-common'
AC_SUBST(SPICE_COMMON_DIR)

AC_CHECK_LIBM
AC_SUBST(LIBM)

AC_CHECK_LIB(rt, clock_gettime, LIBRT="-lrt")
AC_SUBST(LIBRT)

AS_VAR_APPEND([SPICE_NONPKGCONFIG_LIBS], [" -pthread $LIBM $LIBRT"])

SPICE_REQUIRES=""

AS_IF([test x"$have_smartcard" = "xyes"], [
    AS_VAR_APPEND([SPICE_REQUIRES], [" libcacard >= 0.1.2"])
])

SPICE_PROTOCOL_MIN_VER=0.12.16
PKG_CHECK_MODULES([SPICE_PROTOCOL], [spice-protocol >= $SPICE_PROTOCOL_MIN_VER])
AC_SUBST([SPICE_PROTOCOL_MIN_VER])

GLIB2_REQUIRED=2.38
GLIB2_ENCODED_VERSION="GLIB_VERSION_2_38"
PKG_CHECK_MODULES([GLIB2], [glib-2.0 >= $GLIB2_REQUIRED gio-2.0 >= $GLIB2_REQUIRED])
GLIB2_CFLAGS="$GLIB2_CFLAGS -DGLIB_VERSION_MIN_REQUIRED=$GLIB2_ENCODED_VERSION \
  -DGLIB_VERSION_MAX_ALLOWED=$GLIB2_ENCODED_VERSION"
AS_VAR_APPEND([SPICE_REQUIRES], [" glib-2.0 >= $GLIB2_REQUIRED gio-2.0 >= $GLIB2_REQUIRED"])

PKG_CHECK_MODULES([GOBJECT2], [gobject-2.0 >= $GLIB2_REQUIRED])
AS_VAR_APPEND([SPICE_REQUIRES], [" gobject-2.0 >= $GLIB2_REQUIRED"])

#used only by tests
PKG_CHECK_MODULES([GIO_UNIX], [gio-unix-2.0 >= $GLIB2_REQUIRED])

PIXMAN_REQUIRED=0.17.7
PKG_CHECK_MODULES(PIXMAN, pixman-1 >= $PIXMAN_REQUIRED)
AC_SUBST(PIXMAN_CFLAGS)
AC_SUBST(PIXMAN_LIBS)
AS_VAR_APPEND([SPICE_REQUIRES], [" pixman-1 >= $PIXMAN_REQUIRED"])

SPICE_CHECK_CELT051

PKG_CHECK_MODULES(SSL, openssl >= 1.0.0)
AC_SUBST(SSL_CFLAGS)
AC_SUBST(SSL_LIBS)
AS_VAR_APPEND([SPICE_REQUIRES], [" openssl"])

PKG_CHECK_MODULES(EPOXY, epoxy)
AC_SUBST(EPOXY_CFLAGS)
AC_SUBST(EPOXY_LIBS)

AC_CHECK_LIB(jpeg, jpeg_destroy_decompress,
    AC_MSG_CHECKING([for jpeglib.h])
    AC_TRY_CPP(
[#include <stdio.h>
#undef PACKAGE
#undef VERSION
#undef HAVE_STDLIB_H
#include <jpeglib.h>],
        JPEG_LIBS='-ljpeg'
        AC_MSG_RESULT($jpeg_ok),
	AC_MSG_ERROR([jpeglib.h not found])),
    AC_MSG_ERROR([libjpeg not found]))
AC_SUBST(JPEG_LIBS)

AC_CHECK_LIB(z, deflate, Z_LIBS='-lz', AC_MSG_ERROR([zlib not found]))
AC_SUBST(Z_LIBS)


AC_ARG_ENABLE([manual],
               AS_HELP_STRING([--enable-manual=@<:@auto/yes/no@:>@],
                              [Build SPICE manual]),
               [],
               [enable_manual="auto"])
if test "x$enable_manual" != "xno"; then
    AC_PATH_PROG([ASCIIDOC], [asciidoc])
    AS_IF([test -z "$ASCIIDOC" && test "x$enable_manual" = "xyes"],
          [AC_MSG_ERROR([asciidoc is missing and build of manual was requested])])
    AC_PATH_PROG([A2X], [a2x])
    AS_IF([test -z "$A2X" && test "x$enable_manual" = "xyes"],
          [AC_MSG_ERROR([a2x is missing and build of manual was requested])])
fi
AS_IF([test -n "$ASCIIDOC"], [have_asciidoc=yes], [have_asciidoc=no])
AM_CONDITIONAL([BUILD_MANUAL], [test -n "$ASCIIDOC" || test -n "$A2X"])
AM_CONDITIONAL([BUILD_HTML_MANUAL], [test -n "$ASCIIDOC"])
AM_CONDITIONAL([BUILD_CHUNKED_MANUAL], [test -n "$A2X"])

AC_ARG_ENABLE([statistics],
               AS_HELP_STRING([--enable-statistics=@<:@yes/no@:>@],
                              [Build SPICE with statistic code enabled @<:@default=no@:>@]))
AS_IF([test "$enable_statistics" = "yes"],
      [AC_DEFINE([RED_STATISTICS], [1], [Enable SPICE statistics])])

dnl ===========================================================================
dnl check compiler flags

SPICE_COMPILE_WARNINGS
LIBVIRT_LINKER_RELRO
LIBVIRT_LINKER_NO_INDIRECT

AC_SUBST(WARN_CFLAGS)

dnl =========================================================================
dnl -fvisibility stuff

have_gcc4=no
AC_MSG_CHECKING(for -fvisibility)
AC_COMPILE_IFELSE([AC_LANG_SOURCE([
#if defined(__GNUC__) && (__GNUC__ >= 4)
#else
error Need GCC 4.0 for visibility
#endif
int main () { return 0; }
])], have_gcc4=yes)

if test "x$have_gcc4" = "xyes" && test ! "$os_win32" = "yes" ; then
   VISIBILITY_HIDDEN_CFLAGS="-fvisibility=hidden"
fi
AC_MSG_RESULT($have_gcc4)
AC_SUBST(VISIBILITY_HIDDEN_CFLAGS)

dnl ensure linker supports ---version-script option before using it
AC_CACHE_CHECK([if -Wl,--version-script works], [spice_cv_ld_version_script],
    [save_LDFLAGS="$LDFLAGS"
     LDFLAGS="$LDFLAGS -Wl,--version-script=$srcdir/server/spice-server.syms"
     AC_LINK_IFELSE([AC_LANG_PROGRAM([], [])],
        [spice_cv_ld_version_script=yes], [spice_cv_ld_version_script=no])
     LDFLAGS="$save_LDFLAGS"])
AM_CONDITIONAL([HAVE_LD_VERSION_SCRIPT],
    [test x"$spice_cv_ld_version_script" = xyes])

AC_DEFINE([SPICE_USE_SAFER_CONTAINEROF], [1], [Enable more type safe version of SPICE_CONTAINEROF])

AC_SUBST(SPICE_REQUIRES)
AC_SUBST(SPICE_NONPKGCONFIG_CFLAGS)
AC_SUBST(SPICE_NONPKGCONFIG_LIBS)

m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])])

AC_OUTPUT([
Makefile
spice-server.pc
server/Makefile
server/spice-version.h
server/tests/Makefile
docs/Makefile
docs/manual/Makefile
tools/Makefile
])

dnl ==========================================================================
AC_MSG_NOTICE([

        Spice $VERSION
        ==============

        prefix:                   ${prefix}
        C compiler:               ${CC}

        LZ4 support:              ${have_lz4}
        Smartcard:                ${have_smartcard}
        GStreamer:                ${enable_gstreamer}
        SASL support:             ${have_sasl}
        Manual:                   ${have_asciidoc}

        Now type 'make' to build $PACKAGE
])
SPICE_PRINT_MESSAGES
