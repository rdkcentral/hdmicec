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

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cerrno>
#include <cstring>
#include <string>
#include <time.h>
#include "ccec/CCEC.hpp"
#include "ccec/Connection.hpp"
#include "ccec/FrameListener.hpp"
#include "ccec/LibCCEC.hpp"
#include "ccec/Exception.hpp"
#include "ccec/Operands.hpp"
#include "ccec/Driver.hpp"
#include "ccec/CECFrame.hpp"
/*
 * DriverImpl.hpp lives under ccec/src, which AM_CPPFLAGS does not cover, so it is reached by
 * relative path rather than by adding an -I - the same route ccec/test_DriverImpl_Async.cpp
 * already takes.  It is needed for exactly one thing here: the address of the driver's own HAL
 * receive callback, DriverImpl::DriverReceiveCallback, which awaitBusReaderRunning() re-states
 * on the process-global mock so its frame injection cannot be routed away by another fixture.
 */
#include "../../../ccec/src/DriverImpl.hpp"
#include "hdmi_cec_driver_mock.h"

// LibCCEC::init() is the only writer of the CEC log prefix, and the prefix is the only
// observable effect its name argument has - so the null-name branch under test is observable
// only here, and reading the prefix back is also how that branch is undone without a
// term()/init() cycle. The buffer is defined with external linkage in ccec/src/Util.cpp and
// LibCCEC.cpp declares it exactly this way, so this reads production state directly instead of
// inferring it.
CCEC_BEGIN_NAMESPACE
extern char _CEC_LOG_PREFIX[64];
CCEC_END_NAMESPACE

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

/*
 * NO SENTINEL-DRAINING HELPER IS DECLARED OR CALLED HERE, and none can be written.  A case that
 * terminates the shared library leaves the NULL sentinel DriverImpl::close() offers to the receive
 * queue sitting on that queue, and it is tempting to answer that with a helper that issues a
 * read() on the closed driver to take it off.  A read() cannot: DriverImpl::read()'s first
 * statement is
 * `{AutoLock lock_(mutex); if (status != OPENED) throw InvalidStateException(); }`
 * (DriverImpl.cpp:158-162), which runs BEFORE the poll loop, so a read() issued against an
 * already-closed driver throws immediately and never touches rQueue at all.  Any such helper is a
 * no-op describing itself as a mitigation, and must not be reintroduced here or in
 * ccec/test_Driver.cpp.
 *
 * BLOCKED - REQUIRED PRODUCTION CHANGE, REPORTED NOT MADE (Directive 6): DriverImpl::read() must
 * null-check inFrame in its flush arm, or CCEC_OSAL::EventQueue must publish its element count and
 * its condition variable under one lock.  Either alone removes the window; neither is a test-only
 * change, so neither is made here.
 */

