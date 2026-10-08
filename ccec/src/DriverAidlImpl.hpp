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
 * @file DriverAidlImpl.hpp
 *
 * @brief Declaration of the AIDL/binder back-end of the CCEC middleware Driver interface.
 *
 * DriverAidlImpl adapts the out-of-process `com.rdk.hal.hdmicec` AIDL HAL onto the
 * CCEC::Driver contract that DriverImpl implements over the legacy C API, and
 * Driver::getInstance() selects one of the two once, from a runtime query for the
 * `"HdmiCec"` binder service. Like `DriverImpl.hpp` this header is not installed.
 *
 * @note Every AIDL and binder header is included before CCEC_BEGIN_NAMESPACE, so their
 *       namespaces never nest inside CCEC.
 * @warning Requires C++17 and the C++ libbinder AIDL backend, not the NDK backend.
 *
 * @see hdmicec/ccec/src/DriverImpl.hpp
 * @see hdmicec/ccec/include/ccec/Driver.hpp
 * @see AIDL_HAL_MIGRATION_NOTES.md
 */

#ifndef HDMI_CCEC_DRIVER_AIDL_IMPL_HPP_
#define HDMI_CCEC_DRIVER_AIDL_IMPL_HPP_

#include <list>
#include <string>
#include <vector>

/* AIDL and binder headers stay above CCEC_BEGIN_NAMESPACE; the sp<> members need complete types.
 * BnHdmiCecEventListener.h is included only by DriverAidlImpl.cpp, which defines EventListener. */
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <utils/StrongPointer.h>

/* The binder SDK defines LOG_FATAL as a function-like macro; it is dropped here and the CCEC
 * log level restored below, so LOG_FATAL is the CCEC value whatever the include order. */
#undef LOG_FATAL

#include "osal/Mutex.hpp"
#include "osal/EventQueue.hpp"

#include "ccec/Driver.hpp"
#include "ccec/Header.hpp"

/* Restores the CCEC log level, as ccec/Util.hpp defines it, when Util.hpp was included earlier
 * and its include guard will not define it again; an existing definition is never redefined. */
#ifndef LOG_FATAL
/** @brief CCEC fatal log level, identical to the definition in ccec/Util.hpp. */
#define LOG_FATAL 0
#endif

using CCEC_OSAL::EventQueue;
using CCEC_OSAL::Mutex;

CCEC_BEGIN_NAMESPACE

/**
 * @brief AIDL/binder implementation of the CCEC middleware Driver interface.
 *
 * A drop-in sibling of DriverImpl: each Driver virtual keeps the legacy signature, guards,
 * statement order and exceptions, with `com.rdk.hal.hdmicec` AIDL calls in place of the
 * legacy C API. Every observable difference from DriverImpl is documented on its method.
 * Construction touches no binder; binder use begins in isServiceAvailable() or open().
 *
 * @warning Not copyable. One instance is expected, held by Driver::getInstance(), and
 *          received frames reach its incoming queue on a binder threadpool thread.
 *
 * @see DriverImpl - the legacy back-end this class mirrors
 * @see Driver::getInstance() - the single selection point between the two
 */
class DriverAidlImpl : public Driver
{
public:
	/**
	 * @brief Queue of received CEC frames, drained by the Bus reader thread
	 *
	 * Identical to DriverImpl::IncomingQueue. Frames are heap allocated by the
	 * listener, offered here, and taken and deleted by read(). A NULL entry is the
	 * sentinel close() uses to wake a blocked reader.
	 */
	typedef EventQueue<CECFrame *> IncomingQueue;

	/**
	 * @brief Lifecycle state of this back-end
	 *
	 * The same three-state machine DriverImpl runs, with the same values, so that the
	 * state guards on both back-ends are directly comparable. CLOSING exists so that a
	 * receive callback or a blocked reader arriving during teardown is rejected rather
	 * than served.
	 */
	enum {
		CLOSED = 0,
		CLOSING,
		OPENED,
	};

	/**
	 * @brief Default binder driver node inspected by the preflight predicate
	 *
	 * The node libbinder itself opens on this platform. It is exposed as a named
	 * constant, and as the default argument of isBinderPreflightOk(), so that the
	 * production call site names nothing and a test can pass a path of its own.
	 */
	static constexpr const char *DEFAULT_BINDER_DRIVER_PATH = "/dev/binder";

	/**
	 * @brief Default bound, in milliseconds, on resolving the binder context manager
	 *
	 * Obtaining an `IServiceManager` at all polls until binder handle 0 resolves, in
	 * one-second intervals and without an upper bound of its own. This value bounds
	 * the preflight's own probe instead, and is large enough to tolerate one such
	 * interval while keeping LibCCEC::init() bounded on a platform whose
	 * `servicemanager` never started.
	 */
	static constexpr unsigned int DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS = 2000;

