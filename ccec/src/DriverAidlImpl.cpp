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
 * @file DriverAidlImpl.cpp
 *
 * @brief Implementation of the AIDL/binder back-end of the CCEC middleware Driver
 *
 * This translation unit holds the whole of the AIDL back-end: the twelve CCEC::Driver
 * overrides, the runtime service-availability query the selection point in
 * `ccec/src/Driver.cpp` asks, the binder preflight predicate that makes that query safe
 * to attempt at all, and the nested event listener that receives CEC messages from the
 * HAL.@n
 * Every method here mirrors its DriverImpl counterpart's structure, guards, log lines,
 * exceptions and statement order, with the `com.rdk.hal.hdmicec` AIDL call substituted
 * for the legacy HDMI CEC C API. Four of them - read(), isValidLogicalAddress(), poll()
 * and printFrameDetails() - make no HAL call on either back-end and are therefore
 * byte-for-byte copies of the legacy bodies, class name aside. That is deliberate: it
 * is what makes the receive and formatting paths behaviourally indistinguishable
 * between the two back-ends, so the Bus reader thread and everything above it are
 * untouched by this migration.
 *
 * Three observable differences from the legacy back-end are authorized, each forced by
 * the AIDL contract and each documented on the method that carries it - the 16-byte
 * frame limit in write(), the OperationNotSupportedException in writeAsync(), and the
 * coarser failure category in addLogicalAddress(). Any other observable difference is a
 * defect. Two items are blocked on inputs that were not supplied and are marked as such
 * in the code: B1 on getPhysicalAddress() and B2 on close().
 *
 * @note Include-order contract, and it is load bearing: every AIDL, binder and
 *       halcompat header is included before CCEC_BEGIN_NAMESPACE. The legacy
 *       DriverImpl.cpp includes its plain C HAL header from inside the namespace, which
 *       is tolerable there and must not be imitated here - the generated stubs open
 *       `namespace com::rdk::hal::hdmicec` and transitively pull the SDK headers that
 *       open `namespace android`, and nesting either inside CCEC would break the link
 *       and violate the one-definition rule.
 * @note No legacy HAL header is included and no legacy HAL symbol is referenced
 *       anywhere in this file. `ccec/src/DriverImpl.cpp` remains the sole production
 *       include of `ccec/drivers/hdmi_cec_driver.h`, which is possible only because
 *       getPhysicalAddress() reports its B1 block rather than working around it.
 * @note Global functions whose names collide with this class's own members - open,
 *       close, read, write and poll - are called through the global scope operator, so
 *       that `::open()` reaches the system call and `open()` reaches the member.
 *
 * @warning Requires C++17. The generated AIDL stubs include `<optional>`.
 * @warning This is the C++ libbinder AIDL backend, not the NDK backend: the pointer
 *          type is `android::sp<>`, the status type is `android::binder::Status`, and
 *          the server base is the generated `BnHdmiCecEventListener`.
 *
 * @see hdmicec/ccec/src/DriverAidlImpl.hpp
 * @see hdmicec/ccec/src/DriverImpl.cpp
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdint>
#include <string>
#include <vector>

/*
 * AIDL and binder headers. These must stay above CCEC_BEGIN_NAMESPACE - see the
 * include-order note in the file block above.
 */
#include <binder/IBinder.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <binder/Status.h>
#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/StrongPointer.h>
#include <com/rdk/hal/hdmicec/BnHdmiCecEventListener.h>
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <com/rdk/hal/hdmicec/SendMessageStatus.h>
#include <com/rdk/hal/hdmicec/State.h>

/*
 * The shared HAL interface compatibility helpers. This header lives under
 * `rdk-halif-aidl/common/current/` rather than inside either frozen snapshot include
 * root, so a build that reaches the generated stubs but not this file is missing an
 * include root rather than missing a dependency.
 */
#include "halcompat.h"

/*
 * Binder kernel ABI, used by the preflight predicate to inspect the driver node
 * directly - deliberately WITHOUT going through libbinder, because the whole point of
 * the preflight is to decide whether touching libbinder is safe at all.
 *
 * The header is part of the kernel UAPI and is present wherever libbinder itself can be
 * built. It is nevertheless probed rather than required, because a middleware sysroot
 * carrying a prebuilt libbinder.so without the kernel headers is a configuration that
 * can exist. When it is absent the preflight reports "AIDL absent" and the legacy
 * back-end is selected, which is the safe direction to fail in: an unverifiable
 * platform must not be allowed to reach libbinder, where a missing or
 * protocol-mismatched driver node aborts the process outright.
 */
#if defined(__has_include)
#  if __has_include(<linux/android/binder.h>)
#    include <linux/android/binder.h>
/** @brief Set to 1 when the binder kernel UAPI definitions are available. */
#    define CCEC_HAVE_BINDER_UAPI 1
#  endif
#endif
#ifndef CCEC_HAVE_BINDER_UAPI
/** @brief Set to 0 when the binder kernel UAPI definitions are unavailable. */
#define CCEC_HAVE_BINDER_UAPI 0
#endif

#include "DriverAidlImpl.hpp"

#include "osal/EventQueue.hpp"
#include "osal/Exception.hpp"
#include "ccec/Util.hpp"
#include "ccec/Exception.hpp"
#include "ccec/OpCode.hpp"

using CCEC_OSAL::AutoLock;

/**
 * @brief Short alias for the generated `com.rdk.hal.hdmicec` C++ namespace.
 *
 * Introduced instead of `using`-declarations for the individual types: the CCEC
 * middleware compiles with CCEC_NAMESPACE undefined, so its own names sit at global
 * scope, and pulling generic names such as `State` in beside them invites a collision
 * that a later header could create silently.
 */
namespace cechal = ::com::rdk::hal::hdmicec;

/** @brief Short alias for the shared HAL compatibility helpers in halcompat.h. */
namespace halcompat = ::com::rdk::hal::halcompat;

/*
 * Frame block offsets. DriverImpl.hpp defines these for the legacy back-end;
 * DriverAidlImpl.hpp deliberately does not, so they are defined here with the same
 * values, guarded so that a translation unit which has already seen the legacy header
 * keeps one definition.
 */
#ifndef HEADER_OFFSET
/** @brief Index of the CEC header block within a frame. */
#define HEADER_OFFSET 0
#endif
#ifndef OPCODE_OFFSET
/** @brief Index of the CEC opcode block within a frame. */
#define OPCODE_OFFSET 1
#endif

CCEC_BEGIN_NAMESPACE

namespace {

/**
 * @brief Largest CEC message the AIDL transmit contract accepts, in bytes
 *
 * `IHdmiCecController.sendMessage()` states a maximum message size of 16 bytes for the
 * header block plus the opcode block plus the operand blocks. CECFrame carries up to
 * CECFrame::MAX_LENGTH (128) and the legacy HAL specification documents 20, so this
 * limit is the source of authorized observable difference 1. It exists as a named
 * constant so that write() polices the frame against the contract rather than against a
 * literal, and so that the value has exactly one definition.
 *
 * @see DriverAidlImpl::write()
 */
const size_t AIDL_MAX_MESSAGE_LENGTH = 16;

/**
 * @brief Shortest received CEC message this back-end will accept onto the incoming queue
 *
 * One byte, and the exact value matters in both directions.
 *
 * It cannot be zero. An out-of-process HAL can deliver an empty `std::vector<uint8_t>` -
 * nothing in the AIDL contract prevents it and no in-process HAL could produce it, so
 * nothing downstream was ever written to survive it. An empty frame queued here is taken by
 * the Bus reader thread, which hands it to printFrameDetails(); that decodes
 * `Header(frame, HEADER_OFFSET)`, which reaches `frame.at(0)` and raises
 * `std::out_of_range`. printFrameDetails() catches `Exception &`, the CCEC base, which does
 * not match it; Bus::Reader::run() catches `InvalidStateException &`, which does not match
 * it either. The exception therefore escapes the thread function and terminates the
 * process. One malformed message from the HAL is enough. Rejecting the message here, before
 * anything is allocated, is the only place inside this back-end that can prevent it -
 * `ccec/src/Bus.cpp` and the legacy back-end are out of bounds for this migration.
 *
 * It must not be two. A one-byte frame is legitimate CEC traffic: a poll, the header-only
 * ping this class's own poll() transmits, carries exactly one byte and no opcode. The
 * legacy receive path delivers such a frame, and printFrameDetails() handles it safely -
 * `Header(frame, 0)` succeeds and the `frame.length() > OPCODE_OFFSET` test simply skips
 * the opcode decode. Requiring a header plus an opcode would therefore drop valid frames
 * and be a behaviour regression, which is why the bound stops at the smallest length that
 * is decodable rather than at the smallest length that is useful.
 *
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 */
const size_t MIN_RECEIVED_MESSAGE_LENGTH = 1;

/**
 * @brief Lowest logical address the AIDL contract permits, inclusive
 *
 * @see HAL_LOGICAL_ADDRESS_MAX
 */
const int32_t HAL_LOGICAL_ADDRESS_MIN = 0x0;

/**
 * @brief Highest logical address the AIDL contract permits, inclusive
 *
 * THE CONTRACT IS `0x0..0xE` AND IT IS THE HAL'S SIDE OF IT THAT MUST BE POLICED HERE.
 * `IHdmiCecController.addLogicalAddresses()` documents that range for the addresses a
 * client may ask for, and `IHdmiCec.getLogicalAddresses()` returns addresses drawn from
 * the same space; 0xF is the CEC broadcast/unregistered address and is not an address a
 * device holds.@n
 * WHY THE RAW `int32_t` IS CHECKED RATHER THAN THE CONVERTED VALUE. What arrives from an
 * out-of-process HAL is an arbitrary 32-bit integer that the generated proxy reads
 * straight out of the parcel - nothing between the HAL and this back-end constrains it.
 * Converting first and validating afterwards does not work: `LogicalAddress`'s
 * constructor takes the value through a narrower type, so 256 becomes 0 and 271 becomes
 * 0xF, and a validation applied after that conversion sees a plausible address and accepts
 * a value the HAL never reported. Downstream that matters concretely, because
 * `Connection::matchSource()` uses the address the middleware believes it holds to rewrite
 * the initiator nibble of outbound frames - so an accepted-but-wrong address is put on the
 * CEC wire.@n
 * A rejected value falls back to this method's EXISTING no-address behaviour, a zero
 * return, which `LibCCEC::getLogicalAddress()` already turns into an
 * InvalidStateException. Nothing new is invented for the failure path.
 *
 * @see DriverAidlImpl::getLogicalAddress()
 */
const int32_t HAL_LOGICAL_ADDRESS_MAX = 0xE;

/**
 * @brief Most message bytes the receive diagnostic renders before it truncates
 *
 * The receive callback runs on a binder threadpool thread and must return promptly, so its
 * diagnostic has to have a ceiling rather than a length that follows the message. A CEC
 * message cannot legitimately exceed the 16 bytes the AIDL transmit contract states, and
 * CECFrame can carry up to 128, so this ceiling shows every byte of any well-formed message
 * with margin, and truncates a malformed one instead of tracking it.
 *
 * @see renderReceivedMessageHex()
 */
const size_t RECEIVE_LOG_MAX_BYTES = 24;

/** @brief Appended to the rendering when the message was longer than RECEIVE_LOG_MAX_BYTES. */
const char RECEIVE_LOG_TRUNCATION_MARKER[] = "...";

/**
 * @brief Size of the stack buffer the receive diagnostic renders into
 *
 * Three characters per rendered byte - two hex digits and the separating space, matching the
 * `"%02X "` rendering the legacy dump produced - plus room for the truncation marker and its
 * terminator, which `sizeof` already includes. Derived rather than written out, so the
 * buffer cannot fall out of step with the ceiling above.
 */
const size_t RECEIVE_LOG_TEXT_SIZE = (RECEIVE_LOG_MAX_BYTES * 3) + sizeof(RECEIVE_LOG_TRUNCATION_MARKER);

/**
 * @brief Renders a received CEC message as bounded `"%02X "` hex text, without calling out
 *
 * The receive path's whole diagnostic: one bounded line for the callback, rather than a
 * line per stage plus a per-byte-`printf` dump. It matters that this is cheap and finite: the callback it serves
 * arrives on a binder threadpool thread for every CEC message the HAL delivers, and the
 * work it does is work the HAL's own thread pool waits on.@n
 * The rendering is unconditional, and that is a constraint rather than a choice.
 * `dump_buffer()` could skip its work by testing `cec_log_level`, but that variable is
 * file-static in `ccec/src/Util.cpp` and there is no accessor, so no code outside that
 * translation unit can ask whether LOG_DEBUG output is even enabled. The rendering therefore
 * happens on every message regardless of log level, which is exactly why it must be
 * call-free and bounded: two direct nibble-to-character writes plus one separator per byte,
 * into a fixed stack buffer, with no function call, no allocation and no stdio in the loop.
 * CCEC_LOG() then discards the finished line cheaply when the configured level excludes
 * LOG_DEBUG.
 *
 * @param [in]  message - First byte of the message to render. Not read at all when
 *                        @p length is zero.
 * @param [in]  length  - Number of bytes available at @p message. Only the first
 *                        RECEIVE_LOG_MAX_BYTES are rendered; the rest are represented by
 *                        RECEIVE_LOG_TRUNCATION_MARKER.
 * @param [out] text    - Fixed-size buffer receiving the NUL-terminated rendering. Taken as
 *                        a reference to an array of exactly RECEIVE_LOG_TEXT_SIZE
 *                        characters, so a buffer of the wrong size is a compile error
 *                        rather than an overflow.
 *
 * @pre @p message addresses at least @p length readable bytes.
 * @post @p text holds a NUL-terminated string of at most RECEIVE_LOG_TEXT_SIZE - 1
 *       characters. The worst case is bounded by construction: 3 characters per rendered
 *       byte, at most RECEIVE_LOG_MAX_BYTES of them, plus the marker.
 * @warning Never throws, never blocks, performs no allocation and calls nothing.
 *
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 */
void renderReceivedMessageHex(const uint8_t *message, size_t length, char (&text)[RECEIVE_LOG_TEXT_SIZE])
{
	static const char hexDigits[] = "0123456789ABCDEF";

	const size_t rendered = (length > RECEIVE_LOG_MAX_BYTES) ? RECEIVE_LOG_MAX_BYTES : length;
	size_t at = 0;

	for (size_t i = 0; i < rendered; i++) {
		text[at++] = hexDigits[(message[i] >> 4) & 0x0F];
		text[at++] = hexDigits[message[i] & 0x0F];
		text[at++] = ' ';
	}

	if (rendered < length) {
		for (const char *marker = RECEIVE_LOG_TRUNCATION_MARKER; *marker != '\0'; marker++) {
			text[at++] = *marker;
		}
	}

	text[at] = '\0';
}

/**
 * @brief Elapsed time, in milliseconds, above which one synchronous HAL call is reported
 *
 * A diagnostic threshold, not a timeout and not a gate. Crossing it changes nothing about
 * the call: no transaction is abandoned, no exception is raised, no state moves and no
 * retry happens. The only effect is an attempt at one LOG_WARN line naming the operation
 * and the elapsed time, so that a stall which would otherwise be silent can leave
 * evidence in the log a field engineer can find. Whether that line is emitted is
 * CCEC_LOG's decision, since it drops anything above the configured log level.@n
 * The value is taken from the only documented timing figure this project has - the HDMI
 * CEC HAL specification's bound of one second on CEC transmit completion, 200 ms desired.
 * It is deliberately not derived from a benchmark, because no performance measurement has
 * been taken: that measurement is deferred human work on target hardware. Every operation
 * instrumented with it is either a transmit or an operation that must complete well inside
 * a transmit's budget, so one second is a threshold no healthy HAL crosses and every
 * wedged one does.
 *
 * @warning This bounds nothing. The pinned libbinder C++ backend exposes no client-side
 *          transaction deadline, so an unbounded synchronous call remains unbounded and is
 *          merely made observable. Reading this constant as a timeout would be a serious
 *          mistake, and every synchronous HAL call in this file is annotated accordingly.
 *
 * @see warnIfHalCallSlow()
 */
const int64_t SLOW_HAL_CALL_WARN_MS = 1000;

/**
 * @brief Start instant standing for "the monotonic clock could not be read"
 *
 * Negative, so it cannot collide with any real CLOCK_MONOTONIC reading. When a start
 * instant carries this value the elapsed measurement is skipped altogether rather than
 * computed from a fabricated origin: a diagnostic that cannot be trusted is worth less
 * than no diagnostic, and inventing an origin is exactly how a bound becomes no bound at
 * all.
 *
 * @see halCallStarted()
 * @see warnIfHalCallSlow()
 */
const int64_t HAL_CALL_CLOCK_UNREADABLE = -1;

/**
 * @brief Reads CLOCK_MONOTONIC as whole milliseconds
 *
 * The single clock utility in this translation unit, used both by the synchronous-call
 * diagnostics and by the binder context-manager probe's deadline. It sits outside the
 * CCEC_HAVE_BINDER_UAPI guard, unlike the probe that also uses it, because the
 * diagnostics are compiled into every build while the probe additionally needs the binder
 * kernel ABI definitions. One helper serves both, so there is no second clock utility to
 * drift.@n
 * The monotonic clock is used deliberately rather than the wall clock - a wall-clock step
 * during initialization must not extend or collapse either a diagnostic measurement or a
 * bound whose purpose is to keep LibCCEC::init() from stalling. Milliseconds in `int64_t`
 * cannot overflow on any reachable uptime, so no narrowing and no wraparound is possible
 * here.
 *
 * @param [out] nowMs - Receives the reading, in milliseconds. Left untouched on failure.
 *
 * @return bool - Whether the clock could be read
 * @retval true  - @p nowMs holds the current monotonic instant.
 * @retval false - clock_gettime() failed, @p nowMs is unmodified, and the caller must
 *                 treat the measurement as unavailable rather than assume a value.
 *
 * @pre None.
 * @warning Never throws and never blocks.
 *
 * @see halCallStarted()
 */
bool monotonicNowMs(int64_t &nowMs)
{
	struct timespec now;

	if (0 != clock_gettime(CLOCK_MONOTONIC, &now)) {
		return false;
	}

	nowMs = (((int64_t)now.tv_sec) * 1000LL) + (((int64_t)now.tv_nsec) / 1000000LL);

	return true;
}

/**
 * @brief Captures the instant immediately before a synchronous HAL call is issued
 *
 * Paired with warnIfHalCallSlow() around each synchronous AIDL operation. Two clock reads
 * per HAL operation is negligible against an IPC round trip, which is why the
 * instrumentation can be unconditional and needs no sampling, no counter and no state of
 * its own.
 *
 * @return int64_t - Monotonic start instant in milliseconds, or HAL_CALL_CLOCK_UNREADABLE
 *                   when the clock could not be read.
 *
 * @pre None.
 * @warning Never throws and never blocks.
 *
 * @see warnIfHalCallSlow()
 */
int64_t halCallStarted(void)
{
	int64_t nowMs = 0;

	return monotonicNowMs(nowMs) ? nowMs : HAL_CALL_CLOCK_UNREADABLE;
}

/**
 * @brief Attempts one diagnostic line when a synchronous HAL call ran past the threshold
 *
 * The observable half of the treatment for an unbounded synchronous HAL call: the call
 * cannot be bounded, so it is measured instead. This runs after the call has returned,
 * computes how long it took, and when that exceeds SLOW_HAL_CALL_WARN_MS attempts one
 * LOG_WARN line. Whether the line is emitted is CCEC_LOG's decision rather than this
 * function's, since CCEC_LOG drops anything above the configured log level; below the
 * threshold nothing is attempted at all, so no fast path gains a log line. Nothing else
 * happens either way - the call has already returned, so there is nothing left to
 * abandon.@n
 * It exists because the alternatives do not exist or are forbidden. The pinned libbinder
 * C++ backend offers no client-side transaction deadline;
 * `IPCThreadState::talkWithDriver()` retries on EINTR, so neither a signal nor an interval
 * timer can break a blocked transaction out; a bounded-join worker thread could not cancel
 * one either and is forbidden by this migration's threading constraint; and narrowing the
 * driver lock is forbidden because the legacy serialization must be preserved exactly.
 * What remains in scope is making a stall visible, which is what this does.
 *
 * @param [in] operation - Stable name of the AIDL operation that just returned, used
 *                         verbatim in the log so a reader can tell which call stalled.
 * @param [in] startedMs - Value halCallStarted() returned before the call. When it is
 *                         HAL_CALL_CLOCK_UNREADABLE the measurement is skipped.
 *
 * @pre @p startedMs came from halCallStarted() around the same call.
 * @post At most one LOG_WARN line was attempted. Nothing else changed: no HAL state, no
 *       middleware state and no return value anywhere.
 * @warning Never throws and never alters control flow: the caller's status translation,
 *          exception mapping and return value are entirely unaffected.
 * @warning Not nonblocking, and no deadline is enforced anywhere in it. It enforces
 *          nothing on the HAL, and when CCEC_LOG does emit it formats into a stack buffer
 *          and writes through `printf`, which can block on whatever the log sink is. It
 *          runs after the call it describes, so that delay is never added to a HAL call.
 *
 * @see SLOW_HAL_CALL_WARN_MS
 * @see halCallStarted()
 */
void warnIfHalCallSlow(const char *operation, int64_t startedMs)
{
	int64_t nowMs = 0;

	if ((HAL_CALL_CLOCK_UNREADABLE == startedMs) || !monotonicNowMs(nowMs)) {
		return;
	}

	const int64_t elapsedMs = nowMs - startedMs;

	if (elapsedMs > SLOW_HAL_CALL_WARN_MS) {
		/*
		 * One line, naming the exact call and the elapsed time, and bounded independently
		 * against the 499-byte CCEC_LOG expansion limit with the longest operation label
		 * this file passes (the halcompat::isCompatible one) substituted in. It states
		 * plainly that nothing was enforced, so the line can never be mistaken for evidence
		 * of a mitigation, and it points at the platform prerequisite rather than at a
		 * finding number a log reader cannot resolve.
		 */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl: synchronous AIDL call [%s] returned after %lld ms, past the %lld ms threshold. NO DEADLINE WAS ENFORCED and the call was not abandoned - the pinned libbinder offers no client-side transaction bound. A HAL that stalls this long breaks the platform prerequisite recorded on DriverAidlImpl::isServiceAvailable()\r\n", operation, (long long)elapsedMs, (long long)SLOW_HAL_CALL_WARN_MS);
	}
}

#if CCEC_HAVE_BINDER_UAPI

/**
 * @brief Size of the transient binder mapping the context-manager probe installs
 *
 * The binder driver allocates the buffer for an incoming REPLY out of the receiving
 * process's own mapping, so a probe that expects a reply must map something. The value
 * is far smaller than libbinder's own mapping because the probe exchanges a ping and
 * nothing else, and the mapping is unmapped before the predicate returns either way.
 */
const size_t BINDER_PROBE_MAP_SIZE = 64 * 1024;

/**
 * @brief Upper bound on the iterations the context-manager probe will perform
 *
 * A second, structural bound alongside the caller's timeout. The driver interleaves
 * bookkeeping commands - transaction-complete, no-op, spawn-looper - with the reply, so
 * the probe must drain rather than read once; this caps that drain so the predicate
 * terminates even if the clock misbehaves or the driver returns an unexpected stream.
 */
const unsigned int BINDER_PROBE_MAX_ITERATIONS = 64;

/**
 * @brief Longest single poll() wait the context-manager probe will ask for, in milliseconds
 *
 * The probe waits in slices rather than in one call, and this is the length of a slice.
 * That is what makes the `int` that poll() takes safe: the caller's timeout is an
 * `unsigned int` and a large value converted directly to `int` can land negative, and
 * `poll(..., -1)` waits without limit - the exact opposite of the bound this probe exists to
 * provide. A value clamped into [0, POLL_SLICE_MAX_MS] cannot do that, whatever the caller
 * passed and whatever the clock reports.@n
 * Slices also make the wait re-derive itself from the absolute deadline on every iteration,
 * so no single accounting error can extend the total.
 *
 * @see pingBinderContextManager()
 */
const unsigned int POLL_SLICE_MAX_MS = 250;

/*
 * The two bounds must not fight each other: if the iteration cap could expire before the
 * deadline it would cut a legitimately waiting probe short and report a usable context
 * manager as unreachable. Whole slices needed to consume the largest timeout the class
 * accepts must therefore fit inside the cap. Checked at compile time so that changing
 * either constant cannot quietly break the relationship. Note that a handful of iterations
 * are also consumed by EINTR retries and by drain passes that deliver no reply, which is
 * why the cap is left comfortably above the required minimum rather than trimmed to it.
 */
static_assert((DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS / POLL_SLICE_MAX_MS) <= BINDER_PROBE_MAX_ITERATIONS,
              "BINDER_PROBE_MAX_ITERATIONS must cover MAX_CONTEXT_MANAGER_TIMEOUT_MS in POLL_SLICE_MAX_MS slices");

/**
 * @brief Asks the binder driver, under a bounded wait, whether handle 0 resolves
 *
 * Binder handle 0 is the context manager - the `servicemanager` daemon every name
 * lookup goes through. libbinder obtains it by pinging handle 0 and, in the pinned
 * stack, retries that in one-second intervals with no upper bound, so a platform whose
 * driver works but whose `servicemanager` never started would stall middleware
 * initialization indefinitely. This function asks the same question with a deadline,
 * and it asks it over a file descriptor of its own so that no libbinder singleton is
 * created and the process is left exactly as it was.
 *
 * The exchange is the minimum the kernel ABI allows: map a small region so the driver
 * has somewhere to place a reply, post one synchronous BC_TRANSACTION carrying
 * PING_TRANSACTION at handle 0, then drain the driver's command stream under the
 * deadline until the reply or a rejection appears. A missing context manager is
 * rejected promptly with BR_FAILED_REPLY rather than being waited on, so the common
 * negative case costs nothing.
 *
 * How the wait is bounded, and why it is built this way rather than the obvious way:
 * - one absolute deadline is computed before the loop, in 64-bit milliseconds, and every
 *   iteration re-derives its budget from that deadline. Computing a residual from a fresh
 *   elapsed measurement each time - the obvious way - lets any single accounting error
 *   re-grant the full timeout on every pass, so the loop's real bound becomes the
 *   iteration count multiplied by the timeout rather than the timeout;
 * - each wait is sliced and clamped into `[0, POLL_SLICE_MAX_MS]` before the conversion to
 *   the `int` poll() takes. An unclamped `unsigned int` timeout can convert to a negative
 *   `int`, and `poll(..., -1)` blocks without limit. The clamp makes that unreachable by
 *   construction rather than by the caller behaving reasonably;
 * - a slice expiring is not the deadline expiring: the loop continues while the deadline
 *   still leaves budget, and reports the timeout only once it does not;
 * - a failed clock read fails the probe as "unreachable" instead of being treated as zero
 *   elapsed time. An unbounded wait is a worse outcome than a false negative, and a false
 *   negative here simply selects the legacy back-end;
 * - BINDER_PROBE_MAX_ITERATIONS still caps the loop, as belt and braces alongside the
 *   deadline. The static assertion beside it keeps that cap from firing before the deadline
 *   does.
 *
 * @param [in] driverFd  - Open binder driver descriptor whose protocol version has
 *                         already been verified. Left open; the caller closes it.
 * @param [in] timeoutMs - Upper bound, in milliseconds, on waiting for an answer. Zero
 *                         means poll once without waiting, which is how the negative arm
 *                         is exercised, and that meaning is load bearing. The caller
 *                         clamps this to DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS
 *                         before calling, and the slicing above makes the wait safe
 *                         regardless of the value that arrives here.
 *
 * @return bool - Whether handle 0 answered the ping
 * @retval true  - A reply arrived, so a context manager is registered and reachable.
 * @retval false - The mapping failed, the transaction could not be posted, the monotonic
 *                 clock could not be read, the driver rejected the ping, or the deadline
 *                 expired first.
 *
 * @pre @p driverFd is a binder driver descriptor opened O_RDWR.
 * @post The probe's mapping is released on every path, including the early ones. The reply
 *       buffer the driver charged to this descriptor is reclaimed when the caller closes
 *       it, which is why the probe does not issue BC_FREE_BUFFER of its own.
 * @warning Never throws, and never blocks longer than @p timeoutMs plus at most one poll
 *          slice of scheduling latency. Every unexpected condition is treated as
 *          "unreachable" - the direction that yields the legacy back-end.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 * @see POLL_SLICE_MAX_MS
 * @see monotonicNowMs()
 */
bool pingBinderContextManager(int driverFd, unsigned int timeoutMs)
{
	void *mapped = ::mmap(0, BINDER_PROBE_MAP_SIZE, PROT_READ,
	                      MAP_PRIVATE | MAP_NORESERVE, driverFd, 0);
	if (MAP_FAILED == mapped) {
		CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: binder mmap failed with errno %d, treating the context manager as unreachable\r\n", errno);
		return false;
	}

	/*
	 * The write buffer is a command word immediately followed by the transaction
	 * payload, with no alignment padding between them - that is the layout the driver
	 * parses, and it is why the two are memcpy'd into a byte array rather than declared
	 * as a struct the compiler would pad.
	 */
	unsigned char writeBuffer[sizeof(uint32_t) + sizeof(struct binder_transaction_data)];
	const uint32_t writeCommand = BC_TRANSACTION;
	struct binder_transaction_data transaction;

	memset(&transaction, 0, sizeof(transaction));
	transaction.target.handle = 0;                              /* the context manager */
	transaction.code = ::android::IBinder::PING_TRANSACTION;
	transaction.flags = TF_ACCEPT_FDS;                          /* synchronous: TF_ONE_WAY deliberately unset */

	memcpy(writeBuffer, &writeCommand, sizeof(writeCommand));
	memcpy(writeBuffer + sizeof(writeCommand), &transaction, sizeof(transaction));

	struct binder_write_read exchange;

	memset(&exchange, 0, sizeof(exchange));
	exchange.write_size = sizeof(writeBuffer);
	exchange.write_buffer = (binder_uintptr_t)(uintptr_t)writeBuffer;

	bool answered = false;
	bool finished = false;

	if (::ioctl(driverFd, BINDER_WRITE_READ, &exchange) < 0) {
		CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: could not post the context manager ping, errno %d\r\n", errno);
		finished = true;
	}

	/*
	 * One absolute deadline, in 64-bit milliseconds, established before the loop. Every
	 * iteration measures against this instant rather than recomputing an elapsed time, so
	 * no iteration can re-grant the caller's full timeout to itself.
	 */
	int64_t deadlineMs = 0;

	if (!finished) {
		int64_t startedMs = 0;

		if (!monotonicNowMs(startedMs)) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: the monotonic clock could not be read, so the context manager wait cannot be bounded; treating the AIDL HAL as absent\r\n");
			finished = true;
		}
		else {
			deadlineMs = startedMs + (int64_t)timeoutMs;
		}
	}