namespace {

/*
 * ---------------------------------------------------------------------------------
 * MAKING term()-AFTER-init() SAFE, SO NO CASE IN THIS FILE DEPENDS ON ITS POSITION
 *
 * LibCCEC is a process singleton and term() is its only route to the uninitialised state, so
 * the "called before init()" cases have to terminate the shared library - and one case here
 * initialises it.  A term() issued shortly after an init() can be LOST: Bus::start() creates
 * the reader thread, Bus::Reader::run() marks itself RUNNING from inside that new thread, and
 * Bus::Reader::stop() promotes only a RUNNING reader to STOPPING.  A stop request that arrives
 * before the reader was scheduled is therefore dropped, the reader re-arms itself against a
 * closed driver, and the suite hangs or crashes.
 *
 * DECLARING THE INITIALISING CASE LAST WOULD ALSO AVOID IT, AND MUST NOT BE RELIED ON.  Nothing
 * would then term() after it - but that is a dependence on source order: moving the case, or
 * adding one after it, silently re-opens the window, and nothing in the code says so when it
 * breaks.
 *
 * The dependence is removed by waiting for the transition instead of hoping it happened.
 * awaitBusReaderRunning() injects one frame through the mock HAL and waits - bounded, on a
 * real observable, never on a fixed sleep - until a frame listener has been notified.  A
 * delivered frame means Bus::Reader::run() has executed the body that publishes RUNNING, so a
 * following stop() cannot be dropped.  Every case can then restore whatever state it changed,
 * in any order, which is what per-test isolation means here.
 *
 * IT RUNS ONLY WHEN THERE IS A READER TO WAIT FOR.  ensureTerminated() first asks a read-only
 * probe whether the library is initialised at all and skips the wait when it is not, so the
 * injection is paid once per case that actually has to stop a reader and never on a library that
 * is already down.  In this fixture's default order that is every case, because each TearDown()
 * hands the initialised library back to the rest of the binary - measured by running
 * `--gtest_filter=LibCCECUninitializedTest.*`, which shows one `Bus::Reader::stop` between
 * consecutive `[ RUN ]` lines.  KEEP THE INJECTION COUNT AS LOW AS THE DESIGN ALLOWS, because
 * frame dispatch is not free of risk in this suite: Connection::~Connection()
 * is EMPTY in production and never detaches its Bus listener, so any dispatch after a
 * Connection has been destroyed without an explicit close() walks a freed listener.  That is a
 * pre-existing production defect - it segfaults the suite under --gtest_shuffle in
 * ConnectionTest's own pre-existing injection cases, with or without this file's change - and
 * it is reported as BLOCKED rather than worked around.  Keeping the injection count at one, as
 * early as possible, is this fixture's part of not making it worse.
 *
 * BLOCKED, REPORTED NOT FIXED (Directive 6): the underlying window is a defect in
 * CCEC_OSAL::Stoppable and CCEC::Bus - Bus::Reader::run() must not promote a reader that has
 * already been asked to stop, which needs an explicit STARTING state or a start that publishes
 * RUNNING from the calling thread.  Both are production changes.  Until one lands, a test that
 * cycles the shared library has to prove the reader is up before it stops it.
 * ---------------------------------------------------------------------------------
 */
class CountingFrameListener : public FrameListener {
public:
    CountingFrameListener() : notified(0) {}

    void notify(const CECFrame &) const override { ++notified; }

    mutable int notified;
};

void sleepOneMillisecond() {
    struct timespec interval;
    interval.tv_sec = 0;
    interval.tv_nsec = 1000 * 1000L;
    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
        /* finish the remaining interval nanosleep wrote back */
    }
}

/*
 * Returns true once a frame injected through the mock HAL has been delivered to a listener,
 * which is the observable proof that the Bus reader thread is running.  Bounded at 2 s; the
 * delivery is normally immediate, so the bound only fires if the reader never started, which
 * is a failure worth reporting rather than waiting out.
 */
bool awaitBusReaderRunning() {
    HdmiCecDriverMock *mock = HdmiCecDriverMock::getInstance();
    if (mock == nullptr) {
        return false;
    }

    /*
     * THE INBOUND ROUTE IS RE-STATED, NOT ASSUMED - this is what makes the wait below
     * self-sufficient in any run order.
     *
     * The frame injected further down travels to the Bus reader only if the mock's stored
     * receive registration still points at the driver's own callback.  It is a member of the
     * PROCESS-GLOBAL mock, and the real HdmiCecSetRxCallback entry point overwrites it with
     * whatever it is handed: ccec/test_Driver_Mock.cpp's ReceiveMessageCallback case registers a
     * callback of its own, so once that case has run, an injection reaches ITS lambda - through a
     * data pointer that died with its stack frame - and never reaches the CEC stack at all.
     * DriverImpl::open() does not put the driver's registration back either, because it returns
     * early while the driver is already OPENED.
     *
     * Re-stating the driver's own callback here costs two assignments and displaces any
     * inherited registration, which is the same self-sufficiency rule DriverTest.WriteAsync
     * follows when it re-states the HdmiCecOpen default action before opening rather than
     * inheriting whichever one the previous fixture left (ccec/test_Driver.cpp).  The data
     * pointer is 0 because that is precisely what DriverImpl::open() registers alongside the
     * callback, so this restores the registration rather than inventing one.
     *
     * The sibling fixture now also restores what it borrowed, so in practice this is belt and
     * braces - and it stays, because it is what keeps THIS fixture independent of whether any
     * other fixture in the binary is well behaved.
     */
    mock->rxCallback = &DriverImpl::DriverReceiveCallback;
    mock->rxCallbackData = 0;

    CountingFrameListener listener;
    Connection probe(LogicalAddress::PLAYBACK_DEVICE_1, false, "LibCcecReaderProbe");
    probe.open();
    probe.addFrameListener(&listener);

    /* Directed to PLAYBACK_DEVICE_1 from the TV, carrying GIVE_DEVICE_POWER_STATUS: a frame
     * the probe connection accepts and nothing else in this translation unit is watching. */
    const unsigned char frame[] = { 0x04, 0x8F };

    bool delivered = false;
    for (int attempt = 0; attempt < 2000 && !delivered; ++attempt) {
        if ((attempt % 200) == 0) {
            /* Re-injected periodically rather than once: the reader consumes the receive
             * queue, so a frame queued before it was scheduled is still delivered, but a
             * frame lost to a closed-then-reopened driver would otherwise strand the wait. */
            mock->injectReceivedMessage(frame, static_cast<int>(sizeof(frame)));
        }
        if (listener.notified > 0) {
            delivered = true;
            break;
        }
        sleepOneMillisecond();
    }

    probe.removeFrameListener(&listener);
    probe.close();
    return delivered;
}

