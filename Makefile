# rtspwall — build, test and install.
#
# Build dependencies (Raspberry Pi OS / Debian):
#   sudo apt install build-essential pkg-config libdrm-dev \
#                    libavformat-dev libavcodec-dev libavutil-dev
#
#   make                       build src/rtspwall
#   make test                  build and run the unit tests (no hardware needed)
#   make install               install to $(DESTDIR)$(PREFIX)/bin
#   make VERSION=1.2.3         override the version string
#   make OPTFLAGS="-O0 -g"     optimisation/debug flags (default -O2 -g)
#   make WERROR=0              do not turn warnings into errors — for
#                              packagers/distributions building with newer
#                              compilers or extra hardening flags

PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR  ?= $(PREFIX)/bin

# Directory of this Makefile (also when run as make -C or make -f).
SRCDIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# Version from `git describe`, but only when SRCDIR is itself the root of a
# git repository — a release tarball unpacked inside some other repository
# (e.g. a distribution's packaging repo) must not pick up that repo's tags.
GIT_VERSION = $(shell top=$$(git -C "$(SRCDIR)" rev-parse --show-toplevel 2>/dev/null) \
	&& [ "$$(cd "$$top" && pwd -P)" = "$$(cd "$(SRCDIR)" && pwd -P)" ] \
	&& git -C "$(SRCDIR)" describe --tags --always --dirty 2>/dev/null)
VERSION ?= $(or $(GIT_VERSION),0.1.0-dev)

CC      ?= cc
PKG_CONFIG ?= pkg-config

# -Werror by default: a warning here is in practice always a real bug
# (V4L2/DRM structures are unforgiving to guess through), so a plain `make`
# fails rather than letting it pass silently. The code is kept warning-free
# with gcc and clang at -O0/-O1/-O2/-Os/-Og. Packagers whose compiler or
# hardening flags add new warnings can use WERROR=0. The warning flags are
# kept separate from CFLAGS, so an externally supplied CFLAGS (e.g. from
# dpkg-buildflags) does not drop them.
OPTFLAGS ?= -O2 -g
WERROR   ?= 1
WERROR_FLAG = $(if $(filter 1,$(WERROR)),-Werror)
WARNFLAGS   = -Wall -Wextra -Wno-unused-parameter $(WERROR_FLAG)
CFLAGS  ?= $(OPTFLAGS)
# Recursively expanded (=), so pkg-config only runs for targets that need
# it — `make test` works without the libraries installed.
DEPS_CFLAGS = $(shell $(PKG_CONFIG) --cflags libdrm libavformat libavcodec libavutil)
DEPS_LIBS   = $(shell $(PKG_CONFIG) --libs libdrm libavformat libavcodec libavutil)
LDLIBS  += $(DEPS_LIBS) -lpthread

BIN  = src/rtspwall
SRCS = src/main.c src/drm.c src/v4l2.c src/camera_thread.c src/compositor.c \
       src/config.c src/layout.c src/pacing.c
HDRS = src/rtspwall.h src/layout.h src/pacing.h

all: $(BIN)

# Rebuild when VERSION changes, without rebuilding on every run.
VERSION_STAMP = src/.version
$(VERSION_STAMP): FORCE
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

$(BIN): $(SRCS) $(HDRS) $(VERSION_STAMP)
	$(CC) $(CPPFLAGS) $(WARNFLAGS) $(CFLAGS) $(DEPS_CFLAGS) -DVERSION='"$(VERSION)"' \
		-o $@ $(SRCS) $(LDFLAGS) $(LDLIBS)

# `install` (not cp) so that a running binary can be replaced: cp over a
# busy executable fails with "Text file busy", install writes a new inode.
install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(BIN) $(DESTDIR)$(BINDIR)/rtspwall

# The pure logic (pacing.c, layout.c) is built with just the C compiler —
# no pkg-config, no DRM/V4L2/FFmpeg — so the tests run on any development
# machine as well as on the Pi.
#
# Sanitizers catch UB/memory errors in the pure logic. On by default only on
# macOS: on Raspberry Pi OS kernels ASan can crash at startup even with an
# empty main() (address space reservation), so there they are off unless
# asked for explicitly: make test SANITIZE=1.
ifeq ($(shell uname -s),Darwin)
SANITIZE ?= 1
endif
,            := ,
SAN_FLAGS    = $(if $(filter 1,$(SANITIZE)),-fsanitize=address$(,)undefined)
TEST_CFLAGS  = $(OPTFLAGS) -Wall -Wextra $(WERROR_FLAG) $(SAN_FLAGS)
TEST_LDFLAGS = $(SAN_FLAGS)

TESTS = src/test_pacing src/test_layout

src/test_pacing: src/test_pacing.c src/pacing.c src/pacing.h src/test.h
	$(CC) $(TEST_CFLAGS) -o $@ src/test_pacing.c src/pacing.c $(TEST_LDFLAGS)

src/test_layout: src/test_layout.c src/layout.c src/layout.h src/pacing.c src/pacing.h src/test.h
	$(CC) $(TEST_CFLAGS) -o $@ src/test_layout.c src/layout.c src/pacing.c $(TEST_LDFLAGS)

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

clean:
	rm -f $(BIN) $(TESTS) $(VERSION_STAMP)
	rm -rf src/*.dSYM

.PHONY: all install test clean FORCE
