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

#pragma once

#include <binder/Status.h>
#include <com/rdk/hal/PropertyValue.h>
#include <com/rdk/hal/hdmicec/BnHdmiCec.h>
#include <com/rdk/hal/hdmicec/BnHdmiCecController.h>
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <com/rdk/hal/hdmicec/IHdmiCecEventListener.h>
#include <com/rdk/hal/hdmicec/Property.h>
#include <com/rdk/hal/hdmicec/SendMessageStatus.h>
#include <com/rdk/hal/hdmicec/State.h>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utils/StrongPointer.h>
#include <vector>

/**
 * @defgroup HDMI_CEC_MOCKS HDMI CEC Middleware Test Doubles
 * @{
 * @par Test Double Specification
 * Doubles that stand in for a HAL while the CCEC middleware is under test: the GoogleMock double of
 * the legacy C ABI in hdmi_cec_driver_mock.h, and the com.rdk.hal.hdmicec AIDL fake declared here.
 * No production source list references this directory.
 *
 */

/**
 * @defgroup HDMI_CEC_FAKE_AIDL_SERVICE HDMI CEC Fake AIDL Service
 * @{
 * @par Fake Service Specification
 * Hand-written fakes of the frozen com.rdk.hal.hdmicec 0.1.0.0 IHdmiCec and IHdmiCecController
 * interfaces, derived from the generated Bn* server bases and published under
 * ::com::rdk::hal::hdmicec::IHdmiCec::serviceName().@n
 * In process every call, metadata included, dispatches virtually to the fake; out of process the
 * generated onTransact() answers the metadata transactions from the compiled-in constants.
 *
 */

/**
 * @file fake_hdmi_cec_aidl_service.h
 *
 * @brief Declaration of the test-scope fake com.rdk.hal.hdmicec AIDL HdmiCec service.
 *
 * Declares FakeHdmiCecController, FakeHdmiCecService, fakeHdmiCecTraceLabel() and
 * registerFakeHdmiCecService().  There is no GoogleMock or GoogleTest dependency, because the
 * fake-service host binary also compiles the implementation and links only AIDL and binder libraries.
 *
 * @warning Test scope only.  Never reference this header from a production source list, from
 *          ccec/src, or from any installed header.
 *
 * @see hdmi_cec_driver_mock.h
 */

/**
 * @brief Reports one traced object as a stable, address-free label for diagnostics.
 *
 * @param [in] object                     - Object to label, or nullptr; used only as an identity key
 *
 * @return ::std::string                          - The label
 * @retval "absent"                               - The object was nullptr
 * @retval "#N"                                   - N is this object's ordinal, minted on first sight
 *
 * @post A non-null object keeps its ordinal, and so its label, for the rest of the process.
 * @warning Diagnostics only: ordinals depend on trace order, so never parse or assert on a label.
 * @see FakeHdmiCecService, registerFakeHdmiCecService()
 */
::std::string fakeHdmiCecTraceLabel(const void* object);

/**
 * @brief Test-scope fake of the com.rdk.hal.hdmicec IHdmiCecController AIDL interface.
 *
 * The controller FakeHdmiCecService::open() hands out: its add, remove and send methods count and
 * capture calls, validate and track registrations, and answer from setters with fixed defaults.
 * An allocation poll bypasses getSendMessageCallCount(), getLastSentMessage() and
 * setSendMessageResult(): it is recorded in getAllocationPolls() and answers ACK_STATE_1 (free)
 * unless setAllocationPollResult() or setLogicalAddressOccupied() says otherwise.
 *
 * @warning Test scope only - never reference this class from a production source list.
 * @see FakeHdmiCecService
 */