// Guarantees the shared library is left initialized under the prefix the global test
// environment established, whatever happens to the test body - including a fatal
// assertion or an exception escaping mid-cycle.
class ScopedLibCcecRestore {
public:
    ScopedLibCcecRestore() {}

    ScopedLibCcecRestore(const ScopedLibCcecRestore &) = delete;
    ScopedLibCcecRestore &operator=(const ScopedLibCcecRestore &) = delete;

    ~ScopedLibCcecRestore()
    {
        // On the success path the body has already restored the library, and this guard
        // must then cost NOTHING: every extra term()/init() pair is another chance to
        // lose the Bus reader race described in the test below, so the guard probes
        // instead of unconditionally cycling.
        //
        // init() is a side-effect-free probe for "is the library initialized": it checks
        // that flag before touching any other state and throws InvalidStateException when
        // it is set. If the flag is clear the probe performs exactly the restore that was
        // needed anyway.
        bool wasInitialized = false;
        try {
            LibCCEC::getInstance().init("CEC_TEST");
        } catch (const InvalidStateException &) {
            wasInitialized = true;
        }

        // The only case still needing a full cycle is a body that aborted while a
        // different prefix was installed - which can only follow a reported failure.
        if (wasInitialized && (std::string(_CEC_LOG_PREFIX) != std::string("CEC_TEST"))) {
            try {
                LibCCEC::getInstance().term();
            } catch (const InvalidStateException &) {
                // Raced to terminated; the following init() still restores the prefix.
            }
            try {
                LibCCEC::getInstance().init("CEC_TEST");
            } catch (const InvalidStateException &) {
                // Raced to initialized; the prefix is correct either way.
            }
        }
    }
};
} // namespace


class LibCCECTest : public ::testing::Test {
protected:
    void SetUp() override {
        // LibCCEC may already be initialized by other test suites (DriverTest, etc.)
        // Try to initialize, but ignore InvalidStateException if already initialized
        try {
            LibCCEC::getInstance().init("TestCEC");
        } catch (const InvalidStateException&) {
            // Already initialized - this is fine
        }
        // No case in THIS fixture terminates the library, so an init() performed here is never
        // followed by a term() that could race the Bus reader it just started. The cases that do
        // need the uninitialised state live in LibCCECUninitializedTest below, which brings the
        // library down only behind a wait for the reader to publish RUNNING - the ordering that
        // keeps the stop from being dropped.
    }

    void TearDown() override {
        // Don't call term() here to avoid thread cleanup issues
        // LibCCEC will be cleaned up by the global test environment
    }
};

TEST_F(LibCCECTest, GetInstanceReturnsSingleton) {
    LibCCEC& instance1 = LibCCEC::getInstance();
    LibCCEC& instance2 = LibCCEC::getInstance();
    EXPECT_EQ(&instance1, &instance2);
}

