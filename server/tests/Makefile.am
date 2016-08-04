NULL =

@VALGRIND_CHECK_RULES@
VALGRIND_SUPPRESSIONS_FILES = $(srcdir)/valgrind/glib.supp
EXTRA_DIST =				\
	$(VALGRIND_SUPPRESSIONS_FILES)	\
	meson.build			\
	pki/ca-cert.pem			\
	pki/server-cert.pem		\
	pki/server-key.pem		\
	$(NULL)

AM_CPPFLAGS =					\
	-DSPICE_TOP_SRCDIR=\"$(abs_top_srcdir)\"\
	-I$(top_srcdir)				\
	-I$(top_srcdir)/server			\
	-I$(top_builddir)/server		\
	-I$(top_srcdir)/server/tests		\
	$(COMMON_CFLAGS)			\
	$(GIO_UNIX_CFLAGS)			\
	$(GLIB2_CFLAGS)				\
	$(GOBJECT2_CFLAGS)			\
	$(SMARTCARD_CFLAGS)			\
	$(SPICE_NONPKGCONFIG_CFLAGS)		\
	$(SPICE_PROTOCOL_CFLAGS)		\
	$(WARN_CFLAGS)				\
	$(NULL)

noinst_LIBRARIES = libtest.a

libtest_a_SOURCES =				\
	basic-event-loop.c			\
	basic-event-loop.h			\
	test-display-base.c			\
	test-display-base.h			\
	test-glib-compat.c			\
	test-glib-compat.h			\
	$(NULL)

LDADD =								\
	libtest.a						\
	$(SPICE_COMMON_DIR)/common/libspice-common.la		\
	$(top_builddir)/server/libserver.la			\
	$(GIO_UNIX_LIBS)					\
	$(GLIB2_LIBS)						\
	$(EPOXY_LIBS)						\
	$(GOBJECT2_LIBS)					\
	$(SPICE_NONPKGCONFIG_LIBS)		                \
	$(NULL)

check_PROGRAMS =				\
	test-codecs-parsing			\
	test-options				\
	test-stat				\
	test-stream				\
	test-agent-msg-filter			\
	test-loop				\
	test-qxl-parsing			\
	test-stat-file				\
	test-leaks				\
	test-vdagent				\
	test-fail-on-null-core-interface	\
	test-empty-success			\
	test-channel				\
	test-stream-device			\
	test-listen				\
	test-record				\
	$(NULL)

noinst_PROGRAMS =				\
	test-display-no-ssl			\
	test-display-streaming			\
	test-playback				\
	test-display-resolution-changes		\
	test-two-servers			\
	test-display-width-stride		\
	$(check_PROGRAMS)			\
	$(NULL)

TESTS = $(check_PROGRAMS)			\
	$(NULL)

## spice-server-replay

noinst_PROGRAMS += spice-server-replay

spice_server_replay_SOURCES = replay.c		\
	../event-loop.c				\
	basic-event-loop.c			\
	basic-event-loop.h

spice_server_replay_LDADD =					\
	$(SPICE_COMMON_DIR)/common/libspice-common.la		\
	$(top_builddir)/server/libspice-server.la		\
	$(GLIB2_LIBS)						\
	$(EPOXY_LIBS)						\
	$(GOBJECT2_LIBS)					\
	$(SPICE_NONPKGCONFIG_LIBS)		                \
	$(NULL)

## test-stat

noinst_LIBRARIES += \
	libtest-stat1.a \
	libtest-stat2.a \
	libtest-stat3.a \
	libtest-stat4.a \
	$(NULL)

test_stat_SOURCES = test-stat.c
test_stat_LDADD = \
	libtest-stat1.a \
	libtest-stat2.a \
	libtest-stat3.a \
	libtest-stat4.a \
	$(LDADD) \
	$(NULL)

libtest_stat1_a_SOURCES = stat-test.c
libtest_stat1_a_CPPFLAGS = $(AM_CPPFLAGS) -DTEST_COMPRESS_STAT=0 -DTEST_RED_WORKER_STAT=0 -DTEST_NAME=stat_test1

libtest_stat2_a_SOURCES = stat-test.c
libtest_stat2_a_CPPFLAGS = $(AM_CPPFLAGS) -DTEST_COMPRESS_STAT=0 -DTEST_RED_WORKER_STAT=1 -DTEST_NAME=stat_test2

libtest_stat3_a_SOURCES = stat-test.c
libtest_stat3_a_CPPFLAGS = $(AM_CPPFLAGS) -DTEST_COMPRESS_STAT=1 -DTEST_RED_WORKER_STAT=0 -DTEST_NAME=stat_test3

libtest_stat4_a_SOURCES = stat-test.c
libtest_stat4_a_CPPFLAGS = $(AM_CPPFLAGS) -DTEST_COMPRESS_STAT=1 -DTEST_RED_WORKER_STAT=1 -DTEST_NAME=stat_test4

## test-gst (helper) and video-encoders (test)

if HAVE_GSTREAMER
noinst_PROGRAMS += test-gst

if ENABLE_EXTRA_CHECKS
TESTS += video-encoders
endif

test_gst_SOURCES = test-gst.c \
	$(NULL)
test_gst_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	$(GSTREAMER_0_10_CFLAGS)		\
	$(GSTREAMER_1_0_CFLAGS)			\
	$(NULL)
endif

EXTRA_DIST += video-encoders

if HAVE_SASL
check_PROGRAMS += test-sasl
endif
