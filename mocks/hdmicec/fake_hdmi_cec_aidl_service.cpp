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

#include "fake_hdmi_cec_aidl_service.h"
#include <binder/IServiceManager.h>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <utils/Errors.h>
#include <utils/String16.h>

/**
 * @defgroup HDMI_CEC_FAKE_AIDL_SERVICE_IMPL HDMI CEC Fake AIDL Service Implementation
 * @ingroup HDMI_CEC_FAKE_AIDL_SERVICE
 * @{
 * @par Fake Service Implementation Specification
 * Every interface method below follows one shape and nothing more: take the lock, count the call,
 * capture whatever the caller handed over that a test needs to read back, trace the call, then answer
 * - with the canned response the test installed where one exists, and with a fixed value where the
 * declaration records that there is deliberately no control.  Every setter takes the same lock for
 * one assignment, and every observation accessor takes it to return a copy rather than a reference,
 * so a test may install a canned response or read a capture while a binder thread is answering, and
 * what it reads is a stable snapshot.  The two static instance functions sit outside that shape and
 * take no lock, for the reason recorded on FakeHdmiCecService::getInstance().@n
 * The contract of each member - parameters, return value, preconditions, postconditions and the
 * reason the member exists at all - is stated once, on its declaration in
 * fake_hdmi_cec_aidl_service.h, and is copied here with @c \@copydoc rather than restated.  What
 * follows a @c \@copydoc line is only what is true of this body and not of the contract.@n
 * There is deliberately no CEC reasoning anywhere in this file.  It does not read a frame's
 * destination nibble, does not classify a message as directed or broadcast, does not inspect an
 * opcode, does not police a frame length and does not derive a send status from message content.
 * All of that belongs to the middleware adapter under test; a second copy of it here would make the
 * suite assert this fake's opinion instead of the adapter's behaviour.  The fake's whole purpose is
 * to make each adapter branch reachable.
 *
 * There is likewise no GoogleMock and no GoogleTest here, even though the legacy driver double in
 * this same directory is built on both.  This translation unit is also compiled into the separate
 * fake-service host binary, which links the AIDL stub and binder libraries alone, so a single
 * reference to either framework would break that link.
 *
 * The binder threadpool is deliberately absent too.  The service-side threadpool belongs to the host
 * binary and the client-side threadpool belongs to the middleware adapter; starting one here would
 * blur the in-process and out-of-process cases, and telling those two apart is the entire reason both
 * exist.
 *
 */

/**
 * @file fake_hdmi_cec_aidl_service.cpp
 *
 * @brief Implementation of the test-scope fake com.rdk.hal.hdmicec AIDL HdmiCec service.
 *
 * Defines FakeHdmiCecController, FakeHdmiCecService and the registration entry point declared in
 * fake_hdmi_cec_aidl_service.h.  Both classes derive from the generated server bases, so the
 * interface they present is exactly the frozen 0.1.0.0 snapshot's: no .aidl is authored here, no
 * interface is added and no method is added to an interface.
 *
 * @warning Test scope only.  This file is built for test targets exclusively - the fake-service host
 *          binary and the test runners - and must never appear in a production source list, so no
 *          symbol defined here can reach the shipped middleware library.
 *
 * @see fake_hdmi_cec_aidl_service.h
 */

/**
 * @brief Binder driver node consulted before the service manager is reached.
 *
 * Registration checks this node before it touches libbinder, because the linked libbinder aborts the
 * whole process when it cannot open its driver: on a host without kernel binder support the check is
 * what turns "the process died during test set-up" into "registration reported false", which is the
 * behaviour registerFakeHdmiCecService() documents.  It is the default node name the SDK's own
 * process state uses, and it is the node a test runner and its service manager share.
 */
static const char FAKE_HDMI_CEC_BINDER_DRIVER[] = "/dev/binder";

/**
 * @brief The word a diagnostic reports in place of an object it was not given.
 *
 * Spelled once so that absence reads the same way at every trace site, and so that no site can drift
 * into printing an empty field - where a reader could not tell a missing object from a missing value.
 */
static const char FAKE_HDMI_CEC_TRACE_ABSENT[] = "absent";

// Static instance pointer
FakeHdmiCecService* FakeHdmiCecService::instance = nullptr;

/**
 * @copydoc fakeHdmiCecTraceLabel
 *
 * A registry and the sequence it draws from, both function-local statics behind their own short
 * critical section.  Function-local rather than file-scope so that construction is ordered by first
 * use rather than by link order: this translation unit is compiled into two binaries - the L1 test
 * runner and the separate fake-service host - and the first caller differs in each.@n
 * The lock is this registry's own and is taken nowhere else in the file, so a call made from inside
 * one of the fake's critical sections - which is where most of the trace sites sit - cannot deadlock
 * against it, and a call arriving on a binder thread cannot race one made from a test thread.@n
 * Nothing is ever pruned.  Reusing an ordinal would let two different objects appear under one label
 * in a single capture, which is the exact confusion the ordinal exists to remove, and the registry is
 * bounded by the handful of objects one run traces.
 */