	for (unsigned int iteration = 0; !finished && (iteration < BINDER_PROBE_MAX_ITERATIONS); iteration++) {
		int64_t nowMs = 0;

		if (!monotonicNowMs(nowMs)) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: the monotonic clock stopped being readable mid-probe, so the remaining wait cannot be bounded; treating the AIDL HAL as absent\r\n");
			break;
		}

		const int64_t remainingMs = deadlineMs - nowMs;
		int64_t sliceMs = (remainingMs < 0) ? 0 : remainingMs;

		if (sliceMs > (int64_t)POLL_SLICE_MAX_MS) {
			sliceMs = (int64_t)POLL_SLICE_MAX_MS;
		}

		struct pollfd waiter;

		waiter.fd = driverFd;
		waiter.events = POLLIN;
		waiter.revents = 0;

		/*
		 * sliceMs is now known to lie in [0, POLL_SLICE_MAX_MS], so this conversion cannot
		 * produce a negative int and this poll() cannot become an unbounded wait.
		 */
		const int ready = ::poll(&waiter, 1, (int)sliceMs);

		if (ready < 0) {
			if (EINTR == errno) {
				continue;   /* the absolute deadline above still bounds the retry */
			}
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: waiting on the binder driver failed, errno %d\r\n", errno);
			break;
		}
		if (0 == ready) {
			if (remainingMs > (int64_t)sliceMs) {
				continue;   /* the slice expired, not the deadline */
			}
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: the binder context manager did not answer within %u ms, treating the AIDL HAL as absent\r\n", timeoutMs);
			break;
		}

		unsigned char readBuffer[256];

		memset(&exchange, 0, sizeof(exchange));
		exchange.read_size = sizeof(readBuffer);
		exchange.read_buffer = (binder_uintptr_t)(uintptr_t)readBuffer;

		if (::ioctl(driverFd, BINDER_WRITE_READ, &exchange) < 0) {
			if (EINTR == errno) {
				continue;
			}
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: reading the binder reply failed, errno %d\r\n", errno);
			break;
		}
		if (0 == exchange.read_consumed) {
			break;   /* readable but nothing delivered: do not spin */
		}

		size_t consumed = 0;

		while ((consumed + sizeof(uint32_t)) <= (size_t)exchange.read_consumed) {
			uint32_t command = 0;

			memcpy(&command, readBuffer + consumed, sizeof(command));
			consumed += sizeof(command);
			consumed += (size_t)_IOC_SIZE(command);   /* the ABI encodes each command's payload size */

			if (BR_REPLY == command) {
				answered = true;
				finished = true;
				break;
			}
			if ((BR_FAILED_REPLY == command) || (BR_DEAD_REPLY == command) || (BR_ERROR == command)) {
				CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: the binder driver rejected the context manager ping with command 0x%08x, treating the AIDL HAL as absent\r\n", command);
				finished = true;
				break;
			}
			/* Anything else - transaction complete, no-op, spawn looper - is skipped. */
		}
	}

	::munmap(mapped, BINDER_PROBE_MAP_SIZE);

	return answered;
}

#endif /* CCEC_HAVE_BINDER_UAPI */

/*
 * The four default probe operations, i.e. the real kernel-facing calls the preflight
 * performs in production. They exist unconditionally, whether or not this build has the
 * binder kernel UAPI definitions, and that is deliberate: the CCEC_HAVE_BINDER_UAPI
 * guard is hoisted down into them so that isBinderPreflightOk() itself has exactly five
 * decision arms in every configuration. A predicate whose branch structure changed with
 * a build macro could not be gated per branch, and each of these five arms carries one
 * record in the coverage branch manifest.
 *
 * On a build without the UAPI definitions the verdict is unchanged - false - because the
 * version read below fails, and the reason is logged rather than inferred.
 *
 * `::open` and `::close` are qualified for consistency with the rest of this file, where
 * the qualification is required because DriverAidlImpl has members of both names.
 */

/**
 * @brief Default probe: opens the binder driver node with the real `::open`
 *
 * A thin wrapper rather than `&::open` itself, because `::open` is variadic and its
 * address therefore has a type no fixed-arity function pointer can hold.
 *
 * @param [in] path  - Node to open.
 * @param [in] flags - Open flags; the preflight passes `O_RDWR | O_CLOEXEC`.
 *
 * @return int - Descriptor, or negative with `errno` set.
 *
 * @pre None.
 * @warning Never throws.
 *
 * @see DriverAidlImpl::defaultBinderProbe()
 */
int defaultOpenBinderNode(const char *path, int flags)
{
	return ::open(path, flags);
}

/**
 * @brief Copies a `struct stat` into the plain-integer identity the preflight carries
 *
 * The ONE place the POSIX stat types are converted, so that DriverAidlImpl.hpp needs no
 * `<sys/stat.h>` and no host's `dev_t` or `ino_t` width leaks across the probe seam. Each
 * field widens into the widest plain integer that can hold it, so nothing is truncated on
 * any supported host.
 *
 * @param [in]  attributes - Result of a successful `fstat` or `stat`.
 * @param [out] identity   - Receives the converted identity.
 *
 * @pre @p identity is non-null.
 * @post Every member of @p identity is written; none is left at its previous value.
 * @warning Never throws and performs no allocation.
 *
 * @see DriverAidlImpl::BinderNodeIdentity
 */
void copyNodeIdentity(const struct stat &attributes, DriverAidlImpl::BinderNodeIdentity *identity)
{
	identity->device = (unsigned long long)attributes.st_dev;
	identity->inode  = (unsigned long long)attributes.st_ino;
	identity->rdev   = (unsigned long long)attributes.st_rdev;
	identity->mode   = (unsigned int)attributes.st_mode;
	identity->uid    = (unsigned int)attributes.st_uid;
}

/**
 * @brief Default probe: identifies the node an open descriptor refers to, with `fstat`
 *
 * Asked of the DESCRIPTOR rather than of the path, which is the whole point: the answer
 * describes the object this process already holds open, so nothing outside the process can
 * change it afterwards, and it is therefore usable as the reference the later re-check
 * compares a fresh path resolution against.
 *
 * @param [in]  driverFd - Descriptor returned by defaultOpenBinderNode().
 * @param [out] identity - Receives the identity. Written only on success.
 *
 * @return int - Outcome
 * @retval 0  - The identity was written.
 * @retval -1 - `fstat` failed, or @p identity was null; `errno` carries the reason in the
 *              first case and is set to EINVAL in the second.
 *
 * @pre @p driverFd is an open descriptor.
 * @warning Never throws.
 *
 * @see DriverAidlImpl::defaultBinderProbe()
 */
int defaultIdentifyBinderDescriptor(int driverFd, DriverAidlImpl::BinderNodeIdentity *identity)
{
	struct stat attributes;

	if (NULL == identity) {
		errno = EINVAL;
		return -1;
	}

	memset(&attributes, 0, sizeof(attributes));

	if (::fstat(driverFd, &attributes) < 0) {
		return -1;
	}

	copyNodeIdentity(attributes, identity);

	return 0;
}

/**
 * @brief Default probe: identifies whatever a path currently resolves to, with `stat`
 *
 * `stat` and not `lstat`, which is REQUIRED rather than an oversight: a binderfs
 * deployment may legitimately publish `/dev/binder` as a symlink to
 * `/dev/binderfs/binder`, and the identity this yields has to be the identity of the node
 * libbinder will actually open when it resolves the same name. Refusing to follow the link
 * would refuse a real, correctly provisioned platform.
 *
 * @param [in]  path     - Path to resolve, the same one the preflight opened.
 * @param [out] identity - Receives the identity. Written only on success.
 *
 * @return int - Outcome
 * @retval 0  - The identity was written.
 * @retval -1 - The path could not be resolved or stat'ed, or an argument was null;
 *              `errno` carries the reason, or EINVAL for a null argument.
 *
 * @pre None. A path that has ceased to exist is a legitimate input and yields -1.
 * @warning Never throws.
 *
 * @see DriverAidlImpl::defaultBinderProbe()
 * @see DriverAidlImpl::isServiceAvailable()
 */
int defaultIdentifyBinderPath(const char *path, DriverAidlImpl::BinderNodeIdentity *identity)
{
	struct stat attributes;

	if ((NULL == path) || (NULL == identity)) {
		errno = EINVAL;
		return -1;
	}

	memset(&attributes, 0, sizeof(attributes));

	if (::stat(path, &attributes) < 0) {
		return -1;
	}

	copyNodeIdentity(attributes, identity);

	return 0;
}

/**
 * @brief Default probe: reads the driver's protocol version with `BINDER_VERSION`
 *
 * Carries the version out as an `unsigned int` so that no binder kernel type appears in
 * DriverAidlImpl.hpp, and reports failure the way the preflight expects rather than
 * letting a kernel type or an ioctl return code cross the seam.
 *
 * @param [in]  driverFd        - Descriptor returned by defaultOpenBinderNode().
 * @param [out] protocolVersion - Receives the reported version. Written only on success.
 *
 * @return int - Outcome
 * @retval 0  - The version was read.
 * @retval -1 - The ioctl failed, or this build carries no binder kernel ABI definitions
 *              with which to ask. `errno` carries the ioctl's reason in the first case
 *              and ENOSYS in the second, so the caller's log line is meaningful either
 *              way.
 *
 * @pre @p driverFd is an open descriptor and @p protocolVersion is non-null.
 * @warning Never throws.
 *
 * @see DriverAidlImpl::expectedBinderProtocolVersion()
 */
int defaultReadBinderProtocolVersion(int driverFd, unsigned int *protocolVersion)
{
#if CCEC_HAVE_BINDER_UAPI
	struct binder_version driverVersion;

	memset(&driverVersion, 0, sizeof(driverVersion));

	if (::ioctl(driverFd, BINDER_VERSION, &driverVersion) < 0) {
		return -1;
	}

	*protocolVersion = (unsigned int)driverVersion.protocol_version;

	return 0;
#else
	(void)driverFd;
	(void)protocolVersion;

	CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: this build carries no binder kernel ABI definitions, so the driver's protocol version cannot be read; treating the AIDL HAL as absent\r\n");

	errno = ENOSYS;

	return -1;
#endif
}

/**
 * @brief Default probe: pings binder handle 0 under the caller's bound
 *
 * Delegates to pingBinderContextManager() where the binder kernel ABI definitions exist.
 * Where they do not, this is unreachable in practice - the version read above has
 * already failed the predicate - and reports "unreachable", which is the direction that
 * yields the legacy back-end.
 *
 * @param [in] driverFd  - Descriptor whose protocol version has been verified.
 * @param [in] timeoutMs - Upper bound in milliseconds; zero means do not wait.
 *
 * @return bool - Whether a context manager answered
 * @retval true  - It answered.
 * @retval false - It did not within the bound, or this build cannot ask.
 *
 * @pre @p driverFd is an open binder driver descriptor.
 * @warning Never throws and never blocks past @p timeoutMs.
 *
 * @see pingBinderContextManager()
 */
bool defaultPingBinderContextManager(int driverFd, unsigned int timeoutMs)
{
#if CCEC_HAVE_BINDER_UAPI
	return pingBinderContextManager(driverFd, timeoutMs);
#else
	(void)driverFd;
	(void)timeoutMs;

	return false;
#endif
}

/**
 * @brief Default probe: releases the driver descriptor with the real `::close`
 *
 * @param [in] driverFd - Descriptor to release.
 *
 * @return int - Zero on success, negative with `errno` set on failure. The preflight
 *               ignores it, exactly as the code this replaced ignored `::close`'s.
 *
 * @pre @p driverFd was returned by defaultOpenBinderNode().
 * @warning Never throws.
 *
 * @see DriverAidlImpl::defaultBinderProbe()
 */
int defaultCloseBinderNode(int driverFd)
{
	return ::close(driverFd);
}

} /* anonymous namespace */