	/**
	 * @brief Ceiling, in milliseconds, on a caller-supplied context-manager timeout.
	 *
	 * Keeps each probe's wait bounded whatever timeout a caller names; each clamp is logged.
	 * It is five times DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, so the default is never clamped.
	 *
	 * @warning Must remain greater than or equal to DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS.
	 *
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int MAX_CONTEXT_MANAGER_TIMEOUT_MS = 10000;

	/**
	 * @brief POSIX file-type mask, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of `S_IFMT`. BinderNodeIdentity carries `st_mode` as a plain integer, so this
	 * header compiles where the binder kernel UAPI definitions are absent.
	 *
	 * @warning Restated, not redefined; the contract suite asserts it equals `S_IFMT`.
	 *
	 * @see BinderNodeIdentity
	 * @see BINDER_NODE_MODE_CHARACTER_DEVICE
	 */
	static constexpr unsigned int BINDER_NODE_MODE_TYPE_MASK = 0170000u;

	/**
	 * @brief POSIX character-device file type, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of `S_IFCHR`. The binder node is a character device on every supported
	 * layout (devtmpfs or binderfs), so the preflight refuses anything else at the path.
	 *
	 * @warning Restated, not redefined; the contract suite asserts it equals `S_IFCHR`.
	 *
	 * @see BinderNodeIdentity
	 * @see BINDER_NODE_MODE_TYPE_MASK
	 */
	static constexpr unsigned int BINDER_NODE_MODE_CHARACTER_DEVICE = 0020000u;

	/**
	 * @brief The only owner a binder driver node may have for the AIDL path to be selected
	 *
	 * The kernel creates the node owned by root; any other owner means an unprivileged
	 * process could have created or replaced it, and libbinder aborts on a substituted node.
	 *
	 * @see BinderNodeIdentity
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_REQUIRED_OWNER_UID = 0u;

	/**
	 * @brief POSIX permission-bit mask, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of `ACCESSPERMS`. The preflight uses it only to report the node's permission
	 * bits in a diagnostic, never to reach a verdict.
	 *
	 * @warning Restated, not redefined; the contract suite asserts it equals
	 *          `S_IRWXU | S_IRWXG | S_IRWXO`.
	 *
	 * @see BinderNodeIdentity
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_MODE_PERMISSION_MASK = 0777u;

	/**
	 * @brief POSIX group-write permission, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of `S_IWGRP`. With BINDER_NODE_MODE_WORLD_WRITE it identifies a node writable
	 * beyond its owner, which the preflight reports but does not refuse.
	 *
	 * @warning Restated, not redefined; the contract suite asserts it equals `S_IWGRP`.
	 *
	 * @see BINDER_NODE_MODE_WORLD_WRITE
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_MODE_GROUP_WRITE = 0020u;

	/**
	 * @brief POSIX other-write permission, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of `S_IWOTH`, read together with BINDER_NODE_MODE_GROUP_WRITE; neither is a
	 * requirement placed on the platform.
	 *
	 * @warning Restated, not redefined; the contract suite asserts it equals `S_IWOTH`.
	 *
	 * @see BINDER_NODE_MODE_GROUP_WRITE
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_MODE_WORLD_WRITE = 0002u;

	/**
	 * @brief Capacity of the incoming queue, passed to it explicitly by the constructor.
	 *
	 * Received frames and close()'s NULL sentinel share these 32 slots, as in DriverImpl's queue,
	 * so a sentinel offered to a full queue is dropped, as on the legacy path.
	 *
	 * @note `EventQueue::offer()` drops silently at capacity; offerReceivedFrame() refuses instead.
	 *
	 * @see offerReceivedFrame()
	 * @see IncomingQueue
	 */
	static constexpr size_t INCOMING_QUEUE_CAPACITY = 32;

	/**
	 * @brief Pins the incoming queue's capacity to the legacy queue's, at compile time.
	 *
	 * The literal is the default capacity `CCEC_OSAL::EventQueue` gives DriverImpl's queue.
	 *
	 * @warning If the OSAL default changes, re-derive the literal from the legacy queue.
	 *
	 * @see INCOMING_QUEUE_CAPACITY
	 */
	static_assert(INCOMING_QUEUE_CAPACITY == 32,
	              "the incoming queue must hold the 32 entries, close()'s sentinel included, that "
	              "DriverImpl's queue gets from EventQueue's default capacity");

	/**
	 * @brief Constructs the AIDL back-end without touching binder
	 *
	 * Initializes the state to CLOSED, the legacy handle field to 0 and the local
	 * logical-address list to empty, as DriverImpl::DriverImpl() does. No lookup,
	 * `ProcessState`, threadpool or driver-node access happens, so construction is safe on
	 * a legacy-only SOC.
	 *
	 * @post The instance is inert until isServiceAvailable() or open() is called.
	 * @warning Touches no binder; only ordinary allocation, such as the incoming queue's, can fail.
	 *
	 * @see isServiceAvailable()
	 * @see DriverImpl::DriverImpl()
	 */
	DriverAidlImpl(void);

	/**
	 * @brief Destroys the back-end, closing an open session first
	 *
	 * Mirrors ~DriverImpl(): under the instance lock a non-CLOSED instance is closed and a
	 * close exception is logged, not propagated. The event listener is then detached and
	 * released unconditionally, since a failed close may leave the HAL still calling it.
	 *
	 * @pre None; safe on an instance never opened or whose isServiceAvailable() returned false.
	 * @post No AIDL session is held and no listener holds a pointer to this instance.
	 * @warning Never propagates exceptions; re-entering close() relies on the recursive mutex.
	 *
	 * @see close()
	 * @see EventListener
	 */
	virtual ~DriverAidlImpl();

