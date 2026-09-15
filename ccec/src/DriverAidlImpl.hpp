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
 * @brief Declaration of the AIDL/binder back-end of the CCEC middleware Driver interface
 *
 * This is the second of the two CCEC::Driver implementations the middleware ships.
 * It adapts the out-of-process `com.rdk.hal.hdmicec` AIDL HAL onto exactly the same
 * CCEC::Driver contract that DriverImpl implements over the legacy in-process C ABI
 * (`libRCECHal.so`).@n
 * Exactly one of the two is selected once, during initialization, by
 * Driver::getInstance(), on the basis of a runtime query for the `"HdmiCec"` binder
 * service. The selection is then stable for the lifetime of the process. Nothing
 * above the Driver seam changes: the frame queue, the Bus reader and writer threads,
 * Connection, FrameListener and LibCCEC are untouched, and only the thread that
 * produces received frames into the incoming queue differs between the two
 * back-ends.
 *
 * This header is deliberately not an installed public header. It is absent from the
 * `nobase_include_HEADERS` list in `hdmicec/Makefile.am`, exactly as `DriverImpl.hpp`
 * is, which is what allows a second back-end to exist without altering the middleware
 * public API. Production code reaches it only from `ccec/src`; test translation units
 * reach it through the established relative route
 * `#include "../../../ccec/src/DriverAidlImpl.hpp"`.
 *
 * @note Include-order contract, and it is load bearing: every AIDL and binder header
 *       is included before CCEC_BEGIN_NAMESPACE. The generated stubs open
 *       `namespace com::rdk::hal::hdmicec` and transitively pull the `binder/` and
 *       `utils/` SDK headers that open `namespace android`. Including any of them
 *       after CCEC_BEGIN_NAMESPACE would nest those declarations as
 *       `CCEC::com::rdk::hal::hdmicec` and `CCEC::android`, which breaks the link and
 *       silently violates the one-definition rule.
 * @warning This header requires C++17. The generated AIDL stubs include `<optional>`
 *          and `IHdmiCec::getProperty()` takes a `std::optional<PropertyValue>*`, so a
 *          C++14 translation unit cannot compile this file.
 * @warning This is the C++ libbinder AIDL backend, not the NDK backend: the pointer
 *          type is `android::sp<>`, the status type is `android::binder::Status`, and
 *          the server bases are the generated `Bn*` classes.
 *
 * @see hdmicec/ccec/src/DriverImpl.hpp
 * @see hdmicec/ccec/include/ccec/Driver.hpp
 */

#ifndef HDMI_CCEC_DRIVER_AIDL_IMPL_HPP_
#define HDMI_CCEC_DRIVER_AIDL_IMPL_HPP_

/*
 * <atomic> is required by the lifecycle state member, which is an std::atomic<int> so that
 * the one guard read taken without the instance mutex - getIncomingQueue()'s, reached from a
 * binder threadpool thread - is not a data race. The member's own documentation carries the
 * full reasoning.
 */
#include <atomic>
#include <list>
#include <string>

/*
 * AIDL and binder headers. These must stay above CCEC_BEGIN_NAMESPACE - see the
 * include-order note in the file block above. Both interface headers are required at
 * header level rather than forward declared, because the android::sp<> session members
 * below are complete-type class members.
 *
 * <com/rdk/hal/hdmicec/BnHdmiCecEventListener.h> is deliberately not included here.
 * The nested EventListener is forward declared below and defined in DriverAidlImpl.cpp,
 * which keeps the listener's server-side base out of every translation unit that merely
 * includes this header.
 */
#include <com/rdk/hal/hdmicec/IHdmiCec.h>
#include <com/rdk/hal/hdmicec/IHdmiCecController.h>
#include <utils/StrongPointer.h>

/*
 * LOG_FATAL reconciliation, required because two independent definitions collide.
 *
 * The binder SDK's log/log_main.h defines LOG_FATAL as a function-like macro, guarded
 * by #ifndef. ccec/Util.hpp defines it, unguarded, as the integer log level 0, and
 * CCEC code consumes it as a value - CCEC_LOG(LOG_FATAL, ...). Left alone the
 * two orders behave differently: binder-then-ccec merely warns about a redefinition,
 * while ccec-then-binder leaves the function-like macro in force and turns any later
 * use of LOG_FATAL as a value into a hard compile error.
 *
 * Dropping the binder spelling here and restoring the CCEC value below makes this
 * header order independent: whichever way round a translation unit includes things,
 * LOG_FATAL ends up as the CCEC log level, with no diagnostic. Nothing in the binder
 * SDK headers consumes a bare LOG_FATAL - they use LOG_FATAL_IF and LOG_ALWAYS_FATAL,
 * which are separately defined and untouched here - so removing it costs nothing.
 */
#undef LOG_FATAL

#include "osal/Mutex.hpp"
#include "osal/EventQueue.hpp"

#include "ccec/Driver.hpp"
#include "ccec/Header.hpp"

/*
 * Restores the CCEC log level when ccec/Util.hpp was already included by this
 * translation unit and its include guard therefore skipped the block above. The value
 * is identical to the one ccec/Util.hpp defines, and this never redefines an existing
 * definition.
 */
#ifndef LOG_FATAL
/** @brief CCEC fatal log level, identical to the definition in ccec/Util.hpp. */
#define LOG_FATAL 0
#endif

using CCEC_OSAL::EventQueue;
using CCEC_OSAL::Mutex;

CCEC_BEGIN_NAMESPACE

