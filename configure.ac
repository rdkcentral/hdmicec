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
dnl                                               -*- Autoconf -*-
dnl Process this file with autoconf to produce a configure script.

dnl AC_PREREQ([2.69])
AC_INIT(hdmicec, 1.0, balaji_punnuru@cable.comcast.com,rmfwebkitifc)
AC_CONFIG_SRCDIR([osal/src/ConditionVariable.cpp])
AM_CONFIG_HEADER(cfg/config.h)
AC_CONFIG_MACRO_DIR([cfg])
AC_CONFIG_AUX_DIR([cfg])
AM_INIT_AUTOMAKE([foreign no-dist-gzip dist-bzip2 1.9])
LT_INIT

dnl subdirectories.

dnl Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_CPP
AC_PROG_CXXCPP

dnl use pretty build output with automake >= 1.11
m4_ifdef([AM_SILENT_RULES],[AM_SILENT_RULES([yes])],
  [AM_DEFAULT_VERBOSITY=1
   AC_SUBST(AM_DEFAULT_VERBOSITY)])




dnl **********************************
dnl checks for dependencies
dnl **********************************
AC_HEADER_STDC

dnl Checks for header files.
AC_CHECK_HEADERS([limits.h memory.h stdlib.h string.h sys/socket.h unistd.h])

dnl Checks for typedefs, structures, and compiler characteristics.
dnl AC_CHECK_HEADER_STDBOOL
AC_TYPE_UINT8_T

dnl Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([memset strdup strerror])

PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28])

dnl **********************************
dnl HDMI CEC AIDL / Binder HAL back-end
dnl **********************************
dnl libRCEC carries BOTH HAL back-ends in every build for every SOC vendor:
dnl the legacy in-process C ABI and the out-of-process com.rdk.hal.hdmicec
dnl AIDL/Binder HAL.  Which one is used is decided at RUN TIME, exactly once,
dnl by Driver::getInstance in ccec/src/Driver.cpp on the basis of a query for
dnl the "HdmiCec" Binder service.  There is therefore deliberately no
dnl --enable/--disable switch and no AM_CONDITIONAL for the AIDL back-end
dnl here: a configure-time toggle would make the linkage conditional on
dnl vendor, variant or configuration, which is precisely what this migration
dnl forbids.  The dependency is as mandatory as GLIB above, so - following the
dnl bare PKG_CHECK_MODULES call on the preceding line, which supplies no
dnl action-if-not-found and therefore aborts configure - every check below is
dnl hard and fails configure outright when the dependency is missing.
dnl
dnl rdk-halif-aidl ships no pkg-config metadata: it contains no .pc file, no
dnl .pc.in template and no pkgconfig directory anywhere.  PKG_CHECK_MODULES
dnl cannot be used for these dependencies, so the headers and the libraries
dnl are probed directly instead.
dnl
dnl Two staging prefixes are the whole of the orchestrator contract.  Every
dnl include root and library directory below is derived from one of them by a
dnl fixed relative path, so the Autotools build and the hand-written RDK build
dnl in ccec/src/Makefile cannot end up configured with different sets.
AC_ARG_VAR([HALIF_PREFIX],
    [Root of the staged rdk-halif-aidl tree supplying the AIDL stub headers and libraries])
AC_ARG_VAR([HALIF_LIB_DIR],
    [AIDL stub library directory; overrides HALIF_PREFIX/lib/halif for split staging layouts])
AC_ARG_VAR([BINDER_SDK_DIR],
    [Root of the staged linux_binder_idl 2.6.0 Binder SDK, holding lib/binder and include/binder_sdk])
AC_ARG_VAR([BINDER_SDK_INCLUDE_DIR],
    [Binder SDK header prefix; overrides BINDER_SDK_DIR for split staging layouts])

AC_ARG_WITH([halif-prefix],
    AS_HELP_STRING([--with-halif-prefix=DIR],
        [root of the staged rdk-halif-aidl tree supplying the AIDL stub headers
         and libraries; defaults to the sibling rdk-halif-aidl checkout. Same as
         the HALIF_PREFIX variable]),
    [if test "x$withval" = xyes || test "x$withval" = xno || test -z "$withval"; then
         AC_MSG_ERROR([--with-halif-prefix requires a directory argument])
     fi
     HALIF_PREFIX=$withval])

AC_ARG_WITH([binder-sdk],
    AS_HELP_STRING([--with-binder-sdk=DIR],
        [root of the staged linux_binder_idl Binder SDK supplying libbinder and
         libutils. Same as the BINDER_SDK_DIR variable]),
    [if test "x$withval" = xyes || test "x$withval" = xno || test -z "$withval"; then
         AC_MSG_ERROR([--with-binder-sdk requires a directory argument])
     fi
     BINDER_SDK_DIR=$withval])

dnl Fall back to the sibling rdk-halif-aidl checkout when no prefix is given.
dnl srcdir may be relative while the substituted flags are consumed from build
dnl subdirectories, so the value is made absolute here.
if test -z "$HALIF_PREFIX"; then
    if test -d "$srcdir/../rdk-halif-aidl"; then
        HALIF_PREFIX=`cd "$srcdir/../rdk-halif-aidl" && pwd`
    fi
fi