TEST_F(LibCCECTest, InitWithValidName) {
    // LibCCEC is already initialized in SetUp; verify getInstance() is reachable.
    EXPECT_NO_THROW({
        LibCCEC& instance = LibCCEC::getInstance();
        (void)instance;
    });
}

TEST_F(LibCCECTest, InitThrowsWhenAlreadyInitialized) {
    LibCCEC& lib = LibCCEC::getInstance();
    
    // Already initialized in SetUp
    // Try to initialize again - should throw InvalidStateException
    EXPECT_THROW(lib.init("TestCEC2"), InvalidStateException);
}

TEST_F(LibCCECTest, AddLogicalAddressAfterInit) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Verify HdmiCecAddLogicalAddress is called with PLAYBACK_DEVICE_1 (4)
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_1))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));

    LogicalAddress addr(LogicalAddress::PLAYBACK_DEVICE_1);
    int result = 0;
    EXPECT_NO_THROW(result = lib.addLogicalAddress(addr));
    EXPECT_TRUE(result);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}

TEST_F(LibCCECTest, GetLogicalAddressAfterInit) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Configure the mock to return a known logical address value (5 = AUDIO_SYSTEM)
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Invoke([](int, int* addr) -> int {
            if (addr) *addr = LogicalAddress::AUDIO_SYSTEM; // 5
            return HDMI_CEC_IO_SUCCESS;
        }));

    int logicalAddr = 0;
    EXPECT_NO_THROW(logicalAddr = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE));
    EXPECT_EQ(logicalAddr, LogicalAddress::AUDIO_SYSTEM);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}


TEST_F(LibCCECTest, GetPhysicalAddressAfterInit) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Configure the mock to return a specific, known physical address
    const unsigned int expectedPhysAddr = 0x2100;
    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _))
        .Times(1)
        .WillOnce(Invoke([expectedPhysAddr](int, unsigned int* addr) -> int {
            if (addr) *addr = expectedPhysAddr;
            return HDMI_CEC_IO_SUCCESS;
        }));

    unsigned int physAddr = 0;
    EXPECT_NO_THROW(lib.getPhysicalAddress(&physAddr));
    EXPECT_EQ(physAddr, expectedPhysAddr);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}


TEST_F(LibCCECTest, AddMultipleLogicalAddresses) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Each addLogicalAddress call must reach the driver with the correct address value
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_1))
        .Times(1).WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_2))
        .Times(1).WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::AUDIO_SYSTEM))
        .Times(1).WillOnce(Return(HDMI_CEC_IO_SUCCESS));

    LogicalAddress addr1(LogicalAddress::PLAYBACK_DEVICE_1);
    LogicalAddress addr2(LogicalAddress::PLAYBACK_DEVICE_2);
    LogicalAddress addr3(LogicalAddress::AUDIO_SYSTEM);

    EXPECT_TRUE(lib.addLogicalAddress(addr1));
    EXPECT_TRUE(lib.addLogicalAddress(addr2));
    EXPECT_TRUE(lib.addLogicalAddress(addr3));

    ::testing::Mock::VerifyAndClearExpectations(mock);
}

TEST_F(LibCCECTest, GetLogicalAddressForDifferentDeviceTypes) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // Note: DriverImpl::getLogicalAddress ignores devType and always calls HdmiCecGetLogicalAddress.
    // The three calls return distinct pre-configured values so we verify the pass-through.
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _))
        .Times(3)
        .WillOnce(Invoke([](int, int* a) -> int { if (a) *a = LogicalAddress::TV;             return HDMI_CEC_IO_SUCCESS; }))
        .WillOnce(Invoke([](int, int* a) -> int { if (a) *a = LogicalAddress::PLAYBACK_DEVICE_1; return HDMI_CEC_IO_SUCCESS; }))
        .WillOnce(Invoke([](int, int* a) -> int { if (a) *a = LogicalAddress::AUDIO_SYSTEM;   return HDMI_CEC_IO_SUCCESS; }));

    // getLogicalAddress throws InvalidStateException when the driver returns 0 (TV=0),
    // so we test the two non-zero values and handle TV separately.
    EXPECT_THROW(lib.getLogicalAddress(DeviceType::TV), InvalidStateException); // TV=0 triggers the guard

    int addr2 = 0;
    EXPECT_NO_THROW(addr2 = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE));
    EXPECT_EQ(addr2, LogicalAddress::PLAYBACK_DEVICE_1); // 4

    int addr3 = 0;
    EXPECT_NO_THROW(addr3 = lib.getLogicalAddress(DeviceType::AUDIO_SYSTEM));
    EXPECT_EQ(addr3, LogicalAddress::AUDIO_SYSTEM); // 5

    ::testing::Mock::VerifyAndClearExpectations(mock);
}

