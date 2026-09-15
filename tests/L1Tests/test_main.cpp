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

/*
 * L1 TEST HARNESS BOOTSTRAP: the process-global test environment, the legacy HAL
 * double, and the one place that decides which HDMI CEC HAL back-end this process
 * exercises.
 *
 * THE BACK-END SELECTION RESOLVES ONCE PER PROCESS, AND IT RESOLVES HERE.
 * The LibCCEC::init call in SetUp below is the first thing in this binary that forces
 * Driver::getInstance(). Its helper - resolveBackEnd in ccec/src/Driver.cpp, named by
 * symbol rather than by line so that this note survives an edit to a file this one does
 * not own - constructs BOTH back-ends, asks the AIDL one whether its service came up,
 * and emits exactly one selected-path line naming the winner - the line every functional
 * suite, the coverage runner and device-level validation match on, because no
 * introspection API was added to the Driver interface. That choice is then FIXED for the
 * lifetime of the process.
 *
 * Two consequences, and they are the whole reason this file is shaped the way it is:
 * anything that is to influence the selection MUST happen BEFORE that init call, and
 * NOTHING after it can change the outcome. Registering a fake service after init
 * leaves the selection on the legacy back-end and produces a green run that proves
 * nothing at all about the AIDL path. So the mode handling below sits ahead of init by
 * construction, not merely "early in SetUp".
 *
 * CEC_TEST_AIDL_MODE selects what this process finds when it looks the service up. It
 * is read HERE and in tests/L2Tests/test_main.cpp, and NOWHERE ELSE: no production
 * source reads it, and none may.
 *
 *   absent        Register nothing. The lookup finds no service and the legacy
 *                 back-end is selected. An unset or empty variable means exactly this,
 *                 so a plain ./run_L1Tests behaves precisely as it did before the AIDL
 *                 back-end existed, and this mode touches libbinder not at all.
 *                                                                     [invocation A]
 *   compatible    Register an in-process fake reporting its real, frozen metadata.
 *                 The AIDL back-end is selected.                      [invocation B]
 *   incompatible  Register an in-process fake whose interface hash is "-1". The
 *                 service is PRESENT but rejected as incompatible, so the legacy
 *                 back-end is selected and the rejection is logged. Presence alone is
 *                 not sufficient, and this is the mode that proves it. [invocation C]
 *   remote        NOT IMPLEMENTED HERE. It means "launch the out-of-process fake
 *                 host", which is run_L2Tests' job, not this binary's. Seen here it is
 *                 a hard failure naming that runner.
 *
 * An unrecognised value is a HARD FAILURE and never a quiet fall back to absent. A
 * typo that silently downgraded the run to the legacy path would report a green result
 * for an AIDL invocation that never happened, which is the exact class of defect this
 * harness exists to make impossible.
 *
 * WHY AN IN-PROCESS FAKE, AND WHAT IT CAN AND CANNOT PROVE. libbinder resolves a name
 * registered in the calling process to the local BBinder, so interface_cast hands back
 * that very object: no Bp* proxy is created, no transaction crosses the binder driver,
 * and the client threadpool is never involved. That is a real limitation, and it is why
 * a separate L2 tier exists to host the fake in its own process. It is ALSO the only
 * thing that makes the compatibility-rejection branches reachable at all:
 * halcompat::isCompatible (rdk-halif-aidl/common/current/halcompat.h:157-172) rejects
 * an empty or "-1" interface hash at halcompat.h:165-167, and only an object whose
 * getInterfaceHash() dispatches virtually can report such a value. A remote fake
 * cannot, because its generated onTransact answers the metadata transactions from the
 * compiled-in constants. Hence mode incompatible lives here and has no L2 counterpart.
 *
 * THIS FILE DOES NOT START THE BINDER CLIENT THREADPOOL. DriverAidlImpl::open() owns
 * that, and it runs inside the init call below. Starting one here would duplicate an
 * ownership the production back-end already holds.
 */

#include <gtest/gtest.h>
#include <iostream>
#include "hdmi_cec_driver_mock.h"
#include "ccec/LibCCEC.hpp"

/*
 * The fake AIDL service, included unqualified because AM_CPPFLAGS already carries
 * -I$(top_srcdir)/mocks/hdmicec - the same route "hdmi_cec_driver_mock.h" above
 * travels. It supplies the fake, its metadata overrides and the registration entry
 * point that publishes it under the production service name.
 */
#include "fake_hdmi_cec_aidl_service.h"

/*
 * DriverAidlImpl.hpp lives under ccec/src, which AM_CPPFLAGS does not cover, so it is
 * reached by relative path rather than by adding an -I - the same arrangement the ccec/
 * suites use for DriverImpl.hpp, one directory level shallower from here.
 *
 * It is needed for exactly one symbol, DriverAidlImpl::isBinderPreflightOk(), and that
 * one symbol is not a convenience. Before this harness may ask the service manager
 * whether something is already published under the production name it has to reach the
 * service manager, and doing so unguarded is unsafe in two independent ways on the
 * pinned binder stack: with no driver node libbinder ABORTS the process rather than
 * returning an error, and with a driver node but no running servicemanager it BLOCKS
 * INDEFINITELY waiting for binder handle 0. A plain existence check on the driver node
 * would cover only the first. isBinderPreflightOk() covers both - node openable,
 * protocol version equal, handle 0 resolved within a bounded timeout - and its own
 * documentation records that it is public precisely so a test translation unit may call
 * it. Using it here also keeps the driver-node path out of this file, so there is no
 * second spelling of it to drift.
 */