class FakeHdmiCecController : public ::com::rdk::hal::hdmicec::BnHdmiCecController {
public:
    /**
     * @brief Adds logical addresses on the fake HAL, validating them as IHdmiCecController does.
     *
     * @param [in]  logicalAddresses          - Addresses the client marshalled, captured verbatim
     * @param [out] _aidl_return              - By default true only if all are in 0..14 and unregistered
     *
     * @return ::android::binder::Status              - The canned binder status; non-ok writes nothing
     * @post Only an ok status with a true result adds them to getRegisteredLogicalAddresses().
     * @see setAddLogicalAddressesResult(), setAddLogicalAddressesBinderStatus()
     */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                 bool* _aidl_return) override;

    /**
     * @brief Removes logical addresses on the fake HAL, validating them as IHdmiCecController does.
     *
     * @param [in]  logicalAddresses          - Addresses the client marshalled, captured verbatim
     * @param [out] _aidl_return              - Receives the result, by default true only when every
     *                                          address is in 0..14 and currently registered
     *
     * @return ::android::binder::Status              - The canned binder status; a non-ok one leaves
     *                                                  the out-parameter and registrations untouched
     *
     * @post The call count and capture have advanced; only a true result deregisters the addresses.
     * @see setRemoveLogicalAddressesResult(), setRemoveLogicalAddressesBinderStatus()
     */
    ::android::binder::Status removeLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                    bool* _aidl_return) override;

    /**
     * @brief Transmits a CEC frame through the fake HAL, answering one-byte self-addressed polls itself.
     *
     * @param [in]  message                   - Raw frame; a poll is recorded in getAllocationPolls(), any
     *                                          other frame is captured whole for getLastSentMessage()
     * @param [out] _aidl_return              - Receives the poll answer or the canned send status;
     *                                          untouched on a non-ok status
     *
     * @return ::android::binder::Status              - The canned binder status, polls included
     * @see setSendMessageResult(), setAllocationPollResult(), getTotalSendMessageCallCount()
     */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t>& message,
                                          ::com::rdk::hal::hdmicec::SendMessageStatus* _aidl_return) override;

    /**
     * @brief Reports the interface version this fake claims.
     *
     * @return int32_t                                - The version setInterfaceVersion() last
     *                                                  installed; IHdmiCecController::VERSION by default
     *
     * @warning Effective under local (in-process) dispatch only.  Across a binder transaction the
     *          generated onTransact() answers from the compiled-in constant instead.
     * @see setInterfaceVersion(), reset()
     */
    int32_t getInterfaceVersion() override;

    /**
     * @brief Reports the interface hash this fake claims.
     *
     * @return std::string                            - The hash setInterfaceHash() last installed;
     *                                                  IHdmiCecController::HASHVALUE by default
     *
     * @warning Effective under local (in-process) dispatch only, exactly as for
     *          getInterfaceVersion().
     * @see setInterfaceHash(), reset()
     */
    std::string getInterfaceHash() override;

    /**
     * @brief Counts one incoming binder transaction by code, then dispatches it as generated.
     *
     * @param [in]  code                      - Transaction code, TRANSACTION_* for an interface method
     * @param [in]  data                      - Marshalled request, passed through unread
     * @param [out] reply                     - Reply parcel the generated dispatch writes
     * @param [in]  flags                     - Transaction flags, passed through
     *
     * @return ::android::status_t                    - What BnHdmiCecController::onTransact() returns
     *
     * @post getTransactionCounts() reports one more transaction under code.
     * @see getTransactionCounts()
     */
    ::android::status_t onTransact(uint32_t code, const ::android::Parcel& data, ::android::Parcel* reply,
                                   uint32_t flags) override;

    /**
     * @brief Returns the incoming binder transactions by code since construction or reset().
     *
     * @return ::std::map<uint32_t, int32_t>          - Count per code; a code never received is absent
     *
     * @note Transactions BBinder::transact() answers itself, PING_TRANSACTION among them, are never
     *       counted, and local (in-process) dispatch bypasses onTransact(), so nothing is counted there.
     * @see onTransact(), reset()
     */
    ::std::map<uint32_t, int32_t> getTransactionCounts() const;

    // Canned responses for test access

    /**
     * @brief Forces the boolean addLogicalAddresses() reports, in place of its contract validation.
     *
     * Reaches the address-unavailable arm, where the adapter must raise AddressNotAvailableException.
     *
     * @param [in] result                     - Value addLogicalAddresses() writes to its out-parameter
     *
     * @post false registers nothing and true registers every address not yet registered, until reset().
     * @see addLogicalAddresses()
     */
    void setAddLogicalAddressesResult(bool result);

    /**
     * @brief Forces the boolean removeLogicalAddresses() reports, in place of its contract validation.
     *
     * Reaches the ignored-failure arm, where the adapter must log a false result and raise nothing.
     *
     * @param [in] result                     - Value removeLogicalAddresses() writes to its out-parameter
     *
     * @post false deregisters nothing and true deregisters every address given, until reset().
     * @see removeLogicalAddresses()
     */
    void setRemoveLogicalAddressesResult(bool result);

    /**
     * @brief Makes addLogicalAddresses() take at least @p delayMs milliseconds to answer.
     *
     * Reaches the middleware's slow-HAL-call warning, a threshold rather than a timeout.
     *
     * @param [in] delayMs                    - Milliseconds to sleep before answering.  Zero or
     *                                          negative disables the delay
     *
     * @post Default is 0, so every other case pays nothing.  Cleared by reset().
     * @warning Real wall-clock time, slept with no lock held; keep it just above the threshold.
     * @see addLogicalAddresses()
     */
    void setAddLogicalAddressesDelayMs(int32_t delayMs);

    /**
     * @brief Selects the SendMessageStatus sendMessage() reports for an application frame.
     *
     * Reaches every arm of the adapter's status translation, whose sense inverts for broadcasts.
     *
     * @param [in] status                     - Value sendMessage() writes for every non-poll frame
     *
     * @post Default is ACK_STATE_0, acknowledged for a directed frame; polls default to ACK_STATE_1.
     * @see sendMessage(), setAllocationPollResult()
     */
    void setSendMessageResult(::com::rdk::hal::hdmicec::SendMessageStatus status);

    /**
     * @brief Installs the binder status addLogicalAddresses() returns.
     *
     * Reaches the add transport-failure arm, which the adapter must translate into IOException.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     * @see addLogicalAddresses()
     */
    void setAddLogicalAddressesBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status removeLogicalAddresses() returns.
     *
     * Reaches the removal transport-failure arm, which the adapter must log and swallow.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     * @see removeLogicalAddresses()
     */
    void setRemoveLogicalAddressesBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status sendMessage() returns, allocation polls included.
     *
     * Reaches the transmit transport-failure arm (IOException whatever SendMessageStatus is set)
     * and the failed-poll arm of the middleware's allocation.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     * @see sendMessage()
     */
    void setSendMessageBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Marks a logical address as taken, or free, for allocation polls.
     *
     * @param [in] address                    - Logical address whose allocation poll is answered
     * @param [in] occupied                   - true answers ACK_STATE_0 (taken); false restores the
     *                                          default ACK_STATE_1 (free)
     *
     * @post Cleared by reset().
     *
     * @see sendMessage(), setAllocationPollResult()
     */
    void setLogicalAddressOccupied(int32_t address, bool occupied);

    /**
     * @brief Selects the send status an allocation poll of @p address reports.
     *
     * Reaches the failed-poll arm of the middleware's allocation, for example with BUSY.
     *
     * @param [in] address                    - Logical address whose allocation poll is answered
     * @param [in] status                     - Send status that poll reports
     *
     * @post That poll reports @p status, not the default ACK_STATE_1 (free), until reset().
     *
     * @see sendMessage(), setLogicalAddressOccupied()
     */
    void setAllocationPollResult(int32_t address, ::com::rdk::hal::hdmicec::SendMessageStatus status);

    /**
     * @brief Overrides the interface hash this fake controller claims.
     *
     * Reaches the divergence trace on getInterfaceHash(); no selection outcome reads it.
     *
     * @param [in] hash                       - Hash string to report
     *
     * @post Default is IHdmiCecController::HASHVALUE, the frozen hash; reset() restores it.
     * @warning Effective under local (in-process) dispatch only.
     * @see getInterfaceHash(), FakeHdmiCecService::setInterfaceHash()
     */
    void setInterfaceHash(std::string hash);

    /**
     * @brief Overrides the interface version this fake controller claims.
     *
     * Reaches the divergence trace on getInterfaceVersion(); no selection outcome reads it.
     *
     * @param [in] version                    - Interface version to report, installed unvalidated
     *
     * @post Default is IHdmiCecController::VERSION, the frozen version; reset() restores it.
     * @warning Effective under local (in-process) dispatch only, exactly as for setInterfaceHash().
     * @see getInterfaceVersion(), FakeHdmiCecService::setInterfaceVersion()
     */
    void setInterfaceVersion(int32_t version);

    // Observation accessors for test access

    /**
     * @brief Returns the address vector the last addLogicalAddresses() call carried.
     *
     * The assertion target for single-element marshalling: one entry, the requested address.
     *
     * @return ::std::vector<int32_t>                 - The captured vector, empty if never called
     *
     * @see addLogicalAddresses()
     */
    ::std::vector<int32_t> getLastAddedLogicalAddresses() const;

    /**
     * @brief Returns the address vector the last removeLogicalAddresses() call carried.
     *
     * The removal-side counterpart of getLastAddedLogicalAddresses().
     *
     * @return ::std::vector<int32_t>                 - The captured vector, empty if never called
     *
     * @see removeLogicalAddresses()
     */
    ::std::vector<int32_t> getLastRemovedLogicalAddresses() const;

    /**
     * @brief Returns the last application frame sendMessage() was given, allocation polls excluded.
     *
     * The assertion target for frame marshalling and the length boundary: a frame at the limit
     * arrives byte for byte, and an over-length frame never arrives, truncated or otherwise.
     *
     * @return ::std::vector<uint8_t>                 - The captured frame, empty when none was captured
     *
     * @see sendMessage(), getSendMessageCallCount(), getAllocationPolls()
     */
    ::std::vector<uint8_t> getLastSentMessage() const;

    /**
     * @brief Returns how many times addLogicalAddresses() has been called.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see addLogicalAddresses(), reset()
     */
    int32_t getAddLogicalAddressesCallCount() const;

    /**
     * @brief Returns how many times removeLogicalAddresses() has been called.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see removeLogicalAddresses(), reset()
     */
    int32_t getRemoveLogicalAddressesCallCount() const;

    /**
     * @brief Returns how many application frames sendMessage() has been given, allocation polls excluded.
     *
     * A frame the adapter rejects before the HAL call leaves it unchanged, unlike a failed transmit.
     *
     * @return int32_t                                - Frame count since construction or the last reset()
     *
     * @see sendMessage(), getTotalSendMessageCallCount(), getAllocationPolls()
     */
    int32_t getSendMessageCallCount() const;

    /**
     * @brief Returns how many times sendMessage() has been called, allocation polls included.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see sendMessage(), getSendMessageCallCount(), getAllocationPolls()
     */
    int32_t getTotalSendMessageCallCount() const;

    /**
     * @brief Returns the addresses allocation polls asked about, in the order they arrived.
     *
     * @return ::std::vector<int32_t>                 - Polled addresses since construction or reset()
     *
     * @see sendMessage(), reset()
     */
    ::std::vector<int32_t> getAllocationPolls() const;

    /**
     * @brief Returns the logical addresses currently registered through this controller.
     *
     * @return ::std::vector<int32_t>                 - Addresses added and not since removed, in the
     *                                                  order they were added
     *
     * @see addLogicalAddresses(), removeLogicalAddresses(), clearRegisteredLogicalAddresses()
     */
    ::std::vector<int32_t> getRegisteredLogicalAddresses() const;

    /**
     * @brief Drops every registered logical address, as a successful IHdmiCec close does.
     *
     * @post getRegisteredLogicalAddresses() is empty.
     *
     * @see FakeHdmiCecService::close()
     */
    void clearRegisteredLogicalAddresses();

    /**
     * @brief Restores every canned response to its default and clears every capture and counter.
     *
     * Called from fixture set-up, so the one long-lived registered fake leaks nothing between cases.
     *
     * @post Canned responses hold their documented defaults and no add or remove result is forced;
     *       captures, allocation-poll answers and registrations are empty; counters are zero.
     * @see FakeHdmiCecService::reset()
     */
    void reset();

