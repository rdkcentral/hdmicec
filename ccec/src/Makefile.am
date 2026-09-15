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
SUBDIRS =

# The AIDL/Binder HDMI CEC back-end is compiled into libRCEC unconditionally, so
# that both back-ends are present in every build for every SOC vendor and the
# choice between them is made at runtime rather than at build time.  There is
# deliberately no --enable switch and no Automake conditional guarding it.
#
# @AIDL_CPPFLAGS@ carries the four include roots the generated stubs need,
# @AIDL_ABI_CPPFLAGS@ the Binder WIRE ABI define, and @AIDL_CXXFLAGS@ the C++17 dialect
# flag configure settled on.  All three are resolved by configure from the HALIF and
# Binder SDK prefixes.
#
# @AIDL_ABI_CPPFLAGS@ IS NOT OPTIONAL HERE AND ITS ABSENCE IS NOT A STYLE MATTER.
# configure derives it by RESOLVING android::Parcel::ipcSetDataReference against the
# staged libbinder - the mangling of that symbol differs between the two Binder wire
# ABIs - and substitutes either -DBINDER_IPC_32BIT or nothing.  That define selects
# BINDER_CURRENT_PROTOCOL_VERSION (7 against 8) and the width of binder_size_t and
# binder_uintptr_t, so it changes the LAYOUT THE BINDER HEADERS DECLARE rather than
# merely enabling code.  Every target that compiles a translation unit which sees a
# Binder header must therefore carry it, which is the contract configure.ac states
# beside the substitution and which names this file as one of its consumers.
#
# Omitting it here compiled the SHIPPED library against protocol-8 layouts while the two
# test Makefile.am files - which do carry it - compiled their own copies of the same
# headers against protocol 7.  The two disagreements that produces are both silent at
# build time: the middleware's preflight compares the driver's reported protocol version
# against a constant this file got wrong, so a correctly provisioned Binder platform is
# reported as "AIDL absent" and every SOC with an AIDL HAL falls back to the legacy
# back-end; and where a transaction is reached anyway, the structures crossing to the
# kernel carry fields of the wrong width.  Neither shows up in a test suite whose own
# translation units were compiled with the flag.
#
# There is deliberately no literal spelling of the define here, and that is the OPPOSITE
# convention from -std=c++17 below.  The dialect is a property of THIS LIBRARY's source -
# it needs <optional> whatever host builds it - whereas the wire ABI is a property of the
# LIBBINDER THIS BUILD LINKS, which only configure can measure.  A literal here would be
# a second, unmeasured opinion about a value that must agree with the staged SDK exactly,
# and disagreement is what this flag exists to prevent.  The hand-written
# ccec/src/Makefile, which has no configure step, derives the same value for itself from
# the staged libbinder's exported symbol and folds it into CFLAGS as AIDL_ABI_CFLAGS.
#
# -std=c++17 is spelled literally here, in addition to @AIDL_CXXFLAGS@, and the
# duplication is deliberate.  The generated AIDL headers include <optional> and
# declare std::optional parameters, so the dialect is a requirement of this
# library rather than of the host that happens to build it, and the literal is
# what states that requirement in the build file itself instead of leaving it to
# whatever dialect the compiler defaults to - a default that already covers
# C++17 would otherwise hide the requirement until a SOC toolchain with an older
# one reached DriverAidlImpl.cpp.  The literal is also the only form available to
# the hand-written ccec/src/Makefile, which has no configure step to substitute
# into; the two test Makefile.am files name it the same way, so this is the
# fourth of four consumers rather than a new convention.
#
# @AIDL_CXXFLAGS@ carries the dialect flag configure accepted, which is C++17 or
# newer and need not be this literal spelling.  configure settles it by probing
# rather than by naming: a -std= the integrator supplied in CXXFLAGS is probed as
# given and adopted verbatim when it compiles the C++17 constructs the AIDL
# headers use, and configuration stops when it does not, because Automake places
# CXXFLAGS after AM_CXXFLAGS and this package cannot override it; with no such
# flag supplied, -std=c++17, -std=gnu++17 and -std=c++1z are tried in that order
# and the first that compiles is adopted.  The substitution can therefore read
# -std=gnu++17, -std=c++1z or a newer dialect, which is why the order of the two
# on the line below is load-bearing: both reach the compile command, the
# substitution lands last, and the last -std= governs.  That is also why neither
# is redundant - the literal names the requirement, and the substitution is the
# spelling this compiler was measured to accept.
#
# The same four-of-four rule governs @AIDL_ABI_CPPFLAGS@, by a different route: the
# two test Makefile.am files consume the substitution and the hand-written
# ccec/src/Makefile derives the equivalent value itself, so this line is again the
# fourth consumer of four.  The difference is which copy is authoritative -
# configure's measurement of the staged libbinder, never a literal in a Makefile -
# for the reason stated in the block above.  It is spelled as a substitution rather
# than a literal because a wrong wire ABI does not fail to build: the library would
# open the driver and be rejected, or truncate a buffer address, so the value must
# come from the measurement and must reach every translation unit in this library.
AM_CXXFLAGS = -pthread -Wall -std=c++17 @AIDL_CXXFLAGS@ -I${top_srcdir}/osal/include -I${top_srcdir}/ccec/include @AIDL_CPPFLAGS@ @AIDL_ABI_CPPFLAGS@

lib_LTLIBRARIES = libRCEC.la

AM_LDFLAGS = -ltelemetry_msgsender

libRCEC_la_SOURCES = CECFrame.cpp \
                     Util.cpp \
                     DriverImpl.cpp \
                     DriverAidlImpl.cpp \
                     LibCCEC.cpp \
                     Bus.cpp \
                     OpCode.cpp \
                     Connection.cpp \
                     Driver.cpp \
                     MessageDecoder.cpp

