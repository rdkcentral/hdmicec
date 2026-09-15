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
 * This translation unit is a whole program: a main() that publishes the test-scope fake
 * com.rdk.hal.hdmicec service in a process of its own, serves transactions against it, and lives
 * until its parent asks it to stop.@n
 * It exists because process boundaries are not an implementation detail of binder - they are the
 * thing under test.  libbinder resolves a service name that was registered in the calling process
 * to the local BBinder, so interface_cast there hands back that very object: no Bp* proxy is
 * created, no transaction crosses the binder driver, and the client's threadpool is never involved.
 * A fake registered inside the test runner therefore cannot prove the transport at all, however
 * faithfully it implements the interface.  Hosting the same fake here, in a separate process, is
 * what makes the middleware hold a real proxy and receive its event callbacks on a binder thread.
 *
 * There is deliberately nothing else in this file.  It declares no class, wraps nothing, and adds no
 * interface and no abstraction of its own: the only interface published from this process is the one
 * the `.aidl` files already define, and the fake that implements it already knows how to publish
 * itself under the production service name, so this program is a startup order, a readiness signal,
 * a line-oriented control and observation channel over two inherited descriptors, and a shutdown
 * wait.@n
 * The channel exists because a separate process is opaque from the outside.  Once the fake lives
 * over here, the runner over there can no longer call fireOnMessageReceived() to stimulate the
 * receive path, and can no longer read getLastSentMessage() to see what the middleware actually
 * transmitted - so without a channel the out-of-process invocation can only assert that a call did
 * not throw, which a no-op or a corrupt transmit passes just as well as a correct one.  The channel
 * is a plain pipe rather than a second binder interface precisely so that it stays trustworthy when
 * the binder path under test is broken, and so that no new AIDL surface is invented to test the
 * existing one.
 *
 */