/**
 * @brief Receives the HAL's `oneway` CEC events on a binder threadpool thread
 *
 * The AIDL counterpart of the legacy DriverImpl::DriverReceiveCallback() and
 * DriverImpl::DriverTransmitCallback() function pointers, defined here in the
 * implementation file so that the generated server-side base is not pulled into every
 * translation unit that includes DriverAidlImpl.hpp. Only three callbacks are
 * implemented, because BnHdmiCecEventListener already supplies onTransact() and
 * concrete getInterfaceVersion()/getInterfaceHash().@n
 * Of the three, only onMessageReceived() carries behaviour. The other two are
 * diagnostics: acting on them would be new behaviour with no legacy counterpart, since
 * an in-process HAL can neither change state behind the middleware's back nor vanish.
 *
 * Every callback returns `binder::Status::ok()`, including when it has caught a
 * failure. A `oneway` callback has no caller to receive a fault, and letting an
 * exception escape into onTransact() would be worse than dropping one frame - which is
 * exactly the disposition the legacy receive callback takes when it deletes the frame
 * on throw.
 *
 * Listener lifetime, and why detachment rather than destruction is the mechanism. The
 * owner holds one `android::sp<EventListener>` and the HAL holds a strong reference of
 * its own, taken when the listener crossed the binder boundary in `IHdmiCec::open()`.
 * Releasing the owner's reference therefore does not necessarily destroy the object:
 * the HAL's reference can keep it alive and, with it, keep alive a back pointer to a
 * DriverAidlImpl that is being or has been destroyed. That is reachable rather than
 * theoretical, because the AIDL "no further callbacks" guarantee holds only after a
 * successful `IHdmiCec::close()`, while a failed close still drives this back-end to
 * CLOSED - and ~DriverAidlImpl() deliberately swallows that failure, exactly as
 * ~DriverImpl() swallows its own.@n
 * detach() closes that window. Every callback that touches the owner does so under the
 * listener's own lock, and detach() nulls the back pointer under that same lock, so a
 * detached listener the HAL still calls logs and drops rather than dereferencing freed
 * state. The owner detaches on every path that ends a session: both arms of close(),
 * the failure arms of open(), and unconditionally in its destructor.
 *
 * Lock order, stated so a later reader can re-check it rather than re-derive it. The
 * listener lock is a leaf with one documented exception: a receive callback holds it
 * while calling EventQueue::offer(), which takes and releases the queue's own lock
 * inside. The teardown side never runs in the opposite order - close() calls
 * `rQueue.offer(0)` first, which takes and releases the queue lock, and only then calls
 * detach() - so the two acquisitions never nest in both directions and there is no
 * inversion to deadlock on. The one wait that is genuinely absent is the capacity wait:
 * a full queue makes offer() discard rather than block, and EventQueue::poll() waits on
 * its condition variable outside the queue lock, so a blocked consumer cannot hold the
 * queue lock against a producer. What remains is real and is not a time bound - offer()
 * acquires the queue lock, may allocate as it appends, and locks and broadcasts the
 * queue's condition variable - so detach() waits for an in-flight callback to finish
 * that work and for no longer, which is a completion bound rather than a proven duration.
 * The listener lock is also never taken while the owner's instance lock is required in
 * the other direction: a callback reaches the owner only through getIncomingQueue(),
 * which takes no instance lock at all.
 *
 * @warning Reaches its owner through the back pointer below and never through
 *          Driver::getInstance(). The legacy receive callback resolves its target with
 *          `static_cast<DriverImpl &>(Driver::getInstance())`, which is well defined
 *          only because it is registered from DriverImpl::open() and so never runs on
 *          the AIDL path. Reusing that route here would be an ill-typed downcast and
 *          undefined behaviour rather than a failed assertion.
 * @warning Not copyable: RefBase, reached through BnHdmiCecEventListener, keeps its
 *          copy constructor and copy assignment private.
 * @warning Instances are reference counted and must only ever be held in an
 *          `android::sp<>`; the HAL holds a strong reference of its own for the
 *          lifetime of the session, which is why the object may outlive the owner's
 *          reference to it and why detach() exists.
 *
 * @see DriverAidlImpl::EventListener::detach()
 * @see DriverAidlImpl::getIncomingQueue()
 * @see DriverImpl::DriverReceiveCallback()
 */
class DriverAidlImpl::EventListener : public cechal::BnHdmiCecEventListener
{
public:
	/**
	 * @brief Binds a listener to the back-end instance that will consume its frames
	 *
	 * @param [in] ownerDriver - Back-end that owns this listener and whose incoming
	 *                           queue received frames are offered onto. Its address is
	 *                           stored, not a reference, so that detach() can null it
	 *                           when the owner ends the session; a reference member
	 *                           could not be cleared and would leave the only way to
	 *                           stop the callbacks be destroying an object the HAL may
	 *                           still hold a strong reference to.
	 *
	 * @pre @p ownerDriver outlives this listener, or the owner detaches it before it
	 *      ceases to. The second alternative is what the owner actually guarantees.
	 * @post The listener is attached. Callbacks deliver to @p ownerDriver until
	 *       detach() is called.
	 * @warning Never throws.
	 *
	 * @see detach()
	 */
	explicit EventListener(DriverAidlImpl &ownerDriver) : owner(&ownerDriver)
	{
		CCEC_LOG( LOG_DEBUG, "Creating DriverAidlImpl::EventListener done\r\n");
	}

	/**
	 * @brief Severs the link to the owner, after any in-flight callback has finished
	 *
	 * The whole of the CWE-416 guarantee, and it rests on one property: every callback
	 * that touches the owner holds this listener's lock for its entire body, so taking
	 * that same lock here means detach() cannot return while a callback is part-way
	 * through dereferencing the owner. On return, therefore, no callback is inside the
	 * owner and no later callback can enter it - the back pointer is null and
	 * onMessageReceived() drops instead of delivering. That blocking property is the
	 * point of taking the lock rather than merely writing a null.@n
	 * It is called on every path that ends a session, including the ones that failed,
	 * because a failed `IHdmiCec::close()` leaves the HAL entitled to keep calling: the
	 * AIDL guarantee of no further callbacks holds only after a close that succeeded.
	 *
	 * Idempotent, and deliberately so: close() and ~DriverAidlImpl() may both reach it
	 * for the same listener, and open()'s failure arm may reach it for a listener that
	 * never received anything.
	 *
	 * @pre None. Safe on an attached or an already detached listener, and safe to call
	 *      from any thread.
	 * @post The owner back pointer is null, no callback is executing inside the owner,
	 *       and every subsequent callback is a logged drop.
	 * @warning Blocks until an in-flight callback completes, and no time bound is claimed
	 *          for that wait. The one thing a callback cannot do is wait for queue
	 *          capacity, because a full queue makes EventQueue::offer() discard rather
	 *          than block. What it can do is acquire the queue's own lock, allocate as
	 *          the offer appends, and lock and broadcast the queue's condition variable -
	 *          so this call is bounded by the callback finishing that work and by nothing
	 *          shorter.
	 * @warning Does not destroy this object and must not be confused with doing so. The
	 *          HAL holds its own strong reference, so destruction is not the owner's to
	 *          schedule - which is precisely why detachment is the mechanism.
	 *
	 * @see onMessageReceived()
	 * @see DriverAidlImpl::close()
	 */
	void detach(void)
	{
		{AutoLock lock_(ownerMutex);
			owner = 0;
		}
	}

	/**
	 * @brief Delivers one received CEC message onto the owner's incoming queue
	 *
	 * The AIDL replacement for the legacy `HdmiCecRxCallback_t`, and a step-for-step
	 * equivalent of DriverImpl::DriverReceiveCallback(): the payload is copied into a
	 * freshly allocated CECFrame, logged, and handed to the incoming queue through the
	 * owner's state-guarded, acceptance-reporting handoff.@n
	 * Going through the owner rather than touching the queue member is the load-bearing
	 * part. The handoff reaches the queue through getIncomingQueue(), which raises when
	 * the driver is not OPENED, and that is what rejects a callback which arrives during
	 * or after a close and drives the release of the frame that was about to be
	 * enqueued. Offering to the member directly would accept frames the legacy path
	 * rejects.
	 *
	 * Ownership is explicit here, because the queue can refuse. `EventQueue::offer()`
	 * returns void and silently discards its argument once the queue is at capacity, so a
	 * callback that offered and then dropped its pointer would leak one heap frame per
	 * event, without bound, whenever the Bus reader falls behind. Instead
	 * DriverAidlImpl::offerReceivedFrame() reports whether it took the frame: on true the
	 * local pointer is cleared immediately, so neither the code below nor any catch arm
	 * can touch a frame the queue now owns; on false this callback still owns the frame,
	 * releases it and says so at LOG_EXP. Nothing between the successful handoff and the
	 * clearing of the pointer can throw, so there is no window in which both would free
	 * it.
	 *
	 * Diagnostic output is one bounded line, not a loop. The success path makes one
	 * CCEC_LOG() call, carrying the byte count and a hex rendering of at most
	 * RECEIVE_LOG_MAX_BYTES bytes built by renderReceivedMessageHex() into a fixed-size
	 * stack buffer. That bound matters because this runs on a binder thread for every
	 * message received, and the alternative shape - a marker line per stage plus
	 * dump_buffer(), which issues one stdio call per byte - puts an unbounded amount of
	 * per-byte logging on the receive path. The rendering keeps dump_buffer()'s `%02X `
	 * spelling, so the bytes read the same in a log. A refusal, a state-guard rejection
	 * or a length rejection is reported at LOG_EXP, so a line here with no LOG_EXP line
	 * after it means the frame was queued.
	 *
	 * The body is copy-and-enqueue and holds no HAL call and no IPC, so a binder
	 * threadpool thread is never held here waiting on the HAL. It is not free of waits:
	 * the listener lock, the queue's own lock, the allocation the frame copy performs and
	 * the condition-variable broadcast the offer issues can each delay this thread, and
	 * no time bound is claimed. What it cannot do is wait for queue capacity, because a
	 * full queue is a refusal. The frame is handed to the existing cross-thread queue the
	 * Bus reader already drains, so the only thing this back-end changes about the
	 * receive path is which thread produces into that queue.
	 *
	 * The message length is checked first and the detach second, and nothing is
	 * allocated before either. The length check needs neither the owner nor the lock, so
	 * it runs ahead of both and an under-length message never even contends for the
	 * listener lock; MIN_RECEIVED_MESSAGE_LENGTH records why an under-length message
	 * would otherwise terminate the process. The owner back pointer is then read under
	 * the listener lock, and a null pointer - the state detach() leaves behind - means
	 * the session this listener belonged to has ended. A message arriving then is logged
	 * and dropped: not enqueued, not allocated, and above all not delivered through a
	 * pointer to storage that may already be gone. The HAL is entitled to arrive here
	 * after a failed close, so this arm is reachable rather than defensive. The lock is
	 * held across the whole delivery, which is what lets detach() guarantee that no
	 * callback is inside the owner once it returns.
	 *
	 * @param [in] message - The raw CEC message as received by the HAL, header byte
	 *                       first. A message shorter than MIN_RECEIVED_MESSAGE_LENGTH is
	 *                       rejected before the lock is taken and before anything is
	 *                       allocated: an out-of-process HAL can deliver an empty vector,
	 *                       and an empty frame on the queue kills the Bus reader thread
	 *                       and with it the process. A one-byte frame is a legitimate CEC
	 *                       poll and is accepted. The contents are not inspected; see the
	 *                       post-condition for the whole of what is checked.
	 *
	 * @return ::android::binder::Status - Always ok
	 * @retval ok - Returned unconditionally, including after a caught failure and after
	 *              a post-detach drop. See the class documentation for why a `oneway`
	 *              callback must not report a fault.
	 *
	 * @pre The owner is attached and OPENED, otherwise the frame is dropped before
	 *      allocation or discarded after it, respectively.
	 * @post Either the frame is on the incoming queue or it has been released, or no
	 *       frame was ever allocated. It is never leaked and never double freed, in every
	 *       one of the six exits: queued; rejected for length before allocation; dropped
	 *       after a detach before allocation; refused at the queue's receive limit;
	 *       rejected by the OPENED-state guard; or lost to an allocation or append
	 *       failure.
	 * @post Anything that reaches the queue carried at least MIN_RECEIVED_MESSAGE_LENGTH
	 *       bytes, fitted CECFrame::MAX_LENGTH - `CECFrame::append()` raises past it and
	 *       the general catch arm releases the frame - and was successfully allocated and
	 *       copied. That is the whole of the validation. The message is not decoded here:
	 *       no header, opcode or operand is examined, and no claim is made that the
	 *       contents are well formed. What the length floor rules out is specifically the
	 *       empty frame, whose header decode on the Bus reader thread raises
	 *       `std::out_of_range` out of an uncaught path and takes the process down.
	 * @warning Runs on a binder threadpool thread, not on any middleware thread.
	 * @warning A refusal means the Bus reader is not draining. Dropping the frame is then
	 *          the only choice left - the alternative is unbounded growth on a path a
	 *          remote HAL drives - and the drop is logged rather than silent.
	 *
	 * @see detach()
	 * @see DriverAidlImpl::offerReceivedFrame()
	 * @see MIN_RECEIVED_MESSAGE_LENGTH
	 * @see DriverAidlImpl::read()
	 */
	::android::binder::Status onMessageReceived(const ::std::vector<uint8_t> &message) override
	{
		/*
		 * Length check first, before the lock and before anything is allocated. An empty
		 * message from an out-of-process HAL would otherwise become an empty frame on the
		 * incoming queue, and the Bus reader's decode of that frame raises
		 * std::out_of_range - which neither printFrameDetails() nor Bus::Reader::run()
		 * catches, so it escapes the thread function and takes the process with it. See
		 * MIN_RECEIVED_MESSAGE_LENGTH for the full chain and for why the bound is one byte
		 * rather than two. It runs ahead of the detach check because it needs neither the
		 * owner nor the lock, and running it first keeps an under-length message from even
		 * contending for the listener lock.
		 */
		if (message.size() < MIN_RECEIVED_MESSAGE_LENGTH) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener::onMessageReceived : the HAL delivered a %zu byte message, shorter than the %zu byte minimum a CEC frame can carry; discarding it rather than queueing an undecodable frame\r\n", message.size(), MIN_RECEIVED_MESSAGE_LENGTH);

			return ::android::binder::Status::ok();
		}

		{AutoLock lock_(ownerMutex);
			if (owner == 0) {
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener: message received after detach, dropping it\r\n");

				return ::android::binder::Status::ok();
			}

			CECFrame *frame = 0;

			try {
				frame = new CECFrame();
				frame->append(message.data(), message.size());

				/*
				 * One bounded diagnostic for the whole callback. The obvious alternative -
				 * a CCEC_LOG() marker per stage, each a vsnprintf plus a timestamp plus a
				 * printf, and a dump_buffer() issuing one printf() per byte, up to 128 of
				 * them - puts that cost on a binder thread for every message received. The
				 * rendering below is call-free and capped instead; see
				 * renderReceivedMessageHex() for why it cannot be skipped on log level and
				 * therefore has to be cheap.
				 */
				char messageText[RECEIVE_LOG_TEXT_SIZE];

				renderReceivedMessageHex(message.data(), message.size(), messageText);

				CCEC_LOG( LOG_DEBUG, ">>> DriverAidlImpl::EventListener::onMessageReceived : %zu bytes : %s\r\n", message.size(), messageText);

				/*
				 * Ownership is explicit, because the queue can refuse.
				 * `EventQueue::offer()` returns void and silently discards its argument
				 * once the queue is at capacity, so a callback that offered and then
				 * dropped its pointer would leak one heap frame per event, without bound,
				 * whenever the Bus reader falls behind. offerReceivedFrame() reports
				 * whether it took the frame; it still reaches the queue through
				 * getIncomingQueue(), so the OPENED-state guard that rejects a callback
				 * arriving during or after a close still runs and still raises.
				 */
				if (owner->offerReceivedFrame(frame)) {
					/*
					 * The queue owns the frame from this point. Clear the local pointer
					 * before anything else runs, so that neither the code below nor any
					 * catch arm can release a frame that is no longer this callback's.
					 * Nothing between the handoff returning true and this assignment can
					 * throw.
					 */
					frame = 0;
				}
				else {
					/*
					 * The queue refused the frame, so it did not take it and this callback
					 * still owns it. Releasing it here is what keeps a stalled reader from
					 * turning every further event into a leaked frame. The refusal point
					 * is one entry below the capacity, because the last slot is reserved
					 * for close()'s wake-the-reader sentinel; the line below names both
					 * numbers so a log reader is not left thinking the queue reported
					 * itself full one entry early.
					 */
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener::onMessageReceived : the incoming frame queue refused the frame at its %zu entry receive limit, one below its %zu entry capacity so close()'s sentinel always fits; releasing a %zu byte frame rather than leaking it\r\n", (size_t)(DriverAidlImpl::INCOMING_QUEUE_CAPACITY - 1), (size_t)DriverAidlImpl::INCOMING_QUEUE_CAPACITY, message.size());

					delete frame;
					frame = 0;
				}
			}
			catch(InvalidStateException &e) {
				/*
				 * The closed-state rejection, given its own arm so it is distinguishable
				 * in a log from an allocation or append failure. This is the expected and
				 * reachable outcome of a frame arriving while the driver is not OPENED:
				 * getIncomingQueue(), reached through offerReceivedFrame(), raises before
				 * anything is offered, so the frame is still this callback's to release.
				 * It is the AIDL counterpart of the legacy delete-on-throw cleanup, and
				 * separating it from the general arm is what stops a routine
				 * close-race drop from reading like a fault.
				 */
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener::onMessageReceived : the driver is not OPENED (%s), so the incoming queue rejected a %zu byte frame; releasing it\r\n", e.what(), message.size());

				delete frame;
			}
			catch(...) {
				CCEC_LOG( LOG_EXP, "Exception during frame offer...discarding\r\n");
				delete frame;
			}
		}

