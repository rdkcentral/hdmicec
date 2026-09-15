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
 * @brief Process-global test environment for the L2 integration tier, and the parent
 *        half of its two-process lifecycle
 *
 * This translation unit owns everything the L2 cases depend on and nothing they may
 * touch directly: the legacy HAL double, the out-of-process fake service host, every
 * descriptor and every child process in the binary, and the order in which they are
 * brought up. The child half of the lifecycle is
 * `mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp`.@n
 * Three entry points cross into the case file and nothing else is shared:
 * cecL2HostControlChannelIsOpen(), cecL2HostControlRequest() and
 * cecL2ProveEpipeDiagnosticAndChildReaping().
 *
 * @par Why this tier needs a second process
 * libbinder resolves a service name registered in the calling process to the local
 * BBinder, so interface_cast there hands back that very object: no Bp* proxy is created,
 * no transaction crosses the binder driver, and the client threadpool is never involved.
 * An in-process fake therefore cannot exercise the transport, however faithfully it
 * implements the interface, which is what the L1 tier's in-process modes are. Hosting
 * the same fake in a separate process is what makes the middleware hold a real proxy and
 * receive its event callbacks on a binder threadpool thread. The build enforces the
 * separation: the runner compiles the legacy mock, the host binary compiles the fake, and
 * neither target carries the other's source.
 *
 * @par The initialization contract: every setup fault fails the run
 * No step of CecL2TestEnvironment::SetUp() tolerates a failure. The mode handling, the
 * host launch, the readiness wait, the channel handshake and the LibCCEC::init() call
 * each raise a fatal GoogleTest failure on their unhappy paths, and init() in particular
 * is asserted rather than wrapped: what it raises is real - Driver::getInstance().open()
 * refused by the selected HAL, or Bus::start() failing - and a failed initialization
 * reported as a green suite is worse than a red one, because every driver-dependent case
 * then asserts against an unopened stack and every other result in the binary becomes
 * meaningless.
 *
 * @par The host lifecycle this harness owns
 * On the remote mode it launches the fake service host as a child process with an
 * inherited readiness pipe, blocks on that pipe under a bounded timeout, and fails the
 * run rather than proceeding when the bound expires. At the end of the suite
 * CecL2TestEnvironment::TearDown() asks the host to shut down over the control channel,
 * signals it, waits for its exit within a bound, escalates to SIGKILL if it stays, and
 * reaps it. Nothing this run starts outlives it: an orphaned host would hold the
 * production service name and make the next run fail on a stale registration.
 *
 * @par Readiness is a token on a pipe rather than a timed wait
 * The host writes one fixed line to the inherited descriptor whose read end this process
 * holds, and this process blocks on that descriptor until the deadline. A sleep in place
 * of the token would convert a race into a flake - too short on a loaded machine or
 * inside an emulated guest, wasteful when it is not, and never evidence that the service
 * was published at all. The host writes no readiness line on any failure path, so this
 * wait is the only detector of a host that failed to register, of a host that could not
 * reach a binder transport, and of a host blocked indefinitely inside libbinder waiting
 * for binder handle 0 because no service manager is running. A timeout, an end of file
 * without the token, and a token that does not match all fail the run.
 *
 * @par The control and observation channel
 * The remote mode hands the host two further inherited descriptors, named in
 * CEC_FAKE_HOST_CONTROL_FD and CEC_FAKE_HOST_OBSERVE_FD: the read end of a pipe this
 * harness writes commands to, and the write end of a second pipe it reads one reply line
 * per command from. The host's own file block is the normative statement of that
 * protocol and this file implements the client half of it. The channel exists because
 * two of this tier's requirements are otherwise unassertable. An outbound transmit that
 * crossed the driver is visible here only as "sendTo did not throw", while the bytes the
 * service received live in the host process; and an inbound delivery can be caused only
 * by the fake service, which is also in the host process. Without the channel the first
 * is a no-throw tautology that a dropped or corrupted send would satisfy, and the second
 * cannot happen at all.
 *
 * @par CEC_TEST_AIDL_MODE
 * The variable selects what this process finds when it looks the service up. It is read
 * here and in `tests/L1Tests/test_main.cpp` and nowhere else: no production source reads
 * it, and none may.
 * - `absent` - launch nothing. The lookup finds no service, the legacy back-end is
 *   selected, and it is driven through the in-process legacy mock. An unset or empty
 *   variable means exactly this, so a plain `./run_L2Tests` exercises the legacy round
 *   trip and touches libbinder not at all. This is invocation D.
 * - `remote` - launch the out-of-process fake service host with its readiness pipe and
 *   its control and observation channel, wait for the readiness token, establish the
 *   channel with one ping, and only then initialize. The middleware resolves a real proxy
 *   over the binder driver and its listener callback arrives on a binder threadpool
 *   thread. This is invocation E.
 * - `compatible`, `incompatible` - in-process modes, which this runner does not
 *   implement. They mean "register a fake inside this very process", which is
 *   run_L1Tests' job: an in-process registration produces no proxy, no driver transaction
 *   and no callback on a binder thread, so it cannot be what this tier runs. Either one
 *   seen here is a fatal failure naming that runner rather than a quiet downgrade to
 *   `absent`.
 *
 * @note This file does not start the binder client threadpool. DriverAidlImpl::open()
 *       owns that, and it runs inside the init() call. The host starts its own
 *       service-side pool, which is a different pool serving a different direction;
 *       starting one here would duplicate an ownership the production back-end holds.
 * @note A stale registration is fatal and not a condition to work around, and the check
 *       is the host's rather than this process's. Something already published under the
 *       production service name would make the middleware's own lookup resolve against
 *       that service instead of the fake this harness hosts, and the invocation would
 *       still run and describe something nobody chose. The host queries the name with
 *       checkService() and exits with a code of its own, writing no readiness line, which
 *       this harness's bounded wait then reports. Re-checking here would mean this
 *       process reaching the service manager itself, which is unsafe in two independent
 *       ways on the pinned binder stack: with no driver node libbinder aborts the process
 *       rather than returning an error, and with a driver node but no running service
 *       manager it blocks indefinitely. Neither may be risked in a runner that must also
 *       execute the legacy invocation on a host with no binder support at all.
 * @note On the legacy invocation this harness touches libbinder not at all, and it links
 *       no binder symbol so that this cannot be got wrong. It launches a binary; it does
 *       not host a fake.
 * @note SIGPIPE is ignored for the lifetime of this environment and the disposition found
 *       on entry is restored afterwards, so that a write to a pipe whose reader has gone
 *       reports EPIPE to the case that asked for the observation instead of killing this
 *       process before teardown can reap the host. Observations travel over a pipe and
 *       not over binder, because binder is the transport under test and evidence carried
 *       over it would be attesting to the transport with the transport. See
 *       ignoreBrokenPipeSignal().
 *
 * @warning The order of the steps in CecL2TestEnvironment::SetUp() is what makes this
 *          tier mean anything. The init() call is the first thing in this binary that
 *          forces Driver::getInstance(), which constructs both back-ends, asks the AIDL
 *          one whether its service came up, emits one selected-path line naming the
 *          winner, and fixes that choice for the lifetime of the process. Anything that
 *          is to influence the selection has to happen before init(), and nothing after
 *          it can change the outcome. Launching the fake service host after init() leaves
 *          the already-resolved selection on the legacy back-end: the suite then
 *          exercises the legacy path from beginning to end, every case passes, and the
 *          run is reported as AIDL evidence while never having spoken binder.
 * @warning An unrecognised CEC_TEST_AIDL_MODE value is a fatal failure. Unset is the one
 *          value treated permissively, because `absent` is the tier's default and a bare
 *          `./run_L2Tests` is a legitimate way to run the legacy arm; a typo is not, and
 *          a typo that silently downgraded the run would report a green result for an
 *          invocation that never happened.
 *
 * @see mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp
 * @see tests/L2Tests/ccec/test_DualPathIntegration.cpp
 */

#include <gtest/gtest.h>
#include <iostream>
#include "hdmi_cec_driver_mock.h"
#include "ccec/LibCCEC.hpp"

/*
 * POSIX process, descriptor and signal primitives for the host lifecycle. There is
 * deliberately no binder or AIDL header among them: this harness launches a binary and
 * reads a pipe, so it needs no libbinder symbol and must not acquire one - see the
 * stale-registration paragraph in the block above.
 */
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

// Create mock instance before main
static HdmiCecDriverMock* g_driverMock = nullptr;

/*
 * The out-of-process fake service host, if this invocation launched one.
 *
 * Both are module scope because SetUp acquires them and TearDown releases them, and both
 * carry an explicit "nothing here" value so that teardown after a partial setup - the
 * legacy invocation, which launches no host, or a launch that failed halfway - can tell
 * that there is nothing to signal and nothing to reap. TearDown runs even when SetUp
 * failed fatally, which is measured GoogleTest behaviour rather than an assumption, so
 * that distinction is exercised on every failing run and not merely in theory.
 */
static pid_t g_hostPid = -1;

/*
 * The process group the launched host leads, or -1 when this invocation launched no host or
 * could not confirm that the one it launched had left this process's group.
 *
 * WHY A SECOND GLOBAL WHEN IT HOLDS THE SAME NUMBER AS g_hostPid. It is the same number - the
 * child makes itself a group leader, so its group id is its pid - but it is not the same
 * FACT, and teardown has to be able to tell the two apart. g_hostPid says "there is one child
 * to reap". This says "that child's descendants are collected under a group id this harness
 * owns, so a signal to its negation reaches every one of them and reaches nothing else".
 *
 * WHY IT IS -1 UNTIL CONFIRMED, which is the part that matters. Every process starts in its
 * parent's process group, so before the child calls setpgid() its group is the RUNNER'S: a
 * kill() to the negation of an unconfirmed number would signal this runner, the whole
 * GoogleTest process and every sibling process sharing its group. So this is set only after
 * the parent has read the child's group back with getpgid() and found it both equal to the
 * child's pid and different from this process's own group, and it is left at -1 whenever that
 * confirmation does not hold - in which case teardown signals the single pid, exactly as it
 * did before, rather than a group it cannot vouch for.
 *
 * @see startFakeServiceHost(), terminateAndReapFakeServiceHost(),
 *      terminateAndReapChildProcess()
 */
static pid_t g_hostProcessGroupId = -1;

/*
 * Read end of the readiness pipe, held open for the lifetime of the host rather than
 * closed once the token arrives, because it has a second job: end of file on it means
 * the host has closed its write end, which happens when and only when the host process
 * exits. That makes it a death channel, and it is what lets TearDown wait for the host
 * to go away without polling on a timer - see terminateAndReapFakeServiceHost().
 */
static int g_hostReadinessReadFd = -1;

/*
 * The parent's two ends of the host's control and observation channel, or -1 when this
 * invocation supplied none - which is every invocation that launches no host, i.e. the
 * legacy one.
 *
 * What the channel is for, because it is the difference between an AIDL invocation that
 * demonstrates something and one that merely completes. Two of this tier's requirements
 * cannot be discharged from inside this process at all:
 *
 *   Outbound. A transmit that crosses the binder driver is observed by the runner only as
 *   "sendTo returned without throwing". A send that reached the far side with corrupt
 *   bytes, or that a fake had quietly dropped, returns exactly the same way. The bytes the
 *   service actually received live in another process, and the fake is not linked into this
 *   runner - deliberately, since linking it would turn this tier back into the in-process
 *   case - so the only way to read them is to ask the host.
 *
 *   Inbound. The only object that can invoke the middleware's IHdmiCecEventListener is the
 *   fake service, and it is in the host process. Without a way to ask the host to fire a
 *   callback, no case in this binary can cause an inbound delivery, and the requirement
 *   that a received frame arrive other than on the calling thread has nothing to assert.
 *
 * Why it is a pipe and not a binder call, which is the point that makes the evidence worth
 * anything: binder is the thing under test. An observation that travelled over binder would
 * be attesting to the transport with the transport, so a transport fault could corrupt the
 * evidence and the corruption itself would be invisible. These two descriptors are an
 * ordinary pair of inherited pipes, and they work identically whether the driver is healthy,
 * degraded or absent.
 *
 * Both are module scope for the same reason g_hostPid is: SetUp acquires them, TearDown
 * releases them, and the client function that uses them is called from the other translation
 * unit in this binary, which has no other way to reach them.
 */
static int g_hostControlWriteFd = -1;
static int g_hostObserveReadFd  = -1;

/*
 * Whether the host reached readiness and answered on the channel.
 *
 * It gates exactly one thing - the polite `shutdown` request that teardown makes before it
 * signals - and it exists because the readiness-failure path calls the same teardown. A host
 * that never became ready is dead or wedged, so a request there would spend its whole bound
 * discovering what the signal establishes immediately.
 */
static bool g_hostReportedReady = false;

/*
 * The SIGPIPE disposition this harness found on entry, and whether it replaced it.
 *
 * Module scope for the same reason every other piece of channel state here is: SetUp installs the
 * replacement and TearDown puts the original back, and the two are different functions. The bool is
 * what makes the restore a no-op when the install never happened, so a TearDown that runs after a
 * SetUp which failed before its first step - which GoogleTest does - cannot install a
 * default-constructed disposition over whatever the process actually had.
 */
static struct sigaction g_previousSigpipeAction;
static bool g_sigpipeDispositionReplaced = false;

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
 * are a fixed contract shared with tests/L1Tests/test_main.cpp, run_coverage.sh, both CI
 * workflows and the test documentation - they are not free to change here.
 */
const char *const AIDL_MODE_VARIABLE     = "CEC_TEST_AIDL_MODE";
const char *const AIDL_MODE_ABSENT       = "absent";
const char *const AIDL_MODE_COMPATIBLE   = "compatible";
const char *const AIDL_MODE_INCOMPATIBLE = "incompatible";
const char *const AIDL_MODE_REMOTE       = "remote";

/*
 * Where this harness finds the host binary. The build sets it; this harness reads it.
 * The host itself never does, being already running by the time it would matter.
 */
const char *const HOST_PATH_VARIABLE = "CEC_FAKE_AIDL_HOST_PATH";

/*
 * The variable through which the host is told which inherited descriptor to signal on.
 * It carries the number of the write end of the pipe created below - this harness owns
 * the pipe and names the descriptor; the host parses the value strictly and refuses
 * anything that is not an unadorned non-negative decimal, so it is formatted here as
 * plain digits with no sign, no padding and no surrounding space.
 */
const char *const HOST_READY_FD_VARIABLE = "CEC_FAKE_HOST_READY_FD";

/*
 * The one readiness token, matched verbatim including its trailing newline.
 *
 * It is a fixed contract owned by mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp and
 * copied here rather than invented: that file writes this exact line, once, with a raw
 * unbuffered write, and only after publication has succeeded and its service-side
 * threadpool is running. The newline is part of the token because a reader matching a
 * line needs the terminator to know the line is whole - a partial write that delivered
 * the name without it must not be mistaken for readiness. Everything else the host
 * prints goes to standard output and is no part of this signal, so the pipe carries the
 * token and nothing else.
 */
const char *const HOST_READINESS_TOKEN = "FAKE_HDMI_CEC_AIDL_HOST_READY\n";

/**
 * @brief How long to wait for the host's readiness token before failing the run.
 *
 * Thirty seconds, and the value is reasoned rather than picked. It has to cover a cold
 * start of a second process that opens the binder driver, reaches the service manager,
 * publishes a service and starts a threadpool - and reaching the service manager retries
 * at one-second granularity until binder handle 0 resolves, so a bound of a few hundred
 * milliseconds could expire inside a single healthy retry. The AIDL invocation also runs
 * inside an emulated guest on a loaded CI machine, where every one of those steps is
 * slower than on a developer's host by a margin that is not worth predicting. Thirty
 * seconds is far above any plausible healthy startup and far below any CI job timeout,
 * which is the property that matters: a genuine failure is reported in well under a
 * minute with a diagnostic, instead of hanging until something outside this suite kills
 * it and leaves nobody any wiser.
 */
const int HOST_READINESS_TIMEOUT_MS = 30000;

/**
 * @brief How long to wait for a signalled host to exit before escalating to SIGKILL.
 *
 * Five seconds. The host's shutdown path is a self-pipe write, a few closes and a
 * return, so a host that is going to exit does so in milliseconds; a host that has not
 * gone after five seconds is wedged somewhere this harness cannot reach - blocked inside
 * libbinder, most plausibly - and waiting longer would only trade a diagnosable failure
 * for a hung suite. The escalation exists so that a wedged host cannot outlive the run
 * that started it, which would leave the production service name occupied and make the
 * next run fail on a stale registration.
 */
const int HOST_SHUTDOWN_TIMEOUT_MS = 5000;

/**
 * @brief Upper bound on bytes accepted from the readiness pipe before giving up.
 *
 * The token is thirty bytes and the pipe carries nothing else, so anything approaching
 * this cap means the descriptor is not what this harness thinks it is. Bounding the
 * accumulation keeps a misdirected stream from growing a buffer without limit while the
 * timeout has not yet expired.
 */
const std::size_t HOST_READINESS_MAX_BYTES = 4096;

