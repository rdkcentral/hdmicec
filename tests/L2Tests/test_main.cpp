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
 * @file test_main.cpp
 *
 * @brief Global environment of the L2 tier and parent half of the fake service host lifecycle.
 *
 * Owns the legacy HAL mock, the out-of-process fake service host, its pipes and child processes,
 * and their bring-up order; the case file reaches this unit only through
 * cecL2HostControlChannelIsOpen(), cecL2HostControlRequest(),
 * cecL2ProveEpipeDiagnosticAndChildReaping() and cecL2RequestedAidlMode(). CEC_TEST_AIDL_MODE
 * unset, empty or `absent` launches nothing and exercises the legacy back-end (invocation D);
 * `remote` launches the host, waits for its readiness token and pings its control channel before
 * init (AIDL, invocation E).
 * `compatible`, `incompatible` and any other value fail the run, as does every setup fault.
 *
 * @warning LibCCEC::init() fixes the back-end selection for the process, so the host must be
 *          ready before it is called.
 * @see mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp
 * @see tests/L2Tests/ccec/test_DualPathIntegration.cpp
 */

#include <gtest/gtest.h>
#include <iostream>
#include "hdmi_cec_driver_mock.h"
#include "ccec/LibCCEC.hpp"

/* Reached by relative path, as ccec/src is not on AM_CPPFLAGS, as a dynamic_cast target alone;
 * it includes no binder or AIDL header. */
#include "../../ccec/src/DriverImpl.hpp"

/* POSIX primitives for the host lifecycle; this translation unit includes no binder or AIDL
 * header and makes no direct binder API call. */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief Legacy HAL double owned by the global environment, null whenever none is allocated.
 *
 * SetUp allocates it and installs it with setInstance(); TearDown uninstalls and deletes it.
 */
static HdmiCecDriverMock* g_driverMock = nullptr;

/**
 * @brief Pid of the fake service host this invocation launched, or -1 when none was launched.
 *
 * TearDown runs even after a failed SetUp, so -1 tells it there is nothing to signal or reap.
 */
static pid_t g_hostPid = -1;

/**
 * @brief Process group the launched host leads, or -1 when none was launched or confirmed.
 *
 * Set only once getpgid() shows the group equals the host pid and differs from this runner's
 * group; otherwise teardown signals the single pid rather than an unverified group.
 *
 * @see startFakeServiceHost(), terminateAndReapFakeServiceHost(), terminateAndReapChildProcess()
 */
static pid_t g_hostProcessGroupId = -1;

/**
 * @brief Read end of the readiness pipe, kept open for the host's lifetime as its death channel.
 *
 * End of file on it means the host has exited, so TearDown can await the exit without a timer.
 *
 * @see terminateAndReapFakeServiceHost()
 */
static int g_hostReadinessReadFd = -1;

/**
 * @brief This process's ends of the host control and observation pipes, or -1 without a host.
 *
 * The channel lets cases read what the host's fake received and trigger inbound callbacks. It
 * is a plain pipe rather than binder, so the evidence never travels over the transport under test.
 */
static int g_hostControlWriteFd = -1;
static int g_hostObserveReadFd  = -1;

/**
 * @brief Whether the host reached readiness and answered the channel ping.
 *
 * Gates only teardown's polite `shutdown` request; a host that never became ready is signalled.
 */
static bool g_hostReportedReady = false;

/**
 * @brief SIGPIPE disposition found on entry, and whether this harness replaced it.
 *
 * The flag makes the TearDown restore a no-op when SetUp never installed the replacement.
 */
static struct sigaction g_previousSigpipeAction;
static bool g_sigpipeDispositionReplaced = false;

