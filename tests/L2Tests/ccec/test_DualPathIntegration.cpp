/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2016 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/**
 * @defgroup HDMI_CEC_L2_DUALPATH HDMI CEC L2 dual-path integration
 * @brief The L2 tier's dual-back-end round-trip suite - its fixtures, its scope guards and the
 *        14 cases that assert one CCEC round trip against both HDMI CEC HAL back-ends.
 * @{
 */

/**
 * @file test_DualPathIntegration.cpp
 * @brief One CCEC round trip, asserted against both HDMI CEC HAL back-ends.
 *
 * This is the L2 tier's single translation unit.  It joins the same end-to-end path the L1
 * integration cases cover - HAL to Bus to Connection to the typed MessageProcessor overload -
 * and asserts it twice: once against the legacy in-process C ABI through the existing driver
 * mock (invocation D), and once against the out-of-process com.rdk.hal.hdmicec AIDL HAL over
 * real binder IPC to a separately hosted test-scope fake service (invocation E).
 *
 *   Flow A, inbound, legacy back-end:
 *     HAL Rx callback -> DriverImpl::DriverReceiveCallback -> receive queue
 *     -> Bus reader thread -> Connection address filter -> FrameListener::notify
 *     -> MessageDecoder::decode -> the typed MessageProcessor::process overload
 *
 *   Flow A, inbound, AIDL back-end:
 *     fake service (separate process) -> IHdmiCecEventListener::onMessageReceived on a
 *     binder thread -> the same receive queue -> the same Bus reader thread
 *     -> the same address filter -> the same FrameListener::notify -> the same decoder
 *     -> the same typed overload
 *
 *   Flow B, outbound, legacy back-end:
 *     a typed message -> MessageEncoder -> Connection::sendTo -> Bus -> DriverImpl::write
 *     -> the HAL HdmiCecTx, with the exact bytes on the wire
 *
 *   Flow B, outbound, AIDL back-end:
 *     a typed message -> MessageEncoder -> Connection::sendTo -> Bus -> DriverAidlImpl::write
 *     -> IHdmiCecController::sendMessage across the binder driver to the host process
 *
 * The point, plainly: only the producing thread and the transport change.  EventQueue is
 * already the cross-thread synchronization point of the receive path, so the Bus reader and
 * every layer above it are byte-identical on both arms - which is the whole claim this tier
 * exists to test, and the reason the migration reaches no file above the Driver seam.
 *
 * ---------------------------------------------------------------------------------------------
 * Why this tier exists and why it is not an L1 case.
 *
 * libbinder resolves a service name registered in the calling process to the local BBinder, so
 * interface_cast there hands back that very object: no Bp* proxy is created, no transaction
 * crosses the binder driver, and the client threadpool is never involved.  An in-process fake
 * therefore cannot prove the transport however faithfully it implements the interface, and that
 * is exactly what the L1 tier's in-process modes are.  Hosting the same fake in a separate
 * process is what makes the middleware hold a real proxy and receive its event callbacks on a
 * binder threadpool thread.  That is why the fake-service host binary exists, and why L1's
 * invocations B and C cannot substitute for invocation E.
 *
 * The converse is also true and is why L1 keeps its in-process modes: halcompat's compatibility
 * check reads the server's metadata through getInterfaceHash() and getInterfaceVersion(), which
 * on a local object dispatch virtually and can be overridden, whereas a remote Bn* service
 * answers those transactions from its compiled-in constants and cannot report bad metadata at
 * all.  The compatibility-rejection branches are reachable only in-process, so they have no L2
 * counterpart here and none is attempted.
 *
 * ---------------------------------------------------------------------------------------------
 * One process, one outcome.
 *
 * The back-end selection resolves once per process.  tests/L2Tests/test_main.cpp's global
 * ::testing::Environment::SetUp calls LibCCEC::getInstance().init("CEC_TEST"), and that is the
 * first thing in this binary to force Driver::getInstance(), whose helper constructs both
 * back-ends, asks the AIDL one whether its service came up, and emits exactly one selected-path
 * line naming the winner.  By the time any TEST_F body below runs, the choice is made and
 * immutable.
 *
 * A test body can observe the arm.  It can never change it.  Hence one binary and two
 * invocations, differing only in CEC_TEST_AIDL_MODE, and hence the two arm-specific fixtures
 * below skip rather than adapt when the resolved back-end is not theirs.
 *
 * ---------------------------------------------------------------------------------------------
 * Why the selected-path log line is not asserted in a test body.
 *
 * It cannot be, and the mechanism is worth stating so that nobody adds a case that appears to
 * do it.  CCEC_LOG writes to stdout with printf, prefixed [_CEC_LOG_PREFIX] and gated on the
 * file-static cec_log_level in ccec/src/Util.cpp, whose default is LOG_INFO, so a LOG_INFO line
 * prints by default - but the factory emits its line during the global environment's SetUp,
 * before the first test body.  No TEST_F can capture it retroactively, and it cannot be
 * re-triggered, because the selection is already resolved and the helper that emits it has
 * internal linkage in another translation unit.
 *
 * So the log assertion belongs to the coverage runner, which greps the per-invocation captured
 * log - exactly what its own contract specifies.  This file's contribution is to record the
 * expected literal per arm in the case manifest below, copied from ccec/src/Driver.cpp, so that
 * the runner and a human reader align on one string.  In-process, back-end identity is asserted
 * by dynamic_cast against the two non-installed concrete headers, which is authoritative and
 * needs no log at all.  No introspection API is added to the Driver interface for this, and none
 * may be: that would grow the middleware public surface, which the migration forbids.
 *
 * ---------------------------------------------------------------------------------------------
 * What is covered here and what is blocked.
 *
 * Covered: the middleware leg of both flows on the legacy back-end; both flows on the AIDL
 * back-end - flow B outbound over a real proxy and a real driver transaction, flow A inbound
 * over a real binder callback thread - the selection itself, which back-end resolved, that it is
 * stable across repeated factory calls, and that it matches the mode the harness was given; and
 * one guarantee about the harness that both arms depend on - that a write to a pipe whose reader
 * has gone reports EPIPE to the case that asked for it instead of terminating this runner before
 * its teardown can reap the host.
 *
 * Blocked, and reported rather than closed:
 *
 *   The plugin leg.  Both flows are covered for their middleware leg, which is the whole of the
 *   flow that lives in this repository.  The plugin leg - HdmiCecSink/Source FrameListener and
 *   its typed handlers - cannot be joined to this leg by any test-only change: the plugin L1 and
 *   L2 binaries link entservices-testframework's CEC mock (Tests/mocks/HdmiCec.h) in place of
 *   this middleware, so those processes contain no back-end at all and none is selectable in
 *   them.
 *     Required change, reported and not made - build the plugin test binaries against the real
 *     hdmicec libraries (libRCEC/libRCECOSHal) instead of the framework CEC mock, i.e. add the
 *     middleware include path and link the two libraries in the plugin test CMakeLists, and drop
 *     the -include of Tests/mocks/HdmiCec.h for those targets.  Only then can one test span
 *     HAL -> middleware -> plugin.  Those files are outside this migration's scope.
 *
 *   Inbound delivery on the AIDL arm is covered, and the mechanism is what makes it coverable at
 *   all.  The fake service lives in the host process and is not linked into this runner - that
 *   separation is the tier - so this file cannot call it directly, and
 *   FakeHdmiCecController::sendMessage records the frame and returns its canned status with no
 *   loopback.  What makes an inbound case possible is that the host serves a control and
 *   observation channel: two inherited pipe descriptors, named to the child by
 *   CEC_FAKE_HOST_CONTROL_FD and CEC_FAKE_HOST_OBSERVE_FD, over which it reads newline-terminated
 *   commands and writes exactly one reply line per command.  tests/L2Tests/test_main.cpp creates
 *   the pipes, clears FD_CLOEXEC on the child's two ends between fork() and exec(), exports the
 *   two numbers, and exposes the request/reply call to this translation unit through the cross-TU
 *   seam declared below.
 *
 *   The channel is a pipe and not binder, which is the whole reason it is evidence.  Binder is the
 *   thing under test on invocation E, so an observation that travelled over binder would be
 *   asserting a transport with itself.  Over the pipe: `deliver <hex>` makes the host fire
 *   onMessageReceived on the listener FakeHdmiCecService captured during open(), which arrives in
 *   this process on a binder threadpool thread; `sent-count` and `last-sent` report what the fake
 *   actually received from an outbound transaction, so a sendMessage that never arrived or arrived
 *   corrupted is caught rather than passed; and `open-count` and `close-count` report the fake
 *   service's own session lifecycle, so an open that never crossed the driver, a session closed
 *   behind a case's back, or a term() that closed nothing on the far side is caught the same way.
 *   Both flows on the AIDL arm are therefore observed from outside the transport they exercise,
 *   and no inbound AIDL case skips inside its own arm.
 *
 *   getPhysicalAddress on the AIDL arm is blocked on B1, the device-settings HAL contract that
 *   was to be supplied to this migration and was not.  It is deliberately not asserted on
 *   invocation E, and this sentence exists so that its absence reads as a report rather than an
 *   omission.  On the legacy arm it is unaffected and is covered by the L1 tier.
 *
 *   close on the AIDL arm maps to IHdmiCec.close, which is B2 - a high-confidence candidate
 *   pending owner confirmation, because HdmiCecClose has no mapping-table entry.  Every case
 *   here closes its own Connection, which does not reach Driver::close; the two places that do
 *   are the global environment's term() and the state-guard case, which cycles the library
 *   deliberately and carries the marker in its own doc block.  A green result here does not
 *   confirm that mapping.
 *
 * ---------------------------------------------------------------------------------------------
 * The one inheritance from the template that is rejected.
 *
 * The L1 template's inbound arm reaches the stack through DriverImpl::DriverReceiveCallback,
 * installed on the mock by restoreDriverInboundRoute() and called unconditionally from its
 * fixture's SetUp.  That is not safe here.  DriverImpl::DriverReceiveCallback resolves its target
 * with static_cast<DriverImpl &>(Driver::getInstance()).  Once the factory can return a
 * DriverAidlImpl - which it can, and on invocation E it does - that cast is ill-typed: undefined
 * behaviour, not a failed assertion, with no diagnostic and no bounded consequence.
 *
 * The hazard is live in this tier specifically, and that is the part it would be easy to miss.
 * tests/L2Tests/test_main.cpp installs the legacy HdmiCecDriverMock unconditionally on both
 * arms - it is the HAL the legacy arm drives, and there is no reason for the harness to withhold
 * it - so mock->rxCallback is writable on invocation E too, and a fixture that installed the
 * legacy route without checking would arm the cast on the very arm where it is wrong.
 *
 * So the legacy fixture below confirms the resolved back-end is legacy before it installs the
 * route, never after, and the AIDL fixture is forbidden from touching the mock's callback
 * members at all.  Reordering those two steps is the single most dangerous edit that can be made
 * to this file.
 *
 * ---------------------------------------------------------------------------------------------
 * Self-sufficiency.
 *
 * Every case here establishes its own preconditions and leaves no shared state altered: each opens
 * its own Connection, registers its own listener, and clears mock expectations in TearDown.  This
 * is deliberate - the L1 suite this file is derived from contains cases that fail under
 * --gtest_shuffle because they depend on each other, and these must not join them.
 *
 * The cleanup is RAII and not a trailing call, and that distinction is load-bearing rather than
 * stylistic.  A fatal assertion returns from a test body immediately, so a removeFrameListener()
 * and a close() written at the end of a body do not run on the one exit path where they matter
 * most - the failing one - and Connection::~Connection() is empty, so nothing else runs them
 * either.  Every case in this file therefore holds its connection in a ScopedConnection, whose
 * destructor detaches the listener and closes the connection on a normal return, on a fatal
 * assertion's early return, and on an exception escaping the body alike.  The one case that also
 * cycles the CEC library holds a ScopedCecLibraryCycle beside it for the same reason.  See both
 * guards' own doc blocks for the failure they prevent.
 *
 * The measured evidence for that independence was taken on invocation D, which is the arm a host
 * without binder support can run, and it covers the nine cases other than
 * WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner: every one of
 * them passes when a --gtest_filter selects it alone; the whole DualPath* suite passes under
 * --gtest_shuffle at two seeds and under --gtest_repeat=2; and with two of the fatal assertions
 * deliberately forced to fail, the remaining seven still pass and the process still tears the
 * library down cleanly, with no crash and no failure reported in an unrelated case.  That last run
 * is the one that exercises the guards: it is the exit path a trailing close() does not cover.
 *
 * That measurement does not cover
 * WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner, whose
 * independence rests on construction instead - the stronger of the two claims.  It reads the
 * process's SIGPIPE disposition without altering it, and everything else it uses - two pipes, a
 * child, and a third pipe of its own for the direct demonstration - it creates, drives and
 * releases within the case.  It consults no shared state, alters none, sleeps for nothing and polls
 * no clock, and it leaves no descriptor and no child behind on any path, including every failure
 * path.  Nothing about it can differ between running alone, in file order, or wherever a shuffle
 * puts it.
 *
 * ---------------------------------------------------------------------------------------------
 * Execution environment.
 *
 * This translation unit compiles and links anywhere the middleware does.  Executing it is
 * another matter: invocation D needs nothing special, while invocation E needs a binder-capable
 * kernel, a binder protocol version matching the one the linked libbinder was built for, and a
 * running servicemanager.  Neither the development host nor a hosted CI runner provides those,
 * so invocation E belongs to .github/workflows/aidl-path-tests.yml on the binder-capable guest.
 *
 * Measured on the host this file was written and built on, so that the constraint reads as a
 * fact rather than as a caveat: kernel 6.12.85+, CONFIG_ANDROID_BINDER_IPC not set, binder absent
 * from /proc/filesystems, and no /dev/binder node.  Invocation D is therefore the arm that is
 * executable here and invocation E is deferred, not skipped over.
 *
 * No result is claimed in this file that was not executed, and no timing figure appears anywhere
 * in it.
 */

/* =============================================================================================
 * Case manifest - the cross-file handoff to whoever wires the invocation matrix.
 *
 * The coverage runner gates every invocation on three things: a zero exit status, an executed
 * test count matching the expected count for that filter, and the asserted selected-path line in
 * the captured log.  For invocations D and E those numbers and that string come from this file and
 * from nowhere else, so this block publishes them.
 *
 *   Fixture                  Cases  Executes under        Skips under
 *   -------------------------------------------------------------------------------------------
 *   DualPathSelectionTest        4  D and E               nothing
 *   DualPathLegacyFlowTest       6  D                     E (fixture SetUp, back-end check)
 *   DualPathAidlFlowTest         4  E, all four           D (fixture SetUp, back-end check)
 *   -------------------------------------------------------------------------------------------
 *   Registered in this file     14
 *
 *   The fourth selection-fixture case is WriteControlCommandReportsEpipeAndTheChildIsStillReaped-
 *   InsteadOfKillingTheRunner, which is about the harness rather than a back-end: it drives the
 *   harness's own writeControlCommand() against a descriptor whose reader has gone, requires the
 *   command-specific EPIPE diagnostic back, and requires the child that made the descriptor
 *   reader-less to be reaped through the same terminate-and-reap a teardown performs - which is
 *   exactly what keeps the host's control channel diagnosable and the host reapable.  It builds its
 *   own pipes and its own child, so it needs no host, no driver and no service manager, and it
 *   executes under both invocations.
 *
 *   The filter that selects the whole suite:  --gtest_filter=DualPath*
 *   All three fixtures share the DualPath prefix precisely so that one glob does it, matching the
 *   sibling convention in tests/L1Tests/ccec/test_DriverAidl.cpp.
 *
 * The registered total is 14 and it is identical for D and E.  That is a deliberate property, not
 * a coincidence, and it is what lets the runner hold one expected count for both invocations.  The
 * two arm-specific fixtures skip rather than fail when the resolved back-end is not theirs, so
 * every case is registered and reported under both invocations; only the pass/skip split differs:
 *
 *   invocation D   14 registered = 4 selection pass + 6 legacy pass + 4 AIDL skip
 *   invocation E   14 registered = 4 selection pass + 6 legacy skip + 4 AIDL pass
 *
 * The skip identities follow from the fixture guards rather than from the case count, and that is
 * the property a skip allowlist cares about: the selection fixture skips for nothing, so under D
 * the skips are the four DualPathAidlFlowTest cases and nothing else, and under E the six
 * DualPathLegacyFlowTest cases and nothing else.  Adding an unconditional case moves the passing
 * count and no skip identity, and the runner reads every count from the binary rather than from a
 * literal.
 *
 * The measured D split, and exactly what it covers.  Invocation D - the one arm a host with no
 * binder support can run - is observed as 14 tests from 3 test suites ran, 10 passed, 4 skipped,
 * exit status zero, with "back-end selected : legacy" in the log preceded by "the binder transport
 * is unavailable on this platform".  That second line is the fallback-not-abort requirement made
 * visible in the run's own output rather than argued for in a comment.  The four skips are the
 * DualPathAidlFlowTest cases and nothing else.
 *
 * The invocation E split is derived from the fixture guards above and is not measured anywhere in
 * this repository, because it needs a binder-capable kernel, a matching binder protocol version
 * and a running servicemanager; whoever wires the binder-capable runner should re-measure it there
 * rather than trust this table's arithmetic.
 *
 * No case in this file skips inside the arm it was written for.  There are exactly three
 * GTEST_SKIP sites, and they are not all of one kind - the distinction is spelled out because a
 * skip allowlist built on the assumption that they are would be wrong:
 *
 *   Two are opposite-arm fixture skips, one in each arm-specific fixture's SetUp.
 *     DualPathLegacyFlowTest::SetUp skips when the resolved back-end is not legacy, so it fires
 *     under invocation E and takes all six of its cases with it.
 *     DualPathAidlFlowTest::SetUp skips when the resolved back-end is not AIDL, so it fires under
 *     invocation D and takes all four of its cases with it.
 *
 *   One is inside a case body - DualPathSelectionTest.TheResolvedBackEndMatchesTheModeTheHarness-
 *     WasGiven - and it is not an arm skip, which is why it is allowed to stay.  It fires only when
 *     CEC_TEST_AIDL_MODE is unset or empty, i.e. when no arm was requested and the case therefore
 *     has no request to hold the outcome against; the invocation matrix sets the variable
 *     explicitly for both D and E, so it fires under neither.  Measured under invocation D: the
 *     four skips reported are the DualPathAidlFlowTest cases and this case passes.  Its
 *     appearance in a matrix run means the runner did not export the variable, which is a harness
 *     fault to be treated as a failure and not allowlisted.
 *
 * In particular the two inbound AIDL cases - InboundFrameFromTheFakeServiceArrivesOnABinderThread-
 * AndReachesTheTypedProcessor and AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheState-
 * Guard - execute under invocation E through the host's control channel, so an invocation E that
 * reports any AIDL case skipped is a defect to investigate rather than an expected result.
 *
 * For whoever builds an explicit skip allowlist in the runner, the whole allowlist is: under D,
 * the four DualPathAidlFlowTest cases and nothing else; under E, the six DualPathLegacyFlowTest
 * cases and nothing else.  The third site above belongs in no allowlist.
 *
 * And one non-skip that matters to the same reader: DualPathAidlFlowTest::SetUp asserts that the
 * host's control and observation channel is open, deliberately rather than skipping on it.  Under
 * invocation E the AIDL back-end resolved, which means a host was published and ready, which means
 * this harness handed it a channel and proved it with a ping - so a closed channel contradicts the
 * arm that was selected and is a defect in the harness or the host, not a platform this tier
 * cannot run on.  Skipping on it would let invocation E report green with every observation it
 * exists to make quietly not made, which is the exact failure shape this tier is built to rule out.
 *
 * The selected-path log line, per arm - transcribed verbatim from ccec/src/Driver.cpp, whose three
 * constants live in an anonymous namespace in that translation unit and therefore cannot be
 * imported.  These are the strings the runner greps; exactly one appears per process:
 *
 *   invocation E (AIDL selected)
 *     Driver::getInstance : HDMI CEC HAL back-end selected : AIDL
 *
 *   invocation D (legacy selected)
 *     Driver::getInstance : HDMI CEC HAL back-end selected : legacy
 *
 * Each is emitted at LOG_INFO through CCEC_LOG, so each appears on stdout prefixed by the CEC log
 * prefix and a timestamp; grep for the trailing substring rather than for a whole line.  On the
 * legacy arm one of three additional lines precedes it, saying why the AIDL back-end was not
 * selected.  The factory emits all three through one format string with the reason substituted
 * into it, so they differ only in that reason:
 *
 *     "...the binder transport is unavailable on this platform"
 *     "...the binder transport is reachable but no compatible service resolved"
 *     "...the service query failed unexpectedly, so no usable service could be established"
 *
 * All three are deliberately worded unlike the selected-path line so that grepping for the
 * selected-path line still yields exactly one hit per process.  The first is what invocation D
 * produces on a host without a binder driver - observed verbatim in the measured run recorded
 * above - which is the fallback-not-abort requirement made visible in the run's own log.  The
 * third is the catch-all arm, emitted when the availability query itself failed rather than
 * answering, and it is listed here so that a run showing it is read as a query fault rather than
 * as either of the two ordinary fallback conditions.
 *
 * Why the line is not asserted by a case in this file is explained in the file block above: it is
 * emitted during the global environment's SetUp, before the first test body, and cannot be
 * re-triggered.  In-process, back-end identity is asserted by dynamic_cast instead, which is
 * authoritative.  This manifest exists so that the runner's grep and a human reader agree on one
 * string.
 *
 * This manifest is the authority: if the case set changes, this block changes with it, and the
 * runner's expected counts for invocations D and E are read from here.
 * ============================================================================================= */