dnl Four include roots are required to compile ccec/src/DriverAidlImpl.cpp:
dnl   1. HALIF_PREFIX/hdmicec/0.1.0.0/include  the generated com/rdk/hal/hdmicec
dnl                                            client stubs and server bases
dnl   2. HALIF_PREFIX/common/0.2.0.0/include   com/rdk/hal/PropertyValue.h, the
dnl                                            frozen import the hdmicec 0.1.0.0
dnl                                            snapshot was generated and pinned
dnl                                            against - 0.2.0.0, deliberately
dnl                                            NOT the newer common release
dnl   3. HALIF_PREFIX/common/current           halcompat.h, which is NOT inside
dnl                                            root 2, so the two snapshot roots
dnl                                            alone do not reach it
dnl   4. the Binder SDK header root            binder/, utils/ and android-base/
dnl                                            pulled in by the generated headers
dnl A root present in one consumer and absent from another is the failure this
dnl single derived set exists to prevent, so all four are resolved once here.
dnl
dnl CI hazard: roots 2 and 3 live under common/, outside the hdmicec/-only
dnl sparse checkout that rdk-halif-aidl is documented with.  A fresh sparse
dnl clone fails the halcompat.h probe below until common/ is materialised.  The
dnl fix belongs in the workflow files, not here; this note exists so that the
dnl failure is diagnosable rather than mysterious.
AIDL_CPPFLAGS=""
hdmicec_aidl_missing=""

AC_MSG_CHECKING([for the generated AIDL stub include root])
if test -n "$HALIF_PREFIX" && test -d "$HALIF_PREFIX/hdmicec/0.1.0.0/include"; then
    AIDL_CPPFLAGS="$AIDL_CPPFLAGS -I$HALIF_PREFIX/hdmicec/0.1.0.0/include"
    AC_MSG_RESULT([$HALIF_PREFIX/hdmicec/0.1.0.0/include])
else
    hdmicec_aidl_missing="$hdmicec_aidl_missing $HALIF_PREFIX/hdmicec/0.1.0.0/include"
    AC_MSG_RESULT([not found])
fi

AC_MSG_CHECKING([for the frozen common 0.2.0.0 include root])
if test -n "$HALIF_PREFIX" && test -d "$HALIF_PREFIX/common/0.2.0.0/include"; then
    AIDL_CPPFLAGS="$AIDL_CPPFLAGS -I$HALIF_PREFIX/common/0.2.0.0/include"
    AC_MSG_RESULT([$HALIF_PREFIX/common/0.2.0.0/include])
else
    hdmicec_aidl_missing="$hdmicec_aidl_missing $HALIF_PREFIX/common/0.2.0.0/include"
    AC_MSG_RESULT([not found])
fi

AC_MSG_CHECKING([for the halcompat.h include root])
if test -n "$HALIF_PREFIX" && test -d "$HALIF_PREFIX/common/current"; then
    AIDL_CPPFLAGS="$AIDL_CPPFLAGS -I$HALIF_PREFIX/common/current"
    AC_MSG_RESULT([$HALIF_PREFIX/common/current])
else
    hdmicec_aidl_missing="$hdmicec_aidl_missing $HALIF_PREFIX/common/current"
    AC_MSG_RESULT([not found])
fi

dnl The Binder header root differs between a local development layout and a
dnl Yocto or sysroot staging layout, so the candidates are tried in the same
dnl order the AIDL snapshot's own CMake build resolves them: the development
dnl layout first, then the split-staging override, then a flat include
dnl directory.  A candidate whose base variable is unset is skipped, so that it
dnl cannot expand to an absolute /include/... path that happens to match some
dnl unrelated host directory.
AC_MSG_CHECKING([for the Binder SDK header root])
if test -n "$BINDER_SDK_DIR" && test -d "$BINDER_SDK_DIR/include/binder_sdk"; then
    BINDER_INCLUDE_DIR="$BINDER_SDK_DIR/include/binder_sdk"
elif test -n "$BINDER_SDK_INCLUDE_DIR" && test -d "$BINDER_SDK_INCLUDE_DIR/include/binder_sdk"; then
    BINDER_INCLUDE_DIR="$BINDER_SDK_INCLUDE_DIR/include/binder_sdk"
elif test -n "$BINDER_SDK_INCLUDE_DIR" && test -d "$BINDER_SDK_INCLUDE_DIR/include"; then
    BINDER_INCLUDE_DIR="$BINDER_SDK_INCLUDE_DIR/include"
elif test -n "$BINDER_SDK_DIR" && test -d "$BINDER_SDK_DIR/include"; then
    BINDER_INCLUDE_DIR="$BINDER_SDK_DIR/include"
else
    BINDER_INCLUDE_DIR=""
fi
if test -n "$BINDER_INCLUDE_DIR"; then
    AIDL_CPPFLAGS="$AIDL_CPPFLAGS -I$BINDER_INCLUDE_DIR"
    AC_MSG_RESULT([$BINDER_INCLUDE_DIR])
else
    hdmicec_aidl_missing="$hdmicec_aidl_missing BINDER_SDK_DIR/include/binder_sdk"
    AC_MSG_RESULT([not found; relying on the ambient CPPFLAGS])
fi

dnl Library directories, derived from the same two prefixes.  Each falls back
dnl to the ambient LDFLAGS when it cannot be resolved, so a sysroot that stages
dnl these libraries on the default search path still configures cleanly; the
dnl link probe further down is what decides whether the result actually works.
AC_MSG_CHECKING([for the AIDL stub library directory])
if test -z "$HALIF_LIB_DIR" && test -n "$HALIF_PREFIX" && test -d "$HALIF_PREFIX/lib/halif"; then
    HALIF_LIB_DIR="$HALIF_PREFIX/lib/halif"
