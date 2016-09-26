NULL =
SUBDIRS = . tests

check-valgrind:
	$(MAKE) -C tests check-valgrind

AM_CPPFLAGS =					\
	-DSPICE_SERVER_INTERNAL			\
	$(COMMON_CFLAGS)			\
	$(GLIB2_CFLAGS)				\
	$(GOBJECT2_CFLAGS)			\
	$(LZ4_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(SASL_CFLAGS)				\
	$(SLIRP_CFLAGS)				\
	$(SMARTCARD_CFLAGS)			\
	$(GSTREAMER_0_10_CFLAGS)		\
	$(GSTREAMER_1_0_CFLAGS)			\
	$(SPICE_PROTOCOL_CFLAGS)		\
	$(SSL_CFLAGS)				\
	$(EPOXY_CFLAGS)				\
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(WARN_CFLAGS)				\
	$(ORC_CFLAGS)				\
	$(NULL)

noinst_LTLIBRARIES = libserver.la
lib_LTLIBRARIES = libspice-server.la

libspice_server_la_LDFLAGS =			\
	-version-info $(SPICE_LT_VERSION)	\
	-no-undefined				\
	$(RELRO_LDFLAGS)			\
	$(NO_INDIRECT_LDFLAGS)			\
	$(NULL)

if HAVE_LD_VERSION_SCRIPT
libspice_server_la_LDFLAGS += \
	-Wl,--version-script=$(srcdir)/spice-server.syms
EXTRA_libspice_server_la_DEPENDENCIES = $(srcdir)/spice-server.syms
endif

libserver_la_LIBADD =							\
	$(SPICE_COMMON_DIR)/common/libspice-common.la			\
	$(SPICE_COMMON_DIR)/common/libspice-common-server.la		\
	$(GL_LIBS)							\
	$(GLIB2_LIBS)							\
	$(GOBJECT2_LIBS)						\
	$(JPEG_LIBS)							\
	$(LZ4_LIBS)							\
	$(LIBRT)							\
	$(PIXMAN_LIBS)							\
	$(SASL_LIBS)							\
	$(SLIRP_LIBS)							\
	$(GSTREAMER_0_10_LIBS)						\
	$(GSTREAMER_1_0_LIBS)						\
	$(SSL_LIBS)							\
	$(EPOXY_LIBS)							\
	$(Z_LIBS)							\
	$(SPICE_NONPKGCONFIG_LIBS)					\
	$(ORC_LIBS)							\
	$(NULL)

libspice_serverincludedir = $(includedir)/spice-server
libspice_serverinclude_HEADERS =		\
	spice-audio.h				\
	spice-char.h				\
	spice-core.h				\
	spice-experimental.h			\
	spice-input.h				\
	spice-migration.h			\
	spice-qxl.h				\
	spice-server.h				\
	spice-version.h				\
	spice-replay.h				\
	spice.h					\
	$(NULL)

spice_built_sources = spice-server-enums.h spice-server-enums.c

libserver_la_SOURCES =				\
	$(libspice_serverinclude_HEADERS)	\
	$(spice_built_sources)			\
	agent-msg-filter.c			\
	agent-msg-filter.h			\
	cache-item.h				\
	char-device.c				\
	char-device.h				\
	common-graphics-channel.c		\
	common-graphics-channel.h		\
	cursor-channel.c			\
	cursor-channel-client.c			\
	cursor-channel-client.h			\
	cursor-channel.h			\
	dcc.c					\
	dcc.h					\
	dcc-private.h				\
	dcc-send.c				\
	dispatcher.c				\
	dispatcher.h				\
	display-channel.c			\
	display-channel.h			\
	display-channel-private.h		\
	display-limits.h			\
	egl.c					\
	egl.h					\
	event-loop.c				\
	glib-compat.h				\
	glz-encoder.c				\
	glz-encoder-dict.c			\
	glz-encoder-dict.h			\
	glz-encoder.h				\
	glz-encoder-priv.h			\
	image-cache.c				\
	image-cache.h				\
	image-encoders.c			\
	image-encoders.h			\
	inputs-channel.c			\
	inputs-channel-client.c			\
	inputs-channel-client.h			\
	inputs-channel.h			\
	jpeg-encoder.c				\
	jpeg-encoder.h				\
	main-channel.c				\
	main-channel-client.c			\
	main-channel-client.h			\
	main-channel.h				\
	main-dispatcher.c			\
	main-dispatcher.h			\
	memslot.c				\
	memslot.h				\
	migration-protocol.h			\
	mjpeg-encoder.c				\
	net-utils.c				\
	net-utils.h				\
	pixmap-cache.c				\
	pixmap-cache.h				\
	red-channel.c				\
	red-channel-capabilities.c		\
	red-channel-capabilities.h		\
	red-channel-client.c			\
	red-channel-client.h			\
	red-channel.h				\
	red-client.c				\
	red-client.h				\
	red-common.h				\
	red-parse-qxl.c				\
	red-parse-qxl.h				\
	red-pipe-item.c				\
	red-pipe-item.h				\
	red-qxl.c				\
	red-qxl.h				\
	red-record-qxl.c			\
	red-record-qxl.h			\
	red-replay-qxl.c			\
	reds.c					\
	reds.h					\
	reds-private.h				\
	red-stream.c				\
	red-stream.h				\
	red-worker.c				\
	red-worker.h				\
	sound.c					\
	sound.h					\
	spice-bitmap-utils.c			\
	spice-bitmap-utils.h			\
	spicevmc.c				\
	stat-file.c				\
	stat-file.h				\
	stat.h					\
	stream-channel.c			\
	stream-channel.h			\
	red-stream-device.c			\
	red-stream-device.h			\
	sw-canvas.c				\
	tree.c					\
	tree.h					\
	utils.c					\
	utils.h					\
	video-encoder.h				\
	video-stream.c				\
	video-stream.h				\
	zlib-encoder.c				\
	zlib-encoder.h				\
	$(NULL)

if HAVE_LZ4
libserver_la_SOURCES +=				\
	lz4-encoder.c				\
	lz4-encoder.h				\
	$(NULL)
endif

if HAVE_SMARTCARD
libserver_la_SOURCES +=			\
	smartcard.c			\
	smartcard.h			\
	smartcard-channel-client.c	\
	smartcard-channel-client.h	\
	$(NULL)
endif

if HAVE_GSTREAMER
libserver_la_SOURCES +=			\
	gstreamer-encoder.c		\
	$(NULL)
endif

libspice_server_la_LIBADD = libserver.la
libspice_server_la_SOURCES =

spice-server-enums.c: spice-server.h spice-server-enums.c.tmpl
	$(AM_V_GEN)glib-mkenums --template $(srcdir)/spice-server-enums.c.tmpl $< > $@

spice-server-enums.h: spice-server.h spice-server-enums.h.tmpl
	$(AM_V_GEN)glib-mkenums --template $(srcdir)/spice-server-enums.h.tmpl $< > $@

EXTRA_DIST =					\
	meson.build				\
	spice-bitmap-utils.tmpl.c		\
	cache-item.tmpl.c			\
	glz-encode-match.tmpl.c			\
	glz-encode.tmpl.c			\
	spice-server.syms			\
	spice-server-enums.h.tmpl		\
	spice-server-enums.c.tmpl		\
	$(NULL)

BUILT_SOURCES = $(spice_built_sources)
