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
#include <mutex>
#include <optional>
#include <string>
#include <utils/StrongPointer.h>
#include <vector>

/**
 * @defgroup HDMI_CEC_MOCKS HDMI CEC Middleware Test Doubles
 * @{
 * @par Test Double Specification
 * The doubles in this directory stand in for a HAL while the CCEC middleware is under test.
 * Two transports are covered: the legacy in-process C ABI, stood in for by the GoogleMock double
 * declared in hdmi_cec_driver_mock.h, and the out-of-process com.rdk.hal.hdmicec AIDL HAL, stood in
 * for by the fake declared here.  Nothing in this directory is referenced by a production source
 * list, so nothing here can reach the shipped middleware library.
 *
 */

/**
 * @defgroup HDMI_CEC_FAKE_AIDL_SERVICE HDMI CEC Fake AIDL Service
 * @{
 * @par Fake Service Specification
 * A pair of hand-written fakes implementing the existing com.rdk.hal.hdmicec AIDL interface exactly
 * as the frozen 0.1.0.0 snapshot declares it.  No .aidl is authored here, no interface is added and
 * no method is added to an interface: both classes derive from the generated server bases and
 * implement the interface methods those bases leave pure virtual - the seven declared by IHdmiCec
 * and the three declared by IHdmiCecController.@n
 * Two further methods are overridden that the bases do not leave pure virtual, and the distinction
 * matters.  getInterfaceVersion() and getInterfaceHash() are pure virtual on the two interfaces but
 * concrete on BnHdmiCec and BnHdmiCecController, which answer them from the snapshot's compiled-in
 * version and hash; deriving from a generated base is therefore already enough to satisfy them.
 * This fake overrides that concrete pair deliberately, because overriding it is the only way to make
 * a fake report metadata the middleware has to refuse, and that in turn is the only way to reach the
 * present-but-incompatible arm of the middleware's back-end selection.  The mode in which the
 * override takes effect, and the reason a served fake cannot report divergent metadata at all, are
 * below.@n
 * The fakes are registered under the production service name published by
 * ::com::rdk::hal::hdmicec::IHdmiCec::serviceName(), which is what makes the middleware's own
 * service lookup - and therefore its runtime back-end selection - reach them.
 *
 * Two dispatch modes matter and behave differently:
 * - In-process: libbinder resolves a name registered in the calling process to the local BBinder, so
 *   interface_cast hands back this object and every call, including getInterfaceVersion() and
 *   getInterfaceHash(), dispatches virtually here.  This is the only mode in which the four settable
 *   metadata values - each class's interface hash and interface version - take effect, and of those
 *   four only the service's hash can change a selection outcome, because the middleware's
 *   compatibility check reads the service interface's metadata alone.
 * - Out-of-process: the client holds a real proxy, transactions cross the binder driver and event
 *   callbacks arrive on the client's binder threadpool.  The generated onTransact() answers the
 *   metadata transactions from the compiled-in constants, so the metadata overrides are inert here.
 *
 */

/**
 * @file fake_hdmi_cec_aidl_service.h
 *
 * @brief Declaration of the test-scope fake com.rdk.hal.hdmicec AIDL HdmiCec service.
 *
 * Declares FakeHdmiCecController, FakeHdmiCecService, the address-free trace label by which every
 * diagnostic here identifies an object, and the registration entry point that publishes the service
 * under the production service name.  Every class here is a hand-written fake with plain settable
 * canned responses and plain virtual overrides: there is deliberately no GoogleMock and no
 * GoogleTest dependency, because the implementation of this header is also compiled into the
 * separate fake-service host binary, which links only the AIDL stub and binder libraries.
 *
 * @warning Test scope only.  This header is built for test targets exclusively and must never be
 *          referenced from a production source list, from ccec/src, or from any installed header.
 *
 * @see hdmi_cec_driver_mock.h
 */

/**
 * @brief Reports one traced object as a stable, address-free label.
 *
 * The single spelling of object identity in every diagnostic this fake and its host binary print.
 * A raw pointer value answers none of the questions a reader of these lines actually has - is an
 * object held at all, is it the same one as the line above, how many of them have there been - while
 * changing on every run, so it turns a diff of two captured logs into noise and discloses the
 * process's address layout for no diagnostic gain.  A label answers all three and does neither.@n
 * Presence is reported as a word.  Identity is reported as an ordinal minted from one process-wide
 * sequence on first sight of an object and reported for that object thereafter, so "listener #1" and
 * "listener #2" tell a re-registration from a repeat, a number denotes exactly one object for the
 * life of the process whatever kind of object it is, and no two objects ever share one.
 *
 * @param [in] object                     - Object to label, or nullptr.  Used as an identity key
 *                                          only; its value is never printed and never returned
 *
 * @return ::std::string                          - The label
 * @retval "absent"                               - The object was nullptr
 * @retval "#N"                                   - N is this object's ordinal, minted on first sight
 *
 * @post A non-null object has an ordinal for the rest of the process, and a later call for the same
 *       object returns the same label.
 *
 * @warning Diagnostics only.  Nothing may parse a label or assert on one: the ordinals a run mints
 *          depend on the order in which that run traced its objects, so they are stable within a
 *          capture and deliberately not across captures.
 * @warning Identity is keyed on the address, which is never disclosed, and the registry is never
 *          pruned.  An object destroyed and another allocated at the same address are therefore
 *          reported under one ordinal.  The alternative - an ordinal carried as a member - would mean
 *          widening the generated server bases, which this fake may not do, and the objects traced
 *          here are a session's own and outlive the lines that name them.
 *
 * @note An ordinal belongs to a pointer value, so one object reached through two different base
 *       pointers of a multiply-inherited type can be minted two of them.  Every site in this fake and
 *       its host traces an object through a single static type - a listener always as
 *       ::com::rdk::hal::hdmicec::IHdmiCecEventListener, the service always as FakeHdmiCecService -
 *       which is what keeps one object under one ordinal across the lines that name it, and a new
 *       site has to do the same.
 *
 * @see FakeHdmiCecService, registerFakeHdmiCecService()
 */
::std::string fakeHdmiCecTraceLabel(const void* object);

/**
 * @brief Test-scope fake of the com.rdk.hal.hdmicec IHdmiCecController AIDL interface.
 *
 * The controller half of the fake HAL: the object a client receives from
 * FakeHdmiCecService::open() and through which it adds and removes logical addresses and transmits
 * CEC messages.@n
 * It derives from the generated server base ::com::rdk::hal::hdmicec::BnHdmiCecController, and
 * deliberately not from IHdmiCecControllerDefault, because IHdmiCecControllerDefault::onAsBinder()
 * returns nullptr: an object derived from it can never be published to the service manager nor
 * reached through a proxy, so a fake built on it would silently never be called.
 *
 * The fake is intentionally dumb.  Each method records what it was given, increments its call
 * counter and answers with the canned response the test installed.  It performs no CEC reasoning of
 * any kind: it does not inspect a frame's destination nibble, does not classify a message as
 * directed or broadcast, and does not compute a SendMessageStatus.  That translation logic belongs
 * to the middleware adapter under test, and duplicating it here would make the test assert the
 * fake's opinion instead of the adapter's behaviour.
 *
 * Every canned response has a deterministic default, stated on its setter, so a test that
 * configures nothing at all still observes a fully functional controller.
 *
 * @warning Test scope only - never reference this class from a production source list.
 *
 * @see FakeHdmiCecService
 */
