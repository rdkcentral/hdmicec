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

# L2 (integration) test tier for the CCEC middleware.
#
# This directory holds the tier that exercises a full CCEC round trip against each of the
# two HAL back-ends over REAL transport: the legacy in-process C ABI through the existing
# mock, and the out-of-process com.rdk.hal.hdmicec AIDL HAL through a separately hosted
# test-scope fake service.  Two programs are therefore declared here rather than one, and
# the split of sources between them - not any flag below - is the reason this tier exists.
#
# Neither program is installed: both are noinst_PROGRAMS, so nothing declared in this file
# can reach a shipped binary.  That is the structural half of the guarantee that the
# test-scope fake never leaves test scope; the other half is the source split at
# run_L2Tests_SOURCES and fake_hdmi_cec_aidl_host_SOURCES below.
#
# Recursion into this directory comes from tests/Makefile.am naming L2Tests in SUBDIRS
# under `if ENABLE_L1TESTS`, and the Makefile itself from the tests/L2Tests/Makefile entry
# in configure.ac's AC_CONFIG_FILES.  Both halves live outside this file, by necessity: a
# Makefile.am can declare neither.

# Every entry in run_L2Tests_SOURCES below except test_main.cpp names a source in a
# SUBDIRECTORY - ccec/ for the integration case, $(top_srcdir)/mocks/hdmicec for the legacy
# mock - and so does every entry in fake_hdmi_cec_aidl_host_SOURCES.  With subdir-objects
# off, automake flattens each object into this directory and warns ONCE PER SUCH SOURCE,
# every one of them reported against the _SOURCES line that named it.  The subdirectory
# layout mirrors the source tree and is the suite's convention, so the option is enabled
# rather than either flattening the layout or living with one warning per file.  This is
# the same reason tests/L1Tests/Makefile.am:44 sets it, and the rationale there applies
# here unchanged.
#
# ENABLING IT REORDERS NOTHING, which is the property that matters wherever GoogleTest
# static initialisation is involved: automake emits am_run_L2Tests_OBJECTS in the order of
# the list below either way and only the object PATHS change, so the initialisation order
# that the "ORDER IS SIGNIFICANT" note further down depends on is untouched.  That is
# checkable rather than asserted - read am_run_L2Tests_OBJECTS in the generated Makefile
# and it is the list below, in order, with subdirectory prefixes.
#
# The one visible consequence is that each mock's object, .gcno and .deps land beside its
# source in mocks/hdmicec/ instead of here.  That is the same class of in-tree build output
# the build already leaves in ccec/src and osal/src, and it is untracked.  To clear it, use
# BOUNDED commands from the component root - NOT `git clean -fdx` at the repository root,
# because this component is checked out beside others in a shared workspace and a
# repository-wide clean deletes untracked work belonging to all of them:
#     make -C tests/L2Tests clean
#     find ccec osal tests mocks \( -name '*.gcno' -o -name '*.gcda' \) -delete
# The first command removes the objects wherever subdir-objects placed them; the second is
# needed because automake's clean rules do not cover coverage artefacts.
AUTOMAKE_OPTIONS = subdir-objects

