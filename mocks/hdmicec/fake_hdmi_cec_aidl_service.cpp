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
#include <algorithm>
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
 * Each interface method takes the lock, counts the call, captures what a test reads back, traces it
 * and answers, except that the metadata getters count nothing and trace only on divergence. An
 * allocation poll (a one-byte frame addressed to its own initiator) is recorded only by
 * getTotalSendMessageCallCount() and getAllocationPolls() and answered from setAllocationPollResult()
 * or setLogicalAddressOccupied(), ACK_STATE_1 (free) by default, never from setSendMessageResult(),
 * though a non-ok setSendMessageBinderStatus() fails it. Non-static setters and accessors take the
 * same lock and accessors return copies; no GoogleTest, GoogleMock or binder threadpool is used,
 * because the fake-service host binary links this file too.
 */

/**
 * @file fake_hdmi_cec_aidl_service.cpp
 *
 * @brief Implementation of the test-scope fake com.rdk.hal.hdmicec AIDL HdmiCec service.
 *
 * Defines FakeHdmiCecController, FakeHdmiCecService and registerFakeHdmiCecService(). Both classes
 * derive from the generated server bases, so they present exactly the frozen 0.1.0.0 interface.
 *
 * @warning Test scope only: built for test targets and never listed in a production source list.
 * @see fake_hdmi_cec_aidl_service.h
 */

/** @brief Binder node checked before libbinder is touched, so a host without it gets false, not an abort. */
static const char FAKE_HDMI_CEC_BINDER_DRIVER[] = "/dev/binder";

/** @brief Word a trace prints in place of an object it was not given, so absence never prints empty. */
static const char FAKE_HDMI_CEC_TRACE_ABSENT[] = "absent";

/** @brief Lowest logical address IHdmiCecController accepts in an add or remove request. */
static const int32_t FAKE_HDMI_CEC_MIN_DIRECT_ADDRESS = 0;

/** @brief Highest logical address IHdmiCecController accepts in an add or remove request (0xE). */
static const int32_t FAKE_HDMI_CEC_MAX_DIRECT_ADDRESS = 14;

/**
 * @brief Reports whether @p address lies in the directly addressable range the controller accepts.
 *
 * @param [in] address                    - Logical address taken from an add or remove request
 *
 * @return bool                                   - true for 0..14 inclusive, false otherwise
 * @see FakeHdmiCecController::addLogicalAddresses(), FakeHdmiCecController::removeLogicalAddresses()
 */
static bool fakeHdmiCecIsDirectAddress(int32_t address)
{
    return (address >= FAKE_HDMI_CEC_MIN_DIRECT_ADDRESS) && (address <= FAKE_HDMI_CEC_MAX_DIRECT_ADDRESS);
}

// Static instance pointer
FakeHdmiCecService* FakeHdmiCecService::instance = nullptr;

/**
 * @brief Labels @p object "absent" when null, otherwise "#N" with an ordinal minted on its first sight.
 *
 * @param [in] object                     - Object to label, or nullptr; used only as an identity key
 *
 * @return ::std::string                          - The label, unchanged for the rest of the process
 * @note The registry is function-local behind a lock of its own and is never pruned.
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
 * @brief Counts and captures an add request, then registers its addresses when the answer is true.
 *
 * @param [in]  logicalAddresses          - Addresses requested, captured as they arrived
 * @param [out] _aidl_return              - Receives the forced result or, without one, true only when
 *                                          every address is in 0..14 and none is registered yet
 *
 * @return ::android::binder::Status              - The canned status; a non-ok one writes and registers nothing
 * @note Any configured delay is slept first, with no lock held.
 */