class FakeHdmiCecController : public ::com::rdk::hal::hdmicec::BnHdmiCecController {
public:
    /**
     * @brief Adds logical addresses on the fake HAL.
     *
     * Captures the vector for later inspection, increments the add call counter, writes the canned
     * boolean result and returns the canned binder status.  The vector is never interpreted: its
     * width is exactly what the caller marshalled, which is the property the single-element-array
     * assertions rely on.
     *
     * @param [in]  logicalAddresses          - Addresses the client marshalled.  Captured verbatim and
     *                                          retrievable through getLastAddedLogicalAddresses()
     * @param [out] _aidl_return              - Receives the canned boolean result.  Left untouched
     *                                          when the canned binder status is non-ok, and when the
     *                                          pointer is null
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim.
     *                                                  A non-ok status the test installed is returned
     *                                                  unchanged and before the out-parameter is
     *                                                  written, and the adapter under test must map it
     *                                                  to IOException
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @pre setAddLogicalAddressesResult() and setAddLogicalAddressesBinderStatus() select the
     *      outcome; both have deterministic defaults, so neither call is required.
     * @post getAddLogicalAddressesCallCount() has advanced by one and
     *       getLastAddedLogicalAddresses() reports this call's vector, whatever the outcome.
     *
     * @see setAddLogicalAddressesResult(), setAddLogicalAddressesBinderStatus(),
     *      getLastAddedLogicalAddresses(), getAddLogicalAddressesCallCount()
     */
    ::android::binder::Status addLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                 bool* _aidl_return) override;

    /**
     * @brief Removes logical addresses on the fake HAL.
     *
     * Captures the vector, increments the remove call counter, writes the canned boolean result and
     * returns the canned binder status.
     *
     * @param [in]  logicalAddresses          - Addresses the client marshalled.  Captured verbatim and
     *                                          retrievable through getLastRemovedLogicalAddresses()
     * @param [out] _aidl_return              - Receives the canned boolean result.  Left untouched
     *                                          when the canned binder status is non-ok, and when the
     *                                          pointer is null
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim.
     *                                                  A non-ok status the test installed is returned
     *                                                  unchanged and before the out-parameter is
     *                                                  written, and the adapter under test must log it
     *                                                  and let nothing escape
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getRemoveLogicalAddressesCallCount() has advanced by one and
     *       getLastRemovedLogicalAddresses() reports this call's vector, whatever the outcome.
     *
     * @see setRemoveLogicalAddressesResult(), setRemoveLogicalAddressesBinderStatus(),
     *      getLastRemovedLogicalAddresses(), getRemoveLogicalAddressesCallCount()
     */
    ::android::binder::Status removeLogicalAddresses(const ::std::vector<int32_t>& logicalAddresses,
                                                    bool* _aidl_return) override;

    /**
     * @brief Transmits a CEC message through the fake HAL.
     *
     * Captures the byte vector, increments the send call counter, writes the canned
     * SendMessageStatus and returns the canned binder status.  The message bytes are never examined:
     * whether the frame is directed or broadcast, and what that implies about the meaning of
     * ACK_STATE_0 and ACK_STATE_1, is entirely the adapter's concern.
     *
     * @param [in]  message                   - Raw CEC frame the client marshalled.  Captured verbatim
     *                                          and retrievable through getLastSentMessage()
     * @param [out] _aidl_return              - Receives the canned SendMessageStatus.  Left untouched
     *                                          when the canned binder status is non-ok, and when the
     *                                          pointer is null
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim.
     *                                                  A non-ok status the test installed is returned
     *                                                  unchanged and before the out-parameter is
     *                                                  written, and the adapter under test must map it
     *                                                  to IOException whatever send status was also
     *                                                  installed
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getSendMessageCallCount() has advanced by one and getLastSentMessage() reports this
     *       frame, whatever the outcome.  No length limit is applied here, so a frame the adapter
     *       under test was required to reject leaves both unchanged and is thereby distinguishable
     *       from one it truncated.
     *
     * @see setSendMessageResult(), setSendMessageBinderStatus(), getLastSentMessage(),
     *      getSendMessageCallCount()
     */
    ::android::binder::Status sendMessage(const ::std::vector<uint8_t>& message,
                                          ::com::rdk::hal::hdmicec::SendMessageStatus* _aidl_return) override;

    /**
     * @brief Reports the interface version this fake claims.
     *
     * BnHdmiCecController already implements this method concretely, from the same compiled-in
     * ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION, so the interface's pure-virtual
     * declaration is satisfied without an override and this one is not required.  It is kept so that
     * both metadata methods on both fake classes answer from a member that setInterfaceVersion()
     * installs and reset() restores, which gives the pair one shape and one place to restore.@n
     * The value reported is whatever setInterfaceVersion() last installed, defaulting to the
     * compiled-in constant.  What that value can and cannot decide is stated on the setter, and it is
     * narrower than the service interface's version: the middleware's compatibility check reads the
     * service interface's metadata alone.
     *
     * @return int32_t                                - The reported interface version.  The
     *                                                  compiled-in constant until
     *                                                  setInterfaceVersion() installs another
     *
     * @warning Effective under local (in-process) dispatch only.  Across a binder transaction the
     *          generated onTransact() answers from the compiled-in constant instead.
     *
     * @see setInterfaceVersion(), reset()
     */
    int32_t getInterfaceVersion() override;

    /**
     * @brief Reports the interface hash this fake claims.
     *
     * Overrides the concrete implementation BnHdmiCecController supplies, answering the hash
     * setInterfaceHash() last installed and defaulting to the same compiled-in
     * ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE.  It is no more required than the
     * version override is, and is settable on exactly the same terms.
     *
     * @return std::string                            - The reported interface hash.  The compiled-in
     *                                                  constant until setInterfaceHash() installs
     *                                                  another
     *
     * @warning Effective under local (in-process) dispatch only, exactly as for
     *          getInterfaceVersion().
     *
     * @see setInterfaceHash(), reset()
     */
    std::string getInterfaceHash() override;

    // Canned responses for test access

    /**
     * @brief Selects the boolean addLogicalAddresses() reports.
     *
     * Exists to reach the address-unavailable arm: the AIDL HAL collapses the legacy HAL's
     * distinction between "logical address unavailable" and "general error" into one boolean, and
     * false is the value the adapter under test must translate into
     * AddressNotAvailableException - the caller-visible difference the migration registers and
     * exercises through both real Sink call paths.
     *
     * @param [in] result                     - Value addLogicalAddresses() writes to its out-parameter
     *
     * @post Default is true, so an unconfigured fake reports a successful address acquisition.
     *
     * @see addLogicalAddresses()
     */
    void setAddLogicalAddressesResult(bool result);

    /**
     * @brief Selects the boolean removeLogicalAddresses() reports.
     *
     * Exists to reach the ignored-failure arm: the legacy back-end discards the HAL's removal return
     * value and returns silently, so the adapter under test must log a false result and let nothing
     * escape.  Raising there would be an unregistered behaviour difference, and false is how a test
     * proves it does not.
     *
     * @param [in] result                     - Value removeLogicalAddresses() writes to its out-parameter
     *
     * @post Default is true.
     *
     * @see removeLogicalAddresses()
     */
    void setRemoveLogicalAddressesResult(bool result);

    /**
     * @brief Makes addLogicalAddresses() take at least @p delayMs milliseconds to answer.
     *
     * Exists to reach the middleware's slow-HAL-call diagnostic, which is a THRESHOLD and not a
     * timeout: DriverAidlImpl brackets every synchronous AIDL call with a monotonic clock read and
     * emits one LOG_WARN line when the call outlives its threshold, abandoning nothing and raising
     * nothing.  That warning arm cannot be reached by any canned result or status, because none of
     * them takes time; only a call that is genuinely slow reaches it.  A test that asserted the
     * warning without making a call slow would be asserting nothing.
     *
     * addLogicalAddresses() rather than another method because it is the shortest complete round
     * trip a test can drive through the public Driver interface, so the delay is paid once and no
     * session state changes around it.
     *
     * @param [in] delayMs                    - Milliseconds to sleep before answering.  Zero or
     *                                          negative disables the delay
     *
     * @post Default is 0, so every other case pays nothing.  Cleared by reset().
     *
     * @warning The sleep is taken with NO LOCK HELD, deliberately: the instance mutex guards the
     *          canned responses and the captures, and holding it across a sleep would stall a
     *          concurrent capture read on a remote fake answering on binder threads.  The delay
     *          value is read under the lock and the lock is dropped before sleeping.
     * @warning Real wall-clock time, so a caller pays it in test duration.  Keep it just above the
     *          middleware's threshold rather than comfortably above it.
     *
     * @see addLogicalAddresses()
     */
    void setAddLogicalAddressesDelayMs(int32_t delayMs);

    /**
     * @brief Selects the SendMessageStatus sendMessage() reports.
     *
     * Exists to reach every arm of the adapter's status translation, whose sense inverts between a
     * directed and a broadcast destination: ACK_STATE_0 means acknowledged for a directed message but
     * rejected for a broadcast, ACK_STATE_1 is the mirror of that, and BUSY means arbitration failed
     * and nothing was sent.  The fake reports the value verbatim and forms no opinion about which
     * reading applies.
     *
     * @param [in] status                     - Value sendMessage() writes to its out-parameter
     *
     * @post Default is ::com::rdk::hal::hdmicec::SendMessageStatus::ACK_STATE_0, which for a directed
     *       message is the acknowledged case.
     *
     * @see sendMessage()
     */
    void setSendMessageResult(::com::rdk::hal::hdmicec::SendMessageStatus status);

    /**
     * @brief Installs the binder status addLogicalAddresses() returns.
     *
     * Exists to reach the transport-failure arm: a non-ok binder status must be translated by the
     * adapter into IOException.  Independent of the statuses installed for the other two methods, so
     * a test can fail one call without disturbing the rest.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     *
     * @see addLogicalAddresses()
     */
    void setAddLogicalAddressesBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status removeLogicalAddresses() returns.
     *
     * Exists to reach the removal transport-failure arm, which - unlike the add case - the adapter
     * must log and swallow rather than raise, mirroring the legacy back-end's silent return.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     *
     * @see removeLogicalAddresses()
     */
    void setRemoveLogicalAddressesBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status sendMessage() returns.
     *
     * Exists to reach the transmit transport-failure arm, which the adapter must translate into
     * IOException regardless of any SendMessageStatus value.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     *
     * @see sendMessage()
     */
    void setSendMessageBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Overrides the interface hash this fake controller claims.
     *
     * The controller half of the metadata pair, and its reach is narrower than the service's, which
     * is the first thing to know about it.  The middleware's compatibility check reads the service
     * interface's metadata alone - IHdmiCec - so a hash installed here decides no selection outcome
     * and must not be read as a second route to the present-but-incompatible arm; what it does is
     * make the value this class reports observable and changeable, so the divergence trace on
     * getInterfaceHash() is reached by a test rather than left as an unreachable defensive branch.
     * The parameter is an arbitrary string, exactly as on the service's setter, because no value is
     * privileged here.
     *
     * @param [in] hash                       - Hash string to report
     *
     * @post Default is ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE, the real frozen hash
     *       compiled into the snapshot, so an unconfigured controller reports the compatible value.
     *       reset() restores that default, so an override cannot leak into the next case.
     *
     * @warning Effective under local (in-process) dispatch only.  A remotely served fake cannot
     *          report divergent metadata at all, because the generated onTransact() answers the
     *          metadata transactions from the compiled-in constants, so this setter has no effect on
     *          the out-of-process invocation and must not be judged redundant on the evidence of a
     *          remote run ignoring it.
     *
     * @see getInterfaceHash(), setInterfaceVersion(), reset(), FakeHdmiCecService::setInterfaceHash()
     */
    void setInterfaceHash(std::string hash);

    /**
     * @brief Overrides the interface version this fake controller claims.
     *
     * The version half of the same pair, on the same terms and with the same reach: the compatibility
     * check never reads it, so it decides no selection outcome, and its job is to make the divergence
     * trace on getInterfaceVersion() reachable and reached.  No validation is applied - any int32_t
     * is installed as given - because the whole point of the control is to report a value the
     * snapshot would not.
     *
     * @param [in] version                    - Interface version to report
     *
     * @post Default is ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION, the version compiled
     *       into the snapshot, so an unconfigured controller reports the compatible value.  reset()
     *       restores that default.
     *
     * @warning Effective under local (in-process) dispatch only, exactly as for setInterfaceHash().
     *
     * @see getInterfaceVersion(), setInterfaceHash(), reset(),
     *      FakeHdmiCecService::setInterfaceVersion()
     */
    void setInterfaceVersion(int32_t version);

    // Observation accessors for test access

    /**
     * @brief Returns the address vector the last addLogicalAddresses() call carried.
     *
     * The assertion target for single-element array marshalling: the middleware's address operations
     * are single-valued, so a correct adapter marshals exactly one entry carrying exactly the
     * requested address.  Without this capture that assertion cannot be written at all.
     *
     * @return ::std::vector<int32_t>                 - The captured vector, empty if never called
     *
     * @see addLogicalAddresses()
     */
    ::std::vector<int32_t> getLastAddedLogicalAddresses() const;

    /**
     * @brief Returns the address vector the last removeLogicalAddresses() call carried.
     *
     * The removal-side counterpart of getLastAddedLogicalAddresses(), asserting the same
     * single-element marshalling contract.
     *
     * @return ::std::vector<int32_t>                 - The captured vector, empty if never called
     *
     * @see removeLogicalAddresses()
     */
    ::std::vector<int32_t> getLastRemovedLogicalAddresses() const;

    /**
     * @brief Returns the message bytes the last sendMessage() call carried.
     *
     * The assertion target for frame marshalling and for the frame-length boundary cases, where a
     * frame at the contract limit must arrive byte for byte and an over-length frame must never
     * arrive at all - truncation would silently put a corrupt CEC frame on the bus, so the test
     * distinguishes "rejected" from "trimmed" by reading this capture.
     *
     * @return ::std::vector<uint8_t>                 - The captured frame, empty if never called
     *
     * @see sendMessage(), getSendMessageCallCount()
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
     * @brief Returns how many times sendMessage() has been called.
     *
     * The assertion target for "the adapter rejected this frame without transmitting": a guard that
     * fires before the HAL call leaves this counter unchanged, which is the only way to tell a
     * rejection apart from a failed transmit.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see sendMessage(), reset()
     */
    int32_t getSendMessageCallCount() const;

    /**
     * @brief Restores every canned response to its default and clears every capture and counter.
     *
     * Called from a fixture's set-up so that one long-lived registered fake - the service resolves
     * once per process, so the fake cannot be re-registered per case - does not leak configuration or
     * observations from one case into the next.
     *
     * @post Canned responses hold their documented defaults; captures are empty; counters are zero.
     *
     * @see FakeHdmiCecService::reset()
     */
    void reset();