	/**
	 * @brief Opens the AIDL HDMI CEC session and registers this device's logical address
	 *
	 * Mirrors DriverImpl::open() with `IHdmiCec::open()` in place of `HdmiCecOpen()`: it returns
	 * silently unless the state is CLOSED, and raises IOException when no proxy is held, the open
	 * fails or no controller is returned. Once OPENED it calls registerDeviceLogicalAddress().
	 *
	 * @pre isServiceAvailable() has returned true.
	 * @post The state is OPENED with at most one logical address registered, or IOException was raised.
	 * @warning Synchronous binder calls with no client-side deadline; see isServiceAvailable().
	 * @see registerDeviceLogicalAddress()
	 * @see close()
	 */
	virtual void  open(void) noexcept(false);

	/**
	 * @brief Closes the AIDL HDMI CEC session
	 *
	 * Reproduces DriverImpl::close() in order: a silent return unless OPENED, then CLOSING, the NULL
	 * sentinel, `IHdmiCec::close()`, listener release, CLOSED, and only then IOException if the close
	 * failed or reported false. The local list is kept, as on the legacy path; a failure records its
	 * address in unconfirmedReleaseAddress for the next registration to release.
	 *
	 * @post CLOSED, with no controller or listener held, whether or not IOException is raised.
	 * @warning A synchronous binder call with no client-side deadline; see isServiceAvailable().
	 * @note Pending owner confirmation (B2): IHdmiCec::close() stands in for HdmiCecClose().
	 *
	 * @see open()
	 * @see DriverImpl::close()
	 */
	virtual void  close(void) noexcept(false);

	/**
	 * @brief Takes the next received CEC frame, blocking until one arrives
	 *
	 * Mirrors DriverImpl::read() with no AIDL call, its flush loop included. Throws
	 * InvalidStateException when not OPENED on entry, or after flushing the queue when closed
	 * while blocked.
	 *
	 * @param [out] frame - The received frame; the flush also writes it, so ignore it after a throw.
	 *
	 * @pre open() has completed successfully.
	 * @warning Blocks the Bus reader thread until a frame or the close sentinel arrives.
	 *
	 * @see close()
	 * @see DriverImpl::read()
	 */
	virtual void  read(CECFrame &frame) noexcept(false);

	/**
	 * @brief Transmits a CEC frame synchronously and reports the bus outcome
	 *
	 * Mirrors DriverImpl::write() with `IHdmiCecController::sendMessage()` for `HdmiCecTx()`.
	 * Throws IOException on transport failure, `BUSY`, an over-length frame or no controller;
	 * CECNoAckException on a directed no-ack or the CEC CTS 9-3-3 broadcast arm;
	 * InvalidStateException when not OPENED; `std::out_of_range` for an empty frame.
	 *
	 * @param [in] frame - The frame to transmit, header byte first; at most 16 bytes.
	 *
	 * @warning Frames of 17 to 20 bytes, sendable on legacy, raise IOException here.
	 * @warning Holds the instance lock across a deadline-free binder call; see isServiceAvailable().
	 *
	 * @see DriverImpl::write()
	 */
	virtual void  write(const CECFrame &frame) noexcept(false);

	/**
	 * @brief Not supported on the AIDL back-end; raises after the legacy prelude
	 *
	 * Asynchronous transmit is neither migrated nor emulated, and no production caller
	 * reaches it. The legacy prelude and state guard run first, so an empty frame raises
	 * `std::out_of_range` and a closed driver InvalidStateException, exactly as on legacy;
	 * otherwise OperationNotSupportedException is always raised.
	 *
	 * @param [in] frame - The frame that would have been transmitted; must be non-empty.
	 *
	 * @warning A valid frame on an open driver succeeds on legacy and raises here.
	 *
	 * @see write() - the synchronous path every production caller actually reaches
	 * @see DriverImpl::writeAsync()
	 */
	virtual void  writeAsync(const CECFrame &frame) noexcept(false);

	/**
	 * @brief Relinquishes one logical address
	 *
	 * Mirrors DriverImpl::removeLogicalAddress(): state guard, local removal, then a one-element
	 * `IHdmiCecController::removeLogicalAddresses()` whose failure is logged and ignored. The held
	 * address stays in unconfirmedReleaseAddress until its release is confirmed or settled by an add.
	 *
	 * @param [in] source - The logical address to relinquish.
	 * @pre open() has completed successfully; otherwise InvalidStateException is raised.
	 * @post The address is absent from the local list whether or not the HAL agreed.
	 * @warning A synchronous binder call with no client-side deadline; see isServiceAvailable().
	 * @see addLogicalAddress()
	 * @see DriverImpl::removeLogicalAddress()
	 */
	virtual void  removeLogicalAddress(const LogicalAddress &source);