		return ::android::binder::Status::ok();
	}

	/**
	 * @brief Records a HAL state transition in the log and does nothing else
	 *
	 * There is deliberately no action here. Considered and rejected: offering the NULL
	 * sentinel onto the incoming queue on a transition to `State::CLOSED`, so that a
	 * blocked Bus reader unwinds. It has no legacy counterpart - an in-process HAL
	 * cannot close itself underneath the middleware - so it would be new behaviour, and
	 * new behaviour is out of bounds for a transport migration. Death recipients and
	 * `linkToDeath()` are omitted for the same reason.
	 *
	 * @param [in] oldState - State the HAL is leaving.
	 * @param [in] newState - State the HAL has entered.
	 *
	 * @return ::android::binder::Status - Always ok
	 * @retval ok - Returned unconditionally.
	 *
	 * @pre None.
	 * @post No middleware state has changed.
	 * @post No exception escapes, on any path.
	 * @warning Runs on a binder threadpool thread.
	 * @warning THE COMPLETE BODY IS CONTAINED IN A CATCH-ALL, and it has to be. This is
	 *          a `oneway` callback, so THERE IS NO CALLER TO RECEIVE A FAULT: an
	 *          exception leaving here unwinds into the generated `onTransact()`, which is
	 *          not written to expect one, and from there into libbinder's threadpool
	 *          loop, taking down a binder thread over a log line. The one thing in the
	 *          body that can raise is `cechal::toString()`, which returns a
	 *          `std::string` by value for each argument, so either construction can
	 *          raise `std::bad_alloc`. Dropping the diagnostic and returning ok is the
	 *          only correct disposition, because there is nothing to report a failure to.
	 * @warning THE FALLBACK DIAGNOSTIC MUST ITSELF BE ALLOCATION-FREE. The handler
	 *          renders both states as plain integers through `%d`, constructs no
	 *          `std::string` and calls no `toString()`. A handler that repeated the
	 *          failing construction - or that allocated at all, having most likely
	 *          arrived there because an allocation failed - would defeat the containment
	 *          it exists to provide. `CCEC_LOG()` is safe to call from the handler: it
	 *          formats into a fixed 500-byte stack buffer with `vsnprintf` and allocates
	 *          nothing (`ccec/src/Util.cpp`).
	 * @see onMessageReceived()
	 * @see onMessageSent()
	 */
	::android::binder::Status onStateChanged(cechal::State oldState, cechal::State newState) override
	{
		/*
		 * THE WHOLE BODY IS CONTAINED, for the same reason onMessageReceived()'s is: this
		 * is a `oneway` callback, so it has NO CALLER to receive a fault. An exception
		 * escaping here unwinds into the generated `onTransact()`, which is not written to
		 * expect one, and from there into libbinder's threadpool loop - taking down a
		 * binder thread of this process over a log line. There is nothing to report a
		 * failure TO, so the only correct disposition is to drop the diagnostic and return
		 * ok.
		 *
		 * What can actually throw is not hypothetical: `cechal::toString()` returns a
		 * `std::string` by value for each argument, and either construction can raise
		 * `std::bad_alloc` - a real possibility on a receive path a HAL can drive as fast
		 * as it likes.
		 */
		try {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl::EventListener: HAL state changed from %s to %s\r\n", cechal::toString(oldState).c_str(), cechal::toString(newState).c_str());
		}
		catch(...) {
			/*
			 * THE FALLBACK MUST NOT BE ABLE TO THROW IN TURN, which is why it renders the
			 * two states as PLAIN INTEGERS through a `%d` conversion and constructs no
			 * `std::string` and calls no `toString()`. A handler that repeated the failing
			 * construction - or that allocated at all, having most likely arrived here
			 * because an allocation failed - would defeat the containment it exists to
			 * provide. `CCEC_LOG()` itself is safe to call from here: it formats into a
			 * fixed 500-byte stack buffer with `vsnprintf` and allocates nothing
			 * (`ccec/src/Util.cpp`), so the fallback cannot raise in turn.
			 */
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener: HAL state changed from %d to %d; the descriptive form of this diagnostic could not be built and was dropped\r\n", (int)oldState, (int)newState);
		}

		return ::android::binder::Status::ok();
	}

	/**
	 * @brief Records the outcome of a HAL-side transmit in the log and nothing else
	 *
	 * A diagnostic only, mirroring the shape of DriverImpl::DriverTransmitCallback(),
	 * which the legacy back-end also uses for nothing but logging. Synchronous transmit
	 * results reach callers as the return value of
	 * `IHdmiCecController::sendMessage()`, so nothing here feeds back into the
	 * middleware.@n
	 * The legacy callback logs only on failure. This one logs unconditionally, because
	 * `SendMessageStatus` has no back-end-independent success value: `ACK_STATE_0`
	 * means acknowledged for a directed message and rejected for a broadcast, so
	 * deciding "failure" would require re-deriving the destination nibble from the
	 * message. Doing that in a log-only callback would duplicate write()'s translation
	 * in a second place where it could drift, so the status is reported verbatim and
	 * left to the reader.
	 *
	 * The line carries the message bytes as well as the status and the length. A status on
	 * its own cannot be tied to a transmit when several are in flight, and a length cannot
	 * distinguish two frames of the same size, so a report meant to be read after the fact
	 * has to name which message it is about. The bytes are rendered as lowercase hex into a
	 * bounded stack buffer sized for `CECFrame::MAX_LENGTH`, and a longer message - which
	 * the HAL cannot legitimately echo, since write() refuses to send one - is truncated in
	 * the rendering only, with an ellipsis, because a diagnostic must not be able to
	 * overrun and must not allocate on a binder thread.
	 *
	 * @param [in] message - The message the HAL transmitted, echoed back.
	 * @param [in] status  - Bus outcome the HAL observed.
	 *
	 * @return ::android::binder::Status - Always ok
	 * @retval ok - Returned unconditionally.
	 *
	 * @pre None.
	 * @post No middleware state has changed.
	 * @post No exception escapes, on any path.
	 * @warning Runs on a binder threadpool thread.
	 * @warning THE COMPLETE BODY IS CONTAINED IN A CATCH-ALL, and it has to be. This is
	 *          a `oneway` callback, so THERE IS NO CALLER TO RECEIVE A FAULT: an
	 *          exception leaving here unwinds into the generated `onTransact()`, which is
	 *          not written to expect one, and from there into libbinder's threadpool
	 *          loop, taking down a binder thread over a log line. The hex rendering is
	 *          bounded and allocates nothing, but `cechal::toString()` returns a
	 *          `std::string` by value and can raise `std::bad_alloc`, and this callback
	 *          fires once per transmit, so the containment is not theoretical.
	 * @warning THE FALLBACK DIAGNOSTIC MUST ITSELF BE ALLOCATION-FREE. The handler
	 *          reports the status as a plain integer through `%d` and the message length,
	 *          constructing no `std::string`, calling no `toString()` and not repeating
	 *          the hex rendering. `CCEC_LOG()` is safe to call from the handler: it
	 *          formats into a fixed 500-byte stack buffer with `vsnprintf` and allocates
	 *          nothing (`ccec/src/Util.cpp`).
	 *
	 * @see DriverAidlImpl::write() - where a transmit outcome is actually acted on
	 * @see onStateChanged() - same containment, same reason
	 */
	::android::binder::Status onMessageSent(const ::std::vector<uint8_t> &message, cechal::SendMessageStatus status) override
	{
		/*
		 * THE WHOLE BODY IS CONTAINED, for the reason onStateChanged() states: a `oneway`
		 * callback has no caller to receive a fault, so an escaping exception would unwind
		 * into the generated `onTransact()` and then into libbinder's threadpool loop.
		 * `cechal::toString()` returns a `std::string` by value and can raise
		 * `std::bad_alloc`, and this callback fires once per transmit, so the containment
		 * is not theoretical.
		 */
		try {
			/*
			 * Two hex digits per byte plus the terminator, with the capacity fixed at compile
			 * time from the frame maximum rather than from the argument, so the rendering
			 * cannot be made to grow by anything the HAL sends.
			 */
			char rendered[(CECFrame::MAX_LENGTH * 2) + 4];
			size_t written = 0;

			for (size_t index = 0; (index < message.size()) && ((written + 2) < sizeof(rendered)); index++) {
				written += (size_t)snprintf(rendered + written, sizeof(rendered) - written, "%02x", message[index]);
			}

			rendered[written] = '\0';

			if (message.size() > (size_t)CECFrame::MAX_LENGTH) {
				snprintf(rendered + written, sizeof(rendered) - written, "...");
			}

			CCEC_LOG( LOG_DEBUG, "======== onMessageSent received. Result: %s, message length: %zu, message bytes: %s\r\n", cechal::toString(status).c_str(), message.size(), rendered);
		}
		catch(...) {
			/*
			 * The allocation-free fallback: the status as a PLAIN INTEGER and the length,
			 * with no `std::string`, no `toString()` and no hex rendering. It reports the
			 * two facts that identify the event without repeating the construction that
			 * failed, and `CCEC_LOG()` formats into a fixed stack buffer and allocates
			 * nothing, so it cannot raise in turn.
			 */
			CCEC_LOG( LOG_EXP, "======== onMessageSent received. Result: %d, message length: %zu; the descriptive form of this diagnostic could not be built and was dropped\r\n", (int)status, message.size());
		}

		return ::android::binder::Status::ok();
	}

private:
	/**
	 * @brief Guards @ref owner against a callback racing the owner's teardown
	 *
	 * Held for the whole of every callback that dereferences @ref owner, and by
	 * detach() while it nulls it. Those two facts together are what make detach()
	 * synchronous with respect to in-flight delivery rather than merely eventual.@n
	 * It is a lock of the listener's, deliberately distinct from the owner's instance
	 * lock: a callback must be able to complete while the owner's teardown holds that
	 * instance lock, which is exactly the situation close() and ~DriverAidlImpl() create.
	 * See the lock-order paragraph in the class documentation.
	 */
	Mutex ownerMutex;
	/**
	 * @brief The back-end that owns this listener and consumes its frames, or null
	 *
	 * A pointer rather than a reference precisely so that it can be nulled. Null means
	 * the owner has detached and this listener - which the HAL may still hold a strong
	 * reference to - must not touch it again. Read and written only under
	 * @ref ownerMutex.
	 */
	DriverAidlImpl *owner;
};

/**
 * @copydoc CCEC::DriverAidlImpl::DriverAidlImpl
 *
 * The member initializer list is the whole of it: the lifecycle state, the retained
 * legacy handle field, the queue's capacity and the null availability reason. Everything
 * else - the lock, the local address list and the two session proxies - is default
 * constructed, which for the proxies means null.
 */
DriverAidlImpl::DriverAidlImpl() : status(CLOSED), nativeHandle(0), rQueue(INCOMING_QUEUE_CAPACITY), availabilityReason(NULL)
{
	CCEC_LOG( LOG_DEBUG, "Creating DriverAidlImpl done\r\n");
}

/**
 * @copydoc CCEC::DriverAidlImpl::~DriverAidlImpl
 *
 * The catch names `Exception`, the CCEC base, which is what the legacy destructor
 * catches. The detach that follows sits outside the state test on purpose; the comment
 * on it enumerates the three paths that reach it.
 *
 * @see DriverAidlImpl::EventListener::detach()
 */
DriverAidlImpl::~DriverAidlImpl()
{
    {AutoLock lock_(mutex);
		if (status != CLOSED) {
			try{
                this->close();
	        }
	        catch(Exception &e)
	        {
                CCEC_LOG( LOG_EXP, "DriverAidlImpl: Caught Exception while calling ~DriverAidlImpl::close()\r\n");

            }
		}

		/*
		 * Unconditional, and the last thing this object does. It must run on all three
		 * paths that reach here, which is why it sits outside the state test above:
		 * close() threw and its exception was swallowed just now; close() returned
		 * silently or succeeded and already detached, making this the idempotent second
		 * call; or the instance was never opened but open() had already constructed a
		 * listener before failing. Leaving any of those with an attached listener is
		 * CWE-416 - the storage this back pointer names is about to cease to exist,
		 * while the HAL's own strong reference can keep the listener callable.
		 */
		if (eventListener != 0) {
			eventListener->detach();
			eventListener.clear();
		}
    }
}

/**
 * @copydoc CCEC::DriverAidlImpl::open
 *
 * The steps run in the legacy order and under the same lock: the state test, the proxy
 * test, the threadpool start, the listener construction, the HAL open, the status and
 * controller tests, and the state change. The `#if 0`'d legacy throw is carried here
 * verbatim rather than deleted, so this file and DriverImpl.cpp read alike at that point.
 * The `eventListener == 0` guard is what makes the per-session listener rule hold: it
 * finds a null pointer because every session-ending path released the previous one.
 *
 * @see DriverAidlImpl::close()
 * @see DriverImpl::open()
 */
void DriverAidlImpl::open(void) noexcept(false)
{
    {AutoLock lock_(mutex);
		if (status != CLOSED) {
			#if 0
				throw InvalidStateException();
			#else
				return;
			#endif
		}

		if (hdmiCecService == 0) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::open : no compatible AIDL service proxy is held\r\n");
			throw IOException();
		}

		/*
		 * IHdmiCecEventListener is declared oneway, so its callbacks arrive on a binder
		 * thread inside this process, and binder starts no threadpool by default -
		 * without this call no CEC message is ever delivered. startThreadPool() is
		 * idempotent and safe whether or not another component in this process has
		 * already started one, which is precisely why the maximum thread count is left
		 * alone: no back-end-local flag can detect a pool started elsewhere, and
		 * lowering an already-established maximum can abort the process. The library
		 * default governs the thread count and joinThreadPool() is never called, since
		 * it would not return.
		 */
		::android::ProcessState::self()->startThreadPool();

		if (eventListener == 0) {
			eventListener = new EventListener(*this);
		}

		::android::sp<cechal::IHdmiCecController> controller;
		/* Synchronous, with no client-side deadline available: measured, not bounded. */
		const int64_t openStartedMs = halCallStarted();
		::android::binder::Status txn = hdmiCecService->open(eventListener, &controller);

		warnIfHalCallSlow("IHdmiCec::open", openStartedMs);

		CCEC_LOG( LOG_DEBUG, "DriverAidlImpl:: call IHdmiCec::open DONE %s, controller %s\r\n", txn.toString8().string(), (controller == 0) ? "null" : "present");

		if (!txn.isOk() || (controller == 0)) {
			/*
			 * A failed open still handed the listener across the binder boundary, so
			 * the HAL may already hold a strong reference to it and is under no
			 * obligation to drop it - the "no further callbacks" guarantee attaches to
			 * a successful close, and there was no successful open to close. Detach
			 * before unwinding, so that a callback arriving on a session that never
			 * opened cannot reach this instance.
			 */
			if (eventListener != 0) {
				eventListener->detach();
				eventListener.clear();
			}

			throw IOException();
		}

		hdmiCecController = controller;
		status = OPENED;
    }
}

/**
 * @copydoc CCEC::DriverAidlImpl::close
 *
 * The six steps run in the legacy order, and each of them is observable: the state test,
 * the move to CLOSING, the sentinel offer, the HAL close, the listener detach and
 * release, and the move to CLOSED ahead of any raise. The sentinel is offered before the
 * HAL transaction, which is the order the legacy back-end has and is relied upon, and the
 * detach runs on both arms, so a failed close still leaves this side fully closed - no
 * controller held, no listener attached, state CLOSED - before the exception is raised.
 * Blocked item B2 concerns which HAL call step four makes; it does not affect the
 * ordering of the other five.
 *
 * @see DriverAidlImpl::open()
 * @see DriverImpl::close()
 */
void  DriverAidlImpl::close(void) noexcept(false)
{

    {AutoLock lock_(mutex);
		if (status != OPENED) {
			#if 0
				throw InvalidStateException();
			#else
				return;
			#endif
		}
		status = CLOSING;

		/*
		 * The sentinel is offered under queueProducerMutex, because close() is a
		 * producer on this queue and not merely its terminator. The receive path
		 * establishes that there is room before it parts with ownership of a frame, and
		 * an occupancy observation is only stable while every producer that can raise
		 * occupancy is serialized against it - offering here without the producer lock
		 * would let this sentinel land in the gap between that check and its offer, and
		 * `EventQueue::offer()` would then discard the frame silently while the receive
		 * path reported it accepted, leaking it. The lock is this class's own, held for
		 * one offer with no IPC under it, and it is taken inside the instance lock, which
		 * is the only nesting order that exists: offerReceivedFrame() takes
		 * queueProducerMutex alone and never the instance lock, so no inversion is
		 * possible. The offer statement itself matches the legacy line.
		 *
		 * @see DriverAidlImpl::offerReceivedFrame()
		 */
		/* Use NULL as sentinel */
		{AutoLock lock_(queueProducerMutex);
			rQueue.offer(0);
		}

		bool closed = false;
		::android::binder::Status txn = ::android::binder::Status::fromStatusT(::android::DEAD_OBJECT);

		if (hdmiCecService != 0) {
			/* B2: candidate mapping for the legacy HdmiCecClose(), pending confirmation. */
			/* Synchronous, with no client-side deadline available: measured, not bounded. */
			const int64_t closeStartedMs = halCallStarted();

			txn = hdmiCecService->close(hdmiCecController, &closed);

			warnIfHalCallSlow("IHdmiCec::close", closeStartedMs);
		}
		else {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::close : no AIDL service proxy is held\r\n");
		}

		hdmiCecController.clear();

		/*
		 * Detach before the failure check, so that both arms detach. The placement is
		 * the whole of the CWE-416 guarantee here: a close that reported failure leaves
		 * the HAL entitled to keep calling the listener, because the AIDL guarantee of
		 * no further callbacks attaches to a successful close, and the failure arm below
		 * leaves by way of an exception that ~DriverAidlImpl() swallows. Detaching only
		 * on success would leave exactly that path holding a listener with a live back
		 * pointer into an object about to be destroyed.
		 *
		 * detach() returns only once any in-flight callback has finished, and it is
		 * called after the sentinel offer above rather than before it, so the queue
		 * lock is never held while the listener lock is being waited on. See the
		 * lock-order paragraph on EventListener.
		 */
		if (eventListener != 0) {
			eventListener->detach();
			eventListener.clear();
		}

		CCEC_LOG( LOG_DEBUG, "DriverAidlImpl:: call IHdmiCec::close DONE %s, result %s\r\n", txn.toString8().string(), closed ? "true" : "false");

		if (!txn.isOk() || !closed) {
            status = CLOSED;
			throw IOException();
		}

		status = CLOSED;
    }
}

/**
 * @copydoc CCEC::DriverAidlImpl::read
 *
 * A copy of DriverImpl::read(), class name aside, with one departure that the flush loop's
 * own comment argues in full: the same entry guard, the same re-check under the lock when
 * the queue yields nothing, and the same flush-then-raise on the NULL close sentinel - but
 * every pointer the flush dequeues is null checked, not only the first poll, because more
 * than one close sentinel can be present and dereferencing the second would fault on the
 * Bus reader thread. There is no AIDL call in this method at all. Copying rather than
 * re-deriving is the specification, because any other divergence here would be a
 * divergence in the Bus reader's behaviour.
 *
 * REGISTERED DEPARTURE FROM A DIRECTIVE, STATED AT METHOD LEVEL SO IT IS NOT ONLY VISIBLE
 * TO A READER WHO REACHES THE FLUSH LOOP. AAP section 0.3.3 specifies this method as
 * byte-identical to `ccec/src/DriverImpl.cpp:156-192`, and the null check inside the flush
 * loop is a deliberate departure from that directive - the one place this method is not the
 * copy the directive asks for.
 *
 * WHAT THE DIRECTIVE WOULD HAVE REQUIRED, AND WHY IT IS NOT DONE. The legacy flush arm -
 * `ccec/src/DriverImpl.cpp:179-183` - dequeues into `inFrame` and immediately evaluates
 * `frame = *inFrame` with no null test, while the only entries that reach it are received
 * frames and close()'s NULL sentinels. close() offers a sentinel on each transition out of
 * OPENED, so a stop/reopen/close, or a close racing a second close, leaves two: the checked
 * poll at the top of the loop consumes the first and the flush then meets the second and
 * dereferences NULL. That is an unfixed defect in the legacy back-end, and it is out of this
 * migration's scope for a reason that is not discretionary - `ccec/src/DriverImpl.hpp` and
 * `ccec/src/DriverImpl.cpp` are reference-only contracts here and are not modified by this
 * work. Reproducing the defect faithfully on the AIDL path would put a null dereference on
 * the Bus reader thread, the one thread in the middleware whose death silently ends all CEC
 * reception, so the copy is departed from rather than the fault reproduced.
 *
 * IT IS NOT AN OBSERVABLE BEHAVIOUR CHANGE, which is why it is registered as a departure
 * from a directive and not as a fourth observable difference. The only entry whose handling
 * this check can alter is a NULL one, and the previous handling of a NULL one was undefined
 * behaviour rather than a defined behaviour a caller could have depended on. Every non-NULL
 * entry takes exactly the path it took before - copied into @p frame, then released - the
 * method still raises InvalidStateException after draining, and a dropped second sentinel
 * has nothing left to signal because the loop has already established that the driver is no
 * longer OPENED. The flush loop's own comment carries that argument in full.
 *
 * The departure is recorded here for the specification owner alongside the two registered
 * observable differences, on getLogicalAddress() and write(), so that every place this
 * back-end is not what the plan literally specifies is stated on the method that carries it.
 * offerReceivedFrame() carried a third until its difference was closed rather than registered -
 * the incoming queue is sized one entry larger than the legacy one, so its reserved slot costs
 * no receive depth - and it is named here only for a reader arriving from an older revision of
 * this comment.
 *
 * THIS DEPARTURE IS REACHABLE WITHOUT ANY HAL MISBEHAVIOUR, which distinguishes it from the
 * other two: those need the HAL to report an address or a status its own contract forbids,
 * whereas close() offers a sentinel on each transition out of OPENED, so an ordinary stop,
 * reopen and close - or a close racing a second close - is enough to leave two of them. What
 * the check changes is nonetheless not a caller-visible outcome: the only entry whose handling
 * differs is a NULL one, and its previous handling was a dereference, which is undefined
 * behaviour rather than a behaviour a caller could have depended on.
 *
 * @see DriverAidlImpl::close() - the producer of the NULL sentinels this method drains
 * @see DriverImpl::read()
 */
void  DriverAidlImpl::read(CECFrame &frame)  noexcept(false)
{
    {AutoLock lock_(mutex);
		if (status != OPENED) {
			throw InvalidStateException();
		}
    }

    CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::Read()\r\n");

    bool backToPoll = false;
	do {
		backToPoll = false;

		CECFrame * inFrame = rQueue.poll();

		if (inFrame != 0) {
			frame = *inFrame;
			delete inFrame;
		}
		else {AutoLock lock_(mutex);

			if (status != OPENED) {
				/*
				 * Flush and return. EVERY dequeued pointer is null checked, including
				 * the ones this loop takes, because a NULL entry in this queue is not a
				 * malformed frame - it is close()'s wake-the-reader sentinel, and more
				 * than one of them can be present. close() offers one on each
				 * transition out of OPENED, so a stop/reopen/close or a close racing a
				 * second close leaves two, the checked poll above consumes the first,
				 * and this loop then meets the second. Dereferencing it would fault on
				 * the Bus reader thread, which is the one thread in the middleware whose
				 * death silently ends all CEC reception.
				 *
				 * A skipped sentinel is DROPPED rather than acted on, which is correct
				 * and complete here: this loop has already established that the driver
				 * is no longer OPENED and it raises InvalidStateException below
				 * regardless of what it drained, so the second sentinel has nothing
				 * left to signal.
				 *
				 * THIS IS NOT AN OBSERVABLE BEHAVIOUR CHANGE and so it does not weaken
				 * the byte-for-byte parity this method otherwise keeps with
				 * DriverImpl::read(). The only entry this check can alter the handling
				 * of is a NULL one, and the previous handling of a NULL one was
				 * undefined behaviour - a null dereference - not a defined behaviour
				 * that some caller could have depended on. Every non-NULL entry takes
				 * exactly the path it took before: copied into `frame`, then released.
				 */
				while (rQueue.size() > 0) {
					inFrame = rQueue.poll();

					if (inFrame != 0) {
						frame = *inFrame;
						delete inFrame;
					}
				}
				throw InvalidStateException();
			}
			else {
				backToPoll = true;
			}
		}
    } while(backToPoll);
}

/**
 * @copydoc CCEC::DriverAidlImpl::writeAsync
 *
 * The two prelude statements below sit outside the lock, in that order, because that is
 * where DriverImpl::writeAsync() has them and the ordering is observable: an empty frame
 * raises out of printFrameDetails()'s header decode before the state guard is ever
 * reached. Reordering the prelude for tidiness would be an unregistered behaviour change.
 *
 * @see DriverAidlImpl::write() - the synchronous path every production caller reaches
 * @see DriverImpl::writeAsync()
 */
void  DriverAidlImpl::writeAsync(const CECFrame &frame)  noexcept(false)
{

	const uint8_t *buf = NULL;
	size_t length = 0;

	frame.getBuffer(&buf, &length);
	printFrameDetails(frame);

    {AutoLock lock_(mutex);
	if (status != OPENED) {
		throw InvalidStateException();
	}
		CCEC_LOG( LOG_EXP, "DriverAidlImpl::writeAsync is not supported on the AIDL back-end; asynchronous transmit is not migrated and is deliberately not emulated. Declining a frame of %zu bytes.\r\n", length);

		throw OperationNotSupportedException();
    }
}


/*
 * Only 1 write is allowed at a time. Queue the write request and wait for response.
 */
