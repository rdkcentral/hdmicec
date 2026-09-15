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
RM          := rm -rf 
# RELEASE HARDENING OF THE SHIPPING LIBRARY.  -O2 and _FORTIFY_SOURCE are here and
# not in ccec/src/Makefile.am, and that asymmetry is deliberate and load-bearing.
#
# QA finding P5-05 measured both shipping artefacts and found neither carried any
# DT_FLAGS or DT_FLAGS_1 entry - so no BIND_NOW, partial RELRO only - and found this
# one carrying no FORTIFY reference at all beyond the stack canary.  The three flags
# added for it are:
#
#   -O2                      FORTIFY IS INERT WITHOUT IT.  _FORTIFY_SOURCE needs the
#                            optimiser's constant folding to know an object's size
#                            at a call site; at -O0 glibc's headers define it away
#                            and the __*_chk entry points are never referenced.  So
#                            -O2 is a prerequisite of the line below rather than a
#                            performance change, and this file previously requested
#                            no optimisation level at all.
#   -U_FORTIFY_SOURCE        BEFORE the -D, so a value already in the environment's
#                            CFLAGS - or a distribution toolchain that predefines
#                            _FORTIFY_SOURCE=3, as Ubuntu's gcc-13 does when
#                            optimising - cannot collide with the -D and turn a
#                            hardening request into a "redefined" warning or, worse,
#                            leave the inherited value in force unexamined.
#                            Undefining first makes the value this file asks for the
#                            value it gets, on every toolchain.
#   -D_FORTIFY_SOURCE=2      Level 2 rather than 3: 2 is supported by every glibc
#                            and every GCC an RDK SOC toolchain is likely to carry,
#                            whereas 3 needs GCC 12 or newer with
#                            __builtin_dynamic_object_size.  A SOC toolchain that
#                            cannot honour the requested level would silently give
#                            back less protection than the log claims.
#
# -g is retained: it emits DWARF and does not affect code generation, and the RDK
# packaging step strips shipped objects separately.
CFLAGS      := -g -O2 -fPIC -D_REENTRANT -Wall -std=c++17 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
LIBNAME     := RCEC
LIBNAMEFULL := lib$(LIBNAME).so

OBJS:= CECFrame.o \
	Connection.o \
	Driver.o \
	MessageDecoder.o \
	Bus.o \
	DriverImpl.o \
	DriverAidlImpl.o \
	LibCCEC.o \
	OpCode.o \
	Util.o \

# THE ORCHESTRATOR CONTRACT OF THIS FILE, IN FULL.  Every path this build needs
# from outside its own source tree is derived from one of the variables below,
# and each is exported by the RDK build orchestrator exactly as it already
# exports OPENSOURCE_BASE, GLIB_LIBRARY_PATH and GLIBS further down.
#
# The AIDL/Binder back-end is built into libRCEC unconditionally, for every SOC
# vendor, so its include roots and link edges are not conditional on anything.
# Both are derived from the same two staging prefixes configure takes:
#
#   HALIF_PREFIX             root of the staged rdk-halif-aidl tree
#   BINDER_SDK_DIR           root of the staged linux_binder_idl Binder SDK
#   BINDER_SDK_INCLUDE_DIR   optional override, for a split staging layout whose
#                            headers do not sit under BINDER_SDK_DIR.  It is not
#                            defaulted to BINDER_SDK_DIR here: BINDER_SDK_DIR
#                            supplies two of the four candidate header layouts
#                            below on its own, so a default would only hide
#                            which prefix a resolved root actually came from
#   HALIF_LIB_DIR            optional override, for a layout that stages the stub
#                            libraries outside the rdk-halif-aidl tree
#   BINDER_LIB_DIR           optional override, likewise for libbinder/libutils
#
# The LEGACY back-end and the telemetry call sites need two further staging roots,
# and they are named here for the same reason the Binder roots are: this is the
# build every SOC vendor actually runs through rdk_build.sh, and a header it
# cannot name is a header it cannot be certified against.
#
#   TELEMETRY_PREFIX         root of the staged RDK telemetry tree, which
#                            supplies <telemetry_busmessage_sender.h> to
#                            Bus.cpp, LibCCEC.cpp and MessageDecoder.cpp
#   TELEMETRY_INCLUDE_DIR    optional override, naming that header's own
#                            directory when no candidate layout below describes
#                            the staging tree
#   LEGACY_HAL_PREFIX        root of the staged legacy HDMI CEC HAL headers,
#                            which supply "ccec/drivers/hdmi_cec_driver.h" to
#                            DriverImpl.cpp.  The subpath is part of the
#                            contract: DriverImpl.cpp reaches the header as
#                            ccec/drivers/hdmi_cec_driver.h, so a prefix that
#                            presents it flat - as the upstream
#                            rdk-halif-hdmi_cec repository lays it out, at
#                            include/hdmi_cec_driver.h - has to be staged into
#                            that subpath rather than named as it stands
#   LEGACY_HAL_INCLUDE_DIR   optional override, likewise
#
# A deployment that does not export them fails with the named error this file
# raises: every root it needs from outside this source tree is probed below - the
# Binder header and library roots, the two production header roots, and the three
# AIDL stub include roots with their stub library directory - which is the
# intended outcome, because the alternative is silently shipping a library that
# carries only one back-end, or one certified against test-scope headers.
#
# HALIF_LIB_DIR is deliberately NOT defaulted here with a `?=` any more.  It is
# resolved beside the three AIDL include roots it belongs with, further down, by
# the same probe they use and under the same $(origin) test the Binder library
# directory below uses, so that an unset HALIF_PREFIX cannot expand to the host's
# own /lib/halif.  That expansion is QA finding N6: with the prefix unset - and
# every other root correctly staged - this file emitted three absolute include
# roots belonging to the host and a -L/lib/halif beside them, and named none of
# them.

# THE BINDER LIBRARY DIRECTORY IS PROBED, FOR THE SAME REASON THE HEADER ROOT
# BELOW IS.  configure.ac accepts two layouts for it - lib/binder, as the SDK's
# own build stages it, and then a flat lib/, as a Yocto-style sysroot presents it
# - so a hand build that assumed only the first would fail to link on a staging
# tree the Autotools build configures happily, which is the same asymmetry the
# header probe exists to remove.  The candidates and their order are exactly
# configure.ac's; each is skipped when BINDER_SDK_DIR is unset, so an unset
# prefix cannot expand to an absolute /lib/binder belonging to the host; and each
# is probed by libbinder.so itself rather than by the bare directory, because
# $(wildcard) cannot distinguish a directory from a file and a directory without
# libbinder.so cannot satisfy the -lbinder edge anyway.
#
# The probe is skipped entirely when BINDER_LIB_DIR was supplied from the
# environment or the command line, which is exactly what the ?= this replaces
# guaranteed - $(origin) is undefined only when nothing has set it.
ifeq ($(origin BINDER_LIB_DIR),undefined)
BINDER_LIB_CANDIDATES := \
	$(if $(strip $(BINDER_SDK_DIR)),$(BINDER_SDK_DIR)/lib/binder) \
	$(if $(strip $(BINDER_SDK_DIR)),$(BINDER_SDK_DIR)/lib)

BINDER_LIB_DIR := $(firstword \
	$(patsubst %/libbinder.so,%, \
		$(wildcard $(addsuffix /libbinder.so,$(BINDER_LIB_CANDIDATES)))))
endif