fi
if test -n "$HALIF_LIB_DIR"; then
    AC_MSG_RESULT([$HALIF_LIB_DIR])
else
    AC_MSG_RESULT([not found; relying on the ambient LDFLAGS])
fi

AC_MSG_CHECKING([for the Binder SDK library directory])
if test -n "$BINDER_SDK_DIR" && test -d "$BINDER_SDK_DIR/lib/binder"; then
    BINDER_LIB_DIR="$BINDER_SDK_DIR/lib/binder"
elif test -n "$BINDER_SDK_DIR" && test -d "$BINDER_SDK_DIR/lib"; then
    BINDER_LIB_DIR="$BINDER_SDK_DIR/lib"
else
    BINDER_LIB_DIR=""
fi
if test -n "$BINDER_LIB_DIR"; then
    AC_MSG_RESULT([$BINDER_LIB_DIR])
else
    AC_MSG_RESULT([not found; relying on the ambient LDFLAGS])
fi

AIDL_LDFLAGS=""
if test -n "$HALIF_LIB_DIR"; then
    AIDL_LDFLAGS="$AIDL_LDFLAGS -L$HALIF_LIB_DIR"
fi
if test -n "$BINDER_LIB_DIR"; then
    AIDL_LDFLAGS="$AIDL_LDFLAGS -L$BINDER_LIB_DIR"
fi

dnl The direct link edges, in dependency order.  libbinder's own transitive
dnl closure - liblog, libbase, libcutils and libcutils_sockets - is staged and
dnl made loadable rather than named here, because nothing in the middleware
dnl references it; a link failure caused by that closure being absent from the
dnl loader path is a staging problem, not a missing direct edge.
AIDL_LIBS="-lhdmicec-v0.1.0.0-cpp -lcommon-v0.2.0.0-cpp -lbinder -lutils"

dnl Every probe below runs against saved copies of the four flag variables and
dnl restores them afterwards, so that no probe flag leaks into the substituted
dnl values or into any later check.
hdmicec_saved_CPPFLAGS=$CPPFLAGS
hdmicec_saved_CXXFLAGS=$CXXFLAGS
hdmicec_saved_LDFLAGS=$LDFLAGS
hdmicec_saved_LIBS=$LIBS

AC_LANG_PUSH([C++])

dnl The C++17 requirement is pinned explicitly rather than inherited from the
dnl compiler default: the generated AIDL headers include <optional> and declare
dnl std::optional parameters, and halcompat.h opens a nested-namespace
dnl definition.  A host whose default dialect already covers C++17 hides the
dnl requirement, so a SOC toolchain with an older default has to fail here, at
dnl configure time, rather than part-way through compiling DriverAidlImpl.cpp.
dnl AX_CXX_COMPILE_STDCXX_17 is deliberately not used: autoconf-archive is not
dnl vendored and AC_CONFIG_MACRO_DIR points at cfg/, which carries no .m4 file,
dnl so that macro would survive unexpanded into configure.  The probe below is
dnl self-contained and exercises both constructs the real dependencies use.
dnl
dnl TWO PROPERTIES ARE LOAD-BEARING, and both exist because Automake emits
dnl "$(AM_CXXFLAGS) $(CXXFLAGS)" in that order, so whatever the user put in
dnl CXXFLAGS lands LAST on every compile command and wins:
dnl
dnl   1. A dialect flag is ALWAYS settled, never left empty because the ambient
dnl      default happened to pass.  An empty AIDL_CXXFLAGS pins nothing, so the
dnl      dialect would be whatever the compiler defaults to on the day - which
dnl      is how a build can pass configure and then be compiled at C++14 by a
dnl      later "make CXXFLAGS=-std=c++14".
dnl   2. An explicit -std= already present in the user's CPPFLAGS or CXXFLAGS is
dnl      taken as the dialect and PROBED AS GIVEN.  A dialect that is C++17 or
dnl      newer passes the probe and is adopted verbatim rather than fought with;
dnl      one that is older fails the probe and configuration stops here, naming
dnl      it, because Automake cannot be made to override it.
dnl
dnl The probe, not a list of dialect names, decides: it compiles the two
dnl constructs the real dependencies use, so c++17, gnu++17, c++20, c++2a, c++23
dnl and anything newer are accepted on evidence, and c++14 or gnu++14 is
dnl rejected on evidence.  What configure cannot see is an override applied
dnl afterwards at make time; nothing in the tree screens for that, so such an
dnl override surfaces as a compile error inside the generated AIDL headers
dnl rather than as a named diagnostic.
m4_define([HDMICEC_CXX17_TESTPROGRAM],
    [AC_LANG_PROGRAM([[
#include <optional>
namespace hdmicec_cxx17_probe::inner { typedef int probe_type; }
]], [[
std::optional<hdmicec_cxx17_probe::inner::probe_type> probe(17);
return probe.value() == 17 ? 0 : 1;
]])])