::std::string fakeHdmiCecTraceLabel(const void* object)
{
    if (object == nullptr) {
        return ::std::string(FAKE_HDMI_CEC_TRACE_ABSENT);
    }

    static ::std::mutex ordinalMutex;
    static ::std::map<const void*, int32_t> ordinals;
    static int32_t lastOrdinal = 0;

    ::std::lock_guard<::std::mutex> guard(ordinalMutex);

    const ::std::pair<::std::map<const void*, int32_t>::iterator, bool> minted =
        ordinals.emplace(object, lastOrdinal + 1);

    if (minted.second) {
        lastOrdinal = minted.first->second;
    }

    return "#" + ::std::to_string(minted.first->second);
}

/**
 * @copydoc FakeHdmiCecController::addLogicalAddresses
 *
 * The counter and the capture advance before the status is examined, which is what makes both report
 * the call on the failing arms too.  The vector is stored exactly as it arrived - neither normalised,
 * sorted, deduplicated nor trimmed - because its width is what the single-element assertions read.  A
 * null out-parameter is traced rather than dereferenced.
 */
::android::binder::Status FakeHdmiCecController::addLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                                    bool* _aidl_return)
{
    /*
     * The optional delay, taken FIRST and with no lock held.  It is read under the instance mutex
     * and the mutex is dropped before the sleep, so a concurrent capture read on a remote fake
     * answering on binder threads is never blocked behind it.  Zero is the default and costs one
     * lock acquisition and a comparison, which is why this is unconditional rather than compiled
     * out.  See setAddLogicalAddressesDelayMs() for why a real sleep is the only way to reach the
     * middleware's slow-call threshold.
     */
    int32_t delayMs = 0;
    {
        ::std::lock_guard<::std::mutex> delayGuard(mutex);

        delayMs = addLogicalAddressesDelayMs;
    }

    if (delayMs > 0) {
        std::cout << "[FakeHdmiCecController::addLogicalAddresses] Delaying " << delayMs
                  << " ms before answering, as configured" << std::endl;

        ::std::this_thread::sleep_for(::std::chrono::milliseconds(delayMs));
    }

    ::std::lock_guard<::std::mutex> guard(mutex);

    ++addLogicalAddressesCallCount;
    lastAddedLogicalAddresses = logicalAddresses;

    std::cout << "[FakeHdmiCecController::addLogicalAddresses] Received " << logicalAddresses.size()
              << " address(es), reporting " << (addLogicalAddressesResult ? "true" : "false")
              << ", status: " << addLogicalAddressesBinderStatus.toString8().c_str() << std::endl;

    if (!addLogicalAddressesBinderStatus.isOk()) {
        return addLogicalAddressesBinderStatus;
    }

    if (_aidl_return) {
        *_aidl_return = addLogicalAddressesResult;
    } else {
        std::cout << "[FakeHdmiCecController::addLogicalAddresses] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecController::removeLogicalAddresses
 *
 * Identical in shape to addLogicalAddresses(), and deliberately so: a false result and a non-ok
 * status are ordinary outcomes on the removal path rather than errors, so neither is treated any
 * differently in this body than the successful case is.
 */
::android::binder::Status FakeHdmiCecController::removeLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                                       bool* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++removeLogicalAddressesCallCount;
    lastRemovedLogicalAddresses = logicalAddresses;

    std::cout << "[FakeHdmiCecController::removeLogicalAddresses] Received " << logicalAddresses.size()
              << " address(es), reporting " << (removeLogicalAddressesResult ? "true" : "false")
              << ", status: " << removeLogicalAddressesBinderStatus.toString8().c_str() << std::endl;

    if (!removeLogicalAddressesBinderStatus.isOk()) {
        return removeLogicalAddressesBinderStatus;
    }

    if (_aidl_return) {
        *_aidl_return = removeLogicalAddressesResult;
    } else {
        std::cout << "[FakeHdmiCecController::removeLogicalAddresses] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecController::sendMessage
 *
 * The frame is captured whole and never examined.  Nothing in this body reads a destination nibble,
 * inspects an opcode or applies a length limit, so nothing here can classify a frame as directed or
 * broadcast, and the meaning of ACK_STATE_0 and ACK_STATE_1 - inverted between those two cases -
 * stays a property of the adapter under test.
 */
::android::binder::Status FakeHdmiCecController::sendMessage(const ::std::vector<uint8_t>& message,
                                                            ::com::rdk::hal::hdmicec::SendMessageStatus* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++sendMessageCallCount;
    lastSentMessage = message;

    std::cout << "[FakeHdmiCecController::sendMessage] Received " << message.size()
              << " byte(s), reporting " << ::com::rdk::hal::hdmicec::toString(sendMessageResult)
              << ", status: " << sendMessageBinderStatus.toString8().c_str() << std::endl;

    if (!sendMessageBinderStatus.isOk()) {
        return sendMessageBinderStatus;
    }

    if (_aidl_return) {
        *_aidl_return = sendMessageResult;
    } else {
        std::cout << "[FakeHdmiCecController::sendMessage] Null out-parameter, status not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecController::getInterfaceVersion
 *
 * The divergence trace fires only when the reported version differs from the compiled-in one, so an
 * ordinary run prints nothing here and the line that does appear names the moment this controller was
 * made to report something the snapshot would not - rather than leaving a silent metadata change to be
 * inferred from whatever failed afterwards.  setInterfaceVersion() is what installs such a value, and
 * the cases in DriverAidlCompatibilityTest that drive it are what make this branch a reached one.
 */
int32_t FakeHdmiCecController::getInterfaceVersion()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    if (interfaceVersionResult != ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION) {
        std::cout << "[FakeHdmiCecController::getInterfaceVersion] Reporting overridden version: "
                  << interfaceVersionResult << std::endl;
    }

    return interfaceVersionResult;
}

/**
 * @copydoc FakeHdmiCecController::getInterfaceHash
 *
 * Carries the same divergence trace as getInterfaceVersion(), on the same terms and driven by
 * setInterfaceHash().
 */
std::string FakeHdmiCecController::getInterfaceHash()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    if (interfaceHashResult != ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE) {
        std::cout << "[FakeHdmiCecController::getInterfaceHash] Reporting overridden hash: \""
                  << interfaceHashResult << "\"" << std::endl;
    }

    return interfaceHashResult;
}

/**
 * @copydoc FakeHdmiCecController::setAddLogicalAddressesResult
 */
void FakeHdmiCecController::setAddLogicalAddressesResult(bool result)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesResult = result;
}

/**
 * @copydoc FakeHdmiCecController::setRemoveLogicalAddressesResult
 *
 * Held in a member of its own, so installing it disturbs no other canned response.
 */
void FakeHdmiCecController::setRemoveLogicalAddressesResult(bool result)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    removeLogicalAddressesResult = result;
}

/**
 * @copydoc FakeHdmiCecController::setAddLogicalAddressesDelayMs
 *
 * Stores the value only; the sleep itself is taken inside addLogicalAddresses(), with the lock
 * dropped, for the reason recorded on the declaration.  A negative value is stored as given and
 * treated as "no delay" by the comparison at the point of use, which keeps this setter free of a
 * clamp a caller would then have to reason about.
 */
void FakeHdmiCecController::setAddLogicalAddressesDelayMs(int32_t delayMs)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesDelayMs = delayMs;
}

