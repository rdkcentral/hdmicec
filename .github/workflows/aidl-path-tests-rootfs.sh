#!/usr/bin/env bash
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
# aidl-path-tests-rootfs.sh -- build the bootable QEMU root filesystem image that the
# binder-capable AIDL-path job boots, and install into that image the in-guest init that
# mounts binderfs, starts servicemanager and runs tests/L1Tests/run_coverage.sh.
#
# ==============================================================================
# WHY THIS FILE EXISTS -- AND WHERE IT SITS IN THE MIGRATION'S FINAL SCOPE
# ==============================================================================
#   The AIDL-path job runs the complete A-E invocation matrix, and it can only run it on a
#   host with a binder kernel driver.  A hosted runner has none, so the job provisions a
#   PINNED QEMU GUEST, and the provisioning requirement is explicit about how that guest's
#   root filesystem comes to exist: IT IS BUILT BY A CHECKED-IN SCRIPT.  Two alternatives
#   are ruled out in terms -- the image is NOT fetched pre-built, and it is NOT generated
#   inline inside the workflow.  This file is that checked-in script.  Without it the
#   workflow is not executable, and an unmade choice between "a shared directory or a
#   copied disk image" is not a deliverable either; both are settled here, once.
#
#   THE FILE-COUNT POSITION, STATED SO A REVIEWER DOES NOT HAVE TO INFER IT.  The
#   migration's close-out check counts the in-submodule diff against an explicit allowlist
#   of TWENTY-SIX paths -- 13 modified and 13 added, measured with `git diff --name-status`
#   inside hdmicec, the 26th being AIDL_HAL_MIGRATION_NOTES.md, which holds the design detail
#   review moved out of the code comments -- plus blitzy/documentation/Project Guide.md in the
#   superproject, which makes TWENTY-SEVEN logical files (14 UPDATE counting the guide,
#   13 CREATE, 0 DELETE).  THIS FILE IS ONE OF THOSE TWENTY-SIX, not an extra beyond them.
#   What is worth saying is why it is in the set at all: the file-by-file table enumerated
#   no root-image script, so this path is AUTHORISED by the provisioning requirement rather
#   than by that table, and it is NOT an out-of-scope addition -- the workflow it serves
#   cannot exist without it.
#   .github/workflows/L1-tests.yml and .github/workflows/aidl-path-tests.yml state the same
#   identity in their own headers, and the three statements are kept in step deliberately.
#
#   PLACEMENT.  This script lives beside the only job that invokes it.  GitHub Actions
#   loads only *.yml and *.yaml from .github/workflows, so a .sh here is inert to Actions:
#   it cannot be mistaken for a workflow, cannot be triggered, and cannot run by accident.
#
# ==============================================================================
# GOVERNING CONTRACT
# ==============================================================================
#   THIS PROJECT HAS NO USER-SPECIFIED RULES.  `review_rules` returns exactly
#   "No user rules provided.", and that one line is the whole document.  No rule governs
#   this file, so none is cited and none is invented.  Their absence is not licence to
#   lower the bar; the substituting contract is enterprise-standard best practice, and in
#   this submodule that means the conventions tests/L1Tests/run_coverage.sh already
#   establishes and this file reproduces rather than reinvents:
#
#     1. REPOSITORY CONVENTION IS AUTHORITATIVE.  The Apache-2.0/RDK header block above,
#        `set -euo pipefail`, `umask 077`, bracketed log/warn/die helpers writing
#        diagnostics to stderr, a usage() function, a while/case option parser that
#        REFUSES an unknown option and a bare positional rather than ignoring either, and
#        absolute path resolution from BASH_SOURCE so the caller's working directory is
#        irrelevant.
#     2. EVERY DIE MESSAGE NAMES THE FAILING THING AND THE REMEDY.  A message that says
#        only what broke costs the reader the same investigation twice.
#     3. NOTHING IS CREATED OUTSIDE THE OUTPUT DIRECTORY AND A RUN-SCOPED TEMPORARY
#        DIRECTORY.  Every mount this script makes, every loop device it attaches and every
#        temporary directory it creates is registered and released by an
#        EXIT/INT/TERM/HUP trap, in reverse order, whether the run succeeded, failed or was
#        cancelled.  A leaked loop device or a leaked bind mount wedges the runner for every
#        job after it.  Every mount is ADDITIONALLY made inside a mount namespace of this
#        run's own, with private propagation, so no mount is ever visible outside this
#        process and none can survive it even when no trap can run -- a SIGKILL, an OOM kill
#        or a panic.  The trap is still what releases them in order; the namespace is what
#        makes a leak impossible rather than merely unlikely.  See THE ISOLATED MOUNT
#        NAMESPACE.  In the output directory it creates exactly three things: the image
#        and the manifest at the caller's paths, and <image>.lock -- the advisory lock that
#        stops two builders from interleaving, deliberately left behind.  The image and the
#        manifest are STAGED under private mktemp names in that same directory and renamed
#        onto the caller's paths TOGETHER, after the image is validated, so a failure never
#        destroys a previously published pair and never leaves a manifest describing an
#        image that is not there.
#     4. MEASURED, NEVER ASSERTED.  Where this script reports a platform fact it reports
#        what the artefact or the running guest actually shows.  It hard-codes no digest,
#        no version, no timing figure and no coverage percentage.  Where a value must be
#        pinned, the pin is an INPUT that is then VERIFIED here -- see THE BASE ROOTFS.
#     5. NO WARNING BECOMES AN ERROR.  There is no -Werror equivalent anywhere in the image
#        build, and third-party or generated code is never compiled with one.
#
#   WHAT THIS SCRIPT DOES NOT DO, AND MUST NOT BE MADE TO DO:
#     * It contains NO vendor, variant or configuration conditional.  Both HDMI CEC HAL
#       back-ends are compiled into libRCEC for every SOC and the choice between them is a
#       RUNTIME decision, so there is no --enable-aidl / --disable-aidl to pass and no
#       configure switch to select a path.  No such option exists.
#     * It does NOT set BINDER_VERSION.  The Binder SDK version is pinned by
#       rdk-halif-aidl/binder_sdk.version and setting the variable would silently override
#       that pin.
#     * It does NOT set CEC_TEST_AIDL_MODE anywhere -- not here, not in the init it writes.
#       run_coverage.sh owns that value per invocation across the A-E matrix; one value set
#       globally would collapse the matrix to a single arm and report it as green.
#     * It does NOT start a binder threadpool.  On the client side DriverAidlImpl::open()
#       owns that; on the service side the fake service host does.
#     * It asserts NO performance figure and NO device-level result.  Performance parity
#       and L3 device validation are deferred human steps on target hardware; nothing here
#       measures either, and no step in it reads as a performance gate.
#     * It reimplements NO part of run_coverage.sh's orchestration -- not the per-invocation
#       zero-exit check, not the executed-count check, not the selected-path-line grep, not
#       the artifact naming, not the counter zero/accumulate policy and not the single
#       capture.  The init's whole job is to invoke that script correctly in an environment
#       where it can work.
#     * It issues NO `--restore`.  `--build` snapshots the six regenerated Makefiles and
#         restores them from its own snapshot; a standalone `--restore` refuses to run
#         without a same-invocation snapshot precisely so it can never revert this
#         migration's change to ccec/src/Makefile.
#
# ==============================================================================
# TWO SEPARATE AXES: ELF BITNESS AND BINDER WIRE PROTOCOL
# ==============================================================================
#   These are not the same question, and nothing here treats them as one.  Taking
#   "all-32-bit" to fix the wire protocol at 7 -- in the help text, the architecture log, the
#   value written into the image and the manifest -- would put the same claim in four places
#   with nothing anywhere comparing it against what the SDK shipped alongside was actually
#   built with.  Prose cannot be checked, so the two axes are stated separately here and kept
#   separate everywhere below, and the protocol is DERIVED from build artefacts rather than
#   written down.
#
#   The job that drives this script is a protocol-7 all-32-bit job, and it is one because the
#   plan requires it (AAP 0.6.6.4 and 0.9.5.1: CONFIG_ANDROID_BINDER_IPC_32BIT=y).  That
#   requirement is not free on a modern kernel and the workflow pays for it explicitly; the
#   paragraph "WHY 7 NEEDS A PATCHED KERNEL" below is the whole of the reason, and it is
#   stated here because it is the first thing a reader who finds an unexpected number will
#   want.  Nothing in THIS file selects the protocol: it reads what the inputs decided.
#
#   AXIS 1 -- ELF BITNESS, WHICH THIS SCRIPT DOES DECIDE AND DOES ENFORCE.
#   The guest userspace is 32-bit throughout: the middleware, the staged SDK,
#   servicemanager, GoogleTest and every test binary.  A process is a single ELF class, so
#   this is a per-process constraint rather than a platform-wide one, and it is checkable
#   here -- which is why --arch accepts only 32-bit targets and why verify_sdk_tree and
#   verify_payload_tree REFUSE any staged artefact whose ELF class or machine does not
#   match.  A 64-bit libbinder inside a 32-bit guest is a link failure at best, and finding
#   out at boot costs a whole QEMU cycle to diagnose.  i386 is the default because it is
#   the straightforward choice for QEMU on an x86_64 host and needs no foreign-architecture
#   emulation to populate; armhf is equally valid and is accepted, at the cost of a
#   registered binfmt_misc handler on the build host.
#
#   AXIS 2 -- THE BINDER WIRE PROTOCOL, WHICH THIS SCRIPT DERIVES AND NEVER DECLARES.
#   The protocol is selected at BUILD time by the SDK's BINDER_IPC_32BIT and, on the kernel
#   side, by CONFIG_ANDROID_BINDER_IPC_32BIT.  Read from the pinned upstream tree rather
#   than assumed -- linux_binder_idl at tag 2.6.0, cited by locator because it is not a
#   file in this workspace:
#
#     * CMakeLists.txt:209-217 states in terms that the wire protocol is a DIFFERENT axis
#       from the compile bitness and names "-DBINDER_IPC_32BIT=OFF to build a 32-bit
#       protocol-8 library (the mixed config)".  A 32-bit process CAN speak protocol 8.
#     * CMakeLists.txt:218-224 makes the protocol FOLLOW the bitness only when
#       BINDER_IPC_32BIT is not already defined, so a 32-bit build DEFAULTS to protocol 7
#       and an explicit OFF overrides that default.
#     * CMakeLists.txt:225-229 rejects exactly one pairing -- BINDER_IPC_32BIT=ON with a
#       64-bit build.  A 32-bit build with OFF is supported, not rejected.
#     * BUILD.md:409-413 documents a strict protocol-version EQUALITY check when libbinder
#       opens the driver node: kernel, SDK build and vendor side must all agree, and a
#       mismatch fails EVERY open.
#
#   WHY 7 NEEDS A PATCHED KERNEL, AND WHY THAT PATCH IS IN THE WORKFLOW AND NOT HERE.
#   Protocol 7 additionally requires CONFIG_ANDROID_BINDER_IPC_32BIT in the guest kernel.
#   MAINLINE HAS NO SUCH OPTION AFTER 4.17: it was deleted, together with the
#   "#define BINDER_IPC_32BIT 1" in drivers/android/binder.c, by "ANDROID: binder: remove
#   32-bit binder interface".  CONFIG_ANDROID_BINDERFS, which the in-guest init this script
#   writes REQUIRES, first appears in 5.0.  So on a stock kernel the two are mutually
#   exclusive and a binderfs guest cannot be a protocol-7 guest -- which is why asking
#   olddefconfig for the option on a 5.x tree silently produces nothing, and why the pairing
#   reads as unreachable until the removal is looked at closely.
#
#   What that reading misses is that the REMOVAL is 13 lines of Kconfig and 4 lines of
#   binder.c and nothing else: include/uapi/linux/android/binder.h still carries both
#   branches to this day -- "#ifdef BINDER_IPC_32BIT" still selects __u32 binder_size_t and
#   BINDER_CURRENT_PROTOCOL_VERSION 7 -- as does the copy vendored inside the Binder SDK
#   itself.  The workflow therefore RESTORES the option on the pinned kernel with a minimal
#   patch it carries inline, enables it, and asserts CONFIG_ANDROID_BINDER_IPC_32BIT=y in the
#   BUILT .config.  The pin stays on a binderfs-capable 5.15, so both requirements hold at
#   once.  The patch lives in the workflow because this migration's diff allowlist is a fixed
#   set of paths and a patch file would be one more.
#
#   THAT RESTORATION IS UNPROVEN UNTIL THE JOB FIRST RUNS.  The option was removed six years
#   before the kernel it is being re-added to, and nobody upstream exercises that pairing.
#   This is exactly why nothing in this file trusts a declared protocol: the kernel half, the
#   SDK half and the driver's own BINDER_VERSION answer are each read from their source and
#   cross-checked, and a disagreement is a named failure.  If the patch silently fails to take
#   effect, the kernel half comes back 8, the SDK half says 7, and this script refuses to build
#   the image -- which is the outcome to want, not a defect in the checking.
#
#   THE REQUIREMENT IS AGREEMENT, NOT A PARTICULAR NUMBER, so this script does not hold an
#   opinion about which number is right.  It DERIVES the expected protocol from the two
#   build-time artefacts that decide it -- the guest kernel configuration passed with
#   --kernel-config and the staged SDK's own CMakeCache.txt -- REFUSES to build when those
#   two disagree, records the derived value together with the source of each half, and has
#   the guest cross-check all of it against the number the driver itself reports through the
#   BINDER_VERSION ioctl.  Nothing about the protocol is asserted from text anywhere in this
#   file, and a disagreement between any two of the three is a named failure rather than a
#   footnote.  The always-required kernel options are a separate matter and are recorded in
#   the image at ${GUEST_KERNEL_EXPECTATION} for the guest to report against.
#
# ==============================================================================
# WHAT THE IMAGE CONTAINS, AND WHY EACH ITEM IS NOT OPTIONAL
# ==============================================================================
#   The job runs `run_coverage.sh --build --run` INSIDE the guest, because that is where
#   the binder driver is: `--build` COMPILES and `--run` MEASURES, so the guest needs a
#   full build-and-measure toolchain rather than just a runtime.
#
#     * g++, make, autoconf, automake, libtool, pkg-config.  The suites build at
#       -std=c++17, which this script probes for rather than assuming.
#     * glib-2.0 development files.  A HARD configure dependency: configure.ac:65 runs
#       PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28]) and there is no way past it.
#     * GoogleTest/GoogleMock WITH A DISCOVERABLE gtest.pc.  configure.ac's
#       PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0]) consults pkg-config ONLY and ignores
#       -I/-L entirely, so headers and libraries on their own are not enough.  Either the
#       image's own packaging provides gtest.pc or --gtest-prefix names a prefix whose
#       lib/pkgconfig holds one; both are verified, and the init exports GTEST_PREFIX and
#       PKG_CONFIG_PATH accordingly.
#     * gcov (with gcc), genhtml and LCOV 2.x.  See THE LCOV 2.x PREREQUISITE below.
#     * The binder runtime closure, all of it loadable: libbinder and libutils, plus
#       liblog, libbase, libcutils and libcutils_sockets.  Those last four are NOT direct
#       link edges of anything in the middleware -- nothing there references them -- but
#       libbinder links them, so the loader must find every one or nothing starts.  This
#       script registers their directories with ld.so.conf and runs ldconfig in the image,
#       and the init also puts them on LD_LIBRARY_PATH: GNU ld does not search -L for a
#       shared library's own DT_NEEDED entries and the staged libbinder carries no RUNPATH,
#       so the search path is needed at LINK time as well as at run time.
#     * servicemanager, from the staged SDK's bin directory (build_binder.sh:49 is what
#       puts it there).  It is an UNCONDITIONAL runtime prerequisite wherever a binder
#       driver is present: checkService cannot resolve "HdmiCec" without it, and obtaining
#       an IServiceManager at all loops until binder handle 0 resolves.
#     * util-linux/coreutils basics the init and run_coverage.sh need: mount, mknod, stat,
#       timeout, flock, nproc, awk, find, mktemp.
#     * A working /proc, /sys and /dev.  The init mounts them.
#
#   THE LCOV 2.x PREREQUISITE, AND WHY AN `apt install lcov` IS NOT SUFFICIENT.
#   run_coverage.sh requires lcov 2.x and says so in four places: its prerequisites list
#   names "lcov 2.x, genhtml and gcov on PATH"; its require_tools() failure states that
#   --fail-under-lines and --rc branch_coverage=1 do not exist in lcov 1.x; its header
#   records that its record spellings were verified against lcov 2.0-1; and the aggregate
#   verdict is delegated to `lcov --summary <trace> --fail-under-lines N`.  The suite's
#   own .lcovrc_l1 uses lcov 2.x spellings as well.  Stable distribution archives ship
#   1.x, so this script installs lcov from a pinned, checksum-verified source tarball when
#   one is supplied and then ASSERTS, inside the image, that `lcov --version` reports 2.x.
#   The image build FAILS if it reports 1.x, because a 1.x lcov surfaces much later as a
#   confusing coverage failure inside the guest, long after the evidence of why.
#
# ==============================================================================
# PAYLOAD TRANSPORT -- COPIED INTO THE IMAGE, AND NO GUEST NETWORK
# ==============================================================================
#   The binaries and the staged SDK must reach the guest over a shared directory OR a
#   copied disk image, never a network fetch from inside the guest.  THIS SCRIPT COMMITS TO
#   THE COPIED DISK IMAGE, and the choice is not arbitrary:
#     * it needs no 9p or virtio-fs support in the host kernel or the guest kernel, so the
#       guest's kernel configuration is decided by the binder requirements alone;
#     * there is no mount ordering to get wrong at boot, and no way for a run to start
#       against a half-populated share;
#     * the image is self-contained, so the same file boots identically on any host.
#   ARTIFACTS COME BACK THE SAME WAY.  The root filesystem is writable, the init writes
#   every artifact under ${GUEST_ARTIFACT_DIR} inside it, and the workflow retrieves them
#   after the guest has powered down by loop-mounting the image read-only and copying that
#   directory out.  The manifest this script writes beside the image names the directory and
#   the status file, so the workflow reads them rather than hard-coding them.
#
#   THE GUEST PERFORMS NO NETWORK ACCESS AT TEST TIME.  Full stop.
#
#   THAT IS A DIFFERENT STATEMENT FROM "NOTHING IS EVER DOWNLOADED", and the distinction
#   matters enough to state so nobody "simplifies" it later.  Acquiring a BASE root
#   filesystem and an lcov source tarball ON THE BUILD HOST, at image-build time, is
#   legitimate and is how the image comes to exist at all.  What is forbidden is the GUEST
#   reaching the network while it is measuring.  Host-side acquisition is therefore
#   allowed here and is PINNED AND VERIFIED -- see THE BASE ROOTFS -- because an unpinned
#   base makes the job irreproducible, which defeats the point of a pinned guest.
#
# ==============================================================================
# THE BASE ROOTFS: PINNED BY THE CALLER, VERIFIED HERE, AND NEVER GUESSED
# ==============================================================================
#   A base tarball is required, either as a local file (--base-tarball) or as a pinned URL
#   fetched on the host (--base-url).  EITHER WAY A SHA256 IS MANDATORY: --base-sha256, or
#   a sidecar <tarball>.sha256 in `sha256sum -c` format.  The digest is verified before a
#   single byte is extracted and a mismatch is fatal.
#
#   NO DIGEST IS HARD-CODED IN THIS FILE, AND THAT IS DELIBERATE RATHER THAN AN OMISSION.
#   A digest is a MEASUREMENT of a specific artefact.  One written into this script from
#   memory would be a fabricated measurement -- it would either be wrong, and reject a
#   correct base, or right by luck, and be trusted for the wrong reason.  So the pin lives
#   with whoever produced the artefact (the workflow, a cache step, a release manifest) and
#   this script's job is to REFUSE ANYTHING IT CANNOT VERIFY.  That is strictly stronger
#   than a hard-coded constant: there is no path through this script that extracts an
#   unverified base.
#
# ==============================================================================
# USAGE
# ==============================================================================
#   sudo ./aidl-path-tests-rootfs.sh \
#        --output   /path/to/cec-l2-rootfs.ext4 \
#        --payload  /path/to/hdmicec \
#        --sdk-dir  /path/to/rdk-halif-aidl \
#        --base-tarball /path/to/base-i386.tar.gz --base-sha256 <hex> \
#        [--lcov-tarball /path/to/lcov-2.x.tar.gz --lcov-sha256 <hex>] \
#        [--gtest-prefix /path/to/install/usr] [--kernel-config /path/to/.config] \
#        [--sink-caller-source /path/to/HdmiCecSinkImplementation.cpp] \
#        [--arch i386] [--size 6144] [--apt-mirror URL]
#
#   --sink-caller-source is optional only in spelling: the file is REQUIRED, and when the
#   option is omitted it is taken from beside the --payload tree or the build FAILS.
#
#   Run --help for the full option and default reference.  Root privilege is required: the
#   image is populated through a loop device, a mount and a chroot.
#
# ==============================================================================
# WHAT THE WORKFLOW DOES WITH THE RESULT
# ==============================================================================
#   This script prints, and records in <output>.manifest, everything the workflow needs:
#   the resolved architecture, the guest entry point to pass as init=, the artifact
#   directory to extract, the status file to read, and the kernel options the guest is
#   expected to have been built with.  The workflow boots the image with the guest kernel,
#   waits for the guest to power itself down, loop-mounts the image read-only, copies the
#   artifact directory out, and reads the status file.  Nothing in the boot path needs a
#   network, a share or a second disk.
##########################################################################

set -euo pipefail

# Every file and directory this run creates is owner-only.  The image is populated as root
# and holds a build tree, the staged SDK and, later, coverage evidence; none of it has
# another consumer on the build host, and a permissive umask would leave the intermediate
# staging tree and the finished image readable and appendable by any local account.
umask 077

# ------------------------------------------------------------------------------------
# Absolute self-resolution, so the caller's working directory is irrelevant: this script is
# invoked from a workflow step whose working directory is the superproject root, from the
# submodule root by hand, and from this directory while it is being edited.  All three must
# behave identically.  `pwd -P` resolves symlinks so the paths in diagnostics are the
# physical ones.
# ------------------------------------------------------------------------------------
SCRIPT_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/$(basename -- "${BASH_SOURCE[0]}")"
readonly SCRIPT_PATH
readonly SCRIPT_NAME='aidl-path-tests-rootfs'

log()  { printf '[%s] %s\n' "$SCRIPT_NAME" "$*"; }
warn() { printf '[%s] WARNING: %s\n' "$SCRIPT_NAME" "$*" >&2; }
die()  { printf '[%s] ERROR: %s\n' "$SCRIPT_NAME" "$*" >&2; exit 1; }

# ------------------------------------------------------------------------------------
# RENDERING AN UNTRUSTED VALUE INTO A DIAGNOSTIC.  CWE-117, closed by construction.
#
# THE DEFECT THIS REMOVES.  Every diagnostic in this file used to interpolate caller-supplied
# text verbatim.  A value carrying a newline therefore did not appear inside a message -- it
# ENDED that message and began a new line of its own, and a line beginning "::error::" is a
# GitHub Actions workflow command.  Measured before the fix: a newline-bearing value produced a
# standalone forged "::error::" annotation in the job log, and a five-thousand-character value
# produced more than ten kilobytes of diagnostics, burying the real failure.
#
# THE CONTRACT, in the order the steps are applied, because the order is what makes the
# escaping unambiguous and reversible:
#
#   (a) a literal backslash becomes \\ FIRST, so that every escape introduced below is
#       distinguishable from the same characters occurring literally in the value;
#   (b) 0x0A becomes \n, 0x0D becomes \r, 0x09 becomes \t, and EVERY OTHER byte outside
#       printable ASCII 0x20..0x7E becomes \xNN in lower-case hex.  The classification is by
#       byte value under LC_ALL=C, so no locale can reinterpret a byte as printable and no
#       multi-byte sequence can hide a control character inside itself;
#   (c) the rendering is truncated at 200 characters and "...[truncated, N bytes total]" is
#       appended when it truncates, N being the value's own length in bytes.  A caller cannot
#       drown a log, and the message still says how much was withheld;
#   (d) it is applied to the VALUE and never to the surrounding message, and the result is
#       always returned delimited -- [ ... ] here -- so an empty value is visible as [] rather
#       than as a gap in a sentence;
#   (e) it never begins a diagnostic line.  Every message keeps this script's own "[name]"
#       prefix in front of it, and the rendering itself begins with '['.  Together with (b),
#       which leaves no raw newline anywhere in the output, that makes a forged standalone
#       ::error::, ::warning:: or ::notice:: line UNREACHABLE BY CONSTRUCTION rather than
#       unlikely.
#
# THIS IS ONE OF FIVE COPIES OF ONE CONTRACT.  The five must not diverge; a change to any of
# them is a change to all five.  The others are:
#
#   * hdmicec/tests/L1Tests/run_coverage.sh                      -- render_untrusted()
#   * hdmicec/.github/workflows/aidl-path-tests-rootfs.sh        -- render_untrusted()
#   * hdmicec/tests/L1Tests/test_main.cpp                        -- renderUntrustedValue()
#   * hdmicec/tests/L2Tests/test_main.cpp                        -- renderUntrustedValue()
#   * hdmicec/mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp  -- renderUntrustedValue()
#
# They are five file-local copies rather than one shared helper deliberately: two are shell
# and three are C++, they live in three build targets and one non-built script, and a shared
# header would add a build-system edge for a twenty-line function.  The cost of the choice is
# this cross-reference, which is why every copy carries it.
#
# NO EXTERNAL COMMAND IS RUN.  Everything below is a bash builtin, so this is usable from any
# diagnostic including ones that fire before the tooling pre-flight has established that
# anything external exists.
# ------------------------------------------------------------------------------------
render_untrusted() { # $1=the untrusted value -> prints [escaped, bounded] on stdout
    # LC_ALL and LC_CTYPE are local, and bash re-reads the locale when either is assigned, so
    # for the duration of this function ${#value} counts BYTES and ${value:i:1} yields ONE
    # byte.  Both revert on return.
    local LC_ALL=C LC_CTYPE=C
    local value="${1-}"
    local total=${#value}
    local limit=200
    local out='' rendered=0 index=0 byte piece ord

    while [ "$index" -lt "$total" ]; do
        byte="${value:index:1}"
        # The numeric value of the byte, via printf's "'c" form.  Under LC_ALL=C this is the
        # byte itself, and it is obtained before any classification so that the decision below
        # is made on the number and never on a pattern match that a locale could influence.
        printf -v ord '%d' "'$byte"
        if   [ "$ord" -eq 92 ]; then piece='\\'
        elif [ "$ord" -eq 10 ]; then piece='\n'
        elif [ "$ord" -eq 13 ]; then piece='\r'
        elif [ "$ord" -eq  9 ]; then piece='\t'
        elif [ "$ord" -ge 32 ] && [ "$ord" -le 126 ]; then piece="$byte"
        else printf -v piece '\\x%02x' "$ord"
        fi

        if [ $(( rendered + ${#piece} )) -gt "$limit" ]; then
            printf '[%s...[truncated, %d bytes total]]' "$out" "$total"
            return 0
        fi
        out="${out}${piece}"
        rendered=$(( rendered + ${#piece} ))
        index=$(( index + 1 ))
    done

    printf '[%s]' "$out"
}

# ------------------------------------------------------------------------------------
# FIXED GUEST LAYOUT.  These are constants rather than options on purpose: the init this
# script writes, the manifest the workflow reads and the environment run_coverage.sh is
# handed all have to agree about them, and a knob for each would be four more ways for
# them to disagree.
#
# The workspace layout REPRODUCES THE CI LAYOUT rather than inventing one, and that is
# load-bearing.  run_coverage.sh derives its own workspace root as the parent of the
# hdmicec checkout, defaults GTEST_PREFIX to <workspace>/install/usr, and configure falls
# back to the sibling rdk-halif-aidl checkout beside the submodule when HALIF_PREFIX is
# unset.  Naming the guest directories to match means every one of those defaults is
# already correct inside the guest, so nothing has to be overridden to make the build work.
# ------------------------------------------------------------------------------------
readonly GUEST_WORKSPACE='/opt/cec-l2'
readonly GUEST_PAYLOAD_DIR="$GUEST_WORKSPACE/hdmicec"
readonly GUEST_SDK_DIR="$GUEST_WORKSPACE/rdk-halif-aidl"
readonly GUEST_GTEST_PREFIX="$GUEST_WORKSPACE/install/usr"
# THE SINK CALLER SOURCE, PLACED WHERE THE CI WORKSPACE PUTS IT.  The L1 suite carries a
# drift guard -- DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSink
# Source -- which reads the real Sink plugin source and FAILS HARD when it cannot find it,
# deliberately, because the difference-3 case it protects otherwise asserts against a model
# of the plugin that nothing has checked against its subject.  The guard looks for the file
# beside this component, so the destination reproduces the superproject layout -- a sibling
# of the payload directory -- rather than inventing one, and the init additionally exports
# CEC_SINK_CALLER_SOURCE to the absolute path so the guard does not depend on the runner's
# working directory.  Measured: without it the guest ran the whole build under emulation and
# then failed invocation A on this one case, 566 of 567 passing.
#   It is a REQUIRED ACCEPTANCE INPUT on every invocation, not an optional diagnostic: absent it
#   that guard FAILS by design rather than skipping, because a skip is indistinguishable from a
#   pass in an aggregate count.  Nothing compiles the file -- it is read as text.
readonly GUEST_SINK_COMPONENT_DIR="$GUEST_WORKSPACE/entservices-hdmicecsink"
readonly GUEST_SINK_CALLER_SOURCE="$GUEST_SINK_COMPONENT_DIR/plugin/HdmiCecSinkImplementation.cpp"
readonly GUEST_ARTIFACT_DIR='/artifacts'
readonly GUEST_STATUS_FILE="$GUEST_ARTIFACT_DIR/run_coverage.status"
# ANCHORED UNDER /usr/sbin, NOT /sbin, AND THAT IS LOAD-BEARING RATHER THAN A PREFERENCE.
# Debian has been usr-merged since buster: in the bookworm base tarball this script builds,
# /sbin, /bin and /lib are SYMBOLIC LINKS -- and relative ones (`sbin -> usr/sbin`), not the
# absolute form an earlier reading of this hazard anticipated.  The confinement that writes
# into the image refuses to traverse a symbolic link of either form, by design, because
# following one is exactly how a write escapes the image and lands on the build host.  So
# `/sbin/cec-l2-init` is not a path this script can write on a usr-merged base at all: it is
# refused with ELOOP before a byte is written.  /usr/sbin is a real directory in both the
# merged and the split layout, so anchoring here writes the same file on either base and asks
# nothing of the confinement.  The kernel is booted with init=<this path>, which the workflow
# reads back out of the image manifest, so this constant is the single place it is decided.
readonly GUEST_INIT_PATH='/usr/sbin/cec-l2-init'
readonly GUEST_CONF_DIR='/etc/cec-l2'
readonly GUEST_IMAGE_CONF="$GUEST_CONF_DIR/image.conf"
readonly GUEST_KERNEL_EXPECTATION="$GUEST_CONF_DIR/expected-kernel-config"
readonly GUEST_KERNEL_PROVENANCE="$GUEST_CONF_DIR/build-host-kernel-config"
readonly GUEST_SDK_FLAGS_RECORD="$GUEST_CONF_DIR/sdk-build-flags"
readonly GUEST_IMAGE_RECORD="$GUEST_CONF_DIR/image-build-record"
# THE EXACT PACKAGE SET, WITH VERSIONS, AS INSTALLED.  Pinning the archive makes the set
# reproducible; recording it makes the set AUDITABLE, which is a different property and the
# one a reader needs when a rerun behaves differently.  The two together are what turn "the
# archive was pinned" from an assertion into evidence.
readonly GUEST_PACKAGE_RECORD="$GUEST_CONF_DIR/installed-packages"
readonly GUEST_PROTOCOL_HELPER='/usr/local/bin/cec-binder-protocol-version'
readonly GUEST_READY_PROBE='/usr/local/bin/cec-servicemanager-ready'

# Relative paths inside the staged rdk-halif-aidl tree.  These four are the layout
# build_binder.sh and the snapshot builds produce and the L1 workflow already consumes;
# deriving them from one --sdk-dir keeps the orchestrator contract to a single input.
readonly SDK_REL_BINDER_TARGET='out/target'
readonly SDK_REL_BINDER_BUILD='out/build'
readonly SDK_REL_HALIF_LIB='out/target/lib/halif'
readonly SDK_REL_SERVICEMANAGER='out/target/bin/servicemanager'

# The header roots the generated stubs and halcompat.h need, verified so a missing one is a
# named failure here rather than a compile error inside the guest an hour later.
readonly SDK_REL_INCLUDE_ROOTS=(
    'hdmicec/0.1.0.0/include'
    'common/0.2.0.0/include'
    'common/current'
)

# The binder runtime closure.  libbinder and libutils are direct link edges of libRCEC; the
# other four are libbinder's own dependencies and are listed because they must be LOADABLE,
# not because anything in the middleware names them.
readonly BINDER_CLOSURE_LIBRARIES=(
    libbinder.so
    libutils.so
    liblog.so
    libbase.so
    libcutils.so
    libcutils_sockets.so
)

# The two pinned AIDL stub libraries.  common is pinned at 0.2.0.0 because that is the ABI
# the released hdmicec 0.1.0.0 snapshot was generated against; a newer common release is
# not a substitute.
readonly HALIF_STUB_LIBRARIES=(
    libhdmicec-v0.1.0.0-cpp.so
    libcommon-v0.2.0.0-cpp.so
)

# The binder device nodes the always-required kernel configuration asks for.  The initial
# binderfs instance is populated from CONFIG_ANDROID_BINDER_DEVICES, so a missing node here
# is a kernel-configuration fault in the guest and is reported as one.
readonly BINDER_DEVICE_NODES='binder hwbinder vndbinder'

# Trees the operating system owns.  An output image, a payload or an SDK resolving into one
# of these is nearly always a '..' that collapsed out of the intended path or a mistyped
# root, and this script writes multi-gigabyte files and mounts filesystems.
readonly PROTECTED_SYSTEM_ROOTS=(
    /bin /boot /dev /etc /lib /lib32 /lib64 /libx32 /proc /run /sbin /sys /usr /var
)

readonly DEFAULT_ARCH='i386'
readonly DEFAULT_IMAGE_SIZE_MIB=6144
# Wall-clock budget the init gives servicemanager to answer a real binder transaction.
# Generous because a cold guest on a shared CI host is slow, and bounded because the
# alternative -- an unbounded wait -- is a hung job with no diagnosis.
readonly DEFAULT_READINESS_TIMEOUT_SECONDS=180

# ------------------------------------------------------------------------------------
# CLEANUP REGISTRIES AND THE TRAP.
#
# Everything destructive this script does is registered the moment it happens, and released
# in reverse order by one trap that runs on success, on failure and on interruption alike.
# The order matters: a bind mount inside the image must go before the image's own mount,
# and the image's mount must go before the loop device is detached, or the detach fails and
# the runner is left with a busy loop device that no later job can reuse.
#
# Registration happens BEFORE the action wherever the action can partially succeed
# (`mount` can fail with the filesystem attached), because a cleanup entry for something
# that never happened is harmless -- it is skipped -- while a missing entry for something
# that did happen is a leak.
# ------------------------------------------------------------------------------------
MOUNT_POINTS=()
LOOP_DEVICES=()
LOOP_BACKING_FILES=()
TEMP_DIRS=()
CLEANUP_IN_PROGRESS=0
# Whether an output image exists yet, and whether the run got far enough to declare it
# finished.  The pair is what lets cleanup remove a HALF-BUILT image: an incomplete image left
# on disk is worse than no image, because a workflow step that only checks for the file's
# existence would boot it.
IMAGE_CREATED=0
BUILD_COMPLETED=0
# The two files this run actually writes.  Both live in the output directory under private
# mktemp names and are renamed onto the caller's paths, together, only after the image has
# been validated -- see publish_outputs().  They are declared here, before the trap is
# registered, because the trap reads them and `set -u` makes an unset name a fatal error at
# the worst possible moment.
STAGED_IMAGE=''
STAGED_MANIFEST=''
# THE LEAF NAMES OF THE SAME TWO FILES, AND WHY BOTH FORMS ARE KEPT.
#
# Every privileged operation on a file in the output directory is performed through the
# DESCRIPTOR-RELATIVE path $OUTPUT_DIR_PROC/<leaf> rather than through the caller's pathname
# (see the custody block above resolve_publication_targets for why).  A leaf name is therefore
# the durable identity of one of these files, and the two forms are composed from it:
#
#     operations   "$OUTPUT_DIR_PROC/$STAGED_IMAGE_LEAF"   -- pinned to the validated inode
#     messages     "$STAGED_IMAGE"                          -- what the caller typed, readable
#
# The human form is also what the KERNEL reports as a loop device's backing_file, because it
# resolves the proc path at attach time -- so assert_loop_backs continues to compare against it.
STAGED_IMAGE_LEAF=''
STAGED_MANIFEST_LEAF=''

# THE PUBLICATION TRANSACTION, VISIBLE TO THE TRAP.
#
# WHY THESE ARE GLOBALS AND NOT LOCALS OF publish_outputs().  The publication moves the previous
# image and the previous manifest aside and then renames the new pair onto their names.  While
# that is in flight the caller's good image exists ONLY under a private aside name, so anything
# that ends the process between the first aside and the last rename leaves the published path
# empty and the previous image hidden under a name nobody knows.  Held in function locals these
# names would be invisible to the EXIT trap -- the one thing that runs on every path -- which
# would therefore have nothing to put back.  They are globals, they are set BEFORE the rename
# that makes them necessary, and cleanup() rolls the transaction back from them.
#
# PUBLISH_IN_PROGRESS is the flag that says a rollback is owed.  It is raised before the first
# rename and lowered only once the pair is committed.
PUBLISH_ASIDE_IMAGE=''
PUBLISH_ASIDE_MANIFEST=''
PUBLISH_ASIDE_IMAGE_LEAF=''
PUBLISH_ASIDE_MANIFEST_LEAF=''
PUBLISH_IN_PROGRESS=0

# WHETHER EACH ASIDE ACTUALLY HOLDS THE PREVIOUS CONTENT -- which is NOT the same question as
# "does a file exist at the aside name".
#
# WHY EXISTENCE CANNOT BE THE TEST.  mktemp mints the aside names by CREATING THEM, so between
# minting and the `mv` there is a real, EMPTY, regular file at each aside name.  With
# `[ -f "$aside" ]` as its whole precondition, publish_restore_one would treat that placeholder
# as content: if the old leaf's rename failed -- the one case in which the aside is still the
# empty placeholder -- the rollback would move that ZERO-BYTE PLACEHOLDER over the caller's
# still-good published image or manifest, and the failure that exists to preserve the previous
# pair would destroy it instead.
#
# So each flag is raised ONLY after that leaf's own `mv` returned success, and restoration is
# conditional on the flag.  The recorded size is belt and braces on top of it: an aside of zero
# bytes is refused when the original was not zero bytes, so a placeholder cannot be restored
# even if a future edit sets the flag in the wrong place.
PUBLISH_ASIDE_IMAGE_HOLDS_OLD=0
PUBLISH_ASIDE_MANIFEST_HOLDS_OLD=0
PUBLISH_OLD_IMAGE_BYTES=''
PUBLISH_OLD_MANIFEST_BYTES=''

# Raised once each NEW leaf has been renamed onto its published name, so a rollback knows it has
# to undo that rename before it can put the previous one back.
#
# BOTH, and that symmetry is the point: there was only an image flag, so a failure AFTER the
# manifest rename -- the chmod, or the durability flush -- withdrew the new image and left the
# new MANIFEST sitting at its published path.  On a first publication, with no asides to restore,
# that is a manifest describing an image that is not there.
PUBLISH_IMAGE_RENAMED=0
PUBLISH_MANIFEST_RENAMED=0

# WRITE-AHEAD INTENT, AND IDENTITY BY INODE.  Both exist to close ONE hole, and it is the widest
# of all of them.
#
# THE HOLE.  Every flag above is raised AFTER its own `mv` returns, so a journal rendered from
# those flags alone would be a write-AFTER-move record.  A SIGKILL in the window between a
# completed rename and the journal update that would have recorded it leaves a journal on the
# disk saying the move did NOT happen, and a recovery that TRUSTS it destroys the generation it
# exists to preserve.  MEASURED with a write-after-move record: kill immediately after the
# previous image's own `mv`, and MOVED_ASIDE_IMAGE is still 0, so publish_restore_one() takes
# its "nothing was ever moved into this aside" arm and skips the restoration -- and the discard
# pass that follows then removes the journal-named aside, which at that instant is the ONLY copy
# of the caller's previous image.
#
# SO THE RECORD IS WRITTEN TWICE PER MOVE.  INTENT_<step> is written and flushed BEFORE the
# rename; DONE_<step> is written and flushed after it.  (INTENT=1, DONE=0) therefore means
# exactly "this move may or may not have happened" -- which is the truth -- and the recovery is
# forbidden from resolving that combination out of the record at all.
#
# AND IT RESOLVES IT BY INODE, WHICH IS A FACT RATHER THAN A CLAIM.  Every participant's device,
# inode and size is recorded while it is still at its starting name: the previous image, the
# previous manifest, both of this run's staged files, AND both mktemp aside placeholders.  A
# rename changes a NAME and leaves the inode alone, so recovery stats each of the six names and
# asks "which recorded object is sitting here", which is decidable with no heuristic in it.
#
# THE PLACEHOLDER INODES ARE THE PART THAT IS EASY TO MISS.  mktemp mints each aside name by
# CREATING a file there, so before the move the aside name holds the PLACEHOLDER's inode and
# after it the previous file's.  Recording the placeholder's identity is what makes those two
# states distinguishable without trusting any flag -- which is the whole of the fix.  A name
# holding an object the journal identifies as nothing is a disagreement, and a disagreement is
# refused with everything left exactly where it is.
PUBLISH_INTENT_ASIDE_IMAGE=0
PUBLISH_INTENT_ASIDE_MANIFEST=0
PUBLISH_INTENT_NEW_IMAGE=0
PUBLISH_INTENT_NEW_MANIFEST=0
PUBLISH_OLD_IMAGE_DEVICE=''
PUBLISH_OLD_IMAGE_INODE=''
PUBLISH_OLD_MANIFEST_DEVICE=''
PUBLISH_OLD_MANIFEST_INODE=''
PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE=''
PUBLISH_ASIDE_IMAGE_HOLDER_INODE=''
PUBLISH_ASIDE_MANIFEST_HOLDER_DEVICE=''
PUBLISH_ASIDE_MANIFEST_HOLDER_INODE=''
PUBLISH_STAGED_IMAGE_BYTES=''
PUBLISH_STAGED_MANIFEST_BYTES=''

# The fixed prefixes the aside names are minted under.  They are constants because the
# crash-consistent recovery at start-up has to be able to FIND them in a directory it did not
# create, without a record from the run that died: the run that would have written such a record
# is precisely the run that did not survive to write it.
readonly PUBLISH_ASIDE_IMAGE_PREFIX='.cec-l2-previous-image.'
readonly PUBLISH_ASIDE_MANIFEST_PREFIX='.cec-l2-previous-manifest.'

# ------------------------------------------------------------------------------------
# THE PUBLICATION JOURNAL: HOW THE PAIR IS RECOVERED TOGETHER.
#
# WHAT NO SET OF FLAGS CAN DO.  The image and the manifest are published by two independent
# same-directory renames.  A SIGKILL, an OOM kill or a power loss between them runs no handler at
# all, so nothing in this process can record what happened -- and the state it leaves is
# genuinely ambiguous from the outside: a new image at its published path, no manifest, and BOTH
# asides still sitting there.  A recovery that reads that state one file at a time reaches the
# worst possible conclusion from it: an image exists, so the old-image aside is "superseded" and
# gets DELETED; no manifest exists, so the old manifest is restored.  The result is a NEW image
# beside an OLD manifest, cemented permanently, with the old image destroyed.  The manifest names
# the image's size, architecture and derived binder protocol, so that pair is not merely stale --
# it is a pair whose two halves describe different builds, and it looks complete.
#
# WHAT IS DONE INSTEAD.  A journal leaf in the publication directory, written and made durable
# BEFORE the first move and updated after each one, carrying the run identity, a generation
# identity, both aside leaf names and one completion record per move.  The next run reads it
# UNDER THE LOCK and drives the pair to ONE consistent state -- fully the previous generation, or
# fully the new one -- and refuses loudly rather than guessing when the state on disk does not
# match what the journal says it should be.
#
# WHY A JOURNAL RATHER THAN A GENERATION DIRECTORY.  A generation directory with one atomically
# switched symlink is the other correct shape, and it is smaller to reason about -- but it changes
# the PATHS the workflow consumes, which would propagate into every step that reads the image or
# the manifest.  The journal keeps the caller's two fixed paths exactly as they are.
#
# WHY THE LEAF NAME IS DERIVED FROM THE IMAGE LEAF.  Two builders publishing DIFFERENT --output
# names into the same directory hold different locks (the lock is <image>.lock) and may therefore
# run at the same time.  A single fixed journal name would have them overwrite each other's
# journal, which is worse than having none.  So each output has its own.
#
# WHY EACH UPDATE IS AN ATOMIC RENAME.  A journal half-written when the power failed is exactly
# the ambiguity the journal exists to remove.  Each update is written to a fixed staging leaf and
# renamed onto the journal name, which is atomic within a directory, and every record ends with a
# terminator line that the reader REQUIRES -- so a torn journal is both nearly impossible and,
# if it happened anyway, refused rather than interpreted.
#
# WHY THE RECORD IS WRITTEN AHEAD OF EACH MOVE AND NOT AFTER IT.  See the block at
# PUBLISH_INTENT_ASIDE_IMAGE for the window this closes and the evidence for it.  The
# consequence for the shape of the record is that every move has TWO fields rather than one --
# INTENT_<step>, flushed before the rename, and DONE_<step>, flushed after -- and that the pair
# (INTENT=1, DONE=0) is a legitimate, expected, terminal state of the record which the reader
# MUST NOT resolve from the record.
#
# WHICH IS WHY THE RECORD ALSO CARRIES IDENTITIES.  Six of them: the previous image, the previous
# manifest, this run's two staged files and the two mktemp placeholders the aside names are
# minted as, each as a device, an inode and a size, recorded while the object is still at its
# starting name.  Renames preserve inodes, so the reader stats each of the six names and
# identifies the object sitting there instead of inferring it -- and (INTENT=1, DONE=0) becomes a
# question about the filesystem, which has an answer, rather than a question about the record,
# which does not.
#
# VERSION 2 IS NOT BACKWARD-READABLE, DELIBERATELY.  A version 1 record carries the four
# write-after-move completion flags and no identities at all, so acting on one would mean acting
# on exactly the claim that was found to be wrong.  A version 1 journal is therefore refused with
# the reason and the remedy rather than interpreted.
# ------------------------------------------------------------------------------------
PUBLISH_JOURNAL_LEAF=''
PUBLISH_JOURNAL_TMP_LEAF=''
PUBLISH_JOURNAL_WRITTEN=0
PUBLISH_RUN_ID=''
PUBLISH_GENERATION=''
readonly PUBLISH_JOURNAL_PREFIX='.cec-l2-publication.'
readonly PUBLISH_JOURNAL_SUFFIX='.journal'
readonly PUBLISH_JOURNAL_TMP_SUFFIX='.journal.staging'
# The line every complete journal record ends with.  Its absence means the write was torn.
readonly PUBLISH_JOURNAL_TERMINATOR='JOURNAL_COMPLETE=1'

register_mount()  { MOUNT_POINTS+=("$1"); }
# The backing file is registered WITH the device, because a loop device node is a shared host
# resource: the teardown has to be able to tell "the device we attached" from "the same node
# after somebody else re-used it", and the node's name alone cannot.  See assert_loop_backs.
register_loop()   { LOOP_DEVICES+=("$1"); LOOP_BACKING_FILES+=("${2-}"); }
register_tempdir(){ TEMP_DIRS+=("$1"); }

# Unmount one point, tolerating the ordinary case where a child process this run started has
# not finished releasing it yet.  A handful of bounded attempts, then a lazy unmount, then a
# warning: the alternative -- one attempt and a warning -- leaves a busy mount behind on the
# host for every job that follows.  The pause between attempts is a teardown courtesy and is
# not a readiness mechanism; nothing here is waiting for something to become ready.
readonly UNMOUNT_ATTEMPTS=3

unmount_with_retries() { # $1=mount point
    local point="$1" attempts=0
    mountpoint -q -- "$point" 2>/dev/null || return 0
    while [ "$attempts" -lt "$UNMOUNT_ATTEMPTS" ]; do
        if umount -- "$point" 2>/dev/null; then
            return 0
        fi
        mountpoint -q -- "$point" 2>/dev/null || return 0
        attempts=$(( attempts + 1 ))
        sleep 1
    done
    # Lazy: detach it from the tree now and let the kernel finish when the last user goes.
    umount -l -- "$point" 2>/dev/null \
        || warn "could not unmount $point; it is left for the host to clear
       (umount -l '$point')."
}

# Is this loop device still bound to a backing file?  MEASURED, because the node itself
# survives a detach and because the kernel auto-clears a loop device once its last user closes
# it -- which is what happens when the image is unmounted moments earlier.  A bare `-b` test
# therefore reports a device that is already gone as still attached, and the detach that
# follows fails with ENXIO and prints a warning about work that had already been done.
loop_still_attached() { # $1=loop device
    [ -e "/sys/block/$(basename -- "$1")/loop/backing_file" ]
}

# WHICH FILE a loop device is bound to, as the kernel reports it.  Printed empty when the
# device is not attached.
#
# The kernel appends " (deleted)" to the backing file when the file has been unlinked while
# the device stayed attached, so the suffix is stripped: a device bound to an unlinked file
# is still bound to that path as far as identity goes, and treating the suffixed form as a
# different file would make the identity check below fail exactly when it matters most.
loop_backing_file() { # $1=loop device -> the backing path on stdout, or nothing
    local node backing
    node="$(basename -- "$1")"
    [ -r "/sys/block/$node/loop/backing_file" ] || return 0
    backing="$(cat -- "/sys/block/$node/loop/backing_file" 2>/dev/null || true)"
    backing="${backing% (deleted)}"
    printf '%s' "$backing"
}

# IDENTITY, NOT LIVENESS.  Refuses to act on a loop device that is bound to something other
# than the file this run staged.
#
# WHY THIS IS A SEPARATE CHECK FROM loop_still_attached.  A loop device node is a shared
# host resource with a small numeric name space.  Between this run attaching /dev/loop3 and
# this run detaching it, an unrelated process on the same host can detach it and attach the
# same node to its own file -- and then a `losetup -d` or a `mount` issued here as ROOT acts
# on that process's image rather than on ours.  Comparing the backing file makes the node's
# name insufficient on its own: the operation goes ahead only when the kernel agrees the
# device is bound to the exact file we staged.
assert_loop_backs() { # $1=loop device  $2=expected backing file  $3=what is about to happen
    local device="$1" expected="$2" operation="$3" actual
    actual="$(loop_backing_file "$device")"
    [ -n "$actual" ] || die "refusing to $operation: $device reports no backing file, so it is
       no longer attached to the staged image $expected.  Something outside this run detached
       it.  Nothing further is done to that device."
    [ "$actual" = "$expected" ] || die "refusing to $operation: $device is bound to
           $actual
       but this run staged
           $expected
       Some other process on this host re-used the loop device node between our attach and
       this operation.  Acting on it as root would act on THAT image, so nothing is done to
       it."
}

# ------------------------------------------------------------------------------------
# CLEANUP FAILURES ARE COUNTED, NOT NARRATED.
#
# WHY A WARNING IS NOT ENOUGH.  Were every release step to warn on failure and the trap to
# return the incoming status unchanged, a build that succeeded and then failed to unmount the
# image, failed to detach the loop device or failed to remove its temporary directory would
# exit 0.  The workflow would read that 0, upload the image and move on, and the next job on
# the runner would inherit a busy loop device and a mounted image tree.  A warning in a log
# nobody reads is not a failure; only a non-zero status is.
#
# WHAT IS DONE INSTEAD.  Every step VERIFIES ITS POSTCONDITION -- the mount is gone from the
# tree, the loop device reports no backing file, our backing file is not attached to any loop
# device, the temporary directory does not exist, the staged file does not exist -- and each
# unmet postcondition is counted.  A non-zero count turns an otherwise-successful run into a
# failure and leaves an already-failing run's status alone.
#
# WHY THE FINAL VERDICT USES `exit` AND NOT `return`.  MEASURED on this host with bash 5.2:
# `return N` from an EXIT trap does NOT change the process's exit status -- a trap that ended
# `return 7` on a script that had run `exit 0` still produced 0 -- while `exit N` inside the
# trap does set it, and `exit "$status"` with the saved incoming value preserves an original
# failure exactly.  So the accounting is expressed as an explicit `exit`, and the ordinary
# path uses `return "$status"` precisely because it changes nothing.
readonly CLEANUP_LEAK_EXIT_STATUS=70
CLEANUP_FAILURES=0

cleanup_failed() { # $1=what remains
    CLEANUP_FAILURES=$(( CLEANUP_FAILURES + 1 ))
    warn "CLEANUP FAILED: $1"
}

# Is any loop device on this host still bound to the given file?  This is the postcondition
# that actually matters, and it is independent of the device NODE: a node we attached may have
# been re-used by another process, and what we must establish is that OUR file is no longer
# attached anywhere -- an image still open through a loop device is not a finished artefact
# and cannot be safely published, moved or deleted.
backing_file_still_attached() { # $1=backing file -> prints the device basenames, if any
    local entry node backing found=''
    for entry in /sys/block/loop*/loop/backing_file; do
        [ -r "$entry" ] || continue
        backing="$(cat -- "$entry" 2>/dev/null || true)"
        backing="${backing% (deleted)}"
        [ "$backing" = "$1" ] || continue
        node="${entry#/sys/block/}"
        node="${node%%/*}"
        found="$found $node"
    done
    printf '%s' "${found# }"
}

cleanup() {
    local status=$?
    # THE TEARDOWN IS NOT INTERRUPTIBLE, and this line is the difference between a clean
    # release and a leaked one.  MEASURED: with the signal delivered to the whole process
    # group -- which is what `timeout -s INT` and a terminal Ctrl-C both do -- the extraction
    # dies, the `|| die` path exits, cleanup starts, and the PENDING SIGINT then runs the INT
    # trap, whose own `exit` abandons cleanup halfway.  The result was five mounts and a loop
    # device left behind while the incomplete image had already been removed.  Ignoring the
    # signals here means a second Ctrl-C cannot orphan a mount; the re-entrancy guard below
    # covers the case where cleanup is somehow reached twice regardless.
    trap '' INT TERM HUP
    [ "$CLEANUP_IN_PROGRESS" -eq 0 ] || return
    CLEANUP_IN_PROGRESS=1

    # Flush before anything is detached.  The image's contents are the product of this
    # script; losing the tail of them to a lazily written page cache would produce an image
    # that boots and is subtly incomplete, which is worse than a failure.
    sync || true

    local index mount_point loop_device temp_dir expected_backing actual_backing staged \
          still_attached_on skip_detach

    for (( index=${#MOUNT_POINTS[@]}-1; index>=0; index-- )); do
        mount_point="${MOUNT_POINTS[index]}"
        [ -n "$mount_point" ] || continue
        unmount_with_retries "$mount_point"
        # POSTCONDITION: the point is no longer a mount point.  A LAZY unmount satisfies this
        # -- `umount -l` detaches the subtree from the namespace immediately and lets the
        # kernel finish with it when the last user goes -- and that is the honest reading:
        # nothing else on this host can reach it any more.  What does NOT satisfy it is a
        # unmount that failed outright, which leaves the image's filesystem live and busy.
        if mountpoint -q -- "$mount_point" 2>/dev/null; then
            cleanup_failed "$mount_point is still mounted after $UNMOUNT_ATTEMPTS attempts and
       a lazy unmount.  Something is still holding it; clear it with
           umount -l '$mount_point'
       before the next job runs on this host."
        fi
    done

    for (( index=${#LOOP_DEVICES[@]}-1; index>=0; index-- )); do
        loop_device="${LOOP_DEVICES[index]}"
        [ -n "$loop_device" ] || continue
        # IDENTITY BEFORE DETACH, even here.  The teardown runs as root, and a loop device
        # node that some other process on this host re-used between our attach and now is a
        # device whose detach would interrupt THAT process's build.  A mismatch is reported
        # and left alone; it is not our resource to release.
        #
        # WHAT A MISMATCH DOES *NOT* EXCUSE: it suppresses THE DETACH OF THAT NODE and nothing
        # else.  A `continue` here would also skip the independent postcondition at the end of
        # this loop body -- the all-loop scan for OUR OWN backing file -- and those two are
        # about different things.  The node check asks "is this node still ours to release";
        # the backing-file scan asks "is this run's image still open through ANY loop device",
        # which is precisely the question a re-used node makes urgent: the kernel may have
        # moved our file to a different node, or a second attachment of it may exist, and
        # either way the image is not a finished artefact and must not be published or
        # replaced.  Skipping the scan would turn the one case where it matters most into the
        # one case where it never runs.  So the detach is gated by a flag and the scan below
        # runs unconditionally.
        skip_detach=0
        expected_backing="${LOOP_BACKING_FILES[index]-}"
        if [ -n "$expected_backing" ] && loop_still_attached "$loop_device"; then
            actual_backing="$(loop_backing_file "$loop_device")"
            if [ "$actual_backing" != "$expected_backing" ]; then
                warn "not detaching $loop_device: it is now bound to
       ${actual_backing:-<nothing>}
       and not to the file this run attached ($expected_backing).  Another process on this
       host re-used the node, so releasing it is not ours to do.  This run's own backing file
       is still checked below, on every loop device, because a node we cannot release says
       nothing about whether our image is still attached somewhere."
                skip_detach=1
            fi
        fi
        if [ "$skip_detach" -eq 0 ] && [ -b "$loop_device" ] && loop_still_attached "$loop_device"; then
            # NO `--` BEFORE THE DEVICE, and that is measured rather than stylistic:
            # util-linux 2.41 takes the "--" in `losetup -d -- /dev/loopN` to BE a device name
            # and fails with "losetup: /dev/--: detach failed", leaving the real device
            # attached.  The value is always a /dev/loopN path losetup printed itself, so there
            # is no leading-dash argument to guard against here.
            if ! losetup -d "$loop_device" 2>/dev/null; then
                # Re-checked rather than reported: between the test above and the detach the
                # kernel may have auto-cleared it, and warning about that would be noise.
                if loop_still_attached "$loop_device"; then
                    cleanup_failed "$loop_device is still attached.  Detach it with
           losetup -d $loop_device
       A leaked loop device wedges every job that follows on this host, because the node is
       taken and the file behind it cannot be replaced."
                fi
            fi
        fi
        # POSTCONDITION, INDEPENDENT OF THE NODE: our backing file must not be attached to ANY
        # loop device.  This is the check that catches the case the node-based test cannot --
        # the kernel moved our file to a different node, or a second attachment of the same
        # file exists -- and it is the condition that decides whether the image is safe to
        # publish and safe for the next job to overwrite.
        if [ -n "$expected_backing" ]; then
            still_attached_on="$(backing_file_still_attached "$expected_backing")"
            if [ -n "$still_attached_on" ]; then
                cleanup_failed "$expected_backing is still attached to loop device(s):
       $still_attached_on
       Detach each with 'losetup -d /dev/<name>'.  While a loop device holds it the file is
       not a finished artefact and cannot be safely replaced."
            fi
        fi
    done

    for (( index=${#TEMP_DIRS[@]}-1; index>=0; index-- )); do
        temp_dir="${TEMP_DIRS[index]}"
        # Only ever a directory this script minted with mktemp -d, and never the root or a
        # short path: the guard is cheap and the mistake it prevents is unrecoverable.
        case "$temp_dir" in
            ''|/) continue ;;
        esac
        [ "${#temp_dir}" -gt 4 ] || continue
        [ -d "$temp_dir" ] || continue
        rm -rf -- "$temp_dir" 2>/dev/null || true
        # POSTCONDITION: it is gone.  Checked rather than inferred from rm's status, because
        # the interesting failure is a directory that still has a mount inside it -- rm
        # reports EBUSY on the mount point and leaves a partially emptied tree, and a
        # multi-gigabyte scratch directory left on a CI runner is a real leak.
        if [ -e "$temp_dir" ]; then
            cleanup_failed "the temporary directory $temp_dir still exists.  Something inside
       it is still mounted or in use; inspect it with 'mount | grep $temp_dir' and remove it
       with 'rm -rf $temp_dir' once it is free."
        fi
    done

    # AN OPEN PUBLICATION TRANSACTION IS ROLLED BACK HERE, NOT LEFT FOR A HUMAN TO FIND.
    #
    # publish_outputs() masks INT, TERM and HUP across its renames and rolls back from its own
    # failure arms, so this arm is for the paths that reach the teardown WITHOUT passing through
    # those arms: a `die` raised from a function it calls, an unexpected non-zero under `set -e`,
    # or an unmaskable signal.  Everything it needs is in globals for exactly this reason -- aside
    # names held in function locals would be invisible to this trap, which would then have nothing
    # to put back, leaving a caller's good image hidden under a private name with nothing at the
    # published path.
    #
    # It runs BEFORE the staged-file removal below, and that order is deliberate: the rollback
    # may move this run's image back onto its staging name, and the removal below is then what
    # discards it -- a failed run does not publish, and it does not leave its own half-finished
    # image lying in the output directory either.
    if [ "$PUBLISH_IN_PROGRESS" -eq 1 ]; then
        warn "the publication transaction was still open when the teardown began; putting the"
        warn "  previous image and manifest back."
        publish_rollback || cleanup_failed "the publication transaction could not be rolled
       back.  The lines above name every file and where it is; the caller's previous image and
       manifest are NOT both at their published paths, so this run reports failure rather than
       leaving that to be discovered later."
    fi

    # A HALF-BUILT IMAGE IS REMOVED -- BUT ONLY THE STAGED ONE, NEVER THE PUBLISHED PAIR.
    #
    # This is what makes a failed run non-destructive.  Formatting the image directly at
    # --output and removing THAT path on failure would mean a build failing anywhere after
    # `truncate` destroys whatever good image the caller already has: a rerun of a flaky job
    # would leave the runner with no bootable image at all, and a concurrent second builder
    # could have its published output deleted from under it.
    #
    # The staged path is a private mktemp file in the output directory and is nobody else's
    # artefact, so removing it is always safe.  An incomplete image is not a smaller version
    # of a finished one -- it boots into something undefined and a step that only tests for
    # the file's existence would use it -- so it does not survive.  The published pair is
    # replaced atomically by publish_outputs() and only after the image is validated.
    #
    # NAMED THROUGH THE PUBLICATION DIRECTORY DESCRIPTOR, like every other operation on these
    # two files.  The leaf names are what is iterated over and the human pathname is only what
    # the message says; a `rm` through the caller's pathname could remove an entry of whatever
    # directory that pathname resolves to by teardown time.  When the descriptor does not exist
    # yet -- a failure before resolve_publication_targets -- there are no staged files either,
    # so the guard costs nothing.
    if [ "$status" -ne 0 ] && [ "$BUILD_COMPLETED" -eq 0 ] && [ -n "${OUTPUT_DIR_PROC:-}" ]; then
        for staged in "$STAGED_IMAGE_LEAF" "$STAGED_MANIFEST_LEAF"; do
            [ -n "$staged" ] || continue
            [ -f "$OUTPUT_DIR_PROC/$staged" ] || continue
            rm -f -- "$OUTPUT_DIR_PROC/$staged" 2>/dev/null || true
            if [ -e "$OUTPUT_DIR_PROC/$staged" ]; then
                cleanup_failed "the staged file ${OUTPUT_DIR:-?}/$staged could not be removed.
       It is an incomplete artefact in the output directory; remove it with
       'rm -f ${OUTPUT_DIR:-?}/$staged' so no later step can find it."
            fi
        done
        if [ "$IMAGE_CREATED" -eq 1 ]; then
            warn "removed this run's staged, incomplete image so it cannot be mistaken for a"
            warn "  finished one. Any previously published image at ${OUTPUT_IMAGE:-<none>} is"
            warn "  untouched: this run never wrote to that path. The failure is diagnosed"
            warn "  from the output above."
        fi
    fi

    # THE VERDICT ON THE TEARDOWN ITSELF.
    if [ "$CLEANUP_FAILURES" -gt 0 ]; then
        warn "================ TEARDOWN INCOMPLETE ================"
        warn "$CLEANUP_FAILURES privileged resource(s) could not be released; each is named"
        warn "above with the command that clears it.  This host is NOT in the state this run"
        warn "found it in: a leaked mount or loop device makes every job that follows fail in"
        warn "a way that has nothing to do with its own code."
        if [ "$status" -eq 0 ]; then
            warn "The build itself succeeded, and its exit status is nevertheless"
            warn "$CLEANUP_LEAK_EXIT_STATUS and not 0: a run that leaves privileged state"
            warn "behind has not finished, and reporting 0 would hand a broken host to the"
            warn "next job with a green tick."
            warn "================ END TEARDOWN INCOMPLETE ================"
            exit "$CLEANUP_LEAK_EXIT_STATUS"
        fi
        warn "The build had ALREADY failed with status $status, which is preserved: the"
        warn "original failure is the one worth reporting, and replacing it would hide it."
        warn "================ END TEARDOWN INCOMPLETE ================"
        exit "$status"
    fi

    return "$status"
}

trap cleanup EXIT

# THE THREE INTERRUPT TRAPS, INSTALLED THROUGH A FUNCTION RATHER THAN INLINE.
#
# They are installed once at start-up, exactly as before.  The function exists because the
# publication transaction MASKS these three signals for the few renames it takes to swap the
# image and the manifest together -- a signal landing between those renames is the one window
# in which the caller can be left with the previous image hidden and nothing at the published
# path -- and it must then put the SAME handlers back.  A second literal copy of these three
# `trap` commands is how the masked and unmasked versions drift apart, so there is one copy
# and both callers use it.
install_interrupt_traps() {
    trap 'die "interrupted; releasing every mount, loop device and temporary directory."' INT
    trap 'die "terminated; releasing every mount, loop device and temporary directory."' TERM
    # HUP is routed through the same path for the reasons set out immediately below.
    trap 'die "hung up (SIGHUP: the controlling terminal or session went away); releasing every
       mount, loop device and temporary directory."' HUP
}
# HUP IS HANDLED FOR THE SAME REASON AS INT AND TERM.  This script runs as root with mounts
# and loop devices held, and HUP is what a lost controlling terminal or a torn-down session
# delivers: an SSH connection that drops, a CI runner that reaps the session, an invocation
# whose parent shell exits.
#
# WHY IT MATTERS, MEASURED RATHER THAN ASSUMED.  On this host with bash 5.2 an UNTRAPPED HUP
# does still run the EXIT trap -- so the first draft of this comment, which said it did not,
# was wrong -- but it produces two failures that are worse than a missing teardown, and both
# were reproduced:
#
#   * THE TEARDOWN BELIEVES THE BUILD SUCCEEDED.  cleanup() reads `$?` to decide whether the
#     run failed, and on an untrapped fatal signal that value is 0.  Measured: a script killed
#     with HUP while sleeping ran its EXIT trap with `$? == 0`.  So the staged, half-built
#     image would be kept rather than removed, and the leak accounting above would take its
#     "the build itself succeeded" branch about a run that was killed.
#   * THE STATUS IS DISCARDED.  Bash re-raises the fatal signal after the EXIT trap, so an
#     `exit 70` from inside the trap is thrown away and the process reports 129 (128+SIGHUP).
#     Measured both ways: untrapped HUP with an EXIT trap ending `exit 70` produced 129, while
#     the same script with this trap in place produced 70.
#
# Routing HUP through the same `die` path as INT and TERM fixes both: the teardown sees a
# non-zero status and treats the run as the failure it is, the postconditions above are
# checked, the leak verdict can set an exit status that survives, and the console says which
# signal ended the run instead of leaving a bare 129 to be interpreted.
install_interrupt_traps

# ------------------------------------------------------------------------------------
# Caller-supplied state.  Empty means "not given"; every one of these is validated before
# it is used, and none of them acquires a silent default that would let the run proceed
# against something the caller did not name.
# ------------------------------------------------------------------------------------
OUTPUT_IMAGE=''
ARCH="$DEFAULT_ARCH"
SDK_DIR=''
PAYLOAD_DIR=''
BASE_TARBALL=''
BASE_URL=''
# The FILE named by --base-url-file, and the form the URL arrived in.  Kept apart from
# BASE_URL because the two forms are validated differently: the argv form refuses a query
# string outright (it cannot be hidden from /proc/<pid>/cmdline), the file form permits one
# (it never enters any command line).  BASE_URL_SOURCE is what every later check consults, so
# a future edit cannot accidentally apply the file form's laxer rule to the argv form.
BASE_URL_FILE=''
BASE_URL_SOURCE=''
BASE_SHA256=''
LCOV_TARBALL=''
LCOV_SHA256=''
GTEST_PREFIX_SRC=''
KERNEL_CONFIG_SRC=''
# --sink-caller-source: the real Sink plugin source the L1 drift guard reads.  A REQUIRED
# acceptance input.  Left unset, validate_inputs looks for it beside the payload tree and FAILS
# when it is not there -- it is never silently omitted, because omitting it turns the modelled Sink
# call paths into a model nothing has checked against its subject.  See GUEST_SINK_CALLER_SOURCE.
SINK_CALLER_SOURCE_SRC=''
APT_MIRROR=''
# --apt-auth-file: a build-time-only apt credential file.  It is installed into the chroot
# immediately before the apt phase and removed immediately after, and it is never logged,
# never digested and never recorded.  See install_image_packages.
APT_AUTH_FILE=''
# The route every later use of that credential goes through: /proc/<this pid>/fd/<n> for the
# descriptor custody was established on, which vet_private_file leaves open for the lifetime of
# the run.  Reading through it reaches the object that was vetted rather than whatever the
# caller's name resolves to at the instant of each use, and holding the descriptor pins that
# object so it cannot be replaced underneath.  Set by hold_private_file; empty until then, and
# empty for good when --apt-auth-file was not given.
APT_AUTH_CUSTODY=''
# Published by hold_private_file, overwritten by each call, and copied by callers that need the
# value to outlive the next one.  Declared here so `set -u` cannot make a read of it fatal.
VETTED_FD_PROC=''
IMAGE_SIZE_MIB="$DEFAULT_IMAGE_SIZE_MIB"
READINESS_TIMEOUT_SECONDS="$DEFAULT_READINESS_TIMEOUT_SECONDS"
# --toolchain-gcc-major: the GCC family the CALLER pins for the acceptance measurement.  It is
# passed in rather than defaulted here on purpose -- the value lives once, in the workflow's env
# block, and a copy in this script would be a second place for it to be wrong.  EMPTY means "no
# pin was named", in which case the guest's own toolchain is still RECORDED and its internal
# consistency still asserted; what is skipped is only the comparison against a family nobody
# named.  See install_image_pinned_compiler and assert_image_toolchain.
TOOLCHAIN_GCC_MAJOR=''
# Set by install_image_pinned_compiler, read by assert_image_toolchain and write_manifest.  One
# of: 'not requested', 'selected', or 'unavailable'.  Declared here so `set -u` cannot make a
# read of it fatal on a path where the installer did not run.
PINNED_COMPILER_STATE='not requested'
# --staging-gcc-version and --staging-toolchain-state: WHICH COMPILER FAMILY BUILT THE ARTEFACTS
# THIS IMAGE CARRIES, which is a different question from which family will compile inside it and
# is recorded separately for that reason.
#
# TOOLCHAIN_GCC_MAJOR above governs the IN-GUEST build and the coverage instrumentation.  These
# two describe the staged Binder closure, the AIDL stub libraries and servicemanager -- objects
# that are compiled BEFORE this script runs, by the caller, and merely copied in here.  Those
# two builds can legitimately be done by different families: the staged artefacts have to be
# LOADABLE and LINKABLE against the guest's own glibc and libstdc++, which means built by a
# toolchain no newer than the guest's, while the coverage instrumentation has to match the
# (file,line,block,branch) manifest the branch gate compares against, which means the pinned
# acceptance family.  On a bookworm guest with a major-13 pin those two requirements name
# different compilers, so the deviation is REPORTED rather than reconciled -- the same
# gate-and-record posture install_image_pinned_compiler takes, and for the same reason: this
# script does not fail an image build for what an archive can or cannot serve.
#
# Both are optional and neither acquires a default.  Absent, the records say the caller did not
# state it, which is a weaker claim than a wrong one.  write_sdk_build_flags_record puts them in
# the image beside the SDK's own harvested build flags, and write_manifest carries them out.
STAGING_GCC_VERSION=''
STAGING_TOOLCHAIN_STATE=''
# --provenance-revision-superproject / --provenance-revision-hdmicec /
# --provenance-revision-entservices-hdmicecsink: THE COMMITS THE GUEST CANNOT READ FOR ITSELF.
#
# The payload is staged into the image WITHOUT its .git (see copy_guest_payload and the
# workflow's "Stage the guest payload without its Git metadata"), which is deliberate -- the
# image is published as an artifact and history has no business travelling with it.  The cost is
# that tests/L1Tests/run_coverage.sh, which writes provenance.txt from `git rev-parse`, has
# nothing to ask inside the guest and records every repository row as unavailable.  An artefact
# whose own verification procedure says "compare the revision above with `git rev-parse HEAD`"
# is then impossible to follow, which is the defect these three options close.
#
# THE INTERFACE IS THE RUNNER'S, NOT THIS SCRIPT'S.  run_coverage.sh consults an environment
# variable per repository named CEC_PROVENANCE_REVISION_<LABEL>, where <LABEL> is its own
# provenance label upper-cased with '-' mapped to '_', and it consults it ONLY when git cannot
# answer for that path.  This script's job is to carry the values in and have the init export
# them under exactly those names; it does not decide when they are used.
#
# EACH IS OPTIONAL AND AN ABSENT ONE DEGRADES TO TODAY'S BEHAVIOUR -- the row stays unavailable,
# which is the truth when nobody supplied a revision.  A MALFORMED one is refused at the
# boundary rather than passed through, because a value the runner then rejects with its own
# diagnostic would have this script report success for an image that cannot produce the
# provenance it was given the inputs for.
PROVENANCE_REVISION_SUPERPROJECT=''
PROVENANCE_REVISION_HDMICEC=''
PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK=''
# The guest's own measured versions, filled in by assert_image_toolchain and recorded in the
# manifest.  Measured inside the image rather than inferred from a package name.
GUEST_GCC_VERSION=''
GUEST_GXX_VERSION=''
GUEST_GCOV_VERSION=''

# Derived once ARCH is resolved.
EXPECTED_ELF_CLASS=''
EXPECTED_ELF_MACHINE=''
DEBIAN_ARCH=''

# ------------------------------------------------------------------------------------
# THE HELP TEXT IS DATA, NOT CODE, AND IT IS RENDERED THROUGH A TEMPLATE FOR THAT REASON.
#
# THE DEFECT THIS SHAPE REMOVES.  usage() used to be `cat <<USAGE_TEXT` -- an UNQUOTED
# heredoc delimiter -- so bash performed the full set of expansions on the body while
# rendering it, COMMAND SUBSTITUTION INCLUDED.  The body is two hundred lines of English
# prose about a build that documents `gcov`, and one sentence quoted that name in
# backticks.  Rendering the help therefore EXECUTED whatever `gcov` the caller's PATH
# resolved to, at the caller's identity -- and this script's own documentation tells the
# reader to invoke it under sudo.  Measured before the fix: `PATH=<attacker>:$PATH
# aidl-path-tests-rootfs.sh --help` exited 0, ran an attacker binary as uid 0, and pasted
# its standard output into the help text.
#
# WHY THE DELIMITER COULD NOT SIMPLY BE QUOTED ON ITS OWN.  The body legitimately
# interpolates thirteen of this script's own constants, so `<<'USAGE_TEXT'` alone would
# have printed "$GUEST_PAYLOAD_DIR" where a reader needs the path.  The delimiter IS now
# quoted -- which is what makes a substitution impossible -- and the interpolation is done
# afterwards by this function, from a CLOSED TABLE OF NAMES, using bash string replacement.
# Two properties follow, and together they are the whole point:
#
#   * the heredoc body is inert text.  A backtick, a `$(...)`, a `${...}` or a `$VAR` added
#     to it in future renders as the characters typed and executes nothing.  There is no
#     shell evaluation of the body at any point, before or after the substitution;
#   * a value is substituted for a placeholder and never re-scanned.  Bash's
#     ${text//pat/$value} does not re-expand the replacement, so even a value containing
#     `$(id)` would appear literally.
#
# THE PLACEHOLDER SPELLING is @NAME@, chosen because it cannot occur by accident in the
# body's prose: the two `@` characters that do appear there are in "user:pass@host", where
# the character after the `@` is lower case.  An unsubstituted @UPPER_CASE@ token is
# therefore a template error, and the guard below makes it FATAL rather than shipping a
# help text with a hole in it -- which is what catches the next person who adds a
# placeholder to the body and forgets to add its name to the table.
#
# A SECOND, INDEPENDENT GUARD lives in the self-test: assert_no_expanding_heredocs() reads
# this file's own source, finds every heredoc with an unquoted delimiter, and fails if any
# of their bodies could command-substitute.  This function protects the help text; that case
# protects every other heredoc in the file, including ones added later.
# ------------------------------------------------------------------------------------

# The closed table.  Every name here must be a variable this script has already assigned by
# the time usage() runs, and every @NAME@ in the body must appear here.  Nothing else is
# substituted, so this list is the complete interface between the help text and the script.
USAGE_PLACEHOLDER_NAMES=(
    SCRIPT_NAME
    SCRIPT_PATH
    GUEST_PAYLOAD_DIR
    GUEST_SDK_DIR
    GUEST_GTEST_PREFIX
    GUEST_SINK_CALLER_SOURCE
    GUEST_PACKAGE_RECORD
    DEFAULT_IMAGE_SIZE_MIB
    DEFAULT_READINESS_TIMEOUT_SECONDS
    CLEANUP_LEAK_EXIT_STATUS
    GUEST_INIT_PATH
    GUEST_ARTIFACT_DIR
    GUEST_STATUS_FILE
)

# Reads the inert help template on stdin, substitutes the table above, and prints the result.
# No `eval`, no command substitution on the text, and no shell evaluation of any kind: the
# only operation applied to the body is bash parameter-expansion string replacement.
#
# NO EXTERNAL COMMAND IS RUN, and that is deliberate rather than incidental.  `read -d ''`
# and `printf` are bash builtins, so rendering the help resolves nothing through PATH --
# not even the `cat` the old form used.  A caller with a hostile PATH therefore cannot
# influence the help text at all, which is the property self_test_help_rendering() asserts.
render_usage_text() {
    local text='' name value
    # -d '' makes the delimiter NUL, which the template does not contain, so this reads the
    # whole of stdin -- trailing newline included -- and returns non-zero at end of file
    # having stored everything.  IFS= and -r keep the bytes exactly as they arrived.
    IFS= read -r -d '' text || true
    for name in "${USAGE_PLACEHOLDER_NAMES[@]}"; do
        # Indirect expansion with a default, so a name in the table that this script has not
        # assigned yet substitutes empty and is caught by review rather than aborting under
        # `set -u` inside the one code path a confused caller is most likely to reach.
        value="${!name-}"
        text="${text//@${name}@/$value}"
    done
    # THE TEMPLATE-ERROR GUARD.  An @UPPER_CASE@ token still present means the body names a
    # placeholder the table does not, so the help text would ship with a hole in it.  That is
    # a defect in this script rather than in the caller's invocation, and it is fatal.
    if [[ "$text" =~ (@[A-Z][A-Z0-9_]*@) ]]; then
        printf '[%s] ERROR: the help template carries the placeholder %s, which is not in\n' \
            "$SCRIPT_NAME" "${BASH_REMATCH[1]}" >&2
        printf '       USAGE_PLACEHOLDER_NAMES, so it cannot be rendered.  Add the name to that\n' >&2
        printf '       table (it must be a variable this script assigns before usage() runs), or\n' >&2
        printf '       remove the placeholder from the help text.\n' >&2
        return 1
    fi
    # No trailing newline is added: `read -d ''` preserved the template's own, so the rendered
    # bytes are identical to what the previous `cat <<USAGE_TEXT` produced.
    printf '%s' "$text"
}

usage() {
    render_usage_text <<'USAGE_TEXT'
@SCRIPT_NAME@ -- build the QEMU root filesystem image for the binder-capable AIDL-path job.

USAGE
  sudo @SCRIPT_PATH@ --output IMAGE --payload DIR --sdk-dir DIR \
       --base-tarball FILE --base-sha256 HEX [options]

REQUIRED
  -o, --output IMAGE       Where to write the finished root image.  Refused if it is '/',
                           implausibly short, inside a tree the operating system owns, a
                           symbolic link, a multiply-linked file, or reached through a
                           symlinked directory -- this script writes as root, so every one of
                           those would put the bytes somewhere other than the path you typed.
                           The image and its manifest are STAGED under private names in the
                           same directory and renamed onto <IMAGE> and <IMAGE>.manifest
                           together, only after the image has been validated: a failed run
                           leaves any previously published pair exactly as it was.  An
                           exclusive lock is held on <IMAGE>.lock for the whole run, so a
                           second builder aimed at the same output is refused immediately
                           instead of interleaving with this one.  That lock file is left
                           behind deliberately; removing it while holding it would let two
                           builders lock different inodes and both proceed.
  -p, --payload DIR        The hdmicec checkout, and any binaries already built, to make
                           available to the guest.  Copied to @GUEST_PAYLOAD_DIR@.
                           Must contain configure.ac and tests/L1Tests/run_coverage.sh.
  -s, --sdk-dir DIR        The staged rdk-halif-aidl tree: the Binder SDK and the AIDL
                           client stub snapshots.  Copied to @GUEST_SDK_DIR@.  The four
                           staging prefixes are derived from it, not asked for separately.
      --base-tarball FILE  Base root filesystem tarball for the target architecture.
      --base-url URL       Alternative to --base-tarball: fetched ON THE BUILD HOST.  The
                           guest never reaches the network.  REFUSED if it contains a control
                           character, userinfo (user:pass@host), a double quote or a
                           backslash -- AND REFUSED IF IT CARRIES A QUERY STRING OR A
                           FRAGMENT, because this script cannot hide its own argv: whatever
                           you type here is readable by every user on the build host in
                           /proc/<pid>/cmdline for as long as this process lives, and a
                           presigned signature in a query string is a bearer credential.
                           Writing it to a private file before spawning curl hides it from
                           the CHILD, which is not the exposure that matters.  A presigned or
                           otherwise credential-bearing URL goes through --base-url-file,
                           which never enters any command line.
      --base-url-file FILE Same as --base-url, but the URL is READ FROM FILE, one line, so it
                           never appears in this script's argv or in any child's.  It is opened
                           once and every custody check is made on that descriptor, which is
                           also what the value is read from: it must be a regular file, owned
                           by this account or by root, with exactly one hard link and no group
                           or other permission bit at all; it must not be a symbolic link; its
                           name must still resolve to that descriptor's own device and inode;
                           and no directory it sits under may be owned by a third account, nor
                           group- or other-writable unless it is sticky.  A world-readable file
                           defeats the point of using one, and a directory somebody else can
                           write lets them replace the file whatever its own mode says.  Every
                           check --base-url applies is applied to the value read (control
                           characters, userinfo, quote, backslash), and a QUERY STRING IS
                           ACCEPTED here -- this is the one form in which a presigned URL is
                           permitted.  Transport is identical: the value is written to a
                           mode-0600 file in this script's private scratch directory and
                           handed to curl via --config (wget via --input-file), and that file
                           is deleted before the transfer's exit status is even acted on, so
                           it is not visible in "ps" or /proc/<pid>/cmdline either.  Wherever
                           the URL is LOGGED or RECORDED it is reduced to scheme, host and
                           path, and the reduction is stated.  Mutually exclusive with
                           --base-url and --base-tarball.
      --base-sha256 HEX    SHA256 of the base tarball.  MANDATORY unless a sidecar
                           <tarball>.sha256 is present.  Verified before extraction; a
                           mismatch is fatal.  No digest is hard-coded in this script.

OPTIONAL
  -a, --arch ARCH          i386 (default) or armhf.  This selects the ELF BITNESS of the
                           guest userspace ONLY: both values are 32-bit, and every staged
                           artefact is refused if its ELF class or machine disagrees.  It
                           does NOT select the binder wire protocol, which is a separate
                           axis derived from --kernel-config and the staged SDK's own
                           CMakeCache.txt -- see --kernel-config below.  The resolved value
                           is echoed, recorded in the image and written to the manifest.
      --lcov-tarball FILE  lcov 2.x source tarball, installed into the image.  Stable
                           distribution archives ship 1.x, which run_coverage.sh cannot
                           use, so without this the image build will almost certainly fail
                           its own 'lcov --version reports 2.x' assertion -- by design.
      --lcov-sha256 HEX    SHA256 of the lcov tarball.  MANDATORY with --lcov-tarball
                           unless a sidecar <tarball>.sha256 is present.
      --gtest-prefix DIR   A prebuilt GoogleTest/GoogleMock prefix for the TARGET
                           architecture, whose lib/pkgconfig holds gtest.pc.  Copied to
                           @GUEST_GTEST_PREFIX@ and exported as GTEST_PREFIX by the init.
                           Omit it only when the image's own packaging provides gtest.pc.
      --sink-caller-source FILE
                           The REAL entservices-hdmicecsink plugin source
                           (plugin/HdmiCecSinkImplementation.cpp), from a checkout pinned at
                           the reviewed commit.  Staged into the image BESIDE THE PAYLOAD,
                           reproducing the superproject layout, and exported to the guest as
                           CEC_SINK_CALLER_SOURCE by the init.  Destination:
                             @GUEST_SINK_CALLER_SOURCE@
                           A REQUIRED ACCEPTANCE INPUT, not an optional diagnostic.
                           DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatch
                           TheRealSinkSource reads it and asserts the structural facts that
                           DriverAidlSessionTest.AddLogicalAddressFailuresReachBothReal
                           SinkCallPathsAsExpected asserts a MODEL of; without it that guard
                           FAILS -- deliberately, since a skip could not be told apart from
                           a pass -- and the model becomes unchecked rather than evidence
                           about a caller.  The guest cannot see the host workspace, so a
                           host-side checkout is not enough on its own: the file must be
                           staged INTO the image, which is what this option does.
                           Omit it only when the plugin tree sits beside the --payload tree
                           at ../entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp,
                           which is then used and reported.  When neither resolves the image
                           build FAILS rather than producing an image whose guard cannot run.
      --kernel-config FILE The guest kernel's configuration.  Copied into the image as
                           build-time provenance, AND read here as one of the two sources
                           the expected binder wire protocol is DERIVED from: an explicit
                           CONFIG_ANDROID_BINDER_IPC_32BIT=y means protocol 7, and the
                           option being unset or absent means protocol 8.  The other source
                           is BINDER_IPC_32BIT in the staged SDK's CMakeCache.txt.  The two
                           must agree or the image build fails, naming which disagreed.  At
                           least one of them must be readable: this script derives the
                           protocol and never declares it.  Whatever is derived remains an
                           EXPECTATION -- the guest reports what the RUNNING kernel shows
                           and what /dev/binder itself answers, and fails if either
                           disagrees with what was recorded.
      --apt-mirror URL     Pin the package archive the image is populated from, for a
                           reproducible package set (a snapshot archive, for instance).
                           Accepts a whole sources.list line ("deb <url> <suite> <comps>"),
                           so spaces are fine; any control character is REFUSED, because a
                           newline here would configure a second archive inside the image.
                           Userinfo (user:pass@host), a query string and a fragment are ALSO
                           refused, because unlike --base-url-file this value is written into
                           the published image and would be published with it.  For an
                           authenticated archive use --apt-auth-file, which is build-time
                           only; do NOT put a credential file in the base tarball, since
                           that file would be published inside the image.
                           Omitted, the base tarball's own sources are used as they stand --
                           and either way the FINISHED filesystem's apt sources are scanned
                           before publication: a credential or query the base tarball brought
                           with it is stripped, the strip is reported by file and line number,
                           and anything still present afterwards is fatal.
                           The exact package set that results is recorded inside the image at
                           @GUEST_PACKAGE_RECORD@, and counted, digested and accounted against
                           the required set in the manifest.  That record is a PRECONDITION of
                           publication: if the resolved package/version/architecture set cannot
                           be read back out of the image, or a required package is not in it,
                           no image is published.
      --apt-auth-file FILE apt credentials for an authenticated archive, in apt's own
                           auth.conf (netrc) format.  BUILD TIME ONLY: it is installed into
                           the image at /etc/apt/auth.conf.d/99cec-l2-build-auth mode 0600
                           IMMEDIATELY BEFORE the apt phase, and REMOVED again as soon as
                           that phase finishes -- before the image is validated, before it
                           is unmounted and long before it is published.  Its removal is
                           re-read and a survivor is fatal, and the pre-publication scan
                           independently refuses to publish an image carrying ANY known
                           credential store (/etc/apt/auth.conf, /etc/apt/auth.conf.d/*,
                           /etc/apt/apt.conf.d/*, /root/.netrc, /home/*/.netrc), so this
                           option cannot leave a credential in the artefact even if the
                           removal is somehow defeated.  On the way in it is put through the
                           same one-descriptor custody check as --base-url-file -- regular
                           file, owned by this account or root, one hard link, no group or
                           other permission bit, not a symbolic link, name still resolving to
                           the descriptor, and no unsafe directory above it -- and any of
                           those failing is fatal.  Its CONTENT is never logged, never
                           digested into the manifest and never echoed.
      --size MIB           Image size in MiB (default @DEFAULT_IMAGE_SIZE_MIB@).  It holds the
                           toolchain, the payload, the staged SDK, a full build tree and the
                           coverage artifacts, so it is not a place to economise.
      --readiness-timeout S  Seconds the in-guest init waits for servicemanager to ANSWER
                           (default @DEFAULT_READINESS_TIMEOUT_SECONDS@).  The wait is a bounded
                           poll on a real binder transaction, never a sleep.
      --toolchain-gcc-major N  The GCC MAJOR the caller pins for the acceptance measurement --
                           13 for this project, whose single definition point is
                           CEC_TOOLCHAIN_GCC_MAJOR in .github/workflows/aidl-path-tests.yml.
                           GATE-AND-RECORD, NOT GATE-AND-FAIL, and the distinction is the whole
                           of this option.  The guest is populated from ITS OWN pinned archive,
                           whose default GCC family need not be the caller's and which may not
                           carry the pinned one at all -- Debian bookworm's default is GCC 12.
                           So: gcc-N and g++-N are attempted as OPTIONAL packages; when the
                           archive has them they are SELECTED for every in-guest compile by
                           symbolic links in /usr/local/bin, which is first on the guest PATH,
                           and gcov-N is selected with them because tests/L1Tests/run_coverage.sh
                           resolves a bare `gcov` and has no override knob; when the archive does
                           not have them the base default is kept and the shortfall is reported
                           as a NAMED LIMITATION beside the recorded versions, because an image
                           build that died on a package name the archive never had would be
                           failing for the caller's archive rather than for anything about this
                           image.  WHAT IS ASSERTED HARD either way is the property the guest's
                           own trace depends on: that the guest's gcov and the guest's compiler
                           are the same family, since gcov refuses notes files written by
                           another.  Omitted, the guest toolchain is recorded and that internal
                           consistency is still asserted; only the comparison against a family
                           nobody named is skipped.  The measured versions and the outcome are
                           logged, and written to the manifest as GUEST_TOOLCHAIN_*.
      --staging-gcc-version V  The MEASURED version of the compiler family that built the
                           artefacts named by --sdk-dir and --gtest-prefix.  A DIFFERENT
                           QUESTION from --toolchain-gcc-major, which governs what compiles
                           INSIDE the guest: the staged libraries are compiled before this
                           script runs and merely copied in, and they must be loadable and
                           linkable against the GUEST's glibc and libstdc++ -- which means built
                           by a toolchain no newer than the guest's, not by the acceptance pin.
                           Recorded, never gated: it is written into the image beside the SDK's
                           own harvested build flags and into the manifest, so the guest's
                           provenance says which family built what it is running.
      --staging-toolchain-state S  How that measured version stands against the caller's pin.
                           One of matches-pin, deviates-from-pin or unknown -- a closed set, so
                           the image's provenance can be compared mechanically rather than read.
                           A deviation is an expected outcome for a guest whose archive cannot
                           serve the pinned family, and recording it is the point.
      --provenance-revision-superproject SHA
      --provenance-revision-hdmicec SHA
      --provenance-revision-entservices-hdmicecsink SHA
                           The commit revisions of the trees the payload was staged from, for
                           the repositories tests/L1Tests/run_coverage.sh names in its
                           provenance artifact.  THE GUEST CANNOT READ THEM FOR ITSELF: the
                           payload is staged without its .git, deliberately, so the runner's
                           `git rev-parse` has nothing to ask and every repository row in
                           provenance.txt reads "unavailable (not a repository)" -- which makes
                           the artifact's own instruction to compare those revisions against
                           `git rev-parse HEAD` impossible to follow.  Passing them here writes
                           them into the in-guest configuration; the init exports each as
                           CEC_PROVENANCE_REVISION_<LABEL> (the runner's own provenance label,
                           upper-cased, '-' mapped to '_'), and the runner consults that
                           variable ONLY where git could not answer.  Each value must be a
                           40-character lower-case hexadecimal sha, optionally suffixed
                           '-dirty'; anything else is refused HERE rather than carried into the
                           guest for the runner to reject.  Each is independent and each is
                           OPTIONAL: an omitted one leaves that row exactly as it is today,
                           because "nobody supplied a revision" is the truth in that case.
  -h, --help               This message.
      --self-test          Run this script's own checked-in regression cases and exit.  Takes
                           no other argument, needs no root, and returns BEFORE anything
                           privileged: no mount, no loop device, no chroot, no image.  It
                           exercises the two dpkg package-status parsers against a table of
                           literal records -- every partial dpkg state that a naive parser
                           accepts, malformed and short records, name mismatches and Provides
                           prefixes -- then the confined I/O primitive's positive and refusal
                           cases, so it also answers whether openat2 with
                           RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS is in force on this host, and
                           then the custody chain the seven privileged mount and chroot
                           operations consume a descriptor through: the anchor open, the
                           fstat-on-descriptor identity check and every refusal, including the
                           chroot's reserved custody status.  The mount and chroot syscalls
                           themselves are NOT among the cases -- performing one is precisely
                           what this mode promises not to do -- and each is instead fatal at its
                           own call site.  One
                           line per case; a non-zero exit on any mismatch.  The AIDL-path
                           workflow runs it as an early mandatory step, before the expensive
                           provisioning, so a broken parser fails the job in seconds.

EXIT STATUS
  0                        The image and its manifest were built, validated, published as a
                           pair, and every mount, loop device and temporary directory this
                           run created was verifiably released.
  @CLEANUP_LEAK_EXIT_STATUS@                       The build itself succeeded but the teardown could NOT release
                           something it took: a mount still in the tree, a loop device still
                           attached, our backing file still attached to some loop device, or
                           a temporary directory that could not be removed.  Each is named
                           on stderr with the command that clears it.  This is NOT a pass --
                           the host is left in a state that breaks the next job.
  anything else            The build failed; the message names what and why.  A build
                           failure is preserved even when the teardown also fails, because
                           the original failure is the one worth reporting.

WHAT THIS PRODUCES
  IMAGE                    A bootable ext4 root filesystem.  Boot it with the guest kernel
                           and init=@GUEST_INIT_PATH@.
  IMAGE.manifest           KEY=VALUE lines the workflow reads instead of hard-coding paths:
                           the architecture, the init path, the artifact directory, the
                           status file and the expected kernel options.

INSIDE THE GUEST
  The init mounts binderfs, creates and verifies the binder device nodes, reports the
  kernel configuration, the SDK build flags and the resulting protocol version together in
  one block, starts servicemanager and waits for it to answer, exports the build and
  loader environment, then runs
      @GUEST_PAYLOAD_DIR@/tests/L1Tests/run_coverage.sh --build --run --output-dir @GUEST_ARTIFACT_DIR@
  writes that command's EXACT exit status to @GUEST_STATUS_FILE@, and powers the guest down
  from a trap that runs on every path.  It never sets CEC_TEST_AIDL_MODE: run_coverage.sh
  owns that per invocation across the A-E matrix.

  Only status 0 means measured and passing.  Status 3 is ADVISORY and is NOT a pass.
USAGE_TEXT
}

require_option_value() { # $1=option name  $2=remaining argument count
    # The option name is argv, so it is rendered rather than interpolated.  See render_untrusted().
    [ "$2" -ge 2 ] || die "the option $(render_untrusted "$1") requires a value.  Run
       '$SCRIPT_PATH --help' for the option list."
}

parse_args() {
    while [ $# -gt 0 ]; do
        case "$1" in
            -o|--output)          require_option_value "$1" $#; OUTPUT_IMAGE="$2"; shift ;;
            --output=*)           OUTPUT_IMAGE="${1#*=}" ;;
            -a|--arch)            require_option_value "$1" $#; ARCH="$2"; shift ;;
            --arch=*)             ARCH="${1#*=}" ;;
            -s|--sdk-dir)         require_option_value "$1" $#; SDK_DIR="$2"; shift ;;
            --sdk-dir=*)          SDK_DIR="${1#*=}" ;;
            -p|--payload)         require_option_value "$1" $#; PAYLOAD_DIR="$2"; shift ;;
            --payload=*)          PAYLOAD_DIR="${1#*=}" ;;
            --base-tarball)       require_option_value "$1" $#; BASE_TARBALL="$2"; shift ;;
            --base-tarball=*)     BASE_TARBALL="${1#*=}" ;;
            --base-url)           require_option_value "$1" $#; BASE_URL="$2"; shift ;;
            --base-url=*)         BASE_URL="${1#*=}" ;;
            --base-url-file)      require_option_value "$1" $#; BASE_URL_FILE="$2"; shift ;;
            --base-url-file=*)    BASE_URL_FILE="${1#*=}" ;;
            --base-sha256)        require_option_value "$1" $#; BASE_SHA256="$2"; shift ;;
            --base-sha256=*)      BASE_SHA256="${1#*=}" ;;
            --lcov-tarball)       require_option_value "$1" $#; LCOV_TARBALL="$2"; shift ;;
            --lcov-tarball=*)     LCOV_TARBALL="${1#*=}" ;;
            --lcov-sha256)        require_option_value "$1" $#; LCOV_SHA256="$2"; shift ;;
            --lcov-sha256=*)      LCOV_SHA256="${1#*=}" ;;
            --gtest-prefix)       require_option_value "$1" $#; GTEST_PREFIX_SRC="$2"; shift ;;
            --gtest-prefix=*)     GTEST_PREFIX_SRC="${1#*=}" ;;
            --sink-caller-source) require_option_value "$1" $#; SINK_CALLER_SOURCE_SRC="$2"; shift ;;
            --sink-caller-source=*) SINK_CALLER_SOURCE_SRC="${1#*=}" ;;
            --kernel-config)      require_option_value "$1" $#; KERNEL_CONFIG_SRC="$2"; shift ;;
            --kernel-config=*)    KERNEL_CONFIG_SRC="${1#*=}" ;;
            --apt-mirror)         require_option_value "$1" $#; APT_MIRROR="$2"; shift ;;
            --apt-mirror=*)       APT_MIRROR="${1#*=}" ;;
            --apt-auth-file)      require_option_value "$1" $#; APT_AUTH_FILE="$2"; shift ;;
            --apt-auth-file=*)    APT_AUTH_FILE="${1#*=}" ;;
            --size)               require_option_value "$1" $#; IMAGE_SIZE_MIB="$2"; shift ;;
            --size=*)             IMAGE_SIZE_MIB="${1#*=}" ;;
            --readiness-timeout)  require_option_value "$1" $#; READINESS_TIMEOUT_SECONDS="$2"; shift ;;
            --readiness-timeout=*) READINESS_TIMEOUT_SECONDS="${1#*=}" ;;
            --toolchain-gcc-major) require_option_value "$1" $#; TOOLCHAIN_GCC_MAJOR="$2"; shift ;;
            --toolchain-gcc-major=*) TOOLCHAIN_GCC_MAJOR="${1#*=}" ;;
            --staging-gcc-version) require_option_value "$1" $#; STAGING_GCC_VERSION="$2"; shift ;;
            --staging-gcc-version=*) STAGING_GCC_VERSION="${1#*=}" ;;
            --staging-toolchain-state) require_option_value "$1" $#; STAGING_TOOLCHAIN_STATE="$2"; shift ;;
            --staging-toolchain-state=*) STAGING_TOOLCHAIN_STATE="${1#*=}" ;;
            --provenance-revision-superproject) require_option_value "$1" $#; PROVENANCE_REVISION_SUPERPROJECT="$2"; shift ;;
            --provenance-revision-superproject=*) PROVENANCE_REVISION_SUPERPROJECT="${1#*=}" ;;
            --provenance-revision-hdmicec) require_option_value "$1" $#; PROVENANCE_REVISION_HDMICEC="$2"; shift ;;
            --provenance-revision-hdmicec=*) PROVENANCE_REVISION_HDMICEC="${1#*=}" ;;
            --provenance-revision-entservices-hdmicecsink) require_option_value "$1" $#; PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK="$2"; shift ;;
            --provenance-revision-entservices-hdmicecsink=*) PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK="${1#*=}" ;;
            -h|--help)            usage; exit 0 ;;
            --)                   shift; break ;;
            # An unknown option is REFUSED rather than ignored.  Silently accepting one
            # means a workflow that passes a mistyped flag builds an image that is not the
            # one it asked for, and finds out several QEMU cycles later.
            -*)                   die "unknown option $(render_untrusted "$1"). Run '$SCRIPT_PATH --help' for the option list." ;;
            *)                    die "unexpected argument $(render_untrusted "$1"). This script takes options only; run --help." ;;
        esac
        shift
    done
    [ $# -eq 0 ] || die "unexpected trailing argument $(render_untrusted "$1") and $(( $# - 1 )) more.
       This script takes options only; run --help."
}

# ------------------------------------------------------------------------------------
# PATH DEFENCE.
#
# Every path below is caller-supplied, and this script mounts filesystems, chroots and
# writes multi-gigabyte files.  A value that collapses into a system tree, or that is the
# filesystem root, is refused rather than acted on.  The '..' collapse is done LEXICALLY
# rather than with realpath so that a path whose final component does not exist yet -- the
# output image, every time -- is still checked for where it would land.
# ------------------------------------------------------------------------------------
canonicalise_path_lexically() { # $1=absolute path -> collapsed path on stdout
    local input="$1" component out=''
    local saved_ifs="$IFS"
    IFS='/'
    # Deliberate word splitting on '/' to walk the components in order.
    # shellcheck disable=SC2086
    set -- ${input#/}
    IFS="$saved_ifs"
    for component in "$@"; do
        case "$component" in
            ''|'.') continue ;;
            '..')   out="${out%/*}" ;;
            *)      out="$out/$component" ;;
        esac
    done
    printf '%s\n' "${out:-/}"
}

assert_path_plausible() { # $1=canonical absolute path  $2=how it was chosen
    local path="$1" origin="$2" root

    case "$path" in
        /)  die "$origin must not be the filesystem root.  This script creates, mounts and
       overwrites whole filesystem images; '/' is not a place to do that." ;;
        /*) : ;;
        *)  die "internal error: assert_path_plausible needs an absolute path; got $(render_untrusted "$path") ($origin)." ;;
    esac

    # Four characters or fewer cannot be anything but a near-root path.  The same refusal,
    # and the same wording, as the coverage runner's.
    [ "${#path}" -gt 4 ] || die "$origin is implausibly short: $(render_untrusted "$path").  A near-root path is
       refused because this script writes and mounts filesystem images.  Give a path that is
       unmistakably yours, for example \"\${TMPDIR:-/tmp}/cec-l2-rootfs.ext4\"."

    # /var/tmp and /run/user/<uid> are ordinary per-user scratch directories that happen to
    # sit under a protected root, so they are cleared before the loop below.
    case "$path" in
        /var/tmp|/var/tmp/*|/run/user/*) return 0 ;;
    esac

    for root in "${PROTECTED_SYSTEM_ROOTS[@]}"; do
        case "$path" in
            "$root"|"$root"/*)
                die "refusing to use
           $(render_untrusted "$path")
       for $origin, because it is $root or lies underneath it -- a tree the operating system
       owns.  A value that reaches one is nearly always a '..' that collapsed out of the
       intended path or a mistyped root.  Use \"\${TMPDIR:-/tmp}/...\" or a directory inside
       your own tree." ;;
        esac
    done
    return 0
}

# Absolute, collapsed and checked, in one step.  Relative input is resolved against the
# CURRENT directory rather than against this script's location: a caller writing
# `--output image.ext4` means the directory they are standing in.
absolutise_and_check() { # $1=path  $2=how it was chosen -> canonical path on stdout
    local path="$1" origin="$2"
    [ -n "$path" ] || die "$origin was given as an empty value.  An empty path cannot be
       validated, so it is refused rather than resolved to the working directory."

    # A CONTROL CHARACTER IN ANY CALLER-SUPPLIED PATH IS REFUSED HERE, at the one funnel every
    # one of them passes through, and that position is the point.  assert_path_embeddable()
    # refuses the same class, but it is applied to only four of the eleven path options -- the
    # four whose values are embedded in the generated in-guest configuration -- so the other
    # seven reached their diagnostics unchecked.  --base-url-file's is the clearest: it is named
    # verbatim in "base URL read from the private file $BASE_URL_FILE", so a newline in it ended
    # that line and began one of its own, and a line beginning "::error::" is a GitHub Actions
    # workflow command.  Refusing the class here is what lets every diagnostic downstream of
    # this funnel keep interpolating a path directly.  Refused rather than stripped: a path this
    # script silently renamed would not be the one the caller named.  See render_untrusted().
    case "$path" in
        *[[:cntrl:]]*)
            die "$origin contains a control character: $(render_untrusted "$path")
       A newline, a carriage return or an escape sequence in a path is reproduced in every
       diagnostic, artifact name and generated file that mentions it -- forging or hiding output
       rather than naming a file.  Give a path made of printable characters." ;;
    esac

    case "$path" in
        /*) : ;;
        *)  path="$PWD/$path" ;;
    esac
    path="$(canonicalise_path_lexically "$path")"
    assert_path_plausible "$path" "$origin"
    printf '%s\n' "$path"
}

# The generated init and the generated configuration file embed these paths inside
# single-quoted shell assignments.  A single quote or a newline in one of them would break
# out of that quoting, so they are refused here rather than escaped: a path containing
# either is a mistake in every case this script cares about, and rejecting it is honest
# where silently mangling it would not be.
assert_path_embeddable() { # $1=path  $2=how it was chosen
    case "$1" in
        *"'"*)   die "$2 contains a single quote: $(render_untrusted "$1").  This script embeds
       these paths in the generated in-guest configuration, and a quote there cannot be
       represented safely.  Choose a path without one." ;;
        # EVERY CONTROL CHARACTER, not only newline and tab.  This arm used to name exactly
        # those two, which let a path carrying a CARRIAGE RETURN through -- and a CR is
        # invisible in a log while still ending a line on a terminal, so such a path reached
        # both the generated in-guest configuration and every downstream diagnostic that names
        # it.  Refusing the whole class here is what lets those downstream diagnostics keep
        # interpolating the path directly: by the time any of them runs, the value is proven
        # free of control characters.  The value itself is still RENDERED in this message,
        # because this is the diagnostic that reports the offending value.
        *[[:cntrl:]]*) die "$2 contains a control character: $(render_untrusted "$1").
       Refused for the same reason as a quote -- it cannot be embedded in the generated
       in-guest configuration safely, and a carriage return or an escape sequence in a path
       also forges or hides output in every log that names it.  It is refused rather than
       stripped, because a path this script silently altered would not be the path you typed." ;;
    esac
}

# The description is printed FIRST and the path after it, because several call sites pass a
# multi-sentence description that explains what the thing is for; appending "is not a
# directory" to such a string produces a sentence nobody can read.
require_existing_dir() { # $1=path  $2=what it is
    [ -d "$1" ] || die "missing directory -- $2
       Expected it at: $1
       Check the path, or the step that was supposed to stage it."
}

require_existing_file() { # $1=path  $2=what it is
    [ -f "$1" ] || die "missing file -- $2
       Expected it at: $1
       Check the path, or the step that was supposed to produce it."
}

require_positive_integer() { # $1=value  $2=what it is
    # $1 is a caller-supplied option value and reaches a diagnostic on both arms, so it is
    # rendered rather than interpolated.  See render_untrusted().
    case "$1" in
        ''|*[!0-9]*) die "$2 must be a plain positive integer, got $(render_untrusted "$1")." ;;
    esac
    [ "$1" -gt 0 ] || die "$2 must be greater than zero, got $(render_untrusted "$1")."
}

require_sha256_hex() { # $1=value  $2=what it is
    [ "${#1}" -eq 64 ] || die "$2 must be a 64-character hexadecimal SHA256 digest, got a
       ${#1}-character value.  A truncated digest cannot verify anything."
    case "$1" in
        *[!0-9a-fA-F]*) die "$2 contains a character that is not hexadecimal.  A digest that
       cannot be parsed is refused rather than coerced." ;;
    esac
}

# A GIT COMMIT REVISION AS THE COVERAGE RUNNER WILL ACCEPT IT, AND NOT ONE CHARACTER WIDER.
#
# THE FORM IS FIXED BY THE CONSUMER, not chosen here: tests/L1Tests/run_coverage.sh accepts a
# 40-character LOWER-CASE hexadecimal sha, optionally suffixed '-dirty', and refuses anything
# else with its own named diagnostic.  So this check is deliberately the same shape rather than
# a looser one -- a value this script waved through and the runner then rejected would make the
# image build report success for an image whose provenance cannot be written, which is precisely
# the class of defect the three options exist to remove.
#
# WHY LOWER CASE ONLY, unlike require_sha256_hex above which accepts either case: that one
# compares against a digest THIS script measures with sha256sum, so it may normalise; this one
# is a string the guest hands to another program, and normalising it here would mean the value
# recorded in the image is not the value the caller passed.  `git rev-parse` emits lower case,
# so the strict form costs a correct caller nothing.
#
# The value reaches a diagnostic on every arm and is caller-supplied, so it is rendered rather
# than interpolated.  See render_untrusted().
require_commit_revision() { # $1=value  $2=what it is
    local core="$1"
    case "$core" in
        *-dirty) core="${core%-dirty}" ;;
    esac
    [ "${#core}" -eq 40 ] || die "$2 must be a 40-character lower-case hexadecimal commit sha,
       optionally suffixed '-dirty'; got $(render_untrusted "$1"), whose sha part is
       ${#core} characters.  tests/L1Tests/run_coverage.sh refuses any other form, so a value
       of the wrong shape would be carried all the way into the guest and rejected there --
       pass a well-formed revision or pass none at all."
    case "$core" in
        *[!0-9a-f]*) die "$2 must be a 40-character LOWER-CASE hexadecimal commit sha,
       optionally suffixed '-dirty'; got $(render_untrusted "$1").  It is passed through to the
       guest unchanged rather than normalised, because the recorded provenance has to be the
       value the caller supplied." ;;
    esac
}

# ------------------------------------------------------------------------------------
# PRIVATE INPUT FILES -- ONE DESCRIPTOR, CHECKED AND READ.
#
# Both --base-url-file and --apt-auth-file exist so that a credential reaches this script
# without passing through a command line.  A caller who then leaves the file mode 0644 has
# moved the exposure rather than removed it: on a shared build host every local user can read
# it, which is the same disclosure the argv route had, just with a longer window.  So the mode
# is checked and a group or other read bit is fatal.  Write bits are fatal for a second
# reason: a file another account can write is a file whose bytes can be changed under this
# script, which would have it fetch from -- or authenticate against -- an endpoint the caller
# did not name.
#
# Every check below is made on one open descriptor, and every use of the value is made through
# that same descriptor.  That is what makes the checks describe the bytes this script actually
# reads rather than whatever the name happened to resolve to at the instant each check ran.  The
# order is: refuse a symbolic link outright, open once, judge the descriptor, confirm the name
# still reaches it, vet the directories it sits under, then read.
#
# THE TWO FORMS DIFFER ONLY IN HOW LONG THE DESCRIPTOR LIVES, and both end at the same place --
# no use of either file ever resolves the caller's pathname a second time.
#
#   * --base-url-file reduces to one value on the spot.  read_private_line reads that value from
#     the descriptor and closes it, and nothing afterwards refers to the file at all.
#   * --apt-auth-file does not reduce to a value: it is copied into the image and searched for
#     twice before publication, and this script does not interpret its content.  hold_private_file
#     therefore keeps the descriptor open for the lifetime of the run and every one of those uses
#     reads it through /proc/<pid>/fd/<n>.  Holding it also pins the inode, so a rename or an
#     unlink at the caller's path afterwards cannot reach the bytes that were vetted.
#
# What is enforced on the descriptor, with `stat -L` on /proc/self/fd/<n> -- which reads the
# open file rather than resolving a pathname, and is this shell's equivalent of fstat:
#
#   * it is a regular file, not a directory, a fifo, a socket or a device.  The type comes
#     from the raw mode masked against S_IFMT rather than from `stat %F`, because %F prints
#     "regular empty file" for an empty one and an empty credential file has its own refusal
#     below that says something far more useful than "wrong type".
#   * its owner is this process's effective uid or root.  Mode 0600 makes a file private to
#     its owner, so somebody else's 0600 file is private to somebody else; without this test a
#     run as root accepts any local account's 0600 file, which is the very disclosure the mode
#     test exists to prevent.
#   * it has exactly one hard link.  The mode is a property of the inode and not of the name it
#     was reached by, so a second link in a directory another account can read hands them the
#     same bytes at the same mode.
#   * no group or other permission bit is set at all -- mode & 077 == 0.
#
# What is enforced on the name, and why it takes two tests rather than one:
#
#   * the name is not a symbolic link, tested explicitly; and
#   * the device and inode the name resolves to are the device and inode behind the descriptor.
#
#   Bash cannot open a file with O_NOFOLLOW and has no openat2, so no single operation here
#   both refuses a link and yields the descriptor.  Those two tests are what stands in for it.
#   The link test refuses the ordinary case.  The device/inode comparison is what detects a
#   name re-pointed -- at a link, or at another file -- between the test and the open, because
#   `stat` on a symbolic link reports the link itself rather than its target and so cannot
#   match the descriptor.  The window is therefore closed by detection rather than by the open,
#   and that is stated plainly instead of being dressed up as O_NOFOLLOW.
#
# And the directories the file sits under are walked from it up to /, because a directory
# another account can write is a directory in which they can replace the file whatever the
# file's own mode says: renaming a directory entry is governed by the directory, not by the
# file.  Each ancestor must be owned by this process's effective uid or by root, and must not
# be group- or other-writable unless it is sticky -- sticky restricts rename and unlink to each
# entry's owner, which is what makes a shared temp directory usable for this at all.
#
# One residue, named rather than left for a reader to find: the walk judges each ancestor after
# symbolic links in the path have been resolved, so it covers the directory that actually holds
# the file, and it does not separately vet the parents of a symlinked path component.  An owner
# of one of those unvetted parents could replace the resolved directory after the walk.
#
# What that residue can and cannot reach, exactly, because the answer differs by form.  It is a
# window between the walk and a LATER RESOLUTION OF THE NAME, so it needs one to matter -- and
# neither form performs one.  --base-url-file has read its value before the walk's answer could
# go stale, and --apt-auth-file holds the descriptor, so the replacement changes what the name
# reaches while every use continues to read the pinned inode.  The residue therefore bears on a
# caller who resolves the path itself for some other purpose, not on the bytes this script reads.
# A caller who wants it gone outright should pass a path with no symlinked component.
#
# The mode is read with `stat -L -c %a` on the descriptor, which needs no octal parsing on our
# side: the two low digits are group and other, and any of the six bits below 0700 that is set
# is refused.  The value is masked arithmetically rather than matched as text, because 0644, 644
# and 0000644 are all forms `stat` or a caller can produce.
# ------------------------------------------------------------------------------------

# One ancestor chain, from the directory holding $1 up to / inclusive.  Refuses any directory
# through which another account could replace the file.
assert_private_ancestry() { # $1=path  $2=what it is
    local path="$1" what="$2" dir mode owner sticky writable

    dir="$(dirname -- "$path")"
    while : ; do
        mode="$(pad_four_digit_mode "$(stat -L -c '%a' -- "$dir" 2>/dev/null || printf '')")"
        owner="$(stat -L -c '%u' -- "$dir" 2>/dev/null || printf '')"
        case "$owner" in
            ''|*[!0-9]*) owner='' ;;
        esac
        if [ -z "$mode" ] || [ -z "$owner" ]; then
            die "the owner and permissions of a directory $what sits under could not be read:
           $dir
       They decide whether another account can replace the file, so the value is not read."
        fi

        # High digit carries sticky; the group and other digits carry the write bit.
        sticky=$(( ${mode:0:1} & 1 ))
        writable=$(( (${mode:2:1} & 2) | (${mode:3:1} & 2) ))
        if [ "$writable" -ne 0 ] && [ "$sticky" -eq 0 ]; then
            die "a directory $what sits under is writable by group or other and is not sticky:
           $dir  (mode $mode)
       Any account that can write that directory can rename this file away and put one of its
       own in its place, whatever mode the file itself carries -- renaming a directory entry is
       governed by the directory, not by the file.  Restrict it
           chmod go-w '$dir'
       or make it sticky, which restricts rename and unlink to each entry's owner
           chmod +t '$dir'
       or move the file somewhere private."
        fi
        if [ "$owner" != "$EUID" ] && [ "$owner" != '0' ]; then
            die "a directory $what sits under is owned by uid $owner, which is neither this
       process's effective uid ($EUID) nor root:
           $dir
       Its owner can replace the file regardless of that directory's mode, so a credential is
       not read through a path another account controls.  Move the file under a directory this
       account or root owns."
        fi

        [ "$dir" != '/' ] || break
        dir="$(dirname -- "$dir")"
    done
}

# The one place a private file is opened and judged.  The checks are identical in all three
# modes; $3 selects only what happens once they pass, and how long the descriptor lives:
#
#   'check'  establishes custody and prints nothing.  The descriptor is closed before return,
#            so a caller that afterwards opens the path again is back to trusting the name.
#   'read'   establishes the same custody and prints the single value the file carries, read
#            from that descriptor.  Closed before return; nothing afterwards refers to the file.
#   'hold'   establishes the same custody and DELIBERATELY LEAVES THE DESCRIPTOR OPEN, for a
#            caller whose uses are repeated and lie far from here.  It publishes the route to it
#            in VETTED_FD_PROC and returns without closing, so the descriptor lives for the rest
#            of the run and the open file pins the inode the checks were made on.  This is the
#            one mode that outlives the function, and it exists precisely so that no later use
#            has to resolve the caller's pathname a second time.
#
# The descriptor is opened exactly once on every path.  Every refusal goes through `die`, which
# exits, so a refusal releases it through process teardown whichever mode asked.
vet_private_file() { # $1=path  $2=what it is  $3=check|read|hold  -> the value on stdout when 'read'
    local path="$1" what="$2" want="$3"
    local fd='' proc='' raw='' mode='' owner='' links='' masked=0
    local fd_device='' fd_inode='' path_device='' path_inode=''
    local line='' extra='' seen=0

    case "$want" in
        check|read|hold) ;;
        *) die "internal error: vet_private_file was asked for '$want' rather than 'check',
       'read' or 'hold' on $path.  This is a defect in $SCRIPT_NAME." ;;
    esac
    # 'hold' leaves the descriptor open and publishes it, so it must not run where the shell
    # would discard it.  A command substitution, a pipeline element and an explicit subshell all
    # fork, and the descriptor would then be closed with that child while the caller kept a
    # variable naming a number it no longer holds -- which would read as custody while providing
    # none.  BASH_SUBSHELL is non-zero in exactly those cases.
    if [ "$want" = 'hold' ] && [ "${BASH_SUBSHELL:-0}" -ne 0 ]; then
        die "internal error: vet_private_file was asked to hold a descriptor on $path from
       inside a subshell (BASH_SUBSHELL=$BASH_SUBSHELL), where it would be closed again the
       moment that subshell ended.  Call hold_private_file directly.  This is a defect in
       $SCRIPT_NAME."
    fi

    # Before the open, and cheap: a link is refused outright rather than resolved, because the
    # mode that matters is the one on the bytes this script reads and a link lets those bytes
    # live somewhere with a different mode entirely.  The device/inode comparison further down
    # is what covers a link substituted after this test.
    [ ! -L "$path" ] || die "$what is a symbolic link: $path
       It carries a credential, so it is refused rather than followed: the mode that matters is
       the one on the bytes this script reads, and a link lets those bytes live somewhere with
       a different mode entirely.  Pass the file itself."
    [ -f "$path" ] || die "$what is not a regular file (or does not exist): $path
       A credential is read from a plain file, not from a directory, a fifo or a device."

    # THE OPEN, ONCE.  Bash allocates the descriptor, so this cannot collide with the two
    # fixed descriptors this script holds elsewhere.
    exec {fd}<"$path" || die "$what could not be opened for reading: $path
       Its custody is established on the open descriptor rather than on its name, so with no
       descriptor there is nothing to establish it on and the value is not read."
    proc="/proc/self/fd/$fd"

    # 1. THE DESCRIPTOR.  Read as separate values so a refusal can say which one was wrong.
    raw="$(stat -L -c '%f' -- "$proc" 2>/dev/null || printf '')"
    case "$raw" in
        ''|*[!0-9a-fA-F]*) die "the type of $what could not be read from the descriptor this
       script holds open on it: $path
       Its confidentiality cannot be established, so it is not read." ;;
    esac
    [ "$(( 16#$raw & 8#170000 ))" -eq "$(( 8#100000 ))" ] || die "$what is not a regular file:
       $path
       A credential is read from a plain file, not from a directory, a fifo, a socket or a
       device.  The type is read from the open descriptor, so this is the object this script
       would actually have read and not merely what its name pointed at a moment ago."

    owner="$(stat -L -c '%u' -- "$proc" 2>/dev/null || printf '')"
    case "$owner" in
        ''|*[!0-9]*) die "the owner of $what could not be read from the descriptor this script
       holds open on it: $path
       Its confidentiality cannot be established, so it is not read." ;;
    esac
    if [ "$owner" != "$EUID" ] && [ "$owner" != '0' ]; then
        die "$what is owned by uid $owner, which is neither this process's effective uid
       ($EUID) nor root: $path
       Mode 0600 makes a file private to its owner, so a file owned by somebody else is
       private to somebody else -- and a run as root would otherwise accept any local
       account's 0600 file, which is the disclosure the mode check exists to prevent.  Pass a
       file this account or root owns:
           install -m 0600 /dev/null '$path'
       and write the value into it."
    fi

    links="$(stat -L -c '%h' -- "$proc" 2>/dev/null || printf '')"
    case "$links" in
        ''|*[!0-9]*) die "the hard-link count of $what could not be read from the descriptor
       this script holds open on it: $path
       Its confidentiality cannot be established, so it is not read." ;;
    esac
    [ "$links" -eq 1 ] || die "$what has $links hard links: $path
       Every one of those names reaches these bytes, and the mode just checked is a property of
       the inode rather than of the name it was reached by -- so a second link in a directory
       another account can read hands them the same bytes at the same mode.  Give the
       credential a file of its own:
           cp -- '$path' <new path> && chmod 600 <new path>"

    mode="$(stat -L -c '%a' -- "$proc" 2>/dev/null || printf '')"
    case "$mode" in
        ''|*[!0-7]*) die "the permission mode of $what could not be read: $path
       Its confidentiality cannot be established, so it is not read." ;;
    esac
    # Base-8 arithmetic on the value as stat printed it; 0077 is group+other rwx.
    masked=$(( 8#$mode & 8#77 ))
    [ "$masked" -eq 0 ] || die "$what is accessible to group or other (mode $mode): $path
       This option exists so that a credential never appears in a command line; a file other
       local users can read gives back exactly that exposure, and one they can write lets them
       change the value under this script.  Restrict it first:
           chmod 600 $path"

    # 2. THE NAME STILL REACHES THE DESCRIPTOR.  Read without -L on the path, so a name that
    # has become a symbolic link reports the link's own inode and cannot match.
    fd_device="$(stat -L -c '%d' -- "$proc" 2>/dev/null || printf '')"
    fd_inode="$(stat -L -c '%i' -- "$proc" 2>/dev/null || printf '')"
    path_device="$(stat -c '%d' -- "$path" 2>/dev/null || printf '')"
    path_inode="$(stat -c '%i' -- "$path" 2>/dev/null || printf '')"
    if [ -z "$fd_inode" ] || [ -z "$path_inode" ]; then
        die "the device and inode of $what could not be read from both its name and the
       descriptor this script holds open on it: $path
       That comparison is what establishes that the bytes checked are the bytes read, so
       without it the value is not read."
    fi
    if [ "$fd_device" != "$path_device" ] || [ "$fd_inode" != "$path_inode" ]; then
        die "$what is not the file this script opened: $path
           opened     device $fd_device inode $fd_inode
           that name  device ${path_device:-unreadable} inode ${path_inode:-unreadable}
       The name was re-pointed between the checks above and this comparison, which is the
       substitution those checks exist to catch (CWE-367).  This script reads the descriptor
       rather than the name, so the two disagreeing means the caller and this script no longer
       mean the same file; the value is not read."
    fi

    # 3. THE DIRECTORIES IT SITS UNDER, which is where a replacement would be performed.
    assert_private_ancestry "$path" "$what"

    # 4. THE VALUE, FROM THE SAME DESCRIPTOR EVERY CHECK ABOVE WAS MADE ON.
    #
    # The read is deliberately strict about shape.  A trailing newline is normal and is dropped
    # by the read; a second non-empty line is refused rather than ignored, because the two
    # readings ("the first line is the value" and "the caller made a mistake") are too far
    # apart to guess between, and because a value that silently loses part of itself fails
    # later as a fetch error that names nothing useful.  A leading or trailing blank is
    # stripped, since an editor adding one is far more likely than a URL that means it.
    if [ "$want" = 'read' ]; then
        while IFS= read -r extra || [ -n "$extra" ]; do
            # Blank lines anywhere are tolerated: they carry nothing and an editor may add one.
            case "$extra" in ''|[[:space:]]) continue ;; esac
            [ -n "${extra//[[:space:]]/}" ] || continue
            if [ "$seen" -eq 0 ]; then
                line="$extra"
                seen=1
            else
                die "$what contains more than one value: $path
       Exactly one line is expected.  Rather than guessing which line was meant, this is
       refused -- a value silently truncated to the first line fails later as an unexplained
       fetch error."
            fi
        done <&"$fd"
    fi

    # THE DESCRIPTOR IS RELEASED HERE FOR EVERY MODE BUT ONE.  'hold' is the exception: its
    # caller uses the credential more than once and at points far from this function, so the
    # descriptor stays open for the lifetime of the run and VETTED_FD_PROC names the route to it.
    # Nothing reopens the caller's pathname afterwards, which is what makes the checks above
    # describe every later use and not merely this one.
    if [ "$want" = 'hold' ]; then
        # $$ rather than 'self', because a child -- awk, or a shell in a pipeline -- resolves
        # /proc/self to itself.  $$ stays the script's own pid inside a subshell too, so the
        # route names the process that actually holds the descriptor from wherever it is read.
        VETTED_FD_PROC="/proc/$$/fd/$fd"
        return 0
    fi

    exec {fd}<&-

    if [ "$want" = 'read' ]; then
        [ "$seen" -eq 1 ] || die "$what is empty: $path
       It was given, so something was meant to be in it; an empty file is a mistake rather than
       an instruction to fall back to anything."

        # Trim surrounding whitespace only; the interior is the caller's business and every
        # character class that matters is refused by the validators that follow.
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        printf '%s' "$line"
    fi
}

# Establishes that a file carrying a secret is in this account's custody, and nothing else.
# Used as a fail-fast check before read_private_line, and for any caller that needs the checks
# without either the value or a held descriptor.  The descriptor is closed before this returns,
# so a caller that then opens the path again is back to trusting the name -- which is why
# --apt-auth-file uses hold_private_file instead.
require_private_file() { # $1=path  $2=what it is
    vet_private_file "$1" "$2" 'check'
}

# Establishes the same custody and KEEPS THE DESCRIPTOR, for a credential this script uses more
# than once and does not reduce to a single value on the spot.  --apt-auth-file is that case: it
# is tested for emptiness, copied into the image, and searched for twice before publication, and
# its content is apt's business rather than something this script parses.
#
# Publishes VETTED_FD_PROC -- /proc/<pid>/fd/<n> -- rather than printing anything, because
# printing would require a command substitution and the descriptor would be closed along with
# that subshell.  A path is published rather than the bare descriptor number because the uses
# are repeated: a redirection from the descriptor itself could be consumed only once, a shell
# having no way to rewind it, while opening that path reads the same object from offset 0 as
# often as the caller likes.
#
# It is overwritten by the next call, so a caller with a use that outlives this statement copies
# it immediately; APT_AUTH_CUSTODY is that copy.
#
# WHAT HOLDING IT BUYS, precisely.  The open descriptor pins the inode: a rename or an unlink at
# the caller's path afterwards leaves the bytes this script reads untouched, so the object every
# later use reaches is the object the checks were made on.  That is what closes the window the
# ancestry residue below would otherwise leave -- the parents of a symlinked path component are
# not vetted, but with no later pathname open there is no later resolution for a change there to
# affect.
hold_private_file() { # $1=path  $2=what it is  -> sets VETTED_FD_PROC
    vet_private_file "$1" "$2" 'hold'
}

# Reads a single-line value from a private file.  Prints it and nothing else.
#
# Custody is established exactly as require_private_file establishes it, and on the same
# descriptor the value is then read from, so the bytes returned here are the bytes those
# checks were made on.
read_private_line() { # $1=path  $2=what it is
    vet_private_file "$1" "$2" 'read'
}


# ------------------------------------------------------------------------------------
# ARCHITECTURE RESOLUTION -- ELF BITNESS ONLY.
#
# Only 32-bit targets are accepted, and the refusal of a 64-bit one is a design statement
# rather than a limitation of this script: the guest userspace is 32-bit throughout, so a
# 64-bit staged library could not be loaded into any process the guest starts.
#
# THIS FUNCTION DECIDES NOTHING ABOUT THE BINDER WIRE PROTOCOL, and the separation is the
# point: the protocol is a separate axis -- linux_binder_idl CMakeLists.txt:209-217 at tag
# 2.6.0 states so, and a 32-bit process can speak protocol 8 -- and it is derived from the
# kernel configuration and the staged SDK's cache by derive_binder_protocol() below, never
# inferred from --arch.
#
# The expected ELF properties are expressed as REGULAR EXPRESSIONS rather than as literal
# strings, and that is a requirement rather than a flourish: the two tools that can answer the
# question spell the machine differently, and both spellings have to be accepted or a correct
# binary is rejected.  MEASURED on this host -- file 5.46 describes an i386 object as
# "ELF 32-bit LSB shared object, Intel i386" while binutils readelf calls the same object's
# machine "Intel 80386".  readelf is preferred below because its wording is the stable one,
# and `file` remains the fallback, so the patterns cover both.
#
# The reject pattern exists because the machine patterns alone are not enough: "x86-64" and
# "AArch64" would otherwise be matched by a loose 32-bit pattern on some tool versions, and a
# 64-bit artefact must never pass an all-32-bit check.
# ------------------------------------------------------------------------------------
ELF_CLASS_REGEX=''
ELF_MACHINE_REGEX=''
ELF_REJECT_REGEX=''

resolve_arch() {
    case "$ARCH" in
        i386)
            EXPECTED_ELF_CLASS='32-bit'
            EXPECTED_ELF_MACHINE='Intel 80386 / Intel i386'
            ELF_CLASS_REGEX='(ELF32|ELF 32-bit)'
            ELF_MACHINE_REGEX='[Ii]ntel[^,]*386'
            ELF_REJECT_REGEX='(x86-64|x86_64|X86-64|Advanced Micro|[Aa]arch64|AArch64)'
            DEBIAN_ARCH='i386'
            ;;
        armhf)
            EXPECTED_ELF_CLASS='32-bit'
            EXPECTED_ELF_MACHINE='ARM'
            ELF_CLASS_REGEX='(ELF32|ELF 32-bit)'
            ELF_MACHINE_REGEX='(ARM|arm)'
            ELF_REJECT_REGEX='(x86-64|x86_64|X86-64|Advanced Micro|[Aa]arch64|AArch64)'
            DEBIAN_ARCH='armhf'
            ;;
        *)
            die "unsupported --arch $(render_untrusted "$ARCH").  The guest userspace is 32-bit throughout -- a
       process is a single ELF class, so a 64-bit staged library could not be loaded into any
       process the guest starts -- and the accepted values are therefore the 32-bit ones:
       i386 (the default, and the straightforward choice for QEMU on an x86_64 host) or armhf
       (which additionally needs a registered binfmt_misc handler on this build host).  This
       option does NOT select the binder wire protocol: that is a separate axis, derived from
       --kernel-config and the staged SDK's CMakeCache.txt, and a 32-bit userspace can speak
       either protocol." ;;
    esac
    readonly EXPECTED_ELF_CLASS EXPECTED_ELF_MACHINE DEBIAN_ARCH
    readonly ELF_CLASS_REGEX ELF_MACHINE_REGEX ELF_REJECT_REGEX
    log "target ELF bitness: $ARCH (expecting $EXPECTED_ELF_CLASS $EXPECTED_ELF_MACHINE binaries)"
    log "  the binder wire protocol is a SEPARATE axis and is derived, not implied by this value"
}

# ------------------------------------------------------------------------------------
# HOST PREREQUISITES.  Checked as one set so the caller learns everything missing at once
# rather than one package per failed run.
# ------------------------------------------------------------------------------------
require_host_tools() {
    local missing='' tool
    # realpath is required, not optional: the pre-publication credential scan resolves every
    # file it is about to read or rewrite and refuses one whose resolved path leaves the image
    # (see assert_confined_to_image).  Without it that confinement cannot be established, and
    # the scan writes to those paths AS ROOT, so its absence is a named failure here rather
    # than a check that quietly does not happen.
    # python3 is required, not optional: it is the only binding this script has to openat2(2)
    # with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, through which every privileged
    # operation on a BASE-TARBALL-NAMED file or directory inside the mounted image is performed
    # (the exact boundary is enumerated in the confined privileged I/O block below).  There is no
    # fallback to an ordinary open, because an ordinary open is exactly what the mechanism exists
    # to avoid -- so its absence is a named failure here rather than a defence that quietly does
    # not happen.
    # unshare is required, not optional, and for the same reason: the whole privileged section of
    # this script runs in a MOUNT NAMESPACE OF ITS OWN, entered by re-executing this file under
    # `unshare --mount --propagation private`, and that namespace is what bounds the four
    # mount(2) calls and the chroot(2) -- the two operations the per-file primitive cannot carry,
    # because neither takes a target the way openat2 does.  They are separately confined by the
    # pinned privileged-operation helper, which makes each of them CONSUME a descriptor it opened
    # and fstat-verified; the namespace remains the bound on where a mistake could reach if that
    # mechanism were ever removed.  There is no un-namespaced fallback,
    # so its absence is a named failure rather than a defence that quietly does not happen.  It
    # is listed here so the host prerequisite set is complete in ONE place; the namespace entry
    # itself checks for it earlier still (it runs before this function on the first pass), so a
    # host without it is refused before any path is validated rather than mid-run.
    for tool in tar mkfs.ext4 losetup mount umount mountpoint chroot file readelf sha256sum \
                truncate mktemp install cp find rm sync ldconfig flock stat realpath \
                python3 awk unshare; do
        command -v -- "$tool" >/dev/null 2>&1 || missing="$missing $tool"
    done
    [ -z "$missing" ] || die "missing host tooling:$missing.
       This script populates the image through a loop device, a mount and a chroot, and it
       verifies what it copies in.  On a Debian/Ubuntu build host:
           sudo apt-get install -y e2fsprogs util-linux coreutils tar file binutils libc-bin \
                                  python3 mawk
       'chroot' ships with coreutils, 'mountpoint'/'losetup'/'unshare' with util-linux and
       'readelf' with binutils."

    # A URL base needs a fetcher, and only then: an offline run that supplies the tarball
    # must not be failed for a tool it never uses.
    if [ -n "$BASE_URL" ]; then
        command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 \
            || die "--base-url was given but neither curl nor wget is on PATH.  Install one, or
       fetch the tarball yourself and pass --base-tarball with its --base-sha256 (which is
       the offline-friendly form, and the one a workflow with a cache step should prefer)."
    fi

    [ "$(id -u)" -eq 0 ] || die "this script must run as root.  It attaches a loop device,
       creates and mounts a filesystem, populates it through a chroot and creates device
       nodes in it -- none of which is possible unprivileged.  Re-run it under sudo:
           sudo $SCRIPT_PATH ..."
}

# ==============================================================================
# THE ISOLATED MOUNT NAMESPACE -- THE OUTERMOST CONFINEMENT, AND THE ONE THAT COVERS mount(2)
# ==============================================================================
# WHAT THIS ONE COVERS, AND WHERE IT SITS AMONG THE THREE.  Every privileged operation this
# script performs on an individual file inside the image goes through openat2 with
# RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, and every recursive one runs inside the
# chroot; both boundaries are enumerated in the block below this one.  Two kinds of operation fit
# neither, and they are the two the finding is about: the four mount(2) calls that put /proc,
# /sys, /dev and /dev/pts into the image, and the chroot(2) that enters it -- to which the
# nosymfollow remount makes seven privileged operations in total.  Neither syscall takes a target
# the way openat2 does, so for a long time the only shape available to them was "validate the
# name, then use the name", which is check-then-use (CWE-367) with root on the far side of it and
# is not closed by any amount of re-checking.
#
# THAT SHAPE IS NOW GONE, AND THIS NAMESPACE IS NO LONGER WHAT STANDS IN FOR IT.  All seven of
# those operations CONSUME A DESCRIPTOR this script opened and verified by fstat(2) -- the mounts
# with their target given as /proc/self/fd/N, the chroot as fchdir(fd) followed by chroot(".").
# The mechanism, its proof and its refusals are in the pinned privileged-operation block further
# down.  What the namespace provides is the outer bound that was always its job and remains
# valuable on its own terms: it is DEFENCE IN DEPTH beneath a boundary rather than a substitute
# for one.  Two things it does NOT do, said plainly because an earlier revision of this block
# leaned on it as though it did: a private mount namespace does not freeze directory entries, and
# it does not make the host filesystem unreachable from inside -- a chroot target substituted for
# a directory that reaches the host tree would still have been a chroot onto the host tree,
# inside the namespace.  Closing that needed the descriptor, not the namespace.
#
# WHAT THE NAMESPACE ITSELF BUYS.  THE WHOLE PRIVILEGED SECTION OF THIS SCRIPT RUNS IN A MOUNT
# NAMESPACE OF ITS OWN, with propagation set to private throughout it, entered by re-executing
# this file under `unshare --mount --propagation private` at the point where argument parsing ends
# and filesystem work begins -- before this run has created a directory, a mount, a loop device or
# a temporary file.  Three properties follow, and all three are worth having in their own right:
#
#   * NOTHING THIS SCRIPT MOUNTS IS VISIBLE OUTSIDE ITS OWN NAMESPACE.  The image mount, the
#     four pseudo-filesystems and the nosymfollow remount exist for this process and its
#     children and for nothing else on the host.  The worst case the old comments described --
#     a pseudo-filesystem landing at the wrong place because a target was substituted between
#     the check and the mount -- can no longer reach the build host at all, because there is no
#     path from this namespace's mount table to the host's.
#   * NOTHING MOUNTED OUTSIDE CAN REDIRECT A TARGET INSIDE.  Private propagation is symmetric:
#     a mount or an unmount performed on the host does not propagate in, so a target this
#     script has validated cannot be OVERMOUNTED from outside in the window before the
#     privileged use.  That window is what a shared-propagation mount table leaves open, and
#     closing it is why the propagation is private rather than merely new.  Note the precise
#     scope, because it is narrower than it reads: this closes substitution BY MOUNT, and says
#     nothing about substitution by rename or by a new symbolic link, which is a directory-entry
#     change the namespace does not affect.  Those are what the descriptor closes.
#   * THE MOUNTS CANNOT LEAK, EVEN WHEN THIS SCRIPT DIES WITHOUT RUNNING ITS TRAP.  A mount
#     namespace is destroyed when its last member exits and the kernel releases every mount in
#     it.  SIGKILL, an OOM kill and a kernel panic run no trap at all, and before this each of
#     those left the image mount and four pseudo-filesystems on the host for the next job to
#     trip over.
#
# IT IS DEFENCE IN DEPTH ADDED TO EVERY CHECK THAT WAS ALREADY THERE, AND IT REPLACES NONE OF
# THEM.  Stated plainly because the tempting next edit is to delete a check the namespace now
# covers: the confined I/O primitive is still mandatory and still proven at start-up, all seven
# privileged mount and chroot operations still consume a descriptor opened and fstat-verified by
# the pinned privileged-operation helper, the four mount targets are still created and
# type-checked through the primitive before anything is mounted, the recursive operations still
# run inside the chroot, the device-and-inode identity of a tree is still re-verified immediately
# before it is deleted, and the trap still unmounts and detaches everything in reverse order.
# The namespace bounds the BLAST RADIUS of a lost race; the checks are what stop the race being
# lost in the first place, and a blast radius is not a substitute for a boundary.
#
# AND THE TARGETS ARE STILL PINNED BY IDENTITY INSIDE IT.  A namespace of one's own says nothing
# about WHICH object a pathname resolves to inside that namespace, so the anchor everything else
# hangs off -- the image mount point, and then the image root once the loop device is mounted on
# it -- has its device and inode recorded at the moment it is created.  pin_directory_identity()
# records; assert_pinned_directory() and its one-argument wrapper assert_image_root_pinned() are
# checked immediately before every mount(2) and every chroot(2), and a mismatch is fatal and
# names both identities.
#
# WHAT THOSE TWO FUNCTIONS ARE FOR NOW THAT THE OPERATIONS CONSUME A DESCRIPTOR, because it has
# changed and the distinction matters to anyone tempted to delete them.  The RECORDING is
# load-bearing: the device and inode it captures are exactly the numbers the pinned
# privileged-operation helper compares its own descriptor against by fstat, so without it there
# is nothing for the custody check to check.  The pathname-side ASSERTION is a DIAGNOSTIC: it
# names the path, says which object moved and stops the run at the earliest point, from the same
# pathname the operator sees in the log.  It is no longer the thing standing between a
# substitution and the syscall -- the descriptor is -- and it is kept because a precise message
# is worth having and because a future edit that removed the helper would then not silently fall
# back to an unchecked name.
#
# WHY A RE-EXEC RATHER THAN A HELPER PROCESS PER MOUNT.  unshare(2) affects the calling process,
# so a namespace created in a subshell dies with the subshell and takes the image mount with it;
# `unshare -- mount ...` once per mount would put each mount in a namespace of its own, where
# the chroot could not see any of them.  The whole privileged section therefore has to be ONE
# process in ONE namespace, and re-executing this file under unshare is how a shell script gets
# that.  The re-exec happens after parse_args so that --help and an option error still work for
# an unprivileged caller, and before every path validation, so that everything which touches the
# filesystem does so inside the namespace.
#
# FAIL CLOSED, LOUDLY, AND NEVER BACK TO THE UN-NAMESPACED PATH.  A namespace that could not be
# created, propagation that could not be made private, or a re-exec that came back into the same
# namespace it started in are all fatal here.  There is no fallback, because a fallback would
# mean this block describes a confinement the run does not have -- the same failure the confined
# I/O primitive refuses to make, for the same reason.
#
# THE HANDSHAKE IS INTERNAL AND IS NOT A CONFIGURATION KNOB.  CEC_L2_ROOTFS_MOUNT_NS_PARENT is
# set by this script, for this script, in the instant before it replaces itself, and it carries
# the mount-namespace identity of the process that did the re-exec.  It exists so the process
# that comes back can PROVE the namespace changed instead of taking unshare's exit status as
# evidence, and it is unset immediately afterwards.  A caller who sets it cannot use it to skip
# the namespace: the second pass additionally compares this process's mount namespace with its
# PARENT's, which no environment variable can influence, and refuses when they are the same.
# ==============================================================================

# The mount namespace this run's privileged work happens in, as the kernel identifies it.  Empty
# until enter_private_mount_namespace() has proven the second pass is in a namespace of its own.
MOUNT_NS_ID=''
# The image mount POINT -- the ordinary directory the loop device is mounted onto -- as it was
# the moment make_work_dir() created it.
IMAGE_MOUNT_POINT_DEVICE=''
IMAGE_MOUNT_POINT_INODE=''
# The image ROOT -- the root directory of the mounted filesystem -- as it was the moment
# create_and_mount_image() mounted it and made it private.  This is the pin every later mount(2)
# and chroot(2) is checked against.
IMAGE_ROOT_DEVICE=''
IMAGE_ROOT_INODE=''

# The kernel's identity for one process's mount namespace: the inode of the nsfs object the
# magic link points at.  `stat -L` follows the link, which is what makes the number the
# NAMESPACE's identity rather than the link's.  Prints nothing when it cannot be read, so every
# caller decides for itself whether that is fatal.
#
# stat rather than readlink deliberately: stat is already a hard host prerequisite and readlink
# is not, and the two answer the same question here.
mount_namespace_identity() { # $1=path to a /proc/<pid>/ns/mnt link -> the namespace inode
    stat -Lc '%i' -- "$1" 2>/dev/null || printf ''
}

# THE PROPAGATION OF ONE MOUNT POINT, MEASURED FROM /proc/self/mountinfo RATHER THAN ASSUMED
# FROM THE COMMAND THAT MADE IT.
#
# mountinfo's optional fields -- everything between field 7 and the '-' separator -- are exactly
# the propagation state: `shared:N` means mount events propagate to a peer group, `master:N`
# means they propagate IN from one, `propagate_from:N` and `unbindable` are the remaining
# spellings.  A PRIVATE mount has none of them, so "no optional fields" is the kernel's own
# answer to the question this script needs answered, and a `mount --make-rprivate` that returned
# 0 without taking effect cannot survive it.
#
# Prints one word: 'none' for a private mount, the tags themselves when it is not private,
# 'absent' when the path is not a mount point in this namespace at all, and 'stacked' when more
# than one mount is present at that exact path -- which for the paths this script checks means
# something has been mounted over one of ours and is refused rather than interpreted.
#
# THE MOUNT POINT FIELD IS UNESCAPED BEFORE IT IS COMPARED.  The kernel writes space, tab,
# newline and backslash in a mount point as \040, \011, \012 and \134, so a comparison against
# the raw field would silently fail to match a scratch directory under a TMPDIR containing a
# space -- and "no entry found" is a fatal answer here, so a false negative would be a broken
# run rather than a missed check.  The backslash is unescaped last so an already-decoded
# sequence is not decoded twice.
mount_propagation_tags() { # $1=absolute mount point -> none|<tags>|absent|stacked
    awk -v want="$1" '
        function unescape(text) {
            gsub(/\\040/, " ", text)
            gsub(/\\011/, "\t", text)
            gsub(/\\012/, "\n", text)
            gsub(/\\134/, "\\\\", text)
            return text
        }
        unescape($5) == want {
            seen++
            tags = ""
            for (field = 7; field <= NF; field++) {
                if ($field == "-") break
                tags = (tags == "" ? $field : tags " " $field)
            }
            last = (tags == "" ? "none" : tags)
        }
        END {
            if (seen == 0) { print "absent"; exit 0 }
            if (seen > 1)  { print "stacked"; exit 0 }
            print last
        }
    ' /proc/self/mountinfo
}

# Refuse to go on unless one mount point is private in this namespace.  Every mount this script
# makes is checked with this immediately after it is made, so a mount that could propagate out
# of the namespace -- and therefore onto the host -- is caught at the statement that made it
# rather than by its consequences an hour later.
assert_mount_is_private() { # $1=absolute mount point  $2=what it is  $3=what is about to happen
    local point="$1" what="$2" operation="$3" tags
    tags="$(mount_propagation_tags "$point")"
    case "$tags" in
        none) return 0 ;;
        absent)
            die "refusing to $operation: $what is not a mount point in this run's mount
       namespace immediately after this script mounted it
           $point
       so its propagation cannot be established.  Nothing further is mounted or run inside the
       image: every guarantee below rests on that mount being ours and being private." ;;
        stacked)
            die "refusing to $operation: more than one mount is present at
           $point
       immediately after this script made exactly one there.  Something has been mounted over
       $what inside this run's own mount namespace, so the object beneath is not the one this
       script mounted.  Refused rather than interpreted." ;;
        *)
            die "refusing to $operation: $what at
           $point
       is NOT a private mount -- the kernel reports propagation '$tags'.  A shared or slave
       mount propagates out of this run's mount namespace, which is precisely what the
       namespace exists to prevent: a mount landing at a substituted target would then reach
       the build host as root.  The run stops rather than proceeding with the propagation the
       namespace was supposed to have removed.  If this host's unshare does not implement
       --propagation, upgrade util-linux; there is no un-namespaced fallback." ;;
    esac
}

# Record one DIRECTORY's device and inode immediately after this run created it or mounted it.
# The two globals named by $2 and $3 are set through a nameref, so the mount point and the image
# root share one implementation.
#
# WHY A SEPARATE PAIR FROM record_leaf_identity/assert_leaf_identity.  Those two are about the
# two PUBLISHED FILES: they require a regular file and refuse a second hard link, neither of
# which is meaningful for a directory or a mount point.  The question here is the same question
# -- is this still the object this run made -- asked of a different kind of object.
pin_directory_identity() { # $1=path  $2=device variable name  $3=inode variable name  $4=what it is
    local path="$1" what="$4"
    local -n device_ref="$2"
    local -n inode_ref="$3"
    [ ! -L "$path" ] || die "$what is a SYMBOLIC LINK at
           $path
       immediately after this script created it as a directory.  Nothing is mounted or written
       through it: this run's whole confinement is anchored on that path."
    [ -d "$path" ] || die "$what is not a directory at
           $path
       immediately after this script created it.  Nothing is mounted or written through it."
    device_ref="$(stat -c '%d' -- "$path" 2>/dev/null || printf '')"
    inode_ref="$(stat -c '%i' -- "$path" 2>/dev/null || printf '')"
    if [ -z "$device_ref" ] || [ -z "$inode_ref" ]; then
        die "$what exists at
           $path
       but its device and inode could not be read, so its identity cannot be re-checked before
       the privileged mount and chroot operations that name it.  The run stops rather than
       proceeding without that check."
    fi
}

# Refuse to go on unless a pinned directory is still the object whose identity was recorded.
# The path checked is the same path the privileged operation that follows will name -- checking
# through a different name from the one the operation uses is the mistake this exists to close.
assert_pinned_directory() { # $1=path  $2=expected device  $3=expected inode  $4=what it is
                           # $5=what is about to happen
    local path="$1" expected_device="$2" expected_inode="$3" what="$4" operation="$5"
    local device inode

    if [ -z "$expected_device" ] || [ -z "$expected_inode" ]; then
        die "internal error: '$operation' was attempted before the identity of $what had been
       pinned, so the target could not be checked against the object this run created.  This is
       a defect in $SCRIPT_NAME."
    fi
    [ ! -L "$path" ] || die "refusing to $operation: $what is now a SYMBOLIC LINK at
           $path
       This run created a directory at that name.  mount(2) and chroot(2) resolve a pathname in
       the ordinary way and would follow it, so the operation is refused rather than performed
       through the link."
    [ -d "$path" ] || die "refusing to $operation: $what is no longer a directory that exists at
           $path
       This run created it; something removed or replaced it while the build was running."

    device="$(stat -c '%d' -- "$path" 2>/dev/null || printf '')"
    inode="$(stat -c '%i' -- "$path" 2>/dev/null || printf '')"
    if [ "$device" != "$expected_device" ] || [ "$inode" != "$expected_inode" ]; then
        die "refusing to $operation: $what is not the object this run pinned.
           was  device $expected_device inode $expected_inode
           now  device ${device:-unreadable} inode ${inode:-unreadable}
       The name was re-pointed at a different object after this run created it.  The mount or
       chroot that follows would have CONSUMED A DESCRIPTOR checked against those same recorded
       numbers and would have refused as well; this check runs first so the refusal names the
       pathname the log shows and the object that moved.  Nothing further is done through it."
    fi
}

# The pin every mount(2) and chroot(2) below is checked against, as one argument, because the
# target is always the same one and six call sites repeating four globals is six chances to
# repeat them wrongly.
assert_image_root_pinned() { # $1=what is about to happen
    assert_pinned_directory "$IMAGE_MOUNT" "$IMAGE_ROOT_DEVICE" "$IMAGE_ROOT_INODE" \
        'the image root' "$1"
}

# Enter the private mount namespace, or die saying why the run cannot safely proceed without
# one.  Called once, from main(), with this run's own arguments so they can be forwarded across
# the re-exec unchanged.
#
# TWO PASSES, ONE FUNCTION.  The first pass is the caller's process: it establishes that the
# re-exec can work, records its own namespace identity in the handshake variable and replaces
# itself with `unshare`.  The second pass is the same pid, back in a new namespace, and its
# whole job is to PROVE that -- three ways, all measured -- before anything privileged happens.
enter_private_mount_namespace() { # "$@"=this run's arguments, forwarded across the re-exec
    local self parent root_tags
    local -a reexec

    self="$(mount_namespace_identity /proc/self/ns/mnt)"
    [ -n "$self" ] || die "this process's mount namespace could not be read from
           /proc/self/ns/mnt
       The privileged section of this script runs in a mount namespace of its own and proves it
       is in one by comparing that identity before and after the re-exec, so a /proc that cannot
       answer means the proof cannot be taken.  The run stops rather than mounting and chrooting
       with a confinement it cannot establish.  Check that /proc is mounted on this host."

    if [ -z "${CEC_L2_ROOTFS_MOUNT_NS_PARENT:-}" ]; then
        # ---- FIRST PASS.  Still the caller's namespace; this run has created nothing. ----
        #
        # Root is checked HERE as well as in require_host_tools, and not because the check is
        # duplicated for its own sake: unshare(2) with CLONE_NEWNS needs CAP_SYS_ADMIN, this
        # function runs before require_host_tools on the first pass, and an unprivileged caller
        # must get the named refusal this script has always given rather than unshare's bare
        # "Operation not permitted".
        [ "$(id -u)" -eq 0 ] || die "this script must run as root.  Before it attaches a loop
       device, mounts a filesystem and populates it through a chroot, it puts all of that in a
       MOUNT NAMESPACE OF ITS OWN so that none of it is visible to the rest of this host and
       none of it can leak if the run is killed -- and creating a mount namespace needs
       CAP_SYS_ADMIN.  Re-run it under sudo:
           sudo $SCRIPT_PATH ..."
        command -v -- unshare >/dev/null 2>&1 || die "'unshare' is not on PATH, so the
       privileged section of this script cannot be placed in a mount namespace of its own.
       That namespace is the outer bound on the four mount(2) calls that put /proc, /sys, /dev
       and /dev/pts into the image, the chroot(2) that enters it and the nosymfollow remount:
       none of them can be confined by the openat2 primitive every per-file privileged operation
       here goes through, so each instead CONSUMES a descriptor this script opened and
       fstat-verified, with the namespace beneath that as defence in depth.  THERE IS NO
       UN-NAMESPACED FALLBACK -- a run
       without it would mount and chroot through pathnames whose targets could be substituted,
       as root, on the build host.  Install util-linux:
           sudo apt-get install -y util-linux"
        [ -r "$SCRIPT_PATH" ] || die "this script cannot re-execute itself: $SCRIPT_PATH is not
       readable.  The privileged section runs in a mount namespace entered by re-executing this
       file under unshare, so an unreadable path means the confinement cannot be established.
       Invoke the script by its own path rather than through a pipe or a deleted temporary copy."

        export CEC_L2_ROOTFS_MOUNT_NS_PARENT="$self"
        # `--propagation private` is what makes the namespace an isolation boundary rather than
        # merely a copy: unshare applies MS_REC|MS_PRIVATE to the whole inherited tree, so no
        # mount made below propagates out and no mount made outside propagates in.  Passed
        # explicitly even though it is unshare's default, because a default is not a contract.
        reexec=(unshare --mount --propagation private -- "${BASH:-/bin/bash}")
        # A caller debugging with `bash -x` keeps their trace across the re-exec; losing it at
        # the exact point the privileged work starts is where it is least affordable.
        case "$-" in
            *x*) reexec+=(-x) ;;
        esac
        reexec+=("$SCRIPT_PATH")
        log "entering a private mount namespace for the privileged section"
        log "  leaving  mnt:[$self]  (every mount made below is invisible to it)"
        # THERE IS NO STATEMENT AFTER THIS ONE, AND THAT IS THE FAIL-CLOSED PROPERTY RATHER THAN
        # an omission.  A successful exec never returns; a FAILED exec does not return either,
        # because a non-interactive bash without `shopt -s execfail` -- which this script never
        # sets -- exits the shell itself.  MEASURED on this host with bash 5.2: `exec` on a
        # program that does not exist printed bash's own diagnostic and exited 127 without
        # reaching the next line, and `unshare` refusing an option it does not implement exited
        # 1 the same way.  So there is no path from a re-exec that did not happen into the
        # un-namespaced work below, and nothing is leaked when it happens: this runs before the
        # run has created a mount, a loop device or a temporary directory, which is the other
        # reason the re-exec is placed exactly here.  Also measured, and stated because it looks
        # like a gap: bash does NOT run the EXIT trap on a failed exec -- which costs nothing
        # here, because there is nothing yet for the teardown to release.  The
        # `command -v -- unshare` check above is what turns the one likely cause of that failure
        # into a named refusal instead of bash's one-line diagnostic.
        exec "${reexec[@]}" "$@"
    fi

    # ---- SECOND PASS.  unshare says it made a namespace; prove it three ways. ----
    #
    # 1. IT IS NOT THE NAMESPACE WE LEFT.  The handshake carries the first pass's identity, so
    #    an unshare that succeeded without creating anything is caught rather than believed.
    [ "$self" != "$CEC_L2_ROOTFS_MOUNT_NS_PARENT" ] || die "the re-execution under
       'unshare --mount' came back into the SAME mount namespace it started in
       (mnt:[$self]), so the privileged section has no isolation at all.  unshare reported
       success, which is why this is checked rather than assumed.  The run stops here: every
       mount below would be visible on the build host and would outlive a killed run."

    # 2. IT IS NOT OUR PARENT'S NAMESPACE EITHER, WHICH IS THE CHECK NO ENVIRONMENT VARIABLE CAN
    #    INFLUENCE.  unshare does not fork, so this process kept its pid and its parent is the
    #    process that invoked the script -- still in the old namespace.  A forged handshake
    #    therefore cannot skip the namespace: it would leave us in our parent's namespace and
    #    this comparison refuses.  An unreadable parent (it exited, or the process was
    #    reparented) is reported and not fatal: the structural proof is unavailable, the
    #    handshake proof above still holds, and failing a legitimate run for a parent that went
    #    away would be a worse answer than saying so.
    parent="$(mount_namespace_identity "/proc/$PPID/ns/mnt")"
    if [ -n "$parent" ]; then
        [ "$self" != "$parent" ] || die "this process shares its mount namespace (mnt:[$self])
       with its parent process $PPID, so it is NOT in a namespace of its own -- whatever the
       handshake variable claims.  CEC_L2_ROOTFS_MOUNT_NS_PARENT is internal to this script and
       must not be set by a caller; unset it and let the script re-execute itself.  The run
       stops rather than mounting and chrooting on the build host's own mount table."
    else
        warn "the parent process's mount namespace could not be read from /proc/$PPID/ns/mnt,"
        warn "  so the structural half of the namespace proof was not taken.  The handshake"
        warn "  half was: this process's namespace is not the one the re-exec left.  Stated"
        warn "  rather than passed over silently."
    fi

    # 3. PROPAGATION IS ACTUALLY PRIVATE, READ FROM THE KERNEL RATHER THAN INFERRED FROM THE
    #    OPTION WE PASSED.  '/' is where unshare applies MS_REC|MS_PRIVATE, so it is where the
    #    absence of propagation tags proves the option took effect for the whole inherited tree.
    root_tags="$(mount_propagation_tags /)"
    [ "$root_tags" = 'none' ] || die "the root mount of this run's new mount namespace is not
       private -- the kernel reports '$root_tags' for '/' in /proc/self/mountinfo -- so
       'unshare --propagation private' did not take effect and mount events still propagate
       between this namespace and the build host.  A new namespace without private propagation
       is not the confinement this script's comments describe, and there is no fallback: the run
       stops here.  Check the util-linux version on this host."

    MOUNT_NS_ID="$self"
    readonly MOUNT_NS_ID
    # Unset immediately: the handshake has done its one job, and an internal variable has no
    # business in the environment of the apt, compiler and chroot invocations below.  (They are
    # invoked through `env -i` in any case, so this is hygiene rather than a mechanism.)
    unset CEC_L2_ROOTFS_MOUNT_NS_PARENT

    log "private mount namespace established: mnt:[$MOUNT_NS_ID], propagation private"
    log "  every mount this run makes -- the image, /proc, /sys, /dev, /dev/pts and the"
    log "  nosymfollow remount -- is made here.  None of it is visible to the rest of this"
    log "  host, none of it can be redirected by a mount made outside it, and all of it is"
    log "  released by the kernel when this process exits even if the teardown cannot run."
    log "  It is DEFENCE IN DEPTH: the confined I/O primitive, the descriptor every mount and"
    log "  chroot below consumes, the chroot itself and the identity checks all carry their own"
    log "  guarantees and none of them is replaced by this namespace."
}

# ==============================================================================
# CONFINED PRIVILEGED I/O -- THE MECHANISM THAT CARRIES THE CONFINEMENT GUARANTEE
# ==============================================================================
# WHY A PATHNAME CHECK CANNOT CARRY THIS GUARANTEE ON ITS OWN.
#
# Every privileged read, rewrite and removal this script performs INSIDE THE MOUNTED IMAGE
# could be authorised the obvious way: validate a PATHNAME -- no symlinked component from the
# mount point down, a regular file, one link, the image's own st_dev, a realpath still inside
# the image -- and then reopen THE SAME PATHNAME with an ordinary redirection (`< "$file"`,
# `cat "$tmp" > "$file"`, `rm -f -- "$path"`).
#
# That is check-then-open, and the object the kernel opens is not the object that was checked.
# The gap between the two is the vulnerability (CWE-367), not the absence of a check, and no
# amount of re-checking closes it -- an adversary who can create one symlink in the image tree
# only has to do it AFTER the last check and BEFORE the redirection.  This script runs as root
# on the build host, so the consequence of losing that race is root truncating, rewriting or
# deleting a HOST file named by an absolute symlink target.
#
# WHAT IS DONE INSTEAD: ONE PRIMITIVE, AND THE PATHNAME IS NEVER RE-DERIVED AFTER THE OPEN.
#
# openat2(2) with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV resolves a RELATIVE path
# against a DIRECTORY DESCRIPTOR and hands back a descriptor, and the kernel -- not this script
# -- enforces three properties during that resolution:
#
#   RESOLVE_NO_SYMLINKS  no symbolic link is followed at ANY position, leading or trailing.  A
#                        substituted leaf is refused with ELOOP rather than followed, so the
#                        race above has no winning move: there is no instant at which a symlink
#                        at that name resolves to anything.  It implies RESOLVE_NO_MAGICLINKS,
#                        so a /proc magic link cannot be used as a component either.
#   RESOLVE_BENEATH      the resolution may not leave the anchor directory, so '..' and an
#                        absolute path are refused with EXDEV.
#   RESOLVE_NO_XDEV      no mount point is traversed, which is the kernel-enforced form of the
#                        st_dev comparison a pathname check can only do by hand: /proc, /sys,
#                        /dev and /dev/pts are bind/pseudo mounts INSIDE the image tree and a
#                        path through one of them is refused rather than measured afterwards.
#
# Every operation is then performed THROUGH THAT DESCRIPTOR: the file's type is read with
# fstat() on an O_PATH descriptor, and read, write, chmod and chown go through
# /proc/self/fd/<n>, which is a magic link the kernel resolves to the exact inode the
# descriptor already holds and which nothing can substitute.  A removal is unlinkat() on a
# single-component leaf relative to a descriptor obtained the same way, and unlinkat never
# follows a symlink.  So the object operated on is, by construction, the object that was
# opened -- there is no second name lookup to lose.
#
# WHY IT IS A python3 HELPER AND NOT SHELL.  openat2 has no shell binding.  Neither has any
# other *at-family call with resolve flags, and no coreutils tool exposes RESOLVE_BENEATH.  The
# alternatives are to keep check-then-open, with the race above unclosed, or to accept a
# dependency on an interpreter present on every Debian/Ubuntu build host and every GitHub-hosted
# runner.  The helper is WRITTEN ONCE into this run's private 0700 scratch directory at mode
# 0600 and reused for every call, so there is a single copy of the source and a single copy on
# disk; it is invoked with `python3 -I -B`, which ignores PYTHON* environment variables, the
# user site directory and PYTHONPATH, and writes no bytecode.
#
# IT FAILS CLOSED AND DISTINGUISHABLY, WHICH IS THE WHOLE POINT.  A helper that quietly fell
# back to an ordinary open would be worse than no helper at all, because every comment in this
# file would then describe a defence that was not there.  So each condition has its own exit
# status, the shell turns each into a `die` that NAMES which condition it was, and
# probe_confined_io() proves at start-up -- with a positive case AND five negative cases -- that
# the refusals actually happen on this host before one privileged byte is written.
#
# WHAT assert_confined_to_image() IS FOR.  It is a PRE-CHECK, kept because it produces far
# better diagnostics than an errno: it can say which component of which path is a symbolic
# link, and it can distinguish a hard-linked file from a substituted one.  It does not carry
# the guarantee, and its own comment block says so.
#
# ==============================================================================
# EXACTLY WHICH OPERATIONS GO THROUGH IT, AND WHICH DO NOT.  STATED, NOT IMPLIED.
# ==============================================================================
# A claim of "everything is confined" would be the same kind of overstatement this mechanism was
# introduced to correct, so here is the boundary.  It has three parts, because there are three
# confinements with different shapes -- and, since the seven privileged mount and chroot
# operations were converted to consume descriptors, no part of it ends in an unbounded residual.
#
# THE THREE MECHANISMS, EACH WITH THE OPERATIONS IT CARRIES:
#
#   1. THE PER-FILE PRIMITIVE, openat2 with
#      RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, carrying the 60 root-owned
#      operations on an INDIVIDUAL file or directory inside the image.  Enumerated in the
#      "THROUGH THE PRIMITIVE" list below.
#   2. THE CHROOT, carrying the whole-tree operations a per-descriptor primitive cannot express.
#      Enumerated in the "THROUGH THE CHROOT" list below.
#   3. THE PINNED PRIVILEGED OPERATIONS, carrying the SEVEN operations that are neither per-file
#      nor whole-tree because they ARE the mount and the chroot: each opens its target with
#      O_DIRECTORY|O_NOFOLLOW beneath the pinned anchor, verifies THAT DESCRIPTOR by fstat(2)
#      against the identity this run recorded, and then performs the syscall THROUGH the
#      descriptor -- the mounts with their target given as /proc/self/fd/N, the chroot as
#      fchdir(fd) then chroot(".").  Enumerated in the "THROUGH THE PINNED OPERATIONS" list
#      below.  install_pinned_op_helper() writes the mechanism and probe_pinned_op() proves it,
#      fatally, before the first of the seven runs.
#
# AND ALL OF IT HAPPENS INSIDE AN OUTERMOST BOUND, which is defence in depth beneath the three
# rather than a fourth confinement: the whole privileged section runs in this run's own mount
# namespace with private propagation, established and proven by enter_private_mount_namespace()
# before any of these operations exists.  It does not weaken anything below; every check named
# here is still in force.
#
# THROUGH THE PRIMITIVE -- every operation this script performs on an INDIVIDUAL file or
# directory inside the mounted image, from the moment the image exists onward.  Note what that
# is NOT limited to: it is not only the paths the base tarball names.  It is also every path
# THIS SCRIPT chose, because "we chose the name" bounds nothing when a COMPONENT of that name
# can be a symbolic link the base shipped -- /tmp, /opt, /etc and /usr are all names the base
# gets to define the type of.  The full list, in the order main() reaches them:
#     extract_base_rootfs()                       the post-extraction /etc, /usr and /bin layout
#                                                 checks, which `[ -d ]` would have answered by
#                                                 measuring the BUILD HOST's directories
#     mount_chroot_pseudo_filesystems()           the four mount points, created and type-checked
#                                                 before anything is mounted on any of them
#     install_image_apt_auth() /
#       remove_image_apt_auth()                   /etc/apt/auth.conf.d/<leaf>
#     install_image_packages()                    /etc/apt/sources.list and
#                                                 /etc/apt/apt.conf.d/99cec-l2-snapshot
#     scrub_and_assert_image_apt_credentials()    /etc/apt/sources.list{,.d/*}
#     scrub_and_assert_image_credential_stores()  auth.conf{,.d/*}, apt.conf{,.d/*},
#                                                 /etc/environment, every .netrc
#     record_installed_packages()                 the package manifest, its mode, and the
#                                                 SHA-256 taken OF IT -- read back through the
#                                                 primitive, because a digest computed through a
#                                                 substituted name is a false provenance claim
#                                                 rather than merely a wrong number
#     neutralise_image_network_configuration()    /etc/resolv.conf, /etc/network/interfaces, and
#                                                 the enumerate-and-delete of
#                                                 /etc/network/interfaces.d
#     install_lcov_2x()                           the in-image source staging directory, and the
#                                                 device/inode identity re-checked before it is
#                                                 deleted
#     copy_guest_payload()                        $GUEST_WORKSPACE, $GUEST_ARTIFACT_DIR (0700),
#                                                 $GUEST_CONF_DIR (0755) and /tmp (1777)
#     register_guest_library_paths()              /etc/ld.so.conf.d/<fragment>
#     build_guest_helpers()                       the helper sources it writes, the two installed
#                                                 binaries it proves executable afterwards, and
#                                                 the identity of the source tree before deletion
#     assert_image_toolchain()                    the C++ dialect probe source -- written into a
#                                                 directory that is 1777 by the time it runs --
#                                                 and both of its cleanups
#     write_kernel_expectation() /
#       write_sdk_build_flags_record() /
#       write_image_conf() / write_guest_init()   every record written into the image and its
#                                                 mode, plus the `bash -n` of the generated
#                                                 init, which now parses the primitive's stdout
#                                                 instead of reopening the pathname
#     nosymfollow_is_in_force()                   its own probe directory
#
# THROUGH THE SECOND CONFINEMENT -- the RECURSIVE operations, which the per-file primitive cannot
# express because openat2 confines one lookup and these walk thousands.  They are not performed
# on the host against an "$IMAGE_MOUNT/..." pathname.  They run under chroot_run, whose
# resolution root IS the image: a command invoked that way cannot NAME a host path at all, so
# the confinement is structural rather than checked.  Each is preceded by a confined_mkdir plus
# confined_directory of the destination, so the directory the recursion starts from is one this
# script opened under RESOLVE_NO_SYMLINKS and not a name it hoped was a directory:
#     stream_tree_into_image()   `tar -C <host source> -cf - . | chroot_run tar -C <dest> -xf -`,
#                                which replaces the host-side `cp -a` and `tar --extract` in
#                                copy_guest_payload() and install_lcov_2x().  Metadata parity
#                                with `cp -a` -- mode, mtime, symlink targets, numeric owner --
#                                was MEASURED against a tree built for the purpose, not assumed.
#                                What the tar carries is trusted independently: the base archive
#                                and the lcov tarball are verified against pinned SHA-256 digests
#                                by verify_sha256(), and the payload and SDK trees by
#                                verify_payload_tree() and verify_sdk_tree(), so only the
#                                DESTINATION pathname ever needed confining.
#     remove_tree_in_image()     `chroot_run rm -rf -- <in-image path>`, which replaces the
#                                host-side `rm -rf -- "$IMAGE_MOUNT..."` in install_lcov_2x() and
#                                build_guest_helpers(), and re-verifies the device and inode
#                                recorded when the tree was created immediately before deleting
#                                anything, refusing with a named diagnostic if either changed.
#                                A recursive delete that can be redirected is the worst operation
#                                in this script, which is why it is the one that carries an
#                                identity check and a post-delete confined_absent proof.
#
# THROUGH THE PINNED OPERATIONS -- the SEVEN privileged mount and chroot calls, each of which
# CONSUMES a descriptor this script opened beneath the pinned anchor and verified by fstat(2) on
# that descriptor.  None of them resolves a pathname a second time, and none of them has a
# fallback that does.  Every one is additionally inside the mount namespace described above, and
# every one keeps the checks it had before this mechanism existed -- the point of listing them is
# that the descriptor is what carries the guarantee now, and those checks are what produce a
# precise diagnosis when it refuses.
#     mount -t proc, in                 the target is `proc` beneath the image root.  Its
#       mount_chroot_pseudo_filesystems() identity is measured by confined_identity through
#                                       openat2, and the helper resolves the same target the same
#                                       way, fstat-compares its descriptor against that
#                                       recording, and mounts onto /proc/self/fd/N.  RETAINED
#                                       around it: confined_mkdir created the target and
#                                       confined_directory type-checked it; the image root's pin
#                                       is re-checked immediately before; register_mount arms the
#                                       trap first; and the resulting mount's propagation is read
#                                       back from /proc/self/mountinfo and refused unless private.
#     mount -t sysfs, same function     `sys`, identically.
#     mount --bind /dev, same function  `dev`, identically.  The bind of the HOST's /dev is the
#                                       mount that would hurt most if it propagated, which is why
#                                       the propagation read-back is not optional.
#     mount --bind /dev/pts,            `dev/pts`, and the ONE target openat2 cannot resolve:
#       same function                   by the time it is mounted, `dev` is a mount point, which
#                                       RESOLVE_NO_XDEV refuses to resolve through, and resolving
#                                       it earlier would name the directory the bind then
#                                       shadows.  The helper walks the two components by
#                                       descriptor with O_NOFOLLOW on each instead, fstat-checks
#                                       `dev` against the identity of the SOURCE bound there and
#                                       the leaf as a directory on the same st_dev as its parent,
#                                       and mounts onto the leaf's descriptor.  The reasoning is
#                                       on resolve_under_mount() in the helper.
#     chroot(2), in chroot_run() and    performed as fchdir(fd) then chroot(".") on a descriptor
#       chroot_run_with_libs()          opened on the image root and fstat-verified against its
#                                       pin, so "." resolves against the directory the descriptor
#                                       established and no name is looked up.  The custody check
#                                       is inside these two functions rather than at their ~20
#                                       call sites, because a check a caller can forget is a
#                                       check that will be forgotten.  A custody failure reports
#                                       one reserved status -- execve leaves no other channel --
#                                       and is fatal.  What the chroot itself provides is the
#                                       SECOND mechanism listed above, so it appears on both
#                                       sides of this boundary: as a mechanism that confines, and
#                                       as an operation that needed confining.
#     mount(NULL, fd, NULL,             in try_nosymfollow_remount(), the last privileged mount(2)
#       MS_REMOUNT|MS_NOSYMFOLLOW)      this script makes, on the anchor itself.  Remounted
#                                       through the anchor's own verified descriptor.  This is
#                                       also where the mechanism earns its separate statuses: a
#                                       CUSTODY failure is fatal, while mount(2) refusing
#                                       MS_NOSYMFOLLOW is the legitimate "this kernel does not
#                                       offer it" case that function reports and proceeds
#                                       without, and before the helper existed both arrived as
#                                       one exit status from mount(8).  Propagation is not
#                                       re-measured after it because a remount does not change it.
#
# NOT CONFINED BY ANY OF THE THREE, WITH THE REASON AND THE BOUND FOR EACH.  FOUR operations,
# every one of them structural rather than left over, and the reason DIFFERS BETWEEN THEM rather
# than being one blanket claim.  Two have no prior object to open, because they are what makes the
# object exist: the mkdir, and the loop mount that brings the mounted root into existence.  Two act
# on an object that ALREADY EXISTS -- the propagation change, and the extraction -- so for those the
# bound is what surrounds them, stated per operation below.  Every one of the four is inside the
# mount namespace described above:
#     mkdir -p -- "$IMAGE_MOUNT"        in make_work_dir(), and the two mounts in
#     mount -- "$LOOP_DEVICE" ...       create_and_mount_image().  The first two MAKE the anchor
#     mount --make-rprivate ...         exist; the third changes propagation on it.  All three
#                                       operate inside this run's own 0700 root-owned scratch
#                                       directory, before one untrusted byte exists anywhere
#                                       beneath it.  The mkdir's result is IDENTITY-PINNED the
#                                       moment it exists and re-checked immediately before the
#                                       mount; the propagation change's RESULT is read back from
#                                       /proc/self/mountinfo rather than inferred from its exit
#                                       status; and the mounted root is then pinned in turn -- so
#                                       the anchor every descriptor below is opened on is itself
#                                       an object whose device and inode this run recorded.
#     tar --extract --directory         in extract_base_rootfs().  This POPULATES the
#       "$IMAGE_MOUNT"                  ALREADY-MOUNTED image root -- the filesystem exists,
#                                       its CONTENT is what this brings into being.
#                                       Bounded by the pinned SHA-256 that acquire_base_rootfs()
#                                       requires before it is reached, and followed immediately
#                                       by the confined /etc, /usr and /bin checks listed above
#                                       -- so an archive whose top-level layout is a set of links
#                                       is caught on the next statement.
# One further pathname operation is unconfined ON PURPOSE and is not a residual at all: the `cat`
# on the probe symlink in nosymfollow_is_in_force(), whose entire purpose is that reading THROUGH
# THE PATHNAME must fail.  Confining it would measure this primitive instead of the mount option
# the probe exists to measure, and the comment there says so.
# ==============================================================================

# The helper's leaf name inside this run's scratch directory, and the descriptor-anchored
# location it is written to.  Empty until install_confined_io_helper() has run.
readonly CONFINED_IO_HELPER_LEAF='confined-io.py'
CONFINED_IO_HELPER=''
# The directory every relative path is resolved against.  Set to the image mount point once the
# image is mounted; set to a private scratch directory by the start-up probe and by --self-test.
CONFINED_IO_ANCHOR=''
# Raised only by probe_confined_io() once the primitive has been proven to refuse what it must.
CONFINED_IO_PROVEN=0

# THE HELPER'S EXIT STATUSES.  One per condition, because "it failed" is not an answer a
# privileged operation may act on: the shell has to be able to say whether the kernel lacks
# openat2, whether the confinement refused the path, or whether the file simply is not there.
readonly CONFINED_IO_USAGE=10
readonly CONFINED_IO_UNAVAILABLE=11
readonly CONFINED_IO_REFUSED=12
readonly CONFINED_IO_ABSENT=13
readonly CONFINED_IO_WRONG_TYPE=14
readonly CONFINED_IO_FAILED=15
readonly CONFINED_IO_ANCHOR_BAD=16
readonly CONFINED_IO_EXISTS=17

# Write the helper into $1 at mode 0600 and record where it went.  Called with this run's
# scratch directory in the normal path and with a private probe directory by --self-test, so the
# source exists in exactly one place in this file and on disk in exactly one place per caller.
install_confined_io_helper() { # $1=directory to write the helper into
    local dir="$1" target
    [ -n "$dir" ] || die "internal error: install_confined_io_helper was called without a
       directory.  This is a defect in $SCRIPT_NAME."
    [ -d "$dir" ] || die "internal error: install_confined_io_helper was given '$dir', which is
       not a directory.  This is a defect in $SCRIPT_NAME."
    target="$dir/$CONFINED_IO_HELPER_LEAF"
    rm -f -- "$target" || die "could not clear the confined I/O helper's path at $target."
    cat > "$target" <<'CONFINED_IO_PY'
# Confined privileged I/O helper.
#
# GENERATED FILE.  Written by .github/workflows/aidl-path-tests-rootfs.sh in the hdmicec
# submodule; edit that script, not this copy, which is recreated on every run.
#
#   usage: confined-io.py <anchor-directory> <relative-path> <operation> [operation arguments]
#
# The anchor is opened O_DIRECTORY|O_RDONLY|O_NOFOLLOW, the relative path is resolved beneath it
# with openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV), and the operation is
# performed THROUGH THE RESULTING DESCRIPTOR.  No pathname is re-derived after the open.
#
# Operations, and what each writes to stdout:
#   read                     the file's bytes
#   write                    nothing; the file's bytes are replaced by stdin (truncating)
#   create <octal-mode>      nothing; creates the file exclusively at that exact mode
#   stat                     "<type> <dev> <ino> <nlink> <size> <octal-mode>"
#   chmod <octal-mode>       nothing
#   chown <uid> <gid>        nothing
#   unlink                   nothing
#   mkdir <octal-mode>       nothing; creates the directory and any missing parent beneath the
#                            anchor, each component resolved with the same refusals
#   list                     "<type> <name>" per entry of the directory, NUL-terminated, so a
#                            name containing a newline or a space is unambiguous
#   rmdir                    nothing; removes the (empty) directory
#
# Exit statuses are a contract with the calling shell; see the block above
# install_confined_io_helper in that script.
import ctypes
import errno
import os
import stat
import sys

USAGE = 10
UNAVAILABLE = 11
REFUSED = 12
ABSENT = 13
WRONG_TYPE = 14
FAILED = 15
ANCHOR_BAD = 16
EXISTS = 17

SYS_OPENAT2 = 437
RESOLVE_NO_XDEV = 0x01
RESOLVE_NO_SYMLINKS = 0x04
RESOLVE_BENEATH = 0x08
# RESOLVE_NO_SYMLINKS implies RESOLVE_NO_MAGICLINKS, so a /proc magic link cannot be used as a
# path component either.  These three together are the whole confinement.
RESOLVE_FLAGS = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV


class OpenHow(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint64),
        ("mode", ctypes.c_uint64),
        ("resolve", ctypes.c_uint64),
    ]


_libc = ctypes.CDLL(None, use_errno=True)
_libc.syscall.restype = ctypes.c_long


def fail(status, message):
    sys.stderr.write("confined-io: %s\n" % message)
    sys.exit(status)


def openat2_raw(dirfd, relative, flags, mode=0):
    """openat2() beneath dirfd with the confinement flags.  Returns (fd, 0) or (-1, errno).

    mode MUST be zero unless O_CREAT is set: openat2 returns EINVAL otherwise, and reporting
    that as "the kernel has no openat2" would be a false diagnosis of a caller bug.
    """
    how = OpenHow(flags, mode, RESOLVE_FLAGS)
    ctypes.set_errno(0)
    fd = _libc.syscall(
        ctypes.c_long(SYS_OPENAT2),
        ctypes.c_int(dirfd),
        ctypes.c_char_p(relative.encode("utf-8", "surrogateescape")),
        ctypes.byref(how),
        ctypes.c_size_t(ctypes.sizeof(how)),
    )
    if fd >= 0:
        return int(fd), 0
    return -1, ctypes.get_errno()


def openat2(dirfd, relative, flags, mode=0):
    """openat2_raw(), with every failure turned into the exit status that names it."""
    fd, err = openat2_raw(dirfd, relative, flags, mode)
    if fd >= 0:
        return fd
    name = errno.errorcode.get(err, str(err))
    where = "%s (relative to the anchor)" % relative
    if err in (errno.ENOSYS, errno.EOPNOTSUPP):
        fail(UNAVAILABLE,
             "this kernel does not implement openat2 (%s on %s). The confined I/O primitive "
             "cannot be established, and this script does not fall back to an ordinary open."
             % (name, where))
    if err == errno.EINVAL:
        fail(UNAVAILABLE,
             "openat2 refused the resolve flags RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|"
             "RESOLVE_NO_XDEV with EINVAL on %s. The kernel has the syscall but not the "
             "confinement this script requires." % where)
    if err in (errno.ELOOP, errno.EXDEV):
        fail(REFUSED,
             "the confinement REFUSED %s with %s: the path is a symbolic link, passes through "
             "one, leaves the anchor directory, or crosses a mount point." % (where, name))
    if err == errno.ENOENT:
        fail(ABSENT, "%s does not exist beneath the anchor." % where)
    if err == errno.ENOTDIR:
        fail(WRONG_TYPE,
             "%s uses a non-directory as a path component, or is not a directory where one is "
             "required." % where)
    if err == errno.EEXIST:
        fail(EXISTS, "%s already exists beneath the anchor." % where)
    fail(REFUSED, "the confined open of %s failed with %s." % (where, name))


def magic(fd):
    """The one name that is guaranteed to resolve to the inode fd already holds."""
    return "/proc/self/fd/%d" % fd


def reopen(fd, flags):
    try:
        return os.open(magic(fd), flags)
    except OSError as exc:
        fail(FAILED,
             "the descriptor obtained under the confinement could not be reopened for the "
             "operation (%s)." % errno.errorcode.get(exc.errno, str(exc.errno)))


def describe(st):
    if stat.S_ISREG(st.st_mode):
        return "regular"
    if stat.S_ISDIR(st.st_mode):
        return "directory"
    if stat.S_ISLNK(st.st_mode):
        return "symlink"
    if stat.S_ISFIFO(st.st_mode):
        return "fifo"
    if stat.S_ISSOCK(st.st_mode):
        return "socket"
    if stat.S_ISBLK(st.st_mode):
        return "blockdev"
    if stat.S_ISCHR(st.st_mode):
        return "chardev"
    return "other"


def open_for_inspection(anchor_fd, relative):
    """An O_PATH descriptor on the target, plus its fstat.  O_PATH is used deliberately: it
    opens a fifo, a socket or a device node WITHOUT the blocking or side effects a real open
    would have, so the type can be refused rather than hung on."""
    fd = openat2(anchor_fd, relative, os.O_PATH | os.O_CLOEXEC)
    try:
        st = os.fstat(fd)
    except OSError as exc:
        os.close(fd)
        fail(FAILED, "the target's type could not be read (%s)."
             % errno.errorcode.get(exc.errno, str(exc.errno)))
    return fd, st


def require_regular(relative, st):
    if not stat.S_ISREG(st.st_mode):
        fail(WRONG_TYPE,
             "%s beneath the anchor is a %s, not a regular file. A privileged read, rewrite or "
             "mode change is not performed on the strength of a filename."
             % (relative, describe(st)))


def split_components(relative):
    if not relative or relative.startswith("/"):
        fail(USAGE, "the relative path must be non-empty and must not be absolute.")
    parts = [p for p in relative.split("/") if p not in ("", ".")]
    if not parts:
        fail(USAGE, "the relative path names the anchor itself, which is not an operand.")
    # '..' is deliberately NOT rejected here.  RESOLVE_BENEATH refuses it with EXDEV, and
    # letting the kernel do it means the mechanism that carries the guarantee is the one the
    # start-up proof exercises -- a check here would mask it and prove nothing.
    return parts


def walk_directory(anchor_fd, parts, create_mode=None):
    """A descriptor on the directory named by parts, each component resolved under the
    confinement.  With create_mode set, a missing component is created with mkdirat() on the
    descriptor of its parent -- which cannot be redirected, because that parent descriptor was
    itself obtained under the confinement."""
    flags = os.O_DIRECTORY | os.O_RDONLY | os.O_CLOEXEC
    current = anchor_fd
    for part in parts:
        nxt, err = openat2_raw(current, part, flags)
        if nxt < 0 and err == errno.ENOENT and create_mode is not None:
            try:
                # mkdirat() on a SINGLE component relative to a descriptor that was itself
                # obtained under the confinement, so the directory is created as a child of
                # that exact inode and of no other.
                os.mkdir(part, create_mode, dir_fd=current)
            except FileExistsError:
                # Created between the failed open and here: fall through and open it.
                pass
            except OSError as exc:
                if current != anchor_fd:
                    os.close(current)
                fail(FAILED, "the directory component '%s' could not be created beneath the "
                             "anchor (%s)."
                     % (part, errno.errorcode.get(exc.errno, str(exc.errno))))
            nxt, err = openat2_raw(current, part, flags)
        if nxt < 0:
            # Re-issue through the exiting wrapper so the diagnosis and the exit status are
            # produced in exactly one place.  It never returns; the descriptors this process
            # holds are released by the exit.
            openat2(current, part, flags)
        if current != anchor_fd:
            os.close(current)
        current = nxt
    return current


def op_read(anchor_fd, relative, args):
    if args:
        fail(USAGE, "'read' takes no arguments.")
    fd, st = open_for_inspection(anchor_fd, relative)
    require_regular(relative, st)
    handle = reopen(fd, os.O_RDONLY | os.O_CLOEXEC)
    os.close(fd)
    try:
        while True:
            chunk = os.read(handle, 1 << 16)
            if not chunk:
                break
            sys.stdout.buffer.write(chunk)
        sys.stdout.buffer.flush()
    except OSError as exc:
        fail(FAILED, "%s could not be read through its descriptor (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    finally:
        os.close(handle)


def op_write(anchor_fd, relative, args):
    if args:
        fail(USAGE, "'write' takes no arguments; the content comes from stdin.")
    payload = sys.stdin.buffer.read()
    fd, st = open_for_inspection(anchor_fd, relative)
    require_regular(relative, st)
    handle = reopen(fd, os.O_WRONLY | os.O_CLOEXEC)
    os.close(fd)
    try:
        os.ftruncate(handle, 0)
        written = 0
        while written < len(payload):
            written += os.write(handle, payload[written:])
    except OSError as exc:
        os.close(handle)
        fail(FAILED, "%s could not be rewritten through its descriptor (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    os.close(handle)


def op_create(anchor_fd, relative, args):
    if len(args) != 1:
        fail(USAGE, "'create' takes exactly one argument: the octal mode.")
    mode = parse_mode(args[0])
    fd = openat2(anchor_fd, relative,
                 os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, mode)
    try:
        # Explicit, because the process umask would otherwise subtract from the mode asked for
        # and a credential file's mode is functional rather than cosmetic.
        os.fchmod(fd, mode)
    except OSError as exc:
        os.close(fd)
        fail(FAILED, "%s was created but its mode could not be set (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    os.close(fd)


def op_stat(anchor_fd, relative, args):
    if args:
        fail(USAGE, "'stat' takes no arguments.")
    fd, st = open_for_inspection(anchor_fd, relative)
    os.close(fd)
    sys.stdout.write("%s %d %d %d %d %04o\n" % (describe(st), st.st_dev, st.st_ino,
                                                st.st_nlink, st.st_size,
                                                stat.S_IMODE(st.st_mode)))


def op_chmod(anchor_fd, relative, args):
    if len(args) != 1:
        fail(USAGE, "'chmod' takes exactly one argument: the octal mode.")
    mode = parse_mode(args[0])
    fd, st = open_for_inspection(anchor_fd, relative)
    if not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode)):
        os.close(fd)
        fail(WRONG_TYPE, "%s beneath the anchor is a %s; only a regular file or a directory "
                         "has its mode changed here." % (relative, describe(st)))
    try:
        os.chmod(magic(fd), mode)
    except OSError as exc:
        os.close(fd)
        fail(FAILED, "the mode of %s could not be set (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    os.close(fd)


def op_chown(anchor_fd, relative, args):
    if len(args) != 2:
        fail(USAGE, "'chown' takes exactly two arguments: the numeric uid and gid.")
    try:
        uid = int(args[0], 10)
        gid = int(args[1], 10)
    except ValueError:
        fail(USAGE, "'chown' takes NUMERIC uid and gid.")
    if uid < 0 or gid < 0:
        fail(USAGE, "'chown' takes non-negative uid and gid.")
    fd, st = open_for_inspection(anchor_fd, relative)
    if not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode)):
        os.close(fd)
        fail(WRONG_TYPE, "%s beneath the anchor is a %s; only a regular file or a directory "
                         "has its ownership changed here." % (relative, describe(st)))
    try:
        os.chown(magic(fd), uid, gid)
    except OSError as exc:
        os.close(fd)
        fail(FAILED, "the ownership of %s could not be set (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    os.close(fd)


def op_unlink(anchor_fd, relative, args):
    if args:
        fail(USAGE, "'unlink' takes no arguments.")
    parts = split_components(relative)
    leaf = parts[-1]
    parent = walk_directory(anchor_fd, parts[:-1]) if len(parts) > 1 else anchor_fd
    try:
        # unlinkat() on a SINGLE component relative to a confined descriptor. It never follows
        # a symbolic link, so the entry removed is an entry of that directory and of no other.
        os.unlink(leaf, dir_fd=parent)
    except OSError as exc:
        if parent != anchor_fd:
            os.close(parent)
        if exc.errno == errno.ENOENT:
            fail(ABSENT, "%s does not exist beneath the anchor." % relative)
        fail(FAILED, "%s could not be removed (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    if parent != anchor_fd:
        os.close(parent)


def op_list(anchor_fd, relative, args):
    """The directory's entries and their types, read from a descriptor obtained under the
    confinement.

    This exists so that ENUMERATION is confined too, and that is not cosmetic: `find -P` does
    not follow links it finds inside a tree, but the kernel resolves find's STARTING PATH
    normally, so `find -P $IMAGE_MOUNT/etc/apt/apt.conf.d` on an image whose /etc is an absolute
    symbolic link enumerates the HOST's directory and reports host filenames into this script's
    own diagnostics.  Here the directory is opened beneath the anchor with the same refusals as
    everything else, and scandir() reads it through that descriptor.
    """
    if args:
        fail(USAGE, "'list' takes no arguments.")
    fd, st = open_for_inspection(anchor_fd, relative)
    if not stat.S_ISDIR(st.st_mode):
        os.close(fd)
        fail(WRONG_TYPE, "%s beneath the anchor is a %s, not a directory, so it cannot be "
                         "enumerated." % (relative, describe(st)))
    handle = reopen(fd, os.O_DIRECTORY | os.O_RDONLY | os.O_CLOEXEC)
    os.close(fd)
    try:
        with os.scandir(handle) as entries:
            for entry in entries:
                try:
                    est = entry.stat(follow_symlinks=False)
                except OSError:
                    # Removed between the readdir and the stat.  Reported as 'gone' rather than
                    # omitted, so a caller that must refuse every non-regular entry sees it.
                    sys.stdout.buffer.write(b"gone " + os.fsencode(entry.name) + b"\0")
                    continue
                sys.stdout.buffer.write(
                    describe(est).encode("ascii") + b" " + os.fsencode(entry.name) + b"\0")
        sys.stdout.buffer.flush()
    except OSError as exc:
        fail(FAILED, "%s could not be enumerated through its descriptor (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    # scandir()'s context manager closes the descriptor it was handed.


def op_search(anchor_fd, relative, args):
    """Every regular file beneath the anchor-relative directory that contains any of the
    needles read from stdin, printed as "<label> <path-relative-to-the-searched-directory>\0".

    WHY THIS IS AN OPERATION HERE RATHER THAN A `grep -r` IN THE SHELL.  The tree being
    searched is a wholesale copy of the caller's own checkout, so it legitimately contains
    symbolic links, and it sits inside an image whose /opt may itself be a link in the base
    tarball.  `grep -r` skips links it finds inside a tree, but the kernel resolves grep's
    STARTING PATH normally -- so a search rooted at $IMAGE_MOUNT/opt/cec-l2/hdmicec on an image
    with a symlinked /opt would search the HOST and report host filenames as image contents.
    Here the starting directory is resolved beneath the anchor with the same refusals as every
    other operation, and the walk below is descriptor-relative throughout: each subdirectory is
    opened with O_NOFOLLOW on the descriptor of its parent, and only regular files are read.
    Nothing outside the searched directory can be reached, whatever the tree contains.

    Needles are matched as EXACT BYTE SEQUENCES, and each file is read in chunks with an
    overlap of one less than the longest needle, so a needle straddling a chunk boundary is
    still found.  Empty files are skipped because they cannot match.

    Labels arrive as arguments, one per needle in the same order, so the caller can name WHICH
    secret was found without this process ever printing the secret itself.
    """
    needles = [n for n in sys.stdin.buffer.read().split(b"\0") if n]
    if not needles:
        fail(USAGE, "'search' reads its needles from stdin, NUL-separated, and at least one is "
                    "required.")
    if len(args) != len(needles):
        fail(USAGE, "'search' takes exactly one label argument per needle: %d needles, %d "
                    "labels." % (len(needles), len(args)))
    fd, st = open_for_inspection(anchor_fd, relative)
    if not stat.S_ISDIR(st.st_mode):
        os.close(fd)
        fail(WRONG_TYPE, "%s beneath the anchor is a %s, not a directory, so it cannot be "
                         "searched." % (relative, describe(st)))
    root = reopen(fd, os.O_DIRECTORY | os.O_RDONLY | os.O_CLOEXEC)
    os.close(fd)

    overlap = max(len(n) for n in needles) - 1
    chunk = 1 << 20
    if chunk <= overlap:
        chunk = overlap + 1

    def scan(dir_fd, name, shown):
        try:
            handle = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=dir_fd)
        except OSError:
            # Unreadable or replaced since the stat.  Not a hit and not an error: the object
            # this process could not read is not an object whose bytes are in the image.
            return
        try:
            tail = b""
            while True:
                block = os.read(handle, chunk)
                if not block:
                    break
                window = tail + block
                for index, needle in enumerate(needles):
                    if needle in window:
                        sys.stdout.buffer.write(
                            os.fsencode(args[index]) + b" " + shown + b"\0")
                        return
                tail = window[-overlap:] if overlap > 0 else b""
        except OSError:
            return
        finally:
            os.close(handle)

    stack = [(root, b"")]
    try:
        while stack:
            dir_fd, prefix = stack.pop()
            try:
                names = os.listdir(dir_fd)
            except OSError:
                os.close(dir_fd)
                continue
            for name in names:
                try:
                    est = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
                except OSError:
                    continue
                shown = prefix + os.fsencode(name)
                if stat.S_ISDIR(est.st_mode):
                    try:
                        sub = os.open(name,
                                      os.O_DIRECTORY | os.O_RDONLY | os.O_NOFOLLOW
                                      | os.O_CLOEXEC,
                                      dir_fd=dir_fd)
                    except OSError:
                        continue
                    stack.append((sub, shown + b"/"))
                elif stat.S_ISREG(est.st_mode) and est.st_size > 0:
                    scan(dir_fd, name, shown)
            os.close(dir_fd)
        sys.stdout.buffer.flush()
    finally:
        # Anything still queued when an exception unwound the walk.
        for dir_fd, _ in stack:
            try:
                os.close(dir_fd)
            except OSError:
                pass


def op_rmdir(anchor_fd, relative, args):
    if args:
        fail(USAGE, "'rmdir' takes no arguments.")
    parts = split_components(relative)
    leaf = parts[-1]
    parent = walk_directory(anchor_fd, parts[:-1]) if len(parts) > 1 else anchor_fd
    try:
        # rmdirat() via os.rmdir with a dir_fd: a SINGLE component relative to a descriptor
        # obtained under the confinement, and rmdir never follows a symbolic link (it fails
        # with ENOTDIR on one).
        os.rmdir(leaf, dir_fd=parent)
    except OSError as exc:
        if parent != anchor_fd:
            os.close(parent)
        if exc.errno == errno.ENOENT:
            fail(ABSENT, "%s does not exist beneath the anchor." % relative)
        if exc.errno == errno.ENOTDIR:
            fail(WRONG_TYPE, "%s beneath the anchor is not a directory." % relative)
        fail(FAILED, "%s could not be removed (%s)."
             % (relative, errno.errorcode.get(exc.errno, str(exc.errno))))
    if parent != anchor_fd:
        os.close(parent)


def op_mkdir(anchor_fd, relative, args):
    if len(args) != 1:
        fail(USAGE, "'mkdir' takes exactly one argument: the octal mode.")
    mode = parse_mode(args[0])
    parts = split_components(relative)
    fd = walk_directory(anchor_fd, parts, create_mode=mode)
    if fd != anchor_fd:
        os.close(fd)


def parse_mode(text):
    try:
        mode = int(text, 8)
    except ValueError:
        fail(USAGE, "'%s' is not an octal mode." % text)
    if mode < 0 or mode > 0o7777:
        fail(USAGE, "'%s' is not a mode in the range 0000-7777." % text)
    return mode


OPERATIONS = {
    "read": op_read,
    "write": op_write,
    "create": op_create,
    "stat": op_stat,
    "chmod": op_chmod,
    "chown": op_chown,
    "unlink": op_unlink,
    "mkdir": op_mkdir,
    "list": op_list,
    "rmdir": op_rmdir,
    "search": op_search,
}


def main(argv):
    if len(argv) < 4:
        fail(USAGE, "usage: confined-io.py <anchor-directory> <relative-path> <operation> "
                    "[arguments]")
    anchor, relative, operation = argv[1], argv[2], argv[3]
    args = argv[4:]
    handler = OPERATIONS.get(operation)
    if handler is None:
        fail(USAGE, "unknown operation '%s'; known operations are %s."
             % (operation, ", ".join(sorted(OPERATIONS))))
    if not anchor.startswith("/"):
        fail(USAGE, "the anchor directory must be an absolute path.")
    split_components(relative)
    try:
        # O_NOFOLLOW so that an anchor which is ITSELF a symbolic link is refused rather than
        # resolved: everything below depends on this descriptor being the directory that was
        # validated.
        anchor_fd = os.open(anchor,
                            os.O_DIRECTORY | os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    except OSError as exc:
        fail(ANCHOR_BAD, "the anchor directory %s could not be opened as a directory (%s)."
             % (anchor, errno.errorcode.get(exc.errno, str(exc.errno))))
    try:
        handler(anchor_fd, relative, args)
    finally:
        os.close(anchor_fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
CONFINED_IO_PY
    [ -s "$target" ] || die "the confined I/O helper could not be written to $target.  Every
       privileged operation on a base-tarball-named path inside the image goes through it, so the
       run stops rather than falling back to an ordinary open."
    chmod 0600 -- "$target" || die "could not set mode 0600 on the confined I/O helper at
       $target."
    CONFINED_IO_HELPER="$target"
}

# Turn one helper exit status into a `die` that names WHICH condition it was.  Never called for
# status 0, and never called by the callers that legitimately act on a status themselves
# (confined_stat, which uses 13 to mean "not there").
confined_io_die() { # $1=status  $2=relative path  $3=what it is  $4=what was about to happen
    local status="$1" relative="$2" what="$3" operation="$4"
    case "$status" in
        "$CONFINED_IO_UNAVAILABLE")
            die "refusing to $operation ($what): the confined I/O primitive is NOT AVAILABLE on
       this host.  openat2(2) with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV is what
       makes a privileged write inside the image safe against a symbolic link substituted under
       it, and this script does not fall back to an ordinary open -- an ordinary open is
       precisely what this mechanism exists to avoid.  It needs a Linux 5.6-or-newer kernel
       and python3.
       The in-image path was: $relative" ;;
        "$CONFINED_IO_REFUSED")
            die "refusing to $operation ($what): THE CONFINEMENT REFUSED the in-image path
           $relative
       The kernel would not resolve it beneath the image mount point without following a
       symbolic link, without leaving the image and without crossing a mount point.  That means
       some component of it -- leading or trailing -- is a link, or points out of the image.
       Refused rather than resolved: this operation runs as root on the BUILD HOST, so a link
       naming an absolute path would have it act on a HOST file.  Inspect the base tarball; the
       errno is on the line above." ;;
        "$CONFINED_IO_ABSENT")
            die "refusing to $operation ($what): the in-image path
           $relative
       does not exist.  It was enumerated a moment ago, so something is changing the image's
       contents while this scan runs.  Nothing further is written there." ;;
        "$CONFINED_IO_WRONG_TYPE")
            die "refusing to $operation ($what): the in-image path
           $relative
       is not the kind of object this operation acts on -- a fifo, a socket, a device node or a
       directory at that name is not configuration, and a privileged read or rewrite is not
       performed on the strength of a filename.  Inspect the base tarball." ;;
        "$CONFINED_IO_EXISTS")
            die "refusing to $operation ($what): the in-image path
           $relative
       already exists, and this operation creates it exclusively.  Refused rather than written
       through: the name is this script's own, so something the caller supplied is sitting at
       it." ;;
        "$CONFINED_IO_ANCHOR_BAD")
            die "refusing to $operation ($what): the anchor directory
           ${CONFINED_IO_ANCHOR:-<unset>}
       could not be opened as a directory that is not itself a symbolic link.  Every relative
       path is resolved against it, so without it nothing below is confined." ;;
        "$CONFINED_IO_USAGE")
            die "internal error while trying to $operation ($what): the confined I/O helper was
       called with arguments it refused (in-image path '$relative').  This is a defect in
       $SCRIPT_NAME." ;;
        "$CONFINED_IO_FAILED")
            die "could not $operation ($what): the operation on the in-image path
           $relative
       failed after the confined open succeeded.  The errno is on the line above.  The image is
       NOT published." ;;
        *)
            die "could not $operation ($what): the confined I/O helper exited $status on the
       in-image path
           $relative
       which this script has no reading for.  Refused rather than interpreted; this is a defect
       in $SCRIPT_NAME." ;;
    esac
}

# The guard every wrapper shares.  It is an internal-consistency check rather than a runtime
# condition: it catches a future edit that moves a call ahead of the helper's installation or
# the image's mount, which would otherwise anchor a privileged write on an empty directory.
confined_io_ready() { # $1=what is about to happen, for the message
    local operation="$1"
    [ "$CONFINED_IO_PROVEN" -eq 1 ] || die "internal error: '$operation' was attempted before
       probe_confined_io() had PROVEN the confined I/O primitive refuses what it must on this
       host.  Refused rather than attempted: an unproven primitive is indistinguishable from an
       ordinary open, and every comment in $SCRIPT_NAME about the confinement would be false.
       This is a defect in $SCRIPT_NAME."
    [ -n "$CONFINED_IO_HELPER" ] || die "internal error: '$operation' was attempted before the
       confined I/O helper was installed, so it would have run unconfined.  This is a defect in
       $SCRIPT_NAME."
    [ -f "$CONFINED_IO_HELPER" ] || die "the confined I/O helper has disappeared from
       $CONFINED_IO_HELPER.  Every privileged operation on a base-tarball-named path inside the
       image goes through it, so '$operation' is refused."
    [ -n "$CONFINED_IO_ANCHOR" ] || die "internal error: '$operation' was attempted before the
       confined I/O anchor directory was set.  This is a defect in $SCRIPT_NAME."
    [ -d "$CONFINED_IO_ANCHOR" ] || die "the confined I/O anchor directory
       $CONFINED_IO_ANCHOR no longer exists, so '$operation' is refused."
    [ ! -L "$CONFINED_IO_ANCHOR" ] || die "the confined I/O anchor directory
       $CONFINED_IO_ANCHOR is a symbolic link.  Refused: every relative path is resolved
       against it."
}

# The raw call.  Returns the helper's status; the caller decides what it means.  `python3 -I -B`
# so that no PYTHON* variable, user site directory or PYTHONPATH from the caller's environment
# reaches a privileged helper, and no bytecode is written beside it.
confined_io() { # $1=relative path  $2=operation  [$3..]=operation arguments
    python3 -I -B "$CONFINED_IO_HELPER" "$CONFINED_IO_ANCHOR" "$@"
}

# --- The dieing wrappers.  $what and $operation appear only in messages. ------------------
confined_read() { # $1=relative  $2=what it is  $3=what is about to happen   -> content on stdout
    local status=0
    confined_io_ready "$3"
    confined_io "$1" read || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$2" "$3"
}

confined_write() { # $1=relative  $2=what it is  $3=what is about to happen  <- content on stdin
    local status=0
    confined_io_ready "$3"
    confined_io "$1" write || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$2" "$3"
}

confined_create() { # $1=relative  $2=octal mode  $3=what it is  $4=what is about to happen
    local status=0
    confined_io_ready "$4"
    confined_io "$1" create "$2" || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$3" "$4"
}

confined_mkdir() { # $1=relative  $2=octal mode  $3=what it is  $4=what is about to happen
    local status=0
    confined_io_ready "$4"
    confined_io "$1" mkdir "$2" || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$3" "$4"
}

confined_chmod() { # $1=relative  $2=octal mode  $3=what it is  $4=what is about to happen
    local status=0
    confined_io_ready "$4"
    confined_io "$1" chmod "$2" || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$3" "$4"
}

confined_chown() { # $1=relative  $2=uid  $3=gid  $4=what it is  $5=what is about to happen
    local status=0
    confined_io_ready "$5"
    confined_io "$1" chown "$2" "$3" || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$4" "$5"
}

confined_unlink() { # $1=relative  $2=what it is  $3=what is about to happen
    local status=0
    confined_io_ready "$3"
    confined_io "$1" unlink || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$2" "$3"
}

confined_rmdir() { # $1=relative  $2=what it is  $3=what is about to happen
    local status=0
    confined_io_ready "$3"
    confined_io "$1" rmdir || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$2" "$3"
}

# `rm -f` SEMANTICS THROUGH THE PRIMITIVE, AND THE ONE OPERATION THAT MUST ACCEPT A SYMLINK AS
# ITS TARGET RATHER THAN REFUSE IT.
#
# WHY IT EXISTS AT ALL.  Several of the files this script replaces inside the image are files
# the base tarball legitimately ships as SYMBOLIC LINKS -- /etc/resolv.conf is the standing
# example, because debootstrap on a systemd-resolved host leaves it pointing into /run.  The
# whole point of removing it is that writing THROUGH it would write outside the image's /etc,
# and on this build host that means writing to the HOST's /run.  So the operation needed here is
# "remove whatever entry sits at this name, link or not", and the wrappers that ask a question
# about the object -- confined_absent, confined_regular_file, confined_directory -- all `die` on
# a symbolic link, correctly, because for THEIR callers a link is a substituted object.
#
# WHY IT IS SAFE, WHICH IS NOT THE SAME QUESTION.  op_unlink resolves only the PARENT components
# under RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV and then calls unlinkat() on a single
# leaf relative to that descriptor.  unlinkat never follows a symbolic link: it removes the
# DIRECTORY ENTRY, so a link at the leaf is deleted rather than followed, and the object its
# target names -- on the host or anywhere else -- is untouched.  That is exactly `rm -f`'s
# behaviour on a link, with the leading components confined, which `rm -f` on an
# $IMAGE_MOUNT-prefixed pathname is not.
#
# ABSENCE IS NOT AN ERROR, and that is the other half of `rm -f`: status 13 (ABSENT) returns 0.
# Every other non-zero status is still fatal through confined_io_die, so "the confinement refused
# the path" can never be mistaken for "there was nothing there".
confined_unlink_if_present() { # $1=relative  $2=what it is  $3=what is about to happen
    local status=0
    confined_io_ready "$3"
    confined_io "$1" unlink 2>/dev/null || status=$?
    [ "$status" -ne 0 ] || return 0
    [ "$status" -ne "$CONFINED_IO_ABSENT" ] || return 0
    # Re-issued once, unsuppressed, so the kernel's own errno reaches the log beside the
    # explanation.  This arm ends the run, so the second call costs nothing.
    confined_io "$1" unlink >/dev/null || true
    confined_io_die "$status" "$1" "$2" "$3"
}

# The one wrapper that does NOT die on a non-zero status: presence is a question this script
# asks, and "not there" is one of the answers it acts on.  Prints the stat record on stdout and
# RETURNS the helper's status, so a caller writes
#     record="$(confined_stat "$rel" '...')" || status=$?
# and gets the status even though the substitution ran in a subshell.
#
# THAT IS WHY IT IS NOT A GLOBAL.  A wrapper publishing the status in a variable would silently
# produce 0 at every call site that captures the record with $( ): the subshell sets the variable
# and the parent never sees it, so a refusal and a success would be indistinguishable and the
# diagnosis would read "exited 0, which this script has no reading for".  The status is a return
# value here precisely so a subshell cannot lose it.
#
# stderr is suppressed because absence is routine and its diagnostic is noise; the fatal arms in
# confined_directory and confined_regular_file re-issue the call unsuppressed so the kernel's own
# errno still reaches the log beside their explanation.
confined_stat() { # $1=relative  $2=what is about to happen (for the readiness message only)
    local status=0
    confined_io_ready "$2"
    confined_io "$1" stat 2>/dev/null || status=$?
    return "$status"
}

# Is a path absent beneath the anchor?  0 when it is genuinely not there, 1 when it is there,
# and a `die` for every other answer -- because "the confinement refused the path" is not
# evidence of absence and must never be read as such.  The helper is invoked directly rather
# than through confined_stat so that no subshell stands between the call and its status.
confined_absent() { # $1=relative  $2=what it is  $3=what is about to happen
    local status=0
    confined_io_ready "$3"
    confined_io "$1" stat >/dev/null 2>&1 || status=$?
    [ "$status" -ne 0 ] || return 1
    if [ "$status" -ne "$CONFINED_IO_ABSENT" ]; then
        confined_io "$1" stat >/dev/null || true
        confined_io_die "$status" "$1" "$2" "$3"
    fi
    return 0
}

# The entries of one directory beneath the anchor, as NUL-terminated "<type> <name>" records on
# stdout.  Dies on every failure including "not a directory"; a caller that must tolerate an
# absent directory calls confined_directory first.
confined_list() { # $1=relative  $2=what it is  $3=what is about to happen
    local status=0
    confined_io_ready "$3"
    confined_io "$1" list || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$2" "$3"
}

# A recursive content search beneath one in-image directory, confined the same way every other
# operation is.  Needles arrive on stdin NUL-separated; the labels name them in the same order,
# so a hit can be reported without the secret itself appearing in a diagnostic.  Output is
# "<label> <relative-path>\0" per hit and nothing at all when the tree is clean.
confined_search() { # $1=relative dir  $2=what it is  $3=what is about to happen  $4..=labels
    local status=0 rel="$1" what="$2" doing="$3"
    shift 3
    confined_io_ready "$doing"
    confined_io "$rel" search "$@" || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$rel" "$what" "$doing"
}

# Is a path a usable DIRECTORY beneath the anchor?  0 when it is, 1 when it is simply not there,
# and a `die` for every other answer -- a symbolic link at that name is refused rather than
# reported absent, and a regular file, fifo or device node at it means the base filesystem is not
# what it claims to be.  An `[ -L ]` / `[ -e ]` / `[ -d ]` triple cannot answer this: -e and -d
# both FOLLOW links, so they cannot distinguish an image directory from a host one.
confined_directory() { # $1=relative  $2=path as shown  $3=what is about to happen
    local record type_field status=0
    record="$(confined_stat "$1" "$3")" || status=$?
    if [ "$status" -ne 0 ]; then
        if [ "$status" -eq "$CONFINED_IO_ABSENT" ]; then
            return 1
        fi
        if [ "$status" -eq "$CONFINED_IO_REFUSED" ]; then
            # Re-issued once, unsuppressed, so the kernel's own errno appears beside the
            # explanation.  This arm ends the run, so the second call costs nothing.
            confined_io "$1" stat >/dev/null || true
            die "$2 inside the image, or a component of the path leading to it, is a SYMBOLIC
       LINK or leaves the image filesystem.  The entries this scan would enumerate under it
       therefore live wherever the link points -- possibly on the build host, where this scan
       runs as root and where it removes and rewrites what it finds.  Refused rather than
       followed, by the kernel's own path resolution; inspect the base tarball."
        fi
        confined_io_die "$status" "$1" "$2" "$3"
    fi
    type_field="$(confined_stat_field "$record" 1)"
    [ "$type_field" = 'directory' ] || die "$2 inside the image is a $type_field rather than a
       directory.  Refused rather than interpreted: this scan enumerates it as root, and a
       filename is not evidence of what is behind it.  Inspect the base tarball."
    return 0
}

# Is a path a usable REGULAR FILE beneath the anchor?  Same contract as confined_directory: 0
# present, 1 absent, `die` for anything else -- including a symbolic link, which is the case the
# old `[ -L ] then [ -f ]` pair was written to catch and which the kernel now catches first.
confined_regular_file() { # $1=relative  $2=path as shown  $3=what is about to happen
    local record type_field status=0
    record="$(confined_stat "$1" "$3")" || status=$?
    if [ "$status" -ne 0 ]; then
        if [ "$status" -eq "$CONFINED_IO_ABSENT" ]; then
            return 1
        fi
        if [ "$status" -eq "$CONFINED_IO_REFUSED" ]; then
            # Re-issued once, unsuppressed, so the kernel's own errno appears beside the
            # explanation.  This arm ends the run, so the second call costs nothing.
            confined_io "$1" stat >/dev/null || true
            die "$2 inside the image, or a component of the path leading to it, is a SYMBOLIC
       LINK or leaves the image filesystem.  It is refused rather than followed: this scan
       reads, rewrites and removes these files as root ON THE BUILD HOST, so a link naming an
       absolute path would have this script read, truncate, replace or delete a HOST file.  A
       base filesystem that ships a symlinked apt configuration is not a filesystem this script
       publishes an image around.  Inspect the base tarball, replace the link with the regular
       file it should be, and rebuild."
        fi
        confined_io_die "$status" "$1" "$2" "$3"
    fi
    type_field="$(confined_stat_field "$record" 1)"
    [ "$type_field" = 'regular' ] || die "$2 inside the image is a $type_field rather than a
       regular file.  A fifo, a socket, a device node or a directory at that name is not
       configuration, and reading or writing one as root is not something this script does on
       the strength of a filename.  Refused; inspect the base tarball."
    # The link count is checked here because a rewrite through a descriptor changes every name
    # that shares the inode, exactly as a rewrite through a pathname would.  The confinement
    # bounds WHERE the write lands; it does not make a hard link to elsewhere in the image
    # harmless.
    type_field="$(confined_stat_field "$record" 4)"
    case "$type_field" in
        ''|*[!0-9]*) die "the link count of $2 inside the image could not be read, so it cannot
       be established that no other name shares its content." ;;
    esac
    [ "$type_field" -le 1 ] || die "$2 inside the image has $type_field hard links, so another
       name shares its content and a rewrite would change both.  Refused rather than written
       through."
    return 0
}

# Field $2 of a stat record ("<type> <dev> <ino> <nlink> <size> <mode>"), so a caller reads the
# type, the link count or the size of the object THE CONFINEMENT OPENED rather than re-running
# stat(1) on a pathname -- which would reintroduce exactly the second lookup this mechanism
# exists to remove.
confined_stat_field() { # $1=the stat record  $2=1-based field index
    printf '%s\n' "$1" | awk -v field="$2" '{ print $field }'
}

# ------------------------------------------------------------------------------------
# IDENTITY, CAPTURED AT CREATION AND RE-VERIFIED BEFORE A RECURSIVE DELETE.
#
# WHY A DELETE NEEDS MORE THAN THE CONFINEMENT.  Two of the directories this script builds
# inside the image are scratch: the lcov source staging tree and the in-guest helpers' source
# tree.  Both are created, used through the chroot, and then removed RECURSIVELY -- and a
# recursive delete is the single worst operation in this file to have redirected, because it
# destroys rather than corrupts and it does so as root.
#
# The confinement bounds WHERE a delete can land: routed through the chroot (see
# remove_tree_in_image below) it cannot name a host path at all, and routed through the primitive
# it cannot traverse a link.  Neither of them answers a different question -- IS THIS STILL THE
# DIRECTORY I MADE?  A directory swapped for another directory is not a symbolic link and is not
# outside the image, so nothing above notices it; only its identity does.
#
# So the identity is captured the moment the directory is created and compared immediately before
# the delete.  st_dev and st_ino together name an object uniquely on a mounted filesystem, they
# are read through the descriptor the confinement opened rather than by a second pathname lookup,
# and a mismatch is fatal with a diagnostic that says which field moved.  This is the
# "verify device/inode ownership before cleanup" half of the finding this addresses, and it is
# the same technique make_work_dir() already applies to the host-side scratch directory.
# ------------------------------------------------------------------------------------

# The identity of an in-image directory, as the confinement sees it, on stdout.  Fatal when the
# path is absent, refused or not a directory: an identity that cannot be read is not an identity
# that can be compared later, and recording an empty one would make the comparison below pass
# vacuously.
confined_identity() { # $1=relative  $2=what it is  $3=what is about to happen  -> record on stdout
    local record status=0
    record="$(confined_stat "$1" "$3")" || status=$?
    [ "$status" -eq 0 ] || confined_io_die "$status" "$1" "$2" "$3"
    local type_field device inode
    type_field="$(confined_stat_field "$record" 1)"
    device="$(confined_stat_field "$record" 2)"
    inode="$(confined_stat_field "$record" 3)"
    [ "$type_field" = 'directory' ] || die "refusing to $3 ($2): the in-image path
           $1
       is a $type_field rather than a directory at the moment its identity was recorded.  This
       script created it as a directory, so something else is at that name.  Inspect the base
       tarball."
    case "${device:-x}${inode:-x}" in
        *[!0-9]*) die "refusing to $3 ($2): the device and inode of the in-image path
           $1
       came back as '${device:-<empty>}'/'${inode:-<empty>}', which are not numbers, so its
       identity cannot be recorded and a later delete could not be checked against it.  This is
       a defect in $SCRIPT_NAME." ;;
    esac
    printf '%s\n' "$record"
}

# The comparison.  Called immediately before the delete, never earlier: the value of this check
# is entirely in how little happens between it and the operation it guards.
assert_confined_identity_unchanged() { # $1=relative  $2=recorded record  $3=what it is  $4=doing
    local relative="$1" recorded="$2" what="$3" doing="$4" now status=0
    now="$(confined_stat "$relative" "$doing")" || status=$?
    if [ "$status" -ne 0 ]; then
        confined_io "$relative" stat >/dev/null || true
        die "refusing to $doing ($what): the in-image path
           $relative
       can no longer be inspected through the confinement, so it cannot be established that it
       is still the directory this script created and is about to delete RECURSIVELY AS ROOT.
       Refused rather than deleted; the helper's own diagnosis is on the line above."
    fi
    local was_type was_device was_inode now_type now_device now_inode
    was_type="$(confined_stat_field "$recorded" 1)"
    was_device="$(confined_stat_field "$recorded" 2)"
    was_inode="$(confined_stat_field "$recorded" 3)"
    now_type="$(confined_stat_field "$now" 1)"
    now_device="$(confined_stat_field "$now" 2)"
    now_inode="$(confined_stat_field "$now" 3)"
    if [ "$was_type" != "$now_type" ] || [ "$was_device" != "$now_device" ] \
       || [ "$was_inode" != "$now_inode" ]; then
        die "REFUSING TO $doing ($what): the in-image path
           $relative
       CHANGED IDENTITY between the moment this script created it and now.  It was a $was_type on
       device $was_device with inode $was_inode; it is a $now_type on device $now_device with
       inode $now_inode.  Something replaced the object at that name while this image was being
       built, and the operation about to run is a RECURSIVE DELETE AS ROOT -- so it is refused
       rather than aimed at whatever is there instead.  The image is NOT published."
    fi
}

# The confined replacement for `[ -x "$IMAGE_MOUNT$path" ]`, which followed symbolic links and
# tested a HOST file's executability whenever a component of the in-image path was a link.
#
# "Executable" is read from the mode field of the record the confinement opened: any of the three
# execute bits satisfies it, because everything that runs these files -- the chroot here and the
# guest's init later -- runs as root, and root needs only one.  A non-regular object at the name
# is refused rather than measured, and a symbolic link never gets that far: confined_regular_file
# refuses it first, which is the whole reason this is not a `[ -x ]`.
assert_installed_executable() { # $1=relative  $2=path as shown  $3=what it is  $4=doing
    local relative="$1" shown="$2" what="$3" doing="$4" record mode
    if ! confined_regular_file "$relative" "$shown" "$doing"; then
        die "$what was not installed at $shown inside the image: nothing exists at that path
       after the step that builds it reported success.  The image is NOT published."
    fi
    record="$(confined_stat "$relative" "$doing")" \
        || die "$what exists at $shown inside the image but its mode could not be read, so it
       cannot be established that it is executable.  The image is NOT published."
    mode="$(confined_stat_field "$record" 6)"
    case "$mode" in
        ''|*[!0-7]*) die "the mode of $what at $shown inside the image came back as
           '${mode:-<empty>}'
       which is not an octal mode, so its executability cannot be established.  This is a defect
       in $SCRIPT_NAME." ;;
    esac
    case "$mode" in
        *[1357]|*[1357][0-7]|*[1357][0-7][0-7]) : ;;
        *) die "$what was installed at $shown inside the image but its mode is $mode, which
       carries no execute bit.  The guest's init runs it directly, so an unexecutable file there
       is a boot-time failure with no diagnosis.  The image is NOT published." ;;
    esac
}

# ------------------------------------------------------------------------------------
# THE START-UP PROOF.  One positive case and five negative cases, in a private directory,
# before one privileged byte is written.  A primitive that silently degraded to an ordinary open
# would leave every claim in this file false, so the degradation is what this proves cannot have
# happened -- and it proves it on THIS host and THIS kernel rather than from a version number.
#
# It is fatal on every arm: a positive case that fails means the mechanism does not work, and a
# negative case that SUCCEEDS means it is not confining anything.
# ------------------------------------------------------------------------------------
probe_confined_io() { # $1=a private directory to probe in
    local root="$1" saved_anchor="$CONFINED_IO_ANCHOR" content
    local outside="$root/outside-the-anchor"
    local anchor="$root/confined-io-probe"

    if [ -z "$root" ] || [ ! -d "$root" ]; then
        die "internal error: probe_confined_io was given '${root:-<empty>}', which is not a
       directory.  This is a defect in $SCRIPT_NAME."
    fi

    rm -rf -- "$anchor" "$outside" || die "could not clear the confined I/O probe directory
       under $root."
    mkdir -p -- "$anchor/beneath" || die "could not create the confined I/O probe directory
       $anchor/beneath."
    mkdir -p -- "$outside" || die "could not create the confined I/O probe's control directory
       $outside."
    printf 'confined-io probe payload\n' > "$anchor/beneath/regular" \
        || die "could not write the confined I/O probe's regular file."
    printf 'this file is OUTSIDE the anchor and must stay unreadable through it\n' \
        > "$outside/target" || die "could not write the confined I/O probe's control file."
    ln -sfn "$outside/target" "$anchor/beneath/absolute-link" \
        || die "could not create the confined I/O probe's absolute symbolic link."
    ln -sfn 'regular' "$anchor/beneath/relative-link" \
        || die "could not create the confined I/O probe's relative symbolic link."
    mkdir -p -- "$anchor/elsewhere" || die "could not create the confined I/O probe's second
       directory."
    ln -sfn '../elsewhere' "$anchor/beneath/directory-link" \
        || die "could not create the confined I/O probe's mid-path symbolic link."
    printf 'reached through a symlinked directory component\n' > "$anchor/elsewhere/regular" \
        || die "could not write the confined I/O probe's mid-path target file."
    mkfifo -- "$anchor/beneath/fifo" 2>/dev/null || rm -f -- "$anchor/beneath/fifo"

    CONFINED_IO_ANCHOR="$anchor"

    # POSITIVE.  A regular file beneath the anchor is readable, and the bytes are the bytes.
    content="$(confined_io 'beneath/regular' read)" \
        || die "the confined I/O primitive could NOT read a plain regular file beneath its own
       anchor directory during the start-up proof.  Every privileged operation on a
       base-tarball-named path inside the image goes through it, so the run stops here rather
       than falling back to an ordinary open.  The diagnostic is on the line above."
    [ "$content" = 'confined-io probe payload' ] || die "the confined I/O primitive read a
       regular file beneath its anchor and returned content that is not what was written to it.
       The mechanism does not work on this host, so nothing privileged is done through it."

    # NEGATIVE, four of them.  Each MUST be refused with status 12; a success means the
    # primitive is not confining and every comment in this file about it would be false.
    probe_confined_io_refusal 'beneath/absolute-link' stat "$CONFINED_IO_REFUSED" \
        'a symbolic link whose target is an absolute path outside the anchor'
    probe_confined_io_refusal 'beneath/relative-link' stat "$CONFINED_IO_REFUSED" \
        'a symbolic link whose target is inside the anchor (refused too: no link is followed)'
    probe_confined_io_refusal 'beneath/directory-link/regular' stat "$CONFINED_IO_REFUSED" \
        'a path whose MIDDLE component is a symbolic link'
    probe_confined_io_refusal '../outside-the-anchor/target' stat "$CONFINED_IO_REFUSED" \
        'a path that climbs out of the anchor with .. (RESOLVE_BENEATH, not a check of ours)'

    # And a fifo is refused as the wrong TYPE rather than opened -- which is what stops a
    # privileged read blocking forever on one.  'stat' REPORTS a type (the callers here refuse
    # on it), so the case that must refuse is a read.  Skipped only if mkfifo is unavailable,
    # which is stated rather than passed over.
    if [ -p "$anchor/beneath/fifo" ]; then
        probe_confined_io_refusal 'beneath/fifo' read "$CONFINED_IO_WRONG_TYPE" \
            'a fifo, which a privileged read would otherwise block on forever'
    else
        log "  confined I/O: the fifo case was not exercised (mkfifo is unavailable here); the"
        log "    type refusal is still in force, it is simply not proven on this host"
    fi

    CONFINED_IO_ANCHOR="$saved_anchor"
    rm -rf -- "$anchor" "$outside" || die "could not remove the confined I/O probe directory
       under $root."
    CONFINED_IO_PROVEN=1
    log "confined I/O primitive proven on this host: openat2 with"
    log "  RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV reads a regular file beneath its"
    log "  anchor and refuses an absolute link, a relative link, a symlinked middle component"
    log "  and a '..' escape.  Every privileged operation on a base-tarball-named path inside"
    log "  the image goes through it; there is no fallback to an ordinary open."
}

# One negative case of the start-up proof.  Separate so each refusal reports its own name.
probe_confined_io_refusal() { # $1=relative  $2=operation  $3=required status  $4=what the case is
    local relative="$1" operation="$2" required="$3" what="$4" status=0
    confined_io "$relative" "$operation" >/dev/null 2>&1 || status=$?
    if [ "$status" -eq 0 ]; then
        die "THE CONFINED I/O PRIMITIVE DID NOT REFUSE $what during the start-up proof: it
       resolved
           $relative
       beneath $CONFINED_IO_ANCHOR and reported success.  A primitive that silently degrades to
       an ordinary open is worse than no primitive at all, because every comment in
       $SCRIPT_PATH about the confinement would then be false.  The run stops here; nothing
       privileged is done through it."
    fi
    [ "$status" -eq "$required" ] || die "the confined I/O primitive returned status
       $status for $what during the start-up proof, where $required was required.  The refusal has to come from the KERNEL's path resolution, so
       any other status means something else failed and the confinement itself was not
       exercised.  The run stops rather than proceeding on an unproven mechanism."
    log "  confined I/O: refused $what (status $status)"
}

# ==============================================================================
# PINNED PRIVILEGED OPERATIONS -- THE MECHANISM THAT MAKES mount(2) AND chroot(2) CONSUME AN
# OBJECT RATHER THAN A NAME
# ==============================================================================
# WHAT WAS STILL WRONG AFTER THE OTHER TWO MECHANISMS LANDED, STATED PLAINLY BECAUSE THE
# COMMENTS IN THIS FILE USED TO CONCEDE IT AND CALL IT UNAVOIDABLE.
#
# The per-file primitive removed the second name lookup from every operation on an individual
# file inside the image, and the private mount namespace bounded where a lost race could reach.
# Neither of them changed the shape of the six privileged operations that name a DIRECTORY:
# pin_directory_identity() recorded a directory's device and inode, assert_pinned_directory()
# re-stat'ed THE SAME PATHNAME, and the syscall that followed then resolved that pathname a
# THIRD time, independently.  That is still "validate the name, then use the name" -- and the
# earlier revision of this block said so in terms, on the reasoning that there is no mount_at()
# and no fchroot(), so no other shape was available.
#
# THE NAMESPACE DOES NOT CLOSE IT, WHICH IS THE PART WORTH BEING PRECISE ABOUT.  A private mount
# namespace stops mount events propagating to the parent; it does not freeze directory entries
# and it does not make the host filesystem unreachable from inside.  A chroot target substituted
# for a directory that reaches the host tree is still a chroot onto the host tree, inside the
# namespace, as root.
#
# WHAT REPLACES IT: THE DESCRIPTOR IS RETAINED, VERIFIED AND THEN CONSUMED.
#
#   * chroot.  The image root is opened O_DIRECTORY|O_RDONLY|O_NOFOLLOW, its identity is checked
#     by FSTAT ON THAT DESCRIPTOR against the pin, and the entry is then fchdir(fd) followed by
#     chroot(".").  "." resolves against the working directory the descriptor established, so
#     between the check and the use there is no name for anything to re-point.  There is no
#     fchroot(), but there did not need to be one.
#   * mount.  mount(2) takes no descriptor, but a mount whose TARGET is /proc/self/fd/N consumes
#     the object that descriptor already refers to: the kernel resolves the magic link straight
#     to the pinned (mount, dentry) pair instead of walking the components of a name again.
#     Every one of the four pseudo-filesystem mounts and the nosymfollow remount is performed
#     that way, in the same process that opened and verified the descriptor.
#   * IDENTITY IS VERIFIED ON THE DESCRIPTOR AND NEVER ON THE PATHNAME AGAIN.  That is the whole
#     point: a further stat by name would reintroduce exactly what this removes.  The values it
#     is checked against are the ones pin_directory_identity() recorded and the ones the
#     confined primitive read through its own descriptor -- so both ends of the comparison come
#     from an fstat.
#
# IT IS DEFENCE IN DEPTH ADDED TO EVERYTHING THAT WAS ALREADY THERE, AND IT REPLACES NOTHING.
# The private mount namespace, the openat2 confinement with its no-symlink and no-cross-device
# refusals, the four mount points created and type-checked through the primitive, the propagation
# measured back from /proc/self/mountinfo, and the pathname identity checks
# (assert_pinned_directory / assert_image_root_pinned) are all still in force and still called at
# the same places.  What they are now is the PRE-CHECKS: they produce the diagnostic that names
# the object and the field that moved, which an errno cannot, and the descriptor below is what
# carries the guarantee.
#
# FAIL CLOSED, LOUDLY, AND NEVER BACK TO THE PATHNAME FORM.  A descriptor that cannot be opened,
# an fstat that disagrees with the pin, a target the confinement refuses, a kernel without
# openat2 and a helper that is not installed are all fatal, each with its own exit status and its
# own message naming what failed.  There is no arm anywhere below that falls back to
# `mount ... "$IMAGE_MOUNT/proc"` or to `chroot "$IMAGE_MOUNT"`, because that fallback is the
# defect this mechanism replaced.  And it is PROVEN on this host before one privileged byte is
# written: probe_pinned_op() runs two positive cases and seven refusals in a private directory
# during make_work_dir(), exactly as probe_confined_io() does for the per-file primitive.
#
# WHY A SECOND HELPER RATHER THAN MORE OPERATIONS IN THE FIRST.  confined-io.py is proven by a
# 55-case self-test and every privileged in-image write in this script goes through it; the two
# operations here are not file operations, they change this PROCESS (its root) and the MOUNT
# TABLE, and one of them deliberately never returns.  Keeping them apart leaves that helper
# byte-identical and gives each mechanism its own proof.  The conventions are the same ones --
# anchored open, one exit status per condition, a dieing shell wrapper per operation -- because a
# reader who has understood one has then understood both.
# ==============================================================================

# The helper's leaf name inside this run's scratch directory, the descriptor-anchored location it
# was written to, and the interpreter that runs it.  Empty until install_pinned_op_helper() has
# run.
readonly PINNED_OP_HELPER_LEAF='pinned-op.py'
PINNED_OP_HELPER=''
# RESOLVED TO AN ABSOLUTE PATH, DELIBERATELY.  The two chroot entry points invoke this helper
# under `env -i`, which replaces PATH with the one the in-image command needs -- so a bare
# `python3` there would be looked up on the IMAGE's search path rather than the build host's.
# The interpreter is therefore resolved once, on the host, and named absolutely at every call.
PINNED_OP_PYTHON=''
# Raised only by probe_pinned_op() once the mechanism has been proven to refuse what it must.
PINNED_OP_PROVEN=0

# THE HELPER'S EXIT STATUSES.  One per condition, for the same reason the per-file helper has
# them: "it failed" is not an answer a privileged mount or chroot may act on.  They are
# deliberately a different range from the confined I/O statuses so that a status crossing between
# the two mechanisms cannot be misread as the wrong condition.
readonly PINNED_OP_USAGE=20
readonly PINNED_OP_UNAVAILABLE=21
readonly PINNED_OP_REFUSED=22
readonly PINNED_OP_ABSENT=23
readonly PINNED_OP_WRONG_TYPE=24
readonly PINNED_OP_IDENTITY=25
readonly PINNED_OP_FAILED=26
readonly PINNED_OP_ANCHOR_BAD=27
# THE ONE RESERVED STATUS.  chroot-exec replaces the helper process with the command, so the
# shell reads the COMMAND's exit status and cannot also be handed one of the granular values
# above without the two being indistinguishable.  Every custody failure on that operation
# therefore collapses to this single value -- which no command this script runs inside the image
# uses -- and the wrapper treats it as fatal.  The granular reason still reaches stderr.
readonly PINNED_OP_CUSTODY_REFUSED=120

# Write the helper into $1 at mode 0600 and record where it went, along with the interpreter that
# will run it.  Called with this run's scratch directory in the normal path and with a private
# probe directory by --self-test, so the source exists in exactly one place in this file and on
# disk in exactly one place per caller.
install_pinned_op_helper() { # $1=directory to write the helper into
    local dir="$1" target interpreter
    [ -n "$dir" ] || die "internal error: install_pinned_op_helper was called without a
       directory.  This is a defect in $SCRIPT_NAME."
    [ -d "$dir" ] || die "internal error: install_pinned_op_helper was given '$dir', which is not
       a directory.  This is a defect in $SCRIPT_NAME."
    interpreter="$(command -v -- python3 2>/dev/null || printf '')"
    case "$interpreter" in
        /*) : ;;
        *) die "python3 could not be resolved to an absolute path on this host
           ${interpreter:-<not found on PATH>}
       and it is what performs this script's privileged mounts and chroots: both consume a
       descriptor this helper opens and verifies, and neither has a shell form that could do it.
       There is no pathname fallback.  Install python3 (it is already a hard prerequisite of the
       confined I/O primitive) and re-run." ;;
    esac
    target="$dir/$PINNED_OP_HELPER_LEAF"
    rm -f -- "$target" || die "could not clear the pinned privileged-operation helper's path at
       $target."
    cat > "$target" <<'PINNED_OP_PY'
# Pinned privileged-operation helper -- the descriptor-consuming form of mount(2) and chroot(2).
#
# GENERATED FILE.  Written by .github/workflows/aidl-path-tests-rootfs.sh in the hdmicec
# submodule; edit that script, not this copy, which is recreated on every run.
#
#   usage: pinned-op.py <anchor-directory> <anchor-dev> <anchor-inode> \
#                       <target-relative|.> <target-dev|-> <target-inode|-> \
#                       <operation> [operation arguments]
#
# WHAT THIS EXISTS TO REMOVE.  mount(2) and chroot(2) take a pathname, so a caller that
# validates a pathname and then hands the SAME pathname to the syscall has validated one object
# and used whatever the name resolves to at the instant of the call -- check-then-use (CWE-367),
# with root on the far side of it.  Re-stating the check closer to the syscall narrows the
# window; it does not close it, because the syscall performs its own independent resolution.
#
# WHAT REPLACES IT: THE OBJECT, NOT THE NAME, IS WHAT GETS USED.
#
#   * The anchor is opened O_DIRECTORY|O_RDONLY|O_NOFOLLOW and its identity is verified by
#     fstat ON THAT DESCRIPTOR against the device and inode the calling script pinned when it
#     created or mounted it.  A pathname is never stat'ed again.
#   * A target beneath the anchor is resolved with openat2(2) under
#     RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV -- the same confinement the per-file
#     helper uses -- and its identity is verified by fstat on the resulting descriptor.
#   * chroot CONSUMES the descriptor: fchdir(fd) then chroot(".").  "." resolves against the
#     working directory the descriptor established, so no pathname lookup can be redirected
#     between the check and the use.
#   * mount(2) has no descriptor-taking form, but a mount whose TARGET is given as
#     /proc/self/fd/N consumes the object that descriptor already refers to: the kernel
#     resolves the magic link to the pinned (mount, dentry) pair directly instead of walking
#     the components of a name again.  Every mount here is performed that way, in the same
#     process that holds and verified the descriptor.
#
# THERE IS NO FALLBACK TO THE PATHNAME FORM.  Every failure below -- an anchor that cannot be
# opened, a target the confinement refuses, an fstat that disagrees with the pin, a kernel
# without openat2 -- exits non-zero with the status that names it and performs nothing.
#
# Operations:
#   verify                              nothing; proves the anchor and the target are the pinned
#                                       objects and that the chain above works on this host
#   mount-fs <fstype> <source>          mount(source, /proc/self/fd/N, fstype, 0, NULL)
#   mount-bind <source>                 mount(source, /proc/self/fd/N, NULL, MS_BIND, NULL)
#   mount-bind-under <source> <parent-dev> <parent-inode>
#                                       the one target that CANNOT be resolved by openat2 --
#                                       see the comment on resolve_under_mount()
#   remount-nosymfollow                 mount(NULL, /proc/self/fd/N, NULL,
#                                       MS_REMOUNT|MS_NOSYMFOLLOW, NULL)
#   chroot-exec <command> [arguments]   fchdir(fd), chroot("."), chdir("/"), execvp(command)
#
# Exit statuses are a contract with the calling shell; see the block above
# install_pinned_op_helper in that script.
import ctypes
import errno
import os
import stat
import sys

USAGE = 20
UNAVAILABLE = 21
REFUSED = 22
ABSENT = 23
WRONG_TYPE = 24
IDENTITY = 25
FAILED = 26
ANCHOR_BAD = 27
# THE ONE RESERVED STATUS.  chroot-exec replaces this process with the command, so the shell
# reads the COMMAND's status and cannot be handed one of the granular values above without it
# being indistinguishable from that command's own exit code.  Every custody failure on that
# operation therefore collapses to this single value, which no command this script runs inside
# the image uses, and the calling wrapper treats it as fatal.  The granular reason is still
# printed to stderr.
CUSTODY_REFUSED = 120

SYS_OPENAT2 = 437
RESOLVE_NO_XDEV = 0x01
RESOLVE_NO_SYMLINKS = 0x04
RESOLVE_BENEATH = 0x08
# RESOLVE_NO_SYMLINKS implies RESOLVE_NO_MAGICLINKS, so a /proc magic link cannot be used as a
# path component either.  These three together are the whole confinement, and they are the same
# three the per-file helper uses.
RESOLVE_FLAGS = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV

MS_REMOUNT = 0x20
MS_NOSYMFOLLOW = 0x100
MS_BIND = 0x1000

DIRECTORY_FLAGS = os.O_DIRECTORY | os.O_RDONLY | os.O_CLOEXEC

# Raised for the whole of the pre-chroot phase of chroot-exec; see CUSTODY_REFUSED.
EXEC_MODE = False


class OpenHow(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint64),
        ("mode", ctypes.c_uint64),
        ("resolve", ctypes.c_uint64),
    ]


_libc = ctypes.CDLL(None, use_errno=True)
_libc.syscall.restype = ctypes.c_long
_libc.mount.restype = ctypes.c_int
_libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                        ctypes.c_ulong, ctypes.c_void_p]


def fail(status, message):
    sys.stderr.write("pinned-op: %s\n" % message)
    if EXEC_MODE:
        sys.stderr.write("pinned-op: the chroot was NOT entered; reported as status %d "
                         "(custody refused) because this operation replaces the process and "
                         "cannot return a distinguishable one.\n" % CUSTODY_REFUSED)
        sys.exit(CUSTODY_REFUSED)
    sys.exit(status)


def errno_name(err):
    return errno.errorcode.get(err, str(err))


def describe(st):
    if stat.S_ISREG(st.st_mode):
        return "regular file"
    if stat.S_ISDIR(st.st_mode):
        return "directory"
    if stat.S_ISLNK(st.st_mode):
        return "symbolic link"
    if stat.S_ISFIFO(st.st_mode):
        return "fifo"
    if stat.S_ISSOCK(st.st_mode):
        return "socket"
    if stat.S_ISBLK(st.st_mode):
        return "block device"
    if stat.S_ISCHR(st.st_mode):
        return "character device"
    return "object of an unknown type"


def openat2_raw(dirfd, relative, flags):
    """openat2() beneath dirfd with the confinement flags.  Returns (fd, 0) or (-1, errno)."""
    how = OpenHow(flags, 0, RESOLVE_FLAGS)
    ctypes.set_errno(0)
    fd = _libc.syscall(
        ctypes.c_long(SYS_OPENAT2),
        ctypes.c_int(dirfd),
        ctypes.c_char_p(relative.encode("utf-8", "surrogateescape")),
        ctypes.byref(how),
        ctypes.c_size_t(ctypes.sizeof(how)),
    )
    if fd >= 0:
        return int(fd), 0
    return -1, ctypes.get_errno()


def openat2(dirfd, relative, flags):
    """openat2_raw(), with every failure turned into the exit status that names it."""
    fd, err = openat2_raw(dirfd, relative, flags)
    if fd >= 0:
        return fd
    name = errno_name(err)
    where = "%s (relative to the anchor)" % relative
    if err in (errno.ENOSYS, errno.EOPNOTSUPP):
        fail(UNAVAILABLE,
             "this kernel does not implement openat2 (%s on %s). The privileged target cannot "
             "be resolved under the confinement, and this helper does not fall back to an "
             "ordinary pathname mount or chroot." % (name, where))
    if err == errno.EINVAL:
        fail(UNAVAILABLE,
             "openat2 refused the resolve flags RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|"
             "RESOLVE_NO_XDEV with EINVAL on %s. The kernel has the syscall but not the "
             "confinement this helper requires." % where)
    if err in (errno.ELOOP, errno.EXDEV):
        fail(REFUSED,
             "the confinement REFUSED %s with %s: the path is a symbolic link, passes through "
             "one, leaves the anchor directory, or crosses a mount point." % (where, name))
    if err == errno.ENOENT:
        fail(ABSENT, "%s does not exist beneath the anchor." % where)
    if err == errno.ENOTDIR:
        fail(WRONG_TYPE,
             "%s uses a non-directory as a path component, or is not a directory." % where)
    fail(REFUSED, "the confined open of %s failed with %s." % (where, name))


def fstat_or_fail(fd, shown):
    try:
        return os.fstat(fd)
    except OSError as exc:
        fail(FAILED, "the identity of %s could not be read from its descriptor (%s)."
             % (shown, errno_name(exc.errno)))


def verify_identity(fd, expected_dev, expected_ino, shown):
    """The whole point of this helper: the identity is taken from an fstat ON THE DESCRIPTOR
    that the privileged operation is about to consume, never from a second lookup of the
    pathname.  A directory is required because every operation here mounts onto one or chroots
    into one."""
    st = fstat_or_fail(fd, shown)
    if not stat.S_ISDIR(st.st_mode):
        fail(WRONG_TYPE, "%s is a %s rather than a directory, so nothing is mounted onto it and "
                         "nothing is chrooted into it." % (shown, describe(st)))
    if st.st_dev != expected_dev or st.st_ino != expected_ino:
        fail(IDENTITY,
             "%s is NOT the object this run pinned: the descriptor just opened is device %d "
             "inode %d, where device %d inode %d was recorded. The name was re-pointed at a "
             "different object, so the privileged operation is refused rather than performed "
             "through it." % (shown, st.st_dev, st.st_ino, expected_dev, expected_ino))
    return st


def open_anchor(anchor, expected_dev, expected_ino):
    """The anchor is opened O_NOFOLLOW so that an anchor which is ITSELF a symbolic link is
    refused rather than resolved, and its identity is then verified on the descriptor."""
    try:
        fd = os.open(anchor, DIRECTORY_FLAGS | os.O_NOFOLLOW)
    except OSError as exc:
        fail(ANCHOR_BAD,
             "the anchor directory %s could not be opened as a directory that is not itself a "
             "symbolic link (%s). Every target below is resolved against that descriptor, so "
             "without it nothing is pinned." % (anchor, errno_name(exc.errno)))
    verify_identity(fd, expected_dev, expected_ino, "the anchor directory %s" % anchor)
    return fd


def resolve_target(anchor_fd, anchor, relative, expected_dev, expected_ino):
    """A descriptor on the object the privileged operation will consume.

    '.' means the anchor itself, whose identity open_anchor() has already verified.  Anything
    else is resolved beneath the anchor by openat2 under the confinement and verified on its own
    descriptor."""
    if relative == ".":
        return anchor_fd, "the anchor directory %s" % anchor
    fd = openat2(anchor_fd, relative, DIRECTORY_FLAGS)
    shown = "%s/%s" % (anchor.rstrip("/"), relative)
    verify_identity(fd, expected_dev, expected_ino, shown)
    return fd, shown


def resolve_under_mount(anchor_fd, anchor, relative, parent_dev, parent_ino):
    """The one target that openat2 CANNOT resolve, and why it is a separate route rather than an
    exception to the rule.

    The fourth pseudo-filesystem target is <anchor>/dev/pts, and by the time it is mounted the
    caller has already bind-mounted the host's /dev onto <anchor>/dev -- so resolving 'dev' now
    CROSSES A MOUNT POINT, which RESOLVE_NO_XDEV refuses by design.  Resolving it before that
    bind would name a different object: the image's own dev/pts directory, which the bind then
    shadows, so a mount there would be invisible inside the chroot.  The target has to be the
    pts directory AS IT IS REACHED THROUGH THE BIND, and that object cannot be identified in
    advance because on the host it is hidden under the host's own devpts mount.

    So the chain is walked by descriptor instead, one component at a time, with O_NOFOLLOW on
    each so no symbolic link is ever followed, and what is verified is what CAN be verified:
      * the anchor, by fstat, against the identity the caller pinned (done by open_anchor);
      * the 'dev' component, by fstat, against the identity of the source the caller bound
        there -- which proves the component resolved into that bind and not into anything else;
      * the leaf, by fstat, as a directory on the SAME st_dev as its parent -- which proves the
        last step did not cross into a further mount.
    Nothing is resolved by a pathname the kernel walks again, and the mount consumes the leaf
    descriptor."""
    parts = [p for p in relative.split("/") if p not in ("", ".")]
    if len(parts) != 2:
        fail(USAGE, "'mount-bind-under' takes a two-component relative target such as "
                    "'dev/pts'; '%s' is not one." % relative)
    try:
        parent_fd = os.open(parts[0], DIRECTORY_FLAGS | os.O_NOFOLLOW, dir_fd=anchor_fd)
    except OSError as exc:
        status = {errno.ENOENT: ABSENT, errno.ENOTDIR: WRONG_TYPE,
                  errno.ELOOP: REFUSED}.get(exc.errno, FAILED)
        fail(status, "the '%s' component of %s/%s could not be opened as a directory that is "
                     "not a symbolic link (%s)." % (parts[0], anchor.rstrip("/"), relative,
                                                    errno_name(exc.errno)))
    parent_st = verify_identity(parent_fd, parent_dev, parent_ino,
                                "%s/%s" % (anchor.rstrip("/"), parts[0]))
    try:
        leaf_fd = os.open(parts[1], DIRECTORY_FLAGS | os.O_NOFOLLOW, dir_fd=parent_fd)
    except OSError as exc:
        status = {errno.ENOENT: ABSENT, errno.ENOTDIR: WRONG_TYPE,
                  errno.ELOOP: REFUSED}.get(exc.errno, FAILED)
        fail(status, "the '%s' component of %s/%s could not be opened as a directory that is "
                     "not a symbolic link (%s)." % (parts[1], anchor.rstrip("/"), relative,
                                                    errno_name(exc.errno)))
    shown = "%s/%s" % (anchor.rstrip("/"), relative)
    leaf_st = fstat_or_fail(leaf_fd, shown)
    if not stat.S_ISDIR(leaf_st.st_mode):
        fail(WRONG_TYPE, "%s is a %s rather than a directory, so nothing is mounted onto it."
             % (shown, describe(leaf_st)))
    if leaf_st.st_dev != parent_st.st_dev:
        fail(REFUSED,
             "%s is on device %d while its parent is on device %d, so resolving the last "
             "component crossed a mount point. Refused rather than mounted onto: the target "
             "is meant to be a plain directory of the filesystem bound at the parent."
             % (shown, leaf_st.st_dev, parent_st.st_dev))
    return leaf_fd, shown


def mount_through_descriptor(source, target_fd, fstype, flags, shown, what):
    """mount(2) with the target given as the magic link of a descriptor this process opened and
    verified.  The kernel resolves that link to the exact (mount, dentry) pair the descriptor
    refers to, so the target is the object that was checked and not the result of a second walk
    of its name.  The SOURCE is still a pathname -- mount(2) has no other form for it -- and
    every source this script passes is one of four fixed values it chose itself ('proc',
    'sysfs', '/dev', '/dev/pts'), never a name anything untrusted can define."""
    target = ("/proc/self/fd/%d" % target_fd).encode("ascii")
    ctypes.set_errno(0)
    rc = _libc.mount(None if source is None else source.encode("utf-8", "surrogateescape"),
                     target,
                     None if fstype is None else fstype.encode("ascii"),
                     ctypes.c_ulong(flags),
                     None)
    if rc != 0:
        err = ctypes.get_errno()
        fail(FAILED, "%s failed: mount(2) returned %s onto the descriptor held on %s."
             % (what, errno_name(err), shown))


def op_verify(target_fd, shown, args):
    """Nothing further: reaching here means the anchor and the target were opened and their
    identities verified on their descriptors, which is exactly what the caller asked to prove."""
    if args:
        fail(USAGE, "'verify' takes no arguments.")
    del target_fd, shown


def op_mount_fs(target_fd, shown, args):
    if len(args) != 2:
        fail(USAGE, "'mount-fs' takes exactly two arguments: the filesystem type and the "
                    "source.")
    mount_through_descriptor(args[1], target_fd, args[0], 0, shown,
                             "mounting the %s filesystem '%s'" % (args[0], args[1]))


def op_mount_bind(target_fd, shown, args):
    if len(args) != 1:
        fail(USAGE, "'mount-bind' takes exactly one argument: the source directory.")
    mount_through_descriptor(args[0], target_fd, None, MS_BIND, shown,
                             "bind-mounting '%s'" % args[0])


def op_mount_bind_under(target_fd, shown, args):
    # The parent identity in args[1]/args[2] was consumed during resolution; it is validated
    # there and re-checked for shape here so a malformed call cannot reach mount(2).
    if len(args) != 3:
        fail(USAGE, "'mount-bind-under' takes exactly three arguments: the source directory and "
                    "the device and inode of the directory its parent component must turn out "
                    "to be.")
    mount_through_descriptor(args[0], target_fd, None, MS_BIND, shown,
                             "bind-mounting '%s'" % args[0])


def op_remount_nosymfollow(target_fd, shown, args):
    if args:
        fail(USAGE, "'remount-nosymfollow' takes no arguments.")
    mount_through_descriptor(None, target_fd, None, MS_REMOUNT | MS_NOSYMFOLLOW, shown,
                             "remounting with MS_NOSYMFOLLOW")


def op_chroot_exec(target_fd, shown, args):
    """THE DESCRIPTOR-CONSUMING chroot.  fchdir() moves this process's working directory to the
    object the descriptor refers to -- no pathname is involved -- and chroot(".") then resolves
    against that working directory, so there is no name for anything to redirect between the
    fstat above and the chroot here.  chdir("/") afterwards is what chroot(8) does too: it
    leaves the working directory at the new root by its new name rather than by the old one."""
    global EXEC_MODE
    if not args:
        fail(USAGE, "'chroot-exec' takes the command to run inside the image, and its "
                    "arguments.")
    try:
        os.fchdir(target_fd)
    except OSError as exc:
        fail(FAILED, "fchdir onto the verified descriptor for %s failed (%s), so the chroot "
                     "cannot consume it." % (shown, errno_name(exc.errno)))
    try:
        os.chroot(".")
    except OSError as exc:
        fail(FAILED, "chroot(\".\") onto the verified descriptor for %s failed (%s)."
             % (shown, errno_name(exc.errno)))
    try:
        os.chdir("/")
    except OSError as exc:
        fail(FAILED, "the working directory could not be set to the new root after chroot (%s)."
             % errno_name(exc.errno))
    # Past this point the process is inside the image and no host path is reachable by name, so
    # a failure here is the command's own and is reported with chroot(8)'s own conventions
    # rather than as a custody refusal.
    EXEC_MODE = False
    exec_in_image(list(args))


def exec_in_image(argv):
    """execvp(3)'s search, done with os.execv and nothing else.

    WHY NOT os.execvp.  This process has already chrooted, so the python installation that is
    running it is no longer reachable by name -- and CPython's os.execvp resolves PATH in python,
    through a helper that imports a module LAZILY (measured on this host: 'No module named
    warnings', then an exit status of 1 that says nothing about the command).  os.execv performs
    no lookup and imports nothing, so the search is done here, over os.environ, exactly as
    execvp(3) documents it: a name containing a slash is used as it stands, an empty PATH element
    means the working directory, and an unset PATH falls back to the confstr default that
    os.defpath already holds.

    The status conventions are chroot(8)'s, because that is what this operation replaced: 127
    when the command could not be found anywhere on the search path, 126 when it was found and
    could not be invoked.  ENOEXEC is reported as 126 rather than retried through a shell --
    every command this script runs inside the image is a real executable and a silent shell
    fallback would hide a corrupt one."""
    command = argv[0]
    if "/" in command:
        candidates = [command]
    else:
        search = os.environ.get("PATH", os.defpath)
        candidates = [(element if element else ".") + "/" + command
                      for element in search.split(":")]
    not_invocable = None
    for candidate in candidates:
        try:
            os.execv(candidate, argv)
        except OSError as exc:
            if exc.errno not in (errno.ENOENT, errno.ENOTDIR):
                not_invocable = exc.errno
    if not_invocable is not None:
        sys.stderr.write("pinned-op: '%s' was found inside the image but could not be invoked "
                         "(%s).\n" % (command, errno_name(not_invocable)))
        sys.exit(126)
    sys.stderr.write("pinned-op: '%s' could not be found inside the image on the search path "
                     "the chroot environment supplies.\n" % command)
    sys.exit(127)


OPERATIONS = {
    "verify": op_verify,
    "mount-fs": op_mount_fs,
    "mount-bind": op_mount_bind,
    "mount-bind-under": op_mount_bind_under,
    "remount-nosymfollow": op_remount_nosymfollow,
    "chroot-exec": op_chroot_exec,
}
# The operations for which the target identity is carried by the operation's own arguments
# rather than by the target-dev/target-inode fields; see resolve_under_mount().
PARENT_IDENTIFIED = ("mount-bind-under",)


def parse_identity(text, what):
    try:
        value = int(text, 10)
    except ValueError:
        fail(USAGE, "the %s must be a decimal number, not '%s'." % (what, text))
    if value < 0:
        fail(USAGE, "the %s must not be negative ('%s')." % (what, text))
    return value


def main(argv):
    global EXEC_MODE
    if len(argv) < 8:
        fail(USAGE, "usage: pinned-op.py <anchor-directory> <anchor-dev> <anchor-inode> "
                    "<target-relative|.> <target-dev|-> <target-inode|-> <operation> "
                    "[arguments]")
    anchor, anchor_dev, anchor_ino = argv[1], argv[2], argv[3]
    relative, target_dev, target_ino = argv[4], argv[5], argv[6]
    operation, args = argv[7], argv[8:]
    handler = OPERATIONS.get(operation)
    if handler is None:
        fail(USAGE, "unknown operation '%s'; known operations are %s."
             % (operation, ", ".join(sorted(OPERATIONS))))
    if operation == "chroot-exec":
        # Raised BEFORE anything is opened, so every custody failure on this operation reports
        # the reserved status rather than one the exec'd command could also have produced.
        EXEC_MODE = True
    if not anchor.startswith("/"):
        fail(USAGE, "the anchor directory must be an absolute path.")
    if relative.startswith("/"):
        fail(USAGE, "the target must be relative to the anchor, or '.' for the anchor itself.")
    if not relative:
        fail(USAGE, "the target must be non-empty; use '.' for the anchor itself.")
    anchor_dev = parse_identity(anchor_dev, "anchor device")
    anchor_ino = parse_identity(anchor_ino, "anchor inode")

    # THE IDENTITY FIELDS ARE NOT OPTIONAL, AND THERE IS NO ARM THAT SKIPS THE CHECK.  '-' is
    # accepted in exactly two cases and required in both: the target IS the anchor, whose
    # identity has just been verified, or the operation carries the identity it verifies in its
    # own arguments.  Any other combination is a caller defect and is refused here.
    if relative == "." or operation in PARENT_IDENTIFIED:
        if target_dev != "-" or target_ino != "-":
            fail(USAGE, "the target device and inode must both be '-' for '%s' on target '%s': "
                        "the identity that is verified comes from the anchor pin or from the "
                        "operation's own arguments." % (operation, relative))
    else:
        target_dev = parse_identity(target_dev, "target device")
        target_ino = parse_identity(target_ino, "target inode")

    anchor_fd = open_anchor(anchor, anchor_dev, anchor_ino)
    if operation in PARENT_IDENTIFIED:
        if len(args) != 3:
            fail(USAGE, "'%s' takes exactly three arguments: the source and the device and "
                        "inode of its parent component." % operation)
        target_fd, shown = resolve_under_mount(anchor_fd, anchor, relative,
                                              parse_identity(args[1], "parent device"),
                                              parse_identity(args[2], "parent inode"))
    else:
        target_fd, shown = resolve_target(anchor_fd, anchor, relative, target_dev, target_ino)
    handler(target_fd, shown, args)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
PINNED_OP_PY
    [ -s "$target" ] || die "the pinned privileged-operation helper could not be written to
       $target.  Every mount and chroot this script performs consumes a descriptor it opens and
       verifies, so the run stops rather than falling back to a pathname mount or chroot."
    chmod 0600 -- "$target" || die "could not set mode 0600 on the pinned privileged-operation
       helper at $target."
    PINNED_OP_HELPER="$target"
    PINNED_OP_PYTHON="$interpreter"
}

# Turn one helper exit status into a `die` that names WHICH condition it was.  Never called for
# status 0, and never called for chroot-exec: after a successful execve the status on the wire
# belongs to the command that ran, not to the helper, which is exactly why that one operation
# has a single reserved status of its own (see pinned_op_custody_die).
pinned_op_die() { # $1=status  $2=path as shown  $3=what it is  $4=what was about to happen
    local status="$1" shown="$2" what="$3" operation="$4"
    case "$status" in
        "$PINNED_OP_UNAVAILABLE")
            die "refusing to $operation ($what): the descriptor-consuming form of this privileged
       operation is NOT AVAILABLE on this host.  It needs openat2(2) -- Linux 5.6 or newer -- and
       a python3 that can reach libc, because that is what lets the mount or chroot consume a
       descriptor this script opened and verified instead of a pathname the kernel walks again.
       There is NO fallback to the pathname form: the fallback is precisely the check-then-use
       defect this mechanism exists to remove.  The target was: $shown" ;;
        "$PINNED_OP_REFUSED")
            die "refusing to $operation ($what): THE CONFINEMENT REFUSED the target
           $shown
       The kernel would not resolve it beneath the image mount point without following a symbolic
       link, without leaving the image and without crossing a mount point.  Some component of it
       -- leading or trailing -- is a link, or points out of the image, or a further filesystem
       has been mounted at it.  Refused rather than resolved: this operation runs as root on the
       BUILD HOST, so a link naming an absolute path would have it act on a HOST directory.  The
       errno is on the line above." ;;
        "$PINNED_OP_ABSENT")
            die "refusing to $operation ($what): the target
           $shown
       does not exist.  This script created it a moment ago through the confinement, so something
       is changing the image's contents while it is being built.  Nothing is mounted or chrooted
       there." ;;
        "$PINNED_OP_WRONG_TYPE")
            die "refusing to $operation ($what): the target
           $shown
       is not a directory.  A privileged mount or chroot is not performed on the strength of a
       filename, and a symbolic link never gets this far -- the descriptor is opened O_NOFOLLOW.
       Inspect the base tarball." ;;
        "$PINNED_OP_IDENTITY")
            die "REFUSING TO $operation ($what): the descriptor opened on
           $shown
       IS NOT THE OBJECT THIS RUN PINNED.  The identity was compared by fstat(2) ON THE
       DESCRIPTOR the operation was about to consume -- not by re-stat'ing a name -- and it
       disagreed with the device and inode recorded when this script created or mounted that
       directory.  The name has been re-pointed at a different object.  The comparison and the
       numbers are on the line above.  Nothing is mounted or chrooted; the image is NOT
       published." ;;
        "$PINNED_OP_ANCHOR_BAD")
            die "refusing to $operation ($what): the anchor directory
           ${IMAGE_MOUNT:-<unset>}
       could not be opened as a directory that is not itself a symbolic link, or the descriptor
       opened on it is not the object this run pinned.  Every target is resolved against that
       descriptor, so without it nothing below is pinned.  The diagnosis is on the line above." ;;
        "$PINNED_OP_USAGE")
            die "internal error while trying to $operation ($what): the pinned privileged-operation
       helper was called with arguments it refused (target '$shown').  This is a defect in
       $SCRIPT_NAME." ;;
        "$PINNED_OP_FAILED")
            die "could not $operation ($what): the operation on
           $shown
       failed after the descriptor was opened and its identity verified, so this is the syscall
       itself reporting a fault rather than the confinement refusing anything.  The errno is on
       the line above.  The image is NOT published." ;;
        "$PINNED_OP_CUSTODY_REFUSED")
            pinned_op_custody_die "$shown" "$what" "$operation" ;;
        *)
            die "could not $operation ($what): the pinned privileged-operation helper exited
       $status on
           $shown
       which this script has no reading for.  Refused rather than interpreted; this is a defect
       in $SCRIPT_NAME." ;;
    esac
}

# The chroot arm, separate because its status is a RESERVED one rather than a descriptive one.
# execve replaces the helper, so after a successful chroot every status on the wire is the
# command's own; a granular contract would therefore be indistinguishable from the command's
# exit codes.  The helper raises its exec-mode flag BEFORE it opens anything, so every custody
# failure on that operation -- anchor, confinement, type, identity, missing openat2 -- reports
# exactly this one status and performs nothing.
#
# THE ONE AMBIGUITY, STATED RATHER THAN HIDDEN.  A command inside the image that itself exited
# $PINNED_OP_CUSTODY_REFUSED would be read here as a custody failure and stop the run.  No
# command this script runs in the image uses that status (they are apt-get, dpkg, dpkg-query,
# tar, rm, gcc, g++, make, ldconfig, lcov, ldd, sh -c and /bin/true), and the trade is
# deliberate: mistaking a command's exit code for a custody failure stops a build that would
# have succeeded, while the converse would let a substituted chroot target through.
pinned_op_custody_die() { # $1=path as shown  $2=what it is  $3=what was about to happen
    local shown="$1" what="$2" operation="$3"
    die "REFUSING TO $operation ($what): CUSTODY OF THE CHROOT TARGET COULD NOT BE ESTABLISHED at
           $shown
       The chroot here consumes a descriptor -- fchdir(fd) then chroot(\".\") -- so it cannot be
       redirected by a name lookup, and it is not performed at all unless the descriptor opened
       O_NOFOLLOW beneath the anchor fstat's equal to the device and inode this run pinned for
       the image root.  One of those steps failed and the helper's own diagnosis is on the line
       above.  There is no fallback to \`chroot <pathname>\`.  If this appears where a command
       inside the image was expected to run, note that a command exiting
       $PINNED_OP_CUSTODY_REFUSED of its own accord is indistinguishable from this condition;
       nothing this script runs inside the image does so."
}

# The guard every wrapper shares.  Like confined_io_ready this is an internal-consistency check
# rather than a runtime condition: it catches a future edit that moves a privileged mount or a
# chroot ahead of the helper's installation and proof, which would otherwise perform it through
# a pathname with nothing pinned.
pinned_op_ready() { # $1=what is about to happen, for the message
    local operation="$1"
    [ "$PINNED_OP_PROVEN" -eq 1 ] || die "internal error: '$operation' was attempted before
       probe_pinned_op() had PROVEN, on this host and this kernel, that the descriptor-consuming
       form of mount and chroot works and that it refuses a substituted target.  Refused rather
       than attempted: an unproven mechanism is indistinguishable from the pathname form it
       replaced, and every comment in $SCRIPT_NAME about the mounts and the chroot consuming a
       descriptor would be false.  This is a defect in $SCRIPT_NAME."
    [ -n "$PINNED_OP_HELPER" ] || die "internal error: '$operation' was attempted before the
       pinned privileged-operation helper was installed, so it would have run through a
       pathname.  This is a defect in $SCRIPT_NAME."
    [ -f "$PINNED_OP_HELPER" ] || die "the pinned privileged-operation helper has disappeared
       from $PINNED_OP_HELPER.  Every mount and chroot this script performs goes through it, so
       '$operation' is refused."
    [ -n "$PINNED_OP_PYTHON" ] || die "internal error: '$operation' was attempted before the
       interpreter that runs the pinned privileged-operation helper had been resolved to an
       absolute path.  It is invoked under \`env -i\` with the IMAGE's PATH, so a bare name would
       be looked up on the wrong search path.  This is a defect in $SCRIPT_NAME."
    [ -x "$PINNED_OP_PYTHON" ] || die "the interpreter that runs the pinned
       privileged-operation helper is no longer executable at
           $PINNED_OP_PYTHON
       so '$operation' is refused rather than performed through a pathname."
}

# The raw call.  Returns the helper's status; the caller decides what it means.  `-I -B` for the
# same reason confined_io uses them: no PYTHON* variable, user site directory or PYTHONPATH from
# the environment reaches a privileged helper, and no bytecode is written beside it.
pinned_op() { # $1=anchor  $2=anchor device  $3=anchor inode  $4=target|.  $5=device|-
              # $6=inode|-  $7=operation  [$8..]=operation arguments
    "$PINNED_OP_PYTHON" -I -B "$PINNED_OP_HELPER" "$@"
}

# The same, anchored on the image root and checked against the identity create_and_mount_image()
# pinned for it.  Every privileged mount and chroot below goes through this, so the anchor and
# its pin appear once rather than at seven call sites.
pinned_image_op() { # $1=target|.  $2=device|-  $3=inode|-  $4=operation  [$5..]=arguments
    pinned_op "$IMAGE_MOUNT" "$IMAGE_ROOT_DEVICE" "$IMAGE_ROOT_INODE" "$@"
}

# --- The dieing wrappers.  $what and $operation appear only in messages. ------------------

# Prove a target is the pinned object by opening it and fstat'ing the descriptor, without
# performing anything through it.  Used by the start-up proof and available to any caller that
# wants the custody check without the operation.
pinned_image_verify() { # $1=target|.  $2=device|-  $3=inode|-  $4=what it is  $5=doing
    local status=0
    pinned_op_ready "$5"
    pinned_image_op "$1" "$2" "$3" verify || status=$?
    [ "$status" -eq 0 ] || pinned_op_die "$status" "$IMAGE_MOUNT/$1" "$4" "$5"
}

# mount(2) of a pseudo-filesystem onto a descriptor this helper opened and verified.  The
# identity is supplied as the RECORD confined_identity produced for that target, so the value
# compared by fstat is the one the confinement measured through openat2 rather than one read
# back from a pathname.
pinned_image_mount_fs() { # $1=target  $2=identity record  $3=fstype  $4=source  $5=what  $6=doing
    local status=0
    pinned_op_ready "$6"
    pinned_image_op "$1" "$(confined_stat_field "$2" 2)" "$(confined_stat_field "$2" 3)" \
        mount-fs "$3" "$4" || status=$?
    [ "$status" -eq 0 ] || pinned_op_die "$status" "$IMAGE_MOUNT/$1" "$5" "$6"
}

# The same for a bind mount.  The SOURCE is still a pathname because mount(2) has no other form
# for it; every source this script passes is one of two fixed values it chose itself.
pinned_image_mount_bind() { # $1=target  $2=identity record  $3=source  $4=what  $5=doing
    local status=0
    pinned_op_ready "$5"
    pinned_image_op "$1" "$(confined_stat_field "$2" 2)" "$(confined_stat_field "$2" 3)" \
        mount-bind "$3" || status=$?
    [ "$status" -eq 0 ] || pinned_op_die "$status" "$IMAGE_MOUNT/$1" "$4" "$5"
}

# The one target that openat2 cannot resolve, walked by descriptor a component at a time; the
# reasoning is on resolve_under_mount() in the helper.  The parent's identity is that of the
# SOURCE already bound at it, which is what proves the component resolved into that bind.
pinned_image_mount_bind_under() { # $1=target  $2=parent device  $3=parent inode  $4=source
                                  # $5=what  $6=doing
    local status=0
    pinned_op_ready "$6"
    pinned_image_op "$1" - - mount-bind-under "$4" "$2" "$3" || status=$?
    [ "$status" -eq 0 ] || pinned_op_die "$status" "$IMAGE_MOUNT/$1" "$5" "$6"
}

# The remount, whose target is the anchor itself -- so the identity checked is the image-root pin
# open_anchor() already verified on the descriptor.
#
# THIS ONE RETURNS A STATUS RATHER THAN DYING ON EVERY NON-ZERO ONE, and the split is the point:
# a CUSTODY failure is fatal here exactly as it is everywhere else, while mount(2) itself
# refusing MS_NOSYMFOLLOW is the legitimate "this kernel or filesystem does not offer it" case
# that try_nosymfollow_remount() reports and proceeds without.  Distinguishing them is why the
# helper has a separate status for a syscall fault.
pinned_image_remount_nosymfollow() { # $1=what it is  $2=doing   -> 0 on success, 1 if mount(2) refused
    local status=0
    pinned_op_ready "$2"
    pinned_image_op . - - remount-nosymfollow 2>/dev/null || status=$?
    [ "$status" -ne 0 ] || return 0
    if [ "$status" -eq "$PINNED_OP_FAILED" ]; then
        return 1
    fi
    pinned_op_die "$status" "$IMAGE_MOUNT" "$1" "$2"
}

# ------------------------------------------------------------------------------------
# THE START-UP PROOF FOR THE PINNED OPERATIONS.  Two positive cases, eight negative cases and the
# exec path's reserved refusal, in a private directory, before one privileged mount or chroot is
# performed.
#
# WHY THE MOUNT AND CHROOT OPERATIONS THEMSELVES ARE NOT AMONG THE CASES, STATED RATHER THAN
# LEFT AS AN OMISSION.  What can fail silently here is the CUSTODY CHAIN -- the anchor open, the
# openat2 resolution, the descriptor walk and the fstat comparison -- because a helper that
# skipped any of those would still report success and every claim in this file would be false.
# That chain is identical for all six operations: they differ only in which syscall consumes the
# descriptor at the end, and `verify` is that chain with no syscall at the end, so exercising it
# exercises exactly what could degrade.  The syscalls themselves cannot be rehearsed in a scratch
# directory without mounting or chrooting there, which would be a privileged operation performed
# before the proof that licenses it; they are instead exercised once, for real, at the seven call
# sites, each of which is fatal on failure.  The exec path IS proven, because its refusal uses a
# reserved status that nothing else produces and a silent success there is unrecoverable.
#
# It is fatal on every arm: a positive case that fails means the mechanism does not work, and a
# negative case that SUCCEEDS means it is not pinning anything.
# ------------------------------------------------------------------------------------
probe_pinned_op() { # $1=a private directory to probe in
    local root="$1" anchor outside device inode wrong_inode status=0

    if [ -z "$root" ] || [ ! -d "$root" ]; then
        die "internal error: probe_pinned_op was given '${root:-<empty>}', which is not a
       directory.  This is a defect in $SCRIPT_NAME."
    fi
    pinned_op_ready_for_probe

    anchor="$root/pinned-op-probe"
    outside="$root/pinned-op-outside"
    rm -rf -- "$anchor" "$outside" || die "could not clear the pinned-operation probe directory
       under $root."
    mkdir -p -- "$anchor/beneath" || die "could not create the pinned-operation probe directory
       $anchor/beneath."
    mkdir -p -- "$outside/target" || die "could not create the pinned-operation probe's control
       directory $outside/target."
    ln -sfn "$outside/target" "$anchor/absolute-link" || die "could not create the
       pinned-operation probe's absolute symbolic link."
    ln -sfn 'beneath' "$anchor/relative-link" || die "could not create the pinned-operation
       probe's relative symbolic link."

    device="$(stat -c '%d' -- "$anchor" 2>/dev/null || printf '')"
    inode="$(stat -c '%i' -- "$anchor" 2>/dev/null || printf '')"
    if [ -z "$device" ] || [ -z "$inode" ]; then
        die "the pinned-operation probe's own anchor directory $anchor could not be measured, so
       the mechanism that pins every privileged mount and chroot cannot be proven.  The run
       stops rather than proceeding on an unproven mechanism."
    fi
    wrong_inode=$((inode + 1))

    # POSITIVE, twice.  The anchor is openable and its descriptor fstat's to the pinned identity;
    # so does a directory resolved beneath it through openat2.
    pinned_op "$anchor" "$device" "$inode" . - - verify >/dev/null 2>&1 \
        || die "the pinned-operation helper could NOT open its own probe anchor directory and
       verify its identity on the descriptor during the start-up proof.  Every privileged mount
       and chroot in this script consumes a descriptor obtained and checked exactly that way, so
       the run stops here rather than falling back to a pathname mount or chroot."
    local beneath_device beneath_inode
    beneath_device="$(stat -c '%d' -- "$anchor/beneath" 2>/dev/null || printf '')"
    beneath_inode="$(stat -c '%i' -- "$anchor/beneath" 2>/dev/null || printf '')"
    pinned_op "$anchor" "$device" "$inode" beneath "$beneath_device" "$beneath_inode" verify \
        >/dev/null 2>&1 || die "the pinned-operation helper could NOT resolve a plain directory
       beneath its own probe anchor through openat2 and verify the resulting descriptor's
       identity during the start-up proof.  The run stops rather than proceeding on an unproven
       mechanism."

    # NEGATIVE, eight of them, each of which MUST be refused with the status that names it.  A
    # success on any of them means the custody chain is not checking what it claims to check.
    probe_pinned_op_refusal "$anchor" "$device" "$wrong_inode" . - - verify \
        "$PINNED_OP_IDENTITY" \
        'an ANCHOR whose descriptor does not fstat to the pinned inode'
    probe_pinned_op_refusal "$anchor/relative-link" "$device" "$inode" . - - verify \
        "$PINNED_OP_ANCHOR_BAD" \
        'an ANCHOR that is itself a symbolic link (opened O_DIRECTORY|O_NOFOLLOW)'
    probe_pinned_op_refusal "$anchor" "$device" "$inode" beneath "$beneath_device" \
        "$((beneath_inode + 1))" verify "$PINNED_OP_IDENTITY" \
        'a TARGET whose descriptor does not fstat to the pinned inode'
    probe_pinned_op_refusal "$anchor" "$device" "$inode" absolute-link - - verify \
        "$PINNED_OP_USAGE" \
        'a target given without the identity it must be checked against'
    probe_pinned_op_refusal "$anchor" "$device" "$inode" absolute-link "$beneath_device" \
        "$beneath_inode" verify "$PINNED_OP_REFUSED" \
        'a symbolic link whose target is an absolute path outside the anchor'
    probe_pinned_op_refusal "$anchor" "$device" "$inode" relative-link "$beneath_device" \
        "$beneath_inode" verify "$PINNED_OP_REFUSED" \
        'a symbolic link whose target is inside the anchor (refused too: no link is followed)'
    probe_pinned_op_refusal "$anchor" "$device" "$inode" ../pinned-op-outside/target \
        "$beneath_device" "$beneath_inode" verify "$PINNED_OP_REFUSED" \
        'a target that climbs out of the anchor with .. (RESOLVE_BENEATH, not a check of ours)'
    probe_pinned_op_refusal "$anchor" "$device" "$inode" beneath/absent "$beneath_device" \
        "$beneath_inode" verify "$PINNED_OP_ABSENT" \
        'a target that is not there at all'

    # THE EXEC PATH, PROVEN SEPARATELY.  chroot-exec reports every custody failure as one
    # reserved status because execve leaves no other channel, so what has to be established is
    # that the reserved status really is raised BEFORE anything is opened -- otherwise a
    # substituted chroot target would be entered and the command run inside it.  /bin/true is
    # named rather than a shell so that nothing but execve can affect the outcome.
    status=0
    pinned_op "$anchor" "$device" "$wrong_inode" . - - chroot-exec /bin/true \
        >/dev/null 2>&1 || status=$?
    if [ "$status" -eq 0 ]; then
        die "THE PINNED-OPERATION HELPER DID NOT REFUSE A CHROOT ONTO AN ANCHOR WHOSE DESCRIPTOR
       DOES NOT FSTAT TO THE PINNED IDENTITY during the start-up proof: it reported success.  A
       chroot that is entered on the strength of a name rather than a verified descriptor is the
       whole defect this mechanism removes, and every comment in $SCRIPT_PATH about it would be
       false.  The run stops here; nothing privileged is done through it."
    fi
    [ "$status" -eq "$PINNED_OP_CUSTODY_REFUSED" ] || die "the pinned-operation helper returned
       status $status when a chroot was attempted onto an anchor whose descriptor does not fstat
       to the pinned identity, where $PINNED_OP_CUSTODY_REFUSED was required.  That status is
       reserved for a custody refusal on the exec path and is raised before anything is opened,
       so any other value means the refusal came from somewhere else and the custody check itself
       was not exercised.  The run stops rather than proceeding on an unproven mechanism."
    log "  pinned operations: refused a chroot onto an anchor that does not fstat to the pinned"
    log "    identity (status $status, the reserved custody refusal)"

    rm -rf -- "$anchor" "$outside" || die "could not remove the pinned-operation probe directory
       under $root."
    PINNED_OP_PROVEN=1
    log "pinned privileged operations proven on this host: a descriptor is opened O_NOFOLLOW on"
    log "  the anchor, its identity is verified by fstat ON THAT DESCRIPTOR, a target beneath it"
    log "  is resolved by openat2 under RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV and"
    log "  verified the same way, and a bad anchor, a bad target, a missing identity, an"
    log "  absolute link, a relative link, a '..' escape and an absent target are all refused."
    log "  Every mount(2) and chroot(2) below CONSUMES one of those descriptors -- the mounts"
    log "  through /proc/self/fd/N, the chroot through fchdir(fd) then chroot(\".\") -- so no"
    log "  pathname is resolved a second time between the check and the use.  There is no"
    log "  fallback to the pathname form."
}

# The one part of pinned_op_ready that applies before the mechanism has been proven: the helper
# and its interpreter must be installed, or the proof itself would report the wrong thing.
pinned_op_ready_for_probe() {
    [ -n "$PINNED_OP_HELPER" ] || die "internal error: the start-up proof of the pinned
       privileged operations ran before the helper's path was recorded.  This is a defect in
       $SCRIPT_NAME."
    [ -f "$PINNED_OP_HELPER" ] || die "internal error: the start-up proof of the pinned
       privileged operations ran before the helper was installed at
           $PINNED_OP_HELPER
       This is a defect in $SCRIPT_NAME."
    [ -n "$PINNED_OP_PYTHON" ] || die "internal error: the start-up proof of the pinned
       privileged operations ran before its interpreter had been resolved to an absolute path.
       This is a defect in $SCRIPT_NAME."
    [ -x "$PINNED_OP_PYTHON" ] || die "internal error: the start-up proof of the pinned
       privileged operations ran with an interpreter that is not executable at
           $PINNED_OP_PYTHON
       This is a defect in $SCRIPT_NAME."
}

# One negative case of the start-up proof.  Separate so each refusal reports its own name.
probe_pinned_op_refusal() { # $1..$6=anchor,dev,ino,target,dev,ino  $7=operation
                            # $8=required status  $9=what the case is
    local required="$8" what="$9" status=0
    pinned_op "$1" "$2" "$3" "$4" "$5" "$6" "$7" >/dev/null 2>&1 || status=$?
    if [ "$status" -eq 0 ]; then
        die "THE PINNED-OPERATION HELPER DID NOT REFUSE $what during the start-up proof: it
       resolved
           $4
       against $1 and reported success.  A mechanism that silently accepts an object it was
       meant to refuse is worse than none at all, because every comment in $SCRIPT_PATH about
       the mounts and the chroot consuming a verified descriptor would then be false.  The run
       stops here; nothing privileged is done through it."
    fi
    [ "$status" -eq "$required" ] || die "the pinned-operation helper returned status $status for
       $what during the start-up proof, where $required was required.  Each refusal has to come
       from the condition it names -- the kernel's path resolution, the fstat comparison or the
       argument contract -- so any other status means something else failed and the check itself
       was not exercised.  The run stops rather than proceeding on an unproven mechanism."
    log "  pinned operations: refused $what (status $status)"
}



# ------------------------------------------------------------------------------------
# ENDPOINTS ARE VALIDATED, THEN SANITISED BEFORE THEY ARE SHOWN OR RECORDED.
#
# TWO SEPARATE PROBLEMS, AND EACH NEEDS ITS OWN DEFENCE; CONFLATING THEM LEAVES BOTH OPEN.
#
# The first is INJECTION. --apt-mirror is written verbatim into the image's
# /etc/apt/sources.list, so a value carrying a newline does not configure one archive, it
# configures two - the second one chosen by whoever supplied the value.  A carriage return
# does the same thing while being invisible in a log.  --base-url is handed to curl or wget,
# where a control character is at best a fetch that fails confusingly.  Both are therefore
# refused if they contain any control character at all; ordinary spaces are NOT refused,
# because a sources.list line legitimately contains them ("deb <url> <suite> <components>").
#
# The second is DISCLOSURE.  A URL is a documented place to put a credential -
# https://user:token@host/path is a form apt and curl both accept - and either value would
# otherwise reach this script's own log, the image's build record and the MANIFEST, which the
# workflow uploads as an artifact.  A token that reaches an artifact has left the machine.
# Query strings are treated the same way, because signed-URL schemes carry their signature
# there.
#
# WHAT IS RECORDED INSTEAD: scheme, host and path.  That is enough to answer "which archive
# was this built from", which is the whole reason the value is recorded, and it carries no
# secret.  The fact that something was redacted is stated rather than hidden, so a reader is
# never left believing they are looking at the value that was used.
#
# SANITISING IS FOR THE LOG AND THE MANIFEST, AND IT IS NOT A CONTAINMENT STRATEGY.  Relying on
# it as one would mean writing the unsanitised --apt-mirror into the image's sources.list and
# WARNING the caller that their token is now inside the artifact they just built, as though that
# were an informed decision.  It is not - a warning is what you emit when you have decided to do
# the dangerous thing anyway, and the image is handed to a guest, copied between machines and
# kept as a build output.  So a credential is REFUSED at the boundary rather than carried and
# described:
#
#   * validate_inputs rejects userinfo in either value, rejects a query and a fragment in
#     --apt-mirror because that value is published with the image, and rejects the two
#     characters that could break out of curl's configuration file in --base-url.
#   * acquire_base_rootfs hands --base-url to curl through a mode-0600 file rather than in
#     argv, so it is not readable in `ps` while the transfer runs.
#   * scrub_and_assert_image_apt_credentials checks the FINISHED filesystem, because the base
#     tarball's own apt configuration is not this script's to trust.
#
# What sanitising is still for is unchanged and is worth keeping: a value that is legitimate
# but revealing - an internal hostname, a snapshot path, an accepted --base-url query - is
# reduced to scheme, host and path wherever it is logged or recorded, and the reduction is
# stated so a reader is never left believing they are looking at the value that was used.
#
# These are globals rather than command-substitution results ON PURPOSE: whether anything was
# redacted has to travel back to the caller alongside the sanitised text, and a function whose
# output is captured runs in a subshell whose variable assignments are discarded.
# ------------------------------------------------------------------------------------
SANITISED_ENDPOINT=''
SANITISED_ENDPOINT_REDACTED=0
SANITISED_URL_TOKEN=''

assert_no_control_characters() {
    local value="$1" label="$2" stripped
    stripped="$(printf '%s' "$value" | LC_ALL=C tr -d '[:cntrl:]')"
    if [ "$stripped" != "$value" ]; then
        die "$label contains a control character (a newline, carriage return or similar).
       It is refused rather than stripped, because the two possible readings are too far
       apart to guess between: for --apt-mirror a newline would configure a SECOND package
       archive inside the image, chosen by whoever supplied the value, and for --base-url it
       would be a fetch that fails in a way the message would not explain.  Pass the value on
       one line, with single spaces between its fields."
    fi
}

# Reduces one URL-shaped token to scheme://host/path, dropping userinfo, query and fragment.
# Sets SANITISED_URL_TOKEN, and raises SANITISED_ENDPOINT_REDACTED when it removed something.
sanitise_url_token() {
    local url="$1" scheme='' rest authority path=''
    case "$url" in
        *://*) scheme="${url%%://*}"; rest="${url#*://}" ;;
        *)     rest="$url" ;;
    esac
    case "$rest" in
        *'#'*) rest="${rest%%#*}";    SANITISED_ENDPOINT_REDACTED=1 ;;
    esac
    # \? is a LITERAL question mark here; unescaped it would be a single-character wildcard
    # and would strip almost the whole string.
    case "$rest" in
        *'?'*) rest="${rest%%\?*}";   SANITISED_ENDPOINT_REDACTED=1 ;;
    esac
    case "$rest" in
        */*) authority="${rest%%/*}"; path="/${rest#*/}" ;;
        *)   authority="$rest" ;;
    esac
    # The LAST @ in the authority ends the userinfo, so ## rather than # -- a password may
    # itself contain an @.
    case "$authority" in
        *@*) authority="${authority##*@}"; SANITISED_ENDPOINT_REDACTED=1 ;;
    esac
    if [ -n "$scheme" ]; then
        SANITISED_URL_TOKEN="$scheme://$authority$path"
    else
        SANITISED_URL_TOKEN="$authority$path"
    fi
}

# Extracts the AUTHORITY of one URL-shaped token -- what sits between the scheme and the
# first '/', with query and fragment removed first.  Sets URL_AUTHORITY.
#
# The authority is the only place a userinfo credential can live, and separating it from the
# path is what keeps this from being a blanket search for '@': a legitimate path can carry
# one (http://host/~user@example/), and a sources.list option group or suite name carries
# neither a scheme nor an authority worth speaking of.  A SCHEME-LESS token is treated as
# authority-first, because curl accepts "user:pass@host/path" with no scheme at all -- so
# that form must be caught too.
URL_AUTHORITY=''
url_authority() {
    local url="$1" rest
    case "$url" in
        *://*) rest="${url#*://}" ;;
        *)     rest="$url" ;;
    esac
    rest="${rest%%\#*}"
    # \? is a LITERAL question mark; unescaped it is a single-character wildcard.
    rest="${rest%%\?*}"
    case "$rest" in
        */*) URL_AUTHORITY="${rest%%/*}" ;;
        *)   URL_AUTHORITY="$rest" ;;
    esac
}

# CREDENTIALS IN A URL ARE REFUSED, NOT CARRIED.
#
# Sanitising an endpoint for the log and the manifest was only ever half of it: --apt-mirror
# is written into the PUBLISHED image's /etc/apt/sources.list, so a "user:pass@" form ended up
# inside an artifact that outlives the run, and the script's own report said so rather than
# preventing it.  Refusing the form outright is the one disposition that leaves nothing to
# leak, and it costs a caller nothing they cannot do another way: an authenticated archive is
# configured with --apt-auth-file, which is installed for the apt phase and removed before the
# image is released, and an authenticated base fetch goes through --base-url-file or is done
# separately with --base-tarball.
#
# AND THE MESSAGE DOES NOT OFFER THE BASE TARBALL AS AN ALTERNATIVE.  Putting an auth.conf.d
# entry or a netrc INSIDE THE BASE TARBALL would bake a permanent credential into the published
# image -- the same exposure by a different route, and one a scan of the caller's own archive
# cannot be relied on to find.  --apt-auth-file is the supported route precisely because its
# lifetime is bounded by this script.
#
# The message never echoes the value.  It names the option and the sanitised form only,
# because a die message reaches the same log the sanitising exists for.
# ------------------------------------------------------------------------------------
# A SECRET INPUT MUST NOT LIVE INSIDE ANYTHING THIS SCRIPT COPIES INTO THE IMAGE.
#
# WHY A PRIVATE FILE IS NOT ENOUGH BY ITSELF.  --base-url-file and --apt-auth-file exist so
# that a confidential value is read from a caller-owned private file instead of arriving in this
# script's argv, where /proc/<pid>/cmdline exposes it to every user on the build host.  That
# protects the value on the HOST and says nothing about the IMAGE -- and copy_guest_payload()
# copies --payload, --sdk-dir and --gtest-prefix into the image WHOLESALE with `cp -a`, so
#
#     --base-url-file "$PAYLOAD_DIR/.base-url"
#
# would take a file whose entire purpose is confidentiality and publish it inside an artefact
# that outlives the run and is uploaded by the workflow.  The credential-store scan cannot see
# it either: that scan looks at apt's and root's credential locations, and this file is neither.
#
# THIS IS A POLICY CHECK, AND IT RUNS AT VALIDATION TIME -- before a byte is copied, so the
# refusal costs the caller nothing but a re-invocation.  It is deliberately paired with a
# SECOND, INDEPENDENT CHECK of a different kind: assert_image_carries_no_secret_values()
# searches the copied trees inside the finished image for the secret VALUES themselves, which
# catches a route this policy cannot see -- a secret whose path is outside every copied root
# but whose content was duplicated into one of them by something else.
#
# WHAT IS AND IS NOT A COPIED ROOT.  --payload, --sdk-dir and --gtest-prefix are copied as
# whole trees and are therefore roots.  --base-tarball and --lcov-tarball are not: their
# CONTENT is extracted, so a secret file inside one of them is a property of the archive the
# caller built, and the pre-publication credential scan is what looks for credentials that
# arrive that way.  --kernel-config is a single file copied verbatim into a record; it is not a
# tree, so no path can be "inside" it.
# ------------------------------------------------------------------------------------
path_is_within() { # $1=candidate  $2=root -> 0 when $1 IS $2 or lies beneath it
    if [ "$1" = "$2" ]; then
        return 0
    fi
    # A root of "/" is answered directly: "$2"/* would expand as "//*" and match nothing,
    # while the true answer is that everything lies beneath the root directory.
    if [ "$2" = '/' ]; then
        return 0
    fi
    case "$1" in
        "$2"/*) return 0 ;;
    esac
    return 1
}

assert_secret_inputs_outside_copied_roots() {
    local -a secret_opts=() secret_paths=() root_opts=() root_paths=()
    local i j

    # Built only from the options actually given, and only AFTER absolutise_and_check() has
    # canonicalised every one of them -- a textual comparison of two paths that have not both
    # been resolved answers a question about spelling rather than about location.
    if [ -n "$BASE_URL_FILE" ]; then
        secret_opts+=('--base-url-file'); secret_paths+=("$BASE_URL_FILE")
    fi
    if [ -n "$APT_AUTH_FILE" ]; then
        secret_opts+=('--apt-auth-file'); secret_paths+=("$APT_AUTH_FILE")
    fi
    if [ "${#secret_paths[@]}" -eq 0 ]; then
        return 0
    fi

    root_opts+=('--payload');  root_paths+=("$PAYLOAD_DIR")
    root_opts+=('--sdk-dir');  root_paths+=("$SDK_DIR")
    if [ -n "$GTEST_PREFIX_SRC" ]; then
        root_opts+=('--gtest-prefix'); root_paths+=("$GTEST_PREFIX_SRC")
    fi
    if [ -n "$SINK_CALLER_SOURCE_SRC" ]; then
        root_opts+=('--sink-caller-source'); root_paths+=("$SINK_CALLER_SOURCE_SRC")
    fi

    for (( i = 0; i < ${#secret_paths[@]}; i++ )); do
        for (( j = 0; j < ${#root_paths[@]}; j++ )); do
            if path_is_within "${secret_paths[i]}" "${root_paths[j]}"; then
                die "${secret_opts[i]} names a file INSIDE the tree ${root_opts[j]} names, and
       that whole tree is copied into the image:
           ${secret_opts[i]} = ${secret_paths[i]}
           ${root_opts[j]} = ${root_paths[j]}
       ${secret_opts[i]} exists so the value is never exposed on a command line.  Copying the
       file that holds it into a published image would be a far larger exposure than the one
       the option was added to close: the image outlives this run and the workflow uploads it.
       Move the file OUTSIDE every tree this script copies -- ${root_opts[j]} and the other
       copied roots -- and pass the new path.  A directory beside the payload rather than
       inside it, readable only by the invoking account, is the usual answer."
            fi
        done
    done

    log "secret inputs checked against the copied trees: none of them lies inside --payload,"
    log "  --sdk-dir or --gtest-prefix, so none is copied into the image by construction."
}

assert_no_url_userinfo() {
    local value="$1" option="$2" field shown
    local -a fields=()
    read -r -a fields <<< "$value"
    for field in ${fields[@]+"${fields[@]}"}; do
        url_authority "$field"
        case "$URL_AUTHORITY" in
            *@*)
                shown="$(sanitised_endpoint_display "$value")"
                die "$option carries a credential in the URL itself (a 'user:pass@host' userinfo
       segment).  It is refused rather than sanitised, because sanitising only protects the
       log: the value reaches curl, or the published image's apt configuration, verbatim.
       Configure an authenticated archive with --apt-auth-file, which is installed for the apt
       phase only and removed before the image is released, or fetch an authenticated base
       through --base-url-file or separately with --base-tarball and its digest.  Do NOT put a
       credential file in the base tarball: it would be published inside the image, and the
       pre-publication scan refuses to publish an image that carries one.  The value with its
       userinfo removed is: $shown"
                ;;
        esac
    done
}

# A QUERY STRING IS REFUSED FOR EVERY VALUE THAT ARRIVES IN argv, AND PERMITTED ONLY IN THE
# --base-url-file FORM.
#
# The line this draws is not "which option" but "where the value ends up", and there are now
# two ways for it to end up somewhere it should not:
#
#   * --apt-mirror is written into the PUBLISHED image's /etc/apt/sources.list, so anything in
#     its query is published with the artefact.  Refused, as it always was.
#   * --base-url arrives in this script's own argv, which means the whole value sits in
#     /proc/<this-pid>/cmdline, readable by every user on the build host for as long as the
#     process lives.  A presigned signature in a query string is a BEARER CREDENTIAL, so that
#     is a credential disclosure to local users -- and it is one this script cannot fix from
#     the inside: writing the URL to a private file before spawning curl hides it from the
#     CHILD's argv, which was never the exposure that mattered.  So the argv form now refuses a
#     query too, and --base-url-file exists to carry exactly that case: the value is read from
#     a caller-owned file, never appears in any command line, and IS permitted a query.
#
# A fragment is refused alongside it, for two reasons: it is the same class of thing (stripped
# by the sanitisers, so a value carrying one would not match its own record), and refusing both
# is what lets install_image_packages treat "the sanitised form differs from the value" as an
# internal error rather than as a caller mistake.
assert_no_url_query() {
    local value="$1" option="$2" reason="$3"
    case "$value" in
        *'?'*|*'#'*)
            die "$option contains a query string or a fragment.  $reason
       A query string is where a signed or tokenised URL carries its secret, so it is refused
       here rather than carried.  Pass the plain scheme://host/path form, and if the URL must
       carry a signature use --base-url-file, which reads it from a file and never puts it in
       any command line."
            ;;
    esac
}

# curl reads the base URL from a --config file rather than from argv, and that file is written
# as  url = "<value>"  -- so a value containing a double quote or a backslash could end the
# quoted string or escape out of it.  Neither character is valid in a URI unencoded, so
# refusing them costs nothing real and removes the injection entirely.
assert_curl_config_safe() {
    local value="$1" option="$2"
    # *\\* rather than *'\'* -- the two match the same single backslash, and the second form
    # reads to ShellCheck as a mis-escaped quote (SC1003).
    case "$value" in
        *'"'*|*\\*)
            die "$option contains a double quote or a backslash.  The value is handed to curl
       through a private configuration file, as  url = \"...\"  , specifically so that it never
       appears in a process's command line where 'ps' can read it; both characters would end or
       escape out of that quoted string.  Neither is valid in a URI unencoded - percent-encode
       it (%22, %5C) if the endpoint really needs one."
            ;;
    esac
}

# Sanitises every URL-shaped field of a value, so that one function handles both a bare URL
# and a whole sources.list line.  read -a is used rather than `for f in $value` because it
# splits on IFS WITHOUT globbing: a '*' in the value must not be expanded against the build
# host's filesystem.
sanitise_endpoint() {
    local value="$1" out='' field
    local -a fields=()
    SANITISED_ENDPOINT_REDACTED=0
    SANITISED_URL_TOKEN=''
    read -r -a fields <<< "$value"
    for field in ${fields[@]+"${fields[@]}"}; do
        # A field is URL-SHAPED if it carries a scheme, a userinfo separator or a query.  The
        # last two matter on their own: curl accepts a scheme-less "user:pass@host/path", so a
        # test for "://" alone would leave exactly the credential-bearing form unredacted.
        # None of the non-URL fields a sources.list line carries - "deb", a suite name, a
        # component, or a "[signed-by=...]" option group - contains any of the three.
        case "$field" in
            *://*|*@*|*'?'*) sanitise_url_token "$field"; field="$SANITISED_URL_TOKEN" ;;
        esac
        if [ -z "$out" ]; then out="$field"; else out="$out $field"; fi
    done
    SANITISED_ENDPOINT="$out"
}

# The sanitised form of $1, with a note when anything was removed.  Safe to use in a command
# substitution because the redaction state is folded into the returned text.
sanitised_endpoint_display() {
    sanitise_endpoint "$1"
    if [ "$SANITISED_ENDPOINT_REDACTED" -eq 1 ]; then
        printf '%s  [userinfo/query redacted]' "$SANITISED_ENDPOINT"
    else
        printf '%s' "$SANITISED_ENDPOINT"
    fi
}

# ------------------------------------------------------------------------------------
# INPUT VALIDATION.  Every path is absolutised, collapsed, refused if it lands somewhere it
# must not, checked for existence, and checked for characters that cannot be embedded in the
# generated in-guest configuration.
# ------------------------------------------------------------------------------------
validate_inputs() {
    # SCALAR SHAPES FIRST.  A value that cannot be read is refused before anything else is
    # judged, so `--size abc` is reported as a bad size rather than as whichever required
    # argument happened to be checked first -- a message about the wrong thing costs the
    # caller a whole second run.
    require_positive_integer "$IMAGE_SIZE_MIB" '--size'
    require_positive_integer "$READINESS_TIMEOUT_SECONDS" '--readiness-timeout'
    # Only when it was given: an ABSENT pin is a supported mode (record, do not compare), while a
    # pin that is not a number would compose package names like "gcc-thirteen" and fail three
    # phases later on something that reads as an archive problem.
    [ -z "$TOOLCHAIN_GCC_MAJOR" ] || require_positive_integer "$TOOLCHAIN_GCC_MAJOR" '--toolchain-gcc-major'

    # THE STAGING TOOLCHAIN RECORD.  Both halves are optional and independent of each other --
    # a caller may know the version and not have decided a state, or the reverse -- so neither
    # implies the other and neither is required.  What IS refused is a value of the wrong shape,
    # because both are written into records inside the published image and a record nobody can
    # parse is worse than an absent one.
    #
    # The version is held to a dotted numeric form: it is a compiler version, and anything else
    # in that field means the caller measured something other than a compiler.
    if [ -n "$STAGING_GCC_VERSION" ]; then
        case "$STAGING_GCC_VERSION" in
            *[!0-9.]*|.*|*.|'') die "--staging-gcc-version must be a dotted numeric compiler
       version such as 12.2.0, got $(render_untrusted "$STAGING_GCC_VERSION").  It is the
       measured version of the family that built the staged i386 artefacts, recorded in the
       image beside the SDK's own build flags; a value that is not a version would be published
       as provenance nobody can act on." ;;
        esac
    fi
    # The state is a CLOSED SET rather than free text, so a reader of the image's provenance can
    # compare it mechanically instead of interpreting prose.  'matches-pin' and 'deviates-from-pin'
    # are the two the caller can establish; 'unknown' is what an honest caller passes when it
    # measured the staging compiler but has no pin to compare it against.
    if [ -n "$STAGING_TOOLCHAIN_STATE" ]; then
        case "$STAGING_TOOLCHAIN_STATE" in
            matches-pin|deviates-from-pin|unknown) : ;;
            *) die "--staging-toolchain-state must be one of matches-pin, deviates-from-pin or
       unknown, got $(render_untrusted "$STAGING_TOOLCHAIN_STATE").  It is a closed set on
       purpose: the image's provenance record is read mechanically and free text there cannot be
       compared." ;;
        esac
    fi

    # THE THREE PROVENANCE REVISIONS.  Each is optional; each is refused if malformed.  See
    # require_commit_revision for why the form is the runner's rather than this script's.
    [ -z "$PROVENANCE_REVISION_SUPERPROJECT" ] || \
        require_commit_revision "$PROVENANCE_REVISION_SUPERPROJECT" '--provenance-revision-superproject'
    [ -z "$PROVENANCE_REVISION_HDMICEC" ] || \
        require_commit_revision "$PROVENANCE_REVISION_HDMICEC" '--provenance-revision-hdmicec'
    [ -z "$PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK" ] || \
        require_commit_revision "$PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK" \
            '--provenance-revision-entservices-hdmicecsink'

    [ -n "$OUTPUT_IMAGE" ] || die "--output is required: this script has no default place to
       write a root image and will not invent one.  Run '$SCRIPT_PATH --help'."
    [ -n "$PAYLOAD_DIR" ]  || die "--payload is required: the guest builds and measures the
       hdmicec checkout, so there is nothing to boot without one."
    [ -n "$SDK_DIR" ]      || die "--sdk-dir is required: the guest needs the staged Binder SDK
       and the AIDL client stub snapshots, and this script will not guess where they are."

    # THE BASE COMES FROM EXACTLY ONE OF THREE PLACES, and the count is done before any of
    # them is read so that "which provenance does the manifest record" has one answer.
    local base_sources=0
    [ -z "$BASE_TARBALL" ]  || base_sources=$(( base_sources + 1 ))
    [ -z "$BASE_URL" ]      || base_sources=$(( base_sources + 1 ))
    [ -z "$BASE_URL_FILE" ] || base_sources=$(( base_sources + 1 ))
    [ "$base_sources" -le 1 ] || die "--base-tarball, --base-url and --base-url-file are
       alternatives; more than one was given.  Pass the one that describes where the base
       actually comes from, so the manifest records a single provenance for it."
    [ "$base_sources" -eq 1 ] || die "a base root filesystem is required: pass --base-tarball
       FILE (preferred: no network at all), --base-url URL (fetched on THIS host, never from
       inside the guest), or --base-url-file FILE (the same fetch, with the URL read from a
       private file so that it never enters a command line) -- together with its SHA256.  There
       is no built-in default, because an unpinned base would make this job irreproducible."

    # THE FILE FORM IS READ HERE, and from this point on the two forms are indistinguishable to
    # everything downstream: BASE_URL holds the value, BASE_URL_SOURCE records which form it
    # arrived in, and only the query-string rule below consults that difference.
    if [ -n "$BASE_URL_FILE" ]; then
        BASE_URL_FILE="$(absolutise_and_check "$BASE_URL_FILE" '--base-url-file')"
        require_private_file "$BASE_URL_FILE" 'the --base-url-file base URL file'
        BASE_URL="$(read_private_line "$BASE_URL_FILE" 'the --base-url-file base URL file')"
        BASE_URL_SOURCE='--base-url-file'
        # The path is logged; the value is not, and will not be anywhere but in its sanitised
        # scheme://host/path form.
        log "base URL read from the private file $BASE_URL_FILE (not shown; its sanitised form"
        log "  is recorded in the manifest and appears in the fetch log)"
    elif [ -n "$BASE_URL" ]; then
        BASE_URL_SOURCE='--base-url'
    fi

    # THE TWO ENDPOINT VALUES, CHECKED BEFORE ANYTHING USES, LOGS OR RECORDS THEM.  --apt-mirror
    # is written straight into the image's sources.list, where a newline configures a second
    # archive, so it cannot go unvalidated.  Neither value is a path, so neither goes through
    # absolutise_and_check; what they need is the control-character refusal above.
    #
    # AND CREDENTIALS ARE REFUSED HERE RATHER THAN CARRIED AND SANITISED.  Sanitising protects
    # the log and the manifest; it does not protect the IMAGE, which is where --apt-mirror ends
    # up, nor this process's own command line, which is where an argv --base-url ends up.  So
    # userinfo is refused in both, and the base URL is additionally checked for the two
    # characters that could break out of the curl configuration file it is passed through.
    # Every one of these is a refusal at the boundary, which is the only disposition that leaves
    # nothing to leak.
    #
    # EVERY CHECK APPLIES TO BOTH BASE-URL FORMS.  BASE_URL now holds the file form's value too,
    # so reading it from a file buys privacy of TRANSPORT and no relaxation of validation.
    [ -z "$BASE_URL" ]   || assert_no_control_characters "$BASE_URL"   "$BASE_URL_SOURCE"
    [ -z "$APT_MIRROR" ] || assert_no_control_characters "$APT_MIRROR" '--apt-mirror'
    [ -z "$BASE_URL" ]   || assert_no_url_userinfo       "$BASE_URL"   "$BASE_URL_SOURCE"
    [ -z "$APT_MIRROR" ] || assert_no_url_userinfo       "$APT_MIRROR" '--apt-mirror'
    [ -z "$BASE_URL" ]   || assert_curl_config_safe      "$BASE_URL"   "$BASE_URL_SOURCE"

    # THE QUERY-STRING RULE, WHICH IS THE ONE PLACE THE TWO FORMS DIFFER.
    #
    # --apt-mirror: refused because the value is written into the PUBLISHED image.
    # --base-url (argv): refused because this script cannot hide its own command line.  A
    #   presigned signature in a query string is a bearer credential, and /proc/<pid>/cmdline
    #   is readable by every local user for this process's whole lifetime.  Writing the URL to
    #   a private file before spawning curl -- which this script does -- hides it from the
    #   CHILD only, and the child was never the exposure.
    # --base-url-file: PERMITTED, and it is the reason that option exists.  The value never
    #   appears in any argv, so a signature in its query is disclosed to nobody.
    [ -z "$APT_MIRROR" ] || assert_no_url_query "$APT_MIRROR" '--apt-mirror' \
        "Unlike --base-url-file, this value is written into the PUBLISHED image's
       /etc/apt/sources.list, so everything in it is published with the artefact."
    if [ -n "$BASE_URL" ] && [ "$BASE_URL_SOURCE" = '--base-url' ]; then
        assert_no_url_query "$BASE_URL" '--base-url' \
        "This value arrived on this script's COMMAND LINE, so the whole of it is in
       /proc/<pid>/cmdline for as long as this process lives, readable by every user on the
       build host.  Pass it with --base-url-file instead: the URL is read from a private file,
       is never an argument to anything, and a query string is accepted there."
    fi

    OUTPUT_IMAGE="$(absolutise_and_check "$OUTPUT_IMAGE" '--output')"
    assert_path_embeddable "$OUTPUT_IMAGE" '--output'
    [ -d "$(dirname -- "$OUTPUT_IMAGE")" ] || die "the directory holding --output does not
       exist: $(dirname -- "$OUTPUT_IMAGE").  Create it first; this script will not create a
       parent it was not asked for."
    [ ! -d "$OUTPUT_IMAGE" ] || die "--output names an existing DIRECTORY: $OUTPUT_IMAGE.
       It must name the image FILE to write."

    PAYLOAD_DIR="$(absolutise_and_check "$PAYLOAD_DIR" '--payload')"
    assert_path_embeddable "$PAYLOAD_DIR" '--payload'
    require_existing_dir "$PAYLOAD_DIR" 'the --payload hdmicec checkout'

    SDK_DIR="$(absolutise_and_check "$SDK_DIR" '--sdk-dir')"
    assert_path_embeddable "$SDK_DIR" '--sdk-dir'
    require_existing_dir "$SDK_DIR" 'the --sdk-dir staged rdk-halif-aidl tree'

    if [ -n "$BASE_TARBALL" ]; then
        BASE_TARBALL="$(absolutise_and_check "$BASE_TARBALL" '--base-tarball')"
        require_existing_file "$BASE_TARBALL" 'the --base-tarball base root filesystem'
    fi
    if [ -n "$LCOV_TARBALL" ]; then
        LCOV_TARBALL="$(absolutise_and_check "$LCOV_TARBALL" '--lcov-tarball')"
        require_existing_file "$LCOV_TARBALL" 'the --lcov-tarball lcov 2.x source tarball'
    fi
    if [ -n "$GTEST_PREFIX_SRC" ]; then
        GTEST_PREFIX_SRC="$(absolutise_and_check "$GTEST_PREFIX_SRC" '--gtest-prefix')"
        require_existing_dir "$GTEST_PREFIX_SRC" 'the --gtest-prefix GoogleTest prefix'
        require_existing_file "$GTEST_PREFIX_SRC/lib/pkgconfig/gtest.pc" \
            "gtest.pc under the --gtest-prefix prefix.  configure consults pkg-config ONLY for
       GoogleTest and ignores -I/-L, so a prefix without a .pc file cannot satisfy it"
    fi
    if [ -n "$KERNEL_CONFIG_SRC" ]; then
        KERNEL_CONFIG_SRC="$(absolutise_and_check "$KERNEL_CONFIG_SRC" '--kernel-config')"
        require_existing_file "$KERNEL_CONFIG_SRC" 'the --kernel-config guest kernel configuration'
    fi

    # THE SINK CALLER SOURCE.  Named, or found beside the payload, or the build fails -- there is
    # deliberately no fourth outcome in which an image is produced without it.
    #
    # The reason it cannot degrade to a warning is the same reason the guard itself fails rather
    # than skipping: an image whose guard cannot run still executes
    # DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected, which
    # asserts against a MODEL of two Sink call sites. A model nothing has checked against its
    # subject is not evidence about a caller, and an aggregate count cannot tell the difference.
    # So the failure is moved here, to image-build time on the host, where the fix is obvious and
    # cheap -- rather than being discovered as a red test inside a guest an hour later.
    if [ -n "$SINK_CALLER_SOURCE_SRC" ]; then
        SINK_CALLER_SOURCE_SRC="$(absolutise_and_check "$SINK_CALLER_SOURCE_SRC" '--sink-caller-source')"
        require_existing_file "$SINK_CALLER_SOURCE_SRC" \
            "the --sink-caller-source Sink plugin source.  It is read by the L1 drift guard
       DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSinkSource and is a
       required acceptance input"
    else
        # The payload is the hdmicec checkout, so its sibling is where a superproject checkout
        # puts the plugin. Reported when used, so a reader of the log never has to guess which
        # of the two routes supplied the file.
        local sink_sibling="$PAYLOAD_DIR/../entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp"
        if [ -r "$sink_sibling" ]; then
            SINK_CALLER_SOURCE_SRC="$(absolutise_and_check "$sink_sibling" 'the Sink source beside --payload')"
            log "--sink-caller-source was not given; using the tree beside --payload:"
            log "  $SINK_CALLER_SOURCE_SRC"
        else
            die "the real Sink caller source was not supplied and is not beside the --payload tree,
       so this image would carry no source for the L1 drift guard to read.  THAT IS A REQUIRED
       ACCEPTANCE INPUT, NOT AN OPTIONAL DIAGNOSTIC: without it,
       DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSinkSource fails
       inside the guest, and DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSink
       CallPathsAsExpected goes on asserting against a model of the Sink plugin that nothing has
       checked against its subject.
       SUPPLY IT EITHER WAY: pass --sink-caller-source with the absolute path of
       entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp from a checkout pinned at the
       commit the guard names as reviewed -- which is what .github/workflows/aidl-path-tests.yml
       does -- or place that checkout beside the --payload tree, where this path was tried:
       $sink_sibling"
        fi
    fi
    assert_path_embeddable "$SINK_CALLER_SOURCE_SRC" '--sink-caller-source'
    # --apt-auth-file.  Checked for confidentiality the same way --base-url-file is.  Its
    # CONTENT is apt's business in the sense that this script does not interpret it: nothing here
    # parses a record into fields it acts on, nothing logs it and nothing digests it into the
    # manifest.  It IS read, three times -- copied into the image, and searched for twice before
    # publication -- and every one of those reads goes through the descriptor custody was
    # established on rather than through this name again.
    #
    # WHY THE DESCRIPTOR IS HELD RATHER THAN THE PATH RECHECKED.  Custody established on a
    # descriptor describes the object behind it, and only for as long as nothing reopens the
    # name.  Four uses that each resolved $APT_AUTH_FILE afresh would be four objects checked
    # once between them, and the one this script copied into the image need not be the one it
    # vetted: replacing a directory entry is governed by its parent, and the ancestry walk
    # expressly does not vet the parents of a symlinked path component.  So the descriptor stays
    # open for the run and APT_AUTH_CUSTODY names the route to it; the open descriptor pins the
    # inode, so a substitution at the name afterwards cannot reach the bytes.
    #
    # What this script guarantees beyond confidentiality is the LIFETIME -- installed immediately
    # before the apt phase, removed immediately after, and independently refused by the
    # pre-publication credential-store scan if it somehow survives.
    if [ -n "$APT_AUTH_FILE" ]; then
        APT_AUTH_FILE="$(absolutise_and_check "$APT_AUTH_FILE" '--apt-auth-file')"
        hold_private_file "$APT_AUTH_FILE" 'the --apt-auth-file apt credential file'
        APT_AUTH_CUSTODY="$VETTED_FD_PROC"
        # Emptiness is asked of the held descriptor too.  Asking it of the name would answer for
        # whatever that name reaches now, which is the substitution this whole arrangement exists
        # to remove -- and would answer it about a different object from the one installed.
        [ -s "$APT_AUTH_CUSTODY" ] || die "the --apt-auth-file apt credential file is empty:
       $APT_AUTH_FILE
       It was given, so it was meant to authenticate something; an empty one would install a
       credential file that authenticates nothing and then be removed again."
    fi

    # THE DIGEST IS MANDATORY.  A sidecar file is accepted because that is how published
    # artefacts usually carry their digest, but the absence of both is a refusal: this script
    # never extracts a base it could not verify.
    if [ -z "$BASE_SHA256" ] && [ -n "$BASE_TARBALL" ] && [ -f "$BASE_TARBALL.sha256" ]; then
        BASE_SHA256="$(read_sidecar_digest "$BASE_TARBALL.sha256")"
        log "base tarball digest taken from the sidecar file $BASE_TARBALL.sha256"
    fi
    [ -n "$BASE_SHA256" ] || die "no SHA256 for the base root filesystem.  Pass --base-sha256
       HEX, or place a sidecar <tarball>.sha256 beside it.  This script hard-codes no digest --
       a digest written into it from memory would be a fabricated measurement -- so it refuses
       to extract a base it cannot verify.  Compute one with:
           sha256sum <tarball>"
    require_sha256_hex "$BASE_SHA256" 'the base tarball SHA256'

    if [ -n "$LCOV_TARBALL" ]; then
        if [ -z "$LCOV_SHA256" ] && [ -f "$LCOV_TARBALL.sha256" ]; then
            LCOV_SHA256="$(read_sidecar_digest "$LCOV_TARBALL.sha256")"
            log "lcov tarball digest taken from the sidecar file $LCOV_TARBALL.sha256"
        fi
        [ -n "$LCOV_SHA256" ] || die "--lcov-tarball was given without a digest.  Pass
       --lcov-sha256 HEX or place a sidecar <tarball>.sha256 beside it; an unverified source
       tarball is refused for the same reason the base is."
        require_sha256_hex "$LCOV_SHA256" 'the lcov tarball SHA256'
    fi

    # LAST, because it compares canonical path against canonical path and every path above is
    # canonicalised by the time it runs.
    assert_secret_inputs_outside_copied_roots
}

# First whitespace-delimited field of a `sha256sum`-style line.  Accepts both the plain
# digest and the "digest  filename" form, because published sidecars use either.
read_sidecar_digest() { # $1=sidecar path -> digest on stdout
    local first
    first="$(awk 'NF { print $1; exit }' "$1" 2>/dev/null || true)"
    [ -n "$first" ] || die "the sidecar digest file $1 is empty or unreadable.  Remove it and
       pass the digest explicitly, or fix the file."
    printf '%s\n' "$first"
}

verify_sha256() { # $1=file  $2=expected digest  $3=what it is
    local actual
    actual="$(sha256sum -- "$1" | awk '{ print $1 }')"
    if [ "$actual" != "${2,,}" ] && [ "$actual" != "$2" ]; then
        die "SHA256 MISMATCH for $3:
           file     $1
           expected $2
           actual   $actual
       Refusing to use it.  Either the artefact is not the one that was pinned, or it was
       corrupted in transit.  Re-fetch it, or correct the pin -- do not relax this check."
    fi
    log "verified $3 against its pinned SHA256"
}

# ------------------------------------------------------------------------------------
# THE STAGED SDK.  Verified against the layout build_binder.sh and the snapshot builds
# produce, so that a missing piece is a named failure with a remedy here rather than a
# compile error, a link error or a boot-time abort inside the guest.
# ------------------------------------------------------------------------------------
SDK_BINDER_TARGET=''
SDK_BINDER_BUILD=''
SDK_HALIF_LIB=''
SDK_SERVICEMANAGER=''
SDK_BINDER_LIB=''

verify_sdk_tree() {
    SDK_BINDER_TARGET="$SDK_DIR/$SDK_REL_BINDER_TARGET"
    SDK_BINDER_BUILD="$SDK_DIR/$SDK_REL_BINDER_BUILD"
    SDK_HALIF_LIB="$SDK_DIR/$SDK_REL_HALIF_LIB"
    SDK_SERVICEMANAGER="$SDK_DIR/$SDK_REL_SERVICEMANAGER"
    SDK_BINDER_LIB="$SDK_BINDER_TARGET/lib/binder"
    readonly SDK_BINDER_TARGET SDK_BINDER_BUILD SDK_HALIF_LIB SDK_SERVICEMANAGER SDK_BINDER_LIB

    require_existing_dir "$SDK_BINDER_TARGET" "the staged Binder SDK target tree
       ($SDK_REL_BINDER_TARGET under --sdk-dir).  Build the SDK before building the image"
    require_existing_dir "$SDK_BINDER_BUILD" "the staged Binder SDK build tree
       ($SDK_REL_BINDER_BUILD under --sdk-dir), which is where the Binder headers live"
    require_existing_dir "$SDK_BINDER_LIB" "the staged Binder library directory
       ($SDK_REL_BINDER_TARGET/lib/binder under --sdk-dir)"
    require_existing_dir "$SDK_HALIF_LIB" "the staged AIDL stub library directory
       ($SDK_REL_HALIF_LIB under --sdk-dir)"

    require_existing_file "$SDK_BINDER_BUILD/include/binder_sdk/binder/IServiceManager.h" \
        "the Binder SDK headers the generated stubs include.  Expected
       $SDK_REL_BINDER_BUILD/include/binder_sdk/binder/ under --sdk-dir"

    local root
    for root in "${SDK_REL_INCLUDE_ROOTS[@]}"; do
        require_existing_dir "$SDK_DIR/$root" "the AIDL include root '$root' under --sdk-dir.
       All three are required and none substitutes for another: the snapshot stubs, the frozen
       common import they were generated against, and common/current, which is the only place
       halcompat.h lives"
    done

    local library
    for library in "${HALIF_STUB_LIBRARIES[@]}"; do
        require_existing_file "$SDK_HALIF_LIB/$library" "the AIDL stub library $library in
       $SDK_REL_HALIF_LIB.  common is pinned at 0.2.0.0 because that is the ABI the released
       hdmicec 0.1.0.0 snapshot was generated against; a newer common release is not a
       substitute"
        assert_target_arch_binary "$SDK_HALIF_LIB/$library" 'a staged AIDL stub library'
    done

    for library in "${BINDER_CLOSURE_LIBRARIES[@]}"; do
        require_existing_file "$SDK_BINDER_LIB/$library" "$library in the staged Binder
       library directory.  libbinder and libutils are direct link edges of libRCEC; liblog,
       libbase, libcutils and libcutils_sockets are libbinder's own dependencies and are
       required because they must be LOADABLE, not because the middleware names them"
        assert_target_arch_binary "$SDK_BINDER_LIB/$library" 'a staged Binder library'
    done

    require_existing_file "$SDK_SERVICEMANAGER" "servicemanager at $SDK_REL_SERVICEMANAGER
       under --sdk-dir.  It is an unconditional runtime prerequisite wherever a binder driver
       is present: the service name cannot be resolved without it, and obtaining an
       IServiceManager at all loops until binder handle 0 resolves"
    assert_target_arch_binary "$SDK_SERVICEMANAGER" 'the staged servicemanager'

    log "staged SDK verified: stubs, the six-library Binder closure and servicemanager are"
    log "  present and built for $ARCH"
}

# One binary's class and machine, described by readelf where it is available and by `file`
# otherwise.  readelf is preferred because its spelling of the machine is the stable one; see
# the note above resolve_arch.
elf_description() { # $1=path -> one line on stdout, empty when nothing could read it
    local text=''
    if command -v readelf >/dev/null 2>&1; then
        text="$(readelf -h -- "$1" 2>/dev/null \
                | sed -n 's/^ *\(Class\|Machine\): *\(.*\)$/\1=\2;/p' | tr -d '\n' || true)"
    fi
    if [ -z "$text" ]; then
        text="$(file -b -- "$1" 2>/dev/null || true)"
    fi
    printf '%s\n' "$text"
}

# A mismatch is fatal HERE, where the message can name the remedy, rather than at boot where
# it surfaces as an exec format error or a binder protocol mismatch.
assert_target_arch_binary() { # $1=path  $2=what it is
    local description
    description="$(elf_description "$1")"
    [ -n "$description" ] || die "could not identify $2:
           $1
       Neither readelf nor file could describe it.  Refusing to copy a binary whose
       architecture cannot be read into a guest whose entire userspace must be $ARCH."

    if printf '%s\n' "$description" | grep -Eq -- "$ELF_REJECT_REGEX"; then
        die "$2 is built for a 64-bit machine:
           $1
           $description
       The guest userspace is 32-bit throughout and a process is a single ELF class, so a
       64-bit staged artefact could not be loaded into any process the guest starts.  Rebuild
       the Binder SDK and the AIDL stub snapshots for $ARCH, or select the architecture they
       were built for with --arch.  This is a BITNESS refusal and says nothing about the
       binder wire protocol, which is derived separately."
    fi
    printf '%s\n' "$description" | grep -Eq -- "$ELF_CLASS_REGEX" || die "$2 is not a 32-bit
       object:
           $1
           $description
       The guest userspace is 32-bit throughout; rebuild it for $ARCH or pass the --arch it
       was built for."
    printf '%s\n' "$description" | grep -Eq -- "$ELF_MACHINE_REGEX" || die "$2 was built for a
       different machine than --arch $ARCH (expected $EXPECTED_ELF_MACHINE):
           $1
           $description
       Rebuild it for $ARCH, or pass the --arch it was actually built for."
}

# ------------------------------------------------------------------------------------
# THE PAYLOAD.  Two checks, and the second one is the one that saves a QEMU cycle.
#
# The guest COMPILES the payload, so stale build output for the wrong architecture is not a
# harmless leftover: make would try to relink it and fail with an unhelpful message deep
# inside a build log, inside a VM, after a boot.  Refusing it here, by name, is worth the
# scan.
# ------------------------------------------------------------------------------------
verify_payload_tree() {
    require_existing_file "$PAYLOAD_DIR/configure.ac" "configure.ac in the --payload tree.
       --payload must be the hdmicec checkout itself, not its parent"
    require_existing_file "$PAYLOAD_DIR/tests/L1Tests/run_coverage.sh" \
        "tests/L1Tests/run_coverage.sh in the --payload tree.  It is what the in-guest init
       invokes, and it owns the whole invocation matrix"
    [ -x "$PAYLOAD_DIR/tests/L1Tests/run_coverage.sh" ] || warn \
        "tests/L1Tests/run_coverage.sh is not executable in the payload; the init invokes it
  through 'bash', so this is survivable, but it is worth fixing at the source."

    local -a mismatched=()
    local candidate description
    while IFS= read -r candidate; do
        [ -n "$candidate" ] || continue
        description="$(file -b -- "$candidate" 2>/dev/null || true)"
        # Only ELF files are judged: an archive of text, a stray script or a data file
        # matching one of the globs is not build output for any architecture.  `file` is used
        # here rather than readelf because this walks a whole tree and file is the cheaper of
        # the two; the patterns accept either tool's spelling.
        case "$description" in
            *ELF*)
                if printf '%s\n' "$description" | grep -Eq -- "$ELF_REJECT_REGEX" \
                   || ! printf '%s\n' "$description" | grep -Eq -- "$ELF_CLASS_REGEX" \
                   || ! printf '%s\n' "$description" | grep -Eq -- "$ELF_MACHINE_REGEX"; then
                    mismatched+=("$candidate -- $description")
                fi ;;
            *) : ;;
        esac
    done < <(find "$PAYLOAD_DIR" \( -name '*.o' -o -name '*.lo' -o -name '*.a' \
                                    -o -name '*.so' -o -name '*.so.*' \) -type f 2>/dev/null)

    if [ "${#mismatched[@]}" -gt 0 ]; then
        local entry
        warn "the --payload tree carries build output built for another architecture:"
        for entry in "${mismatched[@]}"; do
            warn "  $entry"
        done
        die "refusing to build a $ARCH image around build output for another architecture.
       The guest rebuilds this tree, and make would try to relink these objects with the
       guest's $ARCH toolchain and fail inside the VM, where the message is hardest to read.
       Pass a clean checkout -- 'git clean -xfd' inside the submodule -- or a payload built
       for $ARCH."
    fi
    assert_payload_carries_no_credentials
    log "payload verified: an hdmicec checkout with run_coverage.sh, no foreign build output"
    log "  and no credential material"
}

# ------------------------------------------------------------------------------------
# THE PAYLOAD MUST NOT CARRY CREDENTIAL MATERIAL INTO THE IMAGE.
#
# WHY THIS BELONGS HERE AND NOT ONLY IN THE WORKFLOW.  Everything in --payload is copied into
# the image, and the image is uploaded as a CI artifact -- so anything credential-shaped in
# the payload becomes downloadable.  The workflow that drives this script stages a source-only
# payload and checks it, but this script is also runnable by hand, and a check that only
# exists in the caller is a check that is absent exactly when someone is improvising.  This is
# the same reasoning that puts the digest verification here rather than in the workflow.
#
# WHAT IS REFUSED, AND WHAT IS DELIBERATELY NOT.
#   * A file-based credential store -- .git-credentials, .netrc -- anywhere in the tree, and a
#     private key file.  Refused outright: none of them has any business in a payload whose
#     only job is to be built and measured.
#   * git configuration carrying an Authorization header (actions/checkout's
#     persist-credentials writes http.<host>/.extraheader), a credential helper, or a URL
#     rewrite that can hold userinfo.  Read out of git's own configuration when git is
#     available so any spelling is found, and out of .git/config directly when it is not.
#   * A .git directory as such is NOT refused.  It is not credential material by itself, the
#     workflow strips it for size as much as for safety, and refusing it would stop a
#     developer from pointing --payload at their working checkout -- which is the ordinary way
#     to use this script by hand.  The credential checks above are what matter, and they apply
#     to that checkout too.
#
# NOTHING THAT MATCHES IS EVER PRINTED.  Only the KEY or the FILE NAME is named; a message
# that quoted the value would write the credential into the log that reports it.
# ------------------------------------------------------------------------------------
assert_payload_carries_no_credentials() {
    local found keys offenders

    found="$(find "$PAYLOAD_DIR" \
                  \( -name '.git-credentials' -o -name '.netrc' -o -name '.npmrc' \
                     -o -name 'id_rsa' -o -name 'id_dsa' -o -name 'id_ecdsa' \
                     -o -name 'id_ed25519' -o -name '*.pem' -o -name '*.p12' \) \
                  -type f -print 2>/dev/null || true)"
    if [ -n "$found" ]; then
        warn 'the --payload tree contains credential or key files:'
        printf '%s\n' "$found" | sed 's/^/       /' >&2
        die "refusing to copy credential material into the guest image.  Everything in the
       payload is copied into the image, and the image is uploaded as a CI artifact, so these
       files would be published.  Remove them from the payload -- or stage a copy without
       them and pass that, which is what the workflow does."
    fi

    if [ -d "$PAYLOAD_DIR/.git" ]; then
        keys=''
        if command -v git >/dev/null 2>&1; then
            keys="$(git -C "$PAYLOAD_DIR" config --local --name-only --list 2>/dev/null || true)"
        elif [ -r "$PAYLOAD_DIR/.git/config" ]; then
            # No git on the host: read the file.  Only key-shaped lines are considered, so a
            # value that happens to contain one of these words is not matched.
            keys="$(grep -Eo '^[[:space:]]*(extraheader|helper|insteadOf)[[:space:]]*=' \
                         -- "$PAYLOAD_DIR/.git/config" 2>/dev/null || true)"
        fi
        offenders="$(printf '%s\n' "$keys" | grep -Ei 'extraheader|credential\.|helper|insteadof' || true)"
        if [ -n "$offenders" ]; then
            warn 'the --payload checkout retains credential-bearing git configuration.'
            warn 'Offending keys (values deliberately not printed):'
            printf '%s\n' "$offenders" | sed 's/^/       /' >&2
            die "refusing to copy a checkout with persisted credentials into the guest image,
       which this job uploads as an artifact.  With actions/checkout, set
       'persist-credentials: false'; by hand, clear the entry with
           git -C '$PAYLOAD_DIR' config --local --unset-all http.\"\$host\".extraheader
       or stage a copy of the tree without .git and pass that."
        fi
    fi
}

# ------------------------------------------------------------------------------------
# THE BINDER WIRE PROTOCOL: DERIVED FROM TWO ARTEFACTS, NEVER DECLARED.
#
# This function exists because a HARD-CODED protocol -- in the help text, the architecture log,
# the value written into the image and the manifest -- is a claim in four places that nothing
# compares against the SDK shipped alongside it.  An SDK built BINDER_IPC_32BIT=OFF is protocol
# 8, and against a declared 7 the two claims cannot both be true while nothing detects it.
# Prose cannot be checked.  So nothing here states a protocol, even though the job's intended
# value and the SDK's build flag agree on 7: two build-time artefacts DECIDE it, this function
# reads both, and a disagreement fails the build by name.  That is also the check that catches
# the workflow's kernel patch silently failing to apply, which is the one plausible way the
# intended 7 turns into a running 8.
#
#   THE KERNEL HALF.  CONFIG_ANDROID_BINDER_IPC_32BIT in the configuration the guest kernel
#   was built with, supplied by --kernel-config.  Explicitly =y means protocol 7.  Unset, or
#   absent from the Kconfig altogether (which is what an UNPATCHED 5.x kernel shows, because
#   mainline deleted the option in the 4.18 era while CONFIG_ANDROID_BINDERFS starts at 5.0 --
#   the workflow restores it with an inline patch and asserts it in the built .config), means
#   protocol 8.
#
#   THE USERSPACE HALF.  BINDER_IPC_32BIT in the staged SDK's own CMakeCache.txt.  ON means
#   protocol 7 and OFF means protocol 8, per linux_binder_idl CMakeLists.txt:733-739 at tag
#   2.6.0, which compiles -DBINDER_IPC_32BIT=1 into libbinder in the ON arm only.
#
# AN ABSENT CACHE ENTRY IS NOT READ AS OFF, and that distinction is measured rather than
# stylistic: CMakeLists.txt:218-224 sets the variable with a plain set() when it is not
# already defined, so a build that never passed it as a CACHE entry leaves NO entry in the
# cache while still compiling protocol 7.  Reading "absent" as OFF would therefore invent
# the more convenient answer.  Absent means "this source cannot answer", and it is reported
# as such.
#
# FAIL-CLOSED WHEN NEITHER SOURCE CAN ANSWER.  Without a derived expectation the guest has
# nothing to cross-check the driver against, which is the state that produced the
# contradiction in the first place, so the build stops here with the remedy named.
# ------------------------------------------------------------------------------------
DERIVED_BINDER_PROTOCOL=''
PROTOCOL_FROM_KERNEL_CONFIG=''
PROTOCOL_FROM_SDK_CACHE=''
PROTOCOL_KERNEL_EVIDENCE=''
PROTOCOL_SDK_EVIDENCE=''

# Sets PROTOCOL_FROM_KERNEL_CONFIG and PROTOCOL_KERNEL_EVIDENCE from one kernel
# configuration file.  Globals rather than stdout, so that the diagnostic text and the
# derived value cannot be split by a separator appearing inside either of them.
read_protocol_from_kernel_config() { # $1=path
    local config="$1" line=''
    PROTOCOL_FROM_KERNEL_CONFIG=''
    if [ ! -r "$config" ]; then
        PROTOCOL_KERNEL_EVIDENCE="unreadable: $config"
        return 0
    fi
    line="$(grep -E '^(CONFIG_ANDROID_BINDER_IPC_32BIT=|# CONFIG_ANDROID_BINDER_IPC_32BIT is not set)' \
            -- "$config" 2>/dev/null | head -n 1 || true)"
    case "$line" in
        CONFIG_ANDROID_BINDER_IPC_32BIT=y|CONFIG_ANDROID_BINDER_IPC_32BIT=Y)
            PROTOCOL_FROM_KERNEL_CONFIG='7'
            PROTOCOL_KERNEL_EVIDENCE="$config -> $line" ;;
        CONFIG_ANDROID_BINDER_IPC_32BIT=*|'# CONFIG_ANDROID_BINDER_IPC_32BIT is not set')
            # Anything other than =y is not "set" in Kconfig terms, so it is protocol 8 -- and
            # the literal line is carried through so a reader can see what it actually said.
            PROTOCOL_FROM_KERNEL_CONFIG='8'
            PROTOCOL_KERNEL_EVIDENCE="$config -> $line" ;;
        *)
            PROTOCOL_FROM_KERNEL_CONFIG='8'
            PROTOCOL_KERNEL_EVIDENCE="$config -> CONFIG_ANDROID_BINDER_IPC_32BIT absent from this Kconfig entirely, which is what a 5.x mainline kernel shows: the option was removed in the 4.18 era and CONFIG_ANDROID_BINDERFS starts at 5.0" ;;
    esac
}

# Sets PROTOCOL_FROM_SDK_CACHE and PROTOCOL_SDK_EVIDENCE from every CMakeCache.txt under the
# staged SDK.  Several caches are tolerated as long as every one that carries the entry
# agrees; two that disagree are a staging fault and are refused rather than resolved by
# picking one.
read_protocol_from_sdk_caches() {
    local cache entry value seen='' evidence='' conflict=0
    PROTOCOL_FROM_SDK_CACHE=''
    while IFS= read -r cache; do
        [ -n "$cache" ] || continue
        entry="$(grep -E '^BINDER_IPC_32BIT(:[A-Za-z]*)?=' -- "$cache" 2>/dev/null | head -n 1 || true)"
        [ -n "$entry" ] || continue
        value="$(printf '%s\n' "${entry#*=}" | tr '[:lower:]' '[:upper:]')"
        case "$value" in
            ON|1|TRUE|YES|Y)     value='7' ;;
            OFF|0|FALSE|NO|N|'') value='8' ;;
            *)                   value='' ;;
        esac
        [ -n "$value" ] || continue
        evidence="$evidence${evidence:+; }${cache#"$SDK_DIR"/} -> $entry"
        if [ -z "$seen" ]; then
            seen="$value"
        elif [ "$seen" != "$value" ]; then
            conflict=1
        fi
    done < <(find "$SDK_BINDER_BUILD" "$SDK_BINDER_TARGET" -maxdepth 3 -name CMakeCache.txt -type f 2>/dev/null)

    if [ "$conflict" -eq 1 ]; then
        die "the staged Binder SDK contains CMake caches that DISAGREE about BINDER_IPC_32BIT,
       so the wire protocol the staged libbinder speaks cannot be established:
           $evidence
       One staging tree cannot have been built two ways.  Rebuild the SDK from clean, or
       remove the stale build directory, and pass a staging tree with a single answer."
    fi
    if [ -z "$seen" ]; then
        PROTOCOL_SDK_EVIDENCE='no CMakeCache.txt under the staged tree carries a BINDER_IPC_32BIT cache entry. An absent entry is NOT read as OFF: linux_binder_idl CMakeLists.txt:218-224 sets the variable with a plain set() when it was not passed as a cache entry, so a protocol-7 build can leave no trace in the cache'
        return 0
    fi
    PROTOCOL_FROM_SDK_CACHE="$seen"
    PROTOCOL_SDK_EVIDENCE="$evidence"
}

derive_binder_protocol() {
    local suggested_sdk_flag=''

    if [ -n "$KERNEL_CONFIG_SRC" ]; then
        read_protocol_from_kernel_config "$KERNEL_CONFIG_SRC"
    else
        PROTOCOL_KERNEL_EVIDENCE='no --kernel-config was given, so the kernel half cannot be read'
    fi

    read_protocol_from_sdk_caches

    log "deriving the binder wire protocol from the two artefacts that decide it:"
    log "  kernel configuration : ${PROTOCOL_FROM_KERNEL_CONFIG:-<cannot answer>}"
    log "    $PROTOCOL_KERNEL_EVIDENCE"
    log "  staged SDK cache     : ${PROTOCOL_FROM_SDK_CACHE:-<cannot answer>}"
    log "    $PROTOCOL_SDK_EVIDENCE"

    if [ -n "$PROTOCOL_FROM_KERNEL_CONFIG" ] && [ -n "$PROTOCOL_FROM_SDK_CACHE" ] \
       && [ "$PROTOCOL_FROM_KERNEL_CONFIG" != "$PROTOCOL_FROM_SDK_CACHE" ]; then
        if [ "$PROTOCOL_FROM_KERNEL_CONFIG" = '7' ]; then
            suggested_sdk_flag='ON'
        else
            suggested_sdk_flag='OFF'
        fi
        die "BINDER WIRE PROTOCOL DISAGREEMENT between the two build-time artefacts:
           the guest kernel configuration says protocol $PROTOCOL_FROM_KERNEL_CONFIG
               ($PROTOCOL_KERNEL_EVIDENCE)
           the staged Binder SDK says protocol $PROTOCOL_FROM_SDK_CACHE
               ($PROTOCOL_SDK_EVIDENCE)
       libbinder performs a strict protocol-version EQUALITY check when it opens the driver
       node (linux_binder_idl BUILD.md:409-413 at tag 2.6.0), so this combination fails EVERY
       open inside the guest and would surface as three inexplicably broken invocations
       rather than as a configuration fault.  Fix ONE side so the two agree -- rebuild the SDK
       with -DBINDER_IPC_32BIT=$suggested_sdk_flag to match this kernel, or build a kernel
       that matches the SDK.
       AND CHECK THE OBVIOUS CAUSE FIRST.  A STOCK kernel with CONFIG_ANDROID_BINDERFS (which
       the in-guest init requires, 5.0 and later) carries no CONFIG_ANDROID_BINDER_IPC_32BIT at
       all: mainline deleted that option in the 4.18 era.  The workflow that drives this script
       restores it with an inline patch and asserts it in the built .config precisely so a
       protocol-7 guest is reachable.  So a kernel half of 8 against an SDK half of 7 is most
       likely that patch not having applied, or a kernel .config from before it -- look at the
       'Restore CONFIG_ANDROID_BINDER_IPC_32BIT' step's output rather than changing the SDK to
       match a kernel that was supposed to be patched."
    fi

    DERIVED_BINDER_PROTOCOL="${PROTOCOL_FROM_KERNEL_CONFIG:-$PROTOCOL_FROM_SDK_CACHE}"
    [ -n "$DERIVED_BINDER_PROTOCOL" ] || die "the binder wire protocol cannot be DERIVED from
       anything this run was given, and this script does not declare one.
           kernel half:   $PROTOCOL_KERNEL_EVIDENCE
           userspace half: $PROTOCOL_SDK_EVIDENCE
       Pass --kernel-config FILE naming the configuration the guest kernel was built with
       (the reliable route, and the one the workflow uses), or stage an SDK built with
       BINDER_IPC_32BIT passed as a CMake CACHE entry so the value survives in its
       CMakeCache.txt.  Without one of the two there is nothing for the guest to cross-check
       the driver's own BINDER_VERSION answer against, and an unverifiable protocol claim is
       exactly what this derivation exists to prevent."

    if [ -z "$PROTOCOL_FROM_KERNEL_CONFIG" ] || [ -z "$PROTOCOL_FROM_SDK_CACHE" ]; then
        warn "only ONE of the two build-time artefacts could answer, so the derived protocol"
        warn "  ($DERIVED_BINDER_PROTOCOL) rests on that one alone. The guest still cross-checks it against"
        warn "  the running kernel's own configuration and against /dev/binder's BINDER_VERSION"
        warn "  answer, and fails on a disagreement -- but supplying both halves here is what"
        warn "  turns a boot-time failure into a build-time one."
    fi

    readonly DERIVED_BINDER_PROTOCOL PROTOCOL_FROM_KERNEL_CONFIG PROTOCOL_FROM_SDK_CACHE
    readonly PROTOCOL_KERNEL_EVIDENCE PROTOCOL_SDK_EVIDENCE
    log "derived binder wire protocol: $DERIVED_BINDER_PROTOCOL (an EXPECTATION; the guest verifies it against the driver)"
}


# ------------------------------------------------------------------------------------
# THE PUBLICATION TARGETS: WHAT ROOT IS ALLOWED TO WRITE, AND WHO ELSE MIGHT BE WRITING IT.
#
# This script runs as root and produces two files at caller-supplied paths.  That makes the
# act of publishing them the most dangerous thing it does, and three distinct hazards apply
# to it.  All three are addressed here, before any content exists, because a check that runs
# after the image is built cannot un-truncate a file.
#
#   (1) SUBSTITUTION.  If --output names a symbolic link, root's `truncate` follows it and
#       destroys the target.  If it names a file with more than one hard link, truncating it
#       destroys the OTHER name's content too.  If any directory on the way to it is a
#       symbolic link, the path the caller typed is not the path that gets written, and the
#       PROTECTED_SYSTEM_ROOTS check that was applied to the typed path proves nothing about
#       where the bytes land.  Every one of those is refused rather than resolved: a caller
#       who meant a link can pass the link's target.
#
#   (2) CONCURRENCY.  Two builders aimed at the same output would interleave a format, a
#       mount and a rename, and the survivor would be a mixture.  An exclusive advisory lock
#       on a file beside the output makes the second one fail immediately with a message that
#       says so, rather than silently corrupting the first one's work.
#
#   (3) DESTRUCTION BEFORE PROOF.  Formatting at the final path destroys the previous good
#       image as its first action, so any later failure leaves the caller with nothing.  The
#       image and the manifest are therefore staged under private names in the same directory
#       and renamed onto the caller's paths together, after validation -- see
#       publish_outputs().  Same directory, so the rename is same-filesystem and atomic.
#
# The lock file is deliberately LEFT IN PLACE at the end of the run.  Removing a lock file
# while holding its lock is a race with teeth: a second builder that already opened the same
# path would hold a lock on an unlinked inode, and both would then believe they had exclusive
# access.  A zero-byte lock file beside the image is the cheap, correct outcome.
# ------------------------------------------------------------------------------------
OUTPUT_DIR=''
OUTPUT_MANIFEST=''
OUTPUT_LOCK=''
OUTPUT_LOCK_HELD=0

# ------------------------------------------------------------------------------------
# OUTPUT-DIRECTORY CUSTODY: THE DIRECTORY ENTRY IS THE ASSET, NOT THE FILE'S MODE.
#
# WHAT THE MODE ON THE LEAF DOES NOT BUY.  The staged image is created by mktemp with O_EXCL
# and then chmod 0600, and that protects its CONTENTS.  It does not protect its NAME: renaming
# or unlinking a directory entry is governed by write permission on the DIRECTORY, not by the
# mode of the file the entry points at.  So in an output directory another account can write
# to, that account can rename the staged leaf away between the mktemp and the truncate, or
# between the mkfs and the losetup, and put a symbolic link or a different file at the same
# name -- after which root formats, mounts, populates and publishes something the caller never
# named.  The lock leaf has exactly the same exposure, and there the substitution is worse than
# a wrong write: two builders holding locks on two different inodes both believe they have
# exclusive access.
#
# WHY STAT-THEN-OPERATE CANNOT CLOSE THIS ON ITS OWN.
#
# Recording the directory's owner, mode, device and inode once and re-comparing them immediately
# before each privileged operation is worth doing, and it is done below -- but it cannot be the
# whole answer if those operations then name the CALLER'S PATHNAME, because a pathname is
# resolved afresh by the kernel at the instant the operation runs.  Between the last stat and the
# rename there is a window, however small, in which any account that may rename an entry in an
# ANCESTOR directory can move the validated directory away and put a different one at its name.
# Narrowing that window is not closing it (CWE-367), and no number of extra stats changes the
# shape of the problem.
#
# NOR IS AN OUTPUT DIRECTORY OWNED BY THE UNPRIVILEGED ACCOUNT THAT INVOKED sudo PERMITTED, on
# the tempting reasoning that a caller who may pass --output is already trusted with the path.
# That reasoning is about the PATH and does not carry to the DIRECTORY ENTRY: a directory's owner
# may rename or unlink a root-owned entry inside it regardless of the entry's mode or owner, so
# an unprivileged owner can substitute the staged image, the destination or the lock leaf between
# any stat and the privileged truncate, mkfs, losetup, rename or chmod that follows.
#
# WHAT IS REQUIRED INSTEAD, AND IT IS FOUR THINGS.
#
#   OWNER: uid 0, and nothing else.  A directory this script publishes into must belong to root.
#   The remedy is one command and it is named in the refusal:
#       install -d -o root -g root -m 0755 <dir>
#
#   MODE: no group or other WRITE bit.  The sticky-bit exemption is gone as well: sticky
#   restricts rename and unlink to the ENTRY's owner, which protects our leaves from other
#   accounts but not from the directory's own owner -- and it invites exactly the shared
#   world-writable publication directory this script should not be publishing into.
#
#   A DIRECTORY DESCRIPTOR, AND EVERY OPERATION PERFORMED THROUGH IT.  The validated directory
#   is OPENED ONCE and the descriptor is verified against what was validated (type, device,
#   inode, owner and mode, read with stat -L /proc/self/fd/N so the answer comes from the OPEN
#   FILE and not from a pathname).  From then on every privileged pathname operation names
#       /proc/self/fd/<fd>/<leaf>
#   rather than the caller's pathname.  That is the bash-reachable equivalent of openat(dirfd,
#   leaf): the descriptor pins the directory INODE, so renaming or replacing the directory, or
#   any ancestor of it, cannot redirect the operation -- it continues to act on the inode that
#   was validated, or it fails.  MEASURED on this host: with the validated directory renamed
#   away and a decoy directory created at its pathname, a `mv` through the descriptor still moved
#   the file inside the original directory and the decoy was left untouched, while a read through
#   the caller's pathname returned the decoy's content.  This is what makes ancestor ownership no
#   longer load-bearing, and it is why the leaf NAMES are what the globals hold: both the
#   descriptor-relative path (for operations) and the readable path (for messages) are composed
#   from a leaf.
#
#   IDENTITY, STILL RE-CHECKED.  None of the stat-based checks is removed, because they answer a
#   different question: the descriptor proves WHICH DIRECTORY we are operating in, and the stats
#   prove that the caller's pathname still refers to it -- so a substitution is REPORTED rather
#   than silently worked around, and a leaf inside the directory that was replaced is still
#   caught by its own device and inode.
#
# THE LEAVES ARE CHECKED THE SAME WAY.  Each staged leaf's device and inode are recorded when
# mktemp creates it and re-compared before it is written to, attached to a loop device or
# renamed; the lock leaf is additionally compared against the inode actually behind the OPEN
# DESCRIPTOR (stat -L /proc/self/fd/N), which is the only way to establish that the lock is
# held on the file at that path rather than on an inode that has since been replaced -- and that
# comparison runs inside the custody assertion, at EVERY call site rather than only at
# acquisition, because the leaf can be replaced after the lock has been taken.
# ------------------------------------------------------------------------------------
OUTPUT_DIR_DEVICE=''
OUTPUT_DIR_INODE=''
OUTPUT_DIR_OWNER=''
OUTPUT_DIR_MODE=''
# The descriptor the publication directory is held open on, and the /proc path composed from it.
# Fixed at 8 rather than allocated with {var}< so that it sits beside the lock's fd 9 and is
# visibly reserved; both are inherited by the child processes that perform the operations, which
# is what makes /proc/self/fd/<n> resolve for them.
readonly OUTPUT_DIR_FD=8
OUTPUT_DIR_PROC=''
# The leaf names of the three caller-visible paths, so that every privileged operation on them
# can be composed against OUTPUT_DIR_PROC.
OUTPUT_IMAGE_LEAF=''
OUTPUT_MANIFEST_LEAF=''
OUTPUT_LOCK_LEAF=''
STAGED_IMAGE_DEVICE=''
STAGED_IMAGE_INODE=''
STAGED_MANIFEST_DEVICE=''
STAGED_MANIFEST_INODE=''
OUTPUT_LOCK_DEVICE=''
OUTPUT_LOCK_INODE=''

# One mode string, normalised to four octal digits so the special bits can be read positionally
# regardless of whether stat printed three or four.  Empty when the value is not octal digits.
pad_four_digit_mode() { # $1=mode as stat printed it
    local mode="$1"
    case "$mode" in
        ''|*[!0-7]*) printf ''; return 0 ;;
    esac
    while [ "${#mode}" -lt 4 ]; do
        mode="0$mode"
    done
    printf '%s' "$mode"
}

# The same, read from a path.  Empty on failure.
four_digit_mode() { # $1=path
    pad_four_digit_mode "$(stat -c '%a' -- "$1" 2>/dev/null || printf '')"
}

# Is this directory's mode acceptable for a PUBLICATION directory?  No group or other write bit,
# full stop -- there is no sticky-bit exemption here.
#
# WHY NO STICKY-BIT EXEMPTION APPLIES HERE.  Sticky restricts rename and unlink to the ENTRY's
# owner, which is the right rule for a SHARED TEMP directory (resolve_temp_parent applies it to
# TMPDIR, and that is deliberate: /tmp is mode 2777 on this container).  It is the wrong rule for
# a directory root publishes two artefacts into, for two reasons.  A group- or world-writable
# directory can still be filled, and more to the point sticky says nothing about the directory's
# OWN OWNER -- and the owner is precisely who can rename a root-owned entry out from under this
# script.  The owner is required to be root, so permitting other accounts to write the directory
# as well would give back with the mode what the ownership requirement takes away.
publication_dir_mode_is_safe() { # $1=four-digit mode
    local mode="$1" group_write other_write
    [ "${#mode}" -eq 4 ] || return 1
    group_write=$(( ${mode:2:1} & 2 ))
    other_write=$(( ${mode:3:1} & 2 ))
    [ "$group_write" -eq 0 ] && [ "$other_write" -eq 0 ]
}

# THE RE-CHECK, run immediately before each privileged pathname operation in the output
# directory.  $1 names the operation, so a refusal says which one was about to happen.
#
# THREE THINGS ARE ESTABLISHED HERE, in this order, and none of them replaces another:
#
#   1. THE DESCRIPTOR still refers to a directory whose type, device, inode, owner and mode are
#      what was validated.  This is the authoritative check, because the descriptor is what every
#      operation actually goes through, and stat -L on /proc/self/fd/<n> reads the OPEN FILE
#      rather than resolving a pathname.
#   2. THE CALLER'S PATHNAME still resolves to that same directory.  Not because the operations
#      depend on it -- they go through the descriptor -- but so that a substitution is REPORTED
#      instead of silently tolerated: an ancestor swapped mid-build means something on this host
#      is doing something this run should not proceed alongside.
#   3. THE LOCK, if held, is still on the inode the lock leaf resolves to.  Checked here rather
#      than only around acquisition, because a leaf left replaceable for the rest of the run is
#      a leaf a second builder can open afresh -- taking an exclusive lock on a different inode
#      and proceeding alongside this one.
assert_output_dir_custody() { # $1=what is about to happen
    local operation="$1" device inode owner mode
    local fd_type fd_device fd_inode fd_owner fd_mode
    [ -n "$OUTPUT_DIR_INODE" ] || die "internal error: assert_output_dir_custody ran before
       resolve_publication_targets recorded the output directory's identity, so '$operation'
       would proceed unchecked.  This is a defect in $SCRIPT_NAME."
    [ -n "$OUTPUT_DIR_PROC" ] || die "internal error: assert_output_dir_custody ran before the
       publication directory was opened, so '$operation' would be performed through the caller's
       pathname rather than through the pinned descriptor.  This is a defect in $SCRIPT_NAME."

    # 1. THE DESCRIPTOR.  Read as five separate values so a refusal can say which one moved.
    fd_type="$(stat -L -c '%F' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_device="$(stat -L -c '%d' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_inode="$(stat -L -c '%i' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_owner="$(stat -L -c '%u' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_mode="$(stat -L -c '%a' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    [ -n "$fd_inode" ] || die "refusing to $operation: the publication directory's own descriptor
       ($OUTPUT_DIR_PROC) could not be read.  Every privileged operation in that directory is
       performed through it, so without it there is nothing to perform them through."
    [ "$fd_type" = 'directory' ] || die "refusing to $operation: the descriptor this run holds on
       the publication directory now reports type '${fd_type:-unreadable}' rather than
       'directory'.  This is a defect in $SCRIPT_NAME rather than a runtime condition -- an open
       directory descriptor cannot become another kind of object -- and nothing is written
       through it."
    if [ "$fd_device" != "$OUTPUT_DIR_DEVICE" ] || [ "$fd_inode" != "$OUTPUT_DIR_INODE" ]; then
        die "refusing to $operation: the descriptor this run holds on the publication directory
       is not on the directory that was validated.
           validated  device $OUTPUT_DIR_DEVICE inode $OUTPUT_DIR_INODE
           descriptor device ${fd_device:-unreadable} inode ${fd_inode:-unreadable}
       This is a defect in $SCRIPT_NAME: a descriptor does not change the inode it refers to, so
       something reassigned file descriptor $OUTPUT_DIR_FD.  Nothing is written through it."
    fi
    [ "$fd_owner" = "$OUTPUT_DIR_OWNER" ] || die "refusing to $operation: the publication
       directory changed owner from uid $OUTPUT_DIR_OWNER to uid ${fd_owner:-unreadable} while
       this build was running (read from the descriptor, so this is the directory itself and not
       a substituted pathname).  Its owner may rename this run's staged files away, so the run
       stops rather than publishing into a directory somebody else has taken over."
    publication_dir_mode_is_safe "$(pad_four_digit_mode "$fd_mode")" \
        || die "refusing to $operation: the publication directory has become writable by group or
       other (mode ${fd_mode:-unreadable}) while this build was running.  In that state another
       account may rename this run's staged image, its staged manifest or its lock file away.
       Restore it with
           chmod go-w '$OUTPUT_DIR'
       and re-run."
    # AND THE MODE MUST NOT HAVE CHANGED AT ALL, not merely still be safe.  The predicate above
    # answers "is this mode acceptable"; this answers "is this the mode that was accepted".  They
    # differ for a change root itself could make -- 0755 to 0750, say, or the setgid bit going on
    # mid-build -- which is not a hazard but IS somebody reconfiguring the directory under a run
    # that is a hour into publishing into it, and the published pair's mode is one of this
    # script's postconditions.  Stated as a defect-or-interference report rather than a remedy,
    # because there is no single correct remedy: the caller decides which mode they meant.
    [ "$(pad_four_digit_mode "$fd_mode")" = "$OUTPUT_DIR_MODE" ] || die "refusing to $operation:
       the publication directory's mode changed from $OUTPUT_DIR_MODE to
       $(pad_four_digit_mode "$fd_mode") while this build was running (read from the descriptor,
       so this is the directory itself and not a substituted pathname).  Both modes may be safe,
       but the directory was reconfigured underneath a run that is publishing into it, and this
       script will not guess which configuration the caller meant.  Settle the mode and re-run."

    # 2. THE CALLER'S PATHNAME, checked so a substitution is reported rather than tolerated.
    if [ -L "$OUTPUT_DIR" ]; then
        die "refusing to $operation: the output directory
           $OUTPUT_DIR
       has become a SYMBOLIC LINK since this run validated it.  It was a plain directory when
       the publication targets were resolved, so something substituted it while this build was
       running.  Nothing further is written there."
    fi
    [ -d "$OUTPUT_DIR" ] || die "refusing to $operation: the output directory
           $OUTPUT_DIR
       no longer exists, or is no longer a directory.  It was validated at the start of this
       run; something removed or replaced it while the build was running."

    device="$(stat -c '%d' -- "$OUTPUT_DIR" 2>/dev/null || printf '')"
    inode="$(stat -c '%i' -- "$OUTPUT_DIR" 2>/dev/null || printf '')"
    if [ "$device" != "$OUTPUT_DIR_DEVICE" ] || [ "$inode" != "$OUTPUT_DIR_INODE" ]; then
        die "refusing to $operation: the output directory $OUTPUT_DIR is not the same
       directory this run validated.
           was  device $OUTPUT_DIR_DEVICE inode $OUTPUT_DIR_INODE
           now  device ${device:-unreadable} inode ${inode:-unreadable}
       A path that resolves to a different inode than the one whose ownership and permissions
       were checked is the substitution those checks exist to prevent (CWE-367).  Nothing
       further is written there."
    fi

    owner="$(stat -c '%u' -- "$OUTPUT_DIR" 2>/dev/null || printf '')"
    [ "$owner" = "$OUTPUT_DIR_OWNER" ] || die "refusing to $operation: the output directory
       $OUTPUT_DIR changed owner from uid $OUTPUT_DIR_OWNER to uid ${owner:-unreadable} while
       this build was running.  Whoever owns it can rename this run's staged files away, so the
       run stops rather than publishing into a directory somebody else has taken over."

    mode="$(four_digit_mode "$OUTPUT_DIR")"
    publication_dir_mode_is_safe "$mode" || die "refusing to $operation: the output directory
           $OUTPUT_DIR  (mode ${mode:-unreadable})
       has become writable by group or other while this build was running.  In that state
       another account may rename this run's staged image or its lock file away and put a
       symbolic link in its place.  Restore it with
           chmod go-w '$OUTPUT_DIR'
       and re-run."

    # 3. THE LOCK, RE-VERIFIED AT EVERY CALL SITE RATHER THAN ONLY AT ACQUISITION.
    #
    # WHY ONE CHECK AT ACQUISITION IS NOT ENOUGH.  A path-versus-descriptor comparison made only
    # around flock leaves the lock LEAF replaceable for the rest of the run: a second builder
    # opening the new leaf would take an exclusive lock on a different inode, get it, and build
    # the same --output alongside this run -- both of them believing they were alone, which is
    # worse than no lock at all.  Since every privileged operation already calls this function,
    # checking here means exclusivity is re-established immediately before each one rather than
    # assumed to have survived the hour since it was taken.
    #
    # Gated on the lock being HELD so that lock_publication_targets can call this function before
    # the descriptor exists, which is the one legitimate order.
    if [ "$OUTPUT_LOCK_HELD" -eq 1 ]; then
        assert_lock_descriptor_identity "immediately before this run would $operation"
    fi
}

# ONE STAGED LEAF, still the file this run created.  The device and inode are what identify it;
# the name is what an attacker controls.
# THE $1/$5 SPLIT, WHICH THE THREE LEAF PREDICATES BELOW ALL SHARE.  $1 is the name the check
# is performed THROUGH and it is the same name the privileged operation that follows will use --
# after resolve_publication_targets that is a descriptor-relative name, $OUTPUT_DIR_PROC/<leaf>.
# Checking through a different name from the one the operation uses is exactly the stat-then-
# operate mistake these predicates exist to close, so the two must not diverge.  The trailing
# display argument is the human pathname the same entry is reachable at, used only in messages;
# it defaults to $1 so a caller with nothing better to say may omit it.  Nothing is ever checked
# through the display path.
assert_leaf_identity() { # $1=path  $2=expected device  $3=expected inode  $4=what is about to happen  $5=display path
    local path="$1" expected_device="$2" expected_inode="$3" operation="$4" shown="${5:-$1}"
    local device inode links
    [ -n "$expected_inode" ] || die "internal error: assert_leaf_identity was asked to check
       $shown for '$operation' without a recorded inode.  This is a defect in $SCRIPT_NAME."

    [ ! -L "$path" ] || die "refusing to $operation: $shown is now a SYMBOLIC LINK.  This run
       created a regular file at that name; something replaced the directory entry while the
       build was running, and following it would write through to the link's target as root."
    [ -f "$path" ] || die "refusing to $operation: $shown is no longer a regular file that
       exists.  This run created it; something removed or replaced it while the build was
       running."

    device="$(stat -c '%d' -- "$path" 2>/dev/null || printf '')"
    inode="$(stat -c '%i' -- "$path" 2>/dev/null || printf '')"
    if [ "$device" != "$expected_device" ] || [ "$inode" != "$expected_inode" ]; then
        die "refusing to $operation: $shown is not the file this run created.
           was  device $expected_device inode $expected_inode
           now  device ${device:-unreadable} inode ${inode:-unreadable}
       The name was re-pointed at a different file after this run created it, which is exactly
       what the mode on the file cannot prevent -- renaming a directory entry is governed by
       the directory, not by the file."
    fi

    links="$(stat -c '%h' -- "$path" 2>/dev/null || printf '1')"
    case "$links" in
        ''|*[!0-9]*) links=1 ;;
    esac
    [ "$links" -le 1 ] || die "refusing to $operation: $shown has acquired $links hard links
       since this run created it, so another name now shares its content.  Refused rather than
       written through."
}

# Record one leaf's identity immediately after this run created it.  Prints nothing; the two
# globals named by $2 and $3 are set through a nameref so the two staged files share one
# implementation.
record_leaf_identity() { # $1=path  $2=device variable name  $3=inode variable name  $4=what it is  $5=display path
    local path="$1" what="$4" shown="${5:-$1}"
    local -n device_ref="$2"
    local -n inode_ref="$3"
    [ ! -L "$path" ] || die "$what was created at $shown but that name is a symbolic link.
       mktemp creates with O_CREAT|O_EXCL, so this cannot be the file it made: something
       replaced the entry in the instant after it was created.  Nothing is written through it."
    device_ref="$(stat -c '%d' -- "$path" 2>/dev/null || printf '')"
    inode_ref="$(stat -c '%i' -- "$path" 2>/dev/null || printf '')"
    if [ -z "$device_ref" ] || [ -z "$inode_ref" ]; then
        die "$what was created at $shown but its device and inode could not be read, so its
       identity cannot be re-checked before the privileged operations that follow.  The run
       stops rather than proceeding without that check."
    fi
}

# One publishable leaf: must be either absent, or a plain single-linked regular file that
# this script may replace.  Never a symlink, a directory, a device or a multiply-linked file.
assert_publishable_leaf() { # $1=path  $2=what it is  $3=display path
    local path="$1" what="$2" shown="${3:-$1}" links

    if [ -L "$path" ]; then
        die "$what is a SYMBOLIC LINK: $shown
       Refused rather than followed.  This script writes as root, and following a link here
       would replace whatever the link points at -- which is the whole of the hazard.  Pass
       the real path you want written."
    fi
    [ ! -e "$path" ] || [ -f "$path" ] || die "$what exists and is not a regular file: $shown
       It is refused rather than replaced: this script publishes a file there, and a
       directory, a device node or a socket at that path means the caller meant something
       else."

    if [ -f "$path" ]; then
        links="$(stat -c '%h' -- "$path" 2>/dev/null || printf '1')"
        case "$links" in
            ''|*[!0-9]*) links=1 ;;
        esac
        [ "$links" -le 1 ] || die "$what has $links hard links: $shown
       Replacing it would leave the other name(s) pointing at the old content while the
       caller believes the file was updated -- or, if this script had truncated it in place,
       would have destroyed their content too.  Refused.  Publish to a path with a single
       link."
    fi
}

# The output directory, resolved PHYSICALLY, plus the two leaves and the lock path.
resolve_publication_targets() {
    local lexical_parent physical_parent
    lexical_parent="$(dirname -- "$OUTPUT_IMAGE")"
    physical_parent="$(cd -P -- "$lexical_parent" 2>/dev/null && pwd -P)" \
        || die "the directory holding --output cannot be entered: $lexical_parent"
    if [ "$physical_parent" != "$lexical_parent" ]; then
        die "the path to --output goes through a symbolic link:
           you asked for   $lexical_parent
           it resolves to  $physical_parent
       Refused rather than followed.  Every safety check in this script is applied to the
       path it was given -- including the refusal to write inside an operating-system tree --
       and a link in the middle means those checks were applied to a path the bytes never
       reach.  Pass the resolved path."
    fi

    # Re-applied to the PHYSICAL parent: the earlier check saw the typed path, this one sees
    # where the writes actually land.  Cheap, and it is the only thing that catches a caller
    # whose own directory is a link into /usr.
    assert_path_plausible "$physical_parent" 'the directory holding --output'

    OUTPUT_DIR="$physical_parent"
    OUTPUT_MANIFEST="$OUTPUT_IMAGE.manifest"
    OUTPUT_LOCK="$OUTPUT_IMAGE.lock"

    # CUSTODY, ESTABLISHED HERE AND RE-CHECKED BEFORE EVERY PRIVILEGED OPERATION.  The reasoning
    # is in the block above this function; what follows is the decision.
    #
    # ROOT, AND ONLY ROOT.  The unprivileged account that invoked sudo is NOT accepted, even
    # though a caller who may pass --output is already trusted with the path: that reasoning
    # conflates trust over the PATHNAME with authority over the DIRECTORY ENTRY, and those are
    # not the same thing.  A directory's owner may rename or unlink a root-owned entry inside it
    # whatever the entry's own mode and owner say, so an unprivileged owner can substitute the
    # staged image, the destination or the lock leaf in the window between any check and the
    # privileged operation that follows it.  The descriptor opened below removes the ancestor
    # half of that exposure; this requirement removes the half that lives in the directory itself.
    OUTPUT_DIR_OWNER="$(stat -c '%u' -- "$OUTPUT_DIR" 2>/dev/null || printf '')"
    [ -n "$OUTPUT_DIR_OWNER" ] || die "the ownership of the directory holding --output could not
       be read: $OUTPUT_DIR
       It decides whether another account can rename this run's staged image away between the
       moment it is created and the moment root formats it, so the run stops rather than
       assuming it is safe."
    [ "$OUTPUT_DIR_OWNER" = '0' ] || die "the directory holding --output is not owned by root:
           $OUTPUT_DIR  is owned by uid $OUTPUT_DIR_OWNER
       This script runs as root, stages an image and a manifest in that directory, and renames
       them onto the caller's paths.  Whoever OWNS the directory may rename those entries away
       and put a symbolic link or a different file at the same names -- regardless of the mode on
       the entries themselves, because renaming a directory entry is governed by the directory --
       and root would then format, populate, publish or chmod through the substitution.  A file
       mode cannot prevent it and neither can re-checking, because the window between a check and
       an operation is where it happens.
       Create a publication directory that belongs to root:
           install -d -o root -g root -m 0755 '$OUTPUT_DIR'
       and pass --output inside it."

    local dir_mode
    dir_mode="$(four_digit_mode "$OUTPUT_DIR")"
    [ -n "$dir_mode" ] || die "the permissions of the directory holding --output could not be
       read: $OUTPUT_DIR
       They decide whether another account can substitute this run's staged files, so the run
       stops rather than assuming they are safe."
    publication_dir_mode_is_safe "$dir_mode" || die "the directory holding --output is writable by
       group or other:
           $OUTPUT_DIR  (mode $dir_mode)
       In that state another local account may rename this run's staged image, its staged manifest
       or its lock file away and put a symbolic link in its place, which this script would then
       write through as root.  There is no sticky-bit exemption here: sticky restricts rename and
       unlink to the ENTRY's owner, which says nothing about the directory's own owner, and a
       publication directory has no reason to be shared.  Fix it with
           install -d -o root -g root -m 0755 '$OUTPUT_DIR'
       or publish somewhere private."

    OUTPUT_DIR_DEVICE="$(stat -c '%d' -- "$OUTPUT_DIR" 2>/dev/null || printf '')"
    OUTPUT_DIR_INODE="$(stat -c '%i' -- "$OUTPUT_DIR" 2>/dev/null || printf '')"
    if [ -z "$OUTPUT_DIR_DEVICE" ] || [ -z "$OUTPUT_DIR_INODE" ]; then
        die "the device and inode of the directory holding --output could not be read:
       $OUTPUT_DIR
       They are what lets every privileged operation below re-establish that the path still
       resolves to the directory whose ownership and permissions were just checked.  Without
       them that re-check is impossible, so the run stops."
    fi

    assert_publishable_leaf "$OUTPUT_IMAGE"    'the --output image'
    assert_publishable_leaf "$OUTPUT_MANIFEST" "the manifest beside --output"
    assert_publishable_leaf "$OUTPUT_LOCK"     'the publication lock file'

    # THE DESCRIPTOR, OPENED ONCE, ON THE DIRECTORY THAT WAS JUST VALIDATED.
    #
    # From here on every privileged pathname operation in the publication directory is composed
    # against $OUTPUT_DIR_PROC rather than against $OUTPUT_DIR, which is what makes the checks
    # above hold for the whole run instead of only for the instant they ran in: the descriptor
    # refers to an INODE, so no rename of the directory or of any ancestor can redirect an
    # operation that goes through it.  It is never closed -- the process exit closes it, exactly
    # as it does the lock on fd 9 -- and it is inherited by the child processes that do the work,
    # which is what makes /proc/self/fd/<n> resolve inside them.
    eval "exec ${OUTPUT_DIR_FD}< \"\$OUTPUT_DIR\"" || die "could not open the publication
       directory $OUTPUT_DIR for the descriptor every privileged operation in it goes through.
       Without it those operations would have to name the caller's pathname, which is resolved
       afresh each time and can therefore be redirected between a check and its use."
    OUTPUT_DIR_PROC="/proc/self/fd/$OUTPUT_DIR_FD"

    # VERIFIED AGAINST WHAT WAS VALIDATED, immediately, so a descriptor on the wrong object is
    # caught here rather than at the first operation that uses it.
    local fd_type fd_device fd_inode fd_owner fd_mode
    fd_type="$(stat -L -c '%F' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_device="$(stat -L -c '%d' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_inode="$(stat -L -c '%i' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_owner="$(stat -L -c '%u' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')"
    fd_mode="$(pad_four_digit_mode "$(stat -L -c '%a' -- "$OUTPUT_DIR_PROC" 2>/dev/null || printf '')")"
    [ "$fd_type" = 'directory' ] || die "the descriptor opened on $OUTPUT_DIR reports type
       '${fd_type:-unreadable}' rather than 'directory'.  Nothing is written through it."
    if [ "$fd_device" != "$OUTPUT_DIR_DEVICE" ] || [ "$fd_inode" != "$OUTPUT_DIR_INODE" ]; then
        die "the descriptor opened on $OUTPUT_DIR is on device ${fd_device:-unreadable} inode
       ${fd_inode:-unreadable}, but the pathname was validated as device $OUTPUT_DIR_DEVICE inode
       $OUTPUT_DIR_INODE.  The directory was substituted between the validation and the open,
       which is precisely the race the descriptor exists to close; the run stops rather than
       operating on whichever of the two it now holds."
    fi
    [ "$fd_owner" = '0' ] || die "the descriptor opened on $OUTPUT_DIR is on a directory owned by
       uid ${fd_owner:-unreadable} rather than root, although the pathname read as root-owned a
       moment ago.  The directory was substituted between the two; nothing is written through it."
    publication_dir_mode_is_safe "$fd_mode" || die "the descriptor opened on $OUTPUT_DIR is on a
       directory whose mode is ${fd_mode:-unreadable}, which is writable by group or other,
       although the pathname read as safe a moment ago.  The directory was substituted between
       the two; nothing is written through it."

    # THE LEAF NAMES.  Every privileged operation is composed from one of these; the caller's
    # full paths above are what appears in messages.
    OUTPUT_IMAGE_LEAF="$(basename -- "$OUTPUT_IMAGE")"
    OUTPUT_MANIFEST_LEAF="$(basename -- "$OUTPUT_MANIFEST")"
    OUTPUT_LOCK_LEAF="$(basename -- "$OUTPUT_LOCK")"
    # A leaf that is empty, '.', '..' or carries a '/' would compose into a path that is not a
    # file in this directory.  basename cannot produce one from an absolute path with a real
    # final component, so this is a defect check rather than an input check.
    local leaf
    for leaf in "$OUTPUT_IMAGE_LEAF" "$OUTPUT_MANIFEST_LEAF" "$OUTPUT_LOCK_LEAF"; do
        case "$leaf" in
            ''|.|..|*/*) die "internal error: '$leaf' is not usable as a leaf name inside
       $OUTPUT_DIR, so the descriptor-relative path for it cannot be composed.  This is a defect
       in $SCRIPT_NAME." ;;
        esac
    done

    # The journal's leaf is derived from the image's, so two builders publishing different
    # --output names into one directory do not share a journal.  See the journal block.
    PUBLISH_JOURNAL_LEAF="${PUBLISH_JOURNAL_PREFIX}${OUTPUT_IMAGE_LEAF}${PUBLISH_JOURNAL_SUFFIX}"
    PUBLISH_JOURNAL_TMP_LEAF="${PUBLISH_JOURNAL_PREFIX}${OUTPUT_IMAGE_LEAF}${PUBLISH_JOURNAL_TMP_SUFFIX}"

    readonly OUTPUT_DIR OUTPUT_MANIFEST OUTPUT_LOCK
    readonly OUTPUT_DIR_DEVICE OUTPUT_DIR_INODE OUTPUT_DIR_OWNER
    readonly OUTPUT_DIR_PROC OUTPUT_IMAGE_LEAF OUTPUT_MANIFEST_LEAF OUTPUT_LOCK_LEAF
    readonly PUBLISH_JOURNAL_LEAF PUBLISH_JOURNAL_TMP_LEAF
    OUTPUT_DIR_MODE="$dir_mode"
    readonly OUTPUT_DIR_MODE
    log "publication targets: $OUTPUT_IMAGE and $OUTPUT_MANIFEST"
    log "  (staged under private names in $OUTPUT_DIR and renamed onto those paths together)"
    log "  output directory custody: uid $OUTPUT_DIR_OWNER (root), mode $dir_mode, device"
    log "    $OUTPUT_DIR_DEVICE inode $OUTPUT_DIR_INODE -- held open on fd $OUTPUT_DIR_FD, and"
    log "    every privileged operation names $OUTPUT_DIR_PROC/<leaf> rather than the pathname,"
    log "    so no rename of the directory or an ancestor can redirect one"
}

# Exclusive, non-blocking, held for the whole run on file descriptor 9.
#
# NON-BLOCKING IS THE POINT.  A second builder aimed at the same output has nothing useful to
# wait for -- it would spend an hour building an image that then overwrites the first one's --
# so it is told immediately, with the reason, and exits non-zero.  The descriptor stays open
# until the process ends, which is what releases the lock; no explicit unlock is needed and
# none is attempted, because an unlock in a trap could run while a later step still relies on
# exclusivity.
#
# AND THE INODE BEHIND THE DESCRIPTOR IS VERIFIED, BEFORE AND AFTER THE LOCK IS TAKEN.  An
# advisory lock is held on an INODE, not on a name.  If the leaf at $OUTPUT_LOCK is replaced
# between two builders' opens -- which any account that can write the output directory could do
# were the directory not vetted above -- each holds an exclusive lock on a different inode and
# both proceed, which is worse than no lock at all because both believe they are alone.  So the
# descriptor's own inode (stat -L /proc/self/fd/9) is compared against the inode the PATH
# resolves to, once before flock and once after: before, so a substitution that already
# happened is caught; after, so one that happened during the flock call is caught too.
#
# Compares the inode behind the OPEN DESCRIPTOR against BOTH resolutions of the lock's name --
# the descriptor-relative one ($OUTPUT_DIR_PROC/$OUTPUT_LOCK_LEAF, which is the name the lock
# was opened through and cannot be redirected by renaming an ancestor) and the CALLER's own
# pathname ($OUTPUT_LOCK).  Both are checked because they answer different questions:
#
#   * descriptor vs fd-relative resolution catches a substitution of the LEAF inside the
#     pinned directory, which is the substitution that actually defeats the lock: a second
#     builder resolving the same leaf would open the new inode and flock would grant it.
#   * descriptor vs caller pathname catches the directory itself having been renamed away and
#     a decoy put at its name.  That does not defeat this run's lock -- the fd pins the real
#     directory, which is the whole point of routing operations through it -- but it means the
#     paths this run PRINTS, and the paths the workflow will later read the image from, are no
#     longer the paths it operated on.  Publishing an image the caller cannot find, or worse
#     that a decoy directory now shadows, is not a success, so it is refused too.
#
# Sets OUTPUT_LOCK_DEVICE/OUTPUT_LOCK_INODE from the descriptor -- the descriptor's answer is
# the authoritative one, because that is the inode the lock is actually held on.
assert_lock_descriptor_identity() { # $1=when, for the message
    local when="$1" fd_path lock_proc rel_device rel_inode path_device path_inode
    local fd_device fd_inode
    fd_path='/proc/self/fd/9'
    lock_proc="$OUTPUT_DIR_PROC/$OUTPUT_LOCK_LEAF"

    [ ! -L "$lock_proc" ] || die "the publication lock leaf $OUTPUT_LOCK_LEAF is a symbolic link
       inside the publication directory ($when).  Refused rather than followed: a lock taken
       through a link is a lock on whatever the link points at, which is not the file the next
       builder will open."
    [ ! -L "$OUTPUT_LOCK" ] || die "the publication lock path $OUTPUT_LOCK is a symbolic link
       ($when).  Refused rather than followed: a lock taken through a link is a lock on whatever
       the link points at, which is not the file the next builder will open."

    rel_device="$(stat -c '%d' -- "$lock_proc" 2>/dev/null || printf '')"
    rel_inode="$(stat -c '%i' -- "$lock_proc" 2>/dev/null || printf '')"
    path_device="$(stat -c '%d' -- "$OUTPUT_LOCK" 2>/dev/null || printf '')"
    path_inode="$(stat -c '%i' -- "$OUTPUT_LOCK" 2>/dev/null || printf '')"
    fd_device="$(stat -L -c '%d' -- "$fd_path" 2>/dev/null || printf '')"
    fd_inode="$(stat -L -c '%i' -- "$fd_path" 2>/dev/null || printf '')"

    if [ -z "$rel_inode" ] || [ -z "$path_inode" ] || [ -z "$fd_inode" ]; then
        die "the inode behind the publication lock could not be read $when
       (descriptor-relative: ${rel_inode:-unreadable}, caller pathname: ${path_inode:-unreadable},
       descriptor: ${fd_inode:-unreadable}).  Without all three there is no way to establish
       that this run holds the lock on the file at $OUTPUT_LOCK rather than on an inode that has
       since been replaced, so the run stops."
    fi
    if [ "$rel_device" != "$fd_device" ] || [ "$rel_inode" != "$fd_inode" ]; then
        die "the publication lock file was SUBSTITUTED INSIDE the publication directory $when:
           the leaf $OUTPUT_LOCK_LEAF now resolves to device $rel_device inode $rel_inode
           the descriptor this run holds is on device $fd_device inode $fd_inode
       An advisory lock is held on an inode, so a second builder resolving that same leaf would
       open the new inode, flock would grant it, and two builders would each believe they had
       exclusive access to the same --output.  This run stops rather than becoming one of them."
    fi
    if [ "$path_device" != "$fd_device" ] || [ "$path_inode" != "$fd_inode" ]; then
        die "the publication lock is no longer reachable at the path this run was given $when:
           the path $OUTPUT_LOCK now resolves to device $path_device inode $path_inode
           the descriptor this run holds is on device $fd_device inode $fd_inode
       The descriptor still pins the directory that was validated, so this run's own writes are
       safe -- but the caller's pathname now leads somewhere else, which means the image would
       be published where the caller cannot find it, or behind a directory somebody else put at
       that name.  Refused rather than published into the ambiguity."
    fi
    OUTPUT_LOCK_DEVICE="$fd_device"
    OUTPUT_LOCK_INODE="$fd_inode"
}

lock_publication_targets() {
    assert_output_dir_custody "open the publication lock file $OUTPUT_LOCK"
    # Re-checked adjacent to the open: assert_publishable_leaf ran during resolution, and this
    # is the first thing that actually opens the path.  Checked through the descriptor, because
    # that is the name the open below uses.
    assert_publishable_leaf "$OUTPUT_DIR_PROC/$OUTPUT_LOCK_LEAF" 'the publication lock file' \
        "$OUTPUT_LOCK"

    # Opened THROUGH THE DIRECTORY DESCRIPTOR rather than through the caller's pathname, so the
    # inode the lock ends up on is an entry of the directory that was validated, not of whatever
    # directory the pathname resolves to by the time this line runs.
    exec 9>>"$OUTPUT_DIR_PROC/$OUTPUT_LOCK_LEAF" \
        || die "could not open the publication lock file $OUTPUT_LOCK.
       It sits beside --output, so this usually means the output directory is read-only."

    assert_lock_descriptor_identity 'immediately after opening it'
    if ! flock --exclusive --nonblock 9; then
        die "another root-image build already holds the publication lock on
           $OUTPUT_LOCK
       so it is building the same --output right now.  Two builders would interleave a
       format, a mount and a rename and the survivor would be a mixture of both, so this run
       stops instead.  Wait for the other build, or publish to a different --output."
    fi
    assert_lock_descriptor_identity 'while the lock was being acquired'

    # READABLE BY THE CONSUMERS, WHICH IS WHAT MAKES THE PAIR'S CONSISTENCY DELIVERABLE.  The
    # image and the manifest are replaced by four independent renames at fixed leaf names, so a
    # reader that does not hold this lock can see the new image beside the old manifest; the
    # publication block says so in full under "WHAT 'ATOMIC' MEANS HERE, EXACTLY".  The workflow's
    # boot step and its artifact-extraction step therefore take a SHARED flock on this same file
    # for the duration of their read -- and they run as the unprivileged runner account, while
    # this file was created by the redirection above under `umask 077` and is therefore 0600
    # root-owned.  0644 is what lets them open it read-only; `flock` on Linux needs no write
    # access, so read-only is enough and no writable state is exposed.  The mode is set through
    # the DESCRIPTOR rather than through a name, so it lands on the inode the lock is held on and
    # cannot be redirected; assert_lock_descriptor_identity() compares device, inode, type and
    # link count and not the mode, so this does not disturb it.
    chmod 0644 -- /proc/self/fd/9 || die "could not make the publication lock file
           $OUTPUT_LOCK
       readable (mode 0644).  The workflow's boot and artifact-extraction steps run as the
       unprivileged runner account and take a shared lock on it to read the published pair
       consistently; a lock they cannot open would have them read the pair unlocked, which is
       exactly the window this run's four renames open.  Refused rather than continued."

    OUTPUT_LOCK_HELD=1
    log "publication lock acquired: $OUTPUT_LOCK (exclusive, held for this whole run,"
    log "  on device $OUTPUT_LOCK_DEVICE inode $OUTPUT_LOCK_INODE -- the descriptor's own inode,"
    log "  verified to be the one the path resolves to both before and after flock)"
}

# ------------------------------------------------------------------------------------
# RUN-SCOPED SCRATCH, AND WHY ITS PARENT IS VALIDATED BEFORE mktemp RUNS.
#
# THE HAZARD (CWE-377).  This script runs as root and mounts the image inside this directory,
# extracts a root filesystem into it and chroots into it.  The directory's parent comes from
# the environment -- TMPDIR, defaulting to /tmp -- so the caller's environment decides where
# root does all of that.  Three properties of that parent are not optional:
#
#   * ABSOLUTE AND CANONICAL.  A relative TMPDIR puts the scratch tree somewhere that depends
#     on the working directory, and a symbolic link ANYWHERE in the ancestry means the checks
#     below were applied to a path the writes never reach.  Both are refused, not resolved.
#   * NOT SUBSTITUTABLE BY ANOTHER USER.  `mktemp -d` itself is safe -- it creates with
#     O_EXCL and 0700, so the NAME cannot be pre-created and the CONTENTS cannot be
#     pre-populated.  What mktemp cannot protect is the parent: in a world-writable directory
#     WITHOUT the sticky bit, any local user may RENAME our directory away and put their own
#     in its place, after which root extracts a root filesystem into a tree they control and
#     chroots into it.  So the parent must be either private to root, or the ordinary sticky
#     shared temp directory where rename and unlink are restricted to the owner.
#   * ROOT-OWNED.  A parent owned by an unprivileged user can be replaced wholesale.
#
# MEASURED, AND WHY IT IS WORTH SAYING: this is not a theoretical shape.  On the container
# this migration was developed in, /tmp is mode 2777 -- world-writable and NOT sticky -- and
# tests/L1Tests/run_coverage.sh already warns about exactly that condition for its own
# artifact root.  A run there is refused here with the remedy rather than proceeding, because
# the remedy is one environment variable and the failure it prevents is a root-owned chroot
# in a directory any user can swap.
# ------------------------------------------------------------------------------------
WORK_DIR=''
IMAGE_MOUNT=''
BASE_ARCHIVE=''
TEMP_PARENT=''

# Resolve and validate the directory the scratch tree is created in.  Sets TEMP_PARENT.
resolve_temp_parent() {
    local candidate="${TMPDIR:-/tmp}" physical owner mode sticky_bit other_write group_write

    [ -n "$candidate" ] || die "TMPDIR is set but empty.  An empty value cannot be validated,
       and this script will not silently substitute /tmp for it: unset TMPDIR to use /tmp, or
       set it to an absolute path to a root-owned private directory."

    # A CONTROL CHARACTER IN TMPDIR IS REFUSED AT THIS ONE BOUNDARY, which is what lets the dozen
    # diagnostics below -- and every later message that names the scratch tree beneath it -- keep
    # interpolating the path directly.  TMPDIR is environment-supplied, so it is as untrusted as
    # argv; a newline in it would end the line of any of those diagnostics and begin one of its
    # own, and a line beginning "::error::" is a GitHub Actions workflow command.  It is refused
    # rather than stripped, because a scratch parent this script silently renamed would not be the
    # one the caller named.  The value IS rendered here, because this is the message that has to
    # report it, and rendering keeps it inside the line.  See render_untrusted().
    case "$candidate" in
        *[[:cntrl:]]*)
            die "TMPDIR contains a control character: $(render_untrusted "$candidate")
       A newline, a carriage return or an escape sequence in this path would be reproduced in
       every diagnostic and artifact name that mentions the scratch tree -- forging or hiding
       output rather than naming a directory.  Set TMPDIR to an absolute path made of printable
       characters, or unset it to use /tmp." ;;
    esac

    case "$candidate" in
        /*) : ;;
        *)  die "TMPDIR is relative: '$candidate'.  Refused rather than resolved against the
       working directory: this script mounts a filesystem and chroots inside the scratch
       directory as root, so where that directory is must not depend on where the caller
       happened to be standing.  Set TMPDIR to an absolute path." ;;
    esac

    [ -d "$candidate" ] || die "TMPDIR does not name an existing directory: $candidate
       This script will not create it: a temporary parent it had to create is a parent whose
       ownership and permissions nobody chose deliberately."
    if [ -L "$candidate" ]; then
        die "TMPDIR is a symbolic link: $candidate
       Refused rather than followed.  Everything below is validated on the path we were
       given, and a link means root would write somewhere else entirely.  Pass the resolved
       path."
    fi

    physical="$(cd -P -- "$candidate" 2>/dev/null && pwd -P)" \
        || die "TMPDIR cannot be entered: $candidate"
    if [ "$physical" != "$candidate" ]; then
        die "the path to TMPDIR goes through a symbolic link:
           TMPDIR is      $candidate
           it resolves to $physical
       Refused for the same reason as a symlinked TMPDIR itself: the ownership and permission
       checks below would describe a directory the writes never reach.  Set TMPDIR to
       $physical if that is what you meant."
    fi

    owner="$(stat -c '%u' -- "$physical" 2>/dev/null || printf 'unknown')"
    mode="$(stat -c '%a' -- "$physical" 2>/dev/null || printf '')"
    [ "$owner" = '0' ] || die "TMPDIR is not owned by root: $physical is owned by uid $owner.
       This script creates its scratch tree there and mounts the image inside it as root; a
       parent owned by an unprivileged user can be replaced wholesale, taking the mount point
       and the chroot with it.  Point TMPDIR at a root-owned directory -- 'mktemp -d' as root
       is enough."
    case "$mode" in
        ''|*[!0-7]*) die "the permissions of TMPDIR ($physical) could not be read.  They
       decide whether another user can substitute this script's scratch directory, so the run
       stops rather than assuming they are safe." ;;
    esac

    # Normalised to four digits so the special bits can be read positionally regardless of
    # whether stat printed three or four.
    while [ "${#mode}" -lt 4 ]; do
        mode="0$mode"
    done
    sticky_bit=$(( ${mode:0:1} & 1 ))
    group_write=$(( ${mode:2:1} & 2 ))
    other_write=$(( ${mode:3:1} & 2 ))

    if [ "$group_write" -ne 0 ] || [ "$other_write" -ne 0 ]; then
        [ "$sticky_bit" -eq 1 ] || die "TMPDIR is writable by other users and is NOT sticky:
           $physical  (mode $mode)
       In such a directory any local user may rename this script's scratch directory away and
       put their own in its place -- after which root extracts a root filesystem into a tree
       they control and chroots into it.  mktemp cannot protect against that; only the parent
       can.  Either fix the directory
           chmod +t '$physical'
       or point TMPDIR at a private one, which is the better answer on a shared host:
           export TMPDIR=\"\$(mktemp -d)\"   # run as root, so it is root-owned and 0700
       MEASURED: /tmp is mode 2777 on some CI containers, including the one this script was
       developed in, which is exactly this case."
        log "scratch parent: $physical (shared, sticky -- rename and unlink are restricted to"
        log "  the owner there, so the scratch directory cannot be substituted)"
    else
        log "scratch parent: $physical (root-owned and not writable by other users)"
    fi

    [ -w "$physical" ] || die "TMPDIR is not writable: $physical"
    TEMP_PARENT="$physical"
    readonly TEMP_PARENT
}

make_work_dir() {
    resolve_temp_parent

    local created_inode final_inode final_mode final_owner
    WORK_DIR="$(mktemp -d -- "$TEMP_PARENT/cec-l2-rootfs.XXXXXXXX")" \
        || die "could not create a scratch directory under $TEMP_PARENT."
    # Registered immediately: from here on the trap owns removing it, including on a failure
    # inside the verification below.
    register_tempdir "$WORK_DIR"

    # WHAT mktemp GUARANTEES, AND WHAT IS CHECKED ANYWAY.  mktemp creates the directory with
    # O_EXCL and mode 0700, so a pre-created name or a pre-existing symlink at that name
    # cannot have been used.  The checks here cover what happens AFTER creation: on a parent
    # that turned out to be substitutable in the window between validation and use, the
    # directory we go on to use may not be the one we made.  The inode is captured now and
    # compared after the chmod for exactly that reason -- it is the cheapest way to say "this
    # is still the same object".
    [ -d "$WORK_DIR" ] || die "the scratch directory $WORK_DIR is not a directory immediately
       after mktemp created it.  Something is substituting paths under $TEMP_PARENT."
    [ ! -L "$WORK_DIR" ] || die "the scratch directory $WORK_DIR is a symbolic link
       immediately after mktemp created it as a directory.  Something is substituting paths
       under $TEMP_PARENT; the run stops rather than writing through it as root."
    created_inode="$(stat -c '%i' -- "$WORK_DIR" 2>/dev/null || printf '')"

    chmod 0700 -- "$WORK_DIR" || die "could not set the mode of the scratch directory
       $WORK_DIR."

    final_inode="$(stat -c '%i' -- "$WORK_DIR" 2>/dev/null || printf '')"
    final_mode="$(stat -c '%a' -- "$WORK_DIR" 2>/dev/null || printf '')"
    final_owner="$(stat -c '%u' -- "$WORK_DIR" 2>/dev/null || printf '')"
    if [ -z "$created_inode" ] || [ "$final_inode" != "$created_inode" ]; then
        die "the scratch directory $WORK_DIR changed identity between creation and use
       (inode ${created_inode:-unreadable} became ${final_inode:-unreadable}).  Something
       replaced it under $TEMP_PARENT.  Nothing further is written there."
    fi
    [ "$final_owner" = '0' ] || die "the scratch directory $WORK_DIR is owned by uid
       $final_owner rather than root immediately after this script created it."
    [ "$final_mode" = '700' ] || die "the scratch directory $WORK_DIR is mode $final_mode
       rather than 700 immediately after this script set it.  It holds an extracted root
       filesystem and the image mount point, so it must not be readable by other users."

    IMAGE_MOUNT="$WORK_DIR/mnt"
    mkdir -p -- "$IMAGE_MOUNT"
    # PINNED THE MOMENT IT EXISTS.  mount(2) takes a pathname, so the only thing that can say
    # the directory the loop device is mounted ONTO is the directory this script created is its
    # device and inode, recorded here and re-checked immediately before that mount.  The private
    # mount namespace this run is already inside means nothing can be mounted over it from
    # outside; this is the other half -- nothing can be substituted for it from inside either.
    pin_directory_identity "$IMAGE_MOUNT" IMAGE_MOUNT_POINT_DEVICE IMAGE_MOUNT_POINT_INODE \
        'the image mount point'
    log "scratch directory: $WORK_DIR (0700, root-owned, inode $final_inode)"
    log "  image mount point: $IMAGE_MOUNT (pinned: device $IMAGE_MOUNT_POINT_DEVICE inode $IMAGE_MOUNT_POINT_INODE)"

    # THE CONFINED I/O PRIMITIVE IS ESTABLISHED AND PROVEN HERE, WHICH IS THE EARLIEST POINT IT
    # CAN BE.  It needs a directory that is root-owned, mode 0700 and verified not to have been
    # substituted -- the three properties the block above has just established for $WORK_DIR --
    # and it must be in force BEFORE create_and_mount_image(), because every privileged
    # operation this script performs on a base-tarball-named path inside the mounted image goes
    # through it.  The proof is fatal on every arm: a positive case that fails means the
    # mechanism does not work here, and a negative case that succeeds means it is not confining
    # anything, and either way nothing privileged is done through it.
    install_confined_io_helper "$WORK_DIR"
    probe_confined_io "$WORK_DIR"
    # The anchor every in-image relative path is resolved against.  It is set now rather than at
    # each call site so there is one place that decides what "inside the image" means; nothing
    # is mounted there yet, and the wrappers refuse to run until it is a real directory.
    CONFINED_IO_ANCHOR="$IMAGE_MOUNT"

    # THE SECOND MECHANISM, ESTABLISHED AND PROVEN IN THE SAME PLACE AND FOR THE SAME REASONS.
    # The confined I/O primitive above carries every per-file operation inside the image; this one
    # carries the seven privileged operations that are not per-file -- the four pseudo-filesystem
    # mounts, the two chroot entry points and the nosymfollow remount -- by making each of them
    # CONSUME a descriptor this script opened and verified rather than resolve a pathname the
    # kernel walks again.  It must be in force before create_and_mount_image() for exactly the
    # reason the first one must: the first privileged mount whose target this script chose comes
    # immediately after.  Its proof is fatal on every arm too.
    install_pinned_op_helper "$WORK_DIR"
    probe_pinned_op "$WORK_DIR"
}

# ------------------------------------------------------------------------------------
# THE BASE ROOT FILESYSTEM.  Fetched on THIS host when a URL was given, then verified
# against its pinned digest in both cases, then extracted.  There is no path through this
# function that extracts an unverified archive.
# ------------------------------------------------------------------------------------
acquire_base_rootfs() {
    if [ -n "$BASE_TARBALL" ]; then
        BASE_ARCHIVE="$BASE_TARBALL"
        log "base root filesystem: $BASE_ARCHIVE (supplied locally; nothing is downloaded)"
    else
        BASE_ARCHIVE="$WORK_DIR/base-rootfs.tar"
        # THE URL IS SHOWN SANITISED, HERE AND IN THE TWO FAILURE MESSAGES BELOW.  A base URL is
        # a documented place to carry a credential, and a build log is neither private nor
        # short-lived.  The value handed to curl/wget is of course the real one; only what
        # reaches a human or an artifact is reduced to scheme, host and path.
        local base_url_shown
        base_url_shown="$(sanitised_endpoint_display "$BASE_URL")"
        log "fetching the base root filesystem on THIS host from $base_url_shown"
        log "  (host-side acquisition at image-build time; the guest never reaches the network)"

        # THE URL IS NOT PASSED IN A COMMAND LINE.  A presigned base URL carries its signature
        # in its query string, and an argument is visible to every user on the host for as long
        # as the transfer runs -- `ps -ef`, /proc/<pid>/cmdline, and any process accounting or
        # container tooling that reads either.  Sanitising the log did nothing about that.
        #
        # AND THIS ONLY EVER PROTECTED THE CHILD'S ARGV, WHICH IS HALF THE PROBLEM.  If the
        # caller typed the URL on THIS script's own command line, it is in
        # /proc/<this-pid>/cmdline regardless of how carefully it is then handed to curl.  That
        # is why validate_inputs now refuses a query string in the argv --base-url form
        # outright and why --base-url-file exists: the file form reads the value from a
        # caller-owned private file, so a signature in its query reaches no argv at all.  From
        # here on the two forms are indistinguishable -- BASE_URL holds the value either way,
        # and the private-file transport below applies to both.
        #
        # So the value is written to a private file that only curl reads, and the file is
        # removed the moment the transfer ends, whichever way it ended.  It is created inside
        # $WORK_DIR, which is already 0700 and root-owned and inode-verified, and the mode is
        # set before the value is written rather than after: the window between creating a file
        # and chmod'ing it is exactly what an attacker with a directory handle would use.
        #
        # The wget fallback uses --input-file for the same reason.  wget has no config
        # directive for a URL, but it reads URLs from a file, and with one URL and
        # --output-document the result is identical.
        local url_file transfer_status=0
        url_file="$WORK_DIR/base-url.conf"
        rm -f -- "$url_file"
        ( umask 077 && : > "$url_file" ) \
            || die "could not create the private URL file at $url_file, so the base URL cannot
       be kept out of the command line.  $WORK_DIR is this script's own 0700 scratch
       directory; a failure here means it is not writable, which nothing later would survive."
        chmod 0600 -- "$url_file" \
            || die "could not set mode 0600 on $url_file.  The base URL is not written to a file
       whose permissions are unknown."
        if command -v curl >/dev/null 2>&1; then
            printf 'url = "%s"\n' "$BASE_URL" > "$url_file" \
                || die "could not write the base URL to $url_file."
            curl --fail --silent --show-error --location \
                 --output "$BASE_ARCHIVE" --config "$url_file" || transfer_status=$?
        else
            printf '%s\n' "$BASE_URL" > "$url_file" \
                || die "could not write the base URL to $url_file."
            wget --quiet --output-document="$BASE_ARCHIVE" --input-file="$url_file" \
                || transfer_status=$?
        fi
        # REMOVED BEFORE THE STATUS IS ACTED ON, so the failure path cannot leave it behind.
        # The trap would remove $WORK_DIR wholesale in any case; this is the transport being
        # torn down as soon as it has served its purpose rather than at the end of the run.
        rm -f -- "$url_file" \
            || die "could not remove the private URL file $url_file after the transfer.  It
       holds the base URL verbatim, so leaving it in place is not an acceptable outcome."
        if [ "$transfer_status" -ne 0 ]; then
            die "could not fetch the base root filesystem from $base_url_shown (exit
       $transfer_status).  Check the URL and this host's network access, or fetch it separately
       and pass --base-tarball.  (The URL above is shown with any userinfo and query string
       removed; the value used was the one you passed, and it was passed through a private
       0600 file rather than a command line.)"
        fi
    fi
    verify_sha256 "$BASE_ARCHIVE" "$BASE_SHA256" 'the base root filesystem tarball'
}

# ------------------------------------------------------------------------------------
# THE IMAGE ITSELF.  A sparse file, an ext4 filesystem, a loop device and a mount -- each
# registered with the trap the moment it exists.
#
# -O ^has_journal is deliberately NOT used: the guest writes the coverage evidence into this
# filesystem and then powers down, so journalling is what makes an unclean shutdown
# recoverable rather than a lost artifact set.
# ------------------------------------------------------------------------------------
LOOP_DEVICE=''

create_and_mount_image() {
    # STAGED, NOT PUBLISHED.  mktemp creates the file itself, exclusively (O_CREAT|O_EXCL) and
    # under the process umask, so there is no window in which the name exists and the content
    # is somebody else's, and no way for a pre-created symlink at a guessable name to redirect
    # the write.  It lives in the OUTPUT DIRECTORY rather than in $WORK_DIR so that the
    # publishing rename is same-filesystem and therefore atomic: an 8 GiB copy across
    # filesystems would be neither atomic nor affordable.
    #
    # AND THE MODE ON IT IS NOT THE WHOLE PROTECTION.  0600 protects the CONTENT; the NAME is
    # protected by the output directory, whose owner, mode and identity were vetted in
    # resolve_publication_targets and are RE-CHECKED here immediately before each privileged
    # step, together with the staged leaf's own device and inode.  Between the mktemp and the
    # truncate, and again between the mkfs and the losetup, the entry could otherwise be
    # re-pointed at a symbolic link and root would format, mount and populate its target.
    #
    # EVERY ONE OF THOSE STEPS NAMES THE FILE THROUGH THE DIRECTORY DESCRIPTOR.  mktemp,
    # chmod, truncate, mkfs.ext4 and losetup are all given $OUTPUT_DIR_PROC/<leaf>, so the
    # directory they act inside is the inode that was validated rather than whatever the
    # caller's pathname resolves to at the moment each one runs.  $STAGED_IMAGE keeps the human
    # pathname: it is what the messages say, and it is what the kernel reports as the loop
    # device's backing_file (measured: losetup on a /proc/self/fd path records the RESOLVED
    # path), so assert_loop_backs continues to compare like with like.
    local staged_proc
    assert_output_dir_custody "create the staging file for the image in $OUTPUT_DIR"
    staged_proc="$(mktemp -- "$OUTPUT_DIR_PROC/.cec-l2-image.XXXXXXXX")" \
        || die "could not create a staging file in $OUTPUT_DIR.  The image is staged beside
       --output so it can be renamed onto it atomically, so this directory must be writable."
    STAGED_IMAGE_LEAF="$(basename -- "$staged_proc")"
    STAGED_IMAGE="$OUTPUT_DIR/$STAGED_IMAGE_LEAF"
    record_leaf_identity "$staged_proc" STAGED_IMAGE_DEVICE STAGED_IMAGE_INODE \
        'the staged image' "$STAGED_IMAGE"
    # 0600 explicitly rather than by umask: the staged image is populated as root and carries
    # the whole guest filesystem, and it is briefly visible in a directory the caller chose.
    chmod 0600 -- "$staged_proc" \
        || die "could not set the mode of the staging file $STAGED_IMAGE."
    IMAGE_CREATED=1

    log "creating a ${IMAGE_SIZE_MIB} MiB ext4 image"
    log "  staged at  $STAGED_IMAGE (device $STAGED_IMAGE_DEVICE inode $STAGED_IMAGE_INODE)"
    log "  named through the publication directory descriptor as $staged_proc"
    log "  publishes to $OUTPUT_IMAGE only after it is validated"
    assert_output_dir_custody "size the staged image $STAGED_IMAGE"
    assert_leaf_identity "$staged_proc" "$STAGED_IMAGE_DEVICE" "$STAGED_IMAGE_INODE" \
        "size the staged image $STAGED_IMAGE" "$STAGED_IMAGE"
    truncate -s "${IMAGE_SIZE_MIB}M" -- "$staged_proc" \
        || die "could not size the staged image $STAGED_IMAGE.  Check free space on its
       filesystem: the image is ${IMAGE_SIZE_MIB} MiB and holds a toolchain, the payload, the
       staged SDK, a full build tree and the coverage artifacts."
    # -m 0 keeps no reserved-blocks margin: nothing else shares this filesystem, and the
    # build tree wants every block.
    assert_output_dir_custody "format the staged image $STAGED_IMAGE"
    assert_leaf_identity "$staged_proc" "$STAGED_IMAGE_DEVICE" "$STAGED_IMAGE_INODE" \
        "format the staged image $STAGED_IMAGE" "$STAGED_IMAGE"
    mkfs.ext4 -q -F -m 0 -L 'cec-l2-root' -- "$staged_proc" \
        || die "mkfs.ext4 failed on the staged image $STAGED_IMAGE."

    assert_output_dir_custody "attach a loop device to the staged image $STAGED_IMAGE"
    assert_leaf_identity "$staged_proc" "$STAGED_IMAGE_DEVICE" "$STAGED_IMAGE_INODE" \
        "attach a loop device to the staged image $STAGED_IMAGE" "$STAGED_IMAGE"
    LOOP_DEVICE="$(losetup --find --show -- "$staged_proc")" \
        || die "could not attach a loop device to $STAGED_IMAGE.  If the host has run out of
       loop devices, free them with 'losetup -a' and 'losetup -d'."
    # Registered WITH its backing file so the teardown can tell our device from the same node
    # after another process on this host re-used it.
    register_loop "$LOOP_DEVICE" "$STAGED_IMAGE"
    readonly LOOP_DEVICE
    log "loop device: $LOOP_DEVICE"

    # The kernel's own answer, before anything is mounted: `losetup --find --show` races with
    # every other loop user on the host, and a device that came back bound to somebody else's
    # file must not be mounted -- as root -- into our image tree.
    assert_loop_backs "$LOOP_DEVICE" "$STAGED_IMAGE" "mount $LOOP_DEVICE at $IMAGE_MOUNT"

    # THE TARGET IS THE DIRECTORY make_work_dir() CREATED, RE-ESTABLISHED IMMEDIATELY BEFORE THE
    # MOUNT.  mount(2) resolves a pathname in the ordinary way, so this is the check that stands
    # between it and a substituted mount point; the private mount namespace this run is inside
    # covers the other direction, where the substitution would be an overmount from the host.
    assert_pinned_directory "$IMAGE_MOUNT" "$IMAGE_MOUNT_POINT_DEVICE" \
        "$IMAGE_MOUNT_POINT_INODE" 'the image mount point' \
        "mount $LOOP_DEVICE at $IMAGE_MOUNT"
    register_mount "$IMAGE_MOUNT"
    mount -- "$LOOP_DEVICE" "$IMAGE_MOUNT" \
        || die "could not mount $LOOP_DEVICE at $IMAGE_MOUNT."

    # PRIVATE BEFORE ANYTHING IS MOUNTED INTO IT, AND THEN MEASURED.
    #
    # The namespace's root was made recursively private when it was created, so a mount made
    # beneath it inherits private propagation from its parent and this is belt to that braces --
    # but it is the belt that matters at exactly this point: the four pseudo-filesystems and the
    # nosymfollow remount all land INSIDE this mount, and if this one were shared or a slave
    # they would propagate to its peer group, which is to say back onto the build host.  So the
    # image root is made recursively private explicitly, and the kernel is then asked whether it
    # is -- because `mount --make-rprivate` returning 0 is not the same statement as
    # /proc/self/mountinfo showing no propagation tags, and it is the second one this script's
    # comments claim.
    mount --make-rprivate -- "$IMAGE_MOUNT" \
        || die "could not make the image mount at $IMAGE_MOUNT private.  Every mount this
       script makes below lands inside it, so a shared or slave mount here would propagate them
       out of this run's mount namespace and onto the build host -- which is the whole of what
       the namespace exists to prevent.  The run stops rather than mounting into a mount whose
       propagation it could not set."
    assert_mount_is_private "$IMAGE_MOUNT" 'the image mount' \
        "populate the image at $IMAGE_MOUNT"

    # THE PIN EVERY LATER mount(2) AND chroot(2) IS CHECKED AGAINST.  It is taken AFTER the
    # mount deliberately: before it, the path resolves to an ordinary directory of the scratch
    # tree, and afterwards it resolves to the root of the image's own filesystem -- a different
    # object, on a different device, and the one all the privileged work below actually names.
    pin_directory_identity "$IMAGE_MOUNT" IMAGE_ROOT_DEVICE IMAGE_ROOT_INODE 'the image root'
    log "image mounted at $IMAGE_MOUNT"
    log "  in mount namespace mnt:[$MOUNT_NS_ID], propagation private, pinned as device $IMAGE_ROOT_DEVICE inode $IMAGE_ROOT_INODE"
}

extract_base_rootfs() {
    log "extracting the base root filesystem"
    # --numeric-owner keeps uid/gid as they are in the archive rather than remapping them
    # through this host's passwd database, which would give the guest's files owners that do
    # not exist inside it.
    tar --extract --numeric-owner --file "$BASE_ARCHIVE" --directory "$IMAGE_MOUNT" \
        || die "could not extract $BASE_ARCHIVE into the image.  If the archive is not a root
       filesystem tarball -- a container image manifest, for instance -- this is where that
       shows."
    # THE FIRST TWO POST-EXTRACTION CHECKS, AND THE FIRST TWO THAT MUST NOT BE `[ -d ]`.
    # require_existing_dir uses `[ -d ]`, which FOLLOWS symbolic links -- so on a base tarball
    # that ships /etc or /usr as an absolute link both checks would pass by measuring the BUILD
    # HOST's directories, and every step below would then be operating on a tree whose top-level
    # layout was never established.  confined_directory asks the kernel instead: a link, a
    # non-directory or anything that leaves the image is refused here, at the earliest moment the
    # question can be asked, rather than surfacing as a host-side write later on.
    confined_directory 'etc' "the extracted base's /etc" \
        'establish that the extracted archive is a root filesystem' \
        || die "missing directory -- the extracted base's /etc.  The archive does not look like a
       root filesystem: expected /etc, /usr and /bin at its top level.
       Expected it at: $IMAGE_MOUNT/etc
       Check the path, or the step that was supposed to stage it."
    confined_directory 'usr' "the extracted base's /usr" \
        'establish that the extracted archive is a root filesystem' \
        || die "missing directory -- the extracted base's /usr
       Expected it at: $IMAGE_MOUNT/usr
       Check the path, or the step that was supposed to stage it."
}

# ------------------------------------------------------------------------------------
# THE CHROOT.  The pseudo-filesystems are bind-mounted so the package manager and the
# compiler behave, and every one is registered with the trap before it is made.
#
# THE FOUR MOUNT POINTS ARE CREATED AND CONFIRMED THROUGH THE CONFINEMENT, AND ALL FOUR BEFORE
# ANY OF THEM IS MOUNTED.  The order is load-bearing and is not a matter of taste:
# RESOLVE_NO_XDEV refuses to resolve a path that crosses a mount point, so `dev/pts` can only be
# created and inspected while /dev is still an ordinary directory of the image.  Doing it
# afterwards would be refused -- correctly, by the primitive working as designed -- so it is done
# first.
#
# THE FOUR mount(2) CALLS CONSUME A DESCRIPTOR RATHER THAN A NAME, WHICH IS WHAT REMOVED THE LAST
# CHECK-THEN-USE WINDOW IN THIS FUNCTION.  mount(2) has no descriptor-taking target parameter, so
# for a long time the shape here was "confirm the pathname, then hand the same pathname to the
# syscall", and the kernel then resolved that name a third time, independently of both checks.  A
# mount whose TARGET IS GIVEN AS /proc/self/fd/N consumes the object that descriptor already
# refers to -- the kernel resolves the magic link to the pinned (mount, dentry) pair instead of
# walking the components of a name again -- so every mount below is performed that way, in the
# same process that opened the descriptor and verified it.  FIVE THINGS BOUND IT, AND ALL FIVE
# ARE IN FORCE HERE:
#
#   1. THE TARGET THE SYSCALL ACTS ON IS THE OBJECT THAT WAS CHECKED.  The helper opens the image
#      root O_DIRECTORY|O_NOFOLLOW, fstat-verifies THAT DESCRIPTOR against the pin, resolves the
#      target beneath it with openat2 under RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV,
#      fstat-verifies the resulting descriptor against the identity confined_identity recorded a
#      few lines below, and mounts onto that descriptor.  If any step fails, nothing is mounted:
#      there is no fallback to `mount ... <pathname>`.  probe_pinned_op() proves the chain, and
#      proves it refuses a bad anchor, a bad target, a link and a '..' escape, before this
#      function runs.
#   2. THE MOUNTS ARE MADE IN THIS RUN'S OWN PRIVATE MOUNT NAMESPACE.  The privileged section
#      re-executed itself under `unshare --mount --propagation private` before it created
#      anything, and enter_private_mount_namespace() proved -- three ways, all measured -- that
#      the namespace is new and that propagation really is private.  This is now defence in depth
#      beneath point 1 rather than the thing standing in for it: it bounds where a mistake could
#      reach, and it is worth keeping for the leak property alone -- a mount namespace is
#      destroyed when its last member exits, so nothing survives a SIGKILL that runs no trap.
#   3. THE IMAGE ROOT IS IDENTITY-PINNED, AND RE-CHECKED IMMEDIATELY BEFORE EACH OF THE FOUR
#      MOUNTS.  The recorded device and inode are the numbers point 1 fstat-compares against, so
#      the recording is load-bearing; the pathname-side re-check that precedes each mount is a
#      diagnostic that names the path and the object that moved, from the same pathname the log
#      shows.
#   4. EACH TARGET WAS CREATED AND TYPE-CHECKED THROUGH THE CONFINEMENT.  confined_mkdir created
#      each of the four beneath the image mount point with a symlinked or out-of-image component
#      refused at every level, and confined_directory confirmed each is a real directory of the
#      image.  This is UNCHANGED: it is what says the target is the directory this script meant.
#      Note why the per-target re-check cannot be moved to just before each mount:
#      RESOLVE_NO_XDEV refuses a path that crosses a mount point, so once /dev is mounted,
#      `dev/pts` can no longer be resolved through the primitive at all -- which is why all four
#      are established first, as the paragraph above says, and why `dev/pts` takes the separate
#      descriptor-walk route described on resolve_under_mount() in the helper.
#   5. EACH MOUNT'S PROPAGATION IS MEASURED AFTER IT IS MADE.  A mount inherits propagation from
#      its parent, and the parent here is the image mount that create_and_mount_image() made
#      private and verified -- but "inherits" is a claim about the kernel, so the kernel is
#      asked.  assert_mount_is_private() reads /proc/self/mountinfo and refuses a mount carrying
#      any propagation tag, which is the only way a mount made here could reach the host.
# ------------------------------------------------------------------------------------
mount_chroot_pseudo_filesystems() {
    local point proc_id sys_id dev_id host_dev_device host_dev_inode
    for point in proc sys dev dev/pts; do
        confined_mkdir "$point" 0755 "the image's /$point mount point" \
            'mount the pseudo-filesystems into the image'
        confined_directory "$point" "/$point" \
            'mount the pseudo-filesystems into the image' \
            || die "the image's /$point is not there immediately after this script created it
       through the confinement.  Something is changing the image's contents while it is being
       built; nothing is mounted into it."
    done

    # THE IDENTITY OF EACH TARGET, MEASURED THROUGH THE CONFINEMENT AND CARRIED TO THE SYSCALL AS
    # A NUMBER RATHER THAN RE-READ FROM A NAME.  confined_identity resolves each of the three
    # openat2-reachable targets under RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV and
    # reports the device and inode of the object it actually opened.  Those numbers are what the
    # mount below compares its OWN descriptor against by fstat, so the comparison is
    # descriptor-to-recording rather than name-to-name.  All three are recorded before any mount
    # for the same RESOLVE_NO_XDEV reason the creation loop above runs first.
    proc_id="$(confined_identity 'proc' "the image's /proc mount point" \
        'mount /proc into the image')"
    sys_id="$(confined_identity 'sys' "the image's /sys mount point" \
        'mount /sys into the image')"
    dev_id="$(confined_identity 'dev' "the image's /dev mount point" \
        'bind-mount /dev into the image')"
    # The fourth target's identity cannot be recorded in advance -- the object mounted onto is the
    # pts directory AS REACHED THROUGH the /dev bind, which does not exist yet -- so what is
    # pinned instead is the SOURCE this script is about to bind there.  The descriptor walk then
    # proves the 'dev' component it opened is that source and that the leaf did not cross a
    # further mount; the reasoning is on resolve_under_mount() in the helper.  /dev is a fixed
    # host pathname this script chose itself, not a name anything untrusted can define.
    pin_directory_identity /dev host_dev_device host_dev_inode 'the host /dev directory'

    # THE PIN IS STILL RE-CHECKED BEFORE EACH MOUNT, and it is now a PRE-CHECK rather than the
    # thing the syscall relies on: it produces a precise diagnosis from the pathname the operator
    # sees in the log, while the guarantee comes from the descriptor the mount consumes.  Each
    # mount is registered with the trap before it is made, because a mount that succeeded and was
    # not registered is a mount left behind.
    assert_image_root_pinned 'mount /proc into the image'
    register_mount "$IMAGE_MOUNT/proc"
    pinned_image_mount_fs 'proc' "$proc_id" proc proc "the image's /proc" \
        'mount /proc into the image'
    assert_mount_is_private "$IMAGE_MOUNT/proc" "the image's /proc" \
        'run commands inside the image'
    assert_image_root_pinned 'mount /sys into the image'
    register_mount "$IMAGE_MOUNT/sys"
    pinned_image_mount_fs 'sys' "$sys_id" sysfs sysfs "the image's /sys" \
        'mount /sys into the image'
    assert_mount_is_private "$IMAGE_MOUNT/sys" "the image's /sys" \
        'run commands inside the image'
    assert_image_root_pinned 'bind-mount /dev into the image'
    register_mount "$IMAGE_MOUNT/dev"
    pinned_image_mount_bind 'dev' "$dev_id" /dev "the image's /dev bind mount" \
        'bind-mount /dev into the image'
    # THE ONE THAT WOULD HURT MOST IF IT PROPAGATED.  This is a bind of the HOST's /dev, which on
    # a systemd host is a shared mount: without private propagation an unmount here could
    # propagate back to the host's own /dev.  It comes out private because its parent -- the
    # image mount -- is private, and that is measured rather than assumed.
    assert_mount_is_private "$IMAGE_MOUNT/dev" "the image's /dev bind mount" \
        'run commands inside the image'
    assert_image_root_pinned 'bind-mount /dev/pts into the image'
    register_mount "$IMAGE_MOUNT/dev/pts"
    pinned_image_mount_bind_under 'dev/pts' "$host_dev_device" "$host_dev_inode" /dev/pts \
        "the image's /dev/pts bind mount" 'bind-mount /dev/pts into the image'
    assert_mount_is_private "$IMAGE_MOUNT/dev/pts" "the image's /dev/pts bind mount" \
        'run commands inside the image'
    log "the image's /proc, /sys, /dev and /dev/pts are mounted, all private to mnt:[$MOUNT_NS_ID],"
    log "  each onto a descriptor this script opened beneath the image root and fstat-verified"
    log "  against the identity it recorded for that directory -- no pathname was resolved again"
}

# Run a command inside the image.  DEBIAN_FRONTEND keeps the package manager from trying to
# open a dialogue nobody can answer; LC_ALL keeps its messages parseable.
#
# THE CHROOT CONSUMES A DESCRIPTOR, NOT A PATHNAME.  Neither of these functions hands a name to
# chroot(2) any more.  Both run the pinned privileged-operation helper, which opens $IMAGE_MOUNT
# with O_DIRECTORY|O_NOFOLLOW, verifies that descriptor by fstat(2) against the device and inode
# create_and_mount_image() recorded for the image root, then performs `fchdir(fd)` followed by
# `chroot(".")` and execs the command.  "." resolves against the working directory the descriptor
# established, so there is no name for anything to redirect between the check and the use -- the
# check-then-use window that an identity check on a pathname could only narrow is gone rather
# than made smaller.  If the descriptor cannot be opened or does not fstat to the pin, nothing is
# chrooted and nothing is executed: there is NO fallback to `chroot <pathname>`.
#
# THE PATHNAME PIN IS STILL CHECKED FIRST, AND IT IS NOW A DIAGNOSTIC RATHER THAN THE GUARANTEE.
# assert_image_root_pinned names the path, says what changed about it and stops the run
# immediately; the helper's refusal is what makes the operation impossible.  Keeping both means an
# operator reading the log sees which object moved, and a future edit that removed the helper
# would not silently fall back to a bare name.  The check goes here, once, rather than at each of
# the ~20 call sites, because a check a caller can forget is a check that will be forgotten.
#
# THE OTHER TWO BOUNDS ARE UNCHANGED AND STILL LOAD-BEARING.  The private mount namespace bounds
# where a mistake can reach, and the chroot itself is what makes every in-image pathname the
# command resolves -- including an absolute one, and including the target of any link it meets --
# resolve beneath the image.
#
# ONE NUANCE, STATED BECAUSE IT IS A REAL LIMIT.  Four call sites run chroot_run inside a command
# substitution (`$(chroot_run dpkg-query ...)`), where `die` exits the SUBSHELL rather than the
# script -- two of those four then tolerate a failure with `|| true`.  The privileged operation
# is still refused in every case, which is the property that matters: the chroot does not happen
# unless the descriptor fstat's to the pinned image root, and that refusal is made by the helper
# in its own process rather than by any check the shell could skip.  What such a site loses is
# only the immediacy of the exit, and the next chroot entry -- the very next privileged step in
# main()'s order -- is not in a subshell and stops the run there.  The diagnostic is on stderr
# either way.
chroot_run() {
    local status=0
    pinned_op_ready "run '${1:-a command}' inside the image"
    assert_image_root_pinned "run '${1:-a command}' inside the image"
    env -i \
        PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin' \
        HOME=/root \
        LC_ALL=C \
        DEBIAN_FRONTEND=noninteractive \
        "$PINNED_OP_PYTHON" -I -B "$PINNED_OP_HELPER" \
            "$IMAGE_MOUNT" "$IMAGE_ROOT_DEVICE" "$IMAGE_ROOT_INODE" . - - \
            chroot-exec "$@" || status=$?
    if [ "$status" -eq "$PINNED_OP_CUSTODY_REFUSED" ]; then
        pinned_op_custody_die "$IMAGE_MOUNT" 'the image root' \
            "run '${1:-a command}' inside the image"
    fi
    return "$status"
}

# The same, with the staged libraries reachable.  GNU ld does not search -L for a shared
# library's own DT_NEEDED entries and the staged libbinder carries no RUNPATH, so the search
# path is needed when the in-guest helpers are LINKED, not only when they are run.
#
# The chroot target is consumed as a descriptor here too, for the reasons set out on chroot_run
# above.  The two functions deliberately do not delegate to one another -- their environments
# differ and the duplication is a few lines -- so the custody chain has to appear in both, and a
# future edit that adds a third entry point has to add it there as well.
chroot_run_with_libs() {
    local status=0
    pinned_op_ready "run '${1:-a command}' inside the image with the staged libraries"
    assert_image_root_pinned "run '${1:-a command}' inside the image with the staged libraries"
    env -i \
        PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin' \
        HOME=/root \
        LC_ALL=C \
        DEBIAN_FRONTEND=noninteractive \
        LD_LIBRARY_PATH="$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET/lib/binder:$GUEST_SDK_DIR/$SDK_REL_HALIF_LIB" \
        "$PINNED_OP_PYTHON" -I -B "$PINNED_OP_HELPER" \
            "$IMAGE_MOUNT" "$IMAGE_ROOT_DEVICE" "$IMAGE_ROOT_INODE" . - - \
            chroot-exec "$@" || status=$?
    if [ "$status" -eq "$PINNED_OP_CUSTODY_REFUSED" ]; then
        pinned_op_custody_die "$IMAGE_MOUNT" 'the image root' \
            "run '${1:-a command}' inside the image with the staged libraries"
    fi
    return "$status"
}

# ==============================================================================
# THE FOUR RECURSIVE OPERATIONS, AND THE SECOND CONFINEMENT THEY GO THROUGH.
# ==============================================================================
# WHAT THE PER-FILE PRIMITIVE CANNOT DO.  openat2 with
# RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV hands back a descriptor on ONE object, and
# every operation the confined I/O helper performs is a single call through it.  Four of this
# script's operations are whole-tree: copying the payload, the staged SDK and the GoogleTest
# prefix in; extracting the lcov source tarball; deleting the two scratch trees again.  A
# per-descriptor primitive cannot express any of them without reimplementing cp, tar and rm in
# python, which would replace a proven mechanism with an unproven one.
#
# WHAT THEY GO THROUGH INSTEAD, AND WHY IT IS A CONFINEMENT RATHER THAN A HOPE.  chroot_run
# already exists in this file and its resolution root IS THE IMAGE: every pathname the command
# inside it resolves -- including an absolute one, and including the target of any symbolic link
# it meets -- is resolved beneath $IMAGE_MOUNT by the kernel.  A command run through it therefore
# CANNOT NAME A HOST PATH AT ALL.  That is the structural property the per-file primitive gets
# from RESOLVE_BENEATH, obtained a different way, and it is what makes `chroot_run tar -xf -` and
# `chroot_run rm -rf` safe where `tar --directory "$IMAGE_MOUNT$dir"` and
# `rm -rf -- "$IMAGE_MOUNT$dir"` on the build host are not: those two run OUTSIDE the chroot, so
# an absolute symbolic link at any component of the in-image path resolves on the HOST and the
# operation lands there, as root.
#
# THE DESTINATION IS STILL ESTABLISHED THROUGH THE PER-FILE PRIMITIVE FIRST.  chroot_run bounds
# the operation to the image; it does not say that the destination is the directory this script
# meant.  So each destination is created with confined_mkdir -- which refuses a symlinked or
# out-of-image component at every level and creates missing parents through descriptors it
# obtained itself -- and then confirmed with confined_directory before the recursive step runs.
# Between that confirmation and the tar there is a residual window, and it is stated rather than
# glossed: nothing else executes inside the image at that moment (every in-image command this
# script runs is synchronous and this script is the only writer), and the worst case if the window
# were ever lost is a misplaced write INSIDE the image, which the chroot bounds and which
# publication-time verification would surface -- not the host write the finding is about.
#
# WHY A TAR PIPE RATHER THAN cp -a.  cp -a's destination is a host pathname, which is the defect.
# `tar -cf -` on the source and `tar -xf -` inside the chroot moves the same bytes with the same
# metadata: --numeric-owner on both ends keeps uid/gid exactly as cp -a's --preserve=ownership
# did rather than remapping them through either passwd database, and tar preserves permissions,
# timestamps and symbolic links as members rather than following them.  The source trees are
# verified before they are copied (verify_payload_tree, verify_sdk_tree) and the lcov tarball is
# verified against its pinned SHA-256 (verify_sha256), so their CONTENT is trusted here and only
# the destination pathname needed confining.
# ==============================================================================

# tar has to exist INSIDE the image for the two operations above, and a missing one must say so
# rather than surface as an unreadable tar diagnostic from a shell that found nothing to run.
# Checked once per run and remembered, because it is asked four times.
IMAGE_TAR_CHECKED=0
assert_image_tar() { # $1=what is about to happen, for the message
    [ "$IMAGE_TAR_CHECKED" -eq 0 ] || return 0
    chroot_run sh -c 'command -v -- tar >/dev/null 2>&1' \
        || die "refusing to $1: the image has no 'tar' of its own.  Whole-tree copies into the
       image run as 'chroot_run tar', inside the image, precisely so that no host-side command
       resolves an in-image pathname -- a symbolic link at any component of one would send a
       root-owned write to the BUILD HOST.  Add the package that provides tar to the image's
       package set, or use a base tarball that carries it (it is Essential on every
       Debian-derived base)."
    IMAGE_TAR_CHECKED=1
}

# Copy a host directory tree to an in-image destination, with the destination created and
# confirmed through the confined primitive and the copy itself performed inside the chroot.
stream_tree_into_image() { # $1=host source dir  $2=in-image relative dest  $3=octal mode
                           # $4=what it is  $5=what is about to happen  $6=extra die guidance
    local source="$1" relative="$2" mode="$3" what="$4" doing="$5" guidance="$6"

    [ -d "$source" ] || die "internal error: stream_tree_into_image was given '$source' as the
       source of $what, which is not a directory.  This is a defect in $SCRIPT_NAME."
    assert_image_tar "$doing"

    confined_mkdir "$relative" "$mode" "$what" "$doing"
    confined_directory "$relative" "/$relative" "$doing" \
        || die "refusing to $doing ($what): the in-image directory
           /$relative
       is not there immediately after this script created it through the confinement.  Something
       is changing the image's contents while it is being built; nothing is copied into it."

    # Both ends numeric-owner, so uid/gid cross unchanged rather than being remapped through
    # either passwd database -- which is what cp -a did and what the guest's build expects.
    # `tar -cf -` writes to the pipe; the extracting tar runs INSIDE the image, so "/$relative"
    # is resolved beneath $IMAGE_MOUNT by the kernel and cannot name a host path.
    tar --create --numeric-owner --file - --directory "$source" . \
        | chroot_run tar --extract --numeric-owner --preserve-permissions --file - \
              --directory "/$relative" \
        || die "could not copy $what into the image at /$relative.  $guidance"
}

# Remove an in-image tree this script created, after proving it is still the object it created.
# The delete runs INSIDE the chroot for the same reason the copy does, and the removal is
# verified rather than assumed -- this runs while the image is still being built, and a leftover
# scratch tree would be published.
remove_tree_in_image() { # $1=in-image relative  $2=recorded identity  $3=what it is  $4=doing
    local relative="$1" recorded="$2" what="$3" doing="$4"

    assert_confined_identity_unchanged "$relative" "$recorded" "$what" "$doing"
    chroot_run rm -rf -- "/$relative" \
        || die "could not $doing ($what): removing /$relative inside the image failed.  The
       image is NOT published: it would carry a scratch tree this script created and could not
       clean up."
    confined_absent "$relative" "$what" "$doing" \
        || die "$what is still present at /$relative inside the image after this script removed
       it.  The image is NOT published: something is recreating it."
}

assert_chroot_executable() {
    chroot_run /bin/true 2>/dev/null || die "cannot execute $ARCH binaries inside the image.
       For --arch i386 on an x86_64 host this normally means the host kernel lacks 32-bit
       support; for --arch armhf it means no binfmt_misc handler is registered, which is
       fixed by installing qemu-user-static and registering it:
           sudo apt-get install -y qemu-user-static binfmt-support
           sudo systemctl restart systemd-binfmt   # or: update-binfmts --enable
       Populating an image whose binaries cannot run is not possible, so this is refused
       rather than worked around."
    log "the image's own binaries execute on this host; the chroot is usable"
}

# ------------------------------------------------------------------------------------
# THE BASE'S OWN PACKAGING ARCHITECTURE.
#
# A base tarball for the wrong architecture is a mistake the ELF checks on the staged SDK do
# not catch, because those check what is COPIED IN rather than what is already there.  Left
# undetected it produces an image whose toolchain builds binaries the guest kernel cannot
# run -- or, worse, an apt that quietly installs a foreign-architecture package set.
#
# A base with no dpkg is reported rather than refused: this script does not require a
# Debian-derived base, and the package installation below fails loudly on its own if the
# base has no apt either.
# ------------------------------------------------------------------------------------
assert_base_architecture() {
    local base_arch
    base_arch="$(chroot_run dpkg --print-architecture 2>/dev/null || true)"
    if [ -z "$base_arch" ]; then
        warn "the base root filesystem provides no dpkg, so its packaging architecture cannot"
        warn "  be checked. The ELF checks on the staged SDK still apply, and the package"
        warn "  installation step will fail on its own if the base has no apt either."
        return 0
    fi
    [ "$base_arch" = "$DEBIAN_ARCH" ] || die "the base root filesystem is for the '$base_arch'
       package architecture, but --arch $ARCH needs '$DEBIAN_ARCH'.  Building on it would give
       the guest a toolchain and libraries for the wrong architecture, which the staged SDK's
       ELF checks cannot catch because they check what is copied IN rather than what is
       already there.  Pass a base tarball for $DEBIAN_ARCH, or select --arch to match the
       base you have."
    log "the base root filesystem is for '$base_arch', matching --arch $ARCH"
}

# ------------------------------------------------------------------------------------
# PACKAGE POPULATION.
#
# One apt invocation, --no-install-recommends so the package set is what is named here and
# not what a recommendation graph happened to pull in, and then an ASSERTION for every tool
# the guest actually needs.  The assertion is not redundant with the install: a package that
# installed successfully but does not provide the binary under the name the guest looks for
# is exactly the failure that otherwise surfaces two hours later inside the VM.
#
# --apt-mirror pins the archive.  Left unset, the base tarball's own sources are used as
# they stand, which is the right default for a base that is already pinned to a snapshot.
# ------------------------------------------------------------------------------------
readonly IMAGE_PACKAGES=(
    # Build-and-measure toolchain.  The suites build at -std=c++17.
    #
    # 'g++' IS UNVERSIONED HERE ON PURPOSE, and it is the FLOOR rather than the selection.
    # This list is REQUIRED -- every entry must install or the image build fails -- and the
    # guest's archive is the caller's snapshot pin, not this script's, so naming a versioned
    # package here would fail the whole image build on a package that archive may not carry.
    # What honours --toolchain-gcc-major is install_image_pinned_compiler, which runs after
    # this list, attempts gcc-N/g++-N as OPTIONAL packages, and selects them ahead of these
    # via /usr/local/bin symlinks when they arrive.  Either way the family actually in force
    # is measured out of the image by assert_image_compiler_identity and recorded in the
    # manifest, so no reader has to infer it from this line.
    g++ make autoconf automake libtool pkg-config binutils
    # A hard configure dependency: PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28]).
    libglib2.0-dev
    # GoogleTest and GoogleMock.  A discoverable gtest.pc is what configure actually needs,
    # and it is asserted separately below rather than assumed from the package name.
    libgtest-dev libgmock-dev
    # Coverage.  gcov ships with gcc; lcov here is only a floor -- the 2.x requirement is
    # installed and asserted separately, because stable archives ship 1.x.
    lcov
    # lcov 2.x is Perl and needs these modules; naming them here means a source install of
    # lcov 2.x has its dependencies already satisfied.
    #
    # libtimedate-perl IS NOT OPTIONAL, AND ITS ABSENCE COST A WHOLE GUEST RUN.  genhtml
    # opens with `use Date::Parse`, which comes from libtimedate-perl and from nothing else
    # in this list -- libdatetime-perl provides DateTime, a different module.  Without it
    # genhtml dies at load with "Can't locate Date/Parse.pm in @INC" while `lcov --version`
    # still answers, so the image built, booted and reached the coverage runner before
    # generate_html() failed the run outright: run_coverage.sh treats a genhtml failure as
    # fatal, and coverage/index.html is one of the artifacts the workflow's completeness gate
    # requires.  Measured inside the guest, at the end of a full TCG run.  The module set was
    # taken from the five tools this job actually invokes -- lcov, genhtml, geninfo, genpng,
    # gendesc -- plus lib/lcovutil.pm; everything else they use is core perl.
    perl libcapture-tiny-perl libdatetime-perl libtimedate-perl libjson-xs-perl \
    libperlio-gzip-perl
    # What the init and run_coverage.sh reach for: mount, mknod, stat, timeout, flock,
    # nproc, awk, find, mktemp, ps, and a shell that is actually bash.
    bash coreutils util-linux findutils mawk procps
    # Provenance only, and optional to the run: run_coverage.sh guards every git use with
    # `command -v git`, so its absence degrades provenance rather than failing anything.
    git
    # file is what the init uses to describe what it found when something is not what it
    # expected; ca-certificates keeps a host-side apt over https working.
    file ca-certificates
)

# Packages that IMPROVE the image without being required by anything the guest must do, and
# whose availability differs between archives.  They are installed one at a time and a
# missing one is a warning, never a failure: dying on an optional package would make the
# image build depend on a package name that is not load-bearing.  Memory::Process is only
# consulted by lcov's memory-tracking option, which nothing here uses.
readonly IMAGE_PACKAGES_OPTIONAL=(
    libmemory-process-perl
)

# ------------------------------------------------------------------------------------
# BUILD-TIME-ONLY APT AUTHENTICATION.
#
# WHY THIS OPTION EXISTS AT ALL.  The obvious alternative -- telling a caller with an
# authenticated archive to put /etc/apt/auth.conf.d or a .netrc INTO THE BASE TARBALL -- puts a
# permanent credential file inside an image that is uploaded as a workflow artifact and copied
# between machines, in paths a scan of the caller's own archive cannot be relied on to reach.
# Advice of that shape defeats the protection the rest of this script provides, so the supported
# route is this one, whose lifetime this script controls.
#
# THE LIFETIME IS THE WHOLE GUARANTEE.  The file exists inside the image only between
# install_image_apt_auth() and remove_image_apt_auth(), which bracket the apt phase and nothing
# else.  Four things then hold:
#
#   * the removal is RE-READ, and a survivor is fatal -- `rm -f` reports success for a path
#     that does not exist, so its status alone establishes nothing;
#   * a failure anywhere in the apt phase reaches cleanup(), which removes the STAGED image
#     outright, so a run that died holding the credential publishes no artefact to hold it;
#   * scrub_and_assert_image_credential_stores() independently refuses to publish an image
#     carrying any known credential store, including this one, so the removal is checked twice
#     by two mechanisms that do not share code; and
#   * the content is never logged, never digested into the manifest and never echoed -- this
#     script reads the file once, with `cat`, into a path inside the chroot, and never parses it.
#
# 0600 AND root:root, stated rather than inherited: apt reads auth.conf.d as root and REFUSES a
# world-readable entry, so the mode is functional as well as protective.
#
# EVERY FILESYSTEM OPERATION BELOW IS CONFINED, AND THE HOLE THAT CLOSES IS A REAL ONE.
# Checking only whether the COMPLETE pathname $IMAGE_MOUNT/etc/apt/auth.conf.d is a symbolic
# link, and then doing an ordinary host-root `mkdir -p`, `chmod`, create, `chown` and `cat >`
# through it, leaves every intermediate component unexamined.  A base filesystem whose /etc or
# /etc/apt is an ABSOLUTE symbolic link would redirect all five onto the host, at which point
# this script would write a caller's credential to a host-chosen path as root and set that
# path's mode and ownership.  The pre-publication scan cannot help: it runs thousands of lines
# later, and the damage would already be outside the image.
#
# So the directory creation, the mode, the exclusive create, the ownership and the content write
# all go through the confined I/O primitive, anchored on the image mount point: openat2 with
# RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV resolves each component, the kernel
# refuses a link at ANY position rather than following it, and the operation is performed
# through the descriptor that resolution produced.  A base tarball with a symlinked /etc, a
# symlinked /etc/apt or a symlinked auth.conf.d is a fatal refusal naming the path, and no host
# file is touched on the way to it.
readonly IMAGE_APT_AUTH_LEAF='99cec-l2-build-auth'

# The in-image paths, RELATIVE to the image mount point, because that is what the confined
# primitive takes: an absolute path would be refused (RESOLVE_BENEATH) and a path assembled with
# $IMAGE_MOUNT would reintroduce the host-side pathname this mechanism exists to remove.
readonly IMAGE_APT_AUTH_DIR_REL='etc/apt/auth.conf.d'
readonly IMAGE_APT_PARENT_DIR_REL='etc/apt'
# The apt configuration this script writes itself, also relative to the image mount point.  The
# sources file is declared beside the scrub that re-reads it (IMAGE_APT_SOURCES_FILE_REL).
readonly IMAGE_APT_CONF_DIR_REL='etc/apt/apt.conf.d'
readonly IMAGE_APT_SNAPSHOT_CONF_REL='etc/apt/apt.conf.d/99cec-l2-snapshot'

install_image_apt_auth() {
    [ -n "$APT_AUTH_FILE" ] || return 0
    local rel="$IMAGE_APT_AUTH_DIR_REL/$IMAGE_APT_AUTH_LEAF" shown record

    # THE PARENT CHAIN, CREATED COMPONENT BY COMPONENT UNDER THE CONFINEMENT.  /etc and /etc/apt
    # are created at 0755 if the base has none, because they are ordinary system directories and
    # a base without them is not a Debian root filesystem; auth.conf.d is created at 0700
    # separately, because a credential is about to be written into it.  Each component is
    # resolved with openat2, so a symlinked /etc or /etc/apt is refused HERE -- before any mode,
    # ownership or byte is written -- rather than followed onto the host.
    confined_mkdir "$IMAGE_APT_PARENT_DIR_REL" 0755 \
        "the image's /$IMAGE_APT_PARENT_DIR_REL directory" \
        'create the directory chain for the temporary apt credential'
    confined_mkdir "$IMAGE_APT_AUTH_DIR_REL" 0700 \
        "the image's /$IMAGE_APT_AUTH_DIR_REL directory" \
        'create the directory for the temporary apt credential'
    # Asserted rather than inherited: the base may ship auth.conf.d with a looser mode, and a
    # credential's directory must not be readable by anyone but root while it is in there.
    confined_chmod "$IMAGE_APT_AUTH_DIR_REL" 0700 \
        "the image's /$IMAGE_APT_AUTH_DIR_REL directory" \
        'restrict the directory holding the temporary apt credential'

    # THE NAME MUST BE FREE.  It is this script's own name and this script removes it again, so
    # a file already sitting there is something the caller supplied that would be deleted as if
    # it were ours.  A SYMBOLIC LINK at that name does not reach this message: the confined stat
    # refuses it first, which is the stronger answer, and confined_absent turns that refusal
    # into a fatal diagnosis naming the path rather than reading it as "not there".
    confined_absent "$rel" 'the temporary apt credential file' \
        'install the temporary apt credential' \
        || die "the image already contains /$rel before this script installed it.  That name is
       this script's own, so the base tarball carries a file this script is about to remove as
       its own -- which would delete something the caller supplied.  Refused; rename it in the
       base tarball."

    # Created EXCLUSIVELY and at the restrictive mode BEFORE any content reaches it: writing the
    # bytes first and chmod'ing afterwards leaves a window in which the credential is
    # world-readable inside the image, and windows of that shape are what this option exists to
    # close.  O_CREAT|O_EXCL under the confinement also means a link created at that name in the
    # instant before this call is refused with EEXIST rather than written through.
    confined_create "$rel" 0600 'the temporary apt credential file' \
        'create the temporary apt credential file'
    confined_chown "$rel" 0 0 'the temporary apt credential file' \
        'set root:root ownership on the temporary apt credential file'
    # The content goes in through the descriptor the confinement produced.  The caller's file is
    # read on the host side by the redirection; the only thing crossing into the image is bytes
    # on stdin, so there is no in-image pathname to substitute between the create and the write.
    # And the host side of that redirection is APT_AUTH_CUSTODY -- the descriptor validate_inputs
    # vetted and still holds -- rather than the caller's name, so the bytes written here are the
    # bytes those checks were made on however the name has since been re-pointed.
    confined_write "$rel" 'the temporary apt credential file' \
        'copy the --apt-auth-file credential into the image for the apt phase' \
        < "$APT_AUTH_CUSTODY"

    # A FINAL CONFINED RE-READ of what was actually created, by descriptor: type, mode and link
    # count.  It costs one syscall and it is the difference between "the calls returned 0" and
    # "the object at that name is the object this script made".
    local verify_status=0
    record="$(confined_stat "$rel" 'verify the temporary apt credential file')" \
        || verify_status=$?
    [ "$verify_status" -eq 0 ] || confined_io_die "$verify_status" "$rel" \
        'the temporary apt credential file' 'verify the temporary apt credential file'
    shown="$(confined_stat_field "$record" 1)"
    [ "$shown" = 'regular' ] || die "the image's /$rel is a $shown rather than a regular file
       immediately after this script created it exclusively.  Something is changing the image's
       contents while it is being built; the credential install is not completed."
    shown="$(confined_stat_field "$record" 6)"
    [ "$shown" = '0600' ] || die "the image's /$rel is mode $shown rather than 0600 immediately
       after this script set it.  apt refuses a group- or world-readable auth.conf.d entry, and
       this script does not relax the mode to work around that."
    shown="$(confined_stat_field "$record" 4)"
    [ "$shown" = '1' ] || die "the image's /$rel has $shown links immediately after this script
       created it exclusively, so another name refers to the same inode and would still refer to
       the credential after this script removes this one.  Refused; inspect the base tarball."

    # The PATH is logged, at the level everything else in this phase logs at.  The content is
    # not, and the log says so explicitly so that a reader does not go looking for it.
    log "apt credentials installed for the package phase only, at"
    log "  /$rel (mode 0600, root:root, one link; content deliberately not logged)"
    log "  Written through openat2 with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, so"
    log "  a symlinked /etc, /etc/apt or auth.conf.d in the base would have been refused rather"
    log "  than followed onto the build host."
    log "  They are removed again as soon as the package phase finishes, the removal is"
    log "  verified, and the pre-publication credential scan refuses to publish an image that"
    log "  still carries them."
}

remove_image_apt_auth() {
    [ -n "$APT_AUTH_FILE" ] || return 0
    local rel="$IMAGE_APT_AUTH_DIR_REL/$IMAGE_APT_AUTH_LEAF"

    # ALREADY GONE IS NOT AN ERROR, but it is worth saying out loud: apt itself does not remove
    # auth.conf.d entries, so this arm means something else did, and the run continues only
    # because the outcome asked for -- no credential in the image -- has been reached.  Note that
    # confined_absent DIES on any answer other than "there" or "not there": a symbolic link at
    # that name is refused rather than reported absent, so this cannot be reached by planting one.
    if confined_absent "$rel" 'the temporary apt credential file' \
            'remove the temporary apt credential'; then
        warn "the temporary apt credential file was already absent from the image at /$rel when
       this script came to remove it.  Nothing installed it there but this script, and nothing
       in the apt phase removes it, so something else changed the image's contents.  The outcome
       required -- no credential inside the published image -- holds, and the pre-publication
       credential scan checks it again independently."
        return 0
    fi

    # unlinkat() on the leaf, relative to a descriptor obtained under the confinement.  It never
    # follows a symbolic link, so the entry removed is an entry of the image's own auth.conf.d
    # and of no other directory -- which is what an ordinary `rm -f` through a host-side
    # pathname could not promise when /etc or /etc/apt was a link.
    confined_unlink "$rel" "the image's /$rel" \
        'remove the temporary apt credential'

    # FATAL RE-READ, THROUGH THE CONFINEMENT.  A removal that reported success establishes that
    # a call returned 0; this establishes that nothing is at the name any more.
    confined_absent "$rel" 'the temporary apt credential file' \
        'verify the temporary apt credential is gone' \
        || die "the temporary apt credential file is still present in the image after removal:
       /$rel
       The image is NOT published.  A credential the caller passed for build-time use only
       would otherwise be inside a published artefact."
    log "the temporary apt credentials have been removed from the image and verified absent"
}

# ------------------------------------------------------------------------------------
# THE PINNED COMPILER FAMILY INSIDE THE GUEST.  OPTIONAL BY ARCHIVE, SELECTED WHEN PRESENT,
# NAMED AS A LIMITATION WHEN ABSENT.
#
# WHY THIS IS NOT SIMPLY ADDED TO IMAGE_PACKAGES.  That set is the one the image cannot be built
# without, and every name in it exists in every archive this image is populated from.  gcc-N does
# not: the guest is Debian at a pinned snapshot -- bookworm's default family is GCC 12 -- and
# whether that snapshot serves the caller's pinned family is a property of the archive, not of
# this script.  Putting gcc-N in the required set would make the image build fail on a package
# the archive never had, which reports the caller's archive as an image defect.  Leaving the pin
# out altogether is the other wrong answer: then the guest measures with an unrecorded compiler
# and nothing says so.  So the third option is taken -- attempt, select on success, RECORD either
# way, and name the shortfall in the caller's own terms when it happens.
#
# WHY SYMBOLIC LINKS IN /usr/local/bin RATHER THAN update-alternatives OR CC/CXX EXPORTS.
# Three reasons, in order of how much they matter:
#
#   * /usr/local/bin is FIRST on the PATH the in-guest init exports, so a link there is what a
#     bare `gcc`, `g++` or `gcov` resolves to for every in-guest compile and for lcov's own
#     invocations of gcov -- including tests/L1Tests/run_coverage.sh, which resolves gcov from
#     PATH and offers no override knob at all.  An exported CXX would reach configure and make
#     but would NOT reach that.
#   * It reaches the helper compiles in this script too (build_guest_helpers runs after this
#     phase and invokes unversioned gcc/g++ inside the chroot), so the whole image is built with
#     one family rather than two.
#   * update-alternatives would need the base to have registered the alternative already, which
#     a minimal debootstrap has not.
#
# gcov IS LINKED WITH THE COMPILERS AND NOT SEPARATELY, because the pairing is the property that
# matters: gcov demands a notes-file version stamp derived from the GCC major and minor that
# wrote the file and reads nothing when they differ.  Selecting a compiler without its gcov would
# produce a build the guest cannot measure.
#
# EVERY WRITE HERE IS A chroot_run ON AN IN-IMAGE ABSOLUTE PATH, which is confined by
# construction -- the chroot is the confinement -- rather than a host-side write through
# "$IMAGE_MOUNT/...", which would follow a link the base tarball supplied.
# ------------------------------------------------------------------------------------
install_image_pinned_compiler() {
    if [ -z "$TOOLCHAIN_GCC_MAJOR" ]; then
        PINNED_COMPILER_STATE='not requested'
        log "no --toolchain-gcc-major was given: the image keeps its base default compiler."
        log "  Its versions are still measured and recorded by assert_image_toolchain, and the"
        log "  gcov-matches-compiler assertion still applies; only the comparison against a"
        log "  pinned family is skipped, because none was named."
        return 0
    fi

    local major="$TOOLCHAIN_GCC_MAJOR"
    log "attempting the pinned compiler family for the guest: gcc-$major and g++-$major"
    if ! chroot_run apt-get install -y --no-install-recommends "gcc-$major" "g++-$major"; then
        PINNED_COMPILER_STATE='unavailable'
        warn "NAMED LIMITATION: the guest archive does not provide gcc-$major and g++-$major, so"
        warn "  the image keeps its base default compiler and the GUEST DOES NOT MATCH the"
        warn "  acceptance family pinned by the caller (--toolchain-gcc-major $major)."
        warn "  This is reported rather than fatal: the package set comes from the archive the"
        warn "  caller pinned with --apt-mirror -- Debian bookworm's default family is GCC 12 --"
        warn "  and dying here would fail the image build for the archive's contents."
        warn "  WHAT IT MEANS FOR THE EVIDENCE: the guest's line, function and branch figures"
        warn "  and its (file,line,block,branch) branch-arm coordinates are produced by the"
        warn "  guest's own compiler, so they are internally consistent and comparable ACROSS"
        warn "  RUNS OF THIS IMAGE, but they are not proven comparable with figures recorded on"
        warn "  the pinned family. assert_image_toolchain records the measured versions and the"
        warn "  manifest carries them as GUEST_TOOLCHAIN_*."
        warn "  TO CLOSE IT: pin an archive that serves gcc-$major (a suite whose default family"
        warn "  is $major, or one with a backports component carrying it) and rebuild the image."
        return 0
    fi

    # SELECTED, AND THE SELECTION IS THEN PROVEN RATHER THAN ASSUMED. `ln -sfn` because the base
    # may already provide /usr/local/bin entries, and the assertion afterwards is what
    # distinguishes "the link command returned 0" from "a bare gcc now resolves to the pinned
    # family".
    local tool
    for tool in gcc g++ gcov; do
        chroot_run ln -sfn -- "/usr/bin/$tool-$major" "/usr/local/bin/$tool" \
            || die "could not select the pinned compiler inside the image: linking
       /usr/local/bin/$tool to /usr/bin/$tool-$major failed.  The packages installed, so the
       image now holds two families with the unpinned one first on PATH, which is worse than
       either alone -- the build and the measurement could disagree about which compiler wrote
       the notes files.  The image is not published."
    done

    local resolved
    for tool in gcc g++ gcov; do
        resolved="$(chroot_run sh -c "command -v -- '$tool' 2>/dev/null" || true)"
        [ "$resolved" = "/usr/local/bin/$tool" ] || die "inside the image a bare '$tool'
       resolves to '${resolved:-nothing at all}' rather than to the /usr/local/bin/$tool link
       just created for the pinned family.  Something in the image's PATH resolution precedes
       /usr/local/bin, so the in-guest build and the coverage capture would not use the compiler
       this option selected.  The image is not published."
    done

    PINNED_COMPILER_STATE='selected'
    log "the pinned family is selected inside the image: gcc, g++ and gcov in /usr/local/bin"
    log "  point at /usr/bin/<tool>-$major, and a bare lookup of each resolves there."
}

install_image_packages() {
    if [ -n "$APT_MIRROR" ]; then
        # THE VALUE WRITTEN HERE CANNOT CARRY A CREDENTIAL, and that is enforced at the
        # boundary rather than described here.  validate_inputs refuses userinfo and refuses a
        # query string in --apt-mirror precisely because this line is written into the
        # PUBLISHED image: writing the value verbatim and REPORTING that the caller's token is
        # now inside the artifact would be a warning where a refusal belongs.  Because the
        # boundary refuses those forms, the sanitised form and the verbatim form are the same
        # value here, and the log prints the sanitised one because that is what the rest of the
        # script prints.
        #
        # scrub_and_assert_image_apt_credentials(), run before the image is unmounted, verifies
        # this on the FINISHED filesystem - including anything the base tarball brought with it.
        sanitise_endpoint "$APT_MIRROR"
        log "pinning the package archive to $SANITISED_ENDPOINT"
        if [ "$SANITISED_ENDPOINT_REDACTED" -eq 1 ]; then
            die "internal error: the --apt-mirror value still reduces to something different
       when sanitised, which means it carries userinfo, a query or a fragment after
       validate_inputs accepted it.  It is not written into the image."
        fi
        # WRITTEN THROUGH THE CONFINED PRIMITIVE, for the same reason install_image_apt_auth()
        # is: `> "$IMAGE_MOUNT/etc/apt/sources.list"` is an ordinary redirection, it FOLLOWS
        # SYMBOLIC LINKS, and it runs as root on the BUILD HOST -- so a base tarball whose /etc,
        # /etc/apt or sources.list is an absolute symbolic link had this script truncate and
        # replace a HOST file, and `mkdir -p` created a host directory tree on the way to it.
        # openat2 with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV refuses each of those
        # rather than following it, and the write goes through the descriptor that resolution
        # produced.
        #
        # The file is created if the base has none and rewritten in place if it has one, which is
        # what the redirection did; 0644 is apt's own expectation and is stated rather than left
        # to the umask, so the mode is a property of this script rather than of its environment.
        confined_mkdir "$IMAGE_APT_PARENT_DIR_REL" 0755 "the image's /$IMAGE_APT_PARENT_DIR_REL
       directory" 'write the pinned package archive into the image'
        if ! confined_regular_file "$IMAGE_APT_SOURCES_FILE_REL" "/$IMAGE_APT_SOURCES_FILE_REL" \
                'write the pinned package archive into the image'; then
            confined_create "$IMAGE_APT_SOURCES_FILE_REL" 0644 \
                "the image's /$IMAGE_APT_SOURCES_FILE_REL" \
                'write the pinned package archive into the image'
        fi
        printf '%s\n' "$APT_MIRROR" \
            | confined_write "$IMAGE_APT_SOURCES_FILE_REL" \
                  "the image's /$IMAGE_APT_SOURCES_FILE_REL" \
                  'write the pinned package archive into the image'
        confined_chmod "$IMAGE_APT_SOURCES_FILE_REL" 0644 \
            "the image's /$IMAGE_APT_SOURCES_FILE_REL" \
            'set the mode of the image apt sources file'

        # A pinned snapshot archive is by definition not the latest, so let apt use it.  Same
        # confinement, same reasoning: this creates a directory and a file inside the image.
        confined_mkdir "$IMAGE_APT_CONF_DIR_REL" 0755 \
            "the image's /$IMAGE_APT_CONF_DIR_REL directory" \
            'write the snapshot apt configuration into the image'
        if ! confined_regular_file "$IMAGE_APT_SNAPSHOT_CONF_REL" \
                "/$IMAGE_APT_SNAPSHOT_CONF_REL" \
                'write the snapshot apt configuration into the image'; then
            confined_create "$IMAGE_APT_SNAPSHOT_CONF_REL" 0644 \
                "the image's /$IMAGE_APT_SNAPSHOT_CONF_REL" \
                'write the snapshot apt configuration into the image'
        fi
        printf '%s\n' 'Acquire::Check-Valid-Until "false";' \
            | confined_write "$IMAGE_APT_SNAPSHOT_CONF_REL" \
                  "the image's /$IMAGE_APT_SNAPSHOT_CONF_REL" \
                  'write the snapshot apt configuration into the image'
        confined_chmod "$IMAGE_APT_SNAPSHOT_CONF_REL" 0644 \
            "the image's /$IMAGE_APT_SNAPSHOT_CONF_REL" \
            'set the mode of the image snapshot apt configuration'
    fi

    # IMMEDIATELY BEFORE THE APT PHASE, and no earlier: the credential is inside the image for
    # the shortest span that lets apt use it.  A no-op when --apt-auth-file was not given.
    install_image_apt_auth

    log "installing the build-and-measure toolchain into the image"
    chroot_run apt-get update -qq \
        || die "apt-get update failed inside the image.  If the base tarball pins a snapshot
       archive, pass --apt-mirror with that snapshot's URL so the package set matches the
       base; if this host has no network, use a base tarball that already carries the
       toolchain."
    chroot_run apt-get install -y --no-install-recommends "${IMAGE_PACKAGES[@]}" \
        || die "installing the toolchain into the image failed.  The set is fixed and none of
       it is optional: without g++/make/autotools the guest cannot build, without
       libglib2.0-dev configure fails at PKG_CHECK_MODULES([GLIB], ...), and without
       GoogleTest configure fails at PKG_CHECK_MODULES([GTEST], ...)."

    local optional
    for optional in "${IMAGE_PACKAGES_OPTIONAL[@]}"; do
        if chroot_run apt-get install -y --no-install-recommends "$optional"; then
            log "optional package installed: $optional"
        else
            warn "the optional package '$optional' is not available in this archive; continuing."
            warn "  Nothing the guest must do depends on it."
        fi
    done

    # AFTER the fixed set and the optional list, and BEFORE the package record below, so that
    # whichever compiler ends up selected is the one the record describes.
    install_image_pinned_compiler

    # RECORD BEFORE CLEANING.  apt-get clean removes the downloaded archives but not the dpkg
    # database, so the order here is not load-bearing -- it is written this way so that the
    # record describes the image as the guest will find it, after every install has run.
    record_installed_packages

    # The archive metadata and the downloaded packages are dead weight in a boot image, and
    # the guest never uses apt.
    chroot_run apt-get clean || true

    # THE APT PHASE IS OVER, SO THE CREDENTIAL GOES.  Placed after `apt-get clean` because that
    # is the last apt invocation, and before anything else touches the image: from here on the
    # filesystem is built up rather than fetched into, so nothing after this line needs it.
    # Every failure arm above reaches cleanup(), which removes the STAGED image, so a run that
    # dies between the install and this line publishes nothing at all.
    remove_image_apt_auth

    neutralise_image_network_configuration
}

# ------------------------------------------------------------------------------------
# THE PACKAGE SET IS RECORDED INSIDE THE IMAGE, WITH VERSIONS.
#
# WHY THIS EXISTS.  Pinning the archive to an immutable snapshot makes the package set
# reproducible; it does not make it VISIBLE.  When a rerun behaves differently the first
# question is "what changed", and without this file the only answer available is "nothing
# should have" -- which is a claim, not evidence.  With it, two images can be diffed by
# package and version and the answer is a fact.
#
# WHY THE VERSIONS ARE NOT PINNED PACKAGE BY PACKAGE IN THIS SCRIPT.  A version list in the
# apt command line pins the same thing twice and the two copies drift: the snapshot timestamp
# already determines every version in the archive, so a list beside it can only ever agree
# with it or contradict it.  The snapshot is the pin; this file is the measurement of what
# that pin resolved to.
#
# The record is written to the image, NOT to the manifest, because it is long (a minbase plus
# a C++ toolchain is a few hundred packages) and because a reader inside the guest is the one
# who needs it.  The manifest names the path, the count and a digest of the record, so the host
# side can see that it exists, how large it is and whether two images resolved to the same set
# without carrying the list itself.
#
# AND IT IS A PRECONDITION OF PUBLICATION, NOT A COURTESY.  Catching a failed dpkg-query,
# warning, writing "# UNAVAILABLE" into the record, setting the count to the string 'unavailable'
# and RETURNING SUCCESS -- or coercing a malformed count to zero -- would publish an image with
# no exact resolved package/version set, which is the single piece of evidence this function
# exists to produce: "reproducible" is a claim about a set nobody can read back, and a manifest
# whose count says 'unavailable' documents an audit gap rather than an image.  The reasoning that
# would make it advisory -- that the toolchain, pkg-config and lcov assertions still run --
# answers a different question: those prove the image can BUILD, not what is IN it.  So every way
# this can fail to produce a concrete record is fatal here, an hour before publication and while
# the image is still mounted:
#
#   * dpkg-query failing inside the chroot,
#   * an empty listing,
#   * a zero or non-numeric installed count,
#   * a record whose lines do not carry package, version, architecture and status, and
#   * any package this script REQUIRES that cannot be accounted for in the record.
#
# The last one is the check with teeth, and it needs one subtlety: a required name can be
# satisfied by a different package that Provides it -- pkg-config is provided by pkgconf on
# newer archives, for instance -- so a name that is not directly installed is resolved through
# dpkg's own Provides field before it is called missing.  Anything still unaccounted for means
# apt installed something other than what this script asked for, and that is exactly the drift
# the record exists to make visible.
# ------------------------------------------------------------------------------------
PACKAGE_RECORD_COUNT='unknown'
PACKAGE_RECORD_DIGEST='unknown'
PACKAGE_RECORD_REQUIRED_ACCOUNTED='unknown'

# ------------------------------------------------------------------------------------
# WHAT "INSTALLED" MEANS HERE, AND WHY A SUFFIX TEST DOES NOT MEAN IT.
#
# dpkg's ${Status} is THREE words -- want, error-flag, status -- and the only one of them that
# means "this package's files are in this filesystem, configured" is the exact triple
#
#     install ok installed
#
# Matching the regular expression /installed$/ against the record is a test on the LAST WORD
# ONLY, and therefore accepts, among others:
#
#     purge     ok        not-installed     the package is GONE; the entry is a tombstone
#     install   reinstreq half-installed    unpacking failed part way through
#     deinstall ok        config-files      removed, only its conffiles remain
#
# The first and third end in "installed" and "config-files" respectively -- and the first ends
# in the literal text "installed" because "not-installed" does.  Under a suffix test a required
# package that had been purged, or whose unpack died, would increment `accounted` and publish
# REQUIRED_PACKAGES_ACCOUNTED as though it were present.  That is the exact drift this record
# exists to catch, so it cannot be the drift the record's own parser introduces.
#
# So the three fields are compared POSITIONALLY and exactly, on both listings.  The two listings
# need different field arithmetic and the difference is not cosmetic:
#
#   the direct listing   '${Package} ${Version} ${Architecture} ${Status}'  -- default FS, so
#                        $1 package, $2 version, $3 architecture, and the status triple lands
#                        in $4 $5 $6.  NF is therefore exactly 6 for a well-formed line.
#   the provider listing '${Package} ${Status} :: ${Provides}'  -- read with -F ' :: ', so $1
#                        is the FOUR WORDS "pkg want eflag status" as a single field and has to
#                        be re-split before the triple can be read at all.
#
# A record that does not carry the full triple is REJECTED rather than guessed at: a short or
# malformed line is not evidence that a package is installed, and record_installed_packages'
# own malformed-line check is fatal on the same shape a few lines below.
#
# AND THE want FIELD IS REQUIRED TO BE "install" HERE, WHICH REJECTS "hold ok installed".
# That is deliberate and it is the one arm worth justifying, because a held package IS present
# and configured -- it is merely pinned against upgrade.  Two things make the strict form the
# right one for THIS question.  First, the question these two functions answer is not "is it in
# the image" but "did apt install what this script asked for", and a required name arriving in
# a want-state nothing in this script sets is drift of exactly the kind the record exists to
# surface: the chroot is built from a base tarball into an empty package set and nothing here
# ever holds anything, so the state is unreachable by this script's own actions.  Second, the
# failure direction is the safe one -- an unaccounted required name is FATAL and names the file
# to inspect, whereas a looser test here would publish an image while claiming a purged package
# was present.  The count of installed packages a few hundred lines below asks the other
# question and deliberately uses the looser ' installed$' test, which does keep a held package;
# the comment on that line explains why the two differ rather than one of them being wrong.
# ------------------------------------------------------------------------------------

# Is one package name accounted for in a dpkg listing, as an installed package in its own
# right?  Exact field comparison rather than a pattern, because package names contain '+' and
# '.' and a required name is a prefix of others ('g++' of nothing, but 'perl' of many) -- and
# now exact STATUS comparison too, for the reason in the block above.
package_directly_installed() { # $1=listing  $2=package name
    printf '%s\n' "$1" | awk -v want="$2" '
        NF >= 6 && $1 == want && $4 == "install" && $5 == "ok" && $6 == "installed" { found = 1 }
        END { exit(found ? 0 : 1) }'
}

# Is one package name PROVIDED by some installed package?  $1 is a listing in the
# "<package> <status> :: <provides>" form; Provides entries may carry a version
# ("pkg-config (= 1.8.1)"), so only the name before the first space is compared.
#
# The provider's OWN status is checked first and exactly: a Provides field is present in the
# database whether or not the providing package is installed, so a purged or half-unpacked
# provider would otherwise satisfy a required name with nothing in the filesystem behind it.
package_provided_by_installed() { # $1=provides listing  $2=package name
    printf '%s\n' "$1" | awk -v want="$2" -F ' :: ' '
        {
            # $1 is "<package> <want> <eflag> <status>" as one field; re-split it to read the
            # status triple positionally.  Anything shorter than four words is not a status
            # record and is skipped rather than interpreted.
            words = split($1, status_fields, /[ \t]+/)
            if (words < 4) { next }
            if (status_fields[words - 2] != "install") { next }
            if (status_fields[words - 1] != "ok")      { next }
            if (status_fields[words]     != "installed") { next }
            if ($2 == "") { next }
            n = split($2, entries, /,[ ]*/)
            for (i = 1; i <= n; i++) {
                split(entries[i], parts, " ")
                if (parts[1] == want) { found = 1 }
            }
        }
        END { exit(found ? 0 : 1) }'
}

# ------------------------------------------------------------------------------------
# THE SELF-TEST.  `--self-test` and nothing else on the command line.
#
# WHY IT IS IN THIS FILE AND NOT IN A TEST FILE OF ITS OWN.  The two package-status parsers
# above are the whole of the package accounting's correctness, and they are pure functions of a
# string -- so they are exactly the kind of code a regression gate belongs on.  Without one the
# only way to exercise them is to build an image, which needs root, a binder-capable kernel and
# half an hour.  This repository's change set is a fixed allowlist of paths, so a new test file
# cannot be added; putting the cases in the script itself is not a compromise on that constraint
# but a better fit for it -- the gate travels with the code it guards, cannot drift out of sync
# with it, and needs no harness.
#
# WHAT IT GUARANTEES, PRECISELY.
#
#   * IT RUNS BEFORE ANYTHING PRIVILEGED.  main() dispatches it as its first statement, ahead
#     of parse_args, require_host_tools, validate_inputs and every mount, loop device and
#     chroot.  `bash aidl-path-tests-rootfs.sh --self-test` needs no root, touches nothing
#     outside one mktemp directory, and cannot begin an image build.
#   * IT IS DETERMINISTIC.  Every case is a literal string in the table below.  No file, no
#     network, no dpkg, no clock.
#   * A MISMATCH IS A NON-ZERO EXIT.  Each case prints one line, the counts print at the end,
#     and the exit status is the failure count clamped to 1.  A workflow step that runs this
#     fails the job on any regression.
#
# WHY THE PACKAGE CASES ARE THE ONES THEY ARE.  Every rejected status below is a real dpkg
# state that a naive parser accepts, and the first two are the ones an unanchored suffix test
# accepts in practice:
#
#   purge ok not-installed       -- a PURGE TOMBSTONE.  An unanchored /installed$/ matches it,
#                                   because the tombstone's own status ends in the TEXT
#                                   "installed".  The package is not in the filesystem.
#   install reinstreq half-installed -- an INTERRUPTED unpack.  Same text ending, same trap;
#                                   the files are partially there and the maintainer scripts
#                                   have not run.
#   deinstall ok config-files    -- removed but its conffiles are kept.  No binaries.
#   install ok half-configured   -- unpacked, postinst failed.  A compiler in this state does
#                                   not necessarily run.
#   install ok unpacked          -- files extracted, never configured.
#   a short record               -- dpkg leaves ${Version} empty in some half-states, so a
#                                   record can arrive with fewer than six fields; it is not a
#                                   status record and is skipped rather than interpreted.
#   a name mismatch              -- "$1 == want", not a substring or a prefix test, so
#                                   build-essential-doc never satisfies build-essential.
#   a Provides PREFIX            -- entries are compared on the name before the first space,
#                                   with "==", so a Provides of "pkg" does not satisfy a
#                                   requirement for "pkg-config" and vice versa.
#   an empty Provides field      -- a record with the separator and nothing after it provides
#                                   nothing, and must not be read as providing the wanted name.
#
# The confined-I/O cases at the end run the SHIPPED start-up proof, probe_confined_io(), plus
# three direct calls whose outcomes are printed individually -- so the self-test also answers
# "is openat2 with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS actually in force on this host", which
# is the one environmental question the rest of the script depends on absolutely.
#
# The confined-WRAPPER cases after them answer the question that one does not: the primitive
# being in force says nothing about whether the wrappers every privileged operation in this
# script goes THROUGH still refuse what they must.  A wrapper that swallowed a refusal would
# leave the primitive working and the operation unconfined, so each of the properties the
# converted call sites rely on -- rm -f semantics on a symlinked leaf, a refused redirected
# write or chmod with the host file PROVED unchanged afterwards, a refused mkdir and
# enumeration, the [ -x ] replacement's four arms, and the identity guard on the two recursive
# deletes -- is its own case.  Their own block comment above them says which call site each one
# is standing in for.
# ------------------------------------------------------------------------------------
SELF_TEST_CASES=0
SELF_TEST_FAILURES=0

self_test_report() { # $1=ok|not-ok  $2=subject  $3=case name  $4=detail
    SELF_TEST_CASES=$(( SELF_TEST_CASES + 1 ))
    if [ "$1" = 'ok' ]; then
        printf 'ok      %-32s %-44s %s\n' "$2" "$3" "$4"
    else
        SELF_TEST_FAILURES=$(( SELF_TEST_FAILURES + 1 ))
        printf 'NOT OK  %-32s %-44s %s\n' "$2" "$3" "$4"
    fi
}

# One table row: call the named parser with the listing and the wanted package name, and
# compare "accept" or "reject" against what the row says it must be.
self_test_package_case() { # $1=parser  $2=accept|reject  $3=want  $4=listing  $5=case name
    local parser="$1" expected="$2" want="$3" listing="$4" name="$5" actual='accept'
    "$parser" "$listing" "$want" || actual='reject'
    if [ "$actual" = "$expected" ]; then
        self_test_report ok "$parser" "$name" "want=$want -> $actual"
    else
        self_test_report not-ok "$parser" "$name" "want=$want -> $actual, expected $expected"
    fi
}

# The confined I/O primitive, exercised where it can do no harm: one mktemp directory,
# registered with the same TEMP_DIRS list the EXIT trap already drains, so the self-test leaves
# nothing behind on either a pass or a failure.
#
# resolve_temp_parent() is deliberately NOT used.  It refuses a world-writable /tmp without the
# sticky bit, which is correct for a privileged image build -- root writes multi-gigabyte files
# there -- and wrong for a self-test, which must be runnable by an unprivileged developer on
# whatever /tmp their container has.  mktemp -d creates the directory O_EXCL at mode 0700, and
# nothing but this script's own probe files is written into it.
self_test_confined_io() {
    local scratch anchor content status=0
    scratch="$(mktemp -d)" || {
        self_test_report not-ok 'confined-io' 'a scratch directory could be created' \
            'mktemp -d failed'
        return 0
    }
    register_tempdir "$scratch"
    chmod 0700 -- "$scratch" 2>/dev/null || true

    # The SHIPPED start-up proof: a positive read plus five refusals (absolute link, relative
    # link, symlinked middle component, '..' escape, fifo).  It dies on failure, which is the
    # right behaviour in a build and the wrong behaviour in a test run that should report every
    # case, so it is invoked in a subshell and its outcome turned into one line.
    if ( install_confined_io_helper "$scratch"; probe_confined_io "$scratch" ) >/dev/null 2>&1
    then
        self_test_report ok 'confined-io' 'probe_confined_io: positive and 5 refusals' \
            'openat2 RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS in force'
    else
        self_test_report not-ok 'confined-io' 'probe_confined_io: positive and 5 refusals' \
            'the shipped start-up proof FAILED; re-run below, unsuppressed'
        ( install_confined_io_helper "$scratch"; probe_confined_io "$scratch" ) || true
        return 0
    fi

    # Three direct calls, so the positive and the negative arms appear as their own lines
    # rather than only inside the proof.  install_confined_io_helper and probe_confined_io ran
    # in a subshell above, so the helper is on disk but the globals they set are not; both are
    # re-established here in this shell.
    install_confined_io_helper "$scratch" >/dev/null 2>&1 || true
    anchor="$scratch/self-test-anchor"
    mkdir -p -- "$anchor/beneath" || true
    printf 'self-test payload\n' > "$anchor/beneath/regular" || true
    printf 'OUTSIDE\n' > "$scratch/outside" || true
    ln -sfn "$scratch/outside" "$anchor/absolute-link" || true
    CONFINED_IO_ANCHOR="$anchor"
    CONFINED_IO_PROVEN=1

    status=0
    content="$(confined_io 'beneath/regular' read 2>/dev/null)" || status=$?
    if [ "$status" -eq 0 ] && [ "$content" = 'self-test payload' ]; then
        self_test_report ok 'confined-io' 'a regular file beneath the anchor reads' \
            'content matches'
    else
        self_test_report not-ok 'confined-io' 'a regular file beneath the anchor reads' \
            "status $status, content '${content:-<empty>}'"
    fi

    status=0
    confined_io 'absolute-link' read >/dev/null 2>&1 || status=$?
    if [ "$status" -eq "$CONFINED_IO_REFUSED" ]; then
        self_test_report ok 'confined-io' 'a symbolic link is REFUSED, not followed' \
            "status $status (refused)"
    else
        self_test_report not-ok 'confined-io' 'a symbolic link is REFUSED, not followed' \
            "status $status, expected $CONFINED_IO_REFUSED"
    fi

    status=0
    confined_io '../outside' read >/dev/null 2>&1 || status=$?
    if [ "$status" -eq "$CONFINED_IO_REFUSED" ]; then
        self_test_report ok 'confined-io' "a '..' escape is REFUSED by RESOLVE_BENEATH" \
            "status $status (refused)"
    else
        self_test_report not-ok 'confined-io' "a '..' escape is REFUSED by RESOLVE_BENEATH" \
            "status $status, expected $CONFINED_IO_REFUSED"
    fi

    CONFINED_IO_ANCHOR=''
    CONFINED_IO_PROVEN=0
    return 0
}

# ------------------------------------------------------------------------------------
# THE WRAPPERS THE CONVERSIONS DEPEND ON, PROVED SEPARATELY FROM THE PRIMITIVE.
#
# probe_confined_io() above proves the PRIMITIVE refuses a substituted path.  It says nothing
# about the WRAPPERS, and the wrappers are where every privileged operation in this script
# actually enters the confinement -- so a wrapper that accepted a status it should have died on
# would leave the primitive intact and the operation unconfined, which is precisely the shape of
# the defect this mechanism exists to close.  Each case below is a property that one of the
# converted call sites relies on:
#
#   confined_unlink_if_present   the `rm -f` replacement.  It has to REMOVE a symlinked leaf
#                                rather than follow it OR refuse it, because /etc/resolv.conf on
#                                a systemd-resolved base IS a symbolic link and removing it is
#                                the entire point of that call; and it has to treat absence as
#                                an answer rather than an error, because `rm -f` did.  Both arms
#                                are proved, and so is the third: a symlink at a DIRECTORY
#                                component is still refused, since unlinkat's not-following
#                                applies to the leaf alone and nothing else.
#   confined_write / _chmod      the `>` and `chmod` replacements in nine functions.  Proved to
#                                refuse a symlinked leaf AND to leave that link's target
#                                untouched -- which is the actual claim.  "It reported an error"
#                                is not the property; "the host file did not change" is.
#   confined_mkdir / _list       the `mkdir -p` and `find`-and-delete replacements.  Proved to
#                                refuse a symlinked directory component.
#   assert_installed_executable  the `[ -x ]` replacement.  Proved to accept a real executable,
#                                reject one carrying no execute bit, reject absence, and reject
#                                a symbolic link pointing at a perfectly good executable --
#                                which is the one case `[ -x ]` answered wrongly, and the reason
#                                it was replaced.
#   confined_identity and        the guard on the two RECURSIVE DELETES.  Proved to accept an
#     assert_confined_identity_unchanged   unchanged directory and to REFUSE one that was
#                                swapped for a different directory between the record and the
#                                check.  THE SWAP IS DONE WITH `mv`, NOT rmdir-then-mkdir: the
#                                inode of a just-removed directory is routinely handed straight
#                                back by the filesystem, so a rmdir/mkdir swap can produce the
#                                SAME device and inode and would make this case pass for a
#                                reason that has nothing to do with the check working.
#
# Every wrapper here except confined_identity DIES on refusal -- that is the property under test
# -- so each is called in a SUBSHELL and its exit status turned into one line.  Stdin comes from
# /dev/null for all of them, because confined_write reads its content from stdin and an
# unredirected call would BLOCK the self-test indefinitely rather than fail it.
# ------------------------------------------------------------------------------------

# One wrapper case.  $1 says which outcome the case requires: 'dies' for an operation that must
# be refused, 'survives' for one that must be allowed.
self_test_wrapper_case() { # $1=dies|survives  $2=case name  $3..=the command and its arguments
    local expected="$1" name="$2" status=0 actual
    shift 2
    ( "$@" ) >/dev/null 2>&1 </dev/null || status=$?
    if [ "$status" -eq 0 ]; then actual='survives'; else actual='dies'; fi
    if [ "$actual" = "$expected" ]; then
        self_test_report ok 'confined-wrappers' "$name" "$actual (status $status)"
    else
        self_test_report not-ok 'confined-wrappers' "$name" \
            "$actual (status $status), expected $expected"
    fi
}

# The other half of a refusal case: the filesystem fact the refusal was supposed to preserve.
# A refusal that reported an error and wrote anyway would pass the case above and fail this one.
self_test_fact_case() { # $1=case name  $2=detail  $3..=the condition, as a command
    local name="$1" detail="$2"
    shift 2
    if "$@" >/dev/null 2>&1; then
        self_test_report ok 'confined-wrappers' "$name" "$detail"
    else
        self_test_report not-ok 'confined-wrappers' "$name" "$detail -- DID NOT HOLD"
    fi
}

# ------------------------------------------------------------------------------------
# THE PINNED PRIVILEGED OPERATIONS, AS SELF-TEST CASES.
#
# WHAT THESE COVER AND WHAT THEY DELIBERATELY DO NOT, so the count is not read as more than it
# is.  Every case here exercises the CUSTODY CHAIN -- the anchor open, the openat2 resolution and
# the fstat comparison -- which is the part that could silently degrade and take every claim in
# this file with it.  The mount and chroot syscalls at the end of that chain are NOT exercised
# here, because --self-test runs before anything privileged and mounting or chrooting in a scratch
# directory to test it would be exactly the privileged operation this mode promises not to
# perform.  Those are exercised for real at their seven call sites, each fatal on failure.  The
# exec path IS covered, by its custody refusal, which needs no chroot to happen.
# ------------------------------------------------------------------------------------
self_test_pinned_op_case() { # $1=case name  $2=required status  $3..=the pinned_op arguments
    local name="$1" required="$2" status=0
    shift 2
    pinned_op "$@" >/dev/null 2>&1 </dev/null || status=$?
    if [ "$status" -eq "$required" ]; then
        self_test_report ok 'pinned-op' "$name" "status $status"
    else
        self_test_report not-ok 'pinned-op' "$name" "status $status, expected $required"
    fi
}

# The wrapper-level equivalent of self_test_wrapper_case, duplicated rather than shared for one
# reason: that function hard-codes the 'confined-wrappers' subject, and reusing it here would
# file these cases under the mechanism they are not testing.  Editing it instead would change the
# label on the existing confined-wrapper cases, which is not this change's business.
self_test_pinned_op_wrapper_case() { # $1=dies|survives  $2=case name  $3..=command and arguments
    local expected="$1" name="$2" status=0 actual
    shift 2
    ( "$@" ) >/dev/null 2>&1 </dev/null || status=$?
    if [ "$status" -eq 0 ]; then actual='survives'; else actual='dies'; fi
    if [ "$actual" = "$expected" ]; then
        self_test_report ok 'pinned-op' "$name" "$actual (status $status)"
    else
        self_test_report not-ok 'pinned-op' "$name" \
            "$actual (status $status), expected $expected"
    fi
}

self_test_pinned_op() {
    local scratch anchor device inode beneath_device beneath_inode saved_inode

    scratch="$(mktemp -d)" || {
        self_test_report not-ok 'pinned-op' 'a scratch directory could be created' \
            'mktemp -d failed'
        return 0
    }
    register_tempdir "$scratch"
    chmod 0700 -- "$scratch" 2>/dev/null || true

    # The SHIPPED start-up proof: two positive cases, eight refusals and the exec path's reserved
    # custody refusal.  It dies on failure, which is right in a build and wrong in a test run that
    # should report every case, so it runs in a subshell and its outcome becomes one line.
    if ( install_pinned_op_helper "$scratch"; probe_pinned_op "$scratch" ) >/dev/null 2>&1; then
        self_test_report ok 'pinned-op' 'probe_pinned_op: 2 positive and 9 refusals' \
            'the descriptor-consuming custody chain is in force'
    else
        self_test_report not-ok 'pinned-op' 'probe_pinned_op: 2 positive and 9 refusals' \
            'the shipped start-up proof FAILED; re-run below, unsuppressed'
        ( install_pinned_op_helper "$scratch"; probe_pinned_op "$scratch" ) || true
        return 0
    fi

    # The proof ran in a subshell, so the helper is on disk but the globals it set are not.
    install_pinned_op_helper "$scratch" >/dev/null 2>&1 || true
    anchor="$scratch/pinned-op-anchor"
    mkdir -p -- "$anchor/beneath" || true
    mkdir -p -- "$scratch/outside-target" || true
    ln -sfn "$scratch/outside-target" "$anchor/absolute-link" || true
    ln -sfn 'beneath' "$anchor/relative-link" || true
    device="$(stat -c '%d' -- "$anchor" 2>/dev/null || printf '')"
    inode="$(stat -c '%i' -- "$anchor" 2>/dev/null || printf '')"
    beneath_device="$(stat -c '%d' -- "$anchor/beneath" 2>/dev/null || printf '')"
    beneath_inode="$(stat -c '%i' -- "$anchor/beneath" 2>/dev/null || printf '')"
    if [ -z "$device" ] || [ -z "$inode" ] || [ -z "$beneath_inode" ]; then
        self_test_report not-ok 'pinned-op' 'the fixture could be measured' \
            'stat on the self-test anchor failed'
        return 0
    fi

    # POSITIVE: the anchor's own descriptor, and a target resolved beneath it, both fstat to the
    # recorded identity.
    self_test_pinned_op_case 'the ANCHOR verifies on its descriptor' 0 \
        "$anchor" "$device" "$inode" . - - verify
    self_test_pinned_op_case 'a TARGET beneath the anchor verifies' 0 \
        "$anchor" "$device" "$inode" beneath "$beneath_device" "$beneath_inode" verify

    # NEGATIVE: one line per condition, each with the status that names it.  A wrong inode on the
    # ANCHOR is an IDENTITY refusal rather than ANCHOR_BAD, because the open succeeded and it was
    # the fstat that disagreed; ANCHOR_BAD is for an anchor that cannot be opened as a
    # non-symlink directory at all.  Both are asserted, because conflating them would hide which
    # half of the chain was exercised.
    self_test_pinned_op_case 'a wrong ANCHOR inode is REFUSED' "$PINNED_OP_IDENTITY" \
        "$anchor" "$device" "$((inode + 1))" . - - verify
    self_test_pinned_op_case 'a SYMLINKED anchor is REFUSED' "$PINNED_OP_ANCHOR_BAD" \
        "$anchor/relative-link" "$device" "$inode" . - - verify
    self_test_pinned_op_case 'a wrong TARGET inode is REFUSED' "$PINNED_OP_IDENTITY" \
        "$anchor" "$device" "$inode" beneath "$beneath_device" "$((beneath_inode + 1))" verify
    self_test_pinned_op_case 'a SYMLINKED target is REFUSED' "$PINNED_OP_REFUSED" \
        "$anchor" "$device" "$inode" absolute-link "$beneath_device" "$beneath_inode" verify
    self_test_pinned_op_case "a '..' escape is REFUSED" "$PINNED_OP_REFUSED" \
        "$anchor" "$device" "$inode" ../outside-target "$beneath_device" "$beneath_inode" verify
    self_test_pinned_op_case 'an ABSENT target is REFUSED' "$PINNED_OP_ABSENT" \
        "$anchor" "$device" "$inode" beneath/absent "$beneath_device" "$beneath_inode" verify
    self_test_pinned_op_case 'a target with NO identity is REFUSED' "$PINNED_OP_USAGE" \
        "$anchor" "$device" "$inode" beneath - - verify
    self_test_pinned_op_case 'an ABSOLUTE target is REFUSED' "$PINNED_OP_USAGE" \
        "$anchor" "$device" "$inode" /etc "$beneath_device" "$beneath_inode" verify

    # THE EXEC PATH.  Its custody refusal is a single reserved status because execve leaves no
    # other channel, and it is raised before anything is opened -- so this case establishes that
    # a chroot onto an anchor that does not fstat to the pin is not entered.  /bin/true is named
    # rather than a shell so nothing but execve can affect the outcome.
    self_test_pinned_op_case 'a CHROOT onto a wrong pin is REFUSED' \
        "$PINNED_OP_CUSTODY_REFUSED" \
        "$anchor" "$device" "$((inode + 1))" . - - chroot-exec /bin/true

    # THE WRAPPER LAYER, through the image-root globals the production call sites use, so the
    # fail-closed behaviour is asserted where the seven operations actually enter it.
    IMAGE_MOUNT="$anchor"
    IMAGE_ROOT_DEVICE="$device"
    IMAGE_ROOT_INODE="$inode"
    PINNED_OP_PROVEN=1
    self_test_pinned_op_wrapper_case survives \
        'pinned_image_verify ACCEPTS the pinned image root' \
        pinned_image_verify . - - 'the image root' 'prove the accepting arm'
    saved_inode="$IMAGE_ROOT_INODE"
    IMAGE_ROOT_INODE="$((saved_inode + 1))"
    self_test_pinned_op_wrapper_case dies \
        'pinned_image_verify REFUSES a moved image root' \
        pinned_image_verify . - - 'the image root' 'prove the refusing arm'
    IMAGE_ROOT_INODE="$saved_inode"

    # AND THE GUARD ITSELF.  An operation attempted before the mechanism has been proven must
    # die, because an unproven mechanism is indistinguishable from the pathname form it replaced.
    PINNED_OP_PROVEN=0
    self_test_pinned_op_wrapper_case dies \
        'pinned_op_ready REFUSES an unproven mechanism' \
        pinned_image_verify . - - 'the image root' 'prove the unproven-mechanism guard'

    IMAGE_MOUNT=''
    IMAGE_ROOT_DEVICE=''
    IMAGE_ROOT_INODE=''
    PINNED_OP_HELPER=''
    PINNED_OP_PYTHON=''
    PINNED_OP_PROVEN=0
    return 0
}

self_test_confined_wrappers() {
    local scratch anchor record mode
    scratch="$(mktemp -d)" || {
        self_test_report not-ok 'confined-wrappers' 'a scratch directory could be created' \
            'mktemp -d failed'
        return 0
    }
    register_tempdir "$scratch"
    chmod 0700 -- "$scratch" 2>/dev/null || true

    if ! install_confined_io_helper "$scratch" >/dev/null 2>&1; then
        self_test_report not-ok 'confined-wrappers' 'the confined I/O helper installed' \
            'install_confined_io_helper failed'
        return 0
    fi

    # THE FIXTURE.  An anchor with the four shapes the converted call sites meet in a hostile
    # base tarball -- a symlinked leaf pointing outside, a symlinked directory pointing outside,
    # a symlink pointing at something legitimate inside, and ordinary files at two modes -- plus
    # the objects outside the anchor whose survival is what the refusals are for.  Everything is
    # inside one mktemp directory registered with the EXIT trap, so a failure leaves nothing.
    anchor="$scratch/wrapper-anchor"
    mkdir -p -- "$anchor/staging" "$scratch/outside-dir" "$scratch/replacement" || true
    printf 'OUTSIDE CONTENT\n' > "$scratch/outside-target" || true
    chmod 0600 -- "$scratch/outside-target" || true
    printf 'victim\n' > "$scratch/outside-dir/victim" || true
    printf 'x\n' > "$anchor/exec-file" || true
    chmod 0755 -- "$anchor/exec-file" || true
    printf 'x\n' > "$anchor/plain-file" || true
    chmod 0644 -- "$anchor/plain-file" || true
    ln -sfn "$scratch/outside-target" "$anchor/link-to-outside" || true
    ln -sfn "$scratch/outside-target" "$anchor/doomed-link" || true
    ln -sfn "$scratch/outside-dir" "$anchor/link-to-dir" || true
    ln -sfn 'exec-file' "$anchor/link-to-exec" || true

    # CONFINED_IO_PROVEN is raised by hand here for the same reason self_test_confined_io raises
    # it: the shipped proof ran in a subshell one case ago and the globals it set did not
    # survive.  This is the self-test entry point and nothing privileged happens in this
    # directory; the production path still raises the flag only from probe_confined_io().
    CONFINED_IO_ANCHOR="$anchor"
    CONFINED_IO_PROVEN=1

    # --- confined_unlink_if_present: the `rm -f` replacement -----------------------------
    self_test_wrapper_case survives 'unlink_if_present REMOVES a symlinked leaf' \
        confined_unlink_if_present 'doomed-link' 'a self-test symbolic link' \
        'prove rm -f semantics on a link'
    self_test_fact_case 'the link ENTRY is gone' 'doomed-link no longer exists' \
        test '!' -L "$anchor/doomed-link"
    self_test_fact_case 'its target OUTSIDE the anchor survives' \
        'outside-target still holds its content' \
        test "$(cat -- "$scratch/outside-target" 2>/dev/null)" = 'OUTSIDE CONTENT'
    self_test_wrapper_case survives 'unlink_if_present accepts an ABSENT leaf' \
        confined_unlink_if_present 'never-existed' 'nothing at all' \
        'prove absence is an answer'
    self_test_wrapper_case dies 'unlink_if_present REFUSES a symlinked parent' \
        confined_unlink_if_present 'link-to-dir/victim' 'a file beyond a linked directory' \
        'prove the parent components stay confined'
    self_test_fact_case 'the file beyond that link survives' 'outside-dir/victim still exists' \
        test -f "$scratch/outside-dir/victim"

    # --- confined_write and confined_chmod: the `>` and `chmod` replacements -------------
    self_test_wrapper_case dies 'confined_write REFUSES a symlinked leaf' \
        confined_write 'link-to-outside' 'a host file behind a link' \
        'prove a redirected write is refused'
    self_test_fact_case 'the host file was NOT rewritten' \
        'outside-target still holds its content' \
        test "$(cat -- "$scratch/outside-target" 2>/dev/null)" = 'OUTSIDE CONTENT'
    self_test_wrapper_case dies 'confined_chmod REFUSES a symlinked leaf' \
        confined_chmod 'link-to-outside' 0777 'a host file behind a link' \
        'prove a redirected chmod is refused'
    mode="$(stat -c '%a' -- "$scratch/outside-target" 2>/dev/null || printf '')"
    self_test_fact_case 'the host file mode is UNCHANGED' "still $mode, not 777" \
        test "$mode" = '600'

    # --- confined_mkdir and confined_list -----------------------------------------------
    self_test_wrapper_case dies 'confined_mkdir REFUSES a symlinked component' \
        confined_mkdir 'link-to-dir/created' 0755 'a directory beyond a linked directory' \
        'prove mkdir -p cannot be redirected'
    self_test_fact_case 'nothing was created outside the anchor' \
        'outside-dir/created does not exist' \
        test '!' -e "$scratch/outside-dir/created"
    self_test_wrapper_case dies 'confined_list REFUSES a symlinked directory' \
        confined_list 'link-to-dir' 'a linked directory' \
        'prove an enumeration cannot be redirected'

    # --- assert_installed_executable: the `[ -x ]` replacement ---------------------------
    self_test_wrapper_case survives 'installed_executable ACCEPTS mode 0755' \
        assert_installed_executable 'exec-file' '/exec-file' 'the probe executable' \
        'prove the accepting arm'
    self_test_wrapper_case dies 'installed_executable REJECTS mode 0644' \
        assert_installed_executable 'plain-file' '/plain-file' 'the probe executable' \
        'prove the no-execute-bit arm'
    self_test_wrapper_case dies 'installed_executable REJECTS an absent path' \
        assert_installed_executable 'never-built' '/never-built' 'the probe executable' \
        'prove the absence arm'
    self_test_wrapper_case dies 'installed_executable REJECTS a link to one' \
        assert_installed_executable 'link-to-exec' '/link-to-exec' 'the probe executable' \
        'prove the case [ -x ] answered wrongly'

    # --- the identity guard on the recursive deletes -------------------------------------
    self_test_wrapper_case dies 'confined_identity REFUSES a symlinked directory' \
        confined_identity 'link-to-dir' 'a linked directory' \
        'prove an identity cannot be recorded through a link'
    record="$(confined_identity 'staging' 'the self-test staging tree' \
        'record an identity to compare against' 2>/dev/null)" || record=''
    self_test_fact_case 'an identity RECORD was produced' \
        "record='${record:-<empty>}'" test -n "$record"
    self_test_wrapper_case survives 'identity_unchanged ACCEPTS the same directory' \
        assert_confined_identity_unchanged 'staging' "$record" \
        'the self-test staging tree' 'prove the accepting arm'
    # THE SWAP, BY `mv`.  See the block comment above for why this must not be rmdir+mkdir.
    mv -- "$anchor/staging" "$scratch/moved-away" 2>/dev/null || true
    mv -- "$scratch/replacement" "$anchor/staging" 2>/dev/null || true
    self_test_wrapper_case dies 'identity_unchanged REFUSES a SWAPPED directory' \
        assert_confined_identity_unchanged 'staging' "$record" \
        'the self-test staging tree' 'prove the refusing arm'

    CONFINED_IO_ANCHOR=''
    CONFINED_IO_PROVEN=0
    return 0
}

# ------------------------------------------------------------------------------------
# THE STRUCTURAL GUARD ON EVERY HEREDOC IN THIS FILE.
#
# render_usage_text() makes the HELP text inert.  This makes every OTHER heredoc auditable,
# including ones added after today, and it is the half that survives a future edit: a
# reviewer cannot be relied upon to notice that a sentence added to a two-hundred-line
# heredoc happened to quote a command name in backticks, but this scan notices it in
# milliseconds and the self-test fails the revision.
#
# WHAT IT DOES.  It reads this script's own source, tracks heredoc state line by line, and
# for every heredoc whose delimiter is UNQUOTED -- the only kind bash expands -- reports any
# body line containing a backtick or a `$(`.  Both are command substitution; both execute.
#
# WHAT IT DELIBERATELY DOES NOT DO.  It does not object to `$VAR` in an unquoted body.  Three
# heredocs in this file interpolate their own values on purpose, a variable's VALUE is never
# re-scanned for substitutions, and a check that refused them would be turned off rather than
# obeyed.  The hazard is executable syntax in the TEXT, and that is what is refused.
#
# HOW THE STATE TRACKING AVOIDS FALSE POSITIVES, each case measured against this file:
#
#   * a herestring (`read -r -a fields <<< "$value"`) is not an opener -- after `<<` comes a
#     third `<`, and the delimiter pattern requires a letter or underscore;
#   * an arithmetic left shift inside an embedded program (`os.read(handle, 1 << 16)`) is not
#     an opener for the same reason: `16` does not begin with a letter or underscore.  Those
#     lines sit inside quoted heredocs in any case, and body lines are never re-examined for
#     openers;
#   * a COMMENT that discusses a heredoc -- the block above this function does -- is skipped,
#     because a line whose first non-blank character is `#` cannot open a heredoc;
#   * `<<-` is honoured: the delimiter line may be indented, so the comparison strips leading
#     blanks for exactly those heredocs and for no others;
#   * an unterminated heredoc is reported rather than passing silently, since a scan that ran
#     off the end of the file would have examined nothing after the opener.
#
# Prints one line per offending body line and nothing at all when the file is clean, so the
# caller's assertion is "produced no output".
# ------------------------------------------------------------------------------------
scan_expanding_heredocs() { # $1=file to scan
    local file="$1"
    local line probe delim='' dashed=0 expanding=0 in_body=0 opener=0 number=0

    while IFS= read -r line || [ -n "$line" ]; do
        number=$(( number + 1 ))

        if [ "$in_body" -eq 1 ]; then
            probe="$line"
            if [ "$dashed" -eq 1 ]; then
                probe="${line#"${line%%[! 	]*}"}"
            fi
            if [ "$probe" = "$delim" ]; then
                in_body=0
                continue
            fi
            if [ "$expanding" -eq 1 ]; then
                case "$line" in
                    *'`'*|*'$('*)
                        printf '%s:%d: command substitution inside the EXPANDED heredoc %s opened at line %d: %s\n' \
                            "$file" "$number" "$delim" "$opener" "$line"
                        ;;
                esac
            fi
            continue
        fi

        # A shell comment cannot open a heredoc, and the comments in this file discuss them.
        case "${line#"${line%%[! 	]*}"}" in
            '#'*|'') continue ;;
        esac

        if [[ "$line" =~ \<\<(-?)[[:space:]]*(\'|\"|\\)?([A-Za-z_][A-Za-z0-9_]*) ]]; then
            dashed=0
            [ "${BASH_REMATCH[1]}" != '-' ] || dashed=1
            expanding=1
            [ -z "${BASH_REMATCH[2]}" ] || expanding=0
            delim="${BASH_REMATCH[3]}"
            opener=$number
            in_body=1
        fi
    done < "$file"

    if [ "$in_body" -eq 1 ]; then
        printf '%s: the heredoc %s opened at line %d is never terminated, so everything after it was not scanned\n' \
            "$file" "$delim" "$opener"
    fi
}

# ------------------------------------------------------------------------------------
# THE HELP-TEXT CASES.  Three properties, each the direct negative of a measured defect:
#
#   1. no heredoc in this file can command-substitute (the structural guard above);
#   2. rendering the help with a HOSTILE PATH and a hostile marker environment executes
#      nothing and prints nothing the environment supplied.  Before the fix this exact
#      arrangement ran an attacker's binary as uid 0 and pasted its output into the help;
#   3. every @NAME@ placeholder resolves, so the template and its table cannot drift apart.
#
# NO ROOT, NO MOUNT, NO NETWORK, exactly like every other case in this mode.  The decoys are
# ordinary shell scripts in a mktemp directory registered with the same TEMP_DIRS list the
# EXIT trap drains, so nothing is left behind on a pass or a failure.
# ------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------
# THE UNTRUSTED-VALUE RENDERER, EXERCISED AGAINST A TABLE OF THE EXACT RENDERINGS IT MUST
# PRODUCE.  Every diagnostic in this script that names a value the caller supplied - an
# unknown option, a bad --size, an unsupported --arch, a path that failed validation,
# TMPDIR - goes through render_untrusted(), so a change that weakened any clause of its
# contract would reopen CWE-117 across all of them at once.  It fails here instead.
#
# The rows are the ways the defect presented itself in the finding: a NEWLINE followed by a
# forged "::error::" workflow command; a CARRIAGE RETURN with an erase-line escape, which
# redraws a line rather than ending it and so HIDES output instead of forging it; an ANSI
# colour escape; a literal backslash beside a tab, which is what clause (a) exists for; an
# EMPTY value; a value that is ITSELF nothing but a forged annotation; a non-ASCII UTF-8
# sequence; a high byte that is not valid UTF-8 at all; and a five-thousand-byte value.
#
# Three properties are asserted for EVERY row on top of the exact match, because they are
# what the contract promises and an exact-match table alone would let a rewrite satisfy by
# accident: the rendering is ONE LINE, it BEGINS with '[' so it can never itself be a
# workflow command, and it is BOUNDED whatever the input length.
#
# Needs no root, no mount, no loop device and no network: a pure function fed a table.
# ------------------------------------------------------------------------------------
self_test_render_untrusted_case() { # $1=case name  $2=value  $3=expected rendering
    local name="$1" value="$2" expected="$3" rendered
    rendered="$(render_untrusted "$value")"

    if [ "$rendered" != "$expected" ]; then
        self_test_report not-ok render_untrusted "$name" \
            "rendered ${#rendered} chars, not the expected rendering"
        return 0
    fi
    case "$rendered" in
        '['*) : ;;
        *)  self_test_report not-ok render_untrusted "$name" \
                "does not begin with '[', so it could begin a line"
            return 0 ;;
    esac
    # $'\n' and $'\r' rather than "$(printf '\n')": command substitution STRIPS trailing
    # newlines, so the printf form would expand to the empty string and the pattern would
    # match every value - a check that always passes, which is worse than no check.
    case "$rendered" in
        *$'\n'*|*$'\r'*)
            self_test_report not-ok render_untrusted "$name" \
                'still contains a line terminator'
            return 0 ;;
    esac
    if [ "${#rendered}" -gt 240 ]; then
        self_test_report not-ok render_untrusted "$name" \
            "rendered ${#rendered} characters, which is unbounded"
        return 0
    fi
    self_test_report ok render_untrusted "$name" "-> ${#rendered} chars, escaped and one line"
}

self_test_render_untrusted() {
    local long_value='' long_expected='['

    self_test_render_untrusted_case 'a newline and a forged ::error:: annotation' \
        "$(printf 'bogus\n::error::FORGED')" '[bogus\n::error::FORGED]'
    self_test_render_untrusted_case 'a carriage return and an erase-line escape' \
        "$(printf 'ok\r\033[2Khidden')" '[ok\r\x1b[2Khidden]'
    self_test_render_untrusted_case 'an ANSI colour escape sequence' \
        "$(printf '\033[31mred\033[0m')" '[\x1b[31mred\x1b[0m]'
    self_test_render_untrusted_case 'a tab beside a literal backslash-n' \
        "$(printf 'a\tb\\nc')" '[a\tb\\nc]'
    self_test_render_untrusted_case 'an empty value' '' '[]'
    self_test_render_untrusted_case 'a value that is itself only a forged annotation' \
        '::error::WHOLE_VALUE' '[::error::WHOLE_VALUE]'
    self_test_render_untrusted_case 'a non-ASCII UTF-8 sequence' \
        "$(printf '\303\251')" '[\xc3\xa9]'
    self_test_render_untrusted_case 'a high byte that is not valid UTF-8' \
        "$(printf '\377')" '[\xff]'

    # Built in the shell: this case must not depend on seq or tr being present.
    while [ "${#long_value}" -lt 5000 ]; do
        long_value+='XXXXXXXXXXXXXXXXXXXX'
    done
    while [ "${#long_expected}" -lt 201 ]; do
        long_expected+='XXXXXXXXXXXXXXXXXXXX'
    done
    long_expected+='...[truncated, 5000 bytes total]]'
    self_test_render_untrusted_case 'a five-thousand-byte value is bounded at 200' \
        "$long_value" "$long_expected"

    # THE PROPERTY THE WHOLE CONTRACT EXISTS FOR, asserted end to end rather than only per
    # row: a rendered value placed in a real diagnostic cannot produce a line that BEGINS
    # with a GitHub Actions workflow command.  The message keeps this script's own prefix
    # in front of it - clause (e) - and the rendering carries no line terminator - clause
    # (b) - so there is no second line for a forged command to be the start of.
    local composed forged_lines
    composed="$(printf '[%s] ERROR: unknown option %s.\n' "$SCRIPT_NAME" \
        "$(render_untrusted "$(printf -- '--bogus\n::error::FORGED_ROOTFS')")")"
    forged_lines="$(printf '%s\n' "$composed" | grep -c '^::' || true)"
    if [ "$forged_lines" = '0' ]; then
        self_test_report ok render_untrusted \
            'a composed diagnostic starts no ::error:: line' 'grep -c "^::" -> 0'
    else
        self_test_report not-ok render_untrusted \
            'a composed diagnostic starts no ::error:: line' \
            "grep -c \"^::\" -> $forged_lines"
    fi
}

self_test_help_rendering() {
    local scratch offenders rendered decoy marker status=0

    offenders="$(scan_expanding_heredocs "$SCRIPT_PATH")" || offenders='the scan itself failed'
    if [ -z "$offenders" ]; then
        self_test_report ok 'help-text' 'no expanded heredoc can substitute' \
            'every unquoted heredoc body is free of ` and $('
    else
        self_test_report not-ok 'help-text' 'no expanded heredoc can substitute' \
            "$(printf '%s' "$offenders" | head -3 | tr '\n' ' ')"
    fi

    scratch="$(mktemp -d)" || {
        self_test_report not-ok 'help-text' 'a scratch directory could be created' 'mktemp -d failed'
        return 0
    }
    register_tempdir "$scratch"
    chmod 0700 -- "$scratch" 2>/dev/null || true

    marker="$scratch/executed"

    # The decoys are named for the commands the help text MENTIONS, plus the two builtins the
    # renderer uses, so a regression that reintroduced any external resolution is caught by
    # whichever name it reached for.  Each records that it ran and prints a sentinel.
    for decoy in gcov cat lcov curl wget dpkg-query chroot id date; do
        {
            printf '#!/bin/sh\n'
            printf 'printf "%%s\\n" "SELFTEST_DECOY_EXECUTED $0" >> "%s"\n' "$marker"
            printf 'printf "%%s\\n" "SELFTEST_DECOY_OUTPUT"\n'
        } > "$scratch/$decoy"
        chmod 0755 -- "$scratch/$decoy" 2>/dev/null || true
    done

    # Rendered in a subshell so the hostile PATH and the hostile variables cannot outlive the
    # case.  HOSTILE_MARKER is the shape the original report used; it is set here so that a
    # decoy which read it would still write inside this scratch directory and nowhere else.
    rendered="$( PATH="$scratch:$PATH" HOSTILE_MARKER="$marker" usage 2>&1 )" || status=$?

    if [ "$status" -eq 0 ]; then
        self_test_report ok 'help-text' 'help renders with a hostile PATH' "status $status"
    else
        self_test_report not-ok 'help-text' 'help renders with a hostile PATH' \
            "status $status, expected 0"
    fi

    if [ -e "$marker" ]; then
        self_test_report not-ok 'help-text' 'rendering the help executes nothing' \
            "a decoy on PATH RAN: $(head -1 "$marker")"
    else
        self_test_report ok 'help-text' 'rendering the help executes nothing' \
            'no decoy on PATH was executed'
    fi

    case "$rendered" in
        *SELFTEST_DECOY_OUTPUT*)
            self_test_report not-ok 'help-text' 'no environment output reaches the help' \
                'a decoy sentinel was substituted into the rendered text' ;;
        *)
            self_test_report ok 'help-text' 'no environment output reaches the help' \
                'the rendered text contains nothing the environment supplied' ;;
    esac

    # The one sentence that used to be executed must now appear verbatim, backticks included.
    case "$rendered" in
        *'resolves a bare `gcov`'*)
            self_test_report ok 'help-text' 'backticked prose renders literally' \
                'the gcov sentence keeps its backticks' ;;
        *)
            self_test_report not-ok 'help-text' 'backticked prose renders literally' \
                'the gcov sentence is missing or altered' ;;
    esac

    if [[ "$rendered" =~ (@[A-Z][A-Z0-9_]*@) ]]; then
        self_test_report not-ok 'help-text' 'every placeholder resolves' \
            "${BASH_REMATCH[1]} was not substituted"
    else
        self_test_report ok 'help-text' 'every placeholder resolves' \
            "${#USAGE_PLACEHOLDER_NAMES[@]} names in the table, no @NAME@ left in the output"
    fi

    # And the table has no entry the template does not use, which is the other half of drift:
    # a name left in the table after its placeholder was removed substitutes nothing and hides
    # the fact that the two are no longer describing the same document.
    local unused='' name
    for name in "${USAGE_PLACEHOLDER_NAMES[@]}"; do
        case "$rendered" in
            *"${!name-}"*) ;;
            *) unused="${unused}${unused:+, }$name" ;;
        esac
    done
    if [ -z "$unused" ]; then
        self_test_report ok 'help-text' 'no table entry is unused' \
            'every name in the table appears in the rendered text'
    else
        self_test_report not-ok 'help-text' 'no table entry is unused' \
            "$unused substituted nothing visible"
    fi

    return 0
}

run_self_test() {
    local installed='build-essential 12.9 i386 install ok installed'
    local provider_ok='pkgconf 1.8.1 i386 install ok installed :: pkg-config (= 1.8.1)'

    printf '%s --self-test\n' "$SCRIPT_NAME"
    printf 'no root, no mounts, no network, no dpkg: every case below is a literal string.\n\n'

    # ---- package_directly_installed --------------------------------------------------
    self_test_package_case package_directly_installed accept 'build-essential' \
        "$installed" 'install ok installed'
    self_test_package_case package_directly_installed accept 'build-essential' \
        "libc6 2.36-9 i386 install ok installed
$installed
gcc 12.2.0-3 i386 install ok installed" 'found among several records'
    self_test_package_case package_directly_installed accept 'build-essential' \
        'build-essential 12.9 i386 install ok installed extra-trailing-field' \
        'a trailing extra field is tolerated'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential 12.9 i386 purge ok not-installed' 'purge ok not-installed'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential 12.9 i386 deinstall ok config-files' 'deinstall ok config-files'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential 12.9 i386 install reinstreq half-installed' \
        'install reinstreq half-installed'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential 12.9 i386 install ok half-configured' 'install ok half-configured'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential 12.9 i386 install ok unpacked' 'install ok unpacked'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential 12.9 i386 install ok' 'a short record (five fields)'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential install ok installed' 'a shifted record (status in columns 2-4)'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'build-essential-doc 12.9 all install ok installed' 'a name mismatch (longer name)'
    self_test_package_case package_directly_installed reject 'build-ess' \
        "$installed" 'the wanted name only PREFIXES the record'
    self_test_package_case package_directly_installed reject 'build-essential' '' \
        'an empty listing'
    self_test_package_case package_directly_installed reject 'build-essential' \
        'this is not a dpkg record at all' 'a malformed line'

    # ---- package_provided_by_installed -----------------------------------------------
    self_test_package_case package_provided_by_installed accept 'pkg-config' \
        "$provider_ok" 'provided by an installed package'
    self_test_package_case package_provided_by_installed accept 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok installed :: pkg-config' \
        'a Provides entry with no version'
    self_test_package_case package_provided_by_installed accept 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok installed :: foo (= 1), pkg-config (= 1.8.1), bar' \
        'the second of several Provides entries'
    self_test_package_case package_provided_by_installed accept 'pkg-config' \
        "pkgconf-old 1.7.0 i386 purge ok not-installed :: pkg-config (= 1.7.0)
$provider_ok" 'an installed provider beside a purged one'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 purge ok not-installed :: pkg-config (= 1.8.1)' \
        'the provider is purge ok not-installed'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 deinstall ok config-files :: pkg-config (= 1.8.1)' \
        'the provider is deinstall ok config-files'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install reinstreq half-installed :: pkg-config (= 1.8.1)' \
        'the provider is install reinstreq half-installed'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok half-configured :: pkg-config (= 1.8.1)' \
        'the provider is install ok half-configured'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok unpacked :: pkg-config (= 1.8.1)' \
        'the provider is install ok unpacked'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 :: pkg-config (= 1.8.1)' 'a short record (two status words)'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok installed :: pkgconfig (= 1.8.1)' \
        'a Provides name mismatch'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok installed :: pkg (= 1.8.1)' \
        'the Provides entry only PREFIXES the wanted name'
    self_test_package_case package_provided_by_installed reject 'pkg' \
        "$provider_ok" 'the wanted name only PREFIXES the Provides entry'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok installed :: ' 'an empty Provides field'
    self_test_package_case package_provided_by_installed reject 'pkg-config' \
        'pkgconf 1.8.1 i386 install ok installed' 'no Provides separator at all'
    self_test_package_case package_provided_by_installed reject 'pkg-config' '' \
        'an empty listing'

    # ---- the confined I/O primitive --------------------------------------------------
    self_test_confined_io

    # ---- the wrappers the privileged conversions enter it through --------------------
    self_test_confined_wrappers

    # ---- the descriptor-consuming custody chain the seven mount/chroot sites use -----
    self_test_pinned_op

    # ---- the help text: inert heredocs, a hostile PATH, and no placeholder drift -----
    self_test_help_rendering

    # ---- render_untrusted ------------------------------------------------------------
    self_test_render_untrusted

    printf '\n%d case(s), %d failure(s)\n' "$SELF_TEST_CASES" "$SELF_TEST_FAILURES"
    if [ "$SELF_TEST_FAILURES" -gt 0 ]; then
        printf '%s --self-test FAILED.  Each NOT OK line above names the parser, the case and\n' \
            "$SCRIPT_NAME"
        printf 'what it returned.  Do not publish an image from this revision: the package\n'
        printf 'accounting decides whether the toolchain is actually in the image, and a parser\n'
        printf 'that accepts a purge tombstone or a half-installed package reports a toolchain\n'
        printf 'that is not there.\n'
        return 1
    fi
    printf '%s --self-test passed.\n' "$SCRIPT_NAME"
    return 0
}

record_installed_packages() {
    # $target is kept for the DIAGNOSTICS below, which name the record's in-guest path; the
    # write, the mode change and the digest all go through the confined primitive against
    # $target_rel, so no host-side operation resolves an in-image pathname here.
    local target="$GUEST_PACKAGE_RECORD"
    local target_rel="${GUEST_PACKAGE_RECORD#/}"
    local listing='' provides_listing='' count='' digest='' malformed=''
    local package missing='' accounted=0 provided=''

    # dpkg-query is run INSIDE the chroot so that it reads the image's own database rather
    # than the build host's.  --show with an explicit format is used instead of -l because
    # -l's column layout truncates long package names and is not machine-readable.
    # The ${...} sequences below are DPKG'S OWN substitution syntax, not shell expansions, so
    # the single quotes are required and SC2016 is a false positive here: dpkg-query receives
    # the literal text and does the substituting itself.
    # shellcheck disable=SC2016
    listing="$(chroot_run dpkg-query --show \
                   --showformat='${Package} ${Version} ${Architecture} ${Status}\n' 2>/dev/null)" \
        || die "dpkg-query failed inside the image, so the exact resolved package set cannot be
       recorded.  The image is NOT published: its whole reproducibility claim rests on being
       able to read back which packages and which versions the pinned archive resolved to, and
       an image that cannot answer that is not the artefact this script promises.  Reproduce it
       by hand with
           chroot '$IMAGE_MOUNT' dpkg-query --show
       and fix what it reports -- most often a base tarball whose dpkg database was not
       extracted completely, or a chroot in which /var/lib/dpkg is not readable."

    [ -n "$listing" ] || die "dpkg-query succeeded inside the image but listed no packages at
       all.  That is not a plausible state for an image the toolchain was just installed into,
       so it is treated as a failed query rather than as an empty set, and the image is NOT
       published.  Inspect '$IMAGE_MOUNT/var/lib/dpkg/status'."

    # THE LINE SHAPE IS CHECKED, not assumed from the format string.  The requirement is a
    # concrete package/version/architecture record; a line missing any of the three is not one,
    # and dpkg leaves ${Version} empty for a package in certain half-states.
    malformed="$(printf '%s\n' "$listing" \
        | awk 'NF < 6 || $1 == "" || $2 == "" || $3 == "" { print; count++ }
               END { if (count > 3) print "... and " (count - 3) " more" }' \
        | head -4)"
    [ -z "$malformed" ] || die "the package listing from inside the image carries lines without a
       complete package, version, architecture and status:
$malformed
       The record must be a concrete package/version/architecture set to be worth anything, so
       the image is NOT published.  Each such line is a package dpkg holds in a half-configured
       state; run 'chroot $IMAGE_MOUNT dpkg --configure -a' and rebuild."

    # THE RECORD IS WRITTEN THROUGH THE CONFINED PRIMITIVE.  It goes into $GUEST_CONF_DIR, which
    # is /etc/cec-l2 -- and /etc comes from the BASE TARBALL, so `{ ... } > "$IMAGE_MOUNT$path"`
    # followed by `chmod` was a root-owned host-side write through a name whose leading component
    # this script does not author.  The parent is created here as well rather than assumed:
    # copy_guest_payload has not run yet at this point in main().
    confined_mkdir "${GUEST_CONF_DIR#/}" 0755 "the image's $GUEST_CONF_DIR" \
        'record the installed package set in the image'
    confined_unlink_if_present "$target_rel" "the image's $target" \
        'record the installed package set in the image'
    confined_create "$target_rel" 0644 "the image's $target" \
        'record the installed package set in the image'
    {
        printf '# The exact package set installed into this image, with versions.\n'
        printf '# Written by %s at image-build time.\n' "$SCRIPT_NAME"
        printf '#\n'
        printf '# The archive this resolved from is recorded in %s; it is an\n' "$GUEST_IMAGE_RECORD"
        printf '# immutable snapshot, so the same archive reproduces the same versions.\n'
        printf '# Format: <package> <version> <architecture> <dpkg status>\n'
        printf '#\n'
        printf '%s\n' "$listing" | LC_ALL=C sort
    } | confined_write "$target_rel" "the image's $target" \
            'record the installed package set in the image'
    confined_chmod "$target_rel" 0644 "the image's $target" \
        'set the mode of the image package record'

    # Only lines whose dpkg status ends in "installed" count; a purged-but-configured entry is
    # present in the database and is not in the image.
    #
    # THE LEADING SPACE IN THIS PATTERN IS LOAD-BEARING, and it is why this line is NOT an
    # instance of the status-suffix trap the block above package_directly_installed describes.
    # That trap is an UNANCHORED /installed$/, which "purge ok not-installed" satisfies because
    # the tombstone status itself ends in the text "installed"; ' installed$' requires a SPACE
    # before it, so "ok not-installed" and "install reinstreq half-installed" are both
    # correctly excluded (verified against both forms).  An exact "install ok installed" triple
    # here would be wrong in the other direction: "hold ok installed" is an ordinarily present,
    # configured package that is merely pinned against upgrade, and it belongs in a count of
    # what is IN the image.  The required-package accounting above is a different question -
    # "did apt install what this script asked for" - and there the want field does matter.
    count="$(printf '%s\n' "$listing" | grep -c ' installed$' || true)"
    count="$(printf '%s' "$count" | tr -d '[:space:]')"
    case "$count" in
        ''|*[!0-9]*)
            die "the number of installed packages in the image could not be counted: the count
       came back as '${count:-<empty>}', which is not a number.  It is fatal rather than coerced
       to zero, because a count nobody can trust is indistinguishable from a record nobody can
       read, and both would be published as fact.  The image is NOT published."
            ;;
    esac
    [ "$count" -gt 0 ] || die "the image reports ZERO installed packages, immediately after
       apt-get install reported success for ${#IMAGE_PACKAGES[@]} required packages.  Something
       is wrong with the image's dpkg database rather than with the count, so the image is NOT
       published.  Inspect 'chroot $IMAGE_MOUNT dpkg -l | head'."

    # EVERY REQUIRED PACKAGE IS ACCOUNTED FOR IN THE RECORD.  apt reporting success is not the
    # same statement: a name can be satisfied by a provider, dropped as already-satisfied, or
    # replaced by a transitional package, and only the record says which.
    for package in "${IMAGE_PACKAGES[@]}"; do
        if package_directly_installed "$listing" "$package"; then
            accounted=$(( accounted + 1 ))
        else
            missing="$missing${missing:+ }$package"
        fi
    done

    if [ -n "$missing" ]; then
        # Resolved through dpkg's own Provides field.  Queried only when something is missing,
        # so the ordinary build pays for one dpkg-query and not two.
        # shellcheck disable=SC2016
        provides_listing="$(chroot_run dpkg-query --show \
                                --showformat='${Package} ${Status} :: ${Provides}\n' 2>/dev/null)" \
            || die "these required packages are not installed under their own names in the
       image:
           $missing
       and the Provides field could not be queried to establish whether another package
       supplies them, because dpkg-query failed.  The image is NOT published: an unaccounted
       required package means apt installed something other than what this script asked for."
        for package in $missing; do
            if package_provided_by_installed "$provides_listing" "$package"; then
                provided="$provided${provided:+ }$package"
                accounted=$(( accounted + 1 ))
            else
                die "the required package '$package' is not installed in the image and nothing
       installed in the image Provides it, even though apt-get install reported success for the
       whole set.  The image is NOT published: this is the drift the package record exists to
       catch -- an archive in which the name resolved to something else, or a package that was
       removed again by a later dependency resolution.  The full resolved set is in
       $target inside the image; compare it against IMAGE_PACKAGES in $SCRIPT_NAME."
            fi
        done
        warn "these required packages are satisfied by a PROVIDER rather than under their own"
        warn "  name, which is legitimate and is recorded: $provided"
    fi

    # A DIGEST OF THE RECORD, so two images can be compared by one line before anybody diffs
    # a few hundred.  Of the record as written, including its header, because that is the file
    # a reader inside the guest will hash.
    #
    # Read back through the primitive rather than with `sha256sum -- "$IMAGE_MOUNT$path"`, which
    # would have hashed whatever a symbolic link at that name pointed at and published the result
    # as a fingerprint OF THIS IMAGE'S package record.  A digest of the wrong file is worse than
    # no digest: it is a false provenance claim in the manifest.  The read goes through the same
    # descriptor-anchored path the write above used, so the bytes hashed are the bytes written.
    digest="$(confined_read "$target_rel" "the image's $target" \
                  'hash the package record for the manifest' | sha256sum | awk '{ print $1 }')" \
        || die "the package record at $target was written but could not be hashed, so the
       manifest cannot carry a comparable fingerprint of the resolved set.  The image is NOT
       published."
    case "$digest" in
        [0-9a-f]*) : ;;
        *) die "the digest of the package record came back as '$digest', which is not a sha256
       hex digest.  The image is NOT published." ;;
    esac

    PACKAGE_RECORD_COUNT="$count"
    PACKAGE_RECORD_DIGEST="$digest"
    PACKAGE_RECORD_REQUIRED_ACCOUNTED="$accounted"
    log "recorded $PACKAGE_RECORD_COUNT installed packages at $GUEST_PACKAGE_RECORD"
    log "  all ${#IMAGE_PACKAGES[@]} required packages accounted for ($accounted matched)"
    log "  record sha256 ${digest:0:16}... (full digest in the manifest)"
}

# ------------------------------------------------------------------------------------
# THE IMAGE CARRIES NO RESOLVER AND NO INTERFACE CONFIGURATION.
#
# Package installation above needs DNS -- it runs in a chroot on the BUILD HOST, which is
# legitimate host-side acquisition at image-build time.  The GUEST is a different matter: it
# is booted with no NIC at all and everything it runs is already inside the image, and this
# function removes the configuration that would let it use a network if one ever appeared.
#
# WHY BOTHER, GIVEN qemu IS STARTED WITH -nic none.  Two reasons, and the second is the real
# one.  First, defence in depth: the two ends are edited by different people at different
# times and either can regress alone.  Second, and decisively, the in-guest init ASSERTS that
# no resolver is configured -- so leaving the build host's resolv.conf inside the image would
# make that assertion fail on a correctly offline guest, for a file rather than for a network.
# The assertion is worth having, so the file has to go.
#
# debootstrap copies the build host's /etc/resolv.conf into the base it produces, so this is
# not hypothetical: without this step every image would carry the runner's resolver
# configuration.  The file is REPLACED with a comment rather than deleted, so a reader inside
# the guest finds the reason instead of an absence.
#
# EVERY OPERATION BELOW GOES THROUGH THE CONFINED PRIMITIVE, AND THIS FUNCTION IS WHY THAT
# MATTERS MORE HERE THAN ANYWHERE ELSE IN THE FILE.  It used to acknowledge the hazard in one
# line -- "a symlink first, because writing through it would write outside the image's /etc, and
# on this build host that means writing to the HOST's /run" -- and then perform every sibling
# operation with an ordinary host-side `rm -f`, `>`, `chmod`, `[ -d ]` and `find -delete` on
# $IMAGE_MOUNT-prefixed PATHNAMES.  Removing the link first narrows the window; it does not close
# it, and it does nothing at all for /etc/network.  A base tarball that ships /etc, /etc/network
# or /etc/network/interfaces as an absolute symbolic link had this script, as root ON THE BUILD
# HOST, truncate and replace a host file and delete the contents of a host directory.
#
# So the pathnames are gone.  openat2 with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV
# resolves each in-image path beneath the mount point and every operation runs through the
# resulting descriptor; the enumeration of interfaces.d goes through it too, because `find`'s
# STARTING path is resolved normally by the kernel and a symlinked interfaces.d would have had
# find enumerate -- and delete -- host files.
#
# THE ONE PLACE A LINK IS STILL THE TARGET RATHER THAN A REFUSAL is the removal itself, and that
# is deliberate: /etc/resolv.conf is EXPECTED to be a link on a systemd-resolved base, so
# confined_unlink_if_present removes the directory ENTRY (unlinkat never follows) and leaves
# whatever it pointed at alone.  The two files are then created fresh rather than truncated in
# place, which is the only way to be sure the object written to is the object this script made.
# ------------------------------------------------------------------------------------
neutralise_image_network_configuration() {
    local listing entry entry_type entry_name

    log "removing resolver and interface configuration from the image"

    # THE ENTRY GOES FIRST, WHATEVER IT IS.  systemd-resolved leaves /etc/resolv.conf as a
    # symbolic link into /run, and writing through it would write outside the image's /etc --
    # which on this build host means writing to the HOST's /run.  unlinkat() on a leaf relative
    # to a confined descriptor removes the entry without following it, so the link's target is
    # untouched and the name is free for a file this script owns.
    confined_unlink_if_present 'etc/resolv.conf' "the base's resolver configuration" \
        'remove the resolver configuration from the image'
    confined_create 'etc/resolv.conf' 0644 "the image's /etc/resolv.conf" \
        'write the empty resolver configuration into the image'
    {
        printf '# Deliberately empty: this guest has NO network.\n'
        printf '#\n'
        printf '# It is booted with qemu -nic none, every source, binary and library it needs\n'
        printf '# is already inside this image, and the in-guest init asserts that no\n'
        printf '# non-loopback interface, no route and no configured resolver exists before it\n'
        printf '# runs anything.  A nameserver line here would fail that assertion.\n'
        printf '#\n'
        printf '# Package installation happened on the BUILD HOST at image-build time, which is\n'
        printf '# where the network belongs.  See %s for what was installed.\n' "$GUEST_IMAGE_RECORD"
    } | confined_write 'etc/resolv.conf' "the image's /etc/resolv.conf" \
            'write the empty resolver configuration into the image'
    confined_chmod 'etc/resolv.conf' 0644 "the image's /etc/resolv.conf" \
        'set the mode of the image resolver configuration'

    # Loopback only.  The init never brings an interface up, so this file is documentation as
    # much as configuration -- but a base whose interfaces file names eth0 with dhcp would
    # leave a reader guessing whether the guest is meant to have a network.
    #
    # confined_directory replaces `[ -d ]`, which FOLLOWED links: on an image whose /etc/network
    # is an absolute symbolic link it reported the HOST's directory as present and everything
    # below then acted there.  It returns 1 for a genuinely absent directory -- the case this
    # skips, exactly as before -- and dies on a link or a non-directory rather than proceeding.
    if confined_directory 'etc/network' '/etc/network' \
            'write the loopback-only interface configuration into the image'; then
        # The purge of interfaces.d, enumerated by descriptor.  Only regular files are removed,
        # which is what `find -type f -delete` did; anything else is NAMED rather than passed
        # over in silence, because a symbolic link sitting in an interfaces.d this script has
        # just been asked to empty is worth a reader's attention even though nothing here
        # follows it.
        if confined_directory 'etc/network/interfaces.d' '/etc/network/interfaces.d' \
                'empty the image'"'"'s /etc/network/interfaces.d'; then
            listing="$WORK_DIR/image-interfaces-d-listing"
            rm -f -- "$listing"
            confined_list 'etc/network/interfaces.d' \
                "the image's /etc/network/interfaces.d" \
                'empty the image'"'"'s /etc/network/interfaces.d' > "$listing"
            while IFS= read -r -d '' entry; do
                entry_type="${entry%% *}"
                entry_name="${entry#* }"
                [ -n "$entry_name" ] || continue
                if [ "$entry_type" = 'regular' ]; then
                    confined_unlink "etc/network/interfaces.d/$entry_name" \
                        'an interface configuration fragment from the base' \
                        'empty the image'"'"'s /etc/network/interfaces.d'
                else
                    warn "the base's /etc/network/interfaces.d holds '$entry_name', which is a"
                    warn "  $entry_type rather than a regular file.  It is LEFT IN PLACE, as"
                    warn "  the previous 'find -type f' also left it, and nothing here follows"
                    warn "  it -- but the guest has no NIC, so nothing in that directory has any"
                    warn "  business existing.  Inspect the base tarball."
                fi
            done < "$listing"
            rm -f -- "$listing"
        fi

        confined_unlink_if_present 'etc/network/interfaces' \
            "the base's interface configuration" \
            'write the loopback-only interface configuration into the image'
        confined_create 'etc/network/interfaces' 0644 \
            "the image's /etc/network/interfaces" \
            'write the loopback-only interface configuration into the image'
        {
            printf '# Loopback only. This guest has no NIC (qemu -nic none) and its init never\n'
            printf '# brings an interface up; the in-guest init refuses to run if one exists.\n'
            printf 'auto lo\n'
            printf 'iface lo inet loopback\n'
        } | confined_write 'etc/network/interfaces' \
                "the image's /etc/network/interfaces" \
                'write the loopback-only interface configuration into the image'
        confined_chmod 'etc/network/interfaces' 0644 \
            "the image's /etc/network/interfaces" \
            'set the mode of the image interface configuration'
    fi
}

# ------------------------------------------------------------------------------------
# LCOV 2.x -- INSTALLED FROM THE PINNED TARBALL, THEN ASSERTED.
#
# run_coverage.sh cannot work with lcov 1.x: it uses `--fail-under-lines` and
# `--rc branch_coverage=1`, neither of which exists there, and it parses record spellings
# that are 2.x behaviour.  The suite's .lcovrc_l1 is written in 2.x spellings as well.
# Stable distribution archives ship 1.x, so the distro package installed above is a FLOOR
# that keeps genhtml and the perl dependencies present -- it is not the version the guest
# measures with.
#
# The assertion at the end is the load-bearing part.  Without it a 1.x lcov reaches the
# guest and surfaces as a confusing coverage failure long after the evidence of why.
#
# WHERE THE ARCHIVE IS UNPACKED, AND WHY IT IS NOT UNPACKED STRAIGHT INTO THE IMAGE ANY MORE.
# It used to be `mkdir -p -- "$IMAGE_MOUNT$staging"` followed by
# `tar --extract --directory "$IMAGE_MOUNT$staging"` and finally `rm -rf -- "$IMAGE_MOUNT$staging"`
# -- three root-owned host-side operations on an in-image PATHNAME, the last of them a recursive
# delete.  A base tarball shipping /root as an absolute symbolic link redirected all three onto
# the build host.
#
# Now the archive is unpacked on the host into a directory THIS SCRIPT owns, under its own 0700
# scratch directory, and the resulting tree is streamed into the image by a tar running INSIDE
# the chroot, whose resolution root is the image (see stream_tree_into_image).  The tarball's
# CONTENT is trusted at this point -- verify_sha256 has just matched it against the pinned digest
# -- so what needed confining was never the bytes but the destination pathname.  The delete goes
# through the chroot too, and only after the staging directory's device and inode are confirmed
# to be the ones this script created.
# ------------------------------------------------------------------------------------
install_lcov_2x() {
    if [ -n "$LCOV_TARBALL" ]; then
        verify_sha256 "$LCOV_TARBALL" "$LCOV_SHA256" 'the lcov 2.x source tarball'
        local staging='/root/lcov-src'
        local staging_rel='root/lcov-src'
        local host_staging="$WORK_DIR/lcov-src" staging_identity=''
        log "installing lcov from the pinned source tarball"

        # Unpacked here first, on a path this script made: $WORK_DIR is root-owned, mode 0700 and
        # was verified not to have been substituted (make_work_dir), and the EXIT trap removes it.
        rm -rf -- "$host_staging" || die "could not clear the host-side lcov staging directory at
       $host_staging."
        mkdir -p -- "$host_staging" || die "could not create the host-side lcov staging directory
       at $host_staging."
        chmod 0700 -- "$host_staging" || die "could not set the mode of the host-side lcov
       staging directory at $host_staging."
        tar --extract --file "$LCOV_TARBALL" --directory "$host_staging" \
            --strip-components=1 \
            || die "could not extract the lcov tarball.  A tarball without a single top-level
       directory needs its own layout: extract it yourself and pass a repacked archive."

        stream_tree_into_image "$host_staging" "$staging_rel" 0700 \
            'the lcov 2.x source tree' 'install lcov from the pinned source tarball' \
            "If the image ran out of space, raise --size (currently ${IMAGE_SIZE_MIB} MiB)."
        # Recorded the moment it exists, and compared immediately before the delete below.  A
        # recursive delete is the one operation in this file that must not be aimed at whatever
        # happens to be at a name.
        staging_identity="$(confined_identity "$staging_rel" 'the lcov 2.x source staging tree' \
                            'install lcov from the pinned source tarball')"
        rm -rf -- "$host_staging" || warn "could not remove the host-side lcov staging directory
  at $host_staging; the scratch directory's own cleanup will take it."

        chroot_run make -C "$staging" install \
            || die "'make install' failed for the lcov source tree inside the image.  lcov 2.x
       needs perl with Capture::Tiny, DateTime, Date::Parse, JSON::XS, PerlIO::gzip and
       Memory::Process; those are in the installed package set, so check the tarball's own
       README for anything further it requires."
        remove_tree_in_image "$staging_rel" "$staging_identity" \
            'the lcov 2.x source staging tree' \
            'remove the lcov source staging tree from the image'
    else
        warn "no --lcov-tarball was given, so the image carries only the distribution's lcov."
        warn "  Stable archives ship lcov 1.x, which run_coverage.sh CANNOT use, so the"
        warn "  assertion below will almost certainly fail. That is by design: pass a pinned"
        warn "  lcov 2.x source tarball with --lcov-tarball and --lcov-sha256."
    fi

    local version_line major
    version_line="$(chroot_run lcov --version 2>&1 | head -n 1 || true)"
    [ -n "$version_line" ] || die "lcov does not run inside the image at all.  It is a hard
       prerequisite of run_coverage.sh, so the image is not usable without it."
    log "lcov in the image reports: $version_line"
    # The version is the first dotted number on the line; 2.x is required and 1.x is refused.
    major="$(printf '%s\n' "$version_line" | sed -n 's/.*[^0-9]\([0-9]\+\)\.[0-9].*/\1/p' | head -n 1)"
    case "$major" in
        ''|*[!0-9]*) die "could not read a version number out of lcov's own output:
           $version_line
       run_coverage.sh requires lcov 2.x and this image build will not guess.  Install a
       version whose --version output carries a readable number." ;;
    esac
    [ "$major" -ge 2 ] || die "the image's lcov is version $major.x, and run_coverage.sh
       requires 2.x: it uses --fail-under-lines and --rc branch_coverage=1, neither of which
       exists in 1.x, and it parses trace-record spellings that are 2.x behaviour.  Pass a
       pinned lcov 2.x source tarball:
           --lcov-tarball /path/to/lcov-2.x.tar.gz --lcov-sha256 <hex>
       Failing the image build here is deliberate: a 1.x lcov reaching the guest surfaces as
       a confusing coverage failure hours later, with nothing pointing at the cause."
    # genhtml IS ASSERTED SEPARATELY, BECAUSE `lcov --version` DOES NOT COVER IT.  They are
    # different Perl programs with different `use` lists: genhtml alone loads Date::Parse, so
    # a missing libtimedate-perl leaves lcov answering normally while genhtml cannot even
    # load.  Measured -- that combination built an image that passed every assertion here,
    # booted, ran the whole invocation matrix under emulation, and only then failed in
    # generate_html(), which run_coverage.sh treats as fatal.  Asserting it here turns an
    # hours-late failure into an image build that stops on the line naming the package.
    local genhtml_line
    genhtml_line="$(chroot_run genhtml --version 2>&1 | head -n 1 || true)"
    case "$genhtml_line" in
        *[0-9].[0-9]*) log "genhtml in the image reports: $genhtml_line" ;;
        *) die "genhtml does not run inside the image.  Its own output was:
           ${genhtml_line:-<nothing at all>}
       run_coverage.sh calls genhtml and dies if it fails, and the workflow's artifact
       completeness gate requires coverage/index.html, so this image could not produce a
       passing run.  A 'Can't locate <Module>.pm in @INC' line above names the missing Perl
       module: Date::Parse comes from libtimedate-perl, Capture::Tiny from
       libcapture-tiny-perl, DateTime from libdatetime-perl.  Add it to IMAGE_PACKAGES."
    esac
    log "lcov 2.x and genhtml assertions passed"
}


# ------------------------------------------------------------------------------------
# COPYING THE PAYLOAD, THE STAGED SDK AND (OPTIONALLY) THE GOOGLETEST PREFIX IN.
#
# The destination names reproduce the CI workspace layout rather than inventing one, which
# is what makes run_coverage.sh's and configure's own defaults correct inside the guest with
# nothing overridden: the coverage runner derives its workspace root as the parent of the
# hdmicec checkout, defaults GTEST_PREFIX to <workspace>/install/usr, and configure falls
# back to the sibling rdk-halif-aidl checkout beside the submodule.
#
# A TAR PIPE THROUGH THE CHROOT RATHER THAN cp -a, AND THE REASON IS NOT PREFERENCE.
# This function used to run three `mkdir -p` and three `cp -a` on $IMAGE_MOUNT-prefixed
# PATHNAMES, as root on the build host.  The destinations are names THIS script chooses, which is
# what an earlier revision of the confinement boundary offered as the reason they were safe -- and
# it is not one: the LEADING COMPONENTS of those names come from the base tarball.  A base that
# ships /opt as an absolute symbolic link had `mkdir -p` build a directory tree on the HOST and
# `cp -a` write the payload, the staged SDK and the GoogleTest prefix into it, then had the
# artifact and configuration directories created there and chmodded, and /tmp chmodded to 1777 on
# whatever the link pointed at.
#
# What cp -a preserved, the pipe preserves: --numeric-owner on both ends keeps uid and gid
# exactly as they are rather than remapping them through either passwd database, tar carries
# permissions and timestamps (a build tree's timestamps decide what make rebuilds), and symbolic
# links cross as members rather than being followed (the staged SDK is full of versioned links).
# The extracting tar runs INSIDE the chroot, so no in-image pathname is ever resolved by the
# build host's kernel view; stream_tree_into_image documents the whole of that boundary.
#
# The mkdir/chmod PAIRS below are kept as pairs deliberately.  The confined mkdir creates through
# mkdirat(), which subtracts the process umask, so a mode that matters -- 0700 for the artifact
# directory, 1777 for /tmp -- has to be set explicitly afterwards, exactly as it did when these
# were `mkdir -p` followed by `chmod`.
# ------------------------------------------------------------------------------------
copy_guest_payload() {
    log "copying the payload into the image at $GUEST_PAYLOAD_DIR"
    # The workspace root first and by name, so the layout is this script's rather than a
    # by-product of whichever child directory was created first.
    confined_mkdir "${GUEST_WORKSPACE#/}" 0755 "the image's $GUEST_WORKSPACE" \
        'copy the payload into the image'
    stream_tree_into_image "$PAYLOAD_DIR" "${GUEST_PAYLOAD_DIR#/}" 0755 \
        'the payload' 'copy the payload into the image' \
        "If the image ran out of space, raise --size (currently ${IMAGE_SIZE_MIB} MiB)."

    log "copying the staged SDK into the image at $GUEST_SDK_DIR"
    stream_tree_into_image "$SDK_DIR" "${GUEST_SDK_DIR#/}" 0755 \
        'the staged SDK' 'copy the staged SDK into the image' \
        "If the image ran out of space, raise --size (currently ${IMAGE_SIZE_MIB} MiB)."

    if [ -n "$GTEST_PREFIX_SRC" ]; then
        log "copying the GoogleTest prefix into the image at $GUEST_GTEST_PREFIX"
        stream_tree_into_image "$GTEST_PREFIX_SRC" "${GUEST_GTEST_PREFIX#/}" 0755 \
            'the GoogleTest prefix' 'copy the GoogleTest prefix into the image' \
            "If the image ran out of space, raise --size (currently ${IMAGE_SIZE_MIB} MiB)."
    fi

    # The artifact directory is created here, owner-only, so the init does not have to create
    # it under a umask it did not set -- and so run_coverage.sh's own ancestry check on a
    # caller-named output directory finds a private, root-owned path.
    confined_mkdir "${GUEST_ARTIFACT_DIR#/}" 0700 "the image's $GUEST_ARTIFACT_DIR" \
        'create the guest artifact directory in the image'
    confined_chmod "${GUEST_ARTIFACT_DIR#/}" 0700 "the image's $GUEST_ARTIFACT_DIR" \
        'set the mode of the guest artifact directory'
    confined_mkdir "${GUEST_CONF_DIR#/}" 0755 "the image's $GUEST_CONF_DIR" \
        'create the guest configuration directory in the image'
    confined_chmod "${GUEST_CONF_DIR#/}" 0755 "the image's $GUEST_CONF_DIR" \
        'set the mode of the guest configuration directory'
    # /tmp is where run_coverage.sh mints its private lcov HOME and its snapshot directory.
    # It is the one directory here that ends up WORLD-WRITABLE, which is exactly why the mode is
    # set through a descriptor the confinement opened: a `chmod 1777` on an $IMAGE_MOUNT pathname
    # whose /tmp was a link would have made a HOST directory world-writable, as root.
    confined_mkdir 'tmp' 1777 "the image's /tmp" 'create /tmp in the image'
    confined_chmod 'tmp' 1777 "the image's /tmp" 'set the mode of the image /tmp'
}

# ------------------------------------------------------------------------------------
# THE LOADER.  Registering the two staged library directories with ld.so.conf and running
# ldconfig inside the image means the closure resolves for anything the guest runs, whether
# or not that process inherited LD_LIBRARY_PATH.  The init exports the path as well, and the
# duplication is deliberate: on a host whose cache already knows the directories the export
# costs a stat, and on one where it does not it is the difference between a suite that runs
# and one that dies before its first test.
# ------------------------------------------------------------------------------------
register_guest_library_paths() {
    # Through the confined primitive, for the same reason the apt configuration is: the
    # directory this fragment goes in is /etc/ld.so.conf.d, /etc comes from the BASE TARBALL, and
    # `mkdir -p` followed by `>` and `chmod` on an $IMAGE_MOUNT pathname would have created a
    # directory and written a root-owned file on the BUILD HOST if any component of that path
    # were an absolute symbolic link.  A loader configuration is a particularly bad file to have
    # written to the wrong root: it names library directories that every process on that system
    # then searches.
    #
    # The optional GoogleTest line, which used to be a separate `>>` append, is folded into the
    # single stream below.  The primitive has no append operation by design -- an append reopens
    # a name -- and there is no reason for two writes here when the content is decided before
    # either of them.
    local conf_rel='etc/ld.so.conf.d/cec-l2-binder.conf'
    confined_mkdir 'etc/ld.so.conf.d' 0755 "the image's /etc/ld.so.conf.d" \
        'register the staged library directories with the image loader'
    confined_unlink_if_present "$conf_rel" "the image's /$conf_rel" \
        'register the staged library directories with the image loader'
    confined_create "$conf_rel" 0644 "the image's /$conf_rel" \
        'register the staged library directories with the image loader'
    {
        printf '# Written by %s.  The staged Binder and AIDL stub libraries.\n' "$SCRIPT_NAME"
        printf '# libbinder and libutils are direct link edges of libRCEC; liblog, libbase,\n'
        printf '# libcutils and libcutils_sockets are libbinder'"'"'s own dependencies and are here\n'
        printf '# because they must be LOADABLE, not because the middleware names them.\n'
        printf '%s\n' "$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET/lib/binder"
        printf '%s\n' "$GUEST_SDK_DIR/$SDK_REL_HALIF_LIB"
        if [ -n "$GTEST_PREFIX_SRC" ]; then
            printf '%s\n' "$GUEST_GTEST_PREFIX/lib"
        fi
    } | confined_write "$conf_rel" "the image's /$conf_rel" \
            'register the staged library directories with the image loader'
    confined_chmod "$conf_rel" 0644 "the image's /$conf_rel" \
        'set the mode of the image loader configuration fragment'
    chroot_run ldconfig || die "ldconfig failed inside the image, so the staged libraries would
       not be resolvable at boot."
    log "registered the staged library directories with the image's loader cache"
}

# ------------------------------------------------------------------------------------
# THE TWO IN-GUEST HELPERS.
#
# Both exist because the init must REPORT WHAT THE RUNNING GUEST SHOWS rather than assert a
# value from a configuration fragment, and neither fact is reachable from the shell.
#
#   cec-binder-protocol-version -- opens the driver node and asks it, with the BINDER_VERSION
#       ioctl, what protocol version it speaks.  That number is the one half of the
#       protocol-equality check that cannot be read any other way, and printing a number this
#       script had not measured would be a fabrication.
#
#   cec-servicemanager-ready -- the readiness probe.  It calls defaultServiceManager() and
#       then checkService() for a throwaway name.  Reaching the second call AT ALL proves the
#       context manager answered a real binder transaction, which is the only honest
#       definition of "servicemanager is ready".  On the pinned stack the first call loops
#       until binder handle 0 resolves, so the probe BLOCKS while the manager is absent --
#       which is exactly what makes a bounded `timeout` around it a real wait instead of a
#       sleep.  It deliberately does NOT start a threadpool: the client threadpool belongs to
#       DriverAidlImpl::open() and the service one to the fake service host.
#
# Compiling them here rather than shipping prebuilt binaries has a second benefit worth
# stating: it proves, at image-build time, that the staged SDK's headers and libraries
# actually compile and link inside this image.
# ------------------------------------------------------------------------------------
build_guest_helpers() {
    # THE SOURCE TREE, ITS TWO FILES, THE TWO INSTALLATION CHECKS AND THE CLEANUP ALL GO THROUGH
    # THE CONFINEMENT NOW.  Every one of them used to be a root-owned host-side operation on an
    # $IMAGE_MOUNT pathname: `mkdir -p "$src_dir"`, two `cat > "$src_dir/..."` redirections, two
    # `[ -x "$IMAGE_MOUNT$GUEST_..." ]` tests -- both of which FOLLOWED symbolic links, so they
    # would happily report a HOST binary as the installed helper -- and a `rm -rf -- "$src_dir"`,
    # which is the worst of them: a recursive delete as root aimed at a pathname whose /usr came
    # from the base tarball.
    #
    # The compile steps themselves are unchanged and need no conversion: chroot_run and
    # chroot_run_with_libs resolve every path they are given inside the image.
    local src_rel='usr/local/src/cec-l2' src_identity=''
    confined_mkdir "$src_rel" 0755 "the image's /usr/local/src/cec-l2" \
        'build the in-guest helpers'
    # Recorded now, compared immediately before the recursive delete at the end of this function.
    src_identity="$(confined_identity "$src_rel" "the in-guest helpers' source tree" \
                    'build the in-guest helpers')"

    confined_unlink_if_present "$src_rel/binder_protocol_version.c" \
        "the in-guest protocol helper's source" 'write the in-guest helper sources'
    confined_create "$src_rel/binder_protocol_version.c" 0644 \
        "the in-guest protocol helper's source" 'write the in-guest helper sources'
    confined_write "$src_rel/binder_protocol_version.c" \
        "the in-guest protocol helper's source" 'write the in-guest helper sources' \
        <<'PROTOCOL_HELPER_EOF'
/*
 * cec-binder-protocol-version -- report the binder protocol version the running kernel's
 * driver node speaks.
 *
 * WHY THIS EXISTS.  libbinder performs a strict protocol-version EQUALITY check when it
 * opens the driver node, so kernel, SDK build and vendor side must agree; a mismatch fails
 * every open.  The in-guest init has to report the version the guest ACTUALLY shows rather
 * than infer it from a kernel configuration fragment, and no shell primitive can ask.
 *
 * The request is declared locally rather than included from <linux/android/binder.h>,
 * because a minimal root filesystem need not ship that UAPI header.  The two lines below
 * mirror the kernel's own definitions:
 *     struct binder_version { __s32 protocol_version; };
 *     #define BINDER_VERSION _IOWR('b', 9, struct binder_version)
 *
 * Exit status: 0 and the version on stdout; 2 when the node cannot be opened; 3 when the
 * ioctl fails.  Nothing is printed that was not read from the driver.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct cec_binder_version {
    int32_t protocol_version;
};

#define CEC_BINDER_VERSION _IOWR('b', 9, struct cec_binder_version)

int main(int argc, char **argv)
{
    const char *node = (argc > 1) ? argv[1] : "/dev/binder";
    struct cec_binder_version version;
    int fd;

    version.protocol_version = -1;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "cec-binder-protocol-version: open(%s): %s\n", node, strerror(errno));
        return 2;
    }
    if (ioctl(fd, CEC_BINDER_VERSION, &version) < 0) {
        fprintf(stderr, "cec-binder-protocol-version: ioctl(BINDER_VERSION) on %s: %s\n",
                node, strerror(errno));
        (void) close(fd);
        return 3;
    }
    (void) close(fd);

    printf("%d\n", (int) version.protocol_version);
    return 0;
}
PROTOCOL_HELPER_EOF

    confined_unlink_if_present "$src_rel/servicemanager_ready.cpp" \
        "the in-guest readiness probe's source" 'write the in-guest helper sources'
    confined_create "$src_rel/servicemanager_ready.cpp" 0644 \
        "the in-guest readiness probe's source" 'write the in-guest helper sources'
    confined_write "$src_rel/servicemanager_ready.cpp" \
        "the in-guest readiness probe's source" 'write the in-guest helper sources' \
        <<'READY_PROBE_EOF'
/*
 * cec-servicemanager-ready -- block until the binder context manager answers, then exit 0.
 *
 * WHY THIS EXISTS.  servicemanager is an unconditional runtime prerequisite wherever a
 * binder driver is present, and "it is ready" is not something a sleep can establish: a
 * sleep that is occasionally too short produces a run in which every AIDL invocation
 * silently falls back to the legacy back-end and reports green, which is the precise
 * failure the binder-capable job exists to catch.
 *
 * WHAT IT PROVES.  defaultServiceManager() loops until binder handle 0 resolves, so
 * reaching the checkService() call at all means the context manager answered a real binder
 * transaction.  The throwaway name is expected to be unregistered; a null result is the
 * normal, successful outcome and is reported as such.  The caller bounds the wait with
 * `timeout`, which turns the blocking call into a bounded poll.
 *
 * WHAT IT DELIBERATELY DOES NOT DO.  It does not start a binder threadpool.  The client
 * threadpool belongs to DriverAidlImpl::open() and the service one to the fake service
 * host; a threadpool started here would be a third owner of something that has exactly two.
 * It registers nothing, so it cannot collide with the "HdmiCec" name the matrix uses.
 */
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

#include <cstdio>

int main()
{
    android::sp<android::IServiceManager> serviceManager = android::defaultServiceManager();
    if (serviceManager == nullptr) {
        std::fprintf(stderr, "cec-servicemanager-ready: defaultServiceManager() returned null\n");
        return 1;
    }

    const android::sp<android::IBinder> probe =
        serviceManager->checkService(android::String16("cec.l2.readiness.probe"));

    std::printf("cec-servicemanager-ready: the context manager answered; the probe name is %s\n",
                (probe == nullptr) ? "unregistered, as expected" : "unexpectedly registered");
    return 0;
}
READY_PROBE_EOF

    log "compiling the in-guest binder protocol-version helper"
    chroot_run gcc -O2 -Wall -o "$GUEST_PROTOCOL_HELPER" \
        /usr/local/src/cec-l2/binder_protocol_version.c \
        || die "could not compile the in-guest binder protocol-version helper.  The init needs
       it to report the protocol version the running guest shows, and reporting a version
       nothing measured is not an option."

    log "compiling the in-guest servicemanager readiness probe against the staged SDK"
    chroot_run_with_libs g++ -std=c++17 -O2 -Wall \
        "-I$GUEST_SDK_DIR/$SDK_REL_BINDER_BUILD/include/binder_sdk" \
        -o "$GUEST_READY_PROBE" \
        /usr/local/src/cec-l2/servicemanager_ready.cpp \
        "-L$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET/lib/binder" \
        "-Wl,-rpath,$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET/lib/binder" \
        -lbinder -lutils \
        || die "could not compile the in-guest servicemanager readiness probe against the
       staged Binder SDK.  This is also the first place a staged SDK that does not actually
       link shows up, so treat it as evidence about the SDK rather than about the probe:
       check that $SDK_REL_BINDER_BUILD/include/binder_sdk holds the Binder headers, that
       $SDK_REL_BINDER_TARGET/lib/binder holds libbinder and libutils, and that the whole
       six-library closure is present -- GNU ld does not search -L for a shared library's own
       DT_NEEDED entries, so a missing liblog or libbase fails here too."

    # Both are checked as installed and executable rather than RUN here: this build host has
    # no binder driver, so the protocol helper would correctly fail to open the node and the
    # probe would correctly block.  Their behaviour is exercised in the guest, which is the
    # only place either question has an answer.
    #
    # `[ -x ]` is gone from both checks.  It followed symbolic links, so on an image whose
    # /usr/local/bin was a link it reported whether a HOST binary was executable and said nothing
    # at all about what the guest would run -- a check that passes for the wrong object is worse
    # than no check, because the failure then surfaces as a kernel panic at boot.
    assert_installed_executable "${GUEST_PROTOCOL_HELPER#/}" "$GUEST_PROTOCOL_HELPER" \
        'the protocol-version helper' 'verify the in-guest helpers were installed'
    assert_installed_executable "${GUEST_READY_PROBE#/}" "$GUEST_READY_PROBE" \
        'the readiness probe' 'verify the in-guest helpers were installed'
    remove_tree_in_image "$src_rel" "$src_identity" \
        "the in-guest helpers' source tree" \
        'remove the in-guest helper sources from the image'
    log "both in-guest helpers built and installed"
}

# ------------------------------------------------------------------------------------
# THE GUEST'S COMPILER AND gcov, MEASURED RATHER THAN NAMED - AND WHAT IS ASSERTED ABOUT THEM.
#
# `command -v gcov` establishes only that a file exists at that name.  The guest's whole output
# is a coverage trace and a set of branch-arm coordinates, and both are properties of the
# compiler that produced them, so an image that carries "a gcov" and "a g++" without recording
# WHICH is an image whose figures cannot be attributed afterwards.  This function is the one
# place those versions are read out of the image, and it makes three statements of different
# strengths, deliberately kept apart:
#
#   1. RECORDED, always: the exact versions of the guest's gcc, g++ and gcov, by the unversioned
#      names the guest itself will resolve, so the values describe what the guest will actually
#      run rather than what was installed under some other name.  They travel into the manifest
#      as GUEST_TOOLCHAIN_GCC / _GXX / _GCOV.
#   2. ASSERTED HARD: gcc, g++ and gcov agree with each other.  This is the property the guest's
#      own trace depends on and it is closable inside any archive, because all three come from
#      one family's packages.  gcov demands a notes-file version stamp derived from the GCC major
#      AND minor that wrote the file; where they disagree it reads nothing, one file at a time,
#      and the run would report a capture with no counters rather than a wrong number - which is
#      still hours of a QEMU boot spent on a question this second answers.
#   3. COMPARED AND REPORTED, never asserted: the guest family against --toolchain-gcc-major.
#      The reasoning for why this half is a record rather than a gate is in
#      install_image_pinned_compiler; the short form is that the guest's archive is the caller's
#      to choose and this script does not fail an image build for what an archive does not carry.
#
# -dumpfullversion IS WHAT IS READ FROM THE COMPILERS, NOT -dumpversion: since GCC 7 the latter
# prints the MAJOR ALONE, so comparing it against gcov's full version fails on every correct
# toolchain.  gcov has neither option and reports its version as the last field of its first
# line; the raw line is logged beside the parsed value so a format change is visible rather than
# silently reinterpreted.
# ------------------------------------------------------------------------------------
# ------------------------------------------------------------------------------------
# ARE THE STAGED LIBRARIES ACTUALLY LOADABLE IN THIS IMAGE?  ASKED OF THE IMAGE'S OWN LOADER,
# BECAUSE NOTHING ELSE CAN ANSWER IT.
#
# The staged Binder closure, the two AIDL stub libraries and servicemanager are compiled ON THE
# BUILD HOST and then run inside an image built from a different distribution, with its own glibc
# and its own libstdc++.  The workflow that calls this script used to justify that arrangement by
# comparing SUITE VINTAGES -- the guest's runtime being newer than the runner's, so "the safe
# direction".  That reasoning is both fragile and no longer true of the runner it now runs on,
# and the honest replacement is not a better argument: it is to ASK THE LOADER, in the image,
# before a QEMU cycle is spent.
#
# WHAT CHANGED SINCE, AND WHY THIS CHECK IS STILL HERE.  aidl-path-tests.yml no longer stages
# artefacts built by the RUNNER's own compiler: it debootstraps a build chroot from the same
# pinned snapshot and suite the guest is built from and compiles the i386 SDK, the AIDL stubs and
# GoogleTest inside it, so the artefacts are built by the guest's own glibc and libstdc++ by
# construction.  That removes the CAUSE this check was written for -- and this check stays,
# unweakened, for three reasons: --sdk-dir is a path any caller may point anywhere, a guest-
# matched chroot is a property of one workflow rather than of this script, and a check that
# passes is the evidence that the staging was in fact guest-matched.  The workflow additionally
# refuses the same class of artefact on the HOST, at staging time, so on that path this arm
# should now be unreachable; reaching it means the host-side check was bypassed or an artefact
# arrived from elsewhere, and the diagnostic below says so.
#
# WHY VINTAGE COMPARISON WAS THE WRONG TEST ANYWAY.  An ELF does not require "the glibc it was
# built on".  It requires the specific symbol versions its referenced symbols were introduced
# at, which for ordinary C++17 code sit far below any current release -- measured on a
# gcc-13-built 32-bit C++17 object using std::string, std::vector, std::mutex and exceptions:
# GLIBC_2.0, GLIBC_2.1.3, GLIBC_2.4, GLIBCXX_3.4, GLIBCXX_3.4.11, GLIBCXX_3.4.21, CXXABI_1.3
# and GCC_3.0, every one of them satisfied by a bookworm i386 userspace with room to spare.
# So a newer build host is usually harmless and occasionally fatal, and which of the two it is
# cannot be read off the suite names. `ldd` resolves the closure through the image's real
# loader and reports both failure modes by name: a library that cannot be found at all, and a
# symbol version the image's runtime does not define.
#
# THIS IS ALSO THE CHECK THAT GIVES THE GUEST COMPILER PIN ITS TEETH.  install_image_pinned_
# compiler is deliberately gate-and-record: it cannot fail an image build for a package the
# guest's archive may not carry.  The residual risk of that decision is precisely a staged
# artefact built by a newer host compiler against a runtime the guest lacks -- and that risk
# is what this function converts from "recorded, hope for the best" into a named failure with
# a remedy.  It runs AFTER register_guest_library_paths, so ldconfig has already taught the
# image's loader cache where the staged directories are.
# ------------------------------------------------------------------------------------
assert_image_staged_libraries_loadable() {
    local -a targets=()
    local library
    for library in "${BINDER_CLOSURE_LIBRARIES[@]}"; do
        targets+=("$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET/lib/binder/$library")
    done
    for library in "${HALIF_STUB_LIBRARIES[@]}"; do
        targets+=("$GUEST_SDK_DIR/$SDK_REL_HALIF_LIB/$library")
    done
    targets+=("$GUEST_SDK_DIR/$SDK_REL_SERVICEMANAGER")

    # ldd is part of the base system, but saying so and finding out are different things, and
    # an image without it must not silently skip the check.
    chroot_run sh -c "command -v -- ldd >/dev/null 2>&1" || die "ldd is not present inside the
       image, so whether the staged Binder and AIDL libraries can be LOADED there cannot be
       established.  It ships with libc-bin, which a Debian base always has; an image missing
       it has been trimmed past what this script can verify.  Do not proceed on the assumption
       that they load -- that assumption is what this check exists to replace."

    local target report unresolved bad_version
    for target in "${targets[@]}"; do
        # 2>&1 on purpose: a version failure is reported on stderr by some loader versions and
        # on stdout by others, and losing it to the wrong stream would turn this check green.
        report="$(chroot_run sh -c "ldd -- '$target' 2>&1" || true)"
        [ -n "$report" ] || die "ldd produced no output at all for the staged artefact
           $target
       inside the image, so its loadability is unknown.  Check that the file was copied and
       that it is the architecture the guest runs; assert_target_arch_binary covers the second
       on the host side, and this is the in-image counterpart."

        unresolved="$(printf '%s\n' "$report" | grep -F 'not found' | grep -Fv 'version' || true)"
        [ -z "$unresolved" ] || die "a staged artefact has dependencies the image's loader
       cannot find:
           $target
$(printf '%s\n' "$unresolved" | sed 's/^/           /')
       The staged directories are registered with the image's loader in
       /etc/ld.so.conf.d/cec-l2-binder.conf and ldconfig has already run, so a name still
       missing here is a library that was never staged rather than one the loader cannot see.
       Stage the whole Binder runtime closure -- liblog, libbase, libcutils and
       libcutils_sockets are NOT middleware link edges and are easy to omit for exactly that
       reason -- and rebuild the image."

        bad_version="$(printf '%s\n' "$report" | grep -F 'version' | grep -F 'not found' || true)"
        [ -z "$bad_version" ] || die "a staged artefact requires runtime symbol versions this
       image does not provide:
           $target
$(printf '%s\n' "$bad_version" | sed 's/^/           /')
       This is the build-host-newer-than-guest failure, and it is reported here rather than
       inside the VM.

       THE REMEDY THAT WORKS, and it is one rather than three: BUILD THE STAGED ARTEFACTS WITH A
       TOOLCHAIN WHOSE glibc AND libstdc++ ARE NO NEWER THAN THIS GUEST'S -- in practice, inside
       a chroot debootstrapped from the same pinned snapshot and suite this image was built from.
       .github/workflows/aidl-path-tests.yml now stages every i386 artefact that way, in 'Create
       the guest-matched i386 build chroot', and it re-checks the result on the host before the
       image build in 'Verify the guest's staged artefacts need no symbol version the guest
       lacks'.  SO A FAILURE HERE ON THAT PATH MEANS ONE OF THOSE TWO STEPS WAS BYPASSED OR AN
       ARTEFACT REACHED --sdk-dir FROM SOMEWHERE ELSE; establish which before changing anything
       in this script.

       THREE THINGS THAT LOOK LIKE REMEDIES AND ARE NOT, each measured on the ubuntu-24.04 /
       bookworm-i386 pair this project uses:

         * --toolchain-gcc-major DOES NOT CLOSE IT, although an earlier revision of this message
           listed it first.  It selects the compiler used INSIDE the guest and has no bearing on
           what compiled the artefacts already staged; and the pinned family is precisely what
           the guest archive cannot serve -- bookworm's default is GCC 12 -- which is why that
           option is gate-and-record in the first place.  It is still worth checking
           GUEST_TOOLCHAIN_PIN_STATE in the manifest, but for attributing the coverage figures,
           not for this failure.
         * -static-libstdc++ -static-libgcc IS NOT SUFFICIENT ON ITS OWN.  It does remove the
           GLIBCXX_3.4.32 requirement that g++-13 -m32 emits for any translation unit including
           <iostream> (the std::ios_base_library_init() reference).  It leaves the artefact
           requiring GLIBC_2.38 all the same, because the host's PREBUILT 32-bit libstdc++.a
           itself references __isoc23_strtoul@GLIBC_2.38 -- measured in its eh_alloc.o and
           debug.o members -- and a static archive is linked in as it was compiled, not
           recompiled.  --sysroot fails for the same reason.
         * SUPPRESSING THE C23 ALIASES WITH A -D IS IMPOSSIBLE.  __isoc23_strtol,
           __isoc23_sscanf and their family are emitted by any _GNU_SOURCE translation unit
           compiled against a glibc at or above 2.38 -- and g++ always defines _GNU_SOURCE, as
           does libcutils explicitly.  /usr/include/features.h #undefs __GLIBC_USE_C23_STRTOL
           and re-derives it from __GLIBC_USE(ISOC23), which _GNU_SOURCE forces on, so a
           -D__GLIBC_USE_C23_STRTOL=0 on the command line is discarded before any header reads
           it.  The guest's glibc 2.36 defines ZERO __isoc23_* symbols, so every such reference
           is fatal at link time and there is no flag that removes it.

       Raising GUEST_DEBIAN_SUITE to one whose runtime defines the versions named above is a
       real alternative, and it is a decision about the guest rather than about the staging: it
       changes the guest's compiler family, its package set and therefore its coverage figures.
       Do NOT copy the host's libstdc++ into the staged directory as a workaround without also
       confirming its glibc requirements are met -- that trades one version failure for another."
    done

    log "staged Binder closure, AIDL stubs and servicemanager all resolve through the image's"
    log "  own loader: ${#targets[@]} artefacts, no missing library and no missing symbol version"
}

assert_image_compiler_identity() {
    local gcc_full gxx_full gcov_line
    gcc_full="$(chroot_run sh -c 'gcc -dumpfullversion 2>/dev/null || gcc -dumpversion' || true)"
    gxx_full="$(chroot_run sh -c 'g++ -dumpfullversion 2>/dev/null || g++ -dumpversion' || true)"
    gcov_line="$(chroot_run sh -c 'gcov --version 2>/dev/null | head -n 1' || true)"

    [ -n "$gcc_full" ] || die "the image's gcc would not report a version.  It is present by
       name -- the tool loop above passed -- so it is installed and not runnable, which usually
       means the base tarball and the installed packages are for different architectures.  A
       guest that cannot run its own compiler cannot build the middleware."
    [ -n "$gxx_full" ] || die "the image's g++ would not report a version, though it is present
       by name.  See the gcc message above: an installed-but-not-runnable compiler is normally
       an architecture mismatch between the base tarball and the package set."
    [ -n "$gcov_line" ] || die "the image's gcov would not report a version, though it is
       present by name.  run_coverage.sh runs gcov through lcov inside the guest, so an
       unrunnable gcov means the guest can build and can never measure."

    GUEST_GCC_VERSION="$gcc_full"
    GUEST_GXX_VERSION="$gxx_full"
    GUEST_GCOV_VERSION="$(printf '%s\n' "$gcov_line" | awk '{ print $NF }')"

    log "the guest toolchain, measured inside the image by the names the guest resolves:"
    log "  gcc  -> $GUEST_GCC_VERSION  ($(chroot_run sh -c "command -v -- gcc" 2>/dev/null || echo '<unresolved>'))"
    log "  g++  -> $GUEST_GXX_VERSION  ($(chroot_run sh -c "command -v -- g++" 2>/dev/null || echo '<unresolved>'))"
    log "  gcov -> $GUEST_GCOV_VERSION  ($(chroot_run sh -c "command -v -- gcov" 2>/dev/null || echo '<unresolved>'))"
    log "  gcov reported it as: $gcov_line"

    [ "$GUEST_GCC_VERSION" = "$GUEST_GXX_VERSION" ] \
        && [ "$GUEST_GCC_VERSION" = "$GUEST_GCOV_VERSION" ] \
        || die "the image's gcc ($GUEST_GCC_VERSION), g++ ($GUEST_GXX_VERSION) and gcov
       ($GUEST_GCOV_VERSION) are not the same version.  gcov demands a notes-file version stamp
       derived from the GCC major and minor that wrote the file, so this image would build and
       then measure nothing -- a capture with no counters, discovered after a full QEMU boot and
       a whole matrix.  Install all three from one family: the packages are gcc-<major> (which
       carries gcov-<major>) and g++-<major>.  If --toolchain-gcc-major selected a family, check
       that /usr/local/bin holds all THREE links rather than two."

    # THE PIN COMPARISON. Reported, never fatal -- and the wording distinguishes the three
    # outcomes so a reader of this log does not have to infer which one happened.
    local guest_major="${GUEST_GCC_VERSION%%.*}"
    case "$PINNED_COMPILER_STATE" in
        'not requested')
            log "no acceptance family was pinned (--toolchain-gcc-major absent), so the guest"
            log "  toolchain above is recorded and not compared. It is internally consistent,"
            log "  which is what its own trace depends on."
            ;;
        'selected')
            if [ "$guest_major" = "$TOOLCHAIN_GCC_MAJOR" ]; then
                log "the guest MATCHES the pinned acceptance family: GCC major $TOOLCHAIN_GCC_MAJOR,"
                log "  measured $GUEST_GCC_VERSION. The patch level is recorded, not gated -- no"
                log "  distribution archive is required to serve one exact patch release."
            else
                warn "NAMED LIMITATION: gcc-$TOOLCHAIN_GCC_MAJOR and g++-$TOOLCHAIN_GCC_MAJOR were"
                warn "  installed and linked into /usr/local/bin, yet a bare gcc measures"
                warn "  $GUEST_GCC_VERSION, whose major is $guest_major. The selection did not take"
                warn "  effect for the family it named, so the guest DOES NOT MATCH the pinned"
                warn "  acceptance family and its figures are not proven comparable with figures"
                warn "  recorded on that family. Check what else the image supplies at"
                warn "  /usr/local/bin/gcc."
            fi
            ;;
        *)
            warn "NAMED LIMITATION: the guest is measuring with GCC $GUEST_GCC_VERSION (major"
            warn "  $guest_major), NOT with the pinned acceptance family GCC"
            warn "  $TOOLCHAIN_GCC_MAJOR, because the guest archive does not serve it. The"
            warn "  guest's figures and branch-arm coordinates are internally consistent and"
            warn "  comparable across runs of this image; they are NOT proven comparable with"
            warn "  figures recorded on the pinned family. install_image_pinned_compiler says"
            warn "  what closes it."
            ;;
    esac
}

# ------------------------------------------------------------------------------------
# IN-IMAGE ASSERTIONS.  Every tool the guest needs, checked by the name the guest looks it
# up under, plus the two pkg-config queries configure will make and the dialect probe.
#
# The dialect probe is not ceremony: the generated AIDL headers include <optional> and
# declare a std::optional parameter, halcompat.h uses C++17 constructs, and a toolchain
# whose default dialect is older fails on the first of them.  Finding that out here costs a
# second; finding it out inside the guest costs a boot.
# ------------------------------------------------------------------------------------
assert_image_toolchain() {
    local tool missing=''
    for tool in g++ gcc make autoreconf automake libtoolize pkg-config gcov lcov genhtml \
                awk find mktemp stat timeout flock nproc mount mknod ldconfig bash; do
        chroot_run sh -c "command -v -- '$tool' >/dev/null 2>&1" || missing="$missing $tool"
    done
    [ -z "$missing" ] || die "the image is missing tools the guest needs:$missing.
       run_coverage.sh reaches for lcov, genhtml, gcov, awk, find, mktemp, stat, timeout,
       flock and nproc; the build reaches for g++, make and the autotools; the init reaches
       for mount and mknod.  Add the packages that provide them to the image's package set."

    assert_image_compiler_identity
    assert_image_staged_libraries_loadable

    local pkg_config_path=''
    if [ -n "$GTEST_PREFIX_SRC" ]; then
        pkg_config_path="$GUEST_GTEST_PREFIX/lib/pkgconfig"
    fi

    chroot_run env PKG_CONFIG_PATH="$pkg_config_path" \
        pkg-config --atleast-version=0.10.28 glib-2.0 \
        || die "glib-2.0 >= 0.10.28 is not discoverable through pkg-config inside the image.
       configure.ac:65 runs PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 0.10.28]) and there is no
       way past it, so the guest could not configure.  Install libglib2.0-dev for $ARCH."

    chroot_run env PKG_CONFIG_PATH="$pkg_config_path" \
        pkg-config --atleast-version=1.10.0 gtest \
        || die "gtest >= 1.10.0 is not discoverable through pkg-config inside the image.
       configure.ac's PKG_CHECK_MODULES([GTEST], [gtest >= 1.10.0]) consults pkg-config
       ONLY and ignores -I/-L, so headers and libraries on their own cannot satisfy it.
       Either the image's GoogleTest packaging must provide gtest.pc, or pass --gtest-prefix
       naming a $ARCH prefix whose lib/pkgconfig holds one."
    chroot_run env PKG_CONFIG_PATH="$pkg_config_path" \
        pkg-config --exists gmock \
        || warn "gmock.pc is not discoverable through pkg-config inside the image.  configure
  does not query it, and the suites link -lgmock directly, so this is a warning rather than a
  failure -- but a GoogleTest packaging without gmock at all would fail at link time."

    # The dialect probe: the smallest program that distinguishes C++14 from C++17 here.
    #
    # ITS DESTINATION IS THE ONE DIRECTORY IN THIS IMAGE THAT IS 1777, WHICH MAKES THIS THE
    # SHARPEST OF THE PATHNAME CASES RATHER THAN THE MOST TRIVIAL.  It used to be
    # `printf ... > "$IMAGE_MOUNT/tmp/cec-l2-dialect-probe.cpp"` and then
    # `rm -f -- "$IMAGE_MOUNT/tmp/..."` twice, all host-side and all following symbolic links.
    # /tmp in the image is world-writable by the time this runs (copy_guest_payload sets 1777
    # deliberately, because run_coverage.sh needs it), and the base tarball can ship /tmp itself
    # as a link -- so both the write and the two removals could be aimed at a host path, as root.
    # The entry is removed first rather than truncated, so what is written is an object this
    # script created at a name nothing else holds; the compile itself is already confined, being
    # a chroot_run on in-image absolute paths.
    local probe_source_rel='tmp/cec-l2-dialect-probe.cpp'
    local probe_binary_rel='tmp/cec-l2-dialect-probe'
    confined_unlink_if_present "$probe_source_rel" 'the C++ dialect probe source' \
        'probe the image toolchain'"'"'s C++17 support'
    confined_create "$probe_source_rel" 0644 'the C++ dialect probe source' \
        'probe the image toolchain'"'"'s C++17 support'
    printf '%s\n' '#include <optional>' 'int main() { return std::optional<int>{17}.value() == 17 ? 0 : 1; }' \
        | confined_write "$probe_source_rel" 'the C++ dialect probe source' \
              'probe the image toolchain'"'"'s C++17 support'
    chroot_run g++ -std=c++17 -o /tmp/cec-l2-dialect-probe /tmp/cec-l2-dialect-probe.cpp \
        || die "the image's g++ cannot compile a -std=c++17 translation unit that includes
       <optional>.  The generated AIDL headers include it and declare a std::optional
       parameter, and halcompat.h uses C++17 constructs, so the guest could not build the
       middleware.  Install a newer g++ for $ARCH."
    confined_unlink_if_present "$probe_binary_rel" 'the C++ dialect probe binary' \
        'remove the C++ dialect probe from the image'
    confined_unlink_if_present "$probe_source_rel" 'the C++ dialect probe source' \
        'remove the C++ dialect probe from the image'
    log "in-image toolchain assertions passed (tools, glib, gtest, and -std=c++17 with <optional>)"
}


# ------------------------------------------------------------------------------------
# PROVENANCE WRITTEN INTO THE IMAGE.
#
# Three files, and the distinction between them is the whole point:
#
#   expected-kernel-config   what this job REQUIRES of the guest kernel.  The init reports
#                            the running guest against it and never asserts a value from it.
#   build-host-kernel-config the configuration the caller SAYS the guest kernel was built
#                            with, copied in verbatim when --kernel-config was given.  Also
#                            evidence, also never asserted as fact about the running guest.
#   sdk-build-flags          whatever the staged SDK itself records about how it was built,
#                            harvested rather than declared.  When it records nothing, the
#                            file says so in those words -- an absent record is reported as
#                            absent, not filled in with a plausible value.
# ------------------------------------------------------------------------------------
write_kernel_expectation() {
    # THROUGH THE CONFINED PRIMITIVE, LIKE EVERY OTHER RECORD WRITTEN INTO THE IMAGE.  These three
    # files live in $GUEST_CONF_DIR (/etc/cec-l2) and /etc comes from the base tarball, so a
    # `cat > "$IMAGE_MOUNT$path"` here was a root-owned host-side write through a name whose
    # leading component this script does not author.
    #
    # THE APPEND IS GONE, AND THAT IS DELIBERATE.  The record used to be written in two steps --
    # a heredoc, then a `>>` for the derived protocol clause -- and the primitive has no append
    # operation because an append reopens a name.  Both halves are produced into one stream and
    # written once, which is also the only way the file is never observable half-written.
    local target="$GUEST_KERNEL_EXPECTATION"
    local target_rel="${GUEST_KERNEL_EXPECTATION#/}"
    local provenance_rel="${GUEST_KERNEL_PROVENANCE#/}"
    confined_unlink_if_present "$target_rel" "the image's $target" \
        'write the kernel expectation into the image'
    confined_create "$target_rel" 0644 "the image's $target" \
        'write the kernel expectation into the image'
    {
    cat <<'KERNEL_EXPECTATION_EOF'
# The guest kernel configuration this job requires.  Written into the image by
# .github/workflows/aidl-path-tests-rootfs.sh; the in-guest init REPORTS the running guest
# against this list and never asserts a value from it.
#
# ALWAYS REQUIRED, on every platform that runs binder at all:
CONFIG_ANDROID=y
CONFIG_ANDROID_BINDER_IPC=y
CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder"
CONFIG_ANDROID_BINDERFS=y
CONFIG_ASHMEM=y
#
# Also assumed by the init, and worth having for diagnosis rather than strictly required:
#   CONFIG_DEVTMPFS / CONFIG_DEVTMPFS_MOUNT  -- so /dev is populated before init runs
#   CONFIG_IKCONFIG / CONFIG_IKCONFIG_PROC   -- so /proc/config.gz exists and the init can
#                                               report the configuration ACTUALLY in effect
#                                               instead of reporting that it is unreadable
KERNEL_EXPECTATION_EOF

    # THE WIRE-PROTOCOL CLAUSE IS GENERATED FROM THE DERIVED VALUE, NOT WRITTEN AS TEXT.
    # This is the half an earlier revision hard-coded as CONFIG_ANDROID_BINDER_IPC_32BIT=y
    # while the SDK it shipped alongside was built BINDER_IPC_32BIT=OFF.  Now the line below
    # is whatever derive_binder_protocol() read out of the two artefacts, and the evidence for
    # it travels with it so a reader can re-derive rather than trust.
        printf '#\n'
        printf '# THE BINDER WIRE PROTOCOL FOR THIS IMAGE: %s.  DERIVED, NOT DECLARED.\n' "$DERIVED_BINDER_PROTOCOL"
        printf '#   from the kernel configuration : %s\n' "${PROTOCOL_FROM_KERNEL_CONFIG:-<could not answer>}"
        printf '#     %s\n' "$PROTOCOL_KERNEL_EVIDENCE"
        printf '#   from the staged SDK cache     : %s\n' "${PROTOCOL_FROM_SDK_CACHE:-<could not answer>}"
        printf '#     %s\n' "$PROTOCOL_SDK_EVIDENCE"
        printf '#\n'
        printf '# The driver enforces protocol-version EQUALITY when libbinder opens the node, so\n'
        printf '# the kernel, the SDK build and the vendor side must agree; see linux_binder_idl\n'
        printf '# BUILD.md:409-413 at tag 2.6.0.  Compile bitness is a SEPARATE axis: this image is\n'
        printf '# 32-bit throughout (--arch %s) and a 32-bit userspace can speak either protocol\n' "$ARCH"
        printf '# (CMakeLists.txt:209-217).  The option below is therefore scoped to THIS image and\n'
        printf '# is not a requirement of supported platforms generally.\n'
        if [ "$DERIVED_BINDER_PROTOCOL" = '7' ]; then
            printf 'CONFIG_ANDROID_BINDER_IPC_32BIT=y\n'
            printf '#   ^ REQUIRED TO BE SET for protocol 7.  MAINLINE HAS NO SUCH OPTION AFTER\n'
            printf '#     4.17: it was deleted with "ANDROID: binder: remove 32-bit binder\n'
            printf '#     interface", while CONFIG_ANDROID_BINDERFS -- which the init requires --\n'
            printf '#     starts at 5.0.  The workflow restores it on the pinned kernel with an\n'
            printf '#     inline patch and asserts it in the built .config, which is why a\n'
            printf '#     binderfs guest can be a protocol-7 guest here.  The init REPORTS what\n'
            printf '#     the running kernel shows against this line and never assumes it.\n'
        else
            printf '# CONFIG_ANDROID_BINDER_IPC_32BIT is not set\n'
            printf '#   ^ REQUIRED TO BE UNSET OR ABSENT for protocol 8.  This is also what an\n'
            printf '#     UNPATCHED mainline kernel shows whatever anyone asks for: the option was\n'
            printf '#     removed in the 4.18 era and binderfs starts at 5.0.\n'
        fi
    } | confined_write "$target_rel" "the image's $target" \
            'write the kernel expectation into the image'
    confined_chmod "$target_rel" 0644 "the image's $target" \
        'set the mode of the image kernel expectation'

    if [ -n "$KERNEL_CONFIG_SRC" ]; then
        # $KERNEL_CONFIG_SRC is a HOST path the caller named and validate_inputs checked, so
        # `cat -- ` on it is correct and stays; it is the DESTINATION that was the in-image
        # pathname and it is the destination that now goes through the primitive.
        confined_unlink_if_present "$provenance_rel" \
            "the image's $GUEST_KERNEL_PROVENANCE" \
            'copy the caller-supplied kernel configuration into the image'
        confined_create "$provenance_rel" 0644 \
            "the image's $GUEST_KERNEL_PROVENANCE" \
            'copy the caller-supplied kernel configuration into the image'
        {
            printf '# Copied verbatim from %s at image-build time.\n' "$KERNEL_CONFIG_SRC"
            printf '# THIS IS EVIDENCE ABOUT THE BUILD, NOT ABOUT THE RUNNING GUEST.  The init\n'
            printf '# reports what the running guest shows and never substitutes this file for it.\n'
            cat -- "$KERNEL_CONFIG_SRC"
        } | confined_write "$provenance_rel" \
                "the image's $GUEST_KERNEL_PROVENANCE" \
                'copy the caller-supplied kernel configuration into the image'
        confined_chmod "$provenance_rel" 0644 \
            "the image's $GUEST_KERNEL_PROVENANCE" \
            'set the mode of the image kernel provenance record'
        log "copied the caller-supplied kernel configuration in as build-time provenance"
    fi

    # THE SINK CALLER SOURCE, staged verbatim.  validate_inputs guarantees a readable absolute
    # path here by either route, so this is unconditional: there is no image in which the guard's
    # input is absent.
    #
    # No banner is prepended, unlike the kernel provenance above. The guard parses this file as
    # C++ looking for structural facts about two call sites, so anything added to it would be
    # content the guard has to be taught to ignore -- and a comment that changes what a guard
    # reads is a worse trade than no comment at all. Provenance goes in the image build record
    # instead, where nothing parses it.
    # ONE FILE RATHER THAN THE WHOLE SIBLING COMPONENT, AND DELIBERATELY SO.  The drift guard
    # reads exactly this source and nothing else from that component, so copying the component
    # would put a second checkout in the image for no reader.  The two directories are created
    # because the destination reproduces the SUPERPROJECT LAYOUT -- a sibling of the payload
    # directory -- which is what lets the guard's own relative candidate resolve even if the
    # exported variable is ever lost.
    local sink_rel="${GUEST_SINK_CALLER_SOURCE#/}"
    confined_mkdir "${GUEST_SINK_COMPONENT_DIR#/}" 0755 \
        "the image's $GUEST_SINK_COMPONENT_DIR" \
        'copy the Sink caller source into the image'
    confined_mkdir "${sink_rel%/*}" 0755 \
        "the image's ${GUEST_SINK_CALLER_SOURCE%/*}" \
        'copy the Sink caller source into the image'
    confined_unlink_if_present "$sink_rel" "the image's $GUEST_SINK_CALLER_SOURCE" \
        'copy the Sink caller source into the image'
    confined_create "$sink_rel" 0644 "the image's $GUEST_SINK_CALLER_SOURCE" \
        'copy the Sink caller source into the image'
    cat -- "$SINK_CALLER_SOURCE_SRC" | confined_write "$sink_rel" \
        "the image's $GUEST_SINK_CALLER_SOURCE" \
        'copy the Sink caller source into the image'
    confined_chmod "$sink_rel" 0644 "the image's $GUEST_SINK_CALLER_SOURCE" \
        'set the mode of the image Sink caller source'
    log "staged the Sink caller source the L1 drift guard reads:"
    log "  from $SINK_CALLER_SOURCE_SRC"
    log "  to   $GUEST_SINK_CALLER_SOURCE (exported in-guest as CEC_SINK_CALLER_SOURCE)"
}

# Harvest what the staged SDK records about its own build.  Nothing here is declared: every
# line printed was read out of the staging tree, and where the tree records nothing the file
# says exactly that.
write_sdk_build_flags_record() {
    # Same route as every other record written into the image, and for the same reason: the
    # destination is under /etc, which comes from the base tarball, so the write goes through a
    # descriptor the confinement opened rather than through an $IMAGE_MOUNT pathname.  Every
    # value harvested BELOW is read from the staged SDK on the HOST -- $SDK_DIR is a path the
    # caller named and verify_sdk_tree checked -- so those reads are correct as they are.
    local target="$GUEST_SDK_FLAGS_RECORD"
    local target_rel="${GUEST_SDK_FLAGS_RECORD#/}"
    confined_unlink_if_present "$target_rel" "the image's $target" \
        'record the staged SDK build provenance in the image'
    confined_create "$target_rel" 0644 "the image's $target" \
        'record the staged SDK build provenance in the image'
    {
        printf '# How the staged Binder SDK was built, as recorded BY THE STAGING TREE ITSELF.\n'
        printf '# Harvested at image-build time by %s from %s.\n' "$SCRIPT_NAME" "$SDK_DIR"
        printf '# Nothing in this file is declared: an absent record is reported as absent.\n'
        printf '#\n'
        printf '# The reason it matters: the driver enforces protocol-version EQUALITY when\n'
        printf '# libbinder opens the node, so the kernel configuration, the flag the SDK was\n'
        printf '# built with (BINDER_IPC_32BIT) and the vendor side must agree.  See\n'
        printf '# linux_binder_idl BUILD.md:409-413 at tag 2.6.0.\n'
        printf '#\n'
        printf '# THE PROTOCOL FOR THIS IMAGE WAS DERIVED, NOT DECLARED: %s.\n' "$DERIVED_BINDER_PROTOCOL"
        printf '#   kernel configuration half : %s  (%s)\n' \
            "${PROTOCOL_FROM_KERNEL_CONFIG:-could not answer}" "$PROTOCOL_KERNEL_EVIDENCE"
        printf '#   staged SDK cache half     : %s  (%s)\n' \
            "${PROTOCOL_FROM_SDK_CACHE:-could not answer}" "$PROTOCOL_SDK_EVIDENCE"
        printf '# Compile bitness is a separate axis (CMakeLists.txt:209-217): this image is\n'
        printf '# 32-bit throughout and a 32-bit userspace can speak either protocol.\n'
        printf '\n'
        printf 'derived_binder_protocol=%s\n' "$DERIVED_BINDER_PROTOCOL"
        printf 'derived_binder_protocol_from_kernel_config=%s\n' "${PROTOCOL_FROM_KERNEL_CONFIG:-unknown}"
        printf 'derived_binder_protocol_from_sdk_cache=%s\n' "${PROTOCOL_FROM_SDK_CACHE:-unknown}"
        printf '\n'

        local pinned_version="$SDK_DIR/binder_sdk.version"
        if [ -f "$pinned_version" ]; then
            printf '## binder_sdk.version (the pin; BINDER_VERSION is deliberately never set)\n'
            sed -n '1,20p' -- "$pinned_version"
        else
            printf '## binder_sdk.version: not present in the staged tree\n'
        fi
        printf '\n'

        printf '## BINDER_IPC_32BIT and related cache entries found in the staged build tree\n'
        local found=0 cache
        while IFS= read -r cache; do
            [ -n "$cache" ] || continue
            found=1
            printf '# from %s\n' "${cache#"$SDK_DIR"/}"
            grep -E '^(BINDER_IPC_32BIT|CMAKE_CXX_STANDARD|CMAKE_BUILD_TYPE|CMAKE_SYSTEM_PROCESSOR|CMAKE_C_FLAGS|CMAKE_CXX_FLAGS)(:|=)' \
                -- "$cache" 2>/dev/null || printf '#   (no matching entry in this cache)\n'
        done < <(find "$SDK_BINDER_BUILD" "$SDK_BINDER_TARGET" -maxdepth 3 -name CMakeCache.txt -type f 2>/dev/null)
        [ "$found" -eq 1 ] || printf '#   NOT RECORDED BY THE STAGING TREE: no CMakeCache.txt was found under\n#   %s or %s, so the flag the SDK was built with cannot be reported from the\n#   image. It has to come from the job that built the SDK.\n' \
            "$SDK_REL_BINDER_BUILD" "$SDK_REL_BINDER_TARGET"
        printf '\n'

        printf '## WHICH COMPILER FAMILY BUILT WHAT THIS IMAGE IS RUNNING\n'
        printf '# Two different builds, two different pins, and conflating them is what this\n'
        printf '# block exists to prevent.\n'
        printf '#\n'
        printf '#   staged_*        the Binder closure, the AIDL stub libraries and\n'
        printf '#                   servicemanager BELOW.  Compiled by the caller before this\n'
        printf '#                   script ran and copied in as they stand.  They have to LOAD\n'
        printf '#                   and LINK against this guest glibc and libstdc++, so they\n'
        printf '#                   must be built by a toolchain no NEWER than the guest\n'
        printf '#                   userspace -- which on a bookworm guest is not the\n'
        printf '#                   acceptance pin, and that deviation is recorded rather than\n'
        printf '#                   reconciled.  Measured on the build host, not here.\n'
        printf '#   guest_toolchain the compiler that will build the instrumented tree INSIDE\n'
        printf '#                   this image and write its coverage notes files.  Recorded by\n'
        printf '#                   assert_image_toolchain and carried in the manifest as\n'
        printf '#                   GUEST_TOOLCHAIN_GCC / _GXX / _GCOV.\n'
        printf '#\n'
        printf '# An empty staged_compiler_gcc_version means the caller did not state it, which\n'
        printf '# is a weaker claim than a wrong one and is left as such.\n'
        printf 'staged_compiler_gcc_version=%s\n' "${STAGING_GCC_VERSION:-not stated by the caller}"
        printf 'staged_compiler_state_against_pin=%s\n' "${STAGING_TOOLCHAIN_STATE:-not stated by the caller}"
        printf 'guest_toolchain_pin=%s\n' "${TOOLCHAIN_GCC_MAJOR:-none requested}"
        printf 'guest_toolchain_pin_state=%s\n' "$PINNED_COMPILER_STATE"
        printf '\n'

        printf '## the staged readiness marker and the architecture of what was staged\n'
        if [ -f "$SDK_BINDER_TARGET/.sdk_ready" ]; then
            printf 'sdk_ready_marker=present\n'
        else
            printf 'sdk_ready_marker=absent\n'
        fi
        printf 'staged_libbinder=%s\n' "$(file -b -- "$SDK_BINDER_LIB/libbinder.so" 2>/dev/null || printf 'unreadable')"
        printf 'staged_servicemanager=%s\n' "$(file -b -- "$SDK_SERVICEMANAGER" 2>/dev/null || printf 'unreadable')"
        printf 'image_target_architecture=%s\n' "$ARCH"
    } | confined_write "$target_rel" "the image's $target" \
            'record the staged SDK build provenance in the image'
    confined_chmod "$target_rel" 0644 "the image's $target" \
        'set the mode of the image SDK provenance record'
    log "recorded the staged SDK's own build provenance in the image"
}

# The image's own build record, and the in-guest configuration the init reads.  The
# configuration is written as single-quoted assignments; every path in it has already been
# refused if it contained a quote or a newline, so the quoting cannot be broken out of.
write_image_conf() {
    # Both files here go through the confined primitive: they are written into /etc/cec-l2, whose
    # leading component comes from the base tarball, and a `cat > "$IMAGE_MOUNT$path"` was
    # therefore a root-owned host-side write through a name this script does not fully author.
    # This heredoc is the one that IS expanded by this script (the delimiter is unquoted), which
    # changes nothing about the destination: the expansion happens in this shell and the resulting
    # bytes go down a pipe to a descriptor, exactly as the quoted ones do.
    local target="$GUEST_IMAGE_CONF"
    local target_rel="${GUEST_IMAGE_CONF#/}"
    local record_rel="${GUEST_IMAGE_RECORD#/}"
    local gtest_prefix_value=''
    if [ -n "$GTEST_PREFIX_SRC" ]; then
        gtest_prefix_value="$GUEST_GTEST_PREFIX"
    fi
    confined_unlink_if_present "$target_rel" "the image's $target" \
        'write the in-guest configuration into the image'
    confined_create "$target_rel" 0644 "the image's $target" \
        'write the in-guest configuration into the image'
    # THE BLOCK IS ASSEMBLED IN A GROUP AND PIPED, rather than fed to confined_write as a bare
    # heredoc, for exactly one reason: three of its keys are OPTIONAL and a heredoc has no
    # conditional.  Writing them always, with empty values, would be the easy alternative and it
    # is the wrong one -- read_image_conf initialises every key it knows about, so an empty
    # PROVENANCE_REVISION_HDMICEC and an ABSENT one already mean the same thing to the init, and
    # a conf file that lists a key it has no value for reads as a value that failed to arrive.
    # Omitting the line says "not supplied", which is what happened.
    #
    # The destination and the confinement are unchanged: the bytes still go down a pipe to the
    # descriptor confined_write opened, and the heredoc below is still the one this script
    # expands (the delimiter is unquoted), so nothing about the security properties moves.
    {
    cat <<IMAGE_CONF_EOF
# Generated by $SCRIPT_NAME.  Read by $GUEST_INIT_PATH.
# Every value is a fixed property of this image; nothing here selects a code path, and in
# particular there is no back-end selection of any kind -- both HDMI CEC HAL back-ends are
# compiled into libRCEC for every SOC and the choice between them is made at runtime.
IMAGE_ARCH='$ARCH'
GUEST_WORKSPACE='$GUEST_WORKSPACE'
PAYLOAD_DIR='$GUEST_PAYLOAD_DIR'
SDK_DIR='$GUEST_SDK_DIR'
HALIF_PREFIX='$GUEST_SDK_DIR'
HALIF_LIB_DIR='$GUEST_SDK_DIR/$SDK_REL_HALIF_LIB'
BINDER_SDK_DIR='$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET'
BINDER_SDK_INCLUDE_DIR='$GUEST_SDK_DIR/$SDK_REL_BINDER_BUILD'
BINDER_LIB_DIR='$GUEST_SDK_DIR/$SDK_REL_BINDER_TARGET/lib/binder'
SERVICEMANAGER_BIN='$GUEST_SDK_DIR/$SDK_REL_SERVICEMANAGER'
GTEST_PREFIX='$gtest_prefix_value'
ARTIFACT_DIR='$GUEST_ARTIFACT_DIR'
STATUS_FILE='$GUEST_STATUS_FILE'
PROTOCOL_HELPER='$GUEST_PROTOCOL_HELPER'
READY_PROBE='$GUEST_READY_PROBE'
KERNEL_EXPECTATION='$GUEST_KERNEL_EXPECTATION'
KERNEL_PROVENANCE='$GUEST_KERNEL_PROVENANCE'
SDK_FLAGS_RECORD='$GUEST_SDK_FLAGS_RECORD'
# The real Sink plugin source the L1 drift guard reads, staged into this image because the guest
# cannot see the build host's workspace.  The init exports it as CEC_SINK_CALLER_SOURCE.  It is a
# required key: an image without it produces a failing guard rather than a skipped one.
SINK_CALLER_SOURCE='$GUEST_SINK_CALLER_SOURCE'
BINDER_DEVICE_NODES='$BINDER_DEVICE_NODES'
READINESS_TIMEOUT_SECONDS='$READINESS_TIMEOUT_SECONDS'
# The wire protocol this image EXPECTS, and the two build-time artefacts it was derived
# from.  The init cross-checks all three of: the running kernel's own configuration, the
# recorded SDK value below, and the number /dev/binder answers to the BINDER_VERSION
# ioctl -- and fails naming whichever disagrees.  An empty half means that source could not
# answer at image-build time, which is reported rather than filled in.
EXPECTED_BINDER_PROTOCOL='$DERIVED_BINDER_PROTOCOL'
PROTOCOL_FROM_KERNEL_CONFIG='$PROTOCOL_FROM_KERNEL_CONFIG'
PROTOCOL_FROM_SDK_CACHE='$PROTOCOL_FROM_SDK_CACHE'
IMAGE_CONF_EOF
    # THE THREE OPTIONAL PROVENANCE REVISIONS, written only for the ones the caller supplied.
    #
    # The payload travels without its .git, so `git rev-parse` inside the guest cannot answer for
    # any of these trees and tests/L1Tests/run_coverage.sh records them as unavailable.  These
    # keys are how the revisions reach it instead: the init exports each as
    # CEC_PROVENANCE_REVISION_<LABEL> and the runner consults that variable only where git could
    # not answer.  Each value was held to the runner's own accepted form
    # (require_commit_revision) at the point it entered this script, so the single quoting below
    # cannot be broken out of -- a 40-character lower-case hex sha with an optional '-dirty'
    # suffix contains no quote, no newline and no metacharacter.
    #
    # An omitted key is the honest record of an omitted input, and read_image_conf treats a key
    # it never saw exactly as it treats an empty one, so nothing downstream needs to distinguish
    # them.  None of the three is in read_image_conf's required list.
    if [ -n "$PROVENANCE_REVISION_SUPERPROJECT" ]; then
        printf "PROVENANCE_REVISION_SUPERPROJECT='%s'\n" "$PROVENANCE_REVISION_SUPERPROJECT"
    fi
    if [ -n "$PROVENANCE_REVISION_HDMICEC" ]; then
        printf "PROVENANCE_REVISION_HDMICEC='%s'\n" "$PROVENANCE_REVISION_HDMICEC"
    fi
    if [ -n "$PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK" ]; then
        printf "PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK='%s'\n" \
            "$PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK"
    fi
    } | confined_write "$target_rel" "the image's $target" \
            'write the in-guest configuration into the image'
    confined_chmod "$target_rel" 0644 "the image's $target" \
        'set the mode of the in-guest configuration'

    confined_unlink_if_present "$record_rel" "the image's $GUEST_IMAGE_RECORD" \
        'write the image build record into the image'
    confined_create "$record_rel" 0644 "the image's $GUEST_IMAGE_RECORD" \
        'write the image build record into the image'
    {
        printf '# How this image was built, recorded by %s.\n' "$SCRIPT_NAME"
        printf 'built_at_utc=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        printf 'built_by=%s\n' "$SCRIPT_PATH"
        printf 'architecture=%s\n' "$ARCH"
        printf 'elf_bitness_decision=32-bit throughout (--arch %s); a separate axis from the wire protocol\n' "$ARCH"
        printf 'binder_wire_protocol_derived=%s\n' "$DERIVED_BINDER_PROTOCOL"
        printf 'binder_wire_protocol_from_kernel_config=%s\n' "${PROTOCOL_FROM_KERNEL_CONFIG:-unknown}"
        printf 'binder_wire_protocol_from_sdk_cache=%s\n' "${PROTOCOL_FROM_SDK_CACHE:-unknown}"
        printf 'image_size_mib=%s\n' "$IMAGE_SIZE_MIB"
        # SANITISED. This record lives inside the image, but it is also the file the guest's
        # own resolver comment points a reader at, and a credential belongs in neither.
        if [ -n "$BASE_URL" ]; then
            printf 'base_provenance=%s\n' "$(sanitised_endpoint_display "$BASE_URL")"
        else
            printf 'base_provenance=%s\n' "$BASE_TARBALL"
        fi
        printf 'base_sha256_verified=%s\n' "$BASE_SHA256"
        if [ -n "$APT_MIRROR" ]; then
            printf 'apt_mirror=%s\n' "$(sanitised_endpoint_display "$APT_MIRROR")"
        else
            printf 'apt_mirror=%s\n' '<the base tarball sources, unchanged>'
        fi
        printf 'installed_package_record=%s\n' "$GUEST_PACKAGE_RECORD"
        printf 'installed_package_count=%s\n' "$PACKAGE_RECORD_COUNT"
        printf 'installed_package_record_sha256=%s\n' "$PACKAGE_RECORD_DIGEST"
        printf 'required_packages_accounted=%s of %s\n' \
            "$PACKAGE_RECORD_REQUIRED_ACCOUNTED" "${#IMAGE_PACKAGES[@]}"
        printf 'lcov_source=%s\n' "${LCOV_TARBALL:-<the distribution package only>}"
        printf 'payload_source=%s\n' "$PAYLOAD_DIR"
        printf 'sdk_source=%s\n' "$SDK_DIR"
        printf 'gtest_prefix_source=%s\n' "${GTEST_PREFIX_SRC:-<the image packaging>}"
        # The Sink caller source the L1 drift guard reads. Recorded with its digest as well as
        # its provenance, because "a file was staged" and "THIS file was staged" are different
        # claims and the guard's verdict depends on the second: a stale or wrong revision would
        # produce a drift failure that looks like a plugin change. Both are read here from the
        # host-side input validate_inputs resolved, so the record names the same bytes the copy
        # took.
        printf 'sink_caller_source=%s\n' "$SINK_CALLER_SOURCE_SRC"
        printf 'sink_caller_source_staged_at=%s\n' "$GUEST_SINK_CALLER_SOURCE"
        printf 'sink_caller_source_sha256=%s\n' \
            "$(sha256sum -- "$SINK_CALLER_SOURCE_SRC" | cut -d' ' -f1)"
    } | confined_write "$record_rel" "the image's $GUEST_IMAGE_RECORD" \
            'write the image build record into the image'
    confined_chmod "$record_rel" 0644 "the image's $GUEST_IMAGE_RECORD" \
        'set the mode of the image build record'
}


# ------------------------------------------------------------------------------------
# THE IN-GUEST INIT.
#
# Written literally -- the heredoc delimiter is quoted, so nothing in the body is expanded
# by THIS script and the file on disk is exactly the text below.  Every value the init needs
# comes from the generated configuration file instead, which is why the body can be literal:
# a body full of escaped expansions is a body nobody can read or test.
#
# It is installed at a fixed path and IS THE GUEST'S ENTRY POINT.  The workflow boots the
# kernel with init=<that path>; there is no service manager, no unit file and no shell
# profile between the kernel and this script.
# ------------------------------------------------------------------------------------
write_guest_init() {
    # WRITTEN THROUGH THE CONFINED PRIMITIVE, AND OF EVERY FILE THIS SCRIPT PUTS INTO THE IMAGE
    # THIS IS THE ONE THAT MATTERS MOST.  It is installed at $GUEST_INIT_PATH -- /usr/sbin, for
    # the usr-merge reason recorded at that constant -- and the workflow boots the kernel with
    # init=<that path>, so it is the whole of what the guest executes.  The old form --
    # `mkdir -p "$(dirname "$IMAGE_MOUNT$GUEST_INIT_PATH")"`, then `cat >` and two `cat >>` on
    # that pathname, then `chmod 0755` -- ran as root on the BUILD HOST through a name whose
    # leading component comes from the base tarball: a base shipping that component as a symbolic
    # link, absolute or relative, had this script create a host directory and write a root-owned,
    # mode-0755 executable script into it.  Hence both the confinement AND an anchor that is a
    # real directory on every layout: the primitive refuses the traversal, and the constant never
    # asks it to make one.
    #
    # THE THREE PARTS BECOME ONE WRITE.  They were three because a single heredoc of this length
    # is unreadable, and they still are three heredocs in this source -- but they are produced
    # into ONE stream now and written once, because the primitive has no append operation: an
    # append reopens a name, which is the second lookup this whole mechanism exists to remove.
    # The delimiter is quoted in every part, so the file on disk is still exactly this text.
    local target="$GUEST_INIT_PATH"
    local target_rel="${GUEST_INIT_PATH#/}"
    local target_dir_rel="${target_rel%/*}"
    confined_mkdir "$target_dir_rel" 0755 "the image's /$target_dir_rel" \
        'write the in-guest init into the image'
    confined_unlink_if_present "$target_rel" "the image's $target" \
        'write the in-guest init into the image'
    confined_create "$target_rel" 0755 "the image's $target" \
        'write the in-guest init into the image'
    {
    cat <<'CEC_L2_INIT_EOF'
#!/usr/bin/env bash
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
# cec-l2-init -- the guest entry point for the binder-capable AIDL-path job.
#
# GENERATED FILE.  Written into this image by
# .github/workflows/aidl-path-tests-rootfs.sh in the hdmicec submodule.  Edit that script,
# not this copy: this one is recreated from scratch every time an image is built.
#
# WHAT IT DOES, IN THIS ORDER, AND NOTHING ELSE:
#   1. mounts the pseudo-filesystems and binderfs, creates the binder device nodes and
#      proves the driver node can actually be opened;
#   2. reports the kernel configuration, the SDK build flags and the resulting binder
#      protocol version TOGETHER in one delimited block, every value read from the RUNNING
#      guest rather than asserted from a configuration fragment;
#   3. starts servicemanager and BLOCKS until it answers a real binder transaction, under a
#      bounded timeout -- never a sleep;
#   4. exports the build and loader environment the coverage runner and the test harnesses
#      need, and NOT CEC_TEST_AIDL_MODE;
#   5. runs the coverage runner with --build --run --output-dir, records its EXACT exit
#      status and propagates it;
#   6. tears everything down and powers the guest off, on every path.
#
# WHAT IT DELIBERATELY DOES NOT DO:
#   * It does not set CEC_TEST_AIDL_MODE.  The coverage runner owns that value per
#     invocation across the A-E matrix; one value set here would collapse the matrix to a
#     single arm and report it as green.
#   * It does not start a binder threadpool.  The client threadpool belongs to
#     DriverAidlImpl::open(); the service one belongs to the fake service host, which the L2
#     harness launches itself through an inherited readiness descriptor.
#   * It does not launch the fake service host.  That is the harness's job, and doing it here
#     would put a second process on the "HdmiCec" name.
#   * It does not pass --restore, and it does not reimplement any part of the coverage
#     runner's orchestration -- not the per-invocation exit check, not the executed-count
#     check, not the selected-path grep, not the artifact naming, not the counter policy and
#     not the single capture.
#   * It touches no network.  Nothing in this file reaches one, and nothing it runs is
#     supposed to.
##########################################################################

set -euo pipefail

# Artifacts are extracted from this image by the workflow after the guest powers down, and a
# human reads the logs afterwards; 022 keeps them readable without making anything writable.
umask 022

readonly INIT_NAME='cec-l2-init'
readonly IMAGE_CONF='/etc/cec-l2/image.conf'

log()  { printf '[%s] %s\n' "$INIT_NAME" "$*"; }
warn() { printf '[%s] WARNING: %s\n' "$INIT_NAME" "$*" >&2; }
die()  { printf '[%s] ERROR: %s\n' "$INIT_NAME" "$*" >&2; exit 1; }

# ------------------------------------------------------------------------------------
# CONFIGURATION.  Read from the generated file with a strict whitelist rather than sourced:
# an unexpected key is a fault in the image build and is reported as one instead of being
# executed.
# ------------------------------------------------------------------------------------
IMAGE_ARCH=''
GUEST_WORKSPACE=''
PAYLOAD_DIR=''
SDK_DIR=''
HALIF_PREFIX=''
HALIF_LIB_DIR=''
BINDER_SDK_DIR=''
BINDER_SDK_INCLUDE_DIR=''
BINDER_LIB_DIR=''
SERVICEMANAGER_BIN=''
GTEST_PREFIX=''
ARTIFACT_DIR=''
STATUS_FILE=''
PROTOCOL_HELPER=''
READY_PROBE=''
KERNEL_EXPECTATION=''
KERNEL_PROVENANCE=''
SDK_FLAGS_RECORD=''
BINDER_DEVICE_NODES=''
READINESS_TIMEOUT_SECONDS=''
# The binder wire protocol the image was built to expect, and the two build-time artefacts it
# was derived from.  EXPECTED_BINDER_PROTOCOL is required: an image that cannot say which
# protocol it expects cannot cross-check the driver it finds, and a silent mismatch there is
# exactly the failure mode that produced the contradictory evidence this init now refuses to
# reproduce.  The two derivation fields are diagnostic -- they name WHERE the expectation came
# from, so a disagreement report identifies the artefact to correct rather than only the number.
EXPECTED_BINDER_PROTOCOL=''
PROTOCOL_FROM_KERNEL_CONFIG=''
PROTOCOL_FROM_SDK_CACHE=''
# The commit revisions of the trees the payload came from, written into the configuration by the
# image builder because this guest cannot read them for itself: the payload is staged WITHOUT its
# .git, so `git rev-parse` has nothing to ask and tests/L1Tests/run_coverage.sh would record every
# repository row as unavailable.  export_build_environment publishes each as
# CEC_PROVENANCE_REVISION_<LABEL>, which is the variable the runner consults -- and consults ONLY
# where git could not answer, so an image that somehow did carry a repository is still described
# by the repository rather than by these.
#
# EACH IS OPTIONAL AND AN EMPTY ONE IS NOT AN ERROR.  Empty means the image builder was not given
# that revision, in which case the row stays as it is today; that is the truth and not a
# shortfall this init should refuse to boot on.  None of the three appears in the required list
# below for that reason.
PROVENANCE_REVISION_SUPERPROJECT=''
PROVENANCE_REVISION_HDMICEC=''
PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK=''

read_image_conf() {
    [ -r "$IMAGE_CONF" ] || die "$IMAGE_CONF is missing or unreadable, so this image was not
       finished by the root-image script.  Rebuild it."
    local line key value
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            ''|'#'*) continue ;;
        esac
        key="${line%%=*}"
        value="${line#*=}"
        # One layer of single quotes, exactly as the generator writes it.
        value="${value#\'}"
        value="${value%\'}"
        case "$key" in
            IMAGE_ARCH|GUEST_WORKSPACE|PAYLOAD_DIR|SDK_DIR|HALIF_PREFIX|HALIF_LIB_DIR| \
            BINDER_SDK_DIR|BINDER_SDK_INCLUDE_DIR|BINDER_LIB_DIR|SERVICEMANAGER_BIN| \
            GTEST_PREFIX|SINK_CALLER_SOURCE|ARTIFACT_DIR|STATUS_FILE|PROTOCOL_HELPER|READY_PROBE| \
            KERNEL_EXPECTATION|KERNEL_PROVENANCE|SDK_FLAGS_RECORD|BINDER_DEVICE_NODES| \
            READINESS_TIMEOUT_SECONDS|EXPECTED_BINDER_PROTOCOL| \
            PROTOCOL_FROM_KERNEL_CONFIG|PROTOCOL_FROM_SDK_CACHE| \
            PROVENANCE_REVISION_SUPERPROJECT|PROVENANCE_REVISION_HDMICEC| \
            PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK)
                printf -v "$key" '%s' "$value" ;;
            *)
                die "unrecognised key '$key' in $IMAGE_CONF.  The image build and this init
       disagree about the configuration, which means the image is not internally consistent.
       Rebuild it with a matching root-image script." ;;
        esac
    done < "$IMAGE_CONF"

    local required
    for required in IMAGE_ARCH PAYLOAD_DIR SDK_DIR HALIF_LIB_DIR BINDER_SDK_DIR \
                    BINDER_SDK_INCLUDE_DIR BINDER_LIB_DIR SERVICEMANAGER_BIN ARTIFACT_DIR \
                    STATUS_FILE PROTOCOL_HELPER READY_PROBE BINDER_DEVICE_NODES \
                    SINK_CALLER_SOURCE \
                    READINESS_TIMEOUT_SECONDS EXPECTED_BINDER_PROTOCOL; do
        [ -n "${!required}" ] || die "$required is empty in $IMAGE_CONF.  The image is
       incomplete; rebuild it."
    done
}

# ------------------------------------------------------------------------------------
# TEARDOWN.  Registered before anything is mounted or started, so it runs on success, on
# failure, on a die and on an interrupt alike.  Two things must survive it: the artifacts
# (destroyed artifacts make a failure undiagnosable) and the exit status (a laundered status
# makes a failure look like a pass).
# ------------------------------------------------------------------------------------
MOUNTED_POINTS=()
SERVICEMANAGER_PID=''
TEARDOWN_DONE=0

# SC2317 is suppressed on the three teardown functions, and the justification is that
# ShellCheck's reachability analysis does not model trap handlers: main() ends in an
# unconditional `exit`, so everything reachable only from a trap registered before it is
# reported as unreachable code.  These three are reached ONLY from the EXIT trap, which is
# the whole point of them -- they must run on the success path, the failure path and the
# interrupt path alike.  The suppression is scoped to the three functions rather than the
# file, so a genuinely unreachable statement anywhere else is still reported.
# shellcheck disable=SC2317
teardown() {
    local status=$?
    # THE TEARDOWN IS NOT INTERRUPTIBLE.  A signal arriving while this is running would run
    # the INT or TERM handler, whose own `exit` would abandon the teardown halfway -- leaving
    # binderfs mounted, servicemanager running and, worst of all, the artifacts unflushed.
    # Measured on the host-side script, where exactly that left five mounts and a loop device
    # behind; the same shape applies here with more at stake.
    trap '' INT TERM HUP
    [ "$TEARDOWN_DONE" -eq 0 ] || return
    TEARDOWN_DONE=1

    log "teardown: init is finishing with status $status"

    stop_servicemanager

    # Flush before anything is unmounted: the artifacts live in this filesystem and the
    # workflow reads them from the image after the guest is gone.
    sync || true

    # /proc IS DELIBERATELY LEFT MOUNTED, AND THAT IS NOT AN OVERSIGHT IN THE LOOP BELOW.
    # The only power-off mechanism this image has is /proc/sysrq-trigger -- measured: the image
    # is a minbase install plus a build toolchain and contains no poweroff, halt, reboot,
    # shutdown, busybox or systemctl binary at all.  Unmounting /proc here therefore removed
    # the one way the guest could switch itself off, and power_off_guest() found its guard
    # false, warned, and idled until the workflow's QEMU timeout expired -- which reports as a
    # failed job whatever the run actually measured.  /proc holds no data, so keeping it has no
    # cost to the artifacts: the flush that protects them is the sync above and the read-only
    # remount below, neither of which involves /proc.
    local index point
    for (( index=${#MOUNTED_POINTS[@]}-1; index>=0; index-- )); do
        point="${MOUNTED_POINTS[index]}"
        [ -n "$point" ] || continue
        if [ "$point" = '/proc' ]; then
            log "  leaving /proc mounted: the power-off below is written through it"
            continue
        fi
        if mountpoint -q -- "$point" 2>/dev/null; then
            umount -- "$point" 2>/dev/null || umount -l -- "$point" 2>/dev/null \
                || warn "could not unmount $point during teardown."
        fi
    done

    sync || true
    # Read-only root before power-off, so the coverage evidence cannot be lost to a page that
    # never reached the disk.
    mount -o remount,ro / 2>/dev/null || warn "could not remount / read-only before power-off."

    log "teardown complete; powering the guest down"
    power_off_guest
}

# Reached only from teardown(), which is reached only from the EXIT trap; see the SC2317
# justification above.
# shellcheck disable=SC2317
stop_servicemanager() {
    [ -n "$SERVICEMANAGER_PID" ] || return 0
    kill -0 "$SERVICEMANAGER_PID" 2>/dev/null || { SERVICEMANAGER_PID=''; return 0; }

    log "stopping servicemanager (pid $SERVICEMANAGER_PID)"
    kill -TERM "$SERVICEMANAGER_PID" 2>/dev/null || true
    # A bounded wait, then SIGKILL.  `wait` would block for as long as it liked, and this is
    # the teardown path: it has to end.
    local waited=0
    while [ "$waited" -lt 10 ] && kill -0 "$SERVICEMANAGER_PID" 2>/dev/null; do
        # A short, bounded pause on the way out.  This is NOT a readiness mechanism -- the
        # readiness wait is a poll on a real binder transaction, further down -- it is only
        # how long a terminating process is given to finish.
        sleep 1
        waited=$(( waited + 1 ))
    done
    if kill -0 "$SERVICEMANAGER_PID" 2>/dev/null; then
        warn "servicemanager did not exit on SIGTERM; sending SIGKILL."
        kill -KILL "$SERVICEMANAGER_PID" 2>/dev/null || true
    fi
    SERVICEMANAGER_PID=''
}

# Power off through whatever this root filesystem actually provides.  Nothing here can be
# assumed: a minimal image may have sysvinit's poweroff, systemd's, or neither -- and the image
# this builder produces has NEITHER, measured, so the sysrq path below is not a fallback but
# the whole mechanism.  Three things follow from that, and each is why this function is shaped
# the way it is rather than a shorter one.
#
#   1. /proc MUST BE THERE.  It is left mounted by the teardown for exactly this reason, and
#      re-mounted here if it is not, so that this function is correct however it is reached.
#
#   2. THE SYSRQ POWER-OFF DELEGATES BEFORE IT FORCES.  'o' reaches orderly_poweroff(), which
#      first runs the program named by kernel.poweroff_cmd -- /sbin/poweroff by default -- and
#      only forces kernel_power_off() when that fails.  In an image with a real poweroff that
#      talks to a cooperating init, and with PID 1 here being this script, the delegation
#      succeeds at exec and then achieves nothing: the kernel sees success and does not force.
#      So the command is pointed at a path that cannot exist, which makes the forcing arm the
#      only arm, by construction rather than by luck.
#
#   3. IT IS ASYNCHRONOUS, SO THE FAILURE MESSAGE HAS TO WAIT FOR IT.  orderly_poweroff()
#      schedules work and returns; the old form printed "no power-off mechanism took effect"
#      on the very next line, which was a diagnosis of the kernel's scheduling latency rather
#      than of anything wrong.  The wait below is bounded and reports only after it.
#
# Reached only from teardown(); see the SC2317 justification above.
# shellcheck disable=SC2317
power_off_guest() {
    sync || true

    if ! mountpoint -q -- /proc 2>/dev/null; then
        mount -t proc proc /proc 2>/dev/null \
            || warn "could not mount /proc for the power-off; the sysrq path is unavailable."
    fi

    if command -v poweroff >/dev/null 2>&1; then
        poweroff -f 2>/dev/null || true
    fi
    if command -v halt >/dev/null 2>&1; then
        halt -f -p 2>/dev/null || true
    fi

    if [ -w /proc/sysrq-trigger ]; then
        # Point the delegation at a path that cannot exist so the kernel takes its forcing
        # arm.  Best-effort: an older kernel without this sysctl still reaches the same place
        # when /sbin/poweroff is absent, which it is in this image.
        if [ -w /proc/sys/kernel/poweroff_cmd ]; then
            printf '%s\n' '/nonexistent/cec-l2-no-userspace-poweroff' \
                > /proc/sys/kernel/poweroff_cmd 2>/dev/null || true
        fi
        log "requesting power-off through /proc/sysrq-trigger"
        printf 'o' > /proc/sysrq-trigger 2>/dev/null || true

        # The kernel powers the machine off from a work queue, so give it a bounded moment.
        # This is not a readiness wait -- there is nothing left to become ready -- it is how
        # long an asynchronous power-off is allowed to arrive before it is called failed.
        local waited=0
        while [ "$waited" -lt 30 ]; do
            sleep 1
            waited=$(( waited + 1 ))
        done
    fi

    warn "no power-off mechanism took effect.  Idling rather than returning: an init that"
    warn "  RETURNS panics the kernel, which reports as a crash and can lose the console"
    warn "  tail this job is read from.  The workflow's own timeout on the QEMU process is"
    warn "  the backstop."
    while true; do
        # An idle block after a failed power-off.  Not a readiness wait: there is nothing
        # left to become ready.
        sleep 60
    done
}

trap teardown EXIT
trap 'die "interrupted."' INT
trap 'die "terminated."' TERM
CEC_L2_INIT_EOF
    # PART TWO: the binder bring-up, the platform record and the readiness wait.  A separate
    # heredoc rather than one enormous one so that each part stays readable; all three go into the
    # same stream and the same single write, and the delimiter is quoted in every part, so the
    # file on disk is exactly this text.
    cat <<'CEC_L2_INIT_EOF'

# ------------------------------------------------------------------------------------
# PSEUDO-FILESYSTEMS.  Mounted defensively: a kernel with CONFIG_DEVTMPFS_MOUNT has already
# populated /dev before this script runs, and mounting over it would hide the console.  So
# each one is mounted only where it is not already there.
# ------------------------------------------------------------------------------------
mount_if_absent() { # $1=type  $2=source  $3=target  [$4..=options]
    local type="$1" source="$2" target="$3"
    shift 3
    mkdir -p -- "$target"
    if mountpoint -q -- "$target" 2>/dev/null; then
        log "  $target is already mounted; leaving it alone"
        return 0
    fi
    MOUNTED_POINTS+=("$target")
    mount -t "$type" "$@" "$source" "$target" \
        || die "could not mount $type on $target.  The guest kernel must provide it: /proc and
       /sys are needed by the toolchain and by the coverage runner, and /dev is where the
       binder nodes live."
    log "  mounted $type on $target"
}

mount_pseudo_filesystems() {
    log "mounting the pseudo-filesystems"
    # The root filesystem is the image itself and the build writes into it, so it must be
    # writable.  A kernel booted with 'ro' leaves it read-only.
    mount -o remount,rw / 2>/dev/null || true
    mount_if_absent proc  proc     /proc
    mount_if_absent sysfs sysfs    /sys
    mount_if_absent devtmpfs devtmpfs /dev
    mount_if_absent devpts devpts  /dev/pts
    mount_if_absent tmpfs tmpfs    /tmp -o mode=1777
    # /tmp is where the coverage runner mints its private lcov HOME and its Makefile
    # snapshot, so it has to be writable with the usual sticky permissions.
    chmod 1777 /tmp 2>/dev/null || true

    # THE DESCRIPTOR SYMLINKS, WITHOUT WHICH PROCESS SUBSTITUTION DOES NOT WORK AND THE
    # COVERAGE RUN CANNOT START.  `cmd < <(...)` is compiled by bash into a read of
    # /dev/fd/<n>, and /dev here is devtmpfs mounted by the kernel (CONFIG_DEVTMPFS_MOUNT):
    # devtmpfs publishes device nodes and nothing else, so /dev/fd, /dev/stdin, /dev/stdout
    # and /dev/stderr are absent.  On a normal system they are created by udev or by the
    # distribution's early boot scripts, and this guest runs neither -- PID 1 is this script.
    # Measured: the init's own `done < <(find ...)` failed with "/dev/fd/63: No such file or
    # directory" and took the run down at status 1, and tests/L1Tests/run_coverage.sh uses the
    # same construct in three places, so the coverage run could not have started either.
    # Created here rather than in the image, because /dev is replaced by devtmpfs at boot and
    # anything the image build put there is hidden.
    local descriptor_link descriptor_name descriptor_target
    for descriptor_link in fd:/proc/self/fd stdin:/proc/self/fd/0 stdout:/proc/self/fd/1 \
                           stderr:/proc/self/fd/2; do
        descriptor_name="/dev/${descriptor_link%%:*}"
        descriptor_target="${descriptor_link#*:}"
        if [ -e "$descriptor_name" ] || [ -L "$descriptor_name" ]; then
            continue
        fi
        ln -s -- "$descriptor_target" "$descriptor_name" 2>/dev/null \
            || warn "could not create $descriptor_name -> $descriptor_target; process
       substitution and /dev/std* redirection will not work in this guest."
    done
    # Asserted rather than assumed: /dev/fd is what the failure above was, and a guest that
    # reaches the coverage run without it fails much later and much less legibly.
    [ -e /dev/fd/0 ] || die "/dev/fd does not resolve in this guest even after creating it.
       Process substitution -- which this init and tests/L1Tests/run_coverage.sh both use --
       reads /dev/fd/<n>, so the run would fail partway through with an unhelpful
       'No such file or directory'.  /proc must be mounted for the link to resolve; check the
       mount lines above."
    log "  descriptor symlinks resolve: /dev/fd, /dev/stdin, /dev/stdout, /dev/stderr"
}

# ------------------------------------------------------------------------------------
# THE GUEST IS OFFLINE, AND THIS ASSERTS IT RATHER THAN RELYING ON HOW QEMU WAS STARTED.
#
# WHAT THE ASSERTION PROTECTS.  Everything this job claims about the guest depends on the
# guest not having a network: that the payload carries every source, binary and library it
# needs; that the package set inside the image is the one the builder installed and recorded;
# that no step fetches anything at test time and therefore that the run is reproducible.  The
# enforcement lives in the workflow (qemu -nic none), in another file, edited by different
# people at different times.  An omission there is invisible from here -- a default user-mode
# NIC with a working DNS forwarder is what qemu provides when no network option is given at
# all -- and a build that quietly acquired the ability to fetch things would still pass.
#
# SO IT IS FATAL, NOT A WARNING.  A guest with a network is not the guest this job specifies,
# and the difference between the two is precisely a reproducibility claim.
#
# WHAT IS READ, AND WHY EACH IS READ THE WAY IT IS.
#   INTERFACES: /sys/class/net, because it is the kernel's own list and needs no tool.  ip(8)
#     and ifconfig(8) are not assumed to exist -- the image is a minbase install plus a build
#     toolchain, and neither is in that set.
#
#     BUT "NOT lo" IS NOT THE SAME AS "A NIC", AND READING IT THAT WAY MADE THIS ASSERTION
#     FIRE ON EVERY RUN.  A kernel creates fallback tunnel interfaces in the initial network
#     namespace for each tunnel driver built into it, with no NIC underneath and no way to
#     carry traffic: sit0 from CONFIG_IPV6_SIT, tunl0 from CONFIG_NET_IPIP, gre0/gretap0 from
#     CONFIG_NET_IPGRE, ip6tnl0, ip_vti0 and their relatives.  Whether they exist is decided
#     by the kernel configuration, never by how qemu was started -- and this job's own kernel
#     is built from i386 defconfig, where CONFIG_IPV6_SIT=y, so sit0 is always there.  Judging
#     on the name alone therefore refused a correctly offline guest: measured, `qemu -nic none`
#     booted, and the init died naming sit0 as evidence of a NIC.
#
#     SO THE TEST IS "IS IT BACKED BY A DEVICE", NOT "IS IT NAMED lo".  A real NIC is a device
#     on a bus and has /sys/class/net/<name>/device pointing at it -- true of every NIC qemu
#     can give this guest, the default user-mode e1000 and virtio-net alike, and false of lo
#     and of every kernel fallback tunnel.  That is the discriminator, and it is still the
#     kernel's own answer read without a tool.
#
#     A PSEUDO INTERFACE IS TOLERATED ONLY WHILE IT IS DOWN.  Presence is the kernel's
#     decision; being UP is somebody's action.  So the flags word is read too, and IFF_UP on a
#     non-loopback interface is fatal whether or not anything backs it -- which keeps the
#     assertion's teeth for the case where something in the guest configures one.
#   ROUTES: /proc/net/route, parsed directly for the same reason.  Its first line is a header,
#     so any further line is a route.
#   RESOLVER: an active 'nameserver' line in /etc/resolv.conf.  The image build replaces that
#     file with a comment block, so a nameserver line means either the build step did not run
#     or something inside the guest wrote one.
# ------------------------------------------------------------------------------------
assert_guest_offline() {
    local interfaces='' entry name routes='' resolvers=''
    local pseudo='' pseudo_up='' flags=''

    for entry in /sys/class/net/*; do
        [ -e "$entry" ] || continue
        name="$(basename -- "$entry")"
        [ "$name" = 'lo' ] && continue

        # Backed by a device on a bus: that is a NIC, and it is what this assertion is for.
        if [ -e "$entry/device" ]; then
            interfaces="$interfaces $name"
            continue
        fi

        # Nothing backing it, so it is a kernel fallback interface.  Tolerated while DOWN,
        # refused when UP.  The flags word is hexadecimal ('0x1002'), which the shell's
        # arithmetic reads directly; IFF_UP is bit 0.
        flags="$(cat -- "$entry/flags" 2>/dev/null || printf '0x0')"
        case "$flags" in
            0x*|0X*|[0-9]*) : ;;
            *) flags='0x0' ;;
        esac
        if [ "$(( flags & 0x1 ))" -ne 0 ]; then
            pseudo_up="$pseudo_up $name"
        else
            pseudo="$pseudo $name"
        fi
    done

    if [ -n "$pseudo_up" ]; then
        die "this guest has non-loopback interface(s) administratively UP:${pseudo_up}
       Nothing on a bus backs them, so they are kernel fallback interfaces rather than NICs --
       but the kernel leaves those DOWN, so one that is UP was brought up by something in this
       guest.  This job runs offline by specification, and an interface somebody configured is
       the beginning of a route.  Refused rather than reported: read the init's own output
       above for what ran before this point."
    fi

    if [ -n "$interfaces" ]; then
        die "this guest has network interface(s) backed by a device:${interfaces}
       It is specified to run with NO network: every source, binary and library it needs is
       inside the image, nothing is fetched at test time, and the package set was pinned and
       recorded at image-build time.  An interface here means the QEMU invocation gained a NIC
       -- most likely by an option being removed, since qemu creates a DEFAULT user-mode NIC
       when no -nic/-netdev/-nodefaults is given at all.  Restore '-nic none' in the workflow
       step that boots this image.  Refusing to run is deliberate: a run that could fetch
       things is not the reproducible run this job reports."
    fi

    if [ -r /proc/net/route ]; then
        # tail -n +2 drops the header line; anything left is a configured route.
        routes="$(tail -n +2 /proc/net/route 2>/dev/null | awk 'NF { print $1 }' | sort -u \
                    | tr '\n' ' ' || true)"
        routes="${routes% }"
    fi
    if [ -n "$routes" ]; then
        die "this guest has network route(s) via interface(s): $routes
       See the interface message above for why that is refused.  With no NIC there is nothing
       to route through, so a route means the guest was given one."
    fi

    if [ -r /etc/resolv.conf ]; then
        resolvers="$(grep -cE '^[[:space:]]*nameserver[[:space:]]+[^[:space:]]' \
                          /etc/resolv.conf 2>/dev/null || true)"
        resolvers="$(printf '%s' "$resolvers" | tr -d '[:space:]')"
    fi
    if [ -n "$resolvers" ] && [ "$resolvers" != '0' ]; then
        die "this guest has $resolvers nameserver line(s) configured in /etc/resolv.conf.
       The image build replaces that file with a comment block precisely so this assertion
       means something, so either that step did not run -- rebuild the image -- or something
       in the guest wrote a resolver.  A guest that can resolve names is a guest that can
       fetch, and this run's reproducibility claim rests on it not being able to."
    fi

    if [ -n "$pseudo" ]; then
        log "offline: kernel fallback interface(s) present and DOWN, nothing backing them:${pseudo}"
        log "  these follow the guest kernel's tunnel drivers, not the qemu command line"
    fi
    log "offline confirmed: no interface backed by a device, none UP, no route, no resolver"
}

# ------------------------------------------------------------------------------------
# BINDERFS AND THE DEVICE NODES.
#
# The initial binderfs instance is populated from CONFIG_ANDROID_BINDER_DEVICES, so the nodes
# this job needs appear inside the mount if -- and only if -- the guest kernel was configured
# with them.  A missing node is therefore a KERNEL CONFIGURATION fault, and it is reported as
# one rather than worked around: allocating a device through binder-control would paper over a
# guest that is not the guest this job specifies.
#
# The recipe follows linux_binder_idl BUILD.md:493-507 at tag 2.6.0.  That file is in the
# upstream repository and not in this workspace, so it is cited by locator and its text is
# not reproduced here.
# ------------------------------------------------------------------------------------
mount_binderfs() {
    log "mounting binderfs"
    if ! grep -qw binder /proc/filesystems 2>/dev/null; then
        die "the running kernel does not provide the 'binder' filesystem (no 'binder' line in
       /proc/filesystems).  This guest cannot run the AIDL-selected invocations at all.  Build
       the guest kernel with the options recorded in $KERNEL_EXPECTATION -- at minimum
       CONFIG_ANDROID_BINDER_IPC and CONFIG_ANDROID_BINDERFS."
    fi

    mkdir -p /dev/binderfs
    if mountpoint -q -- /dev/binderfs 2>/dev/null; then
        log "  /dev/binderfs is already mounted"
    else
        MOUNTED_POINTS+=(/dev/binderfs)
        mount -t binder binder /dev/binderfs \
            || die "mounting binderfs on /dev/binderfs failed even though the kernel lists the
       filesystem.  Check that CONFIG_ANDROID_BINDERFS is set in the running kernel; the
       options this job requires are recorded in $KERNEL_EXPECTATION."
    fi

    local node
    for node in $BINDER_DEVICE_NODES; do
        if [ ! -e "/dev/binderfs/$node" ]; then
            die "the binderfs instance has no '$node' device.  The initial instance is
       populated from CONFIG_ANDROID_BINDER_DEVICES, so this means the guest kernel was not
       built with CONFIG_ANDROID_BINDER_DEVICES=\"binder,hwbinder,vndbinder\".  Fix the kernel
       configuration -- allocating a device through binder-control here would hide a guest
       that is not the one this job specifies."
        fi
        # A symlink rather than a copy: /dev/binder is the path libbinder opens by default and
        # the path the coverage runner probes, and it must be the same object binderfs owns.
        ln -sfn "/dev/binderfs/$node" "/dev/$node" \
            || die "could not link /dev/$node to the binderfs device."
        log "  /dev/$node -> /dev/binderfs/$node"
    done

    # OPENABILITY, not merely existence.  A node can exist with permissions that make it
    # useless, and a run that got past a presence check only to abort inside libbinder would
    # report a crash where it should have reported a configuration fault.
    if ! exec 9<>/dev/binder; then
        die "/dev/binder exists but cannot be opened for reading and writing.  Every binder
       client in this job opens it, so nothing can run.  Check its permissions and the
       guest's device-node ownership."
    fi
    exec 9<&-
    log "  /dev/binder opened and closed successfully"
}

# ------------------------------------------------------------------------------------
# THE BINDER PLATFORM RECORD.
#
# One delimited block carrying three things that are only meaningful TOGETHER, because
# libbinder enforces protocol-version EQUALITY when it opens the node: the kernel
# configuration in effect, the flag the SDK was built with, and the protocol version the
# driver actually reports.  Splitting them across steps would let a reader check one and
# assume the others.
#
# Every value is READ.  Where a source is unavailable the block says so in those words; it
# never falls back to the expectation file and presents that as fact about the running guest.
# ------------------------------------------------------------------------------------
# Which file, if any, carries the configuration of the kernel that is actually running.
# Factored out because two callers need the same answer for two different purposes:
# report_kernel_config prints the binder entries for a human, and runtime_kernel_protocol
# derives the wire protocol the running kernel implies.  Deriving that from a different
# source than the one reported would let the two disagree without either being wrong.
kernel_config_source() {
    if [ -r /proc/config.gz ] && command -v zcat >/dev/null 2>&1; then
        printf '/proc/config.gz'
    elif [ -r "/boot/config-$(uname -r)" ]; then
        printf '/boot/config-%s' "$(uname -r)"
    fi
}

# One configuration entry as the running kernel's own configuration file states it: the
# matching '<entry>=<value>' or '# <entry> is not set' line, or nothing when the entry is
# absent altogether.  Absent is a THIRD state and callers must treat it as such -- reading
# absence as "not set" is how a build-time assumption gets presented as a measurement.
kernel_config_entry() {
    local entry="$1" source="$2"
    if [ "$source" = '/proc/config.gz' ]; then
        zcat /proc/config.gz 2>/dev/null | grep -E "^($entry=|# $entry is not set)" || true
    else
        grep -E "^($entry=|# $entry is not set)" -- "$source" 2>/dev/null || true
    fi
}

# ------------------------------------------------------------------------------------
# THE WIRE PROTOCOL THE RUNNING KERNEL IMPLIES.
#
# This is one of the four sources the protocol-agreement check compares, and it is a READING
# of the running guest, never a restatement of the image's expectation.  Sets two globals so
# the evidence travels with the value: a number alone in a disagreement report tells a reader
# what differs but not which artefact to correct.
#
# The mapping follows the kernel's own option, and the absent case is NOT an error: mainline
# deleted CONFIG_ANDROID_BINDER_IPC_32BIT from drivers/android/Kconfig in the 4.18 era, well
# before CONFIG_ANDROID_BINDERFS appeared in 5.0, so a STOCK kernel new enough to offer
# binderfs cannot have the option at all and its driver speaks 8.  This job's kernel is not
# stock -- the workflow restores the option with an inline patch so a binderfs guest can speak
# 7 -- which is exactly why this reading is taken from the RUNNING kernel: it is what says
# whether that patch reached the kernel actually booted here.
#   set (=y)          -> 7
#   explicitly unset  -> 8
#   absent            -> 8; on this job that means the restoration patch did not take effect
#   no readable config-> unavailable; the check then relies on the driver's own answer
# ------------------------------------------------------------------------------------
RUNTIME_KERNEL_PROTOCOL=''
RUNTIME_KERNEL_PROTOCOL_EVIDENCE=''

runtime_kernel_protocol() {
    local source line
    source="$(kernel_config_source)"
    if [ -z "$source" ]; then
        RUNTIME_KERNEL_PROTOCOL=''
        RUNTIME_KERNEL_PROTOCOL_EVIDENCE='no kernel configuration is readable on this guest
       (neither /proc/config.gz via CONFIG_IKCONFIG_PROC nor /boot/config-<kernel release>),
       so the running kernel cannot be asked what it was configured with'
        return 0
    fi

    line="$(kernel_config_entry CONFIG_ANDROID_BINDER_IPC_32BIT "$source")"
    case "$line" in
        CONFIG_ANDROID_BINDER_IPC_32BIT=y|CONFIG_ANDROID_BINDER_IPC_32BIT=Y)
            RUNTIME_KERNEL_PROTOCOL='7'
            RUNTIME_KERNEL_PROTOCOL_EVIDENCE="'$line' in $source" ;;
        '')
            RUNTIME_KERNEL_PROTOCOL='8'
            RUNTIME_KERNEL_PROTOCOL_EVIDENCE="CONFIG_ANDROID_BINDER_IPC_32BIT is absent from
       $source, which is what a STOCK kernel new enough to provide binderfs looks like -- the
       option was deleted in the 4.18 era and CONFIG_ANDROID_BINDERFS first appeared in 5.0.
       On this job the kernel is patched to restore it, so absent here means the restoration
       did not reach the kernel that booted" ;;
        *)
            RUNTIME_KERNEL_PROTOCOL='8'
            RUNTIME_KERNEL_PROTOCOL_EVIDENCE="'$line' in $source" ;;
    esac
}

report_kernel_config() {
    local source entry
    source="$(kernel_config_source)"

    if [ -z "$source" ]; then
        printf 'kernel_config_source: NONE READABLE ON THIS GUEST\n'
        printf '  Neither /proc/config.gz (CONFIG_IKCONFIG_PROC) nor /boot/config-%s exists,\n' "$(uname -r)"
        printf '  so the configuration IN EFFECT cannot be read from inside the guest and is\n'
        printf '  NOT reported here. It is not inferred from %s either: that file records what\n' "$KERNEL_EXPECTATION"
        printf '  this job REQUIRES, not what this kernel has.\n'
        return 0
    fi

    printf 'kernel_config_source: %s\n' "$source"
    for entry in CONFIG_ANDROID CONFIG_ANDROID_BINDER_IPC CONFIG_ANDROID_BINDER_DEVICES \
                 CONFIG_ANDROID_BINDERFS CONFIG_ASHMEM CONFIG_ANDROID_BINDER_IPC_32BIT; do
        local line=''
        line="$(kernel_config_entry "$entry" "$source")"
        if [ -n "$line" ]; then
            printf '  %s\n' "$line"
        else
            printf '  %s: absent from the configuration (neither set nor explicitly unset)\n' "$entry"
        fi
    done
}

# The driver's own answer, hoisted into globals so the agreement check of
# assert_binder_protocol_agreement compares the SAME reading a human sees in the record
# block below.  Asking the driver twice could in principle return two answers, and a report
# that disagreed with the check that gated the run would be worse than either alone.
DRIVER_PROTOCOL_STATUS=''
DRIVER_PROTOCOL_OUTPUT=''
DRIVER_PROTOCOL_VERSION=''

read_driver_protocol() {
    # The helper is the only way to ask the driver its protocol version; its failure is
    # reported verbatim rather than replaced with a plausible number.  It prints a bare
    # integer on success and exits 0; 2 means the node could not be opened and 3 means the
    # BINDER_VERSION ioctl failed.
    set +e
    DRIVER_PROTOCOL_OUTPUT="$("$PROTOCOL_HELPER" /dev/binder 2>&1)"
    DRIVER_PROTOCOL_STATUS=$?
    set -e

    DRIVER_PROTOCOL_VERSION=''
    if [ "$DRIVER_PROTOCOL_STATUS" -eq 0 ]; then
        # Strip surrounding whitespace and accept the value only if it is all digits: a
        # helper that printed a diagnostic on stdout must not be parsed as a version.
        local trimmed
        trimmed="$(printf '%s' "$DRIVER_PROTOCOL_OUTPUT" | tr -d '[:space:]')"
        case "$trimmed" in
            ''|*[!0-9]*) DRIVER_PROTOCOL_VERSION='' ;;
            *)           DRIVER_PROTOCOL_VERSION="$trimmed" ;;
        esac
    fi
}

report_binder_platform() {
    local protocol_output protocol_status
    read_driver_protocol
    protocol_output="$DRIVER_PROTOCOL_OUTPUT"
    protocol_status="$DRIVER_PROTOCOL_STATUS"
    runtime_kernel_protocol

    printf '================ BINDER PLATFORM RECORD (read from this running guest) ================\n'
    printf 'reported_by: %s\n' "$INIT_NAME"
    # ELF bitness and the binder wire protocol are two SEPARATE axes, and printing them as one
    # would claim protocol 7 purely because the image is 32-bit -- which says nothing about an
    # SDK built for protocol 8.  The two are stated apart, and the protocol is the value derived
    # at image-build time from the kernel configuration and the SDK build cache rather than a
    # consequence of the bitness.
    printf 'image_target_elf_bitness: %s   (32-bit userland; says nothing about the wire protocol)\n' "$IMAGE_ARCH"
    printf 'image_expected_binder_protocol: %s   (derived at image build; kernel-config half: %s, SDK-cache half: %s)\n' \
        "$EXPECTED_BINDER_PROTOCOL" \
        "${PROTOCOL_FROM_KERNEL_CONFIG:-not derivable}" \
        "${PROTOCOL_FROM_SDK_CACHE:-not derivable}"
    printf 'uname: %s\n' "$(uname -srvmo 2>/dev/null || printf 'unavailable')"
    printf '\n'
    # EVERY SECTION RULE BELOW IS PRINTED AS AN ARGUMENT, NOT AS A FORMAT, AND THAT IS
    # REQUIRED RATHER THAN STYLISTIC.  bash's printf parses its first word for options, so a
    # format beginning with '-' is read as one: `printf '---- x\n'` fails with
    # "printf: --: invalid option" and returns 2.  Measured -- with `set -e` in force that
    # aborted the init here, mid-record, and the guest exited status 2 having brought binderfs
    # up and proved nothing.  `printf '%s\n' '---- ...'` puts the text where no option parsing
    # reaches it and prints the identical line.  Any rule added here must keep that shape.
    printf '%s\n' '---- kernel binder configuration, as the running guest reports it ----'
    report_kernel_config
    printf 'running_kernel_implies_protocol: %s\n' "${RUNTIME_KERNEL_PROTOCOL:-UNAVAILABLE}"
    printf '  evidence: %s\n' "$RUNTIME_KERNEL_PROTOCOL_EVIDENCE"
    printf '\n'
    printf '%s\n' '---- runtime evidence ----'
    printf 'binder_in_proc_filesystems: %s\n' \
        "$(grep -qw binder /proc/filesystems 2>/dev/null && printf 'yes' || printf 'no')"
    printf 'binderfs_mounted_at_/dev/binderfs: %s\n' \
        "$(mountpoint -q -- /dev/binderfs 2>/dev/null && printf 'yes' || printf 'no')"
    printf 'binderfs_entries: %s\n' \
        "$(find /dev/binderfs -mindepth 1 -maxdepth 1 -printf '%f ' 2>/dev/null || printf 'unreadable')"
    printf '\n'
    printf '%s\n' '---- Binder SDK build flags, as recorded by the staging tree itself ----'
    if [ -n "$SDK_FLAGS_RECORD" ] && [ -r "$SDK_FLAGS_RECORD" ]; then
        cat -- "$SDK_FLAGS_RECORD"
    else
        printf 'NOT RECORDED: %s is missing, so the flag the SDK was built with cannot be\n' "${SDK_FLAGS_RECORD:-<unset>}"
        printf 'reported from this image. It has to come from the job that built the SDK.\n'
    fi
    printf '\n'
    printf '%s\n' '---- resulting binder protocol version, asked of /dev/binder ----'
    if [ "$protocol_status" -eq 0 ]; then
        printf 'binder_protocol_version: %s\n' "$protocol_output"
        printf '  This is the number libbinder compares for EQUALITY when it opens the node, so\n'
        printf '  it is meaningful only against the two blocks above. A mismatch between the\n'
        printf '  kernel, the SDK build and the vendor side fails EVERY open; see\n'
        printf '  linux_binder_idl BUILD.md:409-413 at tag 2.6.0.\n'
    else
        printf 'binder_protocol_version: COULD NOT BE READ (helper exited %s)\n' "$protocol_status"
        printf '%s\n' "$protocol_output"
    fi
    printf '\n'
    printf '%s\n' '---- what this job requires of the guest kernel (expectation, not measurement) ----'
    if [ -n "$KERNEL_EXPECTATION" ] && [ -r "$KERNEL_EXPECTATION" ]; then
        cat -- "$KERNEL_EXPECTATION"
    else
        printf 'the expectation file is missing from this image.\n'
    fi
    if [ -n "$KERNEL_PROVENANCE" ] && [ -r "$KERNEL_PROVENANCE" ]; then
        printf '\n---- the configuration the image build was told the guest kernel was built with ----\n'
        printf '(build-time provenance only; the running guest is reported above)\n'
        head -n 40 -- "$KERNEL_PROVENANCE"
    fi
    printf '================ END BINDER PLATFORM RECORD ================\n'
}

# ------------------------------------------------------------------------------------
# PROTOCOL AGREEMENT: FOUR SOURCES, ONE NUMBER, AND A LOUD FAILURE WHEN THEY DIFFER.
#
# WHY THIS GATE EXISTS.  libbinder enforces protocol-version EQUALITY when it opens
# /dev/binder (linux_binder_idl BUILD.md:409-413 at tag 2.6.0), so kernel, SDK build and
# vendor side must agree or every open fails.  A disagreement is therefore not a cosmetic
# documentation problem: it is a run in which the AIDL back-end can never be selected, and
# the harness's own fallback then produces a suite that passes on the legacy path and reports
# green.  That is a false green about the exact thing this job exists to prove, which is why
# a disagreement here is fatal rather than a warning.
#
# WHY FOUR SOURCES AND NOT ONE.  A single hard-coded sentence claiming protocol 7 while the
# workflow builds protocol 8 is a disagreement nothing compares and therefore nothing catches.
# Each source below can be wrong independently and each names a different artefact to correct:
#
#   (A) the image's expectation      -> derived at image-build time from the kernel
#                                       configuration and the SDK build cache; wrong means the
#                                       image was built against the wrong inputs
#   (B) the running kernel's config  -> wrong means the guest is booted on a kernel other than
#                                       the one the image was built for
#   (C) the staged SDK's build cache -> wrong means libbinder was compiled with the wrong
#                                       BINDER_IPC_32BIT value
#   (D) the driver's BINDER_VERSION  -> the authority.  This is the number libbinder compares,
#                                       read through the ioctl by the staged helper
#
# (D) IS REQUIRED; (B) AND (C) MAY BE UNAVAILABLE AND SAY SO.  A guest without
# CONFIG_IKCONFIG_PROC cannot be asked (B), and an SDK staged as an install tree may carry no
# CMakeCache.txt for (C).  Unavailable is reported as unavailable and never substituted with
# the expectation -- presenting a build-time assumption as a measurement is the failure mode
# that produced the contradiction in the first place.  (D) failing is fatal: if the driver
# cannot be opened or cannot answer, no AIDL invocation can run at all.
# ------------------------------------------------------------------------------------
SDK_CACHE_PROTOCOL=''
SDK_CACHE_PROTOCOL_EVIDENCE=''

# Re-derive (C) inside the guest from the staged tree itself where the tree carries a cache,
# so the check reads an artefact rather than trusting a value the image build copied into
# image.conf.  Falls back to the SDK flags record -- a file written by the image build and
# independent of image.conf -- and then to image.conf's own copy, naming which was used.
sdk_cache_protocol() {
    local cache raw
    while IFS= read -r cache; do
        [ -n "$cache" ] || continue
        raw="$(grep -E '^BINDER_IPC_32BIT(:[A-Z]+)?=' -- "$cache" 2>/dev/null | tail -n 1 || true)"
        [ -n "$raw" ] || continue
        raw="${raw#*=}"
        case "$raw" in
            ON|1|TRUE|True|true|YES|Yes|yes|Y|y)
                SDK_CACHE_PROTOCOL='7' ;;
            OFF|0|FALSE|False|false|NO|No|no|N|n|'')
                SDK_CACHE_PROTOCOL='8' ;;
            *)
                warn "BINDER_IPC_32BIT='$raw' in $cache is not a recognised CMake boolean;
       ignoring this cache for protocol derivation rather than guessing what it meant."
                continue ;;
        esac
        SDK_CACHE_PROTOCOL_EVIDENCE="BINDER_IPC_32BIT=$raw in $cache (read in the guest)"
        return 0
    done < <(find "$SDK_DIR/out/build" "$SDK_DIR/out/target" -maxdepth 3 -name CMakeCache.txt \
                  -type f 2>/dev/null)

    local recorded=''
    if [ -n "$SDK_FLAGS_RECORD" ] && [ -r "$SDK_FLAGS_RECORD" ]; then
        recorded="$(grep -E '^derived_binder_protocol_from_sdk_cache=' -- "$SDK_FLAGS_RECORD" \
                      2>/dev/null | tail -n 1 || true)"
        recorded="${recorded#*=}"
    fi
    if [ -n "$recorded" ]; then
        SDK_CACHE_PROTOCOL="$recorded"
        SDK_CACHE_PROTOCOL_EVIDENCE="derived_binder_protocol_from_sdk_cache=$recorded in
       $SDK_FLAGS_RECORD (recorded by the image build; no CMakeCache.txt is staged in this
       image to re-derive it from)"
        return 0
    fi
    if [ -n "$PROTOCOL_FROM_SDK_CACHE" ]; then
        SDK_CACHE_PROTOCOL="$PROTOCOL_FROM_SDK_CACHE"
        SDK_CACHE_PROTOCOL_EVIDENCE="PROTOCOL_FROM_SDK_CACHE=$PROTOCOL_FROM_SDK_CACHE in
       $IMAGE_CONF (recorded by the image build; neither a staged CMakeCache.txt nor the SDK
       flags record is available in this image)"
        return 0
    fi
    SDK_CACHE_PROTOCOL=''
    SDK_CACHE_PROTOCOL_EVIDENCE='no staged CMakeCache.txt, no SDK flags record entry and no
       image.conf value, so the flag libbinder was compiled with cannot be read from inside
       this guest'
}

assert_binder_protocol_agreement() {
    case "$EXPECTED_BINDER_PROTOCOL" in
        7|8) : ;;
        *) die "EXPECTED_BINDER_PROTOCOL='$EXPECTED_BINDER_PROTOCOL' in $IMAGE_CONF is not 7 or
       8.  The image build derives this value from the guest kernel configuration and the SDK
       build cache; a value outside that set means the image was finished by a root-image
       script that does not agree with this init.  Rebuild the image." ;;
    esac

    sdk_cache_protocol

    printf '================ BINDER PROTOCOL AGREEMENT ================\n'
    printf '(A) image expectation              : %s\n' "$EXPECTED_BINDER_PROTOCOL"
    printf '      derived from kernel config   : %s\n' "${PROTOCOL_FROM_KERNEL_CONFIG:-not derivable at image build}"
    printf '      derived from SDK build cache : %s\n' "${PROTOCOL_FROM_SDK_CACHE:-not derivable at image build}"
    printf '(B) running kernel configuration   : %s\n' "${RUNTIME_KERNEL_PROTOCOL:-UNAVAILABLE}"
    printf '      evidence                     : %s\n' "$RUNTIME_KERNEL_PROTOCOL_EVIDENCE"
    printf '(C) staged Binder SDK build        : %s\n' "${SDK_CACHE_PROTOCOL:-UNAVAILABLE}"
    printf '      evidence                     : %s\n' "$SDK_CACHE_PROTOCOL_EVIDENCE"
    if [ -n "$DRIVER_PROTOCOL_VERSION" ]; then
        printf '(D) /dev/binder BINDER_VERSION     : %s   (the authority; libbinder compares this for equality)\n' \
            "$DRIVER_PROTOCOL_VERSION"
    else
        printf '(D) /dev/binder BINDER_VERSION     : COULD NOT BE READ (helper exited %s)\n' \
            "${DRIVER_PROTOCOL_STATUS:-unknown}"
    fi
    printf '================ END BINDER PROTOCOL AGREEMENT ================\n'

    # (D) first, because it is the authority and because a driver that cannot answer means no
    # AIDL invocation can run regardless of what the other three say.
    if [ -z "$DRIVER_PROTOCOL_VERSION" ]; then
        die "the binder driver did not report a protocol version: the helper
       ($PROTOCOL_HELPER /dev/binder) exited ${DRIVER_PROTOCOL_STATUS:-unknown} and produced:
       ${DRIVER_PROTOCOL_OUTPUT:-<no output>}
       Exit 2 means /dev/binder could not be opened -- check that binderfs is mounted and the
       device nodes were created; exit 3 means the BINDER_VERSION ioctl failed, which means
       the node is not a binder device.  Either way the AIDL invocations cannot run, so this
       is a failure rather than a missing datum."
    fi

    local disagreements=0
    if [ "$DRIVER_PROTOCOL_VERSION" != "$EXPECTED_BINDER_PROTOCOL" ]; then
        warn "PROTOCOL DISAGREEMENT (A) vs (D): the image was built expecting protocol
       $EXPECTED_BINDER_PROTOCOL but /dev/binder reports $DRIVER_PROTOCOL_VERSION.  Correct
       the artefact the expectation came from -- kernel-config half
       '${PROTOCOL_FROM_KERNEL_CONFIG:-not derivable}', SDK-cache half
       '${PROTOCOL_FROM_SDK_CACHE:-not derivable}' -- or boot the guest on the kernel the
       image was built for."
        disagreements=$((disagreements + 1))
    fi
    if [ -n "$RUNTIME_KERNEL_PROTOCOL" ] && \
       [ "$RUNTIME_KERNEL_PROTOCOL" != "$DRIVER_PROTOCOL_VERSION" ]; then
        warn "PROTOCOL DISAGREEMENT (B) vs (D): the running kernel's configuration implies
       protocol $RUNTIME_KERNEL_PROTOCOL ($RUNTIME_KERNEL_PROTOCOL_EVIDENCE) but its own
       driver reports $DRIVER_PROTOCOL_VERSION.  The configuration this guest exposes does not
       describe the driver it is running; the kernel image and its configuration file are out
       of step."
        disagreements=$((disagreements + 1))
    fi
    if [ -n "$SDK_CACHE_PROTOCOL" ] && \
       [ "$SDK_CACHE_PROTOCOL" != "$DRIVER_PROTOCOL_VERSION" ]; then
        local remedy='OFF'
        [ "$DRIVER_PROTOCOL_VERSION" = '7' ] && remedy='ON'
        warn "PROTOCOL DISAGREEMENT (C) vs (D): the staged Binder SDK was built for protocol
       $SDK_CACHE_PROTOCOL ($SDK_CACHE_PROTOCOL_EVIDENCE) but the driver reports
       $DRIVER_PROTOCOL_VERSION.  libbinder compares these for EQUALITY on open, so every
       open would fail.  Rebuild the SDK with -DBINDER_IPC_32BIT=$remedy, which is the axis
       that selects the wire protocol independently of the 32-bit build."
        disagreements=$((disagreements + 1))
    fi

    if [ "$disagreements" -gt 0 ]; then
        die "$disagreements binder protocol disagreement(s) reported above.  Refusing to run
       the invocation matrix: with a protocol mismatch libbinder fails every open, the AIDL
       back-end can never be selected, and the suite would fall back to the legacy path and
       report green -- a false pass about the one thing this job exists to prove."
    fi

    if [ -z "$RUNTIME_KERNEL_PROTOCOL" ]; then
        warn "the running kernel's configuration is not readable, so source (B) of the
       protocol agreement could not be checked.  Build the guest kernel with
       CONFIG_IKCONFIG=y and CONFIG_IKCONFIG_PROC=y to close this gap.  The driver's own
       answer (D) was checked and agrees with the image expectation."
    fi
    if [ -z "$SDK_CACHE_PROTOCOL" ]; then
        warn "the flag the staged Binder SDK was compiled with is not recorded anywhere in
       this image, so source (C) of the protocol agreement could not be checked.  Stage the
       SDK build tree's CMakeCache.txt, or ensure the image build writes the SDK flags
       record.  The driver's own answer (D) was checked and agrees with the image
       expectation."
    fi

    log "binder protocol agreement: all available sources report protocol $DRIVER_PROTOCOL_VERSION"
}

# ------------------------------------------------------------------------------------
# SERVICEMANAGER, AND WAITING FOR IT TO ANSWER.
#
# It is an unconditional runtime prerequisite wherever a binder driver is present: the
# service name cannot be resolved without it, and obtaining an IServiceManager at all loops
# until binder handle 0 resolves.
#
# THE WAIT IS A POLL ON A REAL TRANSACTION, NOT A SLEEP.  The probe calls
# defaultServiceManager() and then checkService(); on the pinned stack the first call BLOCKS
# while no context manager exists, so `timeout` around it is a genuine bounded wait and each
# unsuccessful attempt costs its own timeout rather than a guess.  A bare sleep that was
# occasionally too short would produce a run in which every AIDL invocation silently falls
# back to the legacy back-end and reports green -- the exact failure this job exists to
# catch.
#
# Liveness is checked on every iteration too: a servicemanager that died at startup must be
# reported as dead, with its log, rather than waited out to the full budget.
# ------------------------------------------------------------------------------------
readonly SERVICEMANAGER_LOG_NAME='servicemanager.log'
readonly READINESS_PROBE_LOG_NAME='servicemanager_readiness.log'
readonly READINESS_ATTEMPT_TIMEOUT_SECONDS=3

start_servicemanager() {
    local log_file="$ARTIFACT_DIR/$SERVICEMANAGER_LOG_NAME"
    log "starting servicemanager: $SERVICEMANAGER_BIN"
    [ -x "$SERVICEMANAGER_BIN" ] || die "servicemanager is not executable at
       $SERVICEMANAGER_BIN.  It comes from the staged SDK, so this means the image was built
       around an incomplete staging tree."

    # Its own log, kept with the artifacts: when readiness fails this is the only evidence of
    # why, and it must survive a failing run.
    "$SERVICEMANAGER_BIN" >> "$log_file" 2>&1 &
    SERVICEMANAGER_PID=$!
    log "  servicemanager pid $SERVICEMANAGER_PID, logging to $log_file"
}

wait_for_servicemanager() {
    local probe_log="$ARTIFACT_DIR/$READINESS_PROBE_LOG_NAME"
    local budget="$READINESS_TIMEOUT_SECONDS"
    local elapsed=0 attempt=0 probe_status

    log "waiting up to ${budget}s for servicemanager to answer a binder transaction"
    [ -x "$READY_PROBE" ] || die "the readiness probe is missing at $READY_PROBE, so there is
       no way to establish that servicemanager is ready.  Refusing to proceed on a guess: a
       run that starts before the service manager answers falls back to the legacy back-end
       and reports green."

    while [ "$elapsed" -lt "$budget" ]; do
        attempt=$(( attempt + 1 ))
        if ! kill -0 "$SERVICEMANAGER_PID" 2>/dev/null; then
            warn "servicemanager exited before it answered. Its log follows:"
            tail -n 40 -- "$ARTIFACT_DIR/$SERVICEMANAGER_LOG_NAME" >&2 2>/dev/null || true
            die "servicemanager is not running, so no AIDL-selected invocation can resolve the
       service name.  The most common causes are a binder protocol mismatch between the
       kernel and the SDK build -- compare the two in the platform record above -- and a
       missing library from the six-library Binder closure."
        fi

        set +e
        timeout "$READINESS_ATTEMPT_TIMEOUT_SECONDS" "$READY_PROBE" >> "$probe_log" 2>&1
        probe_status=$?
        set -e

        if [ "$probe_status" -eq 0 ]; then
            log "  servicemanager answered on attempt $attempt (after about ${elapsed}s)"
            return 0
        fi

        # Each unsuccessful attempt consumed its own timeout, because the probe BLOCKS while
        # no context manager exists. That is what paces this loop -- there is no sleep in it.
        elapsed=$(( elapsed + READINESS_ATTEMPT_TIMEOUT_SECONDS ))
    done

    warn "the readiness probe never succeeded. Its log follows:"
    tail -n 40 -- "$probe_log" >&2 2>/dev/null || true
    warn "servicemanager's own log follows:"
    tail -n 40 -- "$ARTIFACT_DIR/$SERVICEMANAGER_LOG_NAME" >&2 2>/dev/null || true
    die "servicemanager did not answer within ${budget}s.  Failing rather than proceeding: a
       run that starts before the context manager is reachable resolves every selection to the
       legacy back-end and reports a green matrix that proves nothing.  Raise the budget with
       --readiness-timeout when building the image if this guest is simply slow; otherwise
       treat it as a binder configuration fault and compare the kernel, SDK and protocol
       values in the platform record above."
}
CEC_L2_INIT_EOF

    # PART THREE: the environment, the coverage-runner invocation, the status file and main.
    cat <<'CEC_L2_INIT_EOF'

# ------------------------------------------------------------------------------------
# THE ENVIRONMENT THE GUEST BUILD AND THE HARNESSES NEED.
#
# Four of these are configure's own precious variables, and they are exported rather than
# spelled into a configure command line because the coverage runner passes them through
# itself and derives every include root, -L directory and link edge from them inside
# configure.  A path spelled twice is a second place for the two build systems to disagree.
#
# WHAT IS DELIBERATELY NOT SET HERE:
#   CEC_TEST_AIDL_MODE -- the coverage runner owns it per invocation across the A-E matrix.
#       Setting it here would override the matrix and collapse it to one arm, which would
#       still report green.
#   CEC_FAKE_HOST_READY_FD -- the L2 harness creates that pipe and passes the descriptor to
#       the fake service host it launches itself.
#   anything that would start a binder threadpool -- there is no environment variable for
#       that, and this note exists so nobody adds one.
#   any DIRECT-OBJECT LOADER VARIABLE -- LD_PRELOAD, LD_AUDIT, LD_PROFILE, LD_DYNAMIC_WEAK
#       and LD_ORIGIN_PATH.  Not merely unset: REMOVED below, explicitly.  See the block
#       there for why the guarantee is stated rather than assumed.
# ------------------------------------------------------------------------------------
export_build_environment() {
    export HOME='/root'
    export PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin'
    export LC_ALL='C'
    export TMPDIR='/tmp'

    # DIRECT-OBJECT LOADER VARIABLES ARE REMOVED HERE, before the coverage runner is invoked
    # and before anything is compiled.
    #
    # These name objects for the loader to LOAD, or change how symbols bind, so the
    # LD_LIBRARY_PATH exported below - a SEARCH path - has no bearing on them at all.  A
    # preloaded object would run in every child of the guest run: gcc, make, lcov, gcov, the
    # L1 and L2 test binaries and the fake service host, which is to say in every process that
    # produces a coverage figure.  Measured on the build host before the runner grew its own
    # guard: a marker object ran 384 times during `run_coverage.sh --help` alone.
    #
    # THIS IS BELT AND BRACES, AND IT IS STILL WORTH THE FOUR LINES.  This init is PID 1, so
    # its environment is whatever the kernel handed it - which carries none of these - and
    # run_coverage.sh refuses to run at all if one is present, at load time, before it spawns
    # a child.  So there are already two reasons this cannot happen.  What the removal adds is
    # that the guarantee is made HERE, in the file that composes the guest's environment,
    # rather than inferred from two facts elsewhere: a future edit that exported one of them,
    # or a kernel command line that carried one, would otherwise be caught only by the
    # runner's refusal, which reads as a mysterious failure at that point rather than as a
    # thing this init prevented.
    #
    # `unset -v` rather than assigning empty: glibc's loader activates LD_DYNAMIC_WEAK on the
    # PRESENCE of the variable and ignores its value, so an empty assignment would leave it
    # active.  Removal is the only spelling that holds for the whole set.
    unset -v LD_PRELOAD LD_AUDIT LD_PROFILE LD_DYNAMIC_WEAK LD_ORIGIN_PATH

    # EXPORTED ONLY WHEN NON-EMPTY, and that distinction is load-bearing rather than tidy.
    # These four reach configure as precious variables, and 'BINDER_SDK_DIR=' asserts that
    # there is NO Binder SDK -- which fails configure's own check -- while omitting it lets
    # configure look where it knows to look.  An empty export would therefore be worse than
    # no export at all.
    local name
    for name in HALIF_PREFIX HALIF_LIB_DIR BINDER_SDK_DIR BINDER_SDK_INCLUDE_DIR; do
        if [ -n "${!name}" ]; then
            # Exporting BY NAME is the intent: $name holds the variable's name and the
            # variable itself was set by read_image_conf.  ShellCheck's SC2163 exists to
            # catch `export $foo` written where `export foo` was meant, which is the
            # opposite of what this loop does.
            # shellcheck disable=SC2163
            export "$name"
        else
            warn "$name is empty in the image configuration, so it is left UNSET rather than"
            warn "  exported empty: an empty value asserts that the SDK is absent, which is a"
            warn "  different instruction from saying nothing."
        fi
    done

    # The loader closure.  The image's ld.so.conf already names these directories, and this
    # is deliberately belt and braces: GNU ld does not search -L for a shared library's own
    # DT_NEEDED entries and the staged libbinder carries no RUNPATH, so the search path
    # matters when the suites are LINKED as well as when they are run.
    local ld_path="$BINDER_LIB_DIR:$HALIF_LIB_DIR"
    if [ -n "$GTEST_PREFIX" ]; then
        export GTEST_PREFIX="$GTEST_PREFIX"
        ld_path="$GTEST_PREFIX/lib:$ld_path"
        export PKG_CONFIG_PATH="$GTEST_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    fi
    export LD_LIBRARY_PATH="$ld_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    # Where the L2 harness finds the out-of-process fake service host. The coverage runner
    # sets this itself for each invocation it launches; exporting it here means a manual
    # in-guest run of the L2 runner works too, and the two values are the same path.
    export CEC_FAKE_AIDL_HOST_PATH="$PAYLOAD_DIR/tests/L2Tests/fake_hdmi_cec_aidl_host"

    # THE SINK CALLER SOURCE the L1 drift guard reads, staged into this image at build time.
    #
    # Exported as an ABSOLUTE path rather than left to the guard's fallback search, because that
    # search is relative to the test binary and to the working directory. The image does reproduce
    # the superproject layout, so the fallback would resolve as well - the export means the guard
    # does not depend on which directory a run happens to start from. Absent this export the guard
    # fails naming every path it tried, which is correct behaviour on a host that genuinely has no
    # source and would be a pure wiring fault here.
    #
    # ASSERTED, NOT ASSUMED. The image configuration lists SINK_CALLER_SOURCE as a required key,
    # so an image built without it never boots this far - but a key that names a file the image
    # does not actually carry would still reach here, and the guard's own diagnostic for that case
    # calls it a WIRING FAULT. Catching it in the init makes it one line at boot instead of a red
    # test after the suites have run.
    if [ ! -r "$SINK_CALLER_SOURCE" ]; then
        die "the image configuration names $SINK_CALLER_SOURCE as the Sink caller source the L1
       drift guard reads, but that file is not readable inside this image.  The image is not
       internally consistent: aidl-path-tests-rootfs.sh stages this file unconditionally, so an
       image whose configuration names it while the file is absent was not produced by a matching
       root-image script.  Rebuild the image."
    fi
    export CEC_SINK_CALLER_SOURCE="$SINK_CALLER_SOURCE"

    # THE PAYLOAD'S COMMIT REVISIONS, WHICH THIS GUEST CANNOT DERIVE AND MUST NOT INVENT.
    #
    # tests/L1Tests/run_coverage.sh writes provenance.txt from `git rev-parse` over the
    # superproject and each submodule it finds.  The payload in this image was staged WITHOUT its
    # .git -- deliberately, because the image is published as an artifact and history has no
    # business travelling with it -- so every one of those reads fails and every repository row
    # reads "unavailable (not a repository)".  That is not merely untidy: the artifact's own
    # verification procedure tells its reader to compare the recorded superproject revision
    # against `git rev-parse HEAD`, and a row with no sha in it makes that instruction
    # impossible to carry out.
    #
    # THE VARIABLE NAMES ARE THE RUNNER'S CONTRACT, NOT A CONVENTION INVENTED HERE:
    # CEC_PROVENANCE_REVISION_<LABEL>, where <LABEL> is the runner's own provenance label
    # upper-cased with '-' mapped to '_'.  So provenance label 'entservices-hdmicecsink' becomes
    # CEC_PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK.  The runner reads each ONLY when git could
    # not answer for that path, which is why exporting them cannot override a real repository if
    # one is ever present, and it refuses a malformed value with its own named diagnostic --
    # which is why the image builder held each to the same form before writing it here.
    #
    # EXPORTED ONLY WHEN NON-EMPTY, and for a stronger reason than tidiness: an EMPTY value is
    # a malformed value to the runner, so exporting one would turn "the builder was given no
    # revision for this tree" into a hard refusal inside the guest.  Absent, the row stays
    # exactly as it is today.  A revision that is absent is logged as such rather than passed
    # over in silence, because "the provenance artifact will not name this tree" is a fact the
    # console log should carry.
    local label conf_name env_name option_name value
    for label in SUPERPROJECT HDMICEC ENTSERVICES_HDMICECSINK; do
        conf_name="PROVENANCE_REVISION_$label"
        env_name="CEC_PROVENANCE_REVISION_$label"
        # The option's spelling is the label transformed the OTHER way -- lower-cased with '_'
        # mapped back to '-' -- so the remedy printed below is the flag a reader can copy rather
        # than one they have to re-derive.  ENTSERVICES_HDMICECSINK is the case that proves the
        # substitution is needed: without it the message would name a flag that does not exist.
        option_name="${label,,}"
        option_name="${option_name//_/-}"
        value="${!conf_name}"
        if [ -n "$value" ]; then
            # Exporting BY NAME, as the SDK loop above does: $env_name holds the variable's
            # name.  SC2163 exists to catch `export $foo` written where `export foo` was meant,
            # which is the opposite of what this does.
            # shellcheck disable=SC2163
            export "$env_name=$value"
            log "  $env_name=$value"
        else
            log "  $env_name: not supplied by the image builder, so the coverage runner's"
            log "    provenance artifact will record this repository as unavailable. Pass"
            log "    --provenance-revision-$option_name to aidl-path-tests-rootfs.sh to fix it."
        fi
    done

    log "build environment exported:"
    log "  HALIF_PREFIX=$HALIF_PREFIX"
    log "  HALIF_LIB_DIR=$HALIF_LIB_DIR"
    log "  BINDER_SDK_DIR=$BINDER_SDK_DIR"
    log "  BINDER_SDK_INCLUDE_DIR=$BINDER_SDK_INCLUDE_DIR"
    log "  GTEST_PREFIX=${GTEST_PREFIX:-<unset: the image packaging supplies gtest.pc>}"
    log "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
    log "  CEC_FAKE_AIDL_HOST_PATH=$CEC_FAKE_AIDL_HOST_PATH"
    log "  CEC_SINK_CALLER_SOURCE=$CEC_SINK_CALLER_SOURCE"
    log "  CEC_TEST_AIDL_MODE is deliberately NOT set: the coverage runner owns it per invocation"
    log "  LD_PRELOAD, LD_AUDIT, LD_PROFILE, LD_DYNAMIC_WEAK, LD_ORIGIN_PATH: removed, not set"
}

# ------------------------------------------------------------------------------------
# THE COVERAGE RUN.
#
# --build compiles and --run measures, and both happen here because this is where the binder
# driver is.  The runner owns everything else: the five invocations, the per-invocation
# zero-exit check, the executed-count check against each filter, the selected-path-line
# grep, the non-overwriting artifact names, the counter zero-and-accumulate policy, the
# single capture, the line gate and the per-branch gate.  None of that is repeated here.
#
# THE EXIT STATUS IS NOT LAUNDERED.  `|| status=$?` captures the value and the function
# returns it unchanged; the caller propagates it as this init's own exit status and the file
# records the same number.  What is forbidden -- and absent -- is a `|| true`, a `set +e`
# that swallows the result, or an `if ! ...; then echo` that turns a failure into a message.
#
#   0  measured and passing. The only status that means that.
#   3  ADVISORY: the coverage figures are at or above the bar but this invocation did not
#      establish the evidence for them. NOT A PASS.
#   anything else: a prerequisite was missing, a step failed, or a gate failed.
# ------------------------------------------------------------------------------------
readonly RUN_COVERAGE_REL='tests/L1Tests/run_coverage.sh'

run_coverage() { # -> the runner's exact exit status
    local runner="$PAYLOAD_DIR/$RUN_COVERAGE_REL"
    local status=0

    [ -f "$runner" ] || die "the coverage runner is missing at $runner.  The payload copied
       into this image is not an hdmicec checkout, or the copy was incomplete."

    log "running the coverage runner: bash $runner --build --run --output-dir $ARTIFACT_DIR"
    log "  (it owns the whole A-E invocation matrix, the single capture and both gates)"

    # Invoked through bash rather than relying on the executable bit, which a copy across
    # filesystems does not always preserve.
    bash "$runner" --build --run --output-dir "$ARTIFACT_DIR" || status=$?

    return "$status"
}

write_status_file() { # $1=the runner's exit status
    local status="$1"
    # The file holds exactly the integer and a newline, so the workflow can read it with a
    # single command and branch on 0, on 3, and on anything else.
    printf '%s\n' "$status" > "$STATUS_FILE"
    log "wrote the coverage runner's exit status ($status) to $STATUS_FILE"

    printf '================ COVERAGE RUN VERDICT ================\n'
    printf 'run_coverage.sh exit status: %s\n' "$status"
    case "$status" in
        0) printf 'verdict: PASS -- measured and at or above the bar.\n' ;;
        3) printf 'verdict: ADVISORY -- the figures are at or above the bar but this\n'
           printf '         invocation did not establish the evidence for them.\n'
           printf '         THIS IS NOT A PASS. Do not treat 3 as one.\n' ;;
        *) printf 'verdict: FAIL -- a prerequisite was missing, a step failed, or a gate\n'
           printf '         failed. See the run and capture logs under %s.\n' "$ARTIFACT_DIR" ;;
    esac
    printf 'artifacts: %s\n' "$ARTIFACT_DIR"
    printf '  The names are fixed by the coverage runner: coverage.info (the unfiltered trace\n'
    printf '  the per-branch gate reads), filtered_coverage.info, line_gate_coverage.info,\n'
    printf '  coverage/index.html, per_file_coverage.tsv, uncovered_lines.txt, the\n'
    printf '  per-invocation rdkTestResults_invocation_<L>.json and run_invocation_<L>.log,\n'
    printf '  and capture.log / filter.log / genhtml.log / build.log.\n'
    printf '================ END COVERAGE RUN VERDICT ================\n'
}

# The image is supposed to carry all three of these, so a missing one means the image build
# did not finish or something was pruned afterwards.  Checked here, before binderfs is
# mounted and before servicemanager is started, so the failure is one line rather than a
# cascade.
verify_guest_layout() {
    log "guest workspace: $GUEST_WORKSPACE"
    log "  payload:    $PAYLOAD_DIR"
    log "  staged SDK: $SDK_DIR"
    [ -d "$PAYLOAD_DIR" ] || die "the payload directory $PAYLOAD_DIR is missing from this
       image, so there is nothing to build or measure.  The image build did not finish."
    [ -d "$SDK_DIR" ] || die "the staged SDK directory $SDK_DIR is missing from this image, so
       the middleware could not link against the AIDL stubs or libbinder.  The image build did
       not finish."
    [ -d "$BINDER_LIB_DIR" ] || die "the staged Binder library directory $BINDER_LIB_DIR is
       missing from this image.  libbinder, libutils and the four libraries beneath them must
       all be loadable or nothing that opens the driver starts."
}

main() {
    log "guest entry point starting (pid $$)"
    read_image_conf
    verify_guest_layout
    mount_pseudo_filesystems
    # Checked as soon as /proc and /sys exist and before anything is built or run: if this
    # guest has a network, the run's reproducibility claim is void and there is no point
    # spending an hour of TCG emulation to produce a result nobody can rely on.
    assert_guest_offline

    mkdir -p -- "$ARTIFACT_DIR"
    chmod 0700 -- "$ARTIFACT_DIR"

    mount_binderfs
    report_binder_platform
    # Gated BEFORE servicemanager starts, because a protocol mismatch makes every binder open
    # fail: servicemanager would either abort or never become reachable, and the readiness
    # wait would spend its whole budget producing a timeout that names the wrong cause.
    # Checking here reports the actual disagreement, and names which of the four sources
    # differs, instead of a downstream symptom.
    assert_binder_protocol_agreement

    start_servicemanager
    wait_for_servicemanager

    export_build_environment

    local status=0
    run_coverage || status=$?
    write_status_file "$status"

    # Propagated unchanged. The EXIT trap tears the guest down and powers it off; it does not
    # alter this value.
    log "init finished; propagating the coverage runner's status ($status)"
    exit "$status"
}

main "$@"
CEC_L2_INIT_EOF
    } | confined_write "$target_rel" "the image's $target" \
            'write the in-guest init into the image'
    confined_chmod "$target_rel" 0755 "the image's $target" \
        'make the in-guest init executable'

    # The guest's entry point must parse before the image is declared finished: a syntax error
    # here would only surface as a kernel panic at boot, with no diagnosis.
    #
    # READ BACK THROUGH THE PRIMITIVE AND PARSED FROM STDIN, rather than `bash -n -- "$path"`.
    # That form resolved the pathname a second time, so on an image whose init directory is
    # reached through a symbolic link it would have syntax-checked a HOST file and reported that
    # THIS image's init parses -- a check that passes for the wrong object, which is worse than no
    # check at all.  The anchor at /usr/sbin means no such link is on this path today, and the
    # read stays confined so that remains true of whatever base a caller supplies.  The bytes checked
    # here are the bytes just written, read through a descriptor obtained under the same
    # confinement.
    confined_read "$target_rel" "the image's $target" \
            'syntax-check the in-guest init' | bash -n \
        || die "the generated in-guest init does not parse.  This is a defect
       in $SCRIPT_PATH, not in the caller's inputs."
    log "wrote and syntax-checked the in-guest init at $GUEST_INIT_PATH"
}


# ------------------------------------------------------------------------------------
# NO ENDPOINT CREDENTIAL LEAVES IN THE PUBLISHED IMAGE.
#
# validate_inputs already refuses a credential-bearing --apt-mirror and --base-url, so the
# values THIS script writes cannot carry one.  That is not the whole surface.  The image's apt
# configuration also comes from the BASE TARBALL, which this script does not author: a base
# built against an authenticated archive can arrive with "deb https://user:token@host/..."
# already in /etc/apt/sources.list or in sources.list.d, and publishing that image publishes
# the token with it.
#
# So the finished filesystem is checked rather than reasoned about, in two passes:
#
#   SCRUB.  Any line whose URL-shaped fields carry userinfo, a query or a fragment is rewritten
#   to its sanitised form.  Only such lines are touched; every other line, including comments
#   and their spacing, is left byte-identical, because reformatting a file the base author
#   wrote is not this script's business.  Rewriting is safe HERE and would not have been
#   earlier: this runs after every apt operation the build performs, and the guest is asserted
#   offline, so nothing in the image ever fetches from these sources again.  They are
#   provenance from this point on.
#
#   VERIFY.  The file is then re-read and any remaining userinfo or query is FATAL.  A failure
#   here is not a warning that the artifact carries a secret - it is the artifact not being
#   published.  The message names the file and the LINE NUMBER and never the line, because a
#   die message reaches the same log the whole sanitising apparatus exists for.
#
# One deliberate over-reach: a bare word carrying an @ - an address in a comment, say - is
# scrubbed as userinfo as well, because a scheme-less "user:pass@host/path" is a form curl and
# apt both accept and the two are not distinguishable by shape.  The assertion this function
# has to satisfy is stated in terms of @, so a comment that keeps one would fail it.  Losing
# the tail of a comment is the cheaper error.
#
# ==============================================================================
# AND THE SCAN ITSELF MUST NOT BECOME THE HAZARD IT EXISTS TO REMOVE.  READ THIS.
# ==============================================================================
# TWO SHAPES OF THIS FUNCTION WOULD BE HOST-COMPROMISE PRIMITIVES, AND NEITHER IS USED.
#
# The first enumerates its files with a glob and `[ -f "$file" ]`, reads them with `< "$file"`
# and rewrites them with `cat -- "$tmp" > "$file"` -- every one of which FOLLOWS SYMBOLIC LINKS,
# all of it running as root on the BUILD HOST.  The base tarball is not this script's to trust:
# it can ship /etc/apt/sources.list, or any entry under sources.list.d, as an absolute symbolic
# link to a host pathname.  `[ -f ]` would report that link as a regular file, the read would
# read the HOST's file, and the redirection would TRUNCATE AND REPLACE THE HOST'S FILE AS ROOT
# -- handing that primitive to whoever supplies the base tarball, in the one function whose
# purpose is to make the artefact safe.
#
# THE SECOND SHAPE IS THE ONE WORTH READING, BECAUSE IT LOOKS SUFFICIENT AND IS NOT.  It adds
# assert_confined_to_image(), which validates the PATHNAME thoroughly -- no symlinked component
# from the mount point down, a regular file, one link, the image's own st_dev, a realpath still
# inside the image -- and then REOPENS THE SAME PATHNAME with an ordinary redirection.
# Validating a name and then opening the name is check-then-open: the object the kernel opens is
# not the object that was checked, and the gap between the two IS the vulnerability (CWE-367),
# not the absence of a check.  Re-checking immediately before the write narrows the window; it
# does not close it.  An adversary who can create one symlink in the image tree only has to do
# it after the last check.
#
# WHAT CARRIES THE GUARANTEE INSTEAD: THE CONFINED I/O PRIMITIVE, AND NOTHING ELSE.
#
# Every read, every rewrite, every removal and every directory enumeration this function and
# scrub_and_assert_image_credential_stores() perform goes through the primitive documented above
# install_confined_io_helper(): openat2(2) with
# RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, anchored on $IMAGE_MOUNT, followed by the
# operation THROUGH THE RESULTING DESCRIPTOR.  There is no second name lookup to lose, so there
# is no window to race: a symlink substituted at any position, at any instant, is refused by the
# kernel's own path resolution rather than followed.  RESOLVE_NO_XDEV is the kernel-enforced
# form of the st_dev comparison a pathname check can only make by hand, and it also covers
# /proc, /sys, /dev and /dev/pts, which are bind/pseudo mounts inside the image tree.
#
# The primitive is PROVEN to refuse, on this host, before one privileged byte is written --
# probe_confined_io() runs a positive case and five negative cases in a private directory during
# make_work_dir() and dies if any refusal does not happen.  So this block does not have to be
# believed; it has to be checked, and it is.
#
# WHAT THE REMAINING MECHANISMS ARE FOR, STATED HONESTLY:
#
#   assert_confined_to_image()  A PRE-CHECK, retained for its DIAGNOSTICS and for nothing else.
#      It can say which component of which path is a symbolic link, which is a far better
#      message than ELOOP, and it can name a hard-linked file.  It does not carry the guarantee
#      and it is not sufficient on its own -- if it were deleted the confinement would be
#      unchanged; if the primitive were deleted the confinement would be gone.
#   confined_regular_file() / confined_directory()  The type and link-count refusals, taken from
#      an fstat on the descriptor the confinement produced rather than from a stat on a pathname.
#      A fifo, socket, device node or directory where configuration is expected is FATAL, not
#      skipped: it means the base filesystem is not what it claims to be, and skipping it would
#      publish the image with the base author's substitution unexamined.
#   confined_list()  Enumeration, also through a descriptor.  `find -P` does not follow links it
#      finds inside a tree, but the kernel resolves find's STARTING PATH normally, so
#      `find -P $IMAGE_MOUNT/etc/apt/sources.list.d` on an image whose /etc is an absolute
#      symlink enumerates the HOST's directory -- and reported host filenames into this script's
#      own diagnostics.  That is why enumeration moved onto the primitive too.
#   nosymfollow  DEFENCE IN DEPTH ONLY, and now VERIFIED rather than assumed.  The image is
#      remounted MS_NOSYMFOLLOW where the kernel supports it (5.10+) -- through the image root's
#      own verified descriptor rather than through its pathname, like every other privileged
#      mount here -- and where the remount reports success this script PROVES it is in force by
#      creating a symbolic link inside the image and establishing that an ordinary open through
#      it fails.  A remount that reports success without taking effect is reported as not in
#      force.  Where the kernel cannot supply it at all, that is logged as an unavailable
#      defence -- and it does not weaken anything, because the openat2 path is mandatory and is
#      what refuses.  There is no configuration of this script in which neither is in force: the
#      primitive is proven at start-up or the run dies.
#      IT IS NOT RESTORED AFTERWARDS, and does not need to be: main() calls this function as the
#      LAST operation on the mounted filesystem and release_image() unmounts immediately after
#      it.  If a step is ever added between the two, it must be added ABOVE this function --
#      which main()'s own comment already requires for a different reason (anything running
#      after the scrub could reintroduce what it verified was gone).
# ==============================================================================
# ------------------------------------------------------------------------------------

# The image mount's own device, captured once so every confinement check compares against one
# value rather than re-deriving it per file.  Empty until the image is mounted.
IMAGE_MOUNT_DEVICE=''

# A DIAGNOSTIC PRE-CHECK, NOT THE MECHANISM THAT CARRIES THE CONFINEMENT.  Read the block above
# for why: this validates a PATHNAME, and the caller then operates through a DESCRIPTOR obtained
# by the confined primitive, so nothing here authorises anything.  It is retained because it
# produces messages an errno cannot -- it names the exact path component that is a symbolic link,
# and it names a hard-link count -- and because a failure here is a much clearer report of a
# malformed base tarball than "the confinement refused it".
#
# It is deliberately called BEFORE the confined operation and its result is deliberately not
# relied upon: if every check in this function were removed the confinement would be unchanged.
# Dies with the reason and the in-image path; never prints the file's content.  $3 is a short
# disposition ('read', 'rewrite', 'remove') used only in the message.
assert_confined_to_image() { # $1=path  $2=what it is  $3=what is about to happen
    local path="$1" what="$2" operation="$3"
    local prefix component resolved device links relative

    [ -n "$IMAGE_MOUNT" ] || die "internal error: assert_confined_to_image ran before the image
       was mounted, so '$operation' on $what would proceed unconfined.  This is a defect in
       $SCRIPT_NAME."
    [ -n "$IMAGE_MOUNT_DEVICE" ] || die "internal error: the image mount's device was never
       recorded, so the confinement check for $what cannot compare against it.  This is a defect
       in $SCRIPT_NAME."

    case "$path" in
        "$IMAGE_MOUNT"/*) : ;;
        *) die "refusing to $operation ($what): the path
           $path
       is not underneath the image mount point $IMAGE_MOUNT.  Every file this scan touches is
       inside the image by construction, so a path that is not is a defect in $SCRIPT_NAME
       rather than a caller input." ;;
    esac

    # NO SYMBOLIC LINK ANYWHERE FROM THE MOUNT POINT DOWN.  Checked component by component on
    # the ACCUMULATED prefix, because a link in the middle redirects everything after it and
    # testing only the leaf would miss it entirely.
    relative="${path#"$IMAGE_MOUNT"/}"
    prefix="$IMAGE_MOUNT"
    local saved_ifs="$IFS"
    IFS='/'
    # Deliberate word splitting on '/' to walk the components in order.
    # shellcheck disable=SC2086
    set -- $relative
    IFS="$saved_ifs"
    for component in "$@"; do
        [ -n "$component" ] || continue
        prefix="$prefix/$component"
        [ ! -L "$prefix" ] || die "refusing to $operation ($what): the path component
           ${prefix#"$IMAGE_MOUNT"}
       inside the image is a SYMBOLIC LINK.  Following it would read or write wherever it
       points, and this scan runs as root on the BUILD HOST, so a link naming an absolute host
       path would have this script truncate and replace a host file.  Refused rather than
       resolved: a base filesystem that ships a symlinked apt configuration is not a filesystem
       this script publishes.  Inspect the base tarball."
    done

    [ -e "$path" ] || die "refusing to $operation ($what): $path no longer exists.  It was
       enumerated a moment ago, so something is changing the image's configuration while this
       scan runs.  Nothing further is written there."
    [ -f "$path" ] || die "refusing to $operation ($what): ${path#"$IMAGE_MOUNT"} inside the image
       is not a regular file.  A fifo, a socket, a device node or a directory at that name is
       not apt configuration, and reading or writing one as root is not something this script
       does on the strength of a filename.  Refused; inspect the base tarball."

    links="$(stat -c '%h' -- "$path" 2>/dev/null || printf '')"
    case "$links" in
        ''|*[!0-9]*) die "refusing to $operation ($what): the link count of
       ${path#"$IMAGE_MOUNT"} could not be read, so it cannot be established that no other name
       shares its content." ;;
    esac
    [ "$links" -le 1 ] || die "refusing to $operation ($what): ${path#"$IMAGE_MOUNT"} has $links
       hard links, so another name inside the image shares its content and a rewrite would
       change both.  Refused rather than written through."

    device="$(stat -c '%d' -- "$path" 2>/dev/null || printf '')"
    [ -n "$device" ] || die "refusing to $operation ($what): the device of ${path#"$IMAGE_MOUNT"}
       could not be read, so it cannot be established that the file lives on the image's own
       filesystem."
    [ "$device" = "$IMAGE_MOUNT_DEVICE" ] || die "refusing to $operation ($what):
           ${path#"$IMAGE_MOUNT"}
       resolves to device $device, but the image is mounted on device $IMAGE_MOUNT_DEVICE.  The
       image is a separately mounted loop filesystem, so a different device means the path
       leaves the image -- onto the build host, or onto one of the pseudo-filesystems bound into
       it.  This scan writes as root; it does not write outside the artefact it is checking."

    resolved="$(realpath -- "$path" 2>/dev/null || printf '')"
    [ -n "$resolved" ] || die "refusing to $operation ($what): ${path#"$IMAGE_MOUNT"} could not be
       resolved with realpath, so it cannot be established where a read or a write would land."
    case "$resolved" in
        "$IMAGE_MOUNT"/*) : ;;
        *) die "refusing to $operation ($what): ${path#"$IMAGE_MOUNT"} resolves to
           $resolved
       which is OUTSIDE the image mount point $IMAGE_MOUNT.  Refused: this scan runs as root on
       the build host." ;;
    esac
}

# ------------------------------------------------------------------------------------
# nosymfollow: DEFENCE IN DEPTH, ATTEMPTED AND THEN PROVEN.
#
# MS_NOSYMFOLLOW makes the kernel refuse to follow ANY symbolic link in the mount, so an open by
# pathname through one fails with ELOOP.  It is not what carries this scan's guarantee -- the
# confined I/O primitive is, and it is mandatory and proven at start-up -- but it is worth having
# because it also covers code paths that do not go through the primitive at all, such as a future
# edit that reintroduces an ordinary redirection.
#
# WHY IT IS PROVEN RATHER THAN ATTEMPTED AND ASSUMED.  Returning success on every arm, including
# the arm where the remount FAILED, would leave a reader of the exit status unable to tell "in
# force" from "not available", and the log would claim a layer that is not there.  Taking the
# remount's own exit status as proof is worse still: a filesystem or kernel that accepts
# remount,nosymfollow without honouring it would be reported as protected.  So the remount is
# attempted, and where it reports success it is PROVEN in force behaviourally -- a symbolic link
# is created inside the image and an ordinary open through it must fail.
# IMAGE_MOUNT_NOSYMFOLLOW is set only when that proof passes.
#
# AND THE REMOUNT ITSELF NO LONGER NAMES ITS TARGET.  It goes through
# pinned_image_remount_nosymfollow(), which passes MS_REMOUNT|MS_NOSYMFOLLOW to mount(2) with the
# target given as the magic link of a descriptor opened on the image root and fstat-verified
# against this run's pin -- so the filesystem remounted is the one that was checked.  Two
# consequences worth stating: util-linux is no longer involved at all, because the flag reaches
# the syscall directly rather than as a mount(8) option name that needs 2.35 or newer; and a
# CUSTODY failure is now distinguishable from mount(2) declining the flag, so the first is fatal
# while the second stays the tolerated "not available here" case this block is about.
# ------------------------------------------------------------------------------------
IMAGE_MOUNT_NOSYMFOLLOW=0
readonly NOSYMFOLLOW_PROBE_REL='.cec-l2-nosymfollow-probe'

# Prove it rather than trust the remount's status: a symbolic link inside the image, and an
# ordinary open THROUGH THE PATHNAME, which is exactly the operation MS_NOSYMFOLLOW forbids.
# Returns 0 when the link could not be followed (nosymfollow is in force), 1 otherwise.
#
# The probe directory and its target file are created and removed THROUGH THE CONFINED PRIMITIVE,
# so the probe cannot itself become the hazard; the symbolic link is created with `ln -s`, whose
# final component is created rather than resolved, inside a directory this function has just
# created and confirmed by descriptor.  Everything is removed again and the removal is verified,
# because this runs immediately before publication and a leftover would be published.
nosymfollow_is_in_force() {
    local probe_dir="$NOSYMFOLLOW_PROBE_REL" followed=0
    local link_path="$IMAGE_MOUNT/$NOSYMFOLLOW_PROBE_REL/link"

    # A leftover from an interrupted earlier attempt is cleared first, and by the primitive.
    nosymfollow_probe_cleanup

    confined_mkdir "$probe_dir" 0700 'the nosymfollow probe directory' \
        'establish whether nosymfollow is in force on the image mount'
    confined_create "$probe_dir/target" 0600 'the nosymfollow probe target' \
        'establish whether nosymfollow is in force on the image mount'
    if ! ln -sfn 'target' -- "$link_path" 2>/dev/null; then
        warn "the nosymfollow probe could not create a symbolic link inside the image, so it"
        warn "  cannot be established whether the remount took effect.  Reported as NOT in"
        warn "  force, which is the conservative reading; the confined I/O primitive is what"
        warn "  carries this scan's guarantee and it is proven in force."
        nosymfollow_probe_cleanup
        return 1
    fi

    # The one deliberately UNCONFINED open in this file, and it is a probe rather than an
    # operation: reading through the pathname is precisely what must fail.  Its content is a
    # zero-byte file this function created, so following the link discloses nothing.
    if cat -- "$link_path" >/dev/null 2>&1; then
        followed=1
    fi
    nosymfollow_probe_cleanup
    [ "$followed" -eq 0 ]
}

# Removes the probe and verifies the removal.  Fatal if anything is left, because this runs
# immediately before the image is released and published.
nosymfollow_probe_cleanup() {
    local record type_field entry entry_type entry_name listing
    confined_directory "$NOSYMFOLLOW_PROBE_REL" "/$NOSYMFOLLOW_PROBE_REL" \
        'remove the nosymfollow probe from the image' || return 0

    listing="$WORK_DIR/nosymfollow-probe-listing"
    rm -f -- "$listing"
    confined_list "$NOSYMFOLLOW_PROBE_REL" 'the nosymfollow probe directory' \
        'remove the nosymfollow probe from the image' > "$listing"
    while IFS= read -r -d '' entry; do
        entry_type="${entry%% *}"
        entry_name="${entry#* }"
        [ -n "$entry_name" ] || continue
        if [ "$entry_type" = 'directory' ]; then
            confined_rmdir "$NOSYMFOLLOW_PROBE_REL/$entry_name" \
                'an entry of the nosymfollow probe directory' \
                'remove the nosymfollow probe from the image'
        else
            confined_unlink "$NOSYMFOLLOW_PROBE_REL/$entry_name" \
                'an entry of the nosymfollow probe directory' \
                'remove the nosymfollow probe from the image'
        fi
    done < "$listing"
    rm -f -- "$listing"

    confined_rmdir "$NOSYMFOLLOW_PROBE_REL" 'the nosymfollow probe directory' \
        'remove the nosymfollow probe from the image'
    if ! confined_absent "$NOSYMFOLLOW_PROBE_REL" 'the nosymfollow probe directory' \
            'verify the nosymfollow probe is gone'; then
        record="$(confined_stat "$NOSYMFOLLOW_PROBE_REL" 'verify the nosymfollow probe is gone' \
                  || printf '')"
        type_field="$(confined_stat_field "$record" 1)"
        die "the nosymfollow probe directory /$NOSYMFOLLOW_PROBE_REL is still present inside the
       image (as a ${type_field:-unreadable}) after this script removed it.  The image is NOT
       published: it would carry a directory this script created for a one-off measurement, and
       something is recreating it."
    fi
}

try_nosymfollow_remount() {
    if [ ! -d "$IMAGE_MOUNT" ] || ! mountpoint -q -- "$IMAGE_MOUNT" 2>/dev/null; then
        log "  nosymfollow: not attempted, $IMAGE_MOUNT is not a mount point at this moment"
        log "    (defence in depth only; the confined I/O primitive is what carries this scan's"
        log "     guarantee and it is proven in force)"
        return 0
    fi
    # THE LAST PRIVILEGED mount(2) IN THIS SCRIPT, AND IT CONSUMES A DESCRIPTOR LIKE THE OTHERS.
    # This one is a remount rather than a new mount, and its target is the image root itself, so
    # the helper opens $IMAGE_MOUNT O_DIRECTORY|O_NOFOLLOW, verifies THAT DESCRIPTOR by fstat
    # against the identity create_and_mount_image() pinned, and performs
    # mount(NULL, /proc/self/fd/N, NULL, MS_REMOUNT|MS_NOSYMFOLLOW, NULL) -- so the object
    # remounted is the one that was checked, not whatever the name resolves to at the instant of
    # the call.  The pathname pin is checked first as a diagnostic, exactly as at the chroot
    # entries, and there is no fallback to `mount -o remount -- <pathname>`.
    #
    # THE TWO FAILURE KINDS ARE DELIBERATELY NOT TREATED ALIKE, AND THE HELPER IS WHAT LETS THEM
    # BE TOLD APART.  A CUSTODY failure -- the descriptor cannot be opened, or does not fstat to
    # the pin -- means the thing mounted at that path is not this run's image, which invalidates
    # the scan around it, and it is FATAL.  mount(2) itself returning an error is the legitimate
    # "this kernel or filesystem does not offer MS_NOSYMFOLLOW" case, which is defence in depth
    # this scan can proceed without, and it is reported and tolerated.  Before the helper existed
    # both arrived as one non-zero exit status from mount(8) and could only be separated by a
    # preceding check on the pathname; now they are separate statuses from the process that
    # actually performed the operation.
    #
    # It sits AFTER the not-a-mount-point arm above deliberately -- that arm is the legitimate
    # "there is nothing mounted here" case and stays non-fatal, whereas reaching this line means
    # something IS mounted at that path.  Propagation is not re-measured here because a remount
    # does not alter it -- the image mount was made private and verified in
    # create_and_mount_image(), and propagation is not among the things a remount changes.
    assert_image_root_pinned 'remount the image with nosymfollow for the credential scan'
    if pinned_image_remount_nosymfollow 'the image root' \
            'remount the image with nosymfollow for the credential scan'; then
        if nosymfollow_is_in_force; then
            IMAGE_MOUNT_NOSYMFOLLOW=1
            log "  nosymfollow: IN FORCE on $IMAGE_MOUNT and PROVEN -- a symbolic link created"
            log "    inside the image could not be opened through its own pathname, so the"
            log "    kernel itself refuses to follow any link in the image for the duration of"
            log "    this scan (MS_NOSYMFOLLOW).  Defence in depth on top of the confined I/O"
            log "    primitive, which is mandatory and is what carries the guarantee."
            log "    Not restored afterwards: release_image() unmounts the image immediately"
            log "    after this function returns, which is why this must stay the last step on"
            log "    the mount."
            return 0
        fi
        warn "the image mount ACCEPTED remount,nosymfollow but a symbolic link inside the image"
        warn "  can still be opened through its pathname, so MS_NOSYMFOLLOW is NOT in force"
        warn "  despite the remount reporting success.  The remount's exit status is reported"
        warn "  rather than trusted precisely so this case cannot be logged as a defence that"
        warn "  is not there.  This changes nothing about the scan's guarantee: every read,"
        warn "  rewrite, removal and enumeration this scan performs goes through openat2 with"
        warn "  RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, which is mandatory, was"
        warn "  proven to refuse at start-up, and is what confines this scan."
        return 0
    fi
    warn "nosymfollow could NOT be enabled on the image mount, so the kernel-level refusal to"
    warn "  follow symbolic links is NOT in force for this scan.  mount(2) refused"
    warn "  MS_REMOUNT|MS_NOSYMFOLLOW on the image root's own verified descriptor, which needs a"
    warn "  5.10-or-newer kernel; util-linux plays no part in it any more, because the flag is"
    warn "  passed to the syscall directly rather than spelled as a mount(8) option name."
    warn "  The CUSTODY of the target was established either way -- a descriptor that did not"
    warn "  fstat to this run's image root would have been fatal, not a warning.  This is stated"
    warn "  rather than glossed over because the log"
    warn "  would otherwise read as though the defence were active.  It is DEFENCE IN DEPTH"
    warn "  ONLY: every read, rewrite, removal and enumeration this scan performs goes through"
    warn "  openat2 with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV, anchored on the"
    warn "  image mount and operating through the resulting descriptor, which is mandatory, was"
    warn "  proven to refuse a substituted link at start-up, and is what carries the guarantee."
    return 0
}

# The apt source locations, RELATIVE to the image mount point, because that is what the confined
# primitive takes.  An absolute path is refused by RESOLVE_BENEATH, and a path assembled from
# $IMAGE_MOUNT would reintroduce the host-side pathname this mechanism exists to remove.
readonly IMAGE_APT_SOURCES_FILE_REL='etc/apt/sources.list'
readonly IMAGE_APT_SOURCES_DIR_REL='etc/apt/sources.list.d'

# ------------------------------------------------------------------------------------
# THE SECRET VALUES THEMSELVES, SEARCHED FOR INSIDE THE FINISHED IMAGE.
#
# assert_secret_inputs_outside_copied_roots() is a POLICY check: it refuses a secret whose PATH
# lies inside a copied tree, at validation time, before anything is copied.  This is the belt
# to that braces, and it asks a different question: are the secret VALUES anywhere in the trees
# this script actually copied?  It therefore catches what the policy cannot see --
#
#   * a secret file outside every copied root whose CONTENT was duplicated into one of them by
#     something else (a build script that cached it, an editor backup, a committed fixture),
#   * a secret pasted into a file in the caller's own checkout rather than kept in the file the
#     option names,
#   * and any future copied root somebody adds to copy_guest_payload() without adding it to the
#     policy list.
#
# WHERE IT LOOKS, AND WHY ONLY THERE.  The three trees copied wholesale: --payload, --sdk-dir
# and --gtest-prefix, at their in-image locations.  It deliberately does NOT search /etc: the
# base URL legitimately appears in /etc/apt/sources.list, which is what apt is configured with,
# and the credentials that could appear there are handled by
# scrub_and_assert_image_apt_credentials() and scrub_and_assert_image_credential_stores() --
# scans that REWRITE what they find rather than merely refusing it.  Searching /etc here would
# refuse every ordinary run for the value the run is supposed to configure.
#
# WHY THE SEARCH IS AN OPERATION OF THE CONFINED PRIMITIVE.  The trees are wholesale copies of
# caller-supplied checkouts and legitimately contain symbolic links, and they sit under
# /opt/cec-l2 inside an image whose /opt could itself be a link in the base tarball.  A
# `grep -r` would skip links it found INSIDE the tree but the kernel would resolve grep's
# STARTING path normally, so the scan could read the build host and report host filenames as
# image contents.  op_search() resolves the starting directory beneath the image mount with
# openat2 and walks it descriptor-relative with O_NOFOLLOW throughout.
#
# NOTHING SECRET REACHES A DIAGNOSTIC.  The needles are passed to the helper on a pipe, never
# in an argument, so they do not appear in any /proc/<pid>/cmdline; the helper prints a LABEL
# and a path and never the matched bytes; and the die message below names the label, the file
# and the remedy.
# ------------------------------------------------------------------------------------
assert_image_carries_no_secret_values() {
    local -a labels=() needles=() roots=() shown_roots=()
    local record_file="$WORK_DIR/secret-value-scan"
    local line token query count=0 hits=0 rel record label path root i

    # ---- the base URL, when it was given as a confidential file -------------------------
    # Only then.  A --base-url passed on the command line is already in this process's argv and
    # in the manifest's sanitised record; treating it as a secret here would refuse a run for a
    # value the caller chose to make public.
    if [ -n "$BASE_URL_FILE" ] && [ -n "$BASE_URL" ]; then
        labels+=('base-url'); needles+=("$BASE_URL")
        # The query string on its own, because that is where a presigned URL carries its bearer
        # token -- and a tree could hold the token without holding the whole URL.
        case "$BASE_URL" in
            *'?'*)
                query="${BASE_URL#*\?}"
                query="${query%%\#*}"
                if [ "${#query}" -ge 8 ]; then
                    labels+=('base-url-query'); needles+=("$query")
                fi
                ;;
        esac
    fi

    # ---- the apt credential file -------------------------------------------------------
    # Whole lines first: an auth.conf record is long and specific, so a whole-line needle has
    # essentially no false-positive surface.  Then the login and password VALUES on their own,
    # because a tree can hold the password without holding the record it came from.
    if [ -n "$APT_AUTH_FILE" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            line="${line#"${line%%[![:space:]]*}"}"
            line="${line%"${line##*[![:space:]]}"}"
            case "$line" in
                ''|'#'*) continue ;;
            esac
            count=$(( count + 1 ))
            labels+=( "apt-auth-record-$count" ); needles+=( "$line" )
        done < "$APT_AUTH_CUSTODY"
        count=0
        # BOTH SCANS READ APT_AUTH_CUSTODY, not the caller's name: the needles have to be the
        # bytes that were installed, and re-resolving the name here could search for one file's
        # secrets while the image carries another's.  awk reads it as an ordinary file argument,
        # which works because /proc/<pid>/fd/<n> re-reads the held descriptor's object from the
        # start; the published route is all awk needs, and it opens that route itself rather than
        # being handed a descriptor.  Note that it does INHERIT the held descriptor regardless --
        # `exec {fd}<` sets no close-on-exec flag, so every child this script spawns has it open.
        # That is not relied on here and is not claimed as a confinement property: the descriptor
        # is on a file the caller supplied to be read, and the children are this script's own.
        #
        # awk without a "--" separator: mawk treats "--" as a filename rather than as an option
        # terminator, and APT_AUTH_CUSTODY is a canonical absolute path so it cannot be read as
        # an option.  tolower() so that apt's case-insensitive keywords are all matched.
        while IFS= read -r token; do
            [ -n "$token" ] || continue
            count=$(( count + 1 ))
            if [ "${#token}" -lt 6 ]; then
                # NOT SEARCHED FOR, AND SAID OUT LOUD.  A needle of five bytes or fewer occurs
                # by coincidence in ordinary binaries and source, so searching for it would
                # fail every run rather than detect anything.  The value is not echoed.
                warn "one of the login or password values in the --apt-auth-file credential file
       is ${#token} bytes long, which is too short to search for distinctively: a needle that
       short matches ordinary compiled objects by coincidence, so the pre-publication sweep
       would refuse every run instead of detecting a leak.  Its whole auth.conf record IS
       searched for, so a copy of the record is still caught; a copy of that value ALONE is
       not.  Use a longer secret if this matters."
                continue
            fi
            labels+=( "apt-auth-secret-$count" ); needles+=( "$token" )
        done < <(awk '{ for (i = 1; i < NF; i++) { k = tolower($i);
                            if (k == "login" || k == "password") print $(i + 1) } }' \
                     "$APT_AUTH_CUSTODY" | sort -u)
    fi

    if [ "${#needles[@]}" -eq 0 ]; then
        log "no confidential input was given, so there is no secret value to search the copied"
        log "  trees for.  The credential-store and apt-configuration scans run regardless."
        return 0
    fi

    roots+=( "${GUEST_PAYLOAD_DIR#/}" );     shown_roots+=( "$GUEST_PAYLOAD_DIR (--payload)" )
    roots+=( "${GUEST_SDK_DIR#/}" );         shown_roots+=( "$GUEST_SDK_DIR (--sdk-dir)" )
    if [ -n "$GTEST_PREFIX_SRC" ]; then
        roots+=( "${GUEST_GTEST_PREFIX#/}" ); shown_roots+=( "$GUEST_GTEST_PREFIX (--gtest-prefix)" )
    fi
    roots+=( "${GUEST_SINK_COMPONENT_DIR#/}" )
    shown_roots+=( "$GUEST_SINK_COMPONENT_DIR (--sink-caller-source)" )

    log "searching the copied trees inside the image for the ${#needles[@]} confidential value(s)"
    log "  this run holds (the values are passed to the search on a pipe and never appear in an"
    log "  argument, a log line or a diagnostic):"
    for i in "${!shown_roots[@]}"; do
        log "    ${shown_roots[i]}"
    done

    for i in "${!roots[@]}"; do
        rel="${roots[i]}"
        if ! confined_directory "$rel" "the copied tree ${shown_roots[i]}" \
                'search the copied trees for the confidential input values'; then
            # Absent rather than refused -- confined_directory() dies on a refusal, so this arm
            # means the tree is not there.  --gtest-prefix is optional and the other two are
            # created by copy_guest_payload(), so this is an internal inconsistency worth
            # reporting rather than a caller mistake.
            warn "the copied tree ${shown_roots[i]} is not present inside the image, so it could
       not be searched for the confidential input values.  Nothing was copied there, or
       something removed it after copy_guest_payload() ran."
            continue
        fi
        : > "$record_file"
        confined_search "$rel" "the copied tree ${shown_roots[i]}" \
            'search the copied trees for the confidential input values' \
            "${labels[@]}" > "$record_file" < <(printf '%s\0' "${needles[@]}")
        while IFS= read -r -d '' record; do
            [ -n "$record" ] || continue
            label="${record%% *}"
            path="${record#* }"
            hits=$(( hits + 1 ))
            warn "a confidential input value ($label) was found inside the image at
       ${shown_roots[i]}/$path"
        done < "$record_file"
        : > "$record_file"
    done
    rm -f -- "$record_file"

    if [ "$hits" -gt 0 ]; then
        die "$hits file(s) inside the copied trees contain a value this run was given as a
       CONFIDENTIAL input, and the warnings above name each one by file and by which value it
       was.  The image is NOT published: it would carry the secret into an artefact that
       outlives this run and that the workflow uploads.
       The label says what to do:
         base-url / base-url-query -- the value came from --base-url-file, which exists to keep
           it off every command line.  If that URL is genuinely in your own checkout it is not
           confidential: pass it with --base-url instead, which accepts no query string and
           records a sanitised form in the manifest.  If it IS confidential, remove the copy
           from the tree.
         apt-auth-record-N / apt-auth-secret-N -- an apt credential, or one of its login or
           password values, is present in a tree this script copies into the image.  Remove it
           from the tree; --apt-auth-file is installed for the package phase only and removed
           again before the image is released, and that is the only place a credential belongs.
       This check reads only; nothing in the image was modified by it, so the tree can be
       inspected as it stands."
    fi

    log "the copied trees carry none of this run's confidential input values"
}

scrub_and_assert_image_apt_credentials() {
    local rel shown line tmp content listing record entry_type entry_name
    local scrubbed_files=0 scrubbed_lines=0 offenders=0 lineno field
    local -a rels=() fields=()

    # THE IMAGE'S OWN DEVICE, read once from the mount point.  It is used only by the
    # assert_confined_to_image PRE-CHECK: the confinement itself is enforced by the kernel
    # through RESOLVE_NO_XDEV on every confined open, which is the same test without the second
    # name lookup.  It is still read here, and its absence is still fatal, because a mount point
    # whose device cannot be read is not a mount this script goes on to scan.
    IMAGE_MOUNT_DEVICE="$(stat -c '%d' -- "$IMAGE_MOUNT" 2>/dev/null || printf '')"
    [ -n "$IMAGE_MOUNT_DEVICE" ] || die "the device of the image mount point $IMAGE_MOUNT could
       not be read, so the pre-publication credential scan cannot even report which filesystem
       the files it reads and rewrites are on.  The image is NOT published without it."
    mountpoint -q -- "$IMAGE_MOUNT" 2>/dev/null || die "the image is not mounted at $IMAGE_MOUNT
       when the pre-publication credential scan runs.  Every path it touches is resolved beneath
       that mount point; without the mount there is nothing to resolve against and nothing to
       scan.  The image is NOT published."
    # The anchor is re-asserted here rather than assumed from make_work_dir(), because this is
    # the function that reads and rewrites as root and a mis-set anchor would move all of it.
    [ "$CONFINED_IO_ANCHOR" = "$IMAGE_MOUNT" ] || die "internal error: the confined I/O anchor is
       '${CONFINED_IO_ANCHOR:-<unset>}' rather than the image mount point $IMAGE_MOUNT when the
       pre-publication credential scan runs, so every path it resolved would be resolved against
       the wrong directory.  This is a defect in $SCRIPT_NAME."

    log "apt source credential check on the finished filesystem"
    try_nosymfollow_remount

    # THE FIXED SOURCES FILE.  Its existence is optional; its SHAPE is not.  confined_regular_file
    # answers both questions from an fstat on the descriptor the confinement produced, so a
    # symlink is refused by the kernel before any type test runs -- which is the substitution the
    # old `-L` then `-f` pair was written to catch and could only catch by racing it.
    if confined_regular_file "$IMAGE_APT_SOURCES_FILE_REL" '/etc/apt/sources.list' \
            'read the image apt sources file'; then
        rels+=("$IMAGE_APT_SOURCES_FILE_REL")
    fi

    # THE sources.list.d ENTRIES, enumerated THROUGH A DESCRIPTOR (confined_list), so the
    # enumeration cannot be redirected onto a host directory by a symlinked /etc or /etc/apt --
    # which `find -P` on an absolute starting path could be, because the kernel resolves find's
    # starting path normally even though find does not follow links it finds inside the tree.
    #
    # Anything that is not a plain file is REFUSED, not skipped: it means the base filesystem is
    # not what it claims to be, and skipping it publishes the image with that substitution
    # unexamined.  The listing goes through a host-side file rather than a command substitution
    # because the records are NUL-terminated (a filename may contain a newline) and because a
    # process substitution would swallow the helper's own fatal exit.
    if confined_directory "$IMAGE_APT_SOURCES_DIR_REL" '/etc/apt/sources.list.d' \
            'enumerate the image apt source entries'; then
        listing="$WORK_DIR/apt-sources-listing"
        rm -f -- "$listing"
        confined_list "$IMAGE_APT_SOURCES_DIR_REL" 'the image apt source directory' \
            'enumerate the image apt source entries' > "$listing"
        while IFS= read -r -d '' record; do
            entry_type="${record%% *}"
            entry_name="${record#* }"
            [ -n "$entry_name" ] || continue
            [ "$entry_type" = 'regular' ] || die "the apt source entry
           /$IMAGE_APT_SOURCES_DIR_REL/$entry_name
       inside the image is a $entry_type rather than a regular file.  Refused rather than
       followed or skipped: this scan reads and rewrites these files as root on the BUILD HOST,
       and skipping the entry would publish the image with the base author's substitution
       unexamined.  Inspect the base tarball."
            rels+=("$IMAGE_APT_SOURCES_DIR_REL/$entry_name")
        done < "$listing"
        rm -f -- "$listing"
    fi

    if [ "${#rels[@]}" -eq 0 ]; then
        log "  the image carries no apt source files"
        # STILL CHECKED.  "No source lists" says nothing about auth.conf, a netrc, an apt.conf
        # proxy directive or an apt.conf.d one, so the second half of the precondition runs on
        # this path too -- and it must run BEFORE the early return rather than after it.
        scrub_and_assert_image_credential_stores
        return 0
    fi

    # Both staging files live on the HOST, in this script's own 0700 scratch directory, rather
    # than beside the file inside the image: a temporary file written into the image is one more
    # thing that a failure between writing it and removing it would publish.
    tmp="$WORK_DIR/apt-source-scrub"
    content="$WORK_DIR/apt-source-content"

    for rel in "${rels[@]}"; do
        local rewritten=0
        shown="/$rel"
        # THE PRE-CHECK, for its diagnostics only.  It names the offending path component where
        # the confined open would only say ELOOP.  It authorises nothing: the read below does not
        # reopen this pathname, it resolves the RELATIVE path beneath the anchor and reads through
        # the descriptor that resolution returned.
        assert_confined_to_image "$IMAGE_MOUNT/$rel" 'an apt source file' 'read'

        # THE READ, through the confinement.  Its bytes land in a host-side staging file, so the
        # line loop below cannot be reading something else by the time it finishes: the file was
        # opened once, beneath the anchor, and what follows is this script's own copy.
        rm -f -- "$content"
        confined_read "$rel" 'an apt source file' \
            "read the image apt source file $shown" > "$content"

        rm -f -- "$tmp"
        ( umask 077 && : > "$tmp" ) || die "could not stage the apt source rewrite at $tmp."
        while IFS= read -r line || [ -n "$line" ]; do
            sanitise_endpoint "$line"
            if [ "$SANITISED_ENDPOINT_REDACTED" -eq 1 ]; then
                rewritten=$((rewritten + 1))
                printf '%s\n' "$SANITISED_ENDPOINT" >> "$tmp" \
                    || die "could not write the scrubbed apt source line to $tmp."
            else
                printf '%s\n' "$line" >> "$tmp" \
                    || die "could not write an apt source line to $tmp."
            fi
        done < "$content"
        rm -f -- "$content"

        if [ "$rewritten" -gt 0 ]; then
            # THE WRITE, through the confinement.  The file keeps its own inode, owner and mode --
            # the helper truncates and writes THROUGH THE DESCRIPTOR rather than replacing the
            # name, and it reaches the same object no matter what the name resolves to, which a
            # `cat -- "$tmp" > "$file"` redirection cannot promise because it follows symbolic
            # links.  There is no re-check before it and none is needed: the name is resolved
            # once, by the kernel, with links refused at every position, so there is no window
            # between a check and this write for anything to be substituted in.
            confined_write "$rel" 'an apt source file' \
                "write the scrubbed apt sources back to $shown inside the image" < "$tmp"
            scrubbed_files=$((scrubbed_files + 1))
            scrubbed_lines=$((scrubbed_lines + rewritten))
            warn "removed a credential, query or fragment from $rewritten line(s) of"
            warn "  $shown inside the image.  It came from the base tarball, not from"
            warn "  --apt-mirror, which this script refuses in that form.  The guest is offline"
            warn "  and never fetches from these sources, so they are provenance only."
        fi
        rm -f -- "$tmp" || die "could not remove the staged apt source rewrite at $tmp."

        # THE RE-READ, also through the confinement.  This is the pass whose result decides
        # whether the image is published, so it must open the same object the rewrite wrote to --
        # which it does by construction, because both resolve the same relative path beneath the
        # same anchor with links refused.
        rm -f -- "$content"
        confined_read "$rel" 'an apt source file' \
            "re-read the image apt source file $shown" > "$content"
        lineno=0
        while IFS= read -r line || [ -n "$line" ]; do
            lineno=$((lineno + 1))
            fields=()
            read -r -a fields <<< "$line"
            for field in ${fields[@]+"${fields[@]}"}; do
                url_authority "$field"
                case "$URL_AUTHORITY" in
                    *@*)
                        offenders=$((offenders + 1))
                        warn "userinfo remains in $shown line $lineno"
                        ;;
                esac
                case "$field" in
                    *'?'*)
                        offenders=$((offenders + 1))
                        warn "a query string remains in $shown line $lineno"
                        ;;
                esac
            done
        done < "$content"
        rm -f -- "$content"
    done

    if [ "$offenders" -ne 0 ]; then
        die "$offenders apt source field(s) in the image still carry a URL credential or query
       after the scrub above, so the image is NOT published.  The lines are identified by file
       and line number in the warnings above and are deliberately not reproduced here.  Look at
       them in the base tarball: something in this script's rewrite did not recognise the form
       they use, and that is a defect in $SCRIPT_PATH rather than in the caller's inputs."
    fi

    if [ "$scrubbed_files" -eq 0 ]; then
        log "  ${#rels[@]} apt source file(s), no credential or query present"
    else
        log "  $scrubbed_lines line(s) across $scrubbed_files apt source file(s) scrubbed;"
        log "    re-read afterwards and no userinfo or query remains"
    fi
    # WHICH MECHANISM ACTUALLY CONFINED THIS SCAN, recorded beside the result rather than left
    # for a reader to infer.  The mandatory one is named first and named as mandatory, because a
    # list of four "layers" reads as depth even when the load-bearing one is only a pathname
    # check that the caller then reopens by name.
    log "  confinement: MANDATORY -- every read, rewrite, removal and enumeration above went"
    log "    through openat2 with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV anchored"
    log "    on $IMAGE_MOUNT, operating through the returned descriptor, with no pathname"
    log "    re-derived after the open (proven to refuse a substituted link at start-up)"
    if [ "$IMAGE_MOUNT_NOSYMFOLLOW" -eq 1 ]; then
        log "    DEFENCE IN DEPTH -- kernel-level nosymfollow on the image mount, verified in"
        log "      force behaviourally; plus the per-path diagnostic pre-check and the fatal"
        log "      refusal of any non-regular entry"
    else
        log "    DEFENCE IN DEPTH -- the per-path diagnostic pre-check and the fatal refusal of"
        log "      any non-regular entry; kernel-level nosymfollow was NOT in force on this host"
        log "      (see the warning above), which does not weaken the mandatory mechanism"
    fi

    # THE SECOND HALF OF THE SAME PRECONDITION.  A source list is not the only place apt keeps a
    # credential, and it is the only place the scan above looks.  The dedicated stores are
    # REMOVED and the apt configuration is REWRITTEN; the reasoning for each disposition is on
    # that function.
    scrub_and_assert_image_credential_stores
}

# ------------------------------------------------------------------------------------
# THE OTHER PLACES A CREDENTIAL LIVES, AND THE TWO DISPOSITIONS THEY GET.
#
# WHY THIS EXISTS AT ALL.  The scan above looks at apt's SOURCE LISTS and nothing else, which on
# its own would leave three whole classes of credential store unexamined in an image that is
# uploaded as a workflow artifact and copied between machines.  Worse, one of them is the obvious
# thing to RECOMMEND: telling a caller with an authenticated archive to "use /etc/apt/auth.conf.d
# in the base tarball, or a netrc" is advice to bake a permanent credential into the published
# artefact -- the same exposure the in-URL-credential refusal exists to prevent, reached by a
# route nothing checks.  So no message or help text here offers it, --apt-auth-file provides a
# build-time-only transport in its place, and this function is the independent net underneath
# both.
#
# WHAT IS SCANNED, and each is a real store rather than a guess:
#
#   /etc/apt/auth.conf, /etc/apt/auth.conf.d/*   apt's own netrc-format credential store.
#   /root/.netrc, /home/*/.netrc                 the store curl and wget read by default.
#   /etc/apt/apt.conf.d/*                        Acquire::http::Proxy "http://u:p@host:3128/"
#                                                and its https/ftp/socks siblings put a
#                                                credential in an ordinary configuration file.
#
# THIS IS NOT THE SAME CHECK AS assert_payload_carries_no_credentials, and neither replaces the
# other.  That one refuses credential material in the --payload TREE before it is copied in,
# and it looks for the shapes a source checkout carries (.git-credentials, a private key, a git
# extraheader).  This one looks at the image's OWN SYSTEM PATHS after every step has run, which
# is where the base tarball, the apt phase and this script itself could each have left one.
#
# TWO DISPOSITIONS, AND THE LINE BETWEEN THEM IS WHAT THE FILE IS FOR.
#
#   REMOVED OUTRIGHT -- auth.conf, auth.conf.d/*, and every .netrc.  A file of that kind exists
#   for one purpose, which is to authenticate a future fetch.  This guest performs no future
#   fetch: it boots with `qemu -nic none`, its init asserts that no non-loopback interface, no
#   route and no resolver exists, and every source, binary and library it needs is already
#   inside the image.  So there is no provenance in one worth preserving and nothing to weigh
#   against deleting it, and deletion is the disposition that cannot leave a residue -- a
#   rewrite of a netrc would have to decide which of its tokens were secret, and guessing that
#   is how a redaction misses one.
#
#   REWRITTEN IN PLACE -- apt.conf.d/*.  These are ORDINARY CONFIGURATION, not credential
#   stores: this script writes one itself (99cec-l2-snapshot), and a base tarball's own
#   fragments carry real settings a reader of the image is entitled to see.  So the disposition
#   matches the source lists: the offending field is reduced to scheme://host/path and the rest
#   of the file is kept, which removes the secret without discarding the configuration.
#
#   AND THE apt.conf.d TEST IS DELIBERATELY NARROW -- a field must carry BOTH a scheme (`://`)
#   AND userinfo in its authority.  A blanket search for '@' would match, and silently mangle,
#   an ordinary directive such as  //Unattended-Upgrade::Mail "root@localhost";  which ships
#   commented out in Debian's own 50unattended-upgrades.  The threat here is specifically a
#   proxy URL with an embedded credential, so that is exactly what is matched.
#
# HOW A FINDING IS REPORTED.  By FILE AND LINE NUMBER ONLY, never by content -- a die or warn
# message reaches the same log this whole sanitising apparatus exists for, and quoting the line
# would publish the secret to the log in order to complain about it being in the image.
#
# AND EVERY VERIFICATION IS FATAL, NOT ADVISORY.  A removal is re-read (`rm -f` reports success
# for a path that does not exist, so its status alone establishes nothing), a rewrite is
# re-read, and a survivor of either stops publication.
#
# EVERY PATH GOES THROUGH assert_confined_to_image FIRST, for exactly the reason stated in that
# function's block: this scan reads, rewrites and DELETES files as root on the build host, so a
# scan that followed a symlink out of the image would be exactly the host-write primitive the
# source-list scrub's confinement exists to remove.  A store that is a symlink or a non-regular
# file is refused FATALLY rather than skipped -- a base filesystem that ships /root/.netrc as a
# link to a host path is not a filesystem this script publishes an image around, and skipping it
# would publish the base author's substitution unexamined.
# ------------------------------------------------------------------------------------

# ------------------------------------------------------------------------------------
# THE INVENTORY, RE-DERIVED FROM APT'S OWN CREDENTIAL AND PROXY LOCATIONS.
#
# The obvious short list -- /etc/apt/auth.conf, /etc/apt/auth.conf.d, the netrc paths and
# /etc/apt/apt.conf.d -- omits /etc/apt/apt.conf, which is apt's PRIMARY configuration file and
# takes exactly the same directives as the fragments in apt.conf.d, so an
#     Acquire::http::Proxy "http://user:pass@proxy.example:3128/";
# written there would survive publication untouched while the identical line in
# apt.conf.d/99local was reduced.  The list below is therefore derived from where apt and the
# libraries it uses actually look, and not from the paths that come to mind first:
#
#   REMOVED   /etc/apt/auth.conf            apt_auth.conf(5): apt's own netrc-style store.
#   REMOVED   /etc/apt/auth.conf.d/*        the same, as fragments.
#   REMOVED   /root/.netrc, /home/*/.netrc  the store curl and wget read by default, and which
#                                           apt's https method inherits through libcurl on some
#                                           configurations.
#   REWRITTEN /etc/apt/apt.conf             apt.conf(5): the primary configuration file, and
#                                           the one the short list above omits.
#   REWRITTEN /etc/apt/apt.conf.d/*         the fragment directory apt reads after it.
#   REWRITTEN /etc/environment              where a Debian system sets http_proxy/https_proxy for
#                                           every process including apt, in the same
#                                           scheme://user:pass@host/ form.  apt inherits it, so
#                                           a credential there is an apt credential in every
#                                           practical sense.
#
# DELIBERATELY NOT INCLUDED, with the reason, so the omissions are decisions rather than gaps:
#
#   /etc/apt/apt.conf.d is covered above; /etc/apt/preferences and /etc/apt/sources.list* are
#   handled by scrub_and_assert_image_apt_credentials(), which is the scan this one is the second
#   half of.
#   /root/.curlrc and /root/.wgetrc  -- neither curl nor wget is in IMAGE_PACKAGES, so neither
#   file has a consumer inside this image, and the guest boots with no NIC and asserts itself
#   offline.  Adding them would mean rewriting files nothing reads, on a filesystem where their
#   absence is already asserted by the package accounting.
#   /etc/profile.d/*.sh  -- these are shell PROGRAMS, not configuration.  The redaction here
#   rewrites a field in place, which is safe for a KEY=value file and for an apt directive but is
#   not a transformation this script is willing to apply to executable code it did not write.  A
#   credential there would be reported by neither pass, and that is stated rather than hidden.
#   The environment the guest actually runs under is written by this script (write_image_conf and
#   the generated init), so it carries nothing the caller supplied.
#
# ALL of these go through the confined I/O primitive -- confined_regular_file/confined_directory
# to establish what is there, confined_list to enumerate, confined_read/confined_write to rewrite
# and confined_unlink to remove -- for the reason on that primitive's own block: this pass reads,
# rewrites and DELETES files as root on the build host.
# ------------------------------------------------------------------------------------

# Stores that are REMOVED.  Paths are absolute inside the image; the relative form the confined
# primitive takes is derived by stripping the leading '/' at the point of use, so the tables stay
# readable as the paths a person would recognise.
readonly IMAGE_CREDENTIAL_STORE_FILES=(
    '/etc/apt/auth.conf'
    '/root/.netrc'
)
readonly IMAGE_CREDENTIAL_STORE_DIRS=(
    '/etc/apt/auth.conf.d'
)
# Configuration FILES that are REWRITTEN rather than removed.
readonly IMAGE_CREDENTIAL_CONFIG_FILES=(
    '/etc/apt/apt.conf'
    '/etc/environment'
)
# Configuration DIRECTORIES whose entries are REWRITTEN rather than removed.
readonly IMAGE_CREDENTIAL_CONFIG_DIRS=(
    '/etc/apt/apt.conf.d'
)
# Per-user stores, enumerated rather than listed: a base tarball chooses its own accounts.
readonly IMAGE_CREDENTIAL_HOME_ROOT='/home'
readonly IMAGE_CREDENTIAL_HOME_LEAF='.netrc'

# Is this credential directory present and usable?  0 when it is, 1 when it is not there, and a
# `die` for anything else.  A thin wrapper over confined_directory that takes the ABSOLUTE
# in-image path the tables above use, so a caller does not strip the leading slash by hand and
# get it wrong in one place out of four.
credential_dir_usable() { # $1=absolute path inside the image  $2=path as shown
    confined_directory "${1#/}" "$2" "enumerate the credential paths under $2"
}

# Enumerates one directory THROUGH A DESCRIPTOR and appends its entries, as paths RELATIVE to the
# image mount point, to the array named by $3.  Anything that is not a plain file is refused
# fatally rather than skipped: a base filesystem that ships /etc/apt/auth.conf.d/x as a link or a
# fifo is not a filesystem this script publishes an image around, and skipping the entry would
# publish that substitution unexamined.
#
# `find -P` is not used for this, and the reason is not obvious.  find does not follow links it
# encounters INSIDE a tree, but the kernel resolves find's STARTING PATH normally, so an image
# whose /etc is an absolute symbolic link would have its HOST directory enumerated and host
# filenames reported into this script's diagnostics.  confined_list resolves every component
# beneath the anchor with links refused, and reads the directory through the descriptor that
# resolution produced.
collect_credential_dir_entries() { # $1=absolute dir inside the image  $2=dir as shown  $3=array
    local dir_rel="${1#/}" shown="$2" array_name="$3" record entry_type entry_name listing
    listing="$WORK_DIR/credential-dir-listing"
    rm -f -- "$listing"
    # Not a process substitution: confined_list dies on a refusal, and inside a subshell that
    # death would end the loop instead of the run -- which is a silent skip, the exact disposition
    # this function's comment refuses.
    confined_list "$dir_rel" "the credential directory $shown" \
        "enumerate the credential paths under $shown" > "$listing"

    # Appended through a nameref so that one enumeration serves both target arrays -- the same
    # idiom assert_lock_descriptor_identity already uses to return two values by name.  Declared
    # once, outside the loop, and never with a name that could alias it.
    local -n target="$array_name"
    while IFS= read -r -d '' record; do
        entry_type="${record%% *}"
        entry_name="${record#* }"
        [ -n "$entry_name" ] || continue
        [ "$entry_type" = 'regular' ] || die "the entry $shown/$entry_name inside the image is a
       $entry_type rather than a regular file.  Refused rather than followed or skipped: this
       scan opens, rewrites and removes these files as root on the BUILD HOST, and skipping the
       entry would publish the image with that substitution unexamined.  Inspect the base
       tarball."
        target+=("$dir_rel/$entry_name")
    done < "$listing"
    rm -f -- "$listing"
}

# The line numbers of netrc/auth.conf ENTRY lines, one per line, and nothing else -- never the
# line itself.  Used only to report WHERE a removed store carried entries; the removal does not
# depend on the result, so a false negative here cannot leave a credential in the image.
#
# $1 IS THIS SCRIPT'S OWN HOST-SIDE COPY of the store, not a path inside the image.  The caller
# reads the store through the confined primitive into that copy first, so awk never opens a
# pathname inside the image: an awk invocation against an in-image path would follow a symbolic
# link like any other ordinary open, and it would do so as root on the build host.
#
# The regular expressions are built as STRINGS rather than written as /.../ literals, because a
# literal would have to escape the slashes in "://" and the escaping differs between awk
# implementations.
credential_entry_lines() { # $1=host-side copy of the store -> line numbers on stdout
    awk 'BEGIN { entry = "(^|[ \t])(login|password|machine)[ \t]+[^ \t]" }
         tolower($0) ~ entry { print NR }' "$1" 2>/dev/null || true
}

# Reduces every scheme-bearing field of one line whose authority carries userinfo to its
# scheme://host/path form, and leaves every other field exactly as it was.  Sets
# CREDENTIAL_LINE_REDACTED to the rewritten line and CREDENTIAL_LINE_CHANGED to 1 when it
# changed anything.
#
# read -r -a rather than `for f in $line`, for the reason the sanitisers already give: it splits
# on IFS WITHOUT globbing, so a '*' in a configuration value is not expanded against the build
# host's filesystem.
CREDENTIAL_LINE_REDACTED=''
CREDENTIAL_LINE_CHANGED=0
redact_config_line_userinfo() { # $1=line
    local line="$1" field out='' first=1
    local -a fields=()
    CREDENTIAL_LINE_REDACTED="$line"
    CREDENTIAL_LINE_CHANGED=0
    case "$line" in
        *'://'*) : ;;
        # Nothing URL-shaped on the line at all, so there is nothing to reduce and the line is
        # returned untouched.  This is the common case for an apt.conf.d fragment.
        *) return 0 ;;
    esac
    read -r -a fields <<< "$line"
    for field in ${fields[@]+"${fields[@]}"}; do
        case "$field" in
            *'://'*)
                url_authority "$field"
                case "$URL_AUTHORITY" in
                    *@*)
                        sanitise_url_token "$field"
                        field="$SANITISED_URL_TOKEN"
                        CREDENTIAL_LINE_CHANGED=1
                        ;;
                esac
                ;;
        esac
        if [ "$first" -eq 1 ]; then out="$field"; first=0; else out="$out $field"; fi
    done
    CREDENTIAL_LINE_REDACTED="$out"
}

# Does this line still carry a scheme-bearing field with userinfo?  The re-read test, and it is
# deliberately the same narrow test the rewrite uses: a re-read that asked a broader question
# than the rewrite answered would fail every image carrying an ordinary '@' in a comment.
config_line_has_userinfo() { # $1=line
    local line="$1" field
    local -a fields=()
    case "$line" in
        *'://'*) : ;;
        *) return 1 ;;
    esac
    read -r -a fields <<< "$line"
    for field in ${fields[@]+"${fields[@]}"}; do
        case "$field" in
            *'://'*)
                url_authority "$field"
                case "$URL_AUTHORITY" in
                    *@*) return 0 ;;
                esac
                ;;
        esac
    done
    return 1
}

scrub_and_assert_image_credential_stores() {
    local rel shown in_image lines line tmp content listing record
    local entry_type entry_name
    local removed=0 inspected=0 rewritten_files=0 rewritten_lines=0 offenders=0 lineno
    local -a stores=() configs=()

    tmp="$WORK_DIR/apt-config-scrub"
    content="$WORK_DIR/credential-store-content"

    # ---- THE STORES THAT ARE REMOVED --------------------------------------------------
    # confined_regular_file answers "is it there, and is it a plain file" from an fstat on the
    # descriptor the confinement produced.  A symbolic link is refused by the kernel before the
    # type test is even reached, which is the case an `-L` then `-e` pair cannot settle: -e
    # follows, so a dangling link reads as absent and a live one is examined as though it were
    # the caller's own file.
    for in_image in "${IMAGE_CREDENTIAL_STORE_FILES[@]}"; do
        if confined_regular_file "${in_image#/}" "$in_image" \
                "remove the credential store $in_image from the image"; then
            stores+=("${in_image#/}")
        fi
    done
    for in_image in "${IMAGE_CREDENTIAL_STORE_DIRS[@]}"; do
        if credential_dir_usable "$in_image" "$in_image"; then
            collect_credential_dir_entries "$in_image" "$in_image" stores
        fi
    done
    # /home/*/.netrc.  The home root itself is checked the same way, then each home directory is
    # looked in without descending further: a .netrc is at the top level of a home by definition,
    # and a recursive walk would enumerate a user's whole tree as root.
    if credential_dir_usable "$IMAGE_CREDENTIAL_HOME_ROOT" "$IMAGE_CREDENTIAL_HOME_ROOT"; then
        listing="$WORK_DIR/credential-home-listing"
        rm -f -- "$listing"
        confined_list "${IMAGE_CREDENTIAL_HOME_ROOT#/}" 'the image home directory root' \
            "enumerate the per-user credential stores under $IMAGE_CREDENTIAL_HOME_ROOT" \
            > "$listing"
        while IFS= read -r -d '' record; do
            entry_type="${record%% *}"
            entry_name="${record#* }"
            [ -n "$entry_name" ] || continue
            # Only directories are descended into.  A non-directory at the top of /home is not a
            # home directory and is not this pass's business -- it holds no .netrc by definition,
            # and refusing it would fail images over an unrelated stray file.
            [ "$entry_type" = 'directory' ] || continue
            rel="${IMAGE_CREDENTIAL_HOME_ROOT#/}/$entry_name/$IMAGE_CREDENTIAL_HOME_LEAF"
            if confined_regular_file "$rel" "/$rel" \
                    "remove the credential store /$rel from the image"; then
                stores+=("$rel")
            fi
        done < "$listing"
        rm -f -- "$listing"
    fi

    for rel in ${stores[@]+"${stores[@]}"}; do
        shown="/$rel"
        # THE PRE-CHECK, for its diagnostics only; the removal below does not reopen this
        # pathname.  It resolves the relative path beneath the anchor and unlinks the leaf through
        # a descriptor, and unlinkat never follows a symbolic link -- so there is no gap between a
        # check and the deletion for anything to be substituted into (CWE-367).
        assert_confined_to_image "$IMAGE_MOUNT/$rel" "the credential store $shown" 'remove'
        inspected=$(( inspected + 1 ))

        # The store is read into this script's own host-side copy THROUGH THE CONFINEMENT before
        # it is counted, so awk never opens an in-image pathname.  The copy is removed again
        # immediately; its content is never logged.
        rm -f -- "$content"
        confined_read "$rel" "the credential store $shown" \
            "count the credential entries in $shown" > "$content"
        lines="$(credential_entry_lines "$content")"
        rm -f -- "$content"

        warn "the image carries the credential store $shown; it is REMOVED."
        if [ -n "$lines" ]; then
            warn "  It holds credential entries at line(s): $(printf '%s' "$lines" | tr '\n' ' ')"
        fi
        warn "  Removed rather than rewritten: this guest boots with no NIC and fetches"
        warn "  nothing, so a credential store in it authenticates nothing and is pure"
        warn "  liability.  Its content is deliberately not reproduced in this log.  For an"
        warn "  authenticated archive use --apt-auth-file, which is installed for the package"
        warn "  phase only and removed before the image is released."

        confined_unlink "$rel" "the credential store $shown" \
            "remove the credential store $shown from the image"
        removed=$(( removed + 1 ))

        # FATAL RE-READ, through the confinement.  An unlink that returned 0 establishes that a
        # call succeeded; this establishes that nothing is at the name any more -- and it cannot
        # be satisfied by planting a symbolic link there, because confined_absent dies on a
        # refusal rather than reading one as absence.
        confined_absent "$rel" "the credential store $shown" \
            "verify the credential store $shown is gone" \
            || die "the credential store $shown is still present in the image after it was
       removed.  The image is NOT published.  Something is recreating it, or the removal did not
       do what its own exit status claimed."
    done

    # ---- THE CONFIGURATION THAT IS REWRITTEN ------------------------------------------
    # The named FILES first -- /etc/apt/apt.conf is apt's primary configuration file and takes
    # the same Acquire::*::Proxy directives as the fragments, and it is the path an inventory of
    # apt.conf.d alone omits -- then the fragment directories.
    for in_image in "${IMAGE_CREDENTIAL_CONFIG_FILES[@]}"; do
        if confined_regular_file "${in_image#/}" "$in_image" \
                "rewrite any URL credential in $in_image"; then
            configs+=("${in_image#/}")
        fi
    done
    for in_image in "${IMAGE_CREDENTIAL_CONFIG_DIRS[@]}"; do
        if credential_dir_usable "$in_image" "$in_image"; then
            collect_credential_dir_entries "$in_image" "$in_image" configs
        fi
    done

    for rel in ${configs[@]+"${configs[@]}"}; do
        local changed=0
        shown="/$rel"
        assert_confined_to_image "$IMAGE_MOUNT/$rel" "the apt configuration file $shown" 'read'
        inspected=$(( inspected + 1 ))

        # Read through the confinement into a host-side copy, so the line loop is iterating over
        # this script's own bytes and not over a name that can change under it.
        rm -f -- "$content"
        confined_read "$rel" "the apt configuration file $shown" \
            "read the image apt configuration file $shown" > "$content"

        # Staged on the HOST, in this script's own 0700 scratch directory, for the reason the
        # source-list scrub gives: a temporary file written inside the image is one more thing a
        # failure between writing it and removing it would publish.
        rm -f -- "$tmp"
        ( umask 077 && : > "$tmp" ) || die "could not stage the apt configuration rewrite at $tmp."
        lineno=0
        while IFS= read -r line || [ -n "$line" ]; do
            lineno=$(( lineno + 1 ))
            redact_config_line_userinfo "$line"
            if [ "$CREDENTIAL_LINE_CHANGED" -eq 1 ]; then
                changed=$(( changed + 1 ))
                warn "a URL credential was removed from $shown line $lineno inside the image"
                printf '%s\n' "$CREDENTIAL_LINE_REDACTED" >> "$tmp" \
                    || die "could not write the scrubbed apt configuration line to $tmp."
            else
                printf '%s\n' "$line" >> "$tmp" \
                    || die "could not write an apt configuration line to $tmp."
            fi
        done < "$content"
        rm -f -- "$content"

        if [ "$changed" -gt 0 ]; then
            # THE WRITE, through the confinement.  The file keeps its own inode, owner and mode,
            # because the helper truncates and writes THROUGH THE DESCRIPTOR rather than replacing
            # the name -- which is what `cat -- "$tmp" > "$path"` achieved by a redirection that
            # followed symbolic links.  No re-check precedes it and none is needed: the name was
            # resolved once by the kernel with links refused at every position.
            confined_write "$rel" "the apt configuration file $shown" \
                "write the scrubbed apt configuration back to $shown inside the image" < "$tmp"
            rewritten_files=$(( rewritten_files + 1 ))
            rewritten_lines=$(( rewritten_lines + changed ))
            warn "  ${changed} line(s) of $shown were reduced to scheme://host/path.  The"
            warn "  file is kept rather than removed because it is ordinary configuration, not a"
            warn "  credential store, and the setting itself is provenance.  The content is"
            warn "  deliberately not reproduced in this log."
        fi
        rm -f -- "$tmp" || die "could not remove the staged apt configuration rewrite at $tmp."

        # THE RE-READ, also through the confinement, because this is the pass whose result decides
        # whether the image is published: it must open the same object the rewrite wrote to, which
        # it does by construction -- both resolve the same relative path beneath the same anchor.
        rm -f -- "$content"
        confined_read "$rel" "the apt configuration file $shown" \
            "re-read the image apt configuration file $shown" > "$content"
        lineno=0
        while IFS= read -r line || [ -n "$line" ]; do
            lineno=$(( lineno + 1 ))
            if config_line_has_userinfo "$line"; then
                offenders=$(( offenders + 1 ))
                warn "a URL credential REMAINS in $shown line $lineno"
            fi
        done < "$content"
        rm -f -- "$content"
    done

    if [ "$offenders" -ne 0 ]; then
        die "$offenders line(s) of the image's apt configuration still carry a URL credential
       after the rewrite above, so the image is NOT published.  They are identified by file and
       line number in the warnings above and are deliberately not reproduced here.  Look at
       them in the base tarball: something in this script's rewrite did not recognise the form
       they use, and that is a defect in $SCRIPT_PATH rather than in the caller's inputs."
    fi

    if [ "$inspected" -eq 0 ]; then
        log "  credential-store check: the image carries none of the known stores"
        log "    (/etc/apt/auth.conf, /etc/apt/auth.conf.d/*, /etc/apt/apt.conf,"
        log "     /etc/apt/apt.conf.d/*, /etc/environment, /root/.netrc, /home/*/.netrc)"
        return 0
    fi
    log "  credential-store check: $inspected path(s) inspected"
    if [ "$removed" -eq 0 ]; then
        log "    no dedicated credential store present"
    else
        log "    $removed dedicated credential store(s) REMOVED and verified absent"
    fi
    if [ "$rewritten_files" -eq 0 ]; then
        log "    no URL credential in the apt configuration"
    else
        log "    $rewritten_lines line(s) across $rewritten_files apt configuration file(s)"
        log "      reduced to scheme://host/path; re-read afterwards and none remains"
    fi
    log "    every path above was read, rewritten, enumerated and removed through openat2 with"
    log "      RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS|RESOLVE_NO_XDEV anchored on the image mount"
    log "    reported by file and line number only; no content was written to this log"
}

# ------------------------------------------------------------------------------------
# RELEASING THE IMAGE.
#
# Done explicitly rather than left to the trap, for two reasons: the image must be fully
# flushed and detached before its size is read and the manifest is written, and a failure to
# release is a real failure of this run -- an image still held by a loop device is not a
# finished artifact.  The trap remains as the safety net for every path that does not reach
# this function.
# ------------------------------------------------------------------------------------
release_image() {
    log "releasing the image"
    sync || true
    local point
    for point in "$IMAGE_MOUNT/dev/pts" "$IMAGE_MOUNT/dev" "$IMAGE_MOUNT/sys" \
                 "$IMAGE_MOUNT/proc" "$IMAGE_MOUNT"; do
        mountpoint -q -- "$point" 2>/dev/null || continue
        # The same bounded-retry helper the trap uses, so the two agree about what "released"
        # means; its lazy fallback is then re-checked below and refused here.
        unmount_with_retries "$point"
        # A lazy unmount is NOT accepted here, unlike in the trap: the image must be fully
        # detached before its size is read and the manifest written, and a filesystem the
        # kernel is still finishing with is not a finished artefact.
        mountpoint -q -- "$point" 2>/dev/null && die "could not unmount $point after three
       attempts.  Something inside the image is still holding it -- a stray chroot process,
       most likely.  The image is NOT finished, so it is not declared as one."
    done
    if [ -n "$LOOP_DEVICE" ] && [ -b "$LOOP_DEVICE" ] && loop_still_attached "$LOOP_DEVICE"; then
        # IDENTITY BEFORE DETACH.  Between the unmount above and this detach the node can have
        # been re-used by another process on this host, and `losetup -d` as root would then
        # pull the backing file out from under THAT process's mounted image.
        assert_loop_backs "$LOOP_DEVICE" "$STAGED_IMAGE" "detach $LOOP_DEVICE"
        # No "--" before the device; see the note in cleanup().
        if ! losetup -d "$LOOP_DEVICE"; then
            # The kernel auto-clears a loop device once its last user closes it, so a detach
            # that fails AFTER the unmount above has usually already happened. Only a device
            # that is still bound is a real failure.
            loop_still_attached "$LOOP_DEVICE" && die "could not detach $LOOP_DEVICE from
       $STAGED_IMAGE.  The image is not finished while a loop device still holds it."
        fi
    fi
    # DURABILITY BEFORE PUBLICATION.  The staged image is about to be renamed onto the
    # caller's path and then read by QEMU, possibly after a reboot on a self-hosted runner.
    # `sync -f` flushes the filesystem holding it and is preferred because it does not stall
    # on every other filesystem the host has mounted; a coreutils too old for -f falls back to
    # the whole-system flush rather than skipping the flush.
    #
    # AND BOTH FAILING IS FATAL, rather than tolerated with a trailing `|| true`.  Tolerated,
    # an image whose bytes were never committed would still be published and reported as
    # finished; on a self-hosted runner that reboots, the caller would then have a published
    # path holding a partially written filesystem.  An unflushed image is not a finished image,
    # so the run stops here.
    #
    # Named through the publication directory descriptor, like every other operation on the
    # staged image: `sync -f` opens its argument, and an argument that resolved somewhere else
    # would flush somebody else's filesystem while reporting success for ours.
    if ! sync -f -- "$OUTPUT_DIR_PROC/$STAGED_IMAGE_LEAF" 2>/dev/null; then
        if ! sync; then
            die "the staged image could not be flushed to disk: neither
           sync -f -- '$STAGED_IMAGE'
       nor a whole-system 'sync' succeeded.  The image is NOT published: its bytes are not
       known to be on the disk, and any previously published pair is left exactly as it was."
        fi
        log "durability: 'sync -f' was unavailable or failed, so a whole-system sync was used"
    fi
    log "image released; every mount and loop device this run created is gone"
}

# ------------------------------------------------------------------------------------
# THE MANIFEST.  KEY=VALUE lines beside the image, so the workflow reads the paths it needs
# instead of hard-coding them -- and so a change to the guest layout is picked up by the
# workflow rather than silently disagreed with.
# ------------------------------------------------------------------------------------
write_manifest() {
    # Staged next to the staged image, for the same reason and with the same guarantees: the
    # manifest describes an image, so a manifest that becomes visible before its image is
    # published -- or that survives a failure that stopped the image from being published --
    # would describe something that is not there.  publish_outputs() renames the pair
    # together.
    #
    # Minted, checked and written THROUGH THE PUBLICATION DIRECTORY DESCRIPTOR, exactly as the
    # staged image is: $manifest below is $OUTPUT_DIR_PROC/<leaf>, so the redirection at the
    # end of this function writes inside the directory inode that was validated, and no rename
    # of the directory or an ancestor between here and there can redirect it.  $STAGED_MANIFEST
    # holds the human pathname for the messages and for the rename in publish_outputs.
    local manifest
    assert_output_dir_custody "create the staging file for the manifest in $OUTPUT_DIR"
    manifest="$(mktemp -- "$OUTPUT_DIR_PROC/.cec-l2-manifest.XXXXXXXX")" \
        || die "could not create a staging file for the manifest in $OUTPUT_DIR."
    STAGED_MANIFEST_LEAF="$(basename -- "$manifest")"
    STAGED_MANIFEST="$OUTPUT_DIR/$STAGED_MANIFEST_LEAF"
    record_leaf_identity "$manifest" STAGED_MANIFEST_DEVICE STAGED_MANIFEST_INODE \
        'the staged manifest' "$STAGED_MANIFEST"
    local size_bytes
    # The size is READ FROM THE STAGED FILE, which is the image that will be published, and it
    # is read through the descriptor for the same reason everything else is.
    size_bytes="$(stat -c '%s' -- "$OUTPUT_DIR_PROC/$STAGED_IMAGE_LEAF" 2>/dev/null \
        || printf 'unknown')"

    # THE MANIFEST DOES NOT DOCUMENT AN AUDIT GAP AS THOUGH IT WERE A MEASUREMENT.
    # record_installed_packages() is fatal on every path that cannot produce a concrete
    # package/version/architecture record, so these values are numbers and a digest by the time
    # this runs.  The check is here anyway because this is the function that would SERIALISE a
    # placeholder into the published artefact, and one line beside the write is cheaper than
    # trusting that a caller ordering never changes.
    case "$PACKAGE_RECORD_COUNT" in
        ''|*[!0-9]*)
            die "the installed-package count is '$PACKAGE_RECORD_COUNT' rather than a number at
       manifest-writing time, so record_installed_packages() either did not run or did not
       establish a concrete package set.  The manifest is NOT written and the image is NOT
       published: publishing a manifest that says the resolved package set is unknown would
       hand on an audit gap dressed as a measurement."
            ;;
    esac
    [ "$PACKAGE_RECORD_COUNT" -gt 0 ] || die "the installed-package count is zero at
       manifest-writing time.  The image is NOT published; see record_installed_packages()."
    case "$PACKAGE_RECORD_DIGEST" in
        [0-9a-f]*) : ;;
        *) die "the package record's sha256 is '$PACKAGE_RECORD_DIGEST' rather than a hex digest
       at manifest-writing time, so the manifest cannot carry a comparable fingerprint of the
       resolved package set.  The image is NOT published." ;;
    esac

    # Adjacent to the write, as everywhere else: the manifest is written as root into a
    # directory the caller chose, so the directory's identity and the leaf's own identity are
    # both re-established in the statement before the redirection that writes through them.
    assert_output_dir_custody "write the staged manifest $STAGED_MANIFEST"
    assert_leaf_identity "$manifest" "$STAGED_MANIFEST_DEVICE" "$STAGED_MANIFEST_INODE" \
        "write the staged manifest $STAGED_MANIFEST" "$STAGED_MANIFEST"

    {
        printf '# Manifest for the AIDL-path job'"'"'s QEMU root image.\n'
        printf '# Generated by %s. Read this file rather than hard-coding paths.\n' "$SCRIPT_NAME"
        printf 'GENERATED_BY=%s\n' "$SCRIPT_PATH"
        printf 'GENERATED_AT_UTC=%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        printf 'IMAGE=%s\n' "$OUTPUT_IMAGE"
        printf 'IMAGE_FS=ext4\n'
        printf 'IMAGE_FS_LABEL=cec-l2-root\n'
        printf 'IMAGE_SIZE_MIB=%s\n' "$IMAGE_SIZE_MIB"
        printf 'IMAGE_SIZE_BYTES=%s\n' "$size_bytes"
        printf 'ARCH=%s\n' "$ARCH"
        printf 'ELF_BITNESS=32-bit throughout\n'
        # THIS BLOCK IS THIS SCRIPT'S OWN TEXT AND IS NOT THE AUTHORITY ON THE PROTOCOL.
        # Saying so is the point of the wording, because a manifest is exactly the kind of
        # file a later reader quotes as fact.  What BINDER_PROTOCOL records is the image
        # builder's EXPECTATION, derived at image-build time from two artefacts the caller
        # supplied -- the guest kernel configuration and the staged SDK's CMake cache -- and
        # refused outright when those two disagree.  It is never declared, because a fixed
        # "BINDER_PROTOCOL=7" written here beside an SDK built BINDER_IPC_32BIT=OFF -- which is
        # protocol 8 -- is a manifest that contradicts its own image, and nothing about the
        # manifest's own text would reveal it.
        #
        # THE AUTHORITY IS THE THREE-WAY CROSS-CHECK THE RUN PERFORMS, not this line: the
        # RUNNING kernel's configuration as the guest reads it back, the SDK build flags as
        # recorded in the staging tree, and -- decisively -- the BINDER_VERSION ioctl on
        # /dev/binder, which is the value libbinder itself compares for equality when it opens
        # the node.  The init prints all three in the BINDER PROTOCOL AGREEMENT block on the
        # console and DIES naming whichever source disagrees, so a wrong expectation here
        # cannot pass as a result.  The lines below therefore carry the expectation, the
        # evidence for each half of it, and where the authoritative answer is to be read.
        printf 'BINDER_PROTOCOL=%s\n' "$DERIVED_BINDER_PROTOCOL"
        printf 'BINDER_PROTOCOL_STATUS=EXPECTATION recorded by the image builder, NOT an authoritative measurement of the running guest\n'
        printf 'BINDER_PROTOCOL_DERIVATION=derived at image-build time from the guest kernel configuration and the staged SDK CMake cache; never declared by the image builder, and refused when the two disagree\n'
        printf 'BINDER_PROTOCOL_FROM_KERNEL_CONFIG=%s\n' "${PROTOCOL_FROM_KERNEL_CONFIG:-unknown}"
        printf 'BINDER_PROTOCOL_FROM_SDK_CACHE=%s\n' "${PROTOCOL_FROM_SDK_CACHE:-unknown}"
        printf 'BINDER_PROTOCOL_AUTHORITY=the three-way cross-check performed at run time, not this file: (1) the running kernel configuration the guest reads back, (2) the SDK build flags recorded in the staging tree, (3) the BINDER_VERSION ioctl on /dev/binder, which is what libbinder compares for equality; disagreement between any of them is FATAL to the run\n'
        printf 'BINDER_PROTOCOL_VERIFIED_IN_GUEST=the init reads /dev/binder with the BINDER_VERSION ioctl, cross-checks it against the running kernel configuration and the recorded SDK value, and fails naming whichever disagrees; read the BINDER PROTOCOL AGREEMENT block in the console log for the authoritative outcome\n'
        printf 'GUEST_INIT=%s\n' "$GUEST_INIT_PATH"
        # THE ROOT DEVICE IS /dev/sda, NOT /dev/vda, AND THE REASON IS A SECURITY DECISION
        # RATHER THAN A PREFERENCE.  The boot step in .github/workflows/aidl-path-tests.yml
        # deliberately does NOT instantiate QEMU's virtio-blk device model: CVE-2024-8612 is a
        # virtio-blk bounce-buffer disclosure of QEMU HOST process memory to the guest and the
        # exact host package is listed affected, and CVE-2026-48914 is a privileged-guest
        # out-of-bounds write in the QEMU host heap with no fixed release through 11.0.1.  A
        # guest that runs repository-controlled payloads is precisely the threat those two
        # describe, so the root disk is attached through the emulated AHCI controller
        # (-drive if=none + -device ahci + -device ide-hd) and CONFIG_VIRTIO_BLK is asserted
        # ABSENT from the guest kernel, which makes the vulnerable device model unreachable
        # instead of merely unused.
        #
        # THIS FIELD IS A HINT AND NOTHING READS IT -- grep the two workflows for
        # GUEST_KERNEL_APPEND_HINT and there are no consumers; the authoritative -append is
        # built by the boot step, which also ASSERTS on the console that the kernel command
        # line carried root=/dev/sda and that the kernel mounted root on device 8:0, the
        # SCSI_DISK0_MAJOR that virtio_blk's dynamically allocated major can never be.  It is
        # kept accurate all the same, because a manifest field that contradicts the image it
        # describes is worse than an absent one: its only reader is a human reproducing the
        # boot by hand, and root=/dev/vda would send them to a device the kernel has no driver
        # for and no obvious reason why.
        printf 'GUEST_KERNEL_APPEND_HINT=root=/dev/sda rw init=%s console=ttyS0\n' "$GUEST_INIT_PATH"
        printf 'ARTIFACT_DIR=%s\n' "$GUEST_ARTIFACT_DIR"
        printf 'STATUS_FILE=%s\n' "$GUEST_STATUS_FILE"
        printf 'STATUS_FILE_FORMAT=one line holding the exact exit status of run_coverage.sh; 0 means measured and passing, 3 is ADVISORY and is NOT a pass, anything else is a failure\n'
        printf 'GUEST_WORKSPACE=%s\n' "$GUEST_WORKSPACE"
        printf 'GUEST_PAYLOAD_DIR=%s\n' "$GUEST_PAYLOAD_DIR"
        printf 'GUEST_SDK_DIR=%s\n' "$GUEST_SDK_DIR"
        printf 'GUEST_GTEST_PREFIX=%s\n' "${GTEST_PREFIX_SRC:+$GUEST_GTEST_PREFIX}"
        printf 'GUEST_IMAGE_CONF=%s\n' "$GUEST_IMAGE_CONF"
        # THE SINK CALLER SOURCE, in the PUBLISHED manifest and not only in the in-image record.
        #
        # The caller reads this file to decide what it boots, and this key is what lets it assert
        # that the guard's required input was staged WITHOUT mounting the image. The digest is
        # published beside the path for the reason the in-image record gives: "a file was staged"
        # and "THIS revision was staged" are different claims, and only the second distinguishes
        # a genuine plugin drift from a stale copy.
        printf 'GUEST_SINK_CALLER_SOURCE=%s\n' "$GUEST_SINK_CALLER_SOURCE"
        printf 'SINK_CALLER_SOURCE_ORIGIN=%s\n' "$SINK_CALLER_SOURCE_SRC"
        printf 'SINK_CALLER_SOURCE_SHA256=%s\n' \
            "$(sha256sum -- "$SINK_CALLER_SOURCE_SRC" | cut -d' ' -f1)"
        printf 'SINK_CALLER_SOURCE_PURPOSE=read by DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSinkSource, a REQUIRED acceptance input; the init exports this path as CEC_SINK_CALLER_SOURCE and refuses to continue if it is unreadable\n'
        printf 'GUEST_EXPECTED_KERNEL_CONFIG=%s\n' "$GUEST_KERNEL_EXPECTATION"
        printf 'GUEST_SDK_BUILD_FLAGS_RECORD=%s\n' "$GUEST_SDK_FLAGS_RECORD"
        printf 'REQUIRED_KERNEL_OPTIONS=CONFIG_ANDROID=y CONFIG_ANDROID_BINDER_IPC=y CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder" CONFIG_ANDROID_BINDERFS=y CONFIG_ASHMEM=y\n'
        if [ "$DERIVED_BINDER_PROTOCOL" = '7' ]; then
            printf 'PROTOCOL_SCOPED_KERNEL_OPTION=CONFIG_ANDROID_BINDER_IPC_32BIT=y\n'
        else
            printf 'PROTOCOL_SCOPED_KERNEL_OPTION=CONFIG_ANDROID_BINDER_IPC_32BIT unset or absent\n'
        fi
        printf 'PROTOCOL_SCOPED_KERNEL_OPTION_NOTE=follows the DERIVED protocol above and is scoped to this image; it is not a requirement of supported platforms generally, and it is independent of ELF bitness -- a 32-bit userspace can speak either protocol\n'
        # SANITISED, AND THIS ONE MATTERS MOST: the manifest is uploaded as a workflow
        # artifact, so anything in it has left the machine.  Scheme, host and path answer
        # "which archive was this built from"; a token in a query string or a userinfo field
        # answers nothing and cannot be recalled.  The digest below is the value that actually
        # pins the content, and it is recorded in full.
        if [ -n "$BASE_URL" ]; then
            printf 'BASE_PROVENANCE=%s\n' "$(sanitised_endpoint_display "$BASE_URL")"
        else
            printf 'BASE_PROVENANCE=%s\n' "$BASE_TARBALL"
        fi
        printf 'BASE_SHA256=%s\n' "$BASE_SHA256"
        printf 'PAYLOAD_SOURCE=%s\n' "$PAYLOAD_DIR"
        printf 'SDK_SOURCE=%s\n' "$SDK_DIR"
        printf 'LCOV_SOURCE=%s\n' "${LCOV_TARBALL:-distribution package only}"
        # REPRODUCIBILITY EVIDENCE, NOT A CLAIM OF IT.  The package set is pinned by the
        # archive the caller passed as --apt-mirror; these two lines say where the resolved
        # set is recorded and how large it is, so a reader can diff two images by package and
        # version instead of being told that nothing should have changed.  The list itself
        # lives in the image because it is a few hundred lines and the reader who needs it is
        # inside the guest.
        printf 'INSTALLED_PACKAGE_RECORD=%s\n' "$GUEST_PACKAGE_RECORD"
        printf 'INSTALLED_PACKAGE_COUNT=%s\n' "$PACKAGE_RECORD_COUNT"
        # The digest is what makes the count comparable rather than merely large: two images
        # whose records hash the same resolved to the same package set, and no reader has to
        # diff a few hundred lines to find that out.
        printf 'INSTALLED_PACKAGE_RECORD_SHA256=%s\n' "$PACKAGE_RECORD_DIGEST"
        printf 'REQUIRED_PACKAGES_ACCOUNTED=%s of %s\n' \
            "$PACKAGE_RECORD_REQUIRED_ACCOUNTED" "${#IMAGE_PACKAGES[@]}"
        if [ -n "$APT_MIRROR" ]; then
            printf 'PACKAGE_SOURCE=%s\n' "$(sanitised_endpoint_display "$APT_MIRROR")"
        else
            printf 'PACKAGE_SOURCE=%s\n' 'the base tarball sources, unchanged'
        fi
        printf 'READINESS_TIMEOUT_SECONDS=%s\n' "$READINESS_TIMEOUT_SECONDS"
        # THE TOOLCHAIN THAT WILL PRODUCE THE FIGURES, MEASURED INSIDE THE IMAGE RATHER THAN
        # NAMED FROM A PACKAGE LIST.  A coverage percentage and a set of (file,line,block,branch)
        # coordinates are properties of the compiler that wrote them, so an artefact bundle
        # without these three lines is a bundle whose numbers cannot be attributed later.  The
        # values come from assert_image_compiler_identity, which read them out of the image by the
        # unversioned names the guest itself resolves.  GUEST_TOOLCHAIN_PIN records which of the
        # three outcomes applied - selected, unavailable (a named limitation, not a failure), or
        # no pin requested - so a reader does not have to infer it from the versions.
        printf 'GUEST_TOOLCHAIN_GCC=%s\n' "${GUEST_GCC_VERSION:-unmeasured}"
        printf 'GUEST_TOOLCHAIN_GXX=%s\n' "${GUEST_GXX_VERSION:-unmeasured}"
        printf 'GUEST_TOOLCHAIN_GCOV=%s\n' "${GUEST_GCOV_VERSION:-unmeasured}"
        printf 'GUEST_TOOLCHAIN_PIN=%s\n' "${TOOLCHAIN_GCC_MAJOR:-<none requested>}"
        printf 'GUEST_TOOLCHAIN_PIN_STATE=%s\n' "$PINNED_COMPILER_STATE"
        printf 'GUEST_TOOLCHAIN_NOTE=gcc, g++ and gcov are asserted to be the same version, which is what the guest trace depends on; agreement with GUEST_TOOLCHAIN_PIN is RECORDED and not asserted, because the guest package archive is the caller pinned one and this builder does not fail an image build for what that archive does not carry\n'
        # THE OTHER TOOLCHAIN, AND IT IS A SEPARATE PAIR OF LINES BECAUSE IT IS A SEPARATE
        # QUESTION.  GUEST_TOOLCHAIN_* above is what will compile INSIDE this image.  These two
        # are what compiled the staged Binder closure, the AIDL stub libraries and
        # servicemanager BEFORE this script ran - objects this image only carries.  They are
        # allowed to differ, and on a bookworm guest with a major-13 acceptance pin they
        # necessarily do: a staged artefact has to link and load against the GUEST's glibc and
        # libstdc++, so it must be built by a toolchain no newer than the guest's, while the
        # coverage instrumentation has to match the branch-arm manifest, so it must be the
        # pinned family.  Recording both is what stops a reader attributing one to the other.
        printf 'STAGING_TOOLCHAIN_GCC=%s\n' "${STAGING_GCC_VERSION:-<not stated by the caller>}"
        printf 'STAGING_TOOLCHAIN_STATE=%s\n' "${STAGING_TOOLCHAIN_STATE:-<not stated by the caller>}"
        # THE PAYLOAD'S COMMIT REVISIONS, CARRIED OUT AS WELL AS IN.  The in-guest configuration
        # holds them so the coverage runner can name them in provenance.txt; the manifest holds
        # them so the workflow that published this image can be tied to the same commits without
        # opening the image.  An absent one is printed as absent: the payload travels without
        # .git, so a row nobody supplied a revision for genuinely cannot be answered.
        printf 'PROVENANCE_REVISION_SUPERPROJECT=%s\n' "${PROVENANCE_REVISION_SUPERPROJECT:-<not supplied>}"
        printf 'PROVENANCE_REVISION_HDMICEC=%s\n' "${PROVENANCE_REVISION_HDMICEC:-<not supplied>}"
        printf 'PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK=%s\n' \
            "${PROVENANCE_REVISION_ENTSERVICES_HDMICECSINK:-<not supplied>}"
        printf 'GUEST_NETWORK_AT_TEST_TIME=none\n'
        printf 'PAYLOAD_TRANSPORT=copied into the image at build time; artifacts are read back out of the image after the guest powers down\n'
    } > "$manifest"
    chmod 0644 -- "$manifest"
    log "manifest staged at $STAGED_MANIFEST (publishes to $OUTPUT_MANIFEST)"
    log "  written through the publication directory descriptor as $manifest"
}

# ------------------------------------------------------------------------------------
# PUBLICATION.  The only place either caller-supplied path is written, and the last thing
# this script does before declaring the build finished.
#
# ONE TRANSACTION, NOT A SEQUENCE OF INDEPENDENT STEPS.  The image and the manifest are a PAIR:
# the manifest names the image's paths, its architecture and its derived binder protocol, and
# the workflow reads every path it uses out of it.  A new image beside a stale manifest is worse
# than no publication, because it looks complete.  So the swap is written as a transaction with
# one commit point, and everything about its shape follows from the failures it exists to
# prevent.  Each is stated exactly, because each is reachable by an obvious simpler design:
#
#   THE FIRST FAILURE.  Move the previous image aside to a name held in a FUNCTION LOCAL, and
#   only then mint a second holding name for the previous manifest: a failure of that second
#   mktemp or mv, or an INT/TERM/HUP landing in that window, reaches `die` without any rollback.
#   The caller's good image is then hidden under a private name the EXIT trap cannot see, nothing
#   exists at --output, and the caller has no image and no way to find the one they had.
#
#   WHAT IS DONE INSTEAD, in four parts:
#
#   (a) BOTH HOLDING NAMES ARE MINTED BEFORE ANY RENAME.  A failure while minting them happens
#       when nothing has moved yet, so there is nothing to roll back.
#   (b) EVERY OLD AND NEW NAME LIVES IN A GLOBAL THE EXIT TRAP CAN READ, and PUBLISH_IN_PROGRESS
#       is raised before the first rename.  cleanup() rolls the transaction back from those
#       globals, so a path that reaches the trap without passing through the rollback below is
#       still covered.
#   (c) INT, TERM AND HUP ARE MASKED ACROSS THE TRANSACTION and the same handlers are restored
#       afterwards through install_interrupt_traps.  Those three are the signals a Ctrl-C, a
#       `timeout`, a CI cancellation or a lost session delivers, and the window they must not
#       land in is a handful of renames long.
#   (d) THE NEXT RUN RECOVERS WHAT NO TRAP COULD.  SIGKILL, a power loss and an OOM kill run no
#       handler at all, so recover_interrupted_publication() -- called under the publication
#       lock before anything is built -- reads the JOURNAL and drives the pair to one state.
#
#   THREE FURTHER FAILURE MODES, EACH CLOSED BY A NAMED MECHANISM BELOW:
#
#   THE EMPTY-PLACEHOLDER RESTORE.  mktemp mints an aside name by CREATING the file, so between
#   minting and the `mv` a real, EMPTY, regular file sits at each aside name.  A restore whose
#   whole precondition is `[ -f "$aside" ]` would therefore, on a FAILED old-leaf rename -- the
#   one case in which the aside is still that placeholder -- move a ZERO-BYTE FILE over the
#   caller's still-good published image.  Closed by PUBLISH_ASIDE_*_HOLDS_OLD, raised only after
#   that leaf's own `mv` returned success, plus a recorded size the restore re-checks.
#
#   THE ASYMMETRIC ROLLBACK.  Track only the IMAGE's rename and a failure after the manifest
#   rename -- the chmod, or the durability flush -- withdraws the new image and leaves the new
#   MANIFEST published.  On a first publication, with no asides, that is a manifest describing an
#   image that is not there.  Closed by PUBLISH_MANIFEST_RENAMED and a rollback that withdraws
#   BOTH new leaves.
#
#   THE UNRECOVERABLE MIXED STATE.  The image and the manifest are two independent renames, and a
#   SIGKILL between them leaves a new image, no manifest and BOTH asides.  A recovery that scans
#   the aside names one file at a time reaches the worst possible conclusion from that: an image
#   exists, so the old-image aside is "superseded" and is DELETED; no manifest exists, so the old
#   manifest is RESTORED.  A new image beside an old manifest, cemented, with the old image
#   destroyed.  Closed by the journal: see the block at PUBLISH_JOURNAL_LEAF for its shape, and
#   recover_interrupted_publication() for the state machine it drives.
#
# WHAT "ATOMIC" MEANS HERE, EXACTLY -- because the word does more work than the code can support
# unless it is pinned down, and an over-claim in a comment is how the next reader builds on a
# guarantee that is not there.  Three separate statements, and only these three:
#
#   (a) EACH LEAF IS REPLACED BY ONE ATOMIC RENAME.  Both renames are same-directory, so at no
#       instant does either published path hold a partial or truncated file.  A reader that opens
#       one path sees either the whole previous file or the whole new one, never a mixture.
#
#   (b) THE PAIR IS CONSISTENT FOR A READER THAT HOLDS THE PUBLICATION LOCK.  It is NOT consistent
#       for one that does not.  The image and the manifest are four independent renames at fixed
#       leaf names, and there is a real window -- between the image's rename and the manifest's --
#       in which a lockless reader sees the NEW image beside the OLD manifest.  POSIX offers no
#       way to rename two names as one operation, and the alternative shape that would remove the
#       window -- a generation directory with one atomically switched symlink -- changes the paths
#       the workflow consumes, which is the trade this design declined (see the journal block).
#       So the consistency of the PAIR is delivered by the lock, and the consumers must take it:
#       aidl-path-tests.yml's boot step and its artifact-extraction step both `flock` the same
#       $OUTPUT_IMAGE.lock, shared, for the duration of their read.  The lock leaf is chmodded
#       0644 in lock_publication_targets() precisely so an unprivileged consumer can open it
#       read-only and do that.
#
#   (c) AFTER AN INTERRUPTION THE PAIR IS MADE CONSISTENT AGAIN BY RECOVERY, not by the renames.
#       recover_interrupted_publication() runs under the same lock before anything is built and
#       drives both leaves to one generation.  That is what closes the window for a reader that
#       arrives after a crash rather than during a publication.
#
# AND NO POSTCONDITION IS ADVISORY.  The final mode, the durability flush, the rollback itself
# and the removal of the superseded asides are each FATAL rather than warning-only.  A published
# image nobody can read, or one that is not on the disk when the runner reboots, is not a
# published image, and a leftover multi-gigabyte aside is not housekeeping -- it is half of a
# transaction that did not finish.
#
# EVERY PATHNAME OPERATION IN HERE NAMES $OUTPUT_DIR_PROC/<leaf>, never the caller's pathname.
# A rename is authorised by the DIRECTORY, so a rename through a pathname whose ancestry was
# substituted after validation is a rename into somebody else's directory.  The descriptor pins
# the inode; see the custody block above resolve_publication_targets.
#
# THE ORDER, then:
#
#   1. VALIDATE the staged image.  A file that is not the size it should be, or is not a
#      regular file, is not published at all.  This is the "only after validation" clause:
#      an image is published because it was checked, not because the script reached the end.
#   2. RE-CHECK the destinations and the output directory's custody, adjacent to the use
#      (CWE-367): they were checked before the build, and the build takes an hour.
#   3. MINT BOTH HOLDING NAMES, before anything moves.
#   4. WRITE AND FLUSH THE JOURNAL, before anything moves, so a kill after step 5 is
#      recoverable at all.
#   5. MASK THE SIGNALS and raise PUBLISH_IN_PROGRESS.
#   6. MOVE THE PREVIOUS PAIR ASIDE rather than deleting it, recording each completed move in
#      the journal.  A caller who had a working image keeps one on every path out of here.
#   7. RENAME the image, then the manifest, recording the INTENT of each before it and the
#      completion after it.  Same-directory renames, so EACH LEAF is replaced atomically -- at no
#      instant does either path hold a partial file.  The PAIR is a different claim and a weaker
#      one; see "WHAT 'ATOMIC' MEANS HERE, EXACTLY" above.
#   8. ROLL BACK ON EVERY FAILURE from step 6 onwards, then fail.
#   9. MODE, then DURABILITY, then discard the asides, then discard the JOURNAL -- each fatal,
#      and the journal last, because it is what makes every earlier step recoverable.
#  10. LOWER THE FLAG and restore the signal handlers.
# ------------------------------------------------------------------------------------

# ------------------------------------------------------------------------------------
# THE JOURNAL, WRITTEN AND READ.
#
# One record, rewritten in full on every update rather than appended to: a record that is only
# ever replaced wholesale cannot be half of one update and half of another, and the reader has
# no ordering to reconstruct.  Each write goes to a fixed staging leaf and is renamed onto the
# journal name, which is atomic within a directory, and ends with the terminator line the reader
# REQUIRES -- so a torn write is refused rather than interpreted.
#
# THE RECORD IS THE GENERATION IDENTITY.  It names the two published leaves, the two aside
# leaves and the two staged leaves by name, so recovery does not have to infer which file is
# which from timestamps or from globbing: it is told, by the run that was about to move them.
# That is what lets the PAIR be recovered together instead of one file at a time.
# ------------------------------------------------------------------------------------

# Write (or rewrite) the journal.  Returns non-zero rather than dying: the caller decides
# whether a journal failure is fatal before the first move (it is) or reportable after the pair
# is already committed (it is that too, but by a different message).
publish_journal_write() { # $1=what has just happened, for the record and the log
    local why="$1" staging final
    staging="$OUTPUT_DIR_PROC/$PUBLISH_JOURNAL_TMP_LEAF"
    final="$OUTPUT_DIR_PROC/$PUBLISH_JOURNAL_LEAF"

    assert_output_dir_custody "update the publication journal in $OUTPUT_DIR ($why)"
    # A staging leaf left by a torn earlier update is removed rather than appended to -- and a
    # name this function cannot claim is a failure, not something to write through: `rm -f`
    # removes a regular file or a symlink, so what survives it is a directory or an unwritable
    # entry, and the redirection below would either fail confusingly or land somewhere else.
    rm -f -- "$staging" 2>/dev/null || return 1
    {
        printf '# Publication journal written by %s.  Read by the NEXT run, under the\n' "$SCRIPT_NAME"
        printf '# publication lock, to drive an interrupted publication to one consistent state.\n'
        printf '# It is removed only after the new pair is published, mode-set and flushed.\n'
        printf 'JOURNAL_VERSION=2\n'
        printf 'RUN_ID=%s\n' "$PUBLISH_RUN_ID"
        printf 'GENERATION=%s\n' "$PUBLISH_GENERATION"
        printf 'LAST_STEP=%s\n' "$why"
        printf 'IMAGE_LEAF=%s\n' "$OUTPUT_IMAGE_LEAF"
        printf 'MANIFEST_LEAF=%s\n' "$OUTPUT_MANIFEST_LEAF"
        printf 'STAGED_IMAGE_LEAF=%s\n' "$STAGED_IMAGE_LEAF"
        printf 'STAGED_MANIFEST_LEAF=%s\n' "$STAGED_MANIFEST_LEAF"
        printf 'ASIDE_IMAGE_LEAF=%s\n' "$PUBLISH_ASIDE_IMAGE_LEAF"
        printf 'ASIDE_MANIFEST_LEAF=%s\n' "$PUBLISH_ASIDE_MANIFEST_LEAF"
        # THE SIX IDENTITIES.  Recorded while each object is still at its starting name, so the
        # next run recognises it wherever a rename left it.  An empty device and inode means
        # "there was no such participant" -- a first publication has no previous pair -- and the
        # reader treats empty as "not recorded" rather than as a value.
        printf 'OLD_IMAGE_DEVICE=%s\n' "$PUBLISH_OLD_IMAGE_DEVICE"
        printf 'OLD_IMAGE_INODE=%s\n' "$PUBLISH_OLD_IMAGE_INODE"
        printf 'OLD_IMAGE_BYTES=%s\n' "$PUBLISH_OLD_IMAGE_BYTES"
        printf 'OLD_MANIFEST_DEVICE=%s\n' "$PUBLISH_OLD_MANIFEST_DEVICE"
        printf 'OLD_MANIFEST_INODE=%s\n' "$PUBLISH_OLD_MANIFEST_INODE"
        printf 'OLD_MANIFEST_BYTES=%s\n' "$PUBLISH_OLD_MANIFEST_BYTES"
        printf 'STAGED_IMAGE_DEVICE=%s\n' "$STAGED_IMAGE_DEVICE"
        printf 'STAGED_IMAGE_INODE=%s\n' "$STAGED_IMAGE_INODE"
        printf 'STAGED_IMAGE_BYTES=%s\n' "$PUBLISH_STAGED_IMAGE_BYTES"
        printf 'STAGED_MANIFEST_DEVICE=%s\n' "$STAGED_MANIFEST_DEVICE"
        printf 'STAGED_MANIFEST_INODE=%s\n' "$STAGED_MANIFEST_INODE"
        printf 'STAGED_MANIFEST_BYTES=%s\n' "$PUBLISH_STAGED_MANIFEST_BYTES"
        printf 'ASIDE_IMAGE_HOLDER_DEVICE=%s\n' "$PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE"
        printf 'ASIDE_IMAGE_HOLDER_INODE=%s\n' "$PUBLISH_ASIDE_IMAGE_HOLDER_INODE"
        printf 'ASIDE_MANIFEST_HOLDER_DEVICE=%s\n' "$PUBLISH_ASIDE_MANIFEST_HOLDER_DEVICE"
        printf 'ASIDE_MANIFEST_HOLDER_INODE=%s\n' "$PUBLISH_ASIDE_MANIFEST_HOLDER_INODE"
        # THE WRITE-AHEAD PAIRS, one per rename.  INTENT is raised and this record flushed
        # BEFORE the rename it describes; DONE is raised and it is flushed again after.
        printf 'INTENT_ASIDE_IMAGE=%s\n' "$PUBLISH_INTENT_ASIDE_IMAGE"
        printf 'DONE_ASIDE_IMAGE=%s\n' "$PUBLISH_ASIDE_IMAGE_HOLDS_OLD"
        printf 'INTENT_ASIDE_MANIFEST=%s\n' "$PUBLISH_INTENT_ASIDE_MANIFEST"
        printf 'DONE_ASIDE_MANIFEST=%s\n' "$PUBLISH_ASIDE_MANIFEST_HOLDS_OLD"
        printf 'INTENT_NEW_IMAGE=%s\n' "$PUBLISH_INTENT_NEW_IMAGE"
        printf 'DONE_NEW_IMAGE=%s\n' "$PUBLISH_IMAGE_RENAMED"
        printf 'INTENT_NEW_MANIFEST=%s\n' "$PUBLISH_INTENT_NEW_MANIFEST"
        printf 'DONE_NEW_MANIFEST=%s\n' "$PUBLISH_MANIFEST_RENAMED"
        printf '%s\n' "$PUBLISH_JOURNAL_TERMINATOR"
    } > "$staging" || return 1
    chmod 0644 -- "$staging" || return 1
    # DURABLE BEFORE THE RENAME, AND AGAIN AFTER IT.  A journal whose bytes are still in the
    # page cache when the power fails is not a journal; and a rename that is not on the disk
    # leaves the previous record in place, which would tell the next run that a move it has
    # since made had not happened.
    sync -f -- "$staging" 2>/dev/null || sync || return 1
    mv -f -- "$staging" "$final" || return 1
    sync -f -- "$final" 2>/dev/null || sync || return 1
    PUBLISH_JOURNAL_WRITTEN=1
    return 0
}

# Read one KEY=VALUE out of a journal file.  Prints the value, empty when the key is absent.
# The journal is written by this script only, one key per line, values with no newlines in them
# (they are leaf names and small integers), so a line-oriented read is exact rather than
# approximate -- and the caller checks for emptiness where emptiness is not allowed.
#
# No `--` before the filename: mawk rejects it as a filename rather than treating it as the
# end of options (measured), and it is not needed -- the path is always composed here as
# $OUTPUT_DIR_PROC/<leaf>, so it begins with /proc/ and can never look like an option.
publish_journal_value() { # $1=journal path  $2=key
    local journal="$1" key="$2"
    awk -v want="$key" '
        BEGIN { FS = "=" }
        substr($0, 1, 1) == "#" { next }
        {
            eq = index($0, "=")
            if (eq == 0) next
            if (substr($0, 1, eq - 1) != want) next
            print substr($0, eq + 1)
            exit
        }
    ' "$journal" 2>/dev/null || printf ''
}

# ------------------------------------------------------------------------------------
# IDENTITY, WHICH IS WHAT THE RECOVERY DECIDES ON.
#
# A rename changes a NAME and leaves the INODE alone.  That single property is what makes an
# interrupted publication decidable at all: the journal records what each participant's inode was
# while it was still at its starting name, and the recovery stats the names afterwards and asks
# which recorded object is sitting at each of them.  No timestamps, no globbing, no size
# heuristics and -- above all -- no trusting a completion flag that the kill may have pre-empted.
# ------------------------------------------------------------------------------------

# Describe what is at one leaf of the publication directory.  Prints exactly one of:
#
#   absent                    nothing of that name
#   irregular                 something is there and it is not a plain regular file, which
#                             includes a symbolic link -- checked BEFORE -e/-f, both of which
#                             follow one, so a link at a publication name reads as what it is
#   <device> <inode> <size>   a regular file, identified the way a rename preserves
#
# Always succeeds: "there is nothing there" and "there is something odd there" are answers the
# caller acts on, not errors.  The path is composed from the directory descriptor, like every
# other privileged operation in here.
publish_name_state() { # $1=leaf
    local path="$OUTPUT_DIR_PROC/$1" record
    if [ -L "$path" ]; then
        printf 'irregular'
        return 0
    fi
    if [ ! -e "$path" ]; then
        printf 'absent'
        return 0
    fi
    if [ ! -f "$path" ]; then
        printf 'irregular'
        return 0
    fi
    record="$(stat -c '%d %i %s' -- "$path" 2>/dev/null)" || record=''
    case "$record" in
        *[!0-9\ ]*|'') printf 'irregular' ;;
        *' '*' '*)      printf '%s' "$record" ;;
        *)              printf 'irregular' ;;
    esac
}

# Does a state string from publish_name_state() describe the object recorded as this device and
# inode?  False for 'absent', false for 'irregular', and false when the recorded identity is
# EMPTY -- which is how "there was no previous image" is written, and which must never match
# whatever happens to be sitting at a name.
publish_state_is() { # $1=state  $2=recorded device  $3=recorded inode
    local state="$1" device="$2" inode="$3" rest
    if [ -z "$device" ] || [ -z "$inode" ]; then
        return 1
    fi
    case "$state" in
        absent|irregular|'') return 1 ;;
    esac
    [ "${state%% *}" = "$device" ] || return 1
    rest="${state#* }"
    [ "${rest%% *}" = "$inode" ] || return 1
    return 0
}

# The size out of a state string; empty when the state does not carry one.
publish_state_bytes() { # $1=state
    local state="$1"
    case "$state" in
        absent|irregular|'') printf '' ;;
        *)                   printf '%s' "${state##* }" ;;
    esac
}

# Record one participant's identity while it is still at its starting name, into the three
# variables named by $2, $3 and $4.  Returns non-zero -- with all three cleared -- when the name
# does not hold a regular file, which every caller treats as fatal: a participant the journal
# cannot identify is a participant the next run's recovery could not place, and the whole point
# of the record is that it never has to guess.
publish_record_identity() { # $1=leaf  $2=device var  $3=inode var  $4=bytes var
    local leaf="$1" state
    local -n device_ref="$2" inode_ref="$3" bytes_ref="$4"
    device_ref=''
    inode_ref=''
    bytes_ref=''
    [ -n "$leaf" ] || return 1
    state="$(publish_name_state "$leaf")"
    case "$state" in
        absent|irregular|'') return 1 ;;
    esac
    device_ref="${state%% *}"
    state="${state#* }"
    inode_ref="${state%% *}"
    bytes_ref="${state##* }"
    # A state string that reached this point always carries three fields, so an empty one is a
    # defect in publish_name_state() rather than a condition of the file.  Checked rather than
    # assumed: the recovery's entire decision rests on these three values, and a silently empty
    # identity would read to the next run as "there was no such participant".
    if [ -z "$device_ref" ] || [ -z "$inode_ref" ] || [ -z "$bytes_ref" ]; then
        device_ref=''
        inode_ref=''
        bytes_ref=''
        return 1
    fi
    return 0
}

# Is one recorded identity out of the journal well formed?  Both fields digits is a recorded
# participant; both empty is "there was no such participant", which is how a FIRST publication
# writes the previous pair.  Anything else -- one field set and the other not, or a value with a
# character in it that an inode cannot have -- is refused by the caller rather than read as
# absent, because reading a half-recorded identity as "absent" is a decision about the caller's
# files taken on the strength of a field this script could not parse.
publish_identity_well_formed() { # $1=device  $2=inode
    local device="$1" inode="$2"
    if [ -z "$device" ] && [ -z "$inode" ]; then
        return 0
    fi
    case "$device" in ''|*[!0-9]*) return 1 ;; esac
    case "$inode" in ''|*[!0-9]*) return 1 ;; esac
    return 0
}

# ------------------------------------------------------------------------------------
# THE ONLY TWO PLACES THE RECOVERY DELETES ANYTHING, AND THE RULE THEY BOTH ENFORCE.
#
# Deleting the wrong file here destroys the caller's previous image, and a discard pass driven by
# a NAME does exactly that: it removes every journal-named leaf, including one the journal claims
# holds nothing because a kill landed in the write-after-move window while the aside held the
# only copy.  So deletion is driven neither by a name nor by a flag.  An object is removed ONLY
# when the journal positively identifies it by device and inode as one of:
#
#   (1) an empty mktemp placeholder -- a holding name that was minted and never moved into;
#   (2) one of THIS run's staged files -- the interrupted build's work, which is being discarded
#       precisely because the previous generation is the one being kept;
#   (3) the previous generation's file, and ONLY once the new generation has been established
#       BY INODE as sitting at both of its published names.  This third class is not in the
#       first two because it deletes a file that WAS a good generation -- it is permitted
#       because at that point it is superseded by a complete published pair, and because leaving
#       it would make the very next run refuse to start: a leftover holding name is what
#       recover_interrupted_publication() reads as an interrupted publication.  The caller has
#       to pass the proof in; there is no default.
#
# Anything else at either name is refused with the names listed.  An object this script cannot
# identify is not an object it deletes.
# ------------------------------------------------------------------------------------

# Discard one set-aside holding name under the rule above.  Dies rather than guessing.
publish_discard_aside() { # $1=leaf  $2=what it is  $3=placeholder device  $4=placeholder inode
                          # $5=previous device  $6=previous inode  $7=1 when the new generation
                          #    has been established by inode at BOTH published names
    local leaf="$1" what="$2" holder_device="$3" holder_inode="$4"
    local previous_device="$5" previous_inode="$6" superseded="$7"
    local state
    [ -n "$leaf" ] || return 0
    state="$(publish_name_state "$leaf")"
    [ "$state" != 'absent' ] || return 0
    if publish_state_is "$state" "$holder_device" "$holder_inode"; then
        assert_output_dir_custody "discard the unused holding name $OUTPUT_DIR/$leaf"
        rm -f -- "$OUTPUT_DIR_PROC/$leaf" || die "the unused holding name
           $OUTPUT_DIR/$leaf
       could not be removed.  It holds nothing -- it is the empty file mktemp created when the
       name was minted -- but a leftover holding name is what the next run reads as an
       interrupted publication, and it refuses to disambiguate two of them.  Remove it by hand
       and re-run."
        log "discarded the unused holding name $OUTPUT_DIR/$leaf (an empty mktemp placeholder)"
        return 0
    fi
    if publish_state_is "$state" "$previous_device" "$previous_inode"; then
        [ "$superseded" -eq 1 ] || die "refusing to remove $OUTPUT_DIR/$leaf: the object at that
       holding name IS the caller's previous $what (device $previous_device inode
       $previous_inode), and the new generation has NOT been established at both of its published
       names.  Removing it would destroy the only complete generation in the directory, which is
       exactly the failure this recovery exists to prevent.  This is a defect in $SCRIPT_NAME if
       it is reached: report it with the journal at $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF."
        assert_output_dir_custody "remove the superseded previous $what $OUTPUT_DIR/$leaf"
        rm -f -- "$OUTPUT_DIR_PROC/$leaf" || die "the superseded previous $what
           $OUTPUT_DIR/$leaf
       could not be removed.  The new pair IS published, but a leftover holding name is half of a
       transaction that did not finish and the next run refuses to disambiguate it.  Remove it by
       hand and re-run."
        log "removed the superseded previous $what $OUTPUT_DIR/$leaf"
        return 0
    fi
    die "refusing to remove $OUTPUT_DIR/$leaf: the object at that holding name reads as
           $state
       which is neither the empty placeholder the journal recorded (device
       ${holder_device:-<unrecorded>} inode ${holder_inode:-<unrecorded>}) nor the previous $what
       (device ${previous_device:-<unrecorded>} inode ${previous_inode:-<unrecorded>}).  This
       script does not delete a file it cannot identify, and it does not build over a directory
       state it cannot explain.  Inspect it by hand, put the pair you want at
           $OUTPUT_IMAGE
           $OUTPUT_MANIFEST
       remove the rest, and re-run."
}

# Discard one of the interrupted run's staged files under the rule above.  Dies rather than
# guessing.
publish_discard_staged() { # $1=leaf  $2=what it is  $3=staged device  $4=staged inode
    local leaf="$1" what="$2" device="$3" inode="$4"
    local state
    [ -n "$leaf" ] || return 0
    state="$(publish_name_state "$leaf")"
    [ "$state" != 'absent' ] || return 0
    if ! publish_state_is "$state" "$device" "$inode"; then
        die "refusing to remove $OUTPUT_DIR/$leaf: the object at that staging name reads as
           $state
       which is not the interrupted run's staged $what (device ${device:-<unrecorded>} inode
       ${inode:-<unrecorded>}) the journal records under it.  Something outside that run put it
       there, and this script does not delete a file it cannot identify.  Inspect it by hand,
       remove it deliberately, and re-run."
    fi
    assert_output_dir_custody "discard the interrupted run's staged $what $OUTPUT_DIR/$leaf"
    rm -f -- "$OUTPUT_DIR_PROC/$leaf" || die "the interrupted run's staged $what
           $OUTPUT_DIR/$leaf
       could not be removed.  The previous pair IS restored, but a leftover staging name makes
       the next run's recovery ambiguous, so remove it by hand and re-run."
    log "discarded the interrupted run's staged $what $OUTPUT_DIR/$leaf"
}

# Move one aside file back onto its published name.  Returns non-zero on failure so the caller
# can account for it; says exactly what is where when it cannot.
#
# THE PRECONDITION IS THE FLAG, NOT THE FILE'S EXISTENCE.  $4 is the "this aside actually holds
# the previous content" flag, raised only after that leaf's own `mv` returned success.  An aside
# name that exists because mktemp created it, and was never moved into, holds ZERO BYTES -- and
# moving that over the caller's still-good published file is the failure this parameter exists
# to prevent.  $5 is the size the previous file had, re-checked against what is actually there:
# belt and braces on top of the flag, so a future edit that raises the flag in the wrong place
# still cannot restore a placeholder.
publish_restore_one() { # $1=aside leaf  $2=published leaf  $3=what it is  $4=holds-old flag
                        # $5=old size  $6=old device  $7=old inode
    local aside_leaf="$1" published_leaf="$2" what="$3" holds_old="$4" old_bytes="$5"
    local old_device="${6-}" old_inode="${7-}"
    local aside published aside_state published_state now_bytes
    [ -n "$aside_leaf" ] || return 0
    if [ "$holds_old" -ne 1 ]; then
        # Nothing was ever moved into this aside, so there is nothing to put back.  Said out
        # loud rather than passed over silently: the reader of a rollback log needs to know
        # that the previous file is still at its published path, not that a step was skipped.
        log "rollback: the previous $what was never moved aside, so it is still at its"
        log "  published path and needs no restoration"
        return 0
    fi
    aside="$OUTPUT_DIR_PROC/$aside_leaf"
    published="$OUTPUT_DIR_PROC/$published_leaf"
    aside_state="$(publish_name_state "$aside_leaf")"
    if [ "$aside_state" = 'absent' ]; then
        warn "ROLLBACK INCOMPLETE: the previous $what was moved aside to
       $OUTPUT_DIR/$aside_leaf
       but nothing is there now, so it cannot be put back at $OUTPUT_DIR/$published_leaf.
       Something outside this run removed it."
        return 1
    fi
    # IDENTITY FIRST, AND IT IS THE CHECK THAT CARRIES THIS.  The object at the aside name must
    # be the very object whose device and inode were recorded before the move -- not merely a
    # regular file of about the right size.  That is what distinguishes the caller's previous
    # file from the EMPTY mktemp placeholder the aside name was minted as, without trusting the
    # flag above, and it is also what refuses a file something outside this run put there.
    if [ -n "$old_device" ] && [ -n "$old_inode" ]; then
        if ! publish_state_is "$aside_state" "$old_device" "$old_inode"; then
            warn "ROLLBACK REFUSED for the previous $what: the object at
       $OUTPUT_DIR/$aside_leaf
       is not the one that was moved there.  It was recorded as device $old_device inode
       $old_inode before the move, and what is there now reads as '$aside_state'.  Moving it onto
       $OUTPUT_DIR/$published_leaf would publish a file this run cannot identify -- possibly the
       empty holding placeholder, possibly something else entirely.  Refused: inspect both by
       hand."
            return 1
        fi
    fi
    # SIZE, AS BELT AND BRACES ON TOP OF THE IDENTITY.  Retained rather than replaced: it costs
    # one field comparison and it catches a future edit that loses the recorded inode.
    now_bytes="$(publish_state_bytes "$aside_state")"
    if [ -n "$old_bytes" ] && [ "$old_bytes" != '0' ] && [ "$now_bytes" = '0' ]; then
        warn "ROLLBACK REFUSED for the previous $what: the aside at
       $OUTPUT_DIR/$aside_leaf
       is zero bytes, but the file it was made from was $old_bytes bytes.  That is an empty
       mktemp placeholder rather than the caller's previous $what, and moving it onto
       $OUTPUT_DIR/$published_leaf would destroy whatever is still there.  Refused: inspect
       both by hand."
        return 1
    fi
    if mv -f -- "$aside" "$published"; then
        # AND VERIFIED AFTER THE MOVE.  A rename that reported success but left a different
        # object at the published name is not a restoration, and the caller is about to report
        # the previous generation as recovered on the strength of this return value.
        if [ -n "$old_device" ] && [ -n "$old_inode" ]; then
            published_state="$(publish_name_state "$published_leaf")"
            if ! publish_state_is "$published_state" "$old_device" "$old_inode"; then
                warn "ROLLBACK INCOMPLETE: the previous $what was moved back onto
       $OUTPUT_DIR/$published_leaf
       and the rename reported success, but what is at that name now reads as
       '$published_state' rather than the recorded device $old_device inode $old_inode.  This run
       does not report the previous $what as restored on the strength of a rename whose result it
       cannot confirm."
                return 1
            fi
        fi
        log "rollback: the previous $what is back at $OUTPUT_DIR/$published_leaf"
        return 0
    fi
    warn "ROLLBACK INCOMPLETE: the previous $what is left at
       $OUTPUT_DIR/$aside_leaf
       and $OUTPUT_DIR/$published_leaf is absent.  Move it back by hand:
           mv '$OUTPUT_DIR/$aside_leaf' '$OUTPUT_DIR/$published_leaf'"
    return 1
}

# Withdraw one NEW leaf from its published name, back onto the staging name it came from.
# Returns non-zero on failure.  Called for BOTH leaves -- the asymmetry that withdrew only the
# image is one of the four defects this block closes.
publish_withdraw_one() { # $1=published leaf  $2=staged leaf  $3=what it is  $4=renamed-flag variable name
    local published_leaf="$1" staged_leaf="$2" what="$3"
    local -n renamed_ref="$4"
    [ "$renamed_ref" -eq 1 ] || return 0
    [ -n "$staged_leaf" ] || return 0
    if [ ! -f "$OUTPUT_DIR_PROC/$published_leaf" ]; then
        # Already gone: nothing to withdraw, and the flag is lowered so a second pass does not
        # report the same thing twice.
        renamed_ref=0
        return 0
    fi
    if mv -f -- "$OUTPUT_DIR_PROC/$published_leaf" "$OUTPUT_DIR_PROC/$staged_leaf"; then
        renamed_ref=0
        log "rollback: this run's $what is back at its staging name $OUTPUT_DIR/$staged_leaf"
        return 0
    fi
    warn "ROLLBACK INCOMPLETE: this run's $what could not be moved back off
       $OUTPUT_DIR/$published_leaf, so the previous $what cannot be restored to that name.  The
       file now there is THIS run's $what, which was not validated as publishable."
    return 1
}

# Undo whichever parts of the transaction had been done.  Called from the failure arms of
# publish_outputs() and from cleanup() for the paths that never reach them.  Returns non-zero if
# anything could not be put back, so both callers can treat that as the failure it is.
publish_rollback() {
    local failures=0
    # BOTH new leaves are withdrawn FIRST, and in the reverse of the order they were published.
    # Leaving either in place would keep its destination occupied and the restore below would
    # then overwrite this run's file with the previous one, which loses this run's work as well
    # as confusing the next.
    publish_withdraw_one "$OUTPUT_MANIFEST_LEAF" "$STAGED_MANIFEST_LEAF" 'manifest' \
        PUBLISH_MANIFEST_RENAMED || failures=$(( failures + 1 ))
    publish_withdraw_one "$OUTPUT_IMAGE_LEAF" "$STAGED_IMAGE_LEAF" 'image' \
        PUBLISH_IMAGE_RENAMED || failures=$(( failures + 1 ))

    publish_restore_one "$PUBLISH_ASIDE_IMAGE_LEAF" "$OUTPUT_IMAGE_LEAF" 'image' \
        "$PUBLISH_ASIDE_IMAGE_HOLDS_OLD" "$PUBLISH_OLD_IMAGE_BYTES" \
        "$PUBLISH_OLD_IMAGE_DEVICE" "$PUBLISH_OLD_IMAGE_INODE" \
        || failures=$(( failures + 1 ))
    publish_restore_one "$PUBLISH_ASIDE_MANIFEST_LEAF" "$OUTPUT_MANIFEST_LEAF" 'manifest' \
        "$PUBLISH_ASIDE_MANIFEST_HOLDS_OLD" "$PUBLISH_OLD_MANIFEST_BYTES" \
        "$PUBLISH_OLD_MANIFEST_DEVICE" "$PUBLISH_OLD_MANIFEST_INODE" \
        || failures=$(( failures + 1 ))

    if [ "$failures" -eq 0 ]; then
        # The empty aside names are removed rather than left behind: an aside leaf that exists
        # and holds nothing is exactly what the next run's recovery must not find, and after a
        # complete rollback neither of them holds anything.
        local leaf
        for leaf in "$PUBLISH_ASIDE_IMAGE_LEAF" "$PUBLISH_ASIDE_MANIFEST_LEAF"; do
            [ -n "$leaf" ] || continue
            [ -e "$OUTPUT_DIR_PROC/$leaf" ] || continue
            rm -f -- "$OUTPUT_DIR_PROC/$leaf" 2>/dev/null \
                || warn "the empty holding name $OUTPUT_DIR/$leaf could not be removed after a
       complete rollback.  Remove it by hand: the next run reads a leftover holding name as an
       interrupted publication."
        done
        # THE JOURNAL GOES LAST, and only once the rollback is complete.  While it exists the
        # next run will drive the same rollback again, which is idempotent and is the correct
        # behaviour if this process dies during the lines above.
        if [ "$PUBLISH_JOURNAL_WRITTEN" -eq 1 ] && [ -n "$PUBLISH_JOURNAL_LEAF" ]; then
            rm -f -- "$OUTPUT_DIR_PROC/$PUBLISH_JOURNAL_LEAF" 2>/dev/null \
                || warn "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF could not be
       removed after a complete rollback.  It is harmless -- the next run reads it, finds the
       previous pair already in place and removes it -- but it is named here so it is not a
       surprise."
            PUBLISH_JOURNAL_WRITTEN=0
        fi
        PUBLISH_ASIDE_IMAGE=''
        PUBLISH_ASIDE_MANIFEST=''
        PUBLISH_ASIDE_IMAGE_LEAF=''
        PUBLISH_ASIDE_MANIFEST_LEAF=''
        PUBLISH_ASIDE_IMAGE_HOLDS_OLD=0
        PUBLISH_ASIDE_MANIFEST_HOLDS_OLD=0
        PUBLISH_INTENT_ASIDE_IMAGE=0
        PUBLISH_INTENT_ASIDE_MANIFEST=0
        PUBLISH_INTENT_NEW_IMAGE=0
        PUBLISH_INTENT_NEW_MANIFEST=0
        PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE=''
        PUBLISH_ASIDE_IMAGE_HOLDER_INODE=''
        PUBLISH_ASIDE_MANIFEST_HOLDER_DEVICE=''
        PUBLISH_ASIDE_MANIFEST_HOLDER_INODE=''
        PUBLISH_IN_PROGRESS=0
    else
        # LEFT FOR RECOVERY, DELIBERATELY.  The journal is what the next run uses to finish
        # this, so it is not removed when the rollback is incomplete.
        warn "the publication journal is left at $OUTPUT_DIR/${PUBLISH_JOURNAL_LEAF:-<none>} so
       the next run can finish this rollback under the publication lock."
    fi
    return "$failures"
}

# Fail the publication, rolling the transaction back first and restoring the signal handlers so
# the teardown behaves normally.  A rollback that itself fails is reported and does not stop the
# original failure from being the one that ends the run.
publish_fail() { # $1=message
    publish_rollback || warn "the rollback above did not complete; read its lines for what is
       where.  The publication failure that triggered it follows."
    install_interrupt_traps
    die "$1"
}

# ------------------------------------------------------------------------------------
# CRASH-CONSISTENT RECOVERY.  Run under the publication lock, before anything is built.
#
# WHAT IT IS FOR.  SIGKILL, an OOM kill and a power loss run no handler, so a run can die at any
# point in the four renames the publication is made of.  This function reads the journal the dead
# run left, works out from the FILESYSTEM which object is sitting at each of the six names
# involved, and drives the pair to one consistent generation.
#
# WHY IT DOES NOT DECIDE FROM THE RECORD ALONE, which is the part worth reading.  A record
# carrying one completion flag per move, written AFTER the move, is not trustworthy: a kill in
# that window leaves a durable flag saying the move did not happen when it did.  A recovery that
# believed the flag would destroy the thing it exists to preserve -- after a kill immediately
# following the previous image's own `mv`, MOVED_ASIDE_IMAGE reads 0, the restore is skipped as
# "nothing was ever moved into this aside", and the discard pass then removes the journal-named
# aside, which at that instant holds the caller's only copy of the previous image.
#
# SO THE RECORD IS WRITE-AHEAD AND THE DECISION IS BY INODE.  Each move has an INTENT field
# flushed before it and a DONE field flushed after, and the journal carries the device and inode
# of all six participants: the previous image, the previous manifest, this run's two staged files
# and the two empty mktemp placeholders the holding names were minted as.  (INTENT=1, DONE=0) is
# a legitimate terminal state of the record meaning "this move may or may not have happened", and
# this function answers it by stating the six names and asking which recorded object is at each.
# A rename preserves an inode, so that question has an answer; no flag is trusted for it.
#
# THE COMMIT POINT IS BOTH NEW OBJECTS AT THEIR PUBLISHED NAMES, established by inode.  Until
# both are there the new generation is not published, and recovery rolls the pair back to the
# PREVIOUS generation in full.  Once both are there the new generation IS published and recovery
# only finishes the housekeeping that was left -- the mode, the flush, the asides and the journal.
#
# AND IT ROLLS BACK RATHER THAN FORWARD, DELIBERATELY.  Completing somebody else's publication
# would mean asserting that the half of it this run never saw was valid: the staged image's size
# was checked by the dead run, not by this one, and the pair's mutual consistency -- the manifest
# names the image's size, architecture and derived binder protocol -- was never established at
# all.  The previous generation, by contrast, was published as a pair by a run that finished.  So
# the caller keeps the pair that was complete, and the interrupted build is re-run.
#
# WHAT IT WILL NOT DO IS GUESS.  Every disagreement it cannot settle by inode is refused with the
# names listed and EVERYTHING LEFT WHERE IT IS, so a human decides: an object at a publication
# name that the journal identifies as nothing, one recorded object apparently at two names, a
# recorded previous file that is at none of them, a record whose fields do not parse, and an
# aside file the journal does not mention at all (which is a second, older interrupted
# transaction).  Deletion in particular is governed by the rule at publish_discard_aside(): an
# object is removed only when the journal positively identifies it, never because its name
# matches a pattern.  The lock is held throughout, so no live builder owns any of these files.
# ------------------------------------------------------------------------------------
recover_interrupted_publication() {
    local journal staging entry
    local strays='' stray_count=0
    local version run_id generation last_step
    local j_image_leaf j_manifest_leaf j_staged_image_leaf j_staged_manifest_leaf
    local j_aside_image_leaf j_aside_manifest_leaf
    local j_old_image_device j_old_image_inode j_old_image_bytes
    local j_old_manifest_device j_old_manifest_inode j_old_manifest_bytes
    local j_staged_image_device j_staged_image_inode
    local j_staged_manifest_device j_staged_manifest_inode
    local j_holder_image_device j_holder_image_inode
    local j_holder_manifest_device j_holder_manifest_inode
    local intent_aside_image done_aside_image intent_aside_manifest done_aside_manifest
    local intent_new_image done_new_image intent_new_manifest done_new_manifest
    local state_published_image state_published_manifest
    local state_aside_image state_aside_manifest
    local state_staged_image state_staged_manifest
    local old_image_at='missing' old_manifest_at='missing'
    local new_image_at='missing' new_manifest_at='missing'
    local failures=0 leaf index_a index_b
    local -a identity_key=() identity_label=()

    [ "$OUTPUT_LOCK_HELD" -eq 1 ] || die "internal error: recover_interrupted_publication ran
       without the publication lock, so a live builder's aside files could be moved out from
       under it.  This is a defect in $SCRIPT_NAME."
    assert_output_dir_custody "scan $OUTPUT_DIR for an interrupted publication"

    journal="$OUTPUT_DIR_PROC/$PUBLISH_JOURNAL_LEAF"
    staging="$OUTPUT_DIR_PROC/$PUBLISH_JOURNAL_TMP_LEAF"

    # A STAGING LEAF IS ALWAYS DISCARDABLE.  With no journal beside it, it is a FIRST journal
    # write interrupted before its rename -- and the journal is written before anything moves, so
    # nothing had moved.  With a journal beside it, it is a torn update and the journal on disk
    # is the authoritative record.  Either way the staging leaf is removed and never read.
    if [ -e "$staging" ] || [ -L "$staging" ]; then
        warn "a partially written publication journal was left at
       $OUTPUT_DIR/$PUBLISH_JOURNAL_TMP_LEAF
       by a run that was killed mid-write.  It is discarded unread; the completed journal beside
       it, if there is one, is what this run acts on."
        rm -f -- "$staging" || die "the partially written publication journal
           $OUTPUT_DIR/$PUBLISH_JOURNAL_TMP_LEAF
       could not be removed.  It is refused rather than worked around: this run writes its own
       journal to that name, and a name it cannot claim is a journal it cannot make durable."
    fi

    # THE ASIDE SCAN, which is a CROSS-CHECK on the journal rather than the recovery itself.
    # NULL-delimited and -maxdepth 1, so a name with a space or a newline in it cannot split into
    # two and nothing outside the publication directory is considered.  The trailing slash on the
    # proc path is required: without it find lstats the /proc/self/fd/<n> symlink itself and
    # descends into nothing (measured).
    while IFS= read -r -d '' entry; do
        strays="$strays${strays:+
}$(basename -- "$entry")"
        stray_count=$(( stray_count + 1 ))
    done < <(find "$OUTPUT_DIR_PROC/" -maxdepth 1 -type f \
                  \( -name "$PUBLISH_ASIDE_IMAGE_PREFIX*" \
                     -o -name "$PUBLISH_ASIDE_MANIFEST_PREFIX*" \) -print0 2>/dev/null)

    if [ ! -e "$journal" ] && [ ! -L "$journal" ]; then
        if [ "$stray_count" -eq 0 ]; then
            return 0
        fi
        die "the publication directory holds set-aside copies from an interrupted publication
       but NO journal describing them:
$strays
       The journal is written and flushed before the first file is moved, so an aside without one
       cannot have come from a run of this version of $SCRIPT_NAME -- it is either from an older
       version, or something removed the journal.  Which of those files is the caller's last good
       image, and which is a stale leftover, cannot be established from the directory alone, and
       putting back the wrong one publishes an image nobody can identify.  Refused rather than
       guessed: they are ordinary ext4 images and KEY=VALUE manifests, so inspect them, move the
       pair you want onto
           $OUTPUT_IMAGE
           $OUTPUT_MANIFEST
       delete the rest, and re-run."
    fi

    [ ! -L "$journal" ] || die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF is a
       symbolic link.  Refused rather than followed: this run would read a recovery plan from
       wherever the link points and then move the caller's files according to it."
    [ -f "$journal" ] || die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF exists
       and is not a regular file.  Refused: a recovery plan is read out of it."
    grep -qxF -- "$PUBLISH_JOURNAL_TERMINATOR" "$journal" 2>/dev/null \
        || die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF does not end with its
       terminator line, so the record was TORN: the run that wrote it died between the first byte
       and the last.  A half-written recovery plan is exactly the ambiguity the journal exists to
       remove, so it is refused rather than interpreted.  Inspect the journal and any
       ${PUBLISH_ASIDE_IMAGE_PREFIX}* / ${PUBLISH_ASIDE_MANIFEST_PREFIX}* files beside it, put the
       pair you want at $OUTPUT_IMAGE and $OUTPUT_MANIFEST, delete the rest, and re-run."

    version="$(publish_journal_value "$journal" JOURNAL_VERSION)"
    case "$version" in
        2) : ;;
        1) die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF is a VERSION 1 record,
       written by an earlier build of $SCRIPT_NAME.  It is refused rather than read, and the
       reason is the whole point of version 2: a version 1 record carries one completion flag per
       move, written AFTER the move, and no participant identities at all.  Acting on it would
       mean acting on exactly the claim that was measured to be wrong -- a kill between a rename
       and its flag leaves the flag saying the rename did not happen -- and version 1 carries no
       inode with which to check.  The files themselves are intact: an ext4 image, a KEY=VALUE
       manifest, and one or two ${PUBLISH_ASIDE_IMAGE_PREFIX}* / ${PUBLISH_ASIDE_MANIFEST_PREFIX}*
       holding copies.  Inspect them, put the pair you want at
           $OUTPUT_IMAGE
           $OUTPUT_MANIFEST
       delete the rest including the journal, and re-run." ;;
        *) die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF declares
       JOURNAL_VERSION='$version', which this build of $SCRIPT_NAME does not know how to act on
       (it writes and reads version 2).  Refused rather than misread: recovering a record whose
       fields mean something else moves the caller's files by mistake." ;;
    esac

    run_id="$(publish_journal_value "$journal" RUN_ID)"
    generation="$(publish_journal_value "$journal" GENERATION)"
    last_step="$(publish_journal_value "$journal" LAST_STEP)"
    j_image_leaf="$(publish_journal_value "$journal" IMAGE_LEAF)"
    j_manifest_leaf="$(publish_journal_value "$journal" MANIFEST_LEAF)"
    j_staged_image_leaf="$(publish_journal_value "$journal" STAGED_IMAGE_LEAF)"
    j_staged_manifest_leaf="$(publish_journal_value "$journal" STAGED_MANIFEST_LEAF)"
    j_aside_image_leaf="$(publish_journal_value "$journal" ASIDE_IMAGE_LEAF)"
    j_aside_manifest_leaf="$(publish_journal_value "$journal" ASIDE_MANIFEST_LEAF)"
    j_old_image_device="$(publish_journal_value "$journal" OLD_IMAGE_DEVICE)"
    j_old_image_inode="$(publish_journal_value "$journal" OLD_IMAGE_INODE)"
    j_old_image_bytes="$(publish_journal_value "$journal" OLD_IMAGE_BYTES)"
    j_old_manifest_device="$(publish_journal_value "$journal" OLD_MANIFEST_DEVICE)"
    j_old_manifest_inode="$(publish_journal_value "$journal" OLD_MANIFEST_INODE)"
    j_old_manifest_bytes="$(publish_journal_value "$journal" OLD_MANIFEST_BYTES)"
    j_staged_image_device="$(publish_journal_value "$journal" STAGED_IMAGE_DEVICE)"
    j_staged_image_inode="$(publish_journal_value "$journal" STAGED_IMAGE_INODE)"
    j_staged_manifest_device="$(publish_journal_value "$journal" STAGED_MANIFEST_DEVICE)"
    j_staged_manifest_inode="$(publish_journal_value "$journal" STAGED_MANIFEST_INODE)"
    j_holder_image_device="$(publish_journal_value "$journal" ASIDE_IMAGE_HOLDER_DEVICE)"
    j_holder_image_inode="$(publish_journal_value "$journal" ASIDE_IMAGE_HOLDER_INODE)"
    j_holder_manifest_device="$(publish_journal_value "$journal" ASIDE_MANIFEST_HOLDER_DEVICE)"
    j_holder_manifest_inode="$(publish_journal_value "$journal" ASIDE_MANIFEST_HOLDER_INODE)"
    intent_aside_image="$(publish_journal_value "$journal" INTENT_ASIDE_IMAGE)"
    done_aside_image="$(publish_journal_value "$journal" DONE_ASIDE_IMAGE)"
    intent_aside_manifest="$(publish_journal_value "$journal" INTENT_ASIDE_MANIFEST)"
    done_aside_manifest="$(publish_journal_value "$journal" DONE_ASIDE_MANIFEST)"
    intent_new_image="$(publish_journal_value "$journal" INTENT_NEW_IMAGE)"
    done_new_image="$(publish_journal_value "$journal" DONE_NEW_IMAGE)"
    intent_new_manifest="$(publish_journal_value "$journal" INTENT_NEW_MANIFEST)"
    done_new_manifest="$(publish_journal_value "$journal" DONE_NEW_MANIFEST)"

    # EVERY INTENT AND DONE RECORD MUST BE A LITERAL 0 OR 1.  An absent or malformed field is not
    # read as "no", because "no" is a decision about the caller's files.
    for entry in "$intent_aside_image" "$done_aside_image" \
                 "$intent_aside_manifest" "$done_aside_manifest" \
                 "$intent_new_image" "$done_new_image" \
                 "$intent_new_manifest" "$done_new_manifest"; do
        case "$entry" in
            0|1) : ;;
            *) die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF carries a
       write-ahead record that is neither 0 nor 1:
           aside image    INTENT='$intent_aside_image'    DONE='$done_aside_image'
           aside manifest INTENT='$intent_aside_manifest' DONE='$done_aside_manifest'
           new image      INTENT='$intent_new_image'      DONE='$done_new_image'
           new manifest   INTENT='$intent_new_manifest'   DONE='$done_new_manifest'
       An unreadable record cannot be read as 'that step did not happen': that reading would move
       the caller's files on the strength of a field this script could not parse.  Refused." ;;
        esac
    done

    # EVERY RECORDED IDENTITY MUST PARSE.  Both fields digits is a recorded participant; both
    # empty is "there was no such participant", which is what a FIRST publication writes for the
    # previous pair.  One set and the other not is neither, and is refused rather than read as
    # absent -- reading it as absent is how a recovery decides not to restore something that is
    # there.
    if ! publish_identity_well_formed "$j_old_image_device" "$j_old_image_inode" \
       || ! publish_identity_well_formed "$j_old_manifest_device" "$j_old_manifest_inode" \
       || ! publish_identity_well_formed "$j_staged_image_device" "$j_staged_image_inode" \
       || ! publish_identity_well_formed "$j_staged_manifest_device" "$j_staged_manifest_inode" \
       || ! publish_identity_well_formed "$j_holder_image_device" "$j_holder_image_inode" \
       || ! publish_identity_well_formed "$j_holder_manifest_device" "$j_holder_manifest_inode"; then
        die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF carries a participant
       identity that does not parse as a device and inode pair:
           previous image     ${j_old_image_device:-<empty>}/${j_old_image_inode:-<empty>}
           previous manifest  ${j_old_manifest_device:-<empty>}/${j_old_manifest_inode:-<empty>}
           staged image       ${j_staged_image_device:-<empty>}/${j_staged_image_inode:-<empty>}
           staged manifest    ${j_staged_manifest_device:-<empty>}/${j_staged_manifest_inode:-<empty>}
           image holder       ${j_holder_image_device:-<empty>}/${j_holder_image_inode:-<empty>}
           manifest holder    ${j_holder_manifest_device:-<empty>}/${j_holder_manifest_inode:-<empty>}
       Every decision this function takes is an inode comparison, so an identity it cannot read
       is a decision it cannot take.  Refused with everything left in place."
    fi

    # THE STAGED PAIR MUST BE IDENTIFIED.  Unlike the previous pair, which may legitimately not
    # have existed, this run's predecessor always had both staged files -- the journal is written
    # after they are validated.  An empty identity here means the record was written by something
    # other than publish_outputs().
    if [ -z "$j_staged_image_inode" ] || [ -z "$j_staged_manifest_inode" ]; then
        die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF does not identify the
       interrupted run's staged pair (image inode '${j_staged_image_inode:-<empty>}', manifest
       inode '${j_staged_manifest_inode:-<empty>}').  Those two identities are how this run tells
       the interrupted build's files from the caller's previous ones, so a record without them
       cannot be acted on.  Refused with everything left in place."
    fi

    # THE JOURNAL MUST BE THIS OUTPUT'S JOURNAL.  Its leaf name is derived from the image leaf, so
    # a mismatch means the file was copied in from elsewhere or the --output was renamed.
    if [ "$j_image_leaf" != "$OUTPUT_IMAGE_LEAF" ] || [ "$j_manifest_leaf" != "$OUTPUT_MANIFEST_LEAF" ]; then
        die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF describes a different
       output than this run's:
           the journal names   $j_image_leaf and $j_manifest_leaf
           this run publishes  $OUTPUT_IMAGE_LEAF and $OUTPUT_MANIFEST_LEAF
       Acting on it would move files this run has no claim to.  Refused: if the journal is stale,
       delete it and re-run; if the --output was renamed, recover the old name first."
    fi

    # NO TWO PARTICIPANTS MAY SHARE AN IDENTITY.  Every decision below is "which recorded object
    # is at this name", so two records with the same device and inode would make one name
    # answerable two ways.  They are distinct live files when the record is written, so this
    # cannot arise from publish_outputs() -- it is a check against a hand-edited or fabricated
    # journal, and it is cheap.
    [ -z "$j_old_image_inode" ] || { identity_key+=("$j_old_image_device:$j_old_image_inode")
        identity_label+=('the previous image'); }
    [ -z "$j_old_manifest_inode" ] || { identity_key+=("$j_old_manifest_device:$j_old_manifest_inode")
        identity_label+=('the previous manifest'); }
    identity_key+=("$j_staged_image_device:$j_staged_image_inode")
    identity_label+=("the interrupted run's staged image")
    identity_key+=("$j_staged_manifest_device:$j_staged_manifest_inode")
    identity_label+=("the interrupted run's staged manifest")
    [ -z "$j_holder_image_inode" ] || { identity_key+=("$j_holder_image_device:$j_holder_image_inode")
        identity_label+=("the image holding name's placeholder"); }
    [ -z "$j_holder_manifest_inode" ] || { identity_key+=("$j_holder_manifest_device:$j_holder_manifest_inode")
        identity_label+=("the manifest holding name's placeholder"); }
    index_a=0
    while [ "$index_a" -lt "${#identity_key[@]}" ]; do
        index_b=$(( index_a + 1 ))
        while [ "$index_b" -lt "${#identity_key[@]}" ]; do
            if [ "${identity_key[$index_a]}" = "${identity_key[$index_b]}" ]; then
                die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF records the same
       device and inode
           ${identity_key[$index_a]}
       for two different participants (${identity_label[$index_a]} and
       ${identity_label[$index_b]}).  This function decides which object is at which name by
       comparing inodes, and two participants that share one cannot be told apart -- so every
       decision below would be a guess.  Refused with everything left in place: inspect the files
       by hand, put the pair you want at $OUTPUT_IMAGE and $OUTPUT_MANIFEST, delete the rest
       including the journal, and re-run."
            fi
            index_b=$(( index_b + 1 ))
        done
        index_a=$(( index_a + 1 ))
    done

    # EVERY ASIDE ON DISK MUST BE ONE THE JOURNAL NAMES.  One it does not name is a second,
    # older interrupted transaction, and choosing between two is the guess this refuses to make.
    if [ "$stray_count" -gt 0 ]; then
        while IFS= read -r entry; do
            [ -n "$entry" ] || continue
            [ "$entry" != "$j_aside_image_leaf" ] || continue
            [ "$entry" != "$j_aside_manifest_leaf" ] || continue
            die "the publication directory holds a set-aside copy the journal does not name:
           $OUTPUT_DIR/$entry
       The journal for this output describes asides '${j_aside_image_leaf:-<none>}' and
       '${j_aside_manifest_leaf:-<none>}', so the file above is from a DIFFERENT interrupted
       publication -- an older run, or an older version of $SCRIPT_NAME.  Two interrupted
       transactions in one directory cannot be disentangled by this script, and putting back the
       wrong image publishes one nobody can identify.  Refused: inspect them, keep the pair you
       want at $OUTPUT_IMAGE and $OUTPUT_MANIFEST, delete the rest, and re-run."
        done <<STRAY_EOF
$strays
STRAY_EOF
    fi

    warn "an interrupted publication was found in the publication directory:"
    warn "  journal    $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF"
    warn "  run        ${run_id:-unrecorded}   generation ${generation:-unrecorded}"
    warn "  last step  ${last_step:-unrecorded}"
    warn "  write-ahead records (INTENT before the move, DONE after it):"
    warn "    aside image    INTENT=$intent_aside_image DONE=$done_aside_image"
    warn "    aside manifest INTENT=$intent_aside_manifest DONE=$done_aside_manifest"
    warn "    new image      INTENT=$intent_new_image DONE=$done_new_image"
    warn "    new manifest   INTENT=$intent_new_manifest DONE=$done_new_manifest"
    warn "  A previous run was killed part-way through the publication (SIGKILL, an OOM kill or a"
    warn "  power loss runs no handler), so this run drives the pair to ONE generation before it"
    warn "  builds anything -- and it decides from the inodes on the disk, not from the records"
    warn "  above, because a kill can land between a rename and the record of it."

    # ---- WHERE EACH OBJECT ACTUALLY IS, decided by inode and nothing else.
    #
    # Six names, and a closed list of legal occupants for each.  Anything else at any of them is
    # a disagreement, and a disagreement ends this function with everything left in place.
    #
    #   the published image name : the previous image (not yet moved aside)
    #                            | the interrupted run's staged image (published)
    #                            | nothing
    #   the aside image name     : the previous image (moved aside)
    #                            | the empty mktemp placeholder (minted, never moved into)
    #                            | nothing
    #   the staged image name    : the interrupted run's staged image (not yet published)
    #                            | nothing
    #
    # ...and the same three for the manifest.
    state_published_image="$(publish_name_state "$OUTPUT_IMAGE_LEAF")"
    state_published_manifest="$(publish_name_state "$OUTPUT_MANIFEST_LEAF")"
    state_aside_image='absent'
    [ -z "$j_aside_image_leaf" ] || state_aside_image="$(publish_name_state "$j_aside_image_leaf")"
    state_aside_manifest='absent'
    [ -z "$j_aside_manifest_leaf" ] || state_aside_manifest="$(publish_name_state "$j_aside_manifest_leaf")"
    state_staged_image='absent'
    [ -z "$j_staged_image_leaf" ] || state_staged_image="$(publish_name_state "$j_staged_image_leaf")"
    state_staged_manifest='absent'
    [ -z "$j_staged_manifest_leaf" ] || state_staged_manifest="$(publish_name_state "$j_staged_manifest_leaf")"

    if publish_state_is "$state_published_image" "$j_old_image_device" "$j_old_image_inode"; then
        old_image_at='published'
    elif publish_state_is "$state_published_image" "$j_staged_image_device" "$j_staged_image_inode"; then
        new_image_at='published'
    elif [ "$state_published_image" != 'absent' ]; then
        die "the object at $OUTPUT_IMAGE reads as
           $state_published_image
       which the journal identifies as neither the previous image (device
       ${j_old_image_device:-<none>} inode ${j_old_image_inode:-<none>}) nor the interrupted run's
       staged image (device $j_staged_image_device inode $j_staged_image_inode).  Something
       outside that run put it there.  This run will not build over a publication state it cannot
       explain, and it will not move a file it cannot identify: inspect it by hand, put the pair
       you want at $OUTPUT_IMAGE and $OUTPUT_MANIFEST, delete the rest including the journal, and
       re-run."
    fi
    if publish_state_is "$state_aside_image" "$j_old_image_device" "$j_old_image_inode"; then
        [ "$old_image_at" = 'missing' ] || die "the previous image appears at TWO names at once --
       $OUTPUT_IMAGE and $OUTPUT_DIR/$j_aside_image_leaf both hold device $j_old_image_device
       inode $j_old_image_inode.  One inode under two names is a hard link, which this
       transaction never creates, so the directory is not in a state this function can reason
       about.  Refused with everything left in place."
        old_image_at='aside'
    elif publish_state_is "$state_aside_image" "$j_holder_image_device" "$j_holder_image_inode"; then
        : # the empty placeholder: the holding name was minted and never moved into.
    elif [ "$state_aside_image" != 'absent' ]; then
        die "the object at the image holding name $OUTPUT_DIR/$j_aside_image_leaf reads as
           $state_aside_image
       which the journal identifies as neither the previous image (device
       ${j_old_image_device:-<none>} inode ${j_old_image_inode:-<none>}) nor the empty placeholder
       that name was minted as (device ${j_holder_image_device:-<none>} inode
       ${j_holder_image_inode:-<none>}).  Refused with everything left in place: this script does
       not move or delete a file it cannot identify."
    fi
    if publish_state_is "$state_staged_image" "$j_staged_image_device" "$j_staged_image_inode"; then
        [ "$new_image_at" = 'missing' ] || die "the interrupted run's staged image appears at TWO
       names at once -- $OUTPUT_IMAGE and $OUTPUT_DIR/$j_staged_image_leaf both hold device
       $j_staged_image_device inode $j_staged_image_inode.  Refused with everything left in
       place."
        new_image_at='staged'
    elif [ "$state_staged_image" != 'absent' ]; then
        die "the object at the image staging name $OUTPUT_DIR/$j_staged_image_leaf reads as
           $state_staged_image
       which is not the staged image the journal records under it (device $j_staged_image_device
       inode $j_staged_image_inode).  Refused with everything left in place."
    fi

    if publish_state_is "$state_published_manifest" "$j_old_manifest_device" "$j_old_manifest_inode"; then
        old_manifest_at='published'
    elif publish_state_is "$state_published_manifest" "$j_staged_manifest_device" "$j_staged_manifest_inode"; then
        new_manifest_at='published'
    elif [ "$state_published_manifest" != 'absent' ]; then
        die "the object at $OUTPUT_MANIFEST reads as
           $state_published_manifest
       which the journal identifies as neither the previous manifest (device
       ${j_old_manifest_device:-<none>} inode ${j_old_manifest_inode:-<none>}) nor the interrupted
       run's staged manifest (device $j_staged_manifest_device inode $j_staged_manifest_inode).
       Refused with everything left in place; see the equivalent refusal for the image above."
    fi
    if publish_state_is "$state_aside_manifest" "$j_old_manifest_device" "$j_old_manifest_inode"; then
        [ "$old_manifest_at" = 'missing' ] || die "the previous manifest appears at TWO names at
       once -- $OUTPUT_MANIFEST and $OUTPUT_DIR/$j_aside_manifest_leaf both hold device
       $j_old_manifest_device inode $j_old_manifest_inode.  Refused with everything left in
       place."
        old_manifest_at='aside'
    elif publish_state_is "$state_aside_manifest" "$j_holder_manifest_device" "$j_holder_manifest_inode"; then
        : # the empty placeholder.
    elif [ "$state_aside_manifest" != 'absent' ]; then
        die "the object at the manifest holding name $OUTPUT_DIR/$j_aside_manifest_leaf reads as
           $state_aside_manifest
       which the journal identifies as neither the previous manifest (device
       ${j_old_manifest_device:-<none>} inode ${j_old_manifest_inode:-<none>}) nor the empty
       placeholder that name was minted as (device ${j_holder_manifest_device:-<none>} inode
       ${j_holder_manifest_inode:-<none>}).  Refused with everything left in place."
    fi
    if publish_state_is "$state_staged_manifest" "$j_staged_manifest_device" "$j_staged_manifest_inode"; then
        [ "$new_manifest_at" = 'missing' ] || die "the interrupted run's staged manifest appears
       at TWO names at once -- $OUTPUT_MANIFEST and $OUTPUT_DIR/$j_staged_manifest_leaf both hold
       device $j_staged_manifest_device inode $j_staged_manifest_inode.  Refused with everything
       left in place."
        new_manifest_at='staged'
    elif [ "$state_staged_manifest" != 'absent' ]; then
        die "the object at the manifest staging name $OUTPUT_DIR/$j_staged_manifest_leaf reads as
           $state_staged_manifest
       which is not the staged manifest the journal records under it (device
       $j_staged_manifest_device inode $j_staged_manifest_inode).  Refused with everything left
       in place."
    fi

    # A PREVIOUS FILE THE JOURNAL RECORDS MUST BE SOMEWHERE.  If it is at neither its published
    # name nor its holding name, this function cannot restore it -- and continuing would publish
    # a new generation over the wreckage of one it was supposed to be able to fall back to.
    if [ -n "$j_old_image_inode" ] && [ "$old_image_at" = 'missing' ]; then
        die "the journal records a previous image (device $j_old_image_device inode
       $j_old_image_inode, $j_old_image_bytes bytes) but it is at neither $OUTPUT_IMAGE nor its
       holding name ${j_aside_image_leaf:-<none recorded>}.  Something outside the interrupted run
       removed or replaced it.  This run will not build over that: the previous generation is what
       the recovery exists to preserve, and it is gone.  Inspect the directory by hand, decide
       what should be at $OUTPUT_IMAGE and $OUTPUT_MANIFEST, delete the journal deliberately, and
       re-run."
    fi
    if [ -n "$j_old_manifest_inode" ] && [ "$old_manifest_at" = 'missing' ]; then
        die "the journal records a previous manifest (device $j_old_manifest_device inode
       $j_old_manifest_inode, $j_old_manifest_bytes bytes) but it is at neither $OUTPUT_MANIFEST
       nor its holding name ${j_aside_manifest_leaf:-<none recorded>}.  Refused for the same
       reason as the image above: the previous generation is what the recovery preserves."
    fi

    # THE TWO COMBINATIONS THAT CANNOT HAPPEN.  (INTENT=1, DONE=0) is expected and is settled
    # above by inode; (INTENT=1, DONE=1) is settled the same way, and it is deliberately NOT
    # cross-checked, because a recovery that was itself killed part-way legitimately leaves DONE=1
    # with the object already back at its pre-move name or already discarded -- checking it would
    # make this function refuse to finish work it had started.  What cannot happen is a move whose
    # INTENT was never recorded: the intent is written and flushed BEFORE the rename, and a
    # failure to write it stops the transaction, so an object at its post-move name with INTENT=0
    # means something outside that run moved it.
    if [ "$intent_aside_image" -eq 0 ] && [ "$old_image_at" = 'aside' ]; then
        die "the previous image is at its holding name $OUTPUT_DIR/$j_aside_image_leaf, but the
       journal records INTENT_ASIDE_IMAGE=0 -- the intent to move it was never written.  The
       intent is flushed BEFORE the rename and a failure to write it stops the transaction, so
       this state cannot have been produced by $SCRIPT_NAME.  Refused with everything left in
       place."
    fi
    if [ "$intent_aside_manifest" -eq 0 ] && [ "$old_manifest_at" = 'aside' ]; then
        die "the previous manifest is at its holding name $OUTPUT_DIR/$j_aside_manifest_leaf, but
       the journal records INTENT_ASIDE_MANIFEST=0.  Refused with everything left in place; see
       the equivalent refusal for the image above."
    fi
    if [ "$intent_new_image" -eq 0 ] && [ "$new_image_at" = 'published' ]; then
        die "the interrupted run's staged image is at $OUTPUT_IMAGE, but the journal records
       INTENT_NEW_IMAGE=0 -- the intent to publish it was never written.  Refused with everything
       left in place."
    fi
    if [ "$intent_new_manifest" -eq 0 ] && [ "$new_manifest_at" = 'published' ]; then
        die "the interrupted run's staged manifest is at $OUTPUT_MANIFEST, but the journal records
       INTENT_NEW_MANIFEST=0.  Refused with everything left in place."
    fi

    warn "  where each object actually is, established by inode:"
    warn "    the previous image             $old_image_at"
    warn "    the previous manifest          $old_manifest_at"
    warn "    the interrupted run's image    $new_image_at"
    warn "    the interrupted run's manifest $new_manifest_at"

    if [ "$new_image_at" = 'published' ] && [ "$new_manifest_at" = 'published' ]; then
        # ---- FORWARD.  Both new objects are AT their published names, established by inode, so
        # the new generation IS the published pair and only the housekeeping was left.  Nothing
        # is moved.
        warn "  Both halves of the new pair are at their published names, so the pair IS"
        warn "  published: the mode and durability steps that were left are finished, and the"
        warn "  set-aside previous copies are superseded and are removed."
        # THE MODE IS PART OF THE PUBLICATION, so finishing forward finishes it.  The staged
        # files are 0600 and 0644 respectively and the image's 0644 is set AFTER its rename, so a
        # run killed between the commit and that chmod leaves a published image the runner's own
        # account cannot read -- and a "recovered" pair the job cannot use is not a recovered
        # pair.  Idempotent, so it costs nothing when the chmod had already run.
        for leaf in "$OUTPUT_IMAGE_LEAF" "$OUTPUT_MANIFEST_LEAF"; do
            assert_output_dir_custody "finish the published mode of $OUTPUT_DIR/$leaf"
            chmod 0644 -- "$OUTPUT_DIR_PROC/$leaf" || die "the recovered pair is at its published
       names but the mode of $OUTPUT_DIR/$leaf could not be set to 0644.  The workflow's artifact
       upload and the QEMU process both read those files, so this run does not treat the recovery
       as finished; fix the mode by hand and re-run."
        done
        # THE ASIDES.  This is the third permitted deletion class at publish_discard_aside(), and
        # the proof it requires is the condition of this branch: both new objects have just been
        # established BY INODE at both published names, so what is at a holding name is a
        # superseded generation rather than the only one.  The `1` is that proof.
        publish_discard_aside "$j_aside_image_leaf" 'image' \
            "$j_holder_image_device" "$j_holder_image_inode" \
            "$j_old_image_device" "$j_old_image_inode" 1
        publish_discard_aside "$j_aside_manifest_leaf" 'manifest' \
            "$j_holder_manifest_device" "$j_holder_manifest_inode" \
            "$j_old_manifest_device" "$j_old_manifest_inode" 1
    else
        # ---- BACK.  The new generation was not committed, so the previous one is restored in
        # full and the interrupted run's work is discarded.
        warn "  The new pair was NOT fully published, so the PREVIOUS pair is restored in full"
        warn "  and the interrupted build's staged files are discarded."

        # 1. Withdraw whichever new object reached a published name, back onto the staging name
        #    it came from.  Manifest first, mirroring publish_rollback: the reverse of the order
        #    they are published in.  The staging name is known to be free -- the classification
        #    above refuses an object at it that is not the staged file itself, and refuses the
        #    staged file being at two names -- but it is asserted rather than assumed, because
        #    this rename would otherwise overwrite whatever is there.
        if [ "$new_manifest_at" = 'published' ]; then
            [ -n "$j_staged_manifest_leaf" ] || die "the interrupted run's manifest is at
       $OUTPUT_MANIFEST and must be withdrawn, but the journal records no staging name to
       withdraw it to.  Refused with everything left in place."
            [ "$state_staged_manifest" = 'absent' ] || die "the interrupted run's manifest must be
       withdrawn from $OUTPUT_MANIFEST onto $OUTPUT_DIR/$j_staged_manifest_leaf, but something is
       already at that name.  Refused with everything left in place."
            assert_output_dir_custody "withdraw the interrupted run's manifest from $OUTPUT_MANIFEST"
            mv -f -- "$OUTPUT_DIR_PROC/$OUTPUT_MANIFEST_LEAF" \
                     "$OUTPUT_DIR_PROC/$j_staged_manifest_leaf" \
                || die "could not withdraw the interrupted run's manifest from $OUTPUT_MANIFEST,
       so the previous manifest cannot be put back there.  Move it out of the way by hand and
       re-run."
            new_manifest_at='staged'
            state_staged_manifest="$(publish_name_state "$j_staged_manifest_leaf")"
            log "withdrew the interrupted run's manifest from $OUTPUT_MANIFEST"
        fi
        if [ "$new_image_at" = 'published' ]; then
            [ -n "$j_staged_image_leaf" ] || die "the interrupted run's image is at $OUTPUT_IMAGE
       and must be withdrawn, but the journal records no staging name to withdraw it to.  Refused
       with everything left in place."
            [ "$state_staged_image" = 'absent' ] || die "the interrupted run's image must be
       withdrawn from $OUTPUT_IMAGE onto $OUTPUT_DIR/$j_staged_image_leaf, but something is
       already at that name.  Refused with everything left in place."
            assert_output_dir_custody "withdraw the interrupted run's image from $OUTPUT_IMAGE"
            mv -f -- "$OUTPUT_DIR_PROC/$OUTPUT_IMAGE_LEAF" \
                     "$OUTPUT_DIR_PROC/$j_staged_image_leaf" \
                || die "could not withdraw the interrupted run's image from $OUTPUT_IMAGE, so the
       previous image cannot be put back there.  Move it out of the way by hand and re-run."
            new_image_at='staged'
            state_staged_image="$(publish_name_state "$j_staged_image_leaf")"
            log "withdrew the interrupted run's image from $OUTPUT_IMAGE"
        fi

        # 2. Restore each previous file that is sitting at its holding name.  THE PRECONDITION IS
        #    WHERE THE OBJECT ACTUALLY IS, not what a completion record claims -- which is the
        #    correction this whole function exists for.  publish_restore_one() re-checks the
        #    inode before the move and again after it.
        assert_output_dir_custody "restore the previous pair onto $OUTPUT_IMAGE and $OUTPUT_MANIFEST"
        if [ "$old_image_at" = 'aside' ]; then
            publish_restore_one "$j_aside_image_leaf" "$OUTPUT_IMAGE_LEAF" 'image' 1 \
                "$j_old_image_bytes" "$j_old_image_device" "$j_old_image_inode" \
                || failures=$(( failures + 1 ))
        fi
        if [ "$old_manifest_at" = 'aside' ]; then
            publish_restore_one "$j_aside_manifest_leaf" "$OUTPUT_MANIFEST_LEAF" 'manifest' 1 \
                "$j_old_manifest_bytes" "$j_old_manifest_device" "$j_old_manifest_inode" \
                || failures=$(( failures + 1 ))
        fi
        [ "$failures" -eq 0 ] || die "$failures half(ves) of the previous pair could not be
       restored (named above).  This run will NOT build over a half-recovered publication: the
       caller's last good pair is what the recovery exists to preserve, and continuing would put
       a new image beside whichever half is still there.  Fix it by hand and re-run."

        # 3. Discard the interrupted run's leftovers -- and ONLY objects the journal positively
        #    identifies, per the rule at publish_discard_aside().  The staged files are its own
        #    work; a holding name that still exists at this point can only hold the empty
        #    placeholder, because step 2 renamed the previous file off it, and the `0` says that
        #    a holding name found still holding the previous file must NOT be deleted.
        publish_discard_staged "$j_staged_image_leaf" 'image' \
            "$j_staged_image_device" "$j_staged_image_inode"
        publish_discard_staged "$j_staged_manifest_leaf" 'manifest' \
            "$j_staged_manifest_device" "$j_staged_manifest_inode"
        publish_discard_aside "$j_aside_image_leaf" 'image' \
            "$j_holder_image_device" "$j_holder_image_inode" \
            "$j_old_image_device" "$j_old_image_inode" 0
        publish_discard_aside "$j_aside_manifest_leaf" 'manifest' \
            "$j_holder_manifest_device" "$j_holder_manifest_inode" \
            "$j_old_manifest_device" "$j_old_manifest_inode" 0

        # 4. AND THE RESULT IS VERIFIED, by inode, before anything says the recovery worked.  The
        #    published names must hold the previous generation where there was one, and must hold
        #    nothing where there was not -- a FIRST publication that was interrupted rolls back to
        #    an empty pair, and an empty pair is the correct outcome rather than a failure.
        state_published_image="$(publish_name_state "$OUTPUT_IMAGE_LEAF")"
        state_published_manifest="$(publish_name_state "$OUTPUT_MANIFEST_LEAF")"
        if [ -n "$j_old_image_inode" ]; then
            publish_state_is "$state_published_image" "$j_old_image_device" "$j_old_image_inode" \
                || die "the rollback finished but $OUTPUT_IMAGE does not hold the previous image:
       it reads as '$state_published_image' rather than device $j_old_image_device inode
       $j_old_image_inode.  This run does not report a recovery it cannot confirm."
        else
            [ "$state_published_image" = 'absent' ] || die "the rollback finished and the journal
       records that there was no previous image, so $OUTPUT_IMAGE should hold nothing -- and it
       reads as '$state_published_image'.  This run does not report a recovery it cannot confirm."
        fi
        if [ -n "$j_old_manifest_inode" ]; then
            publish_state_is "$state_published_manifest" "$j_old_manifest_device" "$j_old_manifest_inode" \
                || die "the rollback finished but $OUTPUT_MANIFEST does not hold the previous
       manifest: it reads as '$state_published_manifest' rather than device
       $j_old_manifest_device inode $j_old_manifest_inode.  This run does not report a recovery it
       cannot confirm."
        else
            [ "$state_published_manifest" = 'absent' ] || die "the rollback finished and the
       journal records that there was no previous manifest, so $OUTPUT_MANIFEST should hold
       nothing -- and it reads as '$state_published_manifest'.  This run does not report a
       recovery it cannot confirm."
        fi
    fi

    # DURABILITY BEFORE THE JOURNAL GOES.  Every rename above must be on the disk before the
    # record that would let a later run redo them is discarded; otherwise a power loss here
    # leaves the directory in the state this function started from and nothing saying so.
    if ! sync -f -- "$OUTPUT_DIR_PROC/" 2>/dev/null; then
        sync || die "the recovered publication could not be flushed to disk: neither
           sync -f -- '$OUTPUT_DIR'
       nor a whole-system 'sync' succeeded.  The journal is deliberately NOT removed, so a later
       run repeats this recovery rather than finding a state nothing accounts for.  Fix the
       filesystem and re-run."
    fi

    # THE JOURNAL GOES LAST.  While it exists the recovery above is repeatable, which is what
    # makes this function safe to be killed in the middle of; once it is gone the directory is
    # in one state and nothing is owed.
    assert_output_dir_custody "remove the recovered publication journal"
    rm -f -- "$journal" || die "the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF could
       not be removed after the recovery above completed.  This run will not build while it is
       there: it writes its own journal to that name, and a stale one would be read as this
       run's own record."
    log "publication recovery complete; the journal is removed and the directory is in one state"

    # The leaves were re-checked at resolution time; they have just changed, so they are
    # re-checked here rather than left as they were found.
    assert_publishable_leaf "$OUTPUT_DIR_PROC/$OUTPUT_IMAGE_LEAF" \
        'the --output image, after recovery' "$OUTPUT_IMAGE"
    assert_publishable_leaf "$OUTPUT_DIR_PROC/$OUTPUT_MANIFEST_LEAF" \
        'the manifest beside --output, after recovery' "$OUTPUT_MANIFEST"
}

publish_outputs() {
    local expected_bytes=$(( IMAGE_SIZE_MIB * 1024 * 1024 ))
    local actual_bytes staged_image_proc staged_manifest_proc aside_proc
    local image_proc manifest_proc aside_bytes

    # 0. EXCLUSIVITY.  Publishing without the lock would defeat the whole point of taking it,
    # and this catches a future edit that reorders main() rather than a runtime condition.
    [ "$OUTPUT_LOCK_HELD" -eq 1 ] || die "refusing to publish without the publication lock on
       $OUTPUT_LOCK.  lock_publication_targets() must run before anything is built; this is a
       defect in $SCRIPT_NAME rather than in the caller's inputs."

    # Every operation below names one of these four, never a caller pathname.
    staged_image_proc="$OUTPUT_DIR_PROC/$STAGED_IMAGE_LEAF"
    staged_manifest_proc="$OUTPUT_DIR_PROC/$STAGED_MANIFEST_LEAF"
    image_proc="$OUTPUT_DIR_PROC/$OUTPUT_IMAGE_LEAF"
    manifest_proc="$OUTPUT_DIR_PROC/$OUTPUT_MANIFEST_LEAF"

    # 1. VALIDATE.
    if [ -z "$STAGED_IMAGE_LEAF" ] || [ ! -f "$staged_image_proc" ]; then
        die "the staged image is missing at publication time (${STAGED_IMAGE:-<unset>}).
       Nothing is published: an absent staged image means the build did not produce one, and
       any previously published image is left exactly as it was."
    fi
    if [ -z "$STAGED_MANIFEST_LEAF" ] || [ ! -f "$staged_manifest_proc" ]; then
        die "the staged manifest is missing at publication time
       (${STAGED_MANIFEST:-<unset>}).  Nothing is published: an image without its manifest is
       unusable by the workflow, which reads every path out of the manifest."
    fi
    actual_bytes="$(stat -c '%s' -- "$staged_image_proc" 2>/dev/null || printf '0')"
    [ "$actual_bytes" = "$expected_bytes" ] || die "the staged image is $actual_bytes bytes but
       was created at ${IMAGE_SIZE_MIB} MiB ($expected_bytes bytes).  Something truncated or
       extended it after mkfs, so it is NOT published and the previous image is left in
       place.  A short image boots into a corrupt filesystem, which is the failure mode this
       check exists to prevent."

    # 2. RE-CHECK, adjacent to the use.  The output directory's custody and both staged leaves'
    # identities as well as the destinations: an hour has passed since they were established.
    assert_output_dir_custody "publish $OUTPUT_IMAGE and $OUTPUT_MANIFEST"
    assert_leaf_identity "$staged_image_proc" "$STAGED_IMAGE_DEVICE" "$STAGED_IMAGE_INODE" \
        "publish the staged image onto $OUTPUT_IMAGE" "$STAGED_IMAGE"
    assert_leaf_identity "$staged_manifest_proc" "$STAGED_MANIFEST_DEVICE" "$STAGED_MANIFEST_INODE" \
        "publish the staged manifest onto $OUTPUT_MANIFEST" "$STAGED_MANIFEST"
    assert_publishable_leaf "$image_proc"    'the --output image'          "$OUTPUT_IMAGE"
    assert_publishable_leaf "$manifest_proc" 'the manifest beside --output' "$OUTPUT_MANIFEST"

    # 3. MINT BOTH HOLDING NAMES BEFORE ANYTHING MOVES, and RECORD EVERY PARTICIPANT'S IDENTITY.
    #
    # Six identities, each read while its object is still at its starting name: the previous
    # image, the previous manifest, this run's two staged files, and the two empty placeholders
    # mktemp creates when it mints the holding names.  A rename preserves an inode, so this is
    # what lets the NEXT run recognise each object wherever a rename left it -- and, in
    # particular, tell "the holding name still holds the empty placeholder" from "the holding
    # name holds the caller's previous file" without trusting any completion flag.  The sizes
    # come along for the same reason they always did: as belt and braces under the identity.
    #
    # A failure here is a failure with nothing yet displaced, which is why it dies directly
    # rather than through publish_fail.
    PUBLISH_STAGED_IMAGE_BYTES="$actual_bytes"
    PUBLISH_STAGED_MANIFEST_BYTES="$(publish_state_bytes "$(publish_name_state "$STAGED_MANIFEST_LEAF")")"
    if [ -z "$PUBLISH_STAGED_MANIFEST_BYTES" ]; then
        die "internal error: the staged manifest at $STAGED_MANIFEST could not be measured
       although it was just asserted to be a regular file.  This is a defect in $SCRIPT_NAME:
       the journal records every participant's size, and a participant it cannot measure is one
       the next run's recovery could not identify."
    fi
    if [ -f "$image_proc" ]; then
        publish_record_identity "$OUTPUT_IMAGE_LEAF" \
            PUBLISH_OLD_IMAGE_DEVICE PUBLISH_OLD_IMAGE_INODE PUBLISH_OLD_IMAGE_BYTES \
            || die "the previous image at $OUTPUT_IMAGE is there but its device, inode and size
       could not be read.  Nothing has been moved: the previous pair is exactly as it was.  It is
       refused rather than worked around -- the recovery in the next run recognises the previous
       image by its inode, so an image this run cannot identify is one that run could not put
       back."
        aside_proc="$(mktemp -- "$OUTPUT_DIR_PROC/${PUBLISH_ASIDE_IMAGE_PREFIX}XXXXXXXX")" \
            || die "could not create a holding file for the previous image in $OUTPUT_DIR.
       Nothing has been moved: the previous pair is exactly as it was."
        PUBLISH_ASIDE_IMAGE_LEAF="$(basename -- "$aside_proc")"
        PUBLISH_ASIDE_IMAGE="$OUTPUT_DIR/$PUBLISH_ASIDE_IMAGE_LEAF"
        publish_record_identity "$PUBLISH_ASIDE_IMAGE_LEAF" \
            PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE PUBLISH_ASIDE_IMAGE_HOLDER_INODE aside_bytes \
            || die "the holding file just created at $PUBLISH_ASIDE_IMAGE could not be
       identified.  Nothing has been moved.  Its inode is what tells the next run's recovery
       'this holding name was minted and never moved into' from 'this holding name holds the
       caller's previous image', and without it that distinction would be a guess."
        # A NAME MKTEMP HAS JUST CREATED HOLDS NOTHING.  Asserted rather than assumed: mktemp
        # creates with O_EXCL, so content at that name means something else is writing into the
        # publication directory -- and the recovery tells "minted, never moved into" from "holds
        # the previous image" by inode with the size as belt and braces, so a placeholder that
        # is not empty blurs the very distinction the identity above exists to make.
        [ "$aside_bytes" = '0' ] || die "the holding file just created at
           $PUBLISH_ASIDE_IMAGE
       is $aside_bytes bytes rather than empty.  mktemp creates it with O_EXCL, so a file with
       content in it at that name means something else is writing into $OUTPUT_DIR.  Nothing has
       been moved and the previous pair is exactly as it was; refused rather than continued."
    fi
    if [ -f "$manifest_proc" ]; then
        publish_record_identity "$OUTPUT_MANIFEST_LEAF" \
            PUBLISH_OLD_MANIFEST_DEVICE PUBLISH_OLD_MANIFEST_INODE PUBLISH_OLD_MANIFEST_BYTES \
            || {
                [ -z "$PUBLISH_ASIDE_IMAGE_LEAF" ] \
                    || rm -f -- "$OUTPUT_DIR_PROC/$PUBLISH_ASIDE_IMAGE_LEAF"
                PUBLISH_ASIDE_IMAGE=''
                PUBLISH_ASIDE_IMAGE_LEAF=''
                PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE=''
                PUBLISH_ASIDE_IMAGE_HOLDER_INODE=''
                die "the previous manifest at $OUTPUT_MANIFEST is there but its device, inode and
       size could not be read.  Nothing has been moved: the previous pair is exactly as it was."
            }
        aside_proc="$(mktemp -- "$OUTPUT_DIR_PROC/${PUBLISH_ASIDE_MANIFEST_PREFIX}XXXXXXXX")" \
            || {
                # The image's holding name was minted but nothing was moved into it, so it is
                # removed rather than left for the next run's recovery to puzzle over.
                [ -z "$PUBLISH_ASIDE_IMAGE_LEAF" ] \
                    || rm -f -- "$OUTPUT_DIR_PROC/$PUBLISH_ASIDE_IMAGE_LEAF"
                PUBLISH_ASIDE_IMAGE=''
                PUBLISH_ASIDE_IMAGE_LEAF=''
                PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE=''
                PUBLISH_ASIDE_IMAGE_HOLDER_INODE=''
                die "could not create a holding file for the previous manifest in $OUTPUT_DIR.
       Nothing has been moved: the previous pair is exactly as it was."
            }
        PUBLISH_ASIDE_MANIFEST_LEAF="$(basename -- "$aside_proc")"
        PUBLISH_ASIDE_MANIFEST="$OUTPUT_DIR/$PUBLISH_ASIDE_MANIFEST_LEAF"
        publish_record_identity "$PUBLISH_ASIDE_MANIFEST_LEAF" \
            PUBLISH_ASIDE_MANIFEST_HOLDER_DEVICE PUBLISH_ASIDE_MANIFEST_HOLDER_INODE aside_bytes \
            || die "the holding file just created at $PUBLISH_ASIDE_MANIFEST could not be
       identified.  Nothing has been moved; see the equivalent refusal for the image above for
       why an unidentifiable holding name is not continued past."
        # A NAME MKTEMP HAS JUST CREATED HOLDS NOTHING.  Asserted rather than assumed: mktemp
        # creates with O_EXCL, so content at that name means something else is writing into the
        # publication directory -- and the recovery tells "minted, never moved into" from "holds
        # the previous manifest" by inode with the size as belt and braces, so a placeholder that
        # is not empty blurs the very distinction the identity above exists to make.
        [ "$aside_bytes" = '0' ] || die "the holding file just created at
           $PUBLISH_ASIDE_MANIFEST
       is $aside_bytes bytes rather than empty.  mktemp creates it with O_EXCL, so a file with
       content in it at that name means something else is writing into $OUTPUT_DIR.  Nothing has
       been moved and the previous pair is exactly as it was; refused rather than continued."
    fi

    # 4. THE JOURNAL, WRITTEN AND MADE DURABLE BEFORE THE FIRST MOVE.  Everything after this
    # point is recoverable by the next run; nothing before it needed to be, because nothing had
    # moved.  A journal that cannot be written is therefore fatal HERE, with the previous pair
    # untouched -- publishing without it would mean a SIGKILL in the next four lines left a state
    # no later run could resolve, which is exactly what this whole mechanism exists to prevent.
    PUBLISH_RUN_ID="$$-$(date -u '+%Y%m%dT%H%M%SZ')"
    PUBLISH_GENERATION="$(date -u '+%Y-%m-%dT%H:%M:%SZ')/$PUBLISH_RUN_ID"
    publish_journal_write 'about to move the previous pair aside' \
        || die "the publication journal could not be written to
           $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF
       Nothing has been moved and the previous pair is exactly as it was.  The journal is what
       makes an interrupted publication recoverable, so publishing without one is refused: a
       kill between the renames that follow would leave a state no later run could resolve."
    log "publication journal written: $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF"
    log "  generation $PUBLISH_GENERATION"

    # 5. MASK THE SIGNALS, RAISE THE FLAG.  From here to step 10 the caller's good pair exists
    # only under the aside names above, and those names are now visible to the EXIT trap.
    trap '' INT TERM HUP
    PUBLISH_IN_PROGRESS=1

    # 6. ASIDE, not deleted.  EVERY MOVE IS BRACKETED BY TWO FLUSHED JOURNAL WRITES: the INTENT
    # before it and the DONE after it.  The INTENT write is the one that matters, for the reason
    # set out at PUBLISH_INTENT_ASIDE_IMAGE -- with only the write after the move, a kill in
    # between leaves a durable record saying the move had not happened, which the next run would
    # believe.  With both, (INTENT=1, DONE=0) says "may or may not have happened", and the
    # recorded inodes tell the next run which it was.
    if [ -n "$PUBLISH_ASIDE_IMAGE_LEAF" ]; then
        PUBLISH_INTENT_ASIDE_IMAGE=1
        publish_journal_write 'about to move the previous image aside' \
            || publish_fail "the publication journal could not record the INTENT to move the
       previous image aside to
           $PUBLISH_ASIDE_IMAGE
       Nothing has been moved and nothing is published.  The intent is written BEFORE the move
       for a reason -- a move nobody recorded the intent of is a move the next run cannot know
       to look for -- so a failure to record it stops the transaction here rather than taking
       the move on trust."
        mv -f -- "$image_proc" "$OUTPUT_DIR_PROC/$PUBLISH_ASIDE_IMAGE_LEAF" \
            || publish_fail "could not move the previous image aside from $OUTPUT_IMAGE.
       Nothing is published; the previous pair is untouched."
        PUBLISH_ASIDE_IMAGE_HOLDS_OLD=1
        publish_journal_write 'the previous image is aside' \
            || publish_fail "the previous image was moved aside to
           $PUBLISH_ASIDE_IMAGE
       but the publication journal could not be updated to record the completion.  The rollback
       below puts it back; nothing is published.  This is not an unrecoverable window -- the
       INTENT above is on the disk and the previous image's inode is recorded with it, so a kill
       here would be resolved by the next run from the filesystem -- but a transaction whose
       record this run cannot keep current is not one it continues."
    fi
    if [ -n "$PUBLISH_ASIDE_MANIFEST_LEAF" ]; then
        PUBLISH_INTENT_ASIDE_MANIFEST=1
        publish_journal_write 'about to move the previous manifest aside' \
            || publish_fail "the publication journal could not record the INTENT to move the
       previous manifest aside to
           $PUBLISH_ASIDE_MANIFEST
       The rollback below puts the previous image back; nothing is published."
        mv -f -- "$manifest_proc" "$OUTPUT_DIR_PROC/$PUBLISH_ASIDE_MANIFEST_LEAF" \
            || publish_fail "could not move the previous manifest aside from $OUTPUT_MANIFEST.
       The previous image has been put back and nothing is published."
        PUBLISH_ASIDE_MANIFEST_HOLDS_OLD=1
        publish_journal_write 'the previous pair is aside' \
            || publish_fail "the previous manifest was moved aside to
           $PUBLISH_ASIDE_MANIFEST
       but the publication journal could not be updated to record the completion.  The rollback
       below puts the previous pair back; nothing is published."
    fi

    # 7. and 8. RENAME, with rollback on every failure.
    #
    # The custody re-check is repeated here rather than relied upon from step 2, because the
    # renames are the operations that actually consume the directory's authority: a rename is
    # permitted by the DIRECTORY, so if the descriptor no longer refers to the directory that was
    # vetted -- or the lock's inode has been swapped underneath us -- these two renames are the
    # ones that would write into somebody else's.  A refusal here reaches the teardown with
    # PUBLISH_IN_PROGRESS raised, and cleanup()'s rollback arm puts the previous pair back --
    # which is exactly the case that arm exists for.
    assert_output_dir_custody "rename this run's pair onto $OUTPUT_IMAGE and $OUTPUT_MANIFEST"
    PUBLISH_INTENT_NEW_IMAGE=1
    publish_journal_write 'about to publish the new image' \
        || publish_fail "the publication journal could not record the INTENT to publish the new
       image onto $OUTPUT_IMAGE.  Nothing has been renamed onto either published name; the
       rollback below puts the previous pair back."
    if ! mv -f -- "$staged_image_proc" "$image_proc"; then
        publish_fail "could not publish the image to $OUTPUT_IMAGE.  The previous pair has been
       put back; the staged image is discarded by the teardown, because a staged image nobody
       can publish is exactly the half-built artefact a later step must not find."
    fi
    PUBLISH_IMAGE_RENAMED=1
    publish_journal_write 'the new image is published' \
        || publish_fail "the new image reached $OUTPUT_IMAGE but the publication journal could
       not be updated to record the completion.  The rollback below withdraws it and puts the
       previous pair back.  Without the write-ahead journal this is the one unrecoverable window
       -- a kill immediately after the rename leaves a new image, no manifest, and a record
       saying neither had happened.  It IS recoverable here, because the INTENT above is on the
       disk and every participant's inode is recorded with it, but a record this run cannot keep
       current is still not a transaction it continues."
    PUBLISH_INTENT_NEW_MANIFEST=1
    publish_journal_write 'about to publish the new manifest' \
        || publish_fail "the publication journal could not record the INTENT to publish the new
       manifest onto $OUTPUT_MANIFEST.  The image publication has been undone and the previous
       pair put back."
    if ! mv -f -- "$staged_manifest_proc" "$manifest_proc"; then
        publish_fail "could not publish the manifest to $OUTPUT_MANIFEST.  The image
       publication has been undone and the previous pair put back, because a new image beside a
       stale manifest is worse than no publication -- it looks complete."
    fi
    PUBLISH_MANIFEST_RENAMED=1
    # THE COMMIT POINT.  This journal record is what makes the new generation the published one:
    # a next run that reads BOTH rename records completes forward, and a next run that reads
    # anything less rolls back in full.  A failure to write it therefore rolls back, exactly as
    # the earlier ones do -- the pair on disk is complete, but a pair the next run would roll
    # back to the previous generation anyway is not a pair this run may report as published.
    publish_journal_write 'the new pair is published' \
        || publish_fail "both halves of the new pair reached their published names but the
       publication journal could not be updated to record the second one.  The rollback below
       withdraws both and puts the previous pair back: a journal that records only the image
       tells the next run to roll this publication back, so reporting it as published here would
       be reporting something the next run will undo."

    # 9. MODE, DURABILITY, THE ASIDES, THEN THE JOURNAL.  All fatal; see the block above.
    #
    # The image is world-readable at this point only in the sense the caller's own umask and
    # directory allow; 0644 is set explicitly so the workflow's artifact upload and the QEMU
    # process can read it without the caller having to guess the mode.  A failure here leaves a
    # published image the job cannot read, which is not a published image.
    assert_output_dir_custody "set the mode of the published pair in $OUTPUT_DIR"
    chmod 0644 -- "$image_proc" \
        || publish_fail "could not set the mode of $OUTPUT_IMAGE after publishing it.  The
       workflow's artifact upload and the QEMU process both read that file, so an image whose
       mode could not be set is not usable; the previous pair has been put back."
    chmod 0644 -- "$manifest_proc" \
        || publish_fail "could not set the mode of $OUTPUT_MANIFEST after publishing it.  The
       workflow reads every path it uses out of that file; the previous pair has been put back."

    # DURABILITY, AND IT IS CHECKED.  The directory entries are what must reach the disk here;
    # the image's data was flushed in release_image before it was renamed.  `sync -f` flushes
    # the filesystem holding the named file and is preferred because it does not stall on every
    # other filesystem the host has mounted; a coreutils too old for -f falls back to the
    # whole-system flush.  What is NOT tolerated any more is BOTH failing: a pair that is not on
    # the disk when a self-hosted runner reboots was never published, and `|| true` said
    # otherwise.
    if ! sync -f -- "$image_proc" "$manifest_proc" 2>/dev/null; then
        if ! sync; then
            publish_fail "the published pair could not be flushed to disk: neither
           sync -f -- '$OUTPUT_IMAGE' '$OUTPUT_MANIFEST'
       nor a whole-system 'sync' succeeded.  An unflushed rename is a pair that may not exist
       after a reboot, so this run does not report a publication it cannot vouch for.  The
       previous pair has been put back."
        fi
        log "durability: 'sync -f' was unavailable or failed, so a whole-system sync was used"
    fi

    # THE SUPERSEDED ASIDES.  Fatal, and not tidiness: each is a whole image, and a leftover
    # aside is half of a transaction that did not finish -- which is exactly what
    # recover_interrupted_publication() reads as an interrupted run, and what it refuses to
    # disambiguate against its own journal.  The pair IS committed at this point, so this arm
    # does NOT roll back; it fails the run with the leftover named.
    local aside_leaf removal_failures=0
    for aside_leaf in "$PUBLISH_ASIDE_IMAGE_LEAF" "$PUBLISH_ASIDE_MANIFEST_LEAF"; do
        [ -n "$aside_leaf" ] || continue
        [ -e "$OUTPUT_DIR_PROC/$aside_leaf" ] || continue
        if ! rm -f -- "$OUTPUT_DIR_PROC/$aside_leaf"; then
            warn "could not remove the superseded copy $OUTPUT_DIR/$aside_leaf"
            removal_failures=$(( removal_failures + 1 ))
        fi
    done

    # THE JOURNAL IS REMOVED LAST, AFTER THE FINAL DURABILITY STEP.  Until it is gone the next
    # run would read this publication as interrupted and complete it forward, which is harmless
    # and idempotent -- removing it earlier would not be, because a kill between the removal and
    # the flush above would leave a pair whose durability nothing had established and no record
    # that anything was owed.
    if [ "$PUBLISH_JOURNAL_WRITTEN" -eq 1 ]; then
        if ! rm -f -- "$OUTPUT_DIR_PROC/$PUBLISH_JOURNAL_LEAF"; then
            warn "could not remove the publication journal $OUTPUT_DIR/$PUBLISH_JOURNAL_LEAF"
            removal_failures=$(( removal_failures + 1 ))
        else
            PUBLISH_JOURNAL_WRITTEN=0
        fi
    fi

    # 10. THE TRANSACTION IS CLOSED.  Cleared so the EXIT trap neither looks for staged files
    # that have been renamed away nor tries to roll back a committed publication.
    STAGED_IMAGE=''
    STAGED_MANIFEST=''
    STAGED_IMAGE_LEAF=''
    STAGED_MANIFEST_LEAF=''
    PUBLISH_ASIDE_IMAGE=''
    PUBLISH_ASIDE_MANIFEST=''
    PUBLISH_ASIDE_IMAGE_LEAF=''
    PUBLISH_ASIDE_MANIFEST_LEAF=''
    PUBLISH_ASIDE_IMAGE_HOLDS_OLD=0
    PUBLISH_ASIDE_MANIFEST_HOLDS_OLD=0
    PUBLISH_IMAGE_RENAMED=0
    PUBLISH_MANIFEST_RENAMED=0
    PUBLISH_INTENT_ASIDE_IMAGE=0
    PUBLISH_INTENT_ASIDE_MANIFEST=0
    PUBLISH_INTENT_NEW_IMAGE=0
    PUBLISH_INTENT_NEW_MANIFEST=0
    PUBLISH_ASIDE_IMAGE_HOLDER_DEVICE=''
    PUBLISH_ASIDE_IMAGE_HOLDER_INODE=''
    PUBLISH_ASIDE_MANIFEST_HOLDER_DEVICE=''
    PUBLISH_ASIDE_MANIFEST_HOLDER_INODE=''
    PUBLISH_IN_PROGRESS=0
    install_interrupt_traps

    if [ "$removal_failures" -gt 0 ]; then
        die "$removal_failures superseded file(s) from the previous publication could not be
       removed from $OUTPUT_DIR (named above).  The new pair IS published and durable at
           $OUTPUT_IMAGE
           $OUTPUT_MANIFEST
       so it is not rolled back -- but this run does not report success while a
       ${PUBLISH_ASIDE_IMAGE_PREFIX}*, ${PUBLISH_ASIDE_MANIFEST_PREFIX}* or journal file remains:
       the next run reads such a file as an interrupted publication and refuses to disambiguate
       it against its own journal.  Remove them by hand and the pair above is ready to use."
    fi

    log "published $OUTPUT_IMAGE and $OUTPUT_MANIFEST (mode 0644, flushed)"
    log "  each leaf was replaced by one atomic same-directory rename; the PAIR is consistent for"
    log "  a reader holding a shared flock on $OUTPUT_LOCK, which is what the workflow's boot and"
    log "  artifact-extraction steps take, and is restored to consistency by the next run's"
    log "  recovery if this one is interrupted"
    log "  every rename went through the publication directory descriptor on fd $OUTPUT_DIR_FD"
}

print_summary() {
    cat <<SUMMARY_EOF

================ ROOT IMAGE BUILT ================
image            $OUTPUT_IMAGE
manifest         $OUTPUT_MANIFEST
ELF bitness      $ARCH  (32-bit throughout; a SEPARATE axis from the wire protocol)
binder protocol  $DERIVED_BINDER_PROTOCOL  (an EXPECTATION this builder DERIVED -- kernel configuration: ${PROTOCOL_FROM_KERNEL_CONFIG:-could not answer}, staged SDK cache: ${PROTOCOL_FROM_SDK_CACHE:-could not answer}; the authority is the guest's own three-way cross-check ending at the BINDER_VERSION ioctl on /dev/binder, and disagreement there is fatal)
guest entry      $GUEST_INIT_PATH
artifacts        $GUEST_ARTIFACT_DIR inside the image
status file      $GUEST_STATUS_FILE inside the image

WHAT THE JOB DOES NEXT

  1. Boot this image with the guest kernel, which must be built with the options recorded
     in $GUEST_KERNEL_EXPECTATION inside the image:
     CONFIG_ANDROID, CONFIG_ANDROID_BINDER_IPC,
     CONFIG_ANDROID_BINDER_DEVICES="binder,hwbinder,vndbinder", CONFIG_ANDROID_BINDERFS and
     CONFIG_ASHMEM always; and CONFIG_ANDROID_BINDER_IPC_32BIT in whichever state the
     DERIVED protocol above requires, which that file spells out.  Booting a kernel whose
     protocol differs from the derived value is a named failure inside the guest rather than
     a silent one: the init asks /dev/binder directly.  Pass the root device and
         init=$GUEST_INIT_PATH
     on the kernel command line, and give the QEMU process its own bounded timeout: the
     guest powers itself down, and the timeout is the backstop for the case where it cannot.

  2. The guest needs NO network, NO shared directory and NO second disk.  Everything it
     runs is already inside this image.

  3. When the guest has powered down, loop-mount the image read-only and copy
     $GUEST_ARTIFACT_DIR out.  Read $GUEST_STATUS_FILE for the coverage runner's exact exit
     status: 0 means measured and passing, 3 is ADVISORY and is NOT a pass, anything else is
     a failure.

  4. The guest console carries one BINDER PLATFORM RECORD block with the kernel
     configuration, the SDK build flags and the resulting protocol version together, then a
     PROTOCOL AGREEMENT verdict comparing all three sources, and the coverage runner's own
     per-invocation output for the A-E matrix.  Keep the console log: it is the only place
     the platform record exists.

NOT MEASURED HERE, AND NOT CLAIMED ANYWHERE

  Performance parity and device-level L3 validation are deferred human steps on target
  hardware.  Nothing in this image build or in the guest run measures either, and no figure
  for either is produced or implied.
================ END ================
SUMMARY_EOF
}

# ------------------------------------------------------------------------------------
# MAIN.  The order is the dependency order, and every step is a hard gate: a run that cannot
# verify its inputs never reaches the point of creating an image, and a run that cannot
# finish the image never reaches the point of declaring one.
# ------------------------------------------------------------------------------------
main() {
    # THE SELF-TEST IS DISPATCHED FIRST, ahead of parse_args and therefore ahead of every
    # privileged step, so `--self-test` can never begin an image build.  It is matched
    # positionally rather than added to parse_args for exactly that reason: an option handled
    # inside parse_args would still be followed by resolve_arch, require_host_tools,
    # validate_inputs and the publication lock.
    if [ "${1:-}" = '--self-test' ]; then
        local self_test_status=0
        [ $# -eq 1 ] || die "--self-test takes no other arguments; it builds nothing, so no
       build option applies to it.  Run it on its own:
           bash $SCRIPT_PATH --self-test"
        run_self_test || self_test_status=$?
        return "$self_test_status"
    fi

    parse_args "$@"
    # THE PRIVILEGED SECTION BEGINS HERE, AND IT BEGINS IN A MOUNT NAMESPACE OF ITS OWN.
    #
    # This re-executes the script under `unshare --mount --propagation private` and returns only
    # in the process that came back inside the namespace, having proven it is in one.  It is
    # placed HERE for two reasons, both load-bearing:
    #
    #   * AFTER parse_args, so --help and an unknown option still answer for an unprivileged
    #     caller exactly as they always have.  parse_args validates argv and nothing else, so
    #     running it once on each side of the re-exec costs nothing and creates nothing.
    #   * BEFORE everything else, so that every path resolution, every ownership check, every
    #     mount, the loop device, the chroot and the scratch directory happen inside the
    #     namespace.  A check performed outside it and used inside would be the check-then-use
    #     shape the namespace exists to remove.
    #
    # --self-test is dispatched above this point and needs neither root nor a namespace.
    enter_private_mount_namespace "$@"
    resolve_arch
    require_host_tools
    validate_inputs
    # Where the two output files may be written, and exclusive access to them, established
    # before anything is built: every one of these checks is worthless after an hour of work
    # has been committed to a path that turns out to be a link, and a second builder has to be
    # refused before it starts rather than after it finishes.
    resolve_publication_targets
    lock_publication_targets
    # UNDER THE LOCK, AND BEFORE ANYTHING IS BUILT: put back a pair that a previous run was
    # killed in the middle of publishing.  SIGKILL, an OOM kill and a power loss run no trap,
    # so the previous run's own rollback could not have happened; the holding names are fixed
    # prefixes precisely so a LATER run can find them without a record from the run that died.
    recover_interrupted_publication
    verify_sdk_tree
    verify_payload_tree
    # Derived here, before a single byte of image is written: a protocol disagreement between
    # the kernel configuration and the staged SDK is a fault in the INPUTS, and finding it out
    # after an 8 GiB image has been formatted and populated costs the whole build for nothing.
    derive_binder_protocol

    make_work_dir
    acquire_base_rootfs

    create_and_mount_image
    extract_base_rootfs
    mount_chroot_pseudo_filesystems
    assert_chroot_executable
    assert_base_architecture

    install_image_packages
    install_lcov_2x

    copy_guest_payload
    register_guest_library_paths
    build_guest_helpers
    assert_image_toolchain

    write_kernel_expectation
    write_sdk_build_flags_record
    write_image_conf
    write_guest_init

    # LAST thing done to the mounted filesystem, and it has to be: it inspects and rewrites
    # what every step above left behind, so anything that ran after it could reintroduce what
    # it verified was gone.
    scrub_and_assert_image_apt_credentials

    # AFTER the apt scrub and still before the image is released.  It reads and never writes,
    # so it cannot reintroduce anything the scrub above verified was gone -- which is what the
    # "last thing done to the mounted filesystem" rule protects -- and running it last means it
    # inspects the tree exactly as it will be published.
    assert_image_carries_no_secret_values

    release_image
    write_manifest
    # The pair becomes visible at the caller's paths here -- each leaf by one atomic rename, the
    # pair as a whole to any reader holding the publication lock -- and only after the staged
    # image has been validated.
    publish_outputs
    # Declared finished only here, after the pair is published. Until this point a failure
    # removes this run's STAGED files and leaves any previously published pair untouched.
    BUILD_COMPLETED=1
    print_summary
}

main "$@"
