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
* @defgroup hdmicec
* @{
* @defgroup ccec
* @{
**/


/**
 * @file test_DriverAidl.cpp
 *
 * @brief L1 unit tests for the AIDL/binder HDMI CEC back-end, for the runtime selection
 *        between it and the legacy back-end, and for the compatibility rule that
 *        selection rests on.
 *
 * Three things are exercised, and they are exercised by three different routes because
 * no single route reaches all of them:
 *
 *   -# The compatibility predicate and the binder preflight, by direct call against
 *      locally constructed doubles and against paths this file owns. Neither needs a
 *      registered service, a binder driver or a resolved back-end, so both run under
 *      every invocation.
 *   -# The AIDL back-end's closed-state behaviour, on a local instance.
 *      DriverAidlImpl::DriverAidlImpl() touches no binder at all - it sets the state to
 *      CLOSED, the legacy handle field to 0 and an empty address list, exactly as
 *      DriverImpl::DriverImpl() does (DriverImpl.cpp:87-90) - so a local instance is
 *      constructible even where no service exists, and every status != OPENED guard, the
 *      DriverAidlImpl::writeAsync() prelude ordering, and the two methods that carry no
 *      guard at all are reachable without a HAL of any kind.
 *   -# The back-end actually resolved for this process, through Driver::getInstance().
 *      Which one that is depends on the invocation, so the cases that need a specific one
 *      live in fixtures of their own and assert their precondition in SetUp.
 *
 * Isolation: this suite shares one process-global driver with every other suite in the
 * binary, and the binary is order-sensitive. Cases needing an open driver establish that
 * precondition themselves and leave the driver open again - the state the global
 * environment sets up at start-up - and clear their own mock expectations. Cases needing
 * a driver that is not open construct a local DriverAidlImpl instead of disturbing the
 * shared one. No case here closes, terminates or re-initialises the shared library, and
 * no case here installs a default action on the process-global HAL mock.
 *
 * One other piece of process-global state is touched, and it is touched under custody: the
 * middleware's log level, which one case raises to DEBUG so that a callback reporting at
 * LOG_DEBUG can be observed at all. The only seam for it is production's own
 * check_cec_log_status() and the fixed path it reads, which is shared with every process on
 * the host, so ScopedCecLogLevel locks that path, validates it, replaces it atomically and
 * restores it byte for byte on every exit path - and refuses, rather than forcing, when the
 * path is not safely this run's to modify. No later case inherits a raised level.
 *
 * Seven paths are unreachable by construction and are deliberately not chased. Each is
 * named with its mechanism and with what reaching it would take, so that none of them
 * reads as an oversight:
 *
 * -# "The server reports an older version within the same major." This arm of halcompat's
 *    era-0 rule is empty for this client, not merely untested. IHdmiCec::VERSION is 1000
 *    (IHdmiCec.h:25), which under the positional encoding at halcompat.h:86-96 is era 0,
 *    major 1. The servers that satisfy era(server) == 0 and major(server) == 1 are exactly
 *    the integers 1000 through 1999, and every one of them is >= 1000, so the
 *    `serverVersion >= clientVersion` conjunct at halcompat.h:114 cannot fail while the two
 *    preceding conjuncts hold. A server that reports 999 decodes to era 0, major 0, and is
 *    rejected by the cross-major conjunct at halcompat.h:113 - so labelling such a case
 *    "older same-major" would record a rule that does not exist. The arm is reached instead
 *    with a different client, by calling detail::isCompatible(3020, 3000) directly; a static
 *    assertion below pins it and explains why halcompat.h:128's own (3000, 2000) assertion
 *    is mislabelled.
 * -# The era >= 1 branch of the ternary at halcompat.h:110-111. It requires
 *    era(clientVersion) >= 1, and this client's era is 0 for as long as IHdmiCec::VERSION is
 *    1000, so no server value whatsoever can activate it here. Reaching it needs a re-frozen
 *    interface in era 1 or later, at which point the static assertion on VERSION below fires
 *    and this whole analysis is recomputed.
 * -# The preflight's protocol-version-mismatch arm and its context-manager arm, which are
 *    reachable on any host through the probe seam and are therefore not on this list as
 *    absences but as a constraint on how they are reached. Neither can be synthesised from a
 *    path and a timeout alone: the first needs a node that answers BINDER_VERSION with a
 *    value other than the one this build was compiled against, the second a driver whose
 *    context manager never answers, and a regular file reaches the ioctl arm before either -
 *    so with the default probe they are platform-integration conditions. The predicate takes
 *    a third, defaulted argument for exactly this reason: a DriverAidlImpl::BinderPreflightProbe
 *    of six function pointers whose default is the real syscalls. A synthetic probe therefore
 *    reaches every arm on any host, including a true verdict, and DriverAidlPreflightTest
 *    asserts all eight decision points below - the node's identity, file type and ownership
 *    among them - together with the custody handover and the pre-lookup re-verification that
 *    closes the window between the check and libbinder's own open of the same name. The
 *    production call site passes no arguments and runs the real syscalls.
 * -# Withdrawing a registered service. The pinned C++ IServiceManager exposes no
 *    service-removal API, and the service manager retains a reference to whatever was
 *    published, so a test written around unregistering could not be implemented at all -
 *    dropping the local reference would at best destroy an object the service manager still
 *    advertises. No case here attempts it. The selection-stability case therefore proves its
 *    point by adding a service mid-process rather than by taking one away, which is the
 *    direction that is actually expressible.
 * -# A physical-address read on the AIDL back-end. BLOCKED ITEM B1 - see the contract block
 *    below, which states what is covered, what is not, and the change that would close it.
 * -# The body of DriverAidlImpl::printFrameDetails()' catch(Exception &e). The only statement
 *    in the guarded try that can throw is the Header construction, which reads frame.at(0)
 *    and raises std::out_of_range - and Exception and std::out_of_range are siblings under
 *    std::exception, not related by inheritance, so catch(Exception &e) cannot catch it and
 *    the exception escapes instead. Reaching the handler needs a production change: a broader
 *    handler, or a length guard ahead of the Header. The method is byte-for-byte the legacy
 *    one, whose identical boundary the neighbouring suite records at
 *    test_DriverImpl_Async.cpp:38-46, so this is a shared pre-existing condition and not
 *    something the AIDL back-end introduced. The escape itself is asserted below, by the
 *    writeAsync prelude-ordering cases, so the boundary is pinned and any future change to
 *    the handler is a visible decision.
 * -# The non-CLOSED arm of DriverAidlImpl::~DriverAidlImpl(), which closes an open session on
 *    the way out. No instance a test can destroy is ever OPENED: a local instance holds no
 *    service proxy, so its open() raises IOException before the state moves, and the only
 *    instance that does get opened is the function-local static inside Driver::getInstance(),
 *    whose destructor runs at static destruction - after the last test has finished, where
 *    nothing can assert on it. Reaching it observably would need a production seam for
 *    injecting a proxy, which is a new abstraction and not permitted. Note in passing that
 *    the destructor takes the instance lock and then calls close(), which takes it again;
 *    that is safe rather than a latent deadlock, because CCEC_OSAL::Mutex is created
 *    PTHREAD_MUTEX_RECURSIVE_NP (osal/src/Mutex.cpp:44), and DriverImpl's destructor has the
 *    same shape.
 *
 * @see DriverAidlImpl
 * @see DriverImpl
 * @see Driver::getInstance()
 * @see CecTestEnvironment::SetUp
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "ccec/CECFrame.hpp"
#include "ccec/Connection.hpp"
#include "ccec/Driver.hpp"
#include "ccec/Exception.hpp"
#include "ccec/FrameListener.hpp"
#include "ccec/LibCCEC.hpp"
#include "ccec/OpCode.hpp"
#include "ccec/Operands.hpp"
#include "ccec/Util.hpp"

/*
 * DriverImpl.hpp lives under ccec/src, which AM_CPPFLAGS does not cover, so it is
 * reached by relative path rather than by adding an -I - the established route, taken
 * verbatim from test_DriverImpl_Async.cpp:75. It is needed here for one thing the
 * abstract Driver interface cannot express: naming the legacy concrete type in a
 * dynamic_cast, which is how the cases below establish which back-end this process
 * resolved without a production introspection API being added to Driver.hpp.
 */
#include "../../../ccec/src/DriverImpl.hpp"

/*
 * DriverAidlImpl.hpp is reached by the same relative route, and for the same reason: it
 * is absent from hdmicec/Makefile.am's installed nobase_include_HEADERS list, exactly as
 * DriverImpl.hpp is, which is what makes a second back-end possible without altering the
 * middleware public API - and what makes reaching it from a test translation unit
 * legitimate rather than a hack. It is needed for three things, all of which the abstract
 * interface hides: calling the public static preflight predicate
 * DriverAidlImpl::isBinderPreflightOk(), whose own documentation records that it is
 * public precisely so a test may call it; naming the AIDL concrete type in a
 * dynamic_cast, the other half of the selection assertions; and constructing a local
 * closed instance, which is what reaches this back-end's state guards and its
 * prelude ordering without touching the process-global driver every other suite in this
 * binary shares.
 */
#include "../../../ccec/src/DriverAidlImpl.hpp"

#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <com/rdk/hal/hdmicec/SendMessageStatus.h>
#include <com/rdk/hal/hdmicec/State.h>
#include <utils/StrongPointer.h>

/*
 * ProcessState, for the ONE observation that establishes the binder threadpool exists.
 *
 * Reached only from inside a DriverAidlSessionTest case body, never at file or fixture scope,
 * and only through selfOrNull(). That matters: on a driverless host merely reaching
 * ProcessState::self() raises SIGABRT in this pinned build, and DriverAidlSessionTest.* is
 * excluded from the driverless invocation by the runner's own filter. selfOrNull() adds a
 * second, independent guarantee -- it returns null rather than creating a ProcessState, so it
 * cannot open a driver that is not there.
 */
#include <binder/ProcessState.h>

/*
 * Spelled with quotes, not angle brackets, because that is how ccec/src/DriverAidlImpl.cpp
 * spells its own include of the same header, and there is nothing to be gained from a second
 * spelling. halcompat.h is not inside either frozen snapshot include root - it lives at
 * HALIF_PREFIX/common/current - which is why configure resolves that third root separately,
 * under its "for the halcompat.h include root" check in configure.ac, and why this include
 * would fail with the two snapshot roots alone.
 */
#include "halcompat.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fake_hdmi_cec_aidl_service.h"
#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::SetArgPointee;

/**
 * @brief Short alias for the generated AIDL package, spelled as ccec/src/DriverAidlImpl.cpp
 *        spells its own.
 *
 * Introduced instead of using-declarations for the individual types, for the reason the
 * production back-end records against its alias: this middleware compiles with
 * CCEC_NAMESPACE undefined, so its own names sit at global scope, and pulling generic names
 * such as State in beside them invites a collision a later header could create silently.
 */
namespace cechal = ::com::rdk::hal::hdmicec;

/**
 * @brief Short alias for the shared HAL compatibility helpers in halcompat.h, spelled as
 *        ccec/src/DriverAidlImpl.cpp spells its own.
 */
namespace halcompat = ::com::rdk::hal::halcompat;

/*
 * ---------------------------------------------------------------------------------
 * AIDL contract check: the authoritative values, and the one conflict between them
 *
 * Every number this file asserts against comes from one of four authorities, and where
 * two of them disagree the disagreement is a designed, declared difference between the
 * two back-ends rather than a defect. Tabulated so that a future reader does not have to
 * re-derive any of it:
 *
 *     IHdmiCec::VERSION            1000        IHdmiCec.h:25
 *     IHdmiCec::HASHVALUE          70a901cf... IHdmiCec.h:27
 *     SendMessageStatus            0 / 1 / 2   SendMessageStatus.h - ACK_STATE_0,
 *                                              ACK_STATE_1, BUSY, in that order
 *     REPORT_PHYSICAL_ADDRESS      0x84        ccec/include/ccec/OpCode.hpp:73
 *     CECFrame::MAX_LENGTH         128         ccec/include/ccec/CECFrame.hpp:55-57
 *     AIDL sendMessage maximum     16          IHdmiCecController.aidl - "The maximum
 *                                              message size (Header Block plus opcode
 *                                              block plus operand blocks) is 16 * 8 bits
 *                                              (16bytes)", enforced by
 *                                              DriverAidlImpl::write() against the
 *                                              file-local AIDL_MAX_MESSAGE_LENGTH in
 *                                              ccec/src/DriverAidlImpl.cpp
 *     legacy HAL frame maximum     20          the legacy HAL specification
 *
 * The conflict, and why it is not resolved here. A CECFrame can carry 128 bytes; the
 * legacy HAL accepts 20; the AIDL contract states 16. Each contract is authoritative for
 * its own back-end, and no divergence settles the overlap, so a frame of 17 to 20 bytes
 * is sendable on the legacy back-end and raises IOException on the AIDL one. That is an
 * authorized observable difference, and the important half of it is what does not happen:
 * the frame is policed, never truncated, because a truncated CEC frame on the bus is
 * worse than a refusal. The cases below assert both halves - the exception, and that
 * nothing reached the fake's sendMessage.
 *
 * The inverted ACK sense is the crux of the whole adaptation. SendMessageStatus does not
 * mean one thing: ACK_STATE_0 means acknowledged for a directed message and rejected for
 * a broadcast, and ACK_STATE_1 is the mirror of that. The destination nibble that decides
 * which reading applies is frame.at(0) & 0x0F, broadcast being 0x0F, read exactly as the
 * legacy implementation reads it. The production translation lives in
 * DriverAidlImpl::write(), and each of its arms reproduces one arm of the legacy mapping
 * in DriverImpl.cpp: :261-263 (a HAL error becomes IOException), :265-274 (the five-value
 * send-failed family, likewise IOException), :276-278 (a directed message not
 * acknowledged becomes CECNoAckException) and :279-284 (the CEC CTS 9-3-3 arm, where a
 * rejected broadcast REPORT_PHYSICAL_ADDRESS becomes CECNoAckException so the caller
 * retries, while every other rejected broadcast opcode returns normally).
 *
 * What is covered here and what is blocked. Physical-address retrieval - SC6(f) - is
 * covered on the legacy back-end, where the legacy mock's HdmiCecGetPhysicalAddress
 * expectation is met and the value reaches the caller. On the AIDL back-end it is
 * BLOCKED ITEM B1 and cannot be covered at all, because the method does not read an
 * address: the mandated replacement is an EDID-byte read belonging to the device settings
 * HAL layer rather than to the CEC HAL, and the public header declaring it was to be
 * supplied as a separate input to this migration and was not. Every alternative route is
 * closed - reconstructing the declaration from the plugin test mocks is forbidden and a
 * mock is usage; the AIDL HDMI-output controller EDID event is forbidden outright;
 * substituting any CEC HAL call is forbidden; and acquiring a legacy CEC handle lazily is
 * both that forbidden substitution and independently unsafe, since HdmiCecOpen() performs
 * logical-address discovery for source devices and would put a second CEC controller on
 * the wire. So what is asserted on the AIDL back-end is the documented interim behaviour
 * and nothing more: the block is logged and the caller's out-parameter is left untouched,
 * proved with a distinctive sentinel. This is a partial discharge of SC6(f) and is
 * recorded as one rather than presented as a pass.
 *
 *   Required change, reported not made - supply the device-settings HAL public header
 *   that declares the EDID-byte read, implement DriverAidlImpl::getPhysicalAddress()
 *   against that declaration in place of the B1 log and the untouched out-parameter, then
 *   review DriverImpl::getPhysicalAddress() for whether the same read should replace its
 *   HdmiCecGetPhysicalAddress() call so that both back-ends use the mandated API, and
 *   finally convert the legacy-only assertion below into a both-paths assertion. Until
 *   that header exists the AIDL path has no physical address, which is a real functional
 *   gap and is reported as one.
 *
 * A second item is provisional rather than blocked, and the difference matters.
 * IHdmiCec::close() is a high-confidence candidate for the legacy HdmiCecClose(), pending
 * owner confirmation: the HAL mapping table carries no entry for HdmiCecClose() and no
 * divergence settles it. The candidate is used because a back-end that cannot close is
 * not deliverable, and DriverAidlImpl::close() carries the same marker on the method. The
 * close cases below therefore assert the sequence - state CLOSED before the raise, local
 * address list deliberately not cleared - which is what the legacy back-end does and what
 * stays true whichever AIDL method the owners confirm. A green result here does not
 * confirm the mapping.
 * ---------------------------------------------------------------------------------
 */

/*
 * ---------------------------------------------------------------------------------
 * Fixture manifest - the handoff to whoever wires the invocation matrix
 *
 * The back-end selection resolves once per process, inside the LibCCEC::init() call in
 * CecTestEnvironment::SetUp (tests/L1Tests/test_main.cpp), which is the first thing in this
 * binary to force Driver::getInstance(). By the time any TEST_F body runs the choice is
 * made, so one binary run yields one selection outcome and every outcome needs its own
 * process. That is why the fixtures below are partitioned by invocation and why each
 * invocation-specific one asserts its precondition in SetUp instead of adapting to
 * whatever it finds.
 *
 *   Fixture                       Cases  Invocation  CEC_TEST_AIDL_MODE  Binder driver
 *   ---------------------------------------------------------------------------------
 *   DriverAidlCompatibilityTest      23  A, B, C     any                 no
 *   DriverAidlPreflightTest          28  A, B, C     any                 no
 *   DriverAidlSelectionTest           4  A           absent              no
 *   DriverAidlLocalInstanceTest      25  A, B, C     any                 no
 *   DriverAidlLegacyArmTest           5  A           absent              no
 *   DriverAidlSessionTest            27  B           compatible          yes
 *   DriverAidlTransmitTest           12  B           compatible          yes
 *   ---------------------------------------------------------------------------------
 *                                   124  of which 85 run under invocation A
 *
 * The 85 are the first five fixtures, 23 + 28 + 4 + 25 + 5. The two that invocation A
 * excludes are DriverAidlSessionTest, 27 cases, and DriverAidlTransmitTest, 12 - the 39 in
 * the excluded column below - because both require the AIDL back-end to be the resolved one.
 *
 * The counts the runner will see. Its per-invocation gate reconciles selected plus excluded
 * against registered, so all three numbers matter and a stale one fails the invocation:
 *
 *   invocation   registered   selected   excluded
 *   ------------------------------------------------
 *   A                   607        568         39
 *   B                   607        423        184
 *   C                   607        384        223
 *
 * Registered is 607 = 483 pre-existing + the 124 above. Every figure in that table is measured
 * on this host, with the runner's own filters, by
 *
 *   ./run_L1Tests --gtest_list_tests --gtest_filter=<filter> | grep -cE '^  [A-Za-z]'
 *
 * and the filters taken verbatim from run_coverage.sh's INVOCATION_MATRIX. Listing is a
 * registration query and needs no binder driver, which is why B's and C's selection counts are
 * measurable here even though B and C cannot execute here - and they are measured rather than
 * derived, because arithmetic over the table above silently misses a suite that was renamed or
 * left unclassified.
 *
 * What is not measured here is the pass/fail outcome of invocations B and C. Only invocation A
 * has been executed on this host: 568 selected, 568 passed, exit 0. A binder-capable runner must
 * produce B's and C's outcomes; the counts above tell it what to expect, and a mismatch there
 * means a filter or a classification has drifted rather than that a test failed.
 *
 * The filters are not written out here, deliberately: they are derived by run_coverage.sh
 * from five classification constants - NEUTRAL_SUITES, LEGACY_BOUND_SUITES,
 * CONTRACT_ANY_BACKEND_SUITES, CONTRACT_LEGACY_ONLY_SUITES and CONTRACT_AIDL_ONLY_SUITES - and
 * assembled per invocation in INVOCATION_MATRIX. Copying the assembled strings here would create
 * a second definition that drifts; what matters for this file is only the classification each
 * fixture needs, which is the Invocation column above:
 *
 *   DriverAidlCompatibilityTest, DriverAidlPreflightTest, DriverAidlLocalInstanceTest
 *                                          -> CONTRACT_ANY_BACKEND_SUITES
 *   DriverAidlSelectionTest, DriverAidlLegacyArmTest
 *                                          -> CONTRACT_LEGACY_ONLY_SUITES
 *   DriverAidlSessionTest, DriverAidlTransmitTest
 *                                          -> CONTRACT_AIDL_ONLY_SUITES
 *
 * A fixture in none of those lists is what breaks the runner's selected-plus-excluded
 * reconciliation, so a new fixture here needs a matching entry there. Every case in this file
 * belongs to one of the seven fixtures above, and the seven are all classified.
 *
 * Invocation A's selection is negative - it excludes the two AIDL-only fixtures - and that is
 * not optional. Those two require the AIDL back-end to be the resolved one, so under invocation
 * A their SetUp fails, loudly, with a diagnostic naming the mode they need. That is the design,
 * and the alternative is rejected: GTEST_SKIP would make a skipped arm indistinguishable from a
 * passing one in an aggregate count, which is precisely the swallowed-failure shape this file
 * exists to avoid. No case in this file skips, on any invocation. run_coverage.sh carries the
 * hook for passing a filter through, GTEST_EXTRA_ARGS, and emits an advisory noting that the run
 * was a partial selection, which is the honest record.
 *
 * Invocation C selects only the three back-end-independent fixtures. DriverAidlSelectionTest is
 * excluded from it deliberately: under mode incompatible the legacy back-end is selected for a
 * different reason - a service that is present and rejected rather than absent - and the
 * mid-process registration case would then be registering a second service under a name already
 * taken, which the harness's own collision check (failIfServiceAlreadyPublished() in
 * tests/L1Tests/test_main.cpp) exists to forbid.
 *
 * EXPECTED_SUITE_PATTERN needs no edit for this file. It is deliberately a stable subset of the
 * nine oldest, largest fixtures, so adding a test file never requires touching it; and none of
 * the fixtures here should be added to it, because a run that exercised only this file is exactly
 * the kind of partial run that pattern exists to catch. The count gate in verify_results is not
 * a hardcoded number either - it reconciles the executed count against --gtest_list_tests - so
 * the cases added here need no expected-count edit.
 *
 * References into run_coverage.sh are by name rather than by line, here and everywhere below.
 * That file is a live one that grows, so a line citation into it stops identifying its subject
 * the moment it does; shell function and constant names are greppable and survive an edit, and a
 * line number is a claim about a file this one does not own.
 *
 * Invocations D and E belong to hdmicec/tests/L2Tests (test_main.cpp and
 * ccec/test_DualPathIntegration.cpp) rather than here, and are not duplicated here, because an
 * in-process fake is not IPC: libbinder resolves a name registered in the calling process
 * to the local BBinder, so interface_cast hands back that very object - no Bp* proxy is
 * created, no transaction crosses the binder driver, and the client threadpool is never
 * involved. What that loses is real transport, which is why the L2 tier hosts the fake in
 * its own process and why the genuine "callback arrives on a binder thread" evidence is
 * invocation E's rather than this file's.
 *
 * What it gains is the reason invocations B and C are in-process at all, and it is not a
 * consolation prize: halcompat::isCompatible reads the server's metadata through
 * getInterfaceHash() and getInterfaceVersion() (halcompat.h:164, :171), which on a local
 * object dispatch virtually and can therefore be overridden. A remote Bn* service cannot
 * report bad metadata at all, because its generated onTransact answers those transactions
 * from the compiled-in constants. So the compatibility-rejection branches are reachable
 * only in-process, which is why invocation C has no L2 counterpart.
 *
 * Invocation C is not redundant with the unit tests below, which is worth stating
 * because it looks redundant. "Present but incompatible falls back to legacy" is a
 * factory-level behaviour: the selection helper that acts on the answer is resolveBackEnd() in
 * ccec/src/Driver.cpp, declared in an anonymous namespace and therefore of internal linkage - it
 * cannot be called from here at all. A unit test of the predicate proves the predicate; only a
 * process that starts with an incompatible service registered proves the factory acts on it.
 *
 * ---------------------------------------------------------------------------------
 * What this file establishes, and where - stated so that no green result is read as evidence
 * for something it does not touch. Grouped by where the evidence actually comes from.
 *
 * Established by invocation A, on any host, with no binder driver and no service:
 *
 *   - every compatibility arm by direct call: accept, accept-newer-same-major, and rejection on
 *     a null service, an empty hash, "-1", "notfrozen", an older same-major version, a
 *     cross-major version and a cross-era version;
 *   - every preflight arm, through the probe seam: the empty path, an unopenable node, a node
 *     that opens but refuses the version ioctl, a node whose protocol version differs from this
 *     build's, a matching protocol whose context manager never answers, and the direct positive
 *     verdict - plus descriptor hygiene, one close per successful open, on all six;
 *   - the legacy halves of both authorized observable differences, and SC6(f)'s legacy half:
 *     the physical address really is read through the legacy HAL call;
 *   - the AIDL back-end's closed-state guards, its writeAsync prelude ordering and the three
 *     methods that carry no guard, on locally constructed instances;
 *   - that the factory returns the same object every time, and - only where the environment can
 *     publish a service, which this host cannot - that a service appearing mid-process does not
 *     change the resolved back-end. Where it cannot publish, that case says so in its own
 *     output and the SC6(c) evidence is explicitly not claimed;
 *   - that the two Sink call paths this file models still have the structure the model assumes,
 *     read from the real plugin source - which is a required input rather than an optional one:
 *     a source that cannot be located fails the case instead of reporting an absence, because a
 *     model nobody checked against its subject is not evidence about a caller;
 *   - the legacy back-end's receive path end to end, measured: a frame injected at the legacy
 *     HAL's Rx callback is delivered, within a bounded wait and byte for byte, to an application
 *     FrameListener through the Bus reader and Connection. That is the baseline the AIDL receive
 *     assertions are compared against - the receive path is not an authorized observable
 *     difference, so its AIDL half must match this - and it is also what proves the observation
 *     machinery those AIDL cases use (RecordingFrameListener, ListeningConnection, the bounded
 *     wait) is correctly wired, on a host where it can actually run.
 *
 * Registered here but only executable on a binder-capable runner (invocation B):
 *
 *   - the adapter's translation in both directions: one-element address marshalling for add,
 *     remove and get; the inverted ACK sense across directed and broadcast; the frame-length
 *     policing; the non-ok-status mapping on every consumed method;
 *   - the session lifecycle: the duplicate-open no-op, the null-controller and non-ok-status
 *     open guards, the close sequence, which controller was closed, and that the close sentinel
 *     is offered before the transaction's result is evaluated - proved with a reader actually
 *     parked on the incoming queue;
 *   - the receive path on the AIDL back-end: bounded observable delivery through the Bus reader
 *     to an application FrameListener, bounded non-delivery plus release while closed, and the
 *     post-detach drop after a failed close and after the owner's destruction. The observation
 *     machinery these use is exercised for real under invocation A by the legacy-arm receive
 *     case above, so a failure here is about the AIDL back-end rather than about the harness;
 *   - that a binder threadpool exists in this process, which is the only externally observable
 *     consequence of the obligation production's open() discharges;
 *   - the content of both diagnostic callbacks' reports, from the middleware's own captured
 *     output: onStateChanged's transition with both state names at LOG_INFO, and onMessageSent's
 *     status name, message length and message bytes at LOG_DEBUG - the level being raised for
 *     that one through production's own check_cec_log_status(), under the custody protocol
 *     ScopedCecLogLevel documents, with the raise asserted rather than reported so a refusal
 *     cannot pass as evidence.
 *
 * Not established by this file at any invocation, and named rather than glossed:
 *
 *   - real IPC. No case here causes a Bp* proxy to be created, a transaction to cross the binder
 *     driver, or a callback to arrive on a binder threadpool thread. An in-process fake resolves
 *     to the local BBinder and its triggers run on the calling thread. That evidence is
 *     invocation E's, in hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp, and the L1
 *     threadpool assertion above is the presence of the pool only - not its use;
 *   - the plugin's runtime behaviour. The Sink drift guard reads the plugin's source and asserts
 *     its call shape - and it fails rather than reports when that source cannot be located, so
 *     the shape is checked on every run or the run is red. It does not compile, link or run the
 *     plugin - that component's own test binaries link the framework's CEC mock in place of this
 *     middleware - so what the plugin does with the exceptions it catches remains its own
 *     suite's evidence and the middleware-level L3 step's;
 *   - the AIDL physical address. Blocked on B1, as the header above records. The AIDL arm asserts
 *     the documented interim behaviour, not a working read.
 * ---------------------------------------------------------------------------------
 */
namespace {

/**
 * @brief The AIDL sendMessage length contract, transcribed rather than imported.
 *
 * ccec/src/DriverAidlImpl.cpp defines AIDL_MAX_MESSAGE_LENGTH in an anonymous namespace, so
 * it has internal linkage and no test can reach it. Restating it here is therefore the only
 * option, and the static assertion below is what keeps the restatement honest: it pins
 * the CECFrame capacity the conflict rests on, so the two numbers cannot drift apart
 * unnoticed.
 *
 * @see DriverAidlImpl::write()
 */
constexpr size_t kAidlMaxMessageLength = 16;

/** @brief One byte past the AIDL contract and still within the legacy HAL's 20. */
constexpr size_t kJustOverAidlLimit = 17;

/** @brief The legacy HAL specification's maximum, the far end of the disputed range. */
constexpr size_t kLegacyMaxMessageLength = 20;

/**
 * @brief The selected-path log format, transcribed from SELECTED_BACK_END_LOG_FORMAT in
 *        ccec/src/Driver.cpp.
 *
 * That constant and the two back-end names below it live in an anonymous namespace in that
 * translation unit - by design, since the string is defined once and referenced nowhere
 * else - so they cannot be imported. What is copied here is exactly what is there, and the
 * case that uses it does not merely compare two local strings: it drives the production
 * logger with this format and asserts the line that comes out, which is what makes a
 * reworded production string, or a lowered default log level, visible here.
 */
const char *const kSelectedBackEndLogFormat =
    "Driver::getInstance : HDMI CEC HAL back-end selected : %s\r\n";

/** @brief Back-end name the AIDL arm substitutes, transcribed from SELECTED_BACK_END_AIDL. */
const char *const kSelectedBackEndAidl   = "AIDL";

/** @brief Back-end name the legacy arm substitutes, transcribed from SELECTED_BACK_END_LEGACY. */
const char *const kSelectedBackEndLegacy = "legacy";

/**
 * @brief This client's compiled-against interface version, copied into this translation unit.
 *
 * The copy is required rather than stylistic. IHdmiCec::VERSION is declared
 * `static const int32_t VERSION = 1000;` (IHdmiCec.h:25) with an in-class initializer and no
 * out-of-line definition anywhere in the snapshot - verified against the generated sources
 * and against the shipped libhdmicec-v0.1.0.0-cpp.so, which exports no such symbol. It is
 * therefore usable in a constant expression, and in a by-value argument, but odr-using it
 * fails to link: every GoogleTest comparison macro binds both operands to a const reference,
 * which is an odr-use, and the result is
 * `undefined reference to com::rdk::hal::hdmicec::IHdmiCec::VERSION` at link time. Measured,
 * not inferred. So the header value is read once here into a constexpr of this translation
 * unit's own, and it is that copy the macros below compare against. The static assertion
 * further down still reads the header member directly, which is the point: it is a constant
 * expression, so it pins the real value rather than this copy.
 *
 * @warning Comparing cechal::IHdmiCec::VERSION directly in an EXPECT_* or ASSERT_* macro does
 *          not compile to a working binary - use this copy.
 */
constexpr int32_t kClientInterfaceVersion = cechal::IHdmiCec::VERSION;

/**
 * @brief The real frozen interface hash this client was compiled against (IHdmiCec.h:27).
 *
 * HASHVALUE needs none of the treatment kClientInterfaceVersion documents - `static constexpr
 * char*` is implicitly inline in C++17, so it has a definition - but it is copied alongside
 * for symmetry, so that every comparison in this file names a local constant.
 */
const char *const kFrozenInterfaceHash = cechal::IHdmiCec::HASHVALUE;

/** @brief The interface hash halcompat rejects as a failed hash RPC (halcompat.h:165-167). */
const char *const kBrokenInterfaceHash = "-1";

/** @brief The hash a pre-freeze development server reports (halcompat.h:168-170). */
const char *const kUnfrozenInterfaceHash = "notfrozen";

/**
 * @brief The controller interface's compiled-against version, copied into this translation unit.
 *
 * The copy is required for exactly the reason kClientInterfaceVersion documents, and the
 * reason applies verbatim here: IHdmiCecController::VERSION is declared in-class with no
 * out-of-line definition, so odr-using it in a comparison macro fails to link. This copy is
 * what the fake-metadata cases compare the controller's reported version against.
 *
 * @see kClientInterfaceVersion
 */
constexpr int32_t kControllerClientInterfaceVersion = cechal::IHdmiCecController::VERSION;

/**
 * @brief The controller interface's frozen hash (IHdmiCecController.h), copied alongside.
 *
 * Needs none of the treatment the version copy above does, and is copied for the same
 * symmetry kFrozenInterfaceHash is: every comparison in this file names a local constant.
 *
 * @see kFrozenInterfaceHash
 */
const char *const kControllerFrozenInterfaceHash = cechal::IHdmiCecController::HASHVALUE;

/**
 * @brief A version no interface in this snapshot reports, installed to make a fake diverge.
 *
 * Deliberately not one of the halcompat table values below: those exist to select an arm of the
 * compatibility rule, whereas this one exists only to differ from whatever constant a fake would
 * otherwise report, which is the condition its divergence trace fires on. Using a table value
 * here would suggest the metadata cases were making a compatibility claim, and they are not.
 */
constexpr int32_t kDivergentReportedVersion = 4242;

/**
 * @brief A well-formed hash that is not the frozen one, installed for the same purpose.
 *
 * Forty hex characters, so it is the shape of a real hash rather than a sentinel: the fake stores
 * whatever it is given, and a value that looked like an error code would invite the reading that
 * only error codes can be installed.
 */
const char *const kDivergentReportedHash = "0000000000000000000000000000000000000000";

/** @brief A server version inside this client's era and major, newer than it: must be accepted. */
constexpr int32_t kNewerCompatibleVersion = 1010;

/** @brief The largest era-0 major-1 encoding, the far end of the accepted range. */
constexpr int32_t kNewestCompatibleVersion = 1999;

/** @brief The next major generation up: rejected by the cross-major conjunct. */
constexpr int32_t kCrossMajorVersion = 2000;

/** @brief Era 1: rejected because this client's era is 0. */
constexpr int32_t kCrossEraVersion = 100000;

/** @brief The generator default a pre-freeze server reports: era 0, major 0, so cross-major. */
constexpr int32_t kUnfrozenGeneratorVersion = 1;

/**
 * @brief Client half of a third-party version pair that reaches the era-0 older-same-major arm.
 *
 * The pair exists for one purpose only: that arm is unreachable for this client, as
 * unreachable path 1 in the file block proves, so it is reached with a different client
 * instead. 3020 is era 0, major 3, minor 2.
 *
 * @see kOlderSameMajorServer
 */
constexpr int32_t kOlderSameMajorClient = 3020;

/** @brief Server half of that pair: era 0, major 3, minor 0, so older within the same major. */
constexpr int32_t kOlderSameMajorServer = 3000;

/*
 * ---------------------------------------------------------------------------------
 * Compile-time invariants
 *
 * Every one of these pins a value that an assertion below silently depends on, and every
 * message says what to do if it fires rather than merely that it fired. The point is that
 * a snapshot regeneration or a header edit stops the build here, pointing at the analysis
 * it invalidated, instead of quietly inverting a runtime expectation.
 *
 * detail::isCompatible is constexpr (halcompat.h:108) and detail is an accessible
 * namespace, so the whole version table is checkable without running anything.
 * ---------------------------------------------------------------------------------
 */

static_assert(cechal::IHdmiCec::VERSION == 1000,
    "The compiled-against AIDL interface version is no longer 1000. Every version case in this "
    "file was derived for era 0, major 1 - and so were the two unreachable-by-construction "
    "conclusions recorded in the file block. Recompute both: decode the new VERSION with "
    "halcompat.h:93-96, re-derive which server values satisfy era(server) == era(client) and "
    "major(server) == major(client), and rebuild the accept/reject table below from that. In "
    "particular, if the new era is 1 or greater then the era >= 1 branch at halcompat.h:110-111 "
    "becomes reachable and must be covered rather than recorded as unreachable.");

static_assert(halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, cechal::IHdmiCec::VERSION),
    "A server reporting exactly this client's version is no longer compatible. That is the "
    "identity case; if it fails, the era rules at halcompat.h:108-115 changed fundamentally and "
    "CompatibleWhenServerReportsThisClientsVersion must be revisited before anything else.");

static_assert(halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kNewerCompatibleVersion),
    "A newer server within the same era and major is no longer accepted. Do not 'fix' this by "
    "asserting rejection: accepting a newer additive server is the documented rule "
    "(halcompat.h:100-103), and a test that treated 'not exactly our version' as incompatible "
    "would encode a rule that does not exist. If the rule genuinely changed, update "
    "CompatibleWhenServerReportsNewerVersionInSameMajor and this assertion together.");

static_assert(halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kNewestCompatibleVersion),
    "1999 - the largest era-0 major-1 encoding - is no longer accepted, so the accepted range is "
    "no longer the whole of [1000, 1999]. Unreachable path (1) in the file block is derived from "
    "that range being closed at the top; re-derive it before changing any case.");

static_assert(!halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kCrossMajorVersion),
    "A server in the next major generation is now accepted. In era 0 a major bump is breaking "
    "(halcompat.h:60-62), so this would mean the era-0 rule was relaxed; if that is intended, "
    "IncompatibleWhenServerReportsDifferentMajor must be inverted and the file block's account of "
    "which conjunct rejects 999 rewritten, since that case also relies on the cross-major "
    "conjunct.");

static_assert(!halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kCrossEraVersion),
    "An era-1 server now satisfies this era-0 client. Re-read halcompat.h:110-115: for that to "
    "happen either the era equality conjunct went away or this client's era changed. If the "
    "client moved to era 1 or later, the VERSION assertion above fires too and the whole table "
    "is rebuilt from there.");

static_assert(!halcompat::detail::isCompatible(cechal::IHdmiCec::VERSION, kUnfrozenGeneratorVersion),
    "The generator's default version 1 now satisfies a released client. That is the pre-freeze "
    "development server the rules exist to reject (halcompat.h:105-106); if it is now accepted, "
    "IncompatibleWhenServerReportsUnfrozenGeneratorVersion must be inverted deliberately and not "
    "by accident.");

static_assert(!halcompat::detail::isCompatible(kOlderSameMajorClient, kOlderSameMajorServer),
    "The genuine older-same-major proof no longer holds, and it is the only one this file has. "
    "(3020, 3000) is era 0 == era 0, major 3 == major 3, and 3000 >= 3020 false, so the ordering "
    "conjunct at halcompat.h:114 is what rejects it - which is exactly the arm being pinned. Note "
    "that halcompat.h:128's own static_assert(!isCompatible(3000, 2000), \"era0 older rejected\") "
    "is mislabelled: major(3000) is 3 and major(2000) is 2, so that pair is rejected by the "
    "cross-major conjunct and merely re-covers a case already covered. Do not substitute it for "
    "this one. And do not look for an equivalent pair for this client - unreachable path 1 in "
    "the file block proves none exists.");

static_assert(kDivergentReportedVersion != cechal::IHdmiCec::VERSION
                  && kDivergentReportedVersion != cechal::IHdmiCecController::VERSION,
    "kDivergentReportedVersion now equals a compiled-in interface version. The fake-metadata "
    "cases install it precisely because it differs: each fake's version getter traces only when "
    "the value it reports differs from its own compiled-in constant, so an equal value would "
    "drive the quiet arm while the cases still passed - the exact false green the divergence "
    "trace exists to prevent. Pick another value; nothing else depends on which.");

static_assert(static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0) == 0
              && static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_1) == 1
              && static_cast<int32_t>(cechal::SendMessageStatus::BUSY) == 2,
    "The SendMessageStatus enumerators were renumbered by a snapshot regeneration. Stop here: the "
    "ACK sense is inverted between directed and broadcast messages, so a renumbering that went "
    "unnoticed would silently swap 'acknowledged' for 'rejected' on one of the two - the exact "
    "defect the whole status-translation matrix below exists to prevent. Re-read the enumerator "
    "documentation, then re-derive every arm of DriverAidlImpl::write()'s status translation and "
    "every case in DriverAidlTransmitTest from it.");

static_assert(CECFrame::MAX_LENGTH == 128,
    "CECFrame's capacity changed, and it is one of the three numbers the frame-size difference "
    "rests on - the other two being the AIDL contract's 16 and the legacy HAL specification's 20. "
    "If the capacity dropped below 20 the legacy arm of FrameOfTwentyBytes... can no longer be "
    "constructed at all; if it dropped below 17 the disputed range vanishes and the difference "
    "stops being observable. Re-derive the three frame sizes before touching a case.");

static_assert(kAidlMaxMessageLength < kJustOverAidlLimit
              && kJustOverAidlLimit <= kLegacyMaxMessageLength
              && kLegacyMaxMessageLength <= CECFrame::MAX_LENGTH,
    "The three frame sizes no longer straddle the authority conflict they were chosen to "
    "straddle: 16 must be accepted by both back-ends, 17 must sit strictly above the AIDL limit "
    "and at or below the legacy one, and 20 must still fit in a CECFrame. Fix the constants, not "
    "this assertion - and if AIDL_MAX_MESSAGE_LENGTH in ccec/src/DriverAidlImpl.cpp changed, "
    "kAidlMaxMessageLength here is the transcription that must follow it.");

static_assert(REPORT_PHYSICAL_ADDRESS == 0x84,
    "REPORT_PHYSICAL_ADDRESS is no longer 0x84. It is the selector for the CEC CTS 9-3-3 arm in "
    "DriverAidlImpl::write() (mirroring DriverImpl.cpp:279-284), which is the one rejected "
    "broadcast opcode that must raise CECNoAckException while every other rejected broadcast "
    "returns normally. Update the CTS-arm case and its negative control together, or the two "
    "cases will be asserting the same opcode and the distinction will be untested.");

/**
 * @brief Self-contained stdout capture, at file-descriptor level.
 *
 * Reproduced from the established implementation in this same directory
 * (ccec/test_Util.cpp:143-211) rather than invented, and for the reasons recorded there:
 * the code under test logs through printf - C stdio writing to fd 1 - so redirecting the
 * C++ std::cout streambuf would capture nothing, and GoogleTest's CaptureStdout lives in
 * ::testing::internal, which upstream documents as outside the public API.
 *
 * Why this file needs it at all, since the timing looks wrong at first glance. The
 * selected-path line is emitted once, inside LibCCEC::init in the global environment's
 * SetUp, long before any test body runs, so no test can capture the original emission -
 * that line has already gone to the process log, which is where run_coverage.sh greps for
 * it. What the case below does instead is drive the production logger with the transcribed
 * contract format and capture that, into an anonymous temporary. Three things follow, and
 * the third is why the redirection matters: the transcribed format is proved to be a format
 * CCEC_LOG accepts and substitutes into; the emitted line is proved to still pass the
 * default log-level filter, so a lowered default that silently broke every consumer of the
 * line would be caught here; and the re-emitted line goes only into the temporary, never
 * into the process log, so that log still carries exactly one selected-path hit and the
 * runner's grep contract is untouched.
 *
 * tmpfile() is used rather than a named path: it is unlinked as it is created, so there is
 * nothing for another process to substitute.
 *
 * @warning Redirection is process-wide while an instance is alive. Keep the lifetime to the
 *          narrowest scope that brackets the emission being captured, and call read() - or
 *          let the destructor run - before writing anything that must reach the real stdout.
 * @see isValid()
 */
class StdoutCapture {
public:
    /**
     * @brief Redirects fd 1 into an anonymous temporary file.
     *
     * Every step is failure-checked and unwound in place rather than reported by throwing,
     * because a capture that cannot be established must leave the process's stdout exactly
     * as it found it. The outcome is read with isValid(), which the cases assert before they
     * rely on anything captured.
     *
     * @post isValid() reports whether the redirection is in force; on failure nothing is
     *       redirected and no descriptor is retained.
     */
    StdoutCapture()
        : savedStdout_(-1)
        , sink_(::tmpfile())
    {
        if (sink_ == nullptr) {
            return;
        }

        ::fflush(stdout);
        savedStdout_ = ::dup(STDOUT_FILENO);
        if (savedStdout_ < 0) {
            ::fclose(sink_);
            sink_ = nullptr;
            return;
        }

        if (::dup2(::fileno(sink_), STDOUT_FILENO) < 0) {
            ::close(savedStdout_);
            savedStdout_ = -1;
            ::fclose(sink_);
            sink_ = nullptr;
        }
    }

    /** @brief Not copyable: two instances would each own one process-wide redirection. */
    StdoutCapture(const StdoutCapture &) = delete;

    /** @brief Not assignable, for the same reason the copy constructor is deleted. */
    StdoutCapture &operator=(const StdoutCapture &) = delete;

    /**
     * @brief Restores the real stdout and releases the temporary, on every exit path.
     *
     * Idempotent with read(), which performs the same restoration, so a case that reads the
     * capture and a case that abandons it both leave fd 1 pointing where it started. Nothing
     * is thrown.
     */
    ~StdoutCapture() {
        restore();
        if (sink_ != nullptr) {
            ::fclose(sink_);
        }
    }

    /**
     * @brief Reports whether the redirection was established.
     *
     * @return bool - true when fd 1 is redirected into this instance's temporary and the
     *                original descriptor is held for restoration, false when construction
     *                could not establish either, in which case nothing was redirected.
     */
    bool isValid() const { return sink_ != nullptr && savedStdout_ >= 0; }

    /**
     * @brief Ends the capture and returns everything written while it was in force.
     *
     * Restores the real stdout first, so nothing written afterwards is swallowed, then reads
     * the temporary from the beginning.
     *
     * @return std::string - The captured bytes, empty when construction failed or when
     *                       nothing was written. Safe to call more than once; later calls
     *                       return the same content.
     */
    std::string read() {
        restore();
        if (sink_ == nullptr) {
            return std::string();
        }

        ::fflush(sink_);
        ::rewind(sink_);

        std::string captured;
        char buffer[512];
        size_t bytesRead = 0;
        while ((bytesRead = ::fread(buffer, 1, sizeof(buffer), sink_)) > 0) {
            captured.append(buffer, bytesRead);
        }
        return captured;
    }

private:
    /**
     * @brief Puts fd 1 back and releases the saved descriptor, at most once.
     *
     * The single restoration path, shared by read() and the destructor, which is what makes
     * calling them in either order safe.
     */
    void restore() {
        if (savedStdout_ >= 0) {
            ::fflush(stdout);
            ::dup2(savedStdout_, STDOUT_FILENO);
            ::close(savedStdout_);
            savedStdout_ = -1;
        }
    }

    /** @brief Duplicate of the original fd 1, or -1 when no redirection is in force. */
    int savedStdout_;

    /** @brief The anonymous temporary receiving the captured output, or nullptr on failure. */
    FILE *sink_;
};

/**
 * @brief The minimal correct double for every compatibility arm.
 *
 * halcompat::isCompatible<I> takes a const android::sp<I>& and reads the server's metadata
 * through getInterfaceHash() and getInterfaceVersion() (halcompat.h:164, :171), both of
 * which dispatch virtually. So a locally constructed subclass of the snapshot's own
 * IHdmiCecDefault (IHdmiCec.h:40-72) overriding only those two members is sufficient:
 * there is no need for a Bn* server base, no registration with the service manager and no
 * onTransact involvement whatsoever. IHdmiCec derives from ::android::IInterface and hence
 * from RefBase, so holding one in an android::sp<> is valid.
 *
 * IHdmiCecDefault answers every interface method with UNKNOWN_TRANSACTION, which is
 * exactly right here - the compatibility check never calls one. Its stock metadata is an
 * empty hash (IHdmiCec.h:69-71) and version 0 (:66-68), so the empty-hash arm needs no
 * override at all and is asserted against the stock object deliberately, as evidence that
 * the default really is empty rather than something this file arranged.
 *
 * @warning Deliberately not derived from BnHdmiCec. A Bn* object could be registered and
 *          reached remotely, and its generated onTransact would answer the metadata
 *          transactions from the compiled-in constants - which would make the overrides
 *          below inert. Local, virtual dispatch is the whole mechanism.
 * @see frozenDoubleReportingVersion()
 * @see doubleReportingHash()
 */
class MetadataDouble : public cechal::IHdmiCecDefault {
public:
    /**
     * @brief Constructs a double that will report exactly the metadata given.
     *
     * @param [in] hash    - Interface hash to report, taken by value and moved into place so
     *                       a caller may pass the frozen hash, "-1", "notfrozen" or anything
     *                       else an arm needs.
     * @param [in] version - Interface version to report, in halcompat's positional encoding.
     */
    MetadataDouble(std::string hash, int32_t version)
        : hash_(std::move(hash))
        , version_(version)
    {
    }

    /**
     * @brief Answers the interface-hash query halcompat::isCompatible issues.
     *
     * @return std::string - The hash this double was constructed with, unmodified.
     */
    std::string getInterfaceHash() override { return hash_; }

    /**
     * @brief Answers the interface-version query halcompat::isCompatible issues.
     *
     * @return int32_t - The version this double was constructed with, unmodified.
     */
    int32_t getInterfaceVersion() override { return version_; }

private:
    /** @brief Hash reported to every caller of getInterfaceHash(). */
    std::string hash_;

    /** @brief Version reported to every caller of getInterfaceVersion(). */
    int32_t version_;
};

/**
 * @brief Builds a compatibility double reporting the real frozen hash and a chosen version.
 *
 * The hash is taken from the generated header rather than written out, so a snapshot
 * regeneration cannot leave a stale literal here that would send every version case down
 * the broken-hash arm at halcompat.h:165-167 and make them all pass for the wrong reason.
 *
 * @param [in] version - Interface version the double reports, in halcompat's encoding.
 * @return android::sp<cechal::IHdmiCec> - A local double reporting the frozen hash and that
 *                                         version, never null.
 */
::android::sp<cechal::IHdmiCec> frozenDoubleReportingVersion(int32_t version) {
    return ::android::sp<MetadataDouble>::make(std::string(kFrozenInterfaceHash), version);
}

/**
 * @brief Builds a compatibility double reporting a chosen hash and a compatible version.
 *
 * The version is this client's own, so a rejection can only have come from the hash. A
 * double that reported both a bad hash and a bad version would pass its case while
 * proving nothing about which arm fired.
 *
 * @param [in] hash - Interface hash the double reports. Must not be null.
 * @return android::sp<cechal::IHdmiCec> - A local double reporting that hash and this
 *                                         client's own version, never null.
 */
::android::sp<cechal::IHdmiCec> doubleReportingHash(const char *hash) {
    return ::android::sp<MetadataDouble>::make(std::string(hash), kClientInterfaceVersion);
}

/**
 * @brief The directory temporary nodes are created in, honouring a validated TMPDIR
 *
 * TMPDIR is the environment's way of saying where scratch files belong, and a run whose
 * scratch space is a private per-run directory means it. It is validated rather than trusted,
 * and the validation is not ceremony:
 *
 *   It must be absolute. A relative value would place the node relative to the working
 *   directory, which for this binary is inside the source tree - a stray file there is worse
 *   than ignoring the variable, because it shows up as untracked repository content.
 *
 *   It must exist, be a directory, and be usable. Otherwise mkstemp would simply fail and the
 *   case that needs the node would report a precondition failure for a reason that has nothing
 *   to do with the code under test.
 *
 * Anything that fails validation falls back to /tmp, which is the behaviour this helper had
 * unconditionally before.
 *
 * @return std::string - An absolute directory path, without a trailing separator.
 */
std::string temporaryDirectory() {
    const char *const configured = ::getenv("TMPDIR");

    if (configured != nullptr && configured[0] == '/') {
        struct stat info;

        if (::stat(configured, &info) == 0 && S_ISDIR(info.st_mode)
            && ::access(configured, W_OK | X_OK) == 0) {
            std::string directory(configured);

            while (directory.size() > 1u && directory[directory.size() - 1u] == '/') {
                directory.erase(directory.size() - 1u);
            }
            return directory;
        }
    }

    return std::string("/tmp");
}

/**
 * @brief An openable non-binder filesystem node, unlinked by its own destructor
 *
 * Used by the preflight case that needs a path which opens but is not a binder driver, so that
 * the BINDER_VERSION ioctl arm is the one reached. An anonymous tmpfile() cannot serve: the
 * predicate takes a path and opens it itself, so the node has to be nameable for the duration
 * of the call.
 *
 * The lifetime is RAII and not a call, which is the whole reason this is a class. A manual
 * unlink after the predicate returns is skipped by every path that does not reach it - a fatal
 * assertion earlier in the case, an exception, a timeout that kills the process - and each of
 * those leaves a file behind in a directory that is very often shared. The destructor runs on
 * all of them bar the last, and the last leaves at most one file in a directory the
 * environment nominated for exactly this purpose.
 */
class TemporaryNode {
public:
    /** @brief Creates the node; isValid() reports whether it succeeded. */
    TemporaryNode() {
        const std::string pattern = temporaryDirectory() + "/blitzy_cec_aidl_preflight_XXXXXX";

        std::vector<char> mutablePattern(pattern.begin(), pattern.end());
        mutablePattern.push_back('\0');

        const int fd = ::mkstemp(mutablePattern.data());

        if (fd < 0) {
            return;
        }
        ::close(fd);
        path_.assign(mutablePattern.data());
    }

    /** @brief Not copyable: two owners would each unlink the one node. */
    TemporaryNode(const TemporaryNode &) = delete;

    /** @brief Not assignable, for the same reason the copy constructor is deleted. */
    TemporaryNode &operator=(const TemporaryNode &) = delete;

    /**
     * @brief Unlinks the node, on whichever exit path the case takes.
     *
     * The whole reason this is a class rather than a pair of calls. Nothing is thrown, and a
     * node that was never created is left alone.
     */
    ~TemporaryNode() {
        if (!path_.empty()) {
            ::unlink(path_.c_str());
        }
    }

    /**
     * @brief Reports whether the node was created.
     *
     * @return bool - true when a node exists at path() and will be unlinked by the destructor,
     *                false when mkstemp() failed, in which case path() is empty.
     */
    bool isValid() const { return !path_.empty(); }

    /**
     * @brief The node's path, for handing to the predicate under test.
     *
     * @return const std::string& - The absolute path, or an empty string when creation failed.
     */
    const std::string &path() const { return path_; }

private:
    /** @brief Path of the created node, empty when creation failed. */
    std::string path_;
};

/**
 * @brief Takes custody of the middleware's log-configuration file, raises the level for a
 *        scope, and hands the file and the level back under a checked, verified restoration.
 *
 * The middleware's level lives in `cec_log_level`, a file-static in `ccec/src/Util.cpp` with
 * no setter: the only route to it is `check_cec_log_status()`, which reads a level name from
 * the path production hardcodes - `fopen("/tmp/cec_log_enabled")` at `ccec/src/Util.cpp:82` -
 * and maps it onto that static. So raising the level from a test means writing production's
 * own file and calling production's own reader; there is no other seam, and adding one would
 * be a production change. This guard writes that file, drives that reader, and hands both back
 * - content, mode, owner, existence and the process-wide level - through restoreAndVerify().
 *
 * Restoration is checked and proved, not attempted. restoreAndVerify() runs on the case's
 * normal path, while the custody lock is still held, and it verifies three things rather than
 * assuming any of them: that the publication itself succeeded, every syscall result taken
 * rather than discarded; that the bytes at the path are the captured original, re-read
 * `O_NOFOLLOW`, or that the path is absent again where this guard created it; and that the
 * effective level is the one observed on the way in, re-observed through the same probe. The
 * third is not implied by the second: `check_cec_log_status()` returns early when the path
 * cannot be opened and silently leaves the level alone when the file's first line matches
 * nothing in production's table, so "the file is back" and "the level is back" are two facts
 * and only one of them is about the host's verbosity. That is also why restoration asks for the
 * entry level by name before it publishes the original bytes - the ordering
 * `UtilTest::TearDown()` in `tests/L1Tests/ccec/test_Util.cpp:636-653` uses, for the same
 * reason. A case asserts on what restoreAndVerify() returns.
 *
 * The destructor and the atexit hook are backstops, for the fatal assertion or the `exit()`
 * that skips the explicit call - never the route a case relies on. A destructor cannot fail a
 * test, so a suite whose only restoration is a destructor reports success for a run that left
 * the host altered; that is the shape restoreAndVerify() closes, and it is why the backstops
 * report their own failures on stdout, which is the only reader they have.
 *
 * Threat model, which is why this class is as long as it is. The path is fixed by production
 * and lives in a shared, world-writable directory, which is the classic unsafe-temporary
 * shape: anybody on the host can plant a symlink at that name, swap it between two of this
 * process's syscalls (CWE-59, CWE-367), or write the file concurrently - and a second
 * `run_L1Tests` on the same host is not hypothetical, since sibling clones share `/tmp`. A
 * plain truncating `ofstream` write to that name follows a planted link and truncates
 * whatever it refers to with this process's privileges, and a run that dies between the
 * truncate and the restore leaves the victim corrupted rather than merely modified. The
 * protocol here is therefore lock, then validate, then atomically replace: an exclusive
 * advisory lock on a companion path taken before anything is captured; `lstat()`
 * classification that refuses anything which is not a regular file this user owns, confirmed
 * against the opened descriptor so the classification cannot be swapped out from under it;
 * reads and writes that never follow a link; and a write that goes to a fresh `O_EXCL`
 * temporary in the same directory and is `rename()`d over the path, so a planted link is
 * replaced rather than written through. Restoration runs the same primitive in reverse, under
 * the same still-held lock. The contract is the repository's own - `LogConfigGuard` in
 * `tests/L1Tests/ccec/test_Util.cpp` owns the identical path for the identical reason - and
 * the lock path is deliberately the same string, derived from the same configuration-path
 * constant, so that the two units genuinely exclude one another across processes rather than
 * each holding a lock the other has never heard of.
 *
 * Why a test needs it at all. `onMessageSent` reports at `LOG_DEBUG`, which the default level
 * of `LOG_INFO` suppresses. A case that asserted on such a line without raising the level
 * would either fail for a reason that is not a defect or - worse - be written to assert
 * nothing, which is the shape this guard exists to close: a listener whose body reported
 * nothing would otherwise pass.
 *
 * Failure is reported, never thrown and never guessed at. Every refusal - a lock another
 * process holds, a symlink or a foreign owner at the path, an oversized file that could not be
 * reproduced faithfully, a failed atomic replace - leaves the filesystem exactly as it was,
 * leaves isRaised() false, and records which of those it was in failureReason(). The call site
 * asserts on isRaised() and streams that reason, so "another run holds the lock" and "the path
 * is a symlink" reach the log as themselves instead of collapsing into "could not raise".
 *
 * @warning Not reentrant and not thread safe - the level it moves is process wide, and the
 *          lock is per open file description, so a second instance in this process is refused
 *          exactly as a second process would be. Only ever construct one at a time, on the
 *          test thread.
 * @see check_cec_log_status()
 * @see failureReason()
 * @see restoreAndVerify()
 */
class ScopedCecLogLevel {
public:
    /**
     * @brief Takes custody of the configuration path and raises the level to @p levelName.
     *
     * @param [in] levelName - A name from production's own table (`ccec/src/Util.cpp:58-67`),
     *                         e.g. "DEBUG". A null or empty name, or one too long for the
     *                         single-line configuration file, is refused rather than written.
     *
     * @post isRaised() reports whether the level actually moved: the lock was taken, the path
     *       classified, the effective level observed so restoration has a reference to return
     *       to, the requested name written atomically, read back to confirm the bytes at the
     *       path are the ones written, and production's reader driven with them in place.
     *       False means the level was not raised. In every refusal before publication nothing
     *       on disk was changed. In the one refusal after publication - the read-back finding
     *       foreign bytes - the original is rolled back and the rollback is verified by the
     *       same checks restoreAndVerify() applies; where that rollback cannot be proved, the
     *       custody lock is deliberately retained and the atexit backstop left armed so the
     *       destructor retries and reports, because a failed assertion in the calling case
     *       restores nothing and protects no other process. failureReason() distinguishes the
     *       three outcomes. A case must assert on the return value rather than treat
     *       suppressed output as an assertion that held.
     * @post On success the exclusive lock is held until restoreAndVerify() succeeds or this
     *       object is destroyed, whichever comes first. On a refusal it has been released -
     *       except on the unproved-rollback path above, where it is held to the destructor for
     *       the same reason it is held on success: something published is not yet proved back.
     */
    explicit ScopedCecLogLevel(const char *levelName)
        : raised_(false)
        , restorePending_(false)
        , restored_(false)
        , existed_(false)
        , lockFd_(-1)
        , savedLength_(0)
        , savedMode_(0600)
        , savedUid_(static_cast<uid_t>(-1))
        , savedGid_(static_cast<gid_t>(-1))
        , entryLevel_(kUnobservableLevel)
        , restoreErrno_(0)
    {
        saved_[0] = '\0';

        /*
         * The line is formatted first, into a fixed buffer, because a name that does not fit
         * must be refused before the lock is taken and before anything is captured - there is
         * nothing to undo at that point.
         */
        char line[64];

        if (levelName == nullptr || levelName[0] == '\0') {
            failureReason_ = "no level name was given, so there is no level to raise to";
            return;
        }

        const int formatted = std::snprintf(line, sizeof(line), "%s\n", levelName);

        if (formatted <= 0 || static_cast<size_t>(formatted) >= sizeof(line)) {
            failureReason_ = std::string("the level name [") + levelName
                + "] does not fit the " + std::to_string(sizeof(line))
                + "-byte configuration line this guard writes";
            return;
        }

        if (!buildCustodyPaths()) {
            failureReason_ = std::string("the lock and temporary paths derived from ")
                + kLogConfigPath + " do not fit their buffers, so custody cannot be taken";
            return;
        }

        /*
         * exclusive first, capture second. The path is fixed by production code, so a second
         * writer is possible; capturing before locking would capture whatever that writer had
         * already put there and later hand it back as though it were the original.
         */
        if (!acquireLock()) {
            return;
        }

        if (!captureCurrentState()) {
            releaseLock();
            return;
        }

        /*
         * the reference restoration must return to, observed before anything is raised.
         *
         * It is deliberately observed rather than derived from the captured bytes, and the
         * two are not interchangeable: `check_cec_log_status()` returns early when the path
         * cannot be opened (`ccec/src/Util.cpp:82-86`), so a host whose configuration file is
         * absent, or holds a name production's table does not carry, has an effective level
         * that the file does not describe at all. Restoring to what the file said would then
         * move a level this guard never raised.
         *
         * Observed inside the custody window - after the lock, before the publication - so
         * that no other lock-respecting writer can move the level between the observation and
         * the raise, which would make the reference stale before it was ever used.
         */
        entryLevel_ = probeEffectiveLevel();

        if (entryLevel_ == kUnobservableLevel) {
            failureReason_ = std::string("no CCEC_LOG level produced output, so the effective"
                                         " level this guard would have to restore could not be"
                                         " observed; refusing to raise it, because a raise"
                                         " whose restoration cannot be PROVED would leave the"
                                         " verbosity of every later case in this binary"
                                         " unknown");
            releaseLock();
            return;
        }

        registerAtExitOnce();
        sActive_ = this;

        if (atomicWrite(line, static_cast<size_t>(formatted)) != 0) {
            /*
             * rename() is the only step that publishes anything and it is all-or-nothing, so
             * a failure anywhere in atomicWrite leaves the path as it was found and the
             * temporary already unlinked. There is nothing to restore.
             */
            failureReason_ = std::string("could not replace ") + kLogConfigPath
                + " atomically: " + std::strerror(errno)
                + " (the original is untouched and the level was not raised)";
            sActive_ = nullptr;
            releaseLock();
            return;
        }

        if (!pathHolds(line, static_cast<size_t>(formatted))) {
            /*
             * The bytes at the path are not the ones just written, so something outside this
             * lock replaced it in the interval. This object has modified the file by now, so
             * the original goes back before the refusal is reported - and whether that
             * succeeded is carried into the reason rather than discarded, because a refusal
             * that also failed to put the original back is a different, worse condition than a
             * refusal that left the host as it found it.
             */
            restorePending_ = true;

            failureReason_ = std::string(kLogConfigPath)
                + " does not hold the level this guard wrote, so a writer outside the custody"
                  " lock replaced it; the level was not raised and ";

            const char *const publicationFailure = restoreLevelAndPath();
            std::string verificationDetail;

            if (publicationFailure == nullptr && verifyRestoredState(verificationDetail)) {
                /*
                 * Rolled back and proved, by the same two checks restoreAndVerify() uses, so
                 * this object has nothing outstanding and custody can go. Publishing the
                 * original without verifying it would leave exactly the gap this branch is
                 * here to close: the refusal would be reported while the host stayed altered.
                 */
                restorePending_ = false;
                failureReason_ += "the original has been restored and verified";
                sActive_ = nullptr;
                releaseLock();
                return;
            }

            /*
             * rollback unproved, so custody is kept. Either the publication failed or the
             * verification did, and in both cases the host may be carrying bytes or a level it
             * did not have. Releasing the lock and clearing the active pointer here would
             * disarm every backstop at precisely the moment one is needed: the calling case
             * fails on isRaised(), but a failed assertion restores nothing and protects no
             * other process. So the lock stays held, the atexit hook stays armed, and the
             * destructor retries and reports - which is why restorePending_ remains true.
             */
            failureReason_ += (publicationFailure != nullptr)
                ? std::string("THE ORIGINAL COULD NOT BE RESTORED EITHER: ")
                      + publicationFailure + " (errno " + std::to_string(restoreErrno_) + ": "
                      + std::strerror(restoreErrno_) + ")"
                : std::string("THE ROLLBACK COULD NOT BE VERIFIED: ") + verificationDetail;
            failureReason_ += ". The custody lock is deliberately still held and the atexit"
                              " backstop still armed, so the destructor retries and reports;"
                              " until one of them succeeds this host may carry state this run"
                              " left behind";
            return;
        }

        check_cec_log_status();
        raised_ = true;
    }

    /**
     * @brief Restores the path and the process-wide level, proves it, and releases custody.
     *
     * This is the route a case must take, and the only one that can tell a case anything. It
     * runs while the custody lock is still held, so the original goes back before any other
     * process can observe the raised value as though it were the host's, and it performs three
     * verifications rather than one - each of which has its own way of failing silently:
     *
     *   1. Publication. restoreLevelAndPath() names the entry level back through production's
     *      reader and then publishes the captured original, or removes the file this guard
     *      created where there was none. Every step's outcome is checked - a discarded write,
     *      rename or unlink leaves the shared file holding this run's raised level, or leaves
     *      behind a file the host never had.
     *   2. Content, or absence. The path is re-opened `O_RDONLY|O_NOFOLLOW` and its bytes
     *      compared with the captured original, or its absence confirmed through `lstat`
     *      reporting ENOENT. A rename that reported success against a path something else has
     *      since replaced is caught here and nowhere else.
     *   3. The effective level. `probeEffectiveLevel()` is run again and compared with the
     *      level observed at construction, which is what proves "the file went back and the
     *      level followed it". The two really are separate facts, and step 1's ordering exists
     *      because of it: `check_cec_log_status()` matches the file's first line against
     *      production's own table and silently leaves `cec_log_level` alone when nothing
     *      matches (`ccec/src/Util.cpp:87-97`), so a byte-perfect restoration of a file that
     *      names no level would leave the raised level in force. Step 1 asks for the level by
     *      name so that cannot happen; this step is what establishes that it did not.
     *
     * Custody is released, and the atexit backstop disarmed, only after all three hold. On
     * failure the lock is kept and the backstop stays armed, so the destructor still tries.
     *
     * @param [out] detail - Empty on success. Otherwise names which of the three verifications
     *                       failed and what was observed, in a form a failure message can
     *                       stream, so that a publication fault, a foreign writer and a level
     *                       that did not move are distinguishable rather than collapsed into
     *                       "restore failed".
     *
     * @return bool - True when the path and the process-wide level are proved to be what this
     *         guard found, or when there is nothing to take back (this guard refused to raise,
     *         or a previous call already restored). False means the host may be altered and the
     *         calling case must fail.
     *
     * @post On true, isRaised() is false, custody is released and the destructor does nothing.
     * @post Idempotent: a second call returns true having touched neither the path nor the
     *       level.
     */
    bool restoreAndVerify(std::string &detail)
    {
        detail.clear();

        if (restored_ || (!raised_ && !restorePending_)) {
            /*
             * Nothing was ever published, or it has already been taken back and proved. Either
             * way there is nothing to undo and nothing to verify - a second call must not
             * rewrite a path this guard no longer has custody of.
             *
             * restorePending_ is tested separately from raised_ on purpose. The constructor can
             * publish the requested level and then fail its read-back, which leaves the path
             * modified while the level was never raised; that state has to reach the work below
             * rather than this early return, or the one path that most needs verified
             * restoration would be the one that skipped it.
             */
            return true;
        }

        /* 1. Publication, and 2's precondition: the reader is driven with the bytes in place. */
        const char *const publicationFailure = restoreLevelAndPath();

        if (publicationFailure != nullptr) {
            detail = std::string("could not put ") + kLogConfigPath + " back: "
                + publicationFailure + " (errno " + std::to_string(restoreErrno_) + ": "
                + std::strerror(restoreErrno_) + "). The custody lock is deliberately still"
                  " held and the atexit backstop still armed, so the destructor will try again;"
                  " until one of them succeeds this host carries this run's raised log level";
            return false;
        }

        if (!verifyRestoredState(detail)) {
            return false;
        }

        /*
         * proved, so custody can go. The active pointer is cleared first, so the atexit
         * backstop finds nothing to do even if the release below is the last thing that
         * happens to this object.
         */
        raised_ = false;
        restorePending_ = false;
        restored_ = true;
        if (sActive_ == this) {
            sActive_ = nullptr;
        }
        releaseLock();
        return true;
    }

private:
    /**
     * @brief Proves the path and the process-wide level are what this guard found.
     *
     * The second and third of restoreAndVerify()'s three checks, factored out because the
     * constructor's rollback path has to run exactly the same ones. A rollback that is
     * published but not verified is the defect this function exists to make impossible to
     * reintroduce: there would then be two verification implementations and only one of them
     * would be exercised by the suite.
     *
     * @param [out] detail - Empty when both hold; otherwise which one failed and what was
     *                       observed.
     *
     * @return bool - True when the bytes (or the absence) and the effective level both match
     *         what this guard captured when it took custody.
     *
     * @pre The custody lock is held and restoreLevelAndPath() has just reported success.
     */
    bool verifyRestoredState(std::string &detail)
    {
        /* 1. Content where the file existed, absence where it did not. */
        if (existed_) {
            if (!pathHolds(saved_, savedLength_)) {
                detail = std::string(kLogConfigPath)
                    + " does not hold the bytes this guard captured, although the replace"
                      " reported success, so a writer outside the custody lock has replaced it"
                      " since. The host's own configuration is NOT back, and what stands at"
                      " that path is not this run's to correct";
                return false;
            }
        } else if (!pathIsAbsent()) {
            detail = std::string(kLogConfigPath)
                + " is still present, although this guard created it and has just removed it,"
                  " so either the removal did not take effect or another writer has recreated"
                  " it. A file the host did not have is left behind, and every later process"
                  " reading it will take this run's level for the host's";
            return false;
        }

        /* 2. The level itself - the check a restored file cannot stand in for. */
        const int observedLevel = probeEffectiveLevel();

        if (observedLevel != entryLevel_) {
            detail = std::string("the process-wide CEC log level is ")
                + std::to_string(observedLevel) + " but was " + std::to_string(entryLevel_)
                + " when this guard took custody, so " + kLogConfigPath
                + " was put back without the level following it. Every later case in this"
                  " binary would run at the wrong verbosity, and a case asserting on a"
                  " suppressed line would pass or fail for a reason that is not its own";
            return false;
        }

        return true;
    }

public:

    /**
     * @brief Best-effort backstop for the exit that never reaches restoreAndVerify().
     *
     * It exists for the fatal GoogleTest assertion, the thrown exception and the `exit()` that
     * unwind past the explicit call, and it is deliberately not the route a case relies on: a
     * destructor cannot fail a test, so a suite whose only restoration is a destructor reports
     * success for a run that left the host altered - a raised level still in force, or the
     * shared configuration file changed or missing - which is precisely the shape
     * restoreAndVerify() exists to close. Every case that raises the level must call that
     * instead and assert on what it returns; this is what runs when the case could not.
     *
     * What it can still do, it does: the same single restoration primitive, which names the
     * entry level back through production's reader and then puts the file back byte-for-byte or
     * removes it again. What it cannot do is tell the test, so a failure is reported on stdout
     * instead - naming the path, the step and the errno - because an unreported failure here is
     * indistinguishable from a clean exit, and the run log is the only reader left.
     *
     * It also performs no verification: the read-back and the level re-probe both allocate, and
     * a destructor reached by unwinding is not a place to start allocating, nor is an atexit
     * handler during teardown. Verification belongs to the checked path, which is another
     * reason the backstop cannot substitute for it.
     */
    ~ScopedCecLogLevel()
    {
        /*
         * Two states reach here, and both mean the same thing to a backstop: this object has
         * published something to the shared path that has not been proved taken back. raised_
         * is the ordinary one - a case that never called restoreAndVerify(), or whose fatal
         * assertion skipped it. restorePending_ is the constructor's read-back failure, where
         * the level was never raised but the path was written and the rollback could not be
         * proved; that path deliberately keeps custody so this retry can happen at all.
         */
        if (raised_ || restorePending_) {
            const char *const publicationFailure = restoreLevelAndPath();
            std::string verificationDetail;

            if (publicationFailure != nullptr) {
                reportBackstopFailure(publicationFailure);
            } else if (!verifyRestoredState(verificationDetail)) {
                /*
                 * Published but not proved. Reported rather than assumed, because a destructor
                 * cannot fail a test and an unreported failure here is a host left altered
                 * with nothing in the log to say so.
                 */
                reportBackstopFailure("the restoration could not be verified");
            }
            raised_ = false;
            restorePending_ = false;
        }
        if (sActive_ == this) {
            sActive_ = nullptr;
        }
        /*
         * Released unconditionally, and last. A lock kept past this point would outlive the
         * object that justified holding it and would refuse custody to every later case in
         * this binary and every concurrent run_L1Tests on the host - a worse outcome than the
         * one the retry above already reported.
         */
        releaseLock();
    }

    /**
     * @brief Whether the level was really moved to the name this guard was given.
     *
     * True only when custody was taken, the effective level observed so restoration has a
     * reference, the name written atomically, the written bytes read back from the path, and
     * production's reader driven with them in place. False is a refusal, and failureReason()
     * says which one.
     *
     * Becomes false again once restoreAndVerify() has proved the level back, because from that
     * point this guard no longer holds anything raised.
     *
     * @return bool - true while a raise this guard performed is still in force and proved,
     *                false when the raise was refused or has been proved taken back.
     */
    bool isRaised() const { return raised_; }

    /**
     * @brief Why isRaised() is false, in a form a failure message can stream.
     *
     * Empty while isRaised() is true. Otherwise it names the specific refusal - contended
     * lock, symlink or foreign owner at the path, unreadable or oversized file, failed atomic
     * replace, or a concurrent writer - so an environment fault and a custody refusal are
     * distinguishable in the log rather than collapsed into one diagnosis.
     *
     * @return const std::string& - The refusal, or an empty string while isRaised() is true.
     */
    const std::string &failureReason() const { return failureReason_; }

    /**
     * @brief The effective level this guard found when it took custody.
     *
     * Exposed so a case can assert that the level moved and then came back, rather than
     * taking the guard's word for either. It is the value restoreAndVerify() compares
     * against, so a case that checks the same number is checking the same claim.
     *
     * @return int - A level in `0 .. LOG_MAX - 1`, or kUnobservableLevel() when the level
     *         could not be observed, in which case the guard refused to raise at all.
     */
    int entryLevel() const { return entryLevel_; }

    /**
     * @brief Observes the process-wide effective level, for a case that wants to check it.
     *
     * Public because the evidence is worth more than the encapsulation. `cec_log_level` is
     * a file-static in `ccec/src/Util.cpp` with no accessor, so the only way to observe it
     * is to emit at each level and see which emissions survive - and a case that cannot do
     * that cannot tell "the guard restored the level" from "the guard said it did".
     *
     * @return int - The observed level, or kUnobservableLevel() when nothing was observed.
     *
     * @warning Emits to stdout, so a caller inside a StdoutCapture will find this probe's
     *          own markers in the captured text. Call it outside any capture scope.
     */
    static int observeEffectiveLevel() { return probeEffectiveLevel(); }

    /**
     * @brief The sentinel observeEffectiveLevel() returns when no level emitted at all.
     *
     * @return int - The unobservable-level sentinel, distinct from every real level.
     */
    static int unobservableLevel() { return kUnobservableLevel; }

    /** @brief Not copyable: custody of the shared path belongs to one instance at a time. */
    ScopedCecLogLevel(const ScopedCecLogLevel &) = delete;

    /** @brief Not assignable, for the same reason the copy constructor is deleted. */
    ScopedCecLogLevel &operator=(const ScopedCecLogLevel &) = delete;

private:
    /** @brief The path production's check_cec_log_status() hardcodes. Not this file's choice. */
    static constexpr const char *kLogConfigPath = "/tmp/cec_log_enabled";

    /**
     * @brief The companion lock path's suffix, so the lock name is derived, never re-typed.
     *
     * `tests/L1Tests/ccec/test_Util.cpp` arrives at the same string the same way - it appends
     * ".testlock" to its own copy of the configuration path rather than writing the lock name
     * out - so the two units lock one file. Deriving it on both sides is what keeps that true:
     * a hand-typed whole lock name is one edit away from two "exclusive" holders of two
     * different files, which is worse than no lock at all because it looks like one.
     */
    static constexpr const char *kLockSuffix = ".testlock";

    /**
     * @brief The level names production's reader matches, indexed by the level they select.
     *
     * A transcription of production's own table (`ccec/src/Util.cpp:58-67`), in the same order,
     * where the index is the level - which production relies on too, since it stores the
     * table's second column through `atoi` (`:93`). It is needed because restoration has to
     * name the level it wants back: `check_cec_log_status()` compares the configuration file's
     * first line against these names and silently leaves `cec_log_level` alone when none
     * matches (`:87-97`), so publishing the captured original is not by itself a way of asking
     * for a particular level - only a name from this table is.
     *
     * `tests/L1Tests/ccec/test_Util.cpp:118-127` carries the same transcription for the same
     * reason. Both are transcriptions rather than shared code because production's table has
     * internal linkage and exposes no accessor.
     */
    static constexpr const char *kLevelNames[] = {
        "FATAL", "ERROR", "WARN", "EXP", "NOTICE", "INFO", "DEBUG", "TRACE"
    };

    static_assert(sizeof(kLevelNames) / sizeof(kLevelNames[0]) == LOG_MAX,
                  "the transcribed level table and production's LOG_MAX have diverged, so a"
                  " level this guard restores to could name nothing production recognises");

    /**
     * @brief The name production's reader matches for @p level, or nullptr if it has none.
     *
     * @param [in] level - A level value, not necessarily in range.
     * @return const char* - The table entry, or nullptr when @p level is outside
     *         `0 .. LOG_MAX - 1` and therefore cannot be asked for by name at all.
     */
    static const char *levelName(int level) {
        if (level < 0 || level >= LOG_MAX) {
            return nullptr;
        }
        return kLevelNames[level];
    }

    /**
     * @brief What probeEffectiveLevel() reports when no level produced output at all.
     *
     * A value outside `0 .. LOG_MAX - 1`, so it can never be mistaken for a level. It means
     * the effective level could not be observed, which is a refusal rather than a level: the
     * constructor declines to raise on it, because a raise whose restoration cannot be proved
     * is exactly the shape this class exists to close.
     */
    static constexpr int kUnobservableLevel = -1;

    /**
     * @brief Observes the process-wide effective level, for which production offers no getter.
     *
     * `cec_log_level` is a file-static in `ccec/src/Util.cpp:39` with no accessor, and
     * `CCEC_LOG` emits only when its level is at or below that static
     * (`ccec/src/Util.cpp:116`). So the highest level that still emits is the configured
     * level, and walking down from `LOG_MAX - 1` until something appears reads it without any
     * production change. The technique is the repository's own - `probeEffectiveLogLevel()` in
     * `tests/L1Tests/ccec/test_Util.cpp:584-595` observes the same static the same way, for
     * the same reason - and it is reimplemented here rather than shared, because that unit's
     * helpers are file-local to it and this file must not include another test translation
     * unit.
     *
     * Each emission goes into its own StdoutCapture, so the probe writes nothing to the
     * process log: a probe that leaked its markers would pollute the very output the cases in
     * this file assert against, and `run_coverage.sh` greps that same log for the
     * selected-path line.
     *
     * The marker carries the level and the pid, so a line left behind by any other writer
     * cannot be mistaken for this probe's own emission.
     *
     * @return int - The observed level in `0 .. LOG_MAX - 1`, or kUnobservableLevel when no
     *         level emitted and when stdout could not be captured, the two cases in which
     *         nothing was observed and so nothing may be concluded.
     */
    static int probeEffectiveLevel() {
        for (int level = LOG_MAX - 1; level >= 0; level--) {
            char marker[64];

            const int formatted = std::snprintf(marker, sizeof(marker),
                                                "AIDL_LEVEL_PROBE_%d_%ld", level,
                                                static_cast<long>(::getpid()));

            if (formatted <= 0 || static_cast<size_t>(formatted) >= sizeof(marker)) {
                return kUnobservableLevel;
            }

            std::string emitted;
            {
                StdoutCapture capture;

                if (!capture.isValid()) {
                    return kUnobservableLevel;
                }

                CCEC_LOG(level, "%s", marker);
                emitted = capture.read();
            }

            if (emitted.find(marker) != std::string::npos) {
                return level;
            }
        }

        return kUnobservableLevel;
    }

    /** @brief Retry budget for the custody lock: 500 attempts at 10 ms, so 5 s in total. */
    static constexpr int kLockAttempts = 500;

    /** @brief Interval between lock attempts, in nanoseconds. */
    static constexpr long kLockRetryNs = 10L * 1000L * 1000L;

    /**
     * @brief The custody paths, built once into static storage.
     *
     * Static and pre-formatted because the restoration path must not allocate: it also runs
     * from the `std::atexit` backstop below, where formatting a path through `std::string`
     * would mean allocating during process teardown. The temporary's name is deliberately
     * distinct from the one `test_Util.cpp` uses, so that even a backstop running in an odd
     * order cannot have the two units writing one temporary.
     */
    static inline char sLockPath_[288] = { '\0' };
    static inline char sTempPath_[288] = { '\0' };

    /** @brief The instance currently holding custody, for the atexit backstop. */
    static inline ScopedCecLogLevel *sActive_ = nullptr;

    /** @brief Whether the backstop has been registered; registration happens exactly once. */
    static inline bool sAtExitRegistered_ = false;

    /**
     * @brief Formats the lock and temporary paths from kLogConfigPath, once per process.
     *
     * @return bool - Whether both custody paths are usable
     * @retval true  - Both hold a complete, non-truncated string.
     * @retval false - Formatting overflowed one of the buffers. Both are left empty, so
     *                 no later step can act on a half-formatted path.
     */
    static bool buildCustodyPaths() {
        if (sLockPath_[0] != '\0' && sTempPath_[0] != '\0') {
            return true;
        }

        const int lockLength = std::snprintf(sLockPath_, sizeof(sLockPath_), "%s%s",
                                             kLogConfigPath, kLockSuffix);
        const int tempLength = std::snprintf(sTempPath_, sizeof(sTempPath_), "%s.aidltest.%ld.tmp",
                                             kLogConfigPath, static_cast<long>(::getpid()));

        if (lockLength <= 0 || static_cast<size_t>(lockLength) >= sizeof(sLockPath_)
            || tempLength <= 0 || static_cast<size_t>(tempLength) >= sizeof(sTempPath_)) {
            sLockPath_[0] = '\0';
            sTempPath_[0] = '\0';
            return false;
        }
        return true;
    }

    /**
     * @brief Takes the exclusive advisory lock over the companion path.
     *
     * The lock is over a companion path and not over the configuration file itself: locking
     * the file would mean holding a descriptor to it across the rename() that replaces it, at
     * which point the lock is on the replaced inode and excludes nobody. `flock()` rather than
     * `fcntl()` because the lock is wanted for the lifetime of the descriptor with no byte
     * ranges and is dropped automatically if this process dies. `LOCK_NB` with a bounded
     * retry, because blocking for ever here would turn a contended host into a suite that
     * hangs instead of one that reports.
      *
     * @return bool - Whether custody was taken
     * @retval true  - The lock is held and `lockFd_` carries the locked descriptor.
     * @retval false - failureReason() carries why: either the open refused the path, or
     *                 the bounded retry expired against another holder.
     */
    bool acquireLock() {
        const int fd = ::open(sLockPath_, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);

        if (fd < 0) {
            failureReason_ = std::string("could not open the custody lock ") + sLockPath_ + ": "
                + std::strerror(errno)
                + " (a symlink at that path is refused, which is what O_NOFOLLOW reports as"
                  " ELOOP)";
            return false;
        }

        for (int attempt = 0; attempt < kLockAttempts; attempt++) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                lockFd_ = fd;
                return true;
            }
            if (errno != EWOULDBLOCK) {
                failureReason_ = std::string("could not lock ") + sLockPath_ + ": "
                    + std::strerror(errno);
                ::close(fd);
                return false;
            }

            struct timespec pause;
            pause.tv_sec = 0;
            pause.tv_nsec = kLockRetryNs;
            while (::nanosleep(&pause, &pause) != 0 && errno == EINTR) {
                /* finish the remaining interval nanosleep wrote back */
            }
        }

        failureReason_ = std::string("another holder has kept ") + sLockPath_ + " for over "
            + std::to_string((kLockAttempts * (kLockRetryNs / 1000000L)) / 1000L)
            + " s, so " + kLogConfigPath + " is not this case's to modify. The holder is a"
              " second run_L1Tests on this host, this binary's own Util suite, or another guard"
              " still alive in this process - all three contend for that production-fixed path,"
              " and the first is fixed by running the copies one at a time";
        ::close(fd);
        return false;
    }

    /** @brief Releases the lock, if held. The lock file is never unlinked - see below. */
    void releaseLock() {
        if (lockFd_ < 0) {
            return;
        }
        /*
         * Deliberately not unlinked: another waiter may already hold a descriptor to it, and
         * removing it would let a third process create a new inode and lock that instead -
         * two "exclusive" holders of two different files.
         */
        (void)::flock(lockFd_, LOCK_UN);
        ::close(lockFd_);
        lockFd_ = -1;
    }

    /**
     * @brief Classifies the configuration path and captures it, without following a link.
     *
     * Refuses anything that is not a regular file this user owns, and confirms through
     * `fstat()` on the opened descriptor that the inode read is the one `lstat()` classified -
     * which is what closes the window between the two calls (CWE-367) rather than merely
     * narrowing it. A file larger than the capture buffer is refused outright: a truncated
     * copy could not be restored faithfully, and leaving the host with a truncated
     * configuration would be worse than not touching it at all.
      *
     * @return bool - Whether the path was classified and captured
     * @retval true  - Either the original bytes, mode and ownership are captured, or the
     *                 path is legitimately absent and `existed_` records that.
     * @retval false - The path was refused; failureReason() carries which check refused it.
     */
    bool captureCurrentState() {
        struct stat linkStatus;

        if (::lstat(kLogConfigPath, &linkStatus) != 0) {
            if (errno == ENOENT) {
                existed_ = false;
                return true;
            }
            failureReason_ = std::string("could not inspect ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }

        if (!S_ISREG(linkStatus.st_mode)) {
            failureReason_ = std::string(kLogConfigPath)
                + " exists and is NOT a regular file (mode "
                + std::to_string(static_cast<unsigned long>(linkStatus.st_mode))
                + "), which at a fixed name in a world-writable directory is very likely a"
                  " planted link; refusing to modify it, because replacing or writing through"
                  " it would alter whatever it refers to";
            return false;
        }

        if (linkStatus.st_uid != ::geteuid() && ::geteuid() != 0) {
            failureReason_ = std::string(kLogConfigPath) + " is owned by uid "
                + std::to_string(static_cast<unsigned long>(linkStatus.st_uid))
                + " and this process is not that user; refusing to modify it";
            return false;
        }

        const int fd = ::open(kLogConfigPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

        if (fd < 0) {
            failureReason_ = std::string("could not read ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }

        struct stat openStatus;

        if (::fstat(fd, &openStatus) != 0 || openStatus.st_dev != linkStatus.st_dev
            || openStatus.st_ino != linkStatus.st_ino) {
            ::close(fd);
            failureReason_ = std::string(kLogConfigPath)
                + " was replaced between being classified and being opened, so what would be"
                  " captured is not what was validated; refusing to modify it";
            return false;
        }

        ssize_t got = 0;
        size_t total = 0;

        while (total < sizeof(saved_)
               && (got = ::read(fd, saved_ + total, sizeof(saved_) - total)) > 0) {
            total += static_cast<size_t>(got);
        }

        const bool readFailed = (got < 0);
        char overflow = '\0';
        const bool tooLong = (total == sizeof(saved_)) && (::read(fd, &overflow, 1) > 0);

        ::close(fd);

        if (readFailed) {
            failureReason_ = std::string("could not read ") + kLogConfigPath + ": "
                + std::strerror(errno);
            return false;
        }
        if (tooLong) {
            failureReason_ = std::string(kLogConfigPath) + " is larger than "
                + std::to_string(sizeof(saved_))
                + " bytes, so it could not be restored byte for byte; refusing to modify it";
            return false;
        }

        savedLength_ = total;
        savedMode_ = linkStatus.st_mode & 07777;
        savedUid_ = linkStatus.st_uid;
        savedGid_ = linkStatus.st_gid;
        existed_ = true;
        return true;
    }

    /**
     * @brief Publishes @p length bytes at the configuration path, atomically and link-safely.
     *
     * Allocation-free by construction - only open/write/fchmod/fchown/close/rename/unlink -
     * because the restoration path calls it, and that path also runs from the atexit backstop.
     * The temporary is created `O_EXCL|O_NOFOLLOW` in the same directory so the rename is a
     * same-filesystem atomic replace; a planted link at the destination is therefore replaced
     * rather than written through.
     *
     * @return int - 0 on success, -1 with the path unchanged otherwise.
      *
     * @param [in] data   First byte of the content to publish. Not required to be
     *                   NUL-terminated; exactly @p length bytes are written.
     * @param [in] length Number of bytes to publish.
     *
     * @return int - Publication result
     * @retval 0  - The rename has completed and the path holds the new content.
     * @retval -1 - `errno` is set by the failing syscall, the temporary is unlinked, and
     *              the destination is left exactly as it was.
     */
    int atomicWrite(const char *data, size_t length) {
        if (sTempPath_[0] == '\0') {
            return -1;
        }

        (void)::unlink(sTempPath_);

        const int fd = ::open(sTempPath_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                              0600);

        if (fd < 0) {
            return -1;
        }

        size_t written = 0;

        while (written < length) {
            const ssize_t chunk = ::write(fd, data + written, length - written);

            if (chunk <= 0) {
                if (chunk < 0 && errno == EINTR) {
                    continue;
                }
                ::close(fd);
                (void)::unlink(sTempPath_);
                return -1;
            }
            written += static_cast<size_t>(chunk);
        }

        /*
         * Hand back the metadata the file was found with. The two calls are not equivalent and
         * are not treated as such:
         *
         *   fchmod - this descriptor was created by this process with O_EXCL, so the file is
         *            this process's and fchmod to any mode must succeed. A failure means the
         *            restored file would carry permissions the host did not have before, which
         *            is exactly the silent host mutation this guard exists to prevent.
         *   fchown - genuinely best effort. An unprivileged process cannot give a file away,
         *            so EPERM is the normal outcome whenever the file was owned by somebody
         *            else, and failing on it would abort every unprivileged run for a
         *            condition no test can influence.
         */
        if (::fchmod(fd, savedMode_) != 0) {
            ::close(fd);
            (void)::unlink(sTempPath_);
            return -1;
        }
        if (savedUid_ != static_cast<uid_t>(-1)) {
            /*
             * The result is taken into a named local and then discarded rather than cast away
             * at the call: glibc declares fchown() with warn_unused_result, and a bare
             * `(void)` cast on the call expression does not satisfy that attribute under
             * g++ -Wall, so writing it that way costs a -Wunused-result warning in a build
             * this suite requires to be warning-clean. Discarding a named local does satisfy
             * it, and it also makes the decision visible: the value is examined by a human
             * reading this comment rather than by code, because per the note above there is
             * no remedy an unprivileged process could apply to EPERM here.
             */
            const int ownershipResult = ::fchown(fd, savedUid_, savedGid_);
            (void)ownershipResult;
        }
        if (::close(fd) != 0) {
            (void)::unlink(sTempPath_);
            return -1;
        }
        if (::rename(sTempPath_, kLogConfigPath) != 0) {
            (void)::unlink(sTempPath_);
            return -1;
        }
        return 0;
    }

    /**
     * @brief Whether the path currently holds exactly @p length bytes equal to @p data.
     *
     * Read with `O_NOFOLLOW`, so a link planted at the name is refused rather than followed,
     * and read one byte past what is being compared: a file that starts with @p data and
     * continues is not the file that was published, so the comparison has to be able to
     * observe the extra byte rather than stop at the expected length.
     *
     * The buffer is one byte larger than the capture buffer because both publications this
     * guard performs are verified through here - the short level line on the way in and the
     * captured original, up to `sizeof(saved_)` bytes, on the way out. A buffer sized only for
     * the level line would have reported every longer original as a mismatch, which is a
     * false restoration failure on any host whose configuration file is not a short line.
      *
     * @param [in] data   Expected content, compared byte for byte.
     * @param [in] length Expected length in bytes.
     *
     * @return bool - Whether the path holds exactly that content
     * @retval true  - The path is a regular file of exactly @p length bytes equal to
     *                 @p data.
     * @retval false - Any other content, a longer file, a link standing at the name, or
     *                 an unreadable path.
     */
    bool pathHolds(const char *data, size_t length) const {
        char actual[sizeof(saved_) + 1];

        if (length >= sizeof(actual)) {
            return false;
        }

        const int fd = ::open(kLogConfigPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

        if (fd < 0) {
            return false;
        }

        ssize_t got = 0;
        size_t total = 0;

        while (total < sizeof(actual)
               && (got = ::read(fd, actual + total, sizeof(actual) - total)) > 0) {
            total += static_cast<size_t>(got);
        }
        ::close(fd);

        return (got >= 0) && (total == length) && (std::memcmp(actual, data, length) == 0);
    }

    /**
     * @brief The one restoration primitive: the level first, then the file.
     *
     * Used by the checked path in restoreAndVerify() and by both backstops, so that there is
     * exactly one implementation of what "put it back" means and no route can diverge from it.
     *
     * The order is the whole point, and it is the repository's own - `UtilTest::TearDown()` in
     * `tests/L1Tests/ccec/test_Util.cpp:636-653` restores the level and then the file's bytes,
     * in that order, for exactly this reason. Asking for a level means writing the
     * configuration file, so:
     *
     *   1. The entry level is named back, by publishing the name production's reader matches
     *      for it and driving that reader. This is what actually moves `cec_log_level`, and it
     *      is not interchangeable with publishing the captured original: the original need not
     *      name the entry level at all - an empty file, or one whose first line matches nothing
     *      in production's table, leaves the reader silently holding the raised level
     *      (`ccec/src/Util.cpp:87-97`), and a host with such a file would be left at DEBUG by a
     *      restoration that had faithfully reproduced its bytes.
     *   2. The captured original is published, or the file this guard created is removed, and
     *      the reader is not driven again afterwards - so the file ends as the host had it while
     *      the level stays where step 1 put it. Driving the reader after step 2 would undo it.
     *
     * Every outcome is returned rather than discarded, which is the whole reason this returns
     * anything: a failed write, a failed rename or a failed unlink leaves the host altered -
     * the raised level still in force, or the shared file changed or missing - and a primitive
     * that swallowed them would let that happen while every case in the binary still passed.
     *
     * Allocation-free, because the atexit backstop calls it during process teardown: the level
     * line is formatted into a stack buffer and every description returned is a static literal.
     *
     * @return const char* - nullptr when the level was named back and the file published,
     *         otherwise a static description of the step that failed, with restoreErrno_
     *         carrying the errno that step saw so a report can name the fault without
     *         re-reading an errno the later steps have overwritten.
     */
    const char *restoreLevelAndPath() {
        restoreErrno_ = 0;

        /* 1. The level, by name, through production's own reader. */
        const char *const name = levelName(entryLevel_);

        if (name == nullptr) {
            return "the level observed when custody was taken has no name in production's own"
                   " table, so it cannot be asked for through check_cec_log_status() - the"
                   " transcribed table and LOG_MAX have diverged";
        }

        char line[64];
        const int formatted = std::snprintf(line, sizeof(line), "%s\n", name);

        if (formatted <= 0 || static_cast<size_t>(formatted) >= sizeof(line)) {
            return "the entry level's name does not fit the configuration line this guard"
                   " writes, so the level could not be asked for";
        }

        if (atomicWrite(line, static_cast<size_t>(formatted)) != 0) {
            restoreErrno_ = errno;
            return "the entry level could not be published, so the raised level is still in"
                   " force process wide";
        }

        check_cec_log_status();

        /* 2. The file, without driving the reader again. */
        if (existed_) {
            if (atomicWrite(saved_, savedLength_) != 0) {
                restoreErrno_ = errno;
                return "the level went back but the captured original could not be written"
                       " back atomically, so the path still holds a line this guard wrote";
            }
            return nullptr;
        }

        if (::unlink(kLogConfigPath) != 0 && errno != ENOENT) {
            restoreErrno_ = errno;
            return "the level went back but the configuration file this guard created could"
                   " not be removed again, so a file the host did not have is left behind";
        }
        return nullptr;
    }

    /**
     * @brief Whether the configuration path is absent, classified without following a link.
     *
     * `lstat` rather than `stat`, for the same reason every other inspection in this class uses
     * it: a link standing at the name must be reported as something that is there, not as the
     * absence of its target.
      *
     * @return bool - Whether the path is empty
     * @retval true  - Nothing at all stands at the path.
     * @retval false - Something does, including a link whose target is missing.
     */
    static bool pathIsAbsent() {
        struct stat linkStatus;

        return ::lstat(kLogConfigPath, &linkStatus) != 0 && errno == ENOENT;
    }

    /**
     * @brief Reports a failed backstop restoration on stdout, without allocating.
     *
     * The backstops run from a destructor and from an atexit handler, neither of which can fail
     * a test: GoogleTest's assertion macros need a live test to attribute a failure to, and by
     * the time an atexit handler runs there is none. So the only report available is the run
     * log, and this is it - written in the same voice as the rest of this file, naming the path,
     * the step and the errno, so that a reader of a run that left the host altered sees it said
     * rather than having to infer it from a later case's verbosity.
     *
     * @param [in] failure - The static step description restoreLevelAndPath() returned.
     */
    void reportBackstopFailure(const char *failure) const {
        std::fflush(stdout);
        std::fprintf(stdout,
                     "\r\n[ScopedCecLogLevel] BACKSTOP RESTORATION FAILED for %s: %s (errno"
                     " %d: %s). The process-wide CEC log level was not proved to have returned"
                     " to the value this run found, and this line is the only report there is -"
                     " a destructor cannot fail a test. Delete or repair that path before"
                     " reading any later log in this run as the host's own.\r\n",
                     kLogConfigPath, failure, restoreErrno_, std::strerror(restoreErrno_));
        std::fflush(stdout);
    }

    /** @brief Registers the backstop exactly once per process. */
    void registerAtExitOnce() {
        if (sAtExitRegistered_) {
            return;
        }
        (void)std::atexit(&ScopedCecLogLevel::atExitRestore);
        sAtExitRegistered_ = true;
    }

    /**
     * @brief Last-chance restoration on ordinary process exit, and the second backstop.
     *
     * atexit handlers run on return from main() and on exit(), which is normal control flow -
     * unlike a signal handler, this may safely touch object state and the filesystem. It
     * exists for the exit that never reaches the destructor; a live sActive_ here means an
     * instance still holds custody, since both the destructor and restoreAndVerify() clear it.
     * No signal handler is installed, deliberately: a fatal signal arriving mid-rename would
     * give two racing writers over one temporary, and corrupting the file this guard protects
     * is worse than leaving a debug-logging switch at a stale value that a human can delete.
     *
     * Best effort, for the same reason the destructor is: there is no live test to fail by the
     * time this runs, so a failure is reported on stdout and nowhere else. It is not a
     * substitute for restoreAndVerify(), which is the only route that can make a case fail.
     */
    static void atExitRestore() {
        ScopedCecLogLevel *const active = sActive_;

        if (active == nullptr) {
            return;
        }
        /*
         * Both states, exactly as the destructor treats them: raised_ is the ordinary skipped
         * restoration, restorePending_ the constructor's unproved rollback. An atexit hook that
         * looked only at raised_ would walk past the one state whose whole reason for keeping
         * custody was to have a backstop retry it.
         */
        if (active->raised_ || active->restorePending_) {
            const char *const publicationFailure = active->restoreLevelAndPath();
            std::string verificationDetail;

            if (publicationFailure != nullptr) {
                active->reportBackstopFailure(publicationFailure);
            } else if (!active->verifyRestoredState(verificationDetail)) {
                active->reportBackstopFailure("the restoration could not be verified");
            }
            active->raised_ = false;
            active->restorePending_ = false;
        }
        sActive_ = nullptr;
        active->releaseLock();
    }

    bool raised_;

    /**
     * @brief Whether this guard has published something to the path that is not proved back.
     *
     * Deliberately independent of raised_. The constructor can write the requested level and
     * then fail its read-back, which leaves the path modified while the level was never
     * raised - a state raised_ cannot express, and the one state in which abandoning custody
     * would leave the shared host path altered with every backstop disarmed.
     */
    bool restorePending_;

    /** @brief Whether restoreAndVerify() has already restored, which is what makes it idempotent. */
    bool restored_;

    bool existed_;
    int lockFd_;
    size_t savedLength_;
    mode_t savedMode_;
    uid_t savedUid_;
    gid_t savedGid_;

    /**
     * @brief The effective level observed at construction, which restoration must return to.
     *
     * kUnobservableLevel only while this guard refused to raise; a raised guard always holds a
     * level in `0 .. LOG_MAX - 1` here, because the constructor declines the raise otherwise.
     */
    int entryLevel_;

    /**
     * @brief errno as it stood at the failing restoration step, or 0 when none failed.
     *
     * Captured at the step rather than read at the report, because `check_cec_log_status()` and
     * the unlink that follows it both overwrite errno; a value read later would name the wrong
     * fault. Held in an int so the backstop can report it with no allocation.
     */
    int restoreErrno_;

    /** @brief Why custody was refused or restoration failed; empty while nothing has failed. */
    std::string failureReason_;

    /**
     * @brief The configuration file's original bytes, captured verbatim.
     *
     * A fixed buffer rather than a std::string because the restoration path must not
     * allocate: it also runs from the atexit backstop. A file larger than this is refused
     * outright rather than captured short - see captureCurrentState().
     */
    char saved_[4096];
};

/**
 * @brief Renders bytes exactly as DriverAidlImpl::EventListener::onMessageSent logs them.
 *
 * Lowercase hex, two digits per byte, no separator - the rendering
 * DriverAidlImpl::EventListener::onMessageSent() performs. Kept here rather than written out at
 * each call site so that a case asserting on the production line compares against one rendering
 * rather than against a hand-typed literal that could drift from the message it was built from.
 *
 * One deliberate difference, so that this helper is not mistaken for a second implementation of
 * the production rendering: production renders into a stack buffer sized from
 * CECFrame::MAX_LENGTH and appends an ellipsis to a message longer than that, whereas this
 * helper is unbounded. It is only ever called with the short frames the cases below construct,
 * which are two orders of magnitude inside that bound, so the two renderings coincide; a case
 * that ever needed the truncating arm would have to assert against the bounded form instead,
 * and production refuses to send such a frame in the first place (the frame-length policing
 * asserted by DriverAidlTransmitTest), so only a misbehaving HAL could echo one back.
 *
 * @param [in] bytes Message bytes to render, in order.
 *
 * @return std::string - The concatenated two-digit lowercase hex of every byte, in
 *                       order, and empty for an empty input.
 */
std::string lowercaseHexOf(const std::vector<uint8_t> &bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string rendered;

    rendered.reserve(bytes.size() * 2);
    for (size_t index = 0; index < bytes.size(); index++) {
        rendered.push_back(digits[(bytes[index] >> 4) & 0x0F]);
        rendered.push_back(digits[bytes[index] & 0x0F]);
    }

    return rendered;
}

/*
 * ---------------------------------------------------------------------------------
 * the synthetic binder preflight probe, and why the predicate takes one.
 *
 * Three of the preflight's five decision points cannot be reached from a path and a
 * timeout on any host: the protocol-version comparison needs a node that answers
 * BINDER_VERSION with a value other than the one this build was compiled against, the
 * context-manager arm needs a working driver whose context manager never registered, and
 * the positive verdict needs a binder-capable kernel. A regular file reaches the ioctl arm
 * before all three, and an absent path reaches the open arm before that, so no amount of
 * filesystem arrangement gets past decision point 3.
 *
 * DriverAidlImpl::isBinderPreflightOk() therefore takes its six kernel-facing operations -
 * openNode, identifyDescriptor, identifyPath, readProtocolVersion, pingContextManager and
 * closeNode - as a BinderPreflightProbe of plain function pointers, defaulted to
 * defaultBinderProbe(). The same probe serves the pre-lookup custody re-verification, which
 * is why identifyPath is among them: the re-verification resolves the NAME again, where the
 * preflight asks its questions of the DESCRIPTOR it already holds.
 * Substituting them here is what makes every arm reachable on this host, deterministically
 * and without a binder driver - and it is also what lets the cases assert descriptor
 * hygiene, which no black-box test of the predicate could observe at all.
 *
 * The state is file-scope because the probe members are plain function pointers: they
 * carry no captured context, so a stateful lambda cannot convert to one and the
 * configuration and the counters have to live somewhere the free functions can reach. That
 * is not a shared-state order dependency: GoogleTest runs cases on one thread, every case
 * below calls resetSyntheticProbe() before it configures anything, and nothing outside
 * DriverAidlPreflightTest touches this state. The production selection never sees it -
 * every production call site takes the default argument, which is defaultBinderProbe().
 * ---------------------------------------------------------------------------------
 */

/**
 * @brief The descriptor the synthetic probe hands back for a successful open
 *
 * A value no real descriptor in this process can collide with, so that "the descriptor
 * that was closed is the one that was opened" cannot be satisfied by accident. Nothing
 * ever performs a real operation on it: all four probe members are substituted, so this
 * number is only ever passed between them.
 */
constexpr int kSyntheticBinderFd = 4242;

/**
 * @brief The descriptor the synthetic probe hands back for the SECOND open in one call
 *
 * isServiceAvailable() opens the node twice - once through the preflight, whose descriptor
 * it retains, and once for the pre-lookup liveness check, which cannot go over the retained
 * descriptor because the binder driver permits one mapping per open descriptor. A distinct
 * number for the second open is what lets a case assert WHICH descriptor was pinged and
 * WHICH was released, rather than counting anonymous calls.
 */
constexpr int kSyntheticSecondBinderFd = 4343;

/**
 * @brief The identity of a well-formed binder node: a root-owned character device
 *
 * The values of @c device, @c inode and @c rdev are arbitrary but FIXED, because what the
 * production code does with them is compare them - so what matters is that the "same node"
 * and "different node" answers differ in exactly the field under test. The mode and owner
 * are not arbitrary: they are what the preflight validates, and they are built from the
 * POSIX macros here rather than from the production constants so that the two are
 * independent and a wrong constant cannot agree with itself.
 */
const DriverAidlImpl::BinderNodeIdentity kValidatedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0600),
    0u,
};

/**
 * @brief The same node's identity with ONE field changed: a substituted node
 *
 * Only @c inode differs. That is deliberate and is the sharper test: a substitution that
 * changed every field would pass a comparison that only looked at one of them, whereas this
 * one fails only if the inode is genuinely part of the comparison.
 */
const DriverAidlImpl::BinderNodeIdentity kSubstitutedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000b2b2ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0600),
    0u,
};

/**
 * @brief The SAME OBJECT, re-permissioned: every field of kValidatedNodeIdentity but the mode
 *
 * THE SHAPE OF THE ATTACK THIS EXISTS TO EXPRESS, and the reason it is a separate fixture from
 * kSubstitutedNodeIdentity rather than another variant of it. The device, the inode and the
 * rdev are IDENTICAL, so this is not a substituted node - it is the very node the preflight
 * validated, `chmod`ed after the validation. An adversary who cannot replace the node can
 * still widen access to it, and a re-verification comparing only `(device, inode, rdev)`
 * reports that as "unchanged" because the object genuinely is the same object.
 *
 * The mode is world-writable rather than merely different, so what the case demonstrates is a
 * privilege WIDENING and not an arbitrary edit.
 */
const DriverAidlImpl::BinderNodeIdentity kRePermissionedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0666),
    0u,
};

/**
 * @brief The SAME OBJECT, re-owned: every field of kValidatedNodeIdentity but the uid
 *
 * The other half of the same window, and not implied by the mode case: `chmod` and `chown` are
 * different operations with different preconditions, and a comparison could plausibly be
 * written to cover one and not the other. Device, inode, rdev and mode are identical, so a
 * decline here can only come from the owner being compared.
 *
 * The owner is an unprivileged uid, which is what makes it a handover of the driver node
 * rather than a cosmetic change - and note that the preflight's OWN root-owner check cannot
 * catch it, because that check ran before the change.
 */
const DriverAidlImpl::BinderNodeIdentity kReOwnedNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0600),
    1000u,
};

/**
 * @brief A well-formed node published BROADLY ACCESSIBLE, as a conformant platform may
 *
 * A root-owned character device whose mode is 0666. This is not a malformed node and must not
 * be treated as one: a binder device node has to be openable by every client process that uses
 * binder, so AOSP-derived layouts publish it this way deliberately. It exists so the suite can
 * assert that the preflight ACCEPTS it, which is the regression guard on the one change in this
 * area that would look like a security improvement and would in fact decline the AIDL path on
 * every conformant production platform.
 */
const DriverAidlImpl::BinderNodeIdentity kWorldWritableNodeIdentity = {
    0x0000000000000015ULL,
    0x000000000000a1a1ULL,
    0x0000000000000c00ULL,
    static_cast<unsigned int>(S_IFCHR | 0666),
    0u,
};

/**
 * @brief Configuration and observations for the synthetic probe
 *
 * The @c answer members decide which decision point the predicate stops at; the @c *Calls
 * counters and the @c last* members are what the cases assert against afterwards.
 *
 * THE @c *Second MEMBERS EXIST BECAUSE isServiceAvailable() ASKS TWICE. It pings the context
 * manager once in the preflight and once again immediately before the lookup, and it
 * identifies a descriptor once for the retained node and once for the reopened one - so the
 * arms where the platform CHANGES between the check and the use are only reachable if the
 * probe can answer differently on the second call. Each @c has*Second flag defaults to false,
 * which keeps every existing case's behaviour exactly what it was.
 */
struct SyntheticProbeState {
    /** @brief What openNode() returns: kSyntheticBinderFd, or negative to fail the open. */
    int answerOpen = kSyntheticBinderFd;
    /** @brief What readProtocolVersion() returns: 0 for success, -1 to fail the ioctl. */
    int answerProtocolRead = 0;
    /** @brief The protocol version readProtocolVersion() reports on success. */
    unsigned int answerProtocolVersion = 0u;
    /** @brief What pingContextManager() reports on its FIRST call. */
    bool answerPing = false;
    /** @brief Whether pingContextManager() answers differently from its second call onwards. */
    bool hasSecondPingAnswer = false;
    /** @brief What pingContextManager() reports from its second call onwards, when enabled. */
    bool answerPingSecond = false;
    /** @brief What identifyDescriptor() returns: 0 for success, -1 to fail the fstat. */
    int answerIdentifyDescriptor = 0;
    /** @brief The identity identifyDescriptor() reports on its FIRST successful call. */
    DriverAidlImpl::BinderNodeIdentity answerDescriptorIdentity = kValidatedNodeIdentity;
    /** @brief Whether identifyDescriptor() reports a different node from its second call onwards. */
    bool hasSecondDescriptorIdentity = false;
    /** @brief The identity identifyDescriptor() reports from its second call onwards, when enabled. */
    DriverAidlImpl::BinderNodeIdentity answerDescriptorIdentitySecond = kSubstitutedNodeIdentity;
    /**
     * @brief Whether identifyDescriptor() reports a different RESULT from its second call onwards
     *
     * Distinct from hasSecondDescriptorIdentity, which changes the identity a SUCCESSFUL call
     * reports. This changes whether the call succeeds at all, which is a different arm: the
     * re-verification declines when the reopened descriptor cannot be identified, and it
     * declines when it identifies as a different node, and neither implies the other.
     * answerIdentifyDescriptor cannot express it, because it applies to every call and the
     * preflight's own first call has to succeed for the second one to happen.
     */
    bool hasSecondDescriptorIdentifyResult = false;
    /** @brief What identifyDescriptor() returns from its second call onwards, when enabled. */
    int answerIdentifyDescriptorSecond = 0;
    /** @brief What identifyPath() returns: 0 for success, -1 to fail the stat. */
    int answerIdentifyPath = 0;
    /** @brief The identity identifyPath() reports on success - the node the NAME resolves to. */
    DriverAidlImpl::BinderNodeIdentity answerPathIdentity = kValidatedNodeIdentity;

    /** @brief How many times openNode() was called. */
    int openCalls = 0;
    /** @brief How many times identifyDescriptor() was called. */
    int identifyDescriptorCalls = 0;
    /** @brief How many times identifyPath() was called. */
    int identifyPathCalls = 0;
    /** @brief How many times readProtocolVersion() was called. */
    int protocolReadCalls = 0;
    /** @brief How many times pingContextManager() was called. */
    int pingCalls = 0;
    /** @brief How many times closeNode() was called. */
    int closeCalls = 0;

    /** @brief The path openNode() was last given. */
    std::string lastOpenedPath;
    /** @brief The flags openNode() was last given. */
    int lastOpenFlags = 0;
    /** @brief The descriptor readProtocolVersion() was last given. */
    int lastProtocolFd = -1;
    /** @brief The descriptor pingContextManager() was last given. */
    int lastPingFd = -1;
    /** @brief The deadline pingContextManager() was last given, in milliseconds. */
    unsigned int lastPingTimeoutMs = 0u;
    /** @brief The descriptor closeNode() was last given. */
    int lastClosedFd = -1;
    /** @brief The descriptor identifyDescriptor() was last given. */
    int lastIdentifiedFd = -1;
    /** @brief The path identifyPath() was last given. */
    std::string lastIdentifiedPath;
    /** @brief Every descriptor closeNode() was given, in order, so pairing can be asserted. */
    std::vector<int> closedFds;
};

SyntheticProbeState g_syntheticProbe;

/**
 * @brief Records the call and answers with the configured descriptor.
 *
 * The second successful open in one call answers with kSyntheticSecondBinderFd rather than
 * repeating the first descriptor, so that a case can tell the retained node from the one
 * reopened for the pre-lookup liveness check. A failing configuration fails both.
 *
 * @param [in] path  Driver node path the predicate asked for. Recorded verbatim; a null
 *                  pointer is recorded as an empty string rather than dereferenced.
 * @param [in] flags Open flags the predicate asked for. Recorded, never acted on.
 *
 * @return int - SyntheticProbeState::answerOpen, which is kSyntheticBinderFd for a
 *               successful open or a negative value to fail it.
 */
int syntheticOpenNode(const char *path, int flags) {
    g_syntheticProbe.openCalls++;
    g_syntheticProbe.lastOpenedPath = (path != nullptr) ? std::string(path) : std::string();
    g_syntheticProbe.lastOpenFlags = flags;

    if (g_syntheticProbe.answerOpen < 0) {
        return g_syntheticProbe.answerOpen;
    }

    return (g_syntheticProbe.openCalls >= 2) ? kSyntheticSecondBinderFd
                                             : g_syntheticProbe.answerOpen;
}

/** @brief Records the call and reports the configured node identity, or fails the fstat. */
int syntheticIdentifyDescriptor(int fd, DriverAidlImpl::BinderNodeIdentity *out) {
    g_syntheticProbe.identifyDescriptorCalls++;
    g_syntheticProbe.lastIdentifiedFd = fd;

    if (g_syntheticProbe.answerIdentifyDescriptor != 0) {
        return g_syntheticProbe.answerIdentifyDescriptor;
    }

    /*
     * The second-call FAILURE arm, which is checked before the second-call IDENTITY arm below
     * because a call that fails reports no identity at all.
     */
    if (g_syntheticProbe.hasSecondDescriptorIdentifyResult &&
        (g_syntheticProbe.identifyDescriptorCalls >= 2) &&
        (g_syntheticProbe.answerIdentifyDescriptorSecond != 0)) {
        return g_syntheticProbe.answerIdentifyDescriptorSecond;
    }

    if (out != nullptr) {
        const bool useSecond = g_syntheticProbe.hasSecondDescriptorIdentity &&
                               (g_syntheticProbe.identifyDescriptorCalls >= 2);

        *out = useSecond ? g_syntheticProbe.answerDescriptorIdentitySecond
                         : g_syntheticProbe.answerDescriptorIdentity;
    }

    return 0;
}

/** @brief Records the call and reports what the NAME resolves to, or fails the stat. */
int syntheticIdentifyPath(const char *path, DriverAidlImpl::BinderNodeIdentity *out) {
    g_syntheticProbe.identifyPathCalls++;
    g_syntheticProbe.lastIdentifiedPath = (path != nullptr) ? std::string(path) : std::string();

    if (g_syntheticProbe.answerIdentifyPath != 0) {
        return g_syntheticProbe.answerIdentifyPath;
    }

    if (out != nullptr) {
        *out = g_syntheticProbe.answerPathIdentity;
    }

    return 0;
}

/**
 * @brief Records the call and reports the configured protocol version, or fails the read.
 *
 * @param [in]  fd      Descriptor the predicate is reading through. Recorded only.
 * @param [out] version Receives SyntheticProbeState::answerProtocolVersion. Left untouched
 *                     when the read is configured to fail, or when the pointer is null.
 *
 * @return int - Read result
 * @retval 0  - The version was reported.
 * @retval -1 - The ioctl is configured to fail, and @p version is untouched.
 */
int syntheticReadProtocolVersion(int fd, unsigned int *version) {
    g_syntheticProbe.protocolReadCalls++;
    g_syntheticProbe.lastProtocolFd = fd;

    if (g_syntheticProbe.answerProtocolRead == 0 && version != nullptr) {
        *version = g_syntheticProbe.answerProtocolVersion;
    }
    return g_syntheticProbe.answerProtocolRead;
}

/**
 * @brief Records the call, including the deadline it was given, and answers.
 *
 * Answers differently from the second call onwards when @c hasSecondPingAnswer is set,
 * which is the only way to reach the arm where the context manager answered the preflight
 * and has stopped answering by the time the lookup is about to happen.
 *
 * @param [in] fd        Descriptor the predicate is pinging through. Recorded only.
 * @param [in] timeoutMs Bounded deadline the predicate applied, in milliseconds. Recorded
 *                      so a case can assert that the predicate bounds the wait at all.
 *
 * @return bool - SyntheticProbeState::answerPing, unmodified.
 */
bool syntheticPingContextManager(int fd, unsigned int timeoutMs) {
    g_syntheticProbe.pingCalls++;
    g_syntheticProbe.lastPingFd = fd;
    g_syntheticProbe.lastPingTimeoutMs = timeoutMs;

    if (g_syntheticProbe.hasSecondPingAnswer && (g_syntheticProbe.pingCalls >= 2)) {
        return g_syntheticProbe.answerPingSecond;
    }

    return g_syntheticProbe.answerPing;
}

/**
 * @brief Records the release of the descriptor, in order, and always succeeds.
 *
 * @param [in] fd Descriptor being released. Recorded in call order, so a case can assert
 *               that the descriptor closed is the one that was opened.
 *
 * @return int - Always 0. A close that could fail would introduce a decision point the
 *               predicate does not have.
 */
int syntheticCloseNode(int fd) {
    g_syntheticProbe.closeCalls++;
    g_syntheticProbe.lastClosedFd = fd;
    g_syntheticProbe.closedFds.push_back(fd);
    return 0;
}

/**
 * @brief Clears every counter and installs one answer per decision point.
 *
 * THE IDENTITY ANSWERS RESET TO A WELL-FORMED NODE - a root-owned character device whose
 * path and descriptor agree - so that every case written before the identity arms existed
 * keeps reaching the decision point it was written for. A case about an identity arm
 * overrides the field it is about after calling this, which is also what makes each of
 * those cases name exactly one deviation from a good node.
 *
 * @param [in] answerOpen            - openNode()'s answer: kSyntheticBinderFd, or negative.
 * @param [in] answerProtocolRead    - readProtocolVersion()'s answer: 0 or -1.
 * @param [in] answerProtocolVersion - Protocol version reported on a successful read.
 * @param [in] answerPing            - pingContextManager()'s answer.
 */
void resetSyntheticProbe(int answerOpen,
                         int answerProtocolRead,
                         unsigned int answerProtocolVersion,
                         bool answerPing) {
    g_syntheticProbe = SyntheticProbeState();
    g_syntheticProbe.answerOpen = answerOpen;
    g_syntheticProbe.answerProtocolRead = answerProtocolRead;
    g_syntheticProbe.answerProtocolVersion = answerProtocolVersion;
    g_syntheticProbe.answerPing = answerPing;
}

/**
 * @brief The probe the cases hand to isBinderPreflightOk(), wired to the six functions above
 *
 * BinderPreflightProbe is an aggregate of six function pointers, so this is a brace
 * initialization and nothing more. The returned object is bound to the predicate's const
 * reference parameter for the duration of the call.
 *
 * @return DriverAidlImpl::BinderPreflightProbe - An aggregate of the six functions
 *                                                above, returned by value.
 *
 * @warning The member order here must match the struct's, and every member must be filled:
 *          the production code calls all six unconditionally and does not defend against a
 *          partially filled probe, so a member left out of this aggregate becomes a null
 *          function pointer that the preflight calls through. That is why this is the only
 *          aggregate initialization of the probe in the suite.
 */
DriverAidlImpl::BinderPreflightProbe syntheticProbe() {
    DriverAidlImpl::BinderPreflightProbe probe = {
        syntheticOpenNode,
        syntheticIdentifyDescriptor,
        syntheticIdentifyPath,
        syntheticReadProtocolVersion,
        syntheticPingContextManager,
        syntheticCloseNode,
    };
    return probe;
}

/**
 * @brief A well-formed directed frame: initiator 4, destination 0, then an opcode.
 *
 * @param [in] opcode Opcode byte to follow the header.
 *
 * @return CECFrame - A two-byte frame whose destination nibble is 0, so the status
 *                    translation treats it as directed.
 */
CECFrame directedFrame(uint8_t opcode = GIVE_DEVICE_POWER_STATUS) {
    CECFrame frame;
    frame.append(static_cast<uint8_t>(0x40));
    frame.append(opcode);
    return frame;
}

/**
 * @brief A well-formed broadcast frame: destination nibble 0x0F, then an opcode.
 *
 * @param [in] opcode Opcode byte to follow the header.
 *
 * @return CECFrame - A two-byte frame whose destination nibble is 0x0F, so the status
 *                    translation treats it as a broadcast.
 */
CECFrame broadcastFrame(uint8_t opcode = REPORT_PHYSICAL_ADDRESS) {
    CECFrame frame;
    frame.append(static_cast<uint8_t>(0x4F));
    frame.append(opcode);
    return frame;
}

/**
 * @brief A directed frame padded with operands to exactly @p length bytes.
 *
 * Used for the frame-size boundary. The header and opcode are real so the frame is
 * well-formed at every length; the padding carries no meaning and is not decoded by
 * anything on either path.
 *
 * @param [in] length Total frame length in bytes. A value at or below the two bytes
 *                   directedFrame() already produces yields that frame unpadded.
 *
 * @return CECFrame - A well-formed directed frame of exactly @p length bytes.
 */
CECFrame frameOfLength(size_t length) {
    CECFrame frame = directedFrame();
    while (frame.length() < length) {
        frame.append(static_cast<uint8_t>(0x00));
    }
    return frame;
}

/*
 * ---------------------------------------------------------------------------------
 * reading the real sink caller, so that the modelled call paths cannot go stale.
 *
 * The case that measures authorized difference 3 through the Sink's two call paths models
 * their exception handling: a try with three catches for one, an uncaught propagation for
 * the other. A model is only evidence for as long as it still describes the thing modelled,
 * and nothing in a hand-written model notices when the plugin is reformatted, when a catch
 * arm is removed, or when the caller stops calling the middleware altogether.
 *
 * So the structure is read from the plugin source rather than restated. The plugin is out of
 * scope and is not modified, nor compiled, nor linked - it belongs to another component and
 * its test binaries link the framework's CEC mock rather than this middleware, so compiling
 * it here is not available. What is available, and is what the drift guard needs, is its
 * text: the two call sites, whether each sits inside a try, and which handlers that try
 * carries.
 *
 * The scanner is small but it is not a regex, and that is deliberate. "Is this call inside a
 * try block" is a question about brace nesting, which a pattern match cannot answer, and a
 * naive text search would be fooled by a brace in a comment or a string literal. The scanner
 * below therefore walks the file once, skipping line comments, block comments, string
 * literals and character literals, maintaining the block stack, and recording each try
 * block's span together with the catch clauses that follow it. It is not a C++ parser and
 * does not need to be: it needs braces, comments, literals and two keywords.
 *
 * What it does not handle, stated rather than left to be discovered: raw string literals
 * (R"(...)"), which the scanned file does not contain, and macro-generated braces, which
 * would confuse any textual approach. If the plugin ever adopts either, this guard reports a
 * mismatch and the model must be re-derived by hand - which is a loud, visible outcome and
 * the right one.
 * ---------------------------------------------------------------------------------
 */

/** @brief One try block found by the scanner: its span, and the handlers that follow it. */
struct SourceTryBlock {
    /** @brief Offset of the try body's opening brace. */
    size_t bodyOpen = 0;
    /** @brief Offset of the try body's matching closing brace. */
    size_t bodyClose = 0;
    /** @brief The text inside each following catch clause's parentheses, in source order. */
    std::vector<std::string> handlers;
};

/** @brief One call site found by the scanner. */
struct SourceCallSite {
    /** @brief Offset of the first character of the matched call text. */
    size_t offset = 0;
    /** @brief One-based line number of the call, for diagnostics. */
    size_t line = 0;
    /** @brief Whether any enclosing block is a try block. */
    bool insideAnyTry = false;
    /** @brief Index into the model's try blocks for the innermost enclosing try, or -1. */
    int innermostTry = -1;
    /** @brief The code immediately before the call, whitespace collapsed, for receiver checks. */
    std::string precedingText;
};

/**
 * @brief A one-pass brace/comment/literal-aware model of a C++ source file
 *
 * Records every try block with its handler list and every occurrence of one call text,
 * annotated with the try nesting it sits in. Nothing here interprets types, templates or
 * preprocessor conditionals - the model answers exactly the two questions the drift guard
 * asks and no more.
 */
class CppSourceModel {
public:
    /**
     * @brief Walks @p text once, recording try blocks and occurrences of @p callNeedle
     *
     * @param [in] text       - Whole file contents.
     * @param [in] callNeedle - Call text to locate, matched only at an identifier boundary.
     *
     * @return bool - Whether the walk completed with a balanced block stack
     * @retval true  - The file parsed far enough for the model to be trusted.
     * @retval false - Braces did not balance, so the model must not be relied on.
     */
    bool scan(const std::string &text, const std::string &callNeedle) {
        tryBlocks_.clear();
        callSites_.clear();

        /** @brief One entry on the brace stack: whether it is a try body, and which one. */
        struct OpenBlock {
            /** @brief Whether this block is the body of a try. */
            bool isTry;
            /** @brief Index into tryBlocks_ when isTry, and -1 otherwise. */
            int tryIndex;
        };

        std::vector<OpenBlock> stack;
        std::vector<int> lastClosedTryAtDepth;
        size_t line = 1;
        const size_t length = text.size();

        for (size_t i = 0; i < length; ) {
            const char c = text[i];

            if (c == '\n') {
                line++;
                i++;
                continue;
            }

            // Line comment: everything to the newline is invisible to the model.
            if (c == '/' && (i + 1) < length && text[i + 1] == '/') {
                while (i < length && text[i] != '\n') {
                    i++;
                }
                continue;
            }

            // Block comment, counting the newlines inside it so line numbers stay right.
            if (c == '/' && (i + 1) < length && text[i + 1] == '*') {
                i += 2;
                while ((i + 1) < length && !(text[i] == '*' && text[i + 1] == '/')) {
                    if (text[i] == '\n') {
                        line++;
                    }
                    i++;
                }
                i = (i + 1 < length) ? i + 2 : length;
                continue;
            }

            // String and character literals, with backslash escapes honoured, so a brace or
            // a keyword inside one cannot reach the block stack.
            if (c == '"' || c == '\'') {
                const char quote = c;
                i++;
                while (i < length && text[i] != quote) {
                    if (text[i] == '\\' && (i + 1) < length) {
                        i++;
                    }
                    else if (text[i] == '\n') {
                        line++;
                    }
                    i++;
                }
                i = (i < length) ? i + 1 : length;
                continue;
            }

            if (c == '{') {
                const bool isTry = isPrecededByKeyword(text, i, "try");
                OpenBlock block = { isTry, -1 };

                if (isTry) {
                    SourceTryBlock record;
                    record.bodyOpen = i;
                    tryBlocks_.push_back(record);
                    block.tryIndex = static_cast<int>(tryBlocks_.size()) - 1;
                }
                stack.push_back(block);
                i++;
                continue;
            }

            if (c == '}') {
                if (stack.empty()) {
                    return false;
                }

                const OpenBlock block = stack.back();
                stack.pop_back();

                if (block.isTry) {
                    tryBlocks_[static_cast<size_t>(block.tryIndex)].bodyClose = i;

                    // Remember it at the depth it lived at, so the catch clauses that follow -
                    // which are scanned at that same depth - can be attached to it.
                    if (lastClosedTryAtDepth.size() <= stack.size()) {
                        lastClosedTryAtDepth.resize(stack.size() + 1, -1);
                    }
                    lastClosedTryAtDepth[stack.size()] = block.tryIndex;
                }
                i++;
                continue;
            }

            // A catch clause attaches to the try that most recently closed at this depth. A
            // catch cannot legally follow anything else, so no further disambiguation is
            // needed.
            if (c == 'c' && matchesIdentifier(text, i, "catch")) {
                size_t cursor = skipBlanks(text, i + 5, line);

                if (cursor < length && text[cursor] == '(') {
                    const size_t parameterEnd = matchParenthesis(text, cursor);

                    if (parameterEnd == std::string::npos) {
                        return false;
                    }

                    if (stack.size() < lastClosedTryAtDepth.size()
                        && lastClosedTryAtDepth[stack.size()] >= 0) {
                        const size_t owner =
                            static_cast<size_t>(lastClosedTryAtDepth[stack.size()]);
                        tryBlocks_[owner].handlers.push_back(
                            collapseBlanks(text.substr(cursor + 1, parameterEnd - cursor - 1)));
                    }
                    i = parameterEnd + 1;
                    continue;
                }
            }

            if (c == callNeedle[0] && matchesIdentifier(text, i, callNeedle)) {
                SourceCallSite site;
                site.offset = i;
                site.line = line;

                for (size_t depth = 0; depth < stack.size(); depth++) {
                    if (stack[depth].isTry) {
                        site.insideAnyTry = true;
                        site.innermostTry = stack[depth].tryIndex;
                    }
                }

                const size_t contextStart = (i > 96u) ? (i - 96u) : 0u;
                site.precedingText = collapseBlanks(text.substr(contextStart, i - contextStart));
                callSites_.push_back(site);
                i += callNeedle.size();
                continue;
            }

            i++;
        }

        return stack.empty();
    }

    /**
     * @brief The call sites found, in source order.
     *
     * @return const std::vector<SourceCallSite>& - The call sites recorded by the last
     *                                              successful scan(), in source order. Empty
     *                                              before the first scan.
     */
    const std::vector<SourceCallSite> &callSites() const { return callSites_; }

    /**
     * @brief The try blocks found, in the order their bodies opened.
     *
     * @return const std::vector<SourceTryBlock>& - The try blocks recorded by the last
     *                                              successful scan(), ordered by where their
     *                                              bodies opened. Empty before the first scan.
     */
    const std::vector<SourceTryBlock> &tryBlocks() const { return tryBlocks_; }

private:
    /** @brief Whether @p keyword ends immediately before @p position, blanks aside. */
    static bool isPrecededByKeyword(const std::string &text, size_t position,
                                    const std::string &keyword) {
        size_t cursor = position;

        while (cursor > 0) {
            const char previous = text[cursor - 1];

            if (previous == ' ' || previous == '\t' || previous == '\n' || previous == '\r') {
                cursor--;
                continue;
            }

            // A trailing block comment between the keyword and the brace is skipped too.
            if (previous == '/' && cursor >= 2 && text[cursor - 2] == '*') {
                const size_t opening = text.rfind("/*", cursor - 2);
                if (opening == std::string::npos) {
                    return false;
                }
                cursor = opening;
                continue;
            }
            break;
        }

        if (cursor < keyword.size()) {
            return false;
        }

        if (text.compare(cursor - keyword.size(), keyword.size(), keyword) != 0) {
            return false;
        }

        const size_t before = cursor - keyword.size();
        return (before == 0) || !isIdentifierCharacter(text[before - 1]);
    }

    /** @brief Whether @p needle starts at @p position and is not part of a longer identifier. */
    static bool matchesIdentifier(const std::string &text, size_t position,
                                  const std::string &needle) {
        if (text.compare(position, needle.size(), needle) != 0) {
            return false;
        }
        return (position == 0) || !isIdentifierCharacter(text[position - 1]);
    }

    /**
     * @brief Whether @p c can appear inside a C++ identifier.
     *
     * @param [in] c Character to classify.
     *
     * @return bool - True for a letter, a digit or an underscore; false for anything else,
     *                which is what makes a keyword match a whole token rather than a
     *                substring of a longer name.
     */
    static bool isIdentifierCharacter(char c) {
        return (c == '_') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9');
    }

    /**
     * @brief Advances past whitespace and comments, counting newlines into @p line.
     *
     * @param [in]     text     Source text being walked.
     * @param [in]     position Offset to start from.
     * @param [in,out] line     One-based line counter, advanced by every newline crossed so
     *                         that a later diagnostic can name the line.
     *
     * @return size_t - Offset of the first character that is neither whitespace nor part of
     *                  a comment, or text.size() if the text ends first.
     */
    static size_t skipBlanks(const std::string &text, size_t position, size_t &line) {
        const size_t length = text.size();

        while (position < length) {
            const char c = text[position];

            if (c == '\n') {
                line++;
                position++;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r') {
                position++;
                continue;
            }
            if (c == '/' && (position + 1) < length && text[position + 1] == '/') {
                while (position < length && text[position] != '\n') {
                    position++;
                }
                continue;
            }
            if (c == '/' && (position + 1) < length && text[position + 1] == '*') {
                position += 2;
                while ((position + 1) < length
                       && !(text[position] == '*' && text[position + 1] == '/')) {
                    if (text[position] == '\n') {
                        line++;
                    }
                    position++;
                }
                position = (position + 1 < length) ? position + 2 : length;
                continue;
            }
            break;
        }
        return position;
    }

    /**
     * @brief Offset of the parenthesis matching the one at @p position, or npos.
     *
     * @param [in] text     Source text being walked.
     * @param [in] position Offset of the opening parenthesis.
     *
     * @return size_t - Offset of the matching closing parenthesis, or std::string::npos when
     *                  the parentheses do not balance before the text ends.
     */
    static size_t matchParenthesis(const std::string &text, size_t position) {
        int depth = 0;

        for (size_t i = position; i < text.size(); i++) {
            if (text[i] == '(') {
                depth++;
            }
            else if (text[i] == ')') {
                depth--;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string::npos;
    }

    /**
     * @brief Collapses every run of whitespace to a single space.
     *
     * @param [in] text Text to normalize.
     *
     * @return std::string - @p text with every run of whitespace reduced to one space and no
     *                       leading blank, so that a receiver check compares shape rather
     *                       than the plugin's current formatting.
     */
    static std::string collapseBlanks(const std::string &text) {
        std::string collapsed;
        bool pendingBlank = false;

        for (size_t i = 0; i < text.size(); i++) {
            const char c = text[i];

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pendingBlank = !collapsed.empty();
                continue;
            }
            if (pendingBlank) {
                collapsed.push_back(' ');
                pendingBlank = false;
            }
            collapsed.push_back(c);
        }
        return collapsed;
    }

    /** @brief Try blocks recorded by the last scan(), ordered by where their bodies opened. */
    std::vector<SourceTryBlock> tryBlocks_;
    /** @brief Call sites recorded by the last scan(), in source order. */
    std::vector<SourceCallSite> callSites_;
};

/**
 * @brief The Sink caller source's location relative to a workspace root
 *
 * The plugin is a sibling component of this one, so every candidate below is this constant
 * appended to a root. Named once so the guard's diagnostics and its search agree.
 */
const char *const kSinkCallerRelativePath =
    "entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp";

/**
 * @brief The environment variable both CI workflows export for the pinned Sink checkout.
 *
 * The checkout it names is a required step in both workflows, at the reviewed commit recorded
 * above the drift guard, so a value that is absent or unreadable is a fault to report rather
 * than a condition to accommodate - which is what the guard's two failure arms do.
 */
const char *const kSinkCallerSourceVariable = "CEC_SINK_CALLER_SOURCE";

/** @brief Where the Sink caller source was found, or every path that was tried. */
struct SinkCallerSource {
    /** @brief Whether CEC_SINK_CALLER_SOURCE was set to a non-empty value. */
    bool variableSet = false;
    /** @brief The value of that variable, when it was set. */
    std::string variableValue;
    /** @brief Whether the file was located and read. */
    bool found = false;
    /** @brief The path that was read, when found. */
    std::string path;
    /** @brief Every path that was tried, in order, for the diagnostic. */
    std::vector<std::string> pathsTried;
    /** @brief The file contents, when found. */
    std::string text;
};

/**
 * @brief Reads a whole file, reporting success
 *
 * @param [in]  path     Path to read. Opened binary, so no newline translation occurs and the
 *                      text the scanner walks is exactly what is on disk.
 * @param [out] contents Receives the file's bytes. Left untouched when the pointer is null,
 *                      and written even when the file turns out to be empty.
 *
 * @return bool - Whether usable text was read
 * @retval true  - The file opened, read without error, and is not empty.
 * @retval false - The path could not be opened, the read failed, or the file is empty. An
 *                 empty file is a failure here because the guard's only use for the text is
 *                 to scan it, and there is nothing to scan.
 */
bool readWholeFile(const std::string &path, std::string *contents) {
    std::ifstream stream(path.c_str(), std::ios::in | std::ios::binary);

    if (!stream.is_open()) {
        return false;
    }

    std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    if (stream.bad()) {
        return false;
    }

    if (contents != nullptr) {
        *contents = text;
    }
    return !text.empty();
}

/**
 * @brief The directory holding this process's executable
 *
 * @return std::string - The directory part of `/proc/self/exe`, without a trailing separator,
 *                       or an empty string when the link cannot be read or contains no
 *                       separator. An empty result makes the relative candidates below
 *                       unusable, which the guard reports rather than treating as a mismatch.
 */
std::string executableDirectory() {
    char resolved[4096];
    const ssize_t written = ::readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);

    if (written <= 0) {
        return std::string();
    }
    resolved[written] = '\0';

    const std::string path(resolved);
    const size_t separator = path.rfind('/');

    return (separator == std::string::npos) ? std::string() : path.substr(0, separator);
}

/**
 * @brief Locates and reads the real Sink caller source
 *
 * CEC_SINK_CALLER_SOURCE wins when it is set, and a value that cannot be read is not
 * silently replaced by a search: both CI workflows export it only after checking that the
 * file is there, so an unreadable value means the environment is lying about what it
 * provided. The caller fails on that, and fails separately when nothing pointed at the source
 * and no candidate path resolved either - the source is a required input, so both outcomes are
 * failures and the two are reported apart because they call for different fixes.
 *
 * The fallback candidates are the documented sibling-checkout layout, from two independent
 * anchors: the working directory, which is `hdmicec/tests/L1Tests` under `make check` and
 * under run_coverage.sh, and the executable's own directory, which is that same directory or
 * its `.libs` subdirectory when libtool wrapped the binary. Both depths were verified
 * against the built runner's location rather than assumed.
 *
 * @return SinkCallerSource - The search's whole record: whether the override variable was
 *                            set and to what, every path tried in order, and the text when
 *                            one of them read. A result whose @c found is false still
 *                            carries the path list, which is what lets the guard report
 *                            where it looked instead of only that it failed.
 */
SinkCallerSource locateAndReadSinkCallerSource() {
    SinkCallerSource source;

    const char *const configured = ::getenv(kSinkCallerSourceVariable);

    if (configured != nullptr && configured[0] != '\0') {
        source.variableSet = true;
        source.variableValue = configured;
        source.pathsTried.push_back(source.variableValue);
        source.found = readWholeFile(source.variableValue, &source.text);

        if (source.found) {
            source.path = source.variableValue;
        }
        return source;
    }

    std::vector<std::string> roots;
    roots.push_back("../../..");

    const std::string exeDirectory = executableDirectory();
    if (!exeDirectory.empty()) {
        roots.push_back(exeDirectory + "/../../..");
        roots.push_back(exeDirectory + "/../../../..");
    }

    for (size_t i = 0; i < roots.size(); i++) {
        const std::string candidate = roots[i] + "/" + kSinkCallerRelativePath;
        source.pathsTried.push_back(candidate);

        if (readWholeFile(candidate, &source.text)) {
            source.found = true;
            source.path = candidate;
            break;
        }
    }
    return source;
}

/**
 * @brief Joins the paths a search tried, for a diagnostic that names all of them
 *
 * @param [in] paths Paths in the order they were tried.
 *
 * @return std::string - Each path bracketed and comma-separated, and empty for an empty
 *                       input. Bracketed rather than bare so that a path containing a space
 *                       still reads as one entry.
 */
std::string joinPaths(const std::vector<std::string> &paths) {
    std::string joined;

    for (size_t i = 0; i < paths.size(); i++) {
        if (i > 0) {
            joined += ", ";
        }
        joined += "[" + paths[i] + "]";
    }
    return joined;
}

/*
 * ---------------------------------------------------------------------------------
 * observing a received frame where the middleware actually delivers it.
 *
 * A receive-path case that asserted only "the trigger returned true" would pass against an
 * adapter that dropped every frame on the floor - the trigger's return value describes
 * whether a listener was captured, not whether the frame went anywhere. What has to be
 * observed instead is the delivery, and the place the middleware delivers to is a
 * FrameListener on a Connection: the adapter offers the frame onto the incoming queue, the
 * Bus reader thread drains it, Connection's address filter admits it, and the listener is
 * notified.
 *
 * Delivery is therefore asynchronous with respect to the case body even where the callback
 * itself runs on the calling thread, because the Bus reader is a separate thread. The wait
 * below is a condition variable with a deadline rather than a sleep: a sleep long enough to
 * be reliable would be wasted on every passing run, and a sleep short enough not to be
 * wasted would be flaky - and neither is an assertion. The idiom is the one the neighbouring
 * integration cases established (ccec/test_Connection.cpp).
 * ---------------------------------------------------------------------------------
 */

/**
 * @brief Upper bound on a delivery that is expected to happen, in milliseconds
 *
 * Generous rather than tight, because it is only ever reached on a failing run: the wait is a
 * condition variable and returns the instant the frame arrives, so a passing case pays the real
 * delivery latency and nothing more. The value matches the bound the neighbouring end-to-end
 * integration cases use (ccec/test_Connection.cpp), so a slow host does not make one of them
 * flaky while the other holds.
 */
constexpr int kFrameDeliveryTimeoutMs = 3000;

/**
 * @brief Window over which a delivery is expected not to happen, in milliseconds
 *
 * Necessarily a real wait, since "it has not arrived" and "it will not arrive" cannot be
 * distinguished instantaneously. Deliberately much shorter than the positive bound, for two
 * reasons: a correct rejection is synchronous with the callback, so a frame that was wrongly
 * accepted would be delivered within microseconds of the offer rather than late; and the driver
 * is closed throughout this window, which leaves the Bus reader spinning on its own state guard,
 * so a long window buys no confidence and costs real CPU. The strong evidence for non-delivery
 * is not this window at all - it is the assertion that the frame does not surface after a later
 * re-open, which is an ordering and not a timing.
 */
constexpr int kNonDeliveryWindowMs = 400;

/**
 * @brief The middleware's slow-synchronous-call threshold, in milliseconds, RESTATED
 *
 * Mirrors @c SLOW_HAL_CALL_WARN_MS in @c ccec/src/DriverAidlImpl.cpp. It is restated rather than
 * read because that constant sits inside the anonymous namespace opened at DriverAidlImpl.cpp:185
 * and therefore has internal linkage - @c nm finds no dynamic symbol for it - and because
 * exposing it through the class would be a production change this suite has no business making.
 *
 * @warning Restating a production constant is normally a hazard, and here it is safe in the one
 *          direction that matters. The only case using it delays a HAL call by this value plus a
 *          margin and asserts that the warning line APPEARS. If production raised its threshold
 *          above this value, the delay would fall short, no line would be emitted and that case
 *          would fail - a loud wrong answer rather than a case that had silently stopped
 *          exercising the arm. If production lowered it, the case still crosses and still passes.
 *
 * @see FakeHdmiCecController::setAddLogicalAddressesDelayMs()
 */
constexpr int kSlowHalCallWarnMs = 1000;

/**
 * @brief A FrameListener that records the bytes of every frame it is notified of
 *
 * FrameListener::notify is const, so the recording members are mutable - the same shape the
 * neighbouring suite's listener uses, and for the same reason.
 */
class RecordingFrameListener : public FrameListener {
public:
    /**
     * @brief Records @p frame's bytes and wakes anything waiting on the count
     *
     * @param [in] frame Frame the Bus reader is delivering. Copied out immediately, because
     *                  the frame does not outlive the notification.
     *
     * @note Runs on the Bus reader thread, so the copy is taken under @c mutex_ and the
     *       notification is issued after the lock is dropped.
     */
    void notify(const CECFrame &frame) const override {
        const uint8_t *buffer = nullptr;
        size_t length = 0;
        frame.getBuffer(&buffer, &length);

        std::vector<uint8_t> bytes;
        if (buffer != nullptr) {
            bytes.assign(buffer, buffer + length);
        }

        {
            std::lock_guard<std::mutex> guard(mutex_);
            frames_.push_back(bytes);
        }
        condition_.notify_all();
    }

    /**
     * @brief Waits until at least @p expected frames have arrived, or the deadline passes
     *
     * @param [in] expected  - Number of frames to wait for.
     * @param [in] timeoutMs - Upper bound on the wait, in milliseconds.
     *
     * @return bool - Whether the count was reached within the bound.
     */
    bool waitForFrames(size_t expected, int timeoutMs) const {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this, expected]() { return frames_.size() >= expected; });
    }

    /**
     * @brief How many frames have been delivered so far
     *
     * @return size_t - The number of frames recorded at the moment of the call. A count
     *                  read while the reader thread is running is a snapshot, not a
     *                  settled total.
     */
    size_t frameCount() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return frames_.size();
    }

    /**
     * @brief The bytes of the frame at @p index
     *
     * @param [in] index Zero-based delivery order.
     *
     * @return std::vector<uint8_t> - A copy of that frame's bytes, or an empty vector when
     *                                fewer than @p index + 1 frames have arrived. An empty
     *                                vector is therefore not distinguishable from a
     *                                zero-length frame, which nothing here produces.
     */
    std::vector<uint8_t> frameAt(size_t index) const {
        std::lock_guard<std::mutex> guard(mutex_);
        return (index < frames_.size()) ? frames_[index] : std::vector<uint8_t>();
    }

private:
    /** @brief Guards @c frames_ against the Bus reader thread and the case body at once. */
    mutable std::mutex mutex_;
    /** @brief Notified after each delivery, so waitForFrames() never sleeps in a loop. */
    mutable std::condition_variable condition_;
    /** @brief The bytes of every frame delivered so far, in delivery order. */
    mutable std::vector<std::vector<uint8_t>> frames_;
};

/**
 * @brief A frame listener that PARKS THE BUS READER inside its notification until released
 *
 * Exists for one purpose the recording listener cannot serve: making the incoming queue fill up.
 * The Bus reader drains that queue as fast as frames are offered - it is normally blocked in
 * EventQueue::poll() on an empty queue and woken by each offer - so a case that simply delivered
 * many frames would never see an occupancy above one, and the queue's refusal arm would stay
 * unreachable however many frames it delivered.
 *
 * The mechanism is the reader's own call graph rather than anything injected into the queue. The
 * reader takes a frame, then notifies each registered listener on its own thread; a notification
 * that does not return leaves the reader inside it and not back at poll(). Frames offered from
 * that moment on accumulate, which is precisely the "stalled reader" condition the middleware's
 * refusal arm exists to survive.
 *
 * @warning notify() runs ON THE BUS READER THREAD and blocks it deliberately. A case that
 *          forgets to call release() hangs its own teardown, so every use must release on every
 *          exit path - the cases below do it before their assertions, so that an ASSERT_*
 *          returning early cannot strand the reader.
 * @warning notify() is @c const in the FrameListener interface, so all of this object's state is
 *          @c mutable. That is the same accommodation RecordingFrameListener makes.
 *
 * @see RecordingFrameListener
 * @see DriverAidlImpl::offerReceivedFrame()
 */
class StallingFrameListener : public FrameListener {
public:
    /**
     * @brief Blocks until release() is called, recording that the reader arrived
     *
     * @param [in] frame Frame the Bus reader is delivering. Its bytes are not copied: this
     *                  listener exists to hold the thread, and the delivered content is asserted
     *                  by the recording listener a case attaches alongside it where it matters.
     *
     * @note Records arrival BEFORE blocking and notifies waiters, so waitUntilParked() cannot
     *       race ahead of the block.
     */
    void notify(const CECFrame &frame) const override {
        (void)frame;

        std::unique_lock<std::mutex> guard(mutex_);

        ++parkedCount_;
        condition_.notify_all();

        // Predicate rather than a bare wait, so a spurious wake-up cannot let the reader escape
        // early and quietly drain the queue this listener is holding full.
        condition_.wait(guard, [this] { return released_; });
    }

    /**
     * @brief Waits until the reader is parked inside notify() at least once
     *
     * @param [in] timeoutMs Milliseconds to wait before giving up
     *
     * @return bool - true when the reader is parked, false on timeout
     *
     * @note A false return means the delivery chain never reached this listener at all, which is
     *       a different failure from the queue not filling and must be reported as such.
     */
    bool waitUntilParked(int timeoutMs) const {
        std::unique_lock<std::mutex> guard(mutex_);

        return condition_.wait_for(guard, std::chrono::milliseconds(timeoutMs),
                                   [this] { return parkedCount_ > 0; });
    }

    /**
     * @brief Releases the parked reader, and every later notification, immediately
     *
     * @return None
     *
     * @post notify() returns at once from now on, so this is safe to call more than once and safe
     *       to call when nothing is parked.
     */
    void release() const {
        {
            std::lock_guard<std::mutex> guard(mutex_);

            released_ = true;
        }

        condition_.notify_all();
    }

private:
    /** @brief Guards the two flags below against the Bus reader thread and the case body. */
    mutable std::mutex mutex_;
    /** @brief Signals both directions: parked for the case body, released for the reader. */
    mutable std::condition_variable condition_;
    /** @brief How many times the reader has entered notify(). Non-zero means parked. */
    mutable size_t parkedCount_ = 0;
    /** @brief Set once by release(); notify() returns immediately once it is set. */
    mutable bool released_ = false;
};

/**
 * @brief The thread-name prefix libbinder gives a threadpool thread ON AN ANDROID BUILD
 *
 * Not an invention of this suite: ProcessState::makeBinderThreadName() composes "binder:PID_N"
 * from the driver name and hands it to Thread::run() for each pool thread it spawns.
 *
 * @warning IT IS NOT APPLIED ON THIS LINUX PORT, so a thread carrying it does not exist even when
 *          the pool is running, and its absence is therefore not evidence about the pool. In the
 *          pinned Binder SDK, androidCreateRawThreadEtc() takes its threadName parameter as
 *          `__android_unused`, and the whole block that would honour it -- together with
 *          thread_data_t::trampoline, the only site that calls androidSetThreadName() for a
 *          spawned thread -- sits inside `#if defined(__ANDROID__)`, which the CMake build of the
 *          Binder SDK never defines. prctl(PR_SET_NAME) is therefore never reached and each pool
 *          thread inherits the process's own `comm`. Measured: a guest with a live binder driver,
 *          an AIDL back-end open since LibCCEC::init and a pool demonstrably started had no thread
 *          matching this prefix, and an assertion resting on the prefix failed against correct
 *          production code; every thread of this runner reports the runner's own name, and so does
 *          every thread of the fake service host, which also starts a pool. The pool is therefore
 *          asserted through ProcessState::getThreadPoolMaxThreadCount(), which reads the very flag
 *          startThreadPool() sets and behaves the same on both platforms; this prefix is what a
 *          build that DID apply the name would show, so the probe is kept as corroboration and is
 *          reported rather than asserted -- see processHasABinderThread() below.
 */
const char *const kBinderThreadNamePrefix = "binder:";

/**
 * @brief Whether this process currently has at least one thread named the way libbinder names its
 *        threadpool threads
 *
 * Reads /proc/self/task, which lists one directory per thread in this process, and compares each
 * thread's `comm` against the prefix ProcessState::makeBinderThreadName() composes, which
 * libbinder applies only on an Android build. It is CORROBORATION ONLY: see the warning on
 * kBinderThreadNamePrefix for why the name is never applied on this port, and
 * getThreadPoolMaxThreadCount() for what is asserted instead.
 *
 * @param [out] procIsReadable Set to true once /proc/self/task has been opened, and left
 *                            false when it cannot be. Ignored when null.
 *
 * @return bool - Whether a thread named "binder:*" exists in this process.
 * @retval true  - A binder threadpool thread is running AND this build applies the name.
 * @retval false - No such thread is named, which on this port is the ordinary case whether or not
 *                 a pool is running, so the caller must not read it as evidence either way.
 *
 * @warning A false result is not evidence that the threadpool is absent, for two distinct reasons,
 *          and neither is recoverable from inside this process. A host without /proc reports
 *          nothing at all, which @c procIsReadable distinguishes. And on this Linux port the name
 *          is never applied to the thread in the first place, for the compile-time reason recorded
 *          on kBinderThreadNamePrefix above - so a running pool produces threads that carry the
 *          process's own `comm` and this function returns false. Counting threads instead is not a
 *          substitute: the runner's thread count moves during a run as cases start and join their
 *          own workers, measured going 1 to 5 to 6 with different thread ids at equal counts, so a
 *          count-based inference would be unsound rather than merely imprecise.
 * @note There is consequently no reliable in-process observation of "the pool was started" on this
 *       SDK. The evidence that it was is invocation E's, which receives a `oneway` callback on a
 *       pool thread over real out-of-process IPC - see
 *       hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp. This function is retained because
 *       a build that does apply the name, or an SDK that switches to pthread_setname_np, would
 *       report true, and the caller prints what it observed either way.
 */
bool processHasABinderThread(bool *procIsReadable) {
    if (procIsReadable != nullptr) {
        *procIsReadable = false;
    }

    DIR *taskDirectory = ::opendir("/proc/self/task");

    if (taskDirectory == nullptr) {
        return false;
    }

    if (procIsReadable != nullptr) {
        *procIsReadable = true;
    }

    bool found = false;

    for (struct dirent *entry = ::readdir(taskDirectory); entry != nullptr && !found;
         entry = ::readdir(taskDirectory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        const std::string commPath = std::string("/proc/self/task/") + entry->d_name + "/comm";
        std::string comm;

        if (!readWholeFile(commPath, &comm)) {
            // A thread that exited between readdir and this open. Not an error, and not evidence.
            continue;
        }

        if (comm.compare(0, ::strlen(kBinderThreadNamePrefix), kBinderThreadNamePrefix) == 0) {
            found = true;
        }
    }

    ::closedir(taskDirectory);
    return found;
}

/**
 * @brief Shared state for a thread parked in DriverAidlImpl::read()
 *
 * Held behind a shared pointer, and not on the parking case's stack, for a reason that is
 * specific rather than stylistic: the whole point of that case is that the reader may fail to
 * be released, and a thread that cannot be joined must be abandoned rather than allowed to
 * hang the run - which is only safe if everything it touches outlives the scope that started
 * it. Every member is guarded by @c mutex and every transition notifies @c signal, so the
 * waiting side is a bounded condition-variable wait and never a sleep.
 */
struct BlockedReaderState {
    /** @brief Guards every member below. */
    std::mutex mutex;
    /** @brief Notified on each transition, so a waiter wakes the instant one happens. */
    std::condition_variable signal;

    /** @brief Set once the thread is about to enter its first read(). */
    bool enteredRead = false;
    /** @brief How many frames read() has returned normally. */
    int framesRead = 0;
    /** @brief Set when read() was released by the InvalidStateException the sentinel produces. */
    bool releasedByInvalidState = false;
    /** @brief Set when read() raised something else, which would be a different defect. */
    bool releasedByOtherException = false;
    /** @brief Set when the thread body has finished, whatever released it. */
    bool finished = false;

    /**
     * @brief Waits until @p predicate holds, or the bound passes
     *
     * @param [in] timeoutMs  - Upper bound on the wait, in milliseconds.
     * @param [in] predicate  - Called under the lock; the wait ends when it returns true.
     *
     * @return bool - Whether the predicate became true within the bound.
     */
    template <typename Predicate>
    bool waitFor(int timeoutMs, Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        return signal.wait_for(lock, std::chrono::milliseconds(timeoutMs), predicate);
    }
};

/**
 * @brief A Connection with a listener attached, withdrawn and closed by the destructor
 *
 * RAII rather than a pair of calls at the end of the case body, and for a concrete reason:
 * a fatal assertion returns from the body immediately, and a listener left registered would
 * be a Bus entry pointing at a destroyed stack object - which the next case's frame would
 * dereference. The destructor makes that unreachable however the case leaves.
 */
class ListeningConnection {
public:
    /**
     * @brief Opens a Connection on @p source and registers @p listener on it
     *
     * @param [in] source   - Logical address this connection filters for.
     * @param [in] name     - Connection name, used only in the middleware's own logs.
     * @param [in] listener - Listener to register; must outlive this object.
     */
    ListeningConnection(const LogicalAddress &source, const std::string &name,
                        FrameListener &listener)
        : connection_(source, true, name)
        , listener_(&listener)
    {
        connection_.addFrameListener(listener_);
    }

    /** @brief Not copyable: the registration is an identity the Bus holds a pointer to. */
    ListeningConnection(const ListeningConnection &) = delete;
    /** @brief Not assignable, for the same reason. */
    ListeningConnection &operator=(const ListeningConnection &) = delete;

    /** @brief Withdraws the listener and closes the connection, in that order. */
    ~ListeningConnection() {
        connection_.removeFrameListener(listener_);
        connection_.close();
    }

private:
    /** @brief The connection this object owns; closed by the destructor. */
    Connection connection_;
    /** @brief The registered listener, withdrawn by the destructor. Not owned. */
    FrameListener *listener_;
};


} // namespace

/**
 * @brief Compatibility-rejection coverage, per branch, by direct call.
 *
 * Runs under every invocation because it needs nothing from the environment: no registered
 * service, no binder driver, no resolved back-end and no HAL. Most cases call
 * halcompat::isCompatible<IHdmiCec>() against a locally constructed double and assert the
 * answer; the rest drive the production rejection diagnostic directly, and the last four
 * drive the fake service's and fake controller's own interface-metadata controls - the inputs
 * every compatibility arm above is arranged with - against locally constructed fakes.
 *
 * Nothing in this fixture is ever registered with the service manager, and that is a hard
 * constraint rather than a preference: registering a name calls defaultServiceManager(),
 * which opens the binder driver, and on the pinned stack that aborts the process where the
 * driver node is absent - which is this host. Local construction alone is what makes the
 * metadata controls testable here, and it is sufficient, because those controls take effect
 * under local dispatch and only under local dispatch.
 *
 * No driver precondition is established here, and that is a deliberate departure from the
 * fixture idiom the driver-facing suites in this directory use. Those fixtures open the
 * process-global driver in SetUp because their cases assert against it; nothing in this
 * fixture touches the driver, the HAL mock or the shared library at all, so opening the
 * driver would be interaction with shared state that this fixture has no use for - and
 * shared state that is touched for no reason is exactly how an order dependency gets
 * introduced. TearDown is likewise empty because there is nothing to restore.
 *
 * Self-sufficiency. Every case constructs its own double, calls the predicate and asserts
 * the result. Nothing is carried between cases, nothing outside the fixture is read or
 * written, and each case passes identically alone under a --gtest_filter and inside the
 * full suite, in any order.
 */
class DriverAidlCompatibilityTest : public ::testing::Test {
};

// The double used by every case below is a legitimate stand-in, and this case is what
// establishes that rather than assuming it.
//
// MetadataDouble overrides only getInterfaceHash() and getInterfaceVersion(), which is
// sufficient because halcompat::isCompatible reads nothing else (halcompat.h:161-171). The
// risk in a double that narrow is the opposite of the usual one: not that it does too little,
// but that an interface method might be reached and quietly answer something plausible,
// letting a case pass for a reason it does not name. IHdmiCecDefault forecloses that by
// answering every interface method with UNKNOWN_TRANSACTION (IHdmiCec.h:45-65), so any such
// call surfaces as a non-ok status rather than as a plausible answer - and that is asserted
// here, on all seven, so the guarantee is measured rather than read off the header.
//
// onAsBinder() returning nullptr is asserted for a second, separate reason: it is exactly why
// the fake service deliberately does not derive from IHdmiCecDefault but from BnHdmiCec
// (recorded on FakeHdmiCecService) - an object derived from the default can
// never be published to the service manager nor reached through a proxy. Pinning it here,
// where the one IHdmiCecDefault-derived object in the suite lives, keeps that reasoning
// attached to the thing it explains.
/**
 * @brief MetadataDouble answers no interface method, so no other case can pass on an
 *        answer it never asked for.
 * @pre Runs under every invocation, and against no resolved back-end: the double is
 *      constructed locally and halcompat is called directly.
 * @note Evidence: every one of the seven IHdmiCec interface methods returns a non-ok
 *       binder status, onAsBinder() returns nullptr, and the two overridden metadata
 *       members still report the values installed.
 * @see MetadataDouble
 */
TEST_F(DriverAidlCompatibilityTest, TheCompatibilityDoubleAnswersNoInterfaceMethod) {
    // Held by its concrete type rather than as an sp<IHdmiCec>, and deliberately:
    // IInterface declares onAsBinder() protected, and IHdmiCecDefault re-declares its override
    // public, so the assertion on it below is only well formed through the derived type. The
    // other calls would work either way.
    const ::android::sp<MetadataDouble> doubleUnderTest =
        ::android::sp<MetadataDouble>::make(std::string(kFrozenInterfaceHash),
                                           kClientInterfaceVersion);
    ASSERT_TRUE(doubleUnderTest != nullptr);

    // Every interface method must refuse. Out-parameters are supplied so that a method which
    // wrote one despite refusing would be a use-of-uninitialised rather than a silent pass.
    cechal::State state = cechal::State::CLOSED;
    ::std::optional< ::com::rdk::hal::PropertyValue> property;
    ::std::vector<int32_t> addresses;
    ::android::sp<cechal::IHdmiCecController> controller;
    bool flag = false;

    EXPECT_FALSE(doubleUnderTest->getState(&state).isOk())
        << "the double answered getState. It must refuse every interface method, or a case could "
           "pass on an answer it never asked for";
    EXPECT_FALSE(doubleUnderTest->getProperty(cechal::Property::HAL_CEC_VERSION, &property).isOk())
        << "the double answered getProperty";
    EXPECT_FALSE(doubleUnderTest->getLogicalAddresses(&addresses).isOk())
        << "the double answered getLogicalAddresses";
    EXPECT_FALSE(doubleUnderTest->open(nullptr, &controller).isOk())
        << "the double answered open, which would let a compatibility case accidentally establish "
           "a session";
    EXPECT_FALSE(doubleUnderTest->close(nullptr, &flag).isOk())
        << "the double answered close";
    EXPECT_FALSE(doubleUnderTest->registerEventListener(nullptr, &flag).isOk())
        << "the double answered registerEventListener";
    EXPECT_FALSE(doubleUnderTest->unregisterEventListener(nullptr, &flag).isOk())
        << "the double answered unregisterEventListener";

    EXPECT_EQ(doubleUnderTest->onAsBinder(), nullptr)
        << "IHdmiCecDefault::onAsBinder no longer returns nullptr. That return is why the fake "
           "service derives from BnHdmiCec instead of from IHdmiCecDefault - an object that cannot "
           "produce a binder can never be published nor reached through a proxy - so if this "
           "changed, revisit FakeHdmiCecService, which records that reasoning";

    // And the two members that are overridden still answer, so the double is narrow but not inert.
    EXPECT_EQ(doubleUnderTest->getInterfaceVersion(), kClientInterfaceVersion)
        << "the version override is not in effect, so every version case below is measuring the "
           "stock default rather than the value it installed";
    EXPECT_EQ(doubleUnderTest->getInterfaceHash(), std::string(kFrozenInterfaceHash))
        << "the hash override is not in effect";
}

// A null proxy is rejected before any metadata is read (halcompat.h:161-163). This is the
// arm the selection actually takes when the service manager has nothing published under
// the production name, so it is the most frequently exercised branch in production and the
// cheapest one to get wrong by assuming a null check exists further down.
/**
 * @brief A null proxy is rejected before any metadata is read.
 * @pre Runs under every invocation; needs no service and no resolved back-end.
 * @note Evidence: halcompat::isCompatible reports false for a default-constructed
 *       sp<IHdmiCec>. This is the arm the selection takes whenever nothing is published
 *       under the production name, so it is the most frequently reached of the three.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServiceIsNull) {
    const ::android::sp<cechal::IHdmiCec> absent;

    ASSERT_TRUE(absent == nullptr) << "the fixture's own premise failed: a default-constructed "
                                     "sp<IHdmiCec> must be null for this case to test anything";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(absent))
        << "a null service proxy was accepted as compatible, so the selection would go on to "
           "open() and dereference nothing";
}

// An empty hash means the hash query itself failed - a broken link rather than a
// development build - and is rejected at halcompat.h:165-167.
//
// The double is the stock IHdmiCecDefault, unmodified, because IHdmiCec.h:69-71 already
// returns "" from getInterfaceHash(). Asserting against the stock object rather than
// against an override is the point: it demonstrates that the snapshot's own default really
// is the rejected value, so nothing in this file had to arrange the condition.
/**
 * @brief An empty interface hash is a failed hash query and is never read as agreement.
 * @pre Runs under every invocation; the subject is a stock IHdmiCecDefault, so the
 *      condition is the snapshot's own default rather than something arranged here.
 * @note Evidence: the stock default's getInterfaceHash() is asserted empty first, then
 *       halcompat::isCompatible reports false for it.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenInterfaceHashIsEmpty) {
    const ::android::sp<cechal::IHdmiCec> stockDefault =
        ::android::sp<cechal::IHdmiCecDefault>::make();

    ASSERT_TRUE(stockDefault != nullptr);
    ASSERT_TRUE(stockDefault->getInterfaceHash().empty())
        << "IHdmiCecDefault no longer returns an empty interface hash, so this case is no longer "
           "exercising the empty-hash arm; give it an explicit MetadataDouble(\"\", VERSION) "
           "instead and note that IHdmiCec.h:69-71 changed";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(stockDefault))
        << "a service reporting an empty interface hash was accepted; a failed hash query must "
           "never be read as agreement";
}

// A hash of "-1" is the other spelling of a failed hash query, rejected by the same
// condition at halcompat.h:165-167. It is not interchangeable with the empty case: this is
// the exact value the L1 harness installs for invocation C (its BROKEN_INTERFACE_HASH, applied
// by publishFakeForMode()), so
// the factory-level fallback that invocation proves rests on this branch.
/**
 * @brief The "-1" spelling of a failed hash query is rejected, with a compatible version
 *        reported so the rejection can only be the hash.
 * @pre Runs under every invocation. Not interchangeable with the empty-hash case: this is
 *      the value the L1 harness installs for invocation C, so the factory-level fallback
 *      that invocation proves rests on this branch.
 * @note Evidence: the double's version is asserted equal to this client's, then
 *       halcompat::isCompatible reports false.
 * @see publishFakeForMode
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenInterfaceHashIsMinusOne) {
    const ::android::sp<cechal::IHdmiCec> broken = doubleReportingHash(kBrokenInterfaceHash);

    ASSERT_TRUE(broken != nullptr);
    ASSERT_EQ(broken->getInterfaceVersion(), kClientInterfaceVersion)
        << "the double must report a compatible version, so that a rejection can only have come "
           "from the hash";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(broken))
        << "a service reporting the \"-1\" interface hash was accepted as compatible, which would "
           "make invocation C select the AIDL back-end instead of falling back to legacy";
}

// A "notfrozen" hash is a pre-freeze development server, which makes no compatibility
// promise. halcompat.h:168-170 returns allowUnfrozen for it, and that parameter defaults to
// false (halcompat.h:159), so the default-argument call the production back-end makes from
// DriverAidlImpl::isServiceAvailable() rejects it.
//
// Both dispositions are asserted, because the default is the whole behaviour under test: a
// case that only asserted rejection could not distinguish "rejected because unfrozen
// servers are opt-in" from "rejected because the hash was not recognised at all".
/**
 * @brief A "notfrozen" development server is rejected by the defaulted call and accepted
 *        only when opted into explicitly.
 * @pre Runs under every invocation. The defaulted call is the one the production back-end
 *      makes, which is why the default is the behaviour under test.
 * @note Evidence: both dispositions are asserted on one double. Asserting rejection alone
 *       could not distinguish "rejected because unfrozen servers are opt-in" from
 *       "rejected because the hash was not recognised at all".
 * @see DriverAidlImpl::isServiceAvailable()
 */
TEST_F(DriverAidlCompatibilityTest, UnfrozenServerIsRejectedByDefaultAndAcceptedOnlyWhenOptedIn) {
    const ::android::sp<cechal::IHdmiCec> unfrozen = doubleReportingHash(kUnfrozenInterfaceHash);

    ASSERT_TRUE(unfrozen != nullptr);

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(unfrozen))
        << "an unfrozen development server was accepted by the defaulted call, which is the call "
           "the production back-end makes; a development image must be opted into explicitly";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(unfrozen, true))
        << "an unfrozen server was rejected even with allowUnfrozen=true, so the parameter no "
           "longer selects the behaviour it documents and the rejection above is happening for "
           "some other reason";
}

// The identity case, and the baseline every version case below is measured against: a
// server reporting exactly this client's compiled-against version, with the real frozen
// hash, is compatible. If this failed, every rejection below would be meaningless because
// nothing would ever be accepted.
/**
 * @brief The identity case: the real frozen hash and this client's own version are
 *        accepted.
 * @pre Runs under every invocation. This is the baseline every version case is measured
 *      against; were it to fail, no rejection below would mean anything because nothing
 *      would ever be accepted.
 * @note Evidence: the double's hash is asserted equal to the frozen constant, so the
 *       version comparison is genuinely reached, and then isCompatible reports true.
 */
TEST_F(DriverAidlCompatibilityTest, CompatibleWhenServerReportsThisClientsVersion) {
    const ::android::sp<cechal::IHdmiCec> exact =
        frozenDoubleReportingVersion(kClientInterfaceVersion);

    ASSERT_TRUE(exact != nullptr);
    ASSERT_EQ(exact->getInterfaceHash(), std::string(kFrozenInterfaceHash))
        << "the double must report the real frozen hash, or it would be rejected by the hash arm "
           "and this case would pass without ever reaching the version comparison";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(exact))
        << "a server reporting this client's own interface version was rejected, so no service "
           "could ever be selected on any platform";
}

// A newer server within the same era and major is compatible, and this case exists
// specifically to stop the opposite rule being encoded by accident.
//
// The era-0 rule at halcompat.h:112-114 accepts any server that is same-era, same-major and
// not older, so 1010 against 1000 must be accepted. A test that treated "not exactly our
// version" as incompatible would look reasonable, would pass against a fake that reports
// the exact version, and would be asserting a rule that does not exist - which would then
// reject a legitimately upgraded HAL in the field. Both ends of the accepted range are
// asserted, 1010 and 1999, so the case pins a range rather than a point.
/**
 * @brief A newer server in the same era and major is accepted, at both ends of the
 *        accepted range.
 * @pre Runs under every invocation. Exists to stop the opposite rule being encoded by
 *      accident: treating "not exactly our version" as incompatible would pass against a
 *      fake reporting the exact version while rejecting a legitimately upgraded HAL.
 * @note Evidence: 1010 and 1999 are both accepted, so the case pins the whole range
 *       rather than a point.
 */
TEST_F(DriverAidlCompatibilityTest, CompatibleWhenServerReportsNewerVersionInSameMajor) {
    const ::android::sp<cechal::IHdmiCec> newer =
        frozenDoubleReportingVersion(kNewerCompatibleVersion);
    const ::android::sp<cechal::IHdmiCec> newest =
        frozenDoubleReportingVersion(kNewestCompatibleVersion);

    ASSERT_TRUE(newer != nullptr);
    ASSERT_TRUE(newest != nullptr);
    ASSERT_GT(kNewerCompatibleVersion, kClientInterfaceVersion)
        << "the 'newer' double must actually be newer than this client for the case to mean "
           "anything";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(newer))
        << "a newer server in the same era and major was rejected. The era-0 rule accepts it "
           "(halcompat.h:112-114); rejecting it would refuse an upgraded HAL that is in fact "
           "additive and compatible";

    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(newest))
        << "the top of this client's accepted range was rejected, so the accepted set is no "
           "longer the whole of [1000, 1999] that the file block's analysis relies on";
}

// A different major generation is breaking in era 0, so it is rejected by the major
// equality conjunct at halcompat.h:113 - notably even though 2000 is numerically larger
// than this client's 1000, which is why an ordering-only comparison would wrongly accept it.
/**
 * @brief A next-major server is rejected by the major equality conjunct even though its
 *        version is numerically larger.
 * @pre Runs under every invocation.
 * @note Evidence: the cross-major value is asserted greater than the client's first, so a
 *       rejection cannot be attributed to ordering; an ordering-only comparison would
 *       wrongly accept it.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServerReportsDifferentMajor) {
    const ::android::sp<cechal::IHdmiCec> crossMajor =
        frozenDoubleReportingVersion(kCrossMajorVersion);

    ASSERT_TRUE(crossMajor != nullptr);
    ASSERT_GT(kCrossMajorVersion, kClientInterfaceVersion)
        << "this case is only meaningful while the cross-major value is numerically greater than "
           "the client's, since that is what distinguishes the major conjunct from the ordering "
           "conjunct";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(crossMajor))
        << "a server from the next major generation was accepted. In era 0 a major bump is "
           "breaking, and it is larger numerically, so accepting it means the check degenerated "
           "to an ordering comparison";
}

// A server in a later era is rejected for a third, distinct reason: the era equality
// conjunct at halcompat.h:112. This is not the same branch as the cross-major case above -
// 100000 has era 1 and major 0, so it fails on era first - and it is worth separating
// because an era-1 server is a re-frozen interface rather than a broken one.
/**
 * @brief A later-era server is rejected by the era equality conjunct, which is a third
 *        distinct reason from a null proxy and from a cross-major version.
 * @pre Runs under every invocation.
 * @note Evidence: isCompatible reports false for an era-1 value. Kept separate from the
 *       cross-major case because an era-1 server is a re-frozen interface rather than a
 *       broken one.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServerReportsDifferentEra) {
    const ::android::sp<cechal::IHdmiCec> crossEra =
        frozenDoubleReportingVersion(kCrossEraVersion);

    ASSERT_TRUE(crossEra != nullptr);

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(crossEra))
        << "an era-1 server satisfied this era-0 client. The two eras make different promises, so "
           "this must be rejected until the client itself is re-frozen into era 1 or later";
}

// The generator's default version, 1, is what a pre-freeze development server reports
// through the VERSION channel rather than through the hash. It decodes to era 0, major 0,
// so it is rejected by the cross-major conjunct - and this is the case that would be
// mislabelled as "older same-major" if the encoding were not decoded carefully, since 1 is
// indeed smaller than 1000. It is not older-same-major; there is no such value for this
// client (unreachable path (1) in the file block).
/**
 * @brief The generator's default version of 1 is rejected on major, not on ordering.
 * @pre Runs under every invocation.
 * @note Evidence: the decoded major of 1 is asserted to be 0 and asserted different from
 *       this client's, so the case is pinned to the cross-major conjunct. Mislabelling it
 *       as older-same-major is the available mistake, since 1 is indeed smaller than 1000,
 *       and no older-same-major value exists for this client.
 */
TEST_F(DriverAidlCompatibilityTest, IncompatibleWhenServerReportsUnfrozenGeneratorVersion) {
    const ::android::sp<cechal::IHdmiCec> generatorDefault =
        frozenDoubleReportingVersion(kUnfrozenGeneratorVersion);

    ASSERT_TRUE(generatorDefault != nullptr);
    ASSERT_EQ(halcompat::detail::major(kUnfrozenGeneratorVersion), 0)
        << "version 1 no longer decodes to major 0, so the reason it is rejected has changed and "
           "this case's comment - and the file block's account of it - are now wrong";
    ASSERT_NE(halcompat::detail::major(kUnfrozenGeneratorVersion),
              halcompat::detail::major(kClientInterfaceVersion))
        << "version 1 now shares this client's major, which would make it the older-same-major "
           "case the file block proves cannot exist; re-derive unreachable path (1)";

    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(generatorDefault))
        << "a pre-freeze development server reporting the generator's default version was "
           "accepted as compatible";
}

// The era-0 ordering conjunct at halcompat.h:114, reached with a different client because it
// is unreachable for this one.
//
// This is the whole of unreachable path (1) expressed as an executable assertion. For client
// 1000 the same-era, same-major servers are exactly 1000 through 1999 and every one of them
// satisfies server >= client, so the ordering conjunct can never be the conjunct that
// rejects. Calling detail::isCompatible directly with a client of 3020 and a server of 3000
// - era 0 == era 0, major 3 == major 3, and 3000 >= 3020 false - reaches it. The two
// preceding assertions establish that the other two conjuncts hold, which is what makes this
// a proof about the ordering conjunct specifically rather than just another rejection.
//
// halcompat.h:128's own static_assert(!isCompatible(3000, 2000), "era0 older rejected") does
// not reach this arm: major(3000) is 3, major(2000) is 2, so that pair is rejected on major
// and merely re-covers the cross-major case above.
/**
 * @brief The era-0 ordering conjunct rejects an older server, reached with a different
 *        client because it is unreachable for this one.
 * @pre Runs under every invocation, and calls halcompat::detail::isCompatible directly
 *      with a client of 3020 and a server of 3000 rather than going through a double.
 * @note Evidence: the pair is asserted same-era and same-major first, so the other two
 *       conjuncts hold and the rejection is attributable to ordering alone; a reversed
 *       positive control then shows the pair itself is not what is rejected.
 * @warning halcompat's own static_assert on the pair (3000, 2000) does not reach this
 *          arm - those differ on major, so it merely re-covers the cross-major case.
 */
TEST_F(DriverAidlCompatibilityTest, OlderSameMajorServerIsRejectedByTheOrderingRule) {
    ASSERT_EQ(halcompat::detail::era(kOlderSameMajorClient),
              halcompat::detail::era(kOlderSameMajorServer))
        << "the chosen pair no longer shares an era, so a rejection would come from the era "
           "conjunct and this case would stop testing the ordering conjunct";
    ASSERT_EQ(halcompat::detail::major(kOlderSameMajorClient),
              halcompat::detail::major(kOlderSameMajorServer))
        << "the chosen pair no longer shares a major, so this case has degenerated into the "
           "cross-major case - which is exactly the flaw in halcompat.h:128's own assertion";
    ASSERT_LT(kOlderSameMajorServer, kOlderSameMajorClient)
        << "the server must be older than the client for the ordering conjunct to be the one "
           "that fails";

    EXPECT_FALSE(halcompat::detail::isCompatible(kOlderSameMajorClient, kOlderSameMajorServer))
        << "an older server within the same era and major was accepted, so the ordering conjunct "
           "at halcompat.h:114 is no longer enforced";

    // Positive control on the same pair, reversed: a newer server in that major is accepted,
    // so the rejection above is attributable to the ordering and not to the pair itself.
    EXPECT_TRUE(halcompat::detail::isCompatible(kOlderSameMajorServer, kOlderSameMajorClient))
        << "the reversed pair was also rejected, so something other than ordering is rejecting "
           "both and the case above proves nothing about the ordering conjunct";
}

/**
 * @brief A metadata double whose first hash read fails and whose later reads succeed.
 *
 * This models the generated proxy's actual caching rule, which is the mechanism behind the
 * one behaviour DriverAidlImpl::isServiceAvailable() must never assume away: a metadata
 * value read successfully is cached, but a failed read is stored as -1 and retried on the
 * next call. So a metadata transaction that failed for the compatibility decision can
 * succeed for a diagnostic reread taken immediately afterwards, and the two calls then
 * disagree.
 *
 * @warning Deliberately not derived from BnHdmiCec, for the reason MetadataDouble's own
 *          warning gives: a remote Bn* answers metadata transactions from compiled-in
 *          constants, which would make the override below inert.
 */
class RecoveringMetadataDouble : public cechal::IHdmiCecDefault {
public:
    std::string getInterfaceHash() override
    {
        ++hashReads_;
        return (hashReads_ == 1) ? std::string(kBrokenInterfaceHash)
                                 : std::string(kFrozenInterfaceHash);
    }

    /**
     * @brief Always this client's own version, so only the hash can ever reject.
     *
     * @return int32_t - Always kClientInterfaceVersion.
     */
    int32_t getInterfaceVersion() override { return kClientInterfaceVersion; }

    /**
     * @brief How many times getInterfaceHash() has been called on this double
     *
     * @return int - The read count. Asserting on it is what attributes a disagreement
     *               between two compatibility calls to the modelled first-read failure
     *               rather than to some other difference between the calls.
     */
    int hashReads() const { return hashReads_; }

private:
    /** @brief Reads so far; the first is the one that reports a failed transaction. */
    int hashReads_ = 0;
};

// The counterexample that motivated the current design, kept as the model of the hazard.
//
// The hazard is that a compatibility decision and any later look at the server's metadata are
// separate transactions. The generated proxy caches a metadata value it read successfully but
// stores a failed read as "-1" and retries it, so a read that failed for the decision can
// succeed afterwards. This case is the smallest thing that exhibits it: with a first read that
// fails and a second that recovers, halcompat rejects on read 1 and read 2 reports a perfectly
// compatible server.
//
// Two diagnostic shapes are unconstructible because of it, and both are the reason the
// production rule is what it is. A diagnostic that re-read the metadata and selected a cause
// from the reread hash would fall through its whole chain - a recovered hash is neither empty,
// nor "-1", nor "notfrozen" - and announce that the interface version violated the
// era-and-major rule, when the version was never the problem. A diagnostic that asked
// halcompat a second time and then read the hash and version again separately would combine a
// predicate retry that still failed with a metadata retry that succeeded, land in its
// not-compatible arm holding compatible values, and blame the version rule all the same.
//
// The rule that forecloses both: one deciding predicate call, and then
// DriverAidlImpl::emitCompatibilityRejectionDiagnostic() takes exactly one snapshot and
// reports it as evidence about the server while explicitly declining to say which rule
// applied. This case models the transaction instability itself; the cases further down drive
// the real production emitter and assert its wording.
/**
 * @brief Two compatibility calls on one service can disagree, because the deciding read and
 *        any later read are separate transactions.
 * @pre Runs under every invocation. Uses RecoveringMetadataDouble, whose first hash read
 *      fails and whose second succeeds, which is the proxy's own retry-a-failed-read rule.
 * @note Evidence: the first call rejects, the second accepts, and the double reports
 *       exactly two hash reads - so the disagreement is attributable to the modelled
 *       first-read failure and to nothing else.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, ARecoveredMetadataReadMakesTwoCompatibilityCallsDisagree) {
    const ::android::sp<RecoveringMetadataDouble> recovering =
        ::android::sp<RecoveringMetadataDouble>::make();
    const ::android::sp<cechal::IHdmiCec> service = recovering;

    ASSERT_TRUE(service != nullptr);

    // The decision: the call whose answer actually selects the back-end.
    EXPECT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "the first compatibility call read a \"-1\" hash and accepted the service anyway, so the "
           "modelled transient failure would not have caused a fallback at all and nothing below "
           "this line would be testing the situation it claims to test";

    // The observation: taken exactly as isServiceAvailable() now takes it after a rejection.
    EXPECT_TRUE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "the second compatibility call did not recover, so this double no longer models the "
           "proxy's retry-a-failed-read rule and cannot guard the causal wording it exists to "
           "guard";

    EXPECT_EQ(recovering->hashReads(), 2)
        << "the two compatibility calls did not perform exactly two hash reads, so the "
           "disagreement above came from something other than the modelled first-read failure";
}

/**
 * @brief A compatibility double whose metadata transaction fails outright.
 *
 * Models the one remaining shape the production diagnostic has to survive: the snapshot read
 * itself failing. Over binder that is a dead or refusing service; here it is an exception from
 * a direct call, which is what the emitter's own catch(...) sees either way. The point of the
 * case that uses it is that a failed observation must cost a less specific message and must
 * not be able to disturb the cause the caller already recorded.
 */
class ThrowingMetadataDouble : public cechal::IHdmiCecDefault {
public:
    std::string getInterfaceHash() override
    {
        throw std::runtime_error("simulated metadata transaction failure");
    }

    /**
     * @brief Always this client's own version; it is never reached, since the hash throws.
     *
     * @return int32_t - Always kClientInterfaceVersion.
     */
    int32_t getInterfaceVersion() override { return kClientInterfaceVersion; }
};

/**
 * @brief A double whose hash reads fail twice and succeed from the third read onward.
 *
 * Models delayed recovery: a diagnostic that asked the deciding predicate a second time and
 * then read the hash and version again separately could combine a second read that still
 * failed with a third read that succeeded, and so report compatible values from its
 * not-compatible arm. The single-snapshot rule
 * DriverAidlImpl::emitCompatibilityRejectionDiagnostic() follows makes that combination
 * unconstructible, and this double is what holds that rule to account.
 *
 * @warning Not derived from BnHdmiCec, for the reason MetadataDouble's warning gives.
 */
class DelayedRecoveryMetadataDouble : public cechal::IHdmiCecDefault {
public:
    std::string getInterfaceHash() override
    {
        ++hashReads_;
        return (hashReads_ <= 2) ? std::string(kBrokenInterfaceHash)
                                 : std::string(kFrozenInterfaceHash);
    }

    /**
     * @brief Always this client's own version, so only the hash can ever reject.
     *
     * @return int32_t - Always kClientInterfaceVersion.
     */
    int32_t getInterfaceVersion() override { return kClientInterfaceVersion; }

    /**
     * @brief How many times getInterfaceHash() has been called on this double
     *
     * @return int - The read count, which is what pins the recovery to the third read
     *               rather than to an earlier one.
     */
    int hashReads() const { return hashReads_; }

private:
    /** @brief Reads so far; the first two report a failed transaction and the rest recover. */
    int hashReads_ = 0;
};

// The observation helpers are tested directly, and that is the only way to cover them here.
//
// isServiceAvailable() reaches its compatibility stage only after isBinderPreflightOk() passes
// on the default driver path, and there is no binder transport on this host, so the production
// arm that emits this wording is unreachable in this process -- it belongs to invocation C,
// which is deferred. The two helpers the message is built from are public statics for exactly
// this reason, so the wording itself is covered rather than assumed.
/**
 * @brief Each observed hash category is described distinguishably, and no description makes
 *        a causal claim.
 * @pre Runs under every invocation; calls the production classifier directly, so it needs
 *      no service and no resolved back-end.
 * @note Evidence: the four categories each yield their own phrase, and every phrase is
 *       asserted free of the word "because" - which is the whole point, since a
 *       post-decision read is not entitled to say why the decision went the way it did.
 * @see DriverAidlImpl::describeObservedInterfaceHash()
 */
TEST_F(DriverAidlCompatibilityTest, TheObservedHashDescriptionNamesEachCategoryWithoutClaimingCause) {
    const std::string empty;
    const std::string minusOne(kBrokenInterfaceHash);
    const std::string unfrozen(kUnfrozenInterfaceHash);
    const std::string frozen(kFrozenInterfaceHash);

    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(empty), ::testing::HasSubstr("FAILS"))
        << "an empty observed hash is not described as a failed transaction, so a reader cannot "
           "tell it apart from a value a working server reported";
    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(minusOne), ::testing::HasSubstr("FAILS"))
        << "the \"-1\" marker is not described as a failed transaction";
    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(unfrozen), ::testing::HasSubstr("UNFROZEN"))
        << "the \"notfrozen\" marker is not described as an unfrozen development build";
    EXPECT_THAT(DriverAidlImpl::describeObservedInterfaceHash(frozen), ::testing::HasSubstr("frozen release digest"))
        << "a real frozen digest is not described as one";

    // The whole point of these phrases: none of them asserts what caused the rejection.
    for (const std::string &value : { empty, minusOne, unfrozen, frozen }) {
        const std::string phrase = DriverAidlImpl::describeObservedInterfaceHash(value);
        EXPECT_THAT(phrase, ::testing::Not(::testing::HasSubstr("because")))
            << "the description of [" << value << "] makes a causal claim, which no post-decision "
               "read is entitled to make";
    }
}

// A snapshot that would itself be accepted must never be blamed on the VERSION rule.
// This is the invariant the production message depends on: when this predicate is true the
// message says the metadata changed or recovered; when it is false the message names all three
// possible causes and attributes none.
/**
 * @brief A snapshot that would itself be accepted is reported as acceptable, so the version
 *        rule can never be blamed for it.
 * @pre Runs under every invocation; calls the production predicate directly.
 * @note Evidence: the frozen digest with this client's own version is accepted, and so is
 *       the same digest with a newer version in the same era and major - a helper that
 *       treated "not equal" as unacceptable would encode a rule that does not exist.
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
 */
TEST_F(DriverAidlCompatibilityTest, AnObservedSnapshotThatWouldBeAcceptedReportsChangedMetadata) {
    EXPECT_TRUE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                    std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kClientInterfaceVersion))
        << "a snapshot carrying the frozen digest and this client's own version was reported as "
           "not acceptable, so production would blame the version rule for metadata that is in "
           "fact perfectly compatible -- the exact false diagnosis this helper exists to prevent";

    // Newer within the same era and major is accepted by the real rule, so the observation
    // must be too. A helper that treated "not equal" as unacceptable would encode a false rule.
    EXPECT_TRUE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                    std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kNewerCompatibleVersion))
        << "a newer server within the same era and major was reported as not acceptable, which "
           "contradicts halcompat::detail::isCompatible()";
}

/**
 * @brief Neither half of an observed snapshot can be waved through: a bad hash or an
 *        incompatible version each makes it unacceptable on its own.
 * @pre Runs under every invocation; calls the production predicate directly.
 * @note Evidence: each non-digest hash is paired with a compatible version so only the hash
 *       can reject, and the frozen digest is paired with an older-same-major, a cross-major
 *       and a cross-era version so only the version can - which is what shows the version
 *       half delegates to halcompat rather than reimplementing it.
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
 */
TEST_F(DriverAidlCompatibilityTest, AnObservedSnapshotIsNotAcceptedForABadHashOrAnIncompatibleVersion) {
    // Each non-digest hash, with a compatible version, so only the hash can be the reason.
    for (const char *bad : { "", kBrokenInterfaceHash, kUnfrozenInterfaceHash }) {
        EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                         std::string(bad), kClientInterfaceVersion, kClientInterfaceVersion))
            << "the observed hash [" << bad << "] was reported acceptable even though halcompat "
               "rejects it, so the observation would contradict the decision it accompanies";
    }

    // A frozen digest with an incompatible version: the version half must do the rejecting,
    // and it is the real rule that decides, not a copy of it.
    EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                     std::string(kFrozenInterfaceHash), kOlderSameMajorClient, kOlderSameMajorServer))
        << "an older server within the same major was reported acceptable, so the version half of "
           "the observation is not delegating to halcompat::detail::isCompatible()";
    EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                     std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kCrossMajorVersion))
        << "a cross-major server version was reported acceptable";
    EXPECT_FALSE(DriverAidlImpl::observedMetadataWouldBeAccepted(
                     std::string(kFrozenInterfaceHash), kClientInterfaceVersion, kCrossEraVersion))
        << "a cross-era server version was reported acceptable";
}

// The verifier's exact sequence, reproduced: fail, fail, then succeed.
//
// The hazard was: decision reads and fails; a second predicate call retries and fails, so the
// code concludes "still not compatible"; a third, separate read then succeeds and returns
// compatible values; the classification runs on those and blames the version rule. Production
// now takes one snapshot and asks this predicate about that snapshot alone, so the two halves
// can no longer be mixed -- and whichever read the snapshot lands on, the reported wording is a
// property of that one pair.
// ================================================================================
// the production emitter, driven directly, with its output captured.
//
// Everything above this line tests the observation helpers. That leaves the thing a reviewer
// actually cares about untested: the wording DriverAidlImpl emits. The cases below close that
// by calling DriverAidlImpl::emitCompatibilityRejectionDiagnostic() -- the one and only
// production emitter, called from isServiceAvailable() and from nowhere else -- and asserting
// on the text it writes. There is no second copy of the message here to drift from the first.
//
// Why the emitter rather than isServiceAvailable() itself: that function reaches its
// compatibility stage only after isBinderPreflightOk() passes on the default driver path, and
// this host has no binder transport, so the stage is unreachable in this process. It belongs to
// invocation C. Driving the emitter is what makes the wording verifiable here; invocation C
// remains what proves the factory reaches it.
// ================================================================================

/**
 * @brief The production diagnostic names all three rejection rules and claims none of them.
 *
 * @pre Runs under every invocation. Drives the production emitter directly, because
 *      isServiceAvailable() reaches its compatibility stage only after the preflight passes
 *      on the default driver path, and this host has no binder transport.
 * @note Evidence: the captured text is asserted to mention each of the three rules and to
 *       attribute none of them. There is no second copy of the wording here to drift from
 *       the first.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, TheProductionDiagnosticNamesAllThreeRulesAndClaimsNoneOfThem) {
    // A genuine rejection: frozen hash, cross-major version. halcompat refuses it.
    const ::android::sp<cechal::IHdmiCec> service =
        frozenDoubleReportingVersion(kCrossMajorVersion);
    ASSERT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "this double was accepted, so it does not model a rejection and the diagnostic "
           "under test would never be reached in production for it";

    std::string emitted;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        emitted = capture.read();
    }

    EXPECT_THAT(emitted, ::testing::HasSubstr("REJECTED AS NOT COMPATIBLE"))
        << "the diagnostic did not report the rejection at all";
    EXPECT_THAT(emitted, ::testing::HasSubstr("WHICH ONE APPLIED IS NOT REPORTED HERE"))
        << "the diagnostic no longer declines to name the cause. That refusal is the whole "
           "correction: the metadata halcompat decided on cannot be recovered, so any claim "
           "about which rule applied is a guess presented as a finding";
    // All three possibilities are named as possibilities.
    EXPECT_THAT(emitted, ::testing::HasSubstr("unreadable interface hash"));
    EXPECT_THAT(emitted, ::testing::HasSubstr("UNFROZEN"));
    EXPECT_THAT(emitted, ::testing::HasSubstr("era and major"));
    // And the snapshot is labelled as evidence rather than as the reason.
    EXPECT_THAT(emitted, ::testing::HasSubstr("OBSERVED AFTER the decision"))
        << "the observed values are no longer labelled as post-decision, so a reader would "
           "take them for the values the decision used";
    EXPECT_THAT(emitted, ::testing::HasSubstr("halcompat::isCompatible(), the SOLE decision"))
        << "the diagnostic no longer states that halcompat owns the decision";
    // The observed values must actually reach the log. This is not a redundant assertion: the
    // first version of this message was ~900 characters and CCEC_LOG truncates at 499
    // (MAX_LOG_BUFF in ccec/src/Util.cpp), so the qualification survived and every value the
    // diagnostic exists to report was cut off. Capturing the output is what exposed it.
    EXPECT_THAT(emitted, ::testing::HasSubstr("interface hash ["))
        << "the observed interface hash never reached the log -- check the 499-byte CCEC_LOG "
           "limit before assuming the value was not computed";
    EXPECT_THAT(emitted, ::testing::HasSubstr("server interface version 2000 against this client's 1000"))
        << "the observed and client versions never reached the log";
    EXPECT_THAT(emitted, ::testing::HasSubstr("legacy back-end is selected"))
        << "the outcome sentence never reached the log";
}

/**
 * @brief Recovered metadata is reported as changed metadata, never blamed on the version rule.
 *
 * @pre Runs under every invocation, driving the production emitter directly against
 *      RecoveringMetadataDouble.
 * @note Evidence: the captured text names the changed-or-recovered metadata and does not
 *       name the version rule, which is the misattribution a re-reading diagnostic would
 *       make.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, TheProductionDiagnosticBlamesChangedMetadataNotTheVersionRule) {
    const ::android::sp<RecoveringMetadataDouble> recovering =
        ::android::sp<RecoveringMetadataDouble>::make();
    const ::android::sp<cechal::IHdmiCec> service = recovering;

    // Read 1 is the decision, and it fails on a "-1" hash -- so a fallback happens.
    ASSERT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service))
        << "the deciding call accepted a \"-1\" hash, so no fallback would occur and this case "
           "would not be modelling a rejection at all";

    // The emitter's single snapshot is therefore read 2, which recovers. This is exactly the
    // sequence production performs -- one decision, then one snapshot -- and it is the sequence
    // in which a diagnostic that re-read or re-asked would misreport the cause.
    std::string emitted;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        emitted = capture.read();
    }
    ASSERT_EQ(recovering->hashReads(), 2)
        << "the emitter did not take exactly one snapshot after the deciding read. More than "
           "one read is the defect this design removed: a predicate result combined with "
           "separately retried values is what let compatible metadata be blamed on the "
           "version rule";

    EXPECT_THAT(emitted, ::testing::HasSubstr("WOULD itself have been accepted"))
        << "a snapshot that would have been accepted was not reported as such, so the reader "
           "is given no signal that the metadata changed after the decision";
    EXPECT_THAT(emitted, ::testing::HasSubstr("INTERMITTENT METADATA TRANSACTION"))
        << "the diagnostic did not point at an intermittent transaction, which is the only "
           "honest reading when the post-decision snapshot is itself compatible";
    EXPECT_THAT(emitted, ::testing::HasSubstr("frozen release digest"))
        << "the recovered hash was not described as a frozen digest, so the observation "
           "misreports what the server answered";
    // The regression guard proper: the compatible observation must not be presented as a
    // version failure. The message names the version rule as one of three possibilities,
    // which is why this asserts on the causal claim rather than on the phrase.
    EXPECT_THAT(emitted, ::testing::Not(::testing::HasSubstr("because the interface version")))
        << "the diagnostic asserted a version cause for a server whose observed version is "
           "compatible, which is a statement the single-snapshot rule exists to make "
           "unconstructible";
    EXPECT_THAT(emitted, ::testing::HasSubstr("WHICH ONE APPLIED IS NOT REPORTED HERE"))
        << "the non-causal qualification was dropped on the recovery path";
}

/**
 * @brief A recovery that arrives later than the snapshot is still never blamed on the version.
 *
 * @pre Runs under every invocation, driving the production emitter directly against
 *      DelayedRecoveryMetadataDouble, whose hash recovers only on the third read.
 * @note Evidence: the captured text is the same changed-or-recovered wording as for
 *       immediate recovery, so which read the single snapshot lands on cannot change the
 *       rule the message attributes.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, DelayedMetadataRecoveryOnAThirdReadCannotBeBlamedOnTheVersionRule) {
    const ::android::sp<DelayedRecoveryMetadataDouble> delayed =
        ::android::sp<DelayedRecoveryMetadataDouble>::make();
    const ::android::sp<cechal::IHdmiCec> service = delayed;

    // Decision on read 1: fails. Emitter snapshot on read 2: still fails. So the emitter sees
    // a broken hash and must say so -- reporting the hash, not the version.
    ASSERT_FALSE(halcompat::isCompatible<cechal::IHdmiCec>(service));
    std::string firstEmission;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        firstEmission = capture.read();
    }
    ASSERT_EQ(delayed->hashReads(), 2)
        << "the decision and one snapshot should have consumed exactly two reads";
    EXPECT_THAT(firstEmission, ::testing::HasSubstr("FAILS"))
        << "a \"-1\" snapshot was not described as a failed metadata read";
    EXPECT_THAT(firstEmission, ::testing::Not(::testing::HasSubstr("WOULD itself have been accepted")))
        << "a snapshot that is still broken was reported as one that would have been accepted";

    // Second decision on read 3 -- which recovers, so halcompat now accepts. That is the
    // honest outcome and it is asserted rather than worked around: the point of the case is
    // that no arrangement of reads makes the diagnostic blame the version rule.
    const bool secondVerdict = halcompat::isCompatible<cechal::IHdmiCec>(service);
    ASSERT_TRUE(secondVerdict)
        << "the third read did not recover, so this double is not modelling DELAYED recovery";

    // And the recovered values, put through the emitter, are reported as changed metadata.
    std::string secondEmission;
    {
        StdoutCapture capture;
        DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec"));
        secondEmission = capture.read();
    }
    EXPECT_THAT(secondEmission, ::testing::HasSubstr("WOULD itself have been accepted"))
        << "recovered metadata was not reported as changed or recovered";
    EXPECT_THAT(secondEmission, ::testing::Not(::testing::HasSubstr("because the interface version")))
        << "a version cause was asserted for a compatible observation";
}

/**
 * @brief A failed observation costs a less specific message and cannot relabel the cause.
 *
 * @pre Runs under every invocation, driving the production emitter directly against
 *      ThrowingMetadataDouble, whose metadata transaction fails outright.
 * @note Evidence: the captured text is the fallback message rather than a specific one, and
 *       the cause the caller already recorded is unchanged - a failed post-decision read
 *       must cost detail and must never be able to overwrite the decision's own reason.
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 */
TEST_F(DriverAidlCompatibilityTest, AFailedObservationEmitsTheFallbackMessageAndPreservesTheCause) {
    const ::android::sp<cechal::IHdmiCec> service =
        ::android::sp<ThrowingMetadataDouble>::make();

    // The cause the caller records lives on the instance. The emitter is static, so it has no
    // `this` and cannot reach availabilityReason at all -- the preservation below is structural
    // rather than a promise. This asserts the observable half of that guarantee.
    DriverAidlImpl backEnd;
    const std::string reasonBefore(backEnd.unavailabilityReason() != NULL
                                       ? backEnd.unavailabilityReason()
                                       : "");

    std::string emitted;
    {
        StdoutCapture capture;
        // Must not propagate: a diagnostic that threw would reach isServiceAvailable()'s own
        // handler, which records REASON_QUERY_FAILED -- turning an established compatibility
        // rejection into an unexplained query failure.
        ASSERT_NO_THROW(
            DriverAidlImpl::emitCompatibilityRejectionDiagnostic(service, std::string("HdmiCec")));
        emitted = capture.read();
    }

    EXPECT_THAT(emitted, ::testing::HasSubstr("could not be read back afterwards"))
        << "a failed observation did not produce the reduced message";
    EXPECT_THAT(emitted, ::testing::HasSubstr("its recorded cause unchanged"))
        << "the reduced message no longer states that the recorded cause is unaffected, which "
           "is the property that stops a failed diagnostic being read as a different fault";
    EXPECT_THAT(emitted, ::testing::HasSubstr("REJECTED AS NOT COMPATIBLE"))
        << "the reduced message dropped the rejection itself and reports only the read failure";
    EXPECT_THAT(emitted, ::testing::Not(::testing::HasSubstr("OBSERVED AFTER the decision")))
        << "the detailed observation wording was emitted even though no snapshot was obtained";

    const std::string reasonAfter(backEnd.unavailabilityReason() != NULL
                                      ? backEnd.unavailabilityReason()
                                      : "");
    EXPECT_EQ(reasonBefore, reasonAfter)
        << "the emitter changed an instance's recorded unavailability reason. It is static and "
           "must be incapable of that; if this ever fails, the emitter has grown access to "
           "instance state and the preserve-cause guarantee is no longer structural";
}

// The four cases below are about the fake's own interface-metadata controls rather than about
// halcompat, and they are here rather than in a fixture of their own for a concrete reason: a
// new fixture would sit outside run_coverage.sh's classification constants, and a fixture in
// none of those lists is what breaks its selected-plus-excluded reconciliation. This one is
// already classified CONTRACT_ANY_BACKEND_SUITES, which is exactly right for cases that need
// no registered service, no binder driver and no resolved back-end.
//
// What they establish, and why it is not ceremony. Each fake's two metadata getters trace when
// - and only when - the value they report differs from their own compiled-in constant. Those
// three traces (the controller's version and hash getters, and the service's version getter)
// had no caller that could make them fire, because the setters that install a divergent value
// did not exist: the fake carried one hash setter, on the service. The controls now exist
// because AAP 0.4.1 requires the fake to offer overridable getInterfaceHash/getInterfaceVersion,
// and these cases are what make each of the three a branch that is reached rather than one that
// is merely reachable. A control nothing exercises is indistinguishable from a control that does
// not work.
//
// Each case asserts three things per class, in this order: the getter reports the compiled-in
// constant and stays quiet before any override, which pins the trace's false arm and is what
// stops an ordinary run being flooded; the getter then reports exactly what the setter installed
// and the trace names it, which is the true arm; and reset() puts both values back and returns
// both getters to silence. The setters are traced too, and asserted, because a control that
// changed a value without saying so leaves a captured log in which a later divergence line has
// no cause anywhere above it.
//
// Never registered, in any of the four. Registration calls defaultServiceManager(), which opens
// the binder driver and aborts on this host, so a case that registered would take the whole
// binary down rather than fail.
/**
 * @brief The fake service reports the interface metadata installed on it, and says when it
 *        diverges.
 *
 * @pre Runs under every invocation. The fake is constructed locally and never registered, so
 *      nothing here touches the service manager or the binder driver.
 * @note Evidence: before any override both getters report the compiled-in constants and print
 *       no divergence line; after each setter the matching getter reports the installed value
 *       and the divergence line names it. The version half is what drives the service's
 *       previously unreachable version-divergence trace - unreachable because no
 *       setInterfaceVersion existed to install a differing value. A wrong implementation is a
 *       setter that stores nowhere the getter reads (the getter would keep reporting the
 *       constant), or a getter that reports a literal rather than the member (the same
 *       symptom, with reset() also silently a no-op).
 * @see FakeHdmiCecService::setInterfaceVersion(), FakeHdmiCecService::setInterfaceHash()
 */
TEST_F(DriverAidlCompatibilityTest, TheServiceFakeReportsTheInterfaceMetadataInstalledOnIt) {
    const ::android::sp<FakeHdmiCecService> fake = ::android::sp<FakeHdmiCecService>::make();
    ASSERT_TRUE(fake != nullptr);

    // Defaults first, under capture, so the quiet arm of both traces is pinned rather than
    // assumed. A fake that traced unconditionally would print here.
    int32_t defaultVersion = 0;
    std::string defaultHash;
    std::string defaultOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence traces cannot be read; "
               "failing rather than asserting against an empty capture";

        defaultVersion = fake->getInterfaceVersion();
        defaultHash = fake->getInterfaceHash();

        defaultOutput = capture.read();
    }

    EXPECT_EQ(defaultVersion, kClientInterfaceVersion)
        << "an unconfigured fake service did not report the compiled-in interface version, so "
           "every AIDL-selected invocation would be driving a service the middleware must "
           "refuse - and the compatible case would have no way to occur";
    EXPECT_EQ(defaultHash, std::string(kFrozenInterfaceHash))
        << "an unconfigured fake service did not report the compiled-in interface hash";
    EXPECT_THAT(defaultOutput, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a fake reporting its own compiled-in metadata still printed a divergence line. The "
           "trace is conditional precisely so that the one line a run does print marks the "
           "moment the fake was made to diverge";

    // The hash half. This is the control the L1 harness's incompatible mode uses, so the value
    // installed here is the value that mode installs.
    int32_t versionAfterHash = 0;
    std::string installedHash;
    std::string hashOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        fake->setInterfaceHash(std::string(kBrokenInterfaceHash));
        installedHash = fake->getInterfaceHash();
        versionAfterHash = fake->getInterfaceVersion();

        hashOutput = capture.read();
    }

    EXPECT_EQ(installedHash, std::string(kBrokenInterfaceHash))
        << "the fake service did not report the hash installed on it, so invocation C's "
           "present-but-incompatible arm could not be arranged at all";
    EXPECT_EQ(versionAfterHash, kClientInterfaceVersion)
        << "installing a hash disturbed the reported version. The two are separate members "
           "deliberately: a case that installs one must be able to rely on the other";
    EXPECT_THAT(hashOutput, ::testing::HasSubstr("setInterfaceHash] Hash set from"))
        << "the hash setter installed a value without tracing it, so a captured log would carry "
           "a divergence line below with no cause anywhere above it";
    EXPECT_THAT(hashOutput, ::testing::HasSubstr("getInterfaceHash] Reporting overridden hash"))
        << "the hash getter reported a divergent value without tracing it";

    // The version half: the previously unreachable trace.
    int32_t installedVersion = 0;
    std::string hashAfterVersion;
    std::string versionOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        fake->setInterfaceVersion(kDivergentReportedVersion);
        installedVersion = fake->getInterfaceVersion();
        hashAfterVersion = fake->getInterfaceHash();

        versionOutput = capture.read();
    }

    EXPECT_EQ(installedVersion, kDivergentReportedVersion)
        << "the fake service did not report the interface version installed on it. AAP 0.4.1 "
           "requires this control, and without it the service's version-divergence trace has no "
           "caller that can reach it";
    EXPECT_EQ(hashAfterVersion, std::string(kBrokenInterfaceHash))
        << "installing a version reverted or disturbed the hash installed earlier";
    EXPECT_THAT(versionOutput, ::testing::HasSubstr("setInterfaceVersion] Version set from"))
        << "the version setter installed a value without tracing it";
    // The value is spelled from the constant rather than as a literal, so changing the constant
    // cannot leave this expectation matching a number nothing installs any more.
    EXPECT_THAT(versionOutput,
                ::testing::HasSubstr("getInterfaceVersion] Reporting overridden version: "
                                     + std::to_string(kDivergentReportedVersion)))
        << "the service's version-divergence trace did not fire for a divergent version. That "
           "branch is the one this case exists to drive, and a getter that reported the "
           "installed value without tracing it would leave it unreached";
}

/**
 * @brief The fake service restores both metadata values on reset(), so an override cannot
 *        outlive the case that installed it.
 *
 * @pre Runs under every invocation, on a locally constructed and never registered fake.
 * @note Evidence: both values are installed and observed changed, then reset() is called and
 *       both getters report the compiled-in constants again and print no divergence line. This
 *       matters beyond tidiness: the harness registers one fake for the whole process because
 *       the selection resolves once, so a metadata value that survived a reset would decide the
 *       outcome of every later case. A wrong implementation is a reset() that restores the hash
 *       and forgets the version, which would leave the version-divergence trace firing for the
 *       remainder of the run with nothing naming the cause.
 * @see FakeHdmiCecService::reset()
 */
TEST_F(DriverAidlCompatibilityTest, TheServiceFakeRestoresBothMetadataValuesOnReset) {
    const ::android::sp<FakeHdmiCecService> fake = ::android::sp<FakeHdmiCecService>::make();
    ASSERT_TRUE(fake != nullptr);

    fake->setInterfaceHash(std::string(kDivergentReportedHash));
    fake->setInterfaceVersion(kDivergentReportedVersion);

    // The premise: both values really did change. Without this the case could pass on a fake
    // whose setters never worked.
    ASSERT_EQ(fake->getInterfaceHash(), std::string(kDivergentReportedHash))
        << "the hash was not installed, so this case would assert restoration of a value that "
           "was never disturbed";
    ASSERT_EQ(fake->getInterfaceVersion(), kDivergentReportedVersion)
        << "the version was not installed, for the same reason";

    fake->reset();

    int32_t restoredVersion = 0;
    std::string restoredHash;
    std::string afterReset;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        restoredVersion = fake->getInterfaceVersion();
        restoredHash = fake->getInterfaceHash();

        afterReset = capture.read();
    }

    EXPECT_EQ(restoredHash, std::string(kFrozenInterfaceHash))
        << "reset() did not restore the compiled-in interface hash, so a case that made the "
           "service incompatible would leave every later case running against a service the "
           "middleware refuses";
    EXPECT_EQ(restoredVersion, kClientInterfaceVersion)
        << "reset() did not restore the compiled-in interface version. The hash and the version "
           "are restored by the same critical section, so one surviving means the pair has "
           "drifted apart";
    EXPECT_THAT(afterReset, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a getter still reported divergence after reset(), which means it is reporting a "
           "value reset() did not reach";
}

/**
 * @brief The fake controller reports the interface metadata installed on it, and says when it
 *        diverges.
 *
 * @pre Runs under every invocation, on a controller constructed directly rather than obtained
 *      from a service: the controls are the controller's own, and nothing about them needs a
 *      session. Never registered - a controller is never published under a name in any case,
 *      since a client obtains one from open()'s out-parameter.
 * @note Evidence: both getters report the compiled-in constants and stay quiet before any
 *       override, then each reports the value installed on it and traces the divergence. These
 *       two are the controller's previously unreachable traces - unreachable because the class
 *       carried no metadata setter at all. A wrong implementation is a setter that writes the
 *       service's member instead of the controller's, which would leave both traces here
 *       unreached while appearing to work.
 * @note What this does NOT establish, stated so the case is not over-read: the middleware's
 *       compatibility check reads the service interface's metadata alone, so a divergent value
 *       installed here decides no selection outcome and this case makes no compatibility claim.
 *       Its subject is the control and the fake's own reporting of it.
 * @see FakeHdmiCecController::setInterfaceVersion(), FakeHdmiCecController::setInterfaceHash()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeReportsTheInterfaceMetadataInstalledOnIt) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    int32_t defaultVersion = 0;
    std::string defaultHash;
    std::string defaultOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence traces cannot be read; "
               "failing rather than asserting against an empty capture";

        defaultVersion = controller->getInterfaceVersion();
        defaultHash = controller->getInterfaceHash();

        defaultOutput = capture.read();
    }

    EXPECT_EQ(defaultVersion, kControllerClientInterfaceVersion)
        << "an unconfigured fake controller did not report the compiled-in controller interface "
           "version";
    EXPECT_EQ(defaultHash, std::string(kControllerFrozenInterfaceHash))
        << "an unconfigured fake controller did not report the compiled-in controller interface "
           "hash";
    EXPECT_THAT(defaultOutput, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a controller reporting its own compiled-in metadata still printed a divergence line";

    int32_t installedVersion = 0;
    std::string installedHash;
    std::string divergentOutput;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        controller->setInterfaceHash(std::string(kDivergentReportedHash));
        controller->setInterfaceVersion(kDivergentReportedVersion);

        installedHash = controller->getInterfaceHash();
        installedVersion = controller->getInterfaceVersion();

        divergentOutput = capture.read();
    }

    EXPECT_EQ(installedHash, std::string(kDivergentReportedHash))
        << "the fake controller did not report the hash installed on it. AAP 0.4.1 requires the "
           "fake to offer overridable metadata, and this class had no metadata setter at all";
    EXPECT_EQ(installedVersion, kDivergentReportedVersion)
        << "the fake controller did not report the version installed on it";
    EXPECT_THAT(divergentOutput, ::testing::HasSubstr("setInterfaceHash] Hash set from"))
        << "the controller's hash setter installed a value without tracing it";
    EXPECT_THAT(divergentOutput, ::testing::HasSubstr("setInterfaceVersion] Version set from"))
        << "the controller's version setter installed a value without tracing it";
    EXPECT_THAT(divergentOutput,
                ::testing::HasSubstr("FakeHdmiCecController::getInterfaceHash] Reporting "
                                     "overridden hash"))
        << "the controller's hash-divergence trace did not fire for a divergent hash. That "
           "branch is one of the two this case exists to drive";
    EXPECT_THAT(divergentOutput,
                ::testing::HasSubstr("FakeHdmiCecController::getInterfaceVersion] Reporting "
                                     "overridden version: "
                                     + std::to_string(kDivergentReportedVersion)))
        << "the controller's version-divergence trace did not fire for a divergent version. That "
           "branch is the other one, and the class-qualified prefix is asserted so that the "
           "service's identically worded line cannot satisfy this expectation";
}

/**
 * @brief The fake controller restores both metadata values on reset().
 *
 * @pre Runs under every invocation, on a locally constructed controller.
 * @note Evidence: both values are installed and observed changed, then reset() is called and
 *       both getters report the compiled-in constants again and print no divergence line. The
 *       controller is reset separately from its service - the service's reset() deliberately
 *       does not touch the controller it owns - so a case that configures both must reset both,
 *       and a reset that missed either value would leak an override into the next case through
 *       the one long-lived controller the registered fake hands out.
 * @see FakeHdmiCecController::reset(), FakeHdmiCecService::reset()
 */
TEST_F(DriverAidlCompatibilityTest, TheControllerFakeRestoresBothMetadataValuesOnReset) {
    const ::android::sp<FakeHdmiCecController> controller =
        ::android::sp<FakeHdmiCecController>::make();
    ASSERT_TRUE(controller != nullptr);

    controller->setInterfaceHash(std::string(kDivergentReportedHash));
    controller->setInterfaceVersion(kDivergentReportedVersion);

    ASSERT_EQ(controller->getInterfaceHash(), std::string(kDivergentReportedHash))
        << "the hash was not installed, so this case would assert restoration of a value that "
           "was never disturbed";
    ASSERT_EQ(controller->getInterfaceVersion(), kDivergentReportedVersion)
        << "the version was not installed, for the same reason";

    controller->reset();

    int32_t restoredVersion = 0;
    std::string restoredHash;
    std::string afterReset;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid());

        restoredVersion = controller->getInterfaceVersion();
        restoredHash = controller->getInterfaceHash();

        afterReset = capture.read();
    }

    EXPECT_EQ(restoredHash, std::string(kControllerFrozenInterfaceHash))
        << "reset() did not restore the controller's compiled-in interface hash";
    EXPECT_EQ(restoredVersion, kControllerClientInterfaceVersion)
        << "reset() did not restore the controller's compiled-in interface version, so an "
           "override would outlive the case that installed it";
    EXPECT_THAT(afterReset, ::testing::Not(::testing::HasSubstr("overridden")))
        << "a getter still reported divergence after reset()";
}

/**
 * @brief The binder preflight's negative arms, by direct call.
 *
 * DriverAidlImpl::isBinderPreflightOk() is a public static member taking the binder driver
 * path and a context-manager timeout, both defaulted on its declaration in
 * ccec/src/DriverAidlImpl.hpp to DEFAULT_BINDER_DRIVER_PATH and
 * DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS - named by symbol rather than by line, for the reason
 * the closing note of this file gives about citations into files it does not own. Taking
 * the path as a parameter is exactly what makes these cases possible: a nonexistent path
 * and a non-binder node can be probed without rendering a runner's real /dev/binder
 * unusable, and nothing here writes to, chmods or unlinks the real node. The member's own
 * documentation records that it is public so that a test translation unit may call it; a
 * file-local function in the .cpp would not have been callable at all.
 *
 * Why this predicate must exist at all, since a test of a guard reads as ceremony
 * otherwise. On the pinned binder stack, reaching the service manager unguarded is unsafe
 * in two independent ways: a missing or protocol-mismatched driver node is fatal rather
 * than an error return, because the pin extends libbinder's failed-driver
 * LOG_ALWAYS_FATAL_IF to plain Linux; and a working driver with no running servicemanager
 * blocks indefinitely, because obtaining an IServiceManager polls until binder handle 0
 * resolves with no bound of its own. Either one defeats the absolute requirement that a
 * supported SOC without an AIDL HAL reach the legacy back-end. On a host with no kernel
 * binder support - which is the common case for this suite - the first arm below is the one
 * standing between a plain ./run_L1Tests and a SIGABRT that would take all of the other
 * cases in this binary down with it.
 *
 * No driver precondition is established, for the same reason as the compatibility fixture:
 * nothing here touches the process-global driver, the HAL mock or the shared library.
 *
 * Self-sufficiency. Each case supplies its own path, and the one case that needs a real
 * filesystem node creates it, uses it and unlinks it within the case body. No shared state
 * is read or written, so every case passes alone and in any order.
 */
class DriverAidlPreflightTest : public ::testing::Test {
};

// An empty path is refused outright, before anything is opened, by the first decision point
// in DriverAidlImpl::isBinderPreflightOk(). This is the arm a misconfigured deployment reaches - a
// path variable that resolved to nothing - and refusing it explicitly is what stops
// ::open("") being attempted and its errno being reported as though a node had been found.
/**
 * @brief An empty driver path is refused outright, before anything is opened.
 * @pre Runs under every invocation and needs no back-end, no service and no filesystem
 *      node: the predicate is a public static called directly.
 * @note Evidence: the predicate reports false for a default-constructed path. Refusing
 *       explicitly is what stops ::open("") being attempted and its errno reported as
 *       though a node had been found.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, DeclinesAnEmptyDriverPath) {
    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(std::string()))
        << "the preflight accepted an empty driver path, so a deployment whose path variable "
           "resolved to nothing would be told the binder transport is usable";
}

// A path with no node behind it is declined because ::open fails, at
// DriverAidlImpl::isBinderPreflightOk()'s second decision point. This is the arm this entire
// suite depends on: a host
// without kernel binder support has no /dev/binder, so this is the arm that returns false,
// the selection falls back to the legacy back-end, and every other case in this binary gets
// to run. A regression here would not fail one test - it would abort the process inside
// libbinder before the first test body executed.
/**
 * @brief A path with nothing behind it is declined because the open fails.
 * @pre Runs under every invocation. The chosen path is asserted absent first, so the case
 *      cannot silently drift onto a different arm.
 * @note This is the arm the whole binary depends on: a host without kernel binder support
 *       has no /dev/binder, so this verdict is what makes the selection fall back and lets
 *       every other case run. A regression here would not fail one test - it would abort
 *       the process inside libbinder before the first test body executed.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, DeclinesANonexistentDriverPath) {
    const std::string absent = "/dev/blitzy_cec_aidl_no_such_binder_node";

    ASSERT_NE(::access(absent.c_str(), F_OK), 0)
        << "the path chosen to be absent exists on this host, so this case is not exercising the "
           "cannot-open arm; choose another path";

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(absent))
        << "the preflight accepted a driver path with nothing behind it. On a platform with no "
           "binder driver this is what stands between falling back to the legacy back-end and "
           "libbinder aborting the process during initialization";
}

// A node that opens but is not a binder driver is declined at the next arm along, where the
// BINDER_VERSION ioctl fails, at DriverAidlImpl::isBinderPreflightOk()'s third decision point.
//
// This is a genuinely different branch from the case above and worth separating: "the path
// opened" and "the thing behind the path speaks binder" are two questions, and a predicate
// that asked only the first would pass a stale non-binder node straight through to
// libbinder. A regular file is used because it is the one thing guaranteed to open O_RDWR
// on any host while refusing every ioctl; an anonymous tmpfile() cannot serve, because the
// predicate takes a path and opens it itself.
//
// The node owns its own removal. It lives in the directory TMPDIR nominates when that value
// validates, /tmp otherwise, and its destructor unlinks it - so the fatal assertions below can
// abandon the case without leaving a file behind, which a manual unlink placed after them
// cannot promise.
/**
 * @brief A node that opens but does not speak binder is declined at the ioctl arm.
 * @pre Runs under every invocation. Creates its own regular file through TemporaryNode,
 *      which is the one thing guaranteed to open O_RDWR on any host while refusing every
 *      ioctl, and unlinks it from the destructor however the case leaves.
 * @note Evidence: the node is asserted readable and writable first, so the verdict comes
 *       from the ioctl rather than from the open. "The path opened" and "the thing behind
 *       it speaks binder" are two questions, and a predicate asking only the first would
 *       pass a stale non-binder node straight through to libbinder.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, DeclinesAPathThatOpensButIsNotABinderDriver) {
    TemporaryNode node;

    ASSERT_TRUE(node.isValid()) << "a temporary node could not be created under ["
                                << temporaryDirectory()
                                << "], so this case cannot establish its precondition";
    ASSERT_EQ(::access(node.path().c_str(), R_OK | W_OK), 0)
        << "the temporary node is not readable and writable, so the preflight would decline it at "
           "the cannot-open arm instead of at the ioctl arm this case is about";

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(node.path()))
        << "the preflight accepted a path that opens but does not speak the binder protocol. "
           "Opening a node is not evidence that it is a binder driver, and handing such a node to "
           "libbinder is the condition the pin treats as fatal";
}

// The timeout parameter is accepted and honoured as an argument, asserted at both ends of
// its range on a path that cannot get far enough to wait for anything.
//
// What this does and does not prove, stated plainly rather than left to be assumed. It
// proves the two-argument overload is callable and that neither timeout value changes the
// verdict for an absent node - which is the property that matters for the production call,
// since the default two-second bound must not turn a missing node into a two-second stall.
// It does not reach the context-manager arm itself: that arm needs the node to open and to
// report a matching protocol version first, which a path and a timeout cannot synthesise
// because the real driver is absent here. That arm is reached instead through the synthetic
// probe, by the case below that reports the expected protocol and then declines the ping -
// see note (3) in the file block for why the seam exists and what it does not change.
/**
 * @brief The two-argument overload is callable and neither end of the timeout range changes
 *        the verdict for an absent node.
 * @pre Runs under every invocation, on an absent path.
 * @note What this does not establish, stated so it is not read as more: it does not reach
 *       the context-manager arm, which needs the node to open and report a matching
 *       protocol first. That arm is reached through the synthetic probe instead. What it
 *       does establish is that the default bound cannot turn a missing node into a stall.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, HonoursTheContextManagerTimeoutArgumentWithoutWaitingForAbsentNode) {
    const std::string absent = "/dev/blitzy_cec_aidl_no_such_binder_node";

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(absent, 0u))
        << "a zero timeout was expected to mean 'do not wait at all' and to leave the verdict for "
           "an absent node unchanged";

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     absent, DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS))
        << "the default timeout changed the verdict for an absent node, which would mean the node "
           "check is no longer short-circuiting ahead of the context-manager probe";
}

// The default driver path is the one libbinder itself opens, and the default argument is what
// keeps that path out of every caller.
//
// This case asserts the constant rather than the verdict, deliberately: the verdict for the
// default path is a property of the host - false here, where there is no kernel binder
// support, and true on a binder-capable runner - so asserting it either way would encode
// one host's configuration as a rule. What is host-independent is that the production call
// site names nothing (DriverAidlImpl::isServiceAvailable() calls isBinderPreflightOk() with no
// arguments) and that the harness's own preflight call does the same (from
// publishFakeForMode()), so a drift in this constant would silently change what both of
// them probe. The positive arm - the preflight passing - is reached by invocation B, where the
// AIDL back-end is selected only because it passed.
/**
 * @brief The default driver path and a non-zero default bound are the values the production
 *        and harness call sites actually probe.
 * @pre Runs under every invocation. Asserts the constants rather than the verdict,
 *      deliberately: the verdict for the default path is a property of the host, so
 *      asserting it either way would encode one host's configuration as a rule.
 * @note Evidence: both production selection and the L1 harness call the predicate with no
 *       arguments, so a drift in these constants would silently change what both of them
 *       probe. The positive verdict is invocation B's to establish.
 * @see DriverAidlImpl::isServiceAvailable()
 */
TEST_F(DriverAidlPreflightTest, DefaultDriverPathIsTheNodeLibbinderOpens) {
    EXPECT_STREQ(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH, "/dev/binder")
        << "the default binder driver path changed. Both the production selection and the L1 "
           "harness call isBinderPreflightOk() with no arguments, so this constant is what they "
           "probe; if the platform genuinely moved the node, update this expectation and check "
           "that nothing else hard-codes the old spelling";

    EXPECT_GT(DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, 0u)
        << "the default context-manager timeout is zero, which means the production preflight no "
           "longer waits at all for handle 0 to resolve. A platform whose servicemanager starts "
           "concurrently with the middleware would then be misread as having none";
}

// Decision point 4: a node that answers BINDER_VERSION with a version this build was not
// compiled against is declined, and the mismatch is what decides it.
//
// libbinder enforces protocol equality when it opens the driver, in both directions, so a
// platform whose kernel speaks 7 against an SDK built for 8 fails every open. The preflight
// exists so that condition degrades to "AIDL absent" rather than to the abort the pinned
// stack would take, and this is the arm that does it.
//
// It is reached with the substituted probe rather than with a path, because no path can
// express it: a node either speaks binder - in which case it speaks this build's protocol on
// a correctly provisioned host - or it fails the ioctl one decision point earlier. The probe
// reports a version one greater than the compiled-in expectation, which is a mismatch on
// every host regardless of whether that expectation is 7, 8 or the 0 a build without the
// binder UAPI headers reports.
//
// The arm is identified, not merely the verdict: pingContextManager must never have been
// called. Without that, a predicate that ignored the version and declined for some later
// reason would pass this case while leaving a protocol-mismatched node to reach libbinder.
/**
 * @brief A node reporting a protocol version other than this build's is declined, and the
 *        mismatch alone decides it.
 * @pre Runs under every invocation, through the substituted probe: no path can express this
 *      condition, because a node either speaks this build's protocol on a correctly
 *      provisioned host or fails the ioctl one decision point earlier.
 * @note Evidence: the probe reports one greater than the compiled-in expectation, which is
 *       a mismatch whatever that expectation is; the ping is configured to succeed, so a
 *       false verdict can only be the version; and the arm is identified rather than only
 *       the verdict, since the ping is asserted never to have been called.
 * @see DriverAidlImpl::expectedBinderProtocolVersion()
 */
TEST_F(DriverAidlPreflightTest, DeclinesANodeWhoseProtocolVersionDiffersFromThisBuild) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();
    const unsigned int mismatched = expected + 1u;

    ASSERT_NE(mismatched, expected)
        << "the mismatched version this case constructs is equal to the expected one, so it is "
           "not exercising the mismatch arm at all";

    // The ping would succeed if it were reached, so a false verdict can only have come from
    // the version comparison.
    resetSyntheticProbe(kSyntheticBinderFd, 0, mismatched, true);

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                     syntheticProbe()))
        << "the preflight accepted a node reporting protocol " << mismatched << " while this build "
           "expects " << expected << ". libbinder requires equality, so every open would fail - and "
           "on the pinned stack a failed open is fatal rather than an error return";

    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 1)
        << "the protocol version was not read exactly once, so the verdict did not come from the "
           "version comparison";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 0)
        << "the context manager was probed although the protocol version did not match. The "
           "mismatch must decide the verdict on its own: a predicate that continued past it would "
           "hand a protocol-mismatched node to libbinder if the ping happened to succeed";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor opened for the probe was not released on the mismatch arm";
}

// Decision point 5, negative: a node whose protocol matches but whose context manager never
// answers within the bound is declined.
//
// This is the second of the two hazards the preflight exists for, and it is the one that
// would not fail loudly - it would hang. Obtaining an IServiceManager polls until binder
// handle 0 resolves, with no bound of its own, so a platform with a working driver and no
// running servicemanager would stall LibCCEC::init forever rather than fall back. The bounded
// ping is what turns that into a verdict, and this case is the arm that returns it.
//
// The version matches exactly, so the only remaining decision is the ping - which is what
// makes the false verdict attributable. The deadline handed to the probe is asserted too,
// because a predicate that passed 0 or some constant of its own instead of the caller's
// timeout would leave the production two-second allowance unreachable.
/**
 * @brief A node whose protocol matches but whose context manager never answers within the
 *        bound is declined, and the caller's own bound is what reaches the probe.
 * @pre Runs under every invocation, through the substituted probe with the version matching
 *      exactly, so the ping is the only remaining decision.
 * @note This is the hazard that would not fail loudly but hang: obtaining an
 *       IServiceManager polls until handle 0 resolves with no bound of its own, so a
 *       platform with a working driver and no servicemanager would stall initialization
 *       forever. Evidence: one ping, on the descriptor that was opened, carrying the
 *       caller's 250 ms rather than a constant of the predicate's own, and one close.
 */
TEST_F(DriverAidlPreflightTest, DeclinesAMatchingProtocolWhoseContextManagerNeverAnswers) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, false);

    const unsigned int chosenTimeoutMs = 250u;

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH, chosenTimeoutMs, syntheticProbe()))
        << "the preflight accepted a node whose context manager did not answer. Reaching the "
           "service manager anyway is what blocks LibCCEC::init indefinitely, since obtaining an "
           "IServiceManager polls for handle 0 with no bound of its own";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed exactly once, so this verdict did not come from the "
           "context-manager arm";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs, chosenTimeoutMs)
        << "the caller's deadline was not the one handed to the probe. The bound is the whole point "
           "of this arm: a predicate that substituted a deadline of its own would make the "
           "production timeout unreachable and untestable";
    EXPECT_EQ(g_syntheticProbe.lastPingFd, kSyntheticBinderFd)
        << "the context manager was probed on a descriptor other than the one that was opened and "
           "version-checked";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released after the context-manager probe";
}

// The direct positive verdict: an openable node that reports this build's protocol and whose
// context manager answers is accepted.
//
// Every other preflight case here is a rejection, and a predicate that returned false
// unconditionally would pass all of them. This is the case that rules that out, and it is the
// only assertion in this file that the preflight can say yes at all on a host with no binder
// kernel support - the production positive arm is invocation B's, where the AIDL back-end is
// selected only because the real preflight passed.
//
// All five decision points are traversed, which is asserted rather than assumed: the path was
// opened, the version was read, the ping was made, and the descriptor was released exactly
// once. The open flags are checked too, because O_RDWR is a requirement of the driver rather
// than a detail - a read-only descriptor cannot carry a binder transaction.
/**
 * @brief The direct positive verdict: an openable node reporting this build's protocol and
 *        answering on handle 0 is accepted, and all five decision points are traversed.
 * @pre Runs under every invocation, through the substituted probe. This is the only
 *      assertion here that the preflight can say yes at all on a host with no binder kernel
 *      support; the production positive arm belongs to invocation B.
 * @note Evidence: one open of the given path with O_RDWR set, one version read, one ping
 *       and exactly one close. Without this case a predicate returning false
 *       unconditionally would pass every other case in the fixture.
 */
TEST_F(DriverAidlPreflightTest, AcceptsANodeWhoseProtocolMatchesAndWhoseContextManagerAnswers) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, true);

    EXPECT_TRUE(DriverAidlImpl::isBinderPreflightOk(
                    DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                    DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                    syntheticProbe()))
        << "the preflight declined a node that opened, reported this build's protocol version ("
        << expected << ") and whose context manager answered. With this arm broken the AIDL "
           "back-end can never be selected on any platform, however well provisioned, and every "
           "rejection case in this fixture would still pass";

    EXPECT_EQ(g_syntheticProbe.openCalls, 1) << "the node was not opened exactly once";
    EXPECT_EQ(g_syntheticProbe.lastOpenedPath, std::string(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH))
        << "the preflight opened a path other than the one it was given";
    EXPECT_EQ(g_syntheticProbe.lastOpenFlags & O_RDWR, O_RDWR)
        << "the node was not opened for reading and writing both. A binder transaction needs "
           "both, so a "
           "read-only descriptor would pass the version check and then fail every transaction";
    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 1)
        << "the protocol version was not read exactly once on the accepting path";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed on the accepting path, so the verdict was reached "
           "without establishing that anything answers handle 0";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the probe descriptor was not released on the accepting path, which is the path that "
           "runs at every initialization on a binder-capable platform";
}

// The context-manager timeout ceiling, both arms.
//
// isBinderPreflightOk() takes its bound as an `unsigned int`, so a caller - or a future
// configuration value - can name a figure far larger than any plausible servicemanager
// start-up delay, and every millisecond of it would be time LibCCEC::init() spends blocked
// before the legacy fallback is even considered. The predicate therefore clamps to
// MAX_CONTEXT_MANAGER_TIMEOUT_MS before it probes, and the clamp is observable exactly where
// it matters: the probe records the bound it was actually handed, so these two cases read the
// effective value rather than inferring it from elapsed time, which on a loaded host would be
// a flaky measurement of the same fact.
//
// Both arms are asserted because a predicate that clamped unconditionally would be just as
// wrong as one that never clamped: a caller asking for less than the ceiling must get what it
// asked for, or the whole parameter becomes decorative.
/**
 * @brief A bound above the ceiling is clamped to the ceiling before the probe is made.
 * @pre Runs under every invocation, through the substituted probe with the ping declining,
 *      so the case stays on the context-manager arm.
 * @note Evidence: the probe records the bound it was handed, so the effective value is read
 *       rather than inferred from elapsed time - which on a loaded host would be a flaky
 *       measurement of the same fact. An unclamped bound is time initialization spends
 *       blocked before the legacy fallback is even considered.
 */
TEST_F(DriverAidlPreflightTest, ClampsAContextManagerTimeoutAboveTheCeilingToTheCeiling) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, false);

    const unsigned int requested = DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS * 6u;

    ASSERT_GT(requested, DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the requested bound is not above the ceiling, so this case would exercise the "
           "pass-through arm rather than the clamp";

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH, requested, syntheticProbe()))
        << "the verdict changed for a node whose context manager does not answer, so this case is "
           "no longer reaching the context-manager arm at all";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed exactly once, so the recorded bound below did not "
           "come from this call";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs, DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the probe was handed " << g_syntheticProbe.lastPingTimeoutMs << " ms where the ceiling "
           "is " << DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS << " ms. An unclamped bound is "
           "time LibCCEC::init() spends blocked before it can fall back to the legacy back-end, "
           "which is the one thing the preflight exists to prevent";
}

// The pass-through arm of the same branch: a bound at the ceiling is handed through unchanged.
//
// The boundary value is used rather than a comfortable middle one, because `>` and `>=` differ
// only here and a clamp written with the wrong comparison would lower a legitimate request by
// nothing observable at any other value.
/**
 * @brief A bound exactly at the ceiling is handed through unchanged, so the clamp cannot
 *        lower a legitimate request.
 * @pre Runs under every invocation, through the substituted probe with the ping declining.
 * @note The boundary value is used rather than a comfortable middle one because `>` and
 *       `>=` differ only here: a clamp written with the wrong comparison would lower every
 *       request at the ceiling and nothing at any other value.
 */
TEST_F(DriverAidlPreflightTest, PassesAContextManagerTimeoutAtTheCeilingThroughUnchanged) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, false);

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS,
                     syntheticProbe()))
        << "the verdict changed for a node whose context manager does not answer";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not probed exactly once";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs, DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "a bound exactly at the ceiling was altered on its way to the probe, so the clamp is "
           "using the wrong comparison and every request at the ceiling is being lowered";
}

// The default bound is below the ceiling, so the clamp cannot fire on the production call path.
//
// Asserted as a relation between the two constants rather than as behaviour: if a later change
// raised the default above the ceiling, every initialization would silently take the clamp arm
// and the default would stop meaning what it says. This is the one place that relation is
// checked.
/**
 * @brief The default bound is below the ceiling, so the clamp cannot fire on the production
 *        call path.
 * @pre Runs under every invocation; compares two constants and calls nothing.
 * @note Asserted as a relation rather than as behaviour: were a later change to raise the
 *       default above the ceiling, every initialization would silently take the clamp arm
 *       and the default would stop meaning what it says. This is the one place that
 *       relation is checked.
 */
TEST_F(DriverAidlPreflightTest, TheDefaultContextManagerTimeoutSitsBelowTheCeiling) {
    EXPECT_LT(DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
              DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the default context-manager timeout ("
        << DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS << " ms) is not below the ceiling ("
        << DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS << " ms), so the production call path "
           "would take the clamp arm at every initialization and the default would be unreachable";
}

// Descriptor hygiene, swept across all five decision points in one case.
//
// The predicate opens the driver node itself and must release it on every exit, including the
// three that leave early. A leak here is not cosmetic: the preflight runs at initialization
// in a long-lived middleware process, and on the pinned stack a lingering descriptor on
// /dev/binder is a driver context that nothing will close until the process exits.
//
// The rule asserted is "exactly one close per successful open", which is why the first two
// rows expect zero closes: an empty path opens nothing, and a failed open has nothing to
// release - closing a negative descriptor would be a bug of its own. The sweep is one case
// rather than five because the property is uniform and a gap in it is far easier to see in
// one table than spread across five bodies.
/**
 * @brief Every preflight arm releases exactly the descriptors it opened, including the three
 *        that leave early.
 * @pre Runs under every invocation, sweeping the arms through one table driven by the
 *      substituted probe.
 * @note The rule is one close per successful open, which is why the first rows expect zero:
 *       an empty path opens nothing, and a failed open has nothing to release, so closing a
 *       negative descriptor would be a defect of its own. A leak here is not cosmetic - the
 *       preflight runs at initialization in a long-lived process, and a lingering descriptor
 *       on the driver node is a context nothing will close until the process exits.
 */
TEST_F(DriverAidlPreflightTest, EveryPreflightArmReleasesExactlyTheDescriptorsItOpened) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    /** @brief One row of the sweep: how the probe is configured, and what must follow. */
    struct Arm {
        /** @brief Which decision point this row stops at, quoted in every failure message. */
        const char *name;
        /** @brief Driver path handed to the predicate. */
        const char *path;
        /** @brief openNode()'s answer for this row. */
        int  answerOpen;
        /** @brief readProtocolVersion()'s answer for this row. */
        int  answerProtocolRead;
        /** @brief Protocol version reported on a successful read. */
        unsigned int answerProtocolVersion;
        /** @brief pingContextManager()'s answer for this row. */
        bool answerPing;
        /** @brief The verdict the predicate must reach. */
        bool expectedVerdict;
        /** @brief How many opens this row must perform. */
        int  expectedOpens;
        /** @brief How many closes must follow - one per successful open, and zero otherwise. */
        int  expectedCloses;
    };

    const Arm arms[] = {
        { "decision point 1: empty path",        "",  kSyntheticBinderFd,  0, expected,      true,  false, 0, 0 },
        { "decision point 2: open fails",        "/dev/binder", -1,        0, expected,      true,  false, 1, 0 },
        { "decision point 6: ioctl fails",       "/dev/binder", kSyntheticBinderFd, -1, expected, true, false, 1, 1 },
        { "decision point 7: protocol mismatch", "/dev/binder", kSyntheticBinderFd,  0, expected + 1u, true, false, 1, 1 },
        { "decision point 8: ping declines",     "/dev/binder", kSyntheticBinderFd,  0, expected,      false, false, 1, 1 },
        { "decision point 8: ping answers",      "/dev/binder", kSyntheticBinderFd,  0, expected,      true,  true,  1, 1 },
    };

    for (size_t i = 0; i < sizeof(arms) / sizeof(arms[0]); i++) {
        const Arm &arm = arms[i];

        resetSyntheticProbe(arm.answerOpen, arm.answerProtocolRead, arm.answerProtocolVersion,
                            arm.answerPing);

        const bool verdict = DriverAidlImpl::isBinderPreflightOk(
            std::string(arm.path), DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
            syntheticProbe());

        EXPECT_EQ(verdict, arm.expectedVerdict)
            << "unexpected verdict on " << arm.name;
        EXPECT_EQ(g_syntheticProbe.openCalls, arm.expectedOpens)
            << "unexpected number of opens on " << arm.name;
        EXPECT_EQ(g_syntheticProbe.closeCalls, arm.expectedCloses)
            << "the descriptor accounting is wrong on " << arm.name << ": " << arm.expectedOpens
            << " successful open(s) were expected to be matched by " << arm.expectedCloses
            << " close(s), and " << g_syntheticProbe.closeCalls << " were made. A preflight that "
               "leaks a /dev/binder descriptor leaks a driver context for the life of the process; "
               "one that closes a negative descriptor is a bug of its own";

        if (arm.expectedCloses > 0) {
            EXPECT_EQ(g_syntheticProbe.lastClosedFd, kSyntheticBinderFd)
                << "the descriptor released on " << arm.name << " is not the one that was opened";
        }
    }
}

// THE RESTATED POSIX CONSTANTS, CROSS-CHECKED AGAINST THE SYSTEM HEADERS.
//
// ccec/src/DriverAidlImpl.hpp restates S_IFMT, S_IFCHR and root's uid as constants of its
// own, because that header must still compile where the binder kernel UAPI definitions are
// absent and it therefore includes no <sys/stat.h>. A restatement that drifts from the value
// it restates would be invisible: the file-type test would compare against the wrong mask,
// every real character device would look like something else, and the AIDL path would be
// declined on every correctly provisioned platform with a message blaming the node.
//
// This is the one place the two are compared, and it is why the identity fixtures above build
// their modes from the POSIX macros rather than from the production constants - so the two
// sides of the comparison are independent and a wrong constant cannot agree with itself.
TEST_F(DriverAidlPreflightTest, TheRestatedPosixNodeConstantsMatchTheSystemHeaders) {
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK, static_cast<unsigned int>(S_IFMT))
        << "the restated file-type mask (0" << std::oct << DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK
        << ") differs from S_IFMT (0" << static_cast<unsigned int>(S_IFMT) << std::dec
        << "), so the preflight extracts the wrong bits from st_mode and its character-device "
           "test means nothing";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE, static_cast<unsigned int>(S_IFCHR))
        << "the restated character-device type (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE << ") differs from S_IFCHR (0"
        << static_cast<unsigned int>(S_IFCHR) << std::dec << "), so every real /dev/binder "
           "would be refused and the AIDL path could never be selected on any platform";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_REQUIRED_OWNER_UID, 0u)
        << "the required owner is no longer root. devtmpfs and binderfs both create the binder "
           "node as root, so anything else here either refuses every real platform or accepts a "
           "node an unprivileged process could have created";

    // The three permission constants, restated for the same reason and checked the same way.
    // They feed a DIAGNOSTIC rather than a verdict, which does not make them exempt: a wrong
    // mask makes the permissive-node line report the wrong number, or report nothing at all on
    // exactly the platform an integrator needs to be told about.
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK,
              static_cast<unsigned int>(S_IRWXU | S_IRWXG | S_IRWXO))
        << "the restated permission mask (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK << ") differs from S_IRWXU|S_IRWXG|"
        << "S_IRWXO (0" << static_cast<unsigned int>(S_IRWXU | S_IRWXG | S_IRWXO) << std::dec
        << "), so the permission bits reported for a permissive binder node are the wrong bits";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE, static_cast<unsigned int>(S_IWGRP))
        << "the restated group-write bit (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE << ") differs from S_IWGRP (0"
        << static_cast<unsigned int>(S_IWGRP) << std::dec << "), so a group-writable binder node "
           "would go unreported";

    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE, static_cast<unsigned int>(S_IWOTH))
        << "the restated world-write bit (0" << std::oct
        << DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE << ") differs from S_IWOTH (0"
        << static_cast<unsigned int>(S_IWOTH) << std::dec << "), so a world-writable binder node "
           "would go unreported";

    // The two write bits must be INSIDE the permission mask, or the line would announce a
    // condition and then print bits that cannot express it.
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE &
                  DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE)
        << "the group-write bit falls outside the permission mask the diagnostic prints";
    EXPECT_EQ(DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE &
                  DriverAidlImpl::BINDER_NODE_MODE_PERMISSION_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE)
        << "the world-write bit falls outside the permission mask the diagnostic prints";

    // And neither may collide with the file-type bits, or the observation would fire on the
    // node's TYPE and the character-device check would be reading permission bits.
    EXPECT_EQ((DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE |
               DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE) &
                  DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK,
              0u)
        << "the write bits overlap the file-type mask, so the two readings of st_mode - type and "
           "permissions - are no longer independent";
}

// Decision point 3 of 8: a node that OPENS but whose identity cannot be read is declined,
// before the protocol ioctl is attempted.
//
// This arm is not a formality. The identity is the ONLY thing that makes the later re-check
// mean anything - without it there is nothing to compare the pre-lookup resolution of the
// same pathname against - so a preflight that shrugged at a failed fstat and carried on would
// hand libbinder a name it had certified on no evidence, and on the pinned stack libbinder
// aborts rather than fails when that name turns out not to be a usable binder driver.
//
// The ordering assertion is the second half of the case: the protocol read must NOT have been
// attempted, because an arm that ran after the ioctl would have already spent a syscall on a
// node it was about to refuse and, more importantly, would have moved the identity check
// further from the open it describes.
TEST_F(DriverAidlPreflightTest, DeclinesANodeWhoseDescriptorCannotBeIdentified) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerIdentifyDescriptor = -1;

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight accepted a node it could not identify, so it certified a pathname with "
           "no evidence about what the name refers to and nothing to re-verify before libbinder "
           "opens the same name";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 1)
        << "the identity was not asked for exactly once";
    EXPECT_EQ(g_syntheticProbe.lastIdentifiedFd, kSyntheticBinderFd)
        << "the identity was asked of something other than the descriptor that was opened. Asking "
           "the PATH instead would defeat the purpose: the answer could change underneath the "
           "process, which is precisely the window this check closes";
    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 0)
        << "the protocol ioctl was attempted on a node whose identity had already failed, so the "
           "identity arm no longer sits where it is documented to sit";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on the identity-failure arm";
}

// Decision point 4 of 8: a node that is not a CHARACTER DEVICE is declined.
//
// A regular file at /dev/binder is the shape a substitution takes when an unprivileged
// process wins a race to create the node - and it is also the shape of a plain
// mis-provisioning. Either way it is not the binder driver, and the point of refusing it here
// rather than letting the ioctl arm catch it is that the identity carried forward to the
// pre-lookup re-check has to be the identity of a node that IS a binder driver, not merely of
// something that happened to answer BINDER_VERSION.
//
// Only the file type is changed from a good node, so this case isolates one property.
TEST_F(DriverAidlPreflightTest, DeclinesANodeThatIsNotACharacterDevice) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerDescriptorIdentity.mode = static_cast<unsigned int>(S_IFREG | 0666);

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight accepted a REGULAR FILE as the binder driver. Every supported binder "
           "layout publishes a character device, so this node is not the driver, and a node an "
           "unprivileged process could create is exactly what must not be selected as the HAL";

    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 0)
        << "the protocol ioctl was attempted on a node that is not a character device";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on the file-type arm";
}

// Decision point 5 of 8: a character device NOT OWNED BY ROOT is declined.
//
// devtmpfs and binderfs both create the node as root. A character device at that path owned
// by anyone else is one an unprivileged process was able to create or replace - and selecting
// the AIDL path against it would put the whole HAL boundary, every frame transmitted and every
// frame delivered, behind whatever published itself there. That is the finding this arm exists
// for, and it is the half of it that IS inside the middleware's reach; the other half is the
// platform prerequisite recorded on isServiceAvailable().
//
// The node is left a well-formed character device and only its owner is changed, so a
// regression that merged the two checks into one would fail here rather than pass by accident.
TEST_F(DriverAidlPreflightTest, DeclinesACharacterDeviceThatIsNotOwnedByRoot) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerDescriptorIdentity.uid = 1000u;

    ASSERT_EQ(g_syntheticProbe.answerDescriptorIdentity.mode &
                  DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE)
        << "this case is meant to isolate OWNERSHIP, and the node it configures is not a "
           "character device, so it would be refused one arm earlier";

    EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                     DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                     DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight accepted a binder node owned by an unprivileged user. Anything able to "
           "create that node is able to interpose on every CEC frame the middleware sends and "
           "receives, which is why ownership is refused rather than merely logged";

    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 0)
        << "the protocol ioctl was attempted on a node that is not owned by root";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on the ownership arm";
}

// CUSTODY, BOTH WAYS ROUND, IN ONE CASE because the two halves are one property.
//
// The preflight validates a pathname and libbinder then opens the SAME pathname for itself.
// Releasing the validated descriptor before returning left a window in which the node could
// change meaning, and on the pinned stack losing that window is a process abort rather than a
// wrong answer. So a caller that intends to proceed to the lookup takes CUSTODY: the
// descriptor stays open - which also pins the inode, so the identity cannot be satisfied by a
// recycled node number - and the identity travels with it.
//
// The second half matters just as much: a caller that asks for nothing must get the original
// behaviour, descriptor released, process left exactly as it was found. That is what the
// harness's own preflight assertion and every negative-arm case above rely on, and a preflight
// that retained unconditionally would leak a driver context at every one of them.
TEST_F(DriverAidlPreflightTest, RetainsTheValidatedDescriptorAndItsIdentityOnlyWhenCustodyIsRequested) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, true);

    int retained = 999;
    DriverAidlImpl::BinderNodeIdentity identity;

    memset(&identity, 0, sizeof(identity));

    ASSERT_TRUE(DriverAidlImpl::isBinderPreflightOk(
                    DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                    DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe(),
                    &retained, &identity))
        << "the preflight declined a well-formed node when custody was requested, so the custody "
           "parameters have changed the verdict rather than only what happens to the descriptor";

    EXPECT_EQ(retained, kSyntheticBinderFd)
        << "custody was requested and the validated descriptor was not handed over";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 0)
        << "the descriptor was released even though custody was requested. The caller then holds a "
           "number that no longer refers to anything, and the identity it is about to re-verify is "
           "no longer pinned - which is the whole of what custody buys";

    EXPECT_EQ(identity.device, kValidatedNodeIdentity.device)
        << "the retained identity does not carry the validated node's device";
    EXPECT_EQ(identity.inode, kValidatedNodeIdentity.inode)
        << "the retained identity does not carry the validated node's inode, which is the field a "
           "substitution changes";
    EXPECT_EQ(identity.rdev, kValidatedNodeIdentity.rdev)
        << "the retained identity does not carry the validated node's device numbers";
    EXPECT_EQ(identity.uid, kValidatedNodeIdentity.uid)
        << "the retained identity does not carry the validated node's owner";

    // The other half: no custody requested, original behaviour, nothing left open.
    resetSyntheticProbe(kSyntheticBinderFd, 0, expected, true);

    ASSERT_TRUE(DriverAidlImpl::isBinderPreflightOk(
                    DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                    DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe()))
        << "the preflight declined the same well-formed node when custody was NOT requested";

    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was retained by a caller that asked for nothing. Every negative-arm case "
           "here, and the harness's own preflight assertion, would then leak a /dev/binder "
           "descriptor - a driver context nothing reclaims until the process exits";
    EXPECT_EQ(g_syntheticProbe.lastClosedFd, kSyntheticBinderFd)
        << "the descriptor released is not the one that was opened";
}

// THE CUSTODY SLOT IS CLEARED BEFORE ANY ARM CAN RETURN, swept across every decline.
//
// A caller reads the slot after the call. If a decline left it untouched, the caller would see
// whatever it happened to hold - in production, a stack value; in the sequence below, a
// descriptor from a previous call - and would either release something it does not own or hold
// a custody window it never opened. The slot is therefore set to -1 first, so "not retained"
// and "stale" are the same observation and no caller has to tell them apart.
TEST_F(DriverAidlPreflightTest, ClearsTheCustodySlotOnEveryDeclineSoNoStaleDescriptorIsReported) {
    const unsigned int expected = DriverAidlImpl::expectedBinderProtocolVersion();

    struct Decline {
        const char *name;
        const char *path;
        int  answerOpen;
        int  answerIdentifyDescriptor;
        unsigned int identityMode;
        unsigned int identityUid;
        int  answerProtocolRead;
        unsigned int answerProtocolVersion;
        bool answerPing;
    };

    const unsigned int goodMode = static_cast<unsigned int>(S_IFCHR | 0600);

    const Decline declines[] = {
        { "decision point 1: empty path",           "",            kSyntheticBinderFd,  0, goodMode, 0u,    0,  expected,      true  },
        { "decision point 2: open fails",           "/dev/binder", -1,                  0, goodMode, 0u,    0,  expected,      true  },
        { "decision point 3: identity unreadable",  "/dev/binder", kSyntheticBinderFd, -1, goodMode, 0u,    0,  expected,      true  },
        { "decision point 4: not a character node", "/dev/binder", kSyntheticBinderFd,  0, static_cast<unsigned int>(S_IFREG | 0666), 0u, 0, expected, true },
        { "decision point 5: not owned by root",    "/dev/binder", kSyntheticBinderFd,  0, goodMode, 1000u, 0,  expected,      true  },
        { "decision point 6: ioctl fails",          "/dev/binder", kSyntheticBinderFd,  0, goodMode, 0u,    -1, expected,      true  },
        { "decision point 7: protocol mismatch",    "/dev/binder", kSyntheticBinderFd,  0, goodMode, 0u,    0,  expected + 1u, true  },
        { "decision point 8: ping declines",        "/dev/binder", kSyntheticBinderFd,  0, goodMode, 0u,    0,  expected,      false },
    };

    for (size_t i = 0; i < sizeof(declines) / sizeof(declines[0]); i++) {
        const Decline &decline = declines[i];

        resetSyntheticProbe(decline.answerOpen, decline.answerProtocolRead,
                            decline.answerProtocolVersion, decline.answerPing);
        g_syntheticProbe.answerIdentifyDescriptor = decline.answerIdentifyDescriptor;
        g_syntheticProbe.answerDescriptorIdentity.mode = decline.identityMode;
        g_syntheticProbe.answerDescriptorIdentity.uid = decline.identityUid;

        int retained = kSyntheticBinderFd;
        DriverAidlImpl::BinderNodeIdentity identity;

        memset(&identity, 0, sizeof(identity));

        EXPECT_FALSE(DriverAidlImpl::isBinderPreflightOk(
                         std::string(decline.path),
                         DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe(),
                         &retained, &identity))
            << "unexpected verdict on " << decline.name;
        EXPECT_EQ(retained, -1)
            << "the custody slot still held " << retained << " after " << decline.name
            << ". A caller reading it would either release a descriptor it does not own or believe "
               "it holds a custody window that was never opened";
    }
}

/**
 * @brief The pre-lookup re-verification: the check and the use are ONE path.
 *
 * WHY THESE CASES DRIVE isServiceAvailable() RATHER THAN THE PREFLIGHT. The preflight's
 * verdict is worth acting on only while it still describes the name libbinder is about to
 * open, and the code that establishes that sits between the preflight and the lookup - so it
 * is reachable only through the query itself. All four cases below decline BEFORE the service
 * lookup is entered, which is what makes them safe to run on a host with no binder transport:
 * nothing here reaches libbinder, and the synthetic probe is what guarantees that.
 *
 * The instance is local in every case, so the process-global driver and the resolved selection
 * are untouched and these run identically under every invocation.
 */
// The name resolves to a DIFFERENT node between the check and the use, and the AIDL path is
// declined instead of libbinder being handed the substituted node.
//
// THE EXPLOIT THIS CLOSES, stated concretely so the case is not read as ceremony. The preflight
// validates /dev/binder; before libbinder opens the same name, the node is replaced - unlinked
// and re-created, bind-mounted over, or a symlink re-pointed. Nothing in the middleware would
// have noticed, and libbinder's own open is not a recoverable failure on the pinned stack: it
// is a LOG_ALWAYS_FATAL_IF abort. So the check that exists to guarantee a nonfatal legacy
// fallback would have caused the abort it was written to prevent.
//
// The observation counters are the proof that the decline happened at the identity comparison
// and not somewhere convenient: exactly one path resolution, no second open, and the retained
// descriptor released.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheNameResolvesToADifferentNodeBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerPathIdentity = kSubstitutedNodeIdentity;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the query proceeded to the service lookup after the driver node had been substituted "
           "between the preflight and the lookup";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr)
        << "a decline recorded no reason, so the factory has nothing to name in its fallback line";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a substituted driver node was reported as a SERVICE problem. What failed is the "
           "transport, and naming the service would send an integrator looking in the wrong place";

    EXPECT_EQ(g_syntheticProbe.identifyPathCalls, 1)
        << "the pathname was not re-resolved exactly once immediately before the lookup";
    EXPECT_EQ(g_syntheticProbe.lastIdentifiedPath,
              std::string(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH))
        << "the re-resolution was performed on a different path from the one the preflight "
           "validated, so the comparison is not about the name libbinder will open";
    EXPECT_EQ(g_syntheticProbe.openCalls, 1)
        << "the node was reopened for the liveness check even though the identity comparison had "
           "already failed, so the decline is happening later than it should";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the retained descriptor was not released on the identity-mismatch decline";
    EXPECT_EQ(g_syntheticProbe.lastClosedFd, kSyntheticBinderFd)
        << "the descriptor released is not the retained one";
}

// The name cannot be resolved at all between the check and the use - the node was removed -
// and that is a decline, never a "nothing changed".
//
// The direction matters: a failed re-resolution says nothing about whether the node is still
// the validated one, so treating it as "unchanged" would be the same as not checking. It is
// therefore refused, and the refusal is nonfatal, which is the whole point.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenThePathCannotBeResolvedBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerIdentifyPath = -1;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "a pathname that could no longer be resolved was treated as unchanged, which is "
           "indistinguishable from not re-checking it at all";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "an unresolvable driver node was not reported as a transport condition";

    EXPECT_EQ(g_syntheticProbe.openCalls, 1)
        << "the node was reopened after the re-resolution had already failed";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the retained descriptor was not released when the re-resolution failed";
}

// THE STAGE-ONE DECLINE, WHICH IS A DIFFERENT ARM FROM EVERY CASE ABOVE and until now had no
// case of its own on a host that has a working binder driver.
//
// isServiceAvailable() declines in four ordered stages - preflight, custody re-verification,
// service lookup, compatibility - and the two the cases above drive are the first two: they let
// the preflight PASS and then break the re-identification or the liveness re-check, so they
// reach the transport reason by the second assignment inside the function rather than the
// first. The two later stages need a binder driver and cannot be reached from this host at
// all. The arm this case drives is the FIRST one - the preflight
// itself refusing - and the distinction is not cosmetic, because that arm is the one every
// legacy-only SOC takes on every boot. It is the single most-travelled path in this file on real
// hardware and the one whose failure would abort LibCCEC::init() instead of falling back.
//
// Why it needed writing at all, stated plainly because the answer is the reason the branch-arm
// gate exists. On a host with NO binder driver the factory takes this arm unaided: the default
// path cannot be opened, the preflight declines, and the arm is covered as a side effect of
// every driverless run. On a host that HAS a usable driver - which is the only kind of host that
// can run the AIDL invocations at all - the factory never takes it, so the arm silently went
// unmeasured on exactly the runs that measure everything else. A test that only ever ran where
// the platform happened to force the arm was not testing the arm.
//
// The unusable path is an EMPTY one rather than a plausible-looking absent filename, and that is
// deliberate: an empty path is rejected by isBinderPreflightOk() before it calls the probe at
// all, so the two probe-call assertions below can state something no reason-string comparison
// can - that the decline happened at stage one, ahead of any node work, rather than later on
// with the same reason text. A nonexistent filename would decline for the right reason at the
// wrong stage and this case could not tell the difference.
/**
 * @brief The service query declines at its FIRST stage when the driver path is unusable, and
 *        does so before touching the node.
 * @pre None beyond a constructed local instance. Runs under every invocation, including one on a
 *      host with a fully working binder driver, which is the point of it.
 * @note Complements DriverAidlImpl::isBinderPreflightOk()'s own empty-path case: that one
 *       establishes the predicate refuses, this one establishes isServiceAvailable() ACTS on the
 *       refusal - returns false, records the transport reason, and issues no lookup.
 * @note The probe counters are asserted at zero rather than merely "not more than one". They are
 *       what locates the decline at stage one; the reason text alone is assigned at two
 *       different stages and cannot distinguish them.
 * @see DriverAidlImpl::isServiceAvailable()
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesAtThePreflightStageWhenTheDriverPathIsUnusable) {
    // A probe that would answer every question correctly if it were ever asked. That is the
    // control: a decline recorded here cannot be attributed to a probe rigged to fail, and the
    // counters below show it was not consulted.
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(std::string(),
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "an unusable binder driver path did not decline. This is the arm a legacy-only SOC "
           "takes on every boot, and a true return here would send the factory on to a lookup "
           "against a node it never validated";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr)
        << "the preflight declined and recorded no reason, so the selection helper has nothing to "
           "name in its fallback line and the platform condition becomes undiagnosable";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a preflight decline was not reported as a transport condition, so a legacy-only "
           "platform would be told something other than the truth about why it fell back";

    EXPECT_EQ(g_syntheticProbe.openCalls, 0)
        << "the node was opened although the path could never have been usable, so the decline is "
           "happening after stage one rather than at it - and the descriptor custody the later "
           "stages depend on was taken out for a path that was already refused";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 0)
        << "a descriptor was released although none should ever have been opened";
}

// The pathname still resolves to the validated node, but the descriptor REOPENED for the
// liveness check does not - so the reopen is validated in its own right rather than trusted.
//
// This is the arm that stops the second descriptor being a hole in the guarantee. The liveness
// ping cannot go over the retained descriptor - see the case below for the kernel reason - so a
// second one is opened, and a second open is a second resolution of the same name with its own
// race. Identifying it and comparing it against the SAME validated identity is what makes it
// inherit the whole of the guarantee instead of widening the window it was added to close.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheReopenedDescriptorIsNotTheValidatedNode) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kSubstitutedNodeIdentity;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the descriptor reopened for the liveness check was trusted without being identified "
           "against the node the preflight validated, so the second open is an unguarded second "
           "resolution of the same name";

    EXPECT_EQ(g_syntheticProbe.openCalls, 2)
        << "the liveness check did not open a second descriptor";
    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor that had already failed its identity "
           "comparison";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors - the retained one and the reopened one - were expected to be "
           "released, and " << g_syntheticProbe.closeCalls << " release(s) were made";
}

// The descriptor reopened for the liveness check CANNOT BE IDENTIFIED AT ALL, which is a
// different arm from the one above and is not implied by it: that case answers with a
// different node, this one answers with nothing.
//
// WHY BOTH ARMS EXIST SEPARATELY. The re-verification's third point is a disjunction - the
// reopened descriptor is refused if it cannot be identified OR if it identifies as some other
// object - and the whole point of mapping arms rather than lines is that a disjunction's two
// operands are two decisions. An fstat that fails on a descriptor the open just returned is
// the shape a revoked or torn-down binderfs mount takes, and trusting the descriptor because
// the failure was in the CHECK rather than in the object would be the same category of error
// the retained-identity path exists to remove.
//
// The probe fails identifyDescriptor on its SECOND call only. It has to: the preflight's own
// call is the first, and a probe that failed every call would decline at the preflight and
// never reach the re-verification this case is about.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheReopenedDescriptorCannotBeIdentified) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentifyResult = true;
    g_syntheticProbe.answerIdentifyDescriptorSecond = -1;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the query proceeded to the service lookup over a reopened descriptor whose identity "
           "could not be read, so nothing established that libbinder is about to open the node "
           "the preflight validated";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "an unidentifiable reopened descriptor was not reported as a transport condition";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified at all, so this case did not reach the arm "
           "it is about";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor whose identity had already failed to "
           "read; the ping is the LAST of the four re-verification points and must not run once "
           "an earlier one has declined";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made; a descriptor that failed its identity read is still a "
           "descriptor this check opened";
}

// The context manager answered the PREFLIGHT and has stopped answering by the time the lookup
// is about to happen: declined, under the same bound, instead of hanging inside libbinder.
//
// WHAT THIS BOUNDS AND WHAT IT DOES NOT. defaultServiceManager() polls until binder handle 0
// resolves with no upper bound of its own, so a wedged or dying servicemanager is the dominant
// stall mode for a platform whose driver is perfectly healthy. Asking again here, under the
// preflight's own bound, turns that stall into a decline. It bounds nothing INSIDE getService
// once entered - the pinned libbinder offers no client-side transaction deadline, which is
// recorded as a platform prerequisite on isServiceAvailable() rather than papered over.
//
// THE PING GOES OVER THE SECOND DESCRIPTOR, AND THAT IS ASSERTED, because it is forced by the
// driver rather than chosen: the ping maps the driver's transaction buffer, and the binder
// driver permits exactly ONE mapping per open descriptor for that descriptor's lifetime -
// unmapping releases the range but not the right. A re-ping over the retained descriptor would
// therefore fail on every correctly provisioned platform and decline the AIDL path everywhere,
// which is why this assertion is here rather than the simpler one.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheContextManagerStopsAnsweringBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondPingAnswer = true;
    g_syntheticProbe.answerPingSecond = false;

    DriverAidlImpl backEnd;

    EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                            syntheticProbe()))
        << "the query entered the service lookup with a context manager that had stopped "
           "answering, which is an unbounded wait inside libbinder rather than a fallback";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a context manager that stopped answering was not reported as a transport condition";

    EXPECT_EQ(g_syntheticProbe.pingCalls, 2)
        << "the context manager was probed " << g_syntheticProbe.pingCalls
        << " time(s). The preflight's probe establishes liveness at CHECK time; the second probe "
           "is the one that establishes it at USE time, and without it the whole re-verification "
           "reduces to an identity comparison";
    EXPECT_EQ(g_syntheticProbe.lastPingFd, kSyntheticSecondBinderFd)
        << "the second ping was issued over the RETAINED descriptor. The binder driver permits "
           "one mapping per open descriptor for its lifetime, so that ping cannot succeed on any "
           "real platform and the AIDL path would be declined everywhere";
    EXPECT_EQ(g_syntheticProbe.lastPingTimeoutMs,
              DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS)
        << "the second ping was not bounded by the same timeout as the first, so one of the two "
           "routes into the ping can exceed the ceiling";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made; the reopened descriptor is opened inside the check and must "
           "not outlive it";

    // CUSTODY IS HELD ACROSS THE WINDOW, ASSERTED THROUGH THE RELEASE ORDER, and this is the
    // one property on which the whole identity comparison rests rather than an ordering detail.
    //
    // An open descriptor pins an inode. That is the ONLY reason comparing `inode` across the
    // check-to-use window means anything: without a descriptor held open on the validated node,
    // the kernel is free to recycle that inode number for a replacement created at the same
    // path, and the comparison would report "unchanged" for a node that had in fact been
    // substituted. So the retained descriptor must still be open while the re-verification runs.
    //
    // Nothing else in this suite can see that. `closeCalls == 2` counts the releases but not
    // when they happened; `lastPingFd` proves the ping used the fresh descriptor but not that
    // the retained one was still alive at the time. A "simplification" that released the
    // retained descriptor as soon as the preflight returned - which reads as tidier, since the
    // re-verification opens its own - would keep both of those assertions green and silently
    // delete the pinning guarantee. The release ORDER is what distinguishes the two: the fresh
    // descriptor is released inside the re-verification, and the retained one only when
    // isServiceAvailable() returns, so the fresh one must be released FIRST.
    //
    // This is also the assertion that pins the residual recorded on BinderNodeIdentity: the
    // window is narrowed to its structural minimum and not closed, and the narrowing is worth
    // exactly as much as the custody that backs it.
    ASSERT_EQ(g_syntheticProbe.closedFds.size(), 2u)
        << "the release sequence was not recorded as two entries, so the custody order cannot be "
           "checked at all";
    EXPECT_EQ(g_syntheticProbe.closedFds[0], kSyntheticSecondBinderFd)
        << "the first descriptor released was " << g_syntheticProbe.closedFds[0]
        << " where the descriptor reopened for the pre-lookup liveness check ("
        << kSyntheticSecondBinderFd
        << ") was expected. Releasing the RETAINED descriptor first means custody ended before "
           "the re-verification finished, which unpins the validated inode and lets a "
           "substitution at the same path be recycled onto the same inode number - the identity "
           "comparison would then report 'unchanged' for a node that had been replaced";
    EXPECT_EQ(g_syntheticProbe.closedFds[1], kSyntheticBinderFd)
        << "the descriptor released last was " << g_syntheticProbe.closedFds[1]
        << " where the retained, validated descriptor (" << kSyntheticBinderFd
        << ") was expected. It must outlive the entire re-verification and be released only when "
           "the query returns";
}

// THE SAME INODE, RE-PERMISSIONED between the check and the use: declined.
//
// THE EXPLOIT, CONCRETELY, because this is the half of the window a comparison of
// `(device, inode, rdev)` alone reported as "unchanged". The preflight validates /dev/binder -
// character device, owned by root, protocol matched, context manager answering - and carries
// its identity forward. Before libbinder resolves the same name, the node is `chmod`ed. It is
// not replaced: device, inode and rdev are untouched, and the retained descriptor still pins
// that very inode. An adversary who cannot win a race to create a node at that path can
// nevertheless widen write access to the one that is there, and every check that could have
// noticed - the file-type test and the root-owner test - has already run. So the ONLY thing
// standing in that window is whether the re-verification enforces the attributes it captured.
//
// The divergence is asserted through the LOG as well as through the verdict, because a decline
// alone would not distinguish "the mode was compared" from "something else refused it": the
// numeric divergence mask names bit3, and bit3 is the mode.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheValidatedNodeIsRePermissionedBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kRePermissionedNodeIdentity;

    // The premise of the case, asserted rather than assumed: SAME OBJECT, DIFFERENT MODE. If
    // the fixtures ever drifted so that another field differed too, this would silently become
    // a substituted-node case and would pass for the wrong reason.
    ASSERT_EQ(kRePermissionedNodeIdentity.device, kValidatedNodeIdentity.device);
    ASSERT_EQ(kRePermissionedNodeIdentity.inode, kValidatedNodeIdentity.inode);
    ASSERT_EQ(kRePermissionedNodeIdentity.rdev, kValidatedNodeIdentity.rdev);
    ASSERT_EQ(kRePermissionedNodeIdentity.uid, kValidatedNodeIdentity.uid);
    ASSERT_NE(kRePermissionedNodeIdentity.mode, kValidatedNodeIdentity.mode)
        << "this case is about the MODE and the two fixtures now agree on it, so it exercises "
           "nothing";

    DriverAidlImpl backEnd;
    bool verdict = true;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence diagnostic cannot be read; "
               "failing rather than asserting against an empty capture";

        verdict = backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                             DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                             syntheticProbe());

        captured = capture.read();
    }

    EXPECT_FALSE(verdict)
        << "the query proceeded to the service lookup after the validated binder node had been "
           "re-permissioned. The object is the same object, so nothing else in the path could "
           "have noticed: an attacker who cannot replace the node just widened access to it and "
           "the middleware certified the transport anyway";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr)
        << "a decline recorded no reason, so the factory has nothing to name in its fallback line";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a re-permissioned driver node was reported as a SERVICE problem. What changed is the "
           "transport, and naming the service would send an integrator looking in the wrong place";

    EXPECT_THAT(captured, ::testing::HasSubstr("divergence mask 0x08"))
        << "the decline did not report a MODE divergence (bit3). Either the mode is not being "
           "compared and something else refused this node, or the diagnostic no longer says which "
           "attribute moved - and without that an integrator cannot tell a substituted node from "
           "a re-permissioned one. Captured: [" << captured << "]";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified, so this case did not reach the comparison "
           "it is about";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor whose identity comparison had "
           "already failed, so the decline is happening later than it should";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors - the retained one and the one reopened inside the check - were "
           "expected to be released, and " << g_syntheticProbe.closeCalls << " release(s) were "
           "made. A decline that leaks the fresh descriptor leaks a driver context for the life "
           "of the process";
}

// THE SAME INODE, RE-OWNED between the check and the use: declined.
//
// Not implied by the mode case and deliberately separate. `chmod` and `chown` are different
// operations, a comparison can be written to cover one and miss the other, and the consequence
// here is worse: the driver node is handed to an unprivileged owner, who can then re-permission
// it at will. The preflight's own root-owner check cannot catch it, for the reason that makes
// this whole family of cases necessary - that check ran before the change.
//
// Everything except the owner is identical, so a decline can only come from the uid being
// compared, and bit4 of the divergence mask is what proves it did.
TEST_F(DriverAidlPreflightTest, TheServiceQueryDeclinesWhenTheValidatedNodeIsReOwnedBeforeTheLookup) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kReOwnedNodeIdentity;

    ASSERT_EQ(kReOwnedNodeIdentity.device, kValidatedNodeIdentity.device);
    ASSERT_EQ(kReOwnedNodeIdentity.inode, kValidatedNodeIdentity.inode);
    ASSERT_EQ(kReOwnedNodeIdentity.rdev, kValidatedNodeIdentity.rdev);
    ASSERT_EQ(kReOwnedNodeIdentity.mode, kValidatedNodeIdentity.mode);
    ASSERT_NE(kReOwnedNodeIdentity.uid, kValidatedNodeIdentity.uid)
        << "this case is about the OWNER and the two fixtures now agree on it, so it exercises "
           "nothing";

    DriverAidlImpl backEnd;
    bool verdict = true;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the divergence diagnostic cannot be read";

        verdict = backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                             DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                             syntheticProbe());

        captured = capture.read();
    }

    EXPECT_FALSE(verdict)
        << "the query proceeded to the service lookup after ownership of the validated binder "
           "node had been transferred to an unprivileged user. The preflight refuses such a node "
           "when it SEES it; refusing it only at check time and not at use time leaves the "
           "refusal a formality";

    ASSERT_NE(backEnd.unavailabilityReason(), nullptr) << "a decline recorded no reason";
    EXPECT_THAT(std::string(backEnd.unavailabilityReason()),
                ::testing::HasSubstr("binder transport is unavailable"))
        << "a re-owned driver node was not reported as a transport condition";

    EXPECT_THAT(captured, ::testing::HasSubstr("divergence mask 0x10"))
        << "the decline did not report an OWNER divergence (bit4), so either the uid is not being "
           "compared or the diagnostic cannot distinguish it from the other four attributes. "
           "Captured: [" << captured << "]";

    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was pinged over a descriptor that had already failed its identity "
           "comparison";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made";
}

// THE OVER-TIGHTENING GUARD: five attributes that all AGREE must still pass both comparisons.
//
// WHY THIS CASE IS NECESSARY AND WHY IT IS SHAPED THIS WAY. Adding mode and uid to the
// comparison is the kind of change that can be made too enthusiastically - comparing a value
// that legitimately differs between a `stat` of the path and an `fstat` of a descriptor, say,
// would decline every correctly provisioned platform, and a suite full of negative cases would
// not notice. So the positive direction needs an assertion of its own.
//
// It cannot be asserted by letting the query SUCCEED: that would enter halcompat::getService,
// which reaches defaultServiceManager() and aborts this process on a host with no binder driver
// (SIGABRT, taking every other case in the binary with it). What it does instead is let both
// identity comparisons pass and refuse the LAST of the four re-verification points - the second
// context-manager ping. Reaching that ping at all is the proof: it sits after the path
// comparison and after the reopened-descriptor comparison, so `pingCalls == 2` cannot happen
// unless all five attributes agreed, twice.
TEST_F(DriverAidlPreflightTest, TheServiceQueryAcceptsAnUnchangedNodeOnAllFiveIdentityAttributes) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);

    // Every route to an identity answers with the same node, mode and owner included. This is
    // what a healthy platform looks like across the window.
    g_syntheticProbe.answerDescriptorIdentity = kValidatedNodeIdentity;
    g_syntheticProbe.answerPathIdentity = kValidatedNodeIdentity;
    g_syntheticProbe.hasSecondDescriptorIdentity = true;
    g_syntheticProbe.answerDescriptorIdentitySecond = kValidatedNodeIdentity;

    // The one thing that declines, and it is deliberately the last point of the four.
    g_syntheticProbe.hasSecondPingAnswer = true;
    g_syntheticProbe.answerPingSecond = false;

    DriverAidlImpl backEnd;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid()) << "stdout could not be redirected";

        EXPECT_FALSE(backEnd.isServiceAvailable(DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
                                                DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
                                                syntheticProbe()))
            << "the query entered the service lookup although the second context-manager ping was "
               "refused; on this host that is an abort inside libbinder rather than a fallback";

        captured = capture.read();
    }

    EXPECT_EQ(g_syntheticProbe.identifyPathCalls, 1)
        << "the pathname was not re-resolved, so the first identity comparison never happened";
    EXPECT_EQ(g_syntheticProbe.identifyDescriptorCalls, 2)
        << "the reopened descriptor was not identified, so the second identity comparison never "
           "happened";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 2)
        << "the second context-manager ping was never reached, which means one of the two identity "
           "comparisons REFUSED A NODE THAT HAD NOT CHANGED. That is the over-tightening failure: "
           "the comparison must assert that the five attributes did not MOVE, never that they hold "
           "particular values, or the AIDL path is declined on every correctly provisioned "
           "platform";

    EXPECT_THAT(captured, ::testing::Not(::testing::HasSubstr("divergence mask")))
        << "an identity divergence was reported for a node whose five attributes all agree, so "
           "the comparison is finding a difference that does not exist. Captured: ["
        << captured << "]";

    EXPECT_EQ(g_syntheticProbe.closeCalls, 2)
        << "both descriptors were expected to be released and " << g_syntheticProbe.closeCalls
        << " release(s) were made";
}

// A GROUP- OR WORLD-WRITABLE BINDER NODE IS ACCEPTED, AND THE WARNING IS EMITTED.
//
// THIS CASE IS A REGRESSION GUARD AND ITS DIRECTION IS THE POINT. Refusing a permissive binder
// node reads like a security improvement and is a production-breaking bug: a binder device node
// must be openable by every client process that uses binder, so AOSP-derived platforms - and
// this port vendors AOSP android-13.0.0_r74 - publish it broadly accessible BY DESIGN, and a
// binderfs deployment takes whatever mode binderfs assigns. A restrictive-mode requirement
// would pass in this suite's own CI guest, where the node is root-owned and everything runs as
// root, and would DECLINE THE AIDL PATH ON EVERY CONFORMANT PRODUCTION PLATFORM - green where
// it is measured, migration defeated where it ships.
//
// So the verdict asserted here is TRUE, and the line is asserted as an OBSERVATION beside it:
// the middleware reports what it cannot require, which is the honest boundary. What it does
// require of the node is asserted by the two cases above it - character device, root owner -
// and what it enforces ACROSS the window is asserted by the two re-permissioned/re-owned cases.
TEST_F(DriverAidlPreflightTest, AcceptsAWorldWritableNodeAndReportsItsPermissionBits) {
    resetSyntheticProbe(kSyntheticBinderFd, 0, DriverAidlImpl::expectedBinderProtocolVersion(),
                        true);
    g_syntheticProbe.answerDescriptorIdentity = kWorldWritableNodeIdentity;

    // The premise: still a root-owned character device, so nothing ELSE in the preflight has
    // grounds to refuse it and the verdict below is about the permission bits alone.
    ASSERT_EQ(kWorldWritableNodeIdentity.mode & DriverAidlImpl::BINDER_NODE_MODE_TYPE_MASK,
              DriverAidlImpl::BINDER_NODE_MODE_CHARACTER_DEVICE)
        << "the node this case configures is not a character device, so it would be refused for a "
           "reason that has nothing to do with its permissions";
    ASSERT_EQ(kWorldWritableNodeIdentity.uid, DriverAidlImpl::BINDER_NODE_REQUIRED_OWNER_UID)
        << "the node this case configures is not owned by root, so it would be refused one arm "
           "earlier";
    ASSERT_NE(kWorldWritableNodeIdentity.mode & (DriverAidlImpl::BINDER_NODE_MODE_GROUP_WRITE |
                                                 DriverAidlImpl::BINDER_NODE_MODE_WORLD_WRITE),
              0u)
        << "the node this case configures is not writable beyond its owner, so there is nothing "
           "for the observation to report";

    bool verdict = false;
    std::string captured;

    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so the permissive-node line cannot be read; "
               "failing rather than asserting against an empty capture";

        verdict = DriverAidlImpl::isBinderPreflightOk(
            DriverAidlImpl::DEFAULT_BINDER_DRIVER_PATH,
            DriverAidlImpl::DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, syntheticProbe());

        captured = capture.read();
    }

    EXPECT_TRUE(verdict)
        << "the preflight DECLINED a root-owned character device because its mode was permissive. "
           "A binder node must be openable by every binder client, so this refuses the AIDL path "
           "on conformant production platforms while passing in a root-only CI guest. Node "
           "permission restrictiveness is not a property this middleware can require: the "
           "enforceable controls are the character-device and root-owner checks plus the "
           "five-attribute comparison across the check-to-use window";

    EXPECT_THAT(captured, ::testing::HasSubstr("is writable beyond its owner, permission bits 0666"))
        << "the permissive node was accepted silently. The line is the whole of what the "
           "middleware can contribute here - it is what tells an integrator that, on a platform "
           "which ALSO leaves service registration unauthorized, the precondition of HAL "
           "impersonation is present. Captured: [" << captured << "]";

    // Exactly one line, and it does not turn into a decline on a second call: the observation is
    // stateless, so a caller cannot be surprised by a different verdict the next time round.
    const size_t firstHit = captured.find("is writable beyond its owner");
    ASSERT_NE(firstHit, std::string::npos);
    EXPECT_EQ(captured.find("is writable beyond its owner", firstHit + 1), std::string::npos)
        << "the permissive-node condition was reported more than once in a single preflight, so "
           "the observation has been placed somewhere that runs repeatedly";

    // And the accounting is the accounting of a POSITIVE verdict with no custody requested:
    // one open, one protocol read, one ping, and the descriptor released before return.
    EXPECT_EQ(g_syntheticProbe.openCalls, 1) << "the node was not opened exactly once";
    EXPECT_EQ(g_syntheticProbe.protocolReadCalls, 1)
        << "the protocol read was skipped, so the observation short-circuited the arms after it";
    EXPECT_EQ(g_syntheticProbe.pingCalls, 1)
        << "the context manager was not pinged, so the observation short-circuited the arms after "
           "it";
    EXPECT_EQ(g_syntheticProbe.closeCalls, 1)
        << "the descriptor was not released on a positive verdict with no custody requested";
}

/**
 * @brief The resolved selection under invocation A: legacy, once, and stable thereafter.
 *
 * Requires invocation a - CEC_TEST_AIDL_MODE=absent, or unset, which means the same thing
 * (established by publishFakeForMode()). SetUp asserts that precondition rather than adapting to whatever
 * it finds, so a mis-wired --gtest_filter fails with a diagnostic naming the mode instead of
 * failing somewhere further in with no explanation.
 *
 * How the selection is observed, given that no production introspection API exists and none
 * was added. Two independent routes, both used: a dynamic_cast against the two concrete
 * types, which a test translation unit may name because neither ccec/src/DriverImpl.hpp nor
 * ccec/src/DriverAidlImpl.hpp is an installed header; and the selected-path log line, which
 * is the route the coverage runner and device-level validation use because the concrete
 * headers are out of scope for them.
 *
 * Self-sufficiency. No case here opens, closes or otherwise mutates the process-global
 * driver - they only ask the factory which object it holds, which is a pure read - so there
 * is no shared driver state to establish or restore and TearDown has nothing to do. The one
 * case that changes anything outside itself publishes a service, and its own comment records
 * exactly what that leaves behind and why it is harmless.
 */
class DriverAidlSelectionTest : public ::testing::Test {
protected:
    /**
     * @brief Establishes that the resolved back-end is the legacy one, and caches it
     *
     * @pre Invocation A: CEC_TEST_AIDL_MODE is `absent` or unset, so the factory resolved to
     *      DriverImpl. The selection resolves once per process inside LibCCEC::init and
     *      cannot be changed from here, so this is asserted rather than arranged.
     *
     * @post @c legacyBackEnd holds the resolved instance, non-null.
     */
    void SetUp() override {
        legacyBackEnd = dynamic_cast<DriverImpl *>(&Driver::getInstance());

        ASSERT_NE(legacyBackEnd, nullptr)
            << "this fixture requires the legacy back-end to be the resolved one, i.e. invocation "
               "A (CEC_TEST_AIDL_MODE=absent or unset), and the factory returned something else. "
               "The selection resolves once per process inside LibCCEC::init, so it cannot be "
               "changed from here; re-run with the invocation-A filter from the fixture manifest "
               "above instead";
    }

    /** @brief The resolved legacy back-end, cached by SetUp() for the case bodies. */
    DriverImpl *legacyBackEnd = nullptr;
};

// SC6(b), first half: with no AIDL service registered the factory selects the legacy
// back-end. Asserted by concrete type, in both directions - it is a DriverImpl and it is not
// a DriverAidlImpl - because a one-sided assertion would also pass against some third
// implementation, and because the two casts together are what make "exactly one of the two"
// an assertion rather than a description.
/**
 * @brief SC6(b), first half: with no AIDL service published the factory selects the legacy
 *        back-end.
 * @pre Invocation A, established by the fixture's SetUp.
 * @note Evidence: two casts against the concrete types, in both directions. A one-sided
 *       assertion would also pass against some third implementation, so the pair is what
 *       makes "exactly one of the two" an assertion rather than a description.
 */
TEST_F(DriverAidlSelectionTest, AbsentServiceSelectsTheLegacyBackEnd) {
    Driver &resolved = Driver::getInstance();

    EXPECT_NE(dynamic_cast<DriverImpl *>(&resolved), nullptr)
        << "the resolved back-end is not the legacy one, although no AIDL service is registered";

    EXPECT_EQ(dynamic_cast<DriverAidlImpl *>(&resolved), nullptr)
        << "the resolved back-end is the AIDL one although no AIDL service is registered, so the "
           "selection is not resting on service availability at all";
}

// SC6(b), second half: the selected-path log line names the legacy back-end.
//
// The line the factory actually emitted went to the process log during LibCCEC::init, before
// any test body existed, so it cannot be captured here - and re-emitting it into the process
// log would break the runner's one-hit-per-process grep. What this case does instead is
// drive the production logger with the transcribed contract format, into an anonymous
// temporary, and assert the line that comes out. That proves three things a pair of local
// string comparisons could not: the format is one CCEC_LOG accepts and substitutes the
// back-end name into; the line still survives the default log-level filter, so a lowered
// default that silently broke every consumer of the line is caught here; and the substituted
// name agrees with what the dynamic_cast above established.
/**
 * @brief SC6(b), second half: the selected-path log line names the legacy back-end and
 *        survives the default log-level filter.
 * @pre Invocation A. The line the factory itself emitted went to the process log during
 *      LibCCEC::init, before any test body existed, so it cannot be captured here - and
 *      re-emitting it into the process log would break the runner's one-hit-per-process
 *      grep. This case drives the production logger with the transcribed contract format
 *      into an anonymous temporary instead.
 * @note Evidence: the format's shape is asserted first - exactly one substitution and the
 *       \r\n terminator - then the captured line is asserted to carry the substituted
 *       name, and the two back-end names are asserted distinguishable. That establishes
 *       three things local string comparisons could not: the format is one CCEC_LOG
 *       accepts, the line is not filtered out by the default level, and the name agrees
 *       with what the cast established.
 */
TEST_F(DriverAidlSelectionTest, SelectedPathLogLineNamesTheLegacyBackEnd) {
    // The format is a contract, so its shape is asserted before it is used: exactly one
    // substitution, and the \r\n terminator every CCEC log line carries.
    const std::string format(kSelectedBackEndLogFormat);
    ASSERT_EQ(format.find("%s"), format.rfind("%s"))
        << "the transcribed selected-path format carries more than one substitution, so it no "
           "longer matches SELECTED_BACK_END_LOG_FORMAT in ccec/src/Driver.cpp, where the "
           "back-end name is the only part that varies";
    ASSERT_NE(format.find("%s"), std::string::npos)
        << "the transcribed selected-path format has no substitution at all, so it cannot name a "
           "back-end";
    ASSERT_GE(format.size(), 2u);
    EXPECT_EQ(format.substr(format.size() - 2), "\r\n")
        << "the selected-path format no longer ends in \\r\\n, which every CCEC_LOG line in this "
           "middleware carries and which the runner's line-oriented grep relies on";

    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so nothing about the emitted line can be "
               "established; failing rather than asserting against an empty capture";

        CCEC_LOG(LOG_INFO, kSelectedBackEndLogFormat, kSelectedBackEndLegacy);

        captured = capture.read();
    }

    const std::string expected =
        std::string("HDMI CEC HAL back-end selected : ") + kSelectedBackEndLegacy;

    EXPECT_NE(captured.find(expected), std::string::npos)
        << "the production logger did not emit the selected-path line naming the legacy back-end. "
           "Either the transcribed format at the top of this file has drifted from "
           "SELECTED_BACK_END_LOG_FORMAT and SELECTED_BACK_END_LEGACY in ccec/src/Driver.cpp, or "
           "the default CEC log level is now below LOG_INFO and the "
           "line is being filtered out - in which case every consumer of it, this suite included, "
           "is reading an empty log. Captured instead: [" << captured << "]";

    // The two back-end names must be distinguishable, or the line could not identify either.
    EXPECT_STRNE(kSelectedBackEndLegacy, kSelectedBackEndAidl)
        << "the two back-end names in the log contract are now identical, so the line cannot say "
           "which back-end was selected";
}

// SC6(c), first half: the factory hands back the same object every time.
//
// Driver::getInstance(), in ccec/src/Driver.cpp, holds a reference to a function-local static
// - cited by name rather than by line, as the closing note of this file requires of any
// citation into a file it does not own - so this is what makes "resolved once" observable: a
// factory that re-decided per call, or returned a fresh object, would break the state
// machine every caller depends on - Bus's reader and writer threads and LibCCEC would each
// be talking to a different driver.
/**
 * @brief SC6(c), first half: the factory hands back the same object on every call.
 * @pre Invocation A, though the property is independent of which back-end resolved.
 * @note Evidence: three successive calls yield one address. This is what makes "resolved
 *       once" observable: a factory that re-decided per call, or returned a fresh object,
 *       would leave Bus's reader thread, its writer thread and LibCCEC each talking to a
 *       different driver.
 * @see Driver::getInstance()
 */
TEST_F(DriverAidlSelectionTest, FactoryReturnsTheSameObjectOnEveryCall) {
    Driver *first = &Driver::getInstance();
    Driver *second = &Driver::getInstance();
    Driver *third = &Driver::getInstance();

    ASSERT_NE(first, nullptr);

    EXPECT_EQ(first, second) << "two consecutive calls to Driver::getInstance() returned different "
                               "objects, so the back-end is not a stable singleton";
    EXPECT_EQ(second, third) << "a third call returned yet another object";
    EXPECT_EQ(first, static_cast<Driver *>(legacyBackEnd))
        << "the object the factory returns is not the one SetUp resolved, so the identity "
           "established there does not describe what the rest of this suite is talking to";
}

// SC6(c), second half, and the one that actually needs arranging: registering a service
// mid-process does not change an already-resolved selection.
//
// This is the property that makes a session deterministic. The selection resolves once,
// inside LibCCEC::init, and is then fixed for the lifetime of the process - so a HAL that
// comes up late must not be picked up half way through, leaving one part of a session
// talking to the legacy back-end and another to the AIDL one.
//
// The direction is forced. Proving stability by taking a service away is not expressible:
// the pinned C++ IServiceManager has no service-removal API and retains a reference to
// whatever was published, so there is nothing to deregister - see unreachable path (4) in
// the file block. Adding one is the direction that can be expressed, and it is also the more
// dangerous direction in practice, since it is the one a late-starting HAL produces.
//
// The preflight is not optional here. registerFakeHdmiCecService() guards on the driver node
// itself and so cannot abort, but it does not guard the other hazard: on a host that has a
// driver node and no running servicemanager, defaultServiceManager() blocks until binder
// handle 0 resolves, with no bound, which would hang this binary rather than fail it.
// isBinderPreflightOk() covers both, which is why it is asked first here exactly as the
// harness asks it in publishFakeForMode().
//
// Two forms, and the strong one is mandatory wherever it is possible. The preflight's answer
// decides which form this case runs, and the difference is not cosmetic:
//
//   Strong form, taken whenever the preflight passes - a service really is published while
//   the process is running, and the publication succeeding is asserted. Only then does
//   "the resolved back-end did not change" mean anything, because only then did anything
//   appear for it to change to.
//
//   Weak form, taken only where the preflight declines - no service can be published at all,
//   so the assertions below degenerate into the repeated-singleton identity that
//   FactoryReturnsTheSameObjectOnEveryCall already covers. That is not evidence for SC6(c)
//   and the case says so, in its own log line and in every failure message below, so that a
//   green result on a driverless host cannot be read as the strong form having run.
//
// Why the verdict decides the form rather than the outcome being reported either way. A case
// that only reported a refused publication would make the strong form unreachable: a runner
// where addService refused would print "was refused" and still pass, so the evidence would
// silently be absent on precisely the hosts that can produce it. Asking the preflight first
// binds the form to the environment, and the form is stated in every message.
//
// What this leaves behind, and why it is harmless. Where the registration does succeed, the
// fake stays published for the rest of the process, because there is no way to withdraw it.
// No sibling suite is affected: the selection is already resolved and cannot be re-decided,
// nothing else in this binary looks the name up, and the entry lives only as long as this
// process - the service manager's reference dies with it. The strong reference is held in a
// function-local static rather than in the fixture so that the published binder outlives
// this case; dropping it at the end of the case would leave the service manager advertising
// a destroyed object, which is a worse state than leaving it advertising a live one.
/**
 * @brief SC6(c), second half: publishing a service after the selection has resolved does not
 *        change which back-end the factory holds.
 * @pre Invocation A. Adding a service is the only direction that can be expressed - the
 *      pinned C++ IServiceManager offers no withdrawal - and it is also the dangerous one
 *      in practice, since it is what a late-starting HAL produces.
 * @note Two forms, and the preflight's verdict decides which runs. Where it passes, a
 *       service really is published and the publication is asserted, so "the resolved
 *       back-end did not change" has something to have changed to. Where it declines, the
 *       assertions reduce to the repeated-singleton identity the previous case already
 *       covers; that is not SC6(c) evidence, and the case says so in its own log line and
 *       in every failure message, so a green result on a driverless host cannot be misread.
 * @warning Where publication succeeds the fake stays published for the rest of the process,
 *          held in a function-local static so the binder outlives the case. Nothing else in
 *          this binary looks the name up, the selection cannot be re-decided, and the entry
 *          dies with the process - whereas dropping the reference here would leave the
 *          service manager advertising a destroyed object.
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
TEST_F(DriverAidlSelectionTest, RegisteringAServiceMidProcessDoesNotChangeTheResolvedBackEnd) {
    Driver *before = &Driver::getInstance();
    ASSERT_EQ(dynamic_cast<DriverAidlImpl *>(before), nullptr)
        << "the AIDL back-end was already selected, so this case cannot demonstrate that a "
           "mid-process registration fails to switch to it";

    static ::android::sp<FakeHdmiCecService> latePublishedFake;

    // The environment's own answer decides the form, and it is asked exactly once so that
    // both the branch taken and every message below describe the same verdict.
    const bool canPublish = DriverAidlImpl::isBinderPreflightOk();

    // Named once and reused in every message, so a failure states which form produced it
    // rather than leaving a reader of the log to work it out.
    const char *const form = canPublish
        ? "strong form (a service was published mid-process)"
        : "weak form (this host cannot publish a service, so nothing appeared for the selection "
          "to change to, and SC6(c) is not established by this run)";

    if (canPublish) {
        latePublishedFake = ::android::sp<FakeHdmiCecService>::make();
        ASSERT_TRUE(latePublishedFake != nullptr) << "the fake service could not be constructed";

        // Asserted, not reported. The preflight has just said that this host has a usable
        // binder transport and a reachable context manager, so a refusal here is a real
        // failure of the environment this case depends on - and tolerating it would leave the
        // strong form silently unreached on precisely the runners that can reach it.
        ASSERT_TRUE(registerFakeHdmiCecService(latePublishedFake))
            << "the binder preflight passed, so this host can publish a service, and the service "
               "manager refused this one anyway. Without a successful publication there is nothing "
               "for the selection to have been tempted by, so the assertions below would prove only "
               "that a singleton is a singleton - which "
               "DriverAidlSelectionTest.FactoryReturnsTheSameObjectOnEveryCall already proves. "
               "Fix the environment rather than weakening this case: check that servicemanager is "
               "running and that nothing else holds the name";

        std::cout << "[DriverAidlSelectionTest] " << form
                  << ": the fake was published while this process was running, and the selection "
                     "invariant is now asserted against a service that really is there"
                  << std::endl;
    } else {
        std::cout << "[DriverAidlSelectionTest] " << form
                  << ": the binder preflight declined, so no service was published and the "
                     "assertions below reduce to repeated-singleton identity. Re-run this case on a "
                     "binder-capable runner to obtain the SC6(c) evidence"
                  << std::endl;
    }

    Driver *after = &Driver::getInstance();

    EXPECT_EQ(after, before)
        << "the factory returned a different object after a service was registered, so the "
           "selection is being re-decided per call rather than held for the process. Form: " << form;

    EXPECT_NE(dynamic_cast<DriverImpl *>(after), nullptr)
        << "the resolved back-end is no longer the legacy one. A service that appears after "
           "initialization must NOT be adopted: half a session on each back-end is precisely the "
           "non-determinism resolving once is there to prevent. Form: " << form;

    EXPECT_EQ(dynamic_cast<DriverAidlImpl *>(after), nullptr)
        << "the AIDL back-end was adopted mid-process. Form: " << form;

    // No test property is recorded for the form on purpose: the runner consumes this suite's
    // JSON, and a key it does not know is a change to a contract another file owns. The form
    // is on stdout, which is where the runner's per-invocation log keeps it, and in every
    // failure message above, which is where a reader needs it.
}

/**
 * @brief The AIDL back-end's closed-state behaviour, on a local instance.
 *
 * Runs under every invocation, and that is the whole point of the fixture. DriverAidlImpl's
 * constructor touches no binder at all - it sets the state to CLOSED, the legacy handle
 * field to 0 and an empty address list (DriverAidlImpl::DriverAidlImpl()), exactly as
 * DriverImpl's does (DriverImpl.cpp:87-90) - which is precisely what makes the factory's
 * construct-then-query order safe on a legacy-only SOC. The consequence for testing is
 * large: a local instance is constructible even where no service, no service manager and no
 * binder driver exist, so every status != OPENED guard, the writeAsync prelude ordering, and
 * the three methods that carry no guard at all are reachable without a HAL of any kind.
 *
 * A local instance is used rather than the shared driver, following the idiom the
 * neighbouring async suite established (test_DriverImpl_Async.cpp:515, :532, :543, :556,
 * :567, :581): the process-global driver is open, every other suite in this binary shares
 * it, and closing it to reach a guard would hand a closed driver to whichever suite runs
 * next.
 *
 * And a local instance is never OPENED here, which is a hazard worth naming rather than
 * discovering. IHdmiCec::open() is single-instance and fails EX_ILLEGAL_STATE if a session
 * is already held, so opening a local instance under invocation B - where the
 * process-global AIDL back-end holds the session - is a designed-in failure. It happens
 * that a local instance cannot open at all, on any invocation: its service proxy is only
 * ever cached by its own isServiceAvailable(), so open() raises IOException first
 * (the no-proxy guard at the head of DriverAidlImpl::open()) - which is itself asserted
 * below, and is what keeps every
 * instance here CLOSED and its destructor off the close path.
 *
 * Self-sufficiency. Every case constructs its own instance inside its own body and lets it
 * go out of scope at the end; nothing is shared between cases and nothing outside the
 * fixture is written. The HAL mock is consulted only through Times(0) expectations - what
 * these cases assert about it is that it is not reached - and those are cleared in TearDown.
 */
/* ------------------------------------------------------------------------------------------
 * the receive-queue ownership handoff, and what it takes to test it.
 *
 * The helpers below and the five cases at the end of this fixture's group exist for one
 * production guarantee: DriverAidlImpl::offerReceivedFrame() must report whether the incoming
 * queue actually took a frame, and close()'s NULL sentinel must be serialized against it on
 * queueProducerMutex so that the receive path's occupancy observation cannot go stale. Neither
 * property is reachable through the public surface on a host with no binder driver, because
 * the OPENED state only arrives through open() and open() needs a live compatible AIDL
 * service - hence the test-local subclass, and hence `protected` on the back-end's internal
 * section.
 *
 * Everything here has internal linkage: it is one anonymous namespace, so nothing it declares
 * can collide with a name in another translation unit of this suite.
 * ------------------------------------------------------------------------------------------ */

namespace {

/**
 * @brief A DriverAidlImpl whose incoming queue can be driven directly
 *
 * Why a subclass and not the instance itself. offerReceivedFrame() only accepts a frame while
 * the state is OPENED, and the state only becomes OPENED through open(), which needs a live
 * compatible AIDL service - so on this host, with no binder driver, the ownership contract
 * this turns on would have no coverage at all through the public surface. The back-end's internal
 * section is `protected` for exactly this reason (ccec/src/DriverAidlImpl.hpp), so a test-local
 * subclass can establish the precondition and call the handoff, on any host, with no service,
 * no listener and no threadpool - none of which the queue handoff touches.
 *
 * Forcing the state field is not the same as holding a session, and the distinction is what
 * keeps this honest: markOpened() writes the middleware's own lifecycle field and nothing else,
 * so what these cases exercise is precisely the queue arithmetic, the producer serialization
 * and the ownership report. Anything that would dereference a proxy is not reachable from here
 * and is not asserted here.
 *
 * It cleans up after itself on every exit path. The destructor drains whatever the queue still
 * holds and returns the state to CLOSED, so a case that ends early on a failed expectation
 * neither leaks a frame nor leaves the base destructor to run close() into an EventQueue that
 * would then report leftover elements.
 */
class ReceiveQueueProbe : public DriverAidlImpl {
public:
    ~ReceiveQueueProbe() override {
        drain();
        markClosed();
    }

    /** @brief Puts the instance into the OPENED state the receive handoff requires. */
    void markOpened(void) { status = OPENED; }

    /** @brief Returns the instance to CLOSED, so no destructor takes the close path. */
    void markClosed(void) { status = CLOSED; }

    /**
     * @brief Puts the instance into CLOSING, the state close() sets before it offers its sentinel.
     *
     * The state a blocked reader must observe for read() to take its FLUSH arm. Reachable only
     * from here: production sets it inside close(), between the guard and the HAL call, so no
     * case could otherwise observe read() while the instance is in it.
     */
    void markClosing(void) { status = CLOSING; }

    /**
     * @brief Offers one NULL close sentinel exactly as close() does, under the producer lock.
     *
     * WHY THE PRODUCER LOCK IS TAKEN HERE TOO. close() offers its sentinel under
     * queueProducerMutex because it is a producer on this queue and not merely its
     * terminator; a helper that offered without it would be modelling a close that does not
     * exist. Calling this twice models the two-sentinel state a stop/reopen/close sequence
     * leaves - one sentinel per transition out of OPENED - which is the state read()'s flush
     * loop must survive.
     */
    void postCloseSentinel(void) {
        {CCEC_OSAL::AutoLock lock_(queueProducerMutex);
            rQueue.offer(0);
        }
    }

    /**
     * @brief Places a real frame on the queue the way one that was already there got there.
     *
     * THE ONE PRODUCER PATH THAT SKIPS THE OPENED GUARD, and it models a real state rather than
     * inventing one. offerReceivedFrame() refuses once the instance stops being OPENED, so a
     * frame cannot be ADDED during a close - but a frame accepted while the instance WAS opened
     * is still in the queue when close() runs, and read()'s flush is the code that meets it.
     * offer() cannot express that: a frame offered before the close is consumed by the reader's
     * own poll, so the flush never sees it, and a frame offered after the close is refused.
     *
     * @param [in] frame - Heap frame to place. Ownership passes to the queue, and read()'s flush
     *                     releases it exactly as it releases one the HAL delivered.
     *
     * @pre The caller holds the instance lock, so the reader is parked between its poll and its
     *      state re-check and cannot consume this entry before the flush does.
     * @post The frame is queued behind whatever was already there.
     * @warning Takes queueProducerMutex, exactly as every other producer on this queue does, so
     *          the occupancy this places is visible to the check in offerReceivedFrame().
     *
     * @see postCloseSentinel()
     */
    void postFrameBehindSentinel(CECFrame *frame) {
        {CCEC_OSAL::AutoLock lock_(queueProducerMutex);
            rQueue.offer(frame);
        }
    }

    /**
     * @brief The instance lock read() takes to re-check the state, for a test to hold.
     *
     * THE GATE THAT MAKES THE FLUSH ARM DETERMINISTIC. read() polls the queue and only then
     * takes this lock to re-check the state, so a test that holds it can place a second
     * sentinel in the queue while the reader is already past its poll and cannot yet see the
     * state change. Without the gate the reader would consume both sentinels in its
     * poll-and-loop path and never reach the flush at all.
     *
     * @warning The holder must not also take queueProducerMutex through producerLock():
     *          close() takes the instance lock first and the producer lock inside it, and
     *          reversing that here would invert the only nesting order that exists.
     *          postCloseSentinel() takes the producer lock ALONE, so it is safe to call
     *          while this one is held - which is exactly what the two-sentinel case does.
     */
    Mutex &instanceLock(void) { return mutex; }

    /**
     * @brief Calls the production handoff and reports exactly what it reported.
     *
     * @param [in] frame Frame to hand off. Ownership passes to the queue only if this
     *                  returns true; on false the caller still owns it.
     * @return bool - Exactly what DriverAidlImpl::offerReceivedFrame() reported. Nothing is
     *                reinterpreted here, because the agreement between this value and what
     *                the queue actually holds is the property under test.
     */
    bool offer(CECFrame *frame) { return offerReceivedFrame(frame); }

    /**
     * @brief The lock every producer on the incoming queue must take, for the test to hold.
     *
     * The one thing that makes the close-side acquisition observable. The defect it guards
     * against was a
     * missing lock acquisition in close(), and a serial case cannot see a lock that is not
     * taken. A case that holds this lock and then drives the real close() from another
     * thread can: while it is held, close()'s sentinel offer must not complete, and
     * completing anyway is the regression itself.
     *
     * Handing out the lock rather than a "take it for me" helper is deliberate - the test
     * holds it across a scope of its own, under CCEC_OSAL::AutoLock, exactly as production
     * does.
     *
     * @return Mutex& - DriverAidlImpl::queueProducerMutex, this instance's own.
     *
     * @warning The holder must not also take the instance mutex. close() takes the instance
     *          lock first and this one inside it, so a test holding this one and then
     *          reaching for that one would invert the only nesting order that exists.
     *          Nothing these cases call needs it: occupancy() and take() reach the
     *          EventQueue's own internal lock and nothing else.
     */
    Mutex &producerLock(void) { return queueProducerMutex; }

    /**
     * @brief Current occupancy of the incoming queue.
     *
     * @return size_t - Entries currently queued, sentinels included. A figure alone cannot
     *                  tell a swallowed sentinel from one frame too many, which is why the
     *                  counting drains exist alongside it.
     */
    size_t occupancy(void) { return rQueue.size(); }

    /**
     * @brief Takes one entry exactly as the Bus reader does.
     *
     * @return CECFrame* - The frame taken, or NULL for close()'s wake-the-reader sentinel.
     *                     Only call this while occupancy() is non-zero: EventQueue::poll()
     *                     blocks on an empty queue, which is the reader's contract too.
     */
    CECFrame *take(void) { return rQueue.poll(); }

    /**
     * @brief Empties the queue, counting frames and close sentinels apart and releasing frames.
     *
     * The counting drain. Distinguishing the two is what turns "the queue holds N entries"
     * into "the queue holds these frames and close()'s one sentinel", which is the assertion
     * the reserved-slot rule is actually about - an occupancy figure alone cannot tell a
     * swallowed sentinel from one frame too many.
     *
     * @param [out] frames    - Receives the number of non-NULL entries taken. Each one is
     *                          released here, so the caller must not hold a pointer to any.
     * @param [out] sentinels - Receives the number of NULL entries taken, i.e. the number of
     *                          close() sentinels the queue really accepted.
     *
     * @pre Nothing else is producing onto the queue. EventQueue::poll() blocks on an empty
     *      queue, so this drains strictly while the occupancy it reads is non-zero.
     */
    void drainCounting(size_t &frames, size_t &sentinels) {
        std::vector<CECFrame *> taken;

        drainInto(taken, sentinels);

        frames = taken.size();

        for (size_t i = 0; i < taken.size(); i++) {
            delete taken[i];
        }
    }

    /**
     * @brief Empties the queue, retaining each frame's identity and counting sentinels apart.
     *
     * The variant the ownership cases need. Counting frames establishes how many the queue
     * took; naming them establishes which ones it took, which is what an exactly-one-owner
     * ledger is built from - a count cannot distinguish "the queue kept the frame the caller
     * also released" from "the queue kept a different frame".
     *
     * @param [out] frames    - Receives every non-NULL entry, in queue order. Ownership
     *                          passes to the caller: nothing is released here.
     * @param [out] sentinels - Receives the number of NULL entries taken, i.e. the number of
     *                          close() sentinels the queue really accepted.
     *
     * @pre Nothing else is producing onto the queue. EventQueue::poll() blocks on an empty
     *      queue, so this drains strictly while the occupancy it reads is non-zero.
     */
    void drainInto(std::vector<CECFrame *> &frames, size_t &sentinels) {
        sentinels = 0;

        while (occupancy() > 0) {
            CECFrame *entry = take();

            if (entry == NULL) {
                sentinels++;
            }
            else {
                frames.push_back(entry);
            }
        }
    }

    /**
     * @brief Releases everything still queued and reports how many entries were taken.
     *
     * Expressed through drainCounting() rather than by polling the queue itself, so that
     * take() stays the one place in this fixture that touches EventQueue::poll(). That is
     * not tidiness: EventQueue::poll() leaves its `E front;` local unassigned on the arm
     * where the queue is emptied between the size read and the poll, so every distinct
     * call site of it produces its own pre-existing -Wmaybe-uninitialized diagnostic, and
     * osal/include/osal/EventQueue.hpp is out of bounds for this migration. Funnelling
     * through take() keeps that diagnostic count where it was rather than adding to it.
     *
     * @return size_t - How many entries were taken in total, frames and sentinels together.
     *                  Every frame is released, so the caller must not retain a pointer to any.
     */
    size_t drain(void) {
        size_t frames = 0;
        size_t sentinels = 0;

        drainCounting(frames, sentinels);

        return frames + sentinels;
    }
};

/**
 * @brief A sticky latch whose timed wait is bounded on a monotonic clock.
 *
 * Why this exists rather than CCEC_OSAL::ConditionVariable, which is what the concurrent
 * cases would otherwise signal completion through. The OSAL primitive is right in shape - a
 * timed wait on a sticky
 * condition, so a worker that finishes before the test thread starts waiting is still
 * observed - and wrong in two details that a bounded observation cannot tolerate:
 *
 * - its timed wait computes an absolute deadline and normalises only
 *   `tv_nsec > 1000000000`, not equality, so a wait begun at exactly a 600000-microsecond
 *   boundary hands pthread_cond_timedwait a tv_nsec of exactly 1000000000, which is invalid;
 *   the call returns EINVAL immediately and the wait can spin on it instead of waiting;
 * - it waits on CLOCK_REALTIME, so an NTP step or a manual clock adjustment moves the
 *   deadline underneath a wait already in progress - a wait bounded at 400 ms can expire
 *   instantly or last far longer than its bound.
 *
 * Both live in osal/, which this migration does not touch, so the bound is established here
 * instead: std::chrono::steady_clock cannot be stepped, and the predicate form of
 * wait_until() re-checks the deadline against that same clock on every spurious wake, so the
 * bound holds whatever the wall clock does. The OSAL types are still used where no timed wait
 * is involved - the test thread holds queueProducerMutex under CCEC_OSAL::AutoLock, exactly as
 * production does.
 *
 * Stickiness is load bearing and is preserved: notify() sets the flag as well as waking
 * waiters, so a wait that starts after the notify still returns true, and a false return means
 * a genuine timeout rather than a missed signal.
 */
class MonotonicLatch {
public:
    /**
     * @brief Creates a clear latch.
     *
     * @post The latch is clear, so a wait begun now blocks until notify() or the bound.
     */
    MonotonicLatch(void) : signalled(false) {}

    /** @brief Sets the latch and wakes every waiter. Idempotent, and safe from any thread. */
    void notify(void) {
        {
            std::lock_guard<std::mutex> held(guard);
            signalled = true;
        }

        wakeUp.notify_all();
    }

    /**
     * @brief Waits for the latch for at most @p boundMs milliseconds of monotonic time.
     *
     * @param [in] boundMs - The bound in milliseconds, measured on std::chrono::steady_clock.
     * @return bool - true when the latch was set within the bound, false on a genuine timeout.
     */
    bool wait(long boundMs) {
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);

        std::unique_lock<std::mutex> held(guard);

        return wakeUp.wait_until(held, deadline, [this]() { return signalled; });
    }

    /**
     * @brief Reports the latch's state without waiting at all.
     *
     * @return bool - Whether the latch has been set. Safe from any thread; the read is taken
     *                under the latch's own mutex.
     */
    bool isSignalled(void) {
        std::lock_guard<std::mutex> held(guard);

        return signalled;
    }

private:
    std::mutex              guard;
    std::condition_variable wakeUp;
    bool                    signalled;

    MonotonicLatch(const MonotonicLatch &);
    MonotonicLatch & operator = (const MonotonicLatch &);
};

/**
 * @brief How long a worker is given to finish before it is abandoned rather than waited on.
 *
 * The bound on the disposition path, and it exists because an unconditional join() there
 * defeats every other bound in these cases. The disposition runs after the case body has
 * finished - including after an early return from a failed assertion - and by then the test
 * thread has released the producer lock, so what a worker can have left is one
 * CCEC_OSAL::EventQueue::offer() and, for close(), a transaction that fails immediately on an
 * instance holding no proxy.
 *
 * What that offer does and does not guarantee, because this deadline must not be read as
 * resting on more than the queue provides. offer() takes the queue's own mutex under
 * CCEC_OSAL::AutoLock, so it can wait on whatever holds that mutex; it may allocate, because
 * it appends through std::deque::push_back; and it signals its condition variable while still
 * holding the lock. The one wait it never performs is a wait for capacity: a full queue
 * discards the element instead of parking the producer. So there is no unbounded wait inside
 * it, but no constant-time or nonblocking property either.
 *
 * This deadline is consequently an empirical bound rather than one derived from that work, and
 * it is deliberately generous: two seconds is orders of magnitude above what these workers
 * measure at on this host, so a worker still running when it expires has stopped making
 * progress rather than merely being slow. Its purpose is to keep the disposition path off an
 * unbounded join at all - waiting for ever would turn a reported failure into a hung suite,
 * because the assertion that fired on one of the timeouts above would unwind straight into an
 * unbounded join on the very worker it just reported as not completing - and not to certify
 * how long the remaining work takes.
 *
 * Shorter than PRODUCER_COMPLETION_TIMEOUT_MS deliberately. That timeout is the one a passing
 * run must never hit and can afford to be generous; this one is only ever reached once a
 * failure has already been reported, where the remaining value is a prompt report rather than
 * one more chance for a broken worker to finish.
 */
const long WORKER_ABANDON_DEADLINE_MS = 2000;

/**
 * @brief One worker thread and the bounded disposition every exit path takes.
 *
 * Why this exists rather than a bare join() at the end of the case. The concurrent cases below
 * park a worker on queueProducerMutex on purpose, and they use ASSERT_* while it is parked -
 * and ASSERT_* returns from the case body. A joinable std::thread destroyed by that return
 * calls std::terminate, which would replace a reported failure with an abort and take every
 * remaining case in the binary with it.
 *
 * And why it is not a joiner either, which is the sharper point. An unconditional join on the
 * disposition path is exactly as bad in the one situation the bounds exist for: if a bounded
 * completion assertion fails because a worker is genuinely stuck, unwinding runs the join and
 * the run hangs on the very thread the assertion just reported as not completing. So disposal
 * waits for the body to return, for at most WORKER_ABANDON_DEADLINE_MS of monotonic time, and
 * takes one of two routes:
 *
 * - the body returned: join(), which from that point waits out thread teardown and nothing
 *   else, because the latch this class sets is the body's last statement;
 * - the body did not return: detach(), which does not wait for the thread - that is the whole
 *   of the property relied on here, rather than any claim about how long the call itself takes.
 *   The state the abandoned thread can still reach is what QueueHandoffOverlapHarness then
 *   leaks by design - see there.
 *
 * The declaration order is still part of the mechanism, so it is stated rather than left to be
 * rediscovered. The harness that owns these workers is declared in the case body outside the
 * scope that holds the lock, and the lock is taken in an inner scope. Locals are destroyed in
 * reverse order of declaration, so an early return unwinds the inner scope first - releasing
 * the lock, which is what lets a parked worker finish - and only then disposes the workers.
 * Disposing while the lock was still held would make every disposal hit its deadline and
 * abandon a worker that was merely parked.
 */
class BoundedWorker {
public:
    /**
     * @brief Creates a worker holding no thread; start() is what launches one.
     */
    BoundedWorker(void) {}

    /**
     * @brief Last-resort disposition, unreachable in this file's own use.
     *
     * QueueHandoffOverlapHarness disposes every worker before it deletes any, so a joinable thread
     * here would mean dispose() was never called. Detaching rather than joining keeps even
     * this path off an unbounded wait; the harness's leak-by-design rule is what keeps the
     * detached thread's state alive, and it never deletes a worker it abandoned.
     */
    ~BoundedWorker(void) {
        if (worker.joinable()) {
            worker.detach();
        }
    }

    /**
     * @brief Runs @p body on a new thread and sets the finished latch when it returns.
     *
     * @param [in] body - The worker body. Copied into the thread, so it must capture the
     *                    heap-resident QueueHandoffOverlapState pointer and nothing on the test
     *                    stack: an abandoned worker outlives the case body.
     */
    template <typename Body>
    void start(Body body) {
        worker = std::thread([this, body]() {
            body();
            finished.notify();
        });
    }

    /**
     * @brief Bounded, monotonic wait for the body to return.
     *
     * @param [in] boundMs - The bound in milliseconds, on std::chrono::steady_clock.
     * @return bool - true when the body has returned, false on a genuine timeout.
     */
    bool finishedWithin(long boundMs) { return finished.wait(boundMs); }

    /**
     * @brief Reports whether the body has returned, without waiting.
     *
     * @return bool - Whether the body has returned. This is the body's own completion, not
     *                the thread's teardown, which is the distinction the bounded disposition
     *                rests on.
     */
    bool hasFinished(void) { return finished.isSignalled(); }

    /**
     * @brief Bounded disposition: join a finished worker, abandon one that is not.
     *
     * @param [in] boundMs - How long to wait for the body to return before abandoning.
     * @return bool - true when the worker was joined, false when it had to be detached, in
     *                which case the caller must not release anything the thread can reach.
     */
    bool dispose(long boundMs) {
        if (!worker.joinable()) {
            return true;
        }

        if (!finished.wait(boundMs)) {
            worker.detach();
            return false;
        }

        worker.join();

        return true;
    }

private:
    std::thread    worker;
    MonotonicLatch finished;

    BoundedWorker(const BoundedWorker &);
    BoundedWorker & operator = (const BoundedWorker &);
};

/**
 * @brief Everything a concurrent producer-overlap worker can touch, allocated off the test stack.
 *
 * The counterpart of BoundedWorker's abandonment route. A detached worker may still be inside
 * production close() or offerReceivedFrame() when the case body returns, so every object it
 * can reach - the probe whose queue and locks it uses, the latches it signals, the flags it
 * writes and the frame whose ownership it decides - lives here and is reached through a
 * pointer, never as a reference to a case-body local. A stack local would be gone the moment
 * the case returned, and an abandoned worker would then be writing into a dead frame.
 *
 * One struct serves all three concurrent cases rather than one per case. They need overlapping
 * subsets of the same few fields, and a shared holder keeps the abandonment rule in one place;
 * a case simply leaves the fields it does not use at their initial values.
 */
struct QueueHandoffOverlapState {
    QueueHandoffOverlapState(void)
        : contending(NULL)
        , contendingAccepted(false)
        , contendingRefusedByStateGuard(false)
        , contendingRaisedSomethingElse(false)
        , closeReturnedCleanly(false)
        , closeRaisedIoException(false)
        , closeRaisedSomethingElse(false)
        , producersAtRendezvous(0)
        , producersReleased(false)
        , rendezvousTimedOut(false)
    {}

    /** @brief The instance under test. Its queue and its two locks are the shared state. */
    ReceiveQueueProbe probe;

    /** @brief Set by the receive worker immediately before it enters the production handoff. */
    MonotonicLatch receiveEntered;

    /** @brief Set by the close worker immediately before it enters production close(). */
    MonotonicLatch closeEntered;

    /**
     * @brief Every frame the case allocated, released by the harness on every exit path.
     *
     * The ledger exists because a case that allocates frames and then returns early from a
     * failed assertion would otherwise leak them, and because the release has to happen in
     * exactly one order to be safe: after every worker is disposed, and after the queue has
     * been drained so that the probe's own destructor cannot release the same frame a second
     * time. The harness destructor is the one place that order is expressed.
     */
    std::vector<CECFrame *> allocated;

    /** @brief The frame the receive worker offers. Owned by the case, decided by the race. */
    CECFrame *contending;

    /** @brief What the production handoff reported for @c contending. */
    bool contendingAccepted;

    /** @brief The handoff was refused by the OPENED-state guard rather than by occupancy. */
    bool contendingRefusedByStateGuard;

    /** @brief The handoff raised something that is neither of the two expected outcomes. */
    bool contendingRaisedSomethingElse;

    /** @brief close() returned without raising - which a proxyless instance must not do. */
    bool closeReturnedCleanly;

    /** @brief close() raised IOException, the expected report of the failed transaction. */
    bool closeRaisedIoException;

    /** @brief close() raised something else entirely. */
    bool closeRaisedSomethingElse;

    /**
     * @brief Counts the producers that have reached the stress case's rendezvous.
     *
     * The second producer to arrive is the one that releases the pair, which is what makes the
     * overlap real without the test thread having to be scheduled at the right moment - and
     * without leaning on the production lock, which is the very thing under test and is absent
     * in the mutation this case has to catch.
     */
    std::atomic<unsigned int> producersAtRendezvous;

    /** @brief Flipped by the second producer to arrive; the first spins on it. */
    std::atomic<bool> producersReleased;

    /**
     * @brief Set when a producer gave up waiting for its partner at the rendezvous.
     *
     * Atomic because either worker can be the one to set it, and a plain bool written by two
     * threads is a data race however benign the values look.
     */
    std::atomic<bool> rendezvousTimedOut;

private:
    QueueHandoffOverlapState(const QueueHandoffOverlapState &);
    QueueHandoffOverlapState & operator = (const QueueHandoffOverlapState &);
};

/**
 * @brief Owns the shared state and every worker, and disposes both without an unbounded wait.
 *
 * The one rule this class enforces: nothing a worker can still reach is released while a
 * worker that could reach it is still running. Disposal is bounded (BoundedWorker::dispose),
 * so a stuck worker is detached rather than waited on - and a detached worker is still
 * executing production code against this state, which is why, if any worker was abandoned,
 * the state and the worker objects are deliberately leaked rather than freed. That is a
 * few hundred bytes plus one queue, once, on a path that only runs when a case has already
 * failed and the binary is about to report it; freeing them instead would be a use-after-free
 * in a thread nothing can any longer synchronise with, which is the one outcome worse than a
 * leak. Every clean run - which is every passing run - joins its workers and frees everything.
 */
class QueueHandoffOverlapHarness {
public:
    /**
     * @brief Allocates the shared state on the heap, off the test stack.
     * @post The shared state is live and reachable through operator*() and operator->().
     *       It outlives this harness whenever a worker had to be abandoned, which is the
     *       leak-by-design rule the class exists to enforce.
     */
    QueueHandoffOverlapHarness(void) : state(new QueueHandoffOverlapState()), abandoned(false) {}

    ~QueueHandoffOverlapHarness(void) {
        disposeWorkers();

        if (abandoned) {
            /* leaked by design. See the class documentation: a detached worker can still be
             * inside production code holding this state, and there is no longer any way to
             * find out when it stops. */
            return;
        }

        /*
         * the ownership sweep, in the one order that is safe. Every worker is disposed above,
         * so nothing is producing onto the queue any more; the queue is drained first, so the
         * probe's own destructor finds it empty and cannot release a frame this sweep is about
         * to release; and every allocation the case registered is then released exactly once,
         * whatever the case asserted and however it returned. A case that returned early from a
         * failed assertion therefore reports its failure without leaking on top of it.
         */
        std::vector<CECFrame *> leftInQueue;
        size_t                  sentinelsLeftInQueue = 0;

        state->probe.drainInto(leftInQueue, sentinelsLeftInQueue);

        for (size_t i = 0; i < state->allocated.size(); i++) {
            delete state->allocated[i];
        }

        state->allocated.clear();

        for (size_t i = 0; i < workers.size(); i++) {
            delete workers[i];
        }

        delete state;
    }

    /** @brief The shared state, for the case body to read and to hand to its workers. */
    QueueHandoffOverlapState & operator *  (void) const { return *state; }

    /** @brief The shared state, for the case body to read and to hand to its workers. */
    QueueHandoffOverlapState * operator -> (void) const { return state; }

    /**
     * @brief Starts a worker the harness owns and disposes.
     *
     * @param [in] body - The worker body, which must capture the state pointer and nothing on
     *                    the test stack.
     * @return BoundedWorker& - The worker, for the case's own bounded completion observations.
     */
    template <typename Body>
    BoundedWorker &start(Body body) {
        BoundedWorker *worker = new BoundedWorker();

        workers.push_back(worker);
        worker->start(body);

        return *worker;
    }

    /**
     * @brief Disposes every worker, boundedly. Idempotent, and called again by the destructor.
     *
     * @return bool - true when every worker was joined; false when one had to be abandoned,
     *                which also arms the leak-by-design rule above.
     */
    bool disposeWorkers(void) {
        for (size_t i = 0; i < workers.size(); i++) {
            if (!workers[i]->dispose(WORKER_ABANDON_DEADLINE_MS)) {
                abandoned = true;
            }
        }

        return !abandoned;
    }

    /**
     * @brief Whether any worker had to be abandoned rather than joined, for a case to assert on
     *
     * @return bool - True once a bounded disposition gave up on a worker, which is also
     *                what makes this harness leak its shared state deliberately rather
     *                than free it. False on every clean run.
     */
    bool workerWasAbandoned(void) const { return abandoned; }

private:
    QueueHandoffOverlapState           *state;
    std::vector<BoundedWorker *>  workers;
    bool                          abandoned;

    QueueHandoffOverlapHarness(const QueueHandoffOverlapHarness &);
    QueueHandoffOverlapHarness & operator = (const QueueHandoffOverlapHarness &);
};

/**
 * @brief How long a producer is watched for not completing while the test holds the lock.
 *
 * The bound on the negative observation in the two concurrent cases, and the asymmetry it
 * rests on is what makes those cases deterministic rather than racy:
 *
 * - with the production lock in place the watched producer cannot complete, whatever the
 *   scheduler does, so no length of window can produce a false failure. The window is pure
 *   cost in a passing run, which is why it is a few hundred milliseconds and not seconds;
 * - with the acquisition removed the producer completes in microseconds, so any window
 *   whatsoever detects it. 400 ms is roughly five orders of magnitude of margin over the
 *   work being watched - two size reads, a deque push and a condition signal - so the only
 *   way the removal escapes detection is a machine that deschedules a runnable thread for
 *   two fifths of a second, which would fail this binary's existing timing cases first.
 *
 * Measured both ways before it was committed: with the acquisition deleted from close() both
 * concurrent cases fail on this observation; with it restored both pass.
 */
const long PRODUCER_LOCK_OBSERVATION_MS = 400;

/**
 * @brief How long a released producer is given to finish before the case fails.
 *
 * The positive half of the same observation. Generous by design - it is only reached after
 * the lock has been released, where the remaining work is one CCEC_OSAL::EventQueue::offer()
 * and, for close(), a transaction that fails immediately because a local instance holds no
 * proxy. That offer is a queue-mutex acquisition, a possible std::deque::push_back allocation
 * and a notification issued under that lock, with no wait for capacity anywhere in it; none
 * of that is constant time, so this figure is an empirical margin and not a derived one. Ten
 * seconds sits three to four orders of magnitude above what the work measures at here, which
 * is what makes a timeout mean the producer has stopped making progress rather than that it
 * is slow. Bounded rather than an unqualified join, so a regression that deadlocks the
 * producer is reported as a failure instead of hanging the run.
 */
const long PRODUCER_COMPLETION_TIMEOUT_MS = 10000;

/**
 * @brief How many independent overlaps the stress case drives.
 *
 * The number is chosen from what the case has to catch rather than from a taste for round
 * figures. The illegal state the ownership contract forbids is reachable only while the two
 * producers are genuinely interleaved: the sentinel has to land inside
 * offerReceivedFrame()'s occupancy-check-to-read-back window, which is a deque push, a
 * condition signal and two size reads - a few
 * hundred nanoseconds. A single overlap may miss it, so one iteration proves nothing and many
 * iterations prove it by arithmetic: with the per-iteration hit rate measured on this host at
 * roughly one attempt in eight, 256 attempts leave a miss vanishingly unlikely, and the case
 * still finishes in a fraction of a second because each iteration is two thread creations and
 * about thirty offers, each of them a queue-mutex acquisition, a possible deque append and a
 * notification rather than a free operation.
 *
 * The point of the count is that the detection does not rest on timing. Each iteration asserts
 * an invariant that the fix makes unconditional and the defect makes violable; the iterations
 * exist to give the violation a chance to occur, not to give a wait a chance to expire.
 */
const size_t OVERLAP_STRESS_ITERATIONS = 256;

/** @brief The producers the stress case's rendezvous releases together. */
const unsigned int OVERLAP_RENDEZVOUS_PRODUCERS = 2;

/**
 * @brief How long a producer waits at the rendezvous for its partner before giving up.
 *
 * The bound on the only spin in this file. It is reached exactly when the partner worker never
 * ran at all - a thread that could not be created - so it is a harness failure and is asserted
 * as one; a passing run leaves the rendezvous within nanoseconds of the second arrival. The
 * spin is bounded on std::chrono::steady_clock, so no clock adjustment can extend it.
 */
const long OVERLAP_RENDEZVOUS_BOUND_MS = 5000;

/**
 * @brief The widest deliberate offset between the two producers, in burnOffset() units.
 *
 * Why an offset sweep at all. Released from the same instant, the two producers arrive at the
 * queue with whatever fixed skew this machine's scheduler and cache state happen to impose, and
 * a case that sampled only that one alignment would be sampling one schedule 256 times. Varying
 * the offset between iterations walks the alignment across the window instead, so the sampled
 * interleavings differ by construction rather than by luck. The spread is a few hundred units -
 * a unit being one volatile increment, on the order of a nanosecond - which covers the window
 * the missing lock opens with room on both sides of it.
 */
const unsigned int OVERLAP_OFFSET_SPREAD = 384;

/**
 * @brief Burns @p units of a few nanoseconds each, without sleeping or yielding.
 *
 * The offset primitive. It must not sleep and must not yield: both hand the CPU away for
 * microseconds, which is three orders of magnitude wider than the window being walked across
 * and would turn a fine sweep into a coin toss. A volatile store per unit cannot be elided by
 * the optimiser, which a plain loop or a compiler barrier alone could be.
 *
 * @param [in] units - How many increments to perform. Zero returns immediately.
 */
void burnOffset(unsigned int units) {
    volatile unsigned int sink = 0;

    for (unsigned int unit = 0; unit < units; unit++) {
        sink = sink + 1;
    }
}

/**
 * @brief Arrives at the two-producer rendezvous and returns once both producers are there.
 *
 * The release is done by the second arriver, not by the test thread, and that is the whole
 * reason this exists. A rendezvous the test thread had to release would need the test thread to
 * be scheduled between the two producers' arrivals, on a host where the producers are the
 * runnable threads; and a rendezvous built on the production producer lock - which is what the
 * two timing cases use - releases nothing at all in the mutation this case must catch, because
 * a close() that does not take the lock never parks on it. Handing the release to whichever
 * producer arrives second removes both dependencies: the pair leaves the gate within tens of
 * nanoseconds of each other whatever the scheduler is doing, and nothing about the arrangement
 * depends on production taking a lock.
 *
 * @param [in,out] shared - The state carrying the arrival counter and the release flag.
 * @return bool - true when the pair was released; false when the bound expired, which means the
 *                partner producer never arrived and the iteration is a harness failure.
 */
bool awaitOverlapRendezvous(QueueHandoffOverlapState *shared) {
    const unsigned int arrived =
        shared->producersAtRendezvous.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (arrived >= OVERLAP_RENDEZVOUS_PRODUCERS) {
        shared->producersReleased.store(true, std::memory_order_release);

        return true;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(OVERLAP_RENDEZVOUS_BOUND_MS);

    while (!shared->producersReleased.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }

    return true;
}

/**
 * @brief An IHdmiCec double whose close() reports whatever a case needs it to report
 *
 * THE FAILING CLOSE IS THE POINT. DriverAidlImpl::close() has two failure shapes - a non-ok
 * transaction, and an ok transaction reporting a false result - and both raise IOException
 * AFTER the instance has already released its session references. Nothing in the suite could
 * reach either shape before this double existed: the fake service always closes successfully,
 * and a real HAL cannot be told to fail.
 *
 * Derived from IHdmiCecDefault, and deliberately NOT from BnHdmiCec, for the reason
 * MetadataDouble's own warning gives - a Bn* object could be registered and answer real
 * lookups, which is not wanted from a case-local double.
 *
 * @warning Only close() is overridden. Every other interface method keeps
 *          IHdmiCecDefault's UNKNOWN_TRANSACTION answer, so a case that accidentally
 *          reached one would fail rather than silently succeed. open() is NOT overridden
 *          because it is unreachable from a local instance on a host without a Binder
 *          driver: DriverAidlImpl::open() calls `ProcessState::self()->startThreadPool()`
 *          before it calls the service, and the pinned libbinder terminates the process
 *          when the driver cannot be opened. The reopen half of the B2 residual is
 *          asserted from the instance state and the status contract instead.
 *
 * @see DriverAidlImpl::close()
 */
class ClosingServiceDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief The status the next close() transaction reports. Default: ok. */
    ::android::binder::Status closeStatus = ::android::binder::Status::ok();
    /** @brief The boolean result the next close() writes out. Default: success. */
    bool closeResult = true;
    /** @brief How many times close() has been called on this double. */
    unsigned int closeCalls = 0;
    /** @brief The controller reference the last close() was handed, for identity assertions. */
    ::android::sp<cechal::IHdmiCecController> lastControllerClosed;

    ::android::binder::Status close(const ::android::sp<cechal::IHdmiCecController> &hdmiCecController,
                                   bool *_aidl_return) override {
        closeCalls++;
        lastControllerClosed = hdmiCecController;

        if (_aidl_return != nullptr) {
            *_aidl_return = closeResult;
        }

        return closeStatus;
    }
};

/**
 * @brief An IHdmiCec double that reports whatever logical-address vector a case needs
 *
 * WHAT AN OUT-OF-PROCESS HAL CAN ACTUALLY SEND, which is the point. The generated proxy
 * reads `int32_t` entries straight out of the parcel, so the vector reaching
 * getLogicalAddress() is an arbitrary sequence of 32-bit integers - not a sequence of
 * plausible CEC addresses. Neither the fake service nor a real HAL can be asked to report
 * 256 or -1, so without this double the out-of-contract arms are unreachable.
 *
 * Derived from IHdmiCecDefault and deliberately not from BnHdmiCec, for the reason
 * MetadataDouble's own warning gives.
 *
 * @warning Only getLogicalAddresses() is overridden; every other method keeps
 *          IHdmiCecDefault's UNKNOWN_TRANSACTION answer, so a case that reached one fails
 *          rather than passing quietly.
 *
 * @see DriverAidlImpl::getLogicalAddress()
 */
class AddressReportingDouble : public cechal::IHdmiCecDefault {
public:
    /** @brief The vector the next getLogicalAddresses() writes out, verbatim. */
    std::vector<int32_t> addresses;
    /** @brief The status the next getLogicalAddresses() transaction reports. Default: ok. */
    ::android::binder::Status readStatus = ::android::binder::Status::ok();
    /** @brief How many times getLogicalAddresses() has been called on this double. */
    unsigned int readCalls = 0;

    ::android::binder::Status getLogicalAddresses(::std::vector<int32_t> *_aidl_return) override {
        readCalls++;

        if (_aidl_return != nullptr) {
            *_aidl_return = addresses;
        }

        return readStatus;
    }
};

/**
 * @brief An IHdmiCecController double whose sendMessage() reports any int32 a case needs
 *
 * THE OUT-OF-CONTRACT STATUS IS THE POINT. `sendMessage()`'s result is an `int32_t` the
 * generated proxy reads out of a parcel and hands over as a `SendMessageStatus`, so a HAL can
 * return a value outside the three documented enumerators. Neither the fake service nor a real
 * HAL can be asked to do that, which is why the status is held here as a raw integer and cast
 * on the way out rather than stored as an enumerator.
 *
 * @warning Only sendMessage() is overridden; the address-mutation methods keep
 *          IHdmiCecControllerDefault's UNKNOWN_TRANSACTION answer.
 *
 * @see DriverAidlImpl::write()
 */
class TransmitResultDouble : public cechal::IHdmiCecControllerDefault {
public:
    /** @brief The value the next sendMessage() writes out, cast to the enum unchecked. */
    int32_t reportedStatus = static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0);
    /** @brief The status the next sendMessage() transaction reports. Default: ok. */
    ::android::binder::Status transactionStatus = ::android::binder::Status::ok();
    /** @brief How many times sendMessage() has been called on this double. */
    unsigned int sendCalls = 0;
    /** @brief The bytes the last sendMessage() was handed, for marshalling assertions. */
    std::vector<uint8_t> lastMessage;

    ::android::binder::Status sendMessage(const ::std::vector<uint8_t> &message,
                                         cechal::SendMessageStatus *_aidl_return) override {
        sendCalls++;
        lastMessage = message;

        if (_aidl_return != nullptr) {
            *_aidl_return = static_cast<cechal::SendMessageStatus>(reportedStatus);
        }

        return transactionStatus;
    }
};

/**
 * @brief A DriverAidlImpl that can be placed in an OPENED session without a live HAL
 *
 * WHY INJECTION RATHER THAN open(). Driving the real open() would abort this process, for
 * the ProcessState reason recorded on ClosingServiceDouble, so the session state close()
 * consumes is established directly. That is sufficient for the close-side claims because
 * close() reads exactly three things - the state, the service proxy and the controller
 * reference - and every one of them is set here to the value a successful open() would have
 * left.
 *
 * The listener is the one member a local instance cannot populate: EventListener is a
 * protected nested class that the header only forward declares, so no test translation unit
 * can construct one. close()'s listener detach is therefore covered by the session suite,
 * which runs against the fake service on a Binder-capable host; what these cases establish is
 * that close() releases the CONTROLLER and the STATE on both arms and leaves the service
 * proxy in place, and that it tolerates a null listener exactly as its own guard promises.
 *
 * @warning The destructor forces the state to CLOSED before the base destructor runs, so no
 *          case's teardown re-enters close() and perturbs the call counters it just asserted.
 *
 * @see DriverAidlImpl::close()
 * @see ClosingServiceDouble
 */
class SessionStateProbe : public DriverAidlImpl {
public:
    ~SessionStateProbe() override { status = CLOSED; }

    /**
     * @brief Establishes the session state a successful open() would have left.
     *
     * @param [in] service    - The service proxy close() must call and must RETAIN.
     * @param [in] controller - The controller reference close() must hand over and CLEAR.
     */
    void injectOpenSession(const ::android::sp<cechal::IHdmiCec> &service,
                           const ::android::sp<cechal::IHdmiCecController> &controller) {
        hdmiCecService = service;
        hdmiCecController = controller;
        status = OPENED;
    }

    /**
     * @brief Attaches only the service proxy, leaving the instance CLOSED.
     *
     * getLogicalAddress() carries NO state guard - matching DriverImpl::getLogicalAddress() -
     * so it is reachable on a closed instance and needs nothing but the proxy. Injecting the
     * session state as well would be modelling a precondition the method does not have.
     *
     * @param [in] service - The service proxy getLogicalAddress() must call.
     */
    void injectServiceOnly(const ::android::sp<cechal::IHdmiCec> &service) { hdmiCecService = service; }

    /** @brief Seeds the local logical-address list, so "deliberately not cleared" is testable. */
    void seedLogicalAddress(const LogicalAddress &address) { logicalAddresses.push_back(address); }

    /** @brief Current lifecycle state, as close() left it. */
    int currentStatus(void) const { return status; }
    /**
     * @brief The CLOSED value of DriverAidlImpl's unnamed lifecycle enum.
     *
     * The enum has no type name, so a case cannot spell `DriverAidlImpl::CLOSED` in an
     * assertion and a literal 0 would encode the value rather than the meaning. Reading it
     * back through the class keeps the assertion tied to the definition.
     */
    int closedState(void) const { return CLOSED; }
    /** @brief The OPENED value of the same unnamed enum, for the same reason. */
    int openedState(void) const { return OPENED; }
    /** @brief Whether a controller session reference is still held. */
    bool holdsController(void) const { return hdmiCecController != 0; }
    /** @brief Whether the top-level service proxy is still held, i.e. a reopen is attemptable. */
    bool holdsService(void) const { return hdmiCecService != 0; }
    /** @brief Whether a listener reference is still held. */
    bool holdsListener(void) const { return eventListener != 0; }
    /** @brief Size of the local logical-address list. */
    size_t logicalAddressCount(void) const { return logicalAddresses.size(); }
};

} // namespace

/**
 * @brief Fixture for the properties that hold on a locally constructed back-end, under
 *        every invocation and whichever back-end the process resolved to.
 *
 * Every case here builds its own DriverAidlImpl or DriverImpl on the stack rather than
 * reaching through Driver::getInstance(), which is what makes the set
 * invocation-independent: no service, no proxy and no resolved selection is involved, so
 * the same assertions hold whether the process selected the AIDL or the legacy back-end.
 * That is also why a local instance is always CLOSED - only a successful
 * isServiceAvailable() caches a proxy, and a locally built object never has one.
 *
 * @see DriverAidlSessionFixture for the properties that need an opened AIDL session, and
 *      so have to be partitioned by invocation.
 */
class DriverAidlLocalInstanceTest : public ::testing::Test {
protected:
    /**
     * @brief Clears any legacy HAL expectations left behind by a preceding case.
     *
     * @note Establishes no driver precondition, because none is needed - see the body.
     */
    void SetUp() override {
        mock = HdmiCecDriverMock::getInstance();
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        // No driver precondition is established and none is needed: every case here drives a
        // local instance. The process-global driver is deliberately left exactly as it is,
        // which is also why TearDown does not restore it - nothing here disturbs it.
    }

    /**
     * @brief Verifies and clears legacy HAL expectations, so an unmet one is attributed to
     *        the case that set it.
     *
     * @note Restores no process-global state, because no case in this fixture disturbs any.
     */
    void TearDown() override {
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }
    }

    /**
     * @brief The legacy HAL mock, retained so expectations can be cleared around each case.
     *
     * @note Held rather than used to set expectations: most cases here assert that no
     *       legacy entry point is reached, which is a strict-mock property rather than an
     *       EXPECT_CALL.
     */
    HdmiCecDriverMock *mock = nullptr;
};

// Ordering proof, on the AIDL back-end. writeAsync takes the frame buffer and logs the frame
// before it locks and checks the state (the prelude at the head of
// DriverAidlImpl::writeAsync(), mirroring DriverImpl.cpp:203-204 ahead of the guard at
// :207-209), so a closed driver handed an empty
// frame reports the decode failure - not the invalid state, and not the
// operation-not-supported that is this back-end's authorized difference.
//
// Why std::out_of_range escapes at all: printFrameDetails constructs a Header, which reads
// frame.at(0) and raises std::out_of_range on an empty frame, and its handler catches only
// Exception. Exception and std::out_of_range are siblings under std::exception - not
// related by inheritance - so catch(Exception &e) cannot catch it and it propagates. The
// neighbouring async suite records the same boundary for the legacy back-end
// (test_DriverImpl_Async.cpp:38-46).
//
// The positive control is what makes this an ordering proof rather than one more guard case:
// with a well-formed frame the same closed instance gets past the prelude and is then
// stopped by the state guard instead. If the two were ever reordered, the first expectation
// would start reporting InvalidStateException and fail loudly.
/**
 * @brief writeAsync's frame prelude runs before its state guard on the AIDL back-end, so a
 *        closed driver handed an empty frame reports the decode failure rather than the
 *        invalid state.
 * @pre Runs under every invocation, on a locally constructed CLOSED DriverAidlImpl. No
 *      service, no proxy and no process-global driver are involved.
 * @note Evidence: the empty frame raises std::out_of_range, and a well-formed frame on the
 *       same closed instance is stopped by the state guard instead. The second is the
 *       positive control that makes this an ordering proof - were the two reordered, the
 *       first expectation would start reporting InvalidStateException and fail loudly. No
 *       legacy HAL entry point may be reached either way.
 */
TEST_F(DriverAidlLocalInstanceTest, FrameLoggingPrecedesStateGuardInWriteAsync) {
    ASSERT_NE(mock, nullptr);

    // The frame never survives the prelude, so nothing may reach the legacy HAL either -
    // and on this back-end nothing ever should, at any point.
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _)).Times(0);

    DriverAidlImpl closedDriver;
    CECFrame emptyFrame;

    EXPECT_THROW({ closedDriver.writeAsync(emptyFrame); }, std::out_of_range)
        << "an empty frame on a closed AIDL driver did not raise std::out_of_range, so the frame "
           "prelude no longer runs ahead of the state guard";

    CECFrame wellFormed = directedFrame();
    EXPECT_THROW({ closedDriver.writeAsync(wellFormed); }, InvalidStateException)
        << "a well-formed frame on a closed AIDL driver did not raise InvalidStateException, so "
           "the state guard is no longer reached";
}

// The same ordering, on the legacy back-end, so that the two are compared rather than
// asserted separately and assumed to agree.
//
// This is the half of the writeAsync difference that must be identical on both back-ends.
// The authorized difference is only what happens once the prelude and the guard have both
// passed - success on legacy, OperationNotSupportedException on AIDL - and that arm needs an
// open driver, so it is asserted in the invocation-specific fixtures below. What is asserted
// here is that the two failure modes reached before the guard passes are the same on both,
// which is what makes the difference a single declared one rather than a diffuse divergence.
/**
 * @brief The same prelude-before-guard ordering holds on the legacy back-end, so the two
 *        are compared rather than asserted separately and assumed to agree.
 * @pre Runs under every invocation, on a locally constructed CLOSED DriverImpl.
 * @note This is the half of the writeAsync difference that must be identical on both. The
 *       authorized difference is only what happens once prelude and guard have both passed,
 *       which needs an open driver and belongs to the invocation-specific fixtures; asserting
 *       the shared failure modes here is what makes that difference a single declared one
 *       rather than a diffuse divergence.
 */
TEST_F(DriverAidlLocalInstanceTest, WriteAsyncPreludeOrderIsIdenticalOnTheLegacyBackEnd) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _)).Times(0);

    DriverImpl closedLegacyDriver;
    CECFrame emptyFrame;

    EXPECT_THROW({ closedLegacyDriver.writeAsync(emptyFrame); }, std::out_of_range)
        << "the legacy back-end no longer raises std::out_of_range for an empty frame on a closed "
           "driver, so the two back-ends' prelude ordering has diverged";

    CECFrame wellFormed = directedFrame();
    EXPECT_THROW({ closedLegacyDriver.writeAsync(wellFormed); }, InvalidStateException)
        << "the legacy back-end no longer raises InvalidStateException for a well-formed frame on "
           "a closed driver";
}

// Every state-guarded operation refuses a closed AIDL driver, and refuses it before touching
// any HAL. The guards are swept together rather than one case each because the property being
// asserted is uniform - status != OPENED means InvalidStateException, for all of them - and a
// gap in that set is much easier to see in one list than spread across five cases.
//
// poll() is included and is not redundant: it carries no guard of its own, reaching the guard
// through this back-end's own DriverAidlImpl::write(), so it is the one case
// here that proves the internal call goes to the right place.
/**
 * @brief Every state-guarded operation refuses a closed AIDL driver, and refuses before
 *        touching any HAL.
 * @pre Runs under every invocation, on a locally constructed CLOSED DriverAidlImpl.
 * @note Swept together rather than one case each because the property is uniform, and a gap
 *       in the set is far easier to see in one list. poll() is included and is not
 *       redundant: it carries no guard of its own and reaches the guard through this
 *       back-end's own write(), so it is the one entry here that proves the internal call
 *       goes to the right place.
 * @warning read() must refuse on entry. With an empty queue EventQueue::poll() blocks by
 *          contract, so a read that got past the guard would hang the whole binary rather
 *          than fail one case.
 */
TEST_F(DriverAidlLocalInstanceTest, EveryStateGuardedOperationRefusesAClosedDriver) {
    ASSERT_NE(mock, nullptr);

    // Not one legacy HAL entry point may be reached from the AIDL back-end, in any state.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _)).Times(0);

    DriverAidlImpl closedDriver;
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    CECFrame frame = directedFrame();

    EXPECT_THROW({ closedDriver.write(frame); }, InvalidStateException)
        << "write() accepted a frame on a closed driver";
    EXPECT_THROW({ closedDriver.writeAsync(frame); }, InvalidStateException)
        << "writeAsync() accepted a frame on a closed driver";
    EXPECT_THROW({ closedDriver.addLogicalAddress(address); }, InvalidStateException)
        << "addLogicalAddress() claimed an address on a closed driver, so the driver's bookkeeping "
           "and the HAL's could diverge";
    EXPECT_THROW({ closedDriver.removeLogicalAddress(address); }, InvalidStateException)
        << "removeLogicalAddress() released an address on a closed driver";
    EXPECT_THROW({ closedDriver.poll(address, LogicalAddress(LogicalAddress::TV)); },
                 InvalidStateException)
        << "poll() transmitted on a closed driver, so it is no longer reaching the guard through "
           "this back-end's own write()";

    CECFrame received;
    EXPECT_THROW({ closedDriver.read(received); }, InvalidStateException)
        << "read() did not refuse a closed driver on entry. It must refuse before reaching the "
           "queue: with an empty queue EventQueue::poll blocks by contract, so a read that got "
           "past the guard here would hang the whole test binary rather than fail";
}

// The guard is a state check, not an address check, so no address may slip past it - swept
// from the TV at one end to the broadcast/unregistered value at the other. A guard that
// happened to be written as a validity check would pass the case above and fail here.
/**
 * @brief The address guards are state checks rather than address checks, so no logical
 *        address slips past them while closed.
 * @pre Runs under every invocation, on a locally constructed CLOSED DriverAidlImpl.
 * @note Evidence: the whole space is swept, from the TV at one end to the
 *       broadcast/unregistered value at the other, and neither add nor remove reaches the
 *       legacy HAL. A guard that happened to be written as a validity check would pass the
 *       preceding case and fail here.
 */
TEST_F(DriverAidlLocalInstanceTest, AddressGuardsRejectEveryLogicalAddressWhileClosed) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecRemoveLogicalAddress(_, _)).Times(0);

    DriverAidlImpl closedDriver;

    const int addresses[] = {
        LogicalAddress::TV,                // minimum
        LogicalAddress::PLAYBACK_DEVICE_1, // interior
        LogicalAddress::BROADCAST          // maximum / unregistered
    };

    for (size_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        const LogicalAddress address(addresses[i]);
        EXPECT_THROW({ closedDriver.addLogicalAddress(address); }, InvalidStateException)
            << "addLogicalAddress accepted address " << addresses[i] << " while closed";
        EXPECT_THROW({ closedDriver.removeLogicalAddress(address); }, InvalidStateException)
            << "removeLogicalAddress accepted address " << addresses[i] << " while closed";
    }
}

// close() on an instance that was never opened returns silently, and that is the observable
// legacy behaviour rather than a convenience: DriverImpl::close()'s throw is compiled out
// under #if 0 (DriverImpl.cpp:130-140), and the AIDL back-end carries the same #if 0
// verbatim in DriverAidlImpl::close() so the two files read alike. A close that threw here
// would break LibCCEC::term on any path that had not opened.
//
// The double close is asserted too, because idempotence is what callers actually rely on:
// Bus reaches Driver::close() from two places and LibCCEC from a third.
/**
 * @brief close() on an instance that was never opened returns silently, and does so twice.
 * @pre Runs under every invocation, on a locally constructed CLOSED DriverAidlImpl.
 * @note Silence is the observable legacy behaviour rather than a convenience: DriverImpl's
 *       throw is compiled out under `#if 0`, and the AIDL back-end carries the same block
 *       verbatim so the two files read alike. A close that threw would break LibCCEC::term
 *       on any path that had not opened. The double close is asserted because idempotence is
 *       what callers rely on - Bus reaches Driver::close() from two places and LibCCEC from
 *       a third, and none tracks whether another already did.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseOnANeverOpenedDriverReturnsSilently) {
    ASSERT_NE(mock, nullptr);

    // No legacy close may be attempted from the AIDL back-end, and no AIDL close can be
    // either, since no session was ever held.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    DriverAidlImpl closedDriver;

    EXPECT_NO_THROW({ closedDriver.close(); })
        << "close() raised on a driver that was never opened. The legacy back-end returns "
           "silently - its throw is compiled out - so raising here is an unregistered difference";

    EXPECT_NO_THROW({ closedDriver.close(); })
        << "a second close() raised, so close is not idempotent; Bus and LibCCEC both reach "
           "Driver::close() and neither tracks whether another already did";
}

// F4(d). THE FAILING CLOSE, WHICH IS THE ONE ARM B2 MAKES REACHABLE. close() maps the
// candidate IHdmiCec::close() onto the legacy HdmiCecClose(), and a failure reaches the
// caller as IOException - but the instance must already be fully closed by then, because
// LibCCEC::term() and ~DriverAidlImpl() both swallow that exception and a half-closed object
// would then be indistinguishable from an open one.
//
// BOTH FAILURE SHAPES ARE DRIVEN, because they take different routes through the same
// condition: `!txn.isOk()` for a transport-level failure, and `!closed` for a transaction that
// arrived intact and reported refusal. A fix that handled only one of them would pass a
// single-shape case.
//
// WHAT IS ASSERTED, one claim per line of close()'s post-condition:
//   - the state is CLOSED, and it was set BEFORE the raise (visible after the catch);
//   - the controller reference is GONE, so nothing holds a session the HAL may have dropped;
//   - the service proxy is RETAINED, which is what makes a reopen attemptable at all;
//   - no listener reference survives (null here to begin with - see SessionStateProbe);
//   - the local logical-address list is UNTOUCHED, matching DriverImpl::close();
//   - the HAL was called exactly ONCE, and destruction does not call it again.
TEST_F(DriverAidlLocalInstanceTest, AFailedCloseLeavesTheInstanceClosedWithNoSessionReferences) {
    ASSERT_NE(mock, nullptr);

    // The AIDL back-end reaches no legacy entry point, on this path least of all.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    struct FailureShape {
        const char *name;
        ::android::binder::Status status;
        bool result;
    };

    const FailureShape shapes[] = {
        // Transaction arrived and the HAL refused: ok status, false result.
        { "ok transaction reporting a false result", ::android::binder::Status::ok(), false },
        // Transport failure: the far side is gone, nothing was decided.
        { "non-ok transaction (DEAD_OBJECT)",
          ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT), true },
        // Refusal expressed as an exception code, which is what a live HAL sends.
        { "non-ok transaction (EX_ILLEGAL_STATE)",
          ::android::binder::Status::fromExceptionCode(::android::binder::Status::EX_ILLEGAL_STATE,
                                                       "no session is open"),
          true }
    };

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        const ::android::sp<ClosingServiceDouble> service = ::android::sp<ClosingServiceDouble>::make();
        const ::android::sp<cechal::IHdmiCecController> controller =
            ::android::sp<cechal::IHdmiCecControllerDefault>::make();

        service->closeStatus = shapes[i].status;
        service->closeResult = shapes[i].result;

        {
            SessionStateProbe probe;

            probe.injectOpenSession(service, controller);
            probe.seedLogicalAddress(LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1));

            // Positive control on the injection itself: close() returns silently on any state
            // other than OPENED, so a botched injection would produce a green case that never
            // reached the HAL at all.
            ASSERT_EQ(probe.currentStatus(), probe.openedState())
                << "the injected session state is not OPENED, so close() would return silently "
                   "and this case would assert nothing";

            EXPECT_THROW({ probe.close(); }, IOException)
                << "close() returned normally for the " << shapes[i].name
                << ". Both failure shapes must reach the caller as IOException, exactly as the "
                   "legacy back-end raises on a failed HdmiCecClose()";

            EXPECT_EQ(probe.currentStatus(), probe.closedState())
                << "the state is not CLOSED after the " << shapes[i].name
                << ". close() sets it before it raises precisely so that a caller which swallows "
                   "the exception - LibCCEC::term() and the destructor both do - still sees a "
                   "consistently closed object";

            EXPECT_FALSE(probe.holdsController())
                << "a controller session reference survived the " << shapes[i].name
                << ". The HAL may have dropped that session, so holding it is a reference to "
                   "something that no longer exists";

            EXPECT_TRUE(probe.holdsService())
                << "the service proxy was released by the " << shapes[i].name
                << ". It is the resolved-once selection result and holds no session state; "
                   "dropping it would make a reopen impossible without re-running the whole "
                   "preflight and lookup";

            EXPECT_FALSE(probe.holdsListener())
                << "a listener reference appeared from nowhere during the " << shapes[i].name;

            EXPECT_EQ(probe.logicalAddressCount(), 1u)
                << "the local logical-address list was cleared by the " << shapes[i].name
                << ". DriverImpl::close() does not clear it, so clearing here would be a fourth "
                   "observable difference between the back-ends";

            EXPECT_EQ(service->closeCalls, 1u)
                << "IHdmiCec::close() was called " << service->closeCalls << " times for the "
                << shapes[i].name << ", not once. close() must neither retry nor skip the call";

            EXPECT_EQ(service->lastControllerClosed.get(), controller.get())
                << "close() handed the HAL a controller reference that is not the session's";
        }

        // Destruction after a failed close must be inert: the state is already CLOSED, so the
        // destructor's close() returns silently rather than calling the HAL a second time.
        EXPECT_EQ(service->closeCalls, 1u)
            << "destruction after the " << shapes[i].name
            << " called IHdmiCec::close() again. The destructor closes only an OPENED instance, "
               "and a failed close leaves it CLOSED";
    }
}

// F4(d), B2 residual. A failed close leaves this side clean, but says NOTHING about the HAL's
// side: IHdmiCec::close() is the high-confidence candidate for HdmiCecClose() pending owner
// confirmation, and a close that reported failure may well have left the session open on the
// far side. IHdmiCec::open() is single-instance and refuses a second session with
// EX_ILLEGAL_STATE, so the next open() on this instance has exactly two outcomes and both are
// safe: it succeeds, or it raises IOException. This case pins both halves of that claim.
//
// WHY THE REOPEN IS NOT DRIVEN END TO END HERE, stated rather than left as a gap: open() calls
// `ProcessState::self()->startThreadPool()` before it reaches the service, and the pinned
// libbinder terminates a process whose Binder driver cannot be opened - measured on this host,
// SIGABRT with "Binder driver '/dev/binder' could not be opened. Terminating". A local
// instance therefore cannot execute open() past that line at all on a driverless runner, which
// is why the session suite - which needs a Binder-capable host anyway - owns the live reopen
// and this case owns the two properties that are checkable without one:
//   1. the instance is left in a state from which a reopen can be ATTEMPTED, i.e. CLOSED with
//      the service proxy still held; and
//   2. EX_ILLEGAL_STATE is a NON-OK status, which is the only thing open() tests before it
//      raises IOException, so the refusal cannot be mistaken for a successful reopen.
TEST_F(DriverAidlLocalInstanceTest, AFailedCloseLeavesTheInstanceReopenableOrCleanlyFailing) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecOpen(_)).Times(0);
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    const ::android::sp<ClosingServiceDouble> service = ::android::sp<ClosingServiceDouble>::make();
    const ::android::sp<cechal::IHdmiCecController> controller =
        ::android::sp<cechal::IHdmiCecControllerDefault>::make();

    service->closeResult = false;

    SessionStateProbe probe;

    probe.injectOpenSession(service, controller);

    ASSERT_THROW({ probe.close(); }, IOException);

    // Property 1: a reopen is attemptable. CLOSED is what open()'s own guard requires, and the
    // retained service proxy is what it calls; without either, open() could not even be tried.
    ASSERT_EQ(probe.currentStatus(), probe.closedState())
        << "a failed close left a state other than CLOSED, so open() would return silently "
           "instead of attempting a reopen - the object would be permanently unusable and "
           "would report nothing about why";
    ASSERT_TRUE(probe.holdsService())
        << "a failed close released the service proxy, so open() would raise IOException for "
           "the wrong reason - no proxy rather than a HAL-side session";

    // A second close is silent and reaches the HAL no further, so a caller retrying term()
    // cannot churn a session it no longer holds.
    EXPECT_NO_THROW({ probe.close(); })
        << "a second close() after a failed one raised. close() returns silently on any state "
           "other than OPENED, and Bus and LibCCEC both reach it without tracking each other";
    EXPECT_EQ(service->closeCalls, 1u)
        << "a second close() called the HAL again after the first one failed";

    // Property 2: the refusal open() would receive is unambiguously a failure. This is the
    // status contract open() relies on - its only test is `!txn.isOk()` - so if EX_ILLEGAL_STATE
    // ever reported ok, a HAL-side refusal would be read as a successful reopen and the
    // instance would go to OPENED holding a null controller.
    const ::android::binder::Status refusal = ::android::binder::Status::fromExceptionCode(
        ::android::binder::Status::EX_ILLEGAL_STATE, "a session is already open");

    EXPECT_FALSE(refusal.isOk())
        << "EX_ILLEGAL_STATE now reports ok, so open()'s `!txn.isOk()` arm would not fire and a "
           "HAL-side refusal would be mistaken for a reopen";
    EXPECT_EQ(refusal.exceptionCode(), ::android::binder::Status::EX_ILLEGAL_STATE)
        << "the exception code does not survive Status::fromExceptionCode, so the diagnostic "
           "open() logs could not name the B2 condition";
}

// A fresh instance holds no logical address, and isValidLogicalAddress answers without
// touching any HAL on either back-end - it is a walk of the local list under the instance
// lock (DriverAidlImpl::isValidLogicalAddress(), byte-for-byte the legacy implementation).
// Connection is
// its only production caller (Connection.cpp:354), one address at a time.
//
// The whole address space is swept rather than one value sampled, because "holds nothing" is
// the claim; a single false answer would be consistent with an implementation that special
// cased one address.
/**
 * @brief A fresh instance holds no logical address, and isValidLogicalAddress answers
 *        without touching any HAL.
 * @pre Runs under every invocation, on a locally constructed DriverAidlImpl.
 * @note Evidence: the whole address space is swept rather than one value sampled, because
 *       "holds nothing" is the claim and a single false answer would be consistent with an
 *       implementation that special-cased one address. The query is a walk of the local list
 *       under the instance lock, byte-for-byte the legacy implementation, and Connection is
 *       its only production caller, one address at a time.
 */
TEST_F(DriverAidlLocalInstanceTest, FreshInstanceHoldsNoLogicalAddressAndConsultsNoHal) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    const DriverAidlImpl freshDriver;

    for (int address = LogicalAddress::TV; address <= LogicalAddress::BROADCAST; address++) {
        EXPECT_FALSE(freshDriver.isValidLogicalAddress(LogicalAddress(address)))
            << "a freshly constructed AIDL back-end reported that it holds logical address "
            << address << ", although nothing has been added to it";
    }
}

// open() without a cached service proxy raises IOException, and raises it before anything in
// the process touches libbinder.
//
// The ordering is the substance here, not the exception. The proxy is cached only by a
// successful DriverAidlImpl::isServiceAvailable(), so a local instance never has one; the
// no-proxy guard at the head of DriverAidlImpl::open() checks for it ahead of that method's
// ProcessState::self()->startThreadPool() call. That order is what makes this case
// runnable on a host with no kernel binder support at all: reaching ProcessState first would
// raise SIGABRT and take the whole binary down rather than raise an exception this case can
// catch. It is also what keeps every local instance in this fixture CLOSED, and therefore
// keeps its destructor off the close path.
//
// The legacy HAL must not be reached either. An AIDL back-end that fell back to HdmiCecOpen
// when its own service was missing would be a second, undeclared selection point.
/**
 * @brief open() without a cached service proxy raises IOException, before anything in the
 *        process touches libbinder, and without falling back to the legacy HAL.
 * @pre Runs under every invocation, on a locally constructed DriverAidlImpl - which never
 *      has a proxy, since only a successful isServiceAvailable() caches one.
 * @note The ordering is the substance rather than the exception. The no-proxy check sits
 *       ahead of the ProcessState threadpool call, which is what makes this case runnable on
 *       a host with no kernel binder support at all: reaching ProcessState first would raise
 *       SIGABRT and take the binary down rather than raise something catchable. It is also
 *       what keeps every local instance in this fixture CLOSED, and so keeps its destructor
 *       off the close path.
 * @note A back-end that fell back to HdmiCecOpen when its own service was missing would be a
 *       second, undeclared selection point, so no legacy entry point may be reached. The
 *       state is asserted not to have moved either, so a failed open cannot half-succeed.
 */
TEST_F(DriverAidlLocalInstanceTest, OpenWithoutAServiceProxyRaisesIoExceptionAndTouchesNoLegacyHal) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecOpen(_)).Times(0);
    EXPECT_CALL(*mock, HdmiCecSetRxCallback(_, _, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecSetTxCallback(_, _, _)).Times(0);

    DriverAidlImpl unresolvedDriver;

    EXPECT_THROW({ unresolvedDriver.open(); }, IOException)
        << "open() did not raise IOException on an instance holding no service proxy. If it "
           "instead reached ProcessState::self(), this process would have aborted rather than "
           "arrived here at all on a host without a binder driver";

    // Still closed, so the guards remain live - which is the proof that open() did not
    // half-succeed and leave the state OPENED with no session behind it.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ unresolvedDriver.write(frame); }, InvalidStateException)
        << "the instance behaves as though it were open after a failed open(), so open() moved the "
           "state before it had a session";
}

// getLogicalAddress carries no state guard on either back-end, and answers 0 when it has no
// service to ask (DriverAidlImpl::getLogicalAddress()).
//
// Zero is the contract, not a shortfall, and it covers four distinct conditions - no proxy, a
// non-ok binder status, a successful call reporting no addresses, and the genuine address 0 -
// which are distinguished in the log rather than in the return value. That is deliberate:
// the legacy implementation zero-initialises its local and returns whatever the HAL leaves
// there (DriverImpl.cpp:290-301), and LibCCEC::getLogicalAddress turns a zero into
// InvalidStateException (LibCCEC.cpp:164-167), which is the existing signal callers already
// handle for "no address". Any other sentinel here would suppress that throw and hand a
// caller an address it does not hold.
//
// devType is ignored on both back-ends, so it is swept: a value being honoured here would be
// a divergence from the legacy back-end and from the AIDL get-all contract alike.
/**
 * @brief getLogicalAddress carries no state guard and answers 0 when it has no service to
 *        ask, whatever devType it is given.
 * @pre Runs under every invocation, on a locally constructed DriverAidlImpl holding no proxy.
 * @note Zero is the contract rather than a shortfall, and it covers four conditions - no
 *       proxy, a non-ok binder status, a successful call reporting no addresses, and the
 *       genuine address 0 - distinguished in the log rather than in the return value. The
 *       legacy implementation zero-initialises its local and returns whatever the HAL leaves
 *       there, and LibCCEC::getLogicalAddress turns a zero into InvalidStateException, which
 *       is the signal callers already handle; any other sentinel here would suppress that
 *       throw and hand a caller an address it does not hold.
 * @note devType is swept because it is ignored on both back-ends: a value being honoured
 *       here would diverge from the legacy back-end and from the AIDL get-all contract alike.
 */
TEST_F(DriverAidlLocalInstanceTest, GetLogicalAddressReportsZeroWithoutAServiceAndIgnoresDevType) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    DriverAidlImpl unresolvedDriver;

    const int devTypes[] = { DeviceType::TV, DeviceType::TUNER, DeviceType::PLAYBACK_DEVICE };

    for (size_t i = 0; i < sizeof(devTypes) / sizeof(devTypes[0]); i++) {
        EXPECT_EQ(unresolvedDriver.getLogicalAddress(devTypes[i]), 0)
            << "getLogicalAddress reported a non-zero address for device type " << devTypes[i]
            << " although no AIDL service is held. Zero is what LibCCEC::getLogicalAddress turns "
               "into InvalidStateException, so any other value would report an address this "
               "device does not have";
    }
}

// SC6(f), the AIDL half: BLOCKED ITEM B1. getPhysicalAddress does not read an address on this
// back-end - it logs the block and leaves the caller's out-parameter untouched
// (DriverAidlImpl::getPhysicalAddress()).
//
// The sentinel is what makes "untouched" an assertion rather than a hope. A case that merely
// asserted the call did not throw would pass equally against an implementation that wrote a
// fabricated value, a zero, or uninitialised memory - and writing a plausible-looking
// physical address would be far worse than writing nothing, because a caller cannot tell a
// fabricated address from a real one. Seeding a distinctive value and asserting it survives
// is the only form that rules all of those out.
//
// This is the documented interim behaviour and it is not equivalent to the legacy back-end -
// see the B1 paragraph in the contract block above, which states the change that would close
// the gap. It is also not a fabricated outcome: it is the same observable result the legacy
// path already produces when its own call fails, since that implementation ignores the return
// value and writes nothing. Both plugin call sites already wrap the call and tolerate not
// obtaining an address.
//
// And the block must be *reported*, which is the other half and is asserted here. Writing
// nothing is only half the contract: the compliance criterion is that this method names the
// block and the awaited device settings contract, because writing nothing silently would leave
// an integrator staring at an unchanged variable with no way to learn why the physical address
// never arrived - the difference between a blocked read and a working read that returned the
// value already in the variable is invisible from the outside. A case that asserted only the
// no-throw and the sentinel would pass unchanged against a body reduced to `return;`, which is
// precisely the implementation this case must be able to fail against. So the report is
// captured and matched.
//
// No log-level guard is needed, unlike the diagnostic-callback case in this file. The block is
// emitted at LOG_EXP, which is level 3, and CCEC_LOG prints whenever the level is at or below
// cec_log_level, whose default is LOG_INFO = 5 (Util.cpp:39, :116). So the line is visible at
// the default level and ScopedCecLogLevel - which takes custody of a shared host path - would
// be ceremony here.
/**
 * @brief The AIDL getPhysicalAddress leaves the caller's out-parameter untouched and reports
 *        the block, naming the awaited device settings contract.
 * @pre Runs under every invocation, on a locally constructed DriverAidlImpl. The out
 *      parameter is seeded with a distinctive value first, which is the only form that
 *      distinguishes an untouched variable from a fabricated address or a zeroed one.
 * @note Both halves are asserted, and the second is why this case can fail against a body
 *       reduced to `return;`. Writing nothing is half the contract; the other half is that
 *       the method names the block, because a silent no-op leaves an integrator staring at an
 *       unchanged variable with no way to learn why the address never arrived. So the report
 *       is captured and matched.
 * @note This is the documented interim behaviour and is not equivalent to the legacy
 *       back-end - the contract block above states the change that would close the gap. It is
 *       not a fabricated outcome either: it is what the legacy path already produces when its
 *       own call fails, since that implementation ignores the return value and writes nothing.
 * @note No log-level guard is needed. The block is emitted at LOG_EXP, which is below the
 *       default level, so the line is visible without taking custody of a shared host path.
 */
TEST_F(DriverAidlLocalInstanceTest, GetPhysicalAddressIsBlockedOnB1AndLeavesTheOutParameterUntouched) {
    ASSERT_NE(mock, nullptr);

    // The legacy CEC HAL call must not be substituted on this back-end. That substitution is
    // forbidden outright, and it is independently unsafe: HdmiCecOpen performs logical-address
    // discovery for source devices, which is CEC polling on the wire, so it would put a second
    // controller on the bus while the AIDL HAL is the active one.
    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _)).Times(0);
    EXPECT_CALL(*mock, HdmiCecOpen(_)).Times(0);

    DriverAidlImpl aidlDriver;

    const unsigned int sentinel = 0xDEADBEEFu;
    unsigned int physicalAddress = sentinel;

    /*
     * The two fragments of the report that carry its meaning, transcribed from the producer -
     * DriverAidlImpl::getPhysicalAddress(), which emits the block as an inline literal rather
     * than through a named constant, so there is nothing to import and transcription is the
     * only option available.
     *
     * Two distinctive fragments rather than one token or one whole line. Each is one thing the
     * report has to say and neither can be satisfied by accident: the marker is the searchable
     * identifier an operator or a release note greps for, and the contract phrase is what tells
     * a reader which input is missing and therefore who unblocks it. A shorter match - "B1"
     * alone, say - would be satisfied by unrelated text in the same capture, and matching the
     * whole line instead would fail on a reflow that changed nothing an operator relies on.
     */
    const char *const kBlockedItemMarker = "BLOCKED ITEM B1";
    const char *const kAwaitedContractPhrase =
        "the device settings HAL contract for the EDID byte read was not supplied";

    std::string captured;
    bool raised = false;
    std::string raisedWhat;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so whether the block was reported cannot be "
               "established, and this case would degenerate into 'the call wrote nothing' - "
               "which a body reduced to a bare return would also satisfy";

        /*
         * The throw verdict is taken by hand here rather than with EXPECT_NO_THROW, and only
         * because stdout is redirected for the duration of the call: a failing macro inside
         * this scope would write its own diagnosis into the capture file instead of the run's
         * log, so the reason the case failed would be the one thing missing from the output.
         * Recording the outcome and asserting on it after the capture closes keeps every
         * failure message on the real stdout, and asserts exactly what EXPECT_NO_THROW did.
         */
        try {
            aidlDriver.getPhysicalAddress(&physicalAddress);
        }
        catch (const std::exception &e) {
            raised = true;
            raisedWhat = e.what();
        }
        catch (...) {
            raised = true;
            raisedWhat = "an exception that does not derive from std::exception";
        }

        captured = capture.read();
    }

    EXPECT_FALSE(raised)
        << "getPhysicalAddress raised on the AIDL back-end. The blocked read must degrade to a "
           "log and a return, because both plugin call sites tolerate not obtaining an address "
           "but neither expects a new exception. What was raised: [" << raisedWhat << "]";

    EXPECT_EQ(physicalAddress, sentinel)
        << "the caller's out-parameter was modified. Pending B1 this method must write NOTHING: a "
           "fabricated or zeroed physical address is indistinguishable from a real one to every "
           "caller, which is worse than the caller keeping the value it initialized";

    EXPECT_NE(captured.find(kBlockedItemMarker), std::string::npos)
        << "the blocked read reported nothing that names the block. Without the \""
        << kBlockedItemMarker
        << "\" marker in the log, an integrator sees an unchanged variable and no reason for it, "
           "and the blocked read is indistinguishable from a working one that returned the value "
           "already there. Either the report was removed, its wording no longer carries the "
           "marker, or the effective CEC log level has been lowered below LOG_EXP - in which case "
           "every consumer of this diagnostic, this case included, is reading an empty log. "
           "Captured output was: [" << captured << "]";

    EXPECT_NE(captured.find(kAwaitedContractPhrase), std::string::npos)
        << "the block was reported without naming the awaited device settings HAL contract, so the "
           "report says the read is unavailable but not which input would make it available - "
           "which is the one thing it exists to tell whoever can unblock it. Captured output was: ["
        << captured << "]";
}

// The drift guard for the modelled Sink call paths, and the one case in this file that
// asserts about a file rather than about a driver.
//
// DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected measures
// authorized difference 3 through the two Sink call paths by modelling their exception
// handling: a try with an IOException arm and a generic arm for the first, an uncaught
// propagation for the second. That model is only evidence while it still describes the plugin,
// and by itself it would survive the plugin being reformatted, one of its catch arms being
// removed, the enable-time call acquiring a try block, or the Sink ceasing to call the
// middleware at all - in each of which the modelled case would keep passing while the claim it
// makes about the caller had become false.
//
// So the structure is read from the plugin source here and checked against the model. What this
// case establishes: that the plugin still contains exactly two calls to
// LibCCEC::getInstance().addLogicalAddress(...), that the first of them still sits inside a try
// whose handler list includes an IOException arm and a generic catch(...) arm, and that the
// enable-time one still sits inside no try block. Those are precisely the three structural
// facts the modelled case depends on, and no more: this case asserts nothing about what the
// handlers do, which is the plugin's business and is out of scope.
//
// What it deliberately does not do. It does not compile or link the plugin - that component's
// own test binaries link the test framework's CEC mock in place of this middleware, so the
// plugin cannot be driven from this binary, and building it here would pull in Thunder. It
// therefore proves the call shape has not drifted, not that the plugin behaves as modelled at
// run time; that half remains the plugin component's own suite and the middleware-level L3
// step. Nor does it read the plugin's line numbers: the modelled case cites them for a human
// reader, and a citation drifting is a documentation matter, whereas the structure drifting
// would silently invalidate an assertion.
//
// Why it is in this fixture. This is one of the any-back-end fixtures, so it executes on every
// invocation including the legacy-only ones - which is the whole point, because a host with no
// binder driver never runs the session suite at all, and a drift guard that only ran where
// binder exists would be absent from exactly the runs that are cheapest to perform.
//
// The source is a required acceptance input, not an optional diagnostic, so absence fails. A
// case that printed a paragraph and returned when the source was nowhere to be found would be
// recorded by GoogleTest as a pass - so an acceptance run could go green while the only check
// that ties the modelled Sink call paths to the real plugin never executed at all. That is the
// shape of swallowed evidence this file exists to avoid.
//
// How the source is located, and what each of the three outcomes means:
//   - CEC_SINK_CALLER_SOURCE set and readable -> that file is used, and the path it was read
//     from is reported so the log records which revision the guard actually measured.
//   - CEC_SINK_CALLER_SOURCE set and not readable -> hard failure, wiring fault. The
//     environment claims to have provided the source and has not.
//   - not found at all, by the variable or by any candidate path -> hard failure, missing
//     required input. Every path tried is named, along with the two ways to supply it.
// The two failures keep distinct wording on purpose: "you told me where it is and it is not
// there" and "nobody told me and it is not beside me either" call for different fixes.
//
// Provenance of what is read. Both CI workflows check the Sink component out at the reviewed
// immutable commit 2ad7e1a4712908a4d0fb838caebcbfcaabba55d8 and export
// CEC_SINK_CALLER_SOURCE to the absolute path inside that checkout - a required step, not a
// tolerated one, so a network or permission fault fails the job instead of quietly disabling
// this guard. What the guard reads is therefore a reviewed revision rather than whatever a
// branch happens to hold, and the structural facts asserted below were read from that same
// revision. A drift is consequently noticed at the moment somebody deliberately bumps the pin,
// which is when a human is present to decide whether the model must be re-derived - rather
// than silently, months later, on a branch nobody was watching.
/**
 * @brief The Sink call paths modelled by the difference-3 case still match the real plugin
 *        source, so the model cannot go stale unnoticed.
 * @pre Runs under every invocation. The Sink source must be reachable: through
 *      CEC_SINK_CALLER_SOURCE, or beside the runner. Absence is a hard failure.
 * @note Three outcomes, with distinct wording on purpose. The variable set and readable uses
 *       that file and reports which path was measured; set and unreadable is a wiring fault;
 *       not found at all is a missing required input, and every path tried is named. "You
 *       told me where it is and it is not there" and "nobody told me and it is not beside me
 *       either" call for different fixes.
 * @warning Absence must fail rather than report. A version that printed a paragraph and
 *          returned would be recorded as a pass, so an acceptance run could go green while
 *          the only check tying the model to the real plugin never executed.
 * @note Both CI workflows check the Sink component out at a reviewed immutable commit and
 *       export the variable to an absolute path inside it, as a required step, so what is
 *       read is a reviewed revision rather than whatever a branch holds. A drift is therefore
 *       noticed when somebody deliberately bumps the pin and a human is present to decide
 *       whether the model must be re-derived.
 */
TEST_F(DriverAidlLocalInstanceTest, TheModelledSinkCallPathsStillMatchTheRealSinkSource) {
    const SinkCallerSource source = locateAndReadSinkCallerSource();

    if (source.variableSet && !source.found) {
        FAIL() << kSinkCallerSourceVariable << " is set to [" << source.variableValue
               << "] but that file could not be read. Both CI workflows export this variable "
                  "only after confirming the file exists, so a set-but-unreadable value is a "
                  "WIRING FAULT in the environment: something pointed at a source that is not "
                  "there. It is deliberately diagnosed apart from 'nothing pointed at the "
                  "source at all', which the arm below reports, because the two call for "
                  "different fixes - and neither is tolerated, since either would leave the "
                  "only guard that notices the modelled Sink call paths drifting unexecuted";
    }

    if (!source.found) {
        // missing required input. Distinct from the wiring fault above: nothing pointed at the
        // source and it is not beside this component either. It fails rather than reports,
        // because returning here would be recorded as a pass and the only check that ties the
        // modelled Sink call paths to the real plugin would have been skipped invisibly.
        FAIL() << "the real Sink caller source could not be found, so the drift guard for the "
                  "modelled Sink call paths cannot run. IT IS A REQUIRED ACCEPTANCE INPUT, NOT "
                  "AN OPTIONAL DIAGNOSTIC: without it, "
                  "DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPaths"
                  "AsExpected still asserts against a MODEL of the plugin that nothing has "
                  "checked against its subject, and a model nobody checks is not evidence about "
                  "a caller. "
               << kSinkCallerSourceVariable << " was not set, and these paths were tried: "
               << joinPaths(source.pathsTried)
               << ". SUPPLY IT EITHER WAY: set " << kSinkCallerSourceVariable
               << " to the absolute path of " << kSinkCallerRelativePath
               << " - which is what both CI workflows do, from a checkout pinned at the "
                  "reviewed commit 2ad7e1a4712908a4d0fb838caebcbfcaabba55d8 - or check that "
                  "sibling component out beside this one so one of the candidate paths above "
                  "resolves";
    }

    CppSourceModel model;
    ASSERT_TRUE(model.scan(source.text, "addLogicalAddress("))
        << "the brace/comment/literal walk over [" << source.path
        << "] did not finish with a balanced block stack, so the model of that file cannot be "
           "trusted and no conclusion is drawn from it. The scanner handles line comments, block "
           "comments, string and character literals, but NOT raw string literals - if the plugin "
           "has adopted R\"(...)\" the guard must be re-derived rather than relaxed";

    const std::vector<SourceCallSite> &sites = model.callSites();

    ASSERT_EQ(sites.size(), 2u)
        << "[" << source.path << "] contains " << sites.size()
        << " calls to addLogicalAddress(, not the two the modelled case is written around. If the "
           "count has fallen the Sink has stopped calling the middleware on one of its paths and "
           "the model asserts about a caller that no longer exists; if it has risen there is a "
           "third call path whose exception disposition nobody has measured. Either way the model "
           "in DriverAidlSessionTest.AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected "
           "must be re-derived from the plugin before it is trusted again";

    for (size_t i = 0; i < sites.size(); i++) {
        EXPECT_NE(sites[i].precedingText.find("LibCCEC::getInstance()."), std::string::npos)
            << "the call at [" << source.path << ":" << sites[i].line
            << "] is not reached through LibCCEC::getInstance(), so it is some other "
               "addLogicalAddress and the guard is measuring the wrong call. Preceding code as "
               "read: [" << sites[i].precedingText << "]";
    }

    // Path 1 - the allocation-loop call. Inside a try, and that try's handler list must still
    // contain both arms the model depends on: the distinct IOException arm a transport failure
    // lands in, and the generic arm a refusal falls through to.
    ASSERT_TRUE(sites[0].insideAnyTry)
        << "the first addLogicalAddress call, at [" << source.path << ":" << sites[0].line
        << "], is no longer inside a try block. The modelled case asserts that a refusal reaches "
           "a handled generic arm there and that a transport failure reaches a distinct "
           "IOException arm; with no try at all, both exceptions now propagate out of the "
           "allocation loop and the model describes handling that is gone";

    ASSERT_GE(sites[0].innermostTry, 0);
    const SourceTryBlock &guardingTry =
        model.tryBlocks()[static_cast<size_t>(sites[0].innermostTry)];

    bool hasIoExceptionHandler = false;
    bool hasGenericHandler = false;
    std::string handlerList;

    for (size_t i = 0; i < guardingTry.handlers.size(); i++) {
        if (!handlerList.empty()) {
            handlerList += " | ";
        }
        handlerList += guardingTry.handlers[i];

        if (guardingTry.handlers[i].find("IOException") != std::string::npos) {
            hasIoExceptionHandler = true;
        }
        if (guardingTry.handlers[i] == "...") {
            hasGenericHandler = true;
        }
    }

    EXPECT_TRUE(hasIoExceptionHandler)
        << "the try guarding the first addLogicalAddress call at [" << source.path << ":"
        << sites[0].line
        << "] has no IOException handler. The modelled case asserts that a NON-OK BINDER STATUS "
           "reaches a distinct IOException arm on this path - the one failure this caller "
           "diagnoses specifically - and without that arm it would land in the generic one "
           "instead, indistinguishable from a refused address. Handlers as read: [" << handlerList
        << "]";

    EXPECT_TRUE(hasGenericHandler)
        << "the try guarding the first addLogicalAddress call at [" << source.path << ":"
        << sites[0].line
        << "] has no catch(...) handler. The modelled case asserts that a REFUSED ADDRESS - which "
           "is AddressNotAvailableException, not IOException, and is authorized difference 3's "
           "coarser category - still lands in a handled arm on this path. Without a generic arm "
           "it would propagate out of the allocation loop instead. Handlers as read: ["
        << handlerList << "]";

    // Path 2 - the enable-time call. Outside every try, which is what makes the modelled
    // uncaught-propagation assertion the right shape for it.
    EXPECT_FALSE(sites[1].insideAnyTry)
        << "the second addLogicalAddress call, at [" << source.path << ":" << sites[1].line
        << "], is now inside a try block. The modelled case asserts that the exception PROPAGATES "
           "out of this path uncaught, which was the caller-visible consequence worth measuring; "
           "if the plugin now handles it, the interesting question becomes which arm it lands in "
           "and the model must be re-derived to ask that instead";

    std::cout << "[DriverAidlLocalInstanceTest] the modelled Sink call paths were checked against "
              << source.path << " (path 1 at line " << sites[0].line << ", inside a try with ["
              << handlerList << "]; path 2 at line " << sites[1].line << ", inside no try)."
              << std::endl;
}

// The log-level guard is itself exercised here, on an invocation that actually runs.
//
// Why this case exists and why it is in *this* fixture. `ScopedCecLogLevel` mutates two
// pieces of process-global, host-global state: the file production hardcodes at
// /tmp/cec_log_enabled, which lives in a world-writable directory and is shared with every
// other process on the host, and the file-static `cec_log_level` that file feeds. Its only
// other caller is a DriverAidlSessionTest case, and that whole fixture requires the AIDL
// back-end to be the resolved one - so on a host with no binder driver the guard's custody
// protocol would be compiled and never executed, which is exactly the shape of missing
// evidence this suite exists to refuse. DriverAidlLocalInstanceTest runs under every
// invocation, needs no service and no back-end, so the guard is exercised on every host.
//
// What is asserted, and why each assertion is not implied by the others:
//   - the guard raises: the effective level after construction is DEBUG, observed by
//     emitting at each level rather than read from the file, because the file is an input to
//     production's reader and not the state under test;
//   - the raise is a move, not a coincidence: the level observed on entry is recorded and
//     required to differ from DEBUG only insofar as it must come back to it later - the
//     entry level is whatever the host had, and this case asserts return-to-it rather than
//     any particular value, because a host that legitimately runs at DEBUG must still pass;
//   - restoration is verified rather than attempted: restoreAndVerify() is required to
//     report success, and it is the call that checks publication, then the file's bytes or
//     its absence, then the effective level, all while it still holds the custody lock;
//   - the level really is back afterwards, observed independently of the guard's own check,
//     so a restoreAndVerify() that returned true without doing anything would still fail
//     here;
//   - the second call is idempotent, because the destructor will make one and a
//     double-restore that rewrote the host's file would be a defect;
//   - nothing is left behind at the shared path: it is absent, or it is a regular file, and
//     in neither case is it the guard's temporary.
//
// A refusal is a failure here, deliberately. If another run_L1Tests holds the custody lock,
// or the path is a symlink, or it is owned by another user, the guard refuses and reports
// why - and this case fails with that reason rather than passing on the grounds that the
// guard was careful. The alternative reading, that a refusal is an environment condition to
// tolerate, would make the guard's whole protocol unfalsifiable from the suite.
/**
 * @brief The log-level guard raises the level, restores it verifiably, is idempotent, and
 *        leaves nothing behind at the shared path.
 * @pre Runs under every invocation. The effective level must be observable, and custody of
 *      the shared configuration path must be obtainable.
 * @note Evidence, in order: the level observed on entry is required only to come back, not
 *       to be any particular value, so a host legitimately running at DEBUG still passes;
 *       restoration is required to report success while the custody lock is still held;
 *       the level is then observed independently, so a restore that returned true without
 *       doing anything still fails; a second call is asserted idempotent, because the
 *       destructor will make one; and the shared path is left absent or a regular file that
 *       is not the guard's temporary.
 * @warning A refusal is a failure here, deliberately. Reading a refusal as an environment
 *          condition to tolerate would make the guard's whole protocol unfalsifiable from
 *          the suite.
 */
TEST_F(DriverAidlLocalInstanceTest, TheLogLevelGuardRaisesTheLevelAndRestoresItVerifiably) {
    const int levelBefore = ScopedCecLogLevel::observeEffectiveLevel();

    ASSERT_NE(ScopedCecLogLevel::unobservableLevel(), levelBefore)
        << "no CCEC_LOG level produced any output, so the effective log level could not be "
           "observed at all and this case cannot tell a restored level from an unrestored one. "
           "That is an observation failure rather than a defect in the guard - stdout could not "
           "be captured - and it is reported as a failure because proceeding would assert "
           "nothing";

    {
        ScopedCecLogLevel debugLevel("DEBUG");

        ASSERT_TRUE(debugLevel.isRaised())
            << "the guard refused to take custody of " << "/tmp/cec_log_enabled"
            << " and so did not raise the level. This is asserted rather than tolerated: the "
               "refusal reasons are all conditions a reader must act on - another run_L1Tests "
               "holding the custody lock, a symlink or a non-regular file at the path, foreign "
               "ownership, or a file too large to reproduce byte-for-byte. Reason given: ["
            << debugLevel.failureReason() << "]";

        EXPECT_EQ(levelBefore, debugLevel.entryLevel())
            << "the guard recorded a different entry level than this case observed immediately "
               "before constructing it, which means something moved the process-wide level "
               "between the two observations. Every later comparison in this case rests on the "
               "two agreeing, so a mismatch is reported rather than reconciled";

        EXPECT_EQ(LOG_DEBUG, ScopedCecLogLevel::observeEffectiveLevel())
            << "the guard reported that it raised the level to DEBUG, but an emission at DEBUG "
               "did not survive - so the level did not actually move. The whole purpose of the "
               "guard is to make a LOG_DEBUG line observable in a case that asserts on it, and "
               "a guard that reports success without moving the level would let such a case "
               "assert against silence";

        std::string restoreDetail;
        ASSERT_TRUE(debugLevel.restoreAndVerify(restoreDetail))
            << "restoreAndVerify() reported that it could not prove the host was put back. It "
               "checks three things in order while still holding the custody lock - that the "
               "publication succeeded, that the path holds the original bytes or is absent "
               "again, and that the effective level returned - and a failure of any of them "
               "means this run altered shared state it did not restore. Detail: ["
            << restoreDetail << "]";

        EXPECT_EQ(levelBefore, ScopedCecLogLevel::observeEffectiveLevel())
            << "the level was NOT back to what this case found after restoreAndVerify() "
               "reported success, which is the one claim that cannot be taken on trust: it is "
               "checked here independently of the guard's own verification, so a restoration "
               "that reported success without performing one is caught rather than believed";

        std::string secondDetail;
        EXPECT_TRUE(debugLevel.restoreAndVerify(secondDetail))
            << "a second restoreAndVerify() failed, so the operation is not idempotent - and "
               "it must be, because the destructor makes exactly this call on the path where a "
               "fatal assertion skipped the explicit one. Detail: [" << secondDetail << "]";
    }

    EXPECT_EQ(levelBefore, ScopedCecLogLevel::observeEffectiveLevel())
        << "the destructor left the process-wide level somewhere other than where this case "
           "found it, which would leak verbosity into every later case in this binary";

    struct stat pathStatus;
    if (::lstat("/tmp/cec_log_enabled", &pathStatus) == 0) {
        EXPECT_TRUE(S_ISREG(pathStatus.st_mode))
            << "/tmp/cec_log_enabled is no longer a regular file after the guard released it. "
               "The guard publishes only through a rename of a regular file it created itself, "
               "so anything else at that path came from elsewhere - and a later run would "
               "refuse to take custody of it";
    }
}


/**
 * @brief The legacy arms of the two authorized differences, and SC6(f)'s legacy half.
 *
 * Requires invocation a - CEC_TEST_AIDL_MODE=absent or unset - because these cases drive the
 * process-global driver and it must be the legacy one. SetUp asserts that, with a message
 * naming the mode, so a mis-wired filter fails on a diagnostic rather than on a mock
 * expectation that was never going to be met.
 *
 * Why these are here at all, when the AIDL fixtures below assert the other side. Each of the
 * two authorized observable differences between the back-ends is a pair of behaviours, and a
 * declared difference is only established by asserting both halves against each other. The
 * legacy halves live here, on invocation A; the AIDL halves live in the two fixtures below,
 * on invocation B. Splitting them is not a preference - a single process can only ever hold
 * one back-end - and the pairing is what turns "the AIDL back-end raises" into "the two
 * back-ends differ, in exactly this way, and nowhere else".
 *
 * These cases drive the shared driver rather than a local DriverImpl, and deliberately: the
 * legacy arms need an open driver, and the shared one is the only driver in this process
 * whose HAL is the mock the fixture programs.
 *
 * One case here is not an authorized-difference arm: the receive-path case. The receive path is
 * not a declared difference - both back-ends must deliver an inbound frame identically - so its
 * legacy half is a baseline rather than one side of a pair, and it is measured here because this
 * is the only invocation that executes on a host without a binder driver. It also exercises the
 * same observation machinery the AIDL receive cases use, so a wiring fault in that machinery
 * fails here, on any host, instead of masquerading as an AIDL back-end failure on the
 * binder-capable runner.
 *
 * Self-sufficiency. SetUp establishes the open precondition for itself instead of inheriting
 * whatever the previously executed suite left behind, and TearDown restores it. Each case
 * programs its own mock expectations and TearDown clears them. Nothing is carried between
 * cases and the driver is left open, which is the state the global environment set up.
 */
// The interleaving itself: close() is a producer on the incoming queue, so the
// receive path must leave it a slot and must not report a frame accepted that the queue
// silently dropped.
//
// The bug this case is the regression test for. offerReceivedFrame() observed the occupancy
// under queueProducerMutex and offered on the next line; close() offered its NULL sentinel
// while holding only the instance lock. So: the receive path saw INCOMING_QUEUE_CAPACITY - 1
// entries and decided there was room, close() landed the sentinel in the last slot, and
// EventQueue::offer() - which returns void and whose at-capacity branch is empty - discarded
// the received frame. offerReceivedFrame() still returned true, so
// EventListener::onMessageReceived() cleared its only pointer instead of deleting it: one
// leaked frame per event, unbounded on a path a remote HAL drives.
//
// What is asserted, in the order the interleaving ran. The queue is filled to
// INCOMING_QUEUE_CAPACITY - 1 through the production handoff itself; the frame that would take
// the last slot is refused, so the caller keeps and releases it; close() then lands its
// sentinel in the slot that refusal preserved, and the occupancy proves it went in rather than
// being swallowed; and a frame arriving after the close is reported rejected too. Finally the
// queue's content is counted: exactly the frames the handoff claimed, plus exactly one
// sentinel.
//
// Without the reservation this case fails, which is what makes it a regression test rather than
// a description - measured, not assumed: the refusal at the limit reports true and the occupancy
// goes to INCOMING_QUEUE_CAPACITY. Note that the occupancy check after close() is not sufficient
// on its own, because in that state it also reads as capacity - out of one frame too many and no
// sentinel; the drain at the end is what separates those two, by counting frames and sentinels
// apart.
//
// Ownership follows the handoff's report throughout, so that against broken code this case
// reports failures instead of double-freeing and aborting before its later assertions run. A
// frame is deleted here only where the handoff said the caller still owns it.
//
// The other half of the fix - that close()'s offer is serialized on queueProducerMutex - is not
// what this case or the one after it measures, and saying so is the point rather than a caveat.
// Both are serial, so delete `{AutoLock lock_(queueProducerMutex);` from close() and both still
// pass: a lock that is never contended is a lock whose acquisition no single-threaded case can
// see. That half is measured by the three concurrent cases that follow this pair -
// CloseSentinelOfferBlocksOnTheProducerLockAConcurrentTestHolds and
// CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner, which hold the
// lock from the test thread and require the production producers not to complete while it is
// held, and ManyRealOverlapsKeepTheHandoffReportAndTheQueueInAgreement, which holds no lock and
// instead catches the unserialized sentinel as a violated ownership invariant over many real
// races. The five together cover both halves; no subset is sufficient alone.
/**
 * @brief The receive handoff reserves the queue's last slot for close()'s sentinel and
 *        refuses the frame that would take it.
 * @pre Runs under every invocation, through ReceiveQueueProbe, whose state is forced to
 *      OPENED so the handoff is reachable without a service. Needs a capacity of at least
 *      two.
 * @note Single-threaded, and that is stated rather than left implicit: it measures the
 *       accounting half only. Both producers being serial here means the producer-lock
 *       acquisition could be deleted and this case would still pass - a lock that is never
 *       contended is one no single-threaded case can see. The three concurrent cases below
 *       measure that half.
 * @note Evidence: the occupancy check after close() is not sufficient alone, because a
 *       broken implementation also reads as capacity there - one frame too many and no
 *       sentinel. The counting drain at the end separates those two. Ownership follows the
 *       handoff's report throughout, so against broken code this reports failures rather
 *       than double-freeing and aborting before its later assertions run.
 */
TEST_F(DriverAidlLocalInstanceTest, ReceiveQueueReservesTheLastSlotForCloseSentinelAndRefusesTheFrameThatWouldTakeIt) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GE(capacity, 2u)
        << "the capacity leaves no room for both a received frame and close()'s sentinel, so "
           "this case cannot mean what it says";

    // LEGACY PARITY, PINNED TO A LITERAL AND NOT TO THE PRODUCTION CONSTANT. Everything else in
    // this case derives from `capacity`, which is what makes it a test of the reserve rather than
    // of the depth: it would pass just as happily against a 32-entry queue that refused at 31,
    // which is precisely the difference this back-end is required not to have. So the depth is
    // asserted here against the number the legacy back-end actually gets - 32, the default
    // `EventQueue(size_t cap = 32)` gives DriverImpl's queue, all of which its receive callback
    // may fill. A change to either side of the arithmetic fails here.
    //
    // The same equality is a static_assert in `ccec/src/DriverAidlImpl.hpp`, so the production
    // header cannot be edited into the difference at all. This assertion is not redundant with
    // it: the static_assert proves the arithmetic, and this proves that the code path actually
    // honours the arithmetic - the loop below accepts 32 frames and the 33rd offer is refused.
    ASSERT_EQ(capacity - 1, 32u)
        << "the depth available to received frames is " << (capacity - 1) << ", not the 32 the "
           "legacy back-end gets from EventQueue's default capacity. close()'s reserved slot must "
           "come out of an extra queue entry, never out of the depth a caller sees";

    ReceiveQueueProbe probe;
    probe.markOpened();

    // 1. Fill to the receive limit through the production handoff. Every one of these must be
    //    accepted: a refusal before the limit would mean the reserved slot cost more than one
    //    entry of depth.
    for (size_t i = 0; i < capacity - 1; i++) {
        CECFrame *frame = new CECFrame(directedFrame());

        // Ownership follows the reported return value, exactly as onMessageReceived() does: the
        // frame is released here if and only if the handoff said the queue did not take it. That
        // discipline is what stops this case from becoming a double free of its own against
        // broken code - on which it must report a failure, not corrupt the heap and abort before
        // the remaining assertions run.
        const bool taken = probe.offer(frame);

        EXPECT_TRUE(taken)
            << "the handoff refused frame " << i << " of " << (capacity - 1) << ", below the "
               "receive limit. The reserved slot must cost exactly one entry of queue depth";

        if (!taken) {
            delete frame;
        }

        ASSERT_EQ(probe.occupancy(), i + 1)
            << "the queue holds " << probe.occupancy() << " entries after " << (i + 1)
            << " offers, so what the handoff reported and what the queue did have diverged";
    }

    ASSERT_EQ(probe.occupancy(), capacity - 1)
        << "the queue did not reach the receive limit, so the case that follows would not be "
           "testing a full queue at all";

    // 2. The frame that would take the sentinel's slot is refused, and the caller therefore
    //    still owns it - which is the whole of the ownership contract. Without the
    //    reservation this would return true and the queue would go to capacity.
    CECFrame *displacing = new CECFrame(directedFrame());
    const bool displacingTaken = probe.offer(displacing);

    EXPECT_FALSE(displacingTaken)
        << "the handoff took a frame into the slot reserved for close()'s sentinel. That is the "
           "producer-overlap interleaving: the sentinel would then be the entry "
           "EventQueue::offer() discards, "
           "and a blocked Bus reader would never wake";

    EXPECT_EQ(probe.occupancy(), capacity - 1)
        << "the refused frame was queued anyway, so the return value and the queue disagree "
           "about who owns it - the caller will release a frame the queue still holds";

    // The caller releases it only because the handoff said the caller still owns it, which is
    // precisely what onMessageReceived() does on a false return. Deleting it unconditionally
    // would make this case double-free against code that reported acceptance, which is the one
    // outcome worse than the leak being tested.
    if (!displacingTaken) {
        delete displacing;
    }

    // 3. close() lands its sentinel in the slot that refusal preserved. IOException is expected
    //    and is not the point: a local instance holds no service proxy, so the close transaction
    //    reports DEAD_OBJECT - and the sentinel is offered before that, which is exactly why a
    //    blocked reader unwinds even when the HAL close fails.
    EXPECT_THROW({ probe.close(); }, IOException)
        << "close() on an instance holding no service proxy must still report the failed "
           "transaction. If it returned cleanly, the sentinel assertion below would be proving "
           "something about a different code path";

    EXPECT_EQ(probe.occupancy(), capacity)
        << "close()'s NULL sentinel was SWALLOWED by the full queue. EventQueue::offer() drops "
           "silently at capacity, so the Bus reader would stay blocked in poll() with nothing "
           "left to wake it - which is the failure the reserved slot exists to prevent";

    // 4. A frame arriving after the close is reported rejected rather than accepted and dropped.
    //    The state guard in getIncomingQueue() raises before anything is offered, and that
    //    exception is what drives the caller's release - the same mechanism as the legacy path.
    CECFrame *afterClose = new CECFrame(directedFrame());
    bool afterCloseTaken = false;

    EXPECT_THROW({ afterCloseTaken = probe.offer(afterClose); }, InvalidStateException)
        << "a frame offered after close() was not rejected. Accepting it would put a frame on a "
           "queue nothing will drain, and reporting acceptance without queueing it would leak it";

    EXPECT_EQ(probe.occupancy(), capacity)
        << "the post-close frame reached the queue although the state guard should have raised "
           "before the offer";

    if (!afterCloseTaken) {
        delete afterClose;
    }

    // 5. What the queue actually holds is exactly what was accepted, plus exactly one sentinel.
    //    Counting both is what distinguishes "the sentinel went in" from "an extra frame did".
    size_t frames = 0;
    size_t sentinels = 0;

    probe.drainCounting(frames, sentinels);

    EXPECT_EQ(frames, capacity - 1)
        << "the queue held " << frames << " frames where the handoff accepted " << (capacity - 1);

    EXPECT_EQ(sentinels, 1u)
        << "the queue held " << sentinels << " NULL sentinels; close() posts exactly one and it "
           "must not be droppable";
}

// The ownership ledger: across a queue driven past its receive limit, exactly one
// owner releases each frame - never none, which is the leak, and never two, which is worse.
//
// Why a ledger rather than an allocation count. The defect is not that a frame is
// dropped - a full queue must drop something - but that the handoff's return value stopped
// agreeing with what the queue did, leaving no owner at all. So this case records, for every
// frame it allocates, which side the production code said owns it, then drains the queue and
// checks that the two sets partition the allocations: each frame appears once, in exactly one
// of them. A frame in neither was accepted and dropped, and is leaked in production; a frame in
// both would be released twice.
//
// What this case pins, and what it does not claim to reproduce. The "owners == 0" arm is the
// production leak: the handoff reports acceptance and the queue never took the frame. Reaching it
// requires the offer to be dropped, which needs close()'s sentinel to land unserialized in the
// window between the check and the offer - a genuine interleaving, not something one
// thread can stage. That interleaving is staged, by
// CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner below, which parks
// both producers on the lock and releases them together; this case is the single-threaded half
// and does not duplicate it. What a single thread does establish is the accounting that makes
// the drop impossible in the first place: the queue must take exactly
// INCOMING_QUEUE_CAPACITY - 1 received frames and no more, so the sentinel's slot is never the
// one at stake. The ownership partition is asserted alongside it because a change that fixed the
// count and broke the handoff's report would otherwise pass.
//
// The membership test is a nested scan rather than a set, deliberately: the ledger is a few tens
// of entries with a fixed bound, so the cost is irrelevant, and the alternative would add an
// include to a translation unit whose include block is documented entry by entry.
/**
 * @brief Across a queue driven past its receive limit, every frame has exactly one owner -
 *        the queue or the caller, never both and never neither.
 * @pre Runs under every invocation, through ReceiveQueueProbe forced to OPENED.
 * @note A ledger rather than an allocation count, because the defect is not that a frame is
 *       never allocated but that nothing ends up responsible for it. Each frame is recorded
 *       with the side production said owns it; the drain then has to partition the
 *       allocations. A frame in neither was accepted and dropped, and leaks in production; a
 *       frame in both would be released twice.
 * @note What it does not claim to reproduce: the zero-owner arm needs close()'s sentinel to
 *       land unserialized inside the check-to-offer window, which is a genuine interleaving
 *       no single thread can stage. The overlap case below stages it. What one thread does
 *       establish is the accounting that makes the drop impossible in the first place, and
 *       the partition is asserted alongside so a change that fixed the count and broke the
 *       report cannot pass.
 */
TEST_F(DriverAidlLocalInstanceTest, EveryFrameOfferedToAFullReceiveQueueHasExactlyOneOwner) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GE(capacity, 2u)
        << "the capacity leaves no room for both a received frame and close()'s sentinel";

    // Comfortably past the limit, so the refusal arm is exercised repeatedly rather than once:
    // a stalled Bus reader keeps the queue full for as long as the HAL keeps delivering, and it
    // was every further event that leaked a frame.
    const size_t offeredCount = capacity + 8;

    ReceiveQueueProbe probe;
    probe.markOpened();

    std::vector<CECFrame *> allocated;
    std::vector<CECFrame *> ownedByCaller;   // the handoff returned false: the caller still owns
    std::vector<CECFrame *> ownedByQueue;    // the handoff returned true: the queue owns

    for (size_t i = 0; i < offeredCount; i++) {
        CECFrame *frame = new CECFrame(directedFrame());
        allocated.push_back(frame);

        if (probe.offer(frame)) {
            ownedByQueue.push_back(frame);
        }
        else {
            ownedByCaller.push_back(frame);
        }
    }

    EXPECT_EQ(ownedByQueue.size(), capacity - 1)
        << "the handoff claimed " << ownedByQueue.size() << " frames where the queue can hold "
           << (capacity - 1) << " received ones with the last slot reserved for the sentinel";

    EXPECT_EQ(ownedByCaller.size(), offeredCount - (capacity - 1))
        << "the handoff returned false " << ownedByCaller.size() << " times for "
           << (offeredCount - (capacity - 1)) << " offers made past the receive limit";

    EXPECT_EQ(probe.occupancy(), capacity - 1)
        << "the queue's occupancy does not match what the handoff claimed to have taken";

    // What the queue really holds. Taken out here rather than left for the probe's destructor,
    // because the identity of each entry is the evidence this case rests on.
    std::vector<CECFrame *> takenFromQueue;
    size_t sentinels = 0;

    probe.drainInto(takenFromQueue, sentinels);

    EXPECT_EQ(sentinels, 0u)
        << "a NULL sentinel was on the queue although close() was never called here";

    EXPECT_EQ(takenFromQueue.size(), ownedByQueue.size())
        << "the queue yielded " << takenFromQueue.size() << " frames where the handoff reported "
           << ownedByQueue.size() << " accepted. A shortfall is the ownership leak: the return "
              "value said the queue had taken a frame that EventQueue::offer() had discarded";

    // The partition. One owner per allocation, counted, so that "no owner" and "two owners" are
    // distinguished from each other and from the expected answer.
    for (size_t i = 0; i < allocated.size(); i++) {
        size_t owners = 0;

        for (size_t q = 0; q < takenFromQueue.size(); q++) {
            if (takenFromQueue[q] == allocated[i]) {
                owners++;
            }
        }

        for (size_t c = 0; c < ownedByCaller.size(); c++) {
            if (ownedByCaller[c] == allocated[i]) {
                owners++;
            }
        }

        EXPECT_EQ(owners, 1u)
            << "frame " << i << " of " << allocated.size() << " has " << owners << " owners. "
               "Zero means the handoff reported it accepted and the queue never took it, so "
               "nothing will ever release it - the ownership leak. Two means both the queue and "
               "the caller would release it";
    }

    // Released exactly once each, now that the partition above has established that no entry is
    // owned twice and none is orphaned. Every allocation is released whatever the assertions
    // above reported, so a failing run does not leak on top of failing.
    for (size_t i = 0; i < allocated.size(); i++) {
        delete allocated[i];
    }
}

// The missing acquisition itself: close() must take queueProducerMutex before it
// offers its NULL sentinel, and this is the case that fails if it stops.
//
// Why the two cases above are not enough, stated plainly because it is the reason this one
// exists. Both are serial: they fill the queue, offer once more and only then call close(). A
// lock that is never contended is a lock whose acquisition is invisible, so delete
// `{AutoLock lock_(queueProducerMutex);` from close() and both still pass - they measure the
// reserved slot, which is the other half of the fix. Nothing above measures the serialization.
//
// How this one measures it. The test thread takes queueProducerMutex itself, through the
// probe's producerLock() accessor, and then drives the real production close() from a second
// thread. close() takes the instance lock, moves the state to CLOSING and then reaches for the
// producer lock - which this thread is holding - so it cannot get as far as its offer. Two
// independent observations follow, and both are direct:
//
//   1. the worker does not complete within PRODUCER_LOCK_OBSERVATION_MS. With the acquisition
//      in place that is impossible to violate; without it close() runs to completion in
//      microseconds and the observation fails;
//   2. the queue's occupancy does not move while the lock is held. That is the same fact read
//      from the other side - the sentinel itself has not landed - and it fails for the same
//      reason, so a regression is reported twice rather than once.
//
// Then the lock is released and the worker is required to finish, which is what proves the
// first observation was "blocked" rather than "never started" or "deadlocked": a case that
// asserted only non-completion would pass against a close() that hung forever.
//
// It calls production code and not a copy of it, which is the property the whole case turns
// on. A test that re-stated close()'s `{AutoLock lock_(queueProducerMutex); rQueue.offer(0); }`
// would keep passing after production stopped taking the lock, and would therefore pin
// nothing at all.
//
// The failure direction is deterministic, not a race, and the asymmetry is worth naming: no
// scheduling order can let close() complete while another thread holds the lock it must take,
// so a passing run cannot flake; and the work close() has left to do once unblocked is two
// size reads and a deque push, so 400 ms of margin cannot hide a missing acquisition. Measured
// both ways before this was committed - with the acquisition deleted from close() this case
// fails on observation 1, with it restored it passes.
//
// No sleep is used anywhere here. Every wait is a bounded MonotonicLatch wait that returns the
// moment the worker signals, so the only elapsed time in a passing run is the one deliberate
// negative observation - and the bound is on std::chrono::steady_clock, which a wall-clock step
// cannot move.
//
// What this case proves, and what it does not. It is the timing complement rather than the
// proof, and the distinction matters because a reader who took it for the proof would trust it
// too far. closeEntered is signalled by the worker before it calls close(), so the latch
// establishes that the worker started - not that it reached the producer-lock acquisition. A
// worker descheduled immediately after signalling would satisfy the non-completion observation
// for a reason that has nothing to do with the lock, and every later assertion here would still
// hold once the test released it. That is a scheduling counterexample and it cannot be closed
// from this side of the latch. The case is kept because in practice it detects the missing
// acquisition immediately and reports it in one specific, readable observation; the case that
// carries the weight is the stress case below, which does not reason about time at all and
// instead fails on a violated invariant across many real overlaps.
/**
 * @brief close()'s sentinel offer does not complete while another thread holds
 *        queueProducerMutex, and completes once it is released.
 * @pre Runs under every invocation. The harness holds the probe, latches and flags off this
 *      stack, because a worker that has to be abandoned outlives the case body.
 * @note The timing complement rather than the proof, and the distinction matters because a
 *       reader who took it for the proof would trust it too far. The latch establishes that
 *       the worker started, not that it reached the lock acquisition, so a worker descheduled
 *       immediately after signalling would satisfy the non-completion observation for a
 *       reason unrelated to the lock. That scheduling counterexample cannot be closed from
 *       this side of the latch. It is kept because it detects a missing acquisition
 *       immediately and reports it as one specific readable observation; the stress case
 *       below carries the weight.
 * @note No sleep anywhere: every wait is a bounded MonotonicLatch wait that returns the
 *       moment the worker signals, on a steady clock a wall-clock step cannot move.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseSentinelOfferBlocksOnTheProducerLockAConcurrentTestHolds) {
    // The probe, the latches and the flags the worker writes live in the harness, off this
    // stack, because a worker that has to be abandoned outlives this case body - see
    // QueueHandoffOverlapHarness. `shared` is a pointer captured by value into the worker, never a
    // reference to a local here.
    QueueHandoffOverlapHarness harness;
    QueueHandoffOverlapState  *shared = &*harness;

    shared->probe.markOpened();

    // One resident frame, nowhere near the limit: this case is about the lock and not about
    // capacity, and a non-empty queue gives the occupancy observation below a value that a
    // sentinel arriving early would visibly change.
    CECFrame *resident = new CECFrame(directedFrame());
    const bool residentTaken = shared->probe.offer(resident);

    // Ownership follows the reported return value, and it is acted on before the assertion
    // that can return from this case, so a refusal here reports a failure instead of leaking
    // the frame the handoff handed back.
    if (!residentTaken) {
        delete resident;
    }

    ASSERT_TRUE(residentTaken)
        << "the handoff refused the first frame offered to an empty queue, so this case cannot "
           "establish its precondition";

    ASSERT_EQ(shared->probe.occupancy(), 1u)
        << "the queue does not hold the one frame the handoff reported accepting";

    // Held as a pointer declared outside the lock scope, because the completion observation
    // after that scope needs the same worker the observation inside it watched.
    BoundedWorker *closer = NULL;

    {
        CCEC_OSAL::AutoLock heldByTest(shared->probe.producerLock());

        // shared->closeEntered is signalled by the worker immediately before it calls close(),
        // so that "did not complete" below cannot be satisfied by a thread that never ran; the
        // worker's own finished latch is signalled once close() has returned or raised.
        closer = &harness.start([shared]() {
            shared->closeEntered.notify();

            try {
                shared->probe.close();
                shared->closeReturnedCleanly = true;
            }
            catch (IOException &) {
                // expected, and not what this case is about: a local instance holds no service
                // proxy, so the close transaction reports DEAD_OBJECT. The sentinel is offered
                // before that transaction, which is why a blocked Bus reader unwinds even when
                // the HAL close fails - and why this case can read the sentinel's arrival from
                // a close() that raises.
                shared->closeRaisedIoException = true;
            }
            catch (...) {
                shared->closeRaisedSomethingElse = true;
            }
        });

        ASSERT_TRUE(shared->closeEntered.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "the worker never reached close(), so the observation below would be vacuous. "
               "This is a harness failure rather than a production one";

        // Observation 1: close() cannot get past its producer-lock acquisition.
        ASSERT_FALSE(closer->finishedWithin(PRODUCER_LOCK_OBSERVATION_MS))
            << "close() ran to COMPLETION while this test held queueProducerMutex, so it did "
               "not take that lock before offering its NULL sentinel. That is the "
               "missing close-side acquisition: close() is a producer on the incoming queue, and "
               "an unserialized sentinel can land between offerReceivedFrame()'s occupancy check "
               "and its offer, whereupon EventQueue::offer() discards the received frame "
               "silently while the handoff still reports it accepted - one leaked frame per "
               "event, on a path a remote HAL drives";

        // Observation 2: the same fact read from the queue rather than from the worker.
        EXPECT_EQ(shared->probe.occupancy(), 1u)
            << "an entry reached the incoming queue while this test held queueProducerMutex. "
               "The only other producer is close()'s sentinel offer, so the sentinel was "
               "offered without the lock - the close-side acquisition is missing";
    }

    // The lock is released. close() must now finish: this is what separates "blocked on the "
    // "lock" from "stuck", and a regression that deadlocked the producer fails here instead of
    // hanging the run.
    ASSERT_TRUE(closer->finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "close() did not complete within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms of queueProducerMutex being released, so it is not merely waiting for that "
           "lock. Everything it has left to do is one EventQueue::offer() - which takes the "
           "queue's own mutex and may allocate, but never waits for capacity - and a "
           "transaction that fails immediately on an instance holding no proxy";

    ASSERT_TRUE(harness.disposeWorkers())
        << "the close worker did not finish within " << WORKER_ABANDON_DEADLINE_MS
        << " ms of its body being observed complete, so it was abandoned rather than joined "
           "and the state it holds is leaked by design. Nothing below can be interpreted";

    // Read only after the join disposeWorkers() performed, which is the happens-before that
    // makes these plain bools safe to inspect from this thread.
    EXPECT_TRUE(shared->closeRaisedIoException)
        << "close() on an instance holding no service proxy must report the failed transaction "
           "as IOException; it "
        << (shared->closeReturnedCleanly
                ? "returned cleanly"
                : (shared->closeRaisedSomethingElse ? "raised some other exception"
                                                    : "did neither"))
        << ", so the assertions above were describing a different code path";

    EXPECT_EQ(shared->probe.occupancy(), 2u)
        << "the queue holds " << shared->probe.occupancy() << " entries where it should hold the "
           "resident frame plus close()'s one sentinel. The sentinel is what wakes a blocked "
           "Bus reader and it must arrive exactly once";

    // What the queue actually holds, counted apart, so "the sentinel arrived" is distinguished
    // from "a second frame did".
    size_t frames = 0;
    size_t sentinels = 0;

    shared->probe.drainCounting(frames, sentinels);

    EXPECT_EQ(frames, 1u)
        << "the queue yielded " << frames << " frames where exactly one was ever accepted";

    EXPECT_EQ(sentinels, 1u)
        << "the queue yielded " << sentinels << " NULL sentinels; close() posts exactly one, "
           "and the reserved slot exists so that it can never be the entry "
           "EventQueue::offer() swallows";
}

// The overlap the reservation and the producer lock exist for: a receive handoff and close()'s
// sentinel contending for the incoming queue at the same moment, with the ownership of every frame
// still accounted for exactly once afterwards.
//
// What makes this the real interleaving and not a staged one. Both producers are parked on
// queueProducerMutex - genuinely blocked inside production code, one inside
// offerReceivedFrame() and one inside close() - before the test releases the lock and lets
// them race. Which of the two then wins is the scheduler's choice and is deliberately not
// controlled: both orders are legitimate, both are asserted, and the assertions are written
// against whichever one actually happened.
//
// The occupancy is capacity - 2 when the race starts, which is the arrangement that makes the
// two orders differ observably:
//
//   receive first  the handoff sees capacity - 2, which is below its capacity - 1 refusal
//                  point, so it accepts and the queue reaches capacity - 1. close()'s sentinel
//                  then takes the last slot and the queue reaches capacity.
//   close first    the sentinel takes the queue to capacity - 1. The handoff then sees
//                  capacity - 1, refuses, and the caller keeps the frame.
//
// In both, and this is the invariant: the sentinel is never the entry EventQueue::offer()
// discards, and the handoff's return value agrees with what the queue really holds. Neither
// holds without both acquisitions - the sentinel can land inside the handoff's check-to-offer
// window, the frame is dropped, and true is returned all the same.
//
// It pins both acquisitions, not only close()'s. Each producer is required not to complete
// while the test holds the lock, so deleting the acquisition from either side fails this case
// on the corresponding observation. The receive side's non-completion is also what proves the
// sequencing this case needs: while it is parked the state is still OPENED, so it has already
// passed getIncomingQueue()'s guard and the close that follows cannot turn its offer into a
// state-guard rejection instead of a real contention.
//
// The ledger is the point of the assertions. Every frame allocated here is either owned by the
// queue - drained out of it by identity - or owned by the caller, because the handoff returned
// false. Never both, which would be a double free, and never neither, which is the ownership leak:
// the handoff reporting acceptance for a frame the queue never took, leaving nothing in the
// process that will ever release it.
/**
 * @brief A receive handoff and close()'s sentinel contending for the queue at the same
 *        moment leave every frame with exactly one owner, in either arrival order.
 * @pre Runs under every invocation, through the harness. Needs room for a filled queue, one
 *      contending frame and the sentinel.
 * @note It pins both acquisitions, not only close()'s: each producer is required not to
 *       complete while the test holds the lock, so deleting the acquisition from either side
 *       fails the corresponding observation. The receive side's non-completion is also what
 *       proves the sequencing - while parked its state is still OPENED, so it has already
 *       passed the incoming-queue guard and the close that follows is real contention rather
 *       than a state-guard rejection.
 * @note The ledger is the point of the assertions: every frame is owned by the queue, drained
 *       out of it by identity, or by the caller because the handoff returned false. Never
 *       both, which would be a double free, and never neither, which is the ownership leak.
 */
TEST_F(DriverAidlLocalInstanceTest, CloseSentinelOverlappingAReceiveHandoffLeavesEveryFrameWithExactlyOneOwner) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GE(capacity, 3u)
        << "this case needs room for a filled queue, one contending frame and close()'s "
           "sentinel, so a capacity below three cannot express it";

    // The probe, the latches, the flags the workers write and the frame ledger all live in the
    // harness, off this stack: a worker that has to be abandoned outlives this case body, and
    // the harness releases every allocation on every exit path - see QueueHandoffOverlapHarness.
    QueueHandoffOverlapHarness harness;
    QueueHandoffOverlapState  *shared = &*harness;

    shared->probe.markOpened();

    std::vector<CECFrame *> ownedByCaller;

    // Fill to capacity - 2 through the production handoff, leaving exactly two slots: one the
    // contending frame may take and one reserved for the sentinel.
    for (size_t i = 0; i < capacity - 2; i++) {
        CECFrame *frame = new CECFrame(directedFrame());
        shared->allocated.push_back(frame);

        if (!shared->probe.offer(frame)) {
            ownedByCaller.push_back(frame);
        }
    }

    ASSERT_EQ(shared->probe.occupancy(), capacity - 2)
        << "the queue did not reach " << (capacity - 2) << " entries, so the two orders this "
           "case distinguishes would not differ";

    ASSERT_TRUE(ownedByCaller.empty())
        << "the handoff refused " << ownedByCaller.size() << " frames below its refusal point, "
           "so the precondition this case rests on does not hold";

    shared->contending = new CECFrame(directedFrame());
    shared->allocated.push_back(shared->contending);

    // Held as pointers declared outside the lock scope, because the completion observations
    // after that scope need the same two workers the observations inside it watched.
    BoundedWorker *receiver = NULL;
    BoundedWorker *closer   = NULL;

    {
        CCEC_OSAL::AutoLock heldByTest(shared->probe.producerLock());

        receiver = &harness.start([shared]() {
            shared->receiveEntered.notify();

            try {
                shared->contendingAccepted = shared->probe.offer(shared->contending);
            }
            catch (InvalidStateException &) {
                // Only reachable if the driver stopped being OPENED before this thread read
                // the state. The sequencing below is arranged so that cannot happen, and the
                // assertion after the join says so rather than tolerating it.
                shared->contendingRefusedByStateGuard = true;
            }
            catch (...) {
                shared->contendingRaisedSomethingElse = true;
            }
        });

        ASSERT_TRUE(shared->receiveEntered.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "the receive worker never reached the handoff; this is a harness failure";

        // The receive side's acquisition, and the sequencing proof in one observation: it has
        // passed the state guard - the driver is still OPENED, nothing has closed yet - and is
        // therefore parked on queueProducerMutex with nothing else left to do.
        ASSERT_FALSE(receiver->finishedWithin(PRODUCER_LOCK_OBSERVATION_MS))
            << "the receive handoff COMPLETED while this test held queueProducerMutex, so "
               "offerReceivedFrame() did not take that lock around its occupancy check and its "
               "offer. Without it the check can go stale under the other producer and the "
               "frame is dropped while acceptance is still reported";

        closer = &harness.start([shared]() {
            shared->closeEntered.notify();

            try {
                shared->probe.close();
                shared->closeReturnedCleanly = true;
            }
            catch (IOException &) {
                // Expected: no service proxy, so the close transaction reports DEAD_OBJECT.
                // The sentinel is offered before it, which is the part this case reads.
                shared->closeRaisedIoException = true;
            }
            catch (...) {
                // Any other exception is reported through the queue-content assertions below
                // rather than swallowed silently: a close that did not reach its offer leaves
                // no sentinel, and the sentinel count is asserted.
                shared->closeRaisedSomethingElse = true;
            }
        });

        ASSERT_TRUE(shared->closeEntered.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "the close worker never reached close(); this is a harness failure";

        // The close side's acquisition - the same observation the previous case makes on its
        // own, repeated here because this case must have both producers parked before it lets
        // them race.
        ASSERT_FALSE(closer->finishedWithin(PRODUCER_LOCK_OBSERVATION_MS))
            << "close() ran to completion while this test held queueProducerMutex, so its "
               "sentinel offer is not serialized against the receive path. That is finding "
               "the missing close-side acquisition";

        EXPECT_EQ(shared->probe.occupancy(), capacity - 2)
            << "the occupancy moved while this test held the producer lock, so one of the two "
               "parked producers reached the queue without taking it";
    }

    // Released: the two producers now contend for real, in an order this case does not choose.
    ASSERT_TRUE(receiver->finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the receive handoff did not complete within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms of the producer lock being released";

    ASSERT_TRUE(closer->finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "close() did not complete within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms of the producer lock being released";

    ASSERT_TRUE(harness.disposeWorkers())
        << "a worker did not finish within " << WORKER_ABANDON_DEADLINE_MS
        << " ms of its body being observed complete, so it was abandoned rather than joined "
           "and the state it holds is leaked by design. Nothing below can be interpreted";

    // Everything below reads worker-written state after both joins, which is the happens-before
    // that makes plain bools sufficient here.
    ASSERT_FALSE(shared->contendingRaisedSomethingElse)
        << "the receive handoff raised an exception that is neither InvalidStateException nor "
           "nothing at all, so what follows cannot be interpreted";

    EXPECT_FALSE(shared->contendingRefusedByStateGuard)
        << "the receive handoff was refused by the OPENED-state guard, so it never reached the "
           "producer lock and no contention took place. The sequencing above exists to prevent "
           "exactly this: the handoff is required to be parked on the lock while the state is "
           "still OPENED, before close() is started at all";

    if (shared->contendingRefusedByStateGuard) {
        ownedByCaller.push_back(shared->contending);
    }
    else if (!shared->contendingAccepted) {
        // The legitimate close-first order: the sentinel took the queue to capacity - 1 and the
        // handoff then refused, so the caller still owns this frame and must release it.
        ownedByCaller.push_back(shared->contending);
    }

    const size_t expectedFrames = (capacity - 2) + (shared->contendingAccepted ? 1u : 0u);

    // What the queue actually holds, by identity rather than by count alone.
    std::vector<CECFrame *> takenFromQueue;
    size_t sentinels = 0;

    shared->probe.drainInto(takenFromQueue, sentinels);

    // The invariant that holds in both orders: the sentinel is never swallowed. A sentinel
    // offered onto a queue the receive path has filled to capacity is discarded silently, and
    // the Bus reader then stays blocked in poll() with nothing left to wake it.
    EXPECT_EQ(sentinels, 1u)
        << "the queue yielded " << sentinels << " NULL sentinels after a close that overlapped "
           "a receive handoff. close() posts exactly one, and the slot the receive path "
           "reserves is what guarantees it is never the entry EventQueue::offer() drops";

    EXPECT_EQ(takenFromQueue.size(), expectedFrames)
        << "the queue yielded " << takenFromQueue.size() << " frames where the handoff's "
           "reports account for " << expectedFrames << " ("
        << (capacity - 2) << " filled, plus the shared->contending frame "
        << (shared->contendingAccepted ? "which was accepted" : "which was refused")
        << "). A shortfall is the ownership leak itself: acceptance reported for a frame "
           "EventQueue::offer() had discarded";

    // The shared->contending frame specifically: present in the queue if and only if the handoff said
    // the queue had taken it. This is the one frame whose ownership the race decided.
    size_t contendingInQueue = 0;

    for (size_t q = 0; q < takenFromQueue.size(); q++) {
        if (takenFromQueue[q] == shared->contending) {
            contendingInQueue++;
        }
    }

    EXPECT_EQ(contendingInQueue, shared->contendingAccepted ? 1u : 0u)
        << "the handoff reported the shared->contending frame "
        << (shared->contendingAccepted ? "accepted" : "refused") << " and the queue held it "
        << contendingInQueue << " times. The report and the queue must agree: reporting "
           "acceptance for a frame that is not there leaks it, and reporting refusal for one "
           "that is would have the caller free a frame the queue still owns";

    // The partition, over every allocation: exactly one owner each, counted so that "none" and
    // "two" are distinguished from each other and from the expected answer.
    for (size_t i = 0; i < shared->allocated.size(); i++) {
        size_t owners = 0;

        for (size_t q = 0; q < takenFromQueue.size(); q++) {
            if (takenFromQueue[q] == shared->allocated[i]) {
                owners++;
            }
        }

        for (size_t c = 0; c < ownedByCaller.size(); c++) {
            if (ownedByCaller[c] == shared->allocated[i]) {
                owners++;
            }
        }

        EXPECT_EQ(owners, 1u)
            << "frame " << i << " of " << shared->allocated.size() << " has " << owners << " owners "
               "after the overlap. Zero means the handoff reported it accepted and the queue "
               "never took it, so nothing will ever release it - the ownership leak. Two means "
               "both "
               "the queue and the caller would release it";
    }

    // Nothing is released here. Every frame this case allocated is registered in the harness's
    // ledger and released by its destructor, after the queue has been drained and after every
    // worker has been disposed - which is the only order in which "exactly once" holds on every
    // exit path, including an early return from a failed assertion above.
}

// The invariant rather than the timing: across many independent real overlaps of
// production close()'s sentinel offer against a production receive handoff, the handoff's
// report and the queue's contents never disagree, exactly one sentinel is ever queued, and
// every frame allocated has exactly one owner.
//
// Why this case exists when two concurrent cases already do, and it is a difference of kind
// rather than of degree. Those two prove the missing acquisition by timing: they hold
// queueProducerMutex and require the production producer not to complete while they hold it.
// That works, and it is how the defect was first caught, but the proof has a hole a scheduler
// can walk through. Each of them signals its "entered" latch before it calls into production,
// so the latch establishes that the worker started - not that it reached the lock acquisition
// or the check-and-offer region. A worker descheduled immediately after signalling satisfies
// the bounded non-completion observation for a reason that has nothing to do with the lock,
// and once the test releases the lock the worker completes and every queue-content assertion
// still holds. The counterexample is unlikely, not impossible, and a regression test whose
// proof depends on the scheduler not doing something is a regression test with a hole in it.
//
// So this case proves the property instead of the timing, and asserts exactly what the
// ownership contract is about. The illegal state the missing lock produces is "the handoff
// reported an outcome the queue did not honour" - acceptance reported for a frame
// EventQueue::offer() had silently discarded, leaving no owner at all; and, symmetrically,
// refusal reported for a
// frame the queue did take, which would have the caller free a frame the queue still holds.
// Both are violations of one invariant:
//
//   Reported-accepted if and only if the frame is in the queue
//
// which the fix makes unconditional and the defect makes violable. Two more are asserted
// alongside it because they are the same fix seen from other angles: exactly one sentinel is
// queued, and the frames the queue yields and the frames the handoff handed back partition the
// allocations - every frame with exactly one owner, never two and never none.
//
// How a real overlap is staged without leaning on the thing under test. Each iteration fills
// the queue to capacity - 2 through the production handoff, so that both orders are expressible
// and neither is a no-op, then releases two workers from a rendezvous of this file's own: the
// second producer to arrive releases the pair (awaitOverlapRendezvous), so the pair leaves the
// gate within tens of nanoseconds of each other. Deliberately not the production lock, which
// the two timing cases use and which releases nothing when close() stops taking it - the very
// mutation this case must catch. Deliberately not the test thread either, which would have to
// be scheduled between two arrivals on a host whose runnable threads are the producers.
//
// And the alignment is swept rather than sampled once. burnOffset() is applied after the
// release, alternating which producer carries it and varying its size between iterations, so
// the 256 attempts walk the relative arrival across the window instead of retrying one fixed
// schedule 256 times.
//
// WHAT A PASSING SWEEP GUARANTEES AND WHAT IT DOES NOT, AND WHY THIS CASE NO LONGER ASSERTS
// ANYTHING ABOUT THE SCHEDULER. The sweep guarantees that in every overlap that occurred, the
// invariant held. It cannot guarantee that any particular interleaving occurred - no test can,
// without controlling the scheduler - so a run in which the two producers never actually
// contended would satisfy the invariants vacuously, and something has to make that visible.
//
// An earlier revision made it visible by ASSERTING the census: both outcome orders had to have
// been observed at least once across the 256 attempts. With two CPUs available every run
// produces tens of each, so the assertion looked settled. It is not. Pinned to ONE CPU
// (`taskset -c 0`) the sweep produces 256 close-first-refused-by-the-state-guard outcomes and
// nothing else - measured, repeatedly: the close worker runs to completion before the receive
// worker is scheduled at all - and the case then reports a RED suite on a tree with no product
// regression whatsoever. A single-CPU runner, a busy shared CI executor and a container with a
// one-CPU quota are all valid environments, so an assertion on the census makes the verdict a
// property of the machine rather than of the code. THAT IS THE HOLE THIS REVISION CLOSES, and
// it is the whole reason the orderings below are CONSTRUCTED and not sampled.
//
// So the non-vacuity requirement is discharged where it can be discharged deterministically.
// PART 1 builds each intended ordering directly and single-threaded, through the seams
// ReceiveQueueProbe already exposes - postCloseSentinel() for a sentinel that is already queued
// while the instance is still OPENED, and the real production close() for one that also takes
// the instance out of OPENED - and asserts the SAME three invariants on each. Every arm
// therefore runs on every scheduler and on any number of CPUs, and it is the arms rather than
// the census that establish that the accepted-and-present and refused-and-absent arms of the
// invariant were exercised at all. PART 2 then keeps the contended sweep exactly as it was,
// invariants included, because a real overlap is evidence no constructed ordering can supply:
// it is the only thing in this file that puts the two production producers on the same queue at
// the same instant. Its census is PRINTED and no longer ASSERTED - it is a report of what this
// machine happened to sample, which is all it ever was.
//
// A LATER READER WILL BE TEMPTED TO SIMPLIFY PART 1 BACK OUT, on the grounds that the sweep
// reaches the same three outcomes anyway. It does not reach them reliably: which of the three it
// produces is the scheduler's choice, and on one CPU it produces exactly one of them, 256 times.
// Deleting Part 1 and restoring an assertion on the census restores the false failure.
//
// Measured both ways before it was committed. With `{AutoLock lock_(queueProducerMutex);`
// deleted from DriverAidlImpl::close() this case fails on the invariant - the handoff reporting
// a refusal for a frame the drained queue is holding, and the same frame counted with two
// owners - not on a timeout. With the acquisition restored it passes, repeatedly.
//
// Every allocation is accounted for on every path, including a failing one: each frame is
// registered in the harness's ledger the moment it is allocated, and the harness releases it
// exactly once after its workers are disposed and its queue drained. Nothing here deletes a
// frame itself, and nothing leaks if an assertion stops the loop early.
/**
 * @brief Every intended ordering of close()'s sentinel against the receive handoff is
 *        constructed and asserted, and a contended sweep then holds the same invariants over
 *        whatever real overlaps this machine produces.
 * @pre Runs under every invocation, through the harness. This is the case that carries the
 *      weight, because it reasons about no timing at all and fails on a violated invariant.
 * @note Part 1 CONSTRUCTS the three orderings - receive-first accepted, close-first refused by
 *       the reserved slot, close-first refused by the state guard - single-threaded and through
 *       the probe's own seams, so each is exercised on every scheduler and on any number of
 *       CPUs. The non-vacuity requirement is asserted on these arms, and on nothing else.
 * @note Part 2 sweeps real overlaps. The alignment is varied rather than sampled once: a burn
 *       offset is applied after the release, alternating which producer carries it and varying
 *       its size, so the attempts walk the relative arrival across the window instead of
 *       retrying one fixed schedule. Its invariants are asserted; its census is printed and
 *       deliberately NOT asserted, because which interleaving a machine produces is the
 *       scheduler's choice - pinned to one CPU this sweep produces one outcome 256 times, and
 *       an assertion on the census turned that into a red suite on a tree with no regression.
 * @note Every allocation is accounted for on every path including a failing one: each frame
 *       is registered in the harness ledger as it is allocated and released exactly once
 *       after the workers are disposed and the queue drained, so nothing leaks if an
 *       assertion stops the loop early.
 */
TEST_F(DriverAidlLocalInstanceTest, ManyRealOverlapsKeepTheHandoffReportAndTheQueueInAgreement) {
    const size_t capacity = DriverAidlImpl::INCOMING_QUEUE_CAPACITY;
    ASSERT_GE(capacity, 3u)
        << "this case needs room for a filled queue, one contending frame and close()'s "
           "sentinel, so a capacity below three cannot express it";

    /* -------------------------------------------------------------------------------------
     * PART 1 - THE THREE ORDERINGS, CONSTRUCTED RATHER THAN SAMPLED.
     *
     * Each arm below produces one of the three outcomes by construction, single-threaded, and
     * asserts the same three invariants Part 2 asserts on its sampled overlaps. No thread, no
     * rendezvous and no latch is involved, so no arm can be lost to a scheduler that runs one
     * runnable thread to completion before it runs the other - which is exactly what a
     * one-CPU host does, and which is why the sampled census is no longer asserted. The block
     * comment above this case records that measurement and why this part must not be removed.
     * ----------------------------------------------------------------------------------- */

    // Which of the three orderings a constructed arm is to produce. The three differ by
    // exactly one thing - where the close side runs relative to the handoff - so naming that
    // difference keeps them one shape with one varying step rather than three near-copies.
    enum ConstructedOrdering {
        RECEIVE_FIRST_ACCEPTS,        // the handoff reaches the queue below the refusal point
        CLOSE_FIRST_REFUSED_BY_SLOT,  // the sentinel is already queued, so the handoff refuses
        CLOSE_FIRST_REFUSED_BY_STATE  // close() has left OPENED, so the handoff raises
    };

    // The production close(), run into the same three outcome flags the sweep's close worker
    // writes, so both parts of this case read the close side through one vocabulary. A local
    // instance holds no service proxy, so the transaction fails and close() raises IOException
    // AFTER it has offered its sentinel - which is what lets an arm read the sentinel's
    // arrival from a close() that raised.
    auto driveProductionClose = [](QueueHandoffOverlapState *shared) {
        try {
            shared->probe.close();
            shared->closeReturnedCleanly = true;
        }
        catch (IOException &) {
            shared->closeRaisedIoException = true;
        }
        catch (...) {
            shared->closeRaisedSomethingElse = true;
        }
    };

    // Builds one ordering, asserts it produced that ordering, and asserts the three invariants
    // on it. EXPECT rather than ASSERT throughout, deliberately: a fatal assertion inside a
    // lambda returns from the lambda alone, so it would leave the arm's own report unmade and
    // read as a silently skipped arm rather than as a failure.
    //
    // Returns whether the arm really produced the ordering it names, which is the arm's
    // contribution to the non-vacuity assertion at the end of this case.
    auto constructOrdering = [&](ConstructedOrdering ordering, const char *arm) -> bool {
        // The same harness Part 2 uses, for the same reasons: it owns the probe and the frame
        // ledger off this stack and releases every allocation exactly once on every exit path,
        // including an early return from a failed expectation. No worker is started here, so
        // its bounded disposition has nothing to dispose.
        QueueHandoffOverlapHarness harness;
        QueueHandoffOverlapState  *shared = &*harness;

        shared->probe.markOpened();

        // The frames the handoff handed back, by identity. Pointers only - the ledger owns them.
        std::vector<CECFrame *> ownedByCaller;

        // Filled to capacity - 2 through the production handoff, exactly as Part 2 fills it, so
        // that both the accepted and the refused outcome stay expressible and neither is a
        // no-op. This fill is also what drives the slot-available side of the reserved-slot
        // check, capacity - 2 times over on every arm.
        for (size_t filled = 0; filled < capacity - 2; filled++) {
            CECFrame *frame = new CECFrame(directedFrame());
            shared->allocated.push_back(frame);

            if (!shared->probe.offer(frame)) {
                ownedByCaller.push_back(frame);
            }
        }

        EXPECT_TRUE(ownedByCaller.empty())
            << arm << ": the handoff refused " << ownedByCaller.size() << " frames below its "
               "refusal point, so this arm is not the ordering it names";

        EXPECT_EQ(shared->probe.occupancy(), capacity - 2)
            << arm << ": the queue holds " << shared->probe.occupancy() << " entries where this "
               "arm needs " << (capacity - 2);

        shared->contending = new CECFrame(directedFrame());
        shared->allocated.push_back(shared->contending);

        // THE CLOSE SIDE, AHEAD OF THE HANDOFF ON TWO OF THE THREE ARMS. This is the whole of
        // the difference between the arms, and each choice models a real close rather than an
        // invented state.
        if (ordering == CLOSE_FIRST_REFUSED_BY_SLOT) {
            // close()'s sentinel, offered under queueProducerMutex exactly as close() offers
            // it, reaching the queue BEFORE the handoff does. The instance is deliberately
            // left OPENED, because the guard runs ahead of the occupancy check: were the state
            // moved as well, the handoff below would raise from the guard and this arm would
            // become a second copy of the state-guard arm instead of the occupancy one. This
            // is the state a close leaves for the window between its sentinel offer and its
            // own state store, and postCloseSentinel() is the seam that expresses it.
            shared->probe.postCloseSentinel();
        }
        else if (ordering == CLOSE_FIRST_REFUSED_BY_STATE) {
            // The real production close(), run to completion first: it sets CLOSING, offers
            // its sentinel under the producer lock, and reports the failed transaction of a
            // proxyless instance. The handoff below then meets an instance that is no longer
            // OPENED, which is the earliest form of the close-first order.
            driveProductionClose(shared);
        }

        // THE PRODUCTION HANDOFF, reported exactly as it reported it. Nothing is
        // reinterpreted: the agreement between this report and what the queue really holds is
        // the property under test.
        try {
            shared->contendingAccepted = shared->probe.offer(shared->contending);
        }
        catch (InvalidStateException &) {
            shared->contendingRefusedByStateGuard = true;
        }
        catch (...) {
            shared->contendingRaisedSomethingElse = true;
        }

        if (ordering == RECEIVE_FIRST_ACCEPTS) {
            // The close side runs AFTER the accepted handoff on this arm, so its sentinel lands
            // in the slot the handoff reserved for it - the property that makes the sentinel
            // undroppable, and the one a full queue would break.
            driveProductionClose(shared);
        }

        EXPECT_FALSE(shared->contendingRaisedSomethingElse)
            << arm << ": the receive handoff raised an exception that is neither "
               "InvalidStateException nor nothing at all, so what follows cannot be interpreted";

        // The close side's own outcome, on the two arms that drive production close(). The
        // occupancy arm drives no close at all - its sentinel comes from postCloseSentinel()
        // for the reason given above - so it has no close outcome to read.
        if (ordering != CLOSE_FIRST_REFUSED_BY_SLOT) {
            EXPECT_TRUE(shared->closeRaisedIoException)
                << arm << ": close() did not report the IOException a proxyless instance must "
                   "report, so the sentinel this arm reads may not have come from the code path "
                   "this arm means to exercise";

            EXPECT_FALSE(shared->closeReturnedCleanly)
                << arm << ": close() returned cleanly on an instance holding no service proxy, "
                   "so the assertions below are describing a different code path";

            EXPECT_FALSE(shared->closeRaisedSomethingElse)
                << arm << ": close() raised an exception that is not the IOException a proxyless "
                   "instance must report, so what follows cannot be interpreted";
        }

        // The decisive outcome, asserted per arm. This is what makes each arm the ordering it
        // claims to be rather than whichever ordering the code happened to take, and it is
        // also the arm's own non-vacuity evidence.
        bool producedTheOrdering = false;

        switch (ordering) {
        case RECEIVE_FIRST_ACCEPTS:
            EXPECT_TRUE(shared->contendingAccepted)
                << arm << ": the handoff refused a frame offered onto a queue holding "
                << (capacity - 2) << " of " << capacity << " entries, which is below the "
                   "reserved-slot refusal point, so the accepted-and-present arm of the "
                   "invariant was not reached";

            EXPECT_FALSE(shared->contendingRefusedByStateGuard)
                << arm << ": the handoff was refused by the OPENED-state guard on an arm that "
                   "closes nothing until after the handoff has returned";

            producedTheOrdering =
                shared->contendingAccepted && !shared->contendingRefusedByStateGuard;
            break;

        case CLOSE_FIRST_REFUSED_BY_SLOT:
            EXPECT_FALSE(shared->contendingAccepted)
                << arm << ": the handoff accepted a frame offered onto a queue already holding "
                << (capacity - 1) << " of " << capacity << " entries. The last slot is reserved "
                   "for close()'s sentinel, and accepting here is what makes a sentinel "
                   "droppable";

            EXPECT_FALSE(shared->contendingRefusedByStateGuard)
                << arm << ": the handoff was refused by the OPENED-state guard rather than by "
                   "occupancy, so this arm did not exercise the reserved-slot refusal it exists "
                   "for. The instance is left OPENED precisely so that it cannot";

            producedTheOrdering =
                !shared->contendingAccepted && !shared->contendingRefusedByStateGuard;
            break;

        case CLOSE_FIRST_REFUSED_BY_STATE:
            EXPECT_TRUE(shared->contendingRefusedByStateGuard)
                << arm << ": the handoff did not raise InvalidStateException after a completed "
                   "close(), so the state guard that keeps a frame out of a queue nobody will "
                   "drain again was not exercised";

            EXPECT_FALSE(shared->contendingAccepted)
                << arm << ": the handoff accepted a frame on an instance that is no longer "
                   "OPENED, so the frame would sit in a queue the Bus reader has already been "
                   "told to stop draining";

            producedTheOrdering =
                shared->contendingRefusedByStateGuard && !shared->contendingAccepted;
            break;
        }

        // What the queue really holds, by identity. Draining here also leaves the harness's
        // ownership sweep nothing to drain, which is what keeps each frame released once.
        std::vector<CECFrame *> takenFromQueue;
        size_t                  sentinels = 0;

        shared->probe.drainInto(takenFromQueue, sentinels);

        if (!shared->contendingAccepted) {
            ownedByCaller.push_back(shared->contending);
        }

        // Invariant 1, as Part 2 asserts it: exactly one sentinel, whatever the order was.
        EXPECT_EQ(sentinels, 1u)
            << arm << ": the queue yielded " << sentinels << " NULL sentinels; exactly one is "
               "offered and it must not be droppable - a swallowed sentinel leaves the Bus "
               "reader blocked in poll() with nothing left to wake it";

        // Invariant 2, as Part 2 asserts it: reported-accepted if and only if present.
        size_t contendingInQueue = 0;

        for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
            if (takenFromQueue[entry] == shared->contending) {
                contendingInQueue++;
            }
        }

        EXPECT_EQ(contendingInQueue, shared->contendingAccepted ? 1u : 0u)
            << arm << ": the handoff reported the contending frame "
            << (shared->contendingAccepted ? "ACCEPTED" : "REFUSED") << " and the drained queue "
               "held it " << contendingInQueue << " times. The report and the queue must agree: "
               "acceptance reported for a frame that is not there leaves NOBODY to release it, "
               "and refusal reported for one that is there has the caller free a frame the queue "
               "still owns";

        const size_t expectedFrames = (capacity - 2) + (shared->contendingAccepted ? 1u : 0u);

        EXPECT_EQ(takenFromQueue.size(), expectedFrames)
            << arm << ": the queue yielded " << takenFromQueue.size() << " frames where the "
               "handoff's reports account for " << expectedFrames;

        // Invariant 3, as Part 2 asserts it: the partition over every allocation, so that "no
        // owner" and "two owners" are distinguished from each other and from the right answer.
        size_t frameWithoutAnOwner = 0;
        size_t frameWithTwoOwners  = 0;

        for (size_t allocation = 0; allocation < shared->allocated.size(); allocation++) {
            size_t owners = 0;

            for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
                if (takenFromQueue[entry] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            for (size_t kept = 0; kept < ownedByCaller.size(); kept++) {
                if (ownedByCaller[kept] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            if (owners == 0) {
                frameWithoutAnOwner++;
            }
            else if (owners > 1) {
                frameWithTwoOwners++;
            }
        }

        EXPECT_EQ(frameWithoutAnOwner, 0u)
            << arm << ": " << frameWithoutAnOwner << " frames have NO owner - reported accepted "
               "and not in the queue, so nothing in the process will ever release them";

        EXPECT_EQ(frameWithTwoOwners, 0u)
            << arm << ": " << frameWithTwoOwners << " frames have TWO owners - both the queue "
               "and the caller believe they hold them, which is a double free rather than a leak";

        return producedTheOrdering;
    };

    // The three arms, each run exactly once, in the order that reads as the story: the handoff
    // wins; the sentinel wins on occupancy; the sentinel wins on the state. Every one of them
    // runs on every scheduler, which is the property the sampled census cannot offer.
    const bool receiveFirstConstructed =
        constructOrdering(RECEIVE_FIRST_ACCEPTS, "[constructed receive-first]");
    const bool refusedBySlotConstructed =
        constructOrdering(CLOSE_FIRST_REFUSED_BY_SLOT, "[constructed close-first, occupancy]");
    const bool refusedByStateConstructed =
        constructOrdering(CLOSE_FIRST_REFUSED_BY_STATE, "[constructed close-first, state guard]");

    // The deterministic counterpart of the census below, and the reason it is printed rather
    // than left implicit: a reader of any run - on any number of CPUs - can see that all three
    // orderings were reached, which is precisely what the sampled census cannot show. The
    // assertions at the end of this case are made on these three outcomes.
    std::cout << "[queue handoff constructed orderings] receive-first accepted: "
              << (receiveFirstConstructed ? "yes" : "NO")
              << ", close-first refused by occupancy: "
              << (refusedBySlotConstructed ? "yes" : "NO")
              << ", close-first refused by the state guard: "
              << (refusedByStateConstructed ? "yes" : "NO") << std::endl;

    /* -------------------------------------------------------------------------------------
     * PART 2 - THE CONTENDED SWEEP, UNCHANGED.
     *
     * Real overlaps of the two production producers on one queue, which no constructed
     * ordering can supply. Its invariants are asserted below exactly as before; its census is
     * printed and NOT asserted, for the reason the block comment above this case records.
     * ----------------------------------------------------------------------------------- */

    // The interleaving census. PRINTED, NEVER ASSERTED: which interleaving a machine produces
    // is the scheduler's choice, and asserting it made a one-CPU host report a false failure.
    // It stays because a reader of a passing run should be able to see what was sampled.
    size_t overlapsCompleted        = 0;
    size_t receiveFirstOverlaps     = 0;   // the handoff got in below the refusal point
    size_t closeFirstByOccupancy    = 0;   // the sentinel landed first, so the handoff refused
    size_t closeFirstByStateGuard   = 0;   // close() reached CLOSING before the handoff's guard

    for (size_t iteration = 0; iteration < OVERLAP_STRESS_ITERATIONS; iteration++) {
        // A fresh instance per iteration, so no iteration inherits another's queue, state or
        // rendezvous. The harness holds it off this stack and releases everything itself.
        QueueHandoffOverlapHarness harness;
        QueueHandoffOverlapState  *shared = &*harness;

        shared->probe.markOpened();

        // The frames the handoff handed back, by identity. Pointers only - the ledger owns them.
        std::vector<CECFrame *> ownedByCaller;

        // Fill to capacity - 2 through the production handoff, leaving one slot the contending
        // frame may take and one the sentinel is reserved.
        for (size_t filled = 0; filled < capacity - 2; filled++) {
            CECFrame *frame = new CECFrame(directedFrame());
            shared->allocated.push_back(frame);

            if (!shared->probe.offer(frame)) {
                ownedByCaller.push_back(frame);
            }
        }

        EXPECT_TRUE(ownedByCaller.empty())
            << "iteration " << iteration << ": the handoff refused " << ownedByCaller.size()
            << " frames below its refusal point, so this iteration's overlap would not be the "
               "one the case describes";

        EXPECT_EQ(shared->probe.occupancy(), capacity - 2)
            << "iteration " << iteration << ": the queue holds " << shared->probe.occupancy()
            << " entries where the overlap needs " << (capacity - 2);

        shared->contending = new CECFrame(directedFrame());
        shared->allocated.push_back(shared->contending);

        // The swept offset, alternating sides so both directions of skew are sampled.
        const unsigned int offset =
            static_cast<unsigned int>((iteration * 37u) % OVERLAP_OFFSET_SPREAD);
        const unsigned int receiveOffset = ((iteration % 2) == 0) ? 0u : offset;
        const unsigned int closeOffset   = ((iteration % 2) == 0) ? offset : 0u;

        BoundedWorker &receiver = harness.start([shared, receiveOffset]() {
            shared->receiveEntered.notify();

            if (!awaitOverlapRendezvous(shared)) {
                shared->rendezvousTimedOut.store(true, std::memory_order_release);
                return;
            }

            burnOffset(receiveOffset);

            try {
                shared->contendingAccepted = shared->probe.offer(shared->contending);
            }
            catch (InvalidStateException &) {
                // The legitimate close-first order in its earliest form: close() had already
                // moved the state out of OPENED when this thread reached getIncomingQueue()'s
                // guard. The frame stays the caller's, which the ownership ledger below records.
                shared->contendingRefusedByStateGuard = true;
            }
            catch (...) {
                shared->contendingRaisedSomethingElse = true;
            }
        });

        BoundedWorker &closer = harness.start([shared, closeOffset]() {
            shared->closeEntered.notify();

            if (!awaitOverlapRendezvous(shared)) {
                shared->rendezvousTimedOut.store(true, std::memory_order_release);
                return;
            }

            burnOffset(closeOffset);

            try {
                shared->probe.close();
                shared->closeReturnedCleanly = true;
            }
            catch (IOException &) {
                // Expected: a local instance holds no service proxy, so the close transaction
                // reports DEAD_OBJECT. The sentinel is offered before that transaction, which is
                // why this case can read the sentinel's arrival from a close() that raises.
                shared->closeRaisedIoException = true;
            }
            catch (...) {
                shared->closeRaisedSomethingElse = true;
            }
        });

        // Bounded completion, then a bounded disposition. Both are needed: the first says the
        // producers finished, the second says they were joined rather than abandoned - and only
        // a join is the happens-before that makes the plain flags below safe to read.
        EXPECT_TRUE(receiver.finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "iteration " << iteration << ": the receive handoff did not complete within "
            << PRODUCER_COMPLETION_TIMEOUT_MS << " ms. Nothing is holding the producer lock in "
               "this case, so it is stuck rather than waiting";

        EXPECT_TRUE(closer.finishedWithin(PRODUCER_COMPLETION_TIMEOUT_MS))
            << "iteration " << iteration << ": close() did not complete within "
            << PRODUCER_COMPLETION_TIMEOUT_MS << " ms. Nothing is holding the producer lock in "
               "this case, so it is stuck rather than waiting";

        if (!harness.disposeWorkers()) {
            // A worker was abandoned, so its state is leaked by design and nothing it wrote can
            // be interpreted. Reported and the loop stopped, rather than read anyway.
            ADD_FAILURE()
                << "iteration " << iteration << ": a producer did not finish within "
                << WORKER_ABANDON_DEADLINE_MS << " ms and had to be abandoned rather than "
                   "joined, so this iteration's state is unreadable and the loop is stopped";
            break;
        }

        ASSERT_FALSE(shared->rendezvousTimedOut.load(std::memory_order_acquire))
            << "iteration " << iteration << ": a producer waited " << OVERLAP_RENDEZVOUS_BOUND_MS
            << " ms at the rendezvous and its partner never arrived, so no overlap took place. "
               "This is a harness failure rather than a production one";

        ASSERT_FALSE(shared->contendingRaisedSomethingElse)
            << "iteration " << iteration << ": the receive handoff raised an exception that is "
               "neither InvalidStateException nor nothing at all, so what follows cannot be "
               "interpreted";

        ASSERT_FALSE(shared->closeRaisedSomethingElse)
            << "iteration " << iteration << ": close() raised an exception that is not the "
               "IOException a proxyless instance must report, so what follows cannot be "
               "interpreted";

        EXPECT_FALSE(shared->closeReturnedCleanly)
            << "iteration " << iteration << ": close() returned cleanly on an instance holding "
               "no service proxy, so the assertions below are describing a different code path";

        // What the queue really holds, by identity. Draining here also leaves the harness's
        // ownership sweep nothing to drain, which is what keeps each frame released once.
        std::vector<CECFrame *> takenFromQueue;
        size_t                  sentinels = 0;

        shared->probe.drainInto(takenFromQueue, sentinels);

        if (!shared->contendingAccepted) {
            ownedByCaller.push_back(shared->contending);
        }

        // Invariant 1: exactly one sentinel, whatever the order was. close() posts one, and the
        // slot the receive path reserves is what guarantees it is never the entry
        // EventQueue::offer() drops - a swallowed sentinel leaves the Bus reader blocked in
        // poll() with nothing left to wake it.
        EXPECT_EQ(sentinels, 1u)
            << "iteration " << iteration << ": the queue yielded " << sentinels
            << " NULL sentinels after an overlap; close() posts exactly one and it must not be "
               "droppable";

        // Invariant 2: reported-accepted if and only if present. This is the one the missing
        // lock violates, in either direction, and it is the reason this case exists.
        size_t contendingInQueue = 0;

        for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
            if (takenFromQueue[entry] == shared->contending) {
                contendingInQueue++;
            }
        }

        EXPECT_EQ(contendingInQueue, shared->contendingAccepted ? 1u : 0u)
            << "iteration " << iteration << ": the handoff reported the contending frame "
            << (shared->contendingAccepted ? "ACCEPTED" : "REFUSED") << " and the drained queue "
               "held it " << contendingInQueue << " times. The report and the queue must agree: "
               "acceptance reported for a frame that is not there leaves NOBODY to release it - "
               "the ownership leak - and refusal reported for one that is there has the caller "
               "free a frame the queue still owns. An unserialized close() sentinel landing "
               "between offerReceivedFrame()'s occupancy check and its read-back produces "
               "exactly this disagreement";

        // Invariant 3: the partition, over every allocation, so "no owner" and "two owners" are
        // distinguished from each other and from the expected answer.
        size_t frameWithoutAnOwner = 0;
        size_t frameWithTwoOwners  = 0;

        for (size_t allocation = 0; allocation < shared->allocated.size(); allocation++) {
            size_t owners = 0;

            for (size_t entry = 0; entry < takenFromQueue.size(); entry++) {
                if (takenFromQueue[entry] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            for (size_t kept = 0; kept < ownedByCaller.size(); kept++) {
                if (ownedByCaller[kept] == shared->allocated[allocation]) {
                    owners++;
                }
            }

            if (owners == 0) {
                frameWithoutAnOwner++;
            }
            else if (owners > 1) {
                frameWithTwoOwners++;
            }
        }

        EXPECT_EQ(frameWithoutAnOwner, 0u)
            << "iteration " << iteration << ": " << frameWithoutAnOwner << " frames have NO "
               "owner - the handoff reported them accepted and the queue does not hold them, so "
               "nothing in the process will ever release them. That is the ownership leak, one "
               "frame per event on a path a remote HAL drives";

        EXPECT_EQ(frameWithTwoOwners, 0u)
            << "iteration " << iteration << ": " << frameWithTwoOwners << " frames have TWO "
               "owners - both the queue and the caller believe they hold them, which is a double "
               "free rather than a leak";

        overlapsCompleted++;

        if (shared->contendingRefusedByStateGuard) {
            closeFirstByStateGuard++;
        }
        else if (shared->contendingAccepted) {
            receiveFirstOverlaps++;
        }
        else {
            closeFirstByOccupancy++;
        }

        // One clear failure rather than 256 copies of it: the first violated invariant stops the
        // loop, and the harness releases this iteration's frames on the way out.
        if (HasFailure()) {
            break;
        }
    }

    // The census, printed rather than inferred, so a reader of a passing run can see which
    // interleavings it actually sampled. INFORMATIONAL: nothing below asserts these figures,
    // and a distribution of 0 / 0 / 256 - which is what one CPU produces - is a fact about the
    // machine rather than a defect. The three constructed arms above are what guarantee each
    // ordering was exercised.
    std::cout << "[queue handoff overlap stress] " << overlapsCompleted << " of "
              << OVERLAP_STRESS_ITERATIONS << " overlaps completed: " << receiveFirstOverlaps
              << " receive-first, " << closeFirstByOccupancy << " close-first refused by "
                 "occupancy, " << closeFirstByStateGuard << " close-first refused by the state "
                 "guard (informational; the constructed arms carry the non-vacuity requirement)"
              << std::endl;

    EXPECT_EQ(overlapsCompleted, OVERLAP_STRESS_ITERATIONS)
        << "only " << overlapsCompleted << " of " << OVERLAP_STRESS_ITERATIONS
        << " overlaps completed, so the loop stopped early on a failure reported above";

    // NON-VACUITY, ASSERTED ON THE CONSTRUCTED ARMS AND NOT ON THE SWEEP. Each arm reports
    // whether it really produced the ordering it names, so all three orderings are exercised on
    // every run, on any number of CPUs, and this case fails if an arm stops producing its
    // ordering - which is what an arm silently degenerating into a copy of another would look
    // like. The corresponding assertions on the sampled census were removed deliberately: they
    // made the verdict depend on how many CPUs the runner was given.
    EXPECT_TRUE(receiveFirstConstructed)
        << "the constructed receive-first arm did not produce an accepted handoff, so the "
           "accepted-and-present arm of the invariant was not exercised deterministically";

    EXPECT_TRUE(refusedBySlotConstructed)
        << "the constructed close-first-by-occupancy arm did not produce a refusal from the "
           "reserved-slot rule, so the refused-and-absent arm of the invariant was not exercised "
           "deterministically and the reserved slot is unproven on this run";

    EXPECT_TRUE(refusedByStateConstructed)
        << "the constructed close-first-by-state-guard arm did not produce an "
           "InvalidStateException, so the earliest form of the close-first order was not "
           "exercised deterministically";
}

// F5. TWO CLOSE SENTINELS IN THE RECEIVE QUEUE, WHICH IS THE STATE THAT USED TO CRASH THE BUS
// READER THREAD. read() polls the queue once with a null check, and then - on the arm where the
// instance is no longer OPENED - FLUSHES whatever remains. That flush used to dereference every
// pointer it took without checking, so a second sentinel was a null dereference on the one
// thread in the middleware whose death silently ends all CEC reception.
//
// HOW TWO SENTINELS ARISE, and it is ordinary rather than exotic: close() offers one on every
// transition out of OPENED, so a stop/reopen/close sequence, or a close overlapping a second
// close from another of the three call sites that reach Driver::close(), leaves two in the
// queue. The first is consumed by the checked poll and the second is met by the flush.
//
// HOW THE FLUSH ARM IS REACHED DETERMINISTICALLY, because that is the whole difficulty of this
// case. read() only flushes when its poll yields NULL *and* the state is no longer OPENED by the
// time it takes the instance lock; if the state is still OPENED it loops back and polls again,
// consuming the second sentinel through the CHECKED path instead and proving nothing. So the
// test thread holds the instance lock as a gate:
//   1. the instance is OPENED and the queue is empty, so the reader blocks inside poll();
//   2. the test thread takes the instance lock, moves the instance to CLOSING, and offers TWO
//      sentinels - the reader wakes, consumes the first, and then blocks on the lock;
//   3. the test thread releases the lock; the reader sees CLOSING and enters the flush with the
//      second sentinel still queued.
// Step 2 is what a real close does, in the order a real close does it.
//
// WHAT A FAILURE LOOKS LIKE: without the null check this case does not report a failed
// assertion, it CRASHES the runner - which is exactly the production consequence, and is why
// the queue-empty assertion below is the one that proves the flush actually ran.
TEST_F(DriverAidlLocalInstanceTest, TheReceiveFlushSurvivesASecondCloseSentinel) {
    ASSERT_NE(mock, nullptr);

    // Nothing on this path may reach the legacy HAL: the receive queue and its sentinels are
    // entirely middleware-side on both back-ends.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    ReceiveQueueProbe probe;
    MonotonicLatch    readerEnteredRead;
    MonotonicLatch    readerFinished;

    bool readerRaisedInvalidState = false;
    bool readerRaisedSomethingElse = false;
    bool readerReturnedCleanly = false;

    probe.markOpened();

    ASSERT_EQ(probe.occupancy(), 0u)
        << "the receive queue is not empty before the reader starts, so the reader would not "
           "block inside poll() and the gate below could not be established";

    std::thread reader([&]() {
        CECFrame received;

        readerEnteredRead.notify();

        try {
            probe.read(received);
            readerReturnedCleanly = true;
        }
        catch (InvalidStateException &) {
            readerRaisedInvalidState = true;
        }
        catch (...) {
            readerRaisedSomethingElse = true;
        }

        readerFinished.notify();
    });

    // The reader announces itself before it calls read(), and the margin below covers the
    // entry guard and the descent into poll(). It is generous on purpose: the consequence of
    // it being too short is not a wrong answer but a case that reports it did not reach the
    // flush, which the queue-empty assertion detects and names.
    ASSERT_TRUE(readerEnteredRead.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the reader thread never reached read(), so nothing below was exercised";

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    {
        // THE GATE. Held across the state change and both offers, exactly as close() holds it
        // across its own state change and sentinel offer.
        CCEC_OSAL::AutoLock gate(probe.instanceLock());

        probe.markClosing();

        probe.postCloseSentinel();
        probe.postCloseSentinel();
    }

    ASSERT_TRUE(readerFinished.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the reader did not finish within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms after the gate was released. It is blocked somewhere in read()";

    reader.join();

    EXPECT_TRUE(readerRaisedInvalidState)
        << "read() did not raise InvalidStateException on a driver that stopped being OPENED "
           "while the reader was blocked. That raise is how the Bus reader unwinds on close, "
           "and both back-ends must keep it";

    EXPECT_FALSE(readerReturnedCleanly)
        << "read() returned a frame during teardown, so the flush arm was not taken and this "
           "case exercised the poll-and-loop path instead";

    EXPECT_FALSE(readerRaisedSomethingElse)
        << "read() raised something other than InvalidStateException, so the flush handled the "
           "second sentinel by some path other than skipping it";

    // THE PROOF THAT THE FLUSH RAN AND DRAINED THE SECOND SENTINEL. Two sentinels were offered
    // and the checked poll consumed one, so an empty queue means the flush took the other and
    // survived it. An occupancy of one would mean the reader unwound before the flush - a
    // harness sequencing failure rather than a production one - and is reported as such.
    EXPECT_EQ(probe.occupancy(), 0u)
        << "the receive queue still holds " << probe.occupancy()
        << " entries. If it holds one, the reader raised before reaching the flush and this "
           "case did not exercise the second sentinel at all; if it holds more, the flush "
           "stopped early";
}

// THE PARITY ARM OF THE SAME NULL CHECK: the flush meets a REAL FRAME and must still copy and
// release it, exactly as it did before the check existed.
//
// WHY THIS CASE IS NEEDED BESIDE THE SENTINEL ONE. The null check added two paths where there
// was one, and the sentinel case only proves the new path. A check written the wrong way round -
// skipping frames and dereferencing sentinels - would pass that case and fail this one, and
// nothing else in the suite drains a frame through the flush: every other receive case is
// consumed by the reader's own checked poll, which is a different line.
//
// The frame is placed BEHIND the sentinel and while the gate is held, because that is the only
// arrangement in which the flush is the code that meets it. Offered before the close it would be
// taken by the reader's poll; offered after, offerReceivedFrame() would refuse it. Behind the
// sentinel it is what a frame the HAL delivered a moment before the close looks like, which is
// the real sequence this arm exists for.
TEST_F(DriverAidlLocalInstanceTest, TheReceiveFlushReleasesARealFrameQueuedBehindTheCloseSentinel) {
    ASSERT_NE(mock, nullptr);

    // Nothing on this path may reach the legacy HAL, for the same reason as the sentinel case.
    EXPECT_CALL(*mock, HdmiCecClose(_)).Times(0);

    ReceiveQueueProbe probe;
    MonotonicLatch    readerEnteredRead;
    MonotonicLatch    readerFinished;

    bool readerRaisedInvalidState = false;
    bool readerRaisedSomethingElse = false;
    bool readerReturnedCleanly = false;

    probe.markOpened();

    ASSERT_EQ(probe.occupancy(), 0u)
        << "the receive queue is not empty before the reader starts, so the reader would not "
           "block inside poll() and the gate below could not be established";

    std::thread reader([&]() {
        CECFrame received;

        readerEnteredRead.notify();

        try {
            probe.read(received);
            readerReturnedCleanly = true;
        }
        catch (InvalidStateException &) {
            readerRaisedInvalidState = true;
        }
        catch (...) {
            readerRaisedSomethingElse = true;
        }

        readerFinished.notify();
    });

    ASSERT_TRUE(readerEnteredRead.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the reader thread never reached read(), so nothing below was exercised";

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Ownership passes to the queue here; the flush is what releases it, which is the whole
    // property under test. If the flush never runs, the probe's destructor drains it instead and
    // the occupancy assertion below reports that this case did not exercise its arm.
    CECFrame *behindSentinel = new CECFrame(directedFrame());

    {
        // THE GATE, exactly as in the sentinel case: held across the state change and both
        // offers, so the reader is parked between its poll and its state re-check.
        CCEC_OSAL::AutoLock gate(probe.instanceLock());

        probe.markClosing();

        // ORDER IS THE WHOLE MECHANISM. The sentinel goes first so the reader's blocked poll
        // returns NULL and descends into the flush; the frame goes second so the flush is what
        // meets it.
        probe.postCloseSentinel();
        probe.postFrameBehindSentinel(behindSentinel);
    }

    ASSERT_TRUE(readerFinished.wait(PRODUCER_COMPLETION_TIMEOUT_MS))
        << "the reader did not finish within " << PRODUCER_COMPLETION_TIMEOUT_MS
        << " ms after the gate was released. It is blocked somewhere in read()";

    reader.join();

    EXPECT_TRUE(readerRaisedInvalidState)
        << "read() did not raise InvalidStateException on a driver that stopped being OPENED. "
           "The flush drains whatever is queued and then raises regardless of what it drained, "
           "so a real frame in the queue must not change that outcome";

    EXPECT_FALSE(readerReturnedCleanly)
        << "read() returned the queued frame to its caller. The flush arm drains and raises; "
           "delivering a frame from it would hand the Bus reader a frame during teardown";

    EXPECT_FALSE(readerRaisedSomethingElse)
        << "read() raised something other than InvalidStateException while draining a real frame";

    EXPECT_EQ(probe.occupancy(), 0u)
        << "the receive queue still holds " << probe.occupancy()
        << " entries. One means the reader unwound before the flush, so the frame was never "
           "drained and this case did not exercise the non-NULL arm at all";
}

// F6. THE RECEIVE GUARD, READ THROUGH THE ATOMIC STATE, ON EVERY STATE A CALLBACK CAN ARRIVE
// IN. A binder threadpool thread delivers frames while another thread may be inside open() or
// close() writing the state under the instance mutex; the guard in getIncomingQueue() reads it
// WITHOUT that mutex, deliberately, and the member is an std::atomic<int> so that the read is a
// defined atomic load rather than a data race on a plain int.
//
// WHAT THIS CASE ESTABLISHES, deterministically rather than by racing: the guard's VERDICT is
// unchanged in each of the three states - accepted while OPENED, rejected while CLOSING,
// rejected while CLOSED - and a rejection leaves the frame with the caller so the listener's
// catch can release it. Those verdicts are the observable behaviour the atomic must not have
// altered, and they are what an unlocked read of a torn or cached value could silently change.
//
// CLOSING IS THE STATE THAT MATTERS MOST and is unreachable any other way: production sets it
// inside close(), between the guard and the HAL transaction, so a callback that arrives during a
// close sees exactly this. The concurrent form of the same interleaving - a real receive handoff
// racing a real close - is already driven by the overlap cases above; this one pins the verdict
// per state so a regression in the guard cannot hide behind a scheduler that happened not to
// interleave. Destruction while a callback is in flight is covered by
// DriverAidlSessionTest.ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered, which
// needs a live listener and therefore a Binder-capable host; it is not duplicated here.
TEST_F(DriverAidlLocalInstanceTest, TheReceiveGuardRejectsACallbackDuringAndAfterClose) {
    ASSERT_NE(mock, nullptr);

    // Nothing on the receive path reaches the legacy HAL on this back-end.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);

    ReceiveQueueProbe probe;

    // POSITIVE CONTROL FIRST, so a case that rejected everything - including a frame it should
    // have taken - cannot pass. OPENED must accept, and acceptance transfers ownership.
    probe.markOpened();

    CECFrame *accepted = new CECFrame();
    accepted->append(0x0F);

    bool tookIt = false;

    ASSERT_NO_THROW({ tookIt = probe.offer(accepted); })
        << "the receive handoff raised while the driver was OPENED, so the guard is rejecting "
           "frames it must accept and the rejections below prove nothing";
    ASSERT_TRUE(tookIt)
        << "the receive handoff refused a frame on an OPENED driver with an empty queue";
    ASSERT_EQ(probe.occupancy(), 1u)
        << "the handoff reported it took the frame but the queue does not hold it";

    size_t frames = 0;
    size_t sentinels = 0;

    probe.drainCounting(frames, sentinels);

    ASSERT_EQ(frames, 1u) << "the queue did not hold the accepted frame";
    ASSERT_EQ(sentinels, 0u) << "a close sentinel appeared without a close";

    // The two teardown states. Each is set exactly as production sets it, and each must reject.
    struct TeardownState {
        const char *name;
        bool        closing;
    };

    const TeardownState states[] = {
        { "CLOSING - a callback arriving DURING a close", true },
        { "CLOSED - a callback arriving AFTER a close", false }
    };

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (states[i].closing) {
            probe.markClosing();
        }
        else {
            probe.markClosed();
        }

        CECFrame *rejected = new CECFrame();
        rejected->append(0x0F);

        EXPECT_THROW({ probe.offer(rejected); }, InvalidStateException)
            << "the receive handoff accepted a frame in state " << states[i].name
            << ". The guard in getIncomingQueue() is what rejects a callback during teardown, "
               "and the legacy back-end rejects it the same way";

        EXPECT_EQ(probe.occupancy(), 0u)
            << "a frame reached the queue in state " << states[i].name
            << ", so it would be delivered to the Bus reader after the driver stopped being open";

        // OWNERSHIP STAYED WITH THE CALLER, which is what the listener's catch relies on to
        // release the frame instead of leaking it. Releasing it here is that release.
        delete rejected;
    }
}

// F7. THE HAL'S LOGICAL ADDRESS IS VALIDATED ON THE RAW int32, NOT AFTER CONVERSION. What
// getLogicalAddresses() delivers is an arbitrary 32-bit integer read out of a parcel by the
// generated proxy; the AIDL contract says a logical address is 0x0..0xE, and nothing between
// an out-of-process HAL and this back-end enforces it.
//
// WHY VALIDATING AFTER THE CONVERSION WOULD NOT WORK, which is the reason the boundary values
// below include 256 and 271: LogicalAddress carries the value through a narrower type, so 256
// truncates to 0 and 271 truncates to 0xF - both of which look like plausible addresses to any
// check applied afterwards. A middleware that accepted one would then hand it to
// Connection::matchSource(), which rewrites the initiator nibble of outbound frames, so a
// value the HAL never reported would go out on the CEC wire.
//
// THE REJECTION PATH IS THE METHOD'S EXISTING NO-ADDRESS PATH: a zero return, which
// LibCCEC::getLogicalAddress() already turns into InvalidStateException. Nothing new is
// invented, which is also why the genuine address 0 is in the table - it is indistinguishable
// from "no address" by design on both back-ends, and this case pins that rather than pretending
// otherwise.
TEST_F(DriverAidlLocalInstanceTest, TheLogicalAddressReadAcceptsOnlyContractRangeValues) {
    ASSERT_NE(mock, nullptr);

    // The AIDL back-end never reaches the legacy address read, whatever the AIDL HAL reports.
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    struct AddressCase {
        const char *name;
        int32_t     reported;
        int         expected;
    };

    const AddressCase cases[] = {
        // In contract: accepted verbatim, including both boundaries.
        { "0x0, the lowest contract value and the genuine zero address", 0x0,  0x0 },
        { "0x1, an interior contract value",                            0x1,  0x1 },
        { "0x4, the playback device address the plugins use",           0x4,  0x4 },
        { "0xE, the highest contract value",                            0xE,  0xE },
        // Out of contract: rejected to the no-address return.
        { "0xF, the broadcast/unregistered address - not a held one",   0xF,  0 },
        { "0x10, one past the contract range",                          0x10, 0 },
        { "256, which TRUNCATES TO 0 through a narrower type",          256,  0 },
        { "271, which TRUNCATES TO 0xF through a narrower type",        271,  0 },
        { "-1, a negative status value mistaken for an address",        -1,   0 },
        { "INT32_MAX",                                                  2147483647,        0 },
        { "INT32_MIN",                                                  (-2147483647 - 1), 0 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(cases[i].reported);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        int observed = -1;

        ASSERT_NO_THROW({ observed = probe.getLogicalAddress(0); })
            << "getLogicalAddress raised for " << cases[i].name
            << ". This method never throws on HAL failure - the zero return is the failure "
               "signal, and LibCCEC::getLogicalAddress() is what raises";

        EXPECT_EQ(observed, cases[i].expected)
            << "the HAL reported " << cases[i].reported << " (" << cases[i].name
            << ") and getLogicalAddress returned " << observed << " rather than "
            << cases[i].expected;

        EXPECT_EQ(service->readCalls, 1u)
            << "IHdmiCec::getLogicalAddresses was called " << service->readCalls
            << " times for " << cases[i].name << ", not once. The validation must reject the "
               "value the HAL gave rather than ask again";
    }
}

// F7, continued. THE VALIDATION APPLIES TO ENTRY ZERO, WHICH IS THE ONLY ENTRY THIS BACK-END
// EVER USES. AAP divergence 1 requires a multi-address result to be logged and its FIRST entry
// operated on, with no iteration and no multi-address state - so a vector whose first entry is
// out of contract must be rejected even when a later entry is perfectly valid. Scanning for the
// first acceptable entry would be exactly the iteration divergence 1 forbids, and would also
// silently substitute an address the HAL did not nominate.
TEST_F(DriverAidlLocalInstanceTest, TheLogicalAddressReadValidatesEntryZeroOfAMultiAddressResult) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);

    // Entry zero valid, later entries irrelevant - including one that is out of contract, which
    // must not affect the verdict either.
    {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(0x3);
        service->addresses.push_back(0xFF);
        service->addresses.push_back(0x5);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        EXPECT_EQ(probe.getLogicalAddress(0), 0x3)
            << "a multi-address result with a valid entry zero did not yield entry zero. "
               "Divergence 1 requires the first entry to be used and the condition logged";
    }

    // Entry zero OUT of contract, a valid entry behind it. Rejected, because entry zero is the
    // entry, and 271 would have truncated to 0xF had it been converted first.
    {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(271);
        service->addresses.push_back(0x4);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        EXPECT_EQ(probe.getLogicalAddress(0), 0)
            << "an out-of-contract entry zero was accepted, or a later valid entry was "
               "substituted for it. Scanning for an acceptable entry is the iteration "
               "divergence 1 forbids, and it reports an address the HAL did not nominate";
    }

    // A non-ok transaction still reports no address, and the vector it may have written is not
    // read - the pre-existing arm, re-pinned here so the new validation cannot have displaced it.
    {
        const ::android::sp<AddressReportingDouble> service =
            ::android::sp<AddressReportingDouble>::make();

        service->addresses.push_back(0x4);
        service->readStatus = ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT);

        SessionStateProbe probe;

        probe.injectServiceOnly(service);

        EXPECT_EQ(probe.getLogicalAddress(0), 0)
            << "a failed transaction yielded an address. The vector is not to be trusted when "
               "the transaction reports failure, whatever it contains";
    }
}

// F8. EVERY DOCUMENTED SEND STATUS, ON BOTH DESTINATION KINDS, RE-PINNED HERE BECAUSE THE
// TRANSLATION IS NOW A SWITCH. The mapping's difficulty is the INVERTED broadcast sense -
// ACK_STATE_0 is an acknowledgement for a directed message and a REJECTION for a broadcast,
// ACK_STATE_1 the mirror - so a refactor of the chain into a switch has to be shown not to have
// swapped an arm. Each row states the destination kind, the reported status and the outcome the
// legacy back-end produces for the same combination.
TEST_F(DriverAidlLocalInstanceTest, EveryDocumentedTransmitStatusKeepsItsLegacyMapping) {
    ASSERT_NE(mock, nullptr);

    // The AIDL back-end never reaches the legacy transmit.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);

    enum ExpectedOutcome { OUTCOME_SUCCESS, OUTCOME_NO_ACK, OUTCOME_IO_ERROR };

    struct TransmitCase {
        const char     *name;
        bool            broadcast;
        uint8_t         opcode;
        int32_t         reported;
        ExpectedOutcome expected;
    };

    const TransmitCase cases[] = {
        { "directed ACK_STATE_0 - acknowledged by the addressed follower", false,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0),
          OUTCOME_SUCCESS },
        { "directed ACK_STATE_1 - NOT acknowledged", false,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_1),
          OUTCOME_NO_ACK },
        { "broadcast ACK_STATE_1 - sent and not rejected, the inverted sense", true,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_1),
          OUTCOME_SUCCESS },
        { "broadcast ACK_STATE_0 on REPORT_PHYSICAL_ADDRESS - the CEC CTS 9-3-3 arm", true,
          REPORT_PHYSICAL_ADDRESS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0),
          OUTCOME_NO_ACK },
        { "broadcast ACK_STATE_0 on any other opcode - returns normally, as on legacy", true,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::ACK_STATE_0),
          OUTCOME_SUCCESS },
        { "directed BUSY - arbitration failed, nothing was sent", false,
          GIVE_DEVICE_POWER_STATUS, static_cast<int32_t>(cechal::SendMessageStatus::BUSY),
          OUTCOME_IO_ERROR },
        { "broadcast BUSY - arbitration failed, nothing was sent", true,
          REPORT_PHYSICAL_ADDRESS, static_cast<int32_t>(cechal::SendMessageStatus::BUSY),
          OUTCOME_IO_ERROR }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const ::android::sp<ClosingServiceDouble> service = ::android::sp<ClosingServiceDouble>::make();
        const ::android::sp<TransmitResultDouble> controller = ::android::sp<TransmitResultDouble>::make();

        controller->reportedStatus = cases[i].reported;

        SessionStateProbe probe;

        probe.injectOpenSession(service, controller);

        const CECFrame frame =
            cases[i].broadcast ? broadcastFrame(cases[i].opcode) : directedFrame(cases[i].opcode);

        switch (cases[i].expected) {
            case OUTCOME_SUCCESS:
                EXPECT_NO_THROW({ probe.write(frame); })
                    << "write() raised for " << cases[i].name
                    << ", which the legacy back-end completes normally";
                break;

            case OUTCOME_NO_ACK:
                EXPECT_THROW({ probe.write(frame); }, CECNoAckException)
                    << "write() did not raise CECNoAckException for " << cases[i].name;
                break;

            case OUTCOME_IO_ERROR:
                EXPECT_THROW({ probe.write(frame); }, IOException)
                    << "write() did not raise IOException for " << cases[i].name;
                break;
        }

        // The transmit was attempted exactly once in every arm, including the raising ones:
        // the status is translated after the call, never instead of it.
        EXPECT_EQ(controller->sendCalls, 1u)
            << "IHdmiCecController::sendMessage was called " << controller->sendCalls
            << " times for " << cases[i].name << ", not once";

        // The probe leaves scope here, and its destructor forces the state to CLOSED so no
        // teardown re-enters close() against these doubles.
    }
}

// F8. AN UNDOCUMENTED SEND STATUS IS A FAILURE, NEVER A COMPLETED TRANSMIT. sendMessage()'s
// result is an int32 the generated proxy reads out of a parcel, so a HAL - buggy, newer, or
// hostile - can report a value that is none of the three documented enumerators. Under the old
// if/else-if chain such a value matched no condition and fell through to "Send Completed", so
// the caller was told the frame reached the CEC bus. That is the worst available answer: a
// caller told the transmit completed does not retry, so a suppressed frame is indistinguishable
// from a delivered one all the way up to the plugin.
//
// BOTH DESTINATION KINDS ARE DRIVEN, because the arms that decide the documented values are
// destination-dependent and a default arm placed inside one of them would only catch half the
// cases. The values include 3 - one past the last enumerator, the most likely accident - and a
// negative and a maximal value, which is what a misinterpreted status code or a hostile fake
// would produce.
TEST_F(DriverAidlLocalInstanceTest, AnUndocumentedTransmitStatusIsTreatedAsAFailedTransmit) {
    ASSERT_NE(mock, nullptr);

    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _)).Times(0);

    const int32_t undocumented[] = { 3, 4, 99, -1, 2147483647, (-2147483647 - 1) };

    for (size_t i = 0; i < sizeof(undocumented) / sizeof(undocumented[0]); i++) {
        for (int broadcast = 0; broadcast < 2; broadcast++) {
            const ::android::sp<ClosingServiceDouble> service =
                ::android::sp<ClosingServiceDouble>::make();
            const ::android::sp<TransmitResultDouble> controller =
                ::android::sp<TransmitResultDouble>::make();

            controller->reportedStatus = undocumented[i];

            SessionStateProbe probe;

            probe.injectOpenSession(service, controller);

            const CECFrame frame = (broadcast != 0) ? broadcastFrame() : directedFrame();

            EXPECT_THROW({ probe.write(frame); }, IOException)
                << "write() did not raise IOException for the undocumented send status "
                << undocumented[i] << " on a " << ((broadcast != 0) ? "broadcast" : "directed")
                << " frame. Anything other than a raise reports an unrecognised HAL answer to "
                   "the caller as a completed transmit";

            EXPECT_EQ(controller->sendCalls, 1u)
                << "the transmit was attempted " << controller->sendCalls
                << " times for status " << undocumented[i] << ", not once";
        }
    }
}

/**
 * @brief Fixture for the legacy halves of the authorized differences, on the resolved
 *        process-global legacy back-end with an open driver.
 *
 * Everything here needs an opened driver, which is what separates this fixture from the
 * invocation-independent local-instance one: a locally built object is always CLOSED, so an
 * arm that only differs once prelude and guard have both passed cannot be reached there.
 * Opening the shared driver is only possible where the legacy back-end is the resolved one,
 * so the fixture is partitioned by invocation and asserts that precondition rather than
 * assuming it.
 *
 * @see DriverAidlSessionFixture for the AIDL halves of the same properties, which need an
 *      opened AIDL session and so are partitioned the other way.
 */
class DriverAidlLegacyArmTest : public ::testing::Test {
protected:
    /**
     * @brief Establishes the fixture's precondition: the legacy back-end is resolved and the
     *        shared driver is open.
     *
     * @note Both checks are fatal. A body that ran against a closed driver, or against the
     *       wrong back-end, would report a failure whose cause is the harness rather than
     *       the middleware.
     */
    void SetUp() override {
        mock = HdmiCecDriverMock::getInstance();
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        ASSERT_NE(dynamic_cast<DriverImpl *>(&Driver::getInstance()), nullptr)
            << "this fixture requires the legacy back-end to be the resolved one, i.e. invocation "
               "A (CEC_TEST_AIDL_MODE=absent or unset). The selection resolves once per process "
               "inside LibCCEC::init and cannot be changed from here; re-run with the invocation-A "
               "filter from the fixture manifest above";

        // open() is deliberately not wrapped in catch(...). It returns silently when the driver
        // is already opened - its InvalidStateException throw is compiled out - so the only
        // exception it can raise is the IOException meaning the mock HAL refused to open.
        // Swallowing that would leave the process-global driver CLOSED while this fixture went
        // on asserting against it, and would hand every sibling suite in this binary a closed
        // driver too. A fatal setup failure instead: the body does not run, and TearDown still
        // gets its chance to put the shared state back.
        ASSERT_NO_THROW({ Driver::getInstance().open(); })
            << "the shared driver could not be opened, so this fixture's precondition does not "
               "hold; continuing would assert against a closed driver";
    }

    /**
     * @brief Verifies legacy HAL expectations and restores the shared driver's opened
     *        baseline.
     *
     * @note The restore is a non-fatal expectation so a failure is reported rather than
     *       cutting the rest of the cleanup short, and so no sibling suite in this binary
     *       observes a closed driver because of anything done here.
     */
    void TearDown() override {
        if (mock != nullptr) {
            ::testing::Mock::VerifyAndClearExpectations(mock);
        }

        // Restore the baseline the global environment set up, so no sibling suite observes a
        // closed driver because of anything done here. A non-fatal expectation is used so that
        // a failure is reported rather than cutting the rest of the cleanup short.
        EXPECT_NO_THROW({ Driver::getInstance().open(); })
            << "failed to restore the shared driver to its opened baseline";
    }

    /**
     * @brief The legacy HAL mock through which every case here injects and observes.
     *
     * @note Unlike the local-instance fixture, cases here do set expectations on it: the
     *       legacy arms are precisely the ones that must reach the legacy HAL.
     */
    HdmiCecDriverMock *mock = nullptr;
};

// SC6(f), the legacy half: physical-address retrieval reads the legacy HAL API and the value
// reaches the caller.
//
// This is the half of SC6(f) that is fully dischargeable. The other half is blocked on B1 and
// is covered as the documented interim behaviour by
// DriverAidlLocalInstanceTest.GetPhysicalAddressIsBlockedOnB1AndLeavesTheOutParameterUntouched
// - see the B1 paragraph in the contract block above for what would close it. The two cases
// together are the whole of SC6(f) that exists today, and the pairing is what makes the
// partial discharge visible rather than implied.
//
// The value asserted is a real HDMI physical address shape - 1.0.0.0 encoded as 0x1000 - so
// that a case which happened to leave the out-parameter untouched could not pass by accident.
/**
 * @brief SC6(f), legacy half: physical-address retrieval reads the legacy HAL API and the
 *        value reaches the caller.
 * @pre Invocation A - the legacy back-end must be the resolved one, and the shared driver
 *      open.
 * @note This is the half of SC6(f) that is fully dischargeable; the other half is blocked on
 *       B1 and covered as documented interim behaviour by the local-instance case named in the
 *       body. The pairing is what makes the partial discharge visible rather than implied.
 * @note Evidence: a real HDMI physical-address shape is asserted, and the seeded sentinel is
 *       asserted to be gone, so a case that happened to leave the out-parameter untouched
 *       could not pass by accident - which is also what keeps this case distinguishable from
 *       the blocked AIDL behaviour.
 */
TEST_F(DriverAidlLegacyArmTest, PhysicalAddressIsReadThroughTheLegacyHalApi) {
    ASSERT_NE(mock, nullptr);

    const unsigned int halPhysicalAddress = 0x1000u;
    const unsigned int sentinel = 0xDEADBEEFu;

    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<1>(halPhysicalAddress), Return(HDMI_CEC_IO_SUCCESS)));

    unsigned int physicalAddress = sentinel;
    EXPECT_NO_THROW({ Driver::getInstance().getPhysicalAddress(&physicalAddress); });

    EXPECT_EQ(physicalAddress, halPhysicalAddress)
        << "the legacy back-end did not deliver the HAL's physical address to the caller";
    EXPECT_NE(physicalAddress, sentinel)
        << "the out-parameter still holds the sentinel, so nothing was written and this case would "
           "also pass against the BLOCKED AIDL behaviour - which would make the two back-ends "
           "indistinguishable here and hide the B1 gap";
}

// Authorized difference 1, legacy half: frames of 16, 17 and 20 bytes are all sendable on the
// legacy back-end.
//
// The three sizes straddle an authority conflict rather than sampling a range. A CECFrame
// carries up to 128 bytes; the legacy HAL specification allows 20; the AIDL sendMessage
// contract states 16. Each is authoritative for its own back-end and no divergence settles
// the overlap, so 17 through 20 is a disputed band: accepted here, refused by the AIDL
// back-end. 16 is the control that must be accepted by both, which is what confines the
// difference to the band rather than to the guard's existence.
//
// The captured length is asserted per size, because "the call happened" is not the claim -
// the claim is that the whole frame reached the HAL, un-truncated, which is the property the
// AIDL half refuses to compromise by truncating.
/**
 * @brief Authorized difference 1, legacy half: frames of 16, 17 and 20 bytes are all sendable
 *        on the legacy back-end.
 * @pre Invocation A, with the shared legacy driver open.
 * @note The three sizes straddle an authority conflict rather than sampling a range, and 16 is
 *       the control that both back-ends must accept - which is what confines the difference to
 *       the disputed 17-to-20 band rather than to the existence of a guard.
 * @note The length captured at the HAL is asserted per size, because the claim is that the
 *       whole frame arrives un-truncated, which is precisely the property the AIDL half
 *       refuses to compromise.
 */
TEST_F(DriverAidlLegacyArmTest, FramesUpToTheLegacyMaximumAreSentOnTheLegacyBackEnd) {
    ASSERT_NE(mock, nullptr);

    const size_t sizes[] = { kAidlMaxMessageLength, kJustOverAidlLimit, kLegacyMaxMessageLength };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ::testing::Mock::VerifyAndClearExpectations(mock);

        int capturedLength = -1;
        EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
            .Times(1)
            .WillOnce(DoAll(SaveArg<2>(&capturedLength),
                            SetArgPointee<3>(HDMI_CEC_IO_SUCCESS),
                            Return(HDMI_CEC_IO_SUCCESS)));

        CECFrame frame = frameOfLength(sizes[i]);
        ASSERT_EQ(frame.length(), sizes[i])
            << "the frame builder did not produce a frame of " << sizes[i] << " bytes, so this "
               "iteration is not testing the size it claims to";

        EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
            << "the legacy back-end refused a frame of " << sizes[i] << " bytes, which its own HAL "
               "specification permits";

        EXPECT_EQ(capturedLength, static_cast<int>(sizes[i]))
            << "the legacy back-end passed " << capturedLength << " bytes to the HAL for a frame of "
            << sizes[i] << ", so the frame was truncated or padded on the way through";
    }
}

// Authorized difference 2, legacy half: writeAsync succeeds on an open legacy driver.
//
// This is the arm that the AIDL back-end refuses with OperationNotSupportedException, and it
// is the only part of writeAsync where the two differ - the prelude ordering and the state
// guard are identical on both, asserted in DriverAidlLocalInstanceTest above. So this case is
// the control that confines difference 2 to exactly one arm: if the prelude cases and this one
// all hold, the divergence is one branch wide and not a diffuse one.
//
// No production call site reaches writeAsync on either back-end - every plugin transmit goes
// through Connection::sendToAsync and Connection::sendAsync onto the Bus writer thread, which
// then calls the synchronous write - so the difference is unreachable in production today. It
// is asserted anyway, because "unreachable today" is not "impossible".
/**
 * @brief Authorized difference 2, legacy half: writeAsync succeeds on an open legacy driver.
 * @pre Invocation A, with the shared legacy driver open - the arm needs an open driver, which
 *      is why it cannot live in the invocation-independent fixture.
 * @note This is the control that confines difference 2 to one branch: taken with the prelude
 *       and guard cases, which are identical on both back-ends, the divergence is one arm wide
 *       rather than diffuse.
 * @note Asserted even though no production call site reaches writeAsync on either back-end,
 *       because "unreachable today" is not "impossible".
 */
TEST_F(DriverAidlLegacyArmTest, WriteAsyncSucceedsOnAnOpenLegacyDriver) {
    ASSERT_NE(mock, nullptr);

    int capturedLength = -1;
    EXPECT_CALL(*mock, HdmiCecTxAsync(_, _, _))
        .Times(1)
        .WillOnce(DoAll(SaveArg<2>(&capturedLength), Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().writeAsync(frame); })
        << "asynchronous transmit failed on the legacy back-end, so the legacy half of authorized "
           "difference 2 no longer holds and the difference is no longer one-sided";

    EXPECT_EQ(capturedLength, 2)
        << "the legacy asynchronous transmit received " << capturedLength
        << " bytes rather than the frame's 2";
}

// The legacy back-end still translates its own transmit statuses, asserted on the two arms the
// AIDL translation mirrors, so that the mirror has something measured to be a mirror OF.
//
// Two arms are chosen because they are the two that the inverted AIDL sense makes easy to get
// wrong: a directed frame reported as not acknowledged raises CECNoAckException
// (DriverImpl.cpp:276-278), while a broadcast frame reported the same way returns normally
// unless it is the CEC CTS 9-3-3 opcode (:279-284). The corresponding AIDL arms are asserted
// in DriverAidlTransmitTest below, where ACK_STATE_0 and ACK_STATE_1 swap meaning between the
// two destinations.
/**
 * @brief The legacy back-end's own transmit-status translation, on the two arms the AIDL
 *        translation mirrors.
 * @pre Invocation A, with the shared legacy driver open.
 * @note Measured so the mirror has something measured to be a mirror of. The two arms are the
 *       ones the inverted AIDL sense makes easy to get wrong: unacknowledged directed raises,
 *       unacknowledged broadcast does not, and the CEC CTS 9-3-3 opcode is the one broadcast
 *       that does.
 * @see DriverAidlTransmitTest for the corresponding AIDL arms, where the two status values
 *      swap meaning between the two destinations.
 */
TEST_F(DriverAidlLegacyArmTest, LegacyTransmitStatusTranslationDistinguishesDirectedFromBroadcast) {
    ASSERT_NE(mock, nullptr);

    // A directed frame that was not acknowledged: the addressed follower did not answer.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
                        Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame directed = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(directed); }, CECNoAckException)
        << "an unacknowledged DIRECTED frame did not raise CECNoAckException on the legacy "
           "back-end";

    ::testing::Mock::VerifyAndClearExpectations(mock);

    // The same status on a broadcast frame carrying an opcode other than
    // REPORT_PHYSICAL_ADDRESS returns normally: a broadcast has no single follower to
    // acknowledge it, so "not acknowledged" is not a failure.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
                        Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame broadcast = broadcastFrame(GIVE_DEVICE_POWER_STATUS);
    EXPECT_NO_THROW({ Driver::getInstance().write(broadcast); })
        << "an unacknowledged broadcast frame carrying an opcode other than "
           "REPORT_PHYSICAL_ADDRESS raised on the legacy back-end, so the CEC CTS 9-3-3 arm is no "
           "longer specific to that one opcode";

    ::testing::Mock::VerifyAndClearExpectations(mock);

    // And the CEC CTS 9-3-3 arm itself: a rejected broadcast REPORT_PHYSICAL_ADDRESS does
    // raise, so that the caller retries at least once.
    EXPECT_CALL(*mock, HdmiCecTx(_, _, _, _))
        .Times(1)
        .WillOnce(DoAll(SetArgPointee<3>(HDMI_CEC_IO_SENT_BUT_NOT_ACKD),
                        Return(HDMI_CEC_IO_SUCCESS)));

    CECFrame ctsBroadcast = broadcastFrame(REPORT_PHYSICAL_ADDRESS);
    EXPECT_THROW({ Driver::getInstance().write(ctsBroadcast); }, CECNoAckException)
        << "a rejected broadcast REPORT_PHYSICAL_ADDRESS did not raise on the legacy back-end, so "
           "the CEC CTS 9-3-3 retry arm is gone";
}

// The legacy half of the receive path, which the AIDL half is measured against.
//
// The receive path is not one of the authorized observable differences: a frame the HAL delivers
// must reach an application listener identically on both back-ends, byte for byte. So the claim
// the AIDL cases make - that a received message is copied into a fresh CECFrame, offered through
// the state-guarded accessor, drained by the Bus reader and delivered through Connection's filter
// to a FrameListener - is only meaningful against a measured legacy baseline for the same claim.
// This is that baseline, and it is asserted on the same helpers, with the same bounded wait, so
// the two halves are comparable rather than merely adjacent.
//
// It also exercises the observation machinery itself, on any host, which is the second reason it
// is here. The AIDL receive cases live in an invocation-B fixture and cannot execute without a
// binder driver; if the recording listener or the listening connection those cases rely on were
// wired wrongly, every one of them would fail on the binder-capable runner for a reason that had
// nothing to do with the middleware. Running the identical machinery here, where the legacy HAL
// mock can inject a frame, closes that gap.
//
// The frame is injected through the mock's captured Rx callback, which is the same route the
// existing end-to-end integration cases use (ccec/test_Connection.cpp) - it is the vendor HAL's
// own delivery mechanism, invoked on the caller's thread here rather than on a HAL thread.
/**
 * @brief The legacy half of the receive path, end to end from HAL delivery to an application
 *        listener, which the AIDL half is measured against.
 * @pre Invocation A, with the shared legacy driver open. The frame is injected through the
 *      mock's captured Rx callback, which is the vendor HAL's own delivery mechanism.
 * @note The receive path is not an authorized difference, so the AIDL claim is only meaningful
 *       against a measured legacy baseline for the same claim. Same helpers, same bounded
 *       wait, so the two halves are comparable rather than merely adjacent.
 * @note It also exercises the observation machinery on any host. The AIDL receive cases need a
 *       binder driver; a miswired recording listener or listening connection would fail every
 *       one of them on the binder-capable runner for a reason unrelated to the middleware, and
 *       running the identical machinery here closes that gap.
 */
TEST_F(DriverAidlLegacyArmTest, ReceivedMessageReachesAnApplicationListenerOnTheLegacyBackEnd) {
    ASSERT_NE(mock, nullptr);

    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlLegacyArmTest-receive",
                                            applicationListener);

    // The same shape the AIDL half delivers: a broadcast Report Physical Address with a
    // distinctive address, so a delivered frame cannot be confused with any other.
    const unsigned char injected[] = { 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };
    const std::vector<uint8_t> expected(injected, injected + sizeof(injected));

    mock->injectReceivedMessage(injected, static_cast<int>(sizeof(injected)));

    ASSERT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "a frame injected at the legacy HAL never reached an application listener within "
        << kFrameDeliveryTimeoutMs
        << " ms. Either the legacy receive path is broken - HAL callback, state-guarded queue "
           "accessor, Bus reader, Connection filter, listener - or the observation helpers the "
           "AIDL receive cases share with this one are wired wrongly, in which case those cases "
           "would fail on a binder-capable runner for a reason unrelated to the middleware";

    EXPECT_EQ(applicationListener.frameAt(0), expected)
        << "the delivered frame is not the frame that was injected, so the legacy inbound copy "
           "loses or rewrites bytes - and the AIDL half is measured against this baseline";

    EXPECT_EQ(applicationListener.frameCount(), 1u)
        << "one injected frame produced " << applicationListener.frameCount() << " deliveries";
}

/**
 * @brief Shared precondition for the two invocation-B fixtures.
 *
 * Not a test suite of its own - no TEST_F names it - so it adds no case and no fixture to the
 * filter surface. It exists because both fixtures below need exactly the same non-trivial
 * precondition, and one copy of it is one thing to keep correct rather than two.
 *
 * Requires invocation B - CEC_TEST_AIDL_MODE=compatible. SetUp asserts that the resolved
 * back-end is the AIDL one and fails if it is not, with a message naming the mode. It does
 * not skip: a skipped arm is indistinguishable from a passing one in an aggregate count,
 * which is exactly the shape that would let an AIDL invocation report green having never
 * exercised the AIDL path. And it does not read CEC_TEST_AIDL_MODE to decide - that variable
 * is read by the harness only, by applyAidlModeBeforeInit() and publishFakeForMode() in
 * tests/L1Tests/test_main.cpp; these fixtures are positioned by it
 * and must not branch on it, or a mis-set variable and a mis-wired filter would become
 * indistinguishable.
 *
 * The session is cycled in SetUp, and the order matters:
 *
 *   close -> reset -> open
 *
 * The reason is the captured listener. FakeHdmiCecService::reset() clears it, which returns
 * the event triggers to their no-op behaviour, and the listener is captured only by
 * IHdmiCec::open() - which DriverAidlImpl::open() does not call when the driver is already
 * OPENED, because it returns silently in that state. Resetting without re-opening would
 * therefore leave every trigger a silent no-op, and a case asserting on a delivered frame
 * would fail for a reason that had nothing to do with the code under test. Closing first,
 * then resetting, then opening leaves each case with a clean fake, a captured listener, and
 * counters that describe only SetUp's own open.
 *
 * Closing the shared driver here is safe, and it is safe for a documented reason rather than
 * by luck. close() offers the NULL sentinel onto the incoming queue, which is exactly what
 * that sentinel is for - it wakes the blocked Bus reader - and Bus::Reader::run() catches
 * InvalidStateException and stays in its loop (Bus.cpp:156-173), so it re-arms cleanly the
 * moment open() restores the driver. The reader is left running throughout. What is not done
 * here is reaching the same state through LibCCEC::term() and a re-arming init(): that is the
 * pattern the middleware's own test notes identify as the reader-thread hazard, and the
 * neighbouring async suite records the analysis at test_DriverImpl_Async.cpp:614-636.
 *
 * Self-sufficiency. Every case gets a freshly reset fake and an open session from SetUp, and
 * TearDown resets the fake again and restores the opened baseline non-fatally, so a failing
 * case cannot leave a closed driver or a configured fake behind for the next one - here or in
 * a sibling suite.
 */
class DriverAidlSessionFixture : public ::testing::Test {
protected:
    /**
     * @brief Establishes the invocation-B precondition and cycles the AIDL session so each
     *        case starts from a freshly reset fake with a captured listener.
     *
     * @note Every check is fatal, and none of them skips. A skipped arm is indistinguishable
     *       from a passing one in an aggregate count, which is the shape that would let an
     *       AIDL invocation report green having never exercised the AIDL path.
     * @note The close-reset-open order is load-bearing - see the class block for why
     *       resetting without re-opening would leave every event trigger a silent no-op.
     */
    void SetUp() override {
        aidlBackEnd = dynamic_cast<DriverAidlImpl *>(&Driver::getInstance());

        ASSERT_NE(aidlBackEnd, nullptr)
            << "this fixture requires the AIDL back-end to be the resolved one, i.e. invocation B "
               "(CEC_TEST_AIDL_MODE=compatible, on a host with a binder driver and a running "
               "service manager), and the factory returned the legacy back-end instead. The "
               "selection resolves once per process inside LibCCEC::init and cannot be changed "
               "from here. If this ran under invocation A, apply the invocation-A filter from the "
               "fixture manifest above, which excludes this fixture by name";

        fake = FakeHdmiCecService::getInstance();
        ASSERT_NE(fake, nullptr)
            << "the AIDL back-end resolved but no fake service was published for this process, so "
               "these cases have no HAL to program. The harness publishes it before init "
               "in publishFakeForMode(); if the back-end resolved against some other service, this "
               "run's outcome depends on a process this suite does not own";

        // Bring the session down, wipe the fake, bring it back up - see the ordering note above.
        ASSERT_NO_THROW({ Driver::getInstance().close(); })
            << "the shared driver could not be closed, so the session cannot be cycled and the "
               "fake's captured listener cannot be refreshed";

        fake->reset();
        ASSERT_NE(fake->getController(), nullptr)
            << "the fake reports no controller, so nothing here could reach addLogicalAddresses, "
               "removeLogicalAddresses or sendMessage";
        fake->getController()->reset();

        ASSERT_NO_THROW({ Driver::getInstance().open(); })
            << "the shared driver could not be re-opened after the reset, so this fixture's "
               "precondition does not hold; continuing would assert against a closed driver";

        ASSERT_EQ(fake->getOpenCallCount(), 1)
            << "the fake did not observe exactly one open() after the reset, so the session was "
               "not re-established through the HAL and the listener the triggers need was not "
               "captured";
    }

    /**
     * @brief Wipes the fake and restores the opened baseline, so a failing case leaves neither
     *        a configured fake nor a closed driver behind.
     *
     * @note The reset precedes the restore deliberately: a case that installed a failing open
     *       or close status would otherwise make the restoration fail for a reason the case
     *       created rather than a real one.
     */
    void TearDown() override {
        if (fake != nullptr) {
            // Reset before restoring the baseline: a case that installed a failing close or a
            // failing open status would otherwise make the restoration below fail for a reason
            // the case created rather than for a real one.
            if (fake->getController() != nullptr) {
                fake->getController()->reset();
            }
            fake->reset();
        }

        // Restore the baseline the global environment set up, non-fatally so that a failure is
        // reported rather than cutting the rest of the cleanup short.
        EXPECT_NO_THROW({ Driver::getInstance().open(); })
            << "failed to restore the shared driver to its opened baseline";
    }

    /**
     * @brief The resolved AIDL back-end, held so cases can reach it without repeating the cast.
     *
     * @note Non-null is a fixture precondition rather than something a case need re-check.
     */
    DriverAidlImpl *aidlBackEnd = nullptr;

    /**
     * @brief The published fake HDMI CEC service, which is the programmable HAL these cases
     *        assert against.
     */
    FakeHdmiCecService *fake = nullptr;
};

/**
 * @brief Session lifecycle, address marshalling and event delivery on the AIDL back-end.
 *
 * Requires invocation B. See DriverAidlSessionFixture for the precondition and for why the
 * session is cycled in SetUp.
 */
class DriverAidlSessionTest : public DriverAidlSessionFixture {
};

// SC6(a): a present and compatible service selects the AIDL back-end, asserted by concrete
// type in both directions and by the selected-path log line - the same two independent routes
// the legacy half uses, so that the pair of assertions is symmetric.
/**
 * @brief SC6(a): a present and compatible service selects the AIDL back-end, and the selected
 *        path is named in the log.
 * @pre Invocation B.
 * @note Asserted by concrete type in both directions and by the selected-path line, which are
 *       the same two independent routes the legacy half uses, so the pair is symmetric. The log
 *       route is the one device-level validation has, since the Driver interface carries no
 *       introspection API.
 */
TEST_F(DriverAidlSessionTest, CompatibleServiceSelectsTheAidlBackEndAndNamesItInTheLog) {
    Driver &resolved = Driver::getInstance();

    EXPECT_NE(dynamic_cast<DriverAidlImpl *>(&resolved), nullptr)
        << "a compatible service is registered but the resolved back-end is not the AIDL one";
    EXPECT_EQ(dynamic_cast<DriverImpl *>(&resolved), nullptr)
        << "the legacy back-end was selected although a compatible AIDL service is registered";

    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be redirected, so nothing about the emitted line can be "
               "established";

        CCEC_LOG(LOG_INFO, kSelectedBackEndLogFormat, kSelectedBackEndAidl);

        captured = capture.read();
    }

    const std::string expected =
        std::string("HDMI CEC HAL back-end selected : ") + kSelectedBackEndAidl;

    EXPECT_NE(captured.find(expected), std::string::npos)
        << "the production logger did not emit the selected-path line naming the AIDL back-end. "
           "That line is how the coverage runner and device-level validation establish which "
           "back-end resolved, since no introspection API exists on the Driver interface. "
           "Captured instead: [" << captured << "]";
}

// SC6(d), the add half: one logical address is marshalled as a one-element array.
//
// This is divergence 1 made observable. The AIDL calls take int[], the middleware API is
// single-valued throughout, and the array shape must stay confined to the temporary the
// adapter builds - no multi-address state, no iteration, no fan-out, and no middleware
// structure or signature growing a plural form. Asserting size() == 1 and the value is what
// distinguishes "one address was sent" from "an array happened to arrive"; a case that checked
// only the value would pass against an implementation that padded the vector.
/**
 * @brief SC6(d), add half: one middleware address is marshalled as a one-element array, and is
 *        then held locally.
 * @pre Invocation B, with the session open and the fake reset by SetUp.
 * @note Divergence 1 made observable. Both the size and the value are asserted: a case checking
 *       only the value would pass against an implementation that padded the vector, and the
 *       array shape must stay confined to the adapter's temporary.
 * @note The local effect is asserted too, because that is the caller-visible half - Connection
 *       consults exactly this, one address at a time.
 */
TEST_F(DriverAidlSessionTest, AddLogicalAddressMarshalsExactlyOneElement) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    EXPECT_TRUE(Driver::getInstance().addLogicalAddress(address))
        << "addLogicalAddress reported failure although the fake accepts by default";

    EXPECT_EQ(fake->getController()->getAddLogicalAddressesCallCount(), 1)
        << "the HAL was not asked exactly once to add the address";

    const std::vector<int32_t> sent = fake->getController()->getLastAddedLogicalAddresses();
    ASSERT_EQ(sent.size(), 1u)
        << "addLogicalAddresses received " << sent.size() << " entries for one middleware address. "
           "The array shape must stay confined to the adapter's temporary; a longer vector means "
           "multi-address behaviour leaked into the middleware";
    EXPECT_EQ(sent[0], address.toInt())
        << "the marshalled address does not match the one requested";

    // The address is now held locally, which is the caller-visible half of the operation -
    // Connection consults exactly this, one address at a time (Connection.cpp:354).
    EXPECT_TRUE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was accepted by the HAL but not recorded locally, so isValidLogicalAddress "
           "would deny an address this device holds";
}

// SC6(d), the remove half, plus the order-visible consequence that makes the legacy
// disposition observable.
//
// The legacy shape is the specification here and it is reproduced rather than improved
// (DriverImpl.cpp:316-327): the state guard, then the removal from the local list at :324,
// and only then the HAL call at :325 - whose return value is discarded. So a HAL that declines
// the removal changes nothing: the address is gone locally either way, and nothing is raised.
// Asserting the local effect after a declined remove is what pins that order; a case that only
// asserted "no exception escaped" would pass against an implementation that rolled the local
// removal back.
/**
 * @brief SC6(d), remove half: one element is marshalled, and a HAL refusal changes nothing.
 * @pre Invocation B, with an address added first so the removal has something to remove.
 * @note The legacy shape is the specification and is reproduced rather than improved: guard,
 *       local removal, then the HAL call whose return value is discarded. Asserting the local
 *       effect after a declined removal is what pins that order - a case asserting only "no
 *       exception escaped" would pass against an implementation that rolled the removal back.
 */
TEST_F(DriverAidlSessionTest, RemoveLogicalAddressMarshalsOneElementAndIgnoresHalRefusal) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    ASSERT_TRUE(Driver::getInstance().isValidLogicalAddress(address));

    // The HAL declines the removal, and a non-ok status is asserted separately below.
    fake->getController()->setRemoveLogicalAddressesResult(false);

    EXPECT_NO_THROW({ Driver::getInstance().removeLogicalAddress(address); })
        << "removeLogicalAddress raised when the HAL declined. The legacy back-end discards that "
           "return value and returns silently, so raising here would be an unregistered "
           "behaviour change - and one that Bus and LibCCEC do not expect";

    EXPECT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), 1)
        << "the HAL was not asked exactly once to remove the address";

    const std::vector<int32_t> sent = fake->getController()->getLastRemovedLogicalAddresses();
    ASSERT_EQ(sent.size(), 1u)
        << "removeLogicalAddresses received " << sent.size() << " entries for one middleware "
           "address";
    EXPECT_EQ(sent[0], address.toInt()) << "the marshalled address does not match the one released";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address is still held locally after a declined removal. The local removal precedes "
           "the HAL call and is not rolled back, so it must be gone regardless of what the HAL "
           "reported - that ordering is the legacy behaviour being preserved";
}

// SC6(d), the remove half's successful arm - the one the branch manifest names an
// invocation-B reacher for, and the one no case in this file reached until now.
//
// The declined arm above and the transport-failure arm below both establish that a failure is
// discarded, and between them they leave `ok == true` uncovered: an implementation that always
// took the failure path - logging, and never treating the removal as having succeeded - would
// satisfy every one of the other cases. So this arm is asserted on its own terms, and it asserts
// the four things that together describe a successful removal: exactly one HAL call, a
// one-element vector carrying the right value, no exception, and the address gone locally.
//
// The result is set explicitly rather than left to the fake's default, so the arm the case
// covers is stated in the case rather than inferred from the fake's initial state - which a
// change to that state would silently move.
/**
 * @brief The remove half's successful arm, asserted on its own terms.
 * @pre Invocation B. The HAL result is set explicitly rather than left to the fake's default, so
 *      the arm under test is stated in the case rather than inferred from the fake's initial
 *      state.
 * @note The declined and transport-failure arms both establish that a failure is discarded, and
 *       between them leave the success path uncovered: an implementation that always took the
 *       failure path would satisfy every other case. Four things together describe a success -
 *       exactly one HAL call, a one-element vector carrying the right value, no exception, and
 *       the address gone locally - and all four are asserted.
 */
TEST_F(DriverAidlSessionTest, RemoveLogicalAddressSucceedsMarshalsOneElementAndDropsItLocally) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    ASSERT_TRUE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was not held locally after a successful add, so the removal below would "
           "have nothing to remove and the local assertion would be vacuous";

    ASSERT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), 0)
        << "the HAL was already asked to remove an address before this case asked it to, so the "
           "call count below would not describe this case's own call";

    // The HAL accepts the removal - the arm under test.
    fake->getController()->setRemoveLogicalAddressesResult(true);

    EXPECT_NO_THROW({ Driver::getInstance().removeLogicalAddress(address); })
        << "removeLogicalAddress raised on the success path. Nothing in the legacy shape raises "
           "here at all, and neither Bus nor LibCCEC guards this call";

    EXPECT_EQ(fake->getController()->getRemoveLogicalAddressesCallCount(), 1)
        << "the HAL was asked " << fake->getController()->getRemoveLogicalAddressesCallCount()
        << " times to remove one address. More than once means the adapter is retrying or looping "
           "over something; none means the local list was updated without telling the HAL, which "
           "leaves the middleware's view of this device's addresses and the HAL's permanently "
           "divergent";

    const std::vector<int32_t> sent = fake->getController()->getLastRemovedLogicalAddresses();
    ASSERT_EQ(sent.size(), 1u)
        << "removeLogicalAddresses received " << sent.size()
        << " entries for one middleware address. The array shape must stay confined to the "
           "adapter's temporary - divergence 1 forbids multi-address state in the middleware";
    EXPECT_EQ(sent[0], address.toInt())
        << "the marshalled address does not match the one released, so some other address was "
           "removed at the HAL - which on a real device releases an address this device still "
           "believes it holds";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address is still held locally after a successful removal, so isValidLogicalAddress "
           "would admit frames for an address the HAL no longer serves";
}

// The close sentinel is offered before the transaction and its result are evaluated, proved by a
// reader that is actually parked on the incoming queue.
//
// What this protects. DriverAidlImpl::close() offers the NULL sentinel onto the incoming queue
// immediately after moving the state to CLOSING, and only then performs the HAL transaction and
// evaluates its result - raising IOException if either says the close did not happen. That order
// is load-bearing and it is invisible to every other assertion in this file: move the offer
// below the `if (!txn.isOk() || !closed) throw` and, on the failure path, the sentinel is never
// offered at all. The state still reaches CLOSED, the exception still escapes, the local address
// list is still kept, the call counts are all still right - and a thread parked in the queue's
// blocking poll stays parked forever, because the sentinel is the only thing that ever wakes it.
// A middleware whose reader thread never returns cannot be torn down and cannot be
// re-initialised.
//
// So a reader is actually parked, and its release is what is asserted. Three properties make the
// proof sound rather than a race:
//
//   1. The session is proved live first, by a successful write, so read()'s entry guard cannot be
//      what releases the reader.
//
//   2. The reader is proved parked, positively, by delivering a frame to it and observing read()
//      return with it. After that it re-enters read() and can only be in the blocking poll - it
//      is not "probably scheduled by now", it has demonstrably been round the loop once.
//
//   3. The release cannot be explained by later cleanup. The bound is waited out while the driver
//      is still fully alive and in scope; the destructor has not run, and nothing else touches
//      the queue. And the release is identified specifically: read() must raise
//      InvalidStateException, which is what the sentinel plus a state that is no longer OPENED
//      produces, and not some other exception.
//
// A local instance is used, not the process-global driver, because the global one is the Bus
// reader's and parking a second consumer on the same queue would make the two compete for one
// sentinel. A local instance under invocation B resolves its own proxy through
// isServiceAvailable() and opens against the same fake, which accepts the session, so it has a
// queue of its own with exactly one reader on it.
//
// Both failure arms are covered, because they converge on one condition and each can satisfy it
// alone: the HAL reporting that it did not close, and the transaction itself failing.
//
// The driver and the reader state are heap-held, deliberately. If the sentinel really is missing
// the reader cannot be joined, and abandoning that thread is the only alternative to hanging the
// whole run - which is only safe if everything the thread touches outlives this scope.
/**
 * @brief close() offers the sentinel before it evaluates the transaction, proved by a reader
 *        actually parked on the incoming queue and released on both failure arms.
 * @pre Invocation B. A local instance is used rather than the process-global driver, because
 *      parking a second consumer on the Bus reader's queue would make the two compete for one
 *      sentinel.
 * @note What it protects is invisible to every other assertion here: move the offer below the
 *       failure check and, on the failure path, the sentinel is never offered - the state still
 *       reaches CLOSED, the exception still escapes, every count is still right, and a parked
 *       reader stays parked forever.
 * @note Three properties make it a proof rather than a race: the session is proved live first,
 *       so the entry guard cannot be what releases the reader; the reader is proved parked
 *       positively, by a delivered frame it returns with before re-entering; and the release is
 *       identified specifically as InvalidStateException while the driver is still in scope, so
 *       later cleanup cannot explain it.
 * @warning The driver and the reader state are heap-held deliberately. If the sentinel really is
 *          missing the reader cannot be joined, and abandoning that thread is the only
 *          alternative to hanging the run - which is safe only if everything it touches outlives
 *          this scope.
 */
TEST_F(DriverAidlSessionTest, AFailedCloseStillReleasesAReaderParkedOnTheIncomingQueue) {
    /**
     * @brief One of the two ways close() can report failure, driven identically.
     *
     * @note Tabulated rather than written out twice so the two arms cannot drift apart: they
     *       converge on one condition in production and each must release the reader alone.
     */
    struct Arm {
        const char *description;  /**< Text for SCOPED_TRACE, so a failure names its arm. */
        bool reportNotClosed;     /**< The HAL reports that the session was not closed. */
        bool failTransaction;     /**< The close transaction itself fails. */
    };

    const Arm arms[] = {
        { "arm: the HAL reported that the session was not closed", true, false },
        { "arm: the close transaction itself failed", false, true },
    };

    for (size_t armIndex = 0; armIndex < (sizeof(arms) / sizeof(arms[0])); armIndex++) {
        const Arm &arm = arms[armIndex];
        SCOPED_TRACE(arm.description);

        // A clean slate for this arm, so a control left set by the previous one cannot decide
        // this one's outcome.
        fake->setCloseResult(true);
        fake->setCloseBinderStatus(::android::binder::Status::ok());

        std::shared_ptr<DriverAidlImpl> localDriver = std::make_shared<DriverAidlImpl>();
        std::shared_ptr<BlockedReaderState> reader = std::make_shared<BlockedReaderState>();

        ASSERT_TRUE(localDriver->isServiceAvailable())
            << "a local instance could not resolve the fake service, so it cannot open a session "
               "of its own and no reader can be parked on a queue this case controls";

        ASSERT_NO_THROW({ localDriver->open(); })
            << "the local instance could not open a session against the fake, so there is no queue "
               "to park a reader on";

        // Property 1: the session is live. If it were not, read() would return through its entry
        // guard and the release below would prove nothing.
        CECFrame livenessProbe = directedFrame();
        ASSERT_NO_THROW({ localDriver->write(livenessProbe); })
            << "the local session cannot transmit, so it is not really open and read()'s entry "
               "guard - not the close sentinel - would be what releases the reader";

        ASSERT_NE(fake->getListener(), nullptr)
            << "the local open did not hand a listener to the fake, so no frame can be delivered "
               "to the local queue and the reader cannot be proved parked";

        std::thread readerThread([localDriver, reader]() {
            CECFrame frame;

            try {
                {
                    std::lock_guard<std::mutex> guard(reader->mutex);
                    reader->enteredRead = true;
                }
                reader->signal.notify_all();

                // First read: released by the frame the case delivers.
                localDriver->read(frame);
                {
                    std::lock_guard<std::mutex> guard(reader->mutex);
                    reader->framesRead++;
                }
                reader->signal.notify_all();

                // Second read: nothing is queued, so this parks in the blocking poll and only the
                // close sentinel can release it.
                localDriver->read(frame);
                {
                    std::lock_guard<std::mutex> guard(reader->mutex);
                    reader->framesRead++;
                }
            }
            catch (InvalidStateException &e) {
                (void)e;
                std::lock_guard<std::mutex> guard(reader->mutex);
                reader->releasedByInvalidState = true;
            }
            catch (...) {
                std::lock_guard<std::mutex> guard(reader->mutex);
                reader->releasedByOtherException = true;
            }

            {
                std::lock_guard<std::mutex> guard(reader->mutex);
                reader->finished = true;
            }
            reader->signal.notify_all();
        });

        // From here on every assertion is non-fatal: a fatal one would leave this scope with a
        // thread still running and nothing joined.
        EXPECT_TRUE(
            reader->waitFor(kFrameDeliveryTimeoutMs, [&reader]() { return reader->enteredRead; }))
            << "the reader thread never reached read(), so nothing was parked and the release "
               "asserted below would be meaningless";

        // Property 2: prove the reader is parked, by making it return once.
        const std::vector<uint8_t> wakeUpFrame{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x51, 0x00, 0x04 };
        EXPECT_TRUE(fake->fireOnMessageReceived(wakeUpFrame))
            << "the fake held no listener, so the frame that proves the reader is parked could not "
               "be delivered";

        EXPECT_TRUE(reader->waitFor(kFrameDeliveryTimeoutMs,
                                    [&reader]() { return reader->framesRead >= 1; }))
            << "the frame delivered to the local session never came back out of read(), so the "
               "reader is not parked on THIS driver's queue and the rest of this case cannot "
               "establish anything about the sentinel";

        // Now the failing close. The reader is parked in the poll with an empty queue.
        if (arm.reportNotClosed) {
            fake->setCloseResult(false);
        }
        if (arm.failTransaction) {
            fake->setCloseBinderStatus(
                ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
        }

        EXPECT_THROW({ localDriver->close(); }, IOException)
            << "the failing close did not raise IOException, so this arm is not the failure path "
               "it is meant to exercise";

        const bool releasedInTime =
            reader->waitFor(kFrameDeliveryTimeoutMs, [&reader]() { return reader->finished; });

        EXPECT_TRUE(releasedInTime)
            << "the reader parked on the incoming queue was not released by a close that failed, "
               "within "
            << kFrameDeliveryTimeoutMs
            << " ms, while the driver was still alive and in scope. The NULL sentinel must be "
               "offered before the close transaction and its result are evaluated: on this path "
               "the result check raises, so an offer placed after it never happens and the reader "
               "parks forever. Every other assertion about close() - the CLOSED state, the "
               "IOException, the retained address list, the call counts - still passes when that "
               "happens, which is precisely why this case exists";

        EXPECT_TRUE(reader->releasedByInvalidState)
            << "the reader was released, but not by InvalidStateException. The sentinel plus a "
               "state that is no longer OPENED is what produces that exception, and it is the "
               "release Bus::Reader::run() is written around - it catches InvalidStateException "
               "and re-arms. Some other exception means the reader was woken by something else";

        EXPECT_FALSE(reader->releasedByOtherException)
            << "read() raised an exception that is neither InvalidStateException nor nothing at "
               "all, which would propagate out of Bus::Reader::run() and end the reader thread";

        if (!releasedInTime) {
            // The failure is already reported. This only stops a genuine regression from hanging
            // the entire run: re-open and close cleanly, which offers a fresh sentinel.
            fake->setCloseResult(true);
            fake->setCloseBinderStatus(::android::binder::Status::ok());

            try {
                localDriver->open();
                localDriver->close();
            }
            catch (...) {
                // Nothing to add: the assertion above has already reported the defect, and this
                // rescue is best-effort by construction.
            }

            if (!reader->waitFor(kFrameDeliveryTimeoutMs,
                                 [&reader]() { return reader->finished; })) {
                // Still parked. Abandon the thread rather than hang: it holds shared ownership of
                // both the driver and its own state, so both outlive this scope and nothing it
                // touches is freed underneath it.
                readerThread.detach();
                ADD_FAILURE() << "the reader could not be released even by a clean close, so the "
                                 "thread was abandoned to keep the run from hanging. This process "
                                 "now holds one parked thread and one driver that cannot be "
                                 "collected; treat every later result in this binary as suspect";
                continue;
            }
        }

        readerThread.join();

        fake->setCloseResult(true);
        fake->setCloseBinderStatus(::android::binder::Status::ok());
    }
}

// SC6(d), the get half: the address is read from entry zero of the returned array, and it is
// read from IHdmiCec rather than from the controller.
//
// The interface split is worth asserting and not merely commenting: getLogicalAddresses lives
// on IHdmiCec, while only addLogicalAddresses, removeLogicalAddresses and sendMessage live on
// IHdmiCecController. The existing technical specification attributes it to the controller,
// which is wrong, so the service-side call counter is checked to pin where the call actually
// goes.
/**
 * @brief getLogicalAddress reads entry zero of the result, and asks the service interface rather
 *        than the controller.
 * @pre Invocation B, with the fake holding exactly one address.
 * @note The call count is asserted because getLogicalAddresses lives on IHdmiCec and not on
 *       IHdmiCecController: a zero here means the address came from somewhere else entirely.
 */
TEST_F(DriverAidlSessionTest, GetLogicalAddressReadsEntryZeroFromTheServiceInterface) {
    const int32_t halAddress = 8;
    fake->setLogicalAddressesResult(std::vector<int32_t>{ halAddress });

    const int reported = Driver::getInstance().getLogicalAddress(DeviceType::TUNER);

    EXPECT_EQ(reported, static_cast<int>(halAddress))
        << "getLogicalAddress did not report the single address the HAL holds";

    EXPECT_EQ(fake->getGetLogicalAddressesCallCount(), 1)
        << "IHdmiCec::getLogicalAddresses was not called exactly once. It lives on the service "
           "interface, not on the controller, and a call routed to the controller instead would "
           "not compile against this snapshot - so a zero here means the address came from "
           "somewhere else entirely";
}

// SC6(e): more than one returned address operates on entry zero and logs the condition.
//
// Divergence 1 is explicit that a multi-address HAL result is logged and the first entry used,
// with no multi-address state, no iteration and no dispatch fan-out added to the middleware.
// Both halves are asserted - the returned value and the log naming the count and the entry
// used - because the log is the only signal a platform integrator gets that the HAL is
// reporting more than the middleware can represent, and a silent first-entry pick would hide
// a real configuration problem.
//
// The log half matches the composed line, not a digit, and the difference is not cosmetic. A
// search for a bare "3" - the count this fixture's vector happens to have - could not fail:
// every CCEC_LOG line is prefixed with a YYMMDD-HH:MM:SS:usec timestamp, so a bare digit is
// present in any non-empty capture and such an assertion holds against a log that never
// mentions the count at all. The expected text is therefore built from the fixture's own
// vector, its size and
// its entry 0, and matched as one contiguous string: a digit can then only satisfy it in the
// place the report is supposed to put it, next to the label that gives it meaning. Building it
// from the vector rather than typing it also keeps the expectation and the stimulus from
// drifting apart if the addresses below are ever changed.
/**
 * @brief SC6(e): a multi-address HAL result operates on entry zero and reports the condition,
 *        naming the count and the entry used.
 * @pre Invocation B, with the fake holding more than one address.
 * @note Both halves are asserted because the log is the only signal an integrator gets that the
 *       HAL reports more than the middleware can represent, and a silent first-entry pick would
 *       hide a real configuration problem.
 * @note The expected text is composed from the stimulus vector and matched as one contiguous
 *       string. A bare digit cannot serve: every log line carries a timestamp, so a digit is
 *       present in any non-empty capture and would be satisfied without the report naming the
 *       count at all. Composing it from the vector also keeps expectation and stimulus from
 *       drifting apart.
 */
TEST_F(DriverAidlSessionTest, MultipleReturnedAddressesUseEntryZeroAndAreLogged) {
    const std::vector<int32_t> halAddresses{ 4, 8, 11 };
    fake->setLogicalAddressesResult(halAddresses);

    /*
     * Transcribed from the producer's format string, which lives inline in
     * DriverAidlImpl::getLogicalAddress() and so cannot be imported:
     *
     *   "DriverAidlImpl::getLogicalAddress : the HAL reports %zu logical addresses;
     *    operating on entry 0 [%d]\r\n"      <- one line there, wrapped here
     *
     * The two substitutions are halAddresses.size() and (int)halAddresses[0], which is exactly
     * what is composed here. The terminator is deliberately not part of the expectation: the
     * line is matched as a substring of a capture that also holds the CEC log prefix, the
     * timestamp and anything else emitted while the capture was open.
     */
    const std::string expectedMultiAddressReport =
        std::string("DriverAidlImpl::getLogicalAddress : the HAL reports ")
        + std::to_string(halAddresses.size()) + " logical addresses; operating on entry 0 ["
        + std::to_string(static_cast<int>(halAddresses[0])) + "]";

    int reported = 0;
    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid()) << "stdout could not be redirected, so the log half of this "
                                         "case cannot be established";

        reported = Driver::getInstance().getLogicalAddress(DeviceType::TV);

        captured = capture.read();
    }

    EXPECT_EQ(reported, static_cast<int>(halAddresses[0]))
        << "getLogicalAddress did not report entry 0 of a multi-entry result. Operating on the "
           "first entry is the specified behaviour; picking any other entry, or iterating, would "
           "introduce multi-address behaviour the middleware has no way to represent";

    // One match covers both halves the AAP requires of this report - the count the HAL returned
    // and the entry that was used - because both appear in the one line, each next to its own
    // label. Two loose fragment searches would report which half is missing but would also each
    // be satisfiable by text the report did not produce.
    EXPECT_NE(captured.find(expectedMultiAddressReport), std::string::npos)
        << "the multi-address report is missing or no longer names the count and the entry used. "
           "Expected to find: [" << expectedMultiAddressReport
        << "]. Without it an integrator has no signal that the HAL holds more addresses than the "
           "middleware can represent, and a first-entry pick that hides a real platform "
           "misconfiguration is indistinguishable from a single-address HAL. Captured: ["
        << captured << "]";
}

// An empty result reports 0, and 0 is the contract rather than a shortfall.
//
// The AIDL contract states the array is empty when no additional addresses have been set, so
// this is a documented normal outcome and not an error. Reporting 0 is what makes it behave
// like the legacy path: LibCCEC::getLogicalAddress turns a zero into InvalidStateException
// (LibCCEC.cpp:164-167), which is the signal callers already handle. Any other sentinel would
// suppress that throw and hand a caller an address it does not hold.
/**
 * @brief An empty HAL result reports 0, which is the contract rather than a shortfall.
 * @pre Invocation B, with the fake holding no addresses.
 * @note The AIDL contract states the array is empty when no additional addresses have been set,
 *       so this is a documented normal outcome. Reporting 0 is what keeps the behaviour identical
 *       to the legacy path, where LibCCEC turns a zero into InvalidStateException - the signal
 *       callers already handle. Any other sentinel would suppress that throw.
 */
TEST_F(DriverAidlSessionTest, EmptyAddressResultReportsZeroSoTheExistingCallerSignalSurvives) {
    fake->setLogicalAddressesResult(std::vector<int32_t>());

    EXPECT_EQ(Driver::getInstance().getLogicalAddress(DeviceType::TV), 0)
        << "an empty HAL result did not report 0. Zero is what LibCCEC::getLogicalAddress turns "
           "into InvalidStateException, so any other value would report an address this device "
           "does not have";

    EXPECT_EQ(fake->getGetLogicalAddressesCallCount(), 1)
        << "the HAL was not asked exactly once";
}

// Authorized difference 3: addLogicalAddress's failure category is coarser on the AIDL
// back-end than on the legacy one, and that is forced rather than chosen.
//
// The legacy implementation distinguishes three outcomes from one HAL status
// (DriverImpl.cpp:339-347): LOGICALADDRESS_UNAVAILABLE becomes AddressNotAvailableException,
// GENERAL_ERROR becomes IOException, and any other value is success. IHdmiCecController::
// addLogicalAddresses returns one boolean, documented as false when the address is outside
// 0x0..0xE or already added, so the distinction cannot be carried without a new HAL method -
// which the constraints forbid. So false maps to the nearer legacy category,
// AddressNotAvailableException, and a non-ok binder status maps to IOException.
//
// Both mappings are asserted here, and their caller-visible effect is measured separately
// below rather than assumed to be equivalent.
/**
 * @brief Authorized difference 3: a HAL refusal and a transport failure map to distinct
 *        exceptions, which is the coarser categorisation the AIDL contract forces.
 * @pre Invocation B.
 * @note The legacy implementation distinguishes three outcomes from one HAL status; the AIDL call
 *       returns one boolean, so the distinction cannot be carried without a new HAL method, which
 *       the constraints forbid. A refusal therefore maps to the nearer legacy category and a
 *       non-ok status to IOException.
 * @see The following case, which measures the caller-visible effect through the real Sink call
 *      paths rather than assuming the two categories are equivalent.
 */
TEST_F(DriverAidlSessionTest, AddLogicalAddressMapsRefusalAndTransportFailureToDistinctExceptions) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    // The HAL refuses the address: it is out of range, or already taken.
    fake->getController()->setAddLogicalAddressesResult(false);

    EXPECT_THROW({ Driver::getInstance().addLogicalAddress(address); }, AddressNotAvailableException)
        << "a refused address did not raise AddressNotAvailableException. That is the nearer of "
           "the two legacy categories for 'the address is not available', and it is what the Sink "
           "plugin's allocation loop is written around";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was recorded locally although the HAL refused it, so the middleware's "
           "bookkeeping and the HAL's have diverged";

    fake->getController()->reset();

    // Transport failure: a dead binder, an EX_ILLEGAL_STATE, a marshalling fault. Distinct
    // from a refusal, and mapped distinctly.
    fake->getController()->setAddLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().addLogicalAddress(address); }, IOException)
        << "a non-ok binder status did not raise IOException. A transport failure is not an "
           "unavailable address, and the Sink plugin's call path distinguishes the two";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address was recorded locally although the transaction failed";
}

// The same two failures again, measured through the real caller paths rather than through the
// Driver interface, because "the caller sees the same thing" is a claim about the plugin and
// not about the adapter.
//
// The two Sink call paths differ, and the difference is structural - re-read from the plugin
// source, which is out of scope and is not modified:
//
//   Path 1  LibCCEC::getInstance().addLogicalAddress(...) at
//           entservices-hdmicecsink/plugin/HdmiCecSinkImplementation.cpp:2767, inside the try
//           opened at :2764, which has three catches - catch(InvalidStateException &e) at
//           :2789, catch(IOException &e) at :2793 and catch(...) at :2797. So a transport
//           failure reaches the distinct :2793 arm, while a refused address falls through to
//           the generic :2797 arm. Both set the same poll-thread state, but they are
//           different arms and they log differently.
//
//   Path 2  the enable-time call at :3065, inside the if at :3061 and not inside any try
//           block - the nearby IOException catches at :3040 and :3173 belong to LibCCEC::init
//           and term, not to this call - so either exception propagates out of the enable path.
//
// LibCCEC::addLogicalAddress itself catches nothing and propagates (LibCCEC.cpp:133-144).
// Both dispositions are modelled here - a try/catch shaped like path 1, and an uncaught
// propagation shaped like path 2 - so the caller-visible effect of difference 3 is measured.
// What this establishes is that the coarser category still lands in a specific, handled arm on
// path 1 and still propagates on path 2, which is the property that makes the difference
// tolerable for those callers.
//
// The model is guarded against drift separately, and that guard is what makes this case
// evidence about the plugin rather than about a shape once copied out of it:
// DriverAidlLocalInstanceTest.TheModelledSinkCallPathsStillMatchTheRealSinkSource reads the
// plugin source and asserts the three structural facts this case depends on - two call sites,
// the first inside a try carrying both an IOException arm and a generic arm, the second inside
// no try. It runs on every invocation, including the ones with no binder driver, so it is
// present on runs where this case is filtered out. If the plugin drifts, that case fails and
// names what changed; this one keeps asserting the behaviour, which is still the middleware's
// to guarantee whatever the caller does with it.
/**
 * @brief The same two failures measured through the real Sink call paths, so difference 3's
 *        caller-visible effect is established rather than assumed.
 * @pre Invocation B. Both Sink dispositions are modelled: a try/catch shaped like the first call
 *      site, and an uncaught propagation shaped like the enable-time one.
 * @note "The caller sees the same thing" is a claim about the plugin, not about the adapter, so
 *       it is asserted at the plugin's shape. What this establishes is that the coarser category
 *       still lands in a specific handled arm on the first path and still propagates on the
 *       second, which is the property that makes the difference tolerable for those callers.
 * @see DriverAidlLocalInstanceTest::TheModelledSinkCallPathsStillMatchTheRealSinkSource, which
 *      reads the plugin source and asserts the three structural facts this case depends on. It
 *      runs on every invocation, so it is present on runs where this case is filtered out; if
 *      the plugin drifts, that case fails and names what changed.
 */
TEST_F(DriverAidlSessionTest, AddLogicalAddressFailuresReachBothRealSinkCallPathsAsExpected) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);

    // Path 1, refusal: the three-catch shape at HdmiCecSinkImplementation.cpp:2789-2799. A
    // refusal is not an IOException, so it must fall through to the generic arm.
    fake->getController()->setAddLogicalAddressesResult(false);
    {
        bool reachedInvalidStateArm = false;
        bool reachedIoArm = false;
        bool reachedGenericArm = false;

        try {
            LibCCEC::getInstance().addLogicalAddress(address);
        }
        catch (InvalidStateException &e) {
            (void)e;
            reachedInvalidStateArm = true;
        }
        catch (IOException &e) {
            (void)e;
            reachedIoArm = true;
        }
        catch (...) {
            reachedGenericArm = true;
        }

        EXPECT_FALSE(reachedInvalidStateArm)
            << "a refused address reached the InvalidStateException arm, which the Sink treats as "
               "'the library is not initialized' - a different diagnosis entirely";
        EXPECT_FALSE(reachedIoArm)
            << "a refused address reached the IOException arm at "
               "HdmiCecSinkImplementation.cpp:2793, so a full address pool would be logged and "
               "diagnosed as a transport failure";
        EXPECT_TRUE(reachedGenericArm)
            << "a refused address reached no catch arm on path 1, so nothing was raised at all and "
               "the Sink would carry on believing it holds an address the HAL refused";
    }

    fake->getController()->reset();

    // Path 1, transport failure: this one must reach the distinct IOException arm.
    fake->getController()->setAddLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));
    {
        bool reachedIoArm = false;
        bool reachedGenericArm = false;

        try {
            LibCCEC::getInstance().addLogicalAddress(address);
        }
        catch (IOException &e) {
            (void)e;
            reachedIoArm = true;
        }
        catch (...) {
            reachedGenericArm = true;
        }

        EXPECT_TRUE(reachedIoArm)
            << "a transport failure did not reach the IOException arm at "
               "HdmiCecSinkImplementation.cpp:2793, so the one failure the Sink diagnoses "
               "specifically would be logged as a generic exception instead";
        EXPECT_FALSE(reachedGenericArm) << "the transport failure reached the generic arm instead";
    }

    fake->getController()->reset();

    // Path 2: the enable-time call site sits outside any try block, so the exception must
    // propagate out of LibCCEC rather than be absorbed anywhere below it. Modelled as the
    // uncaught propagation it is; EXPECT_THROW here is the assertion that nothing in the
    // middleware swallows it on the way up.
    fake->getController()->setAddLogicalAddressesResult(false);
    EXPECT_THROW({ LibCCEC::getInstance().addLogicalAddress(address); },
                 AddressNotAvailableException)
        << "the refusal did not propagate out of LibCCEC::addLogicalAddress. The enable-time Sink "
           "call at HdmiCecSinkImplementation.cpp:3065 is not inside a try block, so an exception "
           "absorbed in the middleware would leave that path believing it acquired an address";
}

// The close sequence, asserted as a sequence rather than as an outcome.
//
// Three things are ordered inside DriverAidlImpl::close(), and each is observable:
//
//   1. the state becomes CLOSED **before** any failure is raised
//      (the closing sequence in DriverAidlImpl::close(), mirroring DriverImpl.cpp:145-150), so
//      a caller that
//      swallows the exception still sees a consistently closed object rather than one that
//      believes it is open with no session behind it;
//   2. the local logical-address list is deliberately not cleared - DriverImpl::close() does
//      not clear it either, so isValidLogicalAddress can keep reporting true across a close,
//      and the HAL drops the addresses on its own side anyway. Clearing would be an
//      unauthorized improvement and a fourth observable difference;
//   3. the controller reference is released, so a subsequent operation cannot reach a stale
//      session.
//
// Asserting only that IOException escaped would leave all three unproved. Note that a green
// result here does not confirm B2 - which AIDL method corresponds to the legacy HdmiCecClose()
// is pending owner confirmation; what is asserted is the sequence, which holds whichever
// method the owners confirm.
/**
 * @brief The close sequence asserted as a sequence: CLOSED is reached before the failure is
 *        raised, the local address list is kept, and the controller reference is released.
 * @pre Invocation B, with an address held locally so the second property is not vacuous.
 * @note Asserting only that IOException escaped would leave all three unproved. The kept address
 *       list is deliberate parity - the legacy close does not clear it either, and clearing would
 *       be an unauthorized improvement and a fourth observable difference.
 * @note The controller identity is asserted, not merely that some close happened: a close naming
 *       the wrong controller leaves the real session open HAL-side while the middleware believes
 *       it has gone, which is the state a subsequent open then refuses.
 * @note A green result here does not confirm B2. Which AIDL method corresponds to the legacy
 *       close is pending owner confirmation; what is asserted is the sequence, which holds
 *       whichever method the owners confirm.
 */
TEST_F(DriverAidlSessionTest, FailedCloseStillReachesTheClosedStateAndKeepsTheLocalAddressList) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    ASSERT_TRUE(Driver::getInstance().isValidLogicalAddress(address));

    // The HAL reports that it did not close the session.
    fake->setCloseResult(false);

    EXPECT_THROW({ Driver::getInstance().close(); }, IOException)
        << "a close the HAL reported as failed did not raise IOException";

    EXPECT_EQ(fake->getCloseCallCount(), 1) << "the HAL was not asked exactly once to close";

    // Which session was closed, not merely that one was. The controller is captured before the
    // fake evaluates its configured result, so this holds on the failure path too - and it has
    // to: a close that names the wrong controller leaves the real session open HAL-side while
    // the middleware believes it has gone, which is exactly the state IHdmiCec::open() then
    // refuses with EX_ILLEGAL_STATE.
    {
        const ::android::sp<cechal::IHdmiCecController> closedController =
            fake->getLastClosedController();
        const ::android::sp<cechal::IHdmiCecController> openedController = fake->getController();

        ASSERT_NE(closedController.get(), nullptr)
            << "the HAL recorded no controller for this close, so close() passed nothing "
               "identifiable and the HAL cannot know which session to end";
        EXPECT_EQ(closedController.get(), openedController.get())
            << "the close named a different controller from the one open() returned, on the arm "
               "where the HAL reports the session was not closed";
    }

    // The state reached CLOSED before the raise, so the guards are live again. Observed
    // through a guarded operation, since the state itself is private and no introspection
    // exists - which is the point: this is the caller-visible form of the invariant.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver still behaves as though it were open after a failed close. The state must "
           "reach CLOSED before the exception is raised, or a caller that swallows the IOException "
           "is left with an object that believes it holds a session it does not";

    EXPECT_TRUE(Driver::getInstance().isValidLogicalAddress(address))
        << "close() cleared the local logical-address list. The legacy back-end does not clear it "
           "either, so clearing here would be an unauthorized improvement and a fourth observable "
           "difference between the two back-ends";

    // A second close returns silently, as the legacy back-end does with its throw compiled out.
    EXPECT_NO_THROW({ Driver::getInstance().close(); })
        << "a close on an already-closed driver raised; the legacy back-end returns silently";
    EXPECT_EQ(fake->getCloseCallCount(), 1)
        << "the already-closed driver reached the HAL again, so the state guard is not short-"
           "circuiting the second close";
}

// A non-ok binder status on close is a different arm from a false result, and it must reach the
// same closed state.
//
// Both arms converge on one condition in DriverAidlImpl::close(), and covering
// only one of them would leave a short-circuit reordering - which would skip the CLOSED
// assignment on the transport arm - undetected.
/**
 * @brief The transport arm of the same close guard also reaches the closed state.
 * @pre Invocation B, with a failing close transaction installed on the fake.
 * @note Both arms converge on one condition and either can satisfy it alone, so covering only
 *       one would leave a short-circuit reordering - which would skip the state assignment on
 *       this arm - undetected.
 */
TEST_F(DriverAidlSessionTest, CloseWithNonOkBinderStatusAlsoReachesTheClosedState) {
    fake->setCloseBinderStatus(::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().close(); }, IOException)
        << "a close whose transaction failed did not raise IOException";

    // The controller is captured by the fake before the transaction status is applied, so the
    // identity is observable on this arm too - and a transport failure is precisely when a
    // mis-named session matters most, because nothing HAL-side has changed.
    {
        const ::android::sp<cechal::IHdmiCecController> closedController =
            fake->getLastClosedController();
        const ::android::sp<cechal::IHdmiCecController> openedController = fake->getController();

        ASSERT_NE(closedController.get(), nullptr)
            << "the HAL recorded no controller for a close whose transaction failed, so close() "
               "passed nothing identifiable";
        EXPECT_EQ(closedController.get(), openedController.get())
            << "the close named a different controller from the one open() returned, on the arm "
               "where the transaction itself failed";
    }

    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver still behaves as though it were open after a close whose transaction "
           "failed, although the false-result arm reaches CLOSED correctly. The two arms converge "
           "on one condition, so this is a short-circuit that skips the state assignment";
}

// A received CEC frame is delivered end to end while the driver is open - HAL event, adapter,
// incoming queue, Bus reader thread, Connection's filter, application listener - and the bytes
// that arrive are the bytes that were sent.
//
// Why the delivery and not just the trigger. FakeHdmiCecService::fireOnMessageReceived returns
// false only when no listener was captured; it reports nothing at all about what the listener
// then did with the message. So "the trigger returned true" is satisfied by an adapter that
// allocates the frame and drops it, by one that offers it onto a queue nothing drains, and by
// one whose state guard wrongly rejects it - three defects that would each break the receive
// path completely. What distinguishes them is whether an application listener is notified, so
// that is what is asserted, together with the exact bytes.
//
// The wait is bounded and is a condition variable, not a sleep. Delivery is asynchronous with
// respect to this case even here, because the Bus reader is a separate thread blocked in the
// queue's poll: the adapter offers on the calling thread and the reader wakes and notifies. A
// fixed sleep would be either flaky or wasteful and would assert nothing; the wait returns the
// moment the frame arrives and fails at the deadline.
//
// What this does not establish. In this process the callback itself runs on the calling thread,
// because an in-process fake resolves to the local BBinder and no transaction crosses the
// binder driver. So this case establishes the adapter's behaviour and the delivery chain above
// it - not that a callback arriving on a genuine binder threadpool thread is delivered. That
// is invocation E's, over real IPC, in hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp.
/**
 * @brief A received frame is delivered end to end while the driver is open, and the bytes that
 *        arrive are the bytes that were sent.
 * @pre Invocation B, with the session open and the listener captured by SetUp's cycle.
 * @note The delivery is asserted rather than the trigger's return value: the trigger reports only
 *       whether a listener was captured, so "it returned true" is satisfied by an adapter that
 *       logs and discards. Every step between is load-bearing, and a bounded wait is used so the
 *       case ends the moment the frame arrives and fails at the deadline.
 * @note What it does not establish: in this process the callback runs on the calling thread,
 *       because an in-process fake resolves to the local BBinder and no transaction crosses the
 *       driver. Delivery on a genuine binder threadpool thread is invocation E's, over real IPC.
 */
TEST_F(DriverAidlSessionTest, ReceivedMessageIsAcceptedWhileTheDriverIsOpen) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-open",
                                            applicationListener);

    // A well-formed broadcast Report Physical Address, with a distinctive address so that the
    // frame the listener sees cannot be confused with any other frame in this process.
    const std::vector<uint8_t> message{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so the middleware did not hand "
           "one to IHdmiCec::open() and no callback can ever arrive";

    ASSERT_TRUE(fake->fireOnMessageReceived(message))
        << "no listener was captured, so the trigger was a silent no-op and this case would prove "
           "nothing. SetUp cycles the session precisely so that open() re-captures it";

    ASSERT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "the frame the HAL delivered never reached an application listener within "
        << kFrameDeliveryTimeoutMs
        << " ms, although the trigger reached the middleware's callback. Every step between is "
           "load-bearing: the callback must copy the bytes into a fresh CECFrame and offer it "
           "through the state-guarded incoming-queue accessor, and the Bus reader must be blocked "
           "on that queue rather than on anything else. A callback that logs and returns, or that "
           "offers onto a queue nothing drains, passes every weaker assertion and breaks the "
           "entire receive path";

    const std::vector<uint8_t> delivered = applicationListener.frameAt(0);

    EXPECT_EQ(delivered, message)
        << "the delivered frame is not the frame that was sent, so the byte-array marshalling is "
           "wrong - a truncation, an off-by-one on the length, or a stale buffer. Delivered "
        << delivered.size() << " bytes against " << message.size() << " sent";

    EXPECT_EQ(applicationListener.frameCount(), 1u)
        << "one HAL callback produced " << applicationListener.frameCount()
        << " deliveries, so the frame is being offered more than once or the queue is replaying "
           "it";
}

// AN UNDER-LENGTH MESSAGE IS DISCARDED BEFORE ANYTHING IS ALLOCATED, and the reason this arm is
// worth a case of its own is that the alternative is not a dropped frame but a dead process.
//
// A CECFrame carrying no bytes cannot be decoded: the first thing every consumer does is read
// byte zero for the header nibbles, and CECFrame::at() raises std::out_of_range when there is no
// byte zero to read. Neither printFrameDetails() nor Bus::Reader::run() catches that, so it
// escapes the reader's thread function and takes the process with it. The guard therefore has to
// sit ahead of the allocation and ahead of the queue, and an empty payload is the only input that
// reaches it - no canned status, no binder failure and no state can.
//
// Why it could not have been covered before. Nothing in the middleware ever produces a zero-byte
// message; only a HAL can, which is exactly why the guard exists and exactly why a fake is the
// only way to drive it. On a host with no binder driver the AIDL back-end is never selected and
// this callback never runs at all, so the arm went unmeasured on every driverless run - and the
// runs that DO select the AIDL back-end had no case delivering an empty payload.
//
// Three things are asserted, and the second is the one that separates "discarded" from "queued
// and undecodable":
//
//   1. nothing escapes into the oneway callback. Letting an exception reach onTransact would be
//      worse than dropping the frame, which is the disposition the legacy callback also takes.
//
//   2. no frame is delivered, waited for rather than checked at one instant. A zero-byte frame
//      that had been queued would be drained by the reader and would raise inside it, so a
//      bounded non-delivery window is the observation that the queue never received it.
//
//   3. the delivery chain is alive throughout, established by a positive control after the
//      empty payload. Without it a run where the whole receive path was dead would read as a
//      correct discard.
/**
 * @brief A zero-byte message from the HAL is discarded before allocation, and the receive path
 *        keeps working afterwards.
 * @pre Invocation B, with the AIDL back-end resolved and a session open.
 * @note The guard runs ahead of the allocation and ahead of the queue because a zero-byte
 *       CECFrame raises std::out_of_range in the Bus reader, which nothing on that thread
 *       catches - so the alternative to discarding is losing the process, not losing a frame.
 * @note The positive control after the empty payload is load-bearing: a dead receive chain would
 *       otherwise be indistinguishable from a correct discard.
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 */
TEST_F(DriverAidlSessionTest, AZeroByteMessageFromTheHalIsDiscardedBeforeAllocation) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-too-short",
                                            applicationListener);

    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so no callback can arrive and "
           "this case would prove nothing";

    // The empty payload. fireOnMessageReceived reports only whether a listener was captured, so a
    // true return here says the callback ran - it says nothing about what the callback did, which
    // is what the assertions below are for.
    ASSERT_TRUE(fake->fireOnMessageReceived(std::vector<uint8_t>()))
        << "no listener was captured, so the trigger was a silent no-op";

    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "a zero-byte message reached an application listener. It was therefore allocated as a "
           "CECFrame and queued, and the Bus reader drained it - which means the next consumer to "
           "read byte zero of an empty frame raises std::out_of_range on the reader thread, where "
           "nothing catches it. The guard has to discard ahead of the allocation, not after it";

    EXPECT_EQ(applicationListener.frameCount(), 0u)
        << "the listener holds " << applicationListener.frameCount()
        << " frame(s) after an empty delivery, where none was expected";

    // The positive control: the same chain, one byte longer, must deliver. This is what makes the
    // non-delivery above evidence about the guard rather than about a broken receive path.
    const std::vector<uint8_t> wellFormed{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    ASSERT_TRUE(fake->fireOnMessageReceived(wellFormed))
        << "the positive control could not be triggered, so the discard above is unattributable";

    ASSERT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "a well-formed frame delivered immediately after the empty one never arrived, so the "
           "receive path was dead for the whole case and the non-delivery above says nothing "
           "about the length guard";

    EXPECT_EQ(applicationListener.frameAt(0), wellFormed)
        << "the frame that did arrive is not the well-formed one, so the empty payload disturbed "
           "the marshalling of the frame after it";
}

// THE OTHER ARM OF THE SAME LENGTH GUARD - AN OVER-LENGTH MESSAGE - AND IT IS THE ARM WHERE THE
// TWO BACK-ENDS DIFFER IN CONSEQUENCE AND NOT MERELY IN DEGREE.
//
// A message longer than a CECFrame can hold cannot be copied into one at all: CECFrame::append()
// takes the bytes one at a time and raises std::out_of_range("Frame grows beyond maximum") on the
// byte past the capacity (CECFrame.cpp:54-64). Where that append sits decides what happens next,
// and the two back-ends put it in different places. On the legacy back-end it is OUTSIDE
// DriverImpl::DriverReceiveCallback's try block (DriverImpl.cpp:60), so the exception escapes the
// HAL's callback and takes the process down - measured, and measured identically on the
// pre-migration base, so it is a pre-existing condition of that back-end and out of scope to fix
// here. This back-end is written so that it cannot happen: the allocation and the append are both
// inside the try, and the general catch arm deletes the frame and returns binder::Status::ok().
// Containment is therefore a deliberate property of the new code rather than a byproduct of it,
// and a deliberate property nothing exercises is a property nobody knows still holds.
//
// Why it could not have been covered before. The same reason as the under-length arm: nothing in
// the middleware can produce a message this long - a CECFrame cannot hold one, and the outbound
// guard refuses anything past the AIDL contract's length before a transmit is attempted - so only
// a HAL can deliver one, and only a fake can stand in for a HAL. On a host with no binder driver
// this back-end is never selected and this callback never runs at all.
//
// ONE CASE OVER TWO SIZES rather than two cases, and not a parameterized fixture. 129 and 4096 are
// two points on ONE arm, not two arms, so the reasoning above is stated once and SCOPED_TRACE
// names whichever size failed; the transmit half of this same guard already has exactly this shape
// (FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated loops over its two over-limit sizes
// in a single case); and nothing in this file is value-parameterized, so a TEST_P would introduce
// machinery for two rows. The one thing a loop risks that two cases would not - a fatal assertion
// returning early and leaving the second size unmeasured - is removed by making every per-size
// precondition non-fatal, so each size reports its own outcome whatever the other one did.
//
// THE SIZES ARE DERIVED FROM CECFrame::MAX_LENGTH AND NOT RESTATED. One byte past the capacity is
// the boundary whatever the capacity becomes, and a multiple of it stays comfortably past the
// boundary for the same reason; a case that restated 129 would silently stop testing the boundary
// the day the capacity moved. The static_assert on MAX_LENGTH near the top of this file would fire
// on such a change, but it guards a different question - the 16/17/20 authority conflict on the
// transmit side, and its text sends the reader to those three sizes - so leaning on it here would
// tie this case to an assertion that is not about it, for no saving.
//
// Four things are asserted per size, and the first is the one that separates this back-end from
// the legacy one:
//
//   1. nothing escapes the callback. The fake invokes the listener directly and catches nothing
//      (fake_hdmi_cec_aidl_service.cpp:1063-1085), and under this invocation the fake is
//      in-process, so the dispatch is local and anything escaping onMessageReceived arrives in
//      this case body. EXPECT_NO_THROW is a real observation here rather than a formality: it is
//      the property the legacy back-end does not have on this input, and losing it would not be a
//      dropped frame but a dead process.
//
//   2. no frame is delivered, waited for over a window rather than checked at one instant. The
//      append copies MAX_LENGTH bytes before it raises, so what is being ruled out is not an
//      empty frame but a silently TRUNCATED one - and a truncated frame is a perfectly ordinary
//      frame to every consumer above the queue, which would hand it to the application as though
//      the HAL had sent it. A peer can read a truncated CEC frame as a different message entirely.
//
//   3. the listener holds nothing afterwards, which is what catches a frame that surfaced after
//      the window closed rather than not at all.
//
//   4. the delivery chain is alive, established by a positive control after each over-length
//      payload. The control carries the same header byte as the payload, and this connection
//      filters nothing whatever - DefaultFilter::isFiltered returns false outright for an
//      UNREGISTERED source (Connection.cpp:304-306) - so the two deliveries differ in exactly one
//      property, which is length. Without the control, a receive path that was dead for the whole
//      case would read as correct containment.
//
// The middleware's own log line is deliberately NOT asserted, unlike the queue-refusal case below
// where the log is the only trace the arm leaves. Here the arm emits the general catch arm's line,
// which an allocation failure emits too, so it does not discriminate this arm; and a future change
// that moved the rejection ahead of the allocation would change that text while every property
// asserted above stayed true. The containment is what this case is about.
/**
 * @brief An over-length message from the HAL is contained: nothing escapes the callback, no frame
 *        reaches an application listener, and the receive path keeps working afterwards.
 * @pre Invocation B, with the AIDL back-end resolved and a session open.
 * @note Two sizes in one case - one byte past CECFrame::MAX_LENGTH and a multiple of it, both
 *       derived rather than restated so that neither stops exercising the arm if the capacity
 *       changes. SCOPED_TRACE names whichever size failed, and every per-size precondition is
 *       non-fatal so one size cannot leave the other unmeasured.
 * @note CECFrame::append() copies MAX_LENGTH bytes before it raises, so what the non-delivery
 *       assertions rule out is a silently TRUNCATED frame reaching the application, not an empty
 *       one. On the legacy back-end the same input is fatal, because there the append sits outside
 *       the try (DriverImpl.cpp:60) and the exception escapes the HAL's callback.
 * @note The positive control after each payload is load-bearing: a dead receive chain would
 *       otherwise be indistinguishable from correct containment.
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 * @see CECFrame::append()
 */
TEST_F(DriverAidlSessionTest, AnOverLengthMessageFromTheHalIsDiscardedWithoutEscaping) {
    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so no callback can arrive and "
           "this case would prove nothing";

    // Derived, never restated. One byte past the capacity is the boundary itself, which is the
    // only size an off-by-one in the copy would be visible at; thirty-two times the capacity is
    // the arbitrarily large delivery an out-of-process HAL is free to make, and is the size class
    // an ad-hoc guest harness demonstrated containment at once without leaving a guard behind.
    constexpr size_t frameCapacity = static_cast<size_t>(CECFrame::MAX_LENGTH);
    const size_t overLength[] = { frameCapacity + 1, frameCapacity * 32 };

    // A well-formed broadcast Report Physical Address. It serves twice: as the positive control,
    // and as the first bytes of each over-length payload, so that the payload and the control
    // differ in length and in nothing else.
    const std::vector<uint8_t> wellFormed{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    for (size_t index = 0; index < sizeof(overLength) / sizeof(overLength[0]); index++) {
        const size_t size = overLength[index];

        SCOPED_TRACE("inbound message of " + std::to_string(size) + " bytes, against a CECFrame "
                     "capacity of " + std::to_string(frameCapacity) + " bytes");

        // A fresh listener and connection per size, so each size's counts start from zero and a
        // frame delivered under one size can never be read as a frame delivered under the other.
        RecordingFrameListener applicationListener;
        ListeningConnection listeningConnection(
            LogicalAddress(LogicalAddress::UNREGISTERED),
            "DriverAidlSessionTest-receive-too-long-" + std::to_string(size),
            applicationListener);

        // The payload: the control's bytes, then padding out to the target size. The first
        // frameCapacity bytes are therefore a frame the whole chain above the queue would accept,
        // which is what makes a truncated delivery detectable rather than plausible.
        std::vector<uint8_t> oversized(wellFormed);
        oversized.resize(size, 0x00);

        bool triggered = false;
        EXPECT_NO_THROW({ triggered = fake->fireOnMessageReceived(oversized); })
            << "an exception escaped the middleware's oneway callback and reached the HAL's "
               "delivery. The fake invokes the listener directly and catches nothing, and this "
               "invocation's fake is in-process, so what surfaced here is what would escape into "
               "onTransact on a real transport - and it is exactly how the legacy back-end loses "
               "the process on this same input. The allocation and the append must both stay "
               "inside the try, and every catch arm must release the frame and return ok";

        if (!triggered) {
            ADD_FAILURE()
                << "the over-length delivery never reached the middleware's callback: either no "
                   "listener was captured, or the callback threw and the assertion above has "
                   "already said so. Either way nothing below would be attributable to the length "
                   "guard, so this size is unmeasured rather than passing";
            continue;
        }

        EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
            << "a message too long for a CECFrame reached an application listener. append() copies "
               "the first "
            << frameCapacity
            << " bytes before it raises, so what arrived is the TRUNCATED remains of the message - "
               "indistinguishable, to every consumer above the queue, from a frame the HAL really "
               "sent, and readable by a peer as a different message entirely. The frame must be "
               "released on the catch arm and never offered onto the queue";

        EXPECT_EQ(applicationListener.frameCount(), 0u)
            << "the listener holds " << applicationListener.frameCount()
            << " frame(s) after an over-length delivery, where none was expected - so a frame "
               "surfaced once the non-delivery window had closed rather than not at all";

        // The positive control: the same chain, a length it can hold, must deliver.
        bool controlTriggered = false;
        EXPECT_NO_THROW({ controlTriggered = fake->fireOnMessageReceived(wellFormed); })
            << "the positive control itself raised, so the over-length payload left the callback "
               "or the captured listener in a state the next delivery could not survive";

        if (!controlTriggered) {
            ADD_FAILURE()
                << "the positive control could not be triggered, so the non-delivery above is "
                   "unattributable: a receive path that was dead for this whole size reads exactly "
                   "like correct containment";
            continue;
        }

        EXPECT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
            << "a well-formed frame delivered immediately after the over-length one never arrived "
               "within "
            << kFrameDeliveryTimeoutMs
            << " ms, so the receive path was dead for this size and the non-delivery above says "
               "nothing about the length guard. The chain has to survive a std::out_of_range "
               "caught mid-append: a frame released on the catch arm must leave the owner lock, "
               "the incoming queue and the Bus reader exactly as they were";

        EXPECT_EQ(applicationListener.frameAt(0), wellFormed)
            << "the frame that did arrive is not the well-formed one, so the over-length payload "
               "was not discarded whole - its truncated remains were delivered ahead of the "
               "control, or they disturbed the marshalling of the frame after it";

        EXPECT_EQ(applicationListener.frameCount(), 1u)
            << "the listener holds " << applicationListener.frameCount()
            << " frames after one over-length delivery and one well-formed one, so the over-length "
               "payload was delivered too - late rather than never, which is the same defect with "
               "a delay in front of it";
    }
}

// THE QUEUE'S REFUSAL ARM, WHICH IS WHAT STOPS A STALLED READER TURNING EVERY FURTHER EVENT INTO
// A LEAKED FRAME.
//
// EventQueue::offer() returns void and silently discards its argument once the queue is full, so
// a callback that offered and then dropped its pointer would leak one heap CECFrame per event,
// without bound, for as long as the reader stayed behind. offerReceivedFrame() exists to report
// the refusal instead, and the callback releases the frame when it is refused. That release is
// the arm here.
//
// Making the queue fill is the whole difficulty, and it is why this case needs a listener that
// blocks. The reader is normally parked in EventQueue::poll() on an empty queue and woken by each
// offer, so it drains as fast as frames arrive and the occupancy never leaves one. Parking it
// inside a notification instead - on its own thread, in its own call graph, with nothing injected
// into the queue - is what produces the real condition: frames offered from that moment on
// accumulate exactly as they would behind a slow application.
//
// The numbers are the production ones and are not restated as literals here. The queue's capacity
// reserves its last slot for close()'s wake-the-reader sentinel, so the refusal point is one
// entry below capacity; the case delivers comfortably more than capacity so that the arm is
// reached whatever those two constants are, and asserts on the middleware's own refusal line
// rather than on an occupancy it would have to compute.
//
// The refusal is observed in the middleware's log because a released allocation has no other
// externally visible trace - asserting the absence of a leak would need a heap harness this suite
// does not have. The reader is released BEFORE the assertions, so an ASSERT_* returning early can
// never strand it and hang the teardown.
/**
 * @brief A frame arriving while the incoming queue is at its refusal point is released rather
 *        than leaked, and the receive path recovers once the reader moves again.
 * @pre Invocation B, with the AIDL back-end resolved and a session open.
 * @note The reader is parked inside a blocking listener on its own thread, which is the only way
 *       the queue can reach its refusal point at all: it otherwise drains as fast as frames are
 *       offered and the occupancy never exceeds one.
 * @note Asserted on the middleware's own refusal line, not on an occupancy: the capacity and the
 *       reserved sentinel slot are production constants and a test that restated them would
 *       silently stop testing the arm if either changed.
 * @note The reader is released before the assertions so that a fatal assertion cannot strand it.
 * @see DriverAidlImpl::offerReceivedFrame()
 */
TEST_F(DriverAidlSessionTest, AFrameArrivingWhileTheQueueIsFullIsReleasedRatherThanLeaked) {
    StallingFrameListener stallingListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-queue-full",
                                            stallingListener);

    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake holds no listener although open() completed, so no callback can arrive";

    const std::vector<uint8_t> filler{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x21, 0x00, 0x04 };

    // One frame, to get the reader out of poll() and into the notification where it will stay.
    ASSERT_TRUE(fake->fireOnMessageReceived(filler))
        << "no listener was captured, so the trigger was a silent no-op";

    if (!stallingListener.waitUntilParked(kFrameDeliveryTimeoutMs)) {
        stallingListener.release();

        FAIL() << "the Bus reader never reached the listener within " << kFrameDeliveryTimeoutMs
               << " ms, so it was never parked and the queue could not have filled. That is a "
                  "broken receive path rather than a queue that refused nothing, and the two must "
                  "not share a failure message";
    }

    // Comfortably past the refusal point, whatever the capacity and the reserved slot are. Every
    // delivery from here is offered onto a queue nothing is draining.
    const size_t deliveriesWhileParked = 64;
    size_t triggered = 0;
    std::string logged;
    bool captureWasValid = false;

    {
        StdoutCapture capture;

        // Sampled HERE, before anything reads the capture, because StdoutCapture::read() calls
        // restore() and a restored capture reports itself invalid from then on. Asking isValid()
        // after read() therefore always answers false and says nothing about whether the
        // redirection ever worked - a trap this case fell into and the guest caught.
        captureWasValid = capture.isValid();

        for (size_t index = 0; index < deliveriesWhileParked; index++) {
            if (fake->fireOnMessageReceived(filler)) {
                triggered++;
            }
        }

        // Safe when the redirection failed: read() returns an empty string rather than raising,
        // and the assertion on captureWasValid below is what reports that case.
        logged = capture.read();
    }

    // Released before ANY assertion below, so a fatal one cannot leave the Bus reader parked
    // inside the listener and hang this fixture's teardown.
    stallingListener.release();

    ASSERT_TRUE(captureWasValid)
        << "stdout could not be redirected, so the middleware's own refusal line - the only "
           "externally visible trace a released allocation leaves - could not be read";

    EXPECT_EQ(triggered, deliveriesWhileParked)
        << "only " << triggered << " of " << deliveriesWhileParked
        << " deliveries reached the middleware's callback, so the listener was lost part way "
           "through and the queue was not driven to its refusal point";

    // Two substrings of the production line rather than one token, and neither is a function
    // name: the line names the CONDITION and the DISPOSITION, and both are what a log reader
    // acts on. Asserting the refusal alone would pass on a line that reported a refusal and said
    // nothing about the frame, which is the defect that matters - a refused frame the callback
    // did not release is a leak per event, and the log is the only trace it leaves.
    EXPECT_THAT(logged, ::testing::HasSubstr("refused the frame at its"))
        << "none of " << deliveriesWhileParked
        << " frames delivered onto a queue nothing was draining was refused. Either the queue "
           "accepted every one of them - in which case EventQueue::offer() discarded the "
           "surplus silently and each discarded frame is a leak - or the refusal happened and "
           "went unreported, which is the same defect from a log reader's point of view";

    EXPECT_THAT(logged, ::testing::HasSubstr("rather than leaking it"))
        << "the queue reported a refusal but the line does not say the frame was released, so "
           "either the callback kept a frame the queue never took - one leaked heap CECFrame per "
           "refused event, without bound while the reader stays behind - or the disposition went "
           "unrecorded, and a reader has no way to tell those two apart";

    // The chain recovers. A refusal that also broke the receive path would be a different defect
    // from a refusal that did not, and this is what distinguishes them.
    RecordingFrameListener recoveredListener;
    ListeningConnection recoveredConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-queue-recovered",
                                            recoveredListener);

    ASSERT_TRUE(fake->fireOnMessageReceived(filler))
        << "the recovery delivery could not be triggered";

    EXPECT_TRUE(recoveredListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "no frame arrived after the reader was released, so the refusal left the receive path "
           "wedged rather than merely dropping the frames it could not take";
}

// THE SLOW-CALL DIAGNOSTIC, WHICH IS A THRESHOLD AND NOT A TIMEOUT, and the distinction is the
// whole point of the case.
//
// Crossing it abandons nothing, raises nothing and changes no return value: it leaves one
// LOG_WARN line where a stall would otherwise be completely silent. B4 records that the
// underlying synchronous binder call cannot be bounded at all on the pinned libbinder, so this
// line is the entire mitigation that is in scope, and a mitigation nothing exercises is a
// mitigation nobody knows works.
//
// It cannot be reached by any canned result or status, because none of them takes time. Only a
// call that is genuinely slow reaches it, which is why the fake gained a delay knob rather than a
// flag - and why this case pays real wall-clock time. The delay is set just past the threshold
// rather than comfortably past it, so the case costs the least it can while still crossing.
//
// What is asserted, and what deliberately is not. The warning must appear, and the call must
// still succeed unchanged - a diagnostic that altered the outcome it describes would be a defect
// far worse than a missing log line. The elapsed figure in the line is NOT asserted: it is a real
// measurement on a shared machine and pinning it would make the case flaky without testing
// anything the presence of the line does not already establish.
/**
 * @brief A synchronous AIDL call that outlives the slow-call threshold is reported, and its
 *        result is unaffected.
 * @pre Invocation B, with the AIDL back-end resolved and a session open. Costs real wall-clock
 *      time, just past the production threshold.
 * @note A threshold and not a timeout: nothing is abandoned and nothing raises. The line is the
 *       whole of the in-scope mitigation for an unbounded synchronous call, so it is asserted
 *       directly rather than inferred.
 * @note The elapsed figure in the line is deliberately not asserted - it is a real measurement on
 *       a shared machine, and pinning it would buy flakiness rather than evidence.
 * @see DriverAidlImpl::addLogicalAddress()
 * @see FakeHdmiCecController::setAddLogicalAddressesDelayMs()
 */
TEST_F(DriverAidlSessionTest, ASynchronousCallPastTheSlowThresholdIsReportedWithoutChangingItsResult) {
    ::android::sp<FakeHdmiCecController> controllerFake = fake->getController();

    ASSERT_NE(controllerFake, nullptr)
        << "the controller fake is not reachable, so no delay can be installed and this case "
           "cannot make a call slow";

    // Just past the production threshold. The production constant lives in the anonymous
    // namespace at DriverAidlImpl.cpp:185 and has no dynamic symbol, so it cannot be read from
    // here and is restated in kSlowHalCallWarnMs instead. That restatement is safe in the only
    // direction that matters: if production ever raised its threshold above this value, the
    // delay would fall short, no line would be emitted and the assertion below would FAIL - a
    // loud wrong answer rather than a case that had quietly stopped testing the arm.
    const int32_t delayMs = static_cast<int32_t>(kSlowHalCallWarnMs) + 150;

    controllerFake->setAddLogicalAddressesDelayMs(delayMs);
    controllerFake->setAddLogicalAddressesResult(true);

    std::string logged;
    int addResult = -1;
    bool captureWasValid = false;

    {
        StdoutCapture capture;

        // Sampled before the read, for the reason recorded in the queue-refusal case above:
        // StdoutCapture::read() restores fd 1 and the capture reports itself invalid afterwards,
        // so an isValid() asked after read() answers false whatever the redirection did.
        captureWasValid = capture.isValid();

        addResult = Driver::getInstance().addLogicalAddress(
            LogicalAddress(LogicalAddress::PLAYBACK_DEVICE_1));

        logged = capture.read();
    }

    // Cleared immediately, so no later case in this fixture inherits the delay.
    controllerFake->setAddLogicalAddressesDelayMs(0);

    ASSERT_TRUE(captureWasValid)
        << "stdout could not be redirected, so the diagnostic line could not be read";

    EXPECT_EQ(addResult, 1)
        << "a call that crossed the slow-call threshold returned " << addResult
        << " where success was expected. The diagnostic is a threshold and not a timeout: it must "
           "leave the outcome, the status translation and the return value entirely alone";

    EXPECT_THAT(logged, ::testing::HasSubstr("past the"))
        << "a synchronous AIDL call that took " << delayMs << " ms, past the "
        << kSlowHalCallWarnMs
        << " ms threshold, produced no diagnostic line. That line is the whole of the in-scope "
           "mitigation for a call the pinned libbinder cannot bound, so its absence means a "
           "stalling HAL leaves no trace at all";

    EXPECT_THAT(logged, ::testing::HasSubstr("NO DEADLINE WAS ENFORCED"))
        << "the diagnostic line does not state that nothing was enforced, so a log reader could "
           "mistake it for evidence of a mitigation that abandoned or bounded the call";

    EXPECT_THAT(logged, ::testing::HasSubstr("addLogicalAddresses"))
        << "the diagnostic line does not name the operation that was slow, so it cannot be acted "
           "on: a reader learns that something stalled but not what";
}

// The closing-state rejection, which is the arm that stops a frame leaking per rejected
// message - and the arm that stops a frame arriving too late being delivered to an application
// that has torn its session down.
//
// A frame arriving while the driver is not OPENED must be refused and released, not queued. The
// adapter reaches the queue through a state-guarded accessor that raises when the state is not
// OPENED, exactly as the legacy receive callback does, and the listener catches that and frees
// the allocation. Offering directly to the queue member instead would accept frames the legacy
// path rejects and would leave them in a queue nothing will ever drain until the next open.
//
// Three things are asserted, and the third is what makes this more than a smoke test:
//
//   1. nothing escapes into the binder callback. A oneway callback has no caller to receive a
//      fault, so letting an exception reach onTransact would be worse than dropping one frame -
//      which is the disposition the legacy callback already takes.
//
//   2. the frame is not delivered while the driver is closed, waited for and not merely
//      assumed. A bounded non-delivery window rather than an instantaneous check, because
//      "it had not arrived yet" and "it will never arrive" are different claims.
//
//   3. The frame does not surface after a later re-open. This is the assertion that separates
//      "rejected and released" from "quietly queued": a frame wrongly offered during the closed
//      window would sit in the queue behind the close sentinel, and re-opening re-arms the Bus
//      reader, which would then drain and deliver it - to a listener whose application believes
//      the session was down when it arrived. Nothing about a closed-state check alone can
//      detect that, which is why the re-open is here.
//
// The release itself is observed in the middleware's own log, at a level the default
// configuration prints, because a released allocation has no other externally visible trace -
// asserting on the absence of a leak would need a heap harness this suite does not have.
/**
 * @brief A frame arriving while the driver is not open is refused and released rather than
 *        queued, and does not surface after a later re-open.
 * @pre Invocation B. The release is observed in the middleware's own log, because a released
 *      allocation has no other externally visible trace.
 * @note Three things are asserted and the third is what makes this more than a smoke test:
 *       nothing escapes into the oneway callback; the frame is not delivered during a bounded
 *       window rather than at one instant, because "it had not arrived yet" and "it will never
 *       arrive" are different claims; and it does not surface after a re-open, which is what
 *       separates "rejected and released" from "quietly queued".
 * @note The re-open also carries a positive control, so a run where the delivery chain was dead
 *       throughout cannot be read as a correct rejection.
 */
TEST_F(DriverAidlSessionTest, MessageArrivingWhileClosedIsRejectedAndReleased) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-receive-closed",
                                            applicationListener);

    ASSERT_NO_THROW({ Driver::getInstance().close(); })
        << "the driver could not be closed, so the rejecting-queue precondition does not hold";

    // A distinctive frame, distinct from the open-state case's, so that a delivery seen here
    // cannot be a frame left over from anywhere else.
    const std::vector<uint8_t> message{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x31, 0x00, 0x04 };

    // The listener is still registered with the fake - close() withdraws the middleware's own
    // attachment but the fake retains the binder reference, which is exactly the real hazard
    // being modelled: a HAL that calls back after the session went down.
    ASSERT_NE(fake->getListener(), nullptr)
        << "the fake no longer holds a listener after close(), so this trigger cannot reach the "
           "adapter and the rejection path would not be exercised at all";

    std::string captured;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid()) << "stdout could not be captured, so the release of the "
                                         "rejected frame cannot be observed";

        EXPECT_TRUE(fake->fireOnMessageReceived(message))
            << "the trigger did not reach the listener, so the rejection path was never exercised";

        captured = capture.read();
    }

    // The rejection is visible in one of exactly two forms, and both are correct outcomes of the
    // same design: the accessor raised and the listener released the frame, or the listener had
    // already been detached by close() and dropped it before allocating anything at all. What
    // must not happen is silence, which would mean the frame went into the queue.
    const bool releasedAfterGuardRaised =
        captured.find("Exception during frame offer...discarding") != std::string::npos;
    const bool droppedBecauseDetached =
        captured.find("message received after detach, dropping it") != std::string::npos;

    EXPECT_TRUE(releasedAfterGuardRaised || droppedBecauseDetached)
        << "the middleware logged neither of the two dispositions a frame arriving on a "
           "non-OPENED driver may have - the state-guarded accessor raising and the listener "
           "releasing the allocation, or the listener finding itself detached and dropping the "
           "message. Silence here means the frame was ACCEPTED while the driver was closed, which "
           "is both a divergence from the legacy path and a frame queued where nothing will drain "
           "it. Captured output was: [" << captured << "]";

    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "a frame that arrived while the driver was CLOSED was delivered to an application "
           "listener. The state guard exists so that a late HAL callback cannot reach an "
           "application that has torn its session down";

    // Now the strong half: re-arm the driver and give the Bus reader a generous window. If the
    // frame had been queued rather than released, this is where it would appear.
    ASSERT_NO_THROW({ Driver::getInstance().open(); })
        << "the driver could not be re-opened, so the deferred-delivery half of this case cannot "
           "be established";

    EXPECT_FALSE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "the frame that arrived while the driver was closed was delivered after the re-open, so "
           "it was queued rather than released. Every closed-state assertion above still passed, "
           "which is exactly why this one is here: the frame was merely deferred, and it reached "
           "the application on a session that had nothing to do with it";

    EXPECT_EQ(applicationListener.frameCount(), 0u)
        << "the application listener received " << applicationListener.frameCount()
        << " frames across the closed window and the re-open, and it must receive none";

    // A positive control, so that a green result cannot be explained by the delivery chain being
    // dead for some unrelated reason - a withdrawn listener, a stopped Bus reader, a filter that
    // rejects everything. The re-opened session must still deliver.
    const std::vector<uint8_t> afterReopen{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x41, 0x00, 0x04 };
    ASSERT_TRUE(fake->fireOnMessageReceived(afterReopen))
        << "no listener was captured by the re-open, so the positive control cannot run";

    EXPECT_TRUE(applicationListener.waitForFrames(1, kFrameDeliveryTimeoutMs))
        << "the re-opened session did not deliver either, so the delivery chain was dead "
           "throughout and the non-delivery assertions above prove nothing about the state guard. "
           "This control is what distinguishes 'the frame was correctly rejected' from 'no frame "
           "could have been delivered anyway'";

    EXPECT_EQ(applicationListener.frameAt(0), afterReopen)
        << "the frame delivered after the re-open is not the one that was sent then, so the "
           "closed-window frame surfaced after all";

    // TearDown restores the opened baseline; nothing here is left closed for a sibling suite.
}

// The two diagnostic callbacks report what they were told and do nothing more, which is a
// behavioural claim in two halves - what is logged, and what is not done.
//
// onStateChanged and onMessageSent are logged and acted on in no other way, deliberately: there
// is no legacy counterpart to either, so acting on them would be new behaviour. In particular a
// transition to CLOSED does not offer the close sentinel onto the incoming queue - an in-process
// HAL cannot vanish, so the legacy path has no such notion - and no death recipient is installed
// for the same reason.
//
// "does nothing" is not the same as "is not implemented", and asserting only that a later
// transmit still works cannot tell the two apart: an empty callback body, or one that logged
// nothing, would satisfy it. The diagnostic is the behaviour here, so the diagnostic is what is
// asserted, from the middleware's own captured output.
//
// What each callback reports, and at which level, stated precisely because it decides what has
// to be arranged before anything can be observed:
//
//   DriverAidlImpl::EventListener::onStateChanged() logs at LOG_INFO, and cec_log_level defaults to
//   LOG_INFO, so its line prints on every run. Both state names go through the generated
//   toString(), so "started" and "CLOSED" are the literal text - and both are asserted.
//
//   DriverAidlImpl::EventListener::onMessageSent() logs at LOG_DEBUG, which the default level
//   suppresses, and its line carries three fields: the SendMessageStatus name through
//   toString(), the message length, and the message bytes. All three are asserted, because each
//   answers a different question a reader of a field log has - which outcome the bus reported,
//   and which of several in-flight messages it was about.
//
// The bytes are observable, and how they are rendered is worth stating because the expectation
// below has to reproduce it. Production renders them as lowercase hex, two digits per byte and no
// separator, into a stack buffer sized from CECFrame::MAX_LENGTH, with "..." appended when the
// message is longer than that buffer can hold - a bound rather than an allocation, because the
// callback runs on a binder thread. lowercaseHexOf() in this file reproduces exactly that
// rendering for the four-byte message used here, which is well inside the bound, so the
// assertion compares against one rendering rather than a hand-typed literal.
//
// So the level is raised here, through production's own reader. ScopedCecLogLevel writes a level
// name to /tmp/cec_log_enabled - the path check_cec_log_status() hardcodes - and calls that
// reader, which is the only seam the middleware offers; there is no setter for cec_log_level.
// That path is fixed by production and shared by every process on the host, so it is taken under
// the custody protocol the guard documents: an exclusive lock before anything is captured,
// classification that refuses a symlink or a foreign owner, and an atomic replace rather than a
// truncating write. The guard refuses rather than forces whenever the path is not safely this
// run's to modify.
//
// And the restoration is asserted too, which is the other half of taking custody of a shared
// path. restoreAndVerify() runs below on this case's normal path, while the lock is still held,
// and it proves three things instead of assuming them: that putting the level and the file back
// actually succeeded, every syscall result taken rather than discarded; that the bytes at the
// path are the captured original again, re-read without following a link, or that the path is
// absent again where the guard created it; and that the process-wide level is the one this case
// found, re-observed through the same probe that observed it on the way in. The third does not
// follow from the second - check_cec_log_status() silently leaves the level alone when the file's
// first line matches nothing in production's table - so "the file is back" would not have
// established it. The guard's destructor remains a backstop for the fatal assertion that unwinds
// past the call, and a backstop cannot substitute for it: a destructor cannot fail a test, so a
// failed write, rename or unlink there would leave DEBUG in force process-wide, or the shared
// file altered or missing, while every case in this binary still passed.
//
// And the raise is asserted, not reported. A refusal makes onMessageSent's line invisible, so if
// this case merely noted it and skipped the three assertions, a run that never obtained the
// evidence would be indistinguishable from a run in which the evidence held - which is the
// swallowed-failure shape this file exists to avoid. ASSERT_TRUE on the raise, with the guard's
// own reason streamed, is what keeps missing evidence from passing as evidence.
//
// The level-independent half is asserted as well, and it is the half that matters most: that
// neither callback puts anything on the receive path, and that the session survives both.
/**
 * @brief The two diagnostic callbacks report what they were told and do nothing more.
 * @pre Invocation B. The transmit report is emitted below the default level, so the level is
 *      raised through production's own reader under the log guard's custody protocol.
 * @note The claim is in two halves - what is logged, and what is not done - and asserting only
 *       that a later transmit still works cannot tell an implemented callback from an empty one.
 *       The diagnostic is the behaviour here, so the diagnostic is what is asserted, from the
 *       middleware's own captured output.
 * @note Acting on either callback would be new behaviour, since neither has a legacy
 *       counterpart: in particular a transition to closed does not offer the close sentinel, and
 *       no death recipient is installed.
 * @note All three transmit fields are asserted - status, length and bytes - because each answers
 *       a different question a reader of a field log has, and the length is matched with its
 *       label rather than as a bare digit, which a timestamped capture would satisfy trivially.
 */
TEST_F(DriverAidlSessionTest, DiagnosticCallbacksAreReportedWithoutDisturbingTheSession) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-diagnostics",
                                            applicationListener);

    const std::vector<uint8_t> sentMessage{ 0x40, GIVE_DEVICE_POWER_STATUS, 0x5A, 0xA5 };

    std::string captured;
    bool levelWasRaised = false;
    std::string levelRefusal;
    {
        // Raised before the capture opens, so neither the guard's own custody work nor the
        // level probe it runs can land inside the captured text and be mistaken for output
        // from a callback.
        //
        // The verdict and its reason are copied out here, while the guard is still alive: the
        // guard releases custody at the end of this block, so its own diagnosis has to be
        // taken before that happens if the assertion below is to name it.
        ScopedCecLogLevel debugLevel("DEBUG");
        levelWasRaised = debugLevel.isRaised();
        levelRefusal = debugLevel.failureReason();

        // The capture lives in its own, inner scope, and that is not tidiness. The restoration
        // below re-probes the effective level, which means emitting log lines through
        // production's own CCEC_LOG; with the capture still open those emissions would be
        // appended to the very text the assertions after this block read, and a probe marker
        // sitting in `captured` would be indistinguishable from something a callback reported.
        // Closing the capture first is what keeps the captured text the callbacks' own.
        {
            StdoutCapture capture;
            ASSERT_TRUE(capture.isValid())
                << "stdout could not be captured, so what the two diagnostic callbacks report "
                   "cannot be observed and this case would degenerate into 'the session still "
                   "works'";

            EXPECT_TRUE(fake->fireOnStateChanged(cechal::State::STARTED, cechal::State::CLOSED))
                << "the state-change trigger did not reach the listener";

            EXPECT_TRUE(fake->fireOnMessageSent(sentMessage,
                                                cechal::SendMessageStatus::ACK_STATE_0))
                << "the message-sent trigger did not reach the listener";

            captured = capture.read();
        }

        // Restoration is performed here, checked, and asserted - while the custody lock is
        // still held, and on the case's normal path rather than in a destructor.
        //
        // The guard's destructor is a backstop for the fatal assertion that unwinds past this
        // line, and it cannot report to the test: a destructor has no way to fail a case, so a
        // failed write, rename or unlink there would leave this host with DEBUG logging still
        // in force, or with the shared configuration file altered or missing, while every case
        // in this binary still reported success. That is the shape this call closes. What it
        // returns covers all three: that publishing the level and the file succeeded, that the
        // path holds the captured bytes again (or is absent again where the guard created it),
        // and that the process-wide level is the one this case found - the last of which a
        // restored file does not imply, because check_cec_log_status() leaves the level alone
        // when the file's first line matches nothing in production's table.
        std::string restoreDetail;
        ASSERT_TRUE(debugLevel.restoreAndVerify(restoreDetail)) << restoreDetail;
    }

    ASSERT_TRUE(levelWasRaised)
        << "the middleware log level could not be raised to DEBUG, so onMessageSent's report is "
           "suppressed and this case cannot assert it. The level is raised through production's "
           "own check_cec_log_status(), which reads /tmp/cec_log_enabled, and the guard refuses "
           "rather than forces whenever that path is not safely this run's to modify - so the "
           "refusal below distinguishes an ENVIRONMENT FAULT (the path is not writable, the "
           "directory is gone) from a CUSTODY REFUSAL (another run_L1Tests holds the lock, the "
           "path is a symlink or is owned by another user, a concurrent writer replaced it). "
           "Neither is a defect in the listener, and both are asserted rather than reported so "
           "that missing evidence cannot pass as evidence obtained. Reported reason: ["
        << levelRefusal << "]";

    // Unconditional: the state transition is reported, with both state names.
    EXPECT_NE(captured.find("HAL state changed from"), std::string::npos)
        << "onStateChanged reported nothing at all, although it logs at LOG_INFO and the default "
           "level prints LOG_INFO. Reporting the transition is this callback's whole behaviour, so "
           "silence here means an empty body - and the HAL's own view of its state would then be "
           "invisible in a field log, which is the one place it is ever needed. Captured output "
           "was: [" << captured << "]";

    EXPECT_NE(captured.find("STARTED"), std::string::npos)
        << "the OLD state was not named in the state-change report. Both names are needed: a "
           "report that says only where the HAL ended up cannot distinguish a transition from a "
           "repeated notification. Captured output was: [" << captured << "]";

    EXPECT_NE(captured.find("CLOSED"), std::string::npos)
        << "the new state was not named in the state-change report, so the report carries no "
           "information about what the HAL actually did. Captured output was: [" << captured << "]";

    // Unconditional, because the raise above was asserted rather than reported: with the level at
    // DEBUG, the transmit report must name the status, the length and the message bytes. All
    // three are asserted rather than any one of them, because each answers a different question a
    // reader of a field log has: which outcome the bus reported, and which of several in-flight
    // messages it was about. Asserting only the presence of the line would pass against a report
    // that named none of them.
    EXPECT_NE(captured.find("onMessageSent received"), std::string::npos)
        << "onMessageSent reported nothing at all with the level raised to DEBUG, although "
           "reporting is this callback's whole behaviour. An empty body would leave a transmit "
           "outcome the HAL observed invisible everywhere - write() acts on sendMessage()'s return "
           "value, not on this callback, so nothing else records it. Captured output was: ["
        << captured << "]";

    EXPECT_NE(captured.find(cechal::toString(cechal::SendMessageStatus::ACK_STATE_0)),
              std::string::npos)
        << "the transmit result was reported without naming the SendMessageStatus it carried, so "
           "the report cannot distinguish an acknowledged transmit from a rejected one - which is "
           "the only thing this callback exists to tell anybody. Captured output was: [" << captured
        << "]";

    // The length is matched with its label, composed from the message this case sent, because a
    // bare std::to_string(sentMessage.size()) is a single digit and every captured CCEC_LOG line
    // carries a timestamp - so that form was satisfied by any non-empty capture and could not
    // fail. The label is transcribed from the producer's format string, which reads
    // "... Result: %s, message length: %zu, message bytes: %s".
    const std::string expectedLengthReport =
        std::string("message length: ") + std::to_string(sentMessage.size());

    EXPECT_NE(captured.find(expectedLengthReport), std::string::npos)
        << "the transmit result was reported without the message length. Expected to find: ["
        << expectedLengthReport << "]. Captured output was: [" << captured << "]";

    EXPECT_NE(captured.find(lowercaseHexOf(sentMessage)), std::string::npos)
        << "the transmit report does not carry the message bytes (" << lowercaseHexOf(sentMessage)
        << "). A status and a length cannot tie a report to a message: two frames of the same "
           "length are indistinguishable, and with several transmits in flight the reader cannot "
           "tell which one the bus outcome belongs to. Captured output was: [" << captured << "]";

    // Level-independent, and the strongest half: neither callback put anything on the receive
    // path. A transition to CLOSED must not offer the sentinel, and a transmit report must not be
    // mistaken for an inbound frame - either would deliver to an application listener.
    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "one of the two diagnostic callbacks delivered a frame to an application listener. "
           "Neither carries an inbound message: onStateChanged must not offer the close sentinel - "
           "an in-process HAL cannot vanish, so the legacy path has no such notion - and "
           "onMessageSent reports a frame that has already gone OUT";

    // Still open, and still able to transmit: neither callback changed any state.
    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the driver is no longer usable after the two diagnostic callbacks. A transition to "
           "CLOSED must not close the middleware's own session - reacting to it would be new "
           "behaviour with no legacy counterpart";
}

// The threadpool obligation is discharged idempotently, including when a pool was already
// started elsewhere in the process - which is the case a naive back-end-local flag would break.
//
// DriverAidlImpl::open() calls ProcessState::self()->startThreadPool() unconditionally
// in DriverAidlImpl::open(). That is deliberately the whole of the obligation: the call is
// idempotent, and setThreadPoolMaxThreadCount is not called because no back-end-local flag can
// detect a pool started by another component and lowering an already-established maximum can
// abort the process.
//
// By the time this case runs, a pool has certainly been started - SetUp's open() did it, and so
// did the original open during LibCCEC::init - so the already-started condition is the one being
// exercised, and a second open must still succeed and callbacks must still arrive. The
// never-started condition is covered by the process's first open, inside LibCCEC::init: a
// failure there would abort initialization and no case in this binary would run at all.
//
// What this case cannot establish from the frames alone, and how it establishes it instead. Every
// other assertion here would hold against a back-end that never called startThreadPool() at all,
// because an in-process fake resolves to the local BBinder and its trigger runs the callback on
// the calling thread - so frames arrive whether or not a pool was ever started, and the omission
// would only surface on a real device, where nothing would ever be received. The pool's existence
// therefore needs an observation from outside the frames, and of the two that inspect the process
// from the OUTSIDE, neither is sound on this SDK:
//
//   - By thread name. ProcessState::makeBinderThreadName() composes "binder:PID_N", but
//     androidCreateRawThreadEtc() discards it - its threadName parameter is `__android_unused`
//     and the block that would apply it, along with the only androidSetThreadName() call site for
//     a spawned thread, is `#if defined(__ANDROID__)`, which the SDK's CMake build never defines.
//     Measured: every thread of this runner, and every thread of the fake service host, carries
//     its own process name. So the name cannot be looked for.
//   - By thread count. The runner's count moves during a run as cases start and join their own
//     workers - measured going 1 to 5 to 6, with different thread ids at equal counts - so
//     inferring the pool from a count, or from a count that does not grow when startThreadPool()
//     is called redundantly, would be unsound.
//
// So neither of those is what the assertion looks at: it asks libbinder itself.
// getThreadPoolMaxThreadCount() returns the configured maximum once mThreadPoolStarted is set and
// zero while it is not - exactly the flag startThreadPool() sets - read through a public API and
// behaving identically on both platforms. The thread-name probe is kept and PRINTED rather than
// asserted, with the reason, because it is still corroboration on a build that applies the name.
// The real-thread evidence is invocation E's: it receives a `oneway` callback on a pool thread over
// real out-of-process IPC, which no in-process fake can produce. The two tiers together discharge
// the obligation - a pool with no callback and a callback with no pool are both consistent with a
// broken receive path on a device - and this case owns the idempotency half.
/**
 * @brief open() succeeds and keeps delivering when a binder threadpool has already been started
 *        in the process.
 * @pre Invocation B. The pool is observed before anything here touches the session, so the
 *      evidence is about production's own open during LibCCEC::init rather than about a pool this
 *      case caused.
 * @note This is the case a naive local flag would break: no back-end-local state can detect a
 *       pool another component started, and lowering an already-established maximum can abort the
 *       process, which is why startThreadPool() is the whole of the obligation.
 * @note The pool is observed through ProcessState::getThreadPoolMaxThreadCount(), which reads
 *       the flag startThreadPool() sets and is portable; the "binder:*" thread name is reported
 *       as corroboration only, because the pinned SDK's libutils applies it solely on an Android
 *       build. The real-thread evidence is invocation E's.
 * @note Counting threads is NOT the substitute, and was measured and rejected rather than
 *       assumed: the runner's count moves during a run as cases start and join their own workers
 *       - 1 to 5 to 6, with different thread ids at equal counts - so inferring the pool from a
 *       count, or from a count that does not grow when startThreadPool() is called redundantly,
 *       would be unsound. The idempotency assertions below are unaffected either way.
 */
TEST_F(DriverAidlSessionTest, OpenSucceedsAndDeliversWhenAThreadPoolWasAlreadyStarted) {
    // The pool the process's first open was obliged to start, observed before anything here
    // touches the session - so this is evidence about production's own open during
    // LibCCEC::init, not about a pool this case might have caused.
    //
    // ASKED OF libbinder ITSELF RATHER THAN OF THE THREAD LIST. getThreadPoolMaxThreadCount()
    // returns the configured maximum once mThreadPoolStarted is set and zero while it is not,
    // so a non-zero answer IS the observation that startThreadPool() ran - the same flag, read
    // through a public API, on both platforms. selfOrNull() is used rather than self() so that
    // this line cannot create a ProcessState, and a null answer means no pool can exist.
    const ::android::sp<::android::ProcessState> processState =
        ::android::ProcessState::selfOrNull();
    const size_t poolMaxThreads =
        (processState != nullptr) ? processState->getThreadPoolMaxThreadCount() : 0u;

    EXPECT_GT(poolMaxThreads, 0u)
        << "libbinder reports no started threadpool in this process - although the AIDL back-end "
           "has been open since LibCCEC::init, and DriverAidlImpl::open() is obliged to call "
           "ProcessState::self()->startThreadPool(), which sets exactly the flag this value "
           "reads. Nothing else in this case would notice: an in-process fake dispatches its "
           "trigger on the CALLING thread, so frames arrive regardless, and the omission would "
           "first appear on a real device as a receive path that never delivers anything at all"
        << (processState == nullptr
                ? ". ProcessState::selfOrNull() returned null, so no ProcessState was ever "
                  "created in this process at all - which means the back-end never reached "
                  "libbinder, not merely that it skipped the pool"
                : "");

    // CORROBORATION ONLY, reported and not asserted: on this port a running pool produces threads
    // that carry the process's own name, so a false result here is the ordinary case and asserting
    // on it would fail against a correct back-end. The line is printed either way, so that a build
    // which does apply the name is visible in the log rather than silently indistinguishable. The
    // assertion above rests on the reported maximum; the real-thread evidence is invocation E's,
    // in hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp.
    bool procIsReadable = false;
    const bool poolExists = processHasABinderThread(&procIsReadable);

    if (!procIsReadable) {
        std::cout << "[DriverAidlSessionTest] /proc/self/task could not be read, so no thread "
                     "listing was available on this run."
                  << std::endl;
    }
    else if (poolExists) {
        std::cout << "[DriverAidlSessionTest] a thread named " << kBinderThreadNamePrefix
                  << "* is present, so this build applies the name "
                     "ProcessState::makeBinderThreadName() composes and the binder threadpool is "
                     "directly observable."
                  << std::endl;
    }
    else {
        std::cout << "[DriverAidlSessionTest] no thread named " << kBinderThreadNamePrefix
                  << "* is present. Expected on this port and NOT evidence that the threadpool is "
                     "absent: androidCreateRawThreadEtc() discards the name outside __ANDROID__ "
                     "builds, so a running pool inherits the process's own comm. The evidence that "
                     "DriverAidlImpl::open() started the pool is invocation E's, which receives a "
                     "oneway callback on a pool thread over real out-of-process IPC, in "
                     "hdmicec/tests/L2Tests/ccec/test_DualPathIntegration.cpp. The idempotency "
                     "assertions below are unaffected."
                  << std::endl;
    }

    // A close/open cycle so that open() genuinely runs its body again, rather than returning
    // silently on an already-open driver.
    ASSERT_NO_THROW({ Driver::getInstance().close(); });
    ASSERT_EQ(fake->getCloseCallCount(), 1) << "the close did not reach the HAL";

    // The successful-close arm: the controller handed back to the HAL is the one open()
    // returned. Asserted on the success path here, and on both failure paths in the two close
    // cases above, because a close that names the wrong session would leave the real one open
    // HAL-side while the middleware believed it had gone.
    {
        const ::android::sp<cechal::IHdmiCecController> closedController =
            fake->getLastClosedController();
        const ::android::sp<cechal::IHdmiCecController> openedController = fake->getController();

        ASSERT_NE(closedController.get(), nullptr)
            << "the HAL recorded no controller for a close that reported success, so close() "
               "passed nothing identifiable and the HAL cannot know which session to end";
        EXPECT_EQ(closedController.get(), openedController.get())
            << "the successful close named a different controller from the one open() returned. "
               "IHdmiCec::close takes the controller precisely so the HAL knows which session is "
               "being ended; naming another one would end the wrong session, or none";
    }

    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "open() failed on a second pass, with a binder threadpool already started in this "
           "process. startThreadPool() is idempotent, so a failure here means the back-end is "
           "tracking pool state of its own - which cannot see a pool another component started";

    EXPECT_EQ(fake->getOpenCallCount(), 2)
        << "the HAL did not observe a second open, so the session was not re-established";

    const std::vector<uint8_t> message{ 0x0F, GIVE_DEVICE_POWER_STATUS };
    EXPECT_TRUE(fake->fireOnMessageReceived(message))
        << "no callback arrives after the second open, so the listener was not re-registered and "
           "the receive path is dead for the rest of the session";

    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the re-opened session cannot transmit, so open() did not restore the controller";
}

// The duplicate-open guard, asserted as the silent no-op it is rather than inferred from the
// absence of an exception.
//
// open() on an already-OPENED driver returns without doing anything. That is deliberate and it is
// the legacy behaviour being preserved: DriverImpl::open()'s throw for this case is compiled out,
// so the observable legacy behaviour is a silent return and this back-end has to match it.
//
// "returns silently" and "RE-OPENED the session" are indistinguishable from the outside unless
// the HAL is asked, which is the gap this case closes. Three things must be unchanged, and each
// would be a distinct defect on its own:
//
//   The open count. A second IHdmiCec::open() on a session that is already held fails with
//   EX_ILLEGAL_STATE on a real HAL - the interface is single-instance - so a duplicate open that
//   reached the HAL would raise IOException on a device while passing against a fake that
//   tolerates it. This fake does tolerate it, deliberately, so that the guard is testable at all;
//   which is exactly why the assertion is on the counter and not on the exception.
//
//   The controller. Replacing it would invalidate the session the middleware has been
//   transmitting on, and the old one would be leaked HAL-side with nothing able to close it.
//
//   The listener. Replacing it would leave the HAL holding a listener whose owner had moved on -
//   and re-registering is the one thing the fixture's own close/reset/open cycle depends on not
//   happening implicitly.
//
// This also pins the behaviour the fixture itself relies on: its TearDown restores the opened
// baseline by calling open() unconditionally, which is only harmless because of this guard.
/**
 * @brief Opening an already-open driver returns silently and touches nothing - not the HAL, not
 *        the controller, not the listener.
 * @pre Invocation B, with SetUp having left exactly one open behind it so the counter can
 *      distinguish a no-op from a re-open.
 * @note Asserted on the fake's counter rather than on an exception, because this fake tolerates
 *       duplicate opens deliberately - a real HAL is single-instance and would fail the second
 *       call, so a duplicate that reached the HAL would raise on a device while passing here.
 * @note It also pins the behaviour the fixture itself relies on: TearDown restores the opened
 *       baseline by calling open() unconditionally, which is only harmless because of this guard.
 */
TEST_F(DriverAidlSessionTest, OpeningAnAlreadyOpenDriverIsASilentNoOpThatTouchesNothing) {
    // SetUp left exactly one open behind it, and the session it established is the baseline.
    ASSERT_EQ(fake->getOpenCallCount(), 1)
        << "the fixture did not leave exactly one open behind it, so the counter below cannot "
           "distinguish a no-op from a re-open";

    const ::android::sp<cechal::IHdmiCecController> controllerBefore = fake->getController();
    const ::android::sp<cechal::IHdmiCecEventListener> listenerBefore = fake->getListener();

    ASSERT_NE(controllerBefore.get(), nullptr)
        << "the fake holds no controller after the fixture's open, so there is no session identity "
           "to compare against";
    ASSERT_NE(listenerBefore.get(), nullptr)
        << "the fake holds no listener after the fixture's open, so there is no listener identity "
           "to compare against";

    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "open() raised on an ALREADY-OPEN driver. The legacy back-end's throw for this case is "
           "compiled out, so its observable behaviour is a silent return - raising here would be "
           "an unregistered difference between the back-ends, and one that Bus::start() and the "
           "fixture's own TearDown would both hit";

    EXPECT_EQ(fake->getOpenCallCount(), 1)
        << "the duplicate open reached the HAL: the open count is now " << fake->getOpenCallCount()
        << " where it must still be 1. IHdmiCec::open() is single-instance and a real HAL fails "
           "the second call with EX_ILLEGAL_STATE, so this would raise IOException on a device "
           "while passing here - this fake accepts duplicate opens on purpose, which is what makes "
           "the guard observable at all";

    EXPECT_EQ(fake->getController().get(), controllerBefore.get())
        << "the duplicate open replaced the controller, so the session the middleware has been "
           "transmitting on has been swapped underneath it and the previous one is stranded "
           "HAL-side with nothing able to close it";

    EXPECT_EQ(fake->getListener().get(), listenerBefore.get())
        << "the duplicate open re-registered a listener, so the HAL now holds one whose owner may "
           "already have moved on - and the fixture's close/reset/open cycle, which exists "
           "precisely to refresh that capture deliberately, would no longer control when it "
           "happens";

    // And the untouched session still works, which is the caller-visible form of "nothing
    // happened".
    CECFrame frame = directedFrame();
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the session no longer transmits after a duplicate open that was supposed to do "
           "nothing at all";
}

// The use-after-free regression: a callback that arrives after a failed close, and again after
// its owner has ceased to exist, must be dropped and must touch nothing.
//
// The hazard, stated concretely, because it is not obvious from either side alone. The event
// listener is a binder object and the HAL holds its own strong reference to it, so its lifetime
// is not the middleware's to end. It reaches its owner through a back pointer to a
// DriverAidlImpl. Those two facts together mean that a listener still registered when its owner's
// storage goes away is a live binder object holding a dangling pointer, and the next callback -
// which arrives on the HAL's schedule, not the middleware's - dereferences it. That is CWE-416,
// and the two ways in are exactly the two this case drives: a close that failed, where a
// detachment placed after the error check would be skipped, and a destructor.
//
// The fix it guards is that detachment happens on both arms of close() - before the result is
// evaluated, not after - and unconditionally in the destructor. What makes the guard possible to
// assert at all is that the drop is reported: the detached listener logs a specific line and
// returns ok, which is the only externally visible trace a dropped oneway callback can leave.
// The line is matched verbatim, deliberately - a paraphrase would keep passing after the message
// changed, and the message is the evidence here.
//
// And the run surviving is part of the assertion, not the absence of one. If the back pointer
// were stale, this case would corrupt memory or fault rather than fail: the log assertion is
// what turns that into a diagnosis instead of a crash somewhere later.
//
// Requires invocation b, as the whole fixture does. On a host with no binder driver these lines
// are compiled and type-checked but never executed - which is stated rather than left implicit,
// because a case whose evidence only exists on another runner must not be read as evidence here.
/**
 * @brief A callback arriving after a failed close, and again after its owner has ceased to exist,
 *        is dropped and touches nothing.
 * @pre Invocation B. The listener is retained by this case so it outlives its owner, which is
 *      what the HAL's own strong reference does on a device; without that the hazard would be
 *      unreachable and unasserted.
 * @note The hazard is a use-after-free. The listener is a binder object whose lifetime is not the
 *       middleware's to end, and it reaches its owner through a back pointer, so a listener still
 *       registered when its owner's storage goes away is a live object holding a dangling pointer
 *       that the next callback dereferences. The two ways in are the two arms driven here.
 * @note The invariant is that detachment happens on both arms of close, before the result is
 *       evaluated, and unconditionally in the destructor. The drop line is matched verbatim
 *       because it is the only externally visible trace a dropped oneway callback leaves, and a
 *       paraphrase would keep passing after the message changed.
 * @warning The run surviving is part of the assertion. A stale back pointer would corrupt memory
 *          or fault rather than fail, and the log assertion is what turns that into a diagnosis
 *          instead of a crash somewhere later.
 */
TEST_F(DriverAidlSessionTest, ACallbackAfterAFailedCloseOrOwnerDestructionIsDroppedNotDelivered) {
    RecordingFrameListener applicationListener;
    ListeningConnection listeningConnection(LogicalAddress(LogicalAddress::UNREGISTERED),
                                            "DriverAidlSessionTest-post-detach",
                                            applicationListener);

    const char *const expectedDropLine =
        "DriverAidlImpl::EventListener: message received after detach, dropping it";

    // Held so the listener outlives its owner, which is what the HAL's own strong reference does
    // on a real device. Without it the listener would be collected with the driver and the
    // hazard would be unreachable - and unasserted.
    ::android::sp<cechal::IHdmiCecEventListener> retainedListener;

    const std::vector<uint8_t> afterFailedClose{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x61, 0x00, 0x04 };
    const std::vector<uint8_t> afterDestruction{ 0x4F, REPORT_PHYSICAL_ADDRESS, 0x71, 0x00, 0x04 };

    {
        // a local instance, so that its destruction can be driven from here. The process-global
        // driver outlives every case and could not be used for this.
        DriverAidlImpl localDriver;

        ASSERT_TRUE(localDriver.isServiceAvailable())
            << "a local instance could not resolve the fake service, so it cannot open a session "
               "and no listener would be registered to deliver to afterwards";
        ASSERT_NO_THROW({ localDriver.open(); })
            << "the local instance could not open a session, so there is no listener to retain";

        retainedListener = fake->getListener();
        ASSERT_NE(retainedListener.get(), nullptr)
            << "the local open handed no listener to the HAL, so nothing can arrive after the "
               "session ends and this case has nothing to drive";

        // Arm 1: a close the HAL reports as failed. Detachment must already have happened by the
        // time the exception escapes.
        fake->setCloseResult(false);

        EXPECT_THROW({ localDriver.close(); }, IOException)
            << "the close did not fail, so this arm is not the failure path it is meant to drive";

        std::string capturedAfterFailedClose;
        {
            StdoutCapture capture;
            ASSERT_TRUE(capture.isValid())
                << "stdout could not be captured, so the drop cannot be observed and a delivery to "
                   "freed storage would be indistinguishable from a correct drop";

            const ::android::binder::Status delivered =
                retainedListener->onMessageReceived(afterFailedClose);

            capturedAfterFailedClose = capture.read();

            EXPECT_TRUE(delivered.isOk())
                << "the listener returned a non-ok status for a callback it should simply have "
                   "dropped. A oneway callback has no caller to receive a fault, so anything other "
                   "than ok here reaches onTransact and is worse than dropping one frame";
        }

        EXPECT_NE(capturedAfterFailedClose.find(expectedDropLine), std::string::npos)
            << "a callback arriving after a close that failed was not reported as dropped. The "
               "listener must be detached on both arms of close(), before the result is evaluated: "
               "place the detachment after the error check and this path leaves a live listener "
               "holding a pointer to a driver whose session is gone - and on a real device the "
               "next HAL callback follows. Expected the line [" << expectedDropLine
            << "] and captured [" << capturedAfterFailedClose << "]";

        // The owner is destroyed here. Its state was already CLOSED, so the destructor's own
        // close is a silent no-op and the unconditional detachment is what runs.
    }

    // Arm 2: the owner's storage is gone. The listener is still a live binder object, still
    // callable, and still held by the fake and by this case.
    std::string capturedAfterDestruction;
    {
        StdoutCapture capture;
        ASSERT_TRUE(capture.isValid())
            << "stdout could not be captured, so the post-destruction drop cannot be observed";

        const ::android::binder::Status delivered =
            retainedListener->onMessageReceived(afterDestruction);

        capturedAfterDestruction = capture.read();

        EXPECT_TRUE(delivered.isOk())
            << "the listener returned a non-ok status for a callback arriving after its owner was "
               "destroyed, rather than dropping it";
    }

    EXPECT_NE(capturedAfterDestruction.find(expectedDropLine), std::string::npos)
        << "a callback arriving after the owner's destruction was not reported as dropped. The "
           "destructor must detach unconditionally - not only when the state says a session is "
           "open, and not only when its own close succeeded - because the HAL's strong reference "
           "keeps the listener callable after the storage its back pointer names has ceased to "
           "exist. Expected the line [" << expectedDropLine << "] and captured ["
        << capturedAfterDestruction << "]";

    // Nothing reached an application listener from either callback, which is the other half of
    // "touched nothing": a dropped frame must not be delivered through some other route either.
    EXPECT_FALSE(applicationListener.waitForFrames(1, kNonDeliveryWindowMs))
        << "a callback delivered after the session ended reached an application listener. It was "
           "dropped according to the log and delivered according to the listener, so one of the "
           "two is wrong and the receive path is not in the state it reports";

    fake->setCloseResult(true);
}

// open() reporting a NULL controller is a failure, even when the transaction itself succeeded.
//
// Two conditions converge on one guard - `if (!txn.isOk() || (controller == 0))`
// in DriverAidlImpl::open() - and this is the arm that is easy to omit, because a status of
// ok reads as success. Accepting a null controller would leave the state OPENED with no
// session, and every subsequent transmit would then fail on the controller check instead, far
// from the cause.
/**
 * @brief open() rejects a null controller even when the transaction itself reported success.
 * @pre Invocation B, with the session closed first and the fake configured to report a null
 *      controller.
 * @note Two conditions converge on one guard, and this is the arm easy to omit because a status
 *       of ok reads as success. Accepting a null controller would leave the state open with no
 *       session, and every later transmit would fail on the controller check instead, far from
 *       the cause - so the state is asserted not to have moved.
 */
TEST_F(DriverAidlSessionTest, OpenRejectsANullControllerEvenWhenTheStatusIsOk) {
    ASSERT_NO_THROW({ Driver::getInstance().close(); });

    fake->setOpenReturnsNullController(true);

    EXPECT_THROW({ Driver::getInstance().open(); }, IOException)
        << "open() accepted a null controller reported alongside an ok status, so the state would "
           "be OPENED with no session behind it";

    // Still closed, so the guards are live - open() did not move the state before its checks.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver believes it is open after an open() that reported no controller";

    // Restore a usable session for TearDown; the fake is reset there in any case.
    fake->setOpenReturnsNullController(false);
    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "the session could not be restored after the null-controller case";
}

// A non-ok binder status on open is the other arm of the same guard, and it is asserted
// separately for the same reason: one condition, two independent ways to satisfy it.
//
// The recovery is not a tidy-up, it is half the evidence. Two things have to hold after a failed
// open, and neither follows from the IOException alone:
//
//   The state must not have moved. open() sets OPENED only after both halves of its guard pass,
//   so a failed open must leave a CLOSED driver - observed here through a guarded operation,
//   because the state is private and no introspection exists. An implementation that set the
//   state first and raised afterwards would leave the driver believing it holds a session that
//   the HAL refused, and every later transmit would fail on the controller check instead, far
//   from the cause.
//
//   The failure must not be sticky. The recovery open has to genuinely reach the HAL, which is
//   asserted on the fake's own open counter rather than on the absence of an exception: a driver
//   that had wrongly kept the OPENED state would return silently from open() - that is the
//   documented behaviour in that state - and the recovery would look identical while nothing
//   whatsoever had been re-established.
/**
 * @brief A non-ok transaction on open raises, leaves the state closed, and is not sticky.
 * @pre Invocation B, with the session closed first and a failing open status installed.
 * @note The other arm of the same guard, asserted separately because one condition has two
 *       independent ways to be satisfied.
 * @note The recovery is half the evidence rather than a tidy-up. The failing open is asserted to
 *       have reached the HAL, so the exception is the guard's and not something before the
 *       transaction; and the recovery open is asserted on the fake's counter, because a driver
 *       that had wrongly kept the open state would return silently and look identical while
 *       nothing had been re-established.
 */
TEST_F(DriverAidlSessionTest, OpenRejectsANonOkBinderStatus) {
    ASSERT_NO_THROW({ Driver::getInstance().close(); });

    const int openCountBeforeFailure = fake->getOpenCallCount();

    fake->setOpenBinderStatus(::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().open(); }, IOException)
        << "open() accepted a failed transaction, so a dead HAL would be reported as an open "
           "session";

    EXPECT_EQ(fake->getOpenCallCount(), openCountBeforeFailure + 1)
        << "the failing open did not reach the HAL at all, so the IOException came from somewhere "
           "before the transaction and this case is not exercising the guard it names";

    // The state did not move: the driver is still CLOSED, so its guards are live.
    CECFrame frame = directedFrame();
    EXPECT_THROW({ Driver::getInstance().write(frame); }, InvalidStateException)
        << "the driver behaves as though it were open after an open() whose transaction failed. "
           "The state must only advance once both halves of open()'s guard have passed, or a "
           "caller that swallows the IOException is left holding a driver with no session behind "
           "it - and the next transmit fails on the controller check, far from the cause";

    fake->setOpenBinderStatus(::android::binder::Status::ok());
    EXPECT_NO_THROW({ Driver::getInstance().open(); })
        << "the session could not be restored after the failed-open case";

    EXPECT_EQ(fake->getOpenCallCount(), openCountBeforeFailure + 2)
        << "the recovery open did not reach the HAL, although it raised nothing. That is what a "
           "driver which had wrongly retained the OPENED state looks like from outside: open() "
           "returns silently in that state, so the absence of an exception says nothing. The "
           "counter is what distinguishes a re-established session from a silent no-op";

    // And the restored session really works, which is the caller-visible form of the same claim.
    EXPECT_NO_THROW({ Driver::getInstance().write(frame); })
        << "the driver still refuses to transmit after a successful recovery open, so the session "
           "was not actually re-established";
}

// A non-ok status on getLogicalAddresses reports 0 and does not raise, which is the arm that
// keeps the AIDL back-end's failure signalling identical to the legacy back-end's.
//
// getLogicalAddress never throws on either back-end: the legacy implementation ignores its
// HAL's return value entirely and yields whatever the call left in a zero-initialised local.
// Raising here would be a new exception on a path LibCCEC does not guard, and it would bypass
// the InvalidStateException that LibCCEC raises for a zero - the signal callers already
// handle.
/**
 * @brief A transport failure on the address query reports 0 and does not raise.
 * @pre Invocation B, with a failing query status installed.
 * @note This is the arm that keeps the AIDL back-end's failure signalling identical to the
 *       legacy back-end's, which ignores its HAL's return value entirely. Raising here would be
 *       a new exception on a path LibCCEC does not guard, and it would bypass the zero-means-no-
 *       address signal callers already handle.
 */
TEST_F(DriverAidlSessionTest, GetLogicalAddressReportsZeroOnTransportFailureWithoutRaising) {
    fake->setGetLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    int reported = -1;
    EXPECT_NO_THROW({ reported = Driver::getInstance().getLogicalAddress(DeviceType::TV); })
        << "getLogicalAddress raised on a transport failure. Neither back-end throws here, and "
           "LibCCEC::getLogicalAddress does not guard the call - a new exception would propagate "
           "into the Source plugin's discovery path unhandled";

    EXPECT_EQ(reported, 0)
        << "a failed query did not report 0, so LibCCEC's zero-means-no-address signal is bypassed";
}

// A non-ok status on removeLogicalAddresses is ignored, matching the legacy disposition of
// discarding the HAL return - and the local removal still stands, because it precedes the call.
//
// This is the transport arm of the remove disposition; the refusal arm is asserted above. Both
// are needed: they are separate branches of DriverAidlImpl::removeLogicalAddress() and either
// one raising
// would be an unregistered difference.
/**
 * @brief A transport failure on the removal is ignored, and the local removal still stands
 *        because it precedes the call.
 * @pre Invocation B, with an address added first and a failing removal status installed.
 * @note The transport arm of the remove disposition; the refusal arm is asserted above. Both are
 *       needed because they are separate branches and either one raising would be an unregistered
 *       difference.
 */
TEST_F(DriverAidlSessionTest, RemoveLogicalAddressIgnoresTransportFailureAndStillRemovesLocally) {
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));

    fake->getController()->setRemoveLogicalAddressesBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_NO_THROW({ Driver::getInstance().removeLogicalAddress(address); })
        << "removeLogicalAddress raised on a transport failure. The legacy back-end discards its "
           "HAL's return value, so raising here would be an unregistered behaviour change";

    EXPECT_FALSE(Driver::getInstance().isValidLogicalAddress(address))
        << "the address is still held locally after a removal whose transaction failed. The local "
           "removal precedes the HAL call and is not rolled back";
}

// The four AIDL methods that are deliberately not consumed stay unconsumed.
//
// getState, getProperty, registerEventListener and unregisterEventListener are each excluded
// for a stated reason: the middleware keeps its own CLOSED/CLOSING/OPENED state machine and a
// second source of truth would be worse than none; the HAL properties have no legacy
// counterpart, so reading them would be new behaviour; and the listener registration methods
// exist for non-controlling diagnostic clients, whereas this back-end is the controlling client
// and receives events through the listener it hands to open().
//
// This is asserted rather than commented because an absence is exactly the kind of decision
// that erodes silently. A future change that started consulting getState - the most tempting of
// the four, since it looks like a cheap way to answer "is the HAL up" - would fail here and be
// a visible decision rather than a quiet second state machine. The whole session lifecycle has
// run by the time this case executes: SetUp closed, reset and re-opened.
/**
 * @brief The four AIDL methods deliberately not consumed stay unconsumed.
 * @pre Invocation B, after a full session cycle so that anything the back-end does at open or
 *      close time has had its chance to call them.
 * @note Asserted rather than commented because an absence erodes silently. Each exclusion has a
 *       stated reason: the middleware keeps its own state machine and a second source of truth
 *       would be worse than none; the HAL properties have no legacy counterpart; and the listener
 *       registration methods exist for non-controlling diagnostic clients, whereas this back-end
 *       is the controlling client and receives events through the listener it hands to open().
 */
TEST_F(DriverAidlSessionTest, TheFourUnconsumedAidlMethodsAreNeverCalled) {
    // A representative slice of the surface the middleware does drive, so that the zeros below
    // cannot be explained by nothing having happened at all.
    const LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    ASSERT_TRUE(Driver::getInstance().addLogicalAddress(address));
    CECFrame frame = directedFrame();
    ASSERT_NO_THROW({ Driver::getInstance().write(frame); });
    ASSERT_NO_THROW({ Driver::getInstance().poll(address, LogicalAddress(LogicalAddress::TV)); });
    (void)Driver::getInstance().getLogicalAddress(DeviceType::TV);

    EXPECT_EQ(fake->getGetStateCallCount(), 0)
        << "IHdmiCec::getState was consulted. The middleware tracks its own lifecycle state, and a "
           "second source of truth is worse than none - poll() in particular is a CEC ping via a "
           "one-byte transmit, not a state query";
    EXPECT_EQ(fake->getGetPropertyCallCount(), 0)
        << "IHdmiCec::getProperty was consulted. HAL_CEC_VERSION and the METRIC_* properties have "
           "no legacy counterpart, so reading them would be new behaviour";
    EXPECT_EQ(fake->getRegisterEventListenerCallCount(), 0)
        << "IHdmiCec::registerEventListener was called. It exists for non-controlling diagnostic "
           "clients; this back-end is the controlling client and gets its events from the listener "
           "it passes to open(), so calling it would register a second listener";
    EXPECT_EQ(fake->getUnregisterEventListenerCallCount(), 0)
        << "IHdmiCec::unregisterEventListener was called, which is the mirror of a registration "
           "that never happened";
}

/**
 * @brief Transmit-status translation and the frame-length guard on the AIDL back-end.
 *
 * Requires invocation B. See DriverAidlSessionFixture for the precondition and for why the
 * session is cycled in SetUp.
 *
 * Separated from the session fixture because the subject is different: everything here is one
 * call - write() - and what varies is the status the HAL reports and the destination nibble
 * the frame carries. Keeping the matrix in a fixture of its own keeps each case's body to the
 * one thing it varies.
 */
class DriverAidlTransmitTest : public DriverAidlSessionFixture {
protected:
    /**
     * @brief Programs the canned send status and hands the frame to the AIDL back-end.
     *
     * @param [in] frame  - Frame to transmit, header byte first.
     * @param [in] status - Status the fake reports for the transmission.
     */
    void transmitWithStatus(const CECFrame &frame, cechal::SendMessageStatus status) {
        fake->getController()->setSendMessageResult(status);
        Driver::getInstance().write(frame);
    }

    /**
     * @brief Asserts the fake received exactly one frame, of the expected size.
     *
     * @param [in] length - Frame size in bytes the fake is required to have captured.
     *
     * @note Both the count and the size are checked, because "the call happened" is never the
     *       claim here: a truncated or padded frame reaches the bus as a different CEC
     *       message.
     */
    void expectExactlyOneFrameOfLength(size_t length) {
        EXPECT_EQ(fake->getController()->getSendMessageCallCount(), 1)
            << "the HAL was not asked exactly once to send";
        EXPECT_EQ(fake->getController()->getLastSentMessage().size(), length)
            << "the HAL received a frame of " << fake->getController()->getLastSentMessage().size()
            << " bytes rather than " << length << ", so the frame was truncated or padded";
    }
};

// A directed frame reported ACK_STATE_0 succeeded: the addressed follower acknowledged it.
//
// This is the arm where the inverted sense bites hardest, because the same status value means
// the opposite thing on a broadcast (asserted below). The frame is also checked on the way
// through: transmit success is not merely "no exception", it is "these exact bytes reached the
// HAL".
/**
 * @brief A directed frame the addressed follower acknowledged succeeds, and the exact bytes
 *        reach the HAL.
 * @pre Invocation B, with the session open and the fake reset by SetUp.
 * @note This is the arm where the inverted status sense bites hardest, because the same value
 *       means the opposite thing on a broadcast. Success is asserted as "these bytes arrived"
 *       rather than as "no exception", so a transmit that dropped or rewrote the frame cannot
 *       pass.
 */
TEST_F(DriverAidlTransmitTest, DirectedFrameAcknowledgedByTheFollowerSucceeds) {
    CECFrame frame = directedFrame();

    EXPECT_NO_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a directed frame the follower acknowledged was reported as a failure. ACK_STATE_0 "
           "means acknowledged for a directed message; reading it as a rejection would fail every "
           "successful directed transmit in the system";

    expectExactlyOneFrameOfLength(2u);

    const std::vector<uint8_t> sent = fake->getController()->getLastSentMessage();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0], 0x40) << "the header byte was altered on the way to the HAL";
    EXPECT_EQ(sent[1], GIVE_DEVICE_POWER_STATUS) << "the opcode was altered on the way to the HAL";
}

// A directed frame reported ACK_STATE_1 was not acknowledged, and raises CECNoAckException -
// reproducing DriverImpl.cpp:276-278, where the legacy SENT_BUT_NOT_ACKD does the same.
//
// The exception type is the substance: Connection and the plugins above it distinguish "no
// device answered" from "the transmit failed", and collapsing the two into IOException would
// make an absent peer indistinguishable from a broken HAL.
/**
 * @brief A directed frame reported as not acknowledged raises CECNoAckException, and was still
 *        sent.
 * @pre Invocation B.
 * @note The exception type is the substance: Connection and the plugins above it distinguish "no
 *       device answered" from "the transmit failed", and collapsing the two into IOException
 *       would make an absent peer indistinguishable from a broken HAL.
 * @note The frame is asserted to have reached the HAL, because the exception describes the bus
 *       outcome rather than a refusal to transmit.
 */
TEST_F(DriverAidlTransmitTest, DirectedFrameNotAcknowledgedRaisesNoAck) {
    CECFrame frame = directedFrame();

    EXPECT_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_1); },
                 CECNoAckException)
        << "a directed frame that was not acknowledged did not raise CECNoAckException. "
           "ACK_STATE_1 is the not-acknowledged value for a directed message, and the exception "
           "type is what lets callers tell an absent peer from a broken HAL";

    // It was still sent: the exception describes the bus outcome, not a refusal to transmit.
    expectExactlyOneFrameOfLength(2u);
}

// A broadcast frame reported ACK_STATE_1 succeeded, which is the mirror image of the directed
// case above and the single fact that makes the translation non-trivial.
//
// A broadcast has no addressed follower, so nothing acknowledges it; ACK_STATE_1 there means
// "sent and not rejected". An implementation that mapped ACK_STATE_1 to CECNoAckException
// unconditionally would compile, would pass every directed case, and would fail every
// broadcast the middleware sends - which is most of what the Sink emits during discovery.
/**
 * @brief A broadcast frame reported with the directed not-acknowledged value succeeds, which is
 *        the mirror image of the directed case.
 * @pre Invocation B.
 * @note This is the single fact that makes the translation non-trivial. A broadcast has no
 *       addressed follower, so nothing acknowledges it and that value means sent and not
 *       rejected. An implementation mapping it to CECNoAckException unconditionally would
 *       compile, pass every directed case, and fail every broadcast the middleware sends - which
 *       is most of what the Sink emits during discovery.
 */
TEST_F(DriverAidlTransmitTest, BroadcastFrameNotRejectedSucceeds) {
    CECFrame frame = broadcastFrame(GIVE_DEVICE_POWER_STATUS);

    EXPECT_NO_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_1); })
        << "a broadcast frame reported ACK_STATE_1 was treated as a failure. On a broadcast that "
           "value means sent and not rejected - the opposite of its meaning on a directed message "
           "- so this is the inverted sense being read the wrong way round";

    expectExactlyOneFrameOfLength(2u);
}

// A broadcast frame reported ACK_STATE_0 was rejected by some follower, and for most opcodes
// that returns normally - matching the legacy implementation, which throws only on the CEC CTS
// arm.
//
// This case is the negative control for the CTS case below: without it, an implementation that
// raised on every rejected broadcast would pass the CTS case and be wrong everywhere else.
/**
 * @brief A rejected broadcast carrying an opcode outside the conformance arm returns normally.
 * @pre Invocation B.
 * @note The negative control for the conformance case below: without it, an implementation that
 *       raised on every rejected broadcast would pass that case and be wrong everywhere else.
 *       The legacy implementation raises only on the one opcode, which is what this matches.
 */
TEST_F(DriverAidlTransmitTest, RejectedBroadcastReturnsNormallyForOpcodesOutsideTheCtsArm) {
    CECFrame frame = broadcastFrame(GIVE_DEVICE_POWER_STATUS);

    EXPECT_NO_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a rejected broadcast carrying an opcode outside the CEC CTS 9-3-3 arm raised. The "
           "legacy back-end raises only on a rejected REPORT_PHYSICAL_ADDRESS, so raising here "
           "would make every rejected broadcast an error the callers do not expect";

    expectExactlyOneFrameOfLength(2u);
}

// The CEC CTS 9-3-3 arm: a rejected broadcast REPORT_PHYSICAL_ADDRESS does raise, so that the
// caller retries at least once - which is what the conformance test requires.
//
// This is the one opcode-specific arm in the whole translation (the broadcast branch of
// DriverAidlImpl::write(), mirroring DriverImpl.cpp:279-284), and it is the reason
// REPORT_PHYSICAL_ADDRESS is pinned by
// a static assertion at the top of this file: if that constant drifted, this case and the
// negative control above would silently be testing the same opcode and the distinction between
// them would vanish.
/**
 * @brief The CEC CTS 9-3-3 arm: a rejected broadcast Report Physical Address raises, so the
 *        caller retries at least once.
 * @pre Invocation B.
 * @note The one opcode-specific arm in the whole translation, which is why the opcode constant
 *       is pinned by a static assertion at the top of this file: were it to drift, this case and
 *       its negative control would silently test the same opcode and the distinction between
 *       them would vanish.
 */
TEST_F(DriverAidlTransmitTest, RejectedBroadcastReportPhysicalAddressRaisesForTheCtsRetry) {
    CECFrame frame = broadcastFrame(REPORT_PHYSICAL_ADDRESS);

    EXPECT_THROW({ transmitWithStatus(frame, cechal::SendMessageStatus::ACK_STATE_0); },
                 CECNoAckException)
        << "a rejected broadcast REPORT_PHYSICAL_ADDRESS did not raise CECNoAckException, so the "
           "caller has nothing to retry on and CEC CTS 9-3-3 fails";

    expectExactlyOneFrameOfLength(2u);
}

// A one-byte broadcast reported ACK_STATE_0 returns normally, because the CTS arm requires an
// opcode to inspect and a one-byte frame has none.
//
// The guard is `(length > 1) && ((frame.at(1) & 0xFF) == REPORT_PHYSICAL_ADDRESS)`, and the
// length conjunct is not decoration: poll() transmits exactly such a one-byte frame
// in DriverAidlImpl::poll(), so an implementation that read frame.at(1) without the length
// check would raise std::out_of_range out of every poll - and poll is how the Sink discovers
// the bus.
/**
 * @brief A one-byte broadcast returns normally, because the conformance arm needs an opcode byte
 *        to inspect and a one-byte frame has none.
 * @pre Invocation B, with a frame built to exactly one byte.
 * @note The length conjunct in that guard is not decoration: poll() transmits exactly such a
 *       one-byte frame, so an implementation reading the opcode byte without the length check
 *       would raise std::out_of_range out of every poll - and poll is how the Sink discovers the
 *       bus.
 */
TEST_F(DriverAidlTransmitTest, OneByteBroadcastDoesNotReachTheCtsArm) {
    CECFrame pollFrame;
    pollFrame.append(static_cast<uint8_t>(0x4F));
    ASSERT_EQ(pollFrame.length(), 1u);

    EXPECT_NO_THROW({ transmitWithStatus(pollFrame, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a one-byte broadcast raised. The CTS arm needs an opcode byte to inspect, and reading "
           "frame.at(1) without the length guard would raise std::out_of_range out of every "
           "poll() - which is exactly the frame poll() sends";

    expectExactlyOneFrameOfLength(1u);
}

// BUSY means arbitration failed after two attempts and the message was not sent, which is the
// legacy send-failed family and therefore an IOException - on both destinations, because
// nothing reached the bus for the ACK sense to apply to.
//
// Both destinations are asserted in one case precisely because the answer must not depend on
// the destination here: BUSY is checked before the nibble is ever read
// ahead of the acknowledgement matrix in DriverAidlImpl::write(), and an implementation that
// folded it into that matrix
// would give two different answers for the same failure.
/**
 * @brief A busy result is a transmit failure on both destinations, because nothing reached the
 *        bus for the acknowledgement sense to apply to.
 * @pre Invocation B. Both destinations are driven in one case precisely because the answer must
 *      not depend on the destination.
 * @note Busy means arbitration failed and the message was not sent, which is the legacy
 *       send-failed family. It is decided before the destination nibble is read, so an
 *       implementation that folded it into the acknowledgement matrix would give two different
 *       answers for one failure.
 * @note The broadcast half deliberately uses the conformance opcode, whose rejected arm raises a
 *       different exception - so a wrong exception type there means busy fell through into the
 *       matrix.
 */
TEST_F(DriverAidlTransmitTest, BusyIsATransmitFailureOnBothDestinations) {
    CECFrame directed = directedFrame();

    EXPECT_THROW({ transmitWithStatus(directed, cechal::SendMessageStatus::BUSY); }, IOException)
        << "a BUSY result on a directed frame did not raise IOException. Arbitration failed and "
           "nothing was sent, which is the legacy send-failed family - not a missing "
           "acknowledgement";

    fake->getController()->reset();

    CECFrame broadcast = broadcastFrame(REPORT_PHYSICAL_ADDRESS);
    EXPECT_THROW({ transmitWithStatus(broadcast, cechal::SendMessageStatus::BUSY); }, IOException)
        << "a BUSY result on a broadcast frame did not raise IOException. BUSY is decided before "
           "the destination nibble is read, so it must give the same answer on both destinations - "
           "note that the frame used here is the CTS opcode, whose ACK_STATE_0 arm raises "
           "CECNoAckException, so a different exception type here means BUSY fell through into the "
           "ACK matrix";
}

// A non-ok binder status is a transport failure and raises IOException, reproducing
// DriverImpl.cpp:261-263, where a HAL error return does the same.
//
// It is checked ahead of the reported send status in DriverAidlImpl::write(), which matters:
// the out-parameter is meaningless when the transaction did not complete, so a translation that
// consulted it first would classify a dead binder by whatever happened to be in that variable.
// The canned status here is deliberately one whose own arm would succeed, so a pass proves the
// transport check ran first.
/**
 * @brief A non-ok transaction on send is a transport failure and raises IOException.
 * @pre Invocation B, with a canned send status whose own arm would succeed - so a pass proves
 *      the transport check ran first.
 * @note The transport check is ahead of the reported send status, which matters: the
 *       out-parameter is meaningless when the transaction did not complete, so a translation
 *       consulting it first would classify a dead binder by whatever happened to be in that
 *       variable.
 */
TEST_F(DriverAidlTransmitTest, NonOkBinderStatusOnSendRaisesIoException) {
    CECFrame frame = directedFrame();

    fake->getController()->setSendMessageResult(cechal::SendMessageStatus::ACK_STATE_0);
    fake->getController()->setSendMessageBinderStatus(
        ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT));

    EXPECT_THROW({ Driver::getInstance().write(frame); }, IOException)
        << "a failed transaction did not raise IOException. The canned send status was "
           "ACK_STATE_0, whose own arm succeeds on a directed frame, so a pass here would mean the "
           "status out-parameter was consulted despite the transaction never completing";
}

// Authorized difference 1, AIDL half: a frame over the AIDL contract length is refused, and -
// the half that matters more - nothing is sent.
//
// 16 bytes is accepted, matching the legacy back-end, which confines the difference to the
// disputed 17-to-20 band rather than to the guard's existence. 17 and 20 are refused with
// IOException.
//
// The nothing-was-sent assertion is the point. An IOException raised after a truncated frame
// reached the bus would be strictly worse than either outcome alone: the caller sees a failure
// while a corrupt CEC frame is already on the wire, and a corrupt frame can be interpreted by a
// peer as a different message entirely. So the send counter is asserted to be zero, not merely
// the exception to have been raised. The guard runs ahead of the transmit
// in DriverAidlImpl::write(), which is what makes that true.
/**
 * @brief Authorized difference 1, AIDL half: a frame over the AIDL contract length is refused,
 *        and nothing is sent.
 * @pre Invocation B. The frame at exactly the limit is the control that both back-ends accept,
 *      which confines the difference to the disputed band rather than to the guard's existence.
 * @note The nothing-was-sent assertion is the point. An IOException raised after a truncated
 *       frame reached the bus would be worse than either outcome alone - the caller sees a
 *       failure while a corrupt CEC frame is already on the wire, and a peer can interpret a
 *       corrupt frame as a different message entirely. So the send counter is asserted to be
 *       zero, not merely the exception to have been raised.
 */
TEST_F(DriverAidlTransmitTest, FramesOverTheAidlLimitAreRefusedWithoutBeingSentOrTruncated) {
    // The control: exactly at the limit, accepted, whole.
    CECFrame atLimit = frameOfLength(kAidlMaxMessageLength);
    ASSERT_EQ(atLimit.length(), kAidlMaxMessageLength);

    EXPECT_NO_THROW({ transmitWithStatus(atLimit, cechal::SendMessageStatus::ACK_STATE_0); })
        << "a frame of exactly " << kAidlMaxMessageLength << " bytes was refused, although that is "
           "the AIDL contract's stated maximum and the legacy back-end accepts it too. The "
           "difference between the back-ends must be confined to the disputed band, not extended "
           "to the boundary itself";
    expectExactlyOneFrameOfLength(kAidlMaxMessageLength);

    const size_t overLimit[] = { kJustOverAidlLimit, kLegacyMaxMessageLength };

    for (size_t i = 0; i < sizeof(overLimit) / sizeof(overLimit[0]); i++) {
        fake->getController()->reset();

        CECFrame tooLong = frameOfLength(overLimit[i]);
        ASSERT_EQ(tooLong.length(), overLimit[i])
            << "the frame builder did not produce a frame of " << overLimit[i] << " bytes";

        EXPECT_THROW({ transmitWithStatus(tooLong, cechal::SendMessageStatus::ACK_STATE_0); },
                     IOException)
            << "a frame of " << overLimit[i] << " bytes was accepted, although the AIDL "
               "sendMessage contract states a maximum of " << kAidlMaxMessageLength;

        EXPECT_EQ(fake->getController()->getSendMessageCallCount(), 0)
            << "a frame of " << overLimit[i] << " bytes reached the HAL before being refused. The "
               "guard must run AHEAD of the transmit: an IOException raised after a truncated "
               "frame is already on the wire is worse than either outcome alone, because a peer "
               "can interpret a corrupt frame as a different message entirely";
        EXPECT_TRUE(fake->getController()->getLastSentMessage().empty())
            << "a frame of " << overLimit[i] << " bytes was captured by the HAL, so something was "
               "transmitted despite the refusal";
    }
}

// Authorized difference 2, AIDL half: writeAsync raises OperationNotSupportedException on an
// open driver, after the prelude and the state guard have both passed.
//
// Asynchronous transmit is not migrated and is deliberately not emulated - no threads, no work
// queue, no deferred callback, no wrapper - and that is safe because no production call site
// reaches it: every plugin transmit goes through Connection::sendToAsync and
// Connection::sendAsync onto the Bus writer thread, which then calls the synchronous write.
//
// Nothing may be transmitted, which is asserted rather than assumed: an implementation that
// quietly forwarded the frame to sendMessage before raising would emulate asynchrony badly -
// the frame would go out while the caller was told the operation is unsupported.
/**
 * @brief Authorized difference 2, AIDL half: writeAsync raises OperationNotSupportedException on
 *        an open driver, once prelude and state guard have both passed.
 * @pre Invocation B, with the session open - this is the one arm that needs an open driver,
 *      which is why it cannot live with the prelude cases.
 * @note Asynchronous transmit is not migrated and deliberately not emulated - no threads, no
 *       work queue, no deferred callback, no wrapper - which is safe because no production call
 *       site reaches it: every plugin transmit goes through Connection's asynchronous entry
 *       points onto the Bus writer thread, which then calls the synchronous write.
 * @note Nothing may be transmitted, asserted rather than assumed: an implementation that quietly
 *       forwarded the frame before raising would put it on the bus while telling the caller the
 *       operation is unsupported.
 */
TEST_F(DriverAidlTransmitTest, WriteAsyncRaisesOperationNotSupportedOnAnOpenDriver) {
    CECFrame frame = directedFrame();

    EXPECT_THROW({ Driver::getInstance().writeAsync(frame); }, OperationNotSupportedException)
        << "writeAsync did not raise OperationNotSupportedException on an open AIDL driver. This "
           "is the one arm where the two back-ends differ on writeAsync - the prelude ordering and "
           "the state guard are identical on both - so a different outcome here either emulates "
           "asynchrony or has lost the guard";

    EXPECT_EQ(fake->getController()->getSendMessageCallCount(), 0)
        << "writeAsync transmitted the frame before raising, which is the worst of both outcomes: "
           "the frame is on the bus and the caller was told the operation is unsupported";
}

// poll() reaches the bus as a one-byte CEC ping built from the two addresses, and it reaches it
// through this back-end's own write() rather than through any state query.
//
// The byte layout is asserted, not just the call: initiator in the high nibble, destination in
// the low one, composed in DriverAidlImpl::poll(). getState() is deliberately not used for
// this - a poll
// is a transmit that either gets acknowledged or does not, and answering it from the HAL's
// state would answer a different question entirely, which is why the unconsumed-methods case
// above checks the state counter stays at zero.
/**
 * @brief poll() reaches the bus as a one-byte CEC ping built from both addresses, through this
 *        back-end's own write() rather than through any state query.
 * @pre Invocation B, with the fake acknowledging the ping.
 * @note The byte layout is asserted rather than only the call - initiator in the high nibble,
 *       destination in the low one - so a ping a peer would see as addressed to the wrong device
 *       cannot pass.
 * @see The unconsumed-methods case, which checks the HAL state counter stays at zero: answering a
 *      poll from the HAL's state would answer a different question entirely.
 */
TEST_F(DriverAidlTransmitTest, PollTransmitsAOneByteFrameCarryingBothAddresses) {
    const LogicalAddress from(LogicalAddress::PLAYBACK_DEVICE_1);
    const LogicalAddress to(LogicalAddress::TV);

    fake->getController()->setSendMessageResult(cechal::SendMessageStatus::ACK_STATE_0);

    EXPECT_NO_THROW({ Driver::getInstance().poll(from, to); })
        << "poll() failed against a HAL that acknowledged the ping";

    expectExactlyOneFrameOfLength(1u);

    const std::vector<uint8_t> sent = fake->getController()->getLastSentMessage();
    ASSERT_EQ(sent.size(), 1u);

    const uint8_t expectedByte =
        static_cast<uint8_t>(((from.toInt() & 0x0F) << 4) | (to.toInt() & 0x0F));
    EXPECT_EQ(sent[0], expectedByte)
        << "the poll byte is not the initiator in the high nibble and the destination in the low "
           "one, so a peer would see the ping as addressed to the wrong device";
}

// A poll the addressed device does not answer raises CECNoAckException, which is how "no device
// there" is reported all the way up to Bus.
//
// A poll is a directed one-byte frame, so ACK_STATE_1 means not acknowledged and the directed
// arm applies. This is the arm bus discovery actually depends on: a poll that reported success
// for an absent device would populate the device list with devices that are not there.
/**
 * @brief A poll the addressed device does not answer raises CECNoAckException, which is how "no
 *        device there" is reported up to Bus.
 * @pre Invocation B, with the fake reporting the directed not-acknowledged value.
 * @note A poll is a directed one-byte frame, so the directed arm applies. This is the arm bus
 *       discovery depends on: a poll reporting success for an absent device would populate the
 *       device list with devices that are not there.
 */
TEST_F(DriverAidlTransmitTest, UnansweredPollRaisesNoAck) {
    const LogicalAddress from(LogicalAddress::PLAYBACK_DEVICE_1);
    const LogicalAddress to(LogicalAddress::TV);

    fake->getController()->setSendMessageResult(cechal::SendMessageStatus::ACK_STATE_1);

    EXPECT_THROW({ Driver::getInstance().poll(from, to); }, CECNoAckException)
        << "an unanswered poll did not raise CECNoAckException, so bus discovery would record "
           "devices that are not present";

    expectExactlyOneFrameOfLength(1u);
}


/** @} */
/** @} */
