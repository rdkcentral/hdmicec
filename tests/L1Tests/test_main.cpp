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
 * @file
 * @brief L1 harness bootstrap: test environment, legacy HAL mock and the CEC_TEST_AIDL_MODE switch
 *
 * LibCCEC::init() in CecTestEnvironment::SetUp() fixes the back-end selection for the process, so
 * everything that influences it runs first. CEC_TEST_AIDL_MODE, read only by the test harnesses
 * and never by production code, chooses what the service lookup finds: absent (also unset or
 * empty; nothing registered, legacy selected), compatible (in-process fake with its real frozen
 * metadata; AIDL selected), incompatible (fake reports interface hash "-1"; rejected, legacy
 * selected) or remote (hard failure; only run_L2Tests launches the fake host), with any other
 * value a hard failure. This file does not start the binder client threadpool;
 * DriverAidlImpl::open() owns it.
 */

#include <gtest/gtest.h>
#include <iostream>
#include "hdmi_cec_driver_mock.h"
#include "ccec/LibCCEC.hpp"

// The fake AIDL service, its metadata overrides and its registration entry point, reached through
// the -I$(top_srcdir)/mocks/hdmicec that AM_CPPFLAGS already carries.
#include "fake_hdmi_cec_aidl_service.h"

// Reached by relative path, as ccec/src is not on AM_CPPFLAGS, for the private bounded preflight
// that must pass before the service manager is touched, called through BinderPreflightTestAccess.
#include "../../ccec/src/DriverAidlImpl.hpp"

// Reached the same way for the class alone: registry restoration below uses dynamic_cast to detect
// the legacy back-end, because a removal on the AIDL back-end is a binder transaction.
#include "../../ccec/src/DriverImpl.hpp"

#include <binder/IServiceManager.h>
#include <utils/String16.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

CCEC_BEGIN_NAMESPACE

/**
 * @brief Test-only gateway to private DriverAidlImpl::isBinderPreflightOk(), which befriends it.
 *
 * Forwards its arguments unchanged, so the predicate's own defaults apply.
 * ccec/test_DriverAidl.cpp defines it token-identically, as the one-definition rule requires.
 */
struct BinderPreflightTestAccess {
    /**
     * @brief Calls DriverAidlImpl::isBinderPreflightOk() with @p args, forwarded unchanged.
     *
     * @param [in] args - The predicate's leading arguments, in its parameter order.
     *
     * @return bool - The predicate's verdict.
     */
    template <typename... Args>
    static bool isBinderPreflightOk(Args &&...args) {
        return DriverAidlImpl::isBinderPreflightOk(std::forward<Args>(args)...);
    }
};

CCEC_END_NAMESPACE

// Create mock instance before main
static HdmiCecDriverMock* g_driverMock = nullptr;

/**
 * @brief The fake AIDL service this process published, if any
 *
 * Held for the lifetime of the process and never released: the pinned C++ IServiceManager has no
 * service-removal API, so the registration cannot be withdrawn.
 */
static ::android::sp<FakeHdmiCecService> g_fakeAidlService;