::android::binder::Status FakeHdmiCecController::addLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                                    bool* _aidl_return)
{
    // Optional delay, read under the lock and slept with it dropped, so capture reads are never
    // blocked behind it; see setAddLogicalAddressesDelayMs().
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

    // A forced result wins; otherwise every address must be in range and not yet registered.
    bool result = true;

    if (addLogicalAddressesResult.has_value()) {
        result = *addLogicalAddressesResult;
    } else {
        for (const int32_t address : logicalAddresses) {
            if (!fakeHdmiCecIsDirectAddress(address) ||
                (::std::find(registeredLogicalAddresses.begin(), registeredLogicalAddresses.end(), address) !=
                 registeredLogicalAddresses.end())) {
                result = false;
                break;
            }
        }
    }

    std::cout << "[FakeHdmiCecController::addLogicalAddresses] Received " << logicalAddresses.size()
              << " address(es), reporting " << (result ? "true" : "false")
              << ", status: " << addLogicalAddressesBinderStatus.toString8().c_str() << std::endl;

    if (!addLogicalAddressesBinderStatus.isOk()) {
        return addLogicalAddressesBinderStatus;
    }

    if (result) {
        for (const int32_t address : logicalAddresses) {
            if (::std::find(registeredLogicalAddresses.begin(), registeredLogicalAddresses.end(), address) ==
                registeredLogicalAddresses.end()) {
                registeredLogicalAddresses.push_back(address);
            }
        }
    }

    if (_aidl_return) {
        *_aidl_return = result;
    } else {
        std::cout << "[FakeHdmiCecController::addLogicalAddresses] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @brief Counts and captures a removal request, then deregisters its addresses when the answer is true.
 *
 * @param [in]  logicalAddresses          - Addresses to remove, captured as they arrived
 * @param [out] _aidl_return              - Receives the forced result or, without one, true only when
 *                                          every address is in 0..14 and currently registered
 *
 * @return ::android::binder::Status              - The canned status; a non-ok one writes and removes nothing
 */
::android::binder::Status FakeHdmiCecController::removeLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                                       bool* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++removeLogicalAddressesCallCount;
    lastRemovedLogicalAddresses = logicalAddresses;

    // A forced result wins; otherwise every address must be in range and currently registered.
    bool result = true;

    if (removeLogicalAddressesResult.has_value()) {
        result = *removeLogicalAddressesResult;
    } else {
        for (const int32_t address : logicalAddresses) {
            if (!fakeHdmiCecIsDirectAddress(address) ||
                (::std::find(registeredLogicalAddresses.begin(), registeredLogicalAddresses.end(), address) ==
                 registeredLogicalAddresses.end())) {
                result = false;
                break;
            }
        }
    }

    std::cout << "[FakeHdmiCecController::removeLogicalAddresses] Received " << logicalAddresses.size()
              << " address(es), reporting " << (result ? "true" : "false")
              << ", status: " << removeLogicalAddressesBinderStatus.toString8().c_str() << std::endl;

    if (!removeLogicalAddressesBinderStatus.isOk()) {
        return removeLogicalAddressesBinderStatus;
    }

    if (result) {
        for (const int32_t address : logicalAddresses) {
            registeredLogicalAddresses.erase(
                ::std::remove(registeredLogicalAddresses.begin(), registeredLogicalAddresses.end(), address),
                registeredLogicalAddresses.end());
        }
    }

    if (_aidl_return) {
        *_aidl_return = result;
    } else {
        std::cout << "[FakeHdmiCecController::removeLogicalAddresses] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @brief Counts every call and answers polls from per-address results, other frames from the canned status.
 *
 * @param [in]  message                   - Frame to send; a poll is one byte addressed to its own initiator
 * @param [out] _aidl_return              - Receives a poll's result (ACK_STATE_1, free, by default) or the
 *                                          setSendMessageResult() status; untouched on a non-ok status
 *
 * @return ::android::binder::Status              - The setSendMessageBinderStatus() status, polls included
 * @post Every call advances getTotalSendMessageCallCount(); a poll is recorded only in getAllocationPolls();
 *       any other frame is also counted by getSendMessageCallCount() and captured for getLastSentMessage().
 */
::android::binder::Status FakeHdmiCecController::sendMessage(const ::std::vector<uint8_t>& message,
                                                            ::com::rdk::hal::hdmicec::SendMessageStatus* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++sendMessageTotalCallCount;

    if ((message.size() == 1) && (((message[0] >> 4) & 0x0F) == (message[0] & 0x0F))) {
        const int32_t polled = static_cast<int32_t>(message[0] & 0x0F);
        const ::std::map<int32_t, ::com::rdk::hal::hdmicec::SendMessageStatus>::const_iterator answer =
            allocationPollResults.find(polled);
        const ::com::rdk::hal::hdmicec::SendMessageStatus pollStatus =
            (answer != allocationPollResults.end()) ? answer->second
                                                    : ::com::rdk::hal::hdmicec::SendMessageStatus::ACK_STATE_1;

        allocationPolls.push_back(polled);

        if (!sendMessageBinderStatus.isOk()) {
            std::cout << "[FakeHdmiCecController::sendMessage] Allocation poll of logical address " << polled
                      << " failed, status: " << sendMessageBinderStatus.toString8().c_str() << std::endl;

            return sendMessageBinderStatus;
        }

        std::cout << "[FakeHdmiCecController::sendMessage] Allocation poll of logical address " << polled
                  << ", reporting " << ::com::rdk::hal::hdmicec::toString(pollStatus) << std::endl;

        if (_aidl_return) {
            *_aidl_return = pollStatus;
        } else {
            std::cout << "[FakeHdmiCecController::sendMessage] Null out-parameter, status not written" << std::endl;
        }

        return ::android::binder::Status::ok();
    }

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
 * @brief Returns the installed interface version, tracing only when it differs from the compiled-in one.
 *
 * @return int32_t                                - The installed version, by default IHdmiCecController::VERSION
 * @note No selection outcome reads the controller's metadata, so a divergent value only traces.
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
 * @brief Returns the installed interface hash, tracing only when it differs from the compiled-in one.
 *
 * @return std::string                            - The installed hash, by default IHdmiCecController::HASHVALUE
 * @note No selection outcome reads the controller's metadata, so a divergent value only traces.
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
 * @brief Counts the transaction under its code, then releases the lock before the generated dispatch.
 *
 * The lock is released first because the dispatched method takes the same non-recursive mutex.
 */
::android::status_t FakeHdmiCecController::onTransact(uint32_t code, const ::android::Parcel& data,
                                                      ::android::Parcel* reply, uint32_t flags)
{
    {
        ::std::lock_guard<::std::mutex> guard(mutex);
        ++transactionCounts[code];
    }

    return ::com::rdk::hal::hdmicec::BnHdmiCecController::onTransact(code, data, reply, flags);
}

/**
 * @brief Returns a copy of the per-code transaction counts, taken under the lock.
 */
::std::map<uint32_t, int32_t> FakeHdmiCecController::getTransactionCounts() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return transactionCounts;
}

/**
 * @brief Forces the result addLogicalAddresses() reports in place of its validation, until reset().
 *
 * @param [in] result                     - Value to report; true registers every address not yet registered
 */
void FakeHdmiCecController::setAddLogicalAddressesResult(bool result)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesResult = result;
}

/**
 * @brief Forces the result removeLogicalAddresses() reports in place of its validation, until reset().
 *
 * @param [in] result                     - Value to report; true deregisters every address given
 */
void FakeHdmiCecController::setRemoveLogicalAddressesResult(bool result)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    removeLogicalAddressesResult = result;
}

/**
 * @brief Stores how long addLogicalAddresses() sleeps, with no lock held, before answering.
 *
 * @param [in] delayMs                    - Milliseconds to sleep; zero or negative means no delay
 */
void FakeHdmiCecController::setAddLogicalAddressesDelayMs(int32_t delayMs)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesDelayMs = delayMs;
}