/**
 * @copydoc CCEC::DriverAidlImpl::write
 *
 * The statement order is DriverImpl::write()'s: the prelude outside the lock, then the
 * state guard, then the length check, then the transmit with the lock still held, then
 * the status translation. Each arm of that translation reproduces one arm of the legacy
 * mapping:
 * - a non-ok binder status - transport failure, `EX_ILLEGAL_STATE`, dead binder - is an
 *   IOException, as `err != HDMI_CEC_IO_SUCCESS` is;
 * - `BUSY`, meaning arbitration failed after two attempts and nothing was sent, is an
 *   IOException, as the legacy send-failed family is;
 * - a directed message with `ACK_STATE_1` was not acknowledged and raises
 *   CECNoAckException;
 * - a broadcast with `ACK_STATE_0` raises CECNoAckException only on the CEC CTS 9-3-3
 *   arm - a rejected REPORT_PHYSICAL_ADDRESS - so that the caller retries, and returns
 *   normally for every other opcode, which is what the legacy implementation does;
 * - a directed `ACK_STATE_0` and a broadcast `ACK_STATE_1` both mean success and return
 *   normally.
 *
 * The destination nibble is read from `frame.at(0) & 0x0F`, the same expression the
 * legacy implementation uses. The translation is exhaustive over the enum and its default
 * arm fails: `sendMessage()`'s result is an `int32_t` read out of a parcel, so a value
 * outside the three documented enumerators can arrive, and it is logged by number and
 * raises IOException rather than being reported to the caller as a completed transmit.
 *
 * REGISTERED OBSERVABLE DIFFERENCE FROM THE LEGACY BACK-END, AND NOT ONE OF THE THREE THE
 * PLAN AUTHORIZES - the default arm above, and only it. Every documented arm is the legacy
 * mapping unchanged; what has no legacy counterpart is the treatment of a status that is
 * documented by neither side. DriverImpl::write() - `ccec/src/DriverImpl.cpp:265-274` -
 * tests the HAL's result against a closed set of five failure values and takes no action at
 * all on anything else, so an unrecognised status falls through its guards and the transmit
 * RETURNS NORMALLY, which the caller reads as delivered. This back-end raises IOException
 * for the same case. A middleware caller can therefore see a failure where the legacy path
 * reported success, and that is registered rather than presented as parity.
 *
 * IT IS REACHABLE ONLY ON A HAL CONTRACT VIOLATION. `SendMessageStatus` has exactly three
 * enumerators - `ACK_STATE_0`, `ACK_STATE_1` and `BUSY` - and all three have their own arm
 * above. A conformant HAL cannot reach the default arm, so no behaviour a conformant
 * platform exhibits differs between the two back-ends. It is nonetheless reachable in
 * practice rather than theoretically: the generated proxy fills `sendResult` from an
 * `int32_t` read out of the parcel, and nothing on the wire constrains what an
 * out-of-process HAL puts there.
 *
 * THE REJECTION STAYS, AND THE DIRECTION OF THE ERROR IS WHY. Reporting an unknown status as
 * a completed transmit is the one wrong answer a caller cannot recover from: a caller told
 * the frame reached the bus does not retry, so a suppressed transmit would look - from
 * Bus's writer thread, through Connection, up to the plugins - exactly like a delivered one,
 * with no diagnostic anywhere and the CEC exchange simply not happening. IOException is the
 * category the legacy back-end already uses for a transmit that did not complete, so callers
 * handle it today, and the numeric value the HAL reported is preserved in the LOG_EXP line.
 * Matching the legacy fall-through would reproduce a defect on a transport where it is
 * reachable, which is not what behaviour preservation is for.
 *
 * AAP section 0.8.2.1 enumerates exactly three authorized observable differences - this
 * method's 16-byte frame limit, writeAsync()'s OperationNotSupportedException and
 * addLogicalAddress()'s coarser failure category - and this is not among them. It is
 * recorded here in that section's terms rather than silently adopted; admitting it to that
 * list, or requiring the legacy fall-through, is the specification owner's decision.
 *
 * @see DriverAidlImpl::poll()
 * @see DriverImpl::write()
 */
void  DriverAidlImpl::write(const CECFrame &frame)  noexcept(false)
{

	const uint8_t *buf = NULL;
	size_t length = 0;

	frame.getBuffer(&buf, &length);
	printFrameDetails(frame);

    {AutoLock lock_(mutex);
	if (status != OPENED) {
		throw InvalidStateException();
	}
		if (length > AIDL_MAX_MESSAGE_LENGTH) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::write frame of %zu bytes exceeds the AIDL sendMessage limit of %zu bytes; refusing to truncate\r\n", length, AIDL_MAX_MESSAGE_LENGTH);
			throw IOException();
		}
		if (hdmiCecController == 0) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::write : no AIDL controller session is held\r\n");
			throw IOException();
		}
		cechal::SendMessageStatus sendResult = cechal::SendMessageStatus::BUSY;
		CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::write to call IHdmiCecController::sendMessage\r\n");

		/* Synchronous, with no client-side deadline available: measured, not bounded. */
		const int64_t sendStartedMs = halCallStarted();

		::android::binder::Status txn = hdmiCecController->sendMessage(std::vector<uint8_t>(buf, buf + length), &sendResult);

		warnIfHalCallSlow("IHdmiCecController::sendMessage", sendStartedMs);

		CCEC_LOG( LOG_DEBUG, ">>>>>>> >>>>> >>>> >> >> >\r\n");

		dump_buffer((unsigned char*)buf,length);

		CCEC_LOG(LOG_DEBUG, "==========================\r\n");

		CCEC_LOG( LOG_DEBUG, "DriverAidlImpl:: call IHdmiCecController::sendMessage DONE %s, result %s\r\n", txn.toString8().string(), cechal::toString(sendResult).c_str());

		if (!txn.isOk()) {
			throw IOException();
		}

		/*
		 * EXHAUSTIVE OVER THE ENUM, WITH A FAILING DEFAULT, and that shape is the point
		 * rather than tidiness. `sendResult` is filled by the generated proxy from an
		 * `int32_t` read out of the parcel, so an out-of-process HAL can put ANY 32-bit
		 * value in it - the three documented enumerators do not constrain what arrives.
		 * A chain of positive tests that fell through to the success path would therefore
		 * report an unknown or hostile value to the caller as a COMPLETED TRANSMIT, and a
		 * caller told the frame reached the bus does not retry: a suppressed transmit
		 * would look, from the plugin upwards, exactly like a delivered one.
		 *
		 * Every documented arm below is the legacy mapping unchanged, including the
		 * INVERTED broadcast sense, and the destination nibble is still read from
		 * `frame.at(0) & 0x0F` inside the arms that need it rather than before the switch -
		 * so BUSY is still decided without touching the frame, exactly as it was.
		 */
		switch (sendResult) {
			case cechal::SendMessageStatus::BUSY:
				/* Arbitration failed after two attempts and the message was not sent, which
				   is the legacy send-failed family and therefore an IOException. */
				throw IOException();

			case cechal::SendMessageStatus::ACK_STATE_1:
				/* Directed: not acknowledged by the addressed follower. Broadcast: sent and
				   not rejected, which is success - the inverted sense. */
				if ((frame.at(0) & 0x0F) != 0x0F) {
					throw CECNoAckException();
				}
				break;

			case cechal::SendMessageStatus::ACK_STATE_0:
				/* CEC CTS 9-3-3 -Ensure that the DUT will accept a negatively for broadcat report physical address msg and retry atleast once */
				if (((frame.at(0) & 0x0F) == 0x0F) && (length > 1) && ((frame.at(1) & 0xFF) == REPORT_PHYSICAL_ADDRESS )) {
					throw CECNoAckException();
				}
				/* Directed ACK_STATE_0 is an acknowledgement, and a broadcast ACK_STATE_0 on
				   any other opcode returns normally, which is what the legacy back-end does. */
				break;

			default:
				/*
				 * ONLY THE NUMERIC VALUE IS LOGGED, for the same reason the address
				 * validation logs only a number: the value is HAL-controlled and must
				 * never reach the diagnostic as text or as a format string.
				 */
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::write : the HAL reported send status %d, which is not a documented SendMessageStatus value; the transmit is treated as FAILED rather than assumed successful\r\n", (int)sendResult);
				throw IOException();
		}
    }

    CCEC_LOG( LOG_DEBUG, "Send Completed\r\n");
}

/**
 * @copydoc CCEC::DriverAidlImpl::getLogicalAddress
 *
 * `devType` is echoed into the log so the two back-ends' traces line up, and is used for
 * nothing else. The array shape the AIDL call requires is confined to the local vector
 * below, so nothing plural escapes this method. Each of the five ways the return value
 * can be zero writes its own log line, which is where the distinction the return value
 * cannot carry is recorded; one of them is a first entry outside the AIDL contract range
 * 0x0..0xE, which is rejected on the raw `int32_t` rather than converted.
 *
 * REGISTERED OBSERVABLE DIFFERENCE FROM THE LEGACY BACK-END, AND NOT ONE OF THE THREE THE
 * PLAN AUTHORIZES. That rejection has no legacy counterpart. DriverImpl::getLogicalAddress()
 * - `ccec/src/DriverImpl.cpp:290-301` - calls `HdmiCecGetLogicalAddress()` at :296, ignores
 * its return value and returns whatever the HAL left in the local, so a legacy HAL that
 * reports 0x1F or -1 hands that value to the caller. This back-end returns 0 for it instead.
 * A middleware caller can therefore see a different answer for the same misbehaving HAL, and
 * that is registered rather than presented as parity.
 *
 * IT IS REACHABLE ONLY ON A HAL CONTRACT VIOLATION. `IHdmiCec.getLogicalAddresses()` returns
 * addresses drawn from the CEC address space, and `IHdmiCecController.addLogicalAddresses()`
 * documents `0x0..0xE` for it - see HAL_LOGICAL_ADDRESS_MAX for the range and for why the
 * raw parcel value is what gets checked. A conformant HAL never produces a value this arm can
 * reject, so no behaviour a conformant platform exhibits differs between the two back-ends.
 *
 * ZERO IS NOT A NEW SENTINEL, WHICH IS WHY THE REJECTION COSTS NOTHING A CALLER RELIED ON.
 * Zero is the existing "no address" signal on both back-ends: the legacy implementation
 * initializes its local to zero and returns it whenever the HAL writes nothing, and
 * LibCCEC::getLogicalAddress() - `ccec/src/LibCCEC.cpp:162-168` - turns a zero return into
 * InvalidStateException. So an out-of-contract address is reported through the one channel
 * every caller above this method already handles, and the value the HAL actually named is
 * preserved in the LOG_EXP line rather than lost.
 *
 * THE VALIDATION STAYS. Passing the value through instead would hand `LogicalAddress`'s
 * narrowing constructor an integer it was never meant to see, and the result is not a
 * detectable error but a plausible wrong address - 256 arrives as 0x0 and 271 as 0xF - which
 * then propagates into frame headers and into isValidLogicalAddress() as if the HAL had
 * reported it. Reporting no address is both correct and diagnosable; silently substituting a
 * different device's address is neither.
 *
 * AAP section 0.8.2.1 enumerates exactly three authorized observable differences - write()'s
 * 16-byte frame limit, writeAsync()'s OperationNotSupportedException and
 * addLogicalAddress()'s coarser failure category - and this is not among them. It is
 * recorded here in that section's terms rather than silently adopted; admitting it to that
 * list, or requiring the value be passed through, is the specification owner's decision.
 *
 * @see DriverAidlImpl::isValidLogicalAddress()
 * @see HAL_LOGICAL_ADDRESS_MAX - the contract range and why the raw value is checked
 * @see DriverImpl::getLogicalAddress()
 */
int DriverAidlImpl::getLogicalAddress(int devType)
{
    {AutoLock lock_(mutex);
	int logicalAddress = 0;
	CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::getLogicalAddress called for devType : %d \r\n", devType);

	std::vector<int32_t> halAddresses;

	if (hdmiCecService == 0) {
		CCEC_LOG( LOG_EXP, "DriverAidlImpl::getLogicalAddress : no AIDL service proxy is held; reporting no address\r\n");
	}
	else {
		/* Synchronous, with no client-side deadline available: measured, not bounded. */
		const int64_t getStartedMs = halCallStarted();

		::android::binder::Status txn = hdmiCecService->getLogicalAddresses(&halAddresses);

		warnIfHalCallSlow("IHdmiCec::getLogicalAddresses", getStartedMs);

		if (!txn.isOk()) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::getLogicalAddress : IHdmiCec::getLogicalAddresses failed [%s]; reporting no address\r\n", txn.toString8().string());
		}
		else if (halAddresses.empty()) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl::getLogicalAddress : the HAL holds no logical addresses; reporting no address\r\n");
		}
		else {
			/*
			 * The RAW value, taken before any conversion, because that is the only point
			 * at which the HAL's actual answer is still visible. See
			 * HAL_LOGICAL_ADDRESS_MAX for why validating after a conversion would accept
			 * values the HAL never reported.
			 */
			const int32_t rawAddress = halAddresses[0];

			if (halAddresses.size() > 1) {
				CCEC_LOG( LOG_INFO, "DriverAidlImpl::getLogicalAddress : the HAL reports %zu logical addresses; operating on entry 0 [%d]\r\n", halAddresses.size(), (int)rawAddress);
			}

			if ((rawAddress < HAL_LOGICAL_ADDRESS_MIN) || (rawAddress > HAL_LOGICAL_ADDRESS_MAX)) {
				/*
				 * ONLY THE NUMERIC VALUE IS LOGGED. The rejected value is
				 * HAL-controlled, so it is rendered through a `%d` conversion of an
				 * integer and never as a format string or as text the HAL supplied -
				 * the diagnostic cannot be turned into a formatting primitive by what it
				 * reports.
				 */
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::getLogicalAddress : the HAL reported logical address %d, which is outside the contract range %d..%d; reporting no address\r\n", (int)rawAddress, (int)HAL_LOGICAL_ADDRESS_MIN, (int)HAL_LOGICAL_ADDRESS_MAX);
			}
			else {
				logicalAddress = (int)rawAddress;
			}
		}
	}

	CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::getLogicalAddress got logical Address : %d \r\n", logicalAddress);
	return logicalAddress;
    }
}

/**
 * @copydoc CCEC::DriverAidlImpl::getPhysicalAddress
 *
 * Blocked item B1. This body is the whole of the block: it logs, leaves the out
 * parameter untouched and returns. The legacy counterpart it stands in for is
 * `HdmiCecGetPhysicalAddress()`, and what is awaited is the device-settings HAL header
 * declaring the mandated EDID-byte read, owed by whoever owns the device-settings HAL
 * contract. Every alternative route is closed, and each for its own reason:
 * - reconstructing the declaration from the plugin test mocks is forbidden, and a mock
 *   is usage;
 * - the AIDL HDMI-output controller EDID event is forbidden outright;
 * - substituting any CEC HAL call is forbidden;
 * - acquiring a legacy CEC handle lazily is forbidden as such a substitution and is
 *   independently unsafe, because `HdmiCecOpen()` performs logical-address discovery for
 *   source devices - that is CEC polling on the wire, so it would put a second CEC
 *   controller on the bus while the AIDL HAL is the active controller, and the legacy
 *   header additionally warns that the API is not thread safe and requires a matching
 *   close.
 *
 * Returning without writing is the minimum-harm interim behaviour rather than a
 * fabricated value: it is the same observable outcome the legacy path already produces
 * when its own call fails, since the legacy implementation ignores the return value and
 * writes nothing on failure. Both plugin call sites already wrap the call and tolerate
 * not obtaining an address.
 *
 * @see DriverImpl::getPhysicalAddress() - the legacy implementation, unchanged
 */
void DriverAidlImpl::getPhysicalAddress(unsigned int *physicalAddress)
{
    {AutoLock lock_(mutex);
        CCEC_LOG( LOG_EXP, "DriverAidlImpl::getPhysicalAddress : BLOCKED ITEM B1 - the device settings HAL contract for the EDID byte read was not supplied, so the physical address is unavailable on the AIDL back-end. The caller's value is left untouched.\r\n");

        (void)physicalAddress;   /* deliberately not written - see B1 above */

        return ;
    }
}


/**
 * @copydoc CCEC::DriverAidlImpl::removeLogicalAddress
 *
 * DriverImpl::removeLogicalAddress()'s shape is the specification and is reproduced
 * rather than improved: the state guard, then the local removal, then the HAL call whose
 * result the legacy implementation discards. Both a false result and a non-ok binder
 * status are therefore logged and otherwise ignored - raising where the legacy back-end
 * returns silently would be an unregistered behaviour change - and the local removal
 * precedes the call and is not rolled back.
 *
 * @see DriverAidlImpl::addLogicalAddress()
 * @see DriverImpl::removeLogicalAddress()
 */
void DriverAidlImpl::removeLogicalAddress(const LogicalAddress &source)
{
    {AutoLock lock_(mutex);
		if (status != OPENED) {
			throw InvalidStateException();
		}

		logicalAddresses.remove(source);

		if (hdmiCecController == 0) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::removeLogicalAddress : no AIDL controller session is held; ignored, matching the legacy back-end\r\n");
		}
		else {
			bool removed = false;
			/* Synchronous, with no client-side deadline available: measured, not bounded. */
			const int64_t removeStartedMs = halCallStarted();
			::android::binder::Status txn = hdmiCecController->removeLogicalAddresses(std::vector<int32_t>{ source.toInt() }, &removed);

			warnIfHalCallSlow("IHdmiCecController::removeLogicalAddresses", removeStartedMs);

			if (!txn.isOk()) {
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::removeLogicalAddress : IHdmiCecController::removeLogicalAddresses failed [%s]; ignored, matching the legacy back-end\r\n", txn.toString8().string());
			}
			else if (!removed) {
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::removeLogicalAddress : the HAL declined to remove logical address %d; ignored, matching the legacy back-end\r\n", source.toInt());
			}
		}
    }
}

/**
 * @copydoc CCEC::DriverAidlImpl::addLogicalAddress
 *
 * The state guard, the local bookkeeping and the return-outside-the-lock shape all match
 * DriverImpl::addLogicalAddress(), and the one-element vector is built inline so no
 * plural form escapes this method.
 *
 * @see DriverAidlImpl::removeLogicalAddress()
 * @see DriverImpl::addLogicalAddress()
 */
bool DriverAidlImpl::addLogicalAddress(const LogicalAddress &source)
{
    {AutoLock lock_(mutex);

		if (status != OPENED) {
			throw InvalidStateException();
		}

		if (hdmiCecController == 0) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : no AIDL controller session is held\r\n");
			throw IOException();
		}

		bool added = false;
		/* Synchronous, with no client-side deadline available: measured, not bounded. */
		const int64_t addStartedMs = halCallStarted();
		::android::binder::Status txn = hdmiCecController->addLogicalAddresses(std::vector<int32_t>{ source.toInt() }, &added);

		warnIfHalCallSlow("IHdmiCecController::addLogicalAddresses", addStartedMs);

		if (!txn.isOk()) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : IHdmiCecController::addLogicalAddresses failed [%s]\r\n", txn.toString8().string());
			throw IOException();
		}
		else if (!added) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : the HAL declined logical address %d\r\n", source.toInt());
			throw AddressNotAvailableException();
		}
		else {
			logicalAddresses.push_back(source);
		}
    }

    return true;
}

/**
 * @copydoc CCEC::DriverAidlImpl::isValidLogicalAddress
 *
 * Copied rather than re-derived because there is nothing here for the transport to
 * change, and because keeping it identical is what makes the two back-ends
 * indistinguishable to Connection, its only production caller.
 *
 * @see DriverAidlImpl::addLogicalAddress()
 * @see DriverImpl::isValidLogicalAddress()
 */
bool DriverAidlImpl::isValidLogicalAddress(const LogicalAddress & source) const
{
	AutoLock lock_(mutex);
		bool found = false;
		std::list<LogicalAddress>::const_iterator it;
		for (it = logicalAddresses.begin(); it != logicalAddresses.end(); it++) {
			if(*it == source) {
				found = true;
				break;
			}
		}
	return found;
}

/**
 * @copydoc CCEC::DriverAidlImpl::poll
 *
 * Consulting the HAL's `getState()` instead would create a second source of truth beside
 * the middleware's own state machine, which is why the ping is a one-byte transmit and
 * the outcome reaches the caller through write()'s exceptions.
 *
 * The trailing `#if 0` block is inert legacy code, carried verbatim so that this method
 * remains a character-for-character copy of its counterpart and the two files diff
 * cleanly. It is not new dead code and it is not a placeholder.
 *
 * @see DriverAidlImpl::write()
 * @see DriverImpl::poll()
 */
void DriverAidlImpl::poll(const LogicalAddress &from, const LogicalAddress &to)
				  noexcept(false)
{
	uint8_t firstByte = (((from.toInt() & 0x0F) << 4) | (to.toInt() & 0x0F));
	CCEC_LOG( LOG_DEBUG, "$$$$$$$$$$$$$$$$$$$$ POST POLL [%s] [%s]$$$$$$$$$$$$$$$$$$$$$\r\n", from.toString().c_str(), to.toString().c_str());

	{
		CECFrame frame;
		frame.append(firstByte);
		write(frame);
	}

#if 0
	{
		/* Send a Poll so indicate there is a device present */
		CECFrame *frame = new CECFrame();
		frame->append(firstByte);
		rQueue.offer(frame);
	}
#endif
}

/**
 * @copydoc CCEC::DriverAidlImpl::getIncomingQueue
 *
 * The guard is the load-bearing part of this accessor, not the return: it is what rejects
 * a receive callback arriving during or after a close, and the listener must reach the
 * queue through here rather than touching the member. The unlocked state read is the
 * pre-existing shape of DriverImpl::getIncomingQueue(). The race it carries exists today
 * between the vendor HAL's thread and a closing thread; here it is the same shape with a
 * binder thread in place of the HAL thread. Adding a lock would be an unregistered
 * behaviour change, so the read stays as it is.
 */