dnl WHICH -std= ACTUALLY WINS, and why the two orders below are not the same.
dnl
dnl Automake compiles with
dnl     $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CXXFLAGS) $(CXXFLAGS)
dnl so at BUILD time only a -std= in the user's CXXFLAGS can defeat the dialect
dnl this package puts in AM_CXXFLAGS.  One in CPPFLAGS is itself overridden,
dnl because AM_CXXFLAGS comes after CPPFLAGS.
dnl
dnl Autoconf, by contrast, probes with
dnl     $CXX -c $CXXFLAGS $CPPFLAGS conftest.cpp
dnl - CPPFLAGS LAST, the opposite way round.  Probing with the ambient flags as
dnl they stand therefore does not measure what the build will do: a -std= sitting
dnl in CPPFLAGS would decide the probe and then be overridden at build time.
dnl
dnl So every -std= is stripped from the copies used for probing, and the dialect
dnl under test is appended to the CPPFLAGS copy, which autoconf places last.  The
dnl probe then measures exactly the dialect the build will settle on.  These are
dnl probe-local copies; nothing here is substituted.
hdmicec_probe_CPPFLAGS=""
hdmicec_cppflags_std=""
for hdmicec_std_flag in $hdmicec_saved_CPPFLAGS; do
    case $hdmicec_std_flag in
        -std=*) hdmicec_cppflags_std=$hdmicec_std_flag ;;
        *) hdmicec_probe_CPPFLAGS="$hdmicec_probe_CPPFLAGS $hdmicec_std_flag" ;;
    esac
done
hdmicec_probe_CXXFLAGS=""
hdmicec_user_std=""
for hdmicec_std_flag in $hdmicec_saved_CXXFLAGS; do
    case $hdmicec_std_flag in
        -std=*) hdmicec_user_std=$hdmicec_std_flag ;;
        *) hdmicec_probe_CXXFLAGS="$hdmicec_probe_CXXFLAGS $hdmicec_std_flag" ;;
    esac
done

AC_MSG_CHECKING([whether $CXX supports the C++17 constructs the AIDL headers require])
AIDL_CXXFLAGS=""
CXXFLAGS="$hdmicec_probe_CXXFLAGS"
if test -n "$hdmicec_user_std"; then
    dnl A dialect named in CXXFLAGS is the one that will be in force, so it is
    dnl probed as given and adopted verbatim when it passes.  C++17 and anything
    dnl newer passes; C++14 and older does not, and configuration stops.
    CPPFLAGS="$hdmicec_probe_CPPFLAGS $hdmicec_user_std"
    AC_COMPILE_IFELSE([HDMICEC_CXX17_TESTPROGRAM],
        [AIDL_CXXFLAGS="$hdmicec_user_std"
         AC_MSG_RESULT([yes, with the requested $hdmicec_user_std])],
        [AC_MSG_RESULT([no])
         AC_MSG_ERROR([$hdmicec_user_std was requested through CXXFLAGS, and $CXX
cannot compile a nested-namespace definition and std::optional with it.  The AIDL
HDMI CEC back-end is present in every build and needs C++17 or newer, and a
dialect named in CXXFLAGS cannot be overridden by this package, because Automake
places CXXFLAGS after AM_CXXFLAGS on every compile command.  Remove the flag and
let configure settle the dialect, or name one that is C++17 or newer.])])
else
    dnl No dialect was named, so one is settled here rather than inherited from
    dnl whatever this compiler happens to default to.  The candidates are tried in
    dnl order and the first that compiles the two constructs is adopted; -std=c++1z
    dnl is the spelling older GCC and Clang releases accept.
    for hdmicec_std_candidate in -std=c++17 -std=gnu++17 -std=c++1z; do
        CPPFLAGS="$hdmicec_probe_CPPFLAGS $hdmicec_std_candidate"
        AC_COMPILE_IFELSE([HDMICEC_CXX17_TESTPROGRAM],
            [AIDL_CXXFLAGS="$hdmicec_std_candidate"],
            [])
        if test -n "$AIDL_CXXFLAGS"; then
            break
        fi
    done
    if test -n "$AIDL_CXXFLAGS"; then
        AC_MSG_RESULT([yes, with $AIDL_CXXFLAGS])
    else
        AC_MSG_RESULT([no])
        AC_MSG_ERROR([a C++17 compiler is required to build the AIDL HDMI CEC
back-end, which is present in every build.  $CXX rejected a nested-namespace
definition and std::optional with each of -std=c++17, -std=gnu++17 and
-std=c++1z.  Use a newer C++ compiler or set CXX accordingly.])
    fi
fi

dnl A -std= in CPPFLAGS is reported rather than obeyed or ignored silently: it has
dnl no effect on the targets this package builds, because AM_CXXFLAGS follows
dnl CPPFLAGS, and an integrator who put it there meant it to have one.
if test -n "$hdmicec_cppflags_std" && test "x$hdmicec_cppflags_std" != "x$AIDL_CXXFLAGS"; then
    AC_MSG_WARN([$hdmicec_cppflags_std was given in CPPFLAGS and has no effect on
this package: Automake places AM_CXXFLAGS after CPPFLAGS, so the dialect in force
will be $AIDL_CXXFLAGS.  Put it in CXXFLAGS if it was meant to take effect - but
note that anything older than C++17 is then rejected outright, because the AIDL
HDMI CEC back-end cannot be compiled with it.])
fi