namespace {

/* Diagnostic rendering of untrusted values: every value is escaped, bounded and quoted so that
 * no input can end a log line or forge a workflow-command line. */

/** @brief How many rendered characters a diagnostic will carry before it is truncated. */
const std::size_t RENDER_LIMIT = 200;

/**
 * @brief Renders an untrusted byte string as one bounded, escaped, double-quoted token.
 *
 * A backslash is doubled first, newline, CR and tab become C escapes, every other byte outside
 * 0x20..0x7E becomes a lower-case hex escape, and output past RENDER_LIMIT is truncated.
 *
 * @param [in] value                      - Bytes to render; any content is accepted
 * @param [in] length                     - Number of bytes in value
 *
 * @return std::string                            - The quoted, single-line rendering
 *
 * @warning Apply it to the value only, never to a whole message.
 * @note Identical copies live in tests/L1Tests/test_main.cpp, the fake host, run_coverage.sh
 *       and aidl-path-tests-rootfs.sh; change all five together.
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

        /* Backslash first, so every escape added below stays distinguishable from the same
         * characters occurring literally in the value. */
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
inline std::string renderUntrustedValue(const std::string &value)
{
    return renderUntrustedValue(value.data(), value.size());
}

/**
 * @brief renderUntrustedValue() for a C string that may be null.
 *
 * A null pointer renders as the undelimited <unset>, so unset and empty ("") stay distinct.
 *
 * @param [in] value                      - C string to render, or null
 *
 * @return std::string                            - <unset> for null, else the quoted rendering
 * @see renderUntrustedValue(const char *, std::size_t)
 */
inline std::string renderUntrustedValue(const char *value)
{
    if (value == nullptr) {
        return std::string("<unset>");
    }
    return renderUntrustedValue(value, std::strlen(value));
}

/**
 * @brief The harness variable and its four values, spelled once each.
 *
 * A fixed contract shared with the L1 runner, run_coverage.sh, both CI workflows and the docs.
 */
const char *const AIDL_MODE_VARIABLE     = "CEC_TEST_AIDL_MODE";
const char *const AIDL_MODE_ABSENT       = "absent";
const char *const AIDL_MODE_COMPATIBLE   = "compatible";
const char *const AIDL_MODE_INCOMPATIBLE = "incompatible";
const char *const AIDL_MODE_REMOTE       = "remote";

/** @brief Variable naming the fake service host binary; set by the build, read by this harness. */
const char *const HOST_PATH_VARIABLE = "CEC_FAKE_AIDL_HOST_PATH";

/**
 * @brief Variable giving the host the number of its inherited readiness-pipe write end.
 *
 * Formatted as plain unsigned decimal, the only form the host's strict parser accepts.
 */
const char *const HOST_READY_FD_VARIABLE = "CEC_FAKE_HOST_READY_FD";

/**
 * @brief The host's readiness line, matched verbatim including its trailing newline.
 *
 * Owned by the fake host source, which writes it once after publishing the service; the
 * newline proves the line arrived whole.
 */
const char *const HOST_READINESS_TOKEN = "FAKE_HDMI_CEC_AIDL_HOST_READY\n";

/**
 * @brief How long to wait for the host's readiness token before failing the run.
 *
 * Thirty seconds covers a cold host start in an emulated CI guest, including the service
 * manager lookup's one-second retries, while still failing well inside any CI job timeout.
 */
const int HOST_READINESS_TIMEOUT_MS = 30000;

/**
 * @brief How long to wait for a signalled host to exit before escalating to SIGKILL.
 *
 * A healthy host exits in milliseconds; one still running after five seconds is wedged, and
 * SIGKILL stops it outliving the run while holding the production service name.
 */
const int HOST_SHUTDOWN_TIMEOUT_MS = 5000;

/**
 * @brief Upper bound on bytes accepted from the readiness pipe before giving up.
 *
 * The token is thirty bytes, so reaching the cap means the descriptor is not the host's.
 */
const std::size_t HOST_READINESS_MAX_BYTES = 4096;

/**
 * @brief Variables naming the host's inherited control (read) and observation (write) ends.
 *
 * Owned by the fake host source, which refuses to start unless both are set, valid, distinct
 * and open in the right direction; this harness always sets both.
 */
const char *const HOST_CONTROL_FD_VARIABLE = "CEC_FAKE_HOST_CONTROL_FD";
const char *const HOST_OBSERVE_FD_VARIABLE = "CEC_FAKE_HOST_OBSERVE_FD";

/**
 * @brief How long one control command may wait for its reply before the request fails.
 *
 * A failure bound, not an expected duration: healthy replies take under a millisecond, and ten
 * seconds absorbs CI guest load while turning a stalled or dead host into a failed assertion.
 */
const int HOST_CONTROL_REPLY_TIMEOUT_MS = 10000;

/**
 * @brief Upper bound on bytes accepted while assembling one reply line.
 *
 * The longest reply, `calls`, is well under a kilobyte, so reaching the cap means the stream is
 * not the host's replies; same value as HOST_READINESS_MAX_BYTES.
 */
const std::size_t HOST_CONTROL_MAX_REPLY_BYTES = 4096;

/** @brief Prefix every diagnostic line this harness prints carries. */
const char *const TRACE_PREFIX = "[CecL2TestEnvironment] ";

/**
 * @brief Exit codes of a child that failed before becoming the host.
 *
 * They lie outside the host's own exit codes, so a status shows whether the host was reached.
 */
const int CHILD_EXIT_PRE_EXEC_FAILED = 120;
const int CHILD_EXIT_EXEC_FAILED     = 121;

/**
 * @brief How the wait for the host's readiness token ended.
 *
 * Each failure is distinct because each points to a different cause in a CI log.
 */
enum class ReadinessOutcome {
    Ready,              /**< @brief The token arrived, matched verbatim, and the host is serving. */
    TimedOut,           /**< @brief The bound expired with no token; the host is still alive.     */
    ClosedWithoutToken, /**< @brief The write end closed with no token; the host exited.          */
    TokenMismatch,      /**< @brief A non-token line, or the byte cap reached with no newline.    */
    PipeError           /**< @brief The descriptor itself failed, which is none of the above.     */
};

/**
 * @brief Writes a buffer to a descriptor in full, tolerating short and interrupted writes.
 *
 * It neither allocates nor locks, so forked children use it for launch diagnostics and probe
 * handshakes.
 *
 * @param [in] fd                         - Descriptor to write to
 * @param [in] data                       - Buffer to write. Must not be null
 * @param [in] length                     - Number of bytes to write
 *
 * @return None
 *
 * @warning A failed write returns silently; the caller chooses its next step.
 */
void writeRawFully(int fd, const char *data, std::size_t length)
{
    std::size_t written = 0;

    while (written < length) {
        const ssize_t result = ::write(fd, data + written, length - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        if (result == 0) {
            /* No progress on a positive request; retrying would spin. */
            return;
        }

        written += static_cast<std::size_t>(result);
    }
}

/**
 * @brief Highest descriptor the close() fallback sweeps when close_range() is unavailable.
 *
 * Bounds the loop where RLIMIT_NOFILE can be 1048576; descriptors above the cap would survive,
 * but glibc 2.34 and kernel 5.9 provide close_range(), so the fallback is not normally taken.
 */
const unsigned int DESCRIPTOR_SWEEP_FALLBACK_CAP = 65536U;

/**
 * @brief Closes an inclusive span of descriptor numbers, whether or not any of them is open.
 *
 * Post-fork safe: syscalls only, no allocation or lock. Errors, mostly EBADF, are ignored.
 *
 * @param [in] low                        - First descriptor number to close
 * @param [in] high                       - Last descriptor number to close, inclusive; ~0U for all
 *
 * @return None
 *
 * @see closeInheritedDescriptorsExcept(), DESCRIPTOR_SWEEP_FALLBACK_CAP
 */
void closeDescriptorRange(unsigned int low, unsigned int high)
{
    if (low > high) {
        return;
    }

#if defined(__GLIBC__) && ((__GLIBC__ > 2) || ((__GLIBC__ == 2) && (__GLIBC_MINOR__ >= 34)))
    if (::close_range(low, high, 0) == 0) {
        return;
    }
    /* ENOSYS on a kernel older than 5.9; fall through to the loop below. */
#endif

    unsigned int last = high;

    if (last > DESCRIPTOR_SWEEP_FALLBACK_CAP) {
        last = DESCRIPTOR_SWEEP_FALLBACK_CAP;

        struct rlimit descriptorLimit;

        if ((::getrlimit(RLIMIT_NOFILE, &descriptorLimit) == 0) &&
            (descriptorLimit.rlim_cur != RLIM_INFINITY) &&
            (descriptorLimit.rlim_cur < static_cast<rlim_t>(DESCRIPTOR_SWEEP_FALLBACK_CAP))) {
            last = static_cast<unsigned int>(descriptorLimit.rlim_cur);
        }
    }

    for (unsigned int fd = low; fd <= last; ++fd) {
        ::close(static_cast<int>(fd));
    }
}

/**
 * @brief Closes every descriptor above the standard streams except the three named.
 *
 * Keeps unrelated runner descriptors out of the host, where an inherited pipe write end would
 * stop the death channel ever reporting end of file. Post-fork safe.
 *
 * @param [in] keepFirst                  - A descriptor the child must keep, or negative for none
 * @param [in] keepSecond                 - A second such descriptor, or negative for none
 * @param [in] keepThird                  - A third such descriptor, or negative for none
 *
 * @return None
 *
 * @warning Call only in a forked child; in the parent it would close descriptors still in use.
 * @see closeDescriptorRange(), startFakeServiceHost()
 */
void closeInheritedDescriptorsExcept(int keepFirst, int keepSecond, int keepThird)
{
    int keep[3] = { keepFirst, keepSecond, keepThird };

    /* Fixed three-element sort (no allocation after fork), so the sweep becomes the gaps
     * between kept numbers, which close_range() can close. */
    for (int pass = 0; pass < 2; ++pass) {
        for (int index = 0; index < (2 - pass); ++index) {
            if (keep[index] > keep[index + 1]) {
                const int swapped   = keep[index];
                keep[index]         = keep[index + 1];
                keep[index + 1]     = swapped;
            }
        }
    }

    unsigned int low = static_cast<unsigned int>(STDERR_FILENO) + 1U;

    for (int index = 0; index < 3; ++index) {
        if (keep[index] < 0) {
            continue;
        }

        const unsigned int kept = static_cast<unsigned int>(keep[index]);

        if (kept < low) {
            /* A duplicate, or a number at or below the standard streams: nothing to sweep. */
            continue;
        }

        closeDescriptorRange(low, kept - 1U);
        low = kept + 1U;
    }

    closeDescriptorRange(low, ~0U);
}

/**
 * @brief Renders a wait status as a phrase naming how a process ended.
 *
 * The exit status is reported as a raw number; the host's own trace names the step it reached.
 *
 * @param [in] status                     - Status filled in by waitpid()
 *
 * @return std::string                            - Description of how the process ended
 *
 * @see reapChildBlocking()
 */
std::string describeWaitStatus(int status)
{
    if (WIFEXITED(status)) {
        return "exited with status " + std::to_string(WEXITSTATUS(status));
    }

    if (WIFSIGNALED(status)) {
        return "was killed by signal " + std::to_string(WTERMSIG(status));
    }

    return "changed state in an unexpected way (raw wait status " + std::to_string(status) + ")";
}

/**
 * @brief Reaps a child process, blocking until it has been collected.
 *
 * Callers reach it only after an observed exit or SIGKILL, so the block is short by construction.
 *
 * @param [in]  childPid                  - Pid of the child to collect
 * @param [out] description               - Receives how it ended, or why it could not be reaped
 *
 * @return bool                                   - Whether the process was collected
 * @retval true                                   - Reaped; description names how it ended
 * @retval false                                  - waitpid() failed; description says why
 *
 * @pre childPid names a live or exited child of this process.
 * @see describeWaitStatus(), terminateAndReapChildProcess()
 */
bool reapChildBlocking(pid_t childPid, std::string &description)
{
    int status = 0;
    pid_t reaped = -1;

    do {
        reaped = ::waitpid(childPid, &status, 0);
    } while (reaped < 0 && errno == EINTR);

    if (reaped < 0) {
        description = std::string("could not be reaped: ") + std::strerror(errno);
        return false;
    }

    description = describeWaitStatus(status);
    return true;
}

/* SIGPIPE is ignored for the whole test environment, so a control write to a vanished host
 * returns EPIPE instead of killing the runner before TearDown can reap the host. */

/**
 * @brief Stops a write to a reader-less pipe from terminating this runner.
 *
 * Sets SIGPIPE to SIG_IGN and saves the previous disposition; idempotent, so the saved
 * original is never overwritten.
 *
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on success
 *
 * @return bool                                   - Whether SIGPIPE is now ignored
 * @retval true                                   - It is; a write to a closed pipe fails with EPIPE
 * @retval false                                  - sigaction() refused; the run must not proceed
 *
 * @see restoreBrokenPipeSignalDisposition(), writeControlCommand()
 */
bool ignoreBrokenPipeSignal(std::string &failureDetail)
{
    if (g_sigpipeDispositionReplaced) {
        return true;
    }

    struct sigaction ignoreAction;
    std::memset(&ignoreAction, 0, sizeof(ignoreAction));
    ignoreAction.sa_handler = SIG_IGN;
    ::sigemptyset(&ignoreAction.sa_mask);
    ignoreAction.sa_flags = 0;

    if (::sigaction(SIGPIPE, &ignoreAction, &g_previousSigpipeAction) != 0) {
        failureDetail = std::string("SIGPIPE could not be ignored: ") + std::strerror(errno) +
                        ". Its default disposition terminates this process, so a host that closed "
                        "its control descriptor would kill this runner at the write() rather than "
                        "reporting EPIPE - no diagnostic recorded, and no teardown to signal and "
                        "reap the host, which would then outlive the run holding the production "
                        "service name";
        return false;
    }

    g_sigpipeDispositionReplaced = true;

    std::cout << TRACE_PREFIX << "SIGPIPE is ignored for the lifetime of this test environment, so a "
                 "write to the host's control descriptor after the host has gone reports EPIPE and "
                 "fails one assertion instead of terminating this runner and orphaning the host"
              << std::endl;
    return true;
}

/**
 * @brief Puts back the SIGPIPE disposition this harness found on entry.
 *
 * Idempotent and a no-op when nothing was replaced, so it is safe after a failed SetUp.
 *
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on success
 *
 * @return bool                                   - Whether the original disposition is back
 * @retval true                                   - It is, or there was nothing to restore
 * @retval false                                  - sigaction() refused; SIGPIPE stays ignored
 *
 * @post Nothing that runs afterwards inherits a disposition chosen by this harness.
 * @see ignoreBrokenPipeSignal()
 */
bool restoreBrokenPipeSignalDisposition(std::string &failureDetail)
{
    if (!g_sigpipeDispositionReplaced) {
        return true;
    }

    if (::sigaction(SIGPIPE, &g_previousSigpipeAction, nullptr) != 0) {
        failureDetail = std::string("the original SIGPIPE disposition could not be restored: ") +
                        std::strerror(errno) +
                        ". SIGPIPE stays ignored for whatever runs after this test environment, "
                        "which is a change this harness made and did not announce";
        return false;
    }

    g_sigpipeDispositionReplaced = false;

    std::cout << TRACE_PREFIX << "The SIGPIPE disposition in force before this run has been restored"
              << std::endl;
    return true;
}

/**
 * @brief Launches the fake service host as a child process with readiness and control pipes.
 *
 * The child leads its own process group and inherits only its three named pipe ends and stdio.
 *
 * @param [in]  hostPath                  - Path of the host binary, from CEC_FAKE_AIDL_HOST_PATH
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on success
 *
 * @return bool                                   - Whether the host was launched
 * @retval true                                   - Started and must be reaped; g_host* state set
 * @retval false                                  - Bad binary, or pipe or fork failed; nothing open
 *
 * @warning Success does not mean the service is published; awaitHostReadiness() decides that.
 * @see awaitHostReadiness(), terminateAndReapFakeServiceHost()
 */
bool startFakeServiceHost(const std::string &hostPath, std::string &failureDetail)
{
    /* Checked before the fork so a bad path yields a sentence naming it; the child still
     * handles its own exec failure, because this check cannot be conclusive. */
    if (::access(hostPath.c_str(), X_OK) != 0) {
        failureDetail = renderUntrustedValue(hostPath) + ", named by " + HOST_PATH_VARIABLE +
                        ", is not an executable file (" + std::strerror(errno) +
                        "). The build sets this variable to the fake_hdmi_cec_aidl_host "
                        "binary it built alongside this runner; a stale, unbuilt or "
                        "hand-edited value is the usual cause";
        return false;
    }

    /* O_CLOEXEC sets close-on-exec atomically at creation, so a program that another thread
     * forks and execs concurrently cannot inherit either end. */
    int readinessPipe[2] = { -1, -1 };
    if (::pipe2(readinessPipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("the readiness pipe could not be created: ") +
                        std::strerror(errno);
        return false;
    }

    const int readFd  = readinessPipe[0];
    const int writeFd = readinessPipe[1];

    /* Control pipe: parent writes, child reads. Observation pipe: child writes, parent reads.
     * Any failure closes everything already open. */
    int controlPipe[2] = { -1, -1 };
    if (::pipe2(controlPipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("the host control pipe could not be created: ") +
                        std::strerror(errno);
        ::close(readFd);
        ::close(writeFd);
        return false;
    }

    int observePipe[2] = { -1, -1 };
    if (::pipe2(observePipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("the host observation pipe could not be created: ") +
                        std::strerror(errno);
        ::close(readFd);
        ::close(writeFd);
        ::close(controlPipe[0]);
        ::close(controlPipe[1]);
        return false;
    }

    const int controlChildReadFd    = controlPipe[0];
    const int controlParentWriteFd  = controlPipe[1];
    const int observeParentReadFd   = observePipe[0];
    const int observeChildWriteFd   = observePipe[1];

    /* argv is the path alone: the host takes its whole configuration from the environment. */
    std::string hostPathForChild = hostPath;
    char *const childArgv[] = { const_cast<char *>(hostPathForChild.c_str()), nullptr };

    /* Child environment: ours minus any inherited descriptor variables, plus all three new ones.
     * Every string is appended before pointers are taken, so vector growth cannot dangle them. */
    const std::string readyFdPrefix     = std::string(HOST_READY_FD_VARIABLE) + "=";
    const std::string readyFdAssignment = readyFdPrefix + std::to_string(writeFd);

    const std::string controlFdPrefix     = std::string(HOST_CONTROL_FD_VARIABLE) + "=";
    const std::string controlFdAssignment = controlFdPrefix + std::to_string(controlChildReadFd);

    const std::string observeFdPrefix     = std::string(HOST_OBSERVE_FD_VARIABLE) + "=";
    const std::string observeFdAssignment = observeFdPrefix + std::to_string(observeChildWriteFd);

    std::vector<std::string> childEnvironmentEntries;
    for (char **cursor = ::environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
        if (std::strncmp(*cursor, readyFdPrefix.c_str(), readyFdPrefix.size()) == 0) {
            continue;
        }
        if (std::strncmp(*cursor, controlFdPrefix.c_str(), controlFdPrefix.size()) == 0) {
            continue;
        }
        if (std::strncmp(*cursor, observeFdPrefix.c_str(), observeFdPrefix.size()) == 0) {
            continue;
        }
        childEnvironmentEntries.push_back(*cursor);
    }
    childEnvironmentEntries.push_back(readyFdAssignment);
    childEnvironmentEntries.push_back(controlFdAssignment);
    childEnvironmentEntries.push_back(observeFdAssignment);

    std::vector<char *> childEnvironment;
    childEnvironment.reserve(childEnvironmentEntries.size() + 1);
    for (std::string &entry : childEnvironmentEntries) {
        childEnvironment.push_back(const_cast<char *>(entry.c_str()));
    }
    childEnvironment.push_back(nullptr);

    const pid_t child = ::fork();

    if (child < 0) {
        failureDetail = std::string("the host process could not be forked: ") +
                        std::strerror(errno);
        ::close(readFd);
        ::close(writeFd);
        ::close(controlChildReadFd);
        ::close(controlParentWriteFd);
        ::close(observeParentReadFd);
        ::close(observeChildWriteFd);
        return false;
    }

    if (child == 0) {
        /* In the child: nothing before execve() allocates or locks; messages are literals, and
         * _exit() avoids flushing the parent's stdio buffers a second time. */
        ::close(readFd);

        /* Close the parent's channel ends, so the host sees end of file when the parent closes
         * its own control write end. */
        ::close(controlParentWriteFd);
        ::close(observeParentReadFd);

        /* Lead a new process group so teardown can signal the host's descendants; the parent
         * repeats the call, and a failure here refuses the launch. */
        if (::setpgid(0, 0) != 0) {
            static const char groupMessage[] =
                "[FakeHdmiCecAidlHost:child] the child could not be made a process group leader; "
                "its descendants would have been unreachable at teardown while the only "
                "group-wide signal available addressed the runner itself, so the launch is "
                "refused instead\n";
            writeRawFully(STDERR_FILENO, groupMessage, sizeof(groupMessage) - 1);
            ::_exit(CHILD_EXIT_PRE_EXEC_FAILED);
        }

        if (::fcntl(writeFd, F_SETFD, 0) != 0) {
            static const char preExecMessage[] =
                "[FakeHdmiCecAidlHost:child] the readiness descriptor could not be made "
                "inheritable; the host would have had nothing to signal on\n";
            writeRawFully(STDERR_FILENO, preExecMessage, sizeof(preExecMessage) - 1);
            ::_exit(CHILD_EXIT_PRE_EXEC_FAILED);
        }

        /* Clear FD_CLOEXEC on the two channel ends the host is told about, or they would not
         * survive the exec and the host would refuse to start. */
        if (::fcntl(controlChildReadFd, F_SETFD, 0) != 0) {
            static const char controlMessage[] =
                "[FakeHdmiCecAidlHost:child] the control descriptor could not be made "
                "inheritable; the host would have had no commands to read\n";
            writeRawFully(STDERR_FILENO, controlMessage, sizeof(controlMessage) - 1);
            ::_exit(CHILD_EXIT_PRE_EXEC_FAILED);
        }

        if (::fcntl(observeChildWriteFd, F_SETFD, 0) != 0) {
            static const char observeMessage[] =
                "[FakeHdmiCecAidlHost:child] the observation descriptor could not be made "
                "inheritable; the host would have had nowhere to reply\n";
            writeRawFully(STDERR_FILENO, observeMessage, sizeof(observeMessage) - 1);
            ::_exit(CHILD_EXIT_PRE_EXEC_FAILED);
        }

        /* Close every other inherited descriptor so only the three named in the environment reach
         * the host; kept next to the F_SETFD calls because both act on the same set. */
        closeInheritedDescriptorsExcept(writeFd, controlChildReadFd, observeChildWriteFd);

        ::execve(childArgv[0], childArgv, childEnvironment.data());

        /* Reached only if execve() failed; the parent sees end of file plus this exit status. */
        static const char execMessage[] =
            "[FakeHdmiCecAidlHost:child] the fake service host binary could not be "
            "executed; the path named by CEC_FAKE_AIDL_HOST_PATH is not runnable\n";
        writeRawFully(STDERR_FILENO, execMessage, sizeof(execMessage) - 1);
        ::_exit(CHILD_EXIT_EXEC_FAILED);
    }

    /* Parent half of the both-sides setpgid() idiom. EACCES (child already exec'd) and ESRCH
     * (child already exited) are expected; anything else is traced. */
    if (::setpgid(child, child) != 0) {
        const int setpgidError = errno;

        if ((setpgidError != EACCES) && (setpgidError != ESRCH)) {
            std::cout << TRACE_PREFIX << "The parent's setpgid() for pid "
                      << static_cast<long>(child) << " failed with an unexpected error ("
                      << std::strerror(setpgidError)
                      << "); the child's own call is checked below and teardown falls back to "
                         "signalling the single pid if it did not take effect" << std::endl;
        }
    }

    /* Read the group back: a group is signalled later only if it equals the child pid and
     * differs from this runner's group; otherwise -1 and the single pid is signalled. */
    const pid_t observedGroup = ::getpgid(child);
    const pid_t runnerGroup   = ::getpgrp();

    if ((observedGroup == child) && (observedGroup != runnerGroup)) {
        g_hostProcessGroupId = observedGroup;
    } else {
        g_hostProcessGroupId = -1;

        std::cout << TRACE_PREFIX << "The launched host's process group could not be confirmed "
                     "(getpgid(" << static_cast<long>(child) << ") reported "
                  << static_cast<long>(observedGroup) << ", this runner's group is "
                  << static_cast<long>(runnerGroup)
                  << "); teardown will signal the single pid rather than a group it cannot "
                     "vouch for, so a process the host starts could outlive this run"
                  << std::endl;
    }

    /* Close the parent's copy of the readiness write end, so a dead child yields end of file
     * instead of a full-timeout wait. */
    ::close(writeFd);

    /* Likewise close the child's channel ends, so end of file is meaningful in both directions. */
    ::close(controlChildReadFd);
    ::close(observeChildWriteFd);

    g_hostPid              = child;
    g_hostReadinessReadFd  = readFd;
    g_hostControlWriteFd   = controlParentWriteFd;
    g_hostObserveReadFd    = observeParentReadFd;

    std::cout << TRACE_PREFIX << "Launched the out-of-process fake HDMI CEC AIDL service host "
              << renderUntrustedValue(hostPath) << " as pid " << static_cast<long>(child)
              << ", signalling readiness on "
              << HOST_READY_FD_VARIABLE << "=" << writeFd << ", reading commands from "
              << HOST_CONTROL_FD_VARIABLE << "=" << controlChildReadFd << " and replying on "
              << HOST_OBSERVE_FD_VARIABLE << "=" << observeChildWriteFd << std::endl;
    return true;
}

/**
 * @brief Blocks until the host reports ready, or the bound expires.
 *
 * poll() waits on the readiness pipe against a steady_clock deadline, so a healthy host is seen
 * at once and an interrupted poll() cannot extend the bound.
 *
 * @param [out] observed                  - Receives the first line, partial bytes, or errno text
 *
 * @return ReadinessOutcome                       - How the wait ended
 * @retval ReadinessOutcome::Ready                - The token arrived and matched verbatim
 * @retval ReadinessOutcome::TimedOut             - The bound expired; the host has not published
 * @retval ReadinessOutcome::ClosedWithoutToken   - The write end closed with no token; host exited
 * @retval ReadinessOutcome::TokenMismatch        - A non-token line or an overlong unterminated one
 * @retval ReadinessOutcome::PipeError            - poll() or read() failed on the descriptor
 *
 * @pre startFakeServiceHost() succeeded and this process closed its copy of the write end.
 * @warning Anything but Ready must fail the run; continuing would report legacy results as AIDL.
 * @see startFakeServiceHost(), HOST_READINESS_TIMEOUT_MS, HOST_READINESS_TOKEN
 */
ReadinessOutcome awaitHostReadiness(std::string &observed)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(HOST_READINESS_TIMEOUT_MS);

    std::string received;

    for (;;) {
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            observed = received;
            return ReadinessOutcome::TimedOut;
        }

        struct pollfd watched;
        watched.fd      = g_hostReadinessReadFd;
        watched.events  = POLLIN;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remaining.count()));

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            observed = std::string("poll() on the readiness pipe failed: ") +
                       std::strerror(errno);
            return ReadinessOutcome::PipeError;
        }

        if (ready == 0) {
            observed = received;
            return ReadinessOutcome::TimedOut;
        }

        /* POLLHUP may accompany buffered data, so read() decides: bytes are consumed first and
         * end of file is concluded only from a zero-length read. */
        char chunk[128];
        const ssize_t got = ::read(g_hostReadinessReadFd, chunk, sizeof(chunk));

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            observed = std::string("read() on the readiness pipe failed: ") +
                       std::strerror(errno);
            return ReadinessOutcome::PipeError;
        }