private:
    /** @brief Guards every canned response and capture below; all critical sections are short and
     *         hold no lock across a callback, because a remote fake answers on binder threads while a
     *         test thread may be reading a capture or installing a canned response. */
    mutable ::std::mutex mutex;

    /** @brief Canned addLogicalAddresses() result.  Default: address acquired. */
    bool addLogicalAddressesResult = true;

    /** @brief Canned removeLogicalAddresses() result.  Default: address removed. */
    bool removeLogicalAddressesResult = true;

    /** @brief Milliseconds addLogicalAddresses() sleeps before answering, with no lock held.
     *         Default: 0, so no case pays for it unless it asks. */
    int32_t addLogicalAddressesDelayMs = 0;

    /** @brief Canned sendMessage() status.  Default: ACK_STATE_0, acknowledged for a directed frame. */
    ::com::rdk::hal::hdmicec::SendMessageStatus sendMessageResult =
        ::com::rdk::hal::hdmicec::SendMessageStatus::ACK_STATE_0;

    /** @brief Canned addLogicalAddresses() binder status.  Default: ok. */
    ::android::binder::Status addLogicalAddressesBinderStatus;

    /** @brief Canned removeLogicalAddresses() binder status.  Default: ok. */
    ::android::binder::Status removeLogicalAddressesBinderStatus;

    /** @brief Canned sendMessage() binder status.  Default: ok. */
    ::android::binder::Status sendMessageBinderStatus;

    ::std::vector<int32_t> lastAddedLogicalAddresses;
    ::std::vector<int32_t> lastRemovedLogicalAddresses;
    ::std::vector<uint8_t> lastSentMessage;

    int32_t addLogicalAddressesCallCount = 0;
    int32_t removeLogicalAddressesCallCount = 0;
    int32_t sendMessageCallCount = 0;

    /** @brief Interface version getInterfaceVersion() reports.  Written by this initialiser, by
     *         setInterfaceVersion() and by reset(); it is a member rather than a literal in the
     *         getter so that reset() has one place to restore and the getter has one value to
     *         return.  Default: the frozen version. */
    int32_t interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCecController::VERSION;

    /** @brief Interface hash getInterfaceHash() reports.  Written by this initialiser, by
     *         setInterfaceHash() and by reset(), exactly as the version is.  Default: the frozen
     *         hash. */
    ::std::string interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCecController::HASHVALUE;
};