#include "../../ccec/src/DriverAidlImpl.hpp"

/*
 * DriverImpl.hpp, reached by the same relative-path route and for one symbol as well: the
 * class itself, so that the per-test registry restoration below can establish by dynamic_cast
 * WHICH back-end this process resolved to. It needs to know, because restoring the registry
 * means calling Driver::removeLogicalAddress(), and on the AIDL back-end that issues a binder
 * transaction - work this harness must not perform. Nothing else in this file needs the type,
 * and no case is served by it.
 */
#include "../../ccec/src/DriverImpl.hpp"

#include <binder/IServiceManager.h>
#include <utils/String16.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Create mock instance before main
static HdmiCecDriverMock* g_driverMock = nullptr;

/*
 * The fake AIDL service this process published, if any, following the same
 * single-pointer idiom as g_driverMock above.
 *
 * It is held for the lifetime of the process and deliberately never released in
 * TearDown. The pinned C++ IServiceManager exposes NO service-removal API, so the
 * registration cannot be withdrawn once made; the service manager keeps a reference to
 * the published binder, and the middleware may still hold one too. Dropping this
 * reference would therefore not unpublish anything - it would at best destroy an object
 * the service manager still advertises. There is nothing to deregister, and no code
 * below attempts it.
 */
static ::android::sp<FakeHdmiCecService> g_fakeAidlService;

namespace {

/*
 * RENDERING AN UNTRUSTED VALUE INTO A DIAGNOSTIC.  CWE-117, closed by construction.
 *
 * THE DEFECT THIS REMOVES.  Diagnostics in this file used to stream caller-supplied text
 * verbatim.  A value carrying a newline therefore did not appear inside a message - it ENDED
 * that message and began a line of its own, and a line beginning "::error::" is a GitHub
 * Actions workflow command.  Measured before the fix:
 * CEC_TEST_AIDL_MODE=$'bogus\n::error::FORGED_L1_ANNOTATION' produced a standalone forged
 * "::error::" annotation in the run log, and a five-thousand-character value produced more
 * than ten kilobytes of diagnostics, burying the real failure.
 *
 * THE CONTRACT, in the order the steps are applied, because the order is what makes the
 * escaping unambiguous and reversible:
 *
 *   (a) a literal backslash becomes \\ FIRST, so that every escape introduced below is
 *       distinguishable from the same characters occurring literally in the value;
 *   (b) 0x0A becomes \n, 0x0D becomes \r, 0x09 becomes \t, and EVERY OTHER byte outside
 *       printable ASCII 0x20..0x7E becomes \xNN in lower-case hex.  The classification is by
 *       byte value on unsigned char and calls no locale-sensitive function - no isprint, no
 *       iswprint, no ctype table - so it behaves as it would under LC_ALL=C whatever locale
 *       the process is in, and no multi-byte sequence can hide a control character inside
 *       itself;
 *   (c) the rendering is truncated at RENDER_LIMIT characters and
 *       "...[truncated, N bytes total]" is appended when it truncates, N being the value's own
 *       length in bytes.  A caller cannot drown a log, and the message still says how much
 *       was withheld;
 *   (d) it is applied to the VALUE and never to the surrounding message, and the result is
 *       always returned delimited - "..." here - so an empty value is visible as "" rather
 *       than as a gap in a sentence;
 *   (e) it never begins a diagnostic line.  Every message keeps its own prefix in front of it,
 *       and the rendering itself begins with '"'.  Together with (b), which leaves no raw
 *       newline anywhere in the output, that makes a forged standalone ::error::, ::warning::
 *       or ::notice:: line UNREACHABLE BY CONSTRUCTION rather than unlikely.
 *
 * THIS IS ONE OF FIVE COPIES OF ONE CONTRACT.  The five must not diverge; a change to any of
 * them is a change to all five.  The others are:
 *
 *   - hdmicec/tests/L1Tests/run_coverage.sh                      -- render_untrusted()
 *   - hdmicec/.github/workflows/aidl-path-tests-rootfs.sh        -- render_untrusted()
 *   - hdmicec/tests/L1Tests/test_main.cpp                        -- renderUntrustedValue()
 *   - hdmicec/tests/L2Tests/test_main.cpp                        -- renderUntrustedValue()
 *   - hdmicec/mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp  -- renderUntrustedValue()
 *
 * The two convenience overloads are `inline` so that a copy which happens not to need one of
 * them in its own file is not a -Wunused-function warning: the five copies are identical by
 * construction, and which overloads a given file calls is a property of that file's callers.
 *
 * They are five file-local copies rather than one shared helper deliberately: two are shell
 * and three are C++, they live in three build targets and one non-built script, and a shared
 * header would add a build-system edge for a twenty-line function.  The cost of the choice is
 * this cross-reference, which is why every copy carries it.
 */

/** @brief How many rendered characters a diagnostic will carry before it is truncated. */
const std::size_t RENDER_LIMIT = 200;

/**
 * @brief Renders an untrusted byte string as one bounded, escaped, quoted token.
 *
 * The five-clause contract above, implemented once. Every diagnostic in this file that names
 * a value the environment, the command line or another process supplied goes through it.
 *
 * @param [in] value  - The bytes to render. Any content at all is acceptable, including
 *                      embedded newlines, carriage returns, ANSI escape sequences, invalid
 *                      UTF-8 and multi-kilobyte payloads
 *
 * @return std::string  - The rendering, always surrounded by double quotes, always one line,
 *                        never longer than RENDER_LIMIT characters plus the truncation note
 *                        and the two quotes
 *
 * @post The result contains no byte below 0x20 and no byte above 0x7E, so it cannot terminate
 *       the line it sits on and cannot begin a new one.
 *
 * @warning Apply it to the VALUE only. Rendering a whole message would escape the message's
 *          own punctuation and destroy the prefix that clause (e) depends on.
 */
std::string renderUntrustedValue(const char *value, std::size_t length)
{
    static const char HEX_DIGITS[] = "0123456789abcdef";

    std::string rendered;
    rendered.reserve(RENDER_LIMIT + 40);
    rendered.push_back('"');

    std::size_t used = 0;
    bool truncated = false;

    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(value[index]);
        char escape[5];
        std::size_t escapeLength = 0;

        /*
         * The backslash arm is FIRST, and that ordering is the whole of clause (a): escaping it
         * before anything else is introduced is what makes a rendered "\n" unambiguously the
         * two characters the value contained or unambiguously a newline it contained, never
         * either.
         */
        if (byte == '\\') {
            escape[0] = '\\'; escape[1] = '\\'; escapeLength = 2;
        } else if (byte == '\n') {
            escape[0] = '\\'; escape[1] = 'n';  escapeLength = 2;
        } else if (byte == '\r') {
            escape[0] = '\\'; escape[1] = 'r';  escapeLength = 2;
        } else if (byte == '\t') {
            escape[0] = '\\'; escape[1] = 't';  escapeLength = 2;
        } else if (byte >= 0x20u && byte <= 0x7Eu) {
            escape[0] = static_cast<char>(byte); escapeLength = 1;
        } else {
            escape[0] = '\\';
            escape[1] = 'x';
            escape[2] = HEX_DIGITS[(byte >> 4) & 0x0Fu];
            escape[3] = HEX_DIGITS[byte & 0x0Fu];
            escapeLength = 4;
        }

        if (used + escapeLength > RENDER_LIMIT) {
            truncated = true;
            break;
        }

        rendered.append(escape, escapeLength);
        used += escapeLength;
    }