        if (got == 0) {
            observed = received;
            return ReadinessOutcome::ClosedWithoutToken;
        }

        received.append(chunk, static_cast<std::size_t>(got));

        /* Match only a whole line, so a truncated write is never taken as readiness. */
        const std::size_t terminator = received.find('\n');
        if (terminator != std::string::npos) {
            const std::string firstLine = received.substr(0, terminator + 1);
            observed = firstLine;
            return (firstLine == HOST_READINESS_TOKEN) ? ReadinessOutcome::Ready
                                                       : ReadinessOutcome::TokenMismatch;
        }

        if (received.size() >= HOST_READINESS_MAX_BYTES) {
            observed = received;
            return ReadinessOutcome::TokenMismatch;
        }
    }
}

/**
 * @brief Bytes read past the end of the last reply line, held for the next request.
 *
 * Keeps framing intact however read() splits the stream; empty in a healthy session.
 */
std::string controlReplyResidual;

/**
 * @brief Serialises control requests against each other.
 *
 * No case issues concurrent requests today; the lock keeps the channel in step if one ever does.
 */
std::mutex controlRequestMutex;

/**
 * @brief Writes one command line to the host's control descriptor, bounded by a deadline.
 *
 * poll() for writability turns a host that stopped reading into a reported failure. The
 * descriptor is a parameter so the EPIPE arm can be driven on a pipe whose reader has gone.
 *
 * @param [in]  line                      - Command text without its terminator
 * @param [in]  controlWriteFd            - Open write descriptor; normally g_hostControlWriteFd
 * @param [in]  deadline                  - Monotonic instant after which this gives up
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on success
 *
 * @return bool                                   - Whether the line and its terminator were written
 * @retval true                                   - The host has the command in its pipe
 * @retval false                                  - Timeout, reader closed, or descriptor failure
 *
 * @see performHostControlRequest(), cecL2ProveEpipeDiagnosticAndChildReaping()
 */
bool writeControlCommand(const std::string &line, int controlWriteFd,
                         const std::chrono::steady_clock::time_point &deadline,
                         std::string &failureDetail)
{
    const std::string framed = line + "\n";
    std::size_t written = 0;

    while (written < framed.size()) {
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            failureDetail = "the command \"" + line + "\" could not be written to the host within " +
                            std::to_string(HOST_CONTROL_REPLY_TIMEOUT_MS) +
                            " ms; the host is not reading its control descriptor, so it has stopped "
                            "serving the channel";
            return false;
        }

        struct pollfd watched;
        watched.fd      = controlWriteFd;
        watched.events  = POLLOUT;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remaining.count()));

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            failureDetail = std::string("poll() on the host control pipe failed: ") +
                            std::strerror(errno);
            return false;
        }

        if (ready == 0) {
            continue; /* Re-checked against the deadline at the top of the loop. */
        }

        /* POLLERR (reader closed) falls through to write(), which reports EPIPE; that is sound
         * only because ignoreBrokenPipeSignal() is in force for the whole environment. */
        const ssize_t result = ::write(controlWriteFd, framed.data() + written,
                                       framed.size() - written);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            failureDetail = "the command \"" + line + "\" could not be written to the host: " +
                            std::strerror(errno) +
                            (errno == EPIPE ? ". EPIPE means the host has closed its control "
                                              "descriptor or exited"
                                            : "");
            return false;
        }

        if (result == 0) {
            failureDetail = "the host control pipe accepted none of \"" + line +
                            "\" on a positive-length write, so retrying would spin";
            return false;
        }

        written += static_cast<std::size_t>(result);
    }

    return true;
}

/**
 * @brief Reads exactly one reply line from the host's observation descriptor, bounded.
 *
 * Starts from bytes a previous request read past its terminator, and reports end of file (host
 * exited) apart from timeout (host alive but silent).
 *
 * @param [in]  command                   - Command this reply answers, for the diagnostic
 * @param [in]  deadline                  - Monotonic instant after which this gives up
 * @param [out] reply                     - Receives the line without its terminator, or untouched
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on success
 *
 * @return bool                                   - Whether one complete reply line was read
 * @retval true                                   - reply holds it, terminator stripped
 * @retval false                                  - Timeout, end of file, descriptor failure or cap
 *
 * @pre g_hostObserveReadFd is open.
 * @see performHostControlRequest()
 */
bool readControlReply(const std::string &command,
                      const std::chrono::steady_clock::time_point &deadline,
                      std::string &reply,
                      std::string &failureDetail)
{
    std::string received = controlReplyResidual;
    controlReplyResidual.clear();

    for (;;) {
        const std::size_t terminator = received.find('\n');

        if (terminator != std::string::npos) {
            std::string line = received.substr(0, terminator);
            controlReplyResidual = received.substr(terminator + 1);

            /* Defensive rather than expected: the host writes bare newlines. */
            while (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            reply = line;
            return true;
        }

        if (received.size() >= HOST_CONTROL_MAX_REPLY_BYTES) {
            failureDetail = "the host sent " + std::to_string(received.size()) +
                            " bytes with no line terminator in reply to " +
                            renderUntrustedValue(command) +
                            ", so the observation descriptor is not carrying this protocol's "
                            "replies. First bytes: " + renderUntrustedValue(received);
            return false;
        }

        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            failureDetail = "the host did not answer " + renderUntrustedValue(command) + " within " +
                            std::to_string(HOST_CONTROL_REPLY_TIMEOUT_MS) +
                            " ms. It is still holding the observation pipe open, so it is alive and "
                            "not answering rather than gone" +
                            (received.empty() ? std::string()
                                              : (". Partial reply received: " +
                                                 renderUntrustedValue(received)));
            return false;
        }

        struct pollfd watched;
        watched.fd      = g_hostObserveReadFd;
        watched.events  = POLLIN;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remaining.count()));

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            failureDetail = std::string("poll() on the host observation pipe failed: ") +
                            std::strerror(errno);
            return false;
        }

        if (ready == 0) {
            continue; /* Re-checked against the deadline at the top of the loop. */
        }

        char chunk[256];
        const ssize_t got = ::read(g_hostObserveReadFd, chunk, sizeof(chunk));

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            failureDetail = std::string("read() on the host observation pipe failed: ") +
                            std::strerror(errno);
            return false;
        }

        if (got == 0) {
            failureDetail = "the host closed the observation pipe without answering " +
                            renderUntrustedValue(command) +
                            ", so it has exited. Its own trace on standard output, prefixed "
                            "[FakeHdmiCecAidlHost], names the step it reached, and its exit status "
                            "is reported by this harness's teardown" +
                            (received.empty() ? std::string()
                                              : (". Partial reply received: " +
                                                 renderUntrustedValue(received)));
            return false;
        }

        received.append(chunk, static_cast<std::size_t>(got));
    }
}

/**
 * @brief Sends one command to the fake service host and returns its single reply line.
 *
 * The whole client half of the protocol, so its framing cannot drift between functions.
 *
 * @param [in]  command                   - Command text without a terminator, e.g. "sent-count"
 * @param [out] reply                     - Receives the reply line without its terminator
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether one reply was exchanged
 * @retval true                                   - reply holds the answer, possibly an "ERR " line
 * @retval false                                  - No answer was obtained; failureDetail says why
 *
 * @warning An "ERR " reply returns true; judging it is the calling test's job.
 * @see writeControlCommand(), readControlReply()
 */