/*
 * Fixture for the "called before init()" guard family - LibCCEC::term,
 * LibCCEC::addLogicalAddress, LibCCEC::getLogicalAddress and LibCCEC::getPhysicalAddress each
 * throw InvalidStateException when the library has not been initialised, and those four throw
 * arms are the invalid-state family this suite is required to cover.  The null-name arm of
 * init() belongs here too, because it is only reachable from the uninitialised state.
 *
 * PER-TEST STATE ISOLATION, NOT DECLARATION ORDER.  LibCCEC is a process singleton, so every
 * case here changes state the whole binary shares.  SetUp() therefore establishes the
 * precondition each case needs - terminated - and TearDown() puts back the state the rest of
 * the binary expects - initialised under the prefix the global environment set.  Both are
 * idempotent, both work from whichever state the previous case left, and neither depends on
 * which case ran before or on where a case appears in this file.  Adding a case, reordering
 * two, or running one alone with --gtest_filter all behave identically.
 *
 * DO NOT REPLACE THIS WITH A ONE-TERM-PER-FIXTURE SCHEME.  Terminating once in the first SetUp()
 * and re-initialising only in TearDownTestSuite() looks cheaper, and it forces the one initialising
 * case to be declared LAST so that no SetUp() term()s straight after its init() - the same
 * declaration-order dependence this design exists to remove.  ensureTerminated() waits for the Bus
 * reader to be observably running before it stops it (see awaitBusReaderRunning above), which makes
 * a term() after an init() safe wherever it occurs and costs one frame injection per case.
 */
class LibCCECUninitializedTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(ensureTerminated())
            << "could not bring LibCCEC to the uninitialised state this case requires";
    }

    void TearDown() override {
        /* The rest of this binary runs against an initialised library under the CEC_TEST
         * prefix, so that is what every case here hands back - including the one that empties
         * the prefix on purpose. */
        EXPECT_TRUE(ensureInitialized())
            << "could not restore the initialised LibCCEC state the rest of the suite expects";
        strncpy(_CEC_LOG_PREFIX, "CEC_TEST", sizeof(_CEC_LOG_PREFIX) - 1);
        _CEC_LOG_PREFIX[sizeof(_CEC_LOG_PREFIX) - 1] = '\0';
    }

    /*
     * Bring the shared library to the UNINITIALISED state, from either state.
     *
     * The reader-readiness wait is what makes this safe to call immediately after another
     * case initialised the library: term() while the reader has not yet published RUNNING is
     * the dropped-stop window described above.  The wait is skipped when the library is
     * already terminated, because there is then no reader to wait for.
     */
    static bool ensureTerminated() {
        /* ALREADY TERMINATED: there is no reader to wait for and nothing to stop.  Checked with
         * a read-only probe - getPhysicalAddress() throws InvalidStateException when the
         * library is uninitialised and otherwise only reads - so this path costs no state
         * change at all.  It is what makes this helper safe to call from either state; in the
         * default order no case reaches it, because every TearDown() leaves the library
         * initialised for the rest of the binary. */
        if (!isInitialized()) {
            return true;
        }

        /* INITIALISED: the reader must be observably running before it is stopped, or the stop
         * can be dropped.  This is the ONE frame injection this fixture performs per run. */
        if (!awaitBusReaderRunning()) {
            return false;
        }

        try {
            LibCCEC::getInstance().term();
        } catch (const InvalidStateException &) {
            /* Raced to terminated by something else; that is the state wanted. */
        }
        return true;
    }

    /*
     * Read-only "is the library initialised?" probe.
     *
     * getPhysicalAddress() is one of the four guarded accessors this fixture tests: it throws
     * InvalidStateException before init() and otherwise reads through to the HAL, changing no
     * library state either way.  Using init() as the probe instead - which also answers the
     * question, by throwing when it is already up - would INITIALISE the library on the path
     * where it was not, and that is the state change this fixture exists to avoid making
     * incidentally.
     */
    static bool isInitialized() {
        unsigned int physicalAddress = 0;
        try {
            LibCCEC::getInstance().getPhysicalAddress(&physicalAddress);
            return true;
        } catch (const InvalidStateException &) {
            return false;
        }
    }

    /* Bring the shared library to the INITIALISED state, from either state. */
    static bool ensureInitialized() {
        try {
            LibCCEC::getInstance().init("CEC_TEST");
        } catch (const InvalidStateException &) {
            /* Already initialised, which is the state wanted. */
        }
        return true;
    }
};