    if (truncated) {
        rendered.append("...[truncated, ");
        rendered.append(std::to_string(static_cast<unsigned long long>(length)));
        rendered.append(" bytes total]");
    }

    rendered.push_back('"');
    return rendered;
}

/** @brief renderUntrustedValue() for a std::string. @see renderUntrustedValue(const char *, std::size_t) */
inline std::string renderUntrustedValue(const std::string &value)
{
    return renderUntrustedValue(value.data(), value.size());
}

/**
 * @brief renderUntrustedValue() for a C string that may be null.
 *
 * A null pointer renders as the four characters <unset>, undelimited, because "unset" and
 * "set to the empty string" are different facts and a diagnostic that showed both as "" would
 * be describing the wrong one.
 *
 * @see renderUntrustedValue(const char *, std::size_t)
 */
inline std::string renderUntrustedValue(const char *value)
{
    if (value == nullptr) {
        return std::string("<unset>");
    }
    return renderUntrustedValue(value, std::strlen(value));
}

/*
 * The harness variable and its four values, spelled exactly once each. These spellings
 * are a fixed contract shared with tests/L2Tests/test_main.cpp, run_coverage.sh, both
 * CI workflows and the test documentation - they are not free to change here.
 */
const char *const AIDL_MODE_VARIABLE     = "CEC_TEST_AIDL_MODE";
const char *const AIDL_MODE_ABSENT       = "absent";
const char *const AIDL_MODE_COMPATIBLE   = "compatible";
const char *const AIDL_MODE_INCOMPATIBLE = "incompatible";
const char *const AIDL_MODE_REMOTE       = "remote";

/*
 * The interface hash halcompat::isCompatible rejects at halcompat.h:165-167, where the
 * accompanying comment reads "hash RPC failed - not a dev build, a broken link". It is
 * the value mode incompatible installs, and reporting it is the only way to drive a
 * PRESENT service down the incompatible arm of the selection.
 */
const char *const BROKEN_INTERFACE_HASH = "-1";