DriverAidlImpl::IncomingQueue & DriverAidlImpl::getIncomingQueue(void)
{
	if (status != OPENED) {
		throw InvalidStateException();
	}

	return rQueue;
}

/**
 * @copydoc CCEC::DriverAidlImpl::offerReceivedFrame
 *
 * Why the occupancy observation holds, stated in terms of who can raise it. Every producer
 * on this queue is serialized on queueProducerMutex - this method and close()'s sentinel
 * offer, which takes the same lock for exactly that statement - so while this lock is held
 * nothing can add an entry. Consumers are unaffected by the lock and keep removing, which
 * can only lower the occupancy, so "there is room" cannot turn false between the check and
 * the offer. Consumers being removal-only is not on its own enough to reach that
 * conclusion: close() is a producer, and a close exempted from this lock could land its
 * sentinel in precisely that gap, whereupon `EventQueue::offer()` would discard the
 * received frame silently while this method still reported it accepted - one leaked frame
 * per event.
 *
 * `osal/include/osal/EventQueue.hpp` is out of bounds for this migration, so the silent
 * discard it performs at capacity is worked around here rather than fixed there.
 *
 * RECEIVE DEPTH IS AT PARITY WITH THE LEGACY BACK-END, AND THE RESERVED SLOT IS WHY IT TAKES
 * READING. The legacy queue leaves `CCEC_OSAL::EventQueue`'s default in place -
 * `EventQueue(size_t cap = 32)` in `osal/include/osal/EventQueue.hpp` - and DriverImpl's
 * receive callback offers straight onto it, so received frames fill all 32 slots. This queue
 * is constructed with INCOMING_QUEUE_CAPACITY, which is 33, and this method refuses at one
 * below that. Received frames therefore fill @b 32 here as well: the same burst depth, the
 * same frame accepted at every occupancy, no caller-observable difference to register. The
 * thirty-third slot is not depth available to anyone - it exists so close()'s NULL sentinel
 * has somewhere to land that a received frame can never have taken.
 *
 * THE RESERVE IS NOT COSMETIC, and what its absence costs is why the queue is sized for it.
 * `EventQueue::offer()` returns void and, at capacity, discards its argument in a branch
 * whose body is an empty `/ * @TODO Throw Exception * /`. If the sentinel could meet a full
 * queue it would be dropped silently and reported nowhere; the Bus reader would stay blocked
 * in `EventQueue::poll()` with nothing left to wake it, and it is the one thread in the
 * middleware whose death silently ends all CEC reception. DriverImpl is exposed to exactly
 * that - it is an unfixed legacy liveness defect, not a behaviour this back-end is obliged to
 * reproduce - and the one thing that must not happen while fixing it is to pay for the fix in
 * receive depth, because that would trade a rare hang for a difference the caller meets under
 * ordinary load. Sizing the queue one larger is what avoids the trade.
 *
 * SO NOTHING IS REGISTERED AGAINST AAP SECTION 0.8.2.1 HERE. That section enumerates exactly
 * three authorized observable differences - write()'s 16-byte frame limit, writeAsync()'s
 * OperationNotSupportedException and addLogicalAddress()'s coarser failure category - and
 * closes the list. This method adds no fourth: at 33 slots with one reserved, the observable
 * receive behaviour is the legacy behaviour. An earlier revision of this back-end sized the
 * queue at 32 and reserved a slot out of it, which did add a fourth difference - first frame
 * lost at a burst depth of 31 against legacy's 32 - and the note is kept here because the
 * arithmetic is the only thing separating the two revisions, and a later change that lowers
 * the constant to 32 without removing the reserve reintroduces that difference silently.
 *
 * @see DriverAidlImpl::close() - the other producer, serialized on the same lock
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 * @see DriverAidlImpl::INCOMING_QUEUE_CAPACITY - the same effective-depth arithmetic stated
 *      from the constant's side, in `ccec/src/DriverAidlImpl.hpp`
 */
bool DriverAidlImpl::offerReceivedFrame(CECFrame *frame)
{
	/*
	 * The reserved-slot rule below subtracts one from the capacity, so a queue of fewer
	 * than two entries would leave the receive path with no usable slot at all. Asserted
	 * at compile time, at the one place the arithmetic is relied on, so that a future
	 * change to the constant cannot quietly produce a receive path that accepts nothing.
	 */
	static_assert(INCOMING_QUEUE_CAPACITY >= 2,
	              "INCOMING_QUEUE_CAPACITY must leave one slot for a received frame and one for close()'s sentinel");

	IncomingQueue &queue = getIncomingQueue();

	bool accepted = false;

    {AutoLock lock_(queueProducerMutex);
		/*
		 * One slot is reserved for close()'s sentinel, which is why the refusal point is
		 * one below the capacity rather than at it. close() offers NULL to wake a blocked
		 * Bus reader, under this same lock; if the receive path were allowed to fill the
		 * last slot, that offer would be silently discarded by EventQueue::offer() and the
		 * reader would stay asleep with nothing left to wake it. Stopping one entry early
		 * makes the sentinel undroppable, and it costs no receive depth: the capacity is
		 * sized at 33 precisely so that one below it is 32, which is what the legacy
		 * queue's OSAL default accepts. Lowering INCOMING_QUEUE_CAPACITY to 32 without
		 * also removing this check would turn a free guarantee into a caller-observable
		 * difference - see the constant's own note.
		 */
		const size_t occupancyBefore = queue.size();

		if (occupancyBefore >= (INCOMING_QUEUE_CAPACITY - 1)) {
			return false;
		}

		queue.offer(frame);

		/*
		 * The outcome is read back from the queue rather than assumed from the check
		 * above, because EventQueue::offer() returns void and reports nothing at all. The
		 * read-back is trusted in one direction only, and the asymmetry is deliberate:
		 *
		 * - a read-back below the capacity is consistent with the offer having gone in, and
		 *   is the ordinary outcome. It may sit below occupancyBefore, because a consumer
		 *   can take entries - including this very frame - between the offer and this read;
		 *   the Bus reader is normally blocked in EventQueue::poll() on an empty queue and
		 *   is woken by the offer itself, so "offered, taken, and back to the previous
		 *   occupancy" is the common interleaving, not a corner. Reporting a refusal there
		 *   would hand the caller a pointer the queue already owns and may already have
		 *   destroyed, turning a bounded leak into a double free;
		 * - a read-back at or above the capacity is inconsistent with acceptance and is
		 *   reported as a refusal. Producers are serialized here, so an accepted offer can
		 *   leave at most occupancyBefore + 1 entries, which the check above kept below the
		 *   capacity. Seeing the queue at capacity therefore means something outside this
		 *   lock filled it and this offer was discarded - the frame is still the caller's,
		 *   and saying so keeps the return value honest if a later change ever breaks that
		 *   assumption.
		 */
		const size_t occupancyAfter = queue.size();

		accepted = (occupancyAfter < INCOMING_QUEUE_CAPACITY);
    }

	return accepted;
}

/**
 * @copydoc CCEC::DriverAidlImpl::printFrameDetails
 *
 * A byte-for-byte copy of DriverImpl::printFrameDetails(), class name aside, and two
 * details of that copy are load bearing rather than incidental. The catch names
 * `Exception`, the CCEC base, so the `std::out_of_range` an empty frame's header decode
 * raises leaves this method - which is the mechanism by which write() and writeAsync()
 * raise out of their prelude, ahead of their state guard, on both back-ends alike. And
 * the log line terminates with a bare `\n` where every other log line in this file uses
 * `\r\n`; that is how the legacy line reads, and normalizing it would break the
 * character-for-character copy this method is required to be.
 *
 * @see DriverAidlImpl::write()
 * @see DriverImpl::printFrameDetails()
 */
void  DriverAidlImpl::printFrameDetails(const CECFrame &frame)  noexcept(false) {
	const uint8_t *buf = NULL;
	char strBuffer[50] = {0};
	size_t len = 0;
	const char *opname = "none";

	try{
		frame.getBuffer(&buf, &len);
		Header header(frame,HEADER_OFFSET);
		for (size_t i = 0; i < len; i++) {
			snprintf(strBuffer + strlen(strBuffer) , (sizeof(strBuffer) - strlen(strBuffer)) ,"%02X ",(uint8_t) *(buf + i));
		}
		if (frame.length() > OPCODE_OFFSET) {
			opname = GetOpName(OpCode(frame,OPCODE_OFFSET).opCode());
			CCEC_LOG( LOG_INFO, "%s to %s : opcode: %s :%s\n",header.from.toString().c_str(), header.to.toString().c_str(), opname, strBuffer);
		}
	}
	catch(Exception &e)
	{
		CCEC_LOG(LOG_EXP, "printFrameDetails caught %s \r\n",e.what());
	}
}

/**
 * @copydoc CCEC::DriverAidlImpl::defaultBinderProbe
 *
 * The instance is a function-local static initialized from the six file-local wrappers
 * above, so the six real kernel-facing operations are named in exactly one place.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
const DriverAidlImpl::BinderPreflightProbe &DriverAidlImpl::defaultBinderProbe(void)
{
	static const BinderPreflightProbe probe = {
		defaultOpenBinderNode,
		defaultIdentifyBinderDescriptor,
		defaultIdentifyBinderPath,
		defaultReadBinderProtocolVersion,
		defaultPingBinderContextManager,
		defaultCloseBinderNode,
	};

	return probe;
}

/**
 * @copydoc CCEC::DriverAidlImpl::expectedBinderProtocolVersion
 *
 * The `BINDER_CURRENT_PROTOCOL_VERSION` read sits under the CCEC_HAVE_BINDER_UAPI guard.
 * This is the one place in this file where the absence of the binder kernel ABI
 * definitions changes a returned value rather than only a log line.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
unsigned int DriverAidlImpl::expectedBinderProtocolVersion(void)
{
#if CCEC_HAVE_BINDER_UAPI
	return (unsigned int)BINDER_CURRENT_PROTOCOL_VERSION;
#else
	return 0;
#endif
}

/**
 * @copydoc CCEC::DriverAidlImpl::isBinderPreflightOk
 *
 * Eight decision points, in this order, and the order is part of the contract: the path is
 * empty; the node could not be opened; the object behind the descriptor could not be
 * identified; it is not a character device; it is not owned by root; its version could not
 * be read; the version differs from this build's; the context manager did not answer.
 * Each carries one record in the coverage branch manifest, so reordering them, merging two
 * of them, or making any of them conditional on a build macro invalidates that mapping.
 * That is why the CCEC_HAVE_BINDER_UAPI guard lives inside the default probe rather than
 * around this body: a build without the binder kernel ABI definitions still runs every arm
 * and still reaches the same verdict, false, by failing the version read with ENOSYS and
 * logging why.
 *
 * The abort hazard the first checks exist for is observed rather than inferred: on a
 * driverless host, merely reaching `ProcessState::self()` raises SIGABRT, which is why
 * nothing in this body may touch libbinder and every operation it performs goes through
 * the probe's plain syscalls instead. A missing context manager is the other hazard, and
 * it blocks rather than aborting, which is what the bounded handle-0 check answers.
 *
 * @see DriverAidlImpl::isServiceAvailable()
 * @see DriverAidlImpl::BinderPreflightProbe
 * @see DriverAidlImpl::defaultBinderProbe()
 * @see DriverAidlImpl::expectedBinderProtocolVersion()
 */
bool DriverAidlImpl::isBinderPreflightOk(const std::string &binderDriverPath,
                                         unsigned int contextManagerTimeoutMs,
                                         const BinderPreflightProbe &probe,
                                         int *retainedDescriptor,
                                         BinderNodeIdentity *retainedIdentity)
{
	/*
	 * The custody slot is cleared BEFORE any arm can return, so that "no descriptor was
	 * retained" and "a value left over from a previous call" are the same observation and
	 * no caller has to distinguish them. Everything below may therefore return early
	 * without touching it again.
	 */
	if (NULL != retainedDescriptor) {
		*retainedDescriptor = -1;
	}

	/* Decision point 1 of 8: the path itself. */
	if (binderDriverPath.empty()) {
		CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: no binder driver path was given; treating the AIDL HAL as absent\r\n");
		return false;
	}

	/* Decision point 2 of 8: the node exists and can be opened. */
	const int driverFd = probe.openNode(binderDriverPath.c_str(), O_RDWR | O_CLOEXEC);

	if (driverFd < 0) {
		/*
		 * errno is captured before anything else runs, because every call between the
		 * failed open and the report - CCEC_LOG included - is free to overwrite it.
		 *
		 * The two causes are reported separately, and they are not the same platform
		 * condition. ENOENT means this kernel carries no binder driver at all, which is
		 * the ordinary, expected, entirely healthy state of a legacy-only SOC and the
		 * exact case the fallback exists to serve. Any other errno means the node is
		 * there and this process could not use it - EACCES for a permission or policy
		 * problem, EMFILE or ENFILE for exhausted descriptors, ENODEV for a driver that
		 * unregistered - and those are misconfigurations an integrator has to act on.
		 * Collapsing them into one line would make a broken platform indistinguishable
		 * from a legacy-only one, so both arms name what happened, and the second carries
		 * the errno text as well as its number because nobody reads a build log with an
		 * errno table beside them.
		 */
		const int openErrno = errno;

		if (openErrno == ENOENT) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: no binder driver node exists at [%s]; this platform carries no binder transport, so the AIDL HAL is absent and the legacy back-end is the correct outcome\r\n", binderDriverPath.c_str());
		}
		else {
			CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] EXISTS but could not be opened, errno %d (%s); treating the AIDL HAL as absent. Unlike a missing node this is a platform fault rather than a legacy-only platform, and it needs an integrator's attention\r\n", binderDriverPath.c_str(), openErrno, strerror(openErrno));
		}

		return false;
	}

	/*
	 * Decision point 3 of 8: the object behind the descriptor can be identified at all.
	 *
	 * Asked of the DESCRIPTOR, never of the path. The answer then describes what this
	 * process already holds open, which is the only form of the question whose answer
	 * cannot be changed underneath it, and it is the reference the caller's re-check
	 * compares a fresh resolution of the same name against.
	 */
	BinderNodeIdentity identity;

	memset(&identity, 0, sizeof(identity));

	if (0 != probe.identifyDescriptor(driverFd, &identity)) {
		/* Captured first, and reported with its text, for the reason stated at decision point 2. */
		const int identifyErrno = errno;

		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] opened but its node could not be identified, errno %d (%s); treating the AIDL HAL as absent. Without an identity there is nothing to re-verify before libbinder opens the same name, so the check could not be made to mean anything\r\n", binderDriverPath.c_str(), identifyErrno, strerror(identifyErrno));
		probe.closeNode(driverFd);
		return false;
	}

	/*
	 * Decision point 4 of 8: it is a CHARACTER DEVICE.
	 *
	 * Every supported binder layout publishes one - a devtmpfs node under `/dev`, or a
	 * binderfs node reached directly or through a symlink - so anything else behind this
	 * name is not the binder driver however it is named. A regular file at that path is the
	 * shape a substitution takes when an unprivileged process wins a race to create it.
	 */
	if ((identity.mode & BINDER_NODE_MODE_TYPE_MASK) != BINDER_NODE_MODE_CHARACTER_DEVICE) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] is not a character device, mode 0%o; treating the AIDL HAL as absent. Every supported binder layout publishes a character device, so this node is not the binder driver and must not be handed to libbinder\r\n", binderDriverPath.c_str(), (unsigned int)(identity.mode & BINDER_NODE_MODE_TYPE_MASK));
		probe.closeNode(driverFd);
		return false;
	}

	/*
	 * Decision point 5 of 8: it is owned by ROOT.
	 *
	 * devtmpfs and binderfs both create the node as root. A node at this path owned by
	 * anyone else is one an unprivileged process was able to create or replace, and
	 * selecting the AIDL path against it would put the whole HAL boundary - every frame
	 * transmitted, every frame delivered - behind whatever registered itself there. Only
	 * the numeric owner is logged: nothing HAL-controlled or filesystem-controlled is
	 * rendered as text here.
	 */
	if (identity.uid != BINDER_NODE_REQUIRED_OWNER_UID) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] is owned by uid %u rather than %u; treating the AIDL HAL as absent. A binder node not owned by root is one an unprivileged process could have created, so it is refused rather than trusted\r\n", binderDriverPath.c_str(), identity.uid, (unsigned int)BINDER_NODE_REQUIRED_OWNER_UID);
		probe.closeNode(driverFd);
		return false;
	}

	/*
	 * AN OBSERVATION, AND DELIBERATELY NOT A DECISION POINT. It cannot change the verdict,
	 * nothing below reads it, and it is placed here - after the file type and the owner have
	 * been established - only so that what it describes is known to be a root-owned
	 * character device rather than an arbitrary node.
	 *
	 * WHAT IT REPORTS. A binder node writable beyond its owner means every process in that
	 * group, or every process on the box, can open the driver. That is worth a line in the
	 * log of the one component whose whole HAL boundary sits behind that driver, because on
	 * a platform that ALSO fails to authorize service registration it is the precondition of
	 * the impersonation recorded on isServiceAvailable() - and nowhere else does an
	 * integrator get told the two conditions coexist.
	 *
	 * WHY IT IS A LINE AND NOT A REFUSAL, WHICH IS THE WHOLE POINT AND MUST NOT BE
	 * "FIXED" LATER. A binder device node HAS to be openable by every client process that
	 * uses binder, so AOSP-derived platforms - and this port vendors AOSP
	 * `android-13.0.0_r74` - publish it broadly accessible BY DESIGN, and a binderfs
	 * deployment gets whatever mode binderfs assigns. Refusing a group- or world-writable
	 * node would therefore decline the AIDL path on conformant production platforms while
	 * PASSING in CI, whose guest node is root-owned with everything running as root: green
	 * here, migration silently defeated everywhere that matters. Restrictiveness is not a
	 * property this middleware can require, so it is not required - it is reported.
	 *
	 * WHAT DOES CARRY A VERDICT, so this reads as a boundary rather than as a shrug: the two
	 * checks immediately above (character device, owned by root), and the five-attribute
	 * identity comparison the caller makes immediately before the lookup, which is what
	 * catches this mode CHANGING inside the check-to-use window even though its value is not
	 * dictated here. Numeric only, in octal, as a mode is read.
	 */
	if (0u != (identity.mode & (BINDER_NODE_MODE_GROUP_WRITE | BINDER_NODE_MODE_WORLD_WRITE))) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] is writable beyond its owner, permission bits 0%o; continuing, because a binder node must be openable by every binder client and restrictive modes are NOT a property this middleware can require. On a platform that also leaves service registration unauthorized this is the precondition of HAL impersonation - see the service-authorization prerequisite on isServiceAvailable()\r\n", binderDriverPath.c_str(), (identity.mode & BINDER_NODE_MODE_PERMISSION_MASK));
	}

	/* Decision point 6 of 8: the node will tell us which protocol it speaks. */
	unsigned int protocolVersion = 0;

	if (0 != probe.readProtocolVersion(driverFd, &protocolVersion)) {
		/* Captured first, and reported with its text, for the reason stated at decision point 2. */
		const int ioctlErrno = errno;

		/*
		 * LOG_WARN rather than LOG_INFO, and that is the whole point of splitting the absent
		 * node from the broken one.  A node that does not exist is a legacy-only SOC behaving
		 * exactly as designed, which is why that arm alone stays at LOG_INFO.  Reaching here
		 * means the node existed and opened, and then refused the one ioctl every binder
		 * client issues before anything else.  No correctly provisioned platform does that:
		 * it is a mis-created device node, a driver that is not binder, or a kernel and a
		 * libbinder that disagree about the interface.  The fallback it causes is otherwise
		 * silent, so this line is the only place an integrator can find out.  It sits at the
		 * same level as its two siblings - the failed open above and the protocol mismatch
		 * below - because all three describe a platform fault rather than an absent HAL.
		 */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] opened but REFUSED to report its protocol version, errno %d (%s); treating the AIDL HAL as absent. Unlike a missing node this is a PLATFORM FAULT rather than a legacy-only platform: the node exists and opens, so it is either not a binder driver or was mis-created\r\n", binderDriverPath.c_str(), ioctlErrno, strerror(ioctlErrno));
		probe.closeNode(driverFd);
		return false;
	}

	/*
	 * Decision point 7 of 8: equality, not a minimum. libbinder enforces an exact match
	 * when it opens the driver, so a difference in either direction fails every open and
	 * is reported here as "AIDL absent" rather than being left for libbinder to abort on.
	 */
	if (protocolVersion != expectedBinderProtocolVersion()) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] speaks protocol %d but this build expects %d; treating the AIDL HAL as absent\r\n", binderDriverPath.c_str(), (int)protocolVersion, (int)expectedBinderProtocolVersion());
		probe.closeNode(driverFd);
		return false;
	}

	/*
	 * Clamp before probing. The parameter is an unsigned int, so a caller can name a value
	 * far larger than any plausible servicemanager start-up delay, and every millisecond of
	 * it would be time LibCCEC::init() spends blocked. Clamping here keeps the ceiling in
	 * one place; the probe's own slicing then bounds each individual wait whatever value
	 * arrives. Zero is left alone deliberately - it means "do not wait at all", which is
	 * how the negative arm is exercised.
	 */
	unsigned int effectiveTimeoutMs = contextManagerTimeoutMs;

	if (effectiveTimeoutMs > MAX_CONTEXT_MANAGER_TIMEOUT_MS) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: a context manager timeout of %u ms was requested, above the %u ms ceiling; using the ceiling so that initialization stays bounded\r\n", contextManagerTimeoutMs, (unsigned int)MAX_CONTEXT_MANAGER_TIMEOUT_MS);
		effectiveTimeoutMs = MAX_CONTEXT_MANAGER_TIMEOUT_MS;
	}

	/* Decision point 8 of 8: a context manager is registered and answers, under bound. */
	const bool contextManagerReachable = probe.pingContextManager(driverFd, effectiveTimeoutMs);

	if (!contextManagerReachable) {
		probe.closeNode(driverFd);
		return false;
	}

	CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: binder driver [%s] and its context manager are usable\r\n", binderDriverPath.c_str());

	/*
	 * CUSTODY, OR THE ORIGINAL RELEASE, and this is the only place the two differ.
	 *
	 * A caller that asked for custody keeps the validated descriptor open and receives the
	 * identity beside it, so that the node it re-resolves before the use can be compared
	 * with the node that was actually validated, and so that the inode cannot be recycled
	 * underneath the comparison while the descriptor is held. A caller that asked for
	 * nothing gets the behaviour this predicate had before custody existed: the descriptor
	 * is released here, and the process is left exactly as it was found.
	 */
	if (NULL != retainedDescriptor) {
		*retainedDescriptor = driverFd;

		if (NULL != retainedIdentity) {
			*retainedIdentity = identity;
		}
	}
	else {
		probe.closeNode(driverFd);
	}

	return true;
}