/**
 * @brief Stores, uninterpreted, the status sendMessage() reports for frames other than allocation polls.
 *
 * @param [in] status                     - Status to report; ACK_STATE_0 by default
 */
void FakeHdmiCecController::setSendMessageResult(::com::rdk::hal::hdmicec::SendMessageStatus status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    sendMessageResult = status;
}

/**
 * @brief Stores the binder status addLogicalAddresses() returns.
 *
 * @param [in] status                     - Status to return, ok (the default) or non-ok
 */
void FakeHdmiCecController::setAddLogicalAddressesBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesBinderStatus = status;
}

/**
 * @brief Stores the binder status removeLogicalAddresses() returns.
 *
 * @param [in] status                     - Status to return, ok (the default) or non-ok
 */
void FakeHdmiCecController::setRemoveLogicalAddressesBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    removeLogicalAddressesBinderStatus = status;
}

/**
 * @brief Stores the binder status sendMessage() returns, for allocation polls and other frames alike.
 *
 * @param [in] status                     - Status to return, ok (the default) or non-ok
 */
void FakeHdmiCecController::setSendMessageBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    sendMessageBinderStatus = status;
}

/**
 * @brief Marks @p address taken, so its polls answer ACK_STATE_0, or drops its answer to restore ACK_STATE_1.
 *
 * @param [in] address                    - Logical address whose allocation poll is answered
 * @param [in] occupied                   - true to mark it taken, false to restore the free default
 */