dnl Probes from here on use the dialect flag settled above, appended last for the
dnl same ordering reason.  The flag itself is substituted for the consuming
dnl Makefile.am files rather than injected into CXXFLAGS, so that the library and
dnl the test suites carry it explicitly.
CXXFLAGS="$hdmicec_probe_CXXFLAGS"
CPPFLAGS="$hdmicec_probe_CPPFLAGS $AIDL_CPPFLAGS $AIDL_CXXFLAGS"

dnl Every probe from here on begins by undefining VERSION.  AM_INIT_AUTOMAKE
dnl puts a package-version string into the confdefs prologue that autoconf
dnl prepends to each test program, and both halcompat.h and the generated
dnl headers spell the interface version as the identifier VERSION - as a static
dnl member on IHdmiCec and as I::VERSION at halcompat.h:171 - so the macro
dnl would expand inside them and the probe would fail for a reason that has
dnl nothing to do with the dependency being present.  This is confined to the
dnl probes: no source file under hdmicec includes cfg/config.h, so the shipped
dnl build never sees the macro.  A translation unit that came to include both
dnl cfg/config.h and halcompat.h would need the same undef.
dnl
dnl One include of halcompat.h proves three of the four roots at once: the
dnl header itself lives under common/current, and it pulls in
dnl binder/IServiceManager.h, binder/IInterface.h and utils/String16.h from the
dnl Binder SDK root.
AC_MSG_CHECKING([for halcompat.h and the Binder SDK headers])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#undef VERSION
#include <halcompat.h>
]], [[
return 0;
]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([the HAL interface headers required by the AIDL HDMI CEC
back-end were not found.  halcompat.h and the Binder SDK headers it includes
are mandatory: libRCEC always contains both HAL back-ends and chooses between
them at run time, so this dependency cannot be disabled.
  include roots tried:$AIDL_CPPFLAGS
  roots that do not exist:$hdmicec_aidl_missing
Set the two staging prefixes with --with-halif-prefix=DIR and
--with-binder-sdk=DIR, or through the HALIF_PREFIX and BINDER_SDK_DIR
variables.  When headers and libraries are staged separately, point
BINDER_SDK_INCLUDE_DIR at the Binder header prefix.  Note that halcompat.h
lives at HALIF_PREFIX/common/current, outside the hdmicec/-only sparse checkout
rdk-halif-aidl is documented with, so a sparse clone must materialise common/
as well.])])

dnl The second include proves the remaining root and the frozen common 0.2.0.0
dnl import that the generated header pulls in as com/rdk/hal/PropertyValue.h.
AC_MSG_CHECKING([for the generated com.rdk.hal.hdmicec AIDL client headers])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#undef VERSION
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
]], [[
return ::com::rdk::hal::hdmicec::IHdmiCec::VERSION > 0 ? 0 : 1;
]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([the generated com.rdk.hal.hdmicec AIDL client headers were
not found, or they could not be compiled.  The C++ libbinder backend is
expected, so com/rdk/hal/hdmicec/IHdmiCec.h must declare the IHdmiCec
interface, and com/rdk/hal/PropertyValue.h must come from the frozen common
0.2.0.0 snapshot the hdmicec 0.1.0.0 snapshot was generated against.
  include roots tried:$AIDL_CPPFLAGS
  roots that do not exist:$hdmicec_aidl_missing
Set --with-halif-prefix=DIR or HALIF_PREFIX to the staged rdk-halif-aidl
tree.])])

LDFLAGS="$hdmicec_saved_LDFLAGS $AIDL_LDFLAGS"
LIBS="$AIDL_LIBS $hdmicec_saved_LIBS"

dnl The link probe resolves one real symbol from each layer: serviceName from
dnl the hdmicec stub library, defaultServiceManager from libbinder and the
dnl String16 constructor from libutils.  Naming libcommon-v0.2.0.0-cpp on the
dnl same command line makes the linker locate that file too, which is what
dnl confirms the pinned 0.2.0.0 ABI is the one actually staged.  The probe is
dnl only linked and never run, so it is safe on a host with no Binder driver.
AC_MSG_CHECKING([for the AIDL stub and Binder client libraries])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#undef VERSION
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <binder/IServiceManager.h>
#include <utils/String16.h>
]], [[
::android::sp< ::android::IServiceManager> sm = ::android::defaultServiceManager();
::android::String16 name(::com::rdk::hal::hdmicec::IHdmiCec::serviceName().c_str());
return sm != nullptr && name.size() > 0 ? 0 : 1;
]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([the AIDL stub and Binder client libraries could not be
linked.  All four are unconditional link edges of libRCEC, in this order:
  $AIDL_LIBS
searched in:$AIDL_LDFLAGS
libcommon-v0.2.0.0-cpp is required at exactly 0.2.0.0, the ABI the hdmicec
0.1.0.0 snapshot was generated and pinned against; a newer common release is
not a substitute.  Set --with-halif-prefix=DIR and --with-binder-sdk=DIR, or
the HALIF_PREFIX and BINDER_SDK_DIR variables, and use HALIF_LIB_DIR when the
stub libraries are staged away from the headers.  If the libraries are present
but the link still fails, check that libbinder's own closure - liblog, libbase,
libcutils and libcutils_sockets - is staged and on the loader path; those are
not direct edges and must not be added as such.])])