#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
/*
 * <thread> is here for std::this_thread::get_id() and std::thread::id, which the inbound
 * assertions need and nothing else in this file provides.  The requirement invocation E has to
 * discharge is not merely that a frame arrived but that it arrived on a binder thread, and the
 * only way a test can establish that is to compare the thread that delivered the notification
 * against the thread that is running the test body.  Recording it is DecodingFrameListener's
 * job, because the test thread is blocked in the wait while the delivery happens and cannot
 * observe the delivering thread any other way.
 */
#include <thread>
/*
 * <cstdlib> is for std::getenv, used in exactly one case and only to assert that the resolved
 * back-end matches the mode the harness was given.  Nothing in this file decides anything from
 * the environment: tests/L2Tests/test_main.cpp owns reading CEC_TEST_AIDL_MODE and acting on it,
 * before the selection resolves.
 */
#include <cstdlib>
/*
 * <cstring> is for std::memset and std::strerror, used by the broken-pipe case below: the first to
 * zero a struct sigaction before it is filled in, the second to turn a failing system call's errno
 * into a sentence rather than a number nobody can read.
 */
#include <cstring>
/*
 * <cerrno> is for the errno the strtol() calls in the host-reply parsers below have to clear and
 * then inspect: strtol reports a range error only through errno, and a parser that skipped that
 * check would silently accept an out-of-range count as a number.  It is also what the broken-pipe
 * case reads to establish that its own write failed with EPIPE rather than with anything else.
 */
#include <cerrno>
/*
 * The POSIX descriptor and signal primitives, for one case - the broken-pipe guarantee in
 * DualPathSelectionTest.  It needs sigaction() from <csignal> to read back the disposition the
 * harness installed, and pipe2() and close() from <unistd.h> with O_CLOEXEC from <fcntl.h> for the
 * direct demonstration in its third step.  The two pipes and the child that drive the harness's own
 * writeControlCommand() and its own reaping are not built here: they belong to
 * tests/L2Tests/test_main.cpp, which owns every descriptor of that kind, and are reached through the
 * third seam function declared below.  Nothing else in this file touches a raw descriptor, and no
 * case here opens a file, a socket or a process.
 */
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include "ccec/Connection.hpp"
#include "ccec/Exception.hpp"
#include "ccec/CECFrame.hpp"
#include "ccec/MessageDecoder.hpp"
#include "ccec/MessageEncoder.hpp"
#include "ccec/MessageProcessor.hpp"
#include "ccec/Messages.hpp"
#include "ccec/Driver.hpp"
/*
 * ccec/LibCCEC.hpp is named explicitly and the reason is stated rather than left to be guessed: no
 * code in this file calls LibCCEC directly, and ccec/Connection.hpp includes ccec/LibCCEC.hpp anyway,
 * so this line adds no symbol.  It is here because the library's initialization is the precondition
 * of every case below - LibCCEC::init is the call that resolves the back-end selection, in the
 * global environment, before any body runs - and because the one case that would drive term() and
 * init() itself names them.  A reader looking for what this tier depends on should not have to
 * infer it from another header's include list.
 */
#include "ccec/LibCCEC.hpp"

/*
 * The two concrete back-end declarations, reached by relative path.
 *
 * ccec/src is not one of the AM_CPPFLAGS include roots and deliberately is not made one, so both
 * headers are reached the way the existing L1 units already reach DriverImpl.hpp - from
 * tests/L1Tests/ccec/ at exactly this depth, which is why the route transfers here unchanged.
 * Neither header is installed: both are absent from the nobase_include_HEADERS list in
 * hdmicec/Makefile.am, which is what allows a second back-end to exist without altering the
 * middleware public API, and what makes it legitimate for a test to name the concrete types at
 * all.
 *
 * Three things are needed from them and nothing else:
 *   (1) the concrete type names, so that back-end identity can be established by dynamic_cast
 *       against the object Driver::getInstance() returns.  No production introspection API is
 *       added for this, and none may be;
 *   (2) the address of DriverImpl::DriverReceiveCallback, for restoreDriverInboundRoute() below;
 *   (3) DriverAidlImpl's name as a dynamic_cast target, for the same identity question asked
 *       from the other side.
 *
 * Including DriverAidlImpl.hpp does not make this file an AIDL client.  It constructs neither
 * back-end, calls no AIDL method, touches no android::sp<> and reaches no service manager.  The
 * header brings the generated stubs and the binder SDK headers in with it because its own
 * session members are complete-type android::sp<> members; that is a compile-time consequence,
 * not a behavioural one.
 */
#include "../../../ccec/src/DriverImpl.hpp"
#include "../../../ccec/src/DriverAidlImpl.hpp"

#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::SetArgPointee;

/* =============================================================================================
 * The cross-translation-unit seam: how a case in this file drives and observes the fake service
 * that lives in the host process - and, in the third function, how it drives the harness's own
 * control-channel write and its own child reaping.
 *
 * These three functions are defined in tests/L2Tests/test_main.cpp, which owns the host's lifecycle
 * and therefore owns every pipe and every child in this binary.  The text below is the same contract
 * that appears above their definitions, written out here in full on purpose: two declarations of
 * one thing is the shape that usually rots, and the defence is that a reader of either side sees
 * the whole agreement rather than a pointer to it.  If one is edited, both are.
 *
 * A mismatch cannot go unnoticed.  The parameter types are std::string and the return type is bool,
 * so the exported name encodes the entire signature; a change on one side that the other does not
 * match is an undefined symbol at link time naming the function, not a silent difference in
 * behaviour.  That is why this is an extern declaration rather than a new header: a header for two
 * functions used by one file in one directory would add a file to the build, and nothing test-scope
 * in this migration may grow a production or installed surface.
 *
 * Why the channel exists at all - the two things this process cannot see from inside itself:
 *
 *   Outbound.  A transmit that crossed the binder driver is visible here only as "sendTo did not
 *   throw".  The bytes the service actually received are recorded by the fake, which is in the host
 *   process and is deliberately not linked into this runner - linking it would resolve the service
 *   name locally and turn this tier back into the in-process case.  So a send that arrived with
 *   corrupt bytes, or that never arrived at all, returns exactly as a correct one does, and only the
 *   host can say which happened.
 *
 *   Inbound.  The only object that can invoke the middleware's IHdmiCecEventListener is that same
 *   fake.  Without a way to ask the host to fire a callback, no case in this binary can cause an
 *   inbound delivery, and the requirement that a received frame arrive other than on the calling
 *   thread has nothing to assert against.
 *
 * The evidence travels over a pipe and not over binder, and that is the point: binder is the thing
 * under test.  Evidence carried over binder would be attesting to the transport with the
 * transport - a transport fault could corrupt the evidence, and the corruption would be
 * invisible.  The channel is two ordinary inherited pipes and behaves identically whether the
 * driver is healthy, degraded or absent.
 *
 * The command vocabulary is stated normatively in mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp
 * and is not restated here.  The ones this file uses are `listener`, `sent-count`, `last-sent`,
 * `open-count`, `close-count` and `deliver <lowercase-hex>`.  Every wait is bounded against one
 * monotonic deadline, so a host that has exited or gone silent produces a failed assertion naming
 * the command and never a hung suite.
 * ============================================================================================= */

/**
 * @brief Reports whether the fake service host's control and observation channel is usable.
 *
 * True only on an invocation that launched the host and completed its readiness handshake, which is
 * CEC_TEST_AIDL_MODE=remote alone.  On the legacy invocation there is no second process, so there is
 * nothing to ask and this reports false.
 *
 * @return bool                                   - Whether a request would have somewhere to go
 * @retval true                                   - Both descriptors are open and the host answered a
 *                                                  ping during setup
 * @retval false                                  - No host was launched, the launch failed, or the
 *                                                  channel has been closed by teardown
 *
 * @warning A case whose assertions depend on the channel must fail rather than skip when this is
 *          false.  Skipping on it would hide a broken handoff behind a green run, which is the exact
 *          failure this tier exists to rule out - which is why DualPathAidlFlowTest::SetUp asserts
 *          on it once, for every case in the fixture.
 *
 * @see cecL2HostControlRequest()
 */
extern bool cecL2HostControlChannelIsOpen();

/**
 * @brief Sends one command to the fake service host and returns its single reply line.
 *
 * Bounded in every direction and on every path: the write waits for the pipe to accept bytes, the
 * read waits for one newline, both against one deadline taken at entry, and an interrupted call
 * resumes against that same deadline rather than restarting it.  A host that has exited is reported
 * at once from EPIPE or end of file; a host that is alive and silent is reported when the bound
 * expires.  Nothing here can block indefinitely, because a hung harness is killed from outside with
 * no result recorded, which is strictly worse than any failed assertion.
 *
 * @param [in]  command                   - Command text without a terminator, e.g. "sent-count".
 *                                          Must be non-blank and free of newline and carriage
 *                                          return; both are rejected rather than sent, because
 *                                          either would desynchronise the one-reply-per-command
 *                                          framing for every later request
 * @param [out] reply                     - Receives the reply line without its terminator.
 *                                          Untouched when this reports failure
 * @param [out] failureDetail             - Receives a sentence naming what went wrong and what it
 *                                          means.  Untouched when this reports success
 *
 * @return bool                                   - Whether one command was exchanged for one reply
 * @retval true                                   - reply holds the host's answer, beginning "OK " or
 *                                                  "ERR "
 * @retval false                                  - No channel, a malformed command, the bound
 *                                                  expired, the host exited, or the reply was
 *                                                  unclassifiable; failureDetail says which
 *
 * @pre cecL2HostControlChannelIsOpen() reports true.
 *
 * @warning An "ERR " reply is a successful exchange and reports true.  Whether the host's refusal is
 *          expected is the calling case's judgement, not this function's, so the reply text must be
 *          checked - which is what the askHost* helpers below do.
 *
 * @see cecL2HostControlChannelIsOpen()
 */
extern bool cecL2HostControlRequest(const std::string &command, std::string &reply,
                                    std::string &failureDetail);

/**
 * @brief Drives the harness's own control-channel write and its own reaping against a broken pipe.
 *
 * The third seam, and the only one that is not about the fake service.  It exists because ignoring
 * SIGPIPE buys the harness exactly two things - the EPIPE arm of its writeControlCommand() becomes
 * reachable, and the teardown that signals and reaps the host still gets to run - and neither is
 * exercised by any ordinary invocation, since a healthy host reads its control descriptor until
 * teardown closes it.  A case that built a look-alike instead, out of its own pipe and its own raw
 * ::write(), would establish the kernel's behaviour and this process's signal disposition - neither
 * of which is in doubt - while leaving both of those two things completely unexercised.
 *
 * So this drives the real code: the harness's own writeControlCommand(), which takes its descriptor
 * as a parameter for exactly this reason, and the harness's own terminateAndReapChildProcess(), which
 * is the same function the global environment's TearDown uses on the host.  Neither is a copy and
 * neither has a test-only branch, so a change that broke either breaks this case.
 *
 * What it does, in order.  It creates a handshake pipe and a probe pipe; forks a child whose entire
 * job is to close both ends of the probe pipe, report that it has done so over the handshake pipe,
 * make SIGTERM fatal to itself and then block; closes the parent's copy of the handshake write end
 * and the parent's read end of the probe pipe, so that no reader of the probe pipe remains anywhere;
 * waits, bounded, for the child's report, because until it arrives the child may still hold the
 * inherited read end and a write would succeed; calls writeControlCommand() on the probe pipe's write
 * end and requires it to fail with a sentence naming the command and reporting EPIPE; ends the child
 * through terminateAndReapChildProcess() and requires the reap to succeed; and releases every
 * descriptor it opened, on every path.
 *
 * Deterministic with no platform support of any kind: no fake service host, no /dev/binder, no
 * service manager, no back-end.  close() on a pipe's last read end followed by write() to its write
 * end returns -1 with EPIPE synchronously, and SIGTERM to a child at SIG_DFL ends it.  Nothing is
 * slept on and no wall clock is polled - each of the three waits is a real wait on a real event under
 * one bound - so it behaves identically under invocation D on a host with no binder support, which is
 * where it ordinarily runs, and under invocation E on the binder-capable guest.  It touches none of
 * the live channel's state, so it is safe to call while a real host session is open.
 *
 * @param [out] observedDiagnostic        - Receives the sentence writeControlCommand() produced, so
 *                                          this case can assert on its substance at its own line
 *                                          rather than trusting the seam's own check.  Empty if the
 *                                          probe never reached that call, and empty if the call
 *                                          unexpectedly succeeded
 * @param [out] failureDetail             - Receives a sentence naming the step that failed and what
 *                                          its failure means.  Untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - The real write reported EPIPE with a diagnostic
 *                                                  naming the command, and the real terminate-and-
 *                                                  reap collected the probe's child
 * @retval false                                  - One step did not hold; failureDetail names which
 *
 * @pre SIGPIPE is not at its default disposition.  The seam verifies this before it writes anything
 *      and refuses rather than proceeding, and the case asserts it fatally first as well: a probe
 *      that terminated the runner while establishing that the runner cannot be terminated would be
 *      the worst available outcome.
 *
 * @post No descriptor and no child created by the call outlives it, on every path.
 *
 * @warning It does not and cannot establish what happens without the disposition installed, because
 *          establishing that would mean terminating this process.  That is why the case reads the
 *          disposition back out of the process and fails fatally on SIG_DFL: that assertion and this
 *          seam are two halves of one property.
 *
 * @see cecL2HostControlRequest()
 */
extern bool cecL2ProveEpipeDiagnosticAndChildReaping(std::string &observedDiagnostic,
                                                     std::string &failureDetail);

/**
 * @brief Re-states the driver's own HAL receive registration on the process-global mock.
 *
 * A frame injected after this call travels the production inbound route,
 * HAL -> DriverImpl -> Bus -> Connection, rather than reaching whatever callback was registered
 * last.  A test has to do this because the mock stores whatever the real HdmiCecSetRxCallback entry
 * point is handed, on an instance that outlives every fixture, and the global environment does not
 * install the driver's registration itself - it creates the mock and initializes the library, and
 * DriverImpl::open() registers through the mock only on the first open.  Any case anywhere in a
 * binary that registers a callback of its own therefore leaves an injection reaching that callback
 * - through a data pointer that may have died with its stack frame - instead of the CEC stack, and
 * DriverImpl::open() does not put the driver's registration back, because it returns early while
 * the driver is already OPENED.  The L1 tier measured the consequence deterministically:
 * --gtest_repeat=2 over a filter mixing a callback-registering case with the inbound integration
 * cases passes iteration 1 and fails every inbound case in iteration 2.  Establishing the route per
 * fixture is what makes each case behave identically whether it runs alone, in file order, or
 * wherever a shuffle puts it.
 *
 * The data pointer is 0 because that is exactly what DriverImpl::open() registers alongside the
 * callback, so this restores the driver's registration rather than inventing a new one.
 *
 * @param [in] mock                       - Process-global legacy HAL mock whose receive
 *                                          registration is restored.  A null pointer is tolerated
 *                                          and nothing is installed, so a fixture that skipped
 *                                          before resolving the mock cannot fault here
 *
 * @return None
 *
 * @pre The resolved back-end is the legacy one, established by the caller.
 *
 * @warning This may be called only once the resolved back-end has been confirmed to be the legacy
 *          one, and the prohibition is not stylistic.  The callback it installs resolves its target
 *          with static_cast<DriverImpl &>(Driver::getInstance()) inside
 *          DriverImpl::DriverReceiveCallback, which is ill-typed - undefined behaviour - once the
 *          factory can return a DriverAidlImpl.  The L2 harness installs the legacy mock on both
 *          arms, so mock->rxCallback is writable on the AIDL arm too and nothing but the caller's
 *          own check stands between this function and that cast.  DualPathLegacyFlowTest::SetUp
 *          performs that check first and skips before reaching here; DualPathAidlFlowTest never
 *          calls this at all.
 *
 * @see DualPathLegacyFlowTest::SetUp()
 * @see DriverImpl::DriverReceiveCallback()
 */
static void restoreDriverInboundRoute(HdmiCecDriverMock *mock) {
    if (mock != nullptr) {
        mock->rxCallback = &DriverImpl::DriverReceiveCallback;
        mock->rxCallbackData = 0;
    }
}