void FakeHdmiCecController::setLogicalAddressOccupied(int32_t address, bool occupied)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    if (occupied) {
        allocationPollResults[address] = ::com::rdk::hal::hdmicec::SendMessageStatus::ACK_STATE_0;
    } else {
        allocationPollResults.erase(address);
    }
}

/**
 * @brief Stores the status an allocation poll of @p address reports, replacing any earlier answer.
 *
 * @param [in] address                    - Logical address whose allocation poll is answered
 * @param [in] status                     - Status that poll reports
 */
void FakeHdmiCecController::setAllocationPollResult(int32_t address,
                                                    ::com::rdk::hal::hdmicec::SendMessageStatus status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    allocationPollResults[address] = status;
}

/**
 * @brief Traces the replaced and the new hash on one line, then stores @p hash unvalidated.
 *
 * @param [in] hash                       - Hash getInterfaceHash() reports; HASHVALUE restores the default
 */
void FakeHdmiCecController::setInterfaceHash(std::string hash)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecController::setInterfaceHash] Hash set from \"" << interfaceHashResult
              << "\" to \"" << hash << "\"" << std::endl;
    interfaceHashResult = ::std::move(hash);
}

/**
 * @brief Traces the replaced and the new version, then stores @p version unvalidated.
 *
 * @param [in] version                    - Version getInterfaceVersion() reports; VERSION restores the default
 */
void FakeHdmiCecController::setInterfaceVersion(int32_t version)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecController::setInterfaceVersion] Version set from "
              << interfaceVersionResult << " to " << version << std::endl;
    interfaceVersionResult = version;
}

/**
 * @brief Returns a copy of the address vector the last addLogicalAddresses() call carried.
 *
 * @return ::std::vector<int32_t>                 - The captured vector, empty if never called
 */
::std::vector<int32_t> FakeHdmiCecController::getLastAddedLogicalAddresses() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastAddedLogicalAddresses;
}

/**
 * @brief Returns a copy of the address vector the last removeLogicalAddresses() call carried.
 *
 * @return ::std::vector<int32_t>                 - The captured vector, empty if never called
 */
::std::vector<int32_t> FakeHdmiCecController::getLastRemovedLogicalAddresses() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastRemovedLogicalAddresses;
}

/**
 * @brief Returns a copy of the last frame sendMessage() captured, allocation polls excluded.
 *
 * @return ::std::vector<uint8_t>                 - The captured frame, empty if none arrived
 */
::std::vector<uint8_t> FakeHdmiCecController::getLastSentMessage() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastSentMessage;
}

/**
 * @brief Returns how many addLogicalAddresses() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecController::getAddLogicalAddressesCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return addLogicalAddressesCallCount;
}

/**
 * @brief Returns how many removeLogicalAddresses() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecController::getRemoveLogicalAddressesCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return removeLogicalAddressesCallCount;
}

/**
 * @brief Returns how many non-poll frames sendMessage() received since construction or the last reset().
 *
 * @return int32_t                                - The frame count, allocation polls excluded
 */
int32_t FakeHdmiCecController::getSendMessageCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return sendMessageCallCount;
}

/**
 * @brief Returns how many sendMessage() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count, allocation polls included
 */
int32_t FakeHdmiCecController::getTotalSendMessageCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return sendMessageTotalCallCount;
}

/**
 * @brief Returns a copy of the addresses allocation polls asked about, in arrival order.
 *
 * @return ::std::vector<int32_t>                 - The polled addresses, empty if none arrived
 */
::std::vector<int32_t> FakeHdmiCecController::getAllocationPolls() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return allocationPolls;
}

/**
 * @brief Returns a copy of the addresses registered and not since removed, in the order they were added.
 *
 * @return ::std::vector<int32_t>                 - The registered addresses
 */
::std::vector<int32_t> FakeHdmiCecController::getRegisteredLogicalAddresses() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return registeredLogicalAddresses;
}

/**
 * @brief Drops every registered logical address.
 *
 * @post getRegisteredLogicalAddresses() is empty.
 * @see FakeHdmiCecService::close()
 */
void FakeHdmiCecController::clearRegisteredLogicalAddresses()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    registeredLogicalAddresses.clear();
}

/**
 * @brief Restores every default in one critical section and clears every capture, counter and record.
 *
 * @post No add or remove result is forced; captures, all four counters, allocation-poll answers and
 *       records, and registrations are empty or zero; the frozen version and hash are reported again.
 * @see FakeHdmiCecService::reset()
 */