/**
 * @file fake_hdmi_cec_aidl_service_host.cpp
 *
 * @brief Separate-process host for the test-scope fake com.rdk.hal.hdmicec AIDL HdmiCec service.
 *
 * Publishes the fake declared in fake_hdmi_cec_aidl_service.h under the production service name,
 * starts the service-side binder threadpool that serves incoming transactions, signals readiness to
 * the parent that launched it, then serves the parent's control and observation channel - the route
 * by which a runner triggers an inbound event and reads back what the middleware actually sent - and
 * stops on a `shutdown` command, on end of file, or on a termination signal.@n
 * This binary is the only place the AIDL receive path is genuinely exercised: it is the counterpart
 * of the out-of-process test invocation, in which the middleware resolves a real proxy over the
 * binder driver and its listener callback arrives on a binder threadpool thread rather than on the
 * caller's own stack.  Without this program that invocation cannot exist.
 *
 * @par The lifecycle contract, in full
 * This file is the normative statement of the host side of the contract.  The parent end lives in
 * the L2 runner's harness (tests/L2Tests/test_main.cpp) and is written against the text below.
 *
 * Startup, in this order and no other:
 * -# Install the shutdown handlers, so that a termination signal arriving at any later point still
 *    produces a clean exit rather than a default-disposition kill.
 * -# Resolve the readiness file descriptor.  What is checked here is the spelling of the value - a
 *    plain non-negative descriptor number and nothing more - and it is checked before anything is
 *    published, so a botched handoff costs an exit code and no publication.  Whether that descriptor
 *    is open and writable is settled by the readiness write in step 7, which fails with
 *    EXIT_READINESS_WRITE_FAILED.
 * -# Resolve the control and observation channel, if the parent supplied one.  Both descriptors are
 *    checked here for their spelling and for being open in this process in the direction they will be
 *    used, because a channel this program cannot serve has to be refused before anything is published
 *    rather than discovered as an EBADF three commands later; and - only when a channel was supplied -
 *    ignore SIGPIPE so that a client closing its end reports EPIPE instead of terminating this
 *    process.
 * -# Confirm a binder driver node is present and openable.
 * -# Confirm nothing is already published under the production service name.  Something there is a
 *    hard failure and never a condition to work around: this run's outcome would otherwise depend on
 *    a process this suite does not own.
 * -# Construct the fake, start the service-side threadpool, then publish the fake.  The pool is
 *    started before publication so that no transaction can ever arrive with no thread to serve it.
 * -# Write the readiness line - only now, after publication has succeeded and the pool is running,
 *    so that a parent which has seen the line may rely on the service being both published and
 *    served.
 * -# Serve the control and observation channel, if the parent supplied one, until a `shutdown`
 *    command, end of file on the control descriptor, a parent that has closed the observation
 *    descriptor, or a termination signal arrives - every one of those four is a clean end of session.
 *    Where no channel was supplied, block until signalled exactly as this program always has.
 * -# Return EXIT_SUCCESS.
 *
 * @par Environment
 * - `CEC_FAKE_AIDL_HOST_PATH` is where the parent finds this binary.  The build sets it and the L2
 *   harness reads it; this program never does, being already running by the time it matters.
 * - `CEC_FAKE_HOST_READY_FD` is the number of an inherited file descriptor - the write end of a pipe
 *   the parent holds open - on which the readiness line is delivered.  Unset means "no parent is
 *   listening", and the line goes to standard output instead so that a human can run this binary
 *   from a shell and watch it work.  Set but not a plain non-negative descriptor number is a hard
 *   failure, because a parent that asked to be signalled on a pipe and was silently answered on
 *   standard output would wait out its whole timeout with nothing to explain why.  That check is on
 *   the value only: whether the descriptor is open and writable is settled by the readiness write,
 *   which exits EXIT_READINESS_WRITE_FAILED when it is not.
 * - `CEC_FAKE_HOST_CONTROL_FD` is the number of an inherited file descriptor - the read end of a
 *   pipe the parent writes to - from which this program reads newline-terminated ASCII commands.
 * - `CEC_FAKE_HOST_OBSERVE_FD` is the number of an inherited file descriptor - the write end of a
 *   second pipe the parent reads from - to which this program writes exactly one newline-terminated
 *   reply per command.
 *
 * Both channel variables are optional and they travel together.  Both unset means no channel: this
 * program then behaves exactly as it did before the channel existed, which is why an existing
 * invocation that supplies neither is unaffected.  One set without the other, or either set to
 * something that is not a usable descriptor of the right direction, is a hard failure that exits
 * EXIT_BAD_CONTROL_CHANNEL and writes no readiness token - a parent that asked for a channel and
 * was silently served without one would sit in its own bounded wait for a reply that can never
 * come.  Unlike the readiness descriptor, these two are established as open and correctly directed
 * before they are accepted, because this program reads and writes them itself throughout the session.
 *
 * @par The control and observation protocol, in full
 * This block is the normative statement of the protocol.  The client end lives in the L2 runner and
 * is written against the text here, so nothing below may be changed without changing that client.@n
 * Framing: the parent writes one command per line, terminated by a single `\n`; this program writes
 * exactly one reply line per command, terminated by a single `\n`.  A trailing `\r` on a command
 * line is tolerated and stripped.  Tokens are separated by runs of spaces or tabs.  Every reply
 * begins with either `OK ` or `ERR `, so a client can classify an outcome before parsing it.  A
 * blank or whitespace-only line is not a command: it is ignored and produces no reply.  A command
 * line longer than MAX_COMMAND_LINE_LENGTH bytes is answered `ERR command-too-long` and discarded
 * unparsed, whether or not its terminator arrived in the same read - how a client's bytes happened to
 * be split must never decide whether its command was accepted.@n
 * Reply delivery: each reply is written within one whole-call deadline of OBSERVE_WRITE_TIMEOUT_MS,
 * measured on the monotonic clock, and is at most MAX_REPLY_LINE_LENGTH bytes - the size up to which a
 * pipe write is atomic, so a reply is never delivered in pieces.  A client that stops reading costs
 * this program one deadline and then the session: the reply is abandoned and the host exits
 * EXIT_CONTROL_CHANNEL_FAILED, which is the one outcome a client can act on where a hang is not.  A
 * reply that would exceed the cap - only the `ERR unknown-command <verb>` echo of a verb from a client
 * that has lost its framing can - is refused unwritten and ends the session the same way.
 *
 * Commands, with every reply each can produce:
 * - `ping` - liveness, touching nothing.  Replies `OK pong`.
 * - `deliver <lowercase-hex>` - invokes `onMessageReceived` on the listener the middleware handed to
 *   `open()`, with exactly the bytes the hex encodes; for example `deliver 0f8f` delivers two bytes.
 *   Replies `OK delivered <byteCount>`, or `ERR no-listener` when no listener is held, or
 *   `ERR bad-hex` when the payload is not an even-length run of hexadecimal digits.  `OK delivered`
 *   states that the callback was invoked on the held listener and nothing further: the callback is
 *   `oneway`, so whether the middleware then queued, decoded and dispatched the frame is what the
 *   test asserts on its own side of the boundary, which is the assertion that matters anyway.
 * - `sent-count` - replies `OK sent-count <n>`, the fake controller's real `sendMessage()`
 *   invocation count.
 * - `last-sent` - replies `OK last-sent <lowercase-hex>`, the bytes of the fake controller's last
 *   captured `sendMessage()` frame.  The hex field is empty when nothing has been captured, so that
 *   reply is `OK last-sent` followed by one space and then the newline, with nothing between them.
 * - `open-count` - replies `OK open-count <n>`, the fake service's real `open()` invocation count.
 * - `close-count` - replies `OK close-count <n>`, the fake service's real `close()` invocation count.
 *   Both are consumed by `DualPathAidlFlowTest` in
 *   `tests/L2Tests/ccec/test_DualPathIntegration.cpp`, which asserts the live-session invariant
 *   `open - close == 1` and then exact per-transition deltas around the one close/reopen cycle that
 *   tier performs.  That is evidence only an out-of-process fake can give: it establishes that the
 *   session lifecycle really crossed the driver, and how many times, which an in-process fake
 *   answers from the same address space and therefore cannot.
 * - `listener` - replies `OK listener present` or `OK listener absent`.
 * - `shutdown` - replies `OK shutdown` and then performs the same clean teardown the signal path
 *   performs, exiting EXIT_SUCCESS.  Anything the client had already queued behind it is not served,
 *   so `shutdown` is the last command of a session by definition.
 * - anything else - replies `ERR unknown-command <verb>` and the loop continues, so one mistyped
 *   command does not end the session.  Every verb outside the list above is answered that way, so a
 *   client whose vocabulary does not match this one is told so rather than silently served.
 *
 * That list is the whole vocabulary.  Three verbs are deliberately absent, recorded here so that
 * nobody restores one on the assumption it was overlooked:
 * - `state-changed` and `message-sent`, which would fire the fake's two diagnostic callbacks over
 *   IPC.  The adapter is required to log those callbacks and to act on them in no other way, and that
 *   requirement is already asserted in process by
 *   DriverAidlSessionTest.DiagnosticCallbacksAreReportedWithoutDisturbingTheSession, which calls
 *   FakeHdmiCecService::fireOnStateChanged() and fireOnMessageSent() directly and reads what they
 *   logged, so an out-of-process variant would add a verb without adding evidence.
 * - `reset`, which cannot be made safe: FakeHdmiCecService::reset() clears the captured listener, so
 *   a reset issued mid-session would silently destroy the live session's receive path and every
 *   `deliver` after it would answer `ERR no-listener` for a reason no assertion would explain.  The
 *   out-of-process invocation opens its session once, in the harness, and reads counters rather than
 *   rewinding them.
 *
 * Two further error replies apply to every command that takes arguments: `ERR bad-args <verb>` when
 * the argument count is wrong, and `ERR no-controller` if the fake service were ever to hand out no
 * controller - documented because a client must be able to classify every line this program can emit,
 * not because it is reachable today.
 *
 * @par The parent's obligations for the channel
 * The parent creates two pipes, passes the control read end and the observation write end to this
 * child as inherited descriptors, and names those descriptor numbers in `CEC_FAKE_HOST_CONTROL_FD`
 * and `CEC_FAKE_HOST_OBSERVE_FD`.  Descriptors created with `O_CLOEXEC` - which is how they should
 * be created, so that no unrelated exec leaks them - do not survive the exec of this binary unless
 * the parent clears `FD_CLOEXEC` on the child's copies between fork() and exec(), exactly as it
 * already does for the readiness descriptor.  A parent that forgets that step names descriptors this
 * program will reject as unusable, which is the failure it is meant to be: rejected loudly beats
 * served silently.@n
 * The parent must keep the other two ends open for as long as it intends to drive this host, must
 * read each reply before issuing the next command, and must bound every wait for a reply.  Closing
 * the control write end is a legitimate way to end the session: this program reads end of file and
 * shuts down cleanly, as it does for a signal.
 *
 * @par Readiness
 * The readiness signal is the exact line `FAKE_HDMI_CEC_AIDL_HOST_READY\n`, written once, with a raw
 * write that loops over partial writes and retries an interrupted one.  It is a fixed token and the
 * parent matches it verbatim.  Everything else this program prints is diagnostics on standard output
 * and no part of the signal.
 *
 * @par The parent's obligations
 * The parent creates the pipe, passes its write end to this child as an inherited descriptor, names
 * that descriptor in `CEC_FAKE_HOST_READY_FD`, and waits for the token under a bounded timeout after
 * which it fails the run rather than proceeding.  The bound is not optional: a run that proceeds
 * without the token tests the legacy back-end while reporting an AIDL result.  In its teardown the
 * parent terminates this child and reaps it, so that no host outlives the suite that launched it.
 *
 * @warning `CEC_TEST_AIDL_MODE` is not read here.  That variable is read by the L1 and L2 harnesses
 *          and nowhere else; this host is launched by the remote mode rather than being told about
 *          it, and a second reader of it would be a second place for the modes to drift.
 * @warning A sleep-based readiness signal is not acceptable anywhere in this contract.  A timed wait
 *          in place of the token converts a race into a flake: it passes on a fast machine, fails on
 *          a loaded one, and never once proves the service was actually published.
 * @warning This host does not reimplement the middleware's bounded binder preflight.  That predicate
 *          is production code in the middleware's own source directory and this binary links no
 *          libRCEC, so its protocol-version equality check and its bounded wait for binder handle 0
 *          are both absent here.  One
 *          consequence survives and is real: where a driver node exists but no service manager is
 *          running, reaching the service manager blocks - it retries in one-second intervals until
 *          binder handle 0 resolves - and this program will sit there.  The parent's bounded
 *          readiness timeout is the only guard against that case, which is part of why it is
 *          mandatory.  A service manager is an unconditional runtime prerequisite wherever a binder
 *          driver is present.
 * @note The threadpool started here is the service side, serving transactions addressed to the fake.
 *       It is unrelated to the client-side pool, which the middleware's AIDL back-end starts inside
 *       its own open() call.  The fake implementation itself starts neither, which is what keeps the
 *       in-process and out-of-process cases distinguishable.
 * @warning Test scope only.  This program is a noinst_PROGRAMS target built for test targets
 *          exclusively; it is never installed and no production source list references the directory
 *          it lives in, so nothing here can reach the shipped middleware library.
 *
 * @see fake_hdmi_cec_aidl_service.h
 * @see registerFakeHdmiCecService()
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
#include <poll.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

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
static const std::size_t RENDER_LIMIT = 200;

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
static inline std::string renderUntrustedValue(const std::string &value)
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
 * A failed out-of-process invocation is diagnosed from these lines and nothing else: the parent sees
 * a timeout, and only this output says which step the host reached before it stopped.  The prefix is
 * what separates them from the fake's own `[FakeRegistration]` and `[Fake...]` lines in an
 * interleaved capture.
 */