/**
 * @brief AIDL/binder implementation of the CCEC middleware Driver interface
 *
 * DriverAidlImpl is a drop-in sibling of DriverImpl. It implements every CCEC::Driver
 * virtual with the same signature, the same guards, the same statement order and the
 * same exceptions, substituting the `com.rdk.hal.hdmicec` AIDL calls for the legacy
 * HDMI CEC C API. Where the legacy implementation contains something that reads like a
 * defect and would ordinarily be corrected - the `#if 0`'d throws in open() and
 * close(), getLogicalAddress() ignoring its `devType` argument, removeLogicalAddress()
 * discarding the HAL return, close() leaving the local address list populated, or
 * writeAsync() doing frame work before its state guard - this class reproduces the
 * observable behaviour rather than the improved one, because callers and the existing
 * test suite depend on it.
 *
 * Three observable differences from the legacy back-end are authorized, each forced by
 * the AIDL contract and each documented on the method that carries it: the 16-byte
 * frame limit enforced by write(), the OperationNotSupportedException raised by
 * writeAsync(), and the coarser failure category reported by addLogicalAddress().
 *
 * Two further differences are DELIVERED AND REGISTERED WITHOUT BEING AUTHORIZED, which is
 * stated here so that the authorized list above is not read as the complete set. Each is a
 * defensive guard the AIDL transport makes necessary, each is documented in full on the
 * method that carries it, and each is registered for the specification owner rather than
 * silently adopted:
 * - getLogicalAddress() reports no address when the HAL names one outside the AIDL
 *   contract range 0x0..0xE, where the legacy back-end returns whatever the HAL wrote;
 * - write() raises IOException on a `SendMessageStatus` value outside the three documented
 *   enumerators, where the legacy back-end treats an unrecognised status as success.
 * NEITHER IS REACHABLE UNLESS THE HAL VIOLATES ITS OWN CONTRACT - one requires an address
 * the interface forbids, the other an enumerator it does not define - and in both cases
 * the alternative is to carry the malformed value onward: an out-of-contract logical
 * address into the frame headers the middleware builds from it, or a transmit the HAL
 * declined reported to the caller as delivered. A third registration, of a deliberate
 * departure from the plan's byte-identical directive rather than of a behaviour change,
 * sits on read().
 *
 * The incoming queue's reserved slot is deliberately NOT on that list, and the reason is
 * arithmetic rather than argument: the queue is constructed at 33 entries and
 * offerReceivedFrame() refuses at one below, so received frames fill 32 - exactly what the
 * legacy queue's OSAL default accepts. The reserve buys the sentinel a slot that no
 * received frame can occupy without costing any receive depth, so there is no observable
 * difference to register. See INCOMING_QUEUE_CAPACITY, where the arithmetic and the
 * consequence of lowering it are stated together.
 *
 * Any observable difference that is neither authorized above nor registered on its method
 * is a defect.
 *
 * The AIDL surface is split across two interfaces and only nine of its thirteen
 * methods are consumed. `IHdmiCec` supplies open(), close() and getLogicalAddresses();
 * `IHdmiCecController`, obtained from open(), supplies addLogicalAddresses(),
 * removeLogicalAddresses() and sendMessage(); the listener supplies
 * onMessageReceived(), onStateChanged() and onMessageSent(). `getState()`,
 * `getProperty()`, `registerEventListener()` and `unregisterEventListener()` are
 * deliberately not consumed: the middleware keeps its own state machine, the HAL
 * properties have no legacy counterpart, and this back-end is the controlling client
 * and so receives events through the listener it hands to open().
 *
 * Construction touches no binder at all, which is what makes the factory's
 * construct-then-query shape safe on a legacy-only SOC. Every binder interaction
 * begins either in isServiceAvailable() or in open().
 *
 * @warning Instances are not copyable. Exactly one is expected to exist, as the
 *          function-local static held by Driver::getInstance().
 * @warning The receive path delivers on a binder threadpool thread, so the incoming
 *          queue is written from a thread this class does not own. That queue is the
 *          existing cross-thread synchronization point and needs no change.
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
	 * @brief Ceiling, in milliseconds, that a caller-supplied context-manager timeout is
	 *        clamped to
	 *
	 * isBinderPreflightOk() takes its timeout as an `unsigned int`, so a caller - a test, or
	 * a future platform-integration knob - can name a value far larger than any plausible
	 * `servicemanager` start-up delay, and every millisecond of it is time LibCCEC::init()
	 * would spend blocked. The parameter is therefore clamped to this ceiling, and the
	 * clamp is logged so a mis-set value is visible rather than silently obeyed.@n
	 * It is an order of magnitude above DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, so the default
	 * is never clamped and no legitimate integration value is truncated, while an
	 * initialization stall stays bounded by a figure a human can reason about.
	 *
	 * @warning Must remain greater than or equal to DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS, or
	 *          the default itself would be clamped and the default path would log on every
	 *          start-up.
	 *
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int MAX_CONTEXT_MANAGER_TIMEOUT_MS = 10000;

	/**
	 * @brief POSIX file-type mask, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of POSIX `S_IFMT`, written out here for exactly the reason the protocol
	 * version crosses BinderPreflightProbe as a plain `unsigned int`: this header must
	 * still compile where the binder kernel UAPI definitions are absent, so
	 * BinderNodeIdentity carries `st_mode` as a plain integer and the file-type bits are
	 * extracted with a constant of our own rather than with a macro from a header this
	 * file does not include.
	 *
	 * @warning It is restated, NOT redefined: the value is fixed by POSIX and by the Linux
	 *          ABI, and the contract suite asserts this constant equals `S_IFMT` so that
	 *          the restatement cannot drift unnoticed.
	 *
	 * @see BinderNodeIdentity
	 * @see BINDER_NODE_MODE_CHARACTER_DEVICE
	 */
	static constexpr unsigned int BINDER_NODE_MODE_TYPE_MASK = 0170000u;

	/**
	 * @brief POSIX character-device file type, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of POSIX `S_IFCHR`. The binder driver node is a character device on every
	 * supported layout - a devtmpfs node under `/dev`, or a binderfs node under
	 * `/dev/binderfs` reached directly or through a symlink - so anything else behind the
	 * configured path is not the binder driver, whatever it is named, and the preflight
	 * refuses it.
	 *
	 * @warning Restated rather than redefined, and cross-checked against `S_IFCHR` by the
	 *          contract suite for the same reason as BINDER_NODE_MODE_TYPE_MASK.
	 *
	 * @see BinderNodeIdentity
	 * @see BINDER_NODE_MODE_TYPE_MASK
	 */
	static constexpr unsigned int BINDER_NODE_MODE_CHARACTER_DEVICE = 0020000u;

	/**
	 * @brief The only owner a binder driver node may have for the AIDL path to be selected
	 *
	 * The binder driver node is created by the kernel through devtmpfs, or by mounting
	 * binderfs, and in both cases it is owned by root. A node at the configured path owned
	 * by anyone else is a node some unprivileged process was able to create or replace,
	 * which is the precondition of the substitution the identity checks close - and on the
	 * pinned stack the consequence of handing libbinder a substituted node is an abort, not
	 * an error return.
	 *
	 * @see BinderNodeIdentity
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_REQUIRED_OWNER_UID = 0u;

	/**
	 * @brief POSIX permission-bit mask, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of POSIX `ACCESSPERMS`, i.e. `S_IRWXU | S_IRWXG | S_IRWXO`, restated for
	 * exactly the reason BINDER_NODE_MODE_TYPE_MASK is: BinderNodeIdentity carries
	 * `st_mode` as a plain integer and this header must still compile where the binder
	 * kernel UAPI definitions are absent.
	 *
	 * USED FOR REPORTING, NEVER FOR A VERDICT. The preflight extracts these bits solely to
	 * put them in a diagnostic - see the note on isBinderPreflightOk() about why node
	 * permission restrictiveness is not something this middleware can require.
	 *
	 * @warning Restated rather than redefined, and cross-checked against
	 *          `S_IRWXU | S_IRWXG | S_IRWXO` by the contract suite for the same reason as
	 *          BINDER_NODE_MODE_TYPE_MASK.
	 *
	 * @see BinderNodeIdentity
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_MODE_PERMISSION_MASK = 0777u;

	/**
	 * @brief POSIX group-write permission, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of POSIX `S_IWGRP`. Paired with BINDER_NODE_MODE_WORLD_WRITE to recognize a
	 * node writable beyond its owner, which the preflight OBSERVES and REPORTS and does not
	 * refuse.
	 *
	 * @warning Restated rather than redefined, and cross-checked against `S_IWGRP` by the
	 *          contract suite.
	 *
	 * @see BINDER_NODE_MODE_WORLD_WRITE
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_MODE_GROUP_WRITE = 0020u;

	/**
	 * @brief POSIX other-write permission, restated so this header needs no `<sys/stat.h>`
	 *
	 * The value of POSIX `S_IWOTH`. See BINDER_NODE_MODE_GROUP_WRITE; the two are read
	 * together and neither is a requirement placed on the platform.
	 *
	 * @warning Restated rather than redefined, and cross-checked against `S_IWOTH` by the
	 *          contract suite.
	 *
	 * @see BINDER_NODE_MODE_GROUP_WRITE
	 * @see isBinderPreflightOk()
	 */
	static constexpr unsigned int BINDER_NODE_MODE_WORLD_WRITE = 0002u;

	/**
	 * @brief Number of received frames the incoming queue holds before it stops accepting
	 *
	 * The value CCEC_OSAL::EventQueue would have defaulted to, named here and passed to
	 * the queue explicitly by the constructor, so that the capacity this class reasons
	 * about and the capacity the queue enforces are the same number by construction rather
	 * than by coincidence. A future change to the OSAL default cannot silently desynchronize
	 * them.@n
	 * It matters because `EventQueue::offer()` returns void and drops its argument
	 * silently when the queue is at capacity. The receive path therefore has to establish
	 * for itself that there is room before it parts with ownership of a frame - see
	 * offerReceivedFrame().@n
	 * The last of these slots is reserved for close()'s NULL sentinel: the receive path
	 * refuses at one below this number, so the wake-the-reader offer can never be the one
	 * the queue swallows. The depth available to received frames is therefore this value
	 * minus one, which is 32 - the same number the legacy queue's OSAL default accepts.
	 *
	 * @note THIS IS WHY THE VALUE IS 33 AND NOT 32, AND THE ARITHMETIC IS THE WHOLE POINT.
	 *       DriverImpl leaves its own queue on the OSAL default of 32 and lets received
	 *       frames fill all 32, which is why its close() sentinel can be the offer
	 *       `EventQueue::offer()` silently discards - an unfixed legacy liveness defect
	 *       this back-end does not reproduce. Reserving a slot out of 32 would have fixed
	 *       that defect at the cost of a caller-observable difference: received frames
	 *       would fill 31 where legacy fills 32, so a sustained receive burst would drop
	 *       one event earlier on this back-end than on the legacy one. Sizing the queue
	 *       one larger instead buys the reserve without paying for it. Received frames
	 *       fill @b 32 - exactly what legacy accepts - and the sentinel still has a slot
	 *       nothing else can take. There is no observable difference to register, and the
	 *       cost is one pointer-sized slot that only ever holds the sentinel.
	 *
	 * @see offerReceivedFrame()
	 * @see IncomingQueue
	 */
	static constexpr size_t INCOMING_QUEUE_CAPACITY = 33;

	/**
	 * @brief Pins the receive depth to the legacy contract, at compile time
	 *
	 * The parity this class claims against DriverImpl is a property of the ARITHMETIC
	 * between two independent facts - this capacity, and offerReceivedFrame()'s refusal at
	 * one below it - and neither fact states the depth on its own. That makes the depth
	 * silently editable: lowering the capacity to 32 while leaving the refusal where it is
	 * takes the reserved slot back out of the caller's share and restores the difference,
	 * and no test can catch it, because every case that exercises the reserve derives its
	 * own expectations from INCOMING_QUEUE_CAPACITY and would simply expect the smaller
	 * number.
	 *
	 * So the depth is pinned here to a LITERAL, deliberately not to an expression over the
	 * constant above, and the number is not arbitrary: 32 is the capacity
	 * `CCEC_OSAL::EventQueue`'s own default constructor gives the legacy back-end, at
	 * `EventQueue(size_t cap = 32)` in `osal/include/osal/EventQueue.hpp`, and legacy lets
	 * received frames fill all of it. Anything that changes the depth available to received
	 * frames on this back-end therefore fails to compile rather than shipping, whichever
	 * side of the arithmetic is edited.
	 *
	 * @warning If the OSAL default is ever changed, this is the assertion that must be
	 *          re-derived, and it should move with the same reasoning rather than simply
	 *          being relaxed: the number to pin is whatever depth the legacy receive path
	 *          gets, not whatever this back-end happens to give.
	 * @see INCOMING_QUEUE_CAPACITY
	 * @see offerReceivedFrame()
	 */
	static_assert(INCOMING_QUEUE_CAPACITY - 1 == 32,
	              "the depth available to received frames must equal the 32 the legacy back-end "
	              "gets from EventQueue's default capacity; the reserved slot for close()'s "
	              "sentinel must come out of an extra entry, never out of the caller's share");

	/**
	 * @brief Constructs the AIDL back-end without touching binder
	 *
	 * Initializes the state to CLOSED, the legacy handle field to 0 and the local
	 * logical-address list to empty, exactly as DriverImpl::DriverImpl() does. No
	 * service lookup, no `ProcessState`, no threadpool and no driver-node access
	 * happen here.@n
	 * This is deliberate and it is what the selection order requires: both back-ends
	 * are always constructed, and only then is the AIDL one asked whether its service
	 * came up. A constructor that reached for binder would abort the process on a
	 * legacy-only SOC before the fallback could ever be taken.
	 *
	 * @post The instance is inert until isServiceAvailable() or open() is called.
	 * @warning Never throws, so that the enclosing function-local static in
	 *          Driver::getInstance() cannot fail to initialize.
	 *
	 * @see isServiceAvailable()
	 * @see DriverImpl::DriverImpl()
	 */
	DriverAidlImpl(void);

	/**
	 * @brief Destroys the back-end, closing an open session first
	 *
	 * Mirrors ~DriverImpl(): under the instance lock, a state other than CLOSED is
	 * closed through this class's own close(), and an exception escaping that close is
	 * caught and logged rather than propagated out of the destructor.@n
	 * One obligation is added that the legacy destructor has no need of, because the
	 * legacy back-end has no listener object: the event listener is detached and released
	 * unconditionally, after the close attempt. Swallowing the close exception is
	 * required of a destructor, but a swallowed close failure is exactly the case in
	 * which the HAL may still hold - and still call - the listener, so the detach cannot
	 * be left to the close that just failed.
	 *
	 * @pre None. Safe on an instance that was never opened, and safe on one whose
	 *      isServiceAvailable() returned false.
	 * @post No AIDL session is held and no listener holds a pointer to this instance, on
	 *       every path through this destructor.
	 * @warning Does not propagate exceptions.
	 * @warning Takes the instance lock and then calls close(), which takes it again. That
	 *          is sound because CCEC_OSAL::Mutex is recursive, and it is the legacy
	 *          destructor's own shape, preserved rather than tidied.
	 * @warning The detach takes the listener's lock while this destructor holds the
	 *          instance lock. That nesting is one-directional: a callback never acquires
	 *          the instance lock, because it reaches this object only through
	 *          getIncomingQueue(), which takes no lock at all.
	 *
	 * @see close()
	 * @see EventListener
	 */
	virtual ~DriverAidlImpl();

	/**
	 * @brief Opens the AIDL HDMI CEC session and begins receiving CEC messages
	 *
	 * Reproduces DriverImpl::open() with `IHdmiCec::open()` substituted for
	 * `HdmiCecOpen()`. A call made while the state is not CLOSED returns silently: the
	 * `throw InvalidStateException()` in DriverImpl::open() is `#if 0`'d out, so a silent
	 * return is the observable legacy behaviour and is what this back-end must
	 * reproduce.@n
	 * On the way through, the process binder threadpool is started so that the `oneway`
	 * listener callbacks have a thread to arrive on, the nested EventListener is handed
	 * to `IHdmiCec::open()`, and the `IHdmiCecController` it returns is retained for the
	 * transmit and logical-address operations. The state then becomes OPENED.@n
	 * The null-controller arm is contract rather than defence: `IHdmiCec.open()` is
	 * declared `@nullable` and documented as returning null on error, so an ok status
	 * carrying no controller is reachable and must not be mistaken for success.@n
	 * A fresh listener is constructed per session. Every path that ends a session -
	 * both arms of close(), both failure arms here, and the destructor - detaches and
	 * releases the listener, so the next open() finds none and builds one. A listener
	 * reused across sessions would be one the previous session's HAL may still hold a
	 * strong reference to, which is the use-after-free window detachment closes.
	 *
	 * @throws IOException - No compatible service proxy is held, or the
	 *                       `IHdmiCec::open()` transaction reported a non-ok binder
	 *                       status, or it succeeded and returned a null controller. This
	 *                       mirrors the way DriverImpl::open() maps an `HdmiCecOpen()`
	 *                       failure. In the latter two cases the listener is detached and
	 *                       released before the exception leaves, because a failed open
	 *                       may still have left the HAL holding it.
	 *
	 * @pre isServiceAvailable() has returned true, so a compatible `IHdmiCec` proxy is
	 *      held. Calling open() without that is an IOException, not a crash.
	 * @post The state is OPENED and a controller session is held - which close() must
	 *       release - or nothing is held at all and IOException was raised.
	 * @warning Not idempotent HAL-side. `IHdmiCec.open()` admits a single controlling
	 *          client and fails with `EX_ILLEGAL_STATE` when a session is already open,
	 *          which is why this class runs the same CLOSED/CLOSING/OPENED machine the
	 *          legacy back-end runs.
	 *
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. The AIDL call this makes
	 *          is a SYNCHRONOUS binder transaction with NO client-side deadline, so a HAL
	 *          that does not answer blocks this call for as long as it chooses. The
	 *          elapsed time is measured and a stall past the threshold is reported naming
	 *          this exact method, and that report is the whole of what the middleware can
	 *          do. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable().
	 * @see close()
	 * @see DriverImpl::open()
	 */
	virtual void  open(void) noexcept(false);

	/**
	 * @brief Closes the AIDL HDMI CEC session
	 *
	 * Reproduces DriverImpl::close() step for step. A call made while the state is not
	 * OPENED returns silently, for the same `#if 0` reason open() does. Otherwise the
	 * state becomes CLOSING, the NULL sentinel is offered onto the incoming queue so a
	 * blocked reader wakes and unwinds, the HAL session is released, the listener is
	 * detached and released, the state becomes CLOSED, and only then is a failure
	 * reported - so a caller that swallows the exception still sees a consistently
	 * closed object. The sentinel is offered before the HAL transaction, which is the
	 * observable legacy order.@n
	 * The local logical-address list is deliberately not cleared. DriverImpl::close()
	 * does not clear it either, so isValidLogicalAddress() can remain true across a
	 * close on the legacy path, and the HAL removes the addresses on its own side.
	 * Clearing here would be an unauthorized improvement.
	 *
	 * @throws IOException - The close transaction reported a non-ok binder status, or
	 *                       reported success with a false result. The state is already
	 *                       CLOSED when this is raised, matching the order in which
	 *                       DriverImpl::close() sets the state before raising on an
	 *                       `HdmiCecClose()` failure.
	 *
	 * @pre None. Safe to call on a closed instance, which returns silently.
	 * @post The state is CLOSED, no controller is held, and the listener is detached and
	 *       released whether or not the HAL reported a successful close. The local
	 *       address list is untouched, by design.
	 * @warning The detach is placed before the failure check and must stay there. A
	 *          failed `IHdmiCec::close()` leaves the HAL entitled to keep calling the
	 *          listener, since the AIDL no-further-callbacks guarantee attaches only to a
	 *          close that succeeded; detaching only on the success arm would leave a
	 *          listener holding a back pointer into an owner that is about to be
	 *          destroyed, which is CWE-416 by construction.
	 * @warning Blocked item B2. `IHdmiCec.close()` is a high-confidence candidate for
	 *          the legacy `HdmiCecClose()`, pending confirmation by the HAL mapping-table
	 *          owners: the mapping table carries no entry for `HdmiCecClose()`, and the
	 *          candidate is used here only because a back-end that cannot close is not
	 *          deliverable - every `LibCCEC::term()` would leak an open session and the
	 *          next open() would fail `EX_ILLEGAL_STATE`. If the mapping owners reject
	 *          the candidate, the body of this one method changes and nothing else does.
	 *
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. The AIDL call this makes
	 *          is a SYNCHRONOUS binder transaction with NO client-side deadline, so a HAL
	 *          that does not answer blocks this call for as long as it chooses. The
	 *          elapsed time is measured and a stall past the threshold is reported naming
	 *          this exact method, and that report is the whole of what the middleware can
	 *          do. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable().
	 * @see open()
	 * @see DriverImpl::close()
	 */
	virtual void  close(void) noexcept(false);

	/**
	 * @brief Takes the next received CEC frame, blocking until one arrives
	 *
	 * Byte-for-byte the legacy DriverImpl::read(): the same queue, the same
	 * `status != OPENED` guard, and the same behaviour on the NULL sentinel, which
	 * flushes whatever remains in the queue and then raises. There is no AIDL call in
	 * this method at all, and that is precisely what leaves the Bus reader thread
	 * unchanged by this migration - only the thread that produces into the queue
	 * differs between the two back-ends.@n
	 * The one departure, and it changes nothing observable: every pointer the flush
	 * dequeues is null checked as well, not only the first poll. close() offers one
	 * sentinel per transition out of OPENED, so more than one can be present, and the
	 * legacy loop would dereference the second and fault on the Bus reader thread. A
	 * NULL entry is skipped and the drain continues; the previous handling of one was
	 * undefined behaviour rather than a behaviour a caller could depend on.
	 *
	 * @param [out] frame - Receives a copy of the frame taken from the incoming queue.
	 *                      Also written by the flush that precedes the raise on close,
	 *                      exactly as on the legacy path, so a caller that catches the
	 *                      exception must not rely on its contents. A flush that meets
	 *                      only close sentinels leaves it untouched.
	 *
	 * @throws InvalidStateException - The driver was not OPENED on entry, or it stopped
	 *                                 being OPENED while this call was blocked, in
	 *                                 which case the queue is flushed first.
	 *
	 * @pre open() has completed successfully.
	 * @warning Blocks the calling thread until a frame or the close sentinel arrives.
	 *          It is called from the Bus reader thread, never from a plugin thread.
	 *
	 * @see close()
	 * @see DriverImpl::read()
	 */
	virtual void  read(CECFrame &frame) noexcept(false);

	/**
	 * @brief Transmits a CEC frame synchronously and reports the bus outcome
	 *
	 * Reproduces DriverImpl::write() including its statement order: the frame buffer is
	 * taken and the frame is logged, then the lock is acquired and the state checked,
	 * then the transmit runs with the lock still held. `IHdmiCecController::sendMessage()`
	 * replaces `HdmiCecTx()`, and the returned `SendMessageStatus` is translated onto
	 * the legacy exception set.@n
	 * That translation has to respect an inverted sense: `ACK_STATE_0` means
	 * acknowledged for a directed message but rejected for a broadcast, and `ACK_STATE_1`
	 * is the mirror of that. The destination nibble is read from `frame.at(0) & 0x0F`
	 * exactly as the legacy implementation reads it, and the CEC CTS 9-3-3 arm - a
	 * broadcast REPORT_PHYSICAL_ADDRESS that was rejected - raises so the caller retries.@n
	 * The translation is EXHAUSTIVE over `SendMessageStatus` and an UNDOCUMENTED value
	 * FAILS rather than falling through to success. The result is an `int32_t` the
	 * generated proxy reads out of a parcel, so a HAL can return a value outside the three
	 * documented enumerators; reporting one as a completed transmit would make a suppressed
	 * frame indistinguishable from a delivered one and would stop the caller retrying.
	 *
	 * @param [in] frame - The frame to transmit, header byte first. Must be no longer
	 *                     than 16 bytes.
	 *
	 * @throws IOException           - The `sendMessage()` transaction reported a non-ok
	 *                                 binder status; or the HAL reported `BUSY`, meaning
	 *                                 arbitration failed and nothing was sent; or the
	 *                                 frame exceeded AIDL_MAX_MESSAGE_LENGTH; or no
	 *                                 controller session is held; or the HAL reported a
	 *                                 send status that is not a documented
	 *                                 `SendMessageStatus` value.
	 * @throws CECNoAckException     - A directed message was not acknowledged, or the
	 *                                 CEC CTS 9-3-3 broadcast arm was hit.
	 * @throws InvalidStateException - The driver is not OPENED.
	 * @throws std::out_of_range     - The frame carries no header byte. The prelude
	 *                                 decodes that byte before the state guard runs, and
	 *                                 the formatter catches only the CCEC Exception
	 *                                 family, so the frame access raises through this
	 *                                 method; the status translation reads the same byte
	 *                                 again later. Both readings are identical on the
	 *                                 legacy back-end, which runs the same prelude in the
	 *                                 same order, so an empty frame raises this rather
	 *                                 than InvalidStateException even on a closed driver.
	 *
	 * @pre open() has completed successfully.
	 * @post A normal return means no arm of the legacy status mapping was triggered. It
	 *       does not mean every receiver accepted the frame: a broadcast the HAL reports
	 *       as `ACK_STATE_0` - which the AIDL contract calls a rejected broadcast -
	 *       returns normally for every opcode except the CEC CTS 9-3-3
	 *       REPORT_PHYSICAL_ADDRESS arm, because that is what the legacy mapping does.
	 * @warning Authorized observable difference 1. `sendMessage()` states a 16-byte
	 *          maximum while CECFrame carries up to CECFrame::MAX_LENGTH (128) and the
	 *          legacy HAL specification allows 20, so a frame of 17 to 20 bytes is
	 *          sendable on the legacy back-end and raises IOException here. The frame is
	 *          policed, never truncated: truncating would put a corrupt CEC frame on the
	 *          bus. No frame the current plugin surface produces comes close to the limit.
	 * @warning Holds the instance lock across the whole IPC round trip, exactly as the
	 *          legacy implementation holds it across the whole in-process call. This
	 *          preserves the existing serialization rather than introducing new
	 *          contention.
	 *
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. The AIDL call this makes
	 *          is a SYNCHRONOUS binder transaction with NO client-side deadline, so a HAL
	 *          that does not answer blocks this call for as long as it chooses. The
	 *          elapsed time is measured and a stall past the threshold is reported naming
	 *          this exact method, and that report is the whole of what the middleware can
	 *          do. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable(). It matters most HERE: the instance mutex is
	 *          held across the transmit, deliberately, to preserve legacy `HdmiCecTx`
	 *          serialization - so a stalled transmit also holds off every other operation
	 *          on this back-end, and narrowing that critical section is not an option.
	 *
	 * @see poll()
	 * @see DriverImpl::write()
	 */
	virtual void  write(const CECFrame &frame) noexcept(false);

	/**
	 * @brief Not supported on the AIDL back-end; raises after the legacy prelude
	 *
	 * Asynchronous transmit is not migrated. The AIDL HAL exposes no asynchronous
	 * transmit and none is emulated here - no threads, no work queue, no deferred
	 * callback and no wrapper. This is a decided point, not an unresolved mapping, and
	 * it is safe because no production call site reaches this method: every plugin
	 * transmit goes through Connection::sendToAsync() and Connection::sendAsync() onto
	 * the Bus writer thread, which then calls the synchronous write().@n
	 * The legacy statement order is nevertheless reproduced exactly, because it is
	 * observable. DriverImpl::writeAsync() takes the frame buffer and logs the frame
	 * before it locks and checks the state, so an empty frame on a closed driver raises
	 * `std::out_of_range` from the frame access rather than InvalidStateException. This
	 * override runs the same two prelude calls, applies the same state guard, and only
	 * then declines the transmit.
	 *
	 * @param [in] frame - The frame that would have been transmitted. Still accessed by
	 *                     the prelude, so an empty frame raises before the guard runs.
	 *
	 * @throws std::out_of_range               - Raised by the prelude for an empty
	 *                                           frame, on both back-ends alike.
	 * @throws InvalidStateException           - The driver is not OPENED.
	 * @throws OperationNotSupportedException  - Always, once the prelude and the guard
	 *                                           have passed.
	 *
	 * @pre None beyond the prelude's own requirement that the frame be non-empty.
	 * @warning Authorized observable difference 2. A valid frame on an open driver
	 *          succeeds on the legacy back-end and raises
	 *          OperationNotSupportedException here. The two closed-driver and
	 *          empty-frame cases behave identically on both, because the prelude
	 *          precedes the guard on both.
	 *
	 * @see write() - the synchronous path every production caller actually reaches
	 * @see DriverImpl::writeAsync()
	 */
	virtual void  writeAsync(const CECFrame &frame) noexcept(false);

	/**
	 * @brief Relinquishes one logical address
	 *
	 * Reproduces DriverImpl::removeLogicalAddress(), whose shape is the specification:
	 * the state guard first, then the removal from the local list, and only then the
	 * HAL call - whose result the legacy implementation ignores. That disposition is
	 * kept. `IHdmiCecController::removeLogicalAddresses()` is called with a one-element
	 * vector, and both a false result and a non-ok binder status are logged and
	 * otherwise ignored, because raising where the legacy back-end returns silently
	 * would be an unregistered behaviour change.
	 *
	 * @param [in] source - The logical address to relinquish. Marshalled as a
	 *                      one-element `std::vector<int32_t>`; no multi-address state is
	 *                      introduced anywhere.
	 *
	 * @throws InvalidStateException - The driver is not OPENED.
	 *
	 * @pre open() has completed successfully.
	 * @post The address is absent from the local list whether or not the HAL agreed.
	 * @warning Reports nothing about HAL-side failure, by design. A caller that needs to
	 *          know an address was released must re-query.
	 *
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. The AIDL call this makes
	 *          is a SYNCHRONOUS binder transaction with NO client-side deadline, so a HAL
	 *          that does not answer blocks this call for as long as it chooses. The
	 *          elapsed time is measured and a stall past the threshold is reported naming
	 *          this exact method, and that report is the whole of what the middleware can
	 *          do. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable().
	 *
	 * @see addLogicalAddress()
	 * @see DriverImpl::removeLogicalAddress()
	 */
	virtual void  removeLogicalAddress(const LogicalAddress &source);

	/**
	 * @brief Acquires one logical address
	 *
	 * Calls `IHdmiCecController::addLogicalAddresses()` with a one-element vector built
	 * from LogicalAddress::toInt(), then records the address in the local list on
	 * success. The state guard and the local bookkeeping match
	 * DriverImpl::addLogicalAddress() exactly.
	 *
	 * @param [in] source - The logical address to acquire.
	 *
	 * @return bool - Acquisition result
	 * @retval true  - The address was acquired. This is the only value ever returned;
	 *                 every failure leaves by way of an exception, as on the legacy
	 *                 back-end.
	 *
	 * @throws AddressNotAvailableException - The HAL reported false, meaning the address
	 *                                        is outside 0x0..0xE or is already taken.
	 * @throws IOException                  - No controller session is held, or the
	 *                                        transaction reported a non-ok binder status.
	 * @throws InvalidStateException        - The driver is not OPENED.
	 *
	 * @pre open() has completed successfully.
	 * @post On success the address is in the local list, so isValidLogicalAddress()
	 *       reports it.
	 * @warning Authorized observable difference 3. The legacy back-end distinguishes
	 *          three HAL outcomes - address unavailable, general error, and anything
	 *          else as success - whereas `addLogicalAddresses()` returns a single
	 *          boolean documented as false both when the address is out of range and
	 *          when it is already added. Carrying the legacy distinction would require a
	 *          new HAL method, which is out of bounds for this migration, so false maps
	 *          to the nearer legacy category, AddressNotAvailableException. Callers that
	 *          discriminate IOException from other failures therefore see a different
	 *          category for the same underlying condition.
	 *
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. The AIDL call this makes
	 *          is a SYNCHRONOUS binder transaction with NO client-side deadline, so a HAL
	 *          that does not answer blocks this call for as long as it chooses. The
	 *          elapsed time is measured and a stall past the threshold is reported naming
	 *          this exact method, and that report is the whole of what the middleware can
	 *          do. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable().
	 *
	 * @see removeLogicalAddress()
	 * @see DriverImpl::addLogicalAddress()
	 */
	virtual bool  addLogicalAddress   (const LogicalAddress &source);

	/**
	 * @brief Reads back the logical address in use
	 *
	 * Calls `IHdmiCec::getLogicalAddresses()` - on the top-level interface, not on the
	 * controller - and returns the first entry. `devType` is ignored, exactly as
	 * DriverImpl::getLogicalAddress() ignores it, and the legacy per-device-type get and
	 * the AIDL get-all are treated as equivalent.@n
	 * When the HAL returns more than one address the count and the entry used are
	 * logged and the first entry is returned. No iteration, no multi-address state and
	 * no dispatch fan-out is introduced.
	 *
	 * @param [in] devType - Accepted for signature compatibility with the Driver
	 *                       interface and with the legacy back-end. Not used.
	 *
	 * @return int - The logical address in use
	 * @retval 0 - No address is in use. This single value covers five cases, which are
	 *             distinguished in the log rather than in the return value: no service
	 *             proxy is held, a non-ok binder status, a successful call that returned
	 *             no addresses, a first entry outside the AIDL contract range 0x0..0xE -
	 *             which is rejected on the raw `int32_t`, before any conversion could
	 *             make an out-of-range value look like a plausible address - and the
	 *             genuine address 0. The legacy implementation behaves the same way for
	 *             the cases it has - it zero-initializes its local and returns whatever
	 *             the HAL leaves there - and LibCCEC::getLogicalAddress() turns a zero
	 *             into an InvalidStateException, which is the existing signal for
	 *             "no address". Any other sentinel here would suppress that throw.
	 * @retval other - The first logical address the HAL reports, once it is known to be
	 *                 inside the contract range.
	 *
	 * @pre None. Unlike most methods here this one carries no state guard, matching the
	 *      legacy implementation.
	 * @post No state changed.
	 * @warning Never throws on HAL failure. The zero return is the failure signal.
	 * @warning An empty result is a documented normal outcome rather than an error: the
	 *          AIDL contract states the array is empty when no additional addresses have
	 *          been set.
	 *
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. The AIDL call this makes
	 *          is a SYNCHRONOUS binder transaction with NO client-side deadline, so a HAL
	 *          that does not answer blocks this call for as long as it chooses. The
	 *          elapsed time is measured and a stall past the threshold is reported naming
	 *          this exact method, and that report is the whole of what the middleware can
	 *          do. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable().
	 *
	 * @see isValidLogicalAddress()
	 * @see DriverImpl::getLogicalAddress()
	 */
	virtual int   getLogicalAddress(int devType);

	/**
	 * @brief Physical-address retrieval - blocked on B1 for this back-end
	 *
	 * Blocked item B1. On the AIDL back-end this method does not retrieve a physical
	 * address. It logs that the read is unavailable pending the device-settings HAL
	 * contract, names B1, and returns leaving the caller's out parameter untouched.@n
	 * The legacy back-end reads the address with `HdmiCecGetPhysicalAddress()`, a CEC
	 * HAL call that needs a native handle obtained from `HdmiCecOpen()` - a handle this
	 * back-end never has, because it opens the AIDL HAL instead. The mandated
	 * replacement is an EDID-byte read that belongs to the device-settings HAL layer,
	 * not to the CEC HAL; its public header was to be supplied as a separate input to
	 * this migration and was not, and it is owed by whoever owns the device-settings HAL
	 * contract. Reconstructing that declaration from usage, taking the AIDL HDMI-output
	 * EDID event, and substituting any CEC HAL call are all excluded, and acquiring a
	 * legacy CEC handle lazily is excluded on top of that because `HdmiCecOpen()`
	 * performs logical-address discovery for source devices, which puts CEC traffic on
	 * the wire and would leave two controllers contending.
	 *
	 * Returning without writing is the minimum-harm interim behaviour rather than a
	 * fabricated value: it is the same observable outcome the legacy path already
	 * produces when its own call fails, since the legacy implementation ignores the
	 * return value and writes nothing on failure. Both plugin call sites already wrap
	 * the call and tolerate not obtaining an address.
	 *
	 * @param [out] physicalAddress - Would receive the physical address. Left untouched
	 *                                by this back-end, so the caller keeps whatever it
	 *                                initialized the variable to.
	 *
	 * @pre None.
	 * @post The out parameter is unmodified.
	 * @warning The physical address is unavailable on the AIDL back-end until B1
	 *          resolves. This is a real functional gap, and it is reported rather than
	 *          papered over.
	 * @warning The body of this method is the one place that changes when the B1 contract
	 *          arrives. At that point the mandated device-settings read is
	 *          implemented here against the supplied declaration, and
	 *          DriverImpl::getPhysicalAddress() is reviewed for whether the same read
	 *          should replace its own CEC HAL call so that both back-ends use the
	 *          mandated API.
	 *
	 * @see DriverImpl::getPhysicalAddress() - the legacy implementation, unchanged
	 */
	virtual void  getPhysicalAddress(unsigned int *physicalAddress);

	/**
	 * @brief Reports whether a logical address is one this device holds
	 *
	 * Byte-for-byte the legacy DriverImpl::isValidLogicalAddress(): a walk of the local
	 * list under the instance lock, with no HAL call on either back-end. Because close()
	 * does not clear that list on either back-end, this can still report true after a
	 * close.
	 *
	 * @param [in] source - The logical address to test.
	 *
	 * @return bool - Whether the address is held locally
	 * @retval true  - The address is in the local list.
	 * @retval false - It is not.
	 *
	 * @pre None. Valid in any state.
	 * @post No state changed.
	 * @warning Never throws.
	 *
	 * @see addLogicalAddress()
	 * @see DriverImpl::isValidLogicalAddress()
	 */
	virtual bool isValidLogicalAddress(const LogicalAddress &source) const;

	/**
	 * @brief Pings a logical address by transmitting a header-only frame
	 *
	 * Byte-for-byte the legacy DriverImpl::poll(): the header byte is assembled as
	 * `((from & 0x0F) << 4) | (to & 0x0F)`, appended to a one-byte CECFrame, and handed
	 * to this back-end's own write(). The AIDL `getState()` is deliberately not used -
	 * a poll is a CEC ping performed by a one-byte transmit, not a state query - so the
	 * outcome reaches the caller through write()'s exceptions.
	 *
	 * @param [in] from - Initiator logical address, placed in the high nibble.
	 * @param [in] to   - Follower logical address to ping, placed in the low nibble.
	 *
	 * @throws CECNoAckException     - Nothing acknowledged the ping, which is the
	 *                                 normal way a caller learns the address is free.
	 * @throws IOException           - The transmit failed, or the HAL was busy.
	 * @throws InvalidStateException - The driver is not OPENED.
	 *
	 * @pre open() has completed successfully.
	 * @post The one-byte frame was handed to write(), so the poll travels over whichever
	 *       transport the enclosing back-end uses.
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE, INHERITED THROUGH write().
	 *          This method issues no AIDL call of its own, and that is exactly why the
	 *          prerequisite is restated rather than left to be inferred: the transmit it
	 *          delegates to is a SYNCHRONOUS binder transaction with NO client-side
	 *          deadline, so a HAL that does not answer blocks this call for as long as it
	 *          chooses, and a caller reading only the @throws list above would take the
	 *          three exceptions for the complete set of outcomes. The elapsed time is
	 *          measured and a stall past the threshold is reported naming
	 *          `IHdmiCecController::sendMessage`, not this method, so a log reader
	 *          diagnosing a wedged poll should look for that label. Because write() holds
	 *          the instance mutex across the transmit to preserve legacy `HdmiCecTx`
	 *          serialization, a stalled poll also holds off every other operation on this
	 *          back-end. The prerequisite, the reason the pinned libbinder admits no
	 *          in-middleware bound, and each mechanism that was ruled out are recorded
	 *          once on isServiceAvailable().
	 *
	 * @see write()
	 * @see DriverImpl::poll()
	 */
	virtual void poll(const LogicalAddress &from, const LogicalAddress &to) noexcept(false);

	/**
	 * @brief Logs a decoded, human-readable rendering of a frame
	 *
	 * Byte-for-byte the legacy DriverImpl::printFrameDetails(): pure formatting with no
	 * HAL call on either back-end. The header is decoded from the frame, the opcode name
	 * is resolved when the frame is long enough to carry one, and every CCEC exception
	 * raised while decoding a malformed frame is caught and logged so that diagnostics
	 * can never break a transmit.
	 *
	 * An empty frame is the one case that is not contained here, and containing it would
	 * change observable behaviour. The formatter's only handler names Exception, the CCEC
	 * base, while the header decode of a frame with no bytes raises `std::out_of_range`,
	 * which is outside that family - so that exception leaves this method. write() and
	 * writeAsync() call this from their prelude, ahead of their state guard, so an empty
	 * frame raises `std::out_of_range` out of them rather than InvalidStateException on
	 * both back-ends alike.
	 *
	 * @param [in] frame - The frame to render.
	 *
	 * @throws std::out_of_range - The frame carries no header byte, so the header decode
	 *                             reaches past the end of it. Not caught here, on either
	 *                             back-end.
	 *
	 * @pre The frame is non-empty. A malformed non-empty frame is tolerated: the CCEC
	 *      exceptions its decode raises are caught here and logged.
	 * @warning Declared `noexcept(false)` to match the Driver interface. It does not
	 *          propagate CCEC exceptions, and it does propagate the `std::out_of_range`
	 *          above.
	 *
	 * @see write()
	 * @see DriverImpl::printFrameDetails()
	 */
	virtual void printFrameDetails(const CECFrame &frame) noexcept(false);

	/*
	 * Declared here and defined further down, next to the predicate that consumes it,
	 * because isServiceAvailable() below takes it by reference and a parameter type - unlike
	 * a default argument - is not looked up in the complete-class context. A reference to an
	 * incomplete nested type is all a declaration needs.
	 */
	struct BinderPreflightProbe;

	/**
	 * @brief Reports whether a usable, compatible AIDL HDMI CEC service is present
	 *
	 * This is the question the selection helper in `ccec/src/Driver.cpp` asks, once,
	 * after both back-ends have been constructed. It answers in four ordered stages and
	 * stops at the first that fails:
	 * -# isBinderPreflightOk() on the driver path, so that nothing in this process
	 *    touches libbinder unless doing so is known to be safe. Custody of the validated
	 *    descriptor and its identity is taken here and held for the rest of the method;
	 * -# the CUSTODY RE-VERIFICATION, immediately before the lookup: the same path is
	 *    resolved again and its identity compared against the one the preflight
	 *    validated, and the context manager is asked again under the same bound. This
	 *    stage is what makes the check and the use one path rather than two;
	 * -# the service lookup, which resolves `IHdmiCec::serviceName()` - the literal
	 *    `"HdmiCec"` - through the binder service manager and yields a typed proxy or
	 *    nullptr;
	 * -# the compatibility check, which rejects a null proxy, an empty or `"-1"`
	 *    interface hash, an unfrozen development server, and a server whose interface
	 *    version does not satisfy the compiled-against `IHdmiCec::VERSION` within the
	 *    same era and major.
	 *
	 * Presence alone is not sufficient: a service that answers but cannot be spoken to
	 * compatibly is treated exactly as an absent one, and the legacy back-end is
	 * selected. The compatibility rule is not reimplemented here - `halcompat` is reused
	 * as it stands - and that rule accepts a server newer than this client within the
	 * same era and major, so "not exactly this client's version" is not incompatible. A
	 * resolved and compatible proxy is cached on this instance for open() to use, so the
	 * lookup happens once rather than once per operation.
	 *
	 * @param [in] binderDriverPath        - Binder driver node the preflight inspects and
	 *                                       the re-verification resolves again. A
	 *                                       parameter for the same reason
	 *                                       isBinderPreflightOk() takes one: the arms
	 *                                       that decline can then be exercised without
	 *                                       rendering a runner's real driver unusable.
	 *                                       Defaults to DEFAULT_BINDER_DRIVER_PATH, so
	 *                                       the production call site names nothing.
	 * @param [in] contextManagerTimeoutMs - Upper bound, in milliseconds, applied to BOTH
	 *                                       context-manager probes. Defaults to
	 *                                       DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS.
	 * @param [in] probe                   - The six kernel-facing operations the preflight
	 *                                       and the re-verification perform. Defaults to
	 *                                       defaultBinderProbe(), the real syscalls.
	 *
	 * @return bool - Whether the AIDL back-end should be selected
	 * @retval true  - The preflight passed, the node was still the same one at the moment
	 *                 of use, the service resolved, and it is compatible. The proxy is
	 *                 cached and open() may be called.
	 * @retval false - Any stage failed. The reason is logged and recorded for
	 *                 unavailabilityReason(), and the caller must select the legacy
	 *                 back-end.
	 *
	 * @pre None. Safe to call on a platform with no binder driver, no `servicemanager`
	 *      and no AIDL HAL whatsoever - which is the entire point of the preflight.
	 * @post No lifecycle transition occurs; the instance remains CLOSED either way. On a
	 *       true result, and only then, the service proxy is held for open() to use. The
	 *       descriptor the preflight retained is released on every exit path from this
	 *       method, including the throwing one, so the custody window closes here and
	 *       nothing survives it.
	 * @warning Never aborts, never blocks indefinitely and never propagates an
	 *          exception, because a legacy-only SOC must reach the legacy back-end
	 *          rather than fail to initialize.
	 * @warning Not thread safe, and not intended to be: it is called once from the
	 *          factory's one-time static initializer, which the language already
	 *          serializes.
	 * @warning HARD PLATFORM PREREQUISITE - SERVICE AUTHORIZATION. The platform MUST
	 *          enforce service-manager add and find authorization for the `"HdmiCec"`
	 *          service identity, so that ONLY the genuine HDMI CEC HAL may register that
	 *          name and only authorized clients may resolve it, and the platform MUST
	 *          apply restrictive ownership and mode to the Binder node so that an
	 *          unprivileged process cannot reach the driver at all. A platform that does
	 *          neither is not made safe by anything in this middleware.@n
	 *          THIS PREREQUISITE IS NOT MET BY THE PINNED BINDER SDK ON ITS OWN, and an
	 *          integrator must not assume otherwise from the presence of `servicemanager`.
	 *          In `linux_binder_idl` 2.6.0 the daemon's authorization hooks are compiled
	 *          out on a non-Android build: `cmds/servicemanager/Access.cpp` guards its
	 *          `selinux_check_access()` call with `#ifdef __ANDROID__` and takes `#else
	 *          return true;`, so on a Linux port `canAdd()`, `canFind()` and `canList()`
	 *          allow everything, and any process able to open the Binder node may register
	 *          `"HdmiCec"`. Supplying the authorization is therefore platform-image work -
	 *          an SELinux-enabled daemon build with a policy that labels this service, or
	 *          an equivalent restriction on who may reach the Binder context at all - and
	 *          it cannot be moved into this middleware, which is a client. Do not reach for
	 *          `IServiceManager::isDeclared()` as a stand-in either: on the same pin
	 *          `cmds/servicemanager/ServiceManager.cpp` answers it from `isVintfDeclared()`
	 *          only under `#ifdef __ANDROID__` and otherwise reports false unconditionally,
	 *          so a declaration check here would decline every service on every conformant
	 *          platform using this pin - a fallback that is wrong rather than safe.@n
	 *          THE CONSEQUENCE, PLAINLY. This predicate identifies a service by the
	 *          generated service name plus the interface hash and version the frozen AIDL
	 *          snapshot compiles in. Every one of those three values is PUBLICLY
	 *          REPRODUCIBLE - they are constants in a published interface package, not
	 *          secrets - so on a platform that lets any process register `"HdmiCec"`, a
	 *          hostile local process can register first, be selected as the HAL, and from
	 *          then on observe every outbound CEC frame, suppress or alter transmits,
	 *          report logical addresses of its choosing, and inject arbitrary inbound
	 *          frames through the event listener this back-end hands it at open().
	 *          Selection is resolved once per process, so that substitution persists for
	 *          the process lifetime.@n
	 *          WHAT THIS BACK-END DOES CHECK, so the boundary is not overstated: the
	 *          Binder node is required to exist, to be a CHARACTER DEVICE and to be owned
	 *          by UID 0; its kernel protocol version must equal the one the linked
	 *          libbinder was built for; ALL FIVE of its captured identity attributes -
	 *          device, inode, rdev, mode and uid - are re-verified immediately before the
	 *          lookup, so the node can neither be substituted NOR be re-permissioned or
	 *          re-owned in the check-to-use window; and the resolved service must present
	 *          a compatible interface hash and version.
	 *          Those checks establish that the TRANSPORT is the platform's genuine Binder
	 *          driver and that the peer speaks this exact interface revision. NONE of them
	 *          is authentication of the peer, and none can be: the middleware is a client
	 *          and the pinned libbinder C++ backend gives a client no credential of the
	 *          service it resolved. `BpBinder` - the proxy type this lookup yields - exposes
	 *          transact, liveness, death linkage and object attachment and no peer identity
	 *          whatsoever, and `IPCThreadState::getCallingUid()`/`getCallingPid()`/
	 *          `getCallingSid()` describe an INBOUND transaction being served, which a
	 *          client performing a lookup does not have. The nearest thing the pin offers is
	 *          `IServiceManager::getServiceDebugInfo()`, which reports the daemon's record of
	 *          the pid that registered a name; that is EVIDENCE FOR A LOG AND NOT A GATE -
	 *          it is the registrar's unauthenticated bookkeeping, it says nothing about
	 *          whether that pid was entitled to register, and it is only reachable after
	 *          `defaultServiceManager()` has already opened the driver. Treating it as an
	 *          authorization check would substitute a weaker mechanism for the platform
	 *          obligation stated above.@n
	 *          WHAT WAS DELIBERATELY NOT ADDED, and must not be added later in the name of
	 *          fixing this: no HAL method (the AIDL surface is consumed as it exists), no
	 *          public middleware selector or override (the public API does not change), and
	 *          no vendor, variant or configuration conditional on the selection - AAP
	 *          divergence 4 requires the selection to rest on runtime service availability
	 *          alone, and a conditional would be a second, weaker authorization mechanism
	 *          that a hostile process on a misconfigured platform could satisfy just as
	 *          easily.
	 * @warning HARD PLATFORM PREREQUISITE - BOUNDED HAL RESPONSE. Once a synchronous AIDL
	 *          transaction is entered, NO CLIENT-SIDE DEADLINE EXISTS. The platform MUST
	 *          guarantee that the HDMI CEC HAL service answers every transaction within a
	 *          bound, and a platform that cannot guarantee it MUST NOT register
	 *          `"HdmiCec"` - which makes this predicate decline and the middleware select
	 *          the legacy back-end, the correct outcome for such a platform. The
	 *          middleware cannot substitute a bound of its own, and the reason is the
	 *          pinned libbinder rather than a design choice: the C++ backend exposes no
	 *          per-transaction timeout; `IPCThreadState::talkWithDriver()` retries on
	 *          EINTR, so neither a signal nor an interval timer can break a blocked
	 *          transaction out; `linkToDeath` detects a DEAD service and not a HUNG one,
	 *          and death recipients are omitted by design because the legacy in-process
	 *          HAL had no counterpart; a watchdog thread could not cancel a blocked
	 *          transaction either and is barred as a new abstraction; and narrowing
	 *          write()'s critical section is barred because holding the instance mutex
	 *          across the transmit is what preserves legacy `HdmiCecTx` serialization.
	 *          What IS bounded is bounded: the driver-node and context-manager checks in
	 *          both stages above have hard deadlines, so a wedged `servicemanager` - the
	 *          dominant stall mode on an otherwise healthy platform - becomes a decline
	 *          rather than an unbounded wait. Beyond that the residual is the platform's,
	 *          and every synchronous call measures itself and reports a stall naming the
	 *          exact method and elapsed time.
	 *
	 * @see isBinderPreflightOk()
	 * @see BinderNodeIdentity
	 * @see unavailabilityReason()
	 * @see Driver::getInstance()
	 */
	bool isServiceAvailable(const std::string &binderDriverPath = DEFAULT_BINDER_DRIVER_PATH,
	                        unsigned int contextManagerTimeoutMs = DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
	                        const BinderPreflightProbe &probe = defaultBinderProbe());

	/**
	 * @brief Reports why the last isServiceAvailable() declined, without asking again
	 *
	 * The decision is preserved rather than reconstructed, and that is the whole point of
	 * this accessor. isServiceAvailable() already establishes which of its four ordered
	 * stages declined; the selection helper in `ccec/src/Driver.cpp` needs that same
	 * answer in order to name the platform condition in its fallback line, and it reads
	 * the record rather than asking a second question. Asking again would be wrong in two
	 * independent ways: re-running the bounded preflight pays its context-manager timeout
	 * a second time, doubling the worst-case delay inside LibCCEC::init() on exactly the
	 * platform least able to afford it; and the second run need not agree with the first,
	 * because a `servicemanager` that starts or dies between the two calls flips its
	 * verdict, so the reported reason could name a condition that did not cause the
	 * fallback.@n
	 * So the reason is recorded by the query that decided it and simply read back here.
	 *
	 * @return const char* - Why the AIDL back-end is unusable
	 * @retval NULL - Either the query has not been run yet, or it returned true. In
	 *                both cases there is no fallback reason to report, and a caller
	 *                must not print one.
	 * @retval non-NULL - A stable, human-readable phrase naming the platform condition
	 *                    that declined, suitable for substitution into a log line. It
	 *                    has static storage duration and outlives this object, so it is
	 *                    safe to hold and to format after this call returns.
	 *
	 * @pre None. Safe before isServiceAvailable() has ever run, which is what the NULL
	 *      retval covers.
	 * @post Nothing changed. This is a pure read of state the query already set.
	 * @warning Never throws and never touches binder.
	 * @warning The returned phrases are a test and tooling contract: the coverage runner
	 *          and the L2 tier both transcribe the resulting log lines. Reword one and
	 *          those consumers stop matching.
	 *
	 * @see isServiceAvailable()
	 * @see Driver::getInstance()
	 */
	const char *unavailabilityReason(void) const;

	/**
	 * @brief The identity of the filesystem node the preflight validated
	 *
	 * WHAT THIS CLOSES, because a struct of five integers reads as bookkeeping otherwise.
	 * The preflight opens the configured path, establishes that the node behind it is a
	 * binder driver of the right protocol whose context manager answers, and then
	 * libbinder OPENS THE SAME PATHNAME AGAIN for itself during the service lookup. Those
	 * are two separate resolutions of one name, and on the pinned stack the second one is
	 * FATAL when it goes wrong: a failed driver open or a protocol mismatch is a
	 * `LOG_ALWAYS_FATAL_IF` abort. So a node swapped between the two resolutions turns a
	 * check whose entire purpose is to guarantee a nonfatal legacy fallback into a process
	 * abort - a time-of-check-to-time-of-use window with the worst possible consequence.
	 *
	 * Carrying the validated node's identity out of the check and comparing it again
	 * immediately before the use is what NARROWS THAT WINDOW TO ITS STRUCTURAL MINIMUM -
	 * narrows, and not closes, and the difference is a residual an integrator must be told
	 * about rather than a shortcoming of this comparison. What remains is stated under
	 * WHAT IS NOT CLOSED below. ALL FIVE ATTRIBUTES CAPTURED HERE ARE COMPARED.
	 * `device` and `inode` together identify one filesystem
	 * object uniquely, and `rdev` is the driver's major/minor pair, so a replacement node -
	 * created, bind-mounted, or reached through a re-pointed symlink - differs in at least
	 * one of the three. The preflight additionally HOLDS THE VALIDATED DESCRIPTOR OPEN
	 * across that window, which is what makes the inode half of the comparison meaningful:
	 * an open descriptor pins the inode, so the number cannot be recycled by a replacement
	 * created at the same path while custody is held.
	 *
	 * `mode` and `uid` are BOTH validated once against fixed requirements inside the check
	 * - character device, owned by root - AND compared again across the window, because the
	 * two do different work and neither substitutes for the other. The validation says the
	 * node was a legitimate, privileged, kernel-published object at check time; the
	 * comparison says nobody `chmod`ed or `chown`ed THAT VERY INODE afterwards. Without the
	 * comparison an attacker who cannot replace the node can still widen write access to it,
	 * or hand it to an unprivileged owner, in the interval before libbinder opens the same
	 * name - and a `(device, inode, rdev)` comparison would report that as "unchanged",
	 * because the object really is the same object.
	 *
	 * The comparison requires no PARTICULAR mode or owner, only that neither MOVED, so it is
	 * correct at any platform baseline: a restrictive node (0600) and a broadly accessible
	 * one (0666, which AOSP-derived layouts publish deliberately because every binder client
	 * must be able to open the node) both pass unchanged.
	 *
	 * WHAT IS NOT CLOSED, stated plainly so nothing here reads as a stronger guarantee than
	 * it is. This comparison is the LAST statement before the lookup, so the interval it
	 * cannot cover is the shortest the code can make it - but libbinder still resolves the
	 * pathname INDEPENDENTLY inside that lookup, and it cannot be handed the descriptor this
	 * struct's identity was taken from. The pin admits no other arrangement: in
	 * `linux_binder_idl` 2.6.0 the only driver entry points are `ProcessState::self()` and
	 * `ProcessState::initWithDriver(const char*)`, both of which take a PATHNAME; the fd
	 * itself is `ProcessState::mDriverFD`, a private member with no accessor and no
	 * injector; `libs/binder/ProcessState.cpp` `open_driver(const char* driver)` performs
	 * `open(driver, O_RDWR | O_CLOEXEC)` on that pathname; and `ProcessState::init()` adds a
	 * THIRD resolution of its own - an `access(driver, R_OK)` probe that silently substitutes
	 * `/dev/binder` when it fails. So a substitution performed by a process privileged enough
	 * to replace a node under `/dev` in the interval AFTER this comparison and BEFORE
	 * libbinder's open is not detectable here, and on the pinned stack a bad open is a
	 * `LOG_ALWAYS_FATAL_IF` abort rather than a decline.
	 *
	 * Re-checking AFTER the lookup was considered and is deliberately not done, because it
	 * would make things worse rather than better: `gProcess` is a `[[clang::no_destroy]]`
	 * static created under a `std::call_once` and `~ProcessState()` is private, so once the
	 * lookup has run the process is bound to whatever libbinder opened FOR ITS WHOLE
	 * LIFETIME and no verdict taken afterwards can undo that binding. A post-lookup decline
	 * would therefore select the legacy back-end while leaving the process permanently
	 * attached to the substituted driver, which is a worse outcome than the abort it was
	 * meant to avoid.
	 *
	 * The residual is consequently the platform's, and it is the ordinary custody assumption
	 * every Binder client on this stack already makes: `/dev` must be writable only by the
	 * privileged platform image, so that no unprivileged process can substitute the node at
	 * all. What this struct buys against that backdrop is that a substitution, a `chmod` or
	 * a `chown` occurring in the far larger interval BEFORE the comparison is caught and
	 * declined instead of being carried into libbinder's fatal open.
	 *
	 * EVERY MEMBER IS A PLAIN INTEGER, AND THAT IS REQUIRED RATHER THAN TIDY. This struct
	 * mentions no binder kernel type and no `<sys/stat.h>` type, for the same reason
	 * BinderPreflightProbe carries the protocol version as an `unsigned int`: this header
	 * must still compile where the binder kernel UAPI definitions are absent. The widths
	 * are the widest plain integers the values can need, so no host's `dev_t` or `ino_t`
	 * can be truncated on the way through.
	 *
	 * @warning INTERNAL AND TEST-VISIBLE, NOT PUBLIC API, for the same reason
	 *          BinderPreflightProbe is: `ccec/src/DriverAidlImpl.hpp` is not an installed
	 *          header, so this adds nothing to the middleware public API.
	 *
	 * @see BinderPreflightProbe::identifyDescriptor
	 * @see BinderPreflightProbe::identifyPath
	 * @see isBinderPreflightOk()
	 * @see isServiceAvailable()
	 */
	struct BinderNodeIdentity {
		/** @brief The device the node lives on, POSIX `st_dev`. Half of the unique object identity. */
		unsigned long long device;
		/** @brief The node number, POSIX `st_ino`. The other half, pinned by the retained descriptor. */
		unsigned long long inode;
		/** @brief The driver's major/minor pair, POSIX `st_rdev`. Meaningful only for a device node. */
		unsigned long long rdev;
		/**
		 * @brief POSIX `st_mode`, verbatim
		 *
		 * Read two ways, both required: its file-type bits are extracted with
		 * BINDER_NODE_MODE_TYPE_MASK and validated once inside the preflight, and the
		 * WHOLE value is compared across the check-to-use window so a `chmod` of the
		 * validated inode cannot pass as "unchanged". Its permission bits are also
		 * OBSERVED and reported when they are broader than owner-only, which is a
		 * diagnostic and never a verdict - see isBinderPreflightOk().
		 */
		unsigned int mode;
		/**
		 * @brief The owning user, POSIX `st_uid`
		 *
		 * Compared against BINDER_NODE_REQUIRED_OWNER_UID inside the preflight, and
		 * compared against the validated value again across the check-to-use window so a
		 * `chown` of the validated inode cannot pass as "unchanged".
		 */
		unsigned int uid;
	};

	/**
	 * @brief The six kernel-facing operations the binder preflight performs
	 *
	 * A SEAM, and one the preflight cannot be verified without. Most of the predicate's
	 * decision arms are unreachable through its path argument alone, on any host: a node
	 * that opens and reports a MISMATCHED protocol version, a node that reports a MATCHING
	 * version and then fails the context-manager check, a node whose identity CHANGES
	 * between the check and the use, and a fully positive verdict. A driverless host
	 * reaches only the first two arms, and a binder-capable host reaches only the positive
	 * one - so on neither can the whole predicate be exercised, and its most consequential
	 * arms (the protocol-equality check, which is what stands between a mismatched kernel
	 * and libbinder's own abort, and the identity re-check, which is what stands between a
	 * substituted node and the same abort) would ship unexercised. Substituting these six
	 * operations makes every arm reachable deterministically, with no binder driver and
	 * without rendering a runner's real driver unusable.
	 *
	 * The struct is deliberately POD-with-function-pointers rather than an abstract
	 * interface: it adds no virtual dispatch, no allocation and no ownership question,
	 * and it keeps the default - the real syscalls - a compile-time constant. It also
	 * mentions no binder kernel type, which is required rather than tidy: this header
	 * must still compile where the binder kernel UAPI definitions are absent, so the
	 * protocol version crosses this boundary as a plain `unsigned int` and the driver
	 * node as a plain descriptor.
	 *
	 * @warning Internal and test-visible, not public API, for the same reason
	 *          isBinderPreflightOk() is: `ccec/src/DriverAidlImpl.hpp` is not an
	 *          installed header, so this adds nothing to the middleware public API, and
	 *          nothing outside `ccec/src` and the test suites may use it.
	 * @warning Every member must be non-null. isBinderPreflightOk() calls them
	 *          unconditionally and does not defend against a partially filled probe,
	 *          exactly as it does not defend against a null `::open`.
	 *
	 * @see defaultBinderProbe()
	 * @see isBinderPreflightOk()
	 * @see expectedBinderProtocolVersion()
	 */
	struct BinderPreflightProbe {
		/**
		 * @brief Opens the binder driver node
		 *
		 * The real implementation is `::open`. Declared with a fixed second parameter
		 * rather than as the variadic `::open` itself, so the default probe supplies a
		 * thin wrapper.
		 *
		 * @param [in] path  - Node to open.
		 * @param [in] flags - Open flags, `O_RDWR | O_CLOEXEC` in production.
		 *
		 * @return int - Descriptor, or negative on failure with `errno` set.
		 */
		int  (*openNode)(const char *path, int flags);
		/**
		 * @brief Reads the identity of the node an already-open descriptor refers to
		 *
		 * The real implementation is `fstat`. It is asked of the DESCRIPTOR rather than of
		 * the path, deliberately: the answer then describes the object this process has
		 * open and can no longer be changed underneath it, which is what makes it usable
		 * as the reference the later re-check compares against.
		 *
		 * @param [in]  fd  - Descriptor returned by openNode().
		 * @param [out] out - Receives the identity. Written only on success.
		 *
		 * @return int - Outcome
		 * @retval 0  - The identity was written to @p out.
		 * @retval -1 - It could not be read; `errno` carries the reason.
		 *
		 * @pre @p out is non-null.
		 *
		 * @see BinderNodeIdentity
		 */
		int  (*identifyDescriptor)(int fd, BinderNodeIdentity *out);
		/**
		 * @brief Reads the identity of whatever the given path currently resolves to
		 *
		 * The real implementation is `stat`, which FOLLOWS SYMLINKS - required rather than
		 * incidental, because a binderfs deployment may legitimately publish
		 * `/dev/binder` as a symlink to `/dev/binderfs/binder`, and the identity this
		 * yields must be the identity of the node libbinder will actually open when it
		 * resolves the same name.
		 *
		 * @param [in]  path - Path to resolve, the same one openNode() was given.
		 * @param [out] out  - Receives the identity. Written only on success.
		 *
		 * @return int - Outcome
		 * @retval 0  - The identity was written to @p out.
		 * @retval -1 - The path could not be resolved or stat'ed; `errno` carries the
		 *              reason. Treated as "declined", never as "unchanged".
		 *
		 * @pre @p path and @p out are non-null.
		 *
		 * @see BinderNodeIdentity
		 * @see isServiceAvailable()
		 */
		int  (*identifyPath)(const char *path, BinderNodeIdentity *out);
		/**
		 * @brief Reads the protocol version the driver reports
		 *
		 * The real implementation is `ioctl(fd, BINDER_VERSION, ...)`. The version is
		 * carried out as an `unsigned int` so that no binder kernel type appears in this
		 * header.
		 *
		 * @param [in]  fd      - Descriptor returned by openNode().
		 * @param [out] version - Receives the reported protocol version. Written only on
		 *                        success.
		 *
		 * @return int - Outcome
		 * @retval 0  - The version was read into @p version.
		 * @retval -1 - It could not be read; `errno` carries the reason where the
		 *              underlying call set one.
		 */
		int  (*readProtocolVersion)(int fd, unsigned int *version);
		/**
		 * @brief Asks, under a bounded wait, whether binder handle 0 resolves
		 *
		 * The real implementation posts a PING_TRANSACTION to handle 0 over @p fd and
		 * drains the driver's command stream under the deadline.
		 *
		 * @param [in] fd        - Descriptor whose protocol version has been verified.
		 * @param [in] timeoutMs - Upper bound in milliseconds; zero means do not wait.
		 *
		 * @return bool - Whether a context manager answered
		 * @retval true  - It answered, so `servicemanager` is reachable.
		 * @retval false - It did not, within the bound.
		 *
		 * @warning ONE CALL PER DESCRIPTOR. The real implementation maps the driver's
		 *          transaction buffer, and the binder driver permits exactly one mapping
		 *          per open descriptor for the LIFETIME of that descriptor - unmapping
		 *          does not restore the right to map again, so a second call on the same
		 *          descriptor fails whatever the platform's state. Callers that need to
		 *          ask twice must ask over a second, independently opened descriptor.
		 *
		 * @see isServiceAvailable()
		 */
		bool (*pingContextManager)(int fd, unsigned int timeoutMs);
		/**
		 * @brief Releases the descriptor openNode() returned
		 *
		 * The real implementation is `::close`.
		 *
		 * @param [in] fd - Descriptor to release.
		 *
		 * @return int - Zero on success, negative on failure. The preflight ignores it,
		 *               exactly as the production code it replaces ignored `::close`'s.
		 */
		int  (*closeNode)(int fd);
	};

	/**
	 * @brief The production probe: the real `::open`, `fstat`, `stat`, `BINDER_VERSION`,
	 *        ping and `::close`
	 *
	 * The default argument of isBinderPreflightOk(), so that every production call site
	 * names nothing and still gets the real kernel-facing operations, including every log
	 * line the predicate emits. Returns a reference to a single immutable instance with
	 * static storage duration, which is why it is safe as a default argument and why no
	 * caller has anything to own.
	 *
	 * @return const BinderPreflightProbe& - The real kernel-facing operations.
	 *
	 * @pre None.
	 * @post Nothing is initialized in the process. The probe holds function pointers and
	 *       performs no work until it is called.
	 * @warning Never throws. The instance is const, so no caller can rebind the probe
	 *          production uses; a test supplies its own by passing it explicitly.
	 *
	 * @see BinderPreflightProbe
	 * @see isBinderPreflightOk()
	 */
	static const BinderPreflightProbe &defaultBinderProbe(void);

	/**
	 * @brief The binder protocol version this build must speak to be usable
	 *
	 * `BINDER_CURRENT_PROTOCOL_VERSION`, exposed as a function so that a test can
	 * synthesise both a matching and a mismatching version without the binder kernel
	 * UAPI definitions leaking into this header or into the test translation unit. The
	 * constant follows `BINDER_IPC_32BIT` - protocol 7 for an all-32-bit platform, 8 for
	 * 32-bit middleware against a 64-bit vendor - which is exactly why it is read from
	 * the build rather than written down anywhere.
	 *
	 * @return unsigned int - Expected protocol version
	 * @retval 0     - This build carries no binder kernel ABI definitions, so no version
	 *                 can be named. The preflight cannot reach its comparison in that
	 *                 configuration, because the default probe's version read fails
	 *                 first.
	 * @retval other - The protocol version the linked libbinder was built for.
	 *
	 * @pre None.
	 * @warning Never throws.
	 *
	 * @see isBinderPreflightOk()
	 */
	static unsigned int expectedBinderProtocolVersion(void);

	/**
	 * @brief Decides whether a binder lookup may safely be attempted at all
	 *
	 * A preflight is required rather than convenient. Reaching the service manager
	 * directly is unsafe in two independent ways on the pinned binder stack, and either
	 * one of them defeats the absolute requirement that a supported SOC without an AIDL
	 * HAL must reach the legacy back-end:
	 * - a missing or protocol-mismatched driver node is fatal, not an error return. The
	 *   pin extends libbinder's failed-driver `LOG_ALWAYS_FATAL_IF` to plain Linux, so
	 *   what upstream guards for Android is live here and would abort the middleware
	 *   during initialization instead of falling back;
	 * - a missing context manager blocks indefinitely. Obtaining an `IServiceManager` at
	 *   all polls in one-second intervals until binder handle 0 resolves, with no upper
	 *   bound of its own, so a working driver with no running `servicemanager` would
	 *   stall LibCCEC::init() forever.
	 *
	 * This predicate therefore runs before anything in the process touches libbinder and
	 * checks, in this order, stopping at the first failure:
	 * -# the driver node exists and can be opened;
	 * -# the node the descriptor refers to can be identified at all;
	 * -# it is a CHARACTER DEVICE, because every supported binder layout publishes one;
	 * -# it is owned by ROOT, because every supported binder layout creates it as root and
	 *    a node owned by anyone else is one an unprivileged process could substitute;
	 * -# the protocol version it reports equals the version the linked libbinder was
	 *    built for, since libbinder enforces equality and a mismatch fails every open;
	 * -# binder handle 0, the context manager, resolves within a bounded timeout rather
	 *    than being waited on without limit.
	 *
	 * Only when all of them pass may the caller proceed to the service lookup and the
	 * compatibility check. Any failure means "AIDL absent" and the legacy back-end.
	 *
	 * ONE THING IS OBSERVED AND REPORTED WITHOUT BEING REQUIRED: a node writable beyond its
	 * owner is logged once, with its permission bits, and the verdict is UNCHANGED. Node
	 * permission restrictiveness is not a property this middleware can require, because a
	 * binder device node must be openable by every client process that uses binder -
	 * AOSP-derived platforms publish it broadly accessible by design and a binderfs
	 * deployment takes the mode binderfs assigns - so refusing a permissive mode would
	 * decline the AIDL path on conformant platforms while passing in a root-only CI guest.
	 * The enforceable node-level controls are therefore the character-device and root-owner
	 * checks above, plus the five-attribute identity comparison the caller makes immediately
	 * before the lookup, which catches this mode or owner CHANGING inside the check-to-use
	 * window even though neither value is dictated here. WHO may register and resolve
	 * `"HdmiCec"` is `servicemanager` add and find policy in the platform image and is
	 * recorded as a hard prerequisite on isServiceAvailable(); no client-side code
	 * substitutes for it.
	 *
	 * CUSTODY, WHICH IS THE OTHER HALF OF WHAT THIS PREDICATE DELIVERS. Validating a
	 * pathname and then handing the same pathname to libbinder to resolve independently is
	 * a time-of-check-to-time-of-use window, and on the pinned stack the consequence of
	 * losing it is not a wrong answer but a process abort. So a caller that intends to
	 * proceed to the lookup passes @p retainedDescriptor and @p retainedIdentity, and on a
	 * positive verdict this predicate does NOT release the validated descriptor: it hands
	 * it over, together with the identity of the node it validated, so that the caller can
	 * re-verify the same name immediately before the use and can hold the node's inode
	 * pinned in the meantime. A caller that passes neither - the harness's own preflight
	 * assertion, and every negative-arm test - gets the original behaviour, with the
	 * descriptor released before return.
	 *
	 * @param [in]  binderDriverPath          - Binder driver node to inspect. Taken as a
	 *                                          parameter, not hard-coded, so that the
	 *                                          negative arms can be exercised - a
	 *                                          nonexistent path, or a path whose reported
	 *                                          protocol differs - without rendering a
	 *                                          test runner's real driver unusable.
	 *                                          Defaults to DEFAULT_BINDER_DRIVER_PATH.
	 * @param [in]  contextManagerTimeoutMs   - Upper bound, in milliseconds, on the
	 *                                          handle-0 resolution check. Zero means do
	 *                                          not wait at all, which is how the timeout
	 *                                          arm is exercised. Defaults to
	 *                                          DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS.
	 * @param [in]  probe                     - The six kernel-facing operations this
	 *                                          predicate performs, injected so that the
	 *                                          arms a path argument cannot reach are
	 *                                          reachable: a node that opens but reports a
	 * @retval true  - Every check passed. The descriptor is retained if and only if
	 *                 @p retainedDescriptor is non-null.
	 * @retval false - The path was empty, or the node is absent or cannot be opened, or
	 *                 cannot be identified, or is not a root-owned character device, or
	 *                 the protocol version could not be read or differs, or handle 0 did
	 *                 not resolve within the bound, or this build carries no binder
	 *                 kernel ABI definitions with which to check any of it. Which one is
	 *                                          positive verdict - none of which any real
	 *                                          path on a driverless host produces.
	 *                                          Defaults to defaultBinderProbe(), which is
	 *                                          the real syscalls, so production behaviour
	 *                                          and every production log line are exactly
	 *                                          what they would be without the seam.
	 * @param [out] retainedDescriptor        - Optional. When non-null AND the verdict is
	 *                                          true, receives the VALIDATED descriptor,
	 *                                          still open, and custody of it passes to the
	 *                                          caller, which must release it through
	 *                                          @p probe `.closeNode`. Set to -1 on every
	 *                                          false verdict, so a caller never has to
	 *                                          distinguish "not retained" from "stale".
	 *                                          Defaults to NULL, which releases the
	 *                                          descriptor here exactly as before.
	 * @param [out] retainedIdentity          - Optional. When non-null AND the verdict is
	 *                                          true, receives the identity of the node
	 *                                          that was validated, for the caller to
	 *                                          compare against a fresh resolution of the
	 *                                          same path immediately before it uses it.
	 *                                          Untouched on a false verdict. Defaults to
	 *                                          NULL.
	 *
	 * @return bool - Whether a binder lookup may safely be attempted
	 *                                          mismatched protocol version, a node that
	 *                                          reports a matching version and then fails
	 *                                          the context-manager check, a node that
	 *                                          cannot be identified or is not a
	 *                                          root-owned character device, and a fully
	 *                 distinguished in the log, not in the return value.
	 *
	 * @pre None whatsoever. This is the first thing that runs, on any platform.
	 * @post Nothing in the process has been left initialized. In particular no
	 *       `ProcessState` singleton is created and no threadpool is started, and the
	 *       descriptor and mapping this check uses are released before it returns, so a
	 *       false result leaves the process exactly as it was. The one exception is the
	 *       custody window a caller opts into by passing @p retainedDescriptor: on a true
	 *       verdict that descriptor is still open when this returns, and closing it is
	 *       then the caller's obligation on every one of its exit paths.
	 * @warning Internal and test-visible, not public API. It is public only so that a
	 *          test translation unit can call it - a private static is unreachable from a
	 *          non-friend, and coupling production code to a test fixture name with a
	 *          `friend` declaration would be worse. `ccec/src/DriverAidlImpl.hpp` is not
	 *          an installed header, so this adds nothing to the middleware public API.
	 *          Nothing outside `ccec/src` and the test suites may call it.
	 * @warning Implementations must not propagate exceptions and must not block beyond
	 *          the stated bound. Both guarantees are what the caller relies on.
	 * @warning The protocol constant this compares against follows `BINDER_IPC_32BIT`, so
	 *          the middleware has to be compiled with the same setting as the libbinder
	 *          it links - an all-32-bit platform speaks protocol 7, and 32-bit middleware
	 *          against a 64-bit vendor speaks 8. Disagreement makes every open fail, and
	 *          this check reports that as "AIDL absent" rather than letting libbinder
	 *          abort on it.
	 *
	 * @see isServiceAvailable()
	 * @see BinderPreflightProbe
	 * @see defaultBinderProbe()
	 * @see expectedBinderProtocolVersion()
	 * @see DEFAULT_BINDER_DRIVER_PATH
	 * @see DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS
	 */
	static bool isBinderPreflightOk(const std::string &binderDriverPath = DEFAULT_BINDER_DRIVER_PATH,
	                                unsigned int contextManagerTimeoutMs = DEFAULT_CONTEXT_MANAGER_TIMEOUT_MS,
	                                const BinderPreflightProbe &probe = defaultBinderProbe(),
	                                int *retainedDescriptor = NULL,
	                                BinderNodeIdentity *retainedIdentity = NULL);

	/**
	 * @brief Names the category of an interface hash observed after a compatibility rejection
	 *
	 * Pure description of one string. It reports what the value is, never what it caused:
	 * isServiceAvailable() cannot know which of halcompat's three rules rejected a server,
	 * because the metadata that predicate read is a local inside a read-only header and a
	 * fresh read is a different binder transaction. The distinction is the whole reason this
	 * function exists rather than a classification chain.
	 *
	 * @param [in] hash - The hash exactly as the server reported it, on a read taken after
	 *                    the decision. May be empty and may contain any byte value.
	 *
	 * @return const char* - A static-storage-duration phrase describing the category. Never
	 *                       NULL, and never a statement about causation.
	 *
	 * @pre None.
	 * @post No state changed.
	 * @warning Diagnostics only. No compatibility decision is taken on this value.
	 * @warning Public so a test translation unit may call it: the production arm that logs it
	 *          is unreachable without a binder transport, so this is the only way the wording
	 *          is covered on a host without one.
	 *
	 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
	 * @see DriverAidlImpl::isServiceAvailable()
	 */
	static const char *describeObservedInterfaceHash(const std::string &hash);

	/**
	 * @brief Whether a post-decision metadata snapshot would itself be accepted
	 *
	 * Answers one question about one snapshot: had these values been the ones halcompat read,
	 * would it have accepted them? A `true` answer does not overturn the rejection - the
	 * decision stands on the metadata that governed - it establishes that the observation
	 * disagrees with the decision, which means the server's metadata changed or recovered and
	 * the rejection must not be attributed to the version rule.
	 *
	 * The version half delegates to `halcompat::detail::isCompatible()`, the real rule, which
	 * is constexpr over two ints and therefore performs no transaction. The hash half mirrors
	 * halcompat's own gate order for the observed value only, including that an unfrozen
	 * server is not accepted, because production calls the predicate with its `allowUnfrozen`
	 * default of false.
	 *
	 * @param [in] hash            - The observed interface hash, unmodified.
	 * @param [in] clientVersion   - This client's compiled-in interface version.
	 * @param [in] observedVersion - The observed server interface version, from the same
	 *                               snapshot as @p hash. Passing values from two different
	 *                               reads is exactly the defect this signature exists to
	 *                               prevent.
	 *
	 * @return bool - True when this snapshot alone would have satisfied the rule.
	 * @retval true  - The observation disagrees with the decision: report changed or recovered
	 *                 metadata, never a version mismatch.
	 * @retval false - The observation is consistent with a rejection, which still does not
	 *                 establish which rule applied.
	 *
	 * @pre @p hash and @p observedVersion come from one snapshot.
	 * @post No state changed.
	 * @warning Diagnostics only. Never used to select a back-end.
	 * @warning Public for the reason describeObservedInterfaceHash() gives.
	 *
	 * @see DriverAidlImpl::describeObservedInterfaceHash()
	 * @see DriverAidlImpl::isServiceAvailable()
	 */
	static bool observedMetadataWouldBeAccepted(const std::string &hash,
	                                            int clientVersion,
	                                            int observedVersion);

	/**
	 * @brief Emits the compatibility-rejection diagnostic for a service halcompat rejected
	 *
	 * The whole of what `isServiceAvailable()` says about a present-but-incompatible
	 * service, in one place, so that the wording a test captures is *byte-for-byte* the
	 * wording production emits. It is a separate function rather than inline in
	 * `isServiceAvailable()` because on a host with no binder driver the preflight
	 * declines first, so the compatibility stage is unreachable there and the diagnostic's
	 * wording would go unverified precisely where it matters most. `isServiceAvailable()`
	 * is its only caller in production, and a second copy of the wording in a test would
	 * be the drift this arrangement exists to prevent.
	 *
	 * It takes one snapshot of the server's interface hash and version and derives every
	 * statement from that single pair. It makes no causal claim about which of halcompat's
	 * three rules rejected the server, because nothing here can know: the metadata the
	 * decision read are locals inside a template in a read-only consumed header, and a
	 * fresh read is a different transaction.
	 *
	 * @param [in] service        - The rejected service. Non-null; the caller has already
	 *                              established that halcompat rejected it.
	 * @param [in] halServiceName - The binder name the service was resolved under, for the
	 *                              log.
	 *
	 * @return void
	 *
	 * @pre `halcompat::isCompatible<IHdmiCec>(service)` has already returned false, and the
	 *      caller has already recorded `REASON_NO_COMPATIBLE_SERVICE`.
	 * @post No member state changes. **This is structural rather than a promise:** the
	 *       function is `static`, so it has no `this` and therefore *cannot* touch
	 *       `availabilityReason`. That is why a failed observation can never relabel an
	 *       established compatibility rejection as `REASON_QUERY_FAILED` -- the outer
	 *       handler that would do so is unreachable from here, because this function
	 *       swallows its own failure.
	 * @warning Diagnostics only. Nothing it computes influences back-end selection.
	 * @warning Public for the reason describeObservedInterfaceHash() gives: on a host with
	 *          no binder driver the preflight declines before `isServiceAvailable()` ever
	 *          reaches the compatibility stage, so this is the only route by which a test
	 *          can exercise the real production wording at all.
	 *
	 * @see DriverAidlImpl::isServiceAvailable()
	 * @see DriverAidlImpl::observedMetadataWouldBeAccepted()
	 */
	static void emitCompatibilityRejectionDiagnostic(
		const ::android::sp< ::com::rdk::hal::hdmicec::IHdmiCec > &service,
		const std::string &halServiceName);