/*
 * The two variables through which the host is told which inherited descriptors carry its
 * control and observation channel. They are a fixed contract owned by
 * mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp, whose file block is the normative
 * statement of the protocol, and they travel together: the host exits with a code of its
 * own and writes no readiness token if one is set without the other, if either is not a
 * plain non-negative descriptor number, if either names a descriptor that is not open in
 * the child or is open in the wrong direction, or if both name the same descriptor. This
 * harness therefore sets both or neither, and never derives one from the other.
 *
 * The control variable carries the number of the descriptor the host reads commands from,
 * the read end of a pipe this harness writes to. The observe variable carries the number of
 * the descriptor the host writes replies to, the write end of a second pipe this harness
 * reads from. Getting the two the wrong way round is refused by the host rather than
 * half-working, which is why the direction is spelled out at every place either name
 * appears in this file.
 */
const char *const HOST_CONTROL_FD_VARIABLE = "CEC_FAKE_HOST_CONTROL_FD";
const char *const HOST_OBSERVE_FD_VARIABLE = "CEC_FAKE_HOST_OBSERVE_FD";

/**
 * @brief How long one control command may wait for its reply before the request fails.
 *
 * Ten seconds, and it is a failure bound rather than an expected duration. Every command in
 * the vocabulary is answered from memory by a host that is already serving - an observation
 * command reads a counter, and a trigger command invokes a `oneway` callback that returns
 * without waiting for the middleware - so a healthy reply arrives in well under a
 * millisecond and this bound is never approached. It exists for the two cases that are not
 * healthy: a host that has stopped serving its loop, and a host that died between the
 * readiness token and the command. Both must present as a failed assertion naming the
 * command, never as a hung suite, because a hung suite is killed from outside with nothing
 * recorded.
 *
 * Generous rather than tight on purpose: the AIDL invocation runs inside an emulated guest on
 * a loaded CI machine, and a bound tuned to a developer's host would convert that load into
 * a flake. Ten seconds is far above any plausible healthy reply and far below any CI job
 * timeout, which is the only property that matters.
 */
const int HOST_CONTROL_REPLY_TIMEOUT_MS = 10000;

/**
 * @brief Upper bound on bytes accepted while assembling one reply line.
 *
 * The longest reply in the protocol is a `last-sent` carrying a maximum-length CEC frame as
 * hex - well under a hundred characters - so this cap can only be reached by a stream that
 * is not the host's replies at all. Bounding the accumulation keeps a misdirected descriptor
 * from growing a buffer without limit inside a wait that has not yet expired, and it is the
 * same disposition, and the same value, that HOST_READINESS_MAX_BYTES applies to the
 * readiness pipe.
 */
const std::size_t HOST_CONTROL_MAX_REPLY_BYTES = 4096;

/** @brief Prefix every diagnostic line this harness prints carries. */
const char *const TRACE_PREFIX = "[CecL2TestEnvironment] ";

/*
 * Exit codes the child reports when it fails before it ever becomes the host.
 *
 * They sit deliberately outside the range the host binary uses for its own documented
 * failures, so that a reader of an exit status can tell "the host ran and refused" from
 * "the host was never reached at all" without consulting either file. The host's codes
 * are file-local to its translation unit and are not duplicated here for the same
 * reason: a second copy of that table would be a second thing to keep in step, and it is
 * not needed, because this harness reports the raw status it observed and the host's own
 * trace on standard output names the step it reached.
 */
const int CHILD_EXIT_PRE_EXEC_FAILED = 120;
const int CHILD_EXIT_EXEC_FAILED     = 121;

/**
 * @brief How the wait for the host's readiness token ended.
 *
 * Every failing value is distinguished rather than collapsed into one "not ready",
 * because the three failures have three different causes and a reader of a CI log has
 * only this to go on: a timeout says the host is still running and has not published, an
 * end of file says the host exited before publishing, and a mismatch says something
 * other than the host is on the far end of that descriptor.
 */
enum class ReadinessOutcome {
    Ready,              /**< @brief The token arrived, matched verbatim, and the host is serving. */
    TimedOut,           /**< @brief The bound expired with no token; the host is still alive.     */
    ClosedWithoutToken, /**< @brief The write end closed with no token; the host exited.          */
    TokenMismatch,      /**< @brief A complete line arrived and it was not the token.             */
    PipeError           /**< @brief The descriptor itself failed, which is none of the above.     */
};

/**
 * @brief Writes a buffer to a descriptor in full, tolerating short and interrupted writes.
 *
 * Used only by the child between fork() and exec(), where a diagnostic is the sole way a
 * pre-exec failure can say anything at all. It calls nothing that allocates and nothing
 * that takes a lock, which is the constraint that applies in that window; the messages it
 * is given are string literals with compile-time lengths, so not even strlen() is
 * involved. A failed write is discarded because there is nothing left to report it to -
 * the caller's very next act is _exit(), and the exit status carries the outcome.
 *
 * @param [in] fd                         - Descriptor to write to
 * @param [in] data                       - Buffer to write. Must not be null
 * @param [in] length                     - Number of bytes to write
 *
 * @return None
 *
 * @warning Deliberately silent on failure. Do not add reporting here; anything richer
 *          would breach the no-allocation constraint this function exists to honour.
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

/*
 * How far the fallback descriptor sweep counts when close_range() is not available.
 *
 * The primary mechanism is close_range(), which closes a whole span in one syscall and needs no
 * bound at all. The fallback is a close() loop, and a loop cannot use the descriptor limit as
 * its bound unmodified: measured on this host RLIMIT_NOFILE is 1048576, so an uncapped loop
 * would issue a million syscalls between fork() and execve(). The cap makes the fallback
 * bounded; its cost is that on a platform with no close_range() a descriptor numbered above the
 * cap would survive into the host. That is stated rather than hidden, and it is not the path
 * this project takes: glibc has provided close_range() since 2.34 and the kernel since 5.9,
 * both of which this build requires by other means already.
 */
const unsigned int DESCRIPTOR_SWEEP_FALLBACK_CAP = 65536U;

/**
 * @brief Closes an inclusive span of descriptor numbers, whether or not any of them is open.
 *
 * Post-fork safe in the sense this file uses throughout: it calls close_range(), getrlimit()
 * and close(), all of which are syscalls, and it allocates nothing and takes no lock. That is
 * the constraint that applies between fork() and execve() in a process that may have a binder
 * threadpool running, because a fork copies a lock's state as it stood and a child that took
 * one could block on a lock no thread of its own will ever release.
 *
 * Errors are deliberately ignored. Closing a descriptor that was never open fails with EBADF,
 * which is the expected result for most of the span and carries no information; and there is
 * nothing a failure could usefully be reported to, since the caller's next act is execve().
 *
 * @param [in] low                        - First descriptor number to close
 * @param [in] high                       - Last descriptor number to close, inclusive. May be
 *                                          ~0U to mean "every number there could be"
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
 * WHAT THIS IS FOR. A fork duplicates every descriptor the parent held, and this runner holds
 * more than the four it created for the host: GoogleTest's own output file when one is
 * requested, the binder driver node on an invocation that resolved the AIDL back-end, and
 * whatever earlier probes left open at the moment of the fork. None of them is the host's
 * business, and each one it inherits is a descriptor the host - and every process the host
 * goes on to start - holds open for as long as it lives. That is how a leak becomes durable:
 * a descendant holding a copy of a pipe's write end keeps the pipe from ever reporting end of
 * file, so the very death channel this harness uses to observe an exit never fires.
 *
 * WHAT IT MUST NOT CLOSE, and why the three are parameters rather than a constant here. The
 * host is TOLD three descriptor numbers in its environment and its whole contract depends on
 * them surviving the exec, which is why the caller clears O_CLOEXEC on exactly those three
 * immediately before this call. Standard input, output and error are kept as well: the host
 * writes its trace to them and this runner's log is where that trace has to land, so the sweep
 * starts one above STDERR_FILENO rather than at zero.
 *
 * Post-fork safe: a fixed three-element sort over locals plus closeDescriptorRange(). No
 * allocation, no lock, no library call that could take either.
 *
 * @param [in] keepFirst                  - A descriptor the child must keep, or negative for
 *                                          none
 * @param [in] keepSecond                 - A second such descriptor, or negative for none
 * @param [in] keepThird                  - A third such descriptor, or negative for none
 *
 * @return None
 *
 * @post In the calling process no descriptor above STDERR_FILENO other than the ones named is
 *       open, so the three named are the only ones an exec can carry into the host.
 *
 * @warning Call this only in a child between fork() and execve(). Called in the parent it
 *          would close the very descriptors the parent uses to talk to the host.
 *
 * @see closeDescriptorRange(), startFakeServiceHost()
 */