/**
 * @brief Test-scope fake of the com.rdk.hal.hdmicec IHdmiCec AIDL service.
 *
 * The service half of the fake HAL: the object published under the production service name, found by
 * the middleware's own service lookup, and from which a client obtains a controller session.@n
 * It derives from the generated server base ::com::rdk::hal::hdmicec::BnHdmiCec, and deliberately not
 * from IHdmiCecDefault, because IHdmiCecDefault::onAsBinder() returns nullptr and an object derived
 * from it cannot be published to the service manager at all.
 *
 * Like its controller, this fake is dumb by design: it records what it was given, counts its calls
 * and answers with canned responses.  It runs no state machine of its own - the middleware tracks its
 * own open and closed states, and a second source of truth here would let a test pass against the
 * fake's opinion rather than the adapter's behaviour.
 *
 * The service owns a FakeHdmiCecController, hands it out from open() and never replaces it, so a test
 * may configure the controller before or after the middleware opens the session.  It also captures the
 * event listener passed to open() and invokes callbacks on it only when a trigger is called
 * explicitly; nothing here fires spontaneously.
 *
 * All seven interface methods are declared because the generated interface declares them pure
 * virtual.  Four of them - getState(), getProperty(), registerEventListener() and
 * unregisterEventListener() - are deliberately not consumed by the middleware, and their call
 * counters exist so a test can assert exactly that.
 *
 * @warning Test scope only - never reference this class from a production source list.
 *
 * @see FakeHdmiCecController, registerFakeHdmiCecService()
 */
class FakeHdmiCecService : public ::com::rdk::hal::hdmicec::BnHdmiCec {
public:
    /** @brief Default logical address reported by getLogicalAddresses().  Default: Playback device. */
    static constexpr int32_t DEFAULT_LOGICAL_ADDRESS = 4;

    /**
     * @brief The one state getState() reports, fixed rather than settable.
     *
     * A started service, which is the only state consistent with a fake that is published and
     * answering.  It is a constant and not a canned response because the middleware never calls
     * getState(), so a setter for it could not change any behaviour under test.@n
     * Note that this two-valued AIDL enum is a different thing from the middleware's own closed,
     * closing and opened machine, and says nothing about it.
     *
     * @see getState()
     */
    static constexpr ::com::rdk::hal::hdmicec::State DEFAULT_STATE =
        ::com::rdk::hal::hdmicec::State::STARTED;

    /**
     * @brief Releases the fake service.
     *
     * Clears the static instance pointer when it still refers to this object, so a destroyed fake can
     * never be handed out by getInstance().  The published binder registration is not withdrawn,
     * because the pinned C++ service manager exposes no service-removal API; a test therefore keeps
     * its registered fake alive for the life of the process and reuses it through reset().
     *
     * @post getInstance() no longer returns this object.
     *
     * @see setInstance(), getInstance()
     */
    virtual ~FakeHdmiCecService();

    /**
     * @brief Reports the HAL state of the fake service.
     *
     * @param [out] _aidl_return              - Receives DEFAULT_STATE, the one state this fake
     *                                          reports.  Left untouched when the pointer is null
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always; this method has no failure arm to
     *                                                  configure, because nothing under test calls it
     *
     * @post getGetStateCallCount() has advanced by one, which is what makes the expectation below
     *       assertable.
     *
     * @warning The middleware deliberately does not consume this method: it tracks its own closed,
     *          closing and opened states, and consulting the HAL's would create a second source of
     *          truth.  The method exists here to satisfy the pure-virtual interface, it answers a
     *          fixed state with a fixed status, and its call counter exists so a test can assert the
     *          adapter never calls it.
     *
     * @see DEFAULT_STATE, getGetStateCallCount()
     */
    ::android::binder::Status getState(::com::rdk::hal::hdmicec::State* _aidl_return) override;

