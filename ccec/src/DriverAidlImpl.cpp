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
 * Holds the CCEC::Driver overrides, the service-availability query and binder preflight
 * behind the back-end selection in ccec/src/Driver.cpp, and the nested HAL event listener.
 * Each method mirrors its DriverImpl counterpart, with the `com.rdk.hal.hdmicec` AIDL call
 * substituted for the legacy HDMI CEC C API. Observable differences from the legacy
 * back-end are documented on the methods that carry them.
 *
 * @note Every AIDL, binder and halcompat header is included before CCEC_BEGIN_NAMESPACE,
 *       and no legacy HAL header or symbol is used in this file.
 * @warning Requires C++17 and the C++ libbinder AIDL backend, not the NDK backend.
 *
 * @see DriverAidlImpl.hpp
 * @see DriverImpl.cpp
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
#include <algorithm>
#include <cstdint>
#include <cxxabi.h>
#include <exception>
#include <string>
#include <vector>

/* AIDL and binder headers: these must stay above CCEC_BEGIN_NAMESPACE. */
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

/* Shared HAL compatibility helpers, from the rdk-halif-aidl/common/current/ include root. */
#include "halcompat.h"

/* Binder kernel UAPI, read by the preflight without libbinder; when it is absent the
 * preflight reports the AIDL HAL absent and the legacy back-end is selected. */
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
 * Used instead of using-declarations, whose generic names such as `State` could collide
 * with the middleware's own global-scope names.
 */
namespace cechal = ::com::rdk::hal::hdmicec;

/** @brief Short alias for the shared HAL compatibility helpers in halcompat.h. */
namespace halcompat = ::com::rdk::hal::halcompat;

/* Frame block offsets, as DriverImpl.hpp defines them, guarded to keep one definition. */
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
 * `IHdmiCecController.sendMessage()` allows 16 bytes, whereas CECFrame carries up to
 * CECFrame::MAX_LENGTH (128); write() enforces this limit.
 *
 * @see DriverAidlImpl::write()
 */
const size_t AIDL_MAX_MESSAGE_LENGTH = 16;

/**
 * @brief Shortest received CEC message this back-end will accept onto the incoming queue
 *
 * An empty message would crash the Bus reader thread when it decodes the header, while a
 * one-byte header-only frame, such as a poll, is legitimate CEC traffic and is delivered.
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
 * 0xF is the broadcast/unregistered address, not one a device holds. The HAL's raw
 * `int32_t` is checked before conversion, because LogicalAddress narrows it (256 becomes
 * 0); a rejected value yields getLogicalAddress()'s no-address result of 0.
 *
 * @see DriverAidlImpl::getLogicalAddress()
 */
const int32_t HAL_LOGICAL_ADDRESS_MAX = 0xE;

/**
 * @brief Most message bytes the receive diagnostic renders before it truncates
 *
 * Shows every byte of a well-formed message (16 at most) with margin, so the binder-thread
 * diagnostic stays bounded.
 *
 * @see renderReceivedMessageHex()
 */
const size_t RECEIVE_LOG_MAX_BYTES = 24;

/** @brief Appended to the rendering when the message was longer than RECEIVE_LOG_MAX_BYTES. */
const char RECEIVE_LOG_TRUNCATION_MARKER[] = "...";

/**
 * @brief Size of the stack buffer the receive diagnostic renders into
 *
 * Three characters per rendered byte (`"%02X "`) plus the truncation marker and its
 * terminator, derived so it cannot fall out of step with RECEIVE_LOG_MAX_BYTES.
 */
const size_t RECEIVE_LOG_TEXT_SIZE = (RECEIVE_LOG_MAX_BYTES * 3) + sizeof(RECEIVE_LOG_TRUNCATION_MARKER);

/**
 * @brief Renders a received CEC message as bounded `"%02X "` hex text, without calling out
 *
 * Call-free and bounded, because it runs on every received message whatever the log level.
 *
 * @param [in]  message - First byte of the message; not read when @p length is zero.
 * @param [in]  length  - Bytes at @p message; only RECEIVE_LOG_MAX_BYTES are rendered.
 * @param [out] text    - Receives the NUL-terminated rendering.
 *
 * @pre @p message addresses at least @p length readable bytes.
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
 * A diagnostic threshold only: crossing it attempts one LOG_WARN line and changes nothing
 * else. The value is the HDMI CEC HAL specification's one-second transmit-completion bound.
 *
 * @warning This bounds nothing: the pinned libbinder has no client-side transaction deadline.
 *
 * @see warnIfHalCallSlow()
 */
const int64_t SLOW_HAL_CALL_WARN_MS = 1000;

/**
 * @brief Start instant standing for "the monotonic clock could not be read"
 *
 * Negative, so it cannot collide with a real reading; a start carrying it skips the
 * elapsed measurement rather than inventing an origin.
 *
 * @see halCallStarted()
 * @see warnIfHalCallSlow()
 */
const int64_t HAL_CALL_CLOCK_UNREADABLE = -1;

/**
 * @brief Reads CLOCK_MONOTONIC as whole milliseconds
 *
 * The one clock helper, shared by the HAL-call diagnostics and the probe deadline.
 *
 * @param [out] nowMs - Receives the reading; left untouched on failure.
 *
 * @return bool - Whether the clock could be read
 * @retval true  - @p nowMs holds the current monotonic instant.
 * @retval false - clock_gettime() failed and @p nowMs is unmodified.
 *
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
 * Paired with warnIfHalCallSlow(); two clock reads are negligible against an IPC round trip.
 *
 * @return int64_t - Monotonic start instant in milliseconds, or HAL_CALL_CLOCK_UNREADABLE
 *                   when the clock could not be read.
 *
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
 * Runs after the call returns and attempts one LOG_WARN line when it took longer than
 * SLOW_HAL_CALL_WARN_MS; a start of HAL_CALL_CLOCK_UNREADABLE skips the measurement.
 *
 * @param [in] operation - Name of the AIDL operation that just returned, logged verbatim.
 * @param [in] startedMs - Value halCallStarted() returned before the same call.
 *
 * @post At most one LOG_WARN line was attempted; nothing else changed.
 * @warning Never throws, never alters control flow and enforces no deadline.
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
		/* One line naming the call and the elapsed time, sized against the CCEC_LOG limit; it
		 * states that nothing was enforced, so it cannot be mistaken for a mitigation. */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl: synchronous AIDL call [%s] returned after %lld ms, past the %lld ms threshold. NO DEADLINE WAS ENFORCED and the call was not abandoned - the pinned libbinder offers no client-side transaction bound. A HAL that stalls this long breaks the platform prerequisite recorded on DriverAidlImpl::isServiceAvailable()\r\n", operation, (long long)elapsedMs, (long long)SLOW_HAL_CALL_WARN_MS);
	}
}

#if CCEC_HAVE_BINDER_UAPI

/**
 * @brief Size of the transient binder mapping the context-manager probe installs
 *
 * The driver places the ping's REPLY in the receiver's own mapping, so the probe maps a
 * small region and unmaps it before returning.
 */
const size_t BINDER_PROBE_MAP_SIZE = 64 * 1024;

/**
 * @brief Upper bound on the iterations the context-manager probe will perform
 *
 * Caps the drain of interleaved driver commands, so the probe terminates even if the clock
 * misbehaves or the driver returns an unexpected stream.
 */
const unsigned int BINDER_PROBE_MAX_ITERATIONS = 64;

/**
 * @brief Longest single poll() wait the context-manager probe will ask for, in milliseconds
 *
 * Clamping each wait into [0, POLL_SLICE_MAX_MS] keeps the `int` passed to poll() from
 * turning negative, which would wait without limit.
 *
 * @see pingBinderContextManager()
 */