namespace {

// Log-injection (CWE-117) rendering contract. It is one of five copies that must not diverge; the
// full contract and the list of copies are in AIDL_HAL_MIGRATION_NOTES.md.

/** @brief How many rendered characters a diagnostic will carry before it is truncated. */
const std::size_t RENDER_LIMIT = 200;

/**
 * @brief Renders an untrusted byte string as one bounded, escaped, quoted token
 *
 * A backslash is escaped first; newline, carriage return and tab become `\n`, `\r` and `\t`, and
 * every other byte outside 0x20..0x7E becomes `\xNN`, classified by byte value without locale.
 * Output beyond RENDER_LIMIT characters is cut and "...[truncated, N bytes total]" appended.
 *
 * @param [in] value   - The bytes to render; any content is acceptable.
 * @param [in] length  - Number of bytes in @p value.
 *
 * @return std::string  - The rendering, double-quoted and on one line.
 * @post The result holds no byte below 0x20 or above 0x7E, so it can neither end nor begin a line.
 * @warning Apply it to the value only, never to the whole message, whose prefix must stay first.
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

        // The backslash arm is first, so every escape introduced below stays unambiguous.
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

/**
 * @brief renderUntrustedValue() for a std::string.
 *
 * @param [in] value   - The string to render; any content is acceptable.
 *
 * @return std::string  - The rendering, double-quoted and on one line.
 * @see renderUntrustedValue(const char *, std::size_t)
 */
inline std::string renderUntrustedValue(const std::string &value)
{
    return renderUntrustedValue(value.data(), value.size());
}

/**
 * @brief renderUntrustedValue() for a C string that may be null
 *
 * A null pointer renders as the undelimited `<unset>`, so "unset" stays distinct from "".
 *
 * @param [in] value   - The C string to render, or null.
 *
 * @return std::string  - `<unset>` for null, else the quoted rendering.
 * @see renderUntrustedValue(const char *, std::size_t)
 */
inline std::string renderUntrustedValue(const char *value)
{
    if (value == nullptr) {
        return std::string("<unset>");
    }
    return renderUntrustedValue(value, std::strlen(value));
}

// The harness variable and its four values, spelled once each: a fixed contract shared with the L2
// harness, run_coverage.sh, both CI workflows and the test documentation.
const char *const AIDL_MODE_VARIABLE     = "CEC_TEST_AIDL_MODE";
const char *const AIDL_MODE_ABSENT       = "absent";
const char *const AIDL_MODE_COMPATIBLE   = "compatible";
const char *const AIDL_MODE_INCOMPATIBLE = "incompatible";
const char *const AIDL_MODE_REMOTE       = "remote";

/**
 * @brief Interface hash that halcompat::isCompatible() rejects as a broken link
 *
 * Mode incompatible installs it to drive a present service down the incompatible arm.
 */
const char *const BROKEN_INTERFACE_HASH = "-1";

/**
 * @brief Fails the run when something is already published under the production service name
 *
 * A stale registration would make the selection resolve against it rather than this harness's
 * fake, so it is a hard failure, neither overwritten nor tolerated.
 *
 * @param [in] serviceName  - Production service name, taken from the generated interface.
 *
 * @pre DriverAidlImpl::isBinderPreflightOk() has returned true.
 * @post Nothing is published or withdrawn; this is a lookup only.
 * @warning Fatal assertions return without unwinding the caller; call via ASSERT_NO_FATAL_FAILURE.
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
 * Compatible needs no setup, as the fake defaults to the real frozen version and hash, and
 * incompatible adds one interface-hash override. With no usable binder transport the run fails.
 *
 * @param [in] mode  - compatible or incompatible; applyAidlModeBeforeInit() resolves the others.
 *
 * @pre The back-end selection has not resolved yet; see applyAidlModeBeforeInit().
 * @post The fake is published as IHdmiCec::serviceName(), installed through
 *       FakeHdmiCecService::setInstance() and held for the lifetime of the process.
 * @warning Fatal assertions return without unwinding the caller; call via ASSERT_NO_FATAL_FAILURE.
 * @see applyAidlModeBeforeInit(), failIfServiceAlreadyPublished()
 */
void publishFakeForMode(const std::string &mode) {
    ASSERT_TRUE(BinderPreflightTestAccess::isBinderPreflightOk())
        << AIDL_MODE_VARIABLE << "=" << mode << " requires a usable binder transport, and this "
           "host does not have one - the driver node is absent or unopenable, its protocol "
           "version differs, or no service manager answered within the bounded timeout. "
           "Failing rather than registering nothing: silently continuing would select the "
           "legacy back-end and report a green result for an AIDL invocation that never ran";

    // From the generated interface, never a literal, so this harness cannot drift from the name the
    // middleware looks up.
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

    // SetUp runs before any TEST_F body, so every case can reach the fake through this pointer; the
    // strong reference keeps it alive.
    FakeHdmiCecService::setInstance(fake.get());
    g_fakeAidlService = fake;

    std::cout << "[CecTestEnvironment] Published the fake AIDL service as \"" << serviceName
              << "\" for " << AIDL_MODE_VARIABLE << "=" << mode << std::endl;
}

/**
 * @brief Returns CEC_TEST_AIDL_MODE as this harness acts on it, an unset or empty value as absent
 *
 * @return std::string  - The mode, not yet validated; applyAidlModeBeforeInit() refuses unknowns.
 * @see applyAidlModeBeforeInit(), failUnlessSelectedBackEndMatchesMode()
 */
std::string resolvedAidlMode() {
    const char *const requested = ::getenv(AIDL_MODE_VARIABLE);
    return (requested != nullptr && requested[0] != '\0') ? std::string(requested)
                                                          : std::string(AIDL_MODE_ABSENT);
}

/**
 * @brief Reads CEC_TEST_AIDL_MODE and acts on it before the back-end selection resolves
 *
 * The modes are described in the file comment; remote is a hard failure here because only
 * run_L2Tests launches the out-of-process fake host. Absent publishes nothing, and where no binder
 * driver node exists the selection's preflight declines before libbinder is reached, so the default
 * invocation runs on a host without kernel binder support.
 *
 * @pre Called before LibCCEC::init() in CecTestEnvironment::SetUp(), which fixes the selection.
 * @post For absent nothing is published; otherwise publishFakeForMode() has published the fake.
 * @warning An unrecognised value is a hard failure, never a quiet fall back to absent.
 * @see publishFakeForMode()
 */
void applyAidlModeBeforeInit() {
    const std::string mode = resolvedAidlMode();

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

    // Rendered, not streamed: this is the one diagnostic naming an unvalidated value, so a newline
    // in it must not end this message and forge a workflow command.
    FAIL() << AIDL_MODE_VARIABLE << " is set to " << renderUntrustedValue(mode)
           << ", which is not a recognised mode. The four are " << AIDL_MODE_ABSENT << ", "
           << AIDL_MODE_COMPATIBLE << ", " << AIDL_MODE_INCOMPATIBLE << " and "
           << AIDL_MODE_REMOTE << ". Refusing to fall back to " << AIDL_MODE_ABSENT
           << ", because a typo must not quietly downgrade the run to the legacy back-end and "
              "report it as a pass";
}

/**
 * @brief Fails the run unless LibCCEC::init() selected the back-end that @p mode requires
 *
 * Absent and incompatible require the legacy back-end, compatible the AIDL one. Identity is read
 * by dynamic_cast, so no binder call is made; under absent an AIDL selection means another process
 * already publishes the production service name.
 *
 * @param [in] mode  - The mode applyAidlModeBeforeInit() accepted, from resolvedAidlMode().
 *
 * @pre LibCCEC::init() has returned, so Driver::getInstance() is resolved for the process.
 * @warning Fatal assertions return without unwinding the caller; call via ASSERT_NO_FATAL_FAILURE.
 * @see failIfServiceAlreadyPublished()
 */
void failUnlessSelectedBackEndMatchesMode(const std::string &mode) {
    Driver &driver = Driver::getInstance();
    const bool legacySelected = (dynamic_cast<DriverImpl *>(&driver) != NULL);
    const bool aidlSelected = (dynamic_cast<DriverAidlImpl *>(&driver) != NULL);
    const char *const selected = legacySelected ? "the legacy back-end"
                                 : aidlSelected ? "the AIDL back-end"
                                                : "a back-end of neither concrete type";

    if (mode == AIDL_MODE_ABSENT) {
        ASSERT_TRUE(legacySelected)
            << AIDL_MODE_VARIABLE << "=" << mode << " registered no AIDL service, yet the factory "
               "selected " << selected << ", so \""
            << ::com::rdk::hal::hdmicec::IHdmiCec::serviceName() << "\" is already published by "
               "another process: a stale registration. This run's outcome would depend on a "
               "process this suite does not own; stop that process and run again";
        return;
    }

    const bool aidlRequired = (mode == AIDL_MODE_COMPATIBLE);
    ASSERT_TRUE(aidlRequired ? aidlSelected : legacySelected)
        << AIDL_MODE_VARIABLE << "=" << mode << " requires the "
        << (aidlRequired ? "AIDL" : "legacy") << " back-end, but the factory selected "
        << selected << ", so no case result in this run is evidence for the mode it was given";
}

/**
 * @brief Test listener that detects and reports logical addresses a case leaves registered
 *
 * The driver is a process singleton whose address list survives close(), so an address one case
 * leaves registered can fail a later case under --gtest_shuffle. After each case it reports every
 * address added since the first case began and, on the legacy back-end only, tries to remove each.
 *
 * @warning It never fails a test and issues no binder call: under an AIDL selection a leaked
 *          address is reported and left registered.
 * @note Legacy removals reach the HAL mock with no expectation set, so gmock prints an accepted
 *       "Uninteresting mock function call" warning, announced by a log line beforehand.
 * @see Driver::isValidLogicalAddress(), Driver::removeLogicalAddress()
 */
class LogicalAddressRegistryGuard : public ::testing::EmptyTestEventListener {
public:
    /**
     * @brief Creates the guard with no baseline yet, so nothing is probed before a test runs
     *
     * @post Driver::getInstance() has not been called; the baseline is taken at the first test, so
     *       a run whose environment SetUp failed never has its selection forced by this guard.
     */
    LogicalAddressRegistryGuard(void) : baselineCaptured(false) {
        for (int index = 0; index < ASSIGNABLE_ADDRESS_COUNT; index++) {
            baselineRegistered[index] = false;
        }
    }