void FakeHdmiCecController::reset()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    addLogicalAddressesResult = ::std::nullopt;                                             // Default: validate each add
    removeLogicalAddressesResult = ::std::nullopt;                                          // Default: validate each removal
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
    sendMessageTotalCallCount = 0;

    allocationPollResults.clear();                                                          // Default: every poll answers free
    allocationPolls.clear();
    registeredLogicalAddresses.clear();
    transactionCounts.clear();

    interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION;          // Default: the frozen version
    interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE;           // Default: the frozen hash

    std::cout << "[FakeHdmiCecController::reset] Canned responses, captures and counters restored to defaults"
              << std::endl;
}


/**
 * @brief Clears the published instance only when it still refers to this object.
 *
 * @post getInstance() no longer returns this object, and a fake published later stays reachable.
 */
FakeHdmiCecService::~FakeHdmiCecService()
{
    if (instance == this) {
        instance = nullptr;
    }
}

/**
 * @brief Counts the call and reports DEFAULT_STATE.
 *
 * @param [out] _aidl_return              - Receives DEFAULT_STATE; untouched when null
 *
 * @return ::android::binder::Status              - Always ok
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
 * @brief Counts the call, traces the property and reports an empty optional.
 *
 * @param [in]  property                  - Property requested; traced only
 * @param [out] _aidl_return              - Receives ::std::nullopt; untouched when null
 *
 * @return ::android::binder::Status              - Always ok
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
 * @brief Counts the call and reports the installed vector or, by default, the controller's registrations.
 *
 * @param [out] _aidl_return              - Receives the vector as it stands, never sorted or truncated;
 *                                          untouched on a non-ok status or when null
 *
 * @return ::android::binder::Status              - The setGetLogicalAddressesBinderStatus() status
 */
::android::binder::Status FakeHdmiCecService::getLogicalAddresses(::std::vector<int32_t>* _aidl_return)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    ++getLogicalAddressesCallCount;

    const ::std::vector<int32_t> reported =
        logicalAddressesResult.has_value() ? *logicalAddressesResult : controller->getRegisteredLogicalAddresses();

    std::cout << "[FakeHdmiCecService::getLogicalAddresses] Reporting " << reported.size()
              << (logicalAddressesResult.has_value() ? " installed" : " registered")
              << " address(es), status: " << getLogicalAddressesBinderStatus.toString8().c_str() << std::endl;

    if (!getLogicalAddressesBinderStatus.isOk()) {
        return getLogicalAddressesBinderStatus;
    }

    if (_aidl_return) {
        *_aidl_return = reported;
    } else {
        std::cout << "[FakeHdmiCecService::getLogicalAddresses] Null out-parameter, addresses not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @brief Counts the call and captures the listener, then hands out the owned controller or nullptr.
 *
 * @param [in]  cecControllerListener     - Listener to capture, even when the open is refused
 * @param [out] _aidl_return              - Receives the controller, or nullptr when the null-controller
 *                                          flag is set; untouched on a non-ok status or when null
 *
 * @return ::android::binder::Status              - The setOpenBinderStatus() status
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
 * @brief Counts and captures the close; an ok status with a true result drops the controller's registrations.
 *
 * @param [in]  hdmiCecController         - Controller being closed, captured as given
 * @param [out] _aidl_return              - Receives the setCloseResult() value; untouched on a non-ok
 *                                          status or when null
 *
 * @return ::android::binder::Status              - The setCloseBinderStatus() status
 * @note The captured listener is kept, so a trigger fired after close still reaches the adapter.
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

    if (closeResult) {
        controller->clearRegisteredLogicalAddresses();
    }

    if (_aidl_return) {
        *_aidl_return = closeResult;
    } else {
        std::cout << "[FakeHdmiCecService::close] Null out-parameter, result not written" << std::endl;
    }

    return ::android::binder::Status::ok();
}