# THE FOUR AIDL/BINDER EDGES ARE FORCED INTO DT_NEEDED FROM HERE, AND THE REASON
# IS MEASURED RATHER THAN STYLISTIC.
#
# All four have to be direct DT_NEEDED entries of the produced libRCEC: AAP 0.4.2
# names four unconditional edges and AAP 0.9.4 checks the ELF against the
# expected NEEDED set.  --as-needed is the effective default in these toolchains
# and it drops the edge for any library that no symbol in the output references.
# libcommon-v0.2.0.0-cpp is that library - PropertyValue is reached only through
# the hdmicec stub library's own interface - so it was absent from DT_NEEDED and
# deleting its token from AIDL_LIBS would have changed nothing observable.
#
# Closing that needs --no-as-needed to take effect on those -l options, and
# neither of the two obvious spellings does.  Measured on this tree:
#
#   * a bare "-Wl,--no-as-needed" is classified by libtool into $compiler_flags,
#     and archive_cmds is
#         $CC -shared $pic_flag $libobjs $deplibs $compiler_flags $wl-soname ...
#     so it lands AFTER the whole $deplibs list, where it cannot affect it;
#   * and Automake rejects it in libRCEC_la_LIBADD outright ("linker flags such
#     as ... belong in libRCEC_la_LDFLAGS").
#
# One comma-joined -Wl group carries its own scope with it: libtool splits it on
# commas and re-emits one -Wl, option per element IN ORDER, so --no-as-needed,
# the four libraries and the restoring --as-needed stay together wherever the
# group as a whole is placed.  That makes the wrapping independent of libtool's
# argument classification, which is what the two failures above turn on.
#
# --as-needed is restored immediately after the four, so the libraries the
# compiler driver appends of its own accord keep the disposition they have today.
# @AIDL_LIBS@ below is kept as well, so configure stays the one place that
# decides the list and libRCEC.la's dependency_libs still records the four edges
# the ordinary way; both forms name the same four libraries, ld sees each name
# twice and emits one DT_NEEDED per library.  The literal names here must
# therefore track AIDL_LIBS in configure.ac - a divergence fails the link on a
# library it cannot find, by name, rather than quietly dropping an edge.
#
# -Wl,-z,relro,-z,now IS THE WHOLE OF THIS FILE'S SHARE OF QA FINDING P5-05, AND
# THE ABSENCE OF -O2 AND -D_FORTIFY_SOURCE FROM AM_CXXFLAGS ABOVE IS DELIBERATE.
#
# The link half.  P5-05 measured the installed Autotools library and found no
# DT_FLAGS and no DT_FLAGS_1 entry at all, so BIND_NOW was absent and the default
# GNU_RELRO segment left the GOT writable for the life of the process - partial
# RELRO.  -z now sets DF_BIND_NOW/DF_1_NOW, which resolves every relocation at load
# time so the GOT can be mapped read-only; -z relro is already the default here and
# is named alongside it so that a toolchain configured with --disable-relro, or an
# inherited -z norelro, cannot silently drop it.  It is spelled as one comma-joined
# -Wl group for the same libtool reason the forcing group below relies on.
#
# The compile half is NOT here, and putting it here would be a measurable
# regression rather than a style choice.  tests/L1Tests/run_coverage.sh configures
# the instrumented build with CXXFLAGS="-fprofile-arcs -ftest-coverage" and no -O
# flag, so the coverage build is implicitly -O0.  Automake emits
#     $(AM_CXXFLAGS) $(CXXFLAGS)
# in that order, so CXXFLAGS wins a conflict but an -O2 HERE would not conflict with
# anything the coverage configuration passes - it would simply take effect.  An
# optimised coverage build renumbers, merges and elides basic-block arcs, which
# invalidates the 92-record branch-arm coverage manifest keyed to the unoptimised
# arc layout: the gate reads (file, line, block, branch) coordinates, and every one
# of them moves.  The failure would present as a coverage regression with no source
# change behind it.
#
# So on the Autotools path, optimisation and fortification are the packager's
# CXXFLAGS, per the Autotools convention that a package sets AM_CXXFLAGS for flags
# its own source requires and leaves -O and -D_FORTIFY_SOURCE to whoever builds it -
# which is also why configure's own default of "-g -O2" already yields the FORTIFY
# __*_chk references this library's release builds carry.  Link flags are not
# subject to that convention: -z now describes how the ARTEFACT must be loaded, no
# build of this package has a reason to want lazy binding, and there is no
# instrumented-build equivalent of the arc-layout problem for it.
#
# The hand-written ccec/src/Makefile carries -O2 and -U_FORTIFY_SOURCE
# -D_FORTIFY_SOURCE=2 itself, and that is the same rule rather than an exception to
# it: that file has no configure step and no packager CXXFLAGS to defer to, it is
# never instrumented for coverage, and it is the artefact P5-05 found carrying no
# FORTIFY reference at all.
libRCEC_la_LDFLAGS = -lpthread \
                     -Wl,-z,relro,-z,now \
                     -Wl,--no-as-needed,-lhdmicec-v0.1.0.0-cpp,-lcommon-v0.2.0.0-cpp,-lbinder,-lutils,--as-needed
# The four direct AIDL/Binder link edges, in dependency order, follow the OSAL
# edge.  They are required whether or not a given platform has an AIDL HAL: the
# back-end's symbols must resolve at library link time, and the runtime query is
# what decides whether they are ever called.  @AIDL_LDFLAGS@ supplies the -L
# search paths both this list and the forcing group above depend on.
libRCEC_la_LIBADD = -lRCECOSHal -L${top_builddir}/osal/src/.libs @AIDL_LDFLAGS@ @AIDL_LIBS@