/**
 * @copydoc FakeHdmiCecController::setSendMessageResult
 *
 * The value is stored without interpretation, so no reading of ACK_STATE_0 or ACK_STATE_1 is fixed
 * here.
 */
void FakeHdmiCecController::setSendMessageResult(::com::rdk::hal::hdmicec::SendMessageStatus status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    sendMessageResult = status;
}

/**
 * @copydoc FakeHdmiCecController::setAddLogicalAddressesBinderStatus
 *
 * Each of the three controller methods holds its own canned status, so failing one leaves the other
 * two alone.
 */
void FakeHdmiCecController::setAddLogicalAddressesBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesBinderStatus = status;
}

/**
 * @copydoc FakeHdmiCecController::setRemoveLogicalAddressesBinderStatus
 */
void FakeHdmiCecController::setRemoveLogicalAddressesBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    removeLogicalAddressesBinderStatus = status;
}

/**
 * @copydoc FakeHdmiCecController::setSendMessageBinderStatus
 *
 * Held independently of the canned send status, so the two can be installed in any combination.
 */
void FakeHdmiCecController::setSendMessageBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    sendMessageBinderStatus = status;
}

/**
 * @copydoc FakeHdmiCecController::setInterfaceHash
 *
 * Traces the value it replaces alongside the value it installs, in the same shape as the service's
 * setter, so one line records where in a run this controller was made to report a divergent hash.  No
 * validation is applied: the string is stored as given, because the caller owns which value it wants
 * reported.
 */
void FakeHdmiCecController::setInterfaceHash(std::string hash)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecController::setInterfaceHash] Hash set from \"" << interfaceHashResult
              << "\" to \"" << hash << "\"" << std::endl;
    interfaceHashResult = ::std::move(hash);
}

/**
 * @copydoc FakeHdmiCecController::setInterfaceVersion
 *
 * Traces the replaced and the installed version for the same reason the hash setter traces its pair,
 * and stores the value without validation: any int32_t is a value the caller may want this controller
 * to report.
 */
void FakeHdmiCecController::setInterfaceVersion(int32_t version)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecController::setInterfaceVersion] Version set from "
              << interfaceVersionResult << " to " << version << std::endl;
    interfaceVersionResult = version;
}

/**
 * @copydoc FakeHdmiCecController::getLastAddedLogicalAddresses
 */
::std::vector<int32_t> FakeHdmiCecController::getLastAddedLogicalAddresses() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastAddedLogicalAddresses;
}