# THE BINDER HEADER ROOT IS PROBED, NOT ASSUMED, AND THE CANDIDATE LIST IS THE
# SAME FOUR LAYOUTS IN THE SAME ORDER configure.ac accepts.  It has to be: this
# hand-written makefile is the build every SOC vendor actually runs through
# rdk_build.sh, so a staging layout that satisfies Autotools and not this file
# would configure cleanly and then fail here - or, worse, be "fixed" by dropping
# the back-end.  The order is the one the AIDL snapshot's own CMake build
# resolves in: the local development layout first, then the split-staging
# override in both its spellings, then a flat include directory.
#
#   1  $(BINDER_SDK_DIR)/include/binder_sdk           development layout
#   2  $(BINDER_SDK_INCLUDE_DIR)/include/binder_sdk   split staging
#   3  $(BINDER_SDK_INCLUDE_DIR)/include              split staging, flat
#   4  $(BINDER_SDK_DIR)/include                      Yocto or sysroot, flat
#
# A CANDIDATE WHOSE PREFIX IS UNSET CONTRIBUTES NOTHING, which is why each one
# is wrapped in $(if ...).  Without that guard an unset prefix would expand to
# the absolute path /include/binder_sdk, and on a host that happens to have such
# a directory the probe would resolve to it and the build would compile against
# headers nobody staged.  configure.ac guards its four the same way, with
# `test -n`, for the same reason.
#
# EACH CANDIDATE IS PROBED BY THE HEADER THE GENERATED STUBS ACTUALLY INCLUDE,
# binder/IBinder.h, rather than by the bare directory.  $(wildcard) is an
# existence test that does not distinguish a directory from a file, so a
# directory probe would accept a stray file of the same name; and a root that
# does not carry binder/IBinder.h cannot work anyway, because every generated
# header reaches it as <binder/IBinder.h> through this -I.  So this test is
# never weaker than configure's `test -d` and turns one class of unstaged tree
# into the named error below instead of a confusing header-not-found later.
# $(wildcard) preserves the order of its arguments and $(firstword) then takes
# the first surviving candidate, so precedence is exactly the list above.
BINDER_INCLUDE_CANDIDATES := \
	$(if $(strip $(BINDER_SDK_DIR)),$(BINDER_SDK_DIR)/include/binder_sdk) \
	$(if $(strip $(BINDER_SDK_INCLUDE_DIR)),$(BINDER_SDK_INCLUDE_DIR)/include/binder_sdk) \
	$(if $(strip $(BINDER_SDK_INCLUDE_DIR)),$(BINDER_SDK_INCLUDE_DIR)/include) \
	$(if $(strip $(BINDER_SDK_DIR)),$(BINDER_SDK_DIR)/include)

BINDER_INCLUDE_DIR := $(firstword \
	$(patsubst %/binder/IBinder.h,%, \
		$(wildcard $(addsuffix /binder/IBinder.h,$(BINDER_INCLUDE_CANDIDATES)))))

# The check is skipped when the only goal is `clean`, so that removing objects
# still works on a host that never staged the SDK.  Every other goal compiles or
# links, and none of those can be honestly attempted without these headers.
BINDER_HEADER_GOALS := $(filter-out clean,$(MAKECMDGOALS))
ifeq ($(strip $(MAKECMDGOALS)),)
BINDER_HEADER_GOALS := all
endif

define BINDER_INCLUDE_NOT_FOUND
none of the four Binder SDK header layouts resolved, so libRCEC cannot be built
with the AIDL HDMI CEC back-end, and the back-end is not optional.  Export the
staging prefixes this makefile derives its paths from - HALIF_PREFIX for the
rdk-halif-aidl tree, BINDER_SDK_DIR for the linux_binder_idl Binder SDK, and
BINDER_SDK_INCLUDE_DIR only if that SDK's headers are staged apart from it -
exactly as the RDK build orchestrator already exports OPENSOURCE_BASE and
GLIB_LIBRARY_PATH.  Tried, in order, each skipped when its prefix is unset and
each probed for binder/IBinder.h beneath it:
  1  $$(BINDER_SDK_DIR)/include/binder_sdk
  2  $$(BINDER_SDK_INCLUDE_DIR)/include/binder_sdk
  3  $$(BINDER_SDK_INCLUDE_DIR)/include
  4  $$(BINDER_SDK_DIR)/include
with BINDER_SDK_DIR=[$(BINDER_SDK_DIR)] and BINDER_SDK_INCLUDE_DIR=[$(BINDER_SDK_INCLUDE_DIR)],
which left these candidates to probe: [$(strip $(BINDER_INCLUDE_CANDIDATES))]
endef

define BINDER_LIB_NOT_FOUND
neither Binder SDK library layout resolved, so libRCEC cannot be linked against
libbinder and libutils, and those edges are not optional.  Export BINDER_SDK_DIR
to the linux_binder_idl Binder SDK staging prefix, or set BINDER_LIB_DIR
directly when the libraries are staged apart from it.  Tried, in order, each
skipped when BINDER_SDK_DIR is unset and each probed for libbinder.so beneath it:
  1  $$(BINDER_SDK_DIR)/lib/binder
  2  $$(BINDER_SDK_DIR)/lib
with BINDER_SDK_DIR=[$(BINDER_SDK_DIR)], which left these candidates to probe:
[$(strip $(BINDER_LIB_CANDIDATES))]
endef

ifneq ($(strip $(BINDER_HEADER_GOALS)),)
ifeq ($(strip $(BINDER_INCLUDE_DIR)),)
$(error $(BINDER_INCLUDE_NOT_FOUND))
endif
ifeq ($(strip $(BINDER_LIB_DIR)),)
$(error $(BINDER_LIB_NOT_FOUND))
endif
endif

# ---------------------------------------------------------------------------
# THE TWO PRODUCTION HEADER ROOTS, PROBED THE SAME WAY THE BINDER ROOT IS.
#
# Three of this library's translation units include <telemetry_busmessage_sender.h>
# - Bus.cpp, LibCCEC.cpp and MessageDecoder.cpp - and DriverImpl.cpp includes
# "ccec/drivers/hdmi_cec_driver.h".  Until this block existed, NO variable in this
# file named either root: the only -I that could have carried the second was
# -I../drivers/include, and ../drivers does not exist in this source tree at all.
#
# The measured consequence is the whole of QA finding P3-HAND.  With only the
# documented AIDL staging prefixes exported, this makefile stopped at
# "telemetry_busmessage_sender.h: No such file or directory", and the only way to
# reach install/lib/libRCEC.so was to add -I../../mocks -I../../stubs - which
# builds the SHIPPING library against ../../mocks/telemetry_busmessage_sender.h,
# whose t2_init and t2_event_s are empty macros, and against
# ../../stubs/ccec/drivers/hdmi_cec_driver.h, a symlink to the GoogleMock HAL
# header.  Both compile, both link, and the result is a certified artefact whose
# telemetry silently does nothing.
#
# So the fix is not another -I.  It is the same shape the Binder header root uses,
# for the same reason: name the root, PROBE it for the exact header it has to
# supply, and stop with a named error rather than guess.  What is added on top of
# that shape is a REFUSAL, below, which is what makes the guarantee structural
# instead of a convention a later engineer can quietly step around.
#
# THE CANDIDATE LISTS FOLLOW THE BINDER PATTERN EXACTLY, and every element is
# wrapped in $(if $(strip ...)) for the reason stated above BINDER_INCLUDE_CANDIDATES:
# an unset prefix would otherwise expand to the absolute /include, and on a host
# that happens to have such a directory the probe would resolve to headers nobody
# staged.  Each candidate is probed by the header it must supply rather than by
# the bare directory, because $(wildcard) does not distinguish a directory from a
# file and a directory that lacks the header cannot satisfy the #include anyway.
# $(wildcard) preserves argument order and $(firstword) takes the first survivor,
# so precedence is exactly the order written.
#
# ONE DELIBERATE DIFFERENCE FROM THE BINDER_LIB_DIR PRECEDENT ABOVE: that variable
# skips its probe entirely when it was set from the environment, whereas a
# TELEMETRY_INCLUDE_DIR or LEGACY_HAL_INCLUDE_DIR supplied from outside is entered
# as the FIRST CANDIDATE and probed like any other.  An override that does not
# actually carry the header then lands in the named error below, naming the header
# and the value, instead of being trusted into a compile failure three files
# later.  Being a candidate rather than an escape hatch is also what keeps an
# override subject to the refusal, which is the point of the refusal.
TELEMETRY_INCLUDE_CANDIDATES := \
	$(if $(strip $(TELEMETRY_INCLUDE_DIR)),$(TELEMETRY_INCLUDE_DIR)) \
	$(if $(strip $(TELEMETRY_PREFIX)),$(TELEMETRY_PREFIX)/include) \
	$(if $(strip $(TELEMETRY_PREFIX)),$(TELEMETRY_PREFIX)/usr/include) \
	$(if $(strip $(TELEMETRY_PREFIX)),$(TELEMETRY_PREFIX)) \
	$(if $(strip $(OPENSOURCE_BASE)),$(OPENSOURCE_BASE)/include) \
	$(if $(strip $(OPENSOURCE_BASE)),$(OPENSOURCE_BASE)/usr/include)