private:
    /** @brief Guards every canned response and capture below; held only for short sections. */
    mutable ::std::mutex mutex;

    /** @brief Forced addLogicalAddresses() result; ::std::nullopt (default) validates each request. */
    ::std::optional<bool> addLogicalAddressesResult;

    /** @brief Forced removeLogicalAddresses() result; ::std::nullopt (default) validates each request. */
    ::std::optional<bool> removeLogicalAddressesResult;

    /** @brief Milliseconds addLogicalAddresses() sleeps, unlocked, before answering (default 0). */
    int32_t addLogicalAddressesDelayMs = 0;

    /** @brief Canned application-frame send status (default ACK_STATE_0, acknowledged when directed). */
    ::com::rdk::hal::hdmicec::SendMessageStatus sendMessageResult =
        ::com::rdk::hal::hdmicec::SendMessageStatus::ACK_STATE_0;

    /** @brief Canned addLogicalAddresses() binder status (default ok). */
    ::android::binder::Status addLogicalAddressesBinderStatus;

    /** @brief Canned removeLogicalAddresses() binder status (default ok). */
    ::android::binder::Status removeLogicalAddressesBinderStatus;

    /** @brief Canned sendMessage() binder status, allocation polls included (default ok). */
    ::android::binder::Status sendMessageBinderStatus;

    ::std::vector<int32_t> lastAddedLogicalAddresses;
    ::std::vector<int32_t> lastRemovedLogicalAddresses;
    ::std::vector<uint8_t> lastSentMessage;

    int32_t addLogicalAddressesCallCount = 0;
    int32_t removeLogicalAddressesCallCount = 0;
    int32_t sendMessageCallCount = 0;

    /** @brief Count of every sendMessage() call, allocation polls included. */
    int32_t sendMessageTotalCallCount = 0;

    /** @brief Allocation-poll answers by address; an absent address answers ACK_STATE_1 (free). */
    ::std::map<int32_t, ::com::rdk::hal::hdmicec::SendMessageStatus> allocationPollResults;

    /** @brief Addresses allocation polls asked about, in arrival order. */
    ::std::vector<int32_t> allocationPolls;

    /** @brief Incoming binder transactions by code, counted by onTransact(). */
    ::std::map<uint32_t, int32_t> transactionCounts;

    /** @brief Addresses registered through this controller and not since removed. */
    ::std::vector<int32_t> registeredLogicalAddresses;

    /** @brief Interface version getInterfaceVersion() reports (default the frozen version). */
    int32_t interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION;

    /** @brief Interface hash getInterfaceHash() reports (default the frozen hash). */
    ::std::string interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE;
};