	/**
	 * @brief Registers @p source as this device's one logical address
	 *
	 * Returns true with no HAL call when @p source is already held. Otherwise the held address is
	 * released first and @p source is added only once the HAL confirms that release. A missing proxy,
	 * non-ok add or unconfirmable release raises IOException; @p source outside 0x0..0xE, a refusal or
	 * a held address the HAL still lists after a declined release raises AddressNotAvailableException.
	 *
	 * @param [in] source - The logical address to register.
	 * @return bool  - Acquisition result
	 * @retval true  - The address is registered; every failure raises instead.
	 * @pre open() has completed successfully; otherwise InvalidStateException is raised.
	 * @post On success only @p source is held. An unconfirmed release keeps the held address; an add
	 *       that fails in transport or raises leaves @p source in unconfirmedReleaseAddress.
	 * @warning Synchronous binder calls with no client-side deadline; see isServiceAvailable().
	 * @see removeLogicalAddress()
	 */
	virtual bool  addLogicalAddress   (const LogicalAddress &source);

	/**
	 * @brief Reads this device's logical address through `IHdmiCec::getLogicalAddresses()`
	 *
	 * Queries the HAL on every call and never answers from the local list. Entry 0 is returned and a
	 * multi-entry result is logged; @p devType is logged only, because open() derives the address
	 * from LOCAL_DEVICE_TYPE.
	 *
	 * @param [in] devType - Accepted for Driver signature compatibility; logged, not used.
	 * @return int    - The logical address in use
	 * @retval 0      - No proxy, a non-ok status, an empty result, an entry outside 0x0..0xE, or 0.
	 * @retval other  - Entry 0 of the HAL's result.
	 * @pre None; there is no state guard, matching DriverImpl::getLogicalAddress().
	 * @warning A non-ok Binder status or no usable address is reported as 0, not raised;
	 *          LibCCEC::getLogicalAddress() turns 0 into InvalidStateException.
	 * @see open()
	 */
	virtual int   getLogicalAddress(int devType);

	/** @brief 1.0.0.0, one nibble per byte, as LibCCEC callers decode it. */
	static constexpr unsigned int FIXED_PHYSICAL_ADDRESS = 0x01000000u;

	/**
	 * @brief Reports the fixed physical address 1.0.0.0.
	 *
	 * @param [out] physicalAddress - Receives FIXED_PHYSICAL_ADDRESS; not written when null.
	 *
	 * @note The AIDL HAL exposes no physical-address query, so the value is fixed and the
	 *       same in every driver state.
	 *
	 * @see DriverImpl::getPhysicalAddress()
	 */
	virtual void  getPhysicalAddress(unsigned int *physicalAddress);

	/**
	 * @brief Reports whether a logical address is one this device holds
	 *
	 * Mirrors DriverImpl::isValidLogicalAddress(): a walk of the local list under the
	 * instance lock, with no HAL call. close() does not clear the list, so this can still
	 * report true after a close.
	 *
	 * @param [in] source - The logical address to test.
	 *
	 * @return bool - Whether the address is held locally
	 * @retval true  - The address is in the local list.
	 * @retval false - It is not.
	 *
	 * @warning Never throws; valid in any state.
	 * @see DriverImpl::isValidLogicalAddress()
	 */
	virtual bool isValidLogicalAddress(const LogicalAddress &source) const;

	/**
	 * @brief Pings a logical address by transmitting a header-only frame
	 *
	 * Mirrors DriverImpl::poll(): `((from & 0x0F) << 4) | (to & 0x0F)` is sent as a one-byte frame
	 * through write(). Throws CECNoAckException when nothing acknowledges (the address is free),
	 * IOException on transmit failure or `BUSY`, and InvalidStateException when not OPENED.
	 *
	 * @param [in] from - Initiator logical address, placed in the high nibble.
	 * @param [in] to   - Follower logical address to ping, placed in the low nibble.
	 *
	 * @warning Inherits write()'s deadline-free binder call and lock; a stall is logged as
	 *          `IHdmiCecController::sendMessage`.
	 *
	 * @see write()
	 * @see DriverImpl::poll()
	 */
	virtual void poll(const LogicalAddress &from, const LogicalAddress &to) noexcept(false);

	/**
	 * @brief Logs a decoded, human-readable rendering of a frame
	 *
	 * Mirrors DriverImpl::printFrameDetails(): pure formatting with no HAL call, logging
	 * every CCEC exception a malformed frame raises. An empty frame raises the uncaught
	 * `std::out_of_range`, so write() and writeAsync() raise it from their prelude.
	 *
	 * @param [in] frame - The frame to render; must be non-empty.
	 *
	 * @see write()
	 * @see DriverImpl::printFrameDetails()
	 */
	virtual void printFrameDetails(const CECFrame &frame) noexcept(false);

	/**
	 * @brief Declared ahead of its definition below, because isServiceAvailable() takes it by reference
	 *
	 * A parameter type, unlike a default argument, is not looked up in the complete-class context.
	 */
	struct BinderPreflightProbe;