namespace {

/* ---------------------------------------------------------------------------------------------
 * The host observation helpers.
 *
 * Each one issues exactly one command over the pipe channel, insists on the one reply shape the
 * protocol defines for it, and hands back a typed value with a sentence explaining any failure.
 * They return bool rather than asserting, so that a caller can attach its own ASSERT_TRUE and the
 * failure appears at the case's line rather than inside a helper - and so that a case which
 * legitimately expects a refusal can inspect the reply instead.
 *
 * There are exactly five, covering the six commands this file uses - one of them serves the two
 * session counters, which are the same reply shape read for the same purpose - and every one of them
 * is used by a case below.  A helper for a command no case sends would imply coverage that does not
 * exist, which is the same defect as a fake control nothing exercises.
 * --------------------------------------------------------------------------------------------- */

/**
 * @brief Splits an "OK <verb> <value>" reply into its value, insisting on the verb.
 *
 * The verb is checked rather than skipped because a reply for the wrong command is precisely what a
 * desynchronised channel produces, and a parser that read the trailing field regardless would carry
 * that desynchronisation into an assertion as a plausible-looking number.
 *
 * @param [in]  reply                     - Reply line as the channel delivered it, terminator already
 *                                          stripped
 * @param [in]  verb                      - Verb the reply must carry, e.g. "sent-count"
 * @param [out] value                     - Receives the text after the verb and its single separating
 *                                          space.  An empty value is legitimate: `last-sent` answers
 *                                          "OK last-sent " with nothing after it when the fake has
 *                                          captured no frame
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether the reply matched "OK <verb>" and its value
 *                                                  was extracted
 * @retval true                                   - value holds the field, possibly empty
 * @retval false                                  - The reply was an "ERR " line, named another verb, or
 *                                                  was malformed; failureDetail says which
 */
bool parseOkReplyValue(const std::string& reply, const std::string& verb, std::string& value,
                       std::string& failureDetail)
{
    const std::string expectedPrefix = "OK " + verb;

    if (reply.compare(0, expectedPrefix.size(), expectedPrefix) != 0) {
        failureDetail = "the fake service host answered \"" + reply + "\" where a reply beginning \"" +
                        expectedPrefix + "\" was required. An \"ERR \" line means the host refused the "
                        "command; a different verb means the channel is out of step with the commands "
                        "being sent";
        return false;
    }

    if (reply.size() == expectedPrefix.size()) {
        value.clear();
        return true;
    }

    if (reply[expectedPrefix.size()] != ' ') {
        failureDetail = "the fake service host answered \"" + reply + "\", whose verb is not \"" + verb +
                        "\" but a longer word beginning with it";
        return false;
    }

    value = reply.substr(expectedPrefix.size() + 1);
    return true;
}

/**
 * @brief Asks the host how many times the fake controller's sendMessage() has really been called.
 *
 * This is the counter that makes an outbound assertion mean something.  It is the fake's own count,
 * read through the fake's own accessor in the host process, so it cannot report a transmit that never
 * arrived - which is exactly what "sendTo did not throw" can do.
 *
 * @param [out] count                     - Receives the invocation count.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether a count was obtained
 * @retval true                                   - count holds the fake's real sendMessage() count
 * @retval false                                  - The channel failed, or the reply was not a
 *                                                  well-formed "OK sent-count <n>"
 */
bool askHostForSentCount(long& count, std::string& failureDetail)
{
    std::string reply;
    if (!cecL2HostControlRequest("sent-count", reply, failureDetail)) {
        return false;
    }

    std::string field;
    if (!parseOkReplyValue(reply, "sent-count", field, failureDetail)) {
        return false;
    }

    errno = 0;
    char* parseEnd = nullptr;
    const long parsed = std::strtol(field.c_str(), &parseEnd, 10);

    if (field.empty() || (errno != 0) || (parseEnd == nullptr) || (*parseEnd != '\0') || (parsed < 0)) {
        failureDetail = "the fake service host reported its sendMessage() count as \"" + field +
                        "\", which is not a non-negative decimal number";
        return false;
    }

    count = parsed;
    return true;
}

/**
 * @brief Asks the host how many times the fake service has really served open() or close().
 *
 * The two session counters, read the same way because they are the same reply shape asked for the
 * same reason: they are the only evidence in this repository that the middleware's AIDL session
 * lifecycle crossed the binder driver, as against its transmits, which `sent-count` covers.  L1's
 * in-process fake cannot give it - a locally resolved service is called inline, so a count there
 * says nothing about a driver transaction - which is precisely why these two verbs exist in the
 * host's vocabulary and why cases here have to consume them.
 *
 * They are the fake's own counters, read through the fake's own accessors in the host process, and
 * they advance at the top of `open()` and `close()` before any canned result is consulted, so a
 * refused open still counts as an open served.
 *
 * @param [in]  verb                      - Either "open-count" or "close-count".  Any other value is
 *                                          a caller mistake and is refused here rather than sent, so
 *                                          that a typo reads as a wrong call and not as a host that
 *                                          rejected an unknown command
 * @param [out] count                     - Receives the invocation count.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether a count was obtained
 * @retval true                                   - count holds the fake service's real count for that
 *                                                  verb
 * @retval false                                  - The verb was not one of the two, the channel
 *                                                  failed, or the reply was not a well-formed
 *                                                  "OK <verb> <n>"
 */
bool askHostForSessionCount(const std::string& verb, long& count, std::string& failureDetail)
{
    if ((verb != "open-count") && (verb != "close-count")) {
        failureDetail = "\"" + verb + "\" is not a session counter; the host serves \"open-count\" "
                        "and \"close-count\" and nothing else of this shape";
        return false;
    }

    std::string reply;
    if (!cecL2HostControlRequest(verb, reply, failureDetail)) {
        return false;
    }

    std::string field;
    if (!parseOkReplyValue(reply, verb, field, failureDetail)) {
        return false;
    }

    errno = 0;
    char* parseEnd = nullptr;
    const long parsed = std::strtol(field.c_str(), &parseEnd, 10);

    if (field.empty() || (errno != 0) || (parseEnd == nullptr) || (*parseEnd != '\0') || (parsed < 0)) {
        failureDetail = "the fake service host reported its " + verb + " as \"" + field +
                        "\", which is not a non-negative decimal number";
        return false;
    }

    count = parsed;
    return true;
}

/**
 * @brief Asks the host for the exact bytes of the fake controller's most recent sendMessage() frame.
 *
 * Rendered by the host as lowercase hexadecimal, two digits per byte and no separators, which is the
 * protocol's one payload encoding in both directions.  An empty string is a legitimate answer and
 * means the fake has captured no frame at all - a distinct outcome from a wrong frame, and one an
 * assertion has to be able to report differently.
 *
 * @param [out] hex                       - Receives the hexadecimal rendering, empty when nothing has
 *                                          been captured.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether the capture was obtained
 * @retval true                                   - hex holds the fake's last captured frame
 * @retval false                                  - The channel failed, or the reply was not a
 *                                                  well-formed "OK last-sent <hex>"
 */
bool askHostForLastSentFrame(std::string& hex, std::string& failureDetail)
{
    std::string reply;
    if (!cecL2HostControlRequest("last-sent", reply, failureDetail)) {
        return false;
    }

    return parseOkReplyValue(reply, "last-sent", hex, failureDetail);
}

/**
 * @brief Asks the host whether the fake is holding an event listener from the middleware.
 *
 * Every inbound case checks this first, and the reason is that without it the negative half of those
 * cases would be vacuous: a `deliver` with no listener held is answered "ERR no-listener" and nothing
 * is dispatched, so "no frame arrived" would be satisfied by the trigger having done nothing at all
 * rather than by the middleware having rejected it.
 *
 * @param [out] present                   - Receives whether a listener is held.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether the answer was obtained
 * @retval true                                   - present holds the fake's real listener state
 * @retval false                                  - The channel failed, or the reply was neither
 *                                                  "OK listener present" nor "OK listener absent"
 */
bool askHostForListenerPresence(bool& present, std::string& failureDetail)
{
    std::string reply;
    if (!cecL2HostControlRequest("listener", reply, failureDetail)) {
        return false;
    }

    std::string field;
    if (!parseOkReplyValue(reply, "listener", field, failureDetail)) {
        return false;
    }

    if (field == "present") {
        present = true;
        return true;
    }

    if (field == "absent") {
        present = false;
        return true;
    }

    failureDetail = "the fake service host described its listener as \"" + field +
                    "\", where the protocol defines only \"present\" and \"absent\"";
    return false;
}

/**
 * @brief Asks the host to invoke onMessageReceived on the middleware's listener with these bytes.
 *
 * The one inbound trigger, and the only one there is: the fake fires through its own
 * fireOnMessageReceived(), so the listener invoked is exactly the one the middleware handed to
 * `open()` and no second delivery route exists.  Because IHdmiCecEventListener is `oneway`, the
 * host's reply says only that the callback was invoked - whether the middleware then queued, decoded
 * and dispatched the frame is what this file asserts on its own side of the boundary, which is the
 * assertion that matters anyway.
 *
 * @param [in]  hex                       - Frame as lowercase hexadecimal, two digits per byte, no
 *                                          separators
 * @param [out] deliveredBytes            - Receives the byte count the host reports delivering, which
 *                                          must equal half the hex length.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether the host invoked the callback
 * @retval true                                   - It did, with deliveredBytes bytes
 * @retval false                                  - The channel failed, or the host refused:
 *                                                  "ERR no-listener" when it holds no listener, or
 *                                                  "ERR bad-hex" when the payload is malformed
 */
bool askHostToDeliverFrame(const std::string& hex, long& deliveredBytes, std::string& failureDetail)
{
    std::string reply;
    if (!cecL2HostControlRequest("deliver " + hex, reply, failureDetail)) {
        return false;
    }

    std::string field;
    if (!parseOkReplyValue(reply, "delivered", field, failureDetail)) {
        return false;
    }

    errno = 0;
    char* parseEnd = nullptr;
    const long parsed = std::strtol(field.c_str(), &parseEnd, 10);

    if (field.empty() || (errno != 0) || (parseEnd == nullptr) || (*parseEnd != '\0') || (parsed < 0)) {
        failureDetail = "the fake service host reported delivering \"" + field +
                        "\" bytes, which is not a non-negative decimal number";
        return false;
    }

    deliveredBytes = parsed;
    return true;
}

/**
 * @brief Renders frame bytes as the lowercase hexadecimal the channel uses.
 *
 * Used for both directions of one comparison: to build a `deliver` payload, and to turn the bytes a
 * case encoded into the exact string the host's `last-sent` reply must equal.  Written out here
 * rather than through a stream manipulator so that the two-digits-per-byte, no-separator,
 * lowercase form is visible at the point it is produced - it is a wire format shared with another
 * process, and a formatting drift would present as a byte-comparison failure with no clue why.
 *
 * @param [in] bytes                      - Frame bytes.  May be null only when length is zero
 * @param [in] length                     - Number of bytes to render
 *
 * @return std::string                            - Lowercase hexadecimal, two digits per byte
 */
std::string toLowercaseHex(const unsigned char* bytes, std::size_t length)
{
    static const char digits[] = "0123456789abcdef";
    std::string rendered;
    rendered.reserve(length * 2);

    for (std::size_t index = 0; index < length; index++) {
        rendered.push_back(digits[(bytes[index] >> 4) & 0x0F]);
        rendered.push_back(digits[bytes[index] & 0x0F]);
    }

    return rendered;
}

/**
 * @brief A MessageProcessor that records which typed overload ran and what it carried.
 *
 * The point of the inbound cases is that a frame is not merely delivered but correctly
 * interpreted, so the recording is per overload rather than a single "something arrived" flag: a
 * frame decoded as the wrong message type leaves the expected counter at zero and the case fails
 * instead of passing on a coincidence.  That distinction is what makes the same helper usable
 * against both back-ends without weakening either arm - a transport that mangled an opcode or
 * dropped an operand would satisfy a boolean flag and cannot satisfy these counters.
 *
 * Only the four message types this file actually asserts on are overridden.  MessageProcessor
 * gives every other process() overload a default body, so an unexpected message type is simply
 * not counted here, which is exactly the behaviour the negative assertions rely on.
 *
 * @warning Not thread safe by itself, and it does not need to be - but only because of a property
 *          of DecodingFrameListener that has to be stated here too, since this is the class whose
 *          state is at risk.  These counters are written only by MessageDecoder::decode, called
 *          from DecodingFrameListener::notify while that listener's mutex is held and before its
 *          notification counter is published.  So a test thread that has returned from
 *          WaitForNotification has observed a completed decode, and no partially written counter
 *          can be read.  If that ordering is ever rearranged, this class needs a lock of its own.
 *
 * @see DecodingFrameListener - the ordering that makes this class safe to read without a lock
 */
class RecordingProcessor : public MessageProcessor {
public:
    /**
     * @brief Constructs the recorder with every counter at zero and every captured value unset.
     *
     * The captured operand values start at -1 rather than 0 so that "never decoded" is
     * distinguishable from "decoded as zero", which matters because 0.0.0.0 is exactly what a
     * zeroed operand pair would render as.
     */
    RecordingProcessor()
        : imageViewOnCount(0)
        , textViewOnCount(0)
        , activeSourceCount(0)
        , standbyCount(0)
        , activeSourcePhysical()
        , activeSourcePackedHigh(-1)
        , activeSourcePackedLow(-1)
        , lastInitiator(-1)
        , lastDestination(-1)
    {
        for (int index = 0; index < 4; index++) {
            activeSourceNibbles[index] = -1;
        }
    }

    /**
     * @brief Counts one decoded <Image View On> and records its header.
     *
     * The message itself carries no operand, so only the count and the header are of interest.
     *
     * @param [in] msg                    - Decoded message, unused beyond its type
     * @param [in] header                 - Initiator and destination as they arrived
     *
     * @return None
     */
    void process(const ImageViewOn& msg, const Header& header) override
    {
        (void)msg;
        imageViewOnCount++;
        recordHeader(header);
    }

    /**
     * @brief Counts one decoded <Text View On> and records its header.
     *
     * Overridden so that an <Image View On> that decoded as this instead is caught by a
     * counter rather than passing on the fact that something arrived.
     *
     * @param [in] msg                    - Decoded message, unused beyond its type
     * @param [in] header                 - Initiator and destination as they arrived
     *
     * @return None
     */
    void process(const TextViewOn& msg, const Header& header) override
    {
        (void)msg;
        textViewOnCount++;
        recordHeader(header);
    }

    /**
     * @brief Counts one decoded <Active Source> and records its header and its operands.
     *
     * This is the only overload here whose message carries operands, and they are recorded three
     * ways so that an operand pair which survived transit but changed value cannot pass.
     *
     * @param [in] msg                    - Decoded message; its physicalAddress is recorded
     * @param [in] header                 - Initiator and destination as they arrived
     *
     * @return None
     */
    void process(const ActiveSource& msg, const Header& header) override
    {
        activeSourceCount++;
        recordPhysicalAddress(msg.physicalAddress);
        recordHeader(header);
    }

    /**
     * @brief Counts one decoded <Standby> and records its header.
     *
     * The state-guard case uses this opcode as its post-restore control precisely because it is a
     * different message type from the frame delivered while the driver was closed.
     *
     * @param [in] msg                    - Decoded message, unused beyond its type
     * @param [in] header                 - Initiator and destination as they arrived
     *
     * @return None
     */
    void process(const Standby& msg, const Header& header) override
    {
        (void)msg;
        standbyCount++;
        recordHeader(header);
    }

    /**
     * @brief How many frames decoded to each of the four message types this file asserts on.
     *
     * A case asserts the expected counter at one and the other three at zero, so a frame that
     * arrived as the wrong message type fails instead of satisfying an "something arrived" check.
     */
    int imageViewOnCount;
    int textViewOnCount;
    int activeSourceCount;
    int standbyCount;

    /**
     * @brief The <Active Source> physical address as PhysicalAddress renders it.
     *
     * Dotted decimal, e.g. "1.0.0.0": PhysicalAddress::toString() overrides CECBytes::toString()
     * to produce that form, so this is the human-readable address and not the packed byte pair.
     * The packed bytes are recorded separately below.
     */
    std::string activeSourcePhysical;

    /**
     * @brief The four decoded nibbles of the <Active Source> physical address, in wire order.
     *
     * Taken from PhysicalAddress::getByteValue(0..3), and -1 in every slot until an
     * <Active Source> is decoded.  These exist because a non-empty rendered string is satisfied by
     * any address: 0.0.0.0, a shifted 0.1.0.0 and a truncated value all render non-empty, so a
     * case that only checked for emptiness would pass on a corrupted operand pair.  Asserting the
     * four nibbles pins the value exactly, and doing it per nibble - rather than only on the
     * rendered string - means a failure report names which digit moved.
     */
    int activeSourceNibbles[4];

    /**
     * @brief The two packed operand bytes as they travelled on the wire.
     *
     * Recovered by re-serializing the decoded PhysicalAddress: two nibbles per byte, so 1.0.0.0
     * packs to 0x10 0x00, and -1 until an <Active Source> is decoded.  The nibbles above and these
     * bytes are two views of the same two octets, and asserting both is deliberate rather than
     * redundant: the nibbles catch a value that decoded wrongly, and these catch a value that
     * decoded correctly but would not re-encode to the same wire image - which is the property an
     * outbound case on the other side of the same address depends on.
     */
    int activeSourcePackedHigh;
    int activeSourcePackedLow;

    /**
     * @brief The header nibbles of the most recently decoded message, or -1 before the first one.
     *
     * Asserted by every inbound case, because an initiator or destination rewritten in transit is
     * a transport defect that the opcode counters above cannot see.
     */
    int lastInitiator;
    int lastDestination;

private:
    /**
     * @brief Records the initiator and destination of a decoded message's header.
     *
     * @param [in] header                 - Header as the decoder produced it
     *
     * @return None
     */
    void recordHeader(const Header& header)
    {
        lastInitiator = header.from.toInt();
        lastDestination = header.to.toInt();
    }

    /**
     * @brief Records a decoded physical address three ways - rendered, per nibble and packed.
     *
     * CECBytes keeps its byte vector protected, so the packed pair is recovered the only way a
     * consumer can: by serializing the operand back into a CECFrame, which is the same public
     * path the encoder uses, and reading the two bytes out of it.  A serialization that produced
     * anything other than two bytes leaves both packed members at -1 rather than reading past the
     * end, so a malformed operand is reported as an unset value instead of causing a fault here.
     *
     * @param [in] address                - Physical address as the decoder produced it
     *
     * @return None
     */
    void recordPhysicalAddress(const PhysicalAddress& address)
    {
        activeSourcePhysical = address.toString();

        for (int index = 0; index < 4; index++) {
            activeSourceNibbles[index] = static_cast<int>(address.getByteValue(index));
        }

        CECFrame packed;
        address.serialize(packed);

        const uint8_t* buffer = 0;
        size_t length = 0;
        packed.getBuffer(&buffer, &length);

        if (buffer != 0 && length == 2) {
            activeSourcePackedHigh = static_cast<int>(buffer[0]);
            activeSourcePackedLow = static_cast<int>(buffer[1]);
        }
    }
};

/**
 * @brief A FrameListener that decodes what it is given, records which thread gave it, and blocks
 *        the test until it has done so.
 *
 * Delivery is asynchronous on both arms - the Bus reader thread notifies listeners, never the
 * thread that caused the frame to appear - so the test needs a real cross-thread wait.
 * WaitForNotification is a bounded predicate wait and deliberately not a fixed sleep: a sleep is
 * either too short under load, or inside an emulated guest, or wasteful when it is not, and it is
 * never once evidence of anything.  A condition variable with wait_for is both correct and quick,
 * and its expiry is a real verdict - "the frame never arrived" - rather than a guess, which is
 * what lets the same helper serve the negative control as well as the positive cases.
 *
 * The delivering thread is recorded here rather than in a test body because the test thread is
 * blocked inside WaitForNotification while the delivery happens, so it cannot observe the
 * delivering thread any other way; by the time it wakes, the only trace left is whatever the
 * listener kept.  That trace is not a diagnostic nicety on the AIDL arm - it is half the
 * requirement.  Invocation E has to show that a received frame arrived without the test thread
 * having delivered it, and comparing NotifyingThread() against the test body's own
 * std::this_thread::get_id() is the assertion that fails if a callback were somehow delivered
 * inline on the calling thread.
 *
 * The ordering inside notify() is a correctness property of this class: the decode happens before
 * the wait predicate is published, and under the same lock.  MessageProcessor state is not thread
 * safe and MessageDecoder::decode mutates it - that is the whole reason a RecordingProcessor can be
 * asserted on at all - so publishing the frame, the thread id and the notification counter before
 * decoding would let a waiter wake, on the counter it had just been shown or on a spurious wake of
 * the condition variable, and read the processor's counters while the decode was still writing
 * them.  That race has a real failure mode rather than a theoretical one: the test thread reads a
 * half-written processor and the case flakes, and it flakes in the direction of passing on a
 * corrupt read, which is worse.
 *
 * The order below closes it at the root.  Everything the waiter can observe - the processor, the
 * frame, the thread id, the counter - is written while the lock is held, and the counter that
 * satisfies WaitForNotification's predicate is written last.  So a waiter that observes the counter
 * has necessarily observed a completed decode, and a spurious wake finds the predicate unsatisfied
 * and goes back to waiting, which is exactly what wait_for's predicate overload is for.  Holding
 * the lock across the decode also serialises two deliveries against each other, which costs nothing
 * here - the Bus reader is a single thread - and removes the question entirely.
 *
 * The cost is bounded and worth naming: a waiter that times out concurrently with a delivery blocks
 * on the mutex until that delivery's decode finishes.  A decode is a switch and one process() call,
 * so this is microseconds, and it cannot extend the wait beyond one delivery's work.
 *
 * A decode that throws is contained here, and that is not defensive decoration either.  This
 * notify() is called by Bus::Reader::run() from inside a try block that catches
 * InvalidStateException alone, so any other exception escaping a listener leaves that thread's
 * run() and terminates the process - taking the whole suite with it and reporting nothing useful.
 * MessageDecoder::decode already swallows std::exception around its own opcode switch, so this is a
 * second line rather than the first; it is here because "the reader thread dies" is not an
 * acceptable way for a test to fail.  The failure is counted rather than swallowed -
 * DecodeFailures() is asserted to be zero by every case that expects a delivery - so a containment
 * that fired is reported instead of hidden.
 *
 * @warning The statement order in notify() must not be rearranged; RecordingProcessor is safe to
 *          read without a lock of its own only because of it.
 *
 * @see Bus::Reader::run() - the caller, whose catch clause is why a decode is contained here
 * @see RecordingProcessor
 */
class DecodingFrameListener : public FrameListener {
public:
    /**
     * @brief Binds the listener to the processor every decoded message is dispatched to.
     *
     * @param [in] processor              - Processor the decoder dispatches to.  Must outlive this
     *                                      listener, which is why a case declares it first
     */
    explicit DecodingFrameListener(MessageProcessor& processor)
        : decoder(processor)
        , notifications(0)
        , decodeFailures(0)
    {
    }