/**
 * @brief Fails the run when something is already published under the production service name
 *
 * A stale registration left behind by another process makes this run's back-end selection
 * resolve against that service instead of the fake this harness publishes, so the run's
 * outcome depends on what happened to be running on the machine. That is a hard failure
 * and not a condition to work around: overwriting the entry hides the collision, and
 * tolerating it makes a green result meaningless.
 *
 * @param [in] serviceName  - Production service name to look up. Supplied by the caller
 *                            from the generated interface rather than from a literal, so
 *                            this harness cannot drift from the name the middleware
 *                            itself looks up.
 *
 * @return None. A collision, or a service manager that cannot be reached, is raised as a
 *         fatal gtest failure.
 *
 * @pre DriverAidlImpl::isBinderPreflightOk() has returned true, so reaching the service
 *      manager here is safe. Called ahead of it, this aborts or blocks the process
 *      instead of failing the test.
 * @post Nothing is published and no registration is withdrawn. This is a lookup only,
 *       and the pinned C++ IServiceManager offers no way to clear a collision.
 * @warning A fatal assertion returns from this function without unwinding its caller, so
 *          a caller invokes it through ASSERT_NO_FATAL_FAILURE and stops on failure.
 *
 * @see publishFakeForMode()
 */
void failIfServiceAlreadyPublished(const std::string &serviceName) {
    const ::android::sp< ::android::IServiceManager> serviceManager =
        ::android::defaultServiceManager();

    ASSERT_TRUE(serviceManager != nullptr)
        << "the binder preflight passed but no service manager could be reached, so this "
           "run cannot establish whether \"" << serviceName << "\" is already published";

    ASSERT_TRUE(serviceManager->checkService(::android::String16(serviceName.c_str())) == nullptr)
        << "\"" << serviceName << "\" is already published by another process. This run's "
           "back-end selection would resolve against that service rather than this "
           "harness's fake, so its outcome would depend on a process this suite does not "
           "own; stop that process and run again";
}

/**
 * @brief Publishes the in-process fake AIDL service, configured for the requested mode
 *
 * The fake's defaults are the real frozen interface version and hash compiled into the
 * snapshot, so mode compatible needs no configuration at all - publishing the fake is the
 * compatible case. Mode incompatible differs by exactly one call, the interface-hash
 * override, which is what keeps the two modes' divergence auditable at a glance.@n
 * Both modes need a usable binder transport, and where there is none this fails the run
 * rather than publishing nothing: publishing nothing would select the legacy back-end and
 * report a green result for an AIDL invocation that never ran.
 *
 * @param [in] mode  - The requested CEC_TEST_AIDL_MODE value, either compatible or
 *                     incompatible. Every other value is resolved by
 *                     applyAidlModeBeforeInit() and never reaches here.
 *
 * @return None. Each failure - no usable binder transport, the name already published, a
 *         fake that cannot be constructed, a fake that cannot be published - is raised as
 *         a fatal gtest failure.
 *
 * @pre The back-end selection has not resolved yet, so this runs ahead of the
 *      LibCCEC::init() call in CecTestEnvironment::SetUp(). The selection resolves once
 *      per process, and a fake published after init leaves the already-resolved selection
 *      on the legacy back-end and produces a green run that proves nothing about the AIDL
 *      path.
 * @pre Nothing is published under the production service name, which
 *      failIfServiceAlreadyPublished() establishes. A service left behind by another
 *      process is a hard failure, because this run's outcome would otherwise depend on a
 *      process the suite does not own.
 * @post The fake is published under the name IHdmiCec::serviceName() reports,
 *       FakeHdmiCecService::setInstance() points the test bodies at it, and a strong
 *       reference to it is held for the lifetime of the process. Nothing withdraws the
 *       registration, because the pinned C++ IServiceManager has no service-removal API.
 * @warning A fatal assertion returns from this function without unwinding its caller, so
 *          a caller invokes it through ASSERT_NO_FATAL_FAILURE and stops on failure.
 *
 * @see applyAidlModeBeforeInit()
 * @see failIfServiceAlreadyPublished()
 */
void publishFakeForMode(const std::string &mode) {
    ASSERT_TRUE(DriverAidlImpl::isBinderPreflightOk())
        << AIDL_MODE_VARIABLE << "=" << mode << " requires a usable binder transport, and this "
           "host does not have one - the driver node is absent or unopenable, its protocol "
           "version differs, or no service manager answered within the bounded timeout. "
           "Failing rather than registering nothing: silently continuing would select the "
           "legacy back-end and report a green result for an AIDL invocation that never ran";

    /*
     * The name comes from the generated interface, never from a literal, so this harness
     * cannot drift from the name the middleware actually looks up.
     */
    const std::string &serviceName = ::com::rdk::hal::hdmicec::IHdmiCec::serviceName();
    ASSERT_NO_FATAL_FAILURE(failIfServiceAlreadyPublished(serviceName));

    ::android::sp<FakeHdmiCecService> fake = ::android::sp<FakeHdmiCecService>::make();
    ASSERT_TRUE(fake != nullptr) << "the fake AIDL service could not be constructed";

    if (mode == AIDL_MODE_INCOMPATIBLE) {
        fake->setInterfaceHash(BROKEN_INTERFACE_HASH);
    }

    ASSERT_TRUE(registerFakeHdmiCecService(fake))
        << "the fake AIDL service could not be published as \"" << serviceName << "\", so "
           "mode " << mode << " cannot be exercised at all";

    /*
     * Publish the pointer the test bodies reach the fake through, and keep the strong
     * reference alive. SetUp runs before any TEST_F body, so this ordering is what lets a
     * case configure or observe a fake that was registered long before it ran.
     */
    FakeHdmiCecService::setInstance(fake.get());
    g_fakeAidlService = fake;

    std::cout << "[CecTestEnvironment] Published the fake AIDL service as \"" << serviceName
              << "\" for " << AIDL_MODE_VARIABLE << "=" << mode << std::endl;
}