    /**
     * @brief Reads a property from the fake service.
     *
     * Always reports an empty optional, which is a valid "property not available" answer, because no
     * property this interface publishes has a legacy counterpart and therefore none is consumed.
     *
     * @param [in]  property                  - Property requested.  Recorded only as a call count
     * @param [out] _aidl_return              - Receives an empty optional.  Left untouched when the
     *                                          pointer is null
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always; this method has no failure arm to
     *                                                  configure, because nothing under test calls it
     *
     * @post getGetPropertyCallCount() has advanced by one.
     *
     * @warning The middleware deliberately does not consume this method: reading the HAL's CEC version
     *          or its transmit metrics would be new behaviour with no legacy counterpart.  It exists
     *          here to satisfy the pure-virtual interface, and its call counter exists so a test can
     *          assert the adapter never calls it.
     *
     * @see getGetPropertyCallCount()
     */
    ::android::binder::Status getProperty(::com::rdk::hal::hdmicec::Property property,
                                          ::std::optional<::com::rdk::hal::PropertyValue>* _aidl_return) override;

    /**
     * @brief Reports the logical addresses the fake HAL holds.
     *
     * @param [out] _aidl_return              - Receives a copy of the canned address vector.  Left
     *                                          untouched when the canned binder status is non-ok, and
     *                                          when the pointer is null
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim.
     *                                                  A non-ok status the test installed is returned
     *                                                  unchanged and before the out-parameter is
     *                                                  written, and the adapter under test must report
     *                                                  it as no address available
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getGetLogicalAddressesCallCount() has advanced by one.
     *
     * @see setLogicalAddressesResult(), setGetLogicalAddressesBinderStatus(),
     *      getGetLogicalAddressesCallCount()
     */
    ::android::binder::Status getLogicalAddresses(::std::vector<int32_t>* _aidl_return) override;

    /**
     * @brief Opens a controller session on the fake HAL and captures the event listener.
     *
     * Captures the listener - which is what makes the three triggers deliver - and writes the owned
     * controller to the out-parameter, or nullptr when the null-controller flag is set.
     *
     * @param [in]  cecControllerListener     - Event listener the client supplies.  Captured and
     *                                          retrievable through getListener().  The parameter name
     *                                          is the generated one; its type is
     *                                          IHdmiCecEventListener
     * @param [out] _aidl_return              - Receives the owned controller, or nullptr when the
     *                                          null-controller flag is set.  Left untouched when the
     *                                          canned binder status is non-ok, and when the pointer is
     *                                          null
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim.
     *                                                  A non-ok status the test installed is returned
     *                                                  unchanged and before the out-parameter is
     *                                                  written; this is also how the EX_ILLEGAL_STATE
     *                                                  the interface documents for an already-open
     *                                                  service is expressed, the fake running no state
     *                                                  machine that could raise it by itself
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getListener() returns the supplied listener and getOpenCallCount() has advanced by one.
     *       The listener is captured even when a null controller or a non-ok status is reported, so the
     *       receive path can be exercised against a session the adapter rejected, and the triggers
     *       stop being no-ops from this point on.
     *
     * @see setOpenReturnsNullController(), setOpenBinderStatus(), getListener(), getController(),
     *      getOpenCallCount()
     */
    ::android::binder::Status open(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecControllerListener,
                                   ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController>* _aidl_return) override;

    /**
     * @brief Closes a controller session on the fake HAL.
     *
     * Captures the controller it was handed, writes the canned boolean result and returns the canned
     * binder status.  The captured listener is left in place, so a trigger fired after a close still
     * reaches the adapter's listener - which is exactly what the "callback arriving during or after a
     * close is rejected by the state guard" case needs.
     *
     * @param [in]  hdmiCecController         - Controller the client is closing.  Captured and
     *                                          retrievable through getLastClosedController()
     * @param [out] _aidl_return              - Receives the canned boolean result.  Left untouched
     *                                          when the canned binder status is non-ok, and when the
     *                                          pointer is null
     *
     * @return ::android::binder::Status              - The canned binder status, returned verbatim.
     *                                                  A non-ok status the test installed is returned
     *                                                  unchanged and before the out-parameter is
     *                                                  written, and the adapter under test must still
     *                                                  reach its own closed state before raising
     * @retval ok                                     - Default, or whatever ok status was installed
     *
     * @post getLastClosedController() reports the supplied controller, getCloseCallCount() has
     *       advanced by one, and getListener() still reports the listener captured by open().
     *
     * @see setCloseResult(), setCloseBinderStatus(), getLastClosedController(), getCloseCallCount()
     */
    ::android::binder::Status close(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController>& hdmiCecController,
                                    bool* _aidl_return) override;

    /**
     * @brief Registers an additional event listener on the fake HAL.
     *
     * Counts the call and reports true; the listener is not retained, because this fake delivers
     * events only to the listener captured by open().
     *
     * @param [in]  cecEventListener          - Listener offered by a non-controlling client.  Counted
     *                                          only
     * @param [out] _aidl_return              - Receives true.  Left untouched when the pointer is null
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always; this method has no failure arm to
     *                                                  configure, because nothing under test calls it
     *
     * @post getRegisterEventListenerCallCount() has advanced by one.
     *
     * @warning The middleware deliberately does not consume this method: it is the controlling client
     *          and receives events through the listener it passes to open().  This method exists here
     *          to satisfy the pure-virtual interface, and its call counter exists so a test can assert
     *          the adapter never calls it.
     *
     * @see getRegisterEventListenerCallCount()
     */
    ::android::binder::Status registerEventListener(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecEventListener,
                                                    bool* _aidl_return) override;

    /**
     * @brief Unregisters an additional event listener on the fake HAL.
     *
     * Counts the call and reports true.
     *
     * @param [in]  cecEventListener          - Listener being withdrawn.  Counted only
     * @param [out] _aidl_return              - Receives true.  Left untouched when the pointer is null
     *
     * @return ::android::binder::Status              - Status
     * @retval ok                                     - Always; this method has no failure arm to
     *                                                  configure, because nothing under test calls it
     *
     * @post getUnregisterEventListenerCallCount() has advanced by one.
     *
     * @warning The middleware deliberately does not consume this method, for the same reason as
     *          registerEventListener().  Its call counter exists so a test can assert that.
     *
     * @see getUnregisterEventListenerCallCount()
     */
    ::android::binder::Status unregisterEventListener(const ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener>& cecEventListener,
                                                      bool* _aidl_return) override;

    /**
     * @brief Reports the interface version this fake claims.
     *
     * BnHdmiCec already implements this method concretely, from the same compiled-in
     * ::com::rdk::hal::hdmicec::IHdmiCec::VERSION, so the override is not what satisfies the
     * interface's pure-virtual declaration; it exists so that the version is answered from a member
     * alongside the hash, both installable and both restored together.@n
     * The value reported is whatever setInterfaceVersion() last installed, defaulting to the
     * compiled-in constant.  Which arms of the middleware's compatibility check this control can and
     * cannot reach is stated on that setter: the harness's incompatible mode drives the hash, and the
     * version arms of the check are covered at unit level by locally constructed doubles.
     *
     * @return int32_t                                - The reported interface version.  The
     *                                                  compiled-in constant until
     *                                                  setInterfaceVersion() installs another
     *
     * @warning Effective under local (in-process) dispatch only.  Across a binder transaction the
     *          generated onTransact() answers from the compiled-in constant instead.
     *
     * @see setInterfaceVersion(), reset(), setInterfaceHash()
     */
    int32_t getInterfaceVersion() override;