	/**
	 * @brief Reports whether a usable, compatible AIDL HDMI CEC service is present
	 *
	 * Asked once by Driver::getInstance(); stops at the first failing stage of isBinderPreflightOk(),
	 * re-verification of that node, the `"HdmiCec"` lookup and the `halcompat` compatibility check.
	 *
	 * @param [in] binderDriverPath        - Binder driver node to inspect; a parameter so tests can vary it.
	 * @param [in] contextManagerTimeoutMs - Bound, in milliseconds, on both context-manager probes.
	 * @param [in] probe                   - Kernel-facing operations; defaults to defaultBinderProbe().
	 *
	 * @return bool - Whether the AIDL back-end should be selected
	 * @retval true  - Every stage passed; the compatible proxy is cached for open().
	 * @retval false - A stage failed; the reason is logged and kept for unavailabilityReason().
	 *
	 * @post The state stays CLOSED and nothing is thrown; each probe is bounded and declines, never aborts.
	 * @warning Past the last probe, libbinder reopens the node and makes the lookup and metadata calls
	 *          with no client-side deadline: the platform must keep the node unchanged, `servicemanager`
	 *          and the HAL answering, and registration of `"HdmiCec"` restricted.
	 * @see isBinderPreflightOk()
	 */
	bool isServiceAvailable(const std::string &binderDriverPath = DEFAULT_BINDER_DRIVER_PATH,
	                        unsigned int contextManagerTimeoutMs = DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
	                        const BinderPreflightProbe &probe = defaultBinderProbe());

	/**
	 * @brief Reports why the last isServiceAvailable() declined, without asking again
	 *
	 * Reads back the recorded reason, so the fallback log names the condition that decided it.
	 *
	 * @return const char* - Why the AIDL back-end is unusable, as a static-storage phrase
	 * @retval NULL     - The query has not run yet, or it returned true.
	 * @retval non-NULL - The phrase naming the stage that declined; safe to hold after return.
	 *
	 * @warning Never throws or touches binder; the coverage runner and L2 tests match the phrases.
	 * @see isServiceAvailable()
	 */
	const char *unavailabilityReason(void) const;

	/**
	 * @brief Identity of the binder node the preflight validated, re-compared before use
	 *
	 * The caller compares all five attributes with a fresh resolution of the path just before
	 * libbinder reopens it, so a node swapped, `chmod`ed or `chown`ed after the check is declined
	 * instead of reaching libbinder's fatal open; this narrows, but cannot close, that window.
	 * Members are plain integers so this header needs no binder UAPI or `<sys/stat.h>` type.
	 *
	 * @warning Internal and test-visible, not public API.
	 * @see isBinderPreflightOk()
	 * @see isServiceAvailable()
	 */
	struct BinderNodeIdentity {
		/** @brief The device the node lives on, POSIX `st_dev`, half of its unique object identity. */
		unsigned long long device;
		/** @brief The node number, POSIX `st_ino`, the other half, pinned by the retained descriptor. */
		unsigned long long inode;
		/** @brief The driver's major/minor pair, POSIX `st_rdev`, meaningful only for a device node. */
		unsigned long long rdev;
		/** @brief POSIX `st_mode`: type validated once, whole value re-compared before use. */
		unsigned int mode;
		/** @brief POSIX `st_uid`: must be BINDER_NODE_REQUIRED_OWNER_UID, re-compared before use. */
		unsigned int uid;
	};

	/**
	 * @brief The six kernel-facing operations the binder preflight performs
	 *
	 * A test seam: substituting them makes every preflight arm reachable without a binder driver.
	 * Plain function pointers keep the real syscalls a compile-time default and binder kernel types
	 * out of this header.
	 *
	 * @warning Internal and test-visible, not public API. Every member must be non-null.
	 * @see defaultBinderProbe()
	 * @see isBinderPreflightOk()
	 */
	struct BinderPreflightProbe {
		/**
		 * @brief Opens the binder driver node; the real implementation wraps `::open`
		 *
		 * @param [in] path  - Node to open.
		 * @param [in] flags - Open flags, `O_RDWR | O_CLOEXEC` in production.
		 *
		 * @return int - Descriptor, or negative on failure with `errno` set.
		 */
		int  (*openNode)(const char *path, int flags);
		/**
		 * @brief Reads the identity of the node an open descriptor refers to (real: `fstat`)
		 *
		 * @param [in]  fd  - Descriptor returned by openNode().
		 * @param [out] out - Receives the identity; written only on success.
		 *
		 * @return int - Outcome
		 * @retval 0  - The identity was written to @p out.
		 * @retval -1 - It could not be read; `errno` carries the reason.
		 *
		 * @pre @p out is non-null.
		 */
		int  (*identifyDescriptor)(int fd, BinderNodeIdentity *out);
		/**
		 * @brief Reads the identity of whatever the path currently resolves to (real: `stat`)
		 *
		 * Follows symlinks, so a binderfs `/dev/binder` symlink yields the node libbinder will open.
		 *
		 * @param [in]  path - Path to resolve, the same one openNode() was given.
		 * @param [out] out  - Receives the identity; written only on success.
		 *
		 * @return int - Outcome
		 * @retval 0  - The identity was written to @p out.
		 * @retval -1 - Not resolvable (`errno` set); treated as declined, never as unchanged.
		 *
		 * @pre @p path and @p out are non-null.
		 */
		int  (*identifyPath)(const char *path, BinderNodeIdentity *out);
		/**
		 * @brief Reads the protocol version the driver reports (real: `ioctl(BINDER_VERSION)`)
		 *
		 * @param [in]  fd      - Descriptor returned by openNode().
		 * @param [out] version - Receives the version; written only on success.
		 *
		 * @return int - Outcome
		 * @retval 0  - The version was read into @p version.
		 * @retval -1 - It could not be read; `errno` carries the reason where one was set.
		 */
		int  (*readProtocolVersion)(int fd, unsigned int *version);
		/**
		 * @brief Asks, under a bounded wait, whether binder handle 0 resolves
		 *
		 * The real implementation sends PING_TRANSACTION to handle 0 and drains under the deadline.
		 *
		 * @param [in] fd        - Descriptor whose protocol version has been verified.
		 * @param [in] timeoutMs - Upper bound in milliseconds; zero means do not wait.
		 *
		 * @return bool - Whether a context manager answered
		 * @retval true  - It answered, so `servicemanager` is reachable.
		 * @retval false - It did not, within the bound.
		 *
		 * @warning One call per descriptor: the driver allows one buffer mapping per descriptor.
		 */
		bool (*pingContextManager)(int fd, unsigned int timeoutMs);
		/**
		 * @brief Releases the descriptor openNode() returned (real: `::close`)
		 *
		 * @param [in] fd - Descriptor to release.
		 *
		 * @return int - Zero on success, negative on failure; the preflight ignores it.
		 */
		int  (*closeNode)(int fd);
	};