/*
 * Internal, and `protected` rather than `private`, so that the receive-queue handoff below and
 * the lock that serializes the queue's producers are both reachable from a test-local subclass.
 * Neither can be reached any other way.
 *
 * The durable invariant these members carry is single-producer ownership of the incoming queue,
 * plus one reserved slot. While the state is OPENED the listener is the only producer of frames,
 * and it hands them over through offerReceivedFrame(), which refuses one entry below the
 * capacity rather than at it: the last slot belongs to the NULL sentinel close() offers, and the
 * sentinel is what wakes a Bus reader blocked in EventQueue::poll(). EventQueue::offer() discards
 * silently when full, so a receive path allowed to fill the last slot would let that sentinel be
 * dropped and leave the reader asleep with nothing to wake it. close() takes queueProducerMutex
 * around its own sentinel offer because it is a producer too, and the reservation holds only
 * while every producer is serialized against the others.
 *
 * Both halves of that contract need coverage on a host with no binder driver, and both are
 * otherwise out of reach. offerReceivedFrame() accepts a frame only while the state is OPENED,
 * and the state becomes OPENED only through open(), which requires a live compatible AIDL
 * service; `protected` lets a test-local subclass establish that precondition and drive the
 * handoff directly, with no service and no driver, exactly as isBinderPreflightOk() is public so
 * its negative arms can be exercised. queueProducerMutex is reachable for a sharper reason: a
 * serial test cannot observe a lock that is not taken, because a case that fills the queue,
 * offers once more and only then closes passes whether or not close() holds it. What does
 * observe it is a test that holds the lock itself and drives the real close() from another
 * thread - the sentinel offer cannot complete while the lock is held, and completing anyway is
 * the regression. A case that re-states close()'s offer instead of calling close() would pass
 * against production code that had stopped taking the lock, which is the one thing such a case
 * exists to catch.
 *
 * Two alternatives are worse. A `friend` declaration would name a test fixture inside production
 * code, which the note on isBinderPreflightOk() already rejects. A public introspection or
 * state-setting API would add real middleware surface, which the plan forbids. Declaration order
 * here matches DriverImpl's, so the two back-ends diff cleanly against one another.
 *
 * REGISTERED DEVIATION FROM A SPECIFIED ACCESS LEVEL. AAP section 0.3.2.1 specifies the binder
 * preflight predicate as a "private static" member of this class. It is declared `static` as
 * specified and `public` rather than `private`, and the state and queue members that support
 * the receive-path contract above are `protected` rather than `private`. Both are deliberate,
 * both are recorded here rather than left to be discovered from the class body, and the
 * specification owner owns the decision to restate the requirement or to accept this layout.
 *
 * STRICT PRIVACY IS NOT AVAILABLE, AND THE SAME AAP SECTION IS WHY. Section 0.3.2.1 also
 * requires that a test translation unit reach the predicate through the relative-path include
 * this header's own file comment names - `#include "../../../ccec/src/DriverAidlImpl.hpp"` -
 * and pass it a synthesized probe, so that the predicate's negative arms can be exercised on a
 * host with no binder driver. Those two requirements cannot both hold at `private`: a private
 * static is unreachable from a non-friend translation unit, and the nested types the injected
 * probe is built from have to be nameable from outside the class as well. That second
 * constraint is not hypothetical - `tests/L1Tests/ccec/test_DriverAidl.cpp` declares
 * NAMESPACE-SCOPE objects of type `DriverAidlImpl::BinderNodeIdentity` and free functions
 * whose parameter and return types are `DriverAidlImpl::BinderNodeIdentity *` and
 * `DriverAidlImpl::BinderPreflightProbe`. None of those declarations can name the type at all
 * if it is protected or private, because they are not members of any subclass of this class.
 * So `public` on the predicate and on the two nested types is what the specified test route
 * costs, and the requirement to be reachable is the half of section 0.3.2.1 that decides it.
 *
 * WHAT EACH GROUP CARRIES, AND WHY THE GROUPING IS THE NARROWEST ONE THAT WORKS.
 * - `public`: the twelve CCEC::Driver overrides and the constructor and destructor, which are
 *   public in the interface this class implements and cannot be narrowed; the lifecycle enum
 *   and the IncomingQueue typedef, which appear in those signatures and in members below; the
 *   named constants; isServiceAvailable() and unavailabilityReason(), which the factory in
 *   `ccec/src/Driver.cpp` calls from outside the class; and the test-reachable preflight
 *   surface - isBinderPreflightOk(), defaultBinderProbe(), expectedBinderProtocolVersion(),
 *   BinderNodeIdentity and BinderPreflightProbe - plus the three static compatibility
 *   diagnostics, which are static, take everything they use as parameters and hold no state.
 * - `protected`: everything that carries instance state or the receive-path invariant - the
 *   EventListener class, getIncomingQueue(), offerReceivedFrame(), the lifecycle state, the
 *   incoming queue, both locks, the local address list, the two session proxies, the listener
 *   pointer and the recorded availability reason. These are reachable only by deriving, which
 *   is what the test-local subclass does and what nothing in production does.
 * - `private`: the copy constructor and copy assignment operator, declared and never defined.
 * Moving any protected member to private would remove the only coverage the receive-path
 * contract has on a driverless host, and moving any public member to protected would break
 * either the Driver interface, the factory, or the namespace-scope test declarations above.
 *
 * None of this adds to the middleware public API, at any of the three access levels -
 * `ccec/src/DriverAidlImpl.hpp` is not an installed header, it is absent from the
 * `nobase_include_HEADERS` list at `hdmicec/Makefile.am:23` exactly as `DriverImpl.hpp` is,
 * nothing in production derives from this class, and no consumer can reach a protected member.
 * Nothing outside `ccec/src` and the test suites may use any of it.
 */