/**
 * @brief Test-scope fake of the com.rdk.hal.hdmicec IHdmiCec AIDL service.
 *
 * Published under the production service name, it owns the one FakeHdmiCecController open() hands
 * out and invokes the captured listener only when a trigger is called.  getLogicalAddresses()
 * reports that controller's registrations unless a test installs a result.  It derives from
 * BnHdmiCec, never IHdmiCecDefault, which cannot be published.
 *
 * @warning Test scope only - never reference this class from a production source list.
 * @see FakeHdmiCecController, registerFakeHdmiCecService()
 */
class FakeHdmiCecService : public ::com::rdk::hal::hdmicec::BnHdmiCec {
public:
    /** @brief The one state getState() reports, fixed at State::STARTED. */
    static constexpr ::com::rdk::hal::hdmicec::State DEFAULT_STATE =
        ::com::rdk::hal::hdmicec::State::STARTED;

    /**
     * @brief Releases the fake service and clears the static instance when it refers to this object.
     *
     * The binder registration is not withdrawn, because the pinned C++ service manager has no
     * removal API; a test keeps its registered fake for the process and reuses it through reset().
     *
     * @post getInstance() no longer returns this object.
     * @see setInstance(), getInstance()
     */
    virtual ~FakeHdmiCecService();

    /**
     * @brief Reports the HAL state of the fake service.
     *
     * @param [out] _aidl_return              - Receives DEFAULT_STATE; untouched on a null pointer
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always
     *
     * @warning Not consumed by the middleware, which tracks its own state.  It exists for the
     *          pure-virtual interface, and getGetStateCallCount() lets a test assert it is unused.
     */
    ::android::binder::Status getState(::com::rdk::hal::hdmicec::State* _aidl_return) override;