static const char TRACE_PREFIX[] = "[FakeHdmiCecAidlHost] ";

/**
 * @brief The one readiness token, spelled exactly once.
 *
 * The parent matches this line verbatim, so it is a fixed contract and not free to vary.  The
 * trailing newline is part of it: a parent reading a line needs the terminator to know the line is
 * whole.
 */
static const char READINESS_TOKEN[] = "FAKE_HDMI_CEC_AIDL_HOST_READY\n";

/**
 * @brief Environment variable naming the inherited descriptor the readiness line is written to.
 *
 * Unset means no parent is listening and the token goes to standard output.  Set but unusable is a
 * hard failure - see resolveReadinessFd().
 */
static const char READY_FD_VARIABLE[] = "CEC_FAKE_HOST_READY_FD";

/**
 * @brief Environment variable naming the inherited descriptor commands are read from.
 *
 * The read end of a pipe the parent writes to.  Unset - together with OBSERVE_FD_VARIABLE - means no
 * channel was supplied and this program serves none.  Set without its partner, or set to something
 * unusable, is a hard failure; see resolveControlChannelFds().
 */
static const char CONTROL_FD_VARIABLE[] = "CEC_FAKE_HOST_CONTROL_FD";

/**
 * @brief Environment variable naming the inherited descriptor replies are written to.
 *
 * The write end of a second pipe the parent reads from.  Exactly one reply line is written to it per
 * command received, and nothing else is ever written to it - every diagnostic in this program goes to
 * standard output, so a client parsing this descriptor sees replies alone.
 */
static const char OBSERVE_FD_VARIABLE[] = "CEC_FAKE_HOST_OBSERVE_FD";

/**
 * @brief Longest command line this program will assemble, in bytes, excluding the terminator.
 *
 * A control descriptor is a pipe an authorised parent owns, not untrusted input, but an unbounded
 * line buffer is still an unbounded allocation driven from outside this process.  The cap is far
 * larger than any command in the vocabulary - the longest realistic one is a `deliver` carrying a
 * maximum-length CEC frame, well under a hundred characters - so it can only ever be reached by a
 * client that has lost its framing, which is exactly the case it exists to answer.
 *
 * @see serveControlChannel()
 */
static const size_t MAX_COMMAND_LINE_LENGTH = 4096;

/**
 * @brief Bytes read from the control descriptor per read() call.
 *
 * Sized to swallow a whole command, and usually several, in one call.  A short read is ordinary and
 * costs nothing: the reader buffers a partial line and resumes on the next poll.
 */
static const size_t CONTROL_READ_CHUNK = 512;

/*
 * TRACED_COMMAND_LIMIT used to live here and bounded the traced command text at 120 bytes with a
 * plain substr().  It is gone because renderUntrustedValue() now bounds that trace, at RENDER_LIMIT,
 * and it escapes as well as bounds - which the substr() did not, so a command carrying a carriage
 * return reached the log intact.  The bound the trace still has, and why the command itself is
 * dispatched whole regardless of it, is documented on renderUntrustedValue() and serveControlChannel().
 */

/**
 * @brief Milliseconds one call to writeReplyLine() may spend delivering a reply, in total.
 *
 * A pipe whose reader has stopped reading eventually stops accepting writes, and a blocking write
 * there would hang this program with no diagnostic and no exit code - the one failure mode a test
 * harness can least afford, because it presents as a hung suite rather than a failed one.  So every
 * reply write is bounded: this program would rather exit EXIT_CONTROL_CHANNEL_FAILED and let the
 * parent's own bounded wait fail loudly than block.  The bound is generous by design, since a
 * healthy parent reads each reply before sending the next command and never comes close to it.@n
 * This is a whole-call budget and not a per-attempt one.  writeReplyLine() converts it once into an
 * absolute CLOCK_MONOTONIC deadline and derives every wait from the time left to that deadline, so a
 * reply interrupted repeatedly, or accepted in stages, cannot cost more than this value in total.
 *
 * @see writeReplyLine()
 */
static const int OBSERVE_WRITE_TIMEOUT_MS = 5000;

/**
 * @brief Longest reply line this program will write, in bytes, including its terminator.
 *
 * PIPE_BUF is the size up to which a pipe write is atomic, and that is the whole reason for the cap
 * rather than a taste for round numbers.  On a descriptor this program has put into nonblocking mode,
 * a write of at most PIPE_BUF bytes either transfers the line whole or transfers nothing and reports
 * EAGAIN; a larger write may transfer part of it and leave the rest for a later call.  A client
 * reading lines can survive "no reply" and cannot survive "half a reply", so a line that cannot be
 * one atomic write is refused before it is attempted instead of being fragmented.@n
 * Every reply a well-framed command produces is far below this: the longest is a `last-sent` carrying
 * a maximum-length CEC frame, a few hundred bytes of hexadecimal.  The one reply that can exceed it is
 * the `ERR unknown-command <verb>` echo of a verb from a client that has lost its framing, since a
 * verb may be as long as MAX_COMMAND_LINE_LENGTH; that reply is refused and the session ends with
 * EXIT_CONTROL_CHANNEL_FAILED, which is the loud outcome such a client has already earned.
 *
 * @see writeReplyLine(), MAX_COMMAND_LINE_LENGTH
 */
static const size_t MAX_REPLY_LINE_LENGTH = PIPE_BUF;

/**
 * @brief Binder driver node checked before anything in this process touches libbinder.
 *
 * The linked libbinder aborts the whole process when it cannot open its driver, so on a host without
 * kernel binder support an unguarded service-manager call would kill this program outright: no
 * diagnostic, no exit code, nothing for the parent to report but a timeout.  Checking the node first
 * turns that into a traced failure with an exit code of its own.@n
 * This single check is not the middleware's bounded preflight, which additionally requires the
 * driver's protocol version to equal the one libbinder was built for and waits for binder handle 0
 * under a bound.  Neither of those is performed here, and the @warning on this file records what
 * that leaves exposed.  The node name is spelled here because it cannot be shared: the fake's own
 * copy is file-local to its implementation and the middleware's is production code this binary
 * deliberately does not link.
 */
static const char BINDER_DRIVER_PATH[] = "/dev/binder";

/**
 * @brief Exit code reported when something is already published under the production service name.
 */
static const int EXIT_STALE_REGISTRATION = 2;

/**
 * @brief Exit code reported when CEC_FAKE_HOST_READY_FD is not a plain descriptor number.
 *
 * The spelling of the value is what this code answers for.  A value that parses and then turns out
 * not to be writable is a different failure, found at the readiness write and reported as
 * EXIT_READINESS_WRITE_FAILED.
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
 * This is also where a readiness descriptor that parsed but is not open for writing in this process
 * arrives, since resolveReadinessFd() checks the spelling of the value and the write establishes the
 * rest.  The fake is published by the time this is reached, and the process exits rather than serve
 * an invocation whose parent can never learn it is ready.
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
 * Covers the failures that are neither configuration nor binder state: the self-pipe, the signal
 * handlers, constructing the fake, and a service manager that cannot be reached at all.  Every one
 * of them means this host never became able to serve, so none of them may exit zero.
 */
static const int EXIT_SETUP_FAILED = 7;

/**
 * @brief Exit code reported when the control and observation channel is configured wrongly.
 *
 * Covers every way the channel can be asked for and not be usable: one of the two variables set
 * without the other, a value that is not a plain non-negative descriptor number, a descriptor number
 * that is not open in this process, and a descriptor open in the wrong direction - a control
 * descriptor that cannot be read or an observation descriptor that cannot be written.  Each of them
 * means the parent intends to drive this host and cannot, so none may exit zero and none may write
 * the readiness token.
 *
 * @see resolveControlChannelFds()
 */
static const int EXIT_BAD_CONTROL_CHANNEL = 8;