const unsigned int POLL_SLICE_MAX_MS = 250;

/* The iteration cap must outlast the longest deadline, or a usable context manager would
 * read as unreachable; it leaves margin for EINTR retries and empty drains. */
static_assert((DriverAidlImpl::MAX_CONTEXT_MANAGER_TIMEOUT_MS / POLL_SLICE_MAX_MS) <= BINDER_PROBE_MAX_ITERATIONS,
              "BINDER_PROBE_MAX_ITERATIONS must cover MAX_CONTEXT_MANAGER_TIMEOUT_MS in POLL_SLICE_MAX_MS slices");

/**
 * @brief Asks the binder driver, under a bounded wait, whether handle 0 resolves
 *
 * Posts one PING_TRANSACTION to handle 0 over the caller's descriptor and drains the reply
 * against one absolute deadline, because libbinder's own lookup retries without limit.
 *
 * @param [in] driverFd  - Binder descriptor opened O_RDWR with a verified protocol; left open.
 * @param [in] timeoutMs - Bound on the wait in milliseconds; zero polls once without waiting.
 *
 * @return bool - Whether handle 0 answered the ping
 * @retval true  - A reply arrived, so a context manager is reachable.
 * @retval false - Mapping, posting or the clock failed, the ping was rejected, or time ran out.
 *
 * @post The probe's mapping is released on every path.
 * @warning Never throws, and never blocks past @p timeoutMs plus one poll slice.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 */
bool pingBinderContextManager(int driverFd, unsigned int timeoutMs)
{
	void *mapped = ::mmap(0, BINDER_PROBE_MAP_SIZE, PROT_READ,
	                      MAP_PRIVATE | MAP_NORESERVE, driverFd, 0);
	if (MAP_FAILED == mapped) {
		CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: binder mmap failed with errno %d, treating the context manager as unreachable\r\n", errno);
		return false;
	}

	/* A command word immediately followed by the transaction payload, unpadded as the driver
	 * parses it, hence a memcpy'd byte array rather than a struct. */
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

	/* One absolute deadline, fixed before the loop, so no iteration re-grants the timeout. */
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

		/* sliceMs lies in [0, POLL_SLICE_MAX_MS], so this poll() cannot wait without limit. */
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

/* The default probe operations: the real kernel-facing calls, compiled into every build so
 * isBinderPreflightOk() has the same eight decision points with or without the binder UAPI. */

/**
 * @brief Default probe: opens the binder driver node with the real `::open`
 *
 * A wrapper because the variadic `::open` has no fixed-arity function-pointer type.
 *
 * @param [in] path  - Node to open.
 * @param [in] flags - Open flags; the preflight passes `O_RDWR | O_CLOEXEC`.
 *
 * @return int - Descriptor, or negative with `errno` set.
 *
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
 * The one place the POSIX stat types are converted, each field widened without truncation,
 * so DriverAidlImpl.hpp needs no `<sys/stat.h>`.
 *
 * @param [in]  attributes - Result of a successful `fstat` or `stat`.
 * @param [out] identity   - Receives the converted identity; every member is written.
 *
 * @pre @p identity is non-null.
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
 * Asked of the descriptor, not the path, so nothing outside the process can change the
 * reference the later path re-check compares against.
 *
 * @param [in]  driverFd - Open descriptor returned by defaultOpenBinderNode().
 * @param [out] identity - Receives the identity; written only on success.
 *
 * @return int - Outcome
 * @retval 0  - The identity was written.
 * @retval -1 - `fstat` failed (`errno` set) or @p identity was null (`errno` EINVAL).
 *
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
 * `stat` rather than `lstat`, so a `/dev/binder` symlink into binderfs yields the node
 * libbinder will actually open.
 *
 * @param [in]  path     - Path to resolve, the same one the preflight opened.
 * @param [out] identity - Receives the identity; written only on success.
 *
 * @return int - Outcome
 * @retval 0  - The identity was written.
 * @retval -1 - The path could not be resolved (`errno` set) or an argument was null (EINVAL).
 *
 * @pre None; a path that no longer exists is a legitimate input and yields -1.
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
 * Carries the version out as an `unsigned int`, so no binder kernel type reaches
 * DriverAidlImpl.hpp.
 *
 * @param [in]  driverFd        - Descriptor returned by defaultOpenBinderNode().
 * @param [out] protocolVersion - Receives the reported version; written only on success.
 *
 * @return int - Outcome
 * @retval 0  - The version was read.
 * @retval -1 - The ioctl failed (`errno` set), or this build has no binder kernel ABI
 *              definitions (`errno` ENOSYS).
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
 * Delegates to pingBinderContextManager(); a build without the binder kernel ABI reports
 * "unreachable", which yields the legacy back-end.
 *
 * @param [in] driverFd  - Descriptor whose protocol version has been verified.
 * @param [in] timeoutMs - Bound on the wait in milliseconds; zero means do not wait.
 *
 * @return bool - Whether a context manager answered
 * @retval true  - It answered.
 * @retval false - It did not within the bound, or this build cannot ask.
 *
 * @warning Never throws, and never blocks past @p timeoutMs plus one poll slice.
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
 * @param [in] driverFd - Descriptor returned by defaultOpenBinderNode().
 *
 * @return int - Zero on success, negative with `errno` set; the preflight ignores it.
 *
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
 * The AIDL counterpart of DriverImpl's receive and transmit callbacks; only
 * onMessageReceived() carries behaviour. Every callback returns `binder::Status::ok()`, even
 * after a caught failure. The owner detaches the listener on every session-ending path, so
 * a callback the HAL still delivers cannot reach freed state.
 *
 * @warning Reaches its owner only through its back pointer, never Driver::getInstance().
 * @warning Hold only in an `android::sp<>`; the HAL keeps a strong reference of its own.
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
	 * @param [in] ownerDriver - Back-end whose incoming queue receives the frames; its address
	 *                           is stored so detach() can null it.
	 *
	 * @pre @p ownerDriver outlives this listener or detaches it first, as the owner does.
	 * @post The listener is attached until detach() is called.
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
	 * Takes the lock every owner-touching callback holds for its whole body, so on return no
	 * callback is inside the owner and later received messages drop. Idempotent.
	 *
	 * @pre None; safe on an attached or a detached listener, from any thread.
	 * @post The owner back pointer is null and every later received message is a logged drop.
	 * @warning Blocks until an in-flight callback completes; no time bound is claimed.
	 * @note Does not destroy this object, which the HAL may still reference.
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
	 * Copies the message into a new CECFrame for offerReceivedFrame(), which applies the OPENED-state
	 * guard; a refused or rejected frame is released, never leaked or freed twice.
	 *
	 * @param [in] message - Raw CEC message, header byte first; one shorter than
	 *                       MIN_RECEIVED_MESSAGE_LENGTH is discarded before any allocation.
	 * @return ::android::binder::Status - Always ok
	 * @retval ok - Returned unconditionally, including after a caught failure or a drop.
	 * @warning Runs on a binder threadpool thread and holds the listener lock throughout.
	 * @see detach()
	 * @see DriverAidlImpl::offerReceivedFrame()
	 */
	::android::binder::Status onMessageReceived(const ::std::vector<uint8_t> &message) override
	{
		/* Length check first, before the lock or any allocation: an empty frame would
		 * crash the Bus reader thread (see MIN_RECEIVED_MESSAGE_LENGTH). */
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

				/* One bounded, call-free diagnostic; see renderReceivedMessageHex(). */
				char messageText[RECEIVE_LOG_TEXT_SIZE];

				renderReceivedMessageHex(message.data(), message.size(), messageText);

				CCEC_LOG( LOG_DEBUG, ">>> DriverAidlImpl::EventListener::onMessageReceived : %zu bytes : %s\r\n", message.size(), messageText);

				/* offer() discards silently at capacity, so offerReceivedFrame() reports
				 * whether it took the frame; it still applies the OPENED-state guard. */
				if (owner->offerReceivedFrame(frame)) {
					/* The queue owns the frame now; clear the pointer so nothing frees it. */
					frame = 0;
				}
				else {
					/* Refused because the queue is full; this callback still owns the frame and
					 * releases it. */
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener::onMessageReceived : the incoming frame queue refused the frame at its %zu entry capacity; releasing a %zu byte frame rather than leaking it\r\n", (size_t)DriverAidlImpl::INCOMING_QUEUE_CAPACITY, message.size());

					delete frame;
					frame = 0;
				}
			}
			catch(InvalidStateException &e) {
				/* The closed-state rejection, kept apart so a close-race drop is not read as a
				 * fault; the guard raised before the offer, so the frame is still ours. */
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
	 * The body sits in a catch-all whose fallback logs both states as plain integers, because
	 * no exception may escape a `oneway` callback into onTransact().
	 *
	 * @param [in] oldState - State the HAL is leaving.
	 * @param [in] newState - State the HAL has entered.
	 *
	 * @return ::android::binder::Status - Always ok
	 * @retval ok - Returned unconditionally.
	 *
	 * @post No middleware state has changed and no exception escapes.
	 * @warning Runs on a binder threadpool thread.
	 *
	 * @see onMessageReceived()
	 * @see onMessageSent()
	 */
	::android::binder::Status onStateChanged(cechal::State oldState, cechal::State newState) override
	{
		/* Contained: an exception escaping this `oneway` callback would unwind into
		 * onTransact(), and cechal::toString() can raise std::bad_alloc. */
		try {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl::EventListener: HAL state changed from %s to %s\r\n", cechal::toString(oldState).c_str(), cechal::toString(newState).c_str());
		}
		catch(...) {
			/* Allocation-free fallback: plain integers, no std::string, so it cannot raise. */
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::EventListener: HAL state changed from %d to %d; the descriptive form of this diagnostic could not be built and was dropped\r\n", (int)oldState, (int)newState);
		}

		return ::android::binder::Status::ok();
	}

	/**
	 * @brief Records the outcome of a HAL-side transmit in the log and nothing else
	 *
	 * Mirrors DriverImpl::DriverTransmitCallback(), but logs every status verbatim with the
	 * message bytes, in a catch-all body whose fallback allocates nothing.
	 *
	 * @param [in] message - The message the HAL transmitted, echoed back.
	 * @param [in] status  - Bus outcome the HAL observed.
	 *
	 * @return ::android::binder::Status - Always ok
	 * @retval ok - Returned unconditionally.
	 *
	 * @post No middleware state has changed and no exception escapes.
	 * @warning Runs on a binder threadpool thread.
	 *
	 * @see DriverAidlImpl::write()
	 * @see onStateChanged()
	 */
	::android::binder::Status onMessageSent(const ::std::vector<uint8_t> &message, cechal::SendMessageStatus status) override
	{
		/* Contained for the reason onStateChanged() gives; this fires once per transmit. */
		try {
			/* Two hex digits per byte plus terminator, sized from the frame maximum. */
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
			/* Allocation-free fallback: the status as an integer and the length. */
			CCEC_LOG( LOG_EXP, "======== onMessageSent received. Result: %d, message length: %zu; the descriptive form of this diagnostic could not be built and was dropped\r\n", (int)status, message.size());
		}

		return ::android::binder::Status::ok();
	}

private:
	/**
	 * @brief Guards @ref owner against a callback racing the owner's teardown
	 *
	 * Held by every owner-dereferencing callback and by detach(); distinct from the owner's
	 * instance lock, so a callback can finish while teardown holds that lock.
	 */
	Mutex ownerMutex;
	/**
	 * @brief Owning back-end that consumes this listener's frames, or null once detached
	 *
	 * Read and written only under @ref ownerMutex.
	 */
	DriverAidlImpl *owner;
};

/**
 * @brief Constructs the back-end in the CLOSED state without touching binder.
 *
 * Members outside the initializer list, including both session proxies, start default
 * constructed, which leaves the proxies null.
 *
 * @see DriverAidlImpl::isServiceAvailable()
 */
DriverAidlImpl::DriverAidlImpl() : status(CLOSED), nativeHandle(0), rQueue(INCOMING_QUEUE_CAPACITY), availabilityReason(NULL)
{
	CCEC_LOG( LOG_DEBUG, "Creating DriverAidlImpl done\r\n");
}

/**
 * @brief Closes the session unless the state is CLOSED, then detaches and releases the listener.
 *
 * Catches `Exception` as the legacy destructor does, then detaches the listener on every path.
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

		/* Unconditional, for all three paths (close() threw, already detached, or a failed
		 * open() left a listener), so no listener outlives this object attached. */
		if (eventListener != 0) {
			eventListener->detach();
			eventListener.clear();
		}
    }
}