// #gap-mw-libccec; LibCCEC::term uninitialised guard.
TEST_F(LibCCECUninitializedTest, TermThrowsWhenNotInitialized) {
    LibCCEC& lib = LibCCEC::getInstance();

    EXPECT_THROW(lib.term(), InvalidStateException);
}

// #gap-mw-libccec; LibCCEC::addLogicalAddress uninitialised guard. The driver must not be
// reached at all, which is what the zero-cardinality expectation pins.
TEST_F(LibCCECUninitializedTest, AddLogicalAddressThrowsWhenNotInitialized) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, _)).Times(0);
    LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    EXPECT_THROW(lib.addLogicalAddress(address), InvalidStateException);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// #gap-mw-libccec; LibCCEC::getLogicalAddress uninitialised guard. This reaches the
// !initialized guard, not the zero-logical-address guard that follows the driver call and is
// already covered by GetLogicalAddressForDifferentDeviceTypes.
TEST_F(LibCCECUninitializedTest, GetLogicalAddressThrowsWhenNotInitialized) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _)).Times(0);
    EXPECT_THROW(lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE), InvalidStateException);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// #gap-mw-libccec; LibCCEC::getPhysicalAddress uninitialised guard.
TEST_F(LibCCECUninitializedTest, GetPhysicalAddressThrowsWhenNotInitialized) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _)).Times(0);
    unsigned int physicalAddress = 0;
    EXPECT_THROW(lib.getPhysicalAddress(&physicalAddress), InvalidStateException);
    ::testing::Mock::VerifyAndClearExpectations(mock);
}

// #gap-mw-libccec; §6.2 rank 23, P1; LibCCEC::init null-name arm.
//
// init(NULL) takes the else arm of the name guard, whose entire observable effect is that it
// empties the shared log prefix. Asserting only that the call does not throw would pass even if
// that arm stopped clearing the prefix, so the prefix is seeded to a known non-empty value
// first and read back afterwards.
//
// POSITION-INDEPENDENT: this case leaves the library INITIALISED, and the fixture's TearDown()
// is what hands that state on regardless of what runs next.  A following SetUp() terminates it
// safely because ensureTerminated() waits for the Bus reader to be observably running first, so
// this case no longer has to be - and is no longer - the last one declared here.
TEST_F(LibCCECUninitializedTest, InitWithNullNameClearsLogPrefix) {
    LibCCEC& lib = LibCCEC::getInstance();
    ScopedLibCcecRestore restoreLibCcec;

    // Seeded before the call under test: if init(NULL) stopped clearing the prefix, this value
    // would still be there afterwards.
    strncpy(_CEC_LOG_PREFIX, "CEC_SEED", sizeof(_CEC_LOG_PREFIX) - 1);
    _CEC_LOG_PREFIX[sizeof(_CEC_LOG_PREFIX) - 1] = '\0';
    const std::string seededPrefix(_CEC_LOG_PREFIX);
    ASSERT_FALSE(seededPrefix.empty());

    // The null-name arm must empty the prefix rather than leave the seed in place.
    EXPECT_NO_THROW({ lib.init(NULL); });

    EXPECT_STREQ("", _CEC_LOG_PREFIX);
    EXPECT_NE(seededPrefix, std::string(_CEC_LOG_PREFIX));

    // The library is otherwise fully up, which the rejected second init confirms.
    EXPECT_THROW(lib.init(NULL), InvalidStateException);

    // The prefix is restored in place rather than through a term()/init() pair.  The library is
    // left initialised, which is what the rest of the binary expects; TearDown() re-establishes
    // it either way, so nothing downstream depends on this case having done so.
    strncpy(_CEC_LOG_PREFIX, "CEC_TEST", sizeof(_CEC_LOG_PREFIX) - 1);
    _CEC_LOG_PREFIX[sizeof(_CEC_LOG_PREFIX) - 1] = '\0';
    EXPECT_STREQ("CEC_TEST", _CEC_LOG_PREFIX);
    EXPECT_EQ('\0', _CEC_LOG_PREFIX[sizeof(_CEC_LOG_PREFIX) - 1]);
}