TELEMETRY_INCLUDE_DIR := $(firstword \
	$(patsubst %/telemetry_busmessage_sender.h,%, \
		$(wildcard $(addsuffix /telemetry_busmessage_sender.h,$(TELEMETRY_INCLUDE_CANDIDATES)))))

# OPENSOURCE_BASE is in the telemetry list and not in the HAL list, and that
# asymmetry is deliberate.  Telemetry is an RDK opensource component and this file
# already consumes $(OPENSOURCE_BASE)/lib in LDFLAGS below, so a correctly staged
# RDK build resolves the telemetry root with no new variable exported at all; the
# legacy CEC HAL header is a HAL, staged by the sdk/devicesettings components
# rdk_env.xml already declares, and inventing an opensource candidate for it would
# only widen the search without describing any real layout.
#
# ../drivers/include IS RETAINED, LAST.  It is the only header root this file named
# before P3-HAND, so a tree that does populate it must keep resolving; it is last
# because a root the orchestrator staged deliberately should outrank one that
# happens to be in the source tree.  It is no longer an unconditional -I, and that
# move is the substance rather than tidying: as a candidate it is probed for the
# header and then subjected to the refusal, so an in-tree directory carrying a
# test-scope header is caught rather than silently compiled against.
LEGACY_HAL_INCLUDE_CANDIDATES := \
	$(if $(strip $(LEGACY_HAL_INCLUDE_DIR)),$(LEGACY_HAL_INCLUDE_DIR)) \
	$(if $(strip $(LEGACY_HAL_PREFIX)),$(LEGACY_HAL_PREFIX)/include) \
	$(if $(strip $(LEGACY_HAL_PREFIX)),$(LEGACY_HAL_PREFIX)/usr/include) \
	$(if $(strip $(LEGACY_HAL_PREFIX)),$(LEGACY_HAL_PREFIX)) \
	../drivers/include

LEGACY_HAL_INCLUDE_DIR := $(firstword \
	$(patsubst %/ccec/drivers/hdmi_cec_driver.h,%, \
		$(wildcard $(addsuffix /ccec/drivers/hdmi_cec_driver.h,$(LEGACY_HAL_INCLUDE_CANDIDATES)))))

# THE REFUSAL.  This makefile is the SHIPPING build - rdk_build.sh invokes it with
# a bare `make` for every SOC branch - so it cannot be certified against the
# repository's own test-scope headers, and P3-HAND is exactly the case of that
# happening and passing.  A CI grep for "mocks" would express the same rule as a
# convention; expressing it here makes it a property of the build.
#
# The match is on CANONICAL paths, from $(realpath), rather than on the text of the
# variable, because the workaround's own spellings - -I../../mocks and
# -I../../stubs - are relative, an orchestrator would supply absolute ones, and
# ./../../mocks/, ../../mocks/. and a symlink to any of them all name the same
# directory.  Canonicalising both sides reduces every spelling to one string, so
# the check does not depend on guessing which one was used.
#
# BOTH THE ROOT AND THE HEADER IT SUPPLIES ARE CANONICALISED, and the second test
# is the one that closes the gap this tree already demonstrates:
# stubs/ccec/drivers/hdmi_cec_driver.h is a SYMLINK into mocks/, so a root outside
# both trees can still serve a test header through a link.  Resolving the header
# file itself catches that, whichever tree it points into.
CCEC_TEST_HEADER_TREES := $(realpath ../../mocks) $(realpath ../../stubs)

# $(1) resolved include root, $(2) the header path beneath it that was probed.
# Expands to the offending tree when either canonical form is that tree or sits
# under it, and to nothing otherwise.  An empty root expands to nothing rather
# than to /$(2), which would otherwise be probed against the host's filesystem
# root; an unresolved root is the named not-found error's business, not this one.
ccec_test_tree_hit = $(strip $(if $(strip $(1)),$(foreach ccec_tree,$(CCEC_TEST_HEADER_TREES), \
	$(if $(filter $(ccec_tree) $(ccec_tree)/%,$(realpath $(1)) $(realpath $(1)/$(2))),$(ccec_tree)))))

TELEMETRY_TEST_TREE_HIT := $(call ccec_test_tree_hit,$(TELEMETRY_INCLUDE_DIR),telemetry_busmessage_sender.h)
LEGACY_HAL_TEST_TREE_HIT := $(call ccec_test_tree_hit,$(LEGACY_HAL_INCLUDE_DIR),ccec/drivers/hdmi_cec_driver.h)

define TELEMETRY_INCLUDE_NOT_FOUND
no staged telemetry header root resolved, so libRCEC cannot be built: Bus.cpp,
LibCCEC.cpp and MessageDecoder.cpp all include <telemetry_busmessage_sender.h>
and none of the candidate layouts below carries it.  Export TELEMETRY_PREFIX to
the staged RDK telemetry tree, or set TELEMETRY_INCLUDE_DIR to that header's own
directory, exactly as the RDK build orchestrator already exports OPENSOURCE_BASE
and GLIB_LIBRARY_PATH.  Tried, in order, each skipped when its variable is unset
and each probed for telemetry_busmessage_sender.h directly beneath it:
  1  $$(TELEMETRY_INCLUDE_DIR)   as supplied
  2  $$(TELEMETRY_PREFIX)/include
  3  $$(TELEMETRY_PREFIX)/usr/include
  4  $$(TELEMETRY_PREFIX)
  5  $$(OPENSOURCE_BASE)/include
  6  $$(OPENSOURCE_BASE)/usr/include
with TELEMETRY_INCLUDE_DIR=[$(TELEMETRY_INCLUDE_DIR)], TELEMETRY_PREFIX=[$(TELEMETRY_PREFIX)]
and OPENSOURCE_BASE=[$(OPENSOURCE_BASE)], which left these candidates to probe:
[$(strip $(TELEMETRY_INCLUDE_CANDIDATES))].
The repository's own mocks/telemetry_busmessage_sender.h is NOT an answer here: it
defines t2_init and t2_event_s as empty macros for the L1 suite, and this build is
the shipping one.  See QA finding P3-HAND.
endef

define LEGACY_HAL_INCLUDE_NOT_FOUND
no staged legacy HDMI CEC HAL header root resolved, so libRCEC cannot be built:
DriverImpl.cpp includes "ccec/drivers/hdmi_cec_driver.h" and none of the candidate
layouts below carries it.  Export LEGACY_HAL_PREFIX to the staged legacy HAL tree,
or set LEGACY_HAL_INCLUDE_DIR to the include root that carries it.  THE SUBPATH IS
PART OF THE CONTRACT: the header must sit at ccec/drivers/hdmi_cec_driver.h beneath
the root, because that is how DriverImpl.cpp reaches it, so an upstream
rdk-halif-hdmi_cec checkout - which lays the header out flat, at
include/hdmi_cec_driver.h - has to be staged into that subpath rather than named
where it stands.  Tried, in order, each skipped when its variable is unset and each
probed for ccec/drivers/hdmi_cec_driver.h beneath it:
  1  $$(LEGACY_HAL_INCLUDE_DIR)   as supplied
  2  $$(LEGACY_HAL_PREFIX)/include
  3  $$(LEGACY_HAL_PREFIX)/usr/include
  4  $$(LEGACY_HAL_PREFIX)
  5  ../drivers/include            the in-tree layout, retained