/**
 * @brief Opens the AIDL session through IHdmiCec::open(), then registers this device's logical address.
 *
 * The steps up to OPENED keep the legacy order under one lock, with the `#if 0` throw carried
 * verbatim. Address registration runs last, under the same recursive lock.
 *
 * @see DriverAidlImpl::registerDeviceLogicalAddress()
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

		/* The oneway listener needs a binder thread. startThreadPool() is idempotent; the
		   thread count stays at the library default and joinThreadPool() is never called. */
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
			/* A failed open may still have handed the listener to the HAL, so detach it
			   before unwinding. */
			if (eventListener != 0) {
				eventListener->detach();
				eventListener.clear();
			}

			throw IOException();
		}

		hdmiCecController = controller;
		status = OPENED;

		registerDeviceLogicalAddress();
    }
}

/**
 * @brief Maps a DeviceType to the logical addresses it may claim, first choice first.
 *
 * @param [in] deviceType - A DeviceType enumerator.
 *
 * @return std::vector<int> - The candidates; empty for a type that has none.
 *
 * @see DriverAidlImpl::registerDeviceLogicalAddress()
 */
std::vector<int> DriverAidlImpl::logicalAddressCandidates(int deviceType)
{
	switch (deviceType) {
		case DeviceType::TV:
			return std::vector<int>{ LogicalAddress::TV };
		case DeviceType::RECORDING_DEVICE:
			return std::vector<int>{ LogicalAddress::RECORDING_DEVICE_1, LogicalAddress::RECORDING_DEVICE_2,
			                         LogicalAddress::RECORDING_DEVICE_3 };
		case DeviceType::TUNER:
			return std::vector<int>{ LogicalAddress::TUNER_1, LogicalAddress::TUNER_2,
			                         LogicalAddress::TUNER_3, LogicalAddress::TUNER_4 };
		case DeviceType::PLAYBACK_DEVICE:
			return std::vector<int>{ LogicalAddress::PLAYBACK_DEVICE_1, LogicalAddress::PLAYBACK_DEVICE_2,
			                         LogicalAddress::PLAYBACK_DEVICE_3 };
		case DeviceType::AUDIO_SYSTEM:
			return std::vector<int>{ LogicalAddress::AUDIO_SYSTEM };
		default:
			return std::vector<int>();
	}
}

/**
 * @brief Runs the enable-time address allocation with every failure contained in this method
 *
 * Replaces the local list, first releasing any address unconfirmedReleaseAddress records (adopted
 * instead while the HAL still lists it, nothing registered while that cannot be read), then keeps
 * each candidate in unconfirmedReleaseAddress until the HAL confirms its add's outcome.
 *
 * @post Only thread cancellation's forced unwind leaves this method as an exception.
 * @see DriverAidlImpl::open()
 */