void closeInheritedDescriptorsExcept(int keepFirst, int keepSecond, int keepThird)
{
    int keep[3] = { keepFirst, keepSecond, keepThird };

    /*
     * A three-element sort, spelled out: no allocation, no comparator object and no library
     * call, because this runs after a fork. Sorting is what lets the sweep be expressed as the
     * gaps between the kept numbers, which is what makes close_range() usable at all.
     */
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
            /*
             * Either a duplicate of a number already passed, or one at or below the standard
             * streams - which the sweep never reaches in any case.
             */
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
 * The exit status is the load-bearing half of a host failure diagnostic, because the host
 * gives each failure class a code of its own and writes no readiness line for any of
 * them. It is reported as the raw number rather than translated into the host's own
 * vocabulary: those codes are file-local to the host's translation unit, and a second
 * copy of that table here would be one more thing to keep in step for no gain, since the
 * host also traces the step it reached to standard output, which this runner inherits.
 *
 * @param [in] status                     - Status filled in by waitpid()
 *
 * @return std::string                            - Human-readable description of the end
 *                                                  of the process
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
 * Called only once the child is known to be on its way out - either because end of file on
 * a descriptor it held says it has closed its descriptors, or because it has been sent
 * SIGKILL, which no process can catch, block or ignore - so the block here is short by
 * construction rather than by hope. Reaping is not optional: an unreaped child becomes a
 * zombie that outlives the suite, and the whole point of owning a child's lifecycle is
 * that nothing survives the run that started it.@n
 * Every caller honours that precondition, which is what makes this the bounded end of an
 * unbounded primitive: terminateAndReapChildProcess() reaches it only after an observed
 * exit or after its own SIGKILL, and EpipeProbeResources' destructor sends SIGKILL
 * immediately before calling it. It is never reached on the strength of a SIGTERM alone,
 * because a child is free to handle SIGTERM and one of this harness's children does.
 *
 * @param [in]  childPid                  - Pid of the child to collect
 * @param [out] description               - Receives how the process ended, or why it could
 *                                          not be collected
 *
 * @return bool                                   - Whether the process was collected
 * @retval true                                   - Reaped; description names how it ended
 * @retval false                                  - waitpid() failed; description says why
 *
 * @pre childPid names a live or exited child of this process.
 *
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

/* ---------------------------------------------------------------------------------------------
 * The disposition that makes this process's channel writes survivable, and why it is a
 * harness-wide one rather than a block around each write.
 *
 * SIGPIPE's default disposition terminates the process, and this harness has exactly one write that
 * can provoke it: writeControlCommand()'s write to the host's control descriptor, whose reader is
 * another process that can exit or close its read end at any moment. Under the default disposition
 * that write never returns - the runner is killed at the call - and two contracts this file states
 * elsewhere are lost with it:
 *
 *   The diagnostic is never produced. writeControlCommand() carries an EPIPE arm whose whole purpose
 *   is to say "the host has closed its control descriptor or exited" in the failure message of the
 *   case that asked for the observation. A signal delivers no errno to anyone, so GoogleTest records
 *   no result at all and the run reads as a crash of unknown origin - the outcome the bounded
 *   channel was designed to rule out, arrived at through the one path that bypasses the bound.
 *
 *   The host is never reaped. The global environment's TearDown is what signals the host, waits for
 *   it and collects it; a process killed by a signal runs no teardown, so a wedged or still-serving
 *   host survives the run that started it, holding the production service name and making the next
 *   run fail on a stale registration. The promise that nothing outlives the run is a promise
 *   TearDown keeps, and TearDown has to be reached.
 *
 * With the disposition installed both are restored, and the EPIPE arm becomes reachable - which is
 * the point of installing it rather than a side effect. The write returns -1 with EPIPE,
 * writeControlCommand() fails with its sentence naming the command, performHostControlRequest()
 * reports false, the calling case's assertion fails at its own line, and TearDown then reaps the host
 * and reports how it ended. One failed assertion carrying the reason, in place of a dead process.
 *
 * That whole chain is exercised rather than argued for. cecL2ProveEpipeDiagnosticAndChildReaping()
 * below builds the hazard out of a pipe and a child of its own, calls this file's own
 * writeControlCommand() on a descriptor whose reader has genuinely gone, requires the false return and
 * the EPIPE sentence naming the command, and then ends that child through the same
 * terminateAndReapChildProcess() a teardown uses. So both halves of the paragraph above are checked by
 * a test: the diagnostic is produced by the real function, and the reap that a killed process would
 * have skipped is performed by the real code and required to succeed. The one thing the seam cannot
 * check is what happens without the disposition, since establishing that would mean terminating the
 * runner; the case therefore reads the disposition back out of the process first and fails fatally if
 * it is the default, before any write is attempted.
 *
 * The scope is the whole life of the test environment: installed as the first step of SetUp and
 * undone as the last step of TearDown. A scoped block around each write would also work, and this is
 * chosen over it for two reasons rather than for brevity. First, the guarantee is asserted on both
 * invocations - the seam above runs from a fixture that skips for nothing, so it executes under the
 * legacy invocation too - and on that invocation no host is launched at all, so a disposition
 * installed beside the pipes would leave that very case unprotected and the runner would die proving
 * the property it was checking. Second, the first statement of SetUp is a place no later reordering
 * can skip, whereas an install that lives next to the launch moves whenever the launch moves.
 *
 * The host binary makes the opposite choice deliberately and it does not contradict this one: it
 * installs the disposition only where a channel was actually supplied, because it runs no test of its
 * own and every process default it can leave alone is one fewer difference between a run with a
 * channel and a run without. This harness's own test is the thing that needs the wider window.
 *
 * Nothing in the middleware under test relies on SIGPIPE's default disposition, so this changes
 * nothing for the code being exercised. CCEC's transports are an in-process C function-pointer ABI on
 * the legacy arm and binder ioctls on the AIDL arm; neither writes to a pipe or a socket, and no
 * ccec/ or osal/ source installs, blocks, raises or waits on SIGPIPE. What changes is confined to
 * this harness's own descriptors: a write to a reader-less pipe reports an error instead of killing
 * the process that made it.
 *
 * Every other descriptor operation in this file is checked rather than assumed, because "the one
 * write" is a claim and not an observation:
 *
 *   The readiness pipe. This process holds the read end only - awaitHostReadiness() and
 *   waitForChildExitViaPipeEof() poll and read it, and the write end is closed in the parent
 *   immediately after the fork. A read cannot raise SIGPIPE, so there is nothing to protect.
 *
 *   The observation pipe. Read end again, in readControlReply(), for the same reason and with the
 *   same consequence.
 *
 *   writeRawFully(). Its only calls are in the child, between fork() and execve(), and its
 *   descriptor is STDERR_FILENO. Standard error being a closed pipe would raise SIGPIPE there - and
 *   the child inherits the disposition installed here, so it is covered by the same install rather
 *   than by an argument that it cannot happen. Nothing else in this file writes to a descriptor.
 *
 *   std::cout. A C++ stream, and the only descriptor behind it is standard output, which belongs to
 *   whoever ran the binary. It is inside the protected window in any case, because the window is the
 *   whole environment.
 *
 * And that is why the original is put back. A disposition is process-wide and outlives this
 * environment: anything running after global teardown - another ::testing::Environment's TearDown, a
 * static destructor, an atexit handler - would otherwise inherit a choice this harness made and never
 * announced. Restoring keeps the change scoped to the window that needs it.
 *
 * One inheritance is worth knowing: SIG_IGN survives fork() and is preserved across execve(), so the
 * host starts life with SIGPIPE already ignored. The host installs its own disposition regardless,
 * and correctly so - a program that must not be killed by a closed pipe cannot make that depend on
 * how it was launched.
 * --------------------------------------------------------------------------------------------- */

/**
 * @brief Stops a write to a reader-less pipe from terminating this runner.
 *
 * Replaces SIGPIPE's disposition with SIG_IGN and keeps the previous one, so that a write whose
 * reader has gone reports EPIPE to its caller instead of killing the process. Idempotent: a second
 * call does nothing and keeps the disposition saved by the first, so the original can never be
 * overwritten by the replacement.
 *
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure.
 *                                          Untouched on success
 *
 * @return bool                                   - Whether SIGPIPE is now ignored
 * @retval true                                   - It is, and a write to a closed pipe fails with
 *                                                  EPIPE
 * @retval false                                  - sigaction() refused; failureDetail says why, and
 *                                                  the run must not proceed
 *
 * @warning A caller that continued on false would be running a harness whose control-channel write
 *          can terminate it, taking the host reap with it. This is a setup fault, not a degraded
 *          mode.
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
 * Idempotent and a no-op when nothing was replaced, which is what makes it safe to call from a
 * TearDown that runs after a SetUp that failed before installing anything.
 *
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure.
 *                                          Untouched on success
 *
 * @return bool                                   - Whether the original disposition is back
 * @retval true                                   - It is, or there was nothing to restore
 * @retval false                                  - sigaction() refused; failureDetail says why, and
 *                                                  SIGPIPE remains ignored process-wide
 *
 * @post Nothing that runs after this call inherits a signal disposition chosen by this harness.
 *
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
 * @brief Launches the fake service host as a child process with a readiness pipe.
 *
 * Creates the pipe, hands its write end to a forked child as an inherited descriptor,
 * names that descriptor in the child's environment and execs the host binary. On success
 * g_hostPid and g_hostReadinessReadFd are set and the caller must wait for the readiness
 * token before doing anything that resolves the back-end selection. On failure nothing is
 * left behind: every error path closes whatever it had opened, so a failed launch cannot
 * leak a descriptor into a run that then reports a different failure.
 *
 * @par Why argv and the environment are built before the fork
 * Everything the child needs is assembled here, in the parent, so that the child's path
 * from fork() to execve() calls nothing that allocates and nothing that takes a lock.
 * setenv() in the child would have been shorter and is the usual shape, but it may
 * reallocate the environment block, and the safe set of operations after a fork does not
 * include allocation. Copying the block and appending one entry costs nothing here and
 * removes the question entirely.
 *
 * @par Descriptor inheritance, stated explicitly because it is easy to get backwards
 * The pipe is created with O_CLOEXEC on both ends, and the child then clears the flag on
 * the write end alone. So the read end is never inherited - it is closed in the child and
 * would be closed again by exec - while the write end survives into the host image, which
 * is the descriptor the host writes its token to. The parent closes its own copy of the
 * write end immediately after the fork, which is what makes end of file on the read end
 * mean "the host closed its write end" rather than "the parent is still holding one open".
 * Getting that one close wrong turns a dead host into a hang.@n
 * AND EVERYTHING ELSE IS SWEPT, which is the other half of the same subject. Clearing
 * O_CLOEXEC on three descriptors says what the host MAY have; it says nothing about what a
 * fork already gave it. The child therefore closes every descriptor above the standard
 * streams other than those three immediately before the exec, so that the host - and
 * anything the host starts - inherits the three it was told about and nothing else. A
 * descendant holding an unswept copy of a pipe's write end is what stops that pipe ever
 * reporting end of file, which is how the death channel above stops reporting death.
 *
 * @par The child leads its own process group
 * The child calls setpgid() before anything else, and the parent repeats the call and then
 * reads the group back with getpgid(). That is what makes it possible to end a host that
 * forked: waitpid() collects one process, so a process the HOST started is beyond it, and
 * the only instrument that reaches such a process is a signal to the group. The read-back
 * is not ceremony - a process inherits its parent's group, so an unconfirmed group id is
 * this runner's own and signalling it would end the runner. See g_hostProcessGroupId.
 *
 * @param [in] hostPath                   - Filesystem path of the host binary, taken from
 *                                          CEC_FAKE_AIDL_HOST_PATH
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure.
 *                                          Untouched on success
 *
 * @return bool                                   - Whether the host was launched
 * @retval true                                   - Forked and exec'd; g_hostPid,
 *                                                  g_hostReadinessReadFd and, where it could
 *                                                  be confirmed, g_hostProcessGroupId are set
 * @retval false                                  - The binary is unusable, or the pipe or
 *                                                  the fork failed; nothing was left open
 *
 * @post On success a child exists and must be reaped, whether or not it ever reports ready.
 * @post On success the child holds the three descriptors named in its environment, the standard
 *       streams, and no other descriptor of this process's.
 *
 * @warning A successful return means the child was started, not that the service was
 *          published. Only the readiness token establishes that, which is why
 *          awaitHostReadiness() is not optional and its bound is not optional either.
 *
 * @see awaitHostReadiness(), terminateAndReapFakeServiceHost()
 */
bool startFakeServiceHost(const std::string &hostPath, std::string &failureDetail)
{
    /*
     * Checked here, before the fork, purely so the common misconfiguration - a path that
     * is empty, misspelled, or names something that was never built - produces a sentence
     * naming the path instead of an exec failure inside a child whose only channel is an
     * exit status. The child still handles its own exec failure below, because this check
     * cannot be conclusive: the file could stop being executable between the two, and a
     * test harness that pretends otherwise would be wrong in the one case that matters.
     */
    if (::access(hostPath.c_str(), X_OK) != 0) {
        failureDetail = renderUntrustedValue(hostPath) + ", named by " + HOST_PATH_VARIABLE +
                        ", is not an executable file (" + std::strerror(errno) +
                        "). The build sets this variable to the fake_hdmi_cec_aidl_host "
                        "binary it built alongside this runner; a stale, unbuilt or "
                        "hand-edited value is the usual cause";
        return false;
    }

    /*
     * O_CLOEXEC on both ends at creation, atomically. pipe() followed by two fcntl() calls
     * would leave a window in which a concurrently forked process inherits a descriptor
     * neither end intended to share, and while this harness is single-threaded at this
     * point, relying on that is a property of today's call site rather than of this
     * function.
     */
    int readinessPipe[2] = { -1, -1 };
    if (::pipe2(readinessPipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("the readiness pipe could not be created: ") +
                        std::strerror(errno);
        return false;
    }

    const int readFd  = readinessPipe[0];
    const int writeFd = readinessPipe[1];

    /*
     * The control and observation channel: two more pipes, in opposite directions, created
     * the same way and for the same reason. One descriptor cannot serve both directions -
     * the host refuses a configuration in which both variables name the same number,
     * because it would have the host reading its own replies - so this is two pipes and
     * four descriptors, of which each process keeps two.
     *
     *   control:  this harness holds the write end, the child inherits the read end
     *   observe:  this harness holds the read end,  the child inherits the write end
     *
     * Either creation failing closes everything already open before returning, so a failed
     * launch leaks no descriptor into a run that then reports a different failure.
     */
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

    /*
     * argv is the path and nothing else: the host takes no arguments and reads its whole
     * configuration from the environment, so there is no argument contract for the two
     * files to keep in step.
     */
    std::string hostPathForChild = hostPath;
    char *const childArgv[] = { const_cast<char *>(hostPathForChild.c_str()), nullptr };

    /*
     * The child's environment: this process's own, minus any inherited value of the three
     * descriptor variables, plus the three this launch is naming. Dropping the inherited
     * values matters - a stale descriptor number from an outer harness would otherwise be
     * ambiguous, and the host, quite correctly, refuses to guess between two.
     *
     * All three are set together. The host treats one channel variable without the other as
     * a hard failure, so a partial handoff is not a degraded channel that might still be
     * useful; it is a run that cannot start, which is the correct outcome and not one to
     * arrive at by accident.
     *
     * The strings are all appended before any pointer into them is taken. Growing the
     * vector afterwards would move the std::string objects, and a short string keeps its
     * characters inside the object, so the pointers would dangle.
     */
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
        /*
         * In the child. Nothing from here to execve() allocates or locks. Both messages below are
         * string literals whose length is known at compile time, so even strlen() is out
         * of the picture, and _exit() is used rather than exit() so that no copy of the
         * parent's stdio buffers is flushed a second time.
         */
        ::close(readFd);

        /*
         * The two ends of the channel that belong to the parent are closed here as well.
         * Leaving the control write end open in the child would mean the host never sees
         * end of file when the parent closes its own copy, so a parent that went away
         * without saying so would leave the host serving a channel nobody drives.
         */
        ::close(controlParentWriteFd);
        ::close(observeParentReadFd);

        /*
         * THE CHILD MAKES ITSELF A PROCESS GROUP LEADER, and this is the single change that
         * lets teardown reach anything the host goes on to start.
         *
         * Without it the host runs in THIS RUNNER'S process group, because that is what a fork
         * inherits. Teardown could then only ever signal the one pid it forked, so a host that
         * forked - or a host path that is a shell script, which forks by its nature - left its
         * own children running after the runner had reaped it and reported success: real
         * processes, holding real copies of this harness's pipes, with the readiness pipe's
         * write end among them, which is precisely the descriptor whose closure teardown reads
         * as "the host has gone". And the group-wide signal that would have collected them was
         * unavailable in the strongest possible sense: with the child in the runner's own
         * group, kill() to the negation of that group id would have killed the runner.
         *
         * BOTH SIDES CALL setpgid(), here and in the parent immediately after the fork, which
         * is the standard idiom and not redundancy. Whichever runs first establishes the group;
         * the other finds the work done and fails harmlessly - the parent with EACCES once the
         * child has exec'd, this call never, since a process may always create its own group.
         * One side alone leaves a window: only the parent, and the child could exec first and
         * be signalled in the runner's group; only the child, and the parent could reach
         * teardown before the child had run at all.
         *
         * A FAILURE HERE REFUSES THE LAUNCH rather than continuing without a group. Carrying on
         * would leave the host in the runner's group with g_hostProcessGroupId unconfirmed, so
         * teardown would silently drop back to signalling one pid - the exact behaviour this
         * exists to end, arrived at without a word. setpgid() is on POSIX's list of functions
         * safe to call after a fork, and it allocates nothing, so it belongs in this window.
         */
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

        /*
         * The step the host's own contract singles out as the one most likely to be
         * forgotten, and it is forgotten silently: all four descriptors were created with
         * O_CLOEXEC, so without clearing the flag here the two the host is about to be
         * told about would not survive its exec. The host would then find the numbers named
         * in its environment closed, refuse to start with a code of its own, and write no
         * readiness token - a loud failure rather than a quiet one, which is the right
         * direction, but a failure nonetheless. It is done for exactly the two descriptors
         * the child keeps and for no others.
         */
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

        /*
         * AND EVERYTHING ELSE THIS PROCESS HAD OPEN GOES, so that the three descriptors named
         * in the child's environment are the only ones the exec carries in.
         *
         * The three explicit closes above deal with the ends of this launch's own pipes that
         * belong to the parent. They are not the whole inheritance: a fork duplicates every
         * descriptor, and this runner holds others - GoogleTest's output file when one was
         * requested, the binder driver node on an invocation that resolved the AIDL back-end,
         * and any descriptor an earlier probe still held at the moment of the fork. Each one
         * the host inherits it keeps for its lifetime and passes to anything it starts, and a
         * copy of a pipe's write end held by a process nobody is watching is how a death
         * channel stops reporting death.
         *
         * It runs AFTER the three F_SETFD calls above, deliberately: the sweep's keep-set and
         * the set whose O_CLOEXEC was just cleared are the same three descriptors, and keeping
         * the two adjacent is what makes a future edit to one obviously an edit to the other.
         */
        closeInheritedDescriptorsExcept(writeFd, controlChildReadFd, observeChildWriteFd);

        ::execve(childArgv[0], childArgv, childEnvironment.data());

        /*
         * Only reachable when execve() failed, since a successful one does not return.
         * The parent sees this as end of file on the readiness pipe together with this
         * exit status, which its diagnostic distinguishes from a host that ran and
         * refused.
         */
        static const char execMessage[] =
            "[FakeHdmiCecAidlHost:child] the fake service host binary could not be "
            "executed; the path named by CEC_FAKE_AIDL_HOST_PATH is not runnable\n";
        writeRawFully(STDERR_FILENO, execMessage, sizeof(execMessage) - 1);
        ::_exit(CHILD_EXIT_EXEC_FAILED);
    }

    /*
     * THE PARENT'S HALF OF THE BOTH-SIDES setpgid() IDIOM, and it is the first thing the parent
     * does so that the window in which the child could be signalled in the runner's group is as
     * short as the kernel allows.
     *
     * Two failures are expected and neither is an error. EACCES means the child has already
     * exec'd, which can only happen after its own setpgid() succeeded - the group exists, and
     * this call had nothing left to do. ESRCH means the child has already exited, in which case
     * there is no group to create and nothing to signal. Anything else is worth a trace,
     * because it would mean the group is in a state neither side established.
     */
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

    /*
     * AND THE GROUP IS READ BACK RATHER THAN ASSUMED, which is what makes a group-wide signal
     * safe to send later. Two things must hold before this harness will ever signal a group:
     * the child's group id must equal its pid, so the group contains the child and its
     * descendants and nothing else; and it must differ from this process's own group, because
     * the group a fork inherits is the runner's and signalling that would kill the runner. When
     * either fails - including when getpgid() itself fails, which it does with ESRCH for a
     * child that has already exited and been collected - the group id stays -1 and teardown
     * signals one pid, exactly as it did before this was introduced.
     */
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

    /*
     * In the parent. Closing this copy of the write end is what gives the read end a meaningful
     * end of file: while the parent holds one open, a dead child produces no EOF and the
     * wait below would sit out its whole timeout diagnosing the wrong thing.
     */
    ::close(writeFd);

    /*
     * And the two channel ends that belong to the child, for the same reason in both
     * directions: while this process holds the control read end open, the host would not
     * see end of file if it closed its write end; and while it holds the observation write
     * end open, a dead host would not produce end of file on the read end, so a bounded
     * wait for a reply would sit out its whole timeout diagnosing the wrong thing.
     */
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


/*
 * renderForDiagnostic() USED TO LIVE HERE, and it was deleted rather than kept beside
 * renderUntrustedValue(). It dropped a TRAILING newline and truncated at 120 characters,
 * which reads like the same job but is not: a newline anywhere other than at the very end
 * survived it untouched, so bytes arriving from the host process - another process, whose
 * output this harness does not control - still ended the diagnostic and began a line of
 * their own. Every one of its ten call sites now uses renderUntrustedValue() above, and the
 * manual \"...\" each of them wrapped it in is gone with it, because that helper delimits its
 * own output. Two renderers with different guarantees is exactly the divergence the
 * five-copy contract exists to prevent, so there is one.
 */

/**
 * @brief Blocks until the host reports ready, or the bound expires.
 *
 * A genuine blocking wait on the readiness pipe, bounded against a monotonic deadline.
 * There is no sleep and no polling interval anywhere in it: poll() is given the time
 * remaining until the deadline and returns the moment a byte arrives, so a healthy host is
 * noticed immediately and an unhealthy one is given the full bound and no more. The
 * deadline is taken from steady_clock rather than accumulated from per-iteration
 * timeouts, so an interrupted poll() resumes against the original deadline and cannot
 * extend the bound by being interrupted repeatedly - and steady_clock in particular
 * because a wall-clock adjustment mid-run must not shorten or lengthen it.
 *
 * @param [out] observed                  - Receives what actually arrived: the complete
 *                                          first line on a match or a mismatch, whatever
 *                                          partial bytes had accumulated on a timeout or
 *                                          an end of file, or the failing call's errno text
 *
 * @return ReadinessOutcome                       - How the wait ended
 * @retval ReadinessOutcome::Ready                - The token arrived and matched verbatim
 * @retval ReadinessOutcome::TimedOut             - The bound expired; the host is alive but
 *                                                  has not published
 * @retval ReadinessOutcome::ClosedWithoutToken   - The write end closed with no token; the
 *                                                  host exited, and its exit status says why
 * @retval ReadinessOutcome::TokenMismatch        - A complete line arrived that was not the
 *                                                  token
 * @retval ReadinessOutcome::PipeError            - poll() or read() failed on the descriptor
 *
 * @pre startFakeServiceHost() has reported success, so g_hostReadinessReadFd is open and
 *      this process has closed its own copy of the write end.
 *
 * @warning Reporting anything other than Ready means the AIDL invocation cannot be run.
 *          The caller must fail the run rather than continue: continuing would leave the
 *          selection on the legacy back-end and describe the result as an AIDL one.
 *
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

        /*
         * POLLHUP is reported whether or not it was requested, and it can arrive together
         * with buffered data, so the outcome is decided by what read() returns rather than
         * by the event bits: pending bytes are consumed first and end of file is only
         * concluded from a zero-length read.
         */
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

        /*
         * Matched only once a whole line is in hand. A partial read that delivered the
         * token's name without its newline is not readiness, and treating it as such would
         * accept a truncated write as a published service.
         */
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

/*
 * Bytes read from the observation pipe that arrived after the newline terminating the reply
 * they were read with, held for the next request.
 *
 * A read() takes whatever is available rather than exactly one line, so a host that wrote
 * two replies before this process read either would otherwise have its second reply
 * discarded and every subsequent request would answer the previous request's line. Keeping
 * the surplus is what makes the framing hold regardless of how the bytes happen to be split,
 * which is the same property the host's own reader has and for the same reason.
 *
 * A healthy session never puts anything here, because the protocol is strictly one reply per
 * command and this client sends the next command only after reading the previous reply.
 */
std::string controlReplyResidual;

/**
 * @brief Serialises control requests against each other.
 *
 * GoogleTest runs cases sequentially and no case starts a thread of its own, so nothing in
 * this binary issues two requests at once today. The lock is here because the consequence of
 * that ever changing is not a failed assertion but a desynchronised channel - two commands
 * interleaved on the wire, each reading the other's reply - and a defect of that shape would
 * be blamed on the transport under test rather than on the harness. One uncontended lock per
 * request costs nothing measurable and removes the failure mode.
 */
std::mutex controlRequestMutex;

/**
 * @brief Writes one command line to the host's control descriptor, bounded by a deadline.
 *
 * Bounded rather than blocking, because the descriptor is a pipe whose reader is another
 * process: a host that has stopped reading would otherwise fill the pipe buffer and block
 * this process indefinitely, which presents as a hung suite. poll() for writability against
 * the remaining time is what turns that into a reported failure.
 *
 * @par Why the descriptor is a parameter and not the module-scope one
 * So that the EPIPE arm below can be driven rather than argued about. That arm exists only
 * because SIGPIPE is ignored, and the case that checks the ignore is in force -
 * tests/L2Tests/ccec/test_DualPathIntegration.cpp's broken-pipe case, through the
 * cecL2ProveEpipeDiagnosticAndChildReaping() seam below - has to make a real call to this
 * very function against a descriptor whose reader has genuinely gone. A function that could only
 * ever write to the live channel could not be called that way: the live channel's reader is
 * the fake service host, and breaking it to prove a diagnostic would destroy the session the
 * rest of the invocation depends on. Taking the descriptor as an argument is the whole of the
 * change; the deadline, the poll, the EINTR handling, the POLLERR fall-through and the two
 * failure sentences are untouched, because a seam that exercised a modified copy of this
 * function would prove nothing about the one the harness actually uses.
 *
 * @param [in]  line                      - Command text without its terminator
 * @param [in]  controlWriteFd            - Descriptor to write to, open for writing. The live
 *                                          call site passes g_hostControlWriteFd; the seam
 *                                          passes a pipe of its own whose reader has gone
 * @param [in]  deadline                  - Monotonic instant after which this gives up
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on
 *                                          success
 *
 * @return bool                                   - Whether the whole line and its terminator
 *                                                  were written
 * @retval true                                   - The host has the command in its pipe
 * @retval false                                  - The bound expired, the host closed its
 *                                                  read end, or the descriptor failed
 *
 * @pre controlWriteFd is open for writing.
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

        /*
         * POLLERR here means the host has closed its read end, which for a write is
         * reported as EPIPE by the write() below; it is not treated separately, so that
         * there is one diagnostic for "the host has gone" rather than two that differ by
         * which call noticed first.
         *
         * That fall-through is only sound because SIGPIPE is ignored. Under its default
         * disposition this write() would not return at all on a closed reader: the process
         * would be terminated here, the EPIPE arm below would never run, and the teardown
         * that signals and reaps the host would never run either. ignoreBrokenPipeSignal()
         * installs the disposition as the first step of the environment's SetUp and
         * restoreBrokenPipeSignalDisposition() undoes it in TearDown; the reasoning is
         * written out in full above that pair. If the install is ever removed, the arm
         * below becomes dead code and this fall-through becomes a way to die silently.
         */
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
 * Assembles bytes until the first newline, starting from whatever a previous request read
 * past its own terminator. End of file before a terminator is a distinct outcome from the
 * bound expiring and is reported as such: the first says the host exited, the second says it
 * is alive and did not answer, and a reader of a CI log has only this to tell them apart.
 *
 * @param [in]  command                   - Command this reply answers, for the diagnostic
 * @param [in]  deadline                  - Monotonic instant after which this gives up
 * @param [out] reply                     - Receives the reply line without its terminator.
 *                                          Untouched on failure
 * @param [out] failureDetail             - Receives a diagnostic on failure. Untouched on
 *                                          success
 *
 * @return bool                                   - Whether one complete reply line was read
 * @retval true                                   - reply holds it, terminator stripped
 * @retval false                                  - The bound expired, the host closed its
 *                                                  write end, the descriptor failed, or the
 *                                                  accumulation exceeded its cap
 *
 * @pre g_hostObserveReadFd is open.
 *
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
 * The whole of the client half of the protocol, in one place, so that the framing this
 * harness produces and the framing the host expects cannot drift apart across functions.
 *
 * @param [in]  command                   - Command text without a terminator, e.g. "sent-count"
 * @param [out] reply                     - Receives the reply line without its terminator
 * @param [out] failureDetail             - Receives a diagnostic when this reports failure
 *
 * @return bool                                   - Whether one reply was exchanged
 * @retval true                                   - reply holds the host's answer, which may
 *                                                  legitimately be an "ERR " line
 * @retval false                                  - No answer was obtained; failureDetail says why
 *
 * @warning An "ERR " reply is a successful exchange and reports true. Whether the host's answer
 *          is acceptable is the calling test's judgement, not this function's, and collapsing
 *          the two would deny a test the ability to assert on an expected refusal.
 *
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

    /*
     * The command is validated before a byte is written, because every way it can be
     * malformed breaks the framing rather than producing a refusal. An embedded newline would
     * send two commands and read the first of two replies, leaving every subsequent request
     * one reply behind - a desynchronisation that presents as unrelated cases failing later.
     * An empty or whitespace-only line produces no reply at all by the host's own contract,
     * so a bounded wait would expire on a command that was accepted. Both are caller
     * mistakes, and both are reported here as such.
     */
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

    /*
     * Every reply in the protocol begins "OK " or "ERR ". A line that does neither is not a
     * reply this client can classify, and accepting it would let a desynchronised channel look
     * like a well-formed refusal.
     */
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
 * Idempotent, and safe after a partial launch: a run that created the pipes and then failed to
 * fork holds descriptors with no host on the other end, and they are released here just the
 * same. Closing the control write end is also how a host that is still serving learns the
 * session is over - it reads end of file and shuts down cleanly - so this is part of the
 * teardown contract and not merely descriptor hygiene.
 *
 * @return None
 *
 * @post Both descriptors are closed and set to -1, so performHostControlRequest() reports "no
 *       channel" rather than writing to a descriptor this process no longer owns.
 *
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
 * Bounded, and free of any timer: while the child is alive it holds the write end of the
 * pipe open, so end of file on the read end happens when and only when the child process
 * has gone. Waiting on that is a real wait on the real event, which is why it is preferred
 * over waitForChildExitByPolling(): a WNOHANG poll around a sleep asks repeatedly instead
 * of being told, so it notices a prompt exit up to one interval late and does a little work
 * while nothing is happening. Both are bounded by their timeoutMs; this one is simply the
 * more precise of the two, and it is available only for a child that holds such a pipe.
 *
 * The descriptor is a parameter rather than the module-scope readiness one so that this
 * function serves every child this harness owns: the host's death channel is its readiness
 * pipe, and the broken-pipe seam's death channel is the handshake pipe it creates for its
 * own child. Both are the same event observed the same way, and one implementation of it is
 * what keeps the two from drifting.
 *
 * @param [in] pipeReadFd                 - Read end of a pipe whose only write end the child
 *                                          holds. A negative value means no death channel
 *                                          exists, which is reported as "not observed"
 * @param [in] timeoutMs                  - Longest time to wait, in milliseconds
 *
 * @return bool                                   - Whether the child was observed to exit
 * @retval true                                   - End of file arrived; the child has exited
 *                                                  and a blocking reap will return at once
 * @retval false                                  - The bound expired, or the descriptor
 *                                                  failed, or none was ever opened
 *
 * @note This is the preferred of the two bounded waits and not the only one. A child that
 *       was never given a death channel is waited for by waitForChildExitByPolling()
 *       instead, so that the bound belongs to terminateAndReapChildProcess() rather than
 *       to what its caller happened to have available.
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

        /*
         * Bytes rather than end of file: the child wrote something after whatever this
         * harness had already consumed from the descriptor. It is no part of any contract,
         * so it is drained and the wait continues.
         */
    }
}