/**
 * @brief Reads CEC_TEST_AIDL_MODE and does what it asks, before the selection resolves
 *
 * The four values, and what each one makes the service lookup find, are set out in the
 * note at the top of this translation unit; this function is the one place in run_L1Tests
 * that acts on them. Mode remote is the exception: it means "launch the out-of-process
 * fake service host", which only run_L2Tests does, so here it is a hard failure naming
 * that runner.@n
 * Every path that does not need libbinder avoids it entirely, which is what keeps the
 * default invocation runnable on a host with no kernel binder support: absent returns
 * without touching it, and both rejection paths fail before reaching it.
 *
 * @return None. An unrecognised value, mode remote, and any failure to publish the fake
 *         are each raised as a fatal gtest failure.
 *
 * @pre Called ahead of the LibCCEC::init() call in CecTestEnvironment::SetUp(), which is
 *      the first thing in this binary that forces Driver::getInstance(). The selection
 *      resolves once per process and is fixed for its lifetime from then on, so a service
 *      reached after init leaves the already-resolved selection on the legacy back-end and
 *      produces a green run that proves nothing about the AIDL path.
 * @post For absent, nothing is published and libbinder has not been touched. For
 *       compatible and incompatible, publishFakeForMode() has published the fake and this
 *       process holds a reference to it.
 * @warning An unrecognised value is a hard failure, never a quiet fall back to absent. A
 *          typo that downgraded the run to the legacy back-end would report a green result
 *          for an AIDL invocation that never happened, which is the class of defect this
 *          harness exists to make impossible.
 * @warning A fatal assertion returns from this function without unwinding its caller, so
 *          CecTestEnvironment::SetUp() invokes it through ASSERT_NO_FATAL_FAILURE.
 *
 * @see publishFakeForMode()
 */
void applyAidlModeBeforeInit() {
    const char *const requested = ::getenv(AIDL_MODE_VARIABLE);

    // Unset and empty both mean absent, so the pre-existing behaviour is the default.
    const std::string mode =
        (requested != nullptr && requested[0] != '\0') ? requested : AIDL_MODE_ABSENT;

    if (mode == AIDL_MODE_ABSENT) {
        std::cout << "[CecTestEnvironment] " << AIDL_MODE_VARIABLE << "=" << mode
                  << ": registering no AIDL service, so the legacy back-end is expected"
                  << std::endl;
        return;
    }

    if (mode == AIDL_MODE_COMPATIBLE || mode == AIDL_MODE_INCOMPATIBLE) {
        ASSERT_NO_FATAL_FAILURE(publishFakeForMode(mode));
        return;
    }

    if (mode == AIDL_MODE_REMOTE) {
        FAIL() << AIDL_MODE_VARIABLE << "=" << mode << " is not implemented by run_L1Tests. It "
                  "means \"launch the out-of-process fake service host\", which only run_L2Tests "
                  "does; an in-process registration cannot produce a proxy, a driver transaction "
                  "or a callback on a binder thread. Run run_L2Tests for this mode";
        return;
    }

    /*
     * THE VALUE IS RENDERED, NOT STREAMED. This is the one diagnostic in this binary that
     * names a value nothing has validated - every arm above it matched a known spelling - so
     * it is the boundary the log-injection contract applies at. Streamed raw, a value of
     * $'bogus\n::error::FORGED' ended this message and began a standalone GitHub workflow
     * command on the next line; renderUntrustedValue() leaves no newline for it to end on,
     * bounds the length, and keeps the sentence's own words in front of the value.
     */
    FAIL() << AIDL_MODE_VARIABLE << " is set to " << renderUntrustedValue(mode)
           << ", which is not a recognised mode. The four are " << AIDL_MODE_ABSENT << ", "
           << AIDL_MODE_COMPATIBLE << ", " << AIDL_MODE_INCOMPATIBLE << " and "
           << AIDL_MODE_REMOTE << ". Refusing to fall back to " << AIDL_MODE_ABSENT
           << ", because a typo must not quietly downgrade the run to the legacy back-end and "
              "report it as a pass";
}