bool performHostControlRequest(const std::string &command, std::string &reply,
                               std::string &failureDetail)
{
    std::lock_guard<std::mutex> guard(controlRequestMutex);

    if ((g_hostControlWriteFd < 0) || (g_hostObserveReadFd < 0)) {
        failureDetail = "no control and observation channel is open, so the command " +
                        renderUntrustedValue(command) +
                        " cannot be sent. The channel exists only where this harness launched the "
                        "out-of-process fake service host, which is CEC_TEST_AIDL_MODE=remote alone; "
                        "on the legacy invocation there is no host and nothing to ask";
        return false;
    }

    /* Validate before writing: an empty, blank or multi-line command would break the
     * one-command-one-reply framing rather than produce a refusal. */
    if (command.empty()) {
        failureDetail = "an empty control command was requested; the host answers a blank line with "
                        "no reply at all, so this would wait out its bound for an answer that is "
                        "never coming";
        return false;
    }

    if (command.find_first_not_of(" \t") == std::string::npos) {
        failureDetail = "the control command " + renderUntrustedValue(command) +
                        " is whitespace only; the host treats such a line as \"not a command\" "
                        "and sends no reply";
        return false;
    }

    if (command.find('\n') != std::string::npos || command.find('\r') != std::string::npos) {
        failureDetail = "the control command " + renderUntrustedValue(command) + " contains a line "
                        "terminator. One request is one line: an embedded terminator would send two "
                        "commands and read one reply, and every later request would return the "
                        "previous one's answer";
        return false;
    }

    if (command.size() >= HOST_CONTROL_MAX_REPLY_BYTES) {
        failureDetail = "the control command is " + std::to_string(command.size()) +
                        " bytes, which the host answers with ERR command-too-long and discards "
                        "unparsed";
        return false;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(HOST_CONTROL_REPLY_TIMEOUT_MS);

    if (!writeControlCommand(command, g_hostControlWriteFd, deadline, failureDetail)) {
        return false;
    }

    if (!readControlReply(command, deadline, reply, failureDetail)) {
        return false;
    }

    /* Every reply begins "OK " or "ERR "; anything else means the channel is out of step. */
    if (reply.compare(0, 3, "OK ") != 0 && reply.compare(0, 4, "ERR ") != 0) {
        failureDetail = "the host answered " + renderUntrustedValue(command) + " with " +
                        renderUntrustedValue(reply) +
                        ", which begins with neither \"OK \" nor \"ERR \". Every reply in the "
                        "protocol does, so the observation descriptor is out of step with the "
                        "commands being sent";
        return false;
    }

    return true;
}

/**
 * @brief Closes this process's two ends of the control and observation channel.
 *
 * Idempotent and safe after a partial launch; closing the write end also stops a serving host.
 *
 * @return None
 *
 * @post Both descriptors are -1, so performHostControlRequest() reports "no channel".
 * @see terminateAndReapFakeServiceHost()
 */
void closeHostControlChannel()
{
    if (g_hostControlWriteFd >= 0) {
        ::close(g_hostControlWriteFd);
        g_hostControlWriteFd = -1;
    }

    if (g_hostObserveReadFd >= 0) {
        ::close(g_hostObserveReadFd);
        g_hostObserveReadFd = -1;
    }

    controlReplyResidual.clear();
}

/**
 * @brief Waits for a child to exit, detected as end of file on a pipe it holds open.
 *
 * Bounded and timer-free, and preferred over waitForChildExitByPolling() for any child holding
 * such a pipe, such as the host's readiness pipe or the broken-pipe seam's handshake pipe.
 *
 * @param [in] pipeReadFd                 - Read end of the child's death pipe; negative for none
 * @param [in] timeoutMs                  - Longest time to wait, in milliseconds
 *
 * @return bool                                   - Whether the child was observed to exit
 * @retval true                                   - End of file arrived; a reap returns at once
 * @retval false                                  - Timeout, descriptor failure, or no descriptor
 *
 * @see terminateAndReapChildProcess(), waitForChildExitByPolling()
 */
bool waitForChildExitViaPipeEof(int pipeReadFd, int timeoutMs)
{
    if (pipeReadFd < 0) {
        return false;
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            return false;
        }

        struct pollfd watched;
        watched.fd      = pipeReadFd;
        watched.events  = POLLIN;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remaining.count()));

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (ready == 0) {
            return false;
        }

        char drain[128];
        const ssize_t got = ::read(pipeReadFd, drain, sizeof(drain));

        if (got == 0) {
            return true;
        }

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        /* Bytes rather than end of file: no part of any contract, so drain and keep waiting. */
    }
}

/**
 * @brief How long to sleep between two WNOHANG waits while polling for a child's exit.
 *
 * Ten milliseconds bounds how late an exit is noticed without busy-looping; it applies only to a
 * child that has no end-of-file death channel.
 */
const int CHILD_EXIT_POLL_INTERVAL_MS = 10;

/**
 * @brief How a bounded poll for a child's exit ended.
 *
 * Three outcomes because the caller acts on each differently: done, escalate, or report failure.
 */
enum class PolledChildExit {
    Collected,    /**< @brief Exited within the bound and was reaped here; no further wait is owed. */
    StillRunning, /**< @brief The bound expired with the child alive; it has not been reaped.       */
    WaitFailed    /**< @brief waitpid() refused, so this process cannot collect that pid.           */
};

/**
 * @brief Waits, bounded, for a child to exit by polling waitpid(), and reaps it if it does.
 *
 * The fallback for a child with no death pipe, keeping terminate-and-reap bounded even for a
 * child that handles SIGTERM. A WNOHANG observation is itself the reap, hence Collected.
 *
 * @param [in]  childPid                  - Pid of the child to wait for. Must be positive
 * @param [in]  timeoutMs                 - Longest wait in milliseconds; non-positive checks once
 * @param [out] description               - How it ended, or why it cannot be reaped; else untouched
 *
 * @return PolledChildExit                        - Which of the three outcomes occurred
 * @retval PolledChildExit::Collected             - Exited and reaped; no further waitpid() is owed
 * @retval PolledChildExit::StillRunning          - The bound expired; alive and unreaped
 * @retval PolledChildExit::WaitFailed            - waitpid() refused, in practice with ECHILD
 *
 * @pre childPid is a child of this process that has been, or is about to be, signalled.
 * @see terminateAndReapChildProcess(), waitForChildExitViaPipeEof(), describeWaitStatus()
 */
PolledChildExit waitForChildExitByPolling(pid_t childPid, int timeoutMs,
                                          std::string &description)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        int status = 0;
        const pid_t observed = ::waitpid(childPid, &status, WNOHANG);

        if (observed > 0) {
            /* waitpid() on one positive pid returns that pid, 0 or -1, so the sign suffices. */
            description = describeWaitStatus(status);
            return PolledChildExit::Collected;
        }

        if (observed < 0) {
            if (errno == EINTR) {
                continue;
            }

            /* Same sentence as reapChildBlocking(); in practice ECHILD. */
            description = std::string("could not be reaped: ") + std::strerror(errno);
            return PolledChildExit::WaitFailed;
        }

        /* Zero: still running. The deadline is re-read from the monotonic clock on every pass. */
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            return PolledChildExit::StillRunning;
        }

        /* poll() with no descriptors is this file's sleep, clamped to the time left before the
         * deadline. */
        const long long remainingMs = static_cast<long long>(remaining.count());
        const int sleepMs           = (remainingMs < CHILD_EXIT_POLL_INTERVAL_MS)
                                          ? static_cast<int>(remainingMs)
                                          : CHILD_EXIT_POLL_INTERVAL_MS;

        ::poll(nullptr, 0, sleepMs);
    }
}

/**
 * @brief How long the host's process group may take to empty after its leader is reaped.
 *
 * Short because every member has already had SIGTERM; erring low only kills a lingering
 * descendant slightly earlier, and SIGKILL follows either way.
 */
const int PROCESS_GROUP_SETTLE_MS = 200;

/**
 * @brief How long the process group may take to empty after SIGKILL.
 *
 * A bound on kernel teardown and reaping by the members' parents, not a grace period; a group
 * still populated afterwards is reported as a leak.
 */
const int PROCESS_GROUP_DRAIN_TIMEOUT_MS = 2000;

/**
 * @brief What a probe of a process group found.
 *
 * @see probeProcessGroup(), awaitProcessGroupEmpty()
 */
enum class ProcessGroupState {
    Empty,        /**< @brief No process is in the group: every member has gone and been reaped. */
    Populated,    /**< @brief At least one member remains, alive or a zombie.                    */
    NotPermitted  /**< @brief Members remain that this process is not allowed to signal.         */
};

/**
 * @brief Renders a process group state as a phrase for a diagnostic.
 *
 * @param [in] state                      - State to describe
 *
 * @return std::string                            - The phrase, without leading capital or
 *                                                  trailing punctuation, for embedding
 *
 * @see probeProcessGroup()
 */
std::string describeProcessGroupState(ProcessGroupState state)
{
    switch (state) {
    case ProcessGroupState::Empty:
        return "the group is empty";

    case ProcessGroupState::Populated:
        return "at least one member is still present, alive or unreaped";

    case ProcessGroupState::NotPermitted:
        return "members remain that this process is not permitted to signal, so they cannot be "
               "ended from here";
    }

    return "the group is in a state this harness does not recognise";
}

/**
 * @brief Asks whether any process is still in a group, without signalling anything.
 *
 * Sends the null signal to the negated group id; an unrecognised errno reports Populated.
 *
 * @param [in] groupId                    - Process group id, positive; negated internally
 *
 * @return ProcessGroupState                      - What the probe found
 *
 * @warning Pass the group id itself, not its negation.
 *
 * @see awaitProcessGroupEmpty(), describeProcessGroupState()
 */
ProcessGroupState probeProcessGroup(pid_t groupId)
{
    if (groupId <= 0) {
        /* Not a group this harness owns: NotPermitted, as only a confirmed empty group is Empty. */
        return ProcessGroupState::NotPermitted;
    }

    if (::kill(-groupId, 0) == 0) {
        return ProcessGroupState::Populated;
    }

    if (errno == ESRCH) {
        return ProcessGroupState::Empty;
    }

    if (errno == EPERM) {
        return ProcessGroupState::NotPermitted;
    }

    return ProcessGroupState::Populated;
}

/**
 * @brief Waits, within a bound, for a process group to have no members left.
 *
 * Polls probeProcessGroup() every CHILD_EXIT_POLL_INTERVAL_MS against one monotonic deadline.
 *
 * @param [in]  groupId                   - Process group id to watch, positive
 * @param [in]  boundMs                   - How long to wait, in milliseconds
 * @param [out] finalState                - Receives the last state observed
 *
 * @return bool                                   - Whether the group is empty
 * @retval true                                   - Every member has gone and been collected
 * @retval false                                  - The bound expired; finalState says why
 *
 * @see probeProcessGroup(), CHILD_EXIT_POLL_INTERVAL_MS
 */
bool awaitProcessGroupEmpty(pid_t groupId, int boundMs, ProcessGroupState &finalState)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(boundMs);

    for (;;) {
        finalState = probeProcessGroup(groupId);

        if (finalState == ProcessGroupState::Empty) {
            return true;
        }

        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            return false;
        }

        const long long remainingMs = static_cast<long long>(remaining.count());
        const int sleepMs           = (remainingMs < CHILD_EXIT_POLL_INTERVAL_MS)
                                          ? static_cast<int>(remainingMs)
                                          : CHILD_EXIT_POLL_INTERVAL_MS;

        ::poll(nullptr, 0, sleepMs);
    }
}

/**
 * @brief Signals a child, waits for it within a bound, escalates if it stays, and reaps it.
 *
 * SIGTERM, a bounded grace period, SIGKILL if needed, then the reap. With a group, both signals
 * go group-wide and the group must end empty; a group id equal to getpgrp() is refused.
 *
 * @param [in]  childPid                  - Pid of the child to end; must be positive
 * @param [in]  description               - How to name the child in traces and in the outcome
 * @param [in]  exitObservationFd         - Read end of the child's death channel; negative to poll
 * @param [in]  gracePeriodMs             - Time allowed after SIGTERM before SIGKILL
 * @param [in]  childProcessGroupId       - Confirmed group the child leads; non-positive for none
 * @param [out] outcome                   - Receives how the child ended, or why it was not reaped
 *
 * @return bool                                   - Whether it was reaped and its group is empty
 * @retval true                                   - Reaped, and no member of its group remains
 * @retval false                                  - Bad pid, refused wait, or group outlived SIGKILL
 *
 * @warning The caller owns every descriptor; none is closed here.
 *
 * @see reapChildBlocking(), waitForChildExitViaPipeEof(), awaitProcessGroupEmpty()
 */