	/**
	 * @brief The production probe: real `::open`, `fstat`, `stat`, `BINDER_VERSION`, ping and `::close`
	 *
	 * @return const BinderPreflightProbe& - A single immutable instance with static storage duration.
	 *
	 * @post Nothing is initialized; the probe does no work until called.
	 * @warning Never throws.
	 * @see isBinderPreflightOk()
	 */
	static const BinderPreflightProbe &defaultBinderProbe(void);

	/**
	 * @brief The binder protocol version this build must speak to be usable
	 *
	 * `BINDER_CURRENT_PROTOCOL_VERSION`, which follows `BINDER_IPC_32BIT`: 7 on all-32-bit platforms,
	 * 8 for 32-bit middleware on a 64-bit vendor. A function keeps the kernel UAPI out of this header.
	 *
	 * @return unsigned int - Expected protocol version
	 * @retval 0     - This build carries no binder kernel ABI definitions.
	 * @retval other - The protocol version the linked libbinder was built for.
	 *
	 * @warning Never throws.
	 * @see isBinderPreflightOk()
	 */
	static unsigned int expectedBinderProtocolVersion(void);

private:
	/** @brief The L1 suite's test-only gateway to isBinderPreflightOk(); production never defines it. */
	friend struct BinderPreflightTestAccess;

	/**
	 * @brief Decides whether a binder lookup may safely be attempted at all
	 *
	 * Runs before anything touches libbinder, which aborts on a bad driver node and blocks forever
	 * without a context manager. Stops at the first failure: the node opens, is identifiable, is a
	 * root-owned character device, reports expectedBinderProtocolVersion(), and answers on handle 0.
	 *
	 * @param [in]  binderDriverPath        - Driver node to inspect, injectable for tests.
	 * @param [in]  contextManagerTimeoutMs - Bound on the handle-0 check in ms; 0 means do not wait.
	 * @param [in]  probe                   - Kernel-facing operations; defaults to the real syscalls.
	 * @param [out] retainedDescriptor      - Optional; on true, the validated fd still open, else -1.
	 * @param [out] retainedIdentity        - Optional; on true with @p retainedDescriptor non-null,
	 *                                        the validated node's identity; otherwise untouched.
	 *
	 * @return bool - Whether a binder lookup may safely be attempted
	 * @retval true  - Every check passed.
	 * @retval false - A check failed, or this build lacks the binder ABI; the log names which.
	 *
	 * @post Nothing stays initialized but a retained descriptor, which the caller closes via @p probe.
	 * @warning Private; tests use BinderPreflightTestAccess. Never throws or blocks beyond the bound.
	 * @see isServiceAvailable()
	 */
	static bool isBinderPreflightOk(const std::string &binderDriverPath = DEFAULT_BINDER_DRIVER_PATH,
	                                unsigned int contextManagerTimeoutMs = DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
	                                const BinderPreflightProbe &probe = defaultBinderProbe(),
	                                int *retainedDescriptor = NULL,
	                                BinderNodeIdentity *retainedIdentity = NULL);

public:
	/**
	 * @brief Names the category of a post-rejection interface hash, never the rejection's cause
	 *
	 * @param [in] hash - The hash as the server reported it; may be empty or hold any byte.
	 *
	 * @return const char* - A static, never-NULL phrase describing the category.
	 *
	 * @warning Diagnostics only; public so a host test can cover the wording.
	 * @see observedMetadataWouldBeAccepted()
	 */
	static const char *describeObservedInterfaceHash(const std::string &hash);