    /**
     * @brief Reads a property from the fake service; always reports an empty optional.
     *
     * @param [in]  property                  - Property requested; counted only
     * @param [out] _aidl_return              - Receives an empty optional; untouched on a null pointer
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always
     *
     * @warning Not consumed by the middleware: no property has a legacy counterpart.  It exists for
     *          the pure-virtual interface, and getGetPropertyCallCount() lets a test assert that.
     */
    ::android::binder::Status getProperty(::com::rdk::hal::hdmicec::Property property,
                                          ::std::optional<::com::rdk::hal::PropertyValue>* _aidl_return) override;

    /**
     * @brief Reports the logical addresses the fake HAL holds.
     *
     * @param [out] _aidl_return              - Receives the installed vector or, by default, the
     *                                          owned controller's registrations; untouched on a
     *                                          non-ok status or a null pointer
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim;
     *                                                  the adapter must treat non-ok as no address
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getGetLogicalAddressesCallCount() has advanced by one.
     * @see setLogicalAddressesResult(), setGetLogicalAddressesBinderStatus()
     */
    ::android::binder::Status getLogicalAddresses(::std::vector<int32_t>* _aidl_return) override;

    /**
     * @brief Opens a controller session on the fake HAL and captures the event listener.
     *
     * @param [in]  cecControllerListener     - Event listener the client supplies; captured
     * @param [out] _aidl_return              - Receives the owned controller, or nullptr when the
     *                                          null-controller flag is set; untouched on a non-ok
     *                                          status or a null pointer
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getOpenCallCount() has advanced and getListener() returns the listener, even when a
     *       null controller or a non-ok status is reported.
     * @see setOpenReturnsNullController(), setOpenBinderStatus(), getListener()
     */
    ::android::binder::Status open(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecControllerListener,
                                   ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController>* _aidl_return) override;

    /**
     * @brief Closes a controller session on the fake HAL, keeping the captured listener.
     *
     * @param [in]  hdmiCecController         - Controller the client is closing, captured verbatim
     * @param [out] _aidl_return              - Receives the canned result; untouched on a non-ok
     *                                          status or a null pointer
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getLastClosedController() reports the controller and getCloseCallCount() has advanced;
     *       a successful close drops the controller's registrations.  getListener() is unchanged, so a
     *       trigger fired after close still reaches the adapter.
     * @see setCloseResult(), setCloseBinderStatus()
     */
    ::android::binder::Status close(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController>& hdmiCecController,
                                    bool* _aidl_return) override;

    /**
     * @brief Registers an additional event listener; counts the call and reports true.
     *
     * @param [in]  cecEventListener          - Listener a non-controlling client offers; counted only
     * @param [out] _aidl_return              - Receives true; untouched on a null pointer
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always
     *
     * @warning Not consumed by the middleware, which receives events through the listener it passes
     *          to open(); getRegisterEventListenerCallCount() lets a test assert that.
     */
    ::android::binder::Status registerEventListener(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecEventListener,
                                                    bool* _aidl_return) override;

    /**
     * @brief Unregisters an additional event listener; counts the call and reports true.
     *
     * @param [in]  cecEventListener          - Listener being withdrawn; counted only
     * @param [out] _aidl_return              - Receives true; untouched on a null pointer
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always
     *
     * @warning Not consumed by the middleware, as for registerEventListener();
     *          getUnregisterEventListenerCallCount() lets a test assert that.
     */
    ::android::binder::Status unregisterEventListener(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecEventListener,
                                                      bool* _aidl_return) override;