namespace {

/*
 * The three fallback-reason phrases, and the first two are a contract.
 *
 * They are declared here rather than at the top of the file because isServiceAvailable()
 * below is the only thing that ever assigns one, and the selection helper in
 * ccec/src/Driver.cpp is the only thing that ever prints one - so keeping them beside
 * their sole producer is what stops a fourth arm being added without a phrase, or a
 * phrase being reworded away from the arm it names.
 *
 * REASON_TRANSPORT_UNAVAILABLE and REASON_NO_COMPATIBLE_SERVICE are the exact texts the
 * selection helper emits, and they are transcribed word for word by
 * tests/L1Tests/run_coverage.sh and by tests/L2Tests/ccec/test_DualPathIntegration.cpp.
 * They are also deliberately worded so that neither contains the selected-path literal,
 * which is what keeps a grep for that literal at exactly one hit per process. Rewording
 * either breaks those consumers.
 *
 * REASON_QUERY_FAILED is the third arm. It covers the catch-all in
 * isServiceAvailable(): a query that neither succeeded nor cleanly declined is a
 * different platform condition from an absent transport and from an unusable service,
 * and reporting it as either of those would misdescribe it.
 */
const char *const REASON_TRANSPORT_UNAVAILABLE = "the binder transport is unavailable on this platform";
const char *const REASON_NO_COMPATIBLE_SERVICE = "the binder transport is reachable but no compatible service resolved";
const char *const REASON_QUERY_FAILED          = "the service query failed unexpectedly, so no usable service could be established";

/** @brief Longest interface hash rendered into a log line by sanitizedInterfaceHash(). */
const size_t MAX_LOGGED_HASH_LENGTH = 16;

/*
 * ONE BIT PER ATTRIBUTE OF BinderNodeIdentity, so that the re-verification's diagnostic can
 * say WHICH attribute changed between the check and the use as a single number rather than
 * as a sentence assembled at runtime.
 *
 * NAMED RATHER THAN WRITTEN AS LITERALS AT THE POINT OF USE, for the same reason the node
 * constants in the header are named: the bit order is a reading contract - it is printed in
 * the log line's own legend and transcribed by the contract suite - so it has to be
 * changeable in exactly one place. The values are plain integers and nothing derives them
 * from a struct layout, because a bit whose meaning depended on member order would silently
 * re-point the moment a member moved.
 */
/** @brief Divergence bit for BinderNodeIdentity::device, POSIX `st_dev`. */
const unsigned int NODE_IDENTITY_DIVERGED_DEVICE = 0x01u;
/** @brief Divergence bit for BinderNodeIdentity::inode, POSIX `st_ino`. */
const unsigned int NODE_IDENTITY_DIVERGED_INODE  = 0x02u;
/** @brief Divergence bit for BinderNodeIdentity::rdev, POSIX `st_rdev`. */
const unsigned int NODE_IDENTITY_DIVERGED_RDEV   = 0x04u;
/** @brief Divergence bit for BinderNodeIdentity::mode, POSIX `st_mode`. */
const unsigned int NODE_IDENTITY_DIVERGED_MODE   = 0x08u;
/** @brief Divergence bit for BinderNodeIdentity::uid, POSIX `st_uid`. */
const unsigned int NODE_IDENTITY_DIVERGED_UID    = 0x10u;

/**
 * @brief Holds the descriptor the preflight retained, and releases it on every exit path
 *
 * WHAT IT IS FOR, since "an RAII holder" says nothing about why one is needed here. The
 * preflight now hands its validated descriptor to isServiceAvailable(), which then has
 * SEVEN ways out - four declines, one success, one incompatible-service arm and one
 * catch-all - and the descriptor is a `/dev/binder` open, i.e. a live driver context that
 * nothing will reclaim until the process exits. Releasing it correctly on each of those
 * seven paths by hand is exactly the kind of accounting that survives review and then
 * fails on the eighth path somebody adds. Scope-bound release makes the property
 * structural instead: the window opens where this object is constructed and closes where
 * it goes out of scope, INCLUDING when an exception unwinds through it, which is the one
 * path no hand-written release can cover here because the catch-all sits outside it.
 *
 * IT IS NOT AN ABSTRACTION IN THE SENSE THE PLAN PROHIBITS. It adds no virtual dispatch,
 * no allocation, no ownership question and no indirection at the HAL boundary - it is the
 * same scope-bound-release idiom the CCEC sources already use for the instance lock
 * (`{AutoLock lock_(mutex);`), applied to a descriptor instead of a mutex, and it is
 * file-local so nothing outside this translation unit can see it.
 *
 * @warning Not copyable and not assignable, because two holders of one descriptor would
 *          close it twice. The copy operations are declared private and never defined.
 * @warning Holds the probe BY REFERENCE. Every use below binds it to a probe that
 *          outlives the holder - a caller's argument or defaultBinderProbe()'s static -
 *          which is the only correct lifetime and the one the single call site provides.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 * @see DriverAidlImpl::isServiceAvailable()
 */
class RetainedBinderNode {
public:
	/**
	 * @brief Takes an empty holder bound to the probe that will release the descriptor
	 *
	 * @param [in] probe - The operations the retained descriptor was opened through, and
	 *                     whose `closeNode` will release it. Must outlive this object.
	 *
	 * @post No descriptor is held. The holder becomes non-empty only when the preflight
	 *       writes into descriptorSlot().
	 */
	explicit RetainedBinderNode(const DriverAidlImpl::BinderPreflightProbe &probe)
		: probe(probe), descriptor(-1)
	{
	}

	/**
	 * @brief Releases the descriptor if one is still held
	 *
	 * @post Nothing is held. This runs on a normal return and on an exception unwinding
	 *       through the enclosing scope, which is what makes the custody window closed by
	 *       construction rather than by care.
	 * @warning Never throws: the probe's `closeNode` is required not to, exactly as the
	 *          real `::close` does not.
	 */
	~RetainedBinderNode()
	{
		release();
	}

	/**
	 * @brief The slot isBinderPreflightOk() writes its retained descriptor into
	 *
	 * @return int* - Address of the held descriptor. The preflight sets it to -1 on every
	 *                decline and to the validated descriptor on a positive verdict, so
	 *                after the call this holder is non-empty if and only if custody was
	 *                actually transferred.
	 *
	 * @pre Nothing is held yet, which is true for the single call site.
	 */
	int *descriptorSlot(void)
	{
		return &descriptor;
	}

	/**
	 * @brief Reports whether a descriptor is currently held
	 *
	 * @return bool - Whether custody is in force
	 * @retval true  - A validated descriptor is held and the node's inode is pinned.
	 * @retval false - Nothing is held, so no custody window is open.
	 */
	bool held(void) const
	{
		return (descriptor >= 0);
	}

	/**
	 * @brief Releases the descriptor early, before the holder goes out of scope
	 *
	 * Idempotent, so the destructor can call it unconditionally after an early release.
	 *
	 * @post Nothing is held.
	 * @warning Never throws.
	 */
	void release(void)
	{
		if (descriptor >= 0) {
			probe.closeNode(descriptor);
			descriptor = -1;
		}
	}

private:
	RetainedBinderNode(const RetainedBinderNode &);
	RetainedBinderNode &operator=(const RetainedBinderNode &);

	/** @brief The operations the descriptor was opened through, and is released through. */
	const DriverAidlImpl::BinderPreflightProbe &probe;
	/** @brief The held descriptor, or -1 when nothing is held. */
	int descriptor;
};

/**
 * @brief Reports whether a fresh resolution yielded the UNCHANGED validated node
 *
 * ALL FIVE CAPTURED ATTRIBUTES ARE ENFORCED - `device`, `inode`, `rdev`, `mode` AND `uid` -
 * and the question this answers is deliberately "has the node CHANGED", not "is the node
 * still acceptable". Those are different questions and only the first one closes a
 * check-to-use window.
 *
 * `device` and `inode` together identify one filesystem object uniquely, and `rdev` is the
 * driver's major/minor pair, so a REPLACEMENT node - newly created, bind-mounted over, or
 * reached through a re-pointed symlink - differs in at least one of the three. That much
 * catches substitution of the object.
 *
 * `mode` AND `uid` CATCH THE OTHER HALF OF THE SAME WINDOW, AND OMITTING THEM LEFT IT OPEN.
 * The preflight validates ownership and file type once, on the object it has open; libbinder
 * then resolves the same pathname for itself, and every client process on the platform opens
 * that node too. An attacker who cannot REPLACE the node can still `chmod` or `chown` THE
 * VERY SAME INODE inside that window - widening write access to the driver, or handing its
 * ownership to an unprivileged user - and a comparison of `(device, inode, rdev)` alone
 * reports that as "unchanged" because the object genuinely is the same object. The
 * preflight's own ownership and file-type checks cannot see it either: they ran before the
 * change. So the values are captured for a reason and are compared here, which makes the
 * validation this function guards a statement about the node AS IT IS AT THE MOMENT OF USE
 * rather than as it was some milliseconds earlier.
 *
 * WHY THIS IS SAFE AT ANY PLATFORM BASELINE, which is the objection an earlier version of
 * this comment raised against including them and got wrong. Comparing `mode` asserts only
 * that it did not CHANGE - it requires no particular value - so a platform whose binder node
 * is restrictive (0600) and a platform whose node is broadly accessible (0666, which
 * AOSP-derived layouts publish deliberately, because every binder client must be able to
 * open it) both pass unchanged. What fails is movement, in either direction, which is
 * precisely the event worth refusing. And refusing is nonfatal: the AIDL path is declined
 * and the legacy back-end is selected.
 *
 * @param [in] validated - The identity the preflight established, taken from the
 *                         descriptor this process holds open.
 * @param [in] observed  - The identity a fresh resolution of the same path just yielded,
 *                         either by re-resolving the pathname or by identifying a
 *                         freshly opened descriptor.
 *
 * @return bool - Whether the fresh resolution is the validated node, unchanged
 * @retval true  - Every one of the five attributes agrees; the check and the use are about
 *                 the same node in the same state.
 * @retval false - At least one diverged, so the name resolves elsewhere or the node was
 *                 re-permissioned or re-owned underneath the validation. WHICH attributes
 *                 diverged is reported in the log, numerically, not in the return value.
 *
 * @pre None.
 * @post No state changed.
 * @warning Never throws and performs no allocation. On a divergence it emits ONE
 *          diagnostic line carrying a divergence bitmask and both value tuples, and it
 *          logs NUMBERS ONLY - no HAL-supplied and no filesystem-supplied text is
 *          rendered, matching the discipline the address-range and transmit-status
 *          diagnostics already follow.
 * @warning The verdict is the caller's to act on. This function DECIDES NOTHING about the
 *          back-end: both call sites turn a false verdict into the same nonfatal decline.
 *
 * @see DriverAidlImpl::BinderNodeIdentity
 * @see reverifyBinderNodeBeforeLookup()
 * @see DriverAidlImpl::isServiceAvailable()
 */
bool binderNodeIdentitiesMatch(const DriverAidlImpl::BinderNodeIdentity &validated,
                               const DriverAidlImpl::BinderNodeIdentity &observed)
{
	const unsigned int divergence =
	        ((validated.device != observed.device) ? NODE_IDENTITY_DIVERGED_DEVICE : 0u) |
	        ((validated.inode  != observed.inode)  ? NODE_IDENTITY_DIVERGED_INODE  : 0u) |
	        ((validated.rdev   != observed.rdev)   ? NODE_IDENTITY_DIVERGED_RDEV   : 0u) |
	        ((validated.mode   != observed.mode)   ? NODE_IDENTITY_DIVERGED_MODE   : 0u) |
	        ((validated.uid    != observed.uid)    ? NODE_IDENTITY_DIVERGED_UID    : 0u);

	if (0u == divergence) {
		return true;
	}

	/*
	 * ONE LINE, EMITTED HERE RATHER THAN AT THE CALL SITES, so that both re-verification
	 * points inherit the detail without either of them growing a conditional of its own -
	 * the second is a disjunction whose other operand is a failed `fstat`, and splitting it
	 * to report a divergence would turn one documented decision into two.
	 *
	 * The mask says which attributes moved and the two tuples say what they moved from and
	 * to, so a reader diagnosing a fallback can distinguish a substituted node from a
	 * re-permissioned or re-owned one without a second run. Modes are octal because that
	 * is how a mode is read; everything else is decimal and unsigned, and nothing here is
	 * a string.
	 */
	CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder node identity CHANGED between the preflight and the lookup, divergence mask 0x%02x (bit0 device, bit1 inode, bit2 rdev, bit3 mode, bit4 uid); validated dev %llu ino %llu rdev %llu mode 0%o uid %u; observed dev %llu ino %llu rdev %llu mode 0%o uid %u\r\n",
	          divergence,
	          validated.device, validated.inode, validated.rdev, validated.mode, validated.uid,
	          observed.device, observed.inode, observed.rdev, observed.mode, observed.uid);

	return false;
}

/**
 * @brief Re-verifies the binder node and its context manager immediately before the lookup
 *
 * THE SINGLE STEP THAT MAKES THE CHECK AND THE USE ONE PATH, and it does two things that
 * are separate concerns only if the failure modes are read separately.
 *
 * FIRST, IDENTITY. The preflight validated a node; libbinder is about to resolve the same
 * PATHNAME independently, and on the pinned stack it aborts rather than returns if what it
 * finds is not a usable binder driver. So the name is resolved once more here and compared
 * against the identity that was validated. A mismatch means the window was lost and the
 * AIDL path is declined - nonfatally, which is the entire value of declining at all.@n
 * The comparison covers ALL FIVE captured attributes, so it catches both halves of the
 * window: a node SUBSTITUTED for another object, and the same object RE-PERMISSIONED or
 * RE-OWNED after the preflight's ownership and file-type checks had already run.
 * binderNodeIdentitiesMatch() records why, and reports which attributes moved.
 *
 * SECOND, LIVENESS UNDER A BOUND. `defaultServiceManager()` polls until binder handle 0
 * resolves, with no upper bound of its own, so the dominant stall mode for a correctly
 * provisioned driver is a wedged or dying `servicemanager`. The preflight's bounded ping
 * establishes liveness at check time; asking again HERE, under the same bound, is what
 * turns "the manager died between the preflight and the lookup" from an unbounded hang
 * inside libbinder into a decline. It bounds what is boundable and claims nothing about
 * what is not: once `getService` is entered, no client-side deadline exists on the pinned
 * libbinder, which is recorded as a platform prerequisite on isServiceAvailable().
 *
 * WHY THE PING GOES OVER A SECOND, FRESHLY OPENED DESCRIPTOR RATHER THAN THE RETAINED ONE,
 * which is the one non-obvious decision here and is forced by the driver rather than
 * chosen. The ping maps the driver's transaction buffer, and the binder driver permits
 * exactly ONE mapping per open descriptor for that descriptor's lifetime: unmapping
 * releases the address range but not the right, so a second mapping attempt on the same
 * descriptor is refused unconditionally. A re-ping over the retained descriptor would
 * therefore fail on every correctly provisioned platform and decline the AIDL path
 * everywhere - a check that always says no is not a check. The fresh descriptor is
 * identified with `fstat` and compared against the SAME validated identity before it is
 * used, so it inherits the whole of the guarantee: it is the same object, and the retained
 * descriptor is still held throughout, which is what keeps the inode from being recycled
 * underneath either comparison.
 *
 * @param [in] binderDriverPath        - The path the preflight validated, resolved again.
 * @param [in] contextManagerTimeoutMs - Bound on the second ping, in milliseconds. The
 *                                       same value the preflight used, clamped there and
 *                                       clamped again here so neither route can exceed
 *                                       the ceiling.
 * @param [in] probe                   - The operations to perform this with; the same
 *                                       probe the preflight used.
 * @param [in] validated               - The identity the preflight established.
 *
 * @return bool - Whether the lookup may proceed
 * @retval true  - The path still resolves to the validated node AND a context manager
 *                 answered over a fresh descriptor within the bound.
 * @retval false - The path no longer resolves, or resolves elsewhere, or the fresh
 *                 descriptor is not the validated node, or it could not be opened or
 *                 identified, or no context manager answered. Every one of these is
 *                 logged and NONE of them throws.
 *
 * @pre The preflight returned true for @p binderDriverPath and its descriptor is still
 *      held by the caller, so @p validated describes an object that still exists.
 * @post No descriptor opened here survives the call: the fresh one is released on every
 *       path, including the refusals. The caller's retained descriptor is untouched.
 * @warning Never throws and never blocks beyond @p contextManagerTimeoutMs.
 *
 * @see binderNodeIdentitiesMatch()
 * @see DriverAidlImpl::isBinderPreflightOk()
 * @see DriverAidlImpl::isServiceAvailable()
 */
bool reverifyBinderNodeBeforeLookup(const std::string &binderDriverPath,
                                    unsigned int contextManagerTimeoutMs,
                                    const DriverAidlImpl::BinderPreflightProbe &probe,
                                    const DriverAidlImpl::BinderNodeIdentity &validated)
{
	DriverAidlImpl::BinderNodeIdentity observed;

	memset(&observed, 0, sizeof(observed));

	/* Re-verification point 1 of 4: the name still resolves to something. */
	if (0 != probe.identifyPath(binderDriverPath.c_str(), &observed)) {
		const int identifyErrno = errno;

		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder driver [%s] could no longer be resolved between the preflight and the lookup, errno %d (%s); declining the AIDL HAL rather than letting libbinder open a name whose meaning changed\r\n", binderDriverPath.c_str(), identifyErrno, strerror(identifyErrno));
		return false;
	}

	/*
	 * Re-verification point 2 of 4: and it resolves to the SAME object that was validated,
	 * IN THE SAME STATE - the attribute-by-attribute detail is emitted by the comparison
	 * itself, which is why this line names the condition and not the values.
	 */
	if (!binderNodeIdentitiesMatch(validated, observed)) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder driver [%s] is NOT the node the preflight validated - it resolves to a different filesystem object, or to the same one re-permissioned or re-owned since; declining the AIDL HAL. A node substituted in this window would otherwise be opened by libbinder, which aborts rather than fails on the pinned stack\r\n", binderDriverPath.c_str());
		return false;
	}

	/*
	 * Re-verification point 3 of 4: a second descriptor on the same node, for the ping the
	 * retained descriptor cannot carry. Opened with the same flags the preflight used, so
	 * the two descriptors are equivalent in every respect that matters.
	 */
	const int freshFd = probe.openNode(binderDriverPath.c_str(), O_RDWR | O_CLOEXEC);

	if (freshFd < 0) {
		const int openErrno = errno;

		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder driver [%s] could not be reopened for the pre-lookup liveness check, errno %d (%s); declining the AIDL HAL\r\n", binderDriverPath.c_str(), openErrno, strerror(openErrno));
		return false;
	}

	DriverAidlImpl::BinderNodeIdentity reopened;

	memset(&reopened, 0, sizeof(reopened));

	if ((0 != probe.identifyDescriptor(freshFd, &reopened)) ||
	    (!binderNodeIdentitiesMatch(validated, reopened))) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : the descriptor reopened on binder driver [%s] could not be identified, or does not refer to the node the preflight validated in the state it validated it; declining the AIDL HAL\r\n", binderDriverPath.c_str());
		probe.closeNode(freshFd);
		return false;
	}

	/*
	 * Clamped here as well as in the preflight, so that neither entry point can exceed the
	 * ceiling and initialization stays bounded whichever route reached this ping.
	 */
	unsigned int effectiveTimeoutMs = contextManagerTimeoutMs;

	if (effectiveTimeoutMs > DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS) {
		effectiveTimeoutMs = DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS;
	}

	/* Re-verification point 4 of 4: a context manager still answers, under the same bound. */
	const bool contextManagerReachable = probe.pingContextManager(freshFd, effectiveTimeoutMs);

	probe.closeNode(freshFd);

	if (!contextManagerReachable) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : the binder context manager answered the preflight but not the pre-lookup check within %u ms; declining the AIDL HAL rather than entering an unbounded wait inside the service manager\r\n", effectiveTimeoutMs);
		return false;
	}

	return true;
}