    /**
     * @brief Decodes one delivered frame, records what delivered it, and wakes any waiter.
     *
     * Called on the Bus reader thread on both arms.  Everything a waiter can observe is written
     * under the lock, with the notification counter written last, so a waiter that sees the counter
     * has seen a completed decode.
     *
     * @param [in] frame                  - Frame as the Bus reader delivered it
     *
     * @return None
     *
     * @post notifications has advanced by one, and either the processor has been updated or
     *       decodeFailures has advanced by one.
     *
     * @warning Nothing here may escape as an exception: Bus::Reader::run() catches only
     *          InvalidStateException, so anything else would end the reader thread and the process.
     */
    void notify(const CECFrame& frame) const override
    {
        std::lock_guard<std::mutex> guard(mutex);

        /*
         * (1) Decode first, so that no waiter can be woken by a predicate that is true while the
         *     processor is still being written.  See the ordering paragraph on this class.
         */
        try {
            const_cast<MessageDecoder&>(decoder).decode(frame);
        }
        catch (const std::exception& e) {
            decodeFailures++;
            lastDecodeError = e.what();
        }
        catch (...) {
            decodeFailures++;
            lastDecodeError = "an exception not derived from std::exception";
        }

        /*
         * (2) Publish second, counter last of all.  The thread id is this thread's on purpose: this
         *     function runs on the thread that delivered the frame - the Bus reader thread on both
         *     arms - so recording it here is what makes NotifyingThread() meaningful to a test body
         *     that was blocked while it happened.
         */
        lastFrame = frame;
        notifyingThread = std::this_thread::get_id();
        notifications++;

        condition.notify_all();
    }

    /**
     * @brief Waits, bounded, until at least this many notifications have been delivered.
     *
     * A predicate wait rather than a sleep, so its expiry is a verdict a negative case can rely on:
     * the frame was given the same opportunity to arrive that a positive case gives it.
     *
     * @param [in] expected               - Notification count the caller is waiting for
     * @param [in] timeoutMs              - Upper bound on the wait, in milliseconds
     *
     * @return bool                               - Whether the count was reached within the bound
     * @retval true                               - At least expected notifications have arrived, and
     *                                              each of their decodes has completed
     * @retval false                              - The bound expired first, which for a negative
     *                                              case is the expected outcome
     */
    bool WaitForNotification(int expected, int timeoutMs) const
    {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this, expected]() { return notifications >= expected; });
    }

    /**
     * @brief How many frames have been delivered to this listener.
     *
     * Read after a wait to distinguish "exactly one arrived" from "more than one arrived", which a
     * predicate wait on its own cannot tell apart.
     *
     * @return int                                - Number of notifications delivered so far
     */
    int Notifications() const
    {
        std::lock_guard<std::mutex> guard(mutex);
        return notifications;
    }

    /**
     * @brief How many delivered frames had a decode that threw.
     *
     * Zero on every healthy delivery.  A non-zero value means a frame reached this listener and the
     * decoder could not interpret it, which is a different failure from "no frame arrived" and has
     * to read differently in a log - hence a counter of its own rather than a silent catch.
     *
     * @return int                                - Number of contained decode failures
     */
    int DecodeFailures() const
    {
        std::lock_guard<std::mutex> guard(mutex);
        return decodeFailures;
    }

    /**
     * @brief The most recent decode failure's text.
     *
     * Used only to make a DecodeFailures() assertion's message say what actually went wrong.
     *
     * @return std::string                        - The text of the most recent contained decode
     *                                              failure, empty when there has been none
     */
    std::string LastDecodeError() const
    {
        std::lock_guard<std::mutex> guard(mutex);
        return lastDecodeError;
    }

    /**
     * @brief The thread that delivered the most recent notification.
     *
     * A default-constructed id compares equal to no running thread, so an assertion that it differs
     * from the test thread's id would pass vacuously - which is why every case that reads this
     * first establishes that a notification actually arrived.
     *
     * @return std::thread::id                    - Id of the delivering thread, default-constructed
     *                                              when nothing has been delivered yet
     */
    std::thread::id NotifyingThread() const
    {
        std::lock_guard<std::mutex> guard(mutex);
        return notifyingThread;
    }

private:
    /**
     * @brief State published under mutex by notify() and read under it by every accessor above.
     *
     * Mutable because FrameListener::notify() is const on both arms, and guarded rather than atomic
     * because the decode and the publication have to be one critical section: notifications is
     * written last, and it is the predicate WaitForNotification waits on.
     */
    MessageDecoder decoder;
    mutable std::mutex mutex;
    mutable std::condition_variable condition;
    mutable int notifications;
    mutable int decodeFailures;
    mutable std::string lastDecodeError;
    mutable CECFrame lastFrame;
    mutable std::thread::id notifyingThread;
};

/**
 * @brief An open Connection whose listener is detached and whose close happens on every exit path.
 *
 * This exists because a fatal assertion returns from the test body immediately, and
 * Connection::~Connection() has an empty body.
 *
 * The hazard is concrete rather than hygienic.  Connection::open() registers the connection's own
 * busFrameListener with the Bus, and only Connection::close() removes it - the destructor removes
 * nothing.  A case that added a stack-allocated DecodingFrameListener and then hit an ASSERT_*
 * before its close() therefore returns with the Bus still holding a pointer to a listener that is
 * about to be destroyed, and with the connection still registered on the Bus.  The next frame the
 * reader thread delivers - from any later case, or from the same one's own in-flight injection -
 * is dispatched through those dead pointers.  That is undefined behaviour on the Bus reader thread,
 * so it does not present as the failed assertion that caused it: it presents as a crash or a
 * corruption somewhere else, in a case that did nothing wrong, and the first failure's cause is
 * lost.
 *
 * A trailing close() at the end of a body cannot fix that, because the whole point of a fatal
 * assertion is that the statements after it do not run.  A destructor is what runs on every exit
 * path - a normal return, a fatal assertion's early return, and an exception escaping the body
 * alike - which is why cleanup lives here and not in the cases.
 *
 * It closes the connection and does not throw.  Connection::close() clears the connection's frame
 * listeners and removes its bus listener, so it alone is sufficient; removeFrameListener() is
 * called first anyway, so that the case's own listener is detached before the connection's own
 * teardown touches anything.  Everything is wrapped, because a destructor that threw during
 * unwinding would terminate the process and replace a reported failure with an unexplained abort -
 * and a failure to clean up is reported through ADD_FAILURE() rather than swallowed, so a close that
 * did not work is visible.
 *
 * @warning A listener registered through addFrameListener() must outlive this guard, so a case
 *          declares its listener before the guard and lets declaration order unwind the two in the
 *          opposite order.
 *
 * @see Connection::close()
 * @see DecodingFrameListener
 */
class ScopedConnection {
public:
    /**
     * @brief Opens a Connection on the given logical address, named for the log.
     *
     * Always opened, because every case in this tier wants an open connection; a case that wanted a
     * closed one would not need this guard at all.
     *
     * @param [in] source                 - Logical address this connection filters for
     * @param [in] name                   - Name the CEC log identifies the connection by
     */
    ScopedConnection(const LogicalAddress& source, const char* name)
        : conn(source, true, name)
        , attached(nullptr)
        , released(false)
    {
    }

    /**
     * @brief Detaches the listener and closes the connection, on whichever exit path is taken.
     *
     * @return None
     *
     * @post The Bus holds no pointer to this connection or to the listener that was registered
     *       through this guard.
     */
    ~ScopedConnection()
    {
        release();
    }

    /**
     * @brief The connection itself, for a case that needs to send on it or address it.
     *
     * @return Connection&                        - Reference to the connection this guard owns,
     *                                              valid until the guard is released or destroyed
     */
    Connection& connection()
    {
        return conn;
    }

    /**
     * @brief Registers a frame listener and remembers it, so that release() can detach it.
     *
     * @param [in] listener               - Listener to register.  Must outlive this guard, which is
     *                                      why a case declares it before the guard
     */
    void addFrameListener(FrameListener* listener)
    {
        conn.addFrameListener(listener);
        attached = listener;
    }

    /**
     * @brief Detaches the listener and closes the connection.  Idempotent.
     *
     * Called by the destructor, and callable early by a case that needs the connection gone before
     * its remaining assertions - the second call then does nothing, so an early release and the
     * destructor cannot both close.
     *
     * @return None
     *
     * @post The Bus holds no pointer to this connection or to the listener that was registered
     *       through this guard.
     */
    void release()
    {
        if (released) {
            return;
        }
        released = true;

        try {
            if (attached != nullptr) {
                conn.removeFrameListener(attached);
                attached = nullptr;
            }
            conn.close();
        }
        catch (const std::exception& e) {
            ADD_FAILURE() << "closing a test Connection raised " << e.what()
                          << "; the Bus may still hold a pointer to a listener that is going out of "
                             "scope";
        }
        catch (...) {
            ADD_FAILURE() << "closing a test Connection raised an exception not derived from "
                             "std::exception; the Bus may still hold a pointer to a listener that is "
                             "going out of scope";
        }
    }

private:
    /**
     * @brief The connection this guard owns, the listener it has to detach, and the idempotence flag.
     *
     * attached is nullptr until addFrameListener() runs, so release() knows whether there is anything
     * to detach; released makes an early release() and the destructor's release() safe together.
     */
    Connection conn;
    FrameListener* attached;
    bool released;
};

/**
 * @brief Takes the CEC library down and guarantees it comes back up, on every exit path.
 *
 * This touches process-global state that every other case in this binary shares, which is why the
 * restoration is a destructor and not a statement at the end of a body.
 *
 * Exactly one case needs it, and it needs it because there is no alternative: the requirement is
 * that a frame delivered while the driver is not OPENED is rejected, and the driver's state is owned
 * by the library, not by a fixture.  LibCCEC::term() is the only way to leave OPENED, and it stops
 * the Bus and closes the HAL for the whole process while it is done.  The L1 async unit records the
 * same disposition for the same reason: exactly one case there cycles the shared library, because it
 * has no alternative either.
 *
 * A fatal assertion in the case body returns from it immediately, so a trailing init() would not
 * run and every case after it - and the global environment's own term() - would be asserting against
 * a stack that was never brought back up.  A destructor runs on a normal return, on a fatal
 * assertion's early return and on an exception alike, so that is where the restoration lives.
 *
 * Both halves report rather than throw.  TakeDown() and Restore() hand back a bool and a sentence so
 * that the case can attach its own ASSERT_TRUE and fail at its own line; the destructor's implicit
 * restore reports through ADD_FAILURE(), because a library that could not be re-initialised is a
 * fact the run has to carry even though nothing can be done about it by then.
 *
 * @warning Blocked item B2 applies to any case that uses this.  Cycling reaches Driver::close(),
 *          whose AIDL mapping to IHdmiCec.close is a high-confidence candidate pending owner
 *          confirmation - HdmiCecClose has no mapping-table entry.  A green result here does not
 *          confirm that mapping, and the production method carries the same marker.
 *
 * @see LibCCEC::term()
 * @see DualPathAidlFlowTest::AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheStateGuard
 */
class ScopedCecLibraryCycle {
public:
    /**
     * @brief Constructs a guard that has not yet taken the library down.
     *
     * Construction is inert: nothing process-global is touched until TakeDown() runs, so a case can
     * declare the guard at the top of its body and cycle the library later, and a case that returns
     * before calling TakeDown() restores nothing because nothing came down.
     */
    ScopedCecLibraryCycle()
        : down(false)
    {
    }

    /**
     * @brief Brings the CEC library back up on whichever exit path the case takes.
     *
     * @return None
     *
     * @post The library is initialised, or the run carries an ADD_FAILURE() recording that it is not
     *       and that every later case is asserting against a stack that is down.
     */
    ~ScopedCecLibraryCycle()
    {
        std::string detail;
        if (!Restore(detail)) {
            ADD_FAILURE() << "the CEC library could not be re-initialised after a case took it down: "
                          << detail
                          << ". Every case after this one, and the global environment's own term(), "
                             "is now asserting against a stack that is not up";
        }
    }

    /**
     * @brief Terminates the CEC library, leaving the driver out of OPENED.
     *
     * @param [out] failureDetail         - Receives a diagnostic when this reports failure.
     *                                      Untouched on success
     *
     * @return bool                               - Whether the library was terminated
     * @retval true                               - The Bus is stopped and the HAL is closed
     * @retval false                              - term() raised; failureDetail carries its text
     *
     * @post On success the driver is CLOSED, so anything the HAL delivers must be rejected.
     *
     * @warning On the AIDL back-end this is a real transaction to the host process, so the host must
     *          still be serving. It is, by construction: the harness reaps the host only in TearDown.
     */
    bool TakeDown(std::string& failureDetail)
    {
        try {
            LibCCEC::getInstance().term();
            down = true;
            return true;
        }
        catch (const std::exception& e) {
            /*
             * LibCCEC::term() clears its own initialized flag only after Driver::close() returns, so
             * a close that raised leaves the library still marked initialised. Recording that here is
             * what lets Restore() know not to call init() a second time on a library that never came
             * down.
             */
            failureDetail = std::string("LibCCEC::term() raised: ") + e.what() +
                            ". The library is still marked initialised, so the driver may not have "
                            "left OPENED and the state guard cannot be exercised";
            return false;
        }
        catch (...) {
            failureDetail = "LibCCEC::term() raised an exception not derived from std::exception";
            return false;
        }
    }

    /**
     * @brief Brings the CEC library back up.  Idempotent, and a no-op when TakeDown() did not run.
     *
     * @param [out] failureDetail         - Receives a diagnostic when this reports failure.
     *                                      Untouched on success
     *
     * @return bool                               - Whether the library is up
     * @retval true                               - It is, or it never came down
     * @retval false                              - init() raised; failureDetail carries its text
     *
     * @post The driver is OPENED and the Bus is running, which is the baseline every other case in
     *       this binary assumes.
     */
    bool Restore(std::string& failureDetail)
    {
        if (!down) {
            return true;
        }

        try {
            LibCCEC::getInstance().init("CEC_TEST");
            down = false;
            return true;
        }
        catch (const std::exception& e) {
            failureDetail = std::string("LibCCEC::init() raised: ") + e.what();
            return false;
        }
        catch (...) {
            failureDetail = "LibCCEC::init() raised an exception not derived from std::exception";
            return false;
        }
    }

private:
    /**
     * @brief Whether TakeDown() reported success, which is what makes Restore() a no-op when it did not.
     *
     * Set only on a term() that returned.  A term() that raised may have left the library still
     * marked initialised, so leaving this false is what stops Restore() calling init() on a library
     * that never came down.
     */
    bool down;
};

} // namespace


/**
 * @brief The selection fixture, and the only one of the three that runs on both arms.
 *
 * It asks nothing of the HAL and nothing of the transport, so it has nothing to skip for: the
 * question it exists to answer - which back-end did the factory resolve to, and is that answer
 * stable - is meaningful under every invocation and is in fact the question that tells a reader
 * of the run whether the other two fixtures did what their names claim.
 *
 * It also holds the one harness-wide guarantee this file checks, and the reason is that same
 * property rather than a shortage of anywhere else to put it.  A guarantee that has to hold on both
 * arms belongs in the only fixture that runs on both arms, and it has to be a fixture that skips for
 * nothing: the two arm-specific fixtures each skip on the opposite invocation, so a case placed in
 * either would go unchecked on exactly the arm where nothing else was watching.  See
 * WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner, whose subject is
 * the harness's own signal disposition, control-channel write and child reaping, and which touches
 * neither back-end.
 *
 * Both casts are computed once in SetUp rather than per case, because the resolved object cannot
 * change: the selection is fixed by the time any body runs, so recomputing them per assertion
 * would suggest a volatility that does not exist.  dynamic_cast is the whole mechanism, and it is
 * legitimate precisely because ccec/src/DriverImpl.hpp and ccec/src/DriverAidlImpl.hpp are not
 * installed headers - no production introspection API is added, and none may be.
 *
 * @see DualPathLegacyFlowTest
 * @see DualPathAidlFlowTest
 */
class DualPathSelectionTest : public ::testing::Test {
protected:
    /**
     * @brief Resolves the singleton once and records which of the two concrete types it is.
     *
     * @return None
     *
     * @post legacyBackEnd and aidlBackEnd each hold either the resolved object or nullptr, and
     *       exactly one of them is non-null on a correctly resolved run.
     */
    void SetUp() override
    {
        Driver &driver = Driver::getInstance();
        legacyBackEnd = dynamic_cast<DriverImpl *>(&driver);
        aidlBackEnd = dynamic_cast<DriverAidlImpl *>(&driver);
    }

    /**
     * @brief Undoes nothing, because this fixture establishes no shared state.
     *
     * @return None
     */
    void TearDown() override
    {
        // Nothing to undo.  This fixture takes no lock, installs no callback, sets no mock
        // expectation and opens no Connection, which is why it is also the fixture that is safe to
        // run first, last or alone.
    }

    /**
     * @brief The resolved singleton viewed as each concrete back-end, nullptr for the one it is not.
     *
     * Populated by SetUp and read by every case in this fixture, so no case calls
     * Driver::getInstance() itself and none can observe a different object than the others.
     */
    DriverImpl *legacyBackEnd = nullptr;
    DriverAidlImpl *aidlBackEnd = nullptr;
};

/**
 * @brief The legacy round-trip fixture - invocation D, driven through the in-process HAL mock.
 *
 * Establishing the route per fixture rather than per case is what makes all six cases behave
 * identically whether they run alone, in file order, or wherever a shuffle puts them, and it is
 * safe for the outbound cases too because it only restores the registration DriverImpl::open()
 * itself installed.
 *
 * @warning The SetUp order below is load-bearing and must not be rearranged.  The inbound cases in
 *          this fixture reach the stack through DriverImpl::DriverReceiveCallback, which resolves
 *          its target with static_cast<DriverImpl &>(Driver::getInstance()).  That cast is
 *          ill-typed - undefined behaviour - whenever the factory has resolved to the AIDL
 *          back-end, and tests/L2Tests/test_main.cpp installs this same legacy mock on both arms,
 *          so mock->rxCallback is writable on invocation E as well.  The back-end check therefore
 *          comes before the route is installed, and the installation is unreachable when the check
 *          skips.
 *
 * @see restoreDriverInboundRoute()
 * @see DriverImpl::DriverReceiveCallback()
 */