/**
 * @brief Exit code reported when the channel failed while it was being served.
 *
 * Distinct from EXIT_BAD_CONTROL_CHANNEL, which is a configuration fault found before the host ever
 * became ready.  This code means the channel was accepted and then stopped working: poll() or read()
 * failed for a reason other than an interruption, a reply was not delivered within the whole-call
 * deadline of OBSERVE_WRITE_TIMEOUT_MS, or a reply was longer than MAX_REPLY_LINE_LENGTH and was
 * refused rather than fragmented.  A parent that has closed its ends deliberately is not this case -
 * that is end of file, or a broken pipe, and both are clean shutdowns.
 *
 * @see serveControlChannel(), writeReplyLine()
 */
static const int EXIT_CONTROL_CHANNEL_FAILED = 9;

/*
 * State the signal handler touches, and therefore the only state in this file that is not local to a
 * function.  Both objects are volatile sig_atomic_t because a handler may run between any two
 * instructions of main and nothing weaker is guaranteed to be read or written indivisibly.
 *
 * EXIT_FAILURE - plain 1 - is deliberately never returned by this program.  Every failure class
 * above has a number of its own, so a parent, a CI log or a person reading an exit status can tell
 * "a stale service was already published" from "the readiness pipe was wrong" without reading the
 * output.
 */

/** @brief Signal number that asked this program to stop, or 0 while none has. */
static volatile std::sig_atomic_t g_shutdownSignalNumber = 0;

/** @brief Write end of the self-pipe, or -1 before it exists.  Written by main, read by the handler. */
static volatile std::sig_atomic_t g_shutdownPipeWriteFd = -1;

/** @brief Read end of the self-pipe, or -1 before it exists.  Never touched by the handler. */
static int g_shutdownPipeReadFd = -1;