/**
 * @brief Renders a server-supplied interface hash safely for a log line
 *
 * The hash arrives from another process over binder, so its content and its length are
 * both outside this process's control and neither may be pasted into a log unexamined.
 * A frozen AIDL hash is a short hexadecimal digest, but a broken or hostile server can
 * answer with anything at all - control characters that would corrupt a terminal or a
 * log parser, or a payload long enough to bury the rest of the diagnosis.
 *
 * Every byte outside printable ASCII becomes `?`, so the length and the shape of what
 * arrived are still visible while nothing it contains can act on a reader's tooling, and
 * anything longer than MAX_LOGGED_HASH_LENGTH is truncated with an ellipsis and its true
 * length reported - truncating silently would be its own small lie.
 *
 * @param [in] hash - The hash exactly as the server reported it. May be empty, may
 *                    contain any byte value, and may be arbitrarily long.
 *
 * @return std::string - A bounded, printable rendering safe to substitute into a log
 *                       line. Empty input yields the literal `<empty>` rather than
 *                       nothing, so an empty hash cannot read as a formatting fault.
 *
 * @pre None.
 * @post No state changed; the argument is not modified.
 * @warning For diagnostics only. No compatibility decision is ever taken on this value:
 *          halcompat::isCompatible() reads the unmodified hash and is the sole decision.
 *
 * @see DriverAidlImpl::isServiceAvailable()
 */
std::string sanitizedInterfaceHash(const std::string &hash)
{
	if (hash.empty()) {
		return std::string("<empty>");
	}

	const size_t rendered = (hash.size() > MAX_LOGGED_HASH_LENGTH) ? MAX_LOGGED_HASH_LENGTH : hash.size();
	std::string safe;

	safe.reserve(rendered + 32);

	for (size_t i = 0; i < rendered; i++) {
		const unsigned char byte = static_cast<unsigned char>(hash[i]);

		safe += ((byte >= 0x20) && (byte < 0x7F)) ? static_cast<char>(byte) : '?';
	}

	if (hash.size() > rendered) {
		char tail[64];

		snprintf(tail, sizeof(tail), "... (%zu bytes total)", hash.size());
		safe += tail;
	}

	return safe;
}

} // anonymous namespace

/**
 * @copydoc CCEC::DriverAidlImpl::describeObservedInterfaceHash
 *
 * The three phrases below say what the observed string is, and none of them says why the
 * rejection happened. That separation is the whole of this function: a reader can see
 * whether the server answered with a frozen digest, with the failure marker, or with an
 * unfrozen development marker, without the code claiming to know which of halcompat's
 * rules did the rejecting.
 *
 * The three phrases restate halcompat's own hash classification because halcompat exposes no
 * entry point for a hash alone - not a public one and not an internal one. Its public
 * `isCompatible<I>()` and `atLeast<I>()` apply the empty, `"-1"` and `"notfrozen"` gates
 * internally and return only a verdict, and `namespace detail` at
 * `rdk-halif-aidl/common/current/halcompat.h:80` holds nothing that takes a hash. The
 * markers named here are therefore read from halcompat's own gate order rather than invented,
 * and nothing in this function decides anything.
 *
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted() - the public/internal surface, in full
 * @see DriverAidlImpl::isServiceAvailable()
 */
const char *DriverAidlImpl::describeObservedInterfaceHash(const std::string &hash)
{
	if (hash.empty()) {
		return "empty - the shape an interface-hash transaction takes when it FAILS, rather than a value a working server reports";
	}

	if (hash == "-1") {
		return "the \"-1\" failure marker - the shape an interface-hash transaction takes when it FAILS, rather than a value a working server reports";
	}

	if (hash == "notfrozen") {
		return "the \"notfrozen\" marker - an UNFROZEN development build, which makes no compatibility promise and is not accepted by default";
	}

	return "a frozen release digest - the shape a working, released server reports";
}

/**
 * @copydoc CCEC::DriverAidlImpl::observedMetadataWouldBeAccepted
 *
 * The version half calls `halcompat::detail::isCompatible()` rather than restating its
 * rule, so this function cannot drift from the rule that governed the decision. The hash
 * half is written out here because halcompat exposes no equivalent entry point for a
 * hash alone; it follows halcompat's own gate order, and it treats an unfrozen server as
 * not accepted because production calls the predicate with its `allowUnfrozen` default of
 * false.
 *
 * DELIBERATE USE OF A HALCOMPAT ENTRY POINT THAT HALCOMPAT MARKS INTERNAL, ANNOTATED HERE
 * BECAUSE NO PUBLIC EQUIVALENT EXISTS FOR EITHER HALF OF THIS OBSERVATION.
 * `rdk-halif-aidl/common/current/halcompat.h:26-27` states that client code never calls
 * `getInterfaceVersion()`/`getInterfaceHash()` directly, and its `namespace detail` at :80,
 * opened by the "Internal encoding machinery" note above it, holds the version predicate
 * this function calls. Both statements are read as written, and this is the one place in
 * this file that departs from them.
 *
 * WHAT THE PUBLIC SURFACE ACTUALLY OFFERS, enumerated so the departure can be checked rather
 * than taken on trust. halcompat's public API is exactly three templates: `getService<I>()`
 * at :143, `isCompatible<I>(service, allowUnfrozen)` at :158 and
 * `atLeast<I>(service, era, major, minor, bugfix, allowUnfrozen)` at :181. All three take a
 * SERVICE PROXY. There is no public predicate over an already-observed version, no public
 * accessor for an observed hash or version, and no public entry point of any kind for a hash
 * alone - so neither half of this observation can be expressed publicly: the version half has
 * only the internal predicate, and the hash half has no halcompat entry point at all, public
 * or internal, which is why its three gates are restated here in halcompat's own order.
 *
 * WHY `atLeast()` IS NOT THE PUBLIC ROUTE IT LOOKS LIKE. It takes a proxy and would therefore
 * re-read `getInterfaceHash()` and `getInterfaceVersion()` from the server itself - a SECOND
 * REMOTE ROUND TRIP, to a server this code has already established is misbehaving, at
 * initialization, inside the fallback path. That breaks the one-snapshot rule this diagnostic
 * is built on: the caller reads the hash and the version exactly once each and every
 * statement it emits is a property of that one pair, precisely so that a predicate result
 * cannot be paired with separately retried values and blame the version rule for a rejection
 * it did not cause. It would also change the question asked, since `atLeast()` gates a named
 * feature release rather than evaluating this client's own `VERSION`.
 *
 * THE DECISION IS UNAFFECTED, AND THAT IS THE LINE THIS ANNOTATION DRAWS. The compatibility
 * VERDICT is taken by the public `halcompat::isCompatible<IHdmiCec>()` call in
 * isServiceAvailable(), which is the sole decision and reads its own metadata. Nothing here
 * or in its callers re-decides anything; the internal entry points are reached only by the
 * post-rejection attribution, on a path taken after the verdict is recorded, and the
 * predicate they reach is `constexpr` over two `int32_t`s and performs no transaction.
 *
 * WHAT WOULD MAKE THIS A PUBLIC CALL. Either a public predicate in halcompat taking an
 * already-observed version pair - the `detail::isCompatible(int32_t, int32_t)` shape at :108,
 * promoted out of `detail` - or a public accessor returning the hash and version halcompat
 * itself read while deciding. The first removes the internal-namespace use; the second
 * removes it and the observation's second read as well, since the values the decision used
 * would then be recoverable instead of unobtainable. Owning that surface belongs to the
 * halcompat owner in `rdk-halif-aidl`, which is a reference-only consumed header here; this
 * annotation records the dependency so the use is visible to them rather than silent.
 *
 * @see DriverAidlImpl::describeObservedInterfaceHash()
 * @see DriverAidlImpl::emitCompatibilityRejectionDiagnostic()
 * @see DriverAidlImpl::isServiceAvailable()
 */
bool DriverAidlImpl::observedMetadataWouldBeAccepted(const std::string &hash,
                                                     int clientVersion,
                                                     int observedVersion)
{
	if (hash.empty() || hash == "-1" || hash == "notfrozen") {
		return false;
	}

	return halcompat::detail::isCompatible(clientVersion, observedVersion);
}


/**
 * @copydoc CCEC::DriverAidlImpl::emitCompatibilityRejectionDiagnostic
 *
 * Every line below is bounded independently against the log buffer, and the body's own
 * comments record which bound each one is written against.
 *
 * DIRECT `getInterfaceHash()`/`getInterfaceVersion()` READS, WHICH HALCOMPAT MARKS INTERNAL,
 * ANNOTATED HERE RATHER THAN LEFT AS AN INCIDENTAL CALL.
 * `rdk-halif-aidl/common/current/halcompat.h:26-27` states that client code never calls those
 * two directly, and the snapshot below does. It is deliberate and it is confined to
 * attribution: halcompat's whole public API - `getService<I>()`, `isCompatible<I>()` and
 * `atLeast<I>()` - takes a service proxy and returns a verdict, and exposes no accessor for
 * the metadata it read while reaching that verdict, so there is no public way to observe what
 * a rejected server reported. The values halcompat used are locals inside a template in a
 * read-only consumed header and cannot be recovered; reading them here is the only way the
 * diagnostic can name anything about the server at all, which is why every line it emits is
 * labelled an observation rather than a cause. observedMetadataWouldBeAccepted() carries the
 * full enumeration of that public surface and of what would have to appear in halcompat for
 * these two reads to become unnecessary.
 *
 * The decision itself is untouched by any of this: the verdict is the public
 * `halcompat::isCompatible<IHdmiCec>()` call in isServiceAvailable(), the cause is recorded
 * there before this function is entered, and this function is static so it cannot reach
 * availabilityReason to relabel it.
 *
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted() - the public/internal surface, in full
 * @see DriverAidlImpl::isServiceAvailable()
 */
void DriverAidlImpl::emitCompatibilityRejectionDiagnostic(
	const ::android::sp< cechal::IHdmiCec > &service,
	const std::string &halServiceName)
{
	try {
		const int clientVersion = cechal::IHdmiCec::VERSION;

		/*
		 * The one snapshot. Each value is read exactly once and nothing below re-reads
		 * either. Combining a predicate result with separately retried values would let
		 * the not-compatible arm hold compatible metadata and blame the version rule for
		 * a rejection it did not cause.
		 */
		/*
		 * INSTRUMENTED LIKE EVERY OTHER SYNCHRONOUS CALL, and for a sharper reason than
		 * consistency. These two are REMOTE ROUND TRIPS to the same server that has just
		 * been rejected, and a server that answers wrongly is exactly the kind that answers
		 * slowly or not at all - so a stall here happens at initialization, inside the
		 * fallback path, on a platform already known to be misbehaving. Left uninstrumented
		 * it would be the one stall in this file with no attribution at all: the last line
		 * in the log would be the rejection, and nothing would say that the process is
		 * blocked in a diagnostic ABOUT the rejection rather than in the rejection itself.
		 * Each is measured separately, so the log names which of the two blocked.
		 */
		const int64_t hashReadStartedMs = halCallStarted();
		const std::string observedHash = service->getInterfaceHash();

		warnIfHalCallSlow("IHdmiCec::getInterfaceHash (post-rejection diagnostic read)", hashReadStartedMs);

		const int64_t versionReadStartedMs = halCallStarted();
		const int observedVersion = service->getInterfaceVersion();

		warnIfHalCallSlow("IHdmiCec::getInterfaceVersion (post-rejection diagnostic read)", versionReadStartedMs);

		/*
		 * Emitted as several short lines, and the reason is a hard platform limit rather
		 * than a preference.
		 *
		 * CCEC_LOG formats through a 500-byte stack buffer -- MAX_LOG_BUFF in
		 * ccec/src/Util.cpp, which is not this migration's to change -- and vsnprintf
		 * truncates silently at 499. One message of about 900 characters does not survive
		 * that, and it fails in the worst possible direction: the sentence explaining that
		 * no cause is being claimed sits at the front and survives, while every observed
		 * value the diagnostic exists to report -- the hash, its classification, both
		 * versions and the recovery note -- is cut off, leaving a prefix that reads like a
		 * complete message. The contract-suite case that captures this output is what
		 * makes that bound observable rather than a matter of trust.
		 *
		 * So each line below is bounded independently, with the longest possible
		 * substitution in it, and the recovery note is a line of its own rather than a
		 * suffix -- as a suffix it could push the line it was appended to over the limit
		 * and take the values with it. Adding to any of these means re-checking its worst
		 * case against 499, not appending and hoping.
		 */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder service [%s] is present but was REJECTED AS NOT COMPATIBLE by halcompat::isCompatible(), the SOLE decision. It rejects for one of three reasons - an unreadable interface hash, an UNFROZEN development server, or a version outside this client's era and major - and WHICH ONE APPLIED IS NOT REPORTED HERE: the metadata the decision read cannot be recovered, and a fresh read is a different transaction\r\n",
		          halServiceName.c_str());

		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : service [%s] OBSERVED AFTER the decision, so this is evidence about the server rather than the cause of the rejection: interface hash [%s], which is %s\r\n",
		          halServiceName.c_str(),
		          sanitizedInterfaceHash(observedHash).c_str(),
		          describeObservedInterfaceHash(observedHash));

		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : service [%s] OBSERVED AFTER the decision: server interface version %d against this client's %d. The AIDL HDMI CEC HAL is treated as absent and the legacy back-end is selected\r\n",
		          halServiceName.c_str(), observedVersion, clientVersion);

		if (observedMetadataWouldBeAccepted(observedHash, clientVersion, observedVersion)) {
			CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : service [%s] NOTE: that observation WOULD itself have been accepted, so the server's metadata changed or recovered after the decision - look for an INTERMITTENT METADATA TRANSACTION rather than a version mismatch\r\n",
			          halServiceName.c_str());
		}
	}
	catch(...) {
		/*
		 * The observation itself failed. That costs a less specific message and nothing
		 * else. The caller recorded the cause before calling this, and this function is
		 * static, so there is no path by which a failed observation can relabel an
		 * established compatibility rejection.
		 */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder service [%s] is present but was REJECTED AS NOT COMPATIBLE by halcompat::isCompatible(), and its metadata could not be read back afterwards to describe the server, because that read failed too. The rejection is unaffected and its recorded cause unchanged: the AIDL HDMI CEC HAL is treated as absent and the legacy back-end is selected\r\n", halServiceName.c_str());
	}
}


/**
 * @copydoc CCEC::DriverAidlImpl::isServiceAvailable
 *
 * The function-level catch-all at the end is what makes the no-propagation guarantee
 * unconditional rather than merely expected: every stage below is written not to raise,
 * and the catch is there for the case where one does anyway, because an exception
 * escaping into the factory's one-time static initializer would fail initialization on a
 * platform whose correct outcome is the legacy back-end.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 * @see DriverAidlImpl::open()
 */
bool DriverAidlImpl::isServiceAvailable(const std::string &binderDriverPath,
                                        unsigned int contextManagerTimeoutMs,
                                        const BinderPreflightProbe &probe)
{
	availabilityReason = NULL;

	/*
	 * THE CUSTODY WINDOW OPENS HERE AND CLOSES WHEN THIS OBJECT GOES OUT OF SCOPE, on every
	 * one of this method's exit paths including the catch-all's. Declared OUTSIDE the try so
	 * that the descriptor is released after the handler has run rather than during the
	 * unwind, which keeps the release out of the exceptional path's way while still making
	 * it unconditional.
	 */
	RetainedBinderNode retainedNode(probe);
	BinderNodeIdentity validatedIdentity;

	memset(&validatedIdentity, 0, sizeof(validatedIdentity));

	try {
		/*
		 * Stage 1: the preflight, WITH CUSTODY. Passing the two out-parameters is what
		 * makes this stage and the lookup below one path instead of two independent
		 * resolutions of the same pathname: the validated descriptor stays open, pinning
		 * the node's inode, and its identity is carried forward to stage 2.
		 */
		if (!isBinderPreflightOk(binderDriverPath, contextManagerTimeoutMs, probe,
		                         retainedNode.descriptorSlot(), &validatedIdentity)) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl::isServiceAvailable : the binder preflight declined; the AIDL HDMI CEC HAL is treated as absent\r\n");
			availabilityReason = REASON_TRANSPORT_UNAVAILABLE;
			return false;
		}

		/*
		 * Stage 2: THE SAME NODE, AND A STILL-LIVE CONTEXT MANAGER, immediately before the
		 * lookup. The preflight's verdict is only worth acting on if it still describes the
		 * name libbinder is about to open, and the check that establishes that has to sit
		 * as close to the use as the code can put it - which is here, with nothing between
		 * this and getService() but the service name.
		 *
		 * A false verdict is reported as an unavailable TRANSPORT rather than as an
		 * unusable service, deliberately: what failed is the driver node or its context
		 * manager, not anything about `"HdmiCec"`, and reporting it as a service problem
		 * would send an integrator looking in the wrong place.
		 */
		if (!reverifyBinderNodeBeforeLookup(binderDriverPath, contextManagerTimeoutMs, probe,
		                                    validatedIdentity)) {
			availabilityReason = REASON_TRANSPORT_UNAVAILABLE;
			return false;
		}

		const std::string &halServiceName = cechal::IHdmiCec::serviceName();
		/* Synchronous lookup, with no client-side deadline available: measured, not bounded. */
		const int64_t lookupStartedMs = halCallStarted();
		::android::sp<cechal::IHdmiCec> service = halcompat::getService<cechal::IHdmiCec>();

		warnIfHalCallSlow("IServiceManager::checkService via halcompat::getService<IHdmiCec>", lookupStartedMs);

		if (service == 0) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl::isServiceAvailable : binder service [%s] is not registered; the AIDL HDMI CEC HAL is treated as absent\r\n", halServiceName.c_str());
			availabilityReason = REASON_NO_COMPATIBLE_SERVICE;
			return false;
		}

		/* getInterfaceHash()/getInterfaceVersion() are round trips: measured, not bounded. */
		const int64_t compatibilityStartedMs = halCallStarted();
		const bool compatible = halcompat::isCompatible<cechal::IHdmiCec>(service);

		warnIfHalCallSlow("IHdmiCec::getInterfaceVersion and getInterfaceHash via halcompat::isCompatible<IHdmiCec>", compatibilityStartedMs);

		if (!compatible) {
			/*
			 * The decision has already been taken, and halcompat took it.
			 * halcompat::isCompatible() is the single implementation of the compatibility
			 * rule; nothing below re-implements it or re-decides anything.
			 *
			 * The cause is recorded here, before any observation, so that what explains
			 * the fallback is fixed by the decision itself and cannot be rewritten by
			 * anything that runs afterwards.
			 */
			availabilityReason = REASON_NO_COMPATIBLE_SERVICE;

			/*
			 * What follows reports an observation about the server and never a cause.
			 *
			 * halcompat decided using its own metadata transactions, and those values
			 * cannot be recovered: they are locals inside a template in a read-only
			 * consumed header. Anything read here is a fresh transaction, and the
			 * generated proxy does not make the two equivalent - it caches a value it
			 * read successfully, but stores a failed read as -1 and retries it on the
			 * next call. So a transaction that failed for the decision can succeed
			 * afterwards, and an in-process fake whose answers change between calls
			 * produces the same divergence. Three rules follow from that, each of them
			 * load bearing:
			 *   * one snapshot. The hash and the version are read exactly once each, and
			 *     every statement derived from them is a property of that one pair. A
			 *     predicate result is never combined with separately retried values,
			 *     because asking the predicate again and then re-reading the metadata can
			 *     land a still-failing retry alongside compatible values;
			 *   * no causal claim. Which of halcompat's three rules rejected this server
			 *     is not reported, because nothing here can know it. All three are named
			 *     as the possibilities they are, and the snapshot is labelled as evidence
			 *     about the server rather than as the reason it was rejected. An observed
			 *     hash that looks frozen in particular does not implicate the
			 *     era-and-major version rule;
			 *   * no reimplementation of the version rule. The version half of the
			 *     observation is evaluated by halcompat::detail::isCompatible(), the real
			 *     rule - constexpr over two ints, so it performs no transaction and
			 *     cannot disagree with itself.
			 *
			 * The diagnostic lives in emitCompatibilityRejectionDiagnostic() rather than
			 * inline here, because on a host with no binder driver the preflight declines
			 * before this stage is entered, so wording written inline could not be
			 * reached by a test at all. That function is static, and being static is the
			 * load-bearing part rather than an implementation detail: with no `this` it
			 * cannot reach availabilityReason, so the cause recorded above survives the
			 * diagnostic by construction. It also swallows its own failure, so the
			 * function-level catch below - which would record REASON_QUERY_FAILED and so
			 * relabel an established compatibility rejection - is unreachable from it.
			 */
			emitCompatibilityRejectionDiagnostic(service, halServiceName);

			return false;
		}

		hdmiCecService = service;

		CCEC_LOG( LOG_INFO, "DriverAidlImpl::isServiceAvailable : binder service [%s] is present and compatible\r\n", halServiceName.c_str());

		return true;
	}
	catch(...) {
		CCEC_LOG( LOG_EXP, "DriverAidlImpl::isServiceAvailable : unexpected failure while querying the AIDL HDMI CEC service; it is treated as absent\r\n");
		availabilityReason = REASON_QUERY_FAILED;
		return false;
	}
}

/**
 * @copydoc CCEC::DriverAidlImpl::unavailabilityReason
 *
 * A pure read of the pointer isServiceAvailable() set. The body is one statement for the
 * reason the declaration gives: rebuilding the answer would re-run the bounded preflight,
 * and the record is what makes the reported condition the one that actually caused the
 * fallback.
 *
 * @see DriverAidlImpl::isServiceAvailable()
 */
const char *DriverAidlImpl::unavailabilityReason(void) const
{
	return availabilityReason;
}




CCEC_END_NAMESPACE


/** @} */
/** @} */