with LEGACY_HAL_INCLUDE_DIR=[$(LEGACY_HAL_INCLUDE_DIR)] and LEGACY_HAL_PREFIX=[$(LEGACY_HAL_PREFIX)],
which left these candidates to probe: [$(strip $(LEGACY_HAL_INCLUDE_CANDIDATES))].
The repository's own stubs/ccec/drivers/hdmi_cec_driver.h is NOT an answer here: it
is a symlink to the GoogleMock HAL header, and this build is the shipping one.
See QA finding P3-HAND.
endef

define PRODUCTION_HEADER_ROOT_IS_TEST_TREE
$(CCEC_REFUSED_VAR) resolved to [$(CCEC_REFUSED_ROOT)] and is REFUSED, because it -
or the $(CCEC_REFUSED_HEADER) it supplies, which resolves to
[$(CCEC_REFUSED_PATH)] - is inside this repository's test-scope header tree(s)
[$(CCEC_REFUSED_TREE)].  Either match is disqualifying on its own: a root outside
mocks/ and stubs/ that reaches a header inside one through a symlink compiles the
same test-scope declaration as a root named there directly.
This makefile is the SHIPPING build of libRCEC - rdk_build.sh invokes it with a
bare `make` on every SOC branch - and mocks/ and stubs/ exist for the L1 and L2
suites: mocks/telemetry_busmessage_sender.h makes t2_init and t2_event_s empty
macros, and stubs/ccec/drivers/hdmi_cec_driver.h is a symlink to the GoogleMock
HAL header.  A library certified against either reports success while sending no
telemetry and while never having compiled against the real HAL declaration, which
is precisely QA finding P3-HAND: the acceptance artefact was producible only with
-I../../mocks -I../../stubs.  This refusal is what stops that workaround being
reused, so it is not to be relaxed or worked around - resolve it by staging the
real headers.
Set $(CCEC_REFUSED_PREFIX_VAR), or $(CCEC_REFUSED_VAR) directly, to a production
staging root outside this repository.  Both are documented in the orchestrator
contract at the top of this file.  Note that the match is on canonical paths, from
$$(realpath), and covers the header file as well as the root, so a root outside
mocks/ and stubs/ that serves the header through a symlink into either is refused
here too.
endef

# All three checks are guarded by BINDER_HEADER_GOALS - the same variable the Binder
# header and library checks above use, and the same condition rather than a similar
# one: it is empty exactly when the only goal is `clean`.  Removing objects has to
# stay possible on a host that staged nothing, and every other goal compiles or
# links and cannot honestly be attempted without these headers.  The variable's
# name predates this block; the condition is shared deliberately, so that one
# change to what "a build that needs headers" means moves all of them together.
ifneq ($(strip $(BINDER_HEADER_GOALS)),)
ifeq ($(strip $(TELEMETRY_INCLUDE_DIR)),)
$(error $(TELEMETRY_INCLUDE_NOT_FOUND))
endif
ifeq ($(strip $(LEGACY_HAL_INCLUDE_DIR)),)
$(error $(LEGACY_HAL_INCLUDE_NOT_FOUND))
endif
# The refusal runs after both roots resolved, so its message can name the value it
# is rejecting.  The six CCEC_REFUSED_* variables carry that value into the message
# above, which is recursively expanded and therefore reads them at $(error) time;
# both checks share one message rather than duplicating it per root.
ifneq ($(strip $(TELEMETRY_TEST_TREE_HIT)),)
CCEC_REFUSED_VAR := TELEMETRY_INCLUDE_DIR
CCEC_REFUSED_PREFIX_VAR := TELEMETRY_PREFIX
CCEC_REFUSED_ROOT := $(TELEMETRY_INCLUDE_DIR)
CCEC_REFUSED_HEADER := telemetry_busmessage_sender.h
CCEC_REFUSED_PATH := $(realpath $(TELEMETRY_INCLUDE_DIR)/telemetry_busmessage_sender.h)
CCEC_REFUSED_TREE := $(TELEMETRY_TEST_TREE_HIT)
$(error $(PRODUCTION_HEADER_ROOT_IS_TEST_TREE))
endif
ifneq ($(strip $(LEGACY_HAL_TEST_TREE_HIT)),)
CCEC_REFUSED_VAR := LEGACY_HAL_INCLUDE_DIR
CCEC_REFUSED_PREFIX_VAR := LEGACY_HAL_PREFIX
CCEC_REFUSED_ROOT := $(LEGACY_HAL_INCLUDE_DIR)
CCEC_REFUSED_HEADER := ccec/drivers/hdmi_cec_driver.h
CCEC_REFUSED_PATH := $(realpath $(LEGACY_HAL_INCLUDE_DIR)/ccec/drivers/hdmi_cec_driver.h)
CCEC_REFUSED_TREE := $(LEGACY_HAL_TEST_TREE_HIT)
$(error $(PRODUCTION_HEADER_ROOT_IS_TEST_TREE))
endif
endif
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# THE THREE AIDL STUB INCLUDE ROOTS AND THE STUB LIBRARY DIRECTORY, PROBED THE
# SAME WAY THE BINDER AND THE TWO PRODUCTION ROOTS ABOVE ARE.
#
# THE MEASURED CONSEQUENCE OF NOT PROBING THEM IS QA FINDING N6.  AIDL_INCLUDE
# below expanded $(HALIF_PREFIX) with no probe and no guard, so with that prefix
# unset - every other root correctly staged - it emitted
# -I/hdmicec/0.1.0.0/include, -I/common/0.2.0.0/include and -I/common/current:
# three absolute roots belonging to the host rather than to any staging tree, one
# of which a host could plausibly own.  The build then stopped several files in,
# with a bare "com/rdk/hal/hdmicec/BnHdmiCecEventListener.h: No such file or
# directory" that named neither the prefix nor the root, while configure.ac -
# given the same tree - names the exact missing root and states that the
# dependency cannot be disabled.  The two builds now say the same thing.
#
# THE SUBPATHS ARE FIXED AND THERE IS NO CANDIDATE LIST, which is the one
# deliberate difference from the Binder and production probes above.  configure.ac
# accepts exactly these three subpaths beneath HALIF_PREFIX and no alternative
# layout - hdmicec/0.1.0.0/include, common/0.2.0.0/include and common/current,
# each behind a plain `test -d` on that one path - so inventing extra candidates
# here would make the hand build accept a staging tree the Autotools build
# rejects, which is precisely the asymmetry the Binder probe's shared candidate
# ORDER exists to remove.  The same inputs must name the same accepted trees
# whichever of the two build systems is driven.
#
# EACH ROOT IS PROBED BY THE HEADER IT MUST ACTUALLY SUPPLY, not by the bare
# directory, for the reason stated above BINDER_INCLUDE_CANDIDATES: $(wildcard)
# is an existence test that does not distinguish a directory from a file, and a
# root that does not carry its header cannot satisfy the #include anyway.  The
# witness for each is the header the code reaches THROUGH that root:
#
#   hdmicec/0.1.0.0/include   com/rdk/hal/hdmicec/IHdmiCec.h
#                             the generated client stubs and server bases
#                             DriverAidlImpl.cpp is compiled against
#   common/0.2.0.0/include    com/rdk/hal/PropertyValue.h
#                             the frozen common import the hdmicec 0.1.0.0
#                             snapshot was generated and pinned against -
#                             0.2.0.0, deliberately not the newer common release
#   common/current            halcompat.h
#                             getService and isCompatible, which do NOT live
#                             under the 0.2.0.0 root, so the two snapshot roots
#                             alone never reach them
#
# AND EACH CANDIDATE IS SKIPPED WHEN HALIF_PREFIX IS UNSET, with the same
# $(if $(strip ...)) every probe in this file uses: an unset prefix must
# contribute no candidate at all rather than an absolute /hdmicec/... path that a
# host directory of that name would satisfy, which is the failure mode that makes
# N6 more than a cosmetic diagnostic.  configure.ac guards its three the same
# way, with `test -n`.
#
# WHY PRODUCTION_HEADER_ROOT_IS_TEST_TREE IS NOT EXTENDED TO THESE FOUR ROOTS.
# That refusal exists because this repository really does carry test-scope
# substitutes for the other two roots' headers - mocks/telemetry_busmessage_sender.h
# and stubs/ccec/drivers/hdmi_cec_driver.h - and the legacy HAL root keeps
# ../drivers/include as a candidate, which can reach the second through a
# symlink.  Neither mocks/ nor stubs/ carries com/rdk/hal/hdmicec/IHdmiCec.h,
# com/rdk/hal/PropertyValue.h or halcompat.h, and neither can:
# mocks/hdmicec/fake_hdmi_cec_aidl_service.h is a CONSUMER of the generated
# stubs, reaching <com/rdk/hal/hdmicec/BnHdmiCec.h> through these very roots
# rather than standing in for them.  The roots here are also fixed subpaths with
# no in-tree candidate and no override variable, so a HALIF_PREFIX aimed at
# either tree simply fails the witness probes and lands in the named error below.
# A refusal on these roots could therefore never fire, and a check that cannot
# fire is worse than no check: it reads as a guarantee.
#
# $(1) the candidate root, empty when its prefix was unset, and $(2) the header
# beneath it that proves it.  Expands to the root when the header is there and to
# nothing otherwise - the same $(wildcard)/$(patsubst)/$(firstword) reduction the
# Binder probes above spell out inline.  It is a function here only because four
# roots share it and a copy per root would be four places to change.
ccec_halif_root = $(firstword $(patsubst %/$(2),%,$(wildcard $(addsuffix /$(2),$(1)))))