    /**
     * @brief Reports the interface version this fake claims.
     *
     * @return int32_t                                - The version setInterfaceVersion() last
     *                                                  installed; IHdmiCec::VERSION by default
     *
     * @warning Effective under local (in-process) dispatch only.  Across a binder transaction the
     *          generated onTransact() answers from the compiled-in constant instead.
     * @see setInterfaceVersion(), reset(), setInterfaceHash()
     */
    int32_t getInterfaceVersion() override;

    /**
     * @brief Reports the interface hash this fake claims.
     *
     * @return std::string                            - The hash setInterfaceHash() last installed;
     *                                                  IHdmiCec::HASHVALUE by default
     *
     * @warning Effective under local (in-process) dispatch only, exactly as for
     *          getInterfaceVersion().
     * @see setInterfaceHash(), reset()
     */
    std::string getInterfaceHash() override;

    /**
     * @brief Counts one incoming binder transaction by code, then dispatches it as generated.
     *
     * @param [in]  code                      - Transaction code, TRANSACTION_* for an interface method
     * @param [in]  data                      - Marshalled request, passed through unread
     * @param [out] reply                     - Reply parcel the generated dispatch writes
     * @param [in]  flags                     - Transaction flags, passed through
     *
     * @return ::android::status_t                    - What BnHdmiCec::onTransact() returns
     *
     * @post getTransactionCounts() reports one more transaction under code.
     * @see getTransactionCounts()
     */
    ::android::status_t onTransact(uint32_t code, const ::android::Parcel& data, ::android::Parcel* reply,
                                   uint32_t flags) override;

    /**
     * @brief Returns the incoming binder transactions by code since construction or reset().
     *
     * @return ::std::map<uint32_t, int32_t>          - Count per code; a code never received is absent
     *
     * @note Transactions BBinder::transact() answers itself, PING_TRANSACTION among them, are never
     *       counted, and local (in-process) dispatch bypasses onTransact(), so nothing is counted there.
     * @see onTransact(), reset(), FakeHdmiCecController::getTransactionCounts()
     */
    ::std::map<uint32_t, int32_t> getTransactionCounts() const;

    // Canned responses for test access

    /**
     * @brief Selects the address vector getLogicalAddresses() reports.
     *
     * A whole vector, to reach three adapter arms: empty (no address), one entry (the ordinary case)
     * and more than one (log the count and use the first entry).
     *
     * @param [in] logicalAddresses           - Addresses to report
     *
     * @post Overrides the default, which reports the owned controller's registrations; reset()
     *       restores that default.
     * @see getLogicalAddresses(), FakeHdmiCecController::getRegisteredLogicalAddresses()
     */
    void setLogicalAddressesResult(const ::std::vector<int32_t>& logicalAddresses);

    /**
     * @brief Selects the boolean close() reports.
     *
     * Reaches the failed-close arm, where the adapter must still reach its own closed state first.
     *
     * @param [in] result                     - Value close() writes to its out-parameter
     *
     * @post Default is true.
     * @see close()
     */
    void setCloseResult(bool result);

    /**
     * @brief Selects whether open() reports a null controller alongside an ok status.
     *
     * Reaches the null-controller arm, which the adapter must raise as IOException.
     *
     * @param [in] returnsNull                - true to report a null controller, false for the owned one
     *
     * @post Default is false, so open() reports a valid non-null controller.
     * @see open(), getController()
     */
    void setOpenReturnsNullController(bool returnsNull);

    /**
     * @brief Installs the binder status open() returns.
     *
     * Reaches the open transport-failure arm, including an already-open service's EX_ILLEGAL_STATE.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     * @see open()
     */
    void setOpenBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status close() returns.
     *
     * Reaches the close transport-failure arm, where the adapter must still reach its closed state.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     * @see close()
     */
    void setCloseBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status getLogicalAddresses() returns.
     *
     * Reaches the failed-query arm, which the adapter must report as no address available.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     * @see getLogicalAddresses(), setLogicalAddressesResult()
     */
    void setGetLogicalAddressesBinderStatus(const ::android::binder::Status& status);

    // getState(), getProperty() and the listener registration pair have no setters: the middleware
    // never calls them, so their call counters are the only controls that matter.

    /**
     * @brief Overrides the interface hash this fake claims.
     *
     * Lets the harness publish a service the middleware must find and then refuse as incompatible.
     *
     * @param [in] hash                       - Hash string to report
     *
     * @post Default is IHdmiCec::HASHVALUE, the frozen hash; reset() restores it.
     * @warning Effective under local (in-process) dispatch only; a served fake reports the constant.
     * @see getInterfaceHash(), setInterfaceVersion(), reset()
     */
    void setInterfaceHash(std::string hash);