/**
 * @copydoc FakeHdmiCecController::getLastRemovedLogicalAddresses
 */
::std::vector<int32_t> FakeHdmiCecController::getLastRemovedLogicalAddresses() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastRemovedLogicalAddresses;
}

/**
 * @copydoc FakeHdmiCecController::getLastSentMessage
 */
::std::vector<uint8_t> FakeHdmiCecController::getLastSentMessage() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastSentMessage;
}

/**
 * @copydoc FakeHdmiCecController::getAddLogicalAddressesCallCount
 */
int32_t FakeHdmiCecController::getAddLogicalAddressesCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return addLogicalAddressesCallCount;
}

/**
 * @copydoc FakeHdmiCecController::getRemoveLogicalAddressesCallCount
 */
int32_t FakeHdmiCecController::getRemoveLogicalAddressesCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return removeLogicalAddressesCallCount;
}

/**
 * @copydoc FakeHdmiCecController::getSendMessageCallCount
 */
int32_t FakeHdmiCecController::getSendMessageCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return sendMessageCallCount;
}

/**
 * @copydoc FakeHdmiCecController::reset
 *
 * One critical section restores all three canned results, all three canned statuses and both metadata
 * values, and clears all three captures and all three counters, so no case can observe a
 * half-restored controller.  Each default is spelled beside the line that restores it, which is what
 * keeps a documented default and the code that reinstates it together.  Restoring the metadata pair is
 * what stops a case that installed a divergent hash or version deciding the outcome of the next one.
 */
void FakeHdmiCecController::reset()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesResult = true;                                                       // Default: address acquired
    removeLogicalAddressesResult = true;                                                    // Default: address removed
    addLogicalAddressesDelayMs = 0;                                                         // Default: answer immediately
    sendMessageResult = ::com::rdk::hal::hdmicec::SendMessageStatus::ACK_STATE_0;            // Default: ACKed directed frame

    addLogicalAddressesBinderStatus = ::android::binder::Status::ok();
    removeLogicalAddressesBinderStatus = ::android::binder::Status::ok();
    sendMessageBinderStatus = ::android::binder::Status::ok();

    lastAddedLogicalAddresses.clear();
    lastRemovedLogicalAddresses.clear();
    lastSentMessage.clear();

    addLogicalAddressesCallCount = 0;
    removeLogicalAddressesCallCount = 0;
    sendMessageCallCount = 0;

    interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION;          // Default: the frozen version
    interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE;           // Default: the frozen hash

    std::cout << "[FakeHdmiCecController::reset] Canned responses, captures and counters restored to defaults"
              << std::endl;
}


/**
 * @copydoc FakeHdmiCecService::~FakeHdmiCecService
 *
 * The instance pointer is compared before it is cleared, so a fake destroyed after a second one was
 * published leaves that second one reachable.
 */
FakeHdmiCecService::~FakeHdmiCecService()
{
    if (instance == this) {
        instance = nullptr;
    }
}

/**
 * @copydoc FakeHdmiCecService::getState
 *
 * DEFAULT_STATE is written straight out; there is no member behind it, so nothing can make this
 * method report anything else, and no canned status exists to make it fail.
 */