/*
 * PER-TEST RESTORATION OF THE ONE PIECE OF DRIVER STATE THAT OUTLIVES A TEST.
 *
 * THE DEFECT THIS EXISTS TO REMOVE. The driver is a process singleton, and its list of
 * acquired logical addresses is the only piece of its state a test can add to and that
 * nothing takes away again: DriverImpl::close() deliberately does not clear the list - the
 * AIDL back-end matches it, and clearing would be an unauthorized improvement to legacy
 * behaviour - so a registration made by one case is still there when the next case runs. Two
 * groups of cases in this binary sit on opposite sides of that: some register an address and
 * do not remove it in their teardown, and others assert the negative precondition that a
 * particular address is NOT registered, because Connection::matchSource() only rewrites a
 * frame's source nibble when Driver::isValidLogicalAddress() says the address is acquired.
 *
 * In declaration order the negative-precondition cases happen to run first and everything
 * passes. Under `--gtest_shuffle` - a valid order, and one CI is entitled to use - the
 * registering cases can run first, and then the negative-precondition cases fail with a
 * rewritten source nibble. Measured: seeds 12345 and 99999 each failed exactly two cases,
 * seed 54321 passed. That is a false regression produced by test order alone, and a suite
 * whose verdict depends on its order cannot be used to certify anything.
 *
 * WHY THE FIX IS HERE AND NOT IN A FIXTURE TEARDOWN. Two reasons, and the second is the one
 * that matters. First, the twelve pre-existing L1 units are outside this migration's diff by
 * design, and an acceptance check enforces that. Second - and this would be the right place
 * even without that boundary - a teardown added to the two registering fixtures would make
 * exactly those two fixtures order-independent and leave every other fixture, and every
 * fixture added later, free to reintroduce the same leak. A process-global listener makes the
 * property hold for all of them: whatever a case registers, the next case starts from the
 * registry the FIRST case started from.
 *
 * WHAT IT DOES NOT DO, deliberately:
 *
 *  - it does not touch production close/term semantics. The registry is restored from
 *    OUTSIDE the driver, through the driver's own public interface, and no production file
 *    changes. A close() that leaves the list populated still leaves it populated;
 *  - it does nothing at all when nothing leaked, which is the case after all but a handful
 *    of the cases in this binary. Detection is a pure list walk under the driver's own lock
 *    (Driver::isValidLogicalAddress() on both back-ends reaches no HAL and no service), so
 *    the common path costs fifteen list walks and issues no HAL call whatsoever;
 *  - it issues NO BINDER CALL, ever. Driver::removeLogicalAddress() on the AIDL back-end is
 *    a transaction, so restoration is performed only when the resolved back-end is the legacy
 *    one; under an AIDL selection a residual registration is reported and left, which is
 *    honest and visible. The order-dependence this guard removes is a legacy-invocation
 *    problem in any case: the cases that leak a registration are the LibCCEC ones, which the
 *    AIDL invocations' filters exclude, while the AIDL session fixture's own cases add and
 *    remove their addresses inside a single case;
 *  - it never fails a test. Everything it calls is wrapped, because a restoration problem
 *    must not be reported against a case that has already produced its own result - it would
 *    attribute a harness fault to unrelated code. It reports loudly on stdout instead, which
 *    is where a reader looking at a later failure will find it.
 *
 * ON THE GMOCK WARNING IT PRODUCES. Restoring on the legacy back-end reaches
 * HdmiCecRemoveLogicalAddress() on the process-global mock, which no expectation covers, so
 * gmock prints its "Uninteresting mock function call" warning and takes the ON_CALL default
 * the mock already installs (hdmi_cec_driver_mock.cpp, returning HDMI_CEC_IO_SUCCESS).
 * That warning is accepted rather than silenced, and the alternative was measured and
 * rejected: installing a permissive EXPECT_CALL and then calling
 * ::testing::Mock::VerifyAndClearExpectations() would clear EVERY expectation live on the
 * mock at that moment, including one a case had legitimately left unmet, and would therefore
 * be capable of hiding a real failure. A warning that is explained is better than a verifier
 * that can lie. ::testing::Mock::AllowUninterestingCalls() would express this exactly and is
 * private in GoogleTest 1.15, so it is not available. An explanatory line is logged
 * immediately before the removals so the warning is never unaccounted for.
 */
class LogicalAddressRegistryGuard : public ::testing::EmptyTestEventListener {
public:
    /**
     * @brief Creates the guard with no baseline yet, so nothing is probed before a test runs
     *
     * @post No baseline is held and Driver::getInstance() has not been called. The baseline is
     *       taken lazily at the start of the first test, which is deliberate: a run whose
     *       environment SetUp failed fatally never starts a test, and this guard must not be
     *       the thing that forces the back-end selection on such a run.
     */
    LogicalAddressRegistryGuard(void) : baselineCaptured(false) {
        for (int index = 0; index < ASSIGNABLE_ADDRESS_COUNT; index++) {
            baselineRegistered[index] = false;
        }
    }

    /**
     * @brief Captures the registry the suite starts from, once, at the first test
     *
     * @param [in] testInfo  - The test about to run. Unused; the first call is what matters,
     *                         not which case it belongs to.
     *
     * @return None.
     *
     * @pre The global environment's SetUp() has completed, so LibCCEC::init() has already
     *      forced the back-end selection and this call cannot be what resolves it.
     * @post The baseline is held for the lifetime of the process. It is captured rather than
     *       assumed empty, so an address that init() itself acquired - none does today - would
     *       be treated as part of the starting state instead of being torn out from under
     *       every case.
     */
    void OnTestStart(const ::testing::TestInfo & /* testInfo */) override {
        if (baselineCaptured) {
            return;
        }

        for (int address = FIRST_ASSIGNABLE_ADDRESS; address <= LAST_ASSIGNABLE_ADDRESS;
             address++) {
            baselineRegistered[address - FIRST_ASSIGNABLE_ADDRESS] = isRegistered(address);
        }

        baselineCaptured = true;
    }