    /**
     * @brief Reports the interface hash this fake claims.
     *
     * Answers the hash the harness installed, which defaults to the compiled-in
     * ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE.  This is the metadata answer the suite has the
     * strongest reason to divert - it is what the harness's incompatible mode installs - and
     * overriding the concrete BnHdmiCec implementation is the only way to divert it.
     *
     * @return std::string                            - The reported interface hash
     *
     * @warning Effective under local (in-process) dispatch only, exactly as for
     *          getInterfaceVersion().
     *
     * @see setInterfaceHash(), reset()
     */
    std::string getInterfaceHash() override;

    // Canned responses for test access

    /**
     * @brief Selects the address vector getLogicalAddresses() reports.
     *
     * Exists to reach three distinct adapter arms, which is why the parameter is a whole vector rather
     * than a single address:
     * - empty, where the adapter must report no address, matching the legacy back-end whose query
     *   leaves its zero-initialised local untouched;
     * - exactly one entry, the ordinary case;
     * - more than one entry, where the adapter must log the count and operate on the first entry only,
     *   adding no multi-address state, no iteration and no dispatch fan-out.
     *
     * @param [in] logicalAddresses           - Addresses to report
     *
     * @post Default is a one-entry vector holding DEFAULT_LOGICAL_ADDRESS.
     *
     * @see getLogicalAddresses(), DEFAULT_LOGICAL_ADDRESS
     */
    void setLogicalAddressesResult(const ::std::vector<int32_t>& logicalAddresses);

    /**
     * @brief Selects the boolean close() reports.
     *
     * Exists to reach the failed-close arm, where the adapter must still complete its own transition
     * to closed before raising, so a subsequent open is not blocked by a half-closed session.
     *
     * @param [in] result                     - Value close() writes to its out-parameter
     *
     * @post Default is true.
     *
     * @see close()
     */
    void setCloseResult(bool result);

    /**
     * @brief Selects whether open() reports a null controller alongside an ok status.
     *
     * Exists to reach the null-controller arm: an ok status with no controller is a HAL that answered
     * successfully and returned nothing usable, and the adapter must raise IOException rather than
     * store a null session and fail later on first use.  The combination is only producible by
     * asking for it here.
     *
     * @param [in] returnsNull                - true to report a null controller, false to report the
     *                                          owned controller
     *
     * @post Default is false, so open() reports a valid non-null controller.
     *
     * @see open(), getController()
     */
    void setOpenReturnsNullController(bool returnsNull);

    /**
     * @brief Installs the binder status open() returns.
     *
     * Exists to reach the open transport-failure arm, which the adapter must translate into
     * IOException.  This is also where the EX_ILLEGAL_STATE the interface documents for an
     * already-open service is expressed, since the fake runs no state machine that could raise it by
     * itself.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     *
     * @see open()
     */
    void setOpenBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status close() returns.
     *
     * Exists to reach the close transport-failure arm, where the adapter must still reach its own
     * closed state before raising.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     *
     * @see close()
     */
    void setCloseBinderStatus(const ::android::binder::Status& status);

    /**
     * @brief Installs the binder status getLogicalAddresses() returns.
     *
     * Exists to reach the failed-query arm, which the adapter must report as no address available -
     * the same observable outcome as a successful but empty query, distinguished in the log rather
     * than in the return value.
     *
     * @param [in] status                     - Status to return, ok or non-ok
     *
     * @post Default is an ok status.
     *
     * @see getLogicalAddresses(), setLogicalAddressesResult()
     */
    void setGetLogicalAddressesBinderStatus(const ::android::binder::Status& status);

    /*
     * There is deliberately no response or status setter for getState(), getProperty(),
     * registerEventListener() or unregisterEventListener().  The middleware never calls any of the
     * four, so a control that varied what they answer could not change any behaviour under test: its
     * only effect would be to suggest coverage that does not exist.  What those four methods do carry
     * is an invocation counter each, because "the adapter never called this" is a real assertion and
     * a counter that must stay zero is how it is written.
     */

    /**
     * @brief Overrides the interface hash this fake claims.
     *
     * The metadata control with a production-path consumer, and it exists for one job: to publish a
     * service the middleware must find and then refuse, so that the factory-level fallback from a
     * present but incompatible service can be observed.  The parameter is an arbitrary string,
     * because the value that produces that outcome belongs to the harness rather than to this fake.
     *
     * @param [in] hash                       - Hash string to report
     *
     * @post Default is ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE, the real frozen hash compiled
     *       into the snapshot, so an unconfigured fake is the compatible case.  reset() restores that
     *       default, so an override cannot leak into the next case.
     *
     * @warning Effective under local (in-process) dispatch only.  A remotely served fake cannot report
     *          divergent metadata at all, because the generated onTransact() answers the metadata
     *          transactions from the compiled-in constants, so this setter has no effect on the
     *          out-of-process invocation and must not be judged redundant on the evidence of a remote
     *          run ignoring it.
     *
     * @note The one consumer that changes a selection outcome is the L1 harness in
     *       tests/L1Tests/test_main.cpp, whose `incompatible` mode installs the broken hash "-1" here
     *       before the middleware's selection resolves, which is what makes the factory-level
     *       fallback observable.  The other rejection arms - the empty hash, the "notfrozen"
     *       development hash and every version arm - are covered at unit level by locally constructed
     *       doubles in tests/L1Tests/ccec/test_DriverAidl.cpp, precisely because a served Bn* object
     *       cannot report bad metadata; nothing reaches those arms through this setter, and this note
     *       must not be widened to claim otherwise.  The same file additionally drives this setter and
     *       the three below it directly, on locally constructed and never registered fakes, which is
     *       what makes each getter's divergence trace a reached branch rather than a reachable one.
     *
     * @see getInterfaceHash(), setInterfaceVersion(), reset()
     */
    void setInterfaceHash(std::string hash);

    /**
     * @brief Overrides the interface version this fake claims.
     *
     * The version half of the service's metadata pair, and its reach is deliberately smaller than the
     * hash's: it decides no selection outcome under test.  The middleware's compatibility check does
     * read this interface's version - the check reads the service interface's metadata and nothing
     * else - but every version arm of it is exercised at unit level by locally constructed doubles
     * rather than through the registered fake, so no harness mode installs a version here.  What this
     * control does is make the value this class reports observable and changeable, which is what makes
     * the divergence trace on getInterfaceVersion() a branch a test reaches rather than a defensive
     * one nothing can drive.  No validation is applied - any int32_t is installed as given - because
     * the whole point of the control is to report a version the snapshot would not.
     *
     * @param [in] version                    - Interface version to report
     *
     * @post Default is ::com::rdk::hal::hdmicec::IHdmiCec::VERSION, the version compiled into the
     *       snapshot, so an unconfigured fake is the compatible case.  reset() restores that default,
     *       so an override cannot leak into the next case.
     *
     * @warning Effective under local (in-process) dispatch only, exactly as for setInterfaceHash().
     *
     * @see getInterfaceVersion(), setInterfaceHash(), reset(),
     *      FakeHdmiCecController::setInterfaceVersion()
     */
    void setInterfaceVersion(int32_t version);

    // Observation accessors for test access

    /**
     * @brief Returns the controller this service owns and hands out from open().
     *
     * The route by which a test configures the controller's canned responses and reads its captures,
     * whether or not a session has been opened yet: the controller is created with the service and
     * never replaced, so a reference taken before open() stays valid afterwards.
     *
     * @return ::android::sp<FakeHdmiCecController>   - The owned controller, never null
     *
     * @see open(), FakeHdmiCecController
     */
    ::android::sp<FakeHdmiCecController> getController() const;