/**
 * @brief Counts and traces the offered listener without retaining it, and reports true.
 *
 * @param [in]  cecEventListener          - Listener offered; traced only
 * @param [out] _aidl_return              - Receives true; untouched when null
 *
 * @return ::android::binder::Status              - Always ok
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
 * @brief Counts and traces the listener being withdrawn and reports true, since nothing was retained.
 *
 * @param [in]  cecEventListener          - Listener being withdrawn; traced only
 * @param [out] _aidl_return              - Receives true; untouched when null
 *
 * @return ::android::binder::Status              - Always ok
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
 * @brief Returns the installed interface version, tracing only when it differs from the compiled-in one.
 *
 * @return int32_t                                - The installed version, by default IHdmiCec::VERSION
 * @see setInterfaceVersion(), reset()
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
 * @brief Returns the installed interface hash, tracing only when it differs from the compiled-in one.
 *
 * @return std::string                            - The installed hash, by default IHdmiCec::HASHVALUE
 * @note A divergent hash is overridden metadata: halcompat refuses only an empty, "-1" or "notfrozen"
 *       hash and otherwise judges the version.
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
 * @brief Counts the transaction under its code, then releases the lock before the generated dispatch.
 *
 * The lock is released first because the dispatched method takes the same non-recursive mutex.
 */
::android::status_t FakeHdmiCecService::onTransact(uint32_t code, const ::android::Parcel& data,
                                                   ::android::Parcel* reply, uint32_t flags)
{
    {
        ::std::lock_guard<::std::mutex> guard(mutex);
        ++transactionCounts[code];
    }

    return ::com::rdk::hal::hdmicec::BnHdmiCec::onTransact(code, data, reply, flags);
}

/**
 * @brief Returns a copy of the per-code transaction counts, taken under the lock.
 */
::std::map<uint32_t, int32_t> FakeHdmiCecService::getTransactionCounts() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return transactionCounts;
}


/**
 * @brief Stores, whole and unvalidated, the vector getLogicalAddresses() reports instead of the registrations.
 *
 * @param [in] logicalAddresses           - Addresses to report, of any width
 */
void FakeHdmiCecService::setLogicalAddressesResult(const ::std::vector<int32_t>& logicalAddresses)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    logicalAddressesResult = logicalAddresses;
}

/**
 * @brief Stores the result close() reports, independently of its binder status.
 *
 * @param [in] result                     - Value to report; true by default
 */
void FakeHdmiCecService::setCloseResult(bool result)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    closeResult = result;
}

/**
 * @brief Stores whether an ok open() hands out nullptr instead of the owned controller.
 *
 * @param [in] returnsNull                - true for nullptr; false, the default, for the owned controller
 */
void FakeHdmiCecService::setOpenReturnsNullController(bool returnsNull)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    openReturnsNullController = returnsNull;
}

/**
 * @brief Stores, uninterpreted, the binder status open() returns.
 *
 * @param [in] status                     - Status to return, ok (the default) or any exception code
 */
void FakeHdmiCecService::setOpenBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    openBinderStatus = status;
}

/**
 * @brief Stores the binder status close() returns, independently of its result.
 *
 * @param [in] status                     - Status to return, ok (the default) or non-ok
 */
void FakeHdmiCecService::setCloseBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    closeBinderStatus = status;
}

/**
 * @brief Stores the binder status getLogicalAddresses() returns, independently of the reported vector.
 *
 * @param [in] status                     - Status to return, ok (the default) or non-ok
 */
void FakeHdmiCecService::setGetLogicalAddressesBinderStatus(const ::android::binder::Status& status)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    getLogicalAddressesBinderStatus = status;
}

/**
 * @brief Traces the replaced and the new hash on one line, then stores @p hash unvalidated.
 *
 * @param [in] hash                       - Hash getInterfaceHash() reports; HASHVALUE restores the default
 * @note The L1 harness's incompatible mode installs "-1", which halcompat refuses.
 */
void FakeHdmiCecService::setInterfaceHash(std::string hash)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecService::setInterfaceHash] Hash set from \"" << interfaceHashResult
              << "\" to \"" << hash << "\"" << std::endl;
    interfaceHashResult = ::std::move(hash);
}

/**
 * @brief Traces the replaced and the new version, then stores @p version unvalidated.
 *
 * @param [in] version                    - Version getInterfaceVersion() reports; VERSION restores the default
 */
void FakeHdmiCecService::setInterfaceVersion(int32_t version)
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    std::cout << "[FakeHdmiCecService::setInterfaceVersion] Version set from "
              << interfaceVersionResult << " to " << version << std::endl;
    interfaceVersionResult = version;
}

/**
 * @brief Returns, under the lock, a strong reference to the owned controller.
 *
 * @return ::android::sp<FakeHdmiCecController>   - The controller, never null because it is never reassigned
 */
::android::sp<FakeHdmiCecController> FakeHdmiCecService::getController() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return controller;
}