/**
 * @brief How long to sleep between two WNOHANG waits while polling for a child's exit.
 *
 * Ten milliseconds, and the number matters only as the worst case by which a poll notices
 * an exit that has already happened: one interval. Against the grace periods this file
 * uses - five seconds for the fake service host, two for the broken-pipe probe child -
 * that lateness is invisible, and an ordinary termination costs a handful of wake-ups
 * rather than a busy loop. A much smaller value would spin a core to shave a delay
 * nothing measures; a much larger one would make a prompt exit look slow and, on the
 * escalation path, delay the SIGKILL past the grace period the caller asked for.
 *
 * It is deliberately not an alternative to the end-of-file channel. Where a child holds a
 * pipe open for its lifetime, waitForChildExitViaPipeEof() waits on the real event with no
 * interval at all and is what terminateAndReapChildProcess() uses; this interval governs
 * only the fallback for a child that was given no such channel, where there is no event to
 * wait on and the choice is between polling and not bounding the wait at all.
 */
const int CHILD_EXIT_POLL_INTERVAL_MS = 10;

/**
 * @brief How a bounded poll for a child's exit ended.
 *
 * Three outcomes rather than a boolean, because the caller must do three different things
 * with them and two of the three are not "the child is gone". A collected child has
 * already been reaped by the poll itself and must not be waited for again; a child still
 * running has to be escalated to SIGKILL; and a wait that was refused means this process
 * cannot collect that pid at all, which is a reported failure rather than something a
 * stronger signal would fix.
 */
enum class PolledChildExit {
    Collected,    /**< @brief Exited within the bound and was reaped here; no further wait is owed. */
    StillRunning, /**< @brief The bound expired with the child alive; it has not been reaped.       */
    WaitFailed    /**< @brief waitpid() refused, so this process cannot collect that pid.           */
};

/**
 * @brief Waits, bounded, for a child to exit by polling waitpid(), and reaps it if it does.
 *
 * The fallback bounded wait, for a child that holds no pipe this harness can watch for end
 * of file. It exists so that the bound on terminate-and-reap is a property of
 * terminateAndReapChildProcess() itself rather than of whether its caller happened to have
 * a death channel to pass: without it, a child with no channel would be waited for by an
 * unbounded blocking reap, and for any child that handles, blocks or ignores SIGTERM - the
 * fake service host installs a SIGTERM handler - that wait has no bound at all.
 *
 * It is the second choice and not the first, which is why waitForChildExitViaPipeEof()
 * still exists and is still what a child with a channel gets. End of file on a pipe is the
 * real event, observed the moment it happens; a WNOHANG poll around a sleep is a repeated
 * question, so it notices an exit up to one CHILD_EXIT_POLL_INTERVAL_MS late and does a
 * small amount of work while nothing is happening. Both are bounded by the same
 * timeoutMs, which is the property the caller's guarantee rests on.
 *
 * Reaping here rather than reporting "it has exited" and leaving the collection to the
 * caller is forced by the mechanism: WNOHANG cannot tell that a child has exited without
 * collecting it, so the poll that observes the exit is the reap. That is why Collected is
 * distinguished from StillRunning at all - a caller that went on to wait again on a pid
 * this call had already collected would get ECHILD and would have to report a child it in
 * fact collected as one it lost.
 *
 * @param [in]  childPid                  - Pid of the child to wait for. Must be positive;
 *                                          the caller refuses a non-positive pid before it
 *                                          signals anything
 * @param [in]  timeoutMs                 - Longest time to wait, in milliseconds. A
 *                                          non-positive value performs exactly one
 *                                          non-blocking check, which is the honest reading
 *                                          of a zero-length grace period
 * @param [out] description               - Receives how the process ended on Collected, or
 *                                          why it could not be collected on WaitFailed. Left
 *                                          untouched on StillRunning, where the caller's
 *                                          escalation and blocking reap produce it instead
 *
 * @return PolledChildExit                        - Which of the three outcomes occurred
 * @retval PolledChildExit::Collected             - Exited within the bound and was reaped;
 *                                                  the caller owes it no further waitpid()
 * @retval PolledChildExit::StillRunning          - The bound expired; the child is alive and
 *                                                  unreaped, and SIGKILL is the next step
 * @retval PolledChildExit::WaitFailed            - waitpid() refused - in practice ECHILD,
 *                                                  meaning the pid is not this process's
 *                                                  child or has already been collected
 *
 * @pre childPid is a child of this process that has been signalled, or is about to be.
 *
 * @post On Collected no child exists for childPid and nothing further may be waited on it.
 *
 * @warning Interrupted waits are retried rather than reported: EINTR here means a signal
 *          arrived, not that the child is unreachable, and a wait that gave up on it would
 *          hand back StillRunning for a child that had exited and escalate to SIGKILL for
 *          no reason.
 *
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
            /*
             * Collected, and the status is this child's: waitpid() asked for one positive
             * pid returns that pid, 0 or -1 and nothing else. The test is on the sign
             * rather than on equality with childPid because those three are the whole of
             * the domain, and an equality test would imply a fourth branch - "some other
             * pid was collected" - that cannot occur and that nothing here could do
             * anything useful about.
             */
            description = describeWaitStatus(status);
            return PolledChildExit::Collected;
        }

        if (observed < 0) {
            if (errno == EINTR) {
                continue;
            }

            /*
             * The same sentence reapChildBlocking() produces for the same condition, so
             * that an outcome string reads identically whichever of the two waits reported
             * it. In practice this is ECHILD: the pid is not a child of this process, or
             * something else has already collected it.
             */
            description = std::string("could not be reaped: ") + std::strerror(errno);
            return PolledChildExit::WaitFailed;
        }

        /*
         * Zero: the child exists and has not changed state. The deadline is re-read from
         * the monotonic clock rather than counted in iterations, so a sleep the kernel cut
         * short by a signal, or ran long under load, cannot stretch or shrink the bound.
         */
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());

        if (remaining.count() <= 0) {
            return PolledChildExit::StillRunning;
        }

        /*
         * poll() with no descriptors is this file's sleep: it is already the primitive
         * every other bounded wait here uses, it needs nothing this translation unit does
         * not already include, and it takes its interval in the same milliseconds as every
         * other bound in the file. The interval is clamped to what is left of the deadline
         * so the last sleep cannot overrun it, and a sleep interrupted by a signal simply
         * returns early to a loop that re-checks both the child and the clock.
         */
        const long long remainingMs = static_cast<long long>(remaining.count());
        const int sleepMs           = (remainingMs < CHILD_EXIT_POLL_INTERVAL_MS)
                                          ? static_cast<int>(remainingMs)
                                          : CHILD_EXIT_POLL_INTERVAL_MS;

        ::poll(nullptr, 0, sleepMs);
    }
}

/*
 * How long a process group is given to empty out after the direct child has been reaped, before
 * the whole group is sent SIGKILL.
 *
 * It is short because by this point every member has already had SIGTERM and the group's leader
 * has been collected: what is left is either a descendant still finishing its own shutdown or
 * one that is not going to. A few tens of milliseconds distinguishes those two on any machine
 * this runs on, and the cost of guessing low is only that a well-behaved descendant is killed a
 * moment before it would have exited - which is not a correctness difference, because SIGKILL is
 * where this path was always going to end.
 */
const int PROCESS_GROUP_SETTLE_MS = 200;

/*
 * And how long it is given to empty out after SIGKILL, which is a different question.
 *
 * SIGKILL cannot be caught, blocked or ignored, so this is not a grace period: it is how long
 * the harness will wait for the KERNEL to finish, which after the signal means the time for each
 * member to be torn down and for its parent - init, or whatever subreaper adopted it - to
 * collect it. Descendants are not this process's children, so waitpid() cannot collect them and
 * cannot be used to observe them going; the group probe is the only observation available, and a
 * group entry persists while any member is still a process, a zombie included. Two seconds is
 * far beyond what that takes and is a bound rather than a wait: a group still populated at the
 * end of it is reported as a leak with its state named, which is the honest outcome.
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
 * The null signal is the whole mechanism: kill() with a signal number of zero performs every
 * check a real signal would - including that the target exists - and delivers nothing. Applied
 * to the negation of a group id it therefore answers "does this group still have a member",
 * which is the only question available about processes that are not this process's children.
 * waitpid() cannot answer it: a grandchild is not a child, so there is nothing for this process
 * to wait on, and its exit is reported to whoever adopted it instead.
 *
 * An unrecognised errno is reported as Populated rather than Empty, deliberately. Every use of
 * this treats Empty as "the guarantee holds", so a state this function does not understand must
 * not be able to produce that answer.
 *
 * @param [in] groupId                    - Process group id, positive. Negated internally, so
 *                                          callers pass the group id itself
 *
 * @return ProcessGroupState                      - What the probe found
 *
 * @warning Pass the group id, not its negation. A caller that negated it first would be asking
 *          about a group whose id is a negative number, which cannot exist, and would get
 *          Empty for every group.
 *
 * @see awaitProcessGroupEmpty(), describeProcessGroupState()
 */