bool terminateAndReapChildProcess(pid_t childPid, const std::string &description,
                                  int exitObservationFd, int gracePeriodMs,
                                  pid_t childProcessGroupId, std::string &outcome)
{
    if (childPid <= 0) {
        /* Defensive: pid 0 or -1 would signal a whole process group, so refuse instead. */
        outcome = "was not reaped: " + std::to_string(static_cast<long>(childPid)) +
                  " is not a child pid, and signalling it would have addressed a process group "
                  "rather than one child";
        return false;
    }

    const long pidForTrace = static_cast<long>(childPid);

    /* Group mode needs a positive id other than this runner's group: an equal id is what an
     * unconfirmed value looks like, so that case drops to signalling the single pid. */
    const pid_t runnerProcessGroup = ::getpgrp();
    const long groupForTrace       = static_cast<long>(childProcessGroupId);
    bool groupMode                 = false;

    if (childProcessGroupId > 0) {
        if (childProcessGroupId == runnerProcessGroup) {
            std::cout << TRACE_PREFIX << "Refusing to signal process group " << groupForTrace
                      << " for " << description
                      << ": it is this runner's own process group, so a group-wide signal would "
                         "end this process. Signalling the single pid instead, which means a "
                         "process the child started could outlive this run" << std::endl;
        } else {
            groupMode = true;
        }
    }

    if (groupMode) {
        std::cout << TRACE_PREFIX << "Terminating " << description << " pid " << pidForTrace
                  << " and every other member of its process group " << groupForTrace
                  << " with SIGTERM" << std::endl;

        /* The group first; the pid is signalled again below in case the child left its group.
         * A second SIGTERM is harmless, and ESRCH is tolerated in both places. */
        if ((::kill(-childProcessGroupId, SIGTERM) != 0) && (errno != ESRCH)) {
            std::cout << TRACE_PREFIX << "SIGTERM could not be delivered to process group "
                      << groupForTrace << " (" << std::strerror(errno)
                      << "); the direct child is signalled below regardless" << std::endl;
        }
    } else {
        std::cout << TRACE_PREFIX << "Terminating " << description << " pid " << pidForTrace
                  << " with SIGTERM" << std::endl;
    }

    if (::kill(childPid, SIGTERM) != 0) {
        /* Almost always ESRCH: the child already exited, as on the readiness-failure path. */
        std::cout << TRACE_PREFIX << "SIGTERM could not be delivered to pid " << pidForTrace << " ("
                  << std::strerror(errno) << "); it has most likely already exited, and it is "
                  "reaped below regardless" << std::endl;
    }

    /* The bounded wait: pipe end of file given a death channel, else waitpid() polling, which
     * reaps as it observes and so is tracked separately. Both spend gracePeriodMs. */
    bool exitObserved    = false;
    bool collectedByPoll = false;
    bool waitRefused     = false;

    if (exitObservationFd >= 0) {
        exitObserved = waitForChildExitViaPipeEof(exitObservationFd, gracePeriodMs);
    } else {
        switch (waitForChildExitByPolling(childPid, gracePeriodMs, outcome)) {
        case PolledChildExit::Collected:
            exitObserved    = true;
            collectedByPoll = true;
            break;

        case PolledChildExit::StillRunning:
            break;

        case PolledChildExit::WaitFailed:
            /* waitpid() disowned the pid (ECHILD): SIGKILL and the reap are skipped, as the
             * number may have been reused, and the poll's outcome is reported. */
            waitRefused = true;
            break;
        }
    }

    bool reaped = collectedByPoll;

    if (!waitRefused) {
        if (!exitObserved) {
            std::cout << TRACE_PREFIX << "Pid " << pidForTrace << " had not exited "
                      << gracePeriodMs << " ms after SIGTERM; escalating to SIGKILL so it cannot "
                      "outlive this run" << std::endl;

            if (groupMode) {
                ::kill(-childProcessGroupId, SIGKILL);
            }

            ::kill(childPid, SIGKILL);
        }

        /* Blocks only briefly, being reached after an observed exit or SIGKILL. Skipped when the
         * poll already collected the child, as a second waitpid() would fail with ECHILD. */
        if (!reaped) {
            reaped = reapChildBlocking(childPid, outcome);
        }
    }

    /* waitpid() cannot show the child's descendants are gone, so a supplied group is probed,
     * except after a refused wait, when the group is not this harness's to address. */
    bool groupDrained = true;

    if (groupMode && !waitRefused) {
        ProcessGroupState groupState = ProcessGroupState::Populated;

        if (!awaitProcessGroupEmpty(childProcessGroupId, PROCESS_GROUP_SETTLE_MS, groupState)) {
            std::cout << TRACE_PREFIX << "Process group " << groupForTrace << " still had members "
                      << PROCESS_GROUP_SETTLE_MS << " ms after " << description
                      << " was reaped (" << describeProcessGroupState(groupState)
                      << "); escalating to SIGKILL for the whole group, because a process the "
                         "child started is not one waitpid() can collect" << std::endl;

            ::kill(-childProcessGroupId, SIGKILL);

            if (!awaitProcessGroupEmpty(childProcessGroupId, PROCESS_GROUP_DRAIN_TIMEOUT_MS,
                                        groupState)) {
                groupDrained = false;

                outcome += ", but its process group " + std::to_string(groupForTrace) +
                           " still had members " +
                           std::to_string(PROCESS_GROUP_DRAIN_TIMEOUT_MS) +
                           " ms after SIGKILL (" + describeProcessGroupState(groupState) +
                           "), so a process it started has outlived this run holding whatever "
                           "descriptors it inherited";
            } else {
                std::cout << TRACE_PREFIX << "Process group " << groupForTrace
                          << " is empty after the group SIGKILL, so nothing " << description
                          << " started is still present" << std::endl;
            }
        }
    }

    std::cout << TRACE_PREFIX << description << " pid " << pidForTrace << " " << outcome
              << std::endl;

    return reaped && groupDrained;
}

/**
 * @brief Terminates the fake service host, waits for it to go, and reaps it.
 *
 * Idempotent and safe after a partial setup. Asks a serving host to `shutdown`, then delegates
 * to terminateAndReapChildProcess() with the readiness pipe as the death channel.
 *
 * @return std::string                            - How the host ended; empty when none was launched
 *
 * @post g_hostPid and g_hostProcessGroupId are -1 and g_hostReadinessReadFd is closed.
 *
 * @see terminateAndReapChildProcess(), waitForChildExitViaPipeEof()
 */
std::string terminateAndReapFakeServiceHost()
{
    if (g_hostPid <= 0) {
        /* Nothing launched, or already run: only a descriptor from a failed launch may remain. */
        if (g_hostReadinessReadFd >= 0) {
            ::close(g_hostReadinessReadFd);
            g_hostReadinessReadFd = -1;
        }
        closeHostControlChannel();
        return std::string();
    }

    const long hostPid = static_cast<long>(g_hostPid);

    /* Polite step, tried only once the host is ready: `shutdown` runs its own teardown. The
     * signal path below runs regardless and is what guarantees the host cannot outlive the run. */
    if (g_hostReportedReady && (g_hostControlWriteFd >= 0)) {
        std::string reply;
        std::string failureDetail;

        if (performHostControlRequest("shutdown", reply, failureDetail)) {
            std::cout << TRACE_PREFIX << "Asked the fake HDMI CEC AIDL service host pid " << hostPid
                      << " to shut down over the control channel; it answered "
                      << renderUntrustedValue(reply) << std::endl;
        } else {
            std::cout << TRACE_PREFIX << "The polite shutdown request to pid " << hostPid
                      << " did not complete (" << failureDetail
                      << "); the signal path below is what guarantees the host does not outlive this "
                         "run, and it runs regardless" << std::endl;
        }
    }

    /* Closed before the signal, so a host still in its command loop sees a clean end of file. */
    closeHostControlChannel();

    /* The shared teardown, i.e. the path the broken-pipe probe exercises; the readiness pipe is
     * the death channel because the host holds its write end for its whole life. */
    std::string outcome;
    const bool reaped =
        terminateAndReapChildProcess(g_hostPid, "the fake HDMI CEC AIDL service host",
                                     g_hostReadinessReadFd, HOST_SHUTDOWN_TIMEOUT_MS,
                                     g_hostProcessGroupId, outcome);

    g_hostPid = -1;

    /* Cleared with the pid: a stale group id could let a second call signal a reused group. */
    g_hostProcessGroupId = -1;

    if (g_hostReadinessReadFd >= 0) {
        ::close(g_hostReadinessReadFd);
        g_hostReadinessReadFd = -1;
    }

    /* Reported non-fatally: a leaked host is worth saying, but must not cut the teardown short. */
    EXPECT_TRUE(reaped) << "the fake HDMI CEC AIDL service host pid " << hostPid << " " << outcome
                        << ", so it may survive this run as an orphan holding the production "
                           "service name";

    return outcome;
}

/* ---- Broken-pipe probe: drives writeControlCommand()'s EPIPE arm and the child reap for real;
 * needs no host or binder, and never touches the live session's descriptors or pid. ---------- */

/**
 * @brief The command line the probe writes to its reader-less descriptor.
 *
 * Outside the host's vocabulary. The probe requires this text in writeControlCommand()'s
 * failure sentence, proving the diagnostic names the command that failed.
 */
const char *const EPIPE_PROBE_COMMAND = "epipe-probe";

/**
 * @brief One bound for every wait the probe makes, in milliseconds.
 *
 * A backstop: each wait completes in microseconds when the mechanism is sound.
 */
const int EPIPE_PROBE_TIMEOUT_MS = 2000;

/**
 * @brief Exit code the probe's child reports when it cannot make SIGTERM fatal to itself.
 *
 * Distinct from the host's codes and the pre-exec codes, so the reaped status is unambiguous.
 */
const int EPIPE_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED = 122;

/**
 * @brief Closes a descriptor if it is open and marks it closed.
 *
 * Idempotent, so cleanup paths sharing a variable never close a reused number twice.
 *
 * @param [in,out] fd                     - Descriptor to close; -1 on return
 *
 * @return None
 */
void closeIfOpen(int &fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

/**
 * @brief Owns the broken-pipe probe's descriptors and child, and releases whatever is left.
 *
 * A scope guard, so every return path releases the same resources. On success the probe reaps
 * its child itself and the destructor has nothing to do.
 *
 * @warning Non-copyable: a copy would release the same descriptors and pid twice.
 */
class EpipeProbeResources {
public:
    /** @brief Read end of the handshake pipe; the probe's own, and its child's death channel. */
    int handshakeReadFd = -1;
    /** @brief Write end of the handshake pipe; the child keeps it, the parent closes its copy. */
    int handshakeWriteFd = -1;
    /** @brief Read end of the probe pipe, closed by the parent to create the hazard. */
    int probeReadFd = -1;
    /** @brief Write end of the probe pipe, the descriptor writeControlCommand() is given. */
    int probeWriteFd = -1;
    /** @brief The probe's child, or -1 once it has been reaped or was never forked. */
    pid_t childPid = -1;

    /** @brief Creates a guard that holds nothing yet. */
    EpipeProbeResources() = default;
    /** @brief Deleted: a copy would release the same descriptors and pid twice. */
    EpipeProbeResources(const EpipeProbeResources &) = delete;
    /** @brief Deleted: a copy would release the same descriptors and pid twice. */
    EpipeProbeResources &operator=(const EpipeProbeResources &) = delete;

    /**
     * @brief Releases whatever the probe still holds, on every path out of it.
     *
     * Closes the four descriptors first, then kills and reaps a surviving child. Every release
     * is idempotent, so the successful path leaves nothing to do.
     *
     * @return None
     *
     * @post No descriptor and no child the probe created outlives the guard.
     *
     * @warning Must not raise: a destructor is implicitly noexcept.
     *
     * @see closeIfOpen(), reapChildBlocking(), proveEpipeDiagnosticAndChildReaping()
     */
    ~EpipeProbeResources()
    {
        closeIfOpen(handshakeReadFd);
        closeIfOpen(handshakeWriteFd);
        closeIfOpen(probeReadFd);
        closeIfOpen(probeWriteFd);

        if (childPid > 0) {
            const long pidForTrace = static_cast<long>(childPid);

            /* SIGKILL, not SIGTERM: the probe gave up partway, and an unreaped child would become
             * a zombie outliving the suite. */
            ::kill(childPid, SIGKILL);

            std::string outcome;
            const bool reaped = reapChildBlocking(childPid, outcome);
            childPid = -1;

            std::cout << TRACE_PREFIX << "The broken-pipe probe child pid " << pidForTrace
                      << " was still live when the probe unwound; it was killed and " << outcome
                      << (reaped ? "" : ", which leaves it uncollected") << std::endl;
        }
    }
};

/**
 * @brief Waits, bounded, for the probe child's "both probe ends are closed" byte.
 *
 * Makes the probe deterministic: until the byte arrives the child may still hold a reader. End
 * of file, meaning the child exited first, is reported apart from the bound expiring.
 *
 * @param [in]  handshakeReadFd           - Read end of the handshake pipe, open
 * @param [out] failureDetail             - Receives a diagnostic on failure; untouched on success
 *
 * @return bool                                   - Whether the byte arrived within the bound
 * @retval true                                   - It did, so the child holds no probe descriptor
 * @retval false                                  - Bound expired, child exited first, or fd failed
 *
 * @pre The parent has closed its own copy of the handshake write end.
 *
 * @see cecL2ProveEpipeDiagnosticAndChildReaping()
 */
bool awaitProbeChildClosedProbePipe(int handshakeReadFd, std::string &failureDetail)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(EPIPE_PROBE_TIMEOUT_MS);

    for (;;) {
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            failureDetail = "the probe child did not report within " +
                            std::to_string(EPIPE_PROBE_TIMEOUT_MS) +
                            " ms that it had closed both ends of the probe pipe, so whether a "
                            "reader still exists cannot be established and the write must not be "
                            "attempted";
            return false;
        }

        struct pollfd watched;
        watched.fd      = handshakeReadFd;
        watched.events  = POLLIN;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remaining.count()));

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            failureDetail = std::string("poll() on the probe handshake pipe failed: ") +
                            std::strerror(errno);
            return false;
        }

        if (ready == 0) {
            continue; /* Re-checked against the deadline at the top of the loop. */
        }

        char marker[8];
        const ssize_t got = ::read(handshakeReadFd, marker, sizeof(marker));

        if (got > 0) {
            return true;
        }

        if (got == 0) {
            failureDetail = "the probe child exited before reporting that it had closed both ends "
                            "of the probe pipe, so the write end may still have a reader. Its exit "
                            "status is reported by the reap below";
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        failureDetail = std::string("read() on the probe handshake pipe failed: ") +
                        std::strerror(errno);
        return false;
    }
}

/**
 * @brief Drives writeControlCommand()'s EPIPE arm and terminateAndReapChildProcess() for real.
 *
 * Steps 0-10: refuse a fatal SIGPIPE disposition, build the pipes and child, remove and confirm
 * every reader, write, check the diagnostic, reap the child, and release the rest.
 *
 * @param [out] observedDiagnostic        - Receives writeControlCommand()'s failure, if any
 * @param [out] failureDetail             - Receives the failing step and why; untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - EPIPE diagnostic named the command; child reaped
 * @retval false                                  - One step did not hold; failureDetail names it
 *
 * @pre SIGPIPE is not at its default disposition; step 0 verifies this.
 *
 * @post No descriptor and no child created here outlives the call, on every path.
 *
 * @see writeControlCommand(), terminateAndReapChildProcess(), EpipeProbeResources
 */
