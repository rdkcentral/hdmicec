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
 * @defgroup HDMI_CEC_FAKE_AIDL_SERVICE_HOST HDMI CEC Fake AIDL Service Host
 * @ingroup HDMI_CEC_FAKE_AIDL_SERVICE
 * @{
 * @par Fake Service Host Specification
 * A standalone program that hosts the fake com.rdk.hal.hdmicec service in its own process, so the
 * middleware holds a real binder proxy and receives callbacks on a binder thread.  A fake
 * registered inside the test runner resolves to the local BBinder and crosses no transport.
 */

/**
 * @file fake_hdmi_cec_aidl_service_host.cpp
 *
 * @brief Separate-process host for the test-scope fake com.rdk.hal.hdmicec AIDL HdmiCec service.
 *
 * Publishes the fake under the production service name, starts the service-side binder threadpool,
 * writes the readiness token, then serves the optional control and observation channel until a
 * `shutdown` command, end of file or SIGTERM/SIGINT.  `CEC_FAKE_HOST_READY_FD` names the readiness
 * descriptor (standard output when unset); `CEC_FAKE_HOST_CONTROL_FD` and
 * `CEC_FAKE_HOST_OBSERVE_FD` name the channel pair, which answers each command with one reply line.
 *
 * @warning The parent must bound its readiness wait: with no service manager running, this blocks.
 * @note Test scope only: a noinst program that never reads `CEC_TEST_AIDL_MODE`.
 * @see fake_hdmi_cec_aidl_service.h, registerFakeHdmiCecService(), AIDL_HAL_MIGRATION_NOTES.md
 */

#include "fake_hdmi_cec_aidl_service.h"

#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <utils/String16.h>
#include <utils/StrongPointer.h>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <poll.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

// Untrusted-value rendering for diagnostics: one of five copies of one contract that must stay
// identical (the clauses and the other copies are listed in AIDL_HAL_MIGRATION_NOTES.md).

/** @brief How many rendered characters a diagnostic will carry before it is truncated. */
static const std::size_t RENDER_LIMIT = 200;

/**
 * @brief Renders an untrusted byte string as one bounded, escaped, quoted token.
 *
 * Escapes `\\` first, then `\n`, `\r`, `\t`, and every other byte outside 0x20..0x7E as `\xNN`;
 * truncates at RENDER_LIMIT and appends "...[truncated, N bytes total]".
 *
 * @param [in] value                      - Bytes to render; any content is acceptable
 * @param [in] length                     - Number of bytes in value
 *
 * @return std::string                            - The rendering: quoted, single-line, bounded
 * @warning Apply it to the value only, never to a whole message.
 */
static std::string renderUntrustedValue(const char *value, std::size_t length)
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

        // The backslash arm comes first so every escape added below stays unambiguous.
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
 * @param [in] value                      - String to render; any content is acceptable
 *
 * @return std::string                            - The rendering: quoted, single-line, bounded
 * @see renderUntrustedValue(const char *, std::size_t)
 */
static inline std::string renderUntrustedValue(const std::string &value)
{
    return renderUntrustedValue(value.data(), value.size());
}

/**
 * @brief renderUntrustedValue() for a C string that may be null.
 *
 * A null pointer renders as the undelimited `<unset>`, so "unset" and "empty" stay distinct.
 *
 * @param [in] value                      - C string to render, or null
 *
 * @return std::string                            - `<unset>` for null, else the quoted rendering
 * @see renderUntrustedValue(const char *, std::size_t)
 */
static inline std::string renderUntrustedValue(const char *value)
{
    if (value == nullptr) {
        return std::string("<unset>");
    }
    return renderUntrustedValue(value, std::strlen(value));
}

/**
 * @brief Prefix every diagnostic line this program prints carries.
 *
 * It separates this host's lines from the fake's own `[Fake...]` lines in an interleaved capture.
 */
static const char TRACE_PREFIX[] = "[FakeHdmiCecAidlHost] ";

/**
 * @brief The one readiness token, spelled exactly once.
 *
 * The parent matches this line verbatim, and the trailing newline is part of it.
 */
static const char READINESS_TOKEN[] = "FAKE_HDMI_CEC_AIDL_HOST_READY\n";

/**
 * @brief Environment variable naming the inherited descriptor the readiness line is written to.
 *
 * Unset means no parent is listening and the token goes to standard output.  Set to anything but a
 * plain descriptor number is a hard failure; see resolveReadinessFd().
 */
static const char READY_FD_VARIABLE[] = "CEC_FAKE_HOST_READY_FD";

/**
 * @brief Environment variable naming the inherited descriptor commands are read from.
 *
 * Travels with OBSERVE_FD_VARIABLE: both unset means no channel, and anything other than a usable
 * pair is a hard failure; see resolveControlChannelFds().
 */
static const char CONTROL_FD_VARIABLE[] = "CEC_FAKE_HOST_CONTROL_FD";

/**
 * @brief Environment variable naming the inherited descriptor replies are written to.
 *
 * Only reply lines, one per command, are ever written to it; diagnostics go to standard output.
 */
static const char OBSERVE_FD_VARIABLE[] = "CEC_FAKE_HOST_OBSERVE_FD";

/**
 * @brief Longest command line this program will assemble, in bytes, excluding the terminator.
 *
 * Far above any real command, so only a client that has lost its framing reaches it.
 *
 * @see serveControlChannel()
 */
static const size_t MAX_COMMAND_LINE_LENGTH = 4096;

/** @brief Bytes read from the control descriptor per read() call; a partial line is buffered. */
static const size_t CONTROL_READ_CHUNK = 512;

/**
 * @brief Milliseconds one call to writeReplyLine() may spend delivering a reply, in total.
 *
 * A whole-call budget on CLOCK_MONOTONIC, so a client that stops reading costs one deadline and an
 * EXIT_CONTROL_CHANNEL_FAILED exit rather than a hang.
 *
 * @see writeReplyLine()
 */
static const int OBSERVE_WRITE_TIMEOUT_MS = 5000;

/**
 * @brief Longest reply line this program will write, in bytes, including its terminator.
 *
 * PIPE_BUF is the size up to which a pipe write is atomic, so a reply is never delivered in pieces;
 * a longer line is refused and ends the session.
 *
 * @see writeReplyLine(), MAX_COMMAND_LINE_LENGTH
 */
static const size_t MAX_REPLY_LINE_LENGTH = PIPE_BUF;

/**
 * @brief Binder driver node checked before anything in this process touches libbinder.
 *
 * The linked libbinder aborts when it cannot open its driver, so checking first turns that into a
 * traced failure with its own exit code.  This is not the middleware's bounded preflight.
 */
static const char BINDER_DRIVER_PATH[] = "/dev/binder";

/**
 * @brief Exit code reported when something is already published under the production service name.
 */
static const int EXIT_STALE_REGISTRATION = 2;

/**
 * @brief Exit code reported when CEC_FAKE_HOST_READY_FD is not a plain descriptor number.
 *
 * A value that parses but is not writable exits EXIT_READINESS_WRITE_FAILED instead.
 *
 * @see resolveReadinessFd(), EXIT_READINESS_WRITE_FAILED
 */
static const int EXIT_BAD_READY_FD = 3;

/**
 * @brief Exit code reported when the service manager refused to publish the fake.
 */
static const int EXIT_REGISTRATION_FAILED = 4;

/**
 * @brief Exit code reported when the readiness line could not be written.
 *
 * Also covers a readiness descriptor that parsed but is not open for writing.  The fake is already
 * published when this is reached.
 *
 * @see resolveReadinessFd(), writeAllRetryingOnInterrupt()
 */
static const int EXIT_READINESS_WRITE_FAILED = 5;

/**
 * @brief Exit code reported when no usable binder driver node is present.
 */
static const int EXIT_NO_BINDER_TRANSPORT = 6;

/**
 * @brief Exit code reported when this program could not set itself up.
 *
 * Covers the self-pipe, the signal handlers, constructing the fake, and an unreachable service
 * manager: each means the host never became able to serve.
 */
static const int EXIT_SETUP_FAILED = 7;

/**
 * @brief Exit code reported when the control and observation channel is configured wrongly.
 *
 * One variable without the other, a value that is not a descriptor number, one descriptor named for
 * both, or a descriptor not open in the direction needed.  No readiness token is written.
 *
 * @see resolveControlChannelFds()
 */
static const int EXIT_BAD_CONTROL_CHANNEL = 8;

/**
 * @brief Exit code reported when the channel failed while it was being served.
 *
 * poll() or read() failed, or a reply missed its deadline or exceeded MAX_REPLY_LINE_LENGTH.  A
 * parent closing its ends is a clean shutdown, not this case.
 *
 * @see serveControlChannel(), writeReplyLine()
 */
static const int EXIT_CONTROL_CHANNEL_FAILED = 9;

// Signal-handler state; the objects the handler touches are volatile sig_atomic_t.  EXIT_FAILURE
// is never returned: every failure class above has an exit code of its own.

/** @brief Signal number that asked this program to stop, or 0 while none has. */
static volatile std::sig_atomic_t g_shutdownSignalNumber = 0;

/** @brief Write end of the self-pipe, or -1 before it exists; written by main, read by the handler. */
static volatile std::sig_atomic_t g_shutdownPipeWriteFd = -1;

/** @brief Read end of the self-pipe, or -1 before it exists; never touched by the handler. */
static int g_shutdownPipeReadFd = -1;

extern "C" {

/**
 * @brief Records a termination request and wakes the waiting main thread.
 *
 * Stores the signal number and writes one byte to the self-pipe, which ends either
 * waitForShutdownSignal() or the poll() in serveControlChannel(); errno is preserved.
 *
 * @param [in] signalNumber               - Signal being delivered, recorded for the exit trace
 *
 * @warning Async-signal-safe by construction: add no other call, tracing included.
 * @see waitForShutdownSignal(), serveControlChannel(), installShutdownHandlers()
 */
static void handleShutdownSignal(int signalNumber)
{
    const int savedErrno = errno;

    g_shutdownSignalNumber = signalNumber;

    const int writeFd = g_shutdownPipeWriteFd;
    if (writeFd >= 0) {
        const unsigned char wakeByte = 1;
        ssize_t written;
        do {
            written = ::write(writeFd, &wakeByte, sizeof(wakeByte));
        } while (written < 0 && errno == EINTR);
    }

    errno = savedErrno;
}

} // extern "C"

/**
 * @brief Creates the self-pipe and installs the termination handlers.
 *
 * Runs first and handles SIGTERM and SIGINT, so a signal at any later point still exits cleanly.
 * Neither handler sets SA_RESTART: interrupted waits are retried explicitly.
 *
 * @return bool                                   - Whether the shutdown path was established
 * @retval true                                   - Self-pipe created and both handlers installed
 * @retval false                                  - The pipe or a handler could not be installed
 *
 * @see handleShutdownSignal(), waitForShutdownSignal(), serveControlChannel()
 */
static bool installShutdownHandlers()
{
    int selfPipe[2] = { -1, -1 };
    if (::pipe(selfPipe) != 0) {
        std::cout << TRACE_PREFIX << "Could not create the shutdown self-pipe: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    g_shutdownPipeReadFd = selfPipe[0];
    g_shutdownPipeWriteFd = selfPipe[1];

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = handleShutdownSignal;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    const int handledSignals[] = { SIGTERM, SIGINT };
    for (size_t index = 0; index < sizeof(handledSignals) / sizeof(handledSignals[0]); ++index) {
        if (::sigaction(handledSignals[index], &action, nullptr) != 0) {
            std::cout << TRACE_PREFIX << "Could not install a handler for signal "
                      << handledSignals[index] << ": " << std::strerror(errno) << std::endl;
            return false;
        }
    }

    std::cout << TRACE_PREFIX << "Shutdown path ready: self-pipe read fd " << g_shutdownPipeReadFd
              << ", write fd " << static_cast<int>(g_shutdownPipeWriteFd)
              << ", handling SIGTERM and SIGINT" << std::endl;
    return true;
}

/**
 * @brief Parses an environment value that is supposed to be a file descriptor number.
 *
 * Accepts only an unadorned run of decimal digits in [0, INT_MAX]; an empty value, whitespace, a
 * sign, a trailing character or an out-of-range value is rejected rather than coerced.
 *
 * @param [in]  rawValue                  - Environment value to parse; null is rejected
 * @param [out] descriptor                - Receives the parsed number.  Untouched on failure
 *
 * @return bool                                   - Whether a descriptor number was parsed
 * @retval true                                   - descriptor holds a value in [0, INT_MAX]
 * @retval false                                  - The value was not a plain descriptor number
 * @note Parsing only; isUsableDescriptor() checks openness and direction.
 * @see resolveReadinessFd(), resolveControlChannelFds(), isUsableDescriptor()
 */
static bool parseDescriptorNumber(const char *rawValue, int &descriptor)
{
    if (rawValue == nullptr || rawValue[0] == '\0') {
        return false;
    }

    const bool startsWithDigit = (rawValue[0] >= '0' && rawValue[0] <= '9');

    errno = 0;
    char *parseEnd = nullptr;
    const long parsedFd = std::strtol(rawValue, &parseEnd, 10);

    if (!startsWithDigit || errno != 0 || parseEnd == rawValue || *parseEnd != '\0' ||
        parsedFd < 0 || parsedFd > INT_MAX) {
        return false;
    }

    descriptor = static_cast<int>(parsedFd);
    return true;
}

/**
 * @brief Reports whether a descriptor is open in this process and usable in the direction needed.
 *
 * Reads the access mode with F_GETFL, which consumes nothing and cannot block, so a descriptor lost
 * to O_CLOEXEC or a crossed-over pipe end is caught before the session starts.
 *
 * @param [in] descriptor                 - Descriptor number to test
 * @param [in] mustBeWritable             - true to require writing, false to require reading
 *
 * @return bool                                   - Whether the descriptor is open and usable
 * @retval true                                   - Open, in an access mode allowing that direction
 * @retval false                                  - Not open here, or open in the wrong direction
 * @see parseDescriptorNumber(), resolveControlChannelFds()
 */
static bool isUsableDescriptor(int descriptor, bool mustBeWritable)
{
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0) {
        return false;
    }

    const int accessMode = flags & O_ACCMODE;

    if (mustBeWritable) {
        return (accessMode == O_WRONLY || accessMode == O_RDWR);
    }

    return (accessMode == O_RDONLY || accessMode == O_RDWR);
}

/**
 * @brief Determines which file descriptor the readiness line is written to.
 *
 * Unset falls back to standard output for a standalone run.  Set but not a plain descriptor number
 * fails at once, since a parent waiting on a pipe would otherwise time out unexplained.
 *
 * @param [out] readinessFd               - Receives the descriptor.  Untouched on failure
 *
 * @return bool                                   - Whether a descriptor was resolved
 * @retval true                                   - readinessFd holds the number or STDOUT_FILENO
 * @retval false                                  - The variable was set but not a descriptor number
 *
 * @warning Only the spelling is checked; the readiness write establishes that it is writable.
 * @see parseDescriptorNumber(), writeAllRetryingOnInterrupt(), READY_FD_VARIABLE
 */
static bool resolveReadinessFd(int &readinessFd)
{
    const char *const rawValue = ::getenv(READY_FD_VARIABLE);

    if (rawValue == nullptr) {
        readinessFd = STDOUT_FILENO;
        std::cout << TRACE_PREFIX << READY_FD_VARIABLE << " is unset, so no parent is listening on a "
                     "pipe; the readiness token will go to standard output (fd " << STDOUT_FILENO
                  << ") for a standalone run" << std::endl;
        return true;
    }

    if (rawValue[0] == '\0') {
        std::cout << TRACE_PREFIX << READY_FD_VARIABLE << " is set to an empty value. Refusing to "
                     "guess: a parent that asked to be signalled on a pipe would wait out its whole "
                     "timeout if this host answered on standard output instead" << std::endl;
        return false;
    }

    // The strict parser is shared with the channel variables; see parseDescriptorNumber().
    int parsedFd = -1;
    if (!parseDescriptorNumber(rawValue, parsedFd)) {
        std::cout << TRACE_PREFIX << READY_FD_VARIABLE << " is set to "
                  << renderUntrustedValue(rawValue)
                  << ", which is not a usable non-negative file descriptor number" << std::endl;
        return false;
    }

    readinessFd = parsedFd;
    std::cout << TRACE_PREFIX << "The readiness token will be written to inherited fd "
              << readinessFd << ", named by " << READY_FD_VARIABLE << std::endl;
    return true;
}

/**
 * @brief Determines whether a control and observation channel was supplied, and validates it.
 *
 * Both unset means no channel; otherwise they must name two distinct descriptors open in the right
 * direction, or the pair is rejected before anything is published.  Outputs untouched on failure.
 *
 * @param [out] controlFd                 - Receives the command descriptor, or -1 for no channel
 * @param [out] observeFd                 - Receives the reply descriptor, or -1 for no channel
 * @param [out] channelEnabled            - Receives whether a channel was supplied
 *
 * @return bool                                   - Whether the channel configuration is acceptable
 * @retval true                                   - Both descriptors usable, or both variables unset
 * @retval false                                  - A channel was asked for and is not usable
 * @see parseDescriptorNumber(), isUsableDescriptor(), serveControlChannel()
 */
static bool resolveControlChannelFds(int &controlFd, int &observeFd, bool &channelEnabled)
{
    const char *const rawControl = ::getenv(CONTROL_FD_VARIABLE);
    const char *const rawObserve = ::getenv(OBSERVE_FD_VARIABLE);

    if (rawControl == nullptr && rawObserve == nullptr) {
        controlFd = -1;
        observeFd = -1;
        channelEnabled = false;
        std::cout << TRACE_PREFIX << CONTROL_FD_VARIABLE << " and " << OBSERVE_FD_VARIABLE
                  << " are both unset, so no control and observation channel was supplied; this host "
                     "will publish the fake and wait to be stopped, exactly as it does when no "
                     "channel is wanted" << std::endl;
        return true;
    }

    if (rawControl == nullptr || rawObserve == nullptr) {
        std::cout << TRACE_PREFIX << "Only one half of the control and observation channel was "
                     "supplied: " << CONTROL_FD_VARIABLE << " is "
                  << (rawControl == nullptr ? "unset" : "set") << " and " << OBSERVE_FD_VARIABLE
                  << " is " << (rawObserve == nullptr ? "unset" : "set")
                  << ". They travel together: commands with nowhere to reply, or replies with no "
                     "commands to answer, is not a channel. Refusing to start" << std::endl;
        return false;
    }

    int resolvedControlFd = -1;
    if (!parseDescriptorNumber(rawControl, resolvedControlFd)) {
        std::cout << TRACE_PREFIX << CONTROL_FD_VARIABLE << " is set to "
                  << renderUntrustedValue(rawControl)
                  << ", which is not a usable non-negative file descriptor number" << std::endl;
        return false;
    }

    int resolvedObserveFd = -1;
    if (!parseDescriptorNumber(rawObserve, resolvedObserveFd)) {
        std::cout << TRACE_PREFIX << OBSERVE_FD_VARIABLE << " is set to "
                  << renderUntrustedValue(rawObserve)
                  << ", which is not a usable non-negative file descriptor number" << std::endl;
        return false;
    }

    if (resolvedControlFd == resolvedObserveFd) {
        std::cout << TRACE_PREFIX << CONTROL_FD_VARIABLE << " and " << OBSERVE_FD_VARIABLE
                  << " both name descriptor " << resolvedControlFd
                  << ". The channel is two pipes, not one: a single descriptor would have this host "
                     "reading its own replies. Refusing to start" << std::endl;
        return false;
    }

    if (!isUsableDescriptor(resolvedControlFd, false)) {
        std::cout << TRACE_PREFIX << CONTROL_FD_VARIABLE << " names descriptor " << resolvedControlFd
                  << ", which is not open for reading in this process (" << std::strerror(errno)
                  << "). A descriptor created with O_CLOEXEC does not survive this program's exec "
                     "unless the parent cleared FD_CLOEXEC on the child's copy first" << std::endl;
        return false;
    }

    if (!isUsableDescriptor(resolvedObserveFd, true)) {
        std::cout << TRACE_PREFIX << OBSERVE_FD_VARIABLE << " names descriptor " << resolvedObserveFd
                  << ", which is not open for writing in this process (" << std::strerror(errno)
                  << "). A descriptor created with O_CLOEXEC does not survive this program's exec "
                     "unless the parent cleared FD_CLOEXEC on the child's copy first" << std::endl;
        return false;
    }

    controlFd = resolvedControlFd;
    observeFd = resolvedObserveFd;
    channelEnabled = true;

    std::cout << TRACE_PREFIX << "Control and observation channel accepted: commands will be read "
                 "from fd " << controlFd << " (named by " << CONTROL_FD_VARIABLE
              << ") and one reply line written per command to fd " << observeFd << " (named by "
              << OBSERVE_FD_VARIABLE << ")" << std::endl;
    return true;
}

/**
 * @brief Stops a closed observation pipe from killing this process.
 *
 * With SIGPIPE ignored, a parent closing its end yields a classifiable EPIPE instead of a silent
 * termination.  A run with no channel keeps the default disposition.
 *
 * @return bool                                   - Whether the disposition was installed
 * @retval true                                   - SIGPIPE is ignored; a closed pipe reports EPIPE
 * @retval false                                  - The disposition could not be installed
 *
 * @pre Called only when resolveControlChannelFds() reported a channel.
 * @see writeReplyLine(), resolveControlChannelFds()
 */
static bool ignoreBrokenPipeSignal()
{
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    ::sigemptyset(&action.sa_mask);
    action.sa_flags = 0;

    if (::sigaction(SIGPIPE, &action, nullptr) != 0) {
        std::cout << TRACE_PREFIX << "Could not ignore SIGPIPE: " << std::strerror(errno)
                  << ". A parent closing its end of the observation pipe could then terminate this "
                     "host with no diagnostic, so it refuses to serve the channel" << std::endl;
        return false;
    }

    std::cout << TRACE_PREFIX << "SIGPIPE is ignored, so a closed observation pipe reports EPIPE "
                 "instead of terminating this host" << std::endl;
    return true;
}

/**
 * @brief Writes a buffer to a descriptor in full, tolerating short and interrupted writes.
 *
 * Raw and unbuffered, so the readiness token never sits in a stream buffer while the parent waits.
 *
 * @param [in] fd                         - Descriptor to write to
 * @param [in] data                       - Buffer to write.  Must not be null
 * @param [in] length                     - Number of bytes to write
 *
 * @return bool                                   - Whether the whole buffer was written
 * @retval true                                   - All length bytes were written
 * @retval false                                  - The write was rejected; errno says why
 * @see resolveReadinessFd()
 */
static bool writeAllRetryingOnInterrupt(int fd, const char *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        const ssize_t result = ::write(fd, data + written, length - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (result == 0) {
            // A zero-length transfer makes no progress; report it rather than spin.
            errno = EIO;
            return false;
        }

        written += static_cast<size_t>(result);
    }

    return true;
}

/**
 * @brief Reports whether a usable binder driver node is present.
 *
 * Consulted before the first libbinder call, which aborts when the driver cannot be opened, so a
 * host without kernel binder support fails with a traced exit code instead.
 *
 * @return bool                                   - Whether the driver node is present and openable
 * @retval true                                   - The node exists and is readable and writable
 * @retval false                                  - The node is absent or cannot be opened
 *
 * @warning A node check only: it verifies neither the protocol version nor a service manager.
 * @see BINDER_DRIVER_PATH, verifyServiceNameIsFree()
 */
static bool isBinderTransportPresent()
{
    if (::access(BINDER_DRIVER_PATH, R_OK | W_OK) != 0) {
        std::cout << TRACE_PREFIX << "Binder driver " << BINDER_DRIVER_PATH
                  << " is absent or unopenable (" << std::strerror(errno)
                  << "), so this host cannot serve anything on this machine" << std::endl;
        return false;
    }

    std::cout << TRACE_PREFIX << "Binder driver " << BINDER_DRIVER_PATH << " is present and openable"
              << std::endl;
    return true;
}

/**
 * @brief Establishes that nothing is already published under the production service name.
 *
 * A stale registration would make the middleware resolve a service this suite does not own, so it
 * is a hard failure.  checkService() answers at once where getService() would poll.
 *
 * @param [in] serviceName                - Production service name, from the generated interface
 *
 * @return int                                    - EXIT_SUCCESS, or the code to exit with
 * @retval EXIT_SUCCESS                           - The name is free and the fake may be published
 * @retval EXIT_SETUP_FAILED                      - No service manager could be reached
 * @retval EXIT_STALE_REGISTRATION                - Something else already holds the name
 * @pre isBinderTransportPresent() has reported true.
 * @see isBinderTransportPresent(), registerFakeHdmiCecService()
 */
static int verifyServiceNameIsFree(const std::string &serviceName)
{
    const ::android::sp< ::android::IServiceManager> serviceManager =
        ::android::defaultServiceManager();

    if (serviceManager == nullptr) {
        std::cout << TRACE_PREFIX << "No service manager could be reached, so whether \""
                  << serviceName << "\" is already published cannot be established" << std::endl;
        return EXIT_SETUP_FAILED;
    }

    if (serviceManager->checkService(::android::String16(serviceName.c_str())) != nullptr) {
        std::cout << TRACE_PREFIX << "\"" << serviceName << "\" is already published by another "
                     "process. Refusing to publish over it: the invocation's back-end selection "
                     "would resolve against that service rather than this host's fake, so its "
                     "outcome would depend on a process this suite does not own. Stop that process "
                     "and run again" << std::endl;
        return EXIT_STALE_REGISTRATION;
    }

    std::cout << TRACE_PREFIX << "\"" << serviceName << "\" is free" << std::endl;
    return EXIT_SUCCESS;
}

/**
 * @brief Blocks until a termination signal is delivered.
 *
 * A genuine blocking read on the self-pipe with no timer; an interrupted read is retried, and end
 * of file also ends the wait.  Used when no control channel was supplied.
 *
 * @param [out] signalNumber              - Receives the signal number, or 0 if the self-pipe closed
 *
 * @return bool                                   - Whether the wait ended as intended
 * @retval true                                   - A signal arrived or the pipe closed
 * @retval false                                  - The self-pipe could not be read
 *
 * @pre installShutdownHandlers() has reported true.
 * @see handleShutdownSignal(), installShutdownHandlers(), serveControlChannel()
 */
static bool waitForShutdownSignal(int &signalNumber)
{
    unsigned char wakeByte = 0;

    for (;;) {
        const ssize_t result = ::read(g_shutdownPipeReadFd, &wakeByte, sizeof(wakeByte));

        if (result >= 0) {
            // A byte means the handler ran and zero means end of file; either way the wait is over.
            signalNumber = static_cast<int>(g_shutdownSignalNumber);
            return true;
        }

        if (errno == EINTR) {
            continue;
        }

        std::cout << TRACE_PREFIX << "Could not wait on the shutdown self-pipe: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
}

/**
 * @brief Outcome of one attempt to deliver a reply line to the observation descriptor.
 *
 * Each of the three outcomes demands a different response from the caller.
 *
 * @see writeReplyLine(), serveControlChannel()
 */
enum class ReplyOutcome {
    /** @brief The whole line reached the observation descriptor; the session continues. */
    DELIVERED,

    /** @brief The parent closed its end of the observation pipe; the session ends cleanly. */
    PARENT_GONE,

    /** @brief Undelivered while the parent is still there; the caller exits EXIT_CONTROL_CHANNEL_FAILED. */
    FAILED
};

/**
 * @brief Writes exactly one reply line to the observation descriptor within one whole-call deadline.
 *
 * @param [in] observeFd                  - Observation descriptor to write to
 * @param [in] reply                      - Reply text without terminator; begins "OK " or "ERR "
 *
 * @return ReplyOutcome                           - How the delivery ended
 * @retval ReplyOutcome::DELIVERED                - Every byte written and the flags restored
 * @retval ReplyOutcome::PARENT_GONE              - No reader left: EPIPE or POLLERR/HUP/NVAL
 * @retval ReplyOutcome::FAILED                   - Refused, timed out, or a system call failed
 * @pre SIGPIPE is ignored and observeFd is open for writing.
 * @see ReplyOutcome, OBSERVE_WRITE_TIMEOUT_MS, ignoreBrokenPipeSignal()
 */
static ReplyOutcome writeReplyLine(int observeFd, const std::string &reply)
{
    const std::string line = reply + "\n";

    if (line.size() > MAX_REPLY_LINE_LENGTH) {
        std::cout << TRACE_PREFIX << "A reply line of " << line.size() << " bytes exceeds the "
                  << MAX_REPLY_LINE_LENGTH
                  << " byte cap, so it is refused rather than attempted: a line that large is no "
                     "longer one atomic pipe write, and a client can survive no reply far better "
                     "than half of one" << std::endl;
        return ReplyOutcome::FAILED;
    }

    // One CLOCK_MONOTONIC deadline for the whole call: every wait uses the time left to it, so
    // interruptions and partial writes cannot extend the bound.
    struct timespec deadline;
    if (::clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        std::cout << TRACE_PREFIX << "Could not read the monotonic clock to bound a reply write: "
                  << std::strerror(errno) << ". An unbounded write is not attempted" << std::endl;
        return ReplyOutcome::FAILED;
    }

    deadline.tv_sec += OBSERVE_WRITE_TIMEOUT_MS / 1000;
    deadline.tv_nsec += static_cast<long>(OBSERVE_WRITE_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    // POLLOUT only promises that a PIPE_BUF write will not block, so O_NONBLOCK is set for this
    // call and the caller's flags are restored on the way out.
    const int originalFlags = ::fcntl(observeFd, F_GETFL);
    if (originalFlags < 0) {
        std::cout << TRACE_PREFIX << "Could not read the flags of fd " << observeFd
                  << " to make a reply write nonblocking: " << std::strerror(errno)
                  << ". A write that could block past the deadline is not attempted" << std::endl;
        return ReplyOutcome::FAILED;
    }

    if (::fcntl(observeFd, F_SETFL, originalFlags | O_NONBLOCK) != 0) {
        std::cout << TRACE_PREFIX << "Could not set O_NONBLOCK on fd " << observeFd << ": "
                  << std::strerror(errno)
                  << ". A write that could block past the deadline is not attempted" << std::endl;
        return ReplyOutcome::FAILED;
    }

    ReplyOutcome outcome = ReplyOutcome::DELIVERED;
    size_t written = 0;

    while (written < line.size()) {
        struct timespec now;
        if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            std::cout << TRACE_PREFIX << "Could not read the monotonic clock while writing a reply: "
                      << std::strerror(errno) << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        long long remainingMs =
            (static_cast<long long>(deadline.tv_sec) - static_cast<long long>(now.tv_sec)) * 1000LL +
            (static_cast<long long>(deadline.tv_nsec) - static_cast<long long>(now.tv_nsec)) /
                1000000LL;

        if (remainingMs < 0) {
            remainingMs = 0;
        }

        if (remainingMs == 0) {
            std::cout << TRACE_PREFIX << "The observation descriptor did not accept a reply within "
                      << OBSERVE_WRITE_TIMEOUT_MS << " ms, so the client is not reading. Refusing to "
                         "block: a hung host is harder to diagnose than a failed one" << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        struct pollfd watched;
        watched.fd = observeFd;
        watched.events = POLLOUT;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remainingMs));

        if (ready < 0) {
            if (errno == EINTR) {
                // An interruption costs only its own time; the next pass recomputes what is left.
                continue;
            }
            std::cout << TRACE_PREFIX << "Could not wait for the observation descriptor to accept a "
                         "reply: " << std::strerror(errno) << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        if (ready == 0) {
            std::cout << TRACE_PREFIX << "The observation descriptor did not accept a reply within "
                      << OBSERVE_WRITE_TIMEOUT_MS << " ms, so the client is not reading. Refusing to "
                         "block: a hung host is harder to diagnose than a failed one" << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        if ((watched.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::cout << TRACE_PREFIX << "The observation descriptor reported that its reader is gone"
                      << std::endl;
            outcome = ReplyOutcome::PARENT_GONE;
            break;
        }

        const ssize_t result = ::write(observeFd, line.data() + written, line.size() - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Not enough room yet: wait again on what is left of the deadline.
                continue;
            }
            if (errno == EPIPE) {
                std::cout << TRACE_PREFIX << "The observation pipe is broken, so the client has "
                             "closed its end" << std::endl;
                outcome = ReplyOutcome::PARENT_GONE;
                break;
            }
            std::cout << TRACE_PREFIX << "Could not write a reply to fd " << observeFd << ": "
                      << std::strerror(errno) << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        if (result == 0) {
            // A zero-length transfer makes no progress; report it rather than spin.
            std::cout << TRACE_PREFIX << "A reply write to fd " << observeFd
                      << " transferred nothing and made no progress" << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        written += static_cast<size_t>(result);
    }

    // Single restore point for every exit from the loop.  A failed restore turns DELIVERED into
    // FAILED; PARENT_GONE stays PARENT_GONE.
    if (::fcntl(observeFd, F_SETFL, originalFlags) != 0) {
        std::cout << TRACE_PREFIX << "Could not restore the original flags on fd " << observeFd << ": "
                  << std::strerror(errno)
                  << ". The descriptor is left nonblocking, which is not the state its owner lent it in"
                  << std::endl;
        if (outcome == ReplyOutcome::DELIVERED) {
            outcome = ReplyOutcome::FAILED;
        }
    }

    return outcome;
}

/**
 * @brief Renders bytes as an unseparated run of lowercase hexadecimal digits.
 *
 * The protocol's one payload encoding, used by `deliver` and `last-sent`.
 *
 * @param [in] bytes                      - Bytes to render
 *
 * @return std::string                            - Hexadecimal text, empty when the input is empty
 * @see parseHexPayload()
 */
static std::string bytesToLowercaseHex(const std::vector<uint8_t> &bytes)
{
    static const char digits[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(bytes.size() * 2);

    for (size_t index = 0; index < bytes.size(); ++index) {
        hex.push_back(digits[(bytes[index] >> 4) & 0x0F]);
        hex.push_back(digits[bytes[index] & 0x0F]);
    }

    return hex;
}

/**
 * @brief Decodes a hexadecimal payload token into bytes.
 *
 * Rejects an empty or odd-length token and any non-hex character; uppercase digits are tolerated.
 * The decoded bytes are delivered uninspected.
 *
 * @param [in]  hex                       - Token to decode
 * @param [out] bytes                     - Receives the bytes; cleared first, empty on failure
 *
 * @return bool                                   - Whether the token decoded
 * @retval true                                   - bytes holds hex.size() / 2 decoded bytes
 * @retval false                                  - Empty, odd length, or a non-hex character
 * @see bytesToLowercaseHex()
 */
static bool parseHexPayload(const std::string &hex, std::vector<uint8_t> &bytes)
{
    bytes.clear();

    if (hex.empty() || (hex.size() % 2) != 0) {
        return false;
    }

    bytes.reserve(hex.size() / 2);

    for (size_t index = 0; index < hex.size(); index += 2) {
        int nibbles[2] = { 0, 0 };

        for (size_t half = 0; half < 2; ++half) {
            const char character = hex[index + half];

            if (character >= '0' && character <= '9') {
                nibbles[half] = character - '0';
            } else if (character >= 'a' && character <= 'f') {
                nibbles[half] = 10 + (character - 'a');
            } else if (character >= 'A' && character <= 'F') {
                nibbles[half] = 10 + (character - 'A');
            } else {
                bytes.clear();
                return false;
            }
        }

        bytes.push_back(static_cast<uint8_t>((nibbles[0] << 4) | nibbles[1]));
    }

    return true;
}

/**
 * @brief Splits a command line into its verb and arguments.
 *
 * Separators are runs of spaces and tabs; an empty result means the line carried no verb.
 *
 * @param [in] line                       - Command line, already stripped of its terminator
 *
 * @return std::vector<std::string>               - The tokens, verb first, empty for a blank line
 * @see handleControlCommand()
 */
static std::vector<std::string> tokenizeCommandLine(const std::string &line)
{
    std::vector<std::string> tokens;
    size_t index = 0;

    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }

        const size_t tokenStart = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }

        if (index > tokenStart) {
            tokens.push_back(line.substr(tokenStart, index - tokenStart));
        }
    }

    return tokens;
}

/** @brief One named AIDL method of a `calls` reply: its field name and its transaction code. */
struct TransactionMethod {
    /** @brief Method name as the reply field spells it, e.g. "getLogicalAddresses". */
    const char *name;

    /** @brief Generated TRANSACTION_* code the method is dispatched under. */
    uint32_t code;
};

/**
 * @brief Renders one interface's transaction counts as the `calls` reply fields.
 *
 * @param [in] interfaceName              - Field prefix, "IHdmiCec" or "IHdmiCecController"
 * @param [in] methods                    - The interface's named methods, in reply order
 * @param [in] counts                     - Transactions by code, from the fake's getTransactionCounts()
 *
 * @return std::string                            - " <interface>.<method>=<n>" per method, then
 *                                                  " <interface>.other=<n>" summing every other code
 * @see handleControlCommand()
 */
static std::string renderTransactionCounts(const std::string &interfaceName,
                                           const std::vector<TransactionMethod> &methods,
                                           const std::map<uint32_t, int32_t> &counts)
{
    std::string fields;
    long long other = 0;

    for (std::map<uint32_t, int32_t>::const_iterator entry = counts.begin(); entry != counts.end();
         ++entry) {
        bool named = false;

        for (size_t index = 0; index < methods.size(); ++index) {
            if (methods[index].code == entry->first) {
                named = true;
                break;
            }
        }

        if (!named) {
            other += entry->second;
        }
    }

    for (size_t index = 0; index < methods.size(); ++index) {
        const std::map<uint32_t, int32_t>::const_iterator found = counts.find(methods[index].code);
        const int32_t count = (found != counts.end()) ? found->second : 0;

        fields += " " + interfaceName + "." + methods[index].name + "=" + std::to_string(count);
    }

    fields += " " + interfaceName + ".other=" + std::to_string(other);
    return fields;
}

/**
 * @brief Executes one command line against the hosted fake and composes its reply.
 *
 * @param [in]  line                      - Command line, stripped of its terminator and trailing CR
 * @param [in]  fake                      - Hosted fake the command acts on
 * @param [out] reply                     - Receives the reply text; untouched when there is none
 * @param [out] shutdownRequested         - Set true by `shutdown` only; never cleared here
 *
 * @return bool                                   - Whether a reply is to be sent
 * @retval true                                   - reply holds one line beginning "OK " or "ERR "
 * @retval false                                  - The line was blank, so no reply is sent
 * @warning No verb resets the fake: reset() would drop the live session's listener.
 * @see FakeHdmiCecService::fireOnMessageReceived(), writeReplyLine()
 */
static bool handleControlCommand(const std::string &line, FakeHdmiCecService &fake,
                                 std::string &reply, bool &shutdownRequested)
{
    const std::vector<std::string> tokens = tokenizeCommandLine(line);

    if (tokens.empty()) {
        return false;
    }

    const std::string &verb = tokens[0];
    const size_t argumentCount = tokens.size() - 1;

    if (verb == "ping") {
        if (argumentCount != 0) {
            reply = "ERR bad-args ping";
            return true;
        }
        reply = "OK pong";
        return true;
    }

    if (verb == "deliver") {
        if (argumentCount != 1) {
            reply = "ERR bad-args deliver";
            return true;
        }

        std::vector<uint8_t> message;
        if (!parseHexPayload(tokens[1], message)) {
            reply = "ERR bad-hex";
            return true;
        }

        if (!fake.fireOnMessageReceived(message)) {
            reply = "ERR no-listener";
            return true;
        }

        reply = "OK delivered " + std::to_string(message.size());
        return true;
    }

    if (verb == "sent-count" || verb == "last-sent") {
        if (argumentCount != 0) {
            reply = "ERR bad-args " + verb;
            return true;
        }

        const ::android::sp<FakeHdmiCecController> controller = fake.getController();
        if (controller == nullptr) {
            reply = "ERR no-controller";
            return true;
        }

        if (verb == "sent-count") {
            reply = "OK sent-count " + std::to_string(controller->getSendMessageCallCount());
        } else {
            reply = "OK last-sent " + bytesToLowercaseHex(controller->getLastSentMessage());
        }
        return true;
    }

    if (verb == "open-count") {
        if (argumentCount != 0) {
            reply = "ERR bad-args open-count";
            return true;
        }
        reply = "OK open-count " + std::to_string(fake.getOpenCallCount());
        return true;
    }

    if (verb == "close-count") {
        if (argumentCount != 0) {
            reply = "ERR bad-args close-count";
            return true;
        }
        reply = "OK close-count " + std::to_string(fake.getCloseCallCount());
        return true;
    }

    if (verb == "listener") {
        if (argumentCount != 0) {
            reply = "ERR bad-args listener";
            return true;
        }
        reply = (fake.getListener() != nullptr) ? "OK listener present" : "OK listener absent";
        return true;
    }

    if (verb == "registered") {
        if (argumentCount != 0) {
            reply = "ERR bad-args registered";
            return true;
        }

        const ::android::sp<FakeHdmiCecController> controller = fake.getController();
        if (controller == nullptr) {
            reply = "ERR no-controller";
            return true;
        }

        const std::vector<int32_t> registered = controller->getRegisteredLogicalAddresses();
        std::string field;
        for (size_t index = 0; index < registered.size(); ++index) {
            field += (index == 0 ? "" : ",") + std::to_string(registered[index]);
        }

        reply = field.empty() ? std::string("OK registered") : "OK registered " + field;
        return true;
    }

    if (verb == "calls") {
        if (argumentCount != 0) {
            reply = "ERR bad-args calls";
            return true;
        }

        const ::android::sp<FakeHdmiCecController> controller = fake.getController();
        if (controller == nullptr) {
            reply = "ERR no-controller";
            return true;
        }

        /** @brief Generated IHdmiCec server base, source of its method TRANSACTION_* codes. */
        typedef ::com::rdk::hal::hdmicec::BnHdmiCec Service;
        /** @brief Generated IHdmiCecController server base, source of its method TRANSACTION_* codes. */
        typedef ::com::rdk::hal::hdmicec::BnHdmiCecController Controller;

        static const std::vector<TransactionMethod> SERVICE_METHODS = {
            { "getState", Service::TRANSACTION_getState },
            { "getProperty", Service::TRANSACTION_getProperty },
            { "getLogicalAddresses", Service::TRANSACTION_getLogicalAddresses },
            { "open", Service::TRANSACTION_open },
            { "close", Service::TRANSACTION_close },
            { "registerEventListener", Service::TRANSACTION_registerEventListener },
            { "unregisterEventListener", Service::TRANSACTION_unregisterEventListener },
            { "getInterfaceVersion", Service::TRANSACTION_getInterfaceVersion },
            { "getInterfaceHash", Service::TRANSACTION_getInterfaceHash },
        };

        static const std::vector<TransactionMethod> CONTROLLER_METHODS = {
            { "addLogicalAddresses", Controller::TRANSACTION_addLogicalAddresses },
            { "removeLogicalAddresses", Controller::TRANSACTION_removeLogicalAddresses },
            { "sendMessage", Controller::TRANSACTION_sendMessage },
            { "getInterfaceVersion", Controller::TRANSACTION_getInterfaceVersion },
            { "getInterfaceHash", Controller::TRANSACTION_getInterfaceHash },
        };

        reply = "OK calls" +
                renderTransactionCounts("IHdmiCec", SERVICE_METHODS, fake.getTransactionCounts()) +
                renderTransactionCounts("IHdmiCecController", CONTROLLER_METHODS,
                                        controller->getTransactionCounts());
        return true;
    }

    if (verb == "shutdown") {
        if (argumentCount != 0) {
            reply = "ERR bad-args shutdown";
            return true;
        }
        shutdownRequested = true;
        reply = "OK shutdown";
        return true;
    }

    reply = "ERR unknown-command " + verb;
    return true;
}

/**
 * @brief Serves the control and observation channel until the session ends.
 *
 * @param [in]  controlFd                 - Descriptor commands are read from
 * @param [in]  observeFd                 - Descriptor replies are written to
 * @param [in]  fake                      - Hosted fake the commands act on
 * @param [out] signalNumber              - Receives the ending signal number, or 0 otherwise
 *
 * @return int                                    - EXIT_SUCCESS, or the code to exit with
 * @retval EXIT_SUCCESS                           - `shutdown`, end of file, reader gone or a signal
 * @retval EXIT_CONTROL_CHANNEL_FAILED            - poll()/read() failed or a reply went undelivered
 * @pre The shutdown path, the channel and the SIGPIPE disposition are established.
 * @see handleControlCommand(), writeReplyLine(), waitForShutdownSignal()
 */
static int serveControlChannel(int controlFd, int observeFd, FakeHdmiCecService &fake,
                               int &signalNumber)
{
    std::string pending;
    bool discardingOverlongLine = false;
    char buffer[CONTROL_READ_CHUNK];

    std::cout << TRACE_PREFIX << "Serving the control and observation channel: commands on fd "
              << controlFd << ", replies on fd " << observeFd
              << ", and SIGTERM or SIGINT still stops this host at any point" << std::endl;

    for (;;) {
        struct pollfd watched[2];
        watched[0].fd = g_shutdownPipeReadFd;
        watched[0].events = POLLIN;
        watched[0].revents = 0;
        watched[1].fd = controlFd;
        watched[1].events = POLLIN;
        watched[1].revents = 0;

        const int ready = ::poll(watched, 2, -1);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cout << TRACE_PREFIX << "Could not wait on the shutdown self-pipe and the control "
                         "descriptor: " << std::strerror(errno) << std::endl;
            return EXIT_CONTROL_CHANNEL_FAILED;
        }

        // The shutdown pipe is examined first: a stop request outranks a queued command.
        if ((watched[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
            unsigned char wakeByte = 0;
            ssize_t consumed;
            do {
                consumed = ::read(g_shutdownPipeReadFd, &wakeByte, sizeof(wakeByte));
            } while (consumed < 0 && errno == EINTR);

            signalNumber = static_cast<int>(g_shutdownSignalNumber);
            std::cout << TRACE_PREFIX << "The shutdown path woke while serving the channel, so the "
                         "session ends here" << std::endl;
            return EXIT_SUCCESS;
        }

        if ((watched[1].revents & POLLIN) != 0) {
            const ssize_t received = ::read(controlFd, buffer, sizeof(buffer));

            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cout << TRACE_PREFIX << "Could not read from the control descriptor: "
                          << std::strerror(errno) << std::endl;
                return EXIT_CONTROL_CHANNEL_FAILED;
            }

            if (received == 0) {
                signalNumber = 0;
                std::cout << TRACE_PREFIX << "The control descriptor reached end of file, so the "
                             "parent has closed its write end; shutting down cleanly" << std::endl;
                return EXIT_SUCCESS;
            }

            pending.append(buffer, static_cast<size_t>(received));
        } else if ((watched[1].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            signalNumber = 0;
            std::cout << TRACE_PREFIX << "The control descriptor reported that its writer is gone, so "
                         "the session ends here" << std::endl;
            return EXIT_SUCCESS;
        }

        size_t newlinePosition = pending.find('\n');
        while (newlinePosition != std::string::npos) {
            std::string line = pending.substr(0, newlinePosition);
            pending.erase(0, newlinePosition + 1);

            // Strip a trailing CR so CRLF-framed commands are understood.
            if (!line.empty() && line[line.size() - 1] == '\r') {
                line.erase(line.size() - 1);
            }

            std::string reply;
            bool shutdownRequested = false;
            bool hasReply = false;

            if (discardingOverlongLine || line.size() > MAX_COMMAND_LINE_LENGTH) {
                // An overlong line, whole or the tail of one already discarded, gets one reply
                // and is never parsed, whatever the read boundaries were.
                discardingOverlongLine = false;
                reply = "ERR command-too-long";
                hasReply = true;
                std::cout << TRACE_PREFIX << "A command line of " << line.size()
                          << " bytes exceeds the " << MAX_COMMAND_LINE_LENGTH
                          << " byte cap; answering \"ERR command-too-long\" without parsing it"
                          << std::endl;
            } else {
                hasReply = handleControlCommand(line, fake, reply, shutdownRequested);
            }

            if (hasReply) {
                // Command and reply both carry caller bytes, so both go through the renderer.
                std::cout << TRACE_PREFIX << "Command " << renderUntrustedValue(line)
                          << " answered " << renderUntrustedValue(reply) << std::endl;

                const ReplyOutcome outcome = writeReplyLine(observeFd, reply);

                if (outcome == ReplyOutcome::PARENT_GONE) {
                    signalNumber = 0;
                    std::cout << TRACE_PREFIX << "The client is no longer reading replies, so the "
                                 "session ends here" << std::endl;
                    return EXIT_SUCCESS;
                }

                if (outcome == ReplyOutcome::FAILED) {
                    return EXIT_CONTROL_CHANNEL_FAILED;
                }
            }

            if (shutdownRequested) {
                signalNumber = 0;
                std::cout << TRACE_PREFIX << "A shutdown command was received and acknowledged, so "
                             "this host stops through the same clean teardown a signal takes"
                          << std::endl;
                return EXIT_SUCCESS;
            }

            newlinePosition = pending.find('\n');
        }

        if (pending.size() > MAX_COMMAND_LINE_LENGTH) {
            // No terminator within the cap: drop the bytes now and owe one "ERR command-too-long"
            // for when the terminator arrives.
            std::cout << TRACE_PREFIX << "A command line exceeded " << MAX_COMMAND_LINE_LENGTH
                      << " bytes with no terminator; discarding it and answering "
                         "\"ERR command-too-long\" when its terminator arrives" << std::endl;
            pending.clear();
            discardingOverlongLine = true;
        }
    }
}

/**
 * @brief Hosts the fake com.rdk.hal.hdmicec AIDL service until asked to stop.
 *
 * Each startup failure traces and returns its own code with no readiness line written; serving
 * and shutdown-wait failures return after it.  Configuration comes from the environment only.
 *
 * @param [in] argc                       - Argument count.  Unused
 * @param [in] argv                       - Argument vector.  Unused
 *
 * @return int                                    - Process exit status
 * @retval EXIT_SUCCESS                           - Published, served, and stopped cleanly
 * @retval EXIT_STALE_REGISTRATION                - The production service name was already taken
 * @retval EXIT_BAD_READY_FD                      - CEC_FAKE_HOST_READY_FD is not a plain number
 * @retval EXIT_REGISTRATION_FAILED               - The service manager refused to publish the fake
 * @retval EXIT_READINESS_WRITE_FAILED            - The readiness token could not be written
 * @retval EXIT_NO_BINDER_TRANSPORT               - No usable binder driver node exists
 * @retval EXIT_SETUP_FAILED                      - Setup, or the plain shutdown wait, failed
 * @retval EXIT_BAD_CONTROL_CHANNEL               - A channel was asked for and is not usable
 * @retval EXIT_CONTROL_CHANNEL_FAILED            - The channel failed while being served
 * @see resolveReadinessFd(), resolveControlChannelFds(), serveControlChannel()
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // The pid comes first: a parent's diagnostics and an orphan hunt both key on it.
    std::cout << TRACE_PREFIX << "Starting the out-of-process fake HDMI CEC AIDL service host, pid "
              << ::getpid() << std::endl;

    if (!installShutdownHandlers()) {
        return EXIT_SETUP_FAILED;
    }

    int readinessFd = -1;
    if (!resolveReadinessFd(readinessFd)) {
        return EXIT_BAD_READY_FD;
    }

    // Resolved before anything is published, so a rejected handoff costs nothing.
    int controlFd = -1;
    int observeFd = -1;
    bool channelEnabled = false;
    if (!resolveControlChannelFds(controlFd, observeFd, channelEnabled)) {
        return EXIT_BAD_CONTROL_CHANNEL;
    }

    if (channelEnabled && !ignoreBrokenPipeSignal()) {
        return EXIT_SETUP_FAILED;
    }

    if (!isBinderTransportPresent()) {
        return EXIT_NO_BINDER_TRANSPORT;
    }

    // The name comes from the generated interface, never spelled here.  Only the service is
    // published: a client receives the controller from open().
    const std::string &serviceName = ::com::rdk::hal::hdmicec::IHdmiCec::serviceName();

    const int nameCheck = verifyServiceNameIsFree(serviceName);
    if (nameCheck != EXIT_SUCCESS) {
        return nameCheck;
    }

    ::android::sp<FakeHdmiCecService> fake = ::android::sp<FakeHdmiCecService>::make();
    if (fake == nullptr) {
        std::cout << TRACE_PREFIX << "The fake HDMI CEC AIDL service could not be constructed"
                  << std::endl;
        return EXIT_SETUP_FAILED;
    }

    // Publish the pointer so in-process code reaches the registered fake; the strong reference
    // above keeps it alive.
    FakeHdmiCecService::setInstance(fake.get());
    std::cout << TRACE_PREFIX << "Constructed the fake HDMI CEC AIDL service "
              << fakeHdmiCecTraceLabel(fake.get()) << std::endl;

    // Service-side pool, started before publication so no transaction finds no thread.  The
    // library default sets its size: lowering an established maximum can abort the process.
    ::android::ProcessState::self()->startThreadPool();
    std::cout << TRACE_PREFIX << "Started the service-side binder threadpool" << std::endl;

    if (!registerFakeHdmiCecService(fake)) {
        std::cout << TRACE_PREFIX << "The fake could not be published as \"" << serviceName
                  << "\", so this host has nothing to serve and is not ready" << std::endl;
        return EXIT_REGISTRATION_FAILED;
    }

    std::cout << TRACE_PREFIX << "Published the fake as \"" << serviceName << "\"" << std::endl;

    // Ready, and only now.  The token is written raw so it cannot sit in a stream buffer.
    if (!writeAllRetryingOnInterrupt(readinessFd, READINESS_TOKEN, std::strlen(READINESS_TOKEN))) {
        std::cout << TRACE_PREFIX << "Could not write the readiness token to fd " << readinessFd
                  << ": " << std::strerror(errno)
                  << ". The parent can never learn this host is ready, so it exits rather than serve "
                     "an invocation that will time out anyway" << std::endl;
        return EXIT_READINESS_WRITE_FAILED;
    }

    std::cout << TRACE_PREFIX << "Wrote the readiness token to fd " << readinessFd
              << (channelEnabled ? "; serving the control and observation channel, and still stoppable "
                                   "with SIGTERM or SIGINT"
                                 : "; waiting for SIGTERM or SIGINT")
              << std::endl;

    // With a channel, serve it alongside the shutdown path; without one, block on the self-pipe.
    int shutdownSignal = 0;
    int serviceExitCode = EXIT_SUCCESS;

    if (channelEnabled) {
        serviceExitCode = serveControlChannel(controlFd, observeFd, *fake, shutdownSignal);
    } else if (!waitForShutdownSignal(shutdownSignal)) {
        serviceExitCode = EXIT_SETUP_FAILED;
    }

    if (shutdownSignal != 0) {
        std::cout << TRACE_PREFIX << "Received signal " << shutdownSignal << ", shutting down"
                  << std::endl;
    } else {
        std::cout << TRACE_PREFIX << "The wait ended without a signal, shutting down" << std::endl;
    }

    // Release everything on every path.  The binder registration needs no withdrawal: the service
    // manager exposes no removal API and process exit releases it.
    FakeHdmiCecService::setInstance(nullptr);
    ::close(g_shutdownPipeReadFd);
    ::close(static_cast<int>(g_shutdownPipeWriteFd));
    g_shutdownPipeReadFd = -1;
    g_shutdownPipeWriteFd = -1;

    if (channelEnabled) {
        ::close(controlFd);
        ::close(observeFd);
    }

    if (serviceExitCode != EXIT_SUCCESS) {
        std::cout << TRACE_PREFIX << "Exiting with status " << serviceExitCode
                  << " after releasing every resource this host held" << std::endl;
        return serviceExitCode;
    }

    std::cout << TRACE_PREFIX << "Exited cleanly" << std::endl;
    return EXIT_SUCCESS;
}

/** @} */ // End of HDMI_CEC_FAKE_AIDL_SERVICE_HOST