# The six roots the L1 suite uses, plus @AIDL_CPPFLAGS@ - the four AIDL/Binder include
# roots configure resolves from the HALIF_PREFIX and BINDER_SDK_DIR staging prefixes: the
# generated com/rdk/hal/hdmicec stubs, the frozen common 0.2.0.0 import, common/current for
# halcompat.h, and the Binder SDK header root.  The set is derived once in configure.ac and
# consumed as a unit by ccec/src/Makefile.am, tests/L1Tests/Makefile.am and this file, so
# that no consumer can end up configured with a different set from the others; the root
# that is easiest to omit and fatal to omit is common/current, because halcompat.h lives
# ONLY there and is not inside common/0.2.0.0/include.
#
# BOTH programs in this directory need all four.  The runner needs them because
# ccec/test_DualPathIntegration.cpp includes ccec/src/DriverAidlImpl.hpp by relative path -
# the same route the ccec/ units use for DriverImpl.hpp - and that header declares
# android::sp<> members of the generated interface types; the host needs them because
# mocks/hdmicec/fake_hdmi_cec_aidl_service.h derives from the generated Bn* server bases.
# AM_CPPFLAGS is per-directory, so both get them from this one assignment.
#
# GTEST_INCLUDE_DIR and GMOCK_INCLUDE_DIR are deliberately left as caller-supplied make
# variables rather than configure substitutions, exactly as tests/L1Tests/Makefile.am has
# them: run_coverage.sh, CI and a manual `make GTEST_INCLUDE_DIR=...` are what set them.
#
# @AIDL_ABI_CPPFLAGS@ carries the Binder WIRE ABI decision configure derives by
# measuring the staged libbinder - -DBINDER_IPC_32BIT, or nothing for a 64-bit ABI.
# BOTH TARGETS IN THIS DIRECTORY MUST HAVE IT, and both do: AM_CPPFLAGS is
# per-directory, so run_L2Tests and fake_hdmi_cec_aidl_host each compile with it, and
# neither declares a per-target _CPPFLAGS that would displace this assignment.  If one
# ever does, it must restate @AIDL_ABI_CPPFLAGS@ - because this tier is the one place
# where two PROCESSES exchange marshalled Binder data, so a runner and a host built
# with different wire layouts would not fail to build; they would fail to talk, and
# the symptom would look like a middleware defect rather than a build defect.
AM_CPPFLAGS = -I$(top_srcdir)/ccec/include \
              -I$(top_srcdir)/osal/include \
              -I$(top_srcdir)/mocks \
              -I$(top_srcdir)/mocks/hdmicec \
              -I$(GTEST_INCLUDE_DIR) \
              -I$(GMOCK_INCLUDE_DIR) \
              @AIDL_CPPFLAGS@ \
              @AIDL_ABI_CPPFLAGS@

# C++17 from the outset, and it is a hard requirement of the dependency rather than a
# preference or a style choice.  halcompat.h opens a nested-namespace definition -
# `namespace com::rdk::hal::halcompat {` - which is C++17 syntax that does not compile at
# c++14 at all.  Two further reasons, each sufficient on its own: the generated IHdmiCec.h
# includes <optional> and declares getProperty(Property, std::optional<PropertyValue>*),
# and the AIDL snapshot these headers come from is itself built at CMAKE_CXX_STANDARD 17.
#
# It is spelled explicitly rather than inherited from the compiler default for the same
# reason configure.ac settles it explicitly: the default happens to be gnu++17 on some
# hosts, and that is precisely what would keep the requirement invisible until a SOC
# toolchain with an older default failed part-way through the build.  @AIDL_CXXFLAGS@
# follows it and carries the dialect flag configure accepted, which is C++17 or newer and
# need not be this spelling: a -std= supplied in CXXFLAGS is probed as given and adopted
# verbatim, and configuration stops instead when it is older than C++17, because Automake
# places CXXFLAGS after AM_CXXFLAGS and the package cannot override it; with none supplied,
# -std=c++17, -std=gnu++17 and -std=c++1z are tried in that order and the first that
# compiles is adopted.  Both flags reach the compile command and the substitution lands
# last, so the substitution is the one that governs - the literal states what this tier
# requires, and the substituted flag is the spelling this compiler was measured to accept.
AM_CXXFLAGS = -Wall -std=c++17 -g @AIDL_CXXFLAGS@

# Define test programs
noinst_PROGRAMS = run_L2Tests fake_hdmi_cec_aidl_host