class DualPathLegacyFlowTest : public ::testing::Test {
protected:
    /**
     * @brief Resolves the HAL mock, clears inherited expectations, confirms the arm, then installs
     *        the legacy inbound route.
     *
     * @return None
     *
     * @pre The global test environment has created the HdmiCecDriverMock singleton.
     *
     * @post On the legacy arm the driver's receive callback points at
     *       DriverImpl::DriverReceiveCallback and no expectation from an earlier case survives.
     *
     * @warning Steps (3) and (4) are ordered and not interchangeable, for the reason the fixture's
     *          own warning gives.
     */
    void SetUp() override
    {
        // (1) The HAL double every case in this fixture drives.  Created by the global
        //     environment, which runs before any fixture, so its absence is a harness failure
        //     rather than a condition to work around.
        mock = HdmiCecDriverMock::getInstance();
        ASSERT_NE(mock, nullptr) << "the global test environment must have created the driver mock";

        // (2) Start from no inherited expectation: this binary shares one driver mock across every
        //     fixture, and a surviving EXPECT_CALL would be attributed to a case that never set it.
        ::testing::Mock::VerifyAndClearExpectations(mock);

        // (3) Confirm the resolved back-end is the legacy one, before touching the mock's callback
        //     members.  This is not a convenience check and it is not interchangeable with step (4)
        //     below - see the fixture's doc block.  GTEST_SKIP returns from SetUp, so step (4) is
        //     genuinely unreachable on the AIDL arm rather than merely discouraged.
        if (dynamic_cast<DriverImpl *>(&Driver::getInstance()) == nullptr) {
            GTEST_SKIP() << "this fixture is invocation D, which requires the legacy back-end; the "
                            "AIDL back-end was selected instead, so the legacy inbound route must "
                            "NOT be installed - DriverImpl::DriverReceiveCallback resolves through "
                            "static_cast<DriverImpl &>(Driver::getInstance()) at "
                            "ccec/src/DriverImpl.cpp:70, which is undefined behaviour against an "
                            "AIDL singleton. Run with CEC_TEST_AIDL_MODE=absent to execute these "
                            "cases";
        }

        // (4) Only now, and only on the legacy arm.
        restoreDriverInboundRoute(mock);
    }

    /**
     * @brief Verifies and clears the shared mock's expectations, tolerating a SetUp that skipped.
     *
     * @return None
     *
     * @post No expectation set by this case can be attributed to the next one.
     */
    void TearDown() override
    {
        // Leave no expectation behind for the next case, and tolerate a SetUp that skipped before
        // the mock was resolved.
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }
    }

    /**
     * @brief The shared HAL double every case in this fixture drives, or nullptr if SetUp skipped
     *        before resolving it.
     */
    HdmiCecDriverMock *mock = nullptr;
};

/**
 * @brief The AIDL round-trip fixture - invocation E, driven over real out-of-process binder IPC.
 *
 * It requires the factory to have resolved to the AIDL back-end, which requires the fake service
 * host to have been launched and to have reported ready before LibCCEC::init ran, which requires
 * CEC_TEST_AIDL_MODE=remote and a binder-capable kernel with a running servicemanager.  None of
 * that can be arranged from inside a test body, so the fixture skips when the arm is not its own
 * rather than failing: that is what keeps one registered case count valid for both invocation D
 * and invocation E, which is in turn what lets the coverage runner gate both on a single expected
 * number.
 *
 * @warning Nothing in this fixture may touch the legacy HAL mock: not mock->rxCallback, not
 *          mock->rxCallbackData, not injectReceivedMessage(), not simulateTxResult(), and no
 *          EXPECT_CALL on any HdmiCec* entry point.  The mock exists here -
 *          tests/L2Tests/test_main.cpp installs it unconditionally on both arms - so every one of
 *          those is compilable and reachable, which is exactly why the prohibition has to be stated
 *          rather than assumed.  Two outcomes follow, neither acceptable: an expectation or an
 *          injected transmit result does nothing, because the AIDL back-end never calls the legacy C
 *          entry points, and a case built on it would assert against a mock nobody drives; while
 *          writing rxCallback arms the undefined-behaviour cast documented on
 *          DualPathLegacyFlowTest above.  The AIDL arm's stimulus comes from the host process and
 *          from nowhere else.
 *
 * @see cecL2HostControlChannelIsOpen()
 * @see DualPathLegacyFlowTest
 */
class DualPathAidlFlowTest : public ::testing::Test {
protected:
    /**
     * @brief Confirms the arm, then requires the host control and observation channel to be open.
     *
     * @return None
     *
     * @pre The harness launched the fake service host and the host reported ready before
     *      LibCCEC::init resolved the selection.
     *
     * @post Every case in this fixture can read what the fake service received and can cause an
     *       inbound delivery.
     *
     * @warning The two steps have deliberately different dispositions: a wrong arm is a skip,
     *          because the cases have no subject; an absent channel is a failure, because the arm
     *          that was selected implies the channel exists.
     */
    void SetUp() override
    {
        // (1) The only skip in this fixture, and it is an opposite-arm skip: this fixture is
        //     invocation E and the resolved back-end is invocation D's.  Every case below then has
        //     no subject at all - there is no AIDL session, no host process and no channel - so
        //     skipping is the honest report and adapting would be a fiction.
        if (dynamic_cast<DriverAidlImpl *>(&Driver::getInstance()) == nullptr) {
            GTEST_SKIP() << "this fixture is invocation E, which requires the AIDL back-end; the "
                            "legacy back-end was selected instead. Run with "
                            "CEC_TEST_AIDL_MODE=remote, with CEC_FAKE_AIDL_HOST_PATH naming the "
                            "fake_hdmi_cec_aidl_host binary, on a binder-capable kernel with a "
                            "running servicemanager, so that the host is published and ready "
                            "before LibCCEC::init resolves the selection";
        }

        // (2) The channel is a precondition of every case in this fixture, and its absence is a
        //     failure and not a skip.  The AIDL back-end resolved, so a host process was published
        //     and ready before init - and this harness always hands that host a control and
        //     observation channel and proves it with a ping before initializing.  A channel that is
        //     not open here therefore contradicts the arm that was selected, which is a defect in
        //     the harness or the host and not a platform this tier cannot run on.  Skipping on it
        //     would let invocation E report green while every observation it exists to make was
        //     quietly not made - the precise shape of failure the tier is built to rule out.
        ASSERT_TRUE(cecL2HostControlChannelIsOpen())
            << "the AIDL back-end was selected, so an out-of-process fake service host is serving "
               "this run, but its control and observation channel is not open. Without it no case in "
               "this fixture can read what the service actually received or cause an inbound "
               "delivery: an outbound case would collapse into \"sendTo did not throw\", which a "
               "dropped or corrupted send satisfies, and the inbound cases have no trigger at all. "
               "tests/L2Tests/test_main.cpp creates both pipes before the fork, clears FD_CLOEXEC on "
               "the child's copies between fork() and exec(), names them in CEC_FAKE_HOST_CONTROL_FD "
               "and CEC_FAKE_HOST_OBSERVE_FD, and pings the host before LibCCEC::init; its trace and "
               "the host's own [FakeHdmiCecAidlHost] trace name the step that failed";
    }

    /**
     * @brief Undoes nothing shared, because each case owns and releases its own connection.
     *
     * @return None
     */
    void TearDown() override
    {
        // Nothing shared to undo.  Each case owns and releases its own Connection and listener,
        // and this fixture sets no mock expectation - deliberately, per the prohibition above.
    }
};


// ---------------------------------------------------------------------------------------------
// Selection - which back-end resolved, that it stays resolved, and that it is the one asked for -
// followed by the one guarantee about the harness that has to hold on both arms.  All four run
// under every invocation.
// ---------------------------------------------------------------------------------------------

/**
 * @brief The factory resolved to exactly one of the two back-ends.
 *
 * Establishes that the two back-ends are independent siblings of the Driver interface rather than
 * layers stacked on each other.  Runs under every invocation and requires neither back-end in
 * particular; the evidence is the pair of dynamic_cast results the fixture computed in SetUp, one of
 * which must succeed and the other must fail.
 *
 * Both halves are asserted, and that is the whole design of the case.  Checking only that one
 * cast succeeded would pass against a hierarchy in which both succeed - a single class inheriting
 * from both implementations, say, or one made a base of the other - and such a hierarchy would
 * break the migration's central property that the two back-ends are siblings selected between,
 * not layers stacked on each other.  Asserting that the other cast fails is what pins that.
 *
 * It also establishes the precondition the two arm-specific fixtures rely on: exactly one of them
 * will run its cases and the other will skip, never both and never neither.
 */
TEST_F(DualPathSelectionTest, TheFactoryResolvedToExactlyOneBackEnd)
{
    const bool isLegacy = (legacyBackEnd != nullptr);
    const bool isAidl = (aidlBackEnd != nullptr);

    EXPECT_TRUE(isLegacy || isAidl)
        << "Driver::getInstance() returned an object that is neither DriverImpl nor "
           "DriverAidlImpl; the factory has resolved to a third implementation, or RTTI is not "
           "available in this build";
    EXPECT_FALSE(isLegacy && isAidl)
        << "the same object cast successfully to BOTH concrete back-ends, so one of them derives "
           "from the other or from a common concrete class; the two must remain independent "
           "siblings of the Driver interface";
}

/**
 * @brief Repeated Driver::getInstance() calls return the same object, so the selection is stable
 *        for the lifetime of the process.
 *
 * Establishes the "resolved once at initialization and held for the process lifetime" requirement.
 * Runs under every invocation and requires neither back-end in particular; the evidence is object
 * identity across repeated factory calls, plus the dynamic type still matching what SetUp recorded.
 *
 * This is the "resolved once at initialization and held stable" requirement, asserted at the L2
 * tier over a real process lifetime rather than inside a unit fixture: by the time this body runs,
 * the library has been initialized, the driver has been opened, the Bus threads are running and
 * an unknown number of factory calls have already been made from production code.  If the factory
 * were to re-resolve - because a service appeared, or because the static initializer were re-run -
 * this is where it would show.
 *
 * No service is registered or deregistered here, deliberately.  The fake is not linked into this
 * runner, so registration is not available to this file at all; and the pinned C++ IServiceManager
 * exposes no service-removal API, so a case written around unregistering could not be implemented
 * even where the fake was linked.  The stability that can honestly be asserted is object identity,
 * and that is what is asserted.
 */
TEST_F(DualPathSelectionTest, RepeatedGetInstanceCallsReturnTheSameObject)
{
    Driver *const first = &Driver::getInstance();
    ASSERT_NE(first, nullptr) << "Driver::getInstance() returned a reference to nothing";

    for (int call = 0; call < 5; call++) {
        EXPECT_EQ(first, &Driver::getInstance())
            << "Driver::getInstance() returned a different object on call " << (call + 2)
            << "; the selection must resolve once and be held for the lifetime of the process";
    }

    // And it is still the same concrete back-end, not merely the same address: a stable address
    // with a changed dynamic type would be a far stranger defect, and this is what would catch it.
    Driver &again = Driver::getInstance();
    EXPECT_EQ(legacyBackEnd, dynamic_cast<DriverImpl *>(&again))
        << "the legacy identity of the resolved back-end changed between calls";
    EXPECT_EQ(aidlBackEnd, dynamic_cast<DriverAidlImpl *>(&again))
        << "the AIDL identity of the resolved back-end changed between calls";
}

/**
 * @brief The back-end that resolved is the one the harness was asked for.
 *
 * Establishes the agreement between the requested arm and the resolved arm, which is what makes a
 * green invocation D or E mean that the intended arm ran.  Runs under every invocation; the evidence
 * is CEC_TEST_AIDL_MODE read for observation only, checked against the fixture's recorded casts.
 *
 * This is what makes a green invocation D or E mean "the intended arm actually ran" rather than
 * merely "some arm ran".  Without it, a misconfigured invocation that silently exercised the
 * legacy path would report green and be filed as AIDL evidence - the exact failure the L2 harness
 * is built to rule out, arrived at from the reporting side instead of the setup side.
 *
 * This is the only place in this file that reads CEC_TEST_AIDL_MODE, and it reads it to observe,
 * never to decide.  tests/L2Tests/test_main.cpp owns acting on the variable, and it does so before
 * LibCCEC::init resolves the selection; nothing a test body could do afterwards would change the
 * outcome anyway.  Only two values can be seen here: that harness treats "compatible" and
 * "incompatible" as a hard failure naming run_L1Tests, and any unrecognised value as a hard
 * failure too, so a run that reached this body has either "absent", "remote", or nothing set.
 *
 * An unset or empty variable skips rather than assuming a default.  The harness does document
 * unset as meaning absent, and a bare ./run_L2Tests is a legitimate way to run the legacy arm -
 * but this case asserts an agreement between a request and an outcome, and where no request was
 * made there is nothing to agree with.  It costs the invocation matrix nothing: the coverage
 * runner sets the variable explicitly for both D and E.
 */
TEST_F(DualPathSelectionTest, TheResolvedBackEndMatchesTheModeTheHarnessWasGiven)
{
    const char *const requestedMode = std::getenv("CEC_TEST_AIDL_MODE");
    if ((requestedMode == nullptr) || (requestedMode[0] == '\0')) {
        GTEST_SKIP() << "CEC_TEST_AIDL_MODE is unset or empty, so no back-end was requested and "
                        "there is no request for this case to hold the outcome against; the "
                        "harness treats that as the legacy arm, and the invocation matrix sets the "
                        "variable explicitly - absent for invocation D, remote for invocation E";
    }

    const std::string mode(requestedMode);
    if (mode == "absent") {
        EXPECT_NE(legacyBackEnd, nullptr)
            << "CEC_TEST_AIDL_MODE=absent asked for the legacy back-end, but the factory resolved "
               "to something else; a service was reachable when none should have been";
        EXPECT_EQ(aidlBackEnd, nullptr)
            << "CEC_TEST_AIDL_MODE=absent asked for the legacy back-end, but the AIDL back-end was "
               "selected; this run is not invocation D whatever its results say";
    }
    else if (mode == "remote") {
        EXPECT_NE(aidlBackEnd, nullptr)
            << "CEC_TEST_AIDL_MODE=remote asked for the AIDL back-end, but the factory resolved to "
               "the legacy one; the fake service host was not published and compatible before "
               "LibCCEC::init ran, so this run proves nothing about the AIDL path";
        EXPECT_EQ(legacyBackEnd, nullptr)
            << "CEC_TEST_AIDL_MODE=remote asked for the AIDL back-end, but the legacy back-end was "
               "selected; this run is not invocation E whatever its results say";
    }
    else {
        FAIL() << "CEC_TEST_AIDL_MODE is set to \"" << mode << "\", which this tier does not "
                  "implement. The in-process modes compatible and incompatible belong to "
                  "run_L1Tests, and tests/L2Tests/test_main.cpp is expected to have refused this "
                  "value before any case ran";
    }
}

/**
 * @brief The harness's own control-channel write reports EPIPE, and its child is still reaped,
 *        instead of this runner being killed.
 *
 * Establishes that a broken control pipe produces a named diagnostic and a reaped child rather than
 * a dead runner.  Runs under every invocation and requires neither back-end, no fake service host,
 * no binder driver and no service manager, because the seam builds its own hazard; the evidence is
 * SIGPIPE's disposition read back out of the process, the harness's own writeControlCommand()
 * diagnostic, and a real child ended through the same terminate-and-reap a teardown performs.
 *
 * The property, and why it is a case rather than a comment: this harness writes commands to a pipe
 * whose reader is the fake service host - another process, which can exit or close its read end at
 * any moment.  Under SIGPIPE's default disposition that write does not return: the runner is killed
 * at the call, and two things this tier promises die with it.  The EPIPE arm of
 * writeControlCommand() would never run, so the failure it exists to name would be recorded nowhere
 * and the run would read as a crash of unknown origin.  And the global environment's TearDown would
 * never run, so the host would be neither signalled nor reaped: it would survive the run holding the
 * production service name, and the next run would fail on a stale registration.  So
 * tests/L2Tests/test_main.cpp installs SIG_IGN as the first step of its SetUp and restores the
 * previous disposition in TearDown, and this case is what notices if that install is ever removed -
 * and, more than that, what proves the two things the install buys are actually there.
 *
 * It drives the real code, which is the whole point.  Both promises above are claims about code that
 * runs only when a pipe's reader has gone, and no ordinary invocation ever puts it in that state: a
 * healthy host reads its control descriptor until teardown closes it.  A case that instead built a
 * look-alike - its own pipe, its own raw ::write(), its own errno - would establish the kernel's
 * behaviour and this process's signal disposition, neither of which is in doubt, while leaving
 * writeControlCommand()'s diagnostic and the reap that follows it entirely unexercised.  So step (2)
 * below calls cecL2ProveEpipeDiagnosticAndChildReaping(), which makes a real call to the harness's
 * own writeControlCommand() against a descriptor whose reader has genuinely gone, requires the
 * command-specific EPIPE sentence back, and then ends a real child through the same
 * terminateAndReapChildProcess() a teardown uses.  Neither is a copy; a change that broke either
 * breaks this case.
 *
 * It is deterministic on any host, and that is the whole of its design.  It needs no fake service
 * host, no binder driver, no service manager and no back-end: the seam builds the hazard itself out
 * of two pipes and a child of its own, and close() on a pipe's last read end followed by write() to
 * its write end returns -1 with EPIPE synchronously, while SIGTERM to a child at SIG_DFL ends it.
 * Nothing is slept on and no wall clock is polled - each wait in the seam is a real wait on a real
 * event under one bound - so this behaves identically under invocation D on a host with no binder
 * support, which is where it ordinarily runs, and under invocation E on the binder-capable guest.
 * Everything it uses is its own, so nothing it does can disturb the harness's live channel or the
 * host's descriptors, and it leaves no descriptor and no child behind on any path.
 *
 * Three steps, in this order, because each one licenses the next:
 *
 *   (1) The disposition is read back out of the process and must not be SIG_DFL.  This is the
 *       precondition, and it comes first and fatally: if the disposition were the default, the
 *       writes in the two steps below would terminate this runner mid-case, and a suite cannot
 *       report on the thing that killed it.  It is asserted as "not the default" rather than
 *       "exactly SIG_IGN" on purpose - the property required is that a broken pipe does not kill
 *       this process, which a handler satisfies as well as SIG_IGN does, and pinning the exact value
 *       would fail a future harness that met the requirement differently.
 *
 *   (2) The real write and the real reap, which is the substance.  The seam's own report is
 *       asserted, and then the diagnostic it hands back is asserted here, at this case's own line,
 *       for the two things that make it worth anything: it names the command that was lost, and it
 *       reports EPIPE rather than a bound that expired.
 *
 *   (3) The consequence, demonstrated directly, on a pipe belonging to this case.  It adds the one
 *       observation step (2) makes indirectly: write() returns, returns -1, and sets EPIPE.  It is
 *       kept because it is the cheapest possible statement of the mechanism the seam relies on, and
 *       it is labelled as secondary because on its own it would prove nothing about the harness.
 *
 * Reaching the assertions at all is the rest of the evidence and cannot be written as an assertion:
 * a process that had died would report nothing.
 */