bool proveEpipeDiagnosticAndChildReaping(std::string &observedDiagnostic,
                                         std::string &failureDetail)
{
    observedDiagnostic.clear();

    /* Step 0: SIGPIPE's disposition, read rather than assumed; under SIG_DFL step 7's write would
     * kill this runner, so the probe refuses to proceed. */
    struct sigaction currentSigpipe;
    std::memset(&currentSigpipe, 0, sizeof(currentSigpipe));

    if (::sigaction(SIGPIPE, nullptr, &currentSigpipe) != 0) {
        failureDetail = std::string("step 0, read SIGPIPE's disposition: sigaction() failed: ") +
                        std::strerror(errno) +
                        ". Whether a write to a reader-less pipe returns or terminates this process "
                        "cannot be established, so no such write may be attempted";
        return false;
    }

    if (currentSigpipe.sa_handler == SIG_DFL) {
        failureDetail = "step 0, read SIGPIPE's disposition: it is SIG_DFL, which TERMINATES this "
                        "process on a write to a reader-less pipe. ignoreBrokenPipeSignal() must "
                        "have run as the first step of CecL2TestEnvironment::SetUp; without it the "
                        "EPIPE arm of writeControlCommand() is dead code and this runner would be "
                        "killed at the write instead of reporting it";
        return false;
    }

    EpipeProbeResources probe;

    /* Step 1: the handshake pipe, carrying the child's "probe ends closed" byte and then serving
     * as its death channel; O_CLOEXEC like every other pipe in this file. */
    int handshakePipe[2] = { -1, -1 };
    if (::pipe2(handshakePipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("step 1, create the handshake pipe: ") + std::strerror(errno) +
                        ". Without it the probe cannot establish that its child has released the "
                        "probe pipe's read end, and a write with a reader still present would "
                        "succeed";
        return false;
    }
    probe.handshakeReadFd  = handshakePipe[0];
    probe.handshakeWriteFd = handshakePipe[1];

    /* Step 2: the probe pipe the real write targets, the probe's own so the live control channel
     * is never disturbed. */
    int probePipe[2] = { -1, -1 };
    if (::pipe2(probePipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("step 2, create the probe pipe: ") + std::strerror(errno) +
                        ". This is the descriptor the real writeControlCommand() call is made "
                        "against, so without it there is nothing to drive";
        return false;
    }
    probe.probeReadFd  = probePipe[0];
    probe.probeWriteFd = probePipe[1];

    /* Step 3: the child, which releases both probe descriptors, says so, and waits to be reaped. */
    const pid_t child = ::fork();

    if (child < 0) {
        failureDetail = std::string("step 3, fork the probe child: ") + std::strerror(errno) +
                        ". Without a child there is no second holder of the probe pipe's read end "
                        "to release it and nothing for the real reaping to collect";
        return false;
    }

    if (child == 0) {
        /* In the child: only fork-safe calls (close, sigaction, raw write, _exit), as a binder
         * threadpool may hold locks at the fork. Both probe ends go, not just the read end. */
        ::close(probe.probeReadFd);
        ::close(probe.probeWriteFd);
        ::close(probe.handshakeReadFd);

        /* No setpgid(): this child neither forks nor execs, so it needs no group, and step 9's
         * -1 group id keeps terminateAndReapChildProcess()'s single-pid path exercised. */

        /* SIGTERM is made fatal explicitly, so step 9's bounded wait does not rest on an
         * inherited disposition. */
        struct sigaction terminateDefault;
        std::memset(&terminateDefault, 0, sizeof(terminateDefault));
        terminateDefault.sa_handler = SIG_DFL;
        ::sigemptyset(&terminateDefault.sa_mask);
        terminateDefault.sa_flags = 0;

        if (::sigaction(SIGTERM, &terminateDefault, nullptr) != 0) {
            ::_exit(EPIPE_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED);
        }

        /* Only now the byte, meaning "the closes above have happened"; a failed write shows the
         * parent end of file once this child is killed. */
        static const char closedMarker[] = "C";
        writeRawFully(probe.handshakeWriteFd, closedMarker, sizeof(closedMarker) - 1);

        /* Blocked until step 9's SIGTERM or SIGKILL; the loop makes any other signal harmless. */
        for (;;) {
            ::pause();
        }
    }

    /* The parent from here on. */
    probe.childPid = child;

    /* Step 4: the parent's handshake write end goes, so end of file on the read end means the
     * child has exited. */
    closeIfOpen(probe.handshakeWriteFd);

    /* Step 5: the parent's probe read end goes, leaving a writer and no reader; left open, the
     * parent itself would be a reader and step 7's write would succeed. */
    closeIfOpen(probe.probeReadFd);

    /* Step 6: wait for the child's report rather than race it for the read end. */
    std::string handshakeFailure;
    if (!awaitProbeChildClosedProbePipe(probe.handshakeReadFd, handshakeFailure)) {
        failureDetail = "step 6, wait for the probe child to release the probe pipe: " +
                        handshakeFailure;
        return false;
    }

    /* Step 7: the real call; it must fail from the write, the deadline being only a backstop. */
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(EPIPE_PROBE_TIMEOUT_MS);

    const bool written = writeControlCommand(EPIPE_PROBE_COMMAND, probe.probeWriteFd, deadline,
                                             observedDiagnostic);

    if (written) {
        failureDetail = "step 7, write \"" + std::string(EPIPE_PROBE_COMMAND) +
                        "\" to a pipe with no reader: writeControlCommand() reported SUCCESS. A "
                        "write that succeeds into a pipe nobody can read means a read end is still "
                        "open somewhere, so the EPIPE arm the host's control channel depends on was "
                        "not reached and its diagnostic was not produced";
        return false;
    }

    /* Step 8: the diagnostic must name the command and "EPIPE"; the deadline arm also names the
     * command, so only "EPIPE" proves the broken-pipe arm ran. */
    if (observedDiagnostic.find(EPIPE_PROBE_COMMAND) == std::string::npos) {
        failureDetail = "step 8, check the diagnostic: writeControlCommand() failed as required but "
                        "its sentence does not name the command \"" +
                        std::string(EPIPE_PROBE_COMMAND) + "\". It said: \"" + observedDiagnostic +
                        "\". A failure that does not say which command was lost leaves the reader of "
                        "a CI log unable to tell which observation went missing";
        return false;
    }

    if (observedDiagnostic.find("EPIPE") == std::string::npos) {
        failureDetail = "step 8, check the diagnostic: writeControlCommand() failed and named the "
                        "command, but its sentence does not report EPIPE, so it did not take the "
                        "broken-pipe arm. It said: \"" + observedDiagnostic +
                        "\". EPIPE is the one errno that means \"the reader has closed its "
                        "descriptor or exited\", and the deadline arm names the command too, so "
                        "without it this could be a bound that expired";
        return false;
    }

    /* Step 9: the real reaping through teardown's function, with the handshake pipe as the death
     * channel; a runner killed by SIGPIPE would never reach it. */
    std::string outcome;
    const bool reaped =
        terminateAndReapChildProcess(probe.childPid, "the broken-pipe probe child",
                                     probe.handshakeReadFd, EPIPE_PROBE_TIMEOUT_MS,
                                     /* childProcessGroupId */ -1, outcome);

    if (!reaped) {
        /* The pid stays set so the guard's destructor still kills and collects the child. */
        failureDetail = "step 9, terminate and reap the probe child: it " + outcome +
                        ". The reap is the second thing ignoring SIGPIPE exists to protect - a "
                        "runner killed at the write above would never reach a teardown - so a probe "
                        "that cannot demonstrate it has demonstrated only half the property";
        return false;
    }

    probe.childPid = -1;

    /* Step 10: release the rest explicitly, so the guard is visibly a backstop. */
    closeIfOpen(probe.probeWriteFd);
    closeIfOpen(probe.handshakeReadFd);

    std::cout << TRACE_PREFIX << "The broken-pipe probe drove writeControlCommand() against a "
                 "reader-less descriptor and it reported: " << observedDiagnostic
              << ". Its child " << outcome << ", collected by the same terminate-and-reap a "
                 "teardown uses" << std::endl;

    return true;
}

/* ---- Process-group probe: drives the group-wide teardown against a child that forked a
 * SIGTERM-ignoring grandchild, and observes the child's descriptor sweep from outside. ------- */

/**
 * @brief Exit status of a probe child that could not create its own process group.
 *
 * Distinct from the other two so a failure names its step; the parent reports it verbatim.
 */
const int GROUP_PROBE_CHILD_EXIT_SETPGID_FAILED = 123;

/** @brief Exit status of a probe child whose own fork of the grandchild failed. */
const int GROUP_PROBE_CHILD_EXIT_FORK_FAILED = 124;

/** @brief Exit status of a probe grandchild that could not make SIGTERM harmless to itself. */
const int GROUP_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED = 125;

/**
 * @brief How long the probe waits for its child's "I am set up" byte.
 *
 * Covers two forks, a setpgid() and a descriptor sweep; expiry means the probe cannot proceed.
 */
const int GROUP_PROBE_HANDSHAKE_TIMEOUT_MS = 5000;

/**
 * @brief The window in which a pipe that should already be at end of file must report it.
 *
 * Checks the witness pipe for end of file and the liveness pipe for its absence; short, as a
 * longer bound would only slow a failing case.
 */
const int GROUP_PROBE_CLOSURE_WINDOW_MS = 250;

/** @brief Grace period the probe child gets after SIGTERM before the escalation. */
const int GROUP_PROBE_GRACE_MS = 2000;

/**
 * @brief Owns the process-group probe's descriptors, child and group, and releases what is left.
 *
 * A scope guard like EpipeProbeResources; on success the destructor has nothing to do.
 *
 * @warning Non-copyable: a copy would release the same descriptors, pid and group twice.
 */
class ProcessGroupProbeResources {
public:
    /** @brief Read end of the handshake pipe; also the child's death channel. */
    int handshakeReadFd = -1;
    /** @brief Write end of the handshake pipe; the child keeps it, the parent closes its copy. */
    int handshakeWriteFd = -1;
    /** @brief Read end of the liveness pipe; end of file on it means the grandchild has gone. */
    int livenessReadFd = -1;
    /** @brief Write end of the liveness pipe; only the grandchild holds it. */
    int livenessWriteFd = -1;
    /** @brief Read end of the witness pipe; end of file on it means the child's sweep ran. */
    int witnessReadFd = -1;
    /** @brief Write end of the witness pipe, never named to the child, which must close it. */
    int witnessWriteFd = -1;
    /** @brief The probe's child, or -1 once reaped or never forked. */
    pid_t childPid = -1;
    /** @brief The group the child leads, or -1 when it was never confirmed. */
    pid_t childGroupId = -1;

    /** @brief Creates a guard that holds nothing yet. */
    ProcessGroupProbeResources() = default;
    /** @brief Deleted: a copy would release the same descriptors, pid and group twice. */
    ProcessGroupProbeResources(const ProcessGroupProbeResources &) = delete;
    /** @brief Deleted: a copy would release the same descriptors, pid and group twice. */
    ProcessGroupProbeResources &operator=(const ProcessGroupProbeResources &) = delete;

    /**
     * @brief Releases every descriptor, then ends the whole group and reaps the child.
     *
     * SIGKILL goes to the group, as a hard-to-kill grandchild is the likeliest leftover; a group
     * that will not empty is traced.
     *
     * @return None
     *
     * @post No descriptor, no child and no member of the child's group outlives the guard.
     *
     * @warning Must not raise: a destructor is implicitly noexcept.
     */
    ~ProcessGroupProbeResources()
    {
        closeIfOpen(handshakeReadFd);
        closeIfOpen(handshakeWriteFd);
        closeIfOpen(livenessReadFd);
        closeIfOpen(livenessWriteFd);
        closeIfOpen(witnessReadFd);
        closeIfOpen(witnessWriteFd);

        if (childGroupId > 0) {
            ::kill(-childGroupId, SIGKILL);
        }

        if (childPid > 0) {
            const long pidForTrace = static_cast<long>(childPid);

            ::kill(childPid, SIGKILL);

            std::string outcome;
            const bool reaped = reapChildBlocking(childPid, outcome);
            childPid          = -1;

            std::cout << TRACE_PREFIX << "The process-group probe's guard killed and collected "
                         "child pid " << pidForTrace << ": " << outcome
                      << (reaped ? "" : " - it may outlive this run") << std::endl;
        }

        if (childGroupId > 0) {
            ProcessGroupState state = ProcessGroupState::Populated;

            if (!awaitProcessGroupEmpty(childGroupId, PROCESS_GROUP_DRAIN_TIMEOUT_MS, state)) {
                std::cout << TRACE_PREFIX << "The process-group probe's guard could not empty group "
                          << static_cast<long>(childGroupId) << " ("
                          << describeProcessGroupState(state)
                          << "); a process it created has outlived this run" << std::endl;
            }

            childGroupId = -1;
        }
    }
};

/**
 * @brief Waits, bounded, for one byte on a descriptor.
 *
 * Polls against one monotonic deadline; end of file is reported apart from the bound expiring.
 *
 * @param [in]  readFd                    - Descriptor to read from
 * @param [in]  timeoutMs                 - How long to wait, in milliseconds
 * @param [out] failureDetail             - Receives a diagnostic on failure; untouched on success
 *
 * @return bool                                   - Whether a byte arrived within the bound
 * @retval true                                   - It did
 * @retval false                                  - Bound expired, writer exited first, or fd failed
 *
 * @see proveHostProcessGroupTeardown()
 */
bool awaitOneByte(int readFd, int timeoutMs, std::string &failureDetail)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            failureDetail = "no byte arrived within " + std::to_string(timeoutMs) + " ms";
            return false;
        }

        struct pollfd watched;
        watched.fd      = readFd;
        watched.events  = POLLIN;
        watched.revents = 0;

        const int ready = ::poll(&watched, 1, static_cast<int>(remaining.count()));

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            failureDetail = std::string("poll() failed: ") + std::strerror(errno);
            return false;
        }

        if (ready == 0) {
            continue; /* Re-checked against the deadline at the top of the loop. */
        }

        char received = 0;
        const ssize_t got = ::read(readFd, &received, sizeof(received));

        if (got > 0) {
            return true;
        }

        if (got == 0) {
            failureDetail = "the writer closed its end without sending anything, so it exited "
                            "before it finished setting itself up";
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        failureDetail = std::string("read() failed: ") + std::strerror(errno);
        return false;
    }
}