# THE PER-TARGET SOURCE SPLIT BELOW IS THE WHOLE REASON THIS TIER EXISTS.  Everything else
# in this file is conventional Automake; this is the part that must not be "tidied".
#
# The fake service source belongs to fake_hdmi_cec_aidl_host and NEVER to run_L2Tests,
# because libbinder resolves a service name that was registered in the CALLING process to
# the local BBinder.  interface_cast then yields the local interface rather than a Bp*
# proxy, no transaction crosses the binder driver, and the client threadpool is never
# involved.  So compiling fake_hdmi_cec_aidl_service.cpp into run_L2Tests would silently
# turn this tier back into the in-process case: the AIDL invocation would still pass, while
# proving nothing about the transport it exists to prove.  Only two separate processes make
# the middleware hold a real Bp* proxy and make onMessageReceived able to arrive on a binder
# threadpool thread AT ALL.
#
# THE SPLIT IS NECESSARY AND IT IS NOT SUFFICIENT, which is why this build declares a host
# that does more than sit there.  Two processes make an inbound callback POSSIBLE; nothing
# about them CAUSES one, because the only object that can invoke the runner's listener is
# now in the host and the runner cannot call it.  What DRIVES an inbound frame is the host's
# TEST-ONLY CONTROL CHANNEL: tests/L2Tests/test_main.cpp launches the host with two extra
# inherited descriptors, a case writes one DELIVER command naming exact bytes, and the host
# calls FakeHdmiCecService::fireOnMessageReceived() on the listener the fake captured during
# open().  The two inbound cases in ccec/test_DualPathIntegration.cpp assert the effect.
# That channel is a pair of pipes between two test processes - no .aidl, no interface, no
# method on an interface, no binder object - so it changes nothing about what the middleware
# talks to, and it is why fake_hdmi_cec_aidl_host_SOURCES below carries the host source
# beside the fake rather than the fake alone.
#
# STATED SO THE BUILD FILE DOES NOT CLAIM A RESULT: those two targets and those two cases are
# AUTHORED AND COMPILED here, and inbound delivery on a binder thread is asserted by cases
# that HAVE NOT EXECUTED, because invocation E needs a binder kernel driver that neither a
# development host nor a hosted CI runner has.  Building this directory establishes that they
# build; running them belongs to .github/workflows/aidl-path-tests.yml.  `make check` in this
# directory runs invocation D, on which the four AIDL cases skip at their fixture guard.
#
# The legacy mock belongs to run_L2Tests, because the legacy arm drives the legacy back-end
# through it and without it that arm has no HAL at all.
#
# fake_hdmi_cec_aidl_service.cpp is ALSO compiled into run_L1Tests, and that is correct
# rather than a contradiction: the L1 in-process arms want exactly the local-BBinder
# behaviour described above - it is the only way to reach the compatibility-rejection
# branches, since a remote Bn* service's generated onTransact writes the compiled-in hash
# and version and cannot report bad metadata.  L1 wants the in-process case; L2 must not
# have it.  The fake's own file contract states the same: it is compiled into run_L1Tests
# and into fake_hdmi_cec_aidl_host, and into no production library.

# Main test runner
run_L2Tests_SOURCES = \
    test_main.cpp \
    ccec/test_DualPathIntegration.cpp \
    $(top_srcdir)/mocks/hdmicec/hdmi_cec_driver_mock.cpp

# ORDER IS SIGNIFICANT IN THIS LIST, as a contract to be kept rather than a hazard to be
# repaired.  GoogleTest registers each TEST_F during static initialisation, the order in
# which those initialisers run follows the link order of the objects, which follows this
# list, and the order in which the suites then RUN is the order they were registered.  So
# moving a translation unit that is ALREADY here, or inserting a new one ahead of one,
# reorders every suite after it and can turn a green run red.
#
# STATED HONESTLY FOR THIS TIER: there is nothing fragile here yet.  The list holds exactly
# one ccec/ unit and the legacy mock, the mock registers no TEST_F of its own, and one unit
# cannot be reordered relative to another.  This block therefore travels as a
# FORWARD-LOOKING CONTRACT FOR WHOEVER ADDS THE SECOND UNIT - the point at which the
# property above starts to matter and the point at which it is easiest to break without
# noticing.
#
# THE RULE, for that edit: append the new translation unit at the END OF ITS OWN GROUP -
# after the last existing ccec/ entry for a ccec/ unit - leaving the mock source last and
# leaving the relative order of every pre-existing entry exactly as it was.  The mock's
# final position is reserved by name rather than by group because it is the HAL that every
# driver-dependent case in this binary resolves against.  Do NOT reorder or relocate an
# existing entry to tidy the list; a pure append is the only safe edit here.