// #gap-mw-libccec; repeated init/term cycling with the three guarded accessors read back after
// the second init.  Placed on LibCCECUninitializedTest rather than LibCCECTest because the case
// has to own the library's lifecycle: SetUp() hands it over terminated and TearDown() puts it
// back initialised under the CEC_TEST prefix, so the cycling below cannot leak into a sibling.
TEST_F(LibCCECUninitializedTest, MultipleInitTermCycles) {
    HdmiCecDriverMock* mock = HdmiCecDriverMock::getInstance();
    LibCCEC& lib = LibCCEC::getInstance();

    // CYCLE 1.  The reader must be observably running before term(), or the stop can be dropped -
    // the same window ensureTerminated() guards.
    EXPECT_NO_THROW({ lib.init("CEC_TEST"); });
    ASSERT_TRUE(awaitBusReaderRunning());
    EXPECT_NO_THROW({ lib.term(); });

    // CYCLE 2.  A second init after a completed term must bring the library fully back up, which
    // is what the three accessor reads below prove rather than assume.
    EXPECT_NO_THROW({ lib.init("CEC_TEST"); });
    ASSERT_TRUE(awaitBusReaderRunning());

    const unsigned int expectedPhysicalAddress = 0x2100;
    EXPECT_CALL(*mock, HdmiCecAddLogicalAddress(_, LogicalAddress::PLAYBACK_DEVICE_1))
        .Times(1)
        .WillOnce(Return(HDMI_CEC_IO_SUCCESS));
    EXPECT_CALL(*mock, HdmiCecGetLogicalAddress(_, _))
        .Times(1)
        .WillOnce(Invoke([](int, int* address) -> int {
            if (address) *address = LogicalAddress::AUDIO_SYSTEM;
            return HDMI_CEC_IO_SUCCESS;
        }));
    EXPECT_CALL(*mock, HdmiCecGetPhysicalAddress(_, _))
        .Times(1)
        .WillOnce(Invoke([expectedPhysicalAddress](int, unsigned int* address) -> int {
            if (address) *address = expectedPhysicalAddress;
            return HDMI_CEC_IO_SUCCESS;
        }));

    LogicalAddress address(LogicalAddress::PLAYBACK_DEVICE_1);
    int addResult = 0;
    EXPECT_NO_THROW(addResult = lib.addLogicalAddress(address));
    EXPECT_TRUE(addResult);

    int logicalAddress = 0;
    EXPECT_NO_THROW(logicalAddress = lib.getLogicalAddress(DeviceType::PLAYBACK_DEVICE));
    EXPECT_EQ(logicalAddress, LogicalAddress::AUDIO_SYSTEM);

    unsigned int physicalAddress = 0;
    EXPECT_NO_THROW(lib.getPhysicalAddress(&physicalAddress));
    EXPECT_EQ(physicalAddress, expectedPhysicalAddress);

    ::testing::Mock::VerifyAndClearExpectations(mock);
}