TEST_F(DualPathSelectionTest,
       WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner)
{
    /*
     * (1) The disposition, read straight out of the process. A null action pointer makes this a
     *     query and changes nothing, so this case observes the harness's choice rather than
     *     establishing one of its own - which matters, because a case that installed the
     *     disposition itself would pass whether or not the harness had.
     */
    struct sigaction current;
    std::memset(&current, 0, sizeof(current));

    ASSERT_EQ(0, ::sigaction(SIGPIPE, nullptr, &current))
        << "SIGPIPE's current disposition could not be read (" << std::strerror(errno)
        << "), so whether this runner survives a write to a closed pipe cannot be established - and "
           "the writes below must not be attempted without knowing";

    const bool sigpipeIsDefaulted = (current.sa_handler == SIG_DFL);
    ASSERT_FALSE(sigpipeIsDefaulted)
        << "SIGPIPE is at its DEFAULT disposition, which TERMINATES the process. The L2 harness "
           "writes control commands to a pipe whose reader is the out-of-process fake service host, "
           "so with this disposition in force a host that has exited kills this runner at the "
           "write(): writeControlCommand()'s EPIPE diagnostic never runs, so no failure is recorded, "
           "and the global environment's TearDown never runs, so the host is neither signalled nor "
           "reaped and outlives the run holding the production service name for the next one. "
           "tests/L2Tests/test_main.cpp must install SIG_IGN as the first step of "
           "CecL2TestEnvironment::SetUp - see ignoreBrokenPipeSignal() - and restore it in TearDown";

    /*
     * (2) The substance. The seam drives the harness's own writeControlCommand() against a
     *     descriptor whose reader has genuinely gone, and then ends its own child through the same
     *     terminate-and-reap a teardown performs on the host. Both are the real functions, so this
     *     is the assertion that makes the SIGPIPE install's two purposes - a diagnostic that gets
     *     produced, and a reap that gets performed - facts about this binary rather than claims
     *     about it.
     */
    std::string observedDiagnostic;
    std::string seamFailure;

    ASSERT_TRUE(cecL2ProveEpipeDiagnosticAndChildReaping(observedDiagnostic, seamFailure))
        << "driving the harness's own control-channel write against a reader-less descriptor, and "
           "reaping the child that made it reader-less, did not hold: "
        << seamFailure
        << ". The step named there is the one to look at; every step the seam takes is necessary to "
           "the two properties this case exists to establish, and the seam leaves no descriptor and "
           "no child behind whichever one failed";

    /*
     *     The diagnostic is then asserted here rather than only inside the seam, so that a wrong
     *     sentence fails at this case's line with the sentence in the message. The command text is
     *     spelled again in this file on purpose: it is a two-place contract exactly like the extern
     *     declarations above, and if the seam ever sends a different command this assertion fails
     *     and both places are updated together.
     */
    EXPECT_NE(std::string::npos, observedDiagnostic.find("epipe-probe"))
        << "the harness's control-channel write failed as required, but its diagnostic does not "
           "name the command that was lost. It said: \"" << observedDiagnostic
        << "\". A failure that does not say WHICH command went missing leaves the reader of a CI log "
           "unable to tell which observation a case never got";

    EXPECT_NE(std::string::npos, observedDiagnostic.find("EPIPE"))
        << "the harness's control-channel write failed and named the command, but its diagnostic "
           "does not report EPIPE, so it did not take the broken-pipe arm. It said: \""
        << observedDiagnostic
        << "\". EPIPE is the one errno that means \"the host has closed its control descriptor or "
           "exited\", and the deadline arm of the same function also names the command - so without "
           "EPIPE this could be a bound that expired, which is a different failure entirely";

    /*
     * (3) The mechanism, demonstrated directly and labelled as the secondary step it is: a pipe of
     *     this case's own, its read end closed, and one byte offered to the write end. O_CLOEXEC
     *     because every descriptor this tier creates carries it - nothing here forks, but a
     *     descriptor that would survive an exec for no reason is the kind of difference that later
     *     becomes a bug.
     *
     *     Both ends are released before anything is asserted about the outcome, so that no path out
     *     of this case - including a fatal assertion's early return - leaks a descriptor into the
     *     rest of the binary.
     */
    int probeChannel[2] = { -1, -1 };
    ASSERT_EQ(0, ::pipe2(probeChannel, O_CLOEXEC))
        << "a pipe could not be created (" << std::strerror(errno)
        << "), so the mechanism this step exists to demonstrate could not be built";

    const int readEndCloseResult = ::close(probeChannel[0]);
    const int readEndCloseErrno  = errno;

    ssize_t writeResult = 0;
    int writeErrno = 0;

    if (readEndCloseResult == 0) {
        const char probeByte = 'x';

        errno = 0;
        writeResult = ::write(probeChannel[1], &probeByte, sizeof(probeByte));
        writeErrno  = errno;
    }

    ::close(probeChannel[1]);

    ASSERT_EQ(0, readEndCloseResult)
        << "the read end of this step's own pipe could not be closed ("
        << std::strerror(readEndCloseErrno)
        << "), so the pipe still has a reader and a successful write below would mean nothing";

    EXPECT_EQ(-1, writeResult)
        << "writing one byte to a pipe whose only reader has been closed returned " << writeResult
        << " where -1 was required. A write that reports success into a pipe nobody can read means "
           "the descriptor is not the one this step created, and the EPIPE arm the harness's control "
           "channel depends on could not be reached even with the right disposition in force";

    EXPECT_EQ(EPIPE, writeErrno)
        << "the write to a reader-less pipe failed with errno " << writeErrno << " ("
        << std::strerror(writeErrno) << ") where EPIPE (" << EPIPE
        << ") was required. EPIPE is the exact error writeControlCommand() classifies as \"the host "
           "has closed its control descriptor or exited\", and it is the only errno that carries "
           "that meaning";

    /*
     * Reaching this line is the rest of the evidence and cannot be expressed as an assertion: under
     * the default disposition this process would have been terminated by the seam's write in step
     * (2) or by the write in step (3), so none of the assertions above would have been reported by
     * anybody. That they were reported at all is the property this case exists to establish.
     */
}


// ---------------------------------------------------------------------------------------------
// Flow A on the legacy back-end - inbound, from the HAL Rx callback to the typed process()
// overload.  Invocation D.
// ---------------------------------------------------------------------------------------------

/**
 * @brief A directed <Image View On> injected at the legacy HAL arrives decoded, at the right
 *        listener, with the right header.
 *
 * Establishes the middleware leg of flow A on the legacy back-end.  Requires invocation D and the
 * resolved back-end to be DriverImpl, which the fixture confirms before it installs the inbound
 * route; the evidence is the recording processor's typed counters and header nibbles, read after the
 * listener has confirmed a notification arrived.
 *
 * Frame { 0x40, 0x04 }: initiator 4 (Playback Device 1) in the high nibble, destination 0 (TV) in
 * the low nibble, opcode 0x04 (<Image View On>).  The Connection is opened as logical address 0,
 * so the destination nibble matches it and Connection's filter must let the frame through.
 *
 * What is asserted is the whole middleware leg of flow A on this back-end: not merely that a frame
 * reached a listener, but that it reached the listener still decodable as <Image View On> and with
 * its header nibbles intact.
 */
TEST_F(DualPathLegacyFlowTest, InboundImageViewOnReachesTheTypedProcessorThroughTheLegacyBackEnd)
{
    RecordingProcessor processor;
    DecodingFrameListener listener(processor);

    // The guard is declared after the listener, so it is destroyed before it: the Bus has let go
    // of both the connection and the listener before the listener's storage dies, on every exit
    // path including a fatal assertion's early return.
    ScopedConnection scoped(LogicalAddress::TV, "L2-Legacy-FlowA-ImageViewOn");
    scoped.addFrameListener(&listener);

    const unsigned char frame[] = { 0x40, 0x04 };
    mock->injectReceivedMessage(frame, static_cast<int>(sizeof(frame)));

    EXPECT_TRUE(listener.WaitForNotification(1, 3000))
        << "no frame reached the listener; the HAL -> DriverImpl -> receive queue -> Bus reader -> "
           "Connection path is broken on the legacy back-end";

    EXPECT_EQ(1, processor.imageViewOnCount)
        << "the frame arrived but did not decode to <Image View On>";
    EXPECT_EQ(0, processor.textViewOnCount)
        << "the frame decoded to <Text View On>, so the opcode was altered in transit";
    EXPECT_EQ(0, processor.activeSourceCount)
        << "the frame decoded to <Active Source>, so the opcode was altered in transit";
    EXPECT_EQ(0, processor.standbyCount)
        << "the frame decoded to <Standby>, so the opcode was altered in transit";
    EXPECT_EQ(static_cast<int>(LogicalAddress::PLAYBACK_DEVICE_1), processor.lastInitiator)
        << "the initiator nibble was lost or rewritten on the way up";
    EXPECT_EQ(static_cast<int>(LogicalAddress::TV), processor.lastDestination)
        << "the destination nibble was lost or rewritten on the way up";
}

/**
 * @brief A broadcast <Active Source> arrives decoded with its operands intact on the legacy
 *        back-end.
 *
 * Establishes that operand bytes survive the inbound leg exactly, not merely that something
 * arrived.  Requires invocation D and the resolved back-end to be DriverImpl; the evidence is the
 * decoded physical address read back in three views - rendered, per nibble, and as the two packed
 * bytes.
 *
 * Frame { 0x4F, 0x82, 0x10, 0x00 }: initiator 4, destination 0xF (broadcast), opcode 0x82
 * (<Active Source>), physical address 1.0.0.0 packed as 0x10 0x00.  This is the case that catches
 * an operand being dropped or shifted: both operand bytes have to survive the heap copy, the
 * queue, the reader thread, the filter and the decoder to be read back here.
 *
 * What it asserts about the operands is the exact value, in three views - the rendered address
 * "1.0.0.0", the four decoded nibbles, and the two packed bytes 0x10 0x00 - because a check that
 * the address merely arrived non-empty is satisfied by any corruption that still yields two bytes:
 * 0.0.0.0 from a zeroed operand pair, 0.1.0.0 from a nibble shift, and a truncated value all pass
 * it.  Exactness is what makes this case a control on the transport rather than on the fact that
 * something was copied.
 *
 * What it deliberately does not establish: anything about the AIDL arm.  The stimulus is the legacy
 * HAL mock's Rx callback, so this is invocation D's evidence only; the AIDL inbound cases below
 * carry their own, and neither substitutes for the other.
 */
TEST_F(DualPathLegacyFlowTest, InboundBroadcastActiveSourceCarriesItsOperandsThroughTheLegacyBackEnd)
{
    RecordingProcessor processor;
    DecodingFrameListener listener(processor);

    ScopedConnection scoped(LogicalAddress::TV, "L2-Legacy-FlowA-ActiveSource");
    scoped.addFrameListener(&listener);

    const unsigned char frame[] = { 0x4F, 0x82, 0x10, 0x00 };
    mock->injectReceivedMessage(frame, static_cast<int>(sizeof(frame)));

    EXPECT_TRUE(listener.WaitForNotification(1, 3000))
        << "a broadcast frame did not reach the listener on the legacy back-end";

    EXPECT_EQ(1, processor.activeSourceCount)
        << "the frame arrived but did not decode to <Active Source>";
    EXPECT_EQ(0, processor.imageViewOnCount)
        << "the frame decoded to <Image View On>, so the opcode was altered in transit";

    // The operands are asserted exactly, not merely for survival.  "Non-empty" is satisfied by
    // 0.0.0.0, by a shifted 0.1.0.0 and by a truncated value alike, so a corrupted address would
    // have passed.  PhysicalAddress::toString() renders dotted decimal and
    // PhysicalAddress::getByteValue(0..3) yields the four nibbles, so the
    // exact value is available and there is no reason to settle for less.  Both views are checked:
    // the four decoded nibbles, and the two packed bytes 0x10 0x00 that 1.0.0.0 travels as.
    EXPECT_EQ("1.0.0.0", processor.activeSourcePhysical)
        << "the <Active Source> physical address did not decode to 1.0.0.0";
    EXPECT_EQ(1, processor.activeSourceNibbles[0])
        << "the first nibble of physical address 1.0.0.0 was lost or shifted on the inbound path";
    EXPECT_EQ(0, processor.activeSourceNibbles[1])
        << "the second nibble of physical address 1.0.0.0 does not match";
    EXPECT_EQ(0, processor.activeSourceNibbles[2])
        << "the third nibble of physical address 1.0.0.0 does not match";
    EXPECT_EQ(0, processor.activeSourceNibbles[3])
        << "the fourth nibble of physical address 1.0.0.0 does not match";
    EXPECT_EQ(0x10, processor.activeSourcePackedHigh)
        << "physical address 1.0.0.0 packs to 0x10 0x00 and the first operand byte does not match, "
           "so the operand pair that arrived would not re-encode to the wire image it came from";
    EXPECT_EQ(0x00, processor.activeSourcePackedLow)
        << "the second packed operand byte of physical address 1.0.0.0 does not match";

    EXPECT_EQ(static_cast<int>(LogicalAddress::BROADCAST), processor.lastDestination)
        << "the broadcast destination nibble was not preserved";
}

/**
 * @brief A frame addressed to a different logical address is filtered out before any decode happens.
 *
 * Establishes that Connection's address filter runs ahead of the decoder, which is the negative
 * control the two inbound cases above depend on.  Requires invocation D and the resolved back-end to
 * be DriverImpl; the evidence is a wait that is expected to expire, plus a notification count and
 * every typed counter still at zero.
 *
 * The negative control for the two cases above.  Without it, a listener that received every frame
 * regardless of addressing would satisfy both of them.  Frame { 0x43, 0x36 } is initiator 4 to
 * destination 3 (Tuner 1) while the Connection is on logical address 0, so Connection's filter
 * must drop it and the processor must stay untouched.
 *
 * The wait is expected to time out, and it is a full wait rather than an immediate check: a
 * negative has to be given the same opportunity to arrive as a positive, otherwise it only proves
 * the test thread was faster than the Bus reader thread.
 */
TEST_F(DualPathLegacyFlowTest, InboundFrameForAnotherAddressIsFilteredBeforeDecodingOnTheLegacyBackEnd)
{
    RecordingProcessor processor;
    DecodingFrameListener listener(processor);

    ScopedConnection scoped(LogicalAddress::TV, "L2-Legacy-FlowA-Filtered");
    scoped.addFrameListener(&listener);

    const unsigned char frame[] = { 0x43, 0x36 };
    mock->injectReceivedMessage(frame, static_cast<int>(sizeof(frame)));

    EXPECT_FALSE(listener.WaitForNotification(1, 1200))
        << "a frame addressed to logical address 3 was delivered to a connection on address 0";
    EXPECT_EQ(0, listener.Notifications())
        << "the filter let a frame through for another logical address";
    EXPECT_EQ(0, processor.standbyCount)
        << "a filtered frame was decoded, so the filter ran after the decoder rather than before";
    EXPECT_EQ(0, processor.imageViewOnCount)
        << "a filtered frame was decoded as <Image View On>";
    EXPECT_EQ(0, processor.activeSourceCount)
        << "a filtered frame was decoded as <Active Source>";
}

// ---------------------------------------------------------------------------------------------
// Flow B on the legacy back-end - outbound, from a typed message to the exact bytes the HAL is
// handed.  Invocation D.
// ---------------------------------------------------------------------------------------------

/**
 * @brief <Image View On> encoded and sent reaches the legacy HAL as exactly the bytes CEC defines.
 *
 * Establishes the middleware leg of flow B on the legacy back-end.  Requires invocation D and the
 * resolved back-end to be DriverImpl; the evidence is the buffer and length captured from the mock's
 * HdmiCecTx expectation, compared byte for byte against the wire image.
 *
 * The whole outbound leg is synchronous - Connection::sendTo calls into Bus and then
 * DriverImpl::write on the calling thread - so the bytes are already at the HAL when sendTo
 * returns and no wait is needed.  What is asserted is the wire image: header nibbles then opcode,
 * nothing more and nothing less.  Expected bytes { 0x40, 0x04 }: initiator 4 (this Connection's
 * source, Playback Device 1) in the high nibble, destination 0 (TV) in the low nibble.
 */
TEST_F(DualPathLegacyFlowTest, OutboundImageViewOnReachesTheLegacyHalAsExactBytes)
{
    std::vector<unsigned char> captured;
    int capturedLength = -1;

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .WillOnce(DoAll(
            Invoke([&captured, &capturedLength](int, const unsigned char* buf, int len, int*) {
                capturedLength = len;
                captured.assign(buf, buf + len);
            }),
            SetArgPointee<3>(HDMI_CEC_IO_SUCCESS),
            Return(HDMI_CEC_IO_SUCCESS)));

    // RAII, because every assertion below is fatal: an ASSERT_* returns from this body at once,
    // and a trailing close() would then never run while the Bus still held this connection.
    ScopedConnection scoped(LogicalAddress::PLAYBACK_DEVICE_1, "L2-Legacy-FlowB-ImageViewOn");

    CECFrame frame;
    MessageEncoder().encode(ImageViewOn(), frame);
    scoped.connection().sendTo(LogicalAddress(LogicalAddress::TV), frame, 1000);

    ASSERT_EQ(2, capturedLength)
        << "an <Image View On> is a header and an opcode - two bytes - so the HAL was handed the "
           "wrong length or was never called at all";
    ASSERT_EQ(2u, captured.size())
        << "the captured buffer length disagrees with the length the HAL was told";
    EXPECT_EQ(0x40, static_cast<int>(captured[0]))
        << "the header nibbles the HAL received do not match the connection source and destination";
    EXPECT_EQ(0x04, static_cast<int>(captured[1]))
        << "the opcode byte is not <Image View On>";
}

/**
 * @brief <Active Source> is handed to the legacy HAL with its operands in wire order.
 *
 * Establishes that a multi-byte frame's operands reach the HAL in order and unaltered.  Requires
 * invocation D and the resolved back-end to be DriverImpl; the evidence is all four captured bytes,
 * asserted individually rather than by length alone.
 *
 * Four bytes { 0x4F, 0x82, 0x10, 0x00 }: header with the broadcast destination nibble, opcode 0x82
 * (<Active Source>), then physical address 1.0.0.0 packed two digits per byte.  A regression that
 * reordered or dropped an operand between the encoder and the HAL is invisible to a test that only
 * checks the opcode, so all four bytes are asserted.
 */
TEST_F(DualPathLegacyFlowTest, OutboundActiveSourceReachesTheLegacyHalWithOperandsInWireOrder)
{
    std::vector<unsigned char> captured;

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .WillOnce(DoAll(
            Invoke([&captured](int, const unsigned char* buf, int len, int*) {
                captured.assign(buf, buf + len);
            }),
            SetArgPointee<3>(HDMI_CEC_IO_SUCCESS),
            Return(HDMI_CEC_IO_SUCCESS)));

    // RAII, for the same reason as the case above: the length check below is fatal.
    ScopedConnection scoped(LogicalAddress::PLAYBACK_DEVICE_1, "L2-Legacy-FlowB-ActiveSource");

    CECFrame frame;
    MessageEncoder().encode(ActiveSource(PhysicalAddress(1, 0, 0, 0)), frame);
    scoped.connection().sendTo(LogicalAddress(LogicalAddress::BROADCAST), frame, 1000);

    ASSERT_EQ(4u, captured.size())
        << "an <Active Source> is a header, an opcode and a two-byte physical address";
    EXPECT_EQ(0x4F, static_cast<int>(captured[0]))
        << "the destination nibble is not BROADCAST";
    EXPECT_EQ(0x82, static_cast<int>(captured[1]))
        << "the opcode byte is not <Active Source>";
    EXPECT_EQ(0x10, static_cast<int>(captured[2]))
        << "physical address 1.0.0.0 packs to 0x10 0x00 and the first operand byte does not match";
    EXPECT_EQ(0x00, static_cast<int>(captured[3]))
        << "the second operand byte of physical address 1.0.0.0 does not match";
}

/**
 * @brief A legacy HAL that refuses the transmission surfaces as an exception, not as a silent
 *        success.
 *
 * Establishes the error leg of flow B on the legacy back-end.  Requires invocation D and the
 * resolved back-end to be DriverImpl; the evidence is the exception the throwing sendTo overload
 * raises when the mock reports a transmit failure.
 *
 * The mock reports HDMI_CEC_IO_SENT_FAILED through the result
 * out-parameter and HDMI_CEC_IO_GENERAL_ERROR as its return, and with the throwing sendTo overload
 * the caller must see an exception rather than a return - which is what a caller relies on to know
 * the message did not go out.
 */
TEST_F(DualPathLegacyFlowTest, OutboundTransmitFailureFromTheLegacyHalSurfacesAsAnException)
{
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .WillRepeatedly(DoAll(
            SetArgPointee<3>(HDMI_CEC_IO_SENT_FAILED),
            Return(HDMI_CEC_IO_GENERAL_ERROR)));

    ScopedConnection scoped(LogicalAddress::PLAYBACK_DEVICE_1, "L2-Legacy-FlowB-Failure");

    CECFrame frame;
    MessageEncoder().encode(ImageViewOn(), frame);

    EXPECT_THROW(
        scoped.connection().sendTo(LogicalAddress(LogicalAddress::TV), frame, 1000, Throw_e()),
        Exception)
        << "a HAL transmit failure must not be reported to the caller as a successful send";
}