dnl **********************************
dnl The kernel Binder UAPI: MANDATORY
dnl **********************************
dnl ccec/src/DriverAidlImpl.cpp inspects the Binder driver node directly - opening
dnl it, reading its protocol version and pinging handle 0 - before anything in the
dnl process is allowed to touch libbinder, because on the pinned stack a missing or
dnl protocol-mismatched node is FATAL rather than an error return.  That inspection
dnl needs linux/android/binder.h, so the header is a hard prerequisite of this
dnl library and not an optional extra.
dnl
dnl WHY THIS IS A MANDATORY PROBE AND NOT A CHECK-AND-DEGRADE.  Making the header
dnl optional does not produce a build that lacks a feature; it produces a build
dnl whose AIDL back-end can NEVER be selected on any platform, because the preflight
dnl it compiles instead always declines.  That contradicts "both back-ends are
dnl present in every build for every SOC vendor", it links libbinder and never uses
dnl it, and no test suite reveals it - the legacy path stays green.  A missing UAPI
dnl is therefore a configuration error, and it fails here.
dnl
dnl The probe exercises the two things the source actually uses rather than mere
dnl header presence: BINDER_CURRENT_PROTOCOL_VERSION, which is the constant the
dnl preflight compares the driver against, and struct binder_write_read, which is
dnl the structure it marshals.  A sysroot carrying a stub or truncated header would
dnl pass a presence test and fail this one.
AC_MSG_CHECKING([for the Binder kernel UAPI in linux/android/binder.h])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#undef VERSION
#include <linux/android/binder.h>
]], [[
struct binder_write_read exchange;
struct binder_version reported;

exchange.write_size = 0;
exchange.write_buffer = (binder_uintptr_t)0;
reported.protocol_version = BINDER_CURRENT_PROTOCOL_VERSION;

return (reported.protocol_version > 0 && exchange.write_size == 0) ? 0 : 1;
]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([the Binder kernel UAPI header linux/android/binder.h is
required and was not usable.  It supplies BINDER_CURRENT_PROTOCOL_VERSION and
struct binder_write_read, which ccec/src/DriverAidlImpl.cpp uses to verify the
Binder driver node BEFORE the process touches libbinder - a verification that
cannot be skipped, because on the pinned Binder stack a missing or
protocol-mismatched node aborts the process rather than returning an error.
This dependency is as mandatory as the AIDL stubs themselves: libRCEC always
carries both HAL back-ends, so a build without this header could never select
the AIDL one on any platform and would silently ship a legacy-only library.
Install the kernel UAPI headers for the target - the linux-libc-dev package on
Debian and Ubuntu, linux-headers elsewhere, or the kernel's own
usr/include/linux/android/binder.h when building against a specific kernel
tree - or point CPPFLAGS at the sysroot that carries them.])])

dnl **********************************
dnl The Binder wire ABI: DERIVED, not assumed
dnl **********************************
dnl BINDER_IPC_32BIT is the single switch that decides the Binder WIRE LAYOUT, and
dnl it decides it in two places at once: BINDER_CURRENT_PROTOCOL_VERSION is 7 with
dnl the macro and 8 without it, and binder_size_t / binder_uintptr_t are 32 bit with
dnl it and 64 bit without - in linux/android/binder.h AND, independently, in the
dnl Binder SDK's own binder/Parcel.h.  The middleware and the libbinder it links
dnl must therefore agree on it.  Nothing makes them agree by default: the macro is
dnl absent unless someone defines it, so a middleware compiled against a 32-bit-ABI
dnl SDK would compare protocol 8 against a driver speaking 7 and would marshal UAPI
dnl structures with the wrong field widths.
dnl
dnl So it is DERIVED FROM THE STAGED LIBRARY rather than defaulted or guessed.  The
dnl derivation asks libbinder itself, through the one exported symbol whose C++
dnl mangling contains binder_size_t: android::Parcel::ipcSetDataReference, whose
dnl third parameter is const binder_size_t *.  A 32-bit-ABI build exports it with
dnl const unsigned int * and a 64-bit-ABI build with const unsigned long long *, so
dnl exactly one of the two link probes below resolves against any given SDK.
dnl
dnl The probes declare the symbol THEMSELVES and deliberately do NOT include the
dnl real Binder headers: including them would make the declaration follow whatever
dnl the ambient macro state happens to be, which is the very thing being measured.
dnl No mangled string is hard-coded either - the compiler derives it from the
dnl declaration, so the check survives a toolchain whose mangling differs.
AC_ARG_VAR([BINDER_IPC_32BIT],
    [Binder wire ABI of the staged SDK: yes for the 32-bit ABI (protocol 7), no for the 64-bit ABI (protocol 8).  Leave unset to derive it from the staged libbinder])

hdmicec_abi_saved_LDFLAGS=$LDFLAGS
dnl libbinder's own closure - liblog, libbase, libcutils, libcutils_sockets - is not
dnl named as a link edge anywhere, so the linker is told where to find it for the
dnl purpose of resolving libbinder's DT_NEEDED entries during these probes only.
if test -n "$BINDER_LIB_DIR"; then
    LDFLAGS="$LDFLAGS -Wl,-rpath-link,$BINDER_LIB_DIR"
fi

hdmicec_abi_32bit_links=no
hdmicec_abi_64bit_links=no