void DriverAidlImpl::registerDeviceLogicalAddress(void)
{
    {AutoLock lock_(mutex);
		logicalAddresses.clear();

		if (hdmiCecController == 0) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : no AIDL controller session is held; no logical address registered\r\n");
			return;
		}

		try {
			/* Only a failed close() or an unconfirmed add or removal leaves a record. It is settled
			   before any add, so the HAL never holds a second address. */
			if (unconfirmedReleaseAddress != LogicalAddress::UNREGISTERED) {
				const int held = unconfirmedReleaseAddress;
				const std::vector<int32_t> release(1, held);
				bool released = false;
				/* Synchronous, with no client-side deadline available: measured, not bounded. */
				const int64_t releaseStartedMs = halCallStarted();
				::android::binder::Status releaseTxn = hdmiCecController->removeLogicalAddresses(release, &released);

				warnIfHalCallSlow("IHdmiCecController::removeLogicalAddresses", releaseStartedMs);

				if (!releaseTxn.isOk() || !released) {
					if (!releaseTxn.isOk()) {
						CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : releasing the recorded logical address %d failed [%s]; reading back the HAL's addresses\r\n", held, releaseTxn.toString8().string());
					}
					else {
						CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : the HAL declined to release the recorded logical address %d; reading back the HAL's addresses\r\n", held);
					}

					/* A refusal can also mean the address was already gone, so the HAL's own list decides. */
					if (hdmiCecService == 0) {
						CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : no AIDL service proxy is held to confirm the release of logical address %d; no logical address registered\r\n", held);
						return;
					}

					std::vector<int32_t> halAddresses;
					/* Synchronous, with no client-side deadline available: measured, not bounded. */
					const int64_t getStartedMs = halCallStarted();
					::android::binder::Status getTxn = hdmiCecService->getLogicalAddresses(&halAddresses);

					warnIfHalCallSlow("IHdmiCec::getLogicalAddresses", getStartedMs);

					if (!getTxn.isOk()) {
						CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : IHdmiCec::getLogicalAddresses failed [%s]; the release of logical address %d is unconfirmed and no logical address is registered\r\n", getTxn.toString8().string(), held);
						return;
					}

					if (std::find(halAddresses.begin(), halAddresses.end(), (int32_t)held) != halAddresses.end()) {
						/* Adopted rather than joined by a second address, since the HAL keeps it either way. */
						std::list<LogicalAddress> adopted(1, LogicalAddress(held));
						logicalAddresses.splice(logicalAddresses.end(), adopted);
						unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
						CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : the HAL still holds logical address %d; adopted as this device's address, nothing added\r\n", held);
						return;
					}
				}

				unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
				CCEC_LOG( LOG_INFO, "DriverAidlImpl::registerDeviceLogicalAddress : the recorded logical address %d is released; allocating\r\n", held);
			}

			const std::vector<int> candidates = logicalAddressCandidates(LOCAL_DEVICE_TYPE);

			for (size_t index = 0; index < candidates.size(); index++) {
				const LogicalAddress candidate(candidates[index]);
				bool isFree = false;

				try {
					poll(candidate, candidate);
					CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::registerDeviceLogicalAddress : logical address %d answered its poll and is taken\r\n", candidate.toInt());
				}
				catch (CECNoAckException &) {
					isFree = true;
				}
				catch (Exception &) {
					isFree = true;
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : the poll of logical address %d failed; treating the address as free\r\n", candidate.toInt());
				}
				catch (abi::__forced_unwind &) {
					throw;
				}
				catch (const std::exception &) {
					isFree = true;
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : the poll of logical address %d raised a non-CEC exception; treating the address as free\r\n", candidate.toInt());
				}
				catch (...) {
					isFree = true;
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : the poll of logical address %d raised an unknown exception; treating the address as free\r\n", candidate.toInt());
				}

				if (!isFree) {
					continue;
				}

				/* Both allocations happen before the add, so nothing that can raise sits between a
				   committed registration and its local record. */
				std::list<LogicalAddress> record(1, candidate);
				const std::vector<int32_t> request(1, candidate.toInt());
				bool added = false;
				bool addRaised = false;
				::android::binder::Status txn;
				/* Kept until the add's outcome is confirmed, so the next addLogicalAddress() releases it. */
				unconfirmedReleaseAddress = candidate.toInt();
				/* Synchronous, with no client-side deadline available: measured, not bounded. */
				const int64_t addStartedMs = halCallStarted();

				try {
					txn = hdmiCecController->addLogicalAddresses(request, &added);
				}
				catch (abi::__forced_unwind &) {
					throw;
				}
				catch (...) {
					addRaised = true;
				}

				warnIfHalCallSlow("IHdmiCecController::addLogicalAddresses", addStartedMs);

				if (addRaised) {
					/* The add may have taken effect before it raised, so it is withdrawn best-effort
					   and allocation stops, as it does on a non-ok add status. */
					const char *withdrawal = "did not report it removed";
					bool removed = false;
					const int64_t removeStartedMs = halCallStarted();

					try {
						if (hdmiCecController->removeLogicalAddresses(request, &removed).isOk() && removed) {
							withdrawal = "removed it";
							unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
						}
					}
					catch (abi::__forced_unwind &) {
						throw;
					}
					catch (...) {
						withdrawal = "raised as well";
					}

					warnIfHalCallSlow("IHdmiCecController::removeLogicalAddresses", removeStartedMs);

					CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : IHdmiCecController::addLogicalAddresses raised an exception for logical address %d; the compensating IHdmiCecController::removeLogicalAddresses %s, and no logical address is recorded\r\n", candidate.toInt(), withdrawal);
					return;
				}

				if (!txn.isOk()) {
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : IHdmiCecController::addLogicalAddresses failed [%s]; no logical address is recorded, and logical address %d is kept as unconfirmed\r\n", txn.toString8().string(), candidate.toInt());
					return;
				}

				if (!added) {
					unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : the HAL declined logical address %d; trying the next candidate\r\n", candidate.toInt());
					continue;
				}

				/* splice() relinks the node allocated above; it neither allocates nor throws. */
				logicalAddresses.splice(logicalAddresses.end(), record);
				unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
				CCEC_LOG( LOG_INFO, "DriverAidlImpl::registerDeviceLogicalAddress : registered logical address %d for device type %d\r\n", candidate.toInt(), LOCAL_DEVICE_TYPE);
				return;
			}

			CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : no free logical address for device type %d; none registered\r\n", LOCAL_DEVICE_TYPE);
		}
		catch (abi::__forced_unwind &) {
			throw;
		}
		catch (const std::exception &) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : allocation raised a standard exception; no logical address registered\r\n");
		}
		catch (...) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::registerDeviceLogicalAddress : allocation raised an unknown exception; no logical address registered\r\n");
		}
    }
}

/**
 * @brief Closes an OPENED AIDL session through IHdmiCec::close() and leaves this side CLOSED.
 *
 * Runs the legacy steps in order with CLOSED set before any raise, so a failed close still leaves
 * this side closed; it also records the held address for the next registration to release.
 *
 * @note Pending owner confirmation (B2): IHdmiCec::close() stands in for HdmiCecClose().
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

		/* Offered under queueProducerMutex, because close() is a producer on this queue too. */
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

		/* Detach on both arms, since a failed close leaves the HAL free to call back, and after
		 * the sentinel offer, so no queue lock is held while detach() waits. */
		if (eventListener != 0) {
			eventListener->detach();
			eventListener.clear();
		}

		CCEC_LOG( LOG_DEBUG, "DriverAidlImpl:: call IHdmiCec::close DONE %s, result %s\r\n", txn.toString8().string(), closed ? "true" : "false");

		if (!txn.isOk() || !closed) {
			/* The HAL may still hold the address, so the next registration releases it first. */
			if (!logicalAddresses.empty()) {
				unconfirmedReleaseAddress = logicalAddresses.front().toInt();
			}
            status = CLOSED;
			throw IOException();
		}

		/* A successful IHdmiCec::close() removes every added address, so no release is pending. */
		unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
		status = CLOSED;
    }
}