::android::binder::Status FakeHdmiCecService::getState(::com::rdk::hal::hdmicec::State* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++getStateCallCount;

    std::cout << "[FakeHdmiCecService::getState] Reporting "
              << ::com::rdk::hal::hdmicec::toString(DEFAULT_STATE) << ", status: ok" << std::endl;

    if (_aidl_return) {
        *_aidl_return = DEFAULT_STATE;
    } else {
        std::cout << "[FakeHdmiCecService::getState] Null out-parameter, state not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::getProperty
 *
 * ::std::nullopt is written straight out.  No PropertyValue is constructed anywhere in this body, so
 * there is no fabricated metric here for a test to assert against itself.
 */
::android::binder::Status FakeHdmiCecService::getProperty(::com::rdk::hal::hdmicec::Property property,
                                                          ::std::optional<::com::rdk::hal::PropertyValue>* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++getPropertyCallCount;

    std::cout << "[FakeHdmiCecService::getProperty] Property " << ::com::rdk::hal::hdmicec::toString(property)
              << " requested, reporting an empty optional, status: ok" << std::endl;

    if (_aidl_return) {
        *_aidl_return = ::std::nullopt;                                                      // Default: property not available
    } else {
        std::cout << "[FakeHdmiCecService::getProperty] Null out-parameter, optional not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::getLogicalAddresses
 *
 * The canned vector is copied out whatever its width.  Nothing here normalises, sorts, deduplicates
 * or truncates it, because each width - empty, one entry, more than one - reaches a different arm of
 * the adapter under test.
 */
::android::binder::Status FakeHdmiCecService::getLogicalAddresses(::std::vector<int32_t>* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++getLogicalAddressesCallCount;

    std::cout << "[FakeHdmiCecService::getLogicalAddresses] Reporting " << logicalAddressesResult.size()
              << " address(es), status: " << getLogicalAddressesBinderStatus.toString8().c_str() << std::endl;

    if (!getLogicalAddressesBinderStatus.isOk()) {
        return getLogicalAddressesBinderStatus;
    }

    if (_aidl_return) {
        *_aidl_return = logicalAddressesResult;
    } else {
        std::cout << "[FakeHdmiCecService::getLogicalAddresses] Null out-parameter, addresses not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::open
 *
 * The listener is captured before the canned status is examined, which is what leaves the receive path
 * exercisable against a session the adapter rejected.  The null-controller flag is read only on the ok
 * arm, so an ok status carrying nothing usable is the one combination a test has to ask for
 * explicitly.
 */
::android::binder::Status FakeHdmiCecService::open(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecControllerListener,
                                                  ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController>* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++openCallCount;
    listener = cecControllerListener;

    std::cout << "[FakeHdmiCecService::open] Listener captured: "
              << fakeHdmiCecTraceLabel(cecControllerListener.get())
              << " on open call " << openCallCount
              << ", reporting controller: "
              << (openReturnsNullController ? "nullptr (requested)" : "the owned controller")
              << ", status: " << openBinderStatus.toString8().c_str() << std::endl;

    if (!openBinderStatus.isOk()) {
        return openBinderStatus;
    }

    if (_aidl_return) {
        if (openReturnsNullController) {
            *_aidl_return = nullptr;
        } else {
            *_aidl_return = controller;
        }
    } else {
        std::cout << "[FakeHdmiCecService::open] Null out-parameter, controller not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::close
 *
 * There is no statement in this body that touches the captured listener, and that omission is
 * deliberate: a trigger fired after a close still has to reach the adapter's listener, because "a
 * callback arriving during or after a close is rejected by the adapter's own state guard" is a
 * required behaviour and clearing the listener here would make it untestable.
 */
::android::binder::Status FakeHdmiCecService::close(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController>& hdmiCecController,
                                                   bool* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++closeCallCount;
    lastClosedController = hdmiCecController;

    std::cout << "[FakeHdmiCecService::close] Controller captured: "
              << fakeHdmiCecTraceLabel(hdmiCecController.get())
              << " on close call " << closeCallCount
              << ", reporting " << (closeResult ? "true" : "false")
              << ", status: " << closeBinderStatus.toString8().c_str() << std::endl;

    if (!closeBinderStatus.isOk()) {
        return closeBinderStatus;
    }

    if (_aidl_return) {
        *_aidl_return = closeResult;
    } else {
        std::cout << "[FakeHdmiCecService::close] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::registerEventListener
 *
 * The offered listener is stored nowhere in this body, so no second delivery route exists for a test
 * to reach by accident.
 */
::android::binder::Status FakeHdmiCecService::registerEventListener(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecEventListener,
                                                                   bool* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++registerEventListenerCallCount;

    std::cout << "[FakeHdmiCecService::registerEventListener] Listener offered: "
              << fakeHdmiCecTraceLabel(cecEventListener.get())
              << " on registerEventListener call " << registerEventListenerCallCount
              << ", not retained, reporting true, status: ok" << std::endl;

    if (_aidl_return) {
        *_aidl_return = true;                                                                // Default: registration accepted
    } else {
        std::cout << "[FakeHdmiCecService::registerEventListener] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::unregisterEventListener
 *
 * Nothing is withdrawn here, because registerEventListener() retains nothing to withdraw.
 */
::android::binder::Status FakeHdmiCecService::unregisterEventListener(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecEventListener,
                                                                     bool* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++unregisterEventListenerCallCount;

    std::cout << "[FakeHdmiCecService::unregisterEventListener] Listener withdrawn: "
              << fakeHdmiCecTraceLabel(cecEventListener.get())
              << " on unregisterEventListener call " << unregisterEventListenerCallCount
              << ", reporting true, status: ok" << std::endl;

    if (_aidl_return) {
        *_aidl_return = true;                                                                // Default: withdrawal accepted
    } else {
        std::cout << "[FakeHdmiCecService::unregisterEventListener] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @copydoc FakeHdmiCecService::getInterfaceVersion
 *
 * Carries the same divergence trace as the controller's version getter, driven by
 * setInterfaceVersion(): it fires only when the reported version differs from the compiled-in one, so
 * an ordinary run prints nothing here and the line that does appear names the change at the moment it
 * would matter.
 */
int32_t FakeHdmiCecService::getInterfaceVersion()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    if (interfaceVersionResult != ::com::rdk::hal::hdmicec::IHdmiCec::VERSION) {
        std::cout << "[FakeHdmiCecService::getInterfaceVersion] Reporting overridden version: "
                  << interfaceVersionResult << std::endl;
    }

    return interfaceVersionResult;
}

/**
 * @copydoc FakeHdmiCecService::getInterfaceHash
 *
 * The trace fires only when the reported hash differs from the compiled-in one, so an ordinary
 * compatible run prints nothing here and the line that does appear marks the deliberately
 * incompatible run.
 */
std::string FakeHdmiCecService::getInterfaceHash()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    if (interfaceHashResult != ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE) {
        std::cout << "[FakeHdmiCecService::getInterfaceHash] Reporting overridden hash: \""
                  << interfaceHashResult << "\"" << std::endl;
    }

    return interfaceHashResult;
}


/**
 * @copydoc FakeHdmiCecService::setLogicalAddressesResult
 *
 * The vector is stored whole and unexamined: nothing here rejects an empty one, caps its width or
 * validates an entry, because every one of those shapes is a case a test needs to be able to install.
 */
void FakeHdmiCecService::setLogicalAddressesResult(const ::std::vector<int32_t>& logicalAddresses)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    logicalAddressesResult = logicalAddresses;
}

/**
 * @copydoc FakeHdmiCecService::setCloseResult
 *
 * Held separately from the canned close status, so a test can drive a transport failure and a
 * HAL-reported refusal independently of one another.
 */
void FakeHdmiCecService::setCloseResult(bool result)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    closeResult = result;
}

/**
 * @copydoc FakeHdmiCecService::setOpenReturnsNullController
 *
 * Only a flag is stored; the owned controller is neither released nor replaced, so clearing the flag
 * restores the ordinary successful open with the same controller object as before.
 */
void FakeHdmiCecService::setOpenReturnsNullController(bool returnsNull)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    openReturnsNullController = returnsNull;
}

/**
 * @copydoc FakeHdmiCecService::setOpenBinderStatus
 *
 * The status is stored without interpretation, so any exception code the caller builds - including the
 * EX_ILLEGAL_STATE the interface documents for an already-open service - arrives at the adapter exactly
 * as it was constructed.
 */
void FakeHdmiCecService::setOpenBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    openBinderStatus = status;
}

/**
 * @copydoc FakeHdmiCecService::setCloseBinderStatus
 *
 * Held in a member of its own, independent of the canned close result, for the reason
 * setCloseResult() records.
 */
void FakeHdmiCecService::setCloseBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    closeBinderStatus = status;
}

/**
 * @copydoc FakeHdmiCecService::setGetLogicalAddressesBinderStatus
 *
 * Independent of the canned address vector, so the failed query and the successful but empty query can
 * be installed separately even though the adapter reports the same outcome for both.
 */
void FakeHdmiCecService::setGetLogicalAddressesBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    getLogicalAddressesBinderStatus = status;
}

/**
 * @copydoc FakeHdmiCecService::setInterfaceHash
 *
 * Traces the value it replaces alongside the value it installs, so the one line a run prints here is
 * the record of where in that run the fake was made deliberately incompatible.  No validation is
 * applied: the string is stored as given, because the harness owns which value produces the refusal.
 */
void FakeHdmiCecService::setInterfaceHash(std::string hash)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecService::setInterfaceHash] Hash set from \"" << interfaceHashResult
              << "\" to \"" << hash << "\"" << std::endl;
    interfaceHashResult = ::std::move(hash);
}

/**
 * @copydoc FakeHdmiCecService::setInterfaceVersion
 *
 * Traces the value it replaces alongside the value it installs, exactly as the hash setter does, so
 * the one line a run prints here is the record of where in that run this service was made to report a
 * divergent version.  Held in a member of its own, so installing a version disturbs neither the hash
 * nor any canned response.
 */
void FakeHdmiCecService::setInterfaceVersion(int32_t version)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecService::setInterfaceVersion] Version set from "
              << interfaceVersionResult << " to " << version << std::endl;
    interfaceVersionResult = version;
}

/**
 * @copydoc FakeHdmiCecService::getController
 *
 * The strong pointer is copied out under the lock, so the controller outlives the call whatever the
 * service does next.  No statement in this class ever reassigns the member, which is what makes the
 * never-null guarantee hold for the life of the service.
 */
::android::sp<FakeHdmiCecController> FakeHdmiCecService::getController() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return controller;
}

/**
 * @copydoc FakeHdmiCecService::getListener
 *
 * Copied out under the lock, so the listener a caller obtained cannot be released underneath it by a
 * concurrent reset().
 */
::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> FakeHdmiCecService::getListener() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return listener;
}