AC_MSG_CHECKING([whether the staged libbinder was built for the 32-bit Binder wire ABI])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#undef VERSION
#include <cstddef>
namespace android {
class Parcel {
public:
	void ipcSetDataReference(const unsigned char *, size_t, const unsigned int *, size_t,
	                         void (*)(Parcel *, const unsigned char *, size_t,
	                                  const unsigned int *, size_t));
};
}
]], [[
::android::Parcel *parcel = 0;
parcel->ipcSetDataReference(0, 0, 0, 0, 0);
return 0;
]])],
    [hdmicec_abi_32bit_links=yes
     AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])])

AC_MSG_CHECKING([whether the staged libbinder was built for the 64-bit Binder wire ABI])
AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#undef VERSION
#include <cstddef>
namespace android {
class Parcel {
public:
	void ipcSetDataReference(const unsigned char *, size_t, const unsigned long long *, size_t,
	                         void (*)(Parcel *, const unsigned char *, size_t,
	                                  const unsigned long long *, size_t));
};
}
]], [[
::android::Parcel *parcel = 0;
parcel->ipcSetDataReference(0, 0, 0, 0, 0);
return 0;
]])],
    [hdmicec_abi_64bit_links=yes
     AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])])

LDFLAGS=$hdmicec_abi_saved_LDFLAGS

dnl Exactly one probe must resolve.  Both resolving, or neither, means the question
dnl was not actually answered - a stub library, a mixed staging directory, or a
dnl linker that resolved nothing - and a wrong answer here is worse than no answer,
dnl so the integrator is asked instead of being guessed for.
hdmicec_abi_detected=unknown
if test "x$hdmicec_abi_32bit_links" = xyes && test "x$hdmicec_abi_64bit_links" = xno; then
    hdmicec_abi_detected=yes
elif test "x$hdmicec_abi_64bit_links" = xyes && test "x$hdmicec_abi_32bit_links" = xno; then
    hdmicec_abi_detected=no
fi

hdmicec_abi_source="the staged libbinder"

case "x$BINDER_IPC_32BIT" in
  x)
    if test "x$hdmicec_abi_detected" = xunknown; then
        AC_MSG_ERROR([the Binder wire ABI of the staged libbinder could not be
determined.  Exactly one of the two probes above had to resolve
android::Parcel::ipcSetDataReference against
  $AIDL_LIBS
searched in:$AIDL_LDFLAGS
and the result was 32-bit=$hdmicec_abi_32bit_links, 64-bit=$hdmicec_abi_64bit_links.
Neither resolving usually means libbinder was not reachable at all, or that its
own closure - liblog, libbase, libcutils, libcutils_sockets - is not staged
beside it; both resolving means the search path holds two SDKs with different
ABIs.  This value cannot be defaulted: it selects BINDER_CURRENT_PROTOCOL_VERSION
(7 against 8) and the width of binder_size_t and binder_uintptr_t, so a wrong
setting makes the middleware reject a Binder driver that in fact matches the SDK.
Fix the staging, or state the ABI explicitly with BINDER_IPC_32BIT=yes for a
32-bit wire ABI or BINDER_IPC_32BIT=no for a 64-bit one.])
    fi
    BINDER_IPC_32BIT=$hdmicec_abi_detected
    ;;
  xyes | xno)
    hdmicec_abi_source="the explicit BINDER_IPC_32BIT setting"
    if test "x$hdmicec_abi_detected" != xunknown &&
       test "x$BINDER_IPC_32BIT" != "x$hdmicec_abi_detected"; then
        AC_MSG_ERROR([BINDER_IPC_32BIT=$BINDER_IPC_32BIT was given explicitly, but
the staged libbinder was measured to use the other Binder wire ABI
(BINDER_IPC_32BIT=$hdmicec_abi_detected).  Configuration stops rather than
honouring either value, because both outcomes are broken: building the
middleware against the setting given would disagree with the library it links,
and overriding the setting given would ignore an integrator's deliberate
instruction.  Correct the staged SDK in --with-binder-sdk / BINDER_SDK_DIR, or
correct the setting, so that the two agree.])
    fi
    ;;
  *)
    AC_MSG_ERROR([BINDER_IPC_32BIT=$BINDER_IPC_32BIT is not a recognised value.
Use yes for the 32-bit Binder wire ABI (protocol 7), no for the 64-bit one
(protocol 8), or leave it unset to derive the setting from the staged
libbinder.])
    ;;
esac

dnl The substituted result.  It is a CPPFLAGS-level define rather than a CXXFLAGS
dnl one because both linux/android/binder.h and binder/Parcel.h consume it at
dnl preprocessing time, and it is propagated per target - the library, the L1
dnl runner, the L2 runner and the L2 fake host - so that no translation unit which
dnl sees a Binder header can be compiled without it.
if test "x$BINDER_IPC_32BIT" = xyes; then
    AIDL_ABI_CPPFLAGS="-DBINDER_IPC_32BIT"
    hdmicec_abi_protocol=7
else
    AIDL_ABI_CPPFLAGS=""
    hdmicec_abi_protocol=8
fi

AC_CHECK_SIZEOF([void *])