    /**
     * @brief Returns the event listener captured by the last open() call.
     *
     * Lets a test confirm the adapter actually supplied a listener before asserting anything about
     * the receive path, so a silent no-op trigger is not mistaken for a delivery failure.
     *
     * @return ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> - The captured listener,
     *                                                                          null if open() has not
     *                                                                          been called
     *
     * @see open(), fireOnMessageReceived()
     */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> getListener() const;

    /**
     * @brief Returns the controller captured by the last close() call.
     *
     * The assertion target for "the session was closed with the controller that open() handed out",
     * which is what proves the adapter kept the two paired rather than closing something else.  A
     * driver that closed a null or a stale controller would still receive this fake's canned result,
     * so without this capture that mistake is invisible.
     *
     * @return ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> - The captured controller,
     *                                                                       null if close() has not
     *                                                                       been called
     *
     * @note Consumed by the close-contract cases of the AIDL back-end's L1 suite, which assert this
     *       equals the exact controller open() reported, on the successful close, the false-result
     *       close and the non-ok-status close alike.  It is a live control with a named consumer, not
     *       a capture kept for its own sake.
     *
     * @see close(), getController(), setCloseResult(), setCloseBinderStatus()
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
     * The assertion target for "a second close on an already-closed session never reaches the HAL",
     * which the adapter's own guard is required to prevent.
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
     * The assertion target for "the adapter never consults the HAL's state machine": this counter must
     * stay zero across a full open, transmit, receive and close cycle.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see getState(), reset()
     */
    int32_t getGetStateCallCount() const;

    /**
     * @brief Returns how many times getProperty() has been called.
     *
     * The assertion target for "the adapter never reads HAL properties": this counter must stay zero
     * across a full session, since no property has a legacy counterpart.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see getProperty(), reset()
     */
    int32_t getGetPropertyCallCount() const;

    /**
     * @brief Returns how many times registerEventListener() has been called.
     *
     * The assertion target for "the adapter receives events through the listener it passed to open()":
     * this counter must stay zero, because the middleware is the controlling client and never
     * registers a diagnostic listener.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see registerEventListener(), reset()
     */
    int32_t getRegisterEventListenerCallCount() const;

    /**
     * @brief Returns how many times unregisterEventListener() has been called.
     *
     * The assertion target for the same expectation as getRegisterEventListenerCallCount(): this
     * counter must stay zero.
     *
     * @return int32_t                                - Call count since construction or the last reset()
     *
     * @see unregisterEventListener(), reset()
     */
    int32_t getUnregisterEventListenerCallCount() const;

    /**
     * @brief Restores every canned response to its default and clears every capture and counter.
     *
     * Called from a fixture's set-up so that one long-lived registered fake - the middleware resolves
     * its back-end once per process, so the fake cannot be re-registered per case - does not leak
     * configuration or observations from one case into the next.  The captured listener is cleared
     * too, which returns the triggers to their pre-open no-op behaviour.
     *
     * @post Canned responses hold their documented defaults; captures are null or empty; counters are
     *       zero; the owned controller is unchanged and is not itself reset.
     *
     * @warning This resets the service only.  Reset the controller through
     *          getController()->reset() when a case also configured it.
     *
     * @see FakeHdmiCecController::reset()
     */
    void reset();

    // Event triggers for test access

    /**
     * @brief Delivers a received CEC message to the captured event listener.
     *
     * The entry point for the receive path: the caller supplies the exact bytes, so the fake forms no
     * frame of its own.  Out of process this call crosses the binder driver and the listener runs on
     * the client's binder threadpool, which is the only arrangement in which the client's threadpool
     * is genuinely exercised.
     *
     * @param [in] message                    - Raw CEC frame bytes to deliver
     *
     * @return bool                                   - Whether the callback was invoked on a listener
     * @retval true                                   - A listener was captured and the callback was
     *                                                  invoked on it
     * @retval false                                  - No listener captured, so nothing was delivered
     *
     * @pre open() must have captured a listener, otherwise this is a silent no-op.
     *
     * @warning What true establishes depends on where the captured listener lives, and the two cases
     *          must not be conflated.  A local listener is reached by direct virtual dispatch, so true
     *          means the callback ran to completion and the status it reported is the callback's own.
     *          A remote listener is reached through a proxy, and IHdmiCecEventListener is declared
     *          oneway, so true means only that the transaction was submitted and accepted by the
     *          driver: the remote callback need not have run yet, and its own outcome is not carried
     *          back on this path at all.  Completion against a remote listener is established
     *          independently, by observing what the middleware did in response - this fake's own
     *          counters and captures, which the separate host's observation commands read through its
     *          accessors.
     * @warning Fired before open(), or after reset() cleared the captured listener, this does nothing
     *          and reports false.  A test that ignores the return value cannot tell a genuine delivery
     *          from a no-op.
     * @warning No lock of this fake is held across the invocation - the captured listener is copied out
     *          first - so a callback may re-enter this fake without deadlocking.  A callback that does
     *          re-enter observes whatever state the fake holds at that moment.
     *
     * @see open(), getListener()
     */
    bool fireOnMessageReceived(const ::std::vector<uint8_t>& message);

    /**
     * @brief Delivers a state-change notification to the captured event listener.
     *
     * Covers the diagnostic callback the adapter is required to log and act on in no other way:
     * an in-process HAL cannot vanish, so there is no legacy counterpart to a state transition and
     * reacting to one would be new behaviour.
     *
     * @param [in] oldState                   - State being left
     * @param [in] newState                   - State being entered
     *
     * @return bool                                   - Whether the callback was invoked on a listener
     * @retval true                                   - A listener was captured and the callback was
     *                                                  invoked on it
     * @retval false                                  - No listener captured, so nothing was delivered
     *
     * @pre open() must have captured a listener, otherwise this is a silent no-op.
     *
     * @warning What true establishes depends on where the captured listener lives, exactly as it does
     *          for fireOnMessageReceived(): completion for a local listener, submission and acceptance
     *          only for a remote one.
     * @warning Fired before open(), or after reset(), this does nothing and reports false.
     * @warning No lock of this fake is held across the invocation, exactly as for
     *          fireOnMessageReceived().
     *
     * @note Called in process by
     *       DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession, which
     *       reads what the adapter logged for it.  The separate-process host deliberately exposes no
     *       command for this trigger, because that assertion is already made here and an IPC variant
     *       of it would prove nothing further.
     *
     * @see open()
     */
    bool fireOnStateChanged(::com::rdk::hal::hdmicec::State oldState,
                            ::com::rdk::hal::hdmicec::State newState);

    /**
     * @brief Delivers a transmit-completion notification to the captured event listener.
     *
     * Covers the second diagnostic callback the adapter is required to log and act on in no other way:
     * synchronous transmit already returns its own status, so nothing about the outcome depends on
     * this notification arriving.
     *
     * @param [in] message                    - Frame the notification refers to
     * @param [in] status                     - Send status reported for it
     *
     * @return bool                                   - Whether the callback was invoked on a listener
     * @retval true                                   - A listener was captured and the callback was
     *                                                  invoked on it
     * @retval false                                  - No listener captured, so nothing was delivered
     *
     * @pre open() must have captured a listener, otherwise this is a silent no-op.
     *
     * @warning What true establishes depends on where the captured listener lives, exactly as it does
     *          for fireOnMessageReceived().
     * @warning Fired before open(), or after reset(), this does nothing and reports false.
     * @warning No lock of this fake is held across the invocation, exactly as for
     *          fireOnMessageReceived().
     *
     * @note Called in process by the same case as fireOnStateChanged(), and exposed by no host
     *       command for the same reason.
     *
     * @see open(), fireOnStateChanged()
     */
    bool fireOnMessageSent(const ::std::vector<uint8_t>& message,
                           ::com::rdk::hal::hdmicec::SendMessageStatus status);

