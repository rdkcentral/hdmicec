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
    osal/test_Thread.cpp \
    osal/test_ConditionVariable.cpp \
    $(top_srcdir)/mocks/hdmicec/hdmi_cec_driver_mock.cpp

run_L1Tests_LDADD = \
    $(top_builddir)/ccec/src/libRCEC.la \
    $(top_builddir)/osal/src/libRCECOSHal.la \
    -lgtest -lgmock

run_L1Tests_LDFLAGS = -pthread

# Run tests on 'make check'
TESTS = run_L1Tests

# Clean up test artifacts
CLEANFILES = *.log *.trs