HALIF_HDMICEC_INCLUDE_CANDIDATE := $(if $(strip $(HALIF_PREFIX)),$(HALIF_PREFIX)/hdmicec/0.1.0.0/include)
HALIF_COMMON_INCLUDE_CANDIDATE := $(if $(strip $(HALIF_PREFIX)),$(HALIF_PREFIX)/common/0.2.0.0/include)
HALCOMPAT_INCLUDE_CANDIDATE := $(if $(strip $(HALIF_PREFIX)),$(HALIF_PREFIX)/common/current)

HALIF_HDMICEC_INCLUDE_DIR := $(call ccec_halif_root,$(HALIF_HDMICEC_INCLUDE_CANDIDATE),com/rdk/hal/hdmicec/IHdmiCec.h)
HALIF_COMMON_INCLUDE_DIR := $(call ccec_halif_root,$(HALIF_COMMON_INCLUDE_CANDIDATE),com/rdk/hal/PropertyValue.h)
HALCOMPAT_INCLUDE_DIR := $(call ccec_halif_root,$(HALCOMPAT_INCLUDE_CANDIDATE),halcompat.h)

# THE STUB LIBRARY DIRECTORY, under the SAME $(origin) test the Binder library
# directory above uses and for the same reason: `?=` assigns only when nothing
# has set the variable, so skipping the probe unless $(origin) is `undefined`
# preserves exactly what the `?=` this replaces guaranteed.  A deployment that
# stages the stub libraries away from the headers and names HALIF_LIB_DIR itself
# - the split staging layout the orchestrator contract at the top of this file
# documents - keeps working untouched and unprobed, which is also how
# configure.ac treats it, with `test -z "$HALIF_LIB_DIR"` before its own
# default.
#
# The single candidate is HALIF_PREFIX/lib/halif, the one layout configure.ac
# derives, and it is probed for libhdmicec-v0.1.0.0-cpp.so beneath it rather than
# for the directory, because that file is what -lhdmicec-v0.1.0.0-cpp resolves
# against and a lib/halif without it cannot satisfy the edge.  This is the
# stricter of the two builds here, deliberately and in line with the Binder
# library directory a few lines up, which errors rather than falling back to the
# ambient LDFLAGS as configure does: a library directory this file cannot name is
# one the build log cannot record.  A sysroot that stages the stub libraries on
# the loader's default search path is served by naming HALIF_LIB_DIR at whatever
# directory holds them, which the override path above accepts without probing.
#
# Whether it was supplied is recorded BEFORE the assignment below, because after
# it $(origin) reads `file` either way and the message would then claim to have
# probed a value it was handed.  This file's other messages report what they
# actually looked at, and this is what keeps this one doing the same.
HALIF_LIB_DIR_SUPPLIED := $(if $(filter undefined,$(origin HALIF_LIB_DIR)),,yes)

ifeq ($(origin HALIF_LIB_DIR),undefined)
HALIF_LIB_CANDIDATES := \
	$(if $(strip $(HALIF_PREFIX)),$(HALIF_PREFIX)/lib/halif)

HALIF_LIB_DIR := $(call ccec_halif_root,$(HALIF_LIB_CANDIDATES),libhdmicec-v0.1.0.0-cpp.so)
endif

# The per-root state the message below reads.  $(1) is a resolved root: expands to
# the value in brackets when it resolved and to the word MISSING when it did not,
# so one error marks each root individually instead of only reporting that
# something was wrong.
ccec_halif_state = $(if $(strip $(1)),resolved [$(strip $(1))],MISSING)

# The unresolved roots on their own, as one summary line, so the message answers
# "which one" without the reader diffing four.  $(1) is the resolved root, $(2)
# the candidate probed for it, and $(3) the subpath to name when HALIF_PREFIX was
# unset and there was therefore no candidate to name at all.
ccec_halif_unresolved = $(if $(strip $(1)),,$(if $(strip $(2)),$(2),$(3)))

HALIF_AIDL_UNRESOLVED := \
	$(call ccec_halif_unresolved,$(HALIF_HDMICEC_INCLUDE_DIR),$(HALIF_HDMICEC_INCLUDE_CANDIDATE),HALIF_PREFIX/hdmicec/0.1.0.0/include) \
	$(call ccec_halif_unresolved,$(HALIF_COMMON_INCLUDE_DIR),$(HALIF_COMMON_INCLUDE_CANDIDATE),HALIF_PREFIX/common/0.2.0.0/include) \
	$(call ccec_halif_unresolved,$(HALCOMPAT_INCLUDE_DIR),$(HALCOMPAT_INCLUDE_CANDIDATE),HALIF_PREFIX/common/current) \
	$(call ccec_halif_unresolved,$(HALIF_LIB_DIR),$(HALIF_LIB_CANDIDATES),HALIF_PREFIX/lib/halif)

define HALIF_AIDL_ROOTS_NOT_FOUND
the staged rdk-halif-aidl tree did not supply every root libRCEC is built
against, so libRCEC cannot be built with the AIDL HDMI CEC back-end, and the
back-end is not optional: this library always contains both HAL back-ends and
chooses between them at run time, so this dependency cannot be disabled.
  roots that did not resolve: [$(strip $(HALIF_AIDL_UNRESOLVED))]