    /**
     * @brief Captures the registry the suite starts from, once, at the first test
     *
     * @param [in] testInfo  - The test about to run; unused.
     *
     * @pre The environment's SetUp() has run LibCCEC::init(), so the selection is already resolved.
     * @post The baseline is held for the lifetime of the process; the address init() acquires on
     *       the AIDL back-end (none on legacy) is part of it and is never removed.
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
     * @brief Reports addresses added since the baseline and, on legacy, tries to remove them
     *
     * @param [in] testInfo  - The case that just finished, to which the report attributes a leak.
     *
     * @pre Runs after the case's own TearDown, so a fixture that cleans up leaves nothing to do.
     * @post Added addresses were reported and, on legacy, removal attempted with failures logged;
     *       baseline addresses are neither removed nor re-added.
     * @warning Never fails a test; each legacy removal produces one gmock warning, announced first.
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
     * @brief The assignable logical addresses 0x0 to 0xE, the only ones a driver can hold
     *
     * 0xF is UNREGISTERED/BROADCAST, a destination that neither back-end can acquire.
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
     * @return bool - Whether the address is acquired; also false when the query itself failed.
     *
     * @note Driver::isValidLogicalAddress() walks the local list on both back-ends, reaches no HAL
     *       and ignores lifecycle state, so it answers on a closed driver too.
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
     * @return bool - Whether removal was attempted; false when the back-end is not the legacy one.
     *
     * @post On the legacy back-end removeLogicalAddress() was called for each address.
     * @warning An InvalidStateException, raised once a case has terminated the library, is reported
     *          and the loop continues.
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

    /** @brief The registry as the first case found it, indexed from FIRST_ASSIGNABLE_ADDRESS. */
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
        
        // Decide what the service lookup inside init() will find. This must precede init(), which
        // fixes the back-end selection, and a failure here stops the run.
        ASSERT_NO_FATAL_FAILURE(applyAidlModeBeforeInit());

        // Initialize the Bus so it's ready for tests
        // Fatal on failure: every driver-dependent case needs an initialized stack.
        ASSERT_NO_THROW({ LibCCEC::getInstance().init("CEC_TEST"); })
            << "the CEC library could not be initialized, so not one suite in this binary "
               "has its precondition; continuing would assert against an uninitialized stack";

        // init() has fixed the selection; a back-end the mode did not ask for stops the run.
        ASSERT_NO_FATAL_FAILURE(failUnlessSelectedBackEndMatchesMode(resolvedAidlMode()));
    }
    
    void TearDown() override {
        // Clean up
        // Non-fatal so cleanup completes; a published fake AIDL service cannot be withdrawn.
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

    // Appended after the default result printer, so a guard report appears beneath the case that
    // caused it. GoogleTest owns the listener and deletes it.
    ::testing::UnitTest::GetInstance()->listeners().Append(new LogicalAddressRegistryGuard());

    return RUN_ALL_TESTS();
}