// ---------------------------------------------------------------------------------------------
// Flow B on the AIDL back-end - outbound, across the binder driver to the hosted fake service.
// Invocation E.
//
// Connection::sendTo -> Bus -> DriverAidlImpl::write -> IHdmiCecController::sendMessage crosses the
// driver into the host process and comes back with a real SendMessageStatus.  Completing that round
// trip is part of the evidence - a real Bp* proxy, a real transaction, a real reply, none of which
// an in-process fake can produce - but it is not the whole of it, and the difference is what these
// two cases turn on.
//
// "It did not throw" is not enough on its own.  sendTo returning normally is satisfied by a send
// that reached the service with corrupt bytes, by a fake that dropped it, and by a marshalling step
// that wrote an empty vector - every one of those completes a transaction and returns a status.  The
// bytes the service actually received are recorded by the fake in the host process, and the fake is
// deliberately not linked into this runner, so the only way to read them is to ask the host.
//
// So each case reads the fake's own counter and capture over the pipe channel: `sent-count`
// before and after, asserted to have advanced by exactly one - not merely to have moved, because a
// retry loop that sent the frame twice is a defect and would satisfy "greater than before" - and
// `last-sent`, asserted to equal the exact hexadecimal rendering of the bytes the case encoded.
//
// The observation travels over the pipe and not over binder, because binder is the thing under
// test.  Asking the service over binder how a binder transmit went would be attesting to the
// transport with the transport; a fault could then corrupt the evidence and the corruption would be
// invisible.  The pipes behave identically whether the driver is healthy, degraded or absent.
// ---------------------------------------------------------------------------------------------

/**
 * @brief <Image View On> encoded and sent arrives at the out-of-process fake as exactly the bytes
 *        CEC defines, once, over real binder IPC.
 *
 * Establishes the middleware leg of flow B on the AIDL back-end over a real proxy and a real driver
 * transaction.  Requires invocation E, the resolved back-end to be DriverAidlImpl, and the fake
 * service host to be serving with its control channel open; the evidence is the wire image this case
 * reconstructs from the encoder's own frame and the connection's own source address, the fake's
 * sendMessage() invocation count read over the pipe channel before and after, and the fake's
 * captured frame read the same way.
 *
 * Two bytes on the wire, { 0x40, 0x04 }: initiator 4 (this Connection's source, Playback Device 1)
 * in the high nibble, destination 0 (TV) in the low nibble, opcode 0x04 (<Image View On>).  A
 * directed message, which matters for the status translation below.
 *
 * What this establishes, in three parts, each of which the other two do not cover:
 *
 *   (1) The round trip completed.  The throwing sendTo overload is used deliberately: with the
 *       non-throwing one a failed transmit is indistinguishable from a successful one, because it
 *       returns either way.  So EXPECT_NO_THROW here is a real assertion about the full stack rather
 *       than a tautology - a CECNoAckException would mean the status translation read the reply as a
 *       rejection, and an IOException would mean the binder Status came back not-ok or the frame
 *       failed the length guard.  It rests on the hosted fake's documented default reply of
 *       ACK_STATE_0, which this runner cannot alter because the fake is not linked into it, and
 *       which is correct for this frame: ACK_STATE_0 on a directed message means acknowledged.
 *
 *   (2) It arrived, exactly once.  The fake's own sendMessage() invocation count, read over the pipe
 *       channel before and after, must advance by exactly one.  Exactly rather than at least,
 *       because a retry that transmitted the frame twice is a defect on a bus where a duplicate
 *       <Image View On> is a second command to a real device.
 *
 *   (3) It arrived intact.  The fake's captured frame, read the same way, must equal the wire image
 *       CEC defines for this message - the header this Connection contributes followed by the bytes
 *       this case encoded.  It is the assertion a corrupt or truncated marshalling fails, and the
 *       comparison is against a value rebuilt with the same two calls sendTo makes rather than a
 *       literal, so it also catches the middleware writing the right length with the wrong content.
 *       Pinned alongside it, and separately, is the payload the encoder produced - the opcode
 *       without the header, because Connection::sendTo prepends the header to its own copy and
 *       leaves the caller's frame untouched, so the encoded frame alone is one byte short of the
 *       wire image - so the case cannot agree with a corrupt send by having encoded the message
 *       wrongly itself.
 *
 * What it deliberately does not establish: anything about the inbound direction.  Nothing here
 * causes the service to call back, and the two inbound cases below own that.
 */
TEST_F(DualPathAidlFlowTest, OutboundImageViewOnCrossesRealBinderIpcToTheFakeService)
{
    std::string detail;

    long sentBefore = -1;
    ASSERT_TRUE(askHostForSentCount(sentBefore, detail)) << detail;

    ScopedConnection scoped(LogicalAddress::PLAYBACK_DEVICE_1, "L2-Aidl-FlowB-ImageViewOn");

    CECFrame frame;
    MessageEncoder().encode(ImageViewOn(), frame);

    EXPECT_NO_THROW(
        scoped.connection().sendTo(LogicalAddress(LogicalAddress::TV), frame, 1000, Throw_e()))
        << "a directed <Image View On> did not complete its round trip to the out-of-process fake "
           "service; the frame did not marshal, the transaction did not cross the binder driver, "
           "or the SendMessageStatus reply was translated as a failure";

    // Two separate things are pinned here, and they must not be conflated.  MessageEncoder writes
    // the PAYLOAD - the opcode and its operands - into the caller's frame.  The header is prepended
    // by Connection::sendTo, which builds its own local CECFrame, serializes the header into it and
    // appends the caller's frame (ccec/src/Connection.cpp:141-157); the caller's frame is taken by
    // const reference and is never modified.  So the frame this case still holds after the send is
    // the payload alone, and the wire image the service received is one byte longer.  Asserting the
    // wire length against the caller's frame would be asserting that sendTo mutates its const
    // argument, which it must not and does not.
    //
    // (a) The payload the encoder produced, so this case cannot agree with a corrupt send by having
    //     encoded the message wrongly itself - and, because it is read AFTER the send, it is also
    //     the assertion that sendTo left the caller's frame alone.
    const uint8_t* payloadBytes = nullptr;
    size_t payloadLength = 0;
    frame.getBuffer(&payloadBytes, &payloadLength);
    ASSERT_EQ(1u, payloadLength)
        << "an <Image View On> payload is a bare opcode - one byte, with no operands - so the "
           "encoder produced the wrong frame and the wire comparison below would be meaningless";
    EXPECT_EQ("04",
              toLowercaseHex(reinterpret_cast<const unsigned char*>(payloadBytes), payloadLength))
        << "the encoded payload is not the 0x04 <Image View On> opcode this case is written around";

    // (b) The wire image the service must have received.  It is RECONSTRUCTED rather than read back
    //     off `frame`, with the same two calls sendTo makes, off the same encoded frame and the
    //     connection's own source address - so the expectation stays derived rather than hardcoded,
    //     and this case can neither agree with a corrupt send by having encoded the frame wrongly
    //     itself nor agree with a wrong header by restating one.  The literal CEC defines is then
    //     cross-checked against the reconstruction, which is how the legacy arm of this flow asserts.
    CECFrame wireImage;
    Header(scoped.connection().getSource(), LogicalAddress(LogicalAddress::TV)).serialize(wireImage);
    wireImage.append(frame);

    const uint8_t* wireBytes = nullptr;
    size_t wireLength = 0;
    wireImage.getBuffer(&wireBytes, &wireLength);
    ASSERT_EQ(2u, wireLength)
        << "an <Image View On> is a header and an opcode - two bytes - so the encoder produced the "
           "wrong frame and the comparison below would be against the wrong expectation";
    const std::string expectedHex =
        toLowercaseHex(reinterpret_cast<const unsigned char*>(wireBytes), wireLength);
    EXPECT_EQ("4004", expectedHex)
        << "the reconstructed wire image is not the { 0x40, 0x04 } this case is written around";

    long sentAfter = -1;
    ASSERT_TRUE(askHostForSentCount(sentAfter, detail)) << detail;
    EXPECT_EQ(sentBefore + 1, sentAfter)
        << "the fake service's own sendMessage() count went from " << sentBefore << " to " << sentAfter
        << ", where exactly one further call was expected. Equal counts mean the transmit never "
           "reached the service at all even though sendTo returned - the failure that an assertion "
           "on no-throw alone cannot see - and a jump of more than one means the frame was "
           "transmitted repeatedly";

    std::string observedHex;
    ASSERT_TRUE(askHostForLastSentFrame(observedHex, detail)) << detail;
    EXPECT_EQ(expectedHex, observedHex)
        << "the fake service received \"" << observedHex << "\" where the encoder produced \""
        << expectedHex << "\". The transaction crossed the driver and the bytes did not survive it: "
           "the header nibbles or the opcode were altered, the frame was truncated, or an empty "
           "vector was marshalled";
}

/**
 * @brief <Active Source> with operands completes a real binder round trip, so a multi-byte frame
 *        survives the parcel.
 *
 * Establishes that length and operands survive the parcel on the AIDL back-end, and that the
 * broadcast arm of the status translation does not raise for this opcode.  Requires invocation E, the
 * resolved back-end to be DriverAidlImpl, and the fake service host to be serving with its control
 * channel open; the evidence is the fake's captured frame and its invocation count, both read over
 * the pipe channel.
 *
 * Four bytes on the wire, { 0x4F, 0x82, 0x10, 0x00 }: header with the broadcast destination
 * nibble, opcode 0x82 (<Active Source>), then physical address 1.0.0.0 packed as 0x10 0x00.
 *
 * Its value over the previous case is the length and the operands.  DriverAidlImpl::write copies
 * the frame into a std::vector<uint8_t> and hands it to sendMessage, so a four-byte frame with
 * operands has to be marshalled, written into the parcel, read back out on the far side and
 * accepted; a two-byte frame would not catch a length error, an off-by-one in the copy, or an
 * operand lost in the marshalling.  It also exercises the length guard from the compliant side:
 * four bytes is well inside the 16-byte AIDL contract, so the guard must let it through.
 *
 * A broadcast message, and that is why the status translation still yields success.  ACK_STATE_0
 * means rejected for a broadcast rather than acknowledged - the sense inverts - but the middleware
 * raises CECNoAckException on that arm only for the CEC CTS 9-3-3 case, a rejected
 * <Report Physical Address>.  This frame's opcode is 0x82, so the arm does not apply and the send
 * returns normally against the host's default reply.
 *
 * The operands are asserted at the service, not merely at the encoder.  The fake's captured frame is
 * read back over the pipe channel and compared byte for byte against the wire image CEC defines for
 * this message, rebuilt from what the encoder produced, so an operand dropped, reordered or zeroed
 * inside the parcel fails here; the encoder's own payload is pinned separately, because the header is
 * prepended by Connection::sendTo to a local copy rather than written into the caller's frame.
 * Asserting the count as well
 * - exactly one further sendMessage() - is what separates "the bytes were wrong" from "the transmit
 * never arrived", which are different defects and must not share one failure message.
 *
 * What it deliberately does not establish: the 16-byte length guard's refusing side.  Four bytes
 * exercises the compliant side only; the boundary at 16, 17 and 20 bytes belongs to the L1 contract
 * suite, where the fake's canned status can be varied per case.
 */
TEST_F(DualPathAidlFlowTest, OutboundActiveSourceWithOperandsCrossesRealBinderIpc)
{
    std::string detail;

    long sentBefore = -1;
    ASSERT_TRUE(askHostForSentCount(sentBefore, detail)) << detail;

    ScopedConnection scoped(LogicalAddress::PLAYBACK_DEVICE_1, "L2-Aidl-FlowB-ActiveSource");

    CECFrame frame;
    MessageEncoder().encode(ActiveSource(PhysicalAddress(1, 0, 0, 0)), frame);

    EXPECT_NO_THROW(
        scoped.connection().sendTo(LogicalAddress(LogicalAddress::BROADCAST), frame, 1000, Throw_e()))
        << "a four-byte broadcast <Active Source> did not complete its round trip to the "
           "out-of-process fake service; the operands did not survive the parcel, the frame was "
           "refused by the length guard, or the broadcast arm of the status translation raised "
           "where it should not";

    // As in the previous case, the payload and the wire image are pinned separately: the encoder
    // writes the opcode and its operands into the caller's frame, and Connection::sendTo prepends
    // the header to a local copy (ccec/src/Connection.cpp:141-157) without touching the caller's.
    //
    // (a) The payload, which for this message carries the operands the case exists to follow
    //     through the parcel.
    const uint8_t* payloadBytes = nullptr;
    size_t payloadLength = 0;
    frame.getBuffer(&payloadBytes, &payloadLength);
    ASSERT_EQ(3u, payloadLength)
        << "an <Active Source> payload is an opcode and a two-byte physical address - three bytes - "
           "so the encoder produced the wrong frame and the wire comparison below would be "
           "meaningless";
    EXPECT_EQ("821000",
              toLowercaseHex(reinterpret_cast<const unsigned char*>(payloadBytes), payloadLength))
        << "the encoded payload is not the { 0x82, 0x10, 0x00 } this case is written around - "
           "physical address 1.0.0.0 packs to 0x10 0x00";

    // (b) The wire image: the header with the broadcast destination nibble, then that payload.
    //     Reconstructed the same way, and for the same reason, as the previous case: sendTo leaves
    //     the caller's frame alone, so the header byte has to be prepended here to obtain what
    //     actually went on the wire.  See the comment there for the full argument.
    CECFrame wireImage;
    Header(scoped.connection().getSource(), LogicalAddress(LogicalAddress::BROADCAST))
        .serialize(wireImage);
    wireImage.append(frame);

    const uint8_t* wireBytes = nullptr;
    size_t wireLength = 0;
    wireImage.getBuffer(&wireBytes, &wireLength);
    ASSERT_EQ(4u, wireLength)
        << "an <Active Source> is a header, an opcode and a two-byte physical address, so the "
           "encoder produced the wrong frame and the comparison below would be against the wrong "
           "expectation";
    const std::string expectedHex =
        toLowercaseHex(reinterpret_cast<const unsigned char*>(wireBytes), wireLength);
    EXPECT_EQ("4f821000", expectedHex)
        << "the reconstructed wire image is not the { 0x4F, 0x82, 0x10, 0x00 } this case is written "
           "around - physical address 1.0.0.0 packs to 0x10 0x00";

    long sentAfter = -1;
    ASSERT_TRUE(askHostForSentCount(sentAfter, detail)) << detail;
    EXPECT_EQ(sentBefore + 1, sentAfter)
        << "the fake service's own sendMessage() count went from " << sentBefore << " to " << sentAfter
        << ", where exactly one further call was expected. Equal counts mean the four-byte transmit "
           "never reached the service even though sendTo returned; more than one means it was "
           "transmitted repeatedly";

    std::string observedHex;
    ASSERT_TRUE(askHostForLastSentFrame(observedHex, detail)) << detail;
    EXPECT_EQ(expectedHex, observedHex)
        << "the fake service received \"" << observedHex << "\" where the encoder produced \""
        << expectedHex << "\". A four-byte frame has to be copied into a std::vector<uint8_t>, "
           "written into the parcel and read back on the far side, and one of those steps lost or "
           "altered a byte - most likely an operand, which a two-byte frame would not have caught";
}

// ---------------------------------------------------------------------------------------------
// Flow A on the AIDL back-end - inbound, from the hosted fake service to the typed process()
// overload.  Invocation E.
//
// Both cases below execute, and the mechanism is the host's control channel.  The only object that
// can invoke the middleware's IHdmiCecEventListener is the fake service, and by design it lives in
// the host process - that separation is the whole substance of this tier - so this runner cannot
// call it directly and the fake is deliberately not linked here.  What it can do is ask: `deliver
// <hex>` reaches the fake's own fireOnMessageReceived(), which invokes the listener the middleware
// handed to open() and no other, so the delivery route under test is the production one and not a
// second one invented for the test.
//
// That is what makes these the only cases in the repository that can prove inbound AIDL delivery.
// onMessageReceived is `oneway`, so it is dispatched inside this process on a binder threadpool
// thread - the pool DriverAidlImpl::open() starts, and the reason it has to.  No in-process fake can
// produce that: a locally registered service resolves to the local BBinder and its callback would
// run inline on the calling thread.  So an inbound frame arriving here at all is evidence about the
// transport and the threadpool together, and the thread-identity assertion below is what
// distinguishes it from an inline call.
//
// What the thread assertion does and does not say, stated exactly rather than overclaimed.  The
// listener records the thread that delivered its notification, which on both arms is the Bus reader
// thread rather than the thread that produced the frame - the queue is the handoff.  So
// NotifyingThread() != this_thread is a necessary condition: it fails if delivery were somehow
// inline on the test thread, which is what makes it worth asserting.  That the callback itself ran
// on a binder thread is established structurally, and the argument is short: the frame's only
// possible origin is the fake, the fake is in another process, its only route into this process is
// the binder driver, and the only threads that execute an incoming oneway transaction here are the
// client threadpool's.  A frame that arrives therefore arrived through them.
// ---------------------------------------------------------------------------------------------

/**
 * @brief A frame delivered by the fake service arrives on a thread that is not the test's, and
 *        reaches the typed processor decoded and intact.
 *
 * Discharges the AIDL arm's inbound requirement.  Requires invocation E, the resolved back-end to be
 * DriverAidlImpl, and the fake service host to be serving with its control channel open; the
 * evidence is the fake's listener-presence and delivery replies, its open and close counters read
 * before and after, the recording processor's typed counters and header nibbles, and the thread id
 * the listener recorded.
 *
 * It is the one thing invocation E can prove that no in-process fake can: that a frame the service
 * originates crosses the binder driver into this process, is dispatched by the client threadpool onto
 * the middleware's IHdmiCecEventListener, and travels from there the same receive queue, the same Bus
 * reader thread and the same address filter as a legacy frame does, to arrive at the same typed
 * process() overload.
 *
 * The frame is { 0x40, 0x04 }, delivered as the hex "4004": initiator 4 (Playback Device 1) in the
 * high nibble, destination 0 (TV) in the low nibble, opcode 0x04 (<Image View On>).  The Connection
 * is opened as TV so the destination matches it and the filter must let the frame through.  It is
 * deliberately the same frame the legacy inbound case injects, so the two arms are compared on
 * identical input and any difference in the result is a difference in the back-end.
 *
 * What is asserted, and each part answers a different way this case could pass while broken:
 *
 *   (1) The trigger was real.  `listener` first, because a `deliver` with no listener held is
 *       answered "ERR no-listener" and dispatches nothing - so without this check a middleware that
 *       never registered its listener would leave the case failing on the wait with a misleading
 *       message.  Then `deliver`, whose reply reports the byte count the fake handed to the
 *       callback; two bytes are asserted, so a truncated trigger is caught before the wait.
 *
 *   (1b) The session behind that listener is real and singular.  `open-count` and `close-count` are
 *       the fake service's own counters, so they are the only evidence available anywhere that the
 *       session lifecycle - not just a transmit - crossed the driver: L1's in-process fake resolves
 *       locally and is called inline, so a count there proves nothing about a transaction.  Exactly
 *       one more open than close is asserted, which is one live session; and both counters are read
 *       again at the end of the case and must be unchanged, because receiving a frame is not a
 *       session event.  Why the assertion is a difference rather than the literal one and zero is
 *       explained at the assertion itself: one case in this fixture cycles the library, and this
 *       file must hold under --gtest_shuffle.
 *
 *   (2) The frame arrived, within a bound.  A predicate wait, not a sleep: its expiry is the real
 *       verdict "the frame never arrived", which is what makes the failure diagnosable.
 *
 *   (3) It arrived as the right message.  imageViewOnCount == 1 with the other three overload
 *       counters at zero, so a frame that decoded to some other message type fails rather than
 *       passing on the fact that something arrived; both header nibbles are asserted, so an
 *       initiator or destination rewritten in the marshalling fails; and DecodeFailures() == 0, so a
 *       delivery whose decode threw and was contained is reported rather than hidden.
 *
 *   (4) It was not delivered inline.  NotifyingThread() != this thread's id.  The section comment
 *       above states exactly what this does and does not establish: it is the necessary condition,
 *       and the binder-thread half is structural, because the fake is in another process and no
 *       other route into this one exists.
 *
 *   (5) The session survived the delivery unchanged - the second half of (1b), asserted after the
 *       frame has arrived rather than before, so that a back-end which reopened or closed its
 *       session around the callback fails here instead of passing everything above.
 *
 * What it deliberately does not assert: the address filter's negative side - the legacy arm's
 * filtered case covers it, and the filter is shared code above the seam, so asserting it twice would
 * add nothing - and anything about close-state rejection, which the next case owns.
 */