# Fake service host: the AIDL HAL's service side, in its own process.
#
# It gets the fake and nothing else.  In particular it does NOT get test_main.cpp or the
# integration case - it is a helper binary, not a second test runner - and it does not get
# the legacy mock, which has no service side to host.
fake_hdmi_cec_aidl_host_SOURCES = \
    $(top_srcdir)/mocks/hdmicec/fake_hdmi_cec_aidl_service.cpp \
    $(top_srcdir)/mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp

# The four AIDL/Binder edges follow the middleware libraries.  libRCEC.la already carries
# them as its own dependencies, but the runner needs them named here in their own right:
# test_main.cpp reaches the service manager directly and the integration case includes
# DriverAidlImpl.hpp, so both reference the stub and Binder libraries from objects that are
# not part of libRCEC.  @AIDL_LIBS@ names them in dependency order, and the pinned
# libcommon-v0.2.0.0-cpp in it is required at exactly 0.2.0.0 - the ABI the hdmicec 0.1.0.0
# snapshot was generated against - because a newer common release would put a different ABI
# underneath pre-generated code compiled against the older headers.
#
# -ltelemetry_msgsender is deliberately not restated here even though ccec/src/Makefile.am
# carries it as AM_LDFLAGS: libtool resolves it through libRCEC.la's dependency_libs, which
# is why tests/L1Tests/Makefile.am does not restate it either.
run_L2Tests_LDADD = \
    $(top_builddir)/ccec/src/libRCEC.la \
    $(top_builddir)/osal/src/libRCECOSHal.la \
    -lgtest -lgmock \
    @AIDL_LDFLAGS@ @AIDL_LIBS@

run_L2Tests_LDFLAGS = -pthread

# The host links the four AIDL/Binder libraries ALONE - no libRCEC, no libRCECOSHal, no
# GoogleTest.  That is its own declared contract, and it is also a correctness property of
# this tier: the host must not contain the middleware whose behaviour the runner is
# measuring, or the two processes would no longer be measuring anything across a boundary.
fake_hdmi_cec_aidl_host_LDADD = @AIDL_LDFLAGS@ @AIDL_LIBS@

# -pthread on the host too, for its SERVICE-SIDE threadpool: the host starts a binder
# threadpool of its own so that incoming transactions are served, which is a separate
# obligation from the client-side pool the middleware starts in DriverAidlImpl::open().
fake_hdmi_cec_aidl_host_LDFLAGS = -pthread

# Run tests on 'make check'
#
# The RUNNER ONLY.  fake_hdmi_cec_aidl_host is a helper the runner launches, waits on and
# reaps - never a test.  Listing it in TESTS would make `make check` execute a service
# process that runs until it is signalled, which would hang the build rather than fail it.
TESTS = run_L2Tests

# test_main.cpp reads the host's location from CEC_FAKE_AIDL_HOST_PATH and documents the
# build as the thing that sets it, so it is set here for `make check` - pointing at the
# host built alongside the runner in this directory.  It is read only in the AIDL
# invocation, so exporting it unconditionally costs the other invocations nothing.  The
# invocation itself is selected by CEC_TEST_AIDL_MODE, which stays with the caller -
# run_coverage.sh or CI - because it is what distinguishes one invocation from the next and
# a build file cannot hold a per-invocation value.
AM_TESTS_ENVIRONMENT = CEC_FAKE_AIDL_HOST_PATH=$(abs_builddir)/fake_hdmi_cec_aidl_host; \
                       export CEC_FAKE_AIDL_HOST_PATH;

# Clean up test artifacts
CLEANFILES = *.log *.trs
