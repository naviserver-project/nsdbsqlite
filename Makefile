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

NSD ?= $(NAVISERVER)/bin/nsd
TESTFLAGS ?=
.PHONY: test
test: all
	$(NSD) -c -d -t $(CURDIR)/tests/test.nscfg $(CURDIR)/tests/all.test $(TESTFLAGS)