    /**
     * @brief Returns the registry to the captured baseline, so the next case starts where the
     *        first one did
     *
     * @param [in] testInfo  - The case that has just finished, named in the report so a leak
     *                         is attributed to the case that made it rather than to the case
     *                         that would have tripped over it.
     *
     * @return None. Nothing here can fail a test: every call is wrapped and every problem is
     *         reported on stdout.
     *
     * @pre Called after the case's own TearDown, which is where GoogleTest sequences listener
     *      OnTestEnd, so a fixture that cleans up after itself has already done so and this
     *      finds nothing to do.
     * @post Every address registered after the baseline was captured has been removed, or the
     *       reason it could not be is reported. Addresses that were part of the baseline are
     *       left exactly as they are.
     * @warning Restoration reaches the legacy HAL mock, which produces one gmock warning per
     *          removal. The note above this class records why that is accepted rather than
     *          suppressed, and an explanatory line is logged immediately before it happens.
     */
    void OnTestEnd(const ::testing::TestInfo &testInfo) override {
        if (!baselineCaptured) {
            return;
        }

        std::vector<int> leaked;

        for (int address = FIRST_ASSIGNABLE_ADDRESS; address <= LAST_ASSIGNABLE_ADDRESS;
             address++) {
            if (isRegistered(address) &&
                !baselineRegistered[address - FIRST_ASSIGNABLE_ADDRESS]) {
                leaked.push_back(address);
            }
        }

        // The overwhelmingly common case: no case in this binary leaked anything, so there is
        // nothing to log, nothing to remove and no HAL call to make.
        if (leaked.empty()) {
            return;
        }

        std::cout << "[LogicalAddressRegistryGuard] " << testInfo.test_suite_name() << "."
                  << testInfo.name() << " left " << leaked.size()
                  << " logical address(es) registered in the process-global driver:";

        for (size_t entry = 0; entry < leaked.size(); entry++) {
            std::cout << " " << LogicalAddress(leaked[entry]).toString();
        }

        std::cout << ". Removing them so the next case starts from the registry the first case "
                     "started from; the gmock \"uninteresting call\" warnings that follow are "
                     "this removal reaching HdmiCecRemoveLogicalAddress and are expected."
                  << std::endl;

        if (!restore(leaked)) {
            return;
        }

        // Verified rather than assumed. A removal that reported nothing and changed nothing
        // would otherwise leave the next case to fail for a reason nothing in the log explains.
        for (size_t entry = 0; entry < leaked.size(); entry++) {
            if (isRegistered(leaked[entry])) {
                const std::string name = LogicalAddress(leaked[entry]).toString();

                std::cout << "[LogicalAddressRegistryGuard] " << name
                          << " is STILL registered after removal. A later case that requires it "
                             "absent will fail, and this line - not that case - is where the "
                             "cause is." << std::endl;
            }
        }
    }

private:
    /**
     * @brief The assignable logical addresses, which are the only ones a driver can hold.
     *
     * 0x0 to 0xE inclusive. 0xF is UNREGISTERED/BROADCAST - a destination, never an address a
     * device acquires - and the AIDL controller documents the same range for
     * addLogicalAddresses(), so probing it would ask about a value neither back-end can hold.
     */
    enum {
        FIRST_ASSIGNABLE_ADDRESS = LogicalAddress::TV,
        LAST_ASSIGNABLE_ADDRESS  = LogicalAddress::SPECIFIC_USE,
        ASSIGNABLE_ADDRESS_COUNT = (LAST_ASSIGNABLE_ADDRESS - FIRST_ASSIGNABLE_ADDRESS) + 1
    };

    /**
     * @brief Whether the driver currently holds @p address, without ever raising
     *
     * @param [in] address  - Assignable logical address to probe.
     * @return bool - Whether the driver reports the address as acquired. False is also the
     *                answer when the query itself failed, which is the safe direction: it
     *                leads to no removal being attempted.
     *
     * @note Driver::isValidLogicalAddress() is a list walk under the driver's own lock on both
     *       back-ends and reaches no HAL, no service and no binder transaction, which is what
     *       makes probing every address after every test free. It does not check the driver's
     *       lifecycle state either, so it answers on a closed driver exactly as it does on an
     *       open one - which is the answer that matters here, since the registry survives a
     *       close.
     */
    bool isRegistered(int address) {
        try {
            return Driver::getInstance().isValidLogicalAddress(LogicalAddress(address));
        }
        catch (...) {
            return false;
        }
    }