protected:
	/**
	 * @brief Receives the HAL's `oneway` CEC events on a binder threadpool thread
	 *
	 * Forward declaration only. The definition derives from the generated
	 * `BnHdmiCecEventListener` and lives in `DriverAidlImpl.cpp`, so that the server-side
	 * base header is not pulled into every translation unit that includes this one.
	 * Defining a nested class out of line is well formed, and its non-public access
	 * states plainly that it is not part of any interface.@n
	 * It holds a nullable back pointer to its owner, guarded by a lock of its own, and
	 * reaches the incoming queue through getIncomingQueue(), never through
	 * Driver::getInstance() - resolving through the factory would reintroduce the legacy
	 * static's `static_cast<DriverImpl &>`, which is ill-typed once the factory can
	 * return this class.@n
	 * The back pointer is nullable because the object's lifetime is not this class's to
	 * decide: the listener crossed the binder boundary in `IHdmiCec::open()`, so the HAL
	 * holds a strong reference of its own and releasing ours need not destroy it. Every
	 * path that ends a session therefore detaches the listener - both arms of close(),
	 * the failure arms of open(), and the destructor - and detachment is synchronous
	 * with respect to any callback already in flight. Without it, a HAL that keeps
	 * calling after a failed close would dereference a destroyed owner, which is
	 * CWE-416.
	 *
	 * @see getIncomingQueue()
	 * @see close()
	 */
	class EventListener;

	/**
	 * @brief Returns the incoming frame queue, but only while the driver is open
	 *
	 * The state-guarded accessor that mirrors DriverImpl::getIncomingQueue(). The guard
	 * is the load-bearing part: it is what rejects a receive callback that arrives during
	 * or after a close, and it is what drives the listener's release of the frame it was
	 * about to enqueue. The listener must reach the queue through here and never touch
	 * the member directly, or it would accept frames the legacy path rejects.@n
	 * The legacy signature carries a native handle that its own body never reads. It is
	 * dropped here, because the AIDL back-end has no handle to pass.
	 *
	 * @return IncomingQueue& - The queue received frames are offered onto and that read()
	 *                          drains.
	 *
	 * @throws InvalidStateException - The driver is not OPENED, i.e. it is closed or
	 *                                 closing.
	 *
	 * @pre None. The state check is the method's purpose.
	 * @warning Reads the state without holding the instance lock, reproducing the
	 *          pre-existing unlocked read in DriverImpl::getIncomingQueue(). This is a
	 *          known pre-existing condition, preserved deliberately rather than fixed,
	 *          because behaviour preservation outranks code improvement here.
	 *
	 * @see read()
	 * @see DriverImpl::getIncomingQueue()
	 */
	IncomingQueue & getIncomingQueue(void);

	/**
	 * @brief Hands one received frame to the incoming queue and reports whether it took it
	 *
	 * The ownership-transferring counterpart of getIncomingQueue(), and the reason the
	 * receive path cannot lose a frame. `CCEC_OSAL::EventQueue::offer()` returns void and
	 * silently discards its argument when the queue is at INCOMING_QUEUE_CAPACITY, so a
	 * caller that offers and then forgets the pointer leaks one frame per event for as long
	 * as the queue stays full. This method closes that hole by establishing that there is
	 * room before it offers, and by telling the caller which of the two of them still owns
	 * the frame afterwards.@n
	 * It reaches the queue through getIncomingQueue(), never through the member, so the
	 * load-bearing opened-state guard still applies: a frame arriving during or after a
	 * close is rejected by an InvalidStateException propagating out of here, exactly as on
	 * the legacy path, and the caller's existing catch releases it.
	 *
	 * @param [in] frame - Frame to hand over. Ownership passes to the queue if and only if
	 *                     this returns true; on false, and on any exception, ownership
	 *                     stays with the caller, which must release it.
	 *
	 * @return bool - Whether ownership was transferred
	 * @retval true  - The frame is on the queue. The caller must not delete it and must
	 *                 drop its pointer immediately.
	 * @retval false - There was no slot this method may use, or the queue was observed at
	 *                 capacity after the offer. The frame was not queued and the caller
	 *                 still owns it.
	 *
	 * @throws InvalidStateException - The driver is not OPENED, raised by
	 *                                 getIncomingQueue(). Ownership stays with the caller.
	 *
	 * @pre @p frame is a heap-allocated frame the caller currently owns.
	 * @post Exactly one of: the frame is queued and this returned true; or the frame is
	 *       still the caller's and this returned false or raised.
	 * @warning Serializes producers on queueProducerMutex - a lock of its own, never the
	 *          instance lock. The instance lock is held across an entire synchronous IPC
	 *          round trip by write(), so reusing it here would couple frame delivery on a
	 *          binder thread to transmit latency and could deadlock the receive path behind
	 *          a stalled HAL.
	 * @warning What makes the check-then-offer sound is that every producer takes this
	 *          lock, and nothing weaker. This method and close()'s NULL sentinel offer are
	 *          the only writers on the queue, and both hold queueProducerMutex for their
	 *          offer, so while it is held nothing can raise the occupancy and the observed
	 *          "there is room" cannot turn false. Consumers do not take this lock and keep
	 *          removing, which only lowers occupancy and is therefore harmless to the
	 *          check.
	 * @warning The last slot is reserved for close()'s sentinel: this method refuses at
	 *          INCOMING_QUEUE_CAPACITY - 1, so the wake-the-reader offer can never be the
	 *          one `EventQueue::offer()` swallows and a blocked Bus reader always wakes.
	 * @warning Acceptance is read back from the queue, not inferred from the check that
	 *          preceded the offer, because `EventQueue::offer()` returns void and reports
	 *          nothing. The read-back is trusted in one direction only: an occupancy at or
	 *          above the capacity is inconsistent with acceptance and is reported as a
	 *          refusal, while an occupancy below it is reported as acceptance even if it did
	 *          not rise - a consumer can take the frame the instant the offer wakes it, so a
	 *          non-rise does not prove refusal, and reporting one would hand the caller a
	 *          pointer the queue already owns. The body states the full reasoning.
	 * @warning Not a bounded-time handoff, and nothing here should be read as one. The only
	 *          wait this method removes is the capacity wait: it never blocks waiting for
	 *          the queue to drain, because a full queue is a refusal. Everything it does do
	 *          can block. It takes queueProducerMutex, and both occupancy reads and the
	 *          offer take the queue's own lock behind `CCEC_OSAL::EventQueue`; the offer
	 *          appends to a `std::deque`, which may allocate; and the offer signals the
	 *          queue's condition variable, which locks and broadcasts. So a binder thread
	 *          calling this can be delayed by any of those, and no time bound is claimed
	 *          for it.
	 *
	 * @see getIncomingQueue()
	 * @see INCOMING_QUEUE_CAPACITY
	 */
	bool offerReceivedFrame(CECFrame *frame);

	/**
	 * @brief Lifecycle state: one of CLOSED, CLOSING or OPENED
	 *
	 * ATOMIC, AND THAT IS THE WHOLE OF THE DIFFERENCE FROM DriverImpl::status. Every write
	 * still happens under the instance mutex and every guard still reads the same three
	 * values, so no observable behaviour changes; what changes is that the ONE read taken
	 * without the mutex - getIncomingQueue()'s guard, reached from a binder threadpool
	 * thread while open() or close() may be writing under the lock - is no longer a data
	 * race on a plain `int`, which is undefined behaviour rather than a merely stale read.
	 * The legacy back-end has the same unlocked read on a plain `int` and this migration
	 * does not touch that file; preserving its OBSERVABLE behaviour is required, whereas
	 * preserving undefined behaviour is not, because undefined behaviour is not defined
	 * observable behaviour to begin with.@n
	 * `std::atomic<int>` rather than an atomic of the enum type because the lifecycle enum
	 * above is UNNAMED and cannot be named as a template argument. The default sequentially
	 * consistent ordering of the implicit load and store conversions is used deliberately:
	 * every access here is a single guard read or a single state write, none is on a hot
	 * path - the receive path's cost is dominated by the frame copy and the queue offer -
	 * and a relaxed load would buy nothing measurable while making the ordering between the
	 * state change and the sentinel offer in close() something a reader would have to
	 * reconstruct.@n
	 * NO LOCK WAS ADDED, which is the other half of the choice: taking the instance mutex in
	 * getIncomingQueue() would put the instance lock inside the receive path, where
	 * offerReceivedFrame() already holds queueProducerMutex, and close() takes those two in
	 * the opposite order - instance lock first, producer lock inside it. That is a
	 * lock-inversion deadlock, so the fix that removes the race must not be a lock.
	 *
	 * @see getIncomingQueue()
	 * @see DriverImpl::status - the plain `int` this deliberately does not mirror
	 */
	std::atomic<int> status;
	/**
	 * @brief Legacy native handle field, retained for shape parity with DriverImpl
	 *
	 * Initialized to 0 by the constructor and never used to address the AIDL HAL, which
	 * is reached through the two interface proxies below. It is kept so that the two
	 * back-ends declare the same members in the same order and diff cleanly against one
	 * another.
	 */
	int nativeHandle;
	/**
	 * @brief Frames received from the HAL, awaiting the Bus reader thread
	 *
	 * Constructed with INCOMING_QUEUE_CAPACITY explicitly rather than left on the OSAL
	 * default, so the capacity offerReceivedFrame() checks against is the capacity the
	 * queue enforces. That equality is what lets the receive path prove, from its check
	 * alone, that its offer cannot be the one the queue silently discards.
	 */
	IncomingQueue rQueue;
        /** @brief Guards the state and the local address list. Mutable, so const methods may lock. */
        mutable Mutex mutex;
	/**
	 * @brief Serializes every producer that offers onto the incoming queue
	 *
	 * Held by both writers on that queue, which is the whole of what makes the receive
	 * path's occupancy check meaningful: offerReceivedFrame(), for its check and its
	 * offer, and close(), for its single NULL sentinel offer. A producer that skipped
	 * this lock could raise the occupancy inside the window between that check and that
	 * offer, whereupon `EventQueue::offer()` would discard the received frame silently
	 * while the receive path reported it accepted.@n
	 * A separate lock from the instance mutex, and deliberately so. The instance mutex is
	 * held by write() across an entire synchronous IPC round trip - that is the legacy
	 * critical section, preserved on purpose - so taking it on the receive path would make
	 * frame delivery on a binder thread wait out every transmit, and would put the receive
	 * path behind a stalled HAL. The hold here is short by comparison, but it is not
	 * bounded: it spans the two occupancy reads and the offer between them, each of which
	 * takes the queue's own lock, and the offer may allocate as it appends and then locks
	 * and broadcasts the queue's condition variable. It performs no IPC and no logging.
	 * close() takes it inside the instance lock, and offerReceivedFrame() takes it alone
	 * and never takes the instance lock, so the nesting order is one-way and no inversion
	 * is possible.
	 *
	 * @see offerReceivedFrame()
	 * @see close()
	 */
	Mutex queueProducerMutex;
	/**
	 * @brief Logical addresses this device currently holds
	 *
	 * A list, exactly as in DriverImpl, because the acquire call may be made more than
	 * once. It is not widened for the AIDL back-end: the array shape the AIDL calls
	 * require is confined to the one-element `std::vector<int32_t>` temporaries built
	 * inside the implementation, and no middleware structure or signature grows a plural
	 * form.
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
	 * Retained here so the HAL's strong reference has a local counterpart and the object
	 * cannot be destroyed while the HAL may still call into it. Constructed fresh by each
	 * open() and released by close(), by open()'s own failure arms and by the destructor,
	 * each of which detaches it first so that a HAL still holding its own reference can no
	 * longer reach this instance through it. Releasing this reference alone would not be
	 * enough: it need not destroy the object, and an undetached survivor is a
	 * use-after-free waiting on the next callback.
	 */
	android::sp<EventListener> eventListener;

	/**
	 * @brief Why the last isServiceAvailable() declined, or NULL
	 *
	 * Set by isServiceAvailable() on every one of its exit paths - to NULL when the
	 * AIDL back-end is usable, and otherwise to the phrase naming the stage that
	 * declined - and read back through unavailabilityReason() by the selection helper.
	 * Holding a `const char *` rather than a copied string is deliberate: every value
	 * ever stored here is a string literal with static storage duration, so there is
	 * nothing to own, nothing to allocate on a path that must not fail, and the pointer
	 * stays valid for the lifetime of the process.
	 *
	 * @warning Initialized to NULL by the constructor, which is what makes
	 *          unavailabilityReason() safe to call before any query has run.
	 */
	const char *availabilityReason;

private:
	/**
	 * @brief Copy construction is not allowed
	 *
	 * Declared and never defined, exactly as in DriverImpl. Copying would duplicate the
	 * session proxies, the frame queue and the lock, none of which has copy semantics.
	 *
	 * @param [in] other - Unused. The declaration exists only to suppress the implicit
	 *                     copy constructor.
	 */
	DriverAidlImpl(const DriverAidlImpl &other); /* Not allowed */

	/**
	 * @brief Copy assignment is not allowed
	 *
	 * Declared and never defined, for the same reason as the copy constructor.
	 *
	 * @param [in] other - Unused.
	 *
	 * @return DriverAidlImpl& - Never returns; the declaration is never defined.
	 */
	DriverAidlImpl & operator = (const DriverAidlImpl &other); /* Not allowed */

};

CCEC_END_NAMESPACE

#endif


/** @} */
/** @} */