/**
 * @copydoc FakeHdmiCecService::getLastClosedController
 *
 * Reports whatever close() was handed, including a null, because "the adapter closed nothing usable" is
 * itself a result a case has to be able to observe.
 */
::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> FakeHdmiCecService::getLastClosedController() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastClosedController;
}

/**
 * @copydoc FakeHdmiCecService::getOpenCallCount
 */
int32_t FakeHdmiCecService::getOpenCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return openCallCount;
}

/**
 * @copydoc FakeHdmiCecService::getCloseCallCount
 */
int32_t FakeHdmiCecService::getCloseCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return closeCallCount;
}

/**
 * @copydoc FakeHdmiCecService::getGetLogicalAddressesCallCount
 */
int32_t FakeHdmiCecService::getGetLogicalAddressesCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return getLogicalAddressesCallCount;
}

/**
 * @copydoc FakeHdmiCecService::getGetStateCallCount
 */
int32_t FakeHdmiCecService::getGetStateCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return getStateCallCount;
}

/**
 * @copydoc FakeHdmiCecService::getGetPropertyCallCount
 */
int32_t FakeHdmiCecService::getGetPropertyCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return getPropertyCallCount;
}

/**
 * @copydoc FakeHdmiCecService::getRegisterEventListenerCallCount
 */
