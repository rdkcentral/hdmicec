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
 * @brief The L2 fixtures, scope guards and cases asserting one CCEC round trip on both back-ends.
 * @{
 */

/**
 * @file test_DualPathIntegration.cpp
 * @brief One CCEC round trip, asserted against both HDMI CEC HAL back-ends.
 *
 * Inbound (HAL -> Bus reader -> Connection -> typed MessageProcessor) and outbound
 * (MessageEncoder -> Connection::sendTo -> Bus -> HAL) frames are asserted on the legacy back-end
 * through the driver mock (invocation D) and on the AIDL back-end over real binder IPC to the
 * out-of-process fake service host (invocation E).
 * The back-end resolves once per process: DualPathSelectionTest runs on both arms, while
 * DualPathLegacyFlowTest and DualPathAidlFlowTest each skip on the arm that is not theirs, and
 * --gtest_filter=DualPath* selects the whole tier. Back-end identity is asserted by dynamic_cast;
 * the coverage runner greps the selected-path log line,
 * "Driver::getInstance : HDMI CEC HAL back-end selected : legacy" or "... : AIDL".
 *
 * @warning DualPathLegacyFlowTest confirms the legacy back-end before restoreDriverInboundRoute(),
 *          because DriverImpl::DriverReceiveCallback's static_cast is undefined behaviour on AIDL.
 * @note The AIDL close() mapping to IHdmiCec.close (B2) is pending owner confirmation, which a
 *       green run does not provide.
 * @see AIDL_HAL_MIGRATION_NOTES.md
 */


#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
/* <thread>: std::this_thread::get_id(), so an inbound case can prove the frame was delivered on a
 * thread other than the test thread. */
#include <thread>
/* <cstdlib>: std::strtol, for the host-reply parsers. */
#include <cstdlib>
/* <cstring>: std::memset and std::strerror, for the broken-pipe case. */
#include <cstring>
/* <cerrno>: errno, for the strtol() range checks in the host-reply parsers and for the EPIPE
 * check in the broken-pipe case. */
#include <cerrno>
/* POSIX signal and descriptor primitives - sigaction(), pipe2(), close(), O_CLOEXEC - used only
 * by the broken-pipe case in DualPathSelectionTest. */
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
/* Named explicitly although Connection.hpp includes it: one case cycles LibCCEC's term() and init(),
 * and two call its getLogicalAddress() and getPhysicalAddress() directly. */
#include "ccec/LibCCEC.hpp"

/* The non-installed concrete back-ends, by relative path because ccec/src is not an include root;
 * needed only as dynamic_cast targets and for the address of DriverImpl::DriverReceiveCallback. */
#include "../../../ccec/src/DriverImpl.hpp"
#include "../../../ccec/src/DriverAidlImpl.hpp"

#include "hdmi_cec_driver_mock.h"

using ::testing::_;
using ::testing::Return;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::SetArgPointee;

/* Cross-translation-unit seam, defined in tests/L2Tests/test_main.cpp: the pipe channel to the
 * out-of-process fake service host, the harness's own broken-pipe probe, and the requested mode. */

/**
 * @brief Reports whether the fake service host's control and observation channel is usable.
 *
 * True only under CEC_TEST_AIDL_MODE=remote once the host has completed its readiness handshake.
 *
 * @return bool                                   - Whether a request has somewhere to go
 * @retval true                                   - Both descriptors are open and the host answered
 *                                                  a ping during setup
 * @retval false                                  - No host was launched, the launch failed, or
 *                                                  teardown has closed the channel
 *
 * @warning A case whose assertions depend on the channel must fail rather than skip when false.
 * @see cecL2HostControlRequest()
 */
extern bool cecL2HostControlChannelIsOpen();

/**
 * @brief Sends one command to the fake service host and returns its single reply line.
 *
 * The write and the read share one deadline taken at entry, so the call never blocks indefinitely.
 *
 * @param [in]  command                   - Non-blank command, no newline or CR, e.g. "sent-count"
 * @param [out] reply                     - Receives the reply; on failure, unchanged or the rejected line
 * @param [out] failureDetail             - Receives what went wrong; untouched on success
 *
 * @return bool                                   - Whether one command was exchanged for one reply
 * @retval true                                   - reply holds the answer, "OK ..." or "ERR ..."
 * @retval false                                  - No channel, a bad command, an expired deadline,
 *                                                  an exited host or an unclassifiable reply
 *
 * @pre cecL2HostControlChannelIsOpen() reports true.
 * @warning An "ERR " reply is a successful exchange; the caller judges whether it was expected.
 */
extern bool cecL2HostControlRequest(const std::string &command, std::string &reply,
                                    std::string &failureDetail);

/**
 * @brief Proves the harness's control write reports EPIPE and its child reaping still succeeds.
 *
 * Forks a child that closes both ends of a probe pipe, writes to that reader-less pipe through the
 * harness's real writeControlCommand(), and ends the child through its real
 * terminateAndReapChildProcess(). Needs no fake service host, binder or service manager.
 *
 * @param [out] observedDiagnostic        - Receives writeControlCommand()'s sentence, or empty
 * @param [out] failureDetail             - Receives the failing step; untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - The write reported EPIPE; the child was reaped
 * @retval false                                  - A step did not hold; failureDetail names which
 *
 * @pre SIGPIPE is not at its default disposition; the seam checks and refuses otherwise.
 * @post No descriptor or child created by the call outlives it, on any path.
 */
extern bool cecL2ProveEpipeDiagnosticAndChildReaping(std::string &observedDiagnostic,
                                                     std::string &failureDetail);

/**
 * @brief Returns CEC_TEST_AIDL_MODE as the harness reads it, so this file never reads it itself.
 *
 * @return std::string                            - The raw value; empty when unset or empty
 */
extern std::string cecL2RequestedAidlMode();

/**
 * @brief Re-installs DriverImpl's own HAL receive callback on the process-global legacy mock.
 *
 * The mock keeps whatever callback was registered last, and DriverImpl::open() does not register
 * again once OPENED, so each legacy case restores the production inbound route itself.
 *
 * @param [in] mock                       - Process-global legacy HAL mock; null installs nothing
 *
 * @return None
 *
 * @pre The resolved back-end has been confirmed to be the legacy one.
 * @warning On the AIDL back-end the installed callback's static_cast<DriverImpl &> is undefined
 *          behaviour, which is why DualPathAidlFlowTest never calls this.
 * @see DualPathLegacyFlowTest::SetUp()
 */
static void restoreDriverInboundRoute(HdmiCecDriverMock *mock) {
    if (mock != nullptr) {
        mock->rxCallback = &DriverImpl::DriverReceiveCallback;
        mock->rxCallbackData = 0;
    }
}


namespace {

/* Host observation helpers: each sends one command, checks the reply shape and returns bool with a
 * failure sentence, so the calling case asserts at its own line. */

/**
 * @brief Splits an "OK <verb> <value>" reply into its value, insisting on the verb.
 *
 * A reply naming another verb is what a desynchronised channel produces, so it is refused.
 *
 * @param [in]  reply                     - Reply line, terminator already stripped
 * @param [in]  verb                      - Verb the reply must carry, e.g. "sent-count"
 * @param [out] value                     - Receives the text after the verb; may be empty
 * @param [out] failureDetail             - Receives a diagnostic on failure
 *
 * @return bool                                   - Whether the reply matched "OK <verb>"
 * @retval true                                   - value holds the field, possibly empty
 * @retval false                                  - An "ERR " line, another verb or a bad shape
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
 * Unlike a sendTo() that did not throw, this count cannot report a transmit that never arrived.
 *
 * @param [out] count                     - Receives the count; untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic on failure
 *
 * @return bool                                   - Whether a count was obtained
 * @retval true                                   - count holds the fake's sendMessage() count
 * @retval false                                  - The channel failed or the reply was not
 *                                                  "OK sent-count <n>"
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
 * The fake counts each call on entry, so a refused open() still counts as served.
 *
 * @param [in]  verb                      - "open-count" or "close-count"; others are refused unsent
 * @param [out] count                     - Receives the count; untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic on failure
 *
 * @return bool                                   - Whether a count was obtained
 * @retval true                                   - count holds the fake's count for that verb
 * @retval false                                  - An unknown verb, a channel failure, or a reply
 *                                                  that was not "OK <verb> <n>"
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
 * @brief Asks the host for the bytes of the fake controller's most recent sendMessage() frame.
 *
 * @param [out] hex                       - Receives lowercase hex, two digits per byte, empty when
 *                                          no frame has been captured; untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic on failure
 *
 * @return bool                                   - Whether the capture was obtained
 * @retval true                                   - hex holds the fake's last captured frame
 * @retval false                                  - The channel failed or the reply was not
 *                                                  "OK last-sent <hex>"
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
 * Inbound cases check this first, so "no frame arrived" cannot be met by a refused `deliver`.
 *
 * @param [out] present                   - Receives the listener state; untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic on failure
 *
 * @return bool                                   - Whether the answer was obtained
 * @retval true                                   - present holds the fake's listener state
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
 * @brief Asks the host which logical addresses are registered through the fake controller.
 *
 * Read from the fake's own registration record in the host process, so it reflects the
 * addLogicalAddresses() calls that really crossed the binder driver.
 *
 * @param [out] addresses                 - Receives the registered addresses in registration order,
 *                                          empty when none is registered.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether the list was obtained
 * @retval true                                   - addresses holds the fake's real registrations
 * @retval false                                  - The channel failed, or the reply was not a
 *                                                  well-formed "OK registered <decimal,...>"
 */
bool askHostForRegisteredAddresses(std::vector<long>& addresses, std::string& failureDetail)
{
    std::string reply;
    if (!cecL2HostControlRequest("registered", reply, failureDetail)) {
        return false;
    }

    std::string field;
    if (!parseOkReplyValue(reply, "registered", field, failureDetail)) {
        return false;
    }

    std::vector<long> parsedAddresses;
    size_t start = 0;

    while (!field.empty()) {
        const size_t comma = field.find(',', start);
        const std::string entry = field.substr(start, (comma == std::string::npos) ? std::string::npos
                                                                                   : comma - start);

        errno = 0;
        char* parseEnd = nullptr;
        const long parsed = std::strtol(entry.c_str(), &parseEnd, 10);

        if (entry.empty() || (errno != 0) || (parseEnd == nullptr) || (*parseEnd != '\0') ||
            (parsed < 0)) {
            failureDetail = "the fake service host reported its registered addresses as \"" + field +
                            "\", which is not a comma-separated list of non-negative decimals";
            return false;
        }

        parsedAddresses.push_back(parsed);

        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }

    addresses = parsedAddresses;
    return true;
}

/**
 * @brief Asks the host how many binder transactions each AIDL method of the fake has received.
 *
 * Counted in the host process as transactions arrive, so neither a cached answer nor a stray call hides.
 *
 * @param [out] counts                    - Receives "<interface>.<method>" -> count for exactly the
 *                                          16 keys the `calls` reply defines.  Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether the counts were obtained
 * @retval true                                   - counts holds the fake's per-method transaction counts
 * @retval false                                  - The channel failed, or the reply was not a
 *                                                  well-formed "OK calls <key>=<n> ..."
 */
bool askHostForCallCounts(std::map<std::string, long>& counts, std::string& failureDetail)
{
    static const char* const EXPECTED_KEYS[] = {
        "IHdmiCec.getState", "IHdmiCec.getProperty", "IHdmiCec.getLogicalAddresses",
        "IHdmiCec.open", "IHdmiCec.close", "IHdmiCec.registerEventListener",
        "IHdmiCec.unregisterEventListener", "IHdmiCec.getInterfaceVersion",
        "IHdmiCec.getInterfaceHash", "IHdmiCec.other",
        "IHdmiCecController.addLogicalAddresses", "IHdmiCecController.removeLogicalAddresses",
        "IHdmiCecController.sendMessage", "IHdmiCecController.getInterfaceVersion",
        "IHdmiCecController.getInterfaceHash", "IHdmiCecController.other",
    };
    const size_t expectedKeyCount = sizeof(EXPECTED_KEYS) / sizeof(EXPECTED_KEYS[0]);

    std::string reply;
    if (!cecL2HostControlRequest("calls", reply, failureDetail)) {
        return false;
    }

    std::string field;
    if (!parseOkReplyValue(reply, "calls", field, failureDetail)) {
        return false;
    }

    std::map<std::string, long> parsedCounts;
    size_t start = 0;

    while (start <= field.size()) {
        const size_t space = field.find(' ', start);
        const std::string token = field.substr(start, (space == std::string::npos) ? std::string::npos
                                                                                   : space - start);
        const size_t equals = token.find('=');
        const std::string key = (equals == std::string::npos) ? std::string() : token.substr(0, equals);
        const std::string value = (equals == std::string::npos) ? std::string() : token.substr(equals + 1);

        bool known = false;
        for (size_t index = 0; index < expectedKeyCount; ++index) {
            if (key == EXPECTED_KEYS[index]) {
                known = true;
                break;
            }
        }

        if (!known || (parsedCounts.count(key) != 0)) {
            failureDetail = "the fake service host answered \"calls\" with \"" + field + "\", whose token \"" +
                            token + "\" is not one of the 16 defined <interface>.<method>=<n> fields, or "
                            "repeats one";
            return false;
        }

        errno = 0;
        char* parseEnd = nullptr;
        const long parsed = std::strtol(value.c_str(), &parseEnd, 10);

        if (value.empty() || (value.find_first_not_of("0123456789") != std::string::npos) || (errno != 0) ||
            (parseEnd == nullptr) || (*parseEnd != '\0') || (parsed < 0)) {
            failureDetail = "the fake service host reported \"" + key + "\" as \"" + value +
                            "\", which is not a non-negative decimal number";
            return false;
        }

        parsedCounts[key] = parsed;

        if (space == std::string::npos) {
            break;
        }
        start = space + 1;
    }

    if (parsedCounts.size() != expectedKeyCount) {
        failureDetail = "the fake service host answered \"calls\" with \"" + field + "\", which carries " +
                        std::to_string(parsedCounts.size()) + " of the 16 defined fields";
        return false;
    }

    counts = parsedCounts;
    return true;
}

/**
 * @brief Asks the host to invoke onMessageReceived on the middleware's listener with these bytes.
 *
 * The callback is oneway, so the reply says only that it was invoked; the case asserts the rest.
 *
 * @param [in]  hex                       - Frame as lowercase hex, two digits per byte
 * @param [out] deliveredBytes            - Receives the byte count delivered; untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic on failure
 *
 * @return bool                                   - Whether the host invoked the callback
 * @retval true                                   - It did, with deliveredBytes bytes
 * @retval false                                  - The channel failed, or the host answered
 *                                                  "ERR no-listener" or "ERR bad-hex"
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
 * @brief Renders frame bytes as the lowercase hexadecimal the host channel uses.
 *
 * @param [in] bytes                      - Frame bytes; may be null only when length is zero
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
 * Per-overload counters make a frame decoded as the wrong type fail rather than pass; message
 * types not overridden here fall through to MessageProcessor's default bodies uncounted.
 *
 * @note Not thread safe: only DecodingFrameListener::notify() writes it, under that listener's
 *       lock and before the notification counter a waiter reads is published.
 * @see DecodingFrameListener
 */
class RecordingProcessor : public MessageProcessor {
public:
    /**
     * @brief Starts every counter at zero, the captured address text empty and every numeric
     *        capture at -1, meaning unset.
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
     * Overridden so that an <Image View On> misdecoded as this is caught by a counter.
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
     * The state-guard case uses it as a control distinct from the frame delivered while closed.
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
     * @brief Per-type decode counts; a case expects one at one and the other three at zero.
     */
    int imageViewOnCount;
    int textViewOnCount;
    int activeSourceCount;
    int standbyCount;

    /**
     * @brief The <Active Source> physical address in dotted form, e.g. "1.0.0.0".
     */
    std::string activeSourcePhysical;

    /**
     * @brief The four decoded <Active Source> address nibbles in wire order, -1 until decoded.
     *
     * Asserted per nibble because any address, even a corrupted one, renders as a non-empty string.
     */
    int activeSourceNibbles[4];

    /**
     * @brief The two packed <Active Source> operand bytes, re-serialized from the decoded address.
     *
     * 1.0.0.0 packs to 0x10 0x00, and both are -1 until decoded; they catch a value that decodes
     * correctly but would not re-encode to the same wire image.
     */
    int activeSourcePackedHigh;
    int activeSourcePackedLow;

    /**
     * @brief Header nibbles of the most recently decoded message, -1 before the first one.
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
     * The packed pair comes from serializing the address into a CECFrame, because CECBytes keeps
     * its bytes protected; a result other than two bytes leaves both packed members at -1.
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
 * @brief A FrameListener that decodes each frame, records the delivering thread and wakes waiters.
 *
 * Cases wait on a bounded predicate, never a sleep, so an expiry is a real "never arrived" verdict.
 * The recorded thread lets an AIDL case prove the frame was not delivered on the test thread, and a
 * decode that throws is counted because Bus::Reader::run() catches only InvalidStateException.
 *
 * @warning notify() decodes and publishes under one lock, counter last; reordering it would let a
 *          waiter read a half-written RecordingProcessor.
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
     * @brief Decodes one frame on the Bus reader thread, records that thread and wakes any waiter.
     *
     * @param [in] frame                  - Frame as the Bus reader delivered it
     *
     * @return None
     *
     * @post notifications has advanced by one, and either the processor was updated or
     *       decodeFailures has advanced by one.
     * @warning No exception may escape: Bus::Reader::run() catches only InvalidStateException.
     */
    void notify(const CECFrame& frame) const override
    {
        std::lock_guard<std::mutex> guard(mutex);

        /* (1) Decode before publishing, so no waiter can wake on a half-written processor. */
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

        /* (2) Publish, counter last; the thread recorded is the delivering Bus reader thread. */
        lastFrame = frame;
        notifyingThread = std::this_thread::get_id();
        notifications++;

        condition.notify_all();
    }

    /**
     * @brief Waits, bounded, until at least this many notifications have been delivered.
     *
     * @param [in] expected               - Notification count the caller is waiting for
     * @param [in] timeoutMs              - Upper bound on the wait, in milliseconds
     *
     * @return bool                               - Whether the count was reached within the bound
     * @retval true                               - At least expected notifications arrived, each
     *                                              with its decode complete
     * @retval false                              - The bound expired first, which is the expected
     *                                              outcome for a negative case
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
     * @brief How many delivered frames had a decode that threw; zero on every healthy delivery.
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
     * Default-constructed until a delivery, so callers first establish that a notification arrived.
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
     * Mutable because FrameListener::notify() is const; locked rather than atomic because the
     * decode and its publication form one critical section.
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
 * @brief Holds an open Connection, which it closes on every exit path after detaching its listener.
 *
 * A fatal assertion returns from the body at once and the Connection destructor removes nothing, so
 * cleanup lives here; a failed close is reported through ADD_FAILURE() rather than thrown.
 *
 * @warning Declare the listener passed to addFrameListener() before this guard so it outlives it.
 * @see Connection::close(), DecodingFrameListener
 */
class ScopedConnection {
public:
    /**
     * @brief Opens a Connection on the given logical address, named for the log.
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
     *       through this guard, or the run carries an ADD_FAILURE() recording that cleanup raised.
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
     * @brief Detaches the listener and closes the connection, at most once.
     *
     * Called by the destructor, and callable early by a case that needs the connection gone first.
     *
     * @return None
     *
     * @post The Bus holds no pointer to this connection or to the listener that was registered
     *       through this guard, or the run carries an ADD_FAILURE() recording that cleanup raised.
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
 * @brief Takes the CEC library down and guarantees it comes back up on every exit path.
 *
 * Only LibCCEC::term() leaves OPENED, and it changes state every case shares, so the restore is
 * in the destructor, which a fatal assertion cannot skip.
 *
 * @note B2: the AIDL close mapping is pending owner confirmation; a pass here does not confirm it.
 * @see DualPathAidlFlowTest::AFrameDeliveredWhileTheDriverIsNotOpenedIsRejectedByTheStateGuard
 */
class ScopedCecLibraryCycle {
public:
    /**
     * @brief Constructs a guard that has not yet taken the library down.
     *
     * Nothing process-global is touched until TakeDown() runs, so a case that returns before it
     * restores nothing.
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
     * @param [out] failureDetail         - Receives a diagnostic on failure; untouched on success
     *
     * @return bool                               - Whether the library was terminated
     * @retval true                               - The Bus is stopped and the HAL is closed
     * @retval false                              - term() raised; failureDetail carries its text
     *
     * @post On success the driver is CLOSED, so anything the HAL delivers must be rejected.
     */
    bool TakeDown(std::string& failureDetail)
    {
        try {
            LibCCEC::getInstance().term();
            down = true;
            return true;
        }
        catch (const std::exception& e) {
            // term() clears its initialized flag only after Driver::close() returns, so a raising
            // close leaves the library marked initialised and Restore() must not init() again.
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
     * @brief Brings the CEC library back up; idempotent, and a no-op unless TakeDown() succeeded.
     *
     * @param [out] failureDetail         - Receives a diagnostic on failure; untouched on success
     *
     * @return bool                               - Whether the library is up
     * @retval true                               - It is, or it never came down
     * @retval false                              - init() raised; failureDetail carries its text
     *
     * @post The driver is OPENED and the Bus is running, the baseline every other case assumes.
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
     * @brief Whether TakeDown() succeeded; Restore() is a no-op while it is false.
     *
     * A term() that raised may leave the library marked initialised, so this stays false and
     * Restore() does not call init() on a library that never came down.
     */
    bool down;
};

} // namespace


/**
 * @brief The selection fixture: which back-end the factory resolved to, and whether it is stable.
 *
 * The only fixture that runs on both arms, so it also holds the one harness guarantee that must
 * hold on both.  Identity is read by dynamic_cast against the non-installed concrete headers, once
 * in SetUp, because the selection cannot change.
 *
 * @see DualPathLegacyFlowTest, DualPathAidlFlowTest
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
        // Nothing to undo: no lock, callback, mock expectation or Connection, so this fixture is
        // safe to run first, last or alone.
    }

    /**
     * @brief The resolved singleton viewed as each concrete back-end, nullptr for the one it
     *        is not.
     *
     * SetUp caches these views once for the selection cases to read; the stability case
     * deliberately repeats Driver::getInstance() calls, then checks a fresh result against them.
     */
    DriverImpl *legacyBackEnd = nullptr;
    DriverAidlImpl *aidlBackEnd = nullptr;
};

/**
 * @brief The legacy round-trip fixture (invocation D), driven through the in-process HAL mock.
 *
 * SetUp installs the legacy inbound route for every case, so each behaves identically alone, in
 * file order or shuffled.
 *
 * @warning SetUp confirms the legacy back-end before installing the route: DriverReceiveCallback's
 *          static_cast to DriverImpl is undefined behaviour when the AIDL back-end is selected.
 * @see restoreDriverInboundRoute(), DriverImpl::DriverReceiveCallback()
 */
class DualPathLegacyFlowTest : public ::testing::Test {
protected:
    /**
     * @brief Resolves the HAL mock, clears inherited expectations, confirms the legacy arm, then
     *        installs the legacy inbound route.
     *
     * @return None
     *
     * @pre The global test environment has created the HdmiCecDriverMock singleton.
     * @post On the legacy arm the receive callback is DriverImpl::DriverReceiveCallback and no
     *       expectation from an earlier case survives.
     */
    void SetUp() override
    {
        // (1) The HAL double every case drives, created by the global environment before any
        //     fixture, so its absence is a harness failure.
        mock = HdmiCecDriverMock::getInstance();
        ASSERT_NE(mock, nullptr) << "the global test environment must have created the driver mock";

        // (2) Start from no inherited expectation: this binary shares one driver mock across every
        //     fixture, and a surviving EXPECT_CALL would be attributed to a case that never set it.
        ::testing::Mock::VerifyAndClearExpectations(mock);

        // (3) Confirm the legacy back-end before touching the mock's callback members; GTEST_SKIP
        //     returns from SetUp, so step (4) is unreachable on the AIDL arm.
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
 * @brief The AIDL round-trip fixture (invocation E), driven over real out-of-process binder IPC.
 *
 * Skips unless the AIDL back-end was selected, so one registered case count holds for D and E.
 *
 * @warning Never touch the legacy HAL mock here: no rxCallback, rxCallbackData,
 *          injectReceivedMessage(), simulateTxResult() or HdmiCec* EXPECT_CALL.
 * @see cecL2HostControlChannelIsOpen(), DualPathLegacyFlowTest
 */
class DualPathAidlFlowTest : public ::testing::Test {
protected:
    /**
     * @brief Skips on the legacy arm, then fails unless the host control channel is open.
     *
     * @return None
     *
     * @pre The fake service host was launched and reported ready before LibCCEC::init.
     * @post Every case can read what the fake received and can cause an inbound delivery.
     */
    void SetUp() override
    {
        // (1) The only skip: on the legacy arm there is no AIDL session, host process or channel,
        //     so every case below has no subject.
        if (dynamic_cast<DriverAidlImpl *>(&Driver::getInstance()) == nullptr) {
            GTEST_SKIP() << "this fixture is invocation E, which requires the AIDL back-end; the "
                            "legacy back-end was selected instead. Run with "
                            "CEC_TEST_AIDL_MODE=remote, with CEC_FAKE_AIDL_HOST_PATH naming the "
                            "fake_hdmi_cec_aidl_host binary, on a binder-capable kernel with a "
                            "running servicemanager, so that the host is published and ready "
                            "before LibCCEC::init resolves the selection";
        }

        // (2) A closed channel is a failure, not a skip: the AIDL arm implies the harness launched
        //     the host and pinged it over this channel before init.
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
// Selection: resolved back-end, stable and as requested, plus a harness check (every invocation).

/**
 * @brief The factory resolved to exactly one of the two back-ends.
 *
 * One of SetUp's two dynamic_casts must succeed and the other must fail; checking only the first
 * would pass a hierarchy in which one back-end derives from the other.  Runs on every invocation.
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
 * Asserts object identity and an unchanged dynamic type over a real process lifetime, after
 * production code has already called the factory.  No service is registered or removed here.
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
 * @brief The back-end that resolved is the one CEC_TEST_AIDL_MODE asked for.
 *
 * Makes a green invocation D or E mean the intended arm ran.  The mode comes through the harness
 * seam cecL2RequestedAidlMode(), since tests/L2Tests/test_main.cpp reads it and acts on it before
 * LibCCEC::init; an unset or empty value skips.
 */
TEST_F(DualPathSelectionTest, TheResolvedBackEndMatchesTheModeTheHarnessWasGiven)
{
    const std::string mode = cecL2RequestedAidlMode();
    if (mode.empty()) {
        GTEST_SKIP() << "CEC_TEST_AIDL_MODE is unset or empty, so no back-end was requested and "
                        "there is no request for this case to hold the outcome against; the "
                        "harness treats that as the legacy arm, and the invocation matrix sets the "
                        "variable explicitly - absent for invocation D, remote for invocation E";
    }

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
 * Asserts that SIGPIPE is not at its default disposition, that the real writeControlCommand() and
 * child reap driven by cecL2ProveEpipeDiagnosticAndChildReaping() yield a diagnostic naming the
 * command and EPIPE, and that a write to a reader-less pipe of its own fails with EPIPE.  It needs
 * no back-end, host or binder, so it runs under every invocation.
 */
TEST_F(DualPathSelectionTest,
       WriteControlCommandReportsEpipeAndTheChildIsStillReapedInsteadOfKillingTheRunner)
{
    // (1) Query SIGPIPE's disposition with a null action, so the case observes the harness's
    //     choice rather than installing its own.
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

    // (2) The real writeControlCommand() against a reader-less descriptor, then the real
    //     terminate-and-reap of the seam's own child.
    std::string observedDiagnostic;
    std::string seamFailure;

    ASSERT_TRUE(cecL2ProveEpipeDiagnosticAndChildReaping(observedDiagnostic, seamFailure))
        << "driving the harness's own control-channel write against a reader-less descriptor, and "
           "reaping the child that made it reader-less, did not hold: "
        << seamFailure
        << ". The step named there is the one to look at; every step the seam takes is necessary to "
           "the two properties this case exists to establish, and the seam leaves no descriptor and "
           "no child behind whichever one failed";

    //     The diagnostic is asserted here too, so a wrong sentence fails at this line; the
    //     command text is a two-place contract with the seam.
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

    // (3) The mechanism itself, secondary: a pipe of this case's own with its read end closed;
    //     both ends are released before any assertion, so no exit path leaks a descriptor.
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

    // Reaching this line is the rest of the evidence: under SIGPIPE's default disposition the
    // writes in steps (2) and (3) would have killed this process before anything was reported.
}


// ---------------------------------------------------------------------------------------------
// Flow A, legacy back-end: inbound HAL Rx callback to the typed process() overload (invocation D).

/**
 * @brief A directed <Image View On> injected at the legacy HAL arrives decoded, at the right
 *        listener, with the right header.
 *
 * Frame { 0x40, 0x04 }: initiator 4 (Playback Device 1), destination 0 (TV), opcode 0x04.  The
 * Connection is on logical address 0, so the filter must pass it to the typed process() overload.
 */
TEST_F(DualPathLegacyFlowTest, InboundImageViewOnReachesTheTypedProcessorThroughTheLegacyBackEnd)
{
    RecordingProcessor processor;
    DecodingFrameListener listener(processor);

    // The guard is declared after the listener so it is destroyed first, and the Bus lets go of
    // both before the listener's storage dies, on every exit path.
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
 * Frame { 0x4F, 0x82, 0x10, 0x00 }: initiator 4, broadcast, opcode 0x82, physical address 1.0.0.0.
 * The address is asserted exactly - rendered, per nibble and as the packed bytes - because a
 * non-empty check also passes 0.0.0.0, a nibble shift or a truncation.
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

    // Exact operands, in two views: the four decoded nibbles and the two packed bytes 0x10 0x00,
    // since "non-empty" would also accept 0.0.0.0, a shifted 0.1.0.0 or a truncated value.
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
 * The negative control for the two inbound cases above: frame { 0x43, 0x36 } goes to address 3
 * while the Connection is on 0.  The wait runs its full 1200-ms bound rather than checking at
 * once, so its verdict is not decided by a race with the Bus reader thread.
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
// Flow B, legacy back-end: outbound typed message to the exact bytes the HAL gets (invocation D).

/**
 * @brief <Image View On> encoded and sent reaches the legacy HAL as exactly the bytes CEC defines.
 *
 * Expected { 0x40, 0x04 }: initiator 4 (the Connection's source), destination 0 (TV), opcode 0x04.
 * The outbound leg is synchronous, so the bytes are at the HAL when sendTo returns and no wait is
 * needed.
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

    // RAII, because a fatal length/size ASSERT_* below returns from this body at once and would
    // bypass a trailing close() while the Bus still held this connection.
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
 * Expected { 0x4F, 0x82, 0x10, 0x00 }: broadcast header, opcode 0x82, physical address 1.0.0.0.
 * All four bytes are asserted, because a reordered or dropped operand is invisible to an opcode
 * check.
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
 * The mock reports HDMI_CEC_IO_SENT_FAILED and returns HDMI_CEC_IO_GENERAL_ERROR, so the throwing
 * sendTo overload must raise, which is how a caller knows the message did not go out.
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
// Logical-address registration, AIDL back-end: across the binder driver (invocation E).

/**
 * @brief Enabling the driver registered exactly one PLAYBACK_DEVICE address at the out-of-process
 *        fake, and LibCCEC reads it back with exactly one IHdmiCec.getLogicalAddresses transaction.
 *
 * Requires invocation E with the hosted fake at its defaults, so the first PLAYBACK_DEVICE candidate
 * (4) is free; every other per-method transaction count must stay unchanged across the read.
 *
 * @pre LibCCEC::init has opened the AIDL back-end against the hosted fake.
 * @see DriverAidlImpl::open(), DriverAidlImpl::getLogicalAddress()
 */
TEST_F(DualPathAidlFlowTest, EnablingTheDriverRegistersOneAddressThatLibCcecReadsBackThroughTheHal)
{
    std::string detail;

    std::vector<long> registered;
    ASSERT_TRUE(askHostForRegisteredAddresses(registered, detail)) << detail;

    ASSERT_EQ(registered.size(), 1u)
        << "the hosted fake holds " << registered.size() << " registered logical address(es) after "
           "init; enabling the driver must register exactly one";
    EXPECT_EQ(registered[0], static_cast<long>(LogicalAddress::PLAYBACK_DEVICE_1))
        << "the registered address is not the first free PLAYBACK_DEVICE candidate";

    std::map<std::string, long> callsBefore;
    ASSERT_TRUE(askHostForCallCounts(callsBefore, detail)) << detail;

    int address = -1;
    ASSERT_NO_THROW({ address = LibCCEC::getInstance().getLogicalAddress(1); })
        << "LibCCEC::getLogicalAddress raised, so the HAL reported no registered address";

    std::map<std::string, long> callsAfter;
    ASSERT_TRUE(askHostForCallCounts(callsAfter, detail)) << detail;

    EXPECT_EQ(address, static_cast<int>(LogicalAddress::PLAYBACK_DEVICE_1))
        << "LibCCEC::getLogicalAddress did not return the address registered at enable";

    for (std::map<std::string, long>::const_iterator before = callsBefore.begin(); before != callsBefore.end();
         ++before) {
        const long expected = before->second + ((before->first == "IHdmiCec.getLogicalAddresses") ? 1 : 0);
        EXPECT_EQ(expected, callsAfter[before->first])
            << "across LibCCEC::getLogicalAddress the fake's " << before->first << " count went from "
            << before->second << " to " << callsAfter[before->first] << "; the read must cross the binder "
               "driver as exactly one IHdmiCec.getLogicalAddresses transaction and nothing else";
    }
}

// ---------------------------------------------------------------------------------------------
// Flow B, AIDL back-end: outbound across the binder driver to the hosted fake (invocation E).

/**
 * @brief <Image View On> encoded and sent arrives at the out-of-process fake as exactly the bytes
 *        CEC defines, once, over real binder IPC.
 *
 * The throwing sendTo must not raise against the fake's default ACK_STATE_0 reply.  Read over the
 * host pipe, the fake's sendMessage() count must then advance by exactly one and its last frame
 * must equal the wire image { 0x40, 0x04 }, rebuilt from the encoded payload and the header.
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

    // (a) The payload alone: sendTo prepends the header to its own copy and leaves the caller's
    //     frame untouched, so reading it after the send also asserts that.
    const uint8_t* payloadBytes = nullptr;
    size_t payloadLength = 0;
    frame.getBuffer(&payloadBytes, &payloadLength);
    ASSERT_EQ(1u, payloadLength)
        << "an <Image View On> payload is a bare opcode - one byte, with no operands - so the "
           "encoder produced the wrong frame and the wire comparison below would be meaningless";
    EXPECT_EQ("04",
              toLowercaseHex(reinterpret_cast<const unsigned char*>(payloadBytes), payloadLength))
        << "the encoded payload is not the 0x04 <Image View On> opcode this case is written around";

    // (b) The wire image the service must have received, rebuilt with the same two calls sendTo
    //     makes and cross-checked against the literal CEC defines.
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
 * Wire bytes { 0x4F, 0x82, 0x10, 0x00 } catch length, copy and operand errors a two-byte frame
 * cannot.  As a broadcast other than <Report Physical Address> the default ACK_STATE_0 reply must
 * not raise, and the fake's count and captured frame are asserted as in the previous case.
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

    // (a) The payload, carrying the operands this case follows through the parcel; sendTo
    //     prepends the header to a local copy, as in the previous case.
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

    // (b) The wire image: the broadcast header, then that payload, rebuilt as in the previous case.
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
// Flow A, AIDL back-end: inbound fake-service frame to the typed process() overload (invocation E).

/**
 * @brief A frame delivered by the fake service arrives on a thread that is not the test's, and
 *        reaches the typed processor decoded and intact.
 *
 * The host's `deliver 4004` makes the fake call the middleware's listener over binder with
 * { 0x40, 0x04 }, the frame the legacy inbound case injects.  Asserted: the fake holds the
 * listener, one live session before and after, the typed overload and header nibbles, no decode
 * failure, and a notifying thread other than the test's.
 */
TEST_F(DualPathAidlFlowTest, InboundFrameFromTheFakeServiceArrivesOnABinderThreadAndReachesTheTypedProcessor)
{
    std::string detail;

    // The fake must hold the listener the middleware handed to open(), or the trigger below would
    // do nothing.
    bool listenerHeld = false;
    ASSERT_TRUE(askHostForListenerPresence(listenerHeld, detail)) << detail;
    ASSERT_TRUE(listenerHeld)
        << "the fake service is holding no event listener, so it has nothing to deliver to. The "
           "middleware passes its listener to IHdmiCec::open() during LibCCEC::init, so this means "
           "the AIDL open() never reached the service even though the AIDL back-end was selected";

    // One live session at the service: a difference rather than literals, because another case
    // cycles the library and this file must pass under --gtest_shuffle.
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

    // Declared after the listener: the guard's destructor detaches it from the Bus, so the
    // listener must outlive the guard.
    ScopedConnection scoped(LogicalAddress::TV, "L2-Aidl-FlowA-ImageViewOn");
    scoped.addFrameListener(&listener);

    long deliveredBytes = -1;
    ASSERT_TRUE(askHostToDeliverFrame("4004", deliveredBytes, detail)) << detail;
    ASSERT_EQ(2, deliveredBytes)
        << "the fake service reported delivering " << deliveredBytes << " bytes where the two bytes "
           "of { 0x40, 0x04 } were sent, so the trigger itself is wrong and nothing below would be "
           "measuring the middleware";

    // The same 3000 ms bound as the legacy inbound cases, so a difference in outcome is a
    // difference in the back-end.
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

    // Checked only after the wait: NotifyingThread() is default-constructed until a notification,
    // so this comparison would otherwise pass vacuously.
    EXPECT_NE(std::this_thread::get_id(), listener.NotifyingThread())
        << "the frame was delivered to the listener ON THE TEST'S OWN THREAD. Nothing in this process "
           "asked for it: the frame originated in the fake service's process and can only have "
           "entered this one through the binder driver, dispatched by the client threadpool and handed "
           "on by the Bus reader thread. This thread's id appearing here means the delivery was "
           "inline, which would mean the service had been resolved LOCALLY rather than as a remote "
           "proxy - the in-process case this tier exists to be distinguishable from";

    // Receiving a frame is not a session event, so neither counter may have moved during the
    // delivery.
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
 * The AIDL counterpart of the legacy delete-on-throw path: a frame the fake delivers while the
 * library is down never arrives, a <Standby> after the restore does, and each transition moves the
 * fake's open or close count by exactly one.
 *
 * @warning Declaration order is load-bearing: listener, then connection guard, then library cycle.
 * @note B2: the AIDL close mapping is pending owner confirmation; a pass here does not confirm it.
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

    // The session counters before the cycle, read rather than assumed because --gtest_shuffle may
    // run another case first; what is asserted below is a pair of deltas.
    long openBeforeCycle = -1;
    long closeBeforeCycle = -1;
    ASSERT_TRUE(askHostForSessionCount("open-count", openBeforeCycle, detail)) << detail;
    ASSERT_TRUE(askHostForSessionCount("close-count", closeBeforeCycle, detail)) << detail;
    ASSERT_EQ(1, openBeforeCycle - closeBeforeCycle)
        << "the fake service reports " << openBeforeCycle << " open() and " << closeBeforeCycle
        << " close() calls, so there is not exactly one live AIDL session to take down and the "
           "deltas asserted below would not be attributable to this case's own cycle";

    // Out of OPENED; the guard's destructor restores the library on every exit path below.
    ScopedCecLibraryCycle cycle;
    ASSERT_TRUE(cycle.TakeDown(detail)) << detail;

    // The close crossed the driver exactly once and opened nothing; this does not discharge B2,
    // the pending confirmation of the IHdmiCec::close mapping.
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

    // Restore before the negative check: only a running Bus reader could drain a wrongly queued
    // frame, so "no frame arrived" means nothing while the stack is down.
    ASSERT_TRUE(cycle.Restore(detail)) << detail;

    // The re-open reached the service exactly once and closed nothing, so the control delivery
    // below reaches a genuinely new session.
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

    // The control: a different opcode delivered after the restore must arrive, so the negative
    // above is not explained by a dead route.
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

// ---------------------------------------------------------------------------------------------
// Physical address, AIDL back-end: the fixed 1.0.0.0, no session or transmit call (invocation E).

/**
 * @brief LibCCEC::getPhysicalAddress reports 1.0.0.0 on the remote AIDL back-end, and no IHdmiCec
 *        or IHdmiCecController transaction crosses the binder driver to the fake service.
 *
 * All 16 per-method transaction counts the host reports must be unchanged across the query.
 *
 * @pre Invocation E; skips with its fixture when the legacy back-end resolved.
 * @note 0x01000000 is 1.0.0.0 as both plugins decode it, one nibble per byte.
 */
TEST_F(DualPathAidlFlowTest, LibCCECReportsTheFixedPhysicalAddressWithoutCrossingBinder)
{
    std::string detail;

    std::map<std::string, long> callsBefore;
    ASSERT_TRUE(askHostForCallCounts(callsBefore, detail)) << detail;

    unsigned int physicalAddress = 0x0F0F0F0Fu;
    ASSERT_NO_THROW(LibCCEC::getInstance().getPhysicalAddress(&physicalAddress));

    std::map<std::string, long> callsAfter;
    ASSERT_TRUE(askHostForCallCounts(callsAfter, detail)) << detail;

    EXPECT_EQ(0x01000000u, physicalAddress)
        << "LibCCEC did not report the fixed 1.0.0.0 encoding 0x01000000 on the AIDL back-end";
    EXPECT_EQ("1.0.0.0",
              PhysicalAddress((uint8_t)((physicalAddress >> 24) & 0xFF), (uint8_t)((physicalAddress >> 16) & 0xFF),
                              (uint8_t)((physicalAddress >> 8) & 0xFF), (uint8_t)(physicalAddress & 0xFF)).toString());

    for (std::map<std::string, long>::const_iterator before = callsBefore.begin(); before != callsBefore.end();
         ++before) {
        EXPECT_EQ(before->second, callsAfter[before->first])
            << "across LibCCEC::getPhysicalAddress the fake's " << before->first << " count went from "
            << before->second << " to " << callsAfter[before->first] << "; the physical-address query must "
               "make no AIDL call";
    }
}

/** @} */ // End of HDMI_CEC_L2_DUALPATH