/**
 * @brief Takes the next received frame from the incoming queue, exactly as DriverImpl::read() does.
 *
 * A copy of the legacy body with only the class name changed and no AIDL call: the entry guard,
 * the re-check under the lock when the queue yields the NULL sentinel, and the flush-then-raise.
 *
 * @param [out] frame - The received frame; the flush also writes it, so ignore it after a throw.
 *
 * @pre The driver is OPENED; otherwise InvalidStateException is raised, as it also is after the
 *      flush when a close ends the wait.
 * @warning The flush dereferences every entry it dequeues, as the legacy flush does, so a second
 *          NULL sentinel queued behind the first faults on both back-ends.
 * @see DriverAidlImpl::close()
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
				/* Flush and return */
				while (rQueue.size() > 0) {
					inFrame = rQueue.poll();
					frame = *inFrame;
					delete inFrame;
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
 * @brief Raises OperationNotSupportedException once the legacy prelude and state guard have run.
 *
 * The prelude runs outside the lock and before the state guard, in DriverImpl::writeAsync()'s
 * order, so an empty frame raises from the header decode before the guard is reached.
 *
 * @see DriverAidlImpl::write()
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


/**
 * @brief Transmits a frame through IHdmiCecController::sendMessage() under the instance lock
 *
 * Keeps DriverImpl::write()'s order and status mapping; an undocumented status is logged by
 * number and returns normally, as an unrecognised legacy status does.
 *
 * @param [in] frame - The frame to transmit, header byte first; at most 16 bytes.
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

		/* One arm per documented status; any other value the parcel carries is logged by number
		   and returns normally, as an unrecognised legacy status does. */
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
				/* CEC CTS 9-3-3: a rejected broadcast REPORT_PHYSICAL_ADDRESS raises CECNoAckException,
				   so the caller retries it. */
				if (((frame.at(0) & 0x0F) == 0x0F) && (length > 1) && ((frame.at(1) & 0xFF) == REPORT_PHYSICAL_ADDRESS )) {
					throw CECNoAckException();
				}
				/* Directed ACK_STATE_0 is an acknowledgement, and a broadcast ACK_STATE_0 on
				   any other opcode returns normally, which is what the legacy back-end does. */
				break;

			default:
				/* Only the numeric value is logged: the value is HAL-controlled and must never reach
				   the log as text or as a format string. */
				CCEC_LOG( LOG_EXP, "DriverAidlImpl::write : the HAL reported send status %d, which is not a documented SendMessageStatus value; the transmit returns normally, as the legacy status mapping does\r\n", (int)sendResult);
				break;
		}
    }

    CCEC_LOG( LOG_DEBUG, "Send Completed\r\n");
}

/**
 * @brief Returns entry 0 of a fresh IHdmiCec::getLogicalAddresses() read, or 0 when none is usable.
 *
 * Each no-address outcome writes its own log line. An entry outside 0x0..0xE is rejected on the
 * raw `int32_t`, before LogicalAddress's narrowing conversion could make it look valid.
 *
 * @see HAL_LOGICAL_ADDRESS_MAX
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
			/* Validated raw, before any conversion; see HAL_LOGICAL_ADDRESS_MAX. */
			const int32_t rawAddress = halAddresses[0];

			if (halAddresses.size() > 1) {
				CCEC_LOG( LOG_INFO, "DriverAidlImpl::getLogicalAddress : the HAL reports %zu logical addresses; operating on entry 0 [%d]\r\n", halAddresses.size(), (int)rawAddress);
			}

			if ((rawAddress < HAL_LOGICAL_ADDRESS_MIN) || (rawAddress > HAL_LOGICAL_ADDRESS_MAX)) {
				/* HAL-controlled, so logged only as a %d integer, never as text. */
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
 * @brief Writes FIXED_PHYSICAL_ADDRESS (1.0.0.0) to a non-null out-parameter, in every state.
 *
 * Touches no service proxy and takes no lock, so the answer never waits on an AIDL call
 * another thread holds the lock across.
 *
 * @param [out] physicalAddress - Receives 0x01000000; a null pointer is logged and not written.
 */
void DriverAidlImpl::getPhysicalAddress(unsigned int *physicalAddress)
{
	if (physicalAddress == NULL) {
		CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::getPhysicalAddress : null out parameter, nothing written\r\n");
		return ;
	}

	*physicalAddress = FIXED_PHYSICAL_ADDRESS;

	CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::getPhysicalAddress got physical Address : %x \r\n", *physicalAddress);
}


/**
 * @brief Drops @p source from the local list, then asks the HAL to release it, ignoring a HAL failure.
 *
 * Keeps DriverImpl::removeLogicalAddress()'s shape: state guard, local removal (never rolled
 * back), then the HAL call, whose false result or non-ok status is logged and ignored. The held
 * address is recorded in unconfirmedReleaseAddress before the local removal and cleared only by an
 * ok, true release, so a release that fails or raises is still known to the next add.
 *
 * @param [in] source - The logical address to relinquish.
 * @see DriverAidlImpl::addLogicalAddress()
 * @see DriverImpl::removeLogicalAddress()
 */
void DriverAidlImpl::removeLogicalAddress(const LogicalAddress &source)
{
    {AutoLock lock_(mutex);
		if (status != OPENED) {
			throw InvalidStateException();
		}

		/* Read before the local removal, so a release the HAL does not confirm is still known. */
		const bool wasHeld = (!logicalAddresses.empty() && (logicalAddresses.front() == source)) || (unconfirmedReleaseAddress == source.toInt());

		/* Recorded before anything that can raise, and cleared only by a confirmed release below. */
		if (wasHeld) {
			unconfirmedReleaseAddress = source.toInt();
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

			if (txn.isOk() && removed) {
				if (unconfirmedReleaseAddress == source.toInt()) {
					unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
				}
			}
		}
    }
}

/**
 * @brief Replaces the held logical address with @p source, adding only after a confirmed release.
 *
 * The held address is the local entry, else unconfirmedReleaseAddress. A false or non-ok release
 * is settled by one IHdmiCec::getLogicalAddresses() read: the address counts as released only when
 * that read succeeds without it; otherwise it stays recorded, nothing is added and this raises.
 * @p source is then kept in unconfirmedReleaseAddress until the add is confirmed: success or a
 * refusal clears it, while a non-ok status or a raise keeps it for the next add to settle.
 *
 * @param [in] source - The logical address to register.
 * @return bool - true; every failure raises instead.
 * @see DriverAidlImpl::removeLogicalAddress()
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

		/* Refused before any release, so a request the HAL must reject cannot cost the held address.
		 * toInt() reads one unsigned byte, so only the upper bound can be crossed. */
		if (source.toInt() > HAL_LOGICAL_ADDRESS_MAX) {
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : logical address %d is outside the contract range %d..%d; nothing released or added\r\n", source.toInt(), (int)HAL_LOGICAL_ADDRESS_MIN, (int)HAL_LOGICAL_ADDRESS_MAX);
			throw AddressNotAvailableException();
		}

		if (!logicalAddresses.empty() && (logicalAddresses.front() == source)) {
			CCEC_LOG( LOG_DEBUG, "DriverAidlImpl::addLogicalAddress : logical address %d is already registered\r\n", source.toInt());
			return true;
		}

		/* The list node is allocated before any HAL call, so nothing can fail once the add commits. */
		std::list<LogicalAddress> registered(1, source);

		if (!logicalAddresses.empty() || (unconfirmedReleaseAddress != LogicalAddress::UNREGISTERED)) {
			const int held = logicalAddresses.empty() ? unconfirmedReleaseAddress : logicalAddresses.front().toInt();
			bool removed = false;
			/* Synchronous, with no client-side deadline available: measured, not bounded. */
			const int64_t removeStartedMs = halCallStarted();
			::android::binder::Status removeTxn = hdmiCecController->removeLogicalAddresses(std::vector<int32_t>{ held }, &removed);

			warnIfHalCallSlow("IHdmiCecController::removeLogicalAddresses", removeStartedMs);

			if (!removeTxn.isOk() || !removed) {
				if (!removeTxn.isOk()) {
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : releasing logical address %d failed [%s]; reading back the HAL's addresses\r\n", held, removeTxn.toString8().string());
				}
				else {
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : the HAL declined to release logical address %d; reading back the HAL's addresses\r\n", held);
				}

				/* A refusal can also mean the address was already gone, so the HAL's own list decides. */
				if (hdmiCecService == 0) {
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : no AIDL service proxy is held to confirm the release of logical address %d; nothing added\r\n", held);
					throw IOException();
				}

				std::vector<int32_t> halAddresses;
				/* Synchronous, with no client-side deadline available: measured, not bounded. */
				const int64_t getStartedMs = halCallStarted();
				::android::binder::Status getTxn = hdmiCecService->getLogicalAddresses(&halAddresses);

				warnIfHalCallSlow("IHdmiCec::getLogicalAddresses", getStartedMs);

				if (!getTxn.isOk()) {
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : IHdmiCec::getLogicalAddresses failed [%s]; the release of logical address %d is unconfirmed and nothing was added\r\n", getTxn.toString8().string(), held);
					throw IOException();
				}

				if (std::find(halAddresses.begin(), halAddresses.end(), (int32_t)held) != halAddresses.end()) {
					CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : the HAL still holds logical address %d; nothing added\r\n", held);
					if (!removeTxn.isOk()) {
						throw IOException();
					}
					throw AddressNotAvailableException();
				}
			}

			logicalAddresses.clear();
		}

		/* Nothing is held now. Recorded before the add and cleared only on its confirmed outcome, so an
		 * add that fails or raises is released, or confirmed absent, before any later add. */
		unconfirmedReleaseAddress = source.toInt();

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
			unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
			CCEC_LOG( LOG_EXP, "DriverAidlImpl::addLogicalAddress : the HAL declined logical address %d\r\n", source.toInt());
			throw AddressNotAvailableException();
		}
		else {
			logicalAddresses.splice(logicalAddresses.end(), registered);
			unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;
		}
    }

    return true;
}