int32_t FakeHdmiCecService::getRegisterEventListenerCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return registerEventListenerCallCount;
}

/**
 * @copydoc FakeHdmiCecService::getUnregisterEventListenerCallCount
 */
int32_t FakeHdmiCecService::getUnregisterEventListenerCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return unregisterEventListenerCallCount;
}

/**
 * @copydoc FakeHdmiCecService::reset
 *
 * One critical section restores every canned response and both metadata values, clears both captures
 * and zeroes all seven counters, so no case can observe a half-reset fake.  Each default is spelled
 * beside the line that restores it, which is what keeps the restored value and the documented default
 * from drifting apart.  Restoring the metadata pair is what stops a case that installed a divergent
 * hash or version deciding the outcome of the next one.  The owned controller is deliberately not
 * touched here, for the reason the warning on the declaration gives.
 */
void FakeHdmiCecService::reset()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    listener = nullptr;
    lastClosedController = nullptr;

    logicalAddressesResult = ::std::vector<int32_t> { DEFAULT_LOGICAL_ADDRESS };              // Default: Playback device
    closeResult = true;                                                                       // Default: session closed
    openReturnsNullController = false;                                                        // Default: a valid controller

    /*
     * Three statuses, not seven. getState(), getProperty(), registerEventListener() and
     * unregisterEventListener() answer a fixed ok, because the middleware never calls them and a
     * settable failure arm on a method nothing under test reaches would imply coverage that does not
     * exist. Their counters are still cleared below: those are what "the adapter never called this"
     * is asserted against.
     */
    openBinderStatus = ::android::binder::Status::ok();
    closeBinderStatus = ::android::binder::Status::ok();
    getLogicalAddressesBinderStatus = ::android::binder::Status::ok();

    openCallCount = 0;
    closeCallCount = 0;
    getLogicalAddressesCallCount = 0;
    getStateCallCount = 0;
    getPropertyCallCount = 0;
    registerEventListenerCallCount = 0;
    unregisterEventListenerCallCount = 0;

    interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCec::VERSION;                     // Default: the frozen version
    interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE;                      // Default: the frozen hash

    std::cout << "[FakeHdmiCecService::reset] Canned responses, captures and counters restored to defaults"
              << std::endl;
}


/**
 * @copydoc FakeHdmiCecService::fireOnMessageReceived
 *
 * The captured listener is copied out inside the critical section and invoked outside it, which is what
 * makes the re-entrancy the declaration describes safe.
 *
 * The status the invocation yields is traced rather than discarded, and what that traced value means
 * follows the local-versus-remote distinction on the declaration: from a local listener it is the
 * callback's own return value, while from a remote proxy the interface is oneway and the value reports
 * that the driver accepted the transaction.  Nothing in this body waits for, or can observe, a remote
 * callback's own outcome, so no branch here is conditioned on the traced status.
 *
 * @see getListener()
 */
bool FakeHdmiCecService::fireOnMessageReceived(const ::std::vector<uint8_t>& message)
{
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> target;
    {
        ::std::lock_guard<::std::mutex> guard(mutex);
        target = listener;
    }

    if (target == nullptr) {
        std::cout << "[FakeHdmiCecService::fireOnMessageReceived] No listener captured, delivery is a no-op"
                  << std::endl;
        return false;
    }

    const ::android::binder::Status status = target->onMessageReceived(message);

    std::cout << "[FakeHdmiCecService::fireOnMessageReceived] Delivered " << message.size()
              << " byte(s) to listener " << fakeHdmiCecTraceLabel(target.get())
              << ", listener returned: " << status.toString8().c_str() << std::endl;

    return true;
}

/**
 * @copydoc FakeHdmiCecService::fireOnStateChanged
 *
 * Identical in shape to fireOnMessageReceived(), including where the lock is released and what the
 * traced status does and does not establish; only the callback invoked and the values traced with it
 * differ.
 */
bool FakeHdmiCecService::fireOnStateChanged(::com::rdk::hal::hdmicec::State oldState,
                                           ::com::rdk::hal::hdmicec::State newState)
{
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> target;
    {
        ::std::lock_guard<::std::mutex> guard(mutex);
        target = listener;
    }

    if (target == nullptr) {
        std::cout << "[FakeHdmiCecService::fireOnStateChanged] No listener captured, delivery is a no-op"
                  << std::endl;
        return false;
    }

    const ::android::binder::Status status = target->onStateChanged(oldState, newState);

    std::cout << "[FakeHdmiCecService::fireOnStateChanged] Delivered "
              << ::com::rdk::hal::hdmicec::toString(oldState) << " -> "
              << ::com::rdk::hal::hdmicec::toString(newState) << " to listener "
              << fakeHdmiCecTraceLabel(target.get())
              << ", listener returned: " << status.toString8().c_str() << std::endl;

    return true;
}