extern "C" {

/**
 * @brief Records a termination request and wakes the waiting main thread.
 *
 * The whole of the handler: record the signal number, then write one byte to the self-pipe so that
 * whichever waiter is running completes - the blocking read in waitForShutdownSignal() when no
 * control channel was supplied, or the poll() in serveControlChannel() when one was, which is why
 * that loop watches this pipe alongside the channel.  Nothing else happens here, and nothing
 * else may - this runs asynchronously between arbitrary instructions, so a stream insertion, an
 * allocation or a binder call would risk deadlocking against a lock the interrupted code already
 * holds.  Both objects it touches are volatile sig_atomic_t and write() is async-signal-safe.@n
 * errno is saved and restored around the write, because the interrupted code may be in the middle of
 * examining its own errno and this handler must not be able to change what it reads.
 *
 * @param [in] signalNumber               - Signal being delivered, recorded for the exit trace
 *
 * @post The running waiter returns, and g_shutdownSignalNumber names the signal.
 *
 * @warning Async-signal-safe by construction.  Adding any other call - tracing included - would
 *          break that property, which is why the exit trace is emitted by main after the wait
 *          returns rather than from here.
 *
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
 * Establishes the shutdown path before anything else happens, so that a signal arriving at any later
 * point - during registration, or while the readiness line is being written - still produces the
 * clean exit the parent expects instead of the default disposition's abrupt kill.@n
 * SIGTERM is what the parent sends when it tears the host down.  SIGINT is handled beside it so that
 * a person running this binary by hand can stop it with the same clean path the suite uses, rather
 * than exercising an exit route the suite never takes.  Neither handler sets SA_RESTART: an
 * interrupted wait is retried explicitly, which keeps the interruption visible in one place instead
 * of relying on the kernel to hide it.
 *
 * @return bool                                   - Whether the shutdown path was established
 * @retval true                                   - Self-pipe created and both handlers installed
 * @retval false                                  - The pipe or a handler could not be installed, in
 *                                                  which case this program has no clean way to stop
 *                                                  and must not continue
 *
 * @post On success g_shutdownPipeReadFd and g_shutdownPipeWriteFd are open, and a termination signal
 *       ends whichever wait is running - waitForShutdownSignal() or serveControlChannel().
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
 * The one strict parser every descriptor-valued variable in this program goes through, so that the
 * readiness descriptor and the two channel descriptors cannot drift into accepting different
 * spellings of the same mistake.  Only an unadorned run of decimal digits naming a value in range is
 * accepted: an empty value, leading whitespace, a sign, a trailing character, a negative number and
 * a value too large to be a descriptor are all rejected rather than coerced.@n
 * strtol() on its own would not do this.  It skips leading whitespace and accepts a sign, so " 7"
 * and "+7" would parse as 7 and "-1" would parse as a negative descriptor the caller then has to
 * catch.  Requiring the first character to be a digit rejects all three before the conversion
 * matters.  A descriptor number a parent formatted correctly never needs that latitude, and a value
 * that does need it is a botched handoff.
 *
 * @param [in]  rawValue                  - Environment value to parse.  Must not be null
 * @param [out] descriptor                - Receives the parsed descriptor number.  Untouched when
 *                                          this reports failure
 *
 * @return bool                                   - Whether a descriptor number was parsed
 * @retval true                                   - descriptor holds a value in [0, INT_MAX]
 * @retval false                                  - The value was not a plain non-negative decimal
 *                                                  descriptor number
 *
 * @note Parsing only.  Whether the number names a descriptor that is actually open, and open in the
 *       direction the caller needs, is isUsableDescriptor()'s question.
 *
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
 * A parent that names a descriptor it never passed, or passes one it created with `O_CLOEXEC` and
 * forgets to clear that flag on the child's copy before exec, leaves this program holding a number
 * that is not open here at all.  Answering commands on it is impossible, and discovering that only
 * when the first read fails would waste the parent's whole timeout, so it is established up front
 * with a call that has no side effect.@n
 * The direction is checked as well as the openness.  A pipe end has one direction, and a control
 * descriptor that turns out to be a write end - or an observation descriptor that turns out to be a
 * read end - means the parent crossed the two over, which is a mistake worth naming rather than
 * discovering as an EBADF three commands later.
 *
 * @param [in] descriptor                 - Descriptor number to test
 * @param [in] mustBeWritable             - true to require a descriptor this process can write,
 *                                          false to require one it can read
 *
 * @return bool                                   - Whether the descriptor is open and usable
 * @retval true                                   - The descriptor is open and its access mode allows
 *                                                  the required direction
 * @retval false                                  - The descriptor is not open in this process, or it
 *                                                  is open in the wrong direction
 *
 * @note F_GETFL is used deliberately in place of a trial read or write: it reports the access mode
 *       without consuming a byte, without blocking, and without side effect of any kind.
 *
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
 * Reads CEC_FAKE_HOST_READY_FD and resolves it to a descriptor number, or falls back to standard
 * output when no parent named one.  The variable is parsed strictly and in full: only an unadorned
 * run of decimal digits is accepted, so an empty value, leading whitespace, a sign, a trailing
 * character, a negative number and a value too large to be a descriptor are all rejected rather than
 * coerced.@n
 * The asymmetry between "unset" and "set to nonsense" is deliberate.  Unset means nobody is
 * listening on a pipe, which is exactly the case when a person runs this binary from a shell, so the
 * token goes to standard output where they can see it.  Set to nonsense means a parent intended to
 * be signalled on a pipe and the handoff was botched; answering on standard output would leave that
 * parent waiting out its entire timeout with nothing to explain why, so it fails immediately and
 * says what the value was.
 *
 * @param [out] readinessFd               - Receives the descriptor the readiness line is to be
 *                                          written to.  Untouched when this reports failure
 *
 * @return bool                                   - Whether a descriptor was resolved
 * @retval true                                   - readinessFd holds either the descriptor number the
 *                                                  parent named, unexamined beyond its spelling, or
 *                                                  STDOUT_FILENO
 * @retval false                                  - The variable was present but not a plain
 *                                                  non-negative descriptor number
 *
 * @warning What is established here is the spelling of the value and nothing else.  The variable is
 *          put through parseDescriptorNumber(), which reports whether it is a plain non-negative
 *          decimal number in descriptor range; it does not report whether that descriptor is open in
 *          this process, nor whether it is open for writing.  The two channel descriptors are held to
 *          the stronger standard - resolveControlChannelFds() puts each through isUsableDescriptor()
 *          as well - because this program reads and writes them itself for the whole session, whereas
 *          the readiness descriptor is written exactly once, and that write is where its openness is
 *          established: it traces the errno and exits EXIT_READINESS_WRITE_FAILED, so a parent still
 *          learns from an exit code rather than from its own expired timeout.
 * @warning The value is read at this point, before the fake is published and long before the
 *          readiness line is written, so a misspelled value costs an exit code and no publication.
 *          Diagnostics on standard output and the shutdown self-pipe precede it; nothing a parent
 *          waits on does.
 *
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

    /*
     * The strict parser is shared with the two channel variables, so every descriptor-valued
     * variable in this program rejects the same spellings of the same mistake. What it will not
     * accept, and why, is documented on parseDescriptorNumber().
     */
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
 * Reads CEC_FAKE_HOST_CONTROL_FD and CEC_FAKE_HOST_OBSERVE_FD and resolves the pair to two usable
 * descriptors, or to "no channel at all".  The two variables travel together and there is no third
 * outcome: a channel with only one half is not a degraded channel, it is a channel whose replies
 * would go nowhere or whose commands would never arrive.@n
 * Every rejected case is rejected loudly, before this program takes any action a parent could
 * observe and long before the readiness token is written, so a botched handoff costs the parent
 * nothing but an exit code it can read.  Serving without the channel a parent asked for would be the
 * worst available outcome: the run would appear to start, the client's first bounded wait for a reply
 * would expire, and nothing would say why.
 *
 * @param [out] controlFd                 - Receives the descriptor commands are read from, or -1 when
 *                                          no channel was supplied.  Untouched on failure
 * @param [out] observeFd                 - Receives the descriptor replies are written to, or -1 when
 *                                          no channel was supplied.  Untouched on failure
 * @param [out] channelEnabled            - Receives whether a channel was supplied at all.  Untouched
 *                                          on failure
 *
 * @return bool                                   - Whether the channel configuration is acceptable
 * @retval true                                   - Either both descriptors are usable and
 *                                                  channelEnabled is true, or neither variable was set
 *                                                  and channelEnabled is false
 * @retval false                                  - The channel was asked for and is not usable: one
 *                                                  variable without the other, a value that is not a
 *                                                  plain descriptor number, a descriptor not open in
 *                                                  this process, or one open in the wrong direction
 *
 * @post On success with channelEnabled false, this program behaves exactly as it did before the
 *       channel existed - which is what keeps an invocation that supplies neither variable unaffected.
 *
 * @warning A descriptor created with O_CLOEXEC does not survive this program's exec unless the parent
 *          cleared FD_CLOEXEC on the child's copy first.  That omission arrives here as "not open in
 *          this process" and is reported as such, naming the descriptor number, because it is the
 *          single most likely handoff mistake.
 *
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
 * Writing to a pipe whose reader has closed raises SIGPIPE, whose default disposition terminates the
 * process outright - no diagnostic, no exit code, nothing for the parent to report.  A parent that
 * closes its channel ends the session legitimately, so this program must see that as an EPIPE it can
 * classify and act on rather than as a fatal signal.@n
 * The disposition is changed only when a channel was actually supplied.  A run with no channel keeps
 * the process defaults it has always had, which is what "both variables unset behaves exactly as
 * before" means in practice.
 *
 * @return bool                                   - Whether the disposition was installed
 * @retval true                                   - SIGPIPE is ignored, and a write to a closed pipe
 *                                                  now fails with EPIPE
 * @retval false                                  - The disposition could not be installed, so a
 *                                                  closed pipe could still take this process down and
 *                                                  it must not proceed
 *
 * @pre Called only when resolveControlChannelFds() reported a channel.
 *
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
 * A single write() call is allowed to transfer fewer bytes than it was given and is allowed to fail
 * outright when a signal arrives mid-call, and the readiness token is the one thing in this program
 * that must arrive whole - a parent matching a line cannot match half of one.  So this loops until
 * every byte is gone, retrying an interrupted call and advancing over a partial one.@n
 * The write is raw and unbuffered on purpose.  A token handed to a C++ stream could sit in that
 * stream's buffer while the parent waits for it, which presents as a hang and not as a bug.
 *
 * @param [in] fd                         - Descriptor to write to
 * @param [in] data                       - Buffer to write.  Must not be null
 * @param [in] length                     - Number of bytes to write
 *
 * @return bool                                   - Whether the whole buffer was written
 * @retval true                                   - All length bytes were written
 * @retval false                                  - The descriptor rejected the write, and errno
 *                                                  describes why
 *
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
            /*
             * A zero-length transfer on a positive request makes no progress, so retrying it would
             * spin forever.  Report it rather than loop.
             */
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
 * Consulted before this process touches libbinder for the first time, because the linked libbinder
 * treats a driver it cannot open as fatal and aborts: on a host with no kernel binder support an
 * unguarded service-manager call would take this program down with no diagnostic and no exit code of
 * its own.  Checking the node first turns that into a traced failure the parent can report, and it
 * keeps this host's failure disposition the same as the fake's own registration routine, which
 * refuses for the same reason rather than aborting.
 *
 * @return bool                                   - Whether the driver node is present and openable
 * @retval true                                   - The node exists and is readable and writable
 * @retval false                                  - The node is absent or cannot be opened, so no
 *                                                  binder transport exists on this host
 *
 * @warning This is a node check and nothing more.  It does not establish that the driver's protocol
 *          version matches the one libbinder was built for, and it does not establish that a service
 *          manager is running - a node with no service manager behind it still blocks, and the
 *          parent's bounded readiness timeout is the guard for that.
 *
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
 * A stale registration left behind by another process - an earlier host that was never reaped, or a
 * real HAL on a device - would make the middleware's own lookup resolve against that service instead
 * of the fake this host publishes.  The invocation would still run, and its result would describe
 * something nobody chose.  So this is a hard failure and not a condition to work around: publishing
 * over the entry would hide the collision, and tolerating it would make a green result meaningless.@n
 * The query uses checkService(), which answers immediately with null when the name is free.
 * getService() would poll for seconds before concluding the same thing and can race with a service
 * registering concurrently, which is precisely the ambiguity this check exists to remove.
 *
 * @param [in] serviceName                - Production service name to test, obtained from the
 *                                          generated interface rather than spelled here
 *
 * @return int                                    - EXIT_SUCCESS, or the code this program must exit
 *                                                  with
 * @retval EXIT_SUCCESS                           - The name is free and the fake may be published
 * @retval EXIT_SETUP_FAILED                      - No service manager could be reached at all, so
 *                                                  whether the name is free is unknown
 * @retval EXIT_STALE_REGISTRATION                - Something else already holds the name
 *
 * @pre isBinderTransportPresent() has already reported true.  Calling this first risks the abort that
 *      check exists to prevent.
 *
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
 * Waits by reading the self-pipe the handler writes to, which is a genuine blocking wait: this
 * process consumes no CPU while it serves transactions on its binder threads and holds this thread
 * still.  There is no timed loop and no polling interval, because a wait that wakes on a timer would
 * be a wait that can be wrong about when to stop.@n
 * A read interrupted by the very signal being waited for is retried, and the byte the handler wrote
 * is then there to be read.  A read that reports end of file is treated as a request to stop as
 * well: it can only mean the write end has been closed, and there is nothing left to wait for.
 *
 * @param [out] signalNumber              - Receives the signal number that ended the wait, or 0 when
 *                                          the wait ended because the self-pipe closed
 *
 * @return bool                                   - Whether the wait ended as intended
 * @retval true                                   - A termination request was received, or the pipe
 *                                                  closed, and this program should exit cleanly
 * @retval false                                  - The self-pipe could not be read, which is an
 *                                                  internal failure rather than a shutdown
 *
 * @pre installShutdownHandlers() has already reported true.
 *
 * @note This is the wait used when the parent supplied no control and observation channel, and it is
 *       unchanged from the program's original behaviour for exactly that reason.  Where a channel was
 *       supplied, serveControlChannel() waits instead and watches the same self-pipe, so a
 *       termination signal is answered either way.
 *
 * @see handleShutdownSignal(), installShutdownHandlers(), serveControlChannel()
 */
static bool waitForShutdownSignal(int &signalNumber)
{
    unsigned char wakeByte = 0;

    for (;;) {
        const ssize_t result = ::read(g_shutdownPipeReadFd, &wakeByte, sizeof(wakeByte));

        if (result >= 0) {
            /*
             * A byte means the handler ran.  Zero means end of file, which can only mean the write
             * end has been closed, and there is nothing left to wait for either way.
             */
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
 * Three outcomes, because they demand three different responses from the caller and collapsing any
 * two of them would either hide a failure or report a deliberate shutdown as one.
 *
 * @see writeReplyLine(), serveControlChannel()
 */
enum class ReplyOutcome {
    /** @brief The whole line reached the observation descriptor; the session continues. */
    DELIVERED,

    /**
     * @brief The parent closed its end of the observation pipe.
     *
     * A legitimate way to end the session, indistinguishable in intent from end of file on the
     * control descriptor, so the caller shuts down cleanly and exits EXIT_SUCCESS.
     */
    PARENT_GONE,

    /**
     * @brief The reply could not be delivered and the parent has not gone away.
     *
     * The write failed for a reason other than an interruption or a broken pipe, the whole-call
     * deadline of OBSERVE_WRITE_TIMEOUT_MS expired with the line unfinished, or the line was longer
     * than MAX_REPLY_LINE_LENGTH and was refused rather than fragmented.  Either way the client is
     * waiting for a line it will never receive, so the caller exits EXIT_CONTROL_CHANNEL_FAILED
     * rather than continue a session that has silently desynchronised.
     */
    FAILED
};

/**
 * @brief Writes exactly one reply line to the observation descriptor within one whole-call deadline.
 *
 * Appends the single newline that terminates a reply - callers pass the reply text without it, so
 * there is one place in this program that decides how a reply is framed - and delivers the whole line
 * or reports why it could not, without ever blocking indefinitely.  The size cap below is what keeps a
 * delivered reply to a single atomic pipe write, so a client never has to make sense of half a line.
 * The write is raw and unbuffered, so a reply cannot sit in a stream buffer while the client waits
 * for it.@n
 * Three properties make the bound real, and each of them is a property of this function rather than
 * of the descriptor it is handed:
 * -# One deadline for the call.  A CLOCK_MONOTONIC instant OBSERVE_WRITE_TIMEOUT_MS ahead is computed
 *    on entry, and every wait is given the milliseconds remaining to it, clamped at zero.  An
 *    interruption or a partially accepted line therefore costs the time it consumed and does not
 *    start a fresh wait, which a per-attempt timeout would.
 * -# A nonblocking write, then the flags back as they were.  poll() reporting POLLOUT promises only
 *    that a write of at most PIPE_BUF bytes will not block, so this function sets O_NONBLOCK on the
 *    descriptor for the duration of the call and restores the caller's original flags on every way
 *    out, including every failure.  EAGAIN means the line is not accepted yet and is waited on again
 *    within the same deadline, not treated as a failure.
 * -# A size cap.  A line longer than MAX_REPLY_LINE_LENGTH is refused before any write is attempted,
 *    because a line larger than PIPE_BUF is no longer one atomic pipe write and a client that reads
 *    lines cannot recover from half of one.
 *
 * @param [in] observeFd                  - Observation descriptor to write to.  Its file status flags
 *                                          are changed for the duration of the call and restored
 *                                          before it returns
 * @param [in] reply                      - Reply text, without its terminator.  Must begin with
 *                                          "OK " or "ERR " to satisfy the protocol, and with its
 *                                          terminator must not exceed MAX_REPLY_LINE_LENGTH bytes
 *
 * @return ReplyOutcome                           - How the delivery ended
 * @retval ReplyOutcome::DELIVERED                - Every byte of the line was written, and the
 *                                                  descriptor's original flags were restored
 * @retval ReplyOutcome::PARENT_GONE              - The descriptor has no reader left to answer: the
 *                                                  write reported EPIPE, or the wait reported POLLERR,
 *                                                  POLLHUP or POLLNVAL.  The session ends cleanly on
 *                                                  all four, since none of them can be answered and
 *                                                  none of them is a fault of this program
 * @retval ReplyOutcome::FAILED                   - The line exceeded MAX_REPLY_LINE_LENGTH and was
 *                                                  not attempted, the monotonic clock or the
 *                                                  descriptor's flags could not be read or set, the
 *                                                  whole-call deadline expired with the line
 *                                                  unfinished, the descriptor rejected the write for
 *                                                  a reason other than EAGAIN, EINTR or EPIPE, or the
 *                                                  original flags could not be restored afterwards
 *
 * @pre SIGPIPE is ignored - ignoreBrokenPipeSignal() has reported true - otherwise a closed reader
 *      terminates this process before EPIPE can be observed.
 * @pre observeFd is open for writing in this process, which resolveControlChannelFds() established
 *      before the channel was accepted.
 *
 * @post The descriptor's file status flags are those it arrived with, on every return path; where
 *       they could not be restored the outcome is FAILED and the reason is traced, so a session never
 *       continues on a descriptor whose mode this program cannot describe.
 *
 * @warning The bound is on this call, not on the session.  A caller answering many commands spends at
 *          most OBSERVE_WRITE_TIMEOUT_MS per reply, and a client that stops reading altogether costs
 *          exactly one deadline, since the first expiry ends the session.
 *
 * @see ReplyOutcome, OBSERVE_WRITE_TIMEOUT_MS, MAX_REPLY_LINE_LENGTH, ignoreBrokenPipeSignal()
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

    /*
     * One deadline for the whole call, taken once, here.  Deriving every wait from the time left to
     * it is what makes the bound a bound: a per-attempt timeout is restarted by each short write and
     * by each interruption, so a client that accepts a byte at a time, or a signal that arrives
     * regularly, could hold this program for an unlimited multiple of OBSERVE_WRITE_TIMEOUT_MS while
     * every individual wait stayed honestly inside it.  CLOCK_MONOTONIC is the clock for this because
     * it cannot be stepped by an administrator or by NTP while the wait is in progress.
     */
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

    /*
     * The descriptor belongs to the caller and arrives in whatever mode the parent created it in,
     * which for an inherited pipe end is blocking.  poll() reporting POLLOUT does not promise that an
     * arbitrary write will not block - the guarantee covers at most PIPE_BUF bytes - so the mode is
     * changed for the duration of this call and restored on the way out, and the caller is handed back
     * the descriptor it lent rather than a different one.
     */
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
                /*
                 * An interruption costs the time it consumed and nothing more: the next pass
                 * recomputes what is left of the one deadline instead of starting a fresh wait.
                 */
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
                /*
                 * Nonblocking, and the pipe has less room than this line needs.  That is ordinary
                 * while a reader is briefly behind - poll() reports a pipe writable on free space,
                 * which is not the same question as room for this whole line - so it waits again on
                 * what is left of the deadline rather than reporting a failure.  The poll() above is
                 * what paces the retry, and the deadline is what ends it if the room never arrives.
                 */
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
            /*
             * A zero-length transfer on a positive request makes no progress, so retrying it would
             * spin forever. Report it rather than loop, exactly as the readiness write does.
             */
            std::cout << TRACE_PREFIX << "A reply write to fd " << observeFd
                      << " transferred nothing and made no progress" << std::endl;
            outcome = ReplyOutcome::FAILED;
            break;
        }

        written += static_cast<size_t>(result);
    }

    /*
     * The single restore point, reached by every way out of the loop, so the mode this function
     * imposed never outlives the call that imposed it.  A failure to restore is not swallowed: the
     * caller's descriptor is then not the one it lent, and a session continuing on it would rest on an
     * assumption this program can no longer make good - so a reply that was delivered is still
     * reported as a failure, while a parent that has already gone away stays PARENT_GONE, since that
     * session is ending cleanly either way.
     */
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
 * The protocol's one payload encoding, in both directions: a `deliver` command carries its frame this
 * way and a `last-sent` reply reports one back the same way.  Two digits per byte, no separators, no
 * prefix and no uppercase, so a client can compare a reply against an expected string without
 * normalising it.
 *
 * @param [in] bytes                      - Bytes to render
 *
 * @return std::string                            - Hexadecimal text, empty when the input is empty
 *
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
 * Strict about length and alphabet and about nothing else: an odd number of digits cannot describe
 * whole bytes and a non-hexadecimal character cannot be guessed at, so both are rejected rather than
 * partially decoded.  Uppercase digits are accepted even though the protocol specifies lowercase,
 * because tolerating them costs nothing and rejecting a payload a client meant correctly costs a
 * confusing test failure.@n
 * The decoded bytes are never inspected.  A frame's destination nibble, its opcode and its length are
 * the middleware's business and the assertion's; this host delivers what it was handed.
 *
 * @param [in]  hex                       - Token to decode
 * @param [out] bytes                     - Receives the decoded bytes.  Cleared first, and left empty
 *                                          when this reports failure
 *
 * @return bool                                   - Whether the token decoded
 * @retval true                                   - bytes holds hex.size() / 2 decoded bytes
 * @retval false                                  - The token was empty, of odd length, or contained a
 *                                                  character that is not a hexadecimal digit
 *
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
 * Separators are runs of spaces and tabs, so repeated or mixed whitespace between tokens is
 * harmless.  An empty result means the line carried no verb, which the caller treats as "not a
 * command" rather than as an error.
 *
 * @param [in] line                       - Command line, already stripped of its terminator
 *
 * @return std::vector<std::string>               - The tokens, verb first, empty for a blank line
 *
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

/**
 * @brief Executes one command line against the hosted fake and composes its reply.
 *
 * The whole of the protocol's semantics, in one place, so that the vocabulary a client codes against
 * and the vocabulary this program implements cannot drift apart across functions.@n
 * Two properties matter more than anything else here and are structural rather than incidental.  The
 * one trigger command, `deliver`, reaches the fake through its own fireOnMessageReceived() helper, so
 * the listener it invokes is exactly the one the middleware handed to `open()` and no second delivery
 * route is invented.  The observation commands read the fake's own counters and captures through its
 * accessors, so what a client learns is the fake's real state and not a copy this program keeps in
 * step by hand - a copy would be capable of reporting a transmit that never arrived, which is
 * precisely the failure the channel exists to make impossible.
 *
 * @param [in]  line                      - Command line, stripped of its terminator and of a trailing
 *                                          carriage return
 * @param [in]  fake                      - Hosted fake the command acts on
 * @param [out] reply                     - Receives the reply text without its terminator.  Untouched
 *                                          when this reports that there is no reply
 * @param [out] shutdownRequested         - Set to true by the `shutdown` command only; never cleared
 *                                          here, so a caller may initialise it once
 *
 * @return bool                                   - Whether a reply is to be sent
 * @retval true                                   - reply holds one line beginning "OK " or "ERR "
 * @retval false                                  - The line was blank or whitespace-only, which is not
 *                                                  a command and produces no reply
 *
 * @post `deliver` has invoked the listener the middleware supplied, if one is held, and an observation
 *       command has read the fake's live state.  Nothing here clears, rewinds or reconfigures the
 *       fake: every command either stimulates the receive path or reports what the fake already holds.
 *
 * @warning No verb resets the fake, deliberately.  FakeHdmiCecService::reset() clears the captured
 *          listener, so exposing it here would let a client destroy the live session's receive path
 *          mid-run and turn every subsequent `deliver` into `ERR no-listener` for a reason no
 *          assertion could explain.  Per-case isolation belongs to the in-process fixtures, which call
 *          reset() directly and re-open afterwards.
 *
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
 * Waits on the shutdown self-pipe and the control descriptor together, with poll(), so that neither
 * starves the other: a termination signal is answered while a command is outstanding, and a command
 * is answered while nothing has signalled.  The alternative - a blocking read on one of the two - is
 * what makes a host either deaf to its parent or unstoppable, and this program must be neither.@n
 * The binder threadpool is untouched by any of this.  Transactions addressed to the fake are served
 * by pool threads that were started before publication; this loop holds one thread, the main one,
 * and holds no lock of the fake's while it waits, so a command and an inbound transaction can be in
 * flight at the same time.@n
 * Framing is handled here and only here: bytes are accumulated until a newline, a partial line is
 * carried across reads rather than mis-parsed as a whole one, and a line longer than
 * MAX_COMMAND_LINE_LENGTH is answered and discarded rather than allowed to grow without bound.  Every
 * read is retried on interruption.
 *
 * @param [in]  controlFd                 - Descriptor commands are read from
 * @param [in]  observeFd                 - Descriptor replies are written to
 * @param [in]  fake                      - Hosted fake the commands act on
 * @param [out] signalNumber              - Receives the signal number that ended the session, or 0
 *                                          when it ended for any other reason
 *
 * @return int                                    - EXIT_SUCCESS, or the code the program must exit with
 * @retval EXIT_SUCCESS                           - The session ended cleanly: a `shutdown` command, end
 *                                                  of file on the control descriptor, a parent that
 *                                                  closed the observation descriptor, or a termination
 *                                                  signal
 * @retval EXIT_CONTROL_CHANNEL_FAILED            - poll() or read() failed for a reason other than an
 *                                                  interruption, or writeReplyLine() reported FAILED:
 *                                                  the whole-call deadline expired, the line exceeded
 *                                                  MAX_REPLY_LINE_LENGTH, or the descriptor rejected
 *                                                  it outright
 *
 * @pre installShutdownHandlers() has reported true, resolveControlChannelFds() has reported a channel,
 *      and ignoreBrokenPipeSignal() has reported true.
 *
 * @post On a return of EXIT_SUCCESS, every command that was read and dispatched has been answered with
 *       exactly one reply line, in order, with one exception: the reply in flight when the parent
 *       closed the observation descriptor, which the client that closed it is no longer waiting for.
 *       Commands a client queued behind a `shutdown`, and buffered bytes whose terminator never
 *       arrived, are deliberately not served.
 * @post On a return of EXIT_CONTROL_CHANNEL_FAILED, every command before the failing one has been
 *       answered in order and the command being answered when the channel failed has not; the exit
 *       code is the only notice the client gets, which is why the parent's own wait for a reply must
 *       be bounded and why this program never continues a session it can no longer answer on.
 *
 * @note End of file on the control descriptor is a clean shutdown and not a failure: it can only mean
 *       the parent closed its write end, which is a legitimate way to end the session and also what
 *       happens if the parent dies, so a host left behind by a dead parent stops instead of lingering.
 *
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

        /*
         * The shutdown pipe is examined first, deliberately. A termination signal outranks a command
         * that happens to be queued behind it: the parent has asked this host to stop and answering
         * one more command first would delay a teardown it is already waiting on.
         */
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

            /*
             * A trailing carriage return is stripped so that a client which frames its commands with
             * CRLF is understood rather than answered "ERR unknown-command ping\r".
             */
            if (!line.empty() && line[line.size() - 1] == '\r') {
                line.erase(line.size() - 1);
            }

            std::string reply;
            bool shutdownRequested = false;
            bool hasReply = false;

            if (discardingOverlongLine || line.size() > MAX_COMMAND_LINE_LENGTH) {
                /*
                 * Either the terminator of a line already discarded for length, or a whole overlong
                 * line that happened to arrive with its terminator inside one read - both are the
                 * same answer, and both must give it, or the cap would depend on how the client's
                 * bytes happened to be split across reads. The bytes are deliberately not parsed: a
                 * line this long has lost its framing, and what looks like a verb at the front of it
                 * cannot be trusted to be one.
                 */
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
                /*
                 * Both halves of this trace are untrusted and both go through the renderer.  The
                 * command arrived from another process, so it may carry any byte at all and may be
                 * thousands of bytes long - a `deliver` with a long payload, or a client that has
                 * lost its framing.  The reply is derived from it: an unrecognised verb is echoed
                 * back as "ERR unknown-command <verb>", so the reply carries caller bytes too.
                 * Rendering supplies the bounding this trace used to do for itself with a plain
                 * substr() - which is why TRACED_COMMAND_LIMIT is gone: two bounding mechanisms with
                 * different guarantees is exactly the divergence the one contract exists to prevent.
                 */
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
            /*
             * No terminator within the cap. The bytes are dropped now so the buffer cannot grow
             * without bound, and a flag remembers that one "ERR command-too-long" is owed when the
             * terminator eventually arrives - which keeps the one-reply-per-line guarantee intact
             * even for a client that has lost its framing.
             */
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
 * Runs the startup sequence documented on this file, in that order, and treats every step as
 * load-bearing: each failure traces what happened, returns a code of its own and writes no readiness
 * line, so a parent never mistakes a host that failed to publish for one that is serving.  Nothing is
 * swallowed and nothing degrades quietly - a host that cannot serve must not report that it can.
 *
 * @param [in] argc                       - Argument count.  Unused: this program takes no arguments
 * @param [in] argv                       - Argument vector.  Unused, for the same reason
 *
 * @return int                                    - Process exit status
 * @retval EXIT_SUCCESS                           - Published, served, and stopped cleanly by any of
 *                                                  its shutdown routes: a SIGTERM or SIGINT, whether
 *                                                  the wait was the plain one on the self-pipe or the
 *                                                  channel loop; a `shutdown` command, acknowledged
 *                                                  before the stop; end of file on the control
 *                                                  descriptor, or poll() reporting that its writer is
 *                                                  gone; or the parent closing the observation
 *                                                  descriptor, which ends the session as deliberately
 *                                                  as end of file does
 * @retval EXIT_STALE_REGISTRATION                - Something was already published under the
 *                                                  production service name
 * @retval EXIT_BAD_READY_FD                      - CEC_FAKE_HOST_READY_FD was set to something that
 *                                                  is not a plain non-negative descriptor number
 * @retval EXIT_REGISTRATION_FAILED               - The service manager refused to publish the fake
 * @retval EXIT_READINESS_WRITE_FAILED            - The readiness token could not be written, so the
 *                                                  parent can never learn this host is ready
 * @retval EXIT_NO_BINDER_TRANSPORT               - No usable binder driver node exists on this host
 * @retval EXIT_SETUP_FAILED                      - The shutdown path, the SIGPIPE disposition, the
 *                                                  fake, or the service manager could not be
 *                                                  established
 * @retval EXIT_BAD_CONTROL_CHANNEL               - A control and observation channel was asked for and
 *                                                  is not usable: one of the two variables without the
 *                                                  other, a value that is not a descriptor number, or
 *                                                  a descriptor that is not open here or is open in
 *                                                  the wrong direction
 * @retval EXIT_CONTROL_CHANNEL_FAILED            - The channel was accepted and then failed while it
 *                                                  was being served
 *
 * @post On every return path the process has published no readiness line unless it genuinely became
 *       ready, and holds no resource that outlives it: the binder registration is released with the
 *       process and the fake clears its own published pointer as it is destroyed.
 *
 * @warning Configuration is taken from the environment, never from the command line, so that the
 *          parent's launch is a plain exec of the path in CEC_FAKE_AIDL_HOST_PATH with no argument
 *          contract to keep in step.
 * @warning Both channel variables are validated - their spelling, and each descriptor's openness and
 *          direction - before the fake is published, and a rejected channel writes no readiness token,
 *          so a parent that botched that handoff learns it from an exit code rather than from its own
 *          expired timeout.  CEC_FAKE_HOST_READY_FD is held to the spelling alone: a value that parses
 *          and is not writable is found at the readiness write, after publication, and exits
 *          EXIT_READINESS_WRITE_FAILED.
 *
 * @see resolveReadinessFd(), resolveControlChannelFds(), verifyServiceNameIsFree(),
 *      registerFakeHdmiCecService(), serveControlChannel(), waitForShutdownSignal()
 */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * The pid is traced first because it is what a parent's diagnostics, and a person hunting an
     * orphan, both key on.  It is streamed rather than cast: pid_t is an integer type of unspecified
     * width, so a cast would either be redundant here or lossy somewhere else.
     */
    std::cout << TRACE_PREFIX << "Starting the out-of-process fake HDMI CEC AIDL service host, pid "
              << ::getpid() << std::endl;

    if (!installShutdownHandlers()) {
        return EXIT_SETUP_FAILED;
    }

    int readinessFd = -1;
    if (!resolveReadinessFd(readinessFd)) {
        return EXIT_BAD_READY_FD;
    }

    /*
     * The channel is resolved here, beside the readiness descriptor and before anything is published,
     * for the same reason: a handoff this program is going to reject costs nothing when it is rejected
     * before the fake exists and before a readiness token could mislead a parent.
     */
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

    /*
     * The name comes from the generated interface and is never spelled here, so this host cannot
     * drift from the name the middleware actually looks up.  Only the service is published: a client
     * receives its controller from the out-parameter of open(), so the controller is never a
     * separately registered name.
     */
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

    /*
     * Publish the pointer so that anything in this process which needs the hosted fake reaches the
     * one that was registered, following the same single-pointer idiom the legacy driver double in
     * this directory uses.  The strong reference above is what keeps it alive.
     */
    FakeHdmiCecService::setInstance(fake.get());
    std::cout << TRACE_PREFIX << "Constructed the fake HDMI CEC AIDL service "
              << fakeHdmiCecTraceLabel(fake.get()) << std::endl;

    /*
     * The service-side threadpool, started before publication so that no transaction can arrive with
     * no thread to serve it.  startThreadPool() is idempotent and the library's own default governs
     * the thread count: setThreadPoolMaxThreadCount() is deliberately not called, because lowering a
     * maximum another component already established can abort the process.
     */
    ::android::ProcessState::self()->startThreadPool();
    std::cout << TRACE_PREFIX << "Started the service-side binder threadpool" << std::endl;

    if (!registerFakeHdmiCecService(fake)) {
        std::cout << TRACE_PREFIX << "The fake could not be published as \"" << serviceName
                  << "\", so this host has nothing to serve and is not ready" << std::endl;
        return EXIT_REGISTRATION_FAILED;
    }

    std::cout << TRACE_PREFIX << "Published the fake as \"" << serviceName << "\"" << std::endl;

    /*
     * Ready, and only now.  The token is written raw and unbuffered: handed to a C++ stream it could
     * sit in that stream's buffer while the parent waits for it, which presents as a hang rather than
     * as a bug.  The trace below the write is diagnostics and is no part of the signal.
     */
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

    /*
     * Two ways to wait, and which one applies is decided by whether a parent supplied a channel.
     * With one, both the channel and the shutdown path are watched together so neither starves the
     * other.  Without one, this is the same blocking wait on the self-pipe the program has always
     * performed, unchanged, which is what keeps an invocation that supplies neither variable exactly
     * as it was.
     */
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

    /*
     * Release what this program took, on every path including a failed one.  The published pointer is
     * cleared so nothing can reach a fake that is going away, the self-pipe descriptors are closed,
     * the channel descriptors this process inherited are closed so the parent sees end of file
     * promptly rather than at exit, and the binder registration needs no withdrawal: the pinned
     * service manager exposes no removal API and process exit releases it.
     */
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