ProcessGroupState probeProcessGroup(pid_t groupId)
{
    if (groupId <= 0) {
        /*
         * Not a group this harness ever owns. Reported as NotPermitted rather than Empty for the
         * same reason an unrecognised errno is: only a real, confirmed, empty group may produce
         * the answer callers act on.
         */
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
 * Polls probeProcessGroup() on the same interval and against the same monotonic deadline as
 * waitForChildExitByPolling(), and for the same reason: the deadline is re-read from the clock
 * rather than counted in iterations, so a sleep the kernel cut short or ran long cannot stretch
 * or shrink the bound. poll() with no descriptors is this file's sleep.
 *
 * @param [in]  groupId                   - Process group id to watch, positive
 * @param [in]  boundMs                   - How long to wait, in milliseconds
 * @param [out] finalState                - Receives the last state observed, so a caller
 *                                          reporting a failure can name what it found
 *
 * @return bool                                   - Whether the group is empty
 * @retval true                                   - It is: every member has gone and been
 *                                                  collected
 * @retval false                                  - The bound expired with members still there,
 *                                                  or with members this process may not signal;
 *                                                  finalState says which
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
 * The whole of the terminate-and-reap contract, in one place and free of module-scope state, so
 * that every child this harness creates ends the same way and the guarantee "nothing this run
 * started outlives it" has exactly one implementation. terminateAndReapFakeServiceHost() below
 * is one caller and cecL2ProveEpipeDiagnosticAndChildReaping() is the other; the second exists
 * so that the reaping the SIGPIPE disposition is installed to make reachable is driven by a
 * test rather than asserted about in a comment, and it drives this very function rather than a
 * copy of it, because a copy would prove nothing about the path teardown actually takes.
 *
 * SIGTERM first, because a child that installs a handler for it - the fake service host does -
 * gets to run its own clean shutdown. SIGKILL only after the bounded wait expires, so that a
 * child wedged somewhere this harness cannot reach still cannot survive: an orphaned host would
 * hold the production service name and make the next run fail on a stale registration, turning
 * one failure into a series of them.
 *
 * @par The bound is this function's, on every path
 * The wait and the escalation run unconditionally, and the observation descriptor selects only
 * how the grace period is spent, never whether there is one. With a descriptor the wait is
 * end of file on a pipe the child holds for its lifetime - the real event, observed the instant
 * it happens, and the path the broken-pipe seam exercises. Without one the same gracePeriodMs
 * is spent polling waitpid() with WNOHANG, which notices an exit up to one
 * CHILD_EXIT_POLL_INTERVAL_MS late and reaps it in the act of noticing. Either way the grace
 * period is bounded by gracePeriodMs, and the final blocking reap is bounded because it is
 * reached only after the child has been observed to exit or has been sent SIGKILL, which no
 * process can catch, block or ignore.@n
 * That is a deliberate change of where the guarantee lives. An earlier form ran the wait and the
 * escalation only when a descriptor was supplied and let an unbounded blocking reap serve as the
 * wait otherwise, justified by the claim that a child without a channel leaves SIGTERM at
 * SIG_DFL. That claim is a property of particular children rather than of this function - the
 * fake service host installs a SIGTERM handler, so for that child SIGTERM is not
 * default-terminating - and it made a termination guarantee depend on an invariant held in
 * whichever caller assigned the descriptor. A child that handles or ignores SIGTERM is now
 * bounded whether or not it was given a death channel, and no caller has to know that to be
 * safe.
 *
 * @par The group, and why reaping the child was never the whole guarantee
 * waitpid() collects one process, and the one it collects is the only one a caller knew about.
 * A child that forked - or one whose path is a shell script, which forks by its nature - leaves
 * processes this harness never created and cannot wait on: they are reparented to init, keep
 * running, and keep every descriptor they inherited, copies of the pipes this runner uses to
 * observe an exit among them. The signal is the only instrument that reaches them, and it
 * reaches them only through the process group, which is why startFakeServiceHost() makes its
 * child a group leader and why this function takes the group id as a parameter.@n
 * So the sequence is group-wide from end to end where a group was supplied: SIGTERM to the
 * group, the bounded wait for the direct child, SIGKILL where it did not go, the reap of the
 * direct child, and then a probe of the group that must find it empty. The probe is what makes
 * "no descendant outlived this run" a checked fact rather than an intention: a grandchild is not
 * a child, so no wait can confirm it is gone, and kill() to the group failing with ESRCH is the
 * one observation available that says the group has no members left. A group still populated
 * after SIGKILL is reported as a failure with its state named.@n
 * WHERE NO GROUP IS SUPPLIED nothing about the old behaviour changes: one pid is signalled, one
 * pid is waited for, one pid is reaped, and no group is probed. That is the correct treatment
 * for a child the caller created itself and did not put in a group - the broken-pipe probe's
 * child is one, and it neither forks nor execs, so it can have no descendants to collect.
 *
 * @param [in]  childPid                  - Pid of the child to end. Must be positive
 * @param [in]  description               - How to name the child in traces and in the outcome,
 *                                          e.g. "the fake HDMI CEC AIDL service host"
 * @param [in]  exitObservationFd         - Read end of a pipe whose only write end the child
 *                                          holds, used to observe its exit without a timer.
 *                                          Negative when the child has no such channel, in
 *                                          which case the same grace period is served by
 *                                          polling waitpid() instead. The wait is never
 *                                          skipped
 * @param [in]  gracePeriodMs             - How long the child has to exit after SIGTERM before
 *                                          SIGKILL is sent. Applied on both paths
 * @param [in]  childProcessGroupId       - Process group the child leads, where the caller
 *                                          established one and read it back. Signals then go to
 *                                          the whole group and the group is required to be empty
 *                                          at the end. Pass a non-positive value when the child
 *                                          leads no group of its own, and one pid is signalled,
 *                                          waited for and reaped exactly as before
 * @param [out] outcome                   - Receives how the child ended, or why it could not
 *                                          be collected, in both cases as a phrase
 *
 * @return bool                                   - Whether the child was collected AND its
 *                                                  group, where one was given, is empty
 * @retval true                                   - Reaped, by the polling wait or by the
 *                                                  blocking reap; outcome names how it ended,
 *                                                  and no member of its group remains
 * @retval false                                  - childPid was not a usable pid, or waitpid()
 *                                                  refused, or the group still had members after
 *                                                  SIGKILL; outcome says which
 *
 * @post No child exists for childPid, so nothing this call was given can become a zombie -
 *       except where waitpid() itself refused the pid, which is reported as false because no
 *       signal can remedy it.
 * @post Where a group was supplied and this returned true, kill() to that group fails with
 *       ESRCH, so no process the child started - at any depth - is still present.
 *
 * @warning The caller owns the descriptors. This function closes none of them, because the two
 *          callers hold different ones for different lifetimes and a close here would be
 *          invisible to whichever of them still needed it.
 * @warning A child collected by the polling wait must not be waited on again: a second
 *          waitpid() on a reaped pid fails with ECHILD, which would report a child that was in
 *          fact collected as one this harness lost. The blocking reap below is therefore
 *          skipped on exactly that path, and on no other.
 * @warning childProcessGroupId must be a group the CALLER established and read back, never one
 *          it assumed. Every process starts in its parent's group, so an unconfirmed group id is
 *          this runner's own, and a signal to it would end the runner, GoogleTest and every
 *          sibling process sharing the group. This function refuses that case rather than
 *          trusting the caller - it compares the id against getpgrp() and drops to signalling the
 *          single pid where they match - but a caller that passes an unconfirmed value is relying
 *          on a guard instead of on a fact.
 *
 * @see reapChildBlocking(), waitForChildExitViaPipeEof(), waitForChildExitByPolling(),
 *      awaitProcessGroupEmpty(), probeProcessGroup()
 */
bool terminateAndReapChildProcess(pid_t childPid, const std::string &description,
                                  int exitObservationFd, int gracePeriodMs,
                                  pid_t childProcessGroupId, std::string &outcome)
{
    if (childPid <= 0) {
        /*
         * Defensive rather than expected: both callers check before calling, and a signal sent
         * to a non-positive pid is not a no-op - 0 addresses this process's whole process group
         * and -1 every process this user may signal. Refusing here means that a future caller
         * which forgot the check gets a reported failure instead of a runner that killed itself.
         */
        outcome = "was not reaped: " + std::to_string(static_cast<long>(childPid)) +
                  " is not a child pid, and signalling it would have addressed a process group "
                  "rather than one child";
        return false;
    }

    const long pidForTrace = static_cast<long>(childPid);

    /*
     * Whether this call may address a process group: decided here, once, from the caller's value
     * AND from this process's own group rather than from the caller's word alone.
     *
     * The refusal arm is the one that earns its place. A group id equal to this process's group
     * is what an UNCONFIRMED value looks like - a fork inherits its parent's group, so a caller
     * that took the child's pid and called it a group id without reading the group back would
     * pass exactly this - and a signal to it would end this runner and every process sharing its
     * group. Dropping to the single pid is strictly less than the caller asked for and is said
     * out loud, because it means a descendant could survive; killing the runner is not a degraded
     * outcome but a catastrophic one.
     */
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

        /*
         * The group first, then the pid below. The child leads the group, so the group signal
         * already reaches it; the second signal is deliberate belt and braces for the one case
         * the group signal would miss - a child that left its group after this harness confirmed
         * it, by calling setsid() or setpgid() again. A handler that runs twice is harmless, the
         * host's own shutdown being idempotent, and a child that has gone yields ESRCH, which is
         * tolerated in both places.
         */
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
        /*
         * Almost always ESRCH, meaning the child has already exited - which is exactly the
         * case on the host's readiness-failure path. It still has to be reaped, so this is
         * traced and not treated as an error.
         */
        std::cout << TRACE_PREFIX << "SIGTERM could not be delivered to pid " << pidForTrace << " ("
                  << std::strerror(errno) << "); it has most likely already exited, and it is "
                  "reaped below regardless" << std::endl;
    }

    /*
     * The bounded wait, in whichever of its two forms this child's descriptor selects. Both
     * spend the same gracePeriodMs and both are bounded; what the descriptor buys is precision,
     * not the bound. End of file on a pipe the child holds for its lifetime is the real event
     * and is noticed the instant it happens, so it stays the preferred form and remains the one
     * the broken-pipe seam drives; polling waitpid() with WNOHANG is what a child with no such
     * channel gets, and it reaps in the act of observing, which is why its outcome is tracked
     * separately below.
     */
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
            /*
             * waitpid() has disowned the pid - ECHILD in practice, meaning it is not a child of
             * this process or something else has already collected it. Neither SIGKILL nor a
             * blocking reap can improve on that, and signalling a pid this process does not own
             * is a hazard rather than a precaution, because the operating system is free to have
             * reused the number. So the escalation and the reap are both skipped and the outcome
             * the poll produced is what gets reported.
             */
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

        /*
         * The blocking reap, and it blocks only for as long as it takes the kernel to hand over a
         * status that already exists or is about to: it is reached either after the child was
         * observed to exit, or after SIGKILL, which cannot be caught, blocked or ignored, so the
         * child is dead or dying in both cases. It is skipped on exactly one path - where the
         * polling wait above already collected the child - because a second waitpid() on a reaped
         * pid fails with ECHILD, and reporting that as a failure would describe a child this
         * harness did collect as one it lost.
         */
        if (!reaped) {
            reaped = reapChildBlocking(childPid, outcome);
        }
    }

    /*
     * AND THE PART waitpid() COULD NEVER ESTABLISH: that nothing the child started is still
     * running. It is checked rather than argued for, and only where a group was supplied and the
     * wait above did not refuse the pid.
     *
     * The refused-wait exclusion follows the reasoning that skips SIGKILL on that same path: a
     * waitpid() that disowned the pid means this process does not own the child, so its group is
     * not this harness's to signal and the number may have been reused. Probing it would report
     * on processes this call has no relationship with, and the return value is false there
     * already.
     */
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
 * Idempotent and safe after a partial setup, which is not decoration: this runs from the
 * readiness-failure path in SetUp and again from TearDown, and TearDown executes even when
 * SetUp failed fatally. Clearing g_hostPid on the way out is what makes the second call a
 * no-op instead of a signal sent to a pid this process no longer owns - a pid the operating
 * system is free to have reused.
 *
 * What is host-specific stays here and the rest is terminateAndReapChildProcess()'s: the polite
 * `shutdown` request, the channel close and the readiness descriptor belong to this host and to
 * no other child, while signalling, the bounded wait, the escalation and the reap are the same
 * for every child this harness creates and are therefore implemented once, above. The host's
 * death channel is its readiness pipe, which it holds open for as long as it lives, so that
 * descriptor is what this call hands over as the exit observation.
 *
 * @return std::string                            - How the host ended, for a caller that is
 *                                                  building a diagnostic. Empty when no host
 *                                                  was ever launched
 *
 * @post g_hostPid is -1, g_hostProcessGroupId is -1 and g_hostReadinessReadFd is closed, so no
 *       descriptor and no child outlive this call - and, where the launch confirmed the host's
 *       process group, no process the host itself started outlives it either, which
 *       terminateAndReapChildProcess() establishes by probing the group rather than by waiting on
 *       processes it never forked.
 *
 * @see terminateAndReapChildProcess(), waitForChildExitViaPipeEof()
 */
std::string terminateAndReapFakeServiceHost()
{
    if (g_hostPid <= 0) {
        /*
         * Nothing was launched - the legacy invocation, or a launch that failed before it
         * forked - or this has already run. Either way there is nothing to signal; only a
         * descriptor might remain, and only if a launch failed after creating the pipe.
         */
        if (g_hostReadinessReadFd >= 0) {
            ::close(g_hostReadinessReadFd);
            g_hostReadinessReadFd = -1;
        }
        closeHostControlChannel();
        return std::string();
    }

    const long hostPid = static_cast<long>(g_hostPid);

    /*
     * The polite step, and it is only a step: the host's own `shutdown` command performs
     * exactly the teardown its signal path performs, so asking first lets the host exit
     * through its documented path with its own trace rather than being signalled out of a
     * poll(). It is attempted only where the host reached readiness and the channel is open,
     * because on the readiness-failure path the host is already dead or wedged and a request
     * there would spend its whole bound learning what the signal below establishes at once.
     *
     * Nothing rests on it. A failed or unanswered `shutdown` is traced and the signal path
     * runs regardless: SIGTERM, then a bounded wait, then SIGKILL. The guarantee that no host
     * outlives the run is the signal escalation and the reap, never the request - a request
     * can only be sent to a host that is still serving, which is the one case that never
     * needed a guarantee.
     */
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

    /*
     * Closed before the signal, so that a host still sitting in its command loop sees end of
     * file on its control descriptor - a clean end of session by its own contract - and so
     * that nothing later in this function can write to a descriptor whose peer is going away.
     */
    closeHostControlChannel();

    /*
     * The signal, the bounded wait on the readiness pipe's end of file, the escalation and the
     * reap are all terminateAndReapChildProcess()'s, so that the path a teardown takes is the
     * path the broken-pipe seam exercises. The readiness descriptor is handed over as the exit
     * observation because the host holds its write end for exactly as long as it lives.
     */
    std::string outcome;
    const bool reaped =
        terminateAndReapChildProcess(g_hostPid, "the fake HDMI CEC AIDL service host",
                                     g_hostReadinessReadFd, HOST_SHUTDOWN_TIMEOUT_MS,
                                     g_hostProcessGroupId, outcome);

    g_hostPid = -1;

    /*
     * Cleared with the pid and for the same reason: both are the identity of a child that no
     * longer exists, and a group id left behind would let a second call signal a group whose
     * number the operating system is free to have reused. It is -1 already when the launch could
     * not confirm the group, so this is not conditional on how the launch went.
     */
    g_hostProcessGroupId = -1;

    if (g_hostReadinessReadFd >= 0) {
        ::close(g_hostReadinessReadFd);
        g_hostReadinessReadFd = -1;
    }

    /*
     * Reported rather than swallowed, and non-fatally: a host this harness could not
     * collect is a leaked process and worth saying so, but it is not a reason to cut short
     * the rest of a teardown.
     */
    EXPECT_TRUE(reaped) << "the fake HDMI CEC AIDL service host pid " << hostPid << " " << outcome
                        << ", so it may survive this run as an orphan holding the production "
                           "service name";

    return outcome;
}

/* ---------------------------------------------------------------------------------------------
 * The broken-pipe probe: the harness's own control-channel write and its own reaping, driven.
 *
 * What it is for. Ignoring SIGPIPE buys this harness two things and nothing else: the EPIPE arm of
 * writeControlCommand() becomes reachable, and the teardown that signals and reaps the host still
 * gets to run. Both are claims about code that only executes when a pipe's reader has gone, so
 * neither is established by anything the ordinary invocations do - a healthy host reads its control
 * descriptor until teardown closes it, and the arm never runs. A test that constructed a look-alike
 * instead - its own pipe, its own raw ::write(), its own errno - would establish the kernel's
 * behaviour and the process's signal disposition, which are not in doubt, while leaving the function
 * whose diagnostic the install exists to make reachable completely unexercised, and leaving the reap
 * unexercised with it.
 *
 * So this drives the real code. writeControlCommand() takes its descriptor as a parameter, so it can
 * be called against a pipe the probe owns; terminateAndReapChildProcess() takes its pid as a
 * parameter, so it can end a child the probe owns. Neither is a copy and neither is reached through a
 * test-only branch: they are the same functions performHostControlRequest() and
 * terminateAndReapFakeServiceHost() call, and a change that broke either would break this probe.
 *
 * What it needs from the platform: nothing. No fake service host, no /dev/binder, no service
 * manager, no back-end, and no network. close() on the last read end of a pipe and then write() to
 * its write end returns -1 with EPIPE synchronously, and SIGTERM to a child at SIG_DFL ends it,
 * so the probe behaves identically under invocation D on a host with no binder support - which is
 * where it ordinarily runs - and under invocation E on the binder-capable guest.
 *
 * Why there is a child at all, and not simply a pipe with its read end closed. Two reasons, and the
 * first is the one that makes the probe deterministic rather than nearly deterministic:
 *
 *   The reader has to be provably gone. A pipe's write end reports EPIPE only when no read end
 *   remains open anywhere. A fork duplicates every descriptor, so the child holds a copy of the read
 *   end from the moment it exists; if the probe wrote before the child had closed that copy, the
 *   write would succeed into the pipe buffer and the probe would fail for a reason that has nothing
 *   to do with the property under test. That is a race, and it is removed rather than narrowed: the
 *   child closes both probe ends first and only then writes one byte to a handshake pipe, and the
 *   parent does not write until it has read that byte. The parent's own copy of the read end is
 *   closed before the write for the same reason, and it is closed by the parent because nothing else
 *   can - a descriptor is closed only by the process that holds it.
 *
 *   There has to be something to reap. The second half of what the disposition buys is the reap, and
 *   a reap needs a real child that a real waitpid() collects. The probe's child is that child, and
 *   terminateAndReapChildProcess() is what ends it, so the probe's evidence covers both halves.
 *
 * The handshake pipe does a second job for free: the child holds its write end for as long as it
 * lives, so end of file on the parent's read end means the child has exited. That is exactly the
 * shape of the host's readiness pipe, and it lets the probe hand a real death channel to
 * terminateAndReapChildProcess() so that its bounded wait and its SIGKILL escalation are the ones a
 * teardown uses rather than the polling fallback a channel-less child would take. Both of that
 * function's waits are bounded, so the fallback would be sound; it would simply not be the path
 * under test.
 *
 * What it never touches: g_hostControlWriteFd, g_hostObserveReadFd, g_hostPid,
 * g_hostReadinessReadFd, g_hostReportedReady and the SIGPIPE disposition itself. Under invocation E
 * a live host is on the other end of the first two and a live session depends on all of them, so the
 * probe owns four descriptors and one child of its own and reads the disposition without altering
 * it. It leaves none of them behind on any path, including every failure path, which is what
 * EpipeProbeResources is for.
 * --------------------------------------------------------------------------------------------- */

/**
 * @brief The command line the probe writes to its reader-less descriptor.
 *
 * Recognisable on purpose, and not a command in the host's vocabulary: this line is never written to
 * a live channel, and a reader of a diagnostic that quotes it should be able to tell at once that it
 * came from the probe rather than from an observation a case asked for. The probe requires this exact
 * text to appear in writeControlCommand()'s failure sentence, which is what proves the diagnostic
 * names the command that failed rather than being a generic message.
 */
const char *const EPIPE_PROBE_COMMAND = "epipe-probe";

/**
 * @brief One bound for every wait the probe makes, in milliseconds.
 *
 * Two seconds, and it is a backstop rather than an expected duration. Each of the three waits it
 * governs - the handshake byte from a child whose only job is to close two descriptors and write it,
 * the write that a reader-less pipe rejects synchronously, and the exit of a child that is at
 * SIG_DFL for SIGTERM - completes in microseconds when the mechanism is sound, and none of them can
 * legitimately be slow. It is short rather than generous for the same reason the host's bounds are
 * long: the host's waits cross a process start, a binder driver and a service manager inside an
 * emulated guest, while nothing here leaves the kernel's pipe and process machinery. Two seconds is
 * far above any plausible scheduling delay on a loaded machine and short enough that a harness fault
 * is reported promptly instead of eating a CI job's timeout.
 */
const int EPIPE_PROBE_TIMEOUT_MS = 2000;

/**
 * @brief Exit code the probe's child reports when it cannot make SIGTERM fatal to itself.
 *
 * Outside the range the host binary uses and distinct from the two pre-exec codes above, so that a
 * status observed by the probe's reap is unambiguous. The child claims this code only on a path that
 * also writes no handshake byte, so the parent's bounded wait reports it as "the child exited before
 * reporting that it had closed the probe pipe" rather than as a timeout.
 */
const int EPIPE_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED = 122;

/**
 * @brief Closes a descriptor if it is open and marks it closed.
 *
 * The idempotence is the point: every cleanup path in the probe below runs this over the same
 * variables, so a descriptor already released by an earlier step is not closed a second time. A
 * double close is not harmless in a process that opens descriptors afterwards - the number can by
 * then belong to something else entirely - which is why the variable is reset rather than merely
 * closed.
 *
 * @param [in,out] fd                     - Descriptor to close. Set to -1 on return, whether it was
 *                                          open or already closed
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
 * A scope guard rather than a trailing cleanup block, and the distinction is load-bearing: the probe
 * has ten steps and any of them can report failure and return, so cleanup written at the end would
 * run on the one path where it does not matter and be skipped on all the paths where it does. With
 * this guard every return - success, any failure, or an exception escaping a std::string operation -
 * passes through the same release.
 *
 * The child is a backstop and not the mechanism. On the successful path the probe ends its child
 * through terminateAndReapChildProcess() and clears the pid, so this destructor does nothing at all;
 * that is deliberate, because the reap is part of what the probe exists to establish and a reap that
 * only happened in a destructor would prove nothing about the code a teardown runs. The destructor's
 * SIGKILL exists for the paths that failed before that step, where a child is alive, unwanted, and
 * about to be forgotten.
 *
 * @warning Non-copyable deliberately: a copy would duplicate descriptor numbers and a pid, and the
 *          second destructor would close and signal what the first had already released.
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

    EpipeProbeResources() = default;
    EpipeProbeResources(const EpipeProbeResources &) = delete;
    EpipeProbeResources &operator=(const EpipeProbeResources &) = delete;

    /**
     * @brief Releases whatever the probe still holds, on every path out of it.
     *
     * Runs over the same five members every explicit cleanup step in the probe runs over,
     * and every release is idempotent: closeIfOpen() resets each descriptor as it closes
     * it, and the successful path clears the pid, so a descriptor already released is not
     * closed a second time and a child already reaped is not signalled again. That matters
     * because a descriptor number closed twice can by then belong to something else
     * entirely, and a pid signalled after it has been reaped can have been reused by the
     * operating system.@n
     * The four descriptors go first and the child last. Their order among themselves does
     * not matter, since each release is independent of the others, but ending the child is
     * the only step here that can block, so doing it last means no descriptor is held open
     * across that wait. A surviving child is killed rather than asked to stop, because this
     * path is reached only where the probe gave up partway through and the orderly ending
     * is what it gave up on.@n
     * On the successful path there is nothing left to do, and that is deliberate: the probe
     * ends its child through terminateAndReapChildProcess() and closes its remaining
     * descriptors explicitly, because the reap is part of what the probe establishes and a
     * reap that only ever happened in a destructor would say nothing about the path a
     * teardown takes.
     *
     * @return None
     *
     * @post No descriptor and no child the probe created outlives the guard, so nothing it
     *       opened leaks into the rest of the run and nothing it forked becomes a zombie
     *       that outlives the suite.
     *
     * @warning Does not raise, and nothing that could raise may be added to it. A
     *          destructor is implicitly noexcept, so an exception escaping this one while
     *          the stack unwound from a failed probe step would terminate the process
     *          instead of failing one case. The calls it makes are close(), kill(),
     *          waitpid() and one trace line, none of which raises short of allocation
     *          failure, and a child it could not collect is reported through that trace
     *          rather than by raising.
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

            /*
             * SIGKILL rather than SIGTERM here, because this path is reached only when the probe
             * gave up partway through and the orderly ending is what it gave up on. An unreaped
             * child would become a zombie that outlives the suite, and the guarantee this harness
             * makes about the host applies just as much to a child it created to test that
             * guarantee.
             */
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
 * The step that makes the probe deterministic. Until this byte arrives the child may still hold its
 * inherited copy of the probe pipe's read end, and a write to a pipe that still has a reader
 * succeeds. Waiting for the byte turns "the child has probably closed it by now" into an observed
 * fact, and it is a real wait on the real event rather than a sleep: poll() is given the time
 * remaining until one monotonic deadline and returns the moment the byte is there.
 *
 * End of file is a different outcome from the bound expiring and is reported as such. It means the
 * child exited before reporting - it could not make SIGTERM fatal to itself, or it was killed - and
 * that is a fault in the probe rather than in the property under test, so it must not be described
 * as a timeout.
 *
 * @param [in]  handshakeReadFd           - Read end of the handshake pipe, open, with the parent's
 *                                          copy of the write end already closed
 * @param [out] failureDetail             - Receives a diagnostic naming what went wrong. Untouched
 *                                          on success
 *
 * @return bool                                   - Whether the byte arrived within the bound
 * @retval true                                   - It did, so the child holds no probe descriptor
 * @retval false                                  - The bound expired, the child exited first, or
 *                                                  the descriptor failed
 *
 * @pre The parent has closed its own copy of the handshake write end, or end of file can never
 *      arrive and a dead child would present as a timeout.
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
 * The ten steps, and every one of them is necessary: step 0 refuses to proceed under a disposition
 * that would terminate this process, steps 1 to 3 build a pipe whose reader can be removed and a
 * child that can be reaped, steps 4 and 5 remove every reader of the probe pipe, step 6 establishes
 * that the removal is complete, step 7 makes the real call, step 8 requires the real diagnostic,
 * step 9 performs the real reaping, and step 10 releases what is left. Skipping any of 4, 5 or 6
 * turns the EPIPE the probe requires into a successful write.
 *
 * @param [out] observedDiagnostic        - Receives the failure sentence writeControlCommand()
 *                                          produced, so the caller can assert on its substance at
 *                                          its own line. Empty when the probe never reached that
 *                                          call, and empty when the call unexpectedly succeeded,
 *                                          because writeControlCommand() leaves its out-parameter
 *                                          untouched on success
 * @param [out] failureDetail             - Receives a sentence naming the step that failed and what
 *                                          its failure means. Untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - The real write reported EPIPE with a diagnostic
 *                                                  naming the command, and the real reaping
 *                                                  collected the child
 * @retval false                                  - One step did not hold; failureDetail names it
 *
 * @pre SIGPIPE is not at its default disposition, which step 0 verifies rather than assumes.
 *
 * @post No descriptor and no child created here outlives the call, on every path.
 *
 * @see writeControlCommand(), terminateAndReapChildProcess(), EpipeProbeResources
 */
bool proveEpipeDiagnosticAndChildReaping(std::string &observedDiagnostic,
                                         std::string &failureDetail)
{
    observedDiagnostic.clear();

    /*
     * Step 0. The disposition, read out of the process rather than assumed from the fact that
     * SetUp installed it. This is a refusal and not an assertion: under SIG_DFL the write in step 7
     * would not return at all, and a probe that killed the runner while checking that the runner
     * cannot be killed would be the worst possible outcome. The caller checks the same thing and
     * fails the case on it; this check exists so that the check cannot be skipped by a future
     * caller.
     */
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

    /*
     * Step 1. The handshake pipe, which carries the child's "I have closed both probe ends" byte and
     * then serves as its death channel. O_CLOEXEC on both ends at creation, atomically, exactly as
     * every other pipe in this file: nothing here execs, but a descriptor that would survive an exec
     * for no reason is the kind of difference that later becomes a bug.
     */
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

    /*
     * Step 2. The probe pipe: the descriptor pair the real write is aimed at. It is the probe's own
     * so that nothing here can disturb the live control channel, whose reader on invocation E is a
     * host serving a session the rest of the invocation depends on.
     */
    int probePipe[2] = { -1, -1 };
    if (::pipe2(probePipe, O_CLOEXEC) != 0) {
        failureDetail = std::string("step 2, create the probe pipe: ") + std::strerror(errno) +
                        ". This is the descriptor the real writeControlCommand() call is made "
                        "against, so without it there is nothing to drive";
        return false;
    }
    probe.probeReadFd  = probePipe[0];
    probe.probeWriteFd = probePipe[1];

    /*
     * Step 3. The child. Its entire job is to release both probe descriptors, say so, and then stay
     * alive until it is signalled, so that the reaping in step 9 has a real child to collect.
     */
    const pid_t child = ::fork();

    if (child < 0) {
        failureDetail = std::string("step 3, fork the probe child: ") + std::strerror(errno) +
                        ". Without a child there is no second holder of the probe pipe's read end "
                        "to release it and nothing for the real reaping to collect";
        return false;
    }

    if (child == 0) {
        /*
         * In the child. Every call from here on is one that may be made after a fork: close(),
         * sigaction(), a raw write() and _exit(). Nothing allocates and nothing takes a lock,
         * which matters because this process may have a binder threadpool running when the fork
         * happens - on invocation E it does - and a fork copies a lock's state as it stood, so a
         * child that took one could deadlock against a lock no thread of its own will ever
         * release. std::memset is pure computation over a local and takes nothing.
         *
         * Both probe ends go, not just the read end. The read end is the one that must go for the
         * parent's write to report EPIPE; the write end goes with it because a child that kept it
         * would hold open a descriptor it has no use for, and a descriptor nobody needs and nobody
         * watches is how a leak starts.
         */
        ::close(probe.probeReadFd);
        ::close(probe.probeWriteFd);
        ::close(probe.handshakeReadFd);

        /*
         * NO setpgid() HERE, deliberately, and step 9 passes -1 as the group id to match. The
         * host's child is made a group leader because it EXECS a binary this harness does not
         * control, which may fork; this child execs nothing and forks nothing - the whole of its
         * remaining life is the write below and pause() - so it can have no descendants, and a
         * group would be a mechanism whose guarantee this child does not need. Keeping it out of
         * one has a second value: the single-pid path through terminateAndReapChildProcess()
         * stays exercised on every run of this tier, so the branch a caller without a group takes
         * is covered rather than merely present.
         */

        /*
         * SIGTERM is made fatal to this child explicitly, so that step 9's terminate-and-reap does
         * not depend on what disposition the child happened to inherit. The parent ignores SIGPIPE
         * and nothing in this binary touches SIGTERM, so the inherited disposition is already the
         * default - but "already correct by accident" is not a property to build a bounded wait on,
         * and the probe deliberately hands terminateAndReapChildProcess() a child it can be sure
         * SIGTERM ends.
         */
        struct sigaction terminateDefault;
        std::memset(&terminateDefault, 0, sizeof(terminateDefault));
        terminateDefault.sa_handler = SIG_DFL;
        ::sigemptyset(&terminateDefault.sa_mask);
        terminateDefault.sa_flags = 0;

        if (::sigaction(SIGTERM, &terminateDefault, nullptr) != 0) {
            ::_exit(EPIPE_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED);
        }

        /*
         * And only now the byte, because its whole meaning is "the closes above have happened". A
         * byte written before them would license the parent's write while a reader still existed,
         * which is the race this handshake exists to remove. writeRawFully() is used because it is
         * this file's post-fork write and tolerates a short or interrupted one; a failure here is
         * silent by its contract and presents to the parent as end of file when this child is
         * killed.
         */
        static const char closedMarker[] = "C";
        writeRawFully(probe.handshakeWriteFd, closedMarker, sizeof(closedMarker) - 1);

        /*
         * Blocked until signalled, so the child is alive and reapable when step 9 runs. pause()
         * returns only when a signal arrives, and a signal that arrives is either SIGTERM or SIGKILL
         * from step 9 - both of which end this child - so the loop is what makes any other signal
         * harmless rather than an early exit that would leave step 9 with nothing to terminate.
         */
        for (;;) {
            ::pause();
        }
    }

    /* The parent from here on. */
    probe.childPid = child;

    /*
     * Step 4. The parent's copy of the handshake write end goes, and this is the close that makes
     * end of file on the read end mean "the child has exited". While the parent holds a write end
     * open, a dead child produces no end of file, and the death channel step 9 relies on would sit
     * out its whole bound diagnosing the wrong thing.
     */
    closeIfOpen(probe.handshakeWriteFd);

    /*
     * Step 5. The parent's read end of the probe pipe goes, and this is the step that creates the
     * hazard. A pipe reports EPIPE to a writer only when no read end remains open in any process:
     * the child released its inherited copy above, and this releases the parent's, so after this
     * line the pipe has a writer and no reader at all. Leaving it open - the single easiest mistake
     * to make here - would leave the parent itself as a reader, the write in step 7 would succeed
     * into the pipe buffer, and the probe would report a failure that says nothing about the
     * property under test.
     */
    closeIfOpen(probe.probeReadFd);

    /*
     * Step 6. The child's report, waited for rather than assumed. Without it the probe would be
     * racing its own child for the read end - see awaitProbeChildClosedProbePipe().
     */
    std::string handshakeFailure;
    if (!awaitProbeChildClosedProbePipe(probe.handshakeReadFd, handshakeFailure)) {
        failureDetail = "step 6, wait for the probe child to release the probe pipe: " +
                        handshakeFailure;
        return false;
    }

    /*
     * Step 7. The real call, against the real function, with a bounded deadline. It must fail, and
     * it must fail from the write rather than from the deadline: a reader-less pipe rejects the
     * write synchronously, so the bound is a backstop that a sound mechanism never approaches.
     */
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

    /*
     * Step 8. The diagnostic itself, which is the half of the property a bare errno cannot establish.
     * Two substrings are required and each rules out a different wrong answer: the command text
     * proves the sentence names which command failed, and "EPIPE" proves it took the broken-pipe arm
     * rather than the deadline arm - the deadline arm also names the command, so the command alone
     * would not distinguish them.
     */
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

    /*
     * Step 9. The real reaping, through the same function a teardown uses, with the handshake pipe
     * as the death channel so that its bounded wait and its escalation are exercised rather than
     * bypassed. This is the second half of what ignoring SIGPIPE buys: a runner killed by the signal
     * would never have reached it.
     */
    std::string outcome;
    const bool reaped =
        terminateAndReapChildProcess(probe.childPid, "the broken-pipe probe child",
                                     probe.handshakeReadFd, EPIPE_PROBE_TIMEOUT_MS,
                                     /* childProcessGroupId */ -1, outcome);

    if (!reaped) {
        /*
         * The pid is deliberately left set, so that the guard's destructor still tries to kill and
         * collect a child this step could not.
         */
        failureDetail = "step 9, terminate and reap the probe child: it " + outcome +
                        ". The reap is the second thing ignoring SIGPIPE exists to protect - a "
                        "runner killed at the write above would never reach a teardown - so a probe "
                        "that cannot demonstrate it has demonstrated only half the property";
        return false;
    }

    probe.childPid = -1;

    /*
     * Step 10. What is left goes now rather than at the guard's destructor, so that the ordinary
     * path releases its own resources explicitly and the guard is visibly a backstop. Both calls are
     * idempotent, so the destructor that follows finds nothing to do.
     */
    closeIfOpen(probe.probeWriteFd);
    closeIfOpen(probe.handshakeReadFd);

    std::cout << TRACE_PREFIX << "The broken-pipe probe drove writeControlCommand() against a "
                 "reader-less descriptor and it reported: " << observedDiagnostic
              << ". Its child " << outcome << ", collected by the same terminate-and-reap a "
                 "teardown uses" << std::endl;

    return true;
}

/* ---------------------------------------------------------------------------------------------
 * The process-group probe: teardown's reach past the child it forked, driven rather than argued.
 *
 * WHAT IT ESTABLISHES, and why neither half is observable from the ordinary invocations. The fake
 * service host does not fork, so on every ordinary run the group teardown addresses a group of one
 * and behaves indistinguishably from signalling a pid. The two properties that only matter when a
 * host DOES fork - and a host path that is a shell script forks by its nature - therefore have
 * nothing to exercise them:
 *
 *   Descendants do not outlive the run. A process the host started is not this runner's child, so
 *   waitpid() cannot collect it and cannot observe it going. Before the launch put its child in a
 *   process group of its own there was no instrument that reached such a process at all, and the
 *   only group-wide signal available addressed the runner itself. This probe creates exactly that
 *   shape - a child, and a grandchild the runner never forked - and requires the grandchild to be
 *   gone at the end.
 *
 *   The child inherits three descriptors and no others. A fork duplicates everything the parent
 *   held, and a descendant holding a copy of a pipe's write end keeps that pipe from ever
 *   reporting end of file, which is precisely how a death channel stops reporting death. The
 *   sweep that closes the rest is invisible from inside the child, so this probe observes it from
 *   outside: the parent hands the child a WITNESS pipe it never names to it, closes its own copy
 *   of the write end, and requires end of file - which can only arrive once the child's inherited
 *   copy has been closed by the sweep.
 *
 * WHY A GRANDCHILD THAT IGNORES SIGTERM. A grandchild at the default disposition would die on the
 * group's SIGTERM, and the run would then prove that the first signal reaches a group - worth
 * having, but it would leave the escalation untested and would pass just as well against a
 * teardown that only ever sent SIGTERM. Ignoring SIGTERM forces the path that matters: the direct
 * child exits, is reaped, the group is found to still have a member, and the whole group is sent
 * SIGKILL, which nothing can ignore.
 *
 * WHAT IT NEEDS FROM THE PLATFORM: nothing. No fake host, no /dev/binder, no service manager and
 * no back-end. fork(), setpgid(), a pipe and a signal are all of it, so it behaves identically on
 * a host with no binder support - where it ordinarily runs, under invocation D - and on the
 * binder-capable guest under E.
 *
 * WHAT IT LEAVES BEHIND: nothing, on any path. The resources guard below kills the whole group and
 * reaps the child from its destructor, so a probe that fails halfway leaves no more behind than one
 * that succeeds.
 * --------------------------------------------------------------------------------------------- */

/**
 * @brief Exit status of a probe child that could not create its own process group.
 *
 * Distinct from the other two so that a failure names the step it happened at. The parent
 * reports the status verbatim rather than translating it, which is the same treatment the
 * host's own codes get.
 */
const int GROUP_PROBE_CHILD_EXIT_SETPGID_FAILED = 123;

/** @brief Exit status of a probe child whose own fork of the grandchild failed. */
const int GROUP_PROBE_CHILD_EXIT_FORK_FAILED = 124;

/** @brief Exit status of a probe grandchild that could not make SIGTERM harmless to itself. */
const int GROUP_PROBE_CHILD_EXIT_SIGNAL_SETUP_FAILED = 125;

/**
 * @brief How long the probe waits for its child's "I am set up" byte.
 *
 * The same order of magnitude as the broken-pipe probe's handshake bound and for the same
 * reason: it covers a fork, a setpgid(), a descriptor sweep and a second fork on a loaded
 * machine, and expiring means the probe cannot proceed rather than that the property failed.
 */
const int GROUP_PROBE_HANDSHAKE_TIMEOUT_MS = 5000;

/**
 * @brief The window in which a pipe that should already be at end of file must report it.
 *
 * Used twice, both times for a closure that has ALREADY happened by the time it is checked: the
 * witness pipe, whose write end the child's sweep closed before it wrote its handshake byte, and
 * the liveness pipe, checked for the opposite outcome - a grandchild still alive means no end of
 * file within this window. It is a short bound deliberately, because a longer one would only make
 * a failing case slower, not more accurate.
 */
const int GROUP_PROBE_CLOSURE_WINDOW_MS = 250;

/** @brief Grace period the probe child gets after SIGTERM before the escalation. */
const int GROUP_PROBE_GRACE_MS = 2000;

/**
 * @brief Owns the process-group probe's descriptors, child and group, and releases what is left.
 *
 * A scope guard for the same reason EpipeProbeResources is one: the probe has several steps, any
 * of them can report a failure and return, and cleanup written at the end would run only on the
 * path where it does not matter. Every release is idempotent, so the successful path - which ends
 * the child through terminateAndReapChildProcess() and clears the pid - leaves this destructor
 * with nothing to do.
 *
 * @warning Non-copyable deliberately: a copy would duplicate descriptor numbers, a pid and a
 *          group id, and the second destructor would close and signal what the first released.
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

    ProcessGroupProbeResources() = default;
    ProcessGroupProbeResources(const ProcessGroupProbeResources &) = delete;
    ProcessGroupProbeResources &operator=(const ProcessGroupProbeResources &) = delete;

    /**
     * @brief Releases every descriptor, then ends the whole group and reaps the child.
     *
     * SIGKILL to the GROUP rather than to the pid, because this path is reached only where the
     * probe gave up partway through and the thing most likely to be left behind is the very
     * grandchild the probe created to be hard to kill. The group is checked for emptiness
     * afterwards and a leak is traced, because a destructor cannot fail a case and a silent
     * leak here would be a process outliving the suite with nothing said about it.
     *
     * @return None
     *
     * @post No descriptor, no child and no member of the child's group outlives the guard.
     *
     * @warning Does not raise, and nothing that could raise may be added to it: a destructor is
     *          implicitly noexcept, so an exception escaping while the stack unwound from a
     *          failed step would terminate the process instead of failing one case.
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
 * A real wait on the real event rather than a sleep: poll() is given what is left of one
 * monotonic deadline and returns the moment the byte is there. End of file is reported
 * separately from the bound expiring, because it means the writer exited before reporting and
 * that is a fault in the probe rather than in the property under test.
 *
 * @param [in]  readFd                    - Descriptor to read from
 * @param [in]  timeoutMs                 - How long to wait, in milliseconds
 * @param [out] failureDetail             - Receives a diagnostic naming what went wrong.
 *                                          Untouched on success
 *
 * @return bool                                   - Whether a byte arrived within the bound
 * @retval true                                   - It did
 * @retval false                                  - The bound expired, the writer exited first,
 *                                                  or the descriptor failed
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
 * Ten steps, and the two facts it ends on are the ones nothing else in this tier can produce: a
 * grandchild this runner never forked is gone, and a descriptor the child was never told about was
 * closed before it could carry into anything it started. Both are observed - end of file on a pipe
 * whose only remaining writer was the process in question, and kill() to the group failing with
 * ESRCH - rather than inferred from a return value.
 *
 * It drives the REAL functions: startFakeServiceHost()'s child does exactly what this child's
 * first two acts do, in the same order, and step 8 calls the same terminateAndReapChildProcess()
 * that teardown calls, with a group id confirmed the same way. A copy of either would prove
 * nothing about the path a teardown takes.
 *
 * @param [out] failureDetail             - Receives the step and the reason on failure.
 *                                          Untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - The child led its own group, its sweep closed
 *                                                  the witness descriptor, its grandchild
 *                                                  survived SIGTERM and did not survive the
 *                                                  teardown, and the group is empty
 * @retval false                                  - A step failed; failureDetail names which
 *
 * @post No descriptor, no child and no member of the child's group outlives this call, on either
 *       outcome - the resources guard sees to the failing paths.
 *
 * @warning The child's post-fork code is held to the same rule as every other fork in this file:
 *          only setpgid(), close(), close_range(), getrlimit(), fork(), sigaction(), a raw
 *          write(), pause() and _exit(). This process may have a binder threadpool running when
 *          the fork happens, and a fork copies a lock's state as it stood.
 *
 * @see terminateAndReapChildProcess(), closeInheritedDescriptorsExcept(),
 *      awaitProcessGroupEmpty(), startFakeServiceHost()
 */
bool proveHostProcessGroupTeardown(std::string &failureDetail)
{
    ProcessGroupProbeResources probe;

    /*
     * Step 1. Three pipes, all O_CLOEXEC, which matters for none of them here - nothing execs -
     * and is kept anyway so that the shape matches the launch this probe stands in for.
     */
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

    /*
     * Step 2. The fork, and the child's whole life is four calls and a wait.
     */
    const pid_t child = ::fork();

    if (child < 0) {
        failureDetail = std::string("step 2, the probe child could not be forked: ") +
                        std::strerror(errno);
        return false;
    }

    if (child == 0) {
        /*
         * The child. Exactly what startFakeServiceHost()'s child does, in the same order and for
         * the same reasons: its own process group first, then the descriptor sweep, then the work.
         */
        if (::setpgid(0, 0) != 0) {
            ::_exit(GROUP_PROBE_CHILD_EXIT_SETPGID_FAILED);
        }

        /*
         * The sweep, keeping the two descriptors this child was told about. It is what closes the
         * witness pipe's write end - a descriptor nobody named to this child - and the parent's
         * observation of that closure is the only evidence the sweep ran that exists outside this
         * process.
         */
        closeInheritedDescriptorsExcept(probe.handshakeWriteFd, probe.livenessWriteFd, -1);

        const pid_t grandchild = ::fork();

        if (grandchild < 0) {
            ::_exit(GROUP_PROBE_CHILD_EXIT_FORK_FAILED);
        }

        if (grandchild == 0) {
            /*
             * The grandchild: the process this harness never created and cannot wait on. It keeps
             * the liveness pipe's write end and nothing else, so end of file on the parent's read
             * end means this process and only this process has gone.
             */
            ::close(probe.handshakeWriteFd);

            /*
             * And it ignores SIGTERM, so that the group's first signal cannot end it and the
             * escalation to SIGKILL is what must.
             */
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

        /*
         * Back in the child. Its copy of the liveness write end goes, so that the grandchild is
         * the only writer and end of file on the parent's read end is a statement about the
         * grandchild alone. Only then the handshake byte, whose whole meaning is "everything above
         * has happened".
         */
        ::close(probe.livenessWriteFd);

        static const char readyMarker[] = "G";
        writeRawFully(probe.handshakeWriteFd, readyMarker, sizeof(readyMarker) - 1);

        for (;;) {
            ::pause();
        }
    }

    /* The parent from here on. */
    probe.childPid = child;

    /*
     * Step 3. The parent's copies of the three write ends go. Each one is what makes an end of
     * file on the matching read end mean something: while this process holds a write end open, no
     * closure by any other process can produce one.
     */
    closeIfOpen(probe.handshakeWriteFd);
    closeIfOpen(probe.livenessWriteFd);
    closeIfOpen(probe.witnessWriteFd);

    /*
     * Step 4. The child's report, waited for rather than assumed, so that every check below is
     * made against a child that has finished setting itself up.
     */
    std::string handshakeFailure;

    if (!awaitOneByte(probe.handshakeReadFd, GROUP_PROBE_HANDSHAKE_TIMEOUT_MS, handshakeFailure)) {
        failureDetail = "step 4, the probe child did not report that it was ready: " +
                        handshakeFailure;
        return false;
    }

    /*
     * Step 5. The group, read back exactly as the launch reads it back, because a group id that is
     * merely assumed is this runner's own and everything below would then be addressed at the
     * runner.
     */
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

    /*
     * Step 6. The descriptor sweep, observed from outside. The witness pipe's write end was held
     * by this process and by the child and by nobody else; this process closed its copy in step 3
     * and never told the child the number, so end of file here can only mean the child's sweep
     * closed it - and it must already have happened, because the sweep precedes the handshake byte
     * step 4 waited for.
     */
    if (!waitForChildExitViaPipeEof(probe.witnessReadFd, GROUP_PROBE_CLOSURE_WINDOW_MS)) {
        failureDetail = "step 6, the witness descriptor was still open in the probe child " +
                        std::to_string(GROUP_PROBE_CLOSURE_WINDOW_MS) +
                        " ms after it reported ready, so the pre-exec sweep did not close the "
                        "descriptors the child was never told about - a host would inherit them "
                        "and pass them to everything it starts";
        return false;
    }

    /*
     * Step 7. And the grandchild is alive, which is what makes step 9 a measurement rather than a
     * tautology: no end of file within the window means the only holder of the liveness pipe's
     * write end is still running.
     */
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

    /*
     * Step 8. The real teardown, with the group id it confirmed: the same call, the same
     * parameters and the same bounded escalation terminateAndReapFakeServiceHost() makes.
     */
    std::string outcome;
    const bool reaped =
        terminateAndReapChildProcess(probe.childPid, "the process-group probe child",
                                     probe.handshakeReadFd, GROUP_PROBE_GRACE_MS,
                                     probe.childGroupId, outcome);

    if (!reaped) {
        /*
         * The pid and the group are deliberately left set, so the guard's destructor still tries
         * to end what this step could not.
         */
        failureDetail = "step 8, the terminate-and-reap did not complete: " + outcome;
        return false;
    }

    probe.childPid = -1;

    /*
     * Step 9. The fact waitpid() could never have produced. The grandchild ignored SIGTERM, so it
     * can only have gone by way of the group's SIGKILL, and end of file on a pipe whose only
     * writer it was is the observation that says so.
     */
    if (!waitForChildExitViaPipeEof(probe.livenessReadFd, PROCESS_GROUP_DRAIN_TIMEOUT_MS)) {
        failureDetail = "step 9, the probe grandchild was still holding the liveness descriptor " +
                        std::to_string(PROCESS_GROUP_DRAIN_TIMEOUT_MS) +
                        " ms after the teardown reported success, so a process this run started "
                        "has outlived it";
        return false;
    }

    /*
     * Step 10. And the group itself is empty, which covers a descendant that held no descriptor of
     * this harness's at all and would therefore have been invisible to step 9.
     */
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
 * The whole of the remote mode's preparation, and it must complete before anything resolves
 * the back-end selection. Every failure below fails the run: a launch that did not happen,
 * or happened and never became ready, means the AIDL invocation cannot be performed at all,
 * and the alternative to failing is a suite that silently exercises the legacy back-end and
 * reports the result as AIDL evidence.
 *
 * @return None
 *
 * @post On return without a failure the fake service is published, its service-side
 *       threadpool is running, and the selection has not yet been resolved.
 *
 * @warning Raises a fatal GoogleTest failure on every unhappy path, so callers must invoke
 *          it through ASSERT_NO_FATAL_FAILURE to stop rather than continue.
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

        /*
         * The channel is established here, once, before any case depends on it. `ping` touches
         * nothing in the fake and answers from the host's command loop, so it establishes
         * exactly the property the cases need - that a command written here is read there and
         * its reply comes back - and nothing else.
         *
         * It is done in setup rather than left to the first case that asks for an observation
         * because the two failures read completely differently. A broken channel found here is
         * one diagnostic naming the handoff, before a single case has run; found later it is an
         * arbitrary case failing on a timeout, with every other case that uses the channel
         * failing after it and nothing saying which was the cause. The most likely cause by
         * far is the FD_CLOEXEC step in the child - and the host refuses to start at all in
         * that case, so this check would not even be reached - but the descriptors could also
         * be crossed, or the host could be an older build without the channel, and both of
         * those present here.
         */
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

    /*
     * Reaped before the failure is raised, for two reasons: a fatal GoogleTest failure
     * returns from this function immediately, so anything after it would not run, and the
     * reap is what supplies the host's exit status - the single most informative thing
     * about why no token arrived.
     */
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
 * @brief Reads CEC_TEST_AIDL_MODE and does what it asks, before the selection resolves.
 *
 * The one place this tier's two invocations diverge. The legacy mode returns having touched
 * neither libbinder nor a second process, which is what keeps a plain ./run_L2Tests runnable
 * on a host with no kernel binder support at all; the remote mode does the whole launch and
 * handshake here, ahead of init, because here is the only place it still can.
 *
 * @return None
 *
 * @warning An unrecognised value is a fatal failure and never a quiet fall back to the
 *          legacy mode. Unset is the single permissive case, because a bare ./run_L2Tests
 *          legitimately means "run the legacy arm"; a typo does not, and a typo that
 *          silently downgraded the run would report a green result for an invocation that
 *          never happened.
 * @warning Raises fatal GoogleTest failures, so it must be called through
 *          ASSERT_NO_FATAL_FAILURE.
 *
 * @see launchHostAndWaitUntilReady()
 */
void applyAidlModeBeforeInit()
{
    const char *const requested = ::getenv(AIDL_MODE_VARIABLE);

    // Unset and empty both mean absent, so the legacy arm is what a bare run does.
    const std::string mode =
        (requested != nullptr && requested[0] != '\0') ? requested : AIDL_MODE_ABSENT;

    if (mode == AIDL_MODE_ABSENT) {
        std::cout << TRACE_PREFIX << AIDL_MODE_VARIABLE << "=" << mode
                  << ": launching no fake service host, so the legacy back-end is expected and "
                     "this process touches libbinder not at all" << std::endl;
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

    /*
     * THE VALUE IS RENDERED, NOT STREAMED. Every arm above matched a known spelling, so this
     * is the one diagnostic in this binary that names a value nothing has validated - the
     * boundary the log-injection contract applies at. Streamed raw, a mode of
     * $'bogus\n::error::FORGED' ended this message and began a standalone GitHub workflow
     * command on the next line.
     */
    FAIL() << AIDL_MODE_VARIABLE << " is set to " << renderUntrustedValue(mode)
           << ", which is not a recognised mode. run_L2Tests implements " << AIDL_MODE_ABSENT
           << " and " << AIDL_MODE_REMOTE
           << "; " << AIDL_MODE_COMPATIBLE << " and " << AIDL_MODE_INCOMPATIBLE << " belong to "
              "run_L1Tests. Refusing to fall back to " << AIDL_MODE_ABSENT << ", because a typo "
              "must not quietly downgrade the run to the legacy back-end and report it as a pass";
}

} // namespace


/* =============================================================================================
 * The cross-translation-unit seam: the three functions the case file uses - two to drive and observe
 * the out-of-process fake service, and one to drive this harness's own control-channel write and its
 * own child reaping.
 *
 * Why they live here. Every descriptor and every child process in this binary is owned by this
 * harness - it creates the pipes, hands their far ends to a child, signals, reaps and closes - and
 * its lifecycle is the only place that knows whether a host exists at all. The case file needs to
 * use all of that and must own no part of it, so the ownership stays here and only these three entry
 * points cross. The third is the same arrangement applied to the harness's own machinery: the case
 * that has to prove a reader-less control write reports EPIPE, and that the child which made it
 * reader-less is still reaped, cannot own the pipes or the child either.
 *
 * Why there is no header, which is deliberate and not an omission. A header for three functions used
 * by one file in one directory would be a new file in the build, and the migration's own rule is
 * that nothing test-scope reaches a production or installed surface. These are declared in the case
 * file by an identical extern declaration instead, with the same contract text above it.
 *
 * How drift is prevented, since two declarations of the same thing is exactly the shape that
 * usually rots: the parameter types are std::string and the return type is bool, so the C++ name
 * these definitions export encodes the whole signature. A change on either side that the other does
 * not match is an undefined symbol at link time - `make -C tests/L2Tests` fails and names the
 * function - not a silent difference in behaviour. Semantics are kept in step by the contract being
 * written out in full in both places; if either is edited, both are.
 *
 * What a case may assume and what it may not. It may assume that a true return from the request
 * function means one command was written to the host and one reply line was read back, and that the
 * reply is classifiable - it begins "OK " or "ERR ". It may not assume the reply is a success: an
 * "ERR " line is a successful exchange and reports true, because whether the host's refusal is
 * expected is the case's judgement and not this seam's. And it may not assume a channel exists - on
 * the legacy invocation there is no host, so cecL2HostControlChannelIsOpen() reports false and every
 * request fails with a diagnostic saying so rather than blocking. The broken-pipe seam is the one
 * entry point that depends on none of that: it brings its own pipes and its own child, so it means
 * the same thing on both invocations.
 * ============================================================================================= */

/**
 * @brief Reports whether the fake service host's control and observation channel is usable.
 *
 * True only on an invocation that launched the host and completed its readiness handshake, which is
 * CEC_TEST_AIDL_MODE=remote alone. On the legacy invocation there is no second process, so there is
 * nothing to ask and this reports false.
 *
 * @return bool                                   - Whether a request would have somewhere to go
 * @retval true                                   - Both descriptors are open and the host answered
 *                                                  a ping during setup
 * @retval false                                  - No host was launched, the launch failed, or the
 *                                                  channel has been closed by teardown
 *
 * @warning A case whose assertions depend on the channel must fail rather than pass when this is
 *          false. Skipping on it would hide a broken handoff behind a green run, which is the exact
 *          failure this tier exists to rule out.
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
 * The commands and replies are the protocol that mocks/hdmicec/fake_hdmi_cec_aidl_service_host.cpp
 * states normatively in its own file block; that text is the authority and is not restated here.
 * The ones this tier uses are `ping`, `listener`, `sent-count`, `last-sent`, `open-count`,
 * `close-count`, `deliver <hex>` and `shutdown`.
 *
 * Bounded in every direction and on every path. The write waits for the pipe to accept bytes, the
 * read waits for one newline, both against one monotonic deadline of
 * HOST_CONTROL_REPLY_TIMEOUT_MS from entry, and an interrupted call resumes against that same
 * deadline rather than restarting it. There is no path on which this blocks indefinitely: a host
 * that has exited is reported from EPIPE or end of file at once, and a host that is alive and silent
 * is reported when the bound expires. That property is the reason this seam exists in this shape - a
 * harness that could hang would be killed from outside with no result recorded, which is strictly
 * worse than any failed assertion.
 *
 * @param [in]  command                   - Command text without a terminator, e.g. "sent-count".
 *                                          Must be non-blank and contain no newline or carriage
 *                                          return; both are rejected here rather than sent, because
 *                                          either would desynchronise the one-reply-per-command
 *                                          framing for every later request
 * @param [out] reply                     - Receives the reply line without its terminator. Untouched
 *                                          when this reports failure
 * @param [out] failureDetail             - Receives a sentence naming what went wrong and what it
 *                                          means. Untouched when this reports success
 *
 * @return bool                                   - Whether one command was exchanged for one reply
 * @retval true                                   - reply holds the host's answer, beginning "OK " or
 *                                                  "ERR "
 * @retval false                                  - No channel, a malformed command, the bound
 *                                                  expired, the host exited, or the reply was
 *                                                  unclassifiable; failureDetail says which
 *
 * @pre The caller is on the AIDL invocation, i.e. cecL2HostControlChannelIsOpen() reports true.
 *
 * @warning The observation travels over a pipe and not over binder, and that is the whole point.
 *          Binder is the transport under test; evidence carried over it would be attesting to the
 *          transport with the transport, and a fault could then corrupt the evidence invisibly. This
 *          channel is two inherited pipes and works identically whether the driver is healthy,
 *          degraded or absent.
 * @warning An "ERR " reply reports true. Check the reply text.
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
 * The third entry point this file publishes, and the only one that is not about the fake service
 * host. It exists because ignoring SIGPIPE buys exactly two things - writeControlCommand()'s EPIPE
 * arm becomes reachable, and the teardown that reaps the host still runs - and neither is exercised
 * by any ordinary invocation, since a healthy host reads its control descriptor until teardown closes
 * it. A case that built a look-alike instead, with its own pipe and its own raw write(), would
 * establish the kernel's behaviour and this process's signal disposition while leaving both of those
 * two things unexercised.
 *
 * So this drives the real code: the real writeControlCommand(), which takes its descriptor as a
 * parameter for exactly this reason, and the real terminateAndReapChildProcess(), which is the same
 * function the global environment's TearDown uses on the host. Neither is a copy and neither has a
 * test-only branch.
 *
 * What it does, in order. It creates a handshake pipe and a probe pipe; forks a child whose entire
 * job is to close both ends of the probe pipe, report that it has done so over the handshake pipe,
 * make SIGTERM fatal to itself and then block; closes the parent's copy of the handshake write end
 * and the parent's read end of the probe pipe, so that no reader of the probe pipe remains anywhere;
 * waits, bounded, for the child's report, because until it arrives the child may still hold the
 * inherited read end and a write would succeed; calls writeControlCommand() on the probe pipe's write
 * end and requires it to fail with a sentence that names the command and reports EPIPE; ends the
 * child through terminateAndReapChildProcess() and requires the reap to succeed; and releases every
 * descriptor it opened.
 *
 * Deterministic with no platform support of any kind. It needs no fake service host, no
 * /dev/binder, no service manager and no back-end: close() on a pipe's last read end followed by
 * write() to its write end returns -1 with EPIPE synchronously, and SIGTERM to a child at SIG_DFL
 * ends it. Nothing is slept on, no wall clock is polled, and each of its three waits is a real wait
 * on a real event under one bound. It therefore behaves identically under invocation D on a host with
 * no binder support and under invocation E on the binder-capable guest.
 *
 * @param [out] observedDiagnostic        - Receives the sentence writeControlCommand() produced, so
 *                                          the caller can assert on its substance at its own line
 *                                          rather than trusting this function's check. Empty if the
 *                                          probe never reached that call, and empty if the call
 *                                          unexpectedly succeeded
 * @param [out] failureDetail             - Receives a sentence naming the step that failed and what
 *                                          its failure means. Untouched on success
 *
 * @return bool                                   - Whether every step held
 * @retval true                                   - The real write reported EPIPE with a diagnostic
 *                                                  naming the command, and the real terminate-and-
 *                                                  reap collected the probe's child
 * @retval false                                  - One step did not hold; failureDetail names which
 *
 * @pre SIGPIPE is not at its default disposition. This is verified rather than assumed as the
 *      function's first act, and reported as a failure instead of being written into - a probe that
 *      terminated the runner while establishing that the runner cannot be terminated would be the
 *      worst available outcome.
 *
 * @post No descriptor and no child created by this call outlives it, on every path including every
 *       failure path. It touches none of the live channel's state - not g_hostControlWriteFd, not
 *       g_hostObserveReadFd, not g_hostPid, not g_hostReadinessReadFd - so it is safe to call while
 *       a real host session is open, which under invocation E it is.
 *
 * @warning It does not and cannot establish what happens without the disposition installed, because
 *          establishing that would mean terminating this process. The caller reads the disposition
 *          back and fails fatally on SIG_DFL before calling; that assertion and this function are
 *          two halves of one property.
 *
 * @see cecL2HostControlRequest()
 */
bool cecL2ProveEpipeDiagnosticAndChildReaping(std::string &observedDiagnostic,
                                              std::string &failureDetail)
{
    return proveEpipeDiagnosticAndChildReaping(observedDiagnostic, failureDetail);
}

/**
 * @brief Proves teardown ends every process the launched host started, and inherits no more than
 *        the three descriptors it is told about.
 *
 * WHY THIS CASE LIVES IN THE HARNESS'S OWN TRANSLATION UNIT rather than with the tier's flow
 * cases. Everything it drives is here - the launch's process-group creation, its pre-exec
 * descriptor sweep and terminate-and-reap's group-wide escalation - and all three are internal to
 * this file. A case in another unit could only reach them through a bridge written for it, and a
 * bridge is a second surface to keep in step for no gain when the case has no reason to be
 * elsewhere.
 *
 * WHY IT IS A CASE AND NOT A CHECK INSIDE SetUp. A check in SetUp runs before the suite, reports
 * against whatever assertion happens to be in scope, and cannot be named, filtered or seen in the
 * results. This is a property with a name, and it belongs in the results as one - not least because
 * it is the ONLY case in either tier that fails if the group teardown regresses.
 *
 * THE DualPath PREFIX IS DELIBERATE. This tier's runner is driven by one filter per invocation and
 * that filter is the DualPath glob, so a fixture named anything else would be registered, selected
 * by nothing, and excluded by nothing - which the runner's own selected-plus-excluded-equals-
 * registered reconciliation is built to catch and would rightly fail on.
 *
 * WHAT IT ASSERTS, and neither half is reachable from the ordinary invocations, where the fake host
 * neither forks nor is told about any descriptor it does not use:
 *
 *   A grandchild this runner never forked, which ignores SIGTERM, does not outlive the teardown.
 *   waitpid() cannot collect such a process and cannot observe it going, so the evidence is end of
 *   file on a pipe it was the last writer of, plus kill() to the group failing with ESRCH.
 *
 *   A descriptor the child was never told about is closed before it could reach anything the child
 *   starts. Observed from outside the child, as end of file on a pipe whose write end this process
 *   handed over silently and then released.
 *
 * It needs no fake host, no binder driver, no service manager and no back-end, so it runs and means
 * the same thing under every invocation of this tier.
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


// Global test environment to set up mocks
class CecL2TestEnvironment : public ::testing::Environment {
public:
    /**
     * @brief Brings the process to the state every L2 case assumes, in a load-bearing order.
     *
     * Makes a write to a reader-less pipe survivable, installs the legacy HAL double, then does
     * whatever the requested mode asks - which on the remote mode means launching the fake
     * service host and blocking until it reports ready - and only then initializes the CEC
     * library.
     *
     * @return None
     *
     * @post On return without a failure the CEC library is initialized, the back-end
     *       selection is resolved and fixed for the lifetime of the process, and the
     *       requested back-end is the one that was selected.
     *
     * @warning The order of the four steps below is the point of this tier. The init call
     *          is the first thing in this binary that forces Driver::getInstance(), which
     *          resolves the selection once and for all; a service that becomes reachable
     *          after it is a service the middleware never looks for. Moving the mode handling
     *          below init would not fail - it would pass, on the legacy back-end, and report
     *          the run as AIDL evidence.
     * @warning A fatal failure here skips every case in the binary and exits non-zero, and
     *          TearDown still runs, which is why teardown tolerates a partial setup.
     *
     * @see applyAidlModeBeforeInit()
     */
    void SetUp() override {
        /*
         * First, and before any descriptor exists to write to. A control channel whose write can
         * terminate this process is not a channel to proceed with: the failure it is supposed to
         * report would be recorded nowhere, and the teardown that reaps the host would not run.
         * This is the one step that has to precede everything rather than merely come early, and
         * it is a hard failure for the same reason every other setup fault here is - the whole of
         * this binary's evidence rests on what follows it. The reasoning, the scope decision and
         * the audit of every write in this file are written out above ignoreBrokenPipeSignal().
         */
        std::string sigpipeFailure;
        ASSERT_TRUE(ignoreBrokenPipeSignal(sigpipeFailure)) << sigpipeFailure;

        // Create and install the driver mock
        g_driverMock = new HdmiCecDriverMock();
        HdmiCecDriverMock::setInstance(g_driverMock);

        // Decide what the service lookup inside init() below is going to find - and, on the
        // remote mode, have a real service published and served by a real second process
        // before it looks. This runs first because init() is the one-way door.
        ASSERT_NO_FATAL_FAILURE(applyAidlModeBeforeInit());

        /*
         * The ordering property this tier rests on, made observable in the run's own log
         * rather than left to be inferred from the source: this line cannot be printed until
         * the readiness wait above has returned, and init() cannot run until it has been
         * printed. A log in which it precedes the host's readiness trace is a bug.
         */
        std::cout << TRACE_PREFIX << "Initializing the CEC library; this is the call that "
                     "resolves the back-end selection for the lifetime of the process"
                  << std::endl;

        /*
         * Asserted rather than wrapped, because nothing init() raises here can legitimately
         * be ignored. This environment's SetUp runs exactly once per process, so the one
         * condition that would make a second initialization harmless cannot arise, and what
         * init() does raise is real: Driver::getInstance().open() refused by the selected
         * HAL, or Bus::start() failing. Swallowing either reports a green suite for a
         * process that never initialized, with every case then asserting against an unopened
         * stack.
         */
        ASSERT_NO_THROW({ LibCCEC::getInstance().init("CEC_TEST"); })
            << "the CEC library could not be initialized, so not one case in this binary has "
               "its precondition; continuing would assert against an uninitialized stack";
    }

    /**
     * @brief Shuts the CEC stack down, releases the HAL double and reaps the host.
     *
     * @return None
     *
     * @post No child process and no descriptor opened by this harness outlives the run,
     *       including the two ends of the control and observation channel, and the SIGPIPE
     *       disposition that was in force before the run is back in force.
     *
     * @warning The order matters here too, in the opposite direction. term() reaches
     *          Driver::close(), which on the AIDL back-end is a transaction to the host, so
     *          the host must still be serving when it runs. Reaping first would turn an
     *          orderly shutdown into a failed close against a process that had already gone.
     *          The same applies to the channel: it is closed inside the host teardown below,
     *          after term(), so a case's last observation and the library's own close both
     *          happen while the host is alive.
     * @warning Runs even when SetUp failed, so every step tolerates never having happened:
     *          term() raises when the library was never initialized, and the host reap is a
     *          no-op when no host was launched or one was already reaped on the failure path.
     *          Failures are reported non-fatally, so that a first problem cannot hide the
     *          cleanup that follows it.
     *
     * @see terminateAndReapFakeServiceHost()
     */
    void TearDown() override {
        /*
         * Reported rather than swallowed, but non-fatally and deliberately so. TearDown runs
         * after the suite, so every result has already been recorded; a fatal assertion here
         * would both obscure results that were legitimately earned and cut short the cleanup
         * below. One failure here is honest rather than spurious: when SetUp failed, the
         * library was never initialized, so term() raises and is reported alongside the
         * original failure - which correctly says the process never came up.
         */
        EXPECT_NO_THROW({ LibCCEC::getInstance().term(); })
            << "the CEC library could not be terminated cleanly; the remaining cleanup below "
               "still runs, but this process did not shut the CEC stack down properly";

        HdmiCecDriverMock::setInstance(nullptr);
        delete g_driverMock;
        g_driverMock = nullptr;

        /*
         * Last, because term() above needed the host alive to answer its close. Idempotent,
         * so a host already reaped by the readiness-failure path costs nothing here.
         */
        terminateAndReapFakeServiceHost();

        /*
         * And only now the signal disposition, because the host teardown above closes the control
         * channel and makes its own last request over it - every one of which needs the protection
         * that this call removes. A disposition is process-wide and outlives this environment, so
         * putting the original back is what keeps the change scoped to the window that needed it
         * rather than leaving it for a static destructor or an atexit handler to inherit.
         */
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
 * @return int                                    - Zero when every case passed, non-zero
 *                                                  otherwise, including when the global
 *                                                  environment failed to come up
 *
 * @warning Configuration comes from the environment - CEC_TEST_AIDL_MODE and
 *          CEC_FAKE_AIDL_HOST_PATH - and never from the command line, so that every flag on
 *          this binary remains GoogleTest's own.
 */
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    ::testing::AddGlobalTestEnvironment(new CecL2TestEnvironment);
    return RUN_ALL_TESTS();
}


/** @} */
/** @} */