TEST_F(DualPathAidlFlowTest, InboundFrameFromTheFakeServiceArrivesOnABinderThreadAndReachesTheTypedProcessor)
{
    std::string detail;

    /*
     * (1) The listener the middleware handed to open() must be the one the fake is holding, or the
     *     trigger below would do nothing and every assertion after it would be about the wrong
     *     thing.
     */
    bool listenerHeld = false;
    ASSERT_TRUE(askHostForListenerPresence(listenerHeld, detail)) << detail;
    ASSERT_TRUE(listenerHeld)
        << "the fake service is holding no event listener, so it has nothing to deliver to. The "
           "middleware passes its listener to IHdmiCec::open() during LibCCEC::init, so this means "
           "the AIDL open() never reached the service even though the AIDL back-end was selected";

    /*
     * (1b) The session that listener came from, counted at the service.  "A listener is held" says
     *      an open reached the far side; it does not say how many did, nor that the session is still
     *      open.  These two counters do, and they are the only evidence in this repository that the
     *      session lifecycle - as against a transmit - crossed the driver at all: the middleware
     *      opens exactly once, in LibCCEC::init, and never again.
     *
     *      The assertion is the difference and not the literal 1 and 0.  In a process where no
     *      case has cycled the CEC library the counters are exactly one and zero.  One case in this
     *      fixture cycles it deliberately - AFrameDeliveredWhileTheDriverIsNotOpened... has no
     *      alternative, since only LibCCEC::term() can leave OPENED - and each cycle adds one to
     *      each counter.  GoogleTest runs cases in registration order by default but this file is
     *      required to pass under --gtest_shuffle, so a literal expectation here would be an
     *      assertion about case order dressed up as one about the middleware.  The difference is the
     *      invariant that holds under every order: one more open than close means exactly one live
     *      session, which is the property being claimed.
     */
    long openCount = -1;
    long closeCount = -1;
    ASSERT_TRUE(askHostForSessionCount("open-count", openCount, detail)) << detail;
    ASSERT_TRUE(askHostForSessionCount("close-count", closeCount, detail)) << detail;

    EXPECT_GE(openCount, 1)
        << "the fake service has served " << openCount << " IHdmiCec::open() calls, so no session "
           "was ever opened ACROSS THE DRIVER even though the AIDL back-end was selected and a "
           "listener is held. The two cannot both be true of the same service, so the middleware is "
           "talking to a different one";
    EXPECT_EQ(1, openCount - closeCount)
        << "the fake service has served " << openCount << " open() and " << closeCount
        << " close() calls, leaving " << (openCount - closeCount)
        << " live sessions where exactly one was expected. A difference of zero means something "
           "closed the middleware's session behind this case's back - and every assertion below "
           "would then be about a session that is not open; more than one means an open was issued "
           "without a matching close, which on the real HAL fails with EX_ILLEGAL_STATE because "
           "IHdmiCec::open() admits one controlling client at a time";

    RecordingProcessor processor;
    DecodingFrameListener listener(processor);

    // Declared after the listener on purpose: the guard's destructor detaches the listener from the
    // Bus, so the listener has to outlive the guard, and the reverse declaration order would destroy
    // it first.
    ScopedConnection scoped(LogicalAddress::TV, "L2-Aidl-FlowA-ImageViewOn");
    scoped.addFrameListener(&listener);

    long deliveredBytes = -1;
    ASSERT_TRUE(askHostToDeliverFrame("4004", deliveredBytes, detail)) << detail;
    ASSERT_EQ(2, deliveredBytes)
        << "the fake service reported delivering " << deliveredBytes << " bytes where the two bytes "
           "of { 0x40, 0x04 } were sent, so the trigger itself is wrong and nothing below would be "
           "measuring the middleware";

    /*
     * (2) The bound covers an inter-process oneway transaction, the queue handoff and the Bus
     *     reader's wake-up, inside an emulated guest on a loaded machine.  It is the same 3000 ms the
     *     legacy inbound cases use, so a difference in outcome between the two arms is a difference
     *     in the back-end rather than in the patience of the test.
     */
    ASSERT_TRUE(listener.WaitForNotification(1, 3000))
        << "the fake service reported invoking onMessageReceived and no frame reached the listener "
           "within 3000 ms. The transaction left the host, so the break is on this side of the "
           "boundary: the client binder threadpool is not running (DriverAidlImpl::open must call "
           "startThreadPool), the listener's state guard rejected the frame, the frame was offered to "
           "a queue other than the one the Bus reader drains, or the Connection's address filter "
           "dropped it";

    EXPECT_EQ(0, listener.DecodeFailures())
        << "the frame arrived and its decode raised: " << listener.LastDecodeError();

    EXPECT_EQ(1, processor.imageViewOnCount)
        << "the frame arrived over binder but did not decode to <Image View On>";
    EXPECT_EQ(0, processor.textViewOnCount)
        << "the frame decoded to <Text View On>, so the opcode was altered crossing the driver";
    EXPECT_EQ(0, processor.activeSourceCount)
        << "the frame decoded to <Active Source>, so the opcode was altered crossing the driver";
    EXPECT_EQ(0, processor.standbyCount)
        << "the frame decoded to <Standby>, so the opcode was altered crossing the driver";
    EXPECT_EQ(static_cast<int>(LogicalAddress::PLAYBACK_DEVICE_1), processor.lastInitiator)
        << "the initiator nibble was lost or rewritten between the fake and the processor";
    EXPECT_EQ(static_cast<int>(LogicalAddress::TV), processor.lastDestination)
        << "the destination nibble was lost or rewritten between the fake and the processor";

    /*
     * (4) The assertion that fails if the delivery were inline on this thread.  It is checked only
     *     after the wait above has established that a notification exists: NotifyingThread() is a
     *     default-constructed id until then, and that equals no running thread, so this comparison
     *     would pass vacuously in a case that received nothing.
     */
    EXPECT_NE(std::this_thread::get_id(), listener.NotifyingThread())
        << "the frame was delivered to the listener ON THE TEST'S OWN THREAD. Nothing in this process "
           "asked for it: the frame originated in the fake service's process and can only have "
           "entered this one through the binder driver, dispatched by the client threadpool and handed "
           "on by the Bus reader thread. This thread's id appearing here means the delivery was "
           "inline, which would mean the service had been resolved LOCALLY rather than as a remote "
           "proxy - the in-process case this tier exists to be distinguishable from";

    /*
     * (5) And the session is still the one it was, which is an exact expectation rather than an
     *     invariant because it spans only this case: receiving a frame is not a session event, so
     *     neither counter may have moved while the delivery above happened. A back-end that
     *     re-opened its session per inbound frame, or that closed and reopened around the callback,
     *     would satisfy every assertion above and fail here.
     */
    long openCountAfter = -1;
    long closeCountAfter = -1;
    ASSERT_TRUE(askHostForSessionCount("open-count", openCountAfter, detail)) << detail;
    ASSERT_TRUE(askHostForSessionCount("close-count", closeCountAfter, detail)) << detail;

    EXPECT_EQ(openCount, openCountAfter)
        << "the fake service's open() count went from " << openCount << " to " << openCountAfter
        << " while a frame was being delivered. An inbound delivery must not open a session: the "
           "middleware opens once, during LibCCEC::init, and holds that session for the life of the "
           "process";
    EXPECT_EQ(closeCount, closeCountAfter)
        << "the fake service's close() count went from " << closeCount << " to " << closeCountAfter
        << " while a frame was being delivered, so the session was closed underneath this case - the "
           "receive path must not touch the session lifecycle at all";
}

/**
 * @brief A frame delivered while the driver is not OPENED is rejected and released, not queued, and
 *        the process survives it.
 *
 * Establishes that the AIDL listener's state guard refuses a frame outside OPENED exactly as the
 * legacy delete-on-throw path does.  Requires invocation E, the resolved back-end to be
 * DriverAidlImpl, and the fake service host to be serving with its control channel open; the evidence
 * is the fake's delivery reply, the typed counters after the library is brought back up, a second
 * frame with a different opcode that must arrive, and the fake's open and close counters read at
 * each transition.
 *
 * This is the AIDL counterpart of the legacy delete-on-throw path.  DriverImpl::DriverReceiveCallback
 * does not touch the queue directly: it offers through DriverImpl::getIncomingQueue(), which raises
 * InvalidStateException when the status is not OPENED, and the callback then deletes the frame.  That
 * is what rejects a callback arriving during or after a close, and the AIDL listener is required to
 * reject identically - offering straight to the queue would accept frames the legacy path refuses,
 * and a frame accepted while closed is a frame delivered to the application after the session it
 * belonged to ended.
 *
 * How the rejection is observed is the whole difficulty of this case.  A `oneway` callback
 * has no caller to receive a fault, so the fake cannot tell whether the middleware accepted or
 * rejected what it delivered - its reply says only that the callback was invoked.  The observation
 * therefore has to be made on this side, and it is made in three steps whose combination is what
 * rules out a vacuous pass:
 *
 *   (1) The delivery genuinely happened while the driver was out of OPENED.  The fake retains the
 *       listener across close - deliberately, per its own contract, precisely so that this is
 *       testable - so `deliver` reports the callback invoked with two bytes rather than
 *       "ERR no-listener".  A negative that rested on the trigger having done nothing would prove
 *       nothing at all.
 *
 *   (2) The frame never surfaces, even after the stack comes back up.  This is the assertion that
 *       distinguishes "released" from "queued".  A frame wrongly offered while closed would sit in
 *       the receive queue, and DriverAidlImpl::read() consumes a NULL sentinel and loops rather than
 *       draining the queue behind it, so the Bus reader started by the re-initialisation would
 *       deliver it.  The listener stays attached across the whole cycle for exactly this reason.
 *
 *   (3) The route is alive, proved afterwards.  A second frame - <Standby>, a different opcode - is
 *       delivered once the stack is back up and must arrive.  Without this, step (2) would be
 *       satisfied by a route that had simply stopped working, and the case would pass while proving
 *       the opposite of its name.  The differing opcode is what makes the identification exact: one
 *       notification whose message is <Standby> means the closed-window <Image View On> was released,
 *       and an imageViewOnCount above zero at the end means it was queued and delivered late.
 *
 *   (4) And the cycle itself reached the service, once each way.  The fake service's `open-count`
 *       and `close-count` are read before the take-down, after it and after the restore, and each
 *       transition must move exactly one of them by exactly one.  This is what turns "term() did not
 *       raise" into "the close crossed the binder driver", and it is the assertion that would fail if
 *       the take-down closed nothing on the far side - in which case step (2)'s negative would hold
 *       for the wrong reason, the far side never having left its open session at all.  It is also the
 *       only place where a session transition - as against the standing one-live-session invariant
 *       the inbound case checks - is observed from outside the process that owns it.
 *
 * @warning Order of declaration is load-bearing.  The listener is declared first, then the connection
 *          guard, then the library cycle - so destruction runs in reverse: the library is restored,
 *          then the connection is closed against a stack that is up, then the listener is destroyed
 *          with nothing pointing at it.
 *
 * @warning Blocked item B2 applies.  Cycling the library reaches Driver::close(), whose AIDL mapping
 *          to IHdmiCec.close is a high-confidence candidate pending owner confirmation.  Step (4)
 *          observes that the call was made and made once; it says nothing about whether that is the
 *          right call for HdmiCecClose, and a green result here does not confirm the mapping.
 *
 * @see DriverAidlImpl::read()
 * @see ScopedCecLibraryCycle
 */
TEST_F(DualPathAidlFlowTest, AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheStateGuard)
{
    std::string detail;

    bool listenerHeld = false;
    ASSERT_TRUE(askHostForListenerPresence(listenerHeld, detail)) << detail;
    ASSERT_TRUE(listenerHeld)
        << "the fake service is holding no event listener before the driver has even been closed, so "
           "the AIDL open() never reached the service and there is nothing for the state guard to "
           "reject";

    RecordingProcessor processor;
    DecodingFrameListener listener(processor);

    ScopedConnection scoped(LogicalAddress::TV, "L2-Aidl-FlowA-ClosedGuard");
    scoped.addFrameListener(&listener);

    ASSERT_EQ(0, listener.Notifications())
        << "a frame reached this listener before the case delivered anything, so some earlier case "
           "left a frame in flight and the counters below would not be attributable";

    /*
     * The session counters as they stand before the cycle. Read here rather than assumed to be one
     * and zero, because this case must hold under --gtest_shuffle: another case may have run first,
     * and what is being asserted is what this cycle does, which is a pair of deltas and not a pair
     * of absolute values.
     */
    long openBeforeCycle = -1;
    long closeBeforeCycle = -1;
    ASSERT_TRUE(askHostForSessionCount("open-count", openBeforeCycle, detail)) << detail;
    ASSERT_TRUE(askHostForSessionCount("close-count", closeBeforeCycle, detail)) << detail;
    ASSERT_EQ(1, openBeforeCycle - closeBeforeCycle)
        << "the fake service reports " << openBeforeCycle << " open() and " << closeBeforeCycle
        << " close() calls, so there is not exactly one live AIDL session to take down and the "
           "deltas asserted below would not be attributable to this case's own cycle";

    /*
     * Out of OPENED, with the restoration guaranteed by this guard's destructor on every exit path
     * below - including a fatal assertion's early return.
     */
    ScopedCecLibraryCycle cycle;
    ASSERT_TRUE(cycle.TakeDown(detail)) << detail;

    /*
     * The close reached the service, and it reached it once.  term() returning without raising says
     * only that the middleware believed it closed; these counters are the far side of the driver
     * saying so, and this is the only case that observes it. Exactly one,
     * because a close issued twice would fail on the real HAL, and the open count must not move at
     * all - a take-down that reopened anything would leave the state guard with nothing to reject.
     *
     * Blocked item B2 applies and is not discharged by this.  What is asserted is that
     * Driver::close() reached IHdmiCec::close at the service; whether IHdmiCec.close is the correct
     * mapping for HdmiCecClose is a high-confidence candidate pending owner confirmation either way.
     */
    long openAfterTakeDown = -1;
    long closeAfterTakeDown = -1;
    ASSERT_TRUE(askHostForSessionCount("open-count", openAfterTakeDown, detail)) << detail;
    ASSERT_TRUE(askHostForSessionCount("close-count", closeAfterTakeDown, detail)) << detail;

    EXPECT_EQ(closeBeforeCycle + 1, closeAfterTakeDown)
        << "the fake service's close() count went from " << closeBeforeCycle << " to "
        << closeAfterTakeDown << " across LibCCEC::term(), where exactly one further call was "
           "expected. An unchanged count means term() completed WITHOUT the close crossing the "
           "binder driver - the middleware left the far side holding an open session it thinks is "
           "gone - and more than one means close was issued repeatedly, which the real HAL refuses";
    EXPECT_EQ(openBeforeCycle, openAfterTakeDown)
        << "the fake service's open() count moved from " << openBeforeCycle << " to "
        << openAfterTakeDown << " across LibCCEC::term(). Taking the library down must not open a "
           "session; if it did, the driver would be back in OPENED and the state guard this case "
           "exists to exercise would have nothing to reject";

    long deliveredBytes = -1;
    ASSERT_TRUE(askHostToDeliverFrame("4004", deliveredBytes, detail)) << detail;
    ASSERT_EQ(2, deliveredBytes)
        << "the fake service did not invoke the listener with the two bytes of { 0x40, 0x04 } while "
           "the driver was closed, so the rejection this case exists to observe was never provoked";

    /*
     * Back up before the negative is checked, because a closed stack has no Bus reader and would
     * satisfy "no frame arrived" whether the frame was released or sitting in the queue. It is the
     * re-initialisation that starts a reader capable of draining a wrongly queued frame, so the
     * negative is only meaningful on this side of it.
     */
    ASSERT_TRUE(cycle.Restore(detail)) << detail;

    /*
     * And the re-open reached the service, once.  The mirror of the pair above, and it is what makes
     * the control at the end of this case interpretable: the second `deliver` can only prove the
     * route is alive if a second session was genuinely established across the driver, and a
     * re-initialisation that opened nothing would leave that delivery reaching a stale listener.
     * The close count must not move here for the same reason the open count must not move above.
     */
    long openAfterRestore = -1;
    long closeAfterRestore = -1;
    ASSERT_TRUE(askHostForSessionCount("open-count", openAfterRestore, detail)) << detail;
    ASSERT_TRUE(askHostForSessionCount("close-count", closeAfterRestore, detail)) << detail;

    EXPECT_EQ(openBeforeCycle + 1, openAfterRestore)
        << "the fake service's open() count went from " << openAfterTakeDown << " to "
        << openAfterRestore << " across LibCCEC::init(), where exactly one further call was expected "
           "against the " << openBeforeCycle << " served before this case's cycle began. An "
           "unchanged count means the re-initialisation opened no session at the service, so the "
           "middleware is back in OPENED against a far side that has none";
    EXPECT_EQ(closeAfterTakeDown, closeAfterRestore)
        << "the fake service's close() count went from " << closeAfterTakeDown << " to "
        << closeAfterRestore << " across LibCCEC::init(). Bringing the library up must not close "
           "anything, so this is one cycle producing two closes";

    EXPECT_FALSE(listener.WaitForNotification(1, 1200))
        << "the frame delivered while the driver was NOT OPENED reached the listener. The AIDL event "
           "listener must offer through the same state-guarded accessor the legacy receive callback "
           "uses and release the frame when the guard refuses it; a frame accepted while closed is "
           "delivered to the application after the session it belonged to ended";
    EXPECT_EQ(0, listener.Notifications())
        << "a frame delivered while the driver was closed was queued and then dispatched once the "
           "stack came back up, which is the same defect arriving one step later";
    EXPECT_EQ(0, processor.imageViewOnCount)
        << "the closed-window <Image View On> was decoded, so it was accepted rather than released";

    /*
     * (3) The control that stops the negative above from passing for the wrong reason. A different
     *     opcode, so that what arrives can be named rather than merely counted.
     */
    ASSERT_TRUE(askHostForListenerPresence(listenerHeld, detail)) << detail;
    ASSERT_TRUE(listenerHeld)
        << "the fake service holds no listener after the library was re-initialised, so the AIDL "
           "open() on the second cycle did not reach the service and the control below could not "
           "distinguish a rejected frame from a dead route";

    ASSERT_TRUE(askHostToDeliverFrame("4036", deliveredBytes, detail)) << detail;
    ASSERT_EQ(2, deliveredBytes)
        << "the fake service did not invoke the listener with the two bytes of { 0x40, 0x36 }";

    EXPECT_TRUE(listener.WaitForNotification(1, 3000))
        << "the <Standby> delivered AFTER the stack came back up did not arrive either, so the "
           "inbound route is not working at all and the non-delivery asserted above says nothing "
           "about the state guard";
    EXPECT_EQ(0, listener.DecodeFailures())
        << "the post-restore frame arrived and its decode raised: " << listener.LastDecodeError();
    EXPECT_EQ(1, processor.standbyCount)
        << "the frame delivered after the restore did not decode to <Standby>";
    EXPECT_EQ(0, processor.imageViewOnCount)
        << "the <Image View On> delivered while the driver was closed arrived alongside the "
           "post-restore <Standby>, so it had been queued rather than released";
    EXPECT_EQ(1, listener.Notifications())
        << "exactly one frame was expected at this listener - the post-restore <Standby> - and "
        << listener.Notifications() << " arrived, so the closed-window frame was delivered too";
}

/** @} */ // End of HDMI_CEC_L2_DUALPATH