Export HALIF_PREFIX to the staged rdk-halif-aidl tree - and HALIF_LIB_DIR as
well, but only when the stub libraries are staged apart from its headers -
exactly as the RDK build orchestrator already exports OPENSOURCE_BASE and
GLIB_LIBRARY_PATH.  Each root is a FIXED subpath beneath HALIF_PREFIX, the same
three configure.ac accepts and no others, and each was probed for the header or
library it has to supply rather than for the bare directory:
  1  $$(HALIF_PREFIX)/hdmicec/0.1.0.0/include  probed for com/rdk/hal/hdmicec/IHdmiCec.h
     $(call ccec_halif_state,$(HALIF_HDMICEC_INCLUDE_DIR))
  2  $$(HALIF_PREFIX)/common/0.2.0.0/include   probed for com/rdk/hal/PropertyValue.h
     $(call ccec_halif_state,$(HALIF_COMMON_INCLUDE_DIR))
  3  $$(HALIF_PREFIX)/common/current           probed for halcompat.h
     $(call ccec_halif_state,$(HALCOMPAT_INCLUDE_DIR))
  4  $$(HALIF_PREFIX)/lib/halif                probed for libhdmicec-v0.1.0.0-cpp.so
     $(if $(HALIF_LIB_DIR_SUPPLIED),supplied as [$(strip $(HALIF_LIB_DIR))] and taken as given rather than probed,$(call ccec_halif_state,$(HALIF_LIB_DIR)))
with HALIF_PREFIX=[$(HALIF_PREFIX)] and HALIF_LIB_DIR=[$(HALIF_LIB_DIR)],
which left these candidates to probe: [$(strip $(HALIF_HDMICEC_INCLUDE_CANDIDATE)
$(HALIF_COMMON_INCLUDE_CANDIDATE) $(HALCOMPAT_INCLUDE_CANDIDATE)
$(HALIF_LIB_CANDIDATES))]
Roots 2 and 3 live under common/, OUTSIDE the hdmicec/-only sparse checkout
rdk-halif-aidl is documented with, so a sparse clone resolves root 1 and fails
those two until common/ is materialised; configure.ac carries the same note for
the same reason.  A HALIF_LIB_DIR supplied from the environment or the command
line is taken as given rather than probed, and line 4 above says which of the two
happened to the value reported there.
endef

# Guarded by BINDER_HEADER_GOALS - the same variable the Binder and production
# header checks above use, and the same condition rather than a similar one: it
# is empty exactly when the only goal is `clean`.  Removing objects has to stay
# possible on a host that staged nothing, and every other goal compiles or links
# and cannot honestly be attempted without these four roots.  One error is
# raised for all four, so a tree missing three of them is diagnosed in one pass
# rather than one make invocation at a time.
ifneq ($(strip $(BINDER_HEADER_GOALS)),)
ifneq ($(strip $(HALIF_AIDL_UNRESOLVED)),)
$(error $(HALIF_AIDL_ROOTS_NOT_FOUND))
endif
endif
# ---------------------------------------------------------------------------

# The four include roots the AIDL back-end is compiled against: the three probed
# HALIF roots above, in the order configure.ac assembles them, then the probed
# Binder header root.
#
# $(addprefix -I,$(strip ...)) rather than a bare -I on each, for the same reason
# the INCLUDE list below states it: an empty value emits a lone -I, which the
# compiler driver reads as taking the NEXT token as its directory, so a missing
# root would fail naming the wrong thing.  On any goal that compiles, the named
# errors above have already fired for an empty value; this keeps the flags
# well-formed on `clean`, which is the goal those errors deliberately skip.
AIDL_INCLUDE = $(addprefix -I,$(strip $(HALIF_HDMICEC_INCLUDE_DIR)))	\
	$(addprefix -I,$(strip $(HALIF_COMMON_INCLUDE_DIR)))	\
	$(addprefix -I,$(strip $(HALCOMPAT_INCLUDE_DIR)))	\
	$(addprefix -I,$(strip $(BINDER_INCLUDE_DIR)))	\

# The four direct link edges, in dependency order, and the reason they are wrapped.
#
# All four are required to be direct DT_NEEDED entries of the shipping
# libRCEC.so: AAP 0.4.2 names them as four unconditional edges and AAP 0.9.4
# verifies the produced ELF against the expected NEEDED set.  Naming them on the
# link line does not achieve that on its own.  --as-needed is the effective
# default in the GCC configurations this and the RDK toolchains use - measured
# here, where a plain "g++ -shared ... -lm" leaves no DT_NEEDED for libm at all -
# and it drops the edge for any library that no symbol in the output references.
# libcommon-v0.2.0.0-cpp is exactly that library: PropertyValue is reached only
# through the hdmicec stub library's own interface, so with --as-needed in force
# the fourth edge disappeared from the ELF and deleting its token from this line
# would have changed nothing observable.  That silence is what the wrapping below
# closes, and it is why CI asserts four SONAMEs rather than three.
#
# --no-as-needed is scoped to these four and --as-needed is restored immediately
# after them, so the libraries the compiler driver appends of its own accord
# (libstdc++, libm, libgcc_s, libc) keep exactly the disposition they have today
# and nothing else about this link changes.
#
# libbinder's own transitive closure - liblog, libbase, libcutils,
# libcutils_sockets - is staged and made loadable rather than named here, because
# nothing in the middleware references it.
AIDL_LIBS = -Wl,--no-as-needed -lhdmicec-v0.1.0.0-cpp -lcommon-v0.2.0.0-cpp -lbinder -lutils -Wl,--as-needed
# $(addprefix) rather than a bare -L on either probed directory: an empty value
# would otherwise emit a lone -L, which the driver reads as taking the NEXT
# token - the first entry of AIDL_LIBS - as its directory, turning a missing
# prefix into a link failure that names the wrong thing.  The goal-guarded
# errors above are what an empty value normally reaches first; this keeps the
# flags well-formed on any path that does not.
#
# The HALIF half is wrapped for the first time here because HALIF_LIB_DIR is now
# probed rather than defaulted to HALIF_PREFIX/lib/halif.  The old default was
# never empty - it was merely wrong when the prefix was unset, naming the host's
# own /lib/halif, which is the link half of QA finding N6 - so the bare -L was
# safe by accident.  A probed value can legitimately be empty on `clean`, and the
# wrapper is what keeps that invocation's flags well-formed.
AIDL_LDFLAGS = $(addprefix -L,$(strip $(HALIF_LIB_DIR))) $(addprefix -L,$(strip $(BINDER_LIB_DIR)))

# -I../drivers/include is deliberately absent from this list now: it is the last
# candidate of LEGACY_HAL_INCLUDE_CANDIDATES above instead, so the in-tree layout
# still resolves if it is ever populated but does so through the probe, and is
# therefore subject to the test-tree refusal.  An unconditional -I here would be
# outside both.
#
# $(addprefix -I,$(strip ...)) rather than a bare -I on each resolved root, for the
# same reason AIDL_LDFLAGS above uses it: an empty value would emit a lone -I,
# which the compiler driver reads as taking the NEXT token as its directory, so a
# missing root would fail naming the wrong thing.  On any goal that compiles, the
# named errors above have already fired for an empty value; this keeps the flag
# well-formed on `clean`, which is the goal those errors deliberately skip.
INCLUDE = -I.\
	-I../include	\
	-I../../osal/include	\
	$(addprefix -I,$(strip $(TELEMETRY_INCLUDE_DIR)))	\
	$(addprefix -I,$(strip $(LEGACY_HAL_INCLUDE_DIR)))	\
	$(AIDL_INCLUDE)


CFLAGS+= $(INCLUDE)