	/**
	 * @brief Whether a post-decision metadata snapshot would itself be accepted
	 *
	 * The version half calls `halcompat::detail::isCompatible()`; the hash half mirrors halcompat's
	 * gate order, so an unfrozen server is not accepted.
	 *
	 * @param [in] hash            - The observed interface hash, unmodified.
	 * @param [in] clientVersion   - This client's compiled-in interface version.
	 * @param [in] observedVersion - The server version, from the same snapshot as @p hash.
	 *
	 * @return bool - Whether this snapshot alone would have satisfied the rule
	 * @retval true  - The metadata changed or recovered; never report a version mismatch.
	 * @retval false - Consistent with the rejection, without establishing which rule applied.
	 *
	 * @warning Diagnostics only; never used to select a back-end.
	 * @see describeObservedInterfaceHash()
	 */
	static bool observedMetadataWouldBeAccepted(const std::string &hash,
	                                            int clientVersion,
	                                            int observedVersion);

	/**
	 * @brief Emits the compatibility-rejection diagnostic for a service halcompat rejected
	 *
	 * Derives every statement from one snapshot of the server's hash and version and claims nothing
	 * about which rule rejected it. Kept separate from isServiceAvailable() so tests capture the
	 * exact production wording.
	 *
	 * @param [in] service        - The rejected service; non-null.
	 * @param [in] halServiceName - The binder name it was resolved under, for the log.
	 *
	 * @pre halcompat rejected @p service and `REASON_NO_COMPATIBLE_SERVICE` is already recorded.
	 * @post No member state changes; being static, it cannot relabel the recorded reason.
	 * @warning Diagnostics only; never influences back-end selection.
	 * @see observedMetadataWouldBeAccepted()
	 */
	static void emitCompatibilityRejectionDiagnostic(
		const ::android::sp< ::com::rdk::hal::hdmicec::IHdmiCec > &service,
		const std::string &halServiceName);
/* Protected, not private, so a test-local subclass can reach the receive-queue handoff, its
 * producer lock and address-allocation helpers; nothing in production derives from this class. */
protected:
	/**
	 * @brief Receives the HAL's `oneway` CEC events on a binder threadpool thread
	 *
	 * Defined in the .cpp as a `BnHdmiCecEventListener` subclass. It reaches the queue through
	 * getIncomingQueue() via a nullable, lock-guarded back pointer that every session-ending path
	 * detaches, so a HAL still holding the listener can never reach a destroyed owner.
	 *
	 * @see getIncomingQueue()
	 * @see close()
	 */
	class EventListener;

	/**
	 * @brief Returns the incoming frame queue, but only while the driver is open
	 *
	 * Mirrors DriverImpl::getIncomingQueue() minus its unused native handle; the guard is what
	 * rejects receive callbacks arriving during or after a close.
	 *
	 * @return IncomingQueue& - The queue received frames are offered onto and read() drains.
	 * @pre The driver is OPENED; otherwise InvalidStateException is raised.
	 *
	 * @warning Reads the state without the instance lock, as the legacy accessor does.
	 * @see offerReceivedFrame()
	 */
	IncomingQueue & getIncomingQueue(void);

	/**
	 * @brief Hands one received frame to the incoming queue and reports whether it took it
	 *
	 * `EventQueue::offer()` drops silently when full, so this refuses a frame while the queue holds
	 * INCOMING_QUEUE_CAPACITY entries and otherwise offers it, an offer that then always lands.
	 *
	 * @param [in] frame - Heap frame the caller owns; ownership passes only on true.
	 *
	 * @return bool - Whether ownership was transferred
	 * @retval true  - Queued; the caller must drop its pointer.
	 * @retval false - The queue is full; the caller still owns the frame and must release it.
	 * @pre The driver is OPENED; otherwise InvalidStateException is raised; the caller keeps ownership.
	 *
	 * @warning Serializes producers on queueProducerMutex, never the instance lock; not bounded-time.
	 * @see getIncomingQueue()
	 */
	bool offerReceivedFrame(CECFrame *frame);

	/**
	 * @brief Returns the logical addresses a device of @p deviceType may claim, in allocation order
	 *
	 * The inverse of LogicalAddress::getType(): TV {0}, RECORDING_DEVICE {1, 2, 9}, TUNER
	 * {3, 6, 7, 10}, PLAYBACK_DEVICE {4, 8, 11} and AUDIO_SYSTEM {5}; any other type has none.
	 *
	 * @param [in] deviceType - A DeviceType enumerator.
	 *
	 * @return std::vector<int> - Candidate addresses, first choice first; empty for any other type.
	 *
	 * @see registerDeviceLogicalAddress()
	 */
	static std::vector<int> logicalAddressCandidates(int deviceType);