    /**
     * @brief Removes every leaked address through the driver's own public interface
     *
     * @param [in] leaked  - The addresses registered after the baseline was captured.
     * @return bool - Whether removal was attempted. False when the resolved back-end is not
     *                the legacy one, in which case nothing was called and the caller must not
     *                report a residual registration as a failure of a removal that never ran.
     *
     * @post On the legacy back-end every address in @p leaked has had removeLogicalAddress()
     *       called for it. On any other back-end nothing was called at all.
     * @warning An InvalidStateException here is expected rather than exceptional: the driver
     *          must be OPENED for a removal to be accepted, and a case that terminated the
     *          library leaves it CLOSED. That is reported and the loop continues, because the
     *          remaining addresses are worth attempting and because nothing this guard does
     *          may fail a test.
     */
    bool restore(const std::vector<int> &leaked) {
        Driver &driver = Driver::getInstance();

        if (dynamic_cast<DriverImpl *>(&driver) == NULL) {
            std::cout << "[LogicalAddressRegistryGuard] the resolved back-end is not the legacy "
                         "one, so these addresses are LEFT REGISTERED: removing them would issue "
                         "a binder transaction, and this harness issues none. Reported rather "
                         "than passed over in silence, because a later case that needs one of "
                         "these addresses absent would be reading the state this line names."
                      << std::endl;

            return false;
        }

        for (size_t entry = 0; entry < leaked.size(); entry++) {
            try {
                driver.removeLogicalAddress(LogicalAddress(leaked[entry]));
            }
            catch (InvalidStateException &) {
                std::cout << "[LogicalAddressRegistryGuard] "
                          << LogicalAddress(leaked[entry]).toString()
                          << " could not be removed because the driver is not OPENED. The "
                             "registry survives a close, so this address remains registered "
                             "until something reopens the driver and a later case may see it."
                          << std::endl;
            }
            catch (...) {
                std::cout << "[LogicalAddressRegistryGuard] removing "
                          << LogicalAddress(leaked[entry]).toString()
                          << " raised. The address may remain registered." << std::endl;
            }
        }

        return true;
    }

    /** @brief Whether the registry the suite starts from has been captured yet. */
    bool baselineCaptured;

    /**
     * @brief The registry as the first case found it, indexed from FIRST_ASSIGNABLE_ADDRESS.
     *
     * Held rather than assumed empty so that an address the starting state legitimately holds
     * is never removed. It is empty in practice on this binary, and asserting that would be
     * asserting a property of LibCCEC::init() from the wrong place.
     */
    bool baselineRegistered[ASSIGNABLE_ADDRESS_COUNT];
};

} // namespace

// Global test environment to set up mocks
class CecTestEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // Create and install the driver mock
        g_driverMock = new HdmiCecDriverMock();
        HdmiCecDriverMock::setInstance(g_driverMock);
        
        // Decide what the service lookup inside init() below is going to find. This runs
        // before init() because init() is the one-way door described at the top of this
        // file, and a failure here stops the run rather than letting init() resolve a
        // selection the requested mode did not ask for.
        ASSERT_NO_FATAL_FAILURE(applyAidlModeBeforeInit());

        // Initialize the Bus so it's ready for tests
        //
        // A failure here is fatal to the whole run. This SetUp runs once for the binary,
        // and every driver-dependent case in it takes an initialized CEC stack as its
        // precondition, so a suite that carried on past a failed init would assert against
        // an unopened stack and report a green result for a process that never came up.
        // There is nothing here that can legitimately be ignored either: init() raises
        // InvalidStateException when called a second time, but running exactly once per
        // process means a correct run never reaches that condition, and the exceptions it
        // can actually raise - Driver::getInstance().open() refused by the HAL, or
        // Bus::getInstance().start() failing - are all real failures.
        ASSERT_NO_THROW({ LibCCEC::getInstance().init("CEC_TEST"); })
            << "the CEC library could not be initialized, so not one suite in this binary "
               "has its precondition; continuing would assert against an uninitialized stack";
    }
    
    void TearDown() override {
        // Clean up
        //
        // Reported rather than swallowed, but NON-FATALLY and deliberately so. TearDown
        // runs after the suite, so every result this binary produced has already been
        // recorded; aborting here would obscure results that were legitimately earned, and
        // a fatal assertion would also cut the three cleanup statements below short. A
        // non-fatal expectation makes a failing term() visible while leaving the rest of
        // the cleanup to run to completion.
        //
        // One failure here is expected and honest rather than spurious: when init() failed
        // in SetUp the library was never initialized, so term() raises InvalidStateException
        // and is reported as a second failure alongside the first. That is the correct
        // outcome - it says the process never came up - and it is not worth suppressing.
        //
        // Nothing unpublishes the fake AIDL service, if one was published. The pinned C++
        // IServiceManager has no service-removal API, so there is nothing to deregister;
        // g_fakeAidlService above records why its reference is held to the end instead.
        EXPECT_NO_THROW({ LibCCEC::getInstance().term(); })
            << "the CEC library could not be terminated cleanly; the remaining cleanup below "
               "still runs, but this process did not shut the CEC stack down properly";
        
        HdmiCecDriverMock::setInstance(nullptr);
        delete g_driverMock;
        g_driverMock = nullptr;
    }
};

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new CecTestEnvironment);

    // The per-test registry restoration described above LogicalAddressRegistryGuard, which is
    // what makes this binary's result independent of the order its cases run in - including
    // under --gtest_shuffle, which CI is entitled to use and which used to fail two cases on
    // two of three sampled seeds.
    //
    // APPENDED, so it runs after the default result printer's own OnTestEnd: the printer emits
    // the case's [ OK ] or [ FAILED ] line first, and any report this guard makes then appears
    // beneath the case that caused it rather than above it. GoogleTest takes ownership of the
    // listener, which is why nothing here deletes it.
    ::testing::UnitTest::GetInstance()->listeners().Append(new LogicalAddressRegistryGuard());

    return RUN_ALL_TESTS();
}