dnl A WARNING AND NEVER AN ERROR.  A 32-bit wire ABI on a 64-bit target cannot
dnl reach any real Binder driver - a buffer address does not fit in the 32-bit
dnl binder_uintptr_t the UAPI structures then carry - but it is a perfectly
dnl buildable combination and it is what a development host staging a 32-bit-ABI
dnl SDK actually has.  Failing configuration here would stop such a host from
dnl building and testing the legacy path at all, which is the far greater harm.  The
dnl runtime refuses the ping rather than marshalling a truncated pointer, and says
dnl so in the log.
if test "x$BINDER_IPC_32BIT" = xyes && test "x$ac_cv_sizeof_void_p" = x8; then
    AC_MSG_WARN([the derived Binder wire ABI is 32-bit (BINDER_IPC_32BIT, protocol
7) while this target's pointers are 8 bytes wide.  The AIDL back-end will build
and the legacy back-end is unaffected, but this combination cannot reach a real
Binder driver: binder_uintptr_t is then 32 bits and a buffer address cannot be
passed to the kernel without truncation, which the preflight declines to do.  For
a deployable AIDL path, stage a Binder SDK built for this target's pointer width,
or build the middleware for the 32-bit target the SDK was built for.])
fi

AC_LANG_POP([C++])

CPPFLAGS=$hdmicec_saved_CPPFLAGS
CXXFLAGS=$hdmicec_saved_CXXFLAGS
LDFLAGS=$hdmicec_saved_LDFLAGS
LIBS=$hdmicec_saved_LIBS

dnl The substituted contract consumed by ccec/src/Makefile.am,
dnl tests/L1Tests/Makefile.am and tests/L2Tests/Makefile.am:
dnl   AIDL_CPPFLAGS      the four -I include roots
dnl   AIDL_ABI_CPPFLAGS  -DBINDER_IPC_32BIT, or empty, as derived above.  EVERY
dnl                      target that compiles a translation unit which sees a
dnl                      Binder header must carry this, because it changes the
dnl                      wire layout those headers declare
dnl   AIDL_CXXFLAGS      the settled C++17 dialect flag.  Always non-empty: it
dnl                      pins the dialect rather than trusting the compiler
dnl                      default, and it is the user's own -std= when one was
dnl                      given and passed the probe
dnl   AIDL_LDFLAGS       the -L stub and Binder library directories
dnl   AIDL_LIBS          the four direct -l edges, in dependency order
dnl The resolved directories are substituted individually as well, together
dnl with the two prefixes by way of AC_ARG_VAR above, so that a consumer can
dnl derive anything else it needs from the same inputs.  BINDER_IPC_32BIT is
dnl substituted as the decision itself, for a consumer that needs the yes/no
dnl rather than the flag.
AC_SUBST([AIDL_CPPFLAGS])
AC_SUBST([AIDL_ABI_CPPFLAGS])
AC_SUBST([AIDL_CXXFLAGS])
AC_SUBST([AIDL_LDFLAGS])
AC_SUBST([AIDL_LIBS])
AC_SUBST([BINDER_INCLUDE_DIR])
AC_SUBST([BINDER_LIB_DIR])
AC_SUBST([BINDER_IPC_32BIT])

dnl A staging path that resolves to the wrong place must be visible in the
dnl configure output, because the failure it would otherwise produce is a
dnl library that silently carries only one back-end.
AC_MSG_NOTICE([AIDL HDMI CEC back-end: HALIF prefix .......... $HALIF_PREFIX])
AC_MSG_NOTICE([AIDL HDMI CEC back-end: AIDL stub libraries ... $HALIF_LIB_DIR])
AC_MSG_NOTICE([AIDL HDMI CEC back-end: Binder SDK prefix ..... $BINDER_SDK_DIR])
AC_MSG_NOTICE([AIDL HDMI CEC back-end: Binder header root .... $BINDER_INCLUDE_DIR])
AC_MSG_NOTICE([AIDL HDMI CEC back-end: Binder libraries ...... $BINDER_LIB_DIR])
AC_MSG_NOTICE([AIDL HDMI CEC back-end: C++17 dialect ......... $AIDL_CXXFLAGS])
dnl The settled wire ABI, printed with its provenance.  This is the line a reader
dnl compares against the kernel's reported protocol version when the AIDL path is
dnl declined for a protocol mismatch, so it names all three of the decision, the
dnl protocol it implies and where it came from.
AC_MSG_NOTICE([AIDL HDMI CEC back-end: Binder wire ABI ....... BINDER_IPC_32BIT=$BINDER_IPC_32BIT, protocol $hdmicec_abi_protocol, from $hdmicec_abi_source])
AC_MSG_NOTICE([AIDL HDMI CEC back-end: Binder ABI flag ....... ${AIDL_ABI_CPPFLAGS:-none (64-bit wire ABI)}])

dnl Optional L1 unit tests support
AC_ARG_ENABLE([l1tests],
    AS_HELP_STRING([--enable-l1tests], [Enable L1 unit tests (requires gtest)]),
    [enable_l1tests=$enableval],
    [enable_l1tests=no])

AM_CONDITIONAL([ENABLE_L1TESTS], [test "x$enable_l1tests" = "xyes"])

if test "x$enable_l1tests" = "xyes"; then
    PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0], [],
        [AC_MSG_ERROR([Google Test not found. Install libgtest-dev or use --disable-l1tests])])
    AC_SUBST([GTEST_CFLAGS])
    AC_SUBST([GTEST_LIBS])
fi

AC_CONFIG_FILES([Makefile
		 cfg/Makefile
                 osal/Makefile
                 osal/src/Makefile
                 ccec/Makefile
                 ccec/src/Makefile
                 tests/Makefile
                 tests/L1Tests/Makefile
                 tests/L2Tests/Makefile])
AC_OUTPUT