    // Static access for test access

    /**
     * @brief Returns the fake a test harness published for this process.
     *
     * The route by which a test reaches the registered fake in order to configure it, given that the
     * harness registers the fake before initialising the middleware and the test bodies run later.
     *
     * @return FakeHdmiCecService*                    - The published fake, or nullptr when none was set
     *
     * @note This accessor deliberately traces nothing, recorded here so that nobody restores a trace
     *       on the assumption it was overlooked.  It is called from the harness, from the fixtures and
     *       from the fake's own paths, so a line per call would flood every captured log and dilute the
     *       lines a case actually asserts on, while reporting only what setInstance() already reported
     *       once.  A trace behind a quiet log level would be the same flood one configuration change
     *       away.
     *
     * @see setInstance(), registerFakeHdmiCecService()
     */
    static FakeHdmiCecService* getInstance();

    /**
     * @brief Records the fake a test harness published for this process.
     *
     * Called by the harness right after it constructs and registers the fake, and called with nullptr
     * when it tears it down.  This is not a service registry and not a factory: it is a single
     * pointer, at test scope, following the same two-function idiom the legacy driver double in this
     * directory already uses.
     *
     * @param [in] newFake                    - Fake to publish, or nullptr to clear
     *
     * @post getInstance() returns the supplied pointer.
     *
     * @see getInstance()
     */
    static void setInstance(FakeHdmiCecService* newFake);

private:
    /** @brief Guards every canned response and capture below; all critical sections are short, and the
     *         listener is copied out before a trigger invokes it so no callback runs under this lock.
     *         Out of process the interface methods run on binder threads while a test thread may be
     *         reading a capture or installing a canned response. */
    mutable ::std::mutex mutex;

    /** @brief The controller handed out by open(), created with this service and never replaced. */
    ::android::sp<FakeHdmiCecController> controller = ::android::sp<FakeHdmiCecController>::make();

    /** @brief Event listener captured by open().  Default: none, so the triggers are no-ops. */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecEventListener> listener;

    /** @brief Controller captured by close().  Default: none. */
    ::android::sp<::com::rdk::hal::hdmicec::IHdmiCecController> lastClosedController;

    /** @brief Canned getLogicalAddresses() result.  Default: one entry, DEFAULT_LOGICAL_ADDRESS. */
    ::std::vector<int32_t> logicalAddressesResult { DEFAULT_LOGICAL_ADDRESS };

    /** @brief Canned close() result.  Default: session closed. */
    bool closeResult = true;

    /** @brief Whether open() reports a null controller.  Default: false, a valid controller. */
    bool openReturnsNullController = false;

    /** @brief Canned open() binder status.  Default: ok. */
    ::android::binder::Status openBinderStatus;

    /** @brief Canned close() binder status.  Default: ok. */
    ::android::binder::Status closeBinderStatus;

    /** @brief Canned getLogicalAddresses() binder status.  Default: ok. */
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

    /** @brief Interface version getInterfaceVersion() reports.  Default: the compiled-in frozen
     *         version; setInterfaceVersion() is the one control that changes it, and reset() restores
     *         it. */
    int32_t interfaceVersionResult = ::com::rdk::hal::hdmicec::IHdmiCec::VERSION;

    /** @brief Interface hash getInterfaceHash() reports.  Default: the compiled-in frozen hash;
     *         setInterfaceHash() is the one control that changes it, and reset() restores it. */
    ::std::string interfaceHashResult = ::com::rdk::hal::hdmicec::IHdmiCec::HASHVALUE;

    /** @brief The fake published for this process, or nullptr.  Cleared by the destructor. */
    static FakeHdmiCecService* instance;
};

/**
 * @brief Publishes a fake HdmiCec service under the production service name.
 *
 * Registration is an explicit callable step rather than something the constructor does, because the
 * two callers need different things around it.  An in-process harness registers and stops there,
 * since a locally registered name resolves to this very object and no transaction crosses the driver.
 * The separate host binary needs a thread to serve real transactions, so it starts its service-side
 * binder threadpool first, then calls this function to publish the fake, and signals readiness to its
 * parent only after this function has reported success.@n
 * That order is load-bearing rather than incidental.  Publication makes the name resolvable
 * immediately, so a pool started after it leaves a window in which a client can look the name up and
 * transact against a service with no thread to serve it - a race the parent can win, and one that
 * presents as a hung or failed transaction rather than as a startup ordering mistake.  Folding a
 * threadpool into registration would both force one on the in-process caller and fix the order the
 * wrong way round.
 *
 * The name comes from ::com::rdk::hal::hdmicec::IHdmiCec::serviceName(), never from a literal, so
 * there is exactly one spelling of it in the build and this fake cannot drift from the name the
 * middleware looks up.  Only the service is published; a client obtains its controller from the
 * out-parameter of open(), so the controller is never registered separately.
 *
 * @param [in] service                    - Fake to publish.  Must not be null
 *
 * @return bool                                   - Whether the service was published
 * @retval true                                   - The service manager accepted the registration
 * @retval false                                  - The service was null, the binder driver node was
 *                                                  absent or unopenable, no service manager could be
 *                                                  obtained, or the service manager refused the name
 *
 * @pre Nothing is already registered under the production name.  The pinned C++ service manager
 *      exposes no removal API, so a stale registration would decide the outcome of a run; the callers
 *      establish this before they call, and this function does not check it.
 * @post On success the middleware's own service lookup resolves to the supplied fake, which is what
 *       makes its runtime back-end selection choose the AIDL path.
 *
 * @warning Test scope only.  Nothing in a production source list may call this.
 * @warning This function's own guarantees are narrow, and no more than these should be read into a
 *          false return.  It reports false without touching libbinder at all when the supplied
 *          service is null, or when the binder driver node is absent or unopenable - the check that
 *          keeps a host with no kernel binder support from losing its whole run to the fatal
 *          driver-open path inside the linked libbinder.  It reports false when the service manager
 *          it obtained is null, and when that service manager refuses the name.  It does not bound
 *          the acquisition of the service manager itself: obtaining one retries until binder handle 0
 *          resolves, so a node that opens while no service manager is running blocks inside libbinder
 *          instead of returning here, and the pinned library's remaining failure modes are not all
 *          reducible to a return value.  The bound for those cases is the parent harness's readiness
 *          timeout, which fails the run when a host does not report itself ready in time.
 *
 * @see FakeHdmiCecService, FakeHdmiCecService::setInstance()
 */
bool registerFakeHdmiCecService(const ::android::sp<FakeHdmiCecService>& service);

/** @} */ // End of HDMI_CEC_FAKE_AIDL_SERVICE
/** @} */ // End of HDMI_CEC_MOCKS