/**
 * @copydoc FakeHdmiCecService::fireOnMessageSent
 *
 * Identical in shape to fireOnMessageReceived(), with one local naming difference: the status the
 * invocation yields is held as listenerStatus, because the send status being notified already occupies
 * the name status in this signature.
 */
bool FakeHdmiCecService::fireOnMessageSent(const ::std::vector<uint8_t>& message,
                                           ::com::rdk::hal::hdmicec::SendMessageStatus status)
{
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> target;
    {
        ::std::lock_guard<::std::mutex> guard(mutex);
        target = listener;
    }

    if (target == nullptr) {
        std::cout << "[FakeHdmiCecService::fireOnMessageSent] No listener captured, delivery is a no-op"
                  << std::endl;
        return false;
    }

    const ::android::binder::Status listenerStatus = target->onMessageSent(message, status);

    std::cout << "[FakeHdmiCecService::fireOnMessageSent] Delivered " << message.size()
              << " byte(s) with " << ::com::rdk::hal::hdmicec::toString(status)
              << " to listener " << fakeHdmiCecTraceLabel(target.get())
              << ", listener returned: " << listenerStatus.toString8().c_str() << std::endl;

    return true;
}

/**
 * @copydoc FakeHdmiCecService::getInstance
 *
 * Reads the pointer without taking a lock, and needs none: the harness publishes the fake before it
 * initialises the middleware and clears it during teardown, so no test thread and no binder thread can
 * be running while the value changes.
 */
FakeHdmiCecService* FakeHdmiCecService::getInstance()
{
    return instance;
}

/**
 * @copydoc FakeHdmiCecService::setInstance
 *
 * Traces the label of the object it replaces and then the label now in place, so a log carries the one
 * record of every change to this pointer - which is what lets getInstance() stay silent.
 */
void FakeHdmiCecService::setInstance(FakeHdmiCecService* newFake)
{
    std::cout << "[FakeHdmiCecService::setInstance] Setting instance from "
              << fakeHdmiCecTraceLabel(instance) << " to " << fakeHdmiCecTraceLabel(newFake)
              << std::endl;
    instance = newFake;
    std::cout << "[FakeHdmiCecService::setInstance] Instance is now: "
              << fakeHdmiCecTraceLabel(instance) << std::endl;
}

/**
 * @copydoc registerFakeHdmiCecService
 *
 * Four checks, in this order, and the order is what bounds the damage.  The null service and the binder
 * driver node are tested before libbinder is touched at all: the linked libbinder treats a driver it
 * cannot open as fatal and terminates the process, so a host with no kernel binder support reaches the
 * false return here instead of losing its whole run, every legacy case included.  Only then is a service
 * manager obtained and its answer tested, and only then the name offered and the resulting status
 * tested.
 *
 * A driver present but speaking a protocol version the linked libbinder was not built for stays fatal
 * inside libbinder, and is deliberately not screened here: policing that is the middleware's own
 * preflight, and a second copy of a production decision inside a test fake would be a second thing to
 * keep in step.  Nor does anything in this body time-limit the service manager it asks for, which is why
 * the declaration hands that bound to the parent harness.
 *
 * Every arm traces before it returns, so a false is always accompanied by the reason for it.
 */
bool registerFakeHdmiCecService(const ::android::sp<FakeHdmiCecService>& service)
{
    if (service == nullptr) {
        std::cout << "[FakeRegistration] Refusing to publish a null service" << std::endl;
        return false;
    }

    if (::access(FAKE_HDMI_CEC_BINDER_DRIVER, R_OK | W_OK) != 0) {
        std::cout << "[FakeRegistration] Binder driver " << FAKE_HDMI_CEC_BINDER_DRIVER
                  << " is absent or unopenable, service not published" << std::endl;
        return false;
    }

    const ::android::sp<::android::IServiceManager> serviceManager = ::android::defaultServiceManager();
    if (serviceManager == nullptr) {
        std::cout << "[FakeRegistration] Service manager is unreachable, service not published" << std::endl;
        return false;
    }

    const ::std::string& name = ::com::rdk::hal::hdmicec::IHdmiCec::serviceName();
    const ::android::status_t added = serviceManager->addService(::android::String16(name.c_str()), service);

    if (added != ::android::OK) {
        std::cout << "[FakeRegistration] Service manager refused \"" << name
                  << "\", status " << added << std::endl;
        return false;
    }

    std::cout << "[FakeRegistration] Published fake " << fakeHdmiCecTraceLabel(service.get())
              << " as \"" << name << "\"" << std::endl;
    return true;
}

/** @} */ // End of HDMI_CEC_FAKE_AIDL_SERVICE_IMPL