# ---------------------------------------------------------------------------
# Binder wire ABI, and the Binder kernel UAPI, as build prerequisites.
#
# This file has no configure step, so the two decisions configure makes for the
# Autotools build are made here in make, from the same evidence and honouring the
# same override variable, so that both build systems settle on the same answer:
#
#   BINDER_IPC_32BIT   yes | no.  Optional override.  When it is not set, the
#                      value is DERIVED by measuring the staged libbinder (see
#                      below).  When it is set and disagrees with what was
#                      measured, the build stops: silently trusting either side
#                      would produce a library whose marshalling does not match
#                      the library it links against.
#
# Why the macro matters at all: it selects BINDER_CURRENT_PROTOCOL_VERSION (7
# with it, 8 without) and the width of binder_size_t / binder_uintptr_t (__u32
# with it, __u64 without) in the very UAPI structures DriverAidlImpl marshals
# into the driver.  A middleware compiled without it against an SDK built with
# it writes 64-bit fields into a 32-bit layout: nothing fails to build, and
# nothing fails at link, so the mismatch is invisible until the driver rejects
# or misreads the transaction at run time.
#
# How it is derived: android::Parcel::ipcSetDataReference takes a
# `const binder_size_t *`, so the staged libbinder's own exported symbol carries
# the decision in its Itanium mangling.  This reads the answer out of the
# artefact that will actually be linked, rather than out of a build variable that
# may not describe it.  Its declaration is
#
#   void ipcSetDataReference(const uint8_t *data, size_t dataSize,
#                            const binder_size_t *objects, size_t objectsCount,
#                            release_func relFunc);
#
# and TWO DIFFERENT WIDTHS appear in that one mangling.  Confusing them is the
# trap this detector exists to avoid, because only one of them is the wire ABI:
#
#   the DATA MODEL of the target.  `size_t` appears twice - `dataSize` and
#   `objectsCount` - and mangles `m` (unsigned long) on an LP64 target such as
#   x86_64 or aarch64 but `j` (unsigned int) on an ILP32 target such as armhf or
#   i386, which is a large part of the SOC population this build path serves.  It
#   describes the target's data model and says nothing whatever about the wire,
#   so BOTH forms are accepted here.  Because the two occurrences are the same
#   C++ type in one signature they must agree with each other: a candidate whose
#   two `size_t` characters differ is some other function, and keying the wire
#   decision off it would be keying it off a misparse, so it is rejected.
#
#   the WIRE ABI, which is what BINDER_IPC_32BIT actually selects.  Only the
#   middle `const binder_size_t *` carries it - `PKj` (const unsigned int *) for
#   the 32-bit wire ABI, `PKy` (const unsigned long long *) for the 64-bit one -
#   and it is therefore the sole basis for the decision below.  All four
#   combinations occur in the field: an ILP32 middleware speaks either wire ABI
#   and so does an LP64 one, which is exactly why the two widths cannot be read
#   as one.
#
# The symbol looked for is therefore `EPKh<s>PK<w><s>`, with the same <s> in both
# places, and what is carried forward from it is <w> alone.
#
# Anything requested other than a bare `clean` requires these prerequisites: a
# clean must stay possible on a host that has no staged SDK at all.
BINDER_ABI_LIB := $(BINDER_LIB_DIR)/libbinder.so

ifeq ($(strip $(MAKECMDGOALS)),clean)
BINDER_PREREQ_REQUIRED := no
else
BINDER_PREREQ_REQUIRED := yes
endif

# The symbol table is read the same way by the derivation and by the diagnostic
# in its error path, so that a failure reports exactly what the derivation looked
# at.  Recursively assigned, so it is text substituted into each of them rather
# than a command run here.
BINDER_ABI_SYMBOLS = { nm -D --defined-only "$(BINDER_ABI_LIB)" 2>/dev/null || \
		readelf -sW "$(BINDER_ABI_LIB)" 2>/dev/null; }

# Holds <w> - `PKj` or `PKy` - taken from the one candidate whose two <s>
# characters agree.  The agreement is enforced by the backreference in the grep
# pattern, which is why the pattern is a POSIX BRE; the sed then discards the
# data model and keeps the wire parameter, which is the whole of the decision.
#
# `sort -u | head -n 2` bounds the result exactly as before: a library exporting
# two symbols that DISAGREE about the wire reduces to two words, matches neither
# value below, and lands in `unknown` - the hard error - instead of silently
# taking the first.  Two that agree about the wire and differ only in data model
# reduce to one word, which is the right answer, because they make the same
# decision.
BINDER_ABI_WIRE_MANGLING := $(shell if [ -r "$(BINDER_ABI_LIB)" ]; then \
		$(BINDER_ABI_SYMBOLS) | \
		grep -o '19ipcSetDataReferenceEPKh\([jm]\)PK[jy]\1' | \
		sed -n 's/^19ipcSetDataReferenceEPKh[jm]PK\([jy]\)[jm]$$/PK\1/p' | \
		sort -u | head -n 2 | tr '\n' ' '; \
	fi)

# Every ipcSetDataReference mangling the staged library really exports, whatever
# its shape.  Recursively assigned, so the symbol table is read a second time
# only if the error below actually fires, and then only to report what was found
# in place of a recognised candidate - a mismatched-`size_t` symbol, a differently
# shaped signature, or nothing at all.
BINDER_ABI_CANDIDATES = $(shell if [ ! -r "$(BINDER_ABI_LIB)" ]; then \
		echo "none, $(BINDER_ABI_LIB) is not readable"; \
	else \
		ccec_found=$$($(BINDER_ABI_SYMBOLS) | \
			grep -o '19ipcSetDataReference[A-Za-z0-9_]*' | sort -u | \
			tr '\n' ' ' | sed 's/ *$$//'); \
		if [ -z "$$ccec_found" ]; then echo none; else echo "$$ccec_found"; fi; \
	fi)

ifeq ($(strip $(BINDER_ABI_WIRE_MANGLING)),PKj)
BINDER_ABI_DETECTED := yes
else
ifeq ($(strip $(BINDER_ABI_WIRE_MANGLING)),PKy)
BINDER_ABI_DETECTED := no
else
BINDER_ABI_DETECTED := unknown
endif
endif

