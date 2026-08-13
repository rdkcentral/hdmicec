##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2016 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################

# Every entry in run_L1Tests_SOURCES below except test_main.cpp names a source in a SUBDIRECTORY
# (ccec/, osal/, or the mock under $(top_srcdir)/mocks).  With subdir-objects off, automake flattens
# each object into this directory and warns ONCE PER SUCH SOURCE, every one of them reported against
# the run_L1Tests_SOURCES line.  The subdirectory layout mirrors the source tree and is the suite's
# convention, so the option is enabled rather than either flattening the layout or living with one
# warning per file.
#
# ENABLING IT REORDERS NOTHING, which is the property that matters in this file: automake emits
# am_run_L1Tests_OBJECTS in the order of this list either way and only the object PATHS change, so
# the static-initialisation order that the "ORDER IS SIGNIFICANT" note below depends on is untouched.
# That is checkable rather than asserted - read am_run_L1Tests_OBJECTS in the generated Makefile and
# it is the list below, in order, with subdirectory prefixes.
#
# The one visible consequence is that the mock's object, .gcno and .deps land beside its source in
# mocks/hdmicec/ instead of here.  That is the same class of in-tree build output the build already
# leaves in ccec/src and osal/src, and it is untracked.
#
# To clear that output, use BOUNDED commands from the component root.  Do NOT use `git clean -fdx`
# at the repository root: this component is checked out beside others in a shared workspace, and a
# repository-wide clean deletes untracked work belonging to all of them.
#     make -C tests/L1Tests clean
#     find ccec osal tests mocks \( -name '*.gcno' -o -name '*.gcda' \) -delete
# The first command removes the objects wherever subdir-objects placed them; the second is needed
# because automake's clean rules do not cover coverage artefacts.
AUTOMAKE_OPTIONS = subdir-objects

AM_CPPFLAGS = -I$(top_srcdir)/ccec/include \
              -I$(top_srcdir)/osal/include \
              -I$(top_srcdir)/mocks \
              -I$(top_srcdir)/mocks/hdmicec \
              -I$(GTEST_INCLUDE_DIR) \
              -I$(GMOCK_INCLUDE_DIR)

AM_CXXFLAGS = -Wall -std=c++14 -g

# Define test programs
noinst_PROGRAMS = run_L1Tests

# Main test runner
run_L1Tests_SOURCES = \
    test_main.cpp \
    ccec/test_CECFrame.cpp \
    ccec/test_Connection.cpp \
    ccec/test_Bus.cpp \
    ccec/test_LibCCEC.cpp \
    ccec/test_MessageEncoder.cpp \
    ccec/test_MessageDecoder.cpp \
    ccec/test_OpCode.cpp \
    ccec/test_Operands.cpp \
    ccec/test_Driver_Mock.cpp \
    ccec/test_Driver.cpp \
    ccec/test_DriverImpl_Async.cpp \
    ccec/test_Util.cpp \
    osal/test_ConditionVariable.cpp \
    osal/test_Mutex.cpp \
    osal/test_Thread.cpp \
    $(top_srcdir)/mocks/hdmicec/hdmi_cec_driver_mock.cpp

# ORDER IS SIGNIFICANT IN THIS LIST.  GoogleTest registers each TEST_F during static
# initialisation, the order in which those initialisers run follows the link order of the
# objects, which follows this list, and the order in which the suites then RUN is the order
# they were registered.  This suite contains order-fragile tests that pass only in the
# established order, so moving a translation unit that is ALREADY here, or inserting a new one
# ahead of one, reorders every suite after it and can turn a green run red.
#
# THE RULE, stated as the list above actually obeys it: append a new translation unit at the END
# OF ITS OWN GROUP - after the last existing ccec/ entry for a ccec/ unit, after the last
# existing osal/ entry for an osal/ unit - leaving the mock source last and leaving the relative
# order of every pre-existing entry exactly as it was.  That is where the four units added by
# this pass sit: test_DriverImpl_Async.cpp and test_Util.cpp close the ccec/ group, test_Mutex.cpp
# and test_Thread.cpp close the osal/ group.
#
# WHY GROUP-END RATHER THAN LIST-END, since list-end would look simpler: the three order-fragile
# units are ccec/test_Connection.cpp, ccec/test_Bus.cpp and ccec/test_Driver.cpp, and all three
# already precede both insertion points, so the sequence they depend on is untouched either way -
# but grouping keeps the list readable, and every new unit registers AFTER the fragile ones,
# which is the property that actually matters.  Do NOT reorder or relocate an existing entry to
# tidy the list; a pure append is the only safe edit here.

run_L1Tests_LDADD = \
    $(top_builddir)/ccec/src/libRCEC.la \
    $(top_builddir)/osal/src/libRCECOSHal.la \
    -lgtest -lgmock

run_L1Tests_LDFLAGS = -pthread

# Run tests on 'make check'
TESTS = run_L1Tests

# Clean up test artifacts
CLEANFILES = *.log *.trs
