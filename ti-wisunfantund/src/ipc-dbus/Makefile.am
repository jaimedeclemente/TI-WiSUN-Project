#
# Copyright (c) 2016 Nest Labs, Inc.
# All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

AM_CPPFLAGS = \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/util \
	-I$(top_srcdir)/src/ipc-dbus \
	-I$(top_srcdir)/src/wpantund \
	-I$(top_srcdir)/third_party/assert-macros \
	$(NULL)

include $(top_srcdir)/pre.am

EXTRA_DIST = wpantund.conf

DISTCLEANFILES = .deps Makefile

AUTOMAKE_OPTIONS = subdir-objects

dbusconfdir = $(DBUS_CONFDIR)
noinst_LTLIBRARIES =

if ENABLE_FUZZ_TARGETS
# Not currently fuzzing the DBus code. When that changes, uncomment
# the next line.
#noinst_LTLIBRARIES += libwpantund-dbus-fuzz.la
else
dbusconf_DATA = wpantund.conf
noinst_LTLIBRARIES += libwpantund-dbus.la
endif

libwpantund_dbus_la_SOURCES = \
	DBUSIPCServer.cpp \
	DBUSIPCServer.h \
	DBusIPCAPI.cpp \
	DBusIPCAPI.h \
	wpan-dbus.h \
	../util/DBUSHelpers.cpp \
	$(NULL)

libwpantund_dbus_la_LIBADD =  $(DBUS_LIBS)
libwpantund_dbus_la_CPPFLAGS = $(AM_CPPFLAGS) $(DBUS_CFLAGS) $(CODE_COVERAGE_CPPFLAGS)
libwpantund_dbus_la_CXXFLAGS = $(AM_CXXFLAGS) $(BOOST_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)

libwpantund_dbus_fuzz_la_SOURCES = \
	DBUSIPCServer.cpp \
	DBUSIPCServer.h \
	DBusIPCAPI.cpp \
	DBusIPCAPI.h \
	wpan-dbus.h \
	../util/DBUSHelpers.cpp \
	$(NULL)

libwpantund_dbus_fuzz_la_LIBADD =  $(DBUS_LIBS)
libwpantund_dbus_fuzz_la_CPPFLAGS = $(AM_CPPFLAGS) $(DBUS_CFLAGS) $(FUZZ_CPPFLAGS) $(CODE_COVERAGE_CPPFLAGS)
libwpantund_dbus_fuzz_la_CXXFLAGS = $(AM_CXXFLAGS) $(BOOST_CXXFLAGS) $(FUZZ_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)

pkginclude_HEADERS = wpan-dbus.h