/**
 * @brief Drives the group-wide teardown against a child that forked, and the child's own sweep.
 *
 * Steps 1-10 end on two observed facts: a SIGTERM-ignoring grandchild the runner never forked is
 * gone, and a descriptor never named to the child was closed by its sweep.
 *
 * @param [out] failureDetail             - Receives the failing step and why; untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - Grandchild ended, sweep ran, and group is empty
 * @retval false                                  - A step failed; failureDetail names which
 *
 * @post No descriptor, no child and no member of the child's group outlives this call.
 *
 * @warning The child's post-fork code makes only fork-safe calls: no allocation, no locks.
 *
 * @see terminateAndReapChildProcess(), closeInheritedDescriptorsExcept(), startFakeServiceHost()
 */
bool proveHostProcessGroupTeardown(std::string &failureDetail)
{
    ProcessGroupProbeResources probe;

    /* Step 1: three pipes, O_CLOEXEC though nothing execs, so the shape matches the host launch. */
    int handshakePipe[2] = { -1, -1 };
    int livenessPipe[2]  = { -1, -1 };
    int witnessPipe[2]   = { -1, -1 };

    if (::pipe2(handshakePipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("step 1, the handshake pipe could not be created: ") +
                        std::strerror(errno);
        return false;
    }

    probe.handshakeReadFd  = handshakePipe[0];
    probe.handshakeWriteFd = handshakePipe[1];

    if (::pipe2(livenessPipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("step 1, the liveness pipe could not be created: ") +
                        std::strerror(errno);
        return false;
    }

    probe.livenessReadFd  = livenessPipe[0];
    probe.livenessWriteFd = livenessPipe[1];

    if (::pipe2(witnessPipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("step 1, the witness pipe could not be created: ") +
                        std::strerror(errno);
        return false;
    }

    probe.witnessReadFd  = witnessPipe[0];
    probe.witnessWriteFd = witnessPipe[1];

    const pid_t runnerGroupBefore = ::getpgrp();

    /* Step 2: the fork; the child creates its own process group, sweeps its descriptors, forks
     * the grandchild, then sends the handshake byte and waits. */
    const pid_t child = ::fork();

    if (child < 0) {
        failureDetail = std::string("step 2, the probe child could not be forked: ") +
                        std::strerror(errno);
        return false;
    }

    if (child == 0) {
        /* The child: its own process group, then the descriptor sweep, then the work, in the
         * same order as startFakeServiceHost()'s child. */
        if (::setpgid(0, 0) != 0) {
            ::_exit(GROUP_PROBE_CHILD_EXIT_SETPGID_FAILED);
        }

        /* The sweep keeps the two named descriptors; its closing the unnamed witness write end is
         * the only evidence outside this process that it ran. */
        closeInheritedDescriptorsExcept(probe.handshakeWriteFd, probe.livenessWriteFd, -1);

        const pid_t grandchild = ::fork();

        if (grandchild < 0) {
            ::_exit(GROUP_PROBE_CHILD_EXIT_FORK_FAILED);
        }

        if (grandchild == 0) {
            /* The grandchild keeps only the liveness write end, so end of file there means it,
             * and only it, has gone. */
            ::close(probe.handshakeWriteFd);

            /* It ignores SIGTERM, so only the escalation to SIGKILL can end it. */
            struct sigaction ignoreTerminate;
            std::memset(&ignoreTerminate, 0, sizeof(ignoreTerminate));
            ignoreTerminate.sa_handler = SIG_IGN;
            ::sigemptyset(&ignoreTerminate.sa_mask);
            ignoreTerminate.sa_flags = 0;

            if (::sigaction(SIGTERM, &ignoreTerminate, nullptr) != 0) {
                ::_exit(GROUP_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED);
            }

            for (;;) {
                ::pause();
            }
        }

        /* Back in the child: drop its liveness write end so the grandchild is the only writer,
         * and only then send the handshake byte. */
        ::close(probe.livenessWriteFd);

        static const char readyMarker[] = "G";
        writeRawFully(probe.handshakeWriteFd, readyMarker, sizeof(readyMarker) - 1);

        for (;;) {
            ::pause();
        }
    }

    /* The parent from here on. */
    probe.childPid = child;

    /* Step 3: drop the parent's three write ends; while it holds one, no end of file can arrive. */
    closeIfOpen(probe.handshakeWriteFd);
    closeIfOpen(probe.livenessWriteFd);
    closeIfOpen(probe.witnessWriteFd);

    /* Step 4: wait for the child's report, so every check below sees a fully set-up child. */
    std::string handshakeFailure;

    if (!awaitOneByte(probe.handshakeReadFd, GROUP_PROBE_HANDSHAKE_TIMEOUT_MS, handshakeFailure)) {
        failureDetail = "step 4, the probe child did not report that it was ready: " +
                        handshakeFailure;
        return false;
    }

    /* Step 5: read the group back as the launch does; an assumed id is the runner's own. */
    const pid_t observedGroup = ::getpgid(child);

    if (observedGroup != child) {
        failureDetail = "step 5, the probe child's process group is " +
                        std::to_string(static_cast<long>(observedGroup)) + " rather than its own "
                        "pid " + std::to_string(static_cast<long>(child)) +
                        ", so it did not become a group leader";
        return false;
    }

    if (observedGroup == runnerGroupBefore) {
        failureDetail = "step 5, the probe child's process group is this runner's own group " +
                        std::to_string(static_cast<long>(runnerGroupBefore)) +
                        ", so a group-wide signal would have ended this runner";
        return false;
    }

    probe.childGroupId = observedGroup;

    /* Step 6: the sweep, observed from outside: only the child's sweep can have closed the last
     * copy of the witness write end, and it ran before the handshake byte. */
    if (!waitForChildExitViaPipeEof(probe.witnessReadFd, GROUP_PROBE_CLOSURE_WINDOW_MS)) {
        failureDetail = "step 6, the witness descriptor was still open in the probe child " +
                        std::to_string(GROUP_PROBE_CLOSURE_WINDOW_MS) +
                        " ms after it reported ready, so the pre-exec sweep did not close the "
                        "descriptors the child was never told about - a host would inherit them "
                        "and pass them to everything it starts";
        return false;
    }

    /* Step 7: the grandchild must still be alive (no end of file within the window), or step 9
     * would prove nothing. */
    if (waitForChildExitViaPipeEof(probe.livenessReadFd, GROUP_PROBE_CLOSURE_WINDOW_MS)) {
        failureDetail = "step 7, the probe grandchild had already gone before the teardown ran, so "
                        "nothing below would have been evidence that the teardown ended it";
        return false;
    }

    const ProcessGroupState populatedState = probeProcessGroup(probe.childGroupId);

    if (populatedState != ProcessGroupState::Populated) {
        failureDetail = "step 7, the probe child's group reported " +
                        describeProcessGroupState(populatedState) +
                        " while both the child and the grandchild were still running, so the group "
                        "probe cannot be relied on to detect a leak";
        return false;
    }

    /* Step 8: the real teardown, with the same call and confirmed group id the host's uses. */
    std::string outcome;
    const bool reaped =
        terminateAndReapChildProcess(probe.childPid, "the process-group probe child",
                                     probe.handshakeReadFd, GROUP_PROBE_GRACE_MS,
                                     probe.childGroupId, outcome);

    if (!reaped) {
        /* The pid and group stay set so the guard's destructor still ends what this could not. */
        failureDetail = "step 8, the terminate-and-reap did not complete: " + outcome;
        return false;
    }

    probe.childPid = -1;

    /* Step 9: the grandchild ignored SIGTERM, so end of file on its pipe proves the group SIGKILL
     * ended it. */
    if (!waitForChildExitViaPipeEof(probe.livenessReadFd, PROCESS_GROUP_DRAIN_TIMEOUT_MS)) {
        failureDetail = "step 9, the probe grandchild was still holding the liveness descriptor " +
                        std::to_string(PROCESS_GROUP_DRAIN_TIMEOUT_MS) +
                        " ms after the teardown reported success, so a process this run started "
                        "has outlived it";
        return false;
    }

    /* Step 10: the group is empty, covering a descendant holding no descriptor of this harness. */
    const ProcessGroupState finalState = probeProcessGroup(probe.childGroupId);

    if (finalState != ProcessGroupState::Empty) {
        failureDetail = "step 10, after the teardown reported success the probe child's group "
                        "still reported " + describeProcessGroupState(finalState);
        return false;
    }

    probe.childGroupId = -1;

    if (::getpgrp() != runnerGroupBefore) {
        failureDetail = "step 10, this runner's own process group changed from " +
                        std::to_string(static_cast<long>(runnerGroupBefore)) + " to " +
                        std::to_string(static_cast<long>(::getpgrp())) +
                        " during the probe, which no part of it should be able to do";
        return false;
    }

    std::cout << TRACE_PREFIX << "The process-group probe's child led group "
              << static_cast<long>(observedGroup) << ", its pre-exec sweep closed the descriptor it "
                 "was never told about, and its SIGTERM-ignoring grandchild did not outlive the "
                 "teardown: " << outcome << std::endl;

    return true;
}

/**
 * @brief Launches the fake service host and blocks until it reports ready.
 *
 * Completes before anything resolves the back-end selection. Every failure fails the run rather
 * than letting the suite silently exercise the legacy back-end.
 *
 * @return None
 *
 * @post On success the fake service is published and served, and the selection is unresolved.
 *
 * @warning Raises fatal GoogleTest failures; call it through ASSERT_NO_FATAL_FAILURE.
 *
 * @see startFakeServiceHost(), awaitHostReadiness(), terminateAndReapFakeServiceHost()
 */
void launchHostAndWaitUntilReady()
{
    const char *const rawHostPath = ::getenv(HOST_PATH_VARIABLE);

    ASSERT_TRUE(rawHostPath != nullptr && rawHostPath[0] != '\0')
        << AIDL_MODE_VARIABLE << "=" << AIDL_MODE_REMOTE << " requires " << HOST_PATH_VARIABLE
        << " to name the fake service host binary, and it is unset or empty. The build sets it "
           "to the fake_hdmi_cec_aidl_host binary built alongside this runner; with nothing to "
           "launch there is no service to find, and continuing would select the legacy back-end "
           "and report a green result for an AIDL invocation that never ran";

    std::string failureDetail;
    ASSERT_TRUE(startFakeServiceHost(rawHostPath, failureDetail))
        << "the fake HDMI CEC AIDL service host could not be launched: " << failureDetail;

    std::string observed;
    const ReadinessOutcome outcome = awaitHostReadiness(observed);

    if (outcome == ReadinessOutcome::Ready) {
        std::cout << TRACE_PREFIX << "The fake HDMI CEC AIDL service host reported ready; the "
                  "service is published and served, and the back-end selection has NOT yet been "
                  "resolved" << std::endl;

        /* The channel is proven once here with `ping`, which touches nothing in the fake, so a
         * broken channel fails as one setup diagnostic rather than as scattered case timeouts. */
        std::string reply;
        std::string failureDetail;

        if (!performHostControlRequest("ping", reply, failureDetail)) {
            const std::string hostEnd = terminateAndReapFakeServiceHost();
            FAIL() << "the fake HDMI CEC AIDL service host reported ready but its control and "
                      "observation channel does not answer, so no case in this binary could observe "
                      "an outbound transmit at the service or trigger an inbound delivery: "
                   << failureDetail << ". It " << hostEnd;
            return;
        }

        if (reply != "OK pong") {
            const std::string hostEnd = terminateAndReapFakeServiceHost();
            FAIL() << "the fake HDMI CEC AIDL service host answered a control-channel ping with "
                   << renderUntrustedValue(reply) << " where \"OK pong\" was expected. The reply is "
                      "matched verbatim because the protocol is fixed: anything else means the "
                      "descriptor pair is crossed, or the binary at " << HOST_PATH_VARIABLE
                   << " implements a different protocol from the one this harness speaks. It "
                   << hostEnd;
            return;
        }

        g_hostReportedReady = true;

        std::cout << TRACE_PREFIX << "The host's control and observation channel answered \"" << reply
                  << "\"; outbound transmits can be observed at the service and inbound deliveries "
                     "can be triggered" << std::endl;
        return;
    }

    /* Reaped before FAIL(), which returns at once; the reap also supplies the host's exit status,
     * the best clue to why no token arrived. */
    const std::string hostEnd = terminateAndReapFakeServiceHost();

    if (outcome == ReadinessOutcome::ClosedWithoutToken) {
        FAIL() << "the fake HDMI CEC AIDL service host exited without reporting ready, so no "
                  "service was ever published. It " << hostEnd << ". The host writes no readiness "
                  "line on ANY failure path - a stale registration under the production service "
                  "name, an absent binder driver node, a service manager that refused the "
                  "registration - and traces the step it reached to standard output, prefixed "
                  "[FakeHdmiCecAidlHost]; that trace names the cause"
               << (observed.empty() ? std::string()
                                    : (". Partial output before it closed: " +
                                       renderUntrustedValue(observed)));
        return;
    }

    if (outcome == ReadinessOutcome::TimedOut) {
        FAIL() << "the fake HDMI CEC AIDL service host did not report ready within "
               << HOST_READINESS_TIMEOUT_MS << " ms, and it was still running when the bound "
                  "expired, so it is blocked rather than broken. It " << hostEnd << ". The likely "
                  "cause is a binder driver node with no service manager behind it: reaching the "
                  "service manager retries indefinitely, which the host cannot bound and this "
                  "wait therefore has to. A running servicemanager is an unconditional runtime "
                  "prerequisite wherever a binder driver is present"
               << (observed.empty() ? std::string()
                                    : (". Partial output received: " +
                                       renderUntrustedValue(observed)));
        return;
    }

    if (outcome == ReadinessOutcome::TokenMismatch) {
        FAIL() << "the readiness pipe delivered " << renderUntrustedValue(observed)
               << " where the fake HDMI CEC AIDL service host's readiness token was expected. "
                  "The token is matched verbatim on purpose, so this is not a near miss to be "
                  "accepted: either the descriptor named by " << HOST_READY_FD_VARIABLE
               << " is not the pipe this harness created, or the binary at " << HOST_PATH_VARIABLE
               << " is not the fake service host. It " << hostEnd;
        return;
    }

    FAIL() << "the readiness pipe could not be read, so whether the fake HDMI CEC AIDL service "
              "host became ready cannot be established: " << renderUntrustedValue(observed)
           << ". It " << hostEnd;
}

/**
 * @brief Returns CEC_TEST_AIDL_MODE as this harness acts on it, an unset or empty value as absent.
 *
 * Unset and empty both mean absent, so the legacy arm is what a bare run does.
 *
 * @return std::string                            - The mode, not yet validated
 *
 * @see applyAidlModeBeforeInit(), failUnlessSelectedBackEndMatchesMode(), cecL2RequestedAidlMode()
 */
std::string resolvedAidlMode()
{
    const char *const requested = ::getenv(AIDL_MODE_VARIABLE);
    return (requested != nullptr && requested[0] != '\0') ? std::string(requested)
                                                          : std::string(AIDL_MODE_ABSENT);
}

/**
 * @brief Reads CEC_TEST_AIDL_MODE and does what it asks, before the selection resolves.
 *
 * The legacy mode launches no second process, and this translation unit makes no direct binder
 * call; the remote mode launches the host and completes its handshake here, ahead of init.
 *
 * @return None
 *
 * @warning An unrecognised value is a fatal failure, never a fall back to the legacy mode.
 * @warning Raises fatal GoogleTest failures; call it through ASSERT_NO_FATAL_FAILURE.
 *
 * @see launchHostAndWaitUntilReady()
 */
void applyAidlModeBeforeInit()
{
    const std::string mode = resolvedAidlMode();

    if (mode == AIDL_MODE_ABSENT) {
        std::cout << TRACE_PREFIX << AIDL_MODE_VARIABLE << "=" << mode
                  << ": launching no fake service host, so the legacy back-end is expected and "
                     "this harness makes no binder call" << std::endl;
        return;
    }

    if (mode == AIDL_MODE_REMOTE) {
        ASSERT_NO_FATAL_FAILURE(launchHostAndWaitUntilReady());
        return;
    }

    if (mode == AIDL_MODE_COMPATIBLE || mode == AIDL_MODE_INCOMPATIBLE) {
        FAIL() << AIDL_MODE_VARIABLE << "=" << mode << " is not implemented by run_L2Tests. It "
                  "means \"register a fake service INSIDE THIS PROCESS\", which only run_L1Tests "
                  "does: libbinder resolves a locally registered name to the local BBinder, so "
                  "such a registration produces no proxy, no transaction across the driver and no "
                  "callback on a binder thread - none of which this tier exists to test. Refusing "
                  "rather than treating it as " << AIDL_MODE_ABSENT << ", because a misconfigured "
                  "invocation that quietly ran the legacy arm would be reported as a pass. Use "
                  "run_L1Tests for this mode, or " << AIDL_MODE_REMOTE << " for the "
                  "out-of-process equivalent";
        return;
    }

    /* Rendered, not streamed: the one diagnostic naming an unvalidated value, where a raw newline
     * could forge a GitHub workflow command. */
    FAIL() << AIDL_MODE_VARIABLE << " is set to " << renderUntrustedValue(mode)
           << ", which is not a recognised mode. run_L2Tests implements " << AIDL_MODE_ABSENT
           << " and " << AIDL_MODE_REMOTE
           << "; " << AIDL_MODE_COMPATIBLE << " and " << AIDL_MODE_INCOMPATIBLE << " belong to "
              "run_L1Tests. Refusing to fall back to " << AIDL_MODE_ABSENT << ", because a typo "
              "must not quietly downgrade the run to the legacy back-end and report it as a pass";
}

/**
 * @brief Fails the run unless LibCCEC::init() selected the back-end that @p mode requires.
 *
 * Absent requires the legacy back-end and remote the only other one, read by dynamic_cast so this
 * unit still makes no binder call; under absent any other selection is a stale registration.
 *
 * @param [in] mode                       - The mode applyAidlModeBeforeInit() accepted
 *
 * @return None
 *
 * @pre LibCCEC::init() has returned, so Driver::getInstance() is resolved for the process.
 *
 * @warning Raises fatal GoogleTest failures; call it through ASSERT_NO_FATAL_FAILURE.
 */
void failUnlessSelectedBackEndMatchesMode(const std::string &mode)
{
    const bool legacySelected = (dynamic_cast<DriverImpl *>(&Driver::getInstance()) != nullptr);

    if (mode == AIDL_MODE_ABSENT) {
        ASSERT_TRUE(legacySelected)
            << AIDL_MODE_VARIABLE << "=" << mode << " launched no fake service host, yet the "
               "factory selected a back-end other than the legacy one, so a service is already "
               "published under \"HdmiCec\" by another process: a stale registration. This run's "
               "outcome would depend on a process this suite does not own; stop that process and "
               "run again";
        return;
    }

    ASSERT_FALSE(legacySelected)
        << AIDL_MODE_VARIABLE << "=" << mode << " requires the AIDL back-end, but the factory "
           "selected the legacy back-end although the fake service host reported ready; the "
           "factory's \"not usable\" line above names why, and no case result in this run is "
           "evidence for the mode it was given";
}

} // namespace


/* ==== Cross-translation-unit seam: the four functions the case file declares extern; every
 * descriptor, child and mode read stays here; mangled names make parameter drift a link error. */

/**
 * @brief Reports whether the fake service host's control and observation channel is usable.
 *
 * True only under CEC_TEST_AIDL_MODE=remote once the readiness handshake has completed.
 *
 * @return bool                                   - Whether a request would have somewhere to go
 * @retval true                                   - Both descriptors open and the host answered ping
 * @retval false                                  - No host, a failed launch, or closed by teardown
 *
 * @warning A case depending on the channel must fail, not skip, when this is false.
 *
 * @see cecL2HostControlRequest()
 */
bool cecL2HostControlChannelIsOpen()
{
    return (g_hostControlWriteFd >= 0) && (g_hostObserveReadFd >= 0) && g_hostReportedReady;
}

/**
 * @brief Sends one command to the fake service host and returns its single reply line.
 *
 * Speaks the fake host's line protocol, whose verbs handleControlCommand() in the host defines;
 * one deadline bounds every path.
 *
 * @param [in]  command                   - Command text without terminator; no newline or CR
 * @param [out] reply                     - Receives the reply; on failure, unchanged or the rejected line
 * @param [out] failureDetail             - Receives what went wrong; untouched on success
 *
 * @return bool                                   - Whether one command was exchanged for one reply
 * @retval true                                   - reply begins "OK " or "ERR "
 * @retval false                                  - No channel, bad command, timeout, or bad reply
 *
 * @pre cecL2HostControlChannelIsOpen() reports true.
 *
 * @warning An "ERR " reply reports true; check the reply text.
 *
 * @see cecL2HostControlChannelIsOpen()
 */
bool cecL2HostControlRequest(const std::string &command, std::string &reply,
                             std::string &failureDetail)
{
    return performHostControlRequest(command, reply, failureDetail);
}

/**
 * @brief Drives this harness's own control-channel write and its own reaping against a broken pipe.
 *
 * Seam entry for proveEpipeDiagnosticAndChildReaping(). Needs no host or binder and touches no
 * live-channel state, so it means the same on every invocation.
 *
 * @param [out] observedDiagnostic        - Receives writeControlCommand()'s failure, if any
 * @param [out] failureDetail             - Receives the failing step and why; untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - EPIPE diagnostic named the command; child reaped
 * @retval false                                  - One step did not hold; failureDetail names which
 *
 * @pre SIGPIPE is not at its default disposition; verified first, never assumed.
 *
 * @post No descriptor and no child created by this call outlives it, on every path.
 *
 * @see cecL2HostControlRequest()
 */
bool cecL2ProveEpipeDiagnosticAndChildReaping(std::string &observedDiagnostic,
                                              std::string &failureDetail)
{
    return proveEpipeDiagnosticAndChildReaping(observedDiagnostic, failureDetail);
}

/**
 * @brief Returns CEC_TEST_AIDL_MODE as this harness reads it, so no case reads the variable itself.
 *
 * @return std::string                            - The raw value; empty when unset or empty
 *
 * @see applyAidlModeBeforeInit()
 */
std::string cecL2RequestedAidlMode()
{
    const char *const requested = ::getenv(AIDL_MODE_VARIABLE);
    return (requested != nullptr) ? std::string(requested) : std::string();
}

/**
 * @brief Proves teardown ends every process the launched host started, and inherits no more than
 *        the three descriptors it is told about.
 *
 * Lives in the harness's translation unit because everything it drives is internal here; the
 * DualPath prefix keeps it inside the runner's per-invocation filter. Needs no host or binder.
 *
 * @see proveHostProcessGroupTeardown(), startFakeServiceHost(), terminateAndReapChildProcess()
 */
TEST(DualPathHostLifecycleTest, TeardownEndsTheWholeProcessGroupAndInheritsOnlyNamedDescriptors)
{
    std::string failureDetail;

    ASSERT_TRUE(proveHostProcessGroupTeardown(failureDetail))
        << "the harness's child-process lifecycle did not hold, so a process or a descriptor this "
           "run created can outlive it: "
        << failureDetail;
}


/**
 * @brief Global GoogleTest environment: prepares the process for the L2 cases, then cleans up.
 */
class CecL2TestEnvironment : public ::testing::Environment {
public:
    /**
     * @brief Brings the process to the state every L2 case assumes, in a load-bearing order.
     *
     * Ignores SIGPIPE, installs the legacy HAL double, applies the requested mode (launching the
     * host on remote), only then initializes the CEC library, and finally checks its selection.
     *
     * @return None
     *
     * @post On success the library is initialized and the requested back-end is the one selected.
     *
     * @warning init() resolves the back-end selection once, so mode handling must precede it.
     * @warning A fatal failure skips every case, and TearDown still runs.
     *
     * @see applyAidlModeBeforeInit(), failUnlessSelectedBackEndMatchesMode()
     */
    void SetUp() override {
        /* First, before any descriptor exists: a write that could kill this process would lose
         * its failure and skip the host reap. See ignoreBrokenPipeSignal(). */
        std::string sigpipeFailure;
        ASSERT_TRUE(ignoreBrokenPipeSignal(sigpipeFailure)) << sigpipeFailure;

        // Create and install the driver mock
        g_driverMock = new HdmiCecDriverMock();
        HdmiCecDriverMock::setInstance(g_driverMock);

        // Decide what init()'s service lookup will find, publishing a real service first on the
        // remote mode; init() is the one-way door.
        ASSERT_NO_FATAL_FAILURE(applyAidlModeBeforeInit());

        /* Makes the ordering observable: this prints only after the readiness wait and before
         * init(), so a log showing it ahead of the host's readiness trace is a bug. */
        std::cout << TRACE_PREFIX << "Initializing the CEC library; this is the call that "
                     "resolves the back-end selection for the lifetime of the process"
                  << std::endl;

        /* Asserted, not wrapped: SetUp runs once, so whatever init() raises (a refused open() or a
         * failed Bus::start()) is real and must not leave a green suite on an unopened stack. */
        ASSERT_NO_THROW({ LibCCEC::getInstance().init("CEC_TEST"); })
            << "the CEC library could not be initialized, so not one case in this binary has "
               "its precondition; continuing would assert against an uninitialized stack";

        // init() has fixed the selection; a back-end the mode did not ask for stops the run.
        ASSERT_NO_FATAL_FAILURE(failUnlessSelectedBackEndMatchesMode(resolvedAidlMode()));
    }

    /**
     * @brief Shuts the CEC stack down, releases the HAL double and reaps the host.
     *
     * @return None
     *
     * @post No child or descriptor this harness opened outlives the run, and SIGPIPE's original
     *       disposition is restored.
     *
     * @warning term() may transact with the host, so the host is reaped only after it.
     * @warning Runs even when SetUp failed; every step tolerates a partial setup, non-fatally.
     *
     * @see terminateAndReapFakeServiceHost()
     */
    void TearDown() override {
        /* Non-fatal: results are already recorded and the cleanup below must run. After a failed
         * SetUp, term() raises and is reported alongside the original failure. */
        EXPECT_NO_THROW({ LibCCEC::getInstance().term(); })
            << "the CEC library could not be terminated cleanly; the remaining cleanup below "
               "still runs, but this process did not shut the CEC stack down properly";

        HdmiCecDriverMock::setInstance(nullptr);
        delete g_driverMock;
        g_driverMock = nullptr;

        /* Last, since term() needed the host alive; idempotent after a readiness-failure reap. */
        terminateAndReapFakeServiceHost();

        /* Only now the disposition, as the host teardown's last request needed its protection;
         * restoring it keeps a process-wide change scoped to the window that needed it. */
        std::string sigpipeFailure;
        EXPECT_TRUE(restoreBrokenPipeSignalDisposition(sigpipeFailure)) << sigpipeFailure;
    }
};

/**
 * @brief Entry point for the L2 integration runner.
 *
 * @param [in] argc                       - Argument count, consumed by GoogleTest
 * @param [in] argv                       - Argument vector, consumed by GoogleTest
 *
 * @return int                                    - Zero when every case passed, else non-zero
 *
 * @warning Configuration comes from CEC_TEST_AIDL_MODE and CEC_FAKE_AIDL_HOST_PATH, never argv.
 */
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new CecL2TestEnvironment);
    return RUN_ALL_TESTS();
}


/** @} */
/** @} */