/**
 * @brief Returns, under the lock, a strong reference to the listener the last open() captured.
 *
 * @return ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> - The listener, null before open()
 */
::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> FakeHdmiCecService::getListener() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return listener;
}

/**
 * @brief Returns, under the lock, the controller the last close() was handed, a null one included.
 *
 * @return ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> - The controller, null before close()
 */
::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> FakeHdmiCecService::getLastClosedController() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return lastClosedController;
}

/**
 * @brief Returns how many open() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getOpenCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return openCallCount;
}

/**
 * @brief Returns how many close() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getCloseCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return closeCallCount;
}

/**
 * @brief Returns how many getLogicalAddresses() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getGetLogicalAddressesCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return getLogicalAddressesCallCount;
}

/**
 * @brief Returns how many getState() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getGetStateCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return getStateCallCount;
}

/**
 * @brief Returns how many getProperty() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getGetPropertyCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return getPropertyCallCount;
}

/**
 * @brief Returns how many registerEventListener() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getRegisterEventListenerCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return registerEventListenerCallCount;
}

/**
 * @brief Returns how many unregisterEventListener() calls arrived since construction or the last reset().
 *
 * @return int32_t                                - The call count
 */
int32_t FakeHdmiCecService::getUnregisterEventListenerCallCount() const
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    return unregisterEventListenerCallCount;
}

/**
 * @brief Restores every default in one critical section and clears both captures and all seven counters.
 *
 * @post getLogicalAddresses() reports the registrations again, the frozen version and hash are back and
 *       the triggers are no-ops; the owned controller is not reset.
 * @see FakeHdmiCecController::reset()
 */
void FakeHdmiCecService::reset()
{
    ::std::lock_guard<::std::mutex> guard(mutex);

    listener = nullptr;
    lastClosedController = nullptr;

    logicalAddressesResult.reset();                                                           // Default: report registrations
    closeResult = true;                                                                       // Default: session closed
    openReturnsNullController = false;                                                        // Default: a valid controller

    // Three statuses, not seven: the four methods the middleware never calls answer a fixed ok, but
    // their counters are still cleared below.
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
    transactionCounts.clear();

    interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCec::VERSION;                     // Default: the frozen version
    interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE;                      // Default: the frozen hash

    std::cout << "[FakeHdmiCecService::reset] Canned responses, captures and counters restored to defaults"
              << std::endl;
}


/**
 * @brief Invokes onMessageReceived() on the captured listener outside the lock and traces its Status.
 *
 * @param [in] message                    - Frame to deliver
 *
 * @return bool                                   - Whether a listener was captured and invoked
 * @retval true                                   - Invoked, whatever Status it returned
 * @retval false                                  - No listener captured; nothing delivered
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
 * @brief Invokes onStateChanged() on the captured listener outside the lock and traces its Status.
 *
 * @param [in] oldState                   - State being left
 * @param [in] newState                   - State being entered
 *
 * @return bool                                   - Whether a listener was captured and invoked
 * @retval true                                   - Invoked, whatever Status it returned
 * @retval false                                  - No listener captured; nothing delivered
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
 * @brief Invokes onMessageSent() on the captured listener outside the lock and traces its Status.
 *
 * @param [in] message                    - Frame the notification refers to
 * @param [in] status                     - Send status to notify
 *
 * @return bool                                   - Whether a listener was captured and invoked
 * @retval true                                   - Invoked, whatever Status it returned
 * @retval false                                  - No listener captured; nothing delivered
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
 * @brief Returns, without taking a lock, the pointer setInstance() last stored.
 *
 * @return FakeHdmiCecService*                    - The published fake, or nullptr
 * @note Both harnesses publish it before the middleware initialises; the L1 harness never clears it,
 *       and the separate-process host clears it once serving ends.
 */
FakeHdmiCecService* FakeHdmiCecService::getInstance()
{
    return instance;
}

/**
 * @brief Stores @p newFake as the published fake, tracing the label replaced and the label now in place.
 *
 * @param [in] newFake                    - Fake to publish, or nullptr to clear
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
 * @brief Checks @p service, the binder node and the service manager, then registers the service by name.
 *
 * @param [in] service                    - Fake to publish under IHdmiCec::serviceName(); null is refused
 *
 * @return bool                                   - true once published; false, with a trace, when a check or
 *                                                  the registration fails
 * @note The null and node checks precede any libbinder call, so a host without binder gets false.
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