    /**
     * @brief Overrides the interface version this fake claims.
     *
     * Reaches the divergence trace on getInterfaceVersion(); no harness mode installs a version.
     *
     * @param [in] version                    - Interface version to report, installed unvalidated
     *
     * @post Default is IHdmiCec::VERSION, the frozen version; reset() restores it.
     * @warning Effective under local (in-process) dispatch only, exactly as for setInterfaceHash().
     * @see getInterfaceVersion(), FakeHdmiCecController::setInterfaceVersion()
     */
    void setInterfaceVersion(int32_t version);

    // Observation accessors for test access

    /**
     * @brief Returns the controller this service owns and hands out from open().
     *
     * Created with the service and never replaced, so a test may configure it before or after open().
     *
     * @return ::android::sp<FakeHdmiCecController>   - The owned controller, never null
     *
     * @see open(), FakeHdmiCecController
     */
    ::android::sp<FakeHdmiCecController> getController() const;

    /**
     * @brief Returns the event listener captured by the last open() call.
     *
     * Lets a test confirm the adapter supplied a listener before asserting on the receive path.
     *
     * @return ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> - The captured listener,
     *                                                                          null before open()
     *
     * @see open(), fireOnMessageReceived()
     */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> getListener() const;

    /**
     * @brief Returns the controller captured by the last close() call.
     *
     * The assertion target for "the session was closed with the controller open() handed out".
     *
     * @return ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> - The captured controller,
     *                                                                       null before close()
     *
     * @see close(), getController()
     */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> getLastClosedController() const;

    /**
     * @brief Returns how many times open() has been called.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see open(), reset()
     */
    int32_t getOpenCallCount() const;

    /**
     * @brief Returns how many times close() has been called.
     *
     * The assertion target for "a second close on an already-closed session never reaches the HAL".
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see close(), reset()
     */
    int32_t getCloseCallCount() const;

    /**
     * @brief Returns how many times getLogicalAddresses() has been called.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see getLogicalAddresses(), reset()
     */
    int32_t getGetLogicalAddressesCallCount() const;

    /**
     * @brief Returns how many times getState() has been called.
     *
     * Must stay zero across a full session: the adapter never consults the HAL's state machine.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see getState(), reset()
     */
    int32_t getGetStateCallCount() const;

    /**
     * @brief Returns how many times getProperty() has been called.
     *
     * Must stay zero across a full session: the adapter never reads HAL properties.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see getProperty(), reset()
     */
    int32_t getGetPropertyCallCount() const;

    /**
     * @brief Returns how many times registerEventListener() has been called.
     *
     * Must stay zero: the adapter receives events only through the listener it passes to open().
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see registerEventListener(), reset()
     */
    int32_t getRegisterEventListenerCallCount() const;

    /**
     * @brief Returns how many times unregisterEventListener() has been called.
     *
     * Must stay zero, for the same reason as getRegisterEventListenerCallCount().
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see unregisterEventListener(), reset()
     */
    int32_t getUnregisterEventListenerCallCount() const;

    /**
     * @brief Restores every canned response to its default and clears every capture and counter.
     *
     * Called from fixture set-up.  Clearing the captured listener returns the triggers to no-ops.
     *
     * @post Canned responses hold their documented defaults; captures are null or empty; counters
     *       are zero; the owned controller is not itself reset.
     * @warning Reset the controller through getController()->reset() when a case also configured it.
     * @see FakeHdmiCecController::reset()
     */
    void reset();

    // Event triggers for test access

    /**
     * @brief Delivers a received CEC message to the captured event listener.
     *
     * @param [in] message                    - Raw CEC frame bytes to deliver
     *
     * @return bool                                   - Whether the callback was invoked on a listener
     * @retval true                                   - A listener was captured and invoked
     * @retval false                                  - No listener captured, so nothing was delivered
     *
     * @pre open() must have captured a listener, otherwise this is a silent no-op.
     * @warning true means only that the captured listener was invoked, whatever Status it returned,
     *          not that a transport accepted the call or the callback completed.  No lock is held
     *          across the call, so the callback may re-enter.
     * @see open(), getListener()
     */
    bool fireOnMessageReceived(const ::std::vector<uint8_t>& message);

    /**
     * @brief Delivers a state-change notification to the captured event listener.
     *
     * @param [in] oldState                   - State being left
     * @param [in] newState                   - State being entered
     *
     * @return bool                                   - Whether the callback was invoked on a listener
     * @retval true                                   - A listener was captured and invoked
     * @retval false                                  - No listener captured, so nothing was delivered
     *
     * @pre open() must have captured a listener, otherwise this is a silent no-op.
     * @warning What true means, and locking, are as for fireOnMessageReceived().
     * @note Used in process only; the separate-process host exposes no command for this trigger.
     */
    bool fireOnStateChanged(::com::rdk::hal::hdmicec::State oldState,
                            ::com::rdk::hal::hdmicec::State newState);