	/**
	 * @brief Discovers and registers this device's logical address on an opened session
	 *
	 * Releases any address unconfirmedReleaseAddress records (adopted while the HAL still lists it),
	 * then registers the first LOCAL_DEVICE_TYPE candidate poll(c, c) finds free (its poll raised any
	 * exception, CECNoAckException included) with a one-element `addLogicalAddresses()` call. A taken
	 * poll and a HAL refusal move to the next candidate; a non-ok or raising add, or an unreadable
	 * HAL, stops allocation.
	 *
	 * @pre The state is OPENED; the recursive instance lock is taken here.
	 * @post The local list holds the one registered or adopted address, or nothing;
	 *       unconfirmedReleaseAddress names any address whose release or add went unconfirmed.
	 * @warning Every exception but thread cancellation's forced unwind is caught and logged at LOG_EXP.
	 * @see open()
	 */
	void registerDeviceLogicalAddress(void);

	/**
	 * @brief Lifecycle state: one of CLOSED, CLOSING or OPENED
	 *
	 * A plain int, as DriverImpl::status is. Every write happens under the instance mutex, and
	 * getIncomingQueue() reads it without the lock, deliberately reproducing the legacy unlocked read.
	 *
	 * @see getIncomingQueue()
	 */
	int status;
	/** @brief Legacy native handle, always 0; kept so both back-ends declare the same members. */
	int nativeHandle;
	/** @brief Received frames awaiting the Bus reader, and close()'s sentinel; INCOMING_QUEUE_CAPACITY entries. */
	IncomingQueue rQueue;
        /** @brief Guards the state and local address list, and is mutable so const methods may lock it. */
        mutable Mutex mutex;
	/**
	 * @brief Serializes every producer that offers onto the incoming queue
	 *
	 * Held by offerReceivedFrame() for its check and offer, and by close() for its sentinel offer,
	 * so a frame that passed the room check always lands. Separate from the instance mutex, which
	 * write() holds across IPC; close() takes it inside the instance lock and nothing nests the
	 * other way, so no inversion is possible.
	 *
	 * @see offerReceivedFrame()
	 */
	Mutex queueProducerMutex;
	/**
	 * @brief The local record of the one logical address this back-end has registered
	 *
	 * Declared as DriverImpl declares it, but never holds more than one entry: a replacement is
	 * recorded only once the HAL has confirmed the old address released, and close() leaves the
	 * entry for the next open() to replace. isValidLogicalAddress() answers from this list alone.
	 */
	std::list<LogicalAddress> logicalAddresses;

	/**
	 * @brief The top-level AIDL service proxy, cached by isServiceAvailable()
	 *
	 * Supplies open(), close() and getLogicalAddresses(). Null until a successful
	 * isServiceAvailable(), and required to be non-null by open().
	 */
	android::sp< ::com::rdk::hal::hdmicec::IHdmiCec> hdmiCecService;
	/**
	 * @brief The controller session proxy returned by `IHdmiCec::open()`
	 *
	 * Supplies addLogicalAddresses(), removeLogicalAddresses() and sendMessage(). Null
	 * outside an open session, which is why those operations are state guarded. The
	 * split across two interfaces is the reason both proxies are held.
	 */
	android::sp< ::com::rdk::hal::hdmicec::IHdmiCecController> hdmiCecController;
	/**
	 * @brief The listener handed to `IHdmiCec::open()`, held for the session's lifetime
	 *
	 * Created by each open() and released by close(), open()'s failure arms and the destructor, each
	 * of which detaches it first so a HAL still holding a reference cannot reach this instance.
	 */
	android::sp<EventListener> eventListener;

	/**
	 * @brief Why the last isServiceAvailable() declined, or NULL
	 *
	 * Set on every exit path of isServiceAvailable() and read through unavailabilityReason(). Only
	 * ever NULL (the constructor's value) or a string literal, so nothing is owned.
	 */
	const char *availabilityReason;

private:
	/**
	 * @brief Copy construction is not allowed; declared and never defined, as in DriverImpl
	 *
	 * @param [in] other - Unused.
	 */
	DriverAidlImpl(const DriverAidlImpl &other); /* Not allowed */

	/**
	 * @brief Copy assignment is not allowed; declared and never defined, as in DriverImpl
	 *
	 * @param [in] other - Unused.
	 *
	 * @return DriverAidlImpl& - Never returns; the declaration is never defined.
	 */
	DriverAidlImpl & operator = (const DriverAidlImpl &other); /* Not allowed */

	/**
	 * @brief The CEC device type whose logical address open() discovers and registers
	 *
	 * The one place a product sets its device role on the AIDL back-end.
	 */
	static constexpr int LOCAL_DEVICE_TYPE = DeviceType::PLAYBACK_DEVICE;

	/**
	 * @brief An address the HAL may still hold although the local list does not, or UNREGISTERED
	 *
	 * Set before each HAL call that could leave an address registered without a local entry: the
	 * release of the held address by removeLogicalAddress(), and every add. A failed close() also
	 * records the held address. Cleared once the HAL confirms the outcome or by a successful close();
	 * addLogicalAddress() and registerDeviceLogicalAddress() settle it before any add.
	 */
	int unconfirmedReleaseAddress = LogicalAddress::UNREGISTERED;

};

CCEC_END_NAMESPACE

#endif


/** @} */
/** @} */