ifeq ($(strip $(BINDER_IPC_32BIT)),)
ifeq ($(BINDER_ABI_DETECTED),unknown)
ifeq ($(BINDER_PREREQ_REQUIRED),yes)
$(error Cannot determine the Binder wire ABI of $(BINDER_ABI_LIB). The staged libbinder must export exactly one android::Parcel::ipcSetDataReference whose mangling is 19ipcSetDataReferenceEPKh<s>PK<w><s>, where <s> is the target's size_t - `j` on an ILP32 target such as armhf or i386, `m` on an LP64 one such as aarch64 or x86_64 - the SAME character in both places, and <w> alone is the wire ABI: `j` for the 32-bit one, so ...EPKhjPKjj on an ILP32 target and ...EPKhmPKjm on an LP64 one, or `y` for the 64-bit one, so ...EPKhjPKyj and ...EPKhmPKym respectively. Exported instead: $(BINDER_ABI_CANDIDATES). Check that BINDER_SDK_DIR (or BINDER_LIB_DIR) points at the staged linux_binder_idl tree built for this target and that nm or readelf is on PATH, or state the ABI explicitly with `make BINDER_IPC_32BIT=yes` / `make BINDER_IPC_32BIT=no` to match the SDK you staged)
endif
BINDER_IPC_32BIT := no
BINDER_ABI_SOURCE := an undetermined ABI on a clean-only invocation
else
BINDER_IPC_32BIT := $(BINDER_ABI_DETECTED)
BINDER_ABI_SOURCE := the staged libbinder at $(BINDER_ABI_LIB)
endif
else
ifeq ($(filter yes no,$(strip $(BINDER_IPC_32BIT))),)
$(error BINDER_IPC_32BIT was set to "$(BINDER_IPC_32BIT)", which is not a recognised value. Use BINDER_IPC_32BIT=yes for the 32-bit Binder wire ABI or BINDER_IPC_32BIT=no for the 64-bit one, or leave it unset to derive it from the staged libbinder)
endif
ifneq ($(BINDER_ABI_DETECTED),unknown)
ifneq ($(strip $(BINDER_IPC_32BIT)),$(BINDER_ABI_DETECTED))
$(error BINDER_IPC_32BIT=$(strip $(BINDER_IPC_32BIT)) disagrees with the staged libbinder at $(BINDER_ABI_LIB), which was measured as BINDER_IPC_32BIT=$(BINDER_ABI_DETECTED). One of the two is wrong: stage an SDK built for the ABI you asked for, or drop the override and let it be derived. Building either way would marshal Binder transactions in a layout libbinder does not use)
endif
endif
BINDER_ABI_SOURCE := the explicit BINDER_IPC_32BIT=$(strip $(BINDER_IPC_32BIT)) override
endif

ifeq ($(strip $(BINDER_IPC_32BIT)),yes)
AIDL_ABI_CFLAGS := -DBINDER_IPC_32BIT
BINDER_ABI_PROTOCOL := 7
else
AIDL_ABI_CFLAGS :=
BINDER_ABI_PROTOCOL := 8
endif

# Into CFLAGS, which is what every compile command below expands, so that every
# translation unit that can see a Binder header sees the same wire layout.
CFLAGS+= $(AIDL_ABI_CFLAGS)

$(info libRCEC Binder wire ABI: BINDER_IPC_32BIT=$(strip $(BINDER_IPC_32BIT)), protocol $(BINDER_ABI_PROTOCOL), from $(BINDER_ABI_SOURCE))

# The two production header roots are announced beside the wire ABI so that a
# BUILD LOG RECORDS WHICH HEADERS THE ARTEFACT WAS CERTIFIED AGAINST.  P3-HAND was
# reported against an artefact that built cleanly; what the log did not say was
# that the telemetry header it compiled was a no-op macro file.  These two lines
# are what makes that answerable after the fact, from the log alone, without
# re-deriving the include path.  Values are printed even on a bare `clean`, where
# they can legitimately be empty, because the resolution errors are skipped there.
$(info libRCEC telemetry header root: [$(strip $(TELEMETRY_INCLUDE_DIR))])
$(info libRCEC legacy HDMI CEC HAL header root: [$(strip $(LEGACY_HAL_INCLUDE_DIR))])

# The AIDL back-end's own four roots are announced in the same form and for the
# same reason, and QA finding N6 is the case of this banner not doing it: it
# named the telemetry root, the legacy HAL root and the derived wire ABI, so a
# mis-staged HALIF_PREFIX was invisible in the log and the first mention of it
# was a header-not-found from the compiler.  These lines are what makes the AIDL
# half of "which headers was this artefact certified against" answerable from the
# log alone, without re-deriving the include path; the fourth records which
# staged libhdmicec-v0.1.0.0-cpp.so the unconditional link edges resolved
# against.  The labels are configure.ac's own, so the two builds' logs read
# alike.  Values are printed even on a bare `clean`, where they can legitimately
# be empty, because the resolution error is skipped there.
$(info libRCEC generated AIDL stub include root: [$(strip $(HALIF_HDMICEC_INCLUDE_DIR))])
$(info libRCEC frozen common 0.2.0.0 include root: [$(strip $(HALIF_COMMON_INCLUDE_DIR))])
$(info libRCEC halcompat.h include root: [$(strip $(HALCOMPAT_INCLUDE_DIR))])
$(info libRCEC AIDL stub library directory: [$(strip $(HALIF_LIB_DIR))])

# The Binder kernel UAPI is a HARD PREREQUISITE of libRCEC, not an optional
# extra.  DriverAidlImpl's transport preflight marshals struct binder_write_read
# and struct binder_version and compares BINDER_CURRENT_PROTOCOL_VERSION, so a
# build without the header cannot produce a working AIDL back-end - and a build
# that quietly degraded instead of stopping would ship a library that links
# Binder, reports success, and can never select the AIDL back-end on any
# platform.  The probe exercises the three constructs the preflight uses rather
# than testing for the header's presence, so a stub or truncated header fails
# here rather than mid-compile.
ifeq ($(BINDER_PREREQ_REQUIRED),yes)
# The header is pulled in with -include rather than with a directive in the probe
# text, so that no `#` has to survive make's own comment stripping on its way to
# the shell.
BINDER_UAPI_PROBE := $(shell printf '%s\n' \
		'int ccec_binder_uapi_probe(void);' \
		'int ccec_binder_uapi_probe(void) {' \
		'    struct binder_write_read bwr;' \
		'    struct binder_version ver;' \
		'    binder_uintptr_t addr = (binder_uintptr_t)0;' \
		'    bwr.write_size = 0;' \
		'    bwr.write_buffer = addr;' \
		'    ver.protocol_version = BINDER_CURRENT_PROTOCOL_VERSION;' \
		'    return (int)(bwr.write_size + (unsigned long)ver.protocol_version);' \
		'}' | $(CXX) -x c++ -include linux/android/binder.h -c - $(CFLAGS) -o /dev/null > /dev/null 2>&1 && echo yes || echo no)
ifneq ($(strip $(BINDER_UAPI_PROBE)),yes)
$(error The Binder kernel UAPI is unusable: linux/android/binder.h could not be compiled with struct binder_write_read, struct binder_version, binder_uintptr_t and BINDER_CURRENT_PROTOCOL_VERSION. libRCEC carries the AIDL back-end unconditionally and cannot be built without it. Install the kernel UAPI headers for the target (Debian/Ubuntu linux-libc-dev; Yocto/OE the kernel-headers or linux-libc-headers recipe already present in most sysroots), or add the sysroot that provides linux/android/binder.h to CFLAGS. Reproduce with: $(CXX) -x c++ -include linux/android/binder.h -fsyntax-only /dev/null $(CFLAGS))
endif
endif
# ---------------------------------------------------------------------------

LDFLAGS+= -L$(OPENSOURCE_BASE)/lib
LDFLAGS+=-L$(GLIB_LIBRARY_PATH)/
LDFLAGS+=$(GLIBS)
LDFLAGS += -L. -lpthread

# FULL RELRO, the link half of QA finding P5-05.
#
# Measured on the artefact this file produced before the change: `readelf -d
# install/lib/libRCEC.so` listed 30 dynamic entries and NEITHER DT_FLAGS NOR
# DT_FLAGS_1 was among them, so BIND_NOW was absent and the GNU_RELRO segment the
# linker emits by default only covered the ordinary relro region - partial RELRO,
# with a writable PLT/GOT for the whole life of the process.
#
#   -z relro   is already the default in these toolchains and is named anyway, so
#              that a toolchain configured with --disable-relro or an inherited
#              -z norelro does not quietly drop it.  Naming it costs nothing and
#              makes the request explicit at the point the artefact is produced.
#   -z now     is what was actually missing.  It sets DF_BIND_NOW/DF_1_NOW, which
#              makes the loader resolve every relocation at load time so the
#              GOT can be mapped read-only afterwards - full RELRO.  Without it
#              the GOT stays writable and remains an overwrite target.
#
# It is a variable rather than a literal on the link line so that it is greppable
# from CI and reads as a stated policy; it is expanded on the link command below,
# where the ELF is produced.  Position on the link line is immaterial for these
# two options, so nothing else about this link changes.
HARDENING_LDFLAGS = -Wl,-z,relro,-z,now

all: clean library
	@echo "Build Finished...."

library: $(OBJS)
	@echo "Building $(LIBNAMEFULL) ...."
	mkdir -p install/lib
	$(CXX) $(OBJS) $(CFLAGS) -shared -o install/lib/$(LIBNAMEFULL) $(HARDENING_LDFLAGS) $(LDFLAGS) $(AIDL_LDFLAGS) $(AIDL_LIBS)

%.o: %.cpp
	@echo "Building $@ ...."
	$(CXX) -c $<  $(CFLAGS) -o $@

clean:
	@echo "Cleaning the directory..."
	@$(RM) $(OBJS) install