    /**
     * @brief Delivers a transmit-completion notification to the captured event listener.
     *
     * @param [in] message                    - Frame the notification refers to
     * @param [in] status                     - Send status reported for it
     *
     * @return bool                                   - Whether the callback was invoked on a listener
     * @retval true                                   - A listener was captured and invoked
     * @retval false                                  - No listener captured, so nothing was delivered
     *
     * @pre open() must have captured a listener, otherwise this is a silent no-op.
     * @warning What true means, and locking, are as for fireOnMessageReceived().
     * @note Used in process only, exactly as fireOnStateChanged().
     */
    bool fireOnMessageSent(const ::std::vector<uint8_t>& message,
                           ::com::rdk::hal::hdmicec::SendMessageStatus status);

    // Static access for test access

    /**
     * @brief Returns the fake a test harness published for this process.
     *
     * @return FakeHdmiCecService*                    - The published fake, or nullptr when none was set
     *
     * @note Deliberately traces nothing: it is called too often for a per-call line to help.
     * @see setInstance(), registerFakeHdmiCecService()
     */
    static FakeHdmiCecService* getInstance();

    /**
     * @brief Records the fake a test harness published for this process.
     *
     * A single test-scope pointer, the same two-function idiom as the legacy driver double.
     *
     * @param [in] newFake                    - Fake to publish, or nullptr to clear
     *
     * @post getInstance() returns the supplied pointer.
     * @see getInstance()
     */
    static void setInstance(FakeHdmiCecService* newFake);

private:
    /** @brief Guards every canned response and capture below; never held while a callback runs. */
    mutable ::std::mutex mutex;

    /** @brief Incoming binder transactions by code, counted by onTransact(). */
    ::std::map<uint32_t, int32_t> transactionCounts;

    /** @brief The controller handed out by open(), created with this service and never replaced. */
    ::android::sp<FakeHdmiCecController> controller = ::android::sp<FakeHdmiCecController>::make();

    /** @brief Event listener captured by open() (default none, so the triggers are no-ops). */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> listener;

    /** @brief Controller captured by close() (default none). */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> lastClosedController;

    /** @brief Canned getLogicalAddresses() result (default none, so the registrations are reported). */
    ::std::optional<::std::vector<int32_t>> logicalAddressesResult;

    /** @brief Canned close() result (default true, session closed). */
    bool closeResult = true;

    /** @brief Whether open() reports a null controller (default false, a valid controller). */
    bool openReturnsNullController = false;

    /** @brief Canned open() binder status (default ok). */
    ::android::binder::Status openBinderStatus;

    /** @brief Canned close() binder status (default ok). */
    ::android::binder::Status closeBinderStatus;

    /** @brief Canned getLogicalAddresses() binder status (default ok). */
    ::android::binder::Status getLogicalAddressesBinderStatus;

    int32_t openCallCount = 0;
    int32_t closeCallCount = 0;
    int32_t getLogicalAddressesCallCount = 0;

    /** @brief getState() invocation count; expected to stay zero while the middleware drives. */
    int32_t getStateCallCount = 0;

    /** @brief getProperty() invocation count; expected to stay zero while the middleware drives. */
    int32_t getPropertyCallCount = 0;

    /** @brief registerEventListener() invocation count; expected to stay zero. */
    int32_t registerEventListenerCallCount = 0;

    /** @brief unregisterEventListener() invocation count; expected to stay zero. */
    int32_t unregisterEventListenerCallCount = 0;

    /** @brief Interface version getInterfaceVersion() reports (default the frozen version). */
    int32_t interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCec::VERSION;

    /** @brief Interface hash getInterfaceHash() reports (default the frozen hash). */
    ::std::string interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE;

    /** @brief The fake published for this process, or nullptr; cleared when that fake is destroyed. */
    static FakeHdmiCecService* instance;
};

/**
 * @brief Publishes a fake HdmiCec service under the production name, IHdmiCec::serviceName().
 *
 * @param [in] service                    - Fake to publish.  Must not be null
 *
 * @return bool                                   - Whether the service was published
 * @retval true                                   - The service manager accepted the registration
 * @retval false                                  - Null service, absent or unopenable binder node,
 *                                                  no service manager, or the name was refused
 *
 * @pre Nothing is registered under that name yet; a serving host has started its binder threadpool.
 * @post On success the middleware's own service lookup resolves to the supplied fake.
 * @warning Test scope only.  Acquiring the service manager is unbounded and can block.
 */
bool registerFakeHdmiCecService(const ::android::sp<FakeHdmiCecService>& service);

/** @} */ // End of HDMI_CEC_FAKE_AIDL_SERVICE
/** @} */ // End of HDMI_CEC_MOCKS
