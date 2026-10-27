# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Portions created by AOL are Copyright (C) 1999 America Online, Inc.
# All Rights Reserved.

.DEFAULT_GOAL := all

ifndef NAVISERVER
    NAVISERVER  = /usr/local/ns
endif

MODNAME    = nsdbsqlite
MOD        = nsdbsqlite.so
MODOBJS    = nsdbsqlite.o
MODLIBS    += -lnsdb
UNIXOBJS   = sqlite3.o

include  $(NAVISERVER)/include/Makefile.module

# Keep the upstream SQLite amalgamation on baseline warnings. Retain all
# other build flags, including hardening, and strict checks for our driver.
sqlite3.o: CFLAGS_WARNING = -Wall $(CFLAGS_FORTIFY)
sqlite3.o: CFLAGS_COMPILER =

NSD ?= $(NAVISERVER)/bin/nsd
TESTFLAGS ?=
.PHONY: test
test: all test-lifecycle
	$(NSD) -c -d -t $(CURDIR)/tests/test.nscfg $(CURDIR)/tests/all.test $(TESTFLAGS)

# Generate the amalgamation from the latest release in SQLite's official Git mirror.
# Set SQLITE_VERSION to select a specific release instead.
SQLITE_VERSION ?=
GIT ?= git
CURL ?= curl
.PHONY: refresh-sqlite
refresh-sqlite:
	@set -eu; \
	version='$(SQLITE_VERSION)'; \
	if test -z "$$version"; then \
	    tags=$$($(GIT) ls-remote --tags --refs https://github.com/sqlite/sqlite.git 'version-*'); \
	    version=$$(printf '%s\n' "$$tags" | \
	        sed -n 's|.*refs/tags/version-\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\(\.[0-9][0-9]*\)\{0,1\}\)$$|\1|p' | \
	        LC_ALL=C sort -t . -k1,1n -k2,2n -k3,3n -k4,4n | tail -n 1); \
	    if test -z "$$version"; then \
	        echo 'No SQLite release tags found upstream' >&2; exit 1; \
	    fi; \
	fi; \
	if ! printf '%s\n' "$$version" | LC_ALL=C grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$$'; then \
	    echo 'SQLITE_VERSION must be a release number such as 3.53.4' >&2; exit 1; \
	fi; \
	tmp=$$(mktemp -d ./sqlite-refresh.XXXXXX); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	$(CURL) --fail --location --retry 2 --connect-timeout 15 --max-time 120 \
	    "https://github.com/sqlite/sqlite/archive/refs/tags/version-$$version.tar.gz" \
	    --output "$$tmp/sqlite.tar.gz"; \
	tar -xzf "$$tmp/sqlite.tar.gz" -C "$$tmp"; \
	src="$$tmp/sqlite-version-$$version"; \
	(cd "$$src" && ./configure); \
	$(MAKE) -C "$$src" sqlite3.c sqlite3.h; \
	for f in sqlite3.c sqlite3.h; do \
	    actual=$$(sed -n 's/^#define SQLITE_VERSION *"\([^"]*\)".*/\1/p' "$$src/$$f"); \
	    if test "$$actual" != "$$version"; then \
	        echo "Generated $$f version does not match $$version" >&2; exit 1; \
	    fi; \
	done; \
	for f in sqlite3.c sqlite3.h; do \
	    if ! cmp -s "$$src/$$f" "$$f"; then mv "$$src/$$f" "$$f"; fi; \
	done; \
	echo "Refreshed SQLite $$version from github.com/sqlite/sqlite"

nsdbsqlite.o sqlite3.o: sqlite3.h

.PHONY: help
help:
	@printf '%s\n' \
	    'nsdbsqlite targets:' \
	    '  all             Build the driver (default; uses bundled SQLite)' \
	    '  install         Install the driver into NAVISERVER' \
	    '  clean           Remove build artifacts' \
	    '  test            Run lifecycle and isolated integration tests' \
	    '  test-lifecycle  Run direct callback/finalization checks' \
	    '  refresh-sqlite  Download latest release and regenerate sqlite3.c/h' \
	    '  help            Show this help' \
	    '' \
	    'Variables (current values):' \
	    '  NAVISERVER=$(NAVISERVER)' \
	    '  NSD=$(NSD)' \
	    '  TESTFLAGS=$(TESTFLAGS)' \
	    '  SQLITE_VERSION=$(SQLITE_VERSION) (empty selects latest release)' \
	    '  GIT=$(GIT)' \
	    '  CURL=$(CURL)' \
	    '' \
	    'Examples:' \
	    '  make NAVISERVER=/opt/ns' \
	    '  make test TESTFLAGS="-verbose bpse"' \
	    '  make refresh-sqlite' \
	    '  make refresh-sqlite SQLITE_VERSION=3.53.4'

.PHONY: test-lifecycle clean-lifecycle
test-lifecycle: tests/lifecycle-test$(EXEEXT)
	./tests/lifecycle-test$(EXEEXT)

tests/lifecycle-test$(EXEEXT): tests/lifecycle-test.o sqlite3.o
	$(CC) $(LDFLAGS) -o $@ tests/lifecycle-test.o sqlite3.o $(MODLIBS) $(NSLIBS)

tests/lifecycle-test.o: nsdbsqlite.c sqlite3.h $(MODULE_INFO_HEADER)

clean: clean-lifecycle
clean-lifecycle:
	$(RM) tests/lifecycle-test$(EXEEXT) tests/lifecycle-test.o