/**
 * @brief Reports whether @p source is in the local address list, without a HAL call.
 *
 * A copy of DriverImpl::isValidLogicalAddress(), walking the list under the instance lock.
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
 * @brief Pings @p to by sending the one-byte header frame from @p from through write().
 *
 * A copy of DriverImpl::poll(): a one-byte transmit through write() rather than a getState()
 * query, so the outcome arrives as write()'s exceptions. The trailing `#if 0` block is inert
 * legacy code carried verbatim.
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
 * @brief Returns the incoming frame queue, but only while the driver is open
 *
 * The guard rejects a receive callback arriving during or after a close. `status` is a plain int
 * read without the instance lock, keeping DriverImpl::getIncomingQueue()'s unlocked read and race.
 *
 * @return IncomingQueue& - The queue received frames are offered onto and read() drains.
 * @pre The driver is OPENED; otherwise InvalidStateException is raised.
 * @see DriverAidlImpl::offerReceivedFrame()
 */
DriverAidlImpl::IncomingQueue & DriverAidlImpl::getIncomingQueue(void)
{
	if (status != OPENED) {
		throw InvalidStateException();
	}

	return rQueue;
}

/**
 * @brief Queues a received frame unless the incoming queue is full, and reports which happened.
 *
 * Both producers, this method and close()'s sentinel offer, hold queueProducerMutex, and
 * consumers only remove, so an offer made after a passing room check always lands.
 *
 * @param [in] frame - Heap frame the caller owns; ownership passes only on true.
 *
 * @retval true  - Queued; the queue owns the frame.
 * @retval false - The queue held INCOMING_QUEUE_CAPACITY entries; the caller still owns the frame.
 * @pre The driver is OPENED; otherwise getIncomingQueue() raises InvalidStateException.
 *
 * @see DriverAidlImpl::close()
 * @see DriverAidlImpl::EventListener::onMessageReceived()
 */
bool DriverAidlImpl::offerReceivedFrame(CECFrame *frame)
{
	IncomingQueue &queue = getIncomingQueue();

    {AutoLock lock_(queueProducerMutex);
		/* Refused here when full, because EventQueue::offer() would drop the frame silently. */
		if (queue.size() >= INCOMING_QUEUE_CAPACITY) {
			return false;
		}

		queue.offer(frame);
    }

	return true;
}

/**
 * @brief Logs the initiator, follower, opcode name and bytes of a frame that carries an opcode.
 *
 * A byte-for-byte copy of DriverImpl::printFrameDetails(). The catch names the CCEC
 * `Exception` base, so an empty frame's `std::out_of_range` escapes to the caller, and the bare
 * `\n` terminator is the legacy line's own.
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
 * @brief Returns the one immutable probe that isServiceAvailable() and isBinderPreflightOk() default to.
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
 * @brief Returns the binder protocol version this build expects, or 0 without the binder kernel ABI.
 *
 * The `BINDER_CURRENT_PROTOCOL_VERSION` read sits under the CCEC_HAVE_BINDER_UAPI guard.
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
 * @brief Runs the eight preflight decision points in order and returns false at the first that fails.
 *
 * The coverage manifest gates both arcs of every decision point, so none may be reordered, merged
 * or made build-conditional. Nothing here touches libbinder, since reaching `ProcessState::self()`
 * on a driverless host raises SIGABRT; every check uses the probe.
 *
 * @see DriverAidlImpl::isServiceAvailable()
 * @see DriverAidlImpl::BinderPreflightProbe
 */
bool DriverAidlImpl::isBinderPreflightOk(const std::string &binderDriverPath,
                                         unsigned int contextManagerTimeoutMs,
                                         const BinderPreflightProbe &probe,
                                         int *retainedDescriptor,
                                         BinderNodeIdentity *retainedIdentity)
{
	/* Cleared before any arm can return, so "nothing retained" and a stale value from an earlier
	   call read the same. */
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
		/* errno is captured first. ENOENT (no driver: a legacy-only SOC) is logged at INFO; any other
		   errno is a platform fault for an integrator, logged at WARN with its text. */
		const int openErrno = errno;

		if (openErrno == ENOENT) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: no binder driver node exists at [%s]; this platform carries no binder transport, so the AIDL HAL is absent and the legacy back-end is the correct outcome\r\n", binderDriverPath.c_str());
		}
		else {
			CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] EXISTS but could not be opened, errno %d (%s); treating the AIDL HAL as absent. Unlike a missing node this is a platform fault rather than a legacy-only platform, and it needs an integrator's attention\r\n", binderDriverPath.c_str(), openErrno, strerror(openErrno));
		}

		return false;
	}

	/* Decision point 3 of 8: the object behind the descriptor can be identified. Asked of the
	   descriptor, never the path, so the answer cannot change underneath it. */
	BinderNodeIdentity identity;

	memset(&identity, 0, sizeof(identity));

	if (0 != probe.identifyDescriptor(driverFd, &identity)) {
		/* Captured first, and reported with its text, for the reason stated at decision point 2. */
		const int identifyErrno = errno;

		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] opened but its node could not be identified, errno %d (%s); treating the AIDL HAL as absent. Without an identity there is nothing to re-verify before libbinder opens the same name, so the check could not be made to mean anything\r\n", binderDriverPath.c_str(), identifyErrno, strerror(identifyErrno));
		probe.closeNode(driverFd);
		return false;
	}

	/* Decision point 4 of 8: it is a character device, as every supported binder layout
	   publishes; anything else behind this name is not the binder driver. */
	if ((identity.mode & BINDER_NODE_MODE_TYPE_MASK) != BINDER_NODE_MODE_CHARACTER_DEVICE) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] is not a character device, mode 0%o; treating the AIDL HAL as absent. Every supported binder layout publishes a character device, so this node is not the binder driver and must not be handed to libbinder\r\n", binderDriverPath.c_str(), (unsigned int)(identity.mode & BINDER_NODE_MODE_TYPE_MASK));
		probe.closeNode(driverFd);
		return false;
	}

	/* Decision point 5 of 8: it is owned by root, as devtmpfs and binderfs create it; any other
	   owner means an unprivileged process could have created it. Only the number is logged. */
	if (identity.uid != BINDER_NODE_REQUIRED_OWNER_UID) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] is owned by uid %u rather than %u; treating the AIDL HAL as absent. A binder node not owned by root is one an unprivileged process could have created, so it is refused rather than trusted\r\n", binderDriverPath.c_str(), identity.uid, (unsigned int)BINDER_NODE_REQUIRED_OWNER_UID);
		probe.closeNode(driverFd);
		return false;
	}

	/* An observation, not a decision point: a node writable beyond its owner, as every standard
	   binder node is, is logged at INFO and never refused, since every binder client must open it. */
	if (0u != (identity.mode & (BINDER_NODE_MODE_GROUP_WRITE | BINDER_NODE_MODE_WORLD_WRITE))) {
		CCEC_LOG( LOG_INFO, "DriverAidlImpl preflight: binder driver [%s] is writable beyond its owner, permission bits 0%o; continuing, because a binder node must be openable by every binder client and restrictive modes are NOT a property this middleware can require. On a platform that also leaves service registration unauthorized this is the precondition of HAL impersonation - see the service-authorization prerequisite on isServiceAvailable()\r\n", binderDriverPath.c_str(), (identity.mode & BINDER_NODE_MODE_PERMISSION_MASK));
	}

	/* Decision point 6 of 8: the node will tell us which protocol it speaks. */
	unsigned int protocolVersion = 0;

	if (0 != probe.readProtocolVersion(driverFd, &protocolVersion)) {
		/* Captured first, and reported with its text, for the reason stated at decision point 2. */
		const int ioctlErrno = errno;

		/* WARN, like the failed open and the mismatch: a node that opens but refuses this ioctl is a
		   platform fault, and this line is the only report of the otherwise silent fallback. */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] opened but REFUSED to report its protocol version, errno %d (%s); treating the AIDL HAL as absent. Unlike a missing node this is a PLATFORM FAULT rather than a legacy-only platform: the node exists and opens, so it is either not a binder driver or was mis-created\r\n", binderDriverPath.c_str(), ioctlErrno, strerror(ioctlErrno));
		probe.closeNode(driverFd);
		return false;
	}

	/* Decision point 7 of 8: equality, not a minimum, since libbinder requires an exact match;
	   a mismatch reads as "AIDL absent" here rather than a libbinder abort later. */
	if (protocolVersion != expectedBinderProtocolVersion()) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl preflight: binder driver [%s] speaks protocol %d but this build expects %d; treating the AIDL HAL as absent\r\n", binderDriverPath.c_str(), (int)protocolVersion, (int)expectedBinderProtocolVersion());
		probe.closeNode(driverFd);
		return false;
	}

	/* Clamp before probing so the probe's wait stays bounded; zero is left alone and means "do
	   not wait at all". */
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

	/* Custody: a caller that asked keeps the validated descriptor open, pinning the inode, with
	   its identity; otherwise the descriptor is released and the process is left as found. */
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

/* Fallback-reason phrases, assigned only by isServiceAvailable(). The L1 contract suite matches the
   first and the migration notes quote the first two verbatim, so neither may be reworded alone. */
/** @brief Reason recorded when the preflight or the pre-lookup re-verification declines. */
const char *const REASON_TRANSPORT_UNAVAILABLE = "the binder transport is unavailable on this platform";
/** @brief Reason recorded when no service is registered or halcompat rejects it as incompatible. */
const char *const REASON_NO_COMPATIBLE_SERVICE = "the binder transport is reachable but no compatible service resolved";
/** @brief Reason recorded when the query raises unexpectedly and the catch-all handles it. */
const char *const REASON_QUERY_FAILED          = "the service query failed unexpectedly, so no usable service could be established";

/** @brief Longest interface hash rendered into a log line by sanitizedInterfaceHash(). */
const size_t MAX_LOGGED_HASH_LENGTH = 16;

/* One bit per BinderNodeIdentity attribute, named so the bit order - printed in the divergence
   log line's legend and transcribed by the contract suite - changes in one place only. */
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
 * Scope-bound release, in the `{AutoLock lock_(mutex);` idiom, covers every exit of
 * isServiceAvailable() including an exception unwinding to its catch-all.
 *
 * @warning Not copyable (two holders would close one descriptor twice); the probe, held by
 *          reference, must outlive the holder.
 * @see DriverAidlImpl::isBinderPreflightOk()
 * @see DriverAidlImpl::isServiceAvailable()
 */
class RetainedBinderNode {
public:
	/**
	 * @brief Takes an empty holder bound to the probe that will release the descriptor
	 *
	 * @param [in] probe - The operations the descriptor is opened and released through; must
	 *                     outlive this object.
	 *
	 * @post No descriptor is held until the preflight writes into descriptorSlot().
	 */
	explicit RetainedBinderNode(const DriverAidlImpl::BinderPreflightProbe &probe)
		: probe(probe), descriptor(-1)
	{
	}

	/**
	 * @brief Releases the descriptor if one is still held
	 *
	 * @post Nothing is held, after a normal return or an exception unwinding the enclosing scope.
	 * @warning Never throws: the probe's `closeNode` must not, just as `::close` does not.
	 */
	~RetainedBinderNode()
	{
		release();
	}

	/**
	 * @brief The slot isBinderPreflightOk() writes its retained descriptor into
	 *
	 * @return int* - Address of the held descriptor: -1 after a decline, the validated
	 *                descriptor after a positive verdict.
	 *
	 * @pre Nothing is held yet.
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
	/** @brief Copy construction is disallowed; declared and never defined. */
	RetainedBinderNode(const RetainedBinderNode &);
	/** @brief Copy assignment is disallowed; declared and never defined. */
	RetainedBinderNode &operator=(const RetainedBinderNode &);

	/** @brief The operations the descriptor was opened through, and is released through. */
	const DriverAidlImpl::BinderPreflightProbe &probe;
	/** @brief The held descriptor, or -1 when nothing is held. */
	int descriptor;
};

/**
 * @brief Reports whether a fresh resolution yielded the unchanged validated node
 *
 * Compares all five attributes: `device`, `inode` and `rdev` catch a substituted node, `mode`
 * and `uid` the same inode re-permissioned or re-owned; any change, in either direction, fails.
 *
 * @param [in] validated - The identity the preflight took from its retained descriptor.
 * @param [in] observed  - The identity a fresh resolution of the same path yielded.
 *
 * @return bool - Whether the fresh resolution is the validated node, unchanged
 * @retval true  - All five attributes agree.
 * @retval false - At least one diverged; the divergence is logged, numbers only.
 * @warning Never throws or allocates; the caller turns false into a nonfatal decline.
 * @see reverifyBinderNodeBeforeLookup()
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

	/* One line, emitted here so both re-verification points inherit it: the mask names which
	   attributes moved and the tuples their values, modes in octal and nothing as text. */
	CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder node identity CHANGED between the preflight and the lookup, divergence mask 0x%02x (bit0 device, bit1 inode, bit2 rdev, bit3 mode, bit4 uid); validated dev %llu ino %llu rdev %llu mode 0%o uid %u; observed dev %llu ino %llu rdev %llu mode 0%o uid %u\r\n",
	          divergence,
	          validated.device, validated.inode, validated.rdev, validated.mode, validated.uid,
	          observed.device, observed.inode, observed.rdev, observed.mode, observed.uid);

	return false;
}

/**
 * @brief Re-verifies the binder node and its context manager immediately before the lookup
 *
 * Re-resolves the path and compares it with the validated identity, then pings the context
 * manager under the same bound over a freshly opened, identity-checked descriptor, because the
 * driver allows only one mapping per descriptor.
 *
 * @param [in] binderDriverPath        - The path the preflight validated.
 * @param [in] contextManagerTimeoutMs - Bound on the ping in milliseconds, clamped to the ceiling.
 * @param [in] probe                   - The probe the preflight used.
 * @param [in] validated               - The identity the preflight established.
 *
 * @return bool - Whether the lookup may proceed
 * @retval true  - The path is still the validated node and a context manager answered in time.
 * @retval false - Otherwise; every cause is logged, none throws, and no wait exceeds the ping's bound.
 * @pre The preflight returned true and the caller still holds its descriptor.
 * @post The fresh descriptor is released on every path; the retained one is untouched.
 * @see binderNodeIdentitiesMatch()
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

	/* Re-verification point 2 of 4: the same object in the same state; the comparison itself
	   logs which attributes differ. */
	if (!binderNodeIdentitiesMatch(validated, observed)) {
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder driver [%s] is NOT the node the preflight validated - it resolves to a different filesystem object, or to the same one re-permissioned or re-owned since; declining the AIDL HAL. A node substituted in this window would otherwise be opened by libbinder, which aborts rather than fails on the pinned stack\r\n", binderDriverPath.c_str());
		return false;
	}

	/* Re-verification point 3 of 4: a second descriptor for the ping the retained one cannot
	   carry, opened with the preflight's flags. */
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

	/* Clamped here as well as in the preflight, so neither route exceeds the ceiling. */
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
 * Bytes outside printable ASCII become `?`, and input longer than MAX_LOGGED_HASH_LENGTH is
 * truncated with an ellipsis and its true length.
 *
 * @param [in] hash - The hash exactly as the server reported it: any length, any byte value.
 *
 * @return std::string - A bounded, printable rendering; `<empty>` for an empty hash.
 * @warning Diagnostics only: halcompat::isCompatible() reads the unmodified hash and decides.
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
 * @brief Classifies an observed interface hash as empty, "-1", "notfrozen" or a frozen digest.
 *
 * The phrases say what the observed string is, never why the rejection happened. They follow
 * halcompat's own gate order because halcompat exposes no entry point for a hash alone, and
 * this function decides nothing.
 *
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
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
 * @brief Reports whether an observed hash and version would themselves pass halcompat's rule.
 *
 * The hash half restates halcompat's gate order, rejecting an unfrozen server as the production
 * default does; the version half calls `halcompat::detail::isCompatible()`, so it cannot drift.
 *
 * @note Uses halcompat's internal `detail` namespace deliberately: no public entry point takes an
 *       observed version or a hash, and the verdict itself stays with isCompatible().
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
 * @brief Logs one post-rejection snapshot of a rejected service's hash and version as observations.
 *
 * Reads `getInterfaceHash()`/`getInterfaceVersion()` directly, which halcompat marks internal,
 * because halcompat exposes no accessor for the metadata it read; every line is labelled an
 * observation, not a cause. Being static and catching every failure, it cannot relabel
 * availabilityReason.
 *
 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
 * @see DriverAidlImpl::isServiceAvailable()
 */
void DriverAidlImpl::emitCompatibilityRejectionDiagnostic(
	const ::android::sp< cechal::IHdmiCec > &service,
	const std::string &halServiceName)
{
	try {
		const int clientVersion = cechal::IHdmiCec::VERSION;

		/* One snapshot: each value is read once and never re-read. Both reads are remote round trips
		   to a rejected server, so each is timed and a stall names which one blocked. */
		const int64_t hashReadStartedMs = halCallStarted();
		const std::string observedHash = service->getInterfaceHash();

		warnIfHalCallSlow("IHdmiCec::getInterfaceHash (post-rejection diagnostic read)", hashReadStartedMs);

		const int64_t versionReadStartedMs = halCallStarted();
		const int observedVersion = service->getInterfaceVersion();

		warnIfHalCallSlow("IHdmiCec::getInterfaceVersion (post-rejection diagnostic read)", versionReadStartedMs);

		/* Several short lines because CCEC_LOG truncates silently at 499 bytes; each is sized for its
		   worst-case substitution, so re-check that bound before extending one. */
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
		/* The observation failed: the cause recorded before this call stands, and this static
		   function cannot relabel it. */
		CCEC_LOG( LOG_WARN, "DriverAidlImpl::isServiceAvailable : binder service [%s] is present but was REJECTED AS NOT COMPATIBLE by halcompat::isCompatible(), and its metadata could not be read back afterwards to describe the server, because that read failed too. The rejection is unaffected and its recorded cause unchanged: the AIDL HDMI CEC HAL is treated as absent and the legacy back-end is selected\r\n", halServiceName.c_str());
	}
}


/**
 * @brief Runs the bounded preflight and re-verification, then the service lookup and halcompat check
 *
 * A function-level catch-all turns any exception into a decline, so nothing escapes into the
 * factory's one-time static initializer. Both context-manager probes are bounded; the lookup and
 * every metadata read after them are synchronous binder calls, timed but not bounded.
 *
 * @see DriverAidlImpl::isBinderPreflightOk()
 * @see DriverAidlImpl::open()
 */
bool DriverAidlImpl::isServiceAvailable(const std::string &binderDriverPath,
                                        unsigned int contextManagerTimeoutMs,
                                        const BinderPreflightProbe &probe)
{
	availabilityReason = NULL;

	/* The custody window: declared outside the try so the descriptor is released on every exit,
	   after any handler has run. */
	RetainedBinderNode retainedNode(probe);
	BinderNodeIdentity validatedIdentity;

	memset(&validatedIdentity, 0, sizeof(validatedIdentity));

	try {
		/* Stage 1: the preflight with custody; the validated descriptor stays open, pinning the
		   inode, and its identity is carried to stage 2. */
		if (!isBinderPreflightOk(binderDriverPath, contextManagerTimeoutMs, probe,
		                         retainedNode.descriptorSlot(), &validatedIdentity)) {
			CCEC_LOG( LOG_INFO, "DriverAidlImpl::isServiceAvailable : the binder preflight declined; the AIDL HDMI CEC HAL is treated as absent\r\n");
			availabilityReason = REASON_TRANSPORT_UNAVAILABLE;
			return false;
		}

		/* Stage 2: the same node and a live context manager, right before getService(). A failure
		   is a transport problem, not a service one, and is reported as such. */
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
			/* halcompat::isCompatible() has decided; the cause is recorded first so nothing run
			   afterwards can rewrite it. */
			availabilityReason = REASON_NO_COMPATIBLE_SERVICE;

			/* Report an observation, never a cause. The static helper cannot reach availabilityReason
			   and swallows its own failure, so the catch-all below cannot relabel this rejection. */
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
 * @brief Returns the reason the last isServiceAvailable() recorded, or NULL, without asking again.
 *
 * A pure read of the pointer isServiceAvailable() set: rebuilding the answer would repeat the
 * binder query, and the record is what makes the reported condition the one that actually
 * caused the fallback.
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
